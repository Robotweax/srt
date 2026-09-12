// SPDX-License-Identifier: MIT
// Interposition is confined to this executable, never the shipped library.
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>

namespace fault {
enum class Call {
    none,
    algorithm,
    property,
    key,
    encrypt,
    decrypt,
    random,
    derive,
    destroy,
    close
};
Call selected = Call::none;
unsigned remaining = 0;
int handles = 0;
void arm(Call call, unsigned occurrence = 1)
{
    selected = call;
    remaining = occurrence;
}
template <class Function, class... Args>
NTSTATUS invoke(Call call, Function function, Args... args)
{
    if (call == selected && remaining && --remaining == 0) {
        selected = Call::none;
        return static_cast<NTSTATUS>(0xC0000001L);
    }
    const auto status = function(args...);
    if (status >= 0) {
        if (call == Call::algorithm || call == Call::key) {
            ++handles;
        }
        if (call == Call::destroy || call == Call::close) {
            --handles;
        }
    }
    return status;
}
}

// Object-like aliases also cover taking BCryptEncrypt/Decrypt function pointers.
template <class... Args> NTSTATUS test_encrypt(Args... args)
{
    return fault::invoke(fault::Call::encrypt, ::BCryptEncrypt, args...);
}
template <class... Args> NTSTATUS test_decrypt(Args... args)
{
    return fault::invoke(fault::Call::decrypt, ::BCryptDecrypt, args...);
}
// The provider selects encrypt/decrypt via a conditional function pointer, so
// those two wrappers need the exact non-template CNG signature.
NTSTATUS WINAPI injected_encrypt(BCRYPT_KEY_HANDLE key, PUCHAR input,
    ULONG size, VOID* padding, PUCHAR iv, ULONG iv_size, PUCHAR output,
    ULONG capacity, ULONG* written, ULONG flags)
{
    return test_encrypt(key, input, size, padding, iv, iv_size, output,
        capacity, written, flags);
}
NTSTATUS WINAPI injected_decrypt(BCRYPT_KEY_HANDLE key, PUCHAR input,
    ULONG size, VOID* padding, PUCHAR iv, ULONG iv_size, PUCHAR output,
    ULONG capacity, ULONG* written, ULONG flags)
{
    return test_decrypt(key, input, size, padding, iv, iv_size, output,
        capacity, written, flags);
}
#define BCryptEncrypt injected_encrypt
#define BCryptDecrypt injected_decrypt
#define BCryptOpenAlgorithmProvider(...)                                       \
    fault::invoke(                                                             \
        fault::Call::algorithm, ::BCryptOpenAlgorithmProvider, __VA_ARGS__)
#define BCryptSetProperty(...)                                                 \
    fault::invoke(fault::Call::property, ::BCryptSetProperty, __VA_ARGS__)
#define BCryptGenerateSymmetricKey(...)                                        \
    fault::invoke(fault::Call::key, ::BCryptGenerateSymmetricKey, __VA_ARGS__)
#define BCryptGenRandom(...)                                                   \
    fault::invoke(fault::Call::random, ::BCryptGenRandom, __VA_ARGS__)
#define BCryptDeriveKeyPBKDF2(...)                                             \
    fault::invoke(fault::Call::derive, ::BCryptDeriveKeyPBKDF2, __VA_ARGS__)
#define BCryptDestroyKey(...)                                                  \
    fault::invoke(fault::Call::destroy, ::BCryptDestroyKey, __VA_ARGS__)
#define BCryptCloseAlgorithmProvider(...)                                      \
    fault::invoke(                                                             \
        fault::Call::close, ::BCryptCloseAlgorithmProvider, __VA_ARGS__)
#include "../../src/crypto_bcrypt.cpp"

using namespace robotweax::srt;
void require(bool value)
{
    if (!value) {
        throw std::runtime_error("CNG failure contract violated");
    }
}
template <class T> bool zero(const T& output)
{
    return std::all_of(output.begin(), output.end(), [](std::byte b) {
        return b == std::byte {};
    });
}
int main()
{
    try {
        auto& provider = default_crypto_provider();
        std::array<std::byte, 16> key {}, iv {};
        for (auto call :
            {fault::Call::algorithm, fault::Call::property, fault::Call::key}) {
            for (bool gcm : {false, true}) {
                fault::arm(call);
                if (gcm) {
                    std::unique_ptr<AuthenticatedPayloadCipher> cipher;
                    require(provider.make_aes_gcm_cipher(key, cipher)
                            == Error::cryptographic_failure
                        && !cipher);
                } else {
                    std::unique_ptr<PayloadCipher> cipher;
                    require(provider.make_aes_ctr_cipher(key, cipher)
                            == Error::cryptographic_failure
                        && !cipher);
                }
                require(fault::remaining == 0 && fault::handles == 0);
            }
        }
        {
            std::unique_ptr<PayloadCipher> cipher;
            require(provider.make_aes_ctr_cipher(key, cipher) == Error::none);
            std::array<std::byte, 2048> input {}, output;
            output.fill(std::byte {0x5a});
            fault::arm(
                fault::Call::encrypt, 2); // Fail after a successful batch.
            require(cipher->transform(iv, input, output)
                == Error::cryptographic_failure);
            require(fault::remaining == 0 && zero(output));
            require(cipher->transform(iv, input, output) == Error::none);
            input.fill(std::byte {0x5a});
            fault::arm(fault::Call::encrypt, 2);
            require(cipher->transform(iv, input, input)
                == Error::cryptographic_failure);
            require(fault::remaining == 0 && zero(input));
            require(cipher->transform(iv, input, input) == Error::none);
        }
        {
            std::unique_ptr<AuthenticatedPayloadCipher> cipher;
            require(provider.make_aes_gcm_cipher(key, cipher) == Error::none);
            std::array<std::byte, 12> nonce {};
            std::array<std::byte, 16> input {}, output, tag;
            output.fill(std::byte {0x5a});
            tag.fill(std::byte {0x5a});
            fault::arm(fault::Call::encrypt);
            require(cipher->seal(nonce, key, input, output, tag)
                == Error::cryptographic_failure);
            require(zero(output) && zero(tag));
            require(
                cipher->seal(nonce, key, input, output, tag) == Error::none);
            input.fill(std::byte {0x5a});
            fault::arm(fault::Call::decrypt);
            require(cipher->open(nonce, key, output, tag, input)
                == Error::cryptographic_failure);
            require(zero(input));
        }
        std::array<std::byte, 24> wrapped {};
        std::size_t written = 99;
        fault::arm(fault::Call::encrypt, 2);
        require(provider.wrap_key(key, key, wrapped, written)
            == Error::cryptographic_failure);
        require(written == 0 && zero(wrapped));
        require(provider.wrap_key(key, key, wrapped, written) == Error::none);
        std::array<std::byte, 16> plain;
        plain.fill(std::byte {0x5a});
        fault::arm(fault::Call::decrypt, 2);
        require(provider.unwrap_key(key, wrapped, plain, written)
            == Error::cryptographic_failure);
        require(written == 0 && zero(plain));
        fault::arm(fault::Call::random);
        require(provider.random_bytes(plain) == Error::cryptographic_failure);
        fault::arm(fault::Call::derive);
        require(provider.pbkdf2_hmac_sha1(key, key, 2048, plain)
            == Error::cryptographic_failure);
        require(fault::remaining == 0 && fault::handles == 0);
        std::cout << "CNG injected failure checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
