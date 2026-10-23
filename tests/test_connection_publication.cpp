#include "test.hpp"
#include "compat/transport_runtime.hpp"
#include "compat/socket_registry.hpp"
#include "srt/srt.h"

#include <array>
#include <chrono>
#include <string_view>
#include <thread>

// TSan owns the global new/delete interceptors. Keep the independent allocation
// probe available in ordinary/ASan builds without replacing those interceptors.
#if defined(__SANITIZE_THREAD__)
#define ROBOTWEAX_SRT_ALLOCATION_PROBE_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define ROBOTWEAX_SRT_ALLOCATION_PROBE_TSAN 1
#endif
#endif

#if defined(ROBOTWEAX_SRT_ALLOCATION_PROBE_TSAN)
TEST(connection_publication_allocation_probe_requires_non_tsan)
{
    SKIP_UNLESS(
        false, "global allocation probe requires a separate non-TSan build");
}
#else
// Allocation interception belongs only to this isolated test executable.
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>
#ifdef _WIN32
#include <malloc.h>
#endif

namespace robotweax::srt::test {
std::atomic<unsigned> runtime_allocations_until_pause {0};
std::atomic_bool runtime_allocation_paused {false};
std::atomic_bool runtime_allocation_released {false};
void pause_runtime_allocation(std::size_t bytes) noexcept
{
    // Match the runtime object plus bounded shared-ownership bookkeeping,
    // without depending on a particular standard library's control-block size.
    if (bytes < sizeof(compat::ConnectionRuntime)
        || bytes > sizeof(compat::ConnectionRuntime) + 256)
        return;
    auto remaining = runtime_allocations_until_pause.load();
    while (remaining != 0
        && !runtime_allocations_until_pause.compare_exchange_weak(
            remaining, remaining - 1)) {
    }
    if (remaining != 1)
        return;
    runtime_allocation_paused = true;
    while (!runtime_allocation_released.load())
        runtime_allocation_released.wait(false);
}
}

void* operator new(std::size_t size)
{
    if (auto* memory = std::malloc(size == 0 ? 1 : size)) {
        robotweax::srt::test::pause_runtime_allocation(size);
        return memory;
    }
    throw std::bad_alloc {};
}
void* operator new[](std::size_t size)
{
    return ::operator new(size);
}
void operator delete(void* memory) noexcept
{
    std::free(memory);
}
void operator delete[](void* memory) noexcept
{
    std::free(memory);
}
void operator delete(void* memory, std::size_t) noexcept
{
    std::free(memory);
}
void operator delete[](void* memory, std::size_t) noexcept
{
    std::free(memory);
}

void* operator new(std::size_t size, std::align_val_t alignment)
{
    void* memory = nullptr;
#ifdef _WIN32
    memory = _aligned_malloc(
        size == 0 ? 1 : size, static_cast<std::size_t>(alignment));
    if (memory == nullptr)
        throw std::bad_alloc {};
#else
    if (posix_memalign(
            &memory, static_cast<std::size_t>(alignment), size == 0 ? 1 : size)
        != 0)
        throw std::bad_alloc {};
#endif
    robotweax::srt::test::pause_runtime_allocation(size);
    return memory;
}
void* operator new[](std::size_t size, std::align_val_t alignment)
{
    return ::operator new(size, alignment);
}
void operator delete(void* memory, std::align_val_t) noexcept
{
#ifdef _WIN32
    _aligned_free(memory);
#else
    std::free(memory);
#endif
}
void operator delete[](void* memory, std::align_val_t alignment) noexcept
{
    ::operator delete(memory, alignment);
}
void operator delete(
    void* memory, std::size_t, std::align_val_t alignment) noexcept
{
    ::operator delete(memory, alignment);
}
void operator delete[](
    void* memory, std::size_t, std::align_val_t alignment) noexcept
{
    ::operator delete(memory, alignment);
}

// Runtime buffers also use nothrow allocation. Route every standard form
// through the same allocator so ASan sees matching allocation/deallocation.
void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
    try {
        return ::operator new(size);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
    try {
        return ::operator new[](size);
    } catch (...) {
        return nullptr;
    }
}
void operator delete(void* memory, const std::nothrow_t&) noexcept
{
    ::operator delete(memory);
}
void operator delete[](void* memory, const std::nothrow_t&) noexcept
{
    ::operator delete[](memory);
}
void* operator new(std::size_t size, std::align_val_t alignment,
    const std::nothrow_t&) noexcept
{
    try {
        return ::operator new(size, alignment);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t size, std::align_val_t alignment,
    const std::nothrow_t&) noexcept
{
    try {
        return ::operator new[](size, alignment);
    } catch (...) {
        return nullptr;
    }
}
void operator delete(
    void* memory, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
    ::operator delete(memory, alignment);
}
void operator delete[](
    void* memory, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
    ::operator delete[](memory, alignment);
}

namespace {
using namespace robotweax::srt;
using namespace robotweax::srt::compat;
using namespace std::chrono_literals;
struct Sockets {
    SRTSOCKET listener = srt_create_socket();
    SRTSOCKET caller = srt_create_socket();
    SRTSOCKET accepted = SRT_INVALID_SOCK;
    ~Sockets()
    {
        srt_close(accepted);
        srt_close(caller);
        srt_close(listener);
        srt_cleanup();
    }
};
struct ReleaseAllocation {
    ~ReleaseAllocation()
    {
        release();
    }
    void release()
    {
        test::runtime_allocations_until_pause = 0;
        test::runtime_allocation_released = true;
        test::runtime_allocation_released.notify_all();
    }
};
void publication_waits_for_runtime(unsigned allocation)
{
    REQUIRE_EQ(srt_startup(), 0);
    // Callback context outlives socket cleanup, including assertion failure.
    std::atomic<SRTSOCKET> incoming {SRT_INVALID_SOCK};
    Sockets sockets;
    // Release the paused allocation before socket cleanup on failure.
    ReleaseAllocation release;
    REQUIRE_EQ(srt_listen_callback(
                   sockets.listener,
                   [](void* context, SRTSOCKET socket, int, const sockaddr*,
                       const char*) -> int {
                       static_cast<std::atomic<SRTSOCKET>*>(context)->store(
                           socket);
                       return 0;
                   },
                   &incoming),
        0);
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE_EQ(srt_bind(sockets.listener, reinterpret_cast<sockaddr*>(&address),
                   sizeof(address)),
        0);
    int size = sizeof(address);
    REQUIRE_EQ(srt_getsockname(sockets.listener,
                   reinterpret_cast<sockaddr*>(&address), &size),
        0);
    REQUIRE_EQ(srt_listen(sockets.listener, 1), 0);
    const bool synchronous = false;
    REQUIRE_EQ(srt_setsockflag(sockets.caller, SRTO_RCVSYN, &synchronous,
                   sizeof(synchronous)),
        0);
    const int timeout = 2000;
    REQUIRE_EQ(srt_setsockflag(
                   sockets.caller, SRTO_RCVTIMEO, &timeout, sizeof(timeout)),
        0);
    REQUIRE_EQ(srt_setsockflag(
                   sockets.listener, SRTO_RCVTIMEO, &timeout, sizeof(timeout)),
        0);
    test::runtime_allocation_released = false;
    test::runtime_allocation_paused = false;
    test::runtime_allocations_until_pause = allocation;
    REQUIRE_EQ(srt_connect(sockets.caller,
                   reinterpret_cast<sockaddr*>(&address), sizeof(address)),
        0);
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!test::runtime_allocation_paused
        && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    REQUIRE(test::runtime_allocation_paused.load());
    const auto accepted_id = incoming.load();
    REQUIRE(accepted_id != SRT_INVALID_SOCK);
    const auto paused_id = allocation == 1 ? accepted_id : sockets.caller;
    const auto paused = SocketRegistry::instance().find(paused_id);
    REQUIRE(paused);
    {
        std::lock_guard lock(paused->mutex);
        // Verify that this allocation gate hit the intended setup boundary.
        REQUIRE(paused->runtime == nullptr);
        REQUIRE(paused->peer_protocol_socket_id != 0);
    }
    REQUIRE_EQ(srt_getsockstate(paused_id), SRTS_CONNECTING);
    if (allocation == 2) {
        const auto listener_peer = SocketRegistry::instance().find(accepted_id);
        REQUIRE(listener_peer);
        std::lock_guard lock(listener_peer->mutex);
        REQUIRE(listener_peer->runtime != nullptr);
    }
    release.release();
    const auto connected_deadline = std::chrono::steady_clock::now() + 2s;
    while (srt_getsockstate(sockets.caller) == SRTS_CONNECTING
        && std::chrono::steady_clock::now() < connected_deadline)
        std::this_thread::yield();
    REQUIRE_EQ(srt_getsockstate(sockets.caller), SRTS_CONNECTED);
    const auto caller = SocketRegistry::instance().find(sockets.caller);
    REQUIRE(caller);
    {
        std::lock_guard lock(caller->mutex);
        REQUIRE(caller->runtime != nullptr);
    }
    sockets.accepted = srt_accept(sockets.listener, nullptr, nullptr);
    REQUIRE_EQ(sockets.accepted, accepted_id);
    REQUIRE_EQ(srt_sendmsg(sockets.caller, "ready", 5, -1, 1), 5);
    std::array<char, SRT_LIVE_MAX_PLSIZE> received {};
    REQUIRE_EQ(
        srt_recvmsg(sockets.accepted, received.data(), received.size()), 5);
    REQUIRE_EQ(std::string_view(received.data(), 5), "ready");
}
}
TEST(connection_listener_stays_connecting_until_runtime_is_attached)
{
    publication_waits_for_runtime(1);
}
TEST(connection_async_caller_stays_connecting_until_runtime_is_attached)
{
    publication_waits_for_runtime(2);
}
#endif
