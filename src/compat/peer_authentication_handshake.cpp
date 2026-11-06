// SPDX-License-Identifier: MIT
#include "compat/peer_authentication_handshake.hpp"
#include "compat/caller_handshake_steps.hpp"
#include "compat/listener_handshake_sources.hpp"
#include <algorithm>

namespace robotweax::srt::compat {
namespace pa = peer_authentication;
HandshakeMachine::Configuration
PeerAuthenticationHandshake::transport_configuration(
    HandshakeMachine::Configuration configuration,
    const SocketOptions& options) noexcept
{
    // This machine validates transport only. The outer adapter exclusively
    // owns PEERAUTH and withholds its ordinary connected result.
    configuration.require_session_authentication = false;
    configuration.encryption_field = static_cast<std::uint16_t>(
        options.configured_encryption_key_length() / 8);
    return configuration;
}
PeerAuthenticationHandshake::PeerAuthenticationHandshake(
    HandshakeMachine::Configuration configuration, const SocketOptions& options,
    CryptoProvider& provider) noexcept
    : configuration_(configuration)
    , options_(options)
    , transport_(transport_configuration(configuration, options))
    , admission_(provider, configuration.role == ConnectionRole::caller)
    , caller_(configuration.role == ConnectionRole::caller)
    , key_length_(
          static_cast<std::uint8_t>(options.configured_encryption_key_length()))
{
}
PeerAuthenticationHandshake::~PeerAuthenticationHandshake()
{
    abort();
}
void PeerAuthenticationHandshake::clear_credentials() noexcept
{
    options_.clear_peer_authentication();
    (void)options_.set_passphrase({});
}
void PeerAuthenticationHandshake::abort() noexcept
{
    admission_.abort();
    clear_credentials();
    state_ = State::failed;
}
bool PeerAuthenticationHandshake::active(Time now) noexcept
{
    if (state_ == State::fresh || state_ == State::released
        || state_ == State::failed)
        return false;
    if (now >= deadline_) {
        abort();
        return false;
    }
    return true;
}
PeerAuthenticationHandshake::Result PeerAuthenticationHandshake::finish(
    Result result) noexcept
{
    result.ready = state_ == State::ready;
    result.failed = state_ == State::failed;
    return result;
}
void PeerAuthenticationHandshake::fail(Result& result, Error error) noexcept
{
    abort();
    result.error = error;
    result.size = 0;
}
PacketTimestamp PeerAuthenticationHandshake::timestamp(Time now) const noexcept
{
    return PacketTimestamp {static_cast<std::uint32_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now - origin_)
            .count())};
}
bool PeerAuthenticationHandshake::configure_admission(
    Result& result, Time now) noexcept
{
    const auto error = admission_.configure(options_, deadline_, now);
    clear_credentials();
    if (error == Error::none)
        return true;
    fail(result, error);
    return false;
}
bool PeerAuthenticationHandshake::transport_result(
    const HandshakeActions& actions, Result& result) noexcept
{
    for (std::size_t i = 0; i < actions.size; ++i) {
        if (actions.values[i].kind == HandshakeActionKind::failed
            || actions.values[i].kind == HandshakeActionKind::rejected) {
            fail(result, Error::invalid_handshake_value);
            return false;
        }
    }
    return true;
}
void PeerAuthenticationHandshake::emit_action(
    const HandshakeAction& action, Result& result, Time now) noexcept
{
    if (result.size == result.outgoing.size()) {
        fail(result, Error::invalid_state);
        return;
    }
    auto& output = result.outgoing[result.size];
    const auto encoded = encode_handshake_datagram(
        action, timestamp(now), peer_socket_id_, output.bytes);
    if (!encoded) {
        fail(result, encoded.error);
        return;
    }
    output.size = encoded.bytes_written;
    ++result.size;
}
void PeerAuthenticationHandshake::emit_proof(HandshakeAction action,
    const pa::Record& record, Result& result, Time now) noexcept
{
    action.has_peer_authentication = true;
    action.packet.extension_field |= handshake_extension_flag_config;
    action.packet.encryption_field = record.key_length / 8;
    auto error = pa::encode_record(record, action.peer_authentication.bytes,
        action.peer_authentication.size);
    if (error != Error::none) {
        fail(result, error);
        return;
    }
    emit_action(action, result, now);
    if (state_ == State::failed)
        return;
    auto& output = result.outgoing[result.size - 1];
    auto payload = std::span {output.bytes}.subspan(
        packet_header_size, output.size - packet_header_size);
    if (record.message == pa::Message::offer) {
        offer_header_ = decode_handshake_payload(payload).handshake;
        error = admission_.bind_offer(payload, now);
    } else if (record.message == pa::Message::response) {
        error = admission_.seal_response(payload, now);
    } else {
        error = admission_.seal_confirmation(payload, now);
    }
    if (error != Error::none)
        fail(result, error);
}
void PeerAuthenticationHandshake::emit_cached(
    pa::Message message, Result& result, Time now) noexcept
{
    if (result.size == result.outgoing.size()) {
        fail(result, Error::invalid_state);
        return;
    }
    auto& output = result.outgoing[result.size];
    const auto encoded =
        encode_peer_authentication_datagram(admission_.cached(message, now),
            timestamp(now), peer_socket_id_, output.bytes);
    if (!encoded) {
        fail(result, encoded.error);
        return;
    }
    output.size = encoded.bytes_written;
    ++result.size;
}
void PeerAuthenticationHandshake::emit_key(bool response,
    std::span<const std::byte> payload, Result& result, Time now) noexcept
{
    if (payload.empty())
        return;
    if (result.size == result.outgoing.size()) {
        fail(result, Error::invalid_state);
        return;
    }
    auto& output = result.outgoing[result.size];
    MutablePacketView packet;
    packet.kind = PacketKind::control;
    packet.control.type = ControlType::user_defined;
    packet.control.subtype = response ? authenticated_key_response_subtype
                                      : authenticated_key_request_subtype;
    packet.control.destination_socket_id = peer_socket_id_;
    packet.control.timestamp = timestamp(now);
    packet.payload = payload;
    const auto encoded = encode_packet(packet, output.bytes);
    if (!encoded) {
        fail(result, encoded.error);
        return;
    }
    output.size = packet_header_size + payload.size();
    ++result.size;
}
void PeerAuthenticationHandshake::emit_key_request(
    Result& result, Time now) noexcept
{
    std::array<std::byte,
        maximum_key_material_size + authenticated_key_overhead>
        wire {};
    std::size_t written = 0;
    const auto error = admission_.key_request(wire, written, now);
    if (error != Error::none) {
        fail(result, error);
        return;
    }
    emit_key(false, std::span {wire}.first(written), result, now);
}
PeerAuthenticationHandshake::Result PeerAuthenticationHandshake::start(
    Time now, Time deadline) noexcept
{
    Result result;
    if (state_ != State::fresh) {
        result.error = Error::invalid_state;
        return finish(result);
    }
    if (deadline <= now || !options_.peer_authentication_required()
        || configuration_.local_socket_id == 0
        || configuration_.has_group_membership
        || configuration_.group_membership_negotiator
        || configuration_.multicast_dispatch_provider
        || configuration_.path_identifier.size != 0
        || configuration_.require_path_identifier
        || configuration_.require_standby_rtt
        || configuration_.key_material_request.size != 0) {
        fail(result, Error::invalid_state);
        return finish(result);
    }
    origin_ = now;
    deadline_ = deadline;
    state_ = State::discovery;
    if (caller_) {
        const auto actions = transport_.start();
        if (transport_result(actions, result))
            for (std::size_t i = 0; i < actions.size; ++i)
                if (actions.values[i].kind == HandshakeActionKind::send)
                    emit_action(actions.values[i], result, now);
    }
    return finish(result);
}
PeerAuthenticationHandshake::Result
PeerAuthenticationHandshake::start_admitted_listener(
    const ListenerHandshakeAdmission& admitted, Time now,
    Time deadline) noexcept
{
    auto result = start(now, deadline);
    if (result.failed)
        return result;
    if (caller_ || admitted.protocol != ListenerHandshakeProtocol::hsv5
        || admitted.validated_cookie == 0
        || admitted.conclusion.message.packet.syn_cookie
            != admitted.validated_cookie
        || !admitted.conclusion.message.has_peer_authentication) {
        fail(result, Error::invalid_state);
        return finish(result);
    }
    const auto seed = transport_.receive(admitted.initial.message);
    if (!transport_result(seed, result))
        return finish(result);
    if (transport_.state() != HandshakeState::awaiting_conclusion_response) {
        fail(result, Error::invalid_state);
        return finish(result);
    }
    // The stateless listener already checked address/cookie routing. Readdress
    // only the outer header to the admitted child's setup route; the proof
    // payload, including opaque records, stays byte-identical.
    Datagram datagram;
    const auto encoded = encode_peer_authentication_datagram(
        admitted.conclusion.message.peer_authentication_payload.view(),
        admitted.conclusion.control.timestamp, configuration_.local_socket_id,
        datagram.bytes);
    if (!encoded) {
        fail(result, encoded.error);
        return finish(result);
    }
    result =
        receive(std::span {datagram.bytes}.first(encoded.bytes_written), now);
    if (!result.failed && state_ != State::proofs)
        fail(result, Error::invalid_state);
    return finish(result);
}
PeerAuthenticationHandshake::Result PeerAuthenticationHandshake::receive(
    std::span<const std::byte> datagram, Time now) noexcept
{
    Result result;
    if (!active(now)) {
        result.error = Error::invalid_state;
        return finish(result);
    }
    const auto packet = decode_packet(datagram);
    if (!packet || packet.packet.kind != PacketKind::control) {
        result.error = Error::invalid_packet_type;
        return finish(result);
    }
    const auto& control = packet.packet.control;
    if (control.destination_socket_id != configuration_.local_socket_id
        && !(control.destination_socket_id == 0 && !caller_
            && state_ == State::discovery)) {
        result.error = Error::invalid_state;
        return finish(result);
    }
    if (control.type == ControlType::user_defined) {
        if ((state_ != State::keys && state_ != State::ready)
            || control.type_specific != 0
            || (control.subtype != authenticated_key_request_subtype
                && control.subtype != authenticated_key_response_subtype)) {
            result.error = Error::invalid_state;
            return finish(result);
        }
        std::array<std::byte,
            maximum_key_material_size + authenticated_key_overhead>
            response {};
        std::size_t written = 0;
        result.error = admission_.accept_key_control(
            control.subtype == authenticated_key_response_subtype,
            packet.packet.payload, response, written, now);
        if (admission_.failed()) {
            fail(result, result.error);
            return finish(result);
        }
        if (result.error == Error::none) {
            emit_key(true, std::span {response}.first(written), result, now);
            if (state_ != State::failed && admission_.ready_to_publish(now))
                state_ = State::ready;
        }
        return finish(result);
    }
    const auto decoded = decode_handshake_datagram(datagram);
    if (!decoded) {
        result.error = decoded.error;
        return finish(result);
    }
    auto message = decoded.message;
    if (message.packet.request == HandshakeRequest::induction) {
        if (state_ != State::discovery)
            return finish(result);
        const auto actions = transport_.receive(message);
        if (!transport_result(actions, result))
            return finish(result);
        for (std::size_t i = 0; i < actions.size; ++i) {
            const auto& action = actions.values[i];
            if (action.kind != HandshakeActionKind::send)
                continue;
            if (!caller_) {
                peer_socket_id_ = message.packet.socket_id;
                emit_action(action, result, now);
            } else {
                if (!configure_admission(result, now))
                    return finish(result);
                // Conclusions can be routed to the listener's discovery ID;
                // the authenticated response supplies its child's runtime ID.
                peer_socket_id_ = caller_conclusion_destination(
                    configuration_.local_socket_id, message.packet.socket_id);
                pa::Record record;
                result.error =
                    admission_.prepare_offer(key_length_, record, now);
                if (result.error != Error::none) {
                    fail(result, result.error);
                    return finish(result);
                }
                state_ = State::proofs;
                emit_proof(action, record, result, now);
            }
        }
        return finish(result);
    }
    if (!message.has_peer_authentication || message.has_group_membership
        || message.has_path_identifier || message.has_standby_rtt
        || message.has_multicast_dispatch || message.has_session_authentication
        || message.has_key_material_extension) {
        fail(result, Error::authentication_failure);
        return finish(result);
    }
    const auto payload = message.peer_authentication_payload.view();
    if (!caller_ && state_ == State::discovery
        && message.packet.request == HandshakeRequest::conclusion) {
        message.has_peer_authentication =
            false; // Transport validation only; retain original bytes for proof.
        const auto actions = transport_.receive(message);
        if (!transport_result(actions, result))
            return finish(result);
        if (transport_.state() != HandshakeState::connected)
            return finish(result); // cookie/transport not admitted
        if (!configure_admission(result, now))
            return finish(result);
        pa::Record record;
        result.error =
            admission_.prepare_response(payload, key_length_, record, now);
        if (result.error != Error::none) {
            fail(result, result.error);
            return finish(result);
        }
        peer_socket_id_ = message.packet.socket_id;
        offer_header_ = message.packet;
        key_length_ = record.key_length;
        state_ = State::proofs;
        for (std::size_t i = 0; i < actions.size; ++i)
            if (actions.values[i].kind == HandshakeActionKind::send)
                emit_proof(actions.values[i], record, result, now);
    } else if (caller_ && state_ == State::proofs
        && message.packet.request == HandshakeRequest::conclusion) {
        message.has_peer_authentication = false;
        const auto actions = transport_.receive(message);
        if (!transport_result(actions, result))
            return finish(result);
        if (transport_.state() != HandshakeState::connected)
            return finish(result);
        pa::Record record;
        result.error = admission_.prepare_confirmation(payload, record, now);
        if (result.error != Error::none) {
            fail(result, result.error);
            return finish(result);
        }
        peer_socket_id_ = message.packet.socket_id;
        HandshakeAction action {
            .kind = HandshakeActionKind::send, .packet = offer_header_};
        action.packet.request = HandshakeRequest::agreement;
        action.packet.extension_field = handshake_extension_flag_config;
        emit_proof(action, record, result, now);
        if (state_ != State::failed) {
            state_ = State::keys;
            emit_key_request(result, now);
        }
    } else if (!caller_
        && message.packet.request == HandshakeRequest::agreement) {
        auto expected = offer_header_;
        expected.request = HandshakeRequest::agreement;
        expected.extension_field = handshake_extension_flag_config;
        expected.encryption_field = key_length_ / 8;
        std::array<std::byte, handshake_size> header {};
        if (encode_handshake(expected, header) != Error::none
            || !std::equal(header.begin(), header.end(), payload.begin())) {
            fail(result, Error::authentication_failure);
            return finish(result);
        }
        result.error = admission_.accept_confirmation(payload, now);
        if (admission_.failed()) {
            fail(result, result.error);
            return finish(result);
        }
        if (result.error == Error::none) {
            if (state_ != State::ready)
                state_ = State::keys;
            emit_key_request(result, now);
        }
    } else {
        const auto expected = admission_.cached(
            caller_ ? pa::Message::response : pa::Message::offer, now);
        if (!std::equal(payload.begin(), payload.end(), expected.begin(),
                expected.end())) {
            result.error = Error::authentication_failure;
            return finish(result);
        }
        emit_cached(caller_ ? pa::Message::confirmation : pa::Message::response,
            result, now);
        if (state_ == State::keys)
            emit_key_request(result, now);
    }
    return finish(result);
}
PeerAuthenticationHandshake::Result PeerAuthenticationHandshake::retry(
    Time now) noexcept
{
    Result result;
    if (!active(now)) {
        result.error = Error::invalid_state;
        return finish(result);
    }
    if (state_ == State::ready)
        return finish(result);
    if (++retries_ > configuration_.maximum_retries) {
        fail(result, Error::invalid_state);
        return finish(result);
    }
    if (state_ == State::discovery) {
        const auto actions = transport_.timeout();
        if (transport_result(actions, result))
            for (std::size_t i = 0; i < actions.size; ++i)
                if (actions.values[i].kind == HandshakeActionKind::send)
                    emit_action(actions.values[i], result, now);
    } else {
        emit_cached(caller_
                ? (state_ == State::proofs ? pa::Message::offer
                                           : pa::Message::confirmation)
                : pa::Message::response,
            result, now);
        if (state_ == State::keys)
            emit_key_request(result, now);
    }
    return finish(result);
}
const HandshakeMachine* PeerAuthenticationHandshake::transport(
    Time now) noexcept
{
    return state_ == State::released || ready(now) ? &transport_ : nullptr;
}
bool PeerAuthenticationHandshake::ready(Time now) noexcept
{
    return active(now) && state_ == State::ready;
}
std::shared_ptr<CryptoSession> PeerAuthenticationHandshake::release_crypto(
    Time now) noexcept
{
    if (!ready(now))
        return {};
    auto crypto = admission_.release_crypto(now);
    if (crypto)
        state_ = State::released;
    return crypto;
}
std::span<const std::byte> PeerAuthenticationHandshake::peer_public_key(
    Time now) noexcept
{
    return ready(now) ? admission_.peer_public_key(now)
                      : std::span<const std::byte> {};
}
} // namespace robotweax::srt::compat
