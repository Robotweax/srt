#include "test.hpp"
#include "compat/socket_readiness.hpp"
#include "robotweax/srt/udp.hpp"

#include <array>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

using namespace robotweax::srt;
using namespace robotweax::srt::compat;

namespace {
struct ReadyCount {
    std::mutex mutex;
    std::condition_variable changed;
    unsigned count = 0;
};
void note_ready(void* context) noexcept
{
    auto& count = *static_cast<ReadyCount*>(context);
    std::lock_guard lock(count.mutex);
    ++count.count;
    count.changed.notify_all();
}
void wait_count(const std::shared_ptr<ReadyCount>& count, unsigned expected)
{
    std::unique_lock lock(count->mutex);
    REQUIRE(count->changed.wait_for(lock, std::chrono::seconds {2}, [&] {
        return count->count >= expected;
    }));
}
}

TEST(socket_readiness_is_one_shot_and_rearms_level_readability)
{
    for (const auto family : {IpAddressFamily::ipv4, IpAddressFamily::ipv6}) {
        SocketReadiness watcher {2};
        REQUIRE(watcher.start());
        UdpSocket target {family};
        UdpSocket source {family};
        REQUIRE_EQ(target.bind(family == IpAddressFamily::ipv4
                           ? IpEndpoint::loopback()
                           : IpEndpoint::ipv6_loopback()),
            Error::none);
        const auto address = target.local_endpoint();
        REQUIRE(address);
        const auto count = std::make_shared<ReadyCount>();
        const auto token =
            watcher.watch(target.native_handle(), {note_ready, count});
        REQUIRE(token.valid());
        REQUIRE(watcher.arm(token));
        const std::array payload {std::byte {7}};
        REQUIRE(source.send_to(payload, address.endpoint));
        wait_count(count, 1);
        REQUIRE_EQ(watcher.snapshot().armed, 0U);
        REQUIRE_EQ(watcher.snapshot().notifications, 1U);
        // Still readable: rearming must report it without a new datagram edge.
        REQUIRE(watcher.arm(token));
        wait_count(count, 2);
        std::array<std::byte, 16> received {};
        REQUIRE(target.receive_from(received));
        REQUIRE(watcher.arm(token));
        REQUIRE(source.send_to(payload, address.endpoint));
        wait_count(count, 3);
        watcher.cancel(token);
        REQUIRE_EQ(watcher.snapshot().registered, 0U);
        watcher.stop();
        REQUIRE(!watcher.snapshot().running);
    }
}

TEST(socket_readiness_bounds_registration_and_rejects_stale_tokens)
{
    SocketReadiness watcher {1};
    UdpSocket socket;
    const auto count = std::make_shared<ReadyCount>();
    REQUIRE(
        !watcher.watch(socket.native_handle(), {note_ready, count}).valid());
    REQUIRE(watcher.start());
    const auto first =
        watcher.watch(socket.native_handle(), {note_ready, count});
    REQUIRE(first.valid());
    REQUIRE(
        !watcher.watch(socket.native_handle(), {note_ready, count}).valid());
    watcher.cancel(first);
    const auto second =
        watcher.watch(socket.native_handle(), {note_ready, count});
    REQUIRE(second.valid());
    REQUIRE_EQ(first.slot, second.slot);
    REQUIRE(first.generation != second.generation);
    REQUIRE(!watcher.arm(first));
    watcher.cancel(first);
    REQUIRE_EQ(watcher.snapshot().registered, 1U);
    REQUIRE(watcher.arm(second));
    watcher.cancel(second);
    watcher.stop();
    REQUIRE(!watcher.arm(second));
    REQUIRE(!watcher.start());
}

TEST(socket_readiness_can_release_its_last_owner_in_a_callback)
{
    struct Owner {
        std::shared_ptr<SocketReadiness> watcher;
        SocketReadiness::Token token;
        ReadyCount count;
    };
    UdpSocket target;
    UdpSocket source;
    REQUIRE_EQ(target.bind(IpEndpoint::loopback()), Error::none);
    const auto address = target.local_endpoint();
    REQUIRE(address);
    auto owner = std::make_shared<Owner>();
    owner->watcher = std::make_shared<SocketReadiness>(1);
    REQUIRE(owner->watcher->start());
    std::weak_ptr<SocketReadiness> weak = owner->watcher;
    const auto token = owner->watcher->watch(target.native_handle(),
        {[](void* context) noexcept {
             auto& state = *static_cast<Owner*>(context);
             state.watcher->cancel(state.token);
             state.watcher.reset();
             note_ready(&state.count);
         },
            owner});
    REQUIRE(token.valid());
    owner->token = token;
    REQUIRE(owner->watcher->arm(token));
    REQUIRE(source.send_to(std::array {std::byte {1}}, address.endpoint));
    std::unique_lock lock(owner->count.mutex);
    REQUIRE(owner->count.changed.wait_for(lock, std::chrono::seconds {2}, [&] {
        return owner->count.count == 1;
    }));
    REQUIRE(weak.expired());
}

TEST(socket_readiness_cancel_allows_immediate_native_port_rebind)
{
    for (const auto family : {IpAddressFamily::ipv4, IpAddressFamily::ipv6}) {
        SocketReadiness watcher {1};
        REQUIRE(watcher.start());
        const auto count = std::make_shared<ReadyCount>();
        for (unsigned iteration = 0; iteration < 128; ++iteration) {
            std::optional<UdpSocket> target {std::in_place, family};
            REQUIRE_EQ(target->bind(family == IpAddressFamily::ipv4
                               ? IpEndpoint::loopback()
                               : IpEndpoint::ipv6_loopback()),
                Error::none);
            const auto address = target->local_endpoint();
            REQUIRE(address);
            const auto token =
                watcher.watch(target->native_handle(), {note_ready, count});
            REQUIRE(token.valid());
            const auto previous_waits = watcher.snapshot().waits;
            REQUIRE(watcher.arm(token));
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds {2};
            while (watcher.snapshot().waits == previous_waits
                && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::yield();
            }
            REQUIRE(watcher.snapshot().waits > previous_waits);
            watcher.cancel(token);
            target.reset();
            UdpSocket replacement {family};
            REQUIRE_EQ(replacement.bind(address.endpoint), Error::none);
        }
    }
}

TEST(socket_readiness_concurrent_cancel_reclaims_a_slot_once)
{
    SocketReadiness watcher {1};
    REQUIRE(watcher.start());
    UdpSocket target;
    REQUIRE_EQ(target.bind(IpEndpoint::loopback()), Error::none);
    const auto count = std::make_shared<ReadyCount>();
    for (unsigned iteration = 0; iteration < 32; ++iteration) {
        const auto token =
            watcher.watch(target.native_handle(), {note_ready, count});
        REQUIRE(token.valid());
        REQUIRE(watcher.arm(token));
        std::barrier start {3};
        const auto cancel = [&] {
            start.arrive_and_wait();
            watcher.cancel(token);
        };
        std::thread first(cancel);
        std::thread second(cancel);
        start.arrive_and_wait();
        first.join();
        second.join();
        REQUIRE_EQ(watcher.snapshot().registered, 0U);
        const auto replacement =
            watcher.watch(target.native_handle(), {note_ready, count});
        REQUIRE(replacement.valid());
        REQUIRE(!watcher.watch(target.native_handle(), {note_ready, count})
                .valid());
        watcher.cancel(replacement);
    }
}
