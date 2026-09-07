#include "srt/srt.h"

#include <cstdio>
#include <cstdlib>

namespace {

class GlobalRuntimeScope {
public:
    GlobalRuntimeScope()
    {
        if (srt_startup() != 0) {
            fail("startup failed in global constructor");
        }
        socket_ = srt_create_socket();
        poll_ = srt_epoll_create();
        const int events = SRT_EPOLL_IN | SRT_EPOLL_ERR;
        if (socket_ == SRT_INVALID_SOCK
            || poll_ < 0
            || srt_epoll_add_usock(
                poll_, socket_, &events) != 0) {
            fail("resource creation failed in global constructor");
        }
    }

    ~GlobalRuntimeScope()
    {
        if (srt_getsockstate(socket_) != SRTS_INIT) {
            fail("global socket was not retained through main");
        }
        if (srt_cleanup() != 0
            || srt_getsockstate(socket_) != SRTS_NONEXIST) {
            fail("final cleanup failed in global destructor");
        }
        if (srt_cleanup() != 0) {
            fail("idempotent cleanup failed in global destructor");
        }
    }

    GlobalRuntimeScope(const GlobalRuntimeScope&) = delete;
    GlobalRuntimeScope& operator=(const GlobalRuntimeScope&) = delete;

    [[nodiscard]] SRTSOCKET socket() const noexcept
    {
        return socket_;
    }

private:
    [[noreturn]] static void fail(const char* message) noexcept
    {
        std::fprintf(stderr, "%s\n", message);
        std::abort();
    }

    SRTSOCKET socket_ = SRT_INVALID_SOCK;
    int poll_ = -1;
};

GlobalRuntimeScope global_runtime;

} // namespace

int main()
{
    if (srt_getsockstate(global_runtime.socket()) != SRTS_INIT) {
        std::fprintf(stderr,
            "global constructor did not preserve its socket\n");
        return 1;
    }

    // A nested application-owned scope must not tear down the runtime owned
    // by a global object when its reference is released.
    if (srt_startup() != 0) {
        return 2;
    }
    const SRTSOCKET local_socket = srt_create_socket();
    if (local_socket == SRT_INVALID_SOCK
        || srt_getsockstate(local_socket) != SRTS_INIT) {
        return 3;
    }
    if (srt_cleanup() != 0
        || srt_getsockstate(global_runtime.socket()) != SRTS_INIT
        || srt_getsockstate(local_socket) != SRTS_INIT) {
        return 4;
    }

    // The global destructor performs the final cleanup after main returns.
    return 0;
}
