#include "robotweax/srt/crypto_provider.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <array>
#include <climits>
#include <cstddef>
#include <memory>
#include <new>

namespace robotweax::srt {
namespace {

[[nodiscard]] const EVP_CIPHER* ctr_cipher(
    std::size_t key_size) noexcept
{
    switch (key_size) {
    case 16U: return EVP_aes_128_ctr();
    case 24U: return EVP_aes_192_ctr();
    case 32U: return EVP_aes_256_ctr();
    default: return nullptr;
    }
}

[[nodiscard]] const EVP_CIPHER* gcm_cipher(std::size_t key_size) noexcept
{
    switch (key_size) {
    case 16U:
        return EVP_aes_128_gcm();
    case 24U:
        return EVP_aes_192_gcm();
    case 32U:
        return EVP_aes_256_gcm();
    default:
        return nullptr;
    }
}

[[nodiscard]] const EVP_CIPHER* wrap_cipher(
    std::size_t key_size) noexcept
{
    switch (key_size) {
    case 16U: return EVP_aes_128_wrap();
    case 24U: return EVP_aes_192_wrap();
    case 32U: return EVP_aes_256_wrap();
    default: return nullptr;
    }
}

class EvpCipherContext {
public:
    EvpCipherContext() noexcept
        : value_(EVP_CIPHER_CTX_new())
    {
    }

    ~EvpCipherContext()
    {
        EVP_CIPHER_CTX_free(value_);
    }

    EvpCipherContext(const EvpCipherContext&) = delete;
    EvpCipherContext& operator=(const EvpCipherContext&) = delete;

    [[nodiscard]] EVP_CIPHER_CTX* get() const noexcept
    {
        return value_;
    }

private:
    EVP_CIPHER_CTX* value_ = nullptr;
};

class OpenSslPayloadCipher final : public PayloadCipher {
public:
    OpenSslPayloadCipher(
        const EVP_CIPHER* cipher,
        std::span<const std::byte> key) noexcept
    {
        valid_ = context_.get() != nullptr
            && EVP_EncryptInit_ex(context_.get(), cipher, nullptr,
                   reinterpret_cast<const unsigned char*>(key.data()),
                   nullptr)
                == 1
            && EVP_CIPHER_CTX_set_padding(context_.get(), 0) == 1;
    }

    [[nodiscard]] bool valid() const noexcept
    {
        return valid_;
    }

    Error transform(
        std::span<const std::byte, aes_block_size>
            initialization_vector,
        std::span<const std::byte> input,
        std::span<std::byte> output) noexcept override
    {
        if (!valid_ || output.size() < input.size()
            || input.size()
                > static_cast<std::size_t>(INT_MAX)) {
            return output.size() < input.size()
                ? Error::buffer_too_small
                : Error::cryptographic_failure;
        }
        if (EVP_EncryptInit_ex(context_.get(), nullptr, nullptr,
                nullptr,
                reinterpret_cast<const unsigned char*>(
                    initialization_vector.data()))
            != 1) {
            return Error::cryptographic_failure;
        }
        int written = 0;
        if (EVP_EncryptUpdate(context_.get(),
                reinterpret_cast<unsigned char*>(output.data()),
                &written,
                reinterpret_cast<const unsigned char*>(input.data()),
                static_cast<int>(input.size()))
            != 1
            || written != static_cast<int>(input.size())) {
            return Error::cryptographic_failure;
        }
        int final_written = 0;
        if (EVP_EncryptFinal_ex(context_.get(),
                reinterpret_cast<unsigned char*>(output.data())
                    + written,
                &final_written)
                != 1
            || final_written != 0) {
            return Error::cryptographic_failure;
        }
        return Error::none;
    }

private:
    EvpCipherContext context_;
    bool valid_ = false;
};

class OpenSslAuthenticatedPayloadCipher final
    : public AuthenticatedPayloadCipher {
public:
    static constexpr std::size_t initialization_vector_size = 12U;
    static constexpr std::size_t authentication_tag_size = 16U;

    OpenSslAuthenticatedPayloadCipher(
        const EVP_CIPHER* cipher, std::span<const std::byte> key) noexcept
    {
        valid_ = encryption_context_.get() != nullptr
            && decryption_context_.get() != nullptr
            && EVP_EncryptInit_ex(encryption_context_.get(), cipher, nullptr,
                   reinterpret_cast<const unsigned char*>(key.data()), nullptr)
                == 1
            && EVP_CIPHER_CTX_ctrl(encryption_context_.get(),
                   EVP_CTRL_GCM_SET_IVLEN,
                   static_cast<int>(initialization_vector_size), nullptr)
                == 1
            && EVP_DecryptInit_ex(decryption_context_.get(), cipher, nullptr,
                   reinterpret_cast<const unsigned char*>(key.data()), nullptr)
                == 1
            && EVP_CIPHER_CTX_ctrl(decryption_context_.get(),
                   EVP_CTRL_GCM_SET_IVLEN,
                   static_cast<int>(initialization_vector_size), nullptr)
                == 1;
    }

    [[nodiscard]] bool valid() const noexcept
    {
        return valid_;
    }

    Error seal(std::span<const std::byte> initialization_vector,
        std::span<const std::byte> additional_authenticated_data,
        std::span<const std::byte> plaintext, std::span<std::byte> ciphertext,
        std::span<std::byte> authentication_tag) noexcept override
    {
        if (ciphertext.size() < plaintext.size()
            || authentication_tag.size() < authentication_tag_size) {
            return Error::buffer_too_small;
        }
        if (!valid_) {
            return Error::cryptographic_failure;
        }
        if (initialization_vector.size() != initialization_vector_size
            || authentication_tag.size() != authentication_tag_size) {
            return Error::invalid_key_material;
        }
        if (!valid_input_sizes(plaintext, additional_authenticated_data)) {
            return Error::cryptographic_failure;
        }

        const auto fail = [&]() noexcept {
            erase(ciphertext);
            erase(authentication_tag);
            return Error::cryptographic_failure;
        };
        if (EVP_EncryptInit_ex(encryption_context_.get(), nullptr, nullptr,
                nullptr,
                reinterpret_cast<const unsigned char*>(
                    initialization_vector.data()))
            != 1) {
            return fail();
        }
        if (!update_additional_data(encryption_context_.get(),
                additional_authenticated_data, true)) {
            return fail();
        }

        int written = 0;
        if (!plaintext.empty()
            && (EVP_EncryptUpdate(encryption_context_.get(),
                    reinterpret_cast<unsigned char*>(ciphertext.data()),
                    &written,
                    reinterpret_cast<const unsigned char*>(plaintext.data()),
                    static_cast<int>(plaintext.size()))
                    != 1
                || written != static_cast<int>(plaintext.size()))) {
            return fail();
        }
        std::array<unsigned char, EVP_MAX_BLOCK_LENGTH> final_output {};
        int final_written = 0;
        if (EVP_EncryptFinal_ex(
                encryption_context_.get(), final_output.data(), &final_written)
                != 1
            || final_written != 0
            || EVP_CIPHER_CTX_ctrl(encryption_context_.get(),
                   EVP_CTRL_GCM_GET_TAG,
                   static_cast<int>(authentication_tag_size),
                   authentication_tag.data())
                != 1) {
            return fail();
        }
        return Error::none;
    }

    Error open(std::span<const std::byte> initialization_vector,
        std::span<const std::byte> additional_authenticated_data,
        std::span<const std::byte> ciphertext,
        std::span<const std::byte> authentication_tag,
        std::span<std::byte> plaintext) noexcept override
    {
        if (plaintext.size() < ciphertext.size()) {
            return Error::buffer_too_small;
        }
        const auto reject = [&](Error error) noexcept {
            erase(plaintext);
            return error;
        };
        if (authentication_tag.size() != authentication_tag_size) {
            return reject(Error::authentication_failure);
        }
        if (!valid_) {
            return reject(Error::cryptographic_failure);
        }
        if (initialization_vector.size() != initialization_vector_size) {
            return reject(Error::invalid_key_material);
        }
        if (!valid_input_sizes(ciphertext, additional_authenticated_data)) {
            return reject(Error::cryptographic_failure);
        }
        if (EVP_DecryptInit_ex(decryption_context_.get(), nullptr, nullptr,
                nullptr,
                reinterpret_cast<const unsigned char*>(
                    initialization_vector.data()))
                != 1
            || !update_additional_data(decryption_context_.get(),
                additional_authenticated_data, false)) {
            return reject(Error::cryptographic_failure);
        }

        int written = 0;
        if (!ciphertext.empty()
            && (EVP_DecryptUpdate(decryption_context_.get(),
                    reinterpret_cast<unsigned char*>(plaintext.data()),
                    &written,
                    reinterpret_cast<const unsigned char*>(ciphertext.data()),
                    static_cast<int>(ciphertext.size()))
                    != 1
                || written != static_cast<int>(ciphertext.size()))) {
            return reject(Error::cryptographic_failure);
        }
        if (EVP_CIPHER_CTX_ctrl(decryption_context_.get(), EVP_CTRL_GCM_SET_TAG,
                static_cast<int>(authentication_tag_size),
                const_cast<std::byte*>(authentication_tag.data()))
            != 1) {
            return reject(Error::cryptographic_failure);
        }
        std::array<unsigned char, EVP_MAX_BLOCK_LENGTH> final_output {};
        int final_written = 0;
        if (EVP_DecryptFinal_ex(
                decryption_context_.get(), final_output.data(), &final_written)
            != 1) {
            return reject(Error::authentication_failure);
        }
        if (final_written != 0) {
            return reject(Error::cryptographic_failure);
        }
        return Error::none;
    }

private:
    [[nodiscard]] static bool valid_input_sizes(
        std::span<const std::byte> input,
        std::span<const std::byte> additional_authenticated_data) noexcept
    {
        return input.size() <= static_cast<std::size_t>(INT_MAX)
            && additional_authenticated_data.size()
            <= static_cast<std::size_t>(INT_MAX);
    }

    [[nodiscard]] static bool update_additional_data(EVP_CIPHER_CTX* context,
        std::span<const std::byte> additional_authenticated_data,
        bool encrypt) noexcept
    {
        if (additional_authenticated_data.empty()) {
            return true;
        }
        int written = 0;
        const auto* data = reinterpret_cast<const unsigned char*>(
            additional_authenticated_data.data());
        const int size = static_cast<int>(additional_authenticated_data.size());
        return encrypt
            ? EVP_EncryptUpdate(context, nullptr, &written, data, size) == 1
            : EVP_DecryptUpdate(context, nullptr, &written, data, size) == 1;
    }

    static void erase(std::span<std::byte> bytes) noexcept
    {
        if (!bytes.empty()) {
            OPENSSL_cleanse(bytes.data(), bytes.size());
        }
    }

    EvpCipherContext encryption_context_;
    EvpCipherContext decryption_context_;
    bool valid_ = false;
};

class OpenSslCryptoProvider final : public CryptoProvider {
public:
    Error random_bytes(
        std::span<std::byte> destination) noexcept override
    {
        if (destination.size()
            > static_cast<std::size_t>(INT_MAX)) {
            return Error::cryptographic_failure;
        }
        return RAND_bytes(
                   reinterpret_cast<unsigned char*>(
                       destination.data()),
                   static_cast<int>(destination.size()))
                == 1
            ? Error::none : Error::cryptographic_failure;
    }

    Error pbkdf2_hmac_sha1(
        std::span<const std::byte> passphrase,
        std::span<const std::byte> salt,
        std::uint32_t iterations,
        std::span<std::byte> derived_key) noexcept override
    {
        if (passphrase.size() > static_cast<std::size_t>(INT_MAX)
            || salt.size() > static_cast<std::size_t>(INT_MAX)
            || derived_key.size()
                > static_cast<std::size_t>(INT_MAX)
            || iterations
                > static_cast<std::uint32_t>(INT_MAX)) {
            return Error::cryptographic_failure;
        }
        return PKCS5_PBKDF2_HMAC(
                   reinterpret_cast<const char*>(passphrase.data()),
                   static_cast<int>(passphrase.size()),
                   reinterpret_cast<const unsigned char*>(salt.data()),
                   static_cast<int>(salt.size()),
                   static_cast<int>(iterations), EVP_sha1(),
                   static_cast<int>(derived_key.size()),
                   reinterpret_cast<unsigned char*>(
                       derived_key.data()))
                == 1
            ? Error::none : Error::cryptographic_failure;
    }

    Error wrap_key(
        std::span<const std::byte> key_encrypting_key,
        std::span<const std::byte> plaintext_keys,
        std::span<std::byte> destination,
        std::size_t& bytes_written) noexcept override
    {
        bytes_written = 0;
        const EVP_CIPHER* cipher =
            wrap_cipher(key_encrypting_key.size());
        if (cipher == nullptr
            || plaintext_keys.empty()
            || (plaintext_keys.size() % 8U) != 0U
            || plaintext_keys.size()
                > static_cast<std::size_t>(INT_MAX)
            || destination.size() < plaintext_keys.size() + 8U) {
            return Error::invalid_key_material;
        }
        EvpCipherContext context;
        if (context.get() == nullptr) {
            return Error::cryptographic_failure;
        }
        EVP_CIPHER_CTX_set_flags(
            context.get(), EVP_CIPHER_CTX_FLAG_WRAP_ALLOW);
        if (EVP_EncryptInit_ex(context.get(), cipher, nullptr,
                reinterpret_cast<const unsigned char*>(
                    key_encrypting_key.data()),
                nullptr)
            != 1) {
            return Error::cryptographic_failure;
        }
        int written = 0;
        if (EVP_EncryptUpdate(context.get(),
                reinterpret_cast<unsigned char*>(
                    destination.data()),
                &written,
                reinterpret_cast<const unsigned char*>(
                    plaintext_keys.data()),
                static_cast<int>(plaintext_keys.size()))
            != 1) {
            return Error::cryptographic_failure;
        }
        int final_written = 0;
        if (EVP_EncryptFinal_ex(context.get(),
                reinterpret_cast<unsigned char*>(destination.data())
                    + written,
                &final_written)
                != 1) {
            return Error::cryptographic_failure;
        }
        bytes_written = static_cast<std::size_t>(
            written + final_written);
        return bytes_written == plaintext_keys.size() + 8U
            ? Error::none : Error::cryptographic_failure;
    }

    Error unwrap_key(
        std::span<const std::byte> key_encrypting_key,
        std::span<const std::byte> wrapped_keys,
        std::span<std::byte> destination,
        std::size_t& bytes_written) noexcept override
    {
        bytes_written = 0;
        const EVP_CIPHER* cipher =
            wrap_cipher(key_encrypting_key.size());
        if (cipher == nullptr
            || wrapped_keys.size() < 16U
            || (wrapped_keys.size() % 8U) != 0U
            || wrapped_keys.size()
                > static_cast<std::size_t>(INT_MAX)
            || destination.size() < wrapped_keys.size() - 8U) {
            return Error::invalid_key_material;
        }
        EvpCipherContext context;
        if (context.get() == nullptr) {
            return Error::cryptographic_failure;
        }
        EVP_CIPHER_CTX_set_flags(
            context.get(), EVP_CIPHER_CTX_FLAG_WRAP_ALLOW);
        if (EVP_DecryptInit_ex(context.get(), cipher, nullptr,
                reinterpret_cast<const unsigned char*>(
                    key_encrypting_key.data()),
                nullptr)
            != 1) {
            return Error::cryptographic_failure;
        }
        int written = 0;
        if (EVP_DecryptUpdate(context.get(),
                reinterpret_cast<unsigned char*>(
                    destination.data()),
                &written,
                reinterpret_cast<const unsigned char*>(
                    wrapped_keys.data()),
                static_cast<int>(wrapped_keys.size()))
            != 1) {
            return Error::cryptographic_failure;
        }
        int final_written = 0;
        if (EVP_DecryptFinal_ex(context.get(),
                reinterpret_cast<unsigned char*>(destination.data())
                    + written,
                &final_written)
                != 1) {
            return Error::cryptographic_failure;
        }
        bytes_written = static_cast<std::size_t>(
            written + final_written);
        return bytes_written == wrapped_keys.size() - 8U
            ? Error::none : Error::cryptographic_failure;
    }

    Error make_aes_ctr_cipher(
        std::span<const std::byte> key,
        std::unique_ptr<PayloadCipher>& cipher) noexcept override
    {
        cipher.reset();
        const EVP_CIPHER* implementation =
            ctr_cipher(key.size());
        if (implementation == nullptr) {
            return Error::invalid_key_material;
        }
        auto prepared = std::unique_ptr<OpenSslPayloadCipher>{
            new (std::nothrow)
                OpenSslPayloadCipher{implementation, key}};
        if (prepared == nullptr) {
            return Error::cryptographic_failure;
        }
        if (!prepared->valid()) {
            return Error::cryptographic_failure;
        }
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
        const EVP_CIPHER* implementation = gcm_cipher(key.size());
        if (implementation == nullptr) {
            return Error::invalid_key_material;
        }
        auto prepared = std::unique_ptr<OpenSslAuthenticatedPayloadCipher> {
            new (std::nothrow)
                OpenSslAuthenticatedPayloadCipher {implementation, key}};
        if (prepared == nullptr || !prepared->valid()) {
            return Error::cryptographic_failure;
        }
        cipher = std::move(prepared);
        return Error::none;
    }

    void secure_erase(
        std::span<std::byte> bytes) noexcept override
    {
        if (!bytes.empty()) {
            OPENSSL_cleanse(bytes.data(), bytes.size());
        }
    }
};

} // namespace

CryptoProvider& default_crypto_provider() noexcept
{
    static OpenSslCryptoProvider provider;
    return provider;
}

} // namespace robotweax::srt
