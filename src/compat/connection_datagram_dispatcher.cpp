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

std::size_t ChannelPollSendBudget::take(std::size_t maximum) noexcept
{
    auto remaining = remaining_.load(std::memory_order_relaxed);
    for (;;) {
        const auto granted = std::min(remaining, maximum);
        if (remaining_.compare_exchange_weak(
                remaining, remaining - granted, std::memory_order_relaxed))
            return granted;
    }
}

void ChannelPollSendBudget::refund(std::size_t unused) noexcept
{
    remaining_.fetch_add(unused, std::memory_order_relaxed);
}

void ChannelPollSendBudget::begin_completion_window() noexcept
{
    std::lock_guard lock(completion_mutex_);
    completion_window_ = true;
    // Reopening enrollment does not replenish the shared send allowance or
    // remove outstanding members. Only captured, completed routes can renew.
    enrollment_sealed_ = false;
}

bool ChannelPollSendBudget::enroll_poll() noexcept
{
    std::lock_guard lock(completion_mutex_);
    if (!completion_window_)
        return true;
    if (enrollment_sealed_ || pending_polls_ == maximum_poll_requests)
        return false;
    ++pending_polls_;
    return true;
}

void ChannelPollSendBudget::set_completion_wait_deadline(
    std::optional<std::chrono::steady_clock::time_point> deadline) noexcept
{
    std::lock_guard lock(completion_mutex_);
    completion_wait_deadline_ = deadline;
}

bool ChannelPollSendBudget::finish_poll(bool urgent,
    std::optional<std::chrono::steady_clock::time_point>
        buffered_deadline) noexcept
{
    std::lock_guard lock(completion_mutex_);
    if (!completion_window_)
        return true;
    // The channel fixes this bound before collecting any receipts. Comparing
    // under the enrollment lock prevents a worker from using an older bound
    // while the coordinator prepares a later sleep. Final completions wake.
    if (buffered_deadline.has_value() && completion_wait_deadline_.has_value()
        && *buffered_deadline >= *completion_wait_deadline_
        && *buffered_deadline > std::chrono::steady_clock::now())
        urgent = false;
    if (pending_polls_ != 0U)
        --pending_polls_;
    return urgent || (enrollment_sealed_ && pending_polls_ == 0U);
}

bool ChannelPollSendBudget::seal_completion_window() noexcept
{
    std::lock_guard lock(completion_mutex_);
    enrollment_sealed_ = true;
    return pending_polls_ == 0U;
}

struct ConnectionDatagramDispatcher::State {
    const std::weak_ptr<ConnectionRuntime> runtime;
    const Configuration configuration;
    const IpEndpoint peer;
    std::shared_ptr<ConnectionDatagramInbox> inbox;
    std::weak_ptr<ConnectionWorkBinding> binding;
    mutable std::mutex prefix_mutex;
    std::shared_ptr<DatagramInbox> setup_prefix;
    std::shared_ptr<ChannelPollSendBudget> pending_poll;
    bool poll_active = false;
    std::optional<PollCompletion> poll_completion;
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
        std::shared_ptr<ChannelPollSendBudget> cancelled_poll;
        {
            std::lock_guard lock(prefix_mutex);
            retired = true;
            active = false;
            cancelled_poll = std::move(pending_poll);
            poll_completion.reset();
            released = std::move(setup_prefix);
        }
        if (cancelled_poll != nullptr && cancelled_poll->finish_poll(true)) {
            if (const auto target = runtime.lock()) {
                if (configuration.before_poll_wake_for_testing != nullptr) {
                    configuration.before_poll_wake_for_testing(
                        configuration.poll_wake_context_for_testing.get());
                }
                target->notify_channel_poll_completion();
            }
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
        if (!hints.datagrams && !hints.send) {
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
        for (; hints.datagrams && consumed < self.configuration.turn_budget;
            ++consumed) {
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
        std::shared_ptr<ChannelPollSendBudget> round;
        bool poll_prefix_pending;
        {
            std::lock_guard lock(self.prefix_mutex);
            if (!self.retired && self.pending_poll != nullptr) {
                round = std::move(self.pending_poll);
                self.poll_active = true;
            }
            poll_prefix_pending = self.setup_prefix != nullptr;
        }
        if (round != nullptr) {
            RuntimePollResult result;
            std::size_t used = 0;
            const auto channel = round->channel_.lock();
            if (channel != nullptr && runtime->channel_.lock() == channel) {
                bool terminal = false;
                if (poll_prefix_pending) {
                    result = runtime->poll_setup_prefix_deadline(
                        round->ingress_wait_microseconds_, terminal);
                } else {
                    const auto grant = round->take(maximum_turn_budget);
                    auto remaining = grant;
                    result = runtime->poll(remaining, self.inbox.get(),
                        round->ingress_wait_microseconds_, true);
                    used = grant - remaining;
                    round->refund(remaining);
                }
                if (terminal)
                    self.retire();
            }
            const auto completed_at = std::chrono::steady_clock::now();
            bool retired;
            {
                std::lock_guard lock(self.prefix_mutex);
                self.poll_active = false;
                retired = self.retired;
                if (!retired)
                    self.poll_completion =
                        PollCompletion {result, used, round, completed_at};
            }
            const auto ingress_wait =
                std::chrono::microseconds {round->ingress_wait_microseconds_};
            const auto horizon = completed_at + ingress_wait;
            // Idle non-urgent partial receipts retain their existing policy.
            // Buffered receive-only receipts additionally need the coordinator's
            // locked, no-later native revisit bound below. Sender/key/unknown
            // work and retirement retain their existing immediate wake.
            const bool urgent = retired || result.immediate_work
                || !result.receive_wait_safe
                || (result.next_work_delay.has_value()
                    && *result.next_work_delay <= ingress_wait)
                || (result.next_work_deadline.has_value()
                    && *result.next_work_deadline <= horizon);
            const auto buffered_deadline = !retired && !result.immediate_work
                    && result.buffered_completion_wait_safe
                ? result.next_work_deadline
                : std::nullopt;
            if (round->finish_poll(urgent, buffered_deadline)) {
                if (self.configuration.before_poll_wake_for_testing
                    != nullptr) {
                    self.configuration.before_poll_wake_for_testing(
                        self.configuration.poll_wake_context_for_testing.get());
                }
                runtime->notify_channel_poll_completion();
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
    IpEndpoint peer, std::uint64_t* admitted_cutoff) noexcept
{
    if (admitted_cutoff != nullptr)
        *admitted_cutoff = 0;
    const auto target = state_->runtime.lock();
    if (target == nullptr || !target->accepts_datagrams()) {
        retire();
        return ConnectionDatagramInbox::Status::closed;
    }
    const auto status = state_->inbox->publish(
        token, bytes, peer, state_->now(), admitted_cutoff);
    if (status == ConnectionDatagramInbox::Status::wake_failed) {
        if (const auto runtime = state_->runtime.lock(); runtime != nullptr) {
            runtime->mark_broken(0);
        }
        retire();
    }
    return status;
}

bool ConnectionDatagramDispatcher::request_poll(
    std::shared_ptr<ChannelPollSendBudget> round) noexcept
{
    const auto runtime = state_->runtime.lock();
    const auto channel = round != nullptr ? round->channel_.lock() : nullptr;
    if (runtime == nullptr || channel == nullptr
        || runtime->channel_.lock() != channel || !runtime->accepts_datagrams())
        return false;
    std::lock_guard lock(state_->prefix_mutex);
    if (state_->retired || !state_->active || state_->pending_poll != nullptr
        || state_->poll_active || state_->poll_completion.has_value())
        return false;
    if (!round->enroll_poll())
        return false;
    state_->pending_poll = std::move(round);
    if (binding_->notify({.send = true})
        != RuntimeScheduler::SubmitStatus::accepted) {
        (void)state_->pending_poll->finish_poll(false);
        state_->pending_poll.reset();
        return false;
    }
    return true;
}

std::optional<ConnectionDatagramDispatcher::PollCompletion>
ConnectionDatagramDispatcher::take_poll_completion() noexcept
{
    std::lock_guard lock(state_->prefix_mutex);
    return std::exchange(state_->poll_completion, std::nullopt);
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
