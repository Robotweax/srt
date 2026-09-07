#pragma once

#include "robotweax/srt/error.hpp"
#include "srt/version.h"

#include <cstddef>
#include <cstdint>
#include <array>
#include <span>
#include <string_view>

namespace robotweax::srt {

inline constexpr std::size_t extension_header_size = 4;
inline constexpr std::size_t handshake_extension_content_size = 12;
inline constexpr std::size_t maximum_stream_id_size = 512;
inline constexpr std::uint32_t minimum_hsv5_srt_version =
    0x0001'0300U;

enum class HandshakeExtensionType : std::uint16_t {
    handshake_request = 1,
    handshake_response = 2,
    key_material_request = 3,
    key_material_response = 4,
    stream_id = 5,
    congestion = 6,
    packet_filter = 7,
    group = 8,
};

enum class CongestionController : std::uint8_t {
    live = 0,
    file = 1,
};

[[nodiscard]] constexpr std::string_view congestion_controller_name(
    CongestionController controller) noexcept
{
    switch (controller) {
    case CongestionController::live:
        return "live";
    case CongestionController::file:
        return "file";
    }
    return {};
}

[[nodiscard]] bool parse_congestion_controller(
    std::string_view name,
    CongestionController& controller) noexcept;

enum class HandshakeExtensionFlag : std::uint32_t {
    tsbpd_send = 0x0000'0001U,
    tsbpd_receive = 0x0000'0002U,
    crypt = 0x0000'0004U,
    too_late_packet_drop = 0x0000'0008U,
    periodic_nak = 0x0000'0010U,
    retransmit_flag = 0x0000'0020U,
    stream = 0x0000'0040U,
    packet_filter = 0x0000'0080U,
};

[[nodiscard]] constexpr std::uint32_t operator|(
    HandshakeExtensionFlag left,
    HandshakeExtensionFlag right) noexcept
{
    return static_cast<std::uint32_t>(left) | static_cast<std::uint32_t>(right);
}

struct HandshakeExtensionParameters {
    std::uint32_t srt_version = SRT_VERSION_VALUE;
    std::uint32_t flags = static_cast<std::uint32_t>(HandshakeExtensionFlag::tsbpd_send)
        | static_cast<std::uint32_t>(HandshakeExtensionFlag::tsbpd_receive)
        | static_cast<std::uint32_t>(HandshakeExtensionFlag::crypt)
        | static_cast<std::uint32_t>(HandshakeExtensionFlag::too_late_packet_drop)
        | static_cast<std::uint32_t>(HandshakeExtensionFlag::periodic_nak)
        | static_cast<std::uint32_t>(HandshakeExtensionFlag::retransmit_flag)
        | static_cast<std::uint32_t>(HandshakeExtensionFlag::packet_filter);
    std::uint16_t receiver_tsbpd_delay_milliseconds = 120;
    std::uint16_t sender_tsbpd_delay_milliseconds = 120;
};

[[nodiscard]] constexpr bool buffer_mode(
    const HandshakeExtensionParameters& parameters) noexcept
{
    return (parameters.flags
        & static_cast<std::uint32_t>(
            HandshakeExtensionFlag::stream)) != 0U;
}

[[nodiscard]] constexpr bool compatible_transmission_mode(
    const HandshakeExtensionParameters& local,
    const HandshakeExtensionParameters& peer) noexcept
{
    return buffer_mode(local) == buffer_mode(peer);
}

struct NegotiatedLiveOptions {
    bool send_tsbpd = false;
    bool receive_tsbpd = false;
    bool too_late_packet_drop = false;
    bool periodic_nak = false;
    bool retransmit_flag = false;
    std::uint16_t receive_delay_milliseconds = 0;
    std::uint16_t peer_receive_delay_milliseconds = 0;
};

[[nodiscard]] NegotiatedLiveOptions negotiate_live_options(
    const HandshakeExtensionParameters& local,
    const HandshakeExtensionParameters& peer) noexcept;

/** Non-owning view whose content aliases the encoded extension input. */
struct HandshakeExtensionView {
    HandshakeExtensionType type = HandshakeExtensionType::handshake_request;
    std::span<const std::byte> content{};
};

struct ExtensionResult {
    Error error = Error::none;
    HandshakeExtensionView extension{};
    std::size_t bytes_consumed = 0;

    [[nodiscard]] constexpr explicit operator bool() const noexcept
    {
        return error == Error::none;
    }
};

struct ExtensionEncodeResult {
    Error error = Error::none;
    std::size_t bytes_written = 0;

    [[nodiscard]] constexpr explicit operator bool() const noexcept
    {
        return error == Error::none;
    }
};

struct HandshakeParametersResult {
    Error error = Error::none;
    HandshakeExtensionParameters parameters{};

    [[nodiscard]] constexpr explicit operator bool() const noexcept
    {
        return error == Error::none;
    }
};

struct ExtensionTextResult {
    Error error = Error::none;
    std::array<char, maximum_stream_id_size + 1U>
        text{};
    std::size_t size = 0;

    [[nodiscard]] constexpr explicit operator bool() const noexcept
    {
        return error == Error::none;
    }

    [[nodiscard]] constexpr std::string_view view() const noexcept
    {
        // The returned view aliases this result object.
        return {text.data(), size};
    }
};

// Stream IDs are carried during connection setup and have a protocol-defined
// 512-byte limit. Keeping the value bounded avoids allocation in the handshake
// state machines while preserving the exact byte sequence supplied through the
// compatible socket API.
struct StreamId {
    std::array<char, maximum_stream_id_size> bytes{};
    std::size_t size = 0;

    [[nodiscard]] constexpr bool assign(
        std::string_view value) noexcept
    {
        if (value.size() > bytes.size()) {
            return false;
        }
        bytes.fill('\0');
        for (std::size_t index = 0; index < value.size(); ++index) {
            bytes[index] = value[index];
        }
        size = value.size();
        return true;
    }

    [[nodiscard]] constexpr bool empty() const noexcept
    {
        return size == 0U;
    }

    [[nodiscard]] constexpr std::string_view view() const noexcept
    {
        // The returned view aliases this StreamId object.
        return {bytes.data(), size};
    }
};

/**
 * Decodes one extension without allocation. The returned content aliases
 * `bytes` and is invalidated when that input is modified or destroyed.
 */
[[nodiscard]] ExtensionResult decode_extension(
    std::span<const std::byte> bytes) noexcept;

[[nodiscard]] ExtensionEncodeResult encode_extension(
    HandshakeExtensionType type,
    std::span<const std::byte> content,
    std::span<std::byte> destination) noexcept;

[[nodiscard]] HandshakeParametersResult decode_handshake_parameters(
    const HandshakeExtensionView& extension) noexcept;

[[nodiscard]] ExtensionEncodeResult encode_handshake_parameters(
    HandshakeExtensionType type,
    const HandshakeExtensionParameters& parameters,
    std::span<std::byte> destination) noexcept;

[[nodiscard]] ExtensionEncodeResult encode_stream_id(
    std::string_view stream_id,
    std::span<std::byte> destination) noexcept;

[[nodiscard]] ExtensionEncodeResult encode_extension_text(
    HandshakeExtensionType type,
    std::string_view text,
    std::span<std::byte> destination) noexcept;

[[nodiscard]] ExtensionTextResult decode_extension_text(
    const HandshakeExtensionView& extension) noexcept;

} // namespace robotweax::srt
