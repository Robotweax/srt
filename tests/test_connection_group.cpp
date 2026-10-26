#include "test.hpp"

#include "robotweax/srt/connection_group.hpp"
#include "srt/srt.h"

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

TEST(group_membership_admits_multicast_dispatch_only_by_policy)
{
    const GroupMembership dispatch {
        .group_id = group_handle_mask | 7U,
        .type = GroupType::multicast,
        .flags = 0U,
        .weight = 0U,
    };
    // The v1.5.7 default policy never admits type 4.
    REQUIRE_EQ(validate_group_membership(dispatch),
        GroupMembershipValidation::unsupported_type);
    // An endpoint that offers dispatch groups admits a clean type 4.
    const GroupMembershipPolicy offering {.admit_multicast_dispatch = true};
    REQUIRE_EQ(validate_group_membership(dispatch, offering),
        GroupMembershipValidation::compatible);
    // Flags or a weight on a dispatch membership are not negotiable.
    GroupMembership flagged = dispatch;
    flagged.flags = 1U;
    REQUIRE_EQ(validate_group_membership(flagged, offering),
        GroupMembershipValidation::unsupported_type);
    GroupMembership weighted = dispatch;
    weighted.weight = 1U;
    REQUIRE_EQ(validate_group_membership(weighted, offering),
        GroupMembershipValidation::unsupported_type);
    // The policy does not widen anything else.
    GroupMembership balancing = dispatch;
    balancing.type = GroupType::balancing;
    REQUIRE_EQ(validate_group_membership(balancing, offering),
        GroupMembershipValidation::unsupported_type);
    GroupMembership unmasked = dispatch;
    unmasked.group_id = 7U;
    REQUIRE_EQ(validate_group_membership(unmasked, offering),
        GroupMembershipValidation::invalid_group_id);
    REQUIRE_EQ(static_cast<int>(GroupType::multicast),
        static_cast<int>(ROBOTWEAX_SRT_GTYPE_MULTICAST));
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

// --- Multicast dispatch announcement ----------------------------------------

namespace {

[[nodiscard]] MulticastDispatch sample_ipv4_dispatch()
{
    return MulticastDispatch {
        .group_id = group_handle_mask | 0x1234U,
        .multicast_group = {.address = {239, 255, 133, 10}, .port = 5'999},
        .has_interface_hint = true,
        .interface_hint = {.address = {192, 168, 7, 20}},
        .initial_sequence = 0x7fff'fff0U,
        .timestamp_origin_delta_microseconds = -1'234'567'890'123LL,
        .time_to_live = 8,
    };
}

[[nodiscard]] MulticastDispatch sample_ipv6_dispatch()
{
    IpEndpoint group = IpEndpoint::ipv6_any(6'000);
    group.address[0] = 0xff;
    group.address[1] = 0x05;
    group.address[15] = 0x11;
    IpEndpoint interface = IpEndpoint::ipv6_any();
    interface.scope_id = 3;
    return MulticastDispatch {
        .group_id = group_handle_mask | 9U,
        .multicast_group = group,
        .has_interface_hint = true,
        .interface_hint = interface,
        .initial_sequence = 0U,
        .timestamp_origin_delta_microseconds = 42,
        .time_to_live = 1,
    };
}

// Encodes, then returns the content bytes for tampering.
[[nodiscard]] std::array<std::byte, 4 + multicast_dispatch_content_size>
encoded_dispatch(const MulticastDispatch& dispatch)
{
    std::array<std::byte, 4 + multicast_dispatch_content_size> bytes {};
    const auto encoded = encode_multicast_dispatch(dispatch, bytes);
    REQUIRE(encoded);
    REQUIRE_EQ(encoded.bytes_written, bytes.size());
    return bytes;
}

[[nodiscard]] MulticastDispatchDecodeResult decode_bytes(
    std::span<const std::byte> bytes)
{
    const auto extension = decode_extension(bytes);
    REQUIRE(extension);
    return decode_multicast_dispatch(extension.extension);
}

} // namespace

TEST(multicast_dispatch_round_trips_ipv4_and_ipv6_announcements)
{
    for (const auto& sample :
        {sample_ipv4_dispatch(), sample_ipv6_dispatch()}) {
        const auto bytes = encoded_dispatch(sample);
        const auto extension = decode_extension(bytes);
        REQUIRE(extension);
        REQUIRE_EQ(extension.extension.type,
            HandshakeExtensionType::multicast_dispatch);
        REQUIRE_EQ(extension.extension.content.size(),
            multicast_dispatch_content_size);
        const auto decoded = decode_multicast_dispatch(extension.extension);
        REQUIRE(decoded);
        REQUIRE(decoded.dispatch == sample);
    }
    // An absent interface hint round-trips as "operating system default" of
    // the group's family and never as stray selector bytes.
    MulticastDispatch plain = sample_ipv4_dispatch();
    plain.has_interface_hint = false;
    plain.interface_hint = {};
    const auto decoded = decode_bytes(encoded_dispatch(plain));
    REQUIRE(decoded);
    REQUIRE(decoded.dispatch == plain);
    REQUIRE(!decoded.dispatch.has_interface_hint);
}

TEST(multicast_dispatch_encoder_rejects_invalid_announcements)
{
    std::array<std::byte, 64> bytes {};
    MulticastDispatch unmasked = sample_ipv4_dispatch();
    unmasked.group_id = 0x1234U;
    REQUIRE(!encode_multicast_dispatch(unmasked, bytes));
    MulticastDispatch unicast = sample_ipv4_dispatch();
    unicast.multicast_group = IpEndpoint::loopback(5'999);
    REQUIRE(!encode_multicast_dispatch(unicast, bytes));
    MulticastDispatch portless = sample_ipv4_dispatch();
    portless.multicast_group.port = 0;
    REQUIRE(!encode_multicast_dispatch(portless, bytes));
    MulticastDispatch wide = sample_ipv4_dispatch();
    wide.initial_sequence = 0x8000'0000U;
    REQUIRE(!encode_multicast_dispatch(wide, bytes));
    MulticastDispatch mixed = sample_ipv4_dispatch();
    mixed.interface_hint = IpEndpoint::ipv6_loopback();
    REQUIRE(!encode_multicast_dispatch(mixed, bytes));
    MulticastDispatch group_as_interface = sample_ipv4_dispatch();
    group_as_interface.interface_hint = group_as_interface.multicast_group;
    REQUIRE(!encode_multicast_dispatch(group_as_interface, bytes));
    // Too small a destination is reported, not truncated.
    std::array<std::byte, 4 + multicast_dispatch_content_size - 1> small {};
    REQUIRE(!encode_multicast_dispatch(sample_ipv4_dispatch(), small));
}

TEST(multicast_dispatch_decoder_rejects_malformed_wire_content)
{
    const auto good = encoded_dispatch(sample_ipv4_dispatch());
    REQUIRE(decode_bytes(good));
    constexpr std::size_t content = 4; // extension header precedes content

    // Wrong magic or layout version: a foreign extension on this number.
    auto tampered = good;
    tampered[content + 3] = std::byte {0x02};
    REQUIRE(!decode_bytes(tampered));
    tampered = good;
    tampered[content + 0] = std::byte {0x00};
    REQUIRE(!decode_bytes(tampered));
    // Unknown family and unknown flag bits.
    tampered = good;
    tampered[content + 8] = std::byte {5};
    REQUIRE(!decode_bytes(tampered));
    tampered = good;
    tampered[content + 9] = std::byte {0x03};
    REQUIRE(!decode_bytes(tampered));
    // Non-zero IPv4 padding in the group or interface fields.
    tampered = good;
    tampered[content + 12 + 7] = std::byte {1};
    REQUIRE(!decode_bytes(tampered));
    tampered = good;
    tampered[content + 28 + 15] = std::byte {1};
    REQUIRE(!decode_bytes(tampered));
    // Reserved tail must be zero.
    for (const std::size_t offset : {57U, 58U, 59U}) {
        tampered = good;
        tampered[content + offset] = std::byte {0xff};
        REQUIRE(!decode_bytes(tampered));
    }
    // Semantic checks apply to decoded content too.
    tampered = good;
    tampered[content + 12] = std::byte {10}; // 10.255.133.10 is unicast
    REQUIRE(!decode_bytes(tampered));
    tampered = good;
    tampered[content + 44] = std::byte {0x80}; // sequence above 31 bits
    REQUIRE(!decode_bytes(tampered));
    // Hint flag clear but selector bytes present.
    tampered = good;
    tampered[content + 9] = std::byte {0};
    REQUIRE(!decode_bytes(tampered));

    // Wrong type with otherwise valid content, and a short content length.
    const auto extension = decode_extension(good);
    REQUIRE(extension);
    HandshakeExtensionView wrong_type = extension.extension;
    wrong_type.type = HandshakeExtensionType::path_identifier;
    REQUIRE(!decode_multicast_dispatch(wrong_type));
    HandshakeExtensionView short_content = extension.extension;
    short_content.content = short_content.content.first(56);
    REQUIRE(!decode_multicast_dispatch(short_content));

    // IPv6: non-zero bytes in the unused part of the interface field.
    const auto six = encoded_dispatch(sample_ipv6_dispatch());
    REQUIRE(decode_bytes(six));
    auto six_tampered = six;
    six_tampered[content + 28] = std::byte {1};
    REQUIRE(!decode_bytes(six_tampered));
}
