#include "compat/group_send_coordinator.hpp"

#include <algorithm>
#include <cassert>
#include <exception>
#include <limits>

namespace robotweax::srt::compat {
using RuntimeAdmission = GroupRuntimeAdmission;
using Emitter = GroupMulticastEmitter;
namespace {
GroupSendCoordinator::Clock::time_point after(
    GroupSendCoordinator::Clock::time_point now,
    GroupSendCoordinator::Clock::duration delay) noexcept
{
    using Clock = GroupSendCoordinator::Clock;
    const auto ticks = now.time_since_epoch().count();
    return ticks > std::numeric_limits<Clock::rep>::max() - delay.count()
        ? Clock::time_point::max()
        : Clock::time_point {Clock::duration {ticks + delay.count()}};
}
}

struct GroupSendCoordinator::Publication {
    GroupSendCoordinator& owner;
    Frame& frame;
    std::shared_ptr<const GroupPacketEncoder::Packet> packet;
    std::array<std::optional<RuntimeAdmission::Guard>, maximum_members> guards;
};

GroupSendCoordinator::GroupSendCoordinator(Configuration c)
    : configuration_(std::move(c))
    , members_(configuration_.member_capacity)
    , work_(std::make_shared<Work>())
    , readiness_(configuration_.readiness ? configuration_.readiness
                                          : std::make_shared<ReadinessSource>())
{
    state_.next_sequence = configuration_.source.initial_sequence;
    state_.next_message = configuration_.initial_message;
    if (configuration_.key_schedule)
        next_position_ = configuration_.key_schedule->frontier_position();
    const auto& source = configuration_.source;
    maximum_payload_ = make_crypto_payload_budget(configuration_.key_schedule
            ? configuration_.key_schedule->mode()
            : CryptoMode::aes_ctr,
        source.maximum_transmission_unit,
        packet_header_size + (source.destination.is_ipv4() ? 28U : 48U))
                           .maximum_plaintext_payload_size;
    frames_.reserve(source.capacity);
    for (std::size_t i = 0; i < source.capacity; ++i) {
        auto frame = std::make_shared<Frame>();
        frame->recipients.resize(configuration_.member_capacity);
        frames_.push_back(std::move(frame));
    }
}

std::shared_ptr<GroupSendCoordinator> GroupSendCoordinator::make(
    Configuration c, std::shared_ptr<RuntimeScheduler> scheduler,
    CryptoProvider& provider, bool manual) noexcept
{
    if ((!manual && !scheduler) || c.member_capacity == 0
        || c.member_capacity > maximum_members || c.source.capacity == 0
        || c.source.capacity > Emitter::maximum_capacity || c.fanout_batch == 0
        || c.fanout_batch > Emitter::maximum_batch
        || c.maximum_busy_attempts == 0 || c.maximum_busy_attempts > 64
        || c.poll_interval <= Clock::duration::zero()
        || c.poll_interval > std::chrono::seconds {1} || c.initial_message == 0
        || c.admission_busy_grace < Clock::duration::zero()
        || c.admission_busy_grace > std::chrono::seconds {5}
        || c.maximum_unicast_members > maximum_members
        || c.initial_message > 0x03ff'ffffU)
        return {};
    try {
        auto actor = std::shared_ptr<GroupSendCoordinator>(
            new GroupSendCoordinator(std::move(c)));
        auto& config = actor->configuration_;
        const auto source_rate = config.source.wire_bytes_per_second;
        if (!source_rate)
            return {};
        const auto recipients =
            std::max<std::size_t>(1, config.maximum_unicast_members);
        const auto rate = config.aggregate_unicast_wire_bytes_per_second
            ? config.aggregate_unicast_wire_bytes_per_second
            : source_rate
                > std::numeric_limits<std::uint64_t>::max() / recipients
            ? std::numeric_limits<std::uint64_t>::max()
            : source_rate * recipients;
        actor->unicast_budget_ = std::make_shared<GroupUnicastBudget>(
            GroupUnicastBudget::Configuration {.wire_bytes_per_second = rate,
                .maximum_wire_datagram =
                    config.source.maximum_transmission_unit,
                .ip_udp_overhead =
                    config.source.destination.is_ipv4() ? 28U : 48U,
                .clock = config.unicast_budget_clock,
                .clock_context = config.unicast_budget_clock_context});
        if (actor->unicast_budget_->snapshot().closed)
            return {};
        // The minimum is bounded by the size_t member limit, also on Win32.
        actor->state_.unicast_recipient_limit =
            static_cast<std::size_t>(std::min<std::uint64_t>(
                config.maximum_unicast_members, rate / source_rate));
        actor->encoder_ = GroupPacketEncoder::create(
            {.group_id = config.source.group_id,
                .stream_generation = config.source.generation,
                .initial_sequence = config.source.initial_sequence,
                .family = config.source.destination.family,
                .maximum_transmission_unit =
                    config.source.maximum_transmission_unit,
                .packet_capacity = config.source.capacity,
                .key_schedule = config.key_schedule},
            provider);
        if (!actor->encoder_)
            return {};
        actor->manual_ = manual;
        actor->work_->owner = actor;
        if (!manual) {
            const auto reserved = scheduler->reserve_service(
                config.source.affinity, {run, actor->work_});
            if (reserved.status != RuntimeScheduler::SubmitStatus::accepted)
                return {};
            actor->scheduler_ = scheduler;
            actor->service_ = reserved.token;
        }
        actor->emitter_ = manual
            ? Emitter::create_for_testing(config.source)
            : Emitter::create(config.source, std::move(scheduler));
        if (!actor->emitter_)
            return {};
        actor->state_.writable = true;
        return actor;
    } catch (...) {
        return {};
    }
}

std::shared_ptr<GroupSendCoordinator> GroupSendCoordinator::create(
    Configuration c, std::shared_ptr<RuntimeScheduler> scheduler,
    CryptoProvider& p) noexcept
{
    return make(std::move(c), std::move(scheduler), p, false);
}
std::shared_ptr<GroupSendCoordinator> GroupSendCoordinator::create_for_testing(
    Configuration c, CryptoProvider& p) noexcept
{
    return make(std::move(c), {}, p, true);
}
GroupSendCoordinator::~GroupSendCoordinator()
{
    close();
}

GroupSendCoordinator::Clock::time_point
GroupSendCoordinator::admission_now() const noexcept
{
    return configuration_.admission_clock
        ? configuration_.admission_clock(configuration_.admission_clock_context)
        : Clock::now();
}

GroupSendCoordinator::Clock::time_point
GroupSendCoordinator::source_now() const noexcept
{
    return configuration_.source.clock
        ? configuration_.source.clock(configuration_.source.clock_context)
        : Clock::now();
}
bool GroupSendCoordinator::source_writable() const noexcept
{
    return state_.source_mode == SourceMode::unicast_only
        || (emitter_ && emitter_->snapshot().report_writable);
}
void GroupSendCoordinator::fail() noexcept
{
    terminal_failure_ = true;
    state_.failed = true;
}
void GroupSendCoordinator::observe_source_failure() noexcept
{
    const auto failure = emitter_->snapshot().failure;
    if (failure != Emitter::Failure::none) {
        state_.source_failure = failure;
        if (state_.source_mode == SourceMode::multicast)
            state_.failed = true;
    }
}

std::optional<GroupSendCoordinator::Member> GroupSendCoordinator::add_member(
    const std::shared_ptr<ConnectionRuntime>& runtime) noexcept
{
    std::lock_guard lock(publication_);
    if (!runtime || state_.closed || state_.failed || next_member_ == 0
        || state_.source_mode != SourceMode::multicast)
        return {};
    Membership* available = nullptr;
    for (auto& entry : members_) {
        if (entry.state.id && entry.runtime.lock() == runtime)
            return {};
        if (!entry.state.id && !available)
            available = &entry;
    }
    if (!available)
        return {};
    auto binding = RuntimeAdmission::bind(runtime,
        {.group_id = configuration_.source.group_id,
            .stream_generation = configuration_.source.generation,
            .timestamp_origin = configuration_.timestamp_origin,
            .family = configuration_.source.destination.family,
            .key_schedule = configuration_.key_schedule,
            .initial_sequence = state_.next_sequence,
            .unicast_budget = unicast_budget_});
    if (!binding)
        return {};
    available->state = {next_member_++, Detachment::none};
    available->state.effective_sequence = state_.next_sequence;
    available->state.effective_position = next_position_;
    available->runtime = runtime;
    available->binding = std::move(binding);
#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_HYBRID_CONTROL
    available->join_position = next_position_;
    available->join_sequence = state_.next_sequence;
#endif
    ++state_.active_members;
    update_readiness();
    return available->state;
}

void GroupSendCoordinator::remove_member(std::uint64_t id) noexcept
{
    RuntimeAdmission::Binding retired;
    bool retired_lease = false;
    {
        std::lock_guard lock(publication_);
        for (auto& entry : members_) {
            if (id == 0 || entry.state.id != id)
                continue;
            if (entry.state.detachment == Detachment::none) {
                --state_.active_members;
                state_.unicast_members -= static_cast<std::size_t>(
                    entry.state.delivery != GroupOriginalDelivery::multicast);
            } else
                --state_.detached_members;
            retired = std::move(entry.binding);
            retired_lease = entry.unicast_lease;
            entry = {};
            update_readiness();
            break;
        }
    }
    RuntimeAdmission::revoke(retired);
    if (retired_lease) {
        std::lock_guard lock(publication_);
        if (!state_.closed)
            --state_.unicast_reservations;
        update_readiness();
    }
}
std::size_t GroupSendCoordinator::members(std::span<Member> out) const noexcept
{
    std::lock_guard lock(publication_);
    std::size_t count = 0;
    for (const auto& entry : members_)
        if (entry.state.id && count < out.size())
            out[count++] = entry.state;
    return count;
}

void GroupSendCoordinator::detach(std::uint64_t id, Detachment reason) noexcept
{
    for (auto& entry : members_)
        if (entry.state.id == id
            && entry.state.detachment == Detachment::none) {
            entry.state.detachment = reason;
            ++entry.state.status_revision;
            state_.unicast_members -= static_cast<std::size_t>(
                entry.state.delivery != GroupOriginalDelivery::multicast);
            --state_.active_members;
            ++state_.detached_members;
            return;
        }
}

GroupSendCoordinator::Transition GroupSendCoordinator::set_delivery(
    std::uint64_t id, std::uint64_t expected, GroupOriginalDelivery mode,
    bool recover) noexcept
{
    RuntimeAdmission::Binding wake;
    Transition result;
    {
        std::lock_guard lock(publication_);
        if (state_.closed)
            return {.status = TransitionStatus::closed};
        if (state_.failed
            || (state_.source_mode == SourceMode::unicast_only
                && mode != GroupOriginalDelivery::unicast)
            || static_cast<unsigned>(mode)
                > static_cast<unsigned>(GroupOriginalDelivery::overlap))
            return {.status = TransitionStatus::unavailable};
        auto found = std::find_if(
            members_.begin(), members_.end(), [id](const auto& entry) {
                return id && entry.state.id == id;
            });
        if (found == members_.end())
            return {};
        auto& member = *found;
        result.member = member.state;
        if (member.state.revision != expected)
            return result;
        if (member.state.detachment != Detachment::none)
            return {.status = TransitionStatus::unavailable,
                .member = member.state};
        if (mode == member.state.delivery && !recover)
            return {
                .status = TransitionStatus::unchanged, .member = member.state};
        if (expected == std::numeric_limits<std::uint64_t>::max())
            return {
                .status = TransitionStatus::exhausted, .member = member.state};
        if (mode != GroupOriginalDelivery::multicast && !member.unicast_lease
            && state_.unicast_reservations >= state_.unicast_recipient_limit)
            return {.status = TransitionStatus::full, .member = member.state};
        if (!manual_
            && scheduler_->notify_service(service_)
                != RuntimeScheduler::SubmitStatus::accepted)
            return {.status = TransitionStatus::unavailable,
                .member = member.state};
        const auto prefix = RuntimeAdmission::recover_prefix(member.binding,
            state_.next_sequence, recover,
            member.unicast_lease
                || state_.unicast_reservations
                    < state_.unicast_recipient_limit);
        if (prefix.status != RuntimeAdmission::Status::ready) {
            result.status = prefix.status == RuntimeAdmission::Status::busy
                ? TransitionStatus::busy
                : prefix.status == RuntimeAdmission::Status::full
                ? TransitionStatus::full
                : TransitionStatus::unavailable;
            return result;
        }
        state_.unicast_members -= static_cast<std::size_t>(
            member.state.delivery != GroupOriginalDelivery::multicast);
        state_.unicast_members +=
            static_cast<std::size_t>(mode != GroupOriginalDelivery::multicast);
        const bool lease =
            mode != GroupOriginalDelivery::multicast || prefix.pending_unicast;
        state_.unicast_reservations -=
            static_cast<std::size_t>(member.unicast_lease);
        state_.unicast_reservations += static_cast<std::size_t>(lease);
        member.unicast_lease = lease;
        member.state.delivery = mode;
#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_HYBRID_CONTROL
        member.overlap_window.reset();
        member.expired_overlap_root = 0;
        member.expired_overlap_revision = 0;
#endif
        member.state.revision = expected + 1;
        member.state.effective_sequence = state_.next_sequence;
        member.state.effective_position = next_position_;
        result = {.status = TransitionStatus::applied,
            .member = member.state,
            .prefix = prefix.prefix,
            .prefix_scheduled = recover};
        wake = member.binding;
        update_readiness();
    }
    RuntimeAdmission::notify_work(wake);
    return result;
}
GroupSendCoordinator::SourceRecovery
GroupSendCoordinator::recover_source_to_unicast(
    std::uint64_t expected, std::span<const RecoveryMember> selected) noexcept
{
    std::array<RuntimeAdmission::Binding, maximum_members> wake;
    std::size_t wake_count = 0;
    std::shared_ptr<Emitter> retired;
    std::array<std::optional<RuntimeAdmission::RecoveryGuard>, maximum_members>
        guards;
    SourceRecovery result;
    {
        std::lock_guard lock(publication_);
        result.revision = state_.source_revision;
        result.effective_sequence = state_.next_sequence;
        result.effective_position = next_position_;
        const auto refused = [&](TransitionStatus status) {
            auto copy = result;
            copy.status = status;
            return copy;
        };
        if (state_.closed)
            return refused(TransitionStatus::closed);
        if (expected != state_.source_revision)
            return refused(TransitionStatus::stale);
        const auto source = emitter_->snapshot();
        if (terminal_failure_ || state_.source_mode != SourceMode::multicast
            || (source.failure != Emitter::Failure::transport
                && source.failure != Emitter::Failure::partial_datagram))
            return refused(TransitionStatus::unavailable);
        if (state_.inflight_frames || source.queued || source.pending_outcomes
            || source.reported_queued)
            return refused(TransitionStatus::busy);
        if (selected.empty() || selected.size() > members_.size())
            return refused(TransitionStatus::invalid);
        if (expected == std::numeric_limits<std::uint64_t>::max())
            return refused(TransitionStatus::exhausted);
        std::array<const RecoveryMember*, maximum_members> requests {};
        std::size_t new_leases = 0;
        for (const auto& request : selected) {
            auto found = std::find_if(
                members_.begin(), members_.end(), [&](const auto& member) {
                    return request.id && member.state.id == request.id;
                });
            if (found == members_.end())
                return refused(TransitionStatus::stale);
            const auto index =
                static_cast<std::size_t>(found - members_.begin());
            if (requests[index])
                return refused(TransitionStatus::invalid);
            if (found->state.revision != request.expected_revision)
                return refused(TransitionStatus::stale);
            if (found->state.detachment != Detachment::none
                && found->state.detachment != Detachment::source_not_submitted)
                return refused(TransitionStatus::unavailable);
            if (found->state.revision
                    == std::numeric_limits<std::uint64_t>::max()
                || found->state.status_revision
                    == std::numeric_limits<std::uint64_t>::max())
                return refused(TransitionStatus::exhausted);
            requests[index] = &request;
            new_leases += static_cast<std::size_t>(!found->unicast_lease);
        }
        if (new_leases > state_.unicast_recipient_limit
                - std::min(state_.unicast_reservations,
                    state_.unicast_recipient_limit))
            return refused(TransitionStatus::full);
        // All runtime locks stay retained until every prefix has passed.
        for (std::size_t i = 0; i < members_.size(); ++i) {
            if (!requests[i])
                continue;
            guards[i].emplace(RuntimeAdmission::try_recover_prefix(
                members_[i].binding, state_.next_sequence));
            const auto status = guards[i]->result().status;
            if (status != RuntimeAdmission::Status::ready)
                return refused(status == RuntimeAdmission::Status::busy
                        ? TransitionStatus::busy
                        : status == RuntimeAdmission::Status::full
                        ? TransitionStatus::full
                        : TransitionStatus::unavailable);
        }
        if (!manual_
            && scheduler_->notify_service(service_)
                != RuntimeScheduler::SubmitStatus::accepted)
            return refused(TransitionStatus::unavailable);
        for (std::size_t i = 0; i < members_.size(); ++i) {
            auto& member = members_[i];
            if (!requests[i]) {
                if (member.state.id
                    && member.state.detachment == Detachment::none)
                    detach(member.state.id, Detachment::source_unavailable);
                continue;
            }
#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_HYBRID_CONTROL
            if (member.hybrid_root && member.unicast_lease
                && member.state.detachment == Detachment::none
                && member.state.delivery == GroupOriginalDelivery::unicast) {
                member.recovered_hybrid_root = member.hybrid_root;
                member.recovered_member_revision = member.state.revision;
                member.recovered_source_revision = expected;
            }
            member.overlap_window.reset();
#endif
            const auto prefix = guards[i]->commit();
            result.prefix.originals += prefix.prefix.originals;
            result.prefix.repairs += prefix.prefix.repairs;
            const auto expiration =
                prefix.prefix.earliest_expiration_microseconds;
            if (expiration
                && (!result.prefix.earliest_expiration_microseconds
                    || expiration
                        < result.prefix.earliest_expiration_microseconds))
                result.prefix.earliest_expiration_microseconds = expiration;
            if (member.state.detachment != Detachment::none) {
                --state_.detached_members;
                ++state_.active_members;
                member.state.detachment = Detachment::none;
            } else {
                state_.unicast_members -= static_cast<std::size_t>(
                    member.state.delivery != GroupOriginalDelivery::multicast);
            }
            ++member.state.status_revision;
            ++state_.unicast_members;
            if (!member.unicast_lease) {
                member.unicast_lease = true;
                ++state_.unicast_reservations;
            }
            member.state.delivery = GroupOriginalDelivery::unicast;
            ++member.state.revision;
            member.state.effective_sequence = state_.next_sequence;
            member.state.effective_position = next_position_;
            member.busy_since.reset();
            wake[wake_count++] = member.binding;
        }
        state_.source_mode = SourceMode::unicast_only;
        state_.source_revision = expected + 1;
        state_.source_effective_sequence = state_.next_sequence;
        state_.source_effective_position = next_position_;
        state_.source_failure = source.failure;
        state_.failed = false;
        retry_at_.reset();
        result.status = TransitionStatus::applied;
        result.revision = state_.source_revision;
        result.members = wake_count;
        retired = emitter_;
        // No emitter/registry operations or notifications while guards live.
        for (auto& guard : guards)
            if (guard)
                guard->release();
        update_readiness();
    }
    // The failed, fully drained emitter cannot create another completion.
    // Retire its reserved service outside actor/runtime locks.
    retired->close();
    for (std::size_t i = 0; i < wake_count; ++i)
        RuntimeAdmission::notify_work(wake[i]);
    return result;
}

void GroupSendCoordinator::advance_cursor() noexcept
{
    ++next_position_;
    state_.next_sequence = state_.next_sequence.next();
    state_.next_message = (state_.next_message + 1U) & 0x03ff'ffffU;
    if (!state_.next_message)
        state_.next_message = 1;
}
bool GroupSendCoordinator::skip(void* p) noexcept
{
    auto& self = *static_cast<GroupSendCoordinator*>(p);
    if (!self.encoder_->advance_unused())
        return false;
    self.advance_cursor();
    ++self.state_.unused_frames;
    return true;
}
bool GroupSendCoordinator::publish(void* p) noexcept
{
    auto& tx = *static_cast<Publication*>(p);
    if (!tx.owner.encoder_->commit(tx.packet))
        return false;
    for (std::size_t i = 0; i < tx.frame.count; ++i) {
        auto ticket = tx.guards[i]->commit();
        // Every guard was staged while retaining its runtime lock. No fallible
        // work, callbacks or capacity changes can intervene in this commit.
        assert(ticket);
        if (!ticket)
            std::terminate();
        tx.frame.recipients[i].ticket = std::move(ticket);
    }
    tx.frame.occupied = true;
#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_HYBRID_CONTROL
    tx.frame.position = tx.owner.next_position_;
#endif
    if (tx.owner.state_.source_mode == SourceMode::unicast_only) {
        tx.frame.completed =
            true; // Consumer retires delegated tickets, never submits DATA.
        ++tx.owner.state_.unicast_only_frames;
    }
    tx.frame.crypto_generation = tx.packet->crypto_generation();
    tx.frame.remaining = tx.frame.count;
    ++tx.owner.state_.inflight_frames;
    ++tx.owner.state_.accepted_frames;
    tx.owner.advance_cursor();
    return true;
}

GroupSendCoordinator::Status GroupSendCoordinator::send(
    std::span<const std::byte> bytes, PacketTimestamp timestamp,
    Clock::time_point deadline, bool in_order) noexcept
{
    // Notify only after all admission guards and emitter reservations die.
    std::array<RuntimeAdmission::Binding, maximum_members> wake;
    std::size_t wake_count = 0;
    struct Wake {
        decltype(wake)& bindings;
        std::size_t& count;
        ~Wake()
        {
            for (std::size_t i = 0; i < count; ++i)
                RuntimeAdmission::notify_work(bindings[i]);
        }
    } notify {wake, wake_count};
    std::lock_guard lock(publication_);
    struct Refresh {
        GroupSendCoordinator& owner;
        ~Refresh()
        {
            owner.update_readiness();
        }
    } refresh {*this};
    if (state_.closed)
        return Status::closed;
    if (state_.failed)
        return Status::failed;
#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_HYBRID_CONTROL
    expire_overlaps_locked(true);
#endif
    if (bytes.empty() || bytes.size() > maximum_payload_)
        return Status::invalid;
    if (retry_at_ && admission_now() < *retry_at_)
        return Status::busy;
    retry_at_.reset();
    // Empty groups advance the same source/key timeline without a datagram.
    if (state_.active_members == 0)
        return advance_empty(deadline);
    if (!crypto_writable())
        return Status::full;
    auto available =
        std::find_if(frames_.begin(), frames_.end(), [](const auto& frame) {
            return !frame->occupied;
        });
    if (available == frames_.end())
        return Status::full;
    const auto admitted = [&]() noexcept -> Status {
        std::optional<Emitter::Reservation> reservation;
        if (state_.source_mode == SourceMode::multicast) {
            reservation.emplace(
                emitter_->reserve(configuration_.source.generation,
                    state_.next_sequence, deadline, true));
            if (reservation->status() != Emitter::Admission::accepted) {
                switch (reservation->status()) {
                case Emitter::Admission::expired:
                    return Status::expired;
                case Emitter::Admission::full:
                    return Status::full;
                case Emitter::Admission::failed:
                    observe_source_failure();
                    if (!state_.failed)
                        fail();
                    return Status::failed;
                default:
                    fail();
                    return Status::failed;
                }
            }
        } else if (deadline <= source_now())
            return Status::expired;
        Frame& frame = **available;
        Publication tx {*this, frame, {}, {}};
        bool retry = false;
        // A member detached below although its session is still established.
        bool lost_connected_member = false;
        frame.count = 0;
        const DataHeader header {.sequence = state_.next_sequence,
            .message_number = state_.next_message,
            .boundary = MessageBoundary::solo,
            .in_order = in_order,
            .timestamp = timestamp,
            .destination_socket_id = configuration_.source.group_id};
        for (auto& member : members_) {
            if (!member.state.id || member.state.detachment != Detachment::none)
                continue;
            wake[wake_count++] = member.binding;
            auto guard = RuntimeAdmission::try_reserve(member.binding, header,
                bytes.size(), deadline, member.state.delivery);
            using S = RuntimeAdmission::Status;
            Detachment reason = Detachment::none;
            switch (guard.status()) {
            case S::ready:
                member.busy_since.reset();
                break;
            case S::busy:
                if (configuration_.admission_busy_grace
                    > Clock::duration::zero()) {
                    const auto now = admission_now();
                    if (!member.busy_since)
                        member.busy_since = now;
                    if (now < after(*member.busy_since,
                            configuration_.admission_busy_grace)) {
                        retry = true;
                        continue;
                    }
                }
                reason = Detachment::admission_busy;
                break;
            case S::full:
                reason = Detachment::history_full;
                break;
            case S::stale:
                reason = Detachment::stale;
                break;
            case S::closed:
                reason = Detachment::closed;
                break;
            case S::invalid:
                reason = Detachment::invalid;
                break;
            case S::expired:
                reason = Detachment::expired;
                break;
            case S::crypto_error:
                reason = Detachment::crypto_error;
                break;
            }
            if (reason != Detachment::none) {
                // A closed or replaced runtime has already left; it is the
                // late discovery of a member that was gone before this call.
                lost_connected_member = lost_connected_member
                    || (reason != Detachment::closed
                        && reason != Detachment::stale);
                detach(member.state.id, reason);
                continue;
            }
            frame.recipients[frame.count] = {.member = member.state.id};
            tx.guards[frame.count++].emplace(std::move(guard));
        }
        if (retry) {
            retry_at_ = after(admission_now(), configuration_.poll_interval);
            if (!manual_
                && scheduler_->schedule_service_at(service_, *retry_at_)
                    != RuntimeScheduler::SubmitStatus::accepted) {
                fail();
                return Status::failed;
            }
            return Status::busy;
        }
        // No member remains for this message. If all of them had already
        // left, the group was empty in effect and the timeline advances
        // unused, as for a group that is empty on entry; the enclosing scope
        // releases source/runtime guards before that advance. If a member
        // whose session is still established was detached here, the message
        // reaches nobody and must not be accepted: report the loss and leave
        // the sequence, message number, source reservation and key position
        // untouched. Such a member may still listen to the multicast group
        // and expects this sequence from the next original.
        if (frame.count == 0)
            return lost_connected_member ? Status::receivers_lost
                                         : Status::advanced_without_receivers;
        if (!manual_
            && scheduler_->notify_service(service_)
                != RuntimeScheduler::SubmitStatus::accepted) {
            fail();
            return Status::failed;
        }
        auto packet = encoder_->prepare(header, bytes);
        if (!packet) {
            if (packet.status == GroupPacketEncoder::Status::full
                || packet.status
                    == GroupPacketEncoder::Status::allocation_failure)
                return Status::full;
            fail();
            return Status::failed;
        }
        tx.packet = packet.packet;
        for (std::size_t i = 0; i < frame.count; ++i)
            if (!tx.guards[i]->stage(tx.packet)) {
                fail();
                return Status::failed;
            }
        const auto result = reservation
            ? reservation->publish(tx.packet->bytes(),
                  tx.packet->generation_owner(), publish, &tx, *available)
            : publish(&tx) ? Emitter::Admission::accepted
                           : Emitter::Admission::failed;
        if (result != Emitter::Admission::accepted) {
            // A sealed identity cannot be abandoned and reused with new plaintext.
            fail();
            return Status::failed;
        }
        // No snapshot calls on emitter/runtime while their guards remain alive.
        for (std::size_t i = 0; i < frame.count; ++i)
            tx.guards[i]->release();
        return Status::accepted;
    }();
    return admitted == Status::advanced_without_receivers
        ? advance_empty(deadline)
        : admitted;
}

GroupSendCoordinator::Status GroupSendCoordinator::advance_empty(
    Clock::time_point deadline) noexcept
{
    if (state_.source_mode == SourceMode::unicast_only) {
        if (deadline <= source_now())
            return Status::expired;
        if (skip(this))
            return Status::advanced_without_receivers;
        fail();
        return Status::failed;
    }
    const auto result =
        emitter_->advance_unused(configuration_.source.generation,
            state_.next_sequence, skip, this, deadline);
    if (result == Emitter::Admission::expired)
        return Status::expired;
    if (result == Emitter::Admission::accepted)
        return Status::advanced_without_receivers;
    if (result == Emitter::Admission::failed)
        observe_source_failure();
    else
        fail();
    return Status::failed;
}

bool GroupSendCoordinator::crypto_writable() const noexcept
{
    const auto& keys = configuration_.key_schedule;
    if (!keys)
        return true;
    const auto next_generation = keys->generation_at(next_position_)
        + static_cast<std::uint64_t>(keys->announcement_due(next_position_));
    // A directional KM update replaces a parity slot. Do not announce its
    // replacement while an older canonical original still needs that slot.
    // Submission/fanout, rather than admission, frees this bounded horizon.
    for (const auto& frame : frames_)
        if (frame->occupied && frame->crypto_generation
            && next_generation >= *frame->crypto_generation + 2U)
            return false;
    return true;
}

void GroupSendCoordinator::update_readiness() noexcept
{
    state_.writable = !state_.closed && !state_.failed
        && (!retry_at_ || admission_now() >= *retry_at_)
        && (state_.active_members == 0
            || (crypto_writable() && source_writable()
                && state_.inflight_frames < frames_.size()));
    if (!state_.writable)
        readiness_->note_not_ready(SRT_EPOLL_OUT);
    ReadinessSignal::notify(*readiness_);
}
GroupSendCoordinator::Snapshot GroupSendCoordinator::snapshot() const noexcept
{
    std::lock_guard lock(publication_);
    auto state = state_;
    if (unicast_budget_)
        state.unicast_budget = unicast_budget_->snapshot();
    state.writable = !state.closed && !state.failed
        && (!retry_at_ || admission_now() >= *retry_at_)
        && (state.active_members == 0
            || (crypto_writable() && source_writable()
                && state.inflight_frames < frames_.size()));
    return state;
}
#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_HYBRID_CONTROL
void GroupSendCoordinator::expire_overlaps_locked(bool admission) noexcept
{
    const auto raw = std::chrono::duration_cast<std::chrono::microseconds>(
        source_now().time_since_epoch())
                         .count();
    for (auto& m : members_)
        if (m.overlap_window) {
            const auto w = *m.overlap_window;
            if (raw >= 0 && static_cast<std::uint64_t>(raw) >= w.opened_us
                && static_cast<std::uint64_t>(raw) < w.not_after_us
                && (next_position_ < w.end_position
                    || (!admission && next_position_ == w.end_position)))
                continue;
            m.overlap_window.reset();
            if (m.state.detachment != Detachment::none
                || m.state.revision != w.revision
                || m.state.delivery != GroupOriginalDelivery::overlap)
                continue;
            m.expired_overlap_root = w.root;
            m.expired_overlap_revision = w.revision;
            m.state.delivery = GroupOriginalDelivery::unicast;
            ++m.state.revision;
            m.state.effective_sequence = state_.next_sequence;
            m.state.effective_position = next_position_;
        }
}
bool GroupSendCoordinator::declare_source_activity(hybrid::Activity activity,
    std::uint64_t produced, std::uint32_t lifetime) noexcept
{
    std::lock_guard lock(publication_);
    if (state_.closed || state_.failed
        || state_.source_mode != SourceMode::multicast)
        return false;
    const auto at = std::chrono::duration_cast<std::chrono::microseconds>(
        source_now().time_since_epoch())
                        .count();
    return at >= 0
        && source_activity_.declare(activity, produced, lifetime,
            static_cast<std::uint64_t>(at), next_position_);
}
std::optional<hybrid::SourceDataProgress> GroupSendCoordinator::data_progress(
    std::uint64_t id) const noexcept
{
    std::lock_guard lock(publication_);
    const bool native_failure = !terminal_failure_
        && (state_.source_failure == Emitter::Failure::transport
            || state_.source_failure == Emitter::Failure::partial_datagram);
    if (state_.closed || (state_.failed && !native_failure))
        return {};
    for (const auto& member : members_) {
        if (!id || member.state.id != id
            || (member.state.detachment != Detachment::none
                && !(native_failure
                    && state_.source_mode == SourceMode::multicast
                    && member.state.detachment
                        == Detachment::source_not_submitted)))
            continue;
        const auto runtime = RuntimeAdmission::observe_data(member.binding);
        if (!runtime)
            return {};
        if (runtime->retained_relative > UINT64_MAX - member.join_position
            || runtime->unicast.next > UINT64_MAX - member.join_position)
            return {};
        auto unicast = runtime->unicast;
        unicast.next += member.join_position;
        const auto when = std::chrono::duration_cast<std::chrono::microseconds>(
            source_now().time_since_epoch())
                              .count();
        return hybrid::SourceDataProgress {
            .stream = configuration_.source.generation,
            .member = id,
            .revision = member.state.revision,
            .member_origin = member.join_position,
            .admitted_next = next_position_,
            .history_floor = member.join_position + runtime->retained_relative,
            .observed_at_us = when < 0 ? 0 : static_cast<std::uint64_t>(when),
            .multicast = multicast_originals_,
            .unicast = unicast,
            .source_failed = native_failure,
            .unicast_only = state_.source_mode == SourceMode::unicast_only,
            .delivery_mode =
                member.state.delivery == GroupOriginalDelivery::multicast
                ? hybrid::Mode::multicast
                : member.state.delivery == GroupOriginalDelivery::unicast
                ? hybrid::Mode::unicast
                : hybrid::Mode::overlap,
            .activity = when < 0 ? std::nullopt
                : native_failure
                ? std::optional {source_activity_.failure(
                      static_cast<std::uint64_t>(when), next_position_)}
                : source_activity_.observe(
                      static_cast<std::uint64_t>(when), next_position_)};
    }
    return {};
}
#endif
void GroupSendCoordinator::run(void* p) noexcept
{
    if (auto self = static_cast<Work*>(p)->owner.lock())
        self->drive();
}
void GroupSendCoordinator::drive() noexcept
{
    std::lock_guard lock(publication_);
    if (state_.closed)
        return;
#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_HYBRID_CONTROL
    expire_overlaps_locked();
#endif
    std::array<Emitter::Outcome, Emitter::maximum_batch> outcomes;
    const auto count = state_.source_mode == SourceMode::multicast
        ? emitter_->take_outcomes(
              std::span {outcomes}.first(configuration_.fanout_batch))
        : 0U;
    for (std::size_t i = 0; i < count; ++i) {
        // Only this actor publishes source contexts; no external emitter access.
        auto frame = std::static_pointer_cast<const Frame>(outcomes[i].context);
        auto found = std::find_if(
            frames_.begin(), frames_.end(), [&](const auto& candidate) {
                return candidate.get() == frame.get();
            });
        assert(found != frames_.end() && frame->occupied && !frame->completed);
        auto& mutable_frame = **found;
#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_HYBRID_CONTROL
        if (outcomes[i].disposition == Emitter::Disposition::submitted) {
            const auto when =
                std::chrono::duration_cast<std::chrono::microseconds>(
                    outcomes[i].completed_at.time_since_epoch())
                    .count();
            if (when >= 0)
                (void)multicast_originals_.note(
                    mutable_frame.position, static_cast<std::uint64_t>(when));
        }
#endif
        outcomes[i].context.reset(); // Avoid Frame -> Outcome -> Frame cycle.
        mutable_frame.outcome = std::move(outcomes[i]);
        mutable_frame.completed = true;
    }
    const auto cells = frames_.size() * members_.size();
    std::size_t attempts = 0;
    for (std::size_t scanned = 0;
        scanned < cells && attempts < configuration_.fanout_batch; ++scanned) {
        const auto index = fanout_cursor_++ % cells;
        auto& frame = *frames_[index / members_.size()];
        auto& recipient = frame.recipients[index % members_.size()];
        if (!frame.occupied || !frame.completed || !recipient.ticket)
            continue;
        ++attempts;
        using A = RuntimeAdmission::Application;
        const bool delegated =
            recipient.ticket->delivery == GroupOriginalDelivery::unicast
            || (recipient.ticket->delivery == GroupOriginalDelivery::overlap
                && frame.outcome.disposition
                    != Emitter::Disposition::submitted);
        const auto result = delegated ? A::duplicate_or_retired
            // A negative source outcome cannot authorize history. Its captured
            // member ID is never reused, so retirement needs no runtime lock.
            : frame.outcome.disposition != Emitter::Disposition::submitted
            ? A::not_submitted
            : RuntimeAdmission::try_apply(*recipient.ticket, frame.outcome);
        if (result == A::busy) {
            // drive() runs on every accepted send as well as on the poll
            // timer, so attempts alone count sends, not time: a burst of
            // sends while the member is inside one poll must not detach it.
            const auto now = admission_now();
            if (!recipient.busy_since)
                recipient.busy_since = now;
            if (recipient.busy_attempts < configuration_.maximum_busy_attempts)
                ++recipient.busy_attempts;
            if (recipient.busy_attempts < configuration_.maximum_busy_attempts
                || (configuration_.admission_busy_grace
                        > Clock::duration::zero()
                    && now < after(*recipient.busy_since,
                           configuration_.admission_busy_grace)))
                continue;
        }
        if (delegated)
            ++state_.delegated_completions;
        else if (result == A::duplicate_or_retired)
            ++state_.duplicate_completions;
        else if (result == A::applied)
            ++state_.applied_completions;
        else {
            ++state_.abandoned_completions;
            if (result == A::busy)
                detach(recipient.member, Detachment::completion_busy);
            else if (result == A::not_submitted)
                detach(recipient.member, Detachment::source_not_submitted);
            else if (result == A::stale)
                detach(recipient.member, Detachment::stale);
            else if (result == A::invalid_time)
                detach(recipient.member, Detachment::invalid_completion);
        }
        recipient.ticket.reset();
        if (--frame.remaining == 0) {
            frame.occupied = false;
            frame.completed = false;
            frame.count = 0;
            frame.outcome = {};
            --state_.inflight_frames;
        }
    }
    observe_source_failure();
    bool retiring_leases = false;
    for (auto& member : members_) {
        if (!member.unicast_lease
            || (member.state.detachment == Detachment::none
                && member.state.delivery != GroupOriginalDelivery::multicast))
            continue;
        const auto pending = RuntimeAdmission::inspect_unicast(member.binding);
        if (pending.status == RuntimeAdmission::Status::busy
            || (pending.status == RuntimeAdmission::Status::ready
                && pending.pending_unicast)) {
            retiring_leases = true;
            continue;
        }
        member.unicast_lease = false;
        --state_.unicast_reservations;
    }
    if (retry_at_ && admission_now() >= *retry_at_)
        retry_at_.reset();
    bool overlap_pending = false;
#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_HYBRID_CONTROL
    overlap_pending =
        std::any_of(members_.begin(), members_.end(), [](const auto& m) {
            return m.overlap_window.has_value();
        });
#endif
    if (!manual_
        && (state_.inflight_frames || retry_at_ || retiring_leases
            || overlap_pending)
        && scheduler_->schedule_service_at(
               service_, after(Clock::now(), configuration_.poll_interval))
            != RuntimeScheduler::SubmitStatus::accepted)
        fail();
    update_readiness();
}
void GroupSendCoordinator::pump_for_testing() noexcept
{
    if (!manual_)
        return;
    emitter_->pump_for_testing();
    drive();
}
void GroupSendCoordinator::close() noexcept
{
    std::array<RuntimeAdmission::Binding, maximum_members> retired;
    std::size_t count = 0;
    {
        std::lock_guard lock(publication_);
        if (state_.closed)
            return;
        state_.closed = true;
        if (unicast_budget_)
            unicast_budget_->close();
        if (emitter_)
            emitter_->close();
        if (encoder_)
            encoder_->close();
        for (auto& member : members_) {
            if (member.binding)
                retired[count++] = std::move(member.binding);
            member = {};
        }
        for (auto& frame : frames_) {
            state_.abandoned_completions += frame->remaining;
            for (auto& recipient : frame->recipients)
                recipient.ticket.reset();
            frame->outcome = {};
            frame->occupied = false;
            frame->remaining = 0;
        }
        state_.active_members = state_.detached_members =
            state_.inflight_frames = 0;
        state_.unicast_members = 0;
        state_.unicast_reservations = 0;
        if (scheduler_ && service_.valid())
            (void)scheduler_->release_service(service_);
        update_readiness();
    }
    for (std::size_t i = 0; i < count; ++i)
        RuntimeAdmission::revoke(retired[i]);
}
} // namespace robotweax::srt::compat
