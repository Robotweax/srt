// SPDX-License-Identifier: MIT
#include "test.hpp"
#include "peer_authentication.hpp"
#include "peer_authentication_admission.hpp"
#include "robotweax/srt/handshake.hpp"
#include "robotweax/srt/handshake_datagram.hpp"
#include "robotweax/srt/crypto.hpp"
#include "robotweax/srt/socket_options.hpp"
#include "srt/srt.h"
#include "compat/socket_registry.hpp"

#include <algorithm>
#include <atomic>
#include <vector>

using namespace robotweax::srt;
namespace pa = robotweax::srt::peer_authentication;
namespace {
std::span<const std::byte> passphrase()
{
    static constexpr char text[] = "test exchange passphrase";
    return {reinterpret_cast<const std::byte*>(text), sizeof(text) - 1};
}
pa::Digest scalar(unsigned n)
{
    pa::Digest result {};
    result.back() = std::byte(n);
    return result;
}
std::array<std::byte, 64> public_key(unsigned n)
{
    std::unique_ptr<P256Key> key;
    const auto error =
        default_crypto_provider().import_p256_key(scalar(n), key);
    SKIP_UNLESS(error != Error::unsupported,
        "P-256 provider not implemented on this backend yet");
    REQUIRE_EQ(error, Error::none);
    std::array<std::byte, 64> result {};
    REQUIRE_EQ(key->public_key(result), Error::none);
    return result;
}
std::vector<std::byte> packet(const pa::Record& record)
{
    HandshakeAction action {.kind = HandshakeActionKind::send};
    action.packet.extension_field = handshake_extension_flag_config;
    action.packet.encryption_field = record.key_length / 8;
    action.packet.request = record.message == pa::Message::confirmation
        ? HandshakeRequest::agreement
        : HandshakeRequest::conclusion;
    action.has_peer_authentication = true;
    REQUIRE_EQ(pa::encode_record(record, action.peer_authentication.bytes,
                   action.peer_authentication.size),
        Error::none);
    std::array<std::byte, 1500> wire {};
    const auto encoded =
        encode_handshake_datagram(action, PacketTimestamp {7}, 123, wire);
    REQUIRE(encoded);
    const auto decoded = decode_handshake_datagram(
        std::span {wire}.first(encoded.bytes_written));
    REQUIRE(decoded);
    REQUIRE(decoded.message.has_peer_authentication);
    REQUIRE(!decoded.message.has_unknown_extension);
    const auto payload = decoded.message.peer_authentication_payload.view();
    return {payload.begin(), payload.end()};
}
class TrackingProvider final : public CryptoProvider {
public:
    CryptoProvider& real = default_crypto_provider();
    unsigned operations = 0, fail_at = 0, handles = 0, derivations = 0,
             signatures = 0, agreements = 0, verifications = 0, generations = 0;
    unsigned fixed_ephemeral =
        0; // Test-only known ECDH scalar for active-attacker cases.
    bool fault() noexcept
    {
        return ++operations == fail_at;
    }
    class Handle final : public P256Key {
    public:
        TrackingProvider& owner;
        std::unique_ptr<P256Key> key;
        Handle(TrackingProvider& p, std::unique_ptr<P256Key> k)
            : owner(p)
            , key(std::move(k))
        {
            ++owner.handles;
        }
        ~Handle() override
        {
            --owner.handles;
        }
        Error public_key(std::span<std::byte, 64> out) noexcept override
        {
            if (owner.fault()) {
                owner.secure_erase(out);
                return Error::cryptographic_failure;
            }
            return key->public_key(out);
        }
        Error agree(std::span<const std::byte, 64> p,
            std::span<std::byte, 32> out) noexcept override
        {
            ++owner.agreements;
            if (owner.fault()) {
                owner.secure_erase(out);
                return Error::cryptographic_failure;
            }
            return key->agree(p, out);
        }
        Error sign_sha256(std::span<const std::byte> m,
            std::span<std::byte, 64> out) noexcept override
        {
            ++owner.signatures;
            if (owner.fault()) {
                owner.secure_erase(out);
                return Error::cryptographic_failure;
            }
            return key->sign_sha256(m, out);
        }
    };
    Error generate_p256_key(std::unique_ptr<P256Key>& output) noexcept override
    {
        output.reset();
        ++generations;
        if (fault())
            return Error::cryptographic_failure;
        std::unique_ptr<P256Key> key;
        const auto e = fixed_ephemeral
            ? real.import_p256_key(scalar(fixed_ephemeral), key)
            : real.generate_p256_key(key);
        if (e == Error::none)
            output = std::make_unique<Handle>(*this, std::move(key));
        return e;
    }
    Error import_p256_key(std::span<const std::byte, 32> value,
        std::unique_ptr<P256Key>& output) noexcept override
    {
        output.reset();
        if (fault())
            return Error::cryptographic_failure;
        std::unique_ptr<P256Key> key;
        const auto e = real.import_p256_key(value, key);
        if (e == Error::none)
            output = std::make_unique<Handle>(*this, std::move(key));
        return e;
    }
    Error validate_p256_public_key(
        std::span<const std::byte, 64> key) noexcept override
    {
        return fault() ? Error::cryptographic_failure
                       : real.validate_p256_public_key(key);
    }
    Error verify_p256_sha256(std::span<const std::byte, 64> key,
        std::span<const std::byte> m,
        std::span<const std::byte, 64> s) noexcept override
    {
        ++verifications;
        return fault() ? Error::cryptographic_failure
                       : real.verify_p256_sha256(key, m, s);
    }
    Error random_bytes(std::span<std::byte> out) noexcept override
    {
        if (fault()) {
            secure_erase(out);
            return Error::cryptographic_failure;
        }
        return real.random_bytes(out);
    }
    Error sha256(std::span<const std::byte> in,
        std::span<std::byte, 32> out) noexcept override
    {
        if (fault()) {
            secure_erase(out);
            return Error::cryptographic_failure;
        }
        return real.sha256(in, out);
    }
    Error hmac_sha256(std::span<const std::byte> key,
        std::span<const std::byte> in,
        std::span<std::byte, 32> out) noexcept override
    {
        if (fault()) {
            secure_erase(out);
            return Error::cryptographic_failure;
        }
        return real.hmac_sha256(key, in, out);
    }
    Error pbkdf2_hmac_sha1(std::span<const std::byte> p,
        std::span<const std::byte> s, std::uint32_t n,
        std::span<std::byte> out) noexcept override
    {
        ++derivations;
        if (fault()) {
            secure_erase(out);
            return Error::cryptographic_failure;
        }
        return real.pbkdf2_hmac_sha1(p, s, n, out);
    }
    Error wrap_key(std::span<const std::byte> k, std::span<const std::byte> in,
        std::span<std::byte> out, std::size_t& n) noexcept override
    {
        return real.wrap_key(k, in, out, n);
    }
    Error unwrap_key(std::span<const std::byte> k,
        std::span<const std::byte> in, std::span<std::byte> out,
        std::size_t& n) noexcept override
    {
        return real.unwrap_key(k, in, out, n);
    }
    Error make_aes_ctr_cipher(std::span<const std::byte> k,
        std::unique_ptr<PayloadCipher>& out) noexcept override
    {
        return real.make_aes_ctr_cipher(k, out);
    }
    bool supports_aes_gcm() const noexcept override
    {
        return real.supports_aes_gcm();
    }
    Error make_aes_gcm_cipher(std::span<const std::byte> k,
        std::unique_ptr<AuthenticatedPayloadCipher>& out) noexcept override
    {
        return real.make_aes_gcm_cipher(k, out);
    }
    void secure_erase(std::span<std::byte> out) noexcept override
    {
        real.secure_erase(out);
    }
};
struct Pair {
    TrackingProvider cp, lp;
    pa::Exchange caller {cp, true}, listener {lp, false};
    std::vector<std::byte> m1, m2, m3;
    void configure(unsigned listener_pin = 1)
    {
        REQUIRE_EQ(caller.configure(scalar(1), public_key(2)), Error::none);
        REQUIRE_EQ(listener.configure(scalar(2), public_key(listener_pin)),
            Error::none);
    }
    void offer(unsigned length = 16)
    {
        pa::Record r;
        REQUIRE_EQ(caller.prepare_offer(static_cast<std::uint8_t>(length), r),
            Error::none);
        m1 = packet(r);
        REQUIRE_EQ(caller.bind_offer(m1), Error::none);
    }
    void response(unsigned local_length = 0)
    {
        pa::Record r;
        REQUIRE_EQ(listener.prepare_response(
                       m1, static_cast<std::uint8_t>(local_length), r),
            Error::none);
        m2 = packet(r);
        REQUIRE_EQ(listener.seal_response(m2), Error::none);
    }
    void confirmation()
    {
        pa::Record r;
        REQUIRE_EQ(caller.prepare_confirmation(m2, r), Error::none);
        m3 = packet(r);
        REQUIRE_EQ(caller.seal_confirmation(m3, passphrase()), Error::none);
    }
};
bool equal(std::span<const std::byte> a, std::span<const std::byte> b)
{
    return std::equal(a.begin(), a.end(), b.begin(), b.end());
}
Error attempt(TrackingProvider& cp, TrackingProvider& lp)
{
    pa::Exchange c(cp, true), l(lp, false);
    pa::Record r;
    auto error = c.configure(scalar(1), public_key(2));
    if (error != Error::none)
        return error;
    error = l.configure(scalar(2), public_key(1));
    if (error != Error::none)
        return error;
    error = c.prepare_offer(16, r);
    if (error != Error::none)
        return error;
    auto m1 = packet(r);
    error = c.bind_offer(m1);
    if (error != Error::none)
        return error;
    error = l.prepare_response(m1, 0, r);
    if (error != Error::none)
        return error;
    auto m2 = packet(r);
    error = l.seal_response(m2);
    if (error != Error::none)
        return error;
    error = c.prepare_confirmation(m2, r);
    if (error != Error::none)
        return error;
    auto m3 = packet(r);
    error = c.seal_confirmation(m3, passphrase());
    if (error != Error::none)
        return error;
    return l.accept_confirmation(m3, passphrase());
}
}
TEST(peer_auth_exchange_mutual_identity_key_lengths_and_wrapping)
{
    for (unsigned length : {0U, 16U, 24U, 32U}) {
        Pair p;
        p.configure();
        p.offer(length);
        p.response(24);
        REQUIRE_EQ(p.cp.derivations, 0U);
        REQUIRE_EQ(p.lp.derivations, 0U);
        REQUIRE(!p.caller.ready_for_key_exchange());
        REQUIRE(!p.listener.ready_for_key_exchange());
        REQUIRE(p.listener.session_key().empty());
        REQUIRE_EQ(p.lp.handles, 0U);
        p.confirmation();
        REQUIRE_EQ(p.cp.derivations, 1U);
        REQUIRE_EQ(p.lp.derivations, 0U);
        REQUIRE_EQ(
            p.listener.accept_confirmation(p.m3, passphrase()), Error::none);
        REQUIRE_EQ(p.lp.derivations, 1U);
        REQUIRE_EQ(p.cp.handles, 0U);
        REQUIRE_EQ(p.lp.handles, 0U);
        REQUIRE(p.caller.ready_for_key_exchange());
        REQUIRE(p.listener.ready_for_key_exchange());
        REQUIRE(equal(p.caller.peer_public_key(), public_key(2)));
        REQUIRE(equal(p.listener.peer_public_key(), public_key(1)));
        REQUIRE(equal(p.caller.session_key(), p.listener.session_key()));
        for (bool direction : {true, false}) {
            REQUIRE(equal(p.caller.wrapping_key(direction),
                p.listener.wrapping_key(direction)));
            REQUIRE_EQ(
                p.caller.wrapping_key(direction).size(), length ? length : 24U);
            std::array<std::byte, 32> plain {}, unwrapped {};
            plain.fill(std::byte {0x19});
            std::array<std::byte, 40> wrapped {};
            std::size_t n = 0;
            REQUIRE_EQ(p.cp.wrap_key(
                           p.caller.wrapping_key(direction), plain, wrapped, n),
                Error::none);
            REQUIRE_EQ(p.lp.unwrap_key(p.listener.wrapping_key(direction),
                           wrapped, unwrapped, n),
                Error::none);
            REQUIRE_EQ(plain, unwrapped);
        }
        REQUIRE(
            !equal(p.caller.wrapping_key(true), p.caller.wrapping_key(false)));
    }
}
TEST(peer_auth_exchange_retries_are_cached_without_new_crypto)
{
    Pair p;
    p.configure();
    p.offer();
    p.response();
    p.confirmation();
    REQUIRE_EQ(p.listener.accept_confirmation(p.m3, passphrase()), Error::none);
    const auto caller_ops = p.cp.operations, listener_ops = p.lp.operations;
    pa::Record r;
    for (unsigned i = 0; i < 5; ++i) {
        REQUIRE_EQ(p.listener.prepare_response(p.m1, 0, r), Error::none);
        REQUIRE_EQ(p.listener.seal_response(p.m2), Error::none);
        REQUIRE_EQ(p.caller.prepare_confirmation(p.m2, r), Error::none);
        REQUIRE_EQ(p.caller.seal_confirmation(p.m3, passphrase()), Error::none);
        REQUIRE_EQ(
            p.listener.accept_confirmation(p.m3, passphrase()), Error::none);
        REQUIRE(equal(p.caller.cached(pa::Message::offer), p.m1));
        REQUIRE(equal(p.listener.cached(pa::Message::response), p.m2));
        REQUIRE(equal(p.caller.cached(pa::Message::confirmation), p.m3));
    }
    REQUIRE_EQ(p.cp.operations, caller_ops);
    REQUIRE_EQ(p.lp.operations, listener_ops);
    auto altered = p.m1;
    altered[47] ^= std::byte {1};
    REQUIRE_EQ(
        p.listener.prepare_response(altered, 0, r), Error::invalid_state);
    REQUIRE(p.listener.ready_for_key_exchange());
    REQUIRE(equal(p.listener.cached(pa::Message::offer), p.m1));
}
TEST(peer_auth_exchange_unpinned_identity_rejected_before_ephemeral_work)
{
    Pair p;
    p.configure(3);
    p.offer();
    pa::Record r;
    REQUIRE_EQ(
        p.listener.prepare_response(p.m1, 0, r), Error::authentication_failure);
    REQUIRE_EQ(p.lp.generations, 0U);
    REQUIRE_EQ(p.lp.agreements, 0U);
    REQUIRE_EQ(p.lp.derivations, 0U);
    REQUIRE_EQ(p.lp.handles, 0U);
    REQUIRE(p.listener.cached(pa::Message::offer).empty());
}
TEST(peer_auth_exchange_tampered_response_never_reaches_passphrase)
{
    // Header, nonce, share, identifier, signature and confirmation tag.
    for (unsigned offset : {24U, 60U, 92U, 156U, 188U, 252U}) {
        Pair p;
        p.configure();
        p.offer();
        p.response();
        pa::Record r;
        p.m2[offset] ^= std::byte {1};
        REQUIRE(p.caller.prepare_confirmation(p.m2, r) != Error::none);
        REQUIRE_EQ(p.cp.derivations, 0U);
        REQUIRE_EQ(p.cp.handles, 0U);
        REQUIRE(!p.caller.ready_for_key_exchange());
        REQUIRE(p.caller.session_key().empty());
    }
}
TEST(peer_auth_exchange_invalid_signature_budget_precedes_password_check)
{
    Pair p;
    p.configure();
    p.offer();
    p.response();
    p.confirmation();
    auto bad = p.m3;
    bad[60] ^= std::byte {1};
    for (unsigned i = 0; i < 4; ++i) {
        REQUIRE_EQ(p.listener.accept_confirmation(bad, passphrase()),
            Error::authentication_failure);
        REQUIRE_EQ(p.lp.derivations, 0U);
        REQUIRE(!p.listener.ready_for_key_exchange());
        REQUIRE_EQ(p.listener.failed(), i == 3);
    }
    REQUIRE(p.listener.cached(pa::Message::response).empty());
    REQUIRE_EQ(p.listener.accept_confirmation(p.m3, passphrase()),
        Error::invalid_state);
    Pair recover;
    recover.configure();
    recover.offer();
    recover.response();
    recover.confirmation();
    bad = recover.m3;
    bad[60] ^= std::byte {1};
    REQUIRE_EQ(recover.listener.accept_confirmation(bad, passphrase()),
        Error::authentication_failure);
    REQUIRE_EQ(recover.listener.accept_confirmation(recover.m3, passphrase()),
        Error::none);
}
TEST(peer_auth_exchange_valid_signature_bad_confirmation_ends_attempt)
{
    for (bool wrong_password : {true, false}) {
        Pair p;
        p.configure();
        p.offer();
        p.response();
        p.confirmation();
        auto wrong =
            std::vector<std::byte>(passphrase().begin(), passphrase().end());
        wrong[0] ^= std::byte {1};
        if (!wrong_password)
            p.m3.back() ^= std::byte {1};
        REQUIRE_EQ(p.listener.accept_confirmation(p.m3,
                       wrong_password ? std::span<const std::byte>(wrong)
                                      : passphrase()),
            Error::authentication_failure);
        REQUIRE_EQ(p.lp.derivations, 1U);
        REQUIRE(!p.listener.ready_for_key_exchange());
        REQUIRE(p.listener.session_key().empty());
        REQUIRE_EQ(p.listener.accept_confirmation(p.m3, passphrase()),
            Error::invalid_state);
    }
}
TEST(peer_auth_exchange_old_confirmation_and_reflection_rejected)
{
    Pair first;
    first.configure();
    first.offer();
    first.response();
    first.confirmation();
    Pair second;
    second.configure();
    second.offer();
    second.response();
    second.confirmation();
    REQUIRE(first.m1 != second.m1);
    REQUIRE(first.m2 != second.m2);
    REQUIRE_EQ(second.listener.accept_confirmation(first.m3, passphrase()),
        Error::authentication_failure);
    auto reflection = second.m3;
    std::copy_n(second.m2.begin() + 188, 64, reflection.begin() + 60);
    REQUIRE_EQ(second.listener.accept_confirmation(reflection, passphrase()),
        Error::authentication_failure);
    REQUIRE_EQ(second.lp.derivations, 0U);
    REQUIRE_EQ(second.listener.accept_confirmation(second.m3, passphrase()),
        Error::none);
}
TEST(peer_auth_exchange_rejects_changed_local_record_and_negotiation)
{
    Pair p;
    p.configure();
    pa::Record r;
    REQUIRE_EQ(p.caller.prepare_offer(16, r), Error::none);
    r.nonce[0] ^= std::byte {1};
    auto m1 = packet(r);
    REQUIRE_EQ(p.caller.bind_offer(m1), Error::invalid_key_material);
    REQUIRE_EQ(p.cp.handles, 0U);
    Pair q;
    q.configure();
    q.offer();
    REQUIRE_EQ(q.listener.prepare_response(q.m1, 0, r), Error::none);
    auto m2 = packet(r);
    m2[5] = std::byte {4}; // 256-bit header vs 128-bit record.
    REQUIRE_EQ(q.listener.seal_response(m2), Error::invalid_handshake_value);
    REQUIRE_EQ(q.lp.derivations, 0U);
    REQUIRE_EQ(q.lp.handles, 0U);
}
TEST(peer_auth_exchange_pin_rotation_limits_and_abort_cleanup)
{
    TrackingProvider p;
    const auto one = public_key(1), two = public_key(2);
    std::vector<std::byte> pins(one.begin(), one.end());
    pins.insert(pins.end(), two.begin(), two.end());
    pa::Exchange c(p, true);
    REQUIRE_EQ(c.configure(scalar(1), pins), Error::none);
    pa::Record r;
    REQUIRE_EQ(c.prepare_offer(0, r), Error::none);
    REQUIRE_EQ(p.handles, 2U);
    c.abort();
    REQUIRE_EQ(p.handles, 0U);
    REQUIRE(!c.ready_for_key_exchange());
    REQUIRE(c.session_key().empty());
    for (std::size_t count : {0U, 1U, 63U, 65U, 1088U}) {
        pa::Exchange bad(p, true);
        pins.resize(count);
        REQUIRE_EQ(bad.configure(scalar(1), pins), Error::invalid_key_material);
        REQUIRE_EQ(p.handles, 0U);
    }
}
TEST(peer_auth_exchange_each_provider_failure_releases_all_handles)
{
    TrackingProvider baseline_c, baseline_l;
    REQUIRE_EQ(attempt(baseline_c, baseline_l), Error::none);
    for (bool caller : {true, false}) {
        const auto count =
            caller ? baseline_c.operations : baseline_l.operations;
        for (unsigned failure = 1; failure <= count; ++failure) {
            TrackingProvider cp, lp;
            (caller ? cp : lp).fail_at = failure;
            REQUIRE(attempt(cp, lp) != Error::none);
            REQUIRE_EQ(cp.handles, 0U);
            REQUIRE_EQ(lp.handles, 0U);
        }
    }
}

TEST(peer_auth_exchange_selects_rotated_pin_and_accepts_sixteen_keys)
{
    Pair p;
    auto pins = std::vector<std::byte>();
    for (unsigned i = 3; i < 18; ++i) {
        const auto key = public_key(i);
        pins.insert(pins.end(), key.begin(), key.end());
    }
    const auto l = public_key(2);
    pins.insert(pins.end(), l.begin(), l.end());
    REQUIRE_EQ(p.caller.configure(scalar(1), pins), Error::none);
    REQUIRE_EQ(p.listener.configure(scalar(2), public_key(1)), Error::none);
    p.offer();
    p.response();
    p.confirmation();
    REQUIRE_EQ(p.listener.accept_confirmation(p.m3, passphrase()), Error::none);
    REQUIRE(equal(p.caller.peer_public_key(), l));
}
TEST(peer_auth_exchange_provider_defaults_fail_closed)
{
    // Explicit base calls exercise the defaults custom providers inherit.
    (void)public_key(
        1); // Skip only when this backend has no P-256 implementation.
    TrackingProvider p;
    std::unique_ptr<P256Key> key;
    REQUIRE_EQ(p.real.generate_p256_key(key), Error::none);
    REQUIRE_EQ(p.CryptoProvider::generate_p256_key(key), Error::unsupported);
    REQUIRE(!key);
    REQUIRE_EQ(
        p.CryptoProvider::import_p256_key(scalar(1), key), Error::unsupported);
    REQUIRE(!key);
    REQUIRE_EQ(p.CryptoProvider::validate_p256_public_key(public_key(1)),
        Error::unsupported);
    std::array<std::byte, 64> signature {};
    REQUIRE_EQ(p.CryptoProvider::verify_p256_sha256(
                   public_key(1), passphrase(), signature),
        Error::unsupported);
}

TEST(peer_auth_exchange_every_transcript_byte_tamper_blocks_completion)
{
    for (unsigned flight = 0; flight < 3; ++flight) {
        const auto size =
            52 + pa::content_size(static_cast<pa::Message>(flight + 1));
        for (std::size_t offset = 0; offset < size; ++offset) {
            Pair p;
            p.configure();
            p.offer();
            pa::Record r;
            if (flight == 0) {
                p.m1[offset] ^= std::byte {1};
                auto error = p.listener.prepare_response(p.m1, 0, r);
                if (error == Error::none) {
                    p.m2 = packet(r);
                    error = p.listener.seal_response(p.m2);
                }
                if (error == Error::none)
                    error = p.caller.prepare_confirmation(p.m2, r);
                REQUIRE(error != Error::none);
                REQUIRE_EQ(p.cp.derivations, 0U);
            } else {
                p.response();
                if (flight == 1) {
                    p.m2[offset] ^= std::byte {1};
                    REQUIRE(
                        p.caller.prepare_confirmation(p.m2, r) != Error::none);
                    REQUIRE_EQ(p.cp.derivations, 0U);
                } else {
                    p.confirmation();
                    p.m3[offset] ^= std::byte {1};
                    REQUIRE(p.listener.accept_confirmation(p.m3, passphrase())
                        != Error::none);
                }
            }
            REQUIRE(!p.listener.ready_for_key_exchange());
            REQUIRE(p.listener.session_key().empty());
        }
    }
}

TEST(peer_auth_exchange_valid_ephemeral_mac_cannot_replace_identity_signature)
{
    Pair p;
    p.lp.fixed_ephemeral = 4;
    p.configure();
    p.offer();
    p.response();
    pa::Record offer, response;
    REQUIRE_EQ(
        pa::decode_record(std::span {p.m1}.subspan(52), offer), Error::none);
    REQUIRE_EQ(
        pa::decode_record(std::span {p.m2}.subspan(52), response), Error::none);
    response.signature[0] ^= std::byte {1};
    std::unique_ptr<P256Key> attacker;
    REQUIRE_EQ(p.lp.real.import_p256_key(scalar(4), attacker), Error::none);
    pa::Digest shared {}, hash {};
    REQUIRE_EQ(attacker->agree(offer.public_key, shared), Error::none);
    REQUIRE_EQ(
        pa::listener_transcript(p.lp.real, p.m1, p.m2, hash), Error::none);
    pa::KeySchedule attack(p.lp.real);
    REQUIRE_EQ(attack.start(shared, hash), Error::none);
    REQUIRE_EQ(
        attack.listener_confirmation(response.signature, response.confirmation),
        Error::none);
    std::size_t n = 0;
    REQUIRE_EQ(pa::encode_record(response, std::span {p.m2}.subspan(52), n),
        Error::none);
    pa::Record confirmation;
    REQUIRE_EQ(p.caller.prepare_confirmation(p.m2, confirmation),
        Error::authentication_failure);
    REQUIRE_EQ(p.cp.verifications, 1U);
    REQUIRE_EQ(p.cp.derivations, 0U);
    REQUIRE(!p.caller.ready_for_key_exchange());
    REQUIRE_EQ(p.cp.handles, 0U);
}

TEST(peer_auth_runtime_directional_keys_and_authenticated_controls)
{
    for (unsigned length : {16U, 24U, 32U}) {
        Pair p;
        p.configure();
        p.offer(length);
        p.response();
        p.confirmation();
        REQUIRE_EQ(
            p.listener.accept_confirmation(p.m3, passphrase()), Error::none);
        const CryptoConfiguration config {
            .passphrase = "runtime test passphrase",
            .mode = CryptoMode::aes_gcm,
            .enable_aes_gcm = true};
        CryptoSession c(config, p.cp), l(config, p.lp);
        REQUIRE_EQ(c.start_peer_authentication(p.caller), Error::none);
        REQUIRE_EQ(l.start_peer_authentication(p.listener), Error::none);
        REQUIRE_EQ(c.key_length(), length);
        REQUIRE_EQ(c.start_peer_authentication(p.caller), Error::invalid_state);
        REQUIRE_EQ(c.sender_state(), CryptoState::securing);
        REQUIRE(!c.ready_to_send_data());
        REQUIRE(!l.ready_to_send_data());
        REQUIRE(!c.allows_plaintext_fallback());
        REQUIRE_EQ(c.continue_without_peer_key(CryptoState::no_secret),
            Error::invalid_state);
        REQUIRE(!equal(c.pending_key_material(), l.pending_key_material()));
        const auto c_pbkdf = p.cp.derivations, l_pbkdf = p.lp.derivations;
        for (bool caller : {true, false}) {
            auto& sender = caller ? c : l;
            auto& receiver = caller ? l : c;
            auto& auth = *sender.session_authentication();
            auto& peer = *receiver.session_authentication();
            std::array<std::byte, 256> wire {}, reply {};
            std::size_t n = 0, m = 0;
            std::span<const std::byte> material;
            REQUIRE_EQ(auth.seal(false, sender.pending_key_material(), wire, n),
                Error::none);
            const auto body = sender.pending_key_material();
            std::vector<std::byte> mac_input(28 + body.size());
            constexpr std::string_view label = "RWSRT-PEERAUTH-1";
            for (std::size_t i = 0; i < label.size(); ++i)
                mac_input[i] = std::byte(label[i]);
            mac_input[16] = std::byte {2}; // request domain
            mac_input[17] = std::byte(caller ? 1 : 2);
            mac_input[25] = std::byte {1}; // first request counter
            mac_input[26] = std::byte(body.size() >> 8);
            mac_input[27] = std::byte(body.size());
            std::copy(body.begin(), body.end(), mac_input.begin() + 28);
            pa::Digest expected;
            REQUIRE_EQ(p.cp.real.hmac_sha256(
                           p.caller.session_key(), mac_input, expected),
                Error::none);
            REQUIRE(equal(expected, std::span {wire}.first(n).last<32>()));
            REQUIRE(!auth.open(
                false, std::span {wire}.first(n), material)); // role reflection
            wire[n - 1] ^= std::byte {1};
            REQUIRE(!peer.open(false, std::span {wire}.first(n), material));
            wire[n - 1] ^= std::byte {1};
            REQUIRE(peer.open(false, std::span {wire}.first(n), material));
            REQUIRE(!receiver.needs_receive_key_derivation(material));
            REQUIRE_EQ(receiver.accept_key_material(material, true),
                Error::invalid_state);
            REQUIRE_EQ(
                receiver.accept_key_material(material, false), Error::none);
            REQUIRE_EQ(
                peer.seal(true, receiver.key_material_response(), reply, m),
                Error::none);
            REQUIRE(auth.open(true, std::span {reply}.first(m), material));
            REQUIRE_EQ(
                sender.acknowledge_key_material(material, false), Error::none);
            REQUIRE_EQ(sender.sender_state(), CryptoState::secured);
            REQUIRE(sender.ready_to_send_data());
            DataHeader data {.sequence = SequenceNumber {1},
                .message_number = 1,
                .boundary = MessageBoundary::solo,
                .encryption_key = sender.active_sender_key()};
            std::array<std::byte, 3> plaintext {
                std::byte {7}, std::byte {8}, std::byte {9}};
            std::array<std::byte, 3> ciphertext {}, opened {};
            std::array<std::byte, srt_gcm_authentication_tag_size> tag {};
            EncryptionKey key = EncryptionKey::none;
            REQUIRE_EQ(sender.seal(data, plaintext, ciphertext, tag, key),
                Error::none);
            REQUIRE_EQ(
                receiver.open(data, ciphertext, tag, opened), Error::none);
            REQUIRE_EQ(opened, plaintext);
            tag[0] ^= std::byte {1};
            REQUIRE(
                receiver.open(data, ciphertext, tag, opened) != Error::none);

            SessionAuthenticationParameters legacy;
            REQUIRE_EQ(
                auth.handshake_proof(caller, {}, legacy), Error::invalid_state);
            DataHeader header;
            std::array<std::byte, 3> parity {
                std::byte {1}, std::byte {2}, std::byte {3}};
            REQUIRE_EQ(auth.seal_fec(header, parity, wire, n), Error::none);
            REQUIRE(
                !auth.open_fec(header, std::span {wire}.first(n), material));
            REQUIRE(peer.open_fec(header, std::span {wire}.first(n), material));
            REQUIRE(equal(material, parity));
            REQUIRE(
                !peer.open_fec(header, std::span {wire}.first(n), material));
        }
        // A newly generated salt still uses the same directional KEK.
        CryptoSession fresh(config, p.cp);
        REQUIRE_EQ(fresh.start_peer_authentication(p.caller), Error::none);
        REQUIRE(!l.needs_receive_key_derivation(fresh.pending_key_material()));
        REQUIRE_EQ(l.accept_key_material(fresh.pending_key_material(), false),
            Error::none);
        REQUIRE_EQ(p.cp.derivations, c_pbkdf);
        REQUIRE_EQ(p.lp.derivations, l_pbkdf);
    }
}

TEST(peer_auth_runtime_rejects_incomplete_and_incompatible_sessions)
{
    Pair p;
    const CryptoConfiguration config {.passphrase = "runtime test passphrase",
        .mode = CryptoMode::aes_gcm,
        .enable_aes_gcm = true};
    CryptoSession fresh(config);
    REQUIRE_EQ(fresh.start_peer_authentication(p.caller), Error::invalid_state);
    p.configure();
    p.offer();
    p.response();
    p.confirmation();
    CryptoSession legacy({.passphrase = "runtime test passphrase"});
    REQUIRE_EQ(
        legacy.start_peer_authentication(p.caller), Error::invalid_state);
    CryptoSession active(config);
    REQUIRE_EQ(active.start_initiator(), Error::none);
    REQUIRE_EQ(
        active.start_peer_authentication(p.caller), Error::invalid_state);
    CryptoSession session_auth(config);
    REQUIRE_EQ(session_auth.start_session_authentication(true), Error::none);
    REQUIRE_EQ(
        session_auth.start_peer_authentication(p.caller), Error::invalid_state);
    auto rendezvous = config;
    rendezvous.negotiation_context = CryptoNegotiationContext::rendezvous;
    CryptoSession simultaneous(rendezvous);
    REQUIRE_EQ(
        simultaneous.start_peer_authentication(p.caller), Error::invalid_state);
}

TEST(peer_auth_runtime_initialization_failure_is_terminal)
{
    Pair p;
    p.configure();
    p.offer();
    p.response();
    p.confirmation();
    CryptoSession c({.passphrase = "runtime test passphrase",
                        .mode = CryptoMode::aes_gcm,
                        .enable_aes_gcm = true},
        p.cp);
    p.cp.fail_at = p.cp.operations + 1; // initial runtime randomness
    REQUIRE_EQ(
        c.start_peer_authentication(p.caller), Error::cryptographic_failure);
    REQUIRE(c.session_authentication() == nullptr);
    REQUIRE(c.pending_key_material().empty());
    REQUIRE_EQ(c.start_peer_authentication(p.caller), Error::invalid_state);
    REQUIRE_EQ(c.start_initiator(), Error::cryptographic_failure);
    REQUIRE_EQ(c.continue_without_peer_key(CryptoState::no_secret),
        Error::invalid_state);
}

TEST(
    peer_auth_datagram_preserves_opaque_extensions_and_outer_header_independence)
{
    Pair p;
    p.configure();
    pa::Record record;
    REQUIRE_EQ(p.caller.prepare_offer(16, record), Error::none);
    p.m1 = packet(record);
    const std::array<std::byte, 8> opaque {std::byte {0x7e}, std::byte {0x01},
        std::byte {0}, std::byte {1}, std::byte {1}, std::byte {2},
        std::byte {3}, std::byte {4}};
    // An unknown record before PEERAUTH must keep its exact position and bytes.
    p.m1.insert(p.m1.begin() + handshake_size, opaque.begin(), opaque.end());
    auto roundtrip = [](const std::vector<std::byte>& payload) {
        std::array<std::byte, 1500> wire {};
        const auto encoded = encode_peer_authentication_datagram(
            payload, PacketTimestamp {99}, 456, wire);
        REQUIRE(encoded);
        auto decoded = decode_handshake_datagram(
            std::span {wire}.first(encoded.bytes_written));
        REQUIRE(decoded);
        REQUIRE_EQ(decoded.control.destination_socket_id, 456U);
        REQUIRE(
            equal(decoded.message.peer_authentication_payload.view(), payload));
        const auto copy = decoded.message;
        std::fill(wire.begin(), wire.end(), std::byte {0});
        REQUIRE(equal(
            copy.peer_authentication_payload.view(), payload)); // owned bytes
        const auto retry = encode_peer_authentication_datagram(
            copy.peer_authentication_payload.view(), PacketTimestamp {100}, 789,
            wire);
        REQUIRE(retry);
        REQUIRE(
            equal(std::span {wire}.subspan(packet_header_size, payload.size()),
                payload));
        return copy;
    };
    const auto offer = roundtrip(p.m1);
    REQUIRE(offer.has_unknown_extension);
    REQUIRE_EQ(p.caller.bind_offer(offer.peer_authentication_payload.view()),
        Error::none);
    REQUIRE_EQ(p.listener.prepare_response(
                   offer.peer_authentication_payload.view(), 16, record),
        Error::none);
    p.m2 = packet(record);
    p.m2.insert(p.m2.end(), opaque.begin(), opaque.end());
    REQUIRE_EQ(p.listener.seal_response(p.m2), Error::none);
    auto response = roundtrip(p.m2);
    REQUIRE_EQ(p.caller.prepare_confirmation(
                   response.peer_authentication_payload.view(), record),
        Error::none);
    p.m3 = packet(record);
    p.m3.insert(p.m3.end(), opaque.begin(), opaque.end());
    REQUIRE_EQ(p.caller.seal_confirmation(p.m3, passphrase()), Error::none);
    auto confirmation = roundtrip(p.m3);
    REQUIRE_EQ(
        p.listener.accept_confirmation(
            confirmation.peer_authentication_payload.view(), passphrase()),
        Error::none);
    REQUIRE(equal(p.caller.session_key(), p.listener.session_key()));
    HandshakeMachine machine({.role = ConnectionRole::listener});
    const auto actions = machine.receive(offer);
    REQUIRE_EQ(actions.size, 1U);
    REQUIRE_EQ(actions.values[0].kind, HandshakeActionKind::failed);
}

TEST(peer_auth_datagram_rejects_malformed_mixed_and_oversized_payloads)
{
    Pair p;
    p.configure();
    p.offer();
    auto reject = [](std::span<const std::byte> payload) {
        std::array<std::byte, 2048> wire {};
        const auto retry =
            encode_peer_authentication_datagram(payload, {}, 1, wire);
        REQUIRE(!retry);
        REQUIRE_EQ(retry.bytes_written, 0U);
        MutablePacketView packet;
        packet.kind = PacketKind::control;
        packet.control.type = ControlType::handshake;
        packet.payload = payload;
        REQUIRE(encode_packet(packet, wire));
        REQUIRE(!decode_handshake_datagram(
            std::span {wire}.first(packet_header_size + payload.size())));
    };
    for (std::size_t offset : {0U, 4U, 20U, 52U, 53U, 54U, 56U}) {
        auto damaged = p.m1;
        damaged[offset] ^= std::byte {0x80};
        reject(damaged);
    }
    auto missing_config = p.m1;
    missing_config[7] = std::byte {};
    reject(missing_config);
    auto duplicate = p.m1;
    duplicate.insert(
        duplicate.end(), p.m1.begin() + handshake_size, p.m1.end());
    reject(duplicate);
    auto mixed = p.m1;
    mixed.insert(mixed.end(),
        {std::byte {0x7f}, std::byte {0x10}, std::byte {0}, std::byte {0}});
    reject(mixed);
    auto oversized = p.m1;
    oversized.insert(oversized.end(),
        {std::byte {0x7e}, std::byte {1}, std::byte {1}, std::byte {0x40}});
    oversized.resize(oversized.size() + 1280);
    reject(oversized);
    std::array<std::byte, 8> tiny {};
    REQUIRE(!encode_peer_authentication_datagram(p.m1, {}, 1, tiny));
    for (std::size_t size = handshake_size + 1; size < p.m1.size(); ++size)
        reject(std::span {p.m1}.first(size));
}

TEST(peer_auth_datagram_encoder_rejects_inconsistent_actions)
{
    Pair p;
    p.configure();
    pa::Record record;
    REQUIRE_EQ(p.caller.prepare_offer(16, record), Error::none);
    HandshakeAction valid {.kind = HandshakeActionKind::send};
    valid.packet.extension_field = handshake_extension_flag_config;
    valid.packet.encryption_field = 2;
    valid.packet.request = HandshakeRequest::conclusion;
    valid.has_peer_authentication = true;
    REQUIRE_EQ(pa::encode_record(record, valid.peer_authentication.bytes,
                   valid.peer_authentication.size),
        Error::none);
    std::array<std::byte, 1500> wire {};
    REQUIRE(encode_handshake_datagram(valid, {}, 1, wire));
    for (unsigned test = 0; test < 8; ++test) {
        auto action = valid;
        switch (test) {
        case 0:
            action.has_session_authentication = true;
            break;
        case 1:
            action.has_key_material_extension = true;
            break;
        case 2:
            action.packet.extension_field = 0;
            break;
        case 3:
            action.packet.encryption_field = 4;
            break;
        case 4:
            action.packet.request = HandshakeRequest::agreement;
            break;
        case 5:
            action.packet.maximum_transmission_unit = 511;
            break;
        case 6:
            action.peer_authentication.size = 233;
            break;
        case 7:
            action.packet.version = handshake_version_4;
            break;
        }
        const auto encoded = encode_handshake_datagram(action, {}, 1, wire);
        REQUIRE(!encoded);
        REQUIRE_EQ(encoded.bytes_written, 0U);
    }
}

TEST(peer_auth_configuration_credentials_are_validated_and_copied_independently)
{
    PeerAuthenticationCredentials credentials;
    const auto pin = public_key(2);
    REQUIRE_EQ(credentials.set_identity(scalar(1)), Error::none);
    REQUIRE_EQ(credentials.set_trusted(pin), Error::none);
    REQUIRE(credentials.complete());
    REQUIRE(credentials.set_identity(scalar(0)) != Error::none);
    REQUIRE(
        credentials.set_identity(std::span<const std::byte> {}) != Error::none);
    std::array<std::byte, 64> invalid {};
    REQUIRE(credentials.set_trusted(invalid) != Error::none);
    REQUIRE(equal(credentials.trusted(), pin));
    REQUIRE(
        credentials.set_trusted(std::span<const std::byte> {}) != Error::none);
    std::vector<std::byte> pins;
    for (unsigned i = 0; i < 16; ++i)
        pins.insert(pins.end(), pin.begin(), pin.end());
    REQUIRE_EQ(credentials.set_trusted(pins), Error::none);
    pins.insert(pins.end(), pin.begin(), pin.end());
    REQUIRE(credentials.set_trusted(pins) != Error::none);
    auto copy = credentials;
    PeerAuthenticationCredentials assigned;
    REQUIRE_EQ(assigned.set_identity(scalar(3)), Error::none);
    assigned = credentials;
    credentials.clear();
    REQUIRE(!credentials.complete());
    REQUIRE(credentials.trusted().empty());
    for (auto* saved : {&copy, &assigned}) {
        REQUIRE_EQ(
            saved->set_trusted(saved->trusted().subspan(64, 64)), Error::none);
        pa::Exchange exchange(default_crypto_provider(), true);
        REQUIRE_EQ(saved->configure(exchange), Error::none);
        pa::Record record;
        REQUIRE_EQ(exchange.prepare_offer(16, record), Error::none);
        pa::Digest expected;
        REQUIRE_EQ(default_crypto_provider().sha256(public_key(1), expected),
            Error::none);
        REQUIRE_EQ(record.key_identifier, expected);
    }
}

TEST(peer_auth_configuration_dependency_order_and_missing_credentials)
{
    (void)public_key(1); // skip backends without P-256
    SocketOptions options;
    REQUIRE_EQ(options.set(SocketOption::peer_authentication_policy, 1),
        Error::invalid_state);
    REQUIRE_EQ(
        options.set_passphrase("configuration test passphrase"), Error::none);
    REQUIRE_EQ(options.set(SocketOption::crypto_mode, 2), Error::none);
    REQUIRE_EQ(
        options.set(SocketOption::session_authentication, 1), Error::none);
    REQUIRE_EQ(options.set(SocketOption::peer_authentication_policy, 2),
        Error::invalid_state);
    REQUIRE_EQ(
        options.set(SocketOption::peer_authentication_policy, 1), Error::none);
    pa::Exchange missing(default_crypto_provider(), true);
    REQUIRE_EQ(
        options.configure_peer_authentication(missing), Error::invalid_state);
    REQUIRE_EQ(options.set_peer_authentication_key(scalar(1)), Error::none);
    REQUIRE_EQ(
        options.set_peer_authentication_trusted(public_key(2)), Error::none);
    auto copied = options;
    options.clear_peer_authentication();
    REQUIRE_EQ(copied.configure_peer_authentication(missing), Error::none);
    for (auto [option, value] :
        {std::pair {SocketOption::session_authentication, 0},
            {SocketOption::enforced_encryption, 0},
            {SocketOption::crypto_mode, 0}, {SocketOption::crypto_mode, 1},
            {SocketOption::maximum_segment_size, 511},
            {SocketOption::rendezvous, 1},
            {SocketOption::experimental_acp1, 1}}) {
        REQUIRE_EQ(copied.set(option, value), Error::invalid_state);
        auto reverse = copied;
        REQUIRE_EQ(reverse.set(SocketOption::peer_authentication_policy, 0),
            Error::none);
        if (reverse.set(option, value) == Error::none)
            REQUIRE_EQ(reverse.set(SocketOption::peer_authentication_policy, 1),
                Error::invalid_state);
    }
    REQUIRE_EQ(copied.set_passphrase({}), Error::invalid_state);
    copied.clear_peer_authentication();
    REQUIRE_EQ(copied.set_passphrase({}), Error::none);
    REQUIRE(!copied.encryption_enabled());
}

TEST(peer_auth_socket_options_fail_closed_until_admission_is_implemented)
{
    const auto pin = public_key(2);
    REQUIRE_EQ(srt_startup(), 0);
    struct Cleanup {
        ~Cleanup()
        {
            (void)srt_cleanup();
        }
    } cleanup;
    const auto socket = srt_create_socket();
    auto retained = compat::SocketRegistry::instance().find(socket);
    REQUIRE(retained != nullptr);
    REQUIRE(socket != SRT_INVALID_SOCK);
    struct Close {
        SRTSOCKET id;
        ~Close()
        {
            (void)srt_close(id);
        }
    } close {socket};
    const std::int32_t required = 1, off = 0, gcm = 2;
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_ROBOTWEAX_PEERAUTH_POLICY,
                   &required, sizeof(required)),
        SRT_ERROR);
    REQUIRE_EQ(srt_setsockflag(
                   socket, SRTO_PASSPHRASE, "configuration passphrase", 24),
        0);
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_CRYPTOMODE, &gcm, sizeof(gcm)), 0);
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_ROBOTWEAX_SESSIONAUTH, &required,
                   sizeof(required)),
        0);
    REQUIRE_EQ(srt_setsockflag(
                   socket, SRTO_ROBOTWEAX_PEERAUTH_KEY, scalar(1).data(), 32),
        0);
    REQUIRE_EQ(srt_setsockflag(
                   socket, SRTO_ROBOTWEAX_PEERAUTH_TRUSTED, pin.data(), 64),
        0);
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_ROBOTWEAX_PEERAUTH_POLICY,
                   &required, sizeof(required)),
        0);
    std::array<std::byte, 64> returned {};
    int size = 64;
    REQUIRE_EQ(srt_getsockflag(
                   socket, SRTO_ROBOTWEAX_PEERAUTH_KEY, returned.data(), &size),
        SRT_ERROR);
    REQUIRE_EQ(srt_getsockflag(socket, SRTO_ROBOTWEAX_PEERAUTH_TRUSTED,
                   returned.data(), &size),
        0);
    REQUIRE_EQ(returned, pin);
    REQUIRE_EQ(size, 64);
    REQUIRE_EQ(
        srt_setsockflag(socket, SRTO_GROUPCONNECT, &required, sizeof(required)),
        SRT_ERROR);
    REQUIRE_EQ(srt_setsockflag(
                   socket, SRTO_ROBOTWEAX_PEERAUTH_POLICY, &off, sizeof(off)),
        0);
    REQUIRE_EQ(
        srt_setsockflag(socket, SRTO_GROUPCONNECT, &required, sizeof(required)),
        0);
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_ROBOTWEAX_PEERAUTH_POLICY,
                   &required, sizeof(required)),
        SRT_ERROR);
    REQUIRE_EQ(
        srt_setsockflag(socket, SRTO_GROUPCONNECT, &off, sizeof(off)), 0);
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_ROBOTWEAX_PEERAUTH_POLICY,
                   &required, sizeof(required)),
        0);
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE_EQ(srt_bind(socket, reinterpret_cast<const sockaddr*>(&address),
                   sizeof(address)),
        0);
    REQUIRE_EQ(srt_listen(socket, 8), SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ESECFAIL);
    address.sin_port = htons(9);
    REQUIRE_EQ(srt_connect(socket, reinterpret_cast<const sockaddr*>(&address),
                   sizeof(address)),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ESECFAIL);
    REQUIRE_EQ(srt_close(socket), 0);
    REQUIRE(!retained->native_options.peer_authentication_required());
    REQUIRE(retained->native_options.peer_authentication_trusted().empty());
    REQUIRE(!retained->native_options.encryption_enabled());
    pa::Exchange erased(default_crypto_provider(), true);
    REQUIRE_EQ(retained->native_options.configure_peer_authentication(erased),
        Error::invalid_state);
}

TEST(peer_auth_group_policy_cannot_be_enabled_through_option_template)
{
    REQUIRE_EQ(srt_startup(), 0);
    struct Cleanup {
        ~Cleanup()
        {
            (void)srt_cleanup();
        }
    } cleanup;
    const auto group = srt_create_group(SRT_GTYPE_BROADCAST);
    REQUIRE(group != SRT_INVALID_SOCK);
    struct Close {
        SRTSOCKET id;
        ~Close()
        {
            (void)srt_close(id);
        }
    } close {group};
    const int required = 1, gcm = 2;
    REQUIRE_EQ(
        srt_setsockflag(group, SRTO_PASSPHRASE, "configuration passphrase", 24),
        0);
    REQUIRE_EQ(srt_setsockflag(group, SRTO_CRYPTOMODE, &gcm, sizeof(gcm)), 0);
    REQUIRE_EQ(srt_setsockflag(group, SRTO_ROBOTWEAX_SESSIONAUTH, &required,
                   sizeof(required)),
        0);
    REQUIRE_EQ(srt_setsockflag(group, SRTO_ROBOTWEAX_PEERAUTH_POLICY, &required,
                   sizeof(required)),
        SRT_ERROR);
}

namespace {
struct AdmissionPair {
    TrackingProvider cp, lp;
    pa::Admission caller {cp, true}, listener {lp, false};
    pa::Admission::Time now {};
    pa::Admission::Time deadline = now + std::chrono::seconds {1};
    std::vector<std::byte> m1, m2, m3;
    void configure(bool wrong_password = false)
    {
        for (bool calling : {true, false}) {
            SocketOptions options;
            REQUIRE_EQ(options.set_passphrase(calling && wrong_password
                               ? "different test passphrase"
                               : "admission test passphrase"),
                Error::none);
            REQUIRE_EQ(options.set(SocketOption::crypto_mode, 2), Error::none);
            REQUIRE_EQ(options.set(SocketOption::session_authentication, 1),
                Error::none);
            REQUIRE_EQ(
                options.set_peer_authentication_key(scalar(calling ? 1 : 2)),
                Error::none);
            REQUIRE_EQ(options.set_peer_authentication_trusted(
                           public_key(calling ? 2 : 1)),
                Error::none);
            REQUIRE_EQ(options.set(SocketOption::peer_authentication_policy, 1),
                Error::none);
            REQUIRE_EQ(
                (calling ? caller : listener).configure(options, deadline, now),
                Error::none);
        } // The coordinator must own every borrowed configuration byte now.
    }
    void offer(unsigned length = 16)
    {
        pa::Record record;
        REQUIRE_EQ(caller.prepare_offer(length, record, now), Error::none);
        m1 = packet(record);
        REQUIRE_EQ(caller.bind_offer(m1, now), Error::none);
    }
    void response()
    {
        pa::Record record;
        REQUIRE_EQ(listener.prepare_response(m1, 16, record, now), Error::none);
        m2 = packet(record);
        REQUIRE_EQ(listener.seal_response(m2, now), Error::none);
    }
    void confirmation()
    {
        pa::Record record;
        REQUIRE_EQ(caller.prepare_confirmation(m2, record, now), Error::none);
        m3 = packet(record);
        REQUIRE_EQ(caller.seal_confirmation(m3, now), Error::none);
    }
    void proofs(unsigned length = 16)
    {
        configure();
        offer(length);
        response();
        confirmation();
        REQUIRE_EQ(listener.accept_confirmation(m3, now), Error::none);
    }
    void confirm_direction(bool calling)
    {
        auto& sender = calling ? caller : listener;
        auto& receiver = calling ? listener : caller;
        std::array<std::byte, 256> request {}, retry {}, response {},
            duplicate {};
        std::size_t n = 0, m = 0, r = 0, d = 0;
        REQUIRE_EQ(sender.key_request(request, n, now), Error::none);
        REQUIRE(n > authenticated_key_overhead);
        REQUIRE_EQ(sender.key_request(retry, r, now), Error::none);
        REQUIRE(
            equal(std::span {request}.first(n), std::span {retry}.first(r)));
        REQUIRE_EQ(receiver.accept_key_control(
                       false, std::span {request}.first(n), response, m, now),
            Error::none);
        REQUIRE_EQ(receiver.accept_key_control(
                       false, std::span {request}.first(n), duplicate, d, now),
            Error::none);
        REQUIRE(equal(
            std::span {response}.first(m), std::span {duplicate}.first(d)));
        REQUIRE_EQ(sender.accept_key_control(
                       true, std::span {response}.first(m), {}, r, now),
            Error::none);
        REQUIRE_EQ(r, 0U);
    }
};
}

TEST(peer_auth_admission_publishes_only_after_both_runtime_directions_confirm)
{
    for (unsigned length : {16U, 24U, 32U}) {
        for (bool listener_first : {false, true}) {
            AdmissionPair p;
            p.configure();
            p.offer(length);
            p.response();
            p.confirmation();
            REQUIRE(!p.caller.ready_to_publish(p.now));
            REQUIRE(!p.caller.release_crypto(p.now));
            REQUIRE(p.caller.peer_public_key(p.now).empty());
            REQUIRE(!p.listener.release_crypto(p.now));
            REQUIRE_EQ(
                p.listener.accept_confirmation(p.m3, p.now), Error::none);
            REQUIRE_EQ(p.cp.handles, 0U);
            REQUIRE_EQ(p.lp.handles, 0U);
            const auto c_derivations = p.cp.derivations,
                       l_derivations = p.lp.derivations;
            p.confirm_direction(!listener_first);
            REQUIRE(!p.caller.ready_to_publish(p.now));
            REQUIRE(!p.listener.ready_to_publish(p.now));
            p.confirm_direction(listener_first);
            REQUIRE(p.caller.ready_to_publish(p.now));
            REQUIRE(p.listener.ready_to_publish(p.now));
            REQUIRE(equal(p.caller.peer_public_key(p.now), public_key(2)));
            REQUIRE(equal(p.listener.peer_public_key(p.now), public_key(1)));
            REQUIRE_EQ(p.cp.derivations, c_derivations);
            REQUIRE_EQ(p.lp.derivations, l_derivations);
            auto caller = p.caller.release_crypto(p.now);
            auto listener = p.listener.release_crypto(p.now);
            REQUIRE(caller && listener);
            REQUIRE(!p.caller.release_crypto(p.now));
            REQUIRE(!p.listener.release_crypto(p.now));
            REQUIRE_EQ(caller->key_length(), length);
            REQUIRE(
                equal(p.caller.cached(pa::Message::confirmation, p.now), p.m3));
            p.caller.abort();
            p.listener.abort(); // transferred keys survive owner cleanup
            DataHeader header {.sequence = SequenceNumber {1},
                .message_number = 1,
                .boundary = MessageBoundary::solo,
                .encryption_key = caller->active_sender_key()};
            std::array<std::byte, 3> plaintext {
                std::byte {1}, std::byte {2}, std::byte {3}};
            std::array<std::byte, 3> ciphertext {}, opened {};
            std::array<std::byte, srt_gcm_authentication_tag_size> tag {};
            EncryptionKey key = EncryptionKey::none;
            REQUIRE_EQ(caller->seal(header, plaintext, ciphertext, tag, key),
                Error::none);
            REQUIRE_EQ(
                listener->open(header, ciphertext, tag, opened), Error::none);
            REQUIRE_EQ(opened, plaintext);
        }
    }
}

TEST(peer_auth_admission_loss_retries_and_invalid_envelopes_do_not_publish)
{
    AdmissionPair p;
    p.configure();
    p.offer();
    p.response();
    p.confirmation();
    // The first M3 was lost. Its exact cached retry completes the listener proof.
    REQUIRE_EQ(p.listener.accept_confirmation(
                   p.caller.cached(pa::Message::confirmation, p.now), p.now),
        Error::none);
    const auto derivations = p.lp.derivations;
    REQUIRE_EQ(p.listener.accept_confirmation(p.m3, p.now), Error::none);
    REQUIRE_EQ(p.lp.derivations, derivations);
    auto changed = p.m3;
    changed.back() ^= std::byte {1};
    REQUIRE_EQ(
        p.listener.accept_confirmation(changed, p.now), Error::invalid_state);
    std::array<std::byte, 256> request {}, response {};
    std::size_t n = 0, m = 0;
    REQUIRE_EQ(p.caller.key_request(request, n, p.now), Error::none);
    REQUIRE_EQ(p.caller.accept_key_control(
                   false, std::span {request}.first(n), response, m, p.now),
        Error::authentication_failure);
    REQUIRE_EQ(p.listener.accept_key_control(
                   false, std::span {request}.first(n), {}, m, p.now),
        Error::buffer_too_small);
    for (std::size_t i = 0; i < n; ++i) {
        request[i] ^= std::byte {1};
        REQUIRE_EQ(p.listener.accept_key_control(
                       false, std::span {request}.first(n), response, m, p.now),
            Error::authentication_failure);
        REQUIRE(!p.listener.ready_to_publish(p.now));
        request[i] ^= std::byte {1};
    }
    AdmissionPair other;
    other.proofs();
    REQUIRE_EQ(other.listener.accept_key_control(
                   false, std::span {request}.first(n), response, m, other.now),
        Error::authentication_failure);
    p.confirm_direction(true);
    p.confirm_direction(false);
    REQUIRE(p.caller.ready_to_publish(p.now));
    REQUIRE(p.listener.ready_to_publish(p.now));
}

TEST(peer_auth_admission_timeout_and_cancel_erase_each_pending_stage)
{
    for (unsigned stage = 0; stage < 6; ++stage) {
        for (bool cancel : {true, false}) {
            AdmissionPair p;
            p.configure();
            if (stage >= 1)
                p.offer();
            if (stage >= 2)
                p.response();
            if (stage >= 3)
                p.confirmation();
            if (stage >= 4)
                REQUIRE_EQ(
                    p.listener.accept_confirmation(p.m3, p.now), Error::none);
            if (stage >= 5) {
                p.confirm_direction(true);
                p.confirm_direction(false);
            }
            if (cancel) {
                p.caller.abort();
                p.listener.abort();
            } else {
                REQUIRE(p.caller.expire(p.deadline));
                REQUIRE(p.listener.expire(p.deadline));
            }
            REQUIRE(p.caller.failed());
            REQUIRE(p.listener.failed());
            REQUIRE(!p.caller.release_crypto(p.now));
            REQUIRE(!p.listener.release_crypto(p.now));
            REQUIRE_EQ(p.cp.handles, 0U);
            REQUIRE_EQ(p.lp.handles, 0U);
            REQUIRE(p.caller.cached(pa::Message::confirmation, p.now).empty());
            REQUIRE(p.listener.cached(pa::Message::response, p.now).empty());
        }
    }
}

TEST(
    peer_auth_admission_wrong_password_and_invalid_signature_budget_fail_closed)
{
    AdmissionPair wrong;
    wrong.configure(true);
    wrong.offer();
    wrong.response();
    wrong.confirmation();
    REQUIRE_EQ(wrong.listener.accept_confirmation(wrong.m3, wrong.now),
        Error::authentication_failure);
    REQUIRE(wrong.listener.failed());
    REQUIRE(!wrong.caller.ready_to_publish(wrong.now));
    REQUIRE(!wrong.caller.release_crypto(wrong.now));
    REQUIRE(wrong.caller.expire(wrong.deadline));
    AdmissionPair p;
    p.configure();
    p.offer();
    p.response();
    p.confirmation();
    p.m3[60] ^= std::byte {1};
    for (unsigned i = 0; i < 4; ++i) {
        REQUIRE_EQ(p.listener.accept_confirmation(p.m3, p.now),
            Error::authentication_failure);
        REQUIRE_EQ(p.listener.failed(), i == 3);
        REQUIRE_EQ(p.lp.derivations, 0U);
        REQUIRE(!p.listener.release_crypto(p.now));
    }
    REQUIRE_EQ(p.lp.handles, 0U);
}

TEST(peer_auth_admission_authenticated_invalid_key_material_aborts)
{
    AdmissionPair p;
    p.proofs();
    p.confirm_direction(true);
    p.confirm_direction(false);
    auto caller = p.caller.release_crypto(p.now);
    REQUIRE(caller != nullptr);
    std::array<std::byte, 3> invalid {
        std::byte {1}, std::byte {2}, std::byte {3}};
    std::array<std::byte, 256> request {}, response {};
    std::size_t n = 0, m = 0;
    REQUIRE_EQ(
        caller->session_authentication()->seal(false, invalid, request, n),
        Error::none);
    REQUIRE(p.listener.accept_key_control(
                false, std::span {request}.first(n), response, m, p.now)
        != Error::none);
    REQUIRE(p.listener.failed());
    REQUIRE(!p.listener.release_crypto(p.now));
}

TEST(
    peer_auth_listen_callback_sees_only_claimed_identity_after_cookie_validation)
{
    for (bool choose_required : {false, true}) {
        const auto pin = public_key(2);
        REQUIRE_EQ(srt_startup(), 0);
        struct Cleanup {
            ~Cleanup()
            {
                (void)srt_cleanup();
            }
        } cleanup;
        const auto listener = srt_create_socket();
        REQUIRE(listener != SRT_INVALID_SOCK);
        struct Close {
            SRTSOCKET id;
            ~Close()
            {
                (void)srt_close(id);
            }
        } close {listener};
        struct Offered {
            std::array<std::byte, 32> identifier {};
            std::atomic<bool> called {false};
            bool valid = false;
            bool choose_required = false;
            std::array<std::byte, 64> pin {};
            std::shared_ptr<compat::SocketRecord> child;
        } offered;
        offered.choose_required = choose_required;
        offered.pin = public_key(1);
        auto callback = +[](void* opaque, SRTSOCKET child, int, const sockaddr*,
                             const char*) -> int {
            auto& seen = *static_cast<Offered*>(opaque);
            int size = 31;
            const bool short_rejected =
                srt_getsockflag(child, SRTO_ROBOTWEAX_PEERAUTH_OFFERED,
                    seen.identifier.data(), &size)
                == SRT_ERROR;
            size = 32;
            seen.valid = short_rejected
                && srt_getsockflag(child, SRTO_ROBOTWEAX_PEERAUTH_OFFERED,
                       seen.identifier.data(), &size)
                    == 0
                && size == 32;
            std::array<std::byte, 64> authenticated {};
            size = 64;
            seen.valid = seen.valid
                && srt_getsockflag(child, SRTO_ROBOTWEAX_PEERAUTH_PEER,
                       authenticated.data(), &size)
                    == SRT_ERROR;
            seen.child = compat::SocketRegistry::instance().find(child);
            if (seen.choose_required) {
                const int enabled = 1, gcm = 2;
                std::array<std::byte, 32> identity {};
                identity.back() = std::byte {2};
                seen.valid = seen.valid
                    && srt_setsockflag(child, SRTO_PASSPHRASE,
                           "callback test passphrase", 24)
                        == 0
                    && srt_setsockflag(
                           child, SRTO_CRYPTOMODE, &gcm, sizeof(gcm))
                        == 0
                    && srt_setsockflag(child, SRTO_ROBOTWEAX_SESSIONAUTH,
                           &enabled, sizeof(enabled))
                        == 0
                    && srt_setsockflag(child, SRTO_ROBOTWEAX_PEERAUTH_KEY,
                           identity.data(), 32)
                        == 0
                    && srt_setsockflag(child, SRTO_ROBOTWEAX_PEERAUTH_TRUSTED,
                           seen.pin.data(), 64)
                        == 0
                    && srt_setsockflag(child, SRTO_ROBOTWEAX_PEERAUTH_POLICY,
                           &enabled, sizeof(enabled))
                        == 0;
            }
            seen.called.store(true);
            return seen.choose_required && seen.valid ? 0 : -1;
        };
        REQUIRE_EQ(srt_listen_callback(listener, callback, &offered), 0);
        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        REQUIRE_EQ(
            srt_bind(listener, reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)),
            0);
        REQUIRE_EQ(srt_listen(listener, 8), 0);
        int size = sizeof(address);
        REQUIRE_EQ(srt_getsockname(
                       listener, reinterpret_cast<sockaddr*>(&address), &size),
            0);
        const auto destination = IpEndpoint::loopback(ntohs(address.sin_port));
        UdpSocket udp;
        REQUIRE_EQ(udp.bind(IpEndpoint::loopback(0)), Error::none);
        HandshakeMachine machine({.role = ConnectionRole::caller,
            .local_socket_id = 123,
            .initial_sequence = SequenceNumber {1},
            .encryption_field = 2});
        const auto induction = machine.start();
        std::array<std::byte, 1500> wire {};
        const auto initial =
            encode_handshake_datagram(induction.values[0], {}, 0, wire);
        REQUIRE(initial);
        REQUIRE(udp.send_to(
            std::span {wire}.first(initial.bytes_written), destination));
        REQUIRE(udp.wait_readable(1000).ready);
        const auto received = udp.receive_from(wire);
        REQUIRE(received);
        const auto response = decode_handshake_datagram(
            std::span {wire}.first(received.bytes_transferred));
        REQUIRE(response);
        REQUIRE(!offered.called.load());
        auto actions = machine.receive(response.message);
        auto action = actions.values[0];
        REQUIRE_EQ(action.kind, HandshakeActionKind::send);
        pa::Exchange exchange(default_crypto_provider(), true);
        REQUIRE_EQ(exchange.configure(scalar(1), pin), Error::none);
        pa::Record record;
        REQUIRE_EQ(exchange.prepare_offer(16, record), Error::none);
        action.has_peer_authentication = true;
        action.packet.extension_field |= handshake_extension_flag_config;
        REQUIRE_EQ(pa::encode_record(record, action.peer_authentication.bytes,
                       action.peer_authentication.size),
            Error::none);
        const auto encoded = encode_handshake_datagram(
            action, {}, response.message.packet.socket_id, wire);
        REQUIRE(encoded);
        REQUIRE(udp.send_to(
            std::span {wire}.first(encoded.bytes_written), destination));
        REQUIRE(udp.wait_readable(1000).ready);
        const auto rejected = udp.receive_from(wire);
        REQUIRE(rejected);
        const auto rejection = decode_handshake_datagram(
            std::span {wire}.first(rejected.bytes_transferred));
        REQUIRE(rejection);
        if (choose_required)
            REQUIRE_EQ(static_cast<int>(rejection.message.packet.request),
                1000 + SRT_REJ_UNSECURE);
        REQUIRE(offered.called.load());
        REQUIRE(offered.valid);
        REQUIRE_EQ(offered.identifier, record.key_identifier);
        REQUIRE(offered.child != nullptr);
        {
            std::lock_guard lock(offered.child->mutex);
            REQUIRE(!offered.child->has_incoming_peer_authentication);
            REQUIRE(!offered.child->listen_callback_active);
        }
    }
}

TEST(peer_auth_admission_enforces_deadlines_on_entry_and_runtime_import_failure)
{
    AdmissionPair p;
    p.configure();
    p.offer();
    p.response();
    pa::Record record;
    REQUIRE_EQ(p.caller.prepare_confirmation(p.m2, record, p.deadline),
        Error::invalid_state);
    REQUIRE(p.caller.failed());
    REQUIRE_EQ(p.cp.handles, 0U);
    AdmissionPair ready;
    ready.proofs();
    ready.confirm_direction(true);
    ready.confirm_direction(false);
    REQUIRE(!ready.caller.release_crypto(ready.deadline));
    REQUIRE(ready.caller.failed());
    REQUIRE(ready.caller.peer_public_key(ready.now).empty());
    // Fault the runtime's first random draw after a successfully signed M3.
    AdmissionPair failed;
    failed.configure();
    failed.offer();
    failed.response();
    REQUIRE_EQ(
        failed.caller.prepare_confirmation(failed.m2, record, failed.now),
        Error::none);
    auto m3 = packet(record);
    // Use a successful run to count the sealing/import operations, then fail
    // each operation in that transition on a fresh exchange.
    const auto before = failed.cp.operations;
    REQUIRE_EQ(failed.caller.seal_confirmation(m3, failed.now), Error::none);
    const auto count = failed.cp.operations - before;
    for (unsigned operation = 1; operation <= count; ++operation) {
        AdmissionPair faulted;
        faulted.configure();
        faulted.offer();
        faulted.response();
        REQUIRE_EQ(faulted.caller.prepare_confirmation(
                       faulted.m2, record, faulted.now),
            Error::none);
        auto payload = packet(record);
        faulted.cp.fail_at = faulted.cp.operations + operation;
        REQUIRE(faulted.caller.seal_confirmation(payload, faulted.now)
            != Error::none);
        REQUIRE(faulted.caller.failed());
        REQUIRE_EQ(faulted.cp.handles, 0U);
        REQUIRE(!faulted.caller.release_crypto(faulted.now));
        REQUIRE(faulted.caller.cached(pa::Message::confirmation, faulted.now)
                .empty());
    }
}
