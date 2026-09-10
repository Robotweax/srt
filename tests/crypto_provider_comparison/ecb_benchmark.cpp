// SPDX-License-Identifier: MIT
// Test-only access to the exact prepared CNG key wrapper; no production hooks.
#include "../../src/crypto_bcrypt.cpp"
#include <openssl/evp.h>
#include <chrono>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <vector>

void measure_ecb()
{
    using namespace robotweax::srt;
    std::cout
        << "provider,round,key_bytes,payload_bytes,batches,packets_per_batch,"
           "median_batch_ns_per_packet,p95_batch_ns_per_packet,mean_ns_per_"
           "packet\n";
    for (unsigned round = 0; round < 5; ++round) {
        std::array<std::byte, 32> secret {};
        secret[0] = std::byte(round);
        Key key(secret, false);
        std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> ctx(
            EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
        if (!key.handle || !ctx
            || EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_ecb(), nullptr,
                   reinterpret_cast<const unsigned char*>(secret.data()),
                   nullptr)
                != 1
            || EVP_CIPHER_CTX_set_padding(ctx.get(), 0) != 1)
            throw std::runtime_error("ECB setup failed");
        for (unsigned order = 0; order < 2; ++order) {
            const bool native = (round + order) % 2 == 0;
            for (auto size : {16U, 256U, 1024U, 1200U, 1328U, 2048U, 16000U}) {
                std::vector<std::byte> data(size, std::byte {0x29});
                auto expected = data;
                int written = 0;
                if (!key.block(data)
                    || EVP_EncryptUpdate(ctx.get(),
                           reinterpret_cast<unsigned char*>(expected.data()),
                           &written,
                           reinterpret_cast<const unsigned char*>(
                               expected.data()),
                           size)
                        != 1
                    || written != static_cast<int>(size) || data != expected)
                    throw std::runtime_error("ECB differential check failed");
                const auto operation = [&] {
                    if (native) {
                        if (!key.block(data))
                            throw std::runtime_error("CNG ECB failed");
                    } else if (EVP_EncryptUpdate(ctx.get(),
                                   reinterpret_cast<unsigned char*>(
                                       data.data()),
                                   &written,
                                   reinterpret_cast<const unsigned char*>(
                                       data.data()),
                                   size)
                            != 1
                        || written != static_cast<int>(size)) {
                        throw std::runtime_error("OpenSSL ECB failed");
                    }
                };
                for (unsigned i = 0; i < 200; ++i)
                    operation();
                std::vector<double> times;
                for (unsigned batch = 0; batch < 100; ++batch) {
                    const auto start = std::chrono::steady_clock::now();
                    for (unsigned i = 0; i < 256; ++i)
                        operation();
                    const auto end = std::chrono::steady_clock::now();
                    times.push_back(
                        std::chrono::duration<double, std::nano>(end - start)
                            .count()
                        / 256);
                }
                const auto mean =
                    std::accumulate(times.begin(), times.end(), 0.0)
                    / times.size();
                std::sort(times.begin(), times.end());
                std::cout << (native ? "bcrypt" : "openssl") << ',' << round
                          << ",32," << size << ",100,256,"
                          << (times[49] + times[50]) / 2 << ',' << times[94]
                          << ',' << mean << '\n';
            }
        }
    }
}
