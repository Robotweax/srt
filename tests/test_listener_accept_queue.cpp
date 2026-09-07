#include "compat/listener_accept_queue.hpp"
#include "test.hpp"

#include <atomic>
#include <chrono>
#include <future>

using namespace robotweax::srt;
using namespace robotweax::srt::compat;

TEST(listener_accept_queue_publishes_without_waiting_and_reports_saturation)
{
    ListenerAcceptQueue queue {1U};
    std::atomic_size_t observed_size = 0U;
    REQUIRE_EQ(queue.publish(
                   {
                       .handle = 101,
                       .peer = IpEndpoint::loopback(9'001U),
                   },
                   &observed_size),
        ListenerAcceptPublishStatus::published);
    REQUIRE_EQ(observed_size.load(std::memory_order_acquire), 1U);
    REQUIRE_EQ(queue.publish(
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
    REQUIRE_EQ(queue.publish(
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
    REQUIRE_EQ(queue.publish(
                   {
                       .handle = 104,
                       .peer = IpEndpoint::loopback(9'004U),
                   },
                   &observed_size),
        ListenerAcceptPublishStatus::published);
    queue.close(&observed_size);
    REQUIRE_EQ(queue.publish(
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
