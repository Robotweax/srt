#include "test.hpp"

#include "robotweax/srt/send_buffer.hpp"

#include <array>
#include <cstddef>

using namespace robotweax::srt;

TEST(send_buffer_fragments_message_without_per_packet_allocation)
{
    SendBuffer buffer{SequenceNumber{100}, 8, 4};
    const std::array<std::byte, 10> message{
        std::byte{0}, std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4},
        std::byte{5}, std::byte{6}, std::byte{7}, std::byte{8}, std::byte{9},
    };
    REQUIRE_EQ(buffer.enqueue_message(
        message, 7, PacketTimestamp{55}, 900), Error::none);
    REQUIRE_EQ(buffer.size(), 3U);

    const auto first = buffer.next_packet();
    REQUIRE(first.has_value());
    REQUIRE_EQ(first->header.sequence, SequenceNumber{100});
    REQUIRE_EQ(first->header.boundary, MessageBoundary::first);
    REQUIRE_EQ(first->payload.size(), 4U);
    const auto middle = buffer.next_packet();
    REQUIRE(middle.has_value());
    REQUIRE_EQ(middle->header.boundary, MessageBoundary::subsequent);
    const auto last = buffer.next_packet();
    REQUIRE(last.has_value());
    REQUIRE_EQ(last->header.sequence, SequenceNumber{102});
    REQUIRE_EQ(last->header.boundary, MessageBoundary::last);
    REQUIRE_EQ(last->payload.size(), 2U);
    REQUIRE(!buffer.next_packet().has_value());
}

TEST(send_buffer_synchronizes_only_an_empty_group_member)
{
    SendBuffer buffer{SequenceNumber{10}, 8, 4};
    REQUIRE(buffer.synchronize_empty(SequenceNumber{100}));
    REQUIRE_EQ(buffer.next_sequence(), SequenceNumber{100});

    const std::array<std::byte, 3> payload{};
    REQUIRE_EQ(buffer.enqueue_message(payload, 1,
                   PacketTimestamp{0}, 7),
        Error::none);
    REQUIRE(!buffer.synchronize_empty(SequenceNumber{200}));
    REQUIRE(buffer.synchronize_empty(SequenceNumber{101}));
}

TEST(retransmissions_are_scheduled_before_new_packets)
{
    SendBuffer buffer{SequenceNumber{10}, 8, 2};
    const std::array<std::byte, 6> message{};
    REQUIRE_EQ(buffer.enqueue_message(
        message, 1, PacketTimestamp{0}, 99), Error::none);
    REQUIRE(buffer.next_packet().has_value());
    REQUIRE(buffer.next_packet().has_value());
    REQUIRE_EQ(buffer.request_retransmission({
        .first = SequenceNumber{10}, .last = SequenceNumber{10}}), Error::none);

    const auto retransmission = buffer.next_packet();
    REQUIRE(retransmission.has_value());
    REQUIRE_EQ(retransmission->header.sequence, SequenceNumber{10});
    REQUIRE(retransmission->header.retransmitted);
    const auto new_packet = buffer.next_packet();
    REQUIRE(new_packet.has_value());
    REQUIRE_EQ(new_packet->header.sequence, SequenceNumber{12});
    REQUIRE(!new_packet->header.retransmitted);
}

TEST(send_buffer_preserves_encrypted_wire_payload_for_retransmission)
{
    SendBuffer buffer{SequenceNumber{20}, 4, 4};
    const std::array clear{
        std::byte{'s'}, std::byte{'r'},
        std::byte{'t'}, std::byte{'!'}};
    const std::array ciphertext{
        std::byte{0x10}, std::byte{0x20},
        std::byte{0x30}, std::byte{0x40}};
    REQUIRE_EQ(buffer.enqueue_message(
        clear, 1, PacketTimestamp{0}, 99), Error::none);
    const auto original = buffer.next_packet();
    REQUIRE(original.has_value());

    REQUIRE_EQ(buffer.preserve_encrypted_payload(
                   SequenceNumber{20}, EncryptionKey::even,
                   ciphertext),
        Error::none);
    REQUIRE_EQ(buffer.request_retransmission({
        .first = SequenceNumber{20},
        .last = SequenceNumber{20}}), Error::none);
    const auto retransmission = buffer.next_packet();
    REQUIRE(retransmission.has_value());
    REQUIRE(retransmission->header.retransmitted);
    REQUIRE_EQ(retransmission->header.encryption_key,
        EncryptionKey::even);
    REQUIRE(std::equal(ciphertext.begin(), ciphertext.end(),
        retransmission->payload.begin()));
}

TEST(send_buffer_preserves_complete_gcm_wire_payload_immutably)
{
    SendBuffer buffer {SequenceNumber {30}, 4, 4};
    const std::array clear {
        std::byte {'a'}, std::byte {'e'}, std::byte {'a'}, std::byte {'d'}};
    std::array<std::byte, clear.size() + srt_gcm_authentication_tag_size>
        protected_payload {};
    for (std::size_t index = 0; index < protected_payload.size(); ++index) {
        protected_payload[index] = static_cast<std::byte>(0x40U + index);
    }
    REQUIRE_EQ(
        buffer.enqueue_message(clear, 1, PacketTimestamp {0}, 99), Error::none);
    REQUIRE(buffer.next_packet().has_value());

    REQUIRE_EQ(
        buffer.preserve_protected_payload(SequenceNumber {30},
            EncryptionKey::odd, CryptoMode::aes_gcm,
            std::span {protected_payload}.first(protected_payload.size() - 1U)),
        Error::invalid_payload_size);
    REQUIRE_EQ(buffer.preserve_protected_payload(SequenceNumber {30},
                   EncryptionKey::odd, CryptoMode::aes_gcm, protected_payload),
        Error::none);
    REQUIRE_EQ(buffer.buffered_payload_bytes(), clear.size());
    REQUIRE_EQ(buffer.preserve_protected_payload(SequenceNumber {30},
                   EncryptionKey::odd, CryptoMode::aes_gcm, protected_payload),
        Error::none);

    auto changed_tag = protected_payload;
    changed_tag.back() ^= std::byte {1};
    REQUIRE_EQ(buffer.preserve_protected_payload(SequenceNumber {30},
                   EncryptionKey::odd, CryptoMode::aes_gcm, changed_tag),
        Error::invalid_state);
    REQUIRE_EQ(buffer.preserve_protected_payload(SequenceNumber {30},
                   EncryptionKey::odd, CryptoMode::aes_ctr,
                   std::span {protected_payload}.first(clear.size())),
        Error::invalid_state);

    std::size_t retransmission_packets = 0;
    std::size_t retransmission_bytes = 0;
    REQUIRE_EQ(buffer.request_retransmission(
                   {.first = SequenceNumber {30}, .last = SequenceNumber {30}},
                   &retransmission_packets, &retransmission_bytes),
        Error::none);
    REQUIRE_EQ(retransmission_packets, 1U);
    REQUIRE_EQ(retransmission_bytes, clear.size());
    const auto retransmission = buffer.next_packet();
    REQUIRE(retransmission.has_value());
    REQUIRE(retransmission->header.retransmitted);
    REQUIRE_EQ(retransmission->header.encryption_key, EncryptionKey::odd);
    REQUIRE_EQ(retransmission->payload.size(), protected_payload.size());
    REQUIRE(std::equal(protected_payload.begin(), protected_payload.end(),
        retransmission->payload.begin()));
}

TEST(send_buffer_rejects_gcm_that_exceeds_the_protected_wire_ceiling)
{
    SendBuffer buffer {SequenceNumber {40}, 2, maximum_data_payload_size};
    std::array<std::byte, maximum_data_payload_size> plaintext {};
    std::array<std::byte,
        maximum_data_payload_size + srt_gcm_authentication_tag_size>
        oversized {};
    REQUIRE_EQ(buffer.enqueue_message(plaintext, 1, PacketTimestamp {0}, 99),
        Error::none);
    REQUIRE(buffer.next_packet().has_value());
    REQUIRE_EQ(buffer.preserve_protected_payload(SequenceNumber {40},
                   EncryptionKey::even, CryptoMode::aes_gcm, oversized),
        Error::buffer_too_small);
}

TEST(rejected_retransmission_range_does_not_queue_a_partial_prefix)
{
    SendBuffer buffer {SequenceNumber {10}, 8, 1};
    const std::array<std::byte, 4> message {};
    REQUIRE_EQ(buffer.enqueue_message(message, 1, PacketTimestamp {0}, 99),
        Error::none);
    REQUIRE(buffer.next_packet().has_value());
    REQUIRE(buffer.next_packet().has_value());
    REQUIRE(buffer.next_packet().has_value());

    std::size_t queued_packets = 99U;
    std::size_t queued_bytes = 99U;
    REQUIRE_EQ(buffer.request_retransmission(
                   {.first = SequenceNumber {10}, .last = SequenceNumber {13}},
                   &queued_packets, &queued_bytes),
        Error::invalid_control_payload);
    REQUIRE_EQ(queued_packets, 0U);
    REQUIRE_EQ(queued_bytes, 0U);
    REQUIRE(!buffer.has_pending_retransmission());

    const auto unsent = buffer.next_packet();
    REQUIRE(unsent.has_value());
    REQUIRE_EQ(unsent->header.sequence, SequenceNumber {13});
    REQUIRE(!unsent->header.retransmitted);
}

TEST(retransmission_range_validation_accepts_sequence_rollover)
{
    SendBuffer buffer {SequenceNumber {SequenceNumber::mask - 1U}, 4, 1};
    const std::array<std::byte, 4> message {};
    REQUIRE_EQ(buffer.enqueue_message(message, 1, PacketTimestamp {0}, 99),
        Error::none);
    for (std::size_t index = 0; index < message.size(); ++index) {
        REQUIRE(buffer.next_packet().has_value());
    }

    REQUIRE_EQ(buffer.request_retransmission(
                   {.first = SequenceNumber {SequenceNumber::mask},
                       .last = SequenceNumber {0}}),
        Error::none);
    for (const auto expected :
        {SequenceNumber {SequenceNumber::mask}, SequenceNumber {0}}) {
        const auto retransmission = buffer.next_packet();
        REQUIRE(retransmission.has_value());
        REQUIRE_EQ(retransmission->header.sequence, expected);
        REQUIRE(retransmission->header.retransmitted);
    }
}

TEST(stale_drop_range_queue_rejects_invalid_batches_transactionally)
{
    SendBuffer buffer {SequenceNumber {0}, 1, 1};
    const std::array<SequenceRange, 2> ranges {{
        {
            .first = SequenceNumber {10},
            .last = SequenceNumber {10},
        },
        {
            .first = SequenceNumber {20},
            .last = SequenceNumber {19},
        },
    }};

    REQUIRE(!buffer.queue_range_drop_requests(ranges));
    REQUIRE(!buffer.has_pending_drop_request());
    REQUIRE(!buffer.next_pending_drop_request().has_value());
}

TEST(timeout_retransmission_queues_every_sent_packet_once_across_rollover)
{
    SendBuffer buffer{
        SequenceNumber{SequenceNumber::mask - 1U}, 4, 1};
    const std::array<std::byte, 4> message{};
    REQUIRE_EQ(buffer.enqueue_message(
        message, 1, PacketTimestamp{0}, 99), Error::none);
    REQUIRE(buffer.next_packet().has_value());
    REQUIRE(buffer.next_packet().has_value());
    REQUIRE(buffer.next_packet().has_value());

    REQUIRE_EQ(buffer.request_retransmission_of_all_sent(), 3U);
    REQUIRE_EQ(buffer.request_retransmission_of_all_sent(), 0U);
    const auto first = buffer.next_packet();
    const auto second = buffer.next_packet();
    const auto third = buffer.next_packet();
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

    // The fourth packet has never been sent and must not be mistaken for loss.
    const auto original = buffer.next_packet();
    REQUIRE(original.has_value());
    REQUIRE_EQ(original->header.sequence, SequenceNumber{1});
    REQUIRE(!original->header.retransmitted);
}

TEST(acknowledgement_releases_ring_capacity_across_wrap)
{
    SendBuffer buffer{SequenceNumber{SequenceNumber::mask - 1U}, 4, 1};
    const std::array<std::byte, 4> message{};
    REQUIRE_EQ(buffer.enqueue_message(
        message, 1, PacketTimestamp{0}, 99), Error::none);
    REQUIRE_EQ(buffer.next_sequence(), SequenceNumber{2});
    REQUIRE_EQ(buffer.available(), 0U);
    REQUIRE_EQ(buffer.buffered_payload_bytes(), 4U);
    REQUIRE_EQ(buffer.acknowledge_before(SequenceNumber{1}), Error::none);
    REQUIRE_EQ(buffer.first_sequence(), SequenceNumber{1});
    REQUIRE_EQ(buffer.size(), 1U);
    REQUIRE_EQ(buffer.available(), 3U);
    REQUIRE_EQ(buffer.buffered_payload_bytes(), 1U);
    REQUIRE_EQ(buffer.acknowledge_before(SequenceNumber {2}), Error::none);
    REQUIRE_EQ(buffer.buffered_payload_bytes(), 0U);
}

TEST(send_buffer_rejects_ack_beyond_queued_data)
{
    SendBuffer buffer{SequenceNumber{20}, 4, 4};
    const std::array<std::byte, 4> message{};
    REQUIRE_EQ(buffer.enqueue_message(
        message, 1, PacketTimestamp{0}, 99), Error::none);
    REQUIRE_EQ(buffer.acknowledge_before(SequenceNumber{22}),
        Error::invalid_control_payload);
}

TEST(send_buffer_ignores_duplicate_older_ack)
{
    SendBuffer buffer{SequenceNumber{40}, 4, 4};
    const std::array<std::byte, 4> message{};
    REQUIRE_EQ(buffer.enqueue_message(
        message, 1, PacketTimestamp{0}, 99), Error::none);
    REQUIRE_EQ(buffer.acknowledge_before(SequenceNumber{41}), Error::none);
    REQUIRE_EQ(buffer.acknowledge_before(SequenceNumber{40}), Error::none);
    REQUIRE_EQ(buffer.size(), 0U);
}

TEST(stream_enqueue_accepts_the_available_packet_capacity_partially)
{
    SendBuffer buffer{SequenceNumber{100}, 2, 4};
    const std::array<std::byte, 10> bytes{};
    const auto queued = buffer.enqueue_stream(
        bytes, 1, PacketTimestamp{0}, 99);
    REQUIRE(queued);
    REQUIRE_EQ(queued.bytes_accepted, 8U);
    REQUIRE_EQ(buffer.available(), 0U);
    REQUIRE_EQ(buffer.enqueue_stream(
                   std::span{bytes}.last(2),
                   2, PacketTimestamp{0}, 99)
                   .error,
        Error::buffer_too_small);
}

TEST(per_message_expiry_drops_an_interior_message_across_sequence_rollover)
{
    SendBuffer buffer{
        SequenceNumber{SequenceNumber::mask}, 4, 1};
    const std::array<std::byte, 1> durable{
        std::byte{'a'}};
    const std::array<std::byte, 2> expiring{
        std::byte{'b'}, std::byte{'c'}};
    REQUIRE_EQ(buffer.enqueue_message(
        durable, 1, PacketTimestamp{0}, 99, true,
        10, 0), Error::none);
    REQUIRE_EQ(buffer.enqueue_message(
        expiring, 2, PacketTimestamp{0}, 99, true,
        20, 100), Error::none);
    REQUIRE_EQ(buffer.size(), 3U);
    REQUIRE_EQ(buffer.next_sequence(), SequenceNumber{2});

    REQUIRE(!buffer.drop_expired_message(100));
    const auto dropped = buffer.drop_expired_message(101);
    REQUIRE(dropped);
    REQUIRE_EQ(dropped.first_message_number, 2U);
    REQUIRE_EQ(dropped.packets, 2U);
    REQUIRE_EQ(dropped.bytes, 2U);
    REQUIRE_EQ(dropped.sequences.first, SequenceNumber{0});
    REQUIRE_EQ(dropped.sequences.last, SequenceNumber{1});
    REQUIRE_EQ(buffer.size(), 1U);

    const auto packet = buffer.next_packet();
    REQUIRE(packet.has_value());
    REQUIRE_EQ(packet->header.sequence,
        SequenceNumber{SequenceNumber::mask});
    REQUIRE(!buffer.next_packet().has_value());

    REQUIRE_EQ(buffer.acknowledge_before(SequenceNumber{2}),
        Error::none);
    REQUIRE_EQ(buffer.first_sequence(), SequenceNumber{2});
    REQUIRE_EQ(buffer.next_sequence(), SequenceNumber{2});
    REQUIRE_EQ(buffer.size(), 0U);
    REQUIRE_EQ(buffer.available(), buffer.capacity());
}

TEST(send_buffer_statistics_exclude_drop_tombstones_and_measure_span)
{
    SendBuffer buffer{SequenceNumber{50}, 4, 2};
    const std::array<std::byte, 2> expiring{
        std::byte{'o'}, std::byte{'l'}};
    const std::array<std::byte, 1> durable{
        std::byte{'n'}};
    REQUIRE_EQ(buffer.enqueue_message(
                   expiring, 1, PacketTimestamp{1}, 99,
                   true, 1'000, 3'000),
        Error::none);
    REQUIRE_EQ(buffer.enqueue_message(
                   durable, 2, PacketTimestamp{2}, 99,
                   true, 3'500, 0),
        Error::none);
    REQUIRE_EQ(buffer.buffered_payload_bytes(), 3U);
    REQUIRE_EQ(buffer.buffered_span_milliseconds(), 3U);

    REQUIRE(buffer.drop_expired_message(3'001));
    REQUIRE_EQ(buffer.sequence_span(), 2U);
    REQUIRE_EQ(buffer.size(), 1U);
    REQUIRE_EQ(buffer.buffered_payload_bytes(), 1U);
    REQUIRE_EQ(buffer.buffered_span_milliseconds(), 1U);
}

TEST(send_buffer_statistics_update_span_after_prefix_acknowledgement)
{
    SendBuffer buffer {SequenceNumber {70}, 4, 2};
    const std::array<std::byte, 1> payload {std::byte {'x'}};
    REQUIRE_EQ(buffer.enqueue_message(
                   payload, 1, PacketTimestamp {1}, 99, true, 1'000),
        Error::none);
    REQUIRE_EQ(buffer.enqueue_message(
                   payload, 2, PacketTimestamp {2}, 99, true, 3'500),
        Error::none);
    REQUIRE_EQ(buffer.enqueue_message(
                   payload, 3, PacketTimestamp {3}, 99, true, 6'000),
        Error::none);
    REQUIRE_EQ(buffer.buffered_span_milliseconds(), 6U);

    REQUIRE_EQ(buffer.acknowledge_before(SequenceNumber {71}), Error::none);
    REQUIRE_EQ(buffer.buffered_payload_bytes(), 2U);
    REQUIRE_EQ(buffer.buffered_span_milliseconds(), 3U);
}

TEST(send_buffer_tail_probe_skips_unsent_packets_and_empty_buffer)
{
    SendBuffer buffer {SequenceNumber {100}, 8, 1};
    REQUIRE(!buffer.request_retransmission_of_last_sent());
    const std::array<std::byte, 3> input {};
    REQUIRE_EQ(buffer.enqueue_message(input, 1, PacketTimestamp {0}, 900),
        Error::none);
    REQUIRE(!buffer.request_retransmission_of_last_sent());
    REQUIRE(buffer.next_packet().has_value());
    REQUIRE(buffer.next_packet().has_value());
    REQUIRE(buffer.request_retransmission_of_last_sent());
    REQUIRE(buffer.request_retransmission_of_last_sent());
    const auto probe = buffer.next_packet();
    REQUIRE(probe.has_value());
    REQUIRE_EQ(probe->header.sequence, SequenceNumber {101});
    REQUIRE(probe->header.retransmitted);
    const auto unsent = buffer.next_packet();
    REQUIRE(unsent.has_value());
    REQUIRE_EQ(unsent->header.sequence, SequenceNumber {102});
    REQUIRE(!unsent->header.retransmitted);
    REQUIRE(!buffer.next_packet().has_value());
}

TEST(send_buffer_tail_probe_skips_expired_tail_across_sequence_wrap)
{
    SendBuffer buffer {SequenceNumber {SequenceNumber::mask}, 4, 1};
    const std::array<std::byte, 1> input {};
    REQUIRE_EQ(
        buffer.enqueue_message(input, 1, PacketTimestamp {0}, 900, true, 0, 0),
        Error::none);
    REQUIRE_EQ(buffer.enqueue_message(
                   input, 2, PacketTimestamp {0}, 900, true, 0, 100),
        Error::none);
    REQUIRE(buffer.next_packet().has_value());
    REQUIRE(buffer.next_packet().has_value());
    REQUIRE(buffer.drop_expired_message(101));
    REQUIRE(buffer.request_retransmission_of_last_sent());
    const auto probe = buffer.next_packet();
    REQUIRE(probe.has_value());
    REQUIRE_EQ(probe->header.sequence, SequenceNumber {SequenceNumber::mask});
    REQUIRE(probe->header.retransmitted);
}
