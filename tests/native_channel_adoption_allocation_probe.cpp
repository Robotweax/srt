#include "compat/transport_runtime.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>

#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define SRT_ADOPTION_TSAN 1
#endif
#endif
#if defined(__SANITIZE_THREAD__)
#define SRT_ADOPTION_TSAN 1
#endif

#if defined(SRT_ADOPTION_TSAN)
int main()
{
    std::puts("Allocation interception skipped under TSan; ordinary adoption "
              "tests still run.");
    return 0;
}
#else
namespace {
std::atomic<int> fail_after {-1};
std::atomic<int> allocation_count {-1};
}
void* operator new(std::size_t size)
{
    if (allocation_count.load(std::memory_order_relaxed) >= 0)
        allocation_count.fetch_add(1, std::memory_order_relaxed);
    if (fail_after.load(std::memory_order_relaxed) >= 0
        && fail_after.fetch_sub(1, std::memory_order_relaxed) == 0) {
        fail_after.store(-1, std::memory_order_relaxed);
        throw std::bad_alloc {};
    }
    if (auto* result = std::malloc(size == 0 ? 1 : size))
        return result;
    throw std::bad_alloc {};
}
void* operator new[](std::size_t size)
{
    return ::operator new(size);
}
void operator delete(void* pointer) noexcept
{
    std::free(pointer);
}
void operator delete[](void* pointer) noexcept
{
    std::free(pointer);
}
void operator delete(void* pointer, std::size_t) noexcept
{
    std::free(pointer);
}
void operator delete[](void* pointer, std::size_t) noexcept
{
    std::free(pointer);
}

int main()
{
    using namespace robotweax::srt;
    using namespace robotweax::srt::compat;
    auto budget = std::make_shared<NativeChannelBudget>(1);
    UdpSocket socket {IpAddressFamily::ipv4};
    if (!socket.valid() || socket.bind(IpEndpoint::loopback()) != Error::none)
        return 1;
    // Calibrate a successful factory call: empty containers may allocate in
    // their constructors (notably with MSVC). Fail every allocation, including
    // those subobjects, rather than assuming allocation #1 is the control block.
    allocation_count.store(0, std::memory_order_relaxed);
    auto calibration = DatagramChannel::adopt_budgeted(socket, budget);
    const int allocations =
        allocation_count.exchange(-1, std::memory_order_relaxed);
    if (calibration == nullptr || allocations < 2 || socket.valid())
        return 6;
    socket = std::move(calibration->socket);
    calibration.reset();
    if (!socket.valid() || budget->reserved_channels() != 0U)
        return 7;
    const auto identity = socket.native_handle();
    for (int allocation = 0; allocation < allocations; ++allocation) {
        fail_after.store(allocation, std::memory_order_relaxed);
        auto rejected = DatagramChannel::adopt_budgeted(socket, budget);
        const auto pending_failure =
            fail_after.exchange(-1, std::memory_order_relaxed);
        if (pending_failure != -1 || rejected != nullptr || !socket.valid()
            || socket.native_handle() != identity || !socket.local_endpoint()
            || budget->reserved_channels() != 0U)
            return 2;
    }
    auto accepted = DatagramChannel::adopt_budgeted(socket, budget);
    if (accepted == nullptr || socket.valid()
        || accepted->socket.native_handle() != identity
        || budget->reserved_channels() != 1U)
        return 4;
    if (accepted->shutdown() != DatagramChannel::ShutdownStatus::retired
        || accepted->socket.valid() || budget->reserved_channels() != 0U)
        return 5;
    std::printf("All %d factory allocations preserve native ownership and "
                "refund credits on failure; retry passes.\n",
        allocations);
    return 0;
}
#endif
