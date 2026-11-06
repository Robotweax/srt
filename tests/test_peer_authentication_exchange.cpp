// SPDX-License-Identifier: MIT
#include "test.hpp"
#include "peer_authentication.hpp"
#include "peer_authentication_admission.hpp"
#include "robotweax/srt/handshake.hpp"
#include "robotweax/srt/handshake_datagram.hpp"
#include "robotweax/srt/crypto.hpp"
#include "robotweax/srt/codec.hpp"
#include "robotweax/srt/socket_options.hpp"
#include "srt/srt.h"
#include "compat/socket_registry.hpp"
#include "compat/peer_authentication_handshake.hpp"
#include "compat/listener_handshake_sources.hpp"
#include "compat/runtime_work_executor_service.hpp"

#include <algorithm>
#include <atomic>
#include <vector>
#include <future>
#include <thread>

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

TEST(peer_auth_socket_options_and_close_erase_credentials)
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

    REQUIRE_EQ(srt_listen(socket, 8), 0);
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
        if (choose_required) {
            REQUIRE_EQ(
                rejection.message.packet.request, HandshakeRequest::conclusion);
            REQUIRE(rejection.message.has_peer_authentication);
            std::lock_guard lock(offered.child->mutex);
            REQUIRE_EQ(offered.child->state, SRTS_CONNECTING);
            REQUIRE(!offered.child->has_authenticated_peer);
        }
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

namespace {
using PeerHandshake = compat::PeerAuthenticationHandshake;
SocketOptions peer_handshake_options(bool caller, unsigned length = 16)
{
    SocketOptions options;
    REQUIRE_EQ(
        options.set_passphrase("packet adapter test passphrase"), Error::none);
    REQUIRE_EQ(options.set(SocketOption::crypto_mode, 2), Error::none);
    REQUIRE_EQ(
        options.set(SocketOption::session_authentication, 1), Error::none);
    REQUIRE_EQ(
        options.set(SocketOption::encryption_key_length, length), Error::none);
    REQUIRE_EQ(options.set_peer_authentication_key(scalar(caller ? 1 : 2)),
        Error::none);
    REQUIRE_EQ(
        options.set_peer_authentication_trusted(public_key(caller ? 2 : 1)),
        Error::none);
    REQUIRE_EQ(
        options.set(SocketOption::peer_authentication_policy, 1), Error::none);
    return options;
}
HandshakeMachine::Configuration peer_handshake_configuration(
    bool caller, const SocketOptions& options)
{
    HandshakeMachine::Configuration configuration;
    configuration.role =
        caller ? ConnectionRole::caller : ConnectionRole::listener;
    configuration.local_socket_id = caller ? 123 : 456;
    configuration.initial_sequence = SequenceNumber {1};
    configuration.extension_parameters = options.handshake_parameters();
    configuration.minimum_peer_srt_version = 0x010000;
    configuration.cookie_generator = +[](const Handshake&, void*) noexcept {
        return 0x12345678U;
    };
    return configuration;
}
struct PacketAdmissionPair {
    SocketOptions co, lo;
    TrackingProvider cp, lp;
    PeerHandshake caller, listener;
    PeerHandshake::Time now {};
    PeerHandshake::Time deadline = now + std::chrono::seconds {5};
    PeerHandshake::Result offer, response, confirmation, listener_keys;
    explicit PacketAdmissionPair(
        unsigned caller_length = 16, unsigned listener_length = 16)
        : co(peer_handshake_options(true, caller_length))
        , lo(peer_handshake_options(false, listener_length))
        , caller(peer_handshake_configuration(true, co), co, cp)
        , listener(peer_handshake_configuration(false, lo), lo, lp)
    {
    }
    void discovery()
    {
        REQUIRE(!listener.start(now, deadline).failed);
        auto initial = caller.start(now, deadline);
        REQUIRE_EQ(initial.size, 1U);
        auto cookie = listener.receive(initial.outgoing[0].view(), now);
        REQUIRE_EQ(cookie.size, 1U);
        REQUIRE_EQ(cp.operations, 0U);
        REQUIRE_EQ(lp.operations, 0U);
        offer = caller.receive(cookie.outgoing[0].view(), now);
        REQUIRE(!offer.failed);
        REQUIRE_EQ(offer.size, 1U);
        REQUIRE_EQ(lp.operations, 0U);
    }
    void proofs()
    {
        discovery();
        response = listener.receive(offer.outgoing[0].view(), now);
        REQUIRE(!response.failed);
        REQUIRE_EQ(response.size, 1U);
        REQUIRE(listener.transport(now) == nullptr);
        REQUIRE(!listener.ready(now));
        REQUIRE(!listener.release_crypto(now));
        confirmation = caller.receive(response.outgoing[0].view(), now);
        REQUIRE(!confirmation.failed);
        REQUIRE_EQ(confirmation.size, 2U);
        REQUIRE(caller.transport(now) == nullptr);
        REQUIRE(!caller.ready(now));
        REQUIRE(!caller.release_crypto(now));
        listener_keys = listener.receive(confirmation.outgoing[0].view(), now);
        REQUIRE(!listener_keys.failed);
        REQUIRE_EQ(listener_keys.size, 1U);
    }
    void confirm_keys()
    {
        auto listener_ack =
            listener.receive(confirmation.outgoing[1].view(), now);
        REQUIRE(!listener_ack.failed);
        REQUIRE_EQ(listener_ack.size, 1U);
        auto caller_ack = caller.receive(listener_keys.outgoing[0].view(), now);
        REQUIRE(!caller_ack.failed);
        REQUIRE_EQ(caller_ack.size, 1U);
        REQUIRE(!caller.ready(now));
        REQUIRE(!listener.ready(now));
        REQUIRE(caller.receive(listener_ack.outgoing[0].view(), now).ready);
        REQUIRE(listener.receive(caller_ack.outgoing[0].view(), now).ready);
    }
};
}

TEST(peer_auth_transport_adapter_negotiates_and_gates_publication)
{
    for (unsigned length : {0U, 16U, 24U, 32U}) {
        PacketAdmissionPair p(length, 24);
        p.proofs();
        p.confirm_keys();
        REQUIRE(p.caller.transport(p.now) != nullptr);
        REQUIRE(p.listener.transport(p.now) != nullptr);
        REQUIRE_EQ(
            p.caller.transport(p.now)->state(), HandshakeState::connected);
        REQUIRE_EQ(p.caller.peer_socket_id(), 456U);
        REQUIRE_EQ(p.listener.peer_socket_id(), 123U);
        REQUIRE(equal(p.caller.peer_public_key(p.now), public_key(2)));
        REQUIRE(equal(p.listener.peer_public_key(p.now), public_key(1)));
        auto caller = p.caller.release_crypto(p.now),
             listener = p.listener.release_crypto(p.now);
        REQUIRE(caller && listener);
        REQUIRE_EQ(caller->key_length(), length == 0 ? 24U : length);
        REQUIRE_EQ(listener->key_length(), caller->key_length());
        REQUIRE(!p.caller.release_crypto(p.now));
        DataHeader header {.sequence = SequenceNumber {1},
            .message_number = 1,
            .boundary = MessageBoundary::solo,
            .encryption_key = caller->active_sender_key()};
        std::array<std::byte, 1> plain {std::byte {42}}, cipher {}, opened {};
        std::array<std::byte, srt_gcm_authentication_tag_size> tag {};
        EncryptionKey key;
        REQUIRE_EQ(caller->seal(header, plain, cipher, tag, key), Error::none);
        REQUIRE_EQ(listener->open(header, cipher, tag, opened), Error::none);
        REQUIRE_EQ(opened, plain);
    }
}

TEST(peer_auth_transport_adapter_retries_lost_proofs_and_reordered_key_controls)
{
    PacketAdmissionPair p;
    p.discovery();
    p.response = p.listener.receive(p.offer.outgoing[0].view(), p.now);
    REQUIRE_EQ(p.response.size, 1U); // drop first response
    const auto operations = p.lp.operations;
    auto retry = p.caller.retry(p.now + std::chrono::milliseconds {10});
    REQUIRE_EQ(retry.size, 1U);
    REQUIRE(equal(retry.outgoing[0].view().subspan(packet_header_size),
        p.offer.outgoing[0].view().subspan(packet_header_size)));
    auto response = p.listener.receive(retry.outgoing[0].view(), p.now);
    REQUIRE_EQ(response.size, 1U);
    REQUIRE_EQ(p.lp.operations, operations);
    REQUIRE(equal(response.outgoing[0].view().subspan(packet_header_size),
        p.response.outgoing[0].view().subspan(packet_header_size)));
    p.confirmation = p.caller.receive(response.outgoing[0].view(), p.now);
    REQUIRE_EQ(p.confirmation.size, 2U);
    auto reordered =
        p.listener.receive(p.confirmation.outgoing[1].view(), p.now);
    REQUIRE(!reordered.failed);
    REQUIRE_EQ(reordered.size, 0U);
    auto caller_retry = p.caller.retry(p.now + std::chrono::milliseconds {20});
    REQUIRE_EQ(caller_retry.size, 2U);
    p.listener_keys =
        p.listener.receive(caller_retry.outgoing[0].view(), p.now);
    REQUIRE_EQ(p.listener_keys.size, 1U);
    p.confirm_keys();
}

TEST(peer_auth_transport_adapter_rejects_downgrade_and_bad_cookie_before_crypto)
{
    PacketAdmissionPair p;
    p.discovery();
    auto bad_cookie = p.offer.outgoing[0];
    bad_cookie.bytes[packet_header_size + 28] ^= std::byte {1};
    auto rejected = p.listener.receive(bad_cookie.view(), p.now);
    REQUIRE(!rejected.ready);
    REQUIRE_EQ(p.lp.operations, 0U);
    PacketAdmissionPair stripped;
    stripped.discovery();
    const auto decoded =
        decode_handshake_datagram(stripped.offer.outgoing[0].view());
    REQUIRE(decoded);
    HandshakeAction action {.kind = HandshakeActionKind::send,
        .packet = decoded.message.packet,
        .has_handshake_extension = true,
        .extension_type = HandshakeExtensionType::handshake_request,
        .extension_parameters = decoded.message.extension_parameters};
    std::array<std::byte, 1500> bytes {};
    const auto encoded = encode_handshake_datagram(action, {}, 456, bytes);
    REQUIRE(encoded);
    REQUIRE(stripped.listener
            .receive(
                std::span {bytes}.first(encoded.bytes_written), stripped.now)
            .failed);
    REQUIRE_EQ(stripped.lp.operations, 0U);
}

TEST(
    peer_auth_transport_adapter_rejects_wrong_routes_and_unauthenticated_runtime_keys)
{
    PacketAdmissionPair p;
    p.proofs();
    auto wrong_route = p.confirmation.outgoing[1];
    wrong_route.bytes[15] ^= std::byte {1};
    auto result = p.listener.receive(wrong_route.view(), p.now);
    REQUIRE(result.error != Error::none);
    REQUIRE(!result.failed);
    REQUIRE(!result.ready);
    auto invalid_tag = p.confirmation.outgoing[1];
    invalid_tag.bytes[invalid_tag.size - 1] ^= std::byte {1};
    result = p.listener.receive(invalid_tag.view(), p.now);
    REQUIRE_EQ(result.error, Error::authentication_failure);
    REQUIRE(!result.failed);
    auto legacy = p.confirmation.outgoing[1];
    legacy.bytes[2] = std::byte {};
    legacy.bytes[3] = std::byte {3};
    result = p.listener.receive(legacy.view(), p.now);
    REQUIRE(result.error != Error::none);
    REQUIRE(!result.failed);
    p.confirm_keys();
}

TEST(peer_auth_transport_adapter_deadline_and_m3_transport_binding)
{
    PacketAdmissionPair p;
    p.discovery();
    p.response = p.listener.receive(p.offer.outgoing[0].view(), p.now);
    p.confirmation = p.caller.receive(p.response.outgoing[0].view(), p.now);
    REQUIRE_EQ(p.confirmation.size, 2U);
    auto changed = p.confirmation.outgoing[0];
    changed.bytes[packet_header_size + 12] ^= std::byte {1}; // MSS
    REQUIRE(p.listener.receive(changed.view(), p.now).failed);
    REQUIRE_EQ(p.lp.derivations, 0U);
    REQUIRE_EQ(p.lp.handles, 0U);
    REQUIRE(p.caller.retry(p.deadline).failed);
    REQUIRE_EQ(p.cp.handles, 0U);
    REQUIRE(!p.caller.release_crypto(p.now));
}

TEST(
    peer_auth_transport_adapter_accepts_stateless_cookie_admission_on_child_route)
{
    auto co = peer_handshake_options(true, 0),
         lo = peer_handshake_options(false, 24);
    TrackingProvider cp, lp;
    PeerHandshake caller(peer_handshake_configuration(true, co), co, cp);
    const PeerHandshake::Time now {};
    const auto deadline = now + std::chrono::seconds {5};
    auto initial = caller.start(now, deadline);
    REQUIRE_EQ(initial.size, 1U);
    compat::StatelessListenerHandshakeRouter router(
        {.listener_socket_id = 999, .cookie_secret = {std::byte {42}}});
    const auto peer = IpEndpoint::loopback(40000);
    const auto decoded_initial =
        decode_handshake_datagram(initial.outgoing[0].view());
    REQUIRE(decoded_initial);
    auto induction = router.route(decoded_initial.message, peer, 7);
    REQUIRE_EQ(induction.kind,
        compat::ListenerHandshakeRouteKind::send_induction_response);
    std::array<std::byte, 1500> wire {};
    const auto encoded =
        encode_handshake_datagram(induction.response, {}, 123, wire);
    REQUIRE(encoded);
    auto offer =
        caller.receive(std::span {wire}.first(encoded.bytes_written), now);
    REQUIRE_EQ(offer.size, 1U);
    const auto decoded_offer =
        decode_handshake_datagram(offer.outgoing[0].view());
    REQUIRE(decoded_offer);
    auto route = router.route(decoded_offer.message, peer, 7);
    REQUIRE_EQ(
        route.kind, compat::ListenerHandshakeRouteKind::admit_conclusion);
    compat::ListenerHandshakeAdmission admitted {
        .initial = {.message = route.reconstructed_induction, .peer = peer},
        .conclusion = {.message = decoded_offer.message,
            .control = decoded_offer.control,
            .peer = peer},
        .protocol = route.protocol,
        .validated_cookie = route.validated_cookie};
    auto configuration = peer_handshake_configuration(false, lo);
    configuration.cookie_generator =
        +[](const Handshake&, void* cookie) noexcept {
            return *static_cast<std::uint32_t*>(cookie);
        };
    configuration.cookie_context = &route.validated_cookie;
    PeerHandshake listener(configuration, lo, lp);
    auto response = listener.start_admitted_listener(admitted, now, deadline);
    REQUIRE(!response.failed);
    REQUIRE_EQ(response.size, 1U);
    auto confirmation = caller.receive(response.outgoing[0].view(), now);
    REQUIRE(!confirmation.failed);
    REQUIRE_EQ(confirmation.size, 2U);
    REQUIRE_EQ(
        caller.peer_socket_id(), 456U); // child, not stateless listener 999
    auto keys = listener.receive(confirmation.outgoing[0].view(), now);
    REQUIRE_EQ(keys.size, 1U);
    auto caller_ack = caller.receive(keys.outgoing[0].view(), now);
    auto listener_ack = listener.receive(confirmation.outgoing[1].view(), now);
    REQUIRE_EQ(caller_ack.size, 1U);
    REQUIRE_EQ(listener_ack.size, 1U);
    REQUIRE(caller.receive(listener_ack.outgoing[0].view(), now).ready);
    REQUIRE(listener.receive(caller_ack.outgoing[0].view(), now).ready);
    REQUIRE(caller.release_crypto(now) != nullptr);
    REQUIRE(listener.release_crypto(now) != nullptr);
    TrackingProvider rejected_provider;
    PeerHandshake rejected(configuration, lo, rejected_provider);
    admitted.validated_cookie ^= 1;
    REQUIRE(rejected.start_admitted_listener(admitted, now, deadline).failed);
    REQUIRE_EQ(rejected_provider.operations, 0U);
}

TEST(peer_auth_transport_adapter_retry_budget_cleans_pending_secrets)
{
    auto options = peer_handshake_options(true);
    auto configuration = peer_handshake_configuration(true, options);
    configuration.maximum_retries = 1;
    TrackingProvider provider;
    PeerHandshake caller(configuration, options, provider);
    const PeerHandshake::Time now {};
    auto first = caller.start(now, now + std::chrono::seconds {5});
    REQUIRE_EQ(first.size, 1U);
    REQUIRE(!caller.retry(now).failed);
    REQUIRE(caller.retry(now).failed);
    REQUIRE(caller.transport(now) == nullptr);
    REQUIRE(!caller.release_crypto(now));
    REQUIRE_EQ(provider.handles, 0U);
}

TEST(peer_auth_transport_adapter_recovers_lost_runtime_ack_after_peer_is_ready)
{
    PacketAdmissionPair p;
    p.proofs();
    auto lost = p.listener.receive(p.confirmation.outgoing[1].view(), p.now);
    REQUIRE_EQ(lost.size, 1U); // drop caller's key acknowledgement
    auto caller_ack =
        p.caller.receive(p.listener_keys.outgoing[0].view(), p.now);
    REQUIRE_EQ(caller_ack.size, 1U);
    REQUIRE(p.listener.receive(caller_ack.outgoing[0].view(), p.now).ready);
    REQUIRE(!p.caller.ready(p.now));
    REQUIRE(!p.caller.release_crypto(p.now));
    const auto derivations = p.lp.derivations;
    auto retry = p.caller.retry(p.now + std::chrono::milliseconds {50});
    REQUIRE_EQ(retry.size, 2U);
    auto repeated_confirmation =
        p.listener.receive(retry.outgoing[0].view(), p.now);
    REQUIRE(!repeated_confirmation.failed);
    auto ack = p.listener.receive(retry.outgoing[1].view(), p.now);
    REQUIRE_EQ(ack.size, 1U);
    REQUIRE(p.caller.receive(ack.outgoing[0].view(), p.now).ready);
    REQUIRE_EQ(p.lp.derivations, derivations);
}

TEST(peer_auth_public_listener_waits_for_both_keys_and_retains_early_data)
{
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
    const int enabled = 1, gcm = 2, key_length = 16, timeout = 2000;
    const bool asynchronous = false;
    constexpr std::string_view password = "packet adapter test passphrase";
    REQUIRE_EQ(srt_setsockflag(
                   listener, SRTO_PASSPHRASE, password.data(), password.size()),
        0);
    REQUIRE_EQ(
        srt_setsockflag(listener, SRTO_CRYPTOMODE, &gcm, sizeof(gcm)), 0);
    REQUIRE_EQ(srt_setsockflag(
                   listener, SRTO_PBKEYLEN, &key_length, sizeof(key_length)),
        0);
    REQUIRE_EQ(srt_setsockflag(listener, SRTO_ROBOTWEAX_SESSIONAUTH, &enabled,
                   sizeof(enabled)),
        0);
    REQUIRE_EQ(srt_setsockflag(listener, SRTO_ROBOTWEAX_PEERAUTH_POLICY,
                   &enabled, sizeof(enabled)),
        0);
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE_EQ(srt_bind(listener, reinterpret_cast<const sockaddr*>(&address),
                   sizeof(address)),
        0);
    REQUIRE_EQ(srt_listen(listener, 8), SRT_ERROR); // incomplete credentials
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ESECFAIL);
    REQUIRE_EQ(srt_setsockflag(
                   listener, SRTO_ROBOTWEAX_PEERAUTH_KEY, scalar(2).data(), 32),
        0);
    REQUIRE_EQ(srt_setsockflag(listener, SRTO_ROBOTWEAX_PEERAUTH_TRUSTED,
                   public_key(1).data(), 64),
        0);
    REQUIRE_EQ(srt_setsockflag(
                   listener, SRTO_RCVSYN, &asynchronous, sizeof(asynchronous)),
        0);
    REQUIRE_EQ(
        srt_setsockflag(listener, SRTO_RCVTIMEO, &timeout, sizeof(timeout)), 0);
    REQUIRE_EQ(srt_listen(listener, 8), 0);
    int size = sizeof(address);
    REQUIRE_EQ(
        srt_getsockname(listener, reinterpret_cast<sockaddr*>(&address), &size),
        0);
    const auto destination = IpEndpoint::loopback(ntohs(address.sin_port));
    UdpSocket udp;
    REQUIRE_EQ(udp.bind(IpEndpoint::loopback(0)), Error::none);
    auto options = peer_handshake_options(true);
    PeerHandshake caller(peer_handshake_configuration(true, options), options);
    const auto now = std::chrono::steady_clock::now();
    const auto deadline = now + std::chrono::seconds {5};
    auto initial = caller.start(now, deadline);
    REQUIRE_EQ(initial.size, 1U);
    REQUIRE(udp.send_to(initial.outgoing[0].view(), destination));
    PeerHandshake::Datagram held_ack;
    std::array<std::byte, 1500> wire {};
    while (!caller.ready(std::chrono::steady_clock::now())) {
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        REQUIRE(udp.wait_readable(1000).ready);
        const auto received = udp.receive_from(wire);
        REQUIRE(received);
        auto result =
            caller.receive(std::span {wire}.first(received.bytes_transferred),
                std::chrono::steady_clock::now());
        REQUIRE(!result.failed);
        for (std::size_t i = 0; i < result.size; ++i) {
            const auto packet = decode_packet(result.outgoing[i].view());
            REQUIRE(packet);
            if (packet.packet.control.subtype == 0x7f04)
                held_ack = result.outgoing[i];
            else
                REQUIRE(udp.send_to(result.outgoing[i].view(), destination));
        }
    }
    REQUIRE(held_ack.size != 0);
    REQUIRE_EQ(srt_accept(listener, nullptr, nullptr), SRT_INVALID_SOCK);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EASYNCRCV);
    auto crypto = caller.release_crypto(std::chrono::steady_clock::now());
    REQUIRE(crypto != nullptr);
    DataHeader header {.sequence = SequenceNumber {1},
        .message_number = 1,
        .boundary = MessageBoundary::solo,
        .encryption_key = crypto->active_sender_key(),
        .destination_socket_id = caller.peer_socket_id()};
    const std::array<std::byte, 3> plain {
        std::byte {7}, std::byte {8}, std::byte {9}};
    std::array<std::byte, 3 + srt_gcm_authentication_tag_size>
        protected_data {};
    EncryptionKey selected {};
    REQUIRE_EQ(
        crypto->seal(header, plain, std::span {protected_data}.first<3>(),
            std::span {protected_data}.subspan<3>(), selected),
        Error::none);
    const auto encoded = encode_packet(
        {.kind = PacketKind::data, .data = header, .payload = protected_data},
        wire);
    REQUIRE(encoded);
    wire[encoded.bytes_written - 1] ^= std::byte {1};
    REQUIRE(udp.send_to(
        std::span {wire}.first(encoded.bytes_written), destination));
    wire[encoded.bytes_written - 1] ^= std::byte {1};
    REQUIRE(udp.send_to(
        std::span {wire}.first(encoded.bytes_written), destination));
    REQUIRE(udp.send_to(held_ack.view(), destination));
    // Restore blocking accept only after proving no child was published early.
    const bool synchronous = true;
    REQUIRE_EQ(srt_setsockflag(
                   listener, SRTO_RCVSYN, &synchronous, sizeof(synchronous)),
        0);
    const auto child = srt_accept(listener, nullptr, nullptr);
    REQUIRE(child != SRT_INVALID_SOCK);
    Close close_child {child};
    std::array<std::byte, 64> authenticated {};
    size = authenticated.size();
    REQUIRE_EQ(srt_getsockflag(child, SRTO_ROBOTWEAX_PEERAUTH_PEER,
                   authenticated.data(), &size),
        0);
    REQUIRE_EQ(authenticated, public_key(1));
    REQUIRE_EQ(
        srt_setsockflag(child, SRTO_RCVSYN, &synchronous, sizeof(synchronous)),
        0);
    std::array<char, 1500> received {};
    REQUIRE_EQ(srt_recvmsg(child, received.data(), received.size()), 3);
    REQUIRE(std::equal(plain.begin(), plain.end(),
        reinterpret_cast<const std::byte*>(received.data())));
    SRT_TRACEBSTATS stats {};
    REQUIRE_EQ(srt_bstats(child, &stats, 0), 0);
    REQUIRE_EQ(stats.pktRcvUndecryptTotal, 1);
    REQUIRE_EQ(srt_sendmsg(child, "reply", 5, -1, 0), 5);
    bool got_reply = false;
    while (!got_reply && std::chrono::steady_clock::now() < deadline) {
        REQUIRE(udp.wait_readable(1000).ready);
        const auto datagram = udp.receive_from(wire);
        REQUIRE(datagram);
        const auto packet =
            decode_packet(std::span {wire}.first(datagram.bytes_transferred));
        REQUIRE(packet);
        if (packet.packet.kind != PacketKind::data)
            continue;
        REQUIRE_EQ(
            packet.packet.payload.size(), 5 + srt_gcm_authentication_tag_size);
        std::array<std::byte, 5> reply {};
        REQUIRE_EQ(
            crypto->open(packet.packet.data, packet.packet.payload.first<5>(),
                packet.packet.payload.subspan<5>(), reply),
            Error::none);
        REQUIRE(std::equal(reply.begin(), reply.end(),
            reinterpret_cast<const std::byte*>("reply")));
        got_reply = true;
    }
    REQUIRE(got_reply);
    const auto retained = compat::SocketRegistry::instance().find(child);
    REQUIRE(retained != nullptr);
    REQUIRE_EQ(srt_close(child), 0);
    std::lock_guard lock(retained->mutex);
    REQUIRE(!retained->has_authenticated_peer);
    REQUIRE(std::all_of(retained->authenticated_peer.begin(),
        retained->authenticated_peer.end(), [](std::byte b) {
            return b == std::byte {};
        }));
}

TEST(peer_auth_public_listener_rejects_wrong_password_and_untrusted_identity)
{
    for (const bool wrong_password : {false, true}) {
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
        const int enabled = 1, gcm = 2, timeout = 2000;
        constexpr std::string_view password = "packet adapter test passphrase";
        REQUIRE_EQ(srt_setsockflag(listener, SRTO_PASSPHRASE, password.data(),
                       password.size()),
            0);
        REQUIRE_EQ(
            srt_setsockflag(listener, SRTO_CRYPTOMODE, &gcm, sizeof(gcm)), 0);
        REQUIRE_EQ(srt_setsockflag(listener, SRTO_ROBOTWEAX_SESSIONAUTH,
                       &enabled, sizeof(enabled)),
            0);
        REQUIRE_EQ(srt_setsockflag(listener, SRTO_ROBOTWEAX_PEERAUTH_KEY,
                       scalar(2).data(), 32),
            0);
        REQUIRE_EQ(srt_setsockflag(listener, SRTO_ROBOTWEAX_PEERAUTH_TRUSTED,
                       public_key(wrong_password ? 1 : 3).data(), 64),
            0);
        REQUIRE_EQ(srt_setsockflag(listener, SRTO_ROBOTWEAX_PEERAUTH_POLICY,
                       &enabled, sizeof(enabled)),
            0);
        REQUIRE_EQ(
            srt_setsockflag(listener, SRTO_RCVTIMEO, &timeout, sizeof(timeout)),
            0);
        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        REQUIRE_EQ(
            srt_bind(listener, reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)),
            0);
        const bool asynchronous = false;
        REQUIRE_EQ(srt_setsockflag(listener, SRTO_RCVSYN, &asynchronous,
                       sizeof(asynchronous)),
            0);
        REQUIRE_EQ(srt_listen(listener, 8), 0);
        int size = sizeof(address);
        REQUIRE_EQ(srt_getsockname(
                       listener, reinterpret_cast<sockaddr*>(&address), &size),
            0);
        const auto destination = IpEndpoint::loopback(ntohs(address.sin_port));
        UdpSocket udp;
        REQUIRE_EQ(udp.bind(IpEndpoint::loopback(0)), Error::none);
        auto options = peer_handshake_options(true);
        if (wrong_password)
            REQUIRE_EQ(
                options.set_passphrase("wrong packet adapter passphrase"),
                Error::none);
        PeerHandshake caller(
            peer_handshake_configuration(true, options), options);
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds {5};
        auto result = caller.start(std::chrono::steady_clock::now(), deadline);
        bool rejected = false;
        std::array<std::byte, 1500> wire {};
        while (!rejected && std::chrono::steady_clock::now() < deadline) {
            for (std::size_t i = 0; i < result.size; ++i)
                REQUIRE(udp.send_to(result.outgoing[i].view(), destination));
            REQUIRE(udp.wait_readable(1000).ready);
            const auto received = udp.receive_from(wire);
            REQUIRE(received);
            const auto bytes =
                std::span {wire}.first(received.bytes_transferred);
            const auto handshake = decode_handshake_datagram(bytes);
            if (handshake
                && static_cast<int>(handshake.message.packet.request) >= 1000) {
                REQUIRE_EQ(static_cast<int>(handshake.message.packet.request),
                    1000 + SRT_REJ_UNSECURE);
                rejected = true;
            } else {
                result =
                    caller.receive(bytes, std::chrono::steady_clock::now());
                REQUIRE(!result.failed);
                REQUIRE(!result.ready);
            }
        }
        REQUIRE(rejected);
        REQUIRE_EQ(srt_accept(listener, nullptr, nullptr), SRT_INVALID_SOCK);
        REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EASYNCRCV);
        REQUIRE(!caller.release_crypto(std::chrono::steady_clock::now()));
    }
}

namespace {
struct PublicPeerSockets {
    SRTSOCKET listener = SRT_INVALID_SOCK, caller = SRT_INVALID_SOCK,
              child = SRT_INVALID_SOCK;
    sockaddr_in address {};
    PublicPeerSockets()
    {
        (void)public_key(
            1); // Skip before owning sockets on providers without P-256.
        REQUIRE_EQ(srt_startup(), 0);
        listener = srt_create_socket();
        caller = srt_create_socket();
        REQUIRE(listener != SRT_INVALID_SOCK && caller != SRT_INVALID_SOCK);
        configure(listener, false);
        configure(caller, true);
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    }
    ~PublicPeerSockets()
    {
        if (child != SRT_INVALID_SOCK)
            (void)srt_close(child);
        (void)srt_close(caller);
        (void)srt_close(listener);
        (void)srt_cleanup();
    }
    static void configure(SRTSOCKET socket, bool calling)
    {
        const int enabled = 1, gcm = 2, timeout = 1500;
        constexpr std::string_view password =
            "public peer socket test passphrase";
        REQUIRE_EQ(srt_setsockflag(socket, SRTO_PASSPHRASE, password.data(),
                       password.size()),
            0);
        REQUIRE_EQ(
            srt_setsockflag(socket, SRTO_CRYPTOMODE, &gcm, sizeof(gcm)), 0);
        REQUIRE_EQ(srt_setsockflag(socket, SRTO_ROBOTWEAX_SESSIONAUTH, &enabled,
                       sizeof(enabled)),
            0);
        REQUIRE_EQ(srt_setsockflag(socket, SRTO_ROBOTWEAX_PEERAUTH_KEY,
                       scalar(calling ? 1 : 2).data(), 32),
            0);
        REQUIRE_EQ(srt_setsockflag(socket, SRTO_ROBOTWEAX_PEERAUTH_TRUSTED,
                       public_key(calling ? 2 : 1).data(), 64),
            0);
        REQUIRE_EQ(srt_setsockflag(socket, SRTO_ROBOTWEAX_PEERAUTH_POLICY,
                       &enabled, sizeof(enabled)),
            0);
        REQUIRE_EQ(
            srt_setsockflag(socket, SRTO_RCVTIMEO, &timeout, sizeof(timeout)),
            0);
        REQUIRE_EQ(
            srt_setsockflag(socket, SRTO_SNDTIMEO, &timeout, sizeof(timeout)),
            0);
        REQUIRE_EQ(
            srt_setsockflag(socket, SRTO_CONNTIMEO, &timeout, sizeof(timeout)),
            0);
    }
    void listen()
    {
        REQUIRE_EQ(
            srt_bind(listener, reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)),
            0);
        REQUIRE_EQ(srt_listen(listener, 8), 0);
        int size = sizeof(address);
        REQUIRE_EQ(srt_getsockname(
                       listener, reinterpret_cast<sockaddr*>(&address), &size),
            0);
    }
    int connect()
    {
        return srt_connect(caller, reinterpret_cast<const sockaddr*>(&address),
            sizeof(address));
    }
};
struct PublicPeerCallback {
    struct Result {
        int error;
        bool authenticated;
    };
    std::promise<Result> promise;
    static void observe(
        void* opaque, SRTSOCKET socket, int error, const sockaddr*, int)
    {
        std::array<std::byte, 64> key {};
        int size = key.size();
        const bool authenticated = srt_getsockstate(socket) == SRTS_CONNECTED
            && srt_getsockflag(
                   socket, SRTO_ROBOTWEAX_PEERAUTH_PEER, key.data(), &size)
                == 0
            && key == public_key(2);
        static_cast<PublicPeerCallback*>(opaque)->promise.set_value(
            {error, authenticated});
    }
};
}

TEST(peer_auth_public_sockets_blocking_and_async_bidirectional_data)
{
    for (const bool asynchronous : {false, true}) {
        PublicPeerCallback callback;
        PublicPeerSockets sockets;
        auto future = callback.promise.get_future();
        REQUIRE_EQ(srt_connect_callback(
                       sockets.caller, PublicPeerCallback::observe, &callback),
            0);
        const bool synchronous = !asynchronous;
        REQUIRE_EQ(srt_setsockflag(sockets.caller, SRTO_RCVSYN, &synchronous,
                       sizeof(synchronous)),
            0);
        sockets.listen();
        REQUIRE_EQ(sockets.connect(), 0);
        REQUIRE(future.wait_for(std::chrono::seconds {3})
            == std::future_status::ready);
        const auto result = future.get();
        REQUIRE_EQ(result.error, SRT_SUCCESS);
        REQUIRE(result.authenticated);
        const bool blocking_receive = true;
        REQUIRE_EQ(srt_setsockflag(sockets.caller, SRTO_RCVSYN,
                       &blocking_receive, sizeof(blocking_receive)),
            0);
        sockets.child = srt_accept(sockets.listener, nullptr, nullptr);
        REQUIRE(sockets.child != SRT_INVALID_SOCK);
        for (const auto socket : {sockets.caller, sockets.child}) {
            std::array<std::byte, 64> key {};
            int size = key.size();
            REQUIRE_EQ(srt_getsockflag(socket, SRTO_ROBOTWEAX_PEERAUTH_PEER,
                           key.data(), &size),
                0);
            REQUIRE_EQ(key, public_key(socket == sockets.caller ? 2 : 1));
        }
        for (const bool from_caller : {true, false}) {
            const auto sender = from_caller ? sockets.caller : sockets.child;
            const auto receiver = from_caller ? sockets.child : sockets.caller;
            REQUIRE_EQ(
                srt_sendmsg(sender, "authenticated data", 18, -1, 0), 18);
            std::array<char, 1500> received {};
            REQUIRE_EQ(
                srt_recvmsg(receiver, received.data(), received.size()), 18);
            REQUIRE_EQ(std::string_view(received.data(), 18),
                std::string_view("authenticated data"));
        }
    }
}

TEST(peer_auth_public_caller_rejection_never_publishes_authenticated_peer)
{
    for (const bool nonblocking : {false, true})
        for (const int scenario : {0, 1, 2, 3}) {
            PublicPeerCallback callback;
            PublicPeerSockets sockets;
            auto future = callback.promise.get_future();
            REQUIRE_EQ(srt_connect_callback(sockets.caller,
                           PublicPeerCallback::observe, &callback),
                0);
            if (scenario == 0)
                REQUIRE_EQ(srt_setsockflag(sockets.caller, SRTO_PASSPHRASE,
                               "wrong public passphrase", 23),
                    0);
            else if (scenario == 1)
                REQUIRE_EQ(srt_setsockflag(sockets.caller,
                               SRTO_ROBOTWEAX_PEERAUTH_TRUSTED,
                               public_key(3).data(), 64),
                    0);
            else if (scenario == 2)
                REQUIRE_EQ(srt_setsockflag(sockets.listener,
                               SRTO_ROBOTWEAX_PEERAUTH_TRUSTED,
                               public_key(3).data(), 64),
                    0);
            else {
                const int off = 0;
                REQUIRE_EQ(
                    srt_setsockflag(sockets.listener,
                        SRTO_ROBOTWEAX_PEERAUTH_POLICY, &off, sizeof(off)),
                    0);
            }
            const bool asynchronous = false;
            REQUIRE_EQ(srt_setsockflag(sockets.listener, SRTO_RCVSYN,
                           &asynchronous, sizeof(asynchronous)),
                0);
            const bool synchronous = !nonblocking;
            REQUIRE_EQ(srt_setsockflag(sockets.caller, SRTO_RCVSYN,
                           &synchronous, sizeof(synchronous)),
                0);
            sockets.listen();
            REQUIRE_EQ(sockets.connect(), nonblocking ? 0 : SRT_ERROR);
            if (!nonblocking)
                REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ESECFAIL);
            REQUIRE(future.wait_for(std::chrono::seconds {3})
                == std::future_status::ready);
            const auto result = future.get();
            REQUIRE_EQ(result.error, SRT_ESECFAIL);
            REQUIRE(!result.authenticated);
            REQUIRE_EQ(srt_accept(sockets.listener, nullptr, nullptr),
                SRT_INVALID_SOCK);
            REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EASYNCRCV);
            std::array<char, 64> key {};
            int size = key.size();
            REQUIRE_EQ(srt_getsockflag(sockets.caller,
                           SRTO_ROBOTWEAX_PEERAUTH_PEER, key.data(), &size),
                SRT_ERROR);
            REQUIRE_EQ(
                srt_sendmsg(sockets.caller, "blocked", 7, -1, 0), SRT_ERROR);
        }
}

TEST(peer_auth_public_caller_timeout_and_close_cancel_pending_setup)
{
    for (const bool cancel : {false, true}) {
        PublicPeerCallback callback;
        PublicPeerSockets sockets;
        auto future = callback.promise.get_future();
        UdpSocket blackhole;
        REQUIRE_EQ(blackhole.bind(IpEndpoint::loopback(0)), Error::none);
        // Bind a destination owned by us without answering discovery.
        const auto endpoint = blackhole.local_endpoint();
        REQUIRE(endpoint);
        sockets.address.sin_port = htons(endpoint.endpoint.port);
        const bool synchronous = false;
        const int timeout = 200;
        REQUIRE_EQ(srt_setsockflag(sockets.caller, SRTO_RCVSYN, &synchronous,
                       sizeof(synchronous)),
            0);
        REQUIRE_EQ(srt_setsockflag(sockets.caller, SRTO_CONNTIMEO, &timeout,
                       sizeof(timeout)),
            0);
        REQUIRE_EQ(srt_connect_callback(
                       sockets.caller, PublicPeerCallback::observe, &callback),
            0);
        REQUIRE_EQ(sockets.connect(), 0);
        REQUIRE(blackhole.wait_readable(1000).ready);
        if (cancel)
            REQUIRE_EQ(srt_close(sockets.caller), 0);
        REQUIRE(future.wait_for(std::chrono::seconds {3})
            == std::future_status::ready);
        const auto result = future.get();
        REQUIRE_EQ(result.error, cancel ? SRT_ESCLOSED : SRT_ENOSERVER);
        REQUIRE(!result.authenticated);
    }
}

TEST(
    peer_auth_public_sockets_recover_loss_duplicates_and_reordered_confirmation)
{
    PublicPeerSockets sockets;
    const int timeout = 4000;
    REQUIRE_EQ(srt_setsockflag(
                   sockets.caller, SRTO_CONNTIMEO, &timeout, sizeof(timeout)),
        0);
    REQUIRE_EQ(srt_setsockflag(
                   sockets.listener, SRTO_CONNTIMEO, &timeout, sizeof(timeout)),
        0);
    sockets.listen();
    const auto listener = IpEndpoint::loopback(ntohs(sockets.address.sin_port));
    UdpSocket relay;
    REQUIRE_EQ(relay.bind(IpEndpoint::loopback(0)), Error::none);
    const auto endpoint = relay.local_endpoint();
    REQUIRE(endpoint);
    sockets.address.sin_port = htons(endpoint.endpoint.port);
    std::atomic<unsigned> faults {0};
    std::atomic<bool> io_failed {false};
    std::jthread forwarding([&](std::stop_token stop) {
        IpEndpoint caller {};
        bool have_caller = false, dropped_response = false, reordered = false;
        bool dropped_ack[2] {};
        std::vector<std::byte> held_confirmation;
        std::array<std::byte, 1500> wire {};
        while (!stop.stop_requested()) {
            if (!relay.wait_readable(10).ready)
                continue;
            const auto received = relay.receive_from(wire);
            if (!received) {
                io_failed = true;
                return;
            }
            const bool from_listener = received.peer == listener;
            if (!from_listener) {
                caller = received.peer;
                have_caller = true;
            }
            if (!have_caller)
                continue;
            const auto destination = from_listener ? caller : listener;
            const auto bytes =
                std::span {wire}.first(received.bytes_transferred);
            const auto packet = decode_packet(bytes);
            if (!packet)
                continue;
            const auto handshake = decode_handshake_datagram(bytes);
            if (from_listener && handshake
                && handshake.message.has_peer_authentication
                && !dropped_response) {
                dropped_response = true;
                ++faults;
                continue;
            }
            if (!from_listener && handshake
                && handshake.message.packet.request
                    == HandshakeRequest::agreement
                && !reordered) {
                held_confirmation.assign(bytes.begin(), bytes.end());
                continue;
            }
            const bool key_request = packet.packet.kind == PacketKind::control
                && packet.packet.control.type == ControlType::user_defined
                && packet.packet.control.subtype == 0x7f03;
            const bool key_response = packet.packet.kind == PacketKind::control
                && packet.packet.control.type == ControlType::user_defined
                && packet.packet.control.subtype == 0x7f04;
            if (key_response && !dropped_ack[from_listener]) {
                dropped_ack[from_listener] = true;
                ++faults;
                continue;
            }
            if (!relay.send_to(bytes, destination)) {
                io_failed = true;
                return;
            }
            if (!from_listener && key_request && !held_confirmation.empty()) {
                // Deliver the caller's KMREQ before M3, then duplicate M3.
                if (!relay.send_to(held_confirmation, listener)
                    || !relay.send_to(held_confirmation, listener)) {
                    io_failed = true;
                    return;
                }
                held_confirmation.clear();
                reordered = true;
                ++faults;
            }
        }
    });
    REQUIRE_EQ(sockets.connect(), 0);
    // A caller may send before the listener receives its final KMRSP retry.
    REQUIRE_EQ(srt_sendmsg(sockets.caller, "after retries", 13, -1, 0), 13);
    sockets.child = srt_accept(sockets.listener, nullptr, nullptr);
    REQUIRE(sockets.child != SRT_INVALID_SOCK);
    std::array<char, 1500> received {};
    REQUIRE_EQ(
        srt_recvmsg(sockets.child, received.data(), received.size()), 13);
    REQUIRE_EQ(std::string_view(received.data(), 13),
        std::string_view("after retries"));
    REQUIRE_EQ(srt_sendmsg(sockets.child, "reply", 5, -1, 0), 5);
    REQUIRE_EQ(
        srt_recvmsg(sockets.caller, received.data(), received.size()), 5);
    REQUIRE_EQ(std::string_view(received.data(), 5), std::string_view("reply"));
    REQUIRE_EQ(faults.load(), 4U);
    REQUIRE(!io_failed.load());
}

TEST(
    peer_auth_public_caller_gates_callback_data_and_identity_until_final_key_ack)
{
    for (const bool cancel : {false, true}) {
        PublicPeerCallback callback;
        PublicPeerSockets sockets;
        auto future = callback.promise.get_future();
        const bool asynchronous = false;
        REQUIRE_EQ(srt_setsockflag(sockets.caller, SRTO_RCVSYN, &asynchronous,
                       sizeof(asynchronous)),
            0);
        REQUIRE_EQ(srt_setsockflag(sockets.caller, SRTO_SNDSYN, &asynchronous,
                       sizeof(asynchronous)),
            0);
        REQUIRE_EQ(srt_connect_callback(
                       sockets.caller, PublicPeerCallback::observe, &callback),
            0);
        sockets.listen();
        const auto listener =
            IpEndpoint::loopback(ntohs(sockets.address.sin_port));
        UdpSocket relay;
        REQUIRE_EQ(relay.bind(IpEndpoint::loopback(0)), Error::none);
        const auto endpoint = relay.local_endpoint();
        REQUIRE(endpoint);
        sockets.address.sin_port = htons(endpoint.endpoint.port);
        std::promise<void> withheld, early_data;
        auto withheld_future = withheld.get_future();
        auto early_data_future = early_data.get_future();
        std::atomic<bool> release {false};
        std::atomic<bool> io_failed {false};
        std::jthread forwarding([&](std::stop_token stop) {
            IpEndpoint caller {};
            bool have_caller = false, notified = false, data_notified = false;
            std::vector<std::byte> held_ack;
            std::array<std::byte, 1500> wire {};
            while (!stop.stop_requested()) {
                if (release && !held_ack.empty()) {
                    if (!relay.send_to(held_ack, caller)) {
                        io_failed = true;
                        return;
                    }
                    held_ack.clear();
                }
                if (!relay.wait_readable(10).ready)
                    continue;
                const auto received = relay.receive_from(wire);
                if (!received) {
                    io_failed = true;
                    return;
                }
                const bool from_listener = received.peer == listener;
                if (!from_listener) {
                    caller = received.peer;
                    have_caller = true;
                }
                if (!have_caller)
                    continue;
                const auto bytes =
                    std::span {wire}.first(received.bytes_transferred);
                const auto packet = decode_packet(bytes);
                if (from_listener && packet
                    && packet.packet.kind == PacketKind::control
                    && packet.packet.control.type == ControlType::user_defined
                    && packet.packet.control.subtype == 0x7f04 && !release) {
                    held_ack.assign(bytes.begin(), bytes.end());
                    if (!notified) {
                        notified = true;
                        withheld.set_value();
                    }
                    continue;
                }
                if (!relay.send_to(bytes, from_listener ? caller : listener)) {
                    io_failed = true;
                    return;
                }
                if (from_listener && packet
                    && packet.packet.kind == PacketKind::data
                    && !data_notified) {
                    data_notified = true;
                    early_data.set_value();
                }
            }
        });
        REQUIRE_EQ(sockets.connect(), 0);
        REQUIRE(withheld_future.wait_for(std::chrono::seconds {2})
            == std::future_status::ready);
        REQUIRE_EQ(srt_getsockstate(sockets.caller), SRTS_CONNECTING);
        REQUIRE(future.wait_for(std::chrono::milliseconds {0})
            == std::future_status::timeout);
        std::array<std::byte, 64> identity {};
        int size = identity.size();
        REQUIRE_EQ(srt_getsockflag(sockets.caller, SRTO_ROBOTWEAX_PEERAUTH_PEER,
                       identity.data(), &size),
            SRT_ERROR);
        REQUIRE_EQ(
            srt_sendmsg(sockets.caller, "too soon", 8, -1, 0), SRT_ERROR);
        const auto retained =
            compat::SocketRegistry::instance().find(sockets.caller);
        REQUIRE(retained != nullptr);
        {
            std::lock_guard lock(retained->mutex);
            REQUIRE(retained->crypto == nullptr);
            REQUIRE(retained->runtime == nullptr);
        }
        sockets.child = srt_accept(sockets.listener, nullptr, nullptr);
        REQUIRE(sockets.child != SRT_INVALID_SOCK);
        REQUIRE_EQ(srt_sendmsg(sockets.child, "early reply", 11, -1, 0), 11);
        REQUIRE(early_data_future.wait_for(std::chrono::seconds {2})
            == std::future_status::ready);
        std::array<char, 1500> received {};
        REQUIRE_EQ(
            srt_recvmsg(sockets.caller, received.data(), received.size()),
            SRT_ERROR);
        if (cancel)
            REQUIRE_EQ(srt_close(sockets.caller), 0);
        else
            release = true;
        REQUIRE(future.wait_for(std::chrono::seconds {3})
            == std::future_status::ready);
        const auto result = future.get();
        REQUIRE_EQ(result.error, cancel ? SRT_ESCLOSED : SRT_SUCCESS);
        REQUIRE_EQ(result.authenticated, !cancel);
        REQUIRE(!io_failed);
        if (!cancel) {
            const bool synchronous = true;
            REQUIRE_EQ(srt_setsockflag(sockets.caller, SRTO_RCVSYN,
                           &synchronous, sizeof(synchronous)),
                0);
            REQUIRE_EQ(
                srt_recvmsg(sockets.caller, received.data(), received.size()),
                11);
            REQUIRE_EQ(std::string_view(received.data(), 11),
                std::string_view("early reply"));
        }
        if (cancel) {
            std::lock_guard lock(retained->mutex);
            REQUIRE(!retained->has_authenticated_peer);
            REQUIRE(!retained->native_options.peer_authentication_required());
            REQUIRE(
                retained->native_options.peer_authentication_trusted().empty());
        }
    }
}

TEST(peer_auth_public_rotation_and_authenticated_fec_recover_loss)
{
    for (const int key_length : {16, 24, 32}) {
        for (const bool fec : {false, true}) {
            PublicPeerSockets sockets;
            for (const auto socket : {sockets.caller, sockets.listener}) {
                const int refresh = 16, preannounce = 4, latency = 80;
                REQUIRE_EQ(srt_setsockflag(socket, SRTO_PBKEYLEN, &key_length,
                               sizeof(key_length)),
                    0);
                REQUIRE_EQ(srt_setsockflag(socket, SRTO_KMPREANNOUNCE,
                               &preannounce, sizeof(preannounce)),
                    0);
                REQUIRE_EQ(srt_setsockflag(socket, SRTO_KMREFRESHRATE, &refresh,
                               sizeof(refresh)),
                    0);
                REQUIRE_EQ(srt_setsockflag(
                               socket, SRTO_LATENCY, &latency, sizeof(latency)),
                    0);
                if (fec) {
                    constexpr std::string_view filter =
                        "fec,cols:4,rows:1,arq:never";
                    REQUIRE_EQ(srt_setsockflag(socket, SRTO_PACKETFILTER,
                                   filter.data(), filter.size()),
                        0);
                }
            }
            sockets.listen();
            const auto listener =
                IpEndpoint::loopback(ntohs(sockets.address.sin_port));
            UdpSocket relay;
            REQUIRE_EQ(relay.bind(IpEndpoint::loopback(0)), Error::none);
            const auto endpoint = relay.local_endpoint();
            REQUIRE(endpoint);
            sockets.address.sin_port = htons(endpoint.endpoint.port);
            std::array<std::atomic<unsigned>, 2> source {}, parity {},
                dropped {}, transitions {};
            std::atomic<bool> io_failed {false};
            std::jthread forwarding([&](std::stop_token stop) {
                IpEndpoint caller {};
                bool have_caller = false;
                std::array<EncryptionKey, 2> previous {
                    EncryptionKey::none, EncryptionKey::none};
                std::array<bool, 2> corrupted {};
                std::array<std::byte, 1500> wire {};
                while (!stop.stop_requested()) {
                    if (!relay.wait_readable(10).ready)
                        continue;
                    const auto received = relay.receive_from(wire);
                    if (!received) {
                        io_failed = true;
                        return;
                    }
                    const bool from_listener = received.peer == listener;
                    const auto direction = static_cast<unsigned>(from_listener);
                    if (!from_listener) {
                        caller = received.peer;
                        have_caller = true;
                    }
                    if (!have_caller)
                        continue;
                    const auto bytes =
                        std::span {wire}.first(received.bytes_transferred);
                    const auto packet = decode_packet(bytes);
                    const auto destination = from_listener ? caller : listener;
                    if (packet && packet.packet.kind == PacketKind::data) {
                        if (packet.packet.data.message_number != 0
                            && !packet.packet.data.retransmitted) {
                            const auto count = ++source[direction];
                            if (previous[direction]
                                != packet.packet.data.encryption_key) {
                                if (previous[direction] != EncryptionKey::none)
                                    ++transitions[direction];
                                previous[direction] =
                                    packet.packet.data.encryption_key;
                            }
                            if (fec && count % 8 == 2) {
                                ++dropped[direction];
                                continue;
                            }
                        } else if (packet.packet.data.message_number == 0) {
                            ++parity[direction];
                            if (!corrupted[direction]) {
                                // A forged parity packet must not poison recovery.
                                wire[received.bytes_transferred - 1] ^=
                                    std::byte {1};
                                if (!relay.send_to(bytes, destination)) {
                                    io_failed = true;
                                    return;
                                }
                                wire[received.bytes_transferred - 1] ^=
                                    std::byte {1};
                                corrupted[direction] = true;
                            }
                        }
                    }
                    if (!relay.send_to(bytes, destination)) {
                        io_failed = true;
                        return;
                    }
                }
            });
            REQUIRE_EQ(sockets.connect(), 0);
            sockets.child = srt_accept(sockets.listener, nullptr, nullptr);
            REQUIRE(sockets.child != SRT_INVALID_SOCK);
            for (unsigned batch = 0; batch < 12; ++batch) {
                for (const bool reverse : {false, true}) {
                    const auto sender =
                        reverse ? sockets.child : sockets.caller;
                    const auto receiver =
                        reverse ? sockets.caller : sockets.child;
                    std::array<std::array<char, 64>, 8> messages {};
                    for (unsigned i = 0; i < messages.size(); ++i) {
                        for (unsigned j = 0; j < messages[i].size(); ++j)
                            messages[i][j] = static_cast<char>(
                                batch * 8 + i + j + (reverse ? 97 : 0));
                        REQUIRE_EQ(srt_sendmsg(sender, messages[i].data(),
                                       messages[i].size(), -1, 0),
                            64);
                    }
                    for (const auto& expected : messages) {
                        std::array<char, 1500> received {};
                        REQUIRE_EQ(srt_recvmsg(receiver, received.data(),
                                       received.size()),
                            64);
                        REQUIRE(std::equal(expected.begin(), expected.end(),
                            received.begin()));
                    }
                }
            }
            REQUIRE(!io_failed);
            for (unsigned direction = 0; direction < 2; ++direction) {
                REQUIRE_EQ(source[direction].load(), 96U);
                REQUIRE(transitions[direction].load() >= 4U);
                SRT_TRACEBSTATS stats {};
                const auto receiver =
                    direction ? sockets.caller : sockets.child;
                REQUIRE_EQ(srt_bstats(receiver, &stats, 0), 0);
                // The only receiver drop is the injected unauthenticated parity.
                REQUIRE_EQ(stats.pktRcvDropTotal, fec ? 1 : 0);
                if (fec) {
                    REQUIRE_EQ(dropped[direction].load(), 12U);
                    REQUIRE(parity[direction].load() >= 24U);
                    REQUIRE_EQ(stats.pktRcvFilterSupplyTotal, 12);
                    REQUIRE_EQ(stats.pktRcvUndecryptTotal, 1);
                } else {
                    REQUIRE_EQ(stats.pktRcvUndecryptTotal, 0);
                }
            }
        }
    }
}

namespace {
struct PeerWorkGate {
    std::mutex mutex;
    std::condition_variable changed;
    std::size_t started = 0, drained = 0;
    bool released = false;
    static void block(void* opaque) noexcept
    {
        auto& gate = *static_cast<PeerWorkGate*>(opaque);
        std::unique_lock lock(gate.mutex);
        ++gate.started;
        gate.changed.notify_all();
        gate.changed.wait(lock, [&] {
            return gate.released;
        });
    }
    static void count(void* opaque) noexcept
    {
        auto& gate = *static_cast<PeerWorkGate*>(opaque);
        std::lock_guard lock(gate.mutex);
        ++gate.drained;
        gate.changed.notify_all();
    }
    void release()
    {
        std::lock_guard lock(mutex);
        released = true;
        changed.notify_all();
    }
};
struct PeerWorkGateRelease {
    std::shared_ptr<PeerWorkGate> gate;
    ~PeerWorkGateRelease()
    {
        gate->release();
    }
};
}

TEST(peer_auth_public_worker_saturation_fails_bounded_and_recovers)
{
    PublicPeerSockets sockets;
    sockets.listen();
    const auto executor = compat::acquire_runtime_work_executor();
    REQUIRE(executor != nullptr);
    const auto capacity = executor->snapshot();
    const auto gate = std::make_shared<PeerWorkGate>();
    PeerWorkGateRelease release {
        gate}; // Release before socket cleanup even on assertion failure.
    for (std::size_t i = 0; i < capacity.worker_count; ++i)
        REQUIRE_EQ(executor->submit(
                       {.function = PeerWorkGate::block, .context = gate}),
            compat::RuntimeWorkExecutor::SubmitStatus::accepted);
    {
        std::unique_lock lock(gate->mutex);
        REQUIRE(gate->changed.wait_for(lock, std::chrono::seconds {2}, [&] {
            return gate->started == capacity.worker_count;
        }));
    }
    for (std::size_t i = 0; i < capacity.queue_capacity; ++i)
        REQUIRE_EQ(executor->submit(
                       {.function = PeerWorkGate::count, .context = gate}),
            compat::RuntimeWorkExecutor::SubmitStatus::accepted);
    REQUIRE_EQ(sockets.connect(), SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ENOBUF);
    const auto full = executor->snapshot();
    REQUIRE_EQ(full.queued, capacity.queue_capacity);
    REQUIRE_EQ(full.executing, capacity.worker_count);
    REQUIRE(full.rejected_full > capacity.rejected_full);
    const auto failed = compat::SocketRegistry::instance().find(sockets.caller);
    REQUIRE(failed != nullptr);
    {
        std::lock_guard lock(failed->mutex);
        REQUIRE(failed->crypto == nullptr);
        REQUIRE(failed->runtime == nullptr);
        REQUIRE(failed->connect_handshake_operation == nullptr);
        REQUIRE(!failed->has_authenticated_peer);
    }
    REQUIRE_EQ(srt_close(sockets.caller), 0);
    REQUIRE(failed->native_options.peer_authentication_trusted().empty());
    gate->release();
    {
        std::unique_lock lock(gate->mutex);
        REQUIRE(gate->changed.wait_for(lock, std::chrono::seconds {3}, [&] {
            return gate->drained == capacity.queue_capacity;
        }));
    }
    sockets.caller = srt_create_socket();
    REQUIRE(sockets.caller != SRT_INVALID_SOCK);
    PublicPeerSockets::configure(sockets.caller, true);
    REQUIRE_EQ(sockets.connect(), 0);
    sockets.child = srt_accept(sockets.listener, nullptr, nullptr);
    REQUIRE(sockets.child != SRT_INVALID_SOCK);
}

TEST(
    peer_auth_public_concurrent_admissions_keep_identities_and_payloads_isolated)
{
    PublicPeerSockets sockets;
    const int timeout = 5000;
    REQUIRE_EQ(srt_setsockflag(
                   sockets.listener, SRTO_CONNTIMEO, &timeout, sizeof(timeout)),
        0);
    std::vector<std::byte> pins;
    for (unsigned i = 0; i < 8; ++i) {
        const auto key = public_key(i + 3);
        pins.insert(pins.end(), key.begin(), key.end());
    }
    REQUIRE_EQ(srt_setsockflag(sockets.listener,
                   SRTO_ROBOTWEAX_PEERAUTH_TRUSTED, pins.data(), pins.size()),
        0);
    sockets.listen();
    struct Clients {
        std::vector<SRTSOCKET> sockets;
        ~Clients()
        {
            for (const auto id : sockets)
                (void)srt_close(id);
        }
    } clients, children;
    constexpr unsigned count = 8;
    const bool asynchronous = false;
    for (unsigned i = 0; i < count; ++i) {
        const auto id = srt_create_socket();
        REQUIRE(id != SRT_INVALID_SOCK);
        clients.sockets.push_back(id);
        PublicPeerSockets::configure(id, true);
        REQUIRE_EQ(srt_setsockflag(id, SRTO_ROBOTWEAX_PEERAUTH_KEY,
                       scalar(i + 3).data(), 32),
            0);
        REQUIRE_EQ(
            srt_setsockflag(id, SRTO_CONNTIMEO, &timeout, sizeof(timeout)), 0);
        REQUIRE_EQ(srt_setsockflag(
                       id, SRTO_RCVSYN, &asynchronous, sizeof(asynchronous)),
            0);
        const auto stream = std::to_string(i);
        REQUIRE_EQ(
            srt_setsockflag(id, SRTO_STREAMID, stream.data(), stream.size()),
            0);
        REQUIRE_EQ(
            srt_connect(id, reinterpret_cast<const sockaddr*>(&sockets.address),
                sizeof(sockets.address)),
            0);
    }
    // Accept is nonblocking so a failed admission cannot hang the suite.
    REQUIRE_EQ(srt_setsockflag(sockets.listener, SRTO_RCVSYN, &asynchronous,
                   sizeof(asynchronous)),
        0);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds {6};
    while (children.sockets.size() < count
        && std::chrono::steady_clock::now() < deadline) {
        const auto child = srt_accept(sockets.listener, nullptr, nullptr);
        if (child != SRT_INVALID_SOCK)
            children.sockets.push_back(child);
        else {
            REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EASYNCRCV);
            std::this_thread::sleep_for(std::chrono::milliseconds {1});
        }
    }
    REQUIRE_EQ(children.sockets.size(), count);
    // Listener acceptance and caller publication are independent final-KM gates.
    for (const auto caller : clients.sockets) {
        while (srt_getsockstate(caller) == SRTS_CONNECTING
            && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds {1});
    }
    for (unsigned i = 0; i < count; ++i) {
        REQUIRE_EQ(srt_getsockstate(clients.sockets[i]), SRTS_CONNECTED);
        const char byte = static_cast<char>(i);
        REQUIRE_EQ(srt_sendmsg(clients.sockets[i], &byte, 1, -1, 0), 1);
    }
    std::array<bool, count> seen {};
    for (const auto child : children.sockets) {
        std::array<char, 16> stream {};
        int size = stream.size();
        REQUIRE_EQ(
            srt_getsockflag(child, SRTO_STREAMID, stream.data(), &size), 0);
        const auto index = static_cast<unsigned>(stream[0] - '0');
        REQUIRE(index < count && !seen[index]);
        seen[index] = true;
        const bool synchronous = true;
        REQUIRE_EQ(srt_setsockflag(
                       child, SRTO_RCVSYN, &synchronous, sizeof(synchronous)),
            0);
        std::array<char, 1500> received {};
        REQUIRE_EQ(srt_recvmsg(child, received.data(), received.size()), 1);
        REQUIRE_EQ(static_cast<unsigned>(received[0]), index);
        std::array<std::byte, 64> key {};
        size = key.size();
        REQUIRE_EQ(srt_getsockflag(
                       child, SRTO_ROBOTWEAX_PEERAUTH_PEER, key.data(), &size),
            0);
        REQUIRE_EQ(key, public_key(index + 3));
    }
}

TEST(peer_auth_public_queued_setup_obeys_deadline_and_close)
{
    for (const bool cancel : {false, true}) {
        PublicPeerCallback callback;
        PublicPeerSockets sockets;
        auto result = callback.promise.get_future();
        std::future<int>
            closing; // Gate release must precede waiting for this close.
        const bool synchronous = false;
        const int timeout = 100;
        REQUIRE_EQ(srt_setsockflag(sockets.caller, SRTO_RCVSYN, &synchronous,
                       sizeof(synchronous)),
            0);
        REQUIRE_EQ(srt_setsockflag(sockets.caller, SRTO_CONNTIMEO, &timeout,
                       sizeof(timeout)),
            0);
        REQUIRE_EQ(srt_connect_callback(
                       sockets.caller, PublicPeerCallback::observe, &callback),
            0);
        UdpSocket sink;
        REQUIRE_EQ(sink.bind(IpEndpoint::loopback(0)), Error::none);
        const auto endpoint = sink.local_endpoint();
        REQUIRE(endpoint);
        sockets.address.sin_port = htons(endpoint.endpoint.port);
        const auto executor = compat::acquire_runtime_work_executor();
        REQUIRE(executor != nullptr);
        const auto capacity = executor->snapshot();
        const auto gate = std::make_shared<PeerWorkGate>();
        PeerWorkGateRelease release {gate};
        for (std::size_t i = 0; i < capacity.worker_count; ++i)
            REQUIRE_EQ(executor->submit(
                           {.function = PeerWorkGate::block, .context = gate}),
                compat::RuntimeWorkExecutor::SubmitStatus::accepted);
        {
            std::unique_lock lock(gate->mutex);
            REQUIRE(gate->changed.wait_for(lock, std::chrono::seconds {2}, [&] {
                return gate->started == capacity.worker_count;
            }));
        }
        REQUIRE_EQ(sockets.connect(), 0);
        const auto retained =
            compat::SocketRegistry::instance().find(sockets.caller);
        REQUIRE(retained != nullptr);
        if (cancel)
            closing = std::async(std::launch::async, [&] {
                return srt_close(sockets.caller);
            });
        REQUIRE(result.wait_for(std::chrono::milliseconds {150})
            == std::future_status::timeout);
        gate->release();
        REQUIRE(result.wait_for(std::chrono::seconds {2})
            == std::future_status::ready);
        const auto observed = result.get();
        REQUIRE_EQ(observed.error, cancel ? SRT_ESCLOSED : SRT_ENOSERVER);
        REQUIRE(!observed.authenticated);
        if (cancel)
            REQUIRE_EQ(closing.get(), 0);
        // No induction was sent after queue expiry or cancellation.
        REQUIRE(!sink.wait_readable(20).ready);
        {
            std::lock_guard lock(retained->mutex);
            REQUIRE(retained->runtime == nullptr);
            REQUIRE(retained->crypto == nullptr);
            REQUIRE(!retained->has_authenticated_peer);
        }
    }
}
