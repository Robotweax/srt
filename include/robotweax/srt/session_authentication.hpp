// SPDX-License-Identifier: MIT
#pragma once

#include "robotweax/srt/crypto_provider.hpp"
#include "robotweax/srt/packet.hpp"

#include <array>
#include <cstdint>
#include <span>

namespace robotweax::srt {

inline constexpr std::uint16_t session_authentication_extension_type = 0x7f10;
inline constexpr std::uint16_t authenticated_key_request_subtype = 0x7f03;
inline constexpr std::uint16_t authenticated_key_response_subtype = 0x7f04;
inline constexpr std::size_t session_authentication_wire_size = 100;
inline constexpr std::size_t authenticated_key_overhead = 40;
inline constexpr std::uint8_t session_authentication_version = 2;
inline constexpr std::size_t authenticated_fec_overhead = 40;
inline constexpr std::uint8_t acp1_session_version = 3;
inline constexpr std::uint8_t acp1_rendezvous_session_version = 4;
inline constexpr std::uint8_t acp1_group_session_version = 5;
inline constexpr std::size_t acp1_group_context_size = 88;
[[nodiscard]] constexpr bool is_acp1_session_version(
    std::uint8_t version) noexcept
{
    return version == acp1_session_version
        || version == acp1_rendezvous_session_version
        || version == acp1_group_session_version;
}
inline constexpr std::uint16_t authenticated_control_subtype = 0x7f05;
inline constexpr std::size_t authenticated_control_overhead = 60;
inline constexpr std::size_t authenticated_control_inner_offset = 28;

struct SessionAuthenticationParameters {
    std::array<std::byte, 32> caller_nonce {};
    std::array<std::byte, 32> listener_nonce {};
    std::array<std::byte, 32> proof {};
    std::uint8_t version = session_authentication_version;

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

    [[nodiscard]] Error start(bool caller,
        std::uint8_t version = session_authentication_version) noexcept;
#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_ACP1
    // Experimental Caller/Listener v3, Rendezvous v4, or group member v5.
    // Full serialized controls, bounded to 1500
    // bytes, with independent directional keys and a 64-packet replay window.
    [[nodiscard]] Error seal_control(std::span<const std::byte> packet,
        std::span<std::byte> output, std::size_t& written) noexcept;
    [[nodiscard]] bool open_control(
        const PacketView& envelope, PacketView& packet) noexcept;
#endif
    // Version 5 requires the canonical 88-byte group context documented in
    // experimental-acp1.md; other versions reject any group context.
    [[nodiscard]] Error establish(std::span<const std::byte> passphrase,
        const SessionAuthenticationParameters& peer,
        std::uint32_t local_socket_id, std::uint32_t peer_socket_id,
        std::uint32_t local_cookie = 0, std::uint32_t peer_cookie = 0,
        std::span<const std::byte> group_context = {}) noexcept;
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

    // Version 2 authenticates complete parity headers and bodies. A separate
    // 64-packet replay window permits bounded datagram reordering.
    [[nodiscard]] Error seal_fec(const DataHeader& header,
        std::span<const std::byte> parity, std::span<std::byte> output,
        std::size_t& written) noexcept;
    [[nodiscard]] bool open_fec(const DataHeader& header,
        std::span<const std::byte> input,
        std::span<const std::byte>& parity) noexcept;

private:
    [[nodiscard]] Error fec_mac(bool caller, std::uint64_t counter,
        const DataHeader& header, std::span<const std::byte> parity,
        std::span<std::byte, 32> output) noexcept;
    [[nodiscard]] Error mac(std::uint8_t domain, bool caller,
        std::uint64_t counter, std::span<const std::byte> input,
        std::span<std::byte, 32> output) noexcept;
#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_ACP1
    [[nodiscard]] Error control_mac(bool caller,
        std::span<const std::byte> wire, std::span<std::byte, 32> tag) noexcept;
    std::array<std::byte, 32> caller_control_key_ {}, listener_control_key_ {};
    std::uint64_t control_sent_counter_ = 0, control_received_counter_ = 0,
                  control_received_bitmap_ = 0;
#endif
    CryptoProvider& provider_;
    SessionAuthenticationParameters parameters_ {};
    std::array<std::byte, 32> key_ {};
    // KM's maximum wire size is below 128 bytes. Keep this independent of
    // CryptoSession's header to avoid a cyclic public include dependency.
    std::array<std::byte, 128> last_sent_ {}, last_received_ {};
    std::size_t last_sent_size_ = 0, last_received_size_ = 0;
    std::uint64_t sent_counter_ = 0, received_counter_ = 0;
    std::uint64_t fec_sent_counter_ = 0, fec_received_counter_ = 0,
                  fec_received_bitmap_ = 0;
    bool caller_ = false, started_ = false, ready_ = false;
};

} // namespace robotweax::srt
