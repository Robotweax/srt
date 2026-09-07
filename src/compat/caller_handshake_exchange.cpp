#include "compat/caller_handshake_exchange.hpp"

#include <cstddef>

namespace robotweax::srt::compat {

CallerHandshakeExchange::CallerHandshakeExchange(
    HandshakeMachine::Configuration configuration, CallerHandshakeSender sender,
    void* sender_context) noexcept
    : steps_(configuration)
    , sender_(sender)
    , sender_context_(sender_context)
{
}

CallerHandshakeExchangeResult CallerHandshakeExchange::execute(
    const CallerHandshakeStep& step) noexcept
{
    CallerHandshakeExchangeResult result;
    for (std::size_t index = 0; index < step.actions.size; ++index) {
        const HandshakeAction& action = step.actions.values[index];
        switch (action.kind) {
        case HandshakeActionKind::send: {
            if (sender_ == nullptr) {
                result.outcome = CallerHandshakeExchangeOutcome::failed;
                return result;
            }
            const CallerHandshakeSendResult sent =
                sender_(sender_context_, action, step.destination_socket_id);
            if (sent.status == CallerHandshakeSendStatus::failed) {
                result.outcome = CallerHandshakeExchangeOutcome::failed;
                return result;
            }
            if (sent.status == CallerHandshakeSendStatus::io_error) {
                result.outcome = CallerHandshakeExchangeOutcome::io_error;
                result.system_error = sent.system_error;
                return result;
            }
            break;
        }
        case HandshakeActionKind::connected:
            result.outcome = CallerHandshakeExchangeOutcome::connected;
            return result;
        case HandshakeActionKind::rejected:
            result.outcome = CallerHandshakeExchangeOutcome::rejected;
            result.rejection_reason = action.rejection_reason;
            return result;
        case HandshakeActionKind::failed:
            result.outcome = CallerHandshakeExchangeOutcome::failed;
            return result;
        case HandshakeActionKind::arm_timer:
            result.retry_timeout_milliseconds = action.timeout_milliseconds;
            result.arm_retry_timer = true;
            break;
        case HandshakeActionKind::none:
            break;
        }
    }
    return result;
}

CallerHandshakeExchangeResult CallerHandshakeExchange::start() noexcept
{
    return execute(steps_.start());
}

CallerHandshakeExchangeResult CallerHandshakeExchange::receive(
    const HandshakeMessage& message) noexcept
{
    return execute(steps_.receive(message));
}

CallerHandshakeExchangeResult CallerHandshakeExchange::timeout() noexcept
{
    return execute(steps_.timeout());
}

Error CallerHandshakeExchange::set_key_material_request(
    const KeyMaterialBuffer& request, std::uint16_t encryption_field) noexcept
{
    return steps_.set_key_material_request(request, encryption_field);
}

} // namespace robotweax::srt::compat
