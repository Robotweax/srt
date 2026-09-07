#include "test.hpp"

#include "compat/socket_registry.hpp"
#include "srt/srt.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>

namespace {

struct ScopedSrtRuntime {
    int startup_result = srt_startup();

    ~ScopedSrtRuntime()
    {
        if (startup_result == 0) {
            (void)srt_cleanup();
        }
    }
};

struct ConnectObservation {
    std::atomic_int calls = 0;
    std::atomic_int error = SRT_ERROR;
    std::atomic_int peer_family = AF_UNSPEC;
    std::atomic_int token = 0;
    std::atomic_bool finished = false;
};

void observe_connect(void* opaque, SRTSOCKET, int error,
    const sockaddr* peer, int token)
{
    auto& observation =
        *static_cast<ConnectObservation*>(opaque);
    observation.error.store(error);
    observation.peer_family.store(
        peer != nullptr ? peer->sa_family : AF_UNSPEC);
    observation.token.store(token);
    observation.calls.fetch_add(1);
    observation.finished.store(true, std::memory_order_release);
}

[[nodiscard]] bool wait_for_callback(
    const ConnectObservation& observation) noexcept
{
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds{5};
    while (!observation.finished.load(std::memory_order_acquire)
        && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return observation.finished.load(std::memory_order_acquire);
}

[[nodiscard]] sockaddr_in loopback_address(
    std::uint16_t port = 0U) noexcept
{
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return address;
}

[[nodiscard]] bool bind_listener(
    SRTSOCKET listener, sockaddr_in& name)
{
    sockaddr_in requested = loopback_address();
    if (srt_bind(listener,
            reinterpret_cast<const sockaddr*>(&requested),
            static_cast<int>(sizeof(requested))) == SRT_ERROR) {
        return false;
    }
    REQUIRE_EQ(srt_listen(listener, 4), 0);
    int name_size = static_cast<int>(sizeof(name));
    REQUIRE_EQ(srt_getsockname(listener,
                   reinterpret_cast<sockaddr*>(&name), &name_size),
        0);
    REQUIRE_EQ(name_size, static_cast<int>(sizeof(name)));
    return true;
}

[[nodiscard]] std::int32_t socket_initial_sequence(SRTSOCKET socket)
{
    std::int32_t value = SRT_SEQNO_NONE;
    int size = static_cast<int>(sizeof(value));
    REQUIRE_EQ(srt_getsockflag(socket, SRTO_ISN, &value, &size), 0);
    REQUIRE_EQ(size, static_cast<int>(sizeof(value)));
    return value;
}

} // namespace

TEST(connect_debug_validates_the_forced_sequence_without_state_changes)
{
    ScopedSrtRuntime runtime;
    REQUIRE_EQ(runtime.startup_result, 0);
    const SRTSOCKET socket = srt_create_socket();
    REQUIRE(socket != SRT_INVALID_SOCK);
    const std::int32_t original = socket_initial_sequence(socket);
    const sockaddr_in peer = loopback_address(9'000U);

    REQUIRE_EQ(srt_connect_debug(socket,
                   reinterpret_cast<const sockaddr*>(&peer),
                   static_cast<int>(sizeof(peer)), -2),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);
    REQUIRE_EQ(srt_getsockstate(socket), SRTS_INIT);
    REQUIRE_EQ(socket_initial_sequence(socket), original);

    sockaddr invalid{};
    invalid.sa_family = AF_UNSPEC;
    REQUIRE_EQ(srt_connect_debug(socket, &invalid,
                   static_cast<int>(sizeof(invalid)), 123),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);
    REQUIRE_EQ(srt_getsockstate(socket), SRTS_INIT);
    REQUIRE_EQ(socket_initial_sequence(socket), original);

    REQUIRE_EQ(srt_connect_debug(987'654'321,
                   reinterpret_cast<const sockaddr*>(&peer),
                   static_cast<int>(sizeof(peer)), 123),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVSOCK);

    const auto record =
        robotweax::srt::compat::SocketRegistry::instance().find(socket);
    REQUIRE(record != nullptr);
    {
        std::lock_guard lock(record->mutex);
        record->state = SRTS_CONNECTED;
    }
    REQUIRE_EQ(srt_connect_debug(socket,
                   reinterpret_cast<const sockaddr*>(&peer),
                   static_cast<int>(sizeof(peer)), -2),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ECONNSOCK);
    {
        std::lock_guard lock(record->mutex);
        record->state = SRTS_INIT;
    }
    REQUIRE_EQ(srt_close(socket), 0);
}

TEST(connect_bind_validates_both_addresses_before_binding)
{
    ScopedSrtRuntime runtime;
    REQUIRE_EQ(runtime.startup_result, 0);
    const SRTSOCKET socket = srt_create_socket();
    REQUIRE(socket != SRT_INVALID_SOCK);
    const sockaddr_in source = loopback_address();
    const sockaddr_in target = loopback_address(9'000U);

    sockaddr_in6 ipv6_target{};
    ipv6_target.sin6_family = AF_INET6;
    ipv6_target.sin6_port = htons(9'000U);
    ipv6_target.sin6_addr = in6addr_loopback;
    REQUIRE_EQ(srt_connect_bind(socket,
                   reinterpret_cast<const sockaddr*>(&source),
                   reinterpret_cast<const sockaddr*>(&ipv6_target),
                   static_cast<int>(sizeof(ipv6_target))),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);
    REQUIRE_EQ(srt_getsockstate(socket), SRTS_INIT);

    sockaddr_in zero_port_target = target;
    zero_port_target.sin_port = 0;
    REQUIRE_EQ(srt_connect_bind(socket,
                   reinterpret_cast<const sockaddr*>(&source),
                   reinterpret_cast<const sockaddr*>(&zero_port_target),
                   static_cast<int>(sizeof(zero_port_target))),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);
    REQUIRE_EQ(srt_getsockstate(socket), SRTS_INIT);

    REQUIRE_EQ(srt_connect_bind(socket, nullptr,
                   reinterpret_cast<const sockaddr*>(&target),
                   static_cast<int>(sizeof(target))),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);
    REQUIRE_EQ(srt_getsockstate(socket), SRTS_INIT);

    REQUIRE_EQ(srt_connect_bind(987'654'321,
                   reinterpret_cast<const sockaddr*>(&source),
                   reinterpret_cast<const sockaddr*>(&target),
                   static_cast<int>(sizeof(target))),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVSOCK);

    const SRTSOCKET group = srt_create_group(SRT_GTYPE_BROADCAST);
    REQUIRE(group != SRT_INVALID_SOCK);
    REQUIRE_EQ(srt_connect_bind(group,
                   reinterpret_cast<const sockaddr*>(&source),
                   reinterpret_cast<const sockaddr*>(&ipv6_target),
                   static_cast<int>(sizeof(ipv6_target))),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);
    std::size_t member_count = 17U;
    REQUIRE_EQ(srt_group_data(group, nullptr, &member_count), 0);
    REQUIRE_EQ(member_count, 0U);
    REQUIRE_EQ(srt_connect_bind(group, nullptr,
                   reinterpret_cast<const sockaddr*>(&target),
                   static_cast<int>(sizeof(target))),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);
    REQUIRE_EQ(srt_close(group), 0);

    const auto record =
        robotweax::srt::compat::SocketRegistry::instance().find(socket);
    REQUIRE(record != nullptr);
    {
        std::lock_guard lock(record->mutex);
        record->state = SRTS_OPENED;
    }
    REQUIRE_EQ(srt_connect_bind(socket,
                   reinterpret_cast<const sockaddr*>(&source),
                   reinterpret_cast<const sockaddr*>(&target),
                   static_cast<int>(sizeof(target))),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVOP);
    {
        std::lock_guard lock(record->mutex);
        record->state = SRTS_INIT;
    }
    REQUIRE_EQ(srt_close(socket), 0);
}

TEST(connect_debug_forced_isn_preserves_callback_and_epoll_completion)
{
    ScopedSrtRuntime runtime;
    REQUIRE_EQ(runtime.startup_result, 0);
    const SRTSOCKET listener = srt_create_socket();
    REQUIRE(listener != SRT_INVALID_SOCK);
    sockaddr_in listener_name{};
    if (!bind_listener(listener, listener_name)) {
        int system_error = 0;
        REQUIRE_EQ(srt_getlasterror(&system_error), SRT_ESOCKFAIL);
        REQUIRE(system_error != 0);
        REQUIRE_EQ(srt_close(listener), 0);
        return;
    }

    const SRTSOCKET caller = srt_create_socket();
    REQUIRE(caller != SRT_INVALID_SOCK);
    const bool asynchronous = false;
    REQUIRE_EQ(srt_setsockflag(caller, SRTO_RCVSYN,
                   &asynchronous,
                   static_cast<int>(sizeof(asynchronous))),
        0);
    ConnectObservation observation;
    REQUIRE_EQ(srt_connect_callback(
                   caller, observe_connect, &observation),
        0);
    const int eid = srt_epoll_create();
    REQUIRE(eid >= 0);
    const int interest = SRT_EPOLL_OUT | SRT_EPOLL_ERR;
    REQUIRE_EQ(srt_epoll_add_usock(eid, caller, &interest), 0);

    constexpr int forced_initial_sequence = 0x7fff'fffe;
    REQUIRE_EQ(srt_connect_debug(caller,
                   reinterpret_cast<const sockaddr*>(&listener_name),
                   static_cast<int>(sizeof(listener_name)),
                   forced_initial_sequence),
        0);
    std::array<SRT_EPOLL_EVENT, 2> ready{};
    REQUIRE_EQ(srt_epoll_uwait(eid, ready.data(),
                   static_cast<int>(ready.size()), 5'000),
        1);
    REQUIRE_EQ(ready[0].fd, caller);
    REQUIRE_EQ(ready[0].events, SRT_EPOLL_OUT);
    REQUIRE(wait_for_callback(observation));
    REQUIRE_EQ(observation.calls.load(), 1);
    REQUIRE_EQ(observation.error.load(), SRT_SUCCESS);
    REQUIRE_EQ(observation.peer_family.load(), AF_INET);
    REQUIRE_EQ(observation.token.load(), -1);
    REQUIRE_EQ(socket_initial_sequence(caller),
        forced_initial_sequence);

    const SRTSOCKET accepted = srt_accept(listener, nullptr, nullptr);
    REQUIRE(accepted != SRT_INVALID_SOCK);
    REQUIRE_EQ(socket_initial_sequence(accepted),
        forced_initial_sequence);
    REQUIRE_EQ(srt_epoll_release(eid), 0);
    REQUIRE_EQ(srt_close(caller), 0);
    REQUIRE_EQ(srt_close(accepted), 0);
    REQUIRE_EQ(srt_close(listener), 0);
}

TEST(connect_bind_uses_the_requested_source_and_connects)
{
    ScopedSrtRuntime runtime;
    REQUIRE_EQ(runtime.startup_result, 0);
    const SRTSOCKET listener = srt_create_socket();
    REQUIRE(listener != SRT_INVALID_SOCK);
    sockaddr_in listener_name{};
    if (!bind_listener(listener, listener_name)) {
        int system_error = 0;
        REQUIRE_EQ(srt_getlasterror(&system_error), SRT_ESOCKFAIL);
        REQUIRE(system_error != 0);
        REQUIRE_EQ(srt_close(listener), 0);
        return;
    }

    const SRTSOCKET caller = srt_create_socket();
    REQUIRE(caller != SRT_INVALID_SOCK);
    const sockaddr_in source = loopback_address();
    REQUIRE_EQ(srt_connect_bind(caller,
                   reinterpret_cast<const sockaddr*>(&source),
                   reinterpret_cast<const sockaddr*>(&listener_name),
                   static_cast<int>(sizeof(listener_name))),
        0);
    sockaddr_in local{};
    int local_size = static_cast<int>(sizeof(local));
    REQUIRE_EQ(srt_getsockname(caller,
                   reinterpret_cast<sockaddr*>(&local), &local_size),
        0);
    REQUIRE_EQ(local.sin_family, AF_INET);
    REQUIRE_EQ(local.sin_addr.s_addr, source.sin_addr.s_addr);
    REQUIRE(local.sin_port != 0);
    REQUIRE_EQ(srt_getsockstate(caller), SRTS_CONNECTED);

    const SRTSOCKET accepted = srt_accept(listener, nullptr, nullptr);
    REQUIRE(accepted != SRT_INVALID_SOCK);
    REQUIRE_EQ(srt_close(caller), 0);
    REQUIRE_EQ(srt_close(accepted), 0);
    REQUIRE_EQ(srt_close(listener), 0);
}
