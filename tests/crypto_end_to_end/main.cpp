// SPDX-License-Identifier: MIT
// Manual Windows loopback diagnostic. No fixed performance thresholds.
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <ctime>
#endif
#include <srt/srt.h>
#include <array>
#include <chrono>
#include <cstring>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

void require(bool ok)
{
    if (!ok)
        throw std::runtime_error(srt_getlasterror_str());
}
struct Socket {
    SRTSOCKET id = SRT_INVALID_SOCK;
    ~Socket()
    {
        if (id != SRT_INVALID_SOCK)
            srt_close(id);
    }
};
template <class T> void option(SRTSOCKET s, SRT_SOCKOPT name, T value)
{
    require(srt_setsockflag(s, name, &value, sizeof(value)) == 0);
}
double cpu_seconds()
{
#ifdef _WIN32
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(
            GetCurrentProcess(), &created, &exited, &kernel, &user))
        throw std::runtime_error("GetProcessTimes failed");
    const auto ticks = [](FILETIME t) {
        return (static_cast<unsigned long long>(t.dwHighDateTime) << 32)
            | t.dwLowDateTime;
    };
    return (ticks(kernel) + ticks(user)) / 1e7;
#else
    return static_cast<double>(std::clock()) / CLOCKS_PER_SEC;
#endif
}
void configure(SRTSOCKET s, int mode)
{
    require(s != SRT_INVALID_SOCK);
    option(s, SRTO_TRANSTYPE, SRTT_FILE);
    option(s, SRTO_MESSAGEAPI, false);
    option(s, SRTO_SNDTIMEO, 10000);
    option(s, SRTO_RCVTIMEO, 10000);
    option(s, SRTO_CONNTIMEO, 10000);
    if (mode) {
        const char key[] = "synthetic-benchmark-only";
        require(srt_setsockflag(s, SRTO_PASSPHRASE, key, sizeof(key) - 1) == 0);
        option(s, SRTO_PBKEYLEN, 32);
        option(s, SRTO_CRYPTOMODE, mode);
    }
}
int main(int argc, char** argv)
{
    try {
        if (argc != 2)
            throw std::runtime_error("usage: crypto_end_to_end 0|1|2");
        const int mode = std::stoi(argv[1]);
        if (mode < 0 || mode > 2)
            throw std::runtime_error("invalid mode");
        require(srt_startup() == 0);
        {
            Socket listener {srt_create_socket()}, sender {srt_create_socket()};
            configure(listener.id, mode);
            configure(sender.id, mode);
            sockaddr_in address {};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            require(srt_bind(listener.id, reinterpret_cast<sockaddr*>(&address),
                        sizeof(address))
                == 0);
            int length = sizeof(address);
            require(srt_getsockname(listener.id,
                        reinterpret_cast<sockaddr*>(&address), &length)
                == 0);
            require(srt_listen(listener.id, 1) == 0);
            require(srt_connect(sender.id,
                        reinterpret_cast<sockaddr*>(&address), sizeof(address))
                == 0);
            Socket receiver {srt_accept(listener.id, nullptr, nullptr)};
            require(receiver.id != SRT_INVALID_SOCK);
            option(receiver.id, SRTO_RCVTIMEO, 10000);
            if (mode) {
                int observed = -1, size = sizeof(observed);
                require(srt_getsockflag(
                            receiver.id, SRTO_CRYPTOMODE, &observed, &size)
                    == 0);
                if (observed != mode)
                    throw std::runtime_error("crypto mode mismatch");
            }
            constexpr unsigned packets = 131072, payload = 1316;
            std::exception_ptr send_error;
            const double cpu_start = cpu_seconds();
            const auto start = std::chrono::steady_clock::now();
            std::jthread writer([&] {
                try {
                    std::array<char, payload> data;
                    for (unsigned n = 0; n < packets; ++n) {
                        for (unsigned i = 0; i < payload; ++i)
                            data[i] =
                                static_cast<char>((n * 17 + i * 31) & 255);
                        // Include the complete sequence, not only its low byte.
                        std::memcpy(data.data(), &n, sizeof(n));
                        for (unsigned offset = 0; offset < payload;) {
                            const int sent = srt_send(sender.id,
                                data.data() + offset, payload - offset);
                            require(sent > 0);
                            offset += static_cast<unsigned>(sent);
                        }
                    }
                } catch (...) {
                    send_error = std::current_exception();
                }
            });
            std::array<char, payload> data, expected;
            for (unsigned n = 0; n < packets; ++n) {
                for (unsigned offset = 0; offset < payload;) {
                    const int received = srt_recv(
                        receiver.id, data.data() + offset, payload - offset);
                    require(received > 0);
                    offset += static_cast<unsigned>(received);
                }
                for (unsigned i = 0; i < payload; ++i)
                    expected[i] = static_cast<char>((n * 17 + i * 31) & 255);
                std::memcpy(expected.data(), &n, sizeof(n));
                if (data != expected)
                    throw std::runtime_error("payload integrity mismatch");
            }
            writer.join();
            if (send_error)
                std::rethrow_exception(send_error);
            const double wall = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - start)
                                    .count();
            const double cpu = cpu_seconds() - cpu_start;
            std::cout << "{\"mode\":" << mode
                      << ",\"bytes\":" << packets * payload
                      << ",\"wall_seconds\":" << wall
                      << ",\"process_cpu_seconds\":" << cpu
                      << ",\"payload_mbps\":"
                      << packets * payload * 8.0 / wall / 1e6
                      << ",\"cpu_core_equivalents\":" << cpu / wall
                      << ",\"integrity\":true}\n";
        }
        srt_cleanup();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
