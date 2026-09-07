#include "compat/listener_connection_setup.hpp"

#include <utility>

namespace robotweax::srt::compat {
namespace {

[[nodiscard]] bool valid_stateless_hsv5_seed(
    const HandshakeActions& actions, const HandshakeMachine& machine) noexcept
{
    return actions.size == 2U
        && actions.values[0].kind == HandshakeActionKind::send
        && actions.values[1].kind == HandshakeActionKind::arm_timer
        && machine.state() == HandshakeState::awaiting_conclusion_response;
}

} // namespace

ListenerConnectionSetupSteps::ListenerConnectionSetupSteps(
    ListenerHandshakeAdmission admission, Configuration configuration) noexcept
    : admission_(std::move(admission))
    , hsv5_(configuration.hsv5)
{
}

ListenerConnectionSetupResult ListenerConnectionSetupSteps::dispatch(
    const ListenerConnectionSetupEvent& event) noexcept
{
    if (state_ == ListenerConnectionSetupState::connected
        || state_ == ListenerConnectionSetupState::rejected
        || state_ == ListenerConnectionSetupState::failed
        || state_ == ListenerConnectionSetupState::closed
        || state_ == ListenerConnectionSetupState::timed_out) {
        return invalid();
    }

    switch (event.kind) {
    case ListenerConnectionSetupEventKind::start:
        return start();
    case ListenerConnectionSetupEventKind::admit:
        return admit(event.has_handshake ? &event.handshake : nullptr);
    case ListenerConnectionSetupEventKind::handshake:
        return receive_handshake(event.handshake);
    case ListenerConnectionSetupEventKind::retry_timer:
        return timeout();
    case ListenerConnectionSetupEventKind::reject:
        return reject(event.rejection_reason);
    case ListenerConnectionSetupEventKind::close:
        return terminal(ListenerConnectionSetupState::closed,
            ListenerConnectionSetupOutcome::closed);
    case ListenerConnectionSetupEventKind::overall_timeout:
        return terminal(ListenerConnectionSetupState::timed_out,
            ListenerConnectionSetupOutcome::timed_out);
    }
    return invalid();
}

bool ListenerConnectionSetupSteps::reconfigure(
    Configuration configuration) noexcept
{
    if (state_ != ListenerConnectionSetupState::awaiting_admission) {
        return false;
    }
    if (admission_.protocol != ListenerHandshakeProtocol::hsv5) {
        state_ = ListenerConnectionSetupState::failed;
        return false;
    }
    hsv5_ = HandshakeMachine {configuration.hsv5};
    const HandshakeActions seeded = hsv5_.receive(admission_.initial.message);
    if (!valid_stateless_hsv5_seed(seeded, hsv5_)) {
        state_ = ListenerConnectionSetupState::failed;
        return false;
    }
    return true;
}

ListenerConnectionSetupResult ListenerConnectionSetupSteps::start() noexcept
{
    if (state_ != ListenerConnectionSetupState::idle) {
        return invalid();
    }
    if (admission_.protocol != ListenerHandshakeProtocol::hsv5) {
        state_ = ListenerConnectionSetupState::failed;
        return {
            .outcome = ListenerConnectionSetupOutcome::failed,
            .protocol = admission_.protocol,
            .error = Error::invalid_handshake_value,
        };
    }
    const HandshakeActions seeded = hsv5_.receive(admission_.initial.message);
    if (!valid_stateless_hsv5_seed(seeded, hsv5_)) {
        state_ = ListenerConnectionSetupState::failed;
        return {
            .outcome = ListenerConnectionSetupOutcome::failed,
            .protocol = admission_.protocol,
            .error = Error::invalid_handshake_value,
        };
    }

    state_ = ListenerConnectionSetupState::awaiting_admission;
    return {
        .outcome = ListenerConnectionSetupOutcome::running,
        .protocol = admission_.protocol,
    };
}

ListenerConnectionSetupResult ListenerConnectionSetupSteps::admit(
    const HandshakeMessage* message) noexcept
{
    if (state_ != ListenerConnectionSetupState::awaiting_admission) {
        return invalid();
    }
    state_ = ListenerConnectionSetupState::running;
    const HandshakeMessage& conclusion =
        message != nullptr ? *message : admission_.conclusion.message;
    return translate(hsv5_.receive(conclusion));
}

ListenerConnectionSetupResult ListenerConnectionSetupSteps::receive_handshake(
    const HandshakeMessage& message) noexcept
{
    if (state_ != ListenerConnectionSetupState::running) {
        return invalid();
    }
    return translate(hsv5_.receive(message));
}

ListenerConnectionSetupResult ListenerConnectionSetupSteps::timeout() noexcept
{
    if (state_ != ListenerConnectionSetupState::running) {
        return invalid();
    }
    return translate(hsv5_.timeout());
}

ListenerConnectionSetupResult ListenerConnectionSetupSteps::reject(
    int rejection_reason) noexcept
{
    if (state_ != ListenerConnectionSetupState::awaiting_admission
        && state_ != ListenerConnectionSetupState::running) {
        return invalid();
    }
    return translate(hsv5_.reject(rejection_reason));
}

ListenerConnectionSetupResult ListenerConnectionSetupSteps::terminal(
    ListenerConnectionSetupState state,
    ListenerConnectionSetupOutcome outcome) noexcept
{
    state_ = state;
    return {
        .outcome = outcome,
        .protocol = admission_.protocol,
    };
}

ListenerConnectionSetupResult
ListenerConnectionSetupSteps::invalid() const noexcept
{
    return {
        .outcome = ListenerConnectionSetupOutcome::invalid,
        .protocol = admission_.protocol,
        .error = Error::invalid_state,
    };
}

ListenerConnectionSetupResult ListenerConnectionSetupSteps::translate(
    HandshakeActions actions) noexcept
{
    ListenerConnectionSetupResult result {
        .protocol = admission_.protocol,
        .hsv5_actions = std::move(actions),
    };
    for (std::size_t index = 0; index < result.hsv5_actions.size; ++index) {
        const HandshakeAction& action = result.hsv5_actions.values[index];
        if (action.kind == HandshakeActionKind::connected) {
            state_ = ListenerConnectionSetupState::connected;
            result.outcome = ListenerConnectionSetupOutcome::connected;
            return result;
        }
        if (action.kind == HandshakeActionKind::rejected) {
            state_ = ListenerConnectionSetupState::rejected;
            result.outcome = ListenerConnectionSetupOutcome::rejected;
            result.rejection_reason = action.rejection_reason;
            return result;
        }
        if (action.kind == HandshakeActionKind::failed) {
            state_ = ListenerConnectionSetupState::failed;
            result.outcome = ListenerConnectionSetupOutcome::failed;
            result.error = Error::invalid_handshake_value;
            return result;
        }
    }
    result.outcome = ListenerConnectionSetupOutcome::running;
    return result;
}

ListenerConnectionSetupRuntimeState
ListenerConnectionSetupSteps::runtime_state() const noexcept
{
    if (state_ != ListenerConnectionSetupState::connected) {
        return {};
    }
    ListenerConnectionSetupRuntimeState result {
        .error = Error::none,
        .packet_filter = hsv5_.negotiated_packet_filter(),
        .peer_flow_window = hsv5_.peer_flow_window(),
        .has_peer_extension_parameters = hsv5_.has_peer_extension_parameters(),
        .has_peer_stream_id = hsv5_.has_peer_stream_id(),
        .has_peer_group_membership = hsv5_.has_peer_group_membership(),
    };
    if (result.has_peer_extension_parameters) {
        result.peer_extension_parameters = hsv5_.peer_extension_parameters();
    }
    if (result.has_peer_stream_id) {
        result.peer_stream_id = hsv5_.peer_stream_id();
    }
    if (result.has_peer_group_membership) {
        result.peer_group_membership = hsv5_.peer_group_membership();
    }
    return result;
}

} // namespace robotweax::srt::compat
