#include "robotweax/srt/crypto.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(
    const std::uint8_t* data, std::size_t size)
{
    const auto bytes = std::span{
        reinterpret_cast<const std::byte*>(data), size};
    const auto decoded = robotweax::srt::decode_key_material(bytes);
    if (decoded) {
        (void)decoded.key_material.key_count();
        (void)decoded.key_material.salt.size();
        (void)decoded.key_material.wrapped_keys.size();
    }

    const auto selected_mode = size % 3U == 0U
        ? robotweax::srt::CryptoMode::automatic
        : (size % 3U == 1U ? robotweax::srt::CryptoMode::aes_ctr
                           : robotweax::srt::CryptoMode::aes_gcm);
    robotweax::srt::CryptoSession receiver {{
        .passphrase = "malformed key-material fuzz fixture",
        .mode = selected_mode,
        .enable_aes_gcm = true,
    }};
    const auto result = receiver.accept_key_material(bytes, false);
    if (result == robotweax::srt::Error::none && !decoded) {
        __builtin_trap();
    }
    return 0;
}
