#include "test.hpp"

#include "compat/listener_connection_setup.hpp"
#include "srt/version.h"

#include <array>
#include <cstddef>
#include <cstdint>

using namespace robotweax::srt;
using namespace robotweax::srt::compat;

namespace {

constexpr std::uint32_t cookie = 0x1020'3040U;
constexpr std::uint32_t listener_socket_id = 1'001U;
constexpr std::uint32_t caller_socket_id = 2'002U;

[[nodiscard]] std::uint32_t fixed_cookie(const Handshake&, void*) noexcept
{
    return cookie;
}

[[nodiscard]] HandshakeMessage induction()
{
    HandshakeMessage message;
    message.packet.version = handshake_version_4;
    message.packet.extension_field = udt_datagram_socket_type;
    message.packet.initial_sequence = SequenceNumber {0x1234'5678U};
    message.packet.maximum_transmission_unit = 1'500U;
    message.packet.flow_window = 25'600U;
    message.packet.request = HandshakeRequest::induction;
    message.packet.socket_id = caller_socket_id;
    return message;
}

[[nodiscard]] HandshakeMessage hsv5_conclusion()
{
    HandshakeMessage message = induction();
    message.packet.version = handshake_version_5;
    message.packet.request = HandshakeRequest::conclusion;
    message.packet.maximum_transmission_unit = 1'200U;
    message.packet.flow_window = 8'192U;
    message.packet.syn_cookie = cookie;
    message.packet.extension_field = 1U;
    message.has_handshake_extension = true;
    message.extension_type = HandshakeExtensionType::handshake_request;
    message.extension_parameters.srt_version = SRT_VERSION_VALUE;
    return message;
}

[[nodiscard]] ListenerHandshakeAdmission admission(
    ListenerHandshakeProtocol protocol)
{
    return {
        .initial =
            {
                .message = induction(),
                .peer = IpEndpoint::loopback(9'000U),
            },
        .conclusion =
            {
                .message = hsv5_conclusion(),
                .control = {.timestamp = PacketTimestamp {7'001U}},
                .peer = IpEndpoint::loopback(9'000U),
            },
        .protocol = protocol,
        .validated_cookie = cookie,
    };
}

[[nodiscard]] ListenerConnectionSetupSteps::Configuration configuration()
{
    ListenerConnectionSetupSteps::Configuration result;
    result.hsv5.role = ConnectionRole::listener;
    result.hsv5.local_socket_id = listener_socket_id;
    result.hsv5.initial_sequence = SequenceNumber {0x1234'5678U};
    result.hsv5.maximum_transmission_unit = 1'200U;
    result.hsv5.flow_window = 8'192U;
    result.hsv5.timeout_milliseconds = 37U;
    result.hsv5.maximum_retries = 1U;
    result.hsv5.cookie_generator = fixed_cookie;

    return result;
}

[[nodiscard]] const HandshakeAction& require_hsv5_action(
    const ListenerConnectionSetupResult& result, std::size_t index,
    HandshakeActionKind kind)
{
    REQUIRE(index < result.hsv5_actions.size);
    REQUIRE_EQ(result.hsv5_actions.values[index].kind, kind);
    return result.hsv5_actions.values[index];
}

} // namespace

TEST(listener_connection_setup_preserves_bounded_hsv5_admission_actions)
{
    ListenerConnectionSetupSteps setup {
        admission(ListenerHandshakeProtocol::hsv5), configuration()};

    const ListenerConnectionSetupResult started = setup.dispatch({
        .kind = ListenerConnectionSetupEventKind::start,
    });
    REQUIRE_EQ(started.outcome, ListenerConnectionSetupOutcome::running);
    REQUIRE_EQ(started.hsv5_actions.size, 0U);
    REQUIRE_EQ(setup.state(), ListenerConnectionSetupState::awaiting_admission);

    const ListenerConnectionSetupResult connected = setup.dispatch({
        .kind = ListenerConnectionSetupEventKind::admit,
    });
    REQUIRE_EQ(connected.outcome, ListenerConnectionSetupOutcome::connected);
    const HandshakeAction& response =
        require_hsv5_action(connected, 0U, HandshakeActionKind::send);
    REQUIRE_EQ(response.packet.request, HandshakeRequest::conclusion);
    REQUIRE_EQ(response.packet.socket_id, listener_socket_id);
    REQUIRE_EQ(response.packet.syn_cookie, cookie);
    (void)require_hsv5_action(connected, 1U, HandshakeActionKind::connected);
    REQUIRE_EQ(setup.state(), ListenerConnectionSetupState::connected);
    REQUIRE_EQ(setup.hsv5_protocol().peer_flow_window(), 8'192U);
    const ListenerConnectionSetupRuntimeState runtime = setup.runtime_state();
    REQUIRE(runtime);
    REQUIRE_EQ(runtime.peer_flow_window, 8'192U);
    REQUIRE(runtime.has_peer_extension_parameters);
    REQUIRE(!runtime.has_peer_stream_id);
    REQUIRE(!runtime.has_peer_group_membership);
    REQUIRE_EQ(setup
                   .dispatch({
                       .kind = ListenerConnectionSetupEventKind::close,
                   })
                   .outcome,
        ListenerConnectionSetupOutcome::invalid);
}

TEST(listener_connection_setup_exposes_hsv5_reject_and_invalid_input_contracts)
{
    ListenerConnectionSetupSteps rejected {
        admission(ListenerHandshakeProtocol::hsv5), configuration()};
    REQUIRE_EQ(rejected
                   .dispatch({
                       .kind = ListenerConnectionSetupEventKind::start,
                   })
                   .outcome,
        ListenerConnectionSetupOutcome::running);
    const ListenerConnectionSetupResult rejection = rejected.dispatch({
        .kind = ListenerConnectionSetupEventKind::reject,
        .rejection_reason = 10,
    });
    REQUIRE_EQ(rejection.outcome, ListenerConnectionSetupOutcome::rejected);
    REQUIRE_EQ(rejection.rejection_reason, 10);
    REQUIRE_EQ(require_hsv5_action(rejection, 0U, HandshakeActionKind::send)
                   .packet.request,
        static_cast<HandshakeRequest>(1'010));
    (void)require_hsv5_action(rejection, 1U, HandshakeActionKind::rejected);

    auto invalid_admission = admission(ListenerHandshakeProtocol::hsv5);
    invalid_admission.conclusion.message.packet.syn_cookie ^= 1U;
    ListenerConnectionSetupSteps retrying {invalid_admission, configuration()};
    REQUIRE_EQ(retrying
                   .dispatch({
                       .kind = ListenerConnectionSetupEventKind::start,
                   })
                   .outcome,
        ListenerConnectionSetupOutcome::running);
    REQUIRE_EQ(retrying
                   .dispatch({
                       .kind = ListenerConnectionSetupEventKind::admit,
                   })
                   .outcome,
        ListenerConnectionSetupOutcome::failed);
    REQUIRE_EQ(retrying.state(), ListenerConnectionSetupState::failed);
}

TEST(listener_connection_setup_reconfigures_after_listener_policy)
{
    ListenerConnectionSetupSteps setup {
        admission(ListenerHandshakeProtocol::hsv5), configuration()};
    REQUIRE(!setup.reconfigure(configuration()));
    REQUIRE_EQ(setup
                   .dispatch({
                       .kind = ListenerConnectionSetupEventKind::start,
                   })
                   .outcome,
        ListenerConnectionSetupOutcome::running);

    auto updated = configuration();
    updated.hsv5.maximum_transmission_unit = 1'000U;
    updated.hsv5.flow_window = 4'096U;
    REQUIRE(setup.reconfigure(updated));

    HandshakeMessage sanitized = hsv5_conclusion();
    sanitized.packet.flow_window = 2'048U;
    const ListenerConnectionSetupResult connected = setup.dispatch({
        .kind = ListenerConnectionSetupEventKind::admit,
        .handshake = sanitized,
        .has_handshake = true,
    });
    REQUIRE_EQ(connected.outcome, ListenerConnectionSetupOutcome::connected);
    const HandshakeAction& response =
        require_hsv5_action(connected, 0U, HandshakeActionKind::send);
    REQUIRE_EQ(response.packet.maximum_transmission_unit, 1'000U);
    REQUIRE_EQ(response.packet.flow_window, 4'096U);
    REQUIRE_EQ(setup.hsv5_protocol().peer_flow_window(), 2'048U);
    REQUIRE(!setup.reconfigure(configuration()));

    ListenerConnectionSetupSteps invalid {
        admission(ListenerHandshakeProtocol::hsv5), configuration()};
    REQUIRE_EQ(invalid
                   .dispatch({
                       .kind = ListenerConnectionSetupEventKind::start,
                   })
                   .outcome,
        ListenerConnectionSetupOutcome::running);
    auto invalid_configuration = configuration();
    invalid_configuration.hsv5.role = ConnectionRole::caller;
    REQUIRE(!invalid.reconfigure(invalid_configuration));
    REQUIRE_EQ(invalid.state(), ListenerConnectionSetupState::failed);
}

TEST(listener_connection_setup_orders_policy_close_and_timeout_terminally)
{
    ListenerConnectionSetupSteps setup {
        admission(ListenerHandshakeProtocol::hsv5), configuration()};
    REQUIRE_EQ(setup
                   .dispatch({
                       .kind = ListenerConnectionSetupEventKind::admit,
                   })
                   .outcome,
        ListenerConnectionSetupOutcome::invalid);
    REQUIRE_EQ(setup.state(), ListenerConnectionSetupState::idle);
    REQUIRE_EQ(setup
                   .dispatch({
                       .kind = ListenerConnectionSetupEventKind::start,
                   })
                   .outcome,
        ListenerConnectionSetupOutcome::running);
    const ListenerConnectionSetupResult rejected = setup.dispatch({
        .kind = ListenerConnectionSetupEventKind::reject,
        .rejection_reason = 7,
    });
    REQUIRE_EQ(rejected.outcome, ListenerConnectionSetupOutcome::rejected);
    REQUIRE_EQ(rejected.hsv5_actions.size, 2U);

    ListenerConnectionSetupSteps timed_out {
        admission(ListenerHandshakeProtocol::hsv5), configuration()};
    REQUIRE_EQ(timed_out
                   .dispatch({
                       .kind = ListenerConnectionSetupEventKind::start,
                   })
                   .outcome,
        ListenerConnectionSetupOutcome::running);
    REQUIRE_EQ(
        timed_out
            .dispatch({
                .kind = ListenerConnectionSetupEventKind::overall_timeout,
            })
            .outcome,
        ListenerConnectionSetupOutcome::timed_out);
    REQUIRE_EQ(timed_out
                   .dispatch({
                       .kind = ListenerConnectionSetupEventKind::close,
                   })
                   .outcome,
        ListenerConnectionSetupOutcome::invalid);

    ListenerConnectionSetupSteps closed {
        admission(ListenerHandshakeProtocol::hsv5), configuration()};
    REQUIRE_EQ(closed
                   .dispatch({
                       .kind = ListenerConnectionSetupEventKind::close,
                   })
                   .outcome,
        ListenerConnectionSetupOutcome::closed);
    REQUIRE_EQ(closed.state(), ListenerConnectionSetupState::closed);

    ListenerConnectionSetupSteps invalid_protocol {
        admission(ListenerHandshakeProtocol::invalid), configuration()};
    const ListenerConnectionSetupResult failed = invalid_protocol.dispatch({
        .kind = ListenerConnectionSetupEventKind::start,
    });
    REQUIRE_EQ(failed.outcome, ListenerConnectionSetupOutcome::failed);
    REQUIRE_EQ(failed.error, Error::invalid_handshake_value);
}
