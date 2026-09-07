#pragma once

#include "robotweax/srt/crypto_provider.hpp"
#include "robotweax/srt/packet.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace robotweax::srt {

inline constexpr std::size_t srt_salt_size = 16;
inline constexpr std::size_t srt_gcm_initialization_vector_size = 12;
inline constexpr std::size_t srt_gcm_additional_data_size = packet_header_size;
inline constexpr std::size_t srt_gcm_authentication_tag_size = 16;
inline constexpr std::size_t minimum_aes_key_size = 16;
inline constexpr std::size_t maximum_aes_key_size = 32;
inline constexpr std::size_t maximum_wrapped_key_size =
    maximum_aes_key_size * 2U + 8U;
inline constexpr std::size_t key_material_header_size = 16;
inline constexpr std::size_t maximum_key_material_size =
    key_material_header_size + srt_salt_size + maximum_wrapped_key_size;
inline constexpr std::uint32_t srt_pbkdf2_iterations = 2'048;
inline constexpr std::size_t maximum_passphrase_size = 80;
inline constexpr std::size_t minimum_passphrase_size = 10;
inline constexpr std::uint32_t default_key_refresh_rate = 16'777'216;
// Rotate before a full 31-bit DATA sequence/nonce cycle under one traffic key.
// This also matches the maximum positive public SRTO_KMREFRESHRATE value.
inline constexpr std::uint32_t maximum_key_refresh_rate = SequenceNumber::mask;
inline constexpr std::uint32_t default_key_preannouncement = 4'096;
inline constexpr std::uint16_t key_material_request_subtype = 3;
inline constexpr std::uint16_t key_material_response_subtype = 4;

[[nodiscard]] constexpr std::uint16_t
encryption_field_for_key_length(std::size_t key_length) noexcept
{
    return key_length == 16U ? 2U
        : key_length == 24U ? 3U
        : key_length == 32U ? 4U : 0U;
}

[[nodiscard]] constexpr std::size_t
key_length_for_encryption_field(std::uint16_t field) noexcept
{
    return field == 2U ? 16U
        : field == 3U ? 24U
        : field == 4U ? 32U : 0U;
}

enum class KeyMaterialCipher : std::uint8_t {
    none = 0,
    aes_ctr = 2,
    aes_gcm = 4,
};

enum class KeyMaterialAuthentication : std::uint8_t {
    none = 0,
    aes_gcm = 1,
};

enum class CryptoMode : std::uint8_t {
    automatic = 0,
    aes_ctr = 1,
    aes_gcm = 2,
};

struct CryptoSuite {
    CryptoMode mode = CryptoMode::automatic;
    KeyMaterialCipher cipher = KeyMaterialCipher::none;
    KeyMaterialAuthentication authentication = KeyMaterialAuthentication::none;
    std::size_t initialization_vector_size = 0;
    std::size_t authentication_tag_size = 0;
};

inline constexpr CryptoSuite aes_ctr_crypto_suite {
    .mode = CryptoMode::aes_ctr,
    .cipher = KeyMaterialCipher::aes_ctr,
    .authentication = KeyMaterialAuthentication::none,
    .initialization_vector_size = aes_block_size,
    .authentication_tag_size = 0,
};

inline constexpr CryptoSuite aes_gcm_crypto_suite {
    .mode = CryptoMode::aes_gcm,
    .cipher = KeyMaterialCipher::aes_gcm,
    .authentication = KeyMaterialAuthentication::aes_gcm,
    .initialization_vector_size = srt_gcm_initialization_vector_size,
    .authentication_tag_size = srt_gcm_authentication_tag_size,
};

[[nodiscard]] constexpr const CryptoSuite* crypto_suite_for_mode(
    CryptoMode mode) noexcept
{
    if (mode == CryptoMode::aes_ctr) {
        return &aes_ctr_crypto_suite;
    }
    if (mode == CryptoMode::aes_gcm) {
        return &aes_gcm_crypto_suite;
    }
    return nullptr;
}

[[nodiscard]] constexpr const CryptoSuite* crypto_suite_for_key_material(
    KeyMaterialCipher cipher, KeyMaterialAuthentication authentication) noexcept
{
    if (cipher == KeyMaterialCipher::aes_ctr
        && authentication == KeyMaterialAuthentication::none) {
        return &aes_ctr_crypto_suite;
    }
    if (cipher == KeyMaterialCipher::aes_gcm
        && authentication == KeyMaterialAuthentication::aes_gcm) {
        return &aes_gcm_crypto_suite;
    }
    return nullptr;
}

// One authoritative calculation owns the relationship between the selected
// crypto suite, MSS/address-family overhead, packet-filter reservation, and
// application plaintext. maximum_wire_payload_size includes any authentication
// tag but excludes the FEC control-header reservation. AUTO is not an effective
// wire mode and therefore cannot produce a budget before negotiation resolves.
struct CryptoPayloadBudget {
    Error error = Error::none;
    std::size_t maximum_datagram_payload_size = 0;
    std::size_t packet_filter_overhead_size = 0;
    std::size_t authentication_tag_size = 0;
    std::size_t maximum_wire_payload_size = 0;
    std::size_t maximum_plaintext_payload_size = 0;

    [[nodiscard]] constexpr explicit operator bool() const noexcept
    {
        return error == Error::none;
    }
};

[[nodiscard]] constexpr CryptoPayloadBudget make_crypto_payload_budget(
    CryptoMode effective_mode, std::size_t maximum_segment_size,
    std::size_t transport_overhead_size,
    std::size_t packet_filter_overhead_size = 0U) noexcept
{
    const CryptoSuite* suite = crypto_suite_for_mode(effective_mode);
    if (suite == nullptr) {
        return {.error = Error::unsupported};
    }
    if (maximum_segment_size <= transport_overhead_size) {
        return {.error = Error::invalid_state};
    }
    const std::size_t transport_payload_size =
        maximum_segment_size - transport_overhead_size;
    const std::size_t datagram_payload_size =
        transport_payload_size < maximum_data_payload_size
        ? transport_payload_size
        : maximum_data_payload_size;
    if (packet_filter_overhead_size >= datagram_payload_size) {
        return {.error = Error::invalid_state};
    }
    const std::size_t wire_payload_size =
        datagram_payload_size - packet_filter_overhead_size;
    if (suite->authentication_tag_size >= wire_payload_size) {
        return {.error = Error::invalid_state};
    }
    return {
        .maximum_datagram_payload_size = datagram_payload_size,
        .packet_filter_overhead_size = packet_filter_overhead_size,
        .authentication_tag_size = suite->authentication_tag_size,
        .maximum_wire_payload_size = wire_payload_size,
        .maximum_plaintext_payload_size =
            wire_payload_size - suite->authentication_tag_size,
    };
}

enum class CryptoNegotiationContext : std::uint8_t {
    caller_listener = 0,
    rendezvous = 1,
};

struct CryptoModeSelection {
    CryptoMode effective_mode = CryptoMode::automatic;
    bool compatible = false;

    [[nodiscard]] constexpr explicit operator bool() const noexcept
    {
        return compatible;
    }
};

// The initiator is a Caller for caller_listener and the cookie-role initiator
// for rendezvous. AUTO initiators retain CTR. An ordinary AUTO Listener may
// adopt an explicit GCM Caller, whereas Rendezvous GCM requires both peers to
// select GCM explicitly.
[[nodiscard]] constexpr CryptoModeSelection negotiate_crypto_mode(
    CryptoNegotiationContext context, CryptoMode initiator,
    CryptoMode responder) noexcept
{
    if ((context != CryptoNegotiationContext::caller_listener
            && context != CryptoNegotiationContext::rendezvous)
        || (initiator != CryptoMode::automatic
            && crypto_suite_for_mode(initiator) == nullptr)
        || (responder != CryptoMode::automatic
            && crypto_suite_for_mode(responder) == nullptr)) {
        return {};
    }
    const CryptoMode selected =
        initiator == CryptoMode::automatic ? CryptoMode::aes_ctr : initiator;
    if (responder == CryptoMode::automatic) {
        if (context == CryptoNegotiationContext::rendezvous
            && selected == CryptoMode::aes_gcm) {
            return {};
        }
        return {.effective_mode = selected, .compatible = true};
    }
    return responder == selected
        ? CryptoModeSelection {
              .effective_mode = selected,
              .compatible = true,
          }
        : CryptoModeSelection {};
}

enum class StreamEncapsulation : std::uint8_t {
    unspecified = 0,
    mpeg_ts_udp = 1,
    mpeg_ts_srt = 2,
};

enum class CryptoState : std::uint8_t {
    unsecured = 0,
    securing = 1,
    secured = 2,
    no_secret = 3,
    bad_secret = 4,
    bad_crypto_mode = 5,
};

/**
 * Non-owning key-material descriptor. `salt` and `wrapped_keys` borrow the
 * encoded bytes supplied by the caller or decoder; copying this value does not
 * extend their lifetime.
 */
struct KeyMaterialView {
    EncryptionKey keys = EncryptionKey::none;
    std::uint32_t key_encrypting_key_index = 0;
    KeyMaterialCipher cipher = KeyMaterialCipher::aes_ctr;
    KeyMaterialAuthentication authentication =
        KeyMaterialAuthentication::none;
    StreamEncapsulation stream_encapsulation =
        StreamEncapsulation::mpeg_ts_srt;
    std::span<const std::byte> salt{};
    std::size_t key_length = 0;
    std::span<const std::byte> wrapped_keys{};

    [[nodiscard]] constexpr std::size_t key_count() const noexcept
    {
        return keys == EncryptionKey::reserved ? 2U : 1U;
    }
};

struct KeyMaterialDecodeResult {
    Error error = Error::none;
    KeyMaterialView key_material{};

    [[nodiscard]] constexpr explicit operator bool() const noexcept
    {
        return error == Error::none;
    }
};

struct KeyMaterialEncodeResult {
    Error error = Error::none;
    std::size_t bytes_written = 0;

    [[nodiscard]] constexpr explicit operator bool() const noexcept
    {
        return error == Error::none;
    }
};

/** Returns views into `bytes`; the function performs no allocation. */
[[nodiscard]] KeyMaterialDecodeResult decode_key_material(
    std::span<const std::byte> bytes) noexcept;

/** Encodes into caller storage and retains no input/output pointers. */
[[nodiscard]] KeyMaterialEncodeResult encode_key_material(
    const KeyMaterialView& key_material,
    std::span<std::byte> destination) noexcept;

[[nodiscard]] std::array<std::byte, aes_block_size>
make_srt_ctr_initialization_vector(
    SequenceNumber sequence,
    std::span<const std::byte, srt_salt_size> salt) noexcept;

[[nodiscard]] std::array<std::byte, srt_gcm_initialization_vector_size>
make_srt_gcm_initialization_vector(SequenceNumber sequence,
    std::span<const std::byte, srt_salt_size> salt) noexcept;

[[nodiscard]] std::array<std::byte, srt_gcm_additional_data_size>
make_srt_gcm_additional_authenticated_data(const DataHeader& header) noexcept;

struct CryptoConfiguration {
    // Borrowed only during CryptoSession construction; the session copies and
    // later securely erases the passphrase bytes it owns.
    std::string_view passphrase{};
    CryptoMode mode = CryptoMode::automatic;
    CryptoNegotiationContext negotiation_context =
        CryptoNegotiationContext::caller_listener;
    // Explicitly enables the released AES-GCM extension surface. Negotiation
    // still must resolve the configured mode to AES-GCM before DATA uses it.
    bool enable_aes_gcm = false;
    std::size_t key_length = minimum_aes_key_size;
    // Explicit interval below the nonce period; zero/default translation is
    // performed by SocketOptions, not by direct CryptoSession construction.
    std::uint32_t refresh_rate_packets = default_key_refresh_rate;
    std::uint32_t preannouncement_packets =
        default_key_preannouncement;
};

struct KeyMaterialBuffer {
    std::array<std::byte, maximum_key_material_size> bytes{};
    std::size_t size = 0;

    /** Returns storage owned by this object, valid until it is modified. */
    [[nodiscard]] constexpr std::span<const std::byte> view() const noexcept
    {
        return std::span{bytes}.first(size);
    }
};

// One instance owns the independent transmit and receive crypto directions of
// an SRT connection. External connection serialization also serializes this
// class; it intentionally contains no lock in the packet hot path. The
// provider is borrowed and must outlive the session. Public operations are
// noexcept, but concurrent access to one session requires external locking.
class CryptoSession {
public:
    explicit CryptoSession(
        CryptoConfiguration configuration,
        CryptoProvider& provider = default_crypto_provider()) noexcept;
    ~CryptoSession();

    CryptoSession(const CryptoSession&) = delete;
    CryptoSession& operator=(const CryptoSession&) = delete;
    CryptoSession(CryptoSession&&) = delete;
    CryptoSession& operator=(CryptoSession&&) = delete;

    [[nodiscard]] Error start_initiator() noexcept;

    // Installs a peer KMREQ and prepares the byte-identical KMRSP. During an
    // HSv5 initial exchange, clone_for_bidirectional_sender prepares an
    // independent transmit key. DATA remains blocked until its runtime
    // KMREQ is confirmed; a shared handshake key is receive-only compatibility
    // material, never a bidirectional DATA key.
    [[nodiscard]] Error accept_key_material(
        std::span<const std::byte> request,
        bool clone_for_bidirectional_sender) noexcept;

    // During initial bidirectional establishment, preserve the handshake key
    // for legacy reverse reception, then announce fresh local transmit
    // material. A duplicate initial response cannot confirm that new key.
    [[nodiscard]] Error acknowledge_key_material(
        std::span<const std::byte> response,
        bool clone_for_bidirectional_receiver) noexcept;

    // Optional encryption may fall back only before encrypted key material
    // has been accepted. A local failure preparing a direction must not erase
    // that decision at the public handshake boundary.
    [[nodiscard]] bool allows_plaintext_fallback() const noexcept
    {
        return !directional_key_pending_ && !rotation_prepared_
            && sender_state_ != CryptoState::secured
            && receiver_state_ != CryptoState::secured
            && receiver_state_ != CryptoState::securing;
    }

    [[nodiscard]] Error prepare_rotation() noexcept;
    [[nodiscard]] Error note_data_packet_sent() noexcept;
    [[nodiscard]] bool ready_to_send_data() const noexcept;

    [[nodiscard]] Error encrypt(
        SequenceNumber sequence,
        std::span<const std::byte> plaintext,
        std::span<std::byte> ciphertext,
        EncryptionKey& key) noexcept;

    [[nodiscard]] Error decrypt(
        EncryptionKey key,
        SequenceNumber sequence,
        std::span<const std::byte> ciphertext,
        std::span<std::byte> plaintext) noexcept;

    // Authenticated DATA binds the complete canonical SRT data header to the
    // ciphertext. The caller must stamp the active selector into header before
    // sealing; opening publishes plaintext only after the tag is accepted.
    [[nodiscard]] Error seal(const DataHeader& header,
        std::span<const std::byte> plaintext, std::span<std::byte> ciphertext,
        std::span<std::byte> authentication_tag, EncryptionKey& key) noexcept;

    [[nodiscard]] Error open(const DataHeader& header,
        std::span<const std::byte> ciphertext,
        std::span<const std::byte> authentication_tag,
        std::span<std::byte> plaintext) noexcept;

    // Both accessors return session-owned storage. A returned span is invalid
    // after the next non-const session operation or session destruction.
    [[nodiscard]] std::span<const std::byte>
        pending_key_material() const noexcept;
    [[nodiscard]] std::span<const std::byte>
        key_material_response() const noexcept;

    [[nodiscard]] CryptoState sender_state() const noexcept
    {
        return sender_state_;
    }
    [[nodiscard]] CryptoState receiver_state() const noexcept
    {
        return receiver_state_;
    }
    [[nodiscard]] EncryptionKey active_sender_key() const noexcept
    {
        return active_sender_key_;
    }
    [[nodiscard]] std::uint64_t packets_on_active_key() const noexcept
    {
        return packets_on_active_key_;
    }
    [[nodiscard]] bool enabled() const noexcept
    {
        return passphrase_size_ != 0U;
    }
    [[nodiscard]] std::size_t key_length() const noexcept;
    [[nodiscard]] CryptoMode configured_mode() const noexcept
    {
        return configured_mode_;
    }
    [[nodiscard]] CryptoMode effective_mode() const noexcept
    {
        return effective_mode_;
    }
    [[nodiscard]] bool authenticated_data_enabled() const noexcept
    {
        return aes_gcm_enabled_ && effective_mode_ == CryptoMode::aes_gcm;
    }

private:
    // Interoperable passphrase key material uses KEKI zero, so a small bounded
    // content history distinguishes delayed UDP control packets from new keys.
    static constexpr std::size_t key_material_history_capacity = 4;
    using KeyMaterialHistory =
        std::array<KeyMaterialBuffer, key_material_history_capacity>;

    struct KeySlot {
        std::array<std::byte, srt_salt_size> salt{};
        std::array<std::byte, maximum_aes_key_size> key{};
        std::size_t key_length = 0;
        CryptoMode mode = CryptoMode::automatic;
        std::unique_ptr<PayloadCipher> ctr_cipher;
        std::unique_ptr<AuthenticatedPayloadCipher> authenticated_cipher;

        [[nodiscard]] bool ready() const noexcept
        {
            return (mode == CryptoMode::aes_ctr && ctr_cipher != nullptr)
                || (mode == CryptoMode::aes_gcm
                    && authenticated_cipher != nullptr);
        }
    };

    struct ReceiveKeyGeneration {
        // A peer may announce a replacement for an even/odd selector while
        // packets encrypted with the previous generation are still eligible
        // for retransmission. The receive frontier at replacement separates
        // this generation from the newer one without inspecting ciphertext.
        KeySlot slot{};
        bool sequence_ceiling_known = false;
        SequenceNumber sequence_ceiling{};
    };

    // Key selectors are reused on every other rotation. A bounded history
    // covers delayed packets across several reuse cycles without allocating
    // in the receive hot path.
    static constexpr std::size_t receive_key_history_capacity = 4;
    struct ReceiveKeyHistory {
        std::array<ReceiveKeyGeneration,
            receive_key_history_capacity> generations{};
        std::size_t size = 0;
        bool discarded_sequence_ceiling_known = false;
        SequenceNumber discarded_sequence_ceiling{};
    };

    [[nodiscard]] Error validate_configuration() const noexcept;
    [[nodiscard]] Error generate_initial_sender_key(
        std::size_t key_length) noexcept;
    [[nodiscard]] Error prepare_bidirectional_sender(
        std::size_t key_length) noexcept;
    [[nodiscard]] Error generate_next_sender_key() noexcept;
    [[nodiscard]] Error build_sender_key_material(
        EncryptionKey keys) noexcept;
    [[nodiscard]] Error install_key(KeySlot& slot,
        std::span<const std::byte> key,
        std::span<const std::byte, srt_salt_size> salt,
        CryptoMode mode) noexcept;
    [[nodiscard]] Error install_receive_key(EncryptionKey key_selection,
        std::span<const std::byte> key,
        std::span<const std::byte, srt_salt_size> salt,
        CryptoMode mode) noexcept;
    [[nodiscard]] Error clone_key(
        const KeySlot& source, KeySlot& destination) noexcept;
    [[nodiscard]] Error clone_transmit_keys_to_receive() noexcept;
    [[nodiscard]] KeySlot* transmit_slot(EncryptionKey key) noexcept;
    [[nodiscard]] KeySlot* receive_slot(EncryptionKey key) noexcept;
    [[nodiscard]] ReceiveKeyHistory* receive_history(
        EncryptionKey key) noexcept;
    [[nodiscard]] KeySlot* receive_slot_for_packet(EncryptionKey key,
        SequenceNumber sequence, bool commit_sequence = true) noexcept;
    void note_authenticated_receive_sequence(SequenceNumber sequence) noexcept;
    [[nodiscard]] const KeySlot* transmit_slot(
        EncryptionKey key) const noexcept;
    [[nodiscard]] bool history_contains(
        const KeyMaterialHistory& history,
        std::size_t history_size,
        std::span<const std::byte> material) const noexcept;
    void copy_key_material(
        KeyMaterialBuffer& destination,
        std::span<const std::byte> material) noexcept;
    void remember_key_material(
        KeyMaterialHistory& history,
        std::size_t& history_size,
        std::span<const std::byte> material) noexcept;
    void remember_receive_key(
        ReceiveKeyHistory& history, KeySlot& slot) noexcept;
    void erase_receive_history(ReceiveKeyHistory& history) noexcept;
    void erase_slot(KeySlot& slot) noexcept;

    CryptoProvider& provider_;
    std::array<std::byte, maximum_passphrase_size> passphrase_{};
    std::size_t passphrase_size_ = 0;
    CryptoMode configured_mode_ = CryptoMode::automatic;
    CryptoNegotiationContext negotiation_context_ =
        CryptoNegotiationContext::caller_listener;
    bool aes_gcm_enabled_ = false;
    CryptoMode effective_mode_ = CryptoMode::automatic;
    std::size_t configured_key_length_ = minimum_aes_key_size;
    std::uint32_t refresh_rate_packets_ = default_key_refresh_rate;
    std::uint32_t preannouncement_packets_ =
        default_key_preannouncement;
    KeySlot transmit_even_{};
    KeySlot transmit_odd_{};
    KeySlot receive_even_{};
    KeySlot receive_odd_{};
    ReceiveKeyHistory receive_even_history_{};
    ReceiveKeyHistory receive_odd_history_{};
    SequenceNumber highest_receive_sequence_{};
    bool highest_receive_sequence_known_ = false;
    KeyMaterialBuffer pending_key_material_{};
    KeyMaterialBuffer key_material_response_{};
    KeyMaterialHistory received_key_material_history_{};
    std::size_t received_key_material_history_size_ = 0;
    KeyMaterialHistory acknowledged_key_material_history_{};
    std::size_t acknowledged_key_material_history_size_ = 0;
    EncryptionKey active_sender_key_ = EncryptionKey::even;
    std::uint64_t packets_on_active_key_ = 0;
    CryptoState sender_state_ = CryptoState::unsecured;
    CryptoState receiver_state_ = CryptoState::unsecured;
    Error configuration_error_ = Error::none;
    bool rotation_prepared_ = false;
    bool key_material_pending_ = false;
    bool pending_acknowledged_ = false;
    bool directional_key_pending_ = false;
};

} // namespace robotweax::srt
