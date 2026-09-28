#include "compat/socket_readiness.hpp"
#include "robotweax/srt/udp.hpp"

#include <array>
#include <cerrno>
#include <condition_variable>
#include <limits>
#include <utility>
#include <vector>

#if defined(_WIN32)
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#define ROBOTWEAX_READINESS_POLL 1
#elif defined(__linux__)
#include <sys/epoll.h>
#include <unistd.h>
#define ROBOTWEAX_READINESS_EPOLL 1
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__)        \
    || defined(__OpenBSD__) || defined(__DragonFly__)
#include <sys/event.h>
#include <sys/time.h>
#include <unistd.h>
#define ROBOTWEAX_READINESS_KQUEUE 1
#else
#include <poll.h>
#define ROBOTWEAX_READINESS_POLL 1
#endif

// Three backends share one contract: a watch is armed at most once per
// notification, protocol work never runs on the watcher thread, and a
// cancelled watch is never reported again.
//
// epoll/kqueue: the kernel keeps the interest set, so arming and cancelling
// are O(1) control calls from any thread and need no wake-up of the watcher.
// A one-shot interest delivers a single readiness event and disables itself
// until the next arm. poll/WSAPoll: the watcher rebuilds its descriptor array
// from the armed entries per wait, so every arm and cancel wakes it through
// a loopback datagram (Windows, and hosts without the other two).

namespace robotweax::srt::compat {
namespace {

#if defined(ROBOTWEAX_READINESS_POLL)
#if defined(_WIN32)
using Descriptor = WSAPOLLFD;
constexpr short read_events = POLLRDNORM;
#else
using Descriptor = pollfd;
constexpr short read_events = POLLIN;
#endif
Descriptor descriptor(std::uintptr_t socket) noexcept
{
    Descriptor result {};
    result.fd = static_cast<decltype(result.fd)>(socket);
    result.events = read_events;
    return result;
}
#endif

constexpr int wait_timeout_milliseconds = 100;
constexpr std::size_t maximum_events_per_wait = 64;
// Event payload: the watch slot in the low 32 bits and its generation in the
// high 32 bits, so a stale event for a reused slot is recognised.
constexpr std::uint64_t wake_marker =
    (std::numeric_limits<std::uint64_t>::max)();

[[nodiscard]] constexpr std::uint64_t pack_token(
    std::size_t slot, std::uint64_t generation) noexcept
{
    return (generation << 32U)
        | static_cast<std::uint64_t>(slot & 0xffff'ffffU);
}
} // namespace

struct SocketReadiness::State {
    struct Entry {
        std::uintptr_t socket = 0;
        Callback callback;
        std::uint64_t generation = 0;
        bool active = false;
        bool armed = false;
        bool retiring = false;
        // epoll/kqueue: the socket is known to the kernel queue.
        bool registered = false;
    };
    explicit State(std::size_t capacity)
        : entries(capacity)
    {
        free_slots.reserve(capacity);
        tokens.reserve(capacity);
        callbacks.reserve(capacity);
#if defined(ROBOTWEAX_READINESS_POLL)
        descriptors.reserve(capacity + 1U);
#endif
        for (std::size_t index = capacity; index != 0U; --index) {
            free_slots.push_back(index - 1U);
        }
    }
    ~State()
    {
#if defined(ROBOTWEAX_READINESS_EPOLL) || defined(ROBOTWEAX_READINESS_KQUEUE)
        if (queue >= 0) {
            (void)::close(queue);
        }
#endif
    }
    void wake() noexcept
    {
        std::lock_guard lock(wake_mutex);
        const std::array byte {std::byte {1}};
        (void)wake_socket.send_to(byte, wake_endpoint);
        // If the wake socket is full it is already readable. A 100 ms
        // watchdog also bounds shutdown if a local wake send fails otherwise.
    }
    void drain_wake() noexcept
    {
        std::lock_guard lock(wake_mutex);
        std::array<std::byte, 64> bytes {};
        for (std::size_t count = 0; count < 64; ++count) {
            if (!wake_socket.receive_from(bytes)) {
                break;
            }
        }
    }

#if defined(ROBOTWEAX_READINESS_EPOLL)
    [[nodiscard]] bool open_queue() noexcept
    {
        queue = ::epoll_create1(EPOLL_CLOEXEC);
        if (queue < 0) {
            return false;
        }
        epoll_event event {};
        event.events = EPOLLIN;
        event.data.u64 = wake_marker;
        return ::epoll_ctl(queue, EPOLL_CTL_ADD,
                   static_cast<int>(wake_socket.native_handle()), &event)
            == 0;
    }
    // Arms one readiness delivery; the interest disables itself afterwards.
    [[nodiscard]] bool queue_arm(Entry& entry, std::size_t slot) noexcept
    {
        epoll_event event {};
        event.events = EPOLLIN | EPOLLONESHOT;
        event.data.u64 = pack_token(slot, entry.generation);
        const int operation = entry.registered ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
        if (::epoll_ctl(
                queue, operation, static_cast<int>(entry.socket), &event)
            != 0) {
            return false;
        }
        entry.registered = true;
        return true;
    }
    void queue_remove(Entry& entry) noexcept
    {
        if (entry.registered) {
            (void)::epoll_ctl(
                queue, EPOLL_CTL_DEL, static_cast<int>(entry.socket), nullptr);
            entry.registered = false;
        }
    }
    // Returns the number of events, 0 on timeout/interrupt, -1 on failure.
    // Wake events are drained and reported as `woken`.
    int queue_wait(std::vector<std::uint64_t>& payloads, bool& woken) noexcept
    {
        std::array<epoll_event, maximum_events_per_wait> events {};
        const int result = ::epoll_wait(queue, events.data(),
            static_cast<int>(events.size()), wait_timeout_milliseconds);
        if (result < 0) {
            return errno == EINTR ? 0 : -1;
        }
        for (int index = 0; index < result; ++index) {
            if (events[static_cast<std::size_t>(index)].data.u64
                == wake_marker) {
                woken = true;
                continue;
            }
            payloads.push_back(
                events[static_cast<std::size_t>(index)].data.u64);
        }
        return result;
    }
    int queue = -1;
#elif defined(ROBOTWEAX_READINESS_KQUEUE)
    [[nodiscard]] bool open_queue() noexcept
    {
        queue = ::kqueue();
        if (queue < 0) {
            return false;
        }
        struct kevent change;
        EV_SET(&change, wake_socket.native_handle(), EVFILT_READ, EV_ADD, 0, 0,
            reinterpret_cast<void*>(wake_marker));
        return ::kevent(queue, &change, 1, nullptr, 0, nullptr) == 0;
    }
    [[nodiscard]] bool queue_arm(Entry& entry, std::size_t slot) noexcept
    {
        struct kevent change;
        // EV_DISPATCH disables the filter after one delivery; EV_ADD on an
        // existing filter re-enables it with the new payload.
        EV_SET(&change, entry.socket, EVFILT_READ,
            EV_ADD | EV_ENABLE | EV_DISPATCH, 0, 0,
            reinterpret_cast<void*>(pack_token(slot, entry.generation)));
        if (::kevent(queue, &change, 1, nullptr, 0, nullptr) != 0) {
            return false;
        }
        entry.registered = true;
        return true;
    }
    void queue_remove(Entry& entry) noexcept
    {
        if (entry.registered) {
            struct kevent change;
            EV_SET(
                &change, entry.socket, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
            (void)::kevent(queue, &change, 1, nullptr, 0, nullptr);
            entry.registered = false;
        }
    }
    int queue_wait(std::vector<std::uint64_t>& payloads, bool& woken) noexcept
    {
        std::array<struct kevent, maximum_events_per_wait> events {};
        const timespec timeout {
            .tv_sec = 0,
            .tv_nsec = wait_timeout_milliseconds * 1'000'000L,
        };
        const int result = ::kevent(queue, nullptr, 0, events.data(),
            static_cast<int>(events.size()), &timeout);
        if (result < 0) {
            return errno == EINTR ? 0 : -1;
        }
        for (int index = 0; index < result; ++index) {
            const auto payload = reinterpret_cast<std::uintptr_t>(
                events[static_cast<std::size_t>(index)].udata);
            if (static_cast<std::uint64_t>(payload) == wake_marker) {
                woken = true;
                continue;
            }
            payloads.push_back(static_cast<std::uint64_t>(payload));
        }
        return result;
    }
    int queue = -1;
#endif

    std::mutex mutex;
    std::condition_variable poll_finished;
    std::uint64_t poll_epoch = 0;
    std::uint64_t completed_poll_epoch = 0;
    std::size_t cancellation_waiters = 0;
    std::mutex wake_mutex;
    UdpSocket wake_socket;
    IpEndpoint wake_endpoint {};
    std::vector<Entry> entries;
    std::vector<std::size_t> free_slots;
    // The following scratch arrays have one owner: the watcher thread.
#if defined(ROBOTWEAX_READINESS_POLL)
    std::vector<Descriptor> descriptors;
#else
    std::vector<std::uint64_t> ready;
#endif
    std::vector<Token> tokens;
    std::vector<Callback> callbacks;
    Snapshot counters;
    bool started = false;
    bool stopping = false;
};

SocketReadiness::SocketReadiness(std::size_t capacity)
    : state_(std::make_shared<State>(capacity))
{
}
SocketReadiness::~SocketReadiness()
{
    stop();
}

bool SocketReadiness::arms_without_wake() noexcept
{
#if defined(ROBOTWEAX_READINESS_POLL)
    return false;
#else
    return true;
#endif
}

bool SocketReadiness::start() noexcept
{
    std::lock_guard lifecycle_lock(lifecycle_mutex_);
    std::lock_guard lock(state_->mutex);
    if (state_->started) {
        return state_->counters.running;
    }
    state_->started = true;
    if (state_->stopping || state_->entries.empty()
        || state_->wake_socket.bind(IpEndpoint::loopback()) != Error::none) {
        return false;
    }
    const auto endpoint = state_->wake_socket.local_endpoint();
    if (!endpoint) {
        return false;
    }
    state_->wake_endpoint = endpoint.endpoint;
#if !defined(ROBOTWEAX_READINESS_POLL)
    if (!state_->open_queue()) {
        return false;
    }
#endif
    state_->counters.running = true;
    try {
        worker_ = std::thread(run, state_);
    } catch (...) {
        state_->counters.running = false;
        return false;
    }
    return true;
}

SocketReadiness::Token SocketReadiness::watch(
    std::uintptr_t socket, Callback callback) noexcept
{
    std::lock_guard lock(state_->mutex);
    if (!state_->counters.running || state_->stopping
        || state_->free_slots.empty() || callback.function == nullptr
        || socket == (std::numeric_limits<std::uintptr_t>::max)()) {
        return {};
    }
    const auto slot = state_->free_slots.back();
    state_->free_slots.pop_back();
    auto& entry = state_->entries[slot];
    if (++entry.generation == 0U) {
        ++entry.generation;
    }
    entry.socket = socket;
    entry.callback = std::move(callback);
    entry.active = true;
    entry.armed = false;
    entry.retiring = false;
    entry.registered = false;
    ++state_->counters.registered;
    return {slot, entry.generation};
}

bool SocketReadiness::arm(Token token) noexcept
{
    {
        std::lock_guard lock(state_->mutex);
        if (!state_->counters.running || state_->stopping
            || token.slot >= state_->entries.size()) {
            return false;
        }
        auto& entry = state_->entries[token.slot];
        if (!entry.active || entry.generation != token.generation) {
            return false;
        }
        if (entry.armed) {
            return true;
        }
#if !defined(ROBOTWEAX_READINESS_POLL)
        // The kernel queue takes the interest directly; a concurrent wait
        // observes it without being woken.
        if (!state_->queue_arm(entry, token.slot)) {
            return false;
        }
        entry.armed = true;
        ++state_->counters.armed;
        return true;
#else
        entry.armed = true;
        ++state_->counters.armed;
#endif
    }
    state_->wake();
    return true;
}

void SocketReadiness::cancel(Token token) noexcept
{
    Callback retired_callback;
    std::uint64_t pending_epoch = 0;
    {
        std::lock_guard lock(state_->mutex);
        if (token.slot >= state_->entries.size()) {
            return;
        }
        auto& entry = state_->entries[token.slot];
        if (entry.generation != token.generation
            || (!entry.active && !entry.retiring)) {
            return;
        }
        if (entry.active) {
            state_->counters.armed -= entry.armed ? 1U : 0U;
            --state_->counters.registered;
            entry.active = entry.armed = false;
            entry.retiring = true;
            retired_callback = std::move(entry.callback);
#if !defined(ROBOTWEAX_READINESS_POLL)
            // Removing the interest under the lock means no later wait can
            // report this socket; an event already returned is discarded by
            // its generation. The caller may close the socket at once.
            state_->queue_remove(entry);
#endif
        }
#if defined(ROBOTWEAX_READINESS_POLL)
        pending_epoch = state_->poll_epoch;
        if (pending_epoch > state_->completed_poll_epoch) {
            ++state_->cancellation_waiters;
        } else {
            pending_epoch = 0;
        }
#endif
    }
#if defined(ROBOTWEAX_READINESS_POLL)
    state_->wake();
#endif
    {
        std::unique_lock lock(state_->mutex);
        if (pending_epoch != 0) {
            // Removing a watch is not enough for poll: a concurrent poll can
            // retain the native socket after close and keep its UDP port
            // bound.
            state_->poll_finished.wait(lock, [&] {
                return state_->completed_poll_epoch >= pending_epoch;
            });
            --state_->cancellation_waiters;
        }
        auto& entry = state_->entries[token.slot];
        if (entry.generation == token.generation && entry.retiring) {
            entry.retiring = false;
            state_->free_slots.push_back(token.slot);
        }
    }
}

void SocketReadiness::stop() noexcept
{
    std::lock_guard lifecycle_lock(lifecycle_mutex_);
    {
        std::lock_guard lock(state_->mutex);
        state_->stopping = true;
    }
    state_->wake();
    if (worker_.joinable()) {
        if (worker_.get_id() == std::this_thread::get_id()) {
            // A callback may release the last owner. run() owns State until
            // it exits; it never refers to the destroyed SocketReadiness.
            worker_.detach();
        } else {
            worker_.join();
        }
    }
}

SocketReadiness::Snapshot SocketReadiness::snapshot() const noexcept
{
    std::lock_guard lock(state_->mutex);
    return state_->counters;
}

#if defined(ROBOTWEAX_READINESS_POLL)
void SocketReadiness::run(std::shared_ptr<State> state) noexcept
{
    for (;;) {
        state->descriptors.clear();
        state->tokens.clear();
        state->callbacks.clear();
        {
            std::lock_guard lock(state->mutex);
            if (state->stopping) {
                state->counters.running = false;
                return;
            }
            state->descriptors.push_back(
                descriptor(state->wake_socket.native_handle()));
            ++state->poll_epoch;
            for (std::size_t index = 0; index < state->entries.size();
                ++index) {
                const auto& entry = state->entries[index];
                if (entry.active && entry.armed) {
                    state->descriptors.push_back(descriptor(entry.socket));
                    state->tokens.push_back({index, entry.generation});
                }
            }
            ++state->counters.waits;
        }
#if defined(_WIN32)
        const int result = WSAPoll(state->descriptors.data(),
            static_cast<ULONG>(state->descriptors.size()),
            wait_timeout_milliseconds);
        const bool interrupted = result < 0 && WSAGetLastError() == WSAEINTR;
#else
        const int result = ::poll(state->descriptors.data(),
            static_cast<nfds_t>(state->descriptors.size()),
            wait_timeout_milliseconds);
        const bool interrupted = result < 0 && errno == EINTR;
#endif
        const bool idle = interrupted || result == 0;
        if (!idle && state->descriptors.front().revents != 0) {
            state->drain_wake();
        }
        const bool failed = !idle
            && (result < 0
                || (state->descriptors.front().revents
                       & (POLLERR | POLLHUP | POLLNVAL))
                    != 0);
        {
            std::lock_guard lock(state->mutex);
            state->completed_poll_epoch = state->poll_epoch;
            if (state->cancellation_waiters != 0) {
                state->poll_finished.notify_all();
            }
            if (idle) {
                continue;
            }
            if (failed) {
                state->counters.running = false;
            }
            const auto count =
                failed ? state->entries.size() : state->tokens.size();
            for (std::size_t index = 0; index < count; ++index) {
                if (!failed && state->descriptors[index + 1U].revents == 0) {
                    continue;
                }
                const auto token = failed
                    ? Token {index, state->entries[index].generation}
                    : state->tokens[index];
                auto& entry = state->entries[token.slot];
                if (!entry.active || !entry.armed
                    || entry.generation != token.generation) {
                    continue;
                }
                entry.armed = false;
                --state->counters.armed;
                ++state->counters.notifications;
                state->callbacks.push_back(entry.callback);
            }
        }
        for (const auto& callback : state->callbacks) {
            callback.function(callback.context.get());
        }
        if (failed) {
            return;
        }
    }
}
#else
void SocketReadiness::run(std::shared_ptr<State> state) noexcept
{
    for (;;) {
        state->ready.clear();
        state->callbacks.clear();
        {
            std::lock_guard lock(state->mutex);
            if (state->stopping) {
                state->counters.running = false;
                return;
            }
            ++state->poll_epoch;
            ++state->counters.waits;
        }
        bool woken = false;
        const int result = state->queue_wait(state->ready, woken);
        if (woken) {
            state->drain_wake();
        }
        const bool failed = result < 0;
        {
            std::lock_guard lock(state->mutex);
            state->completed_poll_epoch = state->poll_epoch;
            if (state->cancellation_waiters != 0) {
                state->poll_finished.notify_all();
            }
            if (failed) {
                // The queue is unusable: report every armed watch once so
                // its owner falls back to timer polling, then retire.
                state->counters.running = false;
                for (auto& entry : state->entries) {
                    if (entry.active && entry.armed) {
                        entry.armed = false;
                        --state->counters.armed;
                        ++state->counters.notifications;
                        state->callbacks.push_back(entry.callback);
                    }
                }
            } else {
                for (const auto payload : state->ready) {
                    const auto slot =
                        static_cast<std::size_t>(payload & 0xffff'ffffU);
                    const auto generation = payload >> 32U;
                    if (slot >= state->entries.size()) {
                        continue;
                    }
                    auto& entry = state->entries[slot];
                    if (!entry.active || !entry.armed
                        || entry.generation != generation) {
                        continue;
                    }
                    // The one-shot interest is disabled in the kernel until
                    // the next arm.
                    entry.armed = false;
                    --state->counters.armed;
                    ++state->counters.notifications;
                    state->callbacks.push_back(entry.callback);
                }
            }
        }
        for (const auto& callback : state->callbacks) {
            callback.function(callback.context.get());
        }
        if (failed) {
            return;
        }
    }
}
#endif

} // namespace robotweax::srt::compat
