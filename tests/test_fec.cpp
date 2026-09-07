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
