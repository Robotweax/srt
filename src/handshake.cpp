#include "robotweax/srt/handshake.hpp"

#include "hsv5_version_policy.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace robotweax::srt {
namespace {

constexpr int packet_filter_rejection_reason = 14;
constexpr int message_api_rejection_reason = 12;
constexpr int congestion_rejection_reason = 13;
constexpr int group_rejection_reason = 15;
constexpr int version_rejection_reason = 8;
constexpr int rogue_rejection_reason = 4;

[[nodiscard]] std::uint32_t read_u32(const std::byte* bytes) noexcept
{
    return (std::to_integer<std::uint32_t>(bytes[0]) << 24U)
        | (std::to_integer<std::uint32_t>(bytes[1]) << 16U)
        | (std::to_integer<std::uint32_t>(bytes[2]) << 8U)
        | std::to_integer<std::uint32_t>(bytes[3]);
}

void write_u32(std::byte* bytes, std::uint32_t value) noexcept
{
    bytes[0] = static_cast<std::byte>((value >> 24U) & 0xffU);
    bytes[1] = static_cast<std::byte>((value >> 16U) & 0xffU);
    bytes[2] = static_cast<std::byte>((value >> 8U) & 0xffU);
    bytes[3] = static_cast<std::byte>(value & 0xffU);
}

[[nodiscard]] bool valid_request(std::int32_t request) noexcept
{
    return request == static_cast<std::int32_t>(HandshakeRequest::induction)
        || request == static_cast<std::int32_t>(HandshakeRequest::wave_a_hand)
        || request == static_cast<std::int32_t>(HandshakeRequest::conclusion)
        || request == static_cast<std::int32_t>(HandshakeRequest::agreement)
        || request >= 1'000;
}

[[nodiscard]] constexpr bool valid_initial_sequence(
    std::uint32_t value) noexcept
{
    return value <= SequenceNumber::mask;
}

[[nodiscard]] constexpr bool valid_flow_window(std::uint32_t value) noexcept
{
    return value >= 2U
        && value
        <= static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max());
}

} // namespace

HandshakeDecodeResult decode_handshake(std::span<const std::byte> payload) noexcept
{
    const auto decoded = decode_handshake_payload(payload);
    if (!decoded) {
        return {.error = decoded.error};
    }
    return {.handshake = decoded.handshake};
}

HandshakePayloadDecodeResult decode_handshake_payload(
    std::span<const std::byte> payload) noexcept
{
    if (payload.size() < handshake_size) {
        return {.error = Error::invalid_handshake_size};
    }

    Handshake packet;
    packet.version = read_u32(payload.data());
    const std::uint32_t fields = read_u32(payload.data() + 4);
    packet.encryption_field = static_cast<std::uint16_t>(fields >> 16U);
    packet.extension_field = static_cast<std::uint16_t>(fields & 0xffffU);
    const std::uint32_t initial_sequence = read_u32(payload.data() + 8);
    packet.initial_sequence = SequenceNumber {initial_sequence};
    packet.maximum_transmission_unit = read_u32(payload.data() + 12);
    packet.flow_window = read_u32(payload.data() + 16);
    const std::int32_t request = static_cast<std::int32_t>(read_u32(payload.data() + 20));
    packet.request = static_cast<HandshakeRequest>(request);
    packet.socket_id = read_u32(payload.data() + 24);
    packet.syn_cookie = read_u32(payload.data() + 28);
    for (std::size_t index = 0; index < packet.peer_address.size(); ++index) {
        packet.peer_address[index] = read_u32(payload.data() + 32 + index * 4);
    }

    if ((packet.version != handshake_version_4
            && packet.version != handshake_version_5)
        || !valid_initial_sequence(initial_sequence)
        || packet.maximum_transmission_unit < 76U
        || !valid_flow_window(packet.flow_window) || !valid_request(request)) {
        return {.error = Error::invalid_handshake_value};
    }
    return {
        .handshake = packet,
        .extensions = payload.subspan(handshake_size),
    };
}

HandshakeEncodeResult encode_handshake_payload(
    const Handshake& handshake,
    std::span<const std::byte> extensions,
    std::span<std::byte> destination) noexcept
{
    const std::size_t required = handshake_size + extensions.size();
    if (destination.size() < required) {
        return {.error = Error::buffer_too_small};
    }
    const auto error = encode_handshake(handshake, destination.first(handshake_size));
    if (error != Error::none) {
        return {.error = error};
    }
    std::copy(extensions.begin(), extensions.end(), destination.begin() + handshake_size);
    return {.bytes_written = required};
}

Error encode_handshake(const Handshake& packet, std::span<std::byte> destination) noexcept
{
    if (destination.size() < handshake_size) {
        return Error::buffer_too_small;
    }
    const auto request = static_cast<std::int32_t>(packet.request);
    if ((packet.version != handshake_version_4
            && packet.version != handshake_version_5)
        || !valid_initial_sequence(packet.initial_sequence.value())
        || packet.maximum_transmission_unit < 76U
        || !valid_flow_window(packet.flow_window) || !valid_request(request)) {
        return Error::invalid_handshake_value;
    }

    write_u32(destination.data(), packet.version);
    write_u32(destination.data() + 4,
        (static_cast<std::uint32_t>(packet.encryption_field) << 16U)
            | packet.extension_field);
    write_u32(destination.data() + 8, packet.initial_sequence.value());
    write_u32(destination.data() + 12, packet.maximum_transmission_unit);
    write_u32(destination.data() + 16, packet.flow_window);
    write_u32(destination.data() + 20, static_cast<std::uint32_t>(request));
    write_u32(destination.data() + 24, packet.socket_id);
    write_u32(destination.data() + 28, packet.syn_cookie);
    for (std::size_t index = 0; index < packet.peer_address.size(); ++index) {
        write_u32(destination.data() + 32 + index * 4, packet.peer_address[index]);
    }
    return Error::none;
}

HandshakeMachine::HandshakeMachine(Configuration configuration) noexcept
    : configuration_(configuration)
    , negotiated_initial_sequence_(configuration.initial_sequence)
{
}

Handshake HandshakeMachine::base_packet(HandshakeRequest request) const noexcept
{
    Handshake packet;
    packet.version = request == HandshakeRequest::induction
        ? handshake_version_4
        : handshake_version_5;
    packet.initial_sequence = configuration_.initial_sequence;
    packet.maximum_transmission_unit = configuration_.maximum_transmission_unit;
    packet.flow_window = configuration_.flow_window;
    packet.request = request;
    packet.socket_id = configuration_.local_socket_id;
    if (request != HandshakeRequest::induction) {
        packet.encryption_field =
            configuration_.encryption_field;
    }
    return packet;
}

HandshakeExtensionParameters HandshakeMachine::response_extension_parameters() const noexcept
{
    auto response = configuration_.extension_parameters;
    if (has_peer_extension_parameters_) {
        response.receiver_tsbpd_delay_milliseconds = std::max(
            response.receiver_tsbpd_delay_milliseconds,
            peer_extension_parameters_.sender_tsbpd_delay_milliseconds);
        response.sender_tsbpd_delay_milliseconds = std::max(
            response.sender_tsbpd_delay_milliseconds,
            peer_extension_parameters_.receiver_tsbpd_delay_milliseconds);
    }
    return response;
}

HandshakeActions HandshakeMachine::reject_locally(
    int rejection_reason) noexcept
{
    rejection_reason_ = rejection_reason;
    state_ = HandshakeState::rejected;
    HandshakeActions actions;
    actions.push({
        .kind = HandshakeActionKind::rejected,
        .rejection_reason = rejection_reason,
    });
    return actions;
}

HandshakeActions HandshakeMachine::start() noexcept
{
    HandshakeActions actions;
    if (state_ != HandshakeState::idle || configuration_.role != ConnectionRole::caller) {
        actions.push({.kind = HandshakeActionKind::failed});
        state_ = HandshakeState::failed;
        return actions;
    }
    state_ = HandshakeState::awaiting_induction_response;
    actions.push({.kind = HandshakeActionKind::send, .packet = base_packet(HandshakeRequest::induction)});
    actions.push({.kind = HandshakeActionKind::arm_timer,
        .timeout_milliseconds = configuration_.timeout_milliseconds});
    return actions;
}

Error HandshakeMachine::set_key_material_request(
    const KeyMaterialBuffer& request,
    std::uint16_t encryption_field) noexcept
{
    if (configuration_.role != ConnectionRole::caller
        || state_ != HandshakeState::awaiting_induction_response
        || (request.size != 0U
            && key_length_for_encryption_field(
                   encryption_field)
                == 0U)) {
        return Error::invalid_state;
    }
    configuration_.key_material_request = request;
    configuration_.encryption_field = encryption_field;
    return Error::none;
}

HandshakeActions HandshakeMachine::reject(
    int rejection_reason) noexcept
{
    HandshakeActions actions;
    const std::int64_t wire_reason =
        1'000LL + static_cast<std::int64_t>(rejection_reason);
    if (configuration_.role != ConnectionRole::listener
        || state_ != HandshakeState::awaiting_conclusion_response
        || rejection_reason < 0
        || wire_reason
            > std::numeric_limits<std::int32_t>::max()) {
        state_ = HandshakeState::failed;
        actions.push({.kind = HandshakeActionKind::failed});
        return actions;
    }
    auto rejection = base_packet(static_cast<HandshakeRequest>(
        static_cast<std::int32_t>(wire_reason)));
    rejection.version = handshake_version_5;
    rejection.initial_sequence = negotiated_initial_sequence_;
    rejection.syn_cookie = cookie_;
    rejection_reason_ = rejection_reason;
    state_ = HandshakeState::rejected;
    actions.push({
        .kind = HandshakeActionKind::send,
        .packet = rejection,
    });
    actions.push({
        .kind = HandshakeActionKind::rejected,
        .rejection_reason = rejection_reason,
    });
    return actions;
}

HandshakeActions HandshakeMachine::receive(const Handshake& incoming) noexcept
{
    return receive(HandshakeMessage{.packet = incoming});
}

bool HandshakeMachine::is_stale_induction(
    const HandshakeMessage& message) const noexcept
{
    return configuration_.role == ConnectionRole::caller
        && state_ == HandshakeState::awaiting_conclusion_response
        && message.packet.request == HandshakeRequest::induction;
}

HandshakeActions HandshakeMachine::receive(const HandshakeMessage& message) noexcept
{
    HandshakeActions actions;
    const auto& incoming = message.packet;
    if (is_stale_induction(message)) {
        return actions;
    }
    const auto request_value = static_cast<std::int32_t>(incoming.request);
    if (request_value >= 1'000) {
        rejection_reason_ = request_value - 1'000;
        state_ = HandshakeState::rejected;
        actions.push({
            .kind = HandshakeActionKind::rejected,
            .rejection_reason = rejection_reason_,
        });
        return actions;
    }

    if (configuration_.role == ConnectionRole::caller
        && state_ == HandshakeState::connected) {
        return actions;
    }

    if (configuration_.role == ConnectionRole::listener) {
        if (state_ == HandshakeState::idle && incoming.request == HandshakeRequest::induction) {
            if (incoming.version != handshake_version_4) {
                state_ = HandshakeState::failed;
                actions.push({.kind = HandshakeActionKind::failed});
                return actions;
            }
            if (configuration_.cookie_generator == nullptr) {
                state_ = HandshakeState::failed;
                actions.push({.kind = HandshakeActionKind::failed});
                return actions;
            }
            peer_socket_id_ = incoming.socket_id;
            retry_count_ = 0;
            negotiated_initial_sequence_ = incoming.initial_sequence;
            cookie_ = configuration_.cookie_generator(incoming, configuration_.cookie_context);
            if (cookie_ == 0U) {
                state_ = HandshakeState::failed;
                actions.push({.kind = HandshakeActionKind::failed});
                return actions;
            }
            auto response = base_packet(HandshakeRequest::induction);
            response.version = handshake_version_5;
            response.encryption_field =
                configuration_.encryption_field;
            response.extension_field = handshake_srt_magic;
            response.initial_sequence = negotiated_initial_sequence_;
            response.syn_cookie = cookie_;
            response.encryption_field =
                configuration_.encryption_field != 0U
                ? configuration_.encryption_field
                : incoming.encryption_field;
            state_ = HandshakeState::awaiting_conclusion_response;
            actions.push({.kind = HandshakeActionKind::send, .packet = response});
            actions.push({.kind = HandshakeActionKind::arm_timer,
                .timeout_milliseconds = configuration_.timeout_milliseconds});
            return actions;
        }
        if (state_ == HandshakeState::awaiting_conclusion_response
            && incoming.request == HandshakeRequest::conclusion
            && cookie_ != 0U
            && incoming.syn_cookie == cookie_) {
            if (incoming.version != handshake_version_5) {
                return reject(version_rejection_reason);
            }
            const HandshakeExtensionParameters peer_parameters =
                message.has_handshake_extension
                ? message.extension_parameters
                : HandshakeExtensionParameters{};
            if (message.has_handshake_extension) {
                const auto version_status = detail::classify_hsv5_peer_version(
                    peer_parameters.srt_version,
                    configuration_.minimum_peer_srt_version);
                if (version_status == detail::Hsv5PeerVersionStatus::pre_hsv5) {
                    return reject(rogue_rejection_reason);
                }
                if (version_status
                    == detail::Hsv5PeerVersionStatus::
                        below_configured_minimum) {
                    return reject(version_rejection_reason);
                }
            }
            if (!compatible_transmission_mode(
                    configuration_.extension_parameters,
                    peer_parameters)) {
                return reject(
                    message_api_rejection_reason);
            }
            const CongestionController peer_controller =
                message.has_congestion_extension
                ? message.congestion_controller
                : CongestionController::live;
            if (peer_controller
                != configuration_.congestion_controller) {
                return reject(
                    congestion_rejection_reason);
            }
            const bool negotiate_filter =
                message.has_packet_filter_extension;
            if (negotiate_filter) {
                const auto filter =
                    negotiate_packet_filter_configuration(
                        configuration_
                            .packet_filter_configuration,
                        message
                            .packet_filter_configuration);
                if (!filter) {
                    return reject(
                        packet_filter_rejection_reason);
                }
                negotiated_packet_filter_ =
                    filter.configuration;
            }
            if (message.has_stream_id_extension
                && message.has_handshake_extension
                && message.extension_type
                    == HandshakeExtensionType::handshake_request) {
                peer_stream_id_ = message.stream_id;
                has_peer_stream_id_ = true;
            } else {
                peer_stream_id_ = {};
                has_peer_stream_id_ = false;
            }
            if (message.has_group_membership) {
                if (validate_group_membership(
                        message.group_membership)
                        != GroupMembershipValidation::compatible
                    || configuration_
                            .group_membership_negotiator == nullptr) {
                    return reject(group_rejection_reason);
                }
                GroupMembership local{};
                if (!configuration_.group_membership_negotiator(
                        message.group_membership, local,
                        configuration_.group_membership_context)
                    || validate_group_membership(local)
                        != GroupMembershipValidation::compatible
                    || local.type != message.group_membership.type) {
                    return reject(group_rejection_reason);
                }
                peer_group_membership_ =
                    message.group_membership;
                has_peer_group_membership_ = true;
                local_group_response_ = local;
                has_local_group_response_ = true;
            } else {
                peer_group_membership_ = {};
                has_peer_group_membership_ = false;
                local_group_response_ = {};
                has_local_group_response_ = false;
            }
            if (message.has_handshake_extension) {
                peer_extension_parameters_ =
                    message.extension_parameters;
                has_peer_extension_parameters_ = true;
            }
            peer_socket_id_ = incoming.socket_id;
            peer_flow_window_ = incoming.flow_window;
            retry_count_ = 0;
            auto response = base_packet(HandshakeRequest::conclusion);
            response.initial_sequence = negotiated_initial_sequence_;
            response.syn_cookie = cookie_;
            response.encryption_field =
                configuration_.encryption_field != 0U
                ? configuration_.encryption_field
                : incoming.encryption_field;
            if (message.has_handshake_extension
                && message.extension_type == HandshakeExtensionType::handshake_request) {
                response.extension_field |= 1U;
            }
            if (message.has_key_material_extension
                && message.key_material_extension_type
                    == HandshakeExtensionType::key_material_request) {
                response.extension_field |= 2U;
            }
            const bool has_congestion_extension =
                configuration_.congestion_controller
                    != CongestionController::live;
            if (has_congestion_extension
                || negotiated_packet_filter_.enabled
                || has_local_group_response_) {
                response.extension_field |= 4U;
            }
            state_ = HandshakeState::connected;
            actions.push({
                .kind = HandshakeActionKind::send,
                .packet = response,
                .has_handshake_extension = message.has_handshake_extension,
                .extension_type = HandshakeExtensionType::handshake_response,
                .extension_parameters = response_extension_parameters(),
                .has_key_material_extension =
                    message.has_key_material_extension,
                .key_material_extension_type =
                    HandshakeExtensionType::key_material_response,
                .key_material = message.key_material,
                .has_congestion_extension =
                    has_congestion_extension,
                .congestion_controller =
                    configuration_.congestion_controller,
                .has_packet_filter_extension =
                    negotiated_packet_filter_.enabled,
                .packet_filter_configuration =
                    negotiated_packet_filter_,
                .has_group_membership =
                    has_local_group_response_,
                .group_membership = local_group_response_,
            });
            actions.push({.kind = HandshakeActionKind::connected});
            return actions;
        }
        if (state_ == HandshakeState::connected
            && incoming.request == HandshakeRequest::conclusion
            && incoming.syn_cookie == cookie_) {
            if (incoming.version != handshake_version_5) {
                return actions;
            }
            auto response = base_packet(HandshakeRequest::conclusion);
            response.initial_sequence = negotiated_initial_sequence_;
            response.syn_cookie = cookie_;
            response.extension_field =
                (message.has_handshake_extension ? 1U : 0U)
                | (message.has_key_material_extension ? 2U : 0U)
                | ((configuration_.congestion_controller
                            != CongestionController::live
                        || negotiated_packet_filter_.enabled
                        || has_local_group_response_)
                    ? 4U : 0U);
            actions.push({
                .kind = HandshakeActionKind::send,
                .packet = response,
                .has_handshake_extension = message.has_handshake_extension,
                .extension_type = HandshakeExtensionType::handshake_response,
                .extension_parameters = response_extension_parameters(),
                .has_key_material_extension =
                    message.has_key_material_extension,
                .key_material_extension_type =
                    HandshakeExtensionType::key_material_response,
                .key_material = message.key_material,
                .has_congestion_extension =
                    configuration_.congestion_controller
                        != CongestionController::live,
                .congestion_controller =
                    configuration_.congestion_controller,
                .has_packet_filter_extension =
                    negotiated_packet_filter_.enabled,
                .packet_filter_configuration =
                    negotiated_packet_filter_,
                .has_group_membership =
                    has_local_group_response_,
                .group_membership = local_group_response_,
            });
            return actions;
        }
    } else {
        if (state_ == HandshakeState::awaiting_induction_response
            && incoming.request == HandshakeRequest::induction
            && incoming.syn_cookie != 0U) {
            if (incoming.version != handshake_version_5) {
                return reject_locally(version_rejection_reason);
            }
            if (incoming.extension_field != handshake_srt_magic) {
                return reject_locally(rogue_rejection_reason);
            }
            peer_socket_id_ = incoming.socket_id;
            cookie_ = incoming.syn_cookie;
            retry_count_ = 0;
            auto conclusion = base_packet(HandshakeRequest::conclusion);
            conclusion.syn_cookie = cookie_;
            conclusion.extension_field =
                1U | (configuration_.key_material_request.size != 0U
                    ? 2U : 0U)
                | ((configuration_.congestion_controller
                            != CongestionController::live
                        || !configuration_.stream_id.empty()
                        || configuration_
                            .packet_filter_configuration.enabled
                        || configuration_.has_group_membership)
                    ? 4U : 0U);
            state_ = HandshakeState::awaiting_conclusion_response;
            actions.push({
                .kind = HandshakeActionKind::send,
                .packet = conclusion,
                .has_handshake_extension = true,
                .extension_type = HandshakeExtensionType::handshake_request,
                .extension_parameters = configuration_.extension_parameters,
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
                .has_group_membership =
                    configuration_.has_group_membership,
                .group_membership =
                    configuration_.group_membership,
            });
            actions.push({.kind = HandshakeActionKind::arm_timer,
                .timeout_milliseconds = configuration_.timeout_milliseconds});
            return actions;
        }
        if (state_ == HandshakeState::awaiting_conclusion_response
            && incoming.request == HandshakeRequest::conclusion
            && incoming.syn_cookie == cookie_) {
            if (incoming.version != handshake_version_5) {
                return reject_locally(version_rejection_reason);
            }
            if (message.has_handshake_extension
                && message.extension_type
                    != HandshakeExtensionType::handshake_response) {
                return actions;
            }
            if (message.has_handshake_extension) {
                const auto version_status = detail::classify_hsv5_peer_version(
                    message.extension_parameters.srt_version,
                    configuration_.minimum_peer_srt_version);
                if (version_status == detail::Hsv5PeerVersionStatus::pre_hsv5) {
                    return reject_locally(rogue_rejection_reason);
                }
                if (version_status
                    == detail::Hsv5PeerVersionStatus::
                        below_configured_minimum) {
                    return reject_locally(version_rejection_reason);
                }
            }
            // The listener validates the caller's transmission mode and
            // returns rejection reason 12 if it cannot accept it. A
            // successful CONCLUSION is authoritative; HSRSP reports the
            // responder's parameters and is not a second negotiation round.
            // CONFIG extensions are proposals validated by the responder.
            // A successful CONCLUSION need not echo the accepted controller.
            // If it does include one, it must still match the proposal.
            if (message.has_congestion_extension
                && message.congestion_controller
                    != configuration_.congestion_controller) {
                rejection_reason_ =
                    congestion_rejection_reason;
                state_ = HandshakeState::rejected;
                actions.push({
                    .kind =
                        HandshakeActionKind::rejected,
                    .rejection_reason =
                        congestion_rejection_reason,
                });
                return actions;
            }
            if (message.has_packet_filter_extension) {
                const auto filter =
                    apply_packet_filter_response(
                        configuration_
                            .packet_filter_configuration,
                        message
                            .packet_filter_configuration);
                if (!filter) {
                    rejection_reason_ =
                        packet_filter_rejection_reason;
                    state_ = HandshakeState::rejected;
                    actions.push({
                        .kind =
                            HandshakeActionKind::rejected,
                        .rejection_reason =
                            packet_filter_rejection_reason,
                    });
                    return actions;
                }
                negotiated_packet_filter_ =
                    filter.configuration;
            } else if (configuration_
                    .packet_filter_configuration.enabled) {
                rejection_reason_ =
                    packet_filter_rejection_reason;
                state_ = HandshakeState::rejected;
                actions.push({
                    .kind =
                        HandshakeActionKind::rejected,
                    .rejection_reason =
                        packet_filter_rejection_reason,
                });
                return actions;
            }
            if (configuration_.has_group_membership) {
                if (!message.has_group_membership
                    || validate_group_membership(
                        message.group_membership)
                        != GroupMembershipValidation::compatible
                    || message.group_membership.type
                        != configuration_.group_membership.type
                    || message.group_membership.flags
                        != configuration_.group_membership.flags) {
                    rejection_reason_ = group_rejection_reason;
                    state_ = HandshakeState::rejected;
                    actions.push({
                        .kind = HandshakeActionKind::rejected,
                        .rejection_reason = group_rejection_reason,
                    });
                    return actions;
                }
                peer_group_membership_ =
                    message.group_membership;
                has_peer_group_membership_ = true;
            } else if (message.has_group_membership) {
                rejection_reason_ = group_rejection_reason;
                state_ = HandshakeState::rejected;
                actions.push({
                    .kind = HandshakeActionKind::rejected,
                    .rejection_reason = group_rejection_reason,
                });
                return actions;
            }
            if (message.has_handshake_extension) {
                peer_extension_parameters_ =
                    message.extension_parameters;
                has_peer_extension_parameters_ = true;
            }
            peer_socket_id_ = incoming.socket_id;
            peer_flow_window_ = incoming.flow_window;
            retry_count_ = 0;
            state_ = HandshakeState::connected;
            actions.push({.kind = HandshakeActionKind::connected});
            return actions;
        }
    }

    actions.push({.kind = HandshakeActionKind::failed});
    state_ = HandshakeState::failed;
    return actions;
}

HandshakeActions HandshakeMachine::timeout() noexcept
{
    HandshakeActions actions;
    if (state_ == HandshakeState::connected || state_ == HandshakeState::rejected) {
        return actions;
    }
    if (retry_count_ >= configuration_.maximum_retries) {
        state_ = HandshakeState::failed;
        actions.push({.kind = HandshakeActionKind::failed});
        return actions;
    }
    ++retry_count_;

    if (configuration_.role == ConnectionRole::caller
        && state_ == HandshakeState::awaiting_induction_response) {
        actions.push({
            .kind = HandshakeActionKind::send,
            .packet = base_packet(HandshakeRequest::induction),
        });
    } else if (configuration_.role == ConnectionRole::caller
        && state_ == HandshakeState::awaiting_conclusion_response) {
        auto conclusion = base_packet(HandshakeRequest::conclusion);
        conclusion.syn_cookie = cookie_;
        conclusion.extension_field =
            1U | (configuration_.key_material_request.size != 0U
                ? 2U : 0U)
            | ((configuration_.congestion_controller
                        != CongestionController::live
                    || !configuration_.stream_id.empty()
                    || configuration_
                        .packet_filter_configuration.enabled
                    || configuration_.has_group_membership)
                ? 4U : 0U);
        actions.push({
            .kind = HandshakeActionKind::send,
            .packet = conclusion,
            .has_handshake_extension = true,
            .extension_type = HandshakeExtensionType::handshake_request,
            .extension_parameters = configuration_.extension_parameters,
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
            .has_group_membership =
                configuration_.has_group_membership,
            .group_membership =
                configuration_.group_membership,
        });
    } else if (configuration_.role == ConnectionRole::listener
        && state_ == HandshakeState::awaiting_conclusion_response) {
        auto induction = base_packet(HandshakeRequest::induction);
        induction.version = handshake_version_5;
        induction.encryption_field =
            configuration_.encryption_field;
        induction.extension_field = handshake_srt_magic;
        induction.initial_sequence = negotiated_initial_sequence_;
        induction.syn_cookie = cookie_;
        actions.push({.kind = HandshakeActionKind::send, .packet = induction});
    } else {
        state_ = HandshakeState::failed;
        actions.push({.kind = HandshakeActionKind::failed});
        return actions;
    }
    actions.push({
        .kind = HandshakeActionKind::arm_timer,
        .timeout_milliseconds = configuration_.timeout_milliseconds,
    });
    return actions;
}

} // namespace robotweax::srt
