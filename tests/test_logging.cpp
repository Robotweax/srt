#include "test.hpp"

#include "srt/srt.h"
#include "robotweax/srt/udp.hpp"
#include "compat/error_state.hpp"
#include "compat/logging.hpp"

#include <array>
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

struct ScopedSrtRuntime {
    int startup_result = srt_startup();

    ~ScopedSrtRuntime()
    {
        if (startup_result == 0) {
            static_cast<void>(srt_cleanup());
        }
    }
};

struct ScopedLoggingReset {
    ~ScopedLoggingReset()
    {
        srt_setloghandler(nullptr, nullptr);
        srt_setlogflags(0);
        srt_setloglevel(LOG_WARNING);
        constexpr std::array areas{
            SRT_LOGFA_GENERAL,
            SRT_LOGFA_SOCKMGMT,
            SRT_LOGFA_CONN,
            SRT_LOGFA_XTIMER,
            SRT_LOGFA_TSBPD,
            SRT_LOGFA_RSRC,
            SRT_LOGFA_CONGEST,
            SRT_LOGFA_PFILTER,
            SRT_LOGFA_API_CTRL,
            SRT_LOGFA_QUE_CTRL,
            SRT_LOGFA_EPOLL_UPD,
            SRT_LOGFA_API_RECV,
            SRT_LOGFA_BUF_RECV,
            SRT_LOGFA_QUE_RECV,
            SRT_LOGFA_CHN_RECV,
            SRT_LOGFA_GRP_RECV,
            SRT_LOGFA_API_SEND,
            SRT_LOGFA_BUF_SEND,
            SRT_LOGFA_QUE_SEND,
            SRT_LOGFA_CHN_SEND,
            SRT_LOGFA_GRP_SEND,
            SRT_LOGFA_INTERNAL,
            SRT_LOGFA_QUE_MGMT,
            SRT_LOGFA_CHN_MGMT,
            SRT_LOGFA_GRP_MGMT,
            SRT_LOGFA_EPOLL_API,
        };
        srt_resetlogfa(areas.data(), areas.size());
    }
};

struct LogEntry {
    int level = -1;
    std::string file;
    int line = 0;
    std::string area;
    std::string message;
};

struct LogObservation {
    std::mutex mutex;
    std::vector<LogEntry> entries;
    std::condition_variable condition;
    SRTSOCKET inspected_socket = SRT_INVALID_SOCK;
    SRT_SOCKSTATUS inspected_state = SRTS_NONEXIST;
};

void capture_log(void* opaque, int level,
    const char* file, int line, const char* area,
    const char* message)
{
    auto& observation = *static_cast<LogObservation*>(opaque);
    // A handler may inspect socket state without re-entering a socket lock.
    const auto state = observation.inspected_socket != SRT_INVALID_SOCK
        ? srt_getsockstate(observation.inspected_socket)
        : SRTS_NONEXIST;
    std::lock_guard lock(observation.mutex);
    observation.inspected_state = state;
    observation.entries.push_back({
        .level = level,
        .file = file != nullptr ? file : "",
        .line = line,
        .area = area != nullptr ? area : "",
        .message = message != nullptr ? message : "",
    });
    observation.condition.notify_all();
}

struct RecursiveObservation {
    int calls = 0;
    SRTSOCKET nested_socket = SRT_INVALID_SOCK;
};

void recursively_throwing_log(void* opaque, int,
    const char*, int, const char*, const char*)
{
    auto& observation = *static_cast<RecursiveObservation*>(opaque);
    ++observation.calls;
    observation.nested_socket = srt_create_socket();
    throw std::runtime_error{"logging callback test"};
}

struct BlockingObservation {
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false;
    bool release = false;
    int calls = 0;
};

void blocking_log(void* opaque, int,
    const char*, int, const char*, const char*)
{
    auto& observation = *static_cast<BlockingObservation*>(opaque);
    std::unique_lock lock(observation.mutex);
    ++observation.calls;
    observation.entered = true;
    observation.condition.notify_all();
    observation.condition.wait(lock, [&] {
        return observation.release;
    });
}

constexpr int deterministic_flags =
    SRT_LOGF_DISABLE_TIME
    | SRT_LOGF_DISABLE_THREADNAME
    | SRT_LOGF_DISABLE_SEVERITY
    | SRT_LOGF_DISABLE_EOL;

} // namespace

TEST(logging_handler_receives_deterministic_compatibility_events)
{
    ScopedLoggingReset reset;
    LogObservation observation;
    const int area = SRT_LOGFA_API_CTRL;
    srt_resetlogfa(&area, 1U);
    srt_setlogflags(deterministic_flags);
    srt_setloglevel(LOG_NOTICE);
    srt_setloghandler(&observation, capture_log);

    REQUIRE_EQ(srt_startup(), 0);
    srt_setloghandler(nullptr, nullptr);
    srt_setloglevel(LOG_WARNING);
    REQUIRE_EQ(srt_cleanup(), 0);

    std::lock_guard lock(observation.mutex);
    REQUIRE_EQ(observation.entries.size(), 1U);
    REQUIRE_EQ(observation.entries[0].level, LOG_NOTICE);
    REQUIRE(observation.entries[0].file.find("srt_api.cpp")
        != std::string::npos);
    REQUIRE(observation.entries[0].line > 0);
    REQUIRE_EQ(observation.entries[0].area, "srt_startup");
    REQUIRE_EQ(observation.entries[0].message,
        ": startup completed");
}

TEST(logging_level_and_functional_area_changes_apply_immediately)
{
    ScopedSrtRuntime runtime;
    REQUIRE_EQ(runtime.startup_result, 0);
    ScopedLoggingReset reset;
    LogObservation observation;
    const int socket_area = SRT_LOGFA_SOCKMGMT;
    srt_resetlogfa(&socket_area, 1U);
    srt_setlogflags(deterministic_flags);
    srt_setloghandler(&observation, capture_log);

    srt_setloglevel(LOG_WARNING);
    const SRTSOCKET below_level = srt_create_socket();
    REQUIRE(below_level != SRT_INVALID_SOCK);
    REQUIRE_EQ(srt_close(below_level), 0);

    srt_setloglevel(LOG_NOTICE);
    srt_dellogfa(SRT_LOGFA_SOCKMGMT);
    const SRTSOCKET disabled_area = srt_create_socket();
    REQUIRE(disabled_area != SRT_INVALID_SOCK);
    REQUIRE_EQ(srt_close(disabled_area), 0);

    srt_addlogfa(SRT_LOGFA_SOCKMGMT);
    const SRTSOCKET enabled = srt_create_socket();
    REQUIRE(enabled != SRT_INVALID_SOCK);
    REQUIRE_EQ(srt_close(enabled), 0);
    srt_setloghandler(nullptr, nullptr);

    std::lock_guard lock(observation.mutex);
    REQUIRE_EQ(observation.entries.size(), 2U);
    REQUIRE_EQ(observation.entries[0].message, ": socket created");
    REQUIRE_EQ(observation.entries[1].message, ": socket closed");
}

TEST(logging_contains_recursive_callbacks_and_cpp_exceptions)
{
    ScopedSrtRuntime runtime;
    REQUIRE_EQ(runtime.startup_result, 0);
    ScopedLoggingReset reset;
    RecursiveObservation observation;
    const int area = SRT_LOGFA_SOCKMGMT;
    srt_resetlogfa(&area, 1U);
    srt_setlogflags(deterministic_flags);
    srt_setloglevel(LOG_NOTICE);
    srt_setloghandler(&observation, recursively_throwing_log);

    const SRTSOCKET socket = srt_create_socket();
    REQUIRE(socket != SRT_INVALID_SOCK);
    REQUIRE_EQ(observation.calls, 1);
    REQUIRE(observation.nested_socket != SRT_INVALID_SOCK);

    srt_setloghandler(nullptr, nullptr);
    srt_setloglevel(LOG_WARNING);
    REQUIRE_EQ(srt_close(socket), 0);
    REQUIRE_EQ(srt_close(observation.nested_socket), 0);
}

TEST(logging_handler_replacement_is_coherent_during_delivery)
{
    ScopedSrtRuntime runtime;
    REQUIRE_EQ(runtime.startup_result, 0);
    ScopedLoggingReset reset;
    BlockingObservation first;
    LogObservation second;
    const int area = SRT_LOGFA_SOCKMGMT;
    srt_resetlogfa(&area, 1U);
    srt_setlogflags(deterministic_flags);
    srt_setloglevel(LOG_NOTICE);
    srt_setloghandler(&first, blocking_log);

    SRTSOCKET first_socket = SRT_INVALID_SOCK;
    std::thread emitter([&] {
        first_socket = srt_create_socket();
    });
    {
        std::unique_lock lock(first.mutex);
        first.condition.wait(lock, [&] { return first.entered; });
    }

    std::atomic_bool replacement_started = false;
    std::thread replacer([&] {
        replacement_started.store(true, std::memory_order_release);
        srt_setloghandler(&second, capture_log);
    });
    while (!replacement_started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    {
        std::lock_guard lock(first.mutex);
        first.release = true;
    }
    first.condition.notify_all();
    emitter.join();
    replacer.join();

    REQUIRE(first_socket != SRT_INVALID_SOCK);
    REQUIRE_EQ(first.calls, 1);
    const SRTSOCKET second_socket = srt_create_socket();
    REQUIRE(second_socket != SRT_INVALID_SOCK);
    srt_setloghandler(nullptr, nullptr);
    srt_setloglevel(LOG_WARNING);

    {
        std::lock_guard lock(second.mutex);
        REQUIRE_EQ(second.entries.size(), 1U);
        REQUIRE_EQ(second.entries[0].message, ": socket created");
    }
    REQUIRE_EQ(srt_close(first_socket), 0);
    REQUIRE_EQ(srt_close(second_socket), 0);
}

namespace {

struct BufferLogObservation {
    SRTSOCKET socket = SRT_INVALID_SOCK;
    bool getters_ok = true;
    std::vector<LogEntry> entries;
};

void capture_buffer_log(
    void* opaque, int level, const char*, int, const char*, const char* message)
{
    auto& observation = *static_cast<BufferLogObservation*>(opaque);
    if (message == nullptr || std::string_view {message}.find(": UDP ") != 0)
        return;
    observation.entries.push_back({level, "", 0, "", message});
    for (auto option : {SRTO_UDP_SNDBUF, SRTO_UDP_RCVBUF}) {
        int value = 0;
        int size = sizeof(value);
        const int result =
            srt_getsockflag(observation.socket, option, &value, &size);
        observation.getters_ok &= result == 0 && value == 65'536;
    }
    // Verify that the deferred logger also preserves the API's error state.
    int value = 0;
    int size = sizeof(value);
    static_cast<void>(
        srt_getsockflag(SRT_INVALID_SOCK, SRTO_UDP_RCVBUF, &value, &size));
}

} // namespace

TEST(logging_udp_buffers_report_both_sizes_after_unlock_and_preserve_getters)
{
    ScopedSrtRuntime runtime;
    REQUIRE_EQ(runtime.startup_result, 0);
    ScopedLoggingReset reset;
    const int area = SRT_LOGFA_SOCKMGMT;
    srt_resetlogfa(&area, 1U);
    srt_setlogflags(deterministic_flags);
    srt_setloglevel(LOG_DEBUG);
    for (bool acquire : {false, true}) {
        BufferLogObservation observation;
        observation.socket = srt_create_socket();
        REQUIRE(observation.socket != SRT_INVALID_SOCK);
        const int requested = 65'536;
        for (auto option : {SRTO_UDP_SNDBUF, SRTO_UDP_RCVBUF}) {
            REQUIRE_EQ(srt_setsockflag(observation.socket, option, &requested,
                           sizeof(requested)),
                0);
        }
        srt_setloghandler(&observation, capture_buffer_log);
        robotweax::srt::compat::set_last_error(SRT_EASYNCRCV, 77);
        if (acquire) {
            robotweax::srt::UdpSocket native;
            REQUIRE_EQ(native.bind(robotweax::srt::IpEndpoint::loopback(0)),
                robotweax::srt::Error::none);
            const auto handle = native.release_native();
            REQUIRE_EQ(srt_bind_acquire(
                           observation.socket, static_cast<UDPSOCKET>(handle)),
                0);
        } else {
            sockaddr_in local {};
            local.sin_family = AF_INET;
            local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            REQUIRE_EQ(
                srt_bind(observation.socket,
                    reinterpret_cast<const sockaddr*>(&local), sizeof(local)),
                0);
        }
        int system_error = 0;
        REQUIRE_EQ(srt_getlasterror(&system_error), SRT_EASYNCRCV);
        REQUIRE_EQ(system_error, 77);
        REQUIRE(observation.getters_ok);
        REQUIRE_EQ(observation.entries.size(), 2U);
        for (unsigned i = 0; i < 2; ++i) {
            const auto& entry = observation.entries[i];
            char direction[16] {};
            int seen_request = 0, effective = 0, kernel = 0;
            unsigned attempts = 0;
            REQUIRE_EQ(
                std::sscanf(entry.message.c_str(),
                    ": UDP %15s buffer: requested=%d effective=%d kernel=%d "
                    "bytes; attempts=%u",
                    direction, &seen_request, &effective, &kernel, &attempts),
                5);
            REQUIRE_EQ(std::string {direction}, i == 0 ? "send" : "receive");
            REQUIRE_EQ(seen_request, requested);
            REQUIRE(effective > 0);
#if defined(__linux__)
            REQUIRE_EQ(effective, kernel / 2);
#else
            REQUIRE_EQ(effective, kernel);
#endif
            REQUIRE(attempts >= 1U && attempts <= 32U);
            const bool limited = effective < requested
                || entry.message.find("fallback=yes") != std::string::npos;
            REQUIRE_EQ(entry.level, limited ? LOG_WARNING : LOG_DEBUG);
        }
        srt_setloghandler(nullptr, nullptr);
        REQUIRE_EQ(srt_close(observation.socket), 0);
    }
}

TEST(logging_handshake_warning_budget_has_a_fixed_time_bound)
{
    using Clock = std::chrono::steady_clock;
    Clock::time_point next {};
    const auto start = Clock::time_point {} + std::chrono::seconds {1};
    REQUIRE(robotweax::srt::compat::admit_handshake_warning(next, start));
    for (int i = 0; i < 1000; ++i) {
        REQUIRE(!robotweax::srt::compat::admit_handshake_warning(next, start));
    }
    REQUIRE(!robotweax::srt::compat::admit_handshake_warning(
        next, start + std::chrono::milliseconds {99}));
    REQUIRE(robotweax::srt::compat::admit_handshake_warning(
        next, start + std::chrono::milliseconds {100}));
    REQUIRE(!robotweax::srt::compat::admit_handshake_warning(next, start));
}

TEST(logging_handshake_rejection_formats_numeric_peers_and_known_reasons)
{
    ScopedLoggingReset reset;
    LogObservation observation;
    srt_setloghandler(&observation, capture_log);
    srt_setloglevel(LOG_WARNING);
    const int area = SRT_LOGFA_CONN;
    srt_resetlogfa(&area, 1U);
    srt_setlogflags(deterministic_flags);
    robotweax::srt::compat::emit_handshake_rejection(true, 17, 18,
        robotweax::srt::IpEndpoint::loopback(9000), SRT_REJ_BADSECRET);
    robotweax::srt::compat::emit_handshake_rejection(false, 19, 20,
        robotweax::srt::IpEndpoint::ipv6_loopback(9001, 7), 2001);
    srt_setloghandler(nullptr, nullptr);
    REQUIRE_EQ(observation.entries.size(), 2U);
    REQUIRE_EQ(observation.entries[0].level, LOG_WARNING);
    REQUIRE_EQ(observation.entries[0].message,
        ": handshake rejected socket=17 peer=127.0.0.1:9000 peer_socket=18 "
        "reason=10 name=BADSECRET wire=1010");
    REQUIRE_EQ(observation.entries[1].message,
        ": connection setup failed socket=19 peer=[0:0:0:0:0:0:0:1%7]:9001 "
        "peer_socket=20 reason=2001 name=APPLICATION wire=2001");
}

TEST(logging_handshake_rejections_report_both_endpoints_without_secrets)
{
    for (int scenario = 0; scenario < 4; ++scenario) {
        LogObservation observation;
        ScopedSrtRuntime runtime;
        REQUIRE_EQ(runtime.startup_result, 0);
        ScopedLoggingReset reset;
        const SRTSOCKET listener = srt_create_socket();
        const SRTSOCKET caller = srt_create_socket();
        REQUIRE(listener != SRT_INVALID_SOCK);
        REQUIRE(caller != SRT_INVALID_SOCK);
        constexpr char listener_secret[] = "listener-log-fixture";
        constexpr char caller_secret[] = "caller-log-fixture";
        if (scenario == 0 || scenario == 1) {
            REQUIRE_EQ(srt_setsockflag(listener, SRTO_PASSPHRASE,
                           listener_secret, sizeof(listener_secret) - 1),
                0);
        }
        if (scenario == 0 || scenario == 2) {
            REQUIRE_EQ(srt_setsockflag(caller, SRTO_PASSPHRASE, caller_secret,
                           sizeof(caller_secret) - 1),
                0);
        }
        if (scenario == 3) {
            const SRT_TRANSTYPE mode = SRTT_FILE;
            REQUIRE_EQ(
                srt_setsockflag(caller, SRTO_TRANSTYPE, &mode, sizeof(mode)),
                0);
        }
        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        REQUIRE_EQ(
            srt_bind(listener, reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)),
            0);
        REQUIRE_EQ(srt_listen(listener, 4), 0);
        int size = sizeof(address);
        REQUIRE_EQ(srt_getsockname(
                       listener, reinterpret_cast<sockaddr*>(&address), &size),
            0);
        const bool synchronous = false;
        const std::int32_t timeout = 2'000;
        REQUIRE_EQ(srt_setsockflag(
                       caller, SRTO_RCVSYN, &synchronous, sizeof(synchronous)),
            0);
        REQUIRE_EQ(
            srt_setsockflag(caller, SRTO_CONNTIMEO, &timeout, sizeof(timeout)),
            0);
        const int poll = srt_epoll_create();
        REQUIRE(poll >= 0);
        const int events = SRT_EPOLL_IN | SRT_EPOLL_OUT | SRT_EPOLL_ERR;
        REQUIRE_EQ(srt_epoll_add_usock(poll, caller, &events), 0);
        const int area = SRT_LOGFA_CONN;
        srt_resetlogfa(&area, 1U);
        srt_setloglevel(LOG_WARNING);
        srt_setlogflags(deterministic_flags);
        observation.inspected_socket = listener;
        srt_setloghandler(&observation, capture_log);
        REQUIRE_EQ(
            srt_connect(caller, reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)),
            0);
        SRT_EPOLL_EVENT ready {};
        REQUIRE_EQ(srt_epoll_uwait(poll, &ready, 1, 5'000), 1);
        REQUIRE_EQ(ready.events, events);
        REQUIRE_EQ(srt_getsockstate(caller), SRTS_BROKEN);
        const int reason = scenario == 0 ? SRT_REJ_BADSECRET
            : scenario == 3              ? SRT_REJ_MESSAGEAPI
                                         : SRT_REJ_UNSECURE;
        REQUIRE_EQ(srt_getrejectreason(caller), reason);
        {
            std::unique_lock lock(observation.mutex);
            REQUIRE(observation.condition.wait_for(
                lock, std::chrono::seconds {5}, [&] {
                    return observation.entries.size() == 2U;
                }));
            REQUIRE_EQ(observation.inspected_state, SRTS_LISTENING);
        }
        // Close waits for the setup operation, including its final log callback.
        REQUIRE_EQ(srt_close(caller), 0);
        REQUIRE_EQ(srt_close(listener), 0);
        REQUIRE_EQ(srt_epoll_release(poll), 0);
        srt_setloghandler(nullptr, nullptr);
        std::lock_guard lock(observation.mutex);
        REQUIRE_EQ(observation.entries.size(), 2U);
        bool saw_listener = false;
        bool saw_caller = false;
        for (const auto& entry : observation.entries) {
            REQUIRE_EQ(entry.level, LOG_WARNING);
            REQUIRE(entry.message.find("reason=" + std::to_string(reason))
                != std::string::npos);
            REQUIRE(entry.message.find("wire=" + std::to_string(1000 + reason))
                != std::string::npos);
            REQUIRE(entry.message.find(listener_secret) == std::string::npos);
            REQUIRE(entry.message.find(caller_secret) == std::string::npos);
            saw_listener |=
                entry.message.find("handshake rejected") != std::string::npos;
            saw_caller |= entry.message.find("connection setup failed")
                != std::string::npos;
        }
        REQUIRE(saw_listener);
        REQUIRE(saw_caller);
    }
}
