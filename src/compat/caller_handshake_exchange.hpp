#pragma once

#include "compat/caller_handshake_steps.hpp"

#include <cstdint>

namespace robotweax::srt::compat {

enum class CallerHandshakeSendStatus : std::uint8_t {
    sent,
    failed,
    io_error,
};

struct CallerHandshakeSendResult {
    CallerHandshakeSendStatus status = CallerHandshakeSendStatus::sent;
    int system_error = 0;
};

using CallerHandshakeSender = CallerHandshakeSendResult (*)(void* context,
    const HandshakeAction& action,
    std::uint32_t destination_socket_id) noexcept;

enum class CallerHandshakeExchangeOutcome : std::uint8_t {
    running,
    connected,
    rejected,
    failed,
    io_error,
};

struct CallerHandshakeExchangeResult {
    CallerHandshakeExchangeOutcome outcome =
        CallerHandshakeExchangeOutcome::running;
    std::uint32_t retry_timeout_milliseconds = 0;
    int system_error = 0;
    int rejection_reason = 0;
    bool arm_retry_timer = false;
};

// Bounded HSv5 Caller integration step. It owns protocol progression and
// executes the resulting fixed-size action list through an injected noexcept
// sender. It never waits, reads an Inbox, consults a clock, allocates, or
// publishes socket state; those lifecycle concerns remain with the external
// driver until the complete path can move onto a scheduler shard. One external
// driver must serialize all calls for a given exchange.
class CallerHandshakeExchange {
public:
    CallerHandshakeExchange(HandshakeMachine::Configuration configuration,
        CallerHandshakeSender sender, void* sender_context) noexcept;

    [[nodiscard]] CallerHandshakeExchangeResult start() noexcept;
    [[nodiscard]] CallerHandshakeExchangeResult receive(
        const HandshakeMessage& message) noexcept;
    [[nodiscard]] CallerHandshakeExchangeResult timeout() noexcept;
    [[nodiscard]] Error set_key_material_request(
        const KeyMaterialBuffer& request,
        std::uint16_t encryption_field) noexcept;

    [[nodiscard]] HandshakeState state() const noexcept
    {
        return steps_.state();
    }

    [[nodiscard]] std::uint32_t peer_socket_id() const noexcept
    {
        return steps_.peer_socket_id();
    }

    [[nodiscard]] const HandshakeMachine& protocol() const noexcept
    {
        return steps_.protocol();
    }

private:
    [[nodiscard]] CallerHandshakeExchangeResult execute(
        const CallerHandshakeStep& step) noexcept;

    CallerHandshakeSteps steps_;
    CallerHandshakeSender sender_ = nullptr;
    void* sender_context_ = nullptr;
};

} // namespace robotweax::srt::compat
