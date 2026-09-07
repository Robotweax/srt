#pragma once

#include "robotweax/srt/error.hpp"
#include "robotweax/srt/crypto.hpp"
#include "robotweax/srt/connection_group.hpp"
#include "robotweax/srt/handshake_extensions.hpp"
#include "robotweax/srt/packet_filter.hpp"
#include "robotweax/srt/sequence.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace robotweax::srt {

inline constexpr std::size_t handshake_size = 48;
inline constexpr std::uint32_t handshake_version_4 = 4;
inline constexpr std::uint32_t handshake_version_5 = 5;
inline constexpr std::uint16_t handshake_srt_magic = 0x4a17;
inline constexpr std::uint16_t handshake_extension_flag_handshake = 0x0001;
inline constexpr std::uint16_t handshake_extension_flag_key_material = 0x0002;
inline constexpr std::uint16_t handshake_extension_flag_config = 0x0004;

enum class HandshakeRequest : std::int32_t {
    conclusion = -1,
    agreement = -2,
    done = -3,
    wave_a_hand = 0,
    induction = 1,
};

struct Handshake {
    std::uint32_t version = handshake_version_5;
    std::uint16_t encryption_field = 0;
    std::uint16_t extension_field = 0;
    SequenceNumber initial_sequence{};
    std::uint32_t maximum_transmission_unit = 1500;
    std::uint32_t flow_window = 25'600;
    HandshakeRequest request = HandshakeRequest::induction;
    std::uint32_t socket_id = 0;
    std::uint32_t syn_cookie = 0;
    std::array<std::uint32_t, 4> peer_address{};
};

struct HandshakeDecodeResult {
    Error error = Error::none;
    Handshake handshake{};
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return error == Error::none; }
};

struct HandshakePayloadDecodeResult {
    Error error = Error::none;
    Handshake handshake{};
    // Borrows the suffix of the payload passed to decode_handshake_payload().
    std::span<const std::byte> extensions{};

    [[nodiscard]] constexpr explicit operator bool() const noexcept
    {
        return error == Error::none;
    }
};

struct HandshakeEncodeResult {
    Error error = Error::none;
    std::size_t bytes_written = 0;

    [[nodiscard]] constexpr explicit operator bool() const noexcept
    {
        return error == Error::none;
    }
};

/** Decodes the fixed handshake prefix; no input pointer is retained. */
[[nodiscard]] HandshakeDecodeResult decode_handshake(std::span<const std::byte> payload) noexcept;
/**
 * Decodes a handshake and returns an extension span that aliases `payload`.
 * The span remains valid only while the input remains alive and unchanged.
 */
[[nodiscard]] HandshakePayloadDecodeResult decode_handshake_payload(
    std::span<const std::byte> payload) noexcept;
[[nodiscard]] Error encode_handshake(const Handshake& handshake, std::span<std::byte> destination) noexcept;
[[nodiscard]] HandshakeEncodeResult encode_handshake_payload(
    const Handshake& handshake,
    std::span<const std::byte> extensions,
    std::span<std::byte> destination) noexcept;

enum class ConnectionRole : std::uint8_t { caller, listener };
enum class HandshakeState : std::uint8_t {
    idle,
    awaiting_induction_response,
    awaiting_conclusion_response,
    connected,
    rejected,
    failed,
};

enum class HandshakeActionKind : std::uint8_t {
    none,
    send,
    connected,
    rejected,
    arm_timer,
    failed,
};

struct HandshakeAction {
    HandshakeActionKind kind = HandshakeActionKind::none;
    Handshake packet{};
    bool has_handshake_extension = false;
    HandshakeExtensionType extension_type = HandshakeExtensionType::handshake_request;
    HandshakeExtensionParameters extension_parameters{};
    bool has_key_material_extension = false;
    HandshakeExtensionType key_material_extension_type =
        HandshakeExtensionType::key_material_request;
    KeyMaterialBuffer key_material{};
    bool has_stream_id_extension = false;
    StreamId stream_id{};
    bool has_congestion_extension = false;
    CongestionController congestion_controller =
        CongestionController::live;
    bool has_packet_filter_extension = false;
    PacketFilterConfiguration
        packet_filter_configuration{};
    bool has_group_membership = false;
    GroupMembership group_membership{};
    std::uint32_t timeout_milliseconds = 0;
    int rejection_reason = 0;
};

struct HandshakeActions {
    std::array<HandshakeAction, 3> values{};
    std::size_t size = 0;

    constexpr void push(HandshakeAction action) noexcept
    {
        if (size < values.size()) {
            values[size++] = action;
        }
    }
};

// The network integration owns cookie policy because a production cookie must
// incorporate the peer address, a rotating secret, and a bounded time window.
// A plain function pointer keeps this boundary allocation-free. The callback
// and context are borrowed by HandshakeMachine and must outlive it.
using CookieGenerator = std::uint32_t (*)(const Handshake&, void* context) noexcept;

struct HandshakeMessage {
    Handshake packet{};
    bool has_handshake_extension = false;
    HandshakeExtensionType extension_type = HandshakeExtensionType::handshake_request;
    HandshakeExtensionParameters extension_parameters{};
    bool has_key_material_extension = false;
    HandshakeExtensionType key_material_extension_type =
        HandshakeExtensionType::key_material_request;
    KeyMaterialBuffer key_material{};
    bool has_stream_id_extension = false;
    StreamId stream_id{};
    bool has_congestion_extension = false;
    CongestionController congestion_controller =
        CongestionController::live;
    bool has_packet_filter_extension = false;
    PacketFilterConfiguration
        packet_filter_configuration{};
    bool has_group_membership = false;
    GroupMembership group_membership{};
    bool has_unknown_extension = false;
};

using GroupMembershipNegotiator = bool (*)(
    const GroupMembership& peer,
    GroupMembership& local,
    void* context) noexcept;

/**
 * Allocation-free protocol state machine. Configuration and callback pointers
 * are copied at construction; callback contexts remain caller-owned. One
 * instance is not internally synchronized and must be externally serialized.
 */
class HandshakeMachine {
public:
    struct Configuration {
        ConnectionRole role = ConnectionRole::caller;
        std::uint32_t local_socket_id = 0;
        SequenceNumber initial_sequence{};
        std::uint32_t maximum_transmission_unit = 1500;
        std::uint32_t flow_window = 25'600;
        std::uint32_t timeout_milliseconds = 3'000;
        std::uint32_t maximum_retries = 8;
        std::uint16_t encryption_field = 0;
        HandshakeExtensionParameters extension_parameters{};
        // The protocol engine stays policy-neutral. The compatibility API
        // supplies SRTO_MINVERSION's public default (1.0.0).
        std::uint32_t minimum_peer_srt_version = 0;
        KeyMaterialBuffer key_material_request{};
        StreamId stream_id{};
        CongestionController congestion_controller =
            CongestionController::live;
        PacketFilterConfiguration
            packet_filter_configuration{};
        bool has_group_membership = false;
        GroupMembership group_membership{};
        GroupMembershipNegotiator group_membership_negotiator = nullptr;
        void* group_membership_context = nullptr;
        CookieGenerator cookie_generator = nullptr;
        void* cookie_context = nullptr;
    };

    explicit HandshakeMachine(Configuration configuration) noexcept;

    [[nodiscard]] HandshakeState state() const noexcept { return state_; }
    // A caller that has advanced to CONCLUSION must not renegotiate from a
    // delayed INDUCTION. Transport owners use this before crypto policy too.
    [[nodiscard]] bool is_stale_induction(
        const HandshakeMessage& message) const noexcept;
    [[nodiscard]] bool has_peer_extension_parameters() const noexcept
    {
        return has_peer_extension_parameters_;
    }
    [[nodiscard]] const HandshakeExtensionParameters& peer_extension_parameters() const noexcept
    {
        return peer_extension_parameters_;
    }
    [[nodiscard]] bool has_peer_stream_id() const noexcept
    {
        return has_peer_stream_id_;
    }
    [[nodiscard]] const StreamId& peer_stream_id() const noexcept
    {
        return peer_stream_id_;
    }
    [[nodiscard]] std::uint32_t peer_flow_window() const noexcept
    {
        return peer_flow_window_;
    }
    [[nodiscard]] int rejection_reason() const noexcept
    {
        return rejection_reason_;
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
    [[nodiscard]] bool has_peer_group_membership() const noexcept
    {
        return has_peer_group_membership_;
    }
    [[nodiscard]] const GroupMembership&
    peer_group_membership() const noexcept
    {
        return peer_group_membership_;
    }
    [[nodiscard]] HandshakeActions start() noexcept;
    [[nodiscard]] Error set_key_material_request(
        const KeyMaterialBuffer& request,
        std::uint16_t encryption_field) noexcept;
    [[nodiscard]] HandshakeActions reject(
        int rejection_reason) noexcept;
    [[nodiscard]] HandshakeActions receive(const Handshake& packet) noexcept;
    [[nodiscard]] HandshakeActions receive(const HandshakeMessage& message) noexcept;
    [[nodiscard]] HandshakeActions timeout() noexcept;

private:
    [[nodiscard]] Handshake base_packet(HandshakeRequest request) const noexcept;
    [[nodiscard]] HandshakeExtensionParameters response_extension_parameters() const noexcept;
    [[nodiscard]] HandshakeActions reject_locally(
        int rejection_reason) noexcept;

    Configuration configuration_;
    HandshakeState state_ = HandshakeState::idle;
    std::uint32_t peer_socket_id_ = 0;
    std::uint32_t peer_flow_window_ = 0;
    std::uint32_t cookie_ = 0;
    std::uint32_t retry_count_ = 0;
    SequenceNumber negotiated_initial_sequence_{};
    HandshakeExtensionParameters peer_extension_parameters_{};
    bool has_peer_extension_parameters_ = false;
    StreamId peer_stream_id_{};
    bool has_peer_stream_id_ = false;
    PacketFilterConfiguration
        negotiated_packet_filter_{};
    GroupMembership peer_group_membership_{};
    bool has_peer_group_membership_ = false;
    GroupMembership local_group_response_{};
    bool has_local_group_response_ = false;
    int rejection_reason_ = 0;
};

} // namespace robotweax::srt
