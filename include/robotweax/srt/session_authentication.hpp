// SPDX-License-Identifier: MIT
#pragma once

#include "robotweax/srt/crypto_provider.hpp"

#include <array>
#include <cstdint>
#include <span>

namespace robotweax::srt {

inline constexpr std::uint16_t session_authentication_extension_type = 0x7f10;
inline constexpr std::uint16_t authenticated_key_request_subtype = 0x7f03;
inline constexpr std::uint16_t authenticated_key_response_subtype = 0x7f04;
inline constexpr std::size_t session_authentication_wire_size = 100;
inline constexpr std::size_t authenticated_key_overhead = 40;

struct SessionAuthenticationParameters {
    std::array<std::byte, 32> caller_nonce {};
    std::array<std::byte, 32> listener_nonce {};
    std::array<std::byte, 32> proof {};

    [[nodiscard]] bool is_offer() const noexcept;
    [[nodiscard]] bool is_proof() const noexcept;
    [[nodiscard]] bool matches(
        const SessionAuthenticationParameters& peer) const noexcept;
};

// Setup-only PBKDF and random generation; runtime authenticates bounded KM
// messages. The owner serializes calls. Nonces, roles, socket identities and
// the complete initial key material bind the two handshake proofs.
class SessionAuthentication {
public:
    explicit SessionAuthentication(CryptoProvider& provider) noexcept
        : provider_(provider)
    {
    }
    ~SessionAuthentication();
    SessionAuthentication(const SessionAuthentication&) = delete;
    SessionAuthentication& operator=(const SessionAuthentication&) = delete;

    [[nodiscard]] Error start(bool caller) noexcept;
    [[nodiscard]] Error establish(std::span<const std::byte> passphrase,
        const SessionAuthenticationParameters& peer,
        std::uint32_t local_socket_id, std::uint32_t peer_socket_id) noexcept;
    [[nodiscard]] Error handshake_proof(bool caller,
        std::span<const std::byte> initial_material,
        SessionAuthenticationParameters& output) noexcept;
    [[nodiscard]] bool verify_handshake(bool caller,
        std::span<const std::byte> initial_material,
        const SessionAuthenticationParameters& peer) noexcept;

    [[nodiscard]] const SessionAuthenticationParameters&
    parameters() const noexcept
    {
        return parameters_;
    }
    [[nodiscard]] bool ready() const noexcept
    {
        return ready_;
    }
    // Response counters echo requests. Requests increase only when the KM
    // payload changes, so packet loss retries retain their authenticated ID.
    [[nodiscard]] Error seal(bool response, std::span<const std::byte> material,
        std::span<std::byte> output, std::size_t& written) noexcept;
    [[nodiscard]] bool open(bool response, std::span<const std::byte> input,
        std::span<const std::byte>& material) noexcept;

private:
    [[nodiscard]] Error mac(std::uint8_t domain, bool caller,
        std::uint64_t counter, std::span<const std::byte> input,
        std::span<std::byte, 32> output) noexcept;
    CryptoProvider& provider_;
    SessionAuthenticationParameters parameters_ {};
    std::array<std::byte, 32> key_ {};
    // KM's maximum wire size is below 128 bytes. Keep this independent of
    // CryptoSession's header to avoid a cyclic public include dependency.
    std::array<std::byte, 128> last_sent_ {}, last_received_ {};
    std::size_t last_sent_size_ = 0, last_received_size_ = 0;
    std::uint64_t sent_counter_ = 0, received_counter_ = 0;
    bool caller_ = false, started_ = false, ready_ = false;
};

} // namespace robotweax::srt
