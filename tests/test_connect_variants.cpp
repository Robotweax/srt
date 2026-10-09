#include "test.hpp"

#include "compat/socket_registry.hpp"
#include "compat/connect_callback_executor.hpp"
#include "compat/caller_rejection_completion.hpp"
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

TEST(
    connect_nonblocking_timeout_publishes_terminal_state_for_each_epoll_interest)
{
    ConnectObservation observation;
    ScopedSrtRuntime runtime;
    REQUIRE_EQ(runtime.startup_result, 0);
    robotweax::srt::UdpSocket sink;
    REQUIRE(sink.valid());
    REQUIRE_EQ(sink.bind(robotweax::srt::IpEndpoint::loopback()),
        robotweax::srt::Error::none);
    const auto endpoint = sink.local_endpoint();
    REQUIRE(endpoint);
    const auto address = loopback_address(endpoint.endpoint.port);
    const SRTSOCKET socket = srt_create_socket();
    REQUIRE(socket != SRT_INVALID_SOCK);
    const bool synchronous = false;
    const std::int32_t timeout = 350;
    REQUIRE_EQ(
        srt_setsockflag(socket, SRTO_RCVSYN, &synchronous, sizeof(synchronous)),
        0);
    REQUIRE_EQ(
        srt_setsockflag(socket, SRTO_CONNTIMEO, &timeout, sizeof(timeout)), 0);
    REQUIRE_EQ(srt_connect_callback(socket, observe_connect, &observation), 0);
    constexpr std::array<int, 4> interests {SRT_EPOLL_IN | SRT_EPOLL_ERR,
        SRT_EPOLL_OUT | SRT_EPOLL_ERR,
        SRT_EPOLL_IN | SRT_EPOLL_OUT | SRT_EPOLL_ERR, SRT_EPOLL_ERR};
    std::array<int, interests.size()> polls {};
    for (std::size_t i = 0; i < polls.size(); ++i) {
        polls[i] = srt_epoll_create();
        REQUIRE(polls[i] >= 0);
        REQUIRE_EQ(srt_epoll_add_usock(polls[i], socket, &interests[i]), 0);
    }
    REQUIRE_EQ(srt_connect(socket, reinterpret_cast<const sockaddr*>(&address),
                   sizeof(address)),
        0);
    for (std::size_t i = 0; i < polls.size(); ++i) {
        SRT_EPOLL_EVENT ready {};
        REQUIRE_EQ(srt_epoll_uwait(polls[i], &ready, 1, 5'000), 1);
        REQUIRE_EQ(ready.fd, socket);
        REQUIRE_EQ(ready.events, interests[i]);
        REQUIRE_EQ(srt_getsockstate(socket), SRTS_BROKEN);
        REQUIRE_EQ(srt_getrejectreason(socket), SRT_REJ_TIMEOUT);
        REQUIRE_EQ(srt_epoll_release(polls[i]), 0);
    }
    REQUIRE(wait_for_callback(observation));
    REQUIRE_EQ(observation.calls.load(), 1);
    REQUIRE_EQ(observation.error.load(), SRT_ENOSERVER);
    REQUIRE_EQ(srt_connect(socket, reinterpret_cast<const sockaddr*>(&address),
                   sizeof(address)),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ESCLOSED);
    REQUIRE_EQ(srt_close(socket), 0);
}

namespace {

struct RejectionObservation {
    std::atomic_uint calls = 0;
    std::atomic_bool valid = true;
    int reason = SRT_REJ_UNKNOWN;
    std::chrono::steady_clock::time_point connect_start {};
};

void observe_rejection(
    void* opaque, SRTSOCKET socket, int error, const sockaddr*, int)
{
    auto& observation = *static_cast<RejectionObservation*>(opaque);
    if (error != SRT_ECONNREJ || srt_getsockstate(socket) != SRTS_BROKEN
        || srt_getrejectreason(socket) != observation.reason
        || std::chrono::steady_clock::now()
            < observation.connect_start + std::chrono::milliseconds {10}) {
        observation.valid = false;
    }
    ++observation.calls;
}

} // namespace

TEST(connect_nonblocking_repeated_rejection_preserves_completion_and_cleanup)
{
    RejectionObservation observation;
    ScopedSrtRuntime runtime;
    REQUIRE_EQ(runtime.startup_result, 0);
    auto& registry = robotweax::srt::compat::SocketRegistry::instance();
    const auto baseline = registry.size();
    auto executor = robotweax::srt::compat::acquire_connect_callback_executor();
    REQUIRE_EQ(executor->snapshot().created, 0U);
    constexpr char secret[] = "robotweax-reject-test-secret";
    constexpr char wrong_secret[] = "robotweax-wrong-test-secret";
    constexpr std::array<int, 3> reasons {
        SRT_REJ_BADSECRET, SRT_REJ_UNSECURE, SRT_REJ_MESSAGEAPI};
    unsigned attempts = 0;
    for (const int reason : reasons) {
        const auto listener = srt_create_socket();
        REQUIRE(listener != SRT_INVALID_SOCK);
        if (reason == SRT_REJ_MESSAGEAPI) {
            const bool message_api = false;
            REQUIRE_EQ(srt_setsockflag(listener, SRTO_MESSAGEAPI, &message_api,
                           sizeof(message_api)),
                0);
        } else {
            REQUIRE_EQ(srt_setsockflag(listener, SRTO_PASSPHRASE, secret,
                           sizeof(secret) - 1),
                0);
        }
        sockaddr_in address {};
        SKIP_UNLESS(
            bind_listener(listener, address), "IPv4 loopback unavailable");
        observation.reason = reason;
        for (unsigned i = 0; i < 8; ++i) {
            const auto caller = srt_create_socket();
            REQUIRE(caller != SRT_INVALID_SOCK);
            const bool synchronous = false;
            const int timeout = 2'000;
            REQUIRE_EQ(srt_setsockflag(caller, SRTO_RCVSYN, &synchronous,
                           sizeof(synchronous)),
                0);
            REQUIRE_EQ(srt_setsockflag(caller, SRTO_SNDSYN, &synchronous,
                           sizeof(synchronous)),
                0);
            REQUIRE_EQ(srt_setsockflag(
                           caller, SRTO_CONNTIMEO, &timeout, sizeof(timeout)),
                0);
            if (reason == SRT_REJ_BADSECRET) {
                REQUIRE_EQ(srt_setsockflag(caller, SRTO_PASSPHRASE,
                               wrong_secret, sizeof(wrong_secret) - 1),
                    0);
            }
            REQUIRE_EQ(
                srt_connect_callback(caller, observe_rejection, &observation),
                0);
            const int poll = srt_epoll_create();
            REQUIRE(poll >= 0);
            const int interests = SRT_EPOLL_OUT | SRT_EPOLL_ERR;
            REQUIRE_EQ(srt_epoll_add_usock(poll, caller, &interests), 0);
            observation.connect_start = std::chrono::steady_clock::now();
            REQUIRE_EQ(
                srt_connect(caller, reinterpret_cast<const sockaddr*>(&address),
                    sizeof(address)),
                0);
            SRT_EPOLL_EVENT ready {};
            REQUIRE_EQ(srt_epoll_uwait(poll, &ready, 1, 5'000), 1);
            REQUIRE_EQ(ready.fd, caller);
            REQUIRE_EQ(ready.events, interests);
            REQUIRE(std::chrono::steady_clock::now()
                >= observation.connect_start + std::chrono::milliseconds {10});
            REQUIRE_EQ(srt_getsockstate(caller), SRTS_BROKEN);
            REQUIRE_EQ(srt_getrejectreason(caller), reason);
            ++attempts;
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds {5};
            while (executor->snapshot().completed != attempts
                && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::yield();
            }
            REQUIRE_EQ(executor->snapshot().completed, attempts);
            REQUIRE_EQ(observation.calls.load(), attempts);
            REQUIRE(observation.valid.load());
            REQUIRE_EQ(srt_epoll_remove_usock(poll, caller), 0);
            REQUIRE_EQ(srt_close(caller), 0);
            REQUIRE_EQ(srt_epoll_release(poll), 0);
            REQUIRE(registry.find(caller) == nullptr);
            REQUIRE_EQ(srt_getsockstate(caller), SRTS_CLOSED);
            REQUIRE_EQ(observation.calls.load(), attempts);
        }
        REQUIRE_EQ(srt_close(listener), 0);
    }
    REQUIRE_EQ(registry.size(), baseline);
    REQUIRE_EQ(executor->snapshot().created, 1U);
    REQUIRE_EQ(executor->snapshot().reused, attempts - 1U);
    REQUIRE_EQ(executor->snapshot().idle, 1U);
}

TEST(connect_rejects_fec_group_larger_than_listener_receive_window)
{
    ScopedSrtRuntime runtime;
    REQUIRE_EQ(runtime.startup_result, 0);
    constexpr char filter[] = "fec,cols:5,rows:8";
    constexpr std::int32_t receive_window = 32;
    constexpr std::int32_t timeout_milliseconds = 2'000;
    const SRTSOCKET listener = srt_create_socket();
    REQUIRE(listener != SRT_INVALID_SOCK);
    REQUIRE_EQ(srt_setsockflag(listener, SRTO_FC, &receive_window,
                   static_cast<int>(sizeof(receive_window))),
        0);
    REQUIRE_EQ(srt_setsockflag(listener, SRTO_PACKETFILTER, filter,
                   static_cast<int>(sizeof(filter) - 1U)),
        0);
    sockaddr_in listener_name {};
    if (!bind_listener(listener, listener_name)) {
        REQUIRE_EQ(srt_close(listener), 0);
        return;
    }

    const SRTSOCKET caller = srt_create_socket();
    REQUIRE(caller != SRT_INVALID_SOCK);
    REQUIRE_EQ(srt_setsockflag(caller, SRTO_PACKETFILTER, filter,
                   static_cast<int>(sizeof(filter) - 1U)),
        0);
    REQUIRE_EQ(srt_setsockflag(caller, SRTO_CONNTIMEO, &timeout_milliseconds,
                   static_cast<int>(sizeof(timeout_milliseconds))),
        0);
    REQUIRE_EQ(
        srt_connect(caller, reinterpret_cast<const sockaddr*>(&listener_name),
            static_cast<int>(sizeof(listener_name))),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ECONNREJ);
    REQUIRE_EQ(srt_getrejectreason(caller), SRT_REJ_FILTER);
    REQUIRE_EQ(srt_close(caller), 0);
    REQUIRE_EQ(srt_close(listener), 0);
}

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

TEST(connect_rejection_completion_clock_boundaries_and_first_cause)
{
    using namespace robotweax::srt::compat;
    using Clock = CallerRejectionCompletion::Clock;
    const auto start = Clock::time_point {} + std::chrono::seconds {7};
    CallerRejectionCompletion completion {start};
    completion.record({SRT_ESECFAIL, 23, SRT_REJ_BADSECRET});
    REQUIRE(completion.pending());
    REQUIRE(!completion.take_if_due(start));
    REQUIRE(!completion.take_if_due(start + std::chrono::microseconds {9'999}));
    // A repeated peer response must not restart the floor or replace its cause.
    completion.record({SRT_ECONNREJ, 0, SRT_REJ_MESSAGEAPI});
    const auto result =
        completion.take_if_due(start + std::chrono::milliseconds {10});
    REQUIRE(result.has_value());
    REQUIRE_EQ(result->error, SRT_ESECFAIL);
    REQUIRE_EQ(result->system_error, 23);
    REQUIRE_EQ(result->reason, SRT_REJ_BADSECRET);
    REQUIRE(!completion.pending());
    completion.record({SRT_ECONNREJ, 0, SRT_REJ_MESSAGEAPI});
    REQUIRE(!completion.take_if_due(start + std::chrono::seconds {1}));

    CallerRejectionCompletion slow_peer {start};
    slow_peer.record({SRT_ECONNREJ, 0, SRT_REJ_UNSECURE});
    REQUIRE(slow_peer.take_if_due(start + std::chrono::milliseconds {40}));
}

TEST(connect_rejection_completion_cancel_invalidates_late_clock_wake)
{
    using namespace robotweax::srt::compat;
    const auto start = CallerRejectionCompletion::Clock::time_point {};
    CallerRejectionCompletion completion {start};
    completion.record({SRT_ECONNREJ, 0, SRT_REJ_MESSAGEAPI});
    completion.cancel();
    completion.record({SRT_ESECFAIL, 0, SRT_REJ_BADSECRET});
    REQUIRE(!completion.pending());
    REQUIRE(!completion.take_if_due(start + std::chrono::milliseconds {10}));
    REQUIRE(!completion.take_if_due(start + std::chrono::hours {1}));
}
