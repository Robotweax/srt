#include "srt/srt.h"

#include "compat/runtime_scheduler_service.hpp"
#include "compat/runtime_work_executor_service.hpp"
#include "compat/socket_registry.hpp"
#include "compat/socket_readiness.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

#include <csignal>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;

[[nodiscard]] int wait_for_child(
    pid_t child, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        int status = 0;
        const pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child) {
            if (WIFEXITED(status)) {
                return WEXITSTATUS(status);
            }
            return 128;
        }
        if (result < 0) {
            return 129;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            (void)kill(child, SIGKILL);
            (void)waitpid(child, &status, 0);
            return 130;
        }
        std::this_thread::sleep_for(1ms);
    }
}

[[noreturn]] void verify_fresh_child_runtime()
{
    if (srt_startup() != 0) {
        _exit(10);
    }
    const SRTSOCKET socket = srt_create_socket();
    if (socket == SRT_INVALID_SOCK) {
        _exit(11);
    }
    if (srt_close(socket) != 0 || srt_cleanup() != 0) {
        _exit(12);
    }
    std::exit(0);
}

[[nodiscard]] bool rejected_with_fork_error(int result)
{
    return result == SRT_ERROR
        && srt_getlasterror(nullptr) == SRT_EINVOP;
}

[[noreturn]] void verify_inherited_runtime_is_rejected(
    SRTSOCKET inherited_socket, int inherited_poll, bool run_destructors)
{
    if (!rejected_with_fork_error(srt_startup())) {
        _exit(20);
    }
    if (srt_create_socket() != SRT_INVALID_SOCK
        || srt_getlasterror(nullptr) != SRT_EINVOP) {
        _exit(21);
    }
    if (srt_getsockstate(inherited_socket) != SRTS_NONEXIST
        || srt_getlasterror(nullptr) != SRT_EINVOP) {
        _exit(22);
    }
    if (!rejected_with_fork_error(srt_close(inherited_socket))) {
        _exit(23);
    }
    if (!rejected_with_fork_error(
            srt_epoll_release(inherited_poll))) {
        _exit(24);
    }
    if (!rejected_with_fork_error(srt_cleanup())) {
        _exit(25);
    }
    if (robotweax::srt::compat::acquire_runtime_inbox_storage_budget()
        != nullptr) {
        _exit(27);
    }
    if (srt_getversion() != SRT_VERSION_VALUE) {
        _exit(26);
    }
    if (run_destructors) {
        // Deliberately exercise inherited static teardown. exec/_exit remain
        // the supported child path; this is a defensive cleanup regression.
        std::exit(0);
    }
    _exit(0);
}

[[nodiscard]] bool try_activate_listener(SRTSOCKET socket)
{
    constexpr int receive_buffer_bytes = 1'048'576;
    if (srt_setsockflag(socket, SRTO_UDP_RCVBUF,
            &receive_buffer_bytes,
            static_cast<int>(sizeof(receive_buffer_bytes))) != 0) {
        return false;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = 0;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (srt_bind(socket,
            reinterpret_cast<const sockaddr*>(&address),
            static_cast<int>(sizeof(address))) != 0) {
        return false;
    }
    return srt_listen(socket, 4) == 0;
}

[[nodiscard]] bool parent_listener_still_accepts(SRTSOCKET listener)
{
    sockaddr_in address {};
    int address_size = sizeof(address);
    if (srt_getsockname(
            listener, reinterpret_cast<sockaddr*>(&address), &address_size)
        != 0) {
        return false;
    }
    const SRTSOCKET caller = srt_create_socket();
    if (caller == SRT_INVALID_SOCK) {
        return false;
    }
    constexpr int timeout_milliseconds = 2'000;
    constexpr int receive_buffer_bytes = 1'048'576;
    const bool connected =
        srt_setsockflag(caller, SRTO_CONNTIMEO, &timeout_milliseconds,
            sizeof(timeout_milliseconds))
            == 0
        && srt_setsockflag(caller, SRTO_UDP_RCVBUF, &receive_buffer_bytes,
               sizeof(receive_buffer_bytes))
            == 0
        && srt_connect(
               caller, reinterpret_cast<sockaddr*>(&address), address_size)
            == 0;
    const SRTSOCKET accepted =
        connected ? srt_accept(listener, nullptr, nullptr) : SRT_INVALID_SOCK;
    (void)srt_close(caller);
    (void)srt_close(accepted);
    return connected && accepted != SRT_INVALID_SOCK;
}

} // namespace

int main()
{
    const pid_t fresh_child = fork();
    if (fresh_child < 0) {
        std::fprintf(stderr, "fork before runtime initialization failed\n");
        return 1;
    }
    if (fresh_child == 0) {
        verify_fresh_child_runtime();
    }
    const int fresh_result = wait_for_child(fresh_child, 5s);
    if (fresh_result != 0) {
        std::fprintf(stderr,
            "fresh child runtime failed with status %d\n",
            fresh_result);
        return 2;
    }

    if (srt_startup() != 0) {
        std::fprintf(stderr, "parent startup failed\n");
        return 3;
    }
    const SRTSOCKET socket = srt_create_socket();
    if (socket == SRT_INVALID_SOCK) {
        std::fprintf(stderr, "parent socket creation failed\n");
        return 4;
    }
    const auto scheduler = robotweax::srt::compat::acquire_runtime_scheduler();
    if (scheduler == nullptr || !scheduler->snapshot().accepting) {
        std::fprintf(stderr, "parent scheduler startup failed\n");
        return 12;
    }
    if (robotweax::srt::compat::acquire_runtime_inbox_storage_budget()
        == nullptr) {
        std::fprintf(stderr, "parent inbox budget acquisition failed\n");
        return 17;
    }
    const auto work_executor =
        robotweax::srt::compat::acquire_runtime_work_executor();
    if (work_executor == nullptr || !work_executor->snapshot().accepting) {
        std::fprintf(stderr, "parent work executor startup failed\n");
        return 14;
    }
    if (!try_activate_listener(socket)) {
        std::fprintf(stderr, "parent listener activation failed\n");
        return 16;
    }
    const auto watcher = scheduler->acquire_socket_readiness();
    const auto arm_deadline = std::chrono::steady_clock::now() + 2s;
    while (watcher != nullptr && watcher->snapshot().armed == 0U
        && std::chrono::steady_clock::now() < arm_deadline) {
        std::this_thread::sleep_for(1ms);
    }
    if (watcher == nullptr || watcher->snapshot().armed == 0U) {
        std::fprintf(stderr, "parent listener readiness was not armed\n");
        return 17;
    }

    const int poll = srt_epoll_create();
    if (poll < 0) {
        std::fprintf(stderr, "parent epoll creation failed\n");
        return 5;
    }
    const int events = SRT_EPOLL_IN | SRT_EPOLL_ERR;
    if (srt_epoll_add_usock(poll, socket, &events) != 0) {
        std::fprintf(stderr, "parent epoll subscription failed\n");
        return 6;
    }

    const auto record =
        robotweax::srt::compat::SocketRegistry::instance().find(socket);
    if (record == nullptr) {
        std::fprintf(stderr, "parent socket record is unavailable\n");
        return 7;
    }
    std::atomic_bool record_lock_held = false;
    std::atomic_bool release_record_lock = false;
    std::thread lock_holder([&] {
        std::lock_guard lock(record->mutex);
        record_lock_held.store(true, std::memory_order_release);
        while (!release_record_lock.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    });
    while (!record_lock_held.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    int inherited_result = 0;
    for (const bool run_destructors : {false, true}) {
        const pid_t inherited_child = fork();
        if (inherited_child == 0) {
            verify_inherited_runtime_is_rejected(socket, poll, run_destructors);
        }
        inherited_result =
            inherited_child < 0 ? 131 : wait_for_child(inherited_child, 5s);
        if (inherited_result != 0) {
            break;
        }
    }
    release_record_lock.store(true, std::memory_order_release);
    lock_holder.join();
    if (inherited_result != 0) {
        std::fprintf(stderr,
            "inherited child runtime failed with status %d\n",
            inherited_result);
        return 8;
    }
    // A LISTENING status alone cannot detect EPOLL_CTL_DEL on the parent's
    // inherited queue. Require real UDP handshake delivery after child exit.
    if (!parent_listener_still_accepts(socket)) {
        std::fprintf(
            stderr, "parent listener stopped receiving after child exit\n");
        return 18;
    }

    const SRT_SOCKSTATUS parent_state = srt_getsockstate(socket);
    if (parent_state != SRTS_INIT
        && parent_state != SRTS_LISTENING) {
        std::fprintf(stderr,
            "parent socket state changed after fork: %d\n",
            static_cast<int>(parent_state));
        return 9;
    }
    const SRTSOCKET replacement = srt_create_socket();
    if (replacement == SRT_INVALID_SOCK
        || replacement == socket
        || srt_close(replacement) != 0) {
        std::fprintf(stderr,
            "parent runtime was not reusable after fork\n");
        return 10;
    }
    if (srt_epoll_release(poll) != 0
        || srt_close(socket) != 0
        || srt_cleanup() != 0) {
        std::fprintf(stderr, "parent cleanup after fork failed\n");
        return 11;
    }
    if (scheduler->snapshot().accepting) {
        std::fprintf(stderr, "parent scheduler survived final cleanup\n");
        return 13;
    }
    if (work_executor->snapshot().accepting) {
        std::fprintf(stderr, "parent work executor survived final cleanup\n");
        return 15;
    }
    return 0;
}
