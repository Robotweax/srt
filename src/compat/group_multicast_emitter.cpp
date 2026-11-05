#include "compat/group_multicast_emitter.hpp"

#include "compat/transport_runtime.hpp"
#include "robotweax/srt/codec.hpp"
#include "robotweax/srt/connection_group.hpp"

#include <algorithm>
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
    , network_header_bytes_(configuration_.destination.is_ipv4() ? 28U : 48U)
    , readiness_(std::make_shared<ReadinessSource>())
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

GroupMulticastEmitter::Admission GroupMulticastEmitter::enqueue(
    std::uint64_t generation, std::span<const std::byte> packet,
    TimePoint deadline, std::shared_ptr<const void> generation_owner) noexcept
{
    // All of these configuration fields are immutable after construction.
    if (packet.size() > maximum_datagram_size
        || packet.size()
            > configuration_.maximum_transmission_unit - network_header_bytes_)
        return Admission::invalid_packet;
    const auto decoded = decode_packet(packet);
    if (!decoded || decoded.packet.kind != PacketKind::data
        || decoded.packet.data.retransmitted || decoded.packet.payload.empty()
        || decoded.packet.data.message_number == 0U
        || decoded.packet.data.encryption_key == EncryptionKey::reserved
        || (decoded.packet.data.encryption_key != EncryptionKey::none
            && !generation_owner)
        || decoded.packet.data.destination_socket_id != configuration_.group_id)
        return Admission::invalid_packet;
    RetiredOwners retired {};
    std::size_t retired_count = 0;
    std::unique_lock lock(mutex_);
    if (state_.closed)
        return Admission::closed;
    if (state_.failure != Failure::none)
        return Admission::failed;
    if (generation != configuration_.generation)
        return Admission::wrong_generation;
    if (decoded.packet.data.sequence != state_.accepted_next_sequence)
        return Admission::wrong_sequence;
    if (deadline <= now_locked())
        return Admission::expired;
    if (state_.queued == entries_.size())
        return Admission::full;
    Entry& entry = entries_[(head_ + state_.queued) % entries_.size()];
    std::copy(packet.begin(), packet.end(), entry.bytes.begin());
    entry.size = packet.size();
    entry.sequence = decoded.packet.data.sequence;
    entry.deadline = deadline;
    entry.attempts = 0;
    entry.generation_owner = std::move(generation_owner);
    ++state_.queued;
    state_.queued_bytes += packet.size();
    if (!manual_
        && scheduler_->notify_service(service_)
            != RuntimeScheduler::SubmitStatus::accepted) {
        // Roll back this uncommitted tail before cancelling prior accepted work.
        generation_owner = std::move(entry.generation_owner);
        --state_.queued;
        state_.queued_bytes -= entry.size;
        entry.size = 0;
        fail_locked(Failure::scheduler, 0, retired, retired_count);
        lock.unlock();
        ReadinessSignal::notify(*readiness_);
        return Admission::failed;
    }
    ++state_.accepted_packets;
    state_.accepted_next_sequence = state_.accepted_next_sequence.advanced(1);
    if (state_.queued == entries_.size())
        readiness_->note_not_ready(SRT_EPOLL_OUT);
    return Admission::accepted;
}

void GroupMulticastEmitter::pop_locked(
    RetiredOwners& retired, std::size_t& retired_count) noexcept
{
    auto& entry = entries_[head_];
    retired[retired_count++] = std::move(entry.generation_owner);
    state_.queued_bytes -= entry.size;
    entry.size = 0;
    entry.attempts = 0;
    head_ = (head_ + 1) % entries_.size();
    --state_.queued;
    retry_ready_ = TimePoint::min();
}

void GroupMulticastEmitter::fail_locked(Failure failure, int error,
    RetiredOwners& retired, std::size_t& retired_count) noexcept
{
    state_.failure = failure;
    state_.system_error = error;
    state_.cancelled_packets += state_.queued;
    while (state_.queued != 0U)
        pop_locked(retired, retired_count);
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
            pop_locked(retired, retired_count);
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
            pop_locked(retired, retired_count);
            continue;
        }
        if (sent) {
            fail_locked(Failure::partial_datagram, sent.system_error, retired,
                retired_count);
            break;
        }
        if (sent.error == Error::would_block
            || (sent.error == Error::io_error
                && UdpSocket::is_transient_send_error(sent.system_error))) {
            if (entry.attempts >= configuration_.maximum_attempts) {
                ++state_.retry_exhausted_packets;
                pop_locked(retired, retired_count);
                continue;
            }
            retry_ready_ = add_saturated(now, configuration_.retry_delay);
            break;
        }
        fail_locked(
            Failure::transport, sent.system_error, retired, retired_count);
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
            fail_locked(Failure::scheduler, 0, retired, retired_count);
    }
    lock.unlock();
    ReadinessSignal::notify(*readiness_);
}

GroupMulticastEmitter::Snapshot GroupMulticastEmitter::snapshot() const noexcept
{
    std::lock_guard lock(mutex_);
    auto result = state_;
    result.writable = !state_.closed && state_.failure == Failure::none
        && state_.queued < entries_.size();
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
        while (state_.queued != 0U)
            pop_locked(retired, retired_count);
        channel.swap(configuration_.channel);
        scheduler.swap(scheduler_);
        service = std::exchange(service_, {});
        readiness_->note_not_ready(SRT_EPOLL_OUT);
    }
    if (scheduler && service.valid())
        (void)scheduler->release_service(service);
    ReadinessSignal::notify(*readiness_);
}

void GroupMulticastEmitter::pump_for_testing() noexcept
{
    if (manual_)
        drive();
}

} // namespace robotweax::srt::compat
