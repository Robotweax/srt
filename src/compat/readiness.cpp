#include "compat/readiness.hpp"

#include "compat/socket_registry.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <limits>
#include <memory>
#include <mutex>

namespace robotweax::srt::compat {
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
    return signal_state().generation.load();
}

void ReadinessSignal::notify() noexcept
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
