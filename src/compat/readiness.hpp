#pragma once

#include "srt/srt.h"

#include <chrono>
#include <cstdint>
#include <optional>

namespace robotweax::srt::compat {

class ConnectionRuntime;

struct SocketReadinessSnapshot {
    int events = 0;
    std::uint64_t update_version = 0;
    std::optional<std::chrono::steady_clock::time_point> read_wakeup;
    bool exists = false;
};

[[nodiscard]] SocketReadinessSnapshot socket_readiness(
    SRTSOCKET socket) noexcept;

// The socket flag is a snapshot; the runtime may become terminal afterwards.
// Kept separate from registry lookup so both snapshot orderings are testable.
[[nodiscard]] SocketReadinessSnapshot connection_readiness(
    ConnectionRuntime& runtime, bool socket_broken) noexcept;

class ReadinessSignal {
public:
    using Clock = std::chrono::steady_clock;

    [[nodiscard]] static std::uint64_t generation() noexcept;
    static void notify() noexcept;
    static void wait_until(
        std::uint64_t observed_generation,
        Clock::time_point deadline) noexcept;
};

} // namespace robotweax::srt::compat
