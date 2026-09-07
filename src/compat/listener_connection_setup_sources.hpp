#pragma once

#include "compat/listener_connection_setup.hpp"
#include "compat/runtime_scheduler.hpp"
#include "compat/transport_runtime.hpp"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace robotweax::srt::compat {

enum class ListenerConnectionSetupSourceEventKind : std::uint8_t {
    inbox_ready,
    retry_timer,
    overall_timeout,
    closed,
    failure,
};

enum class ListenerConnectionSetupDispatchStatus : std::uint8_t {
    completed,
    full,
    stopped,
    invalid,
    allocation_failed,
};

struct ListenerConnectionSetupSourceEvent {
    ListenerConnectionSetupSourceEventKind kind =
        ListenerConnectionSetupSourceEventKind::failure;
    ListenerConnectionSetupDispatchStatus failure =
        ListenerConnectionSetupDispatchStatus::invalid;
    std::uint64_t generation = 0;
};

// Binds an admitted Listener socket's bounded setup Inbox and deadlines to its
// scheduler affinity. Empty-to-ready edges and timers cross one fixed-capacity
// queue; consumers can wait outside a shard or install a nonblocking ready
// handler. Retry replacement is generation checked so an already queued stale
// deadline cannot mutate the setup driver.
class ListenerConnectionSetupEventSource final
    : public std::enable_shared_from_this<ListenerConnectionSetupEventSource> {
public:
    using EventReadyFunction = void (*)(void*) noexcept;

    ListenerConnectionSetupEventSource(RuntimeScheduler& scheduler,
        std::uint64_t affinity, std::shared_ptr<DatagramInbox> inbox) noexcept;
    ~ListenerConnectionSetupEventSource();

    ListenerConnectionSetupEventSource(
        const ListenerConnectionSetupEventSource&) = delete;
    ListenerConnectionSetupEventSource& operator=(
        const ListenerConnectionSetupEventSource&) = delete;

    [[nodiscard]] ListenerConnectionSetupDispatchStatus start(
        std::chrono::steady_clock::time_point overall_deadline) noexcept;
    [[nodiscard]] ListenerConnectionSetupDispatchStatus arm_retry(
        std::uint32_t timeout_milliseconds) noexcept;
    [[nodiscard]] ListenerConnectionSetupDispatchStatus arm_retry_at(
        std::chrono::steady_clock::time_point deadline) noexcept;
    void cancel_retry() noexcept;

    [[nodiscard]] ListenerConnectionSetupSourceEvent wait() noexcept;
    [[nodiscard]] bool try_pop(
        ListenerConnectionSetupSourceEvent& event) noexcept;
    [[nodiscard]] bool set_event_ready_handler(
        EventReadyFunction function, std::shared_ptr<void> context) noexcept;
    void clear_event_ready_handler() noexcept;

    void acknowledge_inbox() noexcept;
    void close() noexcept;
    void stop() noexcept;

private:
    struct TimerContext;

    static void inbox_ready(void* context) noexcept;
    static void publish_inbox_ready(void* context) noexcept;
    static void publish_timer(void* context) noexcept;

    void request_inbox() noexcept;
    void enqueue(ListenerConnectionSetupSourceEvent event) noexcept;
    void fail(ListenerConnectionSetupDispatchStatus failure) noexcept;
    void cancel_timer(RuntimeScheduler::TimerToken token) noexcept;
    void notify_event_ready() noexcept;

    RuntimeScheduler& scheduler_;
    std::uint64_t affinity_ = 0;
    std::shared_ptr<DatagramInbox> inbox_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::array<ListenerConnectionSetupSourceEvent, 8> events_ {};
    std::size_t head_ = 0;
    std::size_t size_ = 0;
    RuntimeScheduler::TimerToken retry_timer_ {};
    RuntimeScheduler::TimerToken overall_timer_ {};
    std::uint64_t retry_generation_ = 0;
    ListenerConnectionSetupDispatchStatus failure_ =
        ListenerConnectionSetupDispatchStatus::completed;
    EventReadyFunction event_ready_handler_ = nullptr;
    std::shared_ptr<void> event_ready_context_;
    bool started_ = false;
    bool handler_registered_ = false;
    bool inbox_pending_ = false;
    bool closed_ = false;
    bool stopped_ = false;
};

enum class ListenerConnectionSetupActorResultKind : std::uint8_t {
    setup,
    failure,
};

struct ListenerConnectionSetupActorResult {
    ListenerConnectionSetupActorResultKind kind =
        ListenerConnectionSetupActorResultKind::failure;
    ListenerConnectionSetupResult setup {};
    ListenerConnectionSetupRuntimeState runtime {};
    HandshakeMessage peer_handshake {};
    ControlHeader peer_control {};
    ListenerConnectionSetupDispatchStatus failure =
        ListenerConnectionSetupDispatchStatus::invalid;
    bool has_peer_handshake = false;
};

struct ListenerConnectionSetupActorSnapshot {
    std::size_t queued_results = 0;
    ListenerConnectionSetupDispatchStatus failure =
        ListenerConnectionSetupDispatchStatus::completed;
    bool started = false;
    bool result_pending = false;
    bool terminal = false;
};

// Serial post-admission owner for one accepted connection. The source maps
// private Datagram-Inbox readiness and deadlines onto the affinity shard; the
// actor decodes one bounded input and is the only mutator of the common
// HSv5 setup driver. Complete action sets cross a bounded result queue.
// A running result must be acknowledged only after its network actions have
// completed, which keeps Inbox rearming and retry arming behind the I/O
// boundary. Callback, crypto policy, network I/O, runtime attachment, and
// accept publication remain outside the shard.
class ListenerConnectionSetupActor final
    : public std::enable_shared_from_this<ListenerConnectionSetupActor> {
public:
    using ResultReadyFunction = void (*)(void*) noexcept;

    ListenerConnectionSetupActor(std::shared_ptr<RuntimeScheduler> scheduler,
        std::uint64_t affinity, std::shared_ptr<DatagramInbox> inbox,
        ListenerHandshakeAdmission admission,
        ListenerConnectionSetupSteps::Configuration configuration,
        std::size_t result_capacity);
    ~ListenerConnectionSetupActor();

    ListenerConnectionSetupActor(const ListenerConnectionSetupActor&) = delete;
    ListenerConnectionSetupActor& operator=(
        const ListenerConnectionSetupActor&) = delete;

    [[nodiscard]] ListenerConnectionSetupDispatchStatus start(
        std::chrono::steady_clock::time_point overall_deadline) noexcept;
    // Application callback and crypto policy run off-shard before start().
    // Record their terminal rejection here so the actor still remains the
    // sole mutator that emits the selected protocol generation's wire result.
    [[nodiscard]] bool reject(int rejection_reason) noexcept;
    void close() noexcept;
    void stop() noexcept;

    [[nodiscard]] ListenerConnectionSetupActorResult wait() noexcept;
    [[nodiscard]] bool try_pop(
        ListenerConnectionSetupActorResult& result) noexcept;
    [[nodiscard]] bool set_result_ready_handler(
        ResultReadyFunction function, std::shared_ptr<void> context) noexcept;
    void clear_result_ready_handler() noexcept;
    [[nodiscard]] bool complete_result() noexcept;
    [[nodiscard]] ListenerConnectionSetupActorSnapshot
    snapshot() const noexcept;

private:
    static void run_task(void* context) noexcept;
    static void source_ready(void* context) noexcept;

    [[nodiscard]] ListenerConnectionSetupDispatchStatus request_run() noexcept;
    void run() noexcept;
    void initialize() noexcept;
    void drain_source() noexcept;
    void process_inbox() noexcept;
    void process_source_event(
        const ListenerConnectionSetupSourceEvent& event) noexcept;
    void handle_result(ListenerConnectionSetupResult result,
        bool acknowledge_inbox,
        const HandshakeMessage* peer_handshake = nullptr,
        const ControlHeader* peer_control = nullptr) noexcept;
    void publish_running(ListenerConnectionSetupActorResult result,
        bool acknowledge_inbox,
        std::optional<std::chrono::microseconds> retry_delay) noexcept;
    void finish_setup(ListenerConnectionSetupActorResult result) noexcept;
    void finish_failure(ListenerConnectionSetupDispatchStatus failure) noexcept;
    void finish_closed() noexcept;
    void finish(ListenerConnectionSetupActorResult terminal) noexcept;
    void notify_result_ready() noexcept;

    [[nodiscard]] static std::optional<std::chrono::microseconds> retry_delay(
        const ListenerConnectionSetupResult& result) noexcept;
    [[nodiscard]] static bool has_actions(
        const ListenerConnectionSetupResult& result) noexcept;
    [[nodiscard]] static bool terminal_outcome(
        ListenerConnectionSetupOutcome outcome) noexcept;

    std::shared_ptr<RuntimeScheduler> scheduler_;
    std::uint64_t affinity_ = 0;
    std::shared_ptr<DatagramInbox> inbox_;
    ListenerHandshakeAdmission admission_ {};
    ListenerConnectionSetupSteps steps_;
    std::uint32_t local_socket_id_ = 0;
    std::shared_ptr<ListenerConnectionSetupEventSource> source_;
    std::vector<ListenerConnectionSetupActorResult> results_;
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::condition_variable completed_;
    ResultReadyFunction result_ready_handler_ = nullptr;
    std::shared_ptr<void> result_ready_context_;
    std::size_t head_ = 0;
    std::size_t size_ = 0;
    ListenerConnectionSetupDispatchStatus failure_ =
        ListenerConnectionSetupDispatchStatus::completed;
    std::optional<std::chrono::microseconds> pending_retry_delay_;
    std::optional<int> initial_rejection_reason_;
    std::thread::id active_thread_ {};
    bool started_ = false;
    bool initialized_ = false;
    bool close_requested_ = false;
    bool pending_ = false;
    bool scheduled_ = false;
    bool result_pending_ = false;
    bool result_consumed_ = false;
    bool pending_inbox_acknowledgement_ = false;
    bool terminal_ = false;
};

} // namespace robotweax::srt::compat
