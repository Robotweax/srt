#include "test.hpp"
#include "member_filter.hpp"
#include "xor_filter.hpp"

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
TEST(experimental_filter_allocation_probe_requires_non_tsan)
{
    SKIP_UNLESS(
        false, "global allocation probe requires a separate non-TSan build");
}
#else
// Allocation probe belongs only to the isolated filter executable.
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>
#ifdef _WIN32
#include <malloc.h>
#endif

namespace robotweax::srt::test {
std::atomic<std::size_t> filter_allocations {0};
std::atomic<std::size_t> filter_bytes {0};
std::array<void*, 8> state_pointers {};
int fail_after = -1;
unsigned state_attempts = 0;
unsigned live_states = 0;
void record(std::size_t size, void* memory) noexcept
{
    ++filter_allocations;
    filter_bytes += size;
    if (size == sizeof(extensions::xor8::State)) {
        for (auto& pointer : state_pointers)
            if (pointer == nullptr) {
                pointer = memory;
                ++live_states;
                return;
            }
        std::abort();
    }
}
void released(void* memory) noexcept
{
    if (memory == nullptr)
        return;
    for (auto& pointer : state_pointers)
        if (pointer == memory) {
            pointer = nullptr;
            --live_states;
            return;
        }
}
}

void* operator new(std::size_t size)
{
    if (size == sizeof(robotweax::srt::extensions::xor8::State)
        && robotweax::srt::test::fail_after >= 0
        && robotweax::srt::test::state_attempts++
            == static_cast<unsigned>(robotweax::srt::test::fail_after))
        throw std::bad_alloc {};
    if (auto* memory = std::malloc(size == 0 ? 1 : size)) {
        robotweax::srt::test::record(size, memory);
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
    robotweax::srt::test::released(memory);
    std::free(memory);
}
void operator delete[](void* memory) noexcept
{
    robotweax::srt::test::released(memory);
    std::free(memory);
}
void operator delete(void* memory, std::size_t) noexcept
{
    robotweax::srt::test::released(memory);
    std::free(memory);
}
void operator delete[](void* memory, std::size_t) noexcept
{
    robotweax::srt::test::released(memory);
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
    robotweax::srt::test::record(size, memory);
    return memory;
}
void* operator new[](std::size_t size, std::align_val_t alignment)
{
    return ::operator new(size, alignment);
}
void operator delete(void* memory, std::align_val_t) noexcept
{
    robotweax::srt::test::released(memory);
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

namespace {
using namespace robotweax::srt;
using namespace robotweax::srt::extensions;
unsigned constructed = 0;
unsigned destroyed = 0;
}
TEST(
    experimental_filter_large_state_allocation_failure_releases_partial_instances)
{
    for (int failure = 0; failure < 2; ++failure) {
        REQUIRE_EQ(test::live_states, 0U);
        constructed = destroyed = 0;
        auto filter = xor8_filter();
        filter.implementation.construct =
            [](std::span<std::byte> state, const FilterOwner& owner,
                FilterLimits limits, const FilterEnvironment& environment) {
                ++constructed;
                return xor8_filter().implementation.construct(
                    state, owner, limits, environment);
            };
        filter.implementation.destroy =
            [](std::span<std::byte> state) noexcept {
                ++destroyed;
                xor8_filter().implementation.destroy(state);
            };
        {
            MemberFilterRuntime runtime;
            test::state_attempts = 0;
            test::fail_after = failure;
            const auto result = runtime.start(filter, 11, 22,
                SequenceNumber {0}, SequenceNumber {0}, 32, 32, 1433);
            test::fail_after = -1;
            REQUIRE_EQ(result, Error::buffer_too_small);
            REQUIRE_EQ(runtime.poll_sender(SequenceNumber {0}, 1),
                Error::buffer_too_small);
            REQUIRE_EQ(constructed, static_cast<unsigned>(failure));
        }
        REQUIRE_EQ(destroyed, constructed);
        REQUIRE_EQ(test::live_states, 0U);
    }
}
TEST(
    experimental_xor8_events_allocate_no_memory_and_pair_setup_fits_one_mebibyte)
{
    const auto filter = xor8_filter();
    const auto before_bytes = test::filter_bytes.load();
    {
        auto sender = std::make_unique<MemberFilterRuntime>();
        auto receiver = std::make_unique<MemberFilterRuntime>();
        const SequenceNumber initial {0};
        REQUIRE_EQ(
            sender->start(filter, 11, 22, initial, initial, 32768, 32768, 1433),
            Error::none);
        REQUIRE_EQ(receiver->start(
                       filter, 22, 11, initial, initial, 32768, 32768, 1433),
            Error::none);
        REQUIRE(test::filter_bytes.load() - before_bytes <= 1024 * 1024);
        const auto before_events = test::filter_allocations.load();
        std::array<std::byte, 1433> payload {};
        unsigned recovered_count = 0;
        for (std::uint32_t i = 0; i < 1024; ++i) {
            const auto sequence = initial.advanced(i);
            PacketView source {.kind = PacketKind::data,
                .data = {.sequence = sequence,
                    .message_number = i + 1,
                    .boundary = MessageBoundary::solo,
                    .destination_socket_id = 22},
                .payload = std::span {payload}.first(i % 2 ? 1 : 1433)};
            REQUIRE_EQ(
                sender->source_sent(source, sequence, i * 3), Error::none);
            const auto first = initial.advanced(i & ~std::uint32_t {7});
            if (i % 8 != 0) {
                source.data.retransmitted = true;
                REQUIRE_EQ(
                    receiver->receive_source(source, first, i * 3 + 1).error,
                    Error::none);
            }
            if (const auto pending = sender->pending()) {
                const auto decoded = decode_packet(*pending);
                REQUIRE(decoded);
                const auto result =
                    receiver->receive_repair(decoded.packet, first, i * 3 + 2);
                REQUIRE_EQ(result.error, Error::none);
                REQUIRE_EQ(result.recovered.size(), std::size_t {1});
                REQUIRE_EQ(result.recovered[0].data.sequence, first);
                ++recovered_count;
                sender->consume();
            }
        }
        REQUIRE_EQ(recovered_count, 128U);
        REQUIRE_EQ(test::filter_allocations.load(), before_events);
    }
    REQUIRE_EQ(test::live_states, 0U);
}
#endif
