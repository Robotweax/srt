// SPDX-License-Identifier: MIT
#include "compat/hybrid/hybrid_setup.hpp"
#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_HYBRID_CONTROL
#include "compat/socket_registry.hpp"
#include "compat/group_registry.hpp"
#include "compat/runtime_scheduler_service.hpp"
#include <algorithm>
namespace robotweax::srt::compat::hybrid {
namespace {
bool valid_options(const StartupOptions& c, bool caller = false) noexcept
{
    const auto required = startup_fallback | policy_query;
    return (!caller
               || (ReceiverBridge::valid_configuration(c.receiver)
                   && MembershipRecovery::valid_configuration(c.membership)
                   && static_cast<bool>(c.join.owner)
                       == static_cast<bool>(c.join.prepare)))
        && valid(c.source.reporting)
        && (!c.source.returning || valid(*c.source.returning))
        && c.source.command_timeout_us
        && c.source.command_timeout_us <= maximum_age_us
        && c.source.maximum_attempts && c.source.maximum_attempts <= 8
        && valid(c.native.policy)
        && c.native.policy.kind == CapabilityKind::offer
        && (c.native.policy.required & required) == required && c.native.owner
        && c.native.resolve && c.native.revalidate && c.native.enroll
        && c.admission_timeout_us && c.admission_timeout_us <= maximum_age_us
        && c.poll_interval_us >= 1000 && c.poll_interval_us <= 50'000
        && c.poll_interval_us < c.admission_timeout_us
        && c.native.retry_interval_us
        && c.native.retry_interval_us <= c.native.timeout_us
        && c.native.timeout_us <= c.admission_timeout_us
        && c.native.maximum_sends_per_stage
        && c.native.maximum_sends_per_stage <= 8;
}
std::uint64_t add(std::uint64_t a, std::uint64_t b) noexcept
{
    return b > UINT64_MAX - a ? UINT64_MAX : a + b;
}
}
static_assert(sizeof(StartupSession) < 2048);
bool provision_startup(SRTSOCKET id, StartupOptions options) noexcept
{
    if (!valid_options(options))
        return false;
    try {
        auto configuration =
            std::make_shared<const StartupOptions>(std::move(options));
        if (auto group = GroupRegistry::instance().find(id)) {
            if (!valid_options(*configuration, true))
                return false;
            std::lock_guard option(group->option_mutex);
            std::lock_guard lock(group->mutex);
            if (group->closed || group->opened || !group->members.empty()
                || group->category != GroupCategory::dispatch
                || group->dispatch->sender
                || !group->member_public_options.multicast_independent
                || group->hybrid_startup_options)
                return false;
            group->hybrid_startup_options = std::move(configuration);
            return true;
        }
        auto socket = SocketRegistry::instance().find(id);
        if (!socket)
            return false;
        std::lock_guard option(socket->option_mutex);
        std::lock_guard lock(socket->mutex);
        if ((socket->state != SRTS_INIT && socket->state != SRTS_OPENED)
            || socket->runtime || !socket->public_options.multicast_independent
            || !socket->public_options.multicast_coordinated
            || socket->hybrid_startup_options)
            return false;
        socket->hybrid_startup_options = std::move(configuration);
        return true;
    } catch (...) {
        return false;
    }
}
StartupSession::StartupSession(std::shared_ptr<ConnectionRuntime> r,
    std::shared_ptr<const StartupOptions> o,
    std::shared_ptr<GroupSendCoordinator> a, std::shared_ptr<NativeService> n,
    bool caller)
    : runtime_(std::move(r))
    , options_(std::move(o))
    , actor_(std::move(a))
    , native_(std::move(n))
    , work_(std::make_shared<Work>())
{
    state_.caller = caller;
}
std::shared_ptr<StartupSession> StartupSession::start(
    std::shared_ptr<ConnectionRuntime> r,
    std::shared_ptr<const StartupOptions> options,
    std::shared_ptr<GroupSendCoordinator> actor,
    std::uint64_t deadline) noexcept
{
    if (!r || !options || !valid_options(*options, !actor)
        || NativeService::monotonic_now() >= deadline)
        return {};
    auto native_options = options->native;
    const bool caller = !actor;
    if (actor) {
        const auto identity = CoordinatorBridge::identity(actor, r);
        if (!identity)
            return {};
        if ((native_options.stream && native_options.stream != identity->stream)
            || (native_options.member
                && native_options.member != identity->member)
            || (native_options.initial_position
                && native_options.initial_position
                    != identity->initial_position))
            return {};
        native_options.stream = identity->stream;
        native_options.member = identity->member;
        native_options.initial_position = identity->initial_position;
    }
    const auto now = NativeService::monotonic_now();
    deadline = std::min(deadline, add(now, options->admission_timeout_us));
    auto native = attach_native_control(r, std::move(native_options));
    if (!native)
        return {};
    try {
        auto self = std::shared_ptr<StartupSession>(
            new StartupSession(r, options, actor, native, caller));
        self->state_.not_after_us = deadline;
        if (caller) {
            std::lock_guard lock(r->mutex_);
            self->control_ = r->channel_.lock();
        }
        self->scheduler_ = acquire_runtime_scheduler();
        if (!self->scheduler_)
            return {};
        self->work_->owner = self;
        const auto reserved = self->scheduler_->reserve_service(
            native->configuration_.affinity, {run, self->work_});
        if (reserved.status != RuntimeScheduler::SubmitStatus::accepted)
            return {};
        self->service_ = reserved.token;
        if (self->scheduler_->notify_service(self->service_)
            != RuntimeScheduler::SubmitStatus::accepted)
            return {};
        return self;
    } catch (...) {
        native->close();
        return {};
    }
}
StartupSession::~StartupSession()
{
    close();
}
void StartupSession::close() noexcept
{
    cancelled_.store(true);
    std::shared_ptr<ReceiverBridge> receiver;
    std::shared_ptr<MembershipRecovery> membership;
    std::shared_ptr<CoordinatorBridge> source;
    {
        std::lock_guard lock(mutex_);
        state_.closed = true;
        state_.phase = Phase::closed;
        receiver = receiver_;
        membership = membership_;
        source = source_;
    }
    native_->close();
    if (control_)
        control_->unregister_connection(
            native_->origin()->dispatch().group_id, runtime_.get());
    if (receiver)
        receiver->close();
    if (membership)
        membership->close();
    if (source)
        source->close();
    if (scheduler_)
        (void)scheduler_->release_service(service_);
}
StartupSession::Snapshot StartupSession::snapshot() const noexcept
{
    std::lock_guard lock(mutex_);
    auto s = state_;
    if (cancelled_.load() || native_->closed()) {
        s.closed = true;
        if (s.phase != Phase::failed)
            s.phase = Phase::closed;
    }
    if (s.caller && !s.published
        && NativeService::monotonic_now() >= s.not_after_us)
        s.phase = Phase::failed;
    return s;
}
std::shared_ptr<ReceiverBridge> StartupSession::receiver() const noexcept
{
    std::lock_guard lock(mutex_);
    return state_.published ? receiver_ : nullptr;
}
std::shared_ptr<MembershipRecovery> StartupSession::membership() const noexcept
{
    std::lock_guard lock(mutex_);
    return state_.published ? membership_ : nullptr;
}
bool StartupSession::publish_ready(SocketRecord& socket) noexcept
{
    std::lock_guard lock(mutex_);
    if (cancelled_.load() || !state_.caller || state_.phase != Phase::ready
        || !receiver_)
        return false;
    const auto native = native_->snapshot();
    const auto receiver = receiver_->snapshot();
    std::lock_guard record(socket.mutex);
    if (socket.state != SRTS_CONNECTING || socket.runtime != runtime_
        || socket.hybrid_startup_session.get() != this || native.closed
        || !native.active || !receiver.fresh || receiver.mode != Mode::unicast
        || receiver.fallback != ReceiverBridge::RequestState::confirmed
        || receiver.returning == ReceiverBridge::RequestState::pending
        || cancelled_.load()
        || NativeService::monotonic_now()
            >= std::min(state_.not_after_us, native.expires_at_us))
        return false;
    socket.state = SRTS_CONNECTED;
    socket.connect_error = SRT_SUCCESS;
    socket.connect_system_error = 0;
    socket.rejection_reason = SRT_REJ_UNKNOWN;
    state_.published = true;
    state_.phase = Phase::connected;
    return true;
}
void StartupSession::run(void* p) noexcept
{
    if (auto self = static_cast<Work*>(p)->owner.lock())
        self->drive();
}
void StartupSession::drive() noexcept
{
    if (driving_.test_and_set())
        return;
    struct Release {
        std::atomic_flag& f;
        ~Release()
        {
            f.clear();
        }
    } release {driving_};
    if (cancelled_.load())
        return;
    const auto native = native_->snapshot();
    auto state = snapshot();
    auto fail = [&](Outcome o) {
        {
            std::lock_guard lock(mutex_);
            if (!cancelled_.load()) {
                state_.phase = Phase::failed;
                state_.outcome = o;
            }
        }
        if (scheduler_)
            (void)scheduler_->release_service(service_);
    };
    if (native.closed || state.phase == Phase::failed
        || ((state.caller ? !state.published : state.phase != Phase::serving)
            && NativeService::monotonic_now() >= state.not_after_us)) {
        fail(Outcome::stale);
        native_->close();
        return;
    }
    std::shared_ptr<ReceiverBridge> receiver;
    std::shared_ptr<MembershipRecovery> membership;
    std::shared_ptr<CoordinatorBridge> source;
    {
        std::lock_guard lock(mutex_);
        receiver = receiver_;
        membership = membership_;
        source = source_;
    }
    if (native.active) {
        if (state.caller) {
            if (!receiver) {
                receiver = ReceiverBridge::create(native_, options_->receiver);
                if (!receiver) {
                    fail(Outcome::denied);
                    native_->close();
                    return;
                }
                membership = MembershipRecovery::create(
                    native_, options_->membership, options_->join, receiver);
                if (!membership) {
                    fail(Outcome::denied);
                    native_->close();
                    return;
                }
                {
                    std::lock_guard lock(mutex_);
                    if (cancelled_.load()) {
                        receiver->close();
                        membership->close();
                        return;
                    }
                    receiver_ = receiver;
                    membership_ = membership;
                    state_.phase = Phase::admitting;
                }
            }
            receiver->pump();
            if (!requested_) {
                const auto admitted = receiver->request_fallback(
                    native.context.selection.initial_position);
                if (admitted != ReceiverBridge::Admission::accepted) {
                    fail(Outcome::denied);
                    native_->close();
                    return;
                }
                requested_ = true;
            }
            const auto result = receiver->snapshot();
            if (!state.published) {
                if (result.fallback == ReceiverBridge::RequestState::confirmed
                    && result.fresh && result.mode == Mode::unicast) {
                    std::lock_guard lock(mutex_);
                    if (!cancelled_.load())
                        state_.phase = Phase::ready;
                } else if (result.closed
                    || result.fallback == ReceiverBridge::RequestState::denied
                    || result.fallback
                        == ReceiverBridge::RequestState::unknown) {
                    fail(result.fallback_outcome);
                    native_->close();
                    return;
                }
            } else
                membership->pump();
        } else {
            if (!source) {
                std::optional<Authorization> grant;
                {
                    std::unique_lock protocol(
                        native_->protocol_mutex_, std::try_to_lock);
                    if (protocol.owns_lock())
                        if (const auto* g = native_->exchange_.authorization())
                            grant = *g;
                }
                if (grant) {
                    std::shared_ptr<AuthorizationLease> lease;
                    try {
                        lease = std::make_shared<AuthorizationLease>(*grant);
                    } catch (...) {
                        fail(Outcome::budget);
                        native_->close();
                        return;
                    }
                    source = CoordinatorBridge::prepare(
                        actor_, native_, lease, options_->source);
                    if (source) {
                        std::lock_guard lock(mutex_);
                        if (cancelled_.load()) {
                            source->close();
                            return;
                        }
                        lease_ = std::move(lease);
                        source_ = source;
                        state_.phase = Phase::admitting;
                    } else {
                        fail(Outcome::denied);
                        native_->close();
                        return;
                    }
                }
            }
            if (source) {
                source->pump();
                const auto sender = source->snapshot();
                if (sender.closed) {
                    fail(Outcome::denied);
                    native_->close();
                    return;
                }
                if (sender.enrolled) {
                    std::lock_guard lock(mutex_);
                    if (!cancelled_.load())
                        state_.phase = Phase::serving;
                }
            }
        }
    }
    if (cancelled_.load())
        return;
    const auto next = std::chrono::steady_clock::now()
        + std::chrono::microseconds {options_->poll_interval_us};
    if (scheduler_->schedule_service_at(service_, next)
        != RuntimeScheduler::SubmitStatus::accepted) {
        fail(Outcome::denied);
        native_->close();
    }
}
}
#endif
