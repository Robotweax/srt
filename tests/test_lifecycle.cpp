#include "test.hpp"
#include "crypto_test_helpers.hpp"

#include "robotweax/srt/codec.hpp"
#include "robotweax/srt/crypto.hpp"
#include "robotweax/srt/packet.hpp"
#include "robotweax/srt/socket_options.hpp"
#include "compat/runtime_scheduler_service.hpp"
#include "compat/runtime_work_executor_service.hpp"
#include "compat/socket_registry.hpp"
#include "compat/socket_io.hpp"
#include "compat/group_registry.hpp"
#include "compat/random_identity.hpp"
#include "compat/transport_runtime.hpp"
#include "srt/srt.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <mutex>
#include <future>
#include <memory>
#include <span>
#include <string_view>
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

struct ListenerCleanupReentry {
    std::promise<void> entered;
    std::shared_future<void> release;
    std::promise<std::pair<SRTSOCKET, int>> finished;
};

int reenter_creation_from_listener(
    void* opaque, SRTSOCKET, int, const sockaddr*, const char*)
{
    auto& probe = *static_cast<ListenerCleanupReentry*>(opaque);
    probe.entered.set_value();
    probe.release.wait();
    const SRTSOCKET socket = srt_create_socket();
    probe.finished.set_value({socket, srt_getlasterror(nullptr)});
    return SRT_ERROR;
}

} // namespace

TEST(closed_handle_history_answers_membership_across_eviction_and_rebuild)
{
    using robotweax::srt::compat::ClosedHandleHistory;
    ClosedHandleHistory history;
    REQUIRE(!history.contains(1));
    REQUIRE(!history.contains(0));
    REQUIRE(!history.contains(-5));

    // Fill exactly to capacity: every remembered handle is found.
    constexpr auto capacity = ClosedHandleHistory::capacity;
    for (SRTSOCKET handle = 1; handle <= static_cast<SRTSOCKET>(capacity);
        ++handle) {
        history.remember(handle);
    }
    for (SRTSOCKET handle = 1; handle <= static_cast<SRTSOCKET>(capacity);
        ++handle) {
        REQUIRE(history.contains(handle));
    }
    REQUIRE(!history.contains(static_cast<SRTSOCKET>(capacity) + 1));

    // Churn through three more capacities: this evicts more than `capacity`
    // entries, which forces a rebuild of the lookup table from the ring.
    // Only the newest `capacity` handles remain visible afterwards, and
    // handles with the group mask bit set share the table without clashes.
    constexpr SRTSOCKET group_bit = 1 << 30;
    const SRTSOCKET last = static_cast<SRTSOCKET>(capacity) * 4;
    for (SRTSOCKET handle = static_cast<SRTSOCKET>(capacity) + 1;
        handle <= last; ++handle) {
        history.remember((handle % 3 == 0) ? (handle | group_bit) : handle);
    }
    for (SRTSOCKET handle = last - static_cast<SRTSOCKET>(capacity) + 1;
        handle <= last; ++handle) {
        const SRTSOCKET remembered =
            (handle % 3 == 0) ? (handle | group_bit) : handle;
        REQUIRE(history.contains(remembered));
        REQUIRE(!history.contains(
            (handle % 3 == 0) ? handle : (handle | group_bit)));
    }
    for (SRTSOCKET handle = 1;
        handle <= last - static_cast<SRTSOCKET>(capacity); ++handle) {
        REQUIRE(!history.contains(handle));
        REQUIRE(!history.contains(handle | group_bit));
    }

    history.clear();
    REQUIRE(!history.contains(last));
    history.remember(7);
    REQUIRE(history.contains(7));
}

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

TEST(lifecycle_handle_permutation_never_recycles_ids)
{
    using robotweax::srt::compat::HandleSpace;
    using robotweax::srt::compat::next_registry_handle;
    using robotweax::srt::compat::registry_handle_space;
    // A permutation cannot repeat; this checks the construction on a prefix
    // and that both handle spaces are keyed independently.
    constexpr std::uint32_t prefix = 1U << 17;
    std::vector<SRTSOCKET> sockets;
    std::vector<SRTSOCKET> groups;
    std::uint32_t socket_index = 0;
    std::uint32_t group_index = 0;
    while (sockets.size() < prefix) {
        const SRTSOCKET handle =
            next_registry_handle(HandleSpace::socket, socket_index);
        REQUIRE(handle > 0 && handle < SRTGROUP_MASK);
        sockets.push_back(handle);
    }
    while (groups.size() < 64U) {
        groups.push_back(next_registry_handle(HandleSpace::group, group_index));
    }
    REQUIRE(!std::equal(groups.begin(), groups.end(), sockets.begin()));
    std::sort(sockets.begin(), sockets.end());
    REQUIRE(
        std::adjacent_find(sockets.begin(), sockets.end()) == sockets.end());

    // The last position may be the one that maps to zero; after it the space
    // is exhausted and allocation fails instead of wrapping around.
    std::uint32_t last = registry_handle_space - 1U;
    (void)next_registry_handle(HandleSpace::socket, last);
    REQUIRE_EQ(last, registry_handle_space);
    REQUIRE_EQ(
        next_registry_handle(HandleSpace::socket, last), SRT_INVALID_SOCK);
    REQUIRE_EQ(last, registry_handle_space);
}

TEST(lifecycle_socket_and_group_identities_are_unpredictable)
{
    // Handles are wire socket IDs and SRTO_ISN seeds the DATA window. Neither
    // may follow from a previously observed value: no consecutive handles and
    // no fixed ISN function of the handle. A random source makes each check
    // fail with a probability below 1e-7.
    constexpr std::size_t count = 64;
    REQUIRE_EQ(srt_startup(), 0);
    std::vector<SRTSOCKET> sockets;
    std::vector<std::int32_t> sequences;
    for (std::size_t index = 0; index < count; ++index) {
        const SRTSOCKET socket = srt_create_socket();
        REQUIRE(socket != SRT_INVALID_SOCK);
        REQUIRE(socket > 0);
        REQUIRE((socket & SRTGROUP_MASK) == 0);
        std::int32_t sequence = -1;
        int size = sizeof(sequence);
        REQUIRE_EQ(srt_getsockflag(socket, SRTO_ISN, &sequence, &size), 0);
        REQUIRE(sequence >= 0);
        sockets.push_back(socket);
        sequences.push_back(sequence);
    }
    std::size_t adjacent_handles = 0;
    std::size_t derived_sequences = 0;
    for (std::size_t index = 0; index < count; ++index) {
        const auto handle = static_cast<std::uint32_t>(sockets[index]);
        if (index > 0) {
            const auto previous =
                static_cast<std::uint32_t>(sockets[index - 1]);
            adjacent_handles +=
                handle - previous == 1U || previous - handle == 1U;
        }
        derived_sequences += static_cast<std::uint32_t>(sequences[index])
            == ((handle * 2'654'435'761U) & SequenceNumber::mask);
    }
    REQUIRE_EQ(adjacent_handles, std::size_t {0});
    REQUIRE_EQ(derived_sequences, std::size_t {0});
    auto sorted = sequences;
    std::sort(sorted.begin(), sorted.end());
    REQUIRE(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());
    for (const SRTSOCKET socket : sockets) {
        REQUIRE_EQ(srt_close(socket), 0);
    }

    std::vector<SRTSOCKET> groups;
    for (std::size_t index = 0; index < 8; ++index) {
        const SRTSOCKET group = srt_create_group(SRT_GTYPE_BROADCAST);
        REQUIRE(group != SRT_INVALID_SOCK);
        REQUIRE((group & SRTGROUP_MASK) != 0);
        if (!groups.empty()) {
            const SRTSOCKET step = group - groups.back();
            REQUIRE(step != 1 && step != -1);
        }
        groups.push_back(group);
    }
    for (const SRTSOCKET group : groups) {
        REQUIRE_EQ(srt_close(group), 0);
    }
    REQUIRE_EQ(srt_cleanup(), 0);
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
    REQUIRE_EQ(robotweax::srt::compat::existing_runtime_work_executor().get(),
        first_work.get());
    REQUIRE(first->snapshot().accepting);
    REQUIRE(first_work->snapshot().accepting);
    REQUIRE_EQ(first->snapshot().queue_capacity, 8'192U);
    REQUIRE_EQ(first->snapshot().timer_capacity, 8'192U);
    REQUIRE_EQ(first_work->snapshot().worker_count, 4U);
    REQUIRE_EQ(first_work->snapshot().queue_capacity, 1'024U);

    REQUIRE_EQ(srt_cleanup(), 0);
    REQUIRE(!first->snapshot().accepting);
    REQUIRE(!first_work->snapshot().accepting);
    REQUIRE(
        robotweax::srt::compat::existing_runtime_work_executor() == nullptr);

    REQUIRE_EQ(srt_startup(), 0);
    REQUIRE(
        robotweax::srt::compat::existing_runtime_work_executor() == nullptr);
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
    REQUIRE(
        robotweax::srt::compat::existing_runtime_work_executor() == nullptr);
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

TEST(lifecycle_final_cleanup_allows_awaited_callback_reentry)
{
    struct ReentryResults {
        int startup = 0;
        int startup_error = 0;
        SRTSOCKET socket = SRT_INVALID_SOCK;
        int socket_error = 0;
        SRTSOCKET group = SRT_INVALID_SOCK;
        int group_error = 0;
        int poll = SRT_ERROR;
        int poll_error = 0;
        int cleanup = SRT_ERROR;
    };

    for (int generation = 0; generation < 3; ++generation) {
        REQUIRE_EQ(srt_startup(), 0);
        const SRTSOCKET inherited = srt_create_socket();
        REQUIRE(inherited != SRT_INVALID_SOCK);
        const auto record = SocketRegistry::instance().find(inherited);
        REQUIRE(record != nullptr);

        std::promise<void> callback_started;
        auto started = callback_started.get_future();
        std::promise<void> allow_reentry;
        const auto released = allow_reentry.get_future().share();
        std::promise<ReentryResults> callback_finished;
        auto results = callback_finished.get_future();
        {
            std::lock_guard lock(record->mutex);
            record->connect_worker = std::thread([&callback_started, released,
                                                     &callback_finished] {
                robotweax::srt::compat::mark_runtime_cleanup_worker_thread();
                callback_started.set_value();
                released.wait();
                ReentryResults observed;
                observed.startup = srt_startup();
                observed.startup_error = srt_getlasterror(nullptr);
                observed.socket = srt_create_socket();
                observed.socket_error = srt_getlasterror(nullptr);
                observed.group = srt_create_group(SRT_GTYPE_BROADCAST);
                observed.group_error = srt_getlasterror(nullptr);
                observed.poll = srt_epoll_create();
                observed.poll_error = srt_getlasterror(nullptr);
                observed.cleanup = srt_cleanup();
                callback_finished.set_value(observed);
            });
        }
        started.wait();
        std::promise<void> cleanup_finished;
        auto cleaned = cleanup_finished.get_future();
        std::thread cleaner([&] {
            (void)srt_cleanup();
            cleanup_finished.set_value();
        });
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        bool closing = false;
        while (std::chrono::steady_clock::now() < deadline) {
            {
                std::lock_guard lock(record->mutex);
                closing = record->state == SRTS_CLOSING;
            }
            if (closing) {
                break;
            }
            std::this_thread::yield();
        }
        allow_reentry.set_value();
        const bool callback_returned =
            results.wait_for(2s) == std::future_status::ready;
        const bool cleanup_returned =
            cleaned.wait_for(2s) == std::future_status::ready;
        cleaner.join();
        REQUIRE(closing);
        REQUIRE(callback_returned);
        REQUIRE(cleanup_returned);
        const auto observed = results.get();
        REQUIRE_EQ(observed.startup, SRT_ERROR);
        REQUIRE_EQ(observed.startup_error, SRT_EINVOP);
        REQUIRE_EQ(observed.socket, SRT_INVALID_SOCK);
        REQUIRE_EQ(observed.socket_error, SRT_EINVOP);
        REQUIRE_EQ(observed.group, SRT_INVALID_SOCK);
        REQUIRE_EQ(observed.group_error, SRT_EINVOP);
        REQUIRE_EQ(observed.poll, SRT_ERROR);
        REQUIRE_EQ(observed.poll_error, SRT_EINVOP);
        REQUIRE_EQ(observed.cleanup, 0);

        const SRTSOCKET replacement = srt_create_socket();
        REQUIRE(replacement != SRT_INVALID_SOCK);
        REQUIRE(replacement != inherited);
        REQUIRE_EQ(srt_close(replacement), 0);
        REQUIRE_EQ(srt_cleanup(), 0);
    }
}

TEST(lifecycle_listener_callback_reentry_does_not_block_cleanup)
{
    REQUIRE_EQ(srt_startup(), 0);
    const SRTSOCKET listener = srt_create_socket();
    REQUIRE(listener != SRT_INVALID_SOCK);
    ListenerCleanupReentry probe;
    std::promise<void> release;
    probe.release = release.get_future().share();
    auto entered = probe.entered.get_future();
    auto finished = probe.finished.get_future();
    REQUIRE_EQ(
        srt_listen_callback(listener, reenter_creation_from_listener, &probe),
        0);
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = 0;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (srt_bind(listener, reinterpret_cast<const sockaddr*>(&address),
            static_cast<int>(sizeof(address)))
        == SRT_ERROR) {
        REQUIRE_EQ(srt_cleanup(), 0);
        return;
    }
    REQUIRE_EQ(srt_listen(listener, 1), 0);
    int address_size = static_cast<int>(sizeof(address));
    REQUIRE_EQ(srt_getsockname(listener, reinterpret_cast<sockaddr*>(&address),
                   &address_size),
        0);
    const SRTSOCKET caller = srt_create_socket();
    REQUIRE(caller != SRT_INVALID_SOCK);
    const bool asynchronous = false;
    REQUIRE_EQ(srt_setsockflag(caller, SRTO_RCVSYN, &asynchronous,
                   static_cast<int>(sizeof(asynchronous))),
        0);
    REQUIRE_EQ(srt_connect(caller, reinterpret_cast<const sockaddr*>(&address),
                   address_size),
        0);
    const bool callback_entered =
        entered.wait_for(5s) == std::future_status::ready;
    if (!callback_entered) {
        release.set_value();
        REQUIRE_EQ(srt_cleanup(), 0);
        REQUIRE(callback_entered);
        return;
    }

    const auto listener_record = SocketRegistry::instance().find(listener);
    REQUIRE(listener_record != nullptr);
    std::promise<void> cleanup_finished;
    auto cleaned = cleanup_finished.get_future();
    std::thread cleaner([&] {
        (void)srt_cleanup();
        cleanup_finished.set_value();
    });
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    bool closing = false;
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard lock(listener_record->mutex);
            closing = listener_record->state == SRTS_CLOSING;
        }
        if (closing) {
            break;
        }
        std::this_thread::yield();
    }
    release.set_value();
    const bool callback_returned =
        finished.wait_for(2s) == std::future_status::ready;
    const bool cleanup_returned =
        cleaned.wait_for(2s) == std::future_status::ready;
    cleaner.join();
    REQUIRE(closing);
    REQUIRE(callback_returned);
    REQUIRE(cleanup_returned);
    const auto [created, error] = finished.get();
    REQUIRE_EQ(created, SRT_INVALID_SOCK);
    REQUIRE_EQ(error, SRT_EINVOP);
    REQUIRE_EQ(srt_getsockstate(listener), SRTS_NONEXIST);
    REQUIRE_EQ(srt_getsockstate(caller), SRTS_NONEXIST);
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

TEST(sensor_profile_negotiates_and_transfers_loopback_datagrams)
{
    REQUIRE_EQ(srt_startup(), 0);
    constexpr char profile[] = "fec-sensor-v1,cols:4,rows:1,arq:never";
    constexpr std::int32_t timeout_milliseconds = 2'000;

    const SRTSOCKET listener = srt_create_socket();
    REQUIRE(listener != SRT_INVALID_SOCK);
    const SRT_TRANSTYPE sensor_type = SRTT_SENSOR;
    REQUIRE_EQ(srt_setsockflag(listener, SRTO_TRANSTYPE, &sensor_type,
                   static_cast<int>(sizeof(sensor_type))),
        0);
    REQUIRE_EQ(srt_setsockflag(listener, SRTO_RCVTIMEO, &timeout_milliseconds,
                   static_cast<int>(sizeof(timeout_milliseconds))),
        0);

    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = 0;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (srt_bind(listener, reinterpret_cast<const sockaddr*>(&address),
            static_cast<int>(sizeof(address)))
        == SRT_ERROR) {
        int system_error = 0;
        REQUIRE_EQ(srt_getlasterror(&system_error), SRT_ESOCKFAIL);
        REQUIRE(system_error != 0);
        REQUIRE_EQ(srt_close(listener), 0);
        REQUIRE_EQ(srt_cleanup(), 0);
        return;
    }
    REQUIRE_EQ(srt_listen(listener, 1), 0);

    sockaddr_in listener_name {};
    int listener_name_size = static_cast<int>(sizeof(listener_name));
    REQUIRE_EQ(
        srt_getsockname(listener, reinterpret_cast<sockaddr*>(&listener_name),
            &listener_name_size),
        0);

    const SRTSOCKET caller = srt_create_socket();
    REQUIRE(caller != SRT_INVALID_SOCK);
    REQUIRE_EQ(srt_setsockflag(caller, SRTO_PACKETFILTER, profile,
                   static_cast<int>(sizeof(profile) - 1U)),
        0);
    REQUIRE_EQ(srt_setsockflag(caller, SRTO_CONNTIMEO, &timeout_milliseconds,
                   static_cast<int>(sizeof(timeout_milliseconds))),
        0);
    REQUIRE_EQ(
        srt_connect(caller, reinterpret_cast<const sockaddr*>(&listener_name),
            static_cast<int>(sizeof(listener_name))),
        0);
    const SRTSOCKET accepted = srt_accept(listener, nullptr, nullptr);
    REQUIRE(accepted != SRT_INVALID_SOCK);

    for (const SRTSOCKET endpoint : {listener, caller, accepted}) {
        SRT_TRANSTYPE actual_type = SRTT_INVALID;
        int type_size = static_cast<int>(sizeof(actual_type));
        REQUIRE_EQ(
            srt_getsockflag(endpoint, SRTO_TRANSTYPE, &actual_type, &type_size),
            0);
        REQUIRE_EQ(actual_type, SRTT_SENSOR);
    }

    std::array<char, 96> negotiated {};
    int negotiated_size = static_cast<int>(negotiated.size());
    REQUIRE_EQ(srt_getsockflag(caller, SRTO_PACKETFILTER, negotiated.data(),
                   &negotiated_size),
        0);
    REQUIRE((std::string_view {
                 negotiated.data(), static_cast<std::size_t>(negotiated_size)}
        == profile));

    bool tsbpd = true;
    int option_size = static_cast<int>(sizeof(tsbpd));
    REQUIRE_EQ(
        srt_getsockflag(caller, SRTO_TSBPDMODE, &tsbpd, &option_size), 0);
    REQUIRE(!tsbpd);

    constexpr std::array<std::string_view, 4> messages {
        "imu:1", "imu:2", "imu:3", "imu:4"};
    for (const auto message : messages) {
        REQUIRE_EQ(srt_sendmsg(caller, message.data(),
                       static_cast<int>(message.size()), -1, 1),
            static_cast<int>(message.size()));
    }
    for (const auto expected : messages) {
        std::array<char, 64> received {};
        const int size = srt_recvmsg(
            accepted, received.data(), static_cast<int>(received.size()));
        REQUIRE_EQ(size, static_cast<int>(expected.size()));
        REQUIRE(
            (std::string_view {received.data(), static_cast<std::size_t>(size)}
                == expected));
    }

    REQUIRE_EQ(srt_close(accepted), 0);
    REQUIRE_EQ(srt_close(caller), 0);
    REQUIRE_EQ(srt_close(listener), 0);
    REQUIRE_EQ(srt_cleanup(), 0);
}

TEST(control_profile_negotiates_and_transfers_ordered_loopback_commands)
{
    REQUIRE_EQ(srt_startup(), 0);
    const SRT_TRANSTYPE control_type = SRTT_CONTROL;
    constexpr std::int32_t timeout_milliseconds = 3'000;
    const SRTSOCKET listener = srt_create_socket();
    REQUIRE(listener != SRT_INVALID_SOCK);
    REQUIRE_EQ(srt_setsockflag(listener, SRTO_TRANSTYPE, &control_type,
                   static_cast<int>(sizeof(control_type))),
        0);
    REQUIRE_EQ(srt_setsockflag(listener, SRTO_RCVTIMEO, &timeout_milliseconds,
                   static_cast<int>(sizeof(timeout_milliseconds))),
        0);
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = 0;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (srt_bind(listener, reinterpret_cast<const sockaddr*>(&address),
            static_cast<int>(sizeof(address)))
        == SRT_ERROR) {
        int system_error = 0;
        REQUIRE_EQ(srt_getlasterror(&system_error), SRT_ESOCKFAIL);
        REQUIRE(system_error != 0);
        REQUIRE_EQ(srt_close(listener), 0);
        REQUIRE_EQ(srt_cleanup(), 0);
        return;
    }
    REQUIRE_EQ(srt_listen(listener, 1), 0);
    sockaddr_in listener_name {};
    int listener_name_size = static_cast<int>(sizeof(listener_name));
    REQUIRE_EQ(
        srt_getsockname(listener, reinterpret_cast<sockaddr*>(&listener_name),
            &listener_name_size),
        0);

    const SRTSOCKET caller = srt_create_socket();
    REQUIRE(caller != SRT_INVALID_SOCK);
    REQUIRE_EQ(srt_setsockflag(caller, SRTO_TRANSTYPE, &control_type,
                   static_cast<int>(sizeof(control_type))),
        0);
    REQUIRE_EQ(srt_setsockflag(caller, SRTO_CONNTIMEO, &timeout_milliseconds,
                   static_cast<int>(sizeof(timeout_milliseconds))),
        0);
    REQUIRE_EQ(
        srt_connect(caller, reinterpret_cast<const sockaddr*>(&listener_name),
            static_cast<int>(sizeof(listener_name))),
        0);
    const SRTSOCKET accepted = srt_accept(listener, nullptr, nullptr);
    REQUIRE(accepted != SRT_INVALID_SOCK);
    for (const SRTSOCKET endpoint : {caller, accepted}) {
        SRT_TRANSTYPE actual_type = SRTT_INVALID;
        int size = static_cast<int>(sizeof(actual_type));
        REQUIRE_EQ(
            srt_getsockflag(endpoint, SRTO_TRANSTYPE, &actual_type, &size), 0);
        REQUIRE_EQ(actual_type, SRTT_CONTROL);
    }

    SRT_MSGCTRL finite_lifetime = srt_msgctrl_default;
    finite_lifetime.msgttl = 25;
    constexpr char first[] = "arm";
    REQUIRE_EQ(srt_sendmsg2(caller, first, static_cast<int>(sizeof(first) - 1U),
                   &finite_lifetime),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVALMSGAPI);

    for (const std::string_view command : {"arm", "move", "stop"}) {
        REQUIRE_EQ(srt_sendmsg(caller, command.data(),
                       static_cast<int>(command.size()), -1, 0),
            static_cast<int>(command.size()));
    }
    for (const std::string_view expected : {"arm", "move", "stop"}) {
        std::array<char, 32> received {};
        const int size = srt_recvmsg(
            accepted, received.data(), static_cast<int>(received.size()));
        REQUIRE_EQ(size, static_cast<int>(expected.size()));
        REQUIRE(
            (std::string_view {received.data(), static_cast<std::size_t>(size)}
                == expected));
    }
    REQUIRE_EQ(srt_close(accepted), 0);
    REQUIRE_EQ(srt_close(caller), 0);
    REQUIRE_EQ(srt_close(listener), 0);
    REQUIRE_EQ(srt_cleanup(), 0);
}

TEST(sensor_profile_crosses_a_real_udp_fault_relay_without_arq)
{
    REQUIRE_EQ(srt_startup(), 0);
    constexpr char profile[] = "fec-sensor-v1,cols:4,rows:1,arq:never";
    constexpr std::int32_t timeout_milliseconds = 2'000;

    const SRTSOCKET listener = srt_create_socket();
    REQUIRE(listener != SRT_INVALID_SOCK);
    REQUIRE_EQ(srt_setsockflag(listener, SRTO_PACKETFILTER, profile,
                   static_cast<int>(sizeof(profile) - 1U)),
        0);
    REQUIRE_EQ(srt_setsockflag(listener, SRTO_RCVTIMEO, &timeout_milliseconds,
                   static_cast<int>(sizeof(timeout_milliseconds))),
        0);
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = 0;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE_EQ(srt_bind(listener, reinterpret_cast<const sockaddr*>(&address),
                   static_cast<int>(sizeof(address))),
        0);
    REQUIRE_EQ(srt_listen(listener, 1), 0);
    sockaddr_in listener_name {};
    int listener_name_size = static_cast<int>(sizeof(listener_name));
    REQUIRE_EQ(
        srt_getsockname(listener, reinterpret_cast<sockaddr*>(&listener_name),
            &listener_name_size),
        0);
    const IpEndpoint listener_endpoint =
        IpEndpoint::loopback(ntohs(listener_name.sin_port));

    UdpSocket relay;
    REQUIRE(relay.valid());
    REQUIRE_EQ(relay.bind(IpEndpoint::loopback()), Error::none);
    const auto relay_endpoint = relay.local_endpoint();
    REQUIRE(relay_endpoint);
    std::atomic_size_t source_packets = 0U;
    std::atomic_size_t parity_packets = 0U;
    std::atomic_size_t duplicate_sources = 0U;
    std::atomic_size_t relay_errors = 0U;
    std::jthread relay_thread([&](std::stop_token stop) {
        std::array<std::byte, 65'536> datagram {};
        std::array<std::byte, 65'536> held_source {};
        std::array<std::uint32_t, 8> observed_sequences {};
        std::size_t observed_sequence_count = 0U;
        std::size_t held_size = 0U;
        IpEndpoint caller_endpoint {};
        bool caller_known = false;
        const auto forward = [&](std::span<const std::byte> bytes,
                                 IpEndpoint destination) {
            const auto sent = relay.send_to(bytes, destination);
            if (!sent || sent.bytes_transferred != bytes.size()) {
                ++relay_errors;
            }
        };
        while (!stop.stop_requested()) {
            const auto ready = relay.wait_readable(10);
            if (!ready) {
                ++relay_errors;
                break;
            }
            if (!ready.ready) {
                continue;
            }
            const auto received = relay.receive_from(datagram);
            if (!received) {
                ++relay_errors;
                continue;
            }
            const auto bytes = std::span<const std::byte> {
                datagram.data(), received.bytes_transferred};
            if (received.peer == listener_endpoint) {
                if (caller_known) {
                    forward(bytes, caller_endpoint);
                } else {
                    ++relay_errors;
                }
                continue;
            }

            caller_endpoint = received.peer;
            caller_known = true;
            const auto decoded = decode_packet(bytes);
            if (!decoded || decoded.packet.kind != PacketKind::data) {
                forward(bytes, listener_endpoint);
                continue;
            }
            if (decoded.packet.data.message_number == 0U) {
                const std::size_t parity = ++parity_packets;
                if (parity != 2U) {
                    forward(bytes, listener_endpoint);
                }
                continue;
            }

            const std::uint32_t sequence = decoded.packet.data.sequence.value();
            for (std::size_t index = 0U; index < observed_sequence_count;
                ++index) {
                if (observed_sequences[index] == sequence) {
                    ++duplicate_sources;
                }
            }
            if (observed_sequence_count < observed_sequences.size()) {
                observed_sequences[observed_sequence_count++] = sequence;
            }
            const std::size_t source = ++source_packets;
            if (source == 2U || source == 5U) {
                continue;
            }
            if (source == 7U) {
                std::copy(bytes.begin(), bytes.end(), held_source.begin());
                held_size = bytes.size();
                continue;
            }
            forward(bytes, listener_endpoint);
            if (source == 8U && held_size != 0U) {
                forward(
                    std::span<const std::byte> {held_source.data(), held_size},
                    listener_endpoint);
                held_size = 0U;
            }
        }
    });

    const SRTSOCKET caller = srt_create_socket();
    REQUIRE(caller != SRT_INVALID_SOCK);
    REQUIRE_EQ(srt_setsockflag(caller, SRTO_PACKETFILTER, profile,
                   static_cast<int>(sizeof(profile) - 1U)),
        0);
    REQUIRE_EQ(srt_setsockflag(caller, SRTO_CONNTIMEO, &timeout_milliseconds,
                   static_cast<int>(sizeof(timeout_milliseconds))),
        0);
    sockaddr_in relay_name {};
    relay_name.sin_family = AF_INET;
    relay_name.sin_port = htons(relay_endpoint.endpoint.port);
    relay_name.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE_EQ(
        srt_connect(caller, reinterpret_cast<const sockaddr*>(&relay_name),
            static_cast<int>(sizeof(relay_name))),
        0);
    const SRTSOCKET accepted = srt_accept(listener, nullptr, nullptr);
    REQUIRE(accepted != SRT_INVALID_SOCK);

    const auto send_sample = [&](char value) {
        REQUIRE_EQ(srt_sendmsg(caller, &value, 1, -1, 1), 1);
    };
    std::array<bool, 8> delivered {};
    const auto receive_sample = [&] {
        char value = 0;
        REQUIRE_EQ(srt_recvmsg(accepted, &value, 1), 1);
        REQUIRE(value >= 'a');
        REQUIRE(value <= 'h');
        const std::size_t index = static_cast<std::size_t>(value - 'a');
        REQUIRE(!delivered[index]);
        delivered[index] = true;
    };

    for (char value = 'a'; value <= 'd'; ++value) {
        send_sample(value);
    }
    for (std::size_t index = 0U; index < 4U; ++index) {
        receive_sample();
    }
    for (std::size_t index = 0U; index < 4U; ++index) {
        REQUIRE(delivered[index]);
    }

    const auto second_row_start = std::chrono::steady_clock::now();
    for (char value = 'e'; value <= 'h'; ++value) {
        send_sample(value);
    }
    for (std::size_t index = 0U; index < 3U; ++index) {
        receive_sample();
    }
    const auto second_row_elapsed =
        std::chrono::steady_clock::now() - second_row_start;
    REQUIRE(second_row_elapsed < 500ms);
    REQUIRE(!delivered[4]);
    REQUIRE(delivered[5]);
    REQUIRE(delivered[6]);
    REQUIRE(delivered[7]);

    SRT_TRACEBSTATS sender_statistics {};
    SRT_TRACEBSTATS receiver_statistics {};
    const auto acknowledgement_deadline = std::chrono::steady_clock::now() + 1s;
    do {
        REQUIRE_EQ(srt_bstats(caller, &sender_statistics, 0), 0);
        if (sender_statistics.pktSndBuf == 0) {
            break;
        }
        std::this_thread::sleep_for(2ms);
    } while (std::chrono::steady_clock::now() < acknowledgement_deadline);
    REQUIRE_EQ(sender_statistics.pktSndBuf, 0);
    REQUIRE_EQ(sender_statistics.pktRetransTotal, 0);
    REQUIRE_EQ(sender_statistics.pktRecvNAKTotal, 0);
    REQUIRE_EQ(srt_bstats(accepted, &receiver_statistics, 0), 0);
    REQUIRE(receiver_statistics.pktRcvFilterSupplyTotal >= 1);
    REQUIRE(receiver_statistics.pktRcvDropTotal >= 1);

    REQUIRE_EQ(source_packets.load(std::memory_order_acquire), 8U);
    REQUIRE_EQ(parity_packets.load(std::memory_order_acquire), 2U);
    REQUIRE_EQ(duplicate_sources.load(std::memory_order_acquire), 0U);
    REQUIRE_EQ(relay_errors.load(std::memory_order_acquire), 0U);

    REQUIRE_EQ(srt_close(accepted), 0);
    REQUIRE_EQ(srt_close(caller), 0);
    REQUIRE_EQ(srt_close(listener), 0);
    relay_thread.request_stop();
    relay_thread.join();
    REQUIRE_EQ(srt_cleanup(), 0);
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

namespace {
using namespace robotweax::srt::compat;
struct CleanupCycle {
    bool active = true;
    ~CleanupCycle()
    {
        if (active) {
            (void)srt_cleanup();
        }
    }
    void finish()
    {
        REQUIRE_EQ(srt_cleanup(), 0);
        active = false;
    }
};
std::pair<SRTSOCKET, std::shared_ptr<DatagramChannel>> bound_cleanup_channel(
    std::uint16_t port = 0)
{
    const auto socket = srt_create_socket();
    REQUIRE(socket != SRT_INVALID_SOCK);
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    REQUIRE_EQ(srt_bind(socket, reinterpret_cast<sockaddr*>(&address),
                   sizeof(address)),
        0);
    auto record = SocketRegistry::instance().find(socket);
    REQUIRE(record != nullptr);
    std::lock_guard lock(record->mutex);
    return {socket, record->channel};
}
struct CleanupGate {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool released = false;
    static void block(void* pointer) noexcept
    {
        auto& self = *static_cast<CleanupGate*>(pointer);
        std::unique_lock lock(self.mutex);
        self.entered = true;
        self.changed.notify_all();
        self.changed.wait(lock, [&] {
            return self.released;
        });
    }
    void wait()
    {
        std::unique_lock lock(mutex);
        REQUIRE(changed.wait_for(lock, 2s, [&] {
            return entered;
        }));
    }
    void release() noexcept
    {
        std::lock_guard lock(mutex);
        released = true;
        changed.notify_all();
    }
};
struct CleanupGateRelease {
    std::shared_ptr<CleanupGate> gate;
    std::shared_ptr<RuntimeScheduler> scheduler;
    ~CleanupGateRelease()
    {
        gate->release();
        if (scheduler != nullptr) {
            scheduler->stop();
        }
    }
};
} // namespace

TEST(lifecycle_final_cleanup_retires_captured_open_and_closed_bound_channels)
{
    for (bool closed : {false, true}) {
        REQUIRE_EQ(srt_startup(), 0);
        CleanupCycle cycle;
        auto [socket, channel] = bound_cleanup_channel();
        REQUIRE(channel->socket.valid());
        auto scheduler = acquire_runtime_scheduler();
        REQUIRE(scheduler != nullptr);
        auto readiness = scheduler->acquire_socket_readiness();
        REQUIRE(readiness != nullptr);
        REQUIRE(channel->start(scheduler, 0));
        REQUIRE_EQ(readiness->snapshot().registered, 1U);
        if (closed) {
            REQUIRE_EQ(srt_close(socket), 0);
            REQUIRE(channel->socket.valid());
        }
        cycle.finish();
        REQUIRE(!channel->socket.valid());
        REQUIRE(!channel->running());
        REQUIRE(!scheduler->snapshot().accepting);
        REQUIRE_EQ(readiness->snapshot().registered, 0U);
        REQUIRE_EQ(
            channel->shutdown(), DatagramChannel::ShutdownStatus::retired);
        REQUIRE_EQ(srt_getsockstate(socket), SRTS_NONEXIST);
    }
}

TEST(lifecycle_bound_channel_shared_close_and_fresh_generation_are_distinct)
{
    REQUIRE_EQ(srt_startup(), 0);
    CleanupCycle first_cycle;
    auto [first, old_channel] = bound_cleanup_channel();
    const auto endpoint = old_channel->socket.local_endpoint();
    REQUIRE(endpoint);
    auto [second, shared_channel] =
        bound_cleanup_channel(endpoint.endpoint.port);
    REQUIRE_EQ(shared_channel.get(), old_channel.get());
    REQUIRE_EQ(srt_close(first), 0);
    REQUIRE(old_channel->socket.valid());
    REQUIRE_EQ(srt_getsockstate(second), SRTS_OPENED);
    first_cycle.finish();
    REQUIRE(!old_channel->socket.valid());
    REQUIRE_EQ(srt_startup(), 0);
    CleanupCycle second_cycle;
    auto [fresh, fresh_channel] = bound_cleanup_channel(endpoint.endpoint.port);
    REQUIRE(fresh != first && fresh != second);
    REQUIRE(fresh_channel.get() != old_channel.get());
    REQUIRE(fresh_channel->socket.valid());
    REQUIRE_EQ(srt_close(first), 0);
    REQUIRE(fresh_channel->socket.valid());
    second_cycle.finish();
    REQUIRE(!fresh_channel->socket.valid());
}

TEST(lifecycle_final_cleanup_retires_captured_acquired_udp_channel)
{
    REQUIRE_EQ(srt_startup(), 0);
    CleanupCycle cycle;
    UdpSocket native;
    REQUIRE_EQ(native.bind(IpEndpoint::loopback()), Error::none);
    const auto socket = srt_create_socket();
    REQUIRE(socket != SRT_INVALID_SOCK);
    REQUIRE_EQ(srt_bind_acquire(
                   socket, static_cast<UDPSOCKET>(native.release_native())),
        0);
    auto record = SocketRegistry::instance().find(socket);
    REQUIRE(record != nullptr);
    auto channel = record->channel;
    REQUIRE(channel != nullptr);
    cycle.finish();
    REQUIRE(!channel->socket.valid());
}

TEST(
    lifecycle_bound_channel_worker_rejection_retains_batch_until_off_worker_retry)
{
    REQUIRE_EQ(srt_startup(), 0);
    CleanupCycle cycle;
    auto [socket, channel] = bound_cleanup_channel();
    std::weak_ptr<DatagramChannel> captured = channel;
    prepare_bound_channel_retirement();
    REQUIRE_EQ(srt_close(socket), 0);
    channel.reset();
    REQUIRE(!captured.expired());
    auto scheduler = std::make_shared<RuntimeScheduler>(
        RuntimeScheduler::Configuration {.shard_count = 1,
            .queue_capacity_per_shard = 2,
            .timer_capacity_per_shard = 2});
    REQUIRE(scheduler->start());
    struct Probe {
        std::promise<BoundChannelRetirementStatus> result;
        static void run(void* pointer) noexcept
        {
            static_cast<Probe*>(pointer)->result.set_value(
                finish_bound_channel_retirement(
                    std::chrono::steady_clock::time_point::max()));
        }
    };
    auto probe = std::make_shared<Probe>();
    auto result = probe->result.get_future();
    REQUIRE_EQ(scheduler->submit(0, {.function = Probe::run, .context = probe}),
        RuntimeScheduler::SubmitStatus::accepted);
    REQUIRE_EQ(result.wait_for(2s), std::future_status::ready);
    REQUIRE_EQ(result.get(), BoundChannelRetirementStatus::worker_thread);
    REQUIRE(!captured.expired());
    REQUIRE_EQ(
        finish_bound_channel_retirement(std::chrono::steady_clock::now() + 2s),
        BoundChannelRetirementStatus::retired);
    REQUIRE(captured.expired());
    scheduler->stop();
    cycle.finish();
}

TEST(lifecycle_bound_channel_retirement_serializes_reentry_and_new_generation)
{
    REQUIRE_EQ(srt_startup(), 0);
    CleanupCycle cycle;
    auto [socket, channel] = bound_cleanup_channel();
    struct Probe {
        std::shared_ptr<CleanupGate> gate = std::make_shared<CleanupGate>();
        BoundChannelRetirementStatus reentered =
            BoundChannelRetirementStatus::retired;
        static void ready(void* pointer) noexcept
        {
            auto& self = *static_cast<Probe*>(pointer);
            self.reentered = finish_bound_channel_retirement(
                std::chrono::steady_clock::time_point::max());
            CleanupGate::block(self.gate.get());
        }
    };
    auto probe = std::make_shared<Probe>();
    auto inbox = std::make_shared<DatagramInbox>(1);
    REQUIRE(inbox->set_ready_handler(Probe::ready, probe));
    REQUIRE(
        channel->register_setup_inbox(701, IpEndpoint::loopback(9000), inbox));
    prepare_bound_channel_retirement();
    std::future<BoundChannelRetirementStatus> finishing;
    CleanupGateRelease release {probe->gate};
    finishing = std::async(std::launch::async, [] {
        return finish_bound_channel_retirement(
            std::chrono::steady_clock::now() + 2s);
    });
    probe->gate->wait();
    REQUIRE_EQ(
        finish_bound_channel_retirement(std::chrono::steady_clock::now()),
        BoundChannelRetirementStatus::busy);
    auto [fresh, new_channel] = bound_cleanup_channel();
    REQUIRE(new_channel.get() != channel.get());
    prepare_bound_channel_retirement();
    probe->gate->release();
    REQUIRE_EQ(finishing.wait_for(2s), std::future_status::ready);
    REQUIRE_EQ(finishing.get(), BoundChannelRetirementStatus::retired);
    REQUIRE_EQ(probe->reentered, BoundChannelRetirementStatus::busy);
    REQUIRE(!channel->socket.valid());
    REQUIRE(!new_channel->socket.valid());
    cycle.finish();
}

TEST(lifecycle_bound_channel_task_timeout_retains_ownership_for_retry)
{
    REQUIRE_EQ(srt_startup(), 0);
    CleanupCycle cycle;
    auto [socket, channel] = bound_cleanup_channel();
    auto scheduler = std::make_shared<RuntimeScheduler>(
        RuntimeScheduler::Configuration {.shard_count = 1,
            .queue_capacity_per_shard = 4,
            .timer_capacity_per_shard = 4});
    REQUIRE(scheduler->start());
    struct Clock {
        std::atomic_bool armed {false};
        std::shared_ptr<CleanupGate> gate = std::make_shared<CleanupGate>();
        static std::uint64_t now(void* pointer) noexcept
        {
            auto& self = *static_cast<Clock*>(pointer);
            if (self.armed.exchange(false)) {
                CleanupGate::block(self.gate.get());
            }
            return 1000;
        }
    } clock;
    SocketOptions options;
    REQUIRE_EQ(options.set(SocketOption::tsbpd_mode, 0), Error::none);
    auto runtime = std::make_shared<ConnectionRuntime>(
        ConnectionRuntime::Configuration {.channel = channel,
            .peer = IpEndpoint::loopback(9000),
            .peer_socket_id = 90,
            .initial_sequence = SequenceNumber {1000},
            .options = options,
            .origin = ConnectionRuntime::Clock::now(),
            .now_function = Clock::now,
            .now_context = &clock});
    REQUIRE(channel->register_connection(700, runtime));
    CleanupGateRelease release {clock.gate, scheduler};
    clock.armed.store(true);
    REQUIRE(channel->start(scheduler, 0));
    clock.gate->wait();
    prepare_bound_channel_retirement();
    REQUIRE_EQ(
        finish_bound_channel_retirement(std::chrono::steady_clock::now()),
        BoundChannelRetirementStatus::timeout);
    REQUIRE(channel->socket.valid());
    REQUIRE(!channel->running());
    clock.gate->release();
    REQUIRE_EQ(
        finish_bound_channel_retirement(std::chrono::steady_clock::now() + 2s),
        BoundChannelRetirementStatus::retired);
    REQUIRE(!channel->socket.valid());
    REQUIRE(!runtime->accepts_datagrams());
    scheduler->stop();
    cycle.finish();
}

TEST(lifecycle_nested_cleanup_preserves_bound_channel_until_final_reference)
{
    REQUIRE_EQ(srt_startup(), 0);
    CleanupCycle cycle;
    auto [socket, channel] = bound_cleanup_channel();
    REQUIRE_EQ(srt_startup(), 0);
    REQUIRE_EQ(srt_cleanup(), 0);
    REQUIRE(channel->socket.valid());
    cycle.finish();
    REQUIRE(!channel->socket.valid());
}
