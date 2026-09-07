#include "robotweax/srt/crypto.hpp"

#include "robotweax/srt/codec.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace robotweax::srt {
namespace {

inline constexpr std::byte key_material_first_byte{0x12};
inline constexpr std::uint16_t key_material_signature = 0x2029;
inline constexpr std::size_t pbkdf2_salt_size = 8;

[[nodiscard]] bool valid_aes_key_size(std::size_t size) noexcept
{
    return size == 16U || size == 24U || size == 32U;
}

[[nodiscard]] std::uint16_t read_u16(const std::byte* bytes) noexcept
{
    return static_cast<std::uint16_t>(
        (std::to_integer<std::uint16_t>(bytes[0]) << 8U)
        | std::to_integer<std::uint16_t>(bytes[1]));
}

[[nodiscard]] std::uint32_t read_u32(const std::byte* bytes) noexcept
{
    return (std::to_integer<std::uint32_t>(bytes[0]) << 24U)
        | (std::to_integer<std::uint32_t>(bytes[1]) << 16U)
        | (std::to_integer<std::uint32_t>(bytes[2]) << 8U)
        | std::to_integer<std::uint32_t>(bytes[3]);
}

void write_u16(std::byte* bytes, std::uint16_t value) noexcept
{
    bytes[0] = static_cast<std::byte>((value >> 8U) & 0xffU);
    bytes[1] = static_cast<std::byte>(value & 0xffU);
}

void write_u32(std::byte* bytes, std::uint32_t value) noexcept
{
    bytes[0] = static_cast<std::byte>((value >> 24U) & 0xffU);
    bytes[1] = static_cast<std::byte>((value >> 16U) & 0xffU);
    bytes[2] = static_cast<std::byte>((value >> 8U) & 0xffU);
    bytes[3] = static_cast<std::byte>(value & 0xffU);
}

[[nodiscard]] EncryptionKey other_key(EncryptionKey key) noexcept
{
    return key == EncryptionKey::even
        ? EncryptionKey::odd : EncryptionKey::even;
}

} // namespace

KeyMaterialDecodeResult decode_key_material(
    std::span<const std::byte> bytes) noexcept
{
    if (bytes.size() < key_material_header_size) {
        return {.error = Error::invalid_key_material};
    }
    const auto keys = static_cast<EncryptionKey>(
        std::to_integer<std::uint8_t>(bytes[3]) & 0x03U);
    const auto cipher =
        static_cast<KeyMaterialCipher>(std::to_integer<std::uint8_t>(bytes[8]));
    const auto authentication = static_cast<KeyMaterialAuthentication>(
        std::to_integer<std::uint8_t>(bytes[9]));
    const CryptoSuite* suite =
        crypto_suite_for_key_material(cipher, authentication);
    const std::size_t salt_size =
        static_cast<std::size_t>(
            std::to_integer<std::uint8_t>(bytes[14]))
        * 4U;
    const std::size_t key_size =
        static_cast<std::size_t>(
            std::to_integer<std::uint8_t>(bytes[15]))
        * 4U;
    const std::size_t key_count =
        keys == EncryptionKey::reserved ? 2U : 1U;
    if (bytes[0] != key_material_first_byte
        || read_u16(bytes.data() + 1) != key_material_signature
        || (std::to_integer<std::uint8_t>(bytes[3]) & 0xfcU) != 0U
        || keys == EncryptionKey::none || read_u32(bytes.data() + 4) != 0U
        || suite == nullptr
        || bytes[10] != static_cast<std::byte>(StreamEncapsulation::mpeg_ts_srt)
        || bytes[11] != std::byte {0} || read_u16(bytes.data() + 12) != 0U
        || salt_size != srt_salt_size || !valid_aes_key_size(key_size)) {
        return {.error = Error::invalid_key_material};
    }
    const std::size_t wrapped_size = key_count * key_size + 8U;
    const std::size_t required =
        key_material_header_size + salt_size + wrapped_size;
    if (bytes.size() != required) {
        return {.error = Error::invalid_key_material};
    }
    return {
        .key_material =
            {
                .keys = keys,
                .key_encrypting_key_index = 0,
                .cipher = cipher,
                .authentication = authentication,
                .stream_encapsulation = StreamEncapsulation::mpeg_ts_srt,
                .salt = bytes.subspan(key_material_header_size, salt_size),
                .key_length = key_size,
                .wrapped_keys = bytes.subspan(
                    key_material_header_size + salt_size, wrapped_size),
            },
    };
}

KeyMaterialEncodeResult encode_key_material(
    const KeyMaterialView& key_material,
    std::span<std::byte> destination) noexcept
{
    const CryptoSuite* suite = crypto_suite_for_key_material(
        key_material.cipher, key_material.authentication);
    if ((key_material.keys != EncryptionKey::even
            && key_material.keys != EncryptionKey::odd
            && key_material.keys != EncryptionKey::reserved)
        || key_material.key_encrypting_key_index != 0U || suite == nullptr
        || key_material.stream_encapsulation != StreamEncapsulation::mpeg_ts_srt
        || key_material.salt.size() != srt_salt_size
        || !valid_aes_key_size(key_material.key_length)
        || key_material.wrapped_keys.size()
            != key_material.key_count() * key_material.key_length + 8U) {
        return {.error = Error::invalid_key_material};
    }
    const std::size_t required = key_material_header_size
        + key_material.salt.size()
        + key_material.wrapped_keys.size();
    if (destination.size() < required) {
        return {.error = Error::buffer_too_small};
    }
    auto output = destination.first(required);
    std::fill(output.begin(), output.end(), std::byte{0});
    output[0] = key_material_first_byte;
    write_u16(output.data() + 1, key_material_signature);
    output[3] = static_cast<std::byte>(key_material.keys);
    write_u32(output.data() + 4,
        key_material.key_encrypting_key_index);
    output[8] = static_cast<std::byte>(key_material.cipher);
    output[9] = static_cast<std::byte>(key_material.authentication);
    output[10] =
        static_cast<std::byte>(key_material.stream_encapsulation);
    output[14] = static_cast<std::byte>(
        key_material.salt.size() / 4U);
    output[15] = static_cast<std::byte>(
        key_material.key_length / 4U);
    std::copy(key_material.salt.begin(), key_material.salt.end(),
        output.subspan(key_material_header_size).begin());
    std::copy(key_material.wrapped_keys.begin(),
        key_material.wrapped_keys.end(),
        output.subspan(key_material_header_size
            + key_material.salt.size()).begin());
    return {.bytes_written = required};
}

std::array<std::byte, aes_block_size>
make_srt_ctr_initialization_vector(
    SequenceNumber sequence,
    std::span<const std::byte, srt_salt_size> salt) noexcept
{
    std::array<std::byte, aes_block_size> result{};
    std::copy_n(salt.begin(), 14, result.begin());
    const std::uint32_t value = sequence.value();
    result[10] ^= static_cast<std::byte>((value >> 24U) & 0xffU);
    result[11] ^= static_cast<std::byte>((value >> 16U) & 0xffU);
    result[12] ^= static_cast<std::byte>((value >> 8U) & 0xffU);
    result[13] ^= static_cast<std::byte>(value & 0xffU);
    return result;
}

std::array<std::byte, srt_gcm_initialization_vector_size>
make_srt_gcm_initialization_vector(SequenceNumber sequence,
    std::span<const std::byte, srt_salt_size> salt) noexcept
{
    std::array<std::byte, srt_gcm_initialization_vector_size> result {};
    std::copy_n(salt.begin(), result.size(), result.begin());
    const std::uint32_t value = sequence.value();
    result[8] ^= static_cast<std::byte>((value >> 24U) & 0xffU);
    result[9] ^= static_cast<std::byte>((value >> 16U) & 0xffU);
    result[10] ^= static_cast<std::byte>((value >> 8U) & 0xffU);
    result[11] ^= static_cast<std::byte>(value & 0xffU);
    return result;
}

std::array<std::byte, srt_gcm_additional_data_size>
make_srt_gcm_additional_authenticated_data(const DataHeader& header) noexcept
{
    std::array<std::byte, srt_gcm_additional_data_size> result {};
    DataHeader authenticated_header = header;
    authenticated_header.retransmitted = false;
    const MutablePacketView packet {
        .kind = PacketKind::data,
        .data = authenticated_header,
    };
    [[maybe_unused]] const auto encoded = encode_packet(packet, result);
    return result;
}

CryptoSession::CryptoSession(
    CryptoConfiguration configuration, CryptoProvider& provider) noexcept
    : provider_(provider)
    , configured_mode_(configuration.mode)
    , negotiation_context_(configuration.negotiation_context)
    , aes_gcm_enabled_(configuration.enable_aes_gcm)
    , configured_key_length_(configuration.key_length)
    , refresh_rate_packets_(configuration.refresh_rate_packets)
    , preannouncement_packets_(configuration.preannouncement_packets)
{
    if (configuration.passphrase.size() > passphrase_.size()) {
        configuration_error_ = Error::invalid_state;
        return;
    }
    passphrase_size_ = configuration.passphrase.size();
    for (std::size_t index = 0; index < passphrase_size_; ++index) {
        passphrase_[index] = static_cast<std::byte>(
            static_cast<unsigned char>(
                configuration.passphrase[index]));
    }
    configuration_error_ = validate_configuration();
}

CryptoSession::~CryptoSession()
{
    erase_slot(transmit_even_);
    erase_slot(transmit_odd_);
    erase_slot(receive_even_);
    erase_slot(receive_odd_);
    erase_receive_history(receive_even_history_);
    erase_receive_history(receive_odd_history_);
    provider_.secure_erase(passphrase_);
    provider_.secure_erase(pending_key_material_.bytes);
    provider_.secure_erase(key_material_response_.bytes);
    for (auto& material : received_key_material_history_) {
        provider_.secure_erase(material.bytes);
        material.size = 0;
    }
    for (auto& material : acknowledged_key_material_history_) {
        provider_.secure_erase(material.bytes);
        material.size = 0;
    }
}

Error CryptoSession::validate_configuration() const noexcept
{
    if ((configured_mode_ != CryptoMode::automatic
            && crypto_suite_for_mode(configured_mode_) == nullptr)
        || (negotiation_context_ != CryptoNegotiationContext::caller_listener
            && negotiation_context_ != CryptoNegotiationContext::rendezvous)) {
        return Error::invalid_state;
    }
    if (passphrase_size_ == 0U) {
        return Error::none;
    }
    if (configured_mode_ == CryptoMode::aes_gcm
        && (!aes_gcm_enabled_ || !provider_.supports_aes_gcm())) {
        return Error::unsupported;
    }
    if (passphrase_size_ < minimum_passphrase_size
        || passphrase_size_ > maximum_passphrase_size
        || !valid_aes_key_size(configured_key_length_)
        || refresh_rate_packets_ < 2U
        || refresh_rate_packets_ > maximum_key_refresh_rate
        || preannouncement_packets_ == 0U
        || preannouncement_packets_ > (refresh_rate_packets_ - 1U) / 2U) {
        return Error::invalid_state;
    }
    return Error::none;
}

CryptoSession::KeySlot* CryptoSession::transmit_slot(
    EncryptionKey key) noexcept
{
    if (key == EncryptionKey::even) return &transmit_even_;
    if (key == EncryptionKey::odd) return &transmit_odd_;
    return nullptr;
}

const CryptoSession::KeySlot* CryptoSession::transmit_slot(
    EncryptionKey key) const noexcept
{
    if (key == EncryptionKey::even) return &transmit_even_;
    if (key == EncryptionKey::odd) return &transmit_odd_;
    return nullptr;
}

CryptoSession::KeySlot* CryptoSession::receive_slot(
    EncryptionKey key) noexcept
{
    if (key == EncryptionKey::even) return &receive_even_;
    if (key == EncryptionKey::odd) return &receive_odd_;
    return nullptr;
}

CryptoSession::ReceiveKeyHistory* CryptoSession::receive_history(
    EncryptionKey key) noexcept
{
    if (key == EncryptionKey::even) {
        return &receive_even_history_;
    }
    if (key == EncryptionKey::odd) {
        return &receive_odd_history_;
    }
    return nullptr;
}

bool CryptoSession::history_contains(
    const KeyMaterialHistory& history,
    std::size_t history_size,
    std::span<const std::byte> material) const noexcept
{
    for (std::size_t index = 0; index < history_size; ++index) {
        if (history[index].size == material.size()
            && std::equal(material.begin(), material.end(),
                history[index].bytes.begin())) {
            return true;
        }
    }
    return false;
}

void CryptoSession::copy_key_material(
    KeyMaterialBuffer& destination,
    std::span<const std::byte> material) noexcept
{
    provider_.secure_erase(destination.bytes);
    std::copy(material.begin(), material.end(),
        destination.bytes.begin());
    destination.size = material.size();
}

void CryptoSession::remember_key_material(
    KeyMaterialHistory& history,
    std::size_t& history_size,
    std::span<const std::byte> material) noexcept
{
    const std::size_t last =
        std::min(history_size, history.size() - 1U);
    for (std::size_t index = last; index > 0U; --index) {
        history[index] = history[index - 1U];
    }
    copy_key_material(history[0], material);
    history_size = std::min(history_size + 1U, history.size());
}

void CryptoSession::erase_slot(KeySlot& slot) noexcept
{
    slot.ctr_cipher.reset();
    slot.authenticated_cipher.reset();
    provider_.secure_erase(slot.key);
    provider_.secure_erase(slot.salt);
    slot.key_length = 0;
    slot.mode = CryptoMode::automatic;
}

void CryptoSession::erase_receive_history(
    ReceiveKeyHistory& history) noexcept
{
    for (auto& generation : history.generations) {
        erase_slot(generation.slot);
        generation.sequence_ceiling_known = false;
        generation.sequence_ceiling = {};
    }
    history.size = 0;
    history.discarded_sequence_ceiling_known = false;
    history.discarded_sequence_ceiling = {};
}

void CryptoSession::remember_receive_key(
    ReceiveKeyHistory& history, KeySlot& slot) noexcept
{
    const std::size_t last =
        std::min(history.size, history.generations.size() - 1U);
    if (history.size == history.generations.size()) {
        const auto& discarded = history.generations[last];
        if (discarded.sequence_ceiling_known) {
            history.discarded_sequence_ceiling_known = true;
            history.discarded_sequence_ceiling =
                discarded.sequence_ceiling;
        }
        erase_slot(history.generations[last].slot);
    }
    for (std::size_t index = last; index > 0U; --index) {
        erase_slot(history.generations[index].slot);
        history.generations[index] =
            std::move(history.generations[index - 1U]);
        erase_slot(history.generations[index - 1U].slot);
    }

    auto& newest = history.generations[0];
    erase_slot(newest.slot);
    newest.slot = std::move(slot);
    erase_slot(slot);
    newest.sequence_ceiling_known =
        newest.slot.ready() && highest_receive_sequence_known_;
    if (newest.sequence_ceiling_known) {
        newest.sequence_ceiling = highest_receive_sequence_;
    }
    history.size =
        std::min(history.size + 1U, history.generations.size());
}

Error CryptoSession::install_key(KeySlot& slot, std::span<const std::byte> key,
    std::span<const std::byte, srt_salt_size> salt, CryptoMode mode) noexcept
{
    if (!valid_aes_key_size(key.size())
        || crypto_suite_for_mode(mode) == nullptr) {
        return Error::invalid_key_material;
    }
    std::unique_ptr<PayloadCipher> ctr_cipher;
    std::unique_ptr<AuthenticatedPayloadCipher> authenticated_cipher;
    Error prepared = Error::none;
    if (mode == CryptoMode::aes_ctr) {
        prepared = provider_.make_aes_ctr_cipher(key, ctr_cipher);
    } else if (!provider_.supports_aes_gcm()) {
        prepared = Error::unsupported;
    } else {
        prepared = provider_.make_aes_gcm_cipher(key, authenticated_cipher);
    }
    const bool prepared_cipher_missing = mode == CryptoMode::aes_ctr
        ? ctr_cipher == nullptr
        : authenticated_cipher == nullptr;
    if (prepared != Error::none || prepared_cipher_missing) {
        return prepared == Error::none
            ? Error::cryptographic_failure : prepared;
    }
    erase_slot(slot);
    std::copy(key.begin(), key.end(), slot.key.begin());
    std::copy(salt.begin(), salt.end(), slot.salt.begin());
    slot.key_length = key.size();
    slot.mode = mode;
    slot.ctr_cipher = std::move(ctr_cipher);
    slot.authenticated_cipher = std::move(authenticated_cipher);
    return Error::none;
}

Error CryptoSession::install_receive_key(EncryptionKey key_selection,
    std::span<const std::byte> key,
    std::span<const std::byte, srt_salt_size> salt, CryptoMode mode) noexcept
{
    KeySlot* current = receive_slot(key_selection);
    ReceiveKeyHistory* history = receive_history(key_selection);
    if (current == nullptr || history == nullptr) {
        return Error::invalid_key_material;
    }
    if (current->ready() && current->mode == mode
        && current->key_length == key.size()
        && std::equal(key.begin(), key.end(), current->key.begin())
        && std::equal(salt.begin(), salt.end(), current->salt.begin())) {
        return Error::none;
    }

    KeySlot replacement;
    const Error result = install_key(replacement, key, salt, mode);
    if (result != Error::none) {
        return result;
    }
    if (current->ready()) {
        remember_receive_key(*history, *current);
    } else {
        erase_slot(*current);
    }
    *current = std::move(replacement);
    erase_slot(replacement);
    return Error::none;
}

Error CryptoSession::clone_key(
    const KeySlot& source, KeySlot& destination) noexcept
{
    if (!source.ready()) {
        erase_slot(destination);
        return Error::none;
    }
    return install_key(destination,
        std::span {source.key}.first(source.key_length), source.salt,
        source.mode);
}

Error CryptoSession::clone_transmit_keys_to_receive() noexcept
{
    erase_receive_history(receive_even_history_);
    erase_receive_history(receive_odd_history_);
    highest_receive_sequence_known_ = false;
    Error result = clone_key(transmit_even_, receive_even_);
    if (result != Error::none) return result;
    result = clone_key(transmit_odd_, receive_odd_);
    if (result != Error::none) return result;
    receiver_state_ = CryptoState::secured;
    return Error::none;
}

Error CryptoSession::prepare_bidirectional_sender(
    std::size_t key_length) noexcept
{
    // Legacy peers can use the initial handshake key in either direction.
    // Retain it only for reception and announce fresh local transmit material
    // using ordinary runtime KMREQ/KMRSP before releasing any DATA.
    sender_state_ = CryptoState::securing;
    directional_key_pending_ = true;
    pending_acknowledged_ = false;
    rotation_prepared_ = false;
    packets_on_active_key_ = 0;
    active_sender_key_ = EncryptionKey::even;
    erase_slot(transmit_even_);
    erase_slot(transmit_odd_);
    Error result = generate_initial_sender_key(key_length);
    if (result == Error::none) {
        result = build_sender_key_material(EncryptionKey::even);
    }
    return result;
}

Error CryptoSession::generate_initial_sender_key(
    std::size_t key_length) noexcept
{
    std::array<std::byte, srt_salt_size> salt{};
    std::array<std::byte, maximum_aes_key_size> key{};
    Error result = provider_.random_bytes(salt);
    if (result == Error::none) {
        result = provider_.random_bytes(std::span {key}.first(key_length));
    }
    if (result == Error::none) {
        result = install_key(transmit_even_, std::span {key}.first(key_length),
            salt, effective_mode_);
    }
    provider_.secure_erase(key);
    provider_.secure_erase(salt);
    return result;
}

Error CryptoSession::start_initiator() noexcept
{
    if (configuration_error_ != Error::none) {
        if (configuration_error_ == Error::unsupported) {
            sender_state_ = CryptoState::bad_crypto_mode;
        }
        return configuration_error_;
    }
    if (!enabled()) {
        sender_state_ = CryptoState::unsecured;
        receiver_state_ = CryptoState::unsecured;
        return Error::none;
    }
    if (transmit_even_.ready() || transmit_odd_.ready()) {
        return Error::invalid_state;
    }
    effective_mode_ = configured_mode_ == CryptoMode::automatic
        ? CryptoMode::aes_ctr
        : configured_mode_;
    Error result = generate_initial_sender_key(configured_key_length_);
    if (result == Error::none) {
        active_sender_key_ = EncryptionKey::even;
        result = build_sender_key_material(EncryptionKey::even);
    }
    if (result != Error::none) {
        sender_state_ = result == Error::unsupported
            ? CryptoState::bad_crypto_mode
            : CryptoState::no_secret;
        return result;
    }
    sender_state_ = CryptoState::securing;
    pending_acknowledged_ = false;
    return Error::none;
}

Error CryptoSession::build_sender_key_material(
    EncryptionKey keys) noexcept
{
    const KeySlot* primary = nullptr;
    std::array<std::byte, maximum_aes_key_size * 2U> plaintext{};
    std::size_t plaintext_size = 0;
    if (keys == EncryptionKey::reserved) {
        if (!transmit_even_.ready() || !transmit_odd_.ready()
            || transmit_even_.key_length != transmit_odd_.key_length
            || transmit_even_.mode != transmit_odd_.mode
            || transmit_even_.salt != transmit_odd_.salt) {
            return Error::invalid_state;
        }
        primary = &transmit_even_;
        std::copy_n(transmit_even_.key.begin(),
            transmit_even_.key_length, plaintext.begin());
        std::copy_n(transmit_odd_.key.begin(),
            transmit_odd_.key_length,
            plaintext.begin() + transmit_even_.key_length);
        plaintext_size = transmit_even_.key_length
            + transmit_odd_.key_length;
    } else {
        primary = transmit_slot(keys);
        if (primary == nullptr || !primary->ready()) {
            return Error::invalid_state;
        }
        std::copy_n(primary->key.begin(), primary->key_length,
            plaintext.begin());
        plaintext_size = primary->key_length;
    }

    const CryptoSuite* suite = crypto_suite_for_mode(primary->mode);
    if (suite == nullptr) {
        return Error::invalid_state;
    }

    std::array<std::byte, maximum_aes_key_size> kek{};
    const auto passphrase =
        std::span{passphrase_}.first(passphrase_size_);
    const auto pbkdf_salt = std::span{primary->salt}.last(
        pbkdf2_salt_size);
    Error result = provider_.pbkdf2_hmac_sha1(
        passphrase, pbkdf_salt, srt_pbkdf2_iterations,
        std::span{kek}.first(primary->key_length));
    std::array<std::byte, maximum_wrapped_key_size> wrapped{};
    std::size_t wrapped_size = 0;
    if (result == Error::none) {
        result = provider_.wrap_key(
            std::span{kek}.first(primary->key_length),
            std::span{plaintext}.first(plaintext_size),
            wrapped, wrapped_size);
    }
    if (result == Error::none) {
        const KeyMaterialView view {
            .keys = keys,
            .cipher = suite->cipher,
            .authentication = suite->authentication,
            .salt = primary->salt,
            .key_length = primary->key_length,
            .wrapped_keys = std::span {wrapped}.first(wrapped_size),
        };
        const auto encoded = encode_key_material(
            view, pending_key_material_.bytes);
        result = encoded.error;
        pending_key_material_.size =
            encoded ? encoded.bytes_written : 0U;
        key_material_pending_ = static_cast<bool>(encoded);
    }
    provider_.secure_erase(kek);
    provider_.secure_erase(plaintext);
    provider_.secure_erase(wrapped);
    return result;
}

Error CryptoSession::accept_key_material(
    std::span<const std::byte> request,
    bool clone_for_bidirectional_sender) noexcept
{
    const bool preserve_secured_state =
        receiver_state_ == CryptoState::secured
        || receiver_state_ == CryptoState::securing;
    const auto reject = [&](Error error,
                            CryptoState state) noexcept {
        if (!preserve_secured_state) {
            receiver_state_ = state;
        }
        return error;
    };
    if (configuration_error_ != Error::none) {
        return reject(configuration_error_,
            configuration_error_ == Error::unsupported
                ? CryptoState::bad_crypto_mode
                : CryptoState::no_secret);
    }
    if (!enabled()) {
        receiver_state_ = CryptoState::no_secret;
        return Error::cryptographic_failure;
    }
    if (history_contains(received_key_material_history_,
            received_key_material_history_size_, request)) {
        copy_key_material(key_material_response_, request);
        return Error::none;
    }
    const auto decoded = decode_key_material(request);
    if (!decoded) {
        return reject(
            decoded.error, CryptoState::bad_crypto_mode);
    }
    const auto& material = decoded.key_material;
    const CryptoSuite* suite =
        crypto_suite_for_key_material(material.cipher, material.authentication);
    const CryptoModeSelection selection = suite == nullptr
        ? CryptoModeSelection {}
        : negotiate_crypto_mode(
              negotiation_context_, suite->mode, configured_mode_);
    if (!selection
        || (effective_mode_ != CryptoMode::automatic
            && effective_mode_ != selection.effective_mode)
        || (selection.effective_mode == CryptoMode::aes_gcm
            && (!aes_gcm_enabled_ || !provider_.supports_aes_gcm()))) {
        return reject(Error::unsupported, CryptoState::bad_crypto_mode);
    }
    std::array<std::byte, maximum_aes_key_size> kek{};
    const auto pbkdf_salt = material.salt.last(pbkdf2_salt_size);
    Error result = provider_.pbkdf2_hmac_sha1(
        std::span{passphrase_}.first(passphrase_size_),
        pbkdf_salt, srt_pbkdf2_iterations,
        std::span{kek}.first(material.key_length));
    std::array<std::byte, maximum_aes_key_size * 2U> plaintext{};
    std::size_t plaintext_size = 0;
    if (result == Error::none) {
        result = provider_.unwrap_key(
            std::span{kek}.first(material.key_length),
            material.wrapped_keys, plaintext, plaintext_size);
    }
    if (result != Error::none
        || plaintext_size
            != material.key_count() * material.key_length) {
        provider_.secure_erase(kek);
        provider_.secure_erase(plaintext);
        return reject(Error::cryptographic_failure,
            CryptoState::bad_secret);
    }
    std::array<std::byte, srt_salt_size> salt{};
    std::copy(material.salt.begin(), material.salt.end(),
        salt.begin());
    if (material.keys == EncryptionKey::even
        || material.keys == EncryptionKey::reserved) {
        result = install_receive_key(EncryptionKey::even,
            std::span {plaintext}.first(material.key_length), salt,
            selection.effective_mode);
    }
    if (result == Error::none
        && (material.keys == EncryptionKey::odd
            || material.keys == EncryptionKey::reserved)) {
        const std::size_t offset =
            material.keys == EncryptionKey::reserved
            ? material.key_length : 0U;
        result = install_receive_key(EncryptionKey::odd,
            std::span {plaintext}.subspan(offset, material.key_length), salt,
            selection.effective_mode);
    }
    provider_.secure_erase(kek);
    provider_.secure_erase(plaintext);
    provider_.secure_erase(salt);
    if (result != Error::none) {
        return reject(result,
            result == Error::unsupported ? CryptoState::bad_crypto_mode
                                         : CryptoState::no_secret);
    }
    effective_mode_ = selection.effective_mode;
    receiver_state_ = CryptoState::secured;
    if (clone_for_bidirectional_sender
        && !transmit_even_.ready()
        && !transmit_odd_.ready()) {
        result = prepare_bidirectional_sender(key_length());
        if (result != Error::none) {
            return result;
        }
    }
    remember_key_material(received_key_material_history_,
        received_key_material_history_size_, request);
    copy_key_material(key_material_response_, request);
    return Error::none;
}

Error CryptoSession::acknowledge_key_material(
    std::span<const std::byte> response,
    bool clone_for_bidirectional_receiver) noexcept
{
    // A confirmed encrypted handshake must not downgrade while confirming
    // directional keys or rotating established keys. Only the original
    // optional-encryption negotiation may accept NOSECRET as a fallback.
    const bool preserve_secured_state = sender_state_ == CryptoState::secured
        || (sender_state_ == CryptoState::securing
            && (rotation_prepared_ || directional_key_pending_));
    if (response.size() == 4U) {
        const std::uint32_t state = read_u32(response.data());
        if (!preserve_secured_state) {
            sender_state_ = state <= static_cast<std::uint32_t>(
                    CryptoState::bad_crypto_mode)
                ? static_cast<CryptoState>(state)
                : CryptoState::bad_secret;
        }
        return Error::cryptographic_failure;
    }
    if (history_contains(acknowledged_key_material_history_,
            acknowledged_key_material_history_size_, response)) {
        return Error::none;
    }
    if (pending_key_material_.size == 0U
        || response.size() != pending_key_material_.size
        || !std::equal(response.begin(), response.end(),
            pending_key_material_.bytes.begin())) {
        if (!preserve_secured_state) {
            sender_state_ = CryptoState::bad_secret;
        }
        return Error::invalid_key_material;
    }
    if (pending_acknowledged_ && !key_material_pending_) {
        return Error::none;
    }
    pending_acknowledged_ = true;
    key_material_pending_ = false;
    sender_state_ = CryptoState::secured;
    directional_key_pending_ = false;
    remember_key_material(acknowledged_key_material_history_,
        acknowledged_key_material_history_size_, response);
    if (clone_for_bidirectional_receiver
        && !receive_even_.ready()
        && !receive_odd_.ready()) {
        sender_state_ = CryptoState::securing;
        directional_key_pending_ = true;
        const std::size_t sender_key_length = key_length();
        const Error result = clone_transmit_keys_to_receive();
        if (result != Error::none)
            return result;
        remember_key_material(received_key_material_history_,
            received_key_material_history_size_, response);
        return prepare_bidirectional_sender(sender_key_length);
    }
    return Error::none;
}

Error CryptoSession::generate_next_sender_key() noexcept
{
    const EncryptionKey next = other_key(active_sender_key_);
    const KeySlot* current = transmit_slot(active_sender_key_);
    KeySlot* destination = transmit_slot(next);
    if (current == nullptr || destination == nullptr
        || !current->ready()) {
        return Error::invalid_state;
    }
    std::array<std::byte, maximum_aes_key_size> key{};
    Error result = provider_.random_bytes(
        std::span{key}.first(current->key_length));
    if (result == Error::none) {
        result = install_key(*destination,
            std::span {key}.first(current->key_length), current->salt,
            current->mode);
    }
    provider_.secure_erase(key);
    return result;
}

Error CryptoSession::prepare_rotation() noexcept
{
    if (!enabled()) return Error::none;
    if (sender_state_ != CryptoState::secured
        && sender_state_ != CryptoState::securing) {
        return Error::invalid_state;
    }
    const std::uint64_t announcement_at =
        static_cast<std::uint64_t>(refresh_rate_packets_)
        - preannouncement_packets_;
    if (rotation_prepared_
        || packets_on_active_key_ < announcement_at) {
        return Error::none;
    }
    Error result = generate_next_sender_key();
    if (result == Error::none) {
        result = build_sender_key_material(
            EncryptionKey::reserved);
    }
    if (result != Error::none) {
        return result;
    }
    rotation_prepared_ = true;
    pending_acknowledged_ = false;
    sender_state_ = CryptoState::securing;
    return Error::none;
}

Error CryptoSession::note_data_packet_sent() noexcept
{
    if (!enabled()) return Error::none;
    if (packets_on_active_key_
        == std::numeric_limits<std::uint64_t>::max()) {
        return Error::cryptographic_failure;
    }
    ++packets_on_active_key_;
    if (packets_on_active_key_ < refresh_rate_packets_) {
        return Error::none;
    }
    if (!rotation_prepared_ || !pending_acknowledged_) {
        return Error::cryptographic_failure;
    }
    active_sender_key_ = other_key(active_sender_key_);
    packets_on_active_key_ = 0;
    rotation_prepared_ = false;
    pending_acknowledged_ = false;
    sender_state_ = CryptoState::secured;
    return Error::none;
}

bool CryptoSession::ready_to_send_data() const noexcept
{
    if (!enabled()) {
        return true;
    }
    if (effective_mode_ == CryptoMode::aes_gcm) {
        if (!authenticated_data_enabled()) {
            return false;
        }
    }
    if (sender_state_ != CryptoState::secured
        && sender_state_ != CryptoState::securing) {
        return false;
    }
    // Runtime key negotiation can begin after the transport handshake.
    // SECURING therefore does not imply that a peer owns the first key. A
    // prepared rotation is different: its already-acknowledged active key
    // remains usable until the refresh boundary.
    if (sender_state_ == CryptoState::securing
        && !rotation_prepared_) {
        return false;
    }
    const std::uint64_t next_packet_count =
        packets_on_active_key_ + 1U;
    return next_packet_count < refresh_rate_packets_
        || (rotation_prepared_ && pending_acknowledged_);
}

std::size_t CryptoSession::key_length() const noexcept
{
    const KeySlot* sender = transmit_slot(active_sender_key_);
    if (sender != nullptr && sender->ready()) {
        return sender->key_length;
    }
    if (receive_even_.ready()) {
        return receive_even_.key_length;
    }
    if (receive_odd_.ready()) {
        return receive_odd_.key_length;
    }
    return enabled() ? configured_key_length_ : 0U;
}

Error CryptoSession::encrypt(
    SequenceNumber sequence,
    std::span<const std::byte> plaintext,
    std::span<std::byte> ciphertext,
    EncryptionKey& key) noexcept
{
    if (!enabled()) {
        if (ciphertext.size() < plaintext.size()) {
            return Error::buffer_too_small;
        }
        std::copy(plaintext.begin(), plaintext.end(),
            ciphertext.begin());
        key = EncryptionKey::none;
        return Error::none;
    }
    if (sender_state_ != CryptoState::secured
        && sender_state_ != CryptoState::securing) {
        return Error::cryptographic_failure;
    }
    KeySlot* slot = transmit_slot(active_sender_key_);
    if (slot == nullptr || !slot->ready()) {
        return Error::invalid_state;
    }
    if (slot->mode != CryptoMode::aes_ctr || slot->ctr_cipher == nullptr) {
        // AES-GCM DATA must use the header-bound seal() interface. The legacy
        // tagless transform remains CTR-only.
        return Error::unsupported;
    }
    if (!ready_to_send_data()) {
        return Error::cryptographic_failure;
    }
    const auto iv =
        make_srt_ctr_initialization_vector(sequence, slot->salt);
    const Error result = slot->ctr_cipher->transform(iv, plaintext, ciphertext);
    if (result == Error::none) {
        key = active_sender_key_;
    }
    return result;
}

CryptoSession::KeySlot* CryptoSession::receive_slot_for_packet(
    EncryptionKey key, SequenceNumber sequence, bool commit_sequence) noexcept
{
    KeySlot* current = receive_slot(key);
    ReceiveKeyHistory* history = receive_history(key);
    if (current == nullptr || history == nullptr) {
        return nullptr;
    }

    KeySlot* selected = current;
    // Haivision stores encrypted packets in its send buffer. A
    // retransmission therefore retains both its original selector and
    // ciphertext even after that selector has been reused several times.
    // Search oldest to newest so the first ceiling containing the sequence
    // identifies the exact generation independently of arrival order.
    // Current in-order traffic takes the single newest-ceiling comparison.
    const bool may_be_historical =
        history->size != 0U
        && history->generations[0].sequence_ceiling_known
        && sequence.distance_from(
            history->generations[0].sequence_ceiling) <= 0;
    if (may_be_historical) {
        if (history->discarded_sequence_ceiling_known
            && sequence.distance_from(
                history->discarded_sequence_ceiling) <= 0) {
            return nullptr;
        }
        for (std::size_t index = history->size;
             index > 0U; --index) {
            auto& generation = history->generations[index - 1U];
            if (generation.slot.ready()
                && generation.sequence_ceiling_known
                && sequence.distance_from(
                    generation.sequence_ceiling) <= 0) {
                selected = &generation.slot;
                break;
            }
        }
    }

    if (commit_sequence) {
        note_authenticated_receive_sequence(sequence);
    }
    return selected;
}

void CryptoSession::note_authenticated_receive_sequence(
    SequenceNumber sequence) noexcept
{
    if (!highest_receive_sequence_known_
        || sequence.distance_from(highest_receive_sequence_) > 0) {
        highest_receive_sequence_ = sequence;
        highest_receive_sequence_known_ = true;
    }
}

Error CryptoSession::decrypt(
    EncryptionKey key,
    SequenceNumber sequence,
    std::span<const std::byte> ciphertext,
    std::span<std::byte> plaintext) noexcept
{
    if (key == EncryptionKey::none) {
        if (enabled() && receiver_state_ == CryptoState::secured) {
            return Error::cryptographic_failure;
        }
        if (plaintext.size() < ciphertext.size()) {
            return Error::buffer_too_small;
        }
        std::copy(ciphertext.begin(), ciphertext.end(),
            plaintext.begin());
        return Error::none;
    }
    if (effective_mode_ == CryptoMode::aes_gcm) {
        provider_.secure_erase(plaintext);
        return Error::unsupported;
    }
    KeySlot* slot = receive_slot_for_packet(key, sequence);
    if (slot == nullptr || !slot->ready()) {
        return Error::cryptographic_failure;
    }
    if (slot->mode != CryptoMode::aes_ctr || slot->ctr_cipher == nullptr) {
        provider_.secure_erase(plaintext);
        // See encrypt(): authenticated DATA requires header-bound open().
        return Error::unsupported;
    }
    const auto iv =
        make_srt_ctr_initialization_vector(sequence, slot->salt);
    return slot->ctr_cipher->transform(iv, ciphertext, plaintext);
}

Error CryptoSession::seal(const DataHeader& header,
    std::span<const std::byte> plaintext, std::span<std::byte> ciphertext,
    std::span<std::byte> authentication_tag, EncryptionKey& key) noexcept
{
    key = EncryptionKey::none;
    const auto reject = [&](Error error) noexcept {
        provider_.secure_erase(ciphertext);
        provider_.secure_erase(authentication_tag);
        return error;
    };
    if (!enabled() || !authenticated_data_enabled()) {
        return reject(Error::unsupported);
    }
    if (!ready_to_send_data()) {
        return reject(Error::cryptographic_failure);
    }
    KeySlot* slot = transmit_slot(active_sender_key_);
    if (slot == nullptr || slot->mode != CryptoMode::aes_gcm
        || slot->authenticated_cipher == nullptr
        || header.encryption_key != active_sender_key_) {
        return reject(Error::invalid_state);
    }
    const auto iv =
        make_srt_gcm_initialization_vector(header.sequence, slot->salt);
    const auto aad = make_srt_gcm_additional_authenticated_data(header);
    const Error result = slot->authenticated_cipher->seal(
        iv, aad, plaintext, ciphertext, authentication_tag);
    if (result == Error::none) {
        key = active_sender_key_;
    } else {
        provider_.secure_erase(ciphertext);
        provider_.secure_erase(authentication_tag);
    }
    return result;
}

Error CryptoSession::open(const DataHeader& header,
    std::span<const std::byte> ciphertext,
    std::span<const std::byte> authentication_tag,
    std::span<std::byte> plaintext) noexcept
{
    if (!enabled() || !authenticated_data_enabled()) {
        provider_.secure_erase(plaintext);
        return Error::unsupported;
    }
    KeySlot* slot =
        receive_slot_for_packet(header.encryption_key, header.sequence, false);
    if (slot == nullptr || slot->mode != CryptoMode::aes_gcm
        || slot->authenticated_cipher == nullptr) {
        provider_.secure_erase(plaintext);
        return Error::cryptographic_failure;
    }
    const auto iv =
        make_srt_gcm_initialization_vector(header.sequence, slot->salt);
    const auto aad = make_srt_gcm_additional_authenticated_data(header);
    const Error result = slot->authenticated_cipher->open(
        iv, aad, ciphertext, authentication_tag, plaintext);
    if (result == Error::none) {
        note_authenticated_receive_sequence(header.sequence);
    } else {
        provider_.secure_erase(plaintext);
    }
    return result;
}

std::span<const std::byte>
CryptoSession::pending_key_material() const noexcept
{
    return key_material_pending_
        ? pending_key_material_.view()
        : std::span<const std::byte>{};
}

std::span<const std::byte>
CryptoSession::key_material_response() const noexcept
{
    return key_material_response_.view();
}

} // namespace robotweax::srt
