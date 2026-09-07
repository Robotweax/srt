#include "test.hpp"
#include "crypto_test_helpers.hpp"

#include "robotweax/srt/crypto.hpp"
#include "robotweax/srt/handshake_datagram.hpp"
#include "robotweax/srt/rendezvous.hpp"

#include <algorithm>
#include <cstddef>

using namespace robotweax::srt;

namespace {

[[nodiscard]] const HandshakeAction& sent(
    const HandshakeActions& actions)
{
    for (std::size_t index = 0; index < actions.size; ++index) {
        if (actions.values[index].kind
            == HandshakeActionKind::send) {
            return actions.values[index];
        }
    }
    throw std::runtime_error("no send action");
}

[[nodiscard]] bool has_action(
    const HandshakeActions& actions,
    HandshakeActionKind kind)
{
    return std::any_of(actions.values.begin(),
        actions.values.begin()
            + static_cast<std::ptrdiff_t>(actions.size),
        [kind](const HandshakeAction& action) {
            return action.kind == kind;
        });
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
        .has_stream_id_extension =
            action.has_stream_id_extension,
        .stream_id = action.stream_id,
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

TEST(rendezvous_stream_id_follows_the_protocol_initiator)
{
    StreamId initiator_stream_id;
    StreamId responder_stream_id;
    REQUIRE(initiator_stream_id.assign("initiator-route"));
    REQUIRE(responder_stream_id.assign("responder-route"));
    RendezvousHandshakeMachine initiator{{
        .local_socket_id = 10U,
        .local_cookie = 200U,
        .stream_id = initiator_stream_id,
    }};
    RendezvousHandshakeMachine responder{{
        .local_socket_id = 20U,
        .local_cookie = 100U,
        .stream_id = responder_stream_id,
    }};

    const auto initiator_wave = initiator.start();
    const auto responder_wave = responder.start();
    const auto request = initiator.receive(
        message_from(sent(responder_wave)));
    (void)responder.receive(
        message_from(sent(initiator_wave)));
    REQUIRE(sent(request).has_stream_id_extension);
    REQUIRE_EQ(sent(request).stream_id.view(),
        initiator_stream_id.view());

    const auto response = responder.receive(
        message_from(sent(request)));
    REQUIRE(responder.has_peer_stream_id());
    REQUIRE_EQ(responder.peer_stream_id().view(),
        initiator_stream_id.view());
    REQUIRE(!sent(response).has_stream_id_extension);

    auto response_message = message_from(sent(response));
    response_message.has_stream_id_extension = true;
    response_message.stream_id = responder_stream_id;
    const auto completion = initiator.receive(response_message);
    REQUIRE(has_action(
        completion, HandshakeActionKind::connected));
    REQUIRE(!initiator.has_peer_stream_id());
}

[[nodiscard]] RendezvousHandshakeMachine make_machine(std::uint32_t socket_id,
    std::uint32_t cookie, SequenceNumber sequence,
    std::uint32_t flow_window = 25'600U)
{
    return RendezvousHandshakeMachine {{
        .local_socket_id = socket_id,
        .local_cookie = cookie,
        .initial_sequence = sequence,
        .flow_window = flow_window,
        .timeout_milliseconds = 10,
        .maximum_retries = 4,
    }};
}

[[nodiscard]] RendezvousHandshakeMachine make_file_machine(
    std::uint32_t socket_id,
    std::uint32_t cookie)
{
    HandshakeExtensionParameters parameters;
    parameters.flags |= static_cast<std::uint32_t>(
        HandshakeExtensionFlag::stream);
    return RendezvousHandshakeMachine{{
        .local_socket_id = socket_id,
        .local_cookie = cookie,
        .extension_parameters = parameters,
        .congestion_controller =
            CongestionController::file,
    }};
}

[[nodiscard]] HandshakeActions exchange_file_request(
    RendezvousHandshakeMachine& initiator,
    RendezvousHandshakeMachine& responder)
{
    const auto initiator_wave = initiator.start();
    const auto responder_wave = responder.start();
    const auto request = initiator.receive(
        message_from(sent(responder_wave)));
    (void)responder.receive(
        message_from(sent(initiator_wave)));
    return responder.receive(message_from(sent(request)));
}

} // namespace

TEST(rendezvous_starts_with_hsv5_waveahand)
{
    auto machine = make_machine(
        10U, 0x1000'0000U, SequenceNumber{123U});
    const auto actions = machine.start();
    const auto& wave = sent(actions);
    REQUIRE_EQ(machine.state(), RendezvousState::waving);
    REQUIRE_EQ(wave.packet.version, handshake_version_5);
    REQUIRE_EQ(wave.packet.request,
        HandshakeRequest::wave_a_hand);
    REQUIRE_EQ(wave.packet.extension_field, 0U);
    REQUIRE_EQ(wave.packet.socket_id, 10U);
    REQUIRE_EQ(wave.packet.syn_cookie, 0x1000'0000U);
    REQUIRE_EQ(wave.packet.initial_sequence,
        SequenceNumber{123U});
    REQUIRE(has_action(actions,
        HandshakeActionKind::arm_timer));
}

TEST(rendezvous_rejects_an_explicit_peer_below_the_minimum_srt_version)
{
    HandshakeExtensionParameters old_parameters;
    old_parameters.srt_version = 0x0001'0400U;
    RendezvousHandshakeMachine initiator{{
        .local_socket_id = 10U,
        .local_cookie = 200U,
        .extension_parameters = old_parameters,
    }};
    RendezvousHandshakeMachine responder{{
        .local_socket_id = 20U,
        .local_cookie = 100U,
        .minimum_peer_srt_version = 0x0001'0500U,
    }};

    const auto initiator_wave = initiator.start();
    const auto responder_wave = responder.start();
    const auto request = initiator.receive(
        message_from(sent(responder_wave)));
    (void)responder.receive(
        message_from(sent(initiator_wave)));
    const auto rejected = responder.receive(
        message_from(sent(request)));
    REQUIRE_EQ(responder.state(), RendezvousState::rejected);
    REQUIRE_EQ(responder.rejection_reason(), 8);
    REQUIRE_EQ(static_cast<std::int32_t>(
            sent(rejected).packet.request),
        1'008);
}

TEST(rendezvous_rejects_a_legacy_handshake_downgrade)
{
    auto machine = make_machine(
        10U, 200U, SequenceNumber{123U});
    (void)machine.start();

    Handshake wave;
    wave.version = handshake_version_4;
    wave.request = HandshakeRequest::wave_a_hand;
    wave.socket_id = 20U;
    wave.syn_cookie = 100U;
    const auto rejected = machine.receive(wave);

    REQUIRE_EQ(machine.state(), RendezvousState::rejected);
    REQUIRE_EQ(machine.rejection_reason(), 8);
    REQUIRE_EQ(static_cast<std::int32_t>(
            sent(rejected).packet.request),
        1'008);
    REQUIRE(has_action(rejected,
        HandshakeActionKind::rejected));
}

TEST(rendezvous_rejects_a_pre_hsv5_software_version_as_rogue)
{
    auto machine = make_machine(
        10U, 200U, SequenceNumber{123U});
    (void)machine.start();

    HandshakeMessage request;
    request.packet.version = handshake_version_5;
    request.packet.request = HandshakeRequest::conclusion;
    request.packet.socket_id = 20U;
    request.packet.syn_cookie = 100U;
    request.has_handshake_extension = true;
    request.extension_type =
        HandshakeExtensionType::handshake_request;
    request.extension_parameters.srt_version = 0x0001'0200U;
    const auto rejected = machine.receive(request);

    REQUIRE_EQ(machine.state(), RendezvousState::rejected);
    REQUIRE_EQ(machine.rejection_reason(), 4);
    REQUIRE_EQ(static_cast<std::int32_t>(
            sent(rejected).packet.request),
        1'004);
    REQUIRE(!machine.has_peer_extension_parameters());
}

TEST(rendezvous_cookie_contest_is_symmetric_at_wrap_boundaries)
{
    REQUIRE_EQ(resolve_rendezvous_role(20U, 10U),
        RendezvousRole::initiator);
    REQUIRE_EQ(resolve_rendezvous_role(10U, 20U),
        RendezvousRole::responder);
    REQUIRE_EQ(resolve_rendezvous_role(0xffff'ffffU, 1U),
        RendezvousRole::responder);
    REQUIRE_EQ(resolve_rendezvous_role(1U, 0xffff'ffffU),
        RendezvousRole::initiator);
    REQUIRE_EQ(resolve_rendezvous_role(0x8000'0000U, 0U),
        RendezvousRole::responder);
    REQUIRE_EQ(resolve_rendezvous_role(0U, 0x8000'0000U),
        RendezvousRole::initiator);
    REQUIRE_EQ(resolve_rendezvous_role(42U, 42U),
        RendezvousRole::unresolved);
}

TEST(rendezvous_parallel_flow_completes_both_roles)
{
    auto initiator = make_machine(10U, 200U, SequenceNumber {1000U}, 111U);
    auto responder = make_machine(20U, 100U, SequenceNumber {2000U}, 222U);
    const auto initiator_wave = initiator.start();
    const auto responder_wave = responder.start();

    const auto initiator_attention = initiator.receive(
        message_from(sent(responder_wave)));
    const auto responder_attention = responder.receive(
        message_from(sent(initiator_wave)));
    REQUIRE_EQ(initiator.role(),
        RendezvousRole::initiator);
    REQUIRE_EQ(responder.role(),
        RendezvousRole::responder);
    REQUIRE(sent(initiator_attention)
        .has_handshake_extension);
    REQUIRE_EQ(sent(initiator_attention).extension_type,
        HandshakeExtensionType::handshake_request);
    REQUIRE(!sent(responder_attention)
        .has_handshake_extension);

    const auto repeated_request = initiator.receive(
        message_from(sent(responder_attention)));
    REQUIRE_EQ(initiator.state(),
        RendezvousState::initiated);
    REQUIRE_EQ(sent(repeated_request).extension_type,
        HandshakeExtensionType::handshake_request);

    const auto response = responder.receive(
        message_from(sent(initiator_attention)));
    REQUIRE_EQ(responder.state(),
        RendezvousState::initiated);
    REQUIRE_EQ(sent(response).extension_type,
        HandshakeExtensionType::handshake_response);

    const auto initiator_connected = initiator.receive(
        message_from(sent(response)));
    REQUIRE(has_action(initiator_connected,
        HandshakeActionKind::connected));
    REQUIRE_EQ(sent(initiator_connected).packet.request,
        HandshakeRequest::agreement);

    const auto responder_connected = responder.receive(
        message_from(sent(initiator_connected)));
    REQUIRE(has_action(responder_connected,
        HandshakeActionKind::connected));
    REQUIRE_EQ(initiator.state(),
        RendezvousState::connected);
    REQUIRE_EQ(responder.state(),
        RendezvousState::connected);
    REQUIRE_EQ(initiator.peer_initial_sequence(),
        SequenceNumber{2000U});
    REQUIRE_EQ(responder.peer_initial_sequence(),
        SequenceNumber{1000U});
    REQUIRE_EQ(initiator.peer_flow_window(), 222U);
    REQUIRE_EQ(responder.peer_flow_window(), 111U);

    auto legacy_replay = message_from(sent(response));
    legacy_replay.packet.version = handshake_version_4;
    const auto ignored = initiator.receive(legacy_replay);
    REQUIRE_EQ(ignored.size, 0U);
    REQUIRE_EQ(initiator.state(),
        RendezvousState::connected);
}

TEST(rendezvous_serial_flow_completes_from_first_conclusion)
{
    auto initiator = make_machine(
        10U, 200U, SequenceNumber{1000U});
    auto responder = make_machine(
        20U, 100U, SequenceNumber{2000U});
    const auto initiator_wave = initiator.start();
    (void)responder.start();

    const auto request = initiator.receive(
        message_from(sent(responder.timeout())));
    REQUIRE_EQ(initiator.state(),
        RendezvousState::attention);
    const auto response = responder.receive(
        message_from(sent(request)));
    REQUIRE_EQ(responder.state(),
        RendezvousState::fine);
    REQUIRE_EQ(sent(response).extension_type,
        HandshakeExtensionType::handshake_response);

    const auto agreement = initiator.receive(
        message_from(sent(response)));
    const auto done = responder.receive(
        message_from(sent(agreement)));
    REQUIRE(has_action(done,
        HandshakeActionKind::connected));
    REQUIRE_EQ(initiator.state(),
        RendezvousState::connected);
    REQUIRE_EQ(responder.state(),
        RendezvousState::connected);
    (void)initiator_wave;
}

TEST(rendezvous_rejects_peer_flow_window_changes_during_establishment)
{
    auto initiator = make_machine(10U, 200U, SequenceNumber {1000U}, 111U);
    auto responder = make_machine(20U, 100U, SequenceNumber {2000U}, 222U);
    const auto initiator_wave = initiator.start();
    const auto responder_wave = responder.start();
    (void)initiator.receive(message_from(sent(responder_wave)));
    const auto responder_attention =
        responder.receive(message_from(sent(initiator_wave)));

    auto inconsistent = message_from(sent(responder_attention));
    inconsistent.packet.flow_window = 223U;
    const auto failed = initiator.receive(inconsistent);

    REQUIRE(has_action(failed, HandshakeActionKind::failed));
    REQUIRE_EQ(initiator.state(), RendezvousState::failed);
    REQUIRE_EQ(initiator.peer_flow_window(), 222U);
}

TEST(rendezvous_rejects_equal_cookies)
{
    auto left = make_machine(
        10U, 42U, SequenceNumber{1U});
    auto right = make_machine(
        20U, 42U, SequenceNumber{2U});
    const auto right_wave = right.start();
    (void)left.start();
    const auto rejected = left.receive(
        message_from(sent(right_wave)));
    REQUIRE_EQ(left.state(),
        RendezvousState::rejected);
    REQUIRE_EQ(left.rejection_reason(), 9);
    REQUIRE_EQ(static_cast<std::int32_t>(
            sent(rejected).packet.request),
        1009);
    REQUIRE(has_action(rejected,
        HandshakeActionKind::rejected));
}

TEST(rendezvous_rejects_message_and_buffer_mode_mismatch)
{
    HandshakeExtensionParameters stream_parameters;
    stream_parameters.flags |= static_cast<std::uint32_t>(
        HandshakeExtensionFlag::stream);
    RendezvousHandshakeMachine initiator{{
        .local_socket_id = 10U,
        .local_cookie = 200U,
        .initial_sequence = SequenceNumber{1000U},
        .extension_parameters = stream_parameters,
    }};
    auto responder = make_machine(
        20U, 100U, SequenceNumber{2000U});
    const auto initiator_wave = initiator.start();
    const auto responder_wave = responder.start();
    const auto request = initiator.receive(
        message_from(sent(responder_wave)));
    (void)responder.receive(
        message_from(sent(initiator_wave)));

    const auto rejected = responder.receive(
        message_from(sent(request)));
    REQUIRE_EQ(responder.state(),
        RendezvousState::rejected);
    REQUIRE_EQ(responder.rejection_reason(), 12);
    REQUIRE_EQ(static_cast<std::int32_t>(
            sent(rejected).packet.request),
        1'012);
    REQUIRE(has_action(rejected,
        HandshakeActionKind::rejected));
}

TEST(rendezvous_retransmits_each_loss_sensitive_phase)
{
    auto initiator = make_machine(
        10U, 200U, SequenceNumber{1000U});
    auto responder = make_machine(
        20U, 100U, SequenceNumber{2000U});
    const auto initiator_wave = initiator.start();
    const auto responder_wave = responder.start();
    const auto initial_request = initiator.receive(
        message_from(sent(responder_wave)));
    const auto empty_response = responder.receive(
        message_from(sent(initiator_wave)));

    // Lose the first HSREQ and deliver only the empty conclusion.
    const auto retry_request = initiator.receive(
        message_from(sent(empty_response)));
    REQUIRE_EQ(sent(retry_request).extension_type,
        HandshakeExtensionType::handshake_request);
    const auto response = responder.receive(
        message_from(sent(retry_request)));

    // Lose HSRSP. Both timeout paths reproduce their last meaningful phase.
    const auto initiator_timeout = initiator.timeout();
    const auto responder_timeout = responder.timeout();
    REQUIRE_EQ(sent(initiator_timeout).extension_type,
        HandshakeExtensionType::handshake_request);
    REQUIRE_EQ(sent(responder_timeout).extension_type,
        HandshakeExtensionType::handshake_response);

    const auto agreement = initiator.receive(
        message_from(sent(response)));
    // Lose AGREEMENT and recover from the responder's repeated HSRSP.
    const auto repeated_agreement = initiator.receive(
        message_from(sent(responder_timeout)));
    REQUIRE_EQ(sent(repeated_agreement).packet.request,
        HandshakeRequest::agreement);
    const auto responder_connected = responder.receive(
        message_from(sent(repeated_agreement)));
    REQUIRE(has_action(responder_connected,
        HandshakeActionKind::connected));
    REQUIRE_EQ(responder.state(),
        RendezvousState::connected);
    (void)initial_request;
    (void)agreement;
}

TEST(rendezvous_encrypted_flow_carries_kmreq_and_kmrsp)
{
    CryptoSession initiator_crypto{{
        .passphrase = "robotweax-rendezvous-secret",
        .key_length = 32,
    }};
    CryptoSession responder_crypto{{
        .passphrase = "robotweax-rendezvous-secret",
        .key_length = 32,
    }};
    REQUIRE_EQ(initiator_crypto.start_initiator(),
        Error::none);

    KeyMaterialBuffer request;
    const auto pending =
        initiator_crypto.pending_key_material();
    std::copy(pending.begin(), pending.end(),
        request.bytes.begin());
    request.size = pending.size();

    RendezvousHandshakeMachine initiator{{
        .local_socket_id = 10U,
        .local_cookie = 200U,
        .initial_sequence = SequenceNumber{1000U},
        .encryption_field = 4U,
        .key_material_request = request,
    }};
    RendezvousHandshakeMachine responder{{
        .local_socket_id = 20U,
        .local_cookie = 100U,
        .initial_sequence = SequenceNumber{2000U},
        .encryption_field = 4U,
    }};
    const auto initiator_wave = initiator.start();
    const auto responder_wave = responder.start();
    const auto hsreq = initiator.receive(
        message_from(sent(responder_wave)));
    (void)responder.receive(
        message_from(sent(initiator_wave)));
    REQUIRE(sent(hsreq).has_key_material_extension);
    REQUIRE_EQ(sent(hsreq).key_material_extension_type,
        HandshakeExtensionType::key_material_request);
    std::array<std::byte, 1500> datagram{};
    const auto encoded = encode_handshake_datagram(
        sent(hsreq), PacketTimestamp{55U}, 20U, datagram);
    REQUIRE(encoded);
    const auto decoded = decode_handshake_datagram(
        std::span{datagram}.first(encoded.bytes_written));
    REQUIRE(decoded);
    REQUIRE_EQ(decoded.control.destination_socket_id, 20U);
    REQUIRE_EQ(decoded.message.packet.syn_cookie, 200U);
    REQUIRE_EQ(decoded.message.extension_type,
        HandshakeExtensionType::handshake_request);
    REQUIRE_EQ(decoded.message.key_material_extension_type,
        HandshakeExtensionType::key_material_request);
    REQUIRE_EQ(responder_crypto.accept_key_material(
            decoded.message.key_material.view(), true),
        Error::none);

    const auto hsrsp = responder.receive(
        decoded.message);
    REQUIRE(sent(hsrsp).has_key_material_extension);
    REQUIRE_EQ(sent(hsrsp).key_material_extension_type,
        HandshakeExtensionType::key_material_response);
    REQUIRE_EQ(initiator_crypto.acknowledge_key_material(
            sent(hsrsp).key_material.view(), true),
        Error::none);
    confirm_directional_test_keys(initiator_crypto, responder_crypto);
    REQUIRE_EQ(initiator_crypto.sender_state(),
        CryptoState::secured);
    REQUIRE_EQ(initiator_crypto.receiver_state(),
        CryptoState::secured);
    REQUIRE_EQ(responder_crypto.sender_state(),
        CryptoState::secured);
    REQUIRE_EQ(responder_crypto.receiver_state(),
        CryptoState::secured);
}

TEST(rendezvous_can_continue_after_optional_encryption_failure)
{
    CryptoSession initiator_crypto{{
        .passphrase = "robotweax-rendezvous-secret",
        .key_length = 16,
    }};
    REQUIRE_EQ(initiator_crypto.start_initiator(),
        Error::none);
    KeyMaterialBuffer request;
    const auto pending =
        initiator_crypto.pending_key_material();
    std::copy(pending.begin(), pending.end(),
        request.bytes.begin());
    request.size = pending.size();

    RendezvousHandshakeMachine initiator{{
        .local_socket_id = 10U,
        .local_cookie = 200U,
        .encryption_field = 2U,
        .key_material_request = request,
    }};
    RendezvousHandshakeMachine responder{{
        .local_socket_id = 20U,
        .local_cookie = 100U,
    }};
    const auto initiator_wave = initiator.start();
    const auto responder_wave = responder.start();
    const auto hsreq = initiator.receive(
        message_from(sent(responder_wave)));
    (void)responder.receive(
        message_from(sent(initiator_wave)));

    KeyMaterialBuffer failure;
    failure.size = 4U;
    failure.bytes[3] = std::byte{3U};
    REQUIRE_EQ(responder.set_key_material_response(
            failure),
        Error::none);
    const auto hsrsp = responder.receive(
        message_from(sent(hsreq)));
    REQUIRE_EQ(sent(hsrsp).key_material.size, 4U);
    REQUIRE_EQ(initiator_crypto.acknowledge_key_material(
            sent(hsrsp).key_material.view(), true),
        Error::cryptographic_failure);
    REQUIRE_EQ(initiator_crypto.sender_state(),
        CryptoState::no_secret);

    const KeyMaterialBuffer empty_request;
    REQUIRE_EQ(initiator.set_key_material_request(
            empty_request, 0U),
        Error::none);
    const auto agreement = initiator.receive(
        message_from(sent(hsrsp)));
    const auto connected = responder.receive(
        message_from(sent(agreement)));
    REQUIRE(has_action(agreement,
        HandshakeActionKind::connected));
    REQUIRE(has_action(connected,
        HandshakeActionKind::connected));
}

TEST(rendezvous_responder_accepts_a_connected_party_packet)
{
    auto initiator = make_machine(
        10U, 200U, SequenceNumber{1000U});
    auto responder = make_machine(
        20U, 100U, SequenceNumber{2000U});
    const auto initiator_wave = initiator.start();
    const auto responder_wave = responder.start();
    const auto hsreq = initiator.receive(
        message_from(sent(responder_wave)));
    (void)responder.receive(
        message_from(sent(initiator_wave)));
    (void)responder.receive(message_from(sent(hsreq)));
    REQUIRE_EQ(responder.state(),
        RendezvousState::initiated);

    const auto connected =
        responder.note_connected_packet();
    REQUIRE(has_action(connected,
        HandshakeActionKind::connected));
    REQUIRE_EQ(sent(connected).packet.request,
        HandshakeRequest::agreement);
    REQUIRE_EQ(responder.state(),
        RendezvousState::connected);
}

TEST(rendezvous_negotiates_an_exact_packet_filter_configuration)
{
    const auto filter =
        parse_packet_filter_configuration(
            "fec,cols:10,arq:onreq");
    REQUIRE(filter);
    RendezvousHandshakeMachine initiator{{
        .local_socket_id = 10U,
        .local_cookie = 200U,
        .initial_sequence = SequenceNumber{1000U},
        .packet_filter_configuration =
            filter.configuration,
    }};
    RendezvousHandshakeMachine responder{{
        .local_socket_id = 20U,
        .local_cookie = 100U,
        .initial_sequence = SequenceNumber{2000U},
        .packet_filter_configuration =
            filter.configuration,
    }};
    const auto initiator_wave = initiator.start();
    const auto responder_wave = responder.start();
    const auto request = initiator.receive(
        message_from(sent(responder_wave)));
    (void)responder.receive(
        message_from(sent(initiator_wave)));
    REQUIRE(sent(request)
        .has_packet_filter_extension);
    REQUIRE_EQ(sent(request)
            .packet_filter_configuration.view(),
        filter.configuration.view());

    const auto response = responder.receive(
        message_from(sent(request)));
    REQUIRE(sent(response)
        .has_packet_filter_extension);
    REQUIRE_EQ(sent(response)
            .packet_filter_configuration.view(),
        filter.configuration.view());
    const auto connected = initiator.receive(
        message_from(sent(response)));
    REQUIRE(has_action(connected,
        HandshakeActionKind::connected));
    REQUIRE(initiator.has_negotiated_packet_filter());
    REQUIRE(responder.has_negotiated_packet_filter());
    REQUIRE_EQ(initiator
            .negotiated_packet_filter().view(),
        filter.configuration.view());
}

TEST(rendezvous_exchanges_and_validates_the_file_controller)
{
    RendezvousHandshakeMachine initiator{{
        .local_socket_id = 10U,
        .local_cookie = 200U,
        .congestion_controller =
            CongestionController::file,
    }};
    RendezvousHandshakeMachine responder{{
        .local_socket_id = 20U,
        .local_cookie = 100U,
        .congestion_controller =
            CongestionController::file,
    }};
    const auto initiator_wave = initiator.start();
    const auto responder_wave = responder.start();
    const auto request = initiator.receive(
        message_from(sent(responder_wave)));
    (void)responder.receive(
        message_from(sent(initiator_wave)));
    REQUIRE(sent(request).has_congestion_extension);
    REQUIRE_EQ(sent(request).congestion_controller,
        CongestionController::file);

    const auto response = responder.receive(
        message_from(sent(request)));
    REQUIRE(sent(response).has_congestion_extension);
    REQUIRE_EQ(sent(response).congestion_controller,
        CongestionController::file);
    const auto connected = initiator.receive(
        message_from(sent(response)));
    REQUIRE(has_action(connected,
        HandshakeActionKind::connected));
}

TEST(rendezvous_initiator_accepts_file_response_without_stream_echo)
{
    auto initiator = make_file_machine(10U, 200U);
    auto responder = make_file_machine(20U, 100U);
    const auto response =
        exchange_file_request(initiator, responder);
    auto message = message_from(sent(response));
    message.extension_parameters.flags &=
        ~static_cast<std::uint32_t>(
            HandshakeExtensionFlag::stream);

    const auto connected = initiator.receive(message);
    REQUIRE(has_action(connected,
        HandshakeActionKind::connected));
    REQUIRE_EQ(initiator.state(),
        RendezvousState::connected);
}

TEST(rendezvous_initiator_accepts_file_response_without_config_echo)
{
    auto initiator = make_file_machine(10U, 200U);
    auto responder = make_file_machine(20U, 100U);
    const auto response =
        exchange_file_request(initiator, responder);
    auto message = message_from(sent(response));
    message.has_congestion_extension = false;

    const auto connected = initiator.receive(message);
    REQUIRE(has_action(connected,
        HandshakeActionKind::connected));
    REQUIRE_EQ(initiator.state(),
        RendezvousState::connected);
}

TEST(rendezvous_initiator_rejects_explicit_response_controller_mismatch)
{
    auto initiator = make_file_machine(10U, 200U);
    auto responder = make_file_machine(20U, 100U);
    const auto response =
        exchange_file_request(initiator, responder);
    auto message = message_from(sent(response));
    message.has_congestion_extension = true;
    message.congestion_controller =
        CongestionController::live;

    const auto rejected = initiator.receive(message);
    REQUIRE(has_action(rejected,
        HandshakeActionKind::rejected));
    REQUIRE_EQ(initiator.state(),
        RendezvousState::rejected);
    REQUIRE_EQ(initiator.rejection_reason(), 13);
}

TEST(rendezvous_rejects_a_congestion_controller_mismatch)
{
    RendezvousHandshakeMachine initiator{{
        .local_socket_id = 10U,
        .local_cookie = 200U,
        .congestion_controller =
            CongestionController::file,
    }};
    RendezvousHandshakeMachine responder{{
        .local_socket_id = 20U,
        .local_cookie = 100U,
    }};
    const auto initiator_wave = initiator.start();
    const auto responder_wave = responder.start();
    const auto request = initiator.receive(
        message_from(sent(responder_wave)));
    (void)responder.receive(
        message_from(sent(initiator_wave)));
    const auto rejected = responder.receive(
        message_from(sent(request)));
    REQUIRE_EQ(responder.state(),
        RendezvousState::rejected);
    REQUIRE_EQ(responder.rejection_reason(), 13);
    REQUIRE_EQ(static_cast<std::int32_t>(
            sent(rejected).packet.request),
        1'013);
}

TEST(rendezvous_rejects_nonidentical_packet_filter_text)
{
    const auto initiator_filter =
        parse_packet_filter_configuration(
            "fec,cols:10,arq:onreq");
    const auto responder_filter =
        parse_packet_filter_configuration(
            "fec,arq:onreq,cols:10");
    REQUIRE(initiator_filter);
    REQUIRE(responder_filter);
    RendezvousHandshakeMachine initiator{{
        .local_socket_id = 10U,
        .local_cookie = 200U,
        .packet_filter_configuration =
            initiator_filter.configuration,
    }};
    RendezvousHandshakeMachine responder{{
        .local_socket_id = 20U,
        .local_cookie = 100U,
        .packet_filter_configuration =
            responder_filter.configuration,
    }};
    const auto initiator_wave = initiator.start();
    const auto responder_wave = responder.start();
    const auto request = initiator.receive(
        message_from(sent(responder_wave)));
    (void)responder.receive(
        message_from(sent(initiator_wave)));
    const auto rejected = responder.receive(
        message_from(sent(request)));
    REQUIRE_EQ(responder.state(),
        RendezvousState::rejected);
    REQUIRE_EQ(responder.rejection_reason(), 14);
    REQUIRE_EQ(static_cast<std::int32_t>(
            sent(rejected).packet.request),
        1'014);
}

TEST(rendezvous_responder_adopts_the_initiators_packet_filter)
{
    const auto filter =
        parse_packet_filter_configuration(
            "fec,cols:10");
    REQUIRE(filter);
    RendezvousHandshakeMachine initiator{{
        .local_socket_id = 10U,
        .local_cookie = 200U,
        .packet_filter_configuration =
            filter.configuration,
    }};
    RendezvousHandshakeMachine responder{{
        .local_socket_id = 20U,
        .local_cookie = 100U,
    }};
    const auto initiator_wave = initiator.start();
    const auto responder_wave = responder.start();
    const auto request = initiator.receive(
        message_from(sent(responder_wave)));
    (void)responder.receive(
        message_from(sent(initiator_wave)));
    const auto response = responder.receive(
        message_from(sent(request)));
    REQUIRE(sent(response)
        .has_packet_filter_extension);
    REQUIRE_EQ(sent(response)
            .packet_filter_configuration.view(),
        filter.configuration.view());
    const auto connected = initiator.receive(
        message_from(sent(response)));
    REQUIRE(has_action(connected,
        HandshakeActionKind::connected));
    REQUIRE(initiator.has_negotiated_packet_filter());
    REQUIRE(responder.has_negotiated_packet_filter());
}
