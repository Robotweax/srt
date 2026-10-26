#pragma once

#include "robotweax/srt/error.hpp"
#include "robotweax/srt/handshake_extensions.hpp"
#include "robotweax/srt/udp.hpp"

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

// Which group categories the validating endpoint admits. The v1.5.7 profile
// is bonding only. A multicast dispatch group is a Robotweax extension and
// is admitted only where the local endpoint explicitly offers it: a Listener
// configured as a dispatch sender, or a Caller that created a dispatch group.
struct GroupMembershipPolicy {
    bool admit_multicast_dispatch = false;
};

[[nodiscard]] constexpr GroupMembershipValidation validate_group_membership(
    const GroupMembership& membership,
    GroupMembershipPolicy policy = {}) noexcept
{
    if ((membership.group_id & group_handle_mask) == 0U) {
        return GroupMembershipValidation::invalid_group_id;
    }
    switch (membership.type) {
    case GroupType::broadcast:
    case GroupType::backup:
        return GroupMembershipValidation::compatible;
    case GroupType::multicast:
        // Dispatch groups carry no weight and no flags on the wire.
        if (policy.admit_multicast_dispatch && membership.flags == 0U
            && membership.weight == 0U) {
            return GroupMembershipValidation::compatible;
        }
        return GroupMembershipValidation::unsupported_type;
    case GroupType::undefined:
    case GroupType::balancing:
    default:
        return GroupMembershipValidation::unsupported_type;
    }
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

// --- Multicast dispatch announcement ---------------------------------------
//
// Sent by a Listener acting as dispatch sender in its conclusion handshake,
// after it admitted the Caller's type-4 membership. It tells the receiver
// which multicast group carries the payload, which destination socket ID
// those datagrams carry, where the stream currently is, and how the group
// timeline relates to this connection. The session key is not here; it
// travels in the ordinary KMREQ/KMRSP exchange of the unicast connection.
//
// Layout (60 bytes, network byte order):
//   0  u32  magic 'RWMC' with layout version in the low byte (0x5257'4D01)
//   4  u32  dispatch group ID (destination socket ID of payload datagrams)
//   8  u8   address family: 4 or 6
//   9  u8   flags (bit 0: interface hint present)
//  10  u16  multicast UDP port
//  12  16   multicast group address (IPv4 in the first 4 bytes, zero padded)
//  28  16   interface hint (IPv4 address, or IPv6 scope_id in the last 4)
//  44  u32  initial sequence (group next_send_sequence at admission)
//  48  i64  timestamp origin delta in microseconds (group - connection)
//  56  u8   sender multicast TTL / hop limit (informational)
//  57  3    reserved, zero

inline constexpr std::size_t multicast_dispatch_content_size = 60;
inline constexpr std::uint32_t multicast_dispatch_magic = 0x5257'4D01U;
inline constexpr std::uint8_t multicast_dispatch_flag_interface_hint = 0x01U;

struct MulticastDispatch {
    std::uint32_t group_id = 0;
    IpEndpoint multicast_group {};
    // Family matches multicast_group; IPv4 selects by address, IPv6 by
    // scope_id. has_interface_hint false means "operating system default".
    bool has_interface_hint = false;
    IpEndpoint interface_hint {};
    std::uint32_t initial_sequence = 0;
    std::int64_t timestamp_origin_delta_microseconds = 0;
    std::uint8_t time_to_live = 1;

    friend constexpr bool operator==(
        const MulticastDispatch&, const MulticastDispatch&) noexcept = default;
};

struct MulticastDispatchDecodeResult {
    Error error = Error::none;
    MulticastDispatch dispatch {};

    [[nodiscard]] constexpr explicit operator bool() const noexcept
    {
        return error == Error::none;
    }
};

// Semantic validity shared by encoder and decoder: masked group ID, a
// multicast address of a recognised family with a nonzero port, a 31-bit
// sequence, and an interface hint of the same family that is not itself a
// multicast address.
[[nodiscard]] constexpr bool valid_multicast_dispatch(
    const MulticastDispatch& dispatch) noexcept
{
    if ((dispatch.group_id & group_handle_mask) == 0U) {
        return false;
    }
    if (!dispatch.multicast_group.is_multicast()
        || dispatch.multicast_group.port == 0U
        || dispatch.initial_sequence > 0x7fff'ffffU) {
        return false;
    }
    if (dispatch.has_interface_hint) {
        if (dispatch.interface_hint.family != dispatch.multicast_group.family
            || dispatch.interface_hint.is_multicast()) {
            return false;
        }
    }
    return true;
}

// Rejects a wrong type, length, magic/version, family or reserved byte, and
// any payload that fails valid_multicast_dispatch. A foreign extension that
// happens to use the same type number is refused by the magic.
[[nodiscard]] MulticastDispatchDecodeResult decode_multicast_dispatch(
    const HandshakeExtensionView& extension) noexcept;

[[nodiscard]] ExtensionEncodeResult encode_multicast_dispatch(
    const MulticastDispatch& dispatch,
    std::span<std::byte> destination) noexcept;

} // namespace robotweax::srt
