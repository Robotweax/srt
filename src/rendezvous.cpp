#include "robotweax/srt/rendezvous.hpp"

#include "hsv5_version_policy.hpp"

#include <algorithm>
#include <cstdint>

namespace robotweax::srt {
namespace {

constexpr int rendezvous_cookie_rejection_reason = 9;
constexpr int message_api_rejection_reason = 12;
constexpr int congestion_rejection_reason = 13;
constexpr int packet_filter_rejection_reason = 14;
constexpr int version_rejection_reason = 8;
constexpr int rogue_rejection_reason = 4;
constexpr std::uint32_t cookie_half_range = 0x8000'0000U;

[[nodiscard]] bool has_handshake_request(
    const HandshakeMessage& message) noexcept
{
    return message.has_handshake_extension
        && message.extension_type
            == HandshakeExtensionType::handshake_request;
}

[[nodiscard]] bool has_handshake_response(
    const HandshakeMessage& message) noexcept
{
    return message.has_handshake_extension
        && message.extension_type
            == HandshakeExtensionType::handshake_response;
}

[[nodiscard]] bool is_rejection(
    HandshakeRequest request) noexcept
{
    return static_cast<std::int32_t>(request) >= 1'000;
}

} // namespace

RendezvousRole resolve_rendezvous_role(
    std::uint32_t local_cookie,
    std::uint32_t peer_cookie) noexcept
{
    const std::uint32_t difference = local_cookie - peer_cookie;
    if (difference == 0U) {
        return RendezvousRole::unresolved;
    }
    if (difference < cookie_half_range) {
        return RendezvousRole::initiator;
    }
    if (difference > cookie_half_range) {
        return RendezvousRole::responder;
    }
    return static_cast<std::int32_t>(local_cookie)
            > static_cast<std::int32_t>(peer_cookie)
        ? RendezvousRole::initiator
        : RendezvousRole::responder;
}

RendezvousHandshakeMachine::RendezvousHandshakeMachine(
    Configuration configuration) noexcept
    : configuration_(configuration)
{
}

Handshake RendezvousHandshakeMachine::base_packet(
    HandshakeRequest request) const noexcept
{
    Handshake packet;
    packet.version = handshake_version_5;
    packet.encryption_field = configuration_.encryption_field;
    packet.initial_sequence = configuration_.initial_sequence;
    packet.maximum_transmission_unit =
        configuration_.maximum_transmission_unit;
    packet.flow_window = configuration_.flow_window;
    packet.request = request;
    packet.socket_id = configuration_.local_socket_id;
    packet.syn_cookie = configuration_.local_cookie;
    return packet;
}

HandshakeExtensionParameters
RendezvousHandshakeMachine::response_extension_parameters() const noexcept
{
    auto response = configuration_.extension_parameters;
    if (has_peer_extension_parameters_) {
        response.receiver_tsbpd_delay_milliseconds = std::max(
            response.receiver_tsbpd_delay_milliseconds,
            peer_extension_parameters_
                .sender_tsbpd_delay_milliseconds);
        response.sender_tsbpd_delay_milliseconds = std::max(
            response.sender_tsbpd_delay_milliseconds,
            peer_extension_parameters_
                .receiver_tsbpd_delay_milliseconds);
    }
    return response;
}

HandshakeAction RendezvousHandshakeMachine::wave_action() const noexcept
{
    return {
        .kind = HandshakeActionKind::send,
        .packet = base_packet(HandshakeRequest::wave_a_hand),
    };
}

HandshakeAction
RendezvousHandshakeMachine::conclusion_request_action() const noexcept
{
    auto packet = base_packet(HandshakeRequest::conclusion);
    packet.extension_field =
        1U | (configuration_.key_material_request.size != 0U
            ? 2U : 0U)
        | ((configuration_.congestion_controller
                    != CongestionController::live
                || !configuration_.stream_id.empty()
                || configuration_
                    .packet_filter_configuration.enabled)
            ? 4U : 0U);
    return {
        .kind = HandshakeActionKind::send,
        .packet = packet,
        .has_handshake_extension = true,
        .extension_type =
            HandshakeExtensionType::handshake_request,
        .extension_parameters =
            configuration_.extension_parameters,
        .has_key_material_extension =
            configuration_.key_material_request.size != 0U,
        .key_material_extension_type =
            HandshakeExtensionType::key_material_request,
        .key_material = configuration_.key_material_request,
        .has_stream_id_extension =
            !configuration_.stream_id.empty(),
        .stream_id = configuration_.stream_id,
        .has_congestion_extension =
            configuration_.congestion_controller
                != CongestionController::live,
        .congestion_controller =
            configuration_.congestion_controller,
        .has_packet_filter_extension =
            configuration_
                .packet_filter_configuration.enabled,
        .packet_filter_configuration =
            configuration_
                .packet_filter_configuration,
    };
}

HandshakeAction
RendezvousHandshakeMachine::conclusion_response_action() const noexcept
{
    auto packet = base_packet(HandshakeRequest::conclusion);
    const bool has_key_material =
        has_key_material_response_
        || has_peer_key_material_request_;
    packet.extension_field =
        1U | (has_key_material ? 2U : 0U)
        | ((configuration_.congestion_controller
                    != CongestionController::live
                || negotiated_packet_filter_.enabled)
            ? 4U : 0U);
    return {
        .kind = HandshakeActionKind::send,
        .packet = packet,
        .has_handshake_extension = true,
        .extension_type =
            HandshakeExtensionType::handshake_response,
        .extension_parameters = response_extension_parameters(),
        .has_key_material_extension =
            has_key_material,
        .key_material_extension_type =
            HandshakeExtensionType::key_material_response,
        .key_material = has_key_material_response_
            ? key_material_response_
            : peer_key_material_request_,
        .has_congestion_extension =
            configuration_.congestion_controller
                != CongestionController::live,
        .congestion_controller =
            configuration_.congestion_controller,
        .has_packet_filter_extension =
            negotiated_packet_filter_.enabled,
        .packet_filter_configuration =
            negotiated_packet_filter_,
    };
}

HandshakeAction
RendezvousHandshakeMachine::empty_conclusion_action() const noexcept
{
    return {
        .kind = HandshakeActionKind::send,
        .packet = base_packet(HandshakeRequest::conclusion),
    };
}

HandshakeAction
RendezvousHandshakeMachine::agreement_action() const noexcept
{
    auto packet = base_packet(HandshakeRequest::agreement);
    packet.extension_field = 0;
    return {
        .kind = HandshakeActionKind::send,
        .packet = packet,
    };
}

void RendezvousHandshakeMachine::arm_timer(
    HandshakeActions& actions) const noexcept
{
    actions.push({
        .kind = HandshakeActionKind::arm_timer,
        .timeout_milliseconds =
            configuration_.timeout_milliseconds,
    });
}

HandshakeActions
RendezvousHandshakeMachine::reject_cookie_collision() noexcept
{
    rejection_reason_ = rendezvous_cookie_rejection_reason;
    state_ = RendezvousState::rejected;
    HandshakeActions actions;
    actions.push({
        .kind = HandshakeActionKind::send,
        .packet = base_packet(static_cast<HandshakeRequest>(
            1'000 + rendezvous_cookie_rejection_reason)),
    });
    actions.push({
        .kind = HandshakeActionKind::rejected,
        .rejection_reason = rendezvous_cookie_rejection_reason,
    });
    return actions;
}

HandshakeActions
RendezvousHandshakeMachine::reject_message_api() noexcept
{
    rejection_reason_ = message_api_rejection_reason;
    state_ = RendezvousState::rejected;
    HandshakeActions actions;
    actions.push({
        .kind = HandshakeActionKind::send,
        .packet = base_packet(
            static_cast<HandshakeRequest>(
                1'000 + message_api_rejection_reason)),
    });
    actions.push({
        .kind = HandshakeActionKind::rejected,
        .rejection_reason =
            message_api_rejection_reason,
    });
    return actions;
}

HandshakeActions
RendezvousHandshakeMachine::reject_peer_version() noexcept
{
    rejection_reason_ = version_rejection_reason;
    state_ = RendezvousState::rejected;
    HandshakeActions actions;
    actions.push({
        .kind = HandshakeActionKind::send,
        .packet = base_packet(
            static_cast<HandshakeRequest>(
                1'000 + version_rejection_reason)),
    });
    actions.push({
        .kind = HandshakeActionKind::rejected,
        .rejection_reason = version_rejection_reason,
    });
    return actions;
}

HandshakeActions
RendezvousHandshakeMachine::reject_rogue_version() noexcept
{
    rejection_reason_ = rogue_rejection_reason;
    state_ = RendezvousState::rejected;
    HandshakeActions actions;
    actions.push({
        .kind = HandshakeActionKind::send,
        .packet = base_packet(
            static_cast<HandshakeRequest>(
                1'000 + rogue_rejection_reason)),
    });
    actions.push({
        .kind = HandshakeActionKind::rejected,
        .rejection_reason = rogue_rejection_reason,
    });
    return actions;
}

HandshakeActions
RendezvousHandshakeMachine::reject_congestion_controller() noexcept
{
    rejection_reason_ = congestion_rejection_reason;
    state_ = RendezvousState::rejected;
    HandshakeActions actions;
    actions.push({
        .kind = HandshakeActionKind::send,
        .packet = base_packet(
            static_cast<HandshakeRequest>(
                1'000 + congestion_rejection_reason)),
    });
    actions.push({
        .kind = HandshakeActionKind::rejected,
        .rejection_reason =
            congestion_rejection_reason,
    });
    return actions;
}

HandshakeActions
RendezvousHandshakeMachine::reject_packet_filter() noexcept
{
    rejection_reason_ = packet_filter_rejection_reason;
    state_ = RendezvousState::rejected;
    HandshakeActions actions;
    actions.push({
        .kind = HandshakeActionKind::send,
        .packet = base_packet(
            static_cast<HandshakeRequest>(
                1'000 + packet_filter_rejection_reason)),
    });
    actions.push({
        .kind = HandshakeActionKind::rejected,
        .rejection_reason =
            packet_filter_rejection_reason,
    });
    return actions;
}

HandshakeActions RendezvousHandshakeMachine::fail() noexcept
{
    state_ = RendezvousState::failed;
    HandshakeActions actions;
    actions.push({.kind = HandshakeActionKind::failed});
    return actions;
}

void RendezvousHandshakeMachine::remember_peer(
    const HandshakeMessage& message) noexcept
{
    peer_socket_id_ = message.packet.socket_id;
    peer_cookie_ = message.packet.syn_cookie;
    peer_flow_window_ = message.packet.flow_window;
    peer_encryption_field_ =
        message.packet.encryption_field;
    peer_initial_sequence_ =
        message.packet.initial_sequence;
    if (message.has_handshake_extension) {
        peer_extension_parameters_ =
            message.extension_parameters;
        has_peer_extension_parameters_ = true;
    }
    if (message.has_key_material_extension
        && message.key_material_extension_type
            == HandshakeExtensionType::key_material_request) {
        peer_key_material_request_ = message.key_material;
        has_peer_key_material_request_ = true;
    }
    if (message.has_stream_id_extension
        && message.has_handshake_extension
        && message.extension_type
            == HandshakeExtensionType::handshake_request) {
        peer_stream_id_ = message.stream_id;
        has_peer_stream_id_ = true;
    }
}

HandshakeActions RendezvousHandshakeMachine::start() noexcept
{
    if (state_ != RendezvousState::idle
        || configuration_.local_socket_id == 0U
        || configuration_.local_cookie == 0U
        || configuration_.maximum_transmission_unit < 76U
        || configuration_.flow_window == 0U
        || (configuration_.encryption_field != 0U
            && key_length_for_encryption_field(
                   configuration_.encryption_field)
                == 0U)) {
        return fail();
    }
    state_ = RendezvousState::waving;
    HandshakeActions actions;
    actions.push(wave_action());
    arm_timer(actions);
    return actions;
}

Error RendezvousHandshakeMachine::set_key_material_request(
    const KeyMaterialBuffer& request,
    std::uint16_t encryption_field) noexcept
{
    const bool clearing =
        request.size == 0U && encryption_field == 0U;
    const bool active_state =
        state_ == RendezvousState::waving
        || state_ == RendezvousState::attention
        || state_ == RendezvousState::fine
        || state_ == RendezvousState::initiated;
    if (!active_state
        || (!clearing
            && role_ == RendezvousRole::responder)
        || (request.size != 0U
            && key_length_for_encryption_field(
                   encryption_field)
                == 0U)) {
        return Error::invalid_state;
    }
    configuration_.key_material_request = request;
    configuration_.encryption_field = encryption_field;
    const auto crypt_flag =
        static_cast<std::uint32_t>(
            HandshakeExtensionFlag::crypt);
    if (clearing) {
        configuration_.extension_parameters.flags &=
            ~crypt_flag;
    } else if (request.size != 0U) {
        configuration_.extension_parameters.flags |=
            crypt_flag;
    }
    return Error::none;
}

Error RendezvousHandshakeMachine::set_key_material_response(
    const KeyMaterialBuffer& response) noexcept
{
    const bool active_state =
        state_ == RendezvousState::waving
        || state_ == RendezvousState::attention
        || state_ == RendezvousState::fine
        || state_ == RendezvousState::initiated;
    if (!active_state
        || response.size != 4U
        || role_ == RendezvousRole::initiator) {
        return Error::invalid_state;
    }
    key_material_response_ = response;
    has_key_material_response_ = true;
    return Error::none;
}

HandshakeActions RendezvousHandshakeMachine::receive(
    const Handshake& packet) noexcept
{
    return receive(HandshakeMessage{.packet = packet});
}

HandshakeActions RendezvousHandshakeMachine::receive(
    const HandshakeMessage& message) noexcept
{
    const auto& incoming = message.packet;
    if (is_rejection(incoming.request)) {
        rejection_reason_ =
            static_cast<std::int32_t>(incoming.request) - 1'000;
        state_ = RendezvousState::rejected;
        HandshakeActions actions;
        actions.push({
            .kind = HandshakeActionKind::rejected,
            .rejection_reason = rejection_reason_,
        });
        return actions;
    }
    if (state_ == RendezvousState::idle
        || state_ == RendezvousState::rejected
        || state_ == RendezvousState::failed
        || incoming.socket_id == 0U
        || incoming.syn_cookie == 0U) {
        return fail();
    }
    if (peer_cookie_ != 0U
        && (incoming.syn_cookie != peer_cookie_
            || incoming.socket_id != peer_socket_id_
            || incoming.flow_window != peer_flow_window_)) {
        return fail();
    }
    if (state_ == RendezvousState::connected
        && incoming.version != handshake_version_5) {
        return {};
    }
    if (incoming.version != handshake_version_5) {
        return reject_peer_version();
    }

    const bool request = has_handshake_request(message);
    const bool response = has_handshake_response(message);
    if (request || response) {
        const auto version_status = detail::classify_hsv5_peer_version(
            message.extension_parameters.srt_version,
            configuration_.minimum_peer_srt_version);
        if (version_status == detail::Hsv5PeerVersionStatus::pre_hsv5) {
            return reject_rogue_version();
        }
        if (version_status
            == detail::Hsv5PeerVersionStatus::below_configured_minimum) {
            return reject_peer_version();
        }
    }
    auto resolved_role = role_;
    if (resolved_role == RendezvousRole::unresolved) {
        resolved_role = resolve_rendezvous_role(
            configuration_.local_cookie,
            incoming.syn_cookie);
        if (resolved_role == RendezvousRole::unresolved) {
            return reject_cookie_collision();
        }
    }

    HandshakeActions actions;
    // The responder validates the initiator's transmission mode before
    // producing HSRSP. A successful response is authoritative and does not
    // need to echo the accepted STREAM flag.
    if (request
        && !compatible_transmission_mode(
            configuration_.extension_parameters,
            message.extension_parameters)) {
        return reject_message_api();
    }
    if (request) {
        const CongestionController peer_controller =
            message.has_congestion_extension
            ? message.congestion_controller
            : CongestionController::live;
        if (peer_controller
            != configuration_.congestion_controller) {
            return reject_congestion_controller();
        }
    } else if (response
        && message.has_congestion_extension
        && message.congestion_controller
            != configuration_.congestion_controller) {
        // CONFIG is a proposal validated by the responder. Its echo is
        // optional, but an explicit response value must still agree.
        return reject_congestion_controller();
    }
    if (request) {
        if (message.has_packet_filter_extension) {
            const auto filter =
                negotiate_packet_filter_configuration(
                    configuration_
                        .packet_filter_configuration,
                    message.packet_filter_configuration,
                    true);
            if (!filter) {
                return reject_packet_filter();
            }
            negotiated_packet_filter_ =
                filter.configuration;
        } else {
            negotiated_packet_filter_ = {};
        }
    } else if (response) {
        if (message.has_packet_filter_extension) {
            const auto filter =
                negotiate_packet_filter_configuration(
                    configuration_
                        .packet_filter_configuration,
                    message.packet_filter_configuration,
                    true);
            if (!filter) {
                return reject_packet_filter();
            }
            negotiated_packet_filter_ =
                filter.configuration;
        } else if (configuration_
                .packet_filter_configuration.enabled) {
            return reject_packet_filter();
        }
    }

    role_ = resolved_role;
    remember_peer(message);
    retry_count_ = 0;

    switch (state_) {
    case RendezvousState::waving:
        if (incoming.request == HandshakeRequest::wave_a_hand) {
            state_ = RendezvousState::attention;
            actions.push(role_ == RendezvousRole::initiator
                    ? conclusion_request_action()
                    : empty_conclusion_action());
            arm_timer(actions);
            return actions;
        }
        if (incoming.request == HandshakeRequest::conclusion) {
            state_ = RendezvousState::fine;
            if (role_ == RendezvousRole::initiator) {
                actions.push(conclusion_request_action());
            } else if (request) {
                actions.push(conclusion_response_action());
            } else {
                state_ = RendezvousState::attention;
                actions.push(empty_conclusion_action());
            }
            arm_timer(actions);
            return actions;
        }
        break;

    case RendezvousState::attention:
        if (incoming.request == HandshakeRequest::wave_a_hand) {
            actions.push(role_ == RendezvousRole::initiator
                    ? conclusion_request_action()
                    : empty_conclusion_action());
            arm_timer(actions);
            return actions;
        }
        if (incoming.request == HandshakeRequest::conclusion) {
            if (role_ == RendezvousRole::initiator) {
                if (response) {
                    state_ = RendezvousState::connected;
                    actions.push(agreement_action());
                    actions.push({
                        .kind = HandshakeActionKind::connected,
                    });
                } else {
                    state_ = RendezvousState::initiated;
                    actions.push(conclusion_request_action());
                    arm_timer(actions);
                }
                return actions;
            }
            if (request) {
                state_ = RendezvousState::initiated;
                actions.push(conclusion_response_action());
            } else {
                actions.push(empty_conclusion_action());
            }
            arm_timer(actions);
            return actions;
        }
        break;

    case RendezvousState::fine:
        if (role_ == RendezvousRole::initiator
            && incoming.request == HandshakeRequest::conclusion) {
            if (response) {
                state_ = RendezvousState::connected;
                actions.push(agreement_action());
                actions.push({
                    .kind = HandshakeActionKind::connected,
                });
            } else {
                actions.push(conclusion_request_action());
                arm_timer(actions);
            }
            return actions;
        }
        if (role_ == RendezvousRole::responder) {
            if (incoming.request == HandshakeRequest::agreement) {
                state_ = RendezvousState::connected;
                actions.push({
                    .kind = HandshakeActionKind::connected,
                });
                return actions;
            }
            if (incoming.request == HandshakeRequest::conclusion
                && request) {
                actions.push(conclusion_response_action());
                arm_timer(actions);
                return actions;
            }
        }
        break;

    case RendezvousState::initiated:
        if (role_ == RendezvousRole::initiator
            && incoming.request == HandshakeRequest::conclusion) {
            if (response) {
                state_ = RendezvousState::connected;
                actions.push(agreement_action());
                actions.push({
                    .kind = HandshakeActionKind::connected,
                });
            } else {
                actions.push(conclusion_request_action());
                arm_timer(actions);
            }
            return actions;
        }
        if (role_ == RendezvousRole::responder) {
            if (incoming.request == HandshakeRequest::agreement) {
                state_ = RendezvousState::connected;
                actions.push(agreement_action());
                actions.push({
                    .kind = HandshakeActionKind::connected,
                });
                return actions;
            }
            if (incoming.request == HandshakeRequest::conclusion
                && request) {
                actions.push(conclusion_response_action());
                arm_timer(actions);
                return actions;
            }
        }
        break;

    case RendezvousState::connected:
        if (incoming.request == HandshakeRequest::conclusion) {
            if (role_ == RendezvousRole::initiator) {
                actions.push(agreement_action());
            } else if (request) {
                actions.push(conclusion_response_action());
            }
            return actions;
        }
        if (incoming.request == HandshakeRequest::agreement) {
            return actions;
        }
        break;

    case RendezvousState::idle:
    case RendezvousState::rejected:
    case RendezvousState::failed:
        break;
    }
    return fail();
}

HandshakeActions
RendezvousHandshakeMachine::note_connected_packet() noexcept
{
    HandshakeActions actions;
    if ((state_ == RendezvousState::initiated
            || state_ == RendezvousState::fine)
        && role_ == RendezvousRole::responder) {
        state_ = RendezvousState::connected;
        actions.push(agreement_action());
        actions.push({
            .kind = HandshakeActionKind::connected,
        });
        return actions;
    }
    if (state_ != RendezvousState::connected) {
        return fail();
    }
    return actions;
}

HandshakeActions RendezvousHandshakeMachine::timeout() noexcept
{
    if (state_ == RendezvousState::connected
        || state_ == RendezvousState::rejected) {
        return {};
    }
    if (state_ == RendezvousState::idle
        || state_ == RendezvousState::failed
        || retry_count_ >= configuration_.maximum_retries) {
        return fail();
    }
    ++retry_count_;

    HandshakeActions actions;
    switch (state_) {
    case RendezvousState::waving:
        actions.push(wave_action());
        break;
    case RendezvousState::attention:
        actions.push(role_ == RendezvousRole::initiator
                ? conclusion_request_action()
                : empty_conclusion_action());
        break;
    case RendezvousState::fine:
    case RendezvousState::initiated:
        actions.push(role_ == RendezvousRole::initiator
                ? conclusion_request_action()
                : conclusion_response_action());
        break;
    case RendezvousState::idle:
    case RendezvousState::connected:
    case RendezvousState::rejected:
    case RendezvousState::failed:
        return fail();
    }
    arm_timer(actions);
    return actions;
}

} // namespace robotweax::srt
