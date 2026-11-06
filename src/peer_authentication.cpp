// SPDX-License-Identifier: MIT
#include "peer_authentication.hpp"

#include "robotweax/srt/handshake.hpp"

#include <algorithm>
#include <string_view>

namespace robotweax::srt::peer_authentication {
namespace {
constexpr std::string_view label = "RWSRT-PEERAUTH-1";
static_assert(label.size() == 16);

bool zero(std::span<const std::byte> bytes) noexcept
{
    std::byte result {};
    for (const auto value : bytes)
        result |= value;
    return result == std::byte {};
}
bool key_size(std::uint8_t size) noexcept
{
    return size == 16 || size == 24 || size == 32;
}
void copy_label(std::span<std::byte> output) noexcept
{
    for (std::size_t i = 0; i < label.size(); ++i)
        output[i] = std::byte {static_cast<unsigned char>(label[i])};
}
std::uint16_t get16(std::span<const std::byte> input) noexcept
{
    return static_cast<std::uint16_t>(
        (std::to_integer<unsigned>(input[0]) << 8U)
        | std::to_integer<unsigned>(input[1]));
}
void put32(std::span<std::byte> output, std::uint32_t value) noexcept
{
    for (unsigned i = 0; i < 4; ++i)
        output[i] = std::byte(value >> (24U - 8U * i));
}
struct Secret {
    CryptoProvider& provider;
    Digest bytes {};
    explicit Secret(CryptoProvider& p) noexcept
        : provider(p)
    {
    }
    ~Secret()
    {
        provider.secure_erase(bytes);
    }
    Secret(const Secret&) = delete;
    Secret& operator=(const Secret&) = delete;
};
Error expand(CryptoProvider& provider, std::span<const std::byte, 32> root,
    unsigned number, std::span<const std::byte, 32> hash,
    std::span<std::byte, 32> output) noexcept
{
    // RFC 5869 Expand: all outputs fit in the first SHA-256 block.
    std::array<std::byte, 50> info {};
    copy_label(info);
    info[16] = std::byte(number);
    std::copy(hash.begin(), hash.end(), info.begin() + 17);
    info.back() = std::byte {1};
    return provider.hmac_sha256(root, info, output);
}
Error confirm(CryptoProvider& provider, std::span<const std::byte, 32> key,
    std::span<const std::byte, 32> hash,
    std::span<const std::byte, 64> signature,
    std::span<std::byte, 32> output) noexcept
{
    std::array<std::byte, 96> bytes {};
    std::copy(hash.begin(), hash.end(), bytes.begin());
    std::copy(signature.begin(), signature.end(), bytes.begin() + 32);
    const auto error = provider.hmac_sha256(key, bytes, output);
    if (error != Error::none)
        provider.secure_erase(output);
    return error;
}

// Validate framing before copying. Preserve unknown records verbatim; reject
// legacy session-auth and KM records rather than mixing two key schedules.
Error inspect_message(std::span<const std::byte> input, Message expected,
    Record& record, std::size_t& record_offset) noexcept
{
    record_offset = 0;
    if (input.size() > maximum_payload_size)
        return Error::invalid_handshake_size;
    const auto decoded = decode_handshake_payload(input);
    if (!decoded)
        return decoded.error;
    if (decoded.handshake.version != handshake_version_5
        || !(
            decoded.handshake.extension_field & handshake_extension_flag_config)
        || (decoded.handshake.extension_field
            & handshake_extension_flag_key_material)
        || decoded.handshake.request
            != (expected == Message::confirmation
                    ? HandshakeRequest::agreement
                    : HandshakeRequest::conclusion))
        return Error::invalid_handshake_value;
    bool found = false;
    for (std::size_t offset = handshake_size; offset < input.size();) {
        if (input.size() - offset < 4)
            return Error::invalid_extension;
        const auto type = get16(input.subspan(offset, 2));
        const auto length =
            std::size_t(get16(input.subspan(offset + 2, 2))) * 4;
        offset += 4;
        if (length > input.size() - offset)
            return Error::invalid_extension;
        if (type == 3 || type == 4 || type == 0x7f10)
            return Error::invalid_extension;
        if (type == record_type) {
            if (found
                || decode_record(input.subspan(offset, length), record)
                    != Error::none
                || record.message != expected)
                return Error::invalid_extension;
            found = true;
            record_offset = offset;
        }
        offset += length;
    }
    if (!found)
        return Error::invalid_extension;
    return Error::none;
}
Error append_message(std::span<const std::byte> input, Message expected,
    bool zero_proofs, std::span<std::byte> output,
    std::size_t& written) noexcept
{
    written = 0;
    Record record;
    std::size_t record_offset = 0;
    const auto error = inspect_message(input, expected, record, record_offset);
    if (error != Error::none)
        return error;
    const auto proof_offset = expected == Message::offer
        ? 0
        : record_offset + (expected == Message::response ? 136 : 8);
    if (output.size() < 4 + input.size())
        return Error::buffer_too_small;
    put32(output.first(4), static_cast<std::uint32_t>(input.size()));
    std::copy(input.begin(), input.end(), output.begin() + 4);
    if (zero_proofs && proof_offset != 0)
        std::fill_n(output.subspan(4 + proof_offset).begin(), 96, std::byte {});
    written = 4 + input.size();
    return Error::none;
}
Error transcript(CryptoProvider& provider, std::span<const std::byte> offer,
    std::span<const std::byte> response,
    std::span<const std::byte> confirmation, bool caller,
    std::span<std::byte, 32> output) noexcept
{
    // Fixed stack bound: three largest supported handshake payloads + framing.
    std::array<std::byte, 16 + 3 * (4 + maximum_payload_size)> buffer {};
    copy_label(buffer);
    std::size_t used = 16;
    const std::array messages {offer, response, confirmation};
    for (unsigned i = 0; i < (caller ? 3U : 2U); ++i) {
        std::size_t written = 0;
        const auto error =
            append_message(messages[i], static_cast<Message>(i + 1),
                !(caller && i == 1), std::span {buffer}.subspan(used), written);
        if (error != Error::none) {
            provider.secure_erase(output);
            return error;
        }
        used += written;
    }
    const auto error = provider.sha256(std::span {buffer}.first(used), output);
    if (error != Error::none)
        provider.secure_erase(output);
    return error;
}
} // namespace

std::size_t content_size(Message message) noexcept
{
    switch (message) {
    case Message::offer:
        return 136;
    case Message::response:
        return 232;
    case Message::confirmation:
        return 104;
    }
    return 0;
}
Error decode_record(std::span<const std::byte> content, Record& record) noexcept
{
    record = {};
    if (content.size() < 8)
        return Error::invalid_extension;
    const auto message =
        static_cast<Message>(std::to_integer<unsigned>(content[1]));
    const auto length = std::to_integer<std::uint8_t>(content[5]);
    if (content_size(message) != content.size() || content[0] != std::byte {1}
        || content[2] != std::byte {1} || content[3] != std::byte {}
        || content[4] != std::byte {2} || content[6] != std::byte {}
        || content[7] != std::byte {}
        || !(key_size(length) || (message == Message::offer && length == 0)))
        return Error::invalid_extension;
    if (message != Message::confirmation && zero(content.subspan(8, 32)))
        return Error::invalid_extension;
    record.message = message;
    record.key_length = length;
    if (message != Message::confirmation) {
        std::copy_n(content.begin() + 8, 32, record.nonce.begin());
        std::copy_n(content.begin() + 40, 64, record.public_key.begin());
        std::copy_n(content.begin() + 104, 32, record.key_identifier.begin());
    }
    if (message != Message::offer) {
        const auto offset = message == Message::response ? 136 : 8;
        std::copy_n(content.begin() + offset, 64, record.signature.begin());
        std::copy_n(
            content.begin() + offset + 64, 32, record.confirmation.begin());
    }
    return Error::none;
}
Error encode_record(const Record& record, std::span<std::byte> output,
    std::size_t& written) noexcept
{
    written = 0;
    const auto size = content_size(record.message);
    if (size == 0
        || !(key_size(record.key_length)
            || (record.message == Message::offer && record.key_length == 0))
        || (record.message != Message::confirmation && zero(record.nonce)))
        return Error::invalid_extension;
    if (output.size() < size)
        return Error::buffer_too_small;
    std::fill_n(output.begin(), size, std::byte {});
    output[0] = std::byte {1};
    output[1] = std::byte(static_cast<unsigned>(record.message));
    output[2] = std::byte {1};
    output[4] = std::byte {2};
    output[5] = std::byte {record.key_length};
    if (record.message != Message::confirmation) {
        std::copy(record.nonce.begin(), record.nonce.end(), output.begin() + 8);
        std::copy(record.public_key.begin(), record.public_key.end(),
            output.begin() + 40);
        std::copy(record.key_identifier.begin(), record.key_identifier.end(),
            output.begin() + 104);
    }
    if (record.message != Message::offer) {
        const auto offset = record.message == Message::response ? 136 : 8;
        std::copy(record.signature.begin(), record.signature.end(),
            output.begin() + offset);
        std::copy(record.confirmation.begin(), record.confirmation.end(),
            output.begin() + offset + 64);
    }
    written = size;
    return Error::none;
}
Error listener_transcript(CryptoProvider& provider,
    std::span<const std::byte> offer, std::span<const std::byte> response,
    std::span<std::byte, 32> output) noexcept
{
    return transcript(provider, offer, response, {}, false, output);
}
Error caller_transcript(CryptoProvider& provider,
    std::span<const std::byte> offer, std::span<const std::byte> response,
    std::span<const std::byte> confirmation,
    std::span<std::byte, 32> output) noexcept
{
    return transcript(provider, offer, response, confirmation, true, output);
}
KeySchedule::~KeySchedule()
{
    reset();
}
void KeySchedule::reset() noexcept
{
    provider_.secure_erase(listener_key_);
    provider_.secure_erase(chaining_salt_);
    for (auto& key : keys_)
        provider_.secure_erase(key);
    listener_hash_ = {};
    caller_hash_ = {};
    key_length_ = 0;
    state_ = State::empty;
}
Error KeySchedule::start(std::span<const std::byte, 32> shared_secret,
    std::span<const std::byte, 32> listener_hash) noexcept
{
    if (state_ != State::empty) {
        reset();
        return Error::invalid_state;
    }
    Secret hs(provider_);
    const Digest salt {};
    auto error = provider_.hmac_sha256(salt, shared_secret, hs.bytes);
    if (error == Error::none)
        error = expand(provider_, hs.bytes, 1, listener_hash, listener_key_);
    if (error == Error::none)
        error = expand(provider_, hs.bytes, 2, listener_hash, chaining_salt_);
    if (error != Error::none) {
        reset();
        return error;
    }
    std::copy(
        listener_hash.begin(), listener_hash.end(), listener_hash_.begin());
    state_ = State::handshake;
    return Error::none;
}
Error KeySchedule::listener_confirmation(
    std::span<const std::byte, 64> signature,
    std::span<std::byte, 32> output) noexcept
{
    if (state_ != State::handshake) {
        provider_.secure_erase(output);
        return Error::invalid_state;
    }
    const auto error =
        confirm(provider_, listener_key_, listener_hash_, signature, output);
    if (error != Error::none)
        reset();
    return error;
}
Error KeySchedule::derive_authenticated_keys(
    std::span<const std::byte> passphrase,
    std::span<const std::byte, 32> caller_nonce,
    std::span<const std::byte, 32> listener_nonce,
    std::span<const std::byte, 32> caller_hash,
    std::uint8_t key_length) noexcept
{
    if (state_ != State::handshake || passphrase.empty()
        || !key_size(key_length) || zero(caller_nonce)
        || zero(listener_nonce)) {
        reset();
        return Error::invalid_state;
    }
    std::array<std::byte, 80> salt {};
    copy_label(salt);
    std::copy(caller_nonce.begin(), caller_nonce.end(), salt.begin() + 16);
    std::copy(listener_nonce.begin(), listener_nonce.end(), salt.begin() + 48);
    Secret psk(provider_), master(provider_);
    auto error = provider_.pbkdf2_hmac_sha1(passphrase, salt, 2048, psk.bytes);
    if (error == Error::none)
        error = provider_.hmac_sha256(chaining_salt_, psk.bytes, master.bytes);
    // Erase the chaining secret immediately, including provider-error paths.
    provider_.secure_erase(chaining_salt_);
    provider_.secure_erase(psk.bytes);
    provider_.secure_erase(listener_key_);
    for (unsigned i = 0; error == Error::none && i < keys_.size(); ++i)
        error = expand(provider_, master.bytes, i + 3, caller_hash, keys_[i]);
    if (error != Error::none) {
        reset();
        return error;
    }
    // Expand's first block is truncated for AES-128/192 wrapping keys.
    for (unsigned i = 2; i < keys_.size(); ++i)
        provider_.secure_erase(std::span {keys_[i]}.subspan(key_length));
    std::copy(caller_hash.begin(), caller_hash.end(), caller_hash_.begin());
    key_length_ = key_length;
    state_ = State::ready;
    return Error::none;
}
Error KeySchedule::caller_confirmation(std::span<const std::byte, 64> signature,
    std::span<std::byte, 32> output) noexcept
{
    if (state_ != State::ready) {
        provider_.secure_erase(output);
        return Error::invalid_state;
    }
    const auto error =
        confirm(provider_, keys_[0], caller_hash_, signature, output);
    if (error != Error::none)
        reset();
    return error;
}
std::span<const std::byte> KeySchedule::session_key() const noexcept
{
    return state_ == State::ready ? std::span<const std::byte>(keys_[1])
                                  : std::span<const std::byte> {};
}
std::span<const std::byte> KeySchedule::wrapping_key(bool caller) const noexcept
{
    return state_ == State::ready
        ? std::span<const std::byte>(keys_[caller ? 2 : 3]).first(key_length_)
        : std::span<const std::byte> {};
}
namespace {
bool same(
    std::span<const std::byte> left, std::span<const std::byte> right) noexcept
{
    if (left.size() != right.size())
        return false;
    unsigned difference = 0;
    for (std::size_t i = 0; i < left.size(); ++i)
        difference |= std::to_integer<unsigned>(left[i] ^ right[i]);
    return difference == 0;
}
bool same_record(const Record& left, const Record& right) noexcept
{
    std::array<std::byte, 232> a {}, b {};
    std::size_t an = 0, bn = 0;
    return encode_record(left, a, an) == Error::none
        && encode_record(right, b, bn) == Error::none
        && same(std::span {a}.first(an), std::span {b}.first(bn));
}
Error exchange_message(std::span<const std::byte> input, Message message,
    Record& record, std::size_t& offset) noexcept
{
    auto error = inspect_message(input, message, record, offset);
    if (error != Error::none)
        return error;
    const auto handshake = decode_handshake_payload(input).handshake;
    if (handshake.encryption_field != record.key_length / 8
        || handshake.maximum_transmission_unit < 512
        || input.size() > handshake.maximum_transmission_unit - 64)
        return Error::invalid_handshake_value;
    return Error::none;
}
std::span<const std::byte> signed_message(bool caller, const Digest& hash,
    std::array<std::byte, 160>& output) noexcept
{
    const std::string_view context = caller
        ? "Robotweax SRT peer authentication 1, caller"
        : "Robotweax SRT peer authentication 1, listener";
    std::fill_n(output.begin(), 64, std::byte {0x20});
    for (std::size_t i = 0; i < context.size(); ++i)
        output[64 + i] = std::byte(static_cast<unsigned char>(context[i]));
    output[64 + context.size()] = std::byte {};
    std::copy(hash.begin(), hash.end(), output.data() + 65 + context.size());
    return std::span {output}.first(65 + context.size() + hash.size());
}
Error replace_record(std::span<std::byte> output, std::size_t offset,
    const Record& record) noexcept
{
    std::size_t written = 0;
    return encode_record(
        record, output.subspan(offset, content_size(record.message)), written);
}
}
Error validate_payload(
    std::span<const std::byte> input, Message expected) noexcept
{
    Record record;
    std::size_t offset = 0;
    return exchange_message(input, expected, record, offset);
}

void Exchange::Flight::assign(std::span<const std::byte> input) noexcept
{
    std::copy(input.begin(), input.end(), bytes.begin());
    size = input.size();
}
std::span<const std::byte> Exchange::Flight::view() const noexcept
{
    return std::span {bytes}.first(size);
}
Exchange::~Exchange()
{
    abort();
}
void Exchange::abort() noexcept
{
    identity_.reset();
    ephemeral_.reset();
    schedule_.reset();
    for (auto& flight : flights_)
        flight.size = 0;
    peer_ = {};
    state_ = State::failed;
}
Error Exchange::fail(Error error) noexcept
{
    abort();
    return error;
}
Error Exchange::configure(std::span<const std::byte, 32> identity,
    std::span<const std::byte> pins) noexcept
{
    if (state_ != State::fresh)
        return Error::invalid_state;
    if (pins.empty() || pins.size() % 64 != 0 || pins.size() > 16 * 64)
        return fail(Error::invalid_key_material);
    auto error = provider_.import_p256_key(identity, identity_);
    if (error != Error::none || !identity_)
        return fail(
            error == Error::none ? Error::cryptographic_failure : error);
    std::array<std::byte, 64> own_public {};
    error = identity_->public_key(own_public);
    if (error == Error::none)
        error = provider_.sha256(own_public, identity_identifier_);
    if (error != Error::none)
        return fail(error);
    pin_count_ = pins.size() / 64;
    for (std::size_t i = 0; i < pin_count_; ++i) {
        std::copy_n(pins.data() + i * 64, 64, pins_[i].begin());
        error = provider_.validate_p256_public_key(pins_[i]);
        if (error == Error::none)
            error = provider_.sha256(pins_[i], pin_identifiers_[i]);
        if (error != Error::none)
            return fail(error);
    }
    state_ = State::configured;
    return Error::none;
}
Error Exchange::select_peer(const Digest& identifier) noexcept
{
    for (std::size_t i = 0; i < pin_count_; ++i) {
        if (same(identifier, pin_identifiers_[i])) {
            peer_ = pins_[i];
            return Error::none;
        }
    }
    return Error::authentication_failure;
}
Error Exchange::make_share(Record& record) noexcept
{
    auto error = provider_.random_bytes(record.nonce);
    if (error != Error::none || zero(record.nonce))
        return Error::cryptographic_failure;
    error = provider_.generate_p256_key(ephemeral_);
    if (error != Error::none)
        return error;
    if (!ephemeral_)
        return Error::cryptographic_failure;
    record.key_identifier = identity_identifier_;
    return ephemeral_->public_key(record.public_key);
}
Error Exchange::establish(const Record& peer, const Digest& hash) noexcept
{
    if (!ephemeral_)
        return Error::invalid_state;
    Secret shared(provider_);
    auto error = ephemeral_->agree(peer.public_key, shared.bytes);
    ephemeral_.reset();
    if (error == Error::none)
        error = schedule_.start(shared.bytes, hash);
    return error;
}
Error Exchange::prepare_offer(std::uint8_t length, Record& output) noexcept
{
    output = {};
    if (!caller_ || state_ != State::configured)
        return Error::invalid_state;
    if (length != 0 && !key_size(length))
        return fail(Error::invalid_key_material);
    offer_.message = Message::offer;
    offer_.key_length = length;
    auto error = make_share(offer_);
    if (error != Error::none)
        return fail(error);
    output = offer_;
    state_ = State::offer_prepared;
    return Error::none;
}
Error Exchange::bind_offer(std::span<const std::byte> input) noexcept
{
    if (!caller_ || state_ != State::offer_prepared)
        return Error::invalid_state;
    Record record;
    std::size_t offset = 0;
    const auto error = exchange_message(input, Message::offer, record, offset);
    if (error != Error::none)
        return fail(error);
    if (!same_record(record, offer_))
        return fail(Error::invalid_key_material);
    flights_[0].assign(input);
    state_ = State::offered;
    return Error::none;
}
Error Exchange::prepare_response(std::span<const std::byte> input,
    std::uint8_t local_length, Record& output) noexcept
{
    output = {};
    if (caller_)
        return Error::invalid_state;
    if (state_ == State::response_prepared || state_ == State::responded
        || state_ == State::ready) {
        if (!same(input, flights_[0].view()))
            return Error::invalid_state;
        output = response_;
        return Error::none;
    }
    if (state_ != State::configured)
        return Error::invalid_state;
    if (local_length != 0 && !key_size(local_length))
        return fail(Error::invalid_key_material);
    std::size_t offset = 0;
    auto error = exchange_message(input, Message::offer, offer_, offset);
    if (error == Error::none)
        error = select_peer(offer_.key_identifier);
    if (error == Error::none)
        error = provider_.validate_p256_public_key(offer_.public_key);
    if (error != Error::none)
        return fail(error);
    flights_[0].assign(input);
    response_.message = Message::response;
    response_.key_length = offer_.key_length ? offer_.key_length
        : local_length                       ? local_length
                                             : 16;
    error = make_share(response_);
    if (error != Error::none)
        return fail(error);
    output = response_;
    state_ = State::response_prepared;
    return Error::none;
}
Error Exchange::seal_response(std::span<std::byte> input) noexcept
{
    if (caller_)
        return Error::invalid_state;
    if (state_ == State::responded || state_ == State::ready)
        return same(input, flights_[1].view()) ? Error::none
                                               : Error::invalid_state;
    if (state_ != State::response_prepared)
        return Error::invalid_state;
    Record record;
    std::size_t offset = 0;
    auto error = exchange_message(input, Message::response, record, offset);
    if (error != Error::none)
        return fail(error);
    if (!same_record(record, response_))
        return fail(Error::invalid_key_material);
    Digest hash {};
    error = listener_transcript(provider_, flights_[0].view(), input, hash);
    if (error == Error::none)
        error = establish(offer_, hash);
    std::array<std::byte, 160> message {};
    if (error == Error::none)
        error = identity_->sign_sha256(
            signed_message(false, hash, message), response_.signature);
    identity_.reset();
    if (error == Error::none)
        error = schedule_.listener_confirmation(
            response_.signature, response_.confirmation);
    if (error == Error::none)
        error = replace_record(input, offset, response_);
    if (error != Error::none)
        return fail(error);
    flights_[1].assign(input);
    state_ = State::responded;
    return Error::none;
}
Error Exchange::prepare_confirmation(
    std::span<const std::byte> input, Record& output) noexcept
{
    output = {};
    if (!caller_)
        return Error::invalid_state;
    if (state_ == State::confirmation_prepared || state_ == State::ready) {
        if (!same(input, flights_[1].view()))
            return Error::invalid_state;
        output = confirmation_;
        return Error::none;
    }
    if (state_ != State::offered)
        return Error::invalid_state;
    std::size_t offset = 0;
    auto error = exchange_message(input, Message::response, response_, offset);
    if (error != Error::none)
        return fail(error);
    if (offer_.key_length != 0 && response_.key_length != offer_.key_length)
        return fail(Error::invalid_key_material);
    error = select_peer(response_.key_identifier);
    if (error == Error::none)
        error = provider_.validate_p256_public_key(response_.public_key);
    Digest hash {}, tag {};
    if (error == Error::none)
        error = listener_transcript(provider_, flights_[0].view(), input, hash);
    if (error == Error::none)
        error = establish(response_, hash);
    if (error == Error::none)
        error = schedule_.listener_confirmation(response_.signature, tag);
    if (error != Error::none)
        return fail(error);
    if (!same(tag, response_.confirmation))
        return fail(Error::authentication_failure);
    std::array<std::byte, 160> message {};
    error = provider_.verify_p256_sha256(
        peer_, signed_message(false, hash, message), response_.signature);
    if (error != Error::none)
        return fail(error);
    flights_[1].assign(input);
    confirmation_.message = Message::confirmation;
    confirmation_.key_length = response_.key_length;
    output = confirmation_;
    state_ = State::confirmation_prepared;
    return Error::none;
}
Error Exchange::seal_confirmation(
    std::span<std::byte> input, std::span<const std::byte> passphrase) noexcept
{
    if (!caller_)
        return Error::invalid_state;
    if (state_ == State::ready)
        return same(input, flights_[2].view()) ? Error::none
                                               : Error::invalid_state;
    if (state_ != State::confirmation_prepared)
        return Error::invalid_state;
    Record record;
    std::size_t offset = 0;
    auto error = exchange_message(input, Message::confirmation, record, offset);
    if (error != Error::none)
        return fail(error);
    if (!same_record(record, confirmation_))
        return fail(Error::invalid_key_material);
    Digest hash {};
    error = caller_transcript(
        provider_, flights_[0].view(), flights_[1].view(), input, hash);
    if (error == Error::none)
        error = schedule_.derive_authenticated_keys(passphrase, offer_.nonce,
            response_.nonce, hash, response_.key_length);
    std::array<std::byte, 160> message {};
    if (error == Error::none)
        error = identity_->sign_sha256(
            signed_message(true, hash, message), confirmation_.signature);
    identity_.reset();
    if (error == Error::none)
        error = schedule_.caller_confirmation(
            confirmation_.signature, confirmation_.confirmation);
    if (error == Error::none)
        error = replace_record(input, offset, confirmation_);
    if (error != Error::none)
        return fail(error);
    flights_[2].assign(input);
    state_ = State::ready;
    return Error::none;
}
Error Exchange::accept_confirmation(std::span<const std::byte> input,
    std::span<const std::byte> passphrase) noexcept
{
    if (caller_)
        return Error::invalid_state;
    if (state_ == State::ready)
        return same(input, flights_[2].view()) ? Error::none
                                               : Error::invalid_state;
    if (state_ != State::responded)
        return Error::invalid_state;
    Record record;
    std::size_t offset = 0;
    auto error = exchange_message(input, Message::confirmation, record, offset);
    if (error != Error::none)
        return fail(error);
    if (record.key_length != response_.key_length)
        return fail(Error::invalid_key_material);
    Digest hash {}, tag {};
    error = caller_transcript(
        provider_, flights_[0].view(), flights_[1].view(), input, hash);
    if (error != Error::none)
        return fail(error);
    std::array<std::byte, 160> message {};
    error = provider_.verify_p256_sha256(
        peer_, signed_message(true, hash, message), record.signature);
    if (error == Error::authentication_failure && ++invalid_confirmations_ < 4)
        return error;
    if (error != Error::none)
        return fail(error);
    // Never evaluate a passphrase-dependent proof before the pinned signature.
    error = schedule_.derive_authenticated_keys(
        passphrase, offer_.nonce, response_.nonce, hash, response_.key_length);
    if (error == Error::none)
        error = schedule_.caller_confirmation(record.signature, tag);
    if (error != Error::none)
        return fail(error);
    if (!same(tag, record.confirmation))
        return fail(Error::authentication_failure);
    confirmation_ = record;
    flights_[2].assign(input);
    state_ = State::ready;
    return Error::none;
}
bool Exchange::failed() const noexcept
{
    return state_ == State::failed;
}
bool Exchange::ready_for_key_exchange() const noexcept
{
    return state_ == State::ready;
}
std::span<const std::byte> Exchange::cached(Message message) const noexcept
{
    const auto index = static_cast<unsigned>(message);
    return index >= 1 && index <= 3 ? flights_[index - 1].view()
                                    : std::span<const std::byte> {};
}
std::span<const std::byte> Exchange::peer_public_key() const noexcept
{
    return ready_for_key_exchange() ? std::span<const std::byte>(peer_)
                                    : std::span<const std::byte> {};
}
std::span<const std::byte> Exchange::session_key() const noexcept
{
    return ready_for_key_exchange() ? schedule_.session_key()
                                    : std::span<const std::byte> {};
}
std::span<const std::byte> Exchange::wrapping_key(bool caller) const noexcept
{
    return ready_for_key_exchange() ? schedule_.wrapping_key(caller)
                                    : std::span<const std::byte> {};
}

} // namespace robotweax::srt::peer_authentication
