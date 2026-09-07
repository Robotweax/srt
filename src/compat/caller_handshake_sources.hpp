#pragma once

#include "compat/caller_handshake_events.hpp"
#include "compat/transport_runtime.hpp"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

namespace robotweax::srt::compat {

enum class CallerHandshakeSourceEventKind : std::uint8_t {
    inbox_ready,
    retry_timer,
    overall_timeout,
    closed,
    failure,
};

struct CallerHandshakeSourceEvent {
    CallerHandshakeSourceEventKind kind =
        CallerHandshakeSourceEventKind::failure;
    CallerHandshakeDispatchStatus failure =
        CallerHandshakeDispatchStatus::invalid;
    std::uint64_t generation = 0;
};

// Binds one setup Inbox and its handshake deadlines to an affinity shard. A
// blocking compatibility consumer may use wait(); the production asynchronous
// Caller installs one ready handler and drains the same bounded queue without
// blocking a shard or polling the Inbox.
class CallerHandshakeEventSource final
    : public std::enable_shared_from_this<CallerHandshakeEventSource> {
public:
    using EventReadyFunction = void (*)(void*) noexcept;

    CallerHandshakeEventSource(RuntimeScheduler& scheduler,
        std::uint64_t affinity, std::shared_ptr<DatagramInbox> inbox) noexcept;
    ~CallerHandshakeEventSource();

    CallerHandshakeEventSource(const CallerHandshakeEventSource&) = delete;
    CallerHandshakeEventSource& operator=(
        const CallerHandshakeEventSource&) = delete;

    [[nodiscard]] CallerHandshakeDispatchStatus start(
        std::chrono::steady_clock::time_point overall_deadline) noexcept;
    [[nodiscard]] CallerHandshakeDispatchStatus arm_retry(
        std::uint32_t timeout_milliseconds) noexcept;
    [[nodiscard]] CallerHandshakeDispatchStatus arm_retry_at(
        std::chrono::steady_clock::time_point deadline) noexcept;
    void cancel_retry() noexcept;
    [[nodiscard]] CallerHandshakeSourceEvent wait() noexcept;
    [[nodiscard]] bool try_pop(CallerHandshakeSourceEvent& event) noexcept;
    [[nodiscard]] bool set_event_ready_handler(
        EventReadyFunction function, std::shared_ptr<void> context) noexcept;
    void clear_event_ready_handler() noexcept;

    // Releases the single outstanding readiness marker and schedules another
    // one if datagrams arrived while the consumer processed the prior event.
    void acknowledge_inbox() noexcept;
    void close() noexcept;
    void stop() noexcept;

private:
    struct TimerContext;

    static void inbox_ready(void* context) noexcept;
    static void publish_inbox_ready(void* context) noexcept;
    static void publish_timer(void* context) noexcept;

    void request_inbox() noexcept;
    void enqueue(CallerHandshakeSourceEvent event) noexcept;
    void fail(CallerHandshakeDispatchStatus failure) noexcept;
    void cancel_timer(RuntimeScheduler::TimerToken token) noexcept;
    void notify_event_ready() noexcept;

    RuntimeScheduler& scheduler_;
    std::uint64_t affinity_ = 0;
    std::shared_ptr<DatagramInbox> inbox_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::array<CallerHandshakeSourceEvent, 8> events_ {};
    std::size_t head_ = 0;
    std::size_t size_ = 0;
    RuntimeScheduler::TimerToken retry_timer_ {};
    RuntimeScheduler::TimerToken overall_timer_ {};
    std::uint64_t retry_generation_ = 0;
    CallerHandshakeDispatchStatus failure_ =
        CallerHandshakeDispatchStatus::completed;
    EventReadyFunction event_ready_handler_ = nullptr;
    std::shared_ptr<void> event_ready_context_;
    bool started_ = false;
    bool handler_registered_ = false;
    bool inbox_pending_ = false;
    bool closed_ = false;
    bool stopped_ = false;
};

} // namespace robotweax::srt::compat
