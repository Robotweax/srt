// SPDX-License-Identifier: MIT
#include "robotweax/srt/crypto_provider.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cstring>
#include <new>

namespace robotweax::srt {
namespace {
bool ok(NTSTATUS status) noexcept
{
    return status >= 0;
}
PUCHAR data(std::span<const std::byte> bytes) noexcept
{
    return reinterpret_cast<PUCHAR>(const_cast<std::byte*>(bytes.data()));
}
void erase(std::span<std::byte> bytes) noexcept
{
    if (!bytes.empty()) {
        SecureZeroMemory(bytes.data(), bytes.size());
    }
}
bool valid_key(std::span<const std::byte> key) noexcept
{
    return key.size() == 16 || key.size() == 24 || key.size() == 32;
}
bool fits(std::span<const std::byte> bytes) noexcept
{
    // Preserve the OpenSSL provider's input-size ceiling.
    return bytes.size() <= static_cast<std::size_t>(INT_MAX);
}
class Algorithm {
public:
    Algorithm(LPCWSTR name, ULONG flags = 0) noexcept
    {
        if (!ok(BCryptOpenAlgorithmProvider(&handle, name, nullptr, flags))) {
            handle = nullptr;
        }
    }
    ~Algorithm()
    {
        if (handle) {
            BCryptCloseAlgorithmProvider(handle, 0);
        }
    }
    Algorithm(const Algorithm&) = delete;
    Algorithm& operator=(const Algorithm&) = delete;
    BCRYPT_ALG_HANDLE handle = nullptr;
};

// Each prepared cipher owns its key handle; no mutable process-wide key state.
// CNG owns key-object memory and releases it when BCryptDestroyKey is called.
class Key {
public:
    Key(std::span<const std::byte> secret, bool gcm) noexcept
        : algorithm_(BCRYPT_AES_ALGORITHM)
    {
        const auto mode = gcm ? BCRYPT_CHAIN_MODE_GCM : BCRYPT_CHAIN_MODE_ECB;
        const auto length =
            gcm ? sizeof(BCRYPT_CHAIN_MODE_GCM) : sizeof(BCRYPT_CHAIN_MODE_ECB);
        if (algorithm_.handle && valid_key(secret)
            && ok(BCryptSetProperty(algorithm_.handle, BCRYPT_CHAINING_MODE,
                reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(mode)),
                static_cast<ULONG>(length), 0))) {
            if (!ok(BCryptGenerateSymmetricKey(algorithm_.handle, &handle,
                    nullptr, 0, data(secret), static_cast<ULONG>(secret.size()),
                    0))) {
                handle = nullptr;
            }
        }
    }
    ~Key()
    {
        if (handle) {
            BCryptDestroyKey(handle);
        }
    }
    Key(const Key&) = delete;
    Key& operator=(const Key&) = delete;
    bool block(std::span<std::byte> bytes, bool encrypt = true) noexcept
    {
        ULONG written = 0;
        const auto size = static_cast<ULONG>(bytes.size());
        const auto function = encrypt ? BCryptEncrypt : BCryptDecrypt;
        return handle
            && ok(function(handle, data(bytes), size, nullptr, nullptr, 0,
                data(bytes), size, &written, 0))
            && written == size;
    }
    BCRYPT_KEY_HANDLE handle = nullptr;

private:
    Algorithm algorithm_;
};

class Ctr final : public PayloadCipher {
public:
    explicit Ctr(std::span<const std::byte> secret) noexcept
        : key_(secret, false)
    {
    }
    bool valid() const noexcept
    {
        return key_.handle != nullptr;
    }
    Error transform(std::span<const std::byte, aes_block_size> iv,
        std::span<const std::byte> input,
        std::span<std::byte> output) noexcept override
    {
        if (output.size() < input.size()) {
            return Error::buffer_too_small;
        }
        if (!valid() || !fits(input)) {
            return Error::cryptographic_failure;
        }
        std::array<std::byte, 16> counter {};
        std::copy(iv.begin(), iv.end(), counter.begin());
        // Batch counter blocks to avoid one CNG call per AES block. The full
        // 128-bit counter increments big-endian, matching EVP AES-CTR.
        std::array<std::byte, 1024> stream {};
        for (std::size_t offset = 0; offset < input.size();) {
            const auto count = (std::min)(stream.size(), input.size() - offset);
            const auto padded = ((count + 15) / 16) * 16;
            for (std::size_t block = 0; block < padded; block += 16) {
                std::copy(
                    counter.begin(), counter.end(), stream.begin() + block);
                for (std::size_t i = counter.size(); i > 0; --i) {
                    const auto next =
                        std::to_integer<unsigned>(counter[i - 1]) + 1U;
                    counter[i - 1] = static_cast<std::byte>(next & 255U);
                    if (next <= 255U) {
                        break;
                    }
                }
            }
            if (!key_.block(std::span {stream}.first(padded))) {
                erase(stream);
                erase(counter);
                erase(output);
                return Error::cryptographic_failure;
            }
            for (std::size_t i = 0; i < count; ++i) {
                output[offset + i] = input[offset + i] ^ stream[i];
            }
            offset += count;
        }
        erase(stream);
        erase(counter);
        return Error::none;
    }

private:
    Key key_;
};

class Gcm final : public AuthenticatedPayloadCipher {
public:
    explicit Gcm(std::span<const std::byte> secret) noexcept
        : key_(secret, true)
    {
    }
    bool valid() const noexcept
    {
        return key_.handle != nullptr;
    }
    Error seal(std::span<const std::byte> iv, std::span<const std::byte> aad,
        std::span<const std::byte> input, std::span<std::byte> output,
        std::span<std::byte> tag) noexcept override
    {
        if (output.size() < input.size() || tag.size() < 16) {
            return Error::buffer_too_small;
        }
        if (!valid()) {
            return Error::cryptographic_failure;
        }
        if (iv.size() != 12 || tag.size() != 16) {
            return Error::invalid_key_material;
        }
        if (!fits(input) || !fits(aad)) {
            return Error::cryptographic_failure;
        }
        const auto result = crypt(iv, aad, input, output, tag, true);
        if (result != Error::none) {
            erase(output);
            erase(tag);
        }
        return result;
    }
    Error open(std::span<const std::byte> iv, std::span<const std::byte> aad,
        std::span<const std::byte> input, std::span<const std::byte> tag,
        std::span<std::byte> output) noexcept override
    {
        if (output.size() < input.size()) {
            return Error::buffer_too_small;
        }
        const auto reject = [&](Error error) noexcept {
            erase(output);
            return error;
        };
        if (tag.size() != 16) {
            return reject(Error::authentication_failure);
        }
        if (!valid()) {
            return reject(Error::cryptographic_failure);
        }
        if (iv.size() != 12) {
            return reject(Error::invalid_key_material);
        }
        if (!fits(input) || !fits(aad)) {
            return reject(Error::cryptographic_failure);
        }
        std::array<std::byte, 16> copied_tag {};
        std::copy(tag.begin(), tag.end(), copied_tag.begin());
        const auto result = crypt(iv, aad, input, output, copied_tag, false);
        erase(copied_tag);
        return result == Error::none ? result : reject(result);
    }

private:
    Error crypt(std::span<const std::byte> iv, std::span<const std::byte> aad,
        std::span<const std::byte> input, std::span<std::byte> output,
        std::span<std::byte> tag, bool encrypt) noexcept
    {
        std::array<std::byte, 12> nonce {};
        std::copy(iv.begin(), iv.end(), nonce.begin());
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
        BCRYPT_INIT_AUTH_MODE_INFO(info);
        info.pbNonce = data(nonce);
        info.cbNonce = static_cast<ULONG>(nonce.size());
        info.pbAuthData = data(aad);
        info.cbAuthData = static_cast<ULONG>(aad.size());
        info.pbTag = data(tag);
        info.cbTag = static_cast<ULONG>(tag.size());
        ULONG written = 0;
        const auto size = static_cast<ULONG>(input.size());
        const auto function = encrypt ? BCryptEncrypt : BCryptDecrypt;
        // Both pointers must be null for CNG's authenticated empty-message case.
        const auto status = function(key_.handle,
            input.empty() ? nullptr : data(input), size, &info, nullptr, 0,
            input.empty() ? nullptr : data(output), size, &written, 0);
        if (status == static_cast<NTSTATUS>(0xC000A002L)) {
            return Error::authentication_failure;
        }
        return ok(status) && written == size ? Error::none
                                             : Error::cryptographic_failure;
    }
    Key key_;
};

class Provider final : public CryptoProvider {
public:
    Error random_bytes(std::span<std::byte> output) noexcept override
    {
        if (!fits(output)) {
            return Error::cryptographic_failure;
        }
        if (output.empty()) {
            return Error::none;
        }
        return ok(BCryptGenRandom(nullptr, data(output),
                   static_cast<ULONG>(output.size()),
                   BCRYPT_USE_SYSTEM_PREFERRED_RNG))
            ? Error::none
            : Error::cryptographic_failure;
    }
    Error pbkdf2_hmac_sha1(std::span<const std::byte> passphrase,
        std::span<const std::byte> salt, std::uint32_t iterations,
        std::span<std::byte> output) noexcept override
    {
        if (!fits(passphrase) || !fits(salt) || !fits(output) || iterations == 0
            || iterations > static_cast<std::uint32_t>(INT_MAX)) {
            return Error::cryptographic_failure;
        }
        Algorithm algorithm(BCRYPT_SHA1_ALGORITHM, BCRYPT_ALG_HANDLE_HMAC_FLAG);
        return algorithm.handle
                && ok(BCryptDeriveKeyPBKDF2(algorithm.handle, data(passphrase),
                    static_cast<ULONG>(passphrase.size()), data(salt),
                    static_cast<ULONG>(salt.size()), iterations, data(output),
                    static_cast<ULONG>(output.size()), 0))
            ? Error::none
            : Error::cryptographic_failure;
    }
    Error wrap_key(std::span<const std::byte> kek,
        std::span<const std::byte> input, std::span<std::byte> output,
        std::size_t& written) noexcept override
    {
        return wrap(kek, input, output, written, true);
    }
    Error unwrap_key(std::span<const std::byte> kek,
        std::span<const std::byte> input, std::span<std::byte> output,
        std::size_t& written) noexcept override
    {
        return wrap(kek, input, output, written, false);
    }
    Error make_aes_ctr_cipher(std::span<const std::byte> key,
        std::unique_ptr<PayloadCipher>& cipher) noexcept override
    {
        return prepare<Ctr>(key, cipher);
    }
    bool supports_aes_gcm() const noexcept override
    {
        return true;
    }
    Error make_aes_gcm_cipher(std::span<const std::byte> key,
        std::unique_ptr<AuthenticatedPayloadCipher>& cipher) noexcept override
    {
        return prepare<Gcm>(key, cipher);
    }
    void secure_erase(std::span<std::byte> bytes) noexcept override
    {
        erase(bytes);
    }

private:
    template <class Implementation, class Interface>
    static Error prepare(std::span<const std::byte> key,
        std::unique_ptr<Interface>& output) noexcept
    {
        output.reset();
        if (!valid_key(key)) {
            return Error::invalid_key_material;
        }
        auto cipher = std::unique_ptr<Implementation>(
            new (std::nothrow) Implementation(key));
        if (!cipher || !cipher->valid()) {
            return Error::cryptographic_failure;
        }
        output = std::move(cipher);
        return Error::none;
    }
    // RFC 3394, with the default A6 integrity register. ECB is used solely
    // as the AES primitive, never directly as the payload encryption mode.
    static Error wrap(std::span<const std::byte> kek,
        std::span<const std::byte> input, std::span<std::byte> output,
        std::size_t& written, bool encrypt) noexcept
    {
        written = 0;
        if (!valid_key(kek) || input.empty() || input.size() % 8 != 0
            || !fits(input) || (!encrypt && input.size() < 16)
            || output.size()
                < (encrypt ? input.size() + 8 : input.size() - 8)) {
            return Error::invalid_key_material;
        }
        // EVP also rejects the one-register (8-byte plaintext) case.
        if (input.size() < (encrypt ? 16U : 24U)) {
            return Error::cryptographic_failure;
        }
        Key key(kek, false);
        if (!key.handle) {
            return Error::cryptographic_failure;
        }
        const auto size = encrypt ? input.size() : input.size() - 8;
        auto temporary =
            std::unique_ptr<std::byte[]>(new (std::nothrow) std::byte[size]);
        if (!temporary) {
            return Error::cryptographic_failure;
        }
        const std::span<std::byte> registers(temporary.get(), size);
        std::array<std::byte, 16> block {};
        if (encrypt) {
            std::fill_n(block.begin(), 8, std::byte {0xa6});
        } else {
            std::copy_n(input.begin(), 8, block.begin());
        }
        std::copy_n(input.begin() + (encrypt ? 0 : 8), size, registers.begin());
        const auto count = size / 8;
        bool success = true;
        for (std::size_t round = 0; round < 6 && success; ++round) {
            for (std::size_t index = 0; index < count && success; ++index) {
                const auto i = encrypt ? index : count - 1 - index;
                auto t = static_cast<std::uint64_t>(count)
                        * (encrypt ? round : 5 - round)
                    + i + 1;
                const auto xor_counter = [&]() noexcept {
                    for (std::size_t k = 8; k > 0; --k) {
                        block[k - 1] ^= static_cast<std::byte>(t & 255U);
                        t >>= 8;
                    }
                };
                std::copy_n(registers.begin() + i * 8, 8, block.begin() + 8);
                if (!encrypt) {
                    xor_counter();
                }
                success = key.block(block, encrypt);
                if (encrypt) {
                    xor_counter();
                }
                std::copy_n(block.begin() + 8, 8, registers.begin() + i * 8);
            }
        }
        if (!encrypt) {
            unsigned difference = 0;
            for (std::size_t k = 0; k < 8; ++k) {
                difference |= std::to_integer<unsigned>(block[k]) ^ 0xa6U;
            }
            success = success && difference == 0;
        }
        if (success) {
            if (encrypt) {
                std::copy_n(block.begin(), 8, output.begin());
            }
            std::copy(registers.begin(), registers.end(),
                output.begin() + (encrypt ? 8 : 0));
            written = size + (encrypt ? 8 : 0);
        } else {
            erase(output);
        }
        erase(registers);
        erase(block);
        return success ? Error::none : Error::cryptographic_failure;
    }
};
} // namespace
CryptoProvider& default_crypto_provider() noexcept
{
    static Provider provider;
    return provider;
}
} // namespace robotweax::srt
