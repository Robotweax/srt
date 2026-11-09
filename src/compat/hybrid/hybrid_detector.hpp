// SPDX-License-Identifier: MIT
#pragma once
#include "compat/hybrid/hybrid_reporting.hpp"

namespace robotweax::srt::compat::hybrid {
struct DetectionConfiguration {
    // No production defaults. All times are positive, <=5s, in one host
    // monotonic domain. Transit is an assumed bound, not a network measurement.
    std::uint32_t report_age_us = 0, maximum_transit_us = 0, stall_us = 0,
                  emission_lifetime_us = 0, recovery_us = 0,
                  recovery_gap_us = 0, hold_down_us = 0, sample_gap_us = 0,
                  recovery_advances = 0;
};
[[nodiscard]] bool valid(DetectionConfiguration) noexcept;
// Passive assessments only. No cursor, ACK, policy command, network I/O or
// automatic switching. Host serializes observe/snapshot; fixed storage <2KiB.
class PathDetector {
public:
    enum class State {
        unknown,
        observing,
        startup,
        paused,
        ended,
        source_failed,
        awaiting_emission,
        multicast_progress,
        multicast_suspect,
        multicast_stalled,
        all_data_stalled,
        recovering,
        multicast_ready
    };
    enum class Reason {
        none,
        configuration,
        lifetime,
        clock,
        sampling_gap,
        no_report,
        stale_report,
        no_data,
        invalid_evidence,
        policy,
        assessment_expired
    };
    struct Policy {
        bool confirmed = false;
        Mode mode = Mode::multicast;
        std::uint64_t revision = 0, expires_at_us = 0;
    };
    struct Input {
        std::uint64_t now_us = 0, generation = 0;
        std::optional<ReportObservation> source;
        std::optional<ReceiveDataProgress> data;
        Policy policy;
    };
    struct Assessment {
        State state = State::unknown;
        Reason reason = Reason::none;
        bool multicast_stalled = false, all_data_stalled = false,
             fallback_candidate = false, return_candidate = false;
        std::uint64_t observed_at_us = 0, valid_until_us = 0,
                      next_wakeup_us = UINT64_MAX, revision = 0,
                      source_next = 0, source_multicast_next = 0,
                      received_next = 0, received_multicast_next = 0;
        std::uint32_t recovery_data_advances = 0, recovery_source_advances = 0;
    };
    PathDetector(
        DetectionConfiguration, Context, std::uint64_t generation) noexcept;
    void observe(const Input&) noexcept;
    [[nodiscard]] Assessment snapshot(std::uint64_t now_us) const noexcept;

private:
    void reset_evidence() noexcept;
    void unknown(Reason, std::uint64_t) noexcept;
    DetectionConfiguration configuration_;
    Context context_;
    std::uint64_t generation_;
    Assessment state_;
    std::optional<Message> report_;
    std::optional<ReceiveDataProgress> data_;
    Policy policy_;
    mutable std::uint64_t last_now_ = 0;
    mutable bool clock_failed_ = false;
    bool lifetime_failed_ = false, sampled_ = false, baseline_ = false,
         mc_gap_ = false, all_gap_ = false, outage_ = false, recovery_ = false,
         previous_stall_ = false;
    std::uint64_t last_report_received_ = 0, last_sample_ = 0,
                  mc_emission_until_ = 0, all_emission_until_ = 0,
                  mc_gap_since_ = 0, all_gap_since_ = 0, hold_until_ = 0,
                  recovery_start_ = 0, recovery_last_ = 0;
    std::uint32_t recovery_data_ = 0, recovery_source_ = 0;
};
}
