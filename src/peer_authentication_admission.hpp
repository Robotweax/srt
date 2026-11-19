// SPDX-License-Identifier: MIT
#pragma once

#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_PEERAUTH
#include "peer_authentication.hpp"
#include "robotweax/srt/socket_options.hpp"

#include <chrono>
#include <memory>

namespace robotweax::srt::peer_authentication {

// Serial, connection-owned crypto admission coordinator. The socket owner must
// validate routing/cookies, charge admission budgets, run its callback, validate
// transport negotiation and enforce its own deadline before invoking this class.
// Calls that perform public-key crypto/PBKDF belong off the scheduler shard.
// The provider must outlive this object and any released CryptoSession.
// All handshake inputs are exact payloads produced/retained by the datagram
// codec. This class does no I/O and never publishes a socket itself.
class Admission {
public:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;
    Admission(CryptoProvider& provider, bool caller) noexcept
        : provider_(provider)
        , exchange_(provider, caller)
        , caller_(caller)
    {
    }
    ~Admission();
    Admission(const Admission&) = delete;
    Admission& operator=(const Admission&) = delete;

    [[nodiscard]] Error configure(
        const SocketOptions& options, Time deadline, Time now) noexcept;
    [[nodiscard]] Error prepare_offer(
        std::uint8_t key_length, Record& output, Time now) noexcept;
    [[nodiscard]] Error bind_offer(
        std::span<const std::byte> payload, Time now) noexcept;
    [[nodiscard]] Error prepare_response(std::span<const std::byte> offer,
        std::uint8_t local_key_length, Record& output, Time now) noexcept;
    [[nodiscard]] Error seal_response(
        std::span<std::byte> payload, Time now) noexcept;
    [[nodiscard]] Error prepare_confirmation(
        std::span<const std::byte> response, Record& output, Time now) noexcept;
    [[nodiscard]] Error seal_confirmation(
        std::span<std::byte> payload, Time now) noexcept;
    [[nodiscard]] Error accept_confirmation(
        std::span<const std::byte> payload, Time now) noexcept;

    // Runtime KM payloads only, carried in authenticated subtypes 0x7f03/04.
    // Invalid envelopes do not advance admission. Valid envelopes containing
    // invalid key material terminate it. A KMREQ produces a KMRSP in output.
    [[nodiscard]] Error key_request(
        std::span<std::byte> output, std::size_t& written, Time now) noexcept;
    [[nodiscard]] Error accept_key_control(bool response,
        std::span<const std::byte> input, std::span<std::byte> output,
        std::size_t& written, Time now) noexcept;

    // Check a timer even when no input arrives. Any terminal error, cancellation
    // or destruction erases all owned secrets. The owner maps all proof failures
    // to the same outward rejection reason; failed() distinguishes retryable M3.
    [[nodiscard]] bool expire(Time now) noexcept;
    void abort() noexcept;
    [[nodiscard]] bool failed() const noexcept
    {
        return state_ == State::failed;
    }
    [[nodiscard]] bool ready_to_publish(Time now) noexcept;
    // Only a fully confirmed session can escape this coordinator. The socket
    // must still install it atomically before publishing CONNECTED/accept.
    [[nodiscard]] std::shared_ptr<CryptoSession> release_crypto(
        Time now) noexcept;
    [[nodiscard]] std::span<const std::byte> peer_public_key(Time now) noexcept;
    // Exact public flight bytes survive key import for loss/retry handling.
    [[nodiscard]] std::span<const std::byte> cached(
        Message message, Time now) noexcept;

private:
    enum class State { fresh, proofs, keys, ready, released, failed };
    struct Flight {
        std::array<std::byte, maximum_payload_size> bytes {};
        std::size_t size = 0;
        [[nodiscard]] std::span<const std::byte> view() const noexcept
        {
            return std::span {bytes}.first(size);
        }
    };
    [[nodiscard]] bool active(Time now) noexcept;
    [[nodiscard]] Error proof_result(Error result) noexcept;
    [[nodiscard]] Error install_keys() noexcept;
    void erase_passphrase() noexcept;
    void update_ready() noexcept;
    CryptoProvider& provider_;
    Exchange exchange_;
    bool caller_;
    State state_ = State::fresh;
    Time deadline_ {};
    CryptoConfiguration configuration_ {};
    std::array<char, maximum_passphrase_size> passphrase_ {};
    std::size_t passphrase_size_ = 0;
    std::shared_ptr<CryptoSession> crypto_;
    std::array<Flight, 3> flights_ {};
    std::array<std::byte, 64> peer_ {};
};

} // namespace robotweax::srt::peer_authentication
#endif
