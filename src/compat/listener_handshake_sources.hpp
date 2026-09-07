#pragma once

#include "compat/listener_handshake_router.hpp"
#include "compat/runtime_scheduler.hpp"
#include "compat/transport_runtime.hpp"

#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace robotweax::srt::compat {

enum class ListenerHandshakeAdmissionStepKind : std::uint8_t {
    ignore,
    send,
    admit,
    closed,
};

struct ListenerHandshakeAdmission {
    HandshakeEnvelope initial {};
    HandshakeEnvelope conclusion {};
    ListenerHandshakeProtocol protocol = ListenerHandshakeProtocol::invalid;
    std::uint32_t validated_cookie = 0;
};

struct ListenerHandshakeAdmissionStep {
    ListenerHandshakeAdmissionStepKind kind =
        ListenerHandshakeAdmissionStepKind::ignore;
    IpEndpoint peer {};
    std::uint32_t destination_socket_id = 0;
    HandshakeAction response {};
    ListenerHandshakeAdmission admission {};
};

// I/O- and clock-free admission boundary for the Listener actor. Each call
// consumes one already decoded setup envelope and emits exactly one bounded
// result. The owner supplies the cookie time window and performs any send or
// accepted-socket construction after the step returns.
class ListenerHandshakeAdmissionSteps {
public:
    explicit ListenerHandshakeAdmissionSteps(
        StatelessListenerHandshakeRouter::Configuration configuration) noexcept;

    [[nodiscard]] ListenerHandshakeAdmissionStep receive(
        const HandshakeEnvelope& envelope, std::uint64_t time_window) noexcept;
    [[nodiscard]] ListenerHandshakeAdmissionStep close() noexcept;

    [[nodiscard]] bool closed() const noexcept
    {
        return closed_;
    }

private:
    StatelessListenerHandshakeRouter router_;
    bool closed_ = false;
};

enum class ListenerHandshakeSourceEventKind : std::uint8_t {
    inbox_ready,
    closed,
    failure,
};

enum class ListenerHandshakeDispatchStatus : std::uint8_t {
    completed,
    full,
    stopped,
    invalid,
};

struct ListenerHandshakeSourceEvent {
    ListenerHandshakeSourceEventKind kind =
        ListenerHandshakeSourceEventKind::failure;
    ListenerHandshakeDispatchStatus failure =
        ListenerHandshakeDispatchStatus::invalid;
};

// Translates one listener Handshake Inbox's empty-to-ready edge into bounded
// work on the listener affinity shard. The consumer drains the Inbox without
// waiting, acknowledges the marker, and receives another marker if datagrams
// arrived in the meantime. No scheduler shard blocks on accept or application
// callbacks at this boundary.
class ListenerHandshakeEventSource final
    : public std::enable_shared_from_this<ListenerHandshakeEventSource> {
public:
    using EventReadyFunction = void (*)(void*) noexcept;

    ListenerHandshakeEventSource(RuntimeScheduler& scheduler,
        std::uint64_t affinity, std::shared_ptr<HandshakeInbox> inbox) noexcept;
    ~ListenerHandshakeEventSource();

    ListenerHandshakeEventSource(const ListenerHandshakeEventSource&) = delete;
    ListenerHandshakeEventSource& operator=(
        const ListenerHandshakeEventSource&) = delete;

    [[nodiscard]] ListenerHandshakeDispatchStatus start() noexcept;
    [[nodiscard]] ListenerHandshakeSourceEvent wait() noexcept;
    [[nodiscard]] bool try_pop(ListenerHandshakeSourceEvent& event) noexcept;
    [[nodiscard]] bool set_event_ready_handler(
        EventReadyFunction function, std::shared_ptr<void> context) noexcept;
    void clear_event_ready_handler() noexcept;

    void acknowledge_inbox() noexcept;
    void close() noexcept;
    void stop() noexcept;

private:
    static void inbox_ready(void* context) noexcept;
    static void publish_inbox_ready(void* context) noexcept;

    void request_inbox() noexcept;
    void enqueue(ListenerHandshakeSourceEvent event) noexcept;
    void fail(ListenerHandshakeDispatchStatus failure) noexcept;
    void notify_event_ready() noexcept;

    RuntimeScheduler& scheduler_;
    std::uint64_t affinity_ = 0;
    std::shared_ptr<HandshakeInbox> inbox_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::array<ListenerHandshakeSourceEvent, 2> events_ {};
    std::size_t head_ = 0;
    std::size_t size_ = 0;
    ListenerHandshakeDispatchStatus failure_ =
        ListenerHandshakeDispatchStatus::completed;
    EventReadyFunction event_ready_handler_ = nullptr;
    std::shared_ptr<void> event_ready_context_;
    bool started_ = false;
    bool handler_registered_ = false;
    bool inbox_pending_ = false;
    bool closed_ = false;
    bool stopped_ = false;
};

enum class ListenerHandshakeActorResultKind : std::uint8_t {
    step,
    failure,
    closed,
};

struct ListenerHandshakeActorResult {
    ListenerHandshakeActorResultKind kind =
        ListenerHandshakeActorResultKind::failure;
    ListenerHandshakeAdmissionStep step {};
    ListenerHandshakeDispatchStatus failure =
        ListenerHandshakeDispatchStatus::invalid;
};

struct ListenerHandshakeActorSnapshot {
    std::size_t queued_results = 0;
    ListenerHandshakeDispatchStatus failure =
        ListenerHandshakeDispatchStatus::completed;
    bool started = false;
    bool admission_pending = false;
    bool terminal = false;
};

// Joins the Listener readiness source and stateless admission step on one
// affinity shard. Each Inbox marker consumes at most one envelope. Send and
// admit results cross a construction-time-bounded queue with an optional
// nonblocking result-ready callback, so network I/O, callbacks, and accepted-
// socket setup remain outside the scheduler shard. An admit result holds Inbox
// rearming until its consumer completes the setup handoff explicitly.
class ListenerHandshakeActor final
    : public std::enable_shared_from_this<ListenerHandshakeActor> {
public:
    using TimeWindowFunction = std::uint64_t (*)(void*) noexcept;
    using ResultReadyFunction = void (*)(void*) noexcept;

    ListenerHandshakeActor(std::shared_ptr<RuntimeScheduler> scheduler,
        std::uint64_t affinity, std::shared_ptr<HandshakeInbox> inbox,
        StatelessListenerHandshakeRouter::Configuration configuration,
        std::size_t result_capacity, TimeWindowFunction time_window,
        std::shared_ptr<void> time_window_context = {});
    ~ListenerHandshakeActor();

    ListenerHandshakeActor(const ListenerHandshakeActor&) = delete;
    ListenerHandshakeActor& operator=(const ListenerHandshakeActor&) = delete;

    [[nodiscard]] ListenerHandshakeDispatchStatus start() noexcept;
    void close() noexcept;
    void stop() noexcept;

    [[nodiscard]] ListenerHandshakeActorResult wait() noexcept;
    [[nodiscard]] bool try_pop(ListenerHandshakeActorResult& result) noexcept;
    [[nodiscard]] bool set_result_ready_handler(
        ResultReadyFunction function, std::shared_ptr<void> context) noexcept;
    void clear_result_ready_handler() noexcept;
    [[nodiscard]] bool complete_admission() noexcept;
    [[nodiscard]] ListenerHandshakeActorSnapshot snapshot() const noexcept;

private:
    static void run_task(void* context) noexcept;
    static void source_ready(void* context) noexcept;

    void request_run() noexcept;
    void run() noexcept;
    void drain_source() noexcept;
    void process_inbox() noexcept;
    [[nodiscard]] bool publish_step(
        ListenerHandshakeAdmissionStep step) noexcept;
    void finish_failure(ListenerHandshakeDispatchStatus failure) noexcept;
    void finish_closed() noexcept;
    void finish(ListenerHandshakeActorResult terminal) noexcept;
    void notify_result_ready() noexcept;

    std::shared_ptr<RuntimeScheduler> scheduler_;
    std::uint64_t affinity_ = 0;
    std::shared_ptr<HandshakeInbox> inbox_;
    ListenerHandshakeAdmissionSteps steps_;
    TimeWindowFunction time_window_ = nullptr;
    std::shared_ptr<void> time_window_context_;
    std::shared_ptr<ListenerHandshakeEventSource> source_;
    std::vector<ListenerHandshakeActorResult> results_;
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::condition_variable completed_;
    ResultReadyFunction result_ready_handler_ = nullptr;
    std::shared_ptr<void> result_ready_context_;
    std::size_t head_ = 0;
    std::size_t size_ = 0;
    ListenerHandshakeDispatchStatus failure_ =
        ListenerHandshakeDispatchStatus::completed;
    std::thread::id active_thread_ {};
    bool started_ = false;
    bool close_requested_ = false;
    bool pending_ = false;
    bool scheduled_ = false;
    bool admission_pending_ = false;
    bool terminal_ = false;
};

} // namespace robotweax::srt::compat
