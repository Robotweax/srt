// Portable, reproducible mutation replay for hosts without a libFuzzer runtime.
// This provides no coverage feedback; the same entry points also run in libFuzzer.
#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t*, std::size_t);

int main(int argc, char** argv)
{
    std::size_t runs = 1000;
    std::uint64_t seed = 1;
    for (int index = 1; index < argc; index += 2) {
        if (index + 1 == argc)
            return 2;
        const std::string_view key {argv[index]}, value {argv[index + 1]};
        std::uint64_t parsed = 0;
        const auto conversion =
            std::from_chars(value.data(), value.data() + value.size(), parsed);
        if (conversion.ec != std::errc {}
            || conversion.ptr != value.data() + value.size())
            return 2;
        if (key == "--runs" && parsed >= 1 && parsed <= 10000000)
            runs = static_cast<std::size_t>(parsed);
        else if (key == "--seed" && parsed != 0)
            seed = parsed;
        else
            return 2;
    }
    auto random = [state = seed]() mutable {
        state ^= state >> 12U;
        state ^= state << 25U;
        state ^= state >> 27U;
        return state * 2685821657736338717ULL;
    };
    struct Input {
        std::array<std::uint8_t, 2048> bytes {};
        std::size_t size = 0;
    };
    std::array<Input, 8> corpus {};
    corpus[1].size = 32;
    corpus[2].size = 128;
    corpus[2].bytes.fill(255);
    corpus[3].size = corpus[3].bytes.size();
    for (const auto name : {std::string_view {"rwx-fixed-v1"},
             std::string_view {"rwx-repeat-v1"}}) {
        auto& input = corpus[name.size() == 12 ? 4 : 5];
        input.size = name.size();
        std::copy(name.begin(), name.end(), input.bytes.begin());
    }
    for (const auto& input : corpus)
        (void)LLVMFuzzerTestOneInput(input.bytes.data(), input.size);
    for (std::size_t run = 0; run < runs; ++run) {
        auto input = corpus[random() % corpus.size()];
        switch (random() % 4) {
        case 0:
            input.size =
                static_cast<std::size_t>(random() % (input.bytes.size() + 1));
            for (std::size_t index = 0; index < input.size; ++index)
                input.bytes[index] = static_cast<std::uint8_t>(random());
            break;
        case 1:
            input.size =
                static_cast<std::size_t>(random() % (input.bytes.size() + 1));
            break;
        default:
            if (input.size == 0)
                input.size = 1;
            for (std::size_t changes = 0, count = 1 + random() % 8;
                changes < count; ++changes)
                input.bytes[random() % input.size] ^=
                    static_cast<std::uint8_t>(random());
            break;
        }
        (void)LLVMFuzzerTestOneInput(input.bytes.data(), input.size);
        corpus[run % corpus.size()] = input;
    }
    std::cout << "mutation_runs=" << runs << " seed=" << seed
              << " initial_seeds=" << corpus.size() << '\n';
}
