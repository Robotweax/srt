#pragma once

#include <chrono>
#include <optional>

namespace robotweax::srt::compat {
struct RuntimePollResult {
    bool immediate_work = false;
    std::optional<std::chrono::microseconds> next_work_delay = std::nullopt;
    bool receive_wait_safe = false;
    bool coarse_timer_probe = false;
    std::optional<std::chrono::steady_clock::time_point> next_work_deadline =
        std::nullopt;
};

} // namespace robotweax::srt::compat
