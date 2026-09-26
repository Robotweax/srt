#include "compat/socket_readiness.hpp"
#include "robotweax/srt/udp.hpp"

#include <array>
#include <cerrno>
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
#else
#include <poll.h>
#endif

namespace robotweax::srt::compat {
namespace {
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
}

struct SocketReadiness::State {
    struct Entry {
        std::uintptr_t socket = 0;
        Callback callback;
        std::uint64_t generation = 0;
        bool active = false;
        bool armed = false;
    };
    explicit State(std::size_t capacity)
        : entries(capacity)
    {
        free_slots.reserve(capacity);
        descriptors.reserve(capacity + 1U);
        tokens.reserve(capacity);
        callbacks.reserve(capacity);
        for (std::size_t index = capacity; index != 0U; --index) {
            free_slots.push_back(index - 1U);
        }
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
    std::mutex mutex;
    std::mutex wake_mutex;
    UdpSocket wake_socket;
    IpEndpoint wake_endpoint {};
    std::vector<Entry> entries;
    std::vector<std::size_t> free_slots;
    // The following scratch arrays have one owner: the watcher thread.
    std::vector<Descriptor> descriptors;
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
        entry.armed = true;
        ++state_->counters.armed;
    }
    state_->wake();
    return true;
}

void SocketReadiness::cancel(Token token) noexcept
{
    {
        std::lock_guard lock(state_->mutex);
        if (token.slot >= state_->entries.size()) {
            return;
        }
        auto& entry = state_->entries[token.slot];
        if (!entry.active || entry.generation != token.generation) {
            return;
        }
        state_->counters.armed -= entry.armed ? 1U : 0U;
        --state_->counters.registered;
        entry.active = entry.armed = false;
        entry.callback = {};
        state_->free_slots.push_back(token.slot);
    }
    state_->wake();
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
            static_cast<ULONG>(state->descriptors.size()), 100);
        const bool interrupted = result < 0 && WSAGetLastError() == WSAEINTR;
#else
        const int result = ::poll(state->descriptors.data(),
            static_cast<nfds_t>(state->descriptors.size()), 100);
        const bool interrupted = result < 0 && errno == EINTR;
#endif
        if (interrupted || result == 0) {
            continue;
        }
        if (state->descriptors.front().revents != 0) {
            state->drain_wake();
        }
        const bool failed = result < 0
            || (state->descriptors.front().revents
                   & (POLLERR | POLLHUP | POLLNVAL))
                != 0;
        {
            std::lock_guard lock(state->mutex);
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

} // namespace robotweax::srt::compat
