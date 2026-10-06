#include "test.hpp"
#include "srt.h"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>

namespace {

struct ConnectResult {
    std::mutex mutex;
    std::condition_variable changed;
    bool finished = false;
    int error = SRT_SUCCESS;
};

void complete_connect(void* context, SRTSOCKET, int error, const sockaddr*, int)
{
    auto& result = *static_cast<ConnectResult*>(context);
    std::lock_guard lock(result.mutex);
    result.error = error;
    result.finished = true;
    result.changed.notify_all();
}

int select_key_length(
    void* context, SRTSOCKET socket, int, const sockaddr*, const char*)
{
    const auto length = *static_cast<const int*>(context);
    return srt_setsockflag(socket, SRTO_PBKEYLEN, &length, sizeof(length));
}

void verify_key_selection(int caller_length, int listener_length,
    bool callback_selection, bool blocking, bool reject)
{
    ConnectResult completion;
    REQUIRE_EQ(srt_startup(), 0);
    struct Cleanup {
        SRTSOCKET listener = SRT_INVALID_SOCK;
        SRTSOCKET caller = SRT_INVALID_SOCK;
        SRTSOCKET accepted = SRT_INVALID_SOCK;
        ~Cleanup()
        {
            if (caller != SRT_INVALID_SOCK)
                (void)srt_close(caller);
            if (accepted != SRT_INVALID_SOCK)
                (void)srt_close(accepted);
            if (listener != SRT_INVALID_SOCK)
                (void)srt_close(listener);
            (void)srt_cleanup();
        }
    } cleanup;
    cleanup.listener = srt_create_socket();
    cleanup.caller = srt_create_socket();
    REQUIRE(cleanup.listener != SRT_INVALID_SOCK);
    REQUIRE(cleanup.caller != SRT_INVALID_SOCK);
    constexpr char passphrase[] = "key-selection-regression";
    constexpr int timeout = 2'000;
    for (const auto socket : {cleanup.listener, cleanup.caller}) {
        REQUIRE_EQ(srt_setsockflag(socket, SRTO_PASSPHRASE, passphrase,
                       sizeof(passphrase) - 1),
            0);
        REQUIRE_EQ(
            srt_setsockflag(socket, SRTO_CONNTIMEO, &timeout, sizeof(timeout)),
            0);
        REQUIRE_EQ(
            srt_setsockflag(socket, SRTO_RCVTIMEO, &timeout, sizeof(timeout)),
            0);
    }
    REQUIRE_EQ(srt_setsockflag(
                   cleanup.caller, SRTO_RCVSYN, &blocking, sizeof(blocking)),
        0);
    if (caller_length != 0) {
        REQUIRE_EQ(srt_setsockflag(cleanup.caller, SRTO_PBKEYLEN,
                       &caller_length, sizeof(caller_length)),
            0);
    }
    if (callback_selection) {
        REQUIRE_EQ(srt_listen_callback(
                       cleanup.listener, select_key_length, &listener_length),
            0);
    } else if (listener_length != 0) {
        REQUIRE_EQ(srt_setsockflag(cleanup.listener, SRTO_PBKEYLEN,
                       &listener_length, sizeof(listener_length)),
            0);
    }
    if (!blocking) {
        REQUIRE_EQ(
            srt_connect_callback(cleanup.caller, complete_connect, &completion),
            0);
    }
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE_EQ(
        srt_bind(cleanup.listener, reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)),
        0);
    int address_size = sizeof(address);
    REQUIRE_EQ(srt_getsockname(cleanup.listener,
                   reinterpret_cast<sockaddr*>(&address), &address_size),
        0);
    REQUIRE_EQ(srt_listen(cleanup.listener, 1), 0);
    const auto connected = srt_connect(cleanup.caller,
        reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    if (blocking) {
        REQUIRE_EQ(connected, reject ? SRT_ERROR : 0);
        if (reject)
            REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ESECFAIL);
    } else {
        REQUIRE_EQ(connected, 0);
        std::unique_lock lock(completion.mutex);
        REQUIRE(
            completion.changed.wait_for(lock, std::chrono::seconds {3}, [&] {
                return completion.finished;
            }));
        REQUIRE_EQ(completion.error, reject ? SRT_ESECFAIL : SRT_SUCCESS);
    }
    if (reject) {
        REQUIRE_EQ(srt_getrejectreason(cleanup.caller), SRT_REJ_BADSECRET);
        int configured = 0;
        int size = sizeof(configured);
        REQUIRE_EQ(
            srt_getsockflag(cleanup.caller, SRTO_PBKEYLEN, &configured, &size),
            0);
        REQUIRE_EQ(configured, caller_length);
        constexpr bool asynchronous = false;
        REQUIRE_EQ(srt_setsockflag(cleanup.listener, SRTO_RCVSYN, &asynchronous,
                       sizeof(asynchronous)),
            0);
        REQUIRE_EQ(
            srt_accept(cleanup.listener, nullptr, nullptr), SRT_INVALID_SOCK);
        REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EASYNCRCV);
        return;
    }
    cleanup.accepted = srt_accept(cleanup.listener, nullptr, nullptr);
    REQUIRE(cleanup.accepted != SRT_INVALID_SOCK);
    const int expected_length = caller_length != 0    ? caller_length
        : listener_length != 0 && !callback_selection ? listener_length
                                                      : 16;
    for (const auto socket : {cleanup.caller, cleanup.accepted}) {
        int negotiated = 0;
        int size = sizeof(negotiated);
        REQUIRE_EQ(
            srt_getsockflag(socket, SRTO_PBKEYLEN, &negotiated, &size), 0);
        REQUIRE_EQ(negotiated, expected_length);
    }
    // Both directions use the accepted length, including the listener's
    // independently generated sender key after the initial KMRSP.
    constexpr char payload[] = "selected encrypted key";
    constexpr bool synchronous = true;
    REQUIRE_EQ(srt_setsockflag(cleanup.caller, SRTO_RCVSYN, &synchronous,
                   sizeof(synchronous)),
        0);
    for (const bool reverse : {false, true}) {
        const auto sender = reverse ? cleanup.accepted : cleanup.caller;
        const auto receiver = reverse ? cleanup.caller : cleanup.accepted;
        REQUIRE_EQ(srt_send(sender, payload, sizeof(payload)), sizeof(payload));
        std::array<char, SRT_LIVE_DEF_PLSIZE> output {};
        REQUIRE_EQ(srt_recvmsg(receiver, output.data(), output.size()),
            sizeof(payload));
        REQUIRE_EQ(std::memcmp(output.data(), payload, sizeof(payload)), 0);
    }
}

} // namespace

TEST(srt_compat_explicit_key_length_rejects_conflicting_induction)
{
    for (const bool blocking : {true, false}) {
        for (const int caller : {16, 24, 32}) {
            for (const int listener : {16, 24, 32}) {
                if (caller != listener) {
                    verify_key_selection(
                        caller, listener, false, blocking, true);
                }
            }
        }
    }
}

TEST(srt_compat_listener_callback_reports_accepted_key_length)
{
    for (const bool blocking : {true, false}) {
        for (const int caller : {16, 24, 32}) {
            verify_key_selection(caller, 16, true, blocking, false);
        }
    }
}

TEST(srt_compat_unset_key_length_adopts_listener_advertisement)
{
    for (const bool blocking : {true, false}) {
        for (const int listener : {0, 16, 24, 32}) {
            verify_key_selection(0, listener, false, blocking, false);
        }
    }
}

TEST(srt_compat_matching_explicit_key_lengths_connect)
{
    for (const bool blocking : {true, false}) {
        for (const int key : {16, 24, 32}) {
            verify_key_selection(key, key, false, blocking, false);
        }
    }
}
