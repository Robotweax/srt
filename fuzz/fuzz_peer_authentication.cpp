// SPDX-License-Identifier: MIT
#include "peer_authentication.hpp"

#include <algorithm>
#include <cstdlib>

extern "C" int LLVMFuzzerTestOneInput(
    const std::uint8_t* data, std::size_t size)
{
    namespace pa = robotweax::srt::peer_authentication;
    const auto input = std::as_bytes(std::span {data, size});
    auto& provider = robotweax::srt::default_crypto_provider();
    if (input.size() >= 64) {
        (void)provider.validate_p256_public_key(input.first<64>());
        if (input.size() >= 128)
            (void)provider.verify_p256_sha256(
                input.first<64>(), input.subspan(128), input.subspan<64, 64>());
    }
    pa::Record record;
    if (pa::decode_record(input, record) == robotweax::srt::Error::none) {
        std::array<std::byte, 232> encoded {};
        std::size_t written = 0;
        if (pa::encode_record(record, encoded, written)
                != robotweax::srt::Error::none
            || written != input.size()
            || !std::equal(input.begin(), input.end(), encoded.begin()))
            std::abort();
    }
    if (size < 4)
        return 0;
    const auto first = (std::size_t(data[0]) << 8U) | data[1];
    const auto second = (std::size_t(data[2]) << 8U) | data[3];
    if (first > size - 4 || second > size - 4 - first)
        return 0;
    const auto m1 = input.subspan(4, first);
    const auto m2 = input.subspan(4 + first, second);
    const auto m3 = input.subspan(4 + first + second);
    pa::Digest output {};
    (void)pa::listener_transcript(provider, m1, m2, output);
    (void)pa::caller_transcript(provider, m1, m2, m3, output);
    return 0;
}
