#include "compat/listener_accept_queue.hpp"
#include "test.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <barrier>
#include <array>
#include <thread>

using namespace robotweax::srt;
using namespace robotweax::srt::compat;

TEST(listener_accept_queue_publishes_without_waiting_and_reports_saturation)
{
    ListenerAcceptQueue queue {1U};
    std::atomic_size_t observed_size = 0U;
    REQUIRE_EQ(queue.publish(queue.reserve(),
                   {
                       .handle = 101,
                       .peer = IpEndpoint::loopback(9'001U),
                   },
                   &observed_size),
        ListenerAcceptPublishStatus::published);
    REQUIRE_EQ(observed_size.load(std::memory_order_acquire), 1U);
    REQUIRE_EQ(queue.publish(queue.reserve(),
                   {
                       .handle = 102,
                       .peer = IpEndpoint::loopback(9'002U),
                   },
                   &observed_size),
        ListenerAcceptPublishStatus::full);
    REQUIRE_EQ(observed_size.load(std::memory_order_acquire), 1U);

    ListenerAcceptedConnection accepted;
    REQUIRE(queue.pop(false, accepted, &observed_size));
    REQUIRE_EQ(accepted.handle, 101);
    REQUIRE_EQ(accepted.peer.port, 9'001U);
    REQUIRE_EQ(observed_size.load(std::memory_order_acquire), 0U);
    REQUIRE(!queue.pop(false, accepted, &observed_size));
}

TEST(listener_accept_queue_wakes_a_blocking_application_consumer)
{
    ListenerAcceptQueue queue {2U};
    std::atomic_size_t observed_size = 0U;
    auto consumer = std::async(std::launch::async, [&] {
        ListenerAcceptedConnection accepted;
        const bool popped = queue.pop(true, accepted, &observed_size);
        return popped ? accepted.handle : SRT_INVALID_SOCK;
    });

    REQUIRE_EQ(consumer.wait_for(std::chrono::milliseconds {20}),
        std::future_status::timeout);
    REQUIRE_EQ(queue.publish(queue.reserve(),
                   {
                       .handle = 103,
                       .peer = IpEndpoint::loopback(9'003U),
                   },
                   &observed_size),
        ListenerAcceptPublishStatus::published);
    REQUIRE_EQ(
        consumer.wait_for(std::chrono::seconds {1}), std::future_status::ready);
    REQUIRE_EQ(consumer.get(), 103);
    REQUIRE_EQ(observed_size.load(std::memory_order_acquire), 0U);
}

TEST(listener_accept_queue_close_wakes_waiters_and_preserves_queued_ownership)
{
    ListenerAcceptQueue queue {2U};
    std::atomic_size_t observed_size = 0U;
    REQUIRE_EQ(queue.publish(queue.reserve(),
                   {
                       .handle = 104,
                       .peer = IpEndpoint::loopback(9'004U),
                   },
                   &observed_size),
        ListenerAcceptPublishStatus::published);
    queue.close(&observed_size);
    REQUIRE_EQ(queue.publish(queue.reserve(),
                   {
                       .handle = 105,
                       .peer = IpEndpoint::loopback(9'005U),
                   },
                   &observed_size),
        ListenerAcceptPublishStatus::closed);

    ListenerAcceptedConnection accepted;
    REQUIRE(queue.pop(true, accepted, &observed_size));
    REQUIRE_EQ(accepted.handle, 104);
    REQUIRE(!queue.pop(true, accepted, &observed_size));
    REQUIRE_EQ(observed_size.load(std::memory_order_acquire), 0U);
    const ListenerAcceptQueueSnapshot snapshot = queue.snapshot();
    REQUIRE_EQ(snapshot.capacity, 2U);
    REQUIRE_EQ(snapshot.size, 0U);
    REQUIRE(snapshot.closed);

    ListenerAcceptQueue empty_queue {1U};
    auto waiter = std::async(std::launch::async, [&] {
        ListenerAcceptedConnection ignored;
        return empty_queue.pop(true, ignored);
    });
    REQUIRE_EQ(waiter.wait_for(std::chrono::milliseconds {20}),
        std::future_status::timeout);
    empty_queue.close();
    REQUIRE_EQ(
        waiter.wait_for(std::chrono::seconds {1}), std::future_status::ready);
    REQUIRE(!waiter.get());
}

TEST(listener_accept_queue_reserves_capacity_until_publication_or_release)
{
    ListenerAcceptQueue queue {2U};
    auto first = queue.reserve();
    auto second = queue.reserve();
    REQUIRE(first);
    REQUIRE(second);
    auto blocked = queue.reserve();
    REQUIRE(!blocked);
    REQUIRE_EQ(queue.snapshot().size, 0U);
    REQUIRE_EQ(queue.snapshot().reserved, 2U);
    REQUIRE_EQ(queue.publish(queue.reserve(), {.handle = 9}),
        ListenerAcceptPublishStatus::full);
    first.release();
    first.release();
    REQUIRE_EQ(blocked.acquire(), ListenerAcceptReserveStatus::reserved);
    REQUIRE_EQ(queue.snapshot().reserved, 2U);
    REQUIRE_EQ(queue.publish(std::move(second), {.handle = 11}),
        ListenerAcceptPublishStatus::published);
    REQUIRE(!second);
    REQUIRE_EQ(queue.snapshot().size, 1U);
    REQUIRE_EQ(queue.snapshot().reserved, 1U);
    REQUIRE_EQ(queue.publish(std::move(second), {.handle = 12}),
        ListenerAcceptPublishStatus::full);
    ListenerAcceptedConnection accepted;
    REQUIRE(queue.pop(false, accepted));
    REQUIRE_EQ(accepted.handle, 11);
    REQUIRE_EQ(queue.publish(std::move(blocked), {.handle = 13}),
        ListenerAcceptPublishStatus::published);
}

TEST(listener_accept_queue_reservation_move_assignment_releases_old_slot)
{
    ListenerAcceptQueue first {1U};
    ListenerAcceptQueue second {1U};
    auto token = first.reserve();
    {
        auto moved = std::move(token);
        REQUIRE(!token);
        token = second.reserve();
        REQUIRE_EQ(first.snapshot().reserved, 1U);
        moved = std::move(token);
        REQUIRE(!token);
        REQUIRE_EQ(first.snapshot().reserved, 0U);
        REQUIRE_EQ(second.snapshot().reserved, 1U);
        REQUIRE_EQ(first.publish(std::move(moved), {.handle = 14}),
            ListenerAcceptPublishStatus::full);
        REQUIRE(moved);
    }
    REQUIRE_EQ(second.snapshot().reserved, 0U);
}

TEST(listener_accept_queue_close_consumes_reservations_without_publication)
{
    ListenerAcceptQueue queue {1U};
    auto token = queue.reserve();
    queue.close();
    REQUIRE(token);
    REQUIRE_EQ(queue.snapshot().reserved, 1U);
    REQUIRE_EQ(token.acquire(), ListenerAcceptReserveStatus::closed);
    REQUIRE_EQ(queue.publish(std::move(token), {.handle = 15}),
        ListenerAcceptPublishStatus::closed);
    REQUIRE(!token);
    REQUIRE_EQ(queue.snapshot().reserved, 0U);
    REQUIRE_EQ(queue.snapshot().size, 0U);
    REQUIRE(!queue.reserve());
}

TEST(listener_accept_queue_reservations_outlive_closed_owner_safely)
{
    ListenerAcceptQueue::Reservation token;
    {
        ListenerAcceptQueue queue {1U};
        token = queue.reserve();
        REQUIRE(token);
    }
    REQUIRE_EQ(token.acquire(), ListenerAcceptReserveStatus::closed);
    token.release();
    REQUIRE_EQ(token.acquire(), ListenerAcceptReserveStatus::closed);
}

TEST(listener_accept_queue_inflight_reservation_survives_concurrent_accept)
{
    ListenerAcceptQueue queue {2U};
    REQUIRE_EQ(queue.publish(queue.reserve(), {.handle = 16}),
        ListenerAcceptPublishStatus::published);
    auto inflight = queue.reserve();
    auto consumer = std::async(std::launch::async, [&] {
        ListenerAcceptedConnection accepted;
        return queue.pop(false, accepted) && accepted.handle == 16;
    });
    REQUIRE(consumer.get());
    auto next = queue.reserve();
    REQUIRE(next);
    REQUIRE(!queue.reserve());
    REQUIRE_EQ(queue.snapshot().reserved, 2U);
    REQUIRE_EQ(queue.publish(std::move(inflight), {.handle = 17}),
        ListenerAcceptPublishStatus::published);
    REQUIRE_EQ(queue.publish(std::move(next), {.handle = 18}),
        ListenerAcceptPublishStatus::published);
    REQUIRE_EQ(queue.snapshot().size, 2U);
    REQUIRE_EQ(queue.snapshot().reserved, 0U);
}

TEST(listener_accept_queue_concurrent_reservations_and_close_stay_bounded)
{
    ListenerAcceptQueue queue {2U};
    std::barrier phase {5};
    std::array<bool, 4> admitted {};
    std::array<std::thread, 4> threads;
    for (std::size_t index = 0; index < threads.size(); ++index) {
        threads[index] = std::thread([&, index] {
            auto reservation = queue.reserve();
            admitted[index] = static_cast<bool>(reservation);
            phase.arrive_and_wait();
            phase.arrive_and_wait();
        });
    }
    phase.arrive_and_wait();
    const auto before_close = queue.snapshot();
    queue.close();
    phase.arrive_and_wait();
    for (auto& thread : threads)
        thread.join();
    REQUIRE_EQ(before_close.reserved, 2U);
    unsigned count = 0;
    for (const bool accepted : admitted)
        if (accepted)
            ++count;
    REQUIRE_EQ(count, 2U);
    REQUIRE_EQ(queue.snapshot().reserved, 0U);
    REQUIRE(!queue.reserve());
}
