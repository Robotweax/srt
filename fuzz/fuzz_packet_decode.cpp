#include "robotweax/srt/codec.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    const auto bytes = std::as_bytes(std::span{data, size});
    const auto packet = robotweax::srt::decode_packet(bytes);
    if (packet) {
        volatile std::size_t payload_size = packet.packet.payload.size();
        (void)payload_size;
    }
    return 0;
}
