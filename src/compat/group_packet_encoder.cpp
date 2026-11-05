#include "compat/group_packet_encoder.hpp"

#include "robotweax/srt/codec.hpp"
#include "robotweax/srt/connection_group.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace robotweax::srt::compat {

struct GroupPacketEncoder::Generation {
    explicit Generation(CryptoProvider& p) noexcept
        : provider(p)
    {
    }
    ~Generation()
    {
        provider.secure_erase(salt);
    }
    CryptoProvider& provider;
    std::uint64_t index = 0;
    std::array<std::byte, srt_salt_size> salt {};
    // Provider ciphers own and erase their prepared key material. No mutable
    // schedule slot is borrowed; rotation cannot change this generation.
    std::unique_ptr<PayloadCipher> ctr;
    std::unique_ptr<AuthenticatedPayloadCipher> gcm;
};

std::optional<std::uint64_t>
GroupPacketEncoder::Packet::crypto_generation() const noexcept
{
    return generation_ ? std::optional {generation_->index} : std::nullopt;
}

std::shared_ptr<const void>
GroupPacketEncoder::Packet::generation_owner() const noexcept
{
    return generation_;
}

std::unique_ptr<GroupPacketEncoder> GroupPacketEncoder::create(
    Configuration c, CryptoProvider& provider) noexcept
{
    if ((c.group_id & group_handle_mask) == 0U || c.stream_generation == 0U
        || (c.family != IpAddressFamily::ipv4
            && c.family != IpAddressFamily::ipv6)
        || c.packet_capacity == 0U || c.packet_capacity > maximum_packets
        || c.generation_capacity == 0U
        || c.generation_capacity > maximum_generations
        || (c.key_schedule && !c.key_schedule->initialized()))
        return {};
    const auto mode =
        c.key_schedule ? c.key_schedule->mode() : CryptoMode::aes_ctr;
    if (mode == CryptoMode::aes_gcm && !provider.supports_aes_gcm())
        return {};
    const auto budget = make_crypto_payload_budget(mode,
        c.maximum_transmission_unit,
        packet_header_size + (c.family == IpAddressFamily::ipv4 ? 28U : 48U));
    if (!budget)
        return {};
    try {
        auto encoder = std::unique_ptr<GroupPacketEncoder>(
            new GroupPacketEncoder(std::move(c), provider, budget));
        if (encoder->configuration_.key_schedule
            && encoder->next_position_
                != encoder->configuration_.key_schedule->frontier_position())
            return {};
        return encoder;
    } catch (...) {
        return {};
    }
}

GroupPacketEncoder::GroupPacketEncoder(
    Configuration c, CryptoProvider& provider, CryptoPayloadBudget budget)
    : configuration_(std::move(c))
    , provider_(provider)
    , budget_(budget)
{
    state_.next_sequence = configuration_.initial_sequence;
    if (configuration_.key_schedule)
        next_position_ =
            configuration_.key_schedule->position_of(state_.next_sequence);
}

GroupPacketEncoder::~GroupPacketEncoder()
{
    close();
}

std::shared_ptr<GroupPacketEncoder::Generation>
GroupPacketEncoder::generation_locked(std::uint64_t index, Error& error)
{
    std::size_t available = configuration_.generation_capacity;
    for (std::size_t i = 0; i < configuration_.generation_capacity; ++i) {
        if (generations_[i] && generations_[i]->index == index)
            return generations_[i];
        if (!generations_[i] || generations_[i].use_count() == 1) {
            if (available == configuration_.generation_capacity
                || !generations_[i]
                || (generations_[available]
                    && generations_[i]->index < generations_[available]->index))
                available = i;
        }
    }
    if (available == configuration_.generation_capacity) {
        error = Error::would_block;
        return {};
    }
    // Retire an unpinned cache entry before constructing its replacement:
    // cipher setup must not temporarily exceed the generation budget.
    generations_[available].reset();
    auto generation = std::make_shared<Generation>(provider_);
    generation->index = index;
    std::array<std::byte, maximum_aes_key_size> key {};
    const auto length = configuration_.key_schedule->key_length();
    error = configuration_.key_schedule->key_for(
        index, std::span {key}.first(length));
    if (error == Error::none) {
        configuration_.key_schedule->copy_salt(generation->salt);
        error = configuration_.key_schedule->mode() == CryptoMode::aes_gcm
            ? provider_.make_aes_gcm_cipher(
                  std::span {key}.first(length), generation->gcm)
            : provider_.make_aes_ctr_cipher(
                  std::span {key}.first(length), generation->ctr);
    }
    provider_.secure_erase(key);
    if (error != Error::none)
        return {};
    if (!generation->ctr && !generation->gcm) {
        error = Error::cryptographic_failure;
        return {};
    }
    generations_[available] = generation;
    return generation;
}

GroupPacketEncoder::Result GroupPacketEncoder::prepare(
    const DataHeader& header, std::span<const std::byte> plaintext) noexcept
{
    std::lock_guard lock(mutex_);
    if (state_.closed)
        return {.status = Status::closed};
    if (state_.failure != Error::none)
        return {.status = Status::failed, .error = state_.failure};
    if (header.retransmitted || header.encryption_key != EncryptionKey::none
        || header.destination_socket_id != configuration_.group_id
        || header.message_number == 0U || header.message_number > 0x03ff'ffffU
        || static_cast<unsigned>(header.boundary) > 3U || plaintext.empty()
        || plaintext.size() > budget_.maximum_plaintext_payload_size)
        return {.status = Status::invalid_packet};
    if (header.sequence != state_.next_sequence)
        return {.status = Status::wrong_sequence};
    if (pending_) {
        auto canonical = header;
        canonical.encryption_key = pending_->header().encryption_key;
        std::array<std::byte, packet_header_size> encoded {};
        const auto checked = encode_packet(
            {.kind = PacketKind::data, .data = canonical}, encoded);
        if (!checked
            || !std::equal(
                encoded.begin(), encoded.end(), pending_->bytes().begin())
            || plaintext.size() != pending_plaintext_size_
            || !std::equal(
                plaintext.begin(), plaintext.end(), pending_plaintext_.begin()))
            return {.status = Status::conflicting_packet};
        return {.status = Status::prepared, .packet = pending_};
    }
    std::size_t slot = 0;
    for (; slot < configuration_.packet_capacity; ++slot)
        if (packets_[slot].expired())
            break;
    if (slot == configuration_.packet_capacity)
        return {.status = Status::full};
    try {
        auto packet = std::shared_ptr<Packet>(new Packet);
        packet->header_ = header;
        packet->stream_generation_ = configuration_.stream_generation;
        Error error = Error::none;
        if (configuration_.key_schedule) {
            packet->generation_ = generation_locked(
                configuration_.key_schedule->generation_at(next_position_),
                error);
            if (error == Error::would_block)
                return {.status = Status::full};
            if (error != Error::none) {
                state_.failure = error;
                return {.status = Status::failed, .error = error};
            }
            // Trusted, strictly sequential original preparation only. Reject a
            // schedule frontier incompatible with this stream incarnation.
            if (configuration_.key_schedule->position_of(header.sequence)
                != next_position_) {
                state_.failure = Error::invalid_state;
                return {.status = Status::failed, .error = state_.failure};
            }
            packet->header_.encryption_key =
                SharedTransmitKeySchedule::selector_of(
                    packet->generation_->index);
        }
        const auto encoded =
            encode_packet({.kind = PacketKind::data, .data = packet->header_},
                packet->bytes_);
        if (!encoded) {
            state_.failure = encoded.error;
            return {.status = Status::failed, .error = state_.failure};
        }
        auto payload = std::span {packet->bytes_}.subspan(
            packet_header_size, plaintext.size());
        if (!packet->generation_) {
            std::copy(plaintext.begin(), plaintext.end(), payload.begin());
        } else if (packet->generation_->gcm) {
            auto tag = std::span {packet->bytes_}.subspan(
                packet_header_size + plaintext.size(),
                srt_gcm_authentication_tag_size);
            error = packet->generation_->gcm->seal(
                make_srt_gcm_initialization_vector(
                    header.sequence, packet->generation_->salt),
                make_srt_gcm_additional_authenticated_data(packet->header_),
                plaintext, payload, tag);
        } else {
            error = packet->generation_->ctr->transform(
                make_srt_ctr_initialization_vector(
                    header.sequence, packet->generation_->salt),
                plaintext, payload);
        }
        if (error != Error::none) {
            // A failing provider may have written a prefix. Never publish it
            // or permit a replacement plaintext under a possibly used nonce.
            provider_.secure_erase(packet->bytes_);
            state_.failure = error;
            return {.status = Status::failed, .error = error};
        }
        packet->size_ = packet_header_size + plaintext.size()
            + budget_.authentication_tag_size;
        std::copy(
            plaintext.begin(), plaintext.end(), pending_plaintext_.begin());
        pending_plaintext_size_ = plaintext.size();
        packets_[slot] = packet;
        pending_ = std::move(packet);
        return {.status = Status::prepared, .packet = pending_};
    } catch (...) {
        return {.status = Status::allocation_failure};
    }
}

bool GroupPacketEncoder::commit(
    const std::shared_ptr<const Packet>& packet) noexcept
{
    std::shared_ptr<const Packet> retired;
    {
        std::lock_guard lock(mutex_);
        if (state_.closed || state_.failure != Error::none || !pending_
            || packet != pending_
            || next_position_ == std::numeric_limits<std::uint64_t>::max())
            return false;
        retired = std::move(pending_);
        provider_.secure_erase(pending_plaintext_);
        pending_plaintext_size_ = 0;
        state_.next_sequence = state_.next_sequence.next();
        ++next_position_;
        ++state_.committed_packets;
    }
    return true;
}

GroupPacketEncoder::Snapshot GroupPacketEncoder::snapshot() const noexcept
{
    std::lock_guard lock(mutex_);
    auto result = state_;
    result.prepared = pending_ != nullptr;
    for (std::size_t i = 0; i < configuration_.packet_capacity; ++i)
        result.live_packets += !packets_[i].expired();
    for (const auto& generation : generations_)
        result.cached_generations += generation != nullptr;
    return result;
}

void GroupPacketEncoder::close() noexcept
{
    std::shared_ptr<const Packet> retired;
    std::array<std::shared_ptr<Generation>, maximum_generations> generations;
    std::shared_ptr<SharedTransmitKeySchedule> schedule;
    {
        std::lock_guard lock(mutex_);
        if (state_.closed)
            return;
        state_.closed = true;
        retired = std::move(pending_);
        generations.swap(generations_);
        schedule.swap(configuration_.key_schedule);
        provider_.secure_erase(pending_plaintext_);
        pending_plaintext_size_ = 0;
    }
}

} // namespace robotweax::srt::compat
