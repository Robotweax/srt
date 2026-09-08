// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Robotweax GmbH

#include "test.hpp"
#include "robotweax/srt/session.hpp"
#include <array>
using namespace robotweax::srt;
TEST(tighten_never_extends_and_updates_cached_deadline)
{
    ReceiveLossList l {8};
    REQUIRE(l.add({SequenceNumber {10}, SequenceNumber {10}}, 5, 5000));
    l.tighten_fresh_deadlines(3000, 1000);
    REQUIRE_EQ(l.next_fresh_deadline(), 3000U);
    l.tighten_fresh_deadlines(9000, 2000);
    REQUIRE_EQ(l.next_fresh_deadline(), 3000U);
    l.expire_fresh(2999);
    REQUIRE(!l.take_pending_report());
    l.expire_fresh(3000);
    REQUIRE(l.take_pending_report());
    REQUIRE(!l.take_pending_report());
}
TEST(tighten_split_removal_and_immediate_report)
{
    ReceiveLossList l {8};
    REQUIRE(l.add({SequenceNumber {10}, SequenceNumber {14}}, 5, 5000));
    REQUIRE(l.remove(SequenceNumber {12}).removed);
    REQUIRE(l.remove(SequenceNumber {10}).removed);
    l.tighten_fresh_deadlines(1500, 2000);
    auto a = l.take_pending_report(), b = l.take_pending_report();
    REQUIRE(a);
    REQUIRE(b);
    REQUIRE_EQ(a->first, SequenceNumber {11});
    REQUIRE_EQ(a->last, SequenceNumber {11});
    REQUIRE_EQ(b->first, SequenceNumber {13});
    REQUIRE_EQ(b->last, SequenceNumber {14});
    REQUIRE(!l.take_pending_report());
    REQUIRE_EQ(l.next_fresh_deadline(), 0U);
}
TEST(tighten_zero_time_and_legacy_untimed)
{
    ReceiveLossList l {8};
    REQUIRE(l.add({SequenceNumber {10}, SequenceNumber {10}}, 5, 5000));
    REQUIRE(l.add({SequenceNumber {12}, SequenceNumber {12}}, 5));
    l.tighten_fresh_deadlines(0, 0);
    auto a = l.take_pending_report();
    REQUIRE(a);
    REQUIRE_EQ(a->first, SequenceNumber {10});
    REQUIRE(!l.take_pending_report());
}
TEST(tighten_recovered_range_has_no_report)
{
    ReceiveLossList l {8};
    REQUIRE(l.add({SequenceNumber {10}, SequenceNumber {10}}, 5, 5000));
    REQUIRE(l.remove(SequenceNumber {10}).removed);
    l.tighten_fresh_deadlines(100, 100);
    REQUIRE(!l.take_pending_report());
    REQUIRE_EQ(l.next_fresh_deadline(), 0U);
}
static ReliabilitySession trained(
    std::optional<std::uint64_t> reserve, bool automatic = false)
{
    ReliabilitySession r {{.local_initial_sequence = SequenceNumber {100},
        .peer_initial_sequence = SequenceNumber {10},
        .send_capacity_packets = 32,
        .receive_capacity_packets = 32,
        .experimental_recovery_reserve = reserve,
        .experimental_recovery_budget = automatic}};
    r.configure_live({.periodic_nak = false, .retransmit_flag = true}, 0,
        PacketTimestamp {0});
    SocketOptions o;
    REQUIRE_EQ(
        o.set(SocketOption::maximum_reorder_tolerance_packets, 5), Error::none);
    REQUIRE_EQ(r.apply_dynamic_options(o), Error::none);
    std::array<std::byte, 1> bytes {std::byte {'x'}};
    PacketView p;
    p.kind = PacketKind::data;
    p.payload = bytes;
    p.data.boundary = MessageBoundary::solo;
    for (unsigned s : {15U, 10U, 11U, 12U, 13U, 14U}) {
        p.data.sequence = SequenceNumber {s};
        p.data.message_number = s - 9;
        REQUIRE(r.receive(p, 100));
    }
    std::array<std::byte, 8> out {};
    for (int i = 0; i < 6; ++i)
        REQUIRE(r.pop_message(out));
    r.configure_live({.receive_tsbpd = true,
                         .too_late_packet_drop = true,
                         .periodic_nak = false,
                         .retransmit_flag = true},
        0, PacketTimestamp {0});
    r.enable_tsbpd(0, PacketTimestamp {0}, 20000);
    return r;
}
TEST(tighten_session_fragment_completion_rechecks_existing_gap)
{
    for (bool enabled : {false, true}) {
        auto r = trained(
            enabled ? std::optional<std::uint64_t> {2000} : std::nullopt);
        std::array<std::byte, 1> bytes {std::byte {'x'}};
        PacketView p;
        p.kind = PacketKind::data;
        p.payload = bytes;
        p.data.sequence = SequenceNumber {19};
        p.data.message_number = 10;
        p.data.boundary = MessageBoundary::solo;
        p.data.timestamp = PacketTimestamp {10000};
        REQUIRE(r.receive(p, 17000));
        REQUIRE_EQ(r.next_fresh_deadline(), 19000U);
        p.data.sequence = SequenceNumber {17};
        p.data.message_number = 8;
        p.data.boundary = MessageBoundary::first;
        p.data.timestamp = PacketTimestamp {0};
        REQUIRE(r.receive(p, 17500));
        REQUIRE_EQ(r.next_fresh_deadline(), 19000U);
        p.data.sequence = SequenceNumber {18};
        p.data.boundary = MessageBoundary::last;
        auto completed = r.receive(p, 18000);
        REQUIRE(completed);
        REQUIRE_EQ(*r.next_receive_delivery_time(), 20000U);
        bool nak = false;
        for (std::size_t i = 0; i < completed.actions.size; ++i)
            if (completed.actions.values[i].kind
                == ReliabilityActionKind::loss_report) {
                nak = true;
                auto ranges =
                    completed.actions.loss_ranges(completed.actions.values[i]);
                REQUIRE_EQ(ranges.size(), 1U);
                REQUIRE_EQ(ranges[0].first, SequenceNumber {16});
                REQUIRE_EQ(ranges[0].last, SequenceNumber {16});
            }
        REQUIRE_EQ(nak, enabled);
        REQUIRE_EQ(r.next_fresh_deadline(), enabled ? 0U : 19000U);
    }
}
TEST(tighten_session_unknown_budget_reports_immediately)
{
    auto r = trained(2000);
    std::array<std::byte, 1> bytes {std::byte {'x'}};
    PacketView p;
    p.kind = PacketKind::data;
    p.payload = bytes;
    p.data.sequence = SequenceNumber {17};
    p.data.message_number = 8;
    p.data.boundary = MessageBoundary::first;
    auto gap = r.receive(p, 17000);
    REQUIRE(gap);
    REQUIRE(!r.next_receive_delivery_time());
    bool nak = false;
    for (std::size_t i = 0; i < gap.actions.size; ++i)
        if (gap.actions.values[i].kind == ReliabilityActionKind::loss_report)
            nak = true;
    REQUIRE(nak);
    REQUIRE_EQ(r.next_fresh_deadline(), 0U);
}
TEST(tighten_session_initial_rtt_reserve_does_not_delay_short_latency)
{
    auto r = trained(std::nullopt, true);
    std::array<std::byte, 1> bytes {std::byte {'x'}};
    PacketView p;
    p.kind = PacketKind::data;
    p.payload = bytes;
    p.data.sequence = SequenceNumber {19};
    p.data.message_number = 10;
    p.data.boundary = MessageBoundary::solo;
    p.data.timestamp = PacketTimestamp {10000};
    auto gap = r.receive(p, 17000);
    REQUIRE(gap);
    bool nak = false;
    for (std::size_t i = 0; i < gap.actions.size; ++i)
        if (gap.actions.values[i].kind == ReliabilityActionKind::loss_report)
            nak = true;
    REQUIRE(nak);
    REQUIRE_EQ(r.next_fresh_deadline(), 0U);
}

#include "test.hpp"
#include "robotweax/srt/reliability.hpp"
#include "robotweax/srt/session.hpp"
#include <array>
using namespace robotweax::srt;
TEST(fresh_deadline_exact_boundary_and_once_only)
{
    ReceiveLossList list {8};
    REQUIRE(list.add({SequenceNumber {10}, SequenceNumber {10}}, 5, 2000));
    list.expire_fresh(1999);
    REQUIRE(!list.take_pending_report());
    list.expire_fresh(2000);
    const auto report = list.take_pending_report();
    REQUIRE(report);
    REQUIRE_EQ(report->first, SequenceNumber {10});
    list.expire_fresh(9999);
    REQUIRE(!list.take_pending_report());
}
TEST(fresh_deadline_new_gap_does_not_postpone_old_gap)
{
    ReceiveLossList list {8};
    REQUIRE(list.add({SequenceNumber {10}, SequenceNumber {10}}, 5, 2000));
    REQUIRE(list.add({SequenceNumber {12}, SequenceNumber {12}}, 5, 3000));
    list.expire_fresh(2000);
    const auto report = list.take_pending_report();
    REQUIRE(report);
    REQUIRE_EQ(report->first, SequenceNumber {10});
    REQUIRE(!list.take_pending_report());
    list.expire_fresh(3000);
    const auto later = list.take_pending_report();
    REQUIRE(later);
    REQUIRE_EQ(later->first, SequenceNumber {12});
}
TEST(fresh_deadline_split_preserves_deadline_and_removed_packet_stays_removed)
{
    ReceiveLossList list {8};
    REQUIRE(list.add({SequenceNumber {10}, SequenceNumber {14}}, 5, 2000));
    REQUIRE(list.remove(SequenceNumber {12}).removed);
    list.expire_fresh(2000);
    const auto a = list.take_pending_report();
    const auto b = list.take_pending_report();
    REQUIRE(a);
    REQUIRE(b);
    REQUIRE_EQ(a->last, SequenceNumber {11});
    REQUIRE_EQ(b->first, SequenceNumber {13});
    REQUIRE(!list.take_pending_report());
}
TEST(fresh_deadline_recovery_drop_and_capacity_reuse)
{
    ReceiveLossList list {1};
    REQUIRE(list.add({SequenceNumber {10}, SequenceNumber {10}}, 5, 2000));
    REQUIRE(list.remove(SequenceNumber {10}).removed);
    list.expire_fresh(2000);
    REQUIRE(!list.take_pending_report());
    REQUIRE(list.add({SequenceNumber {20}, SequenceNumber {22}}, 5, 3000));
    list.remove_through(SequenceNumber {22});
    list.expire_fresh(3000);
    REQUIRE(!list.take_pending_report());
    REQUIRE(list.add({SequenceNumber {30}, SequenceNumber {30}}, 5, 5000));
    list.expire_fresh(4000);
    REQUIRE(!list.take_pending_report());
    list.expire_fresh(5000);
    REQUIRE(list.take_pending_report());
}
TEST(fresh_deadline_packet_aging_and_zero_ttl_unchanged)
{
    ReceiveLossList list {8};
    REQUIRE(list.add({SequenceNumber {10}, SequenceNumber {10}}, 1, 2000));
    list.age_fresh();
    REQUIRE(!list.take_pending_report());
    list.age_fresh();
    REQUIRE(list.take_pending_report());
    list.expire_fresh(2000);
    REQUIRE(!list.take_pending_report());
    REQUIRE(list.add({SequenceNumber {12}, SequenceNumber {12}}, 0, 9000));
    REQUIRE(list.take_pending_report());
}
TEST(fresh_deadline_periodic_reporting_remains_independent)
{
    ReceiveLossList list {8};
    REQUIRE(list.add({SequenceNumber {10}, SequenceNumber {10}}, 5, 2000));
    list.mark_periodic_reports();
    REQUIRE(list.take_pending_report());
    list.expire_fresh(2000);
    REQUIRE(list.take_pending_report());
}
TEST(fresh_deadline_disabled_for_legacy_untimed_entries)
{
    ReceiveLossList list {8};
    REQUIRE(list.add({SequenceNumber {10}, SequenceNumber {10}}, 5));
    list.expire_fresh(999999);
    REQUIRE(!list.take_pending_report());
}

#include "test.hpp"
#include "robotweax/srt/reliability.hpp"
using namespace robotweax::srt;
TEST(fresh_deadline_cache_earlier_add_is_not_postponed)
{
    ReceiveLossList list {4};
    REQUIRE(list.add({SequenceNumber {10}, SequenceNumber {10}}, 5, 9000));
    list.expire_fresh(1000);
    REQUIRE(list.add({SequenceNumber {20}, SequenceNumber {20}}, 5, 2000));
    list.expire_fresh(2000);
    auto a = list.take_pending_report();
    REQUIRE(a);
    REQUIRE_EQ(a->first, SequenceNumber {20});
    REQUIRE(!list.take_pending_report());
    list.expire_fresh(9000);
    auto b = list.take_pending_report();
    REQUIRE(b);
    REQUIRE_EQ(b->first, SequenceNumber {10});
}
TEST(fresh_deadline_cache_stale_removed_bound_recomputes)
{
    ReceiveLossList list {4};
    REQUIRE(list.add({SequenceNumber {10}, SequenceNumber {10}}, 5, 1000));
    REQUIRE(list.add({SequenceNumber {20}, SequenceNumber {20}}, 5, 9000));
    REQUIRE(list.remove(SequenceNumber {10}).removed);
    list.expire_fresh(1000);
    REQUIRE(!list.take_pending_report());
    list.expire_fresh(8999);
    REQUIRE(!list.take_pending_report());
    list.expire_fresh(9000);
    REQUIRE(list.take_pending_report());
}
TEST(fresh_deadline_cache_aging_then_reuse)
{
    ReceiveLossList list {4};
    REQUIRE(list.add({SequenceNumber {10}, SequenceNumber {10}}, 1, 1000));
    list.age_fresh();
    list.age_fresh();
    REQUIRE(list.take_pending_report());
    list.expire_fresh(1000);
    REQUIRE(!list.take_pending_report());
    REQUIRE(list.add({SequenceNumber {20}, SequenceNumber {20}}, 1, 2000));
    list.expire_fresh(1999);
    REQUIRE(!list.take_pending_report());
    list.expire_fresh(2000);
    REQUIRE(list.take_pending_report());
}
