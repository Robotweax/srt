#pragma once

#include "compat/connection_datagram_inbox.hpp"
#include "compat/runtime_poll_result.hpp"
#include <atomic>
#include <utility>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

namespace robotweax::srt::compat {
class ConnectionRuntime;
class DatagramChannel;
class DatagramInbox;

// A finite send allowance for one channel round, shared across shard callbacks.
class ChannelPollSendBudget {
public:
    static constexpr std::size_t maximum_attempts = 64;
    static constexpr std::size_t maximum_poll_requests = 4;
    [[nodiscard]] std::size_t remaining() const noexcept
    {
        return remaining_.load(std::memory_order_relaxed);
    }

private:
    friend class DatagramChannel;
    friend class ConnectionDatagramDispatcher;
    explicit ChannelPollSendBudget(std::weak_ptr<DatagramChannel> channel,
        std::uint64_t ingress_wait) noexcept
        : channel_(std::move(channel))
        , ingress_wait_microseconds_(ingress_wait)
    {
    }
    [[nodiscard]] std::size_t take(std::size_t maximum) noexcept;
    void refund(std::size_t unused) noexcept;
    // Only the channel coordinator opens/closes enrollment for at most four
    // outstanding captured members. Renewals retain the original send budget.
    // Standalone internal polls keep their existing per-completion wake.
    void begin_completion_window() noexcept;
    [[nodiscard]] bool enroll_poll() noexcept;
    void set_completion_wait_deadline(
        std::optional<std::chrono::steady_clock::time_point> deadline) noexcept;
    [[nodiscard]] bool finish_poll(bool urgent,
        std::optional<std::chrono::steady_clock::time_point> buffered_deadline =
            std::nullopt) noexcept;
    [[nodiscard]] bool seal_completion_window() noexcept;
    std::mutex completion_mutex_;
    std::optional<std::chrono::steady_clock::time_point>
        completion_wait_deadline_;
    std::size_t pending_polls_ = 0;
    bool completion_window_ = false;
    bool enrollment_sealed_ = false;
    const std::weak_ptr<DatagramChannel> channel_;
    const std::uint64_t ingress_wait_microseconds_;
    std::atomic<std::size_t> remaining_ {maximum_attempts};
};

// Optional datagram service. Runtime ownership remains with the connection;
// its protocol methods enforce the close barrier under their existing mutex.
class ConnectionDatagramDispatcher {
public:
    static constexpr std::size_t maximum_turn_budget = 16;
    struct Configuration {
        std::size_t turn_budget = maximum_turn_budget;
        std::uint64_t (*now_function)(void*) noexcept = nullptr;
        std::shared_ptr<void> now_context = nullptr;
        void (*after_pop_for_testing)(void*) noexcept = nullptr;
        std::shared_ptr<void> after_pop_context_for_testing = nullptr;
        void (*before_poll_wake_for_testing)(void*) noexcept = nullptr;
        std::shared_ptr<void> poll_wake_context_for_testing = nullptr;
    };
    struct Snapshot {
        std::uint64_t completed_turns = 0;
        std::uint64_t dispatched_datagrams = 0;
        std::size_t maximum_turn_datagrams = 0;
    };
    [[nodiscard]] static std::shared_ptr<ConnectionDatagramDispatcher> create(
        const std::shared_ptr<ConnectionRuntime>& runtime,
        const std::shared_ptr<RuntimeScheduler>& scheduler,
        const std::shared_ptr<DatagramStorageBudget>& budget,
        std::uint64_t affinity, IpEndpoint peer,
        ConnectionDatagramInbox::Configuration inbox_configuration,
        Configuration configuration,
        // Optional cold, unregistered setup inbox. On success its existing
        // ring becomes a sealed prefix; old publishers target this dispatcher.
        // Failure leaves the setup inbox untouched. Channel route integration
        // must independently commit ownership before exposing an established route.
        const std::shared_ptr<DatagramInbox>& setup_prefix = nullptr,
        const std::shared_ptr<DatagramStorageBudget>& process_budget =
            nullptr) noexcept;
    struct PollCompletion {
        RuntimePollResult result;
        std::size_t send_attempts = 0;
        std::shared_ptr<ChannelPollSendBudget> round;
        std::chrono::steady_clock::time_point completed_at;
        // Captured before runtime polling, never refreshed at receipt delivery.
        // Together these are diagnostic freshness inputs only: the complete
        // protocol invalidation audit and eligibility proof are still absent.
        ConnectionWorkBinding::WorkObservation observed_work;
        std::uint64_t observed_ingress_admitted = 0;
    };
    struct ReceivePollObservation {
        RuntimeReceivePollCertificate certificate;
        ConnectionWorkBinding::WorkObservation work;
        std::uint64_t ingress_admitted = 0;
    };
    // A snapshot only; channel route lifetime and wake handshakes remain separate.
    [[nodiscard]] bool receive_poll_is_current(
        const ReceivePollObservation& observation) const noexcept;
    // Explicit internal request, never inline protocol work. One pending/active/
    // unconsumed result per dispatcher; caller owns deadlines and route commit.
    [[nodiscard]] bool request_poll(
        std::shared_ptr<ChannelPollSendBudget> round) noexcept;
    [[nodiscard]] std::optional<PollCompletion> take_poll_completion() noexcept;
    ~ConnectionDatagramDispatcher();
    ConnectionDatagramDispatcher(const ConnectionDatagramDispatcher&) = delete;
    ConnectionDatagramDispatcher& operator=(
        const ConnectionDatagramDispatcher&) = delete;

    [[nodiscard]] std::shared_ptr<ConnectionDatagramInbox>
    inbox() const noexcept;
    [[nodiscard]] ConnectionDatagramInbox::Status publish(
        ConnectionDatagramInbox::Token token, std::span<const std::byte> bytes,
        IpEndpoint peer, std::uint64_t* admitted_cutoff = nullptr) noexcept;
    // Admission/service retirement only. A dispatched callback may finish;
    // this is not a runtime close or callback quiescence barrier.
    void retire() noexcept;
    // Owning close paths request eventual ring reclamation without waiting.
    // An active callback runs bounded reclamation after its protocol body;
    // otherwise retirement completes it inline. No runtime close is implied.
    void retire_and_reclaim() noexcept;
    // Retire admission, then use the existing synchronous runtime close path.
    // A popped copy can resume afterwards, but cannot mutate the closed runtime.
    void close() noexcept;
    // True only after prefix protocol effects finish, or admission is retired.
    [[nodiscard]] bool setup_prefix_complete() const noexcept;
    [[nodiscard]] bool quiescent() const noexcept;
    struct DrainResult {
        ConnectionWorkBinding::DrainStatus status;
        bool storage_released = false;
    };
    // Retire admission, then wait only for client callbacks outside all worker,
    // route/runtime/inbox locks. No runtime close or scheduler stop is implied.
    // Deadline timeout keeps ring credits; retry is idempotent. Captured closed
    // inbox handles remain safe after a successful ring reclamation.
    [[nodiscard]] DrainResult finish_retirement(
        std::chrono::steady_clock::time_point deadline) noexcept;
    [[nodiscard]] Snapshot snapshot() const noexcept;

private:
    friend class DatagramChannel;
    // Caller holds the route lock, and the prefix inbox lock when supplied.
    // No clock, runtime protocol callback or client callback runs here.
    [[nodiscard]] bool targets(
        const std::shared_ptr<ConnectionRuntime>& runtime,
        const DatagramChannel* channel) const noexcept;
    [[nodiscard]] bool matches_peer(IpEndpoint peer) const noexcept;
    [[nodiscard]] bool claim_route(
        const std::shared_ptr<DatagramInbox>& prefix) noexcept;
    [[nodiscard]] bool activate_route() noexcept;
    struct State;
    ConnectionDatagramDispatcher(std::shared_ptr<State> state,
        std::shared_ptr<ConnectionWorkBinding> binding) noexcept;
    const std::shared_ptr<State> state_;
    const std::shared_ptr<ConnectionWorkBinding> binding_;
};
} // namespace robotweax::srt::compat
