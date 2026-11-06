// SPDX-License-Identifier: MIT
#pragma once
#include "compat/hybrid_progress.hpp"
#include "compat/hybrid_control.hpp"

namespace robotweax::srt::compat::hybrid {
class SourceActivity {
public:
    [[nodiscard]] bool declare(Activity, std::uint64_t produced_next,
        std::uint32_t lifetime_us, std::uint64_t now,
        std::uint64_t admitted_next) noexcept;
    [[nodiscard]] std::optional<ActivityObservation> observe(
        std::uint64_t now, std::uint64_t admitted_next) const noexcept;

private:
    std::optional<ActivityObservation> value_;
};
struct ReportingConfiguration {
    // Zero disables production; nonzero is explicit host policy, not a detector
    // threshold. No default reporting or automatic transition is enabled.
    std::uint32_t interval_us = 0, lifetime_us = 500'000;
};
[[nodiscard]] bool valid(ReportingConfiguration) noexcept;
struct ReportSample {
    Message message;
    std::uint64_t observed_at_us = 0, not_after_us = 0;
};
[[nodiscard]] std::optional<ReportSample> stream_report(
    const SourceDataProgress&, const ActivityObservation&, const Context&, Mode,
    std::uint64_t serial, ReportingConfiguration, std::uint64_t now) noexcept;
[[nodiscard]] std::optional<ReportSample> path_report(
    const ReceiveDataProgress&, const Context&, Mode, std::uint64_t revision,
    std::uint64_t serial, ReportingConfiguration, std::uint64_t now) noexcept;
struct ReportObservation {
    Message message;
    std::uint64_t first_received_us = 0, effective_age_us = 0,
                  next_advanced_at_us = 0, multicast_advanced_at_us = 0;
    bool fresh = false;
};
// One authenticated report per peer, no history/queue allocation. Metadata
// freshness can renew without DATA progress; advancement timestamps cannot.
class ReportTracker {
public:
    explicit ReportTracker(
        Kind kind = Kind::stream_progress, std::uint64_t origin = 0) noexcept
        : kind_(kind)
        , origin_(origin)
    {
    }
    [[nodiscard]] bool accept(const Message&, bool duplicate,
        std::uint64_t first_received_us, std::uint64_t now) noexcept;
    [[nodiscard]] std::optional<ReportObservation> snapshot(
        std::uint64_t now, std::uint32_t maximum_age) const noexcept;

private:
    Kind kind_;
    std::uint64_t origin_;
    std::optional<ReportObservation> value_;
    mutable std::uint64_t last_now_ = 0;
    mutable bool clock_failed_ = false;
};
}
