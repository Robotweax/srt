#include "robotweax/srt/standby_rtt.hpp"

#include <algorithm>
#include <limits>

namespace robotweax::srt {
namespace {
std::uint64_t read_word(const std::byte* p, std::size_t size) noexcept
{
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < size; ++i)
        value = (value << 8) | std::to_integer<unsigned>(p[i]);
    return value;
}
void write_word(std::byte* p, std::uint64_t value, std::size_t size) noexcept
{
    for (std::size_t i = size; i > 0; --i) {
        p[i - 1] = static_cast<std::byte>(value & 255);
        value >>= 8;
    }
}
std::uint64_t after(std::uint64_t now, std::uint64_t delay) noexcept
{
    return now > UINT64_MAX - delay ? UINT64_MAX : now + delay;
}
bool valid_message(const StandbyRttMessage& message) noexcept
{
    return message.identity.is_pair() && message.probe_id != 0
        && (message.kind == StandbyRttKind::request
            || message.kind == StandbyRttKind::echo);
}
}

bool valid_standby_rtt_nonce(const StandbyRttNonce& nonce) noexcept
{
    return std::any_of(nonce.begin(), nonce.end(), [](std::byte b) {
        return b != std::byte {};
    });
}
bool StandbyRttIdentity::is_offer() const noexcept
{
    return valid_standby_rtt_nonce(caller)
        && !valid_standby_rtt_nonce(listener);
}
bool StandbyRttIdentity::is_pair() const noexcept
{
    return valid_standby_rtt_nonce(caller) && valid_standby_rtt_nonce(listener)
        && caller != listener;
}
StandbyRttIdentityResult decode_standby_rtt_identity(
    const HandshakeExtensionView& extension) noexcept
{
    if (extension.type != HandshakeExtensionType::standby_rtt
        || extension.content.size() != standby_rtt_extension_size
        || read_word(extension.content.data(), 4) != 1
        || read_word(extension.content.data() + 4, 4) != 0)
        return {.error = Error::invalid_extension};
    StandbyRttIdentity identity;
    std::copy_n(extension.content.begin() + 8, 16, identity.caller.begin());
    std::copy_n(extension.content.begin() + 24, 16, identity.listener.begin());
    if (!identity.is_offer() && !identity.is_pair())
        return {.error = Error::invalid_extension};
    return {.identity = identity};
}
ExtensionEncodeResult encode_standby_rtt_identity(
    const StandbyRttIdentity& identity,
    std::span<std::byte> destination) noexcept
{
    if (!identity.is_offer() && !identity.is_pair())
        return {.error = Error::invalid_extension};
    std::array<std::byte, standby_rtt_extension_size> content {};
    write_word(content.data(), 1, 4);
    std::copy(
        identity.caller.begin(), identity.caller.end(), content.begin() + 8);
    std::copy(identity.listener.begin(), identity.listener.end(),
        content.begin() + 24);
    return encode_extension(
        HandshakeExtensionType::standby_rtt, content, destination);
}
StandbyRttDecodeResult decode_standby_rtt_datagram(
    std::span<const std::byte> datagram) noexcept
{
    if (datagram.size() != standby_rtt_datagram_size)
        return {.error = Error::invalid_control_payload};
    const auto packet = decode_packet(datagram);
    if (!packet)
        return {.error = packet.error};
    const auto& p = packet.packet;
    if (p.kind != PacketKind::control
        || p.control.type != ControlType::user_defined
        || p.control.subtype != standby_rtt_subtype
        || p.control.type_specific != 0 || read_word(p.payload.data(), 4) != 1)
        return {.error = Error::invalid_control_payload};
    StandbyRttMessage message;
    const auto kind = read_word(p.payload.data() + 4, 4);
    if (kind < 1 || kind > 4)
        return {.error = Error::invalid_control_payload};
    message.caller_initiated = kind <= 2;
    message.kind =
        static_cast<StandbyRttKind>(message.caller_initiated ? kind : kind - 2);
    std::copy_n(p.payload.begin() + 8, 16, message.identity.caller.begin());
    std::copy_n(p.payload.begin() + 24, 16, message.identity.listener.begin());
    message.probe_id = read_word(p.payload.data() + 40, 8);
    if (!valid_message(message))
        return {.error = Error::invalid_control_payload};
    return {.message = message,
        .destination_socket_id = p.control.destination_socket_id};
}
EncodeResult encode_standby_rtt_datagram(const StandbyRttMessage& message,
    PacketTimestamp timestamp, std::uint32_t destination_socket_id,
    std::span<std::byte> destination) noexcept
{
    if (!valid_message(message))
        return {.error = Error::invalid_control_payload};
    std::array<std::byte, 48> payload {};
    write_word(payload.data(), 1, 4);
    write_word(payload.data() + 4,
        static_cast<std::uint32_t>(message.kind)
            + (message.caller_initiated ? 0U : 2U),
        4);
    std::copy(message.identity.caller.begin(), message.identity.caller.end(),
        payload.begin() + 8);
    std::copy(message.identity.listener.begin(),
        message.identity.listener.end(), payload.begin() + 24);
    write_word(payload.data() + 40, message.probe_id, 8);
    return encode_packet(
        {.kind = PacketKind::control,
            .control = {.type = ControlType::user_defined,
                .subtype = standby_rtt_subtype,
                .timestamp = timestamp,
                .destination_socket_id = destination_socket_id},
            .payload = payload},
        destination);
}

Error StandbyRttMeasurement::configure(StandbyRttIdentity identity,
    std::uint64_t generation, std::uint64_t first_due,
    std::uint64_t first_probe_id, bool local_caller) noexcept
{
    *this = {};
    if (!identity.is_pair() || generation == 0 || first_probe_id == 0)
        return Error::invalid_state;
    identity_ = identity;
    generation_ = generation;
    next_due_ = first_due;
    next_id_ = first_probe_id;
    live_ = true;
    local_caller_ = local_caller;
    return Error::none;
}
std::optional<StandbyRttMessage> StandbyRttMeasurement::prepare_request(
    std::uint64_t now, std::uint64_t idle_deadline, std::uint64_t generation,
    bool eligible) noexcept
{
    if (generation != generation_ || !live_)
        return {};
    if (!eligible) {
        pause();
        return {};
    }
    expire(now);
    if (pending_id_ != 0 || now < next_due_ || now >= idle_deadline)
        return {};
    if (next_id_ == UINT64_MAX) {
        sample_.outcome = StandbyRttOutcome::id_exhausted;
        return {};
    }
    pending_id_ = next_id_++;
    prepared_at_ = now;
    idle_deadline_ = idle_deadline;
    deadline_ = std::min(idle_deadline, after(now, 2'000'000));
    next_due_ = after(now, 1'000'000);
    sent_ = false;
    has_early_echo_ = false;
    sample_.outcome = StandbyRttOutcome::pending;
    return StandbyRttMessage {.identity = identity_,
        .probe_id = pending_id_,
        .caller_initiated = local_caller_};
}
void StandbyRttMeasurement::finish_send(std::uint64_t probe_id,
    std::uint64_t generation,
    std::optional<std::uint64_t> native_acceptance_time) noexcept
{
    if (!live_ || generation != generation_ || probe_id == 0
        || probe_id != pending_id_ || sent_)
        return;
    if (!native_acceptance_time || *native_acceptance_time < prepared_at_
        || *native_acceptance_time >= deadline_) {
        pending_id_ = 0;
        sample_.outcome = StandbyRttOutcome::send_skipped;
        return;
    }
    sent_at_ = *native_acceptance_time;
    next_due_ = std::max(next_due_, after(sent_at_, 1'000'000));
    deadline_ = std::min(idle_deadline_, after(sent_at_, 2'000'000));
    sent_ = true;
    if (has_early_echo_)
        record_echo(early_echo_);
}
void StandbyRttMeasurement::record_echo(std::uint64_t arrival) noexcept
{
    if (arrival <= sent_at_) {
        pending_id_ = 0;
        sample_.outcome = StandbyRttOutcome::send_skipped;
        return;
    }
    if (arrival > sent_at_ && arrival < deadline_) {
        sample_.rtt_microseconds = arrival - sent_at_;
        last_sample_at_ = arrival;
        if (sample_.samples != UINT64_MAX)
            ++sample_.samples;
        sample_.outcome = StandbyRttOutcome::measured;
        pending_id_ = 0;
    }
}
void StandbyRttMeasurement::receive_echo(const StandbyRttMessage& message,
    std::uint64_t arrival, std::uint64_t generation, bool live) noexcept
{
    if (!live_ || !live || generation != generation_
        || message.kind != StandbyRttKind::echo
        || message.caller_initiated != local_caller_
        || message.identity != identity_ || pending_id_ == 0
        || message.probe_id != pending_id_)
        return;
    if (arrival < prepared_at_)
        return;
    if (arrival >= deadline_) {
        expire(arrival);
        return;
    }
    if (!sent_) {
        if (!has_early_echo_) {
            early_echo_ = arrival;
            has_early_echo_ = true;
        }
    } else
        record_echo(arrival);
}
std::optional<StandbyRttMessage> StandbyRttMeasurement::make_echo(
    const StandbyRttMessage& message, std::uint64_t now,
    std::uint64_t generation, bool live, bool send_budget) noexcept
{
    if (!live_ || !live || now == UINT64_MAX || generation != generation_
        || !send_budget || !valid_message(message)
        || message.kind != StandbyRttKind::request
        || message.caller_initiated == local_caller_
        || message.identity != identity_ || now < next_echo_due_)
        return {};
    next_echo_due_ = after(now, 1'000'000);
    auto echo = message;
    echo.kind = StandbyRttKind::echo;
    return echo;
}
void StandbyRttMeasurement::expire(std::uint64_t now) noexcept
{
    if (pending_id_ != 0 && now >= deadline_) {
        sample_.outcome = sent_ ? StandbyRttOutcome::timeout
                                : StandbyRttOutcome::send_skipped;
        pending_id_ = 0;
    }
}
void StandbyRttMeasurement::pause() noexcept
{
    if (pending_id_ != 0)
        sample_.outcome = StandbyRttOutcome::paused;
    pending_id_ = 0;
}
void StandbyRttMeasurement::terminate() noexcept
{
    pause();
    live_ = false;
}
StandbyRttSnapshot StandbyRttMeasurement::snapshot(
    std::uint64_t now) const noexcept
{
    auto result = sample_;
    result.age_microseconds = result.samples != 0 && now >= last_sample_at_
        ? now - last_sample_at_
        : UINT64_MAX;
    result.fresh = result.age_microseconds <= 3'000'000;
    return result;
}

bool StandbyRttBudget::owns(Lease lease) const noexcept
{
    return lease.slot < generations_.size() && lease.generation != 0
        && generations_[lease.slot] == lease.generation;
}
std::optional<StandbyRttBudget::Lease> StandbyRttBudget::admit() noexcept
{
    if (serial_ == UINT64_MAX)
        return {};
    for (std::size_t i = 0; i < generations_.size(); ++i) {
        if (generations_[i] == 0) {
            generations_[i] = ++serial_;
            return Lease {
                .generation = serial_, .slot = static_cast<std::uint16_t>(i)};
        }
    }
    return {};
}
void StandbyRttBudget::release(Lease lease) noexcept
{
    if (owns(lease)) {
        generations_[lease.slot] = 0;
        eligible_[lease.slot] = false;
    }
}
void StandbyRttBudget::set_eligible(Lease lease, bool value) noexcept
{
    if (owns(lease))
        eligible_[lease.slot] = value;
}
std::optional<StandbyRttBudget::Lease> StandbyRttBudget::grant_request(
    std::uint64_t now) noexcept
{
    if (now < next_request_ || now == UINT64_MAX)
        return {};
    for (std::size_t scanned = 0; scanned < generations_.size(); ++scanned) {
        const auto i = cursor_;
        cursor_ =
            static_cast<std::uint16_t>((cursor_ + 1) % generations_.size());
        if (generations_[i] != 0 && eligible_[i]) {
            next_request_ = after(now, 31'250);
            return Lease {.generation = generations_[i], .slot = i};
        }
    }
    return {};
}
bool StandbyRttBudget::grant_echo(Lease lease, std::uint64_t now) noexcept
{
    if (!owns(lease) || now < next_echo_ || now == UINT64_MAX)
        return false;
    next_echo_ = after(now, 31'250);
    return true;
}
void StandbyRttBudget::clear() noexcept
{
    generations_.fill(0);
    eligible_.fill(false);
    cursor_ = 0;
    // Do not replenish tokens: repeated cleanup must not create send bursts.
}
} // namespace robotweax::srt
