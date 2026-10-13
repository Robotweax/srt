// SPDX-License-Identifier: MIT
#include <srt/srt.h>

#include <array>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void check(int result)
{
    if (result == SRT_ERROR)
        throw std::runtime_error(srt_getlasterror_str());
}
struct Socket {
    SRTSOCKET handle = SRT_INVALID_SOCK;
    ~Socket()
    {
        if (handle != SRT_INVALID_SOCK)
            (void)srt_close(handle);
    }
};
void set(SRTSOCKET socket, SRT_SOCKOPT option, std::int32_t value)
{
    check(srt_setsockflag(socket, option, &value, sizeof(value)));
}
constexpr unsigned messages = 512;
using Payload = std::array<char, 1316>;
Payload payload(unsigned message)
{
    Payload result {};
    for (unsigned i = 0; i < result.size(); ++i)
        result[i] = static_cast<char>((message * 17 + i * 31) & 255);
    return result;
}
void receive(SRTSOCKET socket, unsigned message)
{
    Payload bytes {};
    check(srt_recvmsg(socket, bytes.data(), bytes.size()));
    if (bytes != payload(message))
        throw std::runtime_error("payload integrity mismatch");
}
void send(SRTSOCKET socket, unsigned message)
{
    const auto bytes = payload(message);
    const int sent = srt_sendmsg(socket, bytes.data(), bytes.size(), -1, 1);
    check(sent);
    if (sent != static_cast<int>(bytes.size()))
        throw std::runtime_error("short message");
}
}

int main(int argc, char** argv)
{
    try {
        if (argc != 6)
            throw std::runtime_error(
                "usage: peer caller|listener port auth key-bytes ctr|gcm");
        check(srt_startup());
        const bool caller = std::string(argv[1]) == "caller";
        const bool authenticated = std::stoi(argv[3]) != 0;
        const bool gcm = std::string(argv[5]) == "gcm";
        const char* passphrase = std::getenv("SRT_INTEROP_PASSPHRASE");
        if (!passphrase || std::strlen(passphrase) < 10)
            throw std::runtime_error(
                "synthetic passphrase environment required");
        {
            Socket socket {srt_create_socket()};
            check(socket.handle);
            check(srt_setsockflag(socket.handle, SRTO_PASSPHRASE, passphrase,
                static_cast<int>(std::strlen(passphrase))));
            set(socket.handle, SRTO_PBKEYLEN, std::stoi(argv[4]));
            set(socket.handle, SRTO_ENFORCEDENCRYPTION, 1);
            set(socket.handle, SRTO_ROBOTWEAX_SESSIONAUTH,
                authenticated ? 1 : 0);
            set(socket.handle, SRTO_KMREFRESHRATE, 32);
            set(socket.handle, SRTO_KMPREANNOUNCE, 8);
            set(socket.handle, SRTO_RCVLATENCY, 0);
            set(socket.handle, SRTO_PEERLATENCY, 0);
            set(socket.handle, SRTO_CONNTIMEO, 6000);
            set(socket.handle, SRTO_RCVTIMEO, 6000);
            set(socket.handle, SRTO_SNDTIMEO, 6000);
            const linger draining {1, 2};
            check(srt_setsockflag(
                socket.handle, SRTO_LINGER, &draining, sizeof(draining)));
#ifdef ENABLE_AEAD_API_PREVIEW
            set(socket.handle, SRTO_CRYPTOMODE, gcm ? 2 : 1);
#else
            if (gcm)
                throw std::runtime_error("GCM preview required");
#endif
            sockaddr_in address {};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port =
                htons(static_cast<unsigned short>(std::stoi(argv[2])));
            Socket accepted;
            SRTSOCKET transport = socket.handle;
            if (caller) {
                check(srt_connect(socket.handle,
                    reinterpret_cast<const sockaddr*>(&address),
                    sizeof(address)));
            } else {
                check(srt_bind(socket.handle,
                    reinterpret_cast<const sockaddr*>(&address),
                    sizeof(address)));
                check(srt_listen(socket.handle, 1));
                std::cout << "ready\n" << std::flush;
                accepted.handle = srt_accept(socket.handle, nullptr, nullptr);
                check(accepted.handle);
                transport = accepted.handle;
            }
            for (unsigned n = 0; n < messages; ++n) {
                if (caller) {
                    send(transport, n);
                    receive(transport, n);
                } else {
                    receive(transport, n);
                    send(transport, n);
                }
            }
            std::int32_t state = 0;
            int state_size = sizeof(state);
            check(srt_getsockflag(
                transport, SRTO_SNDKMSTATE, &state, &state_size));
            if (state != SRT_KM_S_SECURED)
                throw std::runtime_error("sender keys are not secured");
#ifdef ENABLE_AEAD_API_PREVIEW
            state_size = sizeof(state);
            check(srt_getsockflag(
                transport, SRTO_CRYPTOMODE, &state, &state_size));
            if (state != (gcm ? 2 : 1))
                throw std::runtime_error("negotiated cipher mode mismatch");
#endif
            std::int32_t backend = 0;
            int size = sizeof(backend);
            check(srt_getsockflag(
                transport, SRTO_ROBOTWEAX_CRYPTO_BACKEND, &backend, &size));
            std::cout << "{\"backend\":" << backend
                      << ",\"messages\":" << messages << ",\"session_auth\":"
                      << (authenticated ? "true" : "false") << ",\"mode\":\""
                      << (gcm ? "gcm" : "ctr") << "\",\"integrity\":true}\n";
        }
        check(srt_cleanup());
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        (void)srt_cleanup();
        return 1;
    }
}
