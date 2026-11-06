// SPDX-License-Identifier: MIT
#include "compat/hybrid_automatic_return.hpp"
#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_HYBRID_CONTROL
#include "compat/transport_runtime.hpp"
#include <algorithm>
namespace robotweax::srt::compat::hybrid {
namespace {
std::uint64_t add(std::uint64_t a, std::uint64_t b) noexcept
{
    return b > UINT64_MAX - a ? UINT64_MAX : a + b;
}
bool pending(AutomaticReturn::Phase p) noexcept
{
    return p == AutomaticReturn::Phase::overlap_pending
        || p == AutomaticReturn::Phase::collecting
        || p == AutomaticReturn::Phase::multicast_pending;
}
}
static_assert(sizeof(AutomaticReturn) < 2048);
AutomaticReturn::AutomaticReturn(
    std::shared_ptr<ReceiverBridge> r, Configuration c) noexcept
    : receiver_(std::move(r))
    , configuration_(c)
{
}
std::shared_ptr<AutomaticReturn> AutomaticReturn::create(
    std::shared_ptr<ReceiverBridge> receiver, Configuration c) noexcept
{
    if (!receiver || !c.maximum_operations || c.maximum_operations > 8
        || !c.operation_timeout_us || c.operation_timeout_us > maximum_age_us
        || c.coverage_packets < 2 || c.coverage_packets > 32
        || !c.minimum_coverage_us || c.minimum_coverage_us > maximum_interval_us
        || c.minimum_coverage_us >= c.operation_timeout_us
        || !c.proof_lifetime_us || c.proof_lifetime_us > c.operation_timeout_us)
        return {};
    try {
        auto controller = std::shared_ptr<AutomaticReturn>(
            new AutomaticReturn(std::move(receiver), c));
        if (!controller->receiver_->claim_automatic_return())
            return {};
        return controller;
    } catch (...) {
        return {};
    }
}
AutomaticReturn::~AutomaticReturn()
{
    close();
}
void AutomaticReturn::close() noexcept
{
    std::lock_guard lock(mutex_);
    state_ = project();
    if (pending(state_.phase) && state_.phase != Phase::multicast_pending)
        state_.result = ReceiverBridge::RequestState::unknown;
    state_.closed = true;
    state_.phase = Phase::closed;
}
AutomaticReturn::Snapshot AutomaticReturn::project() const noexcept
{
    auto s = state_;
    s.budget_exhausted = s.operations >= configuration_.maximum_operations;
    if (!s.ticket) {
        if (receiver_->snapshot().closed) {
            s.closed = true;
            s.phase = Phase::closed;
        }
        return s;
    }
    const auto receipt =
        receiver_->automatic_return_receipt(request_, s.not_after_us);
    if (receipt.ticket == s.ticket) {
        if (receipt.target == Mode::overlap)
            s.overlap_result = receipt.result;
        else
            s.multicast_result = receipt.result;
        s.outcome = receipt.outcome;
        if (pending(s.phase)
            || (s.closed && receipt.target == Mode::multicast)) {
            if (receipt.result == ReceiverBridge::RequestState::confirmed) {
                if (receipt.target == Mode::multicast) {
                    s.result = ReceiverBridge::RequestState::confirmed;
                    s.commit_position = receipt.boundary;
                    if (!s.closed)
                        s.phase = Phase::confirmed;
                } else if (s.phase == Phase::overlap_pending) {
                    s.phase = Phase::collecting;
                    s.boundary = receipt.boundary;
                }
            } else if (receipt.result == ReceiverBridge::RequestState::denied) {
                s.result = ReceiverBridge::RequestState::denied;
                if (!s.closed)
                    s.phase = Phase::denied;
            } else if (receipt.result
                == ReceiverBridge::RequestState::unknown) {
                s.result = ReceiverBridge::RequestState::unknown;
                if (!s.closed)
                    s.phase = Phase::unknown;
            }
        }
    }
    if (pending(s.phase) && !receipt.current) {
        s.result = ReceiverBridge::RequestState::unknown;
        s.phase = Phase::unknown;
    }
    if (receipt.closed)
        s.closed = true;
    if (s.closed)
        s.phase = Phase::closed;
    return s;
}
AutomaticReturn::Snapshot AutomaticReturn::snapshot() const noexcept
{
    std::lock_guard lock(mutex_);
    return project();
}
void AutomaticReturn::pump() noexcept
{
    const auto lifetime = weak_from_this().lock();
    if (!lifetime || pumping_.test_and_set())
        return;
    struct Release {
        std::atomic_flag& f;
        ~Release()
        {
            f.clear();
        }
    } release {pumping_};
    {
        std::lock_guard lock(mutex_);
        if (state_.closed)
            return;
    }
    receiver_->pump();
    std::lock_guard lock(mutex_);
    state_ = project();
    if (state_.closed)
        return;
    if (state_.phase == Phase::collecting) {
        if (const auto deadline = receiver_->admit_automatic_return(request_,
                state_.ticket, state_.not_after_us, Mode::multicast,
                configuration_.coverage_packets,
                configuration_.minimum_coverage_us,
                configuration_.proof_lifetime_us)) {
            state_.not_after_us = *deadline;
            ++state_.multicast_admitted;
            state_.through = state_.boundary + configuration_.coverage_packets;
            state_.multicast_result = ReceiverBridge::RequestState::pending;
            state_.phase = Phase::multicast_pending;
        }
        return;
    }
    if (pending(state_.phase))
        return;
    const auto candidate = receiver_->automatic_return_candidate();
    if (!candidate || (has_revision_ && candidate->revision <= state_.revision))
        return;
    if (state_.operations >= configuration_.maximum_operations) {
        state_.phase = Phase::exhausted;
        return;
    }
    request_ = *candidate;
    has_revision_ = true;
    ++state_.operations;
    state_.ticket = state_.operations;
    state_.revision = request_.revision;
    state_.boundary = state_.through = state_.commit_position = 0;
    state_.result = ReceiverBridge::RequestState::pending;
    state_.overlap_result = state_.multicast_result =
        ReceiverBridge::RequestState::none;
    state_.outcome = Outcome::ok;
    state_.not_after_us =
        add(request_.started_at_us, configuration_.operation_timeout_us);
    if (const auto deadline = receiver_->admit_automatic_return(
            request_, state_.ticket, state_.not_after_us, Mode::overlap)) {
        state_.not_after_us = *deadline;
        ++state_.overlap_admitted;
        state_.overlap_result = ReceiverBridge::RequestState::pending;
        state_.phase = Phase::overlap_pending;
    } else {
        state_.result = ReceiverBridge::RequestState::unknown;
        state_.phase = Phase::unknown;
    }
}
bool ReceiverBridge::claim_automatic_return() noexcept
{
    const auto native = service_->snapshot();
    std::lock_guard lock(mutex_);
    if (automatic_return_claimed_ || !detector_ || !state_.attached
        || !(context_.selection.supported & return_control) || cancelled_.load()
        || service_->closed() || native.closed || !native.active
        || service_->now() >= native.expires_at_us)
        return false;
    automatic_return_claimed_ = true;
    return true;
}
std::optional<AutomaticRequest>
ReceiverBridge::automatic_return_candidate() const noexcept
{
    const auto native = service_->snapshot();
    std::lock_guard lock(mutex_);
    const auto at = service_->now();
    if (!automatic_return_claimed_ || !detector_ || cancelled_.load()
        || service_->closed() || native.closed || !native.active
        || at >= native.expires_at_us || posted_
        || (active_ && (want_fallback_ || want_return_)) || !confirmed_
        || state_.mode != Mode::unicast || state_.revision > UINT64_MAX - 2
        || at < state_.confirmation_origin_us
        || at - state_.confirmation_origin_us
            >= configuration_.confirmation_lifetime_us)
        return {};
    const auto a = detector_->snapshot(at);
    const auto data = data_progress();
    if (!a.return_candidate || a.revision != state_.revision || !data
        || data->all.next != a.received_next
        || data->multicast.next != a.received_multicast_next)
        return {};
    return AutomaticRequest {.stream = context_.selection.stream,
        .member = context_.selection.member,
        .generation = generation_,
        .revision = state_.revision,
        .initial_position = context_.selection.initial_position,
        .manual_epoch = manual_epoch_,
        .started_at_us = at,
        .admission_deadline_us =
            std::min({a.valid_until_us, native.expires_at_us,
                add(state_.confirmation_origin_us,
                    configuration_.confirmation_lifetime_us)})};
}
ReceiverBridge::ReturnReceipt ReceiverBridge::automatic_return_receipt(
    const AutomaticRequest& r, std::uint64_t deadline) const noexcept
{
    const auto native = service_->snapshot();
    std::lock_guard lock(mutex_);
    auto s = return_receipt_;
    const auto at = service_->now();
    s.closed = cancelled_.load() || service_->closed() || native.closed
        || !native.active || at >= native.expires_at_us;
    s.current = !s.closed && at >= r.started_at_us && at < deadline
        && r.generation == generation_ && r.stream == context_.selection.stream
        && r.member == context_.selection.member
        && r.manual_epoch == manual_epoch_;
    if (s.result == RequestState::pending
        && (!s.current
            || (posted_ && posted_->return_ticket == s.ticket
                && at >= posted_->not_after)
            || (active_return_ticket_ == s.ticket && at >= deadline_)))
        s.result = RequestState::unknown;
    return s;
}
std::optional<std::uint64_t> ReceiverBridge::admit_automatic_return(
    const AutomaticRequest& r, std::uint64_t ticket, std::uint64_t deadline,
    Mode target, std::uint32_t packets, std::uint32_t minimum_span,
    std::uint32_t proof_lifetime) noexcept
{
    const auto native = service_->snapshot();
    std::lock_guard lock(mutex_);
    auto at = service_->now();
    deadline = std::min(
        {deadline, add(r.started_at_us, configuration_.operation_timeout_us),
            native.expires_at_us});
    if (!ticket || !automatic_return_claimed_ || !detector_ || !state_.attached
        || cancelled_.load() || service_->closed() || native.closed
        || !native.active || posted_
        || (active_ && (want_fallback_ || want_return_)) || !confirmed_
        || at < r.started_at_us || at >= deadline || r.generation != generation_
        || r.stream != context_.selection.stream
        || r.member != context_.selection.member
        || r.manual_epoch != manual_epoch_
        || r.initial_position != context_.selection.initial_position
        || at < state_.confirmation_origin_us
        || at - state_.confirmation_origin_us
            >= configuration_.confirmation_lifetime_us)
        return {};
    detect();
    at = service_->now();
    const auto a = detector_->snapshot(at);
    const auto data = data_progress();
    const auto sampled = service_->now();
    if (sampled < at || sampled >= deadline || sampled >= a.valid_until_us
        || sampled - state_.confirmation_origin_us
            >= configuration_.confirmation_lifetime_us
        || cancelled_.load() || service_->closed() || !data
        || a.revision != state_.revision || a.valid_until_us <= at
        || a.received_next != data->all.next
        || a.received_multicast_next != data->multicast.next)
        return {};
    at = sampled;
    Posted posted;
    posted.at = r.started_at_us;
    posted.not_after = deadline;
    posted.target = target;
    posted.return_ticket = ticket;
    if (target == Mode::overlap) {
        if (state_.mode != Mode::unicast || state_.revision != r.revision
            || r.revision > UINT64_MAX - 2 || at >= r.admission_deadline_us
            || !a.return_candidate)
            return {};
        posted.position = posted.proof_begin =
            context_.selection.initial_position;
        posted.proof_observed = at;
        posted.proof_interval = 1;
    } else if (target == Mode::multicast) {
        if (state_.mode != Mode::overlap || state_.revision != r.revision + 1
            || !state_.has_commit || return_receipt_.ticket != ticket
            || return_receipt_.target != Mode::overlap
            || return_receipt_.result != RequestState::confirmed
            || return_receipt_.revision != state_.revision
            || return_receipt_.boundary != state_.commit_position || packets < 2
            || packets > 32 || state_.commit_position > UINT64_MAX - packets
            || a.multicast_stalled || a.all_data_stalled)
            return {};
        const auto report =
            peer_report_.snapshot(at, configuration_.detection->report_age_us);
        const auto through = state_.commit_position + packets;
        if (!report || !report->fresh
            || report->message.activity != Activity::producing
            || report->message.mode != Mode::overlap
            || report->message.revision != state_.revision
            || report->message.multicast_next < through
            || data->multicast.next < report->message.multicast_next)
            return {};
        const auto runtime = service_->runtime_.lock();
        const auto coverage = runtime
            ? runtime->hybrid_multicast_coverage(service_.get(),
                  state_.commit_position, through,
                  context_.selection.initial_position)
            : std::nullopt;
        const auto checked = service_->now();
        if (!coverage || checked < at || checked >= deadline
            || checked >= a.valid_until_us || cancelled_.load()
            || service_->closed()
            || checked - state_.confirmation_origin_us
                >= configuration_.confirmation_lifetime_us
            || coverage->last_at_us > checked
            || coverage->last_at_us <= coverage->first_at_us
            || coverage->last_at_us - coverage->first_at_us < minimum_span
            || coverage->last_at_us - coverage->first_at_us
                > maximum_interval_us
            || checked - coverage->last_at_us >= proof_lifetime)
            return {};
        posted.position = through;
        posted.proof_begin = state_.commit_position;
        posted.proof_observed = coverage->last_at_us;
        posted.proof_interval = static_cast<std::uint32_t>(
            coverage->last_at_us - coverage->first_at_us);
        posted.not_after =
            std::min(deadline, add(coverage->last_at_us, proof_lifetime));
    } else
        return {};
    posted_ = posted;
    return_receipt_ = {.ticket = ticket,
        .revision = state_.revision,
        .boundary = state_.commit_position,
        .through = posted.position,
        .target = target,
        .result = RequestState::pending};
    state_.returning = RequestState::pending;
    state_.return_outcome = Outcome::ok;
    state_.return_target = target;
    return posted.not_after;
}
}
#endif
