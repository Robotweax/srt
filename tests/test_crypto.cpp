#include "test.hpp"
#include "crypto_test_helpers.hpp"

#include "robotweax/srt/crypto.hpp"
#include "compat/transport_runtime.hpp"
#include "srt/srt.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

using namespace robotweax::srt;

namespace {

template <std::size_t Size>
[[nodiscard]] std::array<std::byte, Size> bytes_from_hex(
    std::string_view text)
{
    REQUIRE_EQ(text.size(), Size * 2U);
    std::array<std::byte, Size> result{};
    const auto digit = [](char value) -> std::uint8_t {
        if (value >= '0' && value <= '9') {
            return static_cast<std::uint8_t>(value - '0');
        }
        if (value >= 'a' && value <= 'f') {
            return static_cast<std::uint8_t>(value - 'a' + 10);
        }
        return static_cast<std::uint8_t>(value - 'A' + 10);
    };
    for (std::size_t index = 0; index < Size; ++index) {
        result[index] = static_cast<std::byte>(
            (digit(text[index * 2U]) << 4U)
            | digit(text[index * 2U + 1U]));
    }
    return result;
}

[[nodiscard]] std::span<const std::byte> as_bytes(
    std::string_view text)
{
    return {
        reinterpret_cast<const std::byte*>(text.data()),
        text.size(),
    };
}

template <std::size_t KeySize>
void require_gcm_fixture(const std::array<std::byte, KeySize>& key,
    const std::array<std::byte, srt_gcm_initialization_vector_size>& iv,
    const std::array<std::byte, srt_gcm_additional_data_size>& aad,
    const std::array<std::byte, 32>& plaintext,
    const std::array<std::byte, 32>& expected_ciphertext,
    const std::array<std::byte, srt_gcm_authentication_tag_size>& expected_tag)
{
    CryptoProvider& provider = default_crypto_provider();
    std::unique_ptr<AuthenticatedPayloadCipher> cipher;
    REQUIRE_EQ(provider.make_aes_gcm_cipher(key, cipher), Error::none);
    REQUIRE(cipher != nullptr);

    std::array<std::byte, 32> ciphertext {};
    std::array<std::byte, srt_gcm_authentication_tag_size> tag {};
    REQUIRE_EQ(cipher->seal(iv, aad, plaintext, ciphertext, tag), Error::none);
    REQUIRE_EQ(ciphertext, expected_ciphertext);
    REQUIRE_EQ(tag, expected_tag);

    std::array<std::byte, 32> decrypted {};
    REQUIRE_EQ(cipher->open(iv, aad, ciphertext, tag, decrypted), Error::none);
    REQUIRE_EQ(decrypted, plaintext);

    auto in_place = plaintext;
    tag.fill(std::byte {0});
    REQUIRE_EQ(cipher->seal(iv, aad, in_place, in_place, tag), Error::none);
    REQUIRE_EQ(in_place, expected_ciphertext);
    REQUIRE_EQ(tag, expected_tag);
    REQUIRE_EQ(cipher->open(iv, aad, in_place, tag, in_place), Error::none);
    REQUIRE_EQ(in_place, plaintext);

    auto invalid_tag = expected_tag;
    invalid_tag[0] ^= std::byte {1};
    decrypted.fill(std::byte {0xa5});
    REQUIRE_EQ(
        cipher->open(iv, aad, expected_ciphertext, invalid_tag, decrypted),
        Error::authentication_failure);
    REQUIRE(
        std::all_of(decrypted.begin(), decrypted.end(), [](std::byte value) {
            return value == std::byte {0};
        }));

    auto rejected_in_place = expected_ciphertext;
    REQUIRE_EQ(cipher->open(
                   iv, aad, rejected_in_place, invalid_tag, rejected_in_place),
        Error::authentication_failure);
    REQUIRE(std::all_of(rejected_in_place.begin(), rejected_in_place.end(),
        [](std::byte value) {
            return value == std::byte {0};
        }));

    decrypted.fill(std::byte {0xa5});
    REQUIRE_EQ(cipher->open(iv, aad, expected_ciphertext,
                   std::span {expected_tag}.first(expected_tag.size() - 1U),
                   decrypted),
        Error::authentication_failure);
    REQUIRE(
        std::all_of(decrypted.begin(), decrypted.end(), [](std::byte value) {
            return value == std::byte {0};
        }));

    auto invalid_aad = aad;
    invalid_aad[0] ^= std::byte {1};
    decrypted.fill(std::byte {0xa5});
    REQUIRE_EQ(cipher->open(iv, invalid_aad, expected_ciphertext, expected_tag,
                   decrypted),
        Error::authentication_failure);
    REQUIRE(
        std::all_of(decrypted.begin(), decrypted.end(), [](std::byte value) {
            return value == std::byte {0};
        }));

    REQUIRE_EQ(
        cipher->open(iv, aad, expected_ciphertext, expected_tag, decrypted),
        Error::none);
    REQUIRE_EQ(decrypted, plaintext);
}

class CtrOnlyCryptoProvider final : public CryptoProvider {
public:
    void watch_erasure(const std::byte* address) noexcept
    {
        watched_address_ = address;
        observed_erasure_ = false;
    }

    [[nodiscard]] bool observed_erasure() const noexcept
    {
        return observed_erasure_;
    }

    void fail_random_bytes() noexcept
    {
        fail_random_ = true;
    }

    [[nodiscard]] std::size_t pbkdf2_calls() const noexcept
    {
        return pbkdf2_calls_;
    }

    [[nodiscard]] Error random_bytes(
        std::span<std::byte> destination) noexcept override
    {
        return fail_random_ ? Error::cryptographic_failure
                            : delegate_.random_bytes(destination);
    }

    [[nodiscard]] Error pbkdf2_hmac_sha1(std::span<const std::byte> passphrase,
        std::span<const std::byte> salt, std::uint32_t iterations,
        std::span<std::byte> derived_key) noexcept override
    {
        ++pbkdf2_calls_;
        return delegate_.pbkdf2_hmac_sha1(
            passphrase, salt, iterations, derived_key);
    }

    [[nodiscard]] Error wrap_key(std::span<const std::byte> key_encrypting_key,
        std::span<const std::byte> plaintext_keys,
        std::span<std::byte> destination,
        std::size_t& bytes_written) noexcept override
    {
        return delegate_.wrap_key(
            key_encrypting_key, plaintext_keys, destination, bytes_written);
    }

    [[nodiscard]] Error unwrap_key(
        std::span<const std::byte> key_encrypting_key,
        std::span<const std::byte> wrapped_keys,
        std::span<std::byte> destination,
        std::size_t& bytes_written) noexcept override
    {
        return delegate_.unwrap_key(
            key_encrypting_key, wrapped_keys, destination, bytes_written);
    }

    [[nodiscard]] Error make_aes_ctr_cipher(std::span<const std::byte> key,
        std::unique_ptr<PayloadCipher>& cipher) noexcept override
    {
        return delegate_.make_aes_ctr_cipher(key, cipher);
    }

    [[nodiscard]] bool supports_aes_gcm() const noexcept override
    {
        return false;
    }

    [[nodiscard]] Error make_aes_gcm_cipher(std::span<const std::byte>,
        std::unique_ptr<AuthenticatedPayloadCipher>& cipher) noexcept override
    {
        cipher.reset();
        return Error::unsupported;
    }

    void secure_erase(std::span<std::byte> bytes) noexcept override
    {
        if (bytes.data() == watched_address_) {
            observed_erasure_ = true;
        }
        delegate_.secure_erase(bytes);
    }

private:
    const std::byte* watched_address_ = nullptr;
    bool observed_erasure_ = false;
    bool fail_random_ = false;
    std::size_t pbkdf2_calls_ = 0;
    CryptoProvider& delegate_ = default_crypto_provider();
};

} // namespace

TEST(crypto_key_material_codec_matches_the_srt_wire_layout)
{
    std::array<std::byte, srt_salt_size> salt{};
    for (std::size_t index = 0; index < salt.size(); ++index) {
        salt[index] = static_cast<std::byte>(index);
    }
    std::array<std::byte, 24> wrapped{};
    for (std::size_t index = 0; index < wrapped.size(); ++index) {
        wrapped[index] = static_cast<std::byte>(0x80U + index);
    }
    std::array<std::byte, maximum_key_material_size> encoded{};
    const auto result = encode_key_material({
            .keys = EncryptionKey::even,
            .salt = salt,
            .key_length = 16,
            .wrapped_keys = wrapped,
        },
        encoded);
    REQUIRE(result);
    REQUIRE_EQ(result.bytes_written, 56U);
    REQUIRE_EQ(encoded[0], std::byte{0x12});
    REQUIRE_EQ(encoded[1], std::byte{0x20});
    REQUIRE_EQ(encoded[2], std::byte{0x29});
    REQUIRE_EQ(encoded[3], std::byte{0x01});
    REQUIRE_EQ(encoded[8], std::byte{0x02});
    REQUIRE_EQ(encoded[10], std::byte{0x02});
    REQUIRE_EQ(encoded[14], std::byte{0x04});
    REQUIRE_EQ(encoded[15], std::byte{0x04});

    const auto decoded = decode_key_material(
        std::span{encoded}.first(result.bytes_written));
    REQUIRE(decoded);
    REQUIRE_EQ(decoded.key_material.keys, EncryptionKey::even);
    REQUIRE_EQ(decoded.key_material.key_length, 16U);
    REQUIRE_EQ(decoded.key_material.salt.size(), salt.size());
    REQUIRE_EQ(decoded.key_material.wrapped_keys.size(),
        wrapped.size());

    encoded[11] = std::byte{1};
    REQUIRE_EQ(decode_key_material(
                   std::span{encoded}.first(result.bytes_written))
                   .error,
        Error::invalid_key_material);
}

TEST(crypto_ctr_iv_uses_the_srt_packet_index_layout)
{
    std::array<std::byte, srt_salt_size> salt{};
    for (std::size_t index = 0; index < salt.size(); ++index) {
        salt[index] = static_cast<std::byte>(index);
    }
    const auto iv = make_srt_ctr_initialization_vector(
        SequenceNumber{0x0102'0304U}, salt);
    const auto expected = bytes_from_hex<16>(
        "000102030405060708090b090f090000");
    REQUIRE_EQ(iv, expected);
}

TEST(crypto_gcm_contract_matches_current_srt_iv_and_aad_layout)
{
    REQUIRE_EQ(static_cast<std::uint8_t>(CryptoMode::automatic), 0U);
    REQUIRE_EQ(static_cast<std::uint8_t>(CryptoMode::aes_ctr), 1U);
    REQUIRE_EQ(static_cast<std::uint8_t>(CryptoMode::aes_gcm), 2U);
    REQUIRE_EQ(static_cast<std::uint8_t>(KeyMaterialCipher::aes_gcm), 4U);
    REQUIRE_EQ(
        static_cast<std::uint8_t>(KeyMaterialAuthentication::aes_gcm), 1U);
    REQUIRE_EQ(aes_ctr_crypto_suite.authentication_tag_size, 0U);
    REQUIRE_EQ(aes_gcm_crypto_suite.initialization_vector_size, 12U);
    REQUIRE_EQ(aes_gcm_crypto_suite.authentication_tag_size, 16U);

    const auto salt =
        bytes_from_hex<srt_salt_size>("000102030405060708090a0b0c0d0e0f");
    const auto iv =
        make_srt_gcm_initialization_vector(SequenceNumber {0x0102'0304U}, salt);
    REQUIRE_EQ(iv, bytes_from_hex<12>("0001020304050607090b090f"));

    const DataHeader header {
        .sequence = SequenceNumber {0x0102'0304U},
        .message_number = 0x0123'4567U,
        .boundary = MessageBoundary::solo,
        .in_order = true,
        .encryption_key = EncryptionKey::even,
        .retransmitted = true,
        .timestamp = PacketTimestamp {0x89ab'cdefU},
        .destination_socket_id = 0x1020'3040U,
    };
    const auto aad = make_srt_gcm_additional_authenticated_data(header);
    REQUIRE_EQ(aad, bytes_from_hex<16>("01020304e923456789abcdef10203040"));
}

TEST(crypto_key_material_codec_accepts_only_exact_ctr_and_gcm_suites)
{
    const auto salt =
        bytes_from_hex<srt_salt_size>("000102030405060708090a0b0c0d0e0f");
    std::array<std::byte, 24> wrapped {};
    std::array<std::byte, maximum_key_material_size> encoded {};
    const auto gcm = encode_key_material(
        {
            .keys = EncryptionKey::even,
            .cipher = KeyMaterialCipher::aes_gcm,
            .authentication = KeyMaterialAuthentication::aes_gcm,
            .salt = salt,
            .key_length = 16,
            .wrapped_keys = wrapped,
        },
        encoded);
    REQUIRE(gcm);
    REQUIRE_EQ(encoded[8], std::byte {0x04});
    REQUIRE_EQ(encoded[9], std::byte {0x01});
    const auto decoded_gcm =
        decode_key_material(std::span {encoded}.first(gcm.bytes_written));
    REQUIRE(decoded_gcm);
    REQUIRE_EQ(decoded_gcm.key_material.cipher, KeyMaterialCipher::aes_gcm);
    REQUIRE_EQ(decoded_gcm.key_material.authentication,
        KeyMaterialAuthentication::aes_gcm);

    REQUIRE_EQ(encode_key_material(
                   {
                       .keys = EncryptionKey::even,
                       .cipher = KeyMaterialCipher::aes_gcm,
                       .authentication = KeyMaterialAuthentication::none,
                       .salt = salt,
                       .key_length = 16,
                       .wrapped_keys = wrapped,
                   },
                   encoded)
                   .error,
        Error::invalid_key_material);
    REQUIRE_EQ(encode_key_material(
                   {
                       .keys = EncryptionKey::even,
                       .cipher = KeyMaterialCipher::aes_ctr,
                       .authentication = KeyMaterialAuthentication::aes_gcm,
                       .salt = salt,
                       .key_length = 16,
                       .wrapped_keys = wrapped,
                   },
                   encoded)
                   .error,
        Error::invalid_key_material);

    const auto ctr = encode_key_material(
        {
            .keys = EncryptionKey::even,
            .salt = salt,
            .key_length = 16,
            .wrapped_keys = wrapped,
        },
        encoded);
    REQUIRE(ctr);
    for (std::uint16_t cipher = 0; cipher <= 0xffU; ++cipher) {
        for (std::uint16_t authentication = 0; authentication <= 0xffU;
            ++authentication) {
            encoded[8] = static_cast<std::byte>(cipher);
            encoded[9] = static_cast<std::byte>(authentication);
            const bool exact_ctr = cipher == 2U && authentication == 0U;
            const bool exact_gcm = cipher == 4U && authentication == 1U;
            REQUIRE_EQ(static_cast<bool>(decode_key_material(
                           std::span {encoded}.first(ctr.bytes_written))),
                exact_ctr || exact_gcm);
        }
    }
}

TEST(crypto_mode_negotiation_matches_the_frozen_caller_and_rendezvous_tables)
{
    constexpr std::array modes {
        CryptoMode::automatic,
        CryptoMode::aes_ctr,
        CryptoMode::aes_gcm,
    };
    constexpr std::array caller_listener_expected {
        CryptoMode::aes_ctr,
        CryptoMode::aes_ctr,
        CryptoMode::automatic,
        CryptoMode::aes_ctr,
        CryptoMode::aes_ctr,
        CryptoMode::automatic,
        CryptoMode::aes_gcm,
        CryptoMode::automatic,
        CryptoMode::aes_gcm,
    };
    constexpr std::array rendezvous_expected {
        CryptoMode::aes_ctr,
        CryptoMode::aes_ctr,
        CryptoMode::automatic,
        CryptoMode::aes_ctr,
        CryptoMode::aes_ctr,
        CryptoMode::automatic,
        CryptoMode::automatic,
        CryptoMode::automatic,
        CryptoMode::aes_gcm,
    };

    for (const auto context : {
             CryptoNegotiationContext::caller_listener,
             CryptoNegotiationContext::rendezvous,
         }) {
        const auto& expected =
            context == CryptoNegotiationContext::caller_listener
            ? caller_listener_expected
            : rendezvous_expected;
        for (std::size_t initiator = 0; initiator < modes.size(); ++initiator) {
            for (std::size_t responder = 0; responder < modes.size();
                ++responder) {
                const auto selected = negotiate_crypto_mode(
                    context, modes[initiator], modes[responder]);
                const auto expected_mode =
                    expected[initiator * modes.size() + responder];
                REQUIRE_EQ(static_cast<bool>(selected),
                    expected_mode != CryptoMode::automatic);
                REQUIRE_EQ(selected.effective_mode, expected_mode);
            }
        }
    }

    REQUIRE(!negotiate_crypto_mode(static_cast<CryptoNegotiationContext>(0xffU),
        CryptoMode::aes_ctr, CryptoMode::aes_ctr));
    REQUIRE(!negotiate_crypto_mode(CryptoNegotiationContext::caller_listener,
        static_cast<CryptoMode>(0xffU), CryptoMode::aes_ctr));
}

TEST(openssl_provider_matches_pbkdf2_rfc6070_and_rfc3394)
{
    CryptoProvider& provider = default_crypto_provider();
    std::array<std::byte, 20> derived{};
    REQUIRE_EQ(provider.pbkdf2_hmac_sha1(
                   as_bytes("password"), as_bytes("salt"), 1, derived),
        Error::none);
    REQUIRE_EQ(derived, bytes_from_hex<20>(
        "0c60c80f961f0e71f3a9b524af6012062fe037a6"));

    const auto kek = bytes_from_hex<16>(
        "000102030405060708090a0b0c0d0e0f");
    const auto plaintext = bytes_from_hex<16>(
        "00112233445566778899aabbccddeeff");
    const auto expected = bytes_from_hex<24>(
        "1fa68b0a8112b447aef34bd8fb5a7b82"
        "9d3e862371d2cfe5");
    std::array<std::byte, 24> wrapped{};
    std::size_t wrapped_size = 0;
    REQUIRE_EQ(provider.wrap_key(
                   kek, plaintext, wrapped, wrapped_size),
        Error::none);
    REQUIRE_EQ(wrapped_size, wrapped.size());
    REQUIRE_EQ(wrapped, expected);

    std::array<std::byte, 16> unwrapped{};
    std::size_t unwrapped_size = 0;
    REQUIRE_EQ(provider.unwrap_key(
                   kek, wrapped, unwrapped, unwrapped_size),
        Error::none);
    REQUIRE_EQ(unwrapped_size, plaintext.size());
    REQUIRE_EQ(unwrapped, plaintext);
}

TEST(openssl_provider_matches_the_nist_aes_ctr_vector)
{
    CryptoProvider& provider = default_crypto_provider();
    const auto key = bytes_from_hex<16>(
        "2b7e151628aed2a6abf7158809cf4f3c");
    const auto iv = bytes_from_hex<16>(
        "f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff");
    const auto plaintext = bytes_from_hex<16>(
        "6bc1bee22e409f96e93d7e117393172a");
    const auto expected = bytes_from_hex<16>(
        "874d6191b620e3261bef6864990db6ce");
    std::unique_ptr<PayloadCipher> cipher;
    REQUIRE_EQ(provider.make_aes_ctr_cipher(key, cipher),
        Error::none);
    REQUIRE(cipher != nullptr);
    std::array<std::byte, 16> encrypted{};
    REQUIRE_EQ(cipher->transform(iv, plaintext, encrypted),
        Error::none);
    REQUIRE_EQ(encrypted, expected);
    std::array<std::byte, 16> decrypted{};
    REQUIRE_EQ(cipher->transform(iv, encrypted, decrypted),
        Error::none);
    REQUIRE_EQ(decrypted, plaintext);
}

TEST(openssl_provider_matches_the_frozen_srt_aes_gcm_fixtures)
{
    CryptoProvider& provider = default_crypto_provider();
    REQUIRE(provider.supports_aes_gcm());

    const auto plaintext =
        bytes_from_hex<32>("00112233445566778899aabbccddeeff"
                           "102132435465768798a9bacbdcedfe0f");
    const auto aad = bytes_from_hex<srt_gcm_additional_data_size>(
        "01020304e923456789abcdef10203040");
    require_gcm_fixture(bytes_from_hex<16>("000102030405060708090a0b0c0d0e0f"),
        bytes_from_hex<srt_gcm_initialization_vector_size>(
            "0001020304050607090b090f"),
        aad, plaintext,
        bytes_from_hex<32>("990fcbbac96d671e1a01107984225f50"
                           "4a6ab0b0cadb1158d94af3e1c8c1ed10"),
        bytes_from_hex<srt_gcm_authentication_tag_size>(
            "303da5bcffef8a9ce2c610e2e004ecc5"));

    require_gcm_fixture(
        bytes_from_hex<24>("000102030405060708090a0b0c0d0e0f1011121314151617"),
        bytes_from_hex<srt_gcm_initialization_vector_size>(
            "000102030405060718293a4b"),
        bytes_from_hex<srt_gcm_additional_data_size>(
            "10203040e923456789abcdef10203040"),
        plaintext,
        bytes_from_hex<32>("35f7f6d94a3276c4c4a035530eeaed66"
                           "5330c7abddf5ad0e260a698a1fc133bd"),
        bytes_from_hex<srt_gcm_authentication_tag_size>(
            "84da2e44dcf705af36293e22f27af6f7"));

    require_gcm_fixture(bytes_from_hex<32>("000102030405060708090a0b0c0d0e0f"
                                           "101112131415161718191a1b1c1d1e1f"),
        bytes_from_hex<srt_gcm_initialization_vector_size>(
            "000102030405060777f6f5f5"),
        bytes_from_hex<srt_gcm_additional_data_size>(
            "7ffffffee923456789abcdef10203040"),
        plaintext,
        bytes_from_hex<32>("f719ca09aa1164d5032cd246fcce30c6"
                           "b2a1a96cfa511ee26047cbea924fd7e4"),
        bytes_from_hex<srt_gcm_authentication_tag_size>(
            "d1e882ca6c0870dd38ae75b8c1323a3f"));
}

TEST(openssl_provider_rejects_invalid_aes_gcm_buffers_and_keys)
{
    CryptoProvider& provider = default_crypto_provider();
    std::unique_ptr<AuthenticatedPayloadCipher> cipher;
    const auto key = bytes_from_hex<16>("000102030405060708090a0b0c0d0e0f");
    REQUIRE_EQ(provider.make_aes_gcm_cipher(key, cipher), Error::none);
    REQUIRE(cipher != nullptr);

    std::array<std::byte, 15> invalid_key {};
    REQUIRE_EQ(provider.make_aes_gcm_cipher(invalid_key, cipher),
        Error::invalid_key_material);
    REQUIRE(cipher == nullptr);

    REQUIRE_EQ(provider.make_aes_gcm_cipher(key, cipher), Error::none);
    const auto iv = bytes_from_hex<srt_gcm_initialization_vector_size>(
        "0001020304050607090b090f");
    const auto aad = bytes_from_hex<srt_gcm_additional_data_size>(
        "01020304e923456789abcdef10203040");
    const auto plaintext =
        bytes_from_hex<16>("00112233445566778899aabbccddeeff");
    std::array<std::byte, 15> too_small {};
    std::array<std::byte, srt_gcm_authentication_tag_size> tag {};
    REQUIRE_EQ(cipher->seal(iv, aad, plaintext, too_small, tag),
        Error::buffer_too_small);

    std::array<std::byte, 16> ciphertext {};
    REQUIRE_EQ(cipher->seal(iv, aad, plaintext, ciphertext,
                   std::span {tag}.first(tag.size() - 1U)),
        Error::buffer_too_small);
    REQUIRE_EQ(cipher->open(iv, aad, ciphertext, tag, too_small),
        Error::buffer_too_small);
}

TEST(openssl_provider_matches_the_nist_empty_aes_gcm_vector)
{
    CryptoProvider& provider = default_crypto_provider();
    const std::array<std::byte, 16> key {};
    const std::array<std::byte, srt_gcm_initialization_vector_size> iv {};
    const std::array<std::byte, 0> empty {};
    std::unique_ptr<AuthenticatedPayloadCipher> cipher;
    REQUIRE_EQ(provider.make_aes_gcm_cipher(key, cipher), Error::none);

    std::array<std::byte, srt_gcm_authentication_tag_size> tag {};
    REQUIRE_EQ(cipher->seal(iv, empty, empty, std::span<std::byte> {}, tag),
        Error::none);
    REQUIRE_EQ(tag,
        bytes_from_hex<srt_gcm_authentication_tag_size>(
            "58e2fccefa7e3061367f1d57a4e7455a"));
    REQUIRE_EQ(cipher->open(iv, empty, empty, tag, std::span<std::byte> {}),
        Error::none);
}

TEST(crypto_session_confirms_distinct_initial_bidirectional_data_keys)
{
    for (const auto context : {CryptoNegotiationContext::caller_listener,
             CryptoNegotiationContext::rendezvous}) {
        for (const auto mode : {CryptoMode::aes_ctr, CryptoMode::aes_gcm}) {
            for (const std::size_t key_length : {16U, 24U, 32U}) {
                const CryptoConfiguration configuration {
                    .passphrase = "directional regression fixture",
                    .mode = mode,
                    .negotiation_context = context,
                    .enable_aes_gcm = true,
                    .key_length = key_length,
                };
                CryptoSession caller {configuration};
                REQUIRE(caller.allows_plaintext_fallback());
                auto listener_configuration = configuration;
                listener_configuration.key_length = 16;
                if (context == CryptoNegotiationContext::caller_listener) {
                    listener_configuration.mode = CryptoMode::automatic;
                }
                CryptoSession listener {listener_configuration};
                REQUIRE_EQ(caller.start_initiator(), Error::none);
                const std::vector<std::byte> handshake_key(
                    caller.pending_key_material().begin(),
                    caller.pending_key_material().end());
                REQUIRE_EQ(listener.accept_key_material(handshake_key, true),
                    Error::none);
                REQUIRE_EQ(caller.acknowledge_key_material(
                               listener.key_material_response(), true),
                    Error::none);
                REQUIRE(!caller.ready_to_send_data());
                REQUIRE(!caller.allows_plaintext_fallback());
                REQUIRE(!listener.allows_plaintext_fallback());

                const std::vector<std::byte> caller_key(
                    caller.pending_key_material().begin(),
                    caller.pending_key_material().end());
                const std::vector<std::byte> listener_key(
                    listener.pending_key_material().begin(),
                    listener.pending_key_material().end());

                // Lost responses and duplicate requests retain the same pending
                // directional key rather than generating new material forever.
                REQUIRE_EQ(listener.accept_key_material(handshake_key, true),
                    Error::none);
                REQUIRE(std::equal(listener_key.begin(), listener_key.end(),
                    listener.pending_key_material().begin(),
                    listener.pending_key_material().end()));
                std::array<std::byte, 4> no_secret {};
                no_secret.back() = std::byte {
                    static_cast<unsigned char>(CryptoState::no_secret)};
                for (auto* session : {&caller, &listener}) {
                    REQUIRE_EQ(
                        session->acknowledge_key_material(no_secret, false),
                        Error::cryptographic_failure);
                    REQUIRE_EQ(session->sender_state(), CryptoState::securing);
                    REQUIRE(!session->ready_to_send_data());
                }
                REQUIRE(!listener.ready_to_send_data());

                REQUIRE(!caller_key.empty());
                REQUIRE(!listener_key.empty());
                REQUIRE(caller_key != handshake_key);
                REQUIRE(listener_key != handshake_key);
                REQUIRE(caller_key != listener_key);
                REQUIRE_EQ(listener.key_length(), key_length);
                REQUIRE_EQ(listener.effective_mode(), mode);

                // A duplicate handshake acknowledgement cannot authorize DATA
                // under the separately generated directional key.
                REQUIRE_EQ(caller.acknowledge_key_material(handshake_key, true),
                    Error::none);
                REQUIRE(!caller.ready_to_send_data());

                const auto clear =
                    bytes_from_hex<16>("00112233445566778899aabbccddeeff");
                std::array<std::byte, 16> forward {}, reverse {}, opened {};
                std::array<std::byte, srt_gcm_authentication_tag_size> tag {};
                EncryptionKey selector = EncryptionKey::none;
                DataHeader header {
                    .sequence = SequenceNumber {123456},
                    .encryption_key = EncryptionKey::even,
                    .destination_socket_id = 111,
                };
                if (mode == CryptoMode::aes_gcm) {
                    REQUIRE_EQ(
                        caller.seal(header, clear, forward, tag, selector),
                        Error::cryptographic_failure);
                } else {
                    REQUIRE_EQ(caller.encrypt(
                                   header.sequence, clear, forward, selector),
                        Error::cryptographic_failure);
                }

                REQUIRE_EQ(listener.accept_key_material(caller_key, false),
                    Error::none);
                REQUIRE_EQ(caller.acknowledge_key_material(
                               listener.key_material_response(), false),
                    Error::none);
                REQUIRE_EQ(caller.accept_key_material(listener_key, false),
                    Error::none);
                REQUIRE_EQ(listener.acknowledge_key_material(
                               caller.key_material_response(), false),
                    Error::none);
                REQUIRE(caller.ready_to_send_data());
                REQUIRE(listener.ready_to_send_data());

                // Replayed initial material must not replace the separately
                // confirmed peer receive key in either direction.
                REQUIRE_EQ(caller.accept_key_material(handshake_key, false),
                    Error::none);
                REQUIRE_EQ(listener.accept_key_material(handshake_key, false),
                    Error::none);

                if (mode == CryptoMode::aes_gcm) {
                    REQUIRE_EQ(
                        caller.seal(header, clear, forward, tag, selector),
                        Error::none);
                    REQUIRE_EQ(listener.open(header, forward, tag, opened),
                        Error::none);
                    REQUIRE_EQ(opened, clear);
                    header.destination_socket_id = 222;
                    REQUIRE_EQ(
                        listener.seal(header, clear, reverse, tag, selector),
                        Error::none);
                    REQUIRE_EQ(
                        caller.open(header, reverse, tag, opened), Error::none);
                } else {
                    REQUIRE_EQ(caller.encrypt(
                                   header.sequence, clear, forward, selector),
                        Error::none);
                    REQUIRE_EQ(listener.decrypt(
                                   selector, header.sequence, forward, opened),
                        Error::none);
                    REQUIRE_EQ(opened, clear);
                    REQUIRE_EQ(listener.encrypt(
                                   header.sequence, clear, reverse, selector),
                        Error::none);
                    REQUIRE_EQ(caller.decrypt(
                                   selector, header.sequence, reverse, opened),
                        Error::none);
                }
                REQUIRE_EQ(opened, clear);
                // Equal plaintext and sequence must not expose a shared
                // keystream in opposite directions of one connection.
                REQUIRE(forward != reverse);
            }
        }
    }
}

TEST(crypto_session_accepts_refresh_rates_below_the_nonce_period)
{
    for (const auto mode :
        {CryptoMode::automatic, CryptoMode::aes_ctr, CryptoMode::aes_gcm}) {
        for (const auto refresh :
            {3U, default_key_refresh_rate, SequenceNumber::mask}) {
            CryptoSession session {{
                .passphrase = "native refresh boundary fixture",
                .mode = mode,
                .enable_aes_gcm = true,
                .refresh_rate_packets = refresh,
                .preannouncement_packets = 1,
            }};
            REQUIRE_EQ(session.start_initiator(), Error::none);
            REQUIRE(!session.pending_key_material().empty());
        }
    }
}

TEST(crypto_session_adaptive_announcement_preserves_minimum_and_refresh_budget)
{
    for (const auto mode : {CryptoMode::aes_ctr, CryptoMode::aes_gcm}) {
        for (const auto floor :
            {0ULL, 1ULL, std::numeric_limits<unsigned long long>::max()}) {
            const CryptoConfiguration configuration {
                .passphrase = "adaptive key horizon fixture",
                .mode = mode,
                .enable_aes_gcm = true,
                .refresh_rate_packets = 17,
                .preannouncement_packets = 2,
            };
            CryptoSession sender {configuration};
            CryptoSession receiver {configuration};
            REQUIRE_EQ(sender.start_initiator(), Error::none);
            REQUIRE_EQ(receiver.accept_key_material(
                           sender.pending_key_material(), false),
                Error::none);
            REQUIRE_EQ(sender.acknowledge_key_material(
                           receiver.key_material_response(), false),
                Error::none);
            sender.set_preannouncement_floor(floor);
            const unsigned announcement_at = floor > 8U ? 9U : 15U;
            for (unsigned position = 0; position < 16; ++position) {
                REQUIRE_EQ(sender.prepare_data_packet(position), Error::none);
                REQUIRE_EQ(!sender.pending_key_material().empty(),
                    position >= announcement_at);
                REQUIRE(sender.ready_to_send_data());
                REQUIRE_EQ(sender.active_sender_key(), EncryptionKey::even);
                REQUIRE_EQ(sender.note_data_packet_sent(), Error::none);
            }
            REQUIRE_EQ(sender.packets_on_active_key(), 16U);
            REQUIRE(!sender.ready_to_send_data());
            const auto request = sender.pending_key_material();
            const std::vector saved(request.begin(), request.end());
            sender.set_preannouncement_floor(0U);
            REQUIRE_EQ(sender.prepare_rotation(), Error::none);
            REQUIRE(std::equal(saved.begin(), saved.end(),
                sender.pending_key_material().begin(),
                sender.pending_key_material().end()));
            REQUIRE_EQ(
                receiver.accept_key_material(request, false), Error::none);
            REQUIRE_EQ(sender.acknowledge_key_material(
                           receiver.key_material_response(), false),
                Error::none);
            REQUIRE(sender.ready_to_send_data());
            REQUIRE_EQ(sender.prepare_data_packet(16U), Error::none);
            REQUIRE_EQ(sender.note_data_packet_sent(), Error::none);
            REQUIRE_EQ(sender.active_sender_key(), EncryptionKey::odd);
            REQUIRE_EQ(sender.packets_on_active_key(), 0U);
        }
    }
}

TEST(crypto_session_rejects_refresh_rates_at_or_above_the_nonce_period)
{
    for (const auto mode :
        {CryptoMode::automatic, CryptoMode::aes_ctr, CryptoMode::aes_gcm}) {
        CryptoConfiguration configuration {
            .passphrase = "native refresh boundary fixture",
            .mode = mode,
            .enable_aes_gcm = true,
        };
        CryptoSession valid_sender {configuration};
        REQUIRE_EQ(valid_sender.start_initiator(), Error::none);
        for (const auto refresh : {SequenceNumber::modulus, 0xffff'ffffU}) {
            configuration.refresh_rate_packets = refresh;
            CryptoSession initiator {configuration};
            REQUIRE_EQ(initiator.start_initiator(), Error::invalid_state);
            REQUIRE(initiator.pending_key_material().empty());
            REQUIRE(!initiator.ready_to_send_data());
            CryptoSession responder {configuration};
            REQUIRE_EQ(responder.accept_key_material(
                           valid_sender.pending_key_material(), false),
                Error::invalid_state);
            REQUIRE(responder.key_material_response().empty());
            REQUIRE(!responder.ready_to_send_data());
        }
    }
}

TEST(crypto_session_plaintext_fallback_stops_after_key_confirmation)
{
    const CryptoConfiguration configuration {
        .passphrase = "optional encryption policy fixture",
        .refresh_rate_packets = 4,
        .preannouncement_packets = 1,
    };
    CryptoSession sender {configuration};
    CryptoSession receiver {configuration};
    REQUIRE(sender.allows_plaintext_fallback());
    REQUIRE_EQ(sender.start_initiator(), Error::none);
    REQUIRE(sender.allows_plaintext_fallback());
    REQUIRE_EQ(
        receiver.accept_key_material(sender.pending_key_material(), false),
        Error::none);
    REQUIRE(!receiver.allows_plaintext_fallback());
    REQUIRE_EQ(sender.acknowledge_key_material(
                   receiver.key_material_response(), false),
        Error::none);
    REQUIRE(!sender.allows_plaintext_fallback());
    REQUIRE_EQ(sender.receiver_state(), CryptoState::unsecured);
    for (unsigned i = 0; i < 3; ++i) {
        REQUIRE_EQ(sender.note_data_packet_sent(), Error::none);
    }
    REQUIRE_EQ(sender.prepare_rotation(), Error::none);
    REQUIRE_EQ(sender.sender_state(), CryptoState::securing);
    REQUIRE(!sender.allows_plaintext_fallback());
}

namespace {

void secure_sender(CryptoSession& sender, CryptoSession& receiver)
{
    REQUIRE_EQ(sender.start_initiator(), Error::none);
    REQUIRE_EQ(
        receiver.accept_key_material(sender.pending_key_material(), false),
        Error::none);
    REQUIRE_EQ(sender.acknowledge_key_material(
                   receiver.key_material_response(), false),
        Error::none);
    REQUIRE_EQ(sender.sender_state(), CryptoState::secured);
}

} // namespace

TEST(crypto_key_rotation_accounts_for_gaps_before_sending)
{
    const CryptoConfiguration configuration {
        .passphrase = "sequence gap fixture",
        .refresh_rate_packets = 8,
        .preannouncement_packets = 2};
    CryptoSession sender {configuration}, receiver {configuration};
    secure_sender(sender, receiver);
    REQUIRE_EQ(sender.prepare_data_packet(0), Error::none);
    REQUIRE_EQ(sender.note_data_packet_sent(), Error::none);
    REQUIRE_EQ(sender.prepare_data_packet(6), Error::none);
    REQUIRE_EQ(sender.packets_on_active_key(), 6U);
    REQUIRE_EQ(sender.sender_state(), CryptoState::securing);
    REQUIRE_EQ(sender.prepare_data_packet(6), Error::none);
    REQUIRE_EQ(sender.packets_on_active_key(), 6U);
    // The selected candidate expires before transmission: its position still
    // counts when the following packet becomes the new candidate.
    REQUIRE_EQ(sender.prepare_data_packet(10), Error::none);
    REQUIRE_EQ(sender.packets_on_active_key(), 10U);
    REQUIRE(!sender.ready_to_send_data());
    REQUIRE_EQ(
        receiver.accept_key_material(sender.pending_key_material(), false),
        Error::none);
    REQUIRE_EQ(sender.acknowledge_key_material(
                   receiver.key_material_response(), false),
        Error::none);
    REQUIRE(sender.ready_to_send_data());
    const auto old_key = sender.active_sender_key();
    REQUIRE_EQ(sender.note_data_packet_sent(), Error::none);
    REQUIRE(sender.active_sender_key() != old_key);
    REQUIRE_EQ(sender.packets_on_active_key(), 0U);
}

TEST(crypto_key_rotation_counts_large_gaps_without_signed_wrap)
{
    for (const auto gap : {std::uint64_t {SequenceNumber::half_range},
             std::uint64_t {SequenceNumber::half_range} + 123U}) {
        const CryptoConfiguration configuration {
            .passphrase = "large gap fixture",
            .refresh_rate_packets = maximum_key_refresh_rate};
        CryptoSession sender {configuration}, receiver {configuration};
        secure_sender(sender, receiver);
        REQUIRE_EQ(sender.prepare_data_packet(0), Error::none);
        REQUIRE_EQ(sender.note_data_packet_sent(), Error::none);
        REQUIRE_EQ(sender.prepare_data_packet(gap), Error::none);
        REQUIRE_EQ(sender.packets_on_active_key(), gap);
        REQUIRE(!sender.ready_to_send_data());
        REQUIRE_EQ(sender.sender_state(), CryptoState::securing);
    }
}

TEST(crypto_key_budget_rejects_complete_cycles_before_encryption)
{
    const CryptoConfiguration configuration {
        .passphrase = "wrap prevention fixture",
        .refresh_rate_packets = maximum_key_refresh_rate};
    for (const auto position : {std::uint64_t {SequenceNumber::mask},
             std::uint64_t {SequenceNumber::modulus},
             2U * std::uint64_t {SequenceNumber::modulus},
             std::numeric_limits<std::uint64_t>::max()}) {
        CryptoSession sender {configuration}, receiver {configuration};
        secure_sender(sender, receiver);
        REQUIRE_EQ(sender.prepare_data_packet(0), Error::none);
        REQUIRE_EQ(sender.note_data_packet_sent(), Error::none);
        REQUIRE_EQ(
            sender.prepare_data_packet(position), Error::cryptographic_failure);
        REQUIRE_EQ(sender.packets_on_active_key(), 1U);
    }
}

TEST(crypto_key_rotation_cap_announces_before_boundary)
{
    const CryptoConfiguration configuration {
        .passphrase = "refresh cap fixture",
        .refresh_rate_packets = maximum_key_refresh_rate};
    CryptoSession sender {configuration}, receiver {configuration};
    secure_sender(sender, receiver);
    REQUIRE_EQ(sender.prepare_data_packet(0), Error::none);
    REQUIRE_EQ(sender.note_data_packet_sent(), Error::none);
    const auto threshold =
        effective_key_refresh_cap - default_key_preannouncement;
    REQUIRE_EQ(sender.prepare_data_packet(threshold - 1U), Error::none);
    REQUIRE_EQ(sender.sender_state(), CryptoState::secured);
    REQUIRE_EQ(sender.prepare_data_packet(threshold), Error::none);
    REQUIRE_EQ(sender.sender_state(), CryptoState::securing);
}

TEST(crypto_session_directional_key_generation_failure_blocks_data)
{
    const CryptoConfiguration configuration {
        .passphrase = "directional random failure fixture",
    };
    for (const bool fail_at_caller : {false, true}) {
        CtrOnlyCryptoProvider provider;
        CryptoSession caller {configuration, provider};
        CryptoSession listener {configuration, provider};
        REQUIRE_EQ(caller.start_initiator(), Error::none);
        if (fail_at_caller) {
            REQUIRE_EQ(listener.accept_key_material(
                           caller.pending_key_material(), true),
                Error::none);
            provider.fail_random_bytes();
            REQUIRE_EQ(caller.acknowledge_key_material(
                           listener.key_material_response(), true),
                Error::cryptographic_failure);
        } else {
            provider.fail_random_bytes();
            REQUIRE_EQ(listener.accept_key_material(
                           caller.pending_key_material(), true),
                Error::cryptographic_failure);
        }
        auto& failed = fail_at_caller ? caller : listener;
        REQUIRE(!failed.ready_to_send_data());
        REQUIRE(!failed.allows_plaintext_fallback());
        const std::array<std::byte, 16> clear {};
        std::array<std::byte, 16> ciphertext {};
        ciphertext.fill(std::byte {0x5a});
        EncryptionKey key = EncryptionKey::none;
        REQUIRE(failed.encrypt(SequenceNumber {1}, clear, ciphertext, key)
            != Error::none);
        REQUIRE_EQ(key, EncryptionKey::none);
        REQUIRE(std::all_of(
            ciphertext.begin(), ciphertext.end(), [](std::byte value) {
                return value == std::byte {0x5a};
            }));
    }
}

TEST(crypto_session_rejects_an_oversized_passphrase_without_truncating_it)
{
    std::array<char, maximum_passphrase_size + 1U> passphrase {};
    passphrase.fill('p');
    CryptoSession session {{
        .passphrase = std::string_view {passphrase.data(), passphrase.size()},
    }};
    REQUIRE_EQ(session.start_initiator(), Error::invalid_state);
    REQUIRE(!session.enabled());
}

TEST(crypto_session_seals_and_opens_every_gcm_key_size_for_caller_listener_data)
{
    for (const std::size_t key_length : {16U, 24U, 32U}) {
        const CryptoConfiguration configuration {
            .passphrase = "authenticated session keys",
            .mode = CryptoMode::aes_gcm,
            .enable_aes_gcm = true,
            .key_length = key_length,
        };
        CryptoSession sender {configuration};
        CryptoSession receiver {configuration};
        REQUIRE_EQ(sender.configured_mode(), CryptoMode::aes_gcm);
        REQUIRE_EQ(sender.effective_mode(), CryptoMode::automatic);
        REQUIRE_EQ(sender.start_initiator(), Error::none);
        REQUIRE_EQ(sender.effective_mode(), CryptoMode::aes_gcm);

        const auto request = sender.pending_key_material();
        const auto decoded = decode_key_material(request);
        REQUIRE(decoded);
        REQUIRE_EQ(decoded.key_material.cipher, KeyMaterialCipher::aes_gcm);
        REQUIRE_EQ(decoded.key_material.authentication,
            KeyMaterialAuthentication::aes_gcm);
        REQUIRE_EQ(decoded.key_material.key_length, key_length);
        REQUIRE_EQ(receiver.accept_key_material(request, true), Error::none);
        REQUIRE_EQ(receiver.effective_mode(), CryptoMode::aes_gcm);
        REQUIRE_EQ(sender.acknowledge_key_material(
                       receiver.key_material_response(), true),
            Error::none);
        confirm_directional_test_keys(sender, receiver);

        REQUIRE(sender.ready_to_send_data());
        const auto clear =
            bytes_from_hex<16>("00112233445566778899aabbccddeeff");
        std::array<std::byte, 16> ciphertext {};
        EncryptionKey key = EncryptionKey::none;
        // The legacy tagless transform remains CTR-only. Authenticated DATA
        // must use the header-bound seal/open interface below.
        REQUIRE_EQ(sender.encrypt(SequenceNumber {7}, clear, ciphertext, key),
            Error::unsupported);
        REQUIRE_EQ(key, EncryptionKey::none);

        std::array<std::byte, 16> plaintext;
        plaintext.fill(std::byte {0xa5});
        REQUIRE_EQ(receiver.decrypt(EncryptionKey::even, SequenceNumber {7},
                       ciphertext, plaintext),
            Error::unsupported);
        REQUIRE(std::all_of(
            plaintext.begin(), plaintext.end(), [](std::byte value) {
                return value == std::byte {0};
            }));

        const DataHeader header {
            .sequence = SequenceNumber {7},
            .message_number = 19,
            .boundary = MessageBoundary::solo,
            .in_order = true,
            .encryption_key = sender.active_sender_key(),
            .timestamp = PacketTimestamp {0x1122'3344U},
            .destination_socket_id = 0x5566'7788U,
        };
        std::array<std::byte, 16> protected_ciphertext {};
        std::array<std::byte, srt_gcm_authentication_tag_size> tag {};
        REQUIRE_EQ(sender.seal(header, clear, protected_ciphertext, tag, key),
            Error::none);
        REQUIRE_EQ(key, EncryptionKey::even);
        REQUIRE(!std::equal(
            clear.begin(), clear.end(), protected_ciphertext.begin()));

        DataHeader wrong_selector = header;
        wrong_selector.encryption_key = EncryptionKey::odd;
        protected_ciphertext.fill(std::byte {0xa5});
        tag.fill(std::byte {0xa5});
        key = EncryptionKey::odd;
        REQUIRE_EQ(
            sender.seal(wrong_selector, clear, protected_ciphertext, tag, key),
            Error::invalid_state);
        REQUIRE_EQ(key, EncryptionKey::none);
        REQUIRE(std::all_of(protected_ciphertext.begin(),
            protected_ciphertext.end(), [](std::byte value) {
                return value == std::byte {0};
            }));
        REQUIRE(std::all_of(tag.begin(), tag.end(), [](std::byte value) {
            return value == std::byte {0};
        }));

        REQUIRE_EQ(sender.seal(header, clear, protected_ciphertext, tag, key),
            Error::none);

        plaintext.fill(std::byte {0xa5});
        REQUIRE_EQ(receiver.open(header, protected_ciphertext, tag, plaintext),
            Error::none);
        REQUIRE_EQ(plaintext, clear);

        DataHeader retransmitted = header;
        retransmitted.retransmitted = true;
        plaintext.fill(std::byte {0xa5});
        REQUIRE_EQ(
            receiver.open(retransmitted, protected_ciphertext, tag, plaintext),
            Error::none);
        REQUIRE_EQ(plaintext, clear);

        DataHeader altered = header;
        altered.timestamp = PacketTimestamp {0x1122'3345U};
        plaintext.fill(std::byte {0xa5});
        REQUIRE_EQ(receiver.open(altered, protected_ciphertext, tag, plaintext),
            Error::authentication_failure);
        REQUIRE(std::all_of(
            plaintext.begin(), plaintext.end(), [](std::byte value) {
                return value == std::byte {0};
            }));
    }
}

TEST(crypto_session_rejects_gcm_before_key_setup_when_provider_lacks_support)
{
    CtrOnlyCryptoProvider provider;
    CryptoSession session(
        {
            .passphrase = "provider capability gate",
            .mode = CryptoMode::aes_gcm,
            .enable_aes_gcm = true,
        },
        provider);
    REQUIRE_EQ(session.start_initiator(), Error::unsupported);
    REQUIRE_EQ(session.sender_state(), CryptoState::bad_crypto_mode);
    REQUIRE_EQ(session.effective_mode(), CryptoMode::automatic);
    REQUIRE(session.pending_key_material().empty());
}

TEST(crypto_session_keeps_gcm_disabled_without_the_internal_milestone_gate)
{
    CryptoSession disabled_initiator {{
        .passphrase = "production remains ctr only",
        .mode = CryptoMode::aes_gcm,
    }};
    REQUIRE_EQ(disabled_initiator.start_initiator(), Error::unsupported);
    REQUIRE_EQ(disabled_initiator.sender_state(), CryptoState::bad_crypto_mode);

    CryptoSession enabled_initiator {{
        .passphrase = "production remains ctr only",
        .mode = CryptoMode::aes_gcm,
        .enable_aes_gcm = true,
    }};
    REQUIRE_EQ(enabled_initiator.start_initiator(), Error::none);
    CryptoSession production_default {{
        .passphrase = "production remains ctr only",
    }};
    REQUIRE_EQ(production_default.accept_key_material(
                   enabled_initiator.pending_key_material(), false),
        Error::unsupported);
    REQUIRE_EQ(
        production_default.receiver_state(), CryptoState::bad_crypto_mode);
    REQUIRE_EQ(production_default.effective_mode(), CryptoMode::automatic);
}

TEST(crypto_session_enforces_gcm_negotiation_context_and_mode_mismatches)
{
    const CryptoConfiguration gcm_caller {
        .passphrase = "mode negotiation secret",
        .mode = CryptoMode::aes_gcm,
        .enable_aes_gcm = true,
    };
    CryptoSession caller {gcm_caller};
    REQUIRE_EQ(caller.start_initiator(), Error::none);

    CryptoSession automatic_listener {{
        .passphrase = "mode negotiation secret",
        .enable_aes_gcm = true,
    }};
    REQUIRE_EQ(automatic_listener.accept_key_material(
                   caller.pending_key_material(), false),
        Error::none);
    REQUIRE_EQ(automatic_listener.effective_mode(), CryptoMode::aes_gcm);

    CryptoSession ctr_listener {{
        .passphrase = "mode negotiation secret",
        .mode = CryptoMode::aes_ctr,
        .enable_aes_gcm = true,
    }};
    REQUIRE_EQ(
        ctr_listener.accept_key_material(caller.pending_key_material(), false),
        Error::unsupported);
    REQUIRE_EQ(ctr_listener.receiver_state(), CryptoState::bad_crypto_mode);
    REQUIRE_EQ(ctr_listener.effective_mode(), CryptoMode::automatic);
    REQUIRE(ctr_listener.key_material_response().empty());

    CryptoSession automatic_rendezvous {{
        .passphrase = "mode negotiation secret",
        .negotiation_context = CryptoNegotiationContext::rendezvous,
        .enable_aes_gcm = true,
    }};
    REQUIRE_EQ(automatic_rendezvous.accept_key_material(
                   caller.pending_key_material(), false),
        Error::unsupported);
    REQUIRE_EQ(
        automatic_rendezvous.receiver_state(), CryptoState::bad_crypto_mode);
    REQUIRE_EQ(automatic_rendezvous.effective_mode(), CryptoMode::automatic);

    CryptoSession gcm_rendezvous {{
        .passphrase = "mode negotiation secret",
        .mode = CryptoMode::aes_gcm,
        .negotiation_context = CryptoNegotiationContext::rendezvous,
        .enable_aes_gcm = true,
    }};
    REQUIRE_EQ(gcm_rendezvous.accept_key_material(
                   caller.pending_key_material(), false),
        Error::none);
    REQUIRE_EQ(gcm_rendezvous.effective_mode(), CryptoMode::aes_gcm);
}

TEST(crypto_session_seals_opens_and_rotates_gcm_in_rendezvous_context)
{
    const CryptoConfiguration configuration {
        .passphrase = "authenticated rendezvous session",
        .mode = CryptoMode::aes_gcm,
        .negotiation_context = CryptoNegotiationContext::rendezvous,
        .enable_aes_gcm = true,
        .key_length = 16,
        .refresh_rate_packets = 4,
        .preannouncement_packets = 1,
    };
    CryptoSession sender {configuration};
    CryptoSession receiver {configuration};
    REQUIRE_EQ(sender.start_initiator(), Error::none);
    REQUIRE_EQ(
        receiver.accept_key_material(sender.pending_key_material(), true),
        Error::none);
    REQUIRE_EQ(
        sender.acknowledge_key_material(receiver.key_material_response(), true),
        Error::none);
    confirm_directional_test_keys(sender, receiver);
    REQUIRE(sender.ready_to_send_data());

    EncryptionKey previous_key = EncryptionKey::none;
    std::size_t transitions = 0;
    for (std::uint32_t packet = 0; packet < 12U; ++packet) {
        REQUIRE_EQ(sender.prepare_rotation(), Error::none);
        if (!sender.pending_key_material().empty()) {
            REQUIRE_EQ(receiver.accept_key_material(
                           sender.pending_key_material(), false),
                Error::none);
            REQUIRE_EQ(sender.acknowledge_key_material(
                           receiver.key_material_response(), false),
                Error::none);
        }

        const auto plaintext =
            bytes_from_hex<16>("00112233445566778899aabbccddeeff");
        DataHeader header {
            .sequence = SequenceNumber {100U + packet},
            .message_number = packet + 1U,
            .boundary = MessageBoundary::solo,
            .in_order = true,
            .encryption_key = sender.active_sender_key(),
            .timestamp = PacketTimestamp {1'000U + packet},
            .destination_socket_id = 0x1234'5678U,
        };
        std::array<std::byte, 16> ciphertext {};
        std::array<std::byte, srt_gcm_authentication_tag_size> tag {};
        std::array<std::byte, 16> opened {};
        EncryptionKey key = EncryptionKey::none;
        REQUIRE_EQ(
            sender.seal(header, plaintext, ciphertext, tag, key), Error::none);
        REQUIRE_EQ(key, header.encryption_key);
        REQUIRE_EQ(receiver.open(header, ciphertext, tag, opened), Error::none);
        REQUIRE_EQ(opened, plaintext);
        if (previous_key != EncryptionKey::none && previous_key != key) {
            ++transitions;
        }
        previous_key = key;
        REQUIRE_EQ(sender.note_data_packet_sent(), Error::none);
    }
    REQUIRE(transitions >= 2U);
}

TEST(crypto_session_rotates_gcm_keys_and_handles_stale_control_messages)
{
    const CryptoConfiguration configuration {
        .passphrase = "authenticated rotation secret",
        .mode = CryptoMode::aes_gcm,
        .enable_aes_gcm = true,
        .key_length = 32,
        .refresh_rate_packets = 4,
        .preannouncement_packets = 1,
    };
    CryptoSession sender {configuration};
    CryptoSession receiver {configuration};
    REQUIRE_EQ(sender.start_initiator(), Error::none);
    const std::vector<std::byte> initial_request(
        sender.pending_key_material().begin(),
        sender.pending_key_material().end());
    REQUIRE_EQ(
        receiver.accept_key_material(initial_request, true), Error::none);
    const std::vector<std::byte> initial_response(
        receiver.key_material_response().begin(),
        receiver.key_material_response().end());
    REQUIRE_EQ(
        sender.acknowledge_key_material(initial_response, true), Error::none);
    confirm_directional_test_keys(sender, receiver);

    for (std::size_t packet = 0; packet < 3U; ++packet) {
        REQUIRE_EQ(sender.prepare_rotation(), Error::none);
        REQUIRE_EQ(sender.note_data_packet_sent(), Error::none);
    }
    REQUIRE_EQ(sender.prepare_rotation(), Error::none);
    const auto rotation_request = sender.pending_key_material();
    const auto decoded = decode_key_material(rotation_request);
    REQUIRE(decoded);
    REQUIRE_EQ(decoded.key_material.keys, EncryptionKey::reserved);
    REQUIRE_EQ(decoded.key_material.cipher, KeyMaterialCipher::aes_gcm);
    REQUIRE_EQ(decoded.key_material.authentication,
        KeyMaterialAuthentication::aes_gcm);

    REQUIRE_EQ(
        sender.acknowledge_key_material(initial_response, false), Error::none);
    REQUIRE_EQ(sender.sender_state(), CryptoState::securing);
    REQUIRE(!sender.pending_key_material().empty());
    REQUIRE_EQ(
        receiver.accept_key_material(rotation_request, false), Error::none);
    REQUIRE_EQ(
        receiver.accept_key_material(rotation_request, false), Error::none);
    REQUIRE_EQ(sender.acknowledge_key_material(
                   receiver.key_material_response(), false),
        Error::none);
    REQUIRE_EQ(sender.note_data_packet_sent(), Error::none);
    REQUIRE_EQ(sender.active_sender_key(), EncryptionKey::odd);
    REQUIRE_EQ(sender.effective_mode(), CryptoMode::aes_gcm);
    REQUIRE_EQ(receiver.effective_mode(), CryptoMode::aes_gcm);

    REQUIRE_EQ(
        receiver.accept_key_material(initial_request, false), Error::none);
    REQUIRE(std::equal(receiver.key_material_response().begin(),
        receiver.key_material_response().end(), initial_request.begin(),
        initial_request.end()));
    REQUIRE_EQ(receiver.receiver_state(), CryptoState::secured);
    REQUIRE_EQ(receiver.effective_mode(), CryptoMode::aes_gcm);
}

TEST(crypto_session_exchanges_bidirectional_keys_and_rotates)
{
    const CryptoConfiguration configuration{
        .passphrase = "correct horse battery",
        .key_length = 16,
        .refresh_rate_packets = 8,
        .preannouncement_packets = 2,
    };
    CryptoSession initiator{configuration};
    CryptoSession responder{configuration};
    REQUIRE_EQ(initiator.start_initiator(), Error::none);
    REQUIRE_EQ(initiator.sender_state(), CryptoState::securing);
    REQUIRE(!initiator.pending_key_material().empty());

    REQUIRE_EQ(responder.accept_key_material(
                   initiator.pending_key_material(), true),
        Error::none);
    REQUIRE_EQ(responder.sender_state(), CryptoState::securing);
    REQUIRE_EQ(responder.receiver_state(), CryptoState::secured);
    REQUIRE_EQ(initiator.acknowledge_key_material(
                   responder.key_material_response(), true),
        Error::none);
    confirm_directional_test_keys(initiator, responder);
    REQUIRE_EQ(initiator.sender_state(), CryptoState::secured);
    REQUIRE_EQ(initiator.receiver_state(), CryptoState::secured);

    const auto clear = bytes_from_hex<20>(
        "00112233445566778899aabbccddeeff01020304");
    std::array<std::byte, 20> encrypted{};
    std::array<std::byte, 20> decrypted{};
    EncryptionKey key = EncryptionKey::none;
    REQUIRE_EQ(initiator.encrypt(
                   SequenceNumber{77}, clear, encrypted, key),
        Error::none);
    REQUIRE_EQ(key, EncryptionKey::even);
    REQUIRE(encrypted != clear);
    REQUIRE_EQ(responder.decrypt(
                   key, SequenceNumber{77}, encrypted, decrypted),
        Error::none);
    REQUIRE_EQ(decrypted, clear);

    for (std::size_t index = 0; index < 6U; ++index) {
        REQUIRE_EQ(initiator.prepare_rotation(), Error::none);
        REQUIRE_EQ(initiator.note_data_packet_sent(), Error::none);
    }
    REQUIRE_EQ(initiator.prepare_rotation(), Error::none);
    const auto rotation_request =
        initiator.pending_key_material();
    REQUIRE(!rotation_request.empty());
    const auto decoded = decode_key_material(rotation_request);
    REQUIRE(decoded);
    REQUIRE_EQ(decoded.key_material.keys, EncryptionKey::reserved);
    REQUIRE_EQ(responder.accept_key_material(
                   rotation_request, false),
        Error::none);
    REQUIRE_EQ(initiator.acknowledge_key_material(
                   responder.key_material_response(), false),
        Error::none);

    REQUIRE_EQ(initiator.note_data_packet_sent(), Error::none);
    REQUIRE_EQ(initiator.note_data_packet_sent(), Error::none);
    REQUIRE_EQ(initiator.active_sender_key(), EncryptionKey::odd);
    REQUIRE_EQ(initiator.packets_on_active_key(), 0U);

    REQUIRE_EQ(initiator.encrypt(
                   SequenceNumber{78}, clear, encrypted, key),
        Error::none);
    REQUIRE_EQ(key, EncryptionKey::odd);
    REQUIRE_EQ(responder.decrypt(
                   key, SequenceNumber{78}, encrypted, decrypted),
        Error::none);
    REQUIRE_EQ(decrypted, clear);
}

TEST(crypto_session_decrypts_old_retransmission_after_key_reuse)
{
    const CryptoConfiguration configuration{
        .passphrase = "correct horse battery",
        .key_length = 32,
        .refresh_rate_packets = 4,
        .preannouncement_packets = 1,
    };
    CryptoSession sender{configuration};
    CryptoSession receiver{configuration};
    REQUIRE_EQ(sender.start_initiator(), Error::none);
    REQUIRE_EQ(receiver.accept_key_material(
                   sender.pending_key_material(), true),
        Error::none);
    REQUIRE_EQ(sender.acknowledge_key_material(
                   receiver.key_material_response(), true),
        Error::none);
    confirm_directional_test_keys(sender, receiver);

    const auto old_clear = bytes_from_hex<17>(
        "00112233445566778899aabbccddeeff01");
    std::array<std::byte, 17> old_ciphertext{};
    EncryptionKey old_key = EncryptionKey::none;
    SequenceNumber sequence{100};
    REQUIRE_EQ(sender.encrypt(
                   sequence, old_clear, old_ciphertext, old_key),
        Error::none);
    REQUIRE_EQ(old_key, EncryptionKey::even);
    REQUIRE_EQ(sender.note_data_packet_sent(), Error::none);
    sequence = sequence.next();

    const auto clear = bytes_from_hex<17>(
        "102132435465768798a9bacbdcedfe0f11");
    std::array<std::byte, 17> ciphertext{};
    std::array<std::byte, 17> decrypted{};
    EncryptionKey packet_key = EncryptionKey::none;
    for (std::size_t packet = 0; packet < 7U; ++packet) {
        REQUIRE_EQ(sender.prepare_rotation(), Error::none);
        const auto request = sender.pending_key_material();
        if (!request.empty()) {
            REQUIRE_EQ(receiver.accept_key_material(
                           request, false),
                Error::none);
            REQUIRE_EQ(sender.acknowledge_key_material(
                           receiver.key_material_response(), false),
                Error::none);
        }
        REQUIRE_EQ(sender.encrypt(
                       sequence, clear, ciphertext, packet_key),
            Error::none);
        REQUIRE_EQ(receiver.decrypt(
                       packet_key, sequence,
                       ciphertext, decrypted),
            Error::none);
        REQUIRE_EQ(decrypted, clear);
        // The transport reports accepted CTR sequences after the receive
        // window validated them; the crypto session no longer infers them.
        receiver.note_accepted_receive_sequence(sequence);
        REQUIRE_EQ(sender.note_data_packet_sent(), Error::none);
        sequence = sequence.next();
    }

    // The even selector has now been announced for a new generation, while
    // the stored Haivision-style retransmission still carries the old even
    // ciphertext and its original packet sequence.
    REQUIRE_EQ(sender.active_sender_key(), EncryptionKey::even);
    REQUIRE_EQ(receiver.decrypt(
                   old_key, SequenceNumber{100},
                   old_ciphertext, decrypted),
        Error::none);
    REQUIRE_EQ(decrypted, old_clear);

    REQUIRE_EQ(sender.prepare_rotation(), Error::none);
    REQUIRE_EQ(sender.encrypt(
                   sequence, clear, ciphertext, packet_key),
        Error::none);
    REQUIRE_EQ(packet_key, EncryptionKey::even);
    REQUIRE_EQ(receiver.decrypt(
                   packet_key, sequence,
                   ciphertext, decrypted),
        Error::none);
    REQUIRE_EQ(decrypted, clear);

    REQUIRE_EQ(receiver.decrypt(
                   old_key, SequenceNumber{100},
                   old_ciphertext, decrypted),
        Error::none);
    REQUIRE_EQ(decrypted, old_clear);
    REQUIRE_EQ(receiver.decrypt(
                   packet_key, sequence,
                   ciphertext, decrypted),
        Error::none);
    REQUIRE_EQ(decrypted, clear);
}

TEST(crypto_session_preserves_payloads_across_sustained_rotation_faults)
{
    const CryptoConfiguration configuration{
        .passphrase = "sustained rotation faults",
        .key_length = 32,
        .refresh_rate_packets = 500,
        .preannouncement_packets = 200,
    };
    CryptoSession sender{configuration};
    CryptoSession receiver{configuration};
    REQUIRE_EQ(sender.start_initiator(), Error::none);
    REQUIRE_EQ(receiver.accept_key_material(
                   sender.pending_key_material(), false),
        Error::none);
    REQUIRE_EQ(sender.acknowledge_key_material(
                   receiver.key_material_response(), false),
        Error::none);

    struct PendingPacket {
        SequenceNumber sequence;
        EncryptionKey key = EncryptionKey::none;
        std::array<std::byte, 32> ciphertext{};
        std::array<std::byte, 32> plaintext{};
        std::size_t deliver_after = 0;
    };
    std::vector<PendingPacket> pending;
    const std::array<std::size_t, 3> drops{
        495, 1'495, 2'495};
    const std::array<std::size_t, 3> reorders{
        995, 1'995, 2'995};
    const std::array<std::size_t, 6> delays{
        505, 1'005, 1'505, 2'005, 2'505, 3'005};
    const auto contains =
        [](const auto& values, std::size_t occurrence) {
        return std::find(
                   values.begin(), values.end(), occurrence)
            != values.end();
    };
    const auto deliver = [&receiver](const PendingPacket& packet) {
        std::array<std::byte, 32> decrypted{};
        REQUIRE_EQ(receiver.decrypt(
                       packet.key, packet.sequence,
                       packet.ciphertext, decrypted),
            Error::none);
        REQUIRE_EQ(decrypted, packet.plaintext);
        receiver.note_accepted_receive_sequence(packet.sequence);
    };

    SequenceNumber sequence{100};
    for (std::size_t occurrence = 1;
         occurrence <= 3'500; ++occurrence) {
        REQUIRE_EQ(sender.prepare_rotation(), Error::none);
        const auto request = sender.pending_key_material();
        if (!request.empty()) {
            REQUIRE_EQ(receiver.accept_key_material(
                           request, false),
                Error::none);
            // Haivision may repeat a KMREQ until its response arrives.
            REQUIRE_EQ(receiver.accept_key_material(
                           request, false),
                Error::none);
            REQUIRE_EQ(sender.acknowledge_key_material(
                           receiver.key_material_response(), false),
                Error::none);
        }

        PendingPacket packet;
        packet.sequence = sequence;
        for (std::size_t index = 0;
             index < packet.plaintext.size(); ++index) {
            packet.plaintext[index] = static_cast<std::byte>(
                (occurrence * 17U + index) & 0xffU);
        }
        REQUIRE_EQ(sender.encrypt(
                       packet.sequence, packet.plaintext,
                       packet.ciphertext, packet.key),
            Error::none);
        REQUIRE_EQ(sender.note_data_packet_sent(), Error::none);

        if (contains(drops, occurrence)) {
            // Model TSBPD/NAK latency that lets the selector be reused
            // multiple times before the missing packet is retransmitted.
            packet.deliver_after = 3'501U;
            pending.push_back(packet);
        } else if (contains(reorders, occurrence)) {
            packet.deliver_after = occurrence + 1U;
            pending.push_back(packet);
        } else if (contains(delays, occurrence)) {
            packet.deliver_after = occurrence + 40U;
            pending.push_back(packet);
        } else {
            deliver(packet);
        }

        for (auto candidate = pending.begin();
             candidate != pending.end();) {
            if (candidate->deliver_after > occurrence) {
                ++candidate;
                continue;
            }
            deliver(*candidate);
            candidate = pending.erase(candidate);
        }
        sequence = sequence.next();
    }
    for (const auto& packet : pending) {
        deliver(packet);
    }
}

TEST(crypto_session_rejects_ciphertext_older_than_bounded_key_history)
{
    const CryptoConfiguration configuration{
        .passphrase = "bounded receive key history",
        .key_length = 16,
        .refresh_rate_packets = 3,
        .preannouncement_packets = 1,
    };
    CryptoSession sender{configuration};
    CryptoSession receiver{configuration};
    REQUIRE_EQ(sender.start_initiator(), Error::none);
    REQUIRE_EQ(receiver.accept_key_material(
                   sender.pending_key_material(), false),
        Error::none);
    REQUIRE_EQ(sender.acknowledge_key_material(
                   receiver.key_material_response(), false),
        Error::none);

    const std::array<std::byte, 16> clear{
        std::byte{0x01}, std::byte{0x02}, std::byte{0x03},
        std::byte{0x04}, std::byte{0x05}, std::byte{0x06},
        std::byte{0x07}, std::byte{0x08}, std::byte{0x09},
        std::byte{0x0a}, std::byte{0x0b}, std::byte{0x0c},
        std::byte{0x0d}, std::byte{0x0e}, std::byte{0x0f},
        std::byte{0x10},
    };
    std::array<std::byte, 16> old_ciphertext{};
    EncryptionKey old_key = EncryptionKey::none;
    REQUIRE_EQ(sender.encrypt(
                   SequenceNumber{100}, clear,
                   old_ciphertext, old_key),
        Error::none);
    REQUIRE_EQ(sender.note_data_packet_sent(), Error::none);

    SequenceNumber sequence{101};
    for (std::size_t occurrence = 2;
         occurrence <= 40; ++occurrence) {
        REQUIRE_EQ(sender.prepare_rotation(), Error::none);
        const auto request = sender.pending_key_material();
        if (!request.empty()) {
            REQUIRE_EQ(receiver.accept_key_material(
                           request, false),
                Error::none);
            REQUIRE_EQ(sender.acknowledge_key_material(
                           receiver.key_material_response(), false),
                Error::none);
        }

        std::array<std::byte, 16> ciphertext{};
        std::array<std::byte, 16> plaintext{};
        EncryptionKey key = EncryptionKey::none;
        REQUIRE_EQ(sender.encrypt(
                       sequence, clear, ciphertext, key),
            Error::none);
        REQUIRE_EQ(sender.note_data_packet_sent(), Error::none);
        REQUIRE_EQ(receiver.decrypt(
                       key, sequence, ciphertext, plaintext),
            Error::none);
        REQUIRE_EQ(plaintext, clear);
        receiver.note_accepted_receive_sequence(sequence);
        sequence = sequence.next();
    }

    std::array<std::byte, 16> decrypted{};
    REQUIRE_EQ(receiver.decrypt(
                   old_key, SequenceNumber{100},
                   old_ciphertext, decrypted),
        Error::cryptographic_failure);
}

TEST(crypto_session_routes_current_key_past_old_half_range_ceiling)
{
    const CryptoConfiguration configuration {
        .passphrase = "long receive key history fixture",
        .key_length = 16,
        .refresh_rate_packets = maximum_key_refresh_rate,
        .preannouncement_packets = 1,
    };
    CryptoSession old_sender {configuration};
    CryptoSession current_sender {configuration};
    CryptoSession receiver {configuration};
    REQUIRE_EQ(old_sender.start_initiator(), Error::none);
    REQUIRE_EQ(
        receiver.accept_key_material(old_sender.pending_key_material(), false),
        Error::none);
    REQUIRE_EQ(old_sender.acknowledge_key_material(
                   receiver.key_material_response(), false),
        Error::none);

    const SequenceNumber old_sequence {SequenceNumber::mask - 20U};
    const std::array<std::byte, 16> old_clear {std::byte {0x31}};
    const std::array<std::byte, 16> current_clear {std::byte {0x72}};
    std::array<std::byte, 16> ciphertext {};
    std::array<std::byte, 16> plaintext {};
    EncryptionKey key = EncryptionKey::none;
    REQUIRE_EQ(old_sender.encrypt(old_sequence, old_clear, ciphertext, key),
        Error::none);
    REQUIRE_EQ(receiver.decrypt(key, old_sequence, ciphertext, plaintext),
        Error::none);
    REQUIRE_EQ(plaintext, old_clear);
    receiver.note_accepted_receive_sequence(old_sequence);

    REQUIRE_EQ(current_sender.start_initiator(), Error::none);
    REQUIRE_EQ(receiver.accept_key_material(
                   current_sender.pending_key_material(), false),
        Error::none);
    REQUIRE_EQ(current_sender.acknowledge_key_material(
                   receiver.key_material_response(), false),
        Error::none);
    const SequenceNumber current_start = old_sequence.advanced(6U);
    REQUIRE_EQ(
        current_sender.encrypt(current_start, current_clear, ciphertext, key),
        Error::none);
    REQUIRE_EQ(receiver.decrypt(key, current_start, ciphertext, plaintext),
        Error::none);
    REQUIRE_EQ(plaintext, current_clear);
    receiver.note_accepted_receive_sequence(current_start);

    // Model accepted progress across an otherwise omitted long flight. Every
    // observation advances by at most one receive-window-sized step.
    for (std::uint32_t offset = 8'192U;
        offset < SequenceNumber::half_range - 10U; offset += 8'192U) {
        receiver.note_accepted_receive_sequence(old_sequence.advanced(offset));
    }
    const SequenceNumber near_boundary =
        old_sequence.advanced(SequenceNumber::half_range - 10U);
    REQUIRE_EQ(
        current_sender.encrypt(near_boundary, current_clear, ciphertext, key),
        Error::none);
    REQUIRE_EQ(receiver.decrypt(key, near_boundary, ciphertext, plaintext),
        Error::none);
    REQUIRE_EQ(plaintext, current_clear);
    receiver.note_accepted_receive_sequence(near_boundary);

    const SequenceNumber past_boundary =
        old_sequence.advanced(SequenceNumber::half_range + 1U);
    REQUIRE_EQ(
        current_sender.encrypt(past_boundary, current_clear, ciphertext, key),
        Error::none);
    REQUIRE_EQ(receiver.decrypt(key, past_boundary, ciphertext, plaintext),
        Error::none);
    REQUIRE_EQ(plaintext, current_clear);
}

TEST(crypto_session_ignores_aged_discarded_receive_key_ceiling)
{
    const CryptoConfiguration configuration {
        .passphrase = "discarded receive ceiling fixture",
        .key_length = 16,
        .refresh_rate_packets = maximum_key_refresh_rate,
        .preannouncement_packets = 1,
    };
    CryptoSession receiver {configuration};
    std::array<std::unique_ptr<CryptoSession>, 7> senders {};
    const std::array<std::byte, 16> clear {std::byte {0x5a}};
    std::array<std::byte, 16> ciphertext {};
    std::array<std::byte, 16> plaintext {};
    EncryptionKey key = EncryptionKey::none;

    for (std::uint32_t index = 0; index < 6U; ++index) {
        senders[index] = std::make_unique<CryptoSession>(configuration);
        REQUIRE_EQ(senders[index]->start_initiator(), Error::none);
        REQUIRE_EQ(receiver.accept_key_material(
                       senders[index]->pending_key_material(), false),
            Error::none);
        REQUIRE_EQ(senders[index]->acknowledge_key_material(
                       receiver.key_material_response(), false),
            Error::none);
        const SequenceNumber sequence {index};
        REQUIRE_EQ(senders[index]->encrypt(sequence, clear, ciphertext, key),
            Error::none);
        REQUIRE_EQ(receiver.decrypt(key, sequence, ciphertext, plaintext),
            Error::none);
        REQUIRE_EQ(plaintext, clear);
        receiver.note_accepted_receive_sequence(sequence);
    }

    const SequenceNumber delayed_sequence {SequenceNumber::half_range + 1U};
    std::array<std::byte, 16> delayed_ciphertext {};
    EncryptionKey delayed_key = EncryptionKey::none;
    REQUIRE_EQ(senders[5]->encrypt(
                   delayed_sequence, clear, delayed_ciphertext, delayed_key),
        Error::none);
    for (std::uint32_t offset = 8'192U;
        offset < SequenceNumber::half_range + 10U; offset += 8'192U) {
        receiver.note_accepted_receive_sequence(SequenceNumber {offset});
    }
    const SequenceNumber frontier {SequenceNumber::half_range + 10U};
    REQUIRE_EQ(
        senders[5]->encrypt(frontier, clear, ciphertext, key), Error::none);
    REQUIRE_EQ(
        receiver.decrypt(key, frontier, ciphertext, plaintext), Error::none);
    REQUIRE_EQ(plaintext, clear);
    receiver.note_accepted_receive_sequence(frontier);

    senders[6] = std::make_unique<CryptoSession>(configuration);
    REQUIRE_EQ(senders[6]->start_initiator(), Error::none);
    REQUIRE_EQ(
        receiver.accept_key_material(senders[6]->pending_key_material(), false),
        Error::none);
    REQUIRE_EQ(receiver.decrypt(delayed_key, delayed_sequence,
                   delayed_ciphertext, plaintext),
        Error::none);
    REQUIRE_EQ(plaintext, clear);
}

TEST(crypto_session_derives_each_directional_kek_once_across_rotations)
{
    // The salt is constant per direction for the life of a session, so the
    // 2048-iteration PBKDF2 must run once per direction, not once per key
    // material message. Forged KMREQs with a different salt still derive.
    CtrOnlyCryptoProvider provider;
    const CryptoConfiguration configuration {
        .passphrase = "one derivation per direction",
        .key_length = 16,
        .refresh_rate_packets = 5,
        .preannouncement_packets = 2,
    };
    CryptoSession sender {configuration, provider};
    CryptoSession receiver {configuration, provider};
    REQUIRE_EQ(sender.start_initiator(), Error::none);
    REQUIRE_EQ(
        receiver.accept_key_material(sender.pending_key_material(), false),
        Error::none);
    REQUIRE_EQ(sender.acknowledge_key_material(
                   receiver.key_material_response(), false),
        Error::none);
    const std::size_t after_handshake = provider.pbkdf2_calls();
    REQUIRE(after_handshake >= 2U);

    const auto clear = bytes_from_hex<16>("000102030405060708090a0b0c0d0e0f");
    std::array<std::byte, 16> ciphertext {};
    std::array<std::byte, 16> plaintext {};
    SequenceNumber sequence {100};
    std::size_t exchanged = 0;
    for (std::size_t packet = 0; packet < 40U; ++packet) {
        EncryptionKey key = EncryptionKey::none;
        REQUIRE_EQ(sender.prepare_rotation(), Error::none);
        const auto request = sender.pending_key_material();
        if (!request.empty()) {
            ++exchanged;
            REQUIRE_EQ(
                receiver.accept_key_material(request, false), Error::none);
            REQUIRE_EQ(sender.acknowledge_key_material(
                           receiver.key_material_response(), false),
                Error::none);
        }
        REQUIRE_EQ(
            sender.encrypt(sequence, clear, ciphertext, key), Error::none);
        REQUIRE_EQ(sender.note_data_packet_sent(), Error::none);
        REQUIRE_EQ(receiver.decrypt(key, sequence, ciphertext, plaintext),
            Error::none);
        REQUIRE_EQ(plaintext, clear);
        receiver.note_accepted_receive_sequence(sequence);
        sequence = sequence.next();
    }
    REQUIRE(exchanged >= 5U);
    REQUIRE_EQ(provider.pbkdf2_calls(), after_handshake);
}

TEST(crypto_session_does_not_cache_unwrappable_fresh_salt)
{
    CtrOnlyCryptoProvider provider;
    const CryptoConfiguration configuration {
        .passphrase = "cache only validated receive kek",
        .key_length = 16,
        .refresh_rate_packets = 5,
        .preannouncement_packets = 2,
    };
    CryptoSession sender {configuration};
    CryptoSession receiver {configuration, provider};
    REQUIRE_EQ(sender.start_initiator(), Error::none);
    const std::vector<std::byte> initial_request {
        sender.pending_key_material().begin(),
        sender.pending_key_material().end()};
    REQUIRE_EQ(
        receiver.accept_key_material(initial_request, false), Error::none);
    REQUIRE_EQ(sender.acknowledge_key_material(
                   receiver.key_material_response(), false),
        Error::none);
    const std::size_t after_initial = provider.pbkdf2_calls();

    auto forged = initial_request;
    const auto decoded = decode_key_material(forged);
    REQUIRE(decoded);
    const std::size_t salt_offset = static_cast<std::size_t>(
        decoded.key_material.salt.data() - forged.data());
    forged[salt_offset + srt_salt_size - 1U] ^= std::byte {0x01};
    REQUIRE_EQ(receiver.accept_key_material(forged, false),
        Error::cryptographic_failure);
    REQUIRE_EQ(provider.pbkdf2_calls(), after_initial + 1U);
    REQUIRE_EQ(receiver.receiver_state(), CryptoState::secured);

    std::vector<std::byte> rotation_request;
    for (int sent = 0; sent < 8 && rotation_request.empty(); ++sent) {
        REQUIRE_EQ(sender.note_data_packet_sent(), Error::none);
        REQUIRE_EQ(sender.prepare_rotation(), Error::none);
        const auto pending = sender.pending_key_material();
        rotation_request.assign(pending.begin(), pending.end());
    }
    REQUIRE(!rotation_request.empty());
    REQUIRE_EQ(
        receiver.accept_key_material(rotation_request, false), Error::none);
    REQUIRE_EQ(provider.pbkdf2_calls(), after_initial + 1U);
}

TEST(crypto_runtime_budgets_uncached_key_derivations)
{
    using namespace robotweax::srt::compat;
    CtrOnlyCryptoProvider provider;
    const CryptoConfiguration configuration {
        .passphrase = "budget runtime key derivations",
        .key_length = 16,
        .refresh_rate_packets = 5,
        .preannouncement_packets = 2,
    };
    CryptoSession sender {configuration};
    auto receiver_crypto =
        std::make_shared<CryptoSession>(configuration, provider);
    REQUIRE_EQ(sender.start_initiator(), Error::none);
    const std::vector<std::byte> initial_request {
        sender.pending_key_material().begin(),
        sender.pending_key_material().end()};
    REQUIRE_EQ(receiver_crypto->accept_key_material(initial_request, false),
        Error::none);
    REQUIRE_EQ(sender.acknowledge_key_material(
                   receiver_crypto->key_material_response(), false),
        Error::none);
    const std::size_t after_initial = provider.pbkdf2_calls();

    const auto channel = std::make_shared<DatagramChannel>();
    channel->set_send_hook_for_testing(
        [](std::span<const std::byte> bytes, Ipv4Endpoint,
            void*) noexcept -> UdpIoResult {
            return {.bytes_transferred = bytes.size()};
        },
        nullptr);
    const Ipv4Endpoint peer {.address = {192, 0, 2, 49}, .port = 14'209};
    std::uint64_t now = 1'000'000;
    ConnectionRuntime runtime {{
        .channel = channel,
        .peer = peer,
        .peer_socket_id = 490,
        .initial_sequence = SequenceNumber {900},
        .origin = ConnectionRuntime::Clock::now(),
        .crypto = receiver_crypto,
        .now_function = [](void* context) noexcept -> std::uint64_t {
            return *static_cast<std::uint64_t*>(context);
        },
        .now_context = &now,
    }};
    PacketView request {
        .kind = PacketKind::control,
        .control = {.type = ControlType::user_defined,
            .subtype = key_material_request_subtype,
            .destination_socket_id = 490},
    };
    auto forged = initial_request;
    const auto decoded = decode_key_material(forged);
    REQUIRE(decoded);
    const std::size_t salt_offset = static_cast<std::size_t>(
        decoded.key_material.salt.data() - forged.data());
    const std::byte original_salt_byte =
        forged[salt_offset + srt_salt_size - 1U];
    forged[salt_offset + srt_salt_size - 1U] =
        original_salt_byte ^ std::byte {0x01};
    request.payload = forged;
    const Ipv4Endpoint foreign_peer {
        .address = {192, 0, 2, 50}, .port = peer.port};
    runtime.process_packet(request, foreign_peer);
    REQUIRE_EQ(provider.pbkdf2_calls(), after_initial);
    for (std::uint8_t index = 1; index <= 5; ++index) {
        forged[salt_offset + srt_salt_size - 1U] =
            original_salt_byte ^ static_cast<std::byte>(index);
        request.payload = forged;
        runtime.process_packet(request, peer);
        REQUIRE(!runtime.broken());
    }
    REQUIRE_EQ(provider.pbkdf2_calls(), after_initial + 1U);

    std::vector<std::byte> rotation_request;
    for (int sent = 0; sent < 8 && rotation_request.empty(); ++sent) {
        REQUIRE_EQ(sender.note_data_packet_sent(), Error::none);
        REQUIRE_EQ(sender.prepare_rotation(), Error::none);
        const auto pending = sender.pending_key_material();
        rotation_request.assign(pending.begin(), pending.end());
    }
    REQUIRE(!rotation_request.empty());
    request.payload = rotation_request;
    runtime.process_packet(request, peer);
    REQUIRE(!runtime.broken());
    REQUIRE_EQ(receiver_crypto->key_material_response().size(),
        rotation_request.size());
    REQUIRE(std::equal(rotation_request.begin(), rotation_request.end(),
        receiver_crypto->key_material_response().begin()));
    REQUIRE_EQ(provider.pbkdf2_calls(), after_initial + 1U);

    now += 100'000;
    forged[salt_offset + srt_salt_size - 1U] =
        original_salt_byte ^ std::byte {0x06};
    request.payload = forged;
    runtime.process_packet(request, peer);
    REQUIRE_EQ(provider.pbkdf2_calls(), after_initial + 2U);

    CryptoSession fresh_sender {configuration};
    REQUIRE_EQ(fresh_sender.start_initiator(), Error::none);
    const auto fresh_request = fresh_sender.pending_key_material();
    now += 100'000;
    request.payload = fresh_request;
    runtime.process_packet(request, peer);
    REQUIRE(!runtime.broken());
    REQUIRE_EQ(provider.pbkdf2_calls(), after_initial + 3U);
    REQUIRE(std::equal(fresh_request.begin(), fresh_request.end(),
        receiver_crypto->key_material_response().begin()));
}

TEST(crypto_session_routes_ctr_keys_from_transport_accepted_sequences)
{
    // AES-CTR cannot authenticate a packet, so decrypt() itself records no
    // sequence for key-generation routing. The transport reports sequences
    // the receive window accepted through note_accepted_receive_sequence();
    // an unreported decrypt attempt must leave routing unchanged and genuine
    // traffic must keep decrypting across rotations.
    const CryptoConfiguration configuration {
        .passphrase = "accepted sequences route keys",
        .key_length = 16,
        .refresh_rate_packets = 5,
        .preannouncement_packets = 2,
    };
    CryptoSession sender {configuration};
    CryptoSession receiver {configuration};
    REQUIRE_EQ(sender.start_initiator(), Error::none);
    REQUIRE_EQ(
        receiver.accept_key_material(sender.pending_key_material(), false),
        Error::none);
    REQUIRE_EQ(sender.acknowledge_key_material(
                   receiver.key_material_response(), false),
        Error::none);

    const auto clear = bytes_from_hex<16>("000102030405060708090a0b0c0d0e0f");
    std::array<std::byte, 16> ciphertext {};
    std::array<std::byte, 16> plaintext {};
    SequenceNumber sequence {100};
    const auto exchange_keys = [&] {
        const auto request = sender.pending_key_material();
        if (!request.empty()) {
            REQUIRE_EQ(
                receiver.accept_key_material(request, false), Error::none);
            REQUIRE_EQ(sender.acknowledge_key_material(
                           receiver.key_material_response(), false),
                Error::none);
        }
    };
    const auto send_one = [&] {
        EncryptionKey key = EncryptionKey::none;
        REQUIRE_EQ(sender.prepare_rotation(), Error::none);
        exchange_keys();
        REQUIRE_EQ(
            sender.encrypt(sequence, clear, ciphertext, key), Error::none);
        REQUIRE_EQ(sender.note_data_packet_sent(), Error::none);
        REQUIRE_EQ(receiver.decrypt(key, sequence, ciphertext, plaintext),
            Error::none);
        REQUIRE_EQ(plaintext, clear);
        receiver.note_accepted_receive_sequence(sequence);
        sequence = sequence.next();
    };
    send_one();

    // A decrypt attempt the transport never accepts (out of window) is not
    // reported and must not influence later key selection.
    std::array<std::byte, 16> unaccepted {};
    (void)receiver.decrypt(EncryptionKey::even,
        sequence.advanced((1U << 30) - 1U), ciphertext, unaccepted);

    for (std::size_t packet = 0; packet < 40U; ++packet) {
        send_one();
    }
}

TEST(key_material_state_uses_the_reference_byte_layout)
{
    // The reference implementation sends NOSECRET as 03 00 00 00 on the wire.
    const auto nosecret = encode_key_material_state(CryptoState::no_secret);
    REQUIRE_EQ(nosecret[0], std::byte {3});
    REQUIRE_EQ(nosecret[1], std::byte {0});
    REQUIRE_EQ(nosecret[2], std::byte {0});
    REQUIRE_EQ(nosecret[3], std::byte {0});

    const std::array little {
        std::byte {3}, std::byte {0}, std::byte {0}, std::byte {0}};
    const std::array big {
        std::byte {0}, std::byte {0}, std::byte {0}, std::byte {4}};
    const std::array zero {
        std::byte {0}, std::byte {0}, std::byte {0}, std::byte {0}};
    const std::array unknown {
        std::byte {6}, std::byte {0}, std::byte {0}, std::byte {0}};
    const std::array mixed {
        std::byte {3}, std::byte {0}, std::byte {0}, std::byte {4}};
    REQUIRE(decode_key_material_state(little) == CryptoState::no_secret);
    REQUIRE(decode_key_material_state(big) == CryptoState::bad_secret);
    REQUIRE(decode_key_material_state(zero) == CryptoState::unsecured);
    REQUIRE(!decode_key_material_state(unknown).has_value());
    REQUIRE(!decode_key_material_state(mixed).has_value());
    REQUIRE(
        !decode_key_material_state(std::span {little}.first(3)).has_value());

    // A reference NOSECRET response is recorded as NOSECRET, not BADSECRET.
    CryptoSession sender {{
        .passphrase = "reference state layout fixture",
    }};
    REQUIRE_EQ(sender.start_initiator(), Error::none);
    REQUIRE_EQ(sender.acknowledge_key_material(little, false),
        Error::cryptographic_failure);
    REQUIRE_EQ(sender.sender_state(), CryptoState::no_secret);
}

TEST(crypto_session_erases_pending_announcement_before_optional_fallback)
{
    CtrOnlyCryptoProvider provider;
    CryptoSession sender {
        {.passphrase = "optional erasure fixture", .key_length = 16}, provider};
    REQUIRE_EQ(sender.start_initiator(), Error::none);
    REQUIRE(!sender.pending_key_material().empty());
    provider.watch_erasure(sender.pending_key_material().data());
    REQUIRE_EQ(
        sender.continue_without_peer_key(CryptoState::no_secret), Error::none);
    REQUIRE(provider.observed_erasure());
    REQUIRE(sender.pending_key_material().empty());
    REQUIRE(sender.sending_without_peer_key());
}

TEST(crypto_session_reports_a_wrong_passphrase)
{
    CryptoSession sender{{
        .passphrase = "one passphrase",
    }};
    CryptoSession receiver{{
        .passphrase = "other password",
    }};
    REQUIRE_EQ(sender.start_initiator(), Error::none);
    REQUIRE_EQ(receiver.accept_key_material(
                   sender.pending_key_material(), true),
        Error::cryptographic_failure);
    REQUIRE_EQ(receiver.receiver_state(), CryptoState::bad_secret);
}

TEST(secured_crypto_session_rejects_rogue_key_control_without_downgrade)
{
    const CryptoConfiguration configuration{
        .passphrase = "preserve secured crypto state",
        .key_length = 16,
        .refresh_rate_packets = 3,
        .preannouncement_packets = 1,
    };
    CryptoSession sender{configuration};
    CryptoSession receiver{configuration};
    REQUIRE_EQ(sender.start_initiator(), Error::none);
    const std::vector<std::byte> initial_request(
        sender.pending_key_material().begin(),
        sender.pending_key_material().end());
    REQUIRE_EQ(receiver.accept_key_material(
                   initial_request, true),
        Error::none);
    REQUIRE_EQ(sender.acknowledge_key_material(
                   receiver.key_material_response(), true),
        Error::none);
    confirm_directional_test_keys(sender, receiver);
    REQUIRE_EQ(sender.sender_state(), CryptoState::secured);
    REQUIRE_EQ(receiver.receiver_state(), CryptoState::secured);

    std::vector<std::byte> forged_request = initial_request;
    forged_request.back() ^= std::byte{0x01};
    REQUIRE_EQ(receiver.accept_key_material(
                   forged_request, false),
        Error::cryptographic_failure);
    REQUIRE_EQ(receiver.receiver_state(), CryptoState::secured);

    forged_request.push_back(std::byte{0});
    REQUIRE_EQ(receiver.accept_key_material(
                   forged_request, false),
        Error::invalid_key_material);
    REQUIRE_EQ(receiver.receiver_state(), CryptoState::secured);

    std::array<std::byte, 4> forged_response {};
    for (const std::uint8_t state :
        {static_cast<std::uint8_t>(SRT_KM_S_BADCRYPTOMODE),
            static_cast<std::uint8_t>(SRT_KM_S_E_SIZE),
            static_cast<std::uint8_t>(SRT_KM_S_E_SIZE + 1)}) {
        forged_response.back() = static_cast<std::byte>(state);
        REQUIRE_EQ(sender.acknowledge_key_material(forged_response, false),
            Error::cryptographic_failure);
        REQUIRE_EQ(sender.sender_state(), CryptoState::secured);
    }

    const auto clear = bytes_from_hex<17>(
        "00112233445566778899aabbccddeeff01");
    std::array<std::byte, 17> encrypted{};
    std::array<std::byte, 17> decrypted{};
    EncryptionKey key = EncryptionKey::none;
    REQUIRE_EQ(sender.encrypt(
                   SequenceNumber{91}, clear, encrypted, key),
        Error::none);
    REQUIRE_EQ(receiver.decrypt(
                   key, SequenceNumber{91}, encrypted, decrypted),
        Error::none);
    REQUIRE_EQ(decrypted, clear);

    REQUIRE_EQ(sender.note_data_packet_sent(), Error::none);
    REQUIRE_EQ(sender.note_data_packet_sent(), Error::none);
    REQUIRE_EQ(sender.prepare_rotation(), Error::none);
    REQUIRE_EQ(sender.sender_state(), CryptoState::securing);
    REQUIRE_EQ(sender.acknowledge_key_material(
                   forged_response, false),
        Error::cryptographic_failure);
    REQUIRE_EQ(sender.sender_state(), CryptoState::securing);

    const auto rotation_request = sender.pending_key_material();
    REQUIRE_EQ(receiver.accept_key_material(
                   rotation_request, false),
        Error::none);
    REQUIRE_EQ(sender.acknowledge_key_material(
                   receiver.key_material_response(), false),
        Error::none);
    REQUIRE_EQ(sender.note_data_packet_sent(), Error::none);
    REQUIRE_EQ(sender.sender_state(), CryptoState::secured);
}

TEST(initial_crypto_negotiation_can_report_optional_failure)
{
    CryptoSession sender{{
        .passphrase = "optional initial encryption",
        .key_length = 16,
    }};
    REQUIRE_EQ(sender.start_initiator(), Error::none);
    REQUIRE_EQ(sender.sender_state(), CryptoState::securing);

    const std::array<std::byte, 4> no_secret{
        std::byte{0}, std::byte{0}, std::byte{0},
        static_cast<std::byte>(CryptoState::no_secret),
    };
    REQUIRE_EQ(sender.acknowledge_key_material(
                   no_secret, false),
        Error::cryptographic_failure);
    REQUIRE_EQ(sender.sender_state(), CryptoState::no_secret);
}

TEST(key_material_decoder_rejects_truncated_unaligned_and_oversized_requests)
{
    CryptoSession sender{{
        .passphrase = "bounded key material parser",
        .key_length = 32,
    }};
    REQUIRE_EQ(sender.start_initiator(), Error::none);
    const auto valid = sender.pending_key_material();
    REQUIRE(decode_key_material(valid));

    for (std::size_t size = 0; size < valid.size(); ++size) {
        REQUIRE_EQ(decode_key_material(valid.first(size)).error,
            Error::invalid_key_material);
    }

    std::vector<std::byte> trailing(valid.begin(), valid.end());
    for (std::size_t extra = 1; extra <= 4; ++extra) {
        trailing.push_back(std::byte{0});
        REQUIRE_EQ(decode_key_material(trailing).error,
            Error::invalid_key_material);
    }

    std::vector<std::byte> oversized(4'096, std::byte{0});
    std::copy(valid.begin(), valid.end(), oversized.begin());
    REQUIRE_EQ(decode_key_material(oversized).error,
        Error::invalid_key_material);
}

TEST(crypto_session_supports_every_specified_aes_key_length)
{
    for (const std::size_t key_length : {16U, 24U, 32U}) {
        const CryptoConfiguration configuration{
            .passphrase = "correct horse battery",
            .key_length = key_length,
        };
        CryptoSession sender{configuration};
        CryptoSession receiver{configuration};
        REQUIRE_EQ(sender.start_initiator(), Error::none);
        const auto material =
            decode_key_material(sender.pending_key_material());
        REQUIRE(material);
        REQUIRE_EQ(material.key_material.key_length, key_length);
        REQUIRE_EQ(receiver.accept_key_material(
                       sender.pending_key_material(), true),
            Error::none);
        REQUIRE_EQ(sender.acknowledge_key_material(
                       receiver.key_material_response(), true),
            Error::none);
        confirm_directional_test_keys(sender, receiver);

        const auto clear = bytes_from_hex<17>(
            "00112233445566778899aabbccddeeff01");
        std::array<std::byte, 17> encrypted{};
        std::array<std::byte, 17> decrypted{};
        EncryptionKey key = EncryptionKey::none;
        REQUIRE_EQ(sender.encrypt(
                       SequenceNumber{91}, clear, encrypted, key),
            Error::none);
        REQUIRE_EQ(receiver.decrypt(
                       key, SequenceNumber{91},
                       encrypted, decrypted),
            Error::none);
        REQUIRE_EQ(decrypted, clear);
    }
}

TEST(crypto_session_pauses_at_an_unacknowledged_refresh_boundary)
{
    const CryptoConfiguration configuration{
        .passphrase = "correct horse battery",
        .refresh_rate_packets = 4,
        .preannouncement_packets = 1,
    };
    CryptoSession sender{configuration};
    CryptoSession receiver{configuration};
    REQUIRE_EQ(sender.start_initiator(), Error::none);
    REQUIRE_EQ(receiver.accept_key_material(
                   sender.pending_key_material(), true),
        Error::none);
    REQUIRE_EQ(sender.acknowledge_key_material(
                   receiver.key_material_response(), true),
        Error::none);
    confirm_directional_test_keys(sender, receiver);

    REQUIRE_EQ(sender.note_data_packet_sent(), Error::none);
    REQUIRE_EQ(sender.note_data_packet_sent(), Error::none);
    REQUIRE_EQ(sender.note_data_packet_sent(), Error::none);
    REQUIRE_EQ(sender.prepare_rotation(), Error::none);
    REQUIRE(!sender.ready_to_send_data());
    REQUIRE_EQ(receiver.accept_key_material(
                   sender.pending_key_material(), false),
        Error::none);
    REQUIRE_EQ(sender.acknowledge_key_material(
                   receiver.key_material_response(), false),
        Error::none);
    REQUIRE(sender.ready_to_send_data());
    REQUIRE_EQ(sender.note_data_packet_sent(), Error::none);
    REQUIRE_EQ(sender.active_sender_key(), EncryptionKey::odd);
}

TEST(crypto_session_survives_repeated_rotation_and_sequence_rollover)
{
    constexpr std::uint32_t refresh_rate = 4;
    constexpr std::size_t rotation_count = 5;
    const CryptoConfiguration configuration{
        .passphrase = "correct horse battery",
        .key_length = 32,
        .refresh_rate_packets = refresh_rate,
        .preannouncement_packets = 1,
    };
    CryptoSession sender{configuration};
    CryptoSession receiver{configuration};
    REQUIRE_EQ(sender.start_initiator(), Error::none);
    REQUIRE_EQ(receiver.accept_key_material(
                   sender.pending_key_material(), true),
        Error::none);
    REQUIRE_EQ(sender.acknowledge_key_material(
                   receiver.key_material_response(), true),
        Error::none);
    confirm_directional_test_keys(sender, receiver);

    const auto clear = bytes_from_hex<17>(
        "00112233445566778899aabbccddeeff01");
    std::array<std::byte, 17> encrypted{};
    std::array<std::byte, 17> decrypted{};
    SequenceNumber sequence{SequenceNumber::mask - 2U};
    KeyMaterialBuffer first_request;
    KeyMaterialBuffer first_response;

    for (std::size_t rotation = 0;
         rotation < rotation_count; ++rotation) {
        const EncryptionKey key_before_rotation =
            rotation % 2U == 0U
            ? EncryptionKey::even : EncryptionKey::odd;
        REQUIRE_EQ(sender.active_sender_key(),
            key_before_rotation);

        for (std::uint32_t packet = 0;
             packet < refresh_rate; ++packet) {
            REQUIRE_EQ(sender.prepare_rotation(), Error::none);
            const auto request = sender.pending_key_material();
            if (!request.empty()) {
                const auto decoded = decode_key_material(request);
                REQUIRE(decoded);
                REQUIRE_EQ(decoded.key_material.keys,
                    EncryptionKey::reserved);
                if (rotation == 0U) {
                    std::copy(request.begin(), request.end(),
                        first_request.bytes.begin());
                    first_request.size = request.size();
                }
                if (rotation == 2U) {
                    REQUIRE_EQ(sender.acknowledge_key_material(
                                   first_response.view(), false),
                        Error::none);
                    REQUIRE_EQ(sender.sender_state(),
                        CryptoState::securing);
                    REQUIRE(!sender.pending_key_material().empty());
                }

                REQUIRE_EQ(receiver.accept_key_material(
                               request, false),
                    Error::none);
                REQUIRE_EQ(receiver.accept_key_material(
                               request, false),
                    Error::none);
                const auto response =
                    receiver.key_material_response();
                if (rotation == 0U) {
                    std::copy(response.begin(), response.end(),
                        first_response.bytes.begin());
                    first_response.size = response.size();
                }
                REQUIRE_EQ(sender.acknowledge_key_material(
                               response, false),
                    Error::none);
                REQUIRE_EQ(sender.acknowledge_key_material(
                               response, false),
                    Error::none);
                if (rotation == 2U) {
                    REQUIRE_EQ(receiver.accept_key_material(
                                   first_request.view(), false),
                        Error::none);
                    REQUIRE(std::equal(
                        receiver.key_material_response().begin(),
                        receiver.key_material_response().end(),
                        first_request.view().begin(),
                        first_request.view().end()));
                    REQUIRE_EQ(sender.acknowledge_key_material(
                                   receiver.key_material_response(),
                                   false),
                        Error::none);
                }
            }

            REQUIRE(sender.ready_to_send_data());
            EncryptionKey packet_key = EncryptionKey::none;
            REQUIRE_EQ(sender.encrypt(
                           sequence, clear, encrypted, packet_key),
                Error::none);
            REQUIRE_EQ(packet_key, key_before_rotation);
            REQUIRE_EQ(receiver.decrypt(
                           packet_key, sequence,
                           encrypted, decrypted),
                Error::none);
            REQUIRE_EQ(decrypted, clear);
            REQUIRE_EQ(sender.note_data_packet_sent(), Error::none);
            sequence = sequence.next();
        }

        const EncryptionKey key_after_rotation =
            key_before_rotation == EncryptionKey::even
            ? EncryptionKey::odd : EncryptionKey::even;
        REQUIRE_EQ(sender.active_sender_key(),
            key_after_rotation);
        REQUIRE_EQ(sender.packets_on_active_key(), 0U);
    }

    REQUIRE_EQ(sequence, SequenceNumber{17});
    REQUIRE_EQ(sender.sender_state(), CryptoState::secured);
    REQUIRE_EQ(receiver.receiver_state(), CryptoState::secured);
}

TEST(crypto_session_ignores_a_replayed_request_for_a_retired_key)
{
    for (const CryptoMode mode : {CryptoMode::aes_ctr, CryptoMode::aes_gcm}) {
        const CryptoConfiguration configuration {
            .passphrase = "retired key replay",
            .mode = mode,
            .enable_aes_gcm = true,
            .key_length = 16,
            .refresh_rate_packets = 3,
            .preannouncement_packets = 1,
        };
        CryptoSession sender {configuration};
        CryptoSession receiver {configuration};
        REQUIRE_EQ(sender.start_initiator(), Error::none);
        REQUIRE_EQ(
            receiver.accept_key_material(sender.pending_key_material(), false),
            Error::none);
        REQUIRE_EQ(sender.acknowledge_key_material(
                       receiver.key_material_response(), false),
            Error::none);

        const std::array<std::byte, 16> clear {std::byte {0x5a}};
        // Drive enough rotations that the earliest rotation request has aged
        // out of the receiver's raw message history (4 entries), while its key
        // is still one of the retired generations.
        std::vector<std::byte> first_rotation_request;
        std::size_t rotations = 0;
        SequenceNumber sequence {100};
        while (rotations < 7) {
            REQUIRE_EQ(sender.prepare_rotation(), Error::none);
            const auto request = sender.pending_key_material();
            if (!request.empty()) {
                if (first_rotation_request.empty()) {
                    first_rotation_request.assign(
                        request.begin(), request.end());
                }
                REQUIRE_EQ(
                    receiver.accept_key_material(request, false), Error::none);
                REQUIRE_EQ(sender.acknowledge_key_material(
                               receiver.key_material_response(), false),
                    Error::none);
                ++rotations;
            }
            std::array<std::byte, 16> ciphertext {};
            std::array<std::byte, 16> plaintext {};
            EncryptionKey key = EncryptionKey::none;
            const DataHeader header {
                .sequence = sequence,
                .message_number = sequence.value(),
                .boundary = MessageBoundary::solo,
                .in_order = true,
                .encryption_key = sender.active_sender_key(),
                .timestamp = PacketTimestamp {sequence.value()},
                .destination_socket_id = 0x1234'5678U,
            };
            std::array<std::byte, srt_gcm_authentication_tag_size> tag {};
            if (mode == CryptoMode::aes_gcm) {
                REQUIRE_EQ(sender.seal(header, clear, ciphertext, tag, key),
                    Error::none);
            } else {
                REQUIRE_EQ(sender.encrypt(sequence, clear, ciphertext, key),
                    Error::none);
            }
            REQUIRE_EQ(sender.note_data_packet_sent(), Error::none);
            if (mode == CryptoMode::aes_gcm) {
                REQUIRE_EQ(receiver.open(header, ciphertext, tag, plaintext),
                    Error::none);
            } else {
                REQUIRE_EQ(
                    receiver.decrypt(key, sequence, ciphertext, plaintext),
                    Error::none);
            }
            receiver.note_accepted_receive_sequence(sequence);
            sequence = sequence.next();
        }
        REQUIRE(!first_rotation_request.empty());

        // The replay is refused without touching the secured session ...
        REQUIRE_EQ(receiver.accept_key_material(first_rotation_request, false),
            Error::invalid_key_material);
        REQUIRE_EQ(receiver.receiver_state(), CryptoState::secured);

        // ... and the peer's current key keeps working.
        std::array<std::byte, 16> ciphertext {};
        std::array<std::byte, 16> plaintext {};
        EncryptionKey key = EncryptionKey::none;
        const DataHeader header {
            .sequence = sequence,
            .message_number = sequence.value(),
            .boundary = MessageBoundary::solo,
            .in_order = true,
            .encryption_key = sender.active_sender_key(),
            .timestamp = PacketTimestamp {sequence.value()},
            .destination_socket_id = 0x1234'5678U,
        };
        if (mode == CryptoMode::aes_gcm) {
            std::array<std::byte, srt_gcm_authentication_tag_size> tag {};
            REQUIRE_EQ(
                sender.seal(header, clear, ciphertext, tag, key), Error::none);
            REQUIRE_EQ(
                receiver.open(header, ciphertext, tag, plaintext), Error::none);
        } else {
            REQUIRE_EQ(
                sender.encrypt(sequence, clear, ciphertext, key), Error::none);
            REQUIRE_EQ(receiver.decrypt(key, sequence, ciphertext, plaintext),
                Error::none);
        }
        REQUIRE_EQ(plaintext, clear);
    }
}

TEST(crypto_session_recovers_rotation_after_foreign_key_material)
{
    for (const CryptoMode mode : {CryptoMode::aes_ctr, CryptoMode::aes_gcm}) {
        for (int prior_rotations = 0; prior_rotations != 4; ++prior_rotations) {
            const CryptoConfiguration configuration {
                .passphrase = "review rotation fixture",
                .mode = mode,
                .enable_aes_gcm = true,
                .refresh_rate_packets = 5,
                .preannouncement_packets = 1,
            };
            CryptoSession sender {configuration};
            CryptoSession receiver {configuration};
            CryptoSession foreign {configuration};
            REQUIRE_EQ(sender.start_initiator(), Error::none);
            REQUIRE_EQ(receiver.accept_key_material(
                           sender.pending_key_material(), false),
                Error::none);
            REQUIRE_EQ(sender.acknowledge_key_material(
                           receiver.key_material_response(), false),
                Error::none);
            std::uint32_t next_sequence = 0;
            DataHeader last_header {};
            std::array<std::byte, 1> last_ciphertext {};
            std::array<std::byte, srt_gcm_authentication_tag_size> last_tag {};
            const auto send_packet = [&] {
                const SequenceNumber sequence {next_sequence};
                const std::array<std::byte, 1> clear {std::byte {42}};
                std::array<std::byte, 1> encrypted {}, decrypted {};
                EncryptionKey key = EncryptionKey::none;
                const DataHeader header {
                    .sequence = sequence,
                    .message_number = next_sequence + 1U,
                    .boundary = MessageBoundary::solo,
                    .in_order = true,
                    .encryption_key = sender.active_sender_key(),
                    .timestamp = PacketTimestamp {next_sequence},
                    .destination_socket_id = 0x1234'5678U,
                };
                if (mode == CryptoMode::aes_gcm) {
                    std::array<std::byte, srt_gcm_authentication_tag_size>
                        tag {};
                    REQUIRE_EQ(sender.seal(header, clear, encrypted, tag, key),
                        Error::none);
                    last_tag = tag;
                    REQUIRE_EQ(receiver.open(header, encrypted, tag, decrypted),
                        Error::none);
                } else {
                    REQUIRE_EQ(sender.encrypt(sequence, clear, encrypted, key),
                        Error::none);
                    REQUIRE_EQ(
                        receiver.decrypt(key, sequence, encrypted, decrypted),
                        Error::none);
                }
                REQUIRE_EQ(decrypted, clear);
                last_header = header;
                last_ciphertext = encrypted;
                receiver.note_accepted_receive_sequence(sequence);
                REQUIRE_EQ(sender.note_data_packet_sent(), Error::none);
                ++next_sequence;
            };
            for (int rotation = 0; rotation < prior_rotations % 2; ++rotation) {
                for (int packet = 0; packet < 4; ++packet) {
                    send_packet();
                }
                REQUIRE_EQ(sender.prepare_rotation(), Error::none);
                REQUIRE_EQ(receiver.accept_key_material(
                               sender.pending_key_material(), false),
                    Error::none);
                REQUIRE_EQ(sender.acknowledge_key_material(
                               receiver.key_material_response(), false),
                    Error::none);
                send_packet();
            }
            for (int packet = 0; packet < 4; ++packet) {
                send_packet();
            }
            REQUIRE_EQ(foreign.start_initiator(), Error::none);
            if (prior_rotations >= 2) {
                REQUIRE_EQ(foreign.acknowledge_key_material(
                               foreign.pending_key_material(), false),
                    Error::none);
                for (int packet = 0; packet < 4; ++packet) {
                    REQUIRE_EQ(foreign.note_data_packet_sent(), Error::none);
                }
                REQUIRE_EQ(foreign.prepare_rotation(), Error::none);
            }
            REQUIRE_EQ(receiver.accept_key_material(
                           foreign.pending_key_material(), false),
                Error::none);
            REQUIRE_EQ(sender.prepare_rotation(), Error::none);
            const auto legitimate_request = sender.pending_key_material();
            REQUIRE(!legitimate_request.empty());
            REQUIRE_EQ(receiver.accept_key_material(legitimate_request, false),
                Error::none);
            REQUIRE_EQ(sender.acknowledge_key_material(
                           receiver.key_material_response(), false),
                Error::none);
            // Retransmission keeps the original ciphertext and selector.
            std::array<std::byte, 1> retransmitted {};
            if (mode == CryptoMode::aes_gcm) {
                REQUIRE_EQ(receiver.open(last_header, last_ciphertext, last_tag,
                               retransmitted),
                    Error::none);
            } else {
                REQUIRE_EQ(
                    receiver.decrypt(last_header.encryption_key,
                        last_header.sequence, last_ciphertext, retransmitted),
                    Error::none);
            }
            REQUIRE_EQ(retransmitted[0], std::byte {42});
            // Both announced selectors must work immediately, including the
            // still-active key before the sender changes to its successor.
            send_packet();
            send_packet();
        }
    }
}

TEST(crypto_session_preserves_receive_keys_when_material_changes_key_identity)
{
    for (const CryptoMode mode : {CryptoMode::aes_ctr, CryptoMode::aes_gcm}) {
        const CryptoConfiguration configuration {
            .passphrase = "key identity contract",
            .mode = mode,
            .enable_aes_gcm = true,
        };
        CryptoSession sender {configuration};
        CryptoSession receiver {configuration};
        REQUIRE_EQ(sender.start_initiator(), Error::none);
        const auto original = sender.pending_key_material();
        REQUIRE_EQ(receiver.accept_key_material(original, false), Error::none);
        REQUIRE_EQ(sender.acknowledge_key_material(
                       receiver.key_material_response(), false),
            Error::none);
        for (std::size_t changed = 0; changed < 9U; ++changed) {
            std::vector<std::byte> material(original.begin(), original.end());
            if (changed < 8U) {
                material[key_material_header_size + changed] ^= std::byte {1};
            } else {
                material[3] = static_cast<std::byte>(EncryptionKey::odd);
            }
            REQUIRE_EQ(receiver.accept_key_material(material, false),
                Error::invalid_key_material);
            REQUIRE_EQ(receiver.receiver_state(), CryptoState::secured);
        }
        const std::array<std::byte, 1> clear {std::byte {42}};
        std::array<std::byte, 1> encrypted {}, decrypted {};
        EncryptionKey key = EncryptionKey::none;
        const DataHeader header {
            .sequence = SequenceNumber {100},
            .message_number = 1,
            .boundary = MessageBoundary::solo,
            .in_order = true,
            .encryption_key = sender.active_sender_key(),
            .timestamp = PacketTimestamp {100},
            .destination_socket_id = 123,
        };
        if (mode == CryptoMode::aes_gcm) {
            std::array<std::byte, srt_gcm_authentication_tag_size> tag {};
            REQUIRE_EQ(
                sender.seal(header, clear, encrypted, tag, key), Error::none);
            REQUIRE_EQ(
                receiver.open(header, encrypted, tag, decrypted), Error::none);
        } else {
            REQUIRE_EQ(sender.encrypt(header.sequence, clear, encrypted, key),
                Error::none);
            REQUIRE_EQ(
                receiver.decrypt(key, header.sequence, encrypted, decrypted),
                Error::none);
        }
        REQUIRE_EQ(decrypted, clear);
    }
}
