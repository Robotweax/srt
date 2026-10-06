#pragma once

#include <chrono>
#include <optional>
#include <cstdint>

namespace robotweax::srt::compat {
// Runtime-owned idle receive proof. It never grants native receive credit or
// changes coordinator scheduling; consumers must revalidate with its runtime.
struct RuntimeReceivePollCertificate {
    std::uint64_t runtime_incarnation = 0;
    std::uint64_t state_epoch = 0;
    std::uint64_t ingress_incarnation = 0;
    std::chrono::steady_clock::time_point valid_until;
};

struct RuntimePollResult {
    bool immediate_work = false;
    std::optional<std::chrono::microseconds> next_work_delay = std::nullopt;
    bool receive_wait_safe = false;
    bool coarse_timer_probe = false;
    // A bounded maintenance deadline may accompany an unsafe receive wait.
    // A known deadline alone does not permit native parking or coalescing.
    std::optional<std::chrono::steady_clock::time_point> next_work_deadline =
        std::nullopt;
    // Buffered receive-only maintenance can defer a partial completion wake
    // only behind an already established, no-later channel revisit deadline.
    // This does not permit native receive parking or cover sender/key work.
    bool buffered_completion_wait_safe = false;
    std::optional<RuntimeReceivePollCertificate> receive_certificate =
        std::nullopt;
};

} // namespace robotweax::srt::compat
