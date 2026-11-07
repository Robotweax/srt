// SPDX-License-Identifier: MIT
#include "test.hpp"
#include "compat/hybrid_progress.hpp"
#include <limits>

using namespace robotweax::srt;
namespace hp = robotweax::srt::compat::hybrid;
namespace {
constexpr std::array<std::byte, 3> data {
    std::byte {'a'}, std::byte {'b'}, std::byte {'c'}};
PacketView packet(std::uint32_t sequence)
{
    return {.kind = PacketKind::data,
        .data = {.sequence = SequenceNumber {sequence},
            .message_number = 1,
            .boundary = MessageBoundary::solo,
            .timestamp = PacketTimestamp {100},
            .destination_socket_id = 0x40000001},
        .payload = data};
}
}
TEST(hybrid_progress_high_water_does_not_freshen_on_duplicate_or_old_positions)
{
    hp::DataHighWater p;
    REQUIRE(p.note(5, 100));
    REQUIRE_EQ(p.next, 6U);
    REQUIRE(p.note(5, 200));
    REQUIRE(p.note(1, 300));
    REQUIRE_EQ(p.next, 6U);
    REQUIRE_EQ(p.last_advance_us, 100U);
    REQUIRE_EQ(p.packets, 3U);
    REQUIRE(p.note(9, 90));
    REQUIRE_EQ(p.next, 10U);
    REQUIRE_EQ(p.last_advance_us, 90U);
    REQUIRE(!p.note(UINT64_MAX, 500));
    REQUIRE_EQ(p.next, 10U);
    p.packets = UINT64_MAX;
    REQUIRE(p.note(10, 600));
    REQUIRE_EQ(p.packets, UINT64_MAX);
}
TEST(
    hybrid_progress_late_other_path_copy_after_pop_is_validated_and_counted_once)
{
    hp::ReceiveProgress p(SequenceNumber {10});
    auto a = packet(10);
    p.receive(a, true, hp::DataPath::unicast, 100);
    p.sync_floor(SequenceNumber {11});
    a.data.retransmitted = true;
    p.receive(a, false, hp::DataPath::multicast, 200);
    auto s = p.snapshot(1000);
    REQUIRE(s);
    REQUIRE_EQ(s->all.next, 1001U);
    REQUIRE_EQ(s->unicast.next, 1001U);
    REQUIRE_EQ(s->multicast.next, 1001U);
    REQUIRE_EQ(s->unique_packets, 1U);
    REQUIRE_EQ(s->matched_copies, 1U);
    REQUIRE_EQ(s->window_floor, 1001U);
    p.receive(a, false, hp::DataPath::multicast, 300);
    s = p.snapshot(1000);
    REQUIRE_EQ(s->multicast.last_advance_us, 200U);
    REQUIRE_EQ(s->multicast.packets, 1U);
}
TEST(hybrid_progress_changed_payload_header_and_uncached_packets_do_not_count)
{
    hp::ReceiveProgress p(SequenceNumber {0});
    auto a = packet(0);
    p.receive(a, true, hp::DataPath::unicast, 100);
    std::array<std::byte, 3> changed = data;
    changed[0] ^= std::byte {1};
    a.payload = changed;
    p.receive(a, false, hp::DataPath::multicast, 200);
    a = packet(0);
    a.data.message_number = 2;
    p.receive(a, false, hp::DataPath::multicast, 201);
    a = packet(0);
    a.data.timestamp = PacketTimestamp {101};
    p.receive(a, false, hp::DataPath::multicast, 202);
    a = packet(4);
    p.receive(a, false, hp::DataPath::multicast, 203);
    const auto s = p.snapshot();
    REQUIRE(s);
    REQUIRE(!s->multicast.observed);
    REQUIRE_EQ(s->ignored_copies, 4U);
    REQUIRE_EQ(s->unique_packets, 1U);
}
TEST(hybrid_progress_window_skips_and_gaps_never_manufacture_data_progress)
{
    hp::ReceiveProgress p(SequenceNumber {0});
    p.sync_floor(SequenceNumber {8});
    auto s = p.snapshot();
    REQUIRE(s);
    REQUIRE_EQ(s->window_floor, 8U);
    REQUIRE(!s->all.observed);
    auto a = packet(15);
    p.receive(a, true, hp::DataPath::multicast, 100);
    s = p.snapshot();
    REQUIRE_EQ(s->all.next, 16U);
    REQUIRE_EQ(s->multicast.packets, 1U);
    p.sync_floor(SequenceNumber {16});
    s = p.snapshot();
    REQUIRE_EQ(s->all.last_advance_us, 100U);
    REQUIRE_EQ(s->unique_packets, 1U);
}
TEST(hybrid_progress_recent_cache_is_bounded_by_positions_even_with_reordering)
{
    hp::ReceiveProgress p(SequenceNumber {0});
    p.receive(packet(1), true, hp::DataPath::unicast, 100);
    p.receive(packet(100), true, hp::DataPath::unicast, 101);
    p.receive(packet(1), false, hp::DataPath::multicast, 200);
    REQUIRE(!p.snapshot()->multicast.observed);
    hp::ReceiveProgress q(SequenceNumber {0});
    q.receive(packet(34), true, hp::DataPath::unicast, 100);
    q.receive(packet(2), true, hp::DataPath::unicast, 101);
    q.receive(packet(34), false, hp::DataPath::multicast, 200);
    REQUIRE_EQ(q.snapshot()->multicast.next, 35U);
    for (std::uint32_t i = 101; i < 140; ++i)
        p.receive(packet(i), true, hp::DataPath::unicast, 300 + i);
    p.receive(packet(100), false, hp::DataPath::multicast, 1000);
    REQUIRE(!p.snapshot()->multicast.observed);
}
TEST(hybrid_progress_sequence_lifting_crosses_wraps_without_inventing_data)
{
    const SequenceNumber origin {SequenceNumber::mask - 2};
    hp::ReceiveProgress p(origin);
    p.receive(packet(origin.value()), true, hp::DataPath::unicast, 100);
    auto floor = origin.advanced(4);
    p.sync_floor(floor);
    std::uint64_t distance = 4;
    for (unsigned i = 0; i < 5; ++i) {
        floor = floor.advanced(SequenceNumber::half_range - 1);
        p.sync_floor(floor);
        distance += SequenceNumber::half_range - 1;
    }
    p.receive(
        packet(floor.advanced(5).value()), true, hp::DataPath::multicast, 200);
    const auto s = p.snapshot(1234);
    REQUIRE(s);
    REQUIRE_EQ(s->all.next, 1234 + distance + 6);
    REQUIRE_EQ(s->unique_packets, 2U);
    p.sync_floor(floor.advanced(SequenceNumber::mask));
    REQUIRE(!p.snapshot());
}
TEST(hybrid_progress_invalid_inputs_and_absolute_overflow_are_not_observations)
{
    hp::ReceiveProgress p(SequenceNumber {0});
    auto a = packet(0);
    a.data.encryption_key = EncryptionKey::even;
    p.receive(a, true, hp::DataPath::multicast, 100);
    a = packet(0);
    a.data.message_number = 0;
    p.receive(a, true, hp::DataPath::multicast, 100);
    a = packet(0);
    a.kind = PacketKind::control;
    p.receive(a, true, hp::DataPath::multicast, 100);
    REQUIRE(!p.snapshot()->all.observed);
    p.receive(packet(1), true, hp::DataPath::unicast, 100);
    REQUIRE(!p.snapshot(UINT64_MAX - 1));
}

TEST(
    hybrid_rejoin_coverage_invalidation_preserves_uc_history_and_requires_new_mc_copies)
{
    hp::ReceiveProgress p(SequenceNumber {10});
    auto a = packet(10), b = packet(11);
    p.receive(a, true, hp::DataPath::unicast, 100);
    p.receive(a, false, hp::DataPath::multicast, 110);
    p.receive(b, true, hp::DataPath::multicast, 120);
    REQUIRE(p.coverage(1000, 1002, 1000));
    const auto before = p.snapshot(1000);
    REQUIRE(before);
    p.invalidate_multicast_coverage();
    REQUIRE(!p.coverage(1000, 1002, 1000));
    const auto after = p.snapshot(1000);
    REQUIRE(after);
    REQUIRE_EQ(after->all.next, before->all.next);
    REQUIRE_EQ(
        after->multicast.last_advance_us, before->multicast.last_advance_us);
    REQUIRE_EQ(after->unicast.next, before->unicast.next);
    REQUIRE_EQ(after->unique_packets, before->unique_packets);
    p.receive(a, false, hp::DataPath::multicast, 200);
    REQUIRE(!p.coverage(1000, 1002, 1000));
    p.receive(b, false, hp::DataPath::multicast, 210);
    const auto fresh = p.coverage(1000, 1002, 1000);
    REQUIRE(fresh);
    REQUIRE_EQ(fresh->first_at_us, 200U);
    REQUIRE_EQ(fresh->last_at_us, 210U);
    REQUIRE_EQ(p.snapshot(1000)->multicast.last_advance_us,
        before->multicast.last_advance_us);
}
