// SPDX-License-Identifier: MIT
#include <srt/srt.h>

#include <windows.h>

#include <cstdio>
#include <string_view>

int main(int argc, char** argv)
{
    if (argc != 2)
        return 1;
    const std::string_view mode = argv[1];
    if (mode != "cleanup" && mode != "active")
        return 1;
    if (srt_startup() != 0)
        return 2;
    const SRTSOCKET listener = srt_create_socket();
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (listener == SRT_INVALID_SOCK
        || srt_bind(listener, reinterpret_cast<const sockaddr*>(&address),
               sizeof(address))
            != 0
        || srt_listen(listener, 1) != 0
        || srt_getsockstate(listener) != SRTS_LISTENING)
        return 3;
    if (mode == "cleanup" && srt_cleanup() != 0)
        return 4;
    std::fprintf(stderr, "Calling ExitProcess (%s)\n", argv[1]);
    std::fflush(stderr);
    ExitProcess(0);
}
