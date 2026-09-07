#pragma once

#include "robotweax/srt/crypto.hpp"

#include <cstddef>
#include <span>

namespace robotweax::srt {

/** Result whose packet payload borrows the bytes passed to decode_packet(). */
struct DecodeResult {
    Error error = Error::none;
    PacketView packet{};

    [[nodiscard]] constexpr explicit operator bool() const noexcept
    {
        return error == Error::none;
    }
};

struct EncodeResult {
    Error error = Error::none;
    std::size_t bytes_written = 0;

    [[nodiscard]] constexpr explicit operator bool() const noexcept
    {
        return error == Error::none;
    }
};

/** Both spans borrow the protected payload passed to the decoder. */
struct ProtectedPayloadView {
    std::span<const std::byte> ciphertext {};
    std::span<const std::byte> authentication_tag {};
};

/** Both spans alias caller-owned destination storage. */
struct MutableProtectedPayloadView {
    std::span<std::byte> ciphertext {};
    std::span<std::byte> authentication_tag {};
};

struct ProtectedPayloadDecodeResult {
    Error error = Error::none;
    ProtectedPayloadView payload {};

    [[nodiscard]] constexpr explicit operator bool() const noexcept
    {
        return error == Error::none;
    }
};

struct ProtectedPayloadEncodeResult {
    Error error = Error::none;
    MutableProtectedPayloadView payload {};
    std::size_t bytes_written = 0;

    [[nodiscard]] constexpr explicit operator bool() const noexcept
    {
        return error == Error::none;
    }
};

/**
 * Decodes one complete SRT packet without allocation. The returned payload
 * aliases `bytes` and remains valid only while that input remains alive and
 * unchanged. Independent calls are thread-safe.
 */
[[nodiscard]] DecodeResult decode_packet(std::span<const std::byte> bytes) noexcept;

/**
 * Encodes one packet into caller-owned storage. Inputs are not retained;
 * source payload and destination must not overlap. Only `bytes_written` bytes
 * are initialized on success.
 */
[[nodiscard]] EncodeResult encode_packet(
    const MutablePacketView& packet,
    std::span<std::byte> destination) noexcept;

// Splits the exact protected wire representation into ciphertext and its
// complete trailing tag. Truncated tags and bytes beyond the negotiated
// mode/MSS/FEC budget fail before a crypto provider is invoked.
[[nodiscard]] ProtectedPayloadDecodeResult decode_protected_payload(
    std::span<const std::byte> protected_payload,
    const CryptoPayloadBudget& budget) noexcept;

// Returns caller-owned output spans for ciphertext followed immediately by
// the complete tag. Only the returned prefix is part of the wire payload.
[[nodiscard]] ProtectedPayloadEncodeResult prepare_protected_payload(
    std::span<std::byte> destination, std::size_t plaintext_size,
    const CryptoPayloadBudget& budget) noexcept;

[[nodiscard]] constexpr bool is_known_control_type(std::uint16_t type) noexcept
{
    return type <= static_cast<std::uint16_t>(ControlType::peer_error)
        || type == static_cast<std::uint16_t>(ControlType::user_defined);
}

} // namespace robotweax::srt
