/* SPDX-License-Identifier: MIT */

#include <srt/srt.h>

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

std::runtime_error srt_error(const std::string& operation)
{
    return std::runtime_error(operation + ": " + srt_getlasterror_str());
}

class SrtRuntime final {
public:
    SrtRuntime()
    {
        if (srt_startup() == SRT_ERROR) {
            throw srt_error("srt_startup failed");
        }
    }

    ~SrtRuntime()
    {
        (void)srt_cleanup();
    }

    SrtRuntime(const SrtRuntime&) = delete;
    SrtRuntime& operator=(const SrtRuntime&) = delete;
};

class SrtSocket final {
public:
    SrtSocket()
        : socket_(srt_create_socket())
    {
        if (socket_ == SRT_INVALID_SOCK) {
            throw srt_error("srt_create_socket failed");
        }
    }

    ~SrtSocket()
    {
        if (socket_ != SRT_INVALID_SOCK) {
            (void)srt_close(socket_);
        }
    }

    SrtSocket(const SrtSocket&) = delete;
    SrtSocket& operator=(const SrtSocket&) = delete;

    SRTSOCKET get() const
    {
        return socket_;
    }

private:
    SRTSOCKET socket_;
};

std::string version_string(std::uint32_t version)
{
    return std::to_string((version >> 16U) & 0xffU) + "."
        + std::to_string((version >> 8U) & 0xffU) + "."
        + std::to_string(version & 0xffU);
}

} // namespace

int main()
{
    try {
        SrtRuntime runtime;
        SrtSocket socket;

        constexpr int requested_latency_milliseconds = 120;
        if (srt_setsockflag(socket.get(), SRTO_LATENCY,
                &requested_latency_milliseconds,
                static_cast<int>(sizeof(requested_latency_milliseconds)))
            == SRT_ERROR) {
            throw srt_error("setting SRTO_LATENCY failed");
        }

        int reported_latency_milliseconds = 0;
        int option_size =
            static_cast<int>(sizeof(reported_latency_milliseconds));
        if (srt_getsockflag(socket.get(), SRTO_LATENCY,
                &reported_latency_milliseconds, &option_size)
            == SRT_ERROR) {
            throw srt_error("reading SRTO_LATENCY failed");
        }
        if (option_size
                != static_cast<int>(sizeof(reported_latency_milliseconds))
            || reported_latency_milliseconds
                != requested_latency_milliseconds) {
            throw std::runtime_error("SRTO_LATENCY did not round-trip");
        }

        const std::uint32_t runtime_version = srt_getversion();
        if (runtime_version != SRT_VERSION_VALUE) {
            throw std::runtime_error("the compile-time and runtime SRT "
                                     "compatibility versions differ");
        }

        std::cout << "Robotweax SRT installed consumer OK\n"
                  << "SRT compatibility version: "
                  << version_string(runtime_version) << '\n'
                  << "configured latency: " << reported_latency_milliseconds
                  << " ms\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
