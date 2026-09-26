#include "compat/readiness.hpp"

#include "compat/socket_registry.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <limits>
#include <memory>
#include <mutex>

namespace robotweax::srt::compat {
// Lock order: hub/source -> observer. No observer callback takes a socket,
// runtime or poll-record lock, and state queries never hold an observer lock.
struct ReadinessHub {
    std::mutex mutex;
    ReadinessObserver* observers = nullptr;
    ReadinessObserver* wildcards = nullptr;
    std::atomic_bool has_wildcards = false;
    static ReadinessHub& instance()
    {
        static ReadinessHub hub;
        return hub;
    }
    void notify(bool all) noexcept
    {
        if (!all && !has_wildcards.load(std::memory_order_acquire))
            return;
        std::lock_guard lock(mutex);
        for (auto* observer = all ? observers : wildcards; observer != nullptr;
            observer = all ? observer->next_ : observer->wildcard_next_) {
            observer->invalidate_all();
        }
    }
};

ReadinessObserver::ReadinessObserver()
{
    auto& hub = ReadinessHub::instance();
    std::lock_guard lock(hub.mutex);
    next_ = hub.observers;
    if (next_ != nullptr)
        next_->previous_ = this;
    hub.observers = this;
}
ReadinessObserver::~ReadinessObserver()
{
    auto& hub = ReadinessHub::instance();
    std::lock_guard lock(hub.mutex);
    if (previous_ != nullptr)
        previous_->next_ = next_;
    else
        hub.observers = next_;
    if (next_ != nullptr)
        next_->previous_ = previous_;
}
void ReadinessObserver::change_wildcards(int delta) noexcept
{
    auto& hub = ReadinessHub::instance();
    std::lock_guard lock(hub.mutex);
    if (delta > 0 && wildcards_++ == 0U) {
        wildcard_next_ = hub.wildcards;
        if (wildcard_next_ != nullptr)
            wildcard_next_->wildcard_previous_ = this;
        hub.wildcards = this;
        hub.has_wildcards.store(true, std::memory_order_release);
    } else if (delta < 0 && --wildcards_ == 0U) {
        if (wildcard_previous_ != nullptr)
            wildcard_previous_->wildcard_next_ = wildcard_next_;
        else
            hub.wildcards = wildcard_next_;
        if (wildcard_next_ != nullptr)
            wildcard_next_->wildcard_previous_ = wildcard_previous_;
        wildcard_next_ = wildcard_previous_ = nullptr;
        hub.has_wildcards.store(
            hub.wildcards != nullptr, std::memory_order_release);
    }
}
std::uint64_t ReadinessObserver::generation() noexcept
{
    std::lock_guard lock(mutex_);
    return generation_;
}
void ReadinessObserver::wake() noexcept
{
    std::lock_guard lock(mutex_);
    if (++generation_ == 0U)
        ++generation_;
    changed_.notify_all();
}
void ReadinessObserver::invalidate_all() noexcept
{
    std::lock_guard lock(mutex_);
    if (!all_dirty_) {
        all_dirty_ = true;
        if (++generation_ == 0U)
            ++generation_;
        changed_.notify_all();
    }
}
void ReadinessObserver::invalidate(ReadinessWatch& watch) noexcept
{
    std::lock_guard lock(mutex_);
    if (watch.queued_)
        return;
    watch.queued_ = true;
    watch.dirty_next_ = dirty_;
    if (dirty_ != nullptr)
        dirty_->dirty_previous_ = &watch;
    dirty_ = &watch;
    if (++generation_ == 0U)
        ++generation_;
    changed_.notify_all();
}
void ReadinessObserver::remove_dirty(ReadinessWatch& watch) noexcept
{
    if (!watch.queued_)
        return;
    if (watch.dirty_previous_ != nullptr)
        watch.dirty_previous_->dirty_next_ = watch.dirty_next_;
    else
        dirty_ = watch.dirty_next_;
    if (watch.dirty_next_ != nullptr)
        watch.dirty_next_->dirty_previous_ = watch.dirty_previous_;
    watch.dirty_next_ = watch.dirty_previous_ = nullptr;
    watch.queued_ = false;
}
bool ReadinessObserver::take_changes(std::vector<SRTSOCKET>& handles) noexcept
{
    std::lock_guard lock(mutex_);
    handles.clear();
    const bool all = all_dirty_;
    all_dirty_ = false;
    while (dirty_ != nullptr) {
        if (!all)
            handles.push_back(dirty_->handle_);
        remove_dirty(*dirty_);
    }
    return all;
}
void ReadinessObserver::wait_until(std::uint64_t observed,
    std::chrono::steady_clock::time_point deadline) noexcept
{
    std::unique_lock lock(mutex_);
    (void)changed_.wait_until(lock, deadline, [&] {
        return generation_ != observed;
    });
}
ReadinessWatch::ReadinessWatch(
    ReadinessObserver& observer, SRTSOCKET handle, bool wildcard)
    : observer_(observer)
    , handle_(handle)
    , wildcard_(wildcard)
{
    if (wildcard_)
        observer_.change_wildcards(1);
    observer_.invalidate(*this);
}
ReadinessWatch::~ReadinessWatch()
{
    (void)bind({});
    if (wildcard_)
        observer_.change_wildcards(-1);
    std::lock_guard lock(observer_.mutex_);
    observer_.remove_dirty(*this);
}
bool ReadinessWatch::bind(std::shared_ptr<ReadinessSource> source) noexcept
{
    if (source == source_)
        return false;
    if (source_ != nullptr) {
        std::lock_guard lock(source_->mutex_);
        if (source_previous_ != nullptr)
            source_previous_->source_next_ = source_next_;
        else
            source_->first_ = source_next_;
        if (source_next_ != nullptr)
            source_next_->source_previous_ = source_previous_;
        source_->observed_.store(
            source_->first_ != nullptr, std::memory_order_release);
    }
    source_ = std::move(source);
    source_previous_ = source_next_ = nullptr;
    if (source_ != nullptr) {
        std::lock_guard lock(source_->mutex_);
        source_next_ = source_->first_;
        if (source_next_ != nullptr)
            source_next_->source_previous_ = this;
        source_->first_ = this;
        source_->observed_.store(true, std::memory_order_release);
    }
    return true;
}

namespace {

struct SignalState {
    std::mutex mutex;
    std::condition_variable changed;
    std::atomic<std::uint64_t> generation {1};
    std::atomic<std::size_t> waiters {0};
};

[[nodiscard]] SignalState& signal_state()
{
    static SignalState state;
    return state;
}

} // namespace

SocketReadinessSnapshot socket_readiness(SRTSOCKET handle) noexcept
{
    const auto socket = SocketRegistry::instance().find(handle);
    if (socket == nullptr) {
        const bool closed =
            SocketRegistry::instance().state(handle) == SRTS_CLOSED;
        SocketReadinessSnapshot readiness {};
        readiness.events = closed ? SRT_EPOLL_ERR : 0;
        readiness.exists = closed;
        return readiness;
    }
    // Synchronize runtime terminal state into the socket and group snapshots.
    // Readiness queries must not depend on a prior srt_getsockstate call.
    (void)SocketRegistry::instance().state(handle);

    SocketReadinessSnapshot readiness{};
    readiness.exists = true;
    std::shared_ptr<ConnectionRuntime> runtime;
    SRT_SOCKSTATUS state = SRTS_NONEXIST;
    int connect_error = SRT_SUCCESS;
    std::size_t pending_accepts = 0;
    {
        std::lock_guard lock(socket->mutex);
        state = socket->state;
        runtime = socket->runtime;
        connect_error = socket->connect_error;
        pending_accepts =
            socket->pending_accepts.load(std::memory_order_acquire);
        readiness.update_version = socket->group_update_version;
    }

    if (state == SRTS_LISTENING) {
        readiness.events = pending_accepts != 0U ? SRT_EPOLL_IN : 0;
        return readiness;
    }
    if (state == SRTS_CONNECTING) {
        readiness.events = connect_error == SRT_SUCCESS
            ? 0 : SRT_EPOLL_ERR;
        return readiness;
    }
    if ((state == SRTS_CONNECTED || state == SRTS_BROKEN)
        && runtime != nullptr) {
        auto connected = connection_readiness(*runtime, state == SRTS_BROKEN);
        connected.update_version = readiness.update_version;
        connected.source = runtime->readiness_source();
        return connected;
    }
    if (state == SRTS_BROKEN) {
        readiness.events = SRT_EPOLL_IN | SRT_EPOLL_OUT | SRT_EPOLL_ERR;
    } else if (state == SRTS_CLOSING || state == SRTS_CLOSED
        || state == SRTS_NONEXIST) {
        readiness.events = SRT_EPOLL_ERR;
    }
    return readiness;
}

SocketReadinessSnapshot connection_readiness(
    ConnectionRuntime& runtime, bool socket_broken) noexcept
{
    SocketReadinessSnapshot readiness {};
    readiness.exists = true;
    // Include terminal transitions after the registry snapshot in the same
    // drain path. Checking runtime.broken() later and returning ERR directly
    // can discard the final buffered messages in nonblocking consumers.
    const bool broken = socket_broken || runtime.broken();
    if (broken && runtime.peer_closed()) {
        const bool readable = runtime.readable();
        const auto read_wakeup = runtime.next_readable_deadline();
        if (read_wakeup.has_value()) {
            if (readable) {
                // A peer SHUTDOWN must not surface EPOLL_ERR ahead of data
                // already accepted into the receive buffer. FFmpeg treats
                // ERR as terminal and otherwise skips the final recvmsg.
                readiness.events = SRT_EPOLL_IN;
            } else {
                readiness.read_wakeup = read_wakeup;
            }
            return readiness;
        }
        readiness.events = SRT_EPOLL_IN | SRT_EPOLL_OUT | SRT_EPOLL_ERR;
        return readiness;
    }
    if (broken) {
        readiness.events =
            SRT_EPOLL_IN | SRT_EPOLL_OUT | SRT_EPOLL_ERR;
        return readiness;
    }

    if (runtime.readable()) {
        readiness.events |= SRT_EPOLL_IN;
    } else {
        readiness.read_wakeup = runtime.next_readable_deadline();
    }
    if (runtime.writable()) {
        readiness.events |= SRT_EPOLL_OUT;
    }
    return readiness;
}

std::uint64_t ReadinessSignal::generation() noexcept
{
    (void)ReadinessHub::instance();
    return signal_state().generation.load();
}

namespace {
void notify_legacy_waiters() noexcept
{
    auto& state = signal_state();
    std::uint64_t generation = state.generation.load();
    for (;;) {
        const std::uint64_t next =
            generation == std::numeric_limits<std::uint64_t>::max()
            ? 1U
            : generation + 1U;
        if (state.generation.compare_exchange_weak(generation, next)) {
            break;
        }
    }
    // Sequential consistency across generation and waiters ensures that a
    // notifier either observes a previously registered waiter or that a
    // subsequently registered waiter observes the published generation.
    if (state.waiters.load() == 0U) {
        return;
    }
    // A registered waiter holds this mutex until it either observes the new
    // generation or atomically enters the condition-variable wait. Acquiring
    // it after publishing the generation closes the notification race without
    // imposing the mutex on the common no-waiter transport path.
    {
        std::lock_guard lock(state.mutex);
    }
    state.changed.notify_all();
}

} // namespace

void ReadinessSignal::notify() noexcept
{
    ReadinessHub::instance().notify(true);
    notify_legacy_waiters();
}
void ReadinessSignal::notify(ReadinessSource& source) noexcept
{
    if (source.observed_.load(std::memory_order_acquire)) {
        std::lock_guard lock(source.mutex_);
        for (auto* watch = source.first_; watch != nullptr;
            watch = watch->source_next_) {
            watch->observer_.invalidate(*watch);
        }
    }
    // Group membership can change independently of an attached member runtime.
    // Keep group observers conservative until membership subscriptions exist.
    ReadinessHub::instance().notify(false);
    notify_legacy_waiters();
}

void ReadinessSignal::wait_until(
    std::uint64_t observed_generation,
    Clock::time_point deadline) noexcept
{
    auto& state = signal_state();
    std::unique_lock lock(state.mutex);
    state.waiters.fetch_add(1U);
    (void)state.changed.wait_until(
        lock, deadline, [&state, observed_generation] {
            return state.generation.load() != observed_generation;
        });
    state.waiters.fetch_sub(1U);
}

} // namespace robotweax::srt::compat
