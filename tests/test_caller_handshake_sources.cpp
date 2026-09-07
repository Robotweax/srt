#include "test.hpp"

#include "compat/caller_handshake_sources.hpp"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <thread>

using namespace robotweax::srt;
using namespace robotweax::srt::compat;

namespace {

struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    bool started = false;
    bool released = false;
};

struct ReadinessCounter {
    std::size_t value = 0;
};

struct EventReadyObservation {
    std::mutex mutex;
    std::condition_variable changed;
    std::size_t calls = 0;
};

void count_readiness(void* context) noexcept
{
    ++static_cast<ReadinessCounter*>(context)->value;
}

void observe_event_ready(void* context) noexcept
{
    auto& observation = *static_cast<EventReadyObservation*>(context);
    {
        std::lock_guard lock(observation.mutex);
        ++observation.calls;
    }
    observation.changed.notify_all();
}

void wait_at_gate(void* context) noexcept
{
    auto& gate = *static_cast<Gate*>(context);
    std::unique_lock lock(gate.mutex);
    gate.started = true;
    gate.changed.notify_all();
    gate.changed.wait(lock, [&gate] {
        return gate.released;
    });
}

void do_nothing(void*) noexcept { }

void release_gate(const std::shared_ptr<Gate>& gate)
{
    {
        std::lock_guard lock(gate->mutex);
        gate->released = true;
    }
    gate->changed.notify_all();
}

struct GateRelease {
    std::shared_ptr<Gate> gate;

    ~GateRelease()
    {
        release_gate(gate);
    }
};

void wait_until_started(const std::shared_ptr<Gate>& gate)
{
    std::unique_lock lock(gate->mutex);
    REQUIRE(gate->changed.wait_for(lock, std::chrono::seconds {2}, [&gate] {
        return gate->started;
    }));
}

} // namespace

TEST(caller_handshake_sources_publish_inbox_and_affine_timer_readiness)
{
    RuntimeScheduler scheduler({
        .shard_count = 2,
        .queue_capacity_per_shard = 4,
        .timer_capacity_per_shard = 4,
    });
    REQUIRE(scheduler.start());
    const auto inbox = std::make_shared<DatagramInbox>(2);
    const auto source =
        std::make_shared<CallerHandshakeEventSource>(scheduler, 2U, inbox);
    REQUIRE_EQ(source->start(
                   std::chrono::steady_clock::now() + std::chrono::seconds {2}),
        CallerHandshakeDispatchStatus::completed);
    constexpr std::array<std::byte, 1> payload {std::byte {7}};
    REQUIRE(inbox->push(payload, Ipv4Endpoint::loopback()));
    REQUIRE_EQ(
        source->wait().kind, CallerHandshakeSourceEventKind::inbox_ready);
    DatagramEnvelope envelope;
    REQUIRE_EQ(inbox->pop_for(envelope, std::chrono::milliseconds {0}),
        InboxPopStatus::received);
    source->acknowledge_inbox();

    // Establish the order explicitly: a retry armed before inbox delivery can
    // legitimately fire first when the test thread is descheduled.
    REQUIRE_EQ(
        source->arm_retry(25U), CallerHandshakeDispatchStatus::completed);
    REQUIRE_EQ(
        source->wait().kind, CallerHandshakeSourceEventKind::retry_timer);
    source->close();
    REQUIRE_EQ(source->wait().kind, CallerHandshakeSourceEventKind::closed);
    source->stop();
    scheduler.stop();
}

TEST(caller_handshake_sources_replace_retry_deadlines_without_stale_events)
{
    RuntimeScheduler scheduler({
        .shard_count = 1,
        .queue_capacity_per_shard = 4,
        .timer_capacity_per_shard = 4,
    });
    REQUIRE(scheduler.start());
    const auto inbox = std::make_shared<DatagramInbox>(1);
    const auto source =
        std::make_shared<CallerHandshakeEventSource>(scheduler, 0U, inbox);
    REQUIRE_EQ(source->start(
                   std::chrono::steady_clock::now() + std::chrono::seconds {2}),
        CallerHandshakeDispatchStatus::completed);
    REQUIRE_EQ(source->arm_retry(1U), CallerHandshakeDispatchStatus::completed);
    REQUIRE_EQ(
        source->arm_retry(40U), CallerHandshakeDispatchStatus::completed);
    const auto earliest =
        std::chrono::steady_clock::now() + std::chrono::milliseconds {20};
    REQUIRE_EQ(
        source->wait().kind, CallerHandshakeSourceEventKind::retry_timer);
    REQUIRE(std::chrono::steady_clock::now() >= earliest);
    source->stop();
    scheduler.stop();
}

TEST(caller_handshake_sources_cancel_retry_invalidates_queued_events)
{
    RuntimeScheduler scheduler({
        .shard_count = 1,
        .queue_capacity_per_shard = 4,
        .timer_capacity_per_shard = 4,
    });
    REQUIRE(scheduler.start());
    const auto inbox = std::make_shared<DatagramInbox>(1);
    const auto source =
        std::make_shared<CallerHandshakeEventSource>(scheduler, 0U, inbox);
    REQUIRE_EQ(source->start(
                   std::chrono::steady_clock::now() + std::chrono::seconds {2}),
        CallerHandshakeDispatchStatus::completed);
    REQUIRE_EQ(source->arm_retry_at(std::chrono::steady_clock::now()
                   + std::chrono::milliseconds {1}),
        CallerHandshakeDispatchStatus::completed);
    source->cancel_retry();

    std::this_thread::sleep_for(std::chrono::milliseconds {20});
    CallerHandshakeSourceEvent event;
    REQUIRE(!source->try_pop(event));
    source->close();
    REQUIRE(source->try_pop(event));
    REQUIRE_EQ(event.kind, CallerHandshakeSourceEventKind::closed);
    source->stop();
    scheduler.stop();
}

TEST(caller_handshake_sources_report_scheduler_overload_without_polling)
{
    RuntimeScheduler scheduler({
        .shard_count = 1,
        .queue_capacity_per_shard = 1,
        .timer_capacity_per_shard = 2,
    });
    REQUIRE(scheduler.start());
    const auto inbox = std::make_shared<DatagramInbox>(1);
    const auto source =
        std::make_shared<CallerHandshakeEventSource>(scheduler, 0U, inbox);
    REQUIRE_EQ(source->start(
                   std::chrono::steady_clock::now() + std::chrono::seconds {2}),
        CallerHandshakeDispatchStatus::completed);

    const auto gate = std::make_shared<Gate>();
    GateRelease release {gate};
    REQUIRE_EQ(scheduler.submit(0U,
                   {
                       .function = wait_at_gate,
                       .context = gate,
                   }),
        RuntimeScheduler::SubmitStatus::accepted);
    wait_until_started(gate);
    REQUIRE_EQ(scheduler.submit(0U,
                   {
                       .function = do_nothing,
                       .context = gate,
                   }),
        RuntimeScheduler::SubmitStatus::accepted);

    constexpr std::array<std::byte, 1> payload {std::byte {9}};
    REQUIRE(inbox->push(payload, Ipv4Endpoint::loopback()));
    const CallerHandshakeSourceEvent event = source->wait();
    REQUIRE_EQ(event.kind, CallerHandshakeSourceEventKind::failure);
    REQUIRE_EQ(event.failure, CallerHandshakeDispatchStatus::full);

    source->stop();
    release_gate(gate);
    scheduler.stop();
}

TEST(caller_handshake_source_start_failure_preserves_existing_subscriber)
{
    RuntimeScheduler scheduler({
        .shard_count = 1,
        .queue_capacity_per_shard = 1,
        .timer_capacity_per_shard = 1,
    });
    REQUIRE(scheduler.start());
    const auto inbox = std::make_shared<DatagramInbox>(1);
    const auto counter = std::make_shared<ReadinessCounter>();
    REQUIRE(inbox->set_ready_handler(count_readiness, counter));
    const auto source =
        std::make_shared<CallerHandshakeEventSource>(scheduler, 0U, inbox);
    REQUIRE_EQ(source->start(
                   std::chrono::steady_clock::now() + std::chrono::seconds {2}),
        CallerHandshakeDispatchStatus::invalid);

    constexpr std::array<std::byte, 1> payload {std::byte {5}};
    REQUIRE(inbox->push(payload, Ipv4Endpoint::loopback()));
    REQUIRE_EQ(counter->value, 1U);
    inbox->clear_ready_handler();
    scheduler.stop();
}

TEST(caller_handshake_source_notifies_nonblocking_actor_consumers)
{
    RuntimeScheduler scheduler({
        .shard_count = 1,
        .queue_capacity_per_shard = 4,
        .timer_capacity_per_shard = 4,
    });
    REQUIRE(scheduler.start());
    const auto inbox = std::make_shared<DatagramInbox>(1);
    const auto source =
        std::make_shared<CallerHandshakeEventSource>(scheduler, 0U, inbox);
    const auto observation = std::make_shared<EventReadyObservation>();
    REQUIRE(source->set_event_ready_handler(observe_event_ready, observation));
    REQUIRE_EQ(source->start(
                   std::chrono::steady_clock::now() + std::chrono::seconds {2}),
        CallerHandshakeDispatchStatus::completed);
    REQUIRE_EQ(
        source->arm_retry(10U), CallerHandshakeDispatchStatus::completed);

    {
        std::unique_lock lock(observation->mutex);
        REQUIRE(observation->changed.wait_for(
            lock, std::chrono::seconds {2}, [&observation] {
                return observation->calls != 0U;
            }));
    }
    CallerHandshakeSourceEvent event;
    REQUIRE(source->try_pop(event));
    REQUIRE_EQ(event.kind, CallerHandshakeSourceEventKind::retry_timer);

    source->close();
    REQUIRE(source->try_pop(event));
    REQUIRE_EQ(event.kind, CallerHandshakeSourceEventKind::closed);
    source->clear_event_ready_handler();
    source->stop();
    scheduler.stop();
}
