// SPDX-License-Identifier: MIT
#include "test.hpp"
#include "compat/hybrid_admission.hpp"
#include "robotweax/srt/handshake_datagram.hpp"

#include <algorithm>
#include <limits>
#include <vector>

using namespace robotweax::srt;
namespace h = robotweax::srt::compat::hybrid;
namespace {
struct Fixture {
    h::Context context;
    h::Authorization grant;
    HandshakeAction caller, listener;
    Fixture()
    {
        context.dispatch = {.group_id = group_handle_mask | 1U,
            .multicast_group = {.address = {239, 255, 1, 1}, .port = 6000},
            .initial_sequence = SequenceNumber::mask - 1U,
            .time_to_live = 1};
        context.source = {.kind = MulticastSourceKind::announcement,
            .group_id = context.dispatch.group_id,
            .endpoint = {.address = {192, 0, 2, 1}, .port = 9000}};
        context.offer.supported = h::known_features;
        REQUIRE(h::negotiate(
            context.offer, context.offer, 71, 97, 1234, context.selection));
        grant.credential_id.fill(std::byte {0x11});
        grant.caller_group = group_handle_mask | 2U;
        grant.listener_group = context.dispatch.group_id;
        REQUIRE(grant.stream_id.assign("live/camera1"));
        grant.generation = 7;
        grant.expires_at_us = 1000;
        for (auto* action : {&caller, &listener}) {
            action->kind = HandshakeActionKind::send;
            action->packet.request = HandshakeRequest::conclusion;
            action->packet.initial_sequence =
                SequenceNumber {context.dispatch.initial_sequence};
            action->packet.extension_field = handshake_extension_flag_handshake
                | handshake_extension_flag_config;
            action->has_handshake_extension = true;
            action->extension_parameters.flags = 0x3f;
            action->extension_parameters.srt_version = 0x010507;
            action->has_group_membership = true;
            action->group_membership.type = GroupType::multicast;
            action->has_multicast_source = true;
        }
        caller.packet.socket_id = 107;
        caller.packet.syn_cookie = 123;
        caller.extension_type = HandshakeExtensionType::handshake_request;
        caller.group_membership.group_id = grant.caller_group;
        caller.has_stream_id_extension = true;
        caller.stream_id = grant.stream_id;
        caller.multicast_source.kind = MulticastSourceKind::offer;
        listener.packet.socket_id = 108;
        listener.extension_type = HandshakeExtensionType::handshake_response;
        listener.group_membership.group_id = grant.listener_group;
        listener.has_multicast_dispatch = true;
        listener.multicast_dispatch = context.dispatch;
        listener.multicast_source = context.source;
    }
};
std::vector<std::byte> payload(const HandshakeAction& action)
{
    std::array<std::byte, 1500> out {};
    const auto result =
        encode_handshake_datagram(action, PacketTimestamp {15}, 17, out);
    REQUIRE(result);
    return {out.begin() + packet_header_size,
        out.begin() + static_cast<std::ptrdiff_t>(result.bytes_written)};
}
bool transcript(const Fixture& f, h::AdmissionTranscript& out)
{
    return h::admission_transcript(
        f.context, f.grant, payload(f.caller), payload(f.listener), out);
}
std::vector<std::byte> maximum_payload(
    const HandshakeAction& action, bool caller)
{
    auto bytes = payload(action);
    const auto extra = h::maximum_conclusion_payload - bytes.size();
    // Group records intentionally allow future suffix bytes. The transcript
    // must retain them even though today's group decoder reads eight bytes.
    const auto group_offset = caller ? 80U : 64U;
    const auto words = (8U + extra) / 4U;
    bytes[group_offset + 2] = std::byte(words >> 8U);
    bytes[group_offset + 3] = std::byte(words);
    bytes.insert(bytes.begin() + group_offset + 12, extra, std::byte {0x5a});
    bytes[12] = std::byte {};
    bytes[13] = std::byte {};
    bytes[14] = std::byte {7};
    bytes[15] = std::byte {0xd0}; // MTU 2000
    return bytes;
}
}

TEST(
    hybrid_admission_transcript_owns_exact_payloads_lengths_context_and_credential_id)
{
    Fixture f;
    auto c = payload(f.caller), l = payload(f.listener);
    h::AdmissionTranscript t;
    REQUIRE(h::admission_transcript(f.context, f.grant, c, l, t));
    REQUIRE_EQ(t.size, h::admission_prefix_size + c.size() + l.size());
    REQUIRE_EQ(t.bytes[0], std::byte {'R'});
    REQUIRE_EQ(t.bytes[3], std::byte {1});
    REQUIRE(std::equal(f.grant.credential_id.begin(),
        f.grant.credential_id.end(), t.bytes.begin() + 16));
    REQUIRE(std::equal(
        c.begin(), c.end(), t.bytes.begin() + h::admission_prefix_size));
    const auto before = t.bytes;
    c[0] ^= std::byte {1};
    l[0] ^= std::byte {1};
    REQUIRE_EQ(t.bytes, before);
    REQUIRE_EQ(h::maximum_admission_size, 3192U);
}

TEST(hybrid_admission_transcript_failure_is_atomic_and_lengths_are_bounded)
{
    Fixture f;
    h::AdmissionTranscript t;
    REQUIRE(transcript(f, t));
    const auto before = t.bytes;
    const auto size = t.size;
    auto c = payload(f.caller), l = payload(f.listener);
    for (std::size_t n = 0; n < handshake_size; ++n) {
        REQUIRE(!h::admission_transcript(
            f.context, f.grant, std::span {c}.first(n), l, t));
        REQUIRE_EQ(t.bytes, before);
        REQUIRE_EQ(t.size, size);
    }
    c.resize(h::maximum_conclusion_payload + 1);
    REQUIRE(!h::admission_transcript(f.context, f.grant, c, l, t));
    REQUIRE_EQ(t.bytes, before);
}

TEST(
    hybrid_admission_transcript_requires_explicit_stream_and_both_group_authorizations)
{
    for (unsigned mutation = 0; mutation < 8; ++mutation) {
        Fixture f;
        switch (mutation) {
        case 0:
            f.grant.credential_id = {};
            break;
        case 1:
            f.grant.generation = 0;
            break;
        case 2:
            f.grant.expires_at_us = 0;
            break;
        case 3:
            f.grant.caller_group++;
            break;
        case 4:
            f.grant.listener_group++;
            break;
        case 5:
            REQUIRE(f.grant.stream_id.assign("live/camera2"));
            break;
        case 6:
            f.grant.stream_id.size = 513;
            break;
        case 7:
            f.grant.caller_group = 3;
            break;
        }
        h::AdmissionTranscript t;
        REQUIRE(!transcript(f, t));
    }
}

TEST(
    hybrid_admission_transcript_refuses_roles_reflection_source_and_sequence_mismatch)
{
    for (unsigned mutation = 0; mutation < 8; ++mutation) {
        Fixture f;
        switch (mutation) {
        case 0:
            f.caller.extension_type =
                HandshakeExtensionType::handshake_response;
            f.caller.has_multicast_source = false;
            break;
        case 1:
            f.listener.packet.socket_id = f.caller.packet.socket_id;
            break;
        case 2:
            f.caller.packet.socket_id = 0;
            break;
        case 3:
            f.listener.multicast_source.endpoint.port++;
            break;
        case 4:
            f.listener.multicast_dispatch.time_to_live++;
            break;
        case 5:
            f.caller.has_multicast_source = false;
            break;
        case 6:
            f.context.dispatch.initial_sequence = 1;
            break;
        case 7:
            f.listener.has_stream_id_extension = true;
            break;
        }
        h::AdmissionTranscript t;
        REQUIRE(!transcript(f, t));
    }
}

TEST(
    hybrid_admission_transcript_preserves_transport_cookie_address_and_extension_bytes)
{
    Fixture baseline;
    h::AdmissionTranscript original;
    REQUIRE(transcript(baseline, original));
    for (unsigned mutation = 0; mutation < 7; ++mutation) {
        Fixture f;
        switch (mutation) {
        case 0:
            f.caller.packet.maximum_transmission_unit--;
            break;
        case 1:
            f.listener.packet.flow_window--;
            break;
        case 2:
            f.caller.packet.syn_cookie++;
            break;
        case 3:
            f.listener.packet.peer_address[0] = 7;
            break;
        case 4:
            f.listener.extension_parameters.receiver_tsbpd_delay_milliseconds++;
            break;
        case 5:
            f.caller.extension_parameters.srt_version++;
            break;
        case 6:
            f.caller.has_path_identifier = true;
            f.caller.path_identifier.size = 1;
            f.caller.path_identifier.bytes[0] = std::byte {3};
            break;
        }
        h::AdmissionTranscript changed;
        REQUIRE(transcript(f, changed));
        REQUIRE(changed.bytes != original.bytes);
    }
}

TEST(
    hybrid_admission_transcript_unknown_extensions_and_native_auth7_are_refused)
{
    Fixture f;
    auto c = payload(f.caller), l = payload(f.listener);
    c.insert(c.end(),
        {std::byte {0x7e}, std::byte {0x7e}, std::byte {}, std::byte {1},
            std::byte {}, std::byte {}, std::byte {}, std::byte {}});
    h::AdmissionTranscript t;
    REQUIRE(!h::admission_transcript(f.context, f.grant, c, l, t));
    f.caller.has_session_authentication = true;
    f.caller.session_authentication.version = h::authentication_version;
    REQUIRE(!transcript(f, t));
    f.caller.session_authentication.version = session_authentication_version;
    REQUIRE(transcript(f, t));
}

TEST(hybrid_admission_maximum_payloads_preserve_ignored_group_suffixes)
{
    Fixture f;
    auto c = maximum_payload(f.caller, true),
         l = maximum_payload(f.listener, false);
    REQUIRE_EQ(c.size(), h::maximum_conclusion_payload);
    REQUIRE_EQ(l.size(), h::maximum_conclusion_payload);
    h::AdmissionTranscript t;
    REQUIRE(h::admission_transcript(f.context, f.grant, c, l, t));
    REQUIRE_EQ(t.size, h::maximum_admission_size);
    c[100] ^= std::byte {1};
    h::AdmissionTranscript changed;
    REQUIRE(h::admission_transcript(f.context, f.grant, c, l, changed));
    REQUIRE(t.bytes != changed.bytes);
}

#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_HYBRID_CONTROL
namespace {
class Provider final : public CryptoProvider {
public:
    unsigned next = 0, erased = 0;
    bool fail_random = false, fail_kdf = false, fail_mac = false;
    explicit Provider(unsigned seed)
        : next(seed)
    {
    }
    Error random_bytes(std::span<std::byte> out) noexcept override
    {
        if (fail_random)
            return Error::cryptographic_failure;
        for (auto& b : out)
            b = std::byte(next++);
        return Error::none;
    }
    Error pbkdf2_hmac_sha1(std::span<const std::byte> p,
        std::span<const std::byte> s, std::uint32_t n,
        std::span<std::byte> out) noexcept override
    {
        return fail_kdf
            ? Error::cryptographic_failure
            : default_crypto_provider().pbkdf2_hmac_sha1(p, s, n, out);
    }
    Error hmac_sha256(std::span<const std::byte> k,
        std::span<const std::byte> m,
        std::span<std::byte, 32> out) noexcept override
    {
        return fail_mac ? Error::cryptographic_failure
                        : default_crypto_provider().hmac_sha256(k, m, out);
    }
    Error wrap_key(std::span<const std::byte>, std::span<const std::byte>,
        std::span<std::byte>, std::size_t&) noexcept override
    {
        return Error::unsupported;
    }
    Error unwrap_key(std::span<const std::byte>, std::span<const std::byte>,
        std::span<std::byte>, std::size_t&) noexcept override
    {
        return Error::unsupported;
    }
    Error make_aes_ctr_cipher(std::span<const std::byte>,
        std::unique_ptr<PayloadCipher>&) noexcept override
    {
        return Error::unsupported;
    }
    bool supports_aes_gcm() const noexcept override
    {
        return false;
    }
    Error make_aes_gcm_cipher(std::span<const std::byte>,
        std::unique_ptr<AuthenticatedPayloadCipher>&) noexcept override
    {
        return Error::unsupported;
    }
    void secure_erase(std::span<std::byte> out) noexcept override
    {
        ++erased;
        default_crypto_provider().secure_erase(out);
    }
};
struct Pair {
    Fixture f;
    Provider cp {0}, lp {32};
    h::Admission caller {cp, true}, listener {lp, false};
    std::array<std::byte, 32> secret;
    Pair()
    {
        secret.fill(std::byte {0x22});
    }
    void prepare(const Fixture& left, const Fixture& right)
    {
        REQUIRE(caller.prepare(left.context, left.grant, payload(left.caller),
            payload(left.listener), 10));
        REQUIRE(listener.prepare(right.context, right.grant,
            payload(right.caller), payload(right.listener), 10));
    }
    void establish()
    {
        REQUIRE_EQ(listener.establish(
                       f.grant.credential_id, secret, caller.parameters(), 11),
            Error::none);
        REQUIRE_EQ(caller.establish(f.grant.credential_id, secret,
                       listener.parameters(), 11),
            Error::none);
    }
    void confirm()
    {
        SessionAuthenticationParameters c, l;
        REQUIRE_EQ(caller.proof(c), Error::none);
        REQUIRE_EQ(listener.proof(l), Error::none);
        REQUIRE(caller.confirm(l, f.grant, 12));
        REQUIRE(listener.confirm(c, f.grant, 12));
    }
};
static_assert(sizeof(h::Admission) < 8192);
}

TEST(
    hybrid_admission_releases_one_control_gate_only_after_two_role_proofs_and_final_grant)
{
    Pair p;
    p.prepare(p.f, p.f);
    REQUIRE(!p.caller.authentication());
    p.establish();
    REQUIRE(!p.caller.authentication());
    p.confirm();
    h::Session caller(*p.caller.authentication(), p.f.context, true);
    h::Session listener(*p.listener.authentication(), p.f.context, false);
    REQUIRE(caller.ready());
    REQUIRE(listener.ready());
    h::Session second(*p.caller.authentication(), p.f.context, true);
    REQUIRE(!second.ready());
    h::Message m {.kind = h::Kind::fallback_request,
        .stream = 71,
        .member = 97,
        .serial = 1,
        .next_position = 1234,
        .next_sequence = SequenceNumber::mask - 1,
        .mode = h::Mode::unicast};
    std::array<std::byte, 172> wire {};
    std::size_t n = 0;
    REQUIRE_EQ(caller.seal(m, PacketTimestamp {3}, wire, n),
        h::Session::Status::accepted);
    const auto decoded = decode_packet(wire);
    REQUIRE(decoded);
    REQUIRE_EQ(listener.receive(decoded.packet, 15).status,
        h::Session::Status::accepted);
    const auto erased = p.cp.erased;
    auto* raw = p.caller.authentication();
    auto peer_parameters = p.listener.parameters();
    p.caller.close();
    REQUIRE(p.cp.erased >= erased + 3);
    REQUIRE(!caller.ready());
    REQUIRE_EQ(caller.seal(m, PacketTimestamp {3}, wire, n),
        h::Session::Status::invalid);
    REQUIRE(!p.caller.authentication());
    std::array<std::byte, h::context_size> binding {};
    REQUIRE(h::encode(p.f.context, binding));
    REQUIRE_EQ(
        raw->establish(p.secret, peer_parameters, 107, 108, 0, 0, binding),
        Error::invalid_state);
    REQUIRE(!caller.ready());
}

TEST(
    hybrid_admission_revalidation_revokes_borrowed_gate_on_expiry_or_policy_change)
{
    for (const bool expired : {false, true}) {
        Pair p;
        p.prepare(p.f, p.f);
        p.establish();
        p.confirm();
        h::Session gate(*p.caller.authentication(), p.f.context, true);
        REQUIRE(gate.ready());
        REQUIRE(p.caller.revalidate(p.f.grant, 999));
        auto current = p.f.grant;
        if (!expired)
            current.generation++;
        REQUIRE(!p.caller.revalidate(current, 1000));
        REQUIRE(!gate.ready());
        REQUIRE(!p.caller.revalidate(p.f.grant, 999));
    }
}

TEST(hybrid_admission_grant_revocation_expiry_and_clock_regression_are_terminal)
{
    for (unsigned mutation = 0; mutation < 9; ++mutation) {
        Pair p;
        p.prepare(p.f, p.f);
        p.establish();
        SessionAuthenticationParameters proof;
        REQUIRE_EQ(p.listener.proof(proof), Error::none);
        auto current = p.f.grant;
        std::uint64_t now = 12;
        switch (mutation) {
        case 0:
            current.generation++;
            break;
        case 1:
            current.credential_id[0] ^= std::byte {1};
            break;
        case 2:
            current.caller_group++;
            break;
        case 3:
            current.listener_group++;
            break;
        case 4:
            REQUIRE(current.stream_id.assign("live/other"));
            break;
        case 5:
            current.expires_at_us++;
            break;
        case 6:
            now = current.expires_at_us;
            break;
        case 7:
            now = 10;
            break;
        case 8:
            now = UINT64_MAX;
            break;
        }
        REQUIRE(!p.caller.confirm(proof, current, now));
        REQUIRE_EQ(p.caller.state(), h::Admission::State::closed);
        REQUIRE(!p.caller.authentication());
        REQUIRE(!p.caller.prepare(p.f.context, p.f.grant, payload(p.f.caller),
            payload(p.f.listener), 12));
    }
}

TEST(
    hybrid_admission_different_valid_final_transcripts_cannot_confirm_each_other)
{
    for (unsigned mutation = 0; mutation < 9; ++mutation) {
        Pair p;
        auto other = p.f;
        switch (mutation) {
        case 0:
            other.caller.packet.maximum_transmission_unit--;
            break;
        case 1:
            other.listener.packet.flow_window--;
            break;
        case 2:
            other.caller.packet.syn_cookie++;
            break;
        case 3:
            other.listener.packet.peer_address[0]++;
            break;
        case 4:
            other.caller.extension_parameters
                .receiver_tsbpd_delay_milliseconds++;
            break;
        case 5:
            other.caller.has_path_identifier = true;
            other.caller.path_identifier.size = 1;
            break;
        case 6:
            REQUIRE(other.grant.stream_id.assign("live/camera2"));
            other.caller.stream_id = other.grant.stream_id;
            break;
        case 7:
            other.grant.caller_group++;
            other.caller.group_membership.group_id++;
            break;
        case 8:
            other.caller.has_session_authentication = true;
            break;
        }
        p.prepare(p.f, other);
        p.establish();
        SessionAuthenticationParameters proof;
        REQUIRE_EQ(p.listener.proof(proof), Error::none);
        REQUIRE(!p.caller.confirm(proof, p.f.grant, 12));
        REQUIRE(!p.caller.authentication());
    }
}

TEST(
    hybrid_admission_explicit_pairwise_secret_has_no_passphrase_or_data_key_fallback)
{
    for (unsigned mutation = 0; mutation < 4; ++mutation) {
        Pair p;
        p.prepare(p.f, p.f);
        auto id = p.f.grant.credential_id;
        std::span<const std::byte> secret = p.secret;
        switch (mutation) {
        case 0:
            secret = {};
            break;
        case 1:
            secret = secret.first(16);
            break;
        case 2:
            p.secret.fill(std::byte {});
            break;
        case 3:
            id[0] ^= std::byte {1};
            break;
        }
        REQUIRE_EQ(p.listener.establish(id, secret, p.caller.parameters(), 11),
            Error::invalid_key_material);
        REQUIRE_EQ(p.listener.state(), h::Admission::State::closed);
        REQUIRE(!p.listener.authentication());
    }
    Pair p;
    p.prepare(p.f, p.f);
    REQUIRE_EQ(p.listener.establish(p.f.grant.credential_id, p.secret,
                   p.caller.parameters(), 11),
        Error::none);
    p.secret[0] ^= std::byte {1};
    REQUIRE_EQ(p.caller.establish(p.f.grant.credential_id, p.secret,
                   p.listener.parameters(), 11),
        Error::none);
    SessionAuthenticationParameters proof;
    REQUIRE_EQ(p.listener.proof(proof), Error::none);
    REQUIRE(!p.caller.confirm(proof, p.f.grant, 12));
}

TEST(
    hybrid_admission_self_proofs_tampering_and_provider_failure_never_release_authentication)
{
    for (unsigned mutation = 0; mutation < 34; ++mutation) {
        Pair p;
        p.prepare(p.f, p.f);
        p.establish();
        SessionAuthenticationParameters proof;
        REQUIRE_EQ(
            (mutation == 32 ? p.caller : p.listener).proof(proof), Error::none);
        if (mutation < 32)
            proof.proof[mutation] ^= std::byte {1};
        if (mutation == 33)
            p.cp.fail_mac = true;
        REQUIRE(!p.caller.confirm(proof, p.f.grant, 12));
        REQUIRE_EQ(p.caller.state(), h::Admission::State::closed);
    }
    Pair p;
    p.prepare(p.f, p.f);
    p.establish();
    p.cp.fail_mac = true;
    SessionAuthenticationParameters sentinel;
    sentinel.proof.fill(std::byte {0x55});
    auto out = sentinel;
    REQUIRE_EQ(p.caller.proof(out), Error::cryptographic_failure);
    REQUIRE_EQ(out.proof, sentinel.proof);
    p.cp.fail_mac = false;
    p.confirm();
}

TEST(
    hybrid_admission_owned_snapshot_and_secret_are_not_borrowed_after_establishment)
{
    Pair p;
    auto c = payload(p.f.caller), l = payload(p.f.listener);
    REQUIRE(p.caller.prepare(p.f.context, p.f.grant, c, l, 10));
    REQUIRE(p.listener.prepare(p.f.context, p.f.grant, c, l, 10));
    c.assign(c.size(), std::byte {});
    l.assign(l.size(), std::byte {});
    p.establish();
    p.secret.fill(std::byte {});
    p.confirm();
    SessionAuthenticationParameters proof;
    REQUIRE_EQ(p.listener.proof(proof), Error::none);
    REQUIRE(p.caller.confirm(proof, p.f.grant, 13));
    auto revoked = p.f.grant;
    revoked.generation++;
    REQUIRE(!p.caller.confirm(proof, revoked, 14));
    REQUIRE(!p.caller.authentication());
}

TEST(hybrid_admission_prepare_and_derivation_failures_close_and_cannot_restart)
{
    for (unsigned mutation = 0; mutation < 4; ++mutation) {
        Pair p;
        if (mutation == 0) {
            p.cp.fail_random = true;
            REQUIRE(!p.caller.prepare(p.f.context, p.f.grant,
                payload(p.f.caller), payload(p.f.listener), 10));
        } else if (mutation == 1) {
            REQUIRE(!p.caller.prepare(p.f.context, p.f.grant,
                payload(p.f.caller), payload(p.f.listener), 1000));
        } else {
            p.prepare(p.f, p.f);
            p.lp.fail_kdf = mutation == 2;
            p.lp.fail_mac = mutation == 3;
            REQUIRE_EQ(p.listener.establish(p.f.grant.credential_id, p.secret,
                           p.caller.parameters(), 11),
                Error::cryptographic_failure);
            REQUIRE_EQ(p.listener.state(), h::Admission::State::closed);
            continue;
        }
        REQUIRE_EQ(p.caller.state(), h::Admission::State::closed);
    }
}

TEST(hybrid_admission_independent_python_hashlib_full_transcript_known_answer)
{
    Pair p;
    p.prepare(p.f, p.f);
    p.establish();
    h::AdmissionTranscript t;
    REQUIRE(transcript(p.f, t));
    REQUIRE_EQ(t.size, 528U);
    SessionAuthenticationParameters caller, listener;
    REQUIRE_EQ(p.caller.proof(caller), Error::none);
    REQUIRE_EQ(p.listener.proof(listener), Error::none);
    const auto hex = [](const auto& bytes) {
        std::string text;
        for (auto b : bytes) {
            const auto n = std::to_integer<unsigned>(b);
            text.push_back("0123456789abcdef"[n >> 4]);
            text.push_back("0123456789abcdef"[n & 15]);
        }
        return text;
    };
    REQUIRE_EQ(hex(caller.proof),
        "98f0ee720b2a5be414b0ad2700c0e73b3cd191d47e6f399e6a3ea2e218ea051b");
    REQUIRE_EQ(hex(listener.proof),
        "23462deb1e8aa8e01f8088b7b838f626f014d09436cf038d42dbe14c680985d0");
    p.confirm();
}

TEST(
    hybrid_admission_maximum_transcript_proofs_confirm_without_expanding_legacy_limits)
{
    Pair p;
    const auto c = maximum_payload(p.f.caller, true),
               l = maximum_payload(p.f.listener, false);
    REQUIRE(p.caller.prepare(p.f.context, p.f.grant, c, l, 10));
    REQUIRE(p.listener.prepare(p.f.context, p.f.grant, c, l, 10));
    p.establish();
    p.confirm();
    std::array<std::byte, h::maximum_admission_size + 1> oversized {};
    SessionAuthenticationParameters bounded;
    REQUIRE_EQ(
        p.caller.authentication()->handshake_proof(true, oversized, bounded),
        Error::invalid_state);
    // Ordinary authentication retains the exact original 128-byte proof limit.
    SessionAuthentication a(p.cp), b(p.lp);
    REQUIRE_EQ(a.start(true), Error::none);
    REQUIRE_EQ(b.start(false), Error::none);
    REQUIRE_EQ(b.establish(p.secret, a.parameters(), 108, 107), Error::none);
    REQUIRE_EQ(a.establish(p.secret, b.parameters(), 107, 108), Error::none);
    std::array<std::byte, 129> material {};
    SessionAuthenticationParameters proof;
    REQUIRE_EQ(a.handshake_proof(true, material, proof), Error::invalid_state);
    REQUIRE_EQ(a.handshake_proof(true, std::span {material}.first(128), proof),
        Error::none);
    a.revoke_hybrid_control();
    REQUIRE(a.ready());
}
#endif
