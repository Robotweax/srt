#include "test.hpp"
#include "crypto_test_helpers.hpp"

#include "robotweax/srt/codec.hpp"
#include "robotweax/srt/crypto.hpp"
#include "robotweax/srt/packet.hpp"
#include "robotweax/srt/socket_options.hpp"
#include "compat/runtime_scheduler_service.hpp"
#include "compat/runtime_work_executor_service.hpp"
#include "compat/socket_registry.hpp"
#include "compat/group_registry.hpp"
#include "compat/transport_runtime.hpp"
#include "srt/srt.h"

#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <netdb.h>
#endif

namespace {

using namespace std::chrono_literals;
using namespace robotweax::srt;
using robotweax::srt::compat::SocketRegistry;
using robotweax::srt::compat::ConnectionRuntime;
using robotweax::srt::compat::DatagramChannel;

struct KeyRequestObserver {
    std::atomic_bool request_seen = false;
};

UdpIoResult accept_and_observe_key_request(
    std::span<const std::byte> bytes,
    Ipv4Endpoint,
    void* context) noexcept
{
    auto& observer = *static_cast<KeyRequestObserver*>(context);
    const auto decoded = decode_packet(bytes);
    if (decoded
        && decoded.packet.kind == PacketKind::control
        && decoded.packet.control.type == ControlType::user_defined
        && decoded.packet.control.subtype
            == key_material_request_subtype) {
        observer.request_seen.store(true, std::memory_order_release);
    }
    return {.bytes_transferred = bytes.size()};
}

UdpIoResult accept_datagram(
    std::span<const std::byte> bytes,
    Ipv4Endpoint,
    void*) noexcept
{
    return {.bytes_transferred = bytes.size()};
}

SRTSOCKET create_active_listener()
{
    constexpr std::int32_t udp_receive_buffer_bytes = 1'048'576;
    const SRTSOCKET listener = srt_create_socket();
    REQUIRE(listener != SRT_INVALID_SOCK);
    REQUIRE_EQ(
        srt_setsockflag(listener, SRTO_UDP_RCVBUF,
            &udp_receive_buffer_bytes,
            static_cast<int>(sizeof(udp_receive_buffer_bytes))),
        0);

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = 0;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (srt_bind(listener,
            reinterpret_cast<const sockaddr*>(&address),
            static_cast<int>(sizeof(address))) == SRT_ERROR) {
        int system_error = 0;
        REQUIRE_EQ(srt_getlasterror(&system_error), SRT_ESOCKFAIL);
        REQUIRE(system_error != 0);
        REQUIRE_EQ(srt_close(listener), 0);
        return SRT_INVALID_SOCK;
    }
    REQUIRE_EQ(srt_listen(listener, 4), 0);
    REQUIRE_EQ(srt_getsockstate(listener), SRTS_LISTENING);
    return listener;
}

struct EpollWaitResult {
    int result = 0;
    int error = SRT_SUCCESS;
    SRT_EPOLL_EVENT event{};
    std::chrono::steady_clock::duration elapsed{};
};

} // namespace

TEST(lifecycle_closed_socket_records_release_without_global_cleanup)
{
    REQUIRE_EQ(srt_startup(), 0);
    const auto live = srt_create_socket();
    REQUIRE(live != SRT_INVALID_SOCK);
    const int poll = srt_epoll_create();
    REQUIRE(poll >= 0);
    const int events = SRT_EPOLL_ERR;
    SRTSOCKET first = SRT_INVALID_SOCK;
    SRTSOCKET last = SRT_INVALID_SOCK;
    constexpr auto count =
        robotweax::srt::compat::ClosedHandleHistory::capacity + 128U;
    for (std::size_t i = 0; i < count; ++i) {
        const auto socket = srt_create_socket();
        REQUIRE(socket != SRT_INVALID_SOCK);
        REQUIRE(socket != last);
        if (i == 0U) {
            first = socket;
            REQUIRE_EQ(srt_epoll_add_usock(poll, first, &events), 0);
        }
        std::weak_ptr<robotweax::srt::compat::SocketRecord> weak =
            SocketRegistry::instance().find(socket);
        REQUIRE(!weak.expired());
        REQUIRE_EQ(srt_close(socket), 0);
        REQUIRE(weak.expired());
        REQUIRE(SocketRegistry::instance().find(socket) == nullptr);
        REQUIRE_EQ(srt_getsockstate(socket), SRTS_CLOSED);
        REQUIRE_EQ(srt_listen(socket, 1), SRT_ERROR);
        REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ESCLOSED);
        REQUIRE_EQ(srt_listen(socket, 0), SRT_ERROR);
        REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);
        REQUIRE_EQ(srt_close(socket), 0);
        last = socket;
    }
    REQUIRE_EQ(srt_getsockstate(first), SRTS_NONEXIST);
    REQUIRE_EQ(srt_getsockstate(last), SRTS_CLOSED);
    REQUIRE_EQ(srt_getsockstate(live), SRTS_INIT);
    // Existing subscriptions must still observe closure after history eviction.
    SRT_EPOLL_EVENT ready {};
    REQUIRE_EQ(srt_epoll_uwait(poll, &ready, 1, 0), 1);
    REQUIRE_EQ(ready.fd, first);
    REQUIRE_EQ(ready.events, SRT_EPOLL_ERR);
    REQUIRE_EQ(srt_epoll_add_usock(poll, first, &events), SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVSOCK);
    REQUIRE_EQ(srt_epoll_remove_usock(poll, first), 0);
    REQUIRE_EQ(srt_epoll_release(poll), 0);
    REQUIRE_EQ(srt_close(live), 0);
    REQUIRE_EQ(srt_cleanup(), 0);
    REQUIRE_EQ(srt_getsockstate(last), SRTS_NONEXIST);
}

TEST(lifecycle_closed_group_records_release_without_global_cleanup)
{
    using robotweax::srt::compat::GroupRegistry;
    REQUIRE_EQ(srt_startup(), 0);
    const int poll = srt_epoll_create();
    REQUIRE(poll >= 0);
    const int events = SRT_EPOLL_ERR;
    SRTSOCKET first = SRT_INVALID_SOCK;
    constexpr auto count =
        robotweax::srt::compat::ClosedHandleHistory::capacity + 1U;
    for (std::size_t i = 0; i < count; ++i) {
        const auto group = srt_create_group(SRT_GTYPE_BACKUP);
        REQUIRE(group != SRT_INVALID_SOCK);
        if (i == 0U) {
            first = group;
            REQUIRE_EQ(srt_epoll_add_usock(poll, first, &events), 0);
        }
        std::weak_ptr<robotweax::srt::compat::GroupRecord> weak =
            GroupRegistry::instance().find(group);
        REQUIRE(!weak.expired());
        REQUIRE_EQ(srt_close(group), 0);
        REQUIRE(weak.expired());
        REQUIRE_EQ(srt_getsockstate(group), SRTS_CLOSED);
    }
    REQUIRE_EQ(srt_getsockstate(first), SRTS_NONEXIST);
    REQUIRE_EQ(srt_close(first), 0);
    SRT_EPOLL_EVENT ready {};
    REQUIRE_EQ(srt_epoll_uwait(poll, &ready, 1, 0), 1);
    REQUIRE_EQ(ready.fd, first);
    REQUIRE_EQ(ready.events, SRT_EPOLL_ERR);
    REQUIRE_EQ(srt_epoll_release(poll), 0);
    REQUIRE_EQ(srt_cleanup(), 0);
}

TEST(lifecycle_close_erases_passphrase_even_with_inflight_record_owner)
{
    REQUIRE_EQ(srt_startup(), 0);
    const auto socket = srt_create_socket();
    constexpr char passphrase[] = "synthetic-close-secret";
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_PASSPHRASE, passphrase,
                   static_cast<int>(sizeof(passphrase) - 1U)),
        0);
    auto owner = SocketRegistry::instance().find(socket);
    REQUIRE(owner != nullptr);
    std::weak_ptr<robotweax::srt::compat::SocketRecord> weak = owner;
    REQUIRE_EQ(srt_close(socket), 0);
    REQUIRE(!weak.expired());
    {
        std::lock_guard lock(owner->mutex);
        REQUIRE_EQ(owner->state, SRTS_CLOSED);
        REQUIRE(owner->native_options.passphrase().empty());
    }
    REQUIRE(SocketRegistry::instance().find(socket) == nullptr);
    owner.reset();
    REQUIRE(weak.expired());
    REQUIRE_EQ(srt_cleanup(), 0);
}

TEST(lifecycle_close_erases_group_passphrase_with_inflight_owner)
{
    using robotweax::srt::compat::GroupRegistry;
    REQUIRE_EQ(srt_startup(), 0);
    const auto group = srt_create_group(SRT_GTYPE_BACKUP);
    constexpr char passphrase[] = "synthetic-group-secret";
    REQUIRE_EQ(srt_setsockflag(group, SRTO_PASSPHRASE, passphrase,
                   static_cast<int>(sizeof(passphrase) - 1U)),
        0);
    const auto owner = GroupRegistry::instance().find(group);
    REQUIRE(owner != nullptr);
    REQUIRE_EQ(srt_close(group), 0);
    {
        std::lock_guard lock(owner->mutex);
        REQUIRE(owner->closed);
        REQUIRE(owner->member_native_options.passphrase().empty());
    }
    REQUIRE(GroupRegistry::instance().find(group) == nullptr);
    REQUIRE_EQ(srt_cleanup(), 0);
}

TEST(lifecycle_concurrent_close_and_lookup_preserve_inflight_ownership)
{
    REQUIRE_EQ(srt_startup(), 0);
    for (std::size_t iteration = 0; iteration < 64U; ++iteration) {
        const auto socket = srt_create_socket();
        auto owner = SocketRegistry::instance().find(socket);
        REQUIRE(owner != nullptr);
        std::weak_ptr<robotweax::srt::compat::SocketRecord> weak = owner;
        std::barrier start {3};
        std::atomic_bool valid = true;
        std::jthread closer([&] {
            start.arrive_and_wait();
            if (srt_close(socket) != 0) {
                valid = false;
            }
        });
        std::jthread observer([&] {
            start.arrive_and_wait();
            for (std::size_t i = 0; i < 64U; ++i) {
                const auto found = SocketRegistry::instance().find(socket);
                if (found != nullptr) {
                    std::lock_guard lock(found->mutex);
                    if (found != owner) {
                        valid = false;
                    }
                }
                if (srt_close(socket) != 0) {
                    valid = false;
                }
            }
        });
        start.arrive_and_wait();
        closer.join();
        observer.join();
        REQUIRE(valid.load());
        REQUIRE(!weak.expired());
        REQUIRE_EQ(srt_getsockstate(socket), SRTS_CLOSED);
        owner.reset();
        REQUIRE(weak.expired());
    }
    REQUIRE_EQ(srt_cleanup(), 0);
}

TEST(lifecycle_handle_allocator_exhaustion_does_not_recycle_ids)
{
    using robotweax::srt::compat::next_registry_handle;
    REQUIRE_EQ(next_registry_handle(1), 2);
    REQUIRE_EQ(next_registry_handle(SRTGROUP_MASK - 2), SRTGROUP_MASK - 1);
    REQUIRE_EQ(next_registry_handle(SRTGROUP_MASK - 1), SRT_INVALID_SOCK);
    REQUIRE_EQ(next_registry_handle(SRT_INVALID_SOCK), SRT_INVALID_SOCK);
}

TEST(lifecycle_group_clock_serializes_parallel_drift_and_readiness)
{
    auto group = std::make_shared<robotweax::srt::compat::GroupRecord>();
    const auto origin = ConnectionRuntime::Clock::now();
    const auto peer = IpEndpoint::loopback(9'000);
    const auto make_runtime = [&] {
        return std::make_shared<ConnectionRuntime>(
            ConnectionRuntime::Configuration {
                .group = group,
                .peer = peer,
                .peer_socket_id = 77,
                .negotiated_options = {.receive_tsbpd = true,
                    .receive_delay_milliseconds = 120},
                .origin = origin,
            });
    };
    const auto first = make_runtime();
    const auto second = make_runtime();
    std::barrier start {2};
    std::atomic_bool valid = true;
    const auto exercise = [&](const auto& runtime) {
        constexpr std::array<std::byte, 4> zero {};
        PacketView keepalive;
        keepalive.kind = PacketKind::control;
        keepalive.control.type = ControlType::keepalive;
        keepalive.payload = zero;
        start.arrive_and_wait();
        for (std::uint32_t index = 0; index < 2'000; ++index) {
            keepalive.control.timestamp = PacketTimestamp {index};
            runtime->process_packet(keepalive, peer);
            if (runtime->next_readable_message_sequence().has_value()) {
                valid = false;
            }
            (void)runtime->next_readable_deadline();
        }
    };
    std::thread first_thread([&] {
        exercise(first);
    });
    std::thread second_thread([&] {
        exercise(second);
    });
    // The runtime-owned clock must remain valid without a registry owner.
    group.reset();
    first_thread.join();
    second_thread.join();
    REQUIRE(valid.load());
}

TEST(lifecycle_startup_initializes_platform_networking)
{
    REQUIRE_EQ(srt_startup(), 0);

    addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
    addrinfo* result = nullptr;
    REQUIRE_EQ(getaddrinfo("127.0.0.1", "9000", &hints, &result), 0);
    REQUIRE(result != nullptr);
    freeaddrinfo(result);

    REQUIRE_EQ(srt_cleanup(), 0);
}

TEST(lifecycle_final_cleanup_restarts_runtime_service_generations)
{
    REQUIRE_EQ(srt_startup(), 0);
    const auto first = robotweax::srt::compat::acquire_runtime_scheduler();
    const auto first_work =
        robotweax::srt::compat::acquire_runtime_work_executor();
    REQUIRE(first != nullptr);
    REQUIRE(first_work != nullptr);
    REQUIRE(first->snapshot().accepting);
    REQUIRE(first_work->snapshot().accepting);
    REQUIRE_EQ(first->snapshot().queue_capacity, 8'192U);
    REQUIRE_EQ(first->snapshot().timer_capacity, 8'192U);
    REQUIRE_EQ(first_work->snapshot().worker_count, 4U);
    REQUIRE_EQ(first_work->snapshot().queue_capacity, 1'024U);

    REQUIRE_EQ(srt_cleanup(), 0);
    REQUIRE(!first->snapshot().accepting);
    REQUIRE(!first_work->snapshot().accepting);

    REQUIRE_EQ(srt_startup(), 0);
    const auto second = robotweax::srt::compat::acquire_runtime_scheduler();
    const auto second_work =
        robotweax::srt::compat::acquire_runtime_work_executor();
    REQUIRE(second != nullptr);
    REQUIRE(second_work != nullptr);
    REQUIRE(second.get() != first.get());
    REQUIRE(second_work.get() != first_work.get());
    REQUIRE(second->snapshot().accepting);
    REQUIRE(second_work->snapshot().accepting);

    REQUIRE_EQ(srt_cleanup(), 0);
    REQUIRE(!second->snapshot().accepting);
    REQUIRE(!second_work->snapshot().accepting);
}

TEST(lifecycle_nested_startup_cleanup_releases_only_the_final_reference)
{
    constexpr std::size_t reference_count = 64;
    for (std::size_t index = 0; index < reference_count; ++index) {
        REQUIRE_EQ(srt_startup(), 0);
    }

    const SRTSOCKET socket = srt_create_socket();
    REQUIRE(socket != SRT_INVALID_SOCK);
    for (std::size_t index = 1; index < reference_count; ++index) {
        REQUIRE_EQ(srt_cleanup(), 0);
        REQUIRE_EQ(srt_getsockstate(socket), SRTS_INIT);
    }

    REQUIRE_EQ(srt_cleanup(), 0);
    REQUIRE_EQ(srt_getsockstate(socket), SRTS_NONEXIST);

    // Cleanup is deliberately idempotent when the process-wide reference
    // count is already zero.
    REQUIRE_EQ(srt_cleanup(), 0);
    REQUIRE_EQ(srt_getsockstate(socket), SRTS_NONEXIST);
}

TEST(lifecycle_repeated_startup_cleanup_cycles_do_not_alias_stale_handles)
{
    constexpr std::size_t cycle_count = 128;
    SRTSOCKET previous = SRT_INVALID_SOCK;
    for (std::size_t index = 0; index < cycle_count; ++index) {
        REQUIRE_EQ(srt_startup(), 0);
        const SRTSOCKET current = srt_create_socket();
        REQUIRE(current != SRT_INVALID_SOCK);
        REQUIRE(current != previous);
        REQUIRE_EQ(srt_getsockstate(current), SRTS_INIT);

        REQUIRE_EQ(srt_cleanup(), 0);
        REQUIRE_EQ(srt_getsockstate(current), SRTS_NONEXIST);
        if (previous != SRT_INVALID_SOCK) {
            REQUIRE_EQ(srt_getsockstate(previous), SRTS_NONEXIST);
        }
        previous = current;
    }
}

TEST(lifecycle_cleanup_retires_group_handles_and_generations)
{
    REQUIRE_EQ(srt_startup(), 0);
    const SRTSOCKET first = srt_create_group(SRT_GTYPE_BROADCAST);
    REQUIRE(first != SRT_INVALID_SOCK);
    REQUIRE_EQ(srt_getsockstate(first), SRTS_BROKEN);
    REQUIRE_EQ(srt_cleanup(), 0);
    REQUIRE_EQ(srt_getsockstate(first), SRTS_NONEXIST);

    REQUIRE_EQ(srt_startup(), 0);
    const SRTSOCKET second = srt_create_group(SRT_GTYPE_BROADCAST);
    REQUIRE(second != SRT_INVALID_SOCK);
    REQUIRE(second != first);
    REQUIRE_EQ(srt_getsockstate(first), SRTS_NONEXIST);
    REQUIRE_EQ(srt_cleanup(), 0);
    REQUIRE_EQ(srt_getsockstate(second), SRTS_NONEXIST);
}

TEST(lifecycle_cleanup_closes_an_active_listener)
{
    REQUIRE_EQ(srt_startup(), 0);
    const SRTSOCKET listener = create_active_listener();
    if (listener == SRT_INVALID_SOCK) {
        REQUIRE_EQ(srt_cleanup(), 0);
        return;
    }

    REQUIRE_EQ(srt_cleanup(), 0);
    REQUIRE_EQ(srt_getsockstate(listener), SRTS_NONEXIST);
}

TEST(lifecycle_cleanup_releases_an_epoll_waiter)
{
    REQUIRE_EQ(srt_startup(), 0);
    const SRTSOCKET socket = srt_create_socket();
    REQUIRE(socket != SRT_INVALID_SOCK);
    const int poll = srt_epoll_create();
    REQUIRE(poll >= 0);
    const int events = SRT_EPOLL_IN | SRT_EPOLL_ERR;
    REQUIRE_EQ(srt_epoll_add_usock(poll, socket, &events), 0);

    std::atomic_bool waiter_entered = false;
    std::promise<EpollWaitResult> completion;
    auto completed = completion.get_future();
    std::thread waiter([&] {
        std::array<SRT_EPOLL_EVENT, 1> ready{};
        waiter_entered.store(true, std::memory_order_release);
        const auto start = std::chrono::steady_clock::now();
        const int result = srt_epoll_uwait(
            poll, ready.data(), static_cast<int>(ready.size()), 5'000);
        completion.set_value({
            .result = result,
            .error = srt_getlasterror(nullptr),
            .event = ready[0],
            .elapsed = std::chrono::steady_clock::now() - start,
        });
    });

    while (!waiter_entered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    REQUIRE_EQ(srt_cleanup(), 0);

    const auto wait_status = completed.wait_for(2s);
    // Even a broken wakeup remains bounded by the epoll timeout, allowing the
    // test process to report a normal failure rather than hanging forever.
    waiter.join();
    REQUIRE_EQ(wait_status, std::future_status::ready);
    const EpollWaitResult result = completed.get();
    const bool poll_was_released =
        result.result == SRT_ERROR
        && result.error == SRT_EINVPOLLID;
    const bool socket_reported_closed =
        result.result == 1
        && result.event.fd == socket
        && result.event.events == SRT_EPOLL_ERR;
    REQUIRE(poll_was_released || socket_reported_closed);
    REQUIRE(result.elapsed < 2s);
    REQUIRE_EQ(srt_getsockstate(socket), SRTS_NONEXIST);
    REQUIRE_EQ(srt_epoll_release(poll), SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPOLLID);
}

TEST(lifecycle_concurrent_close_and_cleanup_is_idempotent)
{
    REQUIRE_EQ(srt_startup(), 0);
    constexpr std::size_t socket_count = 8;
    constexpr std::size_t closer_count = 4;
    std::array<SRTSOCKET, socket_count> sockets{};
    sockets.fill(SRT_INVALID_SOCK);
    for (auto& socket : sockets) {
        socket = srt_create_socket();
        REQUIRE(socket != SRT_INVALID_SOCK);
    }

    std::barrier start{static_cast<std::ptrdiff_t>(closer_count + 1U)};
    std::atomic_size_t close_failures = 0;
    std::vector<std::thread> closers;
    closers.reserve(closer_count);
    for (std::size_t worker = 0; worker < closer_count; ++worker) {
        closers.emplace_back([&, worker] {
            start.arrive_and_wait();
            for (std::size_t offset = 0; offset < sockets.size(); ++offset) {
                const std::size_t index =
                    (offset + worker) % sockets.size();
                if (srt_close(sockets[index]) != 0) {
                    close_failures.fetch_add(1U, std::memory_order_relaxed);
                }
            }
        });
    }

    start.arrive_and_wait();
    REQUIRE_EQ(srt_cleanup(), 0);
    for (auto& closer : closers) {
        closer.join();
    }

    REQUIRE_EQ(close_failures.load(std::memory_order_relaxed), 0U);
    for (const SRTSOCKET socket : sockets) {
        REQUIRE_EQ(srt_getsockstate(socket), SRTS_NONEXIST);
        REQUIRE_EQ(srt_close(socket), 0);
    }

    // The runtime remains reusable after the close/cleanup race.
    const SRTSOCKET replacement = srt_create_socket();
    REQUIRE(replacement != SRT_INVALID_SOCK);
    for (const SRTSOCKET socket : sockets) {
        REQUIRE(replacement != socket);
    }
    REQUIRE_EQ(srt_cleanup(), 0);
    REQUIRE_EQ(srt_getsockstate(replacement), SRTS_NONEXIST);
}

TEST(lifecycle_parallel_balanced_clients_preserve_the_runtime_generation)
{
    constexpr std::size_t worker_count = 8;
    constexpr std::size_t iterations = 128;
    std::barrier start{
        static_cast<std::ptrdiff_t>(worker_count)};
    std::atomic_size_t failures = 0;
    std::vector<std::thread> workers;
    workers.reserve(worker_count);

    for (std::size_t worker = 0; worker < worker_count; ++worker) {
        workers.emplace_back([&] {
            start.arrive_and_wait();
            for (std::size_t iteration = 0;
                 iteration < iterations; ++iteration) {
                if (srt_startup() != 0) {
                    failures.fetch_add(1U, std::memory_order_relaxed);
                    continue;
                }

                const SRTSOCKET socket = srt_create_socket();
                const int poll = srt_epoll_create();
                if (socket == SRT_INVALID_SOCK || poll < 0) {
                    failures.fetch_add(1U, std::memory_order_relaxed);
                } else {
                    const int events =
                        SRT_EPOLL_OUT | SRT_EPOLL_ERR;
                    if (srt_epoll_add_usock(
                            poll, socket, &events) != 0
                        || srt_getsockstate(socket) != SRTS_INIT
                        || srt_close(socket) != 0
                        || srt_epoll_release(poll) != 0) {
                        failures.fetch_add(
                            1U, std::memory_order_relaxed);
                    }
                }
                if (srt_cleanup() != 0) {
                    failures.fetch_add(1U, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }

    REQUIRE_EQ(failures.load(std::memory_order_relaxed), 0U);

    // The final balanced cleanup must have ended the generation, and a new
    // implicit generation must still be usable.
    const SRTSOCKET replacement = srt_create_socket();
    REQUIRE(replacement != SRT_INVALID_SOCK);
    const int replacement_poll = srt_epoll_create();
    REQUIRE(replacement_poll >= 0);
    REQUIRE_EQ(srt_epoll_release(replacement_poll), 0);
    REQUIRE_EQ(srt_close(replacement), 0);
    REQUIRE_EQ(srt_cleanup(), 0);
    REQUIRE_EQ(srt_getsockstate(replacement), SRTS_NONEXIST);
}

TEST(lifecycle_implicit_creation_never_enters_a_clearing_generation)
{
    constexpr std::size_t creator_count = 4;
    constexpr std::size_t iterations = 512;
    std::barrier start{
        static_cast<std::ptrdiff_t>(creator_count + 1U)};
    std::atomic_size_t creation_failures = 0;
    std::vector<std::thread> creators;
    creators.reserve(creator_count);

    for (std::size_t creator = 0;
         creator < creator_count; ++creator) {
        creators.emplace_back([&] {
            start.arrive_and_wait();
            for (std::size_t iteration = 0;
                 iteration < iterations; ++iteration) {
                const SRTSOCKET socket = srt_create_socket();
                if (socket == SRT_INVALID_SOCK) {
                    creation_failures.fetch_add(
                        1U, std::memory_order_relaxed);
                }
                const int poll = srt_epoll_create();
                if (poll < 0) {
                    creation_failures.fetch_add(
                        1U, std::memory_order_relaxed);
                }

                if (socket != SRT_INVALID_SOCK) {
                    (void)srt_close(socket);
                }
                if (poll >= 0) {
                    // Cleanup is allowed to have removed the poll between
                    // creation and release.
                    (void)srt_epoll_release(poll);
                }
            }
        });
    }

    std::thread cleaner([&] {
        start.arrive_and_wait();
        for (std::size_t iteration = 0;
             iteration < creator_count * iterations; ++iteration) {
            (void)srt_cleanup();
            if ((iteration & 7U) == 0U) {
                std::this_thread::yield();
            }
        }
    });

    for (auto& creator : creators) {
        creator.join();
    }
    cleaner.join();

    REQUIRE_EQ(
        creation_failures.load(std::memory_order_relaxed), 0U);

    // Drain a possible final implicit reference and prove a clean restart.
    REQUIRE_EQ(srt_cleanup(), 0);
    REQUIRE_EQ(srt_startup(), 0);
    const SRTSOCKET replacement = srt_create_socket();
    REQUIRE(replacement != SRT_INVALID_SOCK);
    REQUIRE_EQ(srt_cleanup(), 0);
    REQUIRE_EQ(srt_getsockstate(replacement), SRTS_NONEXIST);
}

TEST(lifecycle_new_generation_waits_for_the_final_cleanup)
{
    REQUIRE_EQ(srt_startup(), 0);
    const SRTSOCKET inherited = srt_create_socket();
    REQUIRE(inherited != SRT_INVALID_SOCK);
    const auto record = SocketRegistry::instance().find(inherited);
    REQUIRE(record != nullptr);

    std::promise<void> worker_started;
    auto worker_is_started = worker_started.get_future();
    std::promise<void> release_worker;
    const std::shared_future<void> worker_release =
        release_worker.get_future().share();
    {
        std::lock_guard lock(record->mutex);
        record->connect_worker = std::thread(
            [&worker_started, worker_release] {
                worker_started.set_value();
                worker_release.wait();
            });
    }
    worker_is_started.wait();

    std::atomic_int cleanup_result = SRT_ERROR;
    std::thread cleanup([&] {
        cleanup_result.store(
            srt_cleanup(), std::memory_order_release);
    });

    bool cleanup_entered_shutdown = false;
    const auto shutdown_deadline =
        std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < shutdown_deadline) {
        {
            std::lock_guard lock(record->mutex);
            cleanup_entered_shutdown =
                record->state == SRTS_CLOSING;
        }
        if (cleanup_entered_shutdown) {
            break;
        }
        std::this_thread::yield();
    }

    std::atomic_bool creator_entered = false;
    std::promise<SRTSOCKET> created;
    auto created_socket = created.get_future();
    std::thread creator([&] {
        creator_entered.store(true, std::memory_order_release);
        created.set_value(srt_create_socket());
    });
    while (!creator_entered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    const bool creator_waited_for_cleanup =
        created_socket.wait_for(100ms) == std::future_status::timeout;
    release_worker.set_value();
    cleanup.join();
    creator.join();

    REQUIRE(cleanup_entered_shutdown);
    REQUIRE(creator_waited_for_cleanup);
    REQUIRE_EQ(cleanup_result.load(std::memory_order_acquire), 0);
    const SRTSOCKET replacement = created_socket.get();
    REQUIRE(replacement != SRT_INVALID_SOCK);
    REQUIRE(replacement != inherited);
    REQUIRE_EQ(srt_close(replacement), 0);
    REQUIRE_EQ(srt_cleanup(), 0);
    REQUIRE_EQ(srt_getsockstate(replacement), SRTS_NONEXIST);
}

TEST(lifecycle_cleanup_releases_multiple_epoll_waiters)
{
    constexpr std::size_t waiter_count = 12;
    REQUIRE_EQ(srt_startup(), 0);

    std::array<SRTSOCKET, waiter_count> sockets{};
    std::array<int, waiter_count> polls{};
    std::array<int, waiter_count> results{};
    std::array<int, waiter_count> errors{};
    std::array<SRT_EPOLL_EVENT, waiter_count> events{};
    std::barrier start{
        static_cast<std::ptrdiff_t>(waiter_count + 1U)};
    std::atomic_size_t completed = 0;
    std::vector<std::thread> waiters;
    waiters.reserve(waiter_count);

    for (std::size_t index = 0; index < waiter_count; ++index) {
        sockets[index] = srt_create_socket();
        REQUIRE(sockets[index] != SRT_INVALID_SOCK);
        polls[index] = srt_epoll_create();
        REQUIRE(polls[index] >= 0);
        const int subscribed_events = SRT_EPOLL_IN | SRT_EPOLL_ERR;
        REQUIRE_EQ(srt_epoll_add_usock(
                       polls[index], sockets[index],
                       &subscribed_events),
            0);
        waiters.emplace_back([&, index] {
            std::array<SRT_EPOLL_EVENT, 1> ready{};
            start.arrive_and_wait();
            results[index] = srt_epoll_uwait(
                polls[index], ready.data(),
                static_cast<int>(ready.size()), 5'000);
            errors[index] = srt_getlasterror(nullptr);
            events[index] = ready[0];
            completed.fetch_add(1U, std::memory_order_release);
        });
    }

    start.arrive_and_wait();
    REQUIRE_EQ(srt_cleanup(), 0);
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (completed.load(std::memory_order_acquire) != waiter_count
        && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    for (auto& waiter : waiters) {
        waiter.join();
    }

    REQUIRE_EQ(completed.load(std::memory_order_acquire), waiter_count);
    for (std::size_t index = 0; index < waiter_count; ++index) {
        const bool poll_was_released =
            results[index] == SRT_ERROR
            && errors[index] == SRT_EINVPOLLID;
        const bool socket_reported_closed =
            results[index] == 1
            && events[index].fd == sockets[index]
            && events[index].events == SRT_EPOLL_ERR;
        REQUIRE(poll_was_released || socket_reported_closed);
        REQUIRE_EQ(srt_getsockstate(sockets[index]), SRTS_NONEXIST);
    }
}

TEST(lifecycle_released_epoll_rejects_every_late_mutation)
{
    REQUIRE_EQ(srt_startup(), 0);
    const SRTSOCKET socket = srt_create_socket();
    REQUIRE(socket != SRT_INVALID_SOCK);
    const int poll = srt_epoll_create();
    REQUIRE(poll >= 0);
    const int events = SRT_EPOLL_IN | SRT_EPOLL_ERR;
    REQUIRE_EQ(srt_epoll_add_usock(poll, socket, &events), 0);
    REQUIRE_EQ(srt_epoll_release(poll), 0);

    const auto require_invalid_poll = [](int result) {
        REQUIRE_EQ(result, SRT_ERROR);
        REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPOLLID);
    };
    require_invalid_poll(srt_epoll_clear_usocks(poll));
    require_invalid_poll(
        srt_epoll_add_usock(poll, socket, &events));
    require_invalid_poll(
        srt_epoll_update_usock(poll, socket, &events));
    require_invalid_poll(srt_epoll_remove_usock(poll, socket));
    require_invalid_poll(
        srt_epoll_add_ssock(poll, static_cast<SYSSOCKET>(-1), &events));
    require_invalid_poll(
        srt_epoll_update_ssock(
            poll, static_cast<SYSSOCKET>(-1), &events));
    require_invalid_poll(
        srt_epoll_remove_ssock(
            poll, static_cast<SYSSOCKET>(-1)));
    require_invalid_poll(srt_epoll_set(poll, SRT_EPOLL_ENABLE_EMPTY));

    REQUIRE_EQ(srt_close(socket), 0);
    REQUIRE_EQ(srt_cleanup(), 0);
}

TEST(lifecycle_cleanup_interrupts_a_nonblocking_connect_attempt)
{
    REQUIRE_EQ(srt_startup(), 0);
    const SRTSOCKET socket = srt_create_socket();
    REQUIRE(socket != SRT_INVALID_SOCK);

    const bool receive_synchronously = false;
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_RCVSYN,
                   &receive_synchronously,
                   static_cast<int>(sizeof(receive_synchronously))),
        0);
    const std::int32_t connection_timeout_milliseconds = 30'000;
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_CONNTIMEO,
                   &connection_timeout_milliseconds,
                   static_cast<int>(
                       sizeof(connection_timeout_milliseconds))),
        0);

    sockaddr_in unreachable{};
    unreachable.sin_family = AF_INET;
    unreachable.sin_port = htons(65'021);
    unreachable.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (srt_connect(socket,
            reinterpret_cast<const sockaddr*>(&unreachable),
            static_cast<int>(sizeof(unreachable))) == SRT_ERROR) {
        int system_error = 0;
        REQUIRE_EQ(srt_getlasterror(&system_error), SRT_ESOCKFAIL);
        REQUIRE(system_error != 0);
        REQUIRE_EQ(srt_cleanup(), 0);
        return;
    }

    const auto record = SocketRegistry::instance().find(socket);
    REQUIRE(record != nullptr);
    const auto start = std::chrono::steady_clock::now();
    REQUIRE_EQ(srt_cleanup(), 0);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    REQUIRE(elapsed < 2s);
    {
        std::lock_guard lock(record->mutex);
        REQUIRE_EQ(record->state, SRTS_CLOSED);
        REQUIRE(!record->connect_worker.joinable());
    }
    REQUIRE_EQ(srt_getsockstate(socket), SRTS_NONEXIST);
}

TEST(lifecycle_cleanup_closes_listener_caller_and_accepted_socket)
{
    REQUIRE_EQ(srt_startup(), 0);
    const SRTSOCKET listener = create_active_listener();
    if (listener == SRT_INVALID_SOCK) {
        REQUIRE_EQ(srt_cleanup(), 0);
        return;
    }

    sockaddr_in listener_name{};
    int listener_name_size = static_cast<int>(sizeof(listener_name));
    REQUIRE_EQ(srt_getsockname(listener,
                   reinterpret_cast<sockaddr*>(&listener_name),
                   &listener_name_size),
        0);

    const SRTSOCKET caller = srt_create_socket();
    REQUIRE(caller != SRT_INVALID_SOCK);
    const std::int32_t connection_timeout_milliseconds = 2'000;
    REQUIRE_EQ(srt_setsockflag(caller, SRTO_CONNTIMEO,
                   &connection_timeout_milliseconds,
                   static_cast<int>(
                       sizeof(connection_timeout_milliseconds))),
        0);
    REQUIRE_EQ(srt_connect(caller,
                   reinterpret_cast<const sockaddr*>(&listener_name),
                   static_cast<int>(sizeof(listener_name))),
        0);
    const SRTSOCKET accepted = srt_accept(listener, nullptr, nullptr);
    REQUIRE(accepted != SRT_INVALID_SOCK);

    constexpr char payload[] = "active cleanup payload";
    REQUIRE_EQ(srt_send(caller, payload,
                   static_cast<int>(sizeof(payload))),
        static_cast<int>(sizeof(payload)));

    const auto listener_record =
        SocketRegistry::instance().find(listener);
    const auto caller_record =
        SocketRegistry::instance().find(caller);
    const auto accepted_record =
        SocketRegistry::instance().find(accepted);
    REQUIRE(listener_record != nullptr);
    REQUIRE(caller_record != nullptr);
    REQUIRE(accepted_record != nullptr);

    const auto start = std::chrono::steady_clock::now();
    REQUIRE_EQ(srt_cleanup(), 0);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    REQUIRE(elapsed < 2s);

    for (const auto& record :
        {listener_record, caller_record, accepted_record}) {
        std::lock_guard lock(record->mutex);
        REQUIRE_EQ(record->state, SRTS_CLOSED);
        REQUIRE(record->runtime == nullptr);
        REQUIRE(record->channel == nullptr);
    }
    REQUIRE_EQ(srt_getsockstate(listener), SRTS_NONEXIST);
    REQUIRE_EQ(srt_getsockstate(caller), SRTS_NONEXIST);
    REQUIRE_EQ(srt_getsockstate(accepted), SRTS_NONEXIST);
}

TEST(lifecycle_cleanup_bypasses_connected_socket_linger_deadlines)
{
    REQUIRE_EQ(srt_startup(), 0);
    constexpr std::size_t socket_count = 2;
    std::array<SRTSOCKET, socket_count> sockets{};
    std::array<std::shared_ptr<robotweax::srt::compat::SocketRecord>,
        socket_count>
        records{};
    std::array<std::shared_ptr<DatagramChannel>, socket_count>
        channels{};
    std::array<std::shared_ptr<ConnectionRuntime>, socket_count>
        runtimes{};

    SocketOptions options;
    REQUIRE_EQ(options.set(
                   SocketOption::transmission_type,
                   static_cast<std::int64_t>(
                       TransmissionType::file)),
        Error::none);
    REQUIRE_EQ(options.set(
                   SocketOption::maximum_payload_size, 2),
        Error::none);
    const Ipv4Endpoint peer{
        .address = {192, 0, 2, 90},
        .port = 22'000,
    };
    const std::array<std::byte, 2> payload{
        std::byte{'o'}, std::byte{'k'}};

    for (std::size_t index = 0; index < socket_count; ++index) {
        sockets[index] = srt_create_socket();
        REQUIRE(sockets[index] != SRT_INVALID_SOCK);
        records[index] =
            SocketRegistry::instance().find(sockets[index]);
        REQUIRE(records[index] != nullptr);
        channels[index] = std::make_shared<DatagramChannel>();
        channels[index]->set_send_hook_for_testing(
            accept_datagram, nullptr);
        runtimes[index] = std::make_shared<ConnectionRuntime>(
            ConnectionRuntime::Configuration{
                .channel = channels[index],
                .peer = peer,
                .peer_socket_id =
                    static_cast<std::uint32_t>(900U + index),
                .initial_sequence =
                    SequenceNumber{
                        static_cast<std::uint32_t>(5'000U + index)},
                .flow_window_packets = 256,
                .options = options,
                .origin = ConnectionRuntime::Clock::now(),
            });
        {
            std::lock_guard lock(records[index]->mutex);
            records[index]->state = SRTS_CONNECTED;
            records[index]->channel = channels[index];
            records[index]->runtime = runtimes[index];
            records[index]->public_options.transmission_type =
                SRTT_FILE;
            records[index]->public_options.send_synchronous =
                index == 0U;
            records[index]->public_options.linger_enabled = true;
            records[index]->public_options.linger_seconds = 60;
        }
        REQUIRE(channels[index]->register_connection(
            static_cast<std::uint32_t>(sockets[index]),
            runtimes[index]));
        REQUIRE_EQ(runtimes[index]->queue_stream(
                       payload, false, -1).status,
            robotweax::srt::compat::MessageIoStatus::success);
    }

    // Put the asynchronous socket into the deferred-close queue. The
    // synchronous socket remains connected with an undrained send buffer.
    REQUIRE_EQ(srt_close(sockets[1]), 0);
    REQUIRE_EQ(
        SocketRegistry::instance().state(sockets[1]), SRTS_CLOSING);

    const auto start = std::chrono::steady_clock::now();
    REQUIRE_EQ(srt_cleanup(), 0);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    REQUIRE(elapsed < 2s);

    for (std::size_t index = 0; index < socket_count; ++index) {
        std::lock_guard lock(records[index]->mutex);
        REQUIRE_EQ(records[index]->state, SRTS_CLOSED);
        REQUIRE(records[index]->runtime == nullptr);
        REQUIRE(records[index]->channel == nullptr);
        REQUIRE_EQ(
            SocketRegistry::instance().state(sockets[index]),
            SRTS_NONEXIST);
    }
}

TEST(lifecycle_cleanup_stops_an_active_key_rotation)
{
    REQUIRE_EQ(srt_startup(), 0);
    const SRTSOCKET socket = srt_create_socket();
    REQUIRE(socket != SRT_INVALID_SOCK);
    const auto record = SocketRegistry::instance().find(socket);
    REQUIRE(record != nullptr);

    const CryptoConfiguration crypto_configuration{
        .passphrase = "lifecycle rotation fixture",
        .key_length = 16,
        .refresh_rate_packets = 3,
        .preannouncement_packets = 1,
    };
    auto sender_crypto =
        std::make_shared<CryptoSession>(crypto_configuration);
    CryptoSession receiver_crypto{crypto_configuration};
    REQUIRE_EQ(sender_crypto->start_initiator(), Error::none);
    REQUIRE_EQ(receiver_crypto.accept_key_material(
                   sender_crypto->pending_key_material(), true),
        Error::none);
    REQUIRE_EQ(sender_crypto->acknowledge_key_material(
                   receiver_crypto.key_material_response(), true),
        Error::none);
    confirm_directional_test_keys(*sender_crypto, receiver_crypto);
    REQUIRE_EQ(sender_crypto->note_data_packet_sent(), Error::none);
    REQUIRE_EQ(sender_crypto->note_data_packet_sent(), Error::none);

    KeyRequestObserver observer;
    auto channel = std::make_shared<DatagramChannel>();
    channel->set_send_hook_for_testing(
        accept_and_observe_key_request, &observer);
    SocketOptions options;
    auto runtime = std::make_shared<ConnectionRuntime>(
        ConnectionRuntime::Configuration{
            .channel = channel,
            .peer = Ipv4Endpoint{
                .address = {192, 0, 2, 91},
                .port = 22'001,
            },
            .peer_socket_id = 910,
            .initial_sequence = SequenceNumber{6'000},
            .flow_window_packets = 256,
            .options = options,
            .origin = ConnectionRuntime::Clock::now(),
            .crypto = sender_crypto,
        });
    {
        std::lock_guard lock(record->mutex);
        record->state = SRTS_CONNECTED;
        record->channel = channel;
        record->runtime = runtime;
        record->crypto = sender_crypto;
    }
    REQUIRE(channel->register_connection(
        static_cast<std::uint32_t>(socket), runtime));

    std::atomic_bool stop_service = false;
    std::thread service([&] {
        while (!stop_service.load(std::memory_order_acquire)) {
            (void)runtime->poll();
            std::this_thread::yield();
        }
    });

    const auto request_deadline =
        std::chrono::steady_clock::now() + 2s;
    while (!observer.request_seen.load(std::memory_order_acquire)
        && std::chrono::steady_clock::now() < request_deadline) {
        std::this_thread::yield();
    }
    const bool request_was_seen =
        observer.request_seen.load(std::memory_order_acquire);
    const CryptoState state_before_cleanup =
        runtime->sender_crypto_state();

    const auto start = std::chrono::steady_clock::now();
    const int cleanup_result = srt_cleanup();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    stop_service.store(true, std::memory_order_release);
    service.join();

    REQUIRE(request_was_seen);
    REQUIRE_EQ(state_before_cleanup, CryptoState::securing);
    REQUIRE_EQ(cleanup_result, 0);
    REQUIRE(elapsed < 2s);
    {
        std::lock_guard lock(record->mutex);
        REQUIRE_EQ(record->state, SRTS_CLOSED);
        REQUIRE(record->runtime == nullptr);
        REQUIRE(record->crypto == nullptr);
    }
    REQUIRE_EQ(srt_getsockstate(socket), SRTS_NONEXIST);
}
