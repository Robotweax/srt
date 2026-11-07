// SPDX-License-Identifier: MIT
#include "compat/hybrid_receiver.hpp"
#include "compat/transport_runtime.hpp"
#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_HYBRID_CONTROL
#include <algorithm>

namespace robotweax::srt::compat::hybrid {
namespace {
std::uint64_t add(std::uint64_t a, std::uint64_t b) noexcept
{
    return b > UINT64_MAX - a ? UINT64_MAX : a + b;
}
}
static_assert(sizeof(ReceiverBridge) < 4096);
ReceiverBridge::ReceiverBridge(
    std::shared_ptr<NativeService> service, Configuration c) noexcept
    : service_(std::move(service))
    , configuration_(c)
{
}
bool ReceiverBridge::valid_configuration(Configuration c) noexcept
{
    return !(!valid(c.reporting) || (c.detection && !valid(*c.detection))
        || !c.retry_interval_us || c.retry_interval_us > c.round_timeout_us
        || c.round_timeout_us > c.operation_timeout_us
        || c.operation_timeout_us > maximum_age_us || !c.query_interval_us
        || c.query_interval_us > maximum_age_us || !c.confirmation_lifetime_us
        || c.confirmation_lifetime_us > maximum_age_us || !c.maximum_sends
        || c.maximum_sends > 8 || !c.maximum_rounds || c.maximum_rounds > 8);
}
std::shared_ptr<ReceiverBridge> ReceiverBridge::create(
    std::shared_ptr<NativeService> service, Configuration c) noexcept
{
    if (!service || !service->configuration_.caller || !valid_configuration(c))
        return {};
    try {
        auto receiver = std::shared_ptr<ReceiverBridge>(
            new ReceiverBridge(std::move(service), c));
        if (c.detection) {
            const auto native = receiver->service_->snapshot();
            receiver->detector_ = std::make_unique<PathDetector>(
                *c.detection, native.context, native.generation);
        }
        receiver->pump();
        if (!receiver->snapshot().attached)
            return {};
        return receiver;
    } catch (...) {
        return {};
    }
}
ReceiverBridge::~ReceiverBridge()
{
    if (state_.attached)
        close();
}
void ReceiverBridge::close() noexcept
{
    cancelled_.store(true);
    service_->close();
}
ReceiverBridge::Snapshot ReceiverBridge::snapshot() const noexcept
{
    const auto native = service_->snapshot();
    std::lock_guard lock(mutex_);
    const auto at = service_->now();
    auto s = state_;
    s.closed = s.closed || cancelled_.load() || native.closed;
    s.fresh = !s.closed && confirmed_ && at >= s.confirmation_origin_us
        && at - s.confirmation_origin_us
            < configuration_.confirmation_lifetime_us;
    if (s.closed) {
        s.phase = Phase::closed;
        if (s.fallback == RequestState::pending)
            s.fallback = RequestState::unknown;
        if (s.returning == RequestState::pending)
            s.returning = RequestState::unknown;
    } else if (!s.fresh && s.phase == Phase::known)
        s.phase = Phase::unknown;
    if (!s.closed && s.attached) {
        s.next_wakeup_us = posted_ ? at
            : active_              ? std::min(deadline_, round_deadline_)
                                   : next_query_;
        if (active_ && sends_ < configuration_.maximum_sends)
            s.next_wakeup_us = std::min(s.next_wakeup_us, next_send_);
        if (s.fresh)
            s.next_wakeup_us = std::min(s.next_wakeup_us,
                add(s.confirmation_origin_us,
                    configuration_.confirmation_lifetime_us));
        if (configuration_.reporting.interval_us && !pending_report_)
            s.next_wakeup_us = std::min(s.next_wakeup_us, next_report_);
        if (detector_)
            s.next_wakeup_us = std::min(
                {s.next_wakeup_us, detector_->snapshot(at).next_wakeup_us,
                    add(last_now_, configuration_.detection->sample_gap_us)});
        s.next_wakeup_us = std::min(s.next_wakeup_us, native.expires_at_us);
    }
    return s;
}
std::optional<PathDetector::Assessment>
ReceiverBridge::path_assessment() const noexcept
{
    const auto native = service_->snapshot();
    std::lock_guard lock(mutex_);
    const auto at = service_->now();
    if (!detector_ || cancelled_.load() || service_->closed() || native.closed
        || !native.active || at >= native.expires_at_us || !state_.attached)
        return {};
    const auto assessment = detector_->snapshot(at);
    const auto data = data_progress();
    if (!data || data->route_epoch != detector_route_epoch_)
        return PathDetector::Assessment {.state = PathDetector::State::unknown,
            .reason = PathDetector::Reason::no_data,
            .observed_at_us = at};
    return assessment;
}
void ReceiverBridge::detect() noexcept
{
    if (!detector_)
        return;
    const auto native = service_->snapshot();
    const auto data = data_progress();
    const auto at = service_->now();
    if (data && data->route_epoch != detector_route_epoch_) {
        *detector_ =
            PathDetector(*configuration_.detection, context_, generation_);
        detector_route_epoch_ = data->route_epoch;
    }
    const auto expiry = add(
        state_.confirmation_origin_us, configuration_.confirmation_lifetime_us);
    detector_->observe({.now_us = at,
        .generation = generation_,
        .source =
            peer_report_.snapshot(at, configuration_.detection->report_age_us),
        .data = data,
        .policy = {
            .confirmed = confirmed_ && at >= state_.confirmation_origin_us,
            .mode = state_.mode,
            .revision = state_.revision,
            .expires_at_us = std::min(expiry, native.expires_at_us)}});
}
std::optional<ReceiveDataProgress>
ReceiverBridge::data_progress() const noexcept
{
    const auto native = service_->snapshot();
    if (cancelled_.load() || native.closed || !native.active)
        return {};
    const auto r = service_->runtime_.lock();
    return r ? r->hybrid_receive_progress(
                   service_.get(), native.context.selection.initial_position)
             : std::nullopt;
}
std::optional<ReportObservation> ReceiverBridge::peer_report(
    std::uint32_t maximum_age) const noexcept
{
    const auto native = service_->snapshot();
    std::lock_guard lock(mutex_);
    const auto at = service_->now();
    if (cancelled_.load() || service_->closed() || native.closed
        || !native.active || at >= native.expires_at_us || !state_.attached)
        return {};
    return peer_report_.snapshot(at, maximum_age);
}
void ReceiverBridge::report() noexcept
{
    const auto at = service_->now();
    const auto c = configuration_.reporting;
    if (!c.interval_us || pending_report_ || at < next_report_)
        return;
    next_report_ = add(at, c.interval_us);
    ++state_.report_attempts;
    if (!next_serial_ || next_serial_ == UINT64_MAX) {
        cancelled_.store(true);
        return;
    }
    const auto data = data_progress();
    const auto sample = data
        ? path_report(*data, context_, state_.mode, state_.revision,
              next_serial_, c, service_->now())
        : std::nullopt;
    if (!sample) {
        ++state_.reports_skipped;
        return;
    }
    const auto result = service_->send_impl(
        sample->message, sample->not_after_us, sample->observed_at_us, true);
    if (result == NativeService::Admission::accepted) {
        pending_report_ = next_serial_++;
        ++state_.reports_queued;
    } else
        ++state_.reports_skipped;
}
bool ReceiverBridge::claim_automatic() noexcept
{
    const auto native = service_->snapshot();
    std::lock_guard lock(mutex_);
    if (automatic_claimed_ || !detector_ || !state_.attached
        || cancelled_.load() || service_->closed() || native.closed
        || !native.active || service_->now() >= native.expires_at_us)
        return false;
    automatic_claimed_ = true;
    return true;
}
std::optional<AutomaticRequest>
ReceiverBridge::automatic_candidate() const noexcept
{
    const auto native = service_->snapshot();
    std::lock_guard lock(mutex_);
    const auto at = service_->now();
    if (!automatic_claimed_ || !detector_ || !state_.attached
        || cancelled_.load() || service_->closed() || native.closed
        || !native.active || at >= native.expires_at_us || posted_
        || (active_ && (want_fallback_ || want_return_)) || !confirmed_
        || state_.mode != Mode::multicast || at < state_.confirmation_origin_us
        || at - state_.confirmation_origin_us
            >= configuration_.confirmation_lifetime_us)
        return {};
    const auto a = detector_->snapshot(at);
    const auto data = data_progress();
    if (!a.fallback_candidate || a.revision != state_.revision || !data
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
std::optional<std::uint64_t> ReceiverBridge::admit_automatic(
    const AutomaticRequest& request, const AutomaticCursor& cursor,
    std::uint64_t ticket, std::uint32_t cursor_lifetime) noexcept
{
    const auto native = service_->snapshot();
    std::lock_guard lock(mutex_);
    const auto at = service_->now();
    if (!ticket || !automatic_claimed_ || !detector_ || !state_.attached
        || cancelled_.load() || service_->closed() || native.closed
        || !native.active || at >= native.expires_at_us || posted_
        || (active_ && (want_fallback_ || want_return_))
        || request.generation != generation_ || cursor.generation != generation_
        || request.stream != context_.selection.stream
        || request.member != context_.selection.member
        || request.initial_position != context_.selection.initial_position
        || request.manual_epoch != manual_epoch_
        || request.revision != state_.revision || !confirmed_
        || state_.mode != Mode::multicast || at < request.started_at_us
        || at >= request.admission_deadline_us
        || at < state_.confirmation_origin_us
        || at - state_.confirmation_origin_us
            >= configuration_.confirmation_lifetime_us
        || cursor.observed_at_us > at
        || at - cursor.observed_at_us >= cursor_lifetime
        || cursor.position < context_.selection.initial_position)
        return {};
    // Pure resampling under receiver ownership, with no provider callbacks or
    // native protocol work. New DATA or activity may have cleared the outage.
    detect();
    const auto admitted_at = service_->now();
    const auto a = detector_->snapshot(admitted_at);
    const auto data = data_progress();
    if (admitted_at < at || admitted_at >= request.admission_deadline_us
        || cancelled_.load() || service_->closed() || !a.fallback_candidate
        || a.revision != state_.revision || !data
        || cursor.position > data->window_floor
        || data->all.next != a.received_next
        || data->multicast.next != a.received_multicast_next)
        return {};
    const auto origin = std::min(request.started_at_us, cursor.observed_at_us);
    const auto deadline =
        std::min({add(origin, configuration_.operation_timeout_us),
            add(cursor.observed_at_us, cursor_lifetime), native.expires_at_us});
    if (admitted_at >= deadline)
        return {};
    posted_ = Posted {cursor.position, origin, deadline, ticket};
    automatic_receipt_ = {ticket, RequestState::pending, Outcome::ok};
    state_.fallback = RequestState::pending;
    state_.fallback_outcome = Outcome::ok;
    return deadline;
}
ReceiverBridge::AutomaticReceipt
ReceiverBridge::automatic_receipt() const noexcept
{
    const auto native = service_->snapshot();
    std::lock_guard lock(mutex_);
    auto receipt = automatic_receipt_;
    const auto at = service_->now();
    if (receipt.result == RequestState::pending
        && (cancelled_.load() || service_->closed() || native.closed
            || at >= native.expires_at_us
            || (posted_ && posted_->automatic_ticket == receipt.ticket
                && at >= posted_->not_after)
            || (active_automatic_ticket_ == receipt.ticket && at >= deadline_)))
        receipt.result = RequestState::unknown;
    return receipt;
}
std::optional<CoverageProof> ReceiverBridge::coverage_proof(
    std::uint64_t through, std::uint32_t lifetime) const noexcept
{
    const auto native = service_->snapshot();
    std::lock_guard lock(mutex_);
    const auto at = service_->now();
    if (!lifetime || lifetime > maximum_age_us || cancelled_.load()
        || service_->closed() || native.closed || !native.active
        || at >= native.expires_at_us || !confirmed_
        || state_.mode != Mode::overlap || !state_.has_commit
        || !(context_.selection.supported & return_control)
        || at < state_.confirmation_origin_us
        || at - state_.confirmation_origin_us
            >= configuration_.confirmation_lifetime_us)
        return {};
    const auto runtime = service_->runtime_.lock();
    const auto coverage = runtime
        ? runtime->hybrid_multicast_coverage(service_.get(),
              state_.commit_position, through,
              context_.selection.initial_position)
        : std::nullopt;
    const auto checked = service_->now();
    if (cancelled_.load() || service_->closed() || checked < at
        || checked >= native.expires_at_us
        || checked - state_.confirmation_origin_us
            >= configuration_.confirmation_lifetime_us
        || !coverage || coverage->last_at_us > checked
        || coverage->last_at_us <= coverage->first_at_us
        || coverage->last_at_us - coverage->first_at_us > maximum_interval_us
        || checked - coverage->last_at_us >= lifetime)
        return {};
    return CoverageProof {.generation = generation_,
        .revision = state_.revision,
        .boundary = state_.commit_position,
        .through = through,
        .observed_at_us = coverage->last_at_us,
        .not_after_us =
            std::min(add(coverage->last_at_us, lifetime), native.expires_at_us),
        .interval_us = static_cast<std::uint32_t>(
            coverage->last_at_us - coverage->first_at_us),
        .route_epoch = coverage->route_epoch};
}
ReceiverBridge::Admission ReceiverBridge::request_overlap() noexcept
{
    const auto native = service_->snapshot();
    std::lock_guard lock(mutex_);
    const auto at = service_->now();
    if (cancelled_.load() || service_->closed() || native.closed
        || !native.active || at >= native.expires_at_us || !state_.attached)
        return Admission::closed;
    if (!(context_.selection.supported & return_control) || !confirmed_
        || state_.mode != Mode::unicast || at < state_.confirmation_origin_us
        || at - state_.confirmation_origin_us
            >= configuration_.confirmation_lifetime_us)
        return Admission::invalid;
    if (posted_ || (active_ && (want_fallback_ || want_return_)))
        return Admission::busy;
    if (manual_epoch_ == UINT64_MAX)
        return Admission::invalid;
    ++manual_epoch_;
    posted_ = Posted {context_.selection.initial_position, at,
        add(at, configuration_.operation_timeout_us), 0, Mode::overlap,
        context_.selection.initial_position, at, 1};
    state_.returning = RequestState::pending;
    state_.return_outcome = Outcome::ok;
    state_.return_target = Mode::overlap;
    return Admission::accepted;
}
ReceiverBridge::Admission ReceiverBridge::request_multicast(
    const CoverageProof& proof) noexcept
{
    const auto native = service_->snapshot();
    std::lock_guard lock(mutex_);
    const auto at = service_->now();
    if (cancelled_.load() || service_->closed() || native.closed
        || !native.active || at >= native.expires_at_us || !state_.attached)
        return Admission::closed;
    if (!(context_.selection.supported & return_control) || !confirmed_
        || state_.mode != Mode::overlap || !state_.has_commit
        || at < state_.confirmation_origin_us
        || at - state_.confirmation_origin_us
            >= configuration_.confirmation_lifetime_us
        || proof.generation != generation_ || proof.revision != state_.revision
        || proof.boundary != state_.commit_position
        || proof.through <= proof.boundary || proof.observed_at_us > at
        || proof.not_after_us <= at
        || proof.not_after_us > add(proof.observed_at_us, maximum_age_us)
        || !proof.interval_us || proof.interval_us > maximum_interval_us)
        return Admission::invalid;
    if (posted_ || (active_ && (want_fallback_ || want_return_)))
        return Admission::busy;
    const auto data = data_progress();
    const auto runtime = service_->runtime_.lock();
    if (!data || !runtime || !data->multicast.observed
        || !runtime->hybrid_multicast_fence(service_.get(), proof.route_epoch)
        || proof.through > data->multicast.next)
        return Admission::invalid;
    if (manual_epoch_ == UINT64_MAX)
        return Admission::invalid;
    ++manual_epoch_;
    posted_ = Posted {proof.through, proof.observed_at_us, proof.not_after_us,
        0, Mode::multicast, proof.boundary, proof.observed_at_us,
        proof.interval_us, 0, proof.route_epoch};
    state_.returning = RequestState::pending;
    state_.return_outcome = Outcome::ok;
    state_.return_target = Mode::multicast;
    return Admission::accepted;
}
ReceiverBridge::Admission ReceiverBridge::request_fallback(
    std::uint64_t position) noexcept
{
    const auto native = service_->snapshot();
    std::lock_guard lock(mutex_);
    if (cancelled_.load() || native.closed || !state_.attached)
        return Admission::closed;
    if (position < context_.selection.initial_position)
        return Admission::invalid;
    if (posted_ || (active_ && (want_fallback_ || want_return_)))
        return Admission::busy;
    const auto at = service_->now();
    if (confirmed_ && state_.mode == Mode::unicast
        && at >= state_.confirmation_origin_us
        && at - state_.confirmation_origin_us
            < configuration_.confirmation_lifetime_us)
        return Admission::already_unicast;
    if (manual_epoch_ == UINT64_MAX)
        return Admission::invalid;
    ++manual_epoch_;
    posted_ = Posted {position, at};
    state_.fallback = RequestState::pending;
    state_.fallback_outcome = Outcome::ok;
    return Admission::accepted;
}
void ReceiverBridge::pump() noexcept
{
    if (!cancelled_.load())
        (void)service_->receiver_work(*this);
}
void ReceiverBridge::finish(Phase phase, std::uint64_t at) noexcept
{
    if (phase == Phase::unknown)
        confirmed_ = false;
    if (want_fallback_) {
        state_.fallback = phase == Phase::known ? RequestState::confirmed
            : phase == Phase::denied            ? RequestState::denied
                                                : RequestState::unknown;
    }
    if (want_return_)
        state_.returning = phase == Phase::known ? RequestState::confirmed
            : phase == Phase::denied             ? RequestState::denied
                                                 : RequestState::unknown;
    if (active_return_ticket_)
        return_receipt_ = {.ticket = active_return_ticket_,
            .revision = state_.revision,
            .boundary = state_.commit_position,
            .through = requested_position_,
            .target = target_mode_,
            .result = state_.returning,
            .outcome = state_.return_outcome};
    active_return_ticket_ = 0;
    want_return_ = false;
    if (active_automatic_ticket_)
        automatic_receipt_ = {
            active_automatic_ticket_, state_.fallback, state_.fallback_outcome};
    active_automatic_ticket_ = 0;
    active_ = false;
    want_fallback_ = false;
    state_.phase = phase;
    next_query_ = add(at, configuration_.query_interval_us);
}
void ReceiverBridge::start(bool fallback, std::uint64_t position,
    std::uint64_t at, std::uint64_t origin, std::uint64_t not_after,
    std::uint64_t automatic_ticket, Mode target, std::uint64_t proof_begin,
    std::uint32_t proof_interval, std::uint64_t proof_observed,
    std::uint64_t return_ticket, std::uint64_t route_epoch) noexcept
{
    target_mode_ = target;
    want_return_ = fallback && target != Mode::unicast;
    expected_revision_ = state_.revision;
    proof_begin_ = proof_begin;
    proof_interval_ = proof_interval;
    proof_observed_ = proof_observed;
    proof_route_epoch_ = route_epoch;
    active_automatic_ticket_ = automatic_ticket;
    active_return_ticket_ = return_ticket;
    active_ = true;
    want_fallback_ = fallback && !want_return_;
    rounds_ = 0;
    deadline_ = std::min({add(origin, configuration_.operation_timeout_us),
        service_->state_.expires_at_us, not_after});
    if (at >= deadline_) {
        ++state_.exhausted;
        finish(Phase::unknown, at);
        return;
    }
    requested_position_ = position;
    round(want_return_ ? Kind::multicast_ready
            : fallback ? Kind::fallback_request
                       : Kind::delivery_query,
        at);
}
void ReceiverBridge::round(Kind kind, std::uint64_t at) noexcept
{
    if (!next_serial_ || next_serial_ == UINT64_MAX) {
        cancelled_.store(true);
        return;
    }
    const auto position =
        (kind == Kind::fallback_request || kind == Kind::multicast_ready)
        ? requested_position_
        : context_.selection.initial_position;
    request_ = {.kind = kind,
        .stream = context_.selection.stream,
        .member = context_.selection.member,
        .serial = next_serial_++,
        .revision = kind == Kind::multicast_ready ? expected_revision_
                                                  : state_.revision,
        .next_position = position,
        .next_sequence = static_cast<std::uint32_t>(
            (context_.dispatch.initial_sequence
                + ((position - context_.selection.initial_position)
                    & SequenceNumber::mask))
            & SequenceNumber::mask),
        .mode = kind == Kind::fallback_request ? Mode::unicast
            : kind == Kind::multicast_ready    ? target_mode_
                                               : Mode::multicast,
        .multicast_next = kind == Kind::multicast_ready ? proof_begin_ : 0,
        .interval_us = kind == Kind::multicast_ready ? proof_interval_ : 0};
    round_deadline_ =
        std::min(deadline_, add(at, configuration_.round_timeout_us));
    next_send_ = at;
    sends_ = 0;
    sent_ = false;
    queued_ = false;
    ++rounds_;
    ++state_.rounds;
    state_.request_serial = request_.serial;
    state_.phase =
        kind == Kind::fallback_request ? Phase::requesting : Phase::reconciling;
    if (kind == Kind::delivery_query)
        ++state_.queries;
}
void ReceiverBridge::event(
    const NativeService::Event& e, std::uint64_t at) noexcept
{
    if (e.message.kind == Kind::path_report
        && e.kind != NativeService::Event::Kind::received) {
        if (e.message.serial == pending_report_) {
            pending_report_ = 0;
            if (e.kind == NativeService::Event::Kind::submitted)
                ++state_.reports_submitted;
        }
        return;
    }
    if (e.kind == NativeService::Event::Kind::received
        && e.message.kind == Kind::stream_progress
        && e.generation == generation_) {
        if (!peer_report_.accept(e.message,
                e.received.status == Session::Status::duplicate,
                e.received.first_received_us, at))
            ++state_.ignored;
        return;
    }
    if (e.kind != NativeService::Event::Kind::received) {
        if (e.kind == NativeService::Event::Kind::submitted && active_
            && e.message.serial == request_.serial)
            sent_ = true;
        return;
    }
    if (e.generation != generation_
        || (e.message.kind != Kind::delivery_commit
            && e.message.kind != Kind::delivery_status)) {
        ++state_.ignored;
        return;
    }
    const auto& m = e.message;
    const bool boundary = (context_.selection.supported & return_control)
        || m.kind == Kind::delivery_commit;
    const auto commit = (context_.selection.supported & return_control)
        ? m.multicast_next
        : m.next_position;
    if (state_.observed && m.revision < state_.revision) {
        ++state_.stale;
        return;
    }
    if (state_.observed && m.revision > state_.revision
        && m.next_position < state_.cursor) {
        ++state_.conflicts;
        cancelled_.store(true);
        return;
    }
    if (state_.observed && m.revision == state_.revision
        && (m.mode != state_.mode
            || (boundary && state_.has_commit
                && commit != state_.commit_position))) {
        ++state_.conflicts;
        cancelled_.store(true);
        return;
    }
    const bool newer = !state_.observed || m.revision > state_.revision;
    if (newer) {
        state_.revision = m.revision;
        state_.mode = m.mode;
        state_.has_commit = false;
        state_.cursor = m.next_position;
        state_.retained_floor = m.retained_floor;
        state_.observed = true;
        confirmed_ = false;
    } else {
        state_.cursor = std::max(state_.cursor, m.next_position);
        state_.retained_floor =
            std::max(state_.retained_floor, m.retained_floor);
    }
    if (boundary) {
        state_.has_commit = true;
        state_.commit_position = commit;
    }
    if (e.received.status == Session::Status::duplicate) {
        ++state_.duplicates;
        return;
    }
    // Old/cached replies can advance historical knowledge, never freshness.
    const bool ordered = m.serial > highest_reply_;
    highest_reply_ = std::max(highest_reply_, m.serial);
    const auto first = e.received.first_received_us;
    if (!active_ || !sent_ || m.request_serial != request_.serial || !ordered
        || at >= round_deadline_ || at >= deadline_ || first < first_send_
        || first >= round_deadline_ || at < first
        || at - first >= configuration_.confirmation_lifetime_us
        || at < first_send_
        || at - first_send_ >= configuration_.confirmation_lifetime_us) {
        ++state_.unmatched;
        return;
    }
    confirmed_ = true;
    state_.confirmation_origin_us = first_send_;
    state_.outcome = m.outcome;
    if (want_fallback_)
        state_.fallback_outcome = m.outcome;
    if (want_return_)
        state_.return_outcome = m.outcome;
    ++state_.confirmed;
    if (want_return_) {
        if (m.mode == target_mode_ && m.revision == expected_revision_ + 1
            && state_.has_commit) {
            finish(Phase::known, at);
            return;
        }
        if (request_.kind == Kind::multicast_ready
            || m.revision != expected_revision_
            || rounds_ >= configuration_.maximum_rounds) {
            finish(Phase::denied, at);
            return;
        }
        round(Kind::multicast_ready, at);
        return;
    }
    if (want_fallback_ && m.mode != Mode::unicast) {
        if (request_.kind == Kind::fallback_request) {
            finish(Phase::denied, at);
            return;
        }
        if (rounds_ < configuration_.maximum_rounds) {
            round(Kind::fallback_request, at);
            return;
        }
        ++state_.exhausted;
        finish(Phase::unknown, at);
        return;
    }
    finish(Phase::known, at);
}
void ReceiverBridge::work() noexcept
{
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock() || cancelled_.load())
        return;
    const auto at = service_->now();
    if (state_.attached && at < last_now_) {
        cancelled_.store(true);
        lock.unlock();
        close();
        return;
    }
    last_now_ = at;
    if (!state_.attached) {
        std::lock_guard queue(service_->queue_mutex_);
        if (service_->closed() || !service_->state_.active || service_->bridged_
            || service_->application_used_
            || !(service_->state_.context.selection.supported & policy_query))
            return;
        context_ = service_->state_.context;
        peer_report_ = ReportTracker(
            Kind::stream_progress, context_.selection.initial_position);
        generation_ = service_->state_.generation;
        service_->bridged_ = true;
        state_.attached = true;
        state_.cursor = state_.retained_floor =
            context_.selection.initial_position;
        next_query_ = at;
        return;
    }
    // A posted user intent replaces a read-only cycle; old replies cannot
    // confirm the replacement because its request serial is different.
    if (posted_) {
        const auto posted = *posted_;
        posted_.reset();
        start(true, posted.position, at, posted.at, posted.not_after,
            posted.automatic_ticket, posted.target, posted.proof_begin,
            posted.proof_interval, posted.proof_observed, posted.return_ticket,
            posted.route_epoch);
    }
    for (std::size_t i = 0; i < NativeService::batch; ++i) {
        NativeService::Event e;
        {
            std::lock_guard queue(service_->queue_mutex_);
            if (service_->closed() || !service_->event_size_)
                break;
            e = service_->events_[service_->event_head_];
            service_->event_head_ =
                (service_->event_head_ + 1) % service_->event_capacity;
            --service_->event_size_;
            service_->state_.events = service_->event_size_;
        }
        event(e, at);
        if (cancelled_.load())
            break;
    }
    if (!cancelled_.load()) {
        if (active_ && at >= deadline_) {
            ++state_.exhausted;
            confirmed_ = false;
            finish(Phase::unknown, at);
        } else if (active_ && at >= round_deadline_) {
            if (rounds_ >= configuration_.maximum_rounds) {
                ++state_.exhausted;
                confirmed_ = false;
                finish(Phase::unknown, at);
            } else
                round(Kind::delivery_query, at);
        }
        if (!active_ && at >= next_query_)
            start(false, context_.selection.initial_position, at, at);
        if (active_ && at >= next_send_ && sends_ < configuration_.maximum_sends
            && at < round_deadline_) {
            if (request_.kind == Kind::multicast_ready && sends_) {
                if (!next_serial_ || next_serial_ == UINT64_MAX) {
                    cancelled_.store(true);
                    return;
                }
                request_.serial = next_serial_++;
                state_.request_serial = request_.serial;
                sent_ = queued_ = false;
            }
            if (request_.kind == Kind::multicast_ready
                && target_mode_ == Mode::multicast) {
                const auto runtime = service_->runtime_.lock();
                if (!runtime
                    || !runtime->hybrid_multicast_fence(
                        service_.get(), proof_route_epoch_)) {
                    finish(Phase::unknown, at);
                    return;
                }
            }
            ++sends_;
            next_send_ = add(at, configuration_.retry_interval_us);
            const auto admission =
                service_->send_impl(request_, round_deadline_,
                    request_.kind == Kind::multicast_ready
                        ? std::optional<std::uint64_t> {proof_observed_}
                        : std::nullopt,
                    true,
                    request_.kind == Kind::multicast_ready
                            && target_mode_ == Mode::multicast
                        ? proof_route_epoch_
                        : 0);
            if (admission == NativeService::Admission::accepted) {
                if (!queued_) {
                    queued_ = true;
                    first_send_ = at;
                }
                ++state_.attempts;
            } else if (admission == NativeService::Admission::full)
                ++state_.refused;
            else
                cancelled_.store(true);
        }
    }
    if (!cancelled_.load()) {
        detect();
        report();
    }
    if (cancelled_.load()) {
        state_.phase = Phase::closed;
        lock.unlock();
        close();
    }
}
}
#endif
