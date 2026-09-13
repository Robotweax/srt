#pragma once

#include <chrono>
#include <cstdint>

namespace robotweax::srt::compat {

// One ready-queue continuation may omit a full poll. This hint never schedules
// a timer, waits, sends, or renews itself. The next continuation must poll.
class PacedPollContinuation {
public:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;
    static constexpr auto maximum_deferral = std::chrono::microseconds {5};

    void arm(Time pacing_deadline, Time now, std::uint64_t epoch) noexcept
    {
        pending_ = false;
        if (pacing_deadline <= now || now >= Time::max() - maximum_deferral) {
            return;
        }
        since_ = now;
        until_ = pacing_deadline < now + maximum_deferral
            ? pacing_deadline
            : now + maximum_deferral;
        epoch_ = epoch;
        pending_ = true;
    }

    [[nodiscard]] bool pending() const noexcept
    {
        return pending_;
    }

    [[nodiscard]] bool take(Time now, std::uint64_t epoch) noexcept
    {
        const bool skip =
            pending_ && epoch == epoch_ && now >= since_ && now < until_;
        pending_ = false;
        return skip;
    }

private:
    Time since_ {};
    Time until_ {};
    std::uint64_t epoch_ = 0;
    bool pending_ = false;
};

} // namespace robotweax::srt::compat
