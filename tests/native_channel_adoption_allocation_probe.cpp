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
}
void* operator new(std::size_t size)
{
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
    const auto identity = socket.native_handle();
    for (const int allocation : {0, 1}) {
        // Fail object allocation, then shared control-block allocation.
        fail_after.store(allocation, std::memory_order_relaxed);
        auto rejected = DatagramChannel::adopt_budgeted(socket, budget);
        const auto pending_failure =
            fail_after.exchange(-1, std::memory_order_relaxed);
        if (pending_failure != -1 || rejected != nullptr || !socket.valid()
            || socket.native_handle() != identity || !socket.local_endpoint()
            || budget->reserved_channels() != 0U)
            return 2 + allocation;
    }
    auto accepted = DatagramChannel::adopt_budgeted(socket, budget);
    if (accepted == nullptr || socket.valid()
        || accepted->socket.native_handle() != identity
        || budget->reserved_channels() != 1U)
        return 4;
    if (accepted->shutdown() != DatagramChannel::ShutdownStatus::retired
        || accepted->socket.valid() || budget->reserved_channels() != 0U)
        return 5;
    std::puts("Object/control-block failures preserve native ownership and "
              "refund credits; retry passes.");
    return 0;
}
#endif
