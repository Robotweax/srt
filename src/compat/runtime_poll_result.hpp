#pragma once

#include <chrono>
#include <optional>

namespace robotweax::srt::compat {
struct RuntimePollResult {
    bool immediate_work = false;
    std::optional<std::chrono::microseconds> next_work_delay = std::nullopt;
    bool receive_wait_safe = false;
    bool coarse_timer_probe = false;
    // A bounded maintenance deadline may accompany an unsafe receive wait.
    // A known deadline alone does not permit native parking or coalescing.
    std::optional<std::chrono::steady_clock::time_point> next_work_deadline =
        std::nullopt;
};

} // namespace robotweax::srt::compat
