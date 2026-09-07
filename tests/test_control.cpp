#include "test.hpp"

#include "robotweax/srt/codec.hpp"
#include "robotweax/srt/control.hpp"
#include "robotweax/srt/session.hpp"

#include <array>
#include <cstddef>

using namespace robotweax::srt;

TEST(full_acknowledgement_round_trips_all_v101_fields)
{
    Acknowledgement source;
    source.acknowledgement_number = 19;
    source.next_sequence = SequenceNumber{101};
    source.round_trip_time_microseconds = 20'000;
    source.round_trip_time_variance_microseconds = 4'000;
    source.available_receive_buffer_packets = 8'192;
    source.receive_rate_packets_per_second = 9'000;
    source.estimated_link_capacity_packets_per_second = 12'000;
    source.receive_rate_bytes_per_second = 11'844'000;
    source.has_rate_metrics = true;

    std::array<std::byte, 28> payload{};
    const auto payload_encoded = encode_acknowledgement_payload(source, payload);
    REQUIRE(payload_encoded);
    REQUIRE_EQ(payload_encoded.bytes_written, payload.size());

    MutablePacketView packet;
    packet.kind = PacketKind::control;
    packet.control.type = ControlType::acknowledgement;
    packet.control.type_specific = source.acknowledgement_number;
    packet.payload = payload;
    std::array<std::byte, 44> datagram{};
    REQUIRE(encode_packet(packet, datagram));

    const auto decoded_packet = decode_packet(datagram);
    REQUIRE(decoded_packet);
    const auto decoded = decode_acknowledgement(decoded_packet.packet);
    REQUIRE(decoded);
    REQUIRE_EQ(decoded.acknowledgement.kind, AcknowledgementKind::full);
    REQUIRE_EQ(decoded.acknowledgement.acknowledgement_number, 19U);
    REQUIRE_EQ(decoded.acknowledgement.next_sequence, SequenceNumber{101});
    REQUIRE_EQ(decoded.acknowledgement.round_trip_time_microseconds, 20'000U);
    REQUIRE_EQ(decoded.acknowledgement.available_receive_buffer_packets, 8'192U);
    REQUIRE_EQ(decoded.acknowledgement.receive_rate_bytes_per_second, 11'844'000U);
}

TEST(lite_acknowledgement_is_exactly_one_word)
{
    Acknowledgement source;
    source.kind = AcknowledgementKind::lite;
    source.next_sequence = SequenceNumber{0x1234'5678U};
    std::array<std::byte, 4> payload{};
    const auto encoded = encode_acknowledgement_payload(source, payload);
    REQUIRE(encoded);
    REQUIRE_EQ(payload[0], std::byte{0x12});
    REQUIRE_EQ(payload[3], std::byte{0x78});
}

TEST(acknowledgement_decoder_rejects_numbered_lite_and_zero_numbered_full_ack)
{
    std::array<std::byte, 28> payload {};
    PacketView packet;
    packet.kind = PacketKind::control;
    packet.control.type = ControlType::acknowledgement;

    packet.control.type_specific = 1U;
    packet.payload = std::span {payload}.first(4U);
    REQUIRE_EQ(
        decode_acknowledgement(packet).error, Error::invalid_control_payload);

    packet.control.type_specific = 0U;
    packet.payload = payload;
    REQUIRE_EQ(
        decode_acknowledgement(packet).error, Error::invalid_control_payload);
}

TEST(full_acknowledgement_preserves_the_complete_32_bit_number)
{
    std::array<std::byte, 28> payload {};
    PacketView packet;
    packet.kind = PacketKind::control;
    packet.control.type = ControlType::acknowledgement;
    packet.control.type_specific = 0x8000'0000U;
    packet.payload = payload;

    const auto decoded = decode_acknowledgement(packet);
    REQUIRE(decoded);
    REQUIRE_EQ(decoded.acknowledgement.kind, AcknowledgementKind::full);
    REQUIRE_EQ(decoded.acknowledgement.acknowledgement_number, 0x8000'0000U);
}

TEST(lite_acknowledgement_encodes_zero_type_specific)
{
    ReliabilityAction action{
        .kind = ReliabilityActionKind::acknowledgement,
        .acknowledgement = {
            .kind = AcknowledgementKind::lite,
            .acknowledgement_number = 19,
            .next_sequence = SequenceNumber{101},
        },
    };
    std::array<std::byte, packet_header_size + 4U> datagram{};
    const auto encoded = encode_reliability_action(
        action, PacketTimestamp{1}, 77, datagram);
    REQUIRE(encoded);

    const auto decoded = decode_packet(datagram);
    REQUIRE(decoded);
    REQUIRE_EQ(decoded.packet.control.type,
        ControlType::acknowledgement);
    REQUIRE_EQ(decoded.packet.control.type_specific, 0U);
    REQUIRE_EQ(decoded.packet.payload.size(), 4U);
}

TEST(small_acknowledgement_is_exactly_four_words)
{
    Acknowledgement source;
    source.kind = AcknowledgementKind::small;
    source.next_sequence = SequenceNumber {101};
    source.round_trip_time_microseconds = 20'000;
    source.round_trip_time_variance_microseconds = 4'000;
    source.available_receive_buffer_packets = 8'192;

    std::array<std::byte, 16> payload {};
    const auto encoded = encode_acknowledgement_payload(source, payload);
    REQUIRE(encoded);
    REQUIRE_EQ(encoded.bytes_written, payload.size());

    PacketView packet;
    packet.kind = PacketKind::control;
    packet.control.type = ControlType::acknowledgement;
    packet.control.type_specific = source.acknowledgement_number;
    packet.payload = payload;
    const auto decoded = decode_acknowledgement(packet);
    REQUIRE(decoded);
    REQUIRE_EQ(decoded.acknowledgement.kind, AcknowledgementKind::small);
    REQUIRE_EQ(decoded.acknowledgement.next_sequence, SequenceNumber {101});
    REQUIRE_EQ(decoded.acknowledgement.round_trip_time_microseconds, 20'000U);
    REQUIRE_EQ(
        decoded.acknowledgement.round_trip_time_variance_microseconds, 4'000U);
    REQUIRE_EQ(
        decoded.acknowledgement.available_receive_buffer_packets, 8'192U);
    REQUIRE(!decoded.acknowledgement.has_rate_metrics);
}

TEST(small_acknowledgement_emits_zero_type_specific)
{
    ReliabilityAction action {
        .kind = ReliabilityActionKind::acknowledgement,
        .acknowledgement =
            {
                .kind = AcknowledgementKind::small,
                .acknowledgement_number = 19,
                .next_sequence = SequenceNumber {101},
            },
    };
    std::array<std::byte, packet_header_size + 16U> datagram {};
    const auto encoded =
        encode_reliability_action(action, PacketTimestamp {1}, 77, datagram);
    REQUIRE(encoded);

    const auto decoded = decode_packet(datagram);
    REQUIRE(decoded);
    REQUIRE_EQ(decoded.packet.control.type, ControlType::acknowledgement);
    REQUIRE_EQ(decoded.packet.control.type_specific, 0U);
    REQUIRE_EQ(decoded.packet.payload.size(), 16U);
}

TEST(full_acknowledgement_always_contains_the_complete_v101_shape)
{
    Acknowledgement source;
    source.kind = AcknowledgementKind::full;
    source.acknowledgement_number = 1U;
    source.next_sequence = SequenceNumber {101};
    source.has_rate_metrics = false;

    std::array<std::byte, 28> payload {};
    const auto encoded = encode_acknowledgement_payload(source, payload);
    REQUIRE(encoded);
    REQUIRE_EQ(encoded.bytes_written, payload.size());

    PacketView packet;
    packet.kind = PacketKind::control;
    packet.control.type = ControlType::acknowledgement;
    packet.control.type_specific = source.acknowledgement_number;
    packet.payload = payload;
    const auto decoded = decode_acknowledgement(packet);
    REQUIRE(decoded);
    REQUIRE_EQ(decoded.acknowledgement.kind, AcknowledgementKind::full);
    REQUIRE(decoded.acknowledgement.has_rate_metrics);
    REQUIRE_EQ(decoded.acknowledgement.receive_rate_packets_per_second, 0U);
    REQUIRE_EQ(
        decoded.acknowledgement.estimated_link_capacity_packets_per_second, 0U);
    REQUIRE_EQ(decoded.acknowledgement.receive_rate_bytes_per_second, 0U);
}

TEST(acknowledgement_decoder_accepts_deployed_full_wire_lengths)
{
    std::array<std::byte, 32> payload {};
    PacketView packet;
    packet.kind = PacketKind::control;
    packet.control.type = ControlType::acknowledgement;
    packet.control.type_specific = 7;

    for (const std::size_t payload_size : {24U, 28U, 32U}) {
        packet.payload = std::span {payload}.first(payload_size);
        const auto decoded = decode_acknowledgement(packet);
        REQUIRE(decoded);
        REQUIRE_EQ(decoded.acknowledgement.kind, AcknowledgementKind::full);
        REQUIRE_EQ(decoded.acknowledgement.acknowledgement_number, 7U);
        REQUIRE(decoded.acknowledgement.has_rate_metrics);
    }
}

TEST(acknowledgement_decoder_rejects_non_sequence_high_bits)
{
    std::array<std::byte, 28> payload {};
    payload[0] = std::byte {0x80};
    PacketView packet;
    packet.kind = PacketKind::control;
    packet.control.type = ControlType::acknowledgement;

    packet.payload = std::span {payload}.first(4U);
    REQUIRE_EQ(
        decode_acknowledgement(packet).error, Error::invalid_control_payload);
    packet.payload = payload;
    REQUIRE_EQ(
        decode_acknowledgement(packet).error, Error::invalid_control_payload);
    packet.payload = std::span {payload}.first(16U);
    REQUIRE_EQ(
        decode_acknowledgement(packet).error, Error::invalid_control_payload);
}

TEST(loss_ranges_use_reference_range_marker)
{
    const std::array<SequenceRange, 2> ranges{{
        {.first = SequenceNumber{42}, .last = SequenceNumber{42}},
        {.first = SequenceNumber{50}, .last = SequenceNumber{55}},
    }};
    std::array<std::byte, 12> payload{};
    const auto encoded = encode_loss_ranges(ranges, payload);
    REQUIRE(encoded);
    REQUIRE_EQ(encoded.bytes_written, payload.size());
    REQUIRE_EQ(payload[0], std::byte{0});
    REQUIRE_EQ(payload[3], std::byte{42});
    REQUIRE_EQ(payload[4], std::byte{0x80});
    REQUIRE_EQ(payload[7], std::byte{50});

    const auto first = decode_loss_range(payload);
    REQUIRE(first);
    REQUIRE_EQ(first.range.first, SequenceNumber{42});
    REQUIRE_EQ(first.range.last, SequenceNumber{42});
    REQUIRE_EQ(first.bytes_consumed, 4U);
    const auto second = decode_loss_range(std::span{payload}.subspan(first.bytes_consumed));
    REQUIRE(second);
    REQUIRE_EQ(second.range.first, SequenceNumber{50});
    REQUIRE_EQ(second.range.last, SequenceNumber{55});
}

TEST(loss_range_decoder_rejects_truncation_and_reversed_ranges)
{
    const std::array<std::byte, 4> truncated{
        std::byte{0x80}, std::byte{0}, std::byte{0}, std::byte{1},
    };
    REQUIRE_EQ(decode_loss_range(truncated).error, Error::invalid_control_payload);

    const std::array<std::byte, 8> reversed{
        std::byte{0x80}, std::byte{0}, std::byte{0}, std::byte{5},
        std::byte{0}, std::byte{0}, std::byte{0}, std::byte{4},
    };
    REQUIRE_EQ(decode_loss_range(reversed).error, Error::invalid_control_payload);
}

TEST(loss_range_decoder_rejects_every_truncated_range_word)
{
    const std::array<std::byte, 8> range{
        std::byte{0x80}, std::byte{0}, std::byte{0}, std::byte{1},
        std::byte{0}, std::byte{0}, std::byte{0}, std::byte{2},
    };
    for (std::size_t size = 0; size < range.size(); ++size) {
        REQUIRE_EQ(decode_loss_range(
                       std::span{range}.first(size)).error,
            Error::invalid_control_payload);
    }
    REQUIRE(decode_loss_range(range));
}

TEST(drop_request_uses_message_number_and_exact_sequence_pair)
{
    DropRequest source{
        .message_number = 77,
        .sequences = {.first = SequenceNumber{100}, .last = SequenceNumber{105}},
    };
    std::array<std::byte, 8> payload{};
    REQUIRE(encode_drop_request_payload(source, payload));

    MutablePacketView packet;
    packet.kind = PacketKind::control;
    packet.control.type = ControlType::drop_request;
    packet.control.type_specific = source.message_number;
    packet.payload = payload;
    std::array<std::byte, 24> datagram{};
    REQUIRE(encode_packet(packet, datagram));
    const auto decoded_packet = decode_packet(datagram);
    REQUIRE(decoded_packet);
    const auto decoded = decode_drop_request(decoded_packet.packet);
    REQUIRE(decoded);
    REQUIRE_EQ(decoded.request.message_number, 77U);
    REQUIRE_EQ(decoded.request.sequences.first, SequenceNumber{100});
    REQUIRE_EQ(decoded.request.sequences.last, SequenceNumber{105});
}

TEST(drop_request_decoder_requires_exactly_two_sequence_words)
{
    const std::array<std::byte, 9> payload{
        std::byte{0}, std::byte{0}, std::byte{0}, std::byte{100},
        std::byte{0}, std::byte{0}, std::byte{0}, std::byte{105},
        std::byte{0},
    };
    PacketView packet;
    packet.kind = PacketKind::control;
    packet.control.type = ControlType::drop_request;
    packet.control.type_specific = 77;
    for (std::size_t size = 0; size <= payload.size(); ++size) {
        packet.payload = std::span{payload}.first(size);
        const auto decoded = decode_drop_request(packet);
        if (size == 8U) {
            REQUIRE(decoded);
        } else {
            REQUIRE_EQ(decoded.error,
                Error::invalid_control_payload);
        }
    }
}

TEST(drop_request_decoder_rejects_non_sequence_high_bits)
{
    std::array<std::byte, 8> payload {
        std::byte {0x80},
        std::byte {0},
        std::byte {0},
        std::byte {1},
        std::byte {0},
        std::byte {0},
        std::byte {0},
        std::byte {2},
    };
    PacketView packet;
    packet.kind = PacketKind::control;
    packet.control.type = ControlType::drop_request;
    packet.payload = payload;
    REQUIRE_EQ(
        decode_drop_request(packet).error, Error::invalid_control_payload);

    payload[0] = std::byte {0};
    payload[4] = std::byte {0x80};
    REQUIRE_EQ(
        decode_drop_request(packet).error, Error::invalid_control_payload);
}
