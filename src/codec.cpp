#include "robotweax/srt/codec.hpp"

#include <algorithm>
#include <array>
#include <cstdint>

namespace robotweax::srt {
namespace {

[[nodiscard]] std::uint32_t read_u32(const std::byte* bytes) noexcept
{
    return (std::to_integer<std::uint32_t>(bytes[0]) << 24U)
        | (std::to_integer<std::uint32_t>(bytes[1]) << 16U)
        | (std::to_integer<std::uint32_t>(bytes[2]) << 8U)
        | std::to_integer<std::uint32_t>(bytes[3]);
}

void write_u32(std::byte* bytes, std::uint32_t value) noexcept
{
    bytes[0] = static_cast<std::byte>((value >> 24U) & 0xffU);
    bytes[1] = static_cast<std::byte>((value >> 16U) & 0xffU);
    bytes[2] = static_cast<std::byte>((value >> 8U) & 0xffU);
    bytes[3] = static_cast<std::byte>(value & 0xffU);
}

} // namespace

DecodeResult decode_packet(std::span<const std::byte> bytes) noexcept
{
    if (bytes.size() < packet_header_size) {
        return {.error = Error::packet_too_short};
    }

    const std::uint32_t word0 = read_u32(bytes.data());
    const std::uint32_t word1 = read_u32(bytes.data() + 4);
    const std::uint32_t timestamp = read_u32(bytes.data() + 8);
    const std::uint32_t socket_id = read_u32(bytes.data() + 12);

    PacketView packet;
    packet.payload = bytes.subspan(packet_header_size);

    if ((word0 & 0x8000'0000U) == 0U) {
        packet.kind = PacketKind::data;
        packet.data.sequence = SequenceNumber{word0};
        packet.data.boundary = static_cast<MessageBoundary>((word1 >> 30U) & 0x3U);
        packet.data.in_order = ((word1 >> 29U) & 0x1U) != 0U;
        packet.data.encryption_key = static_cast<EncryptionKey>((word1 >> 27U) & 0x3U);
        packet.data.retransmitted = ((word1 >> 26U) & 0x1U) != 0U;
        packet.data.message_number = word1 & 0x03ff'ffffU;
        packet.data.timestamp = PacketTimestamp{timestamp};
        packet.data.destination_socket_id = socket_id;
        return {.packet = packet};
    }

    const auto type = static_cast<std::uint16_t>((word0 >> 16U) & 0x7fffU);
    if (!is_known_control_type(type)) {
        return {.error = Error::invalid_control_type};
    }

    if (!has_valid_control_payload_shape(packet.payload)) {
        return {.error = Error::invalid_control_payload};
    }

    packet.kind = PacketKind::control;
    packet.control.type = static_cast<ControlType>(type);
    packet.control.subtype = static_cast<std::uint16_t>(word0 & 0xffffU);
    packet.control.type_specific = word1;
    packet.control.timestamp = PacketTimestamp{timestamp};
    packet.control.destination_socket_id = socket_id;
    return {.packet = packet};
}

EncodeResult encode_packet(
    const MutablePacketView& packet,
    std::span<std::byte> destination) noexcept
{
    const bool add_zero_word = packet.kind == PacketKind::control
        && packet.payload.empty()
        && control_uses_zero_word_payload(packet.control.type);
    const std::size_t payload_size =
        add_zero_word ? sizeof(std::uint32_t) : packet.payload.size();
    const std::size_t required = packet_header_size + payload_size;
    if (destination.size() < required) {
        return {.error = Error::buffer_too_small};
    }

    std::uint32_t word0 = 0;
    std::uint32_t word1 = 0;
    std::uint32_t timestamp = 0;
    std::uint32_t socket_id = 0;

    if (packet.kind == PacketKind::data) {
        word0 = packet.data.sequence.value();
        word1 = (static_cast<std::uint32_t>(packet.data.boundary) << 30U)
            | (static_cast<std::uint32_t>(packet.data.in_order) << 29U)
            | (static_cast<std::uint32_t>(packet.data.encryption_key) << 27U)
            | (static_cast<std::uint32_t>(packet.data.retransmitted) << 26U)
            | (packet.data.message_number & 0x03ff'ffffU);
        timestamp = packet.data.timestamp.value();
        socket_id = packet.data.destination_socket_id;
    } else {
        const auto type = static_cast<std::uint16_t>(packet.control.type);
        if (!is_known_control_type(type)) {
            return {.error = Error::invalid_control_type};
        }
        word0 = 0x8000'0000U
            | (static_cast<std::uint32_t>(type) << 16U)
            | packet.control.subtype;
        word1 = packet.control.type_specific;
        timestamp = packet.control.timestamp.value();
        socket_id = packet.control.destination_socket_id;
    }

    write_u32(destination.data(), word0);
    write_u32(destination.data() + 4, word1);
    write_u32(destination.data() + 8, timestamp);
    write_u32(destination.data() + 12, socket_id);
    std::copy(packet.payload.begin(), packet.payload.end(), destination.begin() + packet_header_size);
    if (add_zero_word) {
        std::fill_n(destination.begin() + packet_header_size,
            sizeof(std::uint32_t), std::byte {0});
    }
    return {.bytes_written = required};
}

ProtectedPayloadDecodeResult decode_protected_payload(
    std::span<const std::byte> protected_payload,
    const CryptoPayloadBudget& budget) noexcept
{
    if (!budget) {
        return {.error = budget.error};
    }
    if (protected_payload.size() < budget.authentication_tag_size
        || protected_payload.size() > budget.maximum_wire_payload_size) {
        return {.error = Error::invalid_payload_size};
    }
    const std::size_t ciphertext_size =
        protected_payload.size() - budget.authentication_tag_size;
    if (ciphertext_size > budget.maximum_plaintext_payload_size) {
        return {.error = Error::invalid_payload_size};
    }
    return {
        .payload =
            {
                .ciphertext = protected_payload.first(ciphertext_size),
                .authentication_tag =
                    protected_payload.subspan(ciphertext_size),
            },
    };
}

ProtectedPayloadEncodeResult prepare_protected_payload(
    std::span<std::byte> destination, std::size_t plaintext_size,
    const CryptoPayloadBudget& budget) noexcept
{
    if (!budget) {
        return {.error = budget.error};
    }
    if (plaintext_size > budget.maximum_plaintext_payload_size) {
        return {.error = Error::invalid_payload_size};
    }
    const std::size_t protected_size =
        plaintext_size + budget.authentication_tag_size;
    if (protected_size > budget.maximum_wire_payload_size) {
        return {.error = Error::invalid_payload_size};
    }
    if (destination.size() < protected_size) {
        return {.error = Error::buffer_too_small};
    }
    return {
        .payload =
            {
                .ciphertext = destination.first(plaintext_size),
                .authentication_tag = destination.subspan(
                    plaintext_size, budget.authentication_tag_size),
            },
        .bytes_written = protected_size,
    };
}

} // namespace robotweax::srt
