#pragma once

#include "robotweax/srt/sequence.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace robotweax::srt {

inline constexpr std::size_t packet_header_size = 16;
// Maximum SRT DATA payload carried after the 16-byte SRT header. This is a
// protected-wire ceiling: authenticated modes must reserve their complete tag
// inside it rather than increasing the datagram size.
inline constexpr std::size_t maximum_data_payload_size = 1456;

enum class PacketKind : std::uint8_t { data, control };

enum class MessageBoundary : std::uint8_t {
    subsequent = 0,
    last = 1,
    first = 2,
    solo = 3,
};

enum class EncryptionKey : std::uint8_t {
    none = 0,
    even = 1,
    odd = 2,
    reserved = 3,
};

enum class ControlType : std::uint16_t {
    handshake = 0x0000,
    keepalive = 0x0001,
    acknowledgement = 0x0002,
    negative_acknowledgement = 0x0003,
    congestion_warning = 0x0004,
    shutdown = 0x0005,
    acknowledgement_of_ack = 0x0006,
    drop_request = 0x0007,
    peer_error = 0x0008,
    user_defined = 0x7fff,
};

struct DataHeader {
    SequenceNumber sequence;
    std::uint32_t message_number = 0;
    MessageBoundary boundary = MessageBoundary::subsequent;
    bool in_order = false;
    EncryptionKey encryption_key = EncryptionKey::none;
    bool retransmitted = false;
    // Connection-relative microseconds modulo 2^32.
    PacketTimestamp timestamp;
    std::uint32_t destination_socket_id = 0;
};

struct ControlHeader {
    ControlType type = ControlType::handshake;
    std::uint16_t subtype = 0;
    std::uint32_t type_specific = 0;
    // Connection-relative microseconds modulo 2^32.
    PacketTimestamp timestamp;
    std::uint32_t destination_socket_id = 0;
};

/**
 * Non-owning decoded packet descriptor. `payload` aliases the packet bytes
 * supplied to the decoder and is valid only while those bytes remain alive and
 * unchanged. The value is safe to copy, but copying does not extend lifetime.
 */
struct PacketView {
    PacketKind kind = PacketKind::data;
    DataHeader data{};
    ControlHeader control{};
    std::span<const std::byte> payload{};
};

/**
 * Non-owning packet description consumed by encode_packet(). Header values are
 * copied and `payload` is read only during encoding; no input pointer is
 * retained after the call.
 */
struct MutablePacketView {
    PacketKind kind = PacketKind::data;
    DataHeader data{};
    ControlHeader control{};
    std::span<const std::byte> payload{};
};

// SRT control payloads are arrays of 32-bit network-order words. Haivision
// SRT 1.5.7 rejects both an empty control payload and a trailing partial word.
[[nodiscard]] constexpr bool has_valid_control_payload_shape(
    std::span<const std::byte> payload) noexcept
{
    return !payload.empty() && payload.size() % sizeof(std::uint32_t) == 0U;
}

// These controls have no semantic payload, but still carry one zero word on
// the wire. Keeping the list here makes future producers share one canonical
// representation instead of reimplementing writev padding independently.
[[nodiscard]] constexpr bool control_uses_zero_word_payload(
    ControlType type) noexcept
{
    switch (type) {
    case ControlType::keepalive:
    case ControlType::congestion_warning:
    case ControlType::shutdown:
    case ControlType::acknowledgement_of_ack:
    case ControlType::peer_error:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] constexpr bool is_zero_word_payload(
    std::span<const std::byte> payload) noexcept
{
    if (payload.size() != sizeof(std::uint32_t)) {
        return false;
    }
    for (const std::byte value : payload) {
        if (value != std::byte {0}) {
            return false;
        }
    }
    return true;
}

} // namespace robotweax::srt
