#include "test.hpp"

#include "robotweax/srt/codec.hpp"
#include "robotweax/srt/fec.hpp"

#include <algorithm>
#include <array>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace robotweax::srt;

namespace {

PacketFilterConfiguration row_configuration(
    std::uint32_t columns,
    std::string_view arq = "always")
{
    const std::string text =
        "fec,cols:" + std::to_string(columns)
        + ",rows:1,arq:" + std::string{arq};
    const auto parsed =
        parse_packet_filter_configuration(
            text);
    REQUIRE(parsed);
    return parsed.configuration;
}

PacketFilterConfiguration column_configuration(
    std::uint32_t columns,
    std::uint32_t rows,
    std::string_view layout = "even",
    std::string_view arq = "always")
{
    const std::string text =
        "fec,cols:" + std::to_string(columns)
        + ",rows:-" + std::to_string(rows)
        + ",layout:" + std::string{layout}
        + ",arq:" + std::string{arq};
    const auto parsed =
        parse_packet_filter_configuration(text);
    REQUIRE(parsed);
    return parsed.configuration;
}

PacketFilterConfiguration matrix_configuration(
    std::uint32_t columns,
    std::uint32_t rows,
    std::string_view layout = "even",
    std::string_view arq = "always")
{
    const std::string text =
        "fec,cols:" + std::to_string(columns)
        + ",rows:" + std::to_string(rows)
        + ",layout:" + std::string{layout}
        + ",arq:" + std::string{arq};
    const auto parsed =
        parse_packet_filter_configuration(text);
    REQUIRE(parsed);
    return parsed.configuration;
}

PacketView source_packet(
    std::uint32_t sequence,
    std::uint32_t timestamp,
    EncryptionKey key,
    std::span<const std::byte> payload,
    bool in_order = false)
{
    return {
        .kind = PacketKind::data,
        .data = {
            .sequence = SequenceNumber{sequence},
            .message_number = sequence + 1U,
            .boundary = MessageBoundary::solo,
            .in_order = in_order,
            .encryption_key = key,
            .timestamp = PacketTimestamp{timestamp},
            .destination_socket_id = 77,
        },
        .payload = payload,
    };
}

} // namespace

TEST(row_fec_encoder_emits_exact_xor_recovery_fields)
{
    const auto configuration = row_configuration(3U);
    RowFecEncoder encoder{
        configuration, SequenceNumber{100}, 4U};
    const std::array first{
        std::byte{0x10}, std::byte{0x20}};
    const std::array second{
        std::byte{0x01}, std::byte{0x02},
        std::byte{0x03}};
    const std::array third{
        std::byte{0xff}};

    REQUIRE_EQ(encoder.feed_source(source_packet(
                   100, 0x1020'3040U,
                   EncryptionKey::even, first)),
        Error::none);
    REQUIRE(!encoder.control_packet_ready());
    REQUIRE_EQ(encoder.feed_source(source_packet(
                   101, 0x0102'0304U,
                   EncryptionKey::odd, second)),
        Error::none);
    REQUIRE_EQ(encoder.feed_source(source_packet(
                   102, 0xa0b0'c0d0U,
                   EncryptionKey::none, third)),
        Error::none);

    const auto packet = encoder.control_packet();
    REQUIRE(packet.has_value());
    REQUIRE_EQ(packet->header.sequence,
        SequenceNumber{102});
    REQUIRE_EQ(packet->header.message_number, 0U);
    REQUIRE_EQ(packet->header.boundary,
        MessageBoundary::solo);
    REQUIRE_EQ(packet->header.timestamp,
        PacketTimestamp{
            0x1020'3040U ^ 0x0102'0304U
                ^ 0xa0b0'c0d0U});
    REQUIRE_EQ(packet->payload.size(), 8U);

    const auto control =
        decode_fec_control_payload(packet->payload);
    REQUIRE(control);
    REQUIRE_EQ(control.header.group_index, -1);
    REQUIRE_EQ(control.header.flags_recovery,
        1U ^ 2U);
    REQUIRE_EQ(control.header.length_recovery,
        2U ^ 3U ^ 1U);
    REQUIRE_EQ(control.payload_recovery[0],
        std::byte{0x10 ^ 0x01 ^ 0xff});
    REQUIRE_EQ(control.payload_recovery[1],
        std::byte{0x20 ^ 0x02});
    REQUIRE_EQ(control.payload_recovery[2],
        std::byte{0x03});
    REQUIRE_EQ(control.payload_recovery[3],
        std::byte{0});

    std::array<std::byte, 24> datagram{};
    const auto encoded = encode_packet({
        .kind = PacketKind::data,
        .data = packet->header,
        .payload = packet->payload,
    }, datagram);
    REQUIRE(encoded);
    const std::array expected{
        std::byte{0x00}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x66},
        std::byte{0xc0}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00},
        std::byte{0xb1}, std::byte{0x92},
        std::byte{0xf3}, std::byte{0x94},
        std::byte{0x00}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x4d},
        std::byte{0xff}, std::byte{0x03},
        std::byte{0x00}, std::byte{0x00},
        std::byte{0xee}, std::byte{0x22},
        std::byte{0x03}, std::byte{0x00},
    };
    REQUIRE_EQ(datagram, expected);

    encoder.consume_control_packet();
    REQUIRE(!encoder.control_packet_ready());
}

TEST(row_fec_decoder_reconstructs_one_missing_wire_packet)
{
    const auto configuration = row_configuration(3U);
    RowFecEncoder encoder{
        configuration, SequenceNumber{200}, 4U};
    RowFecDecoder decoder{
        configuration, SequenceNumber{200}, 16U, 4U};
    const std::array first{
        std::byte{'a'}, std::byte{'b'}};
    const std::array missing{
        std::byte{'C'}, std::byte{'D'},
        std::byte{'E'}};
    const std::array third{
        std::byte{'x'}};
    const auto first_packet = source_packet(
        200, 10U, EncryptionKey::even, first, true);
    const auto missing_packet = source_packet(
        201, 20U, EncryptionKey::odd, missing, true);
    const auto third_packet = source_packet(
        202, 30U, EncryptionKey::none, third, true);
    REQUIRE_EQ(encoder.feed_source(first_packet),
        Error::none);
    REQUIRE_EQ(encoder.feed_source(missing_packet),
        Error::none);
    REQUIRE_EQ(encoder.feed_source(third_packet),
        Error::none);
    const auto control = encoder.control_packet();
    REQUIRE(control.has_value());

    REQUIRE(decoder.receive(first_packet));
    REQUIRE(decoder.receive(third_packet));
    const PacketView control_view{
        .kind = PacketKind::data,
        .data = control->header,
        .payload = control->payload,
    };
    const auto rebuilt = decoder.receive(control_view);
    REQUIRE(rebuilt);
    REQUIRE(rebuilt.consume_control_packet);
    REQUIRE(rebuilt.has_reconstructed_packet);
    REQUIRE_EQ(rebuilt.reconstructed_packet.data.sequence,
        SequenceNumber{201});
    REQUIRE_EQ(rebuilt.reconstructed_packet.data.message_number, 202U);
    REQUIRE_EQ(rebuilt.reconstructed_packet.data.boundary,
        MessageBoundary::solo);
    REQUIRE(rebuilt.reconstructed_packet.data.in_order);
    REQUIRE_EQ(
        rebuilt.reconstructed_packet.data.encryption_key,
        EncryptionKey::odd);
    REQUIRE(
        rebuilt.reconstructed_packet.data.retransmitted);
    REQUIRE_EQ(rebuilt.reconstructed_packet.data.timestamp,
        PacketTimestamp{20U});
    REQUIRE_EQ(
        rebuilt.reconstructed_packet.payload.size(),
        missing.size());
    REQUIRE(std::equal(
        rebuilt.reconstructed_packet.payload.begin(),
        rebuilt.reconstructed_packet.payload.end(),
        missing.begin()));
}

TEST(row_fec_decoder_zero_pads_a_shorter_peer_control_payload)
{
    // The peer's SRTO_PAYLOADSIZE is not negotiated. A peer encoder with
    // a smaller payload size emits control payloads shorter than the local
    // recovery buffer; they must still rebuild the missing packet.
    const auto configuration = row_configuration(3U);
    RowFecEncoder encoder {configuration, SequenceNumber {200}, 4U};
    RowFecDecoder decoder {configuration, SequenceNumber {200}, 16U, 8U};
    const std::array first {std::byte {'a'}, std::byte {'b'}};
    const std::array missing {
        std::byte {'C'}, std::byte {'D'}, std::byte {'E'}};
    const std::array third {std::byte {'x'}};
    const auto first_packet =
        source_packet(200, 10U, EncryptionKey::none, first, true);
    const auto missing_packet =
        source_packet(201, 20U, EncryptionKey::none, missing, true);
    const auto third_packet =
        source_packet(202, 30U, EncryptionKey::none, third, true);
    REQUIRE_EQ(encoder.feed_source(first_packet), Error::none);
    REQUIRE_EQ(encoder.feed_source(missing_packet), Error::none);
    REQUIRE_EQ(encoder.feed_source(third_packet), Error::none);
    const auto control = encoder.control_packet();
    REQUIRE(control.has_value());
    REQUIRE_EQ(control->payload.size(), 4U + 4U);

    REQUIRE(decoder.receive(first_packet));
    REQUIRE(decoder.receive(third_packet));
    const auto rebuilt = decoder.receive({
        .kind = PacketKind::data,
        .data = control->header,
        .payload = control->payload,
    });
    REQUIRE(rebuilt);
    REQUIRE(rebuilt.consume_control_packet);
    REQUIRE(rebuilt.has_reconstructed_packet);
    REQUIRE_EQ(
        rebuilt.reconstructed_packet.data.sequence, SequenceNumber {201});
    REQUIRE_EQ(rebuilt.reconstructed_packet.payload.size(), missing.size());
    REQUIRE(std::equal(rebuilt.reconstructed_packet.payload.begin(),
        rebuilt.reconstructed_packet.payload.end(), missing.begin()));
}

TEST(row_fec_decoder_reports_a_clipped_longer_peer_packet_for_arq)
{
    // A peer with a larger SRTO_PAYLOADSIZE sends source and control
    // payloads longer than the local recovery buffer. The decoder keeps
    // tracking the group: shorter missing packets are still rebuilt, and
    // a missing packet longer than the buffer is reported through row
    // expiry so ARQ recovers it, instead of leaving the row untracked.
    const auto configuration = row_configuration(3U, "onreq");
    RowFecEncoder encoder {configuration, SequenceNumber {300}, 8U};
    RowFecDecoder decoder {configuration, SequenceNumber {300}, 32U, 4U};
    const std::array long_payload {std::byte {'1'}, std::byte {'2'},
        std::byte {'3'}, std::byte {'4'}, std::byte {'5'}, std::byte {'6'}};
    const std::array short_payload {std::byte {'s'}, std::byte {'t'}};
    const std::array other {std::byte {'o'}};

    // Row 0: the longer packet 301 is lost -> cannot be rebuilt locally.
    const auto p300 = source_packet(300, 1U, EncryptionKey::none, other);
    const auto p301 = source_packet(301, 2U, EncryptionKey::none, long_payload);
    const auto p302 = source_packet(302, 3U, EncryptionKey::none, long_payload);
    REQUIRE_EQ(encoder.feed_source(p300), Error::none);
    REQUIRE_EQ(encoder.feed_source(p301), Error::none);
    REQUIRE_EQ(encoder.feed_source(p302), Error::none);
    const auto control_row0 = encoder.control_packet();
    REQUIRE(control_row0.has_value());
    REQUIRE_EQ(control_row0->payload.size(), 4U + 8U);

    REQUIRE(decoder.receive(p300));
    REQUIRE(decoder.receive(p302));
    const auto not_rebuilt = decoder.receive({
        .kind = PacketKind::data,
        .data = control_row0->header,
        .payload = control_row0->payload,
    });
    REQUIRE(not_rebuilt);
    REQUIRE(not_rebuilt.consume_control_packet);
    REQUIRE(!not_rebuilt.has_reconstructed_packet);
    encoder.consume_control_packet();

    // Row 1: a short packet 304 is lost next to longer neighbours; the
    // clipped control payload still rebuilds it exactly.
    const auto p303 = source_packet(303, 4U, EncryptionKey::none, long_payload);
    const auto p304 =
        source_packet(304, 5U, EncryptionKey::none, short_payload);
    const auto p305 = source_packet(305, 6U, EncryptionKey::none, long_payload);
    REQUIRE_EQ(encoder.feed_source(p303), Error::none);
    REQUIRE_EQ(encoder.feed_source(p304), Error::none);
    REQUIRE_EQ(encoder.feed_source(p305), Error::none);
    const auto control_row1 = encoder.control_packet();
    REQUIRE(control_row1.has_value());
    REQUIRE(decoder.receive(p303));
    // Row 0 expires once row 1 passes its grace window; the clipped
    // packet 301 is reported for ARQ instead of being rebuilt.
    const auto expired = decoder.receive(p305);
    REQUIRE(expired);
    REQUIRE_EQ(expired.irrecoverable_losses.size(), 1U);
    REQUIRE_EQ(expired.irrecoverable_losses[0].first, SequenceNumber {301});
    REQUIRE_EQ(expired.irrecoverable_losses[0].last, SequenceNumber {301});
    const auto rebuilt = decoder.receive({
        .kind = PacketKind::data,
        .data = control_row1->header,
        .payload = control_row1->payload,
    });
    REQUIRE(rebuilt);
    REQUIRE(rebuilt.has_reconstructed_packet);
    REQUIRE_EQ(
        rebuilt.reconstructed_packet.data.sequence, SequenceNumber {304});
    REQUIRE_EQ(
        rebuilt.reconstructed_packet.payload.size(), short_payload.size());
    REQUIRE(std::equal(rebuilt.reconstructed_packet.payload.begin(),
        rebuilt.reconstructed_packet.payload.end(), short_payload.begin()));
}

TEST(column_fec_decoder_zero_pads_a_shorter_peer_control_payload)
{
    const auto configuration = column_configuration(2U, 2U);
    ColumnFecEncoder encoder {configuration, SequenceNumber {400}, 4U};
    ColumnFecDecoder decoder {configuration, SequenceNumber {400}, 16U, 8U};
    const std::array a {std::byte {'a'}};
    const std::array missing {
        std::byte {'M'}, std::byte {'N'}, std::byte {'O'}};
    // Column 0 of the even layout: sequences 400 and 402.
    const auto p400 = source_packet(400, 1U, EncryptionKey::none, a);
    const auto p401 = source_packet(401, 2U, EncryptionKey::none, a);
    const auto p402 = source_packet(402, 3U, EncryptionKey::none, missing);
    for (const auto& packet : {p400, p401, p402}) {
        REQUIRE_EQ(encoder.feed_source(packet), Error::none);
    }
    const auto control = encoder.control_packet();
    REQUIRE(control.has_value());
    REQUIRE_EQ(control->payload.size(), 4U + 4U);
    REQUIRE(decoder.receive(p400));
    REQUIRE(decoder.receive(p401));
    const auto rebuilt = decoder.receive({
        .kind = PacketKind::data,
        .data = control->header,
        .payload = control->payload,
    });
    REQUIRE(rebuilt);
    REQUIRE(rebuilt.consume_control_packet);
    REQUIRE(rebuilt.has_reconstructed_packet);
    REQUIRE_EQ(
        rebuilt.reconstructed_packet.data.sequence, SequenceNumber {402});
    REQUIRE(std::equal(rebuilt.reconstructed_packet.payload.begin(),
        rebuilt.reconstructed_packet.payload.end(), missing.begin()));
}

TEST(row_fec_decoder_reconstructs_exact_wrapped_message_number)
{
    constexpr std::uint32_t maximum_message_number = 0x03ff'ffffU;
    const auto configuration = row_configuration(2U);
    RowFecEncoder encoder {configuration, SequenceNumber {500}, 1U};
    const std::array first_payload {std::byte {'a'}};
    const std::array second_payload {std::byte {'b'}};
    PacketView first =
        source_packet(500U, 10U, EncryptionKey::even, first_payload);
    PacketView second =
        source_packet(501U, 20U, EncryptionKey::even, second_payload);
    first.data.message_number = maximum_message_number;
    second.data.message_number = 1U;
    REQUIRE_EQ(encoder.feed_source(first), Error::none);
    REQUIRE_EQ(encoder.feed_source(second), Error::none);
    const auto control = encoder.control_packet();
    REQUIRE(control.has_value());
    const PacketView control_view {
        .kind = PacketKind::data,
        .data = control->header,
        .payload = control->payload,
    };

    RowFecDecoder forward {configuration, SequenceNumber {500}, 8U, 1U};
    REQUIRE(forward.receive(first));
    const auto rebuilt_second = forward.receive(control_view);
    REQUIRE(rebuilt_second);
    REQUIRE(rebuilt_second.has_reconstructed_packet);
    REQUIRE_EQ(rebuilt_second.reconstructed_packet.data.sequence,
        SequenceNumber {501});
    REQUIRE_EQ(rebuilt_second.reconstructed_packet.data.message_number, 1U);

    RowFecDecoder backward {configuration, SequenceNumber {500}, 8U, 1U};
    REQUIRE(backward.receive(second));
    const auto rebuilt_first = backward.receive(control_view);
    REQUIRE(rebuilt_first);
    REQUIRE(rebuilt_first.has_reconstructed_packet);
    REQUIRE_EQ(
        rebuilt_first.reconstructed_packet.data.sequence, SequenceNumber {500});
    REQUIRE_EQ(rebuilt_first.reconstructed_packet.data.message_number,
        maximum_message_number);
}

TEST(column_fec_decoder_rejects_a_control_for_the_wrong_column_geometry)
{
    const auto configuration =
        column_configuration(3U, 2U);
    ColumnFecDecoder decoder{
        configuration, SequenceNumber{10}, 8U, 4U};
    std::array<std::byte, 8> payload{};
    REQUIRE_EQ(encode_fec_control_header({
                   .group_index = 1,
                   .length_recovery = 1,
               },
                   payload),
        Error::none);
    const auto result = decoder.receive({
        .kind = PacketKind::data,
        .data = {
            .sequence = SequenceNumber{13},
            .message_number = 0,
            .boundary = MessageBoundary::solo,
        },
        .payload = payload,
    });
    REQUIRE(!result);
    REQUIRE(result.consume_control_packet);
    REQUIRE_EQ(
        result.error, Error::invalid_control_payload);
}

TEST(row_fec_decoder_handles_control_first_duplicates_and_rollover)
{
    const auto configuration = row_configuration(2U);
    constexpr std::uint32_t first_sequence =
        SequenceNumber::mask;
    RowFecEncoder encoder{
        configuration,
        SequenceNumber{first_sequence}, 2U};
    RowFecDecoder decoder{
        configuration,
        SequenceNumber{first_sequence}, 8U, 2U};
    const std::array first{
        std::byte{0x11}, std::byte{0x22}};
    const std::array second{
        std::byte{0x33}};
    const auto first_packet = source_packet(
        first_sequence, 100U,
        EncryptionKey::none, first);
    const auto second_packet = source_packet(
        0U, 200U, EncryptionKey::none, second);
    REQUIRE_EQ(encoder.feed_source(first_packet),
        Error::none);
    REQUIRE_EQ(encoder.feed_source(second_packet),
        Error::none);
    const auto control = encoder.control_packet();
    REQUIRE(control.has_value());
    const PacketView control_view{
        .kind = PacketKind::data,
        .data = control->header,
        .payload = control->payload,
    };

    const auto control_first =
        decoder.receive(control_view);
    REQUIRE(control_first);
    REQUIRE(control_first.consume_control_packet);
    REQUIRE(!control_first.has_reconstructed_packet);
    REQUIRE(decoder.receive(control_view));
    const auto rebuilt = decoder.receive(second_packet);
    REQUIRE(rebuilt);
    REQUIRE(rebuilt.has_reconstructed_packet);
    REQUIRE_EQ(rebuilt.reconstructed_packet.data.sequence,
        SequenceNumber{first_sequence});
    REQUIRE_EQ(
        rebuilt.reconstructed_packet.payload.size(), 2U);
    REQUIRE_EQ(
        decoder.receive(second_packet)
            .has_reconstructed_packet,
        false);
}

TEST(row_fec_decoder_rejects_malformed_control_geometry)
{
    const auto configuration = row_configuration(2U);
    RowFecDecoder decoder{
        configuration, SequenceNumber{10}, 8U, 4U};
    std::array<std::byte, 8> payload{};
    REQUIRE_EQ(encode_fec_control_header({
                   .group_index = -1,
                   .flags_recovery = 0,
                   .length_recovery = 1,
               },
                   payload),
        Error::none);
    const PacketView wrong_sequence{
        .kind = PacketKind::data,
        .data = {
            .sequence = SequenceNumber{10},
            .message_number = 0,
            .boundary = MessageBoundary::solo,
        },
        .payload = payload,
    };
    const auto result = decoder.receive(
        wrong_sequence);
    REQUIRE(!result);
    REQUIRE(result.consume_control_packet);
    REQUIRE_EQ(result.error,
        Error::invalid_control_payload);
}

TEST(row_fec_decoder_expires_a_row_after_the_one_third_grace_window)
{
    const auto configuration =
        row_configuration(6U, "onreq");
    RowFecDecoder decoder{
        configuration, SequenceNumber{100}, 32U, 4U};
    const std::array payload{std::byte{'x'}};

    REQUIRE(decoder.receive(source_packet(
        100, 1U, EncryptionKey::none, payload)));
    REQUIRE(decoder.receive(source_packet(
        102, 2U, EncryptionKey::none, payload)));
    REQUIRE(decoder.receive(source_packet(
        106, 3U, EncryptionKey::none, payload)));
    REQUIRE(decoder.receive(source_packet(
        107, 4U, EncryptionKey::none, payload)));
    const auto at_boundary = decoder.receive(
        source_packet(
            108, 5U, EncryptionKey::none, payload));
    REQUIRE(at_boundary);
    REQUIRE(at_boundary.irrecoverable_losses.empty());

    const auto expired = decoder.receive(
        source_packet(
            109, 6U, EncryptionKey::none, payload));
    REQUIRE(expired);
    REQUIRE_EQ(
        expired.irrecoverable_losses.size(), 2U);
    REQUIRE_EQ(
        expired.irrecoverable_losses[0].first,
        SequenceNumber{101});
    REQUIRE_EQ(
        expired.irrecoverable_losses[0].last,
        SequenceNumber{101});
    REQUIRE_EQ(
        expired.irrecoverable_losses[1].first,
        SequenceNumber{103});
    REQUIRE_EQ(
        expired.irrecoverable_losses[1].last,
        SequenceNumber{105});
}

TEST(row_fec_decoder_does_not_expire_a_reconstructed_source_as_lost)
{
    const auto configuration =
        row_configuration(3U, "onreq");
    RowFecEncoder encoder{
        configuration, SequenceNumber{200}, 4U};
    RowFecDecoder decoder{
        configuration, SequenceNumber{200}, 16U, 4U};
    const std::array payload{std::byte{'x'}};
    const auto first = source_packet(
        200, 10U, EncryptionKey::none, payload);
    const auto missing = source_packet(
        201, 11U, EncryptionKey::none, payload);
    const auto last = source_packet(
        202, 12U, EncryptionKey::none, payload);
    REQUIRE_EQ(encoder.feed_source(first), Error::none);
    REQUIRE_EQ(encoder.feed_source(missing), Error::none);
    REQUIRE_EQ(encoder.feed_source(last), Error::none);
    REQUIRE(decoder.receive(first));
    REQUIRE(decoder.receive(last));
    const auto control = encoder.control_packet();
    REQUIRE(control.has_value());
    const auto rebuilt = decoder.receive({
        .kind = PacketKind::data,
        .data = control->header,
        .payload = control->payload,
    });
    REQUIRE(rebuilt);
    REQUIRE(rebuilt.has_reconstructed_packet);

    REQUIRE(decoder.receive(source_packet(
        203, 13U, EncryptionKey::none, payload)));
    REQUIRE(decoder.receive(source_packet(
        204, 14U, EncryptionKey::none, payload)));
    const auto expired = decoder.receive(
        source_packet(
            205, 15U, EncryptionKey::none, payload));
    REQUIRE(expired);
    REQUIRE(expired.irrecoverable_losses.empty());
}

TEST(row_fec_decoder_coalesces_skipped_rows_across_sequence_rollover)
{
    const auto configuration =
        row_configuration(2U, "onreq");
    constexpr std::uint32_t initial =
        SequenceNumber::mask - 1U;
    RowFecDecoder decoder{
        configuration, SequenceNumber{initial}, 8U, 2U};
    const std::array payload{std::byte{'x'}};

    REQUIRE(decoder.receive(source_packet(
        initial, 1U, EncryptionKey::none, payload)));
    const auto expired = decoder.receive(
        source_packet(
            3U, 2U, EncryptionKey::none, payload));
    REQUIRE(expired);
    REQUIRE_EQ(
        expired.irrecoverable_losses.size(), 1U);
    REQUIRE_EQ(
        expired.irrecoverable_losses[0].first,
        SequenceNumber{SequenceNumber::mask});
    REQUIRE_EQ(
        expired.irrecoverable_losses[0].last,
        SequenceNumber{1});
}

TEST(column_fec_encoder_emits_exact_even_column_recovery)
{
    const auto configuration =
        column_configuration(3U, 2U);
    ColumnFecEncoder encoder{
        configuration, SequenceNumber{100}, 4U};
    const std::array first{
        std::byte{0x10}, std::byte{0x20}};
    const std::array middle{std::byte{0x55}};
    const std::array last{
        std::byte{0x01}, std::byte{0x02},
        std::byte{0x03}};

    REQUIRE_EQ(encoder.feed_source(source_packet(
                   100, 10U,
                   EncryptionKey::even, first)),
        Error::none);
    REQUIRE_EQ(encoder.feed_source(source_packet(
                   101, 20U,
                   EncryptionKey::none, middle)),
        Error::none);
    REQUIRE_EQ(encoder.feed_source(source_packet(
                   102, 30U,
                   EncryptionKey::none, middle)),
        Error::none);
    REQUIRE_EQ(encoder.feed_source(source_packet(
                   103, 40U,
                   EncryptionKey::odd, last)),
        Error::none);

    const auto packet = encoder.control_packet();
    REQUIRE(packet.has_value());
    REQUIRE_EQ(
        packet->header.sequence, SequenceNumber{103});
    REQUIRE_EQ(packet->header.message_number, 0U);
    REQUIRE_EQ(packet->header.timestamp,
        PacketTimestamp{10U ^ 40U});
    const auto control =
        decode_fec_control_payload(packet->payload);
    REQUIRE(control);
    REQUIRE_EQ(control.header.group_index, 0);
    REQUIRE_EQ(
        control.header.flags_recovery, 1U ^ 2U);
    REQUIRE_EQ(
        control.header.length_recovery, 2U ^ 3U);
    REQUIRE_EQ(control.payload_recovery[0],
        std::byte{0x10 ^ 0x01});
    REQUIRE_EQ(control.payload_recovery[1],
        std::byte{0x20 ^ 0x02});
    REQUIRE_EQ(control.payload_recovery[2],
        std::byte{0x03});
    REQUIRE_EQ(control.payload_recovery[3],
        std::byte{0});

    std::array<std::byte, 24> datagram{};
    const auto encoded = encode_packet({
        .kind = PacketKind::data,
        .data = packet->header,
        .payload = packet->payload,
    }, datagram);
    REQUIRE(encoded);
    const std::array expected{
        std::byte{0x00}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x67},
        std::byte{0xd0}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x22},
        std::byte{0x00}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x4d},
        std::byte{0x00}, std::byte{0x03},
        std::byte{0x00}, std::byte{0x01},
        std::byte{0x11}, std::byte{0x22},
        std::byte{0x03}, std::byte{0x00},
    };
    REQUIRE_EQ(datagram, expected);
}

TEST(column_fec_encoder_abandons_a_series_with_a_dropped_source)
{
    // A source dropped before transmission leaves a gap in its column. The
    // encoder must skip that series (no parity) instead of returning a fatal
    // error that would tear the connection down, and must recover on the next
    // series. Even layout, columns 3 x rows 2: column c holds indices c and
    // c + 3. Dropping sequence 100 (column 0, position 0) leaves column 0 of
    // the first series with a gap at its second source, sequence 103.
    const auto configuration = column_configuration(3U, 2U);
    ColumnFecEncoder encoder {configuration, SequenceNumber {100}, 1U};
    const std::array payload {std::byte {'x'}};
    std::vector<std::int8_t> first_series_columns;

    // First series: sequence 100 is never fed (dropped before transmission).
    for (std::uint32_t sequence = 101U; sequence <= 105U; ++sequence) {
        REQUIRE_EQ(encoder.feed_source(source_packet(
                       sequence, sequence, EncryptionKey::none, payload)),
            Error::none);
        const auto packet = encoder.control_packet();
        if (packet.has_value()) {
            const auto control = decode_fec_control_payload(packet->payload);
            REQUIRE(control);
            first_series_columns.push_back(control.header.group_index);
            encoder.consume_control_packet();
        }
    }
    // Columns 1 and 2 completed; column 0 was abandoned, so it emits nothing.
    const std::vector<std::int8_t> expected_first {1, 2};
    REQUIRE_EQ(first_series_columns, expected_first);

    // Second series (sequences 106..111) is complete; every column recovers.
    std::vector<std::int8_t> second_series_columns;
    for (std::uint32_t sequence = 106U; sequence <= 111U; ++sequence) {
        REQUIRE_EQ(encoder.feed_source(source_packet(
                       sequence, sequence, EncryptionKey::none, payload)),
            Error::none);
        const auto packet = encoder.control_packet();
        if (packet.has_value()) {
            const auto control = decode_fec_control_payload(packet->payload);
            REQUIRE(control);
            second_series_columns.push_back(control.header.group_index);
            encoder.consume_control_packet();
        }
    }
    const std::vector<std::int8_t> expected_second {0, 1, 2};
    REQUIRE_EQ(second_series_columns, expected_second);
}

TEST(column_fec_encoder_uses_staircase_control_order)
{
    const auto configuration =
        column_configuration(
            4U, 2U, "staircase");
    ColumnFecEncoder encoder{
        configuration, SequenceNumber{200}, 1U};
    const std::array payload{std::byte{'x'}};
    std::vector<std::pair<
        SequenceNumber, std::int8_t>> controls;

    for (std::uint32_t index = 0;
         index < 12U; ++index) {
        REQUIRE_EQ(encoder.feed_source(source_packet(
                       200U + index,
                       index,
                       EncryptionKey::none,
                       payload)),
            Error::none);
        const auto packet = encoder.control_packet();
        if (!packet.has_value()) {
            continue;
        }
        const auto control =
            decode_fec_control_payload(
                packet->payload);
        REQUIRE(control);
        controls.emplace_back(
            packet->header.sequence,
            control.header.group_index);
        encoder.consume_control_packet();
    }

    const std::vector<std::pair<
        SequenceNumber, std::int8_t>> expected{
        {SequenceNumber{204}, 0},
        {SequenceNumber{206}, 2},
        {SequenceNumber{209}, 1},
        {SequenceNumber{211}, 3},
    };
    REQUIRE_EQ(controls, expected);
}

TEST(column_fec_decoder_reconstructs_control_first_across_rollover)
{
    const auto configuration =
        column_configuration(2U, 2U);
    constexpr std::uint32_t initial =
        SequenceNumber::mask - 1U;
    ColumnFecEncoder encoder{
        configuration, SequenceNumber{initial}, 2U};
    ColumnFecDecoder decoder{
        configuration, SequenceNumber{initial}, 8U, 2U};
    const std::array missing{
        std::byte{'A'}, std::byte{'B'}};
    const std::array middle{std::byte{'m'}};
    const std::array survivor{std::byte{'Z'}};
    const auto missing_packet = source_packet(
        initial, 10U, EncryptionKey::even,
        missing, true);
    const auto middle_packet = source_packet(
        SequenceNumber::mask, 20U,
        EncryptionKey::none, middle, true);
    const auto survivor_packet = source_packet(
        0U, 30U, EncryptionKey::odd,
        survivor, true);
    REQUIRE_EQ(
        encoder.feed_source(missing_packet),
        Error::none);
    REQUIRE_EQ(
        encoder.feed_source(middle_packet),
        Error::none);
    REQUIRE_EQ(
        encoder.feed_source(survivor_packet),
        Error::none);
    const auto control = encoder.control_packet();
    REQUIRE(control.has_value());

    const auto control_first = decoder.receive({
        .kind = PacketKind::data,
        .data = control->header,
        .payload = control->payload,
    });
    REQUIRE(control_first);
    REQUIRE(control_first.consume_control_packet);
    REQUIRE(!control_first.has_reconstructed_packet);
    const auto rebuilt =
        decoder.receive(survivor_packet);
    REQUIRE(rebuilt);
    REQUIRE(rebuilt.has_reconstructed_packet);
    REQUIRE_EQ(
        rebuilt.reconstructed_packet.data.sequence,
        SequenceNumber{initial});
    REQUIRE_EQ(
        rebuilt.reconstructed_packet.data.encryption_key,
        EncryptionKey::even);
    REQUIRE_EQ(
        rebuilt.reconstructed_packet.data.timestamp,
        PacketTimestamp{10U});
    REQUIRE(std::equal(
        rebuilt.reconstructed_packet.payload.begin(),
        rebuilt.reconstructed_packet.payload.end(),
        missing.begin()));
    REQUIRE(!decoder.receive(survivor_packet)
                 .has_reconstructed_packet);
}

TEST(column_fec_decoder_expires_even_groups_in_sequence_order)
{
    const auto configuration =
        column_configuration(
            3U, 2U, "even", "onreq");
    ColumnFecDecoder decoder{
        configuration, SequenceNumber{100}, 32U, 1U};
    const std::array payload{std::byte{'x'}};

    for (const std::uint32_t sequence :
         {101U, 103U, 104U}) {
        REQUIRE(decoder.receive(source_packet(
            sequence, sequence,
            EncryptionKey::none, payload)));
    }
    const auto expired = decoder.receive(
        source_packet(
            106U, 106U,
            EncryptionKey::none, payload));
    REQUIRE(expired);
    REQUIRE_EQ(
        expired.irrecoverable_losses.size(), 3U);
    REQUIRE_EQ(
        expired.irrecoverable_losses[0].first,
        SequenceNumber{100});
    REQUIRE_EQ(
        expired.irrecoverable_losses[1].first,
        SequenceNumber{102});
    REQUIRE_EQ(
        expired.irrecoverable_losses[2].first,
        SequenceNumber{105});
    for (const auto& loss :
         expired.irrecoverable_losses) {
        REQUIRE_EQ(loss.first, loss.last);
    }
}

TEST(column_fec_decoder_expires_staircase_columns_incrementally)
{
    const auto configuration =
        column_configuration(
            4U, 2U, "staircase", "onreq");
    ColumnFecDecoder decoder{
        configuration, SequenceNumber{400}, 32U, 1U};
    const std::array payload{std::byte{'x'}};

    const auto first_expiry = decoder.receive(
        source_packet(
            408U, 1U,
            EncryptionKey::none, payload));
    REQUIRE(first_expiry);
    const std::array first_expected{
        SequenceNumber{400}, SequenceNumber{402},
        SequenceNumber{404}, SequenceNumber{406}};
    REQUIRE_EQ(
        first_expiry.irrecoverable_losses.size(),
        first_expected.size());
    for (std::size_t index = 0;
         index < first_expected.size(); ++index) {
        REQUIRE_EQ(
            first_expiry
                .irrecoverable_losses[index].first,
            first_expected[index]);
    }

    const auto second_expiry = decoder.receive(
        source_packet(
            410U, 2U,
            EncryptionKey::none, payload));
    REQUIRE(second_expiry);
    REQUIRE_EQ(
        second_expiry.irrecoverable_losses.size(),
        2U);
    REQUIRE_EQ(
        second_expiry.irrecoverable_losses[0].first,
        SequenceNumber{405});
    REQUIRE_EQ(
        second_expiry.irrecoverable_losses[1].first,
        SequenceNumber{409});
}

TEST(column_fec_decoder_coalesces_expired_matrix_across_rollover)
{
    const auto configuration =
        column_configuration(
            2U, 2U, "even", "onreq");
    constexpr std::uint32_t initial =
        SequenceNumber::mask - 1U;
    ColumnFecDecoder decoder{
        configuration, SequenceNumber{initial}, 8U, 1U};
    const std::array payload{std::byte{'x'}};

    const auto expired = decoder.receive(
        source_packet(
            2U, 1U,
            EncryptionKey::none, payload));
    REQUIRE(expired);
    REQUIRE_EQ(
        expired.irrecoverable_losses.size(), 1U);
    REQUIRE_EQ(
        expired.irrecoverable_losses[0].first,
        SequenceNumber{initial});
    REQUIRE_EQ(
        expired.irrecoverable_losses[0].last,
        SequenceNumber{1});
}

TEST(fec_encoders_resume_exact_parity_after_a_gap_inside_a_column)
{
    // 3 x 3 groups: column zero starts with index 0, then index 3 is
    // dropped. Index 6 must abandon that partial XOR. The next series
    // (9, 12, 15) must emit fresh parity, without any bytes from index 0.
    for (const std::string_view layout : {"even", "staircase"}) {
        const bool staircase = layout == "staircase";
        const auto verify = [staircase](auto& encoder, bool matrix) {
            using Group = std::pair<std::int8_t, std::vector<std::uint32_t>>;
            std::vector<Group> expected = staircase
                ? std::vector<Group> {{1, {4, 7, 10}}, {2, {8, 11, 14}},
                      {0, {9, 12, 15}}, {1, {13, 16, 19}}, {2, {17, 20, 23}}}
                : std::vector<Group> {{1, {1, 4, 7}}, {2, {2, 5, 8}},
                      {0, {9, 12, 15}}, {1, {10, 13, 16}}, {2, {11, 14, 17}}};
            const std::uint32_t count = staircase ? 24U : 18U;
            if (matrix) {
                for (std::uint32_t first = 0; first < count; first += 3U) {
                    if (first != 3U) {
                        expected.push_back(
                            {-1, {first, first + 1U, first + 2U}});
                    }
                }
            }
            const auto payload_for = [](std::uint32_t index) {
                return std::array {static_cast<std::byte>(index + 1U),
                    static_cast<std::byte>(index * 7U + 3U)};
            };
            for (std::uint32_t index = 0; index < count; ++index) {
                if (index == 3U) {
                    continue;
                }
                const auto payload = payload_for(index);
                const auto length = 1U + index % 2U;
                REQUIRE_EQ(encoder.feed_source(source_packet(200U + index,
                               1000U + index * 13U, EncryptionKey::even,
                               std::span {payload}.first(length))),
                    Error::none);
                while (encoder.control_packet_ready()) {
                    const auto packet = encoder.control_packet();
                    REQUIRE(packet.has_value());
                    const auto control =
                        decode_fec_control_payload(packet->payload);
                    REQUIRE(control);
                    const auto found = std::find_if(expected.begin(),
                        expected.end(), [&](const Group& group) {
                            return group.first == control.header.group_index
                                && packet->header.sequence
                                == SequenceNumber {200U + group.second.back()};
                        });
                    // Reject parity for the incomplete group, extra packets,
                    // and duplicates as well as missing parity after recovery.
                    REQUIRE(found != expected.end());
                    std::array<std::byte, 2> expected_payload {};
                    std::uint16_t expected_length = 0;
                    std::uint32_t expected_timestamp = 0;
                    for (const auto source : found->second) {
                        const auto bytes = payload_for(source);
                        const auto source_length = 1U + source % 2U;
                        expected_length ^=
                            static_cast<std::uint16_t>(source_length);
                        expected_timestamp ^= 1000U + source * 13U;
                        for (std::size_t byte = 0; byte < source_length;
                            ++byte) {
                            expected_payload[byte] ^= bytes[byte];
                        }
                    }
                    REQUIRE_EQ(control.header.flags_recovery, 1U);
                    REQUIRE_EQ(control.header.length_recovery, expected_length);
                    REQUIRE_EQ(packet->header.timestamp,
                        PacketTimestamp {expected_timestamp});
                    REQUIRE_EQ(control.payload_recovery.size(),
                        expected_payload.size());
                    REQUIRE(std::equal(control.payload_recovery.begin(),
                        control.payload_recovery.end(),
                        expected_payload.begin()));
                    expected.erase(found);
                    encoder.consume_control_packet();
                }
            }
            REQUIRE(expected.empty());
        };
        ColumnFecEncoder column {
            column_configuration(3U, 3U, layout), SequenceNumber {200}, 2U};
        verify(column, false);
        MatrixFecEncoder matrix {
            matrix_configuration(3U, 3U, layout), SequenceNumber {200}, 2U};
        verify(matrix, true);
    }
}

TEST(matrix_fec_encoder_sends_columns_before_a_ready_row)
{
    const auto configuration =
        matrix_configuration(2U, 2U);
    MatrixFecEncoder encoder{
        configuration, SequenceNumber{200}, 1U};
    const std::array payload{std::byte{'x'}};
    std::vector<std::pair<
        SequenceNumber, std::int8_t>> controls;

    for (std::uint32_t index = 0;
         index < 4U; ++index) {
        REQUIRE_EQ(encoder.feed_source(source_packet(
                       200U + index,
                       index,
                       EncryptionKey::none,
                       payload)),
            Error::none);
        while (encoder.control_packet_ready()) {
            const auto packet =
                encoder.control_packet();
            REQUIRE(packet.has_value());
            const auto control =
                decode_fec_control_payload(
                    packet->payload);
            REQUIRE(control);
            controls.emplace_back(
                packet->header.sequence,
                control.header.group_index);
            encoder.consume_control_packet();
        }
    }

    const std::vector<std::pair<
        SequenceNumber, std::int8_t>> expected{
        {SequenceNumber{201}, -1},
        {SequenceNumber{202}, 0},
        {SequenceNumber{203}, 1},
        {SequenceNumber{203}, -1},
    };
    REQUIRE_EQ(controls, expected);
}

TEST(matrix_fec_decoder_recursively_rebuilds_across_dimensions)
{
    const auto configuration =
        matrix_configuration(
            3U, 3U, "even", "onreq");
    constexpr SequenceNumber initial{
        SequenceNumber::mask - 4U};
    MatrixFecEncoder encoder{
        configuration, initial, 1U};
    MatrixFecDecoder decoder{
        configuration, initial, 32U, 1U};
    std::array<std::array<std::byte, 1>, 9>
        payloads{};
    for (std::size_t index = 0;
         index < payloads.size(); ++index) {
        payloads[index][0] =
            static_cast<std::byte>(
                static_cast<unsigned char>('A')
                + index);
        REQUIRE_EQ(encoder.feed_source(source_packet(
                       initial.advanced(
                           static_cast<std::uint32_t>(
                               index))
                           .value(),
                       static_cast<std::uint32_t>(
                           index),
                       EncryptionKey::none,
                       payloads[index])),
            Error::none);
        while (encoder.control_packet_ready()) {
            const auto control =
                encoder.control_packet();
            REQUIRE(control.has_value());
            const auto accepted = decoder.receive({
                .kind = PacketKind::data,
                .data = control->header,
                .payload = control->payload,
            });
            REQUIRE(accepted);
            REQUIRE(
                accepted.consume_control_packet);
            REQUIRE(
                accepted.reconstructed_packets
                    .empty());
            encoder.consume_control_packet();
        }
    }

    for (const std::uint32_t index :
         {2U, 4U, 6U}) {
        const auto accepted = decoder.receive(
            source_packet(
                initial.advanced(index).value(),
                index,
                EncryptionKey::none,
                payloads[index]));
        REQUIRE(accepted);
        REQUIRE(
            accepted.reconstructed_packets.empty());
    }

    const auto rebuilt = decoder.receive(
        source_packet(
            initial.advanced(7U).value(), 7U,
            EncryptionKey::none,
            payloads[7]));
    REQUIRE(rebuilt);
    REQUIRE_EQ(
        rebuilt.reconstructed_packets.size(), 5U);
    const std::array expected_indices{
        8U, 5U, 3U, 0U, 1U};
    for (std::size_t index = 0;
         index < expected_indices.size(); ++index) {
        const auto expected =
            expected_indices[index];
        REQUIRE_EQ(
            rebuilt.reconstructed_packets[index]
                .data.sequence,
            initial.advanced(expected));
        REQUIRE_EQ(
            rebuilt.reconstructed_packets[index]
                .payload[0],
            payloads[expected][0]);
    }
    REQUIRE(
        rebuilt.irrecoverable_losses.empty());
}

TEST(matrix_fec_decoder_uses_column_expiry_for_onreq_losses)
{
    const auto configuration =
        matrix_configuration(
            2U, 2U, "even", "onreq");
    MatrixFecDecoder decoder{
        configuration, SequenceNumber{300}, 16U, 1U};
    const std::array payload{std::byte{'x'}};

    const auto expired = decoder.receive(
        source_packet(
            304U, 1U, EncryptionKey::none,
            payload));
    REQUIRE(expired);
    REQUIRE_EQ(
        expired.irrecoverable_losses.size(), 1U);
    REQUIRE_EQ(
        expired.irrecoverable_losses[0].first,
        SequenceNumber{300});
    REQUIRE_EQ(
        expired.irrecoverable_losses[0].last,
        SequenceNumber{303});
}

TEST(fec_decoder_resource_estimates_follow_local_limits)
{
    constexpr std::size_t receive_capacity = 64U;
    constexpr std::size_t payload_size = 1'312U;
    const auto row = row_configuration(8U);
    const auto column = column_configuration(8U, 4U, "staircase", "onreq");
    const auto matrix = matrix_configuration(8U, 4U, "staircase", "onreq");

    const auto row_estimate =
        RowFecDecoder::estimate_resources(row, receive_capacity, payload_size);
    const auto column_estimate = ColumnFecDecoder::estimate_resources(
        column, receive_capacity, payload_size);
    const auto matrix_estimate = MatrixFecDecoder::estimate_resources(
        matrix, receive_capacity, payload_size);
    REQUIRE(row_estimate);
    REQUIRE(column_estimate);
    REQUIRE(matrix_estimate);
    REQUIRE_EQ(row_estimate.usage.group_slots, 11U);
    REQUIRE_EQ(column_estimate.usage.group_slots, 40U);
    REQUIRE_EQ(matrix_estimate.usage.group_slots,
        row_estimate.usage.group_slots + column_estimate.usage.group_slots);
    REQUIRE(
        matrix_estimate.usage.dynamic_bytes > row_estimate.usage.dynamic_bytes);
    REQUIRE(matrix_estimate.usage.dynamic_bytes
        > column_estimate.usage.dynamic_bytes);
    REQUIRE(matrix_estimate.usage.reconstruction_bytes
        >= receive_capacity * payload_size);
    REQUIRE(matrix_estimate.usage.dynamic_bytes < 32U * 1'024U * 1'024U);

    const auto largest_geometry =
        matrix_configuration(128U, 2U, "staircase", "onreq");
    const auto largest_estimate = MatrixFecDecoder::estimate_resources(
        largest_geometry, 8'192U, payload_size);
    REQUIRE(largest_estimate);
    REQUIRE(largest_estimate.usage.dynamic_bytes < 64U * 1'024U * 1'024U);

    RowFecDecoder row_decoder {
        row, SequenceNumber {0}, receive_capacity, payload_size};
    ColumnFecDecoder column_decoder {
        column, SequenceNumber {0}, receive_capacity, payload_size};
    MatrixFecDecoder matrix_decoder {
        matrix, SequenceNumber {0}, receive_capacity, payload_size};
    REQUIRE(row_decoder.resource_usage().dynamic_bytes
        >= row_estimate.usage.dynamic_bytes);
    REQUIRE(column_decoder.resource_usage().dynamic_bytes
        >= column_estimate.usage.dynamic_bytes);
    REQUIRE(matrix_decoder.resource_usage().dynamic_bytes
        >= matrix_estimate.usage.dynamic_bytes);
}

TEST(fec_decoder_resource_estimates_reject_invalid_boundaries)
{
    const auto row = row_configuration(8U);
    const auto column = column_configuration(8U, 4U);
    const auto matrix = matrix_configuration(8U, 4U);

    REQUIRE_EQ(RowFecDecoder::estimate_resources(row, 7U, 1'312U).error,
        Error::invalid_state);
    REQUIRE_EQ(ColumnFecDecoder::estimate_resources(column, 31U, 1'312U).error,
        Error::invalid_state);
    REQUIRE_EQ(MatrixFecDecoder::estimate_resources(matrix, 31U, 1'312U).error,
        Error::invalid_state);
    REQUIRE_EQ(MatrixFecDecoder::estimate_resources(matrix, 64U, 0U).error,
        Error::invalid_state);
    REQUIRE_EQ(MatrixFecDecoder::estimate_resources(
                   matrix, 64U, maximum_data_payload_size)
                   .error,
        Error::invalid_state);
    REQUIRE_EQ(MatrixFecDecoder::estimate_resources(row, 64U, 1'312U).error,
        Error::invalid_state);
}

TEST(fec_decoder_owned_storage_is_stable_under_wire_input)
{
    const std::array layouts {
        std::string_view {"even"},
        std::string_view {"staircase"},
    };
    std::array<std::array<std::byte, 4>, 36> payloads {};
    for (std::size_t index = 0; index < payloads.size(); ++index) {
        payloads[index][0] = static_cast<std::byte>(index & 0xffU);
        payloads[index][1] = static_cast<std::byte>((index * 3U) & 0xffU);
    }

    for (const auto layout : layouts) {
        const auto configuration =
            matrix_configuration(3U, 3U, layout, "onreq");
        MatrixFecEncoder encoder {configuration, SequenceNumber {500}, 4U};
        MatrixFecDecoder decoder {configuration, SequenceNumber {500}, 64U, 4U};
        const auto before = decoder.resource_usage();

        for (std::size_t index = 0; index < payloads.size(); ++index) {
            const auto packet =
                source_packet(500U + static_cast<std::uint32_t>(index),
                    static_cast<std::uint32_t>(index * 10U),
                    EncryptionKey::none, payloads[index]);
            REQUIRE_EQ(encoder.feed_source(packet), Error::none);
            const auto accepted = decoder.receive(packet);
            REQUIRE(accepted);
            const auto duplicate = decoder.receive(packet);
            REQUIRE(duplicate);
            while (encoder.control_packet_ready()) {
                const auto control = encoder.control_packet();
                REQUIRE(control.has_value());
                const PacketView control_view {
                    .kind = PacketKind::data,
                    .data = control->header,
                    .payload = control->payload,
                };
                REQUIRE(decoder.receive(control_view));
                REQUIRE(decoder.receive(control_view));
                encoder.consume_control_packet();
            }
        }

        const std::array malformed {
            std::byte {0x7f},
            std::byte {0xff},
            std::byte {0xff},
            std::byte {0xff},
            std::byte {0xaa},
        };
        const auto rejected = decoder.receive({
            .kind = PacketKind::data,
            .data =
                {
                    .sequence = SequenceNumber {535},
                    .message_number = 0,
                    .boundary = MessageBoundary::solo,
                    .destination_socket_id = 77,
                },
            .payload = malformed,
        });
        REQUIRE(!rejected);
        REQUIRE_EQ(before, decoder.resource_usage());
    }
}

TEST(fec_row_resynchronizes_after_receive_window_gap)
{
    const std::array modes {"always", "onreq", "never"};
    const std::array<std::byte, 1> payload {std::byte {1}};
    for (const auto* mode : modes) {
        for (const std::uint32_t initial : {0U, SequenceNumber::mask - 8U}) {
            const auto config = row_configuration(3U, mode);
            const SequenceNumber start {initial};
            const auto sequence = [&](std::uint32_t offset) {
                return start.advanced(offset);
            };
            RowFecDecoder decoder {config, start, 16U, 4U};
            RowFecEncoder encoder {config, sequence(18U), 4U};
            REQUIRE(decoder.receive(
                source_packet(initial, 0U, EncryptionKey::none, payload),
                start));

            for (std::uint32_t index = 18U; index < 21U; ++index) {
                const auto source = source_packet(sequence(index).value(),
                    index, EncryptionKey::none, payload);
                REQUIRE_EQ(encoder.feed_source(source), Error::none);
            }
            const auto parity = encoder.control_packet();
            REQUIRE(parity.has_value());
            const PacketView control {
                .kind = PacketKind::data,
                .data = parity->header,
                .payload = parity->payload,
            };
            REQUIRE(!decoder.receive(control, sequence(18U)));
            REQUIRE(decoder.receive(source_packet(sequence(18U).value(), 18U,
                                        EncryptionKey::none, payload),
                sequence(18U)));
            REQUIRE(decoder.receive(source_packet(sequence(20U).value(), 20U,
                                        EncryptionKey::none, payload),
                sequence(18U)));
            const auto rebuilt = decoder.receive(control, sequence(18U));
            REQUIRE(rebuilt);
            REQUIRE(rebuilt.has_reconstructed_packet);
            REQUIRE_EQ(
                rebuilt.reconstructed_packet.data.sequence, sequence(19U));
        }
    }
}

TEST(fec_column_and_matrix_resynchronize_after_receive_window_gap)
{
    const std::array modes {"always", "onreq", "never"};
    const std::array<std::byte, 1> payload {std::byte {1}};
    for (const auto* mode : modes) {
        for (const auto* layout : {"even", "staircase"}) {
            const SequenceNumber initial {SequenceNumber::mask - 8U};
            const auto sequence = [&](std::uint32_t offset) {
                return initial.advanced(offset);
            };
            const auto config = column_configuration(2U, 2U, layout, mode);
            ColumnFecDecoder decoder {config, initial, 16U, 4U};
            ColumnFecEncoder encoder {config, sequence(16U), 4U};
            REQUIRE(decoder.receive(source_packet(initial.value(), 0U,
                                        EncryptionKey::none, payload),
                initial));
            for (std::uint32_t index = 16U; index < 20U; ++index) {
                const auto source = source_packet(sequence(index).value(),
                    index, EncryptionKey::none, payload);
                REQUIRE_EQ(encoder.feed_source(source), Error::none);
                if (index != 18U) {
                    REQUIRE(decoder.receive(source, sequence(16U)));
                }
                if (encoder.control_packet_ready()) {
                    const auto parity = encoder.control_packet();
                    REQUIRE(parity.has_value());
                    const PacketView control {
                        .kind = PacketKind::data,
                        .data = parity->header,
                        .payload = parity->payload,
                    };
                    const auto rebuilt =
                        decoder.receive(control, sequence(16U));
                    REQUIRE(rebuilt);
                    if (parity->header.sequence == sequence(18U)) {
                        REQUIRE(rebuilt.has_reconstructed_packet);
                        REQUIRE_EQ(rebuilt.reconstructed_packet.data.sequence,
                            sequence(18U));
                    }
                    encoder.consume_control_packet();
                }
            }

            const auto matrix_config =
                matrix_configuration(3U, 2U, layout, mode);
            MatrixFecDecoder matrix {matrix_config, initial, 16U, 4U};
            MatrixFecEncoder matrix_encoder {matrix_config, sequence(18U), 4U};
            REQUIRE(matrix.receive(source_packet(initial.value(), 0U,
                                       EncryptionKey::none, payload),
                initial));
            for (std::uint32_t index = 18U; index < 21U; ++index) {
                const auto source = source_packet(sequence(index).value(),
                    index, EncryptionKey::none, payload);
                REQUIRE_EQ(matrix_encoder.feed_source(source), Error::none);
                if (index != 19U) {
                    REQUIRE(matrix.receive(source, sequence(18U)));
                }
            }
            const auto parity = matrix_encoder.control_packet();
            REQUIRE(parity.has_value());
            const PacketView control {
                .kind = PacketKind::data,
                .data = parity->header,
                .payload = parity->payload,
            };
            const auto rebuilt = matrix.receive(control, sequence(18U));
            REQUIRE(rebuilt);
            REQUIRE_EQ(rebuilt.reconstructed_packets.size(), 1U);
            REQUIRE_EQ(
                rebuilt.reconstructed_packets[0].data.sequence, sequence(19U));
        }
    }
}

TEST(fec_control_and_out_of_window_source_cannot_resynchronize)
{
    const auto config = row_configuration(3U);
    const std::array<std::byte, 1> payload {std::byte {1}};
    RowFecDecoder decoder {config, SequenceNumber {0}, 16U, 4U};
    RowFecEncoder old_encoder {config, SequenceNumber {0}, 4U};
    RowFecEncoder new_encoder {config, SequenceNumber {18}, 4U};
    for (std::uint32_t index = 0; index < 3U; ++index) {
        const auto source =
            source_packet(index, index, EncryptionKey::none, payload);
        REQUIRE_EQ(old_encoder.feed_source(source), Error::none);
        if (index != 1U) {
            REQUIRE(decoder.receive(source, SequenceNumber {0}));
        }
    }
    for (std::uint32_t index = 18U; index < 21U; ++index) {
        REQUIRE_EQ(new_encoder.feed_source(source_packet(
                       index, index, EncryptionKey::none, payload)),
            Error::none);
    }
    const auto new_parity = new_encoder.control_packet();
    REQUIRE(new_parity.has_value());
    const PacketView far_control {
        .kind = PacketKind::data,
        .data = new_parity->header,
        .payload = new_parity->payload,
    };
    REQUIRE(!decoder.receive(far_control, SequenceNumber {0}));
    REQUIRE(
        decoder.receive(source_packet(18U, 18U, EncryptionKey::none, payload),
            SequenceNumber {0}));

    const auto old_parity = old_encoder.control_packet();
    REQUIRE(old_parity.has_value());
    const PacketView old_control {
        .kind = PacketKind::data,
        .data = old_parity->header,
        .payload = old_parity->payload,
    };
    const auto rebuilt = decoder.receive(old_control, SequenceNumber {0});
    REQUIRE(rebuilt);
    REQUIRE(rebuilt.has_reconstructed_packet);
    REQUIRE_EQ(rebuilt.reconstructed_packet.data.sequence, SequenceNumber {1});
    const auto late_control = decoder.receive(old_control, SequenceNumber {3});
    REQUIRE(late_control);
    REQUIRE(late_control.consume_control_packet);
    REQUIRE(!late_control.has_reconstructed_packet);
}

TEST(fec_resynchronization_reports_only_losses_at_receive_floor)
{
    const auto config = row_configuration(3U, "onreq");
    const std::array<std::byte, 1> payload {std::byte {1}};
    RowFecDecoder decoder {config, SequenceNumber {0}, 16U, 4U};
    REQUIRE(decoder.receive(source_packet(0U, 0U, EncryptionKey::none, payload),
        SequenceNumber {0}));
    REQUIRE(
        decoder.receive(source_packet(18U, 18U, EncryptionKey::none, payload),
            SequenceNumber {17}));
    const auto advanced =
        decoder.receive(source_packet(20U, 20U, EncryptionKey::none, payload),
            SequenceNumber {17});
    REQUIRE(advanced);
    REQUIRE_EQ(advanced.irrecoverable_losses.size(), 1U);
    REQUIRE_EQ(advanced.irrecoverable_losses[0].first, SequenceNumber {17});
    REQUIRE_EQ(advanced.irrecoverable_losses[0].last, SequenceNumber {17});
}

TEST(row_only_fec_rejects_oversized_geometry_before_allocation)
{
    auto configuration = row_configuration(128U);
    REQUIRE(RowFecDecoder::estimate_resources(configuration, 8192, 1312));
    for (const auto columns : {129U, 8192U, 2147483647U}) {
        configuration.columns = columns;
        REQUIRE(!supports_row_only_fec(configuration));
        REQUIRE(!RowFecDecoder::estimate_resources(configuration, 8192, 1312));
        bool rejected = false;
        try {
            RowFecDecoder decoder {
                configuration, SequenceNumber {0}, 8192, 1312};
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        REQUIRE(rejected);
    }
}
