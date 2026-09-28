/* SPDX-License-Identifier: MIT */

// A separate process interposes RAND_bytes to show that socket and group
// allocation fail closed while the identity generator cannot be seeded,
// rather than handing out a predictable identity, and recover once the random
// source works. No production hook is added to the library.
#include "srt.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>

namespace {
std::atomic<bool> fail_random {false};
std::atomic<unsigned> injected_failures {0};

void require(bool value, const char* message)
{
    if (!value) {
        std::fprintf(stderr, "FAIL: %s (injected=%u)\n", message,
            injected_failures.load());
        std::exit(1);
    }
}
} // namespace

extern "C" int RAND_bytes(unsigned char* bytes, int length)
{
    using Function = int (*)(unsigned char*, int);
    static auto real =
        reinterpret_cast<Function>(dlsym(RTLD_NEXT, "RAND_bytes"));
    if (fail_random.load()) {
        ++injected_failures;
        return 0;
    }
    return real ? real(bytes, length) : 0;
}

int main()
{
    // The identity generator is seeded on the way into the runtime, so the
    // random source fails from the start: no seed, no identity.
    fail_random = true;
    require(srt_startup() == 0, "startup");
    require(srt_create_socket() == SRT_INVALID_SOCK,
        "socket allocation must fail without randomness");
    require(srt_create_group(SRT_GTYPE_BROADCAST) == SRT_INVALID_SOCK,
        "group allocation must fail without randomness");
    require(injected_failures.load() >= 2U, "random source was consulted");
    fail_random = false;

    const SRTSOCKET socket = srt_create_socket();
    require(socket != SRT_INVALID_SOCK, "socket after recovery");
    const SRTSOCKET group = srt_create_group(SRT_GTYPE_BROADCAST);
    require(group != SRT_INVALID_SOCK, "group after recovery");
    require(srt_close(socket) == 0, "close socket");
    require(srt_close(group) == 0, "close group");
    require(srt_cleanup() == 0, "cleanup");
    std::puts("random identity allocation fails closed");
    return 0;
}
