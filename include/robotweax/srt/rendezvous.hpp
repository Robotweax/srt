#pragma once

#include "robotweax/srt/handshake.hpp"

#include <cstdint>

namespace robotweax::srt {

enum class RendezvousRole : std::uint8_t {
    unresolved,
    initiator,
    responder,
};

enum class RendezvousState : std::uint8_t {
    idle,
    waving,
    attention,
    fine,
    initiated,
    connected,
    rejected,
    failed,
};

// SRT rendezvous cookies are compared as a wrapping 32-bit value by deployed
// implementations. The half-range tie is resolved by signed cookie order so
// both peers still choose opposite roles.
[[nodiscard]] RendezvousRole resolve_rendezvous_role(
    std::uint32_t local_cookie,
    std::uint32_t peer_cookie) noexcept;

class RendezvousHandshakeMachine {
public:
    struct Configuration {
        std::uint32_t local_socket_id = 0;
        std::uint32_t local_cookie = 0;
        SequenceNumber initial_sequence{};
        std::uint32_t maximum_transmission_unit = 1500;
        std::uint32_t flow_window = 25'600;
        std::uint32_t timeout_milliseconds = 250;
        std::uint32_t maximum_retries = 12;
        std::uint16_t encryption_field = 0;
        HandshakeExtensionParameters extension_parameters{};
        // The public compatibility layer supplies SRTO_MINVERSION's default.
        std::uint32_t minimum_peer_srt_version = 0;
        KeyMaterialBuffer key_material_request{};
        StreamId stream_id{};
        CongestionController congestion_controller =
            CongestionController::live;
        PacketFilterConfiguration
            packet_filter_configuration{};
    };

    explicit RendezvousHandshakeMachine(
        Configuration configuration) noexcept;

    [[nodiscard]] RendezvousState state() const noexcept
    {
        return state_;
    }
    [[nodiscard]] RendezvousRole role() const noexcept
    {
        return role_;
    }
    [[nodiscard]] std::uint32_t peer_socket_id() const noexcept
    {
        return peer_socket_id_;
    }
    [[nodiscard]] std::uint32_t peer_cookie() const noexcept
    {
        return peer_cookie_;
    }
    [[nodiscard]] std::uint16_t peer_encryption_field() const noexcept
    {
        return peer_encryption_field_;
    }
    [[nodiscard]] std::uint32_t peer_flow_window() const noexcept
    {
        return peer_flow_window_;
    }
    [[nodiscard]] SequenceNumber peer_initial_sequence() const noexcept
    {
        return peer_initial_sequence_;
    }
    [[nodiscard]] bool has_peer_extension_parameters() const noexcept
    {
        return has_peer_extension_parameters_;
    }
    [[nodiscard]] const HandshakeExtensionParameters&
    peer_extension_parameters() const noexcept
    {
        return peer_extension_parameters_;
    }
    [[nodiscard]] int rejection_reason() const noexcept
    {
        return rejection_reason_;
    }
    [[nodiscard]] bool has_peer_stream_id() const noexcept
    {
        return has_peer_stream_id_;
    }
    [[nodiscard]] const StreamId& peer_stream_id() const noexcept
    {
        return peer_stream_id_;
    }
    [[nodiscard]] bool has_negotiated_packet_filter() const noexcept
    {
        return negotiated_packet_filter_.enabled;
    }
    [[nodiscard]] const PacketFilterConfiguration&
    negotiated_packet_filter() const noexcept
    {
        return negotiated_packet_filter_;
    }

    [[nodiscard]] HandshakeActions start() noexcept;
    [[nodiscard]] Error set_key_material_request(
        const KeyMaterialBuffer& request,
        std::uint16_t encryption_field) noexcept;
    [[nodiscard]] Error set_key_material_response(
        const KeyMaterialBuffer& response) noexcept;
    [[nodiscard]] HandshakeActions receive(
        const Handshake& packet) noexcept;
    [[nodiscard]] HandshakeActions receive(
        const HandshakeMessage& message) noexcept;
    [[nodiscard]] HandshakeActions note_connected_packet() noexcept;
    [[nodiscard]] HandshakeActions timeout() noexcept;

private:
    [[nodiscard]] Handshake base_packet(
        HandshakeRequest request) const noexcept;
    [[nodiscard]] HandshakeExtensionParameters
    response_extension_parameters() const noexcept;
    [[nodiscard]] HandshakeAction wave_action() const noexcept;
    [[nodiscard]] HandshakeAction conclusion_request_action() const noexcept;
    [[nodiscard]] HandshakeAction conclusion_response_action() const noexcept;
    [[nodiscard]] HandshakeAction empty_conclusion_action() const noexcept;
    [[nodiscard]] HandshakeAction agreement_action() const noexcept;
    [[nodiscard]] HandshakeActions reject_cookie_collision() noexcept;
    [[nodiscard]] HandshakeActions reject_message_api() noexcept;
    [[nodiscard]] HandshakeActions reject_peer_version() noexcept;
    [[nodiscard]] HandshakeActions reject_rogue_version() noexcept;
    [[nodiscard]] HandshakeActions
    reject_congestion_controller() noexcept;
    [[nodiscard]] HandshakeActions
    reject_packet_filter() noexcept;
    [[nodiscard]] HandshakeActions fail() noexcept;
    void remember_peer(const HandshakeMessage& message) noexcept;
    void arm_timer(HandshakeActions& actions) const noexcept;

    Configuration configuration_;
    RendezvousState state_ = RendezvousState::idle;
    RendezvousRole role_ = RendezvousRole::unresolved;
    std::uint32_t peer_socket_id_ = 0;
    std::uint32_t peer_cookie_ = 0;
    std::uint32_t peer_flow_window_ = 0;
    std::uint16_t peer_encryption_field_ = 0;
    SequenceNumber peer_initial_sequence_{};
    std::uint32_t retry_count_ = 0;
    HandshakeExtensionParameters peer_extension_parameters_{};
    bool has_peer_extension_parameters_ = false;
    StreamId peer_stream_id_{};
    bool has_peer_stream_id_ = false;
    PacketFilterConfiguration
        negotiated_packet_filter_{};
    KeyMaterialBuffer peer_key_material_request_{};
    bool has_peer_key_material_request_ = false;
    KeyMaterialBuffer key_material_response_{};
    bool has_key_material_response_ = false;
    int rejection_reason_ = 0;
};

} // namespace robotweax::srt
