#include "test.hpp"

#include "robotweax/srt/receive_buffer.hpp"

#include <array>
#include <cstddef>

using namespace robotweax::srt;

namespace {

PacketView data_packet(SequenceNumber sequence, std::uint32_t message_number,
    MessageBoundary boundary, std::span<const std::byte> payload)
{
    PacketView packet;
    packet.kind = PacketKind::data;
    packet.data.sequence = sequence;
    packet.data.message_number = message_number;
    packet.data.boundary = boundary;
    packet.data.timestamp = PacketTimestamp{77};
    packet.payload = payload;
    return packet;
}

} // namespace

TEST(receive_buffer_empty_readiness_is_non_mutating_at_all_capacities)
{
    for (const std::size_t capacity : {1U, 8U, 16'384U}) {
        const SequenceNumber initial {SequenceNumber::mask};
        ReceiveBuffer buffer {initial, capacity};
        for (int query = 0; query < 4; ++query) {
            REQUIRE(!buffer.first_complete_message());
            REQUIRE(!buffer.has_complete_message());
            REQUIRE(!buffer.has_stream_data());
            REQUIRE_EQ(buffer.occupied(), 0U);
            REQUIRE_EQ(buffer.available(), capacity);
            REQUIRE_EQ(buffer.first_stored_sequence(), initial);
            REQUIRE_EQ(buffer.next_ack_sequence(), initial);
        }
    }
}

TEST(
    receive_buffer_empty_fragmented_empty_queries_survive_sequence_and_ring_wrap)
{
    ReceiveBuffer buffer {SequenceNumber {SequenceNumber::mask - 1U}, 8};
    const std::array<std::byte, 1> payload {std::byte {'x'}};
    for (std::uint32_t message = 1; message <= 4; ++message) {
        const auto first = buffer.first_stored_sequence();
        REQUIRE(!buffer.first_complete_message());
        // Out-of-order fragments are occupied, but not yet a complete message.
        REQUIRE(buffer.insert(data_packet(
            first.advanced(2), message, MessageBoundary::last, payload)));
        REQUIRE(!buffer.first_complete_message());
        REQUIRE(buffer.insert(
            data_packet(first, message, MessageBoundary::first, payload)));
        REQUIRE(!buffer.first_complete_message());
        REQUIRE(buffer.insert(data_packet(
            first.next(), message, MessageBoundary::subsequent, payload)));
        const auto complete = buffer.first_complete_message();
        REQUIRE(complete);
        REQUIRE_EQ(complete->first_sequence, first);
        REQUIRE_EQ(complete->last_sequence, first.advanced(2));
        REQUIRE(buffer.has_complete_message());
        REQUIRE_EQ(buffer.occupied(), 3U);
        std::array<std::byte, 3> output {};
        REQUIRE_EQ(buffer.pop_message(output).bytes_written, 3U);
        REQUIRE_EQ(output,
            (std::array<std::byte, 3> {payload[0], payload[0], payload[0]}));
        REQUIRE_EQ(buffer.occupied(), 0U);
        REQUIRE(!buffer.first_complete_message());
        REQUIRE_EQ(buffer.next_ack_sequence(), first.advanced(3));
    }
}

TEST(receive_buffer_empty_readiness_preserves_drop_markers_and_reuse)
{
    ReceiveBuffer buffer {SequenceNumber {100}, 8};
    const std::array<std::byte, 1> payload {std::byte {'d'}};
    REQUIRE(buffer.insert(
        data_packet(SequenceNumber {102}, 1, MessageBoundary::solo, payload)));
    REQUIRE_EQ(buffer.drop_range({SequenceNumber {101}, SequenceNumber {102}}),
        Error::none);
    // Interior tombstones are not occupied payload, but must remain in place.
    REQUIRE_EQ(buffer.occupied(), 0U);
    REQUIRE(!buffer.first_complete_message());
    REQUIRE_EQ(buffer.first_stored_sequence(), SequenceNumber {100});
    REQUIRE_EQ(buffer.next_ack_sequence(), SequenceNumber {100});
    REQUIRE_EQ(buffer
                   .insert(data_packet(
                       SequenceNumber {102}, 1, MessageBoundary::solo, payload))
                   .status,
        ReceiveStatus::duplicate);
    REQUIRE(buffer.insert(
        data_packet(SequenceNumber {100}, 2, MessageBoundary::solo, payload)));
    REQUIRE(buffer.has_complete_message());
    REQUIRE_EQ(buffer.next_ack_sequence(), SequenceNumber {103});
    std::array<std::byte, 1> output {};
    REQUIRE(buffer.pop_message(output));
    REQUIRE(!buffer.first_complete_message());
    REQUIRE_EQ(buffer.first_stored_sequence(), SequenceNumber {103});
    REQUIRE(buffer.insert(
        data_packet(SequenceNumber {103}, 3, MessageBoundary::solo, payload)));
    REQUIRE(buffer.has_complete_message());
    REQUIRE(buffer.pop_message(output));
    REQUIRE(!buffer.first_complete_message());
}

TEST(receive_buffer_empty_readiness_after_discard_and_partial_stream_drain)
{
    ReceiveBuffer buffer {SequenceNumber {SequenceNumber::mask}, 4};
    const std::array<std::byte, 2> payload {std::byte {'a'}, std::byte {'b'}};
    REQUIRE(buffer.insert(
        data_packet(SequenceNumber {0}, 1, MessageBoundary::solo, payload)));
    REQUIRE_EQ(buffer.discard_before(SequenceNumber {1}), Error::none);
    REQUIRE_EQ(buffer.occupied(), 0U);
    REQUIRE(!buffer.first_complete_message());
    REQUIRE(buffer.insert(
        data_packet(SequenceNumber {1}, 2, MessageBoundary::solo, payload)));
    std::array<std::byte, 1> output {};
    REQUIRE_EQ(buffer.pop_stream(output).bytes_written, 1U);
    REQUIRE_EQ(output[0], payload[0]);
    REQUIRE_EQ(buffer.occupied(), 1U);
    REQUIRE(buffer.first_complete_message());
    REQUIRE_EQ(buffer.pop_stream(output).bytes_written, 1U);
    REQUIRE_EQ(output[0], payload[1]);
    REQUIRE_EQ(buffer.occupied(), 0U);
    REQUIRE(!buffer.first_complete_message());
    REQUIRE(!buffer.has_stream_data());
}

TEST(receive_buffer_empty_payload_is_not_an_empty_buffer)
{
    ReceiveBuffer buffer {SequenceNumber {10}, 1};
    REQUIRE(buffer.insert(
        data_packet(SequenceNumber {10}, 1, MessageBoundary::solo, {})));
    REQUIRE_EQ(buffer.buffered_payload_bytes(), 0U);
    REQUIRE_EQ(buffer.occupied(), 1U);
    REQUIRE(buffer.first_complete_message());
    REQUIRE(buffer.has_complete_message());
    REQUIRE(buffer.pop_message({}));
    REQUIRE_EQ(buffer.occupied(), 0U);
    REQUIRE(!buffer.first_complete_message());
}

TEST(receive_buffer_reports_gap_then_advances_ack_when_filled)
{
    ReceiveBuffer buffer{SequenceNumber{100}, 8};
    const std::array<std::byte, 1> payload{std::byte{'x'}};
    const auto out_of_order = buffer.insert(data_packet(
        SequenceNumber{102}, 1, MessageBoundary::last, payload));
    REQUIRE(out_of_order);
    REQUIRE_EQ(out_of_order.status, ReceiveStatus::accepted_out_of_order);
    REQUIRE(out_of_order.has_gap);
    REQUIRE_EQ(out_of_order.gap_before_packet.first, SequenceNumber{100});
    REQUIRE_EQ(out_of_order.gap_before_packet.last, SequenceNumber{101});
    REQUIRE_EQ(buffer.next_ack_sequence(), SequenceNumber{100});

    REQUIRE(buffer.insert(data_packet(
        SequenceNumber{100}, 1, MessageBoundary::first, payload)));
    const auto filled = buffer.insert(data_packet(
        SequenceNumber{101}, 1, MessageBoundary::subsequent, payload));
    REQUIRE(filled);
    REQUIRE_EQ(filled.contiguous_advance, 2U);
    REQUIRE_EQ(buffer.next_ack_sequence(), SequenceNumber{103});
}

TEST(receive_buffer_finds_the_first_complete_message_after_a_gap)
{
    ReceiveBuffer buffer{SequenceNumber{100}, 8};
    const std::array<std::byte, 1> payload{
        std::byte{'x'}};

    auto first = data_packet(
        SequenceNumber{102}, 7,
        MessageBoundary::first, payload);
    first.data.timestamp = PacketTimestamp{9'000};
    auto last = data_packet(
        SequenceNumber{103}, 7,
        MessageBoundary::last, payload);
    last.data.timestamp = PacketTimestamp{9'000};
    REQUIRE(buffer.insert(last));
    REQUIRE(buffer.insert(first));

    REQUIRE(!buffer.has_complete_message());
    const auto message = buffer.first_complete_message();
    REQUIRE(message.has_value());
    REQUIRE_EQ(message->first_sequence,
        SequenceNumber{102});
    REQUIRE_EQ(message->last_sequence,
        SequenceNumber{103});
    REQUIRE_EQ(message->timestamp,
        PacketTimestamp{9'000});
}

TEST(receive_buffer_discards_redundant_prefix_for_group_delivery)
{
    ReceiveBuffer buffer{SequenceNumber{100}, 8};
    const std::array<std::byte, 1> payload{std::byte{'x'}};
    PacketView packet;
    packet.kind = PacketKind::data;
    packet.data.boundary = MessageBoundary::solo;
    packet.data.message_number = 1;
    packet.payload = payload;
    packet.data.sequence = SequenceNumber{100};
    REQUIRE(buffer.insert(packet));
    packet.data.sequence = SequenceNumber{101};
    packet.data.message_number = 2;
    REQUIRE(buffer.insert(packet));
    REQUIRE_EQ(buffer.buffered_payload_bytes(), 2U);

    REQUIRE_EQ(buffer.discard_before(SequenceNumber{102}), Error::none);
    REQUIRE_EQ(buffer.first_stored_sequence(), SequenceNumber{102});
    REQUIRE_EQ(buffer.occupied(), 0U);
    REQUIRE_EQ(buffer.buffered_payload_bytes(), 0U);
    packet.data.sequence = SequenceNumber{101};
    const auto stale = buffer.insert(packet);
    REQUIRE_EQ(stale.error, Error::none);
    REQUIRE_EQ(stale.status, ReceiveStatus::older_than_window);
}

TEST(receive_buffer_reassembles_fragmented_message_in_sequence_order)
{
    ReceiveBuffer buffer{SequenceNumber{10}, 8};
    const std::array<std::byte, 2> first{std::byte{'a'}, std::byte{'b'}};
    const std::array<std::byte, 2> middle{std::byte{'c'}, std::byte{'d'}};
    const std::array<std::byte, 1> last{std::byte{'e'}};
    REQUIRE(buffer.insert(data_packet(
        SequenceNumber{11}, 9, MessageBoundary::subsequent, middle)));
    REQUIRE(buffer.insert(data_packet(
        SequenceNumber{12}, 9, MessageBoundary::last, last)));
    REQUIRE_EQ(buffer.pop_message(std::span<std::byte>{}).error, Error::would_block);
    REQUIRE(buffer.insert(data_packet(
        SequenceNumber{10}, 9, MessageBoundary::first, first)));

    std::array<std::byte, 5> output{};
    const auto message = buffer.pop_message(output);
    REQUIRE(message);
    REQUIRE_EQ(message.bytes_written, 5U);
    REQUIRE_EQ(message.message_number, 9U);
    REQUIRE_EQ(output[0], std::byte{'a'});
    REQUIRE_EQ(output[4], std::byte{'e'});
    REQUIRE_EQ(buffer.occupied(), 0U);
    REQUIRE_EQ(buffer.first_stored_sequence(), SequenceNumber{13});
}

TEST(receive_buffer_does_not_consume_message_when_output_is_too_small)
{
    ReceiveBuffer buffer{SequenceNumber{20}, 4};
    const std::array<std::byte, 3> payload{
        std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
    REQUIRE(buffer.insert(data_packet(
        SequenceNumber{20}, 2, MessageBoundary::solo, payload)));
    std::array<std::byte, 2> small{};
    REQUIRE_EQ(buffer.pop_message(small).error, Error::buffer_too_small);
    REQUIRE_EQ(buffer.occupied(), 1U);
    std::array<std::byte, 3> enough{};
    REQUIRE(buffer.pop_message(enough));
}

TEST(receive_buffer_detects_duplicate_packets)
{
    ReceiveBuffer buffer{SequenceNumber{30}, 4};
    const std::array<std::byte, 1> payload{std::byte{0}};
    const auto packet = data_packet(
        SequenceNumber{30}, 1, MessageBoundary::solo, payload);
    REQUIRE(buffer.insert(packet));
    const auto duplicate = buffer.insert(packet);
    REQUIRE_EQ(duplicate.status, ReceiveStatus::duplicate);
    REQUIRE_EQ(buffer.occupied(), 1U);
}

TEST(stream_reads_cross_packet_boundaries_and_resume_mid_packet)
{
    ReceiveBuffer buffer{
        SequenceNumber{SequenceNumber::mask}, 4};
    const std::array<std::byte, 3> first{
        std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
    const std::array<std::byte, 3> second{
        std::byte{'d'}, std::byte{'e'}, std::byte{'f'}};
    REQUIRE(buffer.insert(data_packet(
        SequenceNumber{0}, 2, MessageBoundary::solo,
        second)));
    REQUIRE(buffer.insert(data_packet(
        SequenceNumber{SequenceNumber::mask}, 1,
        MessageBoundary::solo, first)));
    REQUIRE_EQ(buffer.buffered_payload_bytes(), 6U);

    std::array<std::byte, 2> first_read{};
    REQUIRE_EQ(buffer.pop_stream(first_read).bytes_written, 2U);
    REQUIRE_EQ(first_read[0], std::byte{'a'});
    REQUIRE_EQ(first_read[1], std::byte{'b'});
    REQUIRE_EQ(buffer.occupied(), 2U);
    REQUIRE_EQ(buffer.first_stored_sequence(),
        SequenceNumber{SequenceNumber::mask});
    REQUIRE_EQ(buffer.buffered_payload_bytes(), 4U);

    std::array<std::byte, 3> second_read{};
    REQUIRE_EQ(buffer.pop_stream(second_read).bytes_written, 3U);
    REQUIRE_EQ(second_read[0], std::byte{'c'});
    REQUIRE_EQ(second_read[1], std::byte{'d'});
    REQUIRE_EQ(second_read[2], std::byte{'e'});
    REQUIRE_EQ(buffer.occupied(), 1U);
    REQUIRE_EQ(buffer.first_stored_sequence(), SequenceNumber{0});
    REQUIRE_EQ(buffer.buffered_payload_bytes(), 1U);

    std::array<std::byte, 4> final_read{};
    REQUIRE_EQ(buffer.pop_stream(final_read).bytes_written, 1U);
    REQUIRE_EQ(final_read[0], std::byte{'f'});
    REQUIRE_EQ(buffer.occupied(), 0U);
    REQUIRE_EQ(buffer.first_stored_sequence(), SequenceNumber{1});
    REQUIRE_EQ(buffer.buffered_payload_bytes(), 0U);
}

TEST(stream_read_stops_at_a_sequence_gap)
{
    ReceiveBuffer buffer{SequenceNumber{20}, 4};
    const std::array<std::byte, 1> payload{
        std::byte{'x'}};
    REQUIRE(buffer.insert(data_packet(
        SequenceNumber{22}, 1, MessageBoundary::solo,
        payload)));
    std::array<std::byte, 4> output{};
    REQUIRE_EQ(buffer.pop_stream(output).error,
        Error::would_block);
}

TEST(interior_drop_range_remains_acknowledged_until_delivery_reaches_it)
{
    ReceiveBuffer buffer{
        SequenceNumber{SequenceNumber::mask}, 8};
    const std::array<std::byte, 1> payload{
        std::byte{'x'}};
    REQUIRE(buffer.insert(data_packet(
        SequenceNumber{SequenceNumber::mask}, 1,
        MessageBoundary::solo, payload)));
    REQUIRE(buffer.insert(data_packet(
        SequenceNumber{1}, 2,
        MessageBoundary::last, payload)));
    REQUIRE_EQ(buffer.next_ack_sequence(), SequenceNumber{0});

    REQUIRE_EQ(buffer.drop_range({
                   .first = SequenceNumber{0},
                   .last = SequenceNumber{1},
               },
                   2),
        Error::none);
    REQUIRE_EQ(buffer.next_ack_sequence(), SequenceNumber{2});
    REQUIRE_EQ(buffer.occupied(), 1U);
    REQUIRE_EQ(buffer.buffered_payload_bytes(), 1U);

    std::array<std::byte, 1> output{};
    REQUIRE(buffer.pop_message(output));
    REQUIRE_EQ(buffer.first_stored_sequence(), SequenceNumber{2});
    REQUIRE_EQ(buffer.next_ack_sequence(), SequenceNumber{2});
    REQUIRE_EQ(buffer.occupied(), 0U);
}

TEST(drop_range_outside_the_receive_window_cannot_corrupt_buffer_state)
{
    ReceiveBuffer buffer{SequenceNumber{100}, 4};
    const std::array<std::byte, 1> payload{std::byte{'x'}};
    REQUIRE(buffer.insert(data_packet(
        SequenceNumber{100}, 1, MessageBoundary::solo, payload)));
    REQUIRE(buffer.insert(data_packet(
        SequenceNumber{101}, 2, MessageBoundary::solo, payload)));

    std::size_t newly_dropped = 99U;
    REQUIRE_EQ(buffer.drop_range({
                   .first = SequenceNumber{1'000},
                   .last = SequenceNumber{2'000},
               },
                   3, &newly_dropped),
        Error::none);
    REQUIRE_EQ(newly_dropped, 0U);
    REQUIRE_EQ(buffer.first_stored_sequence(), SequenceNumber{100});
    REQUIRE_EQ(buffer.next_ack_sequence(), SequenceNumber{102});
    REQUIRE_EQ(buffer.occupied(), 2U);

    std::array<std::byte, 1> output{};
    REQUIRE(buffer.pop_message(output));
    REQUIRE(buffer.pop_message(output));
    REQUIRE_EQ(buffer.first_stored_sequence(), SequenceNumber{102});
    REQUIRE_EQ(buffer.occupied(), 0U);
}

TEST(receive_buffer_statistics_report_payload_and_timestamp_span)
{
    ReceiveBuffer buffer{SequenceNumber{80}, 4};
    const std::array<std::byte, 2> first_payload{
        std::byte{'a'}, std::byte{'b'}};
    const std::array<std::byte, 1> second_payload{
        std::byte{'c'}};
    const std::array<std::byte, 1> third_payload {std::byte {'d'}};
    auto first = data_packet(
        SequenceNumber{80}, 1, MessageBoundary::solo,
        first_payload);
    first.data.timestamp = PacketTimestamp{1'000};
    auto second = data_packet(
        SequenceNumber{81}, 2, MessageBoundary::solo,
        second_payload);
    second.data.timestamp = PacketTimestamp{3'500};
    auto third = data_packet(
        SequenceNumber {82}, 3, MessageBoundary::solo, third_payload);
    third.data.timestamp = PacketTimestamp {6'000};
    REQUIRE(buffer.insert(third));
    REQUIRE(buffer.insert(second));
    REQUIRE(buffer.insert(first));
    REQUIRE_EQ(buffer.buffered_payload_bytes(), 4U);
    REQUIRE_EQ(buffer.buffered_span_milliseconds(), 6U);

    std::array<std::byte, 2> output {};
    REQUIRE(buffer.pop_message(output));
    REQUIRE_EQ(buffer.buffered_payload_bytes(), 2U);
    REQUIRE_EQ(buffer.buffered_span_milliseconds(), 3U);
    REQUIRE_EQ(buffer.drop_range({
                   .first = SequenceNumber {82},
                   .last = SequenceNumber {82},
               }),
        Error::none);
    REQUIRE_EQ(buffer.buffered_payload_bytes(), 1U);
    REQUIRE_EQ(buffer.buffered_span_milliseconds(), 1U);
    REQUIRE(buffer.pop_message(output));
    REQUIRE_EQ(buffer.occupied(), 0U);
    REQUIRE_EQ(buffer.buffered_payload_bytes(), 0U);
    REQUIRE_EQ(buffer.buffered_span_milliseconds(), 0U);
}

TEST(receive_buffer_statistics_report_zero_for_an_empty_large_window)
{
    ReceiveBuffer buffer {SequenceNumber {80}, 25'600};
    REQUIRE_EQ(buffer.occupied(), 0U);
    REQUIRE_EQ(buffer.buffered_payload_bytes(), 0U);
    REQUIRE_EQ(buffer.buffered_span_milliseconds(), 0U);
}
