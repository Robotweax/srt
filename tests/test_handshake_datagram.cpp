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
