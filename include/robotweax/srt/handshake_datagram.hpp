#pragma once

#include "robotweax/srt/handshake.hpp"
#include "robotweax/srt/packet.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace robotweax::srt {

struct HandshakeDatagramDecodeResult {
    Error error = Error::none;
    HandshakeMessage message{};
    ControlHeader control{};

    [[nodiscard]] constexpr explicit operator bool() const noexcept
    {
        return error == Error::none;
    }
};

struct HandshakeDatagramEncodeResult {
    Error error = Error::none;
    std::size_t bytes_written = 0;

    [[nodiscard]] constexpr explicit operator bool() const noexcept
    {
        return error == Error::none;
    }
};

[[nodiscard]] HandshakeDatagramDecodeResult decode_handshake_datagram(
    std::span<const std::byte> datagram) noexcept;

[[nodiscard]] HandshakeDatagramEncodeResult encode_handshake_datagram(
    const HandshakeAction& action,
    PacketTimestamp timestamp,
    std::uint32_t destination_socket_id,
    std::span<std::byte> destination) noexcept;

} // namespace robotweax::srt
