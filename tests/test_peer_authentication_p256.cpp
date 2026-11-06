// SPDX-License-Identifier: MIT
#include "test.hpp"
#include "robotweax/srt/crypto_provider.hpp"

#include <algorithm>
#include <array>
#include <string_view>

using namespace robotweax::srt;
namespace {
template <std::size_t N> std::array<std::byte, N> hex(std::string_view s)
{
    REQUIRE_EQ(s.size(), 2 * N);
    std::array<std::byte, N> result {};
    const auto digit = [](char c) {
        return c <= '9' ? c - '0' : c - 'A' + 10;
    };
    for (std::size_t i = 0; i < N; ++i)
        result[i] = std::byte((digit(s[2 * i]) << 4) | digit(s[2 * i + 1]));
    return result;
}
std::span<const std::byte> bytes(std::string_view s)
{
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}
bool zero(std::span<const std::byte> s)
{
    return std::all_of(s.begin(), s.end(), [](auto b) {
        return b == std::byte {};
    });
}
// Public test vectors from RFC 6979 A.2.5 (not credentials).
// https://www.rfc-editor.org/rfc/rfc6979.txt
const auto scalar =
    hex<32>("C9AFA9D845BA75166B5C215767B1D6934E50C3DB36E89B127B8A622B120F6721");
const auto public_key =
    hex<64>("60FED4BA255A9D31C961EB74C6356D68C049B8923B61FA6CE669622E60F29FB6"
            "7903FE1008B8BC99A41AE9E95628BC64F2F1B20C2D7E9F5177A3C294D4462299");
const auto signature =
    hex<64>("EFD48B2AACB6A8FD1140DD9CD45E81D69D2C877B56AAF991C34D0EA84EAF3716"
            "F7CB1C942D657C41D436C7A1B6E29F65F3E900DBB9AFF4064DC4AB2F843ACDA8");
std::unique_ptr<P256Key> import(std::span<const std::byte, 32> value)
{
    std::unique_ptr<P256Key> key;
    const auto error = default_crypto_provider().import_p256_key(value, key);
    SKIP_UNLESS(error != Error::unsupported,
        "P-256 provider not implemented on this backend yet");
    REQUIRE_EQ(error, Error::none);
    REQUIRE(key != nullptr);
    return key;
}
}
TEST(peer_auth_p256_rfc6979_public_key_and_signature)
{
    auto& provider = default_crypto_provider();
    auto key = import(scalar);
    std::array<std::byte, 64> actual {};
    REQUIRE_EQ(key->public_key(actual), Error::none);
    REQUIRE_EQ(actual, public_key);
    REQUIRE_EQ(provider.validate_p256_public_key(public_key), Error::none);
    REQUIRE_EQ(
        provider.verify_p256_sha256(public_key, bytes("sample"), signature),
        Error::none);
    REQUIRE(
        provider.verify_p256_sha256(public_key, bytes("tampered"), signature)
        != Error::none);
    REQUIRE_EQ(key->sign_sha256(bytes("sample"), actual), Error::none);
    REQUIRE_EQ(provider.verify_p256_sha256(public_key, bytes("sample"), actual),
        Error::none);
    for (std::size_t i = 0; i < signature.size(); ++i) {
        auto bad = signature;
        bad[i] ^= std::byte {1};
        REQUIRE(provider.verify_p256_sha256(public_key, bytes("sample"), bad)
            != Error::none);
    }
    actual.fill(std::byte {});
    REQUIRE(provider.verify_p256_sha256(public_key, bytes("sample"), actual)
        != Error::none);
    actual.fill(std::byte {0xff});
    REQUIRE(provider.verify_p256_sha256(public_key, bytes("sample"), actual)
        != Error::none);
}
TEST(peer_auth_p256_scalar_range_and_invalid_points)
{
    auto& provider = default_crypto_provider();
    auto key = import(scalar);
    const auto order = hex<32>(
        "FFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551");
    std::array<std::byte, 32> invalid {};
    for (unsigned i = 0; i < 3; ++i) {
        if (i == 1)
            invalid = order;
        if (i == 2)
            invalid.fill(std::byte {0xff});
        auto target = import(scalar);
        REQUIRE_EQ(provider.import_p256_key(invalid, target),
            Error::invalid_key_material);
        REQUIRE(!target);
    }
    // All-zero/infinity, an off-curve point and coordinates outside the field.
    std::array<std::byte, 64> point {};
    for (unsigned i = 0; i < 3; ++i) {
        if (i == 1) {
            point = public_key;
            point[63] ^= std::byte {1};
        }
        if (i == 2)
            point.fill(std::byte {0xff});
        REQUIRE_EQ(provider.validate_p256_public_key(point),
            Error::invalid_key_material);
        std::array<std::byte, 32> output;
        output.fill(std::byte {0xa5});
        REQUIRE_EQ(key->agree(point, output), Error::invalid_key_material);
        REQUIRE(zero(output));
        REQUIRE(provider.verify_p256_sha256(point, bytes("sample"), signature)
            != Error::none);
    }
}
TEST(peer_auth_p256_ecdh_scalar_one_known_answer_and_symmetry)
{
    auto& provider = default_crypto_provider();
    auto a = import(scalar);
    std::array<std::byte, 32> one {};
    one.back() = std::byte {1};
    auto b = import(one);
    std::array<std::byte, 64> ap {}, bp {};
    REQUIRE_EQ(a->public_key(ap), Error::none);
    REQUIRE_EQ(b->public_key(bp), Error::none);
    std::array<std::byte, 32> ab {}, ba {};
    REQUIRE_EQ(a->agree(bp, ab), Error::none);
    REQUIRE_EQ(b->agree(ap, ba), Error::none);
    REQUIRE_EQ(ab, ba);
    REQUIRE(std::equal(ab.begin(), ab.end(), public_key.begin()));
    // Fresh generation must produce valid independent keys and interoperable shares.
    for (unsigned i = 0; i < 8; ++i) {
        std::unique_ptr<P256Key> fresh;
        REQUIRE_EQ(provider.generate_p256_key(fresh), Error::none);
        REQUIRE(fresh != nullptr);
        REQUIRE_EQ(fresh->public_key(bp), Error::none);
        REQUIRE(bp != ap);
        REQUIRE_EQ(provider.validate_p256_public_key(bp), Error::none);
        REQUIRE_EQ(a->agree(bp, ab), Error::none);
        REQUIRE_EQ(fresh->agree(ap, ba), Error::none);
        REQUIRE_EQ(ab, ba);
        REQUIRE(!zero(ab));
    }
}

TEST(peer_auth_p256_rfc5903_ecdh_known_answer)
{
    // RFC 5903 section 8.1 public test data, independently exercised in both roles.
    // https://www.rfc-editor.org/rfc/rfc5903.txt
    auto initiator = import(hex<32>(
        "C88F01F510D9AC3F70A292DAA2316DE544E9AAB8AFE84049C62A9C57862D1433"));
    auto responder = import(hex<32>(
        "C6EF9C5D78AE012A011164ACB397CE2088685D8F06BF9BE0B283AB46476BEE53"));
    const auto ip = hex<64>(
        "DAD0B65394221CF9B051E1FECA5787D098DFE637FC90B9EF945D0C3772581180"
        "5271A0461CDB8252D61F1C456FA3E59AB1F45B33ACCF5F58389E0577B8990BB3");
    const auto rp = hex<64>(
        "D12DFB5289C8D4F81208B70270398C342296970A0BCCB74C736FC7554494BF63"
        "56FBF3CA366CC23E8157854C13C58D6AAC23F046ADA30F8353E74F33039872AB");
    const auto expected = hex<32>(
        "D6840F6B42F6EDAFD13116E0E12565202FEF8E9ECE7DCE03812464D04B9442DE");
    std::array<std::byte, 64> point {};
    REQUIRE_EQ(initiator->public_key(point), Error::none);
    REQUIRE_EQ(point, ip);
    REQUIRE_EQ(responder->public_key(point), Error::none);
    REQUIRE_EQ(point, rp);
    std::array<std::byte, 32> secret {};
    REQUIRE_EQ(initiator->agree(rp, secret), Error::none);
    REQUIRE_EQ(secret, expected);
    REQUIRE_EQ(responder->agree(ip, secret), Error::none);
    REQUIRE_EQ(secret, expected);
}
