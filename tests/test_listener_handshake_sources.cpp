#include "test.hpp"

#include "compat/listener_handshake_sources.hpp"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
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

[[nodiscard]] StatelessListenerHandshakeRouter::Configuration
admission_configuration()
{
    constexpr std::array<std::byte, 16> secret {
        std::byte {0x01},
        std::byte {0x02},
        std::byte {0x03},
        std::byte {0x04},
        std::byte {0x05},
        std::byte {0x06},
        std::byte {0x07},
        std::byte {0x08},
        std::byte {0x09},
        std::byte {0x0a},
        std::byte {0x0b},
        std::byte {0x0c},
        std::byte {0x0d},
        std::byte {0x0e},
        std::byte {0x0f},
        std::byte {0x10},
    };
    return {
        .listener_socket_id = 900U,
        .maximum_transmission_unit = 1'400U,
        .flow_window = 8'192U,
        .encryption_field = 3U,
        .cookie_secret = secret,
    };
}

[[nodiscard]] HandshakeMessage admission_induction(
    std::uint32_t socket_id = 100U, bool legacy_stream = false)
{
    HandshakeMessage message;
    message.packet.version = handshake_version_4;
    message.packet.encryption_field = legacy_stream ? 0U : 2U;
    message.packet.extension_field =
        legacy_stream ? udt_stream_socket_type : 0U;
    message.packet.initial_sequence = SequenceNumber {1'234U};
    message.packet.maximum_transmission_unit = 1'500U;
    message.packet.flow_window = 25'600U;
    message.packet.request = HandshakeRequest::induction;
    message.packet.socket_id = socket_id;
    return message;
}

[[nodiscard]] HandshakeMessage admission_conclusion(
    const HandshakeMessage& request, std::uint32_t cookie,
    bool legacy_stream = false)
{
    HandshakeMessage message = request;
    message.packet.version =
        legacy_stream ? handshake_version_4 : handshake_version_5;
    message.packet.encryption_field = legacy_stream ? 0U : 3U;
    message.packet.extension_field =
        legacy_stream ? udt_stream_socket_type : 1U;
    message.packet.request = HandshakeRequest::conclusion;
    message.packet.syn_cookie = cookie;
    if (!legacy_stream) {
        message.has_handshake_extension = true;
        message.extension_type = HandshakeExtensionType::handshake_request;
        message.has_stream_id_extension = true;
        REQUIRE(message.stream_id.assign("actor/source"));
    }
    return message;
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

[[nodiscard]] std::uint64_t fixed_listener_time_window(void* context) noexcept
{
    return *static_cast<const std::uint64_t*>(context);
}

void wait_until_terminal(const std::shared_ptr<ListenerHandshakeActor>& actor)
{
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds {2};
    while (!actor->snapshot().terminal
        && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds {1});
    }
    REQUIRE(actor->snapshot().terminal);
}

} // namespace

TEST(listener_handshake_admission_steps_emit_bounded_hsv5_actions)
{
    constexpr std::uint64_t time_window = 42U;
    const IpEndpoint peer = IpEndpoint::loopback(9'000U);
    ListenerHandshakeAdmissionSteps steps {admission_configuration()};
    HandshakeEnvelope induction {
        .message = admission_induction(),
        .control = {.timestamp = PacketTimestamp {70U}},
        .peer = peer,
    };

    const ListenerHandshakeAdmissionStep response =
        steps.receive(induction, time_window);
    REQUIRE_EQ(response.kind, ListenerHandshakeAdmissionStepKind::send);
    REQUIRE_EQ(response.peer, peer);
    REQUIRE_EQ(response.destination_socket_id, 100U);
    REQUIRE_EQ(response.response.kind, HandshakeActionKind::send);
    REQUIRE_EQ(response.response.packet.request, HandshakeRequest::induction);
    REQUIRE(response.response.packet.syn_cookie != 0U);

    HandshakeEnvelope conclusion {
        .message = admission_conclusion(
            induction.message, response.response.packet.syn_cookie),
        .control = {.timestamp = PacketTimestamp {91U}},
        .peer = peer,
    };
    const ListenerHandshakeAdmissionStep admitted =
        steps.receive(conclusion, time_window);
    REQUIRE_EQ(admitted.kind, ListenerHandshakeAdmissionStepKind::admit);
    REQUIRE_EQ(admitted.peer, peer);
    REQUIRE_EQ(admitted.destination_socket_id, 100U);
    REQUIRE_EQ(admitted.admission.protocol, ListenerHandshakeProtocol::hsv5);
    REQUIRE_EQ(admitted.admission.validated_cookie,
        response.response.packet.syn_cookie);
    REQUIRE_EQ(admitted.admission.initial.message.packet.request,
        HandshakeRequest::induction);
    REQUIRE_EQ(
        admitted.admission.initial.message.packet.version, handshake_version_4);
    REQUIRE_EQ(admitted.admission.initial.message.packet.syn_cookie, 0U);
    REQUIRE_EQ(
        admitted.admission.initial.control.timestamp, PacketTimestamp {91U});
    REQUIRE_EQ(admitted.admission.conclusion.message.packet.request,
        HandshakeRequest::conclusion);
    REQUIRE_EQ(
        admitted.admission.conclusion.message.stream_id.view(), "actor/source");
    REQUIRE_EQ(
        admitted.admission.conclusion.control.timestamp, PacketTimestamp {91U});
}

TEST(listener_handshake_admission_steps_drop_legacy_stream_without_admission)
{
    constexpr std::uint64_t time_window = 9U;
    const IpEndpoint peer = IpEndpoint::ipv6_loopback(9'001U, 7U);
    ListenerHandshakeAdmissionSteps steps {admission_configuration()};
    const HandshakeMessage request = admission_induction(101U, true);
    const ListenerHandshakeAdmissionStep response =
        steps.receive({.message = request, .peer = peer}, time_window);
    REQUIRE_EQ(response.kind, ListenerHandshakeAdmissionStepKind::ignore);
}

TEST(
    listener_handshake_admission_steps_ignore_invalid_input_and_close_terminally)
{
    ListenerHandshakeAdmissionSteps steps {admission_configuration()};
    HandshakeMessage invalid = admission_induction();
    invalid.packet.flow_window = 0U;
    REQUIRE_EQ(
        steps.receive({.message = invalid, .peer = IpEndpoint::loopback()}, 1U)
            .kind,
        ListenerHandshakeAdmissionStepKind::ignore);

    REQUIRE_EQ(steps.close().kind, ListenerHandshakeAdmissionStepKind::closed);
    REQUIRE(steps.closed());
    REQUIRE_EQ(steps
                   .receive(
                       {
                           .message = admission_induction(),
                           .peer = IpEndpoint::loopback(),
                       },
                       1U)
                   .kind,
        ListenerHandshakeAdmissionStepKind::closed);
    REQUIRE_EQ(steps.close().kind, ListenerHandshakeAdmissionStepKind::closed);
}

TEST(listener_handshake_actor_runs_admission_on_its_affinity_shard)
{
    auto scheduler =
        std::make_shared<RuntimeScheduler>(RuntimeScheduler::Configuration {
            .shard_count = 2,
            .queue_capacity_per_shard = 4,
            .timer_capacity_per_shard = 1,
        });
    REQUIRE(scheduler->start());
    const auto blocked_shard = std::make_shared<Gate>();
    GateRelease release {blocked_shard};
    REQUIRE_EQ(scheduler->submit(0U,
                   {
                       .function = wait_at_gate,
                       .context = blocked_shard,
                   }),
        RuntimeScheduler::SubmitStatus::accepted);
    wait_until_started(blocked_shard);

    const auto inbox = std::make_shared<HandshakeInbox>(2);
    const auto time_window = std::make_shared<std::uint64_t>(42U);
    const auto actor = std::make_shared<ListenerHandshakeActor>(scheduler, 1U,
        inbox, admission_configuration(), 2U, fixed_listener_time_window,
        time_window);
    REQUIRE_EQ(actor->start(), ListenerHandshakeDispatchStatus::completed);

    const IpEndpoint peer = IpEndpoint::loopback(9'002U);
    const HandshakeEnvelope induction {
        .message = admission_induction(102U),
        .peer = peer,
    };
    REQUIRE(inbox->push(induction));
    const ListenerHandshakeActorResult response = actor->wait();
    REQUIRE_EQ(response.kind, ListenerHandshakeActorResultKind::step);
    REQUIRE_EQ(response.step.kind, ListenerHandshakeAdmissionStepKind::send);
    REQUIRE_EQ(response.step.peer, peer);

    REQUIRE(inbox->push({
        .message = admission_conclusion(
            induction.message, response.step.response.packet.syn_cookie),
        .peer = peer,
    }));
    const ListenerHandshakeActorResult admitted = actor->wait();
    REQUIRE_EQ(admitted.kind, ListenerHandshakeActorResultKind::step);
    REQUIRE_EQ(admitted.step.kind, ListenerHandshakeAdmissionStepKind::admit);
    REQUIRE_EQ(
        admitted.step.admission.protocol, ListenerHandshakeProtocol::hsv5);
    REQUIRE_EQ(admitted.step.admission.validated_cookie,
        response.step.response.packet.syn_cookie);
    REQUIRE(actor->snapshot().admission_pending);
    REQUIRE(actor->complete_admission());
    REQUIRE(!actor->snapshot().admission_pending);
    REQUIRE(!actor->complete_admission());

    actor->close();
    REQUIRE_EQ(actor->wait().kind, ListenerHandshakeActorResultKind::closed);
    actor->stop();
    release_gate(blocked_shard);
    scheduler->stop();
}

TEST(listener_handshake_actor_notifies_a_nonblocking_result_consumer)
{
    auto scheduler =
        std::make_shared<RuntimeScheduler>(RuntimeScheduler::Configuration {
            .shard_count = 1,
            .queue_capacity_per_shard = 4,
            .timer_capacity_per_shard = 1,
        });
    REQUIRE(scheduler->start());
    const auto inbox = std::make_shared<HandshakeInbox>(2);
    const auto time_window = std::make_shared<std::uint64_t>(42U);
    const auto actor = std::make_shared<ListenerHandshakeActor>(scheduler, 0U,
        inbox, admission_configuration(), 2U, fixed_listener_time_window,
        time_window);
    const auto observation = std::make_shared<EventReadyObservation>();
    REQUIRE(actor->set_result_ready_handler(observe_event_ready, observation));
    REQUIRE(!actor->set_result_ready_handler(observe_event_ready, observation));
    REQUIRE_EQ(actor->start(), ListenerHandshakeDispatchStatus::completed);

    REQUIRE(inbox->push({
        .message = admission_induction(132U),
        .peer = IpEndpoint::loopback(9'032U),
    }));
    {
        std::unique_lock lock(observation->mutex);
        REQUIRE(observation->changed.wait_for(
            lock, std::chrono::seconds {2}, [&observation] {
                return observation->calls >= 1U;
            }));
    }
    ListenerHandshakeActorResult response;
    REQUIRE(actor->try_pop(response));
    REQUIRE_EQ(response.kind, ListenerHandshakeActorResultKind::step);
    REQUIRE_EQ(response.step.kind, ListenerHandshakeAdmissionStepKind::send);

    actor->close();
    {
        std::unique_lock lock(observation->mutex);
        REQUIRE(observation->changed.wait_for(
            lock, std::chrono::seconds {2}, [&observation] {
                return observation->calls >= 2U;
            }));
    }
    ListenerHandshakeActorResult closed;
    REQUIRE(actor->try_pop(closed));
    REQUIRE_EQ(closed.kind, ListenerHandshakeActorResultKind::closed);
    actor->clear_result_ready_handler();
    actor->stop();
    scheduler->stop();
}

TEST(listener_handshake_actor_holds_following_input_until_admission_completes)
{
    auto scheduler =
        std::make_shared<RuntimeScheduler>(RuntimeScheduler::Configuration {
            .shard_count = 1,
            .queue_capacity_per_shard = 4,
            .timer_capacity_per_shard = 1,
        });
    REQUIRE(scheduler->start());
    const auto inbox = std::make_shared<HandshakeInbox>(3);
    const auto time_window = std::make_shared<std::uint64_t>(42U);
    const auto actor = std::make_shared<ListenerHandshakeActor>(scheduler, 0U,
        inbox, admission_configuration(), 2U, fixed_listener_time_window,
        time_window);
    REQUIRE_EQ(actor->start(), ListenerHandshakeDispatchStatus::completed);

    const IpEndpoint first_peer = IpEndpoint::loopback(9'012U);
    const HandshakeEnvelope first_induction {
        .message = admission_induction(112U),
        .peer = first_peer,
    };
    REQUIRE(inbox->push(first_induction));
    const ListenerHandshakeActorResult response = actor->wait();
    REQUIRE_EQ(response.step.kind, ListenerHandshakeAdmissionStepKind::send);
    REQUIRE(inbox->push({
        .message = admission_conclusion(
            first_induction.message, response.step.response.packet.syn_cookie),
        .peer = first_peer,
    }));
    REQUIRE_EQ(
        actor->wait().step.kind, ListenerHandshakeAdmissionStepKind::admit);

    REQUIRE(inbox->push({
        .message = admission_induction(113U),
        .peer = IpEndpoint::loopback(9'013U),
    }));
    const auto barrier = std::make_shared<Gate>();
    GateRelease release_barrier {barrier};
    REQUIRE_EQ(scheduler->submit(0U,
                   {
                       .function = wait_at_gate,
                       .context = barrier,
                   }),
        RuntimeScheduler::SubmitStatus::accepted);
    wait_until_started(barrier);
    ListenerHandshakeActorResult following;
    REQUIRE(!actor->try_pop(following));

    REQUIRE(actor->complete_admission());
    release_gate(barrier);
    following = actor->wait();
    REQUIRE_EQ(following.kind, ListenerHandshakeActorResultKind::step);
    REQUIRE_EQ(following.step.kind, ListenerHandshakeAdmissionStepKind::send);

    actor->close();
    REQUIRE_EQ(actor->wait().kind, ListenerHandshakeActorResultKind::closed);
    actor->stop();
    scheduler->stop();
}

TEST(listener_handshake_actor_bounds_results_and_fails_closed)
{
    auto scheduler =
        std::make_shared<RuntimeScheduler>(RuntimeScheduler::Configuration {
            .shard_count = 1,
            .queue_capacity_per_shard = 4,
            .timer_capacity_per_shard = 1,
        });
    REQUIRE(scheduler->start());
    const auto gate = std::make_shared<Gate>();
    GateRelease release {gate};
    REQUIRE_EQ(scheduler->submit(0U,
                   {
                       .function = wait_at_gate,
                       .context = gate,
                   }),
        RuntimeScheduler::SubmitStatus::accepted);
    wait_until_started(gate);

    const auto inbox = std::make_shared<HandshakeInbox>(2);
    const auto time_window = std::make_shared<std::uint64_t>(5U);
    const auto actor = std::make_shared<ListenerHandshakeActor>(scheduler, 0U,
        inbox, admission_configuration(), 1U, fixed_listener_time_window,
        time_window);
    REQUIRE_EQ(actor->start(), ListenerHandshakeDispatchStatus::completed);
    REQUIRE(inbox->push({
        .message = admission_induction(103U),
        .peer = IpEndpoint::loopback(9'003U),
    }));
    REQUIRE(inbox->push({
        .message = admission_induction(104U),
        .peer = IpEndpoint::loopback(9'004U),
    }));
    release_gate(gate);

    wait_until_terminal(actor);
    const ListenerHandshakeActorSnapshot snapshot = actor->snapshot();
    REQUIRE_EQ(snapshot.queued_results, 1U);
    REQUIRE_EQ(snapshot.failure, ListenerHandshakeDispatchStatus::full);
    const ListenerHandshakeActorResult failure = actor->wait();
    REQUIRE_EQ(failure.kind, ListenerHandshakeActorResultKind::failure);
    REQUIRE_EQ(failure.failure, ListenerHandshakeDispatchStatus::full);
    REQUIRE(!inbox->push(HandshakeEnvelope {}));

    actor->stop();
    scheduler->stop();
}

TEST(listener_handshake_actor_close_is_terminal_without_input)
{
    auto scheduler =
        std::make_shared<RuntimeScheduler>(RuntimeScheduler::Configuration {
            .shard_count = 1,
            .queue_capacity_per_shard = 2,
            .timer_capacity_per_shard = 1,
        });
    REQUIRE(scheduler->start());
    const auto inbox = std::make_shared<HandshakeInbox>(1);
    const auto time_window = std::make_shared<std::uint64_t>(1U);
    const auto actor = std::make_shared<ListenerHandshakeActor>(scheduler, 0U,
        inbox, admission_configuration(), 1U, fixed_listener_time_window,
        time_window);
    REQUIRE_EQ(actor->start(), ListenerHandshakeDispatchStatus::completed);

    actor->close();
    REQUIRE_EQ(actor->wait().kind, ListenerHandshakeActorResultKind::closed);
    REQUIRE(actor->snapshot().terminal);
    REQUIRE(!inbox->push(HandshakeEnvelope {}));
    actor->close();
    actor->stop();
    scheduler->stop();
}

TEST(listener_handshake_actor_rejects_a_stopped_scheduler)
{
    auto scheduler =
        std::make_shared<RuntimeScheduler>(RuntimeScheduler::Configuration {
            .shard_count = 1,
            .queue_capacity_per_shard = 1,
            .timer_capacity_per_shard = 1,
        });
    REQUIRE(scheduler->start());
    scheduler->stop();
    const auto inbox = std::make_shared<HandshakeInbox>(1);
    const auto time_window = std::make_shared<std::uint64_t>(1U);
    const auto actor = std::make_shared<ListenerHandshakeActor>(scheduler, 0U,
        inbox, admission_configuration(), 1U, fixed_listener_time_window,
        time_window);

    REQUIRE_EQ(actor->start(), ListenerHandshakeDispatchStatus::stopped);
    const ListenerHandshakeActorResult failure = actor->wait();
    REQUIRE_EQ(failure.kind, ListenerHandshakeActorResultKind::failure);
    REQUIRE_EQ(failure.failure, ListenerHandshakeDispatchStatus::stopped);
    actor->stop();
}

TEST(listener_handshake_actor_start_close_race_is_terminal)
{
    auto scheduler =
        std::make_shared<RuntimeScheduler>(RuntimeScheduler::Configuration {
            .shard_count = 2,
            .queue_capacity_per_shard = 32,
            .timer_capacity_per_shard = 1,
        });
    REQUIRE(scheduler->start());
    const auto time_window = std::make_shared<std::uint64_t>(1U);

    for (std::size_t iteration = 0; iteration < 32U; ++iteration) {
        const auto inbox = std::make_shared<HandshakeInbox>(1);
        const auto actor = std::make_shared<ListenerHandshakeActor>(scheduler,
            iteration, inbox, admission_configuration(), 1U,
            fixed_listener_time_window, time_window);
        std::thread starter {[actor] {
            (void)actor->start();
        }};
        std::thread closer {[actor] {
            actor->close();
        }};
        starter.join();
        closer.join();
        actor->stop();
        REQUIRE(actor->snapshot().terminal);
        const ListenerHandshakeActorResult terminal = actor->wait();
        REQUIRE(terminal.kind == ListenerHandshakeActorResultKind::closed
            || terminal.kind == ListenerHandshakeActorResultKind::failure);
    }
    scheduler->stop();
}

TEST(listener_handshake_source_coalesces_and_rearms_inbox_readiness)
{
    RuntimeScheduler scheduler({
        .shard_count = 2,
        .queue_capacity_per_shard = 4,
        .timer_capacity_per_shard = 1,
    });
    REQUIRE(scheduler.start());
    const auto inbox = std::make_shared<HandshakeInbox>(2);
    const auto source =
        std::make_shared<ListenerHandshakeEventSource>(scheduler, 1U, inbox);
    REQUIRE_EQ(source->start(), ListenerHandshakeDispatchStatus::completed);

    REQUIRE(inbox->push(HandshakeEnvelope {}));
    REQUIRE(inbox->push(HandshakeEnvelope {}));
    REQUIRE_EQ(
        source->wait().kind, ListenerHandshakeSourceEventKind::inbox_ready);

    HandshakeEnvelope envelope;
    REQUIRE_EQ(inbox->pop_for(envelope, std::chrono::milliseconds {0}),
        InboxPopStatus::received);
    source->acknowledge_inbox();
    REQUIRE_EQ(
        source->wait().kind, ListenerHandshakeSourceEventKind::inbox_ready);
    REQUIRE_EQ(inbox->pop_for(envelope, std::chrono::milliseconds {0}),
        InboxPopStatus::received);
    source->acknowledge_inbox();

    ListenerHandshakeSourceEvent event;
    REQUIRE(!source->try_pop(event));
    source->close();
    REQUIRE(source->try_pop(event));
    REQUIRE_EQ(event.kind, ListenerHandshakeSourceEventKind::closed);
    source->stop();
    scheduler.stop();
}

TEST(listener_handshake_source_uses_its_affinity_shard)
{
    RuntimeScheduler scheduler({
        .shard_count = 2,
        .queue_capacity_per_shard = 2,
        .timer_capacity_per_shard = 1,
    });
    REQUIRE(scheduler.start());
    const auto gate = std::make_shared<Gate>();
    GateRelease release {gate};
    REQUIRE_EQ(scheduler.submit(0U,
                   {
                       .function = wait_at_gate,
                       .context = gate,
                   }),
        RuntimeScheduler::SubmitStatus::accepted);
    wait_until_started(gate);

    const auto inbox = std::make_shared<HandshakeInbox>(1);
    const auto source =
        std::make_shared<ListenerHandshakeEventSource>(scheduler, 1U, inbox);
    REQUIRE_EQ(source->start(), ListenerHandshakeDispatchStatus::completed);
    REQUIRE(inbox->push(HandshakeEnvelope {}));
    REQUIRE_EQ(
        source->wait().kind, ListenerHandshakeSourceEventKind::inbox_ready);

    source->stop();
    release_gate(gate);
    scheduler.stop();
}

TEST(listener_handshake_source_reports_scheduler_overload)
{
    RuntimeScheduler scheduler({
        .shard_count = 1,
        .queue_capacity_per_shard = 1,
        .timer_capacity_per_shard = 1,
    });
    REQUIRE(scheduler.start());
    const auto inbox = std::make_shared<HandshakeInbox>(1);
    const auto source =
        std::make_shared<ListenerHandshakeEventSource>(scheduler, 0U, inbox);
    REQUIRE_EQ(source->start(), ListenerHandshakeDispatchStatus::completed);

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

    REQUIRE(inbox->push(HandshakeEnvelope {}));
    const ListenerHandshakeSourceEvent event = source->wait();
    REQUIRE_EQ(event.kind, ListenerHandshakeSourceEventKind::failure);
    REQUIRE_EQ(event.failure, ListenerHandshakeDispatchStatus::full);

    source->stop();
    release_gate(gate);
    scheduler.stop();
}

TEST(listener_handshake_source_start_preserves_existing_subscriber)
{
    RuntimeScheduler scheduler({
        .shard_count = 1,
        .queue_capacity_per_shard = 1,
        .timer_capacity_per_shard = 1,
    });
    REQUIRE(scheduler.start());
    const auto inbox = std::make_shared<HandshakeInbox>(1);
    const auto counter = std::make_shared<ReadinessCounter>();
    REQUIRE(inbox->set_ready_handler(count_readiness, counter));
    const auto source =
        std::make_shared<ListenerHandshakeEventSource>(scheduler, 0U, inbox);
    REQUIRE_EQ(source->start(), ListenerHandshakeDispatchStatus::invalid);

    REQUIRE(inbox->push(HandshakeEnvelope {}));
    REQUIRE_EQ(counter->value, 1U);
    inbox->clear_ready_handler();
    scheduler.stop();
}

TEST(listener_handshake_source_rejects_a_stopped_scheduler)
{
    RuntimeScheduler scheduler({
        .shard_count = 1,
        .queue_capacity_per_shard = 1,
        .timer_capacity_per_shard = 1,
    });
    REQUIRE(scheduler.start());
    scheduler.stop();
    const auto inbox = std::make_shared<HandshakeInbox>(1);
    const auto source =
        std::make_shared<ListenerHandshakeEventSource>(scheduler, 0U, inbox);
    REQUIRE_EQ(source->start(), ListenerHandshakeDispatchStatus::stopped);

    const auto counter = std::make_shared<ReadinessCounter>();
    REQUIRE(inbox->set_ready_handler(count_readiness, counter));
    inbox->clear_ready_handler();
}

TEST(listener_handshake_source_notifies_nonblocking_actor_consumers)
{
    RuntimeScheduler scheduler({
        .shard_count = 1,
        .queue_capacity_per_shard = 2,
        .timer_capacity_per_shard = 1,
    });
    REQUIRE(scheduler.start());
    const auto inbox = std::make_shared<HandshakeInbox>(1);
    const auto source =
        std::make_shared<ListenerHandshakeEventSource>(scheduler, 0U, inbox);
    const auto observation = std::make_shared<EventReadyObservation>();
    REQUIRE(source->set_event_ready_handler(observe_event_ready, observation));
    REQUIRE_EQ(source->start(), ListenerHandshakeDispatchStatus::completed);

    REQUIRE(inbox->push(HandshakeEnvelope {}));
    {
        std::unique_lock lock(observation->mutex);
        REQUIRE(observation->changed.wait_for(
            lock, std::chrono::seconds {2}, [&observation] {
                return observation->calls != 0U;
            }));
    }
    ListenerHandshakeSourceEvent event;
    REQUIRE(source->try_pop(event));
    REQUIRE_EQ(event.kind, ListenerHandshakeSourceEventKind::inbox_ready);

    source->close();
    REQUIRE(source->try_pop(event));
    REQUIRE_EQ(event.kind, ListenerHandshakeSourceEventKind::closed);
    source->clear_event_ready_handler();
    source->stop();
    scheduler.stop();
}

TEST(listener_handshake_inbox_uses_weak_readiness_ownership)
{
    HandshakeInbox inbox {1};
    auto counter = std::make_shared<ReadinessCounter>();
    REQUIRE(inbox.set_ready_handler(count_readiness, counter));
    counter.reset();

    REQUIRE(inbox.push(HandshakeEnvelope {}));
    inbox.clear_ready_handler();

    const auto close_counter = std::make_shared<ReadinessCounter>();
    REQUIRE(inbox.set_ready_handler(count_readiness, close_counter));
    REQUIRE_EQ(close_counter->value, 1U);
    inbox.close();
    REQUIRE_EQ(close_counter->value, 2U);
    inbox.close();
    REQUIRE_EQ(close_counter->value, 2U);
}
