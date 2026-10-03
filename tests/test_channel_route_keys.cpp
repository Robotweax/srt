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
