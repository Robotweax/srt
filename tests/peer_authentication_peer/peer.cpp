// SPDX-License-Identifier: MIT
// Independently authored black-box C API fixture. Synthetic test identity only.
#ifdef PEERAUTH_REFERENCE
#include "srt.h"
#else
#include "srt/srt.h"
#endif
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <thread>

int main(int argc, char** argv)
{
    if (argc != 4)
        return 64;
    const bool listener = std::string_view(argv[1]) == "listener";
    const std::string_view mode = argv[3];
    if (mode != "legacy" && mode != "required" && mode != "session")
        return 64;
    if (srt_startup() != 0)
        return 3;
    const auto socket = srt_create_socket();
    auto set = [&](SRT_SOCKOPT option, int value) {
        return srt_setsockflag(socket, option, &value, sizeof(value)) == 0;
    };
    const auto finish = [&](int result) {
        (void)srt_close(socket);
        (void)srt_cleanup();
        return result;
    };
    constexpr std::string_view password = "black-box qualification fixture";
    if (socket == SRT_INVALID_SOCK
        || srt_setsockflag(
               socket, SRTO_PASSPHRASE, password.data(), password.size())
            != 0
        || !set(static_cast<SRT_SOCKOPT>(62), 2) || !set(SRTO_PBKEYLEN, 16)
        || !set(SRTO_CONNTIMEO, 1000) || !set(SRTO_RCVTIMEO, 1000))
        return finish(3);
    if (mode != "legacy") {
        if (!set(static_cast<SRT_SOCKOPT>(0x01000003), 1))
            return finish(3);
    }
    if (mode == "required") {
        std::array<unsigned char, 32> scalar {};
        scalar.back() = 1;
        // Standard P-256 generator, public key for the synthetic scalar 1.
        constexpr std::array<unsigned char, 64> generator {0x6b, 0x17, 0xd1,
            0xf2, 0xe1, 0x2c, 0x42, 0x47, 0xf8, 0xbc, 0xe6, 0xe5, 0x63, 0xa4,
            0x40, 0xf2, 0x77, 0x03, 0x7d, 0x81, 0x2d, 0xeb, 0x33, 0xa0, 0xf4,
            0xa1, 0x39, 0x45, 0xd8, 0x98, 0xc2, 0x96, 0x4f, 0xe3, 0x42, 0xe2,
            0xfe, 0x1a, 0x7f, 0x9b, 0x8e, 0xe7, 0xeb, 0x4a, 0x7c, 0x0f, 0x9e,
            0x16, 0x2b, 0xce, 0x33, 0x57, 0x6b, 0x31, 0x5e, 0xce, 0xcb, 0xb6,
            0x40, 0x68, 0x37, 0xbf, 0x51, 0xf5};
        if (srt_setsockflag(socket, static_cast<SRT_SOCKOPT>(0x0100001a),
                scalar.data(), scalar.size())
                != 0
            || srt_setsockflag(socket, static_cast<SRT_SOCKOPT>(0x0100001b),
                   generator.data(), generator.size())
                != 0
            || !set(static_cast<SRT_SOCKOPT>(0x01000019), 1))
            return finish(3);
    }
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<unsigned short>(std::atoi(argv[2])));
    if (!listener) {
        const auto result = srt_connect(socket,
            reinterpret_cast<const sockaddr*>(&address), sizeof(address));
        std::printf("CONNECT %d ERROR %d\n", result,
            result == 0 ? 0 : srt_getlasterror(nullptr));
        return finish(result == 0 ? 0 : 2);
    }
    const bool synchronous = false;
    if (srt_setsockflag(socket, SRTO_RCVSYN, &synchronous, sizeof(synchronous))
            != 0
        || srt_bind(socket, reinterpret_cast<const sockaddr*>(&address),
               sizeof(address))
            != 0
        || srt_listen(socket, 4) != 0)
        return finish(3);
    std::puts("READY");
    std::fflush(stdout);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds {3};
    while (std::chrono::steady_clock::now() < deadline) {
        const auto child = srt_accept(socket, nullptr, nullptr);
        if (child != SRT_INVALID_SOCK) {
            std::puts("ACCEPTED");
            (void)srt_close(child);
            return finish(0);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds {5});
    }
    std::puts("NOT_ACCEPTED");
    return finish(2);
}
