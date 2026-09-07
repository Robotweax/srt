#include "test.hpp"

#include "robotweax/srt/handshake.hpp"

#include <array>
#include <cstddef>
#include <limits>

using namespace robotweax::srt;

namespace {

std::uint32_t test_cookie(const Handshake& packet, void* context) noexcept
{
    const auto salt = *static_cast<const std::uint32_t*>(context);
    return packet.socket_id ^ packet.initial_sequence.value() ^ salt;
}

[[nodiscard]] HandshakeMessage message_from(
    const HandshakeAction& action)
{
    return {
        .packet = action.packet,
        .has_handshake_extension =
            action.has_handshake_extension,
        .extension_type = action.extension_type,
        .extension_parameters =
            action.extension_parameters,
        .has_key_material_extension =
            action.has_key_material_extension,
        .key_material_extension_type =
            action.key_material_extension_type,
        .key_material = action.key_material,
        .has_congestion_extension =
            action.has_congestion_extension,
        .congestion_controller =
            action.congestion_controller,
        .has_packet_filter_extension =
            action.has_packet_filter_extension,
        .packet_filter_configuration =
            action.packet_filter_configuration,
    };
}

} // namespace

TEST(handshake_payload_round_trip)
{
    Handshake source;
    source.version = handshake_version_5;
    source.encryption_field = 2;
    source.extension_field = 3;
    source.initial_sequence = SequenceNumber{12345};
    source.maximum_transmission_unit = 1'500;
    source.flow_window = 25'600;
    source.request = HandshakeRequest::conclusion;
    source.socket_id = 0x1020'3040U;
    source.syn_cookie = 0xaabb'ccddU;
    source.peer_address = {1, 2, 3, 4};

    std::array<std::byte, handshake_size> bytes{};
    REQUIRE_EQ(encode_handshake(source, bytes), Error::none);
    const auto decoded = decode_handshake(bytes);
    REQUIRE(decoded);
    REQUIRE_EQ(decoded.handshake.version, source.version);
    REQUIRE_EQ(decoded.handshake.initial_sequence, source.initial_sequence);
    REQUIRE_EQ(decoded.handshake.request, source.request);
    REQUIRE_EQ(decoded.handshake.syn_cookie, source.syn_cookie);
    REQUIRE_EQ(decoded.handshake.peer_address, source.peer_address);
}

TEST(caller_and_listener_complete_foundation_handshake)
{
    std::uint32_t cookie_salt = 0x9e37'79b9U;
    HandshakeMachine caller {{
        .role = ConnectionRole::caller,
        .local_socket_id = 100,
        .initial_sequence = SequenceNumber {10},
        .flow_window = 111,
    }};
    HandshakeMachine listener {{
        .role = ConnectionRole::listener,
        .local_socket_id = 200,
        .initial_sequence = SequenceNumber {20},
        .flow_window = 222,
        .cookie_generator = test_cookie,
        .cookie_context = &cookie_salt,
    }};

    const auto start = caller.start();
    REQUIRE_EQ(start.values[0].kind, HandshakeActionKind::send);
    const auto induction_response = listener.receive(start.values[0].packet);
    REQUIRE_EQ(induction_response.values[0].packet.request, HandshakeRequest::induction);
    REQUIRE_EQ(induction_response.values[0].packet.initial_sequence, SequenceNumber{10});
    REQUIRE(induction_response.values[0].packet.syn_cookie != 0U);

    const auto conclusion = caller.receive(induction_response.values[0].packet);
    REQUIRE_EQ(conclusion.values[0].packet.request, HandshakeRequest::conclusion);
    const auto listener_done = listener.receive(conclusion.values[0].packet);
    REQUIRE_EQ(listener_done.values[1].kind, HandshakeActionKind::connected);
    const auto caller_done = caller.receive(listener_done.values[0].packet);
    REQUIRE_EQ(caller_done.values[0].kind, HandshakeActionKind::connected);
    REQUIRE_EQ(caller.state(), HandshakeState::connected);
    REQUIRE_EQ(listener.state(), HandshakeState::connected);
    REQUIRE_EQ(caller.peer_flow_window(), 222U);
    REQUIRE_EQ(listener.peer_flow_window(), 111U);
}

TEST(caller_ignores_stale_induction_without_extending_deadline)
{
    HandshakeMachine caller {{
        .role = ConnectionRole::caller,
        .local_socket_id = 100U,
        .maximum_retries = 1U,
    }};
    std::uint32_t salt = 42U;
    HandshakeMachine listener {{
        .role = ConnectionRole::listener,
        .local_socket_id = 200U,
        .cookie_generator = test_cookie,
        .cookie_context = &salt,
    }};
    const auto start = caller.start();
    const auto reply = listener.receive(message_from(start.values[0]));
    const auto message = message_from(reply.values[0]);
    REQUIRE(!caller.is_stale_induction(message));
    static_cast<void>(caller.receive(message));
    REQUIRE(caller.is_stale_induction(message));
    static_cast<void>(caller.timeout());
    for (unsigned i = 0; i < 20; ++i) {
        REQUIRE_EQ(caller.receive(message).size, 0U);
        REQUIRE_EQ(
            caller.state(), HandshakeState::awaiting_conclusion_response);
    }
    const auto exhausted = caller.timeout();
    REQUIRE_EQ(exhausted.values[0].kind, HandshakeActionKind::failed);
}

TEST(listener_requires_explicit_cookie_policy)
{
    HandshakeMachine listener{{
        .role = ConnectionRole::listener,
        .local_socket_id = 200,
    }};
    Handshake induction;
    induction.version = handshake_version_4;
    induction.socket_id = 100;

    const auto actions = listener.receive(induction);
    REQUIRE_EQ(actions.values[0].kind, HandshakeActionKind::failed);
    REQUIRE_EQ(listener.state(), HandshakeState::failed);
}

TEST(handshake_rejects_invalid_size_and_mtu)
{
    std::array<std::byte, handshake_size - 1> short_payload{};
    REQUIRE_EQ(decode_handshake(short_payload).error, Error::invalid_handshake_size);

    Handshake invalid;
    invalid.maximum_transmission_unit = 75;
    std::array<std::byte, handshake_size> bytes{};
    REQUIRE_EQ(encode_handshake(invalid, bytes), Error::invalid_handshake_value);
}

TEST(handshake_rejects_out_of_range_initial_sequence_and_flow_window)
{
    Handshake handshake;
    std::array<std::byte, handshake_size> bytes {};

    REQUIRE_EQ(encode_handshake(handshake, bytes), Error::none);
    bytes[8] = std::byte {0x80};
    REQUIRE_EQ(decode_handshake(bytes).error, Error::invalid_handshake_value);

    REQUIRE_EQ(encode_handshake(handshake, bytes), Error::none);
    bytes[8] = std::byte {0x7f};
    bytes[9] = std::byte {0xff};
    bytes[10] = std::byte {0xff};
    bytes[11] = std::byte {0xff};
    REQUIRE_EQ(decode_handshake(bytes).error, Error::none);

    REQUIRE_EQ(encode_handshake(handshake, bytes), Error::none);
    bytes[16] = std::byte {0};
    bytes[17] = std::byte {0};
    bytes[18] = std::byte {0};
    bytes[19] = std::byte {1};
    REQUIRE_EQ(decode_handshake(bytes).error, Error::invalid_handshake_value);

    REQUIRE_EQ(encode_handshake(handshake, bytes), Error::none);
    bytes[16] = std::byte {0x80};
    REQUIRE_EQ(decode_handshake(bytes).error, Error::invalid_handshake_value);

    handshake.initial_sequence = SequenceNumber {SequenceNumber::mask};
    REQUIRE_EQ(encode_handshake(handshake, bytes), Error::none);
    handshake.initial_sequence = SequenceNumber {SequenceNumber::mask - 1U};
    handshake.flow_window = 1U;
    REQUIRE_EQ(
        encode_handshake(handshake, bytes), Error::invalid_handshake_value);
    handshake.flow_window =
        static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max());
    REQUIRE_EQ(encode_handshake(handshake, bytes), Error::none);
    handshake.flow_window = 0x8000'0000U;
    REQUIRE_EQ(
        encode_handshake(handshake, bytes), Error::invalid_handshake_value);
}

TEST(handshake_payload_preserves_variable_length_extensions)
{
    Handshake handshake;
    handshake.request = HandshakeRequest::conclusion;
    std::array<std::byte, 4> extension{
        std::byte{0}, std::byte{1}, std::byte{0}, std::byte{0},
    };
    std::array<std::byte, handshake_size + extension.size()> bytes{};
    const auto encoded = encode_handshake_payload(handshake, extension, bytes);
    REQUIRE(encoded);
    const auto decoded = decode_handshake_payload(bytes);
    REQUIRE(decoded);
    REQUIRE_EQ(decoded.extensions.size(), extension.size());
    REQUIRE_EQ(decoded.extensions[1], extension[1]);
}

TEST(caller_retransmits_induction_until_retry_budget_is_exhausted)
{
    HandshakeMachine caller{{
        .role = ConnectionRole::caller,
        .local_socket_id = 100,
        .maximum_retries = 2,
    }};
    (void)caller.start();

    const auto first_retry = caller.timeout();
    REQUIRE_EQ(first_retry.values[0].kind, HandshakeActionKind::send);
    REQUIRE_EQ(first_retry.values[0].packet.request, HandshakeRequest::induction);
    const auto second_retry = caller.timeout();
    REQUIRE_EQ(second_retry.values[0].kind, HandshakeActionKind::send);
    const auto exhausted = caller.timeout();
    REQUIRE_EQ(exhausted.values[0].kind, HandshakeActionKind::failed);
    REQUIRE_EQ(caller.state(), HandshakeState::failed);
}

TEST(connected_listener_replies_to_repeated_conclusion)
{
    std::uint32_t cookie_salt = 0x9e37'79b9U;
    HandshakeMachine listener{{
        .role = ConnectionRole::listener,
        .local_socket_id = 200,
        .cookie_generator = test_cookie,
        .cookie_context = &cookie_salt,
    }};
    Handshake induction;
    induction.version = handshake_version_4;
    induction.socket_id = 100;
    const auto induction_response = listener.receive(induction);

    HandshakeMessage conclusion;
    conclusion.packet.version = handshake_version_5;
    conclusion.packet.request = HandshakeRequest::conclusion;
    conclusion.packet.socket_id = 100;
    conclusion.packet.syn_cookie = induction_response.values[0].packet.syn_cookie;
    conclusion.has_handshake_extension = true;
    conclusion.extension_type = HandshakeExtensionType::handshake_request;
    conclusion.extension_parameters.receiver_tsbpd_delay_milliseconds = 350;
    conclusion.extension_parameters.sender_tsbpd_delay_milliseconds = 275;
    const auto first_response = listener.receive(conclusion);
    REQUIRE_EQ(first_response.values[1].kind, HandshakeActionKind::connected);
    REQUIRE_EQ(first_response.values[0].extension_parameters
            .receiver_tsbpd_delay_milliseconds,
        275U);
    REQUIRE_EQ(first_response.values[0].extension_parameters
            .sender_tsbpd_delay_milliseconds,
        350U);

    const auto repeated_response = listener.receive(conclusion);
    REQUIRE_EQ(repeated_response.values[0].kind, HandshakeActionKind::send);
    REQUIRE_EQ(repeated_response.values[0].extension_type,
        HandshakeExtensionType::handshake_response);
    REQUIRE_EQ(listener.state(), HandshakeState::connected);
}

TEST(caller_preserves_the_wire_rejection_reason)
{
    HandshakeMachine caller{{
        .role = ConnectionRole::caller,
        .local_socket_id = 100,
    }};
    (void)caller.start();

    Handshake rejection;
    rejection.version = handshake_version_5;
    rejection.request =
        static_cast<HandshakeRequest>(1'000 + 404);
    const auto actions = caller.receive(rejection);
    REQUIRE_EQ(actions.size, 1U);
    REQUIRE_EQ(actions.values[0].kind,
        HandshakeActionKind::rejected);
    REQUIRE_EQ(actions.values[0].rejection_reason, 404);
    REQUIRE_EQ(caller.rejection_reason(), 404);
    REQUIRE_EQ(caller.state(), HandshakeState::rejected);
}

TEST(listener_transmits_application_defined_rejection_reasons)
{
    constexpr int application_reason = 1'404;
    std::uint32_t cookie_salt = 0x9e37'79b9U;
    HandshakeMachine caller{{
        .role = ConnectionRole::caller,
        .local_socket_id = 100,
    }};
    HandshakeMachine listener{{
        .role = ConnectionRole::listener,
        .local_socket_id = 200,
        .cookie_generator = test_cookie,
        .cookie_context = &cookie_salt,
    }};

    const auto induction = caller.start();
    const auto induction_response =
        listener.receive(induction.values[0].packet);
    const auto conclusion =
        caller.receive(induction_response.values[0].packet);
    REQUIRE_EQ(conclusion.values[0].packet.request,
        HandshakeRequest::conclusion);
    REQUIRE_EQ(listener.state(),
        HandshakeState::awaiting_conclusion_response);

    const auto rejected = listener.reject(application_reason);
    REQUIRE_EQ(rejected.size, 2U);
    REQUIRE_EQ(rejected.values[0].kind, HandshakeActionKind::send);
    REQUIRE_EQ(static_cast<std::int32_t>(
                   rejected.values[0].packet.request),
        1'000 + application_reason);
    REQUIRE_EQ(rejected.values[1].kind,
        HandshakeActionKind::rejected);
    REQUIRE_EQ(rejected.values[1].rejection_reason,
        application_reason);

    const auto caller_rejected =
        caller.receive(rejected.values[0].packet);
    REQUIRE_EQ(caller_rejected.values[0].kind,
        HandshakeActionKind::rejected);
    REQUIRE_EQ(caller_rejected.values[0].rejection_reason,
        application_reason);
    REQUIRE_EQ(caller.rejection_reason(), application_reason);
    REQUIRE_EQ(caller.state(), HandshakeState::rejected);

}

TEST(listener_rejects_message_and_buffer_mode_mismatch)
{
    std::uint32_t cookie_salt = 0x9e37'79b9U;
    HandshakeExtensionParameters stream_parameters;
    stream_parameters.flags |= static_cast<std::uint32_t>(
        HandshakeExtensionFlag::stream);
    HandshakeMachine caller{{
        .role = ConnectionRole::caller,
        .local_socket_id = 100,
        .extension_parameters = stream_parameters,
    }};
    HandshakeMachine listener{{
        .role = ConnectionRole::listener,
        .local_socket_id = 200,
        .cookie_generator = test_cookie,
        .cookie_context = &cookie_salt,
    }};

    const auto induction = caller.start();
    const auto induction_response =
        listener.receive(induction.values[0].packet);
    const auto conclusion =
        caller.receive(induction_response.values[0].packet);
    HandshakeMessage request{
        .packet = conclusion.values[0].packet,
        .has_handshake_extension = true,
        .extension_type =
            HandshakeExtensionType::handshake_request,
        .extension_parameters =
            conclusion.values[0].extension_parameters,
    };
    const auto rejected = listener.receive(request);
    REQUIRE_EQ(listener.state(), HandshakeState::rejected);
    REQUIRE_EQ(listener.rejection_reason(), 12);
    REQUIRE_EQ(static_cast<std::int32_t>(
            rejected.values[0].packet.request),
        1'012);
    REQUIRE_EQ(rejected.values[1].kind,
        HandshakeActionKind::rejected);
}

TEST(listener_rejects_an_explicit_peer_below_the_minimum_srt_version)
{
    std::uint32_t cookie_salt = 0x9e37'79b9U;
    HandshakeExtensionParameters old_parameters;
    old_parameters.srt_version = 0x0001'0400U;
    HandshakeMachine caller{{
        .role = ConnectionRole::caller,
        .local_socket_id = 100,
        .extension_parameters = old_parameters,
    }};
    HandshakeMachine listener{{
        .role = ConnectionRole::listener,
        .local_socket_id = 200,
        .minimum_peer_srt_version = 0x0001'0500U,
        .cookie_generator = test_cookie,
        .cookie_context = &cookie_salt,
    }};

    const auto induction = caller.start();
    const auto induction_response = listener.receive(
        message_from(induction.values[0]));
    const auto conclusion = caller.receive(
        message_from(induction_response.values[0]));
    const auto rejected = listener.receive(
        message_from(conclusion.values[0]));
    REQUIRE_EQ(listener.state(), HandshakeState::rejected);
    REQUIRE_EQ(listener.rejection_reason(), 8);
    REQUIRE_EQ(static_cast<std::int32_t>(
            rejected.values[0].packet.request),
        1'008);
}

TEST(caller_rejects_an_explicit_response_below_the_minimum_srt_version)
{
    std::uint32_t cookie_salt = 0x9e37'79b9U;
    HandshakeMachine caller{{
        .role = ConnectionRole::caller,
        .local_socket_id = 100,
        .minimum_peer_srt_version = 0x0001'0500U,
    }};
    HandshakeMachine listener{{
        .role = ConnectionRole::listener,
        .local_socket_id = 200,
        .cookie_generator = test_cookie,
        .cookie_context = &cookie_salt,
    }};

    const auto induction = caller.start();
    const auto induction_response = listener.receive(
        message_from(induction.values[0]));
    const auto conclusion = caller.receive(
        message_from(induction_response.values[0]));
    const auto listener_done = listener.receive(
        message_from(conclusion.values[0]));
    auto response = message_from(listener_done.values[0]);
    response.extension_parameters.srt_version = 0x0001'0400U;
    const auto rejected = caller.receive(response);
    REQUIRE_EQ(rejected.values[0].kind,
        HandshakeActionKind::rejected);
    REQUIRE_EQ(rejected.values[0].rejection_reason, 8);
    REQUIRE_EQ(caller.rejection_reason(), 8);
}

TEST(caller_rejects_a_legacy_induction_downgrade_immediately)
{
    HandshakeMachine caller{{
        .role = ConnectionRole::caller,
        .local_socket_id = 100,
    }};
    (void)caller.start();

    Handshake response;
    response.version = handshake_version_4;
    response.request = HandshakeRequest::induction;
    response.socket_id = 200;
    response.syn_cookie = 0x1020'3040U;
    const auto rejected = caller.receive(response);

    REQUIRE_EQ(rejected.size, 1U);
    REQUIRE_EQ(rejected.values[0].kind,
        HandshakeActionKind::rejected);
    REQUIRE_EQ(rejected.values[0].rejection_reason, 8);
    REQUIRE_EQ(caller.rejection_reason(), 8);
    REQUIRE_EQ(caller.state(), HandshakeState::rejected);
}

TEST(caller_rejects_invalid_hsv5_induction_magic_as_rogue)
{
    HandshakeMachine caller{{
        .role = ConnectionRole::caller,
        .local_socket_id = 100,
    }};
    (void)caller.start();

    Handshake response;
    response.version = handshake_version_5;
    response.request = HandshakeRequest::induction;
    response.extension_field = 0U;
    response.socket_id = 200;
    response.syn_cookie = 0x1020'3040U;
    const auto rejected = caller.receive(response);

    REQUIRE_EQ(rejected.size, 1U);
    REQUIRE_EQ(rejected.values[0].kind,
        HandshakeActionKind::rejected);
    REQUIRE_EQ(rejected.values[0].rejection_reason, 4);
    REQUIRE_EQ(caller.rejection_reason(), 4);
    REQUIRE_EQ(caller.state(), HandshakeState::rejected);
}

TEST(listener_rejects_a_legacy_conclusion_downgrade)
{
    std::uint32_t cookie_salt = 0x9e37'79b9U;
    HandshakeMachine caller{{
        .role = ConnectionRole::caller,
        .local_socket_id = 100,
    }};
    HandshakeMachine listener{{
        .role = ConnectionRole::listener,
        .local_socket_id = 200,
        .cookie_generator = test_cookie,
        .cookie_context = &cookie_salt,
    }};

    const auto induction = caller.start();
    const auto induction_response = listener.receive(
        message_from(induction.values[0]));
    const auto conclusion = caller.receive(
        message_from(induction_response.values[0]));
    auto legacy = message_from(conclusion.values[0]);
    legacy.packet.version = handshake_version_4;
    const auto rejected = listener.receive(legacy);

    REQUIRE_EQ(listener.state(), HandshakeState::rejected);
    REQUIRE_EQ(listener.rejection_reason(), 8);
    REQUIRE(!listener.has_peer_extension_parameters());
    REQUIRE_EQ(static_cast<std::int32_t>(
            rejected.values[0].packet.request),
        1'008);
}

TEST(caller_rejects_a_legacy_conclusion_response)
{
    std::uint32_t cookie_salt = 0x9e37'79b9U;
    HandshakeMachine caller{{
        .role = ConnectionRole::caller,
        .local_socket_id = 100,
    }};
    HandshakeMachine listener{{
        .role = ConnectionRole::listener,
        .local_socket_id = 200,
        .cookie_generator = test_cookie,
        .cookie_context = &cookie_salt,
    }};

    const auto induction = caller.start();
    const auto induction_response = listener.receive(
        message_from(induction.values[0]));
    const auto conclusion = caller.receive(
        message_from(induction_response.values[0]));
    const auto listener_done = listener.receive(
        message_from(conclusion.values[0]));
    auto legacy = message_from(listener_done.values[0]);
    legacy.packet.version = handshake_version_4;
    const auto rejected = caller.receive(legacy);

    REQUIRE_EQ(rejected.values[0].kind,
        HandshakeActionKind::rejected);
    REQUIRE_EQ(rejected.values[0].rejection_reason, 8);
    REQUIRE_EQ(caller.state(), HandshakeState::rejected);
    REQUIRE(!caller.has_peer_extension_parameters());
}

TEST(connected_listener_ignores_a_legacy_conclusion_replay)
{
    std::uint32_t cookie_salt = 0x9e37'79b9U;
    HandshakeMachine caller{{
        .role = ConnectionRole::caller,
        .local_socket_id = 100,
    }};
    HandshakeMachine listener{{
        .role = ConnectionRole::listener,
        .local_socket_id = 200,
        .cookie_generator = test_cookie,
        .cookie_context = &cookie_salt,
    }};

    const auto induction = caller.start();
    const auto induction_response = listener.receive(
        message_from(induction.values[0]));
    const auto conclusion = caller.receive(
        message_from(induction_response.values[0]));
    auto request = message_from(conclusion.values[0]);
    const auto listener_done = listener.receive(request);
    REQUIRE_EQ(listener_done.values[1].kind,
        HandshakeActionKind::connected);

    request.packet.version = handshake_version_4;
    const auto ignored = listener.receive(request);
    REQUIRE_EQ(ignored.size, 0U);
    REQUIRE_EQ(listener.state(), HandshakeState::connected);
}

TEST(listener_rejects_a_pre_hsv5_software_version_as_rogue)
{
    std::uint32_t cookie_salt = 0x9e37'79b9U;
    HandshakeMachine caller{{
        .role = ConnectionRole::caller,
        .local_socket_id = 100,
    }};
    HandshakeMachine listener{{
        .role = ConnectionRole::listener,
        .local_socket_id = 200,
        .cookie_generator = test_cookie,
        .cookie_context = &cookie_salt,
    }};

    const auto induction = caller.start();
    const auto induction_response = listener.receive(
        message_from(induction.values[0]));
    const auto conclusion = caller.receive(
        message_from(induction_response.values[0]));
    auto request = message_from(conclusion.values[0]);
    request.extension_parameters.srt_version = 0x0001'0200U;
    const auto rejected = listener.receive(request);

    REQUIRE_EQ(listener.state(), HandshakeState::rejected);
    REQUIRE_EQ(listener.rejection_reason(), 4);
    REQUIRE_EQ(static_cast<std::int32_t>(
            rejected.values[0].packet.request),
        1'004);
    REQUIRE(!listener.has_peer_extension_parameters());
}

TEST(caller_rejects_a_pre_hsv5_response_version_as_rogue)
{
    std::uint32_t cookie_salt = 0x9e37'79b9U;
    HandshakeMachine caller{{
        .role = ConnectionRole::caller,
        .local_socket_id = 100,
    }};
    HandshakeMachine listener{{
        .role = ConnectionRole::listener,
        .local_socket_id = 200,
        .cookie_generator = test_cookie,
        .cookie_context = &cookie_salt,
    }};

    const auto induction = caller.start();
    const auto induction_response = listener.receive(
        message_from(induction.values[0]));
    const auto conclusion = caller.receive(
        message_from(induction_response.values[0]));
    const auto listener_done = listener.receive(
        message_from(conclusion.values[0]));
    auto response = message_from(listener_done.values[0]);
    response.extension_parameters.srt_version = 0x0001'0200U;
    const auto rejected = caller.receive(response);

    REQUIRE_EQ(rejected.values[0].kind,
        HandshakeActionKind::rejected);
    REQUIRE_EQ(rejected.values[0].rejection_reason, 4);
    REQUIRE_EQ(caller.state(), HandshakeState::rejected);
    REQUIRE(!caller.has_peer_extension_parameters());
}

TEST(caller_and_listener_exchange_the_file_congestion_controller)
{
    std::uint32_t cookie_salt = 0x9e37'79b9U;
    HandshakeMachine caller{{
        .role = ConnectionRole::caller,
        .local_socket_id = 100,
        .congestion_controller =
            CongestionController::file,
    }};
    HandshakeMachine listener{{
        .role = ConnectionRole::listener,
        .local_socket_id = 200,
        .congestion_controller =
            CongestionController::file,
        .cookie_generator = test_cookie,
        .cookie_context = &cookie_salt,
    }};

    const auto induction = caller.start();
    const auto induction_response = listener.receive(
        message_from(induction.values[0]));
    const auto conclusion = caller.receive(
        message_from(induction_response.values[0]));
    REQUIRE(conclusion.values[0]
        .has_congestion_extension);
    REQUIRE_EQ(conclusion.values[0]
            .congestion_controller,
        CongestionController::file);
    REQUIRE((conclusion.values[0]
            .packet.extension_field & 4U) != 0U);

    const auto listener_done = listener.receive(
        message_from(conclusion.values[0]));
    REQUIRE_EQ(listener_done.values[1].kind,
        HandshakeActionKind::connected);
    REQUIRE(listener_done.values[0]
        .has_congestion_extension);
    REQUIRE_EQ(listener_done.values[0]
            .congestion_controller,
        CongestionController::file);

    const auto caller_done = caller.receive(
        message_from(listener_done.values[0]));
    REQUIRE_EQ(caller_done.values[0].kind,
        HandshakeActionKind::connected);
}

TEST(caller_accepts_file_controller_without_a_response_config_echo)
{
    std::uint32_t cookie_salt = 0x9e37'79b9U;
    HandshakeMachine caller{{
        .role = ConnectionRole::caller,
        .local_socket_id = 100,
        .congestion_controller =
            CongestionController::file,
    }};
    HandshakeMachine listener{{
        .role = ConnectionRole::listener,
        .local_socket_id = 200,
        .congestion_controller =
            CongestionController::file,
        .cookie_generator = test_cookie,
        .cookie_context = &cookie_salt,
    }};

    const auto induction = caller.start();
    const auto induction_response = listener.receive(
        message_from(induction.values[0]));
    const auto conclusion = caller.receive(
        message_from(induction_response.values[0]));
    const auto listener_done = listener.receive(
        message_from(conclusion.values[0]));
    auto response = message_from(listener_done.values[0]);
    response.packet.extension_field &= ~4U;
    response.has_congestion_extension = false;

    const auto caller_done = caller.receive(response);
    REQUIRE_EQ(caller_done.values[0].kind,
        HandshakeActionKind::connected);
    REQUIRE_EQ(caller.state(), HandshakeState::connected);
}

TEST(caller_accepts_file_mode_without_a_response_handshake_echo)
{
    std::uint32_t cookie_salt = 0x9e37'79b9U;
    HandshakeExtensionParameters file_parameters;
    file_parameters.flags |= static_cast<std::uint32_t>(
        HandshakeExtensionFlag::stream);
    HandshakeMachine caller{{
        .role = ConnectionRole::caller,
        .local_socket_id = 100,
        .extension_parameters = file_parameters,
        .congestion_controller =
            CongestionController::file,
    }};
    HandshakeMachine listener{{
        .role = ConnectionRole::listener,
        .local_socket_id = 200,
        .extension_parameters = file_parameters,
        .congestion_controller =
            CongestionController::file,
        .cookie_generator = test_cookie,
        .cookie_context = &cookie_salt,
    }};

    const auto induction = caller.start();
    const auto induction_response = listener.receive(
        message_from(induction.values[0]));
    const auto conclusion = caller.receive(
        message_from(induction_response.values[0]));
    const auto listener_done = listener.receive(
        message_from(conclusion.values[0]));
    auto response = message_from(listener_done.values[0]);
    response.packet.extension_field &= ~5U;
    response.has_handshake_extension = false;
    response.has_congestion_extension = false;

    const auto caller_done = caller.receive(response);
    REQUIRE_EQ(caller_done.values[0].kind,
        HandshakeActionKind::connected);
    REQUIRE_EQ(caller.state(), HandshakeState::connected);
}

TEST(caller_accepts_an_explicit_response_mode_after_listener_acceptance)
{
    std::uint32_t cookie_salt = 0x9e37'79b9U;
    HandshakeExtensionParameters file_parameters;
    file_parameters.flags |= static_cast<std::uint32_t>(
        HandshakeExtensionFlag::stream);
    HandshakeMachine caller{{
        .role = ConnectionRole::caller,
        .local_socket_id = 100,
        .extension_parameters = file_parameters,
        .congestion_controller =
            CongestionController::file,
    }};
    HandshakeMachine listener{{
        .role = ConnectionRole::listener,
        .local_socket_id = 200,
        .extension_parameters = file_parameters,
        .congestion_controller =
            CongestionController::file,
        .cookie_generator = test_cookie,
        .cookie_context = &cookie_salt,
    }};

    const auto induction = caller.start();
    const auto induction_response = listener.receive(
        message_from(induction.values[0]));
    const auto conclusion = caller.receive(
        message_from(induction_response.values[0]));
    const auto listener_done = listener.receive(
        message_from(conclusion.values[0]));
    auto response = message_from(listener_done.values[0]);
    response.has_handshake_extension = true;
    response.extension_parameters.flags &=
        ~static_cast<std::uint32_t>(
            HandshakeExtensionFlag::stream);

    const auto caller_done = caller.receive(response);
    REQUIRE_EQ(caller_done.values[0].kind,
        HandshakeActionKind::connected);
    REQUIRE_EQ(caller.rejection_reason(), 0);
    REQUIRE_EQ(caller.state(), HandshakeState::connected);
}

TEST(caller_rejects_an_explicitly_different_response_controller)
{
    std::uint32_t cookie_salt = 0x9e37'79b9U;
    HandshakeMachine caller{{
        .role = ConnectionRole::caller,
        .local_socket_id = 100,
        .congestion_controller =
            CongestionController::file,
    }};
    HandshakeMachine listener{{
        .role = ConnectionRole::listener,
        .local_socket_id = 200,
        .congestion_controller =
            CongestionController::file,
        .cookie_generator = test_cookie,
        .cookie_context = &cookie_salt,
    }};

    const auto induction = caller.start();
    const auto induction_response = listener.receive(
        message_from(induction.values[0]));
    const auto conclusion = caller.receive(
        message_from(induction_response.values[0]));
    const auto listener_done = listener.receive(
        message_from(conclusion.values[0]));
    auto response = message_from(listener_done.values[0]);
    response.has_congestion_extension = true;
    response.congestion_controller =
        CongestionController::live;

    const auto rejected = caller.receive(response);
    REQUIRE_EQ(rejected.values[0].kind,
        HandshakeActionKind::rejected);
    REQUIRE_EQ(rejected.values[0].rejection_reason, 13);
    REQUIRE_EQ(caller.rejection_reason(), 13);
    REQUIRE_EQ(caller.state(), HandshakeState::rejected);
}

TEST(listener_rejects_a_congestion_controller_mismatch)
{
    std::uint32_t cookie_salt = 0x9e37'79b9U;
    HandshakeMachine caller{{
        .role = ConnectionRole::caller,
        .local_socket_id = 100,
        .congestion_controller =
            CongestionController::file,
    }};
    HandshakeMachine listener{{
        .role = ConnectionRole::listener,
        .local_socket_id = 200,
        .cookie_generator = test_cookie,
        .cookie_context = &cookie_salt,
    }};

    const auto induction = caller.start();
    const auto induction_response = listener.receive(
        message_from(induction.values[0]));
    const auto conclusion = caller.receive(
        message_from(induction_response.values[0]));
    const auto rejected = listener.receive(
        message_from(conclusion.values[0]));
    REQUIRE_EQ(listener.state(),
        HandshakeState::rejected);
    REQUIRE_EQ(listener.rejection_reason(), 13);
    REQUIRE_EQ(static_cast<std::int32_t>(
            rejected.values[0].packet.request),
        1'013);
    REQUIRE_EQ(rejected.values[1].kind,
        HandshakeActionKind::rejected);
}

TEST(caller_and_listener_negotiate_complementary_packet_filter_parameters)
{
    const auto caller_filter =
        parse_packet_filter_configuration(
            "fec,cols:10,arq:onreq");
    const auto listener_filter =
        parse_packet_filter_configuration(
            "fec,rows:5,layout:even");
    REQUIRE(caller_filter);
    REQUIRE(listener_filter);

    std::uint32_t cookie_salt = 0x62a1'09b3U;
    HandshakeMachine caller{{
        .role = ConnectionRole::caller,
        .local_socket_id = 100,
        .initial_sequence = SequenceNumber{10},
        .packet_filter_configuration =
            caller_filter.configuration,
    }};
    HandshakeMachine listener{{
        .role = ConnectionRole::listener,
        .local_socket_id = 200,
        .initial_sequence = SequenceNumber{20},
        .packet_filter_configuration =
            listener_filter.configuration,
        .cookie_generator = test_cookie,
        .cookie_context = &cookie_salt,
    }};

    const auto induction = caller.start();
    const auto induction_response =
        listener.receive(
            message_from(induction.values[0]));
    const auto conclusion =
        caller.receive(
            message_from(
                induction_response.values[0]));
    REQUIRE(conclusion.values[0]
        .has_packet_filter_extension);
    REQUIRE_EQ(conclusion.values[0]
            .packet_filter_configuration.view(),
        caller_filter.configuration.view());

    const auto listener_done =
        listener.receive(
            message_from(conclusion.values[0]));
    REQUIRE(listener_done.values[0]
        .has_packet_filter_extension);
    REQUIRE(listener.has_negotiated_packet_filter());
    REQUIRE_EQ(listener.negotiated_packet_filter().view(),
        std::string_view{
            "fec,arq:onreq,cols:10,layout:even,rows:5"});

    const auto caller_done =
        caller.receive(
            message_from(listener_done.values[0]));
    REQUIRE_EQ(caller_done.values[0].kind,
        HandshakeActionKind::connected);
    REQUIRE(caller.has_negotiated_packet_filter());
    REQUIRE_EQ(caller.negotiated_packet_filter().view(),
        listener.negotiated_packet_filter().view());
}

TEST(listener_rejects_conflicting_packet_filter_parameters)
{
    const auto listener_filter =
        parse_packet_filter_configuration(
            "fec,cols:10");
    const auto caller_filter =
        parse_packet_filter_configuration(
            "fec,cols:11");
    REQUIRE(listener_filter);
    REQUIRE(caller_filter);

    std::uint32_t cookie_salt = 0x7a52'19c1U;
    HandshakeMachine listener{{
        .role = ConnectionRole::listener,
        .local_socket_id = 200,
        .packet_filter_configuration =
            listener_filter.configuration,
        .cookie_generator = test_cookie,
        .cookie_context = &cookie_salt,
    }};
    Handshake induction;
    induction.version = handshake_version_4;
    induction.socket_id = 100;
    const auto induction_response =
        listener.receive(induction);

    HandshakeMessage conclusion;
    conclusion.packet.version = handshake_version_5;
    conclusion.packet.request =
        HandshakeRequest::conclusion;
    conclusion.packet.socket_id = 100;
    conclusion.packet.syn_cookie =
        induction_response.values[0]
            .packet.syn_cookie;
    conclusion.has_handshake_extension = true;
    conclusion.extension_type =
        HandshakeExtensionType::handshake_request;
    conclusion.has_packet_filter_extension = true;
    conclusion.packet_filter_configuration =
        caller_filter.configuration;

    const auto rejected =
        listener.receive(conclusion);
    REQUIRE_EQ(listener.state(),
        HandshakeState::rejected);
    REQUIRE_EQ(listener.rejection_reason(), 14);
    REQUIRE_EQ(static_cast<std::int32_t>(
            rejected.values[0].packet.request),
        1'014);
    REQUIRE_EQ(rejected.values[1].kind,
        HandshakeActionKind::rejected);
}

TEST(configured_listener_does_not_force_an_unrequested_packet_filter)
{
    const auto listener_filter =
        parse_packet_filter_configuration(
            "fec,cols:10");
    REQUIRE(listener_filter);
    std::uint32_t cookie_salt = 0x8125'bd41U;
    HandshakeMachine caller{{
        .role = ConnectionRole::caller,
        .local_socket_id = 100,
    }};
    HandshakeMachine listener{{
        .role = ConnectionRole::listener,
        .local_socket_id = 200,
        .packet_filter_configuration =
            listener_filter.configuration,
        .cookie_generator = test_cookie,
        .cookie_context = &cookie_salt,
    }};

    const auto induction = caller.start();
    const auto induction_response =
        listener.receive(
            message_from(induction.values[0]));
    const auto conclusion =
        caller.receive(message_from(
            induction_response.values[0]));
    REQUIRE(!conclusion.values[0]
        .has_packet_filter_extension);
    const auto listener_done =
        listener.receive(message_from(
            conclusion.values[0]));
    REQUIRE(!listener_done.values[0]
        .has_packet_filter_extension);
    REQUIRE(!listener.has_negotiated_packet_filter());
    const auto caller_done =
        caller.receive(message_from(
            listener_done.values[0]));
    REQUIRE_EQ(caller_done.values[0].kind,
        HandshakeActionKind::connected);
    REQUIRE(!caller.has_negotiated_packet_filter());
}
