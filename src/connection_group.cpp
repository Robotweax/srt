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

} // namespace robotweax::srt
