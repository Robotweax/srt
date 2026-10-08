#include "test.hpp"
#include "receive_buffer_test_access.hpp"

#include "robotweax/srt/codec.hpp"
#include "robotweax/srt/session.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <limits>
#include <map>
#include <optional>
#include <vector>

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

// Show the receiver that the peer has sent up to `sequence`. A DROPREQ may
// only advance acknowledgement and window over sequences the peer has
// demonstrably sent; the probe may fall beyond the receive window and is
// then rejected, but it still raises the observed send horizon.
void observe_peer_horizon(ReliabilitySession& receiver, SequenceNumber sequence,
    std::uint64_t now_microseconds)
{
    static const std::array<std::byte, 1> payload {std::byte {'h'}};
    PacketView probe;
    probe.kind = PacketKind::data;
    probe.data.sequence = sequence;
    probe.data.message_number = 0x3ff'ffffU;
    probe.data.boundary = MessageBoundary::solo;
    probe.data.timestamp = PacketTimestamp {0};
    probe.payload = payload;
    REQUIRE(receiver.receive(probe, now_microseconds));
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

TEST(session_drop_request_rejects_ambiguous_progress_transactionally)
{
    constexpr auto limit = SequenceNumber::half_range / 2U;
    for (const auto initial :
        {SequenceNumber {100}, SequenceNumber {SequenceNumber::mask - 5U}}) {
        const std::array ranges {
            SequenceRange {initial, initial.advanced(limit)},
            SequenceRange {
                initial.advanced(limit + 2U), initial.advanced(limit + 2U)},
            SequenceRange {
                initial.advanced(SequenceNumber::modulus - limit - 2U),
                initial.advanced(SequenceNumber::mask)},
        };
        for (const bool deferred : {false, true}) {
            for (const auto range : ranges) {
                ReliabilitySession receiver {{
                    .peer_initial_sequence = initial,
                    .send_capacity_packets = 8,
                    .receive_capacity_packets = 8,
                }};
                receiver.configure_live(
                    {
                        .receive_tsbpd = deferred,
                        .too_late_packet_drop = deferred,
                        .receive_delay_milliseconds = 100,
                    },
                    1'000, PacketTimestamp {0});
                const std::array payload {std::byte {'p'}};
                PacketView data;
                data.kind = PacketKind::data;
                data.data.sequence = initial;
                data.data.message_number = 1;
                data.data.boundary = MessageBoundary::solo;
                data.payload = payload;
                REQUIRE(receiver.receive(data, 1'001));
                const auto before = receiver.next_receive_delivery_time();
                std::array<std::byte, 64> storage {};
                auto packet = encode_and_decode(
                    {
                        .kind = ReliabilityActionKind::drop_request,
                        .drop = {0, range},
                    },
                    storage);
                packet.control.timestamp = PacketTimestamp {0x7fff'fff0U};
                const auto result = receiver.receive(packet, 1'002);
                REQUIRE_EQ(result.error, Error::invalid_control_payload);
                REQUIRE_EQ(result.actions.size, 0U);
                REQUIRE_EQ(result.receiver_drop_packets, 0U);
                REQUIRE_EQ(receiver.receive_buffer().next_ack_sequence(),
                    initial.next());
                REQUIRE_EQ(
                    receiver.receive_buffer().first_stored_sequence(), initial);
                REQUIRE_EQ(receiver.receive_buffer().occupied(), 1U);
                REQUIRE_EQ(receiver.next_receive_delivery_time(), before);
            }
        }
    }
}

TEST(session_drop_request_preserves_large_ttl_and_group_sequence_skips)
{
    constexpr std::uint32_t count = 5'000;
    for (const auto initial :
        {SequenceNumber {101}, SequenceNumber {SequenceNumber::mask - 100U}}) {
        for (const bool group_skip : {false, true}) {
            ReliabilitySession sender {{
                .local_initial_sequence = initial,
                .send_capacity_packets = count,
                .receive_capacity_packets = 8,
                .maximum_payload_size = 1,
            }};
            ReliabilityActions drops;
            if (group_skip) {
                REQUIRE_EQ(sender.skip_group_sequences(initial.advanced(count)),
                    Error::none);
                drops = sender.take_pending_drop_requests();
            } else {
                const std::vector payload(count, std::byte {'t'});
                REQUIRE_EQ(sender.queue_message(
                               payload, PacketTimestamp {0}, true, 0, 1),
                    Error::none);
                drops = sender.drop_expired_sender_message(2);
            }
            REQUIRE_EQ(drops.size, 1U);
            REQUIRE_EQ(drops.values[0].drop.sequences.first, initial);
            REQUIRE_EQ(drops.values[0].drop.sequences.last,
                initial.advanced(count - 1U));
            for (const bool deferred : {false, true}) {
                ReliabilitySession receiver {{
                    .peer_initial_sequence = initial,
                    .send_capacity_packets = 8,
                    .receive_capacity_packets = 8,
                }};
                receiver.configure_live(
                    {
                        .receive_tsbpd = deferred,
                        .too_late_packet_drop = deferred,
                        .receive_delay_milliseconds = 100,
                    },
                    1'000, PacketTimestamp {0});
                std::array<std::byte, 64> storage {};
                auto packet = encode_and_decode(drops.values[0], storage);
                packet.control.timestamp = PacketTimestamp {20};
                // The request covers sequences this receiver has never
                // observed. It must not advance the acknowledgement or the
                // window on its own: an unauthenticated DROPREQ could
                // otherwise acknowledge sequences the peer never sent.
                REQUIRE(receiver.receive(packet, 1'020));
                REQUIRE_EQ(
                    receiver.receive_buffer().next_ack_sequence(), initial);
                REQUIRE_EQ(
                    receiver.receive_buffer().first_stored_sequence(), initial);
                // The first DATA beyond the range proves the peer's position
                // and applies the whole skip.
                const std::array payload {std::byte {'n'}};
                PacketView data;
                data.kind = PacketKind::data;
                data.data.sequence = initial.advanced(count);
                data.data.message_number = 2;
                data.data.boundary = MessageBoundary::solo;
                data.data.timestamp = PacketTimestamp {25};
                data.payload = payload;
                const auto applied = receiver.receive(data, 1'025);
                REQUIRE(applied);
                REQUIRE_EQ(applied.receiver_loss_packets, 0U);
                if (deferred) {
                    REQUIRE_EQ(receiver.receive_buffer().next_ack_sequence(),
                        initial.advanced(count));
                    REQUIRE_EQ(
                        receiver.receive_buffer().first_stored_sequence(),
                        initial);
                    REQUIRE_EQ(receiver.drop_too_late_receiver(101'020)
                                   .receiver_drop_packets,
                        count);
                } else {
                    REQUIRE_EQ(applied.receiver_drop_packets, count);
                    REQUIRE(applied.receiver_packet_accepted_unique);
                    REQUIRE_EQ(receiver.receive_buffer().next_ack_sequence(),
                        initial.advanced(count + 1U));
                }
                REQUIRE_EQ(receiver.receive_buffer().first_stored_sequence(),
                    initial.advanced(count));
            }
        }
    }
}

TEST(session_drop_request_cannot_acknowledge_unobserved_sequences)
{
    // Contract: a DROPREQ advances acknowledgement and window only over
    // sequences the peer has demonstrably sent. A request for the maximum
    // plausible range starting at the next expected sequence must neither
    // acknowledge sequences never seen nor turn later genuine packets into
    // duplicates.
    constexpr auto limit = SequenceNumber::half_range / 2U;
    for (const bool deferred : {false, true}) {
        const SequenceNumber initial {100};
        ReliabilitySession receiver {{
            .peer_initial_sequence = initial,
            .send_capacity_packets = 8,
            .receive_capacity_packets = 8'192,
        }};
        receiver.configure_live(
            {
                .receive_tsbpd = deferred,
                .too_late_packet_drop = deferred,
                .receive_delay_milliseconds = 100,
            },
            1'000, PacketTimestamp {0});
        const std::array payload {std::byte {'p'}};
        PacketView data;
        data.kind = PacketKind::data;
        data.data.boundary = MessageBoundary::solo;
        data.payload = payload;
        for (std::uint32_t index = 0; index < 10U; ++index) {
            data.data.sequence = initial.advanced(index);
            data.data.message_number = index + 1U;
            data.data.timestamp = PacketTimestamp {10U + index};
            REQUIRE(receiver.receive(data, 1'010 + index));
        }
        const auto next = initial.advanced(10U);
        REQUIRE_EQ(receiver.receive_buffer().next_ack_sequence(), next);

        std::array<std::byte, 64> storage {};
        auto packet = encode_and_decode(
            {
                .kind = ReliabilityActionKind::drop_request,
                .drop = {0, {next, next.advanced(limit - 2U)}},
            },
            storage);
        packet.control.timestamp = PacketTimestamp {20};
        const auto held = receiver.receive(packet, 1'020);
        REQUIRE(held);
        REQUIRE_EQ(held.actions.size, 0U);
        REQUIRE_EQ(held.receiver_drop_packets, 0U);
        REQUIRE_EQ(receiver.receive_buffer().next_ack_sequence(), next);
        REQUIRE_EQ(receiver.receive_buffer().occupied(), 10U);

        data.data.sequence = next;
        data.data.message_number = 11U;
        data.data.timestamp = PacketTimestamp {21};
        const auto genuine = receiver.receive(data, 1'021);
        REQUIRE(genuine);
        REQUIRE(genuine.receiver_packet_accepted_unique);
        REQUIRE(!genuine.receiver_packet_belated);
        REQUIRE_EQ(genuine.receiver_drop_packets, 0U);
        REQUIRE_EQ(genuine.receiver_loss_packets, 0U);
        REQUIRE_EQ(receiver.receive_buffer().next_ack_sequence(), next.next());
        REQUIRE_EQ(receiver.receive_buffer().occupied(), 11U);
        REQUIRE_EQ(
            receiver.drop_too_late_receiver(101'021).receiver_drop_packets, 0U);
        std::array<std::byte, 1> output {};
        for (std::uint32_t index = 0; index < 11U; ++index) {
            REQUIRE(receiver.pop_message_at(output, 101'021));
            REQUIRE_EQ(output, payload);
        }
    }
}

TEST(session_drop_request_observed_range_does_not_recreate_loss)
{
    for (const auto initial :
        {SequenceNumber {100}, SequenceNumber {SequenceNumber::mask - 5U}}) {
        for (const bool deferred : {false, true}) {
            ReliabilitySession receiver {{
                .peer_initial_sequence = initial,
                .send_capacity_packets = 8,
                .receive_capacity_packets = 8,
            }};
            receiver.configure_live({.receive_tsbpd = deferred,
                                        .too_late_packet_drop = deferred,
                                        .periodic_nak = true,
                                        .receive_delay_milliseconds = 100},
                1'000, PacketTimestamp {0});
            observe_peer_horizon(receiver, initial.advanced(10U), 1'001);
            std::array<std::byte, 64> storage {};
            const auto request = encode_and_decode(
                {.kind = ReliabilityActionKind::drop_request,
                    .drop = {0, {initial, initial.advanced(9U)}}},
                storage);
            REQUIRE(receiver.receive(request, 1'002));
            if (deferred) {
                REQUIRE(receiver.drop_too_late_receiver(101'002));
            }
            const std::array payload {std::byte {'p'}};
            PacketView data;
            data.kind = PacketKind::data;
            data.data.sequence = initial.advanced(10U);
            data.data.message_number = 1;
            data.data.boundary = MessageBoundary::solo;
            data.payload = payload;
            const auto received = receiver.receive(data, 101'003);
            REQUIRE(received);
            REQUIRE(received.receiver_packet_accepted_unique);
            REQUIRE_EQ(received.receiver_loss_packets, 0U);
            for (std::size_t index = 0; index < received.actions.size;
                ++index) {
                REQUIRE(received.actions.values[index].kind
                    != ReliabilityActionKind::loss_report);
            }
        }
    }
}

TEST(session_drop_request_remainder_survives_full_grace_queue)
{
    for (const auto initial :
        {SequenceNumber {100}, SequenceNumber {SequenceNumber::mask - 2U}}) {
        // Exercise both full consumption and partial trimming of the remainder.
        for (const auto proving_offset : {4U, 5U}) {
            ReliabilitySession receiver {{
                .peer_initial_sequence = initial,
                .send_capacity_packets = 8,
                .receive_capacity_packets = 2,
            }};
            receiver.configure_live({.receive_tsbpd = true,
                                        .too_late_packet_drop = true,
                                        .receive_delay_milliseconds = 100},
                1'000, PacketTimestamp {0});
            const std::array payload {std::byte {'p'}};
            PacketView data;
            data.kind = PacketKind::data;
            data.data.sequence = initial.next();
            data.data.message_number = 1;
            data.data.boundary = MessageBoundary::solo;
            data.payload = payload;
            REQUIRE(receiver.receive(data, 1'001));
            std::array<std::byte, 64> storage {};
            for (const auto last_offset : {0U, 1U, 4U}) {
                const auto first =
                    last_offset == 4U ? initial.advanced(2U) : initial;
                auto request = encode_and_decode(
                    {.kind = ReliabilityActionKind::drop_request,
                        .drop = {0, {first, initial.advanced(last_offset)}}},
                    storage);
                request.control.timestamp = PacketTimestamp {20};
                REQUIRE(receiver.receive(request, 1'030));
            }
            data.data.sequence = initial.advanced(proving_offset);
            data.data.message_number = 2;
            data.data.timestamp = PacketTimestamp {30};
            const auto blocked = receiver.receive(data, 1'040);
            REQUIRE_EQ(blocked.error, Error::would_block);
            REQUIRE_EQ(blocked.actions.size, 0U);
            REQUIRE_EQ(blocked.receiver_drop_packets, 0U);
            REQUIRE(receiver.drop_too_late_receiver(101'040));
            std::array<std::byte, 1> output {};
            REQUIRE(receiver.pop_message_at(output, 101'040));
            REQUIRE_EQ(output, payload);
            data.data.sequence = initial.advanced(5U);
            data.data.timestamp = PacketTimestamp {40};
            const auto retried = receiver.receive(data, 101'041);
            REQUIRE(retried);
            REQUIRE(retried.receiver_packet_accepted_unique);
            REQUIRE_EQ(retried.receiver_drop_packets, 3U);
            REQUIRE_EQ(retried.receiver_loss_packets, 0U);
            REQUIRE_EQ(receiver.receive_buffer().next_ack_sequence(),
                initial.advanced(6U));
        }
    }
}

TEST(session_drop_request_full_grace_queue_is_nonmutating_backpressure)
{
    const SequenceNumber initial {100};
    ReliabilitySession receiver {{
        .peer_initial_sequence = initial,
        .send_capacity_packets = 8,
        .receive_capacity_packets = 2,
    }};
    receiver.configure_live(
        {
            .receive_tsbpd = true,
            .too_late_packet_drop = true,
            .receive_delay_milliseconds = 100,
        },
        1'000, PacketTimestamp {0});
    observe_peer_horizon(receiver, initial.advanced(8U), 1'001);
    std::array<std::byte, 64> storage {};
    for (std::uint32_t index : {0U, 2U}) {
        const auto sequence = initial.advanced(index);
        auto packet = encode_and_decode(
            {
                .kind = ReliabilityActionKind::drop_request,
                .drop = {0, {sequence, sequence}},
            },
            storage);
        packet.control.timestamp = PacketTimestamp {20 + index};
        REQUIRE(receiver.receive(packet, 1'030));
    }
    const auto before = receiver.next_receive_delivery_time();
    auto packet = encode_and_decode(
        {
            .kind = ReliabilityActionKind::drop_request,
            .drop = {0, {initial.next(), initial.next()}},
        },
        storage);
    for (const auto timestamp : {0x7fff'fff0U, 0xffff'ffe0U}) {
        packet.control.timestamp = PacketTimestamp {timestamp};
        const auto full = receiver.receive(packet, 1'031);
        REQUIRE_EQ(full.error, Error::would_block);
        REQUIRE_EQ(full.actions.size, 0U);
        REQUIRE_EQ(
            receiver.receive_buffer().next_ack_sequence(), initial.next());
        REQUIRE_EQ(receiver.next_receive_delivery_time(), before);
    }
    packet = encode_and_decode(
        {
            .kind = ReliabilityActionKind::drop_request,
            .drop = {0, {initial, initial}},
        },
        storage);
    packet.control.timestamp = PacketTimestamp {90};
    REQUIRE(receiver.receive(packet, 1'032));
    REQUIRE_EQ(receiver.next_receive_delivery_time(), before);
    const std::array payload {std::byte {'p'}};
    PacketView data;
    data.kind = PacketKind::data;
    data.data.sequence = initial.next();
    data.data.message_number = 1;
    data.data.boundary = MessageBoundary::solo;
    data.data.timestamp = PacketTimestamp {40};
    data.payload = payload;
    REQUIRE(receiver.receive(data, 1'040));
    std::array<std::byte, 1> output {};
    REQUIRE_EQ(
        receiver.drop_too_late_receiver(101'040).receiver_drop_packets, 2U);
    REQUIRE(receiver.pop_message_at(output, 101'040));
    REQUIRE_EQ(output, payload);
    REQUIRE(!receiver.next_receive_delivery_time());
}

TEST(session_drop_request_index_preserves_wrapped_overlapping_grace_ranges)
{
    for (const auto initial :
        {SequenceNumber {100}, SequenceNumber {SequenceNumber::mask - 31U}}) {
        ReliabilitySession receiver {{
            .peer_initial_sequence = initial,
            .send_capacity_packets = 8,
            .receive_capacity_packets = 64,
        }};
        receiver.configure_live({.receive_tsbpd = true,
                                    .too_late_packet_drop = true,
                                    .receive_delay_milliseconds = 100},
            1'000, PacketTimestamp {0});
        observe_peer_horizon(receiver, initial.advanced(200U), 1'001);
        std::array<std::byte, 64> storage {};
        std::array<SequenceRange, 64> ranges {};
        for (std::uint32_t index = 0; index < ranges.size(); ++index) {
            // Interleave raw wire order and include two distinct ranges
            // with the same first endpoint, spanning the 31-bit wrap.
            const auto position = (index * 17U) % ranges.size();
            const auto first = initial.advanced((position / 2U) * 2U);
            ranges[index] = {first, first.advanced(position % 2U)};
            auto packet =
                encode_and_decode({.kind = ReliabilityActionKind::drop_request,
                                      .drop = {0, ranges[index]}},
                    storage);
            packet.control.timestamp = PacketTimestamp {20U + index};
            REQUIRE(receiver.receive(packet, 1'100));
        }
        const auto deadline = receiver.next_receive_delivery_time();
        REQUIRE(deadline.has_value());
        for (unsigned repetition = 0; repetition < 4; ++repetition) {
            for (const auto range : ranges) {
                auto packet = encode_and_decode(
                    {.kind = ReliabilityActionKind::drop_request,
                        .drop = {0, range}},
                    storage);
                packet.control.timestamp = PacketTimestamp {500U};
                REQUIRE(receiver.receive(packet, 1'500));
                REQUIRE_EQ(receiver.next_receive_delivery_time(), deadline);
            }
        }
        const auto next = initial.advanced(64U);
        auto packet =
            encode_and_decode({.kind = ReliabilityActionKind::drop_request,
                                  .drop = {0, {next, next}}},
                storage);
        REQUIRE_EQ(receiver.receive(packet, 1'500).error, Error::would_block);
        REQUIRE_EQ(receiver.drop_too_late_receiver(101'500).error, Error::none);
        REQUIRE_EQ(receiver.receive_buffer().first_stored_sequence(), next);
        REQUIRE(!receiver.next_receive_delivery_time().has_value());
        // Expiration removes the identity as well as the grace entry, and
        // leaves the preallocated queue reusable for a later request.
        packet.control.timestamp = PacketTimestamp {2'000U};
        REQUIRE(receiver.receive(packet, 102'000));
        REQUIRE(receiver.next_receive_delivery_time().has_value());
        REQUIRE_EQ(receiver.drop_too_late_receiver(103'000).error, Error::none);
        REQUIRE_EQ(
            receiver.receive_buffer().first_stored_sequence(), next.next());
        REQUIRE(!receiver.next_receive_delivery_time().has_value());
    }
}

TEST(session_drop_request_batch_expiry_preserves_future_identity_and_capacity)
{
    for (const auto initial :
        {SequenceNumber {100}, SequenceNumber {SequenceNumber::mask - 31U}}) {
        for (const bool message_api : {false, true}) {
            ReliabilitySession receiver {{
                .peer_initial_sequence = initial,
                .send_capacity_packets = 8,
                .receive_capacity_packets = 64,
            }};
            receiver.set_message_api(message_api);
            receiver.configure_live({.receive_tsbpd = true,
                                        .too_late_packet_drop = true,
                                        .receive_delay_milliseconds = 100},
                1'000, PacketTimestamp {0});
            observe_peer_horizon(receiver, initial.advanced(200U), 1'001);
            std::array<std::byte, 64> storage {};
            const auto request = [&](std::uint32_t position,
                                     std::uint32_t timestamp,
                                     std::uint64_t now) {
                const auto sequence = initial.advanced(position);
                auto packet = encode_and_decode(
                    {.kind = ReliabilityActionKind::drop_request,
                        .drop = {0, {sequence, sequence}}},
                    storage);
                packet.control.timestamp = PacketTimestamp {timestamp};
                return receiver.receive(packet, now);
            };
            // Admission order differs from both wire identity order and
            // deadline order, including the 31-bit sequence wrap.
            for (std::uint32_t index = 0; index < 64U; ++index) {
                const auto position = (index * 17U) % 64U;
                REQUIRE(
                    request(position, position % 2U == 0U ? 20U : 80U, 1'100));
            }
            REQUIRE_EQ(request(64U, 90U, 1'100).error, Error::would_block);
            const auto first = receiver.drop_too_late_receiver(101'030);
            REQUIRE(first);
            REQUIRE_EQ(first.receiver_drop_packets, 32U);
            REQUIRE_EQ(receiver.receive_buffer().first_stored_sequence(),
                initial.next());
            REQUIRE_EQ(receiver.next_receive_delivery_time(),
                std::optional<std::uint64_t> {101'080});
            // Retained identities still detect duplicates; later copies
            // cannot extend the original grace deadline.
            for (std::uint32_t position = 1U; position < 64U; position += 2U) {
                REQUIRE(request(position, 1'000U, 101'030));
                REQUIRE_EQ(receiver.next_receive_delivery_time(),
                    std::optional<std::uint64_t> {101'080});
            }
            for (std::uint32_t position = 64U; position < 96U; ++position) {
                REQUIRE(request(position, 90U, 101'030));
            }
            REQUIRE_EQ(request(96U, 90U, 101'030).error, Error::would_block);
            REQUIRE(request(95U, 1'000U, 101'030));
            const auto second = receiver.drop_too_late_receiver(101'080);
            REQUIRE(second);
            REQUIRE_EQ(second.receiver_drop_packets, 32U);
            REQUIRE_EQ(receiver.receive_buffer().first_stored_sequence(),
                initial.advanced(64U));
            REQUIRE_EQ(receiver.next_receive_delivery_time(),
                std::optional<std::uint64_t> {101'090});
            const auto third = receiver.drop_too_late_receiver(101'090);
            REQUIRE(third);
            REQUIRE_EQ(third.receiver_drop_packets, 32U);
            REQUIRE_EQ(receiver.receive_buffer().first_stored_sequence(),
                initial.advanced(96U));
            REQUIRE(!receiver.next_receive_delivery_time());
            REQUIRE(request(96U, 200U, 101'100));
            REQUIRE_EQ(receiver.next_receive_delivery_time(),
                std::optional<std::uint64_t> {101'200});
            REQUIRE(receiver.drop_too_late_receiver(101'200));
            REQUIRE(!receiver.next_receive_delivery_time());
        }
    }
}

TEST(session_drop_request_accepts_the_supported_sequence_boundaries)
{
    constexpr auto limit = SequenceNumber::half_range / 2U;
    for (const auto initial :
        {SequenceNumber {100}, SequenceNumber {SequenceNumber::mask - 5U}}) {
        ReliabilitySession receiver {{
            .peer_initial_sequence = initial,
            .send_capacity_packets = 8,
            .receive_capacity_packets = 8,
        }};
        std::array<std::byte, 64> storage {};
        const auto packet = encode_and_decode(
            {
                .kind = ReliabilityActionKind::drop_request,
                .drop = {0, {initial, initial.advanced(limit - 1U)}},
            },
            storage);
        const auto result = receiver.receive(packet, 1);
        REQUIRE(result);
        // Accepted, but held until the peer demonstrably sent past it.
        REQUIRE_EQ(result.receiver_drop_packets, 0U);
        REQUIRE_EQ(receiver.receive_buffer().next_ack_sequence(), initial);
        const std::array payload {std::byte {'n'}};
        PacketView data;
        data.kind = PacketKind::data;
        data.data.sequence = initial.advanced(limit);
        data.data.message_number = 1;
        data.data.boundary = MessageBoundary::solo;
        data.payload = payload;
        const auto applied = receiver.receive(data, 2);
        REQUIRE(applied);
        REQUIRE_EQ(applied.receiver_drop_packets, limit);
        REQUIRE_EQ(applied.receiver_loss_packets, 0U);
        REQUIRE(applied.receiver_packet_accepted_unique);
        REQUIRE_EQ(receiver.receive_buffer().next_ack_sequence(),
            initial.advanced(limit + 1U));
        REQUIRE_EQ(receiver.receive_buffer().first_stored_sequence(),
            initial.advanced(limit));
    }
}

TEST(session_group_sequence_skip_rejects_an_unsupported_drop_before_mutation)
{
    constexpr auto limit = SequenceNumber::half_range / 2U;
    const SequenceNumber initial {SequenceNumber::mask - 5U};
    ReliabilitySession sender {{
        .local_initial_sequence = initial,
        .send_capacity_packets = 8,
        .receive_capacity_packets = 8,
    }};
    REQUIRE_EQ(sender.skip_group_sequences(initial.advanced(limit + 1U)),
        Error::invalid_state);
    REQUIRE_EQ(sender.send_buffer().next_sequence(), initial);
    REQUIRE(!sender.has_pending_drop_requests());
    REQUIRE_EQ(
        sender.skip_group_sequences(initial.advanced(limit)), Error::none);
    const auto drops = sender.take_pending_drop_requests();
    REQUIRE_EQ(drops.size, 1U);
    REQUIRE_EQ(drops.values[0].drop.sequences.first, initial);
    REQUIRE_EQ(
        drops.values[0].drop.sequences.last, initial.advanced(limit - 1U));
}

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
    // The skip is applied once the mirror's first packet proves its position.
    REQUIRE_EQ(dropped.receiver_drop_packets, 0U);
    REQUIRE_EQ(
        receiver.receive_buffer().next_ack_sequence(), SequenceNumber {10});

    const std::array<std::byte, 1> payload {std::byte {'g'}};
    REQUIRE_EQ(session.queue_group_message(
                   payload, SequenceNumber {13}, 1, PacketTimestamp {5}),
        Error::none);
    REQUIRE_EQ(session.skip_group_sequences(SequenceNumber {14}),
        Error::invalid_state);
    const auto data = session.next_data_packet();
    REQUIRE(data.has_value());
    REQUIRE_EQ(data->header.sequence, SequenceNumber {13});
    const auto skipped = receiver.receive(view_of(*data), 2);
    REQUIRE(skipped);
    REQUIRE_EQ(skipped.receiver_drop_packets, 3U);
    REQUIRE_EQ(skipped.receiver_loss_packets, 0U);
    REQUIRE_EQ(
        receiver.receive_buffer().next_ack_sequence(), SequenceNumber {14});
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

TEST(session_live_drop_request_retains_received_and_in_flight_solo_packets)
{
    ReliabilitySession receiver {{
        .local_initial_sequence = SequenceNumber {100},
        .peer_initial_sequence = SequenceNumber {10},
        .peer_socket_id = 900,
        .send_capacity_packets = 8,
        .receive_capacity_packets = 8,
    }};
    receiver.configure_live(
        {
            .receive_tsbpd = true,
            .too_late_packet_drop = true,
            .sender_too_late_packet_drop = true,
            .receive_delay_milliseconds = 100,
        },
        1'000, PacketTimestamp {0});
    const std::array<std::byte, 1> payload {std::byte {'p'}};
    PacketView data;
    data.kind = PacketKind::data;
    data.data.sequence = SequenceNumber {10};
    data.data.message_number = 1;
    data.data.boundary = MessageBoundary::solo;
    data.data.timestamp = PacketTimestamp {50};
    data.payload = payload;
    REQUIRE(receiver.receive(data, 1'010));

    std::array<std::byte, 64> storage {};
    const auto drop = encode_and_decode(
        {
            .kind = ReliabilityActionKind::drop_request,
            .drop = {1, {SequenceNumber {10}, SequenceNumber {11}}},
        },
        storage);
    const auto processed = receiver.receive(drop, 1'020);
    REQUIRE(processed);
    REQUIRE_EQ(processed.receiver_drop_packets, 0U);
    REQUIRE_EQ(receiver.receive_buffer().occupied(), 1U);
    // Sequence 11 has not been observed yet, so the request cannot
    // acknowledge it; the acknowledgement stays at the received data.
    REQUIRE_EQ(processed.actions.size, 0U);
    REQUIRE_EQ(
        receiver.receive_buffer().next_ack_sequence(), SequenceNumber {11});

    data.data.sequence = SequenceNumber {11};
    data.data.message_number = 2;
    data.data.timestamp = PacketTimestamp {60};
    const auto delayed = receiver.receive(data, 1'030);
    REQUIRE(delayed);
    REQUIRE(delayed.receiver_packet_accepted_unique);
    REQUIRE_EQ(
        receiver.receive_buffer().next_ack_sequence(), SequenceNumber {12});
    std::array<std::byte, 1> output {};
    REQUIRE_EQ(
        receiver.pop_message_at(output, 101'049).error, Error::would_block);
    REQUIRE(receiver.pop_message_at(output, 101'050));
    REQUIRE_EQ(output, payload);
    REQUIRE(receiver.pop_message_at(output, 101'060));
    REQUIRE_EQ(output, payload);
}

TEST(session_live_drop_request_leaves_true_gap_to_local_deadline_drop)
{
    ReliabilitySession receiver {{
        .local_initial_sequence = SequenceNumber {100},
        .peer_initial_sequence = SequenceNumber {10},
        .peer_socket_id = 900,
        .send_capacity_packets = 8,
        .receive_capacity_packets = 8,
    }};
    receiver.configure_live(
        {
            .receive_tsbpd = true,
            .too_late_packet_drop = true,
            .sender_too_late_packet_drop = true,
            .receive_delay_milliseconds = 100,
        },
        1'000, PacketTimestamp {0});
    const std::array<std::byte, 1> payload {std::byte {'p'}};
    PacketView data;
    data.kind = PacketKind::data;
    data.data.sequence = SequenceNumber {11};
    data.data.message_number = 2;
    data.data.boundary = MessageBoundary::solo;
    data.data.timestamp = PacketTimestamp {50};
    data.payload = payload;
    REQUIRE(receiver.receive(data, 1'010));
    std::array<std::byte, 64> storage {};
    auto drop = encode_and_decode(
        {
            .kind = ReliabilityActionKind::drop_request,
            .drop = {1, {SequenceNumber {10}, SequenceNumber {10}}},
        },
        storage);
    drop.control.timestamp = PacketTimestamp {60};
    const auto processed = receiver.receive(drop, 1'070);
    REQUIRE(processed);
    REQUIRE_EQ(processed.receiver_drop_packets, 0U);
    REQUIRE_EQ(
        receiver.drop_too_late_receiver(101'049).receiver_drop_packets, 0U);
    REQUIRE_EQ(
        receiver.drop_too_late_receiver(101'050).receiver_drop_packets, 1U);
    std::array<std::byte, 1> output {};
    REQUIRE(receiver.pop_message_at(output, 101'050));
    REQUIRE_EQ(output, payload);
}

TEST(session_live_peer_drop_beyond_observed_data_waits_for_later_data)
{
    ReliabilitySession receiver {{
        .local_initial_sequence = SequenceNumber {100},
        .peer_initial_sequence = SequenceNumber {10},
        .peer_socket_id = 900,
        .send_capacity_packets = 8,
        .receive_capacity_packets = 8,
    }};
    receiver.configure_live(
        {
            .receive_tsbpd = true,
            .too_late_packet_drop = true,
            .sender_too_late_packet_drop = true,
            .receive_delay_milliseconds = 100,
        },
        1'000, PacketTimestamp {0});

    // The peer abandons its first eight packets before any of them, or
    // anything later, was observed here. An unauthenticated request alone
    // must not acknowledge or skip sequences the peer may never have sent,
    // so nothing is released at the request's deadline.
    std::array<std::byte, 64> storage {};
    auto drop = encode_and_decode(
        {
            .kind = ReliabilityActionKind::drop_request,
            .drop = {1, {SequenceNumber {10}, SequenceNumber {17}}},
        },
        storage);
    drop.control.timestamp = PacketTimestamp {70};
    const auto held = receiver.receive(drop, 1'080);
    REQUIRE(held);
    REQUIRE_EQ(held.actions.size, 0U);
    REQUIRE_EQ(
        receiver.receive_buffer().first_stored_sequence(), SequenceNumber {10});
    REQUIRE_EQ(
        receiver.receive_buffer().next_ack_sequence(), SequenceNumber {10});
    REQUIRE(!receiver.next_receive_delivery_time());
    REQUIRE_EQ(
        receiver.drop_too_late_receiver(101'070).receiver_drop_packets, 0U);
    REQUIRE_EQ(
        receiver.receive_buffer().first_stored_sequence(), SequenceNumber {10});

    // The first packet past the range proves the peer's position; the held
    // request is applied at once (its grace deadline already passed), the
    // gap is not reported as loss, and the packet is delivered normally.
    const std::array<std::byte, 1> payload {std::byte {'n'}};
    PacketView fresh;
    fresh.kind = PacketKind::data;
    fresh.data.sequence = SequenceNumber {18};
    fresh.data.message_number = 2;
    fresh.data.boundary = MessageBoundary::solo;
    fresh.data.timestamp = PacketTimestamp {90};
    fresh.payload = payload;
    const auto received = receiver.receive(fresh, 101'071);
    REQUIRE(received);
    REQUIRE(received.receiver_packet_accepted_unique);
    REQUIRE_EQ(received.receiver_drop_packets, 8U);
    REQUIRE_EQ(received.receiver_loss_packets, 0U);
    REQUIRE_EQ(
        receiver.receive_buffer().first_stored_sequence(), SequenceNumber {18});
    REQUIRE_EQ(
        receiver.receive_buffer().next_ack_sequence(), SequenceNumber {19});
    std::array<std::byte, 1> output {};
    REQUIRE(receiver.pop_message_at(output, 101'090));
    REQUIRE_EQ(output, payload);
}

TEST(session_live_peer_drop_deadline_preserves_late_originals)
{
    ReliabilitySession receiver {{
        .local_initial_sequence = SequenceNumber {100},
        .peer_initial_sequence = SequenceNumber {10},
        .peer_socket_id = 900,
        .send_capacity_packets = 8,
        .receive_capacity_packets = 8,
    }};
    receiver.configure_live(
        {
            .receive_tsbpd = true,
            .too_late_packet_drop = true,
            .sender_too_late_packet_drop = true,
            .receive_delay_milliseconds = 100,
        },
        1'000, PacketTimestamp {0});
    std::array<std::byte, 64> storage {};
    auto drop = encode_and_decode(
        {
            .kind = ReliabilityActionKind::drop_request,
            .drop = {1, {SequenceNumber {10}, SequenceNumber {11}}},
        },
        storage);
    drop.control.timestamp = PacketTimestamp {70};
    REQUIRE(receiver.receive(drop, 1'080));

    const std::array<std::byte, 1> payload {std::byte {'p'}};
    PacketView original;
    original.kind = PacketKind::data;
    original.data.message_number = 1;
    original.data.boundary = MessageBoundary::solo;
    original.data.timestamp = PacketTimestamp {50};
    original.payload = payload;
    original.data.sequence = SequenceNumber {10};
    REQUIRE(receiver.receive(original, 1'090));
    original.data.sequence = SequenceNumber {11};
    original.data.message_number = 2;
    original.data.timestamp = PacketTimestamp {60};
    REQUIRE(receiver.receive(original, 1'100));

    const auto released = receiver.drop_too_late_receiver(101'070);
    REQUIRE(released);
    REQUIRE_EQ(released.receiver_drop_packets, 0U);
    REQUIRE_EQ(receiver.receive_buffer().occupied(), 2U);
    std::array<std::byte, 1> output {};
    REQUIRE(receiver.pop_message_at(output, 101'070));
    REQUIRE_EQ(output, payload);
    REQUIRE(receiver.pop_message_at(output, 101'070));
    REQUIRE_EQ(output, payload);
}

TEST(session_stream_peer_drop_preserves_partial_chunk_immediately)
{
    ReliabilitySession receiver {{
        .local_initial_sequence = SequenceNumber {100},
        .peer_initial_sequence = SequenceNumber {10},
        .peer_socket_id = 900,
        .send_capacity_packets = 8,
        .receive_capacity_packets = 8,
    }};
    receiver.set_message_api(false);
    const std::array first_payload {
        std::byte {'a'}, std::byte {'b'}, std::byte {'c'}};
    const std::array last_payload {std::byte {'d'}, std::byte {'e'}};
    PacketView packet;
    packet.kind = PacketKind::data;
    packet.data.message_number = 7;
    packet.data.sequence = SequenceNumber {10};
    packet.data.boundary = MessageBoundary::first;
    packet.payload = first_payload;
    REQUIRE(receiver.receive(packet, 1'000));
    packet.data.sequence = SequenceNumber {12};
    packet.data.boundary = MessageBoundary::last;
    packet.payload = last_payload;
    REQUIRE(receiver.receive(packet, 1'001));

    std::array<std::byte, 2> prefix {};
    REQUIRE_EQ(receiver.pop_stream(prefix).bytes_written, 2U);
    REQUIRE_EQ(prefix[0], std::byte {'a'});
    REQUIRE_EQ(prefix[1], std::byte {'b'});

    std::array<std::byte, 64> storage {};
    const auto drop = encode_and_decode(
        {
            .kind = ReliabilityActionKind::drop_request,
            .drop = {7, {SequenceNumber {10}, SequenceNumber {12}}},
        },
        storage);
    const auto processed = receiver.receive(drop, 1'002);
    REQUIRE(processed);
    REQUIRE_EQ(processed.receiver_drop_packets, 1U);
    REQUIRE_EQ(receiver.receive_buffer().occupied(), 2U);
    REQUIRE_EQ(
        receiver.receive_buffer().next_ack_sequence(), SequenceNumber {13});

    std::array<std::byte, 3> suffix {};
    REQUIRE_EQ(receiver.pop_stream(suffix).bytes_written, 3U);
    REQUIRE_EQ(suffix[0], std::byte {'c'});
    REQUIRE_EQ(suffix[1], std::byte {'d'});
    REQUIRE_EQ(suffix[2], std::byte {'e'});
}

TEST(session_stream_peer_drop_preserves_partial_chunk_at_deadline)
{
    ReliabilitySession receiver {{
        .local_initial_sequence = SequenceNumber {100},
        .peer_initial_sequence = SequenceNumber {10},
        .peer_socket_id = 900,
        .send_capacity_packets = 8,
        .receive_capacity_packets = 8,
    }};
    receiver.configure_live(
        {
            .receive_tsbpd = true,
            .too_late_packet_drop = true,
            .sender_too_late_packet_drop = true,
            .receive_delay_milliseconds = 100,
        },
        1'000, PacketTimestamp {0});
    receiver.set_message_api(false);
    const std::array first_payload {
        std::byte {'a'}, std::byte {'b'}, std::byte {'c'}};
    const std::array last_payload {std::byte {'d'}, std::byte {'e'}};
    PacketView packet;
    packet.kind = PacketKind::data;
    packet.data.message_number = 7;
    packet.data.timestamp = PacketTimestamp {50};
    packet.data.sequence = SequenceNumber {10};
    packet.data.boundary = MessageBoundary::first;
    packet.payload = first_payload;
    REQUIRE(receiver.receive(packet, 1'010));
    packet.data.sequence = SequenceNumber {12};
    packet.data.boundary = MessageBoundary::last;
    packet.payload = last_payload;
    REQUIRE(receiver.receive(packet, 1'011));

    std::array<std::byte, 2> prefix {};
    REQUIRE_EQ(receiver.pop_stream_at(prefix, 101'050).bytes_written, 2U);
    REQUIRE_EQ(prefix[0], std::byte {'a'});
    REQUIRE_EQ(prefix[1], std::byte {'b'});

    std::array<std::byte, 64> storage {};
    auto drop = encode_and_decode(
        {
            .kind = ReliabilityActionKind::drop_request,
            .drop = {7, {SequenceNumber {10}, SequenceNumber {12}}},
        },
        storage);
    drop.control.timestamp = PacketTimestamp {70};
    const auto processed = receiver.receive(drop, 101'060);
    REQUIRE(processed);
    REQUIRE_EQ(processed.receiver_drop_packets, 0U);
    REQUIRE_EQ(
        receiver.drop_too_late_receiver(101'069).receiver_drop_packets, 0U);
    const auto released = receiver.drop_too_late_receiver(101'070);
    REQUIRE(released);
    REQUIRE_EQ(released.receiver_drop_packets, 1U);
    REQUIRE_EQ(receiver.receive_buffer().occupied(), 2U);

    std::array<std::byte, 3> suffix {};
    REQUIRE_EQ(receiver.pop_stream_at(suffix, 101'070).bytes_written, 3U);
    REQUIRE_EQ(suffix[0], std::byte {'c'});
    REQUIRE_EQ(suffix[1], std::byte {'d'});
    REQUIRE_EQ(suffix[2], std::byte {'e'});
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

TEST(session_file_mode_coalesces_acknowledgements_and_flushes_on_close)
{
    // File mode previously emitted a full acknowledgement for every received
    // packet. It now follows the same 10 ms control cadence as live mode, so a
    // burst produces no per-packet ACK. The pending cumulative acknowledgement
    // is instead flushed explicitly on close, so a peer that just sent its
    // final packets still receives it and can drain its send buffer.
    ReliabilitySession receiver {{
        .local_initial_sequence = SequenceNumber {100},
        .peer_initial_sequence = SequenceNumber {10},
        .peer_socket_id = 800,
        .send_capacity_packets = 16,
        .receive_capacity_packets = 16,
    }};
    receiver.configure_file(false);

    const auto acknowledgement_count = [](const ReliabilityActions& actions) {
        std::size_t total = 0;
        for (std::size_t index = 0; index < actions.size; ++index) {
            if (actions.values[index].kind
                == ReliabilityActionKind::acknowledgement) {
                ++total;
            }
        }
        return total;
    };

    const std::array payload {std::byte {'a'}};
    // Three in-order packets arrive inside a single 10 ms cadence interval.
    for (std::uint32_t index = 0; index < 3U; ++index) {
        const PacketView data {
            .kind = PacketKind::data,
            .data =
                {
                    .sequence = SequenceNumber {10}.advanced(index),
                    .message_number = index + 1U,
                    .boundary = MessageBoundary::solo,
                },
            .payload = payload,
        };
        const auto result = receiver.receive(data, 100U + index);
        REQUIRE(result);
        REQUIRE_EQ(acknowledgement_count(result.actions), 0U);
    }

    // Nothing is due before the cadence deadline.
    REQUIRE_EQ(receiver.poll_timers(5'000U).size, 0U);

    // Closing flushes the pending cumulative ACK covering all three packets.
    const auto flushed = receiver.flush_acknowledgement(6'000U);
    REQUIRE_EQ(flushed.size, 1U);
    REQUIRE_EQ(flushed.values[0].kind, ReliabilityActionKind::acknowledgement);
    REQUIRE_EQ(
        flushed.values[0].acknowledgement.kind, AcknowledgementKind::full);
    REQUIRE_EQ(
        flushed.values[0].acknowledgement.next_sequence, SequenceNumber {13});

    // The flush clears the pending state: neither a second flush nor a later
    // cadence poll produces a duplicate acknowledgement.
    REQUIRE_EQ(receiver.flush_acknowledgement(6'500U).size, 0U);
    REQUIRE_EQ(receiver.poll_timers(20'000U).size, 0U);
}

TEST(session_ignores_a_zero_ackack_number_without_an_error)
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
    const std::array<std::byte, 4> padding {};
    ackack.payload = padding;
    // This side never numbers an ACK 0, so no RTT sample matches; the packet
    // itself is well formed (libsrt wraps its counter to 0).
    const auto processed = receiver.receive(ackack, 1'000U);
    REQUIRE(processed);
    REQUIRE_EQ(processed.actions.size, 0U);
}

TEST(session_answers_a_wrapped_zero_numbered_full_ack)
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
        .kind = AcknowledgementKind::full,
        .acknowledgement_number = 0,
        .next_sequence = SequenceNumber {101},
        .round_trip_time_microseconds = 20'000,
        .round_trip_time_variance_microseconds = 4'000,
        .available_receive_buffer_packets = 3,
        .receive_rate_packets_per_second = 10,
        .estimated_link_capacity_packets_per_second = 20,
        .has_rate_metrics = true,
    };
    std::array<std::byte, 28> payload {};
    REQUIRE(encode_acknowledgement_payload(acknowledgement, payload));
    MutablePacketView packet;
    packet.kind = PacketKind::control;
    packet.control.type = ControlType::acknowledgement;
    packet.control.type_specific = 0U;
    packet.control.destination_socket_id = 900;
    packet.payload = payload;
    std::array<std::byte, packet_header_size + 28U> datagram {};
    REQUIRE(encode_packet(packet, datagram));
    const auto decoded_packet = decode_packet(datagram);
    REQUIRE(decoded_packet);

    const auto result = sender.receive(decoded_packet.packet, 1'000);
    REQUIRE(result);
    REQUIRE_EQ(sender.send_buffer().size(), 0U);
    REQUIRE_EQ(result.actions.size, 1U);
    REQUIRE_EQ(result.actions.values[0].kind,
        ReliabilityActionKind::acknowledgement_of_ack);
    REQUIRE_EQ(result.actions.values[0].acknowledgement_number, 0U);
    std::array<std::byte, 64> storage {};
    const auto echoed = encode_and_decode(result.actions.values[0], storage);
    REQUIRE_EQ(echoed.control.type, ControlType::acknowledgement_of_ack);
    REQUIRE_EQ(echoed.control.type_specific, 0U);
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

TEST(session_active_sender_empty_receive_side_can_become_tsbpd_readable)
{
    ReliabilitySession session {{
        .local_initial_sequence = SequenceNumber {100},
        .peer_initial_sequence = SequenceNumber {SequenceNumber::mask},
        .send_capacity_packets = 4,
        .receive_capacity_packets = 4,
    }};
    session.enable_tsbpd(1'000, PacketTimestamp {0}, 100);
    const std::array<std::byte, 1> payload {std::byte {'x'}};
    REQUIRE_EQ(
        session.queue_message(payload, PacketTimestamp {0}), Error::none);
    REQUIRE_EQ(session.send_buffer().size(), 1U);
    REQUIRE(!session.message_ready_at(1'000));
    REQUIRE(!session.next_receive_delivery_time());
    REQUIRE_EQ(session.drop_too_late_receiver(1'000).receiver_drop_packets, 0U);

    PacketView packet;
    packet.kind = PacketKind::data;
    packet.data.sequence = SequenceNumber {SequenceNumber::mask};
    packet.data.boundary = MessageBoundary::solo;
    packet.data.timestamp = PacketTimestamp {50};
    packet.payload = payload;
    std::array<std::byte, 1> output {};
    for (int cycle = 0; cycle < 2; ++cycle) {
        REQUIRE(session.receive(packet, 1'010));
        REQUIRE(!session.message_ready_at(1'149));
        REQUIRE_EQ(session.next_receive_delivery_time(),
            std::optional<std::uint64_t> {1'150});
        REQUIRE(session.message_ready_at(1'150));
        REQUIRE(session.pop_message_at(output, 1'150));
        REQUIRE_EQ(output, payload);
        REQUIRE(!session.message_ready_at(1'151));
        REQUIRE(!session.next_receive_delivery_time());
        REQUIRE_EQ(session.send_buffer().size(), 1U);
        packet.data.sequence = packet.data.sequence.next();
    }
    REQUIRE(session.next_data_packet());
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
    receiver.configure_live(
        {
            .receive_tsbpd = true,
            .too_late_packet_drop = true,
            .sender_too_late_packet_drop = true,
            .periodic_nak = true,
            .retransmit_flag = true,
            .receive_delay_milliseconds = 100,
        },
        1'000, PacketTimestamp {0});

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
    receiver.configure_live(
        {
            .receive_tsbpd = true,
            .too_late_packet_drop = false,
            .sender_too_late_packet_drop = false,
            .receive_delay_milliseconds = 100,
        },
        1'000, PacketTimestamp {0});

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
    receiver.configure_live(
        {
            .receive_tsbpd = true,
            .too_late_packet_drop = true,
            .sender_too_late_packet_drop = true,
            .receive_delay_milliseconds = 0,
        },
        1'000'000, PacketTimestamp {0xffff'f000U});

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

TEST(open_window_receive_release_does_not_expedite_an_acknowledgement)
{
    // An application read while the receive window is open must not pull a full
    // ACK forward. Doing so on every read produced one full ACK per read and
    // evicted in-flight ACKACKs; the regular cadence covers an open window.
    ReliabilitySession receiver {{
        .local_initial_sequence = SequenceNumber {100},
        .peer_initial_sequence = SequenceNumber {10},
        .peer_socket_id = 800,
        .send_capacity_packets = 8,
        .receive_capacity_packets = 8,
    }};
    const std::array<std::byte, 1> payload {std::byte {'x'}};
    PacketView packet;
    packet.kind = PacketKind::data;
    packet.data.boundary = MessageBoundary::solo;
    packet.data.sequence = SequenceNumber {10};
    packet.data.message_number = 1;
    packet.payload = payload;
    REQUIRE(receiver.receive(packet, 100));

    // Drain the acknowledgement the data receipt scheduled so the window is
    // known open with nothing pending.
    (void)receiver.poll_timers(10'000);

    std::array<std::byte, 1> output {};
    REQUIRE(receiver.pop_message(output));
    receiver.note_receive_buffer_released(10'001);

    // The window was open (available > 0), so no ACK is expedited; polling
    // right after the read, before the next interval, yields nothing.
    const auto released = receiver.poll_timers(10'002);
    REQUIRE_EQ(released.size, 0U);
}

TEST(session_matches_ackack_beyond_sixty_four_outstanding)
{
    ReliabilitySession receiver {{
        .local_initial_sequence = SequenceNumber {100},
        .peer_initial_sequence = SequenceNumber {10},
        .send_capacity_packets = 8,
        .receive_capacity_packets = 8,
    }};
    const auto first = receiver.make_staged_receive_acknowledgement(
        SequenceNumber {10}, 8U, 1'000U);
    for (std::uint32_t index = 1; index < 400U; ++index) {
        (void)receiver.make_staged_receive_acknowledgement(
            SequenceNumber {10}, 8U, 1'000U + index);
    }
    std::array<std::byte, 64> storage {};
    ReliabilityAction ackack {
        .kind = ReliabilityActionKind::acknowledgement_of_ack,
        .acknowledgement_number = first.acknowledgement.acknowledgement_number,
    };
    REQUIRE(receiver.receive(encode_and_decode(ackack, storage), 1'500U));
    REQUIRE_EQ(receiver.rtt().smoothed_microseconds(), 500U);
    REQUIRE_EQ(receiver.rtt().variation_microseconds(), 250U);
}

TEST(session_coalesces_open_window_releases_without_more_data)
{
    ReliabilitySession receiver {{
        .local_initial_sequence = SequenceNumber {100},
        .peer_initial_sequence = SequenceNumber {10},
        .send_capacity_packets = 3,
        .receive_capacity_packets = 3,
    }};
    const std::array payload {std::byte {'x'}};
    PacketView packet;
    packet.kind = PacketKind::data;
    packet.data.boundary = MessageBoundary::solo;
    packet.payload = payload;
    for (std::uint32_t i = 0; i < 3; ++i) {
        packet.data.sequence = SequenceNumber {10U + i};
        packet.data.message_number = 1U + i;
        REQUIRE(receiver.receive(packet, 100U + i));
    }
    (void)receiver.poll_timers(10'000);
    std::array<std::byte, 1> output {};
    REQUIRE(receiver.pop_message(output));
    receiver.note_receive_buffer_released(10'001);
    const auto first = receiver.poll_timers(10'001);
    REQUIRE_EQ(first.size, 1U);
    REQUIRE_EQ(
        first.values[0].acknowledgement.available_receive_buffer_packets, 1U);
    for (std::uint64_t time : {10'002U, 10'003U}) {
        REQUIRE(receiver.pop_message(output));
        receiver.note_receive_buffer_released(time);
        REQUIRE_EQ(receiver.poll_timers(time).size, 0U);
    }
    REQUIRE_EQ(receiver.poll_timers(20'000).size, 0U);
    const auto later = receiver.poll_timers(20'001);
    REQUIRE_EQ(later.size, 1U);
    REQUIRE_EQ(later.values[0].acknowledgement.kind, AcknowledgementKind::full);
    REQUIRE_EQ(
        later.values[0].acknowledgement.available_receive_buffer_packets, 3U);
    REQUIRE_EQ(receiver.poll_timers(30'001).size, 0U);
}

TEST(session_lite_ack_does_not_replace_advertised_closed_window)
{
    ReliabilitySession receiver {{
        .local_initial_sequence = SequenceNumber {100},
        .peer_initial_sequence = SequenceNumber {10},
        .send_capacity_packets = 128,
        .receive_capacity_packets = 128,
    }};
    receiver.configure_live({.periodic_nak = true, .retransmit_flag = true}, 0,
        PacketTimestamp {0});
    // Staged reception can advertise a closed window independently of the
    // session buffer. A following Lite ACK must not announce its free space.
    (void)receiver.make_staged_receive_acknowledgement(
        SequenceNumber {10}, 0U, 0U);
    const std::array payload {std::byte {'x'}};
    PacketView packet;
    packet.kind = PacketKind::data;
    packet.data.boundary = MessageBoundary::solo;
    packet.payload = payload;
    for (std::uint32_t i = 0; i < 64; ++i) {
        packet.data.sequence = SequenceNumber {10U + i};
        packet.data.message_number = 1U + i;
        REQUIRE(receiver.receive(packet, 100U + i));
    }
    const auto lite = receiver.poll_timers(1'000);
    REQUIRE_EQ(lite.size, 1U);
    REQUIRE_EQ(lite.values[0].acknowledgement.kind, AcknowledgementKind::lite);
    std::array<std::byte, 1> output {};
    REQUIRE(receiver.pop_message(output));
    receiver.note_receive_buffer_released(1'001);
    const auto reopened = receiver.poll_timers(1'001);
    REQUIRE_EQ(reopened.size, 1U);
    REQUIRE_EQ(
        reopened.values[0].acknowledgement.kind, AcknowledgementKind::full);
    REQUIRE_EQ(
        reopened.values[0].acknowledgement.available_receive_buffer_packets,
        65U);
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

TEST(session_peer_drop_request_removes_only_the_dropped_loss_range)
{
    // A DROPREQ for one range must not silence the periodic NAK for an
    // earlier, unrelated loss the peer did not drop.
    ReliabilitySession receiver {{
        .local_initial_sequence = SequenceNumber {100},
        .peer_initial_sequence = SequenceNumber {10},
        .peer_socket_id = 900,
        .send_capacity_packets = 32,
        .receive_capacity_packets = 32,
    }};
    receiver.configure_live(
        {
            .periodic_nak = true,
            .retransmit_flag = true,
        },
        0, PacketTimestamp {0});

    const std::array<std::byte, 1> payload {std::byte {'l'}};
    PacketView packet;
    packet.kind = PacketKind::data;
    packet.data.boundary = MessageBoundary::solo;
    packet.payload = payload;
    packet.data.sequence = SequenceNumber {10};
    packet.data.message_number = 1;
    REQUIRE(receiver.receive(packet, 100));
    // Loss 11..12, then loss 14..19 behind a message at 13 and 20.
    packet.data.sequence = SequenceNumber {13};
    packet.data.message_number = 4;
    REQUIRE(receiver.receive(packet, 200));
    packet.data.sequence = SequenceNumber {20};
    packet.data.message_number = 11;
    REQUIRE(receiver.receive(packet, 300));

    std::array<std::byte, 64> storage {};
    auto drop = encode_and_decode(
        {
            .kind = ReliabilityActionKind::drop_request,
            .drop = {5, {SequenceNumber {14}, SequenceNumber {19}}},
        },
        storage);
    drop.control.timestamp = PacketTimestamp {60};
    REQUIRE(receiver.receive(drop, 400));

    const auto periodic = receiver.poll_timers(300'400);
    bool saw_first_gap = false;
    bool saw_dropped_gap = false;
    for (std::size_t index = 0; index < periodic.size; ++index) {
        if (periodic.values[index].kind != ReliabilityActionKind::loss_report) {
            continue;
        }
        for (const auto& range : periodic.loss_ranges(periodic.values[index])) {
            saw_first_gap = saw_first_gap
                || (range.first == SequenceNumber {11}
                    && range.last == SequenceNumber {12});
            saw_dropped_gap = saw_dropped_gap
                || range.first.distance_from(SequenceNumber {14}) >= 0;
        }
    }
    REQUIRE(saw_first_gap);
    REQUIRE(!saw_dropped_gap);
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

    const auto periodic = receiver.poll_timers(300'300);
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

TEST(session_stale_nak_coalesces_packet_coverage_and_preserves_current_budget)
{
    for (const auto initial :
        {SequenceNumber {100}, SequenceNumber {SequenceNumber::mask - 100U}}) {
        ReliabilitySession sender {{.local_initial_sequence = initial,
            .send_capacity_packets = 1024,
            .receive_capacity_packets = 4,
            .maximum_payload_size = 1}};
        REQUIRE_EQ(
            sender.skip_group_sequences(initial.advanced(8192)), Error::none);
        REQUIRE_EQ(sender.take_pending_drop_requests().size, 1U);
        const std::vector<std::byte> payload(1024);
        REQUIRE_EQ(
            sender.queue_message(payload, PacketTimestamp {0}), Error::none);
        for (std::size_t i = 0; i < payload.size(); ++i)
            REQUIRE(sender.next_data_packet().has_value());
        std::array<SequenceRange, 301> losses {};
        for (std::uint32_t i = 0; i < 300U; ++i) {
            const auto sequence = initial.advanced(299U - i);
            losses[i] = {sequence, sequence};
        }
        losses.back() = {initial.advanced(8192), initial.advanced(9215)};
        const ReliabilityAction action {
            .kind = ReliabilityActionKind::loss_report, .loss = losses.front()};
        std::array<std::byte, maximum_data_payload_size + packet_header_size>
            storage {};
        const auto encoded = encode_reliability_action(
            action, losses, PacketTimestamp {0}, 1, storage);
        REQUIRE(encoded);
        const auto decoded =
            decode_packet(std::span {storage}.first(encoded.bytes_written));
        REQUIRE(decoded);
        const auto result = sender.receive(decoded.packet, 1);
        REQUIRE(result);
        REQUIRE_EQ(result.sender_loss_packets, 256U);
        REQUIRE_EQ(result.actions.size, 1U);
        REQUIRE_EQ(result.actions.values[0].drop.message_number, 0U);
        REQUIRE_EQ(result.actions.values[0].drop.sequences.first, initial);
        REQUIRE_EQ(result.actions.values[0].drop.sequences.last,
            initial.advanced(299));
        for (unsigned turn = 0; turn < 3; ++turn)
            REQUIRE_EQ(
                sender.service_pending_naks(turn + 2U).sender_loss_packets,
                256U);
        REQUIRE_EQ(sender.service_pending_naks(5).sender_loss_packets, 0U);
    }
}

TEST(
    session_stale_nak_backpressure_and_invalid_tail_preserve_the_whole_transaction)
{
    ReliabilitySession sender {{.local_initial_sequence = SequenceNumber {100},
        .send_capacity_packets = 8,
        .receive_capacity_packets = 4,
        .maximum_payload_size = 1}};
    REQUIRE_EQ(sender.skip_group_sequences(SequenceNumber {2100}), Error::none);
    REQUIRE_EQ(sender.take_pending_drop_requests().size, 1U);
    const std::array payload {std::byte {'x'}};
    REQUIRE_EQ(sender.queue_message(payload, PacketTimestamp {0}), Error::none);
    REQUIRE(sender.next_data_packet().has_value());
    std::array<SequenceRange, maximum_loss_words_per_packet> losses {};
    for (std::uint32_t i = 0; i < losses.size(); ++i) {
        const auto sequence = SequenceNumber {100U + 2U * i};
        losses[i] = {sequence, sequence};
    }
    std::array<std::byte, maximum_data_payload_size + packet_header_size>
        storage {};
    auto receive = [&](std::span<const SequenceRange> ranges) {
        const ReliabilityAction action {
            .kind = ReliabilityActionKind::loss_report, .loss = ranges.front()};
        const auto encoded = encode_reliability_action(
            action, ranges, PacketTimestamp {0}, 1, storage);
        REQUIRE(encoded);
        const auto decoded =
            decode_packet(std::span {storage}.first(encoded.bytes_written));
        REQUIRE(decoded);
        return sender.receive(decoded.packet, 1);
    };
    const auto first = receive(losses);
    REQUIRE(first);
    REQUIRE_EQ(first.actions.size, 4U);
    const std::array rejected {
        SequenceRange {SequenceNumber {1100}, SequenceNumber {1100}},
        SequenceRange {SequenceNumber {1102}, SequenceNumber {1102}},
        SequenceRange {SequenceNumber {1104}, SequenceNumber {1104}},
        SequenceRange {SequenceNumber {1106}, SequenceNumber {1106}},
        SequenceRange {SequenceNumber {1108}, SequenceNumber {1108}},
        SequenceRange {SequenceNumber {1110}, SequenceNumber {1110}},
        SequenceRange {SequenceNumber {2100}, SequenceNumber {2100}}};
    REQUIRE_EQ(receive(rejected).error, Error::buffer_too_small);
    REQUIRE(!sender.next_data_packet().has_value());
    const auto still_pending = sender.service_pending_naks(2);
    REQUIRE_EQ(still_pending.sender_loss_packets, 0U);
    REQUIRE_EQ(still_pending.actions.size, 4U);
    for (std::size_t i = 0; i < still_pending.actions.size; ++i)
        REQUIRE_EQ(still_pending.actions.values[i].drop.sequences.first,
            SequenceNumber {108U + static_cast<std::uint32_t>(2U * i)});
    std::size_t replies = first.actions.size + still_pending.actions.size;
    while (sender.has_pending_drop_requests()) {
        const auto next = sender.take_pending_drop_requests();
        REQUIRE(next.size != 0U);
        for (std::size_t i = 0; i < next.size; ++i) {
            REQUIRE(next.values[i].drop.sequences.first.distance_from(
                        SequenceNumber {1100})
                < 0);
            ++replies;
        }
    }
    REQUIRE_EQ(replies, losses.size());
    const std::array invalid_tail {
        SequenceRange {SequenceNumber {1100}, SequenceNumber {1100}},
        SequenceRange {SequenceNumber {2100}, SequenceNumber {2100}},
        SequenceRange {SequenceNumber {2101}, SequenceNumber {2101}}};
    REQUIRE_EQ(receive(invalid_tail).error, Error::invalid_control_payload);
    REQUIRE(!sender.has_pending_drop_requests());
    REQUIRE_EQ(sender.service_pending_naks(3).sender_loss_packets, 0U);
    REQUIRE(!sender.next_data_packet().has_value());
}

TEST(
    session_stale_nak_queued_ack_suppression_is_bounded_and_clips_partial_ranges)
{
    for (const auto initial :
        {SequenceNumber {100}, SequenceNumber {SequenceNumber::mask - 40U}}) {
        ReliabilitySession sender {{.local_initial_sequence = initial,
            .send_capacity_packets = 4,
            .receive_capacity_packets = 4}};
        REQUIRE_EQ(
            sender.skip_group_sequences(initial.advanced(128)), Error::none);
        REQUIRE_EQ(sender.take_pending_drop_requests().size, 1U);
        std::array<SequenceRange, 33> losses {};
        for (std::uint32_t i = 0; i < 32U; ++i) {
            const auto sequence = initial.advanced(i * 2U);
            losses[i] = {sequence, sequence};
        }
        losses.back() = {initial.advanced(80), initial.advanced(100)};
        const ReliabilityAction loss {
            .kind = ReliabilityActionKind::loss_report, .loss = losses.front()};
        std::array<std::byte, 256> storage {};
        const auto encoded = encode_reliability_action(
            loss, losses, PacketTimestamp {0}, 1, storage);
        REQUIRE(encoded);
        const auto decoded =
            decode_packet(std::span {storage}.first(encoded.bytes_written));
        REQUIRE(decoded);
        REQUIRE(sender.receive(decoded.packet, 1));
        REQUIRE(sender.has_pending_drop_requests());
        const ReliabilityAction ack {
            .kind = ReliabilityActionKind::acknowledgement,
            .acknowledgement = {.kind = AcknowledgementKind::lite,
                .next_sequence = initial.advanced(90)}};
        std::array<std::byte, 64> ack_storage {};
        REQUIRE(sender.receive(encode_and_decode(ack, ack_storage), 2));
        unsigned turns = 0;
        unsigned emitted = 0;
        while (sender.has_pending_drop_requests()) {
            const auto next = sender.take_pending_drop_requests();
            ++turns;
            REQUIRE(turns <= 9U);
            // Each turn inspects at most four queued ranges, including ACKed
            // ranges. This backlog cannot be discarded in one unbounded loop.
            if (turns == 1U)
                REQUIRE(sender.has_pending_drop_requests());
            for (std::size_t i = 0; i < next.size; ++i) {
                const auto& range = next.values[i].drop.sequences;
                REQUIRE_EQ(range.first, initial.advanced(90));
                REQUIRE_EQ(range.last, initial.advanced(100));
                ++emitted;
            }
        }
        REQUIRE(turns > 1U);
        REQUIRE_EQ(emitted, 1U);
        const ReliabilityAction repeated {
            .kind = ReliabilityActionKind::loss_report,
            .loss = {initial, initial.advanced(89)}};
        const auto result =
            sender.receive(encode_and_decode(repeated, ack_storage), 3);
        REQUIRE(result);
        REQUIRE_EQ(result.actions.size, 0U);
        REQUIRE(!sender.has_pending_drop_requests());
    }
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
    // Each message is first sent when it is queued; the drop deadline is
    // measured from that first transmission.
    REQUIRE(sender.next_data_packet(1'000).has_value());
    REQUIRE(sender.next_data_packet(1'000).has_value());
    REQUIRE(sender.next_data_packet(3'000).has_value());
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
    // Each message is first sent when it is queued; the drop deadline is
    // measured from that first transmission.
    REQUIRE(sender.next_data_packet(1'000).has_value());
    REQUIRE(sender.next_data_packet(1'000).has_value());
    REQUIRE(sender.next_data_packet(3'000).has_value());
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
    const NegotiatedLiveOptions options {
        .send_tsbpd = true,
        .receive_tsbpd = true,
        .too_late_packet_drop = true,
        .sender_too_late_packet_drop = true,
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
    // Too-late drop abandons only packets that were already sent.
    REQUIRE_EQ(session.drop_too_late_sender(1'020'001).size, 0U);
    REQUIRE(session.next_data_packet());
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
    REQUIRE_EQ(options.set(SocketOption::maximum_bandwidth_bytes_per_second,
                   1'250'000'000),
        Error::none);
    REQUIRE_EQ(session.apply_dynamic_options(options), Error::none);
    REQUIRE_EQ(session.live_pacing_rate_bytes_per_second(), 1'250'000'000U);
    REQUIRE_EQ(options.set(SocketOption::maximum_bandwidth_bytes_per_second, 0),
        Error::none);
    REQUIRE_EQ(session.apply_dynamic_options(options), Error::none);
    REQUIRE_EQ(session.live_pacing_rate_bytes_per_second(), 440'000U);
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

TEST(session_filter_losses_exclude_peer_drops_and_discarded_payloads)
{
    const auto filter =
        parse_packet_filter_configuration("fec,cols:4,rows:1,arq:onreq");
    REQUIRE(filter);
    for (const auto initial :
        {SequenceNumber {100}, SequenceNumber {SequenceNumber::mask - 2U}}) {
        for (const bool discard : {false, true}) {
            ReliabilitySession receiver {{
                .peer_initial_sequence = initial,
                .send_capacity_packets = 16,
                .receive_capacity_packets = 16,
            }};
            receiver.configure_live(
                {.periodic_nak = true}, 0, PacketTimestamp {0});
            receiver.configure_packet_filter(filter.configuration, true);
            const std::array payload {std::byte {'p'}};
            PacketView packet;
            packet.kind = PacketKind::data;
            packet.data.boundary = MessageBoundary::solo;
            packet.payload = payload;
            for (const auto offset : {0U, 5U}) {
                packet.data.sequence = initial.advanced(offset);
                packet.data.message_number = offset + 1U;
                REQUIRE(receiver.receive(packet, offset + 1U));
            }
            if (discard) {
                for (const auto offset : {2U, 3U}) {
                    packet.data.sequence = initial.advanced(offset);
                    packet.data.message_number = offset + 1U;
                    REQUIRE(receiver.receive(
                        packet, offset + 10U, {.discard_payload = true}));
                }
            } else {
                std::array<std::byte, 64> storage {};
                const auto drop = encode_and_decode(
                    {
                        .kind = ReliabilityActionKind::drop_request,
                        .drop = {0,
                            {initial.advanced(2U), initial.advanced(3U)}},
                    },
                    storage);
                REQUIRE(receiver.receive(drop, 10U));
            }
            const std::array losses {
                SequenceRange {initial.next(), initial.advanced(4U)}};
            const auto result = receiver.report_filter_losses(losses, 20U);
            REQUIRE(result);
            REQUIRE_EQ(result.receiver_filter_loss_packets, 2U);
            REQUIRE_EQ(result.actions.size, 1U);
            const auto& action = result.actions.values[0];
            REQUIRE_EQ(action.kind, ReliabilityActionKind::loss_report);
            const auto reported_losses = result.actions.loss_ranges(action);
            REQUIRE_EQ(reported_losses.size(), 2U);
            REQUIRE_EQ(reported_losses[0].first, initial.next());
            REQUIRE_EQ(reported_losses[0].last, initial.next());
            REQUIRE_EQ(reported_losses[1].first, initial.advanced(4U));
            REQUIRE_EQ(reported_losses[1].last, initial.advanced(4U));
            REQUIRE_EQ(receiver.report_filter_losses(losses, 21U)
                           .receiver_filter_loss_packets,
                0U);
        }
    }
}

TEST(session_filter_losses_for_peer_grace_are_removed_when_drop_settles)
{
    const SequenceNumber initial {100};
    ReliabilitySession receiver {{
        .peer_initial_sequence = initial,
        .send_capacity_packets = 16,
        .receive_capacity_packets = 16,
    }};
    const auto filter =
        parse_packet_filter_configuration("fec,cols:4,rows:1,arq:onreq");
    REQUIRE(filter);
    receiver.configure_live(
        {
            .receive_tsbpd = true,
            .too_late_packet_drop = true,
            .periodic_nak = true,
            .receive_delay_milliseconds = 100,
        },
        0, PacketTimestamp {0});
    receiver.configure_packet_filter(filter.configuration, true);
    const std::array payload {std::byte {'p'}};
    PacketView packet;
    packet.kind = PacketKind::data;
    packet.data.boundary = MessageBoundary::solo;
    packet.payload = payload;
    for (const auto offset : {0U, 5U}) {
        packet.data.sequence = initial.advanced(offset);
        packet.data.message_number = offset + 1U;
        REQUIRE(receiver.receive(packet, offset + 1U));
    }
    std::array<std::byte, 64> storage {};
    const auto drop = encode_and_decode(
        {
            .kind = ReliabilityActionKind::drop_request,
            .drop = {0, {initial.advanced(2U), initial.advanced(3U)}},
        },
        storage);
    REQUIRE(receiver.receive(drop, 10U));
    const std::array losses {
        SequenceRange {initial.next(), initial.advanced(4U)}};
    REQUIRE_EQ(
        receiver.report_filter_losses(losses, 20U).receiver_filter_loss_packets,
        4U);
    const auto expired = receiver.drop_too_late_receiver(100'000U);
    REQUIRE(expired);
    REQUIRE_EQ(expired.receiver_drop_packets, 2U);
    const auto actions = receiver.poll_timers(1'000'000U);
    bool reported = false;
    for (std::size_t index = 0; index < actions.size; ++index) {
        const auto& action = actions.values[index];
        if (action.kind != ReliabilityActionKind::loss_report)
            continue;
        reported = true;
        const auto reported_losses = actions.loss_ranges(action);
        REQUIRE_EQ(reported_losses.size(), 2U);
        REQUIRE_EQ(reported_losses[0].first, initial.next());
        REQUIRE_EQ(reported_losses[0].last, initial.next());
        REQUIRE_EQ(reported_losses[1].first, initial.advanced(4U));
        REQUIRE_EQ(reported_losses[1].last, initial.advanced(4U));
    }
    REQUIRE(reported);
    packet.data.sequence = initial.advanced(2U);
    packet.data.message_number = 3U;
    const auto duplicate = receiver.receive(packet, 1'000'001U);
    REQUIRE(duplicate);
    REQUIRE(!duplicate.receiver_packet_accepted_unique);
    REQUIRE_EQ(receiver.report_filter_losses(losses, 1'000'002U)
                   .receiver_filter_loss_packets,
        0U);
}

TEST(session_accepts_filter_losses_reported_out_of_order)
{
    // FEC column groups close out of sequence order, so a later filter-loss
    // report can precede one already tracked. Both must be retained and NAKed;
    // previously the out-of-order report was rejected and never requested.
    const auto filter =
        parse_packet_filter_configuration("fec,cols:4,rows:4,arq:onreq");
    REQUIRE(filter);
    ReliabilitySession receiver {{
        .local_initial_sequence = SequenceNumber {1},
        .peer_initial_sequence = SequenceNumber {100},
        .send_capacity_packets = 64,
        .receive_capacity_packets = 64,
    }};
    const NegotiatedLiveOptions live_options {
        .periodic_nak = true,
        .retransmit_flag = true,
    };
    receiver.configure_live(live_options, 0, PacketTimestamp {0});
    receiver.configure_packet_filter(filter.configuration, true);

    // A later column declares its loss first (sequence 115), then an earlier
    // column declares a lower one (sequence 108).
    const std::array high {SequenceRange {
        .first = SequenceNumber {115},
        .last = SequenceNumber {115},
    }};
    const auto first = receiver.report_filter_losses(high, 100U);
    REQUIRE(first);
    REQUIRE_EQ(first.receiver_filter_loss_packets, 1U);

    const std::array low {SequenceRange {
        .first = SequenceNumber {108},
        .last = SequenceNumber {108},
    }};
    const auto second = receiver.report_filter_losses(low, 101U);
    REQUIRE(second);
    REQUIRE_EQ(second.receiver_filter_loss_packets, 1U);

    // Both losses are NAKed, in ascending order (108 before 115).
    const auto periodic = receiver.poll_timers(1'000'000U);
    std::vector<SequenceNumber> reported;
    for (std::size_t index = 0; index < periodic.size; ++index) {
        const auto& action = periodic.values[index];
        if (action.kind != ReliabilityActionKind::loss_report) {
            continue;
        }
        for (const auto& range : periodic.loss_ranges(action)) {
            reported.push_back(range.first);
        }
    }
    REQUIRE_EQ(reported.size(), 2U);
    REQUIRE_EQ(reported[0], SequenceNumber {108});
    REQUIRE_EQ(reported[1], SequenceNumber {115});
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
            .first = SequenceNumber {101},
            .last = SequenceNumber {100},
        },
        SequenceRange {
            .first = SequenceNumber {100},
            .last = SequenceNumber {101},
        },
    };
    const auto rejected = receiver.report_filter_losses(invalid_losses, 100U);
    REQUIRE(!rejected);
    REQUIRE_EQ(rejected.error, Error::invalid_control_payload);

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

TEST(session_clips_and_merges_filter_losses_within_receive_window)
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
    receiver.configure_live({.periodic_nak = true, .retransmit_flag = true}, 0,
        PacketTimestamp {0});
    receiver.configure_packet_filter(filter.configuration, true);
    const std::array payload {std::byte {'x'}};
    PacketView packet {
        .kind = PacketKind::data,
        .data =
            {
                .sequence = SequenceNumber {102},
                .message_number = 3,
                .boundary = MessageBoundary::solo,
            },
        .payload = payload,
    };
    REQUIRE(receiver.receive(packet, 50U));

    const std::array first {SequenceRange {
        .first = SequenceNumber {103},
        .last = SequenceNumber {104},
    }};
    REQUIRE_EQ(
        receiver.report_filter_losses(first, 100U).receiver_filter_loss_packets,
        2U);

    const std::array overlap {
        SequenceRange {SequenceNumber {101}, SequenceNumber {103}},
        SequenceRange {SequenceNumber {98}, SequenceNumber {101}},
        SequenceRange {SequenceNumber {106}, SequenceNumber {110}},
    };
    const auto merged = receiver.report_filter_losses(overlap, 200U);
    REQUIRE(merged);
    REQUIRE_EQ(merged.receiver_filter_loss_packets, 4U);
    const auto periodic = receiver.poll_timers(1'000'000U);
    std::vector<SequenceNumber> reported;
    for (std::size_t index = 0; index < periodic.size; ++index) {
        if (periodic.values[index].kind != ReliabilityActionKind::loss_report) {
            continue;
        }
        for (const auto& range : periodic.loss_ranges(periodic.values[index])) {
            for (auto sequence = range.first;; sequence = sequence.next()) {
                reported.push_back(sequence);
                if (sequence == range.last) {
                    break;
                }
            }
        }
    }
    const std::vector expected {
        SequenceNumber {100},
        SequenceNumber {101},
        SequenceNumber {103},
        SequenceNumber {104},
        SequenceNumber {106},
        SequenceNumber {107},
    };
    REQUIRE_EQ(reported, expected);
    REQUIRE_EQ(receiver.report_filter_losses(overlap, 300U)
                   .receiver_filter_loss_packets,
        0U);
}

TEST(session_ages_existing_fresh_loss_after_lossmaxttl_is_disabled)
{
    ReliabilitySession receiver {{
        .local_initial_sequence = SequenceNumber {1},
        .peer_initial_sequence = SequenceNumber {100},
        .send_capacity_packets = 16,
        .receive_capacity_packets = 16,
    }};
    receiver.configure_live({.retransmit_flag = true}, 0, PacketTimestamp {0});
    SocketOptions options;
    REQUIRE_EQ(options.set(SocketOption::maximum_reorder_tolerance_packets, 3),
        Error::none);
    REQUIRE_EQ(receiver.apply_dynamic_options(options), Error::none);
    const std::array payload {std::byte {'x'}};
    PacketView packet {
        .kind = PacketKind::data,
        .data = {.boundary = MessageBoundary::solo},
        .payload = payload,
    };
    const auto receive = [&](std::uint32_t sequence) {
        packet.data.sequence = SequenceNumber {sequence};
        packet.data.message_number = sequence;
        return receiver.receive(packet, sequence);
    };
    REQUIRE(receive(102));
    REQUIRE(receive(100));
    REQUIRE_EQ(receiver.reorder_tolerance_packets(), 2U);
    const auto gap = receive(104);
    REQUIRE(gap);
    REQUIRE_EQ(gap.receiver_loss_packets, 1U);
    REQUIRE_EQ(options.set(SocketOption::maximum_reorder_tolerance_packets, 0),
        Error::none);
    REQUIRE_EQ(receiver.apply_dynamic_options(options), Error::none);
    REQUIRE_EQ(receiver.reorder_tolerance_packets(), 0U);
    REQUIRE(receive(105));
    const auto aged = receive(106);
    REQUIRE(aged);
    bool reported = false;
    for (std::size_t index = 0; index < aged.actions.size; ++index) {
        if (aged.actions.values[index].kind
            == ReliabilityActionKind::loss_report) {
            for (const auto& range :
                aged.actions.loss_ranges(aged.actions.values[index])) {
                reported |= range.first == SequenceNumber {103}
                    && range.last == SequenceNumber {103};
            }
        }
    }
    REQUIRE(reported);
}

TEST(sender_rto_keeps_full_fallback_outside_periodic_live_arq)
{
    for (int mode = 0; mode < 4; ++mode) {
        ReliabilitySession sender {{
            .local_initial_sequence = SequenceNumber {10},
            .peer_initial_sequence = SequenceNumber {100},
            .peer_socket_id = 900,
            .send_capacity_packets = 4,
            .receive_capacity_packets = 4,
            .maximum_payload_size = 1,
        }};
        if (mode == 0) {
            sender.configure_file(true);
        } else {
            sender.configure_live(
                NegotiatedLiveOptions {.periodic_nak = mode != 1}, 0,
                PacketTimestamp {0});
            if (mode >= 2) {
                const auto filter = parse_packet_filter_configuration(mode == 2
                        ? "fec,cols:4,rows:1,arq:onreq"
                        : "fec,cols:4,rows:1,arq:never");
                REQUIRE(filter);
                sender.configure_packet_filter(filter.configuration, true);
            }
        }
        const std::array<std::byte, 1> input {};
        for (int i = 0; i < 3; ++i) {
            REQUIRE_EQ(
                sender.queue_message(input, PacketTimestamp {0}), Error::none);
            REQUIRE(sender.next_data_packet().has_value());
            sender.note_data_packet_sent(100);
        }
        REQUIRE(sender.poll_sender_retransmission_timeout(1'000'000));
        for (unsigned i = 0; i < 3; ++i) {
            const auto packet = sender.next_data_packet();
            REQUIRE(packet.has_value());
            REQUIRE_EQ(packet->header.sequence, SequenceNumber {10 + i});
            REQUIRE(packet->header.retransmitted);
        }
        REQUIRE(!sender.next_data_packet().has_value());
    }
}

TEST(sensor_profile_disarms_sender_rto_without_changing_ordinary_fec)
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
        NegotiatedLiveOptions {.periodic_nak = false}, 0, PacketTimestamp {0});
    const auto filter = parse_packet_filter_configuration(
        "fec-sensor-v1,cols:4,rows:1,arq:never");
    REQUIRE(filter);
    sender.configure_packet_filter(filter.configuration, true);

    const std::array<std::byte, 1> input {std::byte {'s'}};
    REQUIRE_EQ(sender.queue_message(input, PacketTimestamp {0}), Error::none);
    REQUIRE(sender.next_data_packet().has_value());
    sender.note_data_packet_sent(100);
    REQUIRE(!sender.poll_sender_retransmission_timeout(1'000'000));
    REQUIRE(!sender.next_data_packet().has_value());
}

TEST(sensor_profile_rejects_fragmented_samples_and_peer_nak_replay)
{
    ReliabilitySession sender {{
        .local_initial_sequence = SequenceNumber {10},
        .peer_initial_sequence = SequenceNumber {100},
        .peer_socket_id = 900,
        .send_capacity_packets = 4,
        .receive_capacity_packets = 4,
        .maximum_payload_size = 2,
    }};
    const auto filter = parse_packet_filter_configuration(
        "fec-sensor-v1,cols:4,rows:1,arq:never");
    REQUIRE(filter);
    sender.configure_packet_filter(filter.configuration, true);

    const std::array<std::byte, 3> oversized {};
    REQUIRE_EQ(sender.queue_message(oversized, PacketTimestamp {0}),
        Error::invalid_payload_size);
    const std::array<std::byte, 1> input {std::byte {'s'}};
    REQUIRE_EQ(sender.queue_message(input, PacketTimestamp {0}), Error::none);
    REQUIRE(sender.next_data_packet().has_value());
    const ReliabilityAction loss {
        .kind = ReliabilityActionKind::loss_report,
        .loss =
            {
                .first = SequenceNumber {10},
                .last = SequenceNumber {10},
            },
    };
    std::array<std::byte, 64> storage {};
    const auto nak = encode_and_decode(loss, storage);
    const auto ignored = sender.receive(nak, 100U);
    REQUIRE(ignored);
    REQUIRE_EQ(ignored.sender_loss_packets, 0U);
    REQUIRE_EQ(ignored.actions.size, 0U);
    REQUIRE(!sender.next_data_packet().has_value());

    ReliabilitySession receiver {{
        .local_initial_sequence = SequenceNumber {100},
        .peer_initial_sequence = SequenceNumber {20},
        .peer_socket_id = 800,
        .send_capacity_packets = 4,
        .receive_capacity_packets = 4,
        .maximum_payload_size = 2,
    }};
    receiver.configure_packet_filter(filter.configuration, true);
    PacketView invalid;
    invalid.kind = PacketKind::data;
    invalid.data.sequence = SequenceNumber {20};
    invalid.data.message_number = 1U;
    invalid.data.boundary = MessageBoundary::first;
    invalid.payload = input;
    REQUIRE_EQ(
        receiver.receive(invalid, 100U).error, Error::invalid_control_payload);
    invalid.data.boundary = MessageBoundary::solo;
    invalid.data.retransmitted = true;
    REQUIRE_EQ(
        receiver.receive(invalid, 100U).error, Error::invalid_control_payload);
}

TEST(sensor_profile_repeats_single_sequence_retirement_until_cumulative_ack)
{
    ReliabilitySession sender {{
        .local_initial_sequence = SequenceNumber {SequenceNumber::mask},
        .peer_initial_sequence = SequenceNumber {100},
        .peer_socket_id = 900,
        .send_capacity_packets = 4,
        .receive_capacity_packets = 4,
        .maximum_payload_size = 4,
    }};
    ReliabilitySession receiver {{
        .local_initial_sequence = SequenceNumber {100},
        .peer_initial_sequence = SequenceNumber {SequenceNumber::mask},
        .peer_socket_id = 800,
        .send_capacity_packets = 4,
        .receive_capacity_packets = 4,
        .maximum_payload_size = 4,
    }};
    const auto filter = parse_packet_filter_configuration(
        "fec-sensor-v1,cols:4,rows:1,arq:never");
    REQUIRE(filter);
    sender.configure_live(
        NegotiatedLiveOptions {.periodic_nak = false}, 0, PacketTimestamp {0});
    receiver.configure_live(
        NegotiatedLiveOptions {.periodic_nak = false}, 0, PacketTimestamp {0});
    sender.configure_packet_filter(filter.configuration, true);
    receiver.configure_packet_filter(filter.configuration, true);

    const std::array<std::byte, 1> expired {std::byte {'x'}};
    const std::array<std::byte, 1> following {std::byte {'y'}};
    REQUIRE_EQ(
        sender.queue_message(expired, PacketTimestamp {1}, true, 1U, 100U),
        Error::none);
    REQUIRE_EQ(
        sender.queue_message(following, PacketTimestamp {2}, true, 2U, 0U),
        Error::none);
    const auto lost_data = sender.next_data_packet();
    const auto later_data = sender.next_data_packet();
    REQUIRE(lost_data.has_value());
    REQUIRE(later_data.has_value());
    REQUIRE_EQ(
        lost_data->header.sequence, SequenceNumber {SequenceNumber::mask});
    REQUIRE_EQ(later_data->header.sequence, SequenceNumber {0});
    REQUIRE(receiver.receive(view_of(*later_data), 10U));

    std::array<std::byte, 1> output {};
    const auto delivered = receiver.pop_message(output);
    REQUIRE(delivered);
    REQUIRE_EQ(output, following);

    // Lose the initial retirement control.
    const auto initial = sender.drop_expired_sender_message(101U);
    REQUIRE_EQ(initial.size, 1U);
    REQUIRE_EQ(initial.values[0].drop.message_number, 0U);
    REQUIRE_EQ(initial.values[0].drop.sequences.first,
        SequenceNumber {SequenceNumber::mask});
    REQUIRE_EQ(initial.values[0].drop.sequences.last,
        SequenceNumber {SequenceNumber::mask});
    REQUIRE_EQ(*sender.next_sender_retirement_deadline(), 10'101U);
    REQUIRE_EQ(sender.poll_timers(10'100U).size, 0U);
    REQUIRE(!sender.has_pending_drop_requests());

    // The timer recreates the same control without replaying DATA.
    REQUIRE_EQ(sender.poll_timers(10'101U).size, 0U);
    REQUIRE(sender.has_pending_drop_requests());
    const auto repeated = sender.take_pending_drop_requests();
    REQUIRE_EQ(repeated.size, 1U);
    REQUIRE_EQ(repeated.values[0].drop.message_number, 0U);
    REQUIRE_EQ(repeated.values[0].drop.sequences.first,
        SequenceNumber {SequenceNumber::mask});
    REQUIRE_EQ(repeated.values[0].drop.sequences.last,
        SequenceNumber {SequenceNumber::mask});
    REQUIRE(!sender.next_data_packet().has_value());

    std::array<std::byte, 64> control_storage {};
    auto drop_packet = encode_and_decode(repeated.values[0], control_storage);
    const auto retired = receiver.receive(drop_packet, 10'102U);
    REQUIRE(retired);
    REQUIRE_EQ(retired.receiver_drop_packets, 1U);
    REQUIRE_EQ(retired.actions.size, 1U);
    REQUIRE_EQ(
        retired.actions.values[0].kind, ReliabilityActionKind::acknowledgement);
    REQUIRE_EQ(retired.actions.values[0].acknowledgement.kind,
        AcknowledgementKind::lite);
    REQUIRE_EQ(receiver.pop_message(output).error, Error::would_block);

    // Lose that ACK. The duplicate is idempotent and produces a new ACK.
    REQUIRE_EQ(sender.poll_timers(20'101U).size, 0U);
    const auto retry = sender.take_pending_drop_requests();
    REQUIRE_EQ(retry.size, 1U);
    drop_packet = encode_and_decode(retry.values[0], control_storage);
    const auto duplicate = receiver.receive(drop_packet, 20'102U);
    REQUIRE(duplicate);
    REQUIRE_EQ(duplicate.receiver_drop_packets, 0U);
    REQUIRE_EQ(duplicate.actions.size, 1U);
    const auto ack_packet =
        encode_and_decode(duplicate.actions.values[0], control_storage);
    REQUIRE(sender.receive(ack_packet, 20'103U));
    REQUIRE_EQ(sender.send_buffer().sequence_span(), 0U);
    REQUIRE(!sender.next_sender_retirement_deadline().has_value());
}

TEST(sensor_profile_delivers_newer_samples_before_gap_repair)
{
    ReliabilitySession receiver {{
        .local_initial_sequence = SequenceNumber {100},
        .peer_initial_sequence = SequenceNumber {10},
        .peer_socket_id = 800,
        .send_capacity_packets = 8,
        .receive_capacity_packets = 8,
        .maximum_payload_size = 4,
    }};
    const auto filter = parse_packet_filter_configuration(
        "fec-sensor-v1,cols:4,rows:1,arq:never");
    REQUIRE(filter);
    receiver.configure_live(
        NegotiatedLiveOptions {.periodic_nak = false}, 0, PacketTimestamp {0});
    receiver.configure_packet_filter(filter.configuration, true);

    const std::array<std::byte, 1> newer {std::byte {'n'}};
    PacketView packet;
    packet.kind = PacketKind::data;
    packet.data.sequence = SequenceNumber {11};
    packet.data.message_number = 2U;
    packet.data.boundary = MessageBoundary::solo;
    packet.payload = newer;
    REQUIRE(receiver.receive(packet, 100U));
    REQUIRE(receiver.message_ready_at(100U));
    REQUIRE_EQ(receiver.next_sensor_receive_gap_deadline(),
        std::optional<std::uint64_t> {20'100U});

    std::array<std::byte, 1> output {};
    const auto delivered_newer = receiver.pop_message_at(output, 100U);
    REQUIRE(delivered_newer);
    REQUIRE_EQ(delivered_newer.first_sequence, SequenceNumber {11});
    REQUIRE_EQ(output, newer);
    REQUIRE_EQ(
        receiver.receive_buffer().next_ack_sequence(), SequenceNumber {10});

    const std::array<std::byte, 1> repair {std::byte {'r'}};
    packet.data.sequence = SequenceNumber {10};
    packet.data.message_number = 1U;
    packet.payload = repair;
    REQUIRE(receiver.receive(packet, 20'099U));
    REQUIRE_EQ(
        receiver.receive_buffer().next_ack_sequence(), SequenceNumber {12});
    const auto delivered_repair = receiver.pop_message_at(output, 20'099U);
    REQUIRE(delivered_repair);
    REQUIRE_EQ(delivered_repair.first_sequence, SequenceNumber {10});
    REQUIRE_EQ(output, repair);
    REQUIRE(!receiver.next_sensor_receive_gap_deadline().has_value());
}

TEST(sensor_profile_expires_a_gap_at_deadline_during_silence)
{
    ReliabilitySession receiver {{
        .local_initial_sequence = SequenceNumber {100},
        .peer_initial_sequence = SequenceNumber {SequenceNumber::mask},
        .peer_socket_id = 800,
        .send_capacity_packets = 8,
        .receive_capacity_packets = 8,
        .maximum_payload_size = 4,
    }};
    const auto filter = parse_packet_filter_configuration(
        "fec-sensor-v1,cols:4,rows:1,arq:never");
    REQUIRE(filter);
    receiver.configure_live(
        NegotiatedLiveOptions {.periodic_nak = false}, 0, PacketTimestamp {0});
    receiver.configure_packet_filter(filter.configuration, true);

    const std::array<std::byte, 1> later {std::byte {'l'}};
    PacketView packet;
    packet.kind = PacketKind::data;
    packet.data.sequence = SequenceNumber {0};
    packet.data.message_number = 2U;
    packet.data.boundary = MessageBoundary::solo;
    packet.payload = later;
    REQUIRE(receiver.receive(packet, 500U));
    REQUIRE_EQ(receiver.next_sensor_receive_gap_deadline(),
        std::optional<std::uint64_t> {20'500U});

    const auto early = receiver.expire_sensor_receive_gaps(20'499U);
    REQUIRE(early);
    REQUIRE_EQ(early.receiver_drop_packets, 0U);
    const auto expired = receiver.expire_sensor_receive_gaps(20'500U);
    REQUIRE(expired);
    REQUIRE_EQ(expired.receiver_drop_packets, 1U);
    REQUIRE_EQ(expired.actions.size, 1U);
    REQUIRE_EQ(expired.actions.values[0].acknowledgement.kind,
        AcknowledgementKind::lite);
    REQUIRE_EQ(expired.actions.values[0].acknowledgement.next_sequence,
        SequenceNumber {1});
    REQUIRE(!receiver.next_sensor_receive_gap_deadline().has_value());

    const std::array<std::byte, 1> late {std::byte {'x'}};
    packet.data.sequence = SequenceNumber {SequenceNumber::mask};
    packet.data.message_number = 1U;
    packet.payload = late;
    const auto rejected = receiver.receive(packet, 20'500U);
    REQUIRE(rejected);
    REQUIRE(rejected.receiver_packet_belated);
    REQUIRE(!rejected.receiver_packet_accepted_unique);
}

TEST(sensor_profile_gap_deadlines_are_independent_and_do_not_extend)
{
    ReliabilitySession receiver {{
        .local_initial_sequence = SequenceNumber {100},
        .peer_initial_sequence = SequenceNumber {10},
        .peer_socket_id = 800,
        .send_capacity_packets = 8,
        .receive_capacity_packets = 8,
        .maximum_payload_size = 4,
    }};
    const auto filter = parse_packet_filter_configuration(
        "fec-sensor-v1,cols:4,rows:1,arq:never");
    REQUIRE(filter);
    receiver.configure_live(
        NegotiatedLiveOptions {.periodic_nak = false}, 0, PacketTimestamp {0});
    receiver.configure_packet_filter(filter.configuration, true);

    const std::array<std::byte, 1> payload {std::byte {'s'}};
    PacketView packet;
    packet.kind = PacketKind::data;
    packet.data.boundary = MessageBoundary::solo;
    packet.payload = payload;
    packet.data.sequence = SequenceNumber {11};
    packet.data.message_number = 2U;
    REQUIRE(receiver.receive(packet, 100U));
    REQUIRE_EQ(receiver.next_sensor_receive_gap_deadline(),
        std::optional<std::uint64_t> {20'100U});

    packet.data.sequence = SequenceNumber {13};
    packet.data.message_number = 4U;
    REQUIRE(receiver.receive(packet, 1'000U));
    REQUIRE_EQ(receiver.next_sensor_receive_gap_deadline(),
        std::optional<std::uint64_t> {20'100U});

    const auto first_expiration = receiver.expire_sensor_receive_gaps(20'100U);
    REQUIRE(first_expiration);
    REQUIRE_EQ(first_expiration.receiver_drop_packets, 1U);
    REQUIRE_EQ(
        receiver.receive_buffer().next_ack_sequence(), SequenceNumber {12});
    REQUIRE_EQ(receiver.next_sensor_receive_gap_deadline(),
        std::optional<std::uint64_t> {21'000U});

    packet.data.sequence = SequenceNumber {12};
    packet.data.message_number = 3U;
    REQUIRE(receiver.receive(packet, 20'999U));
    REQUIRE_EQ(
        receiver.receive_buffer().next_ack_sequence(), SequenceNumber {14});
    REQUIRE(!receiver.next_sensor_receive_gap_deadline().has_value());
}

TEST(live_session_late_ack_can_probe_already_delivered_tail_without_loss)
{
    for (const auto initial : {10U, SequenceNumber::mask}) {
        for (const auto kind :
            {AcknowledgementKind::lite, AcknowledgementKind::full}) {
            for (const bool late_ack : {false, true}) {
                const auto first = SequenceNumber {initial};
                ReliabilitySession sender {{
                    .local_initial_sequence = first,
                    .peer_initial_sequence = SequenceNumber {100},
                    .send_capacity_packets = 4,
                    .receive_capacity_packets = 4,
                    .maximum_payload_size = 1,
                }};
                ReliabilitySession receiver {{
                    .local_initial_sequence = SequenceNumber {100},
                    .peer_initial_sequence = first,
                    .send_capacity_packets = 4,
                    .receive_capacity_packets = 4,
                    .maximum_payload_size = 1,
                }};
                const NegotiatedLiveOptions options {.periodic_nak = true};
                sender.configure_live(options, 0, PacketTimestamp {0});
                receiver.configure_live(options, 0, PacketTimestamp {0});
                const std::array input {std::byte {'x'}, std::byte {'y'}};
                for (const auto byte : input) {
                    REQUIRE_EQ(sender.queue_message(
                                   std::span {&byte, 1}, PacketTimestamp {0}),
                        Error::none);
                    const auto packet = sender.next_data_packet();
                    REQUIRE(packet.has_value());
                    sender.note_data_packet_sent(100);
                    const auto received =
                        receiver.receive(view_of(*packet), 200);
                    REQUIRE(received);
                    REQUIRE(received.receiver_packet_accepted_unique);
                    REQUIRE_EQ(received.receiver_loss_packets, 0U);
                    for (const auto& action :
                        std::span {received.actions.values}.first(
                            received.actions.size)) {
                        REQUIRE(
                            action.kind != ReliabilityActionKind::loss_report);
                    }
                    std::array<std::byte, 1> output {};
                    REQUIRE(receiver.pop_message(output));
                    REQUIRE_EQ(output[0], byte);
                }
                REQUIRE_EQ(receiver.receive_buffer().next_ack_sequence(),
                    first.advanced(2));

                // Both originals reached the application. Only the cumulative
                // ACK is delayed; no NAK has selected a retransmission.
                std::array<std::byte, 64> control_storage {};
                const ReliabilityAction acknowledgement {
                    .kind = ReliabilityActionKind::acknowledgement,
                    .acknowledgement = {.kind = kind,
                        .acknowledgement_number =
                            kind == AcknowledgementKind::full ? 1U : 0U,
                        .next_sequence =
                            receiver.receive_buffer().next_ack_sequence(),
                        .round_trip_time_microseconds = 100'000,
                        .round_trip_time_variance_microseconds = 50'000},
                };
                REQUIRE(!sender.poll_sender_retransmission_timeout(330'099));
                if (late_ack) {
                    REQUIRE(sender.poll_sender_retransmission_timeout(330'100));
                    const auto probe = sender.next_data_packet();
                    REQUIRE(probe.has_value());
                    REQUIRE_EQ(probe->header.sequence, first.next());
                    REQUIRE(probe->header.retransmitted);
                    REQUIRE_EQ(probe->payload[0], input[1]);
                    REQUIRE(!sender.next_data_packet().has_value());
                    sender.note_data_packet_sent(330'100);
                    const auto duplicate =
                        receiver.receive(view_of(*probe), 330'101);
                    REQUIRE(duplicate);
                    REQUIRE(!duplicate.receiver_packet_accepted_unique);
                    REQUIRE_EQ(duplicate.receiver_loss_packets, 0U);
                    std::array<std::byte, 1> output {};
                    REQUIRE(!receiver.pop_message(output));
                }
                REQUIRE(sender.receive(
                    encode_and_decode(acknowledgement, control_storage),
                    late_ack ? 330'102 : 330'099));
                REQUIRE_EQ(sender.send_buffer().packets_in_flight(), 0U);
                REQUIRE(!sender.poll_sender_retransmission_timeout(2'000'000));
                REQUIRE(!sender.next_data_packet().has_value());
            }
        }
    }
}

TEST(live_session_periodic_nak_rto_probes_only_tail_of_unacknowledged_flight)
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
                .next_sequence = SequenceNumber {10},
            },
    };
    REQUIRE(sender.receive(
        encode_and_decode(acknowledgement, control_storage), 1'000));

    REQUIRE(!sender.poll_sender_retransmission_timeout(330'099));
    REQUIRE(sender.poll_sender_retransmission_timeout(330'100));
    const auto retransmission = sender.next_data_packet();
    REQUIRE(retransmission.has_value());
    REQUIRE_EQ(retransmission->header.sequence, SequenceNumber {11});
    REQUIRE(retransmission->header.retransmitted);
    REQUIRE(!sender.next_data_packet().has_value());

    // A lost probe or its ACK must leave recovery armed, but bounded to one
    // packet. Once the cumulative ACK arrives, probing must stop.
    sender.note_data_packet_sent(330'100);
    REQUIRE(sender.poll_sender_retransmission_timeout(2'000'000));
    const auto retry = sender.next_data_packet();
    REQUIRE(retry.has_value());
    REQUIRE_EQ(retry->header.sequence, SequenceNumber {11});
    REQUIRE(retry->header.retransmitted);
    REQUIRE(!sender.next_data_packet().has_value());
    acknowledgement.acknowledgement.next_sequence = SequenceNumber {12};
    REQUIRE(sender.receive(
        encode_and_decode(acknowledgement, control_storage), 2'000'001));
    REQUIRE(!sender.poll_sender_retransmission_timeout(4'000'000));
    REQUIRE(!sender.next_data_packet().has_value());
}

TEST(
    session_discarded_fragment_preserves_following_messages_in_any_arrival_order)
{
    for (const auto initial : {100U, SequenceNumber::mask - 2U}) {
        for (const bool preceding_message : {false, true}) {
            for (std::uint32_t rejected = 1; rejected <= 3; ++rejected) {
                std::array<std::uint32_t, 5> order {0, 1, 2, 3, 4};
                do {
                    const auto first = SequenceNumber {initial};
                    ReliabilitySession receiver {
                        {.local_initial_sequence = SequenceNumber {1},
                            .peer_initial_sequence =
                                preceding_message ? first : first.next(),
                            .send_capacity_packets = 8,
                            .receive_capacity_packets = 8}};
                    const std::array payload {std::byte {'x'}};
                    for (const auto index : order) {
                        if (index == 0 && !preceding_message) {
                            continue;
                        }
                        PacketView data {.kind = PacketKind::data,
                            .data = {.sequence = first.advanced(index),
                                .message_number = index == 0 ? 1U
                                    : index == 4             ? 3U
                                                             : 2U,
                                .boundary = index == 1 ? MessageBoundary::first
                                    : index == 2 ? MessageBoundary::subsequent
                                    : index == 3 ? MessageBoundary::last
                                                 : MessageBoundary::solo},
                            .payload = payload};
                        REQUIRE(receiver.receive(data, 100 + index,
                            {.discard_payload = index == rejected}));
                    }
                    REQUIRE_EQ(receiver.receive_buffer().next_ack_sequence(),
                        first.advanced(5));
                    std::array<std::byte, 8> output {};
                    if (preceding_message) {
                        const auto previous = receiver.pop_message(output);
                        REQUIRE(previous);
                        REQUIRE_EQ(previous.message_number, 1U);
                    }
                    const auto received = receiver.pop_message(output);
                    REQUIRE(received);
                    REQUIRE_EQ(received.message_number, 3U);
                    REQUIRE_EQ(received.bytes_written, 1U);
                    REQUIRE_EQ(receiver.receive_buffer().occupied(), 0U);
                } while (std::next_permutation(order.begin(), order.end()));
            }
        }
    }
}

TEST(session_discarded_message_does_not_acknowledge_missing_fragments)
{
    ReliabilitySession receiver {{.local_initial_sequence = SequenceNumber {1},
        .peer_initial_sequence = SequenceNumber {100},
        .send_capacity_packets = 8,
        .receive_capacity_packets = 8}};
    const std::array payload {std::byte {'x'}};
    const auto receive = [&](std::uint32_t index, bool discard) {
        PacketView data {.kind = PacketKind::data,
            .data = {.sequence = SequenceNumber {100 + index},
                .message_number = index == 3 ? 2U : 1U,
                .boundary = index == 0 ? MessageBoundary::first
                    : index == 1       ? MessageBoundary::subsequent
                    : index == 2       ? MessageBoundary::last
                                       : MessageBoundary::solo},
            .payload = payload};
        REQUIRE(
            receiver.receive(data, 100 + index, {.discard_payload = discard}));
    };
    receive(0, false);
    receive(2, true);
    receive(3, false);
    REQUIRE_EQ(
        receiver.receive_buffer().next_ack_sequence(), SequenceNumber {101});
    std::array<std::byte, 8> output {};
    REQUIRE(!receiver.pop_message(output));
    receive(1, false);
    REQUIRE_EQ(
        receiver.receive_buffer().next_ack_sequence(), SequenceNumber {104});
    const auto received = receiver.pop_message(output);
    REQUIRE(received);
    REQUIRE_EQ(received.message_number, 2U);
}

TEST(session_discarded_message_can_exceed_receive_window)
{
    ReliabilitySession receiver {{.local_initial_sequence = SequenceNumber {1},
        .peer_initial_sequence = SequenceNumber {100},
        .send_capacity_packets = 4,
        .receive_capacity_packets = 4}};
    const std::array payload {std::byte {'x'}};
    for (std::uint32_t index = 0; index <= 20; ++index) {
        PacketView data {.kind = PacketKind::data,
            .data = {.sequence = SequenceNumber {100 + index},
                .message_number = index == 20 ? 2U : 1U,
                .boundary = index == 0 ? MessageBoundary::first
                    : index == 19      ? MessageBoundary::last
                    : index == 20      ? MessageBoundary::solo
                                       : MessageBoundary::subsequent},
            .payload = payload};
        REQUIRE(receiver.receive(
            data, 100 + index, {.discard_payload = index == 0}));
        REQUIRE_EQ(receiver.receive_buffer().next_ack_sequence(),
            SequenceNumber {101 + index});
    }
    std::array<std::byte, 8> output {};
    const auto received = receiver.pop_message(output);
    REQUIRE(received);
    REQUIRE_EQ(received.message_number, 2U);
}

TEST(session_discarded_stream_packet_preserves_other_payload)
{
    ReliabilitySession receiver {{.local_initial_sequence = SequenceNumber {1},
        .peer_initial_sequence = SequenceNumber {100},
        .send_capacity_packets = 4,
        .receive_capacity_packets = 4}};
    receiver.set_message_api(false);
    const std::array payload {std::byte {'x'}};
    for (std::uint32_t index = 0; index < 3; ++index) {
        PacketView data {.kind = PacketKind::data,
            .data = {.sequence = SequenceNumber {100 + index},
                .message_number = 1,
                .boundary = index == 0 ? MessageBoundary::first
                    : index == 2       ? MessageBoundary::last
                                       : MessageBoundary::subsequent},
            .payload = payload};
        REQUIRE(receiver.receive(
            data, 100 + index, {.discard_payload = index == 1}));
    }
    std::array<std::byte, 8> output {};
    std::size_t delivered = 0;
    while (const auto received = receiver.pop_stream(output)) {
        delivered += received.bytes_written;
    }
    REQUIRE_EQ(delivered, 2U);
    REQUIRE_EQ(
        receiver.receive_buffer().next_ack_sequence(), SequenceNumber {103});
}

TEST(session_ackack_shorter_rtt_repeats_filter_nak_before_live_deadline)
{
    ReliabilitySession receiver {{
        .local_initial_sequence = SequenceNumber {1},
        .peer_initial_sequence = SequenceNumber {100},
        .send_capacity_packets = 16,
        .receive_capacity_packets = 16,
    }};
    receiver.configure_live(
        {
            .receive_tsbpd = true,
            .too_late_packet_drop = true,
            .sender_too_late_packet_drop = true,
            .periodic_nak = true,
            .retransmit_flag = true,
            .receive_delay_milliseconds = 120,
        },
        0, PacketTimestamp {0});
    const auto filter =
        parse_packet_filter_configuration("fec,cols:4,rows:1,arq:onreq");
    REQUIRE(filter);
    receiver.configure_packet_filter(filter.configuration, true);
    const std::array payload {std::byte {'x'}};
    PacketView packet {
        .kind = PacketKind::data,
        .data = {.sequence = SequenceNumber {102},
            .message_number = 3,
            .boundary = MessageBoundary::solo},
        .payload = payload,
    };
    REQUIRE(receiver.receive(packet, 100));
    const std::array losses {
        SequenceRange {SequenceNumber {100}, SequenceNumber {101}}};
    REQUIRE_EQ(receiver.report_filter_losses(losses, 200).actions.size, 1U);
    const auto ack = receiver.poll_timers(10'000);
    REQUIRE_EQ(ack.size, 1U);
    REQUIRE_EQ(ack.values[0].kind, ReliabilityActionKind::acknowledgement);
    std::array<std::byte, 64> storage {};
    ReliabilityAction ackack {
        .kind = ReliabilityActionKind::acknowledgement_of_ack,
        .acknowledgement_number =
            ack.values[0].acknowledgement.acknowledgement_number,
    };
    REQUIRE(receiver.receive(encode_and_decode(ackack, storage), 11'000));
    // The first NAK was lost. No further DATA arrives to refresh the timer.
    // A 1 ms RTT must replace the initial 150 ms repeat deadline.
    const auto repeat = receiver.poll_timers(71'000);
    REQUIRE_EQ(repeat.size, 1U);
    REQUIRE_EQ(repeat.values[0].kind, ReliabilityActionKind::loss_report);
    const auto ranges = repeat.loss_ranges(repeat.values[0]);
    REQUIRE_EQ(ranges.size(), 1U);
    REQUIRE_EQ(ranges[0].first, SequenceNumber {100});
    REQUIRE_EQ(ranges[0].last, SequenceNumber {101});
    REQUIRE_EQ(
        receiver.drop_too_late_receiver(71'000).receiver_drop_packets, 0U);
}

TEST(session_stream_receiver_drops_a_lost_packet_inside_a_chunk_per_packet)
{
    ReliabilitySession receiver {{
        .local_initial_sequence = SequenceNumber {100},
        .peer_initial_sequence = SequenceNumber {10},
        .peer_socket_id = 900,
        .send_capacity_packets = 8,
        .receive_capacity_packets = 8,
    }};
    receiver.configure_live(
        {
            .receive_tsbpd = true,
            .too_late_packet_drop = true,
            .sender_too_late_packet_drop = true,
            .periodic_nak = true,
            .retransmit_flag = true,
            .receive_delay_milliseconds = 100,
        },
        1'000, PacketTimestamp {0});
    receiver.set_message_api(false);

    // One stream chunk of three packets; its first packet (10) is lost.
    const std::array<std::byte, 1> second {std::byte {'b'}};
    const std::array<std::byte, 1> third {std::byte {'c'}};
    PacketView packet;
    packet.kind = PacketKind::data;
    packet.data.message_number = 1;
    packet.data.timestamp = PacketTimestamp {50};
    packet.data.sequence = SequenceNumber {11};
    packet.data.boundary = MessageBoundary::subsequent;
    packet.payload = second;
    REQUIRE(receiver.receive(packet, 1'010));
    packet.data.sequence = SequenceNumber {12};
    packet.data.boundary = MessageBoundary::last;
    packet.payload = third;
    REQUIRE(receiver.receive(packet, 1'011));

    // Without a message head there is no complete message, but the stream
    // API delivers by packet: the drop is due at packet 11's delivery time.
    REQUIRE_EQ(receiver.next_receive_delivery_time(),
        std::optional<std::uint64_t> {101'050});
    REQUIRE(!receiver.stream_ready_at(101'049));
    REQUIRE_EQ(
        receiver.drop_too_late_receiver(101'049).receiver_drop_packets, 0U);
    const auto dropped = receiver.drop_too_late_receiver(101'050);
    REQUIRE(dropped);
    // Only the lost packet is dropped; the chunk's later bytes survive.
    REQUIRE_EQ(dropped.receiver_drop_packets, 1U);
    REQUIRE_EQ(dropped.actions.values[0].acknowledgement.next_sequence,
        SequenceNumber {13});
    REQUIRE(receiver.stream_ready_at(101'050));
    std::array<std::byte, 4> output {};
    const auto popped = receiver.pop_stream_at(output, 101'050);
    REQUIRE(popped);
    REQUIRE_EQ(popped.bytes_written, 2U);
    REQUIRE_EQ(output[0], std::byte {'b'});
    REQUIRE_EQ(output[1], std::byte {'c'});
    REQUIRE(!receiver.stream_ready_at(101'050));
}

TEST(session_stream_pop_stops_in_front_of_a_packet_not_yet_due)
{
    ReliabilitySession receiver {{
        .local_initial_sequence = SequenceNumber {100},
        .peer_initial_sequence = SequenceNumber {10},
        .peer_socket_id = 900,
        .send_capacity_packets = 8,
        .receive_capacity_packets = 8,
    }};
    receiver.configure_live(
        {
            .receive_tsbpd = true,
            .receive_delay_milliseconds = 100,
        },
        1'000, PacketTimestamp {0});
    receiver.set_message_api(false);

    const std::array<std::byte, 1> first {std::byte {'a'}};
    const std::array<std::byte, 1> second {std::byte {'b'}};
    PacketView packet;
    packet.kind = PacketKind::data;
    packet.data.message_number = 1;
    packet.data.boundary = MessageBoundary::solo;
    packet.data.sequence = SequenceNumber {10};
    packet.data.timestamp = PacketTimestamp {50};
    packet.payload = first;
    REQUIRE(receiver.receive(packet, 1'010));
    packet.data.sequence = SequenceNumber {11};
    packet.data.timestamp = PacketTimestamp {5'050};
    packet.payload = second;
    REQUIRE(receiver.receive(packet, 1'011));

    std::array<std::byte, 4> output {};
    REQUIRE_EQ(
        receiver.pop_stream_at(output, 101'049).error, Error::would_block);
    // Packet 10 is due at 101'050, packet 11 only at 106'050: one byte.
    const auto popped = receiver.pop_stream_at(output, 101'050);
    REQUIRE(popped);
    REQUIRE_EQ(popped.bytes_written, 1U);
    REQUIRE_EQ(output[0], std::byte {'a'});
    REQUIRE_EQ(popped.delivery_time_microseconds,
        std::optional<std::uint64_t> {101'050});
    REQUIRE(!receiver.stream_ready_at(106'049));
    REQUIRE(receiver.stream_ready_at(106'050));
    const auto rest = receiver.pop_stream_at(output, 106'050);
    REQUIRE(rest);
    REQUIRE_EQ(rest.bytes_written, 1U);
    REQUIRE_EQ(output[0], std::byte {'b'});
}

TEST(session_loss_reports_wait_for_rtt_per_range_without_delaying_new_gaps)
{
    for (const bool filtered : {false, true}) {
        ReliabilitySession receiver {{
            .local_initial_sequence = SequenceNumber {100},
            .peer_initial_sequence = SequenceNumber {10},
            .send_capacity_packets = 32,
            .receive_capacity_packets = 32,
        }};
        receiver.configure_live({.periodic_nak = true, .retransmit_flag = true},
            0, PacketTimestamp {0});
        if (filtered) {
            const auto filter = parse_packet_filter_configuration(
                "fec,cols:4,rows:1,arq:onreq");
            REQUIRE(filter);
            receiver.configure_packet_filter(filter.configuration, true);
        }
        std::array<std::byte, 64> ack_storage {};
        const auto acknowledgement = encode_and_decode(
            {
                .kind = ReliabilityActionKind::acknowledgement,
                .acknowledgement =
                    {
                        .acknowledgement_number = 1,
                        .next_sequence = SequenceNumber {100},
                        .round_trip_time_microseconds = 50'000,
                        .round_trip_time_variance_microseconds = 0,
                        .available_receive_buffer_packets = 32,
                    },
            },
            ack_storage);
        REQUIRE(receiver.receive(acknowledgement, 1));
        REQUIRE_EQ(receiver.rtt().nak_interval_microseconds(), 25'000U);
        const std::array payload {std::byte {'n'}};
        PacketView data {
            .kind = PacketKind::data,
            .data = {.sequence = SequenceNumber {12},
                .message_number = 3,
                .boundary = MessageBoundary::solo},
            .payload = payload,
        };
        const auto count_reports = [](const ReliabilityActions& actions) {
            std::size_t count = 0;
            for (std::size_t index = 0; index < actions.size; ++index) {
                if (actions.values[index].kind
                    == ReliabilityActionKind::loss_report) {
                    count += actions.loss_ranges(actions.values[index]).size();
                }
            }
            return count;
        };
        auto first = receiver.receive(data, 100);
        REQUIRE(first);
        if (filtered) {
            const std::array losses {
                SequenceRange {SequenceNumber {10}, SequenceNumber {11}}};
            first = receiver.report_filter_losses(losses, 100);
            REQUIRE(first);
        }
        REQUIRE_EQ(count_reports(first.actions), 1U);
        REQUIRE_EQ(count_reports(receiver.poll_timers(25'100)), 0U);
        data.data.sequence = SequenceNumber {14};
        data.data.message_number = 5;
        auto new_gap = receiver.receive(data, 30'100);
        REQUIRE(new_gap);
        if (filtered) {
            const std::array losses {
                SequenceRange {SequenceNumber {13}, SequenceNumber {13}}};
            new_gap = receiver.report_filter_losses(losses, 30'100);
            REQUIRE(new_gap);
        }
        REQUIRE_EQ(count_reports(new_gap.actions), 1U);
        REQUIRE_EQ(new_gap.actions.values[0].loss.first, SequenceNumber {13});
        const auto retried = receiver.poll_timers(50'100);
        REQUIRE_EQ(count_reports(retried), 1U);
        for (std::size_t index = 0; index < retried.size; ++index) {
            if (retried.values[index].kind
                == ReliabilityActionKind::loss_report) {
                REQUIRE_EQ(
                    retried.values[index].loss.first, SequenceNumber {10});
            }
        }
        REQUIRE_EQ(count_reports(receiver.poll_timers(75'100)), 0U);
        REQUIRE_EQ(count_reports(receiver.poll_timers(100'100)), 2U);
    }
}

TEST(session_negotiates_sender_drop_from_peer_receiver_policy)
{
    for (const bool local_drop : {false, true}) {
        for (const bool peer_drop : {false, true}) {
            HandshakeExtensionParameters local;
            HandshakeExtensionParameters peer;
            if (!local_drop) {
                local.flags &= ~static_cast<std::uint32_t>(
                    HandshakeExtensionFlag::too_late_packet_drop);
            }
            if (!peer_drop) {
                peer.flags &= ~static_cast<std::uint32_t>(
                    HandshakeExtensionFlag::too_late_packet_drop);
            }
            ReliabilitySession session {{
                .local_initial_sequence = SequenceNumber {10},
                .peer_initial_sequence = SequenceNumber {20},
                .send_capacity_packets = 4,
                .receive_capacity_packets = 4,
            }};
            session.configure_live(
                negotiate_live_options(local, peer), 0, PacketTimestamp {0});
            const std::array<std::byte, 1> payload {std::byte {'s'}};
            REQUIRE_EQ(
                session.queue_message(payload, PacketTimestamp {0}, true, 1),
                Error::none);
            REQUIRE(session.next_data_packet());
            REQUIRE_EQ(session.drop_too_late_sender(1'020'001).size,
                peer_drop ? 1U : 0U);
        }
    }
}

TEST(session_negotiates_receiver_drop_from_local_policy)
{
    for (const bool local_drop : {false, true}) {
        for (const bool peer_drop : {false, true}) {
            HandshakeExtensionParameters local;
            HandshakeExtensionParameters peer;
            local.receiver_tsbpd_delay_milliseconds = 100;
            peer.sender_tsbpd_delay_milliseconds = 100;
            if (!local_drop) {
                local.flags &= ~static_cast<std::uint32_t>(
                    HandshakeExtensionFlag::too_late_packet_drop);
            }
            if (!peer_drop) {
                peer.flags &= ~static_cast<std::uint32_t>(
                    HandshakeExtensionFlag::too_late_packet_drop);
            }
            // Reception must also work when the peer does not receive TSBPD.
            peer.flags &= ~static_cast<std::uint32_t>(
                HandshakeExtensionFlag::tsbpd_receive);
            ReliabilitySession session {{
                .local_initial_sequence = SequenceNumber {100},
                .peer_initial_sequence = SequenceNumber {10},
                .send_capacity_packets = 4,
                .receive_capacity_packets = 4,
            }};
            session.configure_live(negotiate_live_options(local, peer), 1'000,
                PacketTimestamp {0});
            const std::array<std::byte, 1> payload {std::byte {'r'}};
            PacketView packet;
            packet.kind = PacketKind::data;
            packet.data.sequence = SequenceNumber {11};
            packet.data.message_number = 2;
            packet.data.boundary = MessageBoundary::solo;
            packet.data.timestamp = PacketTimestamp {50};
            packet.payload = payload;
            REQUIRE(session.receive(packet, 1'010));
            REQUIRE_EQ(
                session.drop_too_late_receiver(101'050).receiver_drop_packets,
                local_drop ? 1U : 0U);
            REQUIRE_EQ(session.message_ready_at(101'050), local_drop);
        }
    }
}

TEST(session_low_rtt_loss_reports_do_not_apply_a_second_timer_floor)
{
    for (const bool filtered : {false, true}) {
        ReliabilitySession receiver {{
            .local_initial_sequence = SequenceNumber {100},
            .peer_initial_sequence = SequenceNumber {10},
            .send_capacity_packets = 32,
            .receive_capacity_packets = 32,
        }};
        receiver.configure_live({.periodic_nak = true, .retransmit_flag = true},
            0, PacketTimestamp {0});
        if (filtered) {
            const auto filter = parse_packet_filter_configuration(
                "fec,cols:4,rows:1,arq:onreq");
            REQUIRE(filter);
            receiver.configure_packet_filter(filter.configuration, true);
        }
        std::array<std::byte, 64> ack_storage {};
        const auto ack = encode_and_decode(
            {
                .kind = ReliabilityActionKind::acknowledgement,
                .acknowledgement =
                    {
                        .acknowledgement_number = 1,
                        .next_sequence = SequenceNumber {100},
                        .round_trip_time_microseconds = 1'000,
                        .round_trip_time_variance_microseconds = 0,
                        .available_receive_buffer_packets = 32,
                    },
            },
            ack_storage);
        REQUIRE(receiver.receive(ack, 1));
        REQUIRE_EQ(receiver.rtt().nak_interval_microseconds(), 20'000U);
        const std::array payload {std::byte {'n'}};
        PacketView data {
            .kind = PacketKind::data,
            .data = {.sequence = SequenceNumber {12},
                .message_number = 3,
                .boundary = MessageBoundary::solo},
            .payload = payload,
        };
        REQUIRE(receiver.receive(data, 100));
        if (filtered) {
            const std::array losses {
                SequenceRange {SequenceNumber {10}, SequenceNumber {11}}};
            REQUIRE(receiver.report_filter_losses(losses, 100));
        }
        data.data.sequence = SequenceNumber {14};
        data.data.message_number = 5;
        REQUIRE(receiver.receive(data, 19'100));
        if (filtered) {
            const std::array losses {
                SequenceRange {SequenceNumber {13}, SequenceNumber {13}}};
            REQUIRE(receiver.report_filter_losses(losses, 19'100));
        }
        const auto reports = receiver.poll_timers(20'100);
        std::size_t reported_ranges = 0;
        for (const auto& action :
            std::span {reports.values}.first(reports.size)) {
            if (action.kind == ReliabilityActionKind::loss_report) {
                reported_ranges += reports.loss_ranges(action).size();
            }
        }
        REQUIRE_EQ(reported_ranges, 2U);
    }
}

TEST(session_blackhole_live_reports_missing_prefix_on_beyond_window_data)
{
    for (auto initial :
        {SequenceNumber {10}, SequenceNumber {SequenceNumber::mask - 2U}}) {
        ReliabilitySession receiver {{.peer_initial_sequence = initial,
            .send_capacity_packets = 4,
            .receive_capacity_packets = 4}};
        receiver.configure_live({.periodic_nak = true}, 0, PacketTimestamp {0});
        const std::array<std::byte, 1> payload {std::byte {'n'}};
        PacketView probe;
        probe.kind = PacketKind::data;
        probe.data.sequence = initial.advanced(6U);
        probe.data.boundary = MessageBoundary::solo;
        probe.data.message_number = 7;
        probe.payload = payload;
        const auto result = receiver.receive(probe, 100U);
        REQUIRE(result);
        REQUIRE_EQ(receiver.receive_buffer().next_ack_sequence(), initial);
        REQUIRE_EQ(receiver.receive_buffer().occupied(), 0U);
        bool reported = false;
        for (std::size_t i = 0; i < result.actions.size; ++i) {
            const auto& action = result.actions.values[i];
            if (action.kind == ReliabilityActionKind::loss_report) {
                REQUIRE_EQ(action.loss.first, initial);
                REQUIRE_EQ(action.loss.last, initial.advanced(3U));
                reported = true;
            }
        }
        REQUIRE(reported);
    }
}

TEST(session_blackhole_sensor_full_expired_window_admits_fresh_data)
{
    for (auto initial :
        {SequenceNumber {10}, SequenceNumber {SequenceNumber::mask - 2U}}) {
        ReliabilitySession sender {{.local_initial_sequence = initial,
            .peer_initial_sequence = SequenceNumber {100},
            .peer_socket_id = 900,
            .send_capacity_packets = 4,
            .receive_capacity_packets = 4}};
        ReliabilitySession receiver {
            {.local_initial_sequence = SequenceNumber {100},
                .peer_initial_sequence = initial,
                .peer_socket_id = 800,
                .send_capacity_packets = 4,
                .receive_capacity_packets = 4}};
        const auto filter = parse_packet_filter_configuration(
            "fec-sensor-v1,cols:4,rows:1,arq:never");
        REQUIRE(filter);
        sender.configure_live({.periodic_nak = false}, 0, PacketTimestamp {0});
        receiver.configure_live(
            {.periodic_nak = false}, 0, PacketTimestamp {0});
        sender.configure_packet_filter(filter.configuration, true);
        receiver.configure_packet_filter(filter.configuration, true);
        const std::array<std::byte, 1> payload {std::byte {'n'}};
        for (unsigned round = 0; round < 3; ++round) {
            for (unsigned i = 0; i < 4; ++i) {
                REQUIRE_EQ(sender.queue_message(payload,
                               PacketTimestamp {round * 1000U + i}, true,
                               round * 1000U + i, round * 1000U + 100U),
                    Error::none);
                REQUIRE(sender.next_data_packet()
                        .has_value()); // Blackhole every DATA.
            }
            REQUIRE_EQ(sender.queue_message(payload, PacketTimestamp {0}),
                Error::buffer_too_small);
            for (unsigned i = 0; i < 4; ++i)
                REQUIRE_EQ(
                    sender.drop_expired_sender_message(round * 1000U + 101U)
                        .size,
                    1U);
        }
        const auto span_before_invalid = sender.send_buffer().sequence_span();
        REQUIRE_EQ(sender.queue_message({}, PacketTimestamp {3000}),
            Error::invalid_state);
        REQUIRE_EQ(sender.send_buffer().sequence_span(), span_before_invalid);
        REQUIRE_EQ(sender.queue_message(
                       payload, PacketTimestamp {3000}, true, 3000, 0),
            Error::none);
        const auto fresh = sender.next_data_packet();
        REQUIRE(fresh);
        REQUIRE_EQ(fresh->header.sequence, initial.advanced(12U));
        std::array<std::byte, 64> storage {};
        // Retirement alone must never ACK unseen DATA. Deliver the observed
        // probe afterwards; repeated single-sequence controls must release
        // the entire lost prefix without ever replaying expired DATA.
        for (unsigned turn = 0; turn < 40; ++turn) {
            (void)sender.poll_timers(20'000U + turn * 10'000U);
            while (sender.has_pending_drop_requests()) {
                const auto controls = sender.take_pending_drop_requests();
                for (std::size_t i = 0; i < controls.size; ++i) {
                    const auto& action = controls.values[i];
                    REQUIRE_EQ(
                        action.kind, ReliabilityActionKind::drop_request);
                    REQUIRE_EQ(action.drop.sequences.first,
                        action.drop.sequences.last);
                    const auto result =
                        receiver.receive(encode_and_decode(action, storage),
                            20'001U + turn * 10'000U);
                    REQUIRE(result);
                    for (std::size_t j = 0; j < result.actions.size; ++j)
                        REQUIRE(sender.receive(
                            encode_and_decode(
                                result.actions.values[j], storage),
                            20'002U + turn * 10'000U));
                }
            }
            if (turn == 0) {
                REQUIRE_EQ(
                    receiver.receive_buffer().next_ack_sequence(), initial);
                REQUIRE(receiver.receive(view_of(*fresh), 20'003U));
            }
        }
        std::array<std::byte, 1> output {};
        REQUIRE(receiver.pop_message(output));
        REQUIRE_EQ(output, payload);
        REQUIRE_EQ(sender.send_buffer().sequence_span(), 0U);
        REQUIRE(!sender.next_data_packet().has_value());
        REQUIRE(!sender.next_sender_retirement_deadline().has_value());
    }
}

TEST(session_normalizes_duplicate_overlapping_nak_ranges_across_rollover)
{
    for (const auto initial :
        {SequenceNumber {100}, SequenceNumber {SequenceNumber::mask - 3U}}) {
        for (const bool unsent_tail : {false, true}) {
            ReliabilitySession sender {{
                .local_initial_sequence = initial,
                .send_capacity_packets = 16,
                .receive_capacity_packets = 16,
                .maximum_payload_size = 1,
            }};
            const std::array<std::byte, 8> message {};
            REQUIRE_EQ(sender.queue_message(message, PacketTimestamp {0}),
                Error::none);
            for (unsigned i = 0; i < (unsent_tail ? 7U : 8U); ++i)
                REQUIRE(sender.next_data_packet().has_value());
            std::array<SequenceRange, maximum_loss_ranges_per_report> ranges {};
            for (std::size_t i = 0; i < ranges.size(); ++i)
                ranges[i] = {initial.advanced(2U - i % 3U),
                    initial.advanced(7U - i % 2U)};
            const ReliabilityAction action {
                .kind = ReliabilityActionKind::loss_report, .loss = ranges[0]};
            std::array<std::byte, maximum_data_payload_size + 16U> storage {};
            const auto encoded = encode_reliability_action(
                action, ranges, PacketTimestamp {0}, 1, storage);
            REQUIRE(encoded);
            const auto decoded =
                decode_packet(std::span {storage}.first(encoded.bytes_written));
            REQUIRE(decoded);
            const auto result = sender.receive(decoded.packet, 100);
            if (unsent_tail) {
                REQUIRE_EQ(result.error, Error::invalid_control_payload);
                const auto last = sender.next_data_packet();
                REQUIRE(last.has_value());
                REQUIRE(!last->header.retransmitted);
                REQUIRE_EQ(last->header.sequence, initial.advanced(7));
            } else {
                REQUIRE(result);
                REQUIRE_EQ(result.sender_loss_packets, 8U);
                REQUIRE_EQ(result.sender_loss_bytes, 8U);
                for (unsigned i = 0; i < 8; ++i) {
                    const auto packet = sender.next_data_packet();
                    REQUIRE(packet.has_value());
                    REQUIRE(packet->header.retransmitted);
                    REQUIRE_EQ(packet->header.sequence, initial.advanced(i));
                }
            }
            REQUIRE(!sender.next_data_packet().has_value());
        }
    }
}

TEST(session_large_nak_expansion_is_bounded_and_repeated_reports_make_progress)
{
    for (const auto capacity : {1024U, 8192U, 65536U}) {
        const SequenceNumber initial {SequenceNumber::mask - 300U};
        ReliabilitySession sender {{
            .local_initial_sequence = initial,
            .send_capacity_packets = capacity,
            .receive_capacity_packets = 16,
            .maximum_payload_size = 1,
        }};
        const std::vector<std::byte> message(capacity);
        REQUIRE_EQ(
            sender.queue_message(message, PacketTimestamp {0}), Error::none);
        for (unsigned i = 0; i < capacity; ++i)
            REQUIRE(sender.next_data_packet().has_value());
        const ReliabilityAction action {
            .kind = ReliabilityActionKind::loss_report,
            .loss = {initial, initial.advanced(capacity - 1U)},
        };
        std::array<std::byte, 64> storage {};
        const auto encoded =
            encode_reliability_action(action, PacketTimestamp {0}, 1, storage);
        REQUIRE(encoded);
        const auto decoded =
            decode_packet(std::span {storage}.first(encoded.bytes_written));
        REQUIRE(decoded);
        std::size_t total = 0;
        for (unsigned turn = 0; turn < capacity / 256U; ++turn) {
            // Continuously repeat the whole window while its tail is pending.
            const auto result = sender.receive(decoded.packet, turn + 1U);
            REQUIRE(result);
            REQUIRE_EQ(result.sender_loss_packets, 256U);
            total += result.sender_loss_packets;
        }
        REQUIRE_EQ(total, capacity);
        // No wire repeats are needed to complete a single large announcement.
        std::size_t drained = 0;
        while (sender.next_data_packet().has_value())
            ++drained;
        REQUIRE_EQ(drained, capacity);
    }
}

TEST(session_large_nak_finishes_without_another_report_and_survives_ack)
{
    const SequenceNumber initial {123};
    ReliabilitySession sender {{
        .local_initial_sequence = initial,
        .send_capacity_packets = 1024,
        .receive_capacity_packets = 16,
        .maximum_payload_size = 1,
    }};
    const std::vector<std::byte> message(1024);
    REQUIRE_EQ(sender.queue_message(message, PacketTimestamp {0}), Error::none);
    for (unsigned i = 0; i < 1024; ++i)
        REQUIRE(sender.next_data_packet().has_value());
    const ReliabilityAction action {
        .kind = ReliabilityActionKind::loss_report,
        .loss = {initial, initial.advanced(1023)},
    };
    std::array<std::byte, 64> storage {};
    const auto encoded =
        encode_reliability_action(action, PacketTimestamp {0}, 1, storage);
    REQUIRE(encoded);
    const auto decoded =
        decode_packet(std::span {storage}.first(encoded.bytes_written));
    REQUIRE(decoded);
    REQUIRE_EQ(sender.receive(decoded.packet, 1).sender_loss_packets, 256U);
    const ReliabilityAction ack {
        .kind = ReliabilityActionKind::acknowledgement,
        .acknowledgement = {.kind = AcknowledgementKind::lite,
            .next_sequence = initial.advanced(512)},
    };
    REQUIRE(sender.receive(encode_and_decode(ack, storage), 2));
    REQUIRE_EQ(sender.service_pending_naks(2).sender_loss_packets, 256U);
    REQUIRE_EQ(sender.service_pending_naks(3).sender_loss_packets, 256U);
    REQUIRE_EQ(sender.service_pending_naks(4).sender_loss_packets, 0U);
    std::size_t drained = 0;
    while (const auto packet = sender.next_data_packet()) {
        REQUIRE(packet->header.retransmitted);
        REQUIRE_EQ(packet->header.sequence, initial.advanced(512U + drained));
        ++drained;
    }
    REQUIRE_EQ(drained, 512U);
}

TEST(session_sparse_readiness_survives_duplicate_controls_and_clock_progress)
{
    const std::array payload {std::byte {'s'}};
    for (const bool timed : {false, true}) {
        for (const bool messages : {false, true}) {
            for (const auto initial : {SequenceNumber {100},
                     SequenceNumber {SequenceNumber::mask - 7U}}) {
                ReliabilitySession receiver {{.peer_initial_sequence = initial,
                    .send_capacity_packets = 4,
                    .receive_capacity_packets = 65'536}};
                receiver.set_message_api(messages);
                receiver.configure_live({.receive_tsbpd = timed,
                                            .too_late_packet_drop = false,
                                            .receive_delay_milliseconds = 100},
                    1'000, PacketTimestamp {0});
                PacketView distant_packet;
                distant_packet.kind = PacketKind::data;
                distant_packet.data.sequence = initial.advanced(65'535);
                distant_packet.data.message_number = 1;
                distant_packet.data.boundary = MessageBoundary::solo;
                distant_packet.data.timestamp = PacketTimestamp {25};
                distant_packet.payload = payload;
                REQUIRE(receiver.receive(distant_packet, 1'001));
                std::size_t inspected = 0;
                REQUIRE(detail::ReceiveBufferTestAccess::message(
                    receiver.receive_buffer(), inspected));
                REQUIRE(detail::ReceiveBufferTestAccess::packet(
                    receiver.receive_buffer(), inspected));
                REQUIRE_EQ(inspected, 2U);
                const std::array<std::byte, 4> zero_word {};
                PacketView keepalive;
                keepalive.payload = zero_word;
                keepalive.kind = PacketKind::control;
                keepalive.control.type = ControlType::keepalive;
                for (int turn = 0; turn < 64; ++turn) {
                    REQUIRE(receiver.receive(keepalive, 1'010 + turn));
                    REQUIRE(receiver.receive(distant_packet, 1'010 + turn));
                    REQUIRE(!receiver.data_ready_at(1'010 + turn));
                    REQUIRE(!receiver.next_receive_delivery_time());
                    inspected = 0;
                    REQUIRE(detail::ReceiveBufferTestAccess::message(
                        receiver.receive_buffer(), inspected));
                    REQUIRE(detail::ReceiveBufferTestAccess::packet(
                        receiver.receive_buffer(), inspected));
                    REQUIRE_EQ(inspected, 0U);
                }
                auto head = distant_packet;
                head.data.sequence = initial;
                head.data.message_number = 2;
                head.data.timestamp = PacketTimestamp {5};
                REQUIRE(receiver.receive(head, 1'100));
                inspected = 0;
                const auto new_message =
                    detail::ReceiveBufferTestAccess::message(
                        receiver.receive_buffer(), inspected);
                const auto new_packet = detail::ReceiveBufferTestAccess::packet(
                    receiver.receive_buffer(), inspected);
                REQUIRE(new_message);
                REQUIRE(new_packet);
                REQUIRE_EQ(new_message->first_sequence, initial);
                REQUIRE_EQ(new_packet->first_sequence, initial);
                REQUIRE_EQ(inspected, 2U);
                REQUIRE_EQ(receiver.data_ready_at(1'100), !timed);
                const auto deadline = receiver.next_receive_delivery_time();
                REQUIRE_EQ(deadline.has_value(), timed);
                if (timed) {
                    REQUIRE_EQ(*deadline, 101'005U);
                    REQUIRE(!receiver.data_ready_at(*deadline - 1U));
                    REQUIRE(receiver.data_ready_at(*deadline));
                }
                REQUIRE(receiver.data_ready_at(101'005));
                inspected = 0;
                REQUIRE(detail::ReceiveBufferTestAccess::message(
                    receiver.receive_buffer(), inspected));
                REQUIRE_EQ(inspected, 0U);
                std::array<std::byte, 1> output {};
                const auto result = messages
                    ? receiver.pop_message_at(output, 101'005)
                    : receiver.pop_stream_at(output, 101'005);
                REQUIRE(result);
                REQUIRE_EQ(output, payload);
                REQUIRE(!receiver.data_ready_at(101'005));
            }
        }
    }
}

TEST(peer_drop_queue_balanced_admission_is_bounded_in_descending_wire_order)
{
    for (const std::uint32_t capacity : {1U, 64U, 8192U, 65535U, 65536U}) {
        detail::PeerDropQueue queue(capacity);
        REQUIRE_EQ(queue.initialized_slots(), 0U);
        const auto allocation = queue.allocated_slots();
        REQUIRE(allocation >= capacity);
        for (std::uint32_t offset = capacity; offset > 0; --offset) {
            const SequenceRange range {
                SequenceNumber {offset}, SequenceNumber {offset}};
            std::size_t steps = 0;
            REQUIRE(queue.insert(range, offset, &steps));
            REQUIRE(steps <= 4U * std::bit_width(capacity));
        }
        REQUIRE(queue.full());
        REQUIRE_EQ(queue.initialized_slots(), capacity);
        REQUIRE_EQ(queue.allocated_slots(), allocation);
        const auto deadline = queue.next_deadline();
        for (std::uint32_t offset = 1; offset <= capacity; ++offset) {
            const SequenceRange range {
                SequenceNumber {offset}, SequenceNumber {offset}};
            std::size_t steps = 0;
            REQUIRE(queue.contains(range, &steps));
            REQUIRE(steps <= 2U * std::bit_width(capacity));
            REQUIRE(queue.insert(range, 100'000U));
            REQUIRE_EQ(queue.next_deadline(), deadline);
        }
        REQUIRE(!queue.insert({SequenceNumber {0}, SequenceNumber {0}}, 0));
        for (std::uint32_t offset = 1; offset <= capacity; ++offset) {
            const auto index = queue.first_due();
            REQUIRE_EQ(
                queue.at(index).sequences.first, SequenceNumber {offset});
            queue.erase(index);
        }
        REQUIRE(queue.empty());
        REQUIRE(!queue.next_deadline());
        REQUIRE_EQ(queue.next_to_inspect(), detail::PeerDropQueue::none);
        REQUIRE_EQ(queue.storage_bytes(),
            static_cast<std::size_t>(capacity)
                * (capacity <= 65535U ? 24U : 32U));
        REQUIRE(queue.insert({SequenceNumber {0}, SequenceNumber {0}}, 0));
        REQUIRE_EQ(queue.initialized_slots(), capacity);
        REQUIRE_EQ(queue.allocated_slots(), allocation);
    }
}

TEST(peer_drop_queue_matches_reference_through_reuse_wrap_and_arbitrary_erasure)
{
    for (const std::uint32_t capacity : {1U, 2U, 7U, 64U, 127U}) {
        detail::PeerDropQueue queue(capacity);
        std::map<std::uint64_t, std::uint64_t> expected;
        std::uint32_t random = 0xf734ba32U;
        const auto next = [&] {
            random ^= random << 13U;
            random ^= random >> 17U;
            random ^= random << 5U;
            return random;
        };
        const auto key = [](SequenceRange range) {
            return (static_cast<std::uint64_t>(range.first.value()) << 32U)
                | range.last.value();
        };
        for (unsigned iteration = 0; iteration < 5000; ++iteration) {
            if (!queue.empty() && next() % 3U == 0U) {
                auto index = next() % 2U == 0 ? queue.first_due()
                                              : queue.next_to_inspect();
                while (index == detail::PeerDropQueue::none)
                    index = queue.next_to_inspect();
                REQUIRE(expected.erase(key(queue.at(index).sequences)) == 1U);
                queue.erase(index);
            } else {
                const auto first =
                    SequenceNumber {next() % (capacity * 2U)}.advanced(
                        SequenceNumber::mask - capacity);
                const SequenceRange range {first, first.advanced(next() % 2U)};
                const auto deadline = next() % 16U;
                const bool duplicate = expected.contains(key(range));
                const bool accept = duplicate || expected.size() < capacity;
                std::size_t steps = 0;
                REQUIRE_EQ(queue.insert(range, deadline, &steps), accept);
                REQUIRE(steps <= 4U * std::bit_width(capacity));
                if (accept)
                    expected.try_emplace(key(range), deadline);
            }
            REQUIRE_EQ(queue.size(), expected.size());
            REQUIRE_EQ(queue.empty(), expected.empty());
            if (!expected.empty()) {
                const auto earliest = std::min_element(expected.begin(),
                    expected.end(), [](const auto& a, const auto& b) {
                        return a.second < b.second
                            || (a.second == b.second && a.first < b.first);
                    });
                REQUIRE_EQ(queue.next_deadline(),
                    std::optional<std::uint64_t> {earliest->second});
                REQUIRE_EQ(key(queue.at(queue.first_due()).sequences),
                    earliest->first);
            }
            auto copied = queue;
            REQUIRE(copied.allocated_slots() >= capacity);
            REQUIRE_EQ(copied.size(), queue.size());
            if (!copied.empty())
                copied.erase(copied.first_due());
            REQUIRE_EQ(queue.size(), expected.size());
        }
    }
}

TEST(session_deferred_drop_expiry_yields_after_64_ranges_and_keeps_due_wakeup)
{
    for (const auto initial :
        {SequenceNumber {100}, SequenceNumber {SequenceNumber::mask - 1024U}}) {
        for (const bool message_api : {false, true}) {
            constexpr std::uint32_t capacity = 8192;
            ReliabilitySession receiver {{.peer_initial_sequence = initial,
                .send_capacity_packets = 8,
                .receive_capacity_packets = capacity}};
            receiver.set_message_api(message_api);
            receiver.configure_live({.receive_tsbpd = true,
                                        .too_late_packet_drop = true,
                                        .receive_delay_milliseconds = 100},
                1'000, PacketTimestamp {0});
            // The retained packet is later than these grace deadlines, so the
            // ordinary local playout-gap drop cannot bypass deferred work.
            const std::array payload {std::byte {'p'}};
            PacketView data;
            data.kind = PacketKind::data;
            data.data.sequence = initial.advanced(capacity - 1U);
            data.data.boundary = MessageBoundary::solo;
            data.data.timestamp = PacketTimestamp {10'000};
            data.payload = payload;
            REQUIRE(receiver.receive(data, 1'001));
            std::array<std::byte, 64> storage {};
            for (std::uint32_t offset = 512U; offset > 0; --offset) {
                const auto sequence = initial.advanced(offset - 1U);
                auto packet = encode_and_decode(
                    {.kind = ReliabilityActionKind::drop_request,
                        .drop = {0, {sequence, sequence}}},
                    storage);
                packet.control.timestamp = PacketTimestamp {20};
                REQUIRE(receiver.receive(packet, 1'030));
            }
            REQUIRE_EQ(
                receiver.drop_too_late_receiver(101'019).receiver_drop_packets,
                0U);
            for (std::uint32_t turn = 0; turn < 8U; ++turn) {
                const auto result = receiver.drop_too_late_receiver(101'020);
                REQUIRE(result);
                REQUIRE_EQ(result.receiver_drop_packets, 64U);
                REQUIRE_EQ(receiver.receive_buffer().first_stored_sequence(),
                    initial.advanced((turn + 1U) * 64U));
                const auto deadline = receiver.next_receive_delivery_time();
                REQUIRE_EQ(deadline,
                    std::optional<std::uint64_t> {
                        turn == 7U ? 111'000U : 101'020U});
            }
            REQUIRE_EQ(
                receiver.drop_too_late_receiver(101'020).receiver_drop_packets,
                0U);
            REQUIRE_EQ(receiver.receive_buffer().occupied(), 1U);
        }
    }
}

namespace robotweax::srt::detail {
struct SessionTestAccess {
    static ReliabilityProcessResult expire(ReliabilitySession& session,
        std::uint64_t now, std::size_t& ranges, std::size_t& slots) noexcept
    {
        return session.drop_too_late_receiver_impl(now, &ranges, &slots);
    }
};
}

TEST(session_deferred_drop_refreshes_timestamp_bounds_once_per_bounded_group)
{
    constexpr std::uint32_t capacity = 8192;
    const SequenceNumber initial {100};
    ReliabilitySession receiver {{.peer_initial_sequence = initial,
        .send_capacity_packets = 8,
        .receive_capacity_packets = capacity}};
    receiver.configure_live({.receive_tsbpd = true,
                                .too_late_packet_drop = true,
                                .receive_delay_milliseconds = 100},
        1'000, PacketTimestamp {0});
    const std::array payload {std::byte {'p'}};
    PacketView data;
    data.kind = PacketKind::data;
    data.data.sequence = initial.advanced(capacity - 1U);
    data.data.boundary = MessageBoundary::solo;
    data.data.message_number = 1;
    data.data.timestamp = PacketTimestamp {10'000};
    data.payload = payload;
    REQUIRE(receiver.receive(data, 1'001));
    std::array<std::byte, 64> storage {};
    for (std::uint32_t offset = 0; offset < 64U; ++offset) {
        data.data.sequence = initial.advanced(offset * 2U);
        data.data.boundary = MessageBoundary::subsequent;
        data.data.message_number = 2;
        REQUIRE(receiver.receive(data, 1'010));
        const auto sequence = data.data.sequence;
        auto packet =
            encode_and_decode({.kind = ReliabilityActionKind::drop_request,
                                  .drop = {0, {sequence, sequence}}},
                storage);
        packet.control.timestamp = PacketTimestamp {20};
        REQUIRE(receiver.receive(packet, 1'030));
    }
    std::size_t ranges = 0;
    std::size_t slots = 0;
    const auto expired =
        detail::SessionTestAccess::expire(receiver, 101'020, ranges, slots);
    REQUIRE(expired);
    REQUIRE_EQ(expired.receiver_drop_packets, 64U);
    REQUIRE_EQ(ranges, 64U);
    REQUIRE(slots <= 2U * capacity);
    REQUIRE(slots > capacity / 2U); // One real sparse refresh was measured.
    REQUIRE_EQ(receiver.receive_buffer().occupied(), 1U);
    REQUIRE_EQ(receiver.receive_buffer().buffered_payload_bytes(), 1U);
    REQUIRE_EQ(receiver.receive_buffer().buffered_span_milliseconds(), 1U);
}

TEST(peer_drop_queue_reservation_survives_copy_move_and_node_reuse)
{
    for (const std::uint32_t capacity : {1U, 64U, 65536U}) {
        detail::PeerDropQueue original(capacity);
        const SequenceRange first {SequenceNumber {0}, SequenceNumber {0}};
        REQUIRE(original.insert(first, 1));
        auto copied = original;
        detail::PeerDropQueue assigned(1);
        assigned = original;
        auto moved = std::move(copied);
        REQUIRE(copied.empty());
        REQUIRE_EQ(copied.first_due(), detail::PeerDropQueue::none);
        REQUIRE(!copied.next_deadline());
        copied = original;
        for (auto* queue : {&assigned, &moved, &copied}) {
            const auto reserved = queue->allocated_slots();
            REQUIRE(reserved >= capacity);
            for (std::uint32_t i = 1; i < capacity; ++i) {
                const SequenceRange range {
                    SequenceNumber {i}, SequenceNumber {i}};
                REQUIRE(queue->insert(range, i + 1U));
                REQUIRE_EQ(queue->allocated_slots(), reserved);
            }
            REQUIRE(queue->full());
            while (!queue->empty())
                queue->erase(queue->first_due());
            REQUIRE(queue->insert(first, 1));
            REQUIRE_EQ(queue->allocated_slots(), reserved);
            REQUIRE_EQ(queue->initialized_slots(), capacity);
        }
    }
}
