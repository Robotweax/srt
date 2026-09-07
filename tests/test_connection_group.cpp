#include "test.hpp"

#include "robotweax/srt/connection_group.hpp"

#include <array>
#include <cstddef>
#include <span>

using namespace robotweax::srt;

TEST(group_membership_has_exact_hsv5_wire_layout)
{
    const GroupMembership membership{
        .group_id = 0x4001'0203U,
        .type = GroupType::broadcast,
        .flags = group_synchronize_on_message,
        .weight = 0x1234U,
    };
    std::array<std::byte, 12> bytes{};
    const auto encoded = encode_group_membership(
        membership, bytes);
    REQUIRE(encoded);
    REQUIRE_EQ(encoded.bytes_written, bytes.size());
    REQUIRE_EQ(bytes[0], std::byte{0});
    REQUIRE_EQ(bytes[1], std::byte{8});
    REQUIRE_EQ(bytes[2], std::byte{0});
    REQUIRE_EQ(bytes[3], std::byte{2});
    REQUIRE_EQ(bytes[4], std::byte{0x40});
    REQUIRE_EQ(bytes[5], std::byte{0x01});
    REQUIRE_EQ(bytes[6], std::byte{0x02});
    REQUIRE_EQ(bytes[7], std::byte{0x03});
    REQUIRE_EQ(bytes[8], std::byte{0x01});
    REQUIRE_EQ(bytes[9], std::byte{0x01});
    REQUIRE_EQ(bytes[10], std::byte{0x12});
    REQUIRE_EQ(bytes[11], std::byte{0x34});

    const auto extension = decode_extension(bytes);
    REQUIRE(extension);
    const auto decoded =
        decode_group_membership(extension.extension);
    REQUIRE(decoded);
    REQUIRE_EQ(decoded.membership.group_id,
        membership.group_id);
    REQUIRE_EQ(decoded.membership.type, membership.type);
    REQUIRE_EQ(decoded.membership.flags, membership.flags);
    REQUIRE_EQ(decoded.membership.weight, membership.weight);
    REQUIRE_EQ(validate_group_membership(decoded.membership),
        GroupMembershipValidation::compatible);
}

TEST(group_membership_preserves_reserved_and_unknown_types)
{
    for (const auto type : {
             GroupType::balancing,
             GroupType::multicast,
             static_cast<GroupType>(0xffU)}) {
        std::array<std::byte, 12> bytes{};
        const GroupMembership membership{
            .group_id = group_handle_mask | 91U,
            .type = type,
            .flags = 0xa5U,
            .weight = 65'535U,
        };
        const auto encoded =
            encode_group_membership(membership, bytes);
        REQUIRE(encoded);
        const auto extension = decode_extension(bytes);
        REQUIRE(extension);
        const auto decoded =
            decode_group_membership(extension.extension);
        REQUIRE(decoded);
        REQUIRE_EQ(decoded.membership.type, type);
        REQUIRE_EQ(decoded.membership.flags, 0xa5U);
        REQUIRE_EQ(decoded.membership.weight, 65'535U);
        REQUIRE_EQ(
            validate_group_membership(decoded.membership),
            GroupMembershipValidation::unsupported_type);
    }
}

TEST(group_membership_validation_requires_group_handle_mask)
{
    const GroupMembership membership{
        .group_id = 17U,
        .type = GroupType::backup,
    };
    REQUIRE_EQ(validate_group_membership(membership),
        GroupMembershipValidation::invalid_group_id);
}

TEST(group_membership_decoder_accepts_bounded_future_tail)
{
    const std::array<std::byte, 12> content{
        std::byte{0x40}, std::byte{0}, std::byte{0}, std::byte{9},
        std::byte{2}, std::byte{0x80}, std::byte{0}, std::byte{7},
        std::byte{0xde}, std::byte{0xad}, std::byte{0xbe}, std::byte{0xef},
    };
    const HandshakeExtensionView extension{
        .type = HandshakeExtensionType::group,
        .content = content,
    };
    const auto decoded = decode_group_membership(extension);
    REQUIRE(decoded);
    REQUIRE_EQ(decoded.membership.group_id,
        group_handle_mask | 9U);
    REQUIRE_EQ(decoded.membership.type, GroupType::backup);
    REQUIRE_EQ(decoded.membership.flags, 0x80U);
    REQUIRE_EQ(decoded.membership.weight, 7U);
}

TEST(group_membership_decoder_rejects_wrong_type_and_short_content)
{
    const std::array<std::byte, 8> content{};
    REQUIRE_EQ(decode_group_membership({
            .type = HandshakeExtensionType::packet_filter,
            .content = content,
        }).error,
        Error::invalid_extension);
    REQUIRE_EQ(decode_group_membership({
            .type = HandshakeExtensionType::group,
            .content = std::span{content}.first(4),
        }).error,
        Error::invalid_extension);
    REQUIRE_EQ(decode_group_membership({
            .type = HandshakeExtensionType::group,
            .content = std::span{content}.first(7),
        }).error,
        Error::invalid_extension);
}

TEST(group_membership_encoder_reports_small_destination)
{
    std::array<std::byte, 11> bytes{};
    REQUIRE_EQ(encode_group_membership({}, bytes).error,
        Error::buffer_too_small);
}
