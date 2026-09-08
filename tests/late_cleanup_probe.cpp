// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Robotweax GmbH
#include "srt/srt.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
void require(bool ok)
{
    if (!ok) {
        std::fputs("late cleanup probe failed\n", stderr);
        std::abort();
    }
}

struct Owner {
    explicit Owner(const char* mode)
    {
        if (std::strcmp(mode, "implicit-epoll") == 0) {
            require(srt_epoll_create() >= 0);
        } else if (std::strcmp(mode, "implicit-group") == 0) {
            require(srt_create_group(SRT_GTYPE_BROADCAST) != SRT_INVALID_SOCK);
        } else if (std::strcmp(mode, "implicit-socket") == 0) {
            require(srt_create_socket() != SRT_INVALID_SOCK);
        } else {
            require(srt_startup() == 0);
        }
    }
    ~Owner()
    {
        require(srt_cleanup() == 0);
        require(srt_cleanup() == 0);
    }
};

Owner& owner(const char* mode)
{
    static Owner instance(mode);
    return instance;
}
} // namespace

// Also compiled into a shared wrapper around the static SRT library. The
// executable calls only this entry point, matching plugin-style integration.
extern "C" int late_cleanup_probe(const char* mode)
{
    (void)owner(mode);
    // Explicit-startup resources are created AFTER the owner's constructor.
    // A probe creating them inside that constructor misses this ordering bug.
    if (std::strcmp(mode, "startup-only") == 0
        || std::strncmp(mode, "implicit-", 9) == 0) {
        return 0;
    }
    if (std::strcmp(mode, "restart") == 0) {
        require(srt_cleanup() == 0);
        require(srt_startup() == 0);
    }
    if (std::strcmp(mode, "epoll") == 0) {
        require(srt_epoll_create() >= 0);
    } else if (std::strcmp(mode, "group") == 0) {
        require(srt_create_group(SRT_GTYPE_BROADCAST) != SRT_INVALID_SOCK);
    } else {
        const auto socket = srt_create_socket();
        require(socket != SRT_INVALID_SOCK);
        int version = 0;
        int size = sizeof(version);
        require(srt_getsockflag(socket, SRTO_VERSION, &version, &size) == 0);
        if (std::strcmp(mode, "retained") != 0) {
            require(srt_close(socket) == 0);
        }
    }
    if (std::strcmp(mode, "explicit") == 0) {
        require(srt_cleanup() == 0);
    }
    return 0;
}
