#include "robotweax/srt/connection_group.hpp"

#include <array>

namespace robotweax::srt {
namespace {

[[nodiscard]] std::uint32_t read_u32(
    const std::byte* bytes) noexcept
{
    return (std::to_integer<std::uint32_t>(bytes[0]) << 24U)
        | (std::to_integer<std::uint32_t>(bytes[1]) << 16U)
        | (std::to_integer<std::uint32_t>(bytes[2]) << 8U)
        | std::to_integer<std::uint32_t>(bytes[3]);
}

void write_u16(std::byte* bytes, std::uint16_t value) noexcept
{
    bytes[0] = static_cast<std::byte>((value >> 8U) & 0xffU);
    bytes[1] = static_cast<std::byte>(value & 0xffU);
}

void write_u32(std::byte* bytes, std::uint32_t value) noexcept
{
    bytes[0] = static_cast<std::byte>((value >> 24U) & 0xffU);
    bytes[1] = static_cast<std::byte>((value >> 16U) & 0xffU);
    bytes[2] = static_cast<std::byte>((value >> 8U) & 0xffU);
    bytes[3] = static_cast<std::byte>(value & 0xffU);
}

[[nodiscard]] std::uint16_t read_u16(const std::byte* bytes) noexcept
{
    return static_cast<std::uint16_t>(
        (std::to_integer<std::uint16_t>(bytes[0]) << 8U)
        | std::to_integer<std::uint16_t>(bytes[1]));
}

[[nodiscard]] std::uint64_t read_u64(const std::byte* bytes) noexcept
{
    return (static_cast<std::uint64_t>(read_u32(bytes)) << 32U)
        | read_u32(bytes + 4);
}

void write_u64(std::byte* bytes, std::uint64_t value) noexcept
{
    write_u32(bytes, static_cast<std::uint32_t>(value >> 32U));
    write_u32(bytes + 4, static_cast<std::uint32_t>(value & 0xffff'ffffU));
}

} // namespace

GroupMembershipDecodeResult decode_group_membership(
    const HandshakeExtensionView& extension) noexcept
{
    if (extension.type != HandshakeExtensionType::group
        || extension.content.size()
            < group_membership_content_size
        || (extension.content.size() % 4U) != 0U) {
        return {.error = Error::invalid_extension};
    }

    const std::uint32_t group_data =
        read_u32(extension.content.data() + 4);
    return {
        .membership = {
            .group_id = read_u32(extension.content.data()),
            .type = static_cast<GroupType>(
                (group_data >> 24U) & 0xffU),
            .flags = static_cast<std::uint8_t>(
                (group_data >> 16U) & 0xffU),
            .weight = static_cast<std::uint16_t>(
                group_data & 0xffffU),
        },
    };
}

ExtensionEncodeResult encode_group_membership(
    const GroupMembership& membership,
    std::span<std::byte> destination) noexcept
{
    std::array<std::byte, group_membership_content_size>
        content{};
    write_u32(content.data(), membership.group_id);
    content[4] = static_cast<std::byte>(membership.type);
    content[5] = static_cast<std::byte>(membership.flags);
    write_u16(content.data() + 6, membership.weight);
    return encode_extension(
        HandshakeExtensionType::group,
        content,
        destination);
}

MulticastDispatchDecodeResult decode_multicast_dispatch(
    const HandshakeExtensionView& extension) noexcept
{
    constexpr MulticastDispatchDecodeResult invalid {
        .error = Error::invalid_extension};
    if (extension.type != HandshakeExtensionType::multicast_dispatch
        || extension.content.size() != multicast_dispatch_content_size) {
        return invalid;
    }
    const std::byte* content = extension.content.data();
    if (read_u32(content) != multicast_dispatch_magic) {
        return invalid;
    }
    const auto family = std::to_integer<std::uint8_t>(content[8]);
    const auto flags = std::to_integer<std::uint8_t>(content[9]);
    if ((family != 4U && family != 6U)
        || (flags & ~multicast_dispatch_flag_interface_hint) != 0U
        || content[57] != std::byte {} || content[58] != std::byte {}
        || content[59] != std::byte {}) {
        return invalid;
    }
    MulticastDispatch dispatch;
    dispatch.group_id = read_u32(content + 4);
    const IpAddressFamily ip_family =
        family == 4U ? IpAddressFamily::ipv4 : IpAddressFamily::ipv6;
    dispatch.multicast_group.family = ip_family;
    dispatch.multicast_group.port = read_u16(content + 10);
    for (std::size_t index = 0; index < 16; ++index) {
        dispatch.multicast_group.address[index] =
            std::to_integer<std::uint8_t>(content[12 + index]);
    }
    dispatch.has_interface_hint =
        (flags & multicast_dispatch_flag_interface_hint) != 0U;
    dispatch.interface_hint.family = ip_family;
    if (family == 4U) {
        // Padding must be zero so two encoders cannot disagree on bytes
        // the receiver never reads.
        for (std::size_t index = 4; index < 16; ++index) {
            if (content[12 + index] != std::byte {}
                || content[28 + index] != std::byte {}) {
                return invalid;
            }
        }
        for (std::size_t index = 0; index < 4; ++index) {
            dispatch.interface_hint.address[index] =
                std::to_integer<std::uint8_t>(content[28 + index]);
        }
    } else {
        // IPv6 selects the interface by index; the address bytes are unused
        // and must be zero except the index in the last four.
        for (std::size_t index = 0; index < 12; ++index) {
            if (content[28 + index] != std::byte {}) {
                return invalid;
            }
        }
        dispatch.interface_hint.scope_id = read_u32(content + 40);
    }
    if (!dispatch.has_interface_hint
        && dispatch.interface_hint != IpEndpoint {.family = ip_family}) {
        // An absent hint carries no selector bytes.
        return invalid;
    }
    dispatch.initial_sequence = read_u32(content + 44);
    dispatch.timestamp_origin_delta_microseconds =
        static_cast<std::int64_t>(read_u64(content + 48));
    dispatch.time_to_live = std::to_integer<std::uint8_t>(content[56]);
    if (!valid_multicast_dispatch(dispatch)) {
        return invalid;
    }
    return {.dispatch = dispatch};
}

ExtensionEncodeResult encode_multicast_dispatch(
    const MulticastDispatch& dispatch,
    std::span<std::byte> destination) noexcept
{
    if (!valid_multicast_dispatch(dispatch)) {
        return {.error = Error::invalid_extension};
    }
    std::array<std::byte, multicast_dispatch_content_size> content {};
    write_u32(content.data(), multicast_dispatch_magic);
    write_u32(content.data() + 4, dispatch.group_id);
    const bool ipv4 = dispatch.multicast_group.is_ipv4();
    content[8] = static_cast<std::byte>(ipv4 ? 4U : 6U);
    content[9] = static_cast<std::byte>(dispatch.has_interface_hint
            ? multicast_dispatch_flag_interface_hint
            : 0U);
    write_u16(content.data() + 10, dispatch.multicast_group.port);
    for (std::size_t index = 0; index < (ipv4 ? 4U : 16U); ++index) {
        content[12 + index] =
            static_cast<std::byte>(dispatch.multicast_group.address[index]);
    }
    if (dispatch.has_interface_hint) {
        if (ipv4) {
            for (std::size_t index = 0; index < 4U; ++index) {
                content[28 + index] = static_cast<std::byte>(
                    dispatch.interface_hint.address[index]);
            }
        } else {
            write_u32(content.data() + 40, dispatch.interface_hint.scope_id);
        }
    }
    write_u32(content.data() + 44, dispatch.initial_sequence);
    write_u64(content.data() + 48,
        static_cast<std::uint64_t>(
            dispatch.timestamp_origin_delta_microseconds));
    content[56] = static_cast<std::byte>(dispatch.time_to_live);
    return encode_extension(
        HandshakeExtensionType::multicast_dispatch, content, destination);
}

} // namespace robotweax::srt
