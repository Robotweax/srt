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

// One Caller/Listener exchange, independent of sockets. Cookie/admission budgets,
// transport negotiation, deadlines and application publication belong to the
// socket layer. Every caller of this component must supply complete encoded
// handshake payloads, including the generated record. Never change those bytes
// after sealing; cached() is the only source for retransmission. Errors are
// internal: the socket layer must use one rejection reason for all M3 failures.
// An invalid M3 signature consumes the bounded retry allowance; check failed()
// before rejecting the pending child.
//
// ready_for_key_exchange() does NOT authenticate the listener's passphrase:
// the caller must verify its first authenticated KM control before DATA/I/O.
class Exchange {
public:
    explicit Exchange(CryptoProvider& provider, bool caller) noexcept
        : provider_(provider)
        , schedule_(provider)
        , caller_(caller)
    {
    }
    ~Exchange();
    Exchange(const Exchange&) = delete;
    Exchange& operator=(const Exchange&) = delete;
    Exchange(Exchange&&) = delete;
    Exchange& operator=(Exchange&&) = delete;
    [[nodiscard]] Error configure(std::span<const std::byte, 32> identity,
        std::span<const std::byte> pinned_public_keys) noexcept;
    [[nodiscard]] Error prepare_offer(
        std::uint8_t key_length, Record& offer) noexcept;
    [[nodiscard]] Error bind_offer(std::span<const std::byte> offer) noexcept;
    [[nodiscard]] Error prepare_response(std::span<const std::byte> offer,
        std::uint8_t local_key_length, Record& response) noexcept;
    [[nodiscard]] Error seal_response(std::span<std::byte> response) noexcept;
    [[nodiscard]] Error prepare_confirmation(
        std::span<const std::byte> response, Record& confirmation) noexcept;
    [[nodiscard]] Error seal_confirmation(std::span<std::byte> confirmation,
        std::span<const std::byte> passphrase) noexcept;
    [[nodiscard]] Error accept_confirmation(
        std::span<const std::byte> confirmation,
        std::span<const std::byte> passphrase) noexcept;
    [[nodiscard]] bool ready_for_key_exchange() const noexcept;
    [[nodiscard]] bool is_caller() const noexcept
    {
        return caller_;
    }
    [[nodiscard]] bool failed() const noexcept;
    [[nodiscard]] std::span<const std::byte> cached(
        Message message) const noexcept;
    [[nodiscard]] std::span<const std::byte> peer_public_key() const noexcept;
    [[nodiscard]] std::span<const std::byte> session_key() const noexcept;
    [[nodiscard]] std::span<const std::byte> wrapping_key(
        bool caller) const noexcept;
    // Cancels even a partially completed exchange and destroys private handles.
    void abort() noexcept;

private:
    enum class State {
        fresh,
        configured,
        offer_prepared,
        offered,
        response_prepared,
        responded,
        confirmation_prepared,
        ready,
        failed
    };
    struct Flight {
        std::array<std::byte, maximum_payload_size> bytes {};
        std::size_t size = 0;
        void assign(std::span<const std::byte> input) noexcept;
        [[nodiscard]] std::span<const std::byte> view() const noexcept;
    };
    [[nodiscard]] Error fail(Error error) noexcept;
    [[nodiscard]] Error select_peer(const Digest& identifier) noexcept;
    [[nodiscard]] Error make_share(Record& record) noexcept;
    [[nodiscard]] Error establish(
        const Record& peer, const Digest& hash) noexcept;
    CryptoProvider& provider_;
    KeySchedule schedule_;
    bool caller_;
    State state_ = State::fresh;
    std::unique_ptr<P256Key> identity_, ephemeral_;
    Digest identity_identifier_ {};
    std::array<std::array<std::byte, 64>, 16> pins_ {};
    std::array<Digest, 16> pin_identifiers_ {};
    std::size_t pin_count_ = 0;
    std::array<std::byte, 64> peer_ {};
    std::array<Flight, 3> flights_ {};
    Record offer_ {}, response_ {}, confirmation_ {};
    unsigned invalid_confirmations_ = 0;
};

} // namespace robotweax::srt::peer_authentication
#endif
