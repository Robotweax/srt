#include "null_packet_deletion.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

using namespace robotweax::srt;

extern "C" int LLVMFuzzerTestOneInput(
    const std::uint8_t* data, std::size_t size)
{
    const auto bytes = std::as_bytes(std::span {data, size});
    (void)npd::decode_record(bytes);
    std::array<std::byte, 1505> encoded {};
    std::array<std::byte, 1504> restored {};
    // Arbitrary malformed prefixes, also exercising insufficient output capacity.
    (void)npd::decode_payload(bytes, restored);
    if (size != 0) {
        const auto capacity = static_cast<std::size_t>(data[0]) * 5;
        const auto sentinel = std::byte {0xa5};
        restored.fill(sentinel);
        const auto result =
            npd::decode_payload(bytes, std::span {restored}.first(capacity));
        if (!result
            && !std::all_of(
                restored.begin(), restored.end(), [sentinel](std::byte value) {
                    return value == sentinel;
                })) {
            __builtin_trap();
        }
    }
    const auto source = bytes.first(std::min(size, restored.size()));
    const auto result = npd::encode_payload(source, encoded);
    if (result) {
        const auto decoded = npd::decode_payload(
            std::span {encoded}.first(result.bytes_written), restored);
        if (!decoded || decoded.bytes_written != source.size()
            || decoded.null_units != result.null_units
            || !std::equal(source.begin(), source.end(), restored.begin())) {
            __builtin_trap();
        }
    } else if (!source.empty()) {
        __builtin_trap();
    }
    return 0;
}
