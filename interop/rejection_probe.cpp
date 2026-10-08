// Independently authored black-box probe using only the public SRT C API.
// Compile this same source against each library being compared.
#include "srt.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

namespace {
using Clock = std::chrono::steady_clock;

bool parse_number(std::string_view text, int minimum, int maximum, int& value)
{
    const auto parsed =
        std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc {} && parsed.ptr == text.data() + text.size()
        && value >= minimum && value <= maximum;
}

bool checked(int result, const char* operation)
{
    if (result != SRT_ERROR) {
        return true;
    }
    // Do not print library error strings that might include configuration data.
    std::cerr << operation << " failed: error=" << srt_getlasterror(nullptr)
              << '\n';
    return false;
}

struct Runtime {
    bool active = false;
    ~Runtime()
    {
        if (active) {
            (void)srt_cleanup();
        }
    }
};

struct Attempt {
    SRTSOCKET socket = SRT_INVALID_SOCK;
    int poll = -1;
    bool registered = false;
    ~Attempt()
    {
        if (poll >= 0) {
            (void)srt_epoll_release(poll);
        }
        if (socket != SRT_INVALID_SOCK) {
            (void)srt_close(socket);
        }
    }
    bool close()
    {
        if (registered
            && !checked(srt_epoll_remove_usock(poll, socket), "epoll remove")) {
            return false;
        }
        registered = false;
        if (!checked(srt_close(socket), "socket close")) {
            return false;
        }
        socket = SRT_INVALID_SOCK;
        if (!checked(srt_epoll_release(poll), "epoll release")) {
            return false;
        }
        poll = -1;
        return true;
    }
};

int run(int argc, char** argv)
{
    int port = 0;
    int attempts = 0;
    int expected_reason = 0;
    int retry_delay = 0;
    int message_api = 1;
    if (argc < 6 || argc > 8 || std::string_view {argv[1]} != "127.0.0.1"
        || !parse_number(argv[2], 1, 65'535, port)
        || !parse_number(argv[3], 1, 10'000, attempts)
        || !parse_number(argv[4], 1, 9'999, expected_reason)
        || !parse_number(argv[5], 0, 60'000, retry_delay)
        || (argc == 8 && !parse_number(argv[7], 0, 1, message_api))) {
        std::cerr
            << "usage: rejection_probe 127.0.0.1 PORT ATTEMPTS EXPECTED_REJECT"
               " RETRY_DELAY_MS [PASSPHRASE_ENV|- [MESSAGE_API_0_OR_1]]\n";
        return 2;
    }
    const char* secret = nullptr;
    if (argc >= 7 && std::string_view {argv[6]} != "-") {
        secret = std::getenv(argv[6]);
        if (secret == nullptr || std::strlen(secret) < 10
            || std::strlen(secret) > 79) {
            std::cerr << "passphrase environment variable must contain 10..79 "
                         "bytes\n";
            return 2;
        }
    }
    Runtime runtime;
    if (!checked(srt_startup(), "startup")) {
        return 1;
    }
    runtime.active = true;
    srt_setloglevel(LOG_CRIT);
    sockaddr_in peer {};
    peer.sin_family = AF_INET;
    peer.sin_port = htons(static_cast<unsigned short>(port));
    peer.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    std::vector<long long> durations;
    durations.reserve(static_cast<std::size_t>(attempts));
    std::cerr << "library_version=" << srt_getversion()
              << " attempts=" << attempts << " retry_delay_ms=" << retry_delay
              << '\n';
    std::cout << "attempt,connect_result,connect_error,wait_result,wait_error,"
                 "fd_matches,"
                 "events,state,reject_reason,connect_to_ready_us,validated\n";
    const auto campaign_start = Clock::now();
    for (int i = 0; i < attempts; ++i) {
        Attempt attempt;
        attempt.socket = srt_create_socket();
        if (attempt.socket == SRT_INVALID_SOCK) {
            std::cerr << "socket creation failed\n";
            return 1;
        }
        const bool synchronous = false;
        const bool messages = message_api != 0;
        const int connect_timeout = 3'000;
        if (!checked(srt_setsockflag(attempt.socket, SRTO_RCVSYN, &synchronous,
                         sizeof(synchronous)),
                "RCVSYN")
            || !checked(srt_setsockflag(attempt.socket, SRTO_SNDSYN,
                            &synchronous, sizeof(synchronous)),
                "SNDSYN")
            || !checked(srt_setsockflag(attempt.socket, SRTO_MESSAGEAPI,
                            &messages, sizeof(messages)),
                "MESSAGEAPI")
            || !checked(srt_setsockflag(attempt.socket, SRTO_CONNTIMEO,
                            &connect_timeout, sizeof(connect_timeout)),
                "CONNTIMEO")
            || (secret != nullptr
                && !checked(srt_setsockflag(attempt.socket, SRTO_PASSPHRASE,
                                secret, static_cast<int>(std::strlen(secret))),
                    "PASSPHRASE"))) {
            return 1;
        }
        attempt.poll = srt_epoll_create();
        if (!checked(attempt.poll, "epoll create")) {
            return 1;
        }
        const int interests = SRT_EPOLL_OUT | SRT_EPOLL_ERR;
        if (!checked(
                srt_epoll_add_usock(attempt.poll, attempt.socket, &interests),
                "epoll add")) {
            return 1;
        }
        attempt.registered = true;
        SRT_EPOLL_EVENT ready {};
        const auto start = Clock::now();
        const int connected = srt_connect(attempt.socket,
            reinterpret_cast<const sockaddr*>(&peer), sizeof(peer));
        const int connect_error =
            connected == SRT_ERROR ? srt_getlasterror(nullptr) : 0;
        const int waited = connected == 0
            ? srt_epoll_uwait(attempt.poll, &ready, 1, 5'000)
            : 0;
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::microseconds>(
                Clock::now() - start)
                .count();
        const int wait_error =
            waited == SRT_ERROR ? srt_getlasterror(nullptr) : 0;
        const auto state = srt_getsockstate(attempt.socket);
        const int reason = srt_getrejectreason(attempt.socket);
        const bool matching = waited == 1 && ready.fd == attempt.socket;
        const bool valid = connected == 0 && matching
            && (ready.events & SRT_EPOLL_ERR) != 0 && state == SRTS_BROKEN
            && reason == expected_reason;
        std::cout << i + 1 << ',' << connected << ',' << connect_error << ','
                  << waited << ',' << wait_error << ',' << matching << ','
                  << ready.events << ',' << state << ',' << reason << ','
                  << elapsed << ',' << valid << '\n'
                  << std::flush;
        if (!attempt.close() || !valid) {
            std::cerr << "attempt " << i + 1
                      << " failed validation; no timing summary\n";
            return 1;
        }
        durations.push_back(elapsed);
        if (i + 1 < attempts && retry_delay != 0) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds {retry_delay});
        }
    }
    const auto campaign_seconds =
        std::chrono::duration<double>(Clock::now() - campaign_start).count();
    if (!checked(srt_cleanup(), "cleanup")) {
        return 1;
    }
    runtime.active = false;
    std::sort(durations.begin(), durations.end());
    const auto count = durations.size();
    std::cerr << "validated=" << count << " min_us=" << durations.front()
              << " median_us=" << durations[count / 2]
              << " p90_us=" << durations[(count - 1) * 9 / 10]
              << " max_us=" << durations.back()
              << " campaign_seconds=" << campaign_seconds
              << " attempts_per_second=" << count / campaign_seconds << '\n';
    return 0;
}
} // namespace

int main(int argc, char** argv)
{
    try {
        return run(argc, argv);
    } catch (...) {
        std::cerr << "probe failed; no timing summary\n";
        return 1;
    }
}
