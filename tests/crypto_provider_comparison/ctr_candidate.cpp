// SPDX-License-Identifier: MIT
#include "robotweax/srt/crypto_provider.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <vector>
using namespace robotweax::srt;
namespace robotweax::srt {
CryptoProvider& openssl_test_provider() noexcept;
}
std::unique_ptr<PayloadCipher> baseline_ctr(std::span<const std::byte> key);
void check(Error);

void measure_ctr_candidate()
{
    // Include counter carries/full wrap, chunk boundaries, tails and alignment.
    for (auto key_size : {16U, 24U, 32U}) {
        std::vector<std::byte> key(key_size, std::byte {0x53});
        std::unique_ptr<PayloadCipher> candidate;
        check(default_crypto_provider().make_aes_ctr_cipher(key, candidate));
        std::unique_ptr<PayloadCipher> oracle;
        check(openssl_test_provider().make_aes_ctr_cipher(key, oracle));
        for (auto size : {0U, 1U, 7U, 8U, 15U, 16U, 17U, 1008U, 1023U, 1024U,
                 1025U, 1200U, 1316U, 2048U, 2049U, 4097U})
            for (unsigned offset = 0; offset < 8; ++offset)
                for (auto fill : {0U, 255U}) {
                    std::array<std::byte, 16> iv;
                    iv.fill(std::byte(fill));
                    std::vector<std::byte> input(size + 8, std::byte {0x29}),
                        expected(size + 8), actual(size + 8);
                    auto src = std::span(input).subspan(offset, size);
                    check(oracle->transform(
                        iv, src, std::span(expected).subspan(offset, size)));
                    check(candidate->transform(
                        iv, src, std::span(actual).subspan(offset, size)));
                    if (actual != expected)
                        throw std::runtime_error(
                            "candidate differential mismatch");
                    actual = input;
                    auto dst = std::span(actual).subspan(offset, size);
                    check(candidate->transform(iv, dst, dst));
                    if (!std::equal(
                            dst.begin(), dst.end(), expected.begin() + offset))
                        throw std::runtime_error("candidate in-place mismatch");
                }
    }
    std::cout << "round,provider,bytes,in_place,batches,calls_per_batch,median_"
                 "batch_ns_per_call,p95_batch_ns_per_call,mean_ns_per_call\n";
    for (unsigned round = 0; round < 5; ++round)
        for (auto size : {1200U, 1316U})
            for (bool in_place : {false, true})
                for (unsigned order = 0; order < 3; ++order) {
                    const auto variant = (order + round) % 3;
                    std::array<std::byte, 32> key {};
                    key[0] = std::byte(round);
                    std::unique_ptr<PayloadCipher> cipher;
                    if (variant == 0)
                        cipher = baseline_ctr(key);
                    else
                        check((variant == 1 ? default_crypto_provider()
                                            : openssl_test_provider())
                                .make_aes_ctr_cipher(key, cipher));
                    std::array<std::byte, 16> iv {};
                    std::vector<std::byte> input(size, std::byte {0x29}),
                        output(size);
                    // Fixed synthetic IV isolates transform cost. Never use this
                    // benchmark pattern for real traffic. Even counts restore
                    // the input for in-place CTR (XOR is its own inverse).
                    const auto run = [&] {
                        check(cipher->transform(iv, input,
                            in_place ? std::span(input) : std::span(output)));
                    };
                    for (unsigned i = 0; i < 200; ++i)
                        run();
                    std::vector<double> times;
                    for (unsigned batch = 0; batch < 100; ++batch) {
                        const auto start = std::chrono::steady_clock::now();
                        for (unsigned i = 0; i < 256; ++i)
                            run();
                        const auto end = std::chrono::steady_clock::now();
                        times.push_back(
                            std::chrono::duration<double, std::nano>(
                                end - start)
                                .count()
                            / 256);
                        if (in_place
                            && !std::all_of(
                                input.begin(), input.end(), [](auto b) {
                                    return b == std::byte {0x29};
                                }))
                            throw std::runtime_error(
                                "in-place round trip failed");
                    }
                    const auto mean =
                        std::accumulate(times.begin(), times.end(), 0.0)
                        / times.size();
                    std::sort(times.begin(), times.end());
                    const char* names[] = {
                        "bcrypt-baseline", "bcrypt-candidate", "openssl"};
                    std::cout << round << ',' << names[variant] << ',' << size
                              << ',' << in_place << ",100,256,"
                              << (times[49] + times[50]) / 2 << ',' << times[94]
                              << ',' << mean << '\n';
                }
}
