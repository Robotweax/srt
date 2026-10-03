#include "test.hpp"

#include "compat/connection_work_binding.hpp"
#include "compat/transport_runtime.hpp"
#include "robotweax/srt/codec.hpp"

#include <array>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

using namespace robotweax::srt;
using namespace robotweax::srt::compat;

namespace {

constexpr Ipv4Endpoint route_peer {.address = {192, 0, 2, 91}, .port = 14901};

struct RoutePopGate {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool open = false;

    static void block(void* pointer) noexcept
    {
        auto& gate = *static_cast<RoutePopGate*>(pointer);
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

struct RoutePopRelease {
    RoutePopGate& gate;
    ~RoutePopRelease()
    {
        gate.release();
    }
};

std::uint64_t route_now(void*) noexcept
{
    return 1000;
}

std::shared_ptr<DatagramChannel> route_channel()
{
    auto channel = std::make_shared<DatagramChannel>();
    channel->set_send_hook_for_testing(
        [](std::span<const std::byte> bytes, IpEndpoint, void*) noexcept {
            return UdpIoResult {.bytes_transferred = bytes.size()};
        },
        nullptr);
    return channel;
}

std::shared_ptr<ConnectionRuntime> route_runtime(
    const std::shared_ptr<DatagramChannel>& channel, std::uint32_t peer_id = 90,
    RoutePopGate* gate = nullptr,
    std::shared_ptr<ConnectionWorkBinding> binding = nullptr)
{
    SocketOptions options;
    REQUIRE_EQ(
        options.set(SocketOption::maximum_payload_size, 800), Error::none);
    REQUIRE_EQ(options.set(SocketOption::tsbpd_mode, 0), Error::none);
    HandshakeAction response;
    response.kind = HandshakeActionKind::send;
    response.packet.version = handshake_version_5;
    response.packet.request = HandshakeRequest::conclusion;
    response.packet.socket_id = 700;
    response.packet.syn_cookie = 0x12345678;
    return std::make_shared<ConnectionRuntime>(
        ConnectionRuntime::Configuration {
            .channel = channel,
            .peer = route_peer,
            .peer_socket_id = peer_id,
            .initial_sequence = SequenceNumber {1000},
            .options = options,
            .origin = ConnectionRuntime::Clock::now(),
            .handshake_replay_response = response,
            .handshake_replay_enabled = true,
            .now_function = route_now,
            .receive_pop_hook_for_testing =
                gate == nullptr ? nullptr : RoutePopGate::block,
            .receive_pop_context_for_testing = gate,
            .work_binding = std::move(binding),
        });
}

void route_publish(const std::shared_ptr<ConnectionRuntime>& runtime)
{
    const std::array<std::byte, 8> input {};
    PacketView packet;
    packet.kind = PacketKind::data;
    packet.data.sequence = SequenceNumber {1000};
    packet.data.message_number = 7;
    packet.data.boundary = MessageBoundary::solo;
    packet.payload = input;
    runtime->process_packet(packet, route_peer);
}

struct RetirementProbe {
    std::mutex mutex;
    std::condition_variable changed;
    bool requested = false;
    bool completed = false;
    bool completed_during_destruction = false;
    bool registered = false;
};

struct RetirementContext {
    std::shared_ptr<RetirementProbe> probe;

    ~RetirementContext()
    {
        std::unique_lock lock(probe->mutex);
        probe->requested = true;
        probe->changed.notify_all();
        // The external route operation must finish before this destructor
        // returns. A regression holding routes_mutex_ times out, then unwinds
        // safely and lets that operation finish; no async destructor deadlock.
        probe->completed_during_destruction =
            probe->changed.wait_for(lock, std::chrono::seconds {2}, [&] {
                return probe->completed;
            });
    }
};

struct RetirementWorkerJoin {
    std::shared_ptr<RetirementProbe> probe;
    std::thread& worker;

    ~RetirementWorkerJoin()
    {
        {
            std::lock_guard lock(probe->mutex);
            probe->requested = true;
            probe->changed.notify_all();
        }
        worker.join();
    }
};

} // namespace

TEST(channel_route_key_is_setup_identity_even_after_close)
{
    auto channel = route_channel();
    auto runtime = route_runtime(channel);
    const auto key = runtime->handshake_replay_key();
    REQUIRE(key.has_value());
    REQUIRE_EQ(key->peer, IpEndpoint {route_peer});
    REQUIRE_EQ(key->peer_socket_id, 90U);
    runtime->close();
    REQUIRE_EQ(runtime->handshake_replay_key(), key);
    ConnectionRuntime disabled {{.origin = ConnectionRuntime::Clock::now()}};
    REQUIRE(!disabled.handshake_replay_key().has_value());
    ConnectionRuntime no_response {{.origin = ConnectionRuntime::Clock::now(),
        .handshake_replay_enabled = true}};
    REQUIRE(!no_response.handshake_replay_key().has_value());
}

TEST(channel_route_key_register_and_unregister_do_not_wait_for_runtime_mutex)
{
    auto channel = route_channel();
    RoutePopGate gate;
    auto runtime = route_runtime(channel, 90, &gate);
    route_publish(runtime);
    std::future<MessageIoResult> receive;
    std::future<bool> route;
    RoutePopRelease release {gate};
    receive = std::async(std::launch::async, [runtime] {
        std::array<std::byte, 8> output {};
        return runtime->receive_message(output, false, 0);
    });
    gate.wait();
    route = std::async(std::launch::async, [channel, runtime] {
        if (!runtime->handshake_replay_key().has_value()
            || !channel->register_connection(700, runtime)) {
            return false;
        }
        channel->unregister_connection(700);
        return true;
    });
    REQUIRE_EQ(
        route.wait_for(std::chrono::seconds {2}), std::future_status::ready);
    REQUIRE(route.get());
    // Runtime is still held at its real receive-pop hook until released here.
    gate.release();
    REQUIRE_EQ(receive.get().status, MessageIoStatus::success);
}

TEST(channel_route_key_collision_rolls_back_only_the_new_data_route)
{
    auto channel = route_channel();
    auto first = route_runtime(channel);
    auto duplicate = route_runtime(channel);
    auto other = route_runtime(channel, 91);
    REQUIRE(channel->register_connection(700, first));
    REQUIRE(!channel->register_connection(701, duplicate));
    REQUIRE(channel->register_connection(701, other));
    REQUIRE(!channel->register_connection(700, other));
    channel->unregister_connection(700);
    REQUIRE(channel->register_connection(702, duplicate));
    channel->unregister_connection(701);
    channel->unregister_connection(702);
    REQUIRE(channel->register_connection(700, first));
    REQUIRE(channel->register_connection(701, other));
}

TEST(channel_route_key_promotion_preserves_setup_data_and_key_retirement)
{
    auto channel = route_channel();
    auto runtime = route_runtime(channel);
    auto inbox = std::make_shared<DatagramInbox>(2);
    REQUIRE(channel->register_setup_inbox(700, route_peer, inbox));
    const std::array<std::byte, 8> payload {};
    MutablePacketView packet;
    packet.kind = PacketKind::data;
    packet.data.sequence = SequenceNumber {1000};
    packet.data.destination_socket_id = 700;
    packet.data.message_number = 7;
    packet.data.boundary = MessageBoundary::solo;
    packet.payload = payload;
    std::array<std::byte, 1500> wire {};
    const auto encoded = encode_packet(packet, wire);
    REQUIRE(encoded);
    REQUIRE(
        inbox->push(std::span {wire}.first(encoded.bytes_written), route_peer));
    REQUIRE(channel->promote_setup_connection(700, inbox, runtime));
    REQUIRE(!inbox->ready());
    std::array<std::byte, 8> output;
    REQUIRE_EQ(runtime->receive_message(output, false, 0).status,
        MessageIoStatus::success);
    REQUIRE_EQ(output, payload);
    REQUIRE(!channel->promote_setup_connection(700, inbox, runtime));
    channel->unregister_connection(700);
    REQUIRE(channel->register_connection(701, route_runtime(channel)));
}

TEST(channel_route_key_last_runtime_retirement_releases_routes_mutex)
{
    auto channel = route_channel();
    auto scheduler = std::make_shared<RuntimeScheduler>(
        RuntimeScheduler::Configuration {.shard_count = 1,
            .queue_capacity_per_shard = 1,
            .timer_capacity_per_shard = 1,
            .service_capacity_per_shard = 1});
    REQUIRE(scheduler->start());
    auto probe = std::make_shared<RetirementProbe>();
    auto inbox = std::make_shared<DatagramInbox>(1);
    std::thread worker {[channel, probe, inbox] {
        {
            std::unique_lock lock(probe->mutex);
            probe->changed.wait(lock, [&] {
                return probe->requested;
            });
        }
        const bool registered =
            channel->register_setup_inbox(701, route_peer, inbox);
        std::lock_guard lock(probe->mutex);
        probe->registered = registered;
        probe->completed = true;
        probe->changed.notify_all();
    }};
    RetirementWorkerJoin join {probe, worker};
    auto context = std::make_shared<RetirementContext>();
    context->probe = probe;
    auto binding = ConnectionWorkBinding::create(
        scheduler, 0, [](void*, ConnectionWorkHints) noexcept { }, context);
    REQUIRE(binding != nullptr);
    auto runtime = route_runtime(channel, 90, nullptr, binding);
    REQUIRE(channel->register_connection(700, runtime));
    context.reset();
    binding.reset();
    runtime.reset();
    channel->unregister_connection(700);
    {
        std::lock_guard lock(probe->mutex);
        REQUIRE(probe->completed_during_destruction);
        REQUIRE(probe->registered);
    }
    scheduler->stop();
}

namespace {

struct SetupWire {
    std::array<std::byte, 1500> bytes {};
    std::size_t size = 0;
    std::span<const std::byte> view() const
    {
        return std::span {bytes}.first(size);
    }
};

SetupWire setup_data(std::uint32_t sequence, std::byte value)
{
    const std::array payload {value};
    MutablePacketView packet;
    packet.kind = PacketKind::data;
    packet.data.sequence = SequenceNumber {sequence};
    packet.data.destination_socket_id = 700;
    packet.data.message_number = sequence - 993;
    packet.data.boundary = MessageBoundary::solo;
    packet.payload = payload;
    SetupWire wire;
    const auto encoded = encode_packet(packet, wire.bytes);
    REQUIRE(encoded);
    wire.size = encoded.bytes_written;
    return wire;
}

SetupWire setup_conclusion()
{
    HandshakeAction action;
    action.kind = HandshakeActionKind::send;
    action.packet.version = handshake_version_5;
    action.packet.request = HandshakeRequest::conclusion;
    action.packet.socket_id = 90;
    action.packet.syn_cookie = 0x12345678;
    SetupWire wire;
    const auto encoded = encode_handshake_datagram(action, {}, 700, wire.bytes);
    REQUIRE(encoded);
    wire.size = encoded.bytes_written;
    return wire;
}

void require_setup_message(
    const std::shared_ptr<ConnectionRuntime>& runtime, std::byte value)
{
    std::array<std::byte, 8> output {};
    const auto result = runtime->receive_message(output, false, 0);
    REQUIRE_EQ(result.status, MessageIoStatus::success);
    REQUIRE_EQ(result.bytes, 1U);
    REQUIRE_EQ(output.front(), value);
}

} // namespace

TEST(channel_setup_transfer_late_publisher_survives_setup_close_and_id_reuse)
{
    auto channel = route_channel();
    auto runtime = route_runtime(channel);
    auto inbox = std::make_shared<DatagramInbox>(1);
    REQUIRE(channel->register_setup_inbox(700, route_peer, inbox));
    REQUIRE(inbox->push(setup_data(1000, std::byte {1}).view(), route_peer));
    // This reference models a dispatch which captured the setup route before
    // promotion, but has not yet published its datagram.
    auto captured = inbox;
    REQUIRE(channel->promote_setup_connection(700, inbox, runtime));
    inbox->close();
    channel->unregister_connection(700);
    auto replacement = route_runtime(channel, 91);
    REQUIRE(channel->register_connection(700, replacement));
    REQUIRE(captured->push(setup_data(1001, std::byte {2}).view(), route_peer));
    require_setup_message(runtime, std::byte {1});
    require_setup_message(runtime, std::byte {2});
    std::array<std::byte, 8> output {};
    REQUIRE_EQ(replacement->receive_message(output, false, 0).status,
        MessageIoStatus::would_block);
    runtime->close();
    REQUIRE(captured->push(setup_data(1002, std::byte {3}).view(), route_peer));
    REQUIRE_EQ(runtime->receive_message(output, false, 0).status,
        MessageIoStatus::local_closed);
}

TEST(channel_setup_transfer_seals_prefix_without_holding_routes_during_protocol)
{
    auto channel = route_channel();
    RoutePopGate gate;
    channel->set_send_hook_for_testing(
        [](std::span<const std::byte> bytes, IpEndpoint,
            void* context) noexcept {
            RoutePopGate::block(context);
            return UdpIoResult {.bytes_transferred = bytes.size()};
        },
        &gate);
    auto runtime = route_runtime(channel);
    auto inbox = std::make_shared<DatagramInbox>(2);
    REQUIRE(channel->register_setup_inbox(700, route_peer, inbox));
    REQUIRE(inbox->push(setup_conclusion().view(), route_peer));
    REQUIRE(inbox->push(setup_data(1000, std::byte {1}).view(), route_peer));
    auto replacement = route_runtime(channel, 91);
    std::future<bool> promotion;
    std::future<bool> other_route;
    std::future<bool> late;
    RoutePopRelease release {gate};
    promotion = std::async(std::launch::async, [=] {
        return channel->promote_setup_connection(700, inbox, runtime);
    });
    gate.wait();
    // The first sealed packet is in protocol processing. A setup consumer
    // cannot steal the remaining prefix, even though it is still buffered.
    DatagramEnvelope envelope;
    REQUIRE_EQ(inbox->pop_for(envelope, std::chrono::milliseconds {0}),
        InboxPopStatus::closed);
    other_route = std::async(std::launch::async, [=] {
        channel->unregister_connection(700);
        return channel->register_connection(700, replacement)
            && channel->register_setup_inbox(
                701, route_peer, std::make_shared<DatagramInbox>(1));
    });
    REQUIRE_EQ(other_route.wait_for(std::chrono::seconds {2}),
        std::future_status::ready);
    REQUIRE(other_route.get());
    late = std::async(std::launch::async, [=] {
        return inbox->push(setup_data(1001, std::byte {2}).view(), route_peer);
    });
    gate.release();
    REQUIRE(promotion.get());
    REQUIRE(late.get());
    require_setup_message(runtime, std::byte {1});
    require_setup_message(runtime, std::byte {2});
    std::array<std::byte, 8> output {};
    REQUIRE_EQ(replacement->receive_message(output, false, 0).status,
        MessageIoStatus::would_block);
}

TEST(channel_setup_transfer_failed_registration_keeps_original_queue)
{
    auto channel = route_channel();
    auto inbox = std::make_shared<DatagramInbox>(1);
    auto first = route_runtime(channel);
    REQUIRE(channel->register_connection(701, first));
    REQUIRE(channel->register_setup_inbox(700, route_peer, inbox));
    REQUIRE(inbox->push(setup_data(1000, std::byte {1}).view(), route_peer));
    REQUIRE(
        !channel->promote_setup_connection(700, inbox, route_runtime(channel)));
    DatagramEnvelope envelope;
    REQUIRE_EQ(inbox->pop_for(envelope, std::chrono::milliseconds {0}),
        InboxPopStatus::received);
    REQUIRE(inbox->push(setup_data(1000, std::byte {2}).view(), route_peer));
    channel->unregister_connection(701);
    auto runtime = route_runtime(channel);
    REQUIRE(channel->promote_setup_connection(700, inbox, runtime));
    require_setup_message(runtime, std::byte {2});
    // Reusing a sealed inbox for another setup cannot overwrite its identity.
    REQUIRE(channel->register_setup_inbox(702, route_peer, inbox));
    REQUIRE(!channel->promote_setup_connection(
        702, inbox, route_runtime(channel, 91)));
    REQUIRE(channel->register_connection(702, route_runtime(channel, 92)));
}

TEST(channel_setup_transfer_keeps_capacity_and_peer_validation)
{
    auto channel = route_channel();
    auto inbox = std::make_shared<DatagramInbox>(3);
    auto runtime = route_runtime(channel);
    REQUIRE(channel->register_setup_inbox(700, route_peer, inbox));
    const std::array invalid {std::byte {1}};
    REQUIRE(inbox->push(invalid, route_peer));
    const Ipv4Endpoint other_peer {.address = {192, 0, 2, 92}, .port = 14901};
    REQUIRE(inbox->push(setup_data(1000, std::byte {9}).view(), other_peer));
    REQUIRE(inbox->push(setup_data(1000, std::byte {1}).view(), route_peer));
    REQUIRE(!inbox->push(setup_data(1001, std::byte {2}).view(), route_peer));
    REQUIRE(channel->promote_setup_connection(700, inbox, runtime));
    REQUIRE(!inbox->push(setup_data(1001, std::byte {9}).view(), other_peer));
    REQUIRE(inbox->push(setup_data(1001, std::byte {2}).view(), route_peer));
    require_setup_message(runtime, std::byte {1});
    require_setup_message(runtime, std::byte {2});
}

TEST(channel_setup_transfer_pins_runtime_only_until_last_old_inbox_reference)
{
    auto channel = route_channel();
    auto runtime = route_runtime(channel);
    std::weak_ptr<ConnectionRuntime> weak = runtime;
    auto inbox = std::make_shared<DatagramInbox>(1);
    REQUIRE(channel->register_setup_inbox(700, route_peer, inbox));
    REQUIRE(channel->promote_setup_connection(700, inbox, runtime));
    channel->unregister_connection(700);
    runtime.reset();
    REQUIRE(!weak.expired());
    inbox.reset();
    REQUIRE(weak.expired());
}
