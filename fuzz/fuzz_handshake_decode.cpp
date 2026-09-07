#include "robotweax/srt/handshake.hpp"
#include "robotweax/srt/handshake_extensions.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    const auto input = std::as_bytes(std::span{data, size});
    const auto decoded = robotweax::srt::decode_handshake_payload(input);
    if (decoded) {
        std::byte output[robotweax::srt::handshake_size];
        (void)robotweax::srt::encode_handshake(decoded.handshake, output);
        auto extensions = decoded.extensions;
        while (!extensions.empty()) {
            const auto extension = robotweax::srt::decode_extension(extensions);
            if (!extension) {
                break;
            }
            if (extension.extension.type
                    == robotweax::srt::HandshakeExtensionType::handshake_request
                || extension.extension.type
                    == robotweax::srt::HandshakeExtensionType::handshake_response) {
                (void)robotweax::srt::decode_handshake_parameters(extension.extension);
            }
            extensions = extensions.subspan(extension.bytes_consumed);
        }
    }
    return 0;
}
