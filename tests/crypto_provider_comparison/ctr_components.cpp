// SPDX-License-Identifier: MIT
// Isolated diagnostic kernels, not shipped implementation changes.
#include <windows.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <vector>

// Prevent elimination/fusion across repetitions. This cost is common to the
// byte/word comparison but must not be subtracted from production timings.
__declspec(noinline) void xor_bytes(const std::byte* input,
    const std::byte* stream, std::byte* output, std::size_t count)
{
    for (std::size_t i = 0; i < count; ++i)
        output[i] = input[i] ^ stream[i];
}
__declspec(noinline) void xor_words(const std::byte* input,
    const std::byte* stream, std::byte* output, std::size_t count)
{
    std::size_t i = 0;
    for (; count - i >= sizeof(std::uint64_t); i += sizeof(std::uint64_t)) {
        std::uint64_t a, b;
        // memcpy permits unaligned buffers and avoids aliasing violations.
        std::memcpy(&a, input + i, sizeof(a));
        std::memcpy(&b, stream + i, sizeof(b));
        a ^= b;
        std::memcpy(output + i, &a, sizeof(a));
    }
    for (; i < count; ++i)
        output[i] = input[i] ^ stream[i];
}
__declspec(noinline) void fill_counters(std::byte* output, std::size_t count)
{
    std::array<std::byte, 16> counter {};
    for (std::size_t block = 0; block < count; block += 16) {
        std::copy(counter.begin(), counter.end(), output + block);
        for (std::size_t i = counter.size(); i > 0; --i) {
            const auto next = std::to_integer<unsigned>(counter[i - 1]) + 1U;
            counter[i - 1] = static_cast<std::byte>(next & 255U);
            if (next <= 255U)
                break;
        }
    }
    SecureZeroMemory(counter.data(), counter.size());
}

void measure_ctr_components()
{
    // Differential coverage includes empty/tail/unaligned/in-place operation.
    for (std::size_t size = 0; size <= 1456; ++size) {
        for (unsigned offset = 0; offset < 8; ++offset) {
            std::vector<std::byte> input(size + 8), stream(size + 8),
                expected(size + 8), actual(size + 8);
            for (std::size_t i = 0; i < input.size(); ++i) {
                input[i] = std::byte((i * 17 + size) & 255);
                stream[i] = std::byte((i * 31 + offset) & 255);
            }
            xor_bytes(input.data() + offset, stream.data() + offset,
                expected.data() + offset, size);
            xor_words(input.data() + offset, stream.data() + offset,
                actual.data() + offset, size);
            if (actual != expected)
                throw std::runtime_error("word XOR differential mismatch");
            actual = input;
            expected = input;
            xor_bytes(expected.data() + offset, stream.data() + offset,
                expected.data() + offset, size);
            xor_words(actual.data() + offset, stream.data() + offset,
                actual.data() + offset, size);
            if (actual != expected)
                throw std::runtime_error("in-place word XOR mismatch");
        }
    }
    std::cout
        << "round,bytes,in_place,operation,batches,calls_per_batch,median_"
           "batch_ns_per_call,p95_batch_ns_per_call,mean_ns_per_call\n";
    for (unsigned round = 0; round < 5; ++round)
        for (auto size : {256U, 1024U, 1200U, 1316U})
            for (bool in_place : {false, true}) {
                std::vector<std::byte> input(size, std::byte {0x29}),
                    stream((size + 15) / 16 * 16, std::byte {0x53}),
                    output(size);
                for (unsigned order = 0; order < 4; ++order) {
                    const unsigned mode = (order + round) % 4;
                    const auto run = [&] {
                        auto* target = in_place ? input.data() : output.data();
                        if (mode == 0)
                            xor_bytes(
                                input.data(), stream.data(), target, size);
                        else if (mode == 1)
                            xor_words(
                                input.data(), stream.data(), target, size);
                        else if (mode == 2)
                            fill_counters(stream.data(), stream.size());
                        else
                            SecureZeroMemory(stream.data(), stream.size());
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
                    }
                    const auto mean =
                        std::accumulate(times.begin(), times.end(), 0.0)
                        / times.size();
                    std::sort(times.begin(), times.end());
                    const char* names[] = {
                        "xor-byte", "xor-word", "counter-fill", "erase"};
                    std::cout << round << ',' << size << ',' << in_place << ','
                              << names[mode] << ",100,256,"
                              << (times[49] + times[50]) / 2 << ',' << times[94]
                              << ',' << mean << '\n';
                }
            }
}
