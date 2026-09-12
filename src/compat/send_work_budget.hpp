#pragma once

#include <chrono>
#include <cstddef>

namespace robotweax::srt::compat {

// Shared by every connection in one channel callback. The clock starts at
// the first send attempt, after control/receive housekeeping. This is an
// admission bound: a syscall or a descheduled thread can finish after it.
class SendWorkBudget {
public:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;
    static constexpr std::size_t packet_limit = 64;
    static constexpr auto time_limit = std::chrono::microseconds {50};
    static constexpr auto bridge_limit = std::chrono::microseconds {5};

    [[nodiscard]] bool admit(Time now) noexcept
    {
        if (!started_) {
            deadline_ = now + time_limit;
            started_ = true;
        }
        return packets_ < packet_limit && now < deadline_;
    }

    void sent() noexcept
    {
        ++packets_;
    }

    template <class Now>
    [[nodiscard]] bool bridge(Time target, Now now) noexcept
    {
        auto current = now();
        if (!admit(current) || target >= deadline_
            || (target > current && target - current > bridge_limit)) {
            return false;
        }
        // No yielding syscall for a sub-5us gap. Recheck the total budget
        // after every clock read, including when preempted during the bridge.
        while (current < target) {
            current = now();
            if (current >= deadline_) {
                return false;
            }
        }
        return true;
    }

private:
    Time deadline_ {};
    std::size_t packets_ = 0;
    bool started_ = false;
};

} // namespace robotweax::srt::compat
