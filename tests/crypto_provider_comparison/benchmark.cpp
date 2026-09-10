// SPDX-License-Identifier: MIT
// Manual diagnostic only: no thresholds on shared runners.
#include "robotweax/srt/crypto_provider.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <vector>

namespace robotweax::srt {
CryptoProvider& openssl_test_provider() noexcept;
}
using namespace robotweax::srt;
void check(Error error)
{
    if (error != Error::none)
        throw std::runtime_error("crypto operation failed");
}
void measure(CryptoProvider& provider, const char* name, unsigned round,
    std::size_t key_size, std::size_t size, unsigned mode)
{
    std::vector<std::byte> key(key_size, std::byte {0x53});
    // Distinct synthetic key per measurement. Never use real traffic secrets.
    key[0] = std::byte(round);
    key[1] = std::byte(mode);
    key[2] = std::byte(size & 255);
    std::unique_ptr<PayloadCipher> ctr;
    std::unique_ptr<AuthenticatedPayloadCipher> gcm;
    check(provider.make_aes_ctr_cipher(key, ctr));
    check(provider.make_aes_gcm_cipher(key, gcm));
    std::vector<std::byte> input(size, std::byte {0x29}), output(size);
    std::array<std::byte, 16> iv {}, tag {}, aad {};
    std::array<std::byte, 12> nonce {};
    if (mode == 2) {
        check(gcm->seal(nonce, aad, input, output, tag));
        input = output;
    }
    constexpr unsigned warmup = 200, samples = 2000;
    std::vector<double> durations;
    durations.reserve(samples);
    for (unsigned i = 0; i < warmup + samples; ++i) {
        if (mode != 2) {
            for (unsigned j = 0; j < 4; ++j) {
                nonce[11 - j] = std::byte((i >> (8 * j)) & 255);
                iv[11 - j] = nonce[11 - j];
            }
        }
        const auto start = std::chrono::steady_clock::now();
        const auto error = mode == 0 ? ctr->transform(iv, input, output)
            : mode == 1 ? gcm->seal(nonce, aad, input, output, tag)
                        : gcm->open(nonce, aad, input, tag, output);
        const auto end = std::chrono::steady_clock::now();
        check(error);
        if (i >= warmup)
            durations.push_back(
                std::chrono::duration<double, std::nano>(end - start).count());
    }
    if (mode == 2
        && !std::all_of(output.begin(), output.end(), [](std::byte b) {
               return b == std::byte {0x29};
           }))
        throw std::runtime_error("decrypted payload mismatch");
    const double mean =
        std::accumulate(durations.begin(), durations.end(), 0.0) / samples;
    std::sort(durations.begin(), durations.end());
    std::cout << name << ',' << round << ',' << key_size << ',' << size << ','
              << (mode == 0          ? "ctr"
                         : mode == 1 ? "gcm-seal"
                                     : "gcm-open")
              << ',' << samples << ',' << (durations[999] + durations[1000]) / 2
              << ',' << durations[1899] << ',' << mean << '\n';
}
int main()
{
    try {
        std::cout << "provider,round,key_bytes,payload_bytes,operation,samples,"
                     "median_ns,p95_ns,mean_ns\n";
        for (unsigned round = 0; round < 5; ++round) {
            // Alternate provider order to reduce systematic warm-up/order bias.
            for (unsigned order = 0; order < 2; ++order) {
                const bool native = (round + order) % 2 == 0;
                auto& provider = native ? default_crypto_provider()
                                        : openssl_test_provider();
                for (auto key_size : {16U, 24U, 32U})
                    for (auto size : {1200U, 1316U})
                        for (unsigned mode = 0; mode < 3; ++mode)
                            measure(provider, native ? "bcrypt" : "openssl",
                                round, key_size, size, mode);
            }
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
