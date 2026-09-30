#include "test.hpp"

#include "robotweax/srt/reliability.hpp"

#include <array>
#include <iterator>
#include <set>

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

TEST(receive_loss_list_remove_range_keeps_losses_outside_the_range)
{
    ReceiveLossList losses {8};
    REQUIRE(losses.add({SequenceNumber {10}, SequenceNumber {12}}, 1));
    REQUIRE(losses.add({SequenceNumber {20}, SequenceNumber {25}}, 1));
    REQUIRE(losses.add({SequenceNumber {30}, SequenceNumber {30}}, 1));

    // A range covering only the middle entry leaves the others intact.
    REQUIRE(losses.remove_range({SequenceNumber {20}, SequenceNumber {25}}));
    REQUIRE_EQ(losses.size(), 2U);

    // Trim the head and tail of entries the range partially overlaps.
    REQUIRE(losses.add({SequenceNumber {40}, SequenceNumber {45}}, 1));
    REQUIRE(losses.remove_range({SequenceNumber {12}, SequenceNumber {30}}));
    losses.mark_periodic_reports();
    std::array<SequenceRange, 8> reports {};
    REQUIRE_EQ(losses.take_pending_reports(reports), 2U);
    REQUIRE_EQ(reports[0].first, SequenceNumber {10});
    REQUIRE_EQ(reports[0].last, SequenceNumber {11});
    REQUIRE_EQ(reports[1].first, SequenceNumber {40});
    REQUIRE_EQ(reports[1].last, SequenceNumber {45});

    // A range strictly inside an entry splits it.
    REQUIRE(losses.remove_range({SequenceNumber {42}, SequenceNumber {43}}));
    losses.mark_periodic_reports();
    REQUIRE_EQ(losses.take_pending_reports(reports), 3U);
    REQUIRE_EQ(reports[1].first, SequenceNumber {40});
    REQUIRE_EQ(reports[1].last, SequenceNumber {41});
    REQUIRE_EQ(reports[2].first, SequenceNumber {44});
    REQUIRE_EQ(reports[2].last, SequenceNumber {45});

    // A range across the sequence rollover is handled like any other.
    ReceiveLossList wrapped {4};
    REQUIRE(wrapped.add(
        {SequenceNumber {SequenceNumber::mask - 1U}, SequenceNumber {1}}, 1));
    REQUIRE(wrapped.remove_range(
        {SequenceNumber {SequenceNumber::mask}, SequenceNumber {0}}));
    wrapped.mark_periodic_reports();
    REQUIRE_EQ(wrapped.take_pending_reports(reports), 2U);
    REQUIRE_EQ(reports[0].first, SequenceNumber {SequenceNumber::mask - 1U});
    REQUIRE_EQ(reports[0].last, SequenceNumber {SequenceNumber::mask - 1U});
    REQUIRE_EQ(reports[1].first, SequenceNumber {1});
    REQUIRE_EQ(reports[1].last, SequenceNumber {1});
}

TEST(receive_loss_list_pending_and_membership_stay_consistent_under_churn)
{
    // The list keeps counters so per-packet paths skip full walks. A random
    // walk over every mutating operation checks that has_pending_report()
    // always agrees with what take_pending_reports() yields and that
    // remove() agrees with a plain set of lost sequences.
    std::uint32_t state = 0x1234'5678U;
    const auto next_random = [&state]() {
        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        return state;
    };
    ReceiveLossList losses {64};
    std::set<std::uint32_t> model;
    std::uint32_t highest = 100;
    std::array<SequenceRange, 64> reports {};
    for (unsigned step = 0; step < 4'000; ++step) {
        switch (next_random() % 8U) {
        case 0:
        case 1: {
            // A new gap behind the highest sequence, fresh or not.
            const std::uint32_t gap = 1U + next_random() % 4U;
            const SequenceRange range {
                SequenceNumber {highest + 1U}, SequenceNumber {highest + gap}};
            const std::uint32_t ttl = next_random() % 3U;
            if (losses.add(range, ttl)) {
                for (std::uint32_t seq = highest + 1U; seq <= highest + gap;
                    ++seq) {
                    model.insert(seq);
                }
            }
            highest += gap + 1U;
            break;
        }
        case 2:
        case 3: {
            if (model.empty()) {
                break;
            }
            // Remove a lost sequence (hit) or a random one (mostly a miss).
            std::uint32_t target = 0;
            if (next_random() % 2U == 0U) {
                auto it = model.begin();
                std::advance(it, next_random() % model.size());
                target = *it;
            } else {
                target = highest - next_random() % 40U;
            }
            const auto removal = losses.remove(SequenceNumber {target});
            REQUIRE_EQ(removal.removed, model.erase(target) == 1U);
            break;
        }
        case 4:
            losses.age_fresh();
            break;
        case 5:
            losses.mark_periodic_reports();
            REQUIRE_EQ(losses.has_pending_report(), !model.empty());
            break;
        case 6: {
            const std::uint32_t through = highest - next_random() % 40U;
            losses.remove_through(SequenceNumber {through});
            for (auto it = model.begin(); it != model.end();) {
                it = *it <= through ? model.erase(it) : std::next(it);
            }
            break;
        }
        case 7: {
            const std::uint32_t first = highest - next_random() % 40U;
            const std::uint32_t last = first + next_random() % 6U;
            REQUIRE(losses.remove_range(
                {SequenceNumber {first}, SequenceNumber {last}}));
            for (auto it = model.begin(); it != model.end();) {
                it = (*it >= first && *it <= last) ? model.erase(it)
                                                   : std::next(it);
            }
            break;
        }
        }
        REQUIRE_EQ(losses.empty(), model.empty());
        const bool pending = losses.has_pending_report();
        const std::size_t taken = losses.take_pending_reports(reports);
        REQUIRE_EQ(pending, taken != 0U);
        REQUIRE(!losses.has_pending_report());
        for (std::size_t index = 0; index < taken; ++index) {
            for (std::uint32_t seq = reports[index].first.value();
                seq <= reports[index].last.value(); ++seq) {
                REQUIRE(model.contains(seq));
            }
        }
    }
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
    REQUIRE(losses.remove(SequenceNumber {31}).removed);
    REQUIRE_EQ(losses.size(), 5U);
    losses.remove_through(SequenceNumber {24});
    const auto recovered = losses.remove(SequenceNumber {30});
    REQUIRE(recovered.removed);
    REQUIRE_EQ(recovered.remaining_ttl, 0U);
    REQUIRE_EQ(losses.size(), 2U);
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

TEST(receive_loss_list_split_reserve_reports_overflow_without_losing_losses)
{
    ReceiveLossList losses {1};
    REQUIRE(losses.add({SequenceNumber {10}, SequenceNumber {20}}, 0));
    REQUIRE(losses.remove(SequenceNumber {12}).removed);
    REQUIRE_EQ(losses.size(), 2U);
    REQUIRE(!losses.contains(SequenceNumber {12}));
    REQUIRE(losses.contains(SequenceNumber {15}));

    const auto exhausted = losses.remove(SequenceNumber {15});
    REQUIRE(!exhausted.removed);
    REQUIRE(exhausted.capacity_exhausted);
    REQUIRE(losses.contains(SequenceNumber {15}));
    REQUIRE(!losses.remove_range({SequenceNumber {16}, SequenceNumber {17}}));
    REQUIRE(losses.contains(SequenceNumber {16}));
    REQUIRE(losses.contains(SequenceNumber {17}));
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

TEST(receive_loss_list_sorted_insert_rejects_partial_overlaps)
{
    const std::array overlapping {
        SequenceRange {SequenceNumber {105}, SequenceNumber {120}},
        SequenceRange {SequenceNumber {90}, SequenceNumber {105}},
        SequenceRange {SequenceNumber {90}, SequenceNumber {120}},
        SequenceRange {SequenceNumber {100}, SequenceNumber {120}},
    };
    for (const auto& range : overlapping) {
        ReceiveLossList losses {4};
        REQUIRE(losses.insert_sorted(
            {SequenceNumber {100}, SequenceNumber {110}}, 2));
        REQUIRE(!losses.insert_sorted(range, 0));
        REQUIRE_EQ(losses.size(), 1U);
        REQUIRE(!losses.has_pending_report());
        losses.age_fresh();
        losses.age_fresh();
        REQUIRE(!losses.has_pending_report());
        losses.age_fresh();
        const auto report = losses.take_pending_report();
        REQUIRE(report.has_value());
        REQUIRE_EQ(report->first, SequenceNumber {100});
        REQUIRE_EQ(report->last, SequenceNumber {110});
        REQUIRE(!losses.take_pending_report().has_value());
    }
}

TEST(receive_loss_list_sorted_insert_accepts_covered_ranges_at_capacity)
{
    ReceiveLossList losses {1};
    REQUIRE(
        losses.insert_sorted({SequenceNumber {100}, SequenceNumber {110}}, 0));
    REQUIRE(
        losses.insert_sorted({SequenceNumber {100}, SequenceNumber {110}}, 5));
    REQUIRE(
        losses.insert_sorted({SequenceNumber {103}, SequenceNumber {107}}, 5));
    REQUIRE(
        losses.insert_sorted({SequenceNumber {100}, SequenceNumber {105}}, 5));
    REQUIRE_EQ(losses.size(), 1U);
    const auto report = losses.take_pending_report();
    REQUIRE(report.has_value());
    REQUIRE_EQ(report->first, SequenceNumber {100});
    REQUIRE_EQ(report->last, SequenceNumber {110});
    REQUIRE(
        !losses.insert_sorted({SequenceNumber {120}, SequenceNumber {120}}, 0));
}

TEST(receive_loss_list_sorted_batch_orders_ranges_across_rollover)
{
    ReceiveLossList losses {4};
    const auto mask = SequenceNumber::mask;
    REQUIRE(losses.insert_sorted(
        {SequenceNumber {mask - 3U}, SequenceNumber {mask - 2U}}, 0));
    const std::array batch {
        SequenceRange {SequenceNumber {3}, SequenceNumber {4}},
        SequenceRange {SequenceNumber {mask - 6U}, SequenceNumber {mask - 5U}},
        SequenceRange {SequenceNumber {mask}, SequenceNumber {1}},
    };
    REQUIRE(losses.can_insert_all_sorted(batch));
    REQUIRE_EQ(losses.size(), 1U);
    for (const auto& range : batch) {
        REQUIRE(losses.insert_sorted(range, 0));
    }
    REQUIRE_EQ(losses.size(), 4U);
    const std::array expected {
        batch[1],
        SequenceRange {SequenceNumber {mask - 3U}, SequenceNumber {mask - 2U}},
        batch[2],
        batch[0],
    };
    for (const auto& range : expected) {
        const auto report = losses.take_pending_report();
        REQUIRE(report.has_value());
        REQUIRE_EQ(report->first, range.first);
        REQUIRE_EQ(report->last, range.last);
    }
    REQUIRE(!losses.take_pending_report().has_value());
    REQUIRE(!losses.insert_sorted({SequenceNumber {0}, SequenceNumber {2}}, 0));
    const std::array overflow {
        SequenceRange {SequenceNumber {6}, SequenceNumber {6}},
    };
    REQUIRE(!losses.can_insert_all_sorted(overflow));
    REQUIRE_EQ(losses.size(), 4U);
}

TEST(receive_loss_list_sorted_batch_rejects_overlap_without_mutation)
{
    ReceiveLossList losses {4};
    REQUIRE(
        losses.insert_sorted({SequenceNumber {100}, SequenceNumber {110}}, 0));
    const std::array existing_overlap {
        SequenceRange {SequenceNumber {130}, SequenceNumber {140}},
        SequenceRange {SequenceNumber {105}, SequenceNumber {115}},
    };
    REQUIRE(!losses.can_insert_all_sorted(existing_overlap));
    const std::array batch_overlap {
        SequenceRange {SequenceNumber {130}, SequenceNumber {140}},
        SequenceRange {SequenceNumber {125}, SequenceNumber {135}},
    };
    REQUIRE(!losses.can_insert_all_sorted(batch_overlap));
    REQUIRE_EQ(losses.size(), 1U);
    const auto report = losses.take_pending_report();
    REQUIRE(report.has_value());
    REQUIRE_EQ(report->first, SequenceNumber {100});
    REQUIRE_EQ(report->last, SequenceNumber {110});
    REQUIRE(!losses.take_pending_report().has_value());
}

TEST(
    receive_loss_list_keeps_retry_times_when_ranges_split_and_reports_are_bounded)
{
    ReceiveLossList losses {8};
    REQUIRE(losses.add(
        {SequenceNumber {SequenceNumber::mask - 1U}, SequenceNumber {2}}, 0));
    std::array<SequenceRange, 1> report {};
    REQUIRE_EQ(losses.take_pending_reports(report, 0), 1U);
    REQUIRE(losses.remove(SequenceNumber {0}).removed);
    losses.mark_periodic_reports(49'999, 50'000);
    REQUIRE(!losses.has_pending_report());
    losses.mark_periodic_reports(50'000, 50'000);
    REQUIRE_EQ(losses.take_pending_reports(report, 50'000), 1U);
    REQUIRE_EQ(report[0].last, SequenceNumber {SequenceNumber::mask});
    // Marking again must neither inflate the pending count nor delay the
    // second range that did not fit in the previous report's output storage.
    losses.mark_periodic_reports(60'000, 50'000);
    REQUIRE_EQ(losses.take_pending_reports(report, 60'000), 1U);
    REQUIRE_EQ(report[0].first, SequenceNumber {1});
    REQUIRE(!losses.has_pending_report());
    losses.mark_periodic_reports(100'000, 50'000);
    REQUIRE_EQ(losses.take_pending_reports(report, 100'000), 1U);
    REQUIRE_EQ(report[0].last, SequenceNumber {SequenceNumber::mask});
    REQUIRE(!losses.has_pending_report());
    REQUIRE(losses.add({SequenceNumber {5}, SequenceNumber {5}}, 0));
    REQUIRE_EQ(losses.take_pending_reports(report, 100'001), 1U);
    REQUIRE_EQ(report[0].first, SequenceNumber {5});
}
