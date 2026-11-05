#include "test.hpp"
#include "crypto_test_helpers.hpp"

#include "robotweax/srt/handshake_datagram.hpp"
#include "robotweax/srt/udp.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <thread>

using namespace robotweax::srt;

namespace {

std::uint32_t datagram_test_cookie(const Handshake& packet, void* context) noexcept
{
    return packet.socket_id ^ *static_cast<const std::uint32_t*>(context);
}

bool negotiate_test_group(
    const GroupMembership& peer,
    GroupMembership& local,
    void* context) noexcept
{
    local = peer;
    local.group_id = group_handle_mask
        | *static_cast<const std::uint32_t*>(context);
    return true;
}

UdpIoResult receive_datagram(UdpSocket& socket, std::span<std::byte> destination)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
    for (;;) {
        auto received = socket.receive_from(destination);
        if (received.error != Error::would_block
            || std::chrono::steady_clock::now() >= deadline) {
            return received;
        }
        std::this_thread::yield();
    }
}

HandshakeDatagramDecodeResult exchange(
    UdpSocket& sender,
    UdpSocket& receiver,
    Ipv4Endpoint receiver_address,
    const HandshakeAction& action,
    std::uint32_t destination_socket_id)
{
    std::array<std::byte, 1500> outgoing{};
    const auto encoded = encode_handshake_datagram(
        action, PacketTimestamp{42}, destination_socket_id, outgoing);
    REQUIRE(encoded);
    const auto sent = sender.send_to(
        std::span{outgoing}.first(encoded.bytes_written), receiver_address);
    REQUIRE(sent);

    std::array<std::byte, 1500> incoming{};
    const auto received = receive_datagram(receiver, incoming);
    REQUIRE(received);
    return decode_handshake_datagram(
        std::span{incoming}.first(received.bytes_transferred));
}

HandshakeDatagramDecodeResult encode_then_decode(
    const HandshakeAction& action,
    std::uint32_t destination_socket_id)
{
    std::array<std::byte, 1500> bytes{};
    const auto encoded = encode_handshake_datagram(
        action, PacketTimestamp{77}, destination_socket_id, bytes);
    REQUIRE(encoded);
    return decode_handshake_datagram(
        std::span{bytes}.first(encoded.bytes_written));
}

} // namespace

TEST(handshake_datagram_encodes_hsreq_and_control_header)
{
    HandshakeAction action;
    action.kind = HandshakeActionKind::send;
    action.packet.version = handshake_version_5;
    action.packet.request = HandshakeRequest::conclusion;
    action.packet.extension_field = 1;
    action.has_handshake_extension = true;
    action.extension_type = HandshakeExtensionType::handshake_request;

    std::array<std::byte, 80> bytes{};
    const auto encoded = encode_handshake_datagram(
        action, PacketTimestamp{17}, 900, bytes);
    REQUIRE(encoded);
    REQUIRE_EQ(encoded.bytes_written, 80U);

    const auto decoded = decode_handshake_datagram(bytes);
    REQUIRE(decoded);
    REQUIRE_EQ(decoded.control.destination_socket_id, 900U);
    REQUIRE_EQ(decoded.control.timestamp, PacketTimestamp{17});
    REQUIRE(decoded.message.has_handshake_extension);
    REQUIRE_EQ(decoded.message.extension_type, HandshakeExtensionType::handshake_request);
}

TEST(handshake_datagram_skips_unknown_extensions_in_any_record_order)
{
    HandshakeAction action;
    action.kind = HandshakeActionKind::send;
    action.packet.version = handshake_version_5;
    action.packet.request = HandshakeRequest::conclusion;
    action.packet.extension_field =
        handshake_extension_flag_handshake
        | handshake_extension_flag_config;
    action.has_handshake_extension = true;
    action.extension_type =
        HandshakeExtensionType::handshake_request;
    action.has_stream_id_extension = true;
    REQUIRE(action.stream_id.assign("edge/test"));
    action.has_congestion_extension = true;
    action.congestion_controller = CongestionController::file;

    std::array<std::byte, 128> canonical{};
    const auto encoded = encode_handshake_datagram(
        action, PacketTimestamp{17}, 900U, canonical);
    REQUIRE(encoded);
    REQUIRE_EQ(encoded.bytes_written, 104U);

    constexpr std::size_t fixed_size =
        packet_header_size + handshake_size;
    constexpr std::size_t handshake_record_size = 16U;
    constexpr std::size_t stream_id_record_size = 16U;
    constexpr std::size_t congestion_record_size = 8U;
    constexpr std::size_t unknown_record_size = 8U;
    std::array<std::byte, 112> reordered{};
    std::copy_n(canonical.begin(), fixed_size, reordered.begin());

    std::size_t destination = fixed_size;
    const std::size_t congestion_offset = fixed_size
        + handshake_record_size + stream_id_record_size;
    std::copy_n(canonical.begin() + congestion_offset,
        congestion_record_size, reordered.begin() + destination);
    destination += congestion_record_size;

    const std::array<std::byte, unknown_record_size> unknown{
        std::byte{0x12}, std::byte{0x34},
        std::byte{0x00}, std::byte{0x01},
        std::byte{0xde}, std::byte{0xad},
        std::byte{0xbe}, std::byte{0xef},
    };
    std::copy(unknown.begin(), unknown.end(),
        reordered.begin() + destination);
    destination += unknown.size();

    std::copy_n(canonical.begin() + fixed_size,
        handshake_record_size, reordered.begin() + destination);
    destination += handshake_record_size;
    std::copy_n(canonical.begin() + fixed_size
            + handshake_record_size,
        stream_id_record_size, reordered.begin() + destination);
    destination += stream_id_record_size;
    REQUIRE_EQ(destination, reordered.size());

    const auto decoded = decode_handshake_datagram(reordered);
    REQUIRE(decoded);
    REQUIRE(decoded.message.has_handshake_extension);
    REQUIRE_EQ(decoded.message.extension_type,
        HandshakeExtensionType::handshake_request);
    REQUIRE(decoded.message.has_stream_id_extension);
    REQUIRE_EQ(decoded.message.stream_id.view(), "edge/test");
    REQUIRE(decoded.message.has_congestion_extension);
    REQUIRE_EQ(decoded.message.congestion_controller,
        CongestionController::file);
    REQUIRE(decoded.message.has_unknown_extension);
}

TEST(handshake_datagram_ignores_an_unadvertised_unknown_extension)
{
    HandshakeAction action;
    action.kind = HandshakeActionKind::send;
    action.packet.version = handshake_version_5;
    action.packet.request = HandshakeRequest::conclusion;

    std::array<std::byte, 64> fixed{};
    const auto encoded = encode_handshake_datagram(
        action, PacketTimestamp{17}, 900U, fixed);
    REQUIRE(encoded);
    REQUIRE_EQ(encoded.bytes_written, fixed.size());

    std::array<std::byte, 68> datagram{};
    std::copy(fixed.begin(), fixed.end(), datagram.begin());
    datagram[64] = std::byte{0x7f};
    datagram[65] = std::byte{0xff};
    datagram[66] = std::byte{0x00};
    datagram[67] = std::byte{0x00};

    const auto decoded = decode_handshake_datagram(datagram);
    REQUIRE(decoded);
    REQUIRE(!decoded.message.has_handshake_extension);
    REQUIRE(!decoded.message.has_key_material_extension);
    REQUIRE(!decoded.message.has_stream_id_extension);
    REQUIRE(decoded.message.has_unknown_extension);
}

TEST(handshake_datagram_rejects_known_extensions_without_their_category_flag)
{
    HandshakeAction handshake;
    handshake.kind = HandshakeActionKind::send;
    handshake.packet.version = handshake_version_5;
    handshake.packet.request = HandshakeRequest::conclusion;
    handshake.has_handshake_extension = true;
    handshake.extension_type =
        HandshakeExtensionType::handshake_request;

    std::array<std::byte, 80> handshake_bytes{};
    const auto handshake_encoded = encode_handshake_datagram(
        handshake, PacketTimestamp{17}, 900U, handshake_bytes);
    REQUIRE(handshake_encoded);
    REQUIRE_EQ(decode_handshake_datagram(handshake_bytes).error,
        Error::invalid_extension);

    HandshakeAction config;
    config.kind = HandshakeActionKind::send;
    config.packet.version = handshake_version_5;
    config.packet.request = HandshakeRequest::conclusion;
    config.packet.extension_field =
        handshake_extension_flag_handshake;
    config.has_handshake_extension = true;
    config.extension_type =
        HandshakeExtensionType::handshake_request;
    config.has_stream_id_extension = true;
    REQUIRE(config.stream_id.assign("hidden"));

    std::array<std::byte, 92> config_bytes{};
    const auto config_encoded = encode_handshake_datagram(
        config, PacketTimestamp{17}, 900U, config_bytes);
    REQUIRE(config_encoded);
    REQUIRE_EQ(decode_handshake_datagram(config_bytes).error,
        Error::invalid_extension);
}

TEST(handshake_datagram_rejects_duplicate_known_and_truncated_unknown_records)
{
    HandshakeAction action;
    action.kind = HandshakeActionKind::send;
    action.packet.version = handshake_version_5;
    action.packet.request = HandshakeRequest::conclusion;
    action.packet.extension_field =
        handshake_extension_flag_handshake;
    action.has_handshake_extension = true;
    action.extension_type =
        HandshakeExtensionType::handshake_request;

    std::array<std::byte, 80> single{};
    const auto encoded = encode_handshake_datagram(
        action, PacketTimestamp{17}, 900U, single);
    REQUIRE(encoded);

    std::array<std::byte, 96> duplicate{};
    std::copy(single.begin(), single.end(), duplicate.begin());
    std::copy_n(single.begin() + packet_header_size
            + handshake_size,
        16U, duplicate.begin() + single.size());
    REQUIRE_EQ(decode_handshake_datagram(duplicate).error,
        Error::invalid_extension);

    HandshakeAction fixed_action;
    fixed_action.kind = HandshakeActionKind::send;
    fixed_action.packet.version = handshake_version_5;
    fixed_action.packet.request = HandshakeRequest::conclusion;
    std::array<std::byte, 64> fixed{};
    const auto fixed_encoded = encode_handshake_datagram(
        fixed_action, PacketTimestamp{17}, 900U, fixed);
    REQUIRE(fixed_encoded);

    std::array<std::byte, 72> truncated{};
    std::copy(fixed.begin(), fixed.end(), truncated.begin());
    truncated[64] = std::byte{0x12};
    truncated[65] = std::byte{0x34};
    truncated[66] = std::byte{0x00};
    truncated[67] = std::byte{0x02};
    truncated[68] = std::byte{0xde};
    truncated[69] = std::byte{0xad};
    truncated[70] = std::byte{0xbe};
    truncated[71] = std::byte{0xef};
    REQUIRE_EQ(decode_handshake_datagram(truncated).error,
        Error::invalid_extension);
}

TEST(handshake_datagram_chains_and_decodes_packet_filter_configuration)
{
    const auto filter =
        parse_packet_filter_configuration(
            "fec,cols:10,arq:onreq");
    REQUIRE(filter);

    HandshakeAction action;
    action.kind = HandshakeActionKind::send;
    action.packet.version = handshake_version_5;
    action.packet.request =
        HandshakeRequest::conclusion;
    action.packet.extension_field = 5U;
    action.has_handshake_extension = true;
    action.extension_type =
        HandshakeExtensionType::handshake_request;
    action.has_packet_filter_extension = true;
    action.packet_filter_configuration =
        filter.configuration;

    const auto decoded =
        encode_then_decode(action, 902U);
    REQUIRE(decoded);
    REQUIRE(decoded.message.has_handshake_extension);
    REQUIRE(decoded.message
        .has_packet_filter_extension);
    REQUIRE_EQ(decoded.message
            .packet_filter_configuration.view(),
        filter.configuration.view());
    REQUIRE_EQ(decoded.message
            .packet_filter_configuration.columns,
        10U);
    REQUIRE_EQ(decoded.message
            .packet_filter_configuration.arq,
        PacketFilterArqLevel::on_request);
}

TEST(handshake_datagram_round_trips_the_control_v1_wire_identity)
{
    HandshakeAction action;
    action.kind = HandshakeActionKind::send;
    action.packet.version = handshake_version_5;
    action.packet.request = HandshakeRequest::conclusion;
    action.packet.extension_field = 5U;
    action.has_handshake_extension = true;
    action.extension_type = HandshakeExtensionType::handshake_request;
    action.has_congestion_extension = true;
    action.congestion_controller = CongestionController::control;

    const auto decoded = encode_then_decode(action, 905U);
    REQUIRE(decoded);
    REQUIRE(decoded.message.has_congestion_extension);
    REQUIRE_EQ(
        decoded.message.congestion_controller, CongestionController::control);
}

TEST(handshake_datagram_chains_congestion_and_packet_filter_configuration)
{
    const auto filter =
        parse_packet_filter_configuration(
            "fec,cols:10,arq:onreq");
    REQUIRE(filter);

    HandshakeAction action;
    action.kind = HandshakeActionKind::send;
    action.packet.version = handshake_version_5;
    action.packet.request =
        HandshakeRequest::conclusion;
    action.packet.extension_field = 5U;
    action.has_handshake_extension = true;
    action.extension_type =
        HandshakeExtensionType::handshake_request;
    action.has_congestion_extension = true;
    action.congestion_controller =
        CongestionController::file;
    action.has_packet_filter_extension = true;
    action.packet_filter_configuration =
        filter.configuration;

    const auto decoded =
        encode_then_decode(action, 903U);
    REQUIRE(decoded);
    REQUIRE(decoded.message
        .has_congestion_extension);
    REQUIRE_EQ(decoded.message
            .congestion_controller,
        CongestionController::file);
    REQUIRE(decoded.message
        .has_packet_filter_extension);
    REQUIRE_EQ(decoded.message
            .packet_filter_configuration.view(),
        filter.configuration.view());
}

TEST(handshake_datagram_chains_stream_id_between_key_material_and_config)
{
    HandshakeAction action;
    action.kind = HandshakeActionKind::send;
    action.packet.version = handshake_version_5;
    action.packet.request = HandshakeRequest::conclusion;
    action.packet.extension_field = 5U;
    action.has_handshake_extension = true;
    action.extension_type =
        HandshakeExtensionType::handshake_request;
    action.has_stream_id_extension = true;
    REQUIRE(action.stream_id.assign(
        "#!::r=live/robotweax,m=request"));
    action.has_congestion_extension = true;
    action.congestion_controller = CongestionController::file;

    const auto decoded = encode_then_decode(action, 904U);
    REQUIRE(decoded);
    REQUIRE(decoded.message.has_stream_id_extension);
    REQUIRE_EQ(decoded.message.stream_id.view(),
        "#!::r=live/robotweax,m=request");
    REQUIRE(decoded.message.has_congestion_extension);
    REQUIRE_EQ(decoded.message.congestion_controller,
        CongestionController::file);
}

TEST(handshake_datagram_chains_group_membership_as_config_extension)
{
    HandshakeAction action;
    action.kind = HandshakeActionKind::send;
    action.packet.version = handshake_version_5;
    action.packet.request = HandshakeRequest::conclusion;
    action.packet.extension_field = 5U;
    action.has_handshake_extension = true;
    action.extension_type =
        HandshakeExtensionType::handshake_request;
    action.has_group_membership = true;
    action.group_membership = {
        .group_id = group_handle_mask | 17U,
        .type = GroupType::backup,
        .flags = 0U,
        .weight = 29U,
    };

    const auto decoded = encode_then_decode(action, 905U);
    REQUIRE(decoded);
    REQUIRE(decoded.message.has_group_membership);
    REQUIRE_EQ(decoded.message.group_membership.group_id,
        group_handle_mask | 17U);
    REQUIRE_EQ(decoded.message.group_membership.type,
        GroupType::backup);
    REQUIRE_EQ(decoded.message.group_membership.weight, 29U);
}

TEST(hsv5_group_membership_negotiates_distinct_mirror_identity)
{
    std::uint32_t cookie_secret = 0x31a2'51c3U;
    std::uint32_t mirror_base = 91U;
    const GroupMembership caller_membership{
        .group_id = group_handle_mask | 41U,
        .type = GroupType::broadcast,
        .flags = 0U,
        .weight = 7U,
    };
    HandshakeMachine caller{{
        .role = ConnectionRole::caller,
        .local_socket_id = 301U,
        .initial_sequence = SequenceNumber{8'000U},
        .has_group_membership = true,
        .group_membership = caller_membership,
    }};
    HandshakeMachine listener{{
        .role = ConnectionRole::listener,
        .local_socket_id = 302U,
        .initial_sequence = SequenceNumber{8'000U},
        .group_membership_negotiator = negotiate_test_group,
        .group_membership_context = &mirror_base,
        .cookie_generator = datagram_test_cookie,
        .cookie_context = &cookie_secret,
    }};

    const auto induction = encode_then_decode(
        caller.start().values[0], 0U);
    REQUIRE(induction);
    const auto induction_response = encode_then_decode(
        listener.receive(induction.message).values[0], 301U);
    REQUIRE(induction_response);
    const auto conclusion = caller.receive(
        induction_response.message);
    REQUIRE(conclusion.values[0].has_group_membership);
    REQUIRE_EQ(conclusion.values[0].group_membership.group_id,
        caller_membership.group_id);

    const auto request = encode_then_decode(
        conclusion.values[0], 302U);
    REQUIRE(request);
    const auto response = listener.receive(request.message);
    REQUIRE(response.values[0].has_group_membership);
    REQUIRE_EQ(response.values[0].group_membership.group_id,
        group_handle_mask | mirror_base);
    REQUIRE(listener.has_peer_group_membership());
    REQUIRE_EQ(listener.peer_group_membership().group_id,
        caller_membership.group_id);

    const auto response_message = encode_then_decode(
        response.values[0], 301U);
    REQUIRE(response_message);
    const auto connected = caller.receive(
        response_message.message);
    REQUIRE_EQ(connected.values[0].kind,
        HandshakeActionKind::connected);
    REQUIRE(caller.has_peer_group_membership());
    REQUIRE_EQ(caller.peer_group_membership().group_id,
        group_handle_mask | mirror_base);
}

namespace {

struct DispatchProviderContext {
    std::uint32_t mirror_base = 0;
    bool fail = false;
    bool wrong_id = false;
    int calls = 0;
    bool ipv6 = false;
};

bool provide_test_dispatch(MulticastDispatch& dispatch, void* context) noexcept
{
    auto& provider = *static_cast<DispatchProviderContext*>(context);
    ++provider.calls;
    if (provider.fail) {
        return false;
    }
    dispatch = MulticastDispatch {
        .group_id = group_handle_mask
            | (provider.wrong_id ? provider.mirror_base + 1U
                                 : provider.mirror_base),
        .multicast_group = {.address = {239, 255, 133, 10}, .port = 5'999},
        .has_interface_hint = false,
        .interface_hint = {},
        .initial_sequence = 8'000U,
        .timestamp_origin_delta_microseconds = -250,
        .time_to_live = 1,
    };
    if (provider.ipv6) {
        dispatch.multicast_group = {
            .address = {0xff, 0x15, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
            .port = 5999,
            .family = IpAddressFamily::ipv6};
        dispatch.interface_hint = IpEndpoint::ipv6_any();
    }
    return true;
}

[[nodiscard]] HandshakeMachine::Configuration dispatch_caller_configuration(
    std::uint32_t caller_group)
{
    return {
        .role = ConnectionRole::caller,
        .local_socket_id = 401U,
        .initial_sequence = SequenceNumber {8'000U},
        .has_group_membership = true,
        .group_membership =
            {
                .group_id = group_handle_mask | caller_group,
                .type = GroupType::multicast,
                .flags = 0U,
                .weight = 0U,
            },
        .group_membership_policy = {.admit_multicast_dispatch = true},
    };
}

} // namespace

TEST(hsv5_multicast_dispatch_negotiation_announces_the_group)
{
    std::uint32_t cookie_secret = 0x51a2'51c3U;
    std::uint32_t mirror_base = 93U;
    DispatchProviderContext provider {.mirror_base = mirror_base};
    HandshakeMachine caller {dispatch_caller_configuration(43U)};
    HandshakeMachine listener {{
        .role = ConnectionRole::listener,
        .local_socket_id = 402U,
        .initial_sequence = SequenceNumber {8'000U},
        .group_membership_negotiator = negotiate_test_group,
        .group_membership_context = &mirror_base,
        .group_membership_policy = {.admit_multicast_dispatch = true},
        .multicast_dispatch_provider = provide_test_dispatch,
        .multicast_dispatch_context = &provider,
        .cookie_generator = datagram_test_cookie,
        .cookie_context = &cookie_secret,
    }};

    const auto induction = encode_then_decode(caller.start().values[0], 0U);
    REQUIRE(induction);
    const auto induction_response =
        encode_then_decode(listener.receive(induction.message).values[0], 401U);
    REQUIRE(induction_response);
    const auto conclusion = caller.receive(induction_response.message);
    REQUIRE(conclusion.values[0].has_group_membership);
    REQUIRE_EQ(
        conclusion.values[0].group_membership.type, GroupType::multicast);
    // The Caller never announces a multicast group itself.
    REQUIRE(!conclusion.values[0].has_multicast_dispatch);

    const auto request = encode_then_decode(conclusion.values[0], 402U);
    REQUIRE(request);
    REQUIRE(!request.message.has_multicast_dispatch);
    const auto response = listener.receive(request.message);
    REQUIRE_EQ(provider.calls, 1);
    REQUIRE(response.values[0].has_group_membership);
    REQUIRE(response.values[0].has_multicast_dispatch);
    REQUIRE_EQ(response.values[0].multicast_dispatch.group_id,
        group_handle_mask | mirror_base);
    REQUIRE_EQ(
        response.values[0].multicast_dispatch.multicast_group.port, 5'999U);
    REQUIRE((response.values[0].packet.extension_field
                & handshake_extension_flag_config)
        != 0U);

    const auto response_message = encode_then_decode(response.values[0], 401U);
    REQUIRE(response_message);
    REQUIRE(response_message.message.has_multicast_dispatch);
    const auto connected = caller.receive(response_message.message);
    REQUIRE_EQ(connected.values[0].kind, HandshakeActionKind::connected);
    REQUIRE(caller.has_peer_group_membership());
    REQUIRE(caller.has_peer_multicast_dispatch());
    REQUIRE(caller.peer_multicast_dispatch()
        == response.values[0].multicast_dispatch);
    REQUIRE_EQ(caller.peer_multicast_dispatch().group_id,
        caller.peer_group_membership().group_id);
}

TEST(hsv5_multicast_dispatch_is_rejected_without_policy_or_announcement)
{
    // 1. A v1.5.7-profile Listener (default policy) rejects type 4 even
    //    with a group negotiator installed.
    {
        std::uint32_t cookie_secret = 0x61a2'51c3U;
        std::uint32_t mirror_base = 94U;
        HandshakeMachine caller {dispatch_caller_configuration(44U)};
        HandshakeMachine listener {{
            .role = ConnectionRole::listener,
            .local_socket_id = 412U,
            .initial_sequence = SequenceNumber {8'000U},
            .group_membership_negotiator = negotiate_test_group,
            .group_membership_context = &mirror_base,
            .cookie_generator = datagram_test_cookie,
            .cookie_context = &cookie_secret,
        }};
        const auto induction = encode_then_decode(caller.start().values[0], 0U);
        const auto induction_response = encode_then_decode(
            listener.receive(induction.message).values[0], 411U);
        const auto conclusion = caller.receive(induction_response.message);
        const auto request = encode_then_decode(conclusion.values[0], 412U);
        REQUIRE(request);
        const auto response = listener.receive(request.message);
        REQUIRE_EQ(response.size, 2U);
        REQUIRE_EQ(response.values[1].kind, HandshakeActionKind::rejected);
        REQUIRE_EQ(response.values[1].rejection_reason, 15);
        REQUIRE_EQ(listener.state(), HandshakeState::rejected);
    }
    // 2. A dispatch-admitting Listener without a provider, with a failing
    //    provider, or with an announcement naming another group rejects.
    for (const int variant : {0, 1, 2}) {
        std::uint32_t cookie_secret = 0x71a2'51c3U;
        std::uint32_t mirror_base = 95U;
        DispatchProviderContext provider {
            .mirror_base = mirror_base,
            .fail = variant == 1,
            .wrong_id = variant == 2,
        };
        HandshakeMachine caller {dispatch_caller_configuration(45U)};
        HandshakeMachine listener {{
            .role = ConnectionRole::listener,
            .local_socket_id = 422U,
            .initial_sequence = SequenceNumber {8'000U},
            .group_membership_negotiator = negotiate_test_group,
            .group_membership_context = &mirror_base,
            .group_membership_policy = {.admit_multicast_dispatch = true},
            .multicast_dispatch_provider =
                variant == 0 ? nullptr : provide_test_dispatch,
            .multicast_dispatch_context = &provider,
            .cookie_generator = datagram_test_cookie,
            .cookie_context = &cookie_secret,
        }};
        const auto induction = encode_then_decode(caller.start().values[0], 0U);
        const auto induction_response = encode_then_decode(
            listener.receive(induction.message).values[0], 421U);
        const auto conclusion = caller.receive(induction_response.message);
        const auto request = encode_then_decode(conclusion.values[0], 422U);
        REQUIRE(request);
        const auto response = listener.receive(request.message);
        REQUIRE_EQ(response.size, 2U);
        REQUIRE_EQ(response.values[1].kind, HandshakeActionKind::rejected);
        REQUIRE_EQ(response.values[1].rejection_reason, 15);
    }
    // 3. A dispatch Caller rejects a conclusion response that carries the
    //    membership but no announcement.
    {
        std::uint32_t cookie_secret = 0x81a2'51c3U;
        std::uint32_t mirror_base = 96U;
        DispatchProviderContext provider {.mirror_base = mirror_base};
        HandshakeMachine caller {dispatch_caller_configuration(46U)};
        HandshakeMachine listener {{
            .role = ConnectionRole::listener,
            .local_socket_id = 432U,
            .initial_sequence = SequenceNumber {8'000U},
            .group_membership_negotiator = negotiate_test_group,
            .group_membership_context = &mirror_base,
            .group_membership_policy = {.admit_multicast_dispatch = true},
            .multicast_dispatch_provider = provide_test_dispatch,
            .multicast_dispatch_context = &provider,
            .cookie_generator = datagram_test_cookie,
            .cookie_context = &cookie_secret,
        }};
        const auto induction = encode_then_decode(caller.start().values[0], 0U);
        const auto induction_response = encode_then_decode(
            listener.receive(induction.message).values[0], 431U);
        const auto conclusion = caller.receive(induction_response.message);
        const auto request = encode_then_decode(conclusion.values[0], 432U);
        auto response = listener.receive(request.message);
        REQUIRE(response.values[0].has_multicast_dispatch);
        response.values[0].has_multicast_dispatch = false;
        const auto stripped = encode_then_decode(response.values[0], 431U);
        REQUIRE(stripped);
        const auto result = caller.receive(stripped.message);
        REQUIRE_EQ(result.values[0].kind, HandshakeActionKind::rejected);
        REQUIRE(!caller.has_peer_multicast_dispatch());
    }
    // 4. A bonding Caller rejects an unexpected announcement.
    {
        std::uint32_t cookie_secret = 0x91a2'51c3U;
        std::uint32_t mirror_base = 97U;
        DispatchProviderContext provider {.mirror_base = mirror_base};
        HandshakeMachine caller {{
            .role = ConnectionRole::caller,
            .local_socket_id = 441U,
            .initial_sequence = SequenceNumber {8'000U},
            .has_group_membership = true,
            .group_membership =
                {
                    .group_id = group_handle_mask | 47U,
                    .type = GroupType::broadcast,
                    .flags = 0U,
                    .weight = 1U,
                },
        }};
        HandshakeMachine listener {{
            .role = ConnectionRole::listener,
            .local_socket_id = 442U,
            .initial_sequence = SequenceNumber {8'000U},
            .group_membership_negotiator = negotiate_test_group,
            .group_membership_context = &mirror_base,
            .cookie_generator = datagram_test_cookie,
            .cookie_context = &cookie_secret,
        }};
        const auto induction = encode_then_decode(caller.start().values[0], 0U);
        const auto induction_response = encode_then_decode(
            listener.receive(induction.message).values[0], 441U);
        const auto conclusion = caller.receive(induction_response.message);
        const auto request = encode_then_decode(conclusion.values[0], 442U);
        auto response = listener.receive(request.message);
        REQUIRE(response.values[0].has_group_membership);
        REQUIRE(!response.values[0].has_multicast_dispatch);
        response.values[0].has_multicast_dispatch = true;
        REQUIRE(provide_test_dispatch(
            response.values[0].multicast_dispatch, &provider));
        const auto injected = encode_then_decode(response.values[0], 441U);
        REQUIRE(injected);
        const auto result = caller.receive(injected.message);
        REQUIRE_EQ(result.values[0].kind, HandshakeActionKind::rejected);
    }
}

TEST(hsv5_listener_rejects_group_membership_without_admission_policy)
{
    std::uint32_t cookie_secret = 0x41a2'51c3U;
    HandshakeMachine caller{{
        .role = ConnectionRole::caller,
        .local_socket_id = 311U,
        .initial_sequence = SequenceNumber{9'000U},
        .has_group_membership = true,
        .group_membership = {
            .group_id = group_handle_mask | 51U,
            .type = GroupType::broadcast,
            .flags = 0U,
            .weight = 3U,
        },
    }};
    HandshakeMachine listener{{
        .role = ConnectionRole::listener,
        .local_socket_id = 312U,
        .initial_sequence = SequenceNumber{9'000U},
        .cookie_generator = datagram_test_cookie,
        .cookie_context = &cookie_secret,
    }};

    const auto induction = encode_then_decode(
        caller.start().values[0], 0U);
    REQUIRE(induction);
    const auto induction_response = encode_then_decode(
        listener.receive(induction.message).values[0], 311U);
    REQUIRE(induction_response);
    const auto conclusion = caller.receive(
        induction_response.message);
    const auto request = encode_then_decode(
        conclusion.values[0], 312U);
    REQUIRE(request);
    const auto rejection = listener.receive(request.message);
    REQUIRE_EQ(rejection.size, 2U);
    REQUIRE_EQ(rejection.values[0].packet.request,
        static_cast<HandshakeRequest>(1'015));
    REQUIRE_EQ(rejection.values[1].kind,
        HandshakeActionKind::rejected);
    REQUIRE_EQ(rejection.values[1].rejection_reason,
        15);
}

TEST(caller_listener_hsv5_state_machine_completes_over_real_udp)
{
    StreamId caller_stream_id;
    REQUIRE(caller_stream_id.assign(
        "#!::r=live/robotweax,m=request"));
    std::uint32_t cookie_secret = 0x51f2'a93cU;
    HandshakeMachine caller{{
        .role = ConnectionRole::caller,
        .local_socket_id = 100,
        .initial_sequence = SequenceNumber{1000},
        .stream_id = caller_stream_id,
    }};
    HandshakeMachine listener{{
        .role = ConnectionRole::listener,
        .local_socket_id = 200,
        .initial_sequence = SequenceNumber{2000},
        .cookie_generator = datagram_test_cookie,
        .cookie_context = &cookie_secret,
    }};
    UdpSocket caller_socket;
    UdpSocket listener_socket;
    REQUIRE(caller_socket.valid());
    REQUIRE(listener_socket.valid());
    REQUIRE_EQ(caller_socket.bind(Ipv4Endpoint::loopback()), Error::none);
    REQUIRE_EQ(listener_socket.bind(Ipv4Endpoint::loopback()), Error::none);
    const auto caller_address = caller_socket.local_endpoint();
    const auto listener_address = listener_socket.local_endpoint();
    REQUIRE(caller_address);
    REQUIRE(listener_address);

    const auto induction = caller.start();
    const auto at_listener = exchange(caller_socket, listener_socket,
        listener_address.endpoint, induction.values[0], 0);
    REQUIRE(at_listener);
    const auto induction_response = listener.receive(at_listener.message);

    const auto at_caller = exchange(listener_socket, caller_socket,
        caller_address.endpoint, induction_response.values[0], 100);
    REQUIRE(at_caller);
    const auto conclusion = caller.receive(at_caller.message);
    REQUIRE(conclusion.values[0].has_handshake_extension);
    REQUIRE_EQ(conclusion.values[0].extension_type,
        HandshakeExtensionType::handshake_request);
    REQUIRE(conclusion.values[0].has_stream_id_extension);

    const auto conclusion_at_listener = exchange(caller_socket, listener_socket,
        listener_address.endpoint, conclusion.values[0], 0);
    REQUIRE(conclusion_at_listener);
    REQUIRE_EQ(conclusion_at_listener.control.destination_socket_id, 0U);
    const auto conclusion_response = listener.receive(conclusion_at_listener.message);
    REQUIRE(listener.has_peer_stream_id());
    REQUIRE_EQ(listener.peer_stream_id().view(),
        caller_stream_id.view());
    REQUIRE(conclusion_response.values[0].has_handshake_extension);
    REQUIRE_EQ(conclusion_response.values[0].extension_type,
        HandshakeExtensionType::handshake_response);
    REQUIRE(!conclusion_response.values[0]
        .has_stream_id_extension);

    const auto response_at_caller = exchange(listener_socket, caller_socket,
        caller_address.endpoint, conclusion_response.values[0], 100);
    REQUIRE(response_at_caller);
    const auto connected = caller.receive(response_at_caller.message);
    REQUIRE_EQ(connected.values[0].kind, HandshakeActionKind::connected);
    REQUIRE_EQ(caller.state(), HandshakeState::connected);
    REQUIRE_EQ(listener.state(), HandshakeState::connected);
}

TEST(hsv5_handshake_chains_key_material_and_establishes_crypto)
{
    CryptoSession caller_crypto{{
        .passphrase = "correct horse battery",
        .key_length = 24,
    }};
    CryptoSession listener_crypto{{
        .passphrase = "correct horse battery",
        .key_length = 24,
    }};
    REQUIRE_EQ(caller_crypto.start_initiator(), Error::none);
    KeyMaterialBuffer request;
    const auto material = caller_crypto.pending_key_material();
    std::copy(material.begin(), material.end(),
        request.bytes.begin());
    request.size = material.size();

    std::uint32_t cookie_secret = 0x71a2'51c3U;
    HandshakeMachine caller{{
        .role = ConnectionRole::caller,
        .local_socket_id = 101,
        .initial_sequence = SequenceNumber{5'000},
        .encryption_field = 3,
        .key_material_request = request,
    }};
    HandshakeMachine listener{{
        .role = ConnectionRole::listener,
        .local_socket_id = 202,
        .initial_sequence = SequenceNumber{5'000},
        .cookie_generator = datagram_test_cookie,
        .cookie_context = &cookie_secret,
    }};

    const auto induction = caller.start();
    const auto listener_induction = encode_then_decode(
        induction.values[0], 0);
    REQUIRE(listener_induction);
    const auto induction_response =
        listener.receive(listener_induction.message);
    const auto caller_induction = encode_then_decode(
        induction_response.values[0], 101);
    REQUIRE(caller_induction);
    const auto conclusion =
        caller.receive(caller_induction.message);
    REQUIRE(conclusion.values[0].has_handshake_extension);
    REQUIRE(conclusion.values[0].has_key_material_extension);
    REQUIRE_EQ(conclusion.values[0].packet.extension_field, 3U);
    REQUIRE_EQ(conclusion.values[0].packet.encryption_field, 3U);

    const auto conclusion_retry = caller.timeout();
    REQUIRE_EQ(conclusion_retry.values[0].kind,
        HandshakeActionKind::send);
    REQUIRE(conclusion_retry.values[0].has_key_material_extension);
    REQUIRE(std::equal(
        conclusion_retry.values[0].key_material.view().begin(),
        conclusion_retry.values[0].key_material.view().end(),
        conclusion.values[0].key_material.view().begin(),
        conclusion.values[0].key_material.view().end()));
    const auto listener_conclusion = encode_then_decode(
        conclusion_retry.values[0], 202);
    REQUIRE(listener_conclusion);
    REQUIRE(listener_conclusion.message.has_key_material_extension);
    REQUIRE_EQ(listener_conclusion.message.key_material_extension_type,
        HandshakeExtensionType::key_material_request);
    REQUIRE_EQ(listener_crypto.accept_key_material(
                   listener_conclusion.message.key_material.view(), true),
        Error::none);

    const auto response =
        listener.receive(listener_conclusion.message);
    REQUIRE(response.values[0].has_key_material_extension);
    REQUIRE_EQ(response.values[0].key_material_extension_type,
        HandshakeExtensionType::key_material_response);

    const auto response_retry_request = caller.timeout();
    REQUIRE_EQ(response_retry_request.values[0].kind,
        HandshakeActionKind::send);
    REQUIRE(response_retry_request.values[0]
        .has_key_material_extension);
    REQUIRE(std::equal(
        response_retry_request.values[0]
            .key_material.view().begin(),
        response_retry_request.values[0]
            .key_material.view().end(),
        conclusion.values[0].key_material.view().begin(),
        conclusion.values[0].key_material.view().end()));
    const auto repeated_listener_conclusion = encode_then_decode(
        response_retry_request.values[0], 202);
    REQUIRE(repeated_listener_conclusion);
    REQUIRE_EQ(listener_crypto.accept_key_material(
                   repeated_listener_conclusion.message
                       .key_material.view(),
                   true),
        Error::none);
    const auto repeated_response =
        listener.receive(repeated_listener_conclusion.message);
    REQUIRE_EQ(repeated_response.values[0].kind,
        HandshakeActionKind::send);
    REQUIRE(repeated_response.values[0].has_key_material_extension);
    REQUIRE(std::equal(
        repeated_response.values[0].key_material.view().begin(),
        repeated_response.values[0].key_material.view().end(),
        response.values[0].key_material.view().begin(),
        response.values[0].key_material.view().end()));
    const auto caller_response = encode_then_decode(
        repeated_response.values[0], 101);
    REQUIRE(caller_response);
    REQUIRE_EQ(caller_crypto.acknowledge_key_material(
                   caller_response.message.key_material.view(), true),
        Error::none);
    confirm_directional_test_keys(caller_crypto, listener_crypto);
    const auto connected = caller.receive(caller_response.message);
    REQUIRE_EQ(connected.values[0].kind,
        HandshakeActionKind::connected);
    REQUIRE_EQ(caller_crypto.sender_state(), CryptoState::secured);
    REQUIRE_EQ(caller_crypto.receiver_state(), CryptoState::secured);
    REQUIRE_EQ(listener_crypto.sender_state(), CryptoState::secured);
    REQUIRE_EQ(listener_crypto.receiver_state(), CryptoState::secured);
}

TEST(
    session_authentication_datagram_rejects_unknown_version_duplicate_and_truncation)
{
    HandshakeAction action;
    action.kind = HandshakeActionKind::send;
    action.packet.version = handshake_version_5;
    action.packet.request = HandshakeRequest::agreement;
    action.packet.extension_field = handshake_extension_flag_config;
    action.has_session_authentication = true;
    action.session_authentication.caller_nonce[0] = std::byte {1};
    std::array<std::byte, 1500> bytes {};
    const auto encoded =
        encode_handshake_datagram(action, PacketTimestamp {1}, 42, bytes);
    REQUIRE(encoded);
    auto wire = std::span {bytes}.first(encoded.bytes_written);
    const auto decoded = decode_handshake_datagram(wire);
    REQUIRE(decoded);
    REQUIRE(decoded.message.has_session_authentication);
    REQUIRE(decoded.message.session_authentication.matches(
        action.session_authentication));
    constexpr auto offset = packet_header_size + handshake_size;
    bytes[offset + extension_header_size + 3] = std::byte {1};
    REQUIRE(!decode_handshake_datagram(wire));
    bytes[offset + extension_header_size + 3] =
        std::byte {session_authentication_version};
    REQUIRE(!decode_handshake_datagram(wire.first(wire.size() - 4)));
    std::copy(wire.begin() + offset, wire.end(),
        bytes.begin() + encoded.bytes_written);
    REQUIRE(
        !decode_handshake_datagram(std::span {bytes}.first(encoded.bytes_written
            + extension_header_size + session_authentication_wire_size)));
}

namespace {

SessionAuthenticationParameters source_test_auth(unsigned phase)
{
    SessionAuthenticationParameters parameters;
    parameters.caller_nonce[0] = std::byte {1};
    if (phase != 0) {
        parameters.listener_nonce[0] = std::byte {2};
        parameters.proof[0] = static_cast<std::byte>(phase + 2);
    }
    return parameters;
}

struct SourceNegotiation {
    std::uint32_t cookie = 0x51a251c3U;
    std::uint32_t mirror = 93U;
    DispatchProviderContext provider {.mirror_base = mirror};
    HandshakeMachine caller;
    HandshakeMachine listener;

    SourceNegotiation(bool caller_requires = true,
        bool listener_requires = true,
        IpEndpoint source = IpEndpoint::loopback(6000),
        bool authenticate = false)
        : provider {.mirror_base = mirror, .ipv6 = source.is_ipv6()}
        , caller([&] {
            auto config = dispatch_caller_configuration(43U);
            config.require_multicast_source = caller_requires;
            config.require_session_authentication = authenticate;
            config.session_authentication = source_test_auth(0);
            return config;
        }())
        , listener(HandshakeMachine::Configuration {
              .role = ConnectionRole::listener,
              .local_socket_id = 402,
              .group_membership_negotiator = negotiate_test_group,
              .group_membership_context = &mirror,
              .group_membership_policy = {.admit_multicast_dispatch = true},
              .multicast_dispatch_provider = provide_test_dispatch,
              .multicast_dispatch_context = &provider,
              .cookie_generator = datagram_test_cookie,
              .cookie_context = &cookie,
              .require_session_authentication = authenticate,
              .session_authentication = source_test_auth(1),
              .session_authentication_confirmation = source_test_auth(2),
              .require_multicast_source = listener_requires,
              .multicast_source = listener_requires ? source : IpEndpoint {},
          })
    {
    }

    HandshakeAction offer()
    {
        const auto induction = encode_then_decode(caller.start().values[0], 0);
        REQUIRE(induction);
        REQUIRE(!induction.message.has_multicast_source);
        const auto response = encode_then_decode(
            listener.receive(induction.message).values[0], 401);
        REQUIRE(response);
        REQUIRE(!response.message.has_multicast_source);
        return caller.receive(response.message).values[0];
    }

    HandshakeAction reply(const HandshakeAction& offer)
    {
        const auto request = encode_then_decode(offer, 402);
        REQUIRE(request);
        return listener.receive(request.message).values[0];
    }
};

} // namespace

TEST(multicast_source_handshake_requires_bilateral_opt_in)
{
    for (const bool caller_requires : {false, true}) {
        for (const bool listener_requires : {false, true}) {
            SourceNegotiation fixture {caller_requires, listener_requires};
            const auto offer = fixture.offer();
            REQUIRE_EQ(offer.has_multicast_source, caller_requires);
            REQUIRE(offer.multicast_source.kind == MulticastSourceKind::offer);
            const auto reply = fixture.reply(offer);
            const auto decoded = encode_then_decode(reply, 401);
            REQUIRE(decoded);
            const auto result = fixture.caller.receive(decoded.message);
            if (caller_requires != listener_requires) {
                REQUIRE_EQ(
                    result.values[0].kind, HandshakeActionKind::rejected);
                REQUIRE_EQ(fixture.caller.rejection_reason(), 15);
                REQUIRE(!fixture.caller.has_peer_multicast_source());
                REQUIRE_EQ(fixture.provider.calls, 0);
            } else {
                REQUIRE_EQ(
                    result.values[0].kind, HandshakeActionKind::connected);
                REQUIRE_EQ(fixture.caller.has_peer_multicast_source(),
                    caller_requires);
                REQUIRE_EQ(reply.has_multicast_source, caller_requires);
                if (caller_requires) {
                    REQUIRE_EQ(fixture.caller.peer_multicast_source().endpoint,
                        IpEndpoint::loopback(6000));
                    REQUIRE_EQ(fixture.caller.peer_multicast_source().group_id,
                        group_handle_mask | fixture.mirror);
                }
            }
        }
    }
}

TEST(multicast_source_handshake_retries_preserve_the_selected_source)
{
    SourceNegotiation fixture;
    const auto offer = fixture.offer();
    const auto reply = fixture.reply(offer);
    const auto retry = fixture.caller.timeout();
    REQUIRE(retry.size != 0);
    REQUIRE(retry.values[0].has_multicast_source);
    const auto replay = fixture.reply(retry.values[0]);
    REQUIRE(replay.multicast_source == reply.multicast_source);
    REQUIRE(replay.multicast_dispatch == reply.multicast_dispatch);
    REQUIRE_EQ(fixture.provider.calls, 1);
    auto changed = encode_then_decode(offer, 402).message;
    changed.has_multicast_source = false;
    REQUIRE_EQ(fixture.listener.receive(changed).size, 0U);
    changed.has_multicast_source = true;
    changed.multicast_source.endpoint.port = 6001;
    REQUIRE_EQ(fixture.listener.receive(changed).size, 0U);
    changed = encode_then_decode(offer, 402).message;
    changed.group_membership.group_id += 1;
    REQUIRE_EQ(fixture.listener.receive(changed).size, 0U);
    REQUIRE_EQ(fixture.listener.state(), HandshakeState::connected);
}

TEST(multicast_source_handshake_rejects_stripped_or_mismatched_announcements)
{
    for (unsigned mutation = 0; mutation < 7; ++mutation) {
        SourceNegotiation fixture;
        const auto reply = fixture.reply(fixture.offer());
        auto message = encode_then_decode(reply, 401).message;
        switch (mutation) {
        case 0:
            message.has_multicast_source = false;
            break;
        case 1:
            message.multicast_source.kind = MulticastSourceKind::offer;
            break;
        case 2:
            message.multicast_source.group_id += 1;
            break;
        case 3:
            message.multicast_source.endpoint = IpEndpoint::ipv6_loopback(6000);
            break;
        case 4:
            message.multicast_source.endpoint = IpEndpoint::any(6000);
            break;
        case 5:
            message.has_multicast_dispatch = false;
            break;
        case 6:
            message.has_group_membership = false;
            break;
        }
        const auto result = fixture.caller.receive(message);
        REQUIRE_EQ(result.values[0].kind, HandshakeActionKind::rejected);
        REQUIRE(!fixture.caller.has_peer_multicast_source());
        REQUIRE(!fixture.caller.has_peer_multicast_dispatch());
    }
}

TEST(
    multicast_source_datagram_rejects_duplicates_wrong_context_and_oversized_records)
{
    SourceNegotiation fixture;
    const auto offer = fixture.offer();
    const auto reply = fixture.reply(offer);
    for (unsigned mutation = 0; mutation < 7; ++mutation) {
        auto invalid = reply;
        switch (mutation) {
        case 0:
            invalid.packet.extension_field &= ~handshake_extension_flag_config;
            break;
        case 1:
            invalid.packet.request = HandshakeRequest::induction;
            break;
        case 2:
            invalid.has_group_membership = false;
            break;
        case 3:
            invalid.multicast_source.group_id += 1;
            break;
        case 4:
            invalid.extension_type = HandshakeExtensionType::handshake_request;
            break;
        case 5:
            invalid.has_multicast_dispatch = false;
            break;
        case 6:
            invalid.packet.maximum_transmission_unit = 100;
            break;
        }
        std::array<std::byte, 1500> bytes {};
        REQUIRE(!encode_handshake_datagram(invalid, {}, 401, bytes));
    }
    std::array<std::byte, 1500> bytes {};
    const auto encoded = encode_handshake_datagram(reply, {}, 401, bytes);
    REQUIRE(encoded);
    // A duplicate valid source record is still ambiguous and must be rejected.
    const auto appended = encode_multicast_source(reply.multicast_source,
        std::span {bytes}.subspan(encoded.bytes_written));
    REQUIRE(appended);
    REQUIRE(!decode_handshake_datagram(std::span {bytes}.first(
        encoded.bytes_written + appended.bytes_written)));
    // Mutate the source record's group independently of the legacy announcement.
    REQUIRE(encode_handshake_datagram(reply, {}, 401, bytes));
    bytes[encoded.bytes_written - multicast_source_content_size + 11] ^=
        std::byte {1};
    REQUIRE(!decode_handshake_datagram(
        std::span {bytes}.first(encoded.bytes_written)));
}

TEST(multicast_source_handshake_invalid_configuration_fails_before_sending)
{
    for (unsigned mutation = 0; mutation < 5; ++mutation) {
        auto config = dispatch_caller_configuration(43);
        config.require_multicast_source = true;
        switch (mutation) {
        case 0:
            config.has_group_membership = false;
            break;
        case 1:
            config.group_membership.type = GroupType::backup;
            break;
        case 2:
            config.multicast_source = IpEndpoint::loopback(6000);
            break;
        case 3:
            config.group_membership_policy.admit_multicast_dispatch = false;
            break;
        case 4:
            config.congestion_controller = CongestionController::file;
            break;
        }
        HandshakeMachine caller {config};
        const auto result = caller.start();
        REQUIRE_EQ(result.values[0].kind, HandshakeActionKind::failed);
    }
}

TEST(multicast_source_handshake_ipv6_and_source_family_mismatch)
{
    SourceNegotiation fixture {true, true, IpEndpoint::ipv6_loopback(6000)};
    const auto reply = fixture.reply(fixture.offer());
    const auto decoded = encode_then_decode(reply, 401);
    REQUIRE(decoded);
    REQUIRE_EQ(fixture.caller.receive(decoded.message).values[0].kind,
        HandshakeActionKind::connected);
    REQUIRE_EQ(fixture.caller.peer_multicast_source().endpoint,
        IpEndpoint::ipv6_loopback(6000));
    SourceNegotiation mismatched {true, true, IpEndpoint::ipv6_loopback(6000)};
    mismatched.provider.ipv6 = false;
    const auto rejected =
        encode_then_decode(mismatched.reply(mismatched.offer()), 401);
    REQUIRE(rejected);
    REQUIRE_EQ(mismatched.caller.receive(rejected.message).values[0].kind,
        HandshakeActionKind::rejected);
    REQUIRE(!mismatched.caller.has_peer_multicast_source());
}

TEST(multicast_source_handshake_authentication_confirmation_and_retry)
{
    SourceNegotiation fixture {true, true, IpEndpoint::loopback(6000), true};
    const auto offer = fixture.offer();
    const auto reply = fixture.reply(offer);
    REQUIRE_EQ(fixture.listener.state(),
        HandshakeState::awaiting_authentication_confirmation);
    const auto duplicate = fixture.reply(offer);
    REQUIRE_EQ(duplicate.multicast_source, reply.multicast_source);
    const auto timer = fixture.listener.timeout();
    REQUIRE_EQ(timer.values[0].multicast_source, reply.multicast_source);
    auto changed = encode_then_decode(offer, 402).message;
    changed.has_multicast_source = false;
    REQUIRE_EQ(fixture.listener.receive(changed).size, 0U);
    REQUIRE_EQ(fixture.caller.set_session_authentication(source_test_auth(2)),
        Error::none);
    const auto decoded = encode_then_decode(reply, 401);
    REQUIRE(decoded);
    const auto done = fixture.caller.receive(decoded.message);
    REQUIRE_EQ(done.values[0].packet.request, HandshakeRequest::agreement);
    REQUIRE(fixture.caller.has_peer_multicast_source());
    // Source metadata belongs to CONCLUSION, not the authentication finish.
    REQUIRE(!done.values[0].has_multicast_source);
    const auto finish = encode_then_decode(done.values[0], 402);
    REQUIRE(finish);
    (void)fixture.listener.receive(finish.message);
    REQUIRE_EQ(fixture.listener.state(), HandshakeState::connected);
}

TEST(
    multicast_source_handshake_does_not_publish_source_on_authentication_rejection)
{
    SourceNegotiation fixture {true, true, IpEndpoint::loopback(6000), true};
    const auto reply = fixture.reply(fixture.offer());
    auto message = encode_then_decode(reply, 401).message;
    message.has_session_authentication = false;
    REQUIRE_EQ(fixture.caller.receive(message).values[0].kind,
        HandshakeActionKind::rejected);
    REQUIRE(!fixture.caller.has_peer_multicast_source());
}

TEST(multicast_source_datagram_preserves_legacy_bytes_and_exact_mtu_budget)
{
    SourceNegotiation legacy {false, false};
    const auto legacy_reply = legacy.reply(legacy.offer());
    SourceNegotiation selected;
    const auto selected_reply = selected.reply(selected.offer());
    std::array<std::byte, 1500> old_bytes {}, new_bytes {};
    const auto old_encoded =
        encode_handshake_datagram(legacy_reply, {}, 401, old_bytes);
    const auto new_encoded =
        encode_handshake_datagram(selected_reply, {}, 401, new_bytes);
    REQUIRE(old_encoded);
    REQUIRE(new_encoded);
    REQUIRE_EQ(new_encoded.bytes_written - old_encoded.bytes_written,
        extension_header_size + multicast_source_content_size);
    REQUIRE(std::equal(old_bytes.begin(),
        old_bytes.begin() + old_encoded.bytes_written, new_bytes.begin()));
    auto exact = selected_reply;
    exact.packet.maximum_transmission_unit =
        static_cast<std::uint32_t>(new_encoded.bytes_written + 48);
    REQUIRE(encode_handshake_datagram(exact, {}, 401,
        std::span {new_bytes}.first(new_encoded.bytes_written)));
    REQUIRE(!encode_handshake_datagram(exact, {}, 401,
        std::span {new_bytes}.first(new_encoded.bytes_written - 1)));
    REQUIRE(decode_handshake_datagram(
        std::span {new_bytes}.first(new_encoded.bytes_written)));
    // Mutate only the peer's advertised MTU in otherwise valid wire input.
    std::fill_n(new_bytes.begin() + packet_header_size + 12, 4, std::byte {});
    new_bytes[packet_header_size + 15] = std::byte {100};
    REQUIRE(!decode_handshake_datagram(
        std::span {new_bytes}.first(new_encoded.bytes_written)));
    --exact.packet.maximum_transmission_unit;
    REQUIRE(!encode_handshake_datagram(exact, {}, 401, new_bytes));
}

TEST(multicast_source_listener_rejects_missing_or_invalid_configured_source)
{
    for (const auto source : {IpEndpoint {}, IpEndpoint::any(6000),
             IpEndpoint::loopback(), IpEndpoint::ipv6_loopback(6000, 5)}) {
        SourceNegotiation fixture {true, true, source};
        const auto start =
            encode_then_decode(fixture.caller.start().values[0], 0);
        REQUIRE(start);
        const auto result = fixture.listener.receive(start.message);
        REQUIRE_EQ(result.values[0].kind, HandshakeActionKind::failed);
        REQUIRE_EQ(fixture.provider.calls, 0);
    }
}
