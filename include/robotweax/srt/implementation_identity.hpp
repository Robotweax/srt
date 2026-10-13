#pragma once

#include "robotweax/srt/handshake_extensions.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace robotweax::srt {

// Provisional Robotweax experiment, not a registered SRT extension allocation.
inline constexpr std::uint16_t implementation_identity_extension_type =
    static_cast<std::uint16_t>(HandshakeExtensionType::implementation_identity);
inline constexpr std::size_t maximum_implementation_name_size = 32;
inline constexpr std::size_t maximum_implementation_release_size = 64;
inline constexpr std::size_t implementation_identity_prefix_size = 12;

struct ImplementationIdentity {
    std::array<char, maximum_implementation_name_size + 1> name {};
    std::array<char, maximum_implementation_release_size + 1> release {};
    std::uint8_t name_size = 0;
    std::uint8_t release_size = 0;

    [[nodiscard]] bool available() const noexcept
    {
        return name_size != 0;
    }
    bool operator==(const ImplementationIdentity&) const noexcept = default;
};

[[nodiscard]] ImplementationIdentity make_implementation_identity(
    std::string_view name, std::string_view release) noexcept;
// Unknown/invalid contents return an empty observation, not a protocol error.
[[nodiscard]] ImplementationIdentity decode_implementation_identity(
    std::span<const std::byte> content) noexcept;
[[nodiscard]] std::size_t implementation_identity_record_size(
    const ImplementationIdentity& identity) noexcept;
[[nodiscard]] ExtensionEncodeResult encode_implementation_identity(
    const ImplementationIdentity& identity,
    std::span<std::byte> destination) noexcept;

} // namespace robotweax::srt
