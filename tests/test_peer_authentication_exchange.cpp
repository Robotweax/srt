// SPDX-License-Identifier: MIT
#include "test.hpp"
#include "peer_authentication.hpp"
#include "robotweax/srt/handshake.hpp"

#include <algorithm>
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
    std::vector<std::byte> output(52 + pa::content_size(record.message));
    Handshake h;
    h.extension_field = handshake_extension_flag_config;
    h.encryption_field = record.key_length / 8;
    h.request = record.message == pa::Message::confirmation
        ? HandshakeRequest::agreement
        : HandshakeRequest::conclusion;
    REQUIRE_EQ(encode_handshake(h, output), Error::none);
    output[48] = std::byte {0x7f};
    output[49] = std::byte {0x16};
    output[51] = std::byte(pa::content_size(record.message) / 4);
    std::size_t written = 0;
    REQUIRE_EQ(
        pa::encode_record(record, std::span {output}.subspan(52), written),
        Error::none);
    return output;
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
