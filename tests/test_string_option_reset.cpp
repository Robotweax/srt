#include "test.hpp"

#include "srt.h"

#include <array>
#include <string_view>

TEST(srt_compat_string_options_accept_repeated_defaults_and_resets)
{
    REQUIRE_EQ(srt_startup(), 0);
    const SRTSOCKET socket = srt_create_socket();
    REQUIRE(socket != SRT_INVALID_SOCK);

    auto set_twice = [&](SRT_SOCKOPT option, std::string_view value) {
        for (int attempt = 0; attempt < 2; ++attempt)
            REQUIRE_EQ(srt_setsockflag(socket, option, value.data(),
                           static_cast<int>(value.size())),
                0);
    };
    auto check_filter = [&](std::string_view expected) {
        std::array<char, 128> buffer {};
        int size = static_cast<int>(buffer.size());
        REQUIRE_EQ(
            srt_getsockflag(socket, SRTO_PACKETFILTER, buffer.data(), &size),
            0);
        REQUIRE_EQ(size, static_cast<int>(expected.size()));
        REQUIRE_EQ(std::string_view(buffer.data(), expected.size()), expected);
    };

    check_filter("");
    for (const char* value : {"", "fec,cols:2,rows:1", ""}) {
        set_twice(SRTO_PACKETFILTER, value);
        check_filter(value);
    }
    for (const char* value : {"", "file", ""})
        set_twice(SRTO_CONGESTION, value);

    REQUIRE_EQ(srt_close(socket), 0);
    REQUIRE_EQ(srt_cleanup(), 0);
}
