#pragma once

#include "srt/srt.h"
#include <chrono>
#include <optional>

namespace robotweax::srt::compat {

struct CallerPeerRejection {
    SRT_ERRNO error = SRT_ECONNREJ;
    int system_error = 0;
    int reason = SRT_REJ_UNKNOWN;
};

// One owner turn mutates this bounded pending result. Time is supplied by the
// actor so clock-boundary and cancellation behavior can be tested directly.
class CallerRejectionCompletion {
public:
    using Clock = std::chrono::steady_clock;
    explicit CallerRejectionCompletion(Clock::time_point connect_start) noexcept
        : deadline_(connect_start + std::chrono::milliseconds {10})
    {
    }

    void record(CallerPeerRejection rejection) noexcept
    {
        if (!completed_ && !rejection_)
            rejection_ = rejection;
    }
    [[nodiscard]] bool pending() const noexcept
    {
        return rejection_.has_value();
    }
    [[nodiscard]] Clock::time_point deadline() const noexcept
    {
        return deadline_;
    }
    [[nodiscard]] std::optional<CallerPeerRejection> take_if_due(
        Clock::time_point now) noexcept
    {
        if (now < deadline_ || !rejection_)
            return std::nullopt;
        auto result = rejection_;
        completed_ = true;
        rejection_.reset();
        return result;
    }
    void cancel() noexcept
    {
        completed_ = true;
        rejection_.reset();
    }

private:
    bool completed_ = false;
    Clock::time_point deadline_;
    std::optional<CallerPeerRejection> rejection_;
};

} // namespace robotweax::srt::compat
