// SPDX-License-Identifier: MIT
#include "test.hpp"
#include "peer_authentication.hpp"
#include "robotweax/srt/handshake.hpp"

#include <algorithm>
#include <string_view>
#include <vector>

using namespace robotweax::srt;
namespace pa = robotweax::srt::peer_authentication;
namespace {
std::span<const std::byte> bytes(std::string_view s)
{
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}
pa::Digest hex(std::string_view s)
{
    REQUIRE_EQ(s.size(), 64U);
    pa::Digest result {};
    const auto digit = [](char c) {
        return c <= '9' ? c - '0' : c - 'a' + 10;
    };
    for (unsigned i = 0; i < 32; ++i)
        result[i] = std::byte((digit(s[2 * i]) << 4) | digit(s[2 * i + 1]));
    return result;
}
bool zero(std::span<const std::byte> s)
{
    return std::all_of(s.begin(), s.end(), [](auto b) {
        return b == std::byte {};
    });
}
pa::Record record(pa::Message message)
{
    pa::Record r;
    r.message = message;
    r.key_length = 16;
    r.nonce.fill(std::byte {1});
    r.public_key.fill(std::byte {2});
    r.key_identifier.fill(std::byte {3});
    r.signature.fill(std::byte {4});
    r.confirmation.fill(std::byte {5});
    return r;
}
std::vector<std::byte> content(pa::Message message)
{
    std::vector<std::byte> result(pa::content_size(message));
    std::size_t written = 999;
    REQUIRE_EQ(
        pa::encode_record(record(message), result, written), Error::none);
    REQUIRE_EQ(written, result.size());
    return result;
}
std::vector<std::byte> wire(pa::Message message)
{
    auto body = content(message);
    std::vector<std::byte> result(52 + body.size());
    Handshake h;
    h.extension_field = handshake_extension_flag_config;
    h.request = message == pa::Message::confirmation
        ? HandshakeRequest::agreement
        : HandshakeRequest::conclusion;
    REQUIRE_EQ(encode_handshake(h, result), Error::none);
    result[48] = std::byte {0x7f};
    result[49] = std::byte {0x16};
    result[51] = std::byte(body.size() / 4);
    std::copy(body.begin(), body.end(), result.begin() + 52);
    return result;
}
const auto th_l =
    hex("727eace712d5a804b76126a181f58a701a6671b8eceb75baa9441a67e00c1950");
const auto th_c =
    hex("c21cf7993cfbdb5374a1bd956b1f8059316d5eb0204acb8104df7073dc31c242");
const auto hkey2 =
    hex("5c750d58dca2891735e2ed689e4124a51bad63359fd3523b9f8bcdfa11384985");
pa::Digest shared_secret()
{
    pa::Digest result {};
    for (unsigned i = 0; i < 32; ++i)
        result[i] = std::byte(i);
    return result;
}
Error derive(pa::KeySchedule& schedule, unsigned key_length = 16)
{
    pa::Digest c {}, l {};
    c.fill(std::byte {1});
    l.fill(std::byte {2});
    return schedule.derive_authenticated_keys(bytes("fixture passphrase"), c, l,
        th_c, static_cast<std::uint8_t>(key_length));
}
class ObservedProvider final : public CryptoProvider {
public:
    CryptoProvider& real = default_crypto_provider();
    unsigned calls = 0, fail_at = 0, pbkdf_calls = 0;
    bool erased_chain = false, expanded_after_erasure = false;
    Error random_bytes(std::span<std::byte> out) noexcept override
    {
        return real.random_bytes(out);
    }
    Error pbkdf2_hmac_sha1(std::span<const std::byte> p,
        std::span<const std::byte> s, std::uint32_t n,
        std::span<std::byte> out) noexcept override
    {
        ++pbkdf_calls;
        if (++calls == fail_at) {
            std::fill(out.begin(), out.end(), std::byte {0xa5});
            return Error::cryptographic_failure;
        }
        return real.pbkdf2_hmac_sha1(p, s, n, out);
    }
    Error hmac_sha256(std::span<const std::byte> k,
        std::span<const std::byte> in,
        std::span<std::byte, 32> out) noexcept override
    {
        if (in.size() == 50 && in[16] == std::byte {3})
            expanded_after_erasure = erased_chain;
        if (++calls == fail_at) {
            std::fill(out.begin(), out.end(), std::byte {0xa5});
            return Error::cryptographic_failure;
        }
        return real.hmac_sha256(k, in, out);
    }
    // Deliberately inherits the unsupported SHA-256 default.
    Error wrap_key(std::span<const std::byte>, std::span<const std::byte>,
        std::span<std::byte>, std::size_t& n) noexcept override
    {
        n = 0;
        return Error::unsupported;
    }
    Error unwrap_key(std::span<const std::byte>, std::span<const std::byte>,
        std::span<std::byte>, std::size_t& n) noexcept override
    {
        n = 0;
        return Error::unsupported;
    }
    Error make_aes_ctr_cipher(std::span<const std::byte>,
        std::unique_ptr<PayloadCipher>& out) noexcept override
    {
        out.reset();
        return Error::unsupported;
    }
    bool supports_aes_gcm() const noexcept override
    {
        return false;
    }
    Error make_aes_gcm_cipher(std::span<const std::byte>,
        std::unique_ptr<AuthenticatedPayloadCipher>& out) noexcept override
    {
        out.reset();
        return Error::unsupported;
    }
    void secure_erase(std::span<std::byte> out) noexcept override
    {
        if (std::equal(out.begin(), out.end(), hkey2.begin(), hkey2.end()))
            erased_chain = true;
        real.secure_erase(out);
    }
};
} // namespace

TEST(peer_auth_record_wire_layout_and_roundtrip)
{
    for (auto message : {pa::Message::offer, pa::Message::response,
             pa::Message::confirmation}) {
        const auto encoded = content(message);
        const std::array prefix {std::byte {1},
            std::byte(static_cast<unsigned>(message)), std::byte {1},
            std::byte {}, std::byte {2}, std::byte {16}, std::byte {},
            std::byte {}};
        REQUIRE(std::equal(prefix.begin(), prefix.end(), encoded.begin()));
        if (message != pa::Message::confirmation) {
            REQUIRE_EQ(encoded[8], std::byte {1});
            REQUIRE_EQ(encoded[40], std::byte {2});
            REQUIRE_EQ(encoded[104], std::byte {3});
        }
        if (message != pa::Message::offer) {
            REQUIRE_EQ(encoded[encoded.size() - 96], std::byte {4});
            REQUIRE_EQ(encoded.back(), std::byte {5});
        }
        pa::Record decoded;
        REQUIRE_EQ(pa::decode_record(encoded, decoded), Error::none);
        std::vector<std::byte> copy(encoded.size());
        std::size_t n = 0;
        REQUIRE_EQ(pa::encode_record(decoded, copy, n), Error::none);
        REQUIRE_EQ(copy, encoded);
        n = 999;
        REQUIRE_EQ(pa::encode_record(
                       decoded, std::span {copy}.first(copy.size() - 1), n),
            Error::buffer_too_small);
        REQUIRE_EQ(n, 0U);
    }
}
TEST(peer_auth_record_rejects_malformed_prefix_lengths_and_nonce)
{
    for (auto message : {pa::Message::offer, pa::Message::response,
             pa::Message::confirmation}) {
        auto original = content(message);
        pa::Record out;
        for (std::size_t size = 0; size < original.size(); ++size)
            REQUIRE_EQ(pa::decode_record(std::span {original}.first(size), out),
                Error::invalid_extension);
        for (unsigned i : {0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U}) {
            auto bad = original;
            bad[i] = std::byte {0xff};
            REQUIRE_EQ(pa::decode_record(bad, out), Error::invalid_extension);
            REQUIRE(zero(out.nonce));
        }
        auto surplus = original;
        surplus.push_back(std::byte {});
        REQUIRE_EQ(pa::decode_record(surplus, out), Error::invalid_extension);
        original[5] = std::byte {};
        REQUIRE_EQ(pa::decode_record(original, out),
            message == pa::Message::offer ? Error::none
                                          : Error::invalid_extension);
        if (message != pa::Message::confirmation) {
            auto bad = content(message);
            std::fill_n(bad.begin() + 8, 32, std::byte {});
            REQUIRE_EQ(pa::decode_record(bad, out), Error::invalid_extension);
        }
    }
    auto bad = record(static_cast<pa::Message>(255));
    std::array<std::byte, 232> out {};
    std::size_t n = 1;
    REQUIRE_EQ(pa::encode_record(bad, out, n), Error::invalid_extension);
    REQUIRE_EQ(n, 0U);
}
TEST(peer_auth_transcript_independent_vectors_and_proof_zeroing)
{
    auto m1 = wire(pa::Message::offer), m2 = wire(pa::Message::response),
         m3 = wire(pa::Message::confirmation);
    auto& p = default_crypto_provider();
    pa::Digest out {};
    REQUIRE_EQ(pa::listener_transcript(p, m1, m2, out), Error::none);
    REQUIRE_EQ(out, th_l);
    REQUIRE_EQ(pa::caller_transcript(p, m1, m2, m3, out), Error::none);
    REQUIRE_EQ(out, th_c);
    // Every listener proof byte is zeroed in th_L, but bound by th_C.
    for (std::size_t i = m2.size() - 96; i < m2.size(); ++i) {
        m2[i] ^= std::byte {1};
        REQUIRE_EQ(pa::listener_transcript(p, m1, m2, out), Error::none);
        REQUIRE_EQ(out, th_l);
        REQUIRE_EQ(pa::caller_transcript(p, m1, m2, m3, out), Error::none);
        REQUIRE(out != th_c);
        m2[i] ^= std::byte {1};
    }
    for (std::size_t i = m3.size() - 96; i < m3.size(); ++i)
        m3[i] ^= std::byte {1};
    REQUIRE_EQ(pa::caller_transcript(p, m1, m2, m3, out), Error::none);
    REQUIRE_EQ(out, th_c);
    // Unknown records are not dropped or re-serialized.
    m1.insert(m1.end(),
        {std::byte {0x7e}, std::byte {0x01}, std::byte {}, std::byte {1},
            std::byte {9}, std::byte {}, std::byte {}, std::byte {}});
    REQUIRE_EQ(pa::listener_transcript(p, m1, m2, out), Error::none);
    auto unknown = out;
    m1.back() ^= std::byte {1};
    REQUIRE_EQ(pa::listener_transcript(p, m1, m2, out), Error::none);
    REQUIRE(out != unknown);
}
TEST(peer_auth_transcript_rejects_invalid_framing_and_mixed_profiles)
{
    auto& p = default_crypto_provider();
    auto good = wire(pa::Message::offer), m2 = wire(pa::Message::response);
    pa::Digest out {};
    auto rejects = [&](const auto& bad) {
        out.fill(std::byte {0xa5});
        REQUIRE(pa::listener_transcript(p, bad, m2, out) != Error::none);
        REQUIRE(zero(out));
    };
    for (unsigned offset : {7U, 48U, 49U, 50U, 51U}) {
        auto bad = good;
        bad[offset] ^= std::byte {0xff};
        rejects(bad);
    }
    auto bad = good;
    bad.insert(bad.end(), good.begin() + 48, good.end());
    rejects(bad);
    bad = good;
    bad.pop_back();
    rejects(bad);
    bad = good;
    bad[51] = std::byte {0xff};
    rejects(bad);
    bad = good;
    bad.resize(pa::maximum_payload_size + 1);
    rejects(bad);
    for (unsigned type : {3U, 4U, 0x7f10U}) {
        bad = good;
        bad.insert(bad.end(),
            {std::byte(type >> 8U), std::byte(type), std::byte {},
                std::byte {}});
        rejects(bad);
    }
    ObservedProvider custom;
    out.fill(std::byte {1});
    REQUIRE_EQ(
        pa::listener_transcript(custom, good, m2, out), Error::unsupported);
    REQUIRE(zero(out));
}
TEST(peer_auth_sha256_known_answers)
{
    auto& p = default_crypto_provider();
    pa::Digest out {};
    REQUIRE_EQ(p.sha256({}, out), Error::none);
    REQUIRE_EQ(out,
        hex("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b85"
            "5"));
    REQUIRE_EQ(p.sha256(bytes("abc"), out), Error::none);
    REQUIRE_EQ(out,
        hex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015a"
            "d"));
}
TEST(peer_auth_key_schedule_independent_vectors_and_early_erasure)
{
    for (unsigned size : {16U, 24U, 32U}) {
        ObservedProvider p;
        pa::KeySchedule schedule(p);
        auto signature = record(pa::Message::response).signature;
        pa::Digest out {};
        REQUIRE(schedule.session_key().empty());
        REQUIRE_EQ(schedule.start(shared_secret(), th_l), Error::none);
        REQUIRE_EQ(p.pbkdf_calls, 0U);
        REQUIRE_EQ(schedule.listener_confirmation(signature, out), Error::none);
        REQUIRE_EQ(out,
            hex("cd2ebab4f949b9572be47cfee678fa20f0a4351f225c183344bc32eafbe724"
                "37"));
        REQUIRE_EQ(p.pbkdf_calls, 0U);
        REQUIRE_EQ(derive(schedule, size), Error::none);
        REQUIRE(p.erased_chain);
        REQUIRE(p.expanded_after_erasure);
        REQUIRE_EQ(p.pbkdf_calls, 1U);
        REQUIRE_EQ(schedule.caller_confirmation(signature, out), Error::none);
        REQUIRE_EQ(out,
            hex("d9de66da2ca4906b8ac827ce6b9cecdb1ca64064fd2467df1750b095cad299"
                "b6"));
        const auto k4 = hex(
            "39af7f3e1872c17a91da2acdffaaa01544de7b13aecd49729838de51c04f560e");
        const auto k5 = hex(
            "838aaba434e3957224a24bced381f4d715c0f8718f3a48af5e74575f98bc942d");
        const auto k6 = hex(
            "167f823c395e8fd3c9c8921cc81300006fe7348813325a8c3762e6b2867581c5");
        REQUIRE(std::equal(k4.begin(), k4.end(), schedule.session_key().begin(),
            schedule.session_key().end()));
        REQUIRE(std::equal(k5.begin(), k5.begin() + size,
            schedule.wrapping_key(true).begin(),
            schedule.wrapping_key(true).end()));
        REQUIRE(std::equal(k6.begin(), k6.begin() + size,
            schedule.wrapping_key(false).begin(),
            schedule.wrapping_key(false).end()));
        REQUIRE_EQ(schedule.listener_confirmation(signature, out),
            Error::invalid_state);
        REQUIRE(zero(out));
        auto retained = schedule.session_key();
        schedule.reset();
        REQUIRE(zero(retained));
        REQUIRE(schedule.session_key().empty());
        REQUIRE_EQ(
            schedule.caller_confirmation(signature, out), Error::invalid_state);
        REQUIRE(zero(out));
    }
}
TEST(peer_auth_key_schedule_provider_failures_erase_and_fail_closed)
{
    for (unsigned failure = 1; failure <= 9; ++failure) {
        ObservedProvider p;
        p.fail_at = failure;
        pa::KeySchedule schedule(p);
        auto result = schedule.start(shared_secret(), th_l);
        if (result == Error::none)
            result = derive(schedule);
        REQUIRE_EQ(result, Error::cryptographic_failure);
        REQUIRE(schedule.session_key().empty());
        REQUIRE(schedule.wrapping_key(true).empty());
        if (failure >= 4)
            REQUIRE(p.erased_chain);
        p.fail_at = 0;
        REQUIRE_EQ(schedule.start(shared_secret(), th_l), Error::none);
        REQUIRE_EQ(derive(schedule), Error::none);
    }
    ObservedProvider p;
    {
        pa::KeySchedule pending(p);
        REQUIRE_EQ(pending.start(shared_secret(), th_l), Error::none);
        REQUIRE(!p.erased_chain);
    }
    REQUIRE(p.erased_chain);
}
TEST(peer_auth_key_schedule_invalid_transitions_and_key_sizes)
{
    ObservedProvider p;
    pa::KeySchedule schedule(p);
    REQUIRE_EQ(derive(schedule), Error::invalid_state);
    REQUIRE_EQ(p.pbkdf_calls, 0U);
    REQUIRE_EQ(schedule.start(shared_secret(), th_l), Error::none);
    REQUIRE_EQ(schedule.start(shared_secret(), th_l), Error::invalid_state);
    REQUIRE(p.erased_chain);
    for (unsigned size : {0U, 15U, 17U, 31U, 33U}) {
        REQUIRE_EQ(schedule.start(shared_secret(), th_l), Error::none);
        REQUIRE_EQ(derive(schedule, size), Error::invalid_state);
        REQUIRE_EQ(p.pbkdf_calls, 0U);
    }
}

TEST(peer_auth_transcript_binds_every_nonproof_byte)
{
    auto& p = default_crypto_provider();
    const auto m1 = wire(pa::Message::offer), m2 = wire(pa::Message::response),
               m3 = wire(pa::Message::confirmation);
    for (unsigned flight = 0; flight < 3; ++flight) {
        auto first = m1, second = m2, third = m3;
        auto& changed = flight == 0 ? first : flight == 1 ? second : third;
        const auto limit = changed.size() - (flight == 2 ? 96U : 0U);
        for (std::size_t offset = 0; offset < limit; ++offset) {
            changed[offset] ^= std::byte {1};
            pa::Digest hash {};
            const auto error =
                pa::caller_transcript(p, first, second, third, hash);
            if (error == Error::none)
                REQUIRE(hash != th_c);
            else
                REQUIRE(zero(hash));
            changed[offset] ^= std::byte {1};
        }
    }
}
TEST(peer_auth_confirmation_failure_erases_ready_keys)
{
    ObservedProvider p;
    pa::KeySchedule schedule(p);
    REQUIRE_EQ(schedule.start(shared_secret(), th_l), Error::none);
    REQUIRE_EQ(derive(schedule), Error::none);
    auto retained = schedule.session_key();
    p.fail_at = p.calls + 1;
    pa::Digest tag {};
    tag.fill(std::byte {1});
    REQUIRE_EQ(schedule.caller_confirmation(
                   record(pa::Message::confirmation).signature, tag),
        Error::cryptographic_failure);
    REQUIRE(zero(tag));
    REQUIRE(zero(retained));
    REQUIRE(schedule.session_key().empty());
}
