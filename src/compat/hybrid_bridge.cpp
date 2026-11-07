// SPDX-License-Identifier: MIT
#include "compat/hybrid_bridge.hpp"
#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_HYBRID_CONTROL
#include "compat/group_send_coordinator.hpp"
#include <algorithm>
#include <limits>

namespace robotweax::srt::compat::hybrid {
namespace {
bool same(const Authorization& a, const Authorization& b) noexcept
{
    return a.stream_id.size <= a.stream_id.bytes.size()
        && b.stream_id.size <= b.stream_id.bytes.size()
        && a.credential_id == b.credential_id
        && a.caller_group == b.caller_group
        && a.listener_group == b.listener_group
        && a.stream_id.view() == b.stream_id.view()
        && a.generation == b.generation && a.expires_at_us == b.expires_at_us;
}
std::uint64_t add(std::uint64_t a, std::uint64_t b) noexcept
{
    return b > UINT64_MAX - a ? UINT64_MAX : a + b;
}
Mode mode(GroupOriginalDelivery d) noexcept
{
    return d == GroupOriginalDelivery::multicast ? Mode::multicast
        : d == GroupOriginalDelivery::unicast    ? Mode::unicast
                                                 : Mode::overlap;
}
}
static_assert(sizeof(CoordinatorBridge) < 32 * 1024);
void AuthorizationLease::revoke() noexcept
{
    std::shared_ptr<NativeService> service;
    {
        std::lock_guard lock(mutex_);
        revoked_ = true;
        service = service_.lock();
    }
    if (service)
        service->close();
}
std::optional<CoordinatorBridge::Identity> CoordinatorBridge::identity(
    const std::shared_ptr<GroupSendCoordinator>& a,
    const std::shared_ptr<ConnectionRuntime>& runtime) noexcept
{
    if (!a || !runtime)
        return {};
    std::lock_guard lock(a->publication_);
    if (a->state_.closed || a->state_.failed
        || a->state_.source_mode != GroupSendCoordinator::SourceMode::multicast)
        return {};
    for (const auto& m : a->members_)
        if (m.state.id && m.runtime.lock() == runtime
            && m.state.detachment == GroupSendCoordinator::Detachment::none)
            return Identity {a->configuration_.source.generation, m.state.id,
                m.join_position};
    return {};
}
CoordinatorBridge::CoordinatorBridge(
    std::shared_ptr<GroupSendCoordinator> actor,
    std::shared_ptr<NativeService> service,
    std::shared_ptr<AuthorizationLease> lease, Configuration c) noexcept
    : actor_(std::move(actor))
    , service_(std::move(service))
    , authorization_(std::move(lease))
    , configuration_(c)
{
}
std::shared_ptr<CoordinatorBridge> CoordinatorBridge::create(
    std::shared_ptr<GroupSendCoordinator> actor,
    std::shared_ptr<NativeService> service,
    std::shared_ptr<AuthorizationLease> lease, Configuration c) noexcept
{
    return make(
        std::move(actor), std::move(service), std::move(lease), c, true);
}
std::shared_ptr<CoordinatorBridge> CoordinatorBridge::prepare(
    std::shared_ptr<GroupSendCoordinator> actor,
    std::shared_ptr<NativeService> service,
    std::shared_ptr<AuthorizationLease> lease, Configuration c) noexcept
{
    return make(
        std::move(actor), std::move(service), std::move(lease), c, false);
}
std::shared_ptr<CoordinatorBridge> CoordinatorBridge::make(
    std::shared_ptr<GroupSendCoordinator> actor,
    std::shared_ptr<NativeService> service,
    std::shared_ptr<AuthorizationLease> lease, Configuration c,
    bool require_enrollment) noexcept
{
    if (!actor || !service || !lease || !valid(c.reporting)
        || (c.returning && !valid(*c.returning)) || !c.command_timeout_us
        || c.command_timeout_us > maximum_age_us || !c.maximum_attempts
        || c.maximum_attempts > 8 || service->configuration_.caller)
        return {};
    // Socket identity inspection is outside all transaction/protocol locks.
    std::shared_ptr<DatagramChannel> channel;
    {
        std::lock_guard lock(actor->publication_);
        channel = actor->configuration_.source.channel;
    }
    if (!channel)
        return {};
    const auto bound = channel->socket.local_endpoint();
    if (!bound || bound.endpoint != service->origin()->source().endpoint)
        return {};
    try {
        auto bridge = std::shared_ptr<CoordinatorBridge>(new CoordinatorBridge(
            std::move(actor), std::move(service), std::move(lease), c));
        bridge->pump();
        const auto state = bridge->snapshot();
        if ((require_enrollment && !state.enrolled)
            || (!require_enrollment && state.closed))
            return {};
        return bridge;
    } catch (...) {
        return {};
    }
}
CoordinatorBridge::~CoordinatorBridge()
{
    if (state_.enrolled)
        close();
}
void CoordinatorBridge::close() noexcept
{
    cancelled_.store(true);
    authorization_->revoke();
}
CoordinatorBridge::Snapshot CoordinatorBridge::snapshot() const noexcept
{
    std::lock_guard lock(mutex_);
    auto s = state_;
    s.closed = s.closed || cancelled_.load() || service_->closed();
    if (!s.closed && state_.enrolled && configuration_.reporting.interval_us
        && !pending_report_)
        s.next_wakeup_us = next_report_;
    if (!s.closed && s.overlap_deadline_us)
        s.next_wakeup_us = std::min(s.next_wakeup_us, s.overlap_deadline_us);
    return s;
}
std::optional<SourceDataProgress>
CoordinatorBridge::data_progress() const noexcept
{
    const auto native = service_->snapshot();
    std::uint64_t revision = 0;
    {
        std::lock_guard lock(mutex_);
        if (!state_.enrolled || cancelled_.load() || native.closed
            || !native.active)
            return {};
        revision = state_.revision;
    }
    auto sample = actor_->data_progress(native.context.selection.member);
    if (!sample || sample->stream != native.context.selection.stream
        || sample->revision != revision)
        return {};
    return sample;
}
std::optional<ReportObservation> CoordinatorBridge::peer_report(
    std::uint32_t maximum_age) const noexcept
{
    const auto native = service_->snapshot();
    std::lock_guard lock(mutex_);
    const auto at = service_->now();
    if (cancelled_.load() || service_->closed() || native.closed
        || !native.active || at >= native.expires_at_us || !state_.enrolled)
        return {};
    return peer_report_.snapshot(at, maximum_age);
}
void CoordinatorBridge::report() noexcept
{
    const auto at = service_->now();
    const auto c = configuration_.reporting;
    if (!c.interval_us || pending_report_ || at < next_report_)
        return;
    next_report_ = add(at, c.interval_us);
    ++state_.report_attempts;
    if (!transact(nullptr) || cancelled_.load()) {
        ++state_.reports_skipped;
        return;
    }
    const auto& context = service_->state_.context;
    const auto data = actor_->data_progress(context.selection.member);
    if (!next_reply_ || next_reply_ == UINT64_MAX) {
        cancelled_.store(true);
        return;
    }
    if (!data || !data->activity || data->revision != state_.revision) {
        ++state_.reports_skipped;
        return;
    }
    const auto sample = stream_report(*data, *data->activity, context,
        data->delivery_mode, next_reply_, c, service_->now());
    if (!sample) {
        ++state_.reports_skipped;
        return;
    }
    const auto result = service_->send_impl(
        sample->message, sample->not_after_us, sample->observed_at_us, true);
    if (result == NativeService::Admission::accepted) {
        pending_report_ = next_reply_++;
        ++state_.reports_queued;
    } else
        ++state_.reports_skipped;
}
void CoordinatorBridge::pump() noexcept
{
    if (!cancelled_.load())
        (void)service_->bridge_work(*this);
}
bool CoordinatorBridge::transact(Entry* entry, bool wake_actor) noexcept
{
    using A = GroupSendCoordinator;
    using R = GroupRuntimeAdmission;
    // Reserve the wake before taking transaction fences. It performs no I/O.
    if (wake_actor && !actor_->manual_
        && actor_->scheduler_->notify_service(actor_->service_)
            != RuntimeScheduler::SubmitStatus::accepted) {
        cancelled_.store(true);
        return true;
    }
    std::unique_lock authority(authorization_->mutex_, std::try_to_lock);
    if (!authority.owns_lock())
        return false;
    std::unique_lock native(service_->queue_mutex_, std::try_to_lock);
    if (!native.owns_lock())
        return false;
    const auto* grant = service_->exchange_.authorization();
    if (cancelled_.load() || service_->closed() || !service_->state_.active
        || !grant || authorization_->revoked_
        || !same(authorization_->grant_, *grant)
        || (!state_.enrolled
            && (authorization_->claimed_ || service_->bridged_
                || service_->application_used_))) {
        cancelled_.store(true);
        return true;
    }
    std::unique_lock publication(actor_->publication_, std::try_to_lock);
    if (!publication.owns_lock())
        return false;
    auto& a = *actor_;
    const auto& context = service_->state_.context;
    const auto& c = a.configuration_.source;
    auto found =
        std::find_if(a.members_.begin(), a.members_.end(), [&](const auto& m) {
            return m.state.id == context.selection.member;
        });
    if (!a.state_.closed && !a.state_.failed)
        a.expire_overlaps_locked();
    if (found != a.members_.end() && state_.enrolled
        && found->expired_overlap_root == service_->state_.generation
        && found->expired_overlap_revision == state_.revision
        && found->state.revision == state_.revision + 1
        && found->state.delivery == GroupOriginalDelivery::unicast) {
        state_.revision = found->state.revision;
        ++state_.overlap_expirations;
    }
    if (a.state_.closed || a.state_.failed
        || a.state_.source_mode != A::SourceMode::multicast
        || found == a.members_.end() || context.selection.stream != c.generation
        || context.dispatch.group_id != c.group_id
        || context.dispatch.multicast_group != c.destination
        || found->state.detachment != A::Detachment::none
        || context.selection.initial_position != found->join_position
        || context.dispatch.initial_sequence != found->join_sequence.value()
        || (state_.enrolled
            && (a.state_.source_revision != source_revision_
                || found->state.revision != state_.revision))) {
        cancelled_.store(true);
        return true;
    }
    std::unique_lock emitter(a.emitter_->mutex_, std::try_to_lock);
    if (!emitter.owns_lock())
        return false;
    if (a.emitter_->state_.closed
        || a.emitter_->state_.failure != GroupMulticastEmitter::Failure::none) {
        cancelled_.store(true);
        return true;
    }
    auto guard = R::try_recover_prefix(found->binding, a.state_.next_sequence);
    const auto prefix = guard.result();
    if (prefix.status == R::Status::busy)
        return false;
    if (prefix.status == R::Status::stale
        || prefix.status == R::Status::closed) {
        cancelled_.store(true);
        return true;
    }
    // An unavailable runtime cannot carry a hybrid policy lease. History-only
    // failures on an enrolled member can still return an explicit denial.
    std::shared_ptr<ConnectionRuntime> runtime = guard.runtime_;
    std::unique_lock<std::mutex> fallback;
    if (!runtime) {
        runtime = found->runtime.lock();
        if (!runtime) {
            cancelled_.store(true);
            return true;
        }
        fallback = std::unique_lock(runtime->mutex_, std::try_to_lock);
        if (!fallback.owns_lock())
            return false;
    }
    if (runtime->locally_closed_ || runtime->broken_ || runtime->peer_closed_
        || runtime->peer_error_pending_ || runtime->hybrid_service_ != service_
        || runtime->hybrid_conclusions_ != service_->origin()
        || !runtime->group_send_state_
        || runtime->group_send_state_->binding != found->binding) {
        cancelled_.store(true);
        return true;
    }
    std::unique_lock budget(a.unicast_budget_->mutex_, std::try_to_lock);
    if (!budget.owns_lock())
        return false;
    const auto at = service_->now();
    if (at >= grant->expires_at_us || cancelled_.load() || service_->closed()) {
        cancelled_.store(true);
        return true;
    }
    if (found->overlap_window) {
        state_.overlap_deadline_us = found->overlap_window->not_after_us;
        state_.overlap_end_position = found->overlap_window->end_position;
    } else {
        state_.overlap_deadline_us = 0;
        state_.overlap_end_position = 0;
    }
    if (!entry) {
        if (state_.enrolled)
            return true;
        if (prefix.status != R::Status::ready
            || a.unicast_budget_->state_.closed || found->state.revision
            || found->state.delivery != GroupOriginalDelivery::multicast)
            return true;
        peer_report_ = ReportTracker(
            Kind::path_report, context.selection.initial_position);
        state_.enrolled = true;
        state_.revision = found->state.revision;
        source_revision_ = a.state_.source_revision;
        authorization_->claimed_ = true;
        authorization_->service_ = service_;
        service_->bridged_ = true;
        return true;
    }
    auto& e = *entry;
    if (at >= e.deadline) {
        ++state_.expired;
        e.complete = true;
        return true;
    }
    const auto span = runtime->session_.send_buffer().sequence_span();
    const auto floor =
        a.next_position_ >= span ? a.next_position_ - span : a.next_position_;
    Outcome outcome = Outcome::ok;
    const bool query = e.request.kind == Kind::delivery_query;
    const bool ready = e.request.kind == Kind::multicast_ready;
    const bool returning = ready && configuration_.returning
        && (context.selection.supported & return_control);
    const auto target = returning
        ? (e.request.mode == Mode::overlap ? GroupOriginalDelivery::overlap
                                           : GroupOriginalDelivery::multicast)
        : GroupOriginalDelivery::unicast;
    if (e.request.kind != Kind::fallback_request && !query && !returning)
        outcome = Outcome::unsupported;
    else if (!query && e.request.revision != found->state.revision)
        outcome = Outcome::stale;
    else if (!query && prefix.status != R::Status::ready)
        outcome = Outcome::history_unavailable;
    else if (!query && !ready
        && (e.request.next_position < floor
            || e.request.next_position > a.next_position_
            || prefix.prefix.originals + prefix.prefix.repairs
                > context.selection.prefix_limit))
        outcome = Outcome::history_unavailable;
    else if (!query
        && (a.unicast_budget_->state_.closed
            || (!found->unicast_lease
                && a.state_.unicast_reservations
                    >= a.state_.unicast_recipient_limit)))
        outcome = Outcome::budget;
    else if (!query && found->state.revision == UINT64_MAX)
        outcome = Outcome::unsupported;
    if (returning && outcome == Outcome::ok) {
        const auto p = *configuration_.returning;
        if (e.request.age_us >= p.maximum_proof_age_us || at < e.first_received
            || at - e.first_received
                >= p.maximum_proof_age_us - e.request.age_us)
            outcome = Outcome::stale;
        else if (target == GroupOriginalDelivery::overlap) {
            if (found->state.delivery != GroupOriginalDelivery::unicast
                || found->state.revision >= UINT64_MAX - 1
                || e.request.next_position != context.selection.initial_position
                || e.request.multicast_next
                    != context.selection.initial_position
                || a.next_position_ > UINT64_MAX - p.maximum_overlap_packets)
                outcome = Outcome::denied;
        } else {
            const auto& w = found->overlap_window;
            if (found->state.delivery != GroupOriginalDelivery::overlap || !w
                || w->root != service_->state_.generation
                || w->revision != found->state.revision
                || e.request.multicast_next != w->boundary
                || e.request.next_position < w->boundary
                || e.request.next_position - w->boundary
                    < p.minimum_coverage_packets
                || e.request.next_position > a.next_position_
                || e.request.interval_us < p.minimum_coverage_us)
                outcome = Outcome::denied;
        }
    }
    if (!next_reply_ || next_reply_ == UINT64_MAX) {
        cancelled_.store(true);
        return true;
    }
    const bool change = found->state.delivery != target;
    if (outcome == Outcome::ok && !query) {
        if (!ready && prefix.prefix.earliest_expiration_microseconds
            && prefix.prefix.earliest_expiration_microseconds
                <= runtime->now_microseconds())
            outcome = Outcome::history_unavailable;
        else {
            const auto commit_at = service_->now();
            if (commit_at >= grant->expires_at_us || service_->closed()
                || cancelled_.load()) {
                cancelled_.store(true);
                return true;
            }
            if (commit_at >= e.deadline) {
                ++state_.expired;
                e.complete = true;
                return true;
            }
            if (returning && target == GroupOriginalDelivery::multicast) {
                a.expire_overlaps_locked();
                if (!found->overlap_window
                    || found->state.revision != e.request.revision) {
                    if (found->expired_overlap_root
                            == service_->state_.generation
                        && found->expired_overlap_revision == state_.revision
                        && found->state.revision == state_.revision + 1) {
                        state_.revision = found->state.revision;
                        ++state_.overlap_expirations;
                    }
                    state_.overlap_deadline_us = 0;
                    state_.overlap_end_position = 0;
                    outcome = Outcome::stale;
                }
            }
            std::optional<A::Membership::OverlapWindow> window;
            if (returning && target == GroupOriginalDelivery::overlap) {
                const auto raw =
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        a.source_now().time_since_epoch())
                        .count();
                const auto p = *configuration_.returning;
                if (raw < 0
                    || static_cast<std::uint64_t>(raw)
                        > UINT64_MAX - p.maximum_overlap_us)
                    outcome = Outcome::unsupported;
                else
                    window = A::Membership::OverlapWindow {
                        service_->state_.generation, found->state.revision + 1,
                        a.next_position_, static_cast<std::uint64_t>(raw),
                        static_cast<std::uint64_t>(raw) + p.maximum_overlap_us,
                        a.next_position_ + p.maximum_overlap_packets};
            }
            if (outcome == Outcome::ok) {
                if (!ready)
                    (void)guard.commit();
                if (change) {
                    a.state_.unicast_members -=
                        static_cast<std::size_t>(found->state.delivery
                            != GroupOriginalDelivery::multicast);
                    a.state_.unicast_members += static_cast<std::size_t>(
                        target != GroupOriginalDelivery::multicast);
                    const bool lease =
                        target != GroupOriginalDelivery::multicast
                        || prefix.pending_unicast;
                    a.state_.unicast_reservations -=
                        static_cast<std::size_t>(found->unicast_lease);
                    a.state_.unicast_reservations +=
                        static_cast<std::size_t>(lease);
                    found->unicast_lease = lease;
                    found->state.delivery = target;
                    ++found->state.revision;
                    found->state.effective_sequence = a.state_.next_sequence;
                    found->state.effective_position = a.next_position_;
                    found->overlap_window = window;
                    found->expired_overlap_root = 0;
                    found->expired_overlap_revision = 0;
                    state_.overlap_deadline_us =
                        window ? window->not_after_us : 0;
                    state_.overlap_end_position =
                        window ? window->end_position : 0;
                    state_.revision = found->state.revision;
                    ++state_.applied;
                }
            }
        }
    }
    const auto& member = found->state;
    e.reply = {.kind = outcome == Outcome::ok && change && !query
            ? Kind::delivery_commit
            : Kind::delivery_status,
        .stream = context.selection.stream,
        .member = context.selection.member,
        .serial = next_reply_++,
        .revision = member.revision,
        .next_position = a.next_position_,
        .next_sequence = a.state_.next_sequence.value(),
        .mode = mode(member.delivery),
        .retained_floor =
            outcome == Outcome::ok && prefix.status == R::Status::ready
            ? floor
            : a.next_position_,
        .multicast_next = (context.selection.supported & return_control)
            ? member.effective_position
            : 0,
        .outcome = outcome,
        .request_serial = (context.selection.supported & policy_query)
            ? e.request.serial
            : 0};
    e.complete = true;
    e.pending = true;
    if (outcome != Outcome::ok)
        ++state_.denied;
    return true;
}
void CoordinatorBridge::event(const NativeService::Event& event) noexcept
{
    if (event.message.kind == Kind::stream_progress
        && event.kind != NativeService::Event::Kind::received) {
        if (event.message.serial == pending_report_) {
            pending_report_ = 0;
            if (event.kind == NativeService::Event::Kind::submitted)
                ++state_.reports_submitted;
        }
        return;
    }
    if (event.kind == NativeService::Event::Kind::received
        && event.message.kind == Kind::path_report
        && event.generation == service_->state_.generation) {
        if (!peer_report_.accept(event.message,
                event.received.status == Session::Status::duplicate,
                event.received.first_received_us, service_->now()))
            ++state_.ignored;
        return;
    }
    if (event.kind != NativeService::Event::Kind::received) {
        if (event.kind == NativeService::Event::Kind::retryable)
            for (auto& e : entries_)
                if (e.occupied && e.complete
                    && e.reply.serial == event.message.serial
                    && e.sends < configuration_.maximum_attempts)
                    e.pending = true;
        return;
    }
    const auto& m = event.message;
    if ((m.kind != Kind::fallback_request && m.kind != Kind::multicast_ready
            && m.kind != Kind::delivery_query)
        || event.generation != service_->state_.generation) {
        ++state_.ignored;
        return;
    }
    if (m.serial <= highest_ && highest_ - m.serial >= entries_.size()) {
        ++state_.ignored;
        return;
    }
    auto& e = entries_[m.serial % entries_.size()];
    if (e.occupied && e.request.serial == m.serial) {
        if (e.request == m) {
            ++state_.duplicates;
            if (e.complete && e.reply.serial
                && e.sends < configuration_.maximum_attempts)
                e.pending = true;
        }
        return;
    }
    highest_ = std::max(highest_, m.serial);
    e = {.request = m,
        .first_received = event.received.first_received_us,
        .deadline = std::min(add(event.received.first_received_us,
                                 configuration_.command_timeout_us),
            service_->state_.expires_at_us),
        .occupied = true};
    if (m.kind == Kind::multicast_ready && configuration_.returning
        && (service_->state_.context.selection.supported & return_control)) {
        const auto maximum = configuration_.returning->maximum_proof_age_us;
        const auto remaining = m.age_us >= maximum ? 0U : maximum - m.age_us;
        e.deadline = std::min(e.deadline, add(e.first_received, remaining));
    }
}
void CoordinatorBridge::replies() noexcept
{
    std::size_t count = 0;
    for (auto& e : entries_) {
        if (!e.occupied || !e.complete || !e.pending || !e.reply.serial)
            continue;
        if (service_->now() >= e.deadline
            || e.sends >= configuration_.maximum_attempts) {
            e.pending = false;
            continue;
        }
        const auto admission =
            service_->send_impl(e.reply, e.deadline, std::nullopt, true);
        if (admission == NativeService::Admission::accepted) {
            e.pending = false;
            ++e.sends;
            ++state_.replies;
        } else if (admission != NativeService::Admission::full)
            cancelled_.store(true);
        if (++count == NativeService::batch)
            break;
    }
}
void CoordinatorBridge::work() noexcept
{
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock())
        return;
    if (cancelled_.load())
        return;
    if (!state_.enrolled) {
        (void)transact(nullptr);
        return;
    }
    // Idle expiry observation is needed only for an owned overlap window.
    // The actor already polls an active window. Observing expiry cannot add
    // DATA work (UC retains the same reservation), so do not wake it again.
    if (state_.overlap_deadline_us)
        (void)transact(nullptr, false);
    if (cancelled_.load()) {
        lock.unlock();
        close();
        return;
    }
    for (std::size_t i = 0; i < NativeService::batch; ++i) {
        NativeService::Event event;
        {
            std::lock_guard queue(service_->queue_mutex_);
            if (service_->closed() || !service_->event_size_)
                break;
            event = service_->events_[service_->event_head_];
            service_->event_head_ =
                (service_->event_head_ + 1) % service_->event_capacity;
            --service_->event_size_;
            service_->state_.events = service_->event_size_;
        }
        this->event(event);
    }
    std::size_t count = 0;
    bool touched = false;
    for (auto& e : entries_) {
        if (!e.occupied || e.complete)
            continue;
        if (count++ == NativeService::batch)
            break;
        if (service_->now() >= e.deadline) {
            e.complete = true;
            ++state_.expired;
            continue;
        }
        if (!transact(&e) && ++e.busy >= configuration_.maximum_attempts) {
            // Refuse rather than apply after unbounded contention. A fresh
            // request can reconcile; no policy or send serial is consumed.
            e.complete = true;
            ++state_.expired;
        }
        touched = true;
        if (cancelled_.load())
            break;
    }
    const bool pending =
        std::any_of(entries_.begin(), entries_.end(), [](const auto& e) {
            return e.occupied && e.complete && e.pending;
        });
    if (!cancelled_.load() && pending && transact(nullptr)
        && !cancelled_.load())
        replies();
    if (!cancelled_.load())
        report();
    if (cancelled_.load())
        state_.closed = true;
    lock.unlock();
    if (cancelled_.load())
        close();
    if (touched) {
        // All native/authority/emitter/runtime/budget guards are gone.
        {
            std::lock_guard publication(actor_->publication_);
            actor_->update_readiness();
        }
        // Runtime wake occurs below without registry or publication ownership.
        std::shared_ptr<ConnectionRuntime> runtime;
        {
            std::lock_guard publication(actor_->publication_);
            for (const auto& member : actor_->members_)
                if (member.state.id
                    == service_->state_.context.selection.member)
                    runtime = member.runtime.lock();
        }
        if (runtime) {
            runtime->notify_channel_send_work();
            runtime->notify_readiness();
        }
    }
}
}
#endif
