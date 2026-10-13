// SPDX-License-Identifier: MIT
#include "robotweax/srt/codec.hpp"
#include "robotweax/srt/session_authentication.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

using namespace robotweax::srt;
extern "C" int LLVMFuzzerTestOneInput(
    const std::uint8_t* data, std::size_t size)
{
    if (size > 1500)
        return 0;
    for (const auto version : {acp1_session_version,
             acp1_rendezvous_session_version, acp1_group_session_version}) {
        SessionAuthentication caller(default_crypto_provider()),
            listener(default_crypto_provider());
        constexpr std::string_view secret = "synthetic-acp1-fuzz-password";
        const auto pass =
            std::as_bytes(std::span {secret.data(), secret.size()});
        const bool rendezvous = version == acp1_rendezvous_session_version;
        const bool group = version == acp1_group_session_version;
        std::array<std::byte, acp1_group_context_size> context {};
        const auto binding = group ? std::span<const std::byte> {context}
                                   : std::span<const std::byte> {};
        if (caller.start(true, version) != Error::none
            || listener.start(false, version) != Error::none
            || listener.establish(pass, caller.parameters(), 102, 101,
                   rendezvous ? 100U : 0U, rendezvous ? 200U : 0U, binding)
                != Error::none
            || caller.establish(pass, listener.parameters(), 101, 102,
                   rendezvous ? 200U : 0U, rendezvous ? 100U : 0U, binding)
                != Error::none)
            return 0;
        const auto input = std::as_bytes(std::span {data, size});
        PacketView opened;
        const auto raw = decode_packet(input);
        if (raw)
            (void)listener.open_control(raw.packet, opened);
        std::array<std::byte, 1500> wire {};
        std::size_t written = 0;
        if (caller.seal_control(input, wire, written) == Error::none) {
            const auto decoded = decode_packet(std::span {wire}.first(written));
            if (decoded) {
                (void)listener.open_control(decoded.packet, opened);
                (void)listener.open_control(decoded.packet, opened);
            }
        }
    }
    return 0;
}
