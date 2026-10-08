#include "test.hpp"

#include "robotweax/srt/send_buffer.hpp"

#include <algorithm>
#include <array>
#include <vector>
#include <limits>
#include <utility>
#include <cstddef>
#include <bitset>

using namespace robotweax::srt;

TEST(stale_drop_coverage_coalesces_descending_reports_independently_of_window)
{
    for (const auto capacity : {1U, 8192U, 65536U}) {
        SendBuffer buffer {SequenceNumber {100}, capacity, 1};
        detail::RangeDropQueue queue;
        for (std::uint32_t i = 8192U; i != 0U; --i) {
            const std::array ranges {
                SequenceRange {SequenceNumber {i}, SequenceNumber {i}}};
            REQUIRE(buffer.queue_range_drop_requests(ranges));
            std::size_t work = 0;
            REQUIRE(queue.queue(ranges, &work));
            REQUIRE(work <= 5U);
            REQUIRE_EQ(queue.size(), 1U);
        }
        const auto dropped = buffer.next_pending_drop_request();
        REQUIRE(dropped.has_value());
        REQUIRE_EQ(dropped->packets, 8192U);
        REQUIRE_EQ(dropped->sequences.first, SequenceNumber {1});
        REQUIRE_EQ(dropped->sequences.last, SequenceNumber {8192});
        REQUIRE_EQ(dropped->first_message_number, 0U);
        REQUIRE(!buffer.next_pending_drop_request().has_value());
    }
}

TEST(
    stale_drop_coverage_backpressure_is_transactional_and_bridges_free_capacity)
{
    detail::RangeDropQueue queue;
    for (std::uint32_t i = 0; i < detail::RangeDropQueue::capacity; ++i) {
        const auto sequence = SequenceNumber {4U * i};
        const std::array range {SequenceRange {sequence, sequence}};
        REQUIRE(queue.queue(range));
    }
    const std::array extra {
        SequenceRange {SequenceNumber {2000}, SequenceNumber {2000}}};
    REQUIRE(!queue.queue(extra));
    REQUIRE_EQ(queue.size(), detail::RangeDropQueue::capacity);
    // Capacity is checked on the final union, not on a partial insertion.
    const std::array bridge {
        extra[0], SequenceRange {SequenceNumber {1}, SequenceNumber {1999}}};
    REQUIRE(queue.queue(bridge));
    REQUIRE_EQ(queue.size(), 1U);
    const auto merged = queue.next();
    REQUIRE(merged.has_value());
    REQUIRE_EQ(merged->first, SequenceNumber {0});
    REQUIRE_EQ(merged->last, SequenceNumber {2000});
    REQUIRE(queue.empty());

    const std::array valid {
        SequenceRange {SequenceNumber {10}, SequenceNumber {12}}};
    REQUIRE(queue.queue(valid));
    const std::array invalid {
        SequenceRange {SequenceNumber {0}, SequenceNumber {9}},
        SequenceRange {SequenceNumber {20}, SequenceNumber {19}}};
    REQUIRE(!queue.queue(invalid));
    const auto unchanged = queue.next();
    REQUIRE(unchanged.has_value());
    REQUIRE_EQ(unchanged->first, valid[0].first);
    REQUIRE_EQ(unchanged->last, valid[0].last);
    const std::vector<SequenceRange> oversized(
        detail::maximum_range_drop_terms + 1U, valid[0]);
    REQUIRE(!queue.queue(oversized));
    REQUIRE(queue.empty());
}

TEST(
    stale_drop_coverage_wraps_and_splits_ambiguous_unions_without_slot_expansion)
{
    detail::RangeDropQueue queue;
    const std::array wrapping {
        SequenceRange {
            SequenceNumber {SequenceNumber::mask - 5U}, SequenceNumber {2}},
        SequenceRange {
            SequenceNumber {SequenceNumber::mask - 2U}, SequenceNumber {4}},
        SequenceRange {SequenceNumber {3}, SequenceNumber {8}}};
    REQUIRE(queue.queue(wrapping));
    const auto wrap = queue.next();
    REQUIRE(wrap.has_value());
    REQUIRE_EQ(wrap->first, wrapping[0].first);
    REQUIRE_EQ(wrap->last, SequenceNumber {8});
    REQUIRE(queue.empty());

    const std::array whole_wire {
        SequenceRange {SequenceNumber {0},
            SequenceNumber {SequenceNumber::half_range - 1U}},
        SequenceRange {SequenceNumber {SequenceNumber::half_range},
            SequenceNumber {SequenceNumber::mask}}};
    REQUIRE(queue.queue(whole_wire));
    for (const auto range : whole_wire) {
        const auto next = queue.next();
        REQUIRE(next.has_value());
        REQUIRE_EQ(next->first, range.first);
        REQUIRE_EQ(next->last, range.last);
        REQUIRE(next->last.distance_from(next->first) >= 0);
    }
    REQUIRE(queue.empty());
}

TEST(stale_drop_coverage_matches_independent_set_across_reuse_copy_and_rollover)
{
    for (const auto initial :
        {SequenceNumber {100}, SequenceNumber {SequenceNumber::mask - 250U}}) {
        detail::RangeDropQueue queue;
        std::bitset<1024> expected;
        std::uint32_t random = 0x13579bdfU;
        auto step = [&] {
            random = random * 1664525U + 1013904223U;
            return random;
        };
        auto remove = [&](detail::RangeDropQueue& source,
                          std::bitset<1024>& reference) {
            const auto next = source.next();
            REQUIRE_EQ(next.has_value(), reference.any());
            if (!next)
                return;
            const auto length = next->last.distance_from(next->first);
            REQUIRE(length >= 0);
            REQUIRE(length < 1024);
            for (std::uint32_t i = 0; i <= static_cast<std::uint32_t>(length);
                ++i) {
                const auto offset =
                    next->first.advanced(i).distance_from(initial);
                REQUIRE(offset >= 0);
                REQUIRE(offset < 1024);
                REQUIRE(reference.test(static_cast<std::size_t>(offset)));
                reference.reset(static_cast<std::size_t>(offset));
            }
        };
        for (unsigned turn = 0; turn < 12000U; ++turn) {
            if (step() % 4U == 0U)
                remove(queue, expected);
            else {
                std::array<SequenceRange, 5> ranges {};
                for (auto& range : ranges) {
                    const auto offset = step() % 512U;
                    const auto length = step() % 13U;
                    range = {initial.advanced(offset),
                        initial.advanced(offset + length)};
                    for (std::uint32_t i = 0; i <= length; ++i)
                        expected.set(offset + i);
                }
                std::size_t work = 0;
                REQUIRE(queue.queue(ranges, &work));
                REQUIRE(work <= 3U * detail::RangeDropQueue::capacity + 128U);
            }
            if (turn % 101U == 0U) {
                auto copy = queue;
                auto reference = expected;
                auto moved = std::move(copy);
                while (reference.any())
                    remove(moved, reference);
                REQUIRE(moved.empty());
                const std::array range {SequenceRange {initial, initial}};
                REQUIRE(moved.queue(range));
                REQUIRE(moved.next().has_value());
            }
        }
        while (expected.any())
            remove(queue, expected);
        REQUIRE(queue.empty());
    }
}

TEST(stale_drop_coverage_admission_work_is_bounded_at_full_disjoint_capacity)
{
    detail::RangeDropQueue queue;
    for (std::uint32_t i = 0; i < detail::RangeDropQueue::capacity; ++i) {
        const auto sequence = SequenceNumber {i * 4U};
        const std::array range {SequenceRange {sequence, sequence}};
        REQUIRE(queue.queue(range));
    }
    std::array<SequenceRange, maximum_loss_words_per_packet> descending {};
    for (std::size_t i = 0; i < descending.size(); ++i) {
        const auto sequence = SequenceNumber {
            static_cast<std::uint32_t>((descending.size() - i - 1U) * 4U)};
        descending[i] = {sequence, sequence};
    }
    for (unsigned repeat = 0; repeat < 64U; ++repeat) {
        std::size_t work = 0;
        REQUIRE(queue.queue(descending, &work));
        REQUIRE(work <= 64U * maximum_loss_words_per_packet);
        REQUIRE_EQ(queue.size(), detail::RangeDropQueue::capacity);
    }
    while (queue.next().has_value()) {
    }
    REQUIRE(queue.empty());
}

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

TEST(
    send_buffer_retransmission_repeat_clock_starts_at_submission_and_resets_on_reuse)
{
    SendBuffer buffer {SequenceNumber {SequenceNumber::mask}, 1, 1};
    const std::array payload {std::byte {'r'}};
    const auto initial = buffer.first_sequence();
    REQUIRE_EQ(buffer.enqueue_message(payload, 1, PacketTimestamp {0}, 7),
        Error::none);
    REQUIRE(buffer.next_packet());
    std::size_t packets = 0, bytes = 0;
    REQUIRE_EQ(buffer.request_retransmission(
                   {initial, initial}, &packets, &bytes, 0, 100),
        Error::none);
    REQUIRE_EQ(packets, 1U);
    REQUIRE_EQ(bytes, 1U);
    REQUIRE(buffer.next_packet());
    // Zero is a valid send timestamp. Selecting a packet alone did not start
    // the clock; the transport commits its actual successful submission.
    buffer.note_retransmission_sent(initial, 0);
    REQUIRE_EQ(buffer.request_retransmission(
                   {initial, initial}, &packets, &bytes, 99, 100),
        Error::none);
    REQUIRE_EQ(packets, 0U);
    REQUIRE_EQ(bytes, 0U);
    REQUIRE(!buffer.next_packet());
    REQUIRE_EQ(buffer.request_retransmission(
                   {initial, initial}, &packets, &bytes, 100, 100),
        Error::none);
    REQUIRE_EQ(packets, 1U);
    REQUIRE(buffer.next_packet());
    buffer.note_retransmission_sent(
        initial, std::numeric_limits<std::uint64_t>::max());
    REQUIRE_EQ(buffer.request_retransmission(
                   {initial, initial}, &packets, &bytes, 1, 100),
        Error::none);
    REQUIRE_EQ(packets, 0U);
    REQUIRE_EQ(buffer.acknowledge_before(initial.next()), Error::none);
    REQUIRE_EQ(buffer.enqueue_message(payload, 2, PacketTimestamp {0}, 7),
        Error::none);
    REQUIRE(buffer.next_packet());
    REQUIRE_EQ(buffer.request_retransmission(
                   {initial.next(), initial.next()}, &packets, &bytes, 1, 100),
        Error::none);
    REQUIRE_EQ(packets, 1U);
    REQUIRE(buffer.next_packet());
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

TEST(retransmission_ring_compaction_keeps_entries_behind_a_moved_head)
{
    // Ring layout after the steps below: head at physical index 2, logical
    // order [2 (stale), 3, 0, 1]. Compaction must keep 3, 0 and 1 without
    // overwriting entries it has not read yet.
    SendBuffer buffer {SequenceNumber {0}, 4, 1};
    const std::array<std::byte, 1> payload {std::byte {'x'}};
    for (std::uint32_t message = 0; message < 4; ++message) {
        REQUIRE_EQ(
            buffer.enqueue_message(payload, message + 1U, PacketTimestamp {0},
                99, true, 10, message == 2U ? 100U : 0U),
            Error::none);
    }
    while (buffer.next_packet().has_value()) {
    }
    REQUIRE_EQ(
        buffer.request_retransmission({SequenceNumber {0}, SequenceNumber {3}}),
        Error::none);
    // Dequeue two retransmissions: the head moves to index 2.
    for (std::uint32_t expected = 0; expected < 2; ++expected) {
        const auto packet = buffer.next_packet();
        REQUIRE(packet.has_value());
        REQUIRE_EQ(packet->header.sequence, SequenceNumber {expected});
    }
    // Re-request them: they are appended behind the head.
    REQUIRE_EQ(
        buffer.request_retransmission({SequenceNumber {0}, SequenceNumber {1}}),
        Error::none);
    // Message 2 expires: its ring entry becomes stale.
    REQUIRE(buffer.drop_expired_message(101));

    buffer.compact_retransmission_queue_for_testing();
    std::vector<std::uint32_t> retransmitted;
    while (const auto packet = buffer.next_packet()) {
        if (packet->header.retransmitted) {
            retransmitted.push_back(packet->header.sequence.value());
        }
    }
    REQUIRE_EQ(retransmitted, (std::vector<std::uint32_t> {3, 0, 1}));
    REQUIRE(!buffer.has_pending_retransmission());
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

TEST(per_message_expiry_stays_exact_around_the_earliest_expiration_bound)
{
    // The expiry check keeps a lower bound of the earliest TTL so idle polls
    // skip the scan. Acknowledging or dropping the earliest message, or
    // enqueueing a message that expires sooner than the current bound, must
    // not hide an expiration.
    SendBuffer buffer {SequenceNumber {0}, 8, 1};
    const std::array<std::byte, 1> payload {std::byte {'x'}};
    REQUIRE_EQ(buffer.enqueue_message(
                   payload, 1, PacketTimestamp {0}, 99, true, 10, 300),
        Error::none);
    REQUIRE_EQ(buffer.enqueue_message(
                   payload, 2, PacketTimestamp {0}, 99, true, 20, 500),
        Error::none);
    REQUIRE_EQ(buffer.enqueue_message(
                   payload, 3, PacketTimestamp {0}, 99, true, 30, 900),
        Error::none);
    while (buffer.next_packet().has_value()) {
    }
    REQUIRE_EQ(buffer.next_expiration_microseconds(), 300U);
    // Nothing expires before 300; the earliest bound is refined by the scan.
    REQUIRE(!buffer.drop_expired_message(299));
    REQUIRE(!buffer.drop_expired_message(300));
    // The earliest message is acknowledged before it expires: the stale
    // bound must not suppress the next expiration at 500.
    REQUIRE_EQ(buffer.acknowledge_before(SequenceNumber {1}), Error::none);
    REQUIRE_EQ(buffer.next_expiration_microseconds(), 300U);
    REQUIRE(!buffer.drop_expired_message(500));
    REQUIRE_EQ(buffer.next_expiration_microseconds(), 500U);
    const auto first = buffer.drop_expired_message(501);
    REQUIRE(first);
    REQUIRE_EQ(first.first_message_number, 2U);
    // A later enqueue with a sooner TTL than the remaining bound (900).
    REQUIRE_EQ(buffer.enqueue_message(
                   payload, 4, PacketTimestamp {0}, 99, true, 600, 700),
        Error::none);
    while (buffer.next_packet().has_value()) {
    }
    REQUIRE(!buffer.drop_expired_message(700));
    const auto second = buffer.drop_expired_message(701);
    REQUIRE(second);
    REQUIRE_EQ(second.first_message_number, 4U);
    const auto third = buffer.drop_expired_message(901);
    REQUIRE(third);
    REQUIRE_EQ(third.first_message_number, 3U);
    REQUIRE(!buffer.drop_expired_message(100'000));
    REQUIRE(!buffer.next_expiration_microseconds().has_value());
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

TEST(send_buffer_original_order_survives_ack_retransmission_and_ring_wrap)
{
    const SequenceNumber initial {SequenceNumber::mask - 1U};
    SendBuffer buffer {initial, 5, 1};
    const std::array<std::byte, 3> first {
        std::byte {1}, std::byte {2}, std::byte {3}};
    REQUIRE_EQ(
        buffer.enqueue_message(first, 1, PacketTimestamp {7}, 99), Error::none);
    const auto first_packet = buffer.next_packet();
    REQUIRE(first_packet.has_value());
    REQUIRE_EQ(first_packet->header.sequence, initial);
    const auto second_packet = buffer.next_packet();
    REQUIRE(second_packet.has_value());
    REQUIRE_EQ(second_packet->header.sequence, initial.next());
    REQUIRE_EQ(buffer.acknowledge_before(initial.next()), Error::none);
    REQUIRE_EQ(buffer.acknowledge_before(initial), Error::none);
    REQUIRE_EQ(buffer.acknowledge_before(initial.advanced(9)),
        Error::invalid_control_payload);

    const std::array<std::byte, 3> second {
        std::byte {4}, std::byte {5}, std::byte {6}};
    REQUIRE_EQ(buffer.enqueue_message(second, 2, PacketTimestamp {8}, 99),
        Error::none);
    REQUIRE_EQ(buffer.request_retransmission(
                   {.first = initial.next(), .last = initial.next()}),
        Error::none);
    const auto retry = buffer.next_packet();
    REQUIRE(retry.has_value());
    REQUIRE(retry->header.retransmitted);
    REQUIRE_EQ(retry->payload.front(), std::byte {2});
    for (std::uint32_t index = 2; index < 6; ++index) {
        const auto packet = buffer.next_packet();
        REQUIRE(packet.has_value());
        REQUIRE_EQ(packet->header.sequence, initial.advanced(index));
        REQUIRE(!packet->header.retransmitted);
        REQUIRE_EQ(packet->payload.front(), static_cast<std::byte>(index + 1U));
    }
    REQUIRE_EQ(buffer.packets_in_flight(), 5U);
    REQUIRE(!buffer.next_packet().has_value());
    REQUIRE_EQ(buffer.acknowledge_before(initial.advanced(6)), Error::none);
    REQUIRE_EQ(buffer.packets_in_flight(), 0U);
    REQUIRE_EQ(buffer.available(), 5U);
}

TEST(send_buffer_late_drop_keeps_packets_that_were_never_sent)
{
    // Too-late drop abandons only already sent packets. An old message that
    // was never started stays queued and is still sent in order. A message
    // whose first packet was sent is abandoned completely, never in part.
    for (std::size_t sent = 0; sent <= 2; ++sent) {
        SendBuffer buffer {SequenceNumber {100}, 5, 1};
        const std::array<std::byte, 2> old_message {
            std::byte {7}, std::byte {8}};
        const std::array<std::byte, 1> recent_message {std::byte {42}};
        REQUIRE_EQ(buffer.enqueue_message(
                       old_message, 1, PacketTimestamp {1}, 99, true, 10),
            Error::none);
        REQUIRE_EQ(buffer.enqueue_message(
                       recent_message, 2, PacketTimestamp {2}, 99, true, 10),
            Error::none);
        for (std::size_t index = 0; index < sent; ++index) {
            REQUIRE(buffer.next_packet(false, true, 10).has_value());
        }
        const auto dropped = buffer.drop_messages_older_than(10);
        REQUIRE_EQ(dropped.packets, sent == 0U ? 0U : 2U);
        REQUIRE_EQ(
            buffer.first_sequence(), SequenceNumber {sent == 0U ? 100U : 102U});
        const auto packet = buffer.next_packet();
        REQUIRE(packet.has_value());
        REQUIRE_EQ(
            packet->header.sequence, SequenceNumber {sent == 0U ? 100U : 102U});
        REQUIRE_EQ(packet->payload.front(),
            sent == 0U ? std::byte {7} : std::byte {42});
    }
}

TEST(send_buffer_late_drop_rebases_original_selection_before_and_after_cursor)
{
    // Exercise a removed prefix shorter than, equal to, and longer than the
    // already selected prefix. Only sent packets are abandoned.
    for (std::size_t sent = 2; sent <= 3; ++sent) {
        SendBuffer buffer {SequenceNumber {100}, 5, 1};
        const std::array<std::byte, 2> old_message {};
        const std::array<std::byte, 1> recent_message {std::byte {42}};
        REQUIRE_EQ(buffer.enqueue_message(
                       old_message, 1, PacketTimestamp {1}, 99, true, 10),
            Error::none);
        REQUIRE_EQ(buffer.enqueue_message(
                       recent_message, 2, PacketTimestamp {2}, 99, true, 20),
            Error::none);
        for (std::size_t index = 0; index < sent; ++index) {
            // The two old packets are first sent at 10, the recent one at 20.
            REQUIRE(buffer.next_packet(false, true, index < 2U ? 10U : 20U)
                    .has_value());
        }
        REQUIRE_EQ(buffer.drop_messages_older_than(10).packets, 2U);
        REQUIRE_EQ(buffer.first_sequence(), SequenceNumber {102});
        if (sent < 3U) {
            const auto packet = buffer.next_packet();
            REQUIRE(packet.has_value());
            REQUIRE_EQ(packet->header.sequence, SequenceNumber {102});
            REQUIRE_EQ(packet->payload.front(), std::byte {42});
        }
        REQUIRE(!buffer.next_packet().has_value());
        REQUIRE_EQ(buffer.packets_in_flight(), 1U);
        REQUIRE_EQ(buffer.enqueue_message(
                       recent_message, 3, PacketTimestamp {3}, 99, true, 30),
            Error::none);
        const auto appended = buffer.next_packet();
        REQUIRE(appended.has_value());
        REQUIRE_EQ(appended->header.sequence, SequenceNumber {103});
        REQUIRE(!appended->header.retransmitted);
    }
}

TEST(send_buffer_skips_expired_originals_without_losing_later_appends)
{
    const SequenceNumber initial {SequenceNumber::mask};
    SendBuffer buffer {initial, 5, 1};
    const std::array<std::byte, 1> payload {std::byte {7}};
    const std::array<std::byte, 2> expires {};
    REQUIRE_EQ(buffer.enqueue_message(payload, 1, PacketTimestamp {0}, 99),
        Error::none);
    REQUIRE(buffer.next_packet().has_value());
    REQUIRE_EQ(buffer.enqueue_message(
                   expires, 2, PacketTimestamp {0}, 99, true, 1, 10),
        Error::none);
    REQUIRE_EQ(buffer.drop_expired_message(11).packets, 2U);
    REQUIRE(!buffer.next_packet().has_value());
    REQUIRE(!buffer.next_packet().has_value());
    REQUIRE_EQ(buffer.enqueue_message(payload, 3, PacketTimestamp {0}, 99),
        Error::none);
    REQUIRE_EQ(buffer.request_retransmission(
                   {.first = initial, .last = initial.advanced(2)}),
        Error::none);
    const auto retry = buffer.next_packet();
    REQUIRE(retry.has_value());
    REQUIRE(retry->header.retransmitted);
    REQUIRE_EQ(retry->header.sequence, initial);
    REQUIRE(buffer.next_pending_drop_request().has_value());
    const auto appended = buffer.next_packet();
    REQUIRE(appended.has_value());
    REQUIRE_EQ(appended->header.sequence, initial.advanced(3));
    REQUIRE(!appended->header.retransmitted);
    REQUIRE_EQ(buffer.packets_in_flight(), 2U);
    REQUIRE(!buffer.next_packet().has_value());
}

TEST(send_buffer_resumes_after_unsent_ack_full_drop_and_group_resynchronization)
{
    SendBuffer buffer {SequenceNumber {10}, 4, 1};
    const std::array<std::byte, 3> message {};
    REQUIRE_EQ(buffer.enqueue_message(message, 1, PacketTimestamp {0}, 99),
        Error::none);
    REQUIRE(buffer.next_packet().has_value());
    // The buffer-level ACK contract permits releasing queued originals;
    // session-level wire validation is independent of packet selection.
    REQUIRE_EQ(buffer.acknowledge_before(SequenceNumber {12}), Error::none);
    const auto remaining = buffer.next_packet();
    REQUIRE(remaining.has_value());
    REQUIRE_EQ(remaining->header.sequence, SequenceNumber {12});
    REQUIRE_EQ(buffer.drop_messages_older_than(0).packets, 1U);
    REQUIRE(buffer.synchronize_empty(SequenceNumber {SequenceNumber::mask}));
    REQUIRE_EQ(buffer.enqueue_message(message, 2, PacketTimestamp {0}, 99),
        Error::none);
    for (std::uint32_t index = 0; index < 3; ++index) {
        const auto packet = buffer.next_packet();
        REQUIRE(packet.has_value());
        REQUIRE_EQ(packet->header.sequence,
            SequenceNumber {SequenceNumber::mask}.advanced(index));
    }
    REQUIRE(!buffer.next_packet().has_value());
    REQUIRE_EQ(buffer.acknowledge_before(SequenceNumber {2}), Error::none);
    REQUIRE_EQ(buffer.enqueue_stream(message, 3, PacketTimestamp {0}, 99)
                   .bytes_accepted,
        message.size());
    for (std::uint32_t index = 0; index < 3; ++index) {
        const auto packet = buffer.next_packet();
        REQUIRE(packet.has_value());
        REQUIRE_EQ(packet->header.sequence, SequenceNumber {2}.advanced(index));
    }
    REQUIRE(!buffer.next_packet().has_value());
}

TEST(send_buffer_retains_packet_tracks_tombstones_ack_and_recycled_slots)
{
    const SequenceNumber initial {SequenceNumber::mask};
    SendBuffer buffer {initial, 3, 1};
    const std::array payload {std::byte {7}};
    REQUIRE(!buffer.retains_packet(initial));
    REQUIRE_EQ(buffer.enqueue_message(payload, 1, PacketTimestamp {0}, 99),
        Error::none);
    REQUIRE_EQ(buffer.enqueue_message(
                   payload, 2, PacketTimestamp {0}, 99, true, 1, 10),
        Error::none);
    REQUIRE_EQ(buffer.enqueue_message(payload, 3, PacketTimestamp {0}, 99),
        Error::none);
    REQUIRE(buffer.retains_packet(initial));
    REQUIRE(buffer.retains_packet(initial.next()));
    REQUIRE_EQ(buffer.drop_expired_message(11).packets, 1U);
    REQUIRE(!buffer.retains_packet(initial.next()));
    REQUIRE(buffer.retains_packet(initial.advanced(2)));
    REQUIRE(!buffer.retains_packet(initial.advanced(3)));
    REQUIRE_EQ(buffer.acknowledge_before(initial.advanced(2)), Error::none);
    REQUIRE(!buffer.retains_packet(initial));
    REQUIRE_EQ(buffer.enqueue_message(payload, 4, PacketTimestamp {0}, 99),
        Error::none);
    REQUIRE(buffer.retains_packet(initial.advanced(3)));
    REQUIRE(!buffer.retains_packet(initial));
}

TEST(send_buffer_payload_reuse_keeps_full_capacity_and_independent_copies)
{
    const SequenceNumber initial {SequenceNumber::mask - 3U};
    SendBuffer buffer {initial, 8};
    std::array<std::byte, maximum_data_payload_size> payload {};
    const std::byte* reused = nullptr;
    for (unsigned round = 0; round < 40; ++round) {
        payload.fill(static_cast<std::byte>(round));
        const auto bytes =
            std::span {payload}.first(round % 2 ? 1 : payload.size());
        REQUIRE_EQ(buffer.enqueue_message(bytes, round, PacketTimestamp {}, 1),
            Error::none);
        const auto packet = buffer.next_packet();
        REQUIRE(packet);
        if (reused)
            REQUIRE_EQ(packet->payload.data(), reused);
        reused = packet->payload.data();
        REQUIRE_EQ(packet->payload.size(), bytes.size());
        REQUIRE(
            std::equal(bytes.begin(), bytes.end(), packet->payload.begin()));
        REQUIRE_EQ(
            buffer.acknowledge_before(buffer.next_sequence()), Error::none);
    }
    for (unsigned index = 0; index < 8; ++index) {
        payload.fill(static_cast<std::byte>(index));
        REQUIRE_EQ(
            buffer.enqueue_message(payload, index, PacketTimestamp {}, 1),
            Error::none);
    }
    REQUIRE_EQ(buffer.available(), 0U);
    SendBuffer copy = buffer;
    SendBuffer assigned {SequenceNumber {}, 1};
    assigned = buffer;
    SendBuffer moved = std::move(copy);
    REQUIRE_EQ(buffer.acknowledge_before(buffer.next_sequence()), Error::none);
    payload.fill(std::byte {0xee});
    REQUIRE_EQ(buffer.enqueue_message(payload, 100, PacketTimestamp {}, 1),
        Error::none);
    for (auto* independent : {&assigned, &moved}) {
        for (unsigned index = 0; index < 8; ++index) {
            const auto packet = independent->next_packet();
            REQUIRE(packet);
            REQUIRE_EQ(packet->payload.size(), payload.size());
            REQUIRE(std::all_of(packet->payload.begin(), packet->payload.end(),
                [index](std::byte byte) {
                    return byte == static_cast<std::byte>(index);
                }));
        }
    }
}

TEST(send_buffer_payload_tombstones_do_not_own_reused_ciphertext)
{
    SendBuffer buffer {SequenceNumber {100}, 4, 4};
    const std::array<std::byte, 4> clear {};
    const std::array<std::byte, 4> cipher {
        std::byte {1}, std::byte {2}, std::byte {3}, std::byte {4}};
    REQUIRE_EQ(
        buffer.enqueue_message(clear, 1, PacketTimestamp {}, 1, true, 1, 10),
        Error::none);
    const auto first = buffer.next_packet();
    REQUIRE(first);
    const auto* address = first->payload.data();
    REQUIRE(buffer.drop_expired_message(11));
    REQUIRE_EQ(
        buffer.enqueue_message(clear, 2, PacketTimestamp {}, 1), Error::none);
    const auto second = buffer.next_packet();
    REQUIRE(second);
    REQUIRE_EQ(second->payload.data(), address);
    REQUIRE_EQ(buffer.preserve_encrypted_payload(
                   second->header.sequence, EncryptionKey::even, cipher),
        Error::none);
    // Retiring the old sequence tombstone must not release the new owner.
    REQUIRE_EQ(buffer.acknowledge_before(second->header.sequence), Error::none);
    SendBuffer copy = buffer;
    REQUIRE_EQ(buffer.acknowledge_before(buffer.next_sequence()), Error::none);
    REQUIRE_EQ(copy.request_retransmission(
                   {second->header.sequence, second->header.sequence}),
        Error::none);
    const auto retransmit = copy.next_packet();
    REQUIRE(retransmit);
    REQUIRE_EQ(retransmit->header.encryption_key, EncryptionKey::even);
    REQUIRE(
        std::equal(cipher.begin(), cipher.end(), retransmit->payload.begin()));
}

TEST(send_buffer_invalid_dimensions_fail_before_reserving_payload)
{
    for (const auto capacity :
        {std::size_t {0}, std::size_t {SequenceNumber::half_range},
            std::numeric_limits<std::size_t>::max()}) {
        bool rejected = false;
        try {
            SendBuffer buffer {SequenceNumber {}, capacity};
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        REQUIRE(rejected);
    }
}

TEST(send_buffer_payload_pool_preserves_maximum_gcm_tag_across_copy_and_reuse)
{
    SendBuffer buffer {SequenceNumber {SequenceNumber::mask}, 2,
        maximum_data_payload_size - srt_gcm_authentication_tag_size};
    std::array<std::byte, maximum_data_payload_size> wire {};
    for (std::size_t i = 0; i < wire.size(); ++i)
        wire[i] = static_cast<std::byte>(i);
    for (unsigned iteration = 0; iteration < 6; ++iteration) {
        const auto seq = buffer.next_sequence();
        REQUIRE_EQ(
            buffer.enqueue_message(std::span {wire}.first(wire.size()
                                       - srt_gcm_authentication_tag_size),
                1, PacketTimestamp {}, 1),
            Error::none);
        REQUIRE(buffer.next_packet());
        REQUIRE_EQ(buffer.preserve_protected_payload(
                       seq, EncryptionKey::odd, CryptoMode::aes_gcm, wire),
            Error::none);
        SendBuffer copy = buffer;
        REQUIRE_EQ(buffer.acknowledge_before(seq.next()), Error::none);
        REQUIRE_EQ(copy.request_retransmission({seq, seq}), Error::none);
        const auto retransmit = copy.next_packet();
        REQUIRE(retransmit);
        REQUIRE_EQ(retransmit->payload.size(), wire.size());
        REQUIRE(
            std::equal(wire.begin(), wire.end(), retransmit->payload.begin()));
    }
}

TEST(send_buffer_positions_survive_expiration_and_wire_wrap)
{
    SendBuffer buffer {SequenceNumber {SequenceNumber::mask}, 8, 1};
    const std::array one {std::byte {1}};
    const std::array three {std::byte {2}, std::byte {3}, std::byte {4}};
    REQUIRE_EQ(
        buffer.enqueue_message(one, 1, PacketTimestamp {0}, 99), Error::none);
    REQUIRE_EQ(
        buffer.enqueue_message(three, 2, PacketTimestamp {0}, 99, true, 0, 10),
        Error::none);
    REQUIRE_EQ(
        buffer.enqueue_message(one, 3, PacketTimestamp {0}, 99), Error::none);
    REQUIRE_EQ(buffer.next_packet()->sequence_position, 0U);
    REQUIRE_EQ(buffer.drop_expired_message(11).packets, 3U);
    const auto peek = buffer.peek_new_packet();
    REQUIRE(peek.has_value());
    REQUIRE_EQ(peek->sequence_position, 4U);
    REQUIRE_EQ(peek->header.sequence, SequenceNumber {3});
    REQUIRE_EQ(buffer.peek_new_packet()->sequence_position, 4U);
    REQUIRE_EQ(buffer.next_packet()->sequence_position, 4U);
}

TEST(send_buffer_positions_keep_complete_cycles_during_empty_resynchronization)
{
    SendBuffer buffer {SequenceNumber {0}, 8, 1};
    REQUIRE(
        buffer.synchronize_empty(SequenceNumber {SequenceNumber::half_range}));
    REQUIRE(buffer.synchronize_empty(SequenceNumber {0}));
    const std::array one {std::byte {1}};
    REQUIRE_EQ(
        buffer.enqueue_message(one, 1, PacketTimestamp {0}, 99), Error::none);
    REQUIRE_EQ(buffer.peek_new_packet()->sequence_position,
        std::uint64_t {SequenceNumber::modulus});
}

TEST(send_buffer_late_drop_retires_later_sent_fragments_of_the_same_message)
{
    for (const auto sent : {2U, 3U}) {
        SendBuffer buffer {SequenceNumber {SequenceNumber::mask - 1U}, 8, 1};
        const std::array<std::byte, 3> message {};
        const std::array<std::byte, 1> following {std::byte {42}};
        REQUIRE_EQ(buffer.enqueue_message(
                       message, 1, PacketTimestamp {1}, 99, true, 1),
            Error::none);
        REQUIRE_EQ(buffer.enqueue_message(
                       following, 2, PacketTimestamp {2}, 99, true, 1),
            Error::none);
        for (unsigned i = 0; i < sent; ++i)
            REQUIRE(buffer.next_packet(false, true, 10 + i * 10).has_value());
        const auto dropped = buffer.drop_messages_older_than(10);
        REQUIRE_EQ(dropped.packets, 3U);
        REQUIRE_EQ(dropped.sequences.last, SequenceNumber {0});
        REQUIRE_EQ(buffer.size(), 1U);
        const auto next = buffer.next_packet(false, true, 40);
        REQUIRE(next.has_value());
        REQUIRE_EQ(next->header.message_number, 2U);
        REQUIRE_EQ(next->payload[0], std::byte {42});
    }
}

TEST(send_buffer_validates_dropped_unsent_tail_across_ring_and_sequence_wrap)
{
    const SequenceNumber initial {SequenceNumber::mask - 7U};
    SendBuffer buffer {initial, 8, 1};
    const std::array<std::byte, 6> prefix {};
    REQUIRE_EQ(
        buffer.enqueue_message(prefix, 1, PacketTimestamp {0}, 9), Error::none);
    for (unsigned i = 0; i < 6; ++i)
        REQUIRE(buffer.next_packet());
    REQUIRE_EQ(buffer.acknowledge_before(initial.advanced(6)), Error::none);
    const auto first = buffer.first_sequence();
    const std::array<std::byte, 4> expired {};
    const std::array<std::byte, 2> waiting {};
    REQUIRE_EQ(buffer.enqueue_message(
                   expired, 2, PacketTimestamp {0}, 9, true, 1, 100),
        Error::none);
    REQUIRE_EQ(buffer.enqueue_message(waiting, 3, PacketTimestamp {0}, 9),
        Error::none);
    REQUIRE_EQ(buffer.enqueue_message(
                   waiting, 4, PacketTimestamp {0}, 9, true, 1, 100),
        Error::none);
    REQUIRE_EQ(buffer.drop_expired_message(101).packets, 4U);
    REQUIRE_EQ(buffer.drop_expired_message(101).packets, 2U);
    REQUIRE_EQ(buffer.validate_retransmission_range({first, first.advanced(3)}),
        Error::none);
    REQUIRE_EQ(buffer.validate_retransmission_range(
                   {first.advanced(6), first.advanced(7)}),
        Error::none);
    REQUIRE_EQ(buffer.validate_retransmission_range({first, first.advanced(7)}),
        Error::invalid_control_payload);
    REQUIRE_EQ(buffer.acknowledge_before(first.advanced(2)), Error::none);
    REQUIRE_EQ(
        buffer.request_retransmission({first.advanced(3), first.advanced(3)}),
        Error::none);
    const auto drop = buffer.next_pending_drop_request();
    REQUIRE(drop);
    REQUIRE_EQ(drop->sequences.first, first.advanced(2));
    REQUIRE(buffer.next_packet());
    REQUIRE(buffer.next_packet());
    REQUIRE_EQ(buffer.validate_retransmission_range(
                   {first.advanced(2), first.advanced(7)}),
        Error::none);
}
