#pragma once

#include <cstdint>
#include <string_view>

namespace robotweax::srt {

enum class Error : std::uint8_t {
    none = 0,
    buffer_too_small,
    packet_too_short,
    invalid_packet_type,
    invalid_control_type,
    invalid_control_payload,
    invalid_handshake_size,
    invalid_handshake_value,
    invalid_extension,
    invalid_state,
    io_error,
    would_block,
    unsupported,
    invalid_key_material,
    cryptographic_failure,
    authentication_failure,
    invalid_payload_size,
};

[[nodiscard]] constexpr std::string_view describe(Error error) noexcept
{
    switch (error) {
    case Error::none: return "no error";
    case Error::buffer_too_small: return "buffer too small";
    case Error::packet_too_short: return "packet shorter than the SRT header";
    case Error::invalid_packet_type: return "invalid packet type";
    case Error::invalid_control_type: return "invalid control packet type";
    case Error::invalid_control_payload: return "invalid control packet payload";
    case Error::invalid_handshake_size: return "invalid handshake payload size";
    case Error::invalid_handshake_value: return "invalid handshake field";
    case Error::invalid_extension: return "invalid handshake extension";
    case Error::invalid_state: return "event is invalid in the current state";
    case Error::io_error: return "operating system I/O error";
    case Error::would_block: return "operation would block";
    case Error::unsupported: return "feature is not supported";
    case Error::invalid_key_material: return "invalid SRT key material";
    case Error::cryptographic_failure: return "cryptographic operation failed";
    case Error::authentication_failure:
        return "authenticated decryption failed";
    case Error::invalid_payload_size:
        return "invalid protected payload size";
    }
    return "unknown error";
}

} // namespace robotweax::srt
