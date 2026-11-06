// SPDX-License-Identifier: MIT
#include "compat/hybrid_receiver.hpp"
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
std::shared_ptr<ReceiverBridge> ReceiverBridge::create(
    std::shared_ptr<NativeService> service, Configuration c) noexcept
{
    if (!service || !service->configuration_.caller || !c.retry_interval_us
        || c.retry_interval_us > c.round_timeout_us
        || c.round_timeout_us > c.operation_timeout_us
        || c.operation_timeout_us > maximum_age_us || !c.query_interval_us
        || c.query_interval_us > maximum_age_us || !c.confirmation_lifetime_us
        || c.confirmation_lifetime_us > maximum_age_us || !c.maximum_sends
        || c.maximum_sends > 8 || !c.maximum_rounds || c.maximum_rounds > 8)
        return {};
    try {
        auto receiver = std::shared_ptr<ReceiverBridge>(
            new ReceiverBridge(std::move(service), c));
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
    const auto at = service_->now();
    const auto native = service_->snapshot();
    std::lock_guard lock(mutex_);
    auto s = state_;
    s.closed = s.closed || cancelled_.load() || native.closed;
    s.fresh = !s.closed && confirmed_ && at >= s.confirmation_origin_us
        && at - s.confirmation_origin_us
            < configuration_.confirmation_lifetime_us;
    if (s.closed) {
        s.phase = Phase::closed;
        if (s.fallback == RequestState::pending)
            s.fallback = RequestState::unknown;
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
        s.next_wakeup_us = std::min(s.next_wakeup_us, native.expires_at_us);
    }
    return s;
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
    if (posted_ || (active_ && want_fallback_))
        return Admission::busy;
    const auto at = service_->now();
    if (confirmed_ && state_.mode == Mode::unicast
        && at >= state_.confirmation_origin_us
        && at - state_.confirmation_origin_us
            < configuration_.confirmation_lifetime_us)
        return Admission::already_unicast;
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
        state_.fallback_outcome = state_.outcome;
    }
    active_ = false;
    want_fallback_ = false;
    state_.phase = phase;
    next_query_ = add(at, configuration_.query_interval_us);
}
void ReceiverBridge::start(bool fallback, std::uint64_t position,
    std::uint64_t at, std::uint64_t origin) noexcept
{
    active_ = true;
    want_fallback_ = fallback;
    rounds_ = 0;
    deadline_ = std::min(add(origin, configuration_.operation_timeout_us),
        service_->state_.expires_at_us);
    if (at >= deadline_) {
        ++state_.exhausted;
        finish(Phase::unknown, at);
        return;
    }
    requested_position_ = position;
    round(fallback ? Kind::fallback_request : Kind::delivery_query, at);
}
void ReceiverBridge::round(Kind kind, std::uint64_t at) noexcept
{
    if (!next_serial_ || next_serial_ == UINT64_MAX) {
        cancelled_.store(true);
        return;
    }
    const auto position = kind == Kind::fallback_request
        ? requested_position_
        : context_.selection.initial_position;
    request_ = {.kind = kind,
        .stream = context_.selection.stream,
        .member = context_.selection.member,
        .serial = next_serial_++,
        .revision = state_.revision,
        .next_position = position,
        .next_sequence = static_cast<std::uint32_t>(
            (context_.dispatch.initial_sequence
                + ((position - context_.selection.initial_position)
                    & SequenceNumber::mask))
            & SequenceNumber::mask),
        .mode =
            kind == Kind::fallback_request ? Mode::unicast : Mode::multicast};
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
            || (m.kind == Kind::delivery_commit && state_.has_commit
                && m.next_position != state_.commit_position))) {
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
    if (m.kind == Kind::delivery_commit) {
        state_.has_commit = true;
        state_.commit_position = m.next_position;
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
    ++state_.confirmed;
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
        start(true, posted.position, at, posted.at);
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
            ++sends_;
            next_send_ = add(at, configuration_.retry_interval_us);
            const auto admission = service_->send_impl(
                request_, round_deadline_, std::nullopt, true);
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
    if (cancelled_.load()) {
        state_.phase = Phase::closed;
        lock.unlock();
        close();
    }
}
}
#endif
