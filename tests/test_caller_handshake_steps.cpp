#include "test.hpp"

#include "compat/caller_handshake_steps.hpp"

#include <cstddef>
#include <cstdint>

using namespace robotweax::srt;
using namespace robotweax::srt::compat;

namespace {

[[nodiscard]] HandshakeMessage message_from(const HandshakeAction& action)
{
    return {
        .packet = action.packet,
        .has_handshake_extension = action.has_handshake_extension,
        .extension_type = action.extension_type,
        .extension_parameters = action.extension_parameters,
        .has_key_material_extension = action.has_key_material_extension,
        .key_material_extension_type = action.key_material_extension_type,
        .key_material = action.key_material,
        .has_stream_id_extension = action.has_stream_id_extension,
        .stream_id = action.stream_id,
        .has_congestion_extension = action.has_congestion_extension,
        .congestion_controller = action.congestion_controller,
        .has_packet_filter_extension = action.has_packet_filter_extension,
        .packet_filter_configuration = action.packet_filter_configuration,
        .has_group_membership = action.has_group_membership,
        .group_membership = action.group_membership,
    };
}

[[nodiscard]] std::uint32_t fixed_cookie(const Handshake&, void*) noexcept
{
    return 0x1020'3040U;
}

[[nodiscard]] const HandshakeAction& require_action(
    const CallerHandshakeStep& step, std::size_t index,
    HandshakeActionKind kind)
{
    REQUIRE(index < step.actions.size);
    REQUIRE_EQ(step.actions.values[index].kind, kind);
    return step.actions.values[index];
}

} // namespace

TEST(caller_handshake_steps_complete_hsv5_without_waiting_for_io)
{
    CallerHandshakeSteps caller {{
        .role = ConnectionRole::caller,
        .local_socket_id = 100U,
        .initial_sequence = SequenceNumber {10U},
        .flow_window = 111U,
    }};
    HandshakeMachine listener {{
        .role = ConnectionRole::listener,
        .local_socket_id = 200U,
        .initial_sequence = SequenceNumber {20U},
        .flow_window = 222U,
        .cookie_generator = fixed_cookie,
    }};

    const CallerHandshakeStep start = caller.start();
    REQUIRE_EQ(start.destination_socket_id, 0U);
    const auto& induction =
        require_action(start, 0U, HandshakeActionKind::send);
    REQUIRE_EQ(induction.packet.request, HandshakeRequest::induction);
    (void)require_action(start, 1U, HandshakeActionKind::arm_timer);

    const HandshakeActions induction_response =
        listener.receive(message_from(induction));
    REQUIRE_EQ(induction_response.values[0].kind, HandshakeActionKind::send);
    const CallerHandshakeStep conclusion =
        caller.receive(message_from(induction_response.values[0]));
    REQUIRE_EQ(caller.peer_socket_id(), 200U);
    REQUIRE_EQ(caller.destination_socket_id(), 200U);
    REQUIRE_EQ(conclusion.destination_socket_id, 200U);
    const auto& conclusion_packet =
        require_action(conclusion, 0U, HandshakeActionKind::send);
    REQUIRE_EQ(conclusion_packet.packet.request, HandshakeRequest::conclusion);

    const HandshakeActions conclusion_response =
        listener.receive(message_from(conclusion_packet));
    REQUIRE_EQ(conclusion_response.values[0].kind, HandshakeActionKind::send);
    const CallerHandshakeStep connected =
        caller.receive(message_from(conclusion_response.values[0]));
    (void)require_action(connected, 0U, HandshakeActionKind::connected);
    REQUIRE_EQ(caller.state(), HandshakeState::connected);
    REQUIRE_EQ(caller.protocol().peer_flow_window(), 222U);
}

TEST(caller_handshake_steps_preserve_reference_zero_route_and_retry_affinity)
{
    constexpr std::uint32_t caller_socket_id = 0x1020'3040U;
    CallerHandshakeSteps caller {{
        .role = ConnectionRole::caller,
        .local_socket_id = caller_socket_id,
        .initial_sequence = SequenceNumber {77U},
        .maximum_retries = 1U,
    }};
    HandshakeMachine listener {{
        .role = ConnectionRole::listener,
        .local_socket_id = 0xa0b0'c0d0U,
        .cookie_generator = fixed_cookie,
    }};

    const CallerHandshakeStep start = caller.start();
    const HandshakeActions induction_response =
        listener.receive(message_from(start.actions.values[0]));
    HandshakeMessage reference_response =
        message_from(induction_response.values[0]);
    reference_response.packet.socket_id = caller_socket_id;

    const CallerHandshakeStep conclusion = caller.receive(reference_response);
    REQUIRE_EQ(caller.peer_socket_id(), caller_socket_id);
    REQUIRE_EQ(caller.destination_socket_id(), 0U);
    REQUIRE_EQ(conclusion.destination_socket_id, 0U);
    (void)require_action(conclusion, 0U, HandshakeActionKind::send);

    const CallerHandshakeStep retry = caller.timeout();
    REQUIRE_EQ(retry.destination_socket_id, 0U);
    (void)require_action(retry, 0U, HandshakeActionKind::send);
    (void)require_action(retry, 1U, HandshakeActionKind::arm_timer);
    const CallerHandshakeStep exhausted = caller.timeout();
    REQUIRE_EQ(exhausted.destination_socket_id, 0U);
    (void)require_action(exhausted, 0U, HandshakeActionKind::failed);
    REQUIRE_EQ(caller.state(), HandshakeState::failed);
}

TEST(caller_handshake_steps_update_key_material_only_in_valid_phase)
{
    CallerHandshakeSteps caller {{
        .role = ConnectionRole::caller,
        .local_socket_id = 100U,
    }};
    KeyMaterialBuffer request;
    request.bytes[0] = std::byte {0x42};
    request.size = 1U;

    REQUIRE_EQ(
        caller.set_key_material_request(request, 2U), Error::invalid_state);
    (void)caller.start();
    REQUIRE_EQ(caller.set_key_material_request(request, 2U), Error::none);
    REQUIRE_EQ(
        caller.set_key_material_request(request, 9U), Error::invalid_state);
}

TEST(
    caller_handshake_steps_ignore_stale_induction_without_mutating_route_or_retry)
{
    for (unsigned variant = 0; variant < 5; ++variant) {
        CallerHandshakeSteps caller {{
            .role = ConnectionRole::caller,
            .local_socket_id = 100U,
            .maximum_retries = 1U,
        }};
        HandshakeMachine listener {{
            .role = ConnectionRole::listener,
            .local_socket_id = 200U,
            .cookie_generator = fixed_cookie,
        }};
        const auto start = caller.start();
        const auto reply =
            listener.receive(message_from(start.actions.values[0]));
        auto stale = message_from(reply.values[0]);
        const auto conclusion = caller.receive(stale);
        if (variant == 1) {
            stale.packet.socket_id = 999U;
        }
        if (variant == 2) {
            stale.packet.syn_cookie ^= 1U;
        }
        if (variant == 3) {
            stale.packet.encryption_field = 4U;
        }
        if (variant == 4) {
            stale.packet.version = 4U;
        }
        const auto retry = caller.timeout();
        REQUIRE_EQ(retry.actions.values[0].kind, HandshakeActionKind::send);
        for (unsigned duplicate = 0; duplicate < 20; ++duplicate) {
            REQUIRE(caller.protocol().is_stale_induction(stale));
            const auto ignored = caller.receive(stale);
            REQUIRE_EQ(ignored.actions.size, 0U);
            REQUIRE_EQ(
                caller.state(), HandshakeState::awaiting_conclusion_response);
            REQUIRE_EQ(caller.peer_socket_id(), 200U);
            REQUIRE_EQ(caller.destination_socket_id(), 200U);
        }
        const auto final_reply =
            listener.receive(message_from(conclusion.actions.values[0]));
        if (variant == 0) {
            const auto connected =
                caller.receive(message_from(final_reply.values[0]));
            REQUIRE_EQ(connected.actions.values[0].kind,
                HandshakeActionKind::connected);
        } else {
            const auto exhausted = caller.timeout();
            REQUIRE_EQ(
                exhausted.actions.values[0].kind, HandshakeActionKind::failed);
        }
    }
}
