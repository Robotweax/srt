#include "compat/caller_handshake_steps.hpp"

#include <utility>

namespace robotweax::srt::compat {

CallerHandshakeSteps::CallerHandshakeSteps(
    HandshakeMachine::Configuration configuration) noexcept
    : machine_(configuration)
    , local_socket_id_(configuration.local_socket_id)
{
}

CallerHandshakeStep CallerHandshakeSteps::step(
    HandshakeActions actions) const noexcept
{
    return {
        .actions = std::move(actions),
        .destination_socket_id = destination_socket_id_,
    };
}

CallerHandshakeStep CallerHandshakeSteps::start() noexcept
{
    return step(machine_.start());
}

CallerHandshakeStep CallerHandshakeSteps::receive(
    const HandshakeMessage& message) noexcept
{
    if (machine_.is_stale_induction(message)) {
        return step({});
    }
    peer_socket_id_ = message.packet.socket_id;
    if (message.packet.request == HandshakeRequest::induction) {
        destination_socket_id_ =
            caller_conclusion_destination(local_socket_id_, peer_socket_id_);
    }
    return step(machine_.receive(message));
}

CallerHandshakeStep CallerHandshakeSteps::timeout() noexcept
{
    return step(machine_.timeout());
}

Error CallerHandshakeSteps::set_key_material_request(
    const KeyMaterialBuffer& request, std::uint16_t encryption_field) noexcept
{
    return machine_.set_key_material_request(request, encryption_field);
}

} // namespace robotweax::srt::compat
