#include "compat/connection_datagram_dispatcher.hpp"

#include "compat/transport_runtime.hpp"
#include "robotweax/srt/handshake_datagram.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
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
    const IpEndpoint peer;
    std::shared_ptr<ConnectionDatagramInbox> inbox;
    std::weak_ptr<ConnectionWorkBinding> binding;
    mutable std::mutex prefix_mutex;
    std::shared_ptr<DatagramInbox> setup_prefix;
    bool route_claimed = false;
    bool active = true;
    bool retired = false;
    std::atomic_bool reclaim_requested {false};
    std::atomic<std::uint64_t> completed_turns {0};
    std::atomic<std::uint64_t> dispatched_datagrams {0};
    std::atomic<std::size_t> maximum_turn_datagrams {0};

    State(std::weak_ptr<ConnectionRuntime> target, Configuration config,
        IpEndpoint endpoint) noexcept
        : runtime(std::move(target))
        , configuration(std::move(config))
        , peer(endpoint)
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
        std::shared_ptr<DatagramInbox> released;
        {
            std::lock_guard lock(prefix_mutex);
            retired = true;
            active = false;
            released = std::move(setup_prefix);
        }
        if (auto service = binding.lock(); service != nullptr) {
            service->retire();
        }
    }
    static void complete(void* pointer) noexcept
    {
        auto& self = *static_cast<State*>(pointer);
        if (self.reclaim_requested.load(std::memory_order_acquire)) {
            (void)self.inbox->reclaim_retired_storage();
        }
    }
    static void dispatch(void* pointer, ConnectionWorkHints hints) noexcept
    {
        auto& self = *static_cast<State*>(pointer);
        if (!hints.datagrams) {
            return;
        }
        {
            std::lock_guard lock(self.prefix_mutex);
            if (!self.active || self.retired) {
                return;
            }
        }
        const auto runtime = self.runtime.lock();
        if (runtime == nullptr) {
            self.retire();
            return;
        }
        bool completed_prefix = false;
        std::size_t consumed = 0;
        for (; consumed < self.configuration.turn_budget; ++consumed) {
            if (!runtime->accepts_datagrams()) {
                self.retire();
                break;
            }
            DatagramEnvelope envelope;
            std::shared_ptr<DatagramInbox> prefix;
            {
                std::lock_guard lock(self.prefix_mutex);
                prefix = self.setup_prefix;
            }
            const bool from_prefix =
                prefix != nullptr && prefix->pop_dispatch_prefix(envelope);
            if (!from_prefix) {
                if (prefix != nullptr) {
                    std::lock_guard lock(self.prefix_mutex);
                    if (self.setup_prefix == prefix) {
                        self.setup_prefix.reset();
                        completed_prefix = true;
                    }
                }
                if (self.inbox->pop(self.inbox->token(), envelope, self.now())
                    != ConnectionDatagramInbox::Status::accepted) {
                    break;
                }
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
            if (!from_prefix) {
                (void)self.inbox->complete(self.inbox->token());
            }
            // Empty is published only after the last popped prefix copy has
            // completed its protocol call. Release the old ring outside locks.
            if (from_prefix && prefix->dispatch_prefix_empty()) {
                std::lock_guard lock(self.prefix_mutex);
                if (self.setup_prefix == prefix) {
                    self.setup_prefix.reset();
                    completed_prefix = true;
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
        // Channel polling retains the shared send allowance. Wake it after
        // asynchronous ingress effects and when the prefix barrier opens.
        if (consumed != 0 || completed_prefix) {
            runtime->notify_channel_send_work();
        }
        if (!runtime->accepts_datagrams()) {
            self.retire();
        } else {
            bool prefix_pending;
            {
                std::lock_guard lock(self.prefix_mutex);
                prefix_pending = self.setup_prefix != nullptr;
            }
            const auto service = self.binding.lock();
            const bool wake_failed = prefix_pending
                ? service == nullptr
                    || service->notify({.datagrams = true})
                        != RuntimeScheduler::SubmitStatus::accepted
                : self.inbox->rearm(self.inbox->token())
                    == ConnectionDatagramInbox::Status::wake_failed;
            if (wake_failed) {
                runtime->mark_broken(0);
                self.retire();
            }
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
    Configuration configuration,
    const std::shared_ptr<DatagramInbox>& setup_prefix,
    const std::shared_ptr<DatagramStorageBudget>& process_budget) noexcept
{
    if (runtime == nullptr || configuration.turn_budget == 0
        || configuration.turn_budget > maximum_turn_budget
        || !runtime->accepts_datagrams()) {
        return nullptr;
    }
    try {
        auto state =
            std::make_shared<State>(runtime, std::move(configuration), peer);
        auto binding = ConnectionWorkBinding::create(
            scheduler, affinity, State::dispatch, state, State::complete);
        if (binding == nullptr) {
            return nullptr;
        }
        auto inbox = ConnectionDatagramInbox::create(
            budget, binding, peer, inbox_configuration, process_budget);
        if (inbox == nullptr) {
            return nullptr;
        }
        // The reserved service has not been notified or exposed. Initialization
        // completes before the first publication can make its callback runnable.
        inbox->bind_runtime(runtime);
        state->inbox = std::move(inbox);
        state->binding = binding;
        auto dispatcher = std::shared_ptr<ConnectionDatagramDispatcher>(
            new ConnectionDatagramDispatcher(state, binding));
        if (setup_prefix != nullptr) {
            std::lock_guard lock(setup_prefix->mutex_);
            if (setup_prefix->promoted_runtime_ != nullptr
                || setup_prefix->closed_) {
                return nullptr;
            }
            {
                std::lock_guard prefix_lock(state->prefix_mutex);
                state->setup_prefix = setup_prefix;
                state->route_claimed = true;
            }
            setup_prefix->promoted_runtime_ = runtime;
            setup_prefix->promoted_peer_ = peer;
            setup_prefix->queued_promotion_ = true;
            setup_prefix->promoted_dispatcher_ = dispatcher;
            if (binding->notify({.datagrams = true})
                != RuntimeScheduler::SubmitStatus::accepted) {
                setup_prefix->promoted_runtime_.reset();
                setup_prefix->queued_promotion_ = false;
                setup_prefix->promoted_dispatcher_.reset();
                return nullptr;
            }
            setup_prefix->ready_.notify_all();
        }
        return dispatcher;
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

void ConnectionDatagramDispatcher::retire_and_reclaim() noexcept
{
    const bool first_request =
        !state_->reclaim_requested.exchange(true, std::memory_order_acq_rel);
    retire();
    if (first_request) {
        binding_->retire(true);
    }
}

void ConnectionDatagramDispatcher::close() noexcept
{
    retire();
    if (const auto runtime = state_->runtime.lock(); runtime != nullptr) {
        runtime->close();
    }
}

bool ConnectionDatagramDispatcher::targets(
    const std::shared_ptr<ConnectionRuntime>& runtime,
    const DatagramChannel* channel) const noexcept
{
    return runtime != nullptr && state_->runtime.lock() == runtime
        && state_->peer == runtime->peer_
        && runtime->channel_.lock().get() == channel;
}

bool ConnectionDatagramDispatcher::matches_peer(IpEndpoint peer) const noexcept
{
    return state_->peer == peer;
}

bool ConnectionDatagramDispatcher::claim_route(
    const std::shared_ptr<DatagramInbox>& prefix) noexcept
{
    std::lock_guard lock(state_->prefix_mutex);
    const auto admission = state_->inbox->snapshot();
    if (state_->route_claimed || state_->retired
        || state_->setup_prefix != nullptr || admission.closed
        || admission.highwater != 0) {
        return false;
    }
    state_->active = false;
    state_->route_claimed = true;
    state_->setup_prefix = prefix;
    return true;
}

bool ConnectionDatagramDispatcher::activate_route() noexcept
{
    std::lock_guard lock(state_->prefix_mutex);
    if (state_->retired || !state_->route_claimed) {
        return false;
    }
    state_->active = true;
    if (binding_->notify({.datagrams = true})
        != RuntimeScheduler::SubmitStatus::accepted) {
        state_->active = false;
        return false;
    }
    return true;
}

bool ConnectionDatagramDispatcher::setup_prefix_complete() const noexcept
{
    std::lock_guard lock(state_->prefix_mutex);
    return state_->retired
        || (state_->active && state_->setup_prefix == nullptr);
}

bool ConnectionDatagramDispatcher::quiescent() const noexcept
{
    return binding_->quiescent();
}

ConnectionDatagramDispatcher::DrainResult
ConnectionDatagramDispatcher::finish_retirement(
    std::chrono::steady_clock::time_point deadline) noexcept
{
    retire();
    const auto status = binding_->wait_quiescent(deadline);
    return {.status = status,
        .storage_released =
            status == ConnectionWorkBinding::DrainStatus::quiescent
            && state_->inbox->reclaim_retired_storage()};
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
