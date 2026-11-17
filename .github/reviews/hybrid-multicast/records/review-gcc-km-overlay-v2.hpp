// SPDX-License-Identifier: MIT
// Scratch-only diagnostic overlay, not installed or production code.
#pragma once
#include <array>
#include <algorithm>
#include <span>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
namespace robotweax::srt::km_diagnostic {
inline constexpr unsigned capacity = 16384;
struct Event {
    const char* name = nullptr;
    std::uint64_t us = 0, ctx = 0, a = 0, b = 0, c = 0, d = 0, e = 0, f = 0;
    std::atomic<bool> ready = false;
};
inline std::array<Event, capacity> events;
inline std::array<std::atomic<std::uintptr_t>, 128> contexts {};
inline std::atomic<unsigned> next = 0;
inline std::atomic<bool> active = true;
// Exact equality maps synthetic fixture keys to opaque IDs. These bounded
// transient copies are never emitted and are wiped at dump; no key digest is
// logged. This diagnostic-only retention is not a production lifetime contract.
struct Key {
    std::array<std::byte, 32> bytes {};
    std::size_t length = 0;
};
inline std::array<Key, 128> keys {};
inline std::atomic_flag key_lock = ATOMIC_FLAG_INIT;
inline std::atomic<unsigned> key_overflow = 0;
inline bool enabled() noexcept;
inline std::uint64_t key_id(std::span<const std::byte> key) noexcept
{
    if (!enabled() || !active.load() || key.size() > 32)
        return 0;
    while (key_lock.test_and_set(std::memory_order_acquire)) {
    }
    std::uint64_t id = 0;
    if (active.load())
        for (unsigned i = 0; i < keys.size(); ++i) {
            auto& slot = keys[i];
            if (slot.length == key.size()
                && std::equal(key.begin(), key.end(), slot.bytes.begin())) {
                id = i + 1;
                break;
            }
            if (!slot.length) {
                std::copy(key.begin(), key.end(), slot.bytes.begin());
                slot.length = key.size();
                id = i + 1;
                break;
            }
        }
    if (!id && active.load())
        ++key_overflow;
    key_lock.clear(std::memory_order_release);
    return id;
}
inline std::uint64_t context_id(const void* p) noexcept
{
    auto value = reinterpret_cast<std::uintptr_t>(p);
    for (unsigned i = 0; i < contexts.size(); ++i) {
        auto old = contexts[i].load();
        if (old == value)
            return i + 1;
        if (old == 0 && contexts[i].compare_exchange_strong(old, value))
            return i + 1;
        if (old == value)
            return i + 1;
    }
    return 0;
}
inline void dump() noexcept
{
    active = false;
    auto count = next.load();
    unsigned written = 0;
    for (unsigned i = 0; i < count && i < capacity; ++i) {
        const auto& e = events[i];
        if (!e.ready.load(std::memory_order_acquire))
            continue;
        std::fprintf(stderr,
            "[KM_TRACE] %u %s %llu %llu %llu %llu %llu %llu %llu %llu\n", i,
            e.name, static_cast<unsigned long long>(e.us),
            static_cast<unsigned long long>(e.ctx),
            static_cast<unsigned long long>(e.a),
            static_cast<unsigned long long>(e.b),
            static_cast<unsigned long long>(e.c),
            static_cast<unsigned long long>(e.d),
            static_cast<unsigned long long>(e.e),
            static_cast<unsigned long long>(e.f));
        ++written;
    }
    std::fprintf(stderr, "[KM_TRACE_END] offered=%u written=%u capacity=%u\n",
        count, written, capacity);
    std::fprintf(stderr, "[KM_KEY_IDS] overflow=%u\n", key_overflow.load());
    while (key_lock.test_and_set(std::memory_order_acquire)) {
    }
    for (auto& key : keys) {
        volatile std::byte* bytes = key.bytes.data();
        for (unsigned i = 0; i < key.bytes.size(); ++i)
            bytes[i] = std::byte {0};
        key.length = 0;
    }
    key_lock.clear(std::memory_order_release);
}
inline bool enabled() noexcept
{
    static const bool value = []() {
        const char* s = std::getenv("ROBOTWEAX_TEST_KM_TRACE");
        const bool on = s && s[0] == '1' && s[1] == '\0';
        if (on)
            std::atexit(dump);
        return on;
    }();
    return value;
}
inline void emit(const char* name, const void* context, std::uint64_t a = 0,
    std::uint64_t b = 0, std::uint64_t c = 0, std::uint64_t d = 0,
    std::uint64_t e = 0, std::uint64_t f = 0) noexcept
{
    if (!enabled() || !active.load())
        return;
    const auto i = next.fetch_add(1);
    if (i >= capacity)
        return;
    auto& event = events[i];
    event.name = name;
    event.ctx = context_id(context);
    event.us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    event.a = a;
    event.b = b;
    event.c = c;
    event.d = d;
    event.e = e;
    event.f = f;
    event.ready.store(true, std::memory_order_release);
}
inline std::uint64_t identity(std::span<const std::byte> bytes) noexcept
{
    std::uint64_t n = 0;
    if (bytes.size() != 1316)
        return UINT64_MAX;
    for (unsigned i = 0; i < 8; ++i)
        n |= std::to_integer<std::uint64_t>(bytes[i]) << (8 * i);
    return n;
}
} // namespace robotweax::srt::km_diagnostic
