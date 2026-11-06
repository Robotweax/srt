// SPDX-License-Identifier: MIT
#pragma once

#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_PEERAUTH
#include "robotweax/srt/crypto_provider.hpp"

#include <array>
#include <span>

namespace robotweax::srt::peer_authentication {

inline constexpr std::uint16_t record_type = 0x7f16;
inline constexpr std::size_t maximum_payload_size = 1456;
using Digest = std::array<std::byte, 32>;
enum class Message : std::uint8_t { offer = 1, response = 2, confirmation = 3 };

struct Record {
    Message message = Message::offer;
    std::uint8_t key_length = 0;
    Digest nonce {};
    std::array<std::byte, 64> public_key {};
    Digest key_identifier {};
    std::array<std::byte, 64> signature {};
    Digest confirmation {};
};

[[nodiscard]] std::size_t content_size(Message message) noexcept;
// Structural codec only: curve validation and proof verification belong to
// admission, before any use of the decoded public key. Spans must not overlap.
[[nodiscard]] Error encode_record(const Record& record,
    std::span<std::byte> output, std::size_t& written) noexcept;
[[nodiscard]] Error decode_record(
    std::span<const std::byte> content, Record& record) noexcept;

// Inputs begin at the 48-byte handshake field, excluding the SRT packet header.
// Exact received bytes, including unknown extension records, are hashed.
// The caller retains these bytes for retries. No input pointer is retained.
[[nodiscard]] Error listener_transcript(CryptoProvider& provider,
    std::span<const std::byte> offer, std::span<const std::byte> response,
    std::span<std::byte, 32> output) noexcept;
[[nodiscard]] Error caller_transcript(CryptoProvider& provider,
    std::span<const std::byte> offer, std::span<const std::byte> response,
    std::span<const std::byte> confirmation,
    std::span<std::byte, 32> output) noexcept;

// Owns all intermediate secrets. Provider must outlive the schedule. reset()
// is required when admission fails, times out, or is cancelled; destruction
// also erases everything. Methods never expose partially derived keys.
class KeySchedule {
public:
    explicit KeySchedule(CryptoProvider& provider) noexcept
        : provider_(provider)
    {
    }
    ~KeySchedule();
    KeySchedule(const KeySchedule&) = delete;
    KeySchedule& operator=(const KeySchedule&) = delete;
    KeySchedule(KeySchedule&&) = delete;
    KeySchedule& operator=(KeySchedule&&) = delete;

    // Agreement and point validation happen outside this component. The owner
    // must erase its ECDH private key and shared-secret input after this call.
    [[nodiscard]] Error start(std::span<const std::byte, 32> shared_secret,
        std::span<const std::byte, 32> listener_hash) noexcept;
    [[nodiscard]] Error listener_confirmation(
        std::span<const std::byte, 64> signature,
        std::span<std::byte, 32> output) noexcept;
    // Call only after verifying the peer's pinned signature. This primitive
    // cannot attest that authentication happened; the admission layer must.
    [[nodiscard]] Error derive_authenticated_keys(
        std::span<const std::byte> passphrase,
        std::span<const std::byte, 32> caller_nonce,
        std::span<const std::byte, 32> listener_nonce,
        std::span<const std::byte, 32> caller_hash,
        std::uint8_t key_length) noexcept;
    [[nodiscard]] Error caller_confirmation(
        std::span<const std::byte, 64> signature,
        std::span<std::byte, 32> output) noexcept;
    [[nodiscard]] std::span<const std::byte> session_key() const noexcept;
    [[nodiscard]] std::span<const std::byte> wrapping_key(
        bool caller) const noexcept;
    void reset() noexcept;

private:
    enum class State { empty, handshake, ready };
    CryptoProvider& provider_;
    State state_ = State::empty;
    std::uint8_t key_length_ = 0;
    Digest listener_hash_ {}, caller_hash_ {};
    Digest listener_key_ {}, chaining_salt_ {};
    std::array<Digest, 4> keys_ {};
};

} // namespace robotweax::srt::peer_authentication
#endif
