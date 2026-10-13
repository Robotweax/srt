// SPDX-License-Identifier: MIT
#include "robotweax/srt/crypto_provider.hpp"
#include "crypto_mbedtls_config.hpp"

#include <mbedtls/aes.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/gcm.h>
#include <mbedtls/md.h>
#include <mbedtls/nist_kw.h>
#include <mbedtls/pkcs5.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/version.h>

#include <algorithm>
#include <array>
#include <climits>
#include <limits>
#include <mutex>
#include <new>

namespace robotweax::srt {
namespace {

bool valid_key(std::span<const std::byte> key) noexcept
{
    return key.size() == 16 || key.size() == 24 || key.size() == 32;
}

void erase(std::span<std::byte> output) noexcept
{
    if (!output.empty())
        mbedtls_platform_zeroize(output.data(), output.size());
}

template <std::size_t Size>
const unsigned char* data(std::span<const std::byte, Size> bytes) noexcept
{
    return reinterpret_cast<const unsigned char*>(bytes.data());
}

template <std::size_t Size>
unsigned char* data(std::span<std::byte, Size> bytes) noexcept
{
    return reinterpret_cast<unsigned char*>(bytes.data());
}

class MbedTlsPayloadCipher final : public PayloadCipher {
public:
    explicit MbedTlsPayloadCipher(std::span<const std::byte> key) noexcept
    {
        mbedtls_aes_init(&context_);
        valid_ = mbedtls_aes_setkey_enc(&context_, data(key),
                     static_cast<unsigned>(key.size() * 8))
            == 0;
    }
    ~MbedTlsPayloadCipher() override
    {
        mbedtls_aes_free(&context_);
    }

    bool valid() const noexcept
    {
        return valid_;
    }

    Error transform(std::span<const std::byte, aes_block_size> iv,
        std::span<const std::byte> input,
        std::span<std::byte> output) noexcept override
    {
        if (output.size() < input.size())
            return Error::buffer_too_small;
        if (!valid_ || input.size() > static_cast<std::size_t>(INT_MAX))
            return Error::cryptographic_failure;
        if (input.empty())
            return Error::none;
        // Each packet starts from its supplied IV, retaining only the AES key.
        std::array<unsigned char, aes_block_size> counter {};
        std::array<unsigned char, aes_block_size> stream {};
        std::copy_n(data(iv), counter.size(), counter.begin());
        std::size_t offset = 0;
        const int status = mbedtls_aes_crypt_ctr(&context_, input.size(),
            &offset, counter.data(), stream.data(), data(input), data(output));
        mbedtls_platform_zeroize(counter.data(), counter.size());
        mbedtls_platform_zeroize(stream.data(), stream.size());
        if (status != 0) {
            erase(output);
            return Error::cryptographic_failure;
        }
        return Error::none;
    }

private:
    mbedtls_aes_context context_ {};
    bool valid_ = false;
};

class MbedTlsAuthenticatedCipher final : public AuthenticatedPayloadCipher {
public:
    explicit MbedTlsAuthenticatedCipher(std::span<const std::byte> key) noexcept
    {
        mbedtls_gcm_init(&context_);
        valid_ = mbedtls_gcm_setkey(&context_, MBEDTLS_CIPHER_ID_AES, data(key),
                     static_cast<unsigned>(key.size() * 8))
            == 0;
    }
    ~MbedTlsAuthenticatedCipher() override
    {
        mbedtls_gcm_free(&context_);
    }

    bool valid() const noexcept
    {
        return valid_;
    }

    Error seal(std::span<const std::byte> iv, std::span<const std::byte> aad,
        std::span<const std::byte> plaintext, std::span<std::byte> ciphertext,
        std::span<std::byte> tag) noexcept override
    {
        if (ciphertext.size() < plaintext.size() || tag.size() < 16)
            return Error::buffer_too_small;
        if (iv.size() != 12 || tag.size() != 16)
            return Error::invalid_key_material;
        if (!valid_ || plaintext.size() > static_cast<std::size_t>(INT_MAX)
            || aad.size() > static_cast<std::size_t>(INT_MAX))
            return Error::cryptographic_failure;
        unsigned char empty = 0;
        const int status = mbedtls_gcm_crypt_and_tag(&context_,
            MBEDTLS_GCM_ENCRYPT, plaintext.size(), data(iv), iv.size(),
            aad.empty() ? &empty : data(aad), aad.size(),
            plaintext.empty() ? &empty : data(plaintext),
            ciphertext.empty() ? &empty : data(ciphertext), tag.size(),
            data(tag));
        if (status != 0) {
            erase(ciphertext);
            erase(tag);
            return Error::cryptographic_failure;
        }
        return Error::none;
    }

    Error open(std::span<const std::byte> iv, std::span<const std::byte> aad,
        std::span<const std::byte> ciphertext, std::span<const std::byte> tag,
        std::span<std::byte> plaintext) noexcept override
    {
        if (plaintext.size() < ciphertext.size())
            return Error::buffer_too_small;
        const auto reject = [&](Error error) noexcept {
            erase(plaintext);
            return error;
        };
        if (tag.size() != 16)
            return reject(Error::authentication_failure);
        if (iv.size() != 12)
            return reject(Error::invalid_key_material);
        if (!valid_ || ciphertext.size() > static_cast<std::size_t>(INT_MAX)
            || aad.size() > static_cast<std::size_t>(INT_MAX))
            return reject(Error::cryptographic_failure);
        unsigned char empty = 0;
        const int status = mbedtls_gcm_auth_decrypt(&context_,
            ciphertext.size(), data(iv), iv.size(),
            aad.empty() ? &empty : data(aad), aad.size(), data(tag), tag.size(),
            ciphertext.empty() ? &empty : data(ciphertext),
            plaintext.empty() ? &empty : data(plaintext));
        if (status != 0)
            return reject(status == MBEDTLS_ERR_GCM_AUTH_FAILED
                    ? Error::authentication_failure
                    : Error::cryptographic_failure);
        return Error::none;
    }

private:
    mbedtls_gcm_context context_ {};
    bool valid_ = false;
};

class KeyWrapContext {
public:
    KeyWrapContext() noexcept
    {
        mbedtls_nist_kw_init(&value);
    }
    ~KeyWrapContext()
    {
        mbedtls_nist_kw_free(&value);
    }
    mbedtls_nist_kw_context value {};
};

class MbedTlsCryptoProvider final : public CryptoProvider {
public:
    MbedTlsCryptoProvider() noexcept
    {
        mbedtls_entropy_init(&entropy_);
        mbedtls_ctr_drbg_init(&random_);
    }
    ~MbedTlsCryptoProvider() override
    {
        mbedtls_ctr_drbg_free(&random_);
        mbedtls_entropy_free(&entropy_);
    }

    Error random_bytes(std::span<std::byte> destination) noexcept override
    {
        if (destination.empty())
            return Error::none;
        const auto original_destination = destination;
        std::lock_guard lock(random_mutex_);
        if (!seeded_) {
            constexpr unsigned char personalization[] = "Robotweax SRT crypto";
            if (mbedtls_ctr_drbg_seed(&random_, mbedtls_entropy_func, &entropy_,
                    personalization, sizeof(personalization) - 1)
                != 0) {
                mbedtls_ctr_drbg_free(&random_);
                mbedtls_ctr_drbg_init(&random_);
                erase(destination);
                return Error::cryptographic_failure;
            }
            seeded_ = true;
        }
        while (!destination.empty()) {
            const auto length = std::min<std::size_t>(
                destination.size(), MBEDTLS_CTR_DRBG_MAX_REQUEST);
            if (mbedtls_ctr_drbg_random(&random_, data(destination), length)
                != 0) {
                // Includes bytes written before a later chunk failed.
                erase(original_destination);
                return Error::cryptographic_failure;
            }
            destination = destination.subspan(length);
        }
        return Error::none;
    }

    Error pbkdf2_hmac_sha1(std::span<const std::byte> passphrase,
        std::span<const std::byte> salt, std::uint32_t iterations,
        std::span<std::byte> output) noexcept override
    {
        if (iterations == 0 || iterations > static_cast<std::uint32_t>(INT_MAX)
            || output.size() > static_cast<std::size_t>(INT_MAX)
            || passphrase.size() > static_cast<std::size_t>(INT_MAX)
            || salt.size() > static_cast<std::size_t>(INT_MAX))
            return Error::cryptographic_failure;
        if (mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA1, data(passphrase),
                passphrase.size(), data(salt), salt.size(), iterations,
                static_cast<std::uint32_t>(output.size()), data(output))
            != 0) {
            erase(output);
            return Error::cryptographic_failure;
        }
        return Error::none;
    }

    Error hmac_sha256(std::span<const std::byte> key,
        std::span<const std::byte> input,
        std::span<std::byte, 32> output) noexcept override
    {
        const auto* digest = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
        if (digest != nullptr
            && mbedtls_md_hmac(digest, data(key), key.size(), data(input),
                   input.size(), data(output))
                == 0)
            return Error::none;
        erase(output);
        return Error::cryptographic_failure;
    }

    Error wrap_key(std::span<const std::byte> key,
        std::span<const std::byte> input, std::span<std::byte> output,
        std::size_t& written) noexcept override
    {
        written = 0;
        if (!valid_key(key) || input.size() < 16 || input.size() % 8 != 0
            || input.size() > static_cast<std::size_t>(INT_MAX)
            || output.size() < input.size() + 8)
            return Error::invalid_key_material;
        KeyWrapContext context;
        if (mbedtls_nist_kw_setkey(&context.value, MBEDTLS_CIPHER_ID_AES,
                data(key), static_cast<unsigned>(key.size() * 8), 1)
                != 0
            || mbedtls_nist_kw_wrap(&context.value, MBEDTLS_KW_MODE_KW,
                   data(input), input.size(), data(output), &written,
                   output.size())
                != 0
            || written != input.size() + 8) {
            written = 0;
            erase(output);
            return Error::cryptographic_failure;
        }
        return Error::none;
    }

    Error unwrap_key(std::span<const std::byte> key,
        std::span<const std::byte> input, std::span<std::byte> output,
        std::size_t& written) noexcept override
    {
        written = 0;
        if (!valid_key(key) || input.size() < 24 || input.size() % 8 != 0
            || input.size() > static_cast<std::size_t>(INT_MAX)
            || output.size() < input.size() - 8)
            return Error::invalid_key_material;
        KeyWrapContext context;
        if (mbedtls_nist_kw_setkey(&context.value, MBEDTLS_CIPHER_ID_AES,
                data(key), static_cast<unsigned>(key.size() * 8), 0)
                != 0
            || mbedtls_nist_kw_unwrap(&context.value, MBEDTLS_KW_MODE_KW,
                   data(input), input.size(), data(output), &written,
                   output.size())
                != 0
            || written != input.size() - 8) {
            written = 0;
            erase(output);
            return Error::cryptographic_failure;
        }
        return Error::none;
    }

    Error make_aes_ctr_cipher(std::span<const std::byte> key,
        std::unique_ptr<PayloadCipher>& cipher) noexcept override
    {
        cipher.reset();
        if (!valid_key(key))
            return Error::invalid_key_material;
        auto prepared = std::unique_ptr<MbedTlsPayloadCipher> {
            new (std::nothrow) MbedTlsPayloadCipher(key)};
        if (!prepared || !prepared->valid())
            return Error::cryptographic_failure;
        cipher = std::move(prepared);
        return Error::none;
    }

    bool supports_aes_gcm() const noexcept override
    {
        return true;
    }

    Error make_aes_gcm_cipher(std::span<const std::byte> key,
        std::unique_ptr<AuthenticatedPayloadCipher>& cipher) noexcept override
    {
        cipher.reset();
        if (!valid_key(key))
            return Error::invalid_key_material;
        auto prepared = std::unique_ptr<MbedTlsAuthenticatedCipher> {
            new (std::nothrow) MbedTlsAuthenticatedCipher(key)};
        if (!prepared || !prepared->valid())
            return Error::cryptographic_failure;
        cipher = std::move(prepared);
        return Error::none;
    }

    void secure_erase(std::span<std::byte> bytes) noexcept override
    {
        erase(bytes);
    }

private:
    std::mutex random_mutex_;
    mbedtls_entropy_context entropy_ {};
    mbedtls_ctr_drbg_context random_ {};
    bool seeded_ = false;
};

} // namespace

CryptoProvider& default_crypto_provider() noexcept
{
    static MbedTlsCryptoProvider provider;
    return provider;
}

} // namespace robotweax::srt
