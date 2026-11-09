// SPDX-License-Identifier: MIT
#include "compat/hybrid/hybrid_detector.hpp"
#include <algorithm>

namespace robotweax::srt::compat::hybrid {
namespace {
std::uint64_t add(std::uint64_t a, std::uint64_t b) noexcept
{
    return b > UINT64_MAX - a ? UINT64_MAX : a + b;
}
bool bounded(std::uint32_t n) noexcept
{
    return n && n <= maximum_age_us;
}
bool water_valid(
    const DataHighWater& w, std::uint64_t origin, std::uint64_t now) noexcept
{
    return w.next >= origin
        && (w.observed
                ? (w.next > origin && w.packets && w.last_advance_us <= now)
                : (w.next == origin && !w.last_advance_us && !w.packets));
}
bool forward(const DataHighWater& old, const DataHighWater& next) noexcept
{
    return next.next >= old.next && (!old.observed || next.observed)
        && (next.next != old.next
            || next.last_advance_us == old.last_advance_us);
}
}
static_assert(sizeof(PathDetector) < 2048);
bool valid(DetectionConfiguration c) noexcept
{
    return bounded(c.report_age_us) && bounded(c.maximum_transit_us)
        && bounded(c.stall_us) && bounded(c.emission_lifetime_us)
        && bounded(c.recovery_us) && bounded(c.recovery_gap_us)
        && bounded(c.hold_down_us) && bounded(c.sample_gap_us)
        && c.recovery_advances >= 2 && c.recovery_advances <= 64
        && c.maximum_transit_us <= c.stall_us && c.sample_gap_us <= c.stall_us
        && c.sample_gap_us <= c.recovery_gap_us
        && c.recovery_gap_us <= c.recovery_us
        && static_cast<std::uint64_t>(c.report_age_us)
        > static_cast<std::uint64_t>(c.maximum_transit_us) + c.sample_gap_us
        && static_cast<std::uint64_t>(c.emission_lifetime_us)
        > static_cast<std::uint64_t>(c.stall_us) + c.maximum_transit_us
            + c.sample_gap_us;
}
PathDetector::PathDetector(DetectionConfiguration c, Context context,
    std::uint64_t generation) noexcept
    : configuration_(c)
    , context_(std::move(context))
    , generation_(generation)
{
}
void PathDetector::reset_evidence() noexcept
{
    baseline_ = mc_gap_ = all_gap_ = outage_ = recovery_ = previous_stall_ =
        false;
    mc_emission_until_ = all_emission_until_ = hold_until_ = 0;
    recovery_start_ = recovery_last_ = 0;
    recovery_data_ = recovery_source_ = 0;
}
void PathDetector::unknown(Reason reason, std::uint64_t now) noexcept
{
    reset_evidence();
    state_ = {.state = State::unknown, .reason = reason, .observed_at_us = now};
}
void PathDetector::observe(const Input& input) noexcept
{
    const auto now = input.now_us;
    const auto c = configuration_;
    if (now < last_now_)
        clock_failed_ = true;
    last_now_ = std::max(now, last_now_);
    if (!valid(c) || !valid(context_) || !generation_) {
        unknown(Reason::configuration, now);
        return;
    }
    if (input.generation != generation_)
        lifetime_failed_ = true;
    if (clock_failed_) {
        unknown(Reason::clock, now);
        return;
    }
    if (lifetime_failed_) {
        unknown(Reason::lifetime, now);
        return;
    }
    const bool sample_gap = sampled_ && now - last_sample_ > c.sample_gap_us;
    const bool expired =
        sampled_ && state_.valid_until_us && now >= state_.valid_until_us;
    last_sample_ = now;
    sampled_ = true;
    if (!input.source) {
        unknown(Reason::no_report, now);
        return;
    }
    const auto& r = *input.source;
    const auto& m = r.message;
    if (!r.fresh || r.first_received_us > now || m.age_us >= c.report_age_us
        || static_cast<std::uint64_t>(m.age_us) + c.maximum_transit_us
            >= c.report_age_us
        || now - r.first_received_us
            >= c.report_age_us - m.age_us - c.maximum_transit_us) {
        unknown(Reason::stale_report, now);
        return;
    }
    if (!input.data) {
        unknown(Reason::no_data, now);
        return;
    }
    const auto& d = *input.data;
    const auto origin = context_.selection.initial_position;
    if (!valid(m, context_) || m.kind != Kind::stream_progress || !d.valid
        || d.origin != origin || !water_valid(d.all, origin, now)
        || !water_valid(d.multicast, origin, now)
        || !water_valid(d.unicast, origin, now) || d.multicast.next > d.all.next
        || d.unicast.next > d.all.next
        || (report_
            && (m.serial < report_->serial
                || r.first_received_us < last_report_received_
                || (m.serial == report_->serial
                    && (m != *report_
                        || r.first_received_us != last_report_received_))
                || (report_->activity == Activity::ended
                    && m.activity != Activity::source_failed
                    && (m.activity != Activity::ended
                        || m.produced_next != report_->produced_next))
                || (report_->activity == Activity::source_failed
                    && m.activity != Activity::source_failed)
                || m.next_position < report_->next_position
                || m.multicast_next < report_->multicast_next
                || m.produced_next < report_->produced_next
                || m.retained_floor < report_->retained_floor
                || m.revision < report_->revision
                || (m.revision == report_->revision
                    && m.mode != report_->mode)))
        || (data_
            && (!forward(data_->all, d.all)
                || !forward(data_->multicast, d.multicast)
                || !forward(data_->unicast, d.unicast)))) {
        unknown(Reason::invalid_evidence, now);
        return;
    }
    const bool mc_emitted =
        report_ && m.multicast_next > report_->multicast_next;
    const bool all_emitted =
        report_ && m.next_position > report_->next_position;
    const bool mc_received = data_ && d.multicast.next > data_->multicast.next;
    const bool all_received = data_ && d.all.next > data_->all.next;
    report_ = m;
    last_report_received_ = r.first_received_us;
    data_ = d;
    if (!input.policy.confirmed || now >= input.policy.expires_at_us
        || m.revision != input.policy.revision || m.mode != input.policy.mode) {
        unknown(Reason::policy, now);
        return;
    }
    const bool changed_policy = policy_.confirmed
        && (input.policy.revision != policy_.revision
            || input.policy.mode != policy_.mode);
    policy_ = input.policy;
    if (sample_gap || expired || changed_policy)
        reset_evidence();
    state_ = {.state = State::observing,
        .observed_at_us = now,
        .valid_until_us =
            std::min({add(now, c.sample_gap_us), input.policy.expires_at_us,
                add(r.first_received_us,
                    c.report_age_us - m.age_us - c.maximum_transit_us)}),
        .revision = m.revision,
        .source_next = m.next_position,
        .source_multicast_next = m.multicast_next,
        .received_next = d.all.next,
        .received_multicast_next = d.multicast.next};
    const auto activity = [&]() -> std::optional<State> {
        switch (m.activity) {
        case Activity::startup:
            return State::startup;
        case Activity::paused:
            return State::paused;
        case Activity::ended:
            return State::ended;
        case Activity::source_failed:
            return State::source_failed;
        case Activity::producing:
            return {};
        }
        return State::unknown;
    }();
    if (activity) {
        reset_evidence();
        state_.state = *activity;
        state_.next_wakeup_us = state_.valid_until_us;
        return;
    }
    if (!baseline_) {
        baseline_ = true;
        hold_until_ = m.mode == Mode::unicast ? add(now, c.hold_down_us) : 0;
        state_.reason = sample_gap ? Reason::sampling_gap
            : expired              ? Reason::assessment_expired
                                   : Reason::none;
        state_.next_wakeup_us = state_.valid_until_us;
        return;
    }
    if (mc_emitted)
        mc_emission_until_ = add(r.first_received_us, c.emission_lifetime_us);
    if (all_emitted)
        all_emission_until_ = add(r.first_received_us, c.emission_lifetime_us);
    const bool mc_active = now < mc_emission_until_,
               all_active = now < all_emission_until_;
    const auto budget =
        static_cast<std::uint64_t>(c.stall_us) + c.maximum_transit_us;
    if (mc_active && m.multicast_next > d.multicast.next) {
        if (!mc_gap_) {
            mc_gap_ = true;
            mc_gap_since_ = now;
        } else if (mc_received)
            mc_gap_since_ =
                std::max(mc_gap_since_, d.multicast.last_advance_us);
    } else
        mc_gap_ = false;
    if (all_active && m.next_position > d.all.next) {
        if (!all_gap_) {
            all_gap_ = true;
            all_gap_since_ = now;
        } else if (all_received)
            all_gap_since_ = std::max(all_gap_since_, d.all.last_advance_us);
    } else
        all_gap_ = false;
    state_.multicast_stalled = mc_gap_ && now >= add(mc_gap_since_, budget);
    state_.all_data_stalled = all_gap_ && now >= add(all_gap_since_, budget);
    const bool stalled = state_.multicast_stalled || state_.all_data_stalled;
    if (stalled && !previous_stall_) {
        outage_ = true;
        hold_until_ = std::max(hold_until_, add(now, c.hold_down_us));
    }
    previous_stall_ = stalled;
    const bool needs_recovery = outage_ || m.mode == Mode::unicast;
    if (stalled || !mc_active
        || (recovery_ && now >= add(recovery_last_, c.recovery_gap_us))) {
        recovery_ = false;
        recovery_data_ = recovery_source_ = 0;
    }
    if (needs_recovery && !stalled && mc_active && mc_received
        && now - d.multicast.last_advance_us < c.recovery_gap_us) {
        const auto at = d.multicast.last_advance_us;
        if (!recovery_ || at < recovery_last_
            || at - recovery_last_ > c.recovery_gap_us) {
            recovery_ = true;
            recovery_start_ = at;
            recovery_data_ = recovery_source_ = 0;
        }
        recovery_last_ = at;
        recovery_data_ = std::min<std::uint32_t>(64, recovery_data_ + 1);
    }
    if (recovery_ && mc_emitted && r.first_received_us >= recovery_start_)
        recovery_source_ = std::min<std::uint32_t>(64, recovery_source_ + 1);
    const bool recovered = recovery_ && recovery_data_ >= c.recovery_advances
        && recovery_source_ >= c.recovery_advances
        && recovery_last_ >= add(recovery_start_, c.recovery_us)
        && now >= hold_until_ && d.multicast.next >= m.multicast_next;
    state_.fallback_candidate =
        m.mode == Mode::multicast && state_.multicast_stalled;
    state_.return_candidate = m.mode == Mode::unicast && recovered;
    state_.recovery_data_advances = recovery_data_;
    state_.recovery_source_advances = recovery_source_;
    state_.state = state_.all_data_stalled ? State::all_data_stalled
        : state_.multicast_stalled         ? State::multicast_stalled
        : state_.return_candidate          ? State::multicast_ready
        : needs_recovery && recovery_      ? State::recovering
        : mc_gap_                          ? State::multicast_suspect
        : mc_active                        ? State::multicast_progress
                                           : State::awaiting_emission;
    if (recovered && m.mode == Mode::multicast) {
        outage_ = false;
        state_.state = State::multicast_progress;
    }
    if (mc_active)
        state_.valid_until_us =
            std::min(state_.valid_until_us, mc_emission_until_);
    if (all_active)
        state_.valid_until_us =
            std::min(state_.valid_until_us, all_emission_until_);
    if (recovery_)
        state_.valid_until_us = std::min(
            state_.valid_until_us, add(recovery_last_, c.recovery_gap_us));
    state_.next_wakeup_us = state_.valid_until_us;
    for (const auto deadline :
        {mc_gap_ ? add(mc_gap_since_, budget) : UINT64_MAX,
            all_gap_ ? add(all_gap_since_, budget) : UINT64_MAX,
            recovery_ ? hold_until_ : UINT64_MAX})
        if (deadline > now)
            state_.next_wakeup_us = std::min(state_.next_wakeup_us, deadline);
}
PathDetector::Assessment PathDetector::snapshot(
    std::uint64_t now) const noexcept
{
    if (now < last_now_)
        clock_failed_ = true;
    last_now_ = std::max(last_now_, now);
    auto result = state_;
    if (clock_failed_
        || (result.valid_until_us && now >= result.valid_until_us)) {
        result = {.state = State::unknown,
            .reason =
                clock_failed_ ? Reason::clock : Reason::assessment_expired,
            .observed_at_us = now};
    }
    return result;
}
}
