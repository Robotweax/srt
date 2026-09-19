#include "test.hpp"
#include "robotweax/srt/session.hpp"
#include "robotweax/srt/codec.hpp"
#include <array>

using namespace robotweax::srt;
namespace {
ReliabilityProcessResult control(
    ReliabilitySession& session, ReliabilityAction action, std::uint64_t now)
{
    std::array<std::byte, 128> storage {};
    auto encoded =
        encode_reliability_action(action, PacketTimestamp {0}, 77, storage);
    REQUIRE(encoded);
    auto decoded =
        decode_packet(std::span {storage}.first(encoded.bytes_written));
    REQUIRE(decoded);
    return session.receive(decoded.packet, now);
}
ReliabilityProcessResult ack(ReliabilitySession& s, SequenceNumber next)
{
    return control(s,
        {.kind = ReliabilityActionKind::acknowledgement,
            .acknowledgement = {.kind = AcknowledgementKind::lite,
                .next_sequence = next}},
        2000);
}
ReliabilityProcessResult nak(
    ReliabilitySession& s, SequenceNumber first, SequenceNumber last)
{
    return control(s,
        {.kind = ReliabilityActionKind::loss_report, .loss = {first, last}},
        4000);
}
}
TEST(drop_race_splits_acknowledged_abandoned_and_current_sources)
{
    for (auto first :
        {SequenceNumber {10}, SequenceNumber {SequenceNumber::mask - 1}}) {
        ReliabilitySession s {{.local_initial_sequence = first,
            .peer_initial_sequence = SequenceNumber {100},
            .send_capacity_packets = 8,
            .receive_capacity_packets = 8,
            .maximum_payload_size = 1}};
        const std::array<std::byte, 1> payload {std::byte {'x'}};
        REQUIRE_EQ(s.queue_message(payload, PacketTimestamp {0}, true, 1000),
            Error::none);
        REQUIRE_EQ(
            s.queue_message(payload, PacketTimestamp {0}, true, 1000, 2500),
            Error::none);
        REQUIRE_EQ(s.queue_message(payload, PacketTimestamp {0}, true, 1000),
            Error::none);
        for (int i = 0; i < 3; ++i)
            REQUIRE(s.next_data_packet().has_value());
        REQUIRE(ack(s, first.next()));
        REQUIRE(
            ack(s, first)); // Older ACK must not move the boundary backwards.
        REQUIRE(!ack(
            s, first.advanced(10))); // Invalid future ACK must not advance it.
        auto ttl = s.drop_expired_sender_message(2501);
        REQUIRE_EQ(ttl.size, 1U);
        REQUIRE_EQ(ttl.values[0].drop.message_number, 2U);
        auto response = nak(s, first, first.advanced(2));
        REQUIRE(response);
        REQUIRE_EQ(response.actions.size, 1U);
        REQUIRE_EQ(response.actions.values[0].kind,
            ReliabilityActionKind::drop_request);
        REQUIRE_EQ(
            response.actions.values[0].drop.sequences.first, first.next());
        REQUIRE_EQ(
            response.actions.values[0].drop.sequences.last, first.next());
        auto retransmission = s.next_data_packet();
        REQUIRE(retransmission.has_value());
        REQUIRE_EQ(retransmission->header.sequence, first.advanced(2));
        REQUIRE(retransmission->header.retransmitted);
        auto repeated = nak(
            s, first.next(), first.next()); // Lost DROPREQ remains recoverable.
        REQUIRE(repeated);
        REQUIRE_EQ(repeated.actions.size, 1U);
        REQUIRE_EQ(
            repeated.actions.values[0].drop.sequences.first, first.next());
    }
}
TEST(drop_race_partial_message_ack_does_not_hide_abandoned_tail)
{
    for (auto first :
        {SequenceNumber {10}, SequenceNumber {SequenceNumber::mask}}) {
        ReliabilitySession s {{.local_initial_sequence = first,
            .peer_initial_sequence = SequenceNumber {100},
            .send_capacity_packets = 8,
            .receive_capacity_packets = 8,
            .maximum_payload_size = 1}};
        const std::array<std::byte, 3> payload {};
        REQUIRE_EQ(
            s.queue_message(payload, PacketTimestamp {0}, true, 1000, 2500),
            Error::none);
        for (int i = 0; i < 3; ++i)
            REQUIRE(s.next_data_packet().has_value());
        REQUIRE(ack(s, first.next()));
        auto ttl = s.drop_expired_sender_message(2501);
        REQUIRE_EQ(ttl.size, 1U);
        REQUIRE_EQ(ttl.values[0].drop.message_number, 1U);
        auto response = nak(s, first, first.advanced(2));
        REQUIRE(response);
        REQUIRE_EQ(response.actions.size, 1U);
        REQUIRE_EQ(
            response.actions.values[0].drop.sequences.first, first.next());
        REQUIRE_EQ(
            response.actions.values[0].drop.sequences.last, first.advanced(2));
    }
}
TEST(drop_race_ack_of_abandoned_sources_stops_obsolete_drop_replies)
{
    ReliabilitySession s {{.local_initial_sequence = SequenceNumber {10},
        .peer_initial_sequence = SequenceNumber {100},
        .send_capacity_packets = 8,
        .receive_capacity_packets = 8,
        .maximum_payload_size = 1}};
    const std::array<std::byte, 1> payload {};
    REQUIRE_EQ(s.queue_message(payload, PacketTimestamp {0}, true, 1000, 2500),
        Error::none);
    REQUIRE(s.next_data_packet().has_value());
    REQUIRE_EQ(s.drop_expired_sender_message(2501).size, 1U);
    REQUIRE_EQ(
        nak(s, SequenceNumber {10}, SequenceNumber {10}).actions.size, 1U);
    REQUIRE(ack(s, SequenceNumber {11}));
    REQUIRE_EQ(
        nak(s, SequenceNumber {10}, SequenceNumber {10}).actions.size, 0U);
}

namespace {
PacketView wire(
    const ReliabilityAction& action, std::array<std::byte, 128>& bytes)
{
    auto encoded =
        encode_reliability_action(action, PacketTimestamp {0}, 77, bytes);
    REQUIRE(bool(encoded));
    auto decoded =
        decode_packet(std::span {bytes}.first(encoded.bytes_written));
    REQUIRE(bool(decoded));
    return decoded.packet;
}
void verify_tsbpd_delivery(SequenceNumber initial, bool delayed_nak)
{
    ReliabilitySession sender {{.local_initial_sequence = initial,
        .peer_initial_sequence = SequenceNumber {100},
        .peer_socket_id = 900,
        .send_capacity_packets = 8,
        .receive_capacity_packets = 8}};
    ReliabilitySession receiver {
        {.local_initial_sequence = SequenceNumber {100},
            .peer_initial_sequence = initial,
            .peer_socket_id = 800,
            .send_capacity_packets = 8,
            .receive_capacity_packets = 8}};
    receiver.configure_live({.receive_tsbpd = true,
                                .too_late_packet_drop = true,
                                .receive_delay_milliseconds = 200},
        0, PacketTimestamp {0});
    std::array<std::byte, 1> payload {std::byte {'x'}}, output {};
    REQUIRE(sender.queue_message(payload, PacketTimestamp {1'000}, true, 1'000)
        == Error::none);
    auto packet = sender.next_data_packet();
    REQUIRE(packet.has_value());
    auto accepted = receiver.receive({.kind = PacketKind::data,
                                         .data = packet->header,
                                         .payload = packet->payload},
        10'000);
    REQUIRE(bool(accepted) && accepted.receiver_packet_accepted_unique);
    std::array<std::byte, 128> storage {};
    ReliabilityAction ack {.kind = ReliabilityActionKind::acknowledgement,
        .acknowledgement = {.kind = AcknowledgementKind::lite,
            .next_sequence = initial.next()}};
    REQUIRE(bool(sender.receive(wire(ack, storage), 20'000)));
    REQUIRE(sender.send_buffer().size() == 0);
    REQUIRE(receiver.receive_buffer().occupied() == 1);
    REQUIRE(
        receiver.pop_message_at(output, 25'000).error == Error::would_block);
    if (delayed_nak) {
        ReliabilityAction nak {.kind = ReliabilityActionKind::loss_report,
            .loss = {initial, initial}};
        auto reply = sender.receive(wire(nak, storage), 30'000);
        REQUIRE(bool(reply));
        REQUIRE(reply.actions.size == 0);
        REQUIRE(bool(receiver.pop_message_at(output, 300'000))
            && output == payload);
    } else {
        REQUIRE(bool(receiver.pop_message_at(output, 300'000))
            && output == payload);
    }
}
}

TEST(drop_race_preserves_acknowledged_messages_until_tsbpd_delivery)
{
    for (auto initial :
        {SequenceNumber {10}, SequenceNumber {SequenceNumber::mask}}) {
        verify_tsbpd_delivery(initial, false);
        verify_tsbpd_delivery(initial, true);
    }
}
