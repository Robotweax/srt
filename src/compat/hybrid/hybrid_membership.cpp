// SPDX-License-Identifier: MIT
#include "compat/hybrid/hybrid_membership.hpp"
#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_HYBRID_CONTROL
#include "compat/transport_runtime.hpp"
#include "compat/hybrid/hybrid_receiver.hpp"
#include <algorithm>
namespace robotweax::srt::compat::hybrid {
namespace {
std::uint64_t add(std::uint64_t a, std::uint64_t b) noexcept
{
    return b > UINT64_MAX - a ? UINT64_MAX : a + b;
}
}
static_assert(sizeof(MembershipRecovery) < 2048);
bool MembershipRecovery::valid_configuration(Configuration c) noexcept
{
    return c.maximum_recoveries <= 8
        && !(!c.maximum_attempts || c.maximum_attempts > 8 || !c.timeout_us
            || c.timeout_us > maximum_age_us || !c.initial_backoff_us
            || c.initial_backoff_us > c.maximum_backoff_us
            || c.maximum_backoff_us > maximum_interval_us
            || c.initial_backoff_us >= c.timeout_us || c.interface.port
            || c.interface.is_multicast());
}
std::shared_ptr<MembershipRecovery> MembershipRecovery::create(
    std::shared_ptr<NativeService> s, Configuration c) noexcept
{
    return create(std::move(s), c, Provider {});
}
MembershipRecovery::MembershipRecovery(std::shared_ptr<NativeService> s,
    Configuration c, Provider p, std::shared_ptr<ReceiverBridge> receiver)
    : service_(std::move(s))
    , runtime_(service_->runtime_.lock())
    , receiver_(std::move(receiver))
    , identity_(std::make_shared<std::byte>())
    , configuration_(c)
    , provider_(std::move(p))
{
}
std::shared_ptr<MembershipRecovery> MembershipRecovery::create(
    std::shared_ptr<NativeService> s, Configuration c, Provider p,
    std::shared_ptr<ReceiverBridge> receiver) noexcept
{
    if (!s || !valid_configuration(c)
        || (c.maximum_recoveries
            && (!receiver || receiver->service_ != s
                || receiver->snapshot().closed))
        || static_cast<bool>(p.owner) != static_cast<bool>(p.prepare))
        return {};
    const auto native = s->snapshot();
    if (native.closed || !native.active
        || !(native.context.selection.supported & startup_fallback)
        || c.interface.family != native.context.dispatch.multicast_group.family)
        return {};
    try {
        auto result =
            std::shared_ptr<MembershipRecovery>(new MembershipRecovery(
                std::move(s), c, std::move(p), std::move(receiver)));
        if (!result->runtime_)
            return {};
        const auto now = result->service_->now();
        if (now >= native.expires_at_us)
            return {};
        auto& r = *result->runtime_;
        std::shared_ptr<DatagramChannel> control;
        {
            std::lock_guard lock(r.mutex_);
            control = r.channel_.lock();
        }
        if (!control
            || !control->has_ingress_route(native.context.dispatch.group_id, &r,
                PacketIngress::unicast_session))
            return {};
        std::lock_guard lock(r.mutex_);
        if (r.locally_closed_ || r.broken_ || r.peer_closed_
            || r.peer_error_pending_ || !r.hybrid_caller_
            || r.hybrid_service_ != result->service_
            || r.hybrid_conclusions_ != result->service_->origin()
            || !r.hybrid_receive_ || r.multicast_ingress_binding_
            || r.hybrid_membership_claimed_ || r.received_data_)
            return {};
        r.hybrid_receive_->sync_floor(
            r.session_.receive_buffer().first_stored_sequence());
        const auto data = r.hybrid_receive_->snapshot(
            native.context.selection.initial_position);
        const auto checked = result->service_->now();
        if (!data || data->all.observed
            || data->window_floor != native.context.selection.initial_position
            || checked < now
            || checked >= std::min(add(now, c.timeout_us), native.expires_at_us)
            || result->service_->closed())
            return {};
        r.hybrid_membership_ = result->identity_;
        r.hybrid_membership_claimed_ = r.hybrid_membership_live_ = true;
        r.hybrid_membership_recoverable_ = c.maximum_recoveries != 0;
        r.hybrid_receive_route_ = r.multicast_ingress_generation_;
        result->request_ = {native.context, c.interface, native.generation, 0,
            now, std::min(add(now, c.timeout_us), native.expires_at_us)};
        result->state_.not_after_us = result->request_.not_after_us;
        result->next_attempt_ = result->last_now_ = now;
        result->backoff_ = c.initial_backoff_us;
        return result;
    } catch (...) {
        return {};
    }
}
bool MembershipRecovery::live() const noexcept
{
    const auto native = service_->snapshot();
    if (native.closed || !native.active
        || service_->now() >= native.expires_at_us
        || native.generation != request_.generation
        || native.context != request_.context)
        return false;
    std::lock_guard lock(runtime_->mutex_);
    return !runtime_->locally_closed_ && !runtime_->broken_
        && !runtime_->peer_closed_ && !runtime_->peer_error_pending_
        && runtime_->hybrid_service_ == service_
        && runtime_->hybrid_conclusions_ == service_->origin()
        && runtime_->hybrid_membership_ == identity_
        && runtime_->hybrid_membership_live_
        && runtime_->hybrid_observation_current();
}
bool MembershipRecovery::report_loss() noexcept
{
    std::lock_guard lock(mutex_);
    if (state_.closed || !state_.joined || !channel_
        || !configuration_.maximum_recoveries || !live())
        return false;
    std::shared_ptr<const MulticastIngressBinding> binding;
    {
        std::lock_guard runtime(runtime_->mutex_);
        binding = runtime_->multicast_ingress_binding_;
    }
    if (!runtime_->lose_multicast_ingress(binding))
        return false;
    channel_->unregister_connection(
        request_.context.dispatch.group_id, runtime_.get());
    return true;
}
MembershipRecovery::~MembershipRecovery()
{
    close();
}
void MembershipRecovery::close() noexcept
{
    std::shared_ptr<DatagramChannel> channel;
    {
        std::lock_guard lock(mutex_);
        state_.closed = true;
        state_.phase = Phase::closed;
        state_.joined = false;
        channel = std::move(channel_);
        if (channel)
            channel->unregister_connection(
                request_.context.dispatch.group_id, runtime_.get());
        if (runtime_) {
            std::lock_guard runtime(runtime_->mutex_);
            if (runtime_->hybrid_membership_ == identity_) {
                runtime_->hybrid_membership_live_ = false;
                runtime_->hybrid_membership_recoverable_ = false;
            }
        }
    }
    // Worker/channel retirement happens without owner/runtime/protocol locks.
}
MembershipRecovery::Snapshot MembershipRecovery::snapshot() const noexcept
{
    std::lock_guard lock(mutex_);
    auto s = state_;
    const auto now = service_->now();
    if (now < last_now_)
        clock_failed_ = true;
    last_now_ = std::max(last_now_, now);
    if (!s.closed && (clock_failed_ || !live())) {
        s.closed = true;
        s.phase = Phase::closed;
        s.joined = false;
    }
    const auto expiry = service_->snapshot().expires_at_us;
    if (!s.closed) {
        std::lock_guard runtime(runtime_->mutex_);
        s.route_epoch = runtime_->multicast_ingress_generation_;
        if (runtime_->hybrid_membership_loss_) {
            s.joined = false;
            s.phase = Phase::awaiting_unicast;
            s.not_after_us = std::min(add(runtime_->hybrid_membership_loss_at_,
                                          configuration_.timeout_us),
                expiry);
        }
    }
    if (!s.closed && !s.joined && now >= s.not_after_us)
        s.phase = Phase::expired;
    s.budget_exhausted = s.cycle_attempts >= configuration_.maximum_attempts
        || (!s.joined && s.losses > s.recoveries
            && s.recoveries >= configuration_.maximum_recoveries);
    s.next_wakeup_us =
        s.closed ? UINT64_MAX : service_->snapshot().expires_at_us;
    if (!s.closed && s.phase == Phase::awaiting_unicast)
        s.next_wakeup_us = std::min({s.next_wakeup_us, now, s.not_after_us});
    if (!s.closed && s.phase == Phase::waiting)
        s.next_wakeup_us =
            std::min({s.next_wakeup_us, next_attempt_, s.not_after_us});
    if (!s.closed && s.phase == Phase::preparing)
        s.next_wakeup_us = std::min(s.next_wakeup_us, s.not_after_us);
    return s;
}
std::optional<UdpSocket> MembershipRecovery::prepare(const Request& r) noexcept
{
    const auto group = r.context.dispatch.multicast_group;
    UdpSocket socket(group.family);
    const auto local = group.is_ipv4() ? IpEndpoint::any(group.port)
                                       : IpEndpoint::ipv6_any(group.port);
    if (!socket.valid() || socket.set_reuse_address(true) != Error::none
        || socket.bind(local) != Error::none
        || socket.join_multicast(group, r.interface) != Error::none)
        return {};
    return socket;
}
void MembershipRecovery::pump() noexcept
{
    const auto lifetime = weak_from_this().lock();
    if (!lifetime || pumping_.test_and_set())
        return;
    struct Release {
        std::atomic_flag& flag;
        ~Release()
        {
            flag.clear();
        }
    } release {pumping_};
    if (snapshot().closed) {
        close();
        return;
    }
    std::shared_ptr<DatagramChannel> retired;
    const auto expiry = service_->snapshot().expires_at_us;
    {
        std::lock_guard lock(mutex_);
        std::lock_guard runtime(runtime_->mutex_);
        if (!state_.closed && runtime_->hybrid_membership_ == identity_
            && runtime_->hybrid_membership_loss_) {
            const auto loss_at = runtime_->hybrid_membership_loss_at_;
            runtime_->hybrid_membership_loss_ = false;
            ++state_.losses;
            state_.joined = false;
            retired = std::move(channel_);
            if (state_.recoveries >= configuration_.maximum_recoveries) {
                state_.phase = Phase::exhausted;
            } else {
                ++state_.recoveries;
                state_.cycle_attempts = 0;
                request_.recovery = state_.recoveries;
                request_.started_at_us = loss_at;
                request_.not_after_us =
                    std::min(add(loss_at, configuration_.timeout_us), expiry);
                state_.not_after_us = request_.not_after_us;
                next_attempt_ = loss_at;
                backoff_ = configuration_.initial_backoff_us;
                state_.phase = Phase::awaiting_unicast;
            }
        }
    }
    if (retired)
        retired->stop();
    retired.reset();
    // Sender UC approval is separate from membership loss. Lookup outside locks.
    const bool uc_ready = receiver_ && [&] {
        const auto s = receiver_->snapshot();
        return !s.closed && s.fresh && s.mode == Mode::unicast
            && s.returning != ReceiverBridge::RequestState::pending;
    }();
    Request request;
    {
        std::lock_guard lock(mutex_);
        const auto now = service_->now();
        if (now < last_now_)
            clock_failed_ = true;
        last_now_ = std::max(last_now_, now);
        if (state_.closed || state_.joined || state_.phase == Phase::exhausted
            || state_.phase == Phase::expired)
            return;
        if (clock_failed_ || !live()) {
            state_.closed = true;
            state_.phase = Phase::closed;
            return;
        }
        if (!armed_) {
            // The owner pumps only after the receiver is published, and the
            // fallback confirmation that gates publication can take longer
            // than this timeout. Measure the join budget from here, still
            // bounded by the grant's expiry, instead of from creation.
            armed_ = true;
            const auto expires = service_->snapshot().expires_at_us;
            request_.started_at_us = now;
            request_.not_after_us =
                std::min(add(now, configuration_.timeout_us), expires);
            state_.not_after_us = request_.not_after_us;
            next_attempt_ = now;
        }
        if (now >= state_.not_after_us) {
            state_.phase = Phase::expired;
            return;
        }
        if (state_.recoveries && !uc_ready)
            return;
        if (now < next_attempt_)
            return;
        ++state_.attempts;
        ++state_.cycle_attempts;
        state_.phase = Phase::preparing;
        request = request_;
        request.attempt = state_.cycle_attempts;
    }
    auto socket = provider_.prepare
        ? provider_.prepare(provider_.owner.get(), request)
        : prepare(request);
    // Native join preparation and any application callback ran without locks.
    std::shared_ptr<DatagramChannel> channel;
    if (socket && socket->valid()) {
        const auto bound = socket->local_endpoint();
        if (bound
            && bound.endpoint.family
                == request.context.dispatch.multicast_group.family
            && bound.endpoint.port
                == request.context.dispatch.multicast_group.port) {
            try {
                channel = std::make_shared<DatagramChannel>(std::move(*socket));
            } catch (...) {
            }
        }
    }
    std::unique_lock<std::mutex> confirmation;
    if (request.recovery && receiver_)
        confirmation = std::unique_lock(receiver_->mutex_);
    const auto checked_at = service_->now();
    const bool uc_still_ready = !request.recovery
        || (receiver_ && !receiver_->cancelled_.load() && receiver_->confirmed_
            && receiver_->state_.mode == Mode::unicast
            && checked_at >= receiver_->state_.confirmation_origin_us
            && checked_at - receiver_->state_.confirmation_origin_us
                < receiver_->configuration_.confirmation_lifetime_us
            && receiver_->state_.returning
                != ReceiverBridge::RequestState::pending);
    std::lock_guard lock(mutex_);
    const auto now = service_->now();
    if (now < last_now_)
        clock_failed_ = true;
    last_now_ = std::max(last_now_, now);
    if (state_.closed)
        return;
    if (clock_failed_ || !live()) {
        state_.closed = true;
        state_.phase = Phase::closed;
        return;
    }
    if (now >= state_.not_after_us) {
        state_.phase = Phase::expired;
        return;
    }
    if (!uc_still_ready) {
        ++state_.failures;
        state_.phase = state_.cycle_attempts >= configuration_.maximum_attempts
            ? Phase::exhausted
            : Phase::awaiting_unicast;
        return;
    }
    if (channel) {
        const auto source = service_->origin()->source();
        const bool link_local = source.endpoint.is_ipv6()
            && source.endpoint.address[0] == 0xfeU
            && (source.endpoint.address[1] & 0xc0U) == 0x80U;
        const MulticastIngressConfiguration ingress {.source = source,
            .interface_scope_id = link_local ? request.interface.scope_id : 0};
        if (channel->register_ingress_route(request.context.dispatch.group_id,
                runtime_, PacketIngress::multicast_data, ingress, identity_)) {
            if (channel->start() && service_->now() < state_.not_after_us
                && live()) {
                channel_ = std::move(channel);
                state_.joined = true;
                state_.phase = Phase::joined;
                return;
            }
            channel->unregister_connection(
                request.context.dispatch.group_id, runtime_.get());
            // Publication may have admitted native DATA. Never rearm a route
            // after a failed/expired publication or fabricate a fresh ledger.
            state_.closed = true;
            state_.phase = Phase::closed;
            return;
        }
        state_.closed = true;
        state_.phase = Phase::closed;
        return;
    }
    ++state_.failures;
    if (state_.cycle_attempts >= configuration_.maximum_attempts) {
        state_.phase = Phase::exhausted;
        return;
    }
    next_attempt_ = std::min(add(now, backoff_), state_.not_after_us);
    backoff_ = std::min(backoff_ * 2,
        static_cast<std::uint64_t>(configuration_.maximum_backoff_us));
    state_.phase = Phase::waiting;
}
}
#endif
