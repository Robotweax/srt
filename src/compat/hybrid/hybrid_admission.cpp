// SPDX-License-Identifier: MIT
#include "compat/hybrid/hybrid_admission.hpp"
#include "robotweax/srt/handshake_datagram.hpp"

#include <algorithm>

namespace robotweax::srt::compat::hybrid {
namespace {
bool nonzero(const CredentialId& id) noexcept
{
    return std::any_of(id.begin(), id.end(), [](auto b) {
        return b != std::byte {};
    });
}
bool valid_authorization(const Authorization& a) noexcept
{
    return nonzero(a.credential_id) && a.generation && a.expires_at_us
        && a.stream_id.size <= a.stream_id.bytes.size()
        && validate_group_membership(
               {.group_id = a.caller_group, .type = GroupType::multicast},
               {.admit_multicast_dispatch = true})
        == GroupMembershipValidation::compatible
        && validate_group_membership(
               {.group_id = a.listener_group, .type = GroupType::multicast},
               {.admit_multicast_dispatch = true})
        == GroupMembershipValidation::compatible;
}
bool conclusion(std::span<const std::byte> payload, bool caller,
    HandshakeMessage& message) noexcept
{
    if (payload.size() < handshake_size
        || payload.size() > maximum_conclusion_payload)
        return false;
    std::array<std::byte, 1500> datagram {};
    const auto wire = encode_packet(
        {.kind = PacketKind::control,
            .control = {.type = ControlType::handshake, .timestamp = {}},
            .payload = payload},
        datagram);
    if (!wire)
        return false;
    const auto decoded = decode_handshake_datagram(
        std::span {datagram}.first(wire.bytes_written));
    if (!decoded)
        return false;
    message = decoded.message;
    return message.packet.version == handshake_version_5
        && message.packet.request == HandshakeRequest::conclusion
        && message.packet.socket_id && message.has_handshake_extension
        && message.extension_type
        == (caller ? HandshakeExtensionType::handshake_request
                   : HandshakeExtensionType::handshake_response)
        && message.has_group_membership
        && message.group_membership.type == GroupType::multicast
        && validate_group_membership(
               message.group_membership, {.admit_multicast_dispatch = true})
        == GroupMembershipValidation::compatible
        && !message.has_unknown_extension
        && !message.has_packet_filter_extension && !message.has_standby_rtt
        && (!message.has_congestion_extension
            || message.congestion_controller == CongestionController::live);
}
void put32(std::byte* p, std::uint32_t n) noexcept
{
    for (unsigned i = 0; i < 4; ++i)
        p[i] = std::byte(n >> (24 - i * 8));
}
}
bool admission_transcript(const Context& c, const Authorization& a,
    std::span<const std::byte> caller_payload,
    std::span<const std::byte> listener_payload,
    AdmissionTranscript& out) noexcept
{
    if (!valid(c) || !valid_authorization(a))
        return false;
    HandshakeMessage caller, listener;
    if (!conclusion(caller_payload, true, caller)
        || !conclusion(listener_payload, false, listener)
        || caller.packet.socket_id == listener.packet.socket_id
        || caller.group_membership.group_id != a.caller_group
        || listener.group_membership.group_id != a.listener_group
        || a.listener_group != c.dispatch.group_id
        || caller.stream_id.view() != a.stream_id.view()
        || listener.has_stream_id_extension || !caller.has_multicast_source
        || caller.multicast_source.kind != MulticastSourceKind::offer
        || caller.has_multicast_dispatch || !listener.has_multicast_dispatch
        || listener.multicast_dispatch != c.dispatch
        || !listener.has_multicast_source
        || listener.multicast_source != c.source)
        return false;
    AdmissionTranscript transcript;
    transcript.size =
        admission_prefix_size + caller_payload.size() + listener_payload.size();
    put32(transcript.bytes.data(), 0x52574101U);
    put32(transcript.bytes.data() + 4,
        static_cast<std::uint32_t>(transcript.size));
    put32(transcript.bytes.data() + 8,
        static_cast<std::uint32_t>(caller_payload.size()));
    put32(transcript.bytes.data() + 12,
        static_cast<std::uint32_t>(listener_payload.size()));
    std::copy(a.credential_id.begin(), a.credential_id.end(),
        transcript.bytes.begin() + 16);
    if (!encode(c, std::span {transcript.bytes}.subspan(32, context_size)))
        return false;
    std::copy(caller_payload.begin(), caller_payload.end(),
        transcript.bytes.begin() + admission_prefix_size);
    std::copy(listener_payload.begin(), listener_payload.end(),
        transcript.bytes.begin()
            + static_cast<std::ptrdiff_t>(
                admission_prefix_size + caller_payload.size()));
    out = transcript;
    return true;
}

#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_HYBRID_CONTROL
static_assert(sizeof(Admission) < 8 * 1024); // Documented storage bound.
static_assert(maximum_admission_size == hybrid_control_maximum_proof_material);
Admission::Admission(CryptoProvider& provider, bool caller) noexcept
    : authentication_(provider)
    , caller_(caller)
{
}
Admission::~Admission()
{
    close();
}
bool Admission::begin() noexcept
{
    if (state_ != State::empty)
        return false;
    if (authentication_.start(caller_, authentication_version) != Error::none) {
        close();
        return false;
    }
    state_ = State::begun;
    return true;
}
bool Admission::current(const Authorization& a, std::uint64_t now) noexcept
{
    if (now < last_now_ || !valid_authorization(a)
        || a.credential_id != authorization_.credential_id
        || a.caller_group != authorization_.caller_group
        || a.listener_group != authorization_.listener_group
        || a.stream_id.view() != authorization_.stream_id.view()
        || a.generation != authorization_.generation
        || a.expires_at_us != authorization_.expires_at_us
        || now >= a.expires_at_us)
        return false;
    last_now_ = now;
    return true;
}
bool Admission::prepare(const Context& c, const Authorization& a,
    std::span<const std::byte> caller_payload,
    std::span<const std::byte> listener_payload, std::uint64_t now) noexcept
{
    if (state_ != State::empty && state_ != State::begun)
        return false;
    authorization_ = a;
    if (!current(a, now)
        || !admission_transcript(
            c, a, caller_payload, listener_payload, transcript_)
        || !encode(c, context_) || (state_ == State::empty && !begin())) {
        close();
        return false;
    }
    caller_id_ = decode_handshake(caller_payload).handshake.socket_id;
    listener_id_ = decode_handshake(listener_payload).handshake.socket_id;
    state_ = State::prepared;
    return true;
}
Error Admission::establish(const CredentialId& id,
    std::span<const std::byte> secret,
    const SessionAuthenticationParameters& peer, std::uint64_t now) noexcept
{
    if (state_ != State::prepared)
        return Error::invalid_state;
    if (!current(authorization_, now) || id != authorization_.credential_id
        || secret.size() != 32
        || std::all_of(secret.begin(), secret.end(), [](auto b) {
               return b == std::byte {};
           })) {
        close();
        return Error::invalid_key_material;
    }
    const auto error = authentication_.establish(secret, peer,
        caller_ ? caller_id_ : listener_id_,
        caller_ ? listener_id_ : caller_id_, 0, 0, context_);
    if (error != Error::none) {
        close();
        return error;
    }
    state_ = State::established;
    return Error::none;
}
Error Admission::proof(SessionAuthenticationParameters& out) noexcept
{
    if (state_ != State::established && state_ != State::confirmed)
        return Error::invalid_state;
    SessionAuthenticationParameters proof;
    const auto error =
        authentication_.handshake_proof(caller_, transcript_.view(), proof);
    if (error == Error::none)
        out = proof;
    return error;
}
bool Admission::confirm(const SessionAuthenticationParameters& proof,
    const Authorization& a, std::uint64_t now) noexcept
{
    if (state_ != State::established && state_ != State::confirmed)
        return false;
    if (!current(a, now)
        || !authentication_.verify_handshake(
            !caller_, transcript_.view(), proof)) {
        close();
        return false;
    }
    state_ = State::confirmed;
    confirmed_peer_proof_ = proof.proof;
    return true;
}
namespace {
std::array<std::byte, 48> receipt_material(
    std::span<const std::byte, 32> proof) noexcept
{
    std::array<std::byte, 48> bytes {};
    constexpr char label[] = "RWSRT-HYB-READY";
    static_assert(sizeof(label) == 16);
    for (std::size_t i = 0; i < sizeof(label); ++i)
        bytes[i] = std::byte(static_cast<unsigned char>(label[i]));
    std::copy(proof.begin(), proof.end(), bytes.begin() + 16);
    return bytes;
}
}
Error Admission::receipt(SessionAuthenticationParameters& out) noexcept
{
    if (caller_ || state_ != State::confirmed)
        return Error::invalid_state;
    SessionAuthenticationParameters proof;
    const auto error = authentication_.handshake_proof(
        false, receipt_material(confirmed_peer_proof_), proof);
    if (error == Error::none)
        out = proof;
    return error;
}
bool Admission::accept_receipt(
    const SessionAuthenticationParameters& receipt) noexcept
{
    if (!caller_ || state_ != State::confirmed)
        return false;
    SessionAuthenticationParameters own;
    if (proof(own) != Error::none
        || !authentication_.verify_handshake(
            false, receipt_material(own.proof), receipt)) {
        close();
        return false;
    }
    return true;
}
const SessionAuthenticationParameters& Admission::parameters() const noexcept
{
    return authentication_.parameters();
}
bool Admission::revalidate(const Authorization& a, std::uint64_t now) noexcept
{
    if (state_ != State::confirmed)
        return false;
    if (!current(a, now)) {
        close();
        return false;
    }
    return true;
}
SessionAuthentication* Admission::authentication() noexcept
{
    return state_ == State::confirmed ? &authentication_ : nullptr;
}
void Admission::close() noexcept
{
    authentication_.revoke_hybrid_control();
    state_ = State::closed;
    transcript_ = {};
    authorization_ = {};
    confirmed_peer_proof_ = {};
}
#endif
} // namespace robotweax::srt::compat::hybrid
