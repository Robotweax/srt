#pragma once

#include "srt/srt.h"

#include <chrono>
#include <atomic>
#include <cstdint>
#include <optional>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <vector>

namespace robotweax::srt::compat {

class ConnectionRuntime;
class ReadinessWatch;
class ReadinessObserver;

// A source outlives its runtime only while an epoll watch still references it;
// it never owns the runtime or its packet buffers.
class ReadinessSource {
    friend class ReadinessWatch;
    friend class ReadinessSignal;

public:
    struct LowEpochs {
        std::uint64_t in = 0;
        std::uint64_t out = 0;
        std::uint64_t err = 0;
    };

    // Producers record a low level before publishing the next change. Each
    // epoll subscription compares these epochs independently after a rescan.
    void note_not_ready(int events) noexcept;
    [[nodiscard]] LowEpochs low_epochs() const noexcept;
    [[nodiscard]] bool has_observers() const noexcept
    {
        return observed_.load(std::memory_order_acquire);
    }

private:
    std::mutex mutex_;
    ReadinessWatch* first_ = nullptr;
    std::atomic_bool observed_ = false;
    std::atomic<std::uint64_t> low_in_ {0};
    std::atomic<std::uint64_t> low_out_ {0};
    std::atomic<std::uint64_t> low_err_ {0};
};

class ReadinessObserver {
public:
    ReadinessObserver();
    ~ReadinessObserver();
    ReadinessObserver(const ReadinessObserver&) = delete;
    ReadinessObserver& operator=(const ReadinessObserver&) = delete;
    [[nodiscard]] std::uint64_t generation() noexcept;
    void invalidate_all() noexcept;
    void wake() noexcept;
    void wait_until(std::uint64_t generation,
        std::chrono::steady_clock::time_point deadline) noexcept;
    // Caller reserves one handle per subscription before taking the batch.
    [[nodiscard]] bool take_changes(std::vector<SRTSOCKET>& handles) noexcept;

private:
    friend class ReadinessWatch;
    friend class ReadinessSignal;
    friend struct ReadinessHub;
    void invalidate(ReadinessWatch& watch) noexcept;
    void remove_dirty(ReadinessWatch& watch) noexcept;
    void change_wildcards(int delta) noexcept;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::uint64_t generation_ = 1;
    bool all_dirty_ = true;
    ReadinessWatch* dirty_ = nullptr;
    ReadinessObserver* previous_ = nullptr;
    ReadinessObserver* next_ = nullptr;
    ReadinessObserver* wildcard_previous_ = nullptr;
    ReadinessObserver* wildcard_next_ = nullptr;
    std::size_t wildcards_ = 0;
};

class ReadinessWatch {
public:
    ReadinessWatch(
        ReadinessObserver& observer, SRTSOCKET handle, bool wildcard);
    ~ReadinessWatch();
    ReadinessWatch(const ReadinessWatch&) = delete;
    ReadinessWatch& operator=(const ReadinessWatch&) = delete;
    [[nodiscard]] bool bind(std::shared_ptr<ReadinessSource> source) noexcept;

private:
    friend class ReadinessObserver;
    friend class ReadinessSignal;
    ReadinessObserver& observer_;
    SRTSOCKET handle_;
    bool wildcard_;
    std::shared_ptr<ReadinessSource> source_;
    ReadinessWatch* source_previous_ = nullptr;
    ReadinessWatch* source_next_ = nullptr;
    ReadinessWatch* dirty_previous_ = nullptr;
    ReadinessWatch* dirty_next_ = nullptr;
    bool queued_ = false;
};

struct SocketReadinessSnapshot {
    int events = 0;
    std::uint64_t update_version = 0;
    std::optional<std::chrono::steady_clock::time_point> read_wakeup;
    bool exists = false;
    std::shared_ptr<ReadinessSource> source;
};

[[nodiscard]] SocketReadinessSnapshot socket_readiness(
    SRTSOCKET socket) noexcept;

// The socket flag is a snapshot; the runtime may become terminal afterwards.
// Kept separate from registry lookup so both snapshot orderings are testable.
[[nodiscard]] SocketReadinessSnapshot connection_readiness(
    ConnectionRuntime& runtime, bool socket_broken) noexcept;

class ReadinessSignal {
public:
    using Clock = std::chrono::steady_clock;

    [[nodiscard]] static std::uint64_t generation() noexcept;
    // Invalidates every observer: for state transitions that can change the
    // readiness of handles no source describes (connect, accept, group
    // membership). Costs a rescan on every epoll set in the process.
    static void notify() noexcept;
    // Invalidates only the watches bound to `source` (and, if given, to
    // `secondary`, e.g. the group a member belongs to).
    static void notify(
        ReadinessSource& source, ReadinessSource* secondary = nullptr) noexcept;
    // Wakes blocked legacy waiters (accept/connect loops) without touching
    // any observer: for events that change no handle's readiness.
    static void notify_waiters() noexcept;
    static void wait_until(
        std::uint64_t observed_generation,
        Clock::time_point deadline) noexcept;

private:
    static void invalidate_watches(ReadinessSource& source) noexcept;
};

} // namespace robotweax::srt::compat
