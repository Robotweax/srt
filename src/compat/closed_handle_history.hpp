// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Robotweax GmbH and contributors
#pragma once

#include "srt/srt.h"

#include <algorithm>
#include <array>
#include <cstddef>

namespace robotweax::srt::compat {

// Registry-lock protected, allocation-free status history, not ownership.
// Expired handles remain invalid; allocators never recycle their numeric IDs.
class ClosedHandleHistory {
public:
    static constexpr std::size_t capacity = 4096;

    void remember(SRTSOCKET handle) noexcept
    {
        handles_[next_] = handle;
        next_ = (next_ + 1U) % capacity;
    }

    [[nodiscard]] bool contains(SRTSOCKET handle) const noexcept
    {
        return handle > 0
            && std::find(handles_.begin(), handles_.end(), handle)
            != handles_.end();
    }

    void clear() noexcept
    {
        handles_.fill(0);
        next_ = 0;
    }

private:
    std::array<SRTSOCKET, capacity> handles_ {};
    std::size_t next_ = 0;
};

// Fail closed at exhaustion rather than aliasing stale handles (including
// epoll subscriptions and in-flight operations) after numeric wraparound.
[[nodiscard]] constexpr SRTSOCKET next_registry_handle(
    SRTSOCKET current) noexcept
{
    return current <= 0 || current >= SRTGROUP_MASK - 1 ? SRT_INVALID_SOCK
                                                        : current + 1;
}

} // namespace robotweax::srt::compat
