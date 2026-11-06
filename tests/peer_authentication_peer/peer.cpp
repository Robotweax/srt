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

namespace {
// Standard P-256 generator, public key for the synthetic scalar 1.
constexpr std::array<unsigned char, 64> generator {0x6b, 0x17, 0xd1, 0xf2, 0xe1,
    0x2c, 0x42, 0x47, 0xf8, 0xbc, 0xe6, 0xe5, 0x63, 0xa4, 0x40, 0xf2, 0x77,
    0x03, 0x7d, 0x81, 0x2d, 0xeb, 0x33, 0xa0, 0xf4, 0xa1, 0x39, 0x45, 0xd8,
    0x98, 0xc2, 0x96, 0x4f, 0xe3, 0x42, 0xe2, 0xfe, 0x1a, 0x7f, 0x9b, 0x8e,
    0xe7, 0xeb, 0x4a, 0x7c, 0x0f, 0x9e, 0x16, 0x2b, 0xce, 0x33, 0x57, 0x6b,
    0x31, 0x5e, 0xce, 0xcb, 0xb6, 0x40, 0x68, 0x37, 0xbf, 0x51, 0xf5};
bool authenticated_identity(SRTSOCKET socket)
{
    std::array<unsigned char, 64> key {};
    int size = key.size();
    return srt_getsockflag(
               socket, static_cast<SRT_SOCKOPT>(0x0100001d), key.data(), &size)
        == 0
        && size == static_cast<int>(key.size()) && key == generator;
}
bool exchange_payload(SRTSOCKET socket, bool caller)
{
    const bool synchronous = true;
    if (srt_setsockflag(socket, SRTO_RCVSYN, &synchronous, sizeof(synchronous))
        != 0)
        return false;
    constexpr std::string_view probe = "probe";
    std::array<char, 1500> received {};
    if (caller && srt_sendmsg(socket, probe.data(), probe.size(), -1, 0) != 5)
        return false;
    if (srt_recvmsg(socket, received.data(), received.size()) != 5
        || std::string_view(received.data(), 5) != probe)
        return false;
    if (caller)
        return true;
    if (srt_sendmsg(socket, probe.data(), probe.size(), -1, 0) != 5)
        return false;
    // Keep the listener alive until the caller has received the echo and closed.
    return srt_recvmsg(socket, received.data(), received.size()) == SRT_ERROR
        && srt_getlasterror(nullptr) == SRT_ECONNLOST;
}
}

int main(int argc, char** argv)
{
    if (argc != 4)
        return 64;
    const bool listener = std::string_view(argv[1]) == "listener";
    const std::string_view mode = argv[3];
    if (mode != "legacy" && mode != "required" && mode != "session"
        && mode != "unavailable")
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
    if (mode == "unavailable") {
        if (socket == SRT_INVALID_SOCK)
            return finish(3);
        for (unsigned option = 0x01000019; option <= 0x0100001d; ++option) {
            std::array<unsigned char, 64> value {};
            value.back() = 1;
            int size = value.size();
            if (srt_getsockflag(socket, static_cast<SRT_SOCKOPT>(option),
                    value.data(), &size)
                    != SRT_ERROR
                || srt_getlasterror(nullptr) != SRT_EINVOP)
                return finish(3);
            const int written_size = option == 0x01000019 ? 4
                : option == 0x0100001a                    ? 32
                                                          : 64;
            if (srt_setsockflag(socket, static_cast<SRT_SOCKOPT>(option),
                    value.data(), written_size)
                    != SRT_ERROR
                || srt_getlasterror(nullptr) != SRT_EINVOP)
                return finish(3);
        }
        std::puts("UNAVAILABLE");
        return finish(0);
    }
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
        if (result != 0)
            return finish(2);
        if ((mode == "required" && !authenticated_identity(socket))
            || !exchange_payload(socket, true))
            return finish(3);
        return finish(0);
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
            const bool passed =
                (mode != "required" || authenticated_identity(child))
                && exchange_payload(child, false);
            (void)srt_close(child);
            return finish(passed ? 0 : 3);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds {5});
    }
    std::puts("NOT_ACCEPTED");
    return finish(2);
}
