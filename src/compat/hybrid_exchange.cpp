// SPDX-License-Identifier: MIT
#include "compat/hybrid_exchange.hpp"
#include "robotweax/srt/handshake_datagram.hpp"

#include <algorithm>
#include <limits>

namespace robotweax::srt::compat::hybrid {
namespace {
template <typename T> bool zero(const T& bytes) noexcept
{
    return std::all_of(bytes.begin(), bytes.end(), [](auto b) {
        return b == std::byte {};
    });
}
std::uint32_t get32(const std::byte* p) noexcept
{
    std::uint32_t n = 0;
    for (unsigned i = 0; i < 4; ++i)
        n = (n << 8) | std::to_integer<unsigned>(p[i]);
    return n;
}
void put32(std::byte* p, std::uint32_t n) noexcept
{
    for (unsigned i = 0; i < 4; ++i)
        p[i] = std::byte(n >> (24 - i * 8));
}
bool valid_record(const ExchangeRecord& r) noexcept
{
    if (!r.caller_socket || !r.listener_socket
        || r.caller_socket == r.listener_socket || zero(r.credential_id)
        || r.authentication.version != authentication_version)
        return false;
    if (r.kind == ExchangeKind::offer)
        return valid(r.capability) && r.capability.kind == CapabilityKind::offer
            && r.authentication.is_offer();
    if (!r.authentication.is_proof())
        return false;
    if (r.kind == ExchangeKind::selection)
        return valid(r.capability)
            && r.capability.kind == CapabilityKind::selection;
    return (r.kind == ExchangeKind::proof || r.kind == ExchangeKind::receipt)
        && r.capability
        == Capability {.supported = 0, .required = 0, .prefix_limit = 0};
}
}
bool encode(const ExchangeRecord& r, std::span<std::byte> out) noexcept
{
    if (out.size() != exchange_size || !valid_record(r))
        return false;
    std::array<std::byte, exchange_size> wire {};
    put32(wire.data(), 0x52575801);
    put32(wire.data() + 4, static_cast<std::uint32_t>(r.kind));
    put32(wire.data() + 8, r.caller_socket);
    put32(wire.data() + 12, r.listener_socket);
    std::copy(
        r.credential_id.begin(), r.credential_id.end(), wire.begin() + 16);
    if ((r.kind == ExchangeKind::offer || r.kind == ExchangeKind::selection)
        && !encode(r.capability, std::span {wire}.subspan(32, capability_size)))
        return false;
    std::copy(r.authentication.caller_nonce.begin(),
        r.authentication.caller_nonce.end(), wire.begin() + 80);
    std::copy(r.authentication.listener_nonce.begin(),
        r.authentication.listener_nonce.end(), wire.begin() + 112);
    std::copy(r.authentication.proof.begin(), r.authentication.proof.end(),
        wire.begin() + 144);
    std::copy(wire.begin(), wire.end(), out.begin());
    return true;
}
bool decode(std::span<const std::byte> in, ExchangeRecord& out) noexcept
{
    out = {};
    if (in.size() != exchange_size || get32(in.data()) != 0x52575801)
        return false;
    ExchangeRecord r;
    r.kind = static_cast<ExchangeKind>(get32(in.data() + 4));
    r.caller_socket = get32(in.data() + 8);
    r.listener_socket = get32(in.data() + 12);
    std::copy_n(in.begin() + 16, 16, r.credential_id.begin());
    if (r.kind == ExchangeKind::offer || r.kind == ExchangeKind::selection) {
        if (!decode(in.subspan(32, capability_size), r.capability))
            return false;
    } else if (!zero(in.subspan(32, capability_size)))
        return false;
    r.authentication.version = authentication_version;
    std::copy_n(in.begin() + 80, 32, r.authentication.caller_nonce.begin());
    std::copy_n(in.begin() + 112, 32, r.authentication.listener_nonce.begin());
    std::copy_n(in.begin() + 144, 32, r.authentication.proof.begin());
    if (!valid_record(r))
        return false;
    out = r;
    return true;
}

#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_HYBRID_CONTROL
static_assert(sizeof(Exchange) <= 16 * 1024);
namespace {
std::uint64_t add(std::uint64_t a, std::uint64_t b) noexcept
{
    return b > UINT64_MAX - a ? UINT64_MAX : a + b;
}
bool same_grant(const Authorization& a, const Authorization& b) noexcept
{
    return a.stream_id.size <= a.stream_id.bytes.size()
        && b.stream_id.size <= b.stream_id.bytes.size()
        && a.credential_id == b.credential_id
        && a.caller_group == b.caller_group
        && a.listener_group == b.listener_group
        && a.stream_id.view() == b.stream_id.view()
        && a.generation == b.generation && a.expires_at_us == b.expires_at_us;
}
bool message(std::span<const std::byte> payload, HandshakeMessage& out) noexcept
{
    if (payload.size() > maximum_conclusion_payload)
        return false;
    std::array<std::byte, 1500> wire {};
    const auto encoded = encode_packet(
        {.kind = PacketKind::control,
            .control = {.type = ControlType::handshake, .timestamp = {}},
            .payload = payload},
        wire);
    if (!encoded)
        return false;
    const auto decoded = decode_handshake_datagram(
        std::span {wire}.first(encoded.bytes_written));
    if (!decoded)
        return false;
    out = decoded.message;
    return true;
}
bool valid_peer(const IpEndpoint& peer) noexcept
{
    if ((!peer.is_ipv4() && !peer.is_ipv6()) || !peer.port || peer.is_wildcard()
        || peer.is_multicast() || peer.is_ipv4_mapped_ipv6())
        return false;
    if (peer.is_ipv4()) {
        if (peer.scope_id)
            return false;
        for (std::size_t i = 4; i < peer.address.size(); ++i)
            if (peer.address[i])
                return false;
    }
    return true;
}
struct EraseSecret {
    CryptoProvider& provider;
    std::array<std::byte, 32> bytes {};
    ~EraseSecret()
    {
        provider.secure_erase(bytes);
    }
};
}
Exchange::Exchange(CryptoProvider& crypto, bool caller) noexcept
    : crypto_(crypto)
    , caller_(caller)
    , admission_(crypto, caller)
{
}
Exchange::~Exchange()
{
    close();
}
Exchange::Step Exchange::fail() noexcept
{
    close();
    return {.status = Status::closed};
}
void Exchange::close() noexcept
{
    admission_.close();
    state_ = State::closed;
    cached_valid_ = false;
    last_peer_valid_ = false;
    cached_ = {};
    last_peer_ = {};
    caller_payload_ = {};
    listener_payload_ = {};
}
Exchange::Step Exchange::start(const Configuration& c,
    CredentialProvider provider, std::span<const std::byte> caller_payload,
    std::span<const std::byte> listener_payload, std::uint64_t now) noexcept
{
    if (state_ != State::empty)
        return {.status =
                    state_ == State::closed ? Status::closed : Status::ignored};
    if (!provider.resolve || !provider.revalidate || !c.connection_generation
        || !valid_peer(c.established_peer) || zero(c.credential_id)
        || !valid(c.policy) || c.policy.kind != CapabilityKind::offer
        || !c.retry_interval_us || !c.timeout_us
        || c.timeout_us > maximum_age_us || c.retry_interval_us > c.timeout_us
        || !c.maximum_sends_per_stage || c.maximum_sends_per_stage > 8
        || (caller_ ? (c.stream || c.member || c.initial_position)
                    : (!c.stream || !c.member)))
        return fail();
    HandshakeMessage caller, listener;
    if (!message(caller_payload, caller)
        || !message(listener_payload, listener))
        return fail();
    request_ = {.caller = caller_,
        .established_peer = c.established_peer,
        .connection_generation = c.connection_generation,
        .caller_socket = caller.packet.socket_id,
        .listener_socket = listener.packet.socket_id,
        .caller_group = caller.group_membership.group_id,
        .listener_group = listener.group_membership.group_id,
        .stream_id = caller.stream_id,
        .credential_id = c.credential_id,
        .context = {
            .dispatch = c.dispatch, .source = c.source, .offer = c.policy}};
    // Validate all frozen baseline metadata before any credential callback.
    Context baseline = request_.context;
    if (!negotiate(c.policy, c.policy, 1, 1, 0, baseline.selection))
        return fail();
    Authorization baseline_grant {.credential_id = c.credential_id,
        .caller_group = request_.caller_group,
        .listener_group = request_.listener_group,
        .stream_id = request_.stream_id,
        .generation = 1,
        .expires_at_us = UINT64_MAX};
    AdmissionTranscript checked;
    if (!admission_transcript(baseline, baseline_grant, caller_payload,
            listener_payload, checked))
        return fail();
    configuration_ = c;
    provider_ = provider;
    caller_size_ = caller_payload.size();
    listener_size_ = listener_payload.size();
    std::copy(
        caller_payload.begin(), caller_payload.end(), caller_payload_.begin());
    std::copy(listener_payload.begin(), listener_payload.end(),
        listener_payload_.begin());
    last_now_ = now;
    deadline_ = add(now, c.timeout_us);
    if (deadline_ == now)
        return fail();
    if (!caller_) {
        state_ = State::awaiting_offer;
        return {};
    }
    if (!admission_.begin()
        || !cache(ExchangeKind::offer, admission_.parameters(), now))
        return fail();
    state_ = State::awaiting_selection;
    return emit(now, Status::waiting);
}
bool Exchange::validate_grant(std::uint64_t now, bool confirmed) noexcept
{
    Authorization current;
    return grant_.generation && now < grant_.expires_at_us
        && provider_.revalidate(provider_.context, request_, grant_, current)
        && state_ != State::closed && same_grant(current, grant_)
        && (!confirmed || admission_.revalidate(current, now));
}
bool Exchange::live(std::uint64_t now) noexcept
{
    if (state_ == State::empty || state_ == State::closed)
        return false;
    if (now < last_now_ || (state_ != State::active && now >= deadline_)) {
        close();
        return false;
    }
    last_now_ = now;
    if (grant_.generation
        && !validate_grant(
            now, admission_.state() == Admission::State::confirmed)) {
        close();
        return false;
    }
    if (state_ == State::active && now >= deadline_)
        cached_valid_ = false;
    return true;
}
bool Exchange::establish(const ExchangeRecord& peer, std::uint64_t now) noexcept
{
    EraseSecret secret {crypto_};
    Authorization grant;
    if (!provider_.resolve(provider_.context, request_, grant, secret.bytes)
        || state_ == State::closed)
        return false;
    if (grant.credential_id != request_.credential_id
        || !admission_.prepare(request_.context, grant,
            std::span {caller_payload_}.first(caller_size_),
            std::span {listener_payload_}.first(listener_size_), now)
        || admission_.establish(
               request_.credential_id, secret.bytes, peer.authentication, now)
            != Error::none)
        return false;
    grant_ = grant;
    return validate_grant(now, false);
}
bool Exchange::cache(ExchangeKind kind,
    const SessionAuthenticationParameters& proof, std::uint64_t now) noexcept
{
    ExchangeRecord r {.kind = kind,
        .caller_socket = request_.caller_socket,
        .listener_socket = request_.listener_socket,
        .credential_id = request_.credential_id};
    if (kind == ExchangeKind::offer)
        r.capability = configuration_.policy;
    if (kind == ExchangeKind::selection)
        r.capability = request_.context.selection;
    r.authentication = proof;
    if (!encode(r, cached_))
        return false;
    cached_valid_ = true;
    sends_ = 0;
    next_send_ = now;
    return true;
}
Exchange::Step Exchange::emit(std::uint64_t now, Status otherwise) noexcept
{
    if (!cached_valid_ || now < next_send_)
        return {.status = otherwise};
    if (sends_ >= configuration_.maximum_sends_per_stage) {
        cached_valid_ = false;
        return {.status = otherwise};
    }
    ++sends_;
    next_send_ = add(now, configuration_.retry_interval_us);
    return {.status = Status::output, .has_output = true, .output = cached_};
}
Exchange::Step Exchange::receive(
    std::span<const std::byte> bytes, std::uint64_t now) noexcept
{
    if (!live(now))
        return {.status = Status::closed};
    ExchangeRecord r;
    if (!decode(bytes, r) || r.caller_socket != request_.caller_socket
        || r.listener_socket != request_.listener_socket
        || r.credential_id != request_.credential_id)
        return {.status = Status::ignored};
    if (last_peer_valid_
        && std::equal(bytes.begin(), bytes.end(), last_peer_.begin()))
        return emit(now, Status::duplicate);
    const auto& parameters = admission_.parameters();
    if (!caller_ && state_ == State::awaiting_offer
        && r.kind == ExchangeKind::offer) {
        request_.context.offer = r.capability;
        if (!negotiate(r.capability, configuration_.policy,
                configuration_.stream, configuration_.member,
                configuration_.initial_position, request_.context.selection))
            return fail();
        if (!establish(r, now))
            return fail();
        SessionAuthenticationParameters proof;
        if (admission_.proof(proof) != Error::none
            || !cache(ExchangeKind::selection, proof, now))
            return fail();
        state_ = State::awaiting_proof;
    } else if (caller_ && state_ == State::awaiting_selection
        && r.kind == ExchangeKind::selection) {
        if (r.authentication.caller_nonce != parameters.caller_nonce)
            return {.status = Status::ignored};
        request_.context.selection = r.capability;
        if (!valid(request_.context))
            return fail();
        if (!establish(r, now)
            || !admission_.confirm(r.authentication, grant_, now))
            return fail();
        SessionAuthenticationParameters proof;
        if (admission_.proof(proof) != Error::none
            || !cache(ExchangeKind::proof, proof, now))
            return fail();
        state_ = State::awaiting_receipt;
    } else if (!caller_ && state_ == State::awaiting_proof
        && r.kind == ExchangeKind::proof) {
        if (r.authentication.caller_nonce != parameters.caller_nonce
            || r.authentication.listener_nonce != parameters.listener_nonce)
            return {.status = Status::ignored};
        if (!admission_.confirm(r.authentication, grant_, now))
            return fail();
        SessionAuthenticationParameters receipt;
        if (admission_.receipt(receipt) != Error::none
            || !cache(ExchangeKind::receipt, receipt, now))
            return fail();
        state_ = State::active;
    } else if (caller_ && state_ == State::awaiting_receipt
        && r.kind == ExchangeKind::receipt) {
        if (r.authentication.caller_nonce != parameters.caller_nonce
            || r.authentication.listener_nonce != parameters.listener_nonce)
            return {.status = Status::ignored};
        if (!admission_.accept_receipt(r.authentication))
            return fail();
        cached_valid_ = false;
        state_ = State::active;
    } else
        return {.status = Status::ignored};
    std::copy(bytes.begin(), bytes.end(), last_peer_.begin());
    last_peer_valid_ = true;
    return emit(
        now, state_ == State::active ? Status::active : Status::waiting);
}
Exchange::Step Exchange::tick(std::uint64_t now) noexcept
{
    if (!live(now))
        return {.status = Status::closed};
    return emit(
        now, state_ == State::active ? Status::active : Status::waiting);
}
SessionAuthentication* Exchange::authentication() noexcept
{
    return state_ == State::active ? admission_.authentication() : nullptr;
}
std::uint64_t Exchange::next_deadline() const noexcept
{
    if (state_ == State::closed || state_ == State::empty)
        return UINT64_MAX;
    auto at = state_ == State::active ? grant_.expires_at_us : deadline_;
    if (grant_.generation)
        at = std::min(at, grant_.expires_at_us);
    if (cached_valid_)
        at = std::min({at, deadline_, next_send_});
    return at;
}
#endif
} // namespace robotweax::srt::compat::hybrid
