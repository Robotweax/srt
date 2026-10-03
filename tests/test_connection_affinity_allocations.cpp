#include "test.hpp"
#include "compat/connection_affinity_model.hpp"

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
TEST(affinity_full_pool_operations_do_not_allocate_after_setup)
{
    SKIP_UNLESS(
        false, "global allocation probe requires a separate non-TSan build");
}
#else
// Allocation probe belongs only to the isolated ownership-model executable.
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>
#ifdef _WIN32
#include <malloc.h>
#endif

namespace robotweax::srt::test {
std::atomic<std::size_t> affinity_allocations {0};
}

void* operator new(std::size_t size)
{
    if (auto* memory = std::malloc(size == 0 ? 1 : size)) {
        ++robotweax::srt::test::affinity_allocations;
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
    ++robotweax::srt::test::affinity_allocations;
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

namespace {
using Model = robotweax::srt::compat::model::ConnectionAffinity;
using Credits = robotweax::srt::compat::model::ChannelCredits;
using Kind = Model::Kind;
using Admission = Model::Admission;
constexpr std::array payload {
    std::byte {0x53}, std::byte {0x52}, std::byte {0x54}};

template <class T> T present(std::optional<T> value)
{
    REQUIRE(value.has_value());
    return *value;
}
Model::Key connection(Model& model, std::uint32_t wire = 1)
{
    return present(model.admit(1, wire));
}
Model::Turn turn(Model& model, Model::Key key)
{
    auto result = present(model.begin_turn(model.snapshot(key).shard));
    REQUIRE_EQ(result.key, key);
    return result;
}
void drain(Model& model, Model::Key key, std::uint64_t now)
{
    const auto token = present(model.begin_drain(key));
    REQUIRE(!model.begin_drain(key));
    REQUIRE(model.finish_drain(token, now));
    REQUIRE(!model.finish_drain(token, now));
}
void consume(Model& model, Model::Turn owner, std::uint64_t now)
{
    while (auto event = model.next(owner, now)) {
        if (event->kind == Kind::command)
            REQUIRE(model.commit(owner, event->command, now, 42));
        if (event->kind == Kind::timer)
            REQUIRE(model.commit_timer(owner, event->timer, now));
    }
    REQUIRE(model.finish_turn(owner, now));
}
}

TEST(affinity_full_pool_operations_do_not_allocate_after_setup)
{
    auto before = robotweax::srt::test::affinity_allocations.load();
    Model model({});
    REQUIRE_EQ(robotweax::srt::test::affinity_allocations.load(), before + 1);
    before = robotweax::srt::test::affinity_allocations.load();
    std::array<Model::Key, Model::maximum_connections> keys;
    std::array<std::array<Model::Command, Model::command_capacity>,
        Model::maximum_connections>
        commands;
    std::array<Model::Timer, Model::maximum_connections> timers;
    Credits credits;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        keys[i] = connection(model, static_cast<std::uint32_t>(i + 1));
        for (auto& command : commands[i])
            command = present(model.publish_command(keys[i], payload, 1));
        for (std::size_t n = 0; n < Model::datagram_capacity; ++n)
            REQUIRE_EQ(model.publish(keys[i], Kind::control, payload, 1),
                Admission::accepted);
        REQUIRE(model.notify(keys[i], Kind::send, 1));
        REQUIRE(model.notify(keys[i], Kind::receive_release, 1));
        timers[i] = present(model.arm_timer(keys[i], 2));
        REQUIRE(model.publish_timer(timers[i], 2));
    }
    REQUIRE(!model.admit(1, 100));
    // Visit every slot with payload, command, timer and completion storage live.
    for (auto key : keys)
        consume(model, turn(model, key), 3);
    model.stop(4);
    for (std::size_t i = 0; i < keys.size(); ++i) {
        drain(model, keys[i], 5);
        for (auto command : commands[i])
            REQUIRE(model.take_result(command));
    }
    REQUIRE(model.restart());
    auto key = connection(model);
    auto lease = present(credits.grant(key.route));
    REQUIRE(credits.attempt(lease, 1500));
    REQUIRE(credits.release(lease));
    REQUIRE(credits.next_round());
    REQUIRE_EQ(robotweax::srt::test::affinity_allocations.load(), before);
}

#endif
