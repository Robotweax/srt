#pragma once

#include "compat/caller_handshake_exchange.hpp"

#include <chrono>
#include <cstdint>

namespace robotweax::srt::compat {

enum class CallerHandshakeEventKind : std::uint8_t {
    start,
    inbox_ready,
    retry_timer,
    key_material,
    close,
    overall_timeout,
};

struct CallerHandshakeEvent {
    CallerHandshakeEventKind kind = CallerHandshakeEventKind::start;
    HandshakeMessage message {};
    KeyMaterialBuffer key_material {};
    std::chrono::steady_clock::time_point not_after {};
    std::uint16_t encryption_field = 0;
    bool has_not_after = false;
};

enum class CallerHandshakeEventOutcome : std::uint8_t {
    running,
    connected,
    rejected,
    failed,
    io_error,
    closed,
    timed_out,
    invalid,
};

struct CallerHandshakeEventResult {
    CallerHandshakeEventOutcome outcome = CallerHandshakeEventOutcome::running;
    std::uint32_t retry_timeout_milliseconds = 0;
    int system_error = 0;
    int rejection_reason = 0;
    Error error = Error::none;
    bool arm_retry_timer = false;
};

// Serial, no-wait driver for the complete HSv5 protocol/action boundary. The
// transport owner supplies explicit readiness, timer, key-policy, close, and
// overall-timeout events. The driver never waits on an Inbox or reads a clock.
class CallerHandshakeEventDriver {
public:
    CallerHandshakeEventDriver(HandshakeMachine::Configuration configuration,
        CallerHandshakeSender sender, void* sender_context) noexcept;

    [[nodiscard]] CallerHandshakeEventResult dispatch(
        const CallerHandshakeEvent& event) noexcept;

    [[nodiscard]] HandshakeState state() const noexcept
    {
        return exchange_.state();
    }

    [[nodiscard]] std::uint32_t peer_socket_id() const noexcept
    {
        return exchange_.peer_socket_id();
    }

    [[nodiscard]] const HandshakeMachine& protocol() const noexcept
    {
        return exchange_.protocol();
    }

private:
    [[nodiscard]] CallerHandshakeEventResult translate(
        const CallerHandshakeExchangeResult& result) noexcept;

    CallerHandshakeExchange exchange_;
    bool started_ = false;
    bool terminal_ = false;
};

enum class CallerHandshakeDispatchStatus : std::uint8_t {
    completed,
    full,
    stopped,
    invalid,
    allocation_failed,
};

} // namespace robotweax::srt::compat
