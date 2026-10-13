// SPDX-License-Identifier: MIT
#include "robotweax/srt/crypto_provider.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <vector>

namespace robotweax::srt {
CryptoProvider& mbedtls_test_provider() noexcept;
CryptoProvider& openssl_test_provider() noexcept;
}
using namespace robotweax::srt;
void check(Error status)
{
    if (status != Error::none)
        throw std::runtime_error("crypto operation failed");
}
void require(bool value)
{
    if (!value)
        throw std::runtime_error("provider differential mismatch");
}
void compare()
{
    auto& candidate = mbedtls_test_provider();
    auto& oracle = openssl_test_provider();
    std::array<std::byte, 32> derived {}, expected {}, mac {}, expected_mac {};
    std::array<std::byte, 12> nonce {};
    std::array<std::byte, 16> iv {}, aad {}, tag {}, expected_tag {};
    for (const std::size_t key_size : {16U, 24U, 32U}) {
        std::vector<std::byte> key(key_size, std::byte {0x53});
        check(candidate.pbkdf2_hmac_sha1(key, aad, 2048, derived));
        check(oracle.pbkdf2_hmac_sha1(key, aad, 2048, expected));
        require(derived == expected);
        check(candidate.hmac_sha256(key, aad, mac));
        check(oracle.hmac_sha256(key, aad, expected_mac));
        require(mac == expected_mac);
        for (const std::size_t plain_size : {16U, 24U, 32U, 48U, 64U}) {
            std::vector<std::byte> keys(plain_size, std::byte {0x37});
            std::vector<std::byte> wrapped(plain_size + 8),
                expected_wrap(plain_size + 8), plain(plain_size);
            std::size_t size = 0, expected_size = 0;
            check(candidate.wrap_key(key, keys, wrapped, size));
            check(oracle.wrap_key(key, keys, expected_wrap, expected_size));
            require(wrapped == expected_wrap && size == expected_size);
            check(oracle.unwrap_key(key, wrapped, plain, size));
            require(plain == keys);
            check(candidate.unwrap_key(key, expected_wrap, plain, size));
            require(plain == keys);
        }
        std::unique_ptr<PayloadCipher> ctr, expected_ctr;
        std::unique_ptr<AuthenticatedPayloadCipher> gcm, expected_gcm;
        check(candidate.make_aes_ctr_cipher(key, ctr));
        check(oracle.make_aes_ctr_cipher(key, expected_ctr));
        check(candidate.make_aes_gcm_cipher(key, gcm));
        check(oracle.make_aes_gcm_cipher(key, expected_gcm));
        for (const std::size_t size :
            {0U, 1U, 15U, 16U, 17U, 188U, 1316U, 1456U, 16384U}) {
            for (unsigned offset = 0; offset < 8; ++offset) {
                std::vector<std::byte> storage(size + offset),
                    expected_bytes(size);
                auto bytes = std::span {storage}.subspan(offset);
                std::fill(bytes.begin(), bytes.end(), std::byte {0x29});
                check(expected_ctr->transform(iv, bytes, expected_bytes));
                check(ctr->transform(iv, bytes, bytes));
                require(std::equal(
                    bytes.begin(), bytes.end(), expected_bytes.begin()));
                check(ctr->transform(iv, bytes, bytes));
                require(
                    std::all_of(bytes.begin(), bytes.end(), [](std::byte b) {
                        return b == std::byte {0x29};
                    }));
                check(expected_gcm->seal(
                    nonce, aad, bytes, expected_bytes, expected_tag));
                check(gcm->seal(nonce, aad, bytes, bytes, tag));
                require(tag == expected_tag
                    && std::equal(
                        bytes.begin(), bytes.end(), expected_bytes.begin()));
                check(gcm->open(nonce, aad, bytes, tag, bytes));
                require(
                    std::all_of(bytes.begin(), bytes.end(), [](std::byte b) {
                        return b == std::byte {0x29};
                    }));
            }
        }
    }
    std::cerr
        << "Mbed TLS/OpenSSL differential checks passed for all key sizes, "
           "empty/unaligned/in-place packets and wrap/derive/HMAC\n";
}

void measure(CryptoProvider& provider, const char* name, unsigned round,
    std::size_t key_size, std::size_t size, bool authenticated)
{
    std::vector<std::byte> key(key_size, std::byte {0x53}),
        input(size, std::byte {0x29}), output(size);
    key[0] = std::byte(round);
    key[1] = std::byte(authenticated);
    key[2] = std::byte(size & 255);
    key[3] = std::byte((size >> 8) & 255);
    std::unique_ptr<PayloadCipher> ctr;
    std::unique_ptr<AuthenticatedPayloadCipher> gcm;
    check(provider.make_aes_ctr_cipher(key, ctr));
    check(provider.make_aes_gcm_cipher(key, gcm));
    std::array<std::byte, 16> iv {}, aad {}, tag {};
    std::array<std::byte, 12> nonce {};
    constexpr unsigned packets = 128, batches = 40;
    std::vector<double> timings;
    timings.reserve(batches);
    unsigned sequence = 0;
    for (unsigned batch = 0; batch < batches + 4; ++batch) {
        const auto start = std::chrono::steady_clock::now();
        for (unsigned packet = 0; packet < packets; ++packet) {
            ++sequence;
            for (unsigned i = 0; i < 4; ++i)
                iv[15 - i] = nonce[11 - i] =
                    std::byte((sequence >> (8 * i)) & 255);
            check(authenticated ? gcm->seal(nonce, aad, input, output, tag)
                                : ctr->transform(iv, input, output));
        }
        const auto end = std::chrono::steady_clock::now();
        if (batch >= 4)
            timings.push_back(
                std::chrono::duration<double, std::nano>(end - start).count()
                / packets);
    }
    std::sort(timings.begin(), timings.end());
    const double median = (timings[19] + timings[20]) / 2;
    const double mean =
        std::accumulate(timings.begin(), timings.end(), 0.0) / timings.size();
    std::cout << name << ',' << round << ',' << key_size << ',' << size << ','
              << (authenticated ? "gcm-seal" : "ctr") << ',' << median << ','
              << timings[37] << ',' << mean << ',' << size * 8000.0 / median
              << '\n';
}
int main(int argc, char**)
{
    try {
        compare();
        if (argc > 1) {
            std::cout << "backend,round,key_bytes,payload_bytes,operation,"
                         "median_ns,p95_batch_ns,mean_ns,mbps\n";
            for (unsigned round = 0; round < 3; ++round)
                for (const std::size_t key : {16U, 24U, 32U})
                    for (const std::size_t size : {188U, 1316U, 1456U, 16000U})
                        for (const bool gcm : {false, true})
                            for (unsigned order = 0; order < 2; ++order)
                                if ((round + order) % 2 == 0)
                                    measure(mbedtls_test_provider(), "mbedtls",
                                        round, key, size, gcm);
                                else
                                    measure(openssl_test_provider(), "openssl",
                                        round, key, size, gcm);
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
