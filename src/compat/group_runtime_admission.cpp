#include "compat/group_runtime_admission.hpp"

#include <algorithm>
#include <cassert>
#include <limits>
#include <ratio>
#include <utility>

namespace robotweax::srt::compat {
namespace {
std::optional<std::uint64_t> relative_microseconds(
    ConnectionRuntime::Clock::time_point at,
    ConnectionRuntime::Clock::time_point origin) noexcept
{
    if (at < origin)
        return std::nullopt;
    using Scale =
        std::ratio_divide<ConnectionRuntime::Clock::period, std::micro>;
    const auto ticks = static_cast<std::uint64_t>(at.time_since_epoch().count())
        - static_cast<std::uint64_t>(origin.time_since_epoch().count());
    if (ticks > std::numeric_limits<std::uint64_t>::max() / Scale::num)
        return std::nullopt;
    return ticks * Scale::num / Scale::den;
}
}

bool GroupRuntimeAdmission::active_locked(
    const ConnectionRuntime& runtime, const Binding& binding) noexcept
{
    return binding && runtime.group_send_state_
        && runtime.group_send_state_->binding == binding
        && runtime.group_send_state_->generation == binding->member_generation_;
}

GroupRuntimeAdmission::Binding GroupRuntimeAdmission::bind(
    const std::shared_ptr<ConnectionRuntime>& runtime,
    const Configuration& c) noexcept
{
    if (!runtime || (c.group_id & group_handle_mask) == 0U
        || c.stream_generation == 0U
        || (c.family != IpAddressFamily::ipv4
            && c.family != IpAddressFamily::ipv6)
        || (c.key_schedule && !c.key_schedule->initialized()))
        return {};
    try {
        auto binding = std::shared_ptr<GroupSendBinding>(new GroupSendBinding);
        auto state = std::make_unique<GroupRuntimeSendState>();
        std::unique_lock lock(runtime->mutex_);
        if (runtime->locally_closed_ || runtime->peer_closed_
            || runtime->broken_ || runtime->peer_error_pending_
            || runtime->pending_datagram_size_ != 0U
            || runtime->session_.send_buffer().sequence_span() != 0U
            || (c.initial_sequence
                && *c.initial_sequence
                    != runtime->session_.send_buffer().next_sequence())
            || c.timestamp_origin != runtime->origin_
            || c.family != runtime->peer_.wire_family()
            || !runtime->options_.message_api()
            || runtime->options_.transmission_type() != TransmissionType::live
            || !runtime->options_.packet_filter().empty()
            || runtime->session_.packet_filter_policy().enabled()
            || runtime->options_.experimental_acp1()
            || runtime->options_.session_authentication()
            || runtime->options_.congestion_controller()
                != CongestionController::live)
            return {};
        const bool encrypted = runtime->crypto_ && runtime->crypto_->enabled();
        if (encrypted != (c.key_schedule != nullptr)
            || (encrypted
                && (!runtime->crypto_->uses_shared_transmit_schedule(
                        c.key_schedule)
                    || runtime->crypto_->effective_mode()
                        != c.key_schedule->mode()
                    || runtime->crypto_->sending_without_peer_key())))
            return {};
        if (runtime->group_send_state_
            && runtime->group_send_state_->generation
                == std::numeric_limits<std::uint64_t>::max())
            return {};
        binding->runtime_ = runtime;
        binding->group_id_ = c.group_id;
        binding->stream_generation_ = c.stream_generation;
        binding->schedule_ = c.key_schedule;
        binding->runtime_position_origin_ =
            runtime->session_.send_buffer().next_sequence_position();
        binding->source_position_origin_ = c.key_schedule
            ? c.key_schedule->position_of(
                  runtime->session_.send_buffer().next_sequence())
            : 0;
        binding->mode_ =
            c.key_schedule ? c.key_schedule->mode() : CryptoMode::automatic;
        if (!runtime->group_send_state_)
            runtime->group_send_state_ = std::move(state);
        if (c.key_schedule) {
            const auto position = binding->source_position_origin_;
            runtime->group_send_state_->prepared_key_generation =
                std::max(runtime->group_send_state_->prepared_key_generation,
                    c.key_schedule->generation_at(position)
                        + static_cast<std::uint64_t>(
                            c.key_schedule->announcement_due(position)));
        }
        binding->member_generation_ = ++runtime->group_send_state_->generation;
        runtime->group_send_state_->recovery_key_lease.reset();
        runtime->group_send_state_->binding = binding;
        runtime->group_data_egress_.mode =
            ConnectionRuntime::GroupDataEgress::Mode::external_originals;
        runtime->group_data_egress_.destination_socket_id = c.group_id;
        runtime->note_not_ready(SRT_EPOLL_OUT);
        lock.unlock();
        runtime->notify_readiness();
        return binding;
    } catch (...) {
        return {};
    }
}

void GroupRuntimeAdmission::revoke(const Binding& binding) noexcept
{
    if (!binding)
        return;
    if (auto runtime = binding->runtime_.lock()) {
        std::unique_lock lock(runtime->mutex_);
        if (active_locked(*runtime, binding)) {
            runtime->group_send_state_->binding.reset();
            lock.unlock();
            runtime->notify_readiness();
        }
    }
}

GroupRuntimeAdmission::Guard::Guard(std::shared_ptr<ConnectionRuntime> runtime,
    Binding binding, std::unique_lock<std::mutex> lock, DataHeader header,
    std::size_t size, std::uint64_t now, std::uint64_t expiration,
    std::optional<std::uint64_t> crypto_generation,
    GroupOriginalDelivery delivery) noexcept
    : runtime_(std::move(runtime))
    , binding_(std::move(binding))
    , lock_(std::move(lock))
    , header_(header)
    , plaintext_size_(size)
    , now_(now)
    , expiration_(expiration)
    , crypto_generation_(crypto_generation)
    , status_(Status::ready)
    , delivery_(delivery)
{
}
GroupRuntimeAdmission::Guard::~Guard()
{
    release();
}
GroupRuntimeAdmission::Status
GroupRuntimeAdmission::Guard::status() const noexcept
{
    return status_ == Status::ready && !lock_.owns_lock() ? Status::stale
                                                          : status_;
}
void GroupRuntimeAdmission::Guard::release() noexcept
{
    if (lock_.owns_lock())
        lock_.unlock();
    packet_.reset();
    binding_.reset();
    runtime_.reset();
}

GroupRuntimeAdmission::Guard GroupRuntimeAdmission::try_reserve(
    const Binding& binding, DataHeader original, std::size_t size,
    Clock::time_point expiration, GroupOriginalDelivery delivery) noexcept
{
    if (!binding)
        return Guard {Status::stale};
    auto runtime = binding->runtime_.lock();
    if (!runtime)
        return Guard {Status::closed};
    std::unique_lock lock(runtime->mutex_, std::try_to_lock);
    if (!lock.owns_lock())
        return Guard {Status::busy};
    if (runtime->locally_closed_ || runtime->peer_closed_ || runtime->broken_)
        return Guard {Status::closed};
    if (!active_locked(*runtime, binding))
        return Guard {Status::stale};
    if (runtime->peer_error_pending_
        || static_cast<unsigned>(delivery)
            > static_cast<unsigned>(GroupOriginalDelivery::overlap)
        || original.encryption_key != EncryptionKey::none
        || original.destination_socket_id != binding->group_id_)
        return Guard {Status::invalid};
    if (delivery != GroupOriginalDelivery::multicast
        && runtime->session_.send_buffer().sequence_span() >= 256U)
        return Guard {Status::full};
    auto canonical = original;
    // Shape/capacity preflight does not install keys. Actual parity is chosen
    // only after all immutable fields and available storage have passed.
    canonical.encryption_key =
        binding->schedule_ ? EncryptionKey::even : EncryptionKey::none;
    const auto preflight = runtime->session_.preflight_group_original(
        canonical, size, binding->mode_);
    if (preflight != Error::none)
        return Guard {preflight == Error::buffer_too_small ? Status::full
                                                           : Status::invalid};
    const auto budget =
        runtime->options_.payload_budget(runtime->peer_.wire_family(),
            binding->schedule_ ? binding->mode_ : CryptoMode::aes_ctr);
    if (!budget || size > budget.maximum_plaintext_payload_size)
        return Guard {Status::invalid};
    const auto now = runtime->now_microseconds();
    std::uint64_t expires = 0;
    if (expiration != Clock::time_point::max()) {
        const auto relative =
            relative_microseconds(expiration, runtime->origin_);
        if (!relative || *relative <= now)
            return Guard {Status::expired};
        expires = *relative;
    }
    std::optional<std::uint64_t> crypto_generation;
    if (binding->schedule_) {
        auto& recovery = runtime->group_send_state_->recovery_key_lease;
        const auto& history = runtime->session_.send_buffer();
        if (recovery
            && history.next_sequence_position() - history.sequence_span()
                >= recovery->end_position)
            recovery.reset();
        if (recovery) {
            const auto position =
                binding->schedule_->position_of(original.sequence);
            const auto announced = binding->schedule_->generation_at(position)
                + static_cast<std::uint64_t>(
                    binding->schedule_->announcement_due(position));
            if (announced >= recovery->generation
                && announced - recovery->generation >= 2U)
                return Guard {Status::full};
        }
        if (const auto pending =
                runtime->session_.send_buffer().first_pending_group_unicast()) {
            if (pending->sequence_position < binding->runtime_position_origin_)
                return Guard {Status::invalid};
            const auto delta =
                pending->sequence_position - binding->runtime_position_origin_;
            if (delta > std::numeric_limits<std::uint64_t>::max()
                    - binding->source_position_origin_)
                return Guard {Status::invalid};
            const auto old_generation = binding->schedule_->generation_at(
                binding->source_position_origin_ + delta);
            const auto position =
                binding->schedule_->position_of(original.sequence);
            const auto announced = binding->schedule_->generation_at(position)
                + static_cast<std::uint64_t>(
                    binding->schedule_->announcement_due(position));
            if (announced >= old_generation && announced - old_generation >= 2U)
                return Guard {Status::full};
        }
        if (!runtime->crypto_->ready_to_send_data())
            return Guard {Status::busy};
        // Application admission can outrun member polling across several key
        // boundaries. Do not replace a KM request until the existing unicast
        // control path has captured it (including immutable deferred egress).
        // Its KMRSP is not required for later shared-schedule generations.
        if (!runtime->crypto_->pending_key_material().empty()
            && runtime->crypto_->key_material_revision()
                != runtime->last_key_material_revision_sent_)
            return Guard {Status::busy};
        const auto key_position =
            binding->schedule_->position_of(original.sequence);
        runtime->group_send_state_->prepared_key_generation =
            std::max(runtime->group_send_state_->prepared_key_generation,
                binding->schedule_->generation_at(key_position)
                    + static_cast<std::uint64_t>(
                        binding->schedule_->announcement_due(key_position)));
        if (runtime->crypto_->prepare_data_sequence(original.sequence)
            != Error::none)
            return Guard {Status::crypto_error};
        canonical.encryption_key =
            runtime->crypto_->sender_key_for(original.sequence);
        if (canonical.encryption_key != EncryptionKey::even
            && canonical.encryption_key != EncryptionKey::odd)
            return Guard {Status::crypto_error};
        crypto_generation = binding->schedule_->generation_at(
            binding->schedule_->position_of(original.sequence));
    }
    return Guard {std::move(runtime), binding, std::move(lock), canonical, size,
        now, expires, crypto_generation, delivery};
}

bool GroupRuntimeAdmission::Guard::stage(
    std::shared_ptr<const GroupPacketEncoder::Packet> packet) noexcept
{
    if (!lock_.owns_lock() || committed_ || !packet
        || (packet_ && packet_ != packet)
        || packet->stream_generation() != binding_->stream_generation_
        || packet->crypto_generation() != crypto_generation_
        || !packet->uses_key_schedule(binding_->schedule_))
        return false;
    const auto decoded = decode_packet(packet->bytes());
    if (!decoded || decoded.packet.kind != PacketKind::data)
        return false;
    std::array<std::byte, packet_header_size> expected {};
    const auto encoded =
        encode_packet({.kind = PacketKind::data, .data = header_}, expected);
    const auto tag = binding_->mode_ == CryptoMode::aes_gcm
        ? srt_gcm_authentication_tag_size
        : 0U;
    if (!encoded || decoded.packet.payload.size() != plaintext_size_ + tag
        || !std::equal(
            expected.begin(), expected.end(), packet->bytes().begin()))
        return false;
    packet_ = std::move(packet);
    return true;
}

std::optional<GroupRuntimeAdmission::Ticket>
GroupRuntimeAdmission::Guard::commit() noexcept
{
    if (!lock_.owns_lock() || !packet_ || committed_)
        return std::nullopt;
    const auto decoded = decode_packet(packet_->bytes());
    const auto imported = runtime_->session_.import_group_original(header_,
        decoded.packet.payload, plaintext_size_, binding_->mode_, now_,
        expiration_, delivery_);
    // Stage plus retained runtime lock makes every import condition invariant.
    assert(imported == Error::none);
    if (imported != Error::none)
        return std::nullopt;
    const auto token =
        runtime_->session_.send_buffer().group_original_submission(
            header_.sequence);
    assert(token.has_value());
    runtime_->statistics_.update_send_duration(now_, true);
    runtime_->sample_sender_buffer_statistics(now_);
    if (runtime_->session_.send_buffer().available() == 0U)
        runtime_->note_not_ready(SRT_EPOLL_OUT);
    if (binding_->schedule_)
        runtime_->observe_key_sequence_position(token->sequence_position, now_);
    committed_ = true;
    return Ticket {binding_, *token, delivery_};
}

GroupRuntimeAdmission::Recovery GroupRuntimeAdmission::recover_prefix(
    const Binding& binding, SequenceNumber next, bool apply,
    bool allow) noexcept
{
    if (!binding)
        return {};
    const auto runtime = binding->runtime_.lock();
    if (!runtime)
        return {.status = Status::closed};
    std::unique_lock lock(runtime->mutex_, std::try_to_lock);
    if (!lock.owns_lock())
        return {.status = Status::busy};
    if (!active_locked(*runtime, binding) || runtime->locally_closed_
        || runtime->peer_closed_ || runtime->broken_)
        return {};
    auto result = runtime->session_.group_recovery(next, false);
    const auto& history = runtime->session_.send_buffer();
    std::optional<GroupRuntimeSendState::RecoveryKeyLease> key_lease;
    if (result.error == Error::none && binding->schedule_
        && history.sequence_span()) {
        const auto first =
            history.group_original_submission(history.first_sequence());
        if (!first
            || first->sequence_position < binding->runtime_position_origin_)
            return {.status = Status::invalid};
        const auto delta =
            first->sequence_position - binding->runtime_position_origin_;
        if (delta > std::numeric_limits<std::uint64_t>::max()
                - binding->source_position_origin_)
            return {.status = Status::invalid};
        const auto oldest = binding->schedule_->generation_at(
            binding->source_position_origin_ + delta);
        const auto newest = runtime->group_send_state_->prepared_key_generation;
        // No key rollback on the native path. A retained ciphertext is not a
        // recoverable prefix once a later KM has replaced its parity slot.
        if (newest >= oldest && newest - oldest >= 2U)
            return {.status = Status::invalid};
        key_lease = {.generation = oldest,
            .end_position = history.next_sequence_position()};
    }
    if (result.error == Error::none && apply && result.originals && !allow)
        return {.status = Status::full};
    if (result.error == Error::none && apply) {
        result = runtime->session_.group_recovery(next, true);
        // Explicit replay pins the receiver's parity slots through cumulative
        // retirement of this prefix, including queued/submitted repairs.
        if (key_lease)
            runtime->group_send_state_->recovery_key_lease = key_lease;
    }
    return {.status = result.error == Error::none     ? Status::ready
            : result.error == Error::buffer_too_small ? Status::full
                                                      : Status::invalid,
        .prefix = result,
        .pending_unicast =
            runtime->session_.send_buffer().pending_group_unicast()};
}

GroupRuntimeAdmission::Recovery GroupRuntimeAdmission::inspect_unicast(
    const Binding& binding) noexcept
{
    if (!binding)
        return {};
    auto runtime = binding->runtime_.lock();
    if (!runtime)
        return {.status = Status::closed};
    std::unique_lock lock(runtime->mutex_, std::try_to_lock);
    if (!lock.owns_lock())
        return {.status = Status::busy};
    if (!active_locked(*runtime, binding) || runtime->locally_closed_
        || runtime->peer_closed_ || runtime->broken_)
        return {};
    return {.status = Status::ready,
        .pending_unicast =
            runtime->session_.send_buffer().pending_group_unicast()};
}

void GroupRuntimeAdmission::notify_work(const Binding& binding) noexcept
{
    if (binding) {
        if (auto runtime = binding->runtime_.lock()) {
            runtime->notify_channel_send_work();
            runtime->notify_readiness();
        }
    }
}

GroupRuntimeAdmission::Application GroupRuntimeAdmission::try_apply(
    const Ticket& ticket,
    const GroupMulticastEmitter::Outcome& outcome) noexcept
{
    const auto& binding = ticket.binding;
    if (ticket.delivery == GroupOriginalDelivery::unicast)
        return Application::not_submitted;
    if (!binding || outcome.group_id != binding->group_id_
        || outcome.generation != binding->stream_generation_
        || outcome.sequence != ticket.original.sequence)
        return Application::stale;
    auto runtime = binding->runtime_.lock();
    if (!runtime)
        return Application::stale;
    std::unique_lock lock(runtime->mutex_, std::try_to_lock);
    if (!lock.owns_lock())
        return Application::busy;
    if (!active_locked(*runtime, binding) || runtime->locally_closed_
        || runtime->peer_closed_ || runtime->broken_)
        return Application::stale;
    if (outcome.disposition != GroupMulticastEmitter::Disposition::submitted)
        return Application::not_submitted;
    const auto when =
        relative_microseconds(outcome.completed_at, runtime->origin_);
    if (!when || *when > runtime->now_microseconds())
        return Application::invalid_time;
    if (!runtime->session_.note_group_original_submitted(
            ticket.original, *when))
        return Application::duplicate_or_retired;
    lock.unlock();
    notify_work(binding);
    return Application::applied;
}

} // namespace robotweax::srt::compat
