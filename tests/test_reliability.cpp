#include "test.hpp"

#include "robotweax/srt/reliability.hpp"

#include <array>

using namespace robotweax::srt;

TEST(receive_window_tracks_gap_and_recovery)
{
    ReceiveWindow window{SequenceNumber{100}, 32};
    const auto out_of_order = window.observe(SequenceNumber{102});
    REQUIRE_EQ(out_of_order.status, ReceiveStatus::accepted_out_of_order);
    REQUIRE(out_of_order.gap_before_packet.has_value());
    REQUIRE_EQ(out_of_order.gap_before_packet->first, SequenceNumber{100});
    REQUIRE_EQ(out_of_order.gap_before_packet->last, SequenceNumber{101});

    const auto first = window.observe(SequenceNumber{100});
    REQUIRE_EQ(first.contiguous_advance, 1U);
    REQUIRE_EQ(window.first_expected(), SequenceNumber{101});

    const auto second = window.observe(SequenceNumber{101});
    REQUIRE_EQ(second.contiguous_advance, 2U);
    REQUIRE_EQ(window.first_expected(), SequenceNumber{103});
}

TEST(receive_window_handles_wrap_and_duplicates)
{
    ReceiveWindow window{SequenceNumber{SequenceNumber::mask}, 8};
    REQUIRE_EQ(window.observe(SequenceNumber{0}).status, ReceiveStatus::accepted_out_of_order);
    REQUIRE_EQ(window.observe(SequenceNumber{0}).status, ReceiveStatus::duplicate);
    const auto final = window.observe(SequenceNumber{SequenceNumber::mask});
    REQUIRE_EQ(final.contiguous_advance, 2U);
    REQUIRE_EQ(window.first_expected(), SequenceNumber{1});
}

TEST(receive_window_rejects_packets_outside_bounds)
{
    ReceiveWindow window{SequenceNumber{10}, 4};
    REQUIRE_EQ(window.observe(SequenceNumber{9}).status, ReceiveStatus::older_than_window);
    REQUIRE_EQ(window.observe(SequenceNumber{14}).status, ReceiveStatus::beyond_window);
}

TEST(receive_loss_list_ages_splits_and_handles_sequence_rollover)
{
    ReceiveLossList losses{8};
    REQUIRE(losses.add({
        .first = SequenceNumber{SequenceNumber::mask - 1U},
        .last = SequenceNumber{1},
    }, 2));
    REQUIRE(!losses.has_pending_report());

    const auto first_recovery =
        losses.remove(SequenceNumber{SequenceNumber::mask});
    REQUIRE(first_recovery.removed);
    REQUIRE_EQ(first_recovery.remaining_ttl, 2U);
    REQUIRE_EQ(losses.size(), 2U);

    losses.age_fresh();
    REQUIRE(!losses.has_pending_report());
    const auto second_recovery =
        losses.remove(SequenceNumber{0});
    REQUIRE(second_recovery.removed);
    REQUIRE_EQ(second_recovery.remaining_ttl, 1U);

    losses.age_fresh();
    REQUIRE(!losses.has_pending_report());
    losses.age_fresh();
    REQUIRE(losses.has_pending_report());

    const auto lower = losses.take_pending_report();
    const auto upper = losses.take_pending_report();
    REQUIRE(lower.has_value());
    REQUIRE(upper.has_value());
    REQUIRE_EQ(lower->first,
        SequenceNumber{SequenceNumber::mask - 1U});
    REQUIRE_EQ(lower->last,
        SequenceNumber{SequenceNumber::mask - 1U});
    REQUIRE_EQ(upper->first, SequenceNumber{1});
    REQUIRE_EQ(upper->last, SequenceNumber{1});
    REQUIRE(!losses.take_pending_report().has_value());
}

TEST(receive_loss_list_periodic_reports_and_drop_removal_cover_all_ranges)
{
    ReceiveLossList losses{8};
    REQUIRE(losses.add({
        .first = SequenceNumber{10},
        .last = SequenceNumber{11},
    }, 5));
    REQUIRE(losses.add({
        .first = SequenceNumber{13},
        .last = SequenceNumber{15},
    }, 5));

    losses.mark_periodic_reports();
    const auto first = losses.take_pending_report();
    const auto second = losses.take_pending_report();
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE_EQ(first->first, SequenceNumber{10});
    REQUIRE_EQ(first->last, SequenceNumber{11});
    REQUIRE_EQ(second->first, SequenceNumber{13});
    REQUIRE_EQ(second->last, SequenceNumber{15});

    losses.remove_through(SequenceNumber{14});
    REQUIRE_EQ(losses.size(), 1U);
    losses.mark_periodic_reports();
    const auto remaining = losses.take_pending_report();
    REQUIRE(remaining.has_value());
    REQUIRE_EQ(remaining->first, SequenceNumber{15});
    REQUIRE_EQ(remaining->last, SequenceNumber{15});
}

TEST(receive_loss_list_batches_pending_ranges_without_losing_overflow)
{
    constexpr std::size_t batch_capacity = 8;
    ReceiveLossList losses{batch_capacity + 3U};
    for (std::size_t index = 0;
         index < batch_capacity + 3U; ++index) {
        const auto sequence = static_cast<std::uint32_t>(
            index * 2U + 10U);
        REQUIRE(losses.add({
            .first = SequenceNumber{sequence},
            .last = SequenceNumber{sequence},
        }, 1));
    }

    losses.mark_periodic_reports();
    std::array<SequenceRange, batch_capacity> first_batch{};
    REQUIRE_EQ(
        losses.take_pending_reports(first_batch),
        batch_capacity);
    REQUIRE_EQ(first_batch.front().first,
        SequenceNumber{10});
    REQUIRE_EQ(first_batch.back().first,
        SequenceNumber{24});
    REQUIRE(losses.has_pending_report());

    std::array<SequenceRange, batch_capacity> second_batch{};
    REQUIRE_EQ(
        losses.take_pending_reports(second_batch), 3U);
    REQUIRE_EQ(second_batch[0].first, SequenceNumber{26});
    REQUIRE_EQ(second_batch[2].first, SequenceNumber{30});
    REQUIRE(!losses.has_pending_report());
}

TEST(receive_loss_list_validates_batch_append_without_mutation)
{
    ReceiveLossList losses {3};
    REQUIRE(losses.add(
        {
            .first = SequenceNumber {10},
            .last = SequenceNumber {10},
        },
        0));

    const std::array overlapping {
        SequenceRange {
            .first = SequenceNumber {12},
            .last = SequenceNumber {12},
        },
        SequenceRange {
            .first = SequenceNumber {12},
            .last = SequenceNumber {13},
        },
    };
    REQUIRE(!losses.add_all(overlapping, 0));
    REQUIRE_EQ(losses.size(), 1U);

    const std::array valid {
        SequenceRange {
            .first = SequenceNumber {12},
            .last = SequenceNumber {12},
        },
        SequenceRange {
            .first = SequenceNumber {14},
            .last = SequenceNumber {14},
        },
    };
    REQUIRE(losses.add_all(valid, 0));
    REQUIRE_EQ(losses.size(), 3U);

    const std::array overflow {SequenceRange {
        .first = SequenceNumber {16},
        .last = SequenceNumber {16},
    }};
    REQUIRE(!losses.add_all(overflow, 0));
}

TEST(receive_loss_list_prefix_matches_packet_set_across_all_small_gap_patterns)
{
    for (const auto origin :
        {SequenceNumber {100}, SequenceNumber {SequenceNumber::mask - 3}}) {
        for (unsigned missing = 0; missing < 256; ++missing) {
            for (int cutoff = -1; cutoff <= 8; ++cutoff) {
                ReceiveLossList losses {8};
                for (unsigned index = 0; index < 8;) {
                    if ((missing & (1U << index)) == 0U) {
                        ++index;
                        continue;
                    }
                    const auto first = index;
                    while (
                        index + 1 < 8 && (missing & (1U << (index + 1))) != 0U)
                        ++index;
                    REQUIRE(losses.add(
                        {origin.advanced(first), origin.advanced(index)}, 0));
                    ++index;
                }
                const auto last = origin.advanced(cutoff < 0
                        ? SequenceNumber::mask
                        : static_cast<std::uint32_t>(cutoff));
                const unsigned covered = cutoff < 0
                    ? 0U
                    : (1U << static_cast<unsigned>(cutoff + 1)) - 1U;
                const unsigned expected = missing & ~covered;
                for (unsigned repeat = 0; repeat < 2; ++repeat) {
                    losses.remove_through(last);
                    if (repeat != 0U)
                        losses.mark_periodic_reports();
                    std::array<SequenceRange, 8> reports {};
                    const auto count = losses.take_pending_reports(reports);
                    REQUIRE_EQ(count, losses.size());
                    unsigned observed = 0;
                    for (std::size_t index = 0; index < count; ++index) {
                        const int first =
                            reports[index].first.distance_from(origin);
                        const int end =
                            reports[index].last.distance_from(origin);
                        REQUIRE(first >= 0 && first <= end && end < 8);
                        for (int bit = first; bit <= end; ++bit) {
                            REQUIRE(
                                (observed & (1U << static_cast<unsigned>(bit)))
                                == 0U);
                            observed |= 1U << static_cast<unsigned>(bit);
                        }
                    }
                    REQUIRE_EQ(observed, expected);
                    REQUIRE(!losses.has_pending_report());
                }
            }
        }
    }
}

TEST(receive_loss_list_prefix_preserves_report_flags_ttl_and_split_capacity)
{
    ReceiveLossList losses {4};
    REQUIRE(losses.add({SequenceNumber {10}, SequenceNumber {10}}, 0));
    REQUIRE(losses.add({SequenceNumber {20}, SequenceNumber {24}}, 3));
    losses.mark_periodic_reports();
    REQUIRE(
        losses.take_pending_report().has_value()); // Consume only the prefix.
    REQUIRE(losses.add({SequenceNumber {30}, SequenceNumber {32}}, 0));
    REQUIRE(losses.add({SequenceNumber {40}, SequenceNumber {44}}, 2));
    losses.remove_through(SequenceNumber {21});
    REQUIRE_EQ(losses.size(), 3U);
    std::array<SequenceRange, 4> reports {};
    REQUIRE_EQ(losses.take_pending_reports(reports), 2U);
    REQUIRE_EQ(reports[0].first, SequenceNumber {22});
    REQUIRE_EQ(reports[0].last, SequenceNumber {24});
    REQUIRE_EQ(reports[1].first, SequenceNumber {30});
    REQUIRE_EQ(reports[1].last, SequenceNumber {32});
    REQUIRE(!losses.has_pending_report());
    const auto split = losses.remove(SequenceNumber {23});
    REQUIRE(split.removed);
    REQUIRE_EQ(split.remaining_ttl, 3U);
    REQUIRE_EQ(losses.size(), 4U);
    REQUIRE(!losses.remove(SequenceNumber {31})
            .removed); // Full: split is transactional.
    REQUIRE_EQ(losses.size(), 4U);
    losses.remove_through(SequenceNumber {24});
    const auto recovered = losses.remove(SequenceNumber {31});
    REQUIRE(recovered.removed);
    REQUIRE_EQ(recovered.remaining_ttl, 0U);
    REQUIRE_EQ(losses.size(), 3U);
    losses.remove_through(SequenceNumber {32});
    REQUIRE_EQ(losses.size(), 1U);
    losses.age_fresh();
    const auto fresh = losses.remove(SequenceNumber {40});
    REQUIRE(fresh.removed);
    REQUIRE_EQ(fresh.remaining_ttl, 1U);
    losses.age_fresh();
    REQUIRE(!losses.has_pending_report());
    losses.age_fresh();
    REQUIRE_EQ(losses.take_pending_reports(reports), 1U);
    REQUIRE_EQ(reports[0].first, SequenceNumber {41});
    REQUIRE_EQ(reports[0].last, SequenceNumber {44});
    REQUIRE(!losses.has_pending_report());
}

TEST(
    receive_loss_list_fragmented_prefix_removal_reuses_full_capacity_after_wrap)
{
    constexpr unsigned count = 256;
    const SequenceNumber first {SequenceNumber::mask - 300};
    ReceiveLossList losses {count};
    for (unsigned index = 0; index < count; ++index) {
        const auto sequence = first.advanced(index * 2);
        REQUIRE(losses.add({sequence, sequence}, 0));
    }
    losses.remove_through(
        first.advanced(255)); // Cut in a gap after 128 ranges.
    REQUIRE_EQ(losses.size(), 128U);
    losses.remove_through(first.advanced(510));
    REQUIRE(losses.empty());
    REQUIRE(!losses.has_pending_report());
    losses.remove_through(first.advanced(510));
    for (unsigned index = 0; index < count; ++index) {
        const auto sequence = first.advanced(512 + index * 2);
        REQUIRE(losses.add({sequence, sequence}, 2));
    }
    REQUIRE_EQ(losses.size(), count);
    REQUIRE(!losses.has_pending_report());
    const auto overflow = first.advanced(512 + count * 2);
    REQUIRE(!losses.add({overflow, overflow}, 0));
    losses.mark_periodic_reports();
    std::array<SequenceRange, count> reports {};
    REQUIRE_EQ(losses.take_pending_reports(reports), count);
    REQUIRE_EQ(reports.front().first, first.advanced(512));
    REQUIRE_EQ(reports.back().last, first.advanced(512 + (count - 1) * 2));
}
