#include "test.hpp"

#include "robotweax/srt/codec.hpp"
#include "robotweax_srt.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

using namespace robotweax::srt;

TEST(data_packet_round_trip)
{
    const std::array payload{std::byte{0x47}, std::byte{0x01}, std::byte{0x02}};
    MutablePacketView source;
    source.kind = PacketKind::data;
    source.data.sequence = SequenceNumber{0x1234'5678U};
    source.data.message_number = 0x0123'4567U;
    source.data.boundary = MessageBoundary::solo;
    source.data.in_order = true;
    source.data.encryption_key = EncryptionKey::odd;
    source.data.retransmitted = true;
    source.data.timestamp = PacketTimestamp{0xaabb'ccddU};
    source.data.destination_socket_id = 0x1020'3040U;
    source.payload = payload;

    std::array<std::byte, 64> storage{};
    const auto encoded = encode_packet(source, storage);
    REQUIRE(encoded);
    REQUIRE_EQ(encoded.bytes_written, packet_header_size + payload.size());

    const auto decoded = decode_packet(std::span{storage}.first(encoded.bytes_written));
    REQUIRE(decoded);
    REQUIRE_EQ(decoded.packet.kind, PacketKind::data);
    REQUIRE_EQ(decoded.packet.data.sequence, source.data.sequence);
    REQUIRE_EQ(decoded.packet.data.message_number, source.data.message_number);
    REQUIRE_EQ(decoded.packet.data.boundary, MessageBoundary::solo);
    REQUIRE(decoded.packet.data.in_order);
    REQUIRE(decoded.packet.data.retransmitted);
    REQUIRE_EQ(decoded.packet.data.encryption_key, EncryptionKey::odd);
    REQUIRE_EQ(decoded.packet.payload.size(), payload.size());
}

TEST(control_packet_has_exact_wire_layout)
{
    MutablePacketView source;
    source.kind = PacketKind::control;
    source.control.type = ControlType::acknowledgement;
    source.control.subtype = 0x1234U;
    source.control.type_specific = 0x0102'0304U;
    source.control.timestamp = PacketTimestamp{0x0506'0708U};
    source.control.destination_socket_id = 0x090a'0b0cU;

    std::array<std::byte, packet_header_size> storage{};
    const auto result = encode_packet(source, storage);
    REQUIRE(result);
    const std::array expected{
        std::byte{0x80}, std::byte{0x02}, std::byte{0x12}, std::byte{0x34},
        std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04},
        std::byte{0x05}, std::byte{0x06}, std::byte{0x07}, std::byte{0x08},
        std::byte{0x09}, std::byte{0x0a}, std::byte{0x0b}, std::byte{0x0c},
    };
    REQUIRE_EQ(storage, expected);
}

TEST(codec_rejects_short_and_unknown_control_packets)
{
    std::array<std::byte, 15> short_packet{};
    REQUIRE_EQ(decode_packet(short_packet).error, Error::packet_too_short);

    std::array<std::byte, 16> unknown{};
    unknown[0] = std::byte{0x80};
    unknown[1] = std::byte{0x09};
    REQUIRE_EQ(decode_packet(unknown).error, Error::invalid_control_type);
}

TEST(codec_emits_canonical_zero_word_for_payload_free_controls)
{
    constexpr std::array control_types {
        ControlType::acknowledgement_of_ack,
        ControlType::keepalive,
        ControlType::congestion_warning,
        ControlType::shutdown,
        ControlType::peer_error,
    };
    for (const ControlType type : control_types) {
        MutablePacketView packet;
        packet.kind = PacketKind::control;
        packet.control.type = type;
        std::array<std::byte, packet_header_size + 4U> storage {
            std::byte {0xff}};
        const auto encoded = encode_packet(packet, storage);
        REQUIRE(encoded);
        REQUIRE_EQ(encoded.bytes_written, storage.size());
        for (std::size_t index = packet_header_size; index < storage.size();
            ++index) {
            REQUIRE_EQ(storage[index], std::byte {0});
        }
        const auto decoded = decode_packet(storage);
        REQUIRE(decoded);
        REQUIRE(is_zero_word_payload(decoded.packet.payload));
    }
}

TEST(codec_rejects_empty_and_partial_control_words)
{
    MutablePacketView packet;
    packet.kind = PacketKind::control;
    packet.control.type = ControlType::acknowledgement;
    const std::array<std::byte, 4> word {};
    packet.payload = word;
    std::array<std::byte, packet_header_size + 4U> storage {};
    REQUIRE(encode_packet(packet, storage));

    for (const std::size_t payload_size : {0U, 1U, 2U, 3U}) {
        REQUIRE_EQ(decode_packet(std::span {storage}.first(
                                     packet_header_size + payload_size))
                       .error,
            Error::invalid_control_payload);
    }
    REQUIRE(decode_packet(storage));
}

TEST(protected_payload_codec_owns_the_complete_gcm_tag_layout)
{
    const auto budget =
        make_crypto_payload_budget(CryptoMode::aes_gcm, 1'500U, 44U);
    REQUIRE(budget);
    REQUIRE_EQ(budget.maximum_wire_payload_size, 1'456U);
    REQUIRE_EQ(budget.maximum_plaintext_payload_size, 1'440U);
    REQUIRE_EQ(budget.authentication_tag_size, 16U);

    std::array<std::byte, maximum_data_payload_size> storage {};
    const auto output = prepare_protected_payload(
        storage, budget.maximum_plaintext_payload_size, budget);
    REQUIRE(output);
    REQUIRE_EQ(output.bytes_written, storage.size());
    REQUIRE_EQ(output.payload.ciphertext.size(), 1'440U);
    REQUIRE_EQ(output.payload.authentication_tag.size(), 16U);
    output.payload.ciphertext.front() = std::byte {0x31};
    output.payload.authentication_tag.front() = std::byte {0xa5};

    const auto decoded = decode_protected_payload(storage, budget);
    REQUIRE(decoded);
    REQUIRE_EQ(decoded.payload.ciphertext.size(), 1'440U);
    REQUIRE_EQ(decoded.payload.authentication_tag.size(), 16U);
    REQUIRE_EQ(decoded.payload.ciphertext.front(), std::byte {0x31});
    REQUIRE_EQ(decoded.payload.authentication_tag.front(), std::byte {0xa5});

    std::array<std::byte, srt_gcm_authentication_tag_size - 1U> truncated {};
    REQUIRE_EQ(decode_protected_payload(truncated, budget).error,
        Error::invalid_payload_size);
    std::array<std::byte, maximum_data_payload_size + 1U> surplus {};
    REQUIRE_EQ(decode_protected_payload(surplus, budget).error,
        Error::invalid_payload_size);

    std::array<std::byte, 18U> too_small {};
    REQUIRE_EQ(prepare_protected_payload(too_small, 3U, budget).error,
        Error::buffer_too_small);
    REQUIRE_EQ(prepare_protected_payload(
                   storage, budget.maximum_plaintext_payload_size + 1U, budget)
                   .error,
        Error::invalid_payload_size);
}

TEST(protected_payload_codec_keeps_ctr_tagless_and_rejects_unresolved_auto)
{
    const auto ctr =
        make_crypto_payload_budget(CryptoMode::aes_ctr, 1'500U, 44U, 4U);
    REQUIRE(ctr);
    REQUIRE_EQ(ctr.maximum_wire_payload_size, 1'452U);
    REQUIRE_EQ(ctr.maximum_plaintext_payload_size, 1'452U);
    REQUIRE_EQ(ctr.authentication_tag_size, 0U);

    const std::array<std::byte, 3U> payload {
        std::byte {1}, std::byte {2}, std::byte {3}};
    const auto decoded = decode_protected_payload(payload, ctr);
    REQUIRE(decoded);
    REQUIRE_EQ(decoded.payload.ciphertext.size(), payload.size());
    REQUIRE(decoded.payload.authentication_tag.empty());

    const auto unresolved =
        make_crypto_payload_budget(CryptoMode::automatic, 1'500U, 44U);
    REQUIRE(!unresolved);
    REQUIRE_EQ(unresolved.error, Error::unsupported);
    REQUIRE_EQ(decode_protected_payload(payload, unresolved).error,
        Error::unsupported);
}

TEST(c_api_inspects_without_exposing_cpp_types)
{
    std::array<std::uint8_t, 16> bytes{};
    bytes[3] = 42;
    bytes[11] = 7;
    bytes[15] = 9;
    robotweax_srt_packet_info info{};
    info.struct_size = sizeof info;
    info.abi_version = ROBOTWEAX_SRT_ABI_VERSION;
    REQUIRE_EQ(robotweax_srt_inspect_packet(bytes.data(), bytes.size(), &info), ROBOTWEAX_SRT_OK);
    REQUIRE_EQ(info.kind, ROBOTWEAX_SRT_DATA_PACKET);
    REQUIRE_EQ(info.sequence_number, 42U);
    REQUIRE_EQ(info.timestamp, 7U);
    REQUIRE_EQ(info.destination_socket_id, 9U);
}
