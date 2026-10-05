#include "test.hpp"
#include "compat/runtime_scheduler_service.hpp"
#include "compat/socket_registry.hpp"
#include "compat/transport_runtime.hpp"
#include "srt/srt.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <future>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

using namespace robotweax::srt;
using namespace robotweax::srt::compat;

namespace {
int setting(const char* name, const char* value)
{
#if defined(_WIN32)
    return _putenv_s(name, value == nullptr ? "" : value);
#else
    return value == nullptr ? unsetenv(name) : setenv(name, value, 1);
#endif
}
struct Runtime {
    Runtime()
    {
        REQUIRE_EQ(setting("ROBOTWEAX_SRT_CONNECTION_AFFINITY", "1"), 0);
        REQUIRE_EQ(setting("ROBOTWEAX_SRT_INBOX_STORAGE_MIB", "1"), 0);
        REQUIRE_EQ(srt_startup(), 0);
    }
    ~Runtime()
    {
        (void)srt_cleanup();
    }
};
struct Sockets {
    std::vector<SRTSOCKET> values;
    SRTSOCKET create()
    {
        auto socket = srt_create_socket();
        REQUIRE(socket != SRT_INVALID_SOCK);
        values.push_back(socket);
        const int timeout = 2000;
        for (const auto option : {SRTO_CONNTIMEO, SRTO_RCVTIMEO, SRTO_SNDTIMEO})
            REQUIRE_EQ(
                srt_setsockflag(socket, option, &timeout, sizeof(timeout)), 0);
        return socket;
    }
    ~Sockets()
    {
        for (auto socket : values)
            (void)srt_close(socket);
    }
};
sockaddr_in address()
{
    sockaddr_in value {};
    value.sin_family = AF_INET;
    value.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return value;
}
sockaddr_in bind(SRTSOCKET socket)
{
    auto value = address();
    REQUIRE_EQ(srt_bind(socket, reinterpret_cast<const sockaddr*>(&value),
                   sizeof(value)),
        0);
    int size = sizeof(value);
    REQUIRE_EQ(
        srt_getsockname(socket, reinterpret_cast<sockaddr*>(&value), &size), 0);
    return value;
}
std::shared_ptr<ConnectionDatagramDispatcher> dispatcher(SRTSOCKET socket)
{
    auto record = SocketRegistry::instance().find(socket);
    REQUIRE(record != nullptr);
    std::shared_ptr<DatagramChannel> channel;
    std::uint32_t id;
    {
        std::lock_guard lock(record->mutex);
        REQUIRE(record->runtime != nullptr);
        channel = record->channel;
        id = record->protocol_socket_id;
    }
    REQUIRE(channel->scheduled_polling_enabled());
    auto result = channel->connection_dispatcher_for_testing(id);
    REQUIRE(result != nullptr);
    return result;
}
void connected(SRTSOCKET socket)
{
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds {3};
    while (srt_getsockstate(socket) == SRTS_CONNECTING
        && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds {1});
    REQUIRE_EQ(srt_getsockstate(socket), SRTS_CONNECTED);
}
void exchange(SRTSOCKET first, SRTSOCKET second)
{
    const std::array<char, 4> payload {'a', 'c', 'p', '1'};
    for (const auto pair :
        {std::pair {first, second}, std::pair {second, first}}) {
        REQUIRE_EQ(
            srt_sendmsg(pair.first, payload.data(), payload.size(), -1, 1), 4);
        std::array<char, 1500> received {};
        const int count =
            srt_recvmsg(pair.second, received.data(), received.size());
        if (count != 4) {
            const auto error = srt_getlasterror(nullptr);
            std::ostringstream message;
            message << "receive returned " << count << ", error " << error
                    << ": " << srt_getlasterror_str() << ", sender state "
                    << srt_getsockstate(pair.first) << ", receiver state "
                    << srt_getsockstate(pair.second) << ", dispatched "
                    << dispatcher(pair.second)->snapshot().dispatched_datagrams;
            throw std::runtime_error(message.str());
        }
        REQUIRE(std::equal(payload.begin(), payload.end(), received.begin()));
        const auto sink = dispatcher(pair.second);
        // Protocol delivery wakes the receiver before the service callback
        // publishes its accounting at the end of the turn.
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds {2};
        while (sink->snapshot().dispatched_datagrams == 0U
            && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds {1});
        REQUIRE(sink->snapshot().dispatched_datagrams > 0U);
    }
}
SRTSOCKET accept(Sockets& sockets, SRTSOCKET listener)
{
    auto result = srt_accept(listener, nullptr, nullptr);
    REQUIRE(result != SRT_INVALID_SOCK);
    sockets.values.push_back(result);
    return result;
}
}

TEST(public_affinity_selector_rejects_invalid_and_freezes_first_valid_selection)
{
    REQUIRE_EQ(setting("ROBOTWEAX_SRT_CONNECTION_AFFINITY", "invalid"), 0);
    REQUIRE(!runtime_connection_affinity_enabled().has_value());
    for (const char* value : {" 1", "1 ", "01", "-1", "true"}) {
        REQUIRE_EQ(setting("ROBOTWEAX_SRT_CONNECTION_AFFINITY", value), 0);
        REQUIRE(!runtime_connection_affinity_enabled().has_value());
    }
    Runtime runtime;
    REQUIRE_EQ(setting("ROBOTWEAX_SRT_BOUNDED_BIND", "0"), 0);
    REQUIRE(*runtime_connection_affinity_enabled());
    auto scheduler = acquire_runtime_scheduler();
    REQUIRE(scheduler != nullptr);
    REQUIRE_EQ(scheduler->snapshot().service_capacity, 2048U);
    Sockets sockets;
    auto socket = sockets.create();
    auto native = acquire_runtime_native_channel_budget();
    const auto before = native->reserved_channels();
    (void)bind(socket);
    REQUIRE_EQ(native->reserved_channels(), before + 1U);
    REQUIRE_EQ(setting("ROBOTWEAX_SRT_CONNECTION_AFFINITY", "0"), 0);
    REQUIRE(*runtime_connection_affinity_enabled());
}

TEST(
    public_affinity_zero_service_capacity_refuses_bind_and_preserves_adopted_descriptor)
{
    Runtime runtime;
    REQUIRE_EQ(setting("ROBOTWEAX_SRT_SCHEDULER_SERVICES_PER_SHARD", "0"), 0);
    Sockets sockets;
    const auto first = sockets.create();
    auto value = address();
    auto budget = acquire_runtime_native_channel_budget();
    const auto before = budget->reserved_channels();
    REQUIRE_EQ(srt_bind(first, reinterpret_cast<const sockaddr*>(&value),
                   sizeof(value)),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ENOBUF);
    UdpSocket native {IpAddressFamily::ipv4};
    REQUIRE_EQ(native.bind(IpEndpoint::loopback()), Error::none);
    const auto handle = native.native_handle();
    REQUIRE_EQ(
        srt_bind_acquire(first, static_cast<UDPSOCKET>(handle)), SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ENOBUF);
    REQUIRE(native.local_endpoint());
    REQUIRE_EQ(native.native_handle(), handle);
    REQUIRE_EQ(budget->reserved_channels(), before);
    REQUIRE_EQ(
        setting("ROBOTWEAX_SRT_SCHEDULER_SERVICES_PER_SHARD", nullptr), 0);
    (void)bind(first);
}

TEST(
    public_affinity_caller_listener_attaches_real_dispatchers_and_exchanges_messages)
{
    Runtime runtime;
    Sockets sockets;
    const auto listener = sockets.create();
    auto endpoint = bind(listener);
    REQUIRE_EQ(srt_listen(listener, 4), 0);
    const auto caller = sockets.create();
    REQUIRE_EQ(srt_connect(caller, reinterpret_cast<const sockaddr*>(&endpoint),
                   sizeof(endpoint)),
        0);
    const auto accepted = accept(sockets, listener);
    auto caller_sink = dispatcher(caller);
    auto accepted_sink = dispatcher(accepted);
    REQUIRE(caller_sink->inbox()->token().service.shard
        != accepted_sink->inbox()->token().service.shard);
    exchange(caller, accepted);
}

TEST(public_affinity_async_caller_uses_selected_setup)
{
    Runtime runtime;
    Sockets sockets;
    const auto listener = sockets.create();
    auto endpoint = bind(listener);
    REQUIRE_EQ(srt_listen(listener, 4), 0);
    const auto caller = sockets.create();
    const bool asynchronous = false;
    REQUIRE_EQ(srt_setsockflag(
                   caller, SRTO_SNDSYN, &asynchronous, sizeof(asynchronous)),
        0);
    REQUIRE_EQ(srt_connect(caller, reinterpret_cast<const sockaddr*>(&endpoint),
                   sizeof(endpoint)),
        0);
    const auto accepted = accept(sockets, listener);
    connected(caller);
    exchange(caller, accepted);
}

TEST(public_affinity_rendezvous_both_paths_attach_dispatchers)
{
    Runtime runtime;
    Sockets sockets;
    const auto first = sockets.create();
    const auto second = sockets.create();
    const bool rendezvous = true;
    REQUIRE_EQ(srt_setsockflag(
                   first, SRTO_RENDEZVOUS, &rendezvous, sizeof(rendezvous)),
        0);
    REQUIRE_EQ(srt_setsockflag(
                   second, SRTO_RENDEZVOUS, &rendezvous, sizeof(rendezvous)),
        0);
    auto first_address = bind(first);
    auto second_address = bind(second);
    auto attempt = std::async(std::launch::async, [&] {
        return srt_connect(first,
            reinterpret_cast<const sockaddr*>(&second_address),
            sizeof(second_address));
    });
    REQUIRE_EQ(
        srt_connect(second, reinterpret_cast<const sockaddr*>(&first_address),
            sizeof(first_address)),
        0);
    REQUIRE_EQ(attempt.get(), 0);
    exchange(first, second);
}

TEST(
    public_affinity_service_exhaustion_fails_without_inline_fallback_and_retry_recovers)
{
    Runtime runtime;
    REQUIRE_EQ(setting("ROBOTWEAX_SRT_SCHEDULER_SERVICES_PER_SHARD", "1"), 0);
    auto scheduler = acquire_runtime_scheduler();
    REQUIRE(scheduler != nullptr);
    std::array<std::shared_ptr<ConnectionWorkBinding>, 2> occupied;
    for (std::size_t index = 0; index < occupied.size(); ++index) {
        occupied[index] = ConnectionWorkBinding::create(
            scheduler, index, [](void*, ConnectionWorkHints) noexcept { }, {});
        REQUIRE(occupied[index] != nullptr);
    }
    Sockets sockets;
    const auto listener = sockets.create();
    auto endpoint = bind(listener);
    REQUIRE_EQ(srt_listen(listener, 4), 0);
    const auto refused = sockets.create();
    REQUIRE_EQ(
        srt_connect(refused, reinterpret_cast<const sockaddr*>(&endpoint),
            sizeof(endpoint)),
        SRT_ERROR);
    auto record = SocketRegistry::instance().find(refused);
    {
        std::lock_guard lock(record->mutex);
        REQUIRE(record->runtime == nullptr);
    }
    for (auto& slot : occupied) {
        slot->retire();
        slot.reset();
    }
    const auto caller = sockets.create();
    REQUIRE_EQ(srt_connect(caller, reinterpret_cast<const sockaddr*>(&endpoint),
                   sizeof(endpoint)),
        0);
    const auto accepted = accept(sockets, listener);
    exchange(caller, accepted);
    REQUIRE_EQ(
        setting("ROBOTWEAX_SRT_SCHEDULER_SERVICES_PER_SHARD", nullptr), 0);
}

TEST(public_affinity_ring_exhaustion_fails_without_route_then_retry_recovers)
{
    Runtime runtime;
    auto scheduler = acquire_runtime_scheduler();
    auto process = acquire_runtime_inbox_storage_budget();
    auto local = std::make_shared<DatagramStorageBudget>(1024U * 1024U);
    auto binding = ConnectionWorkBinding::create(
        scheduler, 0, [](void*, ConnectionWorkHints) noexcept { }, {});
    REQUIRE(binding != nullptr);
    const auto capacity =
        1024U * 1024U / *ConnectionDatagramInbox::storage_bytes(1);
    auto ring =
        ConnectionDatagramInbox::create(local, binding, IpEndpoint::loopback(),
            {.capacity = capacity, .control_reserve = 0}, process);
    REQUIRE(ring != nullptr);
    Sockets sockets;
    const auto listener = sockets.create();
    auto endpoint = bind(listener);
    REQUIRE_EQ(srt_listen(listener, 4), 0);
    const auto refused = sockets.create();
    REQUIRE_EQ(
        srt_connect(refused, reinterpret_cast<const sockaddr*>(&endpoint),
            sizeof(endpoint)),
        SRT_ERROR);
    auto record = SocketRegistry::instance().find(refused);
    {
        std::lock_guard lock(record->mutex);
        REQUIRE(record->runtime == nullptr);
    }
    ring->close();
    ring.reset();
    binding->retire();
    const auto caller = sockets.create();
    REQUIRE_EQ(srt_connect(caller, reinterpret_cast<const sockaddr*>(&endpoint),
                   sizeof(endpoint)),
        0);
    const auto accepted = accept(sockets, listener);
    exchange(caller, accepted);
}

TEST(public_affinity_native_adoption_and_cleanup_restart_retain_policy)
{
    std::shared_ptr<DatagramChannel> old_channel;
    auto budget = acquire_runtime_native_channel_budget();
    {
        Runtime runtime;
        Sockets sockets;
        const auto socket = sockets.create();
        UdpSocket native {IpAddressFamily::ipv4};
        REQUIRE_EQ(native.bind(IpEndpoint::loopback()), Error::none);
        REQUIRE_EQ(srt_bind_acquire(
                       socket, static_cast<UDPSOCKET>(native.native_handle())),
            0);
        (void)native.release_native();
        auto record = SocketRegistry::instance().find(socket);
        {
            std::lock_guard lock(record->mutex);
            old_channel = record->channel;
        }
        REQUIRE(old_channel->scheduled_polling_enabled());
    }
    REQUIRE(!old_channel->socket.valid());
    REQUIRE_EQ(budget->reserved_channels(), 0U);
    Runtime restarted;
    Sockets sockets;
    const auto listener = sockets.create();
    auto endpoint = bind(listener);
    REQUIRE_EQ(srt_listen(listener, 4), 0);
    const auto caller = sockets.create();
    REQUIRE_EQ(srt_connect(caller, reinterpret_cast<const sockaddr*>(&endpoint),
                   sizeof(endpoint)),
        0);
    const auto accepted = accept(sockets, listener);
    exchange(caller, accepted);
}
