#include "test.hpp"
#include "compat/connection_datagram_dispatcher.hpp"
#include "compat/transport_runtime.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <thread>

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
