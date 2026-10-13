// SPDX-License-Identifier: MIT
// Dependency faults are confined to this executable, never the library.
#include "robotweax/srt/crypto_provider.hpp"
#include <mbedtls/aes.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/gcm.h>
#include <mbedtls/md.h>
#include <mbedtls/nist_kw.h>
#include <mbedtls/pkcs5.h>
#include <mbedtls/platform.h>

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace fault {
enum class Call {
    none,
    allocation,
    seed,
    random,
    ctr_key,
    ctr,
    gcm_key,
    seal,
    open,
    derive,
    hmac,
    wrap_key,
    wrap,
    unwrap
};
std::atomic<Call> selected {Call::none};
std::atomic<unsigned> remaining {0};
void arm(Call call, unsigned occurrence = 1)
{
    selected = call;
    remaining = occurrence;
}
template <class Function, class... Args>
int invoke(Call call, Function function, Args... args)
{
    if (selected == call && remaining.fetch_sub(1) == 1) {
        selected = Call::none;
        return -1;
    }
    return function(args...);
}
std::atomic<unsigned> allocations {0};
std::atomic<int> outstanding {0};
void* allocate(std::size_t count, std::size_t size)
{
    ++allocations;
    if (selected == Call::allocation && remaining.fetch_sub(1) == 1) {
        selected = Call::none;
        return nullptr;
    }
    void* result = std::calloc(count, size);
    if (result)
        ++outstanding;
    return result;
}
void release(void* pointer)
{
    if (pointer)
        --outstanding;
    std::free(pointer);
}
}
#define mbedtls_ctr_drbg_seed(...)                                             \
    fault::invoke(fault::Call::seed, ::mbedtls_ctr_drbg_seed, __VA_ARGS__)
#define mbedtls_ctr_drbg_random(...)                                           \
    fault::invoke(fault::Call::random, ::mbedtls_ctr_drbg_random, __VA_ARGS__)
#define mbedtls_aes_setkey_enc(...)                                            \
    fault::invoke(fault::Call::ctr_key, ::mbedtls_aes_setkey_enc, __VA_ARGS__)
#define mbedtls_aes_crypt_ctr(...)                                             \
    fault::invoke(fault::Call::ctr, ::mbedtls_aes_crypt_ctr, __VA_ARGS__)
#define mbedtls_gcm_setkey(...)                                                \
    fault::invoke(fault::Call::gcm_key, ::mbedtls_gcm_setkey, __VA_ARGS__)
#define mbedtls_gcm_crypt_and_tag(...)                                         \
    fault::invoke(fault::Call::seal, ::mbedtls_gcm_crypt_and_tag, __VA_ARGS__)
#define mbedtls_gcm_auth_decrypt(...)                                          \
    fault::invoke(fault::Call::open, ::mbedtls_gcm_auth_decrypt, __VA_ARGS__)
#define mbedtls_pkcs5_pbkdf2_hmac_ext(...)                                     \
    fault::invoke(                                                             \
        fault::Call::derive, ::mbedtls_pkcs5_pbkdf2_hmac_ext, __VA_ARGS__)
#define mbedtls_md_hmac(...)                                                   \
    fault::invoke(fault::Call::hmac, ::mbedtls_md_hmac, __VA_ARGS__)
#define mbedtls_nist_kw_setkey(...)                                            \
    fault::invoke(fault::Call::wrap_key, ::mbedtls_nist_kw_setkey, __VA_ARGS__)
#define mbedtls_nist_kw_wrap(...)                                              \
    fault::invoke(fault::Call::wrap, ::mbedtls_nist_kw_wrap, __VA_ARGS__)
#define mbedtls_nist_kw_unwrap(...)                                            \
    fault::invoke(fault::Call::unwrap, ::mbedtls_nist_kw_unwrap, __VA_ARGS__)
#define default_crypto_provider mbedtls_fault_provider
#include "../src/crypto_mbedtls.cpp"
#undef default_crypto_provider

using namespace robotweax::srt;
void require(bool value, const char* message)
{
    if (!value)
        throw std::runtime_error(message);
}
template <class Bytes> bool zero(const Bytes& bytes)
{
    return std::all_of(bytes.begin(), bytes.end(), [](std::byte b) {
        return b == std::byte {};
    });
}

int main()
{
    try {
#if defined(MBEDTLS_PLATFORM_MEMORY)
        require(
            mbedtls_platform_set_calloc_free(fault::allocate, fault::release)
                == 0,
            "allocation hook");
#endif
        MbedTlsCryptoProvider provider;
        std::array<std::byte, 32> key {}, output {};
        output.fill(std::byte {0xa5});
        fault::arm(fault::Call::seed);
        require(provider.random_bytes(output) == Error::cryptographic_failure
                && zero(output),
            "seed failure erasure");
        require(provider.random_bytes(output) == Error::none && !zero(output),
            "seed retry");
        std::vector<std::byte> random(
            MBEDTLS_CTR_DRBG_MAX_REQUEST + 7, std::byte {0xa5});
        fault::arm(fault::Call::random, 2);
        require(provider.random_bytes(random) == Error::cryptographic_failure
                && zero(random),
            "partial random failure erasure");
        require(provider.random_bytes(random) == Error::none && !zero(random),
            "random recovery");

        for (const auto call : {fault::Call::derive, fault::Call::hmac}) {
            output.fill(std::byte {0xa5});
            fault::arm(call);
            const auto status = call == fault::Call::derive
                ? provider.pbkdf2_hmac_sha1(key, key, 1, output)
                : provider.hmac_sha256(key, key, output);
            require(status == Error::cryptographic_failure && zero(output),
                "derivation failure erasure");
        }
        for (const auto call : {fault::Call::wrap_key, fault::Call::wrap}) {
            std::array<std::byte, 40> wrapped;
            wrapped.fill(std::byte {0xa5});
            std::size_t written = 123;
            fault::arm(call);
            require(provider.wrap_key(key, key, wrapped, written)
                        == Error::cryptographic_failure
                    && written == 0 && zero(wrapped),
                "wrap failure");
        }
        std::array<std::byte, 40> wrapped {};
        std::size_t written = 0;
        require(provider.wrap_key(key, key, wrapped, written) == Error::none
                && written == 40,
            "valid wrap");
        for (const bool injected : {true, false}) {
            output.fill(std::byte {0xa5});
            if (injected)
                fault::arm(fault::Call::unwrap);
            else
                wrapped[0] ^= std::byte {1};
            written = 123;
            require(provider.unwrap_key(key, wrapped, output, written)
                        == Error::cryptographic_failure
                    && written == 0 && zero(output),
                "unwrap failure erasure");
        }

        std::unique_ptr<PayloadCipher> ctr;
        std::unique_ptr<AuthenticatedPayloadCipher> gcm;
        require(provider.make_aes_ctr_cipher(key, ctr) == Error::none,
            "initial CTR key");
        fault::arm(fault::Call::ctr_key);
        require(provider.make_aes_ctr_cipher(key, ctr)
                    == Error::cryptographic_failure
                && !ctr,
            "CTR key failure reset");
        require(provider.make_aes_gcm_cipher(key, gcm) == Error::none,
            "initial GCM key");
        fault::arm(fault::Call::gcm_key);
        require(provider.make_aes_gcm_cipher(key, gcm)
                    == Error::cryptographic_failure
                && !gcm,
            "GCM key failure reset");
#if defined(MBEDTLS_PLATFORM_MEMORY)
        const auto outstanding_before = fault::outstanding.load();
        fault::arm(fault::Call::allocation);
        require(provider.make_aes_gcm_cipher(key, gcm)
                    == Error::cryptographic_failure
                && !gcm,
            "GCM allocation failure reset");
        require(fault::outstanding == outstanding_before,
            "failed GCM allocation leak");
#endif
        require(provider.make_aes_ctr_cipher(key, ctr) == Error::none,
            "prepared CTR");
        require(provider.make_aes_gcm_cipher(key, gcm) == Error::none,
            "prepared GCM");
        std::array<std::byte, 16> iv {}, tag {};
        std::array<std::byte, 12> nonce {};
        output.fill(std::byte {0xa5});
        fault::arm(fault::Call::ctr);
        require(ctr->transform(iv, key, output) == Error::cryptographic_failure
                && zero(output),
            "CTR failure erasure");
        for (const auto call : {fault::Call::seal, fault::Call::open}) {
            output.fill(std::byte {0xa5});
            tag.fill(std::byte {0xa5});
            fault::arm(call);
            const auto status = call == fault::Call::seal
                ? gcm->seal(nonce, key, key, output, tag)
                : gcm->open(nonce, key, key, tag, output);
            require(status == Error::cryptographic_failure && zero(output),
                "GCM dependency failure erasure");
            require(
                call != fault::Call::seal || zero(tag), "GCM seal tag erasure");
        }
        require(fault::selected == fault::Call::none, "injection consumed");

        // Independent packets reuse prepared keys, with distinct IVs and unaligned in-place buffers.
        std::array<std::byte, 1317> packet {};
        auto payload = std::span {packet}.subspan(1);
        const auto allocations_before = fault::allocations.load();
        for (unsigned n = 0; n < 1000; ++n) {
            for (unsigned i = 0; i < 4; ++i)
                iv[15 - i] = nonce[11 - i] = std::byte((n >> (8 * i)) & 255);
            std::fill(payload.begin(), payload.end(), std::byte {0x29});
            require(ctr->transform(iv, payload, payload) == Error::none,
                "in-place CTR encrypt");
            require(ctr->transform(iv, payload, payload) == Error::none,
                "in-place CTR decrypt");
            require(std::all_of(payload.begin(), payload.end(),
                        [](std::byte b) {
                            return b == std::byte {0x29};
                        }),
                "CTR integrity");
            require(gcm->seal(nonce, key, payload, payload, tag) == Error::none,
                "in-place GCM seal");
            require(gcm->open(nonce, key, payload, tag, payload) == Error::none,
                "in-place GCM open");
            require(std::all_of(payload.begin(), payload.end(),
                        [](std::byte b) {
                            return b == std::byte {0x29};
                        }),
                "GCM integrity");
        }
#if defined(MBEDTLS_PLATFORM_MEMORY)
        require(fault::allocations == allocations_before,
            "prepared payload path allocated");
        std::cout << "Allocation hooks: 0 allocations across 4000 payload "
                     "operations\n";
#else
        (void)allocations_before;
        std::cout << "Allocation hooks unavailable (enable "
                     "MBEDTLS_PLATFORM_MEMORY for qualification)\n";
#endif
        require(gcm->seal(nonce, key, payload, payload, tag) == Error::none,
            "tamper fixture");
        tag[0] ^= std::byte {1};
        require(gcm->open(nonce, key, payload, tag, payload)
                    == Error::authentication_failure
                && zero(payload),
            "in-place authentication erasure");

        std::atomic<bool> okay {true};
        std::vector<std::thread> threads;
        for (unsigned i = 0; i < 8; ++i) {
            threads.emplace_back([&] {
                std::array<std::byte, 32> bytes {};
                for (unsigned n = 0; n < 100; ++n)
                    if (provider.random_bytes(bytes) != Error::none
                        || zero(bytes))
                        okay = false;
            });
        }
        for (auto& thread : threads)
            thread.join();
        require(okay, "concurrent provider RNG");
        std::cout << "Mbed TLS failure, in-place, prepared-key and concurrency "
                     "contracts passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
