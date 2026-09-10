#include "test.hpp"

#include "robotweax/srt/codec.hpp"
#include "robotweax/srt/session.hpp"

#include <array>
#include <cstddef>
#include <limits>
#include <optional>

using namespace robotweax::srt;

namespace {

PacketView view_of(const OutboundPacket& outbound)
{
    PacketView packet;
    packet.kind = PacketKind::data;
    packet.data = outbound.header;
    packet.payload = outbound.payload;
    return packet;
}

PacketView encode_and_decode(const ReliabilityAction& action,
    std::array<std::byte, 64>& storage)
{
    const auto encoded = encode_reliability_action(
        action, PacketTimestamp{0}, 77, storage);
    REQUIRE(encoded);
    const auto decoded = decode_packet(std::span{storage}.first(encoded.bytes_written));
    REQUIRE(decoded);
    return decoded.packet;
}

} // namespace

TEST(reliability_session_recovers_a_lost_fragment_and_completes_ack_cycle)
{
    ReliabilitySession sender{{
        .local_initial_sequence = SequenceNumber{10},
        .peer_initial_sequence = SequenceNumber{100},
        .peer_socket_id = 900,
        .send_capacity_packets = 8,
        .receive_capacity_packets = 8,
        .maximum_payload_size = 2,
    }};
    ReliabilitySession receiver{{
        .local_initial_sequence = SequenceNumber{100},
        .peer_initial_sequence = SequenceNumber{10},
        .peer_socket_id = 800,
        .send_capacity_packets = 8,
        .receive_capacity_packets = 8,
        .maximum_payload_size = 2,
    }};
    const std::array<std::byte, 6> message{
        std::byte{'s'}, std::byte{'r'}, std::byte{'t'},
        std::byte{'-'}, std::byte{'o'}, std::byte{'k'},
    };
    REQUIRE_EQ(sender.queue_message(message, PacketTimestamp{5}), Error::none);
    const auto first = sender.next_data_packet();
    const auto lost = sender.next_data_packet();
    const auto last = sender.next_data_packet();
    REQUIRE(first.has_value());
    REQUIRE(lost.has_value());
    REQUIRE(last.has_value());

    REQUIRE(receiver.receive(view_of(*first), 1'000));
    const auto gap = receiver.receive(view_of(*last), 2'000);
    REQUIRE(gap);
    REQUIRE_EQ(gap.receiver_loss_packets, 1U);
    REQUIRE_EQ(gap.actions.size, 2U);
    REQUIRE_EQ(gap.actions.values[0].kind, ReliabilityActionKind::loss_report);
    REQUIRE_EQ(gap.actions.values[0].loss.first, SequenceNumber{11});
    REQUIRE_EQ(gap.actions.values[0].loss.last, SequenceNumber{11});

    std::array<std::byte, 64> control_storage{};
    const auto nak_packet = encode_and_decode(gap.actions.values[0], control_storage);
    const auto loss = sender.receive(nak_packet, 2'100);
    REQUIRE(loss);
    REQUIRE_EQ(loss.sender_loss_packets, 1U);
    REQUIRE_EQ(loss.sender_loss_bytes, 2U);
    const auto retransmission = sender.next_data_packet();
    REQUIRE(retransmission.has_value());
    REQUIRE_EQ(retransmission->header.sequence, SequenceNumber{11});
    REQUIRE(retransmission->header.retransmitted);

    const auto recovered = receiver.receive(view_of(*retransmission), 3'000);
    REQUIRE(recovered);
    REQUIRE_EQ(receiver.receive_buffer().next_ack_sequence(), SequenceNumber{13});
    const auto& final_ack_action = recovered.actions.values[0];
    REQUIRE_EQ(final_ack_action.kind, ReliabilityActionKind::acknowledgement);
    const auto ack_packet = encode_and_decode(final_ack_action, control_storage);
    const auto acknowledged = sender.receive(ack_packet, 3'100);
    REQUIRE(acknowledged);
    REQUIRE_EQ(sender.send_buffer().size(), 0U);
    REQUIRE_EQ(acknowledged.actions.values[0].kind,
        ReliabilityActionKind::acknowledgement_of_ack);

    const auto ackack_packet = encode_and_decode(
        acknowledged.actions.values[0], control_storage);
    REQUIRE(receiver.receive(ackack_packet, 3'500));
    REQUIRE_EQ(receiver.rtt().smoothed_microseconds(), 500U);

    std::array<std::byte, 6> output{};
    const auto delivered = receiver.pop_message(output);
    REQUIRE(delivered);
    REQUIRE_EQ(output, message);
}

TEST(session_group_queue_uses_coordinator_sequence_and_message_identity)
{
    ReliabilitySession session{{
        .local_initial_sequence = SequenceNumber{10},
        .peer_initial_sequence = SequenceNumber{20},
        .peer_socket_id = 7,
        .send_capacity_packets = 8,
        .receive_capacity_packets = 8,
        .maximum_payload_size = 4,
    }};
    const std::array<std::byte, 5> payload{};
    REQUIRE_EQ(session.queue_group_message(payload,
                   SequenceNumber{100}, 1, PacketTimestamp{5}),
        Error::none);
    REQUIRE_EQ(session.send_buffer().next_sequence(),
        SequenceNumber{102});
    REQUIRE_EQ(session.next_message_number(), 2U);
    REQUIRE_EQ(session.queue_group_message(payload,
                   SequenceNumber{101}, 2, PacketTimestamp{6}),
        Error::invalid_state);
    REQUIRE_EQ(session.queue_group_message(payload,
                   SequenceNumber{102}, 2, PacketTimestamp{6}),
        Error::none);
}

TEST(session_group_sequence_skip_queues_an_exact_dropreq)
{
    ReliabilitySession session {{
        .local_initial_sequence = SequenceNumber {10},
        .peer_initial_sequence = SequenceNumber {20},
        .peer_socket_id = 7,
        .send_capacity_packets = 8,
        .receive_capacity_packets = 8,
    }};

    REQUIRE_EQ(
        session.skip_group_sequences(SequenceNumber {9}), Error::invalid_state);
    REQUIRE_EQ(session.skip_group_sequences(SequenceNumber {13}), Error::none);
    const auto drops = session.take_pending_drop_requests();
    REQUIRE_EQ(drops.size, 1U);
    REQUIRE_EQ(drops.values[0].kind, ReliabilityActionKind::drop_request);
    REQUIRE_EQ(drops.values[0].drop.message_number, 0U);
    REQUIRE_EQ(drops.values[0].drop.sequences.first, SequenceNumber {10});
    REQUIRE_EQ(drops.values[0].drop.sequences.last, SequenceNumber {12});

    ReliabilitySession receiver {{
        .local_initial_sequence = SequenceNumber {20},
        .peer_initial_sequence = SequenceNumber {10},
        .peer_socket_id = 8,
        .send_capacity_packets = 8,
        .receive_capacity_packets = 8,
    }};
    std::array<std::byte, 64> control_storage {};
    const auto drop_packet =
        encode_and_decode(drops.values[0], control_storage);
    const auto dropped = receiver.receive(drop_packet, 1);
    REQUIRE(dropped);
    REQUIRE_EQ(dropped.receiver_drop_packets, 3U);
    REQUIRE_EQ(
        receiver.receive_buffer().next_ack_sequence(), SequenceNumber {13});

    const std::array<std::byte, 1> payload {std::byte {'g'}};
    REQUIRE_EQ(session.queue_group_message(
                   payload, SequenceNumber {13}, 1, PacketTimestamp {5}),
        Error::none);
    REQUIRE_EQ(session.skip_group_sequences(SequenceNumber {14}),
        Error::invalid_state);
    const auto data = session.next_data_packet();
    REQUIRE(data.has_value());
    REQUIRE_EQ(data->header.sequence, SequenceNumber {13});
    REQUIRE(receiver.receive(view_of(*data), 2));
    std::array<std::byte, 1> output {};
    const auto received = receiver.pop_message_at(output, 2);
    REQUIRE(received);
    REQUIRE(!received.delivery_time_microseconds);
    REQUIRE_EQ(output, payload);
}

TEST(session_group_sequence_skip_is_rollover_safe)
{
    ReliabilitySession session {{
        .local_initial_sequence = SequenceNumber {SequenceNumber::mask - 1U},
        .peer_initial_sequence = SequenceNumber {20},
        .peer_socket_id = 7,
        .send_capacity_packets = 8,
        .receive_capacity_packets = 8,
    }};

    REQUIRE_EQ(session.skip_group_sequences(SequenceNumber {1}), Error::none);
    const auto drops = session.take_pending_drop_requests();
    REQUIRE_EQ(drops.size, 1U);
    REQUIRE_EQ(drops.values[0].drop.sequences.first,
        SequenceNumber {SequenceNumber::mask - 1U});
    REQUIRE_EQ(drops.values[0].drop.sequences.last, SequenceNumber {0});
}

TEST(session_late_group_member_adopts_current_message_identity)
{
    ReliabilitySession session{{
        .local_initial_sequence = SequenceNumber{10},
        .peer_initial_sequence = SequenceNumber{20},
        .peer_socket_id = 7,
        .send_capacity_packets = 8,
        .receive_capacity_packets = 8,
    }};
    const std::array<std::byte, 3> payload{};
    REQUIRE_EQ(session.queue_group_message(payload,
                   SequenceNumber{900}, 41, PacketTimestamp{5}),
        Error::none);
    REQUIRE_EQ(session.next_message_number(), 42U);
}

TEST(rtt_estimator_uses_reference_ewma_and_nak_floor)
{
    RttEstimator floor_estimator;
    floor_estimator.observe(1'000);
    REQUIRE_EQ(
        floor_estimator.nak_interval_microseconds(),
        minimum_nak_interval_microseconds);

    RttEstimator estimator;
    estimator.observe(40'000);
    REQUIRE_EQ(estimator.smoothed_microseconds(), 40'000U);
    REQUIRE_EQ(estimator.variation_microseconds(), 20'000U);
    REQUIRE_EQ(estimator.nak_interval_microseconds(), 60'000U);
    estimator.observe(48'000);
    REQUIRE_EQ(estimator.smoothed_microseconds(), 41'000U);
    REQUIRE_EQ(estimator.variation_microseconds(), 17'000U);
}

TEST(rtt_estimator_adopts_peer_ack_estimates_and_smooths_bidirectional_updates)
{
    RttEstimator estimator;
    estimator.observe_peer_estimate(60'000, 10'000, false);
    REQUIRE_EQ(estimator.smoothed_microseconds(), 60'000U);
    REQUIRE_EQ(estimator.variation_microseconds(), 10'000U);

    estimator.observe_peer_estimate(80'000, 20'000, false);
    REQUIRE_EQ(estimator.smoothed_microseconds(), 80'000U);
    REQUIRE_EQ(estimator.variation_microseconds(), 20'000U);

    estimator.observe_peer_estimate(40'000, 5'000, true);
    REQUIRE_EQ(estimator.smoothed_microseconds(), 75'000U);
    REQUIRE_EQ(estimator.variation_microseconds(), 25'000U);
}

TEST(full_acknowledgement_numbers_use_all_32_bits_and_skip_reserved_zero)
{
    AcknowledgementNumberGenerator numbers {0x7fff'ffffU};
    REQUIRE_EQ(numbers.take(), 0x7fff'ffffU);
    REQUIRE_EQ(numbers.take(), 0x8000'0000U);

    AcknowledgementNumberGenerator wrapping {
        std::numeric_limits<std::uint32_t>::max(),
    };
    REQUIRE_EQ(wrapping.take(), std::numeric_limits<std::uint32_t>::max());
    REQUIRE_EQ(wrapping.take(), 1U);
    REQUIRE_EQ(wrapping.take(), 2U);
}

TEST(
    acknowledgement_tracker_rejects_reserved_unknown_duplicate_and_early_ackack)
{
    AcknowledgementTracker tracker {4};
    tracker.record(0U, 100U);
    REQUIRE(!tracker.acknowledge(0U, 200U).has_value());
    REQUIRE(!tracker.acknowledge(1U, 200U).has_value());

    tracker.record(1U, 300U);
    REQUIRE(!tracker.acknowledge(1U, 300U).has_value());
    REQUIRE(!tracker.acknowledge(1U, 299U).has_value());
    REQUIRE_EQ(
        tracker.acknowledge(1U, 450U), std::optional<std::uint32_t> {150U});
    REQUIRE(!tracker.acknowledge(1U, 500U).has_value());
}

TEST(acknowledgement_tracker_evicts_collisions_without_aliasing_stale_ackack)
{
    AcknowledgementTracker tracker {4};
    tracker.record(1U, 100U);
    tracker.record(5U, 200U);

    REQUIRE(!tracker.acknowledge(1U, 300U).has_value());
    REQUIRE_EQ(
        tracker.acknowledge(5U, 350U), std::optional<std::uint32_t> {150U});

    tracker.record(std::numeric_limits<std::uint32_t>::max(), 400U);
    tracker.record(1U, 450U);
    REQUIRE_EQ(
        tracker.acknowledge(std::numeric_limits<std::uint32_t>::max(), 500U),
        std::optional<std::uint32_t> {100U});
    REQUIRE_EQ(
        tracker.acknowledge(1U, 550U), std::optional<std::uint32_t> {100U});
}

TEST(acknowledgement_tracker_clamps_unrepresentable_rtt_samples)
{
    AcknowledgementTracker tracker {1};
    tracker.record(1U, 1U);
    REQUIRE_EQ(tracker.acknowledge(1U,
                   static_cast<std::uint64_t>(
                       std::numeric_limits<std::uint32_t>::max())
                       + 2U),
        std::optional<std::uint32_t> {
            std::numeric_limits<std::uint32_t>::max()});
}

TEST(session_ignores_unknown_and_duplicate_ackack_for_rtt)
{
    ReliabilitySession receiver {{
        .local_initial_sequence = SequenceNumber {100},
        .peer_initial_sequence = SequenceNumber {10},
        .peer_socket_id = 800,
        .send_capacity_packets = 8,
        .receive_capacity_packets = 8,
    }};
    const std::array payload {std::byte {'a'}};
    PacketView data {
        .kind = PacketKind::data,
        .data =
            {
                .sequence = SequenceNumber {10},
                .message_number = 1,
                .boundary = MessageBoundary::solo,
            },
        .payload = payload,
    };
    REQUIRE(receiver.receive(data, 100U));
    const auto due = receiver.poll_timers(10'000U);
    REQUIRE_EQ(due.size, 1U);
    REQUIRE_EQ(due.values[0].kind, ReliabilityActionKind::acknowledgement);
    REQUIRE_EQ(due.values[0].acknowledgement.kind, AcknowledgementKind::full);
    const std::uint32_t full_acknowledgement_number =
        due.values[0].acknowledgement.acknowledgement_number;
    REQUIRE(full_acknowledgement_number != 0U);

    std::array<std::byte, 64> storage {};
    ReliabilityAction ackack {
        .kind = ReliabilityActionKind::acknowledgement_of_ack,
        .acknowledgement_number = full_acknowledgement_number + 100U,
    };
    REQUIRE(receiver.receive(encode_and_decode(ackack, storage), 10'500U));
    REQUIRE_EQ(receiver.rtt().smoothed_microseconds(), 100'000U);
    REQUIRE_EQ(receiver.rtt().variation_microseconds(), 50'000U);

    ackack.acknowledgement_number = full_acknowledgement_number;
    REQUIRE(receiver.receive(encode_and_decode(ackack, storage), 11'000U));
    REQUIRE_EQ(receiver.rtt().smoothed_microseconds(), 1'000U);
    REQUIRE_EQ(receiver.rtt().variation_microseconds(), 500U);

    REQUIRE(receiver.receive(encode_and_decode(ackack, storage), 12'000U));
    REQUIRE_EQ(receiver.rtt().smoothed_microseconds(), 1'000U);
    REQUIRE_EQ(receiver.rtt().variation_microseconds(), 500U);
}

TEST(session_rejects_reserved_zero_ackack_number)
{
    ReliabilitySession receiver {{
        .local_initial_sequence = SequenceNumber {100},
        .peer_initial_sequence = SequenceNumber {10},
        .peer_socket_id = 800,
        .send_capacity_packets = 8,
        .receive_capacity_packets = 8,
    }};
    PacketView ackack;
    ackack.kind = PacketKind::control;
    ackack.control.type = ControlType::acknowledgement_of_ack;
    ackack.control.type_specific = 0U;
    REQUIRE_EQ(
        receiver.receive(ackack, 1'000U).error, Error::invalid_control_payload);
}

TEST(session_tsbpd_gate_holds_complete_message_until_delivery_time)
{
    ReliabilitySession receiver{{
        .local_initial_sequence = SequenceNumber{100},
        .peer_initial_sequence = SequenceNumber{10},
        .send_capacity_packets = 4,
        .receive_capacity_packets = 4,
    }};
    receiver.enable_tsbpd(1'000, PacketTimestamp{0}, 100);
    const std::array<std::byte, 1> payload{std::byte{'x'}};
    PacketView packet;
    packet.kind = PacketKind::data;
    packet.data.sequence = SequenceNumber{10};
    packet.data.boundary = MessageBoundary::solo;
    packet.data.timestamp = PacketTimestamp{50};
    packet.payload = payload;
    REQUIRE(receiver.receive(packet, 1'010));

    std::array<std::byte, 1> output{};
    const auto waiting = receiver.pop_message_at(output, 1'149);
    REQUIRE_EQ(waiting.error, Error::would_block);
    REQUIRE(!waiting.delivery_time_microseconds);
    const auto small = receiver.pop_message_at({}, 1'150);
    REQUIRE_EQ(small.error, Error::buffer_too_small);
    REQUIRE(!small.delivery_time_microseconds);
    const auto delivered = receiver.pop_message_at(output, 1'150);
    REQUIRE(delivered);
    REQUIRE(delivered.delivery_time_microseconds);
    REQUIRE_EQ(*delivered.delivery_time_microseconds, 1'150U);
    receiver.enable_tsbpd(1'000, PacketTimestamp {0}, 300);
    REQUIRE_EQ(*delivered.delivery_time_microseconds, 1'150U);
    REQUIRE_EQ(output[0], std::byte{'x'});
}

TEST(session_receiver_tlpktdrop_releases_a_due_message_and_fakes_ack)
{
    ReliabilitySession receiver{{
        .local_initial_sequence = SequenceNumber{100},
        .peer_initial_sequence = SequenceNumber{10},
        .peer_socket_id = 900,
        .send_capacity_packets = 8,
        .receive_capacity_packets = 8,
    }};
    receiver.configure_live({
        .receive_tsbpd = true,
        .too_late_packet_drop = true,
        .periodic_nak = true,
        .retransmit_flag = true,
        .receive_delay_milliseconds = 100,
    }, 1'000, PacketTimestamp{0});

    const std::array<std::byte, 1> payload{
        std::byte{'p'}};
    PacketView packet;
    packet.kind = PacketKind::data;
    packet.data.sequence = SequenceNumber{11};
    packet.data.message_number = 2;
    packet.data.boundary = MessageBoundary::solo;
    packet.data.timestamp = PacketTimestamp{50};
    packet.payload = payload;
    REQUIRE(receiver.receive(packet, 1'010));

    REQUIRE_EQ(receiver.next_receive_delivery_time(),
        std::optional<std::uint64_t>{101'050});
    REQUIRE_EQ(receiver.drop_too_late_receiver(
                   101'049).receiver_drop_packets,
        0U);
    const auto dropped =
        receiver.drop_too_late_receiver(101'050);
    REQUIRE(dropped);
    REQUIRE_EQ(dropped.receiver_drop_packets, 1U);
    REQUIRE_EQ(dropped.actions.size, 1U);
    REQUIRE_EQ(dropped.actions.values[0].kind,
        ReliabilityActionKind::acknowledgement);
    REQUIRE_EQ(dropped.actions.values[0]
            .acknowledgement.next_sequence,
        SequenceNumber{12});
    REQUIRE(receiver.message_ready_at(101'050));

    std::array<std::byte, 1> output{};
    REQUIRE(receiver.pop_message_at(output, 101'050));
    REQUIRE_EQ(output, payload);
}

TEST(session_receiver_keeps_a_gap_when_tlpktdrop_is_disabled)
{
    ReliabilitySession receiver{{
        .local_initial_sequence = SequenceNumber{100},
        .peer_initial_sequence = SequenceNumber{10},
        .peer_socket_id = 900,
        .send_capacity_packets = 4,
        .receive_capacity_packets = 4,
    }};
    receiver.configure_live({
        .receive_tsbpd = true,
        .too_late_packet_drop = false,
        .receive_delay_milliseconds = 100,
    }, 1'000, PacketTimestamp{0});

    const std::array<std::byte, 1> payload{
        std::byte{'p'}};
    PacketView packet;
    packet.kind = PacketKind::data;
    packet.data.sequence = SequenceNumber{11};
    packet.data.message_number = 2;
    packet.data.boundary = MessageBoundary::solo;
    packet.data.timestamp = PacketTimestamp{50};
    packet.payload = payload;
    REQUIRE(receiver.receive(packet, 1'010));

    REQUIRE(!receiver.next_receive_delivery_time().has_value());
    REQUIRE_EQ(receiver.drop_too_late_receiver(
                   200'000).receiver_drop_packets,
        0U);
    REQUIRE(!receiver.message_ready_at(200'000));
}

TEST(session_receiver_tlpktdrop_is_sequence_and_timestamp_wrap_safe)
{
    ReliabilitySession receiver{{
        .local_initial_sequence = SequenceNumber{100},
        .peer_initial_sequence =
            SequenceNumber{SequenceNumber::mask},
        .peer_socket_id = 900,
        .send_capacity_packets = 4,
        .receive_capacity_packets = 4,
    }};
    receiver.configure_live({
        .receive_tsbpd = true,
        .too_late_packet_drop = true,
        .receive_delay_milliseconds = 0,
    }, 1'000'000, PacketTimestamp{0xffff'f000U});

    const std::array<std::byte, 1> payload{
        std::byte{'w'}};
    PacketView packet;
    packet.kind = PacketKind::data;
    packet.data.sequence = SequenceNumber{0};
    packet.data.message_number = 2;
    packet.data.boundary = MessageBoundary::solo;
    packet.data.timestamp = PacketTimestamp{0x20U};
    packet.payload = payload;
    REQUIRE(receiver.receive(packet, 1'000'010));

    REQUIRE_EQ(receiver.next_receive_delivery_time(),
        std::optional<std::uint64_t>{1'004'128});
    const auto dropped = receiver.drop_too_late_receiver(
        1'004'128);
    REQUIRE(dropped);
    REQUIRE_EQ(dropped.receiver_drop_packets, 1U);
    REQUIRE_EQ(dropped.actions.values[0]
            .acknowledgement.next_sequence,
        SequenceNumber{1});
    std::array<std::byte, 1> output{};
    REQUIRE(receiver.pop_message_at(output, 1'004'128));
    REQUIRE_EQ(output, payload);
}

TEST(session_reopens_a_full_receive_window_after_application_delivery)
{
    ReliabilitySession receiver{{
        .local_initial_sequence = SequenceNumber{100},
        .peer_initial_sequence = SequenceNumber{10},
        .peer_socket_id = 900,
        .send_capacity_packets = 2,
        .receive_capacity_packets = 2,
    }};
    const std::array<std::byte, 1> payload{std::byte{'x'}};
    PacketView packet;
    packet.kind = PacketKind::data;
    packet.data.boundary = MessageBoundary::solo;
    packet.payload = payload;

    packet.data.sequence = SequenceNumber{10};
    packet.data.message_number = 1;
    REQUIRE(receiver.receive(packet, 100));
    packet.data.sequence = SequenceNumber{11};
    packet.data.message_number = 2;
    const auto filled = receiver.receive(packet, 200);
    REQUIRE(filled);
    REQUIRE_EQ(filled.actions.values[0]
            .acknowledgement
            .available_receive_buffer_packets,
        0U);

    const auto zero_window = receiver.poll_timers(10'000);
    REQUIRE_EQ(zero_window.size, 1U);
    REQUIRE_EQ(zero_window.values[0].kind,
        ReliabilityActionKind::acknowledgement);
    REQUIRE_EQ(zero_window.values[0]
            .acknowledgement
            .available_receive_buffer_packets,
        0U);

    std::array<std::byte, 1> output{};
    REQUIRE(receiver.pop_message(output));
    receiver.note_receive_buffer_released(10'001);
    const auto reopened = receiver.poll_timers(10'001);
    REQUIRE_EQ(reopened.size, 1U);
    REQUIRE_EQ(reopened.values[0].kind,
        ReliabilityActionKind::acknowledgement);
    REQUIRE_EQ(reopened.values[0]
            .acknowledgement
            .available_receive_buffer_packets,
        1U);

    packet.data.sequence = SequenceNumber{12};
    packet.data.message_number = 3;
    REQUIRE(receiver.receive(packet, 10'002));
    REQUIRE_EQ(receiver.receive_buffer().available(), 0U);
}

TEST(session_default_buffer_holds_a_tsbpd_burst_beyond_1024_packets)
{
    const SocketOptions options;
    ReliabilitySession receiver{{
        .local_initial_sequence = SequenceNumber{1},
        .peer_initial_sequence = SequenceNumber{10},
        .peer_socket_id = 900,
        .send_capacity_packets = options.send_buffer_packets(),
        .receive_capacity_packets = options.receive_buffer_packets(),
    }};
    receiver.enable_tsbpd(
        1'000, PacketTimestamp{0}, 120'000);
    const std::array<std::byte, 1> payload{std::byte{'b'}};
    PacketView packet;
    packet.kind = PacketKind::data;
    packet.data.boundary = MessageBoundary::solo;
    packet.payload = payload;

    constexpr std::uint32_t packet_count = 1'100;
    for (std::uint32_t index = 0;
         index < packet_count; ++index) {
        packet.data.sequence =
            SequenceNumber{10U + index};
        packet.data.message_number = index + 1U;
        packet.data.timestamp = PacketTimestamp{index};
        REQUIRE(receiver.receive(
            packet, 2'000U + index));
    }
    REQUIRE_EQ(receiver.receive_buffer().occupied(),
        static_cast<std::size_t>(packet_count));

    std::array<std::byte, 1> output{};
    REQUIRE_EQ(receiver.pop_message_at(
            output, 120'999).error,
        Error::would_block);
    for (std::uint32_t index = 0;
         index < packet_count; ++index) {
        REQUIRE(receiver.pop_message_at(
            output, 200'000));
    }
    REQUIRE_EQ(receiver.receive_buffer().occupied(), 0U);
}

TEST(session_tsbpd_drift_ignores_retransmitted_duplicate_and_late_arrivals)
{
    ReliabilitySession receiver{{
        .local_initial_sequence = SequenceNumber{1},
        .peer_initial_sequence = SequenceNumber{10},
        .send_capacity_packets = 8,
        .receive_capacity_packets = 2'048,
    }};
    receiver.configure_live({
        .receive_tsbpd = true,
        .retransmit_flag = true,
    }, 1'000'000, PacketTimestamp{0});

    const std::array<std::byte, 1> payload{std::byte{'t'}};
    PacketView packet;
    packet.kind = PacketKind::data;
    packet.data.boundary = MessageBoundary::solo;
    packet.payload = payload;
    const auto receive = [&](std::uint32_t offset,
                             std::uint32_t sequence_offset,
                             std::uint64_t extra_delay = 0U) {
        packet.data.sequence = SequenceNumber{10U + sequence_offset};
        packet.data.message_number = sequence_offset + 1U;
        packet.data.timestamp = PacketTimestamp{offset * 1'400U};
        return receiver.receive(packet,
            1'008'000U + static_cast<std::uint64_t>(offset) * 1'400U
                + extra_delay);
    };

    for (std::uint32_t offset = 0; offset < 998U; ++offset) {
        REQUIRE(receive(offset, offset));
    }

    packet.data.retransmitted = true;
    const auto duplicate = receive(997U, 997U, 1'000'000U);
    REQUIRE(duplicate);
    REQUIRE(!duplicate.receiver_packet_accepted_unique);
    packet.data.retransmitted = false;

    // The forward packet is a valid clock sample. The subsequently filled
    // gap is a unique original packet, but its late arrival is not.
    REQUIRE(receive(999U, 999U));
    REQUIRE(receive(998U, 998U, 1'000'000U));
    REQUIRE_EQ(receiver.drift_correction_count(), 0U);

    REQUIRE(receive(1'000U, 1'000U));
    REQUIRE_EQ(receiver.drift_correction_count(), 1U);
    REQUIRE_EQ(receiver.total_drift_correction_microseconds(), 5'000);
}

TEST(session_drift_tracer_can_be_disabled_and_reenabled_dynamically)
{
    ReliabilitySession receiver{{
        .local_initial_sequence = SequenceNumber{1},
        .peer_initial_sequence = SequenceNumber{10},
        .send_capacity_packets = 8,
        .receive_capacity_packets = 2'048,
    }};
    receiver.configure_live({.receive_tsbpd = true},
        1'000'000, PacketTimestamp{0});

    SocketOptions options;
    REQUIRE_EQ(options.set(SocketOption::drift_tracer, 0), Error::none);
    REQUIRE_EQ(receiver.apply_dynamic_options(options), Error::none);

    const std::array<std::byte, 1> payload{std::byte{'d'}};
    PacketView packet;
    packet.kind = PacketKind::data;
    packet.data.boundary = MessageBoundary::solo;
    packet.payload = payload;
    const auto receive = [&](std::uint32_t offset) {
        packet.data.sequence = SequenceNumber{10U + offset};
        packet.data.message_number = offset + 1U;
        packet.data.timestamp = PacketTimestamp{offset * 1'400U};
        return receiver.receive(packet,
            1'008'000U + static_cast<std::uint64_t>(offset) * 1'400U);
    };

    for (std::uint32_t offset = 0; offset < 1'000U; ++offset) {
        REQUIRE(receive(offset));
    }
    REQUIRE_EQ(receiver.drift_correction_count(), 0U);

    REQUIRE_EQ(options.set(SocketOption::drift_tracer, 1), Error::none);
    REQUIRE_EQ(receiver.apply_dynamic_options(options), Error::none);
    for (std::uint32_t offset = 1'000U; offset < 2'000U; ++offset) {
        REQUIRE(receive(offset));
    }
    REQUIRE_EQ(receiver.drift_correction_count(), 1U);
}

TEST(session_drift_tracer_samples_keepalive_timestamps)
{
    ReliabilitySession receiver{{
        .send_capacity_packets = 8,
        .receive_capacity_packets = 8,
    }};
    receiver.configure_live({.receive_tsbpd = true},
        1'000'000, PacketTimestamp{0});

    PacketView keepalive;
    keepalive.kind = PacketKind::control;
    keepalive.control.type = ControlType::keepalive;
    const std::array<std::byte, 4> padding {};
    keepalive.payload = padding;
    for (std::uint32_t offset = 0; offset < 1'000U; ++offset) {
        keepalive.control.timestamp = PacketTimestamp{offset * 1'400U};
        REQUIRE(receiver.receive(keepalive,
            1'008'000U
                + static_cast<std::uint64_t>(offset) * 1'400U));
    }
    REQUIRE_EQ(receiver.drift_correction_count(), 1U);
    REQUIRE_EQ(receiver.total_drift_correction_microseconds(), 5'000);
}

TEST(session_classifies_reordered_and_belated_packets)
{
    ReliabilitySession receiver{{
        .local_initial_sequence = SequenceNumber{100},
        .peer_initial_sequence = SequenceNumber{10},
        .send_capacity_packets = 8,
        .receive_capacity_packets = 8,
    }};
    receiver.configure_live({
        .receive_tsbpd = true,
        .retransmit_flag = true,
    }, 1'000, PacketTimestamp{0});

    const std::array<std::byte, 1> payload{
        std::byte{'r'}};
    PacketView packet;
    packet.kind = PacketKind::data;
    packet.data.sequence = SequenceNumber{12};
    packet.data.message_number = 3;
    packet.data.boundary = MessageBoundary::solo;
    packet.data.timestamp = PacketTimestamp{50};
    packet.payload = payload;
    const auto gap = receiver.receive(packet, 1'050);
    REQUIRE(gap);
    REQUIRE_EQ(gap.receiver_loss_packets, 2U);
    REQUIRE_EQ(
        gap.receiver_reorder_distance_packets, 0U);

    packet.data.sequence = SequenceNumber{11};
    packet.data.message_number = 2;
    const auto reordered_one =
        receiver.receive(packet, 1'060);
    REQUIRE(reordered_one);
    REQUIRE_EQ(
        reordered_one.receiver_reorder_distance_packets,
        1U);
    packet.data.sequence = SequenceNumber{10};
    packet.data.message_number = 1;
    const auto reordered_two =
        receiver.receive(packet, 1'070);
    REQUIRE(reordered_two);
    REQUIRE_EQ(
        reordered_two.receiver_reorder_distance_packets,
        2U);

    std::array<std::byte, 1> output{};
    REQUIRE(receiver.pop_message_at(output, 1'070));
    packet.data.retransmitted = true;
    const auto belated = receiver.receive(packet, 2'050);
    REQUIRE(belated);
    REQUIRE(belated.receiver_packet_belated);
    REQUIRE(!belated.receiver_packet_accepted_unique);
    REQUIRE_EQ(
        belated.receiver_reorder_distance_packets, 0U);
    REQUIRE_EQ(
        belated.receiver_belated_delay_microseconds,
        1'000U);

    packet.data.retransmitted = false;
    const auto belated_original = receiver.receive(packet, 2'060);
    REQUIRE(belated_original);
    REQUIRE(belated_original.receiver_packet_belated);
    REQUIRE(!belated_original.receiver_packet_accepted_unique);
    REQUIRE_EQ(
        belated_original.receiver_reorder_distance_packets,
        2U);
}

TEST(session_reorder_classification_is_wrap_safe_and_negotiated)
{
    ReliabilitySession receiver{{
        .local_initial_sequence = SequenceNumber{100},
        .peer_initial_sequence =
            SequenceNumber{SequenceNumber::mask},
        .send_capacity_packets = 4,
        .receive_capacity_packets = 4,
    }};
    receiver.configure_live({
        .retransmit_flag = true,
    }, 0, PacketTimestamp{0});
    const std::array<std::byte, 1> payload{
        std::byte{'w'}};
    PacketView packet;
    packet.kind = PacketKind::data;
    packet.data.sequence = SequenceNumber{0};
    packet.data.boundary = MessageBoundary::solo;
    packet.payload = payload;
    const auto gap = receiver.receive(packet, 100);
    REQUIRE(gap);
    REQUIRE_EQ(gap.receiver_loss_packets, 1U);

    packet.data.sequence =
        SequenceNumber{SequenceNumber::mask};
    const auto recovered = receiver.receive(packet, 200);
    REQUIRE(recovered);
    REQUIRE_EQ(
        recovered.receiver_reorder_distance_packets, 1U);

    ReliabilitySession legacy_peer{{
        .local_initial_sequence = SequenceNumber{200},
        .peer_initial_sequence = SequenceNumber{10},
        .send_capacity_packets = 4,
        .receive_capacity_packets = 4,
    }};
    packet.data.sequence = SequenceNumber{12};
    REQUIRE(legacy_peer.receive(packet, 100));
    packet.data.sequence = SequenceNumber{11};
    const auto indistinguishable =
        legacy_peer.receive(packet, 200);
    REQUIRE(indistinguishable);
    REQUIRE_EQ(
        indistinguishable.receiver_reorder_distance_packets,
        0U);
}

TEST(session_delays_initial_loss_report_by_dynamic_reorder_tolerance)
{
    ReliabilitySession receiver{{
        .local_initial_sequence = SequenceNumber{100},
        .peer_initial_sequence = SequenceNumber{10},
        .send_capacity_packets = 16,
        .receive_capacity_packets = 16,
    }};
    receiver.configure_live({
        .periodic_nak = false,
        .retransmit_flag = true,
    }, 0, PacketTimestamp{0});
    SocketOptions options;
    REQUIRE_EQ(options.set(
                   SocketOption::maximum_reorder_tolerance_packets,
                   2),
        Error::none);
    REQUIRE_EQ(
        receiver.apply_dynamic_options(options), Error::none);
    REQUIRE_EQ(receiver.reorder_tolerance_packets(), 0U);

    const std::array<std::byte, 1> payload{
        std::byte{'d'}};
    PacketView packet;
    packet.kind = PacketKind::data;
    packet.data.boundary = MessageBoundary::solo;
    packet.payload = payload;

    packet.data.sequence = SequenceNumber{12};
    packet.data.message_number = 3;
    const auto gap = receiver.receive(packet, 100);
    REQUIRE(gap);
    REQUIRE_EQ(gap.receiver_loss_packets, 2U);
    REQUIRE_EQ(gap.actions.size, 1U);

    packet.data.sequence = SequenceNumber{10};
    packet.data.message_number = 1;
    const auto early = receiver.receive(packet, 200);
    REQUIRE(early);
    REQUIRE_EQ(
        early.receiver_reorder_distance_packets, 2U);
    REQUIRE_EQ(receiver.reorder_tolerance_packets(), 2U);

    packet.data.sequence = SequenceNumber{11};
    packet.data.message_number = 2;
    REQUIRE(receiver.receive(packet, 250));

    packet.data.sequence = SequenceNumber{15};
    packet.data.message_number = 6;
    const auto delayed_gap = receiver.receive(packet, 300);
    REQUIRE(delayed_gap);
    REQUIRE_EQ(delayed_gap.receiver_loss_packets, 2U);
    REQUIRE_EQ(delayed_gap.actions.size, 0U);

    packet.data.sequence = SequenceNumber{13};
    packet.data.message_number = 4;
    const auto delayed_recovery = receiver.receive(packet, 350);
    REQUIRE(delayed_recovery);
    REQUIRE_EQ(delayed_recovery.actions.size, 0U);

    packet.data.sequence = SequenceNumber{16};
    packet.data.message_number = 7;
    const auto matured = receiver.receive(packet, 400);
    REQUIRE(matured);
    REQUIRE_EQ(matured.actions.size, 1U);
    REQUIRE_EQ(matured.actions.values[0].kind,
        ReliabilityActionKind::loss_report);
    REQUIRE_EQ(matured.actions.values[0].loss.first,
        SequenceNumber{14});
    REQUIRE_EQ(matured.actions.values[0].loss.last,
        SequenceNumber{14});
}

TEST(live_session_reports_a_gap_before_its_timer_paced_acknowledgement)
{
    ReliabilitySession receiver{{
        .local_initial_sequence = SequenceNumber{100},
        .peer_initial_sequence = SequenceNumber{10},
        .send_capacity_packets = 16,
        .receive_capacity_packets = 16,
    }};
    receiver.configure_live({
        .periodic_nak = true,
        .retransmit_flag = true,
    }, 0, PacketTimestamp{0});

    const std::array payload{std::byte{'n'}};
    const PacketView packet{
        .kind = PacketKind::data,
        .data = {
            .sequence = SequenceNumber{12},
            .message_number = 3,
            .boundary = MessageBoundary::solo,
        },
        .payload = payload,
    };
    const auto gap = receiver.receive(packet, 100);
    REQUIRE(gap);
    REQUIRE_EQ(gap.actions.size, 1U);
    REQUIRE_EQ(
        gap.actions.values[0].kind,
        ReliabilityActionKind::loss_report);
    REQUIRE_EQ(
        gap.actions.values[0].loss.first,
        SequenceNumber{10});
    REQUIRE_EQ(
        gap.actions.values[0].loss.last,
        SequenceNumber{11});

    REQUIRE_EQ(receiver.poll_timers(9'999).size, 0U);
    const auto timed = receiver.poll_timers(10'000);
    REQUIRE_EQ(timed.size, 1U);
    REQUIRE_EQ(
        timed.values[0].kind,
        ReliabilityActionKind::acknowledgement);
}

TEST(live_session_lite_ack_uses_zero_without_consuming_full_ack_number)
{
    ReliabilitySession receiver{{
        .local_initial_sequence = SequenceNumber{100},
        .peer_initial_sequence = SequenceNumber{10},
        .send_capacity_packets = 128,
        .receive_capacity_packets = 128,
    }};
    receiver.configure_live({
        .periodic_nak = true,
        .retransmit_flag = true,
    }, 0, PacketTimestamp{0});

    const std::array payload{std::byte{'a'}};
    PacketView packet{
        .kind = PacketKind::data,
        .data = {
            .sequence = SequenceNumber{10},
            .message_number = 1,
            .boundary = MessageBoundary::solo,
        },
        .payload = payload,
    };
    for (std::uint32_t index = 0; index < 64; ++index) {
        packet.data.sequence = SequenceNumber{10 + index};
        packet.data.message_number = index + 1U;
        REQUIRE(receiver.receive(packet, 100 + index));
    }

    const auto lite = receiver.poll_timers(1'000);
    REQUIRE_EQ(lite.size, 1U);
    REQUIRE_EQ(
        lite.values[0].acknowledgement.kind,
        AcknowledgementKind::lite);
    REQUIRE_EQ(
        lite.values[0].acknowledgement.acknowledgement_number,
        0U);

    const auto full = receiver.poll_timers(10'000);
    REQUIRE_EQ(full.size, 1U);
    REQUIRE_EQ(
        full.values[0].acknowledgement.kind,
        AcknowledgementKind::full);
    REQUIRE_EQ(
        full.values[0].acknowledgement.acknowledgement_number,
        1U);
}

TEST(session_applies_only_current_extended_ack_receive_windows)
{
    ReliabilitySession sender {{
        .local_initial_sequence = SequenceNumber {100},
        .peer_initial_sequence = SequenceNumber {10},
        .peer_socket_id = 900,
        .send_capacity_packets = 4,
        .receive_capacity_packets = 4,
        .maximum_payload_size = 1,
    }};
    const std::array<std::byte, 2> message {
        std::byte {'a'},
        std::byte {'b'},
    };
    REQUIRE_EQ(sender.queue_message(message, PacketTimestamp {1}), Error::none);
    REQUIRE(sender.next_data_packet().has_value());

    std::array<std::byte, 64> storage {};
    ReliabilityAction acknowledgement {
        .kind = ReliabilityActionKind::acknowledgement,
        .acknowledgement =
            {
                .kind = AcknowledgementKind::full,
                .acknowledgement_number = 1,
                .next_sequence = SequenceNumber {101},
                .available_receive_buffer_packets = 0,
            },
    };
    const auto zero_window =
        sender.receive(encode_and_decode(acknowledgement, storage), 1'000);
    REQUIRE(zero_window);
    REQUIRE_EQ(zero_window.peer_available_receive_buffer_packets,
        std::optional<std::uint32_t> {0});

    acknowledgement.acknowledgement.acknowledgement_number = 2;
    acknowledgement.acknowledgement.next_sequence = SequenceNumber {100};
    acknowledgement.acknowledgement.available_receive_buffer_packets = 3;
    const auto stale =
        sender.receive(encode_and_decode(acknowledgement, storage), 2'000);
    REQUIRE(stale);
    REQUIRE(!stale.peer_available_receive_buffer_packets.has_value());

    acknowledgement.acknowledgement.kind = AcknowledgementKind::lite;
    acknowledgement.acknowledgement.acknowledgement_number = 0;
    acknowledgement.acknowledgement.next_sequence = SequenceNumber {101};
    const auto lite =
        sender.receive(encode_and_decode(acknowledgement, storage), 3'000);
    REQUIRE(lite);
    REQUIRE(!lite.peer_available_receive_buffer_packets.has_value());

    acknowledgement.acknowledgement.kind = AcknowledgementKind::small;
    acknowledgement.acknowledgement.available_receive_buffer_packets = 2;
    acknowledgement.acknowledgement.round_trip_time_microseconds = 40'000;
    acknowledgement.acknowledgement.round_trip_time_variance_microseconds =
        5'000;
    const auto small =
        sender.receive(encode_and_decode(acknowledgement, storage), 4'000);
    REQUIRE(small);
    REQUIRE_EQ(small.peer_available_receive_buffer_packets,
        std::optional<std::uint32_t> {2});
    REQUIRE_EQ(small.actions.size, 0U);
    REQUIRE_EQ(sender.rtt().smoothed_microseconds(), 40'000U);
    REQUIRE_EQ(sender.rtt().variation_microseconds(), 5'000U);

    acknowledgement.acknowledgement.next_sequence = SequenceNumber {100};
    acknowledgement.acknowledgement.available_receive_buffer_packets = 4;
    const auto stale_small =
        sender.receive(encode_and_decode(acknowledgement, storage), 5'000);
    REQUIRE(stale_small);
    REQUIRE(!stale_small.peer_available_receive_buffer_packets.has_value());
}

TEST(session_acknowledges_the_numbered_reference_small_ack_variant)
{
    ReliabilitySession sender {{
        .local_initial_sequence = SequenceNumber {100},
        .peer_initial_sequence = SequenceNumber {10},
        .peer_socket_id = 900,
        .send_capacity_packets = 4,
        .receive_capacity_packets = 4,
        .maximum_payload_size = 1,
    }};
    const std::array<std::byte, 1> message {std::byte {'a'}};
    REQUIRE_EQ(sender.queue_message(message, PacketTimestamp {1}), Error::none);
    REQUIRE(sender.next_data_packet().has_value());

    Acknowledgement acknowledgement {
        .kind = AcknowledgementKind::small,
        .acknowledgement_number = 7,
        .next_sequence = SequenceNumber {101},
        .round_trip_time_microseconds = 20'000,
        .round_trip_time_variance_microseconds = 4'000,
        .available_receive_buffer_packets = 3,
    };
    std::array<std::byte, 16> payload {};
    REQUIRE(encode_acknowledgement_payload(acknowledgement, payload));
    MutablePacketView packet;
    packet.kind = PacketKind::control;
    packet.control.type = ControlType::acknowledgement;
    packet.control.type_specific = acknowledgement.acknowledgement_number;
    packet.control.destination_socket_id = 900;
    packet.payload = payload;
    std::array<std::byte, packet_header_size + 16U> datagram {};
    REQUIRE(encode_packet(packet, datagram));
    const auto decoded_packet = decode_packet(datagram);
    REQUIRE(decoded_packet);

    const auto result = sender.receive(decoded_packet.packet, 1'000);
    REQUIRE(result);
    REQUIRE_EQ(sender.send_buffer().size(), 0U);
    REQUIRE_EQ(result.peer_available_receive_buffer_packets,
        std::optional<std::uint32_t> {3});
    REQUIRE_EQ(result.actions.size, 1U);
    REQUIRE_EQ(result.actions.values[0].kind,
        ReliabilityActionKind::acknowledgement_of_ack);
    REQUIRE_EQ(result.actions.values[0].acknowledgement_number, 7U);
}

TEST(session_preserves_disjoint_loss_ranges_for_periodic_reports)
{
    ReliabilitySession receiver{{
        .local_initial_sequence = SequenceNumber{100},
        .peer_initial_sequence = SequenceNumber{10},
        .send_capacity_packets = 32,
        .receive_capacity_packets = 32,
    }};
    receiver.configure_live({
        .periodic_nak = true,
        .retransmit_flag = true,
    }, 0, PacketTimestamp{0});

    const std::array<std::byte, 1> payload{
        std::byte{'l'}};
    PacketView packet;
    packet.kind = PacketKind::data;
    packet.data.boundary = MessageBoundary::solo;
    packet.payload = payload;

    packet.data.sequence = SequenceNumber{12};
    packet.data.message_number = 3;
    const auto first_gap = receiver.receive(packet, 100);
    REQUIRE(first_gap);
    REQUIRE_EQ(first_gap.actions.values[0].loss.first,
        SequenceNumber{10});
    REQUIRE_EQ(first_gap.actions.values[0].loss.last,
        SequenceNumber{11});

    packet.data.sequence = SequenceNumber{14};
    packet.data.message_number = 5;
    const auto second_gap = receiver.receive(packet, 200);
    REQUIRE(second_gap);
    REQUIRE_EQ(second_gap.actions.values[0].loss.first,
        SequenceNumber{13});
    REQUIRE_EQ(second_gap.actions.values[0].loss.last,
        SequenceNumber{13});

    packet.data.sequence = SequenceNumber{11};
    packet.data.message_number = 2;
    REQUIRE(receiver.receive(packet, 300));

    const auto periodic = receiver.poll_timers(150'100);
    bool saw_ten = false;
    bool saw_thirteen = false;
    std::size_t loss_report_count = 0;
    std::size_t loss_report_index = periodic.size;
    for (std::size_t index = 0; index < periodic.size; ++index) {
        if (periodic.values[index].kind
            != ReliabilityActionKind::loss_report) {
            continue;
        }
        ++loss_report_count;
        loss_report_index = index;
        for (const auto& range :
             periodic.loss_ranges(periodic.values[index])) {
            saw_ten = saw_ten
                || (range.first == SequenceNumber{10}
                    && range.last == SequenceNumber{10});
            saw_thirteen = saw_thirteen
                || (range.first == SequenceNumber{13}
                    && range.last == SequenceNumber{13});
        }
    }
    REQUIRE_EQ(loss_report_count, 1U);
    REQUIRE(saw_ten);
    REQUIRE(saw_thirteen);

    std::array<std::byte, 64> datagram{};
    const auto encoded = encode_reliability_action(
        periodic.values[loss_report_index],
        periodic.loss_ranges(
            periodic.values[loss_report_index]),
        PacketTimestamp{1}, 77, datagram);
    REQUIRE(encoded);
    const auto decoded = decode_packet(
        std::span{datagram}.first(encoded.bytes_written));
    REQUIRE(decoded);
    REQUIRE_EQ(decoded.packet.control.type,
        ControlType::negative_acknowledgement);
    auto loss_payload = decoded.packet.payload;
    const auto first = decode_loss_range(loss_payload);
    REQUIRE(first);
    loss_payload =
        loss_payload.subspan(first.bytes_consumed);
    const auto second = decode_loss_range(loss_payload);
    REQUIRE(second);
    REQUIRE_EQ(first.range.first, SequenceNumber{10});
    REQUIRE_EQ(second.range.first, SequenceNumber{13});
    REQUIRE_EQ(second.bytes_consumed, loss_payload.size());
}

TEST(multi_range_nak_queues_every_requested_retransmission)
{
    ReliabilitySession sender{{
        .local_initial_sequence = SequenceNumber{10},
        .peer_initial_sequence = SequenceNumber{100},
        .send_capacity_packets = 8,
        .receive_capacity_packets = 8,
        .maximum_payload_size = 1,
    }};
    const std::array<std::byte, 6> message{
        std::byte{'a'}, std::byte{'b'}, std::byte{'c'},
        std::byte{'d'}, std::byte{'e'}, std::byte{'f'},
    };
    REQUIRE_EQ(
        sender.queue_message(message, PacketTimestamp{1}),
        Error::none);
    for (std::size_t index = 0; index < message.size(); ++index) {
        REQUIRE(sender.next_data_packet().has_value());
    }

    const std::array<SequenceRange, 2> losses{{
        {.first = SequenceNumber{10},
            .last = SequenceNumber{11}},
        {.first = SequenceNumber{14},
            .last = SequenceNumber{14}},
    }};
    const ReliabilityAction action{
        .kind = ReliabilityActionKind::loss_report,
        .loss = losses[0],
    };
    std::array<std::byte, 64> datagram{};
    const auto encoded = encode_reliability_action(
        action, losses, PacketTimestamp{2}, 77, datagram);
    REQUIRE(encoded);
    const auto decoded = decode_packet(
        std::span{datagram}.first(encoded.bytes_written));
    REQUIRE(decoded);

    const auto result = sender.receive(decoded.packet, 100);
    REQUIRE(result);
    REQUIRE_EQ(result.sender_loss_packets, 3U);
    for (const auto expected :
         {SequenceNumber{10}, SequenceNumber{11},
             SequenceNumber{14}}) {
        const auto retransmission = sender.next_data_packet();
        REQUIRE(retransmission.has_value());
        REQUIRE_EQ(retransmission->header.sequence, expected);
        REQUIRE(retransmission->header.retransmitted);
    }
}

TEST(rejected_multi_range_nak_does_not_queue_any_retransmissions)
{
    ReliabilitySession sender {{
        .local_initial_sequence = SequenceNumber {10},
        .peer_initial_sequence = SequenceNumber {100},
        .send_capacity_packets = 8,
        .receive_capacity_packets = 8,
        .maximum_payload_size = 1,
    }};
    const std::array<std::byte, 4> message {
        std::byte {'a'},
        std::byte {'b'},
        std::byte {'c'},
        std::byte {'d'},
    };
    REQUIRE_EQ(sender.queue_message(message, PacketTimestamp {1}), Error::none);
    for (std::size_t index = 0; index < 3U; ++index) {
        REQUIRE(sender.next_data_packet().has_value());
    }

    const std::array<SequenceRange, 2> losses {{
        {
            .first = SequenceNumber {10},
            .last = SequenceNumber {10},
        },
        {
            .first = SequenceNumber {12},
            .last = SequenceNumber {13},
        },
    }};
    const ReliabilityAction action {
        .kind = ReliabilityActionKind::loss_report,
        .loss = losses[0],
    };
    std::array<std::byte, 64> datagram {};
    const auto encoded = encode_reliability_action(
        action, losses, PacketTimestamp {2}, 77, datagram);
    REQUIRE(encoded);
    const auto decoded =
        decode_packet(std::span {datagram}.first(encoded.bytes_written));
    REQUIRE(decoded);

    const auto result = sender.receive(decoded.packet, 100);
    REQUIRE(!result);
    REQUIRE_EQ(result.error, Error::invalid_control_payload);
    REQUIRE_EQ(result.sender_loss_packets, 0U);

    const auto unsent = sender.next_data_packet();
    REQUIRE(unsent.has_value());
    REQUIRE_EQ(unsent->header.sequence, SequenceNumber {13});
    REQUIRE(!unsent->header.retransmitted);
    REQUIRE(!sender.next_data_packet().has_value());
}

TEST(maximum_multi_range_nak_fits_one_srt_datagram)
{
    std::array<SequenceRange,
        maximum_loss_ranges_per_report> losses{};
    for (std::size_t index = 0; index < losses.size();
         ++index) {
        const auto first = static_cast<std::uint32_t>(
            index * 3U + 10U);
        losses[index] = {
            .first = SequenceNumber{first},
            .last = SequenceNumber{first + 1U},
        };
    }
    const ReliabilityAction action{
        .kind = ReliabilityActionKind::loss_report,
        .loss = losses.front(),
    };
    std::array<std::byte,
        packet_header_size + maximum_data_payload_size>
        datagram{};
    const auto encoded = encode_reliability_action(
        action, losses, PacketTimestamp{3}, 77, datagram);
    REQUIRE(encoded);
    REQUIRE_EQ(encoded.bytes_written, datagram.size());

    const auto decoded = decode_packet(datagram);
    REQUIRE(decoded);
    std::size_t decoded_ranges = 0;
    auto payload = decoded.packet.payload;
    while (!payload.empty()) {
        const auto range = decode_loss_range(payload);
        REQUIRE(range);
        payload = payload.subspan(range.bytes_consumed);
        ++decoded_ranges;
    }
    REQUIRE_EQ(decoded_ranges, losses.size());
}

TEST(session_adapts_and_decays_reorder_tolerance_without_allocating)
{
    ReliabilitySession receiver{{
        .local_initial_sequence = SequenceNumber{200},
        .peer_initial_sequence = SequenceNumber{10},
        .send_capacity_packets = 128,
        .receive_capacity_packets = 128,
    }};
    receiver.configure_live({
        .periodic_nak = false,
        .retransmit_flag = true,
    }, 0, PacketTimestamp{0});
    SocketOptions options;
    REQUIRE_EQ(options.set(
                   SocketOption::maximum_reorder_tolerance_packets,
                   3),
        Error::none);
    REQUIRE_EQ(
        receiver.apply_dynamic_options(options), Error::none);

    const std::array<std::byte, 1> payload{
        std::byte{'a'}};
    PacketView packet;
    packet.kind = PacketKind::data;
    packet.data.boundary = MessageBoundary::solo;
    packet.payload = payload;
    for (std::uint32_t sequence = 10; sequence <= 59; ++sequence) {
        packet.data.sequence = SequenceNumber{sequence};
        packet.data.message_number = sequence;
        REQUIRE(receiver.receive(packet, sequence));
    }
    REQUIRE_EQ(receiver.reorder_tolerance_packets(), 0U);
    REQUIRE_EQ(receiver.reorder_distance_packets(), 0U);

    packet.data.sequence = SequenceNumber{63};
    packet.data.message_number = 63;
    REQUIRE(receiver.receive(packet, 63));
    packet.data.sequence = SequenceNumber{60};
    packet.data.message_number = 60;
    const auto reordered = receiver.receive(packet, 64);
    REQUIRE(reordered);
    REQUIRE_EQ(
        reordered.receiver_reorder_distance_packets, 3U);
    REQUIRE_EQ(receiver.reorder_tolerance_packets(), 3U);
    REQUIRE_EQ(receiver.reorder_distance_packets(), 3U);

    for (std::uint32_t sequence = 64; sequence <= 113; ++sequence) {
        packet.data.sequence = SequenceNumber{sequence};
        packet.data.message_number = sequence;
        REQUIRE(receiver.receive(packet, sequence + 1U));
    }
    REQUIRE_EQ(receiver.reorder_tolerance_packets(), 2U);
    REQUIRE_EQ(receiver.reorder_distance_packets(), 2U);

    REQUIRE_EQ(options.set(
                   SocketOption::input_bandwidth_bytes_per_second,
                   100'000),
        Error::none);
    REQUIRE_EQ(
        receiver.apply_dynamic_options(options), Error::none);
    REQUIRE_EQ(receiver.reorder_tolerance_packets(), 2U);

    REQUIRE_EQ(options.set(
                   SocketOption::maximum_reorder_tolerance_packets,
                   3),
        Error::none);
    REQUIRE_EQ(
        receiver.apply_dynamic_options(options), Error::none);
    REQUIRE_EQ(receiver.reorder_tolerance_packets(), 2U);
}

TEST(session_drops_expired_sender_data_and_receiver_honors_dropreq)
{
    ReliabilitySession sender{{
        .local_initial_sequence = SequenceNumber{10},
        .peer_initial_sequence = SequenceNumber{100},
        .peer_socket_id = 900,
        .send_capacity_packets = 8,
        .receive_capacity_packets = 8,
        .maximum_payload_size = 2,
    }};
    ReliabilitySession receiver{{
        .local_initial_sequence = SequenceNumber{100},
        .peer_initial_sequence = SequenceNumber{10},
        .peer_socket_id = 800,
        .send_capacity_packets = 8,
        .receive_capacity_packets = 8,
        .maximum_payload_size = 2,
    }};
    const std::array<std::byte, 4> old_message{
        std::byte{'o'}, std::byte{'l'}, std::byte{'d'}, std::byte{'!'},
    };
    REQUIRE_EQ(sender.queue_message(old_message, PacketTimestamp{1}, true, 1'000),
        Error::none);
    const auto first = sender.next_data_packet();
    const auto second = sender.next_data_packet();
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());

    // Only the second fragment arrives, creating a loss record for sequence 10.
    const auto gap = receiver.receive(view_of(*second), 2'000);
    REQUIRE(gap);
    REQUIRE_EQ(gap.actions.values[0].kind, ReliabilityActionKind::loss_report);

    const auto drops = sender.drop_too_late_sender(2'100, 1'000);
    REQUIRE_EQ(drops.size, 1U);
    REQUIRE_EQ(drops.values[0].kind, ReliabilityActionKind::drop_request);
    REQUIRE_EQ(drops.values[0].drop.sequences.first, SequenceNumber{10});
    REQUIRE_EQ(drops.values[0].drop.sequences.last, SequenceNumber{11});
    REQUIRE_EQ(sender.send_buffer().size(), 0U);

    std::array<std::byte, 64> control_storage{};
    const auto drop_packet = encode_and_decode(drops.values[0], control_storage);
    const auto processed = receiver.receive(drop_packet, 2'200);
    REQUIRE(processed);
    REQUIRE_EQ(processed.receiver_drop_packets, 2U);
    REQUIRE_EQ(receiver.receive_buffer().next_ack_sequence(), SequenceNumber{12});
    REQUIRE_EQ(receiver.receive_buffer().occupied(), 0U);
}

TEST(session_replies_to_a_stale_nak_with_sequence_only_dropreq)
{
    ReliabilitySession sender {{
        .local_initial_sequence = SequenceNumber {10},
        .peer_initial_sequence = SequenceNumber {100},
        .peer_socket_id = 900,
        .send_capacity_packets = 8,
        .receive_capacity_packets = 8,
        .maximum_payload_size = 1,
    }};
    const std::array<std::byte, 2> message {
        std::byte {'o'},
        std::byte {'l'},
    };
    REQUIRE_EQ(sender.queue_message(message, PacketTimestamp {1}, true, 1'000),
        Error::none);
    REQUIRE(sender.next_data_packet().has_value());
    REQUIRE(sender.next_data_packet().has_value());
    REQUIRE_EQ(sender.drop_too_late_sender(2'001, 1'000).size, 1U);
    REQUIRE_EQ(sender.send_buffer().first_sequence(), SequenceNumber {12});

    const ReliabilityAction repeated_loss {
        .kind = ReliabilityActionKind::loss_report,
        .loss =
            {
                .first = SequenceNumber {10},
                .last = SequenceNumber {11},
            },
    };
    std::array<std::byte, 64> control_storage {};
    const auto nak = encode_and_decode(repeated_loss, control_storage);
    const auto result = sender.receive(nak, 2'002);
    REQUIRE(result);
    REQUIRE_EQ(result.actions.size, 1U);
    REQUIRE_EQ(
        result.actions.values[0].kind, ReliabilityActionKind::drop_request);
    REQUIRE_EQ(result.actions.values[0].drop.message_number, 0U);
    REQUIRE_EQ(
        result.actions.values[0].drop.sequences.first, SequenceNumber {10});
    REQUIRE_EQ(
        result.actions.values[0].drop.sequences.last, SequenceNumber {11});
}

TEST(session_splits_a_nak_between_stale_and_current_packets)
{
    ReliabilitySession sender {{
        .local_initial_sequence = SequenceNumber {10},
        .peer_initial_sequence = SequenceNumber {100},
        .peer_socket_id = 900,
        .send_capacity_packets = 8,
        .receive_capacity_packets = 8,
        .maximum_payload_size = 1,
    }};
    const std::array<std::byte, 2> old_message {
        std::byte {'o'},
        std::byte {'l'},
    };
    const std::array<std::byte, 1> current_message {
        std::byte {'n'},
    };
    REQUIRE_EQ(
        sender.queue_message(old_message, PacketTimestamp {1}, true, 1'000),
        Error::none);
    REQUIRE_EQ(
        sender.queue_message(current_message, PacketTimestamp {2}, true, 3'000),
        Error::none);
    REQUIRE(sender.next_data_packet().has_value());
    REQUIRE(sender.next_data_packet().has_value());
    REQUIRE(sender.next_data_packet().has_value());
    REQUIRE_EQ(sender.drop_too_late_sender(2'501, 1'000).size, 1U);

    const ReliabilityAction mixed_loss {
        .kind = ReliabilityActionKind::loss_report,
        .loss =
            {
                .first = SequenceNumber {11},
                .last = SequenceNumber {12},
            },
    };
    std::array<std::byte, 64> control_storage {};
    const auto nak = encode_and_decode(mixed_loss, control_storage);
    const auto result = sender.receive(nak, 3'001);
    REQUIRE(result);
    REQUIRE_EQ(result.sender_loss_packets, 1U);
    REQUIRE_EQ(result.actions.size, 1U);
    REQUIRE_EQ(result.actions.values[0].drop.message_number, 0U);
    REQUIRE_EQ(
        result.actions.values[0].drop.sequences.first, SequenceNumber {11});
    REQUIRE_EQ(
        result.actions.values[0].drop.sequences.last, SequenceNumber {11});

    const auto retransmission = sender.next_data_packet();
    REQUIRE(retransmission.has_value());
    REQUIRE(retransmission->header.retransmitted);
    REQUIRE_EQ(retransmission->header.sequence, SequenceNumber {12});
}

TEST(session_splits_a_mixed_nak_across_sequence_rollover)
{
    ReliabilitySession sender {{
        .local_initial_sequence = SequenceNumber {SequenceNumber::mask - 1U},
        .peer_initial_sequence = SequenceNumber {100},
        .peer_socket_id = 900,
        .send_capacity_packets = 8,
        .receive_capacity_packets = 8,
        .maximum_payload_size = 1,
    }};
    const std::array<std::byte, 2> old_message {
        std::byte {'o'},
        std::byte {'l'},
    };
    const std::array<std::byte, 1> current_message {
        std::byte {'n'},
    };
    REQUIRE_EQ(
        sender.queue_message(old_message, PacketTimestamp {1}, true, 1'000),
        Error::none);
    REQUIRE_EQ(
        sender.queue_message(current_message, PacketTimestamp {2}, true, 3'000),
        Error::none);
    REQUIRE(sender.next_data_packet().has_value());
    REQUIRE(sender.next_data_packet().has_value());
    REQUIRE(sender.next_data_packet().has_value());
    REQUIRE_EQ(sender.drop_too_late_sender(2'501, 1'000).size, 1U);
    REQUIRE_EQ(sender.send_buffer().first_sequence(), SequenceNumber {0});

    const ReliabilityAction mixed_loss {
        .kind = ReliabilityActionKind::loss_report,
        .loss =
            {
                .first = SequenceNumber {SequenceNumber::mask},
                .last = SequenceNumber {0},
            },
    };
    std::array<std::byte, 64> control_storage {};
    const auto nak = encode_and_decode(mixed_loss, control_storage);
    const auto result = sender.receive(nak, 3'001);
    REQUIRE(result);
    REQUIRE_EQ(result.sender_loss_packets, 1U);
    REQUIRE_EQ(result.actions.size, 1U);
    REQUIRE_EQ(result.actions.values[0].drop.message_number, 0U);
    REQUIRE_EQ(result.actions.values[0].drop.sequences.first,
        SequenceNumber {SequenceNumber::mask});
    REQUIRE_EQ(result.actions.values[0].drop.sequences.last,
        SequenceNumber {SequenceNumber::mask});

    const auto retransmission = sender.next_data_packet();
    REQUIRE(retransmission.has_value());
    REQUIRE(retransmission->header.retransmitted);
    REQUIRE_EQ(retransmission->header.sequence, SequenceNumber {0});
}

TEST(session_queues_every_dropreq_for_one_multi_message_nak)
{
    ReliabilitySession sender {{
        .local_initial_sequence = SequenceNumber {10},
        .peer_initial_sequence = SequenceNumber {100},
        .peer_socket_id = 900,
        .send_capacity_packets = 8,
        .receive_capacity_packets = 8,
        .maximum_payload_size = 1,
    }};
    const std::array<std::byte, 1> payload {std::byte {'x'}};
    for (std::uint32_t index = 0; index < 6U; ++index) {
        REQUIRE_EQ(sender.queue_message(payload, PacketTimestamp {index}, true,
                       index, 100U + index),
            Error::none);
        REQUIRE(sender.next_data_packet().has_value());
    }
    for (std::uint32_t index = 0; index < 6U; ++index) {
        const auto dropped = sender.drop_expired_sender_message(200U);
        REQUIRE_EQ(dropped.size, 1U);
    }

    std::array<SequenceRange, 6> losses {};
    for (std::uint32_t index = 0; index < losses.size(); ++index) {
        losses[index] = {
            .first = SequenceNumber {10U + index},
            .last = SequenceNumber {10U + index},
        };
    }
    const ReliabilityAction action {
        .kind = ReliabilityActionKind::loss_report,
        .loss = losses[0],
    };
    std::array<std::byte, 128> datagram {};
    const auto encoded = encode_reliability_action(
        action, losses, PacketTimestamp {2}, 77, datagram);
    REQUIRE(encoded);
    const auto decoded =
        decode_packet(std::span {datagram}.first(encoded.bytes_written));
    REQUIRE(decoded);

    const auto first_batch = sender.receive(decoded.packet, 300U);
    REQUIRE(first_batch);
    REQUIRE_EQ(first_batch.actions.size, 4U);
    for (std::size_t index = 0; index < first_batch.actions.size; ++index) {
        REQUIRE_EQ(first_batch.actions.values[index].drop.message_number,
            static_cast<std::uint32_t>(index + 1U));
        REQUIRE_EQ(first_batch.actions.values[index].drop.sequences.first,
            SequenceNumber {static_cast<std::uint32_t>(10U + index)});
    }
    REQUIRE(sender.has_pending_drop_requests());

    const auto second_batch = sender.take_pending_drop_requests();
    REQUIRE_EQ(second_batch.size, 2U);
    for (std::size_t index = 0; index < second_batch.size; ++index) {
        REQUIRE_EQ(second_batch.values[index].drop.message_number,
            static_cast<std::uint32_t>(index + 5U));
        REQUIRE_EQ(second_batch.values[index].drop.sequences.first,
            SequenceNumber {static_cast<std::uint32_t>(14U + index)});
    }
    REQUIRE(!sender.has_pending_drop_requests());
}

TEST(session_message_ttl_emits_dropreq_without_tlpktdrop_negotiation)
{
    ReliabilitySession sender{{
        .local_initial_sequence =
            SequenceNumber{SequenceNumber::mask},
        .peer_initial_sequence = SequenceNumber{100},
        .peer_socket_id = 900,
        .send_capacity_packets = 8,
        .receive_capacity_packets = 8,
        .maximum_payload_size = 1,
    }};
    const std::array<std::byte, 2> message{
        std::byte{'t'}, std::byte{'l'}};
    REQUIRE_EQ(sender.queue_message(
        message, PacketTimestamp{10}, true, 10, 1'000),
        Error::none);

    REQUIRE_EQ(
        sender.drop_expired_sender_message(1'000).size, 0U);
    const auto actions =
        sender.drop_expired_sender_message(1'001);
    REQUIRE_EQ(actions.size, 1U);
    REQUIRE_EQ(actions.values[0].kind,
        ReliabilityActionKind::drop_request);
    REQUIRE_EQ(actions.values[0].drop.message_number, 1U);
    REQUIRE_EQ(actions.values[0].drop.sequences.first,
        SequenceNumber{SequenceNumber::mask});
    REQUIRE_EQ(actions.values[0].drop.sequences.last,
        SequenceNumber{0});
    REQUIRE_EQ(sender.send_buffer().size(), 0U);

    // A lost DROPREQ is recovered when the receiver repeats its NAK.
    std::array<std::byte, 64> control_storage{};
    const ReliabilityAction repeated_loss{
        .kind = ReliabilityActionKind::loss_report,
        .loss = {
            .first =
                SequenceNumber{SequenceNumber::mask},
            .last = SequenceNumber{0},
        },
    };
    const auto nak =
        encode_and_decode(repeated_loss, control_storage);
    const auto retried_drop = sender.receive(nak, 1'002);
    REQUIRE(retried_drop);
    REQUIRE_EQ(retried_drop.actions.size, 1U);
    REQUIRE_EQ(retried_drop.actions.values[0].kind,
        ReliabilityActionKind::drop_request);
    REQUIRE_EQ(retried_drop.actions.values[0].drop.sequences.first,
        SequenceNumber{SequenceNumber::mask});
    REQUIRE_EQ(retried_drop.actions.values[0].drop.sequences.last,
        SequenceNumber{0});
}

TEST(negotiated_live_configuration_controls_tsbpd_and_sender_drop_threshold)
{
    ReliabilitySession session{{
        .local_initial_sequence = SequenceNumber{10},
        .peer_initial_sequence = SequenceNumber{20},
        .send_capacity_packets = 4,
        .receive_capacity_packets = 4,
    }};
    const NegotiatedLiveOptions options{
        .send_tsbpd = true,
        .receive_tsbpd = true,
        .too_late_packet_drop = true,
        .periodic_nak = true,
        .retransmit_flag = true,
        .receive_delay_milliseconds = 300,
        .peer_receive_delay_milliseconds = 200,
    };
    session.configure_live(options, 1'000'000, PacketTimestamp{100'000}, {
        .input_bandwidth_bytes_per_second = 1'000'000,
    });
    REQUIRE(session.live_options().receive_tsbpd);

    const std::array<std::byte, 1> payload{std::byte{'x'}};
    REQUIRE_EQ(session.queue_message(payload, PacketTimestamp{100'000}, true, 1),
        Error::none);
    REQUIRE_EQ(session.drop_too_late_sender(1'020'000).size, 0U);
    REQUIRE_EQ(session.drop_too_late_sender(1'020'001).size, 1U);
}

TEST(session_accepts_haivision_four_byte_ackack_padding)
{
    ReliabilitySession session{{
        .local_initial_sequence = SequenceNumber{10},
        .peer_initial_sequence = SequenceNumber{20},
        .send_capacity_packets = 4,
        .receive_capacity_packets = 4,
    }};
    const std::array<std::byte, 4> padding{};
    PacketView packet;
    packet.kind = PacketKind::control;
    packet.control.type = ControlType::acknowledgement_of_ack;
    packet.control.type_specific = 1;
    packet.payload = padding;
    REQUIRE(session.receive(packet, 100));
}

TEST(session_rejects_noncanonical_payload_free_controls_without_state_change)
{
    ReliabilitySession session {{
        .local_initial_sequence = SequenceNumber {10},
        .peer_initial_sequence = SequenceNumber {20},
        .send_capacity_packets = 4,
        .receive_capacity_packets = 4,
    }};
    session.configure_live(
        {.receive_tsbpd = true}, 1'000'000, PacketTimestamp {0});
    const auto initial_rtt = session.rtt();
    const auto initial_corrections = session.drift_correction_count();
    const std::array<std::byte, 8> malformed {std::byte {1}};

    for (const ControlType type : {ControlType::acknowledgement_of_ack,
             ControlType::keepalive, ControlType::congestion_warning,
             ControlType::shutdown, ControlType::peer_error}) {
        PacketView packet;
        packet.kind = PacketKind::control;
        packet.control.type = type;
        packet.control.type_specific =
            type == ControlType::acknowledgement_of_ack ? 1U : 0U;
        for (const std::size_t payload_size : {0U, 1U, 2U, 3U, 8U}) {
            packet.payload = std::span {malformed}.first(payload_size);
            REQUIRE_EQ(session.receive(packet, 2'000'000U).error,
                Error::invalid_control_payload);
        }
    }
    REQUIRE_EQ(session.rtt().smoothed_microseconds(),
        initial_rtt.smoothed_microseconds());
    REQUIRE_EQ(session.rtt().variation_microseconds(),
        initial_rtt.variation_microseconds());
    REQUIRE_EQ(session.drift_correction_count(), initial_corrections);
}

TEST(session_recognizes_peer_shutdown_control)
{
    ReliabilitySession session{{
        .send_capacity_packets = 4,
        .receive_capacity_packets = 4,
    }};
    const std::array<std::byte, 4> padding{};
    PacketView packet;
    packet.kind = PacketKind::control;
    packet.control.type = ControlType::shutdown;
    packet.payload = padding;
    REQUIRE(session.receive(packet, 100));
}

TEST(dynamic_socket_options_change_livecc_rate_without_recreating_session)
{
    ReliabilitySession session{{
        .send_capacity_packets = 4,
        .receive_capacity_packets = 4,
    }};
    session.configure_live(NegotiatedLiveOptions{.send_tsbpd = true},
        0, PacketTimestamp{0}, {
            .input_bandwidth_bytes_per_second = 100'000,
            .overhead_percent = 0,
        });
    REQUIRE_EQ(session.live_pacing_rate_bytes_per_second(), 100'000U);

    SocketOptions options;
    REQUIRE_EQ(options.set(SocketOption::input_bandwidth_bytes_per_second,
                   400'000),
        Error::none);
    REQUIRE_EQ(options.set(SocketOption::overhead_bandwidth_percent, 10),
        Error::none);
    REQUIRE_EQ(options.set(SocketOption::maximum_bandwidth_bytes_per_second,
                   420'000),
        Error::none);
    REQUIRE_EQ(session.apply_dynamic_options(options), Error::none);
    REQUIRE_EQ(session.live_pacing_rate_bytes_per_second(), 420'000U);
}

TEST(
    session_pacer_counts_new_packet_wire_overhead_without_charging_application_bytes)
{
    ReliabilitySession session {{
        .local_initial_sequence = SequenceNumber {10},
        .peer_initial_sequence = SequenceNumber {20},
        .send_capacity_packets = 4,
        .receive_capacity_packets = 4,
    }};
    session.configure_live(NegotiatedLiveOptions {.send_tsbpd = true}, 0,
        PacketTimestamp {0},
        {
            .input_bandwidth_bytes_per_second = 1'000,
            .overhead_percent = 0,
        });
    const std::array<std::byte, 4> payload {};
    REQUIRE_EQ(
        session.queue_message(payload, PacketTimestamp {1}), Error::none);
    REQUIRE_EQ(
        session.queue_message(payload, PacketTimestamp {2}), Error::none);

    PacketPacer pacer {1'000, 4};
    constexpr std::size_t authentication_tag_size = 16U;
    REQUIRE(session.next_paced_data_packet(pacer, 0, authentication_tag_size));
    const std::uint64_t next_wire_deadline =
        (packet_header_size + payload.size() + authentication_tag_size)
        * 1'000U;
    REQUIRE(!session.next_paced_data_packet(
        pacer, next_wire_deadline - 1U, authentication_tag_size));
    REQUIRE(session.next_paced_data_packet(
        pacer, next_wire_deadline, authentication_tag_size));
}

TEST(dynamic_socket_options_change_filecc_limit_without_recreating_session)
{
    ReliabilitySession session{{
        .send_capacity_packets = 32,
        .receive_capacity_packets = 32,
        .maximum_payload_size = 1'456,
    }};
    session.configure_file(false, {
        .maximum_congestion_window_packets = 32,
        .packet_size_bytes = 1'456,
        .maximum_bandwidth_bytes_per_second = 150'000,
    });
    REQUIRE_EQ(session.file_pacing_rate_bytes_per_second(), 150'000U);

    SocketOptions options;
    REQUIRE_EQ(options.set(
                   SocketOption::maximum_bandwidth_bytes_per_second,
                   600'000),
        Error::none);
    REQUIRE_EQ(session.apply_dynamic_options(options), Error::none);
    REQUIRE_EQ(session.file_pacing_rate_bytes_per_second(), 600'000U);
}

TEST(file_session_exposes_partial_stream_io_across_sequence_rollover)
{
    ReliabilitySession sender{{
        .local_initial_sequence =
            SequenceNumber{SequenceNumber::mask},
        .peer_initial_sequence = SequenceNumber{100},
        .peer_socket_id = 900,
        .send_capacity_packets = 2,
        .receive_capacity_packets = 4,
        .maximum_payload_size = 2,
    }};
    ReliabilitySession receiver{{
        .local_initial_sequence = SequenceNumber{100},
        .peer_initial_sequence =
            SequenceNumber{SequenceNumber::mask},
        .peer_socket_id = 800,
        .send_capacity_packets = 4,
        .receive_capacity_packets = 4,
        .maximum_payload_size = 2,
    }};
    sender.configure_file(false, {
        .maximum_congestion_window_packets = 32,
    });
    receiver.configure_file(false, {
        .maximum_congestion_window_packets = 32,
    });

    const std::array<std::byte, 5> input{
        std::byte{'a'}, std::byte{'b'}, std::byte{'c'},
        std::byte{'d'}, std::byte{'e'}};
    const auto queued = sender.queue_stream(
        input, PacketTimestamp{0});
    REQUIRE(queued);
    REQUIRE_EQ(queued.bytes_accepted, 4U);

    const auto first = sender.next_data_packet();
    const auto second = sender.next_data_packet();
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(receiver.receive(view_of(*second), 1'000));
    REQUIRE(receiver.receive(view_of(*first), 2'000));

    std::array<std::byte, 3> output{};
    REQUIRE_EQ(receiver.pop_stream(output).bytes_written, 3U);
    REQUIRE_EQ(output[0], std::byte{'a'});
    REQUIRE_EQ(output[2], std::byte{'c'});
    REQUIRE_EQ(receiver.receive_buffer().first_stored_sequence(),
        SequenceNumber{0});
}

TEST(file_session_rto_retransmits_in_flight_packets_and_exits_slow_start)
{
    ReliabilitySession sender{{
        .local_initial_sequence =
            SequenceNumber{SequenceNumber::mask - 1U},
        .peer_initial_sequence = SequenceNumber{100},
        .peer_socket_id = 900,
        .send_capacity_packets = 4,
        .receive_capacity_packets = 4,
        .maximum_payload_size = 1,
    }};
    sender.configure_file(false);
    const std::array<std::byte, 3> input{
        std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
    REQUIRE(sender.queue_stream(input, PacketTimestamp{0}));

    for (std::size_t index = 0; index < input.size(); ++index) {
        const auto packet = sender.next_data_packet();
        REQUIRE(packet.has_value());
        sender.note_data_packet_sent(100);
    }
    REQUIRE(sender.file_slow_start());
    REQUIRE(!sender.poll_sender_retransmission_timeout(330'099));
    REQUIRE(sender.poll_sender_retransmission_timeout(330'100));
    REQUIRE(!sender.file_slow_start());

    const auto first = sender.next_data_packet();
    const auto second = sender.next_data_packet();
    const auto third = sender.next_data_packet();
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(third.has_value());
    REQUIRE_EQ(first->header.sequence,
        SequenceNumber{SequenceNumber::mask - 1U});
    REQUIRE_EQ(second->header.sequence,
        SequenceNumber{SequenceNumber::mask});
    REQUIRE_EQ(third->header.sequence, SequenceNumber{0});
    REQUIRE(first->header.retransmitted);
    REQUIRE(second->header.retransmitted);
    REQUIRE(third->header.retransmitted);
}

TEST(file_session_ack_resets_rto_and_laterexmit_waits_for_queued_loss)
{
    ReliabilitySession sender{{
        .local_initial_sequence = SequenceNumber{10},
        .peer_initial_sequence = SequenceNumber{100},
        .peer_socket_id = 900,
        .send_capacity_packets = 4,
        .receive_capacity_packets = 4,
        .maximum_payload_size = 1,
    }};
    sender.configure_file(false);
    const std::array<std::byte, 2> input{
        std::byte{'a'}, std::byte{'b'}};
    REQUIRE(sender.queue_stream(input, PacketTimestamp{0}));
    for (std::size_t index = 0; index < input.size(); ++index) {
        const auto packet = sender.next_data_packet();
        REQUIRE(packet.has_value());
        sender.note_data_packet_sent(100);
    }

    std::array<std::byte, 64> control_storage{};
    ReliabilityAction acknowledgement{
        .kind = ReliabilityActionKind::acknowledgement,
        .acknowledgement = {
            .kind = AcknowledgementKind::lite,
            .next_sequence = SequenceNumber{11},
        },
    };
    const auto ack_packet =
        encode_and_decode(acknowledgement, control_storage);
    REQUIRE(sender.receive(ack_packet, 300'000));
    REQUIRE(!sender.poll_sender_retransmission_timeout(630'000 - 1U));

    ReliabilityAction loss{
        .kind = ReliabilityActionKind::loss_report,
        .loss = {
            .first = SequenceNumber{11},
            .last = SequenceNumber{11},
        },
    };
    const auto nak_packet = encode_and_decode(loss, control_storage);
    REQUIRE(sender.receive(nak_packet, 629'999));
    REQUIRE(sender.poll_sender_retransmission_timeout(630'000));

    // LATEREXMIT preserves the existing loss selection rather than blindly
    // appending the whole flight.
    const auto retransmission = sender.next_data_packet();
    REQUIRE(retransmission.has_value());
    REQUIRE_EQ(retransmission->header.sequence, SequenceNumber{11});
    REQUIRE(retransmission->header.retransmitted);
    REQUIRE(!sender.next_data_packet().has_value());
}

TEST(file_session_sender_rto_uses_the_peer_full_ack_rtt_estimate)
{
    ReliabilitySession sender{{
        .local_initial_sequence = SequenceNumber{10},
        .peer_initial_sequence = SequenceNumber{100},
        .peer_socket_id = 900,
        .send_capacity_packets = 4,
        .receive_capacity_packets = 4,
        .maximum_payload_size = 1,
    }};
    sender.configure_file(false);
    const std::array<std::byte, 2> input{
        std::byte{'a'}, std::byte{'b'}};
    REQUIRE(sender.queue_stream(input, PacketTimestamp{0}));
    for (std::size_t index = 0; index < input.size(); ++index) {
        const auto packet = sender.next_data_packet();
        REQUIRE(packet.has_value());
        sender.note_data_packet_sent(100);
    }

    std::array<std::byte, 64> control_storage{};
    ReliabilityAction acknowledgement{
        .kind = ReliabilityActionKind::acknowledgement,
        .acknowledgement = {
            .kind = AcknowledgementKind::full,
            .acknowledgement_number = 1,
            .next_sequence = SequenceNumber{11},
            .round_trip_time_microseconds = 40'000,
            .round_trip_time_variance_microseconds = 5'000,
        },
    };
    const auto ack_packet =
        encode_and_decode(acknowledgement, control_storage);
    REQUIRE(sender.receive(ack_packet, 1'000));
    REQUIRE_EQ(sender.rtt().smoothed_microseconds(), 40'000U);
    REQUIRE_EQ(sender.rtt().variation_microseconds(), 5'000U);

    // 1 * (40 ms + 4 * 5 ms + 2 * 10 ms) + 10 ms = 90 ms.
    REQUIRE(!sender.poll_sender_retransmission_timeout(90'999));
    REQUIRE(sender.poll_sender_retransmission_timeout(91'000));
}

TEST(live_session_periodic_nak_retains_sender_rto_for_flight_tail_loss)
{
    ReliabilitySession sender {{
        .local_initial_sequence = SequenceNumber {10},
        .peer_initial_sequence = SequenceNumber {100},
        .peer_socket_id = 900,
        .send_capacity_packets = 4,
        .receive_capacity_packets = 4,
        .maximum_payload_size = 1,
    }};
    sender.configure_live(
        NegotiatedLiveOptions {.periodic_nak = true}, 0, PacketTimestamp {0});
    const std::array<std::byte, 2> input {std::byte {'x'}, std::byte {'y'}};
    for (const auto byte : input) {
        REQUIRE_EQ(
            sender.queue_message(std::span {&byte, 1}, PacketTimestamp {0}),
            Error::none);
        const auto packet = sender.next_data_packet();
        REQUIRE(packet.has_value());
        sender.note_data_packet_sent(100);
    }

    std::array<std::byte, 64> control_storage {};
    ReliabilityAction acknowledgement {
        .kind = ReliabilityActionKind::acknowledgement,
        .acknowledgement =
            {
                .kind = AcknowledgementKind::lite,
                .next_sequence = SequenceNumber {11},
            },
    };
    REQUIRE(sender.receive(
        encode_and_decode(acknowledgement, control_storage), 1'000));

    REQUIRE(!sender.poll_sender_retransmission_timeout(330'999));
    REQUIRE(sender.poll_sender_retransmission_timeout(331'000));
    const auto retransmission = sender.next_data_packet();
    REQUIRE(retransmission.has_value());
    REQUIRE_EQ(retransmission->header.sequence, SequenceNumber {11});
    REQUIRE(retransmission->header.retransmitted);
    REQUIRE(!sender.next_data_packet().has_value());
}

TEST(live_session_duplicate_ack_does_not_starve_tail_retransmission)
{
    ReliabilitySession sender {{
        .local_initial_sequence = SequenceNumber {10},
        .peer_initial_sequence = SequenceNumber {100},
        .peer_socket_id = 900,
        .send_capacity_packets = 4,
        .receive_capacity_packets = 4,
        .maximum_payload_size = 1,
    }};
    sender.configure_live(
        NegotiatedLiveOptions {.periodic_nak = true}, 0, PacketTimestamp {0});
    const std::array<std::byte, 2> input {std::byte {'x'}, std::byte {'y'}};
    for (const auto byte : input) {
        REQUIRE_EQ(
            sender.queue_message(std::span {&byte, 1}, PacketTimestamp {0}),
            Error::none);
        const auto packet = sender.next_data_packet();
        REQUIRE(packet.has_value());
        sender.note_data_packet_sent(100);
    }

    std::array<std::byte, 64> control_storage {};
    ReliabilityAction acknowledgement {
        .kind = ReliabilityActionKind::acknowledgement,
        .acknowledgement =
            {
                .kind = AcknowledgementKind::lite,
                .next_sequence = SequenceNumber {11},
            },
    };
    REQUIRE(sender.receive(
        encode_and_decode(acknowledgement, control_storage), 1'000));

    // Full/lite ACKs can repeat the same next sequence while the final DATA
    // packet is missing. They are not progress and must not postpone RTO.
    for (std::uint64_t now = 10'000; now <= 330'000; now += 10'000) {
        REQUIRE(sender.receive(
            encode_and_decode(acknowledgement, control_storage), now));
    }

    REQUIRE(!sender.poll_sender_retransmission_timeout(330'999));
    REQUIRE(sender.poll_sender_retransmission_timeout(331'000));
    const auto retransmission = sender.next_data_packet();
    REQUIRE(retransmission.has_value());
    REQUIRE_EQ(retransmission->header.sequence, SequenceNumber {11});
    REQUIRE(retransmission->header.retransmitted);
    REQUIRE(!sender.next_data_packet().has_value());
}

TEST(live_session_duplicate_full_ack_does_not_starve_tail_retransmission)
{
    ReliabilitySession sender {{
        .local_initial_sequence = SequenceNumber {10},
        .peer_initial_sequence = SequenceNumber {100},
        .peer_socket_id = 900,
        .send_capacity_packets = 4,
        .receive_capacity_packets = 4,
        .maximum_payload_size = 1,
    }};
    sender.configure_live(
        NegotiatedLiveOptions {.periodic_nak = true}, 0, PacketTimestamp {0});
    const std::array<std::byte, 2> input {std::byte {'x'}, std::byte {'y'}};
    for (const auto byte : input) {
        REQUIRE_EQ(
            sender.queue_message(std::span {&byte, 1}, PacketTimestamp {0}),
            Error::none);
        const auto packet = sender.next_data_packet();
        REQUIRE(packet.has_value());
        sender.note_data_packet_sent(100);
    }

    std::array<std::byte, 64> control_storage {};
    ReliabilityAction acknowledgement {
        .kind = ReliabilityActionKind::acknowledgement,
        .acknowledgement =
            {
                .kind = AcknowledgementKind::full,
                .acknowledgement_number = 1,
                .next_sequence = SequenceNumber {11},
                .round_trip_time_microseconds = 100'000,
                .round_trip_time_variance_microseconds = 50'000,
            },
    };
    REQUIRE(sender.receive(
        encode_and_decode(acknowledgement, control_storage), 1'000));

    // Full/lite ACKs can repeat the same next sequence while the final DATA
    // packet is missing. They are not progress and must not postpone RTO.
    for (std::uint64_t now = 10'000; now <= 330'000; now += 10'000) {
        REQUIRE(sender.receive(
            encode_and_decode(acknowledgement, control_storage), now));
    }

    REQUIRE(!sender.poll_sender_retransmission_timeout(330'999));
    REQUIRE(sender.poll_sender_retransmission_timeout(331'000));
    const auto retransmission = sender.next_data_packet();
    REQUIRE(retransmission.has_value());
    REQUIRE_EQ(retransmission->header.sequence, SequenceNumber {11});
    REQUIRE(retransmission->header.retransmitted);
    REQUIRE(!sender.next_data_packet().has_value());
}

TEST(filter_and_file_duplicate_ack_preserves_recovery_timer)
{
    for (const auto kind :
        {AcknowledgementKind::lite, AcknowledgementKind::full}) {
        for (int mode = 0; mode < 3; ++mode) {
            ReliabilitySession sender {{
                .local_initial_sequence = SequenceNumber {10},
                .peer_initial_sequence = SequenceNumber {100},
                .peer_socket_id = 900,
                .send_capacity_packets = 4,
                .receive_capacity_packets = 4,
                .maximum_payload_size = 1,
            }};
            if (mode == 2) {
                sender.configure_file(true);
            } else {
                sender.configure_live(
                    NegotiatedLiveOptions {.periodic_nak = true}, 0,
                    PacketTimestamp {0});
                const auto filter = parse_packet_filter_configuration(mode == 0
                        ? "fec,cols:4,rows:1,arq:onreq"
                        : "fec,cols:4,rows:1,arq:never");
                REQUIRE(filter);
                sender.configure_packet_filter(filter.configuration, true);
            }
            const std::array<std::byte, 1> input {std::byte {'x'}};
            for (int i = 0; i < 2; ++i) {
                REQUIRE_EQ(sender.queue_message(input, PacketTimestamp {0}),
                    Error::none);
                REQUIRE(sender.next_data_packet().has_value());
                sender.note_data_packet_sent(100);
            }
            std::array<std::byte, 64> storage {};
            ReliabilityAction acknowledgement {
                .kind = ReliabilityActionKind::acknowledgement,
                .acknowledgement =
                    {
                        .kind = kind,
                        .acknowledgement_number = 1,
                        .next_sequence = SequenceNumber {11},
                        .round_trip_time_microseconds = 100'000,
                        .round_trip_time_variance_microseconds = 50'000,
                    },
            };
            REQUIRE(sender.receive(
                encode_and_decode(acknowledgement, storage), 1'000));
            for (std::uint64_t now = 10'000; now <= 330'000; now += 10'000) {
                REQUIRE(sender.receive(
                    encode_and_decode(acknowledgement, storage), now));
            }
            REQUIRE(!sender.poll_sender_retransmission_timeout(331'000));
            REQUIRE(!sender.next_data_packet().has_value());
        }
    }
}

TEST(live_session_periodic_nak_preserves_selected_loss_at_sender_rto)
{
    ReliabilitySession sender {{
        .local_initial_sequence = SequenceNumber {10},
        .peer_initial_sequence = SequenceNumber {100},
        .peer_socket_id = 900,
        .send_capacity_packets = 4,
        .receive_capacity_packets = 4,
        .maximum_payload_size = 1,
    }};
    sender.configure_live(
        NegotiatedLiveOptions {.periodic_nak = true}, 0, PacketTimestamp {0});
    const std::array<std::byte, 3> input {
        std::byte {'x'}, std::byte {'y'}, std::byte {'z'}};
    for (const auto byte : input) {
        REQUIRE_EQ(
            sender.queue_message(std::span {&byte, 1}, PacketTimestamp {0}),
            Error::none);
        const auto packet = sender.next_data_packet();
        REQUIRE(packet.has_value());
        sender.note_data_packet_sent(100);
    }

    std::array<std::byte, 64> control_storage {};
    ReliabilityAction acknowledgement {
        .kind = ReliabilityActionKind::acknowledgement,
        .acknowledgement =
            {
                .kind = AcknowledgementKind::lite,
                .next_sequence = SequenceNumber {11},
            },
    };
    REQUIRE(sender.receive(
        encode_and_decode(acknowledgement, control_storage), 1'000));

    ReliabilityAction loss {
        .kind = ReliabilityActionKind::loss_report,
        .loss =
            {
                .first = SequenceNumber {11},
                .last = SequenceNumber {11},
            },
    };
    REQUIRE(sender.receive(encode_and_decode(loss, control_storage), 2'000));

    REQUIRE(sender.poll_sender_retransmission_timeout(331'000));
    const auto retransmission = sender.next_data_packet();
    REQUIRE(retransmission.has_value());
    REQUIRE_EQ(retransmission->header.sequence, SequenceNumber {11});
    REQUIRE(retransmission->header.retransmitted);
    REQUIRE(!sender.next_data_packet().has_value());
}

TEST(session_consumes_negotiated_fec_control_without_advancing_sequence)
{
    const auto filter =
        parse_packet_filter_configuration(
            "fec,cols:10");
    REQUIRE(filter);
    ReliabilitySession receiver{{
        .local_initial_sequence = SequenceNumber{1},
        .peer_initial_sequence = SequenceNumber{100},
        .send_capacity_packets = 8,
        .receive_capacity_packets = 8,
    }};
    receiver.configure_packet_filter(
        filter.configuration);

    const std::array<std::byte, 4> parity{
        std::byte{0xff}, std::byte{0},
        std::byte{0}, std::byte{4}};
    PacketView control;
    control.kind = PacketKind::data;
    control.data.sequence = SequenceNumber{900};
    control.data.message_number = 0U;
    control.data.boundary = MessageBoundary::solo;
    control.payload = parity;
    const auto consumed = receiver.receive(
        control, 1'000);
    REQUIRE(consumed);
    REQUIRE(consumed.receiver_filter_control_packet);
    REQUIRE(!consumed.receiver_packet_accepted_unique);
    REQUIRE_EQ(consumed.actions.size, 0U);
    REQUIRE_EQ(receiver.receive_buffer()
            .next_ack_sequence(),
        SequenceNumber{100});

    const std::array<std::byte, 1> payload{
        std::byte{'x'}};
    PacketView data;
    data.kind = PacketKind::data;
    data.data.sequence = SequenceNumber{100};
    data.data.message_number = 1U;
    data.data.boundary = MessageBoundary::solo;
    data.payload = payload;
    const auto accepted = receiver.receive(
        data, 2'000);
    REQUIRE(accepted);
    REQUIRE(accepted.receiver_packet_accepted_unique);
    REQUIRE(!accepted.receiver_filter_control_packet);
    REQUIRE_EQ(receiver.receive_buffer()
            .next_ack_sequence(),
        SequenceNumber{101});
}

TEST(session_honors_explicit_packet_filter_arq_never)
{
    const auto filter =
        parse_packet_filter_configuration(
            "fec,cols:10,arq:never");
    REQUIRE(filter);
    ReliabilitySession receiver{{
        .local_initial_sequence = SequenceNumber{1},
        .peer_initial_sequence = SequenceNumber{100},
        .send_capacity_packets = 8,
        .receive_capacity_packets = 8,
    }};
    receiver.configure_packet_filter(
        filter.configuration);

    const std::array<std::byte, 1> payload{
        std::byte{'x'}};
    PacketView data;
    data.kind = PacketKind::data;
    data.data.sequence = SequenceNumber{102};
    data.data.message_number = 1U;
    data.data.boundary = MessageBoundary::solo;
    data.payload = payload;
    const auto gap = receiver.receive(
        data, 1'000);
    REQUIRE(gap);
    REQUIRE_EQ(gap.receiver_loss_packets, 2U);
    REQUIRE_EQ(gap.actions.size, 1U);
    REQUIRE_EQ(gap.actions.values[0].kind,
        ReliabilityActionKind::acknowledgement);

    const auto timers =
        receiver.poll_timers(1'000'000);
    for (std::size_t index = 0;
         index < timers.size; ++index) {
        REQUIRE(timers.values[index].kind
            != ReliabilityActionKind::loss_report);
    }
}

TEST(session_reports_only_filter_declared_losses_for_onreq_arq)
{
    const auto filter =
        parse_packet_filter_configuration(
            "fec,cols:4,rows:1,arq:onreq");
    REQUIRE(filter);
    ReliabilitySession receiver{{
        .local_initial_sequence = SequenceNumber{1},
        .peer_initial_sequence = SequenceNumber{100},
        .send_capacity_packets = 16,
        .receive_capacity_packets = 16,
    }};
    receiver.configure_live({
        .periodic_nak = true,
        .retransmit_flag = true,
    }, 0, PacketTimestamp{0});
    receiver.configure_packet_filter(
        filter.configuration, true);

    const std::array payload{std::byte{'x'}};
    PacketView packet{
        .kind = PacketKind::data,
        .data = {
            .sequence = SequenceNumber{102},
            .message_number = 3,
            .boundary = MessageBoundary::solo,
        },
        .payload = payload,
    };
    const auto physical_gap =
        receiver.receive(packet, 100U);
    REQUIRE(physical_gap);
    REQUIRE_EQ(
        physical_gap.receiver_loss_packets, 2U);
    REQUIRE_EQ(physical_gap.actions.size, 0U);

    const std::array losses{SequenceRange{
        .first = SequenceNumber{100},
        .last = SequenceNumber{101},
    }};
    const auto handed_off =
        receiver.report_filter_losses(losses, 200U);
    REQUIRE(handed_off);
    REQUIRE_EQ(
        handed_off.receiver_filter_loss_packets, 2U);
    REQUIRE_EQ(handed_off.actions.size, 1U);
    REQUIRE_EQ(
        handed_off.actions.values[0].kind,
        ReliabilityActionKind::loss_report);
    REQUIRE_EQ(
        handed_off.actions.values[0].loss.first,
        SequenceNumber{100});
    REQUIRE_EQ(
        handed_off.actions.values[0].loss.last,
        SequenceNumber{101});

    packet.data.sequence = SequenceNumber{100};
    packet.data.message_number = 1U;
    packet.data.retransmitted = true;
    REQUIRE(receiver.receive(packet, 300U));

    const auto periodic =
        receiver.poll_timers(1'000'000U);
    bool saw_remaining_loss = false;
    for (std::size_t index = 0;
         index < periodic.size; ++index) {
        if (periodic.values[index].kind
            != ReliabilityActionKind::loss_report) {
            continue;
        }
        const auto ranges = periodic.loss_ranges(
            periodic.values[index]);
        REQUIRE_EQ(ranges.size(), 1U);
        REQUIRE_EQ(
            ranges[0].first, SequenceNumber{101});
        REQUIRE_EQ(
            ranges[0].last, SequenceNumber{101});
        saw_remaining_loss = true;
    }
    REQUIRE(saw_remaining_loss);
}

TEST(session_rejects_filter_loss_batches_transactionally)
{
    const auto filter =
        parse_packet_filter_configuration("fec,cols:4,rows:1,arq:onreq");
    REQUIRE(filter);
    ReliabilitySession receiver {{
        .local_initial_sequence = SequenceNumber {1},
        .peer_initial_sequence = SequenceNumber {100},
        .send_capacity_packets = 8,
        .receive_capacity_packets = 8,
    }};
    receiver.configure_packet_filter(filter.configuration, true);

    const std::array invalid_losses {
        SequenceRange {
            .first = SequenceNumber {100},
            .last = SequenceNumber {100},
        },
        SequenceRange {
            .first = SequenceNumber {100},
            .last = SequenceNumber {101},
        },
    };
    const auto rejected = receiver.report_filter_losses(invalid_losses, 100U);
    REQUIRE(!rejected);
    REQUIRE_EQ(rejected.error, Error::buffer_too_small);

    const std::array valid_losses {SequenceRange {
        .first = SequenceNumber {100},
        .last = SequenceNumber {100},
    }};
    const auto accepted = receiver.report_filter_losses(valid_losses, 200U);
    REQUIRE(accepted);
    REQUIRE_EQ(accepted.receiver_filter_loss_packets, 1U);
    REQUIRE_EQ(accepted.actions.size, 1U);
    REQUIRE_EQ(
        accepted.actions.values[0].kind, ReliabilityActionKind::loss_report);
}
