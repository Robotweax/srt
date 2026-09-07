/* SPDX-License-Identifier: MIT */

// A separate process interposes only SRT's RAND_bytes calls. No production
// provider hook or process-global test override is added to the library.
#include "srt.h"
#include "robotweax/srt/codec.hpp"
#include "compat/socket_registry.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <thread>

namespace {
std::atomic<unsigned> key_random_calls {0};
std::atomic<unsigned> injected_failures {0};
unsigned fail_at = 0;

void require(bool value, const char* message)
{
    if (!value) {
        std::fprintf(stderr, "FAIL: %s (random calls=%u, injected=%u)\n",
            message, key_random_calls.load(), injected_failures.load());
        std::exit(1);
    }
}

struct Capture {
    std::shared_ptr<robotweax::srt::compat::DatagramChannel> channel;
    std::atomic<unsigned> plaintext_packets {0};
    ~Capture()
    {
        if (channel)
            channel->set_send_hook_for_testing(nullptr, nullptr);
    }
};

robotweax::srt::UdpIoResult capture(std::span<const std::byte> bytes,
    robotweax::srt::IpEndpoint peer, void* context) noexcept
{
    using namespace robotweax::srt;
    auto& observation = *static_cast<Capture*>(context);
    const auto packet = decode_packet(bytes);
    if (packet && packet.packet.kind == PacketKind::data
        && packet.packet.data.encryption_key == EncryptionKey::none) {
        ++observation.plaintext_packets;
    }
    return observation.channel->socket.send_to(bytes, peer);
}
} // namespace

extern "C" int RAND_bytes(unsigned char* bytes, int length)
{
    using Function = int (*)(unsigned char*, int);
    static auto real =
        reinterpret_cast<Function>(dlsym(RTLD_NEXT, "RAND_bytes"));
    // The probe uses AES-128: salt and key requests are 16 bytes. Counting is
    // restarted after listener setup, excluding its cookie-secret generation.
    if (length == 16 && ++key_random_calls == fail_at) {
        ++injected_failures;
        return 0;
    }
    return real ? real(bytes, length) : 0;
}

int main(int argc, char** argv)
{
    require(argc == 4, "expected rendezvous, failure-index, and GCM arguments");
    const bool rendezvous = std::atoi(argv[1]) != 0;
    fail_at = static_cast<unsigned>(std::atoi(argv[2]));
    const bool gcm = std::atoi(argv[3]) != 0;
    const bool optional_fallback = fail_at == 99;
    require(srt_startup() == 0, "startup");
    const auto first = srt_create_socket();
    const auto second = srt_create_socket();
    require(first != SRT_INVALID_SOCK && second != SRT_INVALID_SOCK, "sockets");
    constexpr bool enforced = false;
    constexpr int timeout = 500;
    constexpr int key_length = 16;
    constexpr char secret[] = "public setup fault fixture";
    Capture first_capture, second_capture;
    sockaddr_in addresses[2] {};
    const SRTSOCKET sockets[] {first, second};
    Capture* captures[] {&first_capture, &second_capture};
    for (unsigned index = 0; index < 2; ++index) {
        const auto socket = sockets[index];
        if (!optional_fallback || index == 0) {
            require(srt_setsockflag(
                        socket, SRTO_PASSPHRASE, secret, sizeof(secret) - 1)
                    == 0,
                "passphrase");
        }
        require(srt_setsockflag(socket, SRTO_ENFORCEDENCRYPTION, &enforced,
                    sizeof(enforced))
                == 0,
            "optional encryption");
        require(srt_setsockflag(
                    socket, SRTO_PBKEYLEN, &key_length, sizeof(key_length))
                == 0,
            "key length");
        require(
            srt_setsockflag(socket, SRTO_CONNTIMEO, &timeout, sizeof(timeout))
                == 0,
            "timeout");
        if (rendezvous) {
            require(srt_setsockflag(socket, SRTO_RENDEZVOUS, &rendezvous,
                        sizeof(rendezvous))
                    == 0,
                "rendezvous");
        }
#ifdef ENABLE_AEAD_API_PREVIEW
        if (gcm) {
            const int mode = 2;
            require(
                srt_setsockflag(socket, SRTO_CRYPTOMODE, &mode, sizeof(mode))
                    == 0,
                "GCM mode");
        }
#else
        require(!gcm, "GCM requires preview build");
#endif
        addresses[index].sin_family = AF_INET;
        addresses[index].sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        require(srt_bind(socket, reinterpret_cast<sockaddr*>(&addresses[index]),
                    sizeof(addresses[index]))
                == 0,
            "bind");
        int size = sizeof(addresses[index]);
        require(srt_getsockname(socket,
                    reinterpret_cast<sockaddr*>(&addresses[index]), &size)
                == 0,
            "socket address");
        auto record =
            robotweax::srt::compat::SocketRegistry::instance().find(socket);
        require(record != nullptr, "socket record");
        captures[index]->channel = record->channel;
        captures[index]->channel->set_send_hook_for_testing(
            capture, captures[index]);
    }
    int second_result = SRT_ERROR;
    std::thread other;
    key_random_calls = 0;
    if (rendezvous) {
        other = std::thread([&] {
            second_result =
                srt_connect(second, reinterpret_cast<sockaddr*>(&addresses[0]),
                    sizeof(addresses[0]));
        });
    } else {
        require(srt_listen(second, 4) == 0, "listen");
        key_random_calls = 0;
    }
    const int result = srt_connect(first,
        reinterpret_cast<sockaddr*>(&addresses[1]), sizeof(addresses[1]));
    if (other.joinable())
        other.join();
    if (fail_at != 0 && !optional_fallback) {
        require(injected_failures == 1, "fault point must be reached");
        // Whichever RV role was selected must reject the local setup failure.
        // Its peer may briefly finish transport setup, but may not send clear DATA.
        require(
            result == SRT_ERROR || (rendezvous && second_result == SRT_ERROR),
            "local crypto failure must reject connection setup");
        (void)srt_sendmsg(first, "probe", 5, -1, 1);
        if (rendezvous)
            (void)srt_sendmsg(second, "probe", 5, -1, 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        require(first_capture.plaintext_packets == 0
                && second_capture.plaintext_packets == 0,
            "local key preparation failure must not emit plaintext DATA");
    } else {
        require(result == 0 && (!rendezvous || second_result == 0),
            "baseline connect");
        if (optional_fallback) {
            int state = -1;
            int size = sizeof(state);
            require(srt_getsockflag(first, SRTO_SNDKMSTATE, &state, &size) == 0
                    && state == 0,
                "original optional fallback remains available");
        } else {
            require(key_random_calls == 6,
                "initial plus two directional key pairs");
        }
    }
    srt_close(first);
    srt_close(second);
    srt_cleanup();
    std::puts("PASS public crypto setup fault probe");
}
