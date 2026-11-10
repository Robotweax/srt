// SPDX-License-Identifier: MIT
#include "test.hpp"
#include "hybrid_fault_link.hpp"
using namespace robotweax::srt;
using Queue = test::HybridFaultQueue;
namespace {
constexpr std::array<std::byte, 1> a {std::byte {1}}, b {std::byte {2}};
}
TEST(hybrid_link_delay_reorder_duplicate_preserves_bytes_and_endpoint)
{
    Queue q;
    const auto peer = IpEndpoint::loopback(7000);
    REQUIRE_EQ(
        q.enqueue(a, peer, 100, {10, 20, 0, 2}), Queue::Result::accepted);
    REQUIRE_EQ(
        q.enqueue(b, peer, 100, {10, 20, 0, 2}), Queue::Result::accepted);
    REQUIRE(!q.take(109));
    auto p = q.take(110);
    REQUIRE(p);
    REQUIRE_EQ(p->bytes[0], b[0]);
    REQUIRE_EQ(p->peer, peer);
    p = q.take(111);
    REQUIRE(p);
    REQUIRE_EQ(p->bytes[0], b[0]);
    p = q.take(130);
    REQUIRE(p);
    REQUIRE_EQ(p->bytes[0], a[0]);
    REQUIRE(!q.take(1000));
    REQUIRE_EQ(q.snapshot().duplicates, 1U);
}
TEST(hybrid_link_burst_loss_and_retry_never_enqueue_rejected_attempts)
{
    Queue q;
    Queue::Rules r {0, 0, 3, 0, 2, 1};
    auto peer = IpEndpoint::loopback(1);
    REQUIRE_EQ(q.enqueue(a, peer, 100, r),
        Queue::Result::accepted); // first burst loss
    REQUIRE_EQ(q.enqueue(a, peer, 100, r), Queue::Result::retry);
    REQUIRE_EQ(q.enqueue(b, peer, 100, r), Queue::Result::accepted);
    REQUIRE_EQ(q.enqueue(a, peer, 100, r), Queue::Result::retry);
    REQUIRE_EQ(q.enqueue(a, peer, 100, r),
        Queue::Result::accepted); // third accepted lost
    REQUIRE_EQ(q.snapshot().queued, 1U);
    REQUIRE_EQ(q.snapshot().dropped, 2U);
    REQUIRE_EQ(q.snapshot().retries, 2U);
    REQUIRE_EQ(q.take(100)->bytes[0], b[0]);
}
TEST(hybrid_link_capacity_is_fixed_and_duplicate_admission_is_atomic)
{
    Queue q;
    auto peer = IpEndpoint::loopback(1);
    for (unsigned i = 0; i < Queue::capacity - 1; ++i)
        REQUIRE_EQ(q.enqueue(a, peer, 100, {100}), Queue::Result::accepted);
    REQUIRE_EQ(q.enqueue(a, peer, 100, {100, 0, 0, 1}), Queue::Result::retry);
    REQUIRE_EQ(q.snapshot().queued, Queue::capacity - 1);
    REQUIRE_EQ(q.snapshot().accepted, Queue::capacity - 1);
    REQUIRE(q.take(200));
    REQUIRE_EQ(
        q.enqueue(b, peer, 200, {100, 0, 0, 1}), Queue::Result::accepted);
    REQUIRE_EQ(q.snapshot().queued, Queue::capacity);
    REQUIRE_EQ(q.snapshot().peak, Queue::capacity);
    REQUIRE_EQ(q.snapshot().overflows, 1U);
}
TEST(hybrid_link_invalid_bounds_and_later_rules_do_not_retime_queued_packets)
{
    Queue q;
    auto peer = IpEndpoint::loopback(1);
    REQUIRE_EQ(q.enqueue({}, peer, 100, {}), Queue::Result::invalid);
    REQUIRE_EQ(q.enqueue(a, peer, 100, {250001}), Queue::Result::invalid);
    REQUIRE_EQ(q.enqueue(a, peer, UINT64_MAX, {}), Queue::Result::invalid);
    REQUIRE_EQ(q.enqueue(a, peer, 100, {100}), Queue::Result::accepted);
    REQUIRE_EQ(q.enqueue(b, peer, 101, {}), Queue::Result::accepted);
    REQUIRE_EQ(q.take(101)->bytes[0], b[0]);
    REQUIRE(!q.take(199));
    REQUIRE_EQ(q.take(200)->bytes[0], a[0]);
    REQUIRE_EQ(q.snapshot().attempts, 2U);
}
