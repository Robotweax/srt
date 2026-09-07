#pragma once

#include "compat/listener_handshake_sources.hpp"

#include <cstdint>

namespace robotweax::srt::compat {

enum class ListenerConnectionSetupState : std::uint8_t {
    idle,
    awaiting_admission,
    running,
    connected,
    rejected,
    failed,
    closed,
    timed_out,
};

enum class ListenerConnectionSetupEventKind : std::uint8_t {
    start,
    admit,
    handshake,
    retry_timer,
    reject,
    close,
    overall_timeout,
};

struct ListenerConnectionSetupEvent {
    ListenerConnectionSetupEventKind kind =
        ListenerConnectionSetupEventKind::start;
    HandshakeMessage handshake {};
    int rejection_reason = 0;
    bool has_handshake = false;
};

enum class ListenerConnectionSetupOutcome : std::uint8_t {
    running,
    connected,
    rejected,
    failed,
    closed,
    timed_out,
    invalid,
};

struct ListenerConnectionSetupResult {
    ListenerConnectionSetupOutcome outcome =
        ListenerConnectionSetupOutcome::running;
    ListenerHandshakeProtocol protocol = ListenerHandshakeProtocol::invalid;
    HandshakeActions hsv5_actions {};
    Error error = Error::none;
    int rejection_reason = 0;
};

// Immutable publication input captured only after one protocol generation has
// reached connected. It lets the off-shard owner stage socket/runtime state
// without retaining a pointer into the affinity-owned protocol machine.
struct ListenerConnectionSetupRuntimeState {
    Error error = Error::invalid_state;
    HandshakeExtensionParameters peer_extension_parameters {};
    PacketFilterConfiguration packet_filter {};
    StreamId peer_stream_id {};
    GroupMembership peer_group_membership {};
    std::uint32_t peer_flow_window = 0;
    bool has_peer_extension_parameters = false;
    bool has_peer_stream_id = false;
    bool has_peer_group_membership = false;

    [[nodiscard]] constexpr explicit operator bool() const noexcept
    {
        return error == Error::none;
    }
};

// Serial, allocation-free post-admission protocol boundary for a Listener.
// start() reconstructs the stateless discovery state but deliberately emits no
// network work. This leaves application callback, option refresh, and crypto
// policy outside the protocol driver. admit() then consumes the already
// validated CONCLUSION. Every later call consumes exactly one explicit
// message, timer, rejection, close, or overall-timeout event and returns the
// complete fixed-size HSv5 action set. The caller owns all I/O, clocks,
// socket publication, callbacks, and serialization.
class ListenerConnectionSetupSteps {
public:
    struct Configuration {
        HandshakeMachine::Configuration hsv5 {};
    };

    ListenerConnectionSetupSteps(ListenerHandshakeAdmission admission,
        Configuration configuration) noexcept;

    [[nodiscard]] ListenerConnectionSetupResult dispatch(
        const ListenerConnectionSetupEvent& event) noexcept;

    // A Listener callback may change accepted-socket options after stateless
    // validation but before the admitted CONCLUSION is consumed. Rebuild the
    // selected protocol machine while preserving that explicit policy gap.
    // No wire action is emitted and the method is valid only after start().
    [[nodiscard]] bool reconfigure(Configuration configuration) noexcept;

    [[nodiscard]] ListenerConnectionSetupState state() const noexcept
    {
        return state_;
    }

    [[nodiscard]] ListenerHandshakeProtocol protocol() const noexcept
    {
        return admission_.protocol;
    }

    [[nodiscard]] const HandshakeMachine& hsv5_protocol() const noexcept
    {
        return hsv5_;
    }

    [[nodiscard]] ListenerConnectionSetupRuntimeState
    runtime_state() const noexcept;

private:
    [[nodiscard]] ListenerConnectionSetupResult start() noexcept;
    [[nodiscard]] ListenerConnectionSetupResult admit(
        const HandshakeMessage* message) noexcept;
    [[nodiscard]] ListenerConnectionSetupResult receive_handshake(
        const HandshakeMessage& message) noexcept;
    [[nodiscard]] ListenerConnectionSetupResult timeout() noexcept;
    [[nodiscard]] ListenerConnectionSetupResult reject(
        int rejection_reason) noexcept;
    [[nodiscard]] ListenerConnectionSetupResult terminal(
        ListenerConnectionSetupState state,
        ListenerConnectionSetupOutcome outcome) noexcept;
    [[nodiscard]] ListenerConnectionSetupResult invalid() const noexcept;
    [[nodiscard]] ListenerConnectionSetupResult translate(
        HandshakeActions actions) noexcept;

    ListenerHandshakeAdmission admission_ {};
    HandshakeMachine hsv5_;
    ListenerConnectionSetupState state_ = ListenerConnectionSetupState::idle;
};

} // namespace robotweax::srt::compat
