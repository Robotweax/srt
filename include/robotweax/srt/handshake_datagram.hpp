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

#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_PEERAUTH
// Copy a received or sealed payload exactly. Only the outer packet header is
// regenerated. This is also the adapter for moving received setup packets
// between inboxes without changing transcript bytes. Input and output must
// not overlap. Successful encoding does not authenticate the payload.
[[nodiscard]] HandshakeDatagramEncodeResult encode_peer_authentication_datagram(
    std::span<const std::byte> payload, PacketTimestamp timestamp,
    std::uint32_t destination_socket_id,
    std::span<std::byte> destination) noexcept;
#endif

[[nodiscard]] HandshakeDatagramEncodeResult encode_handshake_datagram(
    const HandshakeAction& action,
    PacketTimestamp timestamp,
    std::uint32_t destination_socket_id,
    std::span<std::byte> destination) noexcept;

} // namespace robotweax::srt
