#include "compat/connection_datagram_dispatcher.hpp"

#include "compat/transport_runtime.hpp"
#include "robotweax/srt/handshake_datagram.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <utility>

namespace robotweax::srt::compat {
namespace {
std::uint64_t queue_now() noexcept
{
    const auto now = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch())
                         .count();
    return now > 0 ? static_cast<std::uint64_t>(now) : 0;
}
} // namespace

struct ConnectionDatagramDispatcher::State {
    const std::weak_ptr<ConnectionRuntime> runtime;
    const Configuration configuration;
    std::shared_ptr<ConnectionDatagramInbox> inbox;
    std::weak_ptr<ConnectionWorkBinding> binding;
    std::atomic<std::uint64_t> completed_turns {0};
    std::atomic<std::uint64_t> dispatched_datagrams {0};
    std::atomic<std::size_t> maximum_turn_datagrams {0};

    State(
        std::weak_ptr<ConnectionRuntime> target, Configuration config) noexcept
        : runtime(std::move(target))
        , configuration(std::move(config))
    {
    }

    std::uint64_t now() const noexcept
    {
        return configuration.now_function != nullptr
            ? configuration.now_function(configuration.now_context.get())
            : queue_now();
    }

    void retire() noexcept
    {
        inbox->close();
        if (auto service = binding.lock(); service != nullptr) {
            service->retire();
        }
    }
    static void dispatch(void* pointer, ConnectionWorkHints hints) noexcept
    {
        auto& self = *static_cast<State*>(pointer);
        if (!hints.datagrams) {
            return;
        }
        const auto runtime = self.runtime.lock();
        if (runtime == nullptr) {
            self.retire();
            return;
        }
        std::size_t consumed = 0;
        for (; consumed < self.configuration.turn_budget; ++consumed) {
            if (!runtime->accepts_datagrams()) {
                self.retire();
                break;
            }
            DatagramEnvelope envelope;
            if (self.inbox->pop(self.inbox->token(), envelope, self.now())
                != ConnectionDatagramInbox::Status::accepted) {
                break;
            }
            if (self.configuration.after_pop_for_testing != nullptr) {
                self.configuration.after_pop_for_testing(
                    self.configuration.after_pop_context_for_testing.get());
            }
            const auto bytes = std::span {envelope.bytes}.first(envelope.size);
            const auto decoded = decode_packet(bytes);
            if (decoded) {
                if (decoded.packet.kind == PacketKind::control
                    && decoded.packet.control.type == ControlType::handshake) {
                    const auto handshake = decode_handshake_datagram(bytes);
                    if (handshake) {
                        (void)runtime->process_handshake(
                            handshake.message, envelope.peer);
                    }
                } else {
                    runtime->process_packet(decoded.packet, envelope.peer);
                }
            }
        }
        self.dispatched_datagrams.fetch_add(
            consumed, std::memory_order_relaxed);
        self.maximum_turn_datagrams.store(
            std::max(consumed,
                self.maximum_turn_datagrams.load(std::memory_order_relaxed)),
            std::memory_order_relaxed);
        self.completed_turns.fetch_add(1, std::memory_order_release);
        if (!runtime->accepts_datagrams()) {
            self.retire();
        } else if (self.inbox->rearm(self.inbox->token())
            == ConnectionDatagramInbox::Status::wake_failed) {
            runtime->mark_broken(0);
            self.retire();
        }
    }
};

ConnectionDatagramDispatcher::ConnectionDatagramDispatcher(
    std::shared_ptr<State> state,
    std::shared_ptr<ConnectionWorkBinding> binding) noexcept
    : state_(std::move(state))
    , binding_(std::move(binding))
{
}

std::shared_ptr<ConnectionDatagramDispatcher>
ConnectionDatagramDispatcher::create(
    const std::shared_ptr<ConnectionRuntime>& runtime,
    const std::shared_ptr<RuntimeScheduler>& scheduler,
    const std::shared_ptr<DatagramStorageBudget>& budget,
    std::uint64_t affinity, IpEndpoint peer,
    ConnectionDatagramInbox::Configuration inbox_configuration,
    Configuration configuration) noexcept
{
    if (runtime == nullptr || configuration.turn_budget == 0
        || configuration.turn_budget > maximum_turn_budget
        || !runtime->accepts_datagrams()) {
        return nullptr;
    }
    try {
        auto state = std::make_shared<State>(runtime, std::move(configuration));
        auto binding = ConnectionWorkBinding::create(
            scheduler, affinity, State::dispatch, state);
        if (binding == nullptr) {
            return nullptr;
        }
        auto inbox = ConnectionDatagramInbox::create(
            budget, binding, peer, inbox_configuration);
        if (inbox == nullptr) {
            return nullptr;
        }
        // The reserved service has not been notified or exposed. Initialization
        // completes before the first publication can make its callback runnable.
        state->inbox = std::move(inbox);
        state->binding = binding;
        return std::shared_ptr<ConnectionDatagramDispatcher>(
            new ConnectionDatagramDispatcher(
                std::move(state), std::move(binding)));
    } catch (...) {
        return nullptr;
    }
}

ConnectionDatagramDispatcher::~ConnectionDatagramDispatcher()
{
    retire();
}

std::shared_ptr<ConnectionDatagramInbox>
ConnectionDatagramDispatcher::inbox() const noexcept
{
    return state_->inbox;
}

ConnectionDatagramInbox::Status ConnectionDatagramDispatcher::publish(
    ConnectionDatagramInbox::Token token, std::span<const std::byte> bytes,
    IpEndpoint peer) noexcept
{
    const auto target = state_->runtime.lock();
    if (target == nullptr || !target->accepts_datagrams()) {
        retire();
        return ConnectionDatagramInbox::Status::closed;
    }
    const auto status =
        state_->inbox->publish(token, bytes, peer, state_->now());
    if (status == ConnectionDatagramInbox::Status::wake_failed) {
        if (const auto runtime = state_->runtime.lock(); runtime != nullptr) {
            runtime->mark_broken(0);
        }
        retire();
    }
    return status;
}

void ConnectionDatagramDispatcher::retire() noexcept
{
    state_->retire();
}

void ConnectionDatagramDispatcher::close() noexcept
{
    retire();
    if (const auto runtime = state_->runtime.lock(); runtime != nullptr) {
        runtime->close();
    }
}

bool ConnectionDatagramDispatcher::quiescent() const noexcept
{
    return binding_->quiescent();
}

ConnectionDatagramDispatcher::Snapshot
ConnectionDatagramDispatcher::snapshot() const noexcept
{
    return {.completed_turns =
                state_->completed_turns.load(std::memory_order_acquire),
        .dispatched_datagrams =
            state_->dispatched_datagrams.load(std::memory_order_relaxed),
        .maximum_turn_datagrams =
            state_->maximum_turn_datagrams.load(std::memory_order_relaxed)};
}
} // namespace robotweax::srt::compat
