#pragma once

#include "robotweax/srt/error.hpp"
#include "robotweax/srt/handshake_extensions.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace robotweax::srt {

inline constexpr std::size_t group_membership_content_size = 8;
inline constexpr std::uint32_t group_handle_mask = 0x4000'0000U;
inline constexpr std::uint8_t group_synchronize_on_message = 0x01U;

// Values 3 and 4 are assigned by the published SRT draft but are not exposed
// as supported modes by the Haivision v1.5.7 public enum. Keeping them named
// internally prevents accidental reuse while still allowing safe decoding.
enum class GroupType : std::uint8_t {
    undefined = 0,
    broadcast = 1,
    backup = 2,
    balancing = 3,
    multicast = 4,
};

struct GroupMembership {
    std::uint32_t group_id = 0;
    GroupType type = GroupType::undefined;
    std::uint8_t flags = 0;
    std::uint16_t weight = 0;
};

struct GroupMembershipDecodeResult {
    Error error = Error::none;
    GroupMembership membership{};

    [[nodiscard]] constexpr explicit operator bool() const noexcept
    {
        return error == Error::none;
    }
};

enum class GroupMembershipValidation : std::uint8_t {
    compatible,
    invalid_group_id,
    unsupported_type,
};

[[nodiscard]] constexpr GroupMembershipValidation
validate_group_membership(
    const GroupMembership& membership) noexcept
{
    if ((membership.group_id & group_handle_mask) == 0U) {
        return GroupMembershipValidation::invalid_group_id;
    }
    if (membership.type != GroupType::broadcast
        && membership.type != GroupType::backup) {
        return GroupMembershipValidation::unsupported_type;
    }
    return GroupMembershipValidation::compatible;
}

// Decoding is structural and preserves draft-reserved or unknown type/flag
// values. Call validate_group_membership before admitting a v1.5.7-compatible
// connection. Content following the mandatory two words is ignored so a
// future peer can append fields without making the decoder read out of bounds.
[[nodiscard]] GroupMembershipDecodeResult decode_group_membership(
    const HandshakeExtensionView& extension) noexcept;

[[nodiscard]] ExtensionEncodeResult encode_group_membership(
    const GroupMembership& membership,
    std::span<std::byte> destination) noexcept;

} // namespace robotweax::srt
