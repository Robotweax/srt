#include "test.hpp"

#include "srt/srt.h"
#include "robotweax/srt/udp.hpp"
#include "compat/error_state.hpp"

#include <array>
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
};

void capture_log(void* opaque, int level,
    const char* file, int line, const char* area,
    const char* message)
{
    auto& observation = *static_cast<LogObservation*>(opaque);
    std::lock_guard lock(observation.mutex);
    observation.entries.push_back({
        .level = level,
        .file = file != nullptr ? file : "",
        .line = line,
        .area = area != nullptr ? area : "",
        .message = message != nullptr ? message : "",
    });
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
