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
