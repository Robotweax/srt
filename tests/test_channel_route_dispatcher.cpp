#include "test.hpp"
#include "compat/connection_datagram_dispatcher.hpp"
#include "compat/transport_runtime.hpp"
#include "robotweax/srt/control.hpp"
#include "srt/srt.h"
#include "compat/runtime_work_executor.hpp"
#include "compat/socket_registry.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <limits>
#include <thread>
#include <utility>

using namespace robotweax::srt;
using namespace robotweax::srt::compat;
namespace {
constexpr Ipv4Endpoint sink_peer {.address = {192, 0, 2, 96}, .port = 14906};
struct SinkGate {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool open = false;
    static void block(void* pointer) noexcept
    {
        auto& gate = *static_cast<SinkGate*>(pointer);
        std::unique_lock lock(gate.mutex);
        gate.entered = true;
        gate.changed.notify_all();
        gate.changed.wait(lock, [&] {
            return gate.open;
        });
    }
    void wait()
    {
        std::unique_lock lock(mutex);
        REQUIRE(changed.wait_for(lock, std::chrono::seconds {2}, [&] {
            return entered;
        }));
    }
    void release() noexcept
    {
        std::lock_guard lock(mutex);
        open = true;
        changed.notify_all();
    }
};
struct SinkRelease {
    std::shared_ptr<SinkGate> gate;
    ~SinkRelease()
    {
        gate->release();
    }
};
struct SinkWire {
    std::array<std::byte, 1500> bytes {};
    std::size_t size = 0;
    std::span<const std::byte> view() const
    {
        return std::span {bytes}.first(size);
    }
};
SinkWire sink_data(std::uint32_t offset)
{
    MutablePacketView packet;
    packet.kind = PacketKind::data;
    packet.data.sequence = SequenceNumber {1000 + offset};
    packet.data.destination_socket_id = 700;
    packet.data.message_number = 7 + offset;
    packet.data.boundary = MessageBoundary::solo;
    const std::array payload {static_cast<std::byte>(offset + 1)};
    packet.payload = payload;
    SinkWire wire;
    const auto encoded = encode_packet(packet, wire.bytes);
    REQUIRE(encoded);
    wire.size = encoded.bytes_written;
    return wire;
}
SinkWire sink_replay(std::uint32_t cookie = 0x12345678)
{
    HandshakeAction action;
    action.kind = HandshakeActionKind::send;
    action.packet.version = handshake_version_5;
    action.packet.request = HandshakeRequest::conclusion;
    action.packet.socket_id = 90;
    action.packet.syn_cookie = cookie;
    SinkWire wire;
    const auto encoded = encode_handshake_datagram(
        action, PacketTimestamp {100}, 700, wire.bytes);
    REQUIRE(encoded);
    wire.size = encoded.bytes_written;
    return wire;
}
std::uint64_t sink_runtime_now(void*) noexcept
{
    return 1000;
}
struct SinkFixture {
    std::shared_ptr<RuntimeScheduler> scheduler =
        std::make_shared<RuntimeScheduler>(
            RuntimeScheduler::Configuration {.shard_count = 2,
                .queue_capacity_per_shard = 1,
                .timer_capacity_per_shard = 1,
                .service_capacity_per_shard = 1});
    std::shared_ptr<DatagramChannel> channel =
        std::make_shared<DatagramChannel>();
    std::shared_ptr<DatagramStorageBudget> budget =
        std::make_shared<DatagramStorageBudget>(
            *ConnectionDatagramInbox::storage_bytes(16));
    std::atomic<std::size_t> replay_responses {0};
    std::mutex response_mutex;
    std::condition_variable response_changed;
    std::shared_ptr<ConnectionRuntime> runtime;
    explicit SinkFixture(
        std::shared_ptr<SinkGate> receive_gate = nullptr, bool replay = true)
    {
        REQUIRE(scheduler->start());
        channel->set_send_hook_for_testing(
            [](std::span<const std::byte> bytes, IpEndpoint,
                void* pointer) noexcept {
                auto& self = *static_cast<SinkFixture*>(pointer);
                const auto decoded = decode_packet(bytes);
                if (decoded && decoded.packet.kind == PacketKind::control
                    && decoded.packet.control.type == ControlType::handshake) {
                    std::lock_guard lock(self.response_mutex);
                    self.replay_responses.fetch_add(1);
                    self.response_changed.notify_all();
                }
                return UdpIoResult {.bytes_transferred = bytes.size()};
            },
            this);
        runtime = make_runtime(90, receive_gate.get(), replay);
    }
    std::shared_ptr<ConnectionRuntime> make_runtime(std::uint32_t peer_id = 90,
        SinkGate* receive_gate = nullptr, bool replay = true)
    {
        SocketOptions options;
        REQUIRE_EQ(options.set(SocketOption::tsbpd_mode, 0), Error::none);
        HandshakeAction response;
        response.kind = HandshakeActionKind::send;
        response.packet.version = handshake_version_5;
        response.packet.request = HandshakeRequest::conclusion;
        response.packet.socket_id = 700;
        response.packet.syn_cookie = 0x12345678;
        return std::make_shared<ConnectionRuntime>(
            ConnectionRuntime::Configuration {.channel = channel,
                .peer = sink_peer,
                .peer_socket_id = peer_id,
                .initial_sequence = SequenceNumber {1000},
                .options = options,
                .origin = ConnectionRuntime::Clock::now(),
                .handshake_replay_response = response,
                .handshake_replay_enabled = replay,
                .now_function = sink_runtime_now,
                .receive_pop_hook_for_testing =
                    receive_gate == nullptr ? nullptr : SinkGate::block,
                .receive_pop_context_for_testing = receive_gate});
    }
    std::shared_ptr<ConnectionDatagramDispatcher> dispatcher(
        std::size_t capacity = 16, std::shared_ptr<SinkGate> pop_gate = nullptr)
    {
        auto result = ConnectionDatagramDispatcher::create(runtime, scheduler,
            budget, 1, sink_peer, {.capacity = capacity, .control_reserve = 1},
            {.turn_budget = 2,
                .after_pop_for_testing =
                    pop_gate == nullptr ? nullptr : SinkGate::block,
                .after_pop_context_for_testing = pop_gate});
        REQUIRE(result != nullptr);
        return result;
    }
    RuntimePollResult ingress(const SinkWire& wire, IpEndpoint peer = sink_peer)
    {
        bool received = false;
        auto receive = [&](std::span<std::byte> bytes) noexcept {
            if (received) {
                return UdpIoResult {.error = Error::would_block};
            }
            received = true;
            std::copy(wire.view().begin(), wire.view().end(), bytes.begin());
            return UdpIoResult {.bytes_transferred = wire.size, .peer = peer};
        };
        return channel->run_once_for_testing(receive);
    }
    void wait_replays(std::size_t count)
    {
        std::unique_lock lock(response_mutex);
        REQUIRE(response_changed.wait_for(lock, std::chrono::seconds {2}, [&] {
            return replay_responses.load() >= count;
        }));
    }
    ~SinkFixture()
    {
        scheduler->stop();
    }
};
void sink_receive(
    const std::shared_ptr<ConnectionRuntime>& runtime, std::byte expected)
{
    std::array<std::byte, 8> bytes {};
    const auto received = runtime->receive_message(bytes, true, 2000);
    REQUIRE_EQ(received.status, MessageIoStatus::success);
    REQUIRE_EQ(received.bytes, 1U);
    REQUIRE_EQ(bytes.front(), expected);
}
void sink_block_worker(
    SinkFixture& fixture, const std::shared_ptr<SinkGate>& gate)
{
    REQUIRE_EQ(fixture.scheduler->submit(
                   1, {.function = SinkGate::block, .context = gate}),
        RuntimeScheduler::SubmitStatus::accepted);
    gate->wait();
}
}

TEST(channel_route_dispatcher_copies_actual_ingress_to_reserved_service)
{
    SinkFixture fixture;
    auto gate = std::make_shared<SinkGate>();
    SinkRelease release {gate};
    sink_block_worker(fixture, gate);
    auto dispatcher = fixture.dispatcher();
    REQUIRE(
        fixture.channel->register_connection(700, fixture.runtime, dispatcher));
    auto wire = sink_data(0);
    (void)fixture.ingress(wire);
    wire.bytes.fill(std::byte {0xff});
    REQUIRE_EQ(fixture.runtime->buffer_packet_counts().available_receive, 0U);
    REQUIRE_EQ(dispatcher->inbox()->snapshot().queued, 1U);
    gate->release();
    sink_receive(fixture.runtime, std::byte {1});
    fixture.scheduler->stop();
    REQUIRE_EQ(dispatcher->snapshot().dispatched_datagrams, 1U);
}

TEST(
    channel_route_dispatcher_atomic_promotion_preserves_large_prefix_and_poll_gate)
{
    SinkFixture fixture;
    auto gate = std::make_shared<SinkGate>();
    SinkRelease release {gate};
    sink_block_worker(fixture, gate);
    auto prefix = std::make_shared<DatagramInbox>(9);
    REQUIRE(fixture.channel->register_setup_inbox(700, sink_peer, prefix, 90));
    for (std::uint32_t index = 0; index < 9; ++index) {
        REQUIRE(prefix->push(sink_data(index).view(), sink_peer));
    }
    auto dispatcher = fixture.dispatcher(2);
    REQUIRE(fixture.channel->promote_setup_connection(
        700, prefix, fixture.runtime, dispatcher));
    REQUIRE(!dispatcher->setup_prefix_complete());
    DatagramEnvelope envelope;
    REQUIRE_EQ(prefix->pop_for(envelope, std::chrono::milliseconds {0}),
        InboxPopStatus::closed);
    const auto pending = fixture.ingress(sink_data(9));
    REQUIRE(!pending.immediate_work);
    REQUIRE(pending.next_work_delay.has_value());
    REQUIRE(!pending.receive_wait_safe);
    prefix->close();
    REQUIRE(!prefix->push(sink_data(10).view(), sink_peer));
    REQUIRE_EQ(dispatcher->inbox()->snapshot().data_rejections, 1U);
    gate->release();
    for (std::uint32_t index = 0; index < 10; ++index) {
        sink_receive(fixture.runtime, static_cast<std::byte>(index + 1));
    }
    fixture.scheduler->stop();
    REQUIRE(dispatcher->setup_prefix_complete());
    REQUIRE_EQ(dispatcher->snapshot().dispatched_datagrams, 10U);
    REQUIRE_EQ(dispatcher->snapshot().maximum_turn_datagrams, 2U);
    fixture.channel->unregister_connection(700);
}

TEST(
    channel_route_dispatcher_promotion_and_pending_poll_do_not_hold_runtime_mutex)
{
    auto runtime_gate = std::make_shared<SinkGate>();
    SinkFixture fixture(runtime_gate);
    auto dispatcher = fixture.dispatcher();
    auto prefix = std::make_shared<DatagramInbox>(1);
    REQUIRE(prefix->push(sink_data(1).view(), sink_peer));
    REQUIRE(fixture.channel->register_setup_inbox(700, sink_peer, prefix, 90));
    const auto data = sink_data(0);
    fixture.runtime->process_packet(
        decode_packet(data.view()).packet, sink_peer);
    std::future<MessageIoResult> receive;
    std::future<bool> promotion;
    std::future<RuntimePollResult> poll;
    SinkRelease release {runtime_gate};
    receive = std::async(std::launch::async, [&] {
        std::array<std::byte, 8> bytes {};
        return fixture.runtime->receive_message(bytes, false, 0);
    });
    runtime_gate->wait();
    promotion = std::async(std::launch::async, [&] {
        return fixture.channel->promote_setup_connection(
            700, prefix, fixture.runtime, dispatcher);
    });
    REQUIRE_EQ(promotion.wait_for(std::chrono::seconds {2}),
        std::future_status::ready);
    REQUIRE(promotion.get());
    poll = std::async(std::launch::async, [&] {
        return fixture.channel->poll_connections_for_testing();
    });
    REQUIRE_EQ(
        poll.wait_for(std::chrono::seconds {2}), std::future_status::ready);
    REQUIRE(!poll.get().immediate_work);
    REQUIRE(fixture.channel->register_setup_inbox(
        701, sink_peer, std::make_shared<DatagramInbox>(1), 91));
    runtime_gate->release();
    REQUIRE_EQ(receive.get().status, MessageIoStatus::success);
    sink_receive(fixture.runtime, std::byte {2});
    fixture.scheduler->stop();
    fixture.channel->unregister_connection(700);
}

TEST(channel_route_dispatcher_rejects_wrong_runtime_peer_and_channel)
{
    SinkFixture fixture;
    SinkFixture foreign;
    auto dispatcher = fixture.dispatcher();
    REQUIRE(!fixture.channel->register_connection(
        700, foreign.runtime, dispatcher));
    REQUIRE(!foreign.channel->register_connection(
        700, fixture.runtime, dispatcher));
    auto wrong_peer = sink_peer;
    ++wrong_peer.port;
    auto prefix = std::make_shared<DatagramInbox>(1);
    REQUIRE(prefix->push(sink_data(0).view(), sink_peer));
    REQUIRE(fixture.channel->register_setup_inbox(700, wrong_peer, prefix, 90));
    REQUIRE(!fixture.channel->promote_setup_connection(
        700, prefix, fixture.runtime, dispatcher));
    DatagramEnvelope envelope;
    REQUIRE_EQ(prefix->pop_for(envelope, std::chrono::milliseconds {0}),
        InboxPopStatus::received);
    REQUIRE(
        fixture.channel->register_connection(700, fixture.runtime, dispatcher));
    (void)fixture.ingress(sink_data(0));
    sink_receive(fixture.runtime, std::byte {1});
    fixture.channel->unregister_connection(700);
}

TEST(
    channel_route_dispatcher_rejects_runtime_alias_without_breaking_first_route)
{
    SinkFixture fixture(nullptr, false);
    auto gate = std::make_shared<SinkGate>();
    SinkRelease release {gate};
    sink_block_worker(fixture, gate);
    auto dispatcher = fixture.dispatcher();
    REQUIRE(
        fixture.channel->register_connection(700, fixture.runtime, dispatcher));
    REQUIRE(!fixture.channel->register_connection(
        701, fixture.runtime, dispatcher));
    REQUIRE(!fixture.channel->register_connection(701, fixture.runtime));
    REQUIRE(fixture.runtime->accepts_datagrams());
    (void)fixture.ingress(sink_data(0));
    gate->release();
    sink_receive(fixture.runtime, std::byte {1});
    fixture.channel->unregister_connection(700);
    REQUIRE(!fixture.channel->register_connection(
        700, fixture.runtime, dispatcher));
    auto replacement = fixture.make_runtime(91, nullptr, false);
    REQUIRE(fixture.channel->register_connection(700, replacement));
    fixture.channel->unregister_connection(700);
}

TEST(
    channel_route_dispatcher_used_binding_rolls_back_route_without_sealing_setup)
{
    SinkFixture fixture;
    auto gate = std::make_shared<SinkGate>();
    SinkRelease release {gate};
    sink_block_worker(fixture, gate);
    auto dispatcher = fixture.dispatcher();
    REQUIRE_EQ(dispatcher->publish(dispatcher->inbox()->token(),
                   sink_data(0).view(), sink_peer),
        ConnectionDatagramInbox::Status::accepted);
    auto prefix = std::make_shared<DatagramInbox>(1);
    REQUIRE(prefix->push(sink_data(1).view(), sink_peer));
    REQUIRE(fixture.channel->register_setup_inbox(700, sink_peer, prefix, 90));
    REQUIRE(!fixture.channel->promote_setup_connection(
        700, prefix, fixture.runtime, dispatcher));
    DatagramEnvelope envelope;
    REQUIRE_EQ(prefix->pop_for(envelope, std::chrono::milliseconds {0}),
        InboxPopStatus::received);
    (void)fixture.ingress(sink_data(2));
    REQUIRE_EQ(prefix->pop_for(envelope, std::chrono::milliseconds {0}),
        InboxPopStatus::received);
    REQUIRE(fixture.runtime->accepts_datagrams());
    gate->release();
    sink_receive(fixture.runtime, std::byte {1});
}

TEST(
    channel_route_dispatcher_activation_failure_restores_setup_route_and_prefix)
{
    SinkFixture fixture;
    auto dispatcher = fixture.dispatcher();
    auto prefix = std::make_shared<DatagramInbox>(2);
    REQUIRE(prefix->push(sink_data(0).view(), sink_peer));
    REQUIRE(fixture.channel->register_setup_inbox(700, sink_peer, prefix, 90));
    fixture.scheduler->stop();
    REQUIRE(!fixture.channel->promote_setup_connection(
        700, prefix, fixture.runtime, dispatcher));
    DatagramEnvelope envelope;
    REQUIRE_EQ(prefix->pop_for(envelope, std::chrono::milliseconds {0}),
        InboxPopStatus::received);
    (void)fixture.ingress(sink_data(1));
    REQUIRE_EQ(prefix->pop_for(envelope, std::chrono::milliseconds {0}),
        InboxPopStatus::received);
    REQUIRE(!fixture.runtime->accepts_datagrams());
    auto replacement = fixture.make_runtime();
    REQUIRE(fixture.channel->register_connection(700, replacement));
    fixture.channel->unregister_connection(700);
}

TEST(
    channel_route_dispatcher_unregister_prevents_old_publishers_reaching_reused_id)
{
    SinkFixture fixture;
    auto gate = std::make_shared<SinkGate>();
    SinkRelease release {gate};
    sink_block_worker(fixture, gate);
    auto prefix = std::make_shared<DatagramInbox>(1);
    REQUIRE(prefix->push(sink_data(0).view(), sink_peer));
    REQUIRE(fixture.channel->register_setup_inbox(700, sink_peer, prefix, 90));
    auto dispatcher = fixture.dispatcher(2);
    const auto old_token = dispatcher->inbox()->token();
    REQUIRE(fixture.channel->promote_setup_connection(
        700, prefix, fixture.runtime, dispatcher));
    fixture.channel->unregister_connection(700);
    REQUIRE(!prefix->push(sink_data(1).view(), sink_peer));
    auto old_runtime = fixture.runtime;
    fixture.runtime = fixture.make_runtime();
    auto next = ConnectionDatagramDispatcher::create(fixture.runtime,
        fixture.scheduler, fixture.budget, 0, sink_peer,
        {.capacity = 2, .control_reserve = 1}, {.turn_budget = 2});
    REQUIRE(next != nullptr);
    REQUIRE(fixture.channel->register_connection(700, fixture.runtime, next));
    REQUIRE_EQ(next->publish(old_token, sink_data(0).view(), sink_peer),
        ConnectionDatagramInbox::Status::stale);
    REQUIRE(!prefix->push(sink_data(0).view(), sink_peer));
    (void)fixture.ingress(sink_data(0));
    sink_receive(fixture.runtime, std::byte {1});
    REQUIRE_EQ(old_runtime->buffer_packet_counts().available_receive, 0U);
    gate->release();
    fixture.scheduler->stop();
    REQUIRE_EQ(old_runtime->buffer_packet_counts().available_receive, 0U);
    fixture.channel->unregister_connection(700);
}

TEST(
    channel_route_dispatcher_queues_valid_replays_and_preserves_invalid_listener_fallback)
{
    SinkFixture fixture;
    auto listener = std::make_shared<HandshakeInbox>(4);
    REQUIRE(fixture.channel->set_listener_inbox(listener));
    auto prefix = std::make_shared<DatagramInbox>(1);
    REQUIRE(prefix->push(sink_data(0).view(), sink_peer));
    REQUIRE(fixture.channel->register_setup_inbox(700, sink_peer, prefix, 90));
    auto gate = std::make_shared<SinkGate>();
    SinkRelease release {gate};
    auto dispatcher = fixture.dispatcher(2, gate);
    REQUIRE(fixture.channel->promote_setup_connection(
        700, prefix, fixture.runtime, dispatcher));
    gate->wait();
    REQUIRE(!dispatcher->setup_prefix_complete());
    (void)fixture.ingress(sink_replay(0x87654321));
    HandshakeEnvelope envelope;
    REQUIRE_EQ(listener->pop_for(envelope, std::chrono::milliseconds {0}),
        InboxPopStatus::received);
    REQUIRE_EQ(envelope.message.packet.syn_cookie, 0x87654321U);
    REQUIRE(!fixture.channel->replay_established_handshake(envelope));
    (void)fixture.ingress(sink_data(1));
    (void)fixture.ingress(sink_replay());
    (void)fixture.ingress(sink_replay());
    REQUIRE_EQ(dispatcher->inbox()->snapshot().control_rejections, 1U);
    REQUIRE_EQ(listener->pop_for(envelope, std::chrono::milliseconds {0}),
        InboxPopStatus::timeout);
    REQUIRE_EQ(fixture.replay_responses.load(), 0U);
    const auto wire = sink_replay();
    const auto decoded = decode_handshake_datagram(wire.view());
    REQUIRE(decoded);
    REQUIRE(fixture.channel->replay_established_handshake(
        {.message = decoded.message,
            .control = decoded.control,
            .peer = sink_peer}));
    REQUIRE_EQ(dispatcher->inbox()->snapshot().control_rejections, 2U);
    gate->release();
    sink_receive(fixture.runtime, std::byte {1});
    sink_receive(fixture.runtime, std::byte {2});
    // Replay is the third copy and needs a second service turn.
    fixture.wait_replays(1);
    fixture.scheduler->stop();
    REQUIRE_EQ(dispatcher->snapshot().dispatched_datagrams, 3U);
    REQUIRE(dispatcher->setup_prefix_complete());
    fixture.channel->unregister_connection(700);
}

TEST(channel_route_dispatcher_ingress_wake_failure_marks_runtime_terminal)
{
    SinkFixture fixture;
    auto dispatcher = fixture.dispatcher();
    REQUIRE(
        fixture.channel->register_connection(700, fixture.runtime, dispatcher));
    fixture.scheduler->stop();
    (void)fixture.ingress(sink_data(0));
    REQUIRE(!fixture.runtime->accepts_datagrams());
    REQUIRE(dispatcher->inbox()->snapshot().closed);
    REQUIRE_EQ(fixture.runtime->buffer_packet_counts().available_receive, 0U);
    REQUIRE(dispatcher->setup_prefix_complete());
    fixture.channel->unregister_connection(700);
}

TEST(channel_route_dispatcher_decoded_envelope_replay_waits_for_prefix)
{
    SinkFixture fixture;
    auto prefix = std::make_shared<DatagramInbox>(1);
    REQUIRE(prefix->push(sink_data(0).view(), sink_peer));
    REQUIRE(fixture.channel->register_setup_inbox(700, sink_peer, prefix, 90));
    auto gate = std::make_shared<SinkGate>();
    SinkRelease release {gate};
    auto dispatcher = fixture.dispatcher(16, gate);
    REQUIRE(fixture.channel->promote_setup_connection(
        700, prefix, fixture.runtime, dispatcher));
    gate->wait();
    const auto wire = sink_replay();
    const auto decoded = decode_handshake_datagram(wire.view());
    REQUIRE(decoded);
    auto message = decoded.message;
    // The established handler consumes base replay identity, including when
    // the decoded caller envelope also records an unknown extension.
    message.has_unknown_extension = true;
    REQUIRE(fixture.channel->replay_established_handshake(
        {.message = message, .control = decoded.control, .peer = sink_peer}));
    REQUIRE_EQ(dispatcher->inbox()->snapshot().queued, 1U);
    REQUIRE_EQ(fixture.replay_responses.load(), 0U);
    gate->release();
    sink_receive(fixture.runtime, std::byte {1});
    fixture.wait_replays(1);
    fixture.scheduler->stop();
    REQUIRE_EQ(dispatcher->snapshot().dispatched_datagrams, 2U);
    fixture.channel->unregister_connection(700);
}

TEST(channel_route_dispatcher_registration_activation_failure_unlinks_poll_ring)
{
    SinkFixture fixture;
    auto dispatcher = fixture.dispatcher();
    fixture.scheduler->stop();
    REQUIRE(!fixture.channel->register_connection(
        700, fixture.runtime, dispatcher));
    REQUIRE(!fixture.runtime->accepts_datagrams());
    auto replacement = fixture.make_runtime();
    REQUIRE(fixture.channel->register_connection(700, replacement));
    (void)fixture.channel->poll_connections_for_testing();
    fixture.channel->unregister_connection(700);
    (void)fixture.channel->poll_connections_for_testing();
}

TEST(
    channel_route_dispatcher_cold_promotion_rejects_closed_setup_without_claiming_service)
{
    SinkFixture fixture;
    auto prefix = std::make_shared<DatagramInbox>(1);
    prefix->close();
    REQUIRE(fixture.channel->register_setup_inbox(700, sink_peer, prefix, 90));
    auto dispatcher = fixture.dispatcher();
    REQUIRE(!fixture.channel->promote_setup_connection(
        700, prefix, fixture.runtime, dispatcher));
    REQUIRE(fixture.runtime->accepts_datagrams());
    REQUIRE(
        fixture.channel->register_connection(700, fixture.runtime, dispatcher));
    (void)fixture.ingress(sink_data(0));
    sink_receive(fixture.runtime, std::byte {1});
    fixture.channel->unregister_connection(700);
}

namespace {
struct SinkRouteProbe {
    std::mutex mutex;
    std::condition_variable changed;
    bool requested = false;
    bool done = false;
    bool tested = false;
    bool progressed_during_callback = false;
    void request() noexcept
    {
        std::unique_lock lock(mutex);
        if (tested) {
            return;
        }
        tested = true;
        requested = true;
        changed.notify_all();
        progressed_during_callback =
            changed.wait_for(lock, std::chrono::seconds {2}, [&] {
                return done;
            });
    }
    bool progressed()
    {
        std::lock_guard lock(mutex);
        return progressed_during_callback;
    }
};
struct SinkProbeContext {
    std::shared_ptr<SinkRouteProbe> probe;
    bool on_destroy = false;
    static std::uint64_t now(void* pointer) noexcept
    {
        static_cast<SinkProbeContext*>(pointer)->probe->request();
        return 1;
    }
    ~SinkProbeContext()
    {
        if (on_destroy) {
            probe->request();
        }
    }
};
struct SinkProbeWorker {
    std::shared_ptr<SinkRouteProbe> probe;
    std::thread worker;
    SinkProbeWorker(std::shared_ptr<SinkRouteProbe> state,
        std::shared_ptr<DatagramChannel> channel)
        : probe(std::move(state))
        , worker([state = probe, channel = std::move(channel)] {
            {
                std::unique_lock lock(state->mutex);
                state->changed.wait(lock, [&] {
                    return state->requested;
                });
            }
            (void)channel->register_setup_inbox(
                701, sink_peer, std::make_shared<DatagramInbox>(1), 91);
            std::lock_guard lock(state->mutex);
            state->done = true;
            state->changed.notify_all();
        })
    {
    }
    ~SinkProbeWorker()
    {
        {
            std::lock_guard lock(probe->mutex);
            probe->requested = true;
            probe->changed.notify_all();
        }
        worker.join();
    }
};
}

TEST(channel_route_dispatcher_publication_clock_runs_outside_route_lock)
{
    SinkFixture fixture;
    auto gate = std::make_shared<SinkGate>();
    SinkRelease release {gate};
    sink_block_worker(fixture, gate);
    auto probe = std::make_shared<SinkRouteProbe>();
    auto context = std::make_shared<SinkProbeContext>();
    context->probe = probe;
    auto dispatcher = ConnectionDatagramDispatcher::create(fixture.runtime,
        fixture.scheduler, fixture.budget, 1, sink_peer,
        {.capacity = 16, .control_reserve = 1},
        {.turn_budget = 2,
            .now_function = SinkProbeContext::now,
            .now_context = context});
    REQUIRE(dispatcher != nullptr);
    REQUIRE(
        fixture.channel->register_connection(700, fixture.runtime, dispatcher));
    SinkProbeWorker worker {probe, fixture.channel};
    (void)fixture.ingress(sink_data(0));
    REQUIRE(probe->progressed());
    gate->release();
    sink_receive(fixture.runtime, std::byte {1});
    fixture.channel->unregister_connection(700);
}

TEST(
    channel_route_dispatcher_last_service_context_is_destroyed_outside_route_lock)
{
    SinkFixture fixture;
    auto probe = std::make_shared<SinkRouteProbe>();
    auto context = std::make_shared<SinkProbeContext>();
    context->probe = probe;
    context->on_destroy = true;
    auto dispatcher = ConnectionDatagramDispatcher::create(fixture.runtime,
        fixture.scheduler, fixture.budget, 1, sink_peer,
        {.capacity = 16, .control_reserve = 1},
        {.turn_budget = 2, .now_context = context});
    REQUIRE(dispatcher != nullptr);
    REQUIRE(
        fixture.channel->register_connection(700, fixture.runtime, dispatcher));
    fixture.scheduler->stop();
    context.reset();
    dispatcher.reset();
    SinkProbeWorker worker {probe, fixture.channel};
    fixture.channel->unregister_connection(700);
    REQUIRE(probe->progressed());
}

namespace {
struct SinkNeutralSend {
    std::shared_ptr<DatagramChannel> channel;
    ~SinkNeutralSend()
    {
        channel->set_send_hook_for_testing(
            [](std::span<const std::byte> bytes, IpEndpoint, void*) noexcept {
                return UdpIoResult {.bytes_transferred = bytes.size()};
            },
            nullptr);
    }
};
std::uint64_t ingress_idle_now(void* pointer) noexcept
{
    return static_cast<std::atomic<std::uint64_t>*>(pointer)->load();
}
void ingress_idle_runtime(SinkFixture& fixture, std::atomic<std::uint64_t>& now,
    std::uint32_t timeout_ms = 5, SinkGate* receive_gate = nullptr)
{
    SocketOptions options;
    REQUIRE_EQ(options.set(SocketOption::tsbpd_mode, 0), Error::none);
    fixture.runtime = std::make_shared<ConnectionRuntime>(
        ConnectionRuntime::Configuration {.channel = fixture.channel,
            .peer = sink_peer,
            .peer_socket_id = 90,
            .initial_sequence = SequenceNumber {1000},
            .options = options,
            .origin = ConnectionRuntime::Clock::now(),
            .peer_idle_timeout_milliseconds = timeout_ms,
            .now_function = ingress_idle_now,
            .now_context = &now,
            .receive_pop_hook_for_testing =
                receive_gate == nullptr ? nullptr : SinkGate::block,
            .receive_pop_context_for_testing = receive_gate});
}
}

TEST(channel_ingress_idle_queued_data_gets_bounded_processing_grace)
{
    std::atomic<std::uint64_t> now {1000};
    SinkFixture fixture;
    ingress_idle_runtime(fixture, now);
    auto gate = std::make_shared<SinkGate>();
    SinkRelease release {gate};
    sink_block_worker(fixture, gate);
    auto dispatcher = fixture.dispatcher();
    REQUIRE(
        fixture.channel->register_connection(700, fixture.runtime, dispatcher));
    (void)fixture.ingress(sink_data(0));
    now.store(5001);
    const auto waiting = fixture.channel->poll_connections_for_testing(
        ConnectionRuntime::Clock::time_point {});
    REQUIRE(fixture.runtime->accepts_datagrams());
    REQUIRE(!waiting.immediate_work);
    REQUIRE_EQ(waiting.next_work_delay, std::chrono::microseconds {2000});
    gate->release();
    sink_receive(fixture.runtime, std::byte {1});
    fixture.scheduler->stop();
    REQUIRE(!dispatcher->inbox()->snapshot().in_flight);
    now.store(6000);
    (void)fixture.channel->poll_connections_for_testing(
        ConnectionRuntime::Clock::time_point {});
    REQUIRE(fixture.runtime->accepts_datagrams());
    fixture.channel->unregister_connection(700);
}

TEST(channel_ingress_idle_popped_copy_stays_pending_until_protocol_completion)
{
    std::atomic<std::uint64_t> now {1000};
    SinkFixture fixture;
    ingress_idle_runtime(fixture, now);
    auto gate = std::make_shared<SinkGate>();
    SinkRelease release {gate};
    auto dispatcher = fixture.dispatcher(16, gate);
    REQUIRE(
        fixture.channel->register_connection(700, fixture.runtime, dispatcher));
    (void)fixture.ingress(sink_data(0));
    gate->wait();
    REQUIRE_EQ(dispatcher->inbox()->snapshot().queued, 0U);
    REQUIRE(dispatcher->inbox()->snapshot().in_flight);
    DatagramEnvelope envelope;
    REQUIRE_EQ(
        dispatcher->inbox()->pop(dispatcher->inbox()->token(), envelope, 1),
        ConnectionDatagramInbox::Status::busy);
    now.store(6001);
    // Freeze both clocks: Windows requests immediate continuation below 1 ms,
    // so host elapsed time must not shorten this exact 1 ms grace remainder.
    const auto waiting = fixture.channel->poll_connections_for_testing(
        ConnectionRuntime::Clock::time_point {});
    REQUIRE(!waiting.immediate_work);
    REQUIRE_EQ(waiting.next_work_delay, std::chrono::microseconds {1000});
    REQUIRE(fixture.runtime->accepts_datagrams());
    dispatcher->close();
    REQUIRE(dispatcher->inbox()->snapshot().in_flight);
    REQUIRE(!dispatcher->quiescent());
    gate->release();
    fixture.scheduler->stop();
    REQUIRE(!dispatcher->inbox()->snapshot().in_flight);
    REQUIRE(dispatcher->quiescent());
    REQUIRE_EQ(fixture.runtime->buffer_packet_counts().available_receive, 0U);
    fixture.channel->unregister_connection(700);
}

TEST(channel_ingress_idle_grace_does_not_slide_with_more_or_rejected_ingress)
{
    std::atomic<std::uint64_t> now {1000};
    SinkFixture fixture;
    ingress_idle_runtime(fixture, now);
    auto gate = std::make_shared<SinkGate>();
    SinkRelease release {gate};
    sink_block_worker(fixture, gate);
    auto dispatcher = fixture.dispatcher(2);
    REQUIRE(
        fixture.channel->register_connection(700, fixture.runtime, dispatcher));
    (void)fixture.ingress(sink_data(0));
    now.store(5001);
    const auto first = fixture.channel->poll_connections_for_testing(
        ConnectionRuntime::Clock::time_point {});
    REQUIRE_EQ(first.next_work_delay, std::chrono::microseconds {2000});
    now.store(6000);
    (void)fixture.ingress(sink_data(1));
    REQUIRE_EQ(dispatcher->inbox()->snapshot().data_rejections, 1U);
    const auto second = fixture.channel->poll_connections_for_testing(
        ConnectionRuntime::Clock::time_point {});
    REQUIRE_EQ(second.next_work_delay, std::chrono::microseconds {1001});
    REQUIRE(fixture.runtime->accepts_datagrams());
    // Further accepted control traffic cannot move the last real activity time.
    REQUIRE_EQ(dispatcher->publish(dispatcher->inbox()->token(),
                   sink_replay().view(), sink_peer),
        ConnectionDatagramInbox::Status::accepted);
    now.store(7001);
    (void)fixture.channel->poll_connections_for_testing(
        ConnectionRuntime::Clock::time_point {});
    REQUIRE(!fixture.runtime->accepts_datagrams());
    gate->release();
    fixture.scheduler->stop();
    REQUIRE_EQ(fixture.runtime->buffer_packet_counts().available_receive, 0U);
    fixture.channel->unregister_connection(700);
}

TEST(channel_ingress_idle_invalid_and_stale_admission_do_not_postpone_timeout)
{
    std::atomic<std::uint64_t> now {1000};
    SinkFixture fixture;
    ingress_idle_runtime(fixture, now);
    auto gate = std::make_shared<SinkGate>();
    SinkRelease release {gate};
    sink_block_worker(fixture, gate);
    auto dispatcher = fixture.dispatcher();
    REQUIRE(
        fixture.channel->register_connection(700, fixture.runtime, dispatcher));
    auto stale = dispatcher->inbox()->token();
    ++stale.incarnation;
    REQUIRE_EQ(
        dispatcher->inbox()->publish(stale, sink_data(0).view(), sink_peer, 1),
        ConnectionDatagramInbox::Status::stale);
    auto wrong_peer = sink_peer;
    ++wrong_peer.port;
    REQUIRE_EQ(dispatcher->inbox()->publish(dispatcher->inbox()->token(),
                   sink_data(0).view(), wrong_peer, 1),
        ConnectionDatagramInbox::Status::invalid);
    const std::array<std::byte, 3> invalid {};
    REQUIRE_EQ(dispatcher->inbox()->publish(
                   dispatcher->inbox()->token(), invalid, sink_peer, 1),
        ConnectionDatagramInbox::Status::invalid);
    now.store(6001);
    (void)fixture.channel->poll_connections_for_testing();
    REQUIRE(!fixture.runtime->accepts_datagrams());
    REQUIRE_EQ(dispatcher->inbox()->snapshot().highwater, 0U);
    fixture.channel->unregister_connection(700);
}

namespace {
struct IdlePollClock {
    std::atomic<std::uint64_t> now {1000};
    std::atomic<bool> pause {false};
    std::shared_ptr<SinkGate> gate = std::make_shared<SinkGate>();
    static std::uint64_t read(void* pointer) noexcept
    {
        auto& self = *static_cast<IdlePollClock*>(pointer);
        if (self.pause.exchange(false)) {
            SinkGate::block(self.gate.get());
        }
        return self.now.load();
    }
};
}

TEST(channel_ingress_idle_raw_publication_cannot_cross_runtime_timeout_fence)
{
    IdlePollClock clock;
    SinkFixture fixture;
    SocketOptions options;
    REQUIRE_EQ(options.set(SocketOption::tsbpd_mode, 0), Error::none);
    fixture.runtime = std::make_shared<ConnectionRuntime>(
        ConnectionRuntime::Configuration {.channel = fixture.channel,
            .peer = sink_peer,
            .peer_socket_id = 90,
            .initial_sequence = SequenceNumber {1000},
            .options = options,
            .origin = ConnectionRuntime::Clock::now(),
            .peer_idle_timeout_milliseconds = 5,
            .now_function = IdlePollClock::read,
            .now_context = &clock});
    auto worker_gate = std::make_shared<SinkGate>();
    SinkRelease worker_release {worker_gate};
    sink_block_worker(fixture, worker_gate);
    auto dispatcher = fixture.dispatcher();
    REQUIRE(
        fixture.channel->register_connection(700, fixture.runtime, dispatcher));
    std::future<RuntimePollResult> poll;
    std::future<ConnectionDatagramInbox::Status> publication;
    SinkRelease release {clock.gate};
    clock.now.store(6001);
    clock.pause.store(true);
    poll = std::async(std::launch::async, [&] {
        return fixture.channel->poll_connections_for_testing();
    });
    clock.gate->wait();
    std::promise<void> entered;
    auto started = entered.get_future();
    publication = std::async(std::launch::async, [&] {
        entered.set_value();
        return dispatcher->inbox()->publish(
            dispatcher->inbox()->token(), sink_data(0).view(), sink_peer, 1);
    });
    started.get();
    REQUIRE_EQ(publication.wait_for(std::chrono::milliseconds {50}),
        std::future_status::timeout);
    clock.gate->release();
    (void)poll.get();
    REQUIRE_EQ(publication.get(), ConnectionDatagramInbox::Status::closed);
    REQUIRE_EQ(dispatcher->inbox()->snapshot().highwater, 0U);
    REQUIRE(!fixture.runtime->accepts_datagrams());
    fixture.channel->unregister_connection(700);
}

TEST(channel_ingress_idle_captured_inbox_rejects_closed_runtime)
{
    SinkFixture fixture;
    auto dispatcher = fixture.dispatcher();
    auto captured = dispatcher->inbox();
    fixture.runtime->close();
    REQUIRE_EQ(
        captured->publish(captured->token(), sink_data(0).view(), sink_peer, 1),
        ConnectionDatagramInbox::Status::closed);
    REQUIRE_EQ(captured->snapshot().highwater, 0U);
}

TEST(channel_ingress_idle_other_poll_work_keeps_the_shared_send_allowance)
{
    std::atomic<std::uint64_t> now {1000};
    SinkFixture fixture;
    ingress_idle_runtime(fixture, now);
    auto gate = std::make_shared<SinkGate>();
    SinkRelease release {gate};
    sink_block_worker(fixture, gate);
    auto dispatcher = fixture.dispatcher();
    REQUIRE(
        fixture.channel->register_connection(700, fixture.runtime, dispatcher));
    (void)fixture.ingress(sink_data(0));
    std::atomic<std::size_t> attempts {0};
    SinkNeutralSend clear_hook {fixture.channel};
    fixture.channel->set_send_hook_for_testing(
        [](std::span<const std::byte> bytes, IpEndpoint,
            void* pointer) noexcept {
            static_cast<std::atomic<std::size_t>*>(pointer)->fetch_add(1);
            return UdpIoResult {.bytes_transferred = bytes.size()};
        },
        &attempts);
    const std::array<std::byte, 2500> payload {};
    REQUIRE_EQ(
        fixture.runtime->queue_message(payload, 0, true, false, -1, -1).status,
        MessageIoStatus::success);
    now.store(6001);
    std::size_t remaining = 1;
    const auto result =
        fixture.runtime->poll(remaining, dispatcher->inbox().get(), 2000);
    REQUIRE(result.next_work_delay.has_value());
    REQUIRE_EQ(remaining, 0U);
    REQUIRE_EQ(attempts.load(), 1U);
    REQUIRE(fixture.runtime->accepts_datagrams());
    REQUIRE(fixture.runtime->poll(remaining, dispatcher->inbox().get(), 2000)
            .immediate_work);
    REQUIRE_EQ(attempts.load(), 1U);
    // No send hook can outlive the local attempt counter.
    fixture.channel->unregister_connection(700);
    fixture.channel->set_send_hook_for_testing(
        [](std::span<const std::byte> bytes, IpEndpoint, void*) noexcept {
            return UdpIoResult {.bytes_transferred = bytes.size()};
        },
        nullptr);
}

TEST(channel_ingress_idle_foreign_inbox_cannot_mask_another_runtime_timeout)
{
    std::atomic<std::uint64_t> now {1000};
    SinkFixture target;
    SinkFixture foreign;
    ingress_idle_runtime(target, now);
    auto gate = std::make_shared<SinkGate>();
    SinkRelease release {gate};
    sink_block_worker(foreign, gate);
    auto dispatcher = foreign.dispatcher();
    REQUIRE_EQ(dispatcher->publish(dispatcher->inbox()->token(),
                   sink_data(0).view(), sink_peer),
        ConnectionDatagramInbox::Status::accepted);
    now.store(6001);
    std::size_t remaining = 64;
    (void)target.runtime->poll(remaining, dispatcher->inbox().get(), 2000);
    REQUIRE(!target.runtime->accepts_datagrams());
}

TEST(channel_ingress_idle_grace_is_capped_at_one_peer_timeout)
{
    std::atomic<std::uint64_t> now {1000};
    SinkFixture fixture;
    ingress_idle_runtime(fixture, now);
    auto gate = std::make_shared<SinkGate>();
    SinkRelease release {gate};
    sink_block_worker(fixture, gate);
    auto dispatcher = fixture.dispatcher();
    REQUIRE(
        fixture.channel->register_connection(700, fixture.runtime, dispatcher));
    (void)fixture.ingress(sink_data(0));
    now.store(11001);
    std::size_t remaining = 64;
    (void)fixture.runtime->poll(remaining, dispatcher->inbox().get(),
        (std::numeric_limits<std::uint64_t>::max)());
    REQUIRE(!fixture.runtime->accepts_datagrams());
    fixture.channel->unregister_connection(700);
}

TEST(
    channel_ingress_idle_ignored_protocol_copy_does_not_fabricate_peer_activity)
{
    std::atomic<std::uint64_t> now {1000};
    SinkFixture fixture;
    ingress_idle_runtime(fixture, now);
    auto gate = std::make_shared<SinkGate>();
    SinkRelease release {gate};
    auto dispatcher = fixture.dispatcher(16, gate);
    REQUIRE(
        fixture.channel->register_connection(700, fixture.runtime, dispatcher));
    // This runtime has no replay response, so the established handler ignores
    // the otherwise valid framed handshake. Admission is not peer activity.
    REQUIRE_EQ(dispatcher->publish(dispatcher->inbox()->token(),
                   sink_replay().view(), sink_peer),
        ConnectionDatagramInbox::Status::accepted);
    gate->wait();
    now.store(5001);
    (void)fixture.channel->poll_connections_for_testing();
    REQUIRE(fixture.runtime->accepts_datagrams());
    gate->release();
    fixture.scheduler->stop();
    REQUIRE_EQ(
        fixture.runtime->response_health().last_response_microseconds, 0U);
    REQUIRE(!dispatcher->inbox()->snapshot().in_flight);
    (void)fixture.channel->poll_connections_for_testing();
    REQUIRE(!fixture.runtime->accepts_datagrams());
    fixture.channel->unregister_connection(700);
}

namespace {
std::shared_ptr<DatagramInbox> prefix_deadline_promote(SinkFixture& fixture,
    const std::shared_ptr<ConnectionDatagramDispatcher>& dispatcher,
    const SinkWire& wire)
{
    auto prefix = std::make_shared<DatagramInbox>(2);
    REQUIRE(prefix->push(wire.view(), sink_peer));
    REQUIRE(fixture.channel->register_setup_inbox(700, sink_peer, prefix, 90));
    REQUIRE(fixture.channel->promote_setup_connection(
        700, prefix, fixture.runtime, dispatcher));
    return prefix;
}
}

TEST(channel_prefix_deadline_queued_prefix_has_fixed_terminal_bound)
{
    std::atomic<std::uint64_t> now {1000};
    SinkFixture fixture;
    ingress_idle_runtime(fixture, now);
    auto gate = std::make_shared<SinkGate>();
    SinkRelease release {gate};
    sink_block_worker(fixture, gate);
    auto dispatcher = fixture.dispatcher(2);
    auto prefix = prefix_deadline_promote(fixture, dispatcher, sink_data(0));
    now.store(5001);
    const auto waiting = fixture.channel->poll_connections_for_testing(
        ConnectionRuntime::Clock::time_point {});
    REQUIRE_EQ(waiting.next_work_delay, std::chrono::microseconds {2000});
    REQUIRE(fixture.runtime->accepts_datagrams());
    REQUIRE(!dispatcher->setup_prefix_complete());
    REQUIRE(prefix->push(sink_data(1).view(), sink_peer));
    REQUIRE(!prefix->push(sink_data(2).view(), sink_peer));
    now.store(6001);
    const auto last_wait = fixture.channel->poll_connections_for_testing(
        ConnectionRuntime::Clock::time_point {});
    REQUIRE_EQ(last_wait.next_work_delay, std::chrono::microseconds {1000});
    now.store(7000);
    (void)fixture.channel->poll_connections_for_testing(
        ConnectionRuntime::Clock::time_point {});
    REQUIRE(fixture.runtime->accepts_datagrams());
    now.store(7001);
    (void)fixture.channel->poll_connections_for_testing(
        ConnectionRuntime::Clock::time_point {});
    REQUIRE(!fixture.runtime->accepts_datagrams());
    REQUIRE(dispatcher->setup_prefix_complete());
    REQUIRE(dispatcher->inbox()->snapshot().closed);
    REQUIRE(!prefix->push(sink_data(3).view(), sink_peer));
    gate->release();
    fixture.scheduler->stop();
    REQUIRE_EQ(dispatcher->snapshot().dispatched_datagrams, 0U);
    REQUIRE_EQ(fixture.runtime->buffer_packet_counts().available_receive, 0U);
    fixture.channel->unregister_connection(700);
}

TEST(channel_prefix_deadline_popped_prefix_cannot_resume_after_timeout)
{
    std::atomic<std::uint64_t> now {1000};
    SinkFixture fixture;
    ingress_idle_runtime(fixture, now);
    auto gate = std::make_shared<SinkGate>();
    SinkRelease release {gate};
    auto dispatcher = fixture.dispatcher(2, gate);
    auto prefix = prefix_deadline_promote(fixture, dispatcher, sink_data(0));
    gate->wait();
    REQUIRE(!dispatcher->setup_prefix_complete());
    REQUIRE(prefix->push(sink_data(1).view(), sink_peer));
    now.store(6001);
    (void)fixture.channel->poll_connections_for_testing(
        ConnectionRuntime::Clock::time_point {});
    REQUIRE(fixture.runtime->accepts_datagrams());
    now.store(7001);
    (void)fixture.channel->poll_connections_for_testing(
        ConnectionRuntime::Clock::time_point {});
    REQUIRE(!fixture.runtime->accepts_datagrams());
    REQUIRE(dispatcher->inbox()->snapshot().closed);
    REQUIRE(!dispatcher->quiescent());
    gate->release();
    fixture.scheduler->stop();
    REQUIRE(dispatcher->quiescent());
    REQUIRE_EQ(fixture.runtime->buffer_packet_counts().available_receive, 0U);
    fixture.channel->unregister_connection(700);
}

TEST(channel_prefix_deadline_completed_data_restores_ordinary_polling)
{
    std::atomic<std::uint64_t> now {1000};
    SinkFixture fixture;
    ingress_idle_runtime(fixture, now);
    auto gate = std::make_shared<SinkGate>();
    SinkRelease release {gate};
    auto dispatcher = fixture.dispatcher(2, gate);
    auto prefix = prefix_deadline_promote(fixture, dispatcher, sink_data(0));
    gate->wait();
    now.store(6001);
    (void)fixture.channel->poll_connections_for_testing(
        ConnectionRuntime::Clock::time_point {});
    REQUIRE(fixture.runtime->accepts_datagrams());
    gate->release();
    sink_receive(fixture.runtime, std::byte {1});
    fixture.scheduler->stop();
    REQUIRE(dispatcher->setup_prefix_complete());
    now.store(11001);
    (void)fixture.channel->poll_connections_for_testing(
        ConnectionRuntime::Clock::time_point {});
    REQUIRE(fixture.runtime->accepts_datagrams());
    // Completion uses real handler activity (6001); empty ingress gets no grace.
    now.store(11002);
    (void)fixture.channel->poll_connections_for_testing(
        ConnectionRuntime::Clock::time_point {});
    REQUIRE(!fixture.runtime->accepts_datagrams());
    fixture.channel->unregister_connection(700);
}

TEST(channel_prefix_deadline_ignored_prefix_does_not_fabricate_activity)
{
    std::atomic<std::uint64_t> now {1000};
    SinkFixture fixture;
    ingress_idle_runtime(fixture, now);
    auto gate = std::make_shared<SinkGate>();
    SinkRelease release {gate};
    auto dispatcher = fixture.dispatcher(2, gate);
    auto prefix = prefix_deadline_promote(fixture, dispatcher, sink_replay());
    gate->wait();
    now.store(6001);
    (void)fixture.channel->poll_connections_for_testing(
        ConnectionRuntime::Clock::time_point {});
    REQUIRE(fixture.runtime->accepts_datagrams());
    gate->release();
    fixture.scheduler->stop();
    REQUIRE(dispatcher->setup_prefix_complete());
    REQUIRE_EQ(fixture.replay_responses.load(), 0U);
    (void)fixture.channel->poll_connections_for_testing(
        ConnectionRuntime::Clock::time_point {});
    REQUIRE(!fixture.runtime->accepts_datagrams());
    fixture.channel->unregister_connection(700);
}

TEST(channel_prefix_deadline_grace_is_capped_at_one_peer_timeout)
{
    std::atomic<std::uint64_t> now {1000};
    SinkFixture fixture;
    ingress_idle_runtime(fixture, now, 1);
    auto gate = std::make_shared<SinkGate>();
    SinkRelease release {gate};
    sink_block_worker(fixture, gate);
    auto dispatcher = fixture.dispatcher();
    auto prefix = prefix_deadline_promote(fixture, dispatcher, sink_data(0));
    now.store(1001);
    const auto waiting = fixture.channel->poll_connections_for_testing(
        ConnectionRuntime::Clock::time_point {});
    REQUIRE_EQ(waiting.next_work_delay, std::chrono::microseconds {1000});
    REQUIRE(fixture.runtime->accepts_datagrams());
    now.store(2001);
    (void)fixture.channel->poll_connections_for_testing(
        ConnectionRuntime::Clock::time_point {});
    REQUIRE(!fixture.runtime->accepts_datagrams());
    REQUIRE(dispatcher->inbox()->snapshot().closed);
    fixture.channel->unregister_connection(700);
}

TEST(channel_prefix_deadline_close_retires_prefix_without_worker_progress)
{
    std::atomic<std::uint64_t> now {1000};
    SinkFixture fixture;
    ingress_idle_runtime(fixture, now);
    auto gate = std::make_shared<SinkGate>();
    SinkRelease release {gate};
    sink_block_worker(fixture, gate);
    auto dispatcher = fixture.dispatcher();
    auto prefix = prefix_deadline_promote(fixture, dispatcher, sink_data(0));
    fixture.runtime->close();
    (void)fixture.channel->poll_connections_for_testing(
        ConnectionRuntime::Clock::time_point {});
    REQUIRE(dispatcher->setup_prefix_complete());
    REQUIRE(dispatcher->inbox()->snapshot().closed);
    REQUIRE(!prefix->push(sink_data(1).view(), sink_peer));
    fixture.channel->unregister_connection(700);
}

TEST(channel_prefix_deadline_preserves_already_delivered_receive_data)
{
    std::atomic<std::uint64_t> now {1000};
    SinkFixture fixture;
    ingress_idle_runtime(fixture, now);
    const auto existing = sink_data(0);
    const auto decoded = decode_packet(existing.view());
    REQUIRE(decoded);
    fixture.runtime->process_packet(decoded.packet, sink_peer);
    REQUIRE_EQ(fixture.runtime->buffer_packet_counts().available_receive, 1U);
    auto gate = std::make_shared<SinkGate>();
    SinkRelease release {gate};
    sink_block_worker(fixture, gate);
    auto dispatcher = fixture.dispatcher();
    auto prefix = prefix_deadline_promote(fixture, dispatcher, sink_data(1));
    now.store(8001);
    (void)fixture.channel->poll_connections_for_testing(
        ConnectionRuntime::Clock::time_point {});
    REQUIRE(!fixture.runtime->accepts_datagrams());
    REQUIRE_EQ(fixture.runtime->buffer_packet_counts().available_receive, 1U);
    gate->release();
    fixture.scheduler->stop();
    REQUIRE_EQ(fixture.runtime->buffer_packet_counts().available_receive, 1U);
    fixture.channel->unregister_connection(700);
}

TEST(channel_prefix_deadline_does_not_spend_another_routes_send_allowance)
{
    std::atomic<std::uint64_t> now {1000};
    SinkFixture fixture;
    ingress_idle_runtime(fixture, now);
    auto gate = std::make_shared<SinkGate>();
    SinkRelease release {gate};
    sink_block_worker(fixture, gate);
    auto dispatcher = fixture.dispatcher();
    auto prefix = prefix_deadline_promote(fixture, dispatcher, sink_data(0));
    auto other = fixture.make_runtime(91, nullptr, false);
    REQUIRE(fixture.channel->register_connection(701, other));
    std::atomic<std::size_t> attempts {0};
    SinkNeutralSend clear_hook {fixture.channel};
    fixture.channel->set_send_hook_for_testing(
        [](std::span<const std::byte> bytes, IpEndpoint,
            void* pointer) noexcept {
            static_cast<std::atomic<std::size_t>*>(pointer)->fetch_add(1);
            return UdpIoResult {.bytes_transferred = bytes.size()};
        },
        &attempts);
    const std::array<std::byte, 1316> payload {};
    REQUIRE_EQ(
        fixture.runtime->queue_message(payload, 0, true, false, -1, -1).status,
        MessageIoStatus::success);
    REQUIRE_EQ(other->queue_message(payload, 0, true, false, -1, -1).status,
        MessageIoStatus::success);
    now.store(6001);
    (void)fixture.channel->poll_connections_for_testing(
        ConnectionRuntime::Clock::time_point {});
    REQUIRE_EQ(attempts.load(), 1U);
    REQUIRE(fixture.runtime->accepts_datagrams());
    REQUIRE(other->accepts_datagrams());
    now.store(7001);
    (void)fixture.channel->poll_connections_for_testing(
        ConnectionRuntime::Clock::time_point {});
    REQUIRE(!fixture.runtime->accepts_datagrams());
    REQUIRE(other->accepts_datagrams());
    REQUIRE_EQ(attempts.load(), 1U);
    fixture.channel->unregister_connection(700);
    fixture.channel->unregister_connection(701);
}

TEST(channel_prefix_deadline_busy_runtime_is_retried_without_blocking_channel)
{
    std::atomic<std::uint64_t> now {1000};
    auto receive_gate = std::make_shared<SinkGate>();
    SinkFixture fixture;
    ingress_idle_runtime(fixture, now, 5, receive_gate.get());
    auto pop_gate = std::make_shared<SinkGate>();
    auto dispatcher = fixture.dispatcher(2, pop_gate);
    const auto existing = sink_data(0);
    fixture.runtime->process_packet(
        decode_packet(existing.view()).packet, sink_peer);
    std::future<MessageIoResult> receiver;
    std::future<RuntimePollResult> poll;
    SinkRelease receive_release {receive_gate};
    SinkRelease pop_release {pop_gate};
    auto prefix = prefix_deadline_promote(fixture, dispatcher, sink_data(1));
    pop_gate->wait();
    receiver = std::async(std::launch::async, [&] {
        std::array<std::byte, 8> bytes {};
        return fixture.runtime->receive_message(bytes, false, 0);
    });
    receive_gate->wait();
    now.store(8001);
    poll = std::async(std::launch::async, [&] {
        return fixture.channel->poll_connections_for_testing(
            ConnectionRuntime::Clock::time_point {});
    });
    REQUIRE_EQ(
        poll.wait_for(std::chrono::seconds {2}), std::future_status::ready);
    REQUIRE_EQ(poll.get().next_work_delay, std::chrono::microseconds {2000});
    REQUIRE(!dispatcher->inbox()->snapshot().closed);
    // No route lock is retained while the runtime mutex is busy.
    REQUIRE(fixture.channel->register_setup_inbox(
        701, sink_peer, std::make_shared<DatagramInbox>(1), 91));
    receive_gate->release();
    REQUIRE_EQ(receiver.get().status, MessageIoStatus::success);
    // The next available check uses the original fixed deadline.
    (void)fixture.channel->poll_connections_for_testing(
        ConnectionRuntime::Clock::time_point {});
    REQUIRE(!fixture.runtime->accepts_datagrams());
    REQUIRE(dispatcher->inbox()->snapshot().closed);
    pop_gate->release();
    fixture.scheduler->stop();
    REQUIRE_EQ(fixture.runtime->buffer_packet_counts().available_receive, 0U);
    fixture.channel->unregister_connection(700);
}

TEST(channel_prefix_deadline_zero_peer_timeout_gets_no_processing_grace)
{
    std::atomic<std::uint64_t> now {0};
    SinkFixture fixture;
    ingress_idle_runtime(fixture, now, 0);
    auto gate = std::make_shared<SinkGate>();
    SinkRelease release {gate};
    sink_block_worker(fixture, gate);
    auto dispatcher = fixture.dispatcher();
    auto prefix = prefix_deadline_promote(fixture, dispatcher, sink_data(0));
    (void)fixture.channel->poll_connections_for_testing(
        ConnectionRuntime::Clock::time_point {});
    REQUIRE(fixture.runtime->accepts_datagrams());
    now.store(1);
    (void)fixture.channel->poll_connections_for_testing(
        ConnectionRuntime::Clock::time_point {});
    REQUIRE(!fixture.runtime->accepts_datagrams());
    REQUIRE(dispatcher->inbox()->snapshot().closed);
    fixture.channel->unregister_connection(700);
}

namespace {
struct CohortWireCounts {
    std::atomic<std::size_t> data {0};
    std::atomic<std::size_t> retransmitted {0};
    std::atomic<std::size_t> drops {0};
    static UdpIoResult send(
        std::span<const std::byte> bytes, IpEndpoint, void* pointer) noexcept
    {
        auto& counts = *static_cast<CohortWireCounts*>(pointer);
        const auto packet = decode_packet(bytes);
        if (packet && packet.packet.kind == PacketKind::data) {
            counts.data.fetch_add(1);
            counts.retransmitted.fetch_add(
                packet.packet.data.retransmitted ? 1 : 0);
        } else if (packet
            && packet.packet.control.type == ControlType::drop_request) {
            counts.drops.fetch_add(1);
        }
        return {.bytes_transferred = bytes.size()};
    }
};
void cohort_runtime(SinkFixture& fixture, std::atomic<std::uint64_t>& now,
    bool file = false, NegotiatedLiveOptions negotiated = {})
{
    SocketOptions options;
    REQUIRE_EQ(options.set(SocketOption::tsbpd_mode, 0), Error::none);
    if (file) {
        REQUIRE_EQ(options.set(SocketOption::transmission_type,
                       static_cast<std::int64_t>(TransmissionType::file)),
            Error::none);
        REQUIRE_EQ(
            options.set(SocketOption::maximum_payload_size, 4), Error::none);
    }
    fixture.runtime = std::make_shared<ConnectionRuntime>(
        ConnectionRuntime::Configuration {.channel = fixture.channel,
            .peer = sink_peer,
            .peer_socket_id = 90,
            .initial_sequence = SequenceNumber {1000},
            .options = options,
            .negotiated_options = negotiated,
            .efficient_retransmission = false,
            .origin = ConnectionRuntime::Clock::now(),
            .peer_idle_timeout_milliseconds = 1000,
            .now_function = ingress_idle_now,
            .now_context = &now});
}
RuntimePollResult cohort_poll(SinkFixture& fixture)
{
    return fixture.channel->poll_connections_for_testing(
        ConnectionRuntime::Clock::time_point {});
}
SinkWire cohort_ack()
{
    std::array<std::byte, 32> payload {};
    const auto encoded = encode_acknowledgement_payload(
        {.kind = AcknowledgementKind::lite,
            .next_sequence = SequenceNumber {1001}},
        payload);
    REQUIRE(encoded);
    SinkWire wire;
    const auto packet = encode_packet(
        {.kind = PacketKind::control,
            .control = {.type = ControlType::acknowledgement,
                .destination_socket_id = 700},
            .payload = std::span {payload}.first(encoded.bytes_written)},
        wire.bytes);
    REQUIRE(packet);
    wire.size = packet.bytes_written;
    return wire;
}
SinkWire cohort_nak()
{
    std::array<std::byte, 8> payload {};
    const std::array losses {SequenceRange {
        .first = SequenceNumber {1000}, .last = SequenceNumber {1000}}};
    const auto encoded = encode_loss_ranges(losses, payload);
    REQUIRE(encoded);
    SinkWire wire;
    const auto packet = encode_packet(
        {.kind = PacketKind::control,
            .control = {.type = ControlType::negative_acknowledgement,
                .destination_socket_id = 700},
            .payload = std::span {payload}.first(encoded.bytes_written)},
        wire.bytes);
    REQUIRE(packet);
    wire.size = packet.bytes_written;
    return wire;
}
struct CohortSteps {
    std::mutex mutex;
    std::condition_variable changed;
    std::size_t entered = 0;
    std::size_t released = 0;
    static void block(void* pointer) noexcept
    {
        auto& self = *static_cast<CohortSteps*>(pointer);
        std::unique_lock lock(self.mutex);
        const auto step = ++self.entered;
        self.changed.notify_all();
        self.changed.wait(lock, [&] {
            return self.released >= step;
        });
    }
    void wait(std::size_t step)
    {
        std::unique_lock lock(mutex);
        REQUIRE(changed.wait_for(lock, std::chrono::seconds {2}, [&] {
            return entered >= step;
        }));
    }
    void release(std::size_t step)
    {
        std::lock_guard lock(mutex);
        released = step;
        changed.notify_all();
    }
};
struct CohortRelease {
    std::shared_ptr<CohortSteps> steps;
    ~CohortRelease()
    {
        steps->release(std::numeric_limits<std::size_t>::max());
    }
};
}

TEST(channel_cohort_later_admission_does_not_extend_original_completion_fence)
{
    std::atomic<std::uint64_t> now {1000};
    SinkFixture fixture;
    cohort_runtime(fixture, now);
    auto steps = std::make_shared<CohortSteps>();
    CohortRelease release {steps};
    auto dispatcher =
        ConnectionDatagramDispatcher::create(fixture.runtime, fixture.scheduler,
            fixture.budget, 1, sink_peer, {.capacity = 4, .control_reserve = 1},
            {.turn_budget = 2,
                .after_pop_for_testing = CohortSteps::block,
                .after_pop_context_for_testing = steps});
    REQUIRE(dispatcher != nullptr);
    REQUIRE(
        fixture.channel->register_connection(700, fixture.runtime, dispatcher));
    CohortWireCounts counts;
    SinkNeutralSend neutral {fixture.channel};
    fixture.channel->set_send_hook_for_testing(CohortWireCounts::send, &counts);
    const std::array payload {std::byte {9}};
    REQUIRE_EQ(
        fixture.runtime->queue_message(payload, 0, true, false, -1).status,
        MessageIoStatus::success);
    (void)fixture.ingress(sink_data(0));
    steps->wait(1);
    REQUIRE_EQ(
        cohort_poll(fixture).next_work_delay, std::chrono::microseconds {2000});
    REQUIRE_EQ(counts.data.load(), 0U);
    (void)fixture.ingress(sink_data(1));
    steps->release(1);
    steps->wait(2);
    const auto pending = dispatcher->inbox()->snapshot();
    REQUIRE_EQ(pending.admitted, 2U);
    REQUIRE_EQ(pending.completed, 1U);
    REQUIRE(pending.in_flight);
    (void)cohort_poll(fixture);
    REQUIRE_EQ(counts.data.load(), 1U);
    steps->release(2);
    fixture.scheduler->stop();
    REQUIRE_EQ(dispatcher->inbox()->snapshot().completed, 2U);
    fixture.channel->unregister_connection(700);
}

TEST(channel_cohort_expiry_keeps_polling_until_original_backlog_finishes)
{
    std::atomic<std::uint64_t> now {1000};
    SinkFixture fixture;
    cohort_runtime(fixture, now);
    auto gate = std::make_shared<SinkGate>();
    SinkRelease release {gate};
    sink_block_worker(fixture, gate);
    auto dispatcher = fixture.dispatcher(2);
    REQUIRE(
        fixture.channel->register_connection(700, fixture.runtime, dispatcher));
    CohortWireCounts counts;
    SinkNeutralSend neutral {fixture.channel};
    fixture.channel->set_send_hook_for_testing(CohortWireCounts::send, &counts);
    const std::array payload {std::byte {9}};
    REQUIRE_EQ(
        fixture.runtime->queue_message(payload, 0, true, false, -1).status,
        MessageIoStatus::success);
    (void)fixture.ingress(sink_data(0));
    (void)cohort_poll(fixture);
    now.store(2999);
    (void)cohort_poll(fixture);
    REQUIRE_EQ(counts.data.load(), 0U);
    REQUIRE_EQ(dispatcher->publish(dispatcher->inbox()->token(),
                   sink_data(1).view(), sink_peer),
        ConnectionDatagramInbox::Status::full);
    now.store(3000);
    (void)cohort_poll(fixture);
    REQUIRE_EQ(counts.data.load(), 1U);
    REQUIRE_EQ(
        fixture.runtime->queue_message(payload, 0, true, false, -1).status,
        MessageIoStatus::success);
    REQUIRE_EQ(dispatcher->publish(dispatcher->inbox()->token(),
                   sink_replay().view(), sink_peer),
        ConnectionDatagramInbox::Status::accepted);
    now.store(3100);
    (void)cohort_poll(fixture);
    REQUIRE_EQ(counts.data.load(), 2U);
    REQUIRE_EQ(dispatcher->inbox()->snapshot().completed, 0U);
    fixture.channel->unregister_connection(700);
}

TEST(channel_cohort_wait_does_not_spend_shared_send_credit)
{
    std::atomic<std::uint64_t> now {1000};
    SinkFixture fixture;
    cohort_runtime(fixture, now);
    auto gate = std::make_shared<SinkGate>();
    SinkRelease release {gate};
    auto dispatcher = fixture.dispatcher(4, gate);
    REQUIRE(
        fixture.channel->register_connection(700, fixture.runtime, dispatcher));
    CohortWireCounts counts;
    SinkNeutralSend neutral {fixture.channel};
    fixture.channel->set_send_hook_for_testing(CohortWireCounts::send, &counts);
    const std::array<std::byte, 2500> payload {};
    REQUIRE_EQ(
        fixture.runtime->queue_message(payload, 0, true, false, -1).status,
        MessageIoStatus::success);
    (void)fixture.ingress(sink_data(0));
    gate->wait();
    std::size_t credit = 1;
    (void)fixture.runtime->poll(credit, dispatcher->inbox().get(), 2000, true);
    REQUIRE_EQ(credit, 1U);
    REQUIRE_EQ(counts.data.load(), 0U);
    REQUIRE_EQ(dispatcher->inbox()->snapshot().queued, 0U);
    REQUIRE(dispatcher->inbox()->snapshot().in_flight);
    now.store(3000);
    (void)fixture.runtime->poll(credit, dispatcher->inbox().get(), 2000, true);
    REQUIRE_EQ(credit, 0U);
    REQUIRE_EQ(counts.data.load(), 1U);
    (void)fixture.runtime->poll(credit, dispatcher->inbox().get(), 2000, true);
    REQUIRE_EQ(counts.data.load(), 1U);
    fixture.channel->unregister_connection(700);
}

TEST(channel_cohort_accepted_ack_precedes_due_sender_retransmission)
{
    std::atomic<std::uint64_t> now {100000};
    SinkFixture fixture;
    cohort_runtime(fixture, now, true);
    CohortWireCounts counts;
    SinkNeutralSend neutral {fixture.channel};
    fixture.channel->set_send_hook_for_testing(CohortWireCounts::send, &counts);
    const std::array<std::byte, 4> payload {};
    REQUIRE_EQ(fixture.runtime->queue_stream(payload, false, -1).bytes, 4U);
    (void)fixture.runtime->poll();
    REQUIRE_EQ(counts.data.load(), 1U);
    auto gate = std::make_shared<SinkGate>();
    SinkRelease release {gate};
    auto dispatcher = fixture.dispatcher(4, gate);
    REQUIRE(
        fixture.channel->register_connection(700, fixture.runtime, dispatcher));
    now.store(430000);
    (void)fixture.ingress(cohort_ack());
    gate->wait();
    (void)cohort_poll(fixture);
    REQUIRE_EQ(counts.retransmitted.load(), 0U);
    gate->release();
    fixture.scheduler->stop();
    REQUIRE_EQ(dispatcher->inbox()->snapshot().completed, 1U);
    REQUIRE_EQ(fixture.runtime->buffer_packet_counts().unacknowledged_send, 0U);
    (void)cohort_poll(fixture);
    REQUIRE_EQ(counts.data.load(), 1U);
    REQUIRE_EQ(counts.retransmitted.load(), 0U);
    fixture.channel->unregister_connection(700);
}

TEST(channel_cohort_accepted_nak_is_applied_before_next_poll_send)
{
    std::atomic<std::uint64_t> now {1000};
    SinkFixture fixture;
    cohort_runtime(fixture, now, true);
    CohortWireCounts counts;
    SinkNeutralSend neutral {fixture.channel};
    fixture.channel->set_send_hook_for_testing(CohortWireCounts::send, &counts);
    const std::array<std::byte, 4> payload {};
    REQUIRE_EQ(fixture.runtime->queue_stream(payload, false, -1).bytes, 4U);
    (void)fixture.runtime->poll();
    REQUIRE_EQ(counts.data.load(), 1U);
    auto gate = std::make_shared<SinkGate>();
    SinkRelease release {gate};
    auto dispatcher = fixture.dispatcher(4, gate);
    REQUIRE(
        fixture.channel->register_connection(700, fixture.runtime, dispatcher));
    (void)fixture.ingress(cohort_nak());
    gate->wait();
    (void)cohort_poll(fixture);
    REQUIRE_EQ(counts.retransmitted.load(), 0U);
    gate->release();
    fixture.scheduler->stop();
    now.store(1100); // The original DATA pacing slot must also have elapsed.
    (void)cohort_poll(fixture);
    REQUIRE_EQ(counts.data.load(), 2U);
    REQUIRE_EQ(fixture.runtime->statistics(false, true)
                   .total.sent_retransmitted.packets,
        1U);
    fixture.channel->unregister_connection(700);
}

TEST(channel_cohort_expiry_services_sender_ttl_under_stalled_ingress)
{
    std::atomic<std::uint64_t> now {1000};
    SinkFixture fixture;
    cohort_runtime(fixture, now);
    auto gate = std::make_shared<SinkGate>();
    SinkRelease release {gate};
    sink_block_worker(fixture, gate);
    auto dispatcher = fixture.dispatcher();
    REQUIRE(
        fixture.channel->register_connection(700, fixture.runtime, dispatcher));
    CohortWireCounts counts;
    SinkNeutralSend neutral {fixture.channel};
    fixture.channel->set_send_hook_for_testing(CohortWireCounts::send, &counts);
    const std::array payload {std::byte {9}};
    REQUIRE_EQ(
        fixture.runtime->queue_message(payload, 0, true, false, -1, 2).status,
        MessageIoStatus::success);
    REQUIRE_EQ(dispatcher->publish(dispatcher->inbox()->token(),
                   sink_replay().view(), sink_peer),
        ConnectionDatagramInbox::Status::accepted);
    (void)cohort_poll(fixture);
    now.store(2999);
    (void)cohort_poll(fixture);
    REQUIRE_EQ(counts.drops.load(), 0U);
    REQUIRE_EQ(counts.data.load(), 0U);
    now.store(3001);
    (void)cohort_poll(fixture);
    REQUIRE_EQ(counts.drops.load(), 1U);
    REQUIRE_EQ(counts.data.load(), 0U);
    REQUIRE_EQ(fixture.runtime->statistics(false, true)
                   .total.sender_message_ttl_dropped.packets,
        1U);
    REQUIRE_EQ(dispatcher->inbox()->snapshot().completed, 0U);
    fixture.channel->unregister_connection(700);
}

TEST(channel_cohort_data_fills_gap_before_receiver_deadline_poll)
{
    std::atomic<std::uint64_t> now {100};
    SinkFixture fixture;
    cohort_runtime(fixture, now, false,
        {.receive_tsbpd = true,
            .too_late_packet_drop = true,
            .periodic_nak = true,
            .receive_delay_milliseconds = 1});
    const auto following = sink_data(1);
    fixture.runtime->process_packet(
        decode_packet(following.view()).packet, sink_peer);
    REQUIRE_EQ(fixture.runtime->buffer_packet_counts().available_receive, 1U);
    auto gate = std::make_shared<SinkGate>();
    SinkRelease release {gate};
    auto dispatcher = fixture.dispatcher(4, gate);
    REQUIRE(
        fixture.channel->register_connection(700, fixture.runtime, dispatcher));
    (void)fixture.ingress(sink_data(0));
    gate->wait();
    now.store(1500);
    (void)cohort_poll(fixture);
    REQUIRE_EQ(
        fixture.runtime->receive_floor_sequence(), SequenceNumber {1000});
    gate->release();
    fixture.scheduler->stop();
    (void)cohort_poll(fixture);
    REQUIRE_EQ(fixture.runtime->buffer_packet_counts().available_receive, 2U);
    sink_receive(fixture.runtime, std::byte {1});
    sink_receive(fixture.runtime, std::byte {2});
    fixture.channel->unregister_connection(700);
}

TEST(channel_cohort_closed_inbox_releases_wait_without_fabricated_completion)
{
    std::atomic<std::uint64_t> now {1000};
    SinkFixture fixture;
    cohort_runtime(fixture, now);
    auto gate = std::make_shared<SinkGate>();
    SinkRelease release {gate};
    auto dispatcher = fixture.dispatcher(4, gate);
    REQUIRE(
        fixture.channel->register_connection(700, fixture.runtime, dispatcher));
    CohortWireCounts counts;
    SinkNeutralSend neutral {fixture.channel};
    fixture.channel->set_send_hook_for_testing(CohortWireCounts::send, &counts);
    const std::array payload {std::byte {9}};
    REQUIRE_EQ(
        fixture.runtime->queue_message(payload, 0, true, false, -1).status,
        MessageIoStatus::success);
    (void)fixture.ingress(sink_data(0));
    gate->wait();
    (void)cohort_poll(fixture);
    dispatcher->retire();
    REQUIRE_EQ(dispatcher->inbox()->snapshot().completed, 0U);
    REQUIRE(dispatcher->inbox()->snapshot().in_flight);
    (void)cohort_poll(fixture);
    REQUIRE_EQ(counts.data.load(), 1U);
    REQUIRE(fixture.runtime->accepts_datagrams());
    gate->release();
    fixture.scheduler->stop();
    REQUIRE_EQ(dispatcher->inbox()->snapshot().completed, 1U);
    fixture.channel->unregister_connection(700);
}

TEST(channel_cohort_peer_timeout_remains_outer_terminal_bound)
{
    std::atomic<std::uint64_t> now {1000};
    SinkFixture fixture;
    ingress_idle_runtime(fixture, now);
    auto gate = std::make_shared<SinkGate>();
    SinkRelease release {gate};
    sink_block_worker(fixture, gate);
    auto dispatcher = fixture.dispatcher();
    REQUIRE(
        fixture.channel->register_connection(700, fixture.runtime, dispatcher));
    (void)fixture.ingress(sink_data(0));
    now.store(6001);
    REQUIRE_EQ(
        cohort_poll(fixture).next_work_delay, std::chrono::microseconds {1000});
    REQUIRE(fixture.runtime->accepts_datagrams());
    now.store(7001);
    (void)cohort_poll(fixture);
    REQUIRE(!fixture.runtime->accepts_datagrams());
    REQUIRE_EQ(dispatcher->inbox()->snapshot().completed, 0U);
    fixture.channel->unregister_connection(700);
}

TEST(channel_cohort_unbounded_requested_wait_is_capped_at_two_milliseconds)
{
    std::atomic<std::uint64_t> now {1000};
    SinkFixture fixture;
    cohort_runtime(fixture, now);
    auto gate = std::make_shared<SinkGate>();
    SinkRelease release {gate};
    sink_block_worker(fixture, gate);
    auto dispatcher = fixture.dispatcher();
    CohortWireCounts counts;
    SinkNeutralSend neutral {fixture.channel};
    fixture.channel->set_send_hook_for_testing(CohortWireCounts::send, &counts);
    const std::array payload {std::byte {9}};
    REQUIRE_EQ(
        fixture.runtime->queue_message(payload, 0, true, false, -1).status,
        MessageIoStatus::success);
    REQUIRE_EQ(dispatcher->publish(dispatcher->inbox()->token(),
                   sink_data(0).view(), sink_peer),
        ConnectionDatagramInbox::Status::accepted);
    std::size_t credit = 1;
    const auto waiting =
        fixture.runtime->poll(credit, dispatcher->inbox().get(),
            std::numeric_limits<std::uint64_t>::max(), true);
    REQUIRE_EQ(waiting.next_work_delay, std::chrono::microseconds {2000});
    now.store(3000);
    (void)fixture.runtime->poll(credit, dispatcher->inbox().get(),
        std::numeric_limits<std::uint64_t>::max(), true);
    REQUIRE_EQ(counts.data.load(), 1U);
    REQUIRE_EQ(credit, 0U);
}

TEST(channel_cohort_expired_ack_wait_does_not_suppress_sender_rto)
{
    std::atomic<std::uint64_t> now {100000};
    SinkFixture fixture;
    cohort_runtime(fixture, now, true);
    CohortWireCounts counts;
    SinkNeutralSend neutral {fixture.channel};
    fixture.channel->set_send_hook_for_testing(CohortWireCounts::send, &counts);
    const std::array<std::byte, 4> payload {};
    REQUIRE_EQ(fixture.runtime->queue_stream(payload, false, -1).bytes, 4U);
    (void)fixture.runtime->poll();
    auto gate = std::make_shared<SinkGate>();
    SinkRelease release {gate};
    auto dispatcher = fixture.dispatcher(4, gate);
    REQUIRE(
        fixture.channel->register_connection(700, fixture.runtime, dispatcher));
    now.store(430000);
    (void)fixture.ingress(cohort_ack());
    gate->wait();
    REQUIRE_EQ(counts.data.load(), 1U);
    now.store(432000);
    (void)cohort_poll(fixture);
    REQUIRE_EQ(counts.data.load(), 2U);
    REQUIRE_EQ(fixture.runtime->statistics(false, true)
                   .total.sent_retransmitted.packets,
        1U);
    REQUIRE_EQ(dispatcher->inbox()->snapshot().completed, 0U);
    gate->release();
    fixture.scheduler->stop();
    (void)cohort_poll(fixture);
    REQUIRE_EQ(fixture.runtime->buffer_packet_counts().unacknowledged_send, 0U);
    REQUIRE_EQ(counts.data.load(), 2U);
    fixture.channel->unregister_connection(700);
}

TEST(channel_cohort_wait_does_not_stop_another_route_on_shared_channel)
{
    std::atomic<std::uint64_t> now {1000};
    SinkFixture fixture;
    cohort_runtime(fixture, now);
    auto gate = std::make_shared<SinkGate>();
    SinkRelease release {gate};
    sink_block_worker(fixture, gate);
    auto dispatcher = fixture.dispatcher();
    REQUIRE(
        fixture.channel->register_connection(700, fixture.runtime, dispatcher));
    // Publish directly so the next explicit channel poll owns both route visits.
    REQUIRE_EQ(dispatcher->publish(dispatcher->inbox()->token(),
                   sink_data(0).view(), sink_peer),
        ConnectionDatagramInbox::Status::accepted);
    auto other = fixture.make_runtime(91, nullptr, false);
    REQUIRE(fixture.channel->register_connection(701, other));
    CohortWireCounts counts;
    SinkNeutralSend neutral {fixture.channel};
    fixture.channel->set_send_hook_for_testing(CohortWireCounts::send, &counts);
    const std::array payload {std::byte {9}};
    REQUIRE_EQ(
        fixture.runtime->queue_message(payload, 0, true, false, -1).status,
        MessageIoStatus::success);
    REQUIRE_EQ(other->queue_message(payload, 0, true, false, -1).status,
        MessageIoStatus::success);
    (void)cohort_poll(fixture);
    REQUIRE_EQ(counts.data.load(), 1U);
    REQUIRE_EQ(dispatcher->inbox()->snapshot().completed, 0U);
    REQUIRE(fixture.runtime->accepts_datagrams());
    REQUIRE(other->accepts_datagrams());
    now.store(3000);
    (void)cohort_poll(fixture);
    REQUIRE_EQ(counts.data.load(), 2U);
    fixture.channel->unregister_connection(700);
    fixture.channel->unregister_connection(701);
}

TEST(channel_retirement_drain_reclaims_ring_with_captured_closed_handles)
{
    SinkFixture fixture;
    auto gate = std::make_shared<SinkGate>();
    SinkRelease release {gate};
    sink_block_worker(fixture, gate);
    auto old = fixture.dispatcher(4);
    auto captured = old->inbox();
    const auto token = captured->token();
    REQUIRE_EQ(old->publish(token, sink_data(1).view(), sink_peer),
        ConnectionDatagramInbox::Status::accepted);
    const auto result =
        old->finish_retirement(std::chrono::steady_clock::now());
    REQUIRE_EQ(result.status, ConnectionWorkBinding::DrainStatus::quiescent);
    REQUIRE(result.storage_released);
    REQUIRE(captured->snapshot().storage_released);
    REQUIRE_EQ(fixture.budget->reserved_bytes(), 0U);
    REQUIRE(fixture.runtime->accepts_datagrams());
    DatagramEnvelope envelope;
    REQUIRE_EQ(captured->pop(token, envelope, 0),
        ConnectionDatagramInbox::Status::closed);
    REQUIRE_EQ(captured->rearm(token), ConnectionDatagramInbox::Status::closed);
    REQUIRE_EQ(captured->publish(token, sink_data(0).view(), sink_peer, 0),
        ConnectionDatagramInbox::Status::closed);
    auto next = fixture.dispatcher(4);
    REQUIRE_EQ(next->publish(token, sink_data(0).view(), sink_peer),
        ConnectionDatagramInbox::Status::stale);
    REQUIRE_EQ(fixture.budget->reserved_bytes(),
        *ConnectionDatagramInbox::storage_bytes(4));
    REQUIRE(old->finish_retirement(std::chrono::steady_clock::time_point {})
            .storage_released);
    old.reset();
    captured.reset();
    REQUIRE_EQ(fixture.budget->reserved_bytes(),
        *ConnectionDatagramInbox::storage_bytes(4));
    REQUIRE_EQ(
        next->publish(next->inbox()->token(), sink_data(0).view(), sink_peer),
        ConnectionDatagramInbox::Status::accepted);
    gate->release();
    sink_receive(fixture.runtime, std::byte {1});
    fixture.scheduler->stop();
    REQUIRE_EQ(fixture.runtime->buffer_packet_counts().available_receive, 0U);
    REQUIRE(next->finish_retirement(std::chrono::steady_clock::now())
            .storage_released);
    REQUIRE_EQ(fixture.budget->reserved_bytes(), 0U);
}

TEST(channel_retirement_drain_timeout_keeps_inflight_ring_until_close_barrier)
{
    SinkFixture fixture;
    auto gate = std::make_shared<SinkGate>();
    SinkRelease release {gate};
    auto dispatcher = fixture.dispatcher(4, gate);
    auto captured = dispatcher->inbox();
    REQUIRE_EQ(
        dispatcher->publish(captured->token(), sink_data(0).view(), sink_peer),
        ConnectionDatagramInbox::Status::accepted);
    gate->wait();
    dispatcher->close();
    const auto first =
        dispatcher->finish_retirement(std::chrono::steady_clock::now());
    REQUIRE_EQ(first.status, ConnectionWorkBinding::DrainStatus::timeout);
    REQUIRE(!first.storage_released);
    REQUIRE(captured->snapshot().in_flight);
    REQUIRE(!captured->snapshot().storage_released);
    REQUIRE_EQ(fixture.budget->reserved_bytes(),
        *ConnectionDatagramInbox::storage_bytes(4));
    gate->release();
    const auto second = dispatcher->finish_retirement(
        std::chrono::steady_clock::now() + std::chrono::seconds {2});
    REQUIRE_EQ(second.status, ConnectionWorkBinding::DrainStatus::quiescent);
    REQUIRE(second.storage_released);
    REQUIRE(!captured->snapshot().in_flight);
    REQUIRE_EQ(fixture.budget->reserved_bytes(), 0U);
    REQUIRE_EQ(fixture.runtime->buffer_packet_counts().available_receive, 0U);
    REQUIRE_EQ(
        captured->publish(captured->token(), sink_data(1).view(), sink_peer, 0),
        ConnectionDatagramInbox::Status::closed);
    fixture.scheduler->stop();
}

TEST(channel_retirement_drain_concurrent_callers_return_ring_credit_once)
{
    SinkFixture fixture;
    auto gate = std::make_shared<SinkGate>();
    auto dispatcher = fixture.dispatcher(4, gate);
    auto captured = dispatcher->inbox();
    std::future<ConnectionDatagramDispatcher::DrainResult> first;
    std::future<ConnectionDatagramDispatcher::DrainResult> second;
    SinkRelease release {gate};
    REQUIRE_EQ(
        dispatcher->publish(captured->token(), sink_data(0).view(), sink_peer),
        ConnectionDatagramInbox::Status::accepted);
    gate->wait();
    first = std::async(std::launch::async, [&] {
        return dispatcher->finish_retirement(
            std::chrono::steady_clock::now() + std::chrono::seconds {2});
    });
    second = std::async(std::launch::async, [&] {
        return dispatcher->finish_retirement(
            std::chrono::steady_clock::now() + std::chrono::seconds {2});
    });
    REQUIRE_EQ(first.wait_for(std::chrono::milliseconds {0}),
        std::future_status::timeout);
    REQUIRE_EQ(second.wait_for(std::chrono::milliseconds {0}),
        std::future_status::timeout);
    gate->release();
    const auto a = first.get();
    const auto b = second.get();
    REQUIRE_EQ(a.status, ConnectionWorkBinding::DrainStatus::quiescent);
    REQUIRE_EQ(b.status, ConnectionWorkBinding::DrainStatus::quiescent);
    REQUIRE(a.storage_released && b.storage_released);
    REQUIRE_EQ(fixture.budget->reserved_bytes(), 0U);
    fixture.scheduler->stop();
    REQUIRE(
        dispatcher->finish_retirement(std::chrono::steady_clock::time_point {})
            .storage_released);
    REQUIRE_EQ(fixture.budget->reserved_bytes(), 0U);
}

TEST(channel_retirement_drain_worker_retires_without_wait_or_ring_free)
{
    SinkFixture fixture;
    struct Probe {
        std::weak_ptr<ConnectionDatagramDispatcher> dispatcher;
        std::promise<ConnectionDatagramDispatcher::DrainResult> answer;
        static void run(void* pointer) noexcept
        {
            auto& self = *static_cast<Probe*>(pointer);
            const auto dispatcher = self.dispatcher.lock();
            self.answer.set_value(dispatcher != nullptr
                    ? dispatcher->finish_retirement(
                          std::chrono::steady_clock::now()
                          + std::chrono::seconds {10})
                    : ConnectionDatagramDispatcher::DrainResult {
                          ConnectionWorkBinding::DrainStatus::not_retired,
                          false});
        }
    };
    auto probe = std::make_shared<Probe>();
    auto result = probe->answer.get_future();
    auto dispatcher =
        ConnectionDatagramDispatcher::create(fixture.runtime, fixture.scheduler,
            fixture.budget, 1, sink_peer, {.capacity = 4, .control_reserve = 1},
            {.turn_budget = 2,
                .after_pop_for_testing = Probe::run,
                .after_pop_context_for_testing = probe});
    REQUIRE(dispatcher != nullptr);
    probe->dispatcher = dispatcher;
    REQUIRE_EQ(dispatcher->publish(dispatcher->inbox()->token(),
                   sink_data(0).view(), sink_peer),
        ConnectionDatagramInbox::Status::accepted);
    REQUIRE_EQ(
        result.wait_for(std::chrono::seconds {2}), std::future_status::ready);
    const auto on_worker = result.get();
    REQUIRE_EQ(
        on_worker.status, ConnectionWorkBinding::DrainStatus::worker_thread);
    REQUIRE(!on_worker.storage_released);
    fixture.scheduler->stop();
    // Admission retirement permits the already popped callback to finish;
    // callers needing no further state mutation must close the runtime first.
    REQUIRE_EQ(fixture.runtime->buffer_packet_counts().available_receive, 1U);
    REQUIRE(dispatcher->finish_retirement(std::chrono::steady_clock::now())
            .storage_released);
    REQUIRE_EQ(fixture.budget->reserved_bytes(), 0U);
}

TEST(channel_retirement_drain_paused_prefix_callback_retains_new_ring_credit)
{
    SinkFixture fixture;
    auto gate = std::make_shared<SinkGate>();
    SinkRelease release {gate};
    auto dispatcher = fixture.dispatcher(4, gate);
    auto prefix = prefix_deadline_promote(fixture, dispatcher, sink_data(0));
    gate->wait();
    REQUIRE(!dispatcher->inbox()->snapshot().in_flight);
    REQUIRE(!dispatcher->setup_prefix_complete());
    dispatcher->close();
    const auto timeout =
        dispatcher->finish_retirement(std::chrono::steady_clock::now());
    REQUIRE_EQ(timeout.status, ConnectionWorkBinding::DrainStatus::timeout);
    REQUIRE(!timeout.storage_released);
    REQUIRE_EQ(fixture.budget->reserved_bytes(),
        *ConnectionDatagramInbox::storage_bytes(4));
    REQUIRE(!prefix->push(sink_data(1).view(), sink_peer));
    gate->release();
    const auto quiet = dispatcher->finish_retirement(
        std::chrono::steady_clock::now() + std::chrono::seconds {2});
    REQUIRE_EQ(quiet.status, ConnectionWorkBinding::DrainStatus::quiescent);
    REQUIRE(quiet.storage_released);
    REQUIRE_EQ(fixture.budget->reserved_bytes(), 0U);
    REQUIRE_EQ(fixture.runtime->buffer_packet_counts().available_receive, 0U);
    fixture.channel->unregister_connection(700);
    fixture.scheduler->stop();
    std::weak_ptr<ConnectionRuntime> old_runtime = fixture.runtime;
    fixture.runtime.reset();
    // The separate old setup handle still pins its original runtime identity.
    REQUIRE(!old_runtime.expired());
    prefix.reset();
    REQUIRE(old_runtime.expired());
}

TEST(channel_route_unregister_reclaims_quiet_ring_with_captured_handle)
{
    SinkFixture fixture;
    auto dispatcher = fixture.dispatcher(4);
    auto captured = dispatcher->inbox();
    REQUIRE(
        fixture.channel->register_connection(700, fixture.runtime, dispatcher));
    fixture.channel->unregister_connection(700);
    REQUIRE(captured->snapshot().closed);
    REQUIRE(captured->snapshot().storage_released);
    REQUIRE_EQ(fixture.budget->reserved_bytes(), 0U);
    REQUIRE(fixture.runtime->accepts_datagrams());
    REQUIRE(fixture.channel->retire_connection(700) == nullptr);
    REQUIRE_EQ(
        captured->publish(captured->token(), sink_data(0).view(), sink_peer, 0),
        ConnectionDatagramInbox::Status::closed);
}

TEST(channel_route_retirement_receipt_survives_socket_id_reuse)
{
    SinkFixture fixture;
    auto gate = std::make_shared<SinkGate>();
    SinkRelease release {gate};
    auto old = fixture.dispatcher(4, gate);
    auto captured = old->inbox();
    REQUIRE(fixture.channel->register_connection(700, fixture.runtime, old));
    (void)fixture.ingress(sink_data(0));
    gate->wait();
    auto receipt = fixture.channel->retire_connection(700);
    REQUIRE(receipt == old);
    fixture.runtime->close();
    REQUIRE_EQ(
        receipt->finish_retirement(std::chrono::steady_clock::now()).status,
        ConnectionWorkBinding::DrainStatus::timeout);
    old.reset();
    auto next_runtime = fixture.make_runtime(91, nullptr, false);
    auto next = ConnectionDatagramDispatcher::create(next_runtime,
        fixture.scheduler, fixture.budget, 0, sink_peer,
        {.capacity = 4, .control_reserve = 1}, {.turn_budget = 2});
    REQUIRE(next != nullptr);
    REQUIRE(fixture.channel->register_connection(700, next_runtime, next));
    (void)fixture.ingress(sink_data(0));
    sink_receive(next_runtime, std::byte {1});
    REQUIRE_EQ(
        captured->publish(captured->token(), sink_data(1).view(), sink_peer, 0),
        ConnectionDatagramInbox::Status::closed);
    gate->release();
    REQUIRE(receipt
            ->finish_retirement(
                std::chrono::steady_clock::now() + std::chrono::seconds {2})
            .storage_released);
    REQUIRE_EQ(fixture.runtime->buffer_packet_counts().available_receive, 0U);
    REQUIRE_EQ(fixture.budget->reserved_bytes(),
        *ConnectionDatagramInbox::storage_bytes(4));
    receipt.reset();
    captured.reset();
    (void)fixture.ingress(sink_data(1));
    sink_receive(next_runtime, std::byte {2});
    auto next_receipt = fixture.channel->retire_connection(700);
    REQUIRE(next_receipt == next);
    REQUIRE(next_receipt
            ->finish_retirement(
                std::chrono::steady_clock::now() + std::chrono::seconds {2})
            .storage_released);
    REQUIRE_EQ(fixture.budget->reserved_bytes(), 0U);
}

TEST(channel_route_retirement_receipt_rejects_worker_drain)
{
    SinkFixture fixture;
    struct Probe {
        std::weak_ptr<DatagramChannel> channel;
        std::promise<ConnectionDatagramDispatcher::DrainResult> answer;
        static void run(void* pointer) noexcept
        {
            auto& self = *static_cast<Probe*>(pointer);
            const auto channel = self.channel.lock();
            const auto receipt =
                channel != nullptr ? channel->retire_connection(700) : nullptr;
            self.answer.set_value(receipt != nullptr
                    ? receipt->finish_retirement(
                          std::chrono::steady_clock::now()
                          + std::chrono::seconds {10})
                    : ConnectionDatagramDispatcher::DrainResult {
                          ConnectionWorkBinding::DrainStatus::not_retired,
                          false});
        }
    };
    auto probe = std::make_shared<Probe>();
    probe->channel = fixture.channel;
    auto answer = probe->answer.get_future();
    auto dispatcher =
        ConnectionDatagramDispatcher::create(fixture.runtime, fixture.scheduler,
            fixture.budget, 1, sink_peer, {.capacity = 4, .control_reserve = 1},
            {.turn_budget = 2,
                .after_pop_for_testing = Probe::run,
                .after_pop_context_for_testing = probe});
    REQUIRE(dispatcher != nullptr);
    REQUIRE(
        fixture.channel->register_connection(700, fixture.runtime, dispatcher));
    (void)fixture.ingress(sink_data(0));
    REQUIRE_EQ(
        answer.wait_for(std::chrono::seconds {2}), std::future_status::ready);
    const auto result = answer.get();
    REQUIRE_EQ(
        result.status, ConnectionWorkBinding::DrainStatus::worker_thread);
    REQUIRE(!result.storage_released);
    REQUIRE(fixture.channel->retire_connection(700) == nullptr);
    fixture.scheduler->stop();
    REQUIRE_EQ(fixture.runtime->buffer_packet_counts().available_receive, 1U);
    REQUIRE(dispatcher->finish_retirement(std::chrono::steady_clock::now())
            .storage_released);
    REQUIRE_EQ(fixture.budget->reserved_bytes(), 0U);
}

TEST(channel_fatal_fault_retires_dispatcher_and_breaks_direct_routes)
{
    SinkFixture fixture;
    auto gate = std::make_shared<SinkGate>();
    SinkRelease release {gate};
    auto dispatcher = fixture.dispatcher(4, gate);
    auto captured = dispatcher->inbox();
    auto direct = fixture.make_runtime(91, nullptr, false);
    REQUIRE(
        fixture.channel->register_connection(700, fixture.runtime, dispatcher));
    REQUIRE(fixture.channel->register_connection(701, direct));
    (void)fixture.ingress(sink_data(0));
    gate->wait();
    auto fatal = [](std::span<std::byte>) noexcept {
        return UdpIoResult {.error = Error::io_error, .system_error = EIO};
    };
    (void)fixture.channel->run_once_for_testing(fatal);
    REQUIRE(fixture.runtime->broken());
    REQUIRE(direct->broken());
    REQUIRE(captured->snapshot().closed);
    REQUIRE(!captured->snapshot().storage_released);
    REQUIRE(!fixture.channel->register_connection(
        702, fixture.make_runtime(92, nullptr, false)));
    REQUIRE(!fixture.channel->replay_established_handshake(
        {.message = decode_handshake_datagram(sink_replay().view()).message,
            .peer = sink_peer}));
    auto receipt = fixture.channel->retire_connection(700);
    REQUIRE(receipt == dispatcher);
    REQUIRE_EQ(
        receipt->finish_retirement(std::chrono::steady_clock::now()).status,
        ConnectionWorkBinding::DrainStatus::timeout);
    gate->release();
    REQUIRE(receipt
            ->finish_retirement(
                std::chrono::steady_clock::now() + std::chrono::seconds {2})
            .storage_released);
    REQUIRE_EQ(fixture.runtime->buffer_packet_counts().available_receive, 0U);
    REQUIRE_EQ(fixture.budget->reserved_bytes(), 0U);
    fixture.channel->unregister_connection(701);
    // The first fatal error is terminal; repeated fanout cannot reopen admission.
    (void)fixture.channel->run_once_for_testing(fatal);
    REQUIRE(!fixture.channel->register_connection(700, direct));
}

TEST(channel_fault_fanout_releases_route_lock_and_follows_concurrent_detach)
{
    SinkFixture fixture;
    struct Clock {
        std::atomic_bool armed {false};
        std::shared_ptr<SinkGate> gate = std::make_shared<SinkGate>();
        static std::uint64_t now(void* pointer) noexcept
        {
            auto& self = *static_cast<Clock*>(pointer);
            if (self.armed.exchange(false)) {
                SinkGate::block(self.gate.get());
            }
            return 1000;
        }
    } clock;
    SocketOptions options;
    REQUIRE_EQ(options.set(SocketOption::tsbpd_mode, 0), Error::none);
    auto first = std::make_shared<ConnectionRuntime>(
        ConnectionRuntime::Configuration {.channel = fixture.channel,
            .peer = sink_peer,
            .peer_socket_id = 93,
            .initial_sequence = SequenceNumber {1000},
            .options = options,
            .origin = ConnectionRuntime::Clock::now(),
            .now_function = Clock::now,
            .now_context = &clock});
    auto pending = fixture.dispatcher(4);
    auto last = fixture.make_runtime(94, nullptr, false);
    REQUIRE(fixture.channel->register_connection(698, first));
    REQUIRE(
        fixture.channel->register_connection(699, fixture.runtime, pending));
    REQUIRE(fixture.channel->register_connection(700, last));
    std::future<void> fault;
    std::future<std::shared_ptr<ConnectionDatagramDispatcher>> detached;
    SinkRelease release {clock.gate};
    clock.armed.store(true);
    fault = std::async(std::launch::async, [&] {
        auto fatal = [](std::span<std::byte>) noexcept {
            return UdpIoResult {.error = Error::io_error, .system_error = EIO};
        };
        (void)fixture.channel->run_once_for_testing(fatal);
    });
    clock.gate->wait();
    detached = std::async(std::launch::async, [&] {
        (void)fixture.channel->retire_connection(698);
        return fixture.channel->retire_connection(699);
    });
    // The runtime clock is paused under its mutex. Detach needs only the route
    // lock, proving fanout does not retain that lock through protocol work.
    REQUIRE_EQ(
        detached.wait_for(std::chrono::seconds {2}), std::future_status::ready);
    auto receipt = detached.get();
    REQUIRE(receipt == pending);
    REQUIRE(receipt->finish_retirement(std::chrono::steady_clock::now())
            .storage_released);
    clock.gate->release();
    fault.get();
    REQUIRE(first->broken());
    REQUIRE(last->broken());
    REQUIRE(!fixture.runtime->broken());
    REQUIRE_EQ(fixture.budget->reserved_bytes(), 0U);
    fixture.channel->unregister_connection(700);
}

TEST(channel_fault_fanout_retires_both_active_and_pending_shard_services)
{
    SinkFixture fixture;
    auto active_gate = std::make_shared<SinkGate>();
    auto pending_gate = std::make_shared<SinkGate>();
    SinkRelease active_release {active_gate};
    SinkRelease pending_release {pending_gate};
    auto active = fixture.dispatcher(4, active_gate);
    auto other_runtime = fixture.make_runtime(91, nullptr, false);
    auto pending = ConnectionDatagramDispatcher::create(other_runtime,
        fixture.scheduler, fixture.budget, 0, sink_peer,
        {.capacity = 4, .control_reserve = 1}, {.turn_budget = 2});
    REQUIRE(pending != nullptr);
    REQUIRE(fixture.channel->register_connection(700, fixture.runtime, active));
    REQUIRE(fixture.channel->register_connection(701, other_runtime, pending));
    REQUIRE_EQ(fixture.scheduler->submit(
                   0, {.function = SinkGate::block, .context = pending_gate}),
        RuntimeScheduler::SubmitStatus::accepted);
    pending_gate->wait();
    auto active_inbox = active->inbox();
    auto pending_inbox = pending->inbox();
    REQUIRE_EQ(
        active->publish(active_inbox->token(), sink_data(0).view(), sink_peer),
        ConnectionDatagramInbox::Status::accepted);
    active_gate->wait();
    REQUIRE_EQ(pending->publish(
                   pending_inbox->token(), sink_data(0).view(), sink_peer),
        ConnectionDatagramInbox::Status::accepted);
    auto fatal = [](std::span<std::byte>) noexcept {
        return UdpIoResult {.error = Error::io_error, .system_error = EIO};
    };
    (void)fixture.channel->run_once_for_testing(fatal);
    REQUIRE(fixture.runtime->broken());
    REQUIRE(other_runtime->broken());
    REQUIRE(active_inbox->snapshot().closed);
    REQUIRE(pending_inbox->snapshot().closed);
    REQUIRE_EQ(pending_inbox->snapshot().queued, 0U);
    fixture.channel->unregister_connection(701);
    REQUIRE(pending_inbox->snapshot().storage_released);
    REQUIRE_EQ(fixture.budget->reserved_bytes(),
        *ConnectionDatagramInbox::storage_bytes(4));
    auto receipt = fixture.channel->retire_connection(700);
    REQUIRE_EQ(
        receipt->finish_retirement(std::chrono::steady_clock::now()).status,
        ConnectionWorkBinding::DrainStatus::timeout);
    active_gate->release();
    REQUIRE(receipt
            ->finish_retirement(
                std::chrono::steady_clock::now() + std::chrono::seconds {2})
            .storage_released);
    pending_gate->release();
    fixture.scheduler->stop();
    REQUIRE_EQ(fixture.runtime->buffer_packet_counts().available_receive, 0U);
    REQUIRE_EQ(other_runtime->buffer_packet_counts().available_receive, 0U);
    REQUIRE_EQ(fixture.budget->reserved_bytes(), 0U);
    REQUIRE_EQ(active_inbox->publish(
                   active_inbox->token(), sink_data(1).view(), sink_peer, 0),
        ConnectionDatagramInbox::Status::closed);
    REQUIRE_EQ(pending_inbox->publish(
                   pending_inbox->token(), sink_data(1).view(), sink_peer, 0),
        ConnectionDatagramInbox::Status::closed);
}

TEST(channel_transient_receive_error_preserves_route_admission)
{
    SinkFixture fixture;
    auto dispatcher = fixture.dispatcher(4);
    REQUIRE(
        fixture.channel->register_connection(700, fixture.runtime, dispatcher));
#if defined(_WIN32)
    constexpr int interrupted_receive = WSAEINTR;
#else
    constexpr int interrupted_receive = EINTR;
#endif
    REQUIRE(UdpSocket::is_transient_receive_error(interrupted_receive));
    bool received = false;
    auto transient = [&](std::span<std::byte>) noexcept {
        if (std::exchange(received, true)) {
            return UdpIoResult {.error = Error::would_block};
        }
        return UdpIoResult {
            .error = Error::io_error, .system_error = interrupted_receive};
    };
    (void)fixture.channel->run_once_for_testing(transient);
    REQUIRE(!fixture.runtime->broken());
    REQUIRE(!dispatcher->inbox()->snapshot().closed);
    REQUIRE(fixture.channel->register_connection(
        701, fixture.make_runtime(91, nullptr, false)));
    (void)fixture.ingress(sink_data(0));
    sink_receive(fixture.runtime, std::byte {1});
    fixture.channel->unregister_connection(700);
    fixture.channel->unregister_connection(701);
}

TEST(channel_owning_close_reclaims_after_owner_drops_active_receipt)
{
    SinkFixture fixture;
    auto gate = std::make_shared<SinkGate>();
    SinkRelease release {gate};
    auto dispatcher = fixture.dispatcher(4, gate);
    auto captured = dispatcher->inbox();
    REQUIRE(
        fixture.channel->register_connection(700, fixture.runtime, dispatcher));
    (void)fixture.ingress(sink_data(0));
    gate->wait();
    auto receipt = fixture.channel->retire_connection(700);
    close_connection_runtime(fixture.runtime, {}, receipt);
    REQUIRE(captured->snapshot().closed);
    REQUIRE(!captured->snapshot().storage_released);
    std::weak_ptr<ConnectionDatagramDispatcher> old = dispatcher;
    dispatcher.reset();
    receipt.reset();
    REQUIRE(old.expired());
    REQUIRE_EQ(fixture.budget->reserved_bytes(),
        *ConnectionDatagramInbox::storage_bytes(4));
    gate->release();
    fixture.scheduler->stop();
    REQUIRE(captured->snapshot().storage_released);
    REQUIRE_EQ(fixture.budget->reserved_bytes(), 0U);
    REQUIRE_EQ(fixture.runtime->buffer_packet_counts().available_receive, 0U);
    auto next_scheduler = std::make_shared<RuntimeScheduler>(
        RuntimeScheduler::Configuration {.shard_count = 1,
            .queue_capacity_per_shard = 1,
            .timer_capacity_per_shard = 1,
            .service_capacity_per_shard = 1});
    REQUIRE(next_scheduler->start());
    auto next_runtime = fixture.make_runtime(91, nullptr, false);
    auto next = ConnectionDatagramDispatcher::create(next_runtime,
        next_scheduler, fixture.budget, 0, sink_peer,
        {.capacity = 4, .control_reserve = 1}, {.turn_budget = 2});
    REQUIRE(next != nullptr);
    REQUIRE_EQ(next->inbox()->publish(
                   captured->token(), sink_data(1).view(), sink_peer, 0),
        ConnectionDatagramInbox::Status::stale);
    captured.reset();
    REQUIRE_EQ(fixture.budget->reserved_bytes(),
        *ConnectionDatagramInbox::storage_bytes(4));
    next->retire_and_reclaim();
    next_scheduler->stop();
    REQUIRE_EQ(fixture.budget->reserved_bytes(), 0U);
}

TEST(channel_duplicate_owning_close_reclaims_quiet_receipt)
{
    SinkFixture fixture;
    auto dispatcher = fixture.dispatcher(4);
    auto captured = dispatcher->inbox();
    REQUIRE(
        fixture.channel->register_connection(700, fixture.runtime, dispatcher));
    // Registration can wake an empty callback. Join it to prove this case is
    // quiet rather than depending on how quickly the worker starts.
    fixture.scheduler->stop();
    // Runtime close may already have happened before this owner detaches.
    fixture.runtime->close();
    auto receipt = fixture.channel->retire_connection(700);
    close_connection_runtime(fixture.runtime, {}, receipt);
    REQUIRE(captured->snapshot().storage_released);
    REQUIRE_EQ(fixture.budget->reserved_bytes(), 0U);
    close_connection_runtime(fixture.runtime, {}, receipt);
    REQUIRE_EQ(fixture.budget->reserved_bytes(), 0U);
}

TEST(channel_worker_owning_close_full_executor_keeps_callback_reclamation)
{
    SinkFixture fixture;
    auto executor = std::make_shared<RuntimeWorkExecutor>(
        RuntimeWorkExecutor::Configuration {1, 1});
    REQUIRE(executor->start());
    auto executor_gate = std::make_shared<SinkGate>();
    SinkRelease release_executor {executor_gate};
    REQUIRE_EQ(executor->submit(
                   {.function = SinkGate::block, .context = executor_gate}),
        RuntimeWorkExecutor::SubmitStatus::accepted);
    executor_gate->wait();
    REQUIRE_EQ(executor->submit({.function = [](void*) noexcept { },
                   .context = executor_gate}),
        RuntimeWorkExecutor::SubmitStatus::accepted);
    struct Probe {
        std::weak_ptr<DatagramChannel> channel;
        std::weak_ptr<ConnectionRuntime> runtime;
        std::shared_ptr<RuntimeWorkExecutor> executor;
        std::promise<bool> closed;
        static void run(void* pointer) noexcept
        {
            auto& self = *static_cast<Probe*>(pointer);
            const auto channel = self.channel.lock();
            const auto runtime = self.runtime.lock();
            if (channel == nullptr || runtime == nullptr) {
                self.closed.set_value(false);
                return;
            }
            auto receipt = channel->retire_connection(700);
            close_connection_runtime(
                runtime, self.executor, std::move(receipt));
            self.closed.set_value(!runtime->accepts_datagrams());
        }
    };
    auto probe = std::make_shared<Probe>();
    probe->channel = fixture.channel;
    probe->runtime = fixture.runtime;
    probe->executor = executor;
    auto closed = probe->closed.get_future();
    auto dispatcher =
        ConnectionDatagramDispatcher::create(fixture.runtime, fixture.scheduler,
            fixture.budget, 1, sink_peer, {.capacity = 4, .control_reserve = 1},
            {.turn_budget = 2,
                .after_pop_for_testing = Probe::run,
                .after_pop_context_for_testing = probe});
    REQUIRE(dispatcher != nullptr);
    auto captured = dispatcher->inbox();
    REQUIRE(
        fixture.channel->register_connection(700, fixture.runtime, dispatcher));
    (void)fixture.ingress(sink_data(0));
    REQUIRE_EQ(
        closed.wait_for(std::chrono::seconds {2}), std::future_status::ready);
    REQUIRE(closed.get());
    fixture.scheduler->stop();
    REQUIRE(captured->snapshot().storage_released);
    REQUIRE_EQ(fixture.budget->reserved_bytes(), 0U);
    REQUIRE_EQ(fixture.runtime->buffer_packet_counts().available_receive, 0U);
    REQUIRE_EQ(executor->snapshot().rejected_full, 1U);
    executor_gate->release();
    executor->stop();
}

TEST(channel_registry_close_owns_active_route_reclamation)
{
    SinkFixture fixture;
    auto gate = std::make_shared<SinkGate>();
    SinkRelease release {gate};
    auto dispatcher = fixture.dispatcher(4, gate);
    auto captured = dispatcher->inbox();
    auto& registry = SocketRegistry::instance();
    const auto socket = registry.create();
    REQUIRE(socket != SRT_INVALID_SOCK);
    struct CloseHandle {
        SRTSOCKET socket;
        ~CloseHandle()
        {
            SocketRegistry::instance().close(socket);
        }
    } close_handle {socket};
    auto record = registry.find(socket);
    REQUIRE(record != nullptr);
    {
        std::lock_guard lock(record->mutex);
        record->channel = fixture.channel;
        record->runtime = fixture.runtime;
        record->state = SRTS_CONNECTED;
        record->public_options.linger_enabled = false;
    }
    REQUIRE(fixture.channel->register_connection(
        record->protocol_socket_id, fixture.runtime, dispatcher));
    REQUIRE_EQ(
        dispatcher->publish(captured->token(), sink_data(0).view(), sink_peer),
        ConnectionDatagramInbox::Status::accepted);
    gate->wait();
    registry.close(socket);
    {
        std::lock_guard lock(record->mutex);
        REQUIRE_EQ(record->state, SRTS_CLOSED);
    }
    REQUIRE(!fixture.runtime->accepts_datagrams());
    REQUIRE(captured->snapshot().closed);
    REQUIRE(!captured->snapshot().storage_released);
    dispatcher.reset();
    gate->release();
    fixture.scheduler->stop();
    REQUIRE(captured->snapshot().storage_released);
    REQUIRE_EQ(fixture.budget->reserved_bytes(), 0U);
    REQUIRE_EQ(fixture.runtime->buffer_packet_counts().available_receive, 0U);
}

TEST(
    channel_worker_owning_close_accepted_task_retains_receipt_until_final_drain)
{
    SinkFixture fixture;
    auto callback_gate = std::make_shared<SinkGate>();
    auto executor_gate = std::make_shared<SinkGate>();
    auto executor = std::make_shared<RuntimeWorkExecutor>(
        RuntimeWorkExecutor::Configuration {1, 1});
    REQUIRE(executor->start());
    SinkRelease callback_release {callback_gate};
    SinkRelease executor_release {executor_gate};
    REQUIRE_EQ(executor->submit(
                   {.function = SinkGate::block, .context = executor_gate}),
        RuntimeWorkExecutor::SubmitStatus::accepted);
    executor_gate->wait();
    auto dispatcher = fixture.dispatcher(4, callback_gate);
    auto captured = dispatcher->inbox();
    REQUIRE(
        fixture.channel->register_connection(700, fixture.runtime, dispatcher));
    (void)fixture.ingress(sink_data(0));
    callback_gate->wait();
    struct Close {
        std::shared_ptr<DatagramChannel> channel;
        std::shared_ptr<ConnectionRuntime> runtime;
        std::shared_ptr<RuntimeWorkExecutor> executor;
        std::promise<bool> done;
        static void run(void* pointer) noexcept
        {
            auto& self = *static_cast<Close*>(pointer);
            close_connection_runtime(self.runtime, self.executor,
                self.channel->retire_connection(700));
            self.done.set_value(!self.runtime->accepts_datagrams());
        }
    };
    auto close = std::make_shared<Close>();
    close->channel = fixture.channel;
    close->runtime = fixture.runtime;
    close->executor = executor;
    auto done = close->done.get_future();
    REQUIRE_EQ(fixture.scheduler->submit(
                   0, {.function = Close::run, .context = close}),
        RuntimeScheduler::SubmitStatus::accepted);
    REQUIRE_EQ(
        done.wait_for(std::chrono::seconds {2}), std::future_status::ready);
    REQUIRE(done.get());
    REQUIRE_EQ(executor->snapshot().queued, 1U);
    std::weak_ptr<ConnectionDatagramDispatcher> receipt = dispatcher;
    dispatcher.reset();
    REQUIRE(!receipt.expired());
    REQUIRE(!captured->snapshot().storage_released);
    callback_gate->release();
    fixture.scheduler->stop();
    REQUIRE(captured->snapshot().storage_released);
    REQUIRE_EQ(fixture.budget->reserved_bytes(), 0U);
    REQUIRE(!receipt.expired());
    executor_gate->release();
    executor->stop();
    REQUIRE(receipt.expired());
    REQUIRE_EQ(fixture.runtime->buffer_packet_counts().available_receive, 0U);
}
