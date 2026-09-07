#pragma once

#include "robotweax/srt/handshake.hpp"

#include <cstdint>

namespace robotweax::srt::compat {

// A conforming listener advertises its socket ID in the induction response.
// Haivision SRT 1.5.x instead echoes the caller's ID and expects the
// conclusion on the listener's well-known handshake route (socket ID zero).
[[nodiscard]] constexpr std::uint32_t caller_conclusion_destination(
    std::uint32_t local_socket_id,
    std::uint32_t induction_response_socket_id) noexcept
{
    return induction_response_socket_id == 0U
            || induction_response_socket_id == local_socket_id
        ? 0U
        : induction_response_socket_id;
}

struct CallerHandshakeStep {
    HandshakeActions actions {};
    std::uint32_t destination_socket_id = 0;
};

// I/O-free HSv5 Caller coordinator. Each entry point consumes exactly one
// protocol event and returns the complete bounded action set for the external
// driver. The current connection worker remains that driver; a later runtime
// scheduler can submit these same steps without changing handshake semantics.
class CallerHandshakeSteps {
public:
    explicit CallerHandshakeSteps(
        HandshakeMachine::Configuration configuration) noexcept;

    [[nodiscard]] CallerHandshakeStep start() noexcept;
    [[nodiscard]] CallerHandshakeStep receive(
        const HandshakeMessage& message) noexcept;
    [[nodiscard]] CallerHandshakeStep timeout() noexcept;
    [[nodiscard]] Error set_key_material_request(
        const KeyMaterialBuffer& request,
        std::uint16_t encryption_field) noexcept;

    [[nodiscard]] HandshakeState state() const noexcept
    {
        return machine_.state();
    }

    [[nodiscard]] std::uint32_t peer_socket_id() const noexcept
    {
        return peer_socket_id_;
    }

    [[nodiscard]] std::uint32_t destination_socket_id() const noexcept
    {
        return destination_socket_id_;
    }

    [[nodiscard]] const HandshakeMachine& protocol() const noexcept
    {
        return machine_;
    }

private:
    [[nodiscard]] CallerHandshakeStep step(
        HandshakeActions actions) const noexcept;

    HandshakeMachine machine_;
    std::uint32_t local_socket_id_ = 0;
    std::uint32_t peer_socket_id_ = 0;
    std::uint32_t destination_socket_id_ = 0;
};

} // namespace robotweax::srt::compat
