#include "test.hpp"

#include "compat/caller_handshake_exchange.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

using namespace robotweax::srt;
using namespace robotweax::srt::compat;

namespace {

struct SentHandshake {
    HandshakeAction action {};
    std::uint32_t destination_socket_id = 0;
};

struct SendTrace {
    std::array<SentHandshake, 8> sent {};
    std::size_t size = 0;
    CallerHandshakeSendStatus status = CallerHandshakeSendStatus::sent;
    int system_error = 0;
};

[[nodiscard]] CallerHandshakeSendResult record_send(void* opaque_context,
    const HandshakeAction& action, std::uint32_t destination_socket_id) noexcept
{
    auto& trace = *static_cast<SendTrace*>(opaque_context);
    if (trace.status != CallerHandshakeSendStatus::sent) {
        return {
            .status = trace.status,
            .system_error = trace.system_error,
        };
    }
    if (trace.size == trace.sent.size()) {
        return {.status = CallerHandshakeSendStatus::failed};
    }
    trace.sent[trace.size++] = {
        .action = action,
        .destination_socket_id = destination_socket_id,
    };
    return {};
}

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

} // namespace

TEST(caller_handshake_exchange_runs_complete_bounded_hsv5_actions)
{
    SendTrace trace;
    CallerHandshakeExchange caller {
        {
            .role = ConnectionRole::caller,
            .local_socket_id = 100U,
            .initial_sequence = SequenceNumber {10U},
            .flow_window = 111U,
            .timeout_milliseconds = 37U,
        },
        record_send, &trace};
    HandshakeMachine listener {{
        .role = ConnectionRole::listener,
        .local_socket_id = 200U,
        .initial_sequence = SequenceNumber {20U},
        .flow_window = 222U,
        .cookie_generator = fixed_cookie,
    }};

    const CallerHandshakeExchangeResult started = caller.start();
    REQUIRE_EQ(started.outcome, CallerHandshakeExchangeOutcome::running);
    REQUIRE(started.arm_retry_timer);
    REQUIRE_EQ(started.retry_timeout_milliseconds, 37U);
    REQUIRE_EQ(trace.size, 1U);
    REQUIRE_EQ(trace.sent[0].destination_socket_id, 0U);
    REQUIRE_EQ(
        trace.sent[0].action.packet.request, HandshakeRequest::induction);

    const HandshakeActions induction_response =
        listener.receive(message_from(trace.sent[0].action));
    const CallerHandshakeExchangeResult conclusion =
        caller.receive(message_from(induction_response.values[0]));
    REQUIRE_EQ(conclusion.outcome, CallerHandshakeExchangeOutcome::running);
    REQUIRE(conclusion.arm_retry_timer);
    REQUIRE_EQ(conclusion.retry_timeout_milliseconds, 37U);
    REQUIRE_EQ(trace.size, 2U);
    REQUIRE_EQ(trace.sent[1].destination_socket_id, 200U);
    REQUIRE_EQ(
        trace.sent[1].action.packet.request, HandshakeRequest::conclusion);

    const HandshakeActions conclusion_response =
        listener.receive(message_from(trace.sent[1].action));
    const CallerHandshakeExchangeResult connected =
        caller.receive(message_from(conclusion_response.values[0]));
    REQUIRE_EQ(connected.outcome, CallerHandshakeExchangeOutcome::connected);
    REQUIRE(!connected.arm_retry_timer);
    REQUIRE_EQ(caller.state(), HandshakeState::connected);
    REQUIRE_EQ(caller.peer_socket_id(), 200U);
    REQUIRE_EQ(caller.protocol().peer_flow_window(), 222U);
}

TEST(caller_handshake_exchange_maps_send_and_retry_failures)
{
    SendTrace io_trace {
        .status = CallerHandshakeSendStatus::io_error,
        .system_error = 55,
    };
    CallerHandshakeExchange io_failure {{
                                            .role = ConnectionRole::caller,
                                            .local_socket_id = 100U,
                                        },
        record_send, &io_trace};
    const CallerHandshakeExchangeResult io_result = io_failure.start();
    REQUIRE_EQ(io_result.outcome, CallerHandshakeExchangeOutcome::io_error);
    REQUIRE_EQ(io_result.system_error, 55);
    REQUIRE(!io_result.arm_retry_timer);

    CallerHandshakeExchange missing_sender {{
                                                .role = ConnectionRole::caller,
                                                .local_socket_id = 101U,
                                            },
        nullptr, nullptr};
    REQUIRE_EQ(
        missing_sender.start().outcome, CallerHandshakeExchangeOutcome::failed);

    SendTrace retry_trace;
    CallerHandshakeExchange retrying {{
                                          .role = ConnectionRole::caller,
                                          .local_socket_id = 102U,
                                          .timeout_milliseconds = 41U,
                                          .maximum_retries = 1U,
                                      },
        record_send, &retry_trace};
    (void)retrying.start();
    const CallerHandshakeExchangeResult retry = retrying.timeout();
    REQUIRE_EQ(retry.outcome, CallerHandshakeExchangeOutcome::running);
    REQUIRE(retry.arm_retry_timer);
    REQUIRE_EQ(retry.retry_timeout_milliseconds, 41U);
    REQUIRE_EQ(retry_trace.size, 2U);
    const CallerHandshakeExchangeResult exhausted = retrying.timeout();
    REQUIRE_EQ(exhausted.outcome, CallerHandshakeExchangeOutcome::failed);
    REQUIRE(!exhausted.arm_retry_timer);
    REQUIRE_EQ(retrying.state(), HandshakeState::failed);
}

TEST(caller_handshake_exchange_preserves_protocol_rejection_reason)
{
    SendTrace trace;
    CallerHandshakeExchange caller {{
                                        .role = ConnectionRole::caller,
                                        .local_socket_id = 100U,
                                    },
        record_send, &trace};
    (void)caller.start();

    HandshakeMessage rogue;
    rogue.packet.version = handshake_version_5;
    rogue.packet.request = HandshakeRequest::induction;
    rogue.packet.extension_field = 0U;
    rogue.packet.socket_id = 200U;
    rogue.packet.syn_cookie = 0x1020'3040U;
    const CallerHandshakeExchangeResult rejected = caller.receive(rogue);

    REQUIRE_EQ(rejected.outcome, CallerHandshakeExchangeOutcome::rejected);
    REQUIRE_EQ(rejected.rejection_reason, 4);
    REQUIRE_EQ(caller.state(), HandshakeState::rejected);
    REQUIRE_EQ(trace.size, 1U);
}
