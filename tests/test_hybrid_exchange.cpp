// SPDX-License-Identifier: MIT
#include "test.hpp"
#include "compat/hybrid_exchange.hpp"
#include "robotweax/srt/handshake_datagram.hpp"

#include <algorithm>
#include <vector>

using namespace robotweax::srt;
namespace h = robotweax::srt::compat::hybrid;
namespace {
h::ExchangeRecord record(h::ExchangeKind kind)
{
    h::ExchangeRecord r {
        .kind = kind, .caller_socket = 107, .listener_socket = 108};
    r.credential_id.fill(std::byte {0x11});
    r.authentication.version = h::authentication_version;
    r.authentication.caller_nonce.fill(std::byte {1});
    h::Capability offer;
    offer.supported = h::known_features;
    if (kind == h::ExchangeKind::offer)
        r.capability = offer;
    else {
        r.authentication.listener_nonce.fill(std::byte {2});
        r.authentication.proof.fill(std::byte {3});
        if (kind == h::ExchangeKind::selection)
            REQUIRE(h::negotiate(offer, offer, 71, 97, 1234, r.capability));
    }
    return r;
}
std::array<std::byte, h::exchange_size> wire(const h::ExchangeRecord& r)
{
    std::array<std::byte, h::exchange_size> bytes {};
    REQUIRE(h::encode(r, bytes));
    return bytes;
}
}

TEST(hybrid_exchange_four_records_round_trip_with_strict_fixed_layout)
{
    for (unsigned i = 1; i <= 4; ++i) {
        const auto r = record(static_cast<h::ExchangeKind>(i));
        const auto bytes = wire(r);
        REQUIRE_EQ(bytes.size(), 176U);
        REQUIRE_EQ(bytes[0], std::byte {'R'});
        REQUIRE_EQ(bytes[3], std::byte {1});
        REQUIRE_EQ(bytes[7], std::byte(i));
        REQUIRE_EQ(bytes[11], std::byte {107});
        REQUIRE_EQ(bytes[15], std::byte {108});
        h::ExchangeRecord parsed;
        REQUIRE(h::decode(bytes, parsed));
        REQUIRE_EQ(wire(parsed), bytes);
        if (i >= 3)
            REQUIRE(
                std::all_of(bytes.begin() + 32, bytes.begin() + 80, [](auto b) {
                    return b == std::byte {};
                }));
    }
}
TEST(hybrid_exchange_codec_exact_size_and_atomic_encoding)
{
    const auto r = record(h::ExchangeKind::selection);
    const auto bytes = wire(r);
    h::ExchangeRecord parsed;
    for (std::size_t n = 0; n < bytes.size(); ++n) {
        REQUIRE(!h::decode(std::span {bytes}.first(n), parsed));
        auto sentinel = bytes;
        REQUIRE(!h::encode(r, std::span {sentinel}.first(n)));
        REQUIRE_EQ(sentinel, bytes);
    }
    std::array<std::byte, h::exchange_size + 1> extra {};
    REQUIRE(!h::decode(extra, parsed));
    REQUIRE(!h::encode(r, extra));
    auto invalid = r;
    invalid.caller_socket = 0;
    auto sentinel = bytes;
    REQUIRE(!h::encode(invalid, sentinel));
    REQUIRE_EQ(sentinel, bytes);
}
TEST(hybrid_exchange_codec_refuses_unused_fields_and_wrong_phase_shapes)
{
    for (unsigned mutation = 0; mutation < 9; ++mutation) {
        auto r = record(h::ExchangeKind::offer);
        switch (mutation) {
        case 0:
            r.authentication.listener_nonce[0] = std::byte {1};
            break;
        case 1:
            r.authentication.proof[0] = std::byte {1};
            break;
        case 2:
            r.authentication.caller_nonce = {};
            break;
        case 3:
            r.credential_id = {};
            break;
        case 4:
            r.listener_socket = r.caller_socket;
            break;
        case 5:
            r.authentication.version = 2;
            break;
        case 6:
            r.kind = static_cast<h::ExchangeKind>(5);
            break;
        case 7:
            r.capability.kind = h::CapabilityKind::selection;
            break;
        case 8:
            r.kind = h::ExchangeKind::proof;
            break;
        }
        std::array<std::byte, h::exchange_size> out {};
        REQUIRE(!h::encode(r, out));
    }
    auto bytes = wire(record(h::ExchangeKind::proof));
    h::ExchangeRecord parsed;
    for (std::size_t n = 32; n < 80; ++n) {
        auto changed = bytes;
        changed[n] = std::byte {1};
        REQUIRE(!h::decode(changed, parsed));
    }
}

#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_HYBRID_CONTROL
namespace {
struct Fixture {
    h::Exchange::Configuration caller_configuration, listener_configuration;
    h::Authorization grant;
    std::vector<std::byte> caller_payload, listener_payload;
    Fixture()
    {
        auto& c = caller_configuration;
        c.dispatch = {.group_id = group_handle_mask | 1U,
            .multicast_group = {.address = {239, 255, 1, 1}, .port = 6000},
            .initial_sequence = SequenceNumber::mask - 1U,
            .time_to_live = 1};
        c.source = {.kind = MulticastSourceKind::announcement,
            .group_id = c.dispatch.group_id,
            .endpoint = {.address = {192, 0, 2, 1}, .port = 9000}};
        c.policy.supported = h::known_features;
        c.credential_id.fill(std::byte {0x11});
        c.established_peer = IpEndpoint::loopback(8000);
        c.connection_generation = 123;
        c.retry_interval_us = 100;
        c.timeout_us = 1000;
        listener_configuration = c;
        listener_configuration.established_peer.port = 8001;
        listener_configuration.stream = 71;
        listener_configuration.member = 97;
        listener_configuration.initial_position = 1234;
        grant = {.credential_id = c.credential_id,
            .caller_group = group_handle_mask | 2U,
            .listener_group = c.dispatch.group_id,
            .generation = 7,
            .expires_at_us = 10000};
        REQUIRE(grant.stream_id.assign("live/camera1"));
        for (const bool caller : {true, false}) {
            HandshakeAction a;
            a.kind = HandshakeActionKind::send;
            a.packet.request = HandshakeRequest::conclusion;
            a.packet.initial_sequence =
                SequenceNumber {c.dispatch.initial_sequence};
            a.packet.socket_id = caller ? 107 : 108;
            a.packet.syn_cookie = caller ? 123 : 0;
            a.packet.extension_field = handshake_extension_flag_handshake
                | handshake_extension_flag_config;
            a.has_handshake_extension = true;
            a.extension_type = caller
                ? HandshakeExtensionType::handshake_request
                : HandshakeExtensionType::handshake_response;
            a.extension_parameters.flags = 0x3f;
            a.extension_parameters.srt_version = 0x010507;
            a.has_group_membership = true;
            a.group_membership = {
                .group_id = caller ? grant.caller_group : grant.listener_group,
                .type = GroupType::multicast};
            a.has_multicast_source = true;
            if (caller) {
                a.has_stream_id_extension = true;
                a.stream_id = grant.stream_id;
                a.multicast_source.kind = MulticastSourceKind::offer;
            } else {
                a.has_multicast_dispatch = true;
                a.multicast_dispatch = c.dispatch;
                a.multicast_source = c.source;
            }
            std::array<std::byte, 1500> out {};
            const auto e =
                encode_handshake_datagram(a, PacketTimestamp {}, 0, out);
            REQUIRE(e);
            (caller ? caller_payload : listener_payload)
                .assign(out.begin() + packet_header_size,
                    out.begin() + static_cast<std::ptrdiff_t>(e.bytes_written));
        }
    }
};
class Crypto final : public CryptoProvider {
public:
    unsigned next = 0, random_calls = 0, kdf_calls = 0, mac_calls = 0,
             secret_erases = 0;
    bool fail_random = false, fail_kdf = false, fail_mac = false;
    explicit Crypto(unsigned seed)
        : next(seed)
    {
    }
    Error random_bytes(std::span<std::byte> out) noexcept override
    {
        ++random_calls;
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
        ++kdf_calls;
        return fail_kdf
            ? Error::cryptographic_failure
            : default_crypto_provider().pbkdf2_hmac_sha1(p, s, n, out);
    }
    Error hmac_sha256(std::span<const std::byte> k,
        std::span<const std::byte> m,
        std::span<std::byte, 32> out) noexcept override
    {
        ++mac_calls;
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
        if (out.size() == 32 && std::all_of(out.begin(), out.end(), [](auto b) {
                return b == std::byte {0x22};
            }))
            ++secret_erases;
        default_crypto_provider().secure_erase(out);
    }
};
struct Resolver {
    h::Authorization grant;
    unsigned resolves = 0, revalidates = 0;
    bool allow = true, current = true, zero_secret = false,
         invalidate_after_resolve = false;
    h::CredentialRequest seen;
    h::Exchange* close_during_resolve = nullptr;
    h::Exchange* close_during_revalidate = nullptr;
    static bool resolve(void* p, const h::CredentialRequest& r,
        h::Authorization& out, std::span<std::byte, 32> secret) noexcept
    {
        auto& self = *static_cast<Resolver*>(p);
        ++self.resolves;
        self.seen = r;
        out = self.grant;
        std::fill(secret.begin(), secret.end(),
            self.zero_secret ? std::byte {} : std::byte {0x22});
        if (self.invalidate_after_resolve)
            ++self.grant.generation;
        if (self.close_during_resolve)
            self.close_during_resolve->close();
        return self.allow;
    }
    static bool revalidate(void* p, const h::CredentialRequest& r,
        const h::Authorization&, h::Authorization& out) noexcept
    {
        auto& self = *static_cast<Resolver*>(p);
        ++self.revalidates;
        self.seen = r;
        out = self.grant;
        if (self.close_during_revalidate)
            self.close_during_revalidate->close();
        return self.current;
    }
    h::CredentialProvider provider()
    {
        return {.context = this, .resolve = resolve, .revalidate = revalidate};
    }
};
struct Pair {
    Fixture f;
    Crypto cp {0}, lp {32};
    Resolver cr, lr;
    h::Exchange caller {cp, true}, listener {lp, false};
    h::Exchange::Step offer, selection, proof, receipt;
    Pair()
    {
        cr.grant = f.grant;
        lr.grant = f.grant;
    }
    void start()
    {
        REQUIRE(!listener
                .start(f.listener_configuration, lr.provider(),
                    f.caller_payload, f.listener_payload, 0)
                .has_output);
        offer = caller.start(f.caller_configuration, cr.provider(),
            f.caller_payload, f.listener_payload, 0);
        REQUIRE(offer.has_output);
    }
    void select()
    {
        selection = listener.receive(offer.output, 1);
        REQUIRE(selection.has_output);
    }
    void respond()
    {
        proof = caller.receive(selection.output, 2);
        REQUIRE(proof.has_output);
    }
    void acknowledge()
    {
        receipt = listener.receive(proof.output, 3);
        REQUIRE(receipt.has_output);
    }
    void finish()
    {
        REQUIRE_EQ(caller.receive(receipt.output, 4).status,
            h::Exchange::Status::active);
    }
    void connect()
    {
        start();
        select();
        respond();
        acknowledge();
        finish();
    }
};
static_assert(sizeof(h::Exchange) <= 16 * 1024);
}

TEST(
    hybrid_exchange_four_steps_confirm_both_grants_without_consuming_control_counters)
{
    Pair p;
    p.start();
    REQUIRE(!p.caller.authentication());
    REQUIRE(!p.listener.authentication());
    p.select();
    REQUIRE(!p.listener.authentication());
    p.respond();
    REQUIRE(!p.caller.authentication());
    p.acknowledge();
    REQUIRE(p.listener.authentication());
    REQUIRE(!p.caller.authentication());
    p.finish();
    REQUIRE(p.caller.authentication());
    REQUIRE_EQ(p.cr.resolves, 1U);
    REQUIRE_EQ(p.lr.resolves, 1U);
    REQUIRE_EQ(p.cp.kdf_calls, 1U);
    REQUIRE_EQ(p.lp.kdf_calls, 1U);
    REQUIRE_EQ(p.cp.secret_erases, 1U);
    REQUIRE_EQ(p.lp.secret_erases, 1U);
    REQUIRE_EQ(p.caller.context(), p.listener.context());
    h::Session c(*p.caller.authentication(), p.caller.context(), true);
    h::Session l(*p.listener.authentication(), p.listener.context(), false);
    REQUIRE(c.ready());
    REQUIRE(l.ready());
    h::Message m {.kind = h::Kind::fallback_request,
        .stream = 71,
        .member = 97,
        .serial = 1,
        .next_position = 1234,
        .next_sequence = SequenceNumber::mask - 1,
        .mode = h::Mode::unicast};
    std::array<std::byte, 172> bytes {};
    std::size_t n = 0;
    REQUIRE_EQ(
        c.seal(m, PacketTimestamp {1}, bytes, n), h::Session::Status::accepted);
    const auto decoded = decode_packet(bytes);
    REQUIRE(decoded);
    REQUIRE_EQ(
        l.receive(decoded.packet, 5).status, h::Session::Status::accepted);
}
TEST(
    hybrid_exchange_each_lost_record_recovers_with_identical_bounded_retransmission)
{
    for (unsigned lost = 0; lost < 4; ++lost) {
        Pair p;
        p.start();
        auto offer = p.offer;
        if (lost == 0) {
            offer = p.caller.tick(100);
            REQUIRE_EQ(offer.output, p.offer.output);
        }
        auto selection = p.listener.receive(offer.output, 101);
        REQUIRE(selection.has_output);
        if (lost == 1) {
            const auto retry = p.listener.tick(201);
            REQUIRE_EQ(retry.output, selection.output);
            selection = retry;
        }
        auto proof = p.caller.receive(selection.output, 202);
        REQUIRE(proof.has_output);
        if (lost == 2) {
            const auto retry = p.caller.tick(302);
            REQUIRE_EQ(retry.output, proof.output);
            proof = retry;
        }
        auto receipt = p.listener.receive(proof.output, 303);
        REQUIRE(receipt.has_output);
        if (lost == 3) {
            const auto duplicate = p.caller.tick(403);
            REQUIRE_EQ(duplicate.output, proof.output);
            const auto retry = p.listener.receive(duplicate.output, 403);
            REQUIRE_EQ(retry.output, receipt.output);
            receipt = retry;
        }
        REQUIRE_EQ(p.caller.receive(receipt.output, 404).status,
            h::Exchange::Status::active);
        REQUIRE(p.caller.authentication());
        REQUIRE(p.listener.authentication());
        REQUIRE_EQ(p.cp.kdf_calls, 1U);
        REQUIRE_EQ(p.lp.kdf_calls, 1U);
        REQUIRE_EQ(p.cr.resolves, 1U);
        REQUIRE_EQ(p.lr.resolves, 1U);
    }
}
TEST(
    hybrid_exchange_duplicate_flood_cannot_accelerate_retries_or_repeat_credential_resolution)
{
    Pair p;
    p.start();
    p.select();
    for (unsigned i = 0; i < 2000; ++i) {
        const auto r = p.listener.receive(p.offer.output, 1);
        REQUIRE(!r.has_output);
        REQUIRE_EQ(r.status, h::Exchange::Status::duplicate);
    }
    REQUIRE_EQ(p.lr.resolves, 1U);
    REQUIRE_EQ(p.lp.kdf_calls, 1U);
    REQUIRE_EQ(p.lp.random_calls, 1U);
    REQUIRE_EQ(p.listener.tick(100).status, h::Exchange::Status::waiting);
    REQUIRE_EQ(p.listener.tick(101).output, p.selection.output);
    p.proof = p.caller.receive(p.selection.output, 102);
    REQUIRE(p.proof.has_output);
    p.receipt = p.listener.receive(p.proof.output, 103);
    REQUIRE(p.receipt.has_output);
    REQUIRE_EQ(p.caller.receive(p.receipt.output, 104).status,
        h::Exchange::Status::active);
}
TEST(
    hybrid_exchange_emission_budget_and_setup_deadline_are_exclusive_and_terminal)
{
    Pair p;
    p.f.caller_configuration.maximum_sends_per_stage = 2;
    p.start();
    REQUIRE_EQ(p.caller.next_deadline(), 100U);
    REQUIRE(!p.caller.tick(99).has_output);
    REQUIRE(p.caller.tick(100).has_output);
    REQUIRE_EQ(p.caller.tick(200).status, h::Exchange::Status::waiting);
    REQUIRE_EQ(p.caller.next_deadline(), 1000U);
    REQUIRE_EQ(p.caller.tick(1000).status, h::Exchange::Status::closed);
    REQUIRE_EQ(p.caller.next_deadline(), UINT64_MAX);
    REQUIRE(!p.caller.authentication());
    Pair q;
    q.start();
    REQUIRE_EQ(q.listener.next_deadline(), 1000U);
    REQUIRE_EQ(q.listener.receive(q.offer.output, 1000).status,
        h::Exchange::Status::closed);
    REQUIRE_EQ(q.lr.resolves, 0U);
}
TEST(hybrid_exchange_codec_or_identity_failures_do_not_poison_the_attempt)
{
    Pair p;
    p.start();
    for (std::size_t n = 0; n < h::exchange_size; ++n)
        REQUIRE_EQ(
            p.listener.receive(std::span {p.offer.output}.first(n), 0).status,
            h::Exchange::Status::ignored);
    auto changed = record(h::ExchangeKind::offer);
    changed.credential_id[0] = std::byte {9};
    REQUIRE_EQ(p.listener.receive(wire(changed), 0).status,
        h::Exchange::Status::ignored);
    REQUIRE_EQ(p.lr.resolves, 0U);
    p.select();
    p.respond();
    p.acknowledge();
    p.finish();
}
TEST(
    hybrid_exchange_all_four_records_reject_every_single_byte_mutation_before_activation)
{
    for (unsigned stage = 0; stage < 4; ++stage) {
        for (std::size_t i = 0; i < h::exchange_size; ++i) {
            Pair p;
            p.start();
            if (stage >= 1)
                p.select();
            if (stage >= 2)
                p.respond();
            if (stage >= 3)
                p.acknowledge();
            auto changed = (stage == 0 ? p.offer
                    : stage == 1       ? p.selection
                    : stage == 2       ? p.proof
                                       : p.receipt)
                               .output;
            changed[i] ^= std::byte {1};
            if (stage == 0) {
                auto s = p.listener.receive(changed, 1);
                if (s.has_output) {
                    auto r = p.caller.receive(s.output, 2);
                    if (r.has_output)
                        (void)p.listener.receive(r.output, 3);
                }
                REQUIRE(!p.caller.authentication());
                REQUIRE(!p.listener.authentication());
            } else if (stage == 1) {
                (void)p.caller.receive(changed, 2);
                REQUIRE(!p.caller.authentication());
            } else if (stage == 2) {
                (void)p.listener.receive(changed, 3);
                REQUIRE(!p.listener.authentication());
            } else {
                (void)p.caller.receive(changed, 4);
                REQUIRE(!p.caller.authentication());
            }
        }
    }
}
TEST(hybrid_exchange_reflection_reordering_and_old_attempts_cannot_activate)
{
    Pair p;
    p.start();
    REQUIRE_EQ(p.caller.receive(p.offer.output, 0).status,
        h::Exchange::Status::ignored);
    p.select();
    REQUIRE_EQ(p.listener.receive(p.selection.output, 1).status,
        h::Exchange::Status::ignored);
    p.respond();
    // An ordinary Listener transcript proof is not a receipt for Caller proof.
    h::ExchangeRecord fake;
    REQUIRE(h::decode(p.selection.output, fake));
    fake.kind = h::ExchangeKind::receipt;
    fake.capability = {.supported = 0, .required = 0, .prefix_limit = 0};
    REQUIRE_EQ(
        p.caller.receive(wire(fake), 3).status, h::Exchange::Status::closed);
    Pair q;
    q.cp.next = 64;
    q.start();
    REQUIRE_EQ(q.caller.receive(p.selection.output, 1).status,
        h::Exchange::Status::ignored);
    q.select();
    q.respond();
    q.acknowledge();
    q.finish();
}
TEST(
    hybrid_exchange_provider_denial_malformed_grants_and_zero_keys_have_no_fallback)
{
    for (unsigned mutation = 0; mutation < 7; ++mutation) {
        Pair p;
        switch (mutation) {
        case 0:
            p.lr.allow = false;
            break;
        case 1:
            p.lr.zero_secret = true;
            break;
        case 2:
            p.lr.grant.credential_id = {};
            break;
        case 3:
            p.lr.grant.caller_group++;
            break;
        case 4:
            REQUIRE(p.lr.grant.stream_id.assign("unauthorized"));
            break;
        case 5:
            p.lr.grant.expires_at_us = 1;
            break;
        case 6:
            p.lr.current = false;
            break;
        }
        p.start();
        REQUIRE_EQ(p.listener.receive(p.offer.output, 1).status,
            h::Exchange::Status::closed);
        REQUIRE_EQ(p.lr.resolves, 1U);
        REQUIRE(!p.listener.authentication());
        if (!p.lr.zero_secret)
            REQUIRE_EQ(p.lp.secret_erases, 1U);
    }
}
TEST(hybrid_exchange_provider_uses_frozen_local_provenance_and_payload_metadata)
{
    Pair p;
    p.start();
    p.f.caller_configuration.established_peer.port = 99;
    p.f.listener_configuration.connection_generation = 999;
    p.f.caller_payload.assign(p.f.caller_payload.size(), std::byte {});
    p.select();
    p.respond();
    p.acknowledge();
    p.finish();
    REQUIRE_EQ(p.cr.seen.established_peer.port, 8000U);
    REQUIRE_EQ(p.lr.seen.established_peer.port, 8001U);
    REQUIRE_EQ(p.lr.seen.connection_generation, 123U);
    REQUIRE(p.cr.seen.caller);
    REQUIRE(!p.lr.seen.caller);
    REQUIRE_EQ(p.cr.seen.caller_socket, 107U);
    REQUIRE_EQ(p.cr.seen.listener_socket, 108U);
    REQUIRE_EQ(p.cr.seen.stream_id.view(), "live/camera1");
    REQUIRE_EQ(p.cr.seen.context, p.caller.context());
}
TEST(
    hybrid_exchange_final_grant_change_expiry_and_clock_regression_revoke_roots)
{
    for (unsigned phase = 0; phase < 4; ++phase) {
        Pair p;
        p.start();
        p.select();
        if (phase == 0) {
            p.cr.invalidate_after_resolve = true;
            REQUIRE_EQ(p.caller.receive(p.selection.output, 2).status,
                h::Exchange::Status::closed);
        } else if (phase == 1) {
            p.respond();
            p.lr.grant.generation++;
            REQUIRE_EQ(p.listener.receive(p.proof.output, 3).status,
                h::Exchange::Status::closed);
        } else {
            p.respond();
            p.acknowledge();
            p.finish();
            h::Session gate(
                *p.caller.authentication(), p.caller.context(), true);
            REQUIRE(gate.ready());
            const auto now = phase == 2 ? 10000U : 3U;
            REQUIRE_EQ(p.caller.tick(now).status, h::Exchange::Status::closed);
            REQUIRE(!gate.ready());
        }
    }
}
TEST(
    hybrid_exchange_reentrant_callback_close_and_crypto_failures_never_publish_output)
{
    for (unsigned failure = 0; failure < 6; ++failure) {
        Pair p;
        p.start();
        switch (failure) {
        case 0:
            p.lr.close_during_resolve = &p.listener;
            break;
        case 1:
            p.lr.close_during_revalidate = &p.listener;
            break;
        case 2:
            p.lp.fail_random = true;
            break;
        case 3:
            p.lp.fail_kdf = true;
            break;
        case 4:
            p.lp.fail_mac = true;
            break;
        case 5:
            p.lr.grant.generation = 0;
            break;
        }
        const auto result = p.listener.receive(p.offer.output, 1);
        REQUIRE_EQ(result.status, h::Exchange::Status::closed);
        REQUIRE(!result.has_output);
        REQUIRE(!p.listener.authentication());
        REQUIRE_EQ(p.lp.secret_erases, 1U);
    }
}
TEST(
    hybrid_exchange_retries_after_activation_are_bounded_and_cannot_reset_root_or_window)
{
    Pair p;
    p.connect();
    auto* authentication = p.listener.authentication();
    h::Session gate(*authentication, p.listener.context(), false);
    REQUIRE(gate.ready());
    for (std::uint64_t now = 103; now <= 303; now += 100) {
        const auto step = p.listener.receive(p.proof.output, now);
        REQUIRE(step.has_output);
        REQUIRE_EQ(step.output, p.receipt.output);
    }
    REQUIRE(!p.listener.receive(p.proof.output, 403).has_output);
    REQUIRE_EQ(p.listener.tick(1000).status, h::Exchange::Status::active);
    REQUIRE_EQ(p.listener.next_deadline(), 10000U);
    REQUIRE_EQ(p.listener.authentication(), authentication);
    REQUIRE(gate.ready());
    REQUIRE_EQ(p.lr.resolves, 1U);
    REQUIRE_EQ(p.lp.kdf_calls, 1U);
    h::Session duplicate(*authentication, p.listener.context(), false);
    REQUIRE(!duplicate.ready());
    p.listener.close();
    REQUIRE(!gate.ready());
}
TEST(
    hybrid_exchange_invalid_configuration_and_required_feature_mismatch_fail_explicitly)
{
    for (unsigned mutation = 0; mutation < 9; ++mutation) {
        Pair p;
        switch (mutation) {
        case 0:
            p.f.caller_configuration.connection_generation = 0;
            break;
        case 1:
            p.f.caller_configuration.established_peer = IpEndpoint::any(99);
            break;
        case 2:
            p.f.caller_configuration.credential_id = {};
            break;
        case 3:
            p.f.caller_configuration.maximum_sends_per_stage = 9;
            break;
        case 4:
            p.f.caller_configuration.retry_interval_us = 0;
            break;
        case 5:
            p.f.caller_configuration.timeout_us = h::maximum_age_us + 1;
            break;
        case 6:
            p.f.caller_configuration.stream = 1;
            break;
        case 7:
            p.f.caller_payload[0] = std::byte {1};
            break;
        case 8:
            p.f.caller_configuration.policy.required = h::known_features + 1;
            break;
        }
        auto result = p.caller.start(p.f.caller_configuration, p.cr.provider(),
            p.f.caller_payload, p.f.listener_payload, 0);
        REQUIRE_EQ(result.status, h::Exchange::Status::closed);
        REQUIRE_EQ(p.cr.resolves, 0U);
    }
    Pair p;
    p.f.caller_configuration.policy.required |= h::source_recovery;
    p.f.listener_configuration.policy.supported = h::core_features;
    p.start();
    REQUIRE_EQ(p.listener.receive(p.offer.output, 1).status,
        h::Exchange::Status::closed);
    REQUIRE_EQ(p.lr.resolves, 0U);
}
TEST(
    hybrid_exchange_extreme_deadlines_saturate_without_wrapping_or_renewing_attempts)
{
    Pair p;
    auto result = p.caller.start(p.f.caller_configuration, p.cr.provider(),
        p.f.caller_payload, p.f.listener_payload, UINT64_MAX);
    REQUIRE_EQ(result.status, h::Exchange::Status::closed);
    Pair q;
    result = q.caller.start(q.f.caller_configuration, q.cr.provider(),
        q.f.caller_payload, q.f.listener_payload, UINT64_MAX - 50);
    REQUIRE(result.has_output);
    REQUIRE_EQ(q.caller.next_deadline(), UINT64_MAX);
    REQUIRE_EQ(q.caller.tick(UINT64_MAX).status, h::Exchange::Status::closed);
    Pair r;
    r.start();
    REQUIRE_EQ(r.caller
                   .start(r.f.caller_configuration, r.cr.provider(),
                       r.f.caller_payload, r.f.listener_payload, 0)
                   .status,
        h::Exchange::Status::ignored);
    REQUIRE_EQ(r.cp.random_calls, 1U);
    REQUIRE_EQ(r.caller.next_deadline(), 100U);
}
TEST(
    hybrid_exchange_optional_features_and_prefix_limit_are_bilaterally_selected)
{
    Pair p;
    p.f.listener_configuration.policy.supported = h::core_features | h::overlap;
    p.f.listener_configuration.policy.prefix_limit = 17;
    p.connect();
    REQUIRE_EQ(
        p.caller.context().selection.supported, h::core_features | h::overlap);
    REQUIRE_EQ(p.caller.context().selection.prefix_limit, 17U);
    REQUIRE_EQ(p.caller.context(), p.listener.context());
    REQUIRE_EQ(p.cr.seen.context, p.caller.context());
}
TEST(
    hybrid_exchange_last_emission_response_can_arrive_after_retry_budget_is_spent)
{
    Pair p;
    p.f.caller_configuration.maximum_sends_per_stage = 1;
    p.f.listener_configuration.maximum_sends_per_stage = 1;
    p.start();
    REQUIRE(!p.caller.tick(100).has_output);
    p.selection = p.listener.receive(p.offer.output, 200);
    REQUIRE(p.selection.has_output);
    REQUIRE(!p.listener.tick(300).has_output);
    p.proof = p.caller.receive(p.selection.output, 400);
    REQUIRE(p.proof.has_output);
    REQUIRE(!p.caller.tick(500).has_output);
    p.receipt = p.listener.receive(p.proof.output, 600);
    REQUIRE(p.receipt.has_output);
    REQUIRE_EQ(p.caller.receive(p.receipt.output, 700).status,
        h::Exchange::Status::active);
}
TEST(
    hybrid_exchange_independent_python_known_answers_cover_all_four_records_and_receipt_domain)
{
    Pair p;
    p.connect();
    const std::array expected {
        "52575801000000010000006b0000006c11111111111111111111111111111111"
        "00000001000000010000001f0000000700000100000000000000000000000000"
        "00000000000000000000000000000000000102030405060708090a0b0c0d0e0f"
        "101112131415161718191a1b1c1d1e1f00000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "00000000000000000000000000000000",
        "52575801000000020000006b0000006c11111111111111111111111111111111"
        "00000001000000020000001f0000000700000100000000000000000000000047"
        "000000000000006100000000000004d2000102030405060708090a0b0c0d0e0f"
        "101112131415161718191a1b1c1d1e1f202122232425262728292a2b2c2d2e2f"
        "303132333435363738393a3b3c3d3e3f23462deb1e8aa8e01f8088b7b838f626f"
        "014d09436cf038d42dbe14c680985d0",
        "52575801000000030000006b0000006c11111111111111111111111111111111"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "00000000000000000000000000000000000102030405060708090a0b0c0d0e0f"
        "101112131415161718191a1b1c1d1e1f202122232425262728292a2b2c2d2e2f"
        "303132333435363738393a3b3c3d3e3f98f0ee720b2a5be414b0ad2700c0e73b3"
        "cd191d47e6f399e6a3ea2e218ea051b",
        "52575801000000040000006b0000006c11111111111111111111111111111111"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "00000000000000000000000000000000000102030405060708090a0b0c0d0e0f"
        "101112131415161718191a1b1c1d1e1f202122232425262728292a2b2c2d2e2f"
        "303132333435363738393a3b3c3d3e3f8f1b13d9bef5f6aaa31c89d8bf55783d8"
        "cb698cb1251edacd6d7bd8a8e3cc9ba"};
    const std::array steps {p.offer, p.selection, p.proof, p.receipt};
    for (std::size_t i = 0; i < steps.size(); ++i) {
        std::string hex;
        for (auto b : steps[i].output) {
            const auto n = std::to_integer<unsigned>(b);
            hex.push_back("0123456789abcdef"[n >> 4]);
            hex.push_back("0123456789abcdef"[n & 15]);
        }
        REQUIRE_EQ(hex, std::string_view(expected[i]));
    }
}
#endif
