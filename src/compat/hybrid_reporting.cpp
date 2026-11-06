// SPDX-License-Identifier: MIT
#include "compat/hybrid_reporting.hpp"
#include <algorithm>

namespace robotweax::srt::compat::hybrid {
namespace {
std::uint64_t add(std::uint64_t a, std::uint64_t b) noexcept
{
    return b > UINT64_MAX - a ? UINT64_MAX : a + b;
}
std::uint32_t sequence(const Context& c, std::uint64_t position) noexcept
{
    return static_cast<std::uint32_t>(
        (c.dispatch.initial_sequence
            + ((position - c.selection.initial_position)
                & SequenceNumber::mask))
        & SequenceNumber::mask);
}
}
bool SourceActivity::declare(Activity state, std::uint64_t produced,
    std::uint32_t lifetime, std::uint64_t now, std::uint64_t admitted) noexcept
{
    if (!lifetime || lifetime > maximum_age_us || now > UINT64_MAX - lifetime
        || produced < admitted || state > Activity::ended
        || (state == Activity::startup && produced)
        || (value_
            && ((state == Activity::startup
                    && value_->state != Activity::startup)
                || now < value_->at_us || produced < value_->produced_next
                || (value_->state == Activity::ended
                    && (state != Activity::ended
                        || produced != value_->produced_next)))))
        return false;
    value_ = ActivityObservation {state, produced, now, now + lifetime};
    return true;
}
std::optional<ActivityObservation> SourceActivity::observe(
    std::uint64_t now, std::uint64_t admitted) const noexcept
{
    if (!value_ || now < value_->at_us || now >= value_->expires_at_us
        || admitted > value_->produced_next)
        return {};
    return value_;
}
bool valid(ReportingConfiguration c) noexcept
{
    return c.interval_us <= maximum_interval_us && c.lifetime_us
        && c.lifetime_us <= maximum_age_us
        && (!c.interval_us || c.interval_us <= c.lifetime_us);
}
std::optional<ReportSample> stream_report(const SourceDataProgress& s,
    const ActivityObservation& activity, const Context& c, Mode mode,
    std::uint64_t serial, ReportingConfiguration configuration,
    std::uint64_t now) noexcept
{
    const auto origin = c.selection.initial_position;
    if (!valid(configuration) || !configuration.interval_us
        || s.stream != c.selection.stream || s.member != c.selection.member
        || s.member_origin != origin || s.source_failed || s.unicast_only
        || s.observed_at_us > now || activity.at_us > now
        || now >= activity.expires_at_us
        || s.admitted_next > activity.produced_next
        || activity.state > Activity::ended)
        return {};
    const auto mc =
        std::max(origin, s.multicast.observed ? s.multicast.next : origin);
    const auto uc =
        std::max(origin, s.unicast.observed ? s.unicast.next : origin);
    const auto observed = std::min(s.observed_at_us, activity.at_us);
    const auto deadline = std::min(
        activity.expires_at_us, add(observed, configuration.lifetime_us));
    if (now >= deadline || std::max(mc, uc) > s.admitted_next)
        return {};
    Message m {.kind = Kind::stream_progress,
        .stream = s.stream,
        .member = s.member,
        .serial = serial,
        .revision = s.revision,
        .next_position = std::max(mc, uc),
        .next_sequence = sequence(c, std::max(mc, uc)),
        .mode = mode,
        .retained_floor = s.history_floor,
        .multicast_next = mc,
        .produced_next = activity.produced_next,
        .activity = activity.state,
        .interval_us = configuration.interval_us};
    if (!valid(m, c))
        return {};
    return ReportSample {m, observed, deadline};
}
std::optional<ReportSample> path_report(const ReceiveDataProgress& s,
    const Context& c, Mode mode, std::uint64_t revision, std::uint64_t serial,
    ReportingConfiguration configuration, std::uint64_t now) noexcept
{
    const auto origin = c.selection.initial_position;
    if (!valid(configuration) || !configuration.interval_us || !s.valid
        || s.origin != origin)
        return {};
    const auto next = s.all.observed ? s.all.next : origin;
    const auto mc = s.multicast.observed ? s.multicast.next : origin;
    Message m {.kind = Kind::path_report,
        .stream = c.selection.stream,
        .member = c.selection.member,
        .serial = serial,
        .revision = revision,
        .next_position = next,
        .next_sequence = sequence(c, next),
        .mode = mode,
        .multicast_next = mc,
        .interval_us = configuration.interval_us};
    if (!valid(m, c))
        return {};
    return ReportSample {m, now, add(now, configuration.lifetime_us)};
}
bool ReportTracker::accept(const Message& m, bool duplicate,
    std::uint64_t first, std::uint64_t now) noexcept
{
    if (now < last_now_)
        clock_failed_ = true;
    last_now_ = std::max(last_now_, now);
    if (clock_failed_ || duplicate || m.kind != kind_ || !valid(m)
        || m.next_position < origin_ || m.multicast_next < origin_
        || first > now
        || (kind_ != Kind::stream_progress && kind_ != Kind::path_report))
        return false;
    if (value_) {
        const auto& old = value_->message;
        if (m.serial <= old.serial || first < value_->first_received_us
            || m.stream != old.stream || m.member != old.member
            || m.revision < old.revision || m.next_position < old.next_position
            || m.multicast_next < old.multicast_next
            || m.retained_floor < old.retained_floor
            || m.produced_next < old.produced_next
            || (m.revision == old.revision && m.mode != old.mode)
            || (kind_ == Kind::stream_progress
                && old.activity == Activity::ended
                && (m.activity != Activity::ended
                    || m.produced_next != old.produced_next)))
            return false;
    }
    ReportObservation report {.message = m,
        .first_received_us = first,
        .next_advanced_at_us = value_ ? value_->next_advanced_at_us : 0,
        .multicast_advanced_at_us =
            value_ ? value_->multicast_advanced_at_us : 0};
    if (m.next_position > (value_ ? value_->message.next_position : origin_))
        report.next_advanced_at_us = first;
    if (m.multicast_next > (value_ ? value_->message.multicast_next : origin_))
        report.multicast_advanced_at_us = first;
    value_ = report;
    return true;
}
std::optional<ReportObservation> ReportTracker::snapshot(
    std::uint64_t now, std::uint32_t maximum_age) const noexcept
{
    if (now < last_now_)
        clock_failed_ = true;
    last_now_ = std::max(last_now_, now);
    if (!value_)
        return {};
    auto result = *value_;
    result.effective_age_us = now < result.first_received_us
        ? UINT64_MAX
        : add(result.message.age_us, now - result.first_received_us);
    result.fresh = !clock_failed_ && maximum_age
        && maximum_age <= maximum_age_us
        && result.effective_age_us < maximum_age;
    return result;
}
}
