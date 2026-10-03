#pragma once

#include "compat/connection_datagram_inbox.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace robotweax::srt::compat {
class ConnectionRuntime;
class DatagramChannel;
class DatagramInbox;

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
        const std::shared_ptr<DatagramInbox>& setup_prefix = nullptr) noexcept;
    ~ConnectionDatagramDispatcher();
    ConnectionDatagramDispatcher(const ConnectionDatagramDispatcher&) = delete;
    ConnectionDatagramDispatcher& operator=(
        const ConnectionDatagramDispatcher&) = delete;

    [[nodiscard]] std::shared_ptr<ConnectionDatagramInbox>
    inbox() const noexcept;
    [[nodiscard]] ConnectionDatagramInbox::Status publish(
        ConnectionDatagramInbox::Token token, std::span<const std::byte> bytes,
        IpEndpoint peer) noexcept;
    // Admission/service retirement only. A dispatched callback may finish;
    // this is not a runtime close or callback quiescence barrier.
    void retire() noexcept;
    // Retire admission, then use the existing synchronous runtime close path.
    // A popped copy can resume afterwards, but cannot mutate the closed runtime.
    void close() noexcept;
    // True only after prefix protocol effects finish, or admission is retired.
    [[nodiscard]] bool setup_prefix_complete() const noexcept;
    [[nodiscard]] bool quiescent() const noexcept;
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
