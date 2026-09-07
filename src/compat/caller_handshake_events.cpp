#include "compat/caller_handshake_events.hpp"

namespace robotweax::srt::compat {
CallerHandshakeEventDriver::CallerHandshakeEventDriver(
    HandshakeMachine::Configuration configuration, CallerHandshakeSender sender,
    void* sender_context) noexcept
    : exchange_(configuration, sender, sender_context)
{
}

CallerHandshakeEventResult CallerHandshakeEventDriver::translate(
    const CallerHandshakeExchangeResult& result) noexcept
{
    CallerHandshakeEventResult translated {
        .retry_timeout_milliseconds = result.retry_timeout_milliseconds,
        .system_error = result.system_error,
        .rejection_reason = result.rejection_reason,
        .arm_retry_timer = result.arm_retry_timer,
    };
    switch (result.outcome) {
    case CallerHandshakeExchangeOutcome::running:
        translated.outcome = CallerHandshakeEventOutcome::running;
        break;
    case CallerHandshakeExchangeOutcome::connected:
        translated.outcome = CallerHandshakeEventOutcome::connected;
        terminal_ = true;
        break;
    case CallerHandshakeExchangeOutcome::rejected:
        translated.outcome = CallerHandshakeEventOutcome::rejected;
        terminal_ = true;
        break;
    case CallerHandshakeExchangeOutcome::failed:
        translated.outcome = CallerHandshakeEventOutcome::failed;
        terminal_ = true;
        break;
    case CallerHandshakeExchangeOutcome::io_error:
        translated.outcome = CallerHandshakeEventOutcome::io_error;
        terminal_ = true;
        break;
    }
    return translated;
}

CallerHandshakeEventResult CallerHandshakeEventDriver::dispatch(
    const CallerHandshakeEvent& event) noexcept
{
    if (terminal_) {
        return {.outcome = CallerHandshakeEventOutcome::invalid};
    }
    switch (event.kind) {
    case CallerHandshakeEventKind::start:
        if (started_) {
            return {.outcome = CallerHandshakeEventOutcome::invalid};
        }
        started_ = true;
        return translate(exchange_.start());
    case CallerHandshakeEventKind::inbox_ready:
        if (!started_) {
            return {.outcome = CallerHandshakeEventOutcome::invalid};
        }
        return translate(exchange_.receive(event.message));
    case CallerHandshakeEventKind::retry_timer:
        if (!started_) {
            return {.outcome = CallerHandshakeEventOutcome::invalid};
        }
        {
            CallerHandshakeEventResult result = translate(exchange_.timeout());
            if (result.outcome == CallerHandshakeEventOutcome::failed) {
                result.outcome = CallerHandshakeEventOutcome::timed_out;
            }
            return result;
        }
    case CallerHandshakeEventKind::key_material: {
        if (!started_) {
            return {.outcome = CallerHandshakeEventOutcome::invalid};
        }
        const Error error = exchange_.set_key_material_request(
            event.key_material, event.encryption_field);
        terminal_ = error != Error::none;
        return {
            .outcome = error == Error::none
                ? CallerHandshakeEventOutcome::running
                : CallerHandshakeEventOutcome::failed,
            .error = error,
        };
    }
    case CallerHandshakeEventKind::close:
        terminal_ = true;
        return {.outcome = CallerHandshakeEventOutcome::closed};
    case CallerHandshakeEventKind::overall_timeout:
        terminal_ = true;
        return {.outcome = CallerHandshakeEventOutcome::timed_out};
    }
    return {.outcome = CallerHandshakeEventOutcome::invalid};
}

} // namespace robotweax::srt::compat
