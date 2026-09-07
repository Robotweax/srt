#include "test.hpp"

#include "compat/listener_connection_setup_sources.hpp"
#include "robotweax/srt/codec.hpp"
#include "srt/version.h"

#include <algorithm>
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

constexpr std::uint32_t setup_cookie = 0x1020'3040U;
constexpr std::uint32_t setup_listener_socket_id = 1'001U;
constexpr std::uint32_t setup_caller_socket_id = 2'002U;

[[nodiscard]] std::uint32_t setup_fixed_cookie(const Handshake&, void*) noexcept
{
    return setup_cookie;
}

[[nodiscard]] HandshakeMessage setup_induction()
{
    HandshakeMessage message;
    message.packet.version = handshake_version_4;
    message.packet.extension_field = udt_datagram_socket_type;
    message.packet.initial_sequence = SequenceNumber {0x1234'5678U};
    message.packet.maximum_transmission_unit = 1'500U;
    message.packet.flow_window = 25'600U;
    message.packet.request = HandshakeRequest::induction;
    message.packet.socket_id = setup_caller_socket_id;
    return message;
}

[[nodiscard]] HandshakeMessage setup_hsv5_conclusion()
{
    HandshakeMessage message = setup_induction();
    message.packet.version = handshake_version_5;
    message.packet.request = HandshakeRequest::conclusion;
    message.packet.maximum_transmission_unit = 1'200U;
    message.packet.flow_window = 8'192U;
    message.packet.syn_cookie = setup_cookie;
    message.packet.extension_field = 1U;
    message.has_handshake_extension = true;
    message.extension_type = HandshakeExtensionType::handshake_request;
    message.extension_parameters.srt_version = SRT_VERSION_VALUE;
    return message;
}

[[nodiscard]] ListenerHandshakeAdmission setup_admission(
    ListenerHandshakeProtocol protocol)
{
    return {
        .initial =
            {
                .message = setup_induction(),
                .peer = IpEndpoint::loopback(9'000U),
            },
        .conclusion =
            {
                .message = setup_hsv5_conclusion(),
                .control =
                    {
                        .timestamp = PacketTimestamp {7'001U},
                        .destination_socket_id = setup_listener_socket_id,
                    },
                .peer = IpEndpoint::loopback(9'000U),
            },
        .protocol = protocol,
        .validated_cookie = setup_cookie,
    };
}

[[nodiscard]] ListenerConnectionSetupSteps::Configuration setup_configuration()
{
    ListenerConnectionSetupSteps::Configuration result;
    result.hsv5.role = ConnectionRole::listener;
    result.hsv5.local_socket_id = setup_listener_socket_id;
    result.hsv5.initial_sequence = SequenceNumber {0x1234'5678U};
    result.hsv5.maximum_transmission_unit = 1'200U;
    result.hsv5.flow_window = 8'192U;
    result.hsv5.timeout_milliseconds = 37U;
    result.hsv5.maximum_retries = 1U;
    result.hsv5.cookie_generator = setup_fixed_cookie;

    return result;
}

struct EncodedSetupDatagram {
    std::array<std::byte, DatagramEnvelope::maximum_size> bytes {};
    std::size_t size = 0;
};

[[nodiscard]] EncodedSetupDatagram setup_handshake_datagram(
    const HandshakeMessage& message, std::uint32_t destination_socket_id)
{
    HandshakeAction action;
    action.kind = HandshakeActionKind::send;
    action.packet = message.packet;
    std::array<std::byte, DatagramEnvelope::maximum_size> bytes {};
    const HandshakeDatagramEncodeResult encoded = encode_handshake_datagram(
        action, PacketTimestamp {9'004U}, destination_socket_id, bytes);
    REQUIRE(encoded);
    EncodedSetupDatagram result;
    std::copy_n(bytes.begin(), encoded.bytes_written, result.bytes.begin());
    result.size = encoded.bytes_written;
    return result;
}

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

TEST(listener_connection_setup_sources_publish_inbox_and_deadlines)
{
    RuntimeScheduler scheduler({
        .shard_count = 2,
        .queue_capacity_per_shard = 4,
        .timer_capacity_per_shard = 4,
    });
    REQUIRE(scheduler.start());
    const auto inbox = std::make_shared<DatagramInbox>(2);
    const auto source = std::make_shared<ListenerConnectionSetupEventSource>(
        scheduler, 2U, inbox);
    REQUIRE_EQ(source->start(
                   std::chrono::steady_clock::now() + std::chrono::seconds {2}),
        ListenerConnectionSetupDispatchStatus::completed);
    REQUIRE_EQ(source->arm_retry(25U),
        ListenerConnectionSetupDispatchStatus::completed);

    constexpr std::array<std::byte, 1> payload {std::byte {7}};
    REQUIRE(inbox->push(payload, Ipv4Endpoint::loopback()));
    REQUIRE_EQ(source->wait().kind,
        ListenerConnectionSetupSourceEventKind::inbox_ready);
    DatagramEnvelope envelope;
    REQUIRE_EQ(inbox->pop_for(envelope, std::chrono::milliseconds {0}),
        InboxPopStatus::received);
    source->acknowledge_inbox();

    REQUIRE_EQ(source->wait().kind,
        ListenerConnectionSetupSourceEventKind::retry_timer);
    source->close();
    REQUIRE_EQ(
        source->wait().kind, ListenerConnectionSetupSourceEventKind::closed);
    source->stop();
    scheduler.stop();
}

TEST(listener_connection_setup_sources_rearm_existing_inbox_backlog)
{
    RuntimeScheduler scheduler({
        .shard_count = 1,
        .queue_capacity_per_shard = 4,
        .timer_capacity_per_shard = 2,
    });
    REQUIRE(scheduler.start());
    const auto inbox = std::make_shared<DatagramInbox>(2);
    constexpr std::array<std::byte, 1> first {std::byte {1}};
    constexpr std::array<std::byte, 1> second {std::byte {2}};
    REQUIRE(inbox->push(first, Ipv4Endpoint::loopback()));
    REQUIRE(inbox->push(second, Ipv4Endpoint::loopback()));

    const auto source = std::make_shared<ListenerConnectionSetupEventSource>(
        scheduler, 0U, inbox);
    REQUIRE_EQ(source->start(
                   std::chrono::steady_clock::now() + std::chrono::seconds {2}),
        ListenerConnectionSetupDispatchStatus::completed);

    DatagramEnvelope envelope;
    for (const std::byte expected : {first.front(), second.front()}) {
        REQUIRE_EQ(source->wait().kind,
            ListenerConnectionSetupSourceEventKind::inbox_ready);
        REQUIRE_EQ(inbox->pop_for(envelope, std::chrono::milliseconds {0}),
            InboxPopStatus::received);
        REQUIRE_EQ(envelope.bytes.front(), expected);
        source->acknowledge_inbox();
    }
    ListenerConnectionSetupSourceEvent event;
    REQUIRE(!source->try_pop(event));
    source->stop();
    scheduler.stop();
}

TEST(listener_connection_setup_sources_replace_retry_without_stale_event)
{
    RuntimeScheduler scheduler({
        .shard_count = 1,
        .queue_capacity_per_shard = 4,
        .timer_capacity_per_shard = 4,
    });
    REQUIRE(scheduler.start());
    const auto inbox = std::make_shared<DatagramInbox>(1);
    const auto source = std::make_shared<ListenerConnectionSetupEventSource>(
        scheduler, 0U, inbox);
    REQUIRE_EQ(source->start(
                   std::chrono::steady_clock::now() + std::chrono::seconds {2}),
        ListenerConnectionSetupDispatchStatus::completed);
    REQUIRE_EQ(source->arm_retry(1U),
        ListenerConnectionSetupDispatchStatus::completed);
    REQUIRE_EQ(source->arm_retry(40U),
        ListenerConnectionSetupDispatchStatus::completed);
    const auto earliest =
        std::chrono::steady_clock::now() + std::chrono::milliseconds {20};
    REQUIRE_EQ(source->wait().kind,
        ListenerConnectionSetupSourceEventKind::retry_timer);
    REQUIRE(std::chrono::steady_clock::now() >= earliest);
    source->stop();
    scheduler.stop();
}

TEST(listener_connection_setup_sources_cancel_retry_invalidates_queued_event)
{
    RuntimeScheduler scheduler({
        .shard_count = 1,
        .queue_capacity_per_shard = 4,
        .timer_capacity_per_shard = 4,
    });
    REQUIRE(scheduler.start());
    const auto inbox = std::make_shared<DatagramInbox>(1);
    const auto source = std::make_shared<ListenerConnectionSetupEventSource>(
        scheduler, 0U, inbox);
    REQUIRE_EQ(source->start(
                   std::chrono::steady_clock::now() + std::chrono::seconds {2}),
        ListenerConnectionSetupDispatchStatus::completed);
    REQUIRE_EQ(source->arm_retry_at(std::chrono::steady_clock::now()
                   + std::chrono::milliseconds {1}),
        ListenerConnectionSetupDispatchStatus::completed);
    source->cancel_retry();

    std::this_thread::sleep_for(std::chrono::milliseconds {20});
    ListenerConnectionSetupSourceEvent event;
    REQUIRE(!source->try_pop(event));
    source->close();
    REQUIRE(source->try_pop(event));
    REQUIRE_EQ(event.kind, ListenerConnectionSetupSourceEventKind::closed);
    source->stop();
    scheduler.stop();
}

TEST(listener_connection_setup_sources_report_scheduler_overload)
{
    RuntimeScheduler scheduler({
        .shard_count = 1,
        .queue_capacity_per_shard = 1,
        .timer_capacity_per_shard = 2,
    });
    REQUIRE(scheduler.start());
    const auto inbox = std::make_shared<DatagramInbox>(1);
    const auto source = std::make_shared<ListenerConnectionSetupEventSource>(
        scheduler, 0U, inbox);
    REQUIRE_EQ(source->start(
                   std::chrono::steady_clock::now() + std::chrono::seconds {2}),
        ListenerConnectionSetupDispatchStatus::completed);

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
    const ListenerConnectionSetupSourceEvent event = source->wait();
    REQUIRE_EQ(event.kind, ListenerConnectionSetupSourceEventKind::failure);
    REQUIRE_EQ(event.failure, ListenerConnectionSetupDispatchStatus::full);

    source->stop();
    release_gate(gate);
    scheduler.stop();
}

TEST(listener_connection_setup_source_start_preserves_existing_subscriber)
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
    const auto source = std::make_shared<ListenerConnectionSetupEventSource>(
        scheduler, 0U, inbox);
    REQUIRE_EQ(source->start(
                   std::chrono::steady_clock::now() + std::chrono::seconds {2}),
        ListenerConnectionSetupDispatchStatus::invalid);

    constexpr std::array<std::byte, 1> payload {std::byte {5}};
    REQUIRE(inbox->push(payload, Ipv4Endpoint::loopback()));
    REQUIRE_EQ(counter->value, 1U);
    inbox->clear_ready_handler();
    scheduler.stop();
}

TEST(listener_connection_setup_source_notifies_nonblocking_consumer)
{
    RuntimeScheduler scheduler({
        .shard_count = 1,
        .queue_capacity_per_shard = 4,
        .timer_capacity_per_shard = 4,
    });
    REQUIRE(scheduler.start());
    const auto inbox = std::make_shared<DatagramInbox>(1);
    const auto source = std::make_shared<ListenerConnectionSetupEventSource>(
        scheduler, 0U, inbox);
    const auto observation = std::make_shared<EventReadyObservation>();
    REQUIRE(source->set_event_ready_handler(observe_event_ready, observation));
    REQUIRE_EQ(source->start(
                   std::chrono::steady_clock::now() + std::chrono::seconds {2}),
        ListenerConnectionSetupDispatchStatus::completed);
    REQUIRE_EQ(source->arm_retry(10U),
        ListenerConnectionSetupDispatchStatus::completed);

    {
        std::unique_lock lock(observation->mutex);
        REQUIRE(observation->changed.wait_for(
            lock, std::chrono::seconds {2}, [&observation] {
                return observation->calls != 0U;
            }));
    }
    ListenerConnectionSetupSourceEvent event;
    REQUIRE(source->try_pop(event));
    REQUIRE_EQ(event.kind, ListenerConnectionSetupSourceEventKind::retry_timer);

    source->close();
    REQUIRE(source->try_pop(event));
    REQUIRE_EQ(event.kind, ListenerConnectionSetupSourceEventKind::closed);
    source->clear_event_ready_handler();
    source->stop();
    scheduler.stop();
}

TEST(listener_connection_setup_source_fails_closed_when_timer_capacity_is_full)
{
    RuntimeScheduler scheduler({
        .shard_count = 1,
        .queue_capacity_per_shard = 2,
        .timer_capacity_per_shard = 1,
    });
    REQUIRE(scheduler.start());
    const auto inbox = std::make_shared<DatagramInbox>(1);
    const auto source = std::make_shared<ListenerConnectionSetupEventSource>(
        scheduler, 0U, inbox);
    REQUIRE_EQ(source->start(
                   std::chrono::steady_clock::now() + std::chrono::seconds {2}),
        ListenerConnectionSetupDispatchStatus::completed);
    REQUIRE_EQ(
        source->arm_retry(20U), ListenerConnectionSetupDispatchStatus::full);

    const ListenerConnectionSetupSourceEvent event = source->wait();
    REQUIRE_EQ(event.kind, ListenerConnectionSetupSourceEventKind::failure);
    REQUIRE_EQ(event.failure, ListenerConnectionSetupDispatchStatus::full);
    source->stop();
    scheduler.stop();
}

TEST(listener_connection_setup_source_publishes_overall_timeout)
{
    RuntimeScheduler scheduler({
        .shard_count = 1,
        .queue_capacity_per_shard = 2,
        .timer_capacity_per_shard = 2,
    });
    REQUIRE(scheduler.start());
    const auto inbox = std::make_shared<DatagramInbox>(1);
    const auto source = std::make_shared<ListenerConnectionSetupEventSource>(
        scheduler, 0U, inbox);
    REQUIRE_EQ(source->start(std::chrono::steady_clock::now()
                   + std::chrono::milliseconds {15}),
        ListenerConnectionSetupDispatchStatus::completed);

    const ListenerConnectionSetupSourceEvent event = source->wait();
    REQUIRE_EQ(
        event.kind, ListenerConnectionSetupSourceEventKind::overall_timeout);
    source->stop();
    scheduler.stop();
}

TEST(listener_connection_setup_source_cancels_deadlines_on_close)
{
    RuntimeScheduler scheduler({
        .shard_count = 1,
        .queue_capacity_per_shard = 2,
        .timer_capacity_per_shard = 2,
    });
    REQUIRE(scheduler.start());
    const auto inbox = std::make_shared<DatagramInbox>(1);
    const auto source = std::make_shared<ListenerConnectionSetupEventSource>(
        scheduler, 0U, inbox);
    REQUIRE_EQ(source->start(
                   std::chrono::steady_clock::now() + std::chrono::seconds {2}),
        ListenerConnectionSetupDispatchStatus::completed);
    REQUIRE_EQ(source->arm_retry(1'000U),
        ListenerConnectionSetupDispatchStatus::completed);
    REQUIRE_EQ(scheduler.snapshot().timers, 2U);

    source->close();
    REQUIRE_EQ(scheduler.snapshot().timers, 0U);
    REQUIRE_EQ(
        source->wait().kind, ListenerConnectionSetupSourceEventKind::closed);
    source->stop();
    scheduler.stop();
}

TEST(listener_connection_setup_source_rejects_stopped_scheduler)
{
    RuntimeScheduler scheduler({
        .shard_count = 1,
        .queue_capacity_per_shard = 2,
        .timer_capacity_per_shard = 2,
    });
    REQUIRE(scheduler.start());
    scheduler.stop();
    const auto inbox = std::make_shared<DatagramInbox>(1);
    const auto source = std::make_shared<ListenerConnectionSetupEventSource>(
        scheduler, 0U, inbox);
    REQUIRE_EQ(source->start(
                   std::chrono::steady_clock::now() + std::chrono::seconds {2}),
        ListenerConnectionSetupDispatchStatus::stopped);
}

TEST(listener_connection_setup_actor_connects_hsv5_on_its_affinity_shard)
{
    const auto scheduler =
        std::make_shared<RuntimeScheduler>(RuntimeScheduler::Configuration {
            .shard_count = 2,
            .queue_capacity_per_shard = 8,
            .timer_capacity_per_shard = 4,
        });
    REQUIRE(scheduler->start());
    const auto blocked = std::make_shared<Gate>();
    GateRelease release {blocked};
    REQUIRE_EQ(scheduler->submit(0U,
                   {
                       .function = wait_at_gate,
                       .context = blocked,
                   }),
        RuntimeScheduler::SubmitStatus::accepted);
    wait_until_started(blocked);

    const auto inbox = std::make_shared<DatagramInbox>(2);
    const auto actor = std::make_shared<ListenerConnectionSetupActor>(scheduler,
        1U, inbox, setup_admission(ListenerHandshakeProtocol::hsv5),
        setup_configuration(), 1U);
    REQUIRE_EQ(actor->start(
                   std::chrono::steady_clock::now() + std::chrono::seconds {2}),
        ListenerConnectionSetupDispatchStatus::completed);
    const ListenerConnectionSetupActorResult result = actor->wait();
    REQUIRE_EQ(result.kind, ListenerConnectionSetupActorResultKind::setup);
    REQUIRE_EQ(result.setup.outcome, ListenerConnectionSetupOutcome::connected);
    REQUIRE_EQ(result.setup.hsv5_actions.size, 2U);
    REQUIRE(result.runtime);
    REQUIRE_EQ(result.runtime.peer_flow_window, 8'192U);
    REQUIRE(result.has_peer_handshake);
    REQUIRE_EQ(result.peer_handshake.packet.socket_id, setup_caller_socket_id);
    REQUIRE(actor->snapshot().terminal);

    constexpr std::array<std::byte, 1> preserved {std::byte {0x55}};
    REQUIRE(inbox->push(preserved, IpEndpoint::loopback(9'000U)));
    DatagramEnvelope envelope;
    REQUIRE_EQ(inbox->pop_for(envelope, std::chrono::milliseconds {0}),
        InboxPopStatus::received);
    actor->stop();
    release_gate(blocked);
    scheduler->stop();
}

TEST(listener_connection_setup_actor_notifies_a_nonblocking_result_consumer)
{
    auto scheduler =
        std::make_shared<RuntimeScheduler>(RuntimeScheduler::Configuration {
            .shard_count = 1,
            .queue_capacity_per_shard = 8,
            .timer_capacity_per_shard = 2,
        });
    REQUIRE(scheduler->start());
    const auto inbox = std::make_shared<DatagramInbox>(2U);
    const auto actor = std::make_shared<ListenerConnectionSetupActor>(scheduler,
        0U, inbox, setup_admission(ListenerHandshakeProtocol::hsv5),
        setup_configuration(), 1U);
    const auto observation = std::make_shared<EventReadyObservation>();
    REQUIRE(actor->set_result_ready_handler(observe_event_ready, observation));
    REQUIRE(!actor->set_result_ready_handler(observe_event_ready, observation));
    REQUIRE_EQ(actor->start(
                   std::chrono::steady_clock::now() + std::chrono::seconds {2}),
        ListenerConnectionSetupDispatchStatus::completed);

    {
        std::unique_lock lock(observation->mutex);
        REQUIRE(observation->changed.wait_for(
            lock, std::chrono::seconds {2}, [&observation] {
                return observation->calls >= 1U;
            }));
    }
    ListenerConnectionSetupActorResult result;
    REQUIRE(actor->try_pop(result));
    REQUIRE_EQ(result.kind, ListenerConnectionSetupActorResultKind::setup);
    REQUIRE_EQ(result.setup.outcome, ListenerConnectionSetupOutcome::connected);

    actor->clear_result_ready_handler();
    actor->close();
    actor->stop();
    scheduler->stop();
}

TEST(listener_connection_setup_actor_owns_prestart_policy_rejection)
{
    const auto scheduler =
        std::make_shared<RuntimeScheduler>(RuntimeScheduler::Configuration {
            .shard_count = 1,
            .queue_capacity_per_shard = 8,
            .timer_capacity_per_shard = 4,
        });
    REQUIRE(scheduler->start());
    const auto actor = std::make_shared<ListenerConnectionSetupActor>(scheduler,
        0U, std::make_shared<DatagramInbox>(1),
        setup_admission(ListenerHandshakeProtocol::hsv5), setup_configuration(),
        1U);
    REQUIRE(actor->reject(10));
    REQUIRE(!actor->reject(11));
    REQUIRE_EQ(actor->start(
                   std::chrono::steady_clock::now() + std::chrono::seconds {2}),
        ListenerConnectionSetupDispatchStatus::completed);
    const ListenerConnectionSetupActorResult rejected = actor->wait();
    REQUIRE_EQ(rejected.kind, ListenerConnectionSetupActorResultKind::setup);
    REQUIRE_EQ(
        rejected.setup.outcome, ListenerConnectionSetupOutcome::rejected);
    REQUIRE_EQ(rejected.setup.rejection_reason, 10);
    REQUIRE_EQ(rejected.setup.hsv5_actions.size, 2U);
    REQUIRE_EQ(
        rejected.setup.hsv5_actions.values[0].kind, HandshakeActionKind::send);
    REQUIRE_EQ(rejected.setup.hsv5_actions.values[1].kind,
        HandshakeActionKind::rejected);
    REQUIRE(actor->snapshot().terminal);
    actor->stop();
    scheduler->stop();
}

TEST(listener_connection_setup_actor_rejects_stopped_scheduler)
{
    const auto scheduler =
        std::make_shared<RuntimeScheduler>(RuntimeScheduler::Configuration {
            .shard_count = 1,
            .queue_capacity_per_shard = 2,
            .timer_capacity_per_shard = 2,
        });
    REQUIRE(scheduler->start());
    scheduler->stop();
    const auto actor = std::make_shared<ListenerConnectionSetupActor>(scheduler,
        0U, std::make_shared<DatagramInbox>(1),
        setup_admission(ListenerHandshakeProtocol::hsv5), setup_configuration(),
        1U);
    REQUIRE_EQ(actor->start(
                   std::chrono::steady_clock::now() + std::chrono::seconds {2}),
        ListenerConnectionSetupDispatchStatus::stopped);
    const ListenerConnectionSetupActorResult failure = actor->wait();
    REQUIRE_EQ(failure.kind, ListenerConnectionSetupActorResultKind::failure);
    REQUIRE_EQ(failure.failure, ListenerConnectionSetupDispatchStatus::stopped);
}
