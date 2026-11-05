#include "compat/group_multicast_emitter.hpp"

#include "compat/transport_runtime.hpp"
#include "robotweax/srt/codec.hpp"
#include "robotweax/srt/connection_group.hpp"

#include <algorithm>
#include <cassert>
#include <limits>
#include <utility>

namespace robotweax::srt::compat {

bool GroupMulticastEmitter::valid_configuration(const Configuration& c) noexcept
{
    if (!c.channel || c.channel->running() || !c.channel->socket.valid()
        || (c.destination.family != IpAddressFamily::ipv4
            && c.destination.family != IpAddressFamily::ipv6)
        || c.destination.family != c.channel->socket.family()
        || !c.destination.is_multicast() || c.destination.port == 0U
        || (c.group_id & group_handle_mask) == 0U || c.generation == 0U
        || c.capacity == 0U || c.capacity > maximum_capacity
        || c.batch_size == 0U || c.batch_size > maximum_batch
        || c.wire_bytes_per_second == 0U || c.maximum_attempts == 0U
        || c.maximum_attempts > maximum_retry_attempts
        || c.retry_delay <= Clock::duration::zero())
        return false;
    const std::size_t overhead = c.destination.is_ipv4() ? 28U : 48U;
    return c.maximum_transmission_unit > overhead + packet_header_size;
}

GroupMulticastEmitter::GroupMulticastEmitter(Configuration configuration)
    : configuration_(std::move(configuration))
    , entries_(configuration_.capacity)
    , outcomes_(configuration_.capacity)
    , network_header_bytes_(configuration_.destination.is_ipv4() ? 28U : 48U)
    , readiness_(std::make_shared<ReadinessSource>())
    , outcome_readiness_(std::make_shared<ReadinessSource>())
    , work_(std::make_shared<WorkContext>())
{
    state_.capacity = configuration_.capacity;
    state_.accepted_next_sequence = configuration_.initial_sequence;
}

std::shared_ptr<GroupMulticastEmitter> GroupMulticastEmitter::create(
    Configuration configuration,
    std::shared_ptr<RuntimeScheduler> scheduler) noexcept
{
    if (!scheduler || !valid_configuration(configuration))
        return {};
    try {
        auto emitter = std::shared_ptr<GroupMulticastEmitter>(
            new GroupMulticastEmitter(std::move(configuration)));
        emitter->work_->owner = emitter;
        const auto reserved = scheduler->reserve_service(
            emitter->configuration_.affinity, {run, emitter->work_});
        if (reserved.status != RuntimeScheduler::SubmitStatus::accepted)
            return {};
        emitter->scheduler_ = std::move(scheduler);
        emitter->service_ = reserved.token;
        return emitter;
    } catch (...) {
        return {};
    }
}

std::shared_ptr<GroupMulticastEmitter>
GroupMulticastEmitter::create_for_testing(Configuration configuration) noexcept
{
    if (!valid_configuration(configuration))
        return {};
    try {
        auto emitter = std::shared_ptr<GroupMulticastEmitter>(
            new GroupMulticastEmitter(std::move(configuration)));
        emitter->work_->owner = emitter;
        emitter->manual_ = true;
        return emitter;
    } catch (...) {
        return {};
    }
}

GroupMulticastEmitter::~GroupMulticastEmitter()
{
    close();
}

GroupMulticastEmitter::TimePoint GroupMulticastEmitter::now_locked() noexcept
{
    const auto observed = configuration_.clock
        ? configuration_.clock(configuration_.clock_context)
        : Clock::now();
    last_now_ = std::max(last_now_, observed);
    return last_now_;
}

GroupMulticastEmitter::TimePoint GroupMulticastEmitter::add_saturated(
    TimePoint now, Clock::duration delay) noexcept
{
    const auto count = now.time_since_epoch().count();
    const auto increment = delay.count();
    if (count > std::numeric_limits<Clock::rep>::max() - increment)
        return TimePoint::max();
    return TimePoint {Clock::duration {count + increment}};
}

bool GroupMulticastEmitter::valid_packet(
    std::span<const std::byte> packet, bool has_owner) const noexcept
{
    if (packet.size() > maximum_datagram_size
        || packet.size()
            > configuration_.maximum_transmission_unit - network_header_bytes_)
        return false;
    const auto decoded = decode_packet(packet);
    return decoded && decoded.packet.kind == PacketKind::data
        && !decoded.packet.data.retransmitted && !decoded.packet.payload.empty()
        && decoded.packet.data.message_number != 0U
        && decoded.packet.data.encryption_key != EncryptionKey::reserved
        && (decoded.packet.data.encryption_key == EncryptionKey::none
            || has_owner)
        && decoded.packet.data.destination_socket_id == configuration_.group_id;
}

GroupMulticastEmitter::Reservation::Reservation(
    std::shared_ptr<GroupMulticastEmitter> owner,
    std::unique_lock<std::mutex> lock, TimePoint deadline, bool report) noexcept
    : owner_(std::move(owner))
    , lock_(std::move(lock))
    , deadline_(deadline)
    , status_(Admission::accepted)
    , report_(report)
{
}

GroupMulticastEmitter::Reservation::~Reservation()
{
    if (lock_.owns_lock())
        lock_.unlock();
    // The final owner may close/destroy the emitter: never retain its mutex.
    owner_.reset();
}

GroupMulticastEmitter::Reservation GroupMulticastEmitter::reserve(
    std::uint64_t generation, SequenceNumber sequence, TimePoint deadline,
    bool report) noexcept
{
    std::unique_lock lock(mutex_);
    if (state_.closed)
        return Reservation {Admission::closed};
    if (state_.failure != Failure::none)
        return Reservation {Admission::failed};
    if (generation != configuration_.generation)
        return Reservation {Admission::wrong_generation};
    if (sequence != state_.accepted_next_sequence)
        return Reservation {Admission::wrong_sequence};
    if (deadline <= now_locked())
        return Reservation {Admission::expired};
    if (state_.queued == entries_.size()
        || (report
            && state_.reported_queued + state_.pending_outcomes
                == outcomes_.size()))
        return Reservation {Admission::full};
    return Reservation {work_->owner.lock(), std::move(lock), deadline, report};
}

GroupMulticastEmitter::Admission GroupMulticastEmitter::Reservation::publish(
    std::span<const std::byte> packet,
    std::shared_ptr<const void> generation_owner, Commit commit, void* context,
    std::shared_ptr<const void> outcome_context) noexcept
{
    // Lock is declared last so all owners (including failure/early returns)
    // retire after unlocking. A consumed or moved reservation cannot publish.
    RetiredOwners retired {};
    std::size_t retired_count = 0;
    auto owner = std::move(owner_);
    auto lock = std::move(lock_);
    if (!owner || !lock.owns_lock())
        return status_ == Admission::accepted ? Admission::cancelled : status_;
    status_ = Admission::cancelled;
    if (report_ != (outcome_context != nullptr)
        || !owner->valid_packet(packet, generation_owner != nullptr))
        return Admission::invalid_packet;
    const auto decoded = decode_packet(packet);
    if (decoded.packet.data.sequence != owner->state_.accepted_next_sequence)
        return Admission::wrong_sequence;
    auto& state = owner->state_;
    Entry& entry =
        owner->entries_[(owner->head_ + state.queued) % owner->entries_.size()];
    std::copy(packet.begin(), packet.end(), entry.bytes.begin());
    entry.size = packet.size();
    entry.sequence = decoded.packet.data.sequence;
    entry.deadline = deadline_;
    entry.attempts = 0;
    entry.generation_owner = std::move(generation_owner);
    entry.outcome_context = std::move(outcome_context);
    ++state.queued;
    state.queued_bytes += packet.size();
    auto rollback = [&] {
        generation_owner = std::move(entry.generation_owner);
        outcome_context = std::move(entry.outcome_context);
        --state.queued;
        state.queued_bytes -= entry.size;
        entry.size = 0;
    };
    if (!owner->manual_
        && owner->scheduler_->notify_service(owner->service_)
            != RuntimeScheduler::SubmitStatus::accepted) {
        rollback();
        owner->fail_locked(
            Failure::scheduler, 0, retired, retired_count, owner->now_locked());
        lock.unlock();
        ReadinessSignal::notify(
            *owner->readiness_, owner->outcome_readiness_.get());
        return Admission::failed;
    }
    if (commit && !commit(context)) {
        rollback();
        return Admission::cancelled;
    }
    state.reported_queued += static_cast<std::size_t>(report_);
    ++state.accepted_packets;
    state.accepted_next_sequence = state.accepted_next_sequence.next();
    if (state.queued == owner->entries_.size()) {
        owner->readiness_->note_not_ready(SRT_EPOLL_OUT);
        owner->outcome_readiness_->note_not_ready(SRT_EPOLL_OUT);
    } else if (state.reported_queued + state.pending_outcomes
        == owner->outcomes_.size()) {
        owner->outcome_readiness_->note_not_ready(SRT_EPOLL_OUT);
    }
    return Admission::accepted;
}

GroupMulticastEmitter::Admission GroupMulticastEmitter::enqueue(
    std::uint64_t generation, std::span<const std::byte> packet,
    TimePoint deadline, std::shared_ptr<const void> generation_owner,
    std::shared_ptr<const void> outcome_context) noexcept
{
    if (!valid_packet(packet, generation_owner != nullptr))
        return Admission::invalid_packet;
    auto reservation =
        reserve(generation, decode_packet(packet).packet.data.sequence,
            deadline, outcome_context != nullptr);
    if (reservation.status() != Admission::accepted)
        return reservation.status();
    return reservation.publish(packet, std::move(generation_owner), nullptr,
        nullptr, std::move(outcome_context));
}

GroupMulticastEmitter::Admission GroupMulticastEmitter::advance_unused(
    std::uint64_t generation, SequenceNumber sequence,
    Reservation::Commit commit, void* context, TimePoint deadline) noexcept
{
    std::lock_guard lock(mutex_);
    if (state_.closed)
        return Admission::closed;
    if (state_.failure != Failure::none)
        return Admission::failed;
    if (generation != configuration_.generation)
        return Admission::wrong_generation;
    if (sequence != state_.accepted_next_sequence)
        return Admission::wrong_sequence;
    if (deadline <= now_locked())
        return Admission::expired;
    if (!commit || !commit(context))
        return Admission::cancelled;
    state_.accepted_next_sequence = sequence.next();
    ++state_.unused_packets;
    return Admission::accepted;
}

void GroupMulticastEmitter::pop_locked(RetiredOwners& retired,
    std::size_t& retired_count, Disposition disposition, TimePoint completed_at,
    Failure failure, int system_error) noexcept
{
    auto& entry = entries_[head_];
    if (entry.outcome_context) {
        assert(state_.reported_queued != 0U
            && state_.pending_outcomes < outcomes_.size());
        auto& outcome = outcomes_[(outcome_head_ + state_.pending_outcomes)
            % outcomes_.size()];
        assert(!outcome.context);
        outcome = {.group_id = configuration_.group_id,
            .generation = configuration_.generation,
            .sequence = entry.sequence,
            .disposition = disposition,
            .failure = failure,
            .system_error = system_error,
            .attempts = entry.attempts,
            .datagram_size = entry.size,
            .completed_at = completed_at,
            .context = std::move(entry.outcome_context)};
        --state_.reported_queued;
        ++state_.pending_outcomes;
    }
    retired[retired_count++] = std::move(entry.generation_owner);
    state_.queued_bytes -= entry.size;
    entry.size = 0;
    entry.attempts = 0;
    head_ = (head_ + 1) % entries_.size();
    --state_.queued;
    retry_ready_ = TimePoint::min();
}

void GroupMulticastEmitter::fail_locked(Failure failure, int error,
    RetiredOwners& retired, std::size_t& retired_count,
    TimePoint completed_at) noexcept
{
    state_.failure = failure;
    state_.system_error = error;
    state_.cancelled_packets += state_.queued;
    while (state_.queued != 0U)
        pop_locked(retired, retired_count, Disposition::failed, completed_at,
            failure, error);
    outcome_readiness_->note_not_ready(SRT_EPOLL_OUT);
    readiness_->note_not_ready(SRT_EPOLL_OUT);
}

GroupMulticastEmitter::TimePoint
GroupMulticastEmitter::wakeup_locked() const noexcept
{
    return std::min(
        entries_[head_].deadline, std::max(rate_ready_, retry_ready_));
}

void GroupMulticastEmitter::run(void* context) noexcept
{
    auto& work = *static_cast<WorkContext*>(context);
    if (auto owner = work.owner.lock())
        owner->drive();
}

void GroupMulticastEmitter::drive() noexcept
{
    RetiredOwners retired {};
    std::size_t retired_count = 0;
    std::unique_lock lock(mutex_);
    if (state_.closed || state_.failure != Failure::none)
        return;
    for (std::size_t work = 0;
        work < configuration_.batch_size && state_.queued != 0U; ++work) {
        const auto now = now_locked();
        auto& entry = entries_[head_];
        if (now >= entry.deadline) {
            ++state_.expired_packets;
            pop_locked(retired, retired_count, Disposition::expired, now);
            continue;
        }
        if (now < std::max(rate_ready_, retry_ready_))
            break;
        ++entry.attempts;
        ++state_.send_attempts;
        // No registry, member or coordinator lock is held. Holding mutex_
        // across nonblocking I/O is the close/admission barrier.
        const auto sent = configuration_.channel->send_datagram(
            std::span {entry.bytes}.first(entry.size),
            configuration_.destination);
        if (sent && sent.bytes_transferred == entry.size) {
            ++state_.submitted_packets;
            state_.submitted_bytes += entry.size;
            state_.last_submitted_sequence = entry.sequence;
            const std::uint64_t numerator =
                (entry.size + network_header_bytes_) * 1'000'000'000ULL;
            const auto rate = configuration_.wire_bytes_per_second;
            const auto ns = numerator / rate
                + static_cast<std::uint64_t>(numerator % rate != 0U);
            const auto delay = std::chrono::ceil<Clock::duration>(
                std::chrono::nanoseconds {static_cast<std::int64_t>(ns)});
            rate_ready_ = add_saturated(now, delay);
            pop_locked(
                retired, retired_count, Disposition::submitted, now_locked());
            continue;
        }
        if (sent) {
            fail_locked(Failure::partial_datagram, sent.system_error, retired,
                retired_count, now_locked());
            break;
        }
        if (sent.error == Error::would_block
            || (sent.error == Error::io_error
                && UdpSocket::is_transient_send_error(sent.system_error))) {
            if (entry.attempts >= configuration_.maximum_attempts) {
                ++state_.retry_exhausted_packets;
                pop_locked(retired, retired_count, Disposition::retry_exhausted,
                    now_locked(), Failure::none, sent.system_error);
                continue;
            }
            retry_ready_ = add_saturated(now, configuration_.retry_delay);
            break;
        }
        fail_locked(Failure::transport, sent.system_error, retired,
            retired_count, now_locked());
        break;
    }
    if (!manual_ && state_.failure == Failure::none) {
        auto scheduled = RuntimeScheduler::SubmitStatus::accepted;
        if (state_.queued != 0U)
            scheduled =
                scheduler_->schedule_service_at(service_, wakeup_locked());
        else
            (void)scheduler_->cancel_service_timer(service_);
        if (scheduled != RuntimeScheduler::SubmitStatus::accepted)
            fail_locked(
                Failure::scheduler, 0, retired, retired_count, now_locked());
    }
    lock.unlock();
    ReadinessSignal::notify(*readiness_, outcome_readiness_.get());
}

GroupMulticastEmitter::Snapshot GroupMulticastEmitter::snapshot() const noexcept
{
    std::lock_guard lock(mutex_);
    auto result = state_;
    result.writable = !state_.closed && state_.failure == Failure::none
        && state_.queued < entries_.size();
    result.report_writable = result.writable
        && state_.reported_queued + state_.pending_outcomes < outcomes_.size();
    if (!state_.closed && state_.failure == Failure::none
        && state_.queued != 0U)
        result.next_wakeup = wakeup_locked();
    return result;
}

void GroupMulticastEmitter::close() noexcept
{
    RetiredOwners retired {};
    std::size_t retired_count = 0;
    std::shared_ptr<DatagramChannel> channel;
    std::shared_ptr<RuntimeScheduler> scheduler;
    RuntimeScheduler::ServiceToken service;
    {
        std::lock_guard lock(mutex_);
        if (state_.closed)
            return;
        state_.closed = true;
        state_.cancelled_packets += state_.queued;
        const auto now = now_locked();
        while (state_.queued != 0U)
            pop_locked(retired, retired_count, Disposition::cancelled, now);
        outcome_readiness_->note_not_ready(SRT_EPOLL_OUT);
        channel.swap(configuration_.channel);
        scheduler.swap(scheduler_);
        service = std::exchange(service_, {});
        readiness_->note_not_ready(SRT_EPOLL_OUT);
    }
    if (scheduler && service.valid())
        (void)scheduler->release_service(service);
    ReadinessSignal::notify(*readiness_, outcome_readiness_.get());
}

std::size_t GroupMulticastEmitter::take_outcomes(
    std::span<Outcome> destination) noexcept
{
    // Caller may reuse an output span that still owns old contexts. Retire
    // those only after unlocking, including when their destructors reenter us.
    std::array<Outcome, maximum_batch> retired {};
    std::unique_lock lock(mutex_);
    const auto count =
        std::min({destination.size(), state_.pending_outcomes, maximum_batch});
    for (std::size_t i = 0; i < count; ++i) {
        retired[i] = std::move(destination[i]);
        destination[i] = std::move(outcomes_[outcome_head_]);
        outcomes_[outcome_head_] = Outcome {};
        outcome_head_ = (outcome_head_ + 1U) % outcomes_.size();
        --state_.pending_outcomes;
    }
    if (count != 0U && state_.pending_outcomes == 0U)
        outcome_readiness_->note_not_ready(SRT_EPOLL_IN);
    lock.unlock();
    if (count != 0U)
        ReadinessSignal::notify(*readiness_, outcome_readiness_.get());
    return count;
}

void GroupMulticastEmitter::pump_for_testing() noexcept
{
    if (manual_)
        drive();
}

} // namespace robotweax::srt::compat
