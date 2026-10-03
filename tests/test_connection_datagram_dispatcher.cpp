#include "test.hpp"
#include "compat/connection_datagram_dispatcher.hpp"
#include "compat/transport_runtime.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>

using namespace robotweax::srt;
using namespace robotweax::srt::compat;
namespace {
constexpr Ipv4Endpoint dispatch_peer {
    .address = {192, 0, 2, 95}, .port = 14905};
using DispatchStatus = ConnectionDatagramInbox::Status;
struct DispatchGate {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool open = false;
    static void block(void* context) noexcept
    {
        auto& self = *static_cast<DispatchGate*>(context);
        std::unique_lock lock(self.mutex);
        self.entered = true;
        self.changed.notify_all();
        self.changed.wait(lock, [&] {
            return self.open;
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
struct DispatchRelease {
    std::shared_ptr<DispatchGate> gate;
    ~DispatchRelease()
    {
        gate->release();
    }
};
struct DispatchWire {
    std::array<std::byte, 1500> bytes {};
    std::size_t size = 0;
    std::span<const std::byte> view() const
    {
        return std::span {bytes}.first(size);
    }
};
DispatchWire dispatch_data(std::uint32_t offset)
{
    MutablePacketView packet;
    packet.kind = PacketKind::data;
    packet.data.sequence = SequenceNumber {1000 + offset};
    packet.data.destination_socket_id = 700;
    packet.data.message_number = 7 + offset;
    packet.data.boundary = MessageBoundary::solo;
    const std::array payload {static_cast<std::byte>(offset + 1)};
    packet.payload = payload;
    DispatchWire wire;
    const auto encoded = encode_packet(packet, wire.bytes);
    REQUIRE(encoded);
    wire.size = encoded.bytes_written;
    return wire;
}
struct DispatchFixture {
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
    std::shared_ptr<ConnectionRuntime> runtime;
    DispatchFixture()
    {
        REQUIRE(scheduler->start());
        channel->set_send_hook_for_testing(
            [](std::span<const std::byte> bytes, IpEndpoint, void*) noexcept {
                return UdpIoResult {.bytes_transferred = bytes.size()};
            },
            nullptr);
        SocketOptions options;
        REQUIRE_EQ(options.set(SocketOption::tsbpd_mode, 0), Error::none);
        HandshakeAction response;
        response.kind = HandshakeActionKind::send;
        response.packet.version = handshake_version_5;
        response.packet.request = HandshakeRequest::conclusion;
        response.packet.socket_id = 700;
        response.packet.syn_cookie = 0x12345678;
        runtime = std::make_shared<ConnectionRuntime>(
            ConnectionRuntime::Configuration {.channel = channel,
                .peer = dispatch_peer,
                .peer_socket_id = 90,
                .initial_sequence = SequenceNumber {1000},
                .options = options,
                .origin = ConnectionRuntime::Clock::now(),
                .handshake_replay_response = response,
                .handshake_replay_enabled = true});
    }
    ~DispatchFixture()
    {
        scheduler->stop();
    }
    std::shared_ptr<ConnectionDatagramDispatcher> create(
        std::size_t turn_budget = 2,
        std::shared_ptr<DispatchGate> hook = nullptr)
    {
        auto dispatcher =
            ConnectionDatagramDispatcher::create(runtime, scheduler, budget, 1,
                dispatch_peer, {.capacity = 16, .control_reserve = 1},
                {.turn_budget = turn_budget,
                    .after_pop_for_testing =
                        hook == nullptr ? nullptr : DispatchGate::block,
                    .after_pop_context_for_testing = hook});
        REQUIRE(dispatcher != nullptr);
        return dispatcher;
    }
};
void dispatch_receive(
    const std::shared_ptr<ConnectionRuntime>& runtime, std::byte expected)
{
    std::array<std::byte, 8> bytes {};
    const auto received = runtime->receive_message(bytes, true, 2000);
    REQUIRE_EQ(received.status, MessageIoStatus::success);
    REQUIRE_EQ(received.bytes, 1U);
    REQUIRE_EQ(bytes.front(), expected);
}
} // namespace

TEST(connection_datagram_dispatcher_drains_real_runtime_in_bounded_turns)
{
    DispatchFixture fixture;
    auto dispatcher = fixture.create();
    auto gate = std::make_shared<DispatchGate>();
    DispatchRelease release {gate};
    REQUIRE_EQ(fixture.scheduler->submit(
                   1, {.function = DispatchGate::block, .context = gate}),
        RuntimeScheduler::SubmitStatus::accepted);
    gate->wait();
    for (std::uint32_t index = 0; index < 9; ++index) {
        REQUIRE_EQ(dispatcher->publish(dispatcher->inbox()->token(),
                       dispatch_data(index).view(), dispatch_peer),
            DispatchStatus::accepted);
    }
    gate->release();
    for (std::uint32_t index = 0; index < 9; ++index) {
        dispatch_receive(fixture.runtime, static_cast<std::byte>(index + 1));
    }
    fixture.scheduler->stop();
    REQUIRE_EQ(dispatcher->snapshot().dispatched_datagrams, 9U);
    REQUIRE_EQ(dispatcher->snapshot().maximum_turn_datagrams, 2U);
    REQUIRE_EQ(dispatcher->snapshot().completed_turns, 5U);
    REQUIRE_EQ(dispatcher->inbox()->snapshot().queued, 0U);
}

TEST(
    connection_datagram_dispatcher_popped_copy_cannot_mutate_after_runtime_close)
{
    DispatchFixture fixture;
    auto gate = std::make_shared<DispatchGate>();
    auto dispatcher = fixture.create(2, gate);
    std::future<void> close;
    DispatchRelease release {gate};
    REQUIRE_EQ(dispatcher->publish(dispatcher->inbox()->token(),
                   dispatch_data(0).view(), dispatch_peer),
        DispatchStatus::accepted);
    gate->wait();
    REQUIRE(!dispatcher->quiescent());
    close = std::async(std::launch::async, [dispatcher] {
        dispatcher->close();
    });
    REQUIRE_EQ(
        close.wait_for(std::chrono::seconds {2}), std::future_status::ready);
    close.get();
    REQUIRE(!dispatcher->quiescent());
    REQUIRE_EQ(dispatcher->publish(dispatcher->inbox()->token(),
                   dispatch_data(1).view(), dispatch_peer),
        DispatchStatus::closed);
    gate->release();
    fixture.scheduler->stop();
    REQUIRE(dispatcher->quiescent());
    REQUIRE_EQ(fixture.runtime->buffer_packet_counts().available_receive, 0U);
    std::array<std::byte, 8> bytes {};
    REQUIRE_EQ(fixture.runtime->receive_message(bytes, false, 0).status,
        MessageIoStatus::local_closed);
}

TEST(
    connection_datagram_dispatcher_retirement_rejects_captured_inbox_without_closing_runtime)
{
    DispatchFixture fixture;
    auto dispatcher = fixture.create();
    auto captured = dispatcher->inbox();
    dispatcher->retire();
    REQUIRE_EQ(captured->publish(captured->token(), dispatch_data(0).view(),
                   dispatch_peer, 1),
        DispatchStatus::closed);
    REQUIRE(!fixture.runtime->terminal());
    dispatcher.reset();
    REQUIRE(fixture.budget->reserved_bytes() != 0U);
    captured.reset();
    fixture.scheduler->stop();
    REQUIRE_EQ(fixture.budget->reserved_bytes(), 0U);
}

TEST(
    connection_datagram_dispatcher_does_not_retain_runtime_through_service_context)
{
    DispatchFixture fixture;
    auto dispatcher = fixture.create();
    auto gate = std::make_shared<DispatchGate>();
    DispatchRelease release {gate};
    REQUIRE_EQ(fixture.scheduler->submit(
                   1, {.function = DispatchGate::block, .context = gate}),
        RuntimeScheduler::SubmitStatus::accepted);
    gate->wait();
    REQUIRE_EQ(dispatcher->publish(dispatcher->inbox()->token(),
                   dispatch_data(0).view(), dispatch_peer),
        DispatchStatus::accepted);
    std::weak_ptr<ConnectionRuntime> weak = fixture.runtime;
    fixture.runtime.reset();
    REQUIRE(weak.expired());
    gate->release();
    fixture.scheduler->stop();
    // Stop may cancel the not-yet-dispatched service; retirement still drops it.
    dispatcher->retire();
    REQUIRE_EQ(dispatcher->inbox()->snapshot().queued, 0U);
}

TEST(connection_datagram_dispatcher_failed_wake_breaks_actual_runtime)
{
    DispatchFixture fixture;
    auto dispatcher = fixture.create();
    fixture.scheduler->stop();
    REQUIRE_EQ(dispatcher->publish(dispatcher->inbox()->token(),
                   dispatch_data(0).view(), dispatch_peer),
        DispatchStatus::wake_failed);
    REQUIRE(fixture.runtime->broken());
    REQUIRE(dispatcher->inbox()->snapshot().closed);
}

TEST(connection_datagram_dispatcher_setup_failure_releases_service_and_storage)
{
    DispatchFixture fixture;
    REQUIRE(ConnectionDatagramDispatcher::create(fixture.runtime,
                fixture.scheduler, fixture.budget, 1, dispatch_peer,
                {.capacity = 16, .control_reserve = 1}, {.turn_budget = 0})
        == nullptr);
    REQUIRE(ConnectionDatagramDispatcher::create(fixture.runtime,
                fixture.scheduler, fixture.budget, 1, dispatch_peer,
                {.capacity = 16, .control_reserve = 1}, {.turn_budget = 17})
        == nullptr);
    auto no_storage = std::make_shared<DatagramStorageBudget>(0);
    REQUIRE(ConnectionDatagramDispatcher::create(fixture.runtime,
                fixture.scheduler, no_storage, 1, dispatch_peer,
                {.capacity = 16, .control_reserve = 1}, {})
        == nullptr);
    REQUIRE_EQ(fixture.budget->reserved_bytes(), 0U);
    auto dispatcher = fixture.create();
    REQUIRE(!fixture.runtime->terminal());
}

TEST(connection_datagram_dispatcher_external_runtime_close_stops_new_admission)
{
    DispatchFixture fixture;
    auto dispatcher = fixture.create();
    fixture.runtime->close();
    REQUIRE(!fixture.runtime->accepts_datagrams());
    REQUIRE_EQ(dispatcher->publish(dispatcher->inbox()->token(),
                   dispatch_data(0).view(), dispatch_peer),
        DispatchStatus::closed);
    REQUIRE(dispatcher->inbox()->snapshot().closed);
}

TEST(connection_datagram_dispatcher_preserves_peer_shutdown_datagram_semantics)
{
    DispatchFixture direct;
    DispatchFixture affine;
    struct SecondPop {
        std::shared_ptr<DispatchGate> gate;
        std::size_t count = 0;
        static void run(void* pointer) noexcept
        {
            auto& self = *static_cast<SecondPop*>(pointer);
            if (++self.count == 2) {
                DispatchGate::block(self.gate.get());
            }
        }
    };
    auto gate = std::make_shared<DispatchGate>();
    auto sequence = std::make_shared<SecondPop>();
    sequence->gate = gate;
    auto dispatcher = ConnectionDatagramDispatcher::create(affine.runtime,
        affine.scheduler, affine.budget, 1, dispatch_peer,
        {.capacity = 16, .control_reserve = 1},
        {.turn_budget = 1,
            .after_pop_for_testing = SecondPop::run,
            .after_pop_context_for_testing = sequence});
    REQUIRE(dispatcher != nullptr);
    MutablePacketView packet;
    packet.kind = PacketKind::control;
    packet.control.type = ControlType::shutdown;
    packet.control.destination_socket_id = 700;
    DispatchWire shutdown;
    const auto encoded = encode_packet(packet, shutdown.bytes);
    REQUIRE(encoded);
    shutdown.size = encoded.bytes_written;
    const auto first = dispatch_data(0);
    const auto late = dispatch_data(1);
    for (const auto bytes : {first.view(), shutdown.view(), late.view()}) {
        const auto decoded = decode_packet(bytes);
        REQUIRE(decoded);
        direct.runtime->process_packet(decoded.packet, dispatch_peer);
    }
    auto worker_gate = std::make_shared<DispatchGate>();
    DispatchRelease release {gate};
    DispatchRelease worker_release {worker_gate};
    REQUIRE_EQ(affine.scheduler->submit(1,
                   {.function = DispatchGate::block, .context = worker_gate}),
        RuntimeScheduler::SubmitStatus::accepted);
    worker_gate->wait();
    for (const auto bytes : {first.view(), shutdown.view(), late.view()}) {
        REQUIRE_EQ(dispatcher->publish(
                       dispatcher->inbox()->token(), bytes, dispatch_peer),
            DispatchStatus::accepted);
    }
    worker_gate->release();
    // The second pop follows delivery of the prefix DATA, before SHUTDOWN.
    gate->wait();
    REQUIRE_EQ(affine.runtime->buffer_packet_counts().available_receive, 1U);
    gate->release();
    affine.scheduler->stop();
    REQUIRE_EQ(dispatcher->snapshot().dispatched_datagrams, 2U);
    REQUIRE(affine.runtime->peer_closed());
    REQUIRE(!affine.runtime->accepts_datagrams());
    REQUIRE_EQ(affine.runtime->buffer_packet_counts().available_receive,
        direct.runtime->buffer_packet_counts().available_receive);
    for (std::size_t index = 0; index < 2; ++index) {
        std::array<std::byte, 8> direct_bytes {};
        std::array<std::byte, 8> affine_bytes {};
        const auto direct_result =
            direct.runtime->receive_message(direct_bytes, false, 0);
        const auto affine_result =
            affine.runtime->receive_message(affine_bytes, false, 0);
        REQUIRE_EQ(affine_result.status, direct_result.status);
        REQUIRE_EQ(affine_result.bytes, direct_result.bytes);
        REQUIRE_EQ(affine_bytes, direct_bytes);
    }
}

TEST(
    connection_datagram_dispatcher_decodes_handshake_replay_before_following_data)
{
    DispatchFixture fixture;
    std::atomic<std::size_t> replies {0};
    fixture.channel->set_send_hook_for_testing(
        [](std::span<const std::byte> bytes, IpEndpoint,
            void* context) noexcept {
            const auto decoded = decode_handshake_datagram(bytes);
            if (decoded) {
                static_cast<std::atomic<std::size_t>*>(context)->fetch_add(1);
            }
            return UdpIoResult {.bytes_transferred = bytes.size()};
        },
        &replies);
    auto dispatcher = fixture.create(1);
    HandshakeAction action;
    action.kind = HandshakeActionKind::send;
    action.packet.version = handshake_version_5;
    action.packet.request = HandshakeRequest::conclusion;
    action.packet.socket_id = 90;
    action.packet.syn_cookie = 0x12345678;
    DispatchWire wire;
    const auto encoded = encode_handshake_datagram(action, {}, 700, wire.bytes);
    REQUIRE(encoded);
    wire.size = encoded.bytes_written;
    REQUIRE_EQ(dispatcher->publish(
                   dispatcher->inbox()->token(), wire.view(), dispatch_peer),
        DispatchStatus::accepted);
    REQUIRE_EQ(dispatcher->publish(dispatcher->inbox()->token(),
                   dispatch_data(0).view(), dispatch_peer),
        DispatchStatus::accepted);
    dispatch_receive(fixture.runtime, std::byte {1});
    fixture.scheduler->stop();
    REQUIRE_EQ(replies.load(), 1U);
    REQUIRE_EQ(dispatcher->snapshot().dispatched_datagrams, 2U);
}

TEST(
    connection_datagram_dispatcher_stale_token_cannot_target_replacement_runtime)
{
    DispatchFixture old;
    DispatchFixture replacement;
    auto old_dispatcher = old.create();
    const auto stale = old_dispatcher->inbox()->token();
    old_dispatcher->close();
    auto next = replacement.create();
    REQUIRE_EQ(next->publish(stale, dispatch_data(0).view(), dispatch_peer),
        DispatchStatus::stale);
    std::array<std::byte, 8> bytes {};
    REQUIRE_EQ(replacement.runtime->receive_message(bytes, false, 0).status,
        MessageIoStatus::would_block);
    REQUIRE_EQ(next->publish(next->inbox()->token(), dispatch_data(0).view(),
                   dispatch_peer),
        DispatchStatus::accepted);
    dispatch_receive(replacement.runtime, std::byte {1});
}
