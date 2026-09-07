#include "robotweax/srt/connection_group.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

using namespace robotweax::srt;

extern "C" int LLVMFuzzerTestOneInput(
    const std::uint8_t* data, std::size_t size)
{
    const auto bytes = std::as_bytes(
        std::span{data, size});

    const auto direct = decode_group_membership({
        .type = HandshakeExtensionType::group,
        .content = bytes,
    });
    if (direct) {
        (void)validate_group_membership(
            direct.membership);
    }

    const auto extension = decode_extension(bytes);
    if (extension
        && extension.extension.type
            == HandshakeExtensionType::group) {
        const auto decoded = decode_group_membership(
            extension.extension);
        if (decoded) {
            (void)validate_group_membership(
                decoded.membership);
        }
    }
    return 0;
}
