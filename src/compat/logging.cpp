#include "compat/logging.hpp"

#include <algorithm>
#include <array>
#include <bitset>
#include <chrono>
#include <cstdio>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

namespace robotweax::srt::compat {
namespace {

constexpr std::size_t functional_area_count =
    static_cast<std::size_t>(SRT_LOGFA_LASTNONE) + 1U;
constexpr std::size_t maximum_reset_entries = functional_area_count;

constexpr std::array<int, 26> default_functional_areas{
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

[[nodiscard]] bool valid_functional_area(int area) noexcept
{
    return area >= 0
        && static_cast<std::size_t>(area) < functional_area_count;
}

[[nodiscard]] std::bitset<functional_area_count>
default_area_set() noexcept
{
    std::bitset<functional_area_count> result;
    for (const int area : default_functional_areas) {
        result.set(static_cast<std::size_t>(area));
    }
    return result;
}

struct LoggingState {
    std::recursive_mutex mutex;
    std::bitset<functional_area_count> enabled_areas =
        default_area_set();
    int maximum_level = LOG_WARNING;
    int flags = 0;
    void* handler_opaque = nullptr;
    SRT_LOG_HANDLER_FN* handler = nullptr;
};

[[nodiscard]] LoggingState& logging_state() noexcept
{
    static LoggingState state;
    return state;
}

thread_local bool inside_log_handler = false;

[[nodiscard]] std::string make_prefix(
    int flags, const char* severity, const char* logger)
{
    std::ostringstream output;
    if ((flags & SRT_LOGF_DISABLE_TIME) == 0) {
        const auto now = std::chrono::system_clock::now();
        const auto microseconds = std::chrono::duration_cast<
            std::chrono::microseconds>(now.time_since_epoch()).count();
        output << microseconds;
    }
    if ((flags & SRT_LOGF_DISABLE_THREADNAME) == 0) {
        output << '/' << std::this_thread::get_id();
    }
    if ((flags & SRT_LOGF_DISABLE_SEVERITY) == 0) {
        output << (severity != nullptr ? severity : "")
               << ':' << (logger != nullptr ? logger : "");
    }
    output << ": ";
    return output.str();
}

class HandlerCallGuard {
public:
    HandlerCallGuard() noexcept { inside_log_handler = true; }
    ~HandlerCallGuard() { inside_log_handler = false; }

    HandlerCallGuard(const HandlerCallGuard&) = delete;
    HandlerCallGuard& operator=(const HandlerCallGuard&) = delete;
};

} // namespace

void set_log_level(int level) noexcept
{
    auto& state = logging_state();
    std::lock_guard lock(state.mutex);
    state.maximum_level = level;
}

void add_log_functional_area(int area) noexcept
{
    if (!valid_functional_area(area)) {
        return;
    }
    auto& state = logging_state();
    std::lock_guard lock(state.mutex);
    state.enabled_areas.set(static_cast<std::size_t>(area));
}

void delete_log_functional_area(int area) noexcept
{
    if (!valid_functional_area(area)) {
        return;
    }
    auto& state = logging_state();
    std::lock_guard lock(state.mutex);
    state.enabled_areas.reset(static_cast<std::size_t>(area));
}

void reset_log_functional_areas(
    const int* areas, std::size_t area_count) noexcept
{
    auto& state = logging_state();
    std::lock_guard lock(state.mutex);
    state.enabled_areas.reset();
    if (areas == nullptr) {
        return;
    }
    const std::size_t bounded_count =
        std::min(area_count, maximum_reset_entries);
    for (std::size_t index = 0; index < bounded_count; ++index) {
        if (valid_functional_area(areas[index])) {
            state.enabled_areas.set(
                static_cast<std::size_t>(areas[index]));
        }
    }
}

void set_log_handler(void* opaque, SRT_LOG_HANDLER_FN* handler) noexcept
{
    auto& state = logging_state();
    std::lock_guard lock(state.mutex);
    state.handler_opaque = opaque;
    state.handler = handler;
}

void set_log_flags(int flags) noexcept
{
    auto& state = logging_state();
    std::lock_guard lock(state.mutex);
    state.flags = flags;
}

void emit_compatibility_log(
    int level,
    int functional_area,
    const char* severity,
    const char* logger,
    const char* file,
    int line,
    const char* area,
    const char* message) noexcept
{
    if (!valid_functional_area(functional_area)
        || inside_log_handler) {
        return;
    }

    auto& state = logging_state();
    std::lock_guard lock(state.mutex);
    if (level > state.maximum_level
        || !state.enabled_areas.test(
            static_cast<std::size_t>(functional_area))) {
        return;
    }

    try {
        std::string formatted = make_prefix(
            state.flags, severity, logger);
        if (message != nullptr) {
            formatted += message;
        }
        if ((state.flags & SRT_LOGF_DISABLE_EOL) == 0) {
            formatted.push_back('\n');
        }

        if (state.handler != nullptr) {
            HandlerCallGuard guard;
            try {
                state.handler(state.handler_opaque, level,
                    file != nullptr ? file : "", line,
                    area != nullptr ? area : "", formatted.c_str());
            } catch (...) {
                // C callbacks must never unwind through the SRT ABI.
            }
            return;
        }
        static_cast<void>(std::fwrite(
            formatted.data(), 1U, formatted.size(), stderr));
        static_cast<void>(std::fflush(stderr));
    } catch (...) {
        // Logging must not change protocol or public-API behavior.
    }
}

} // namespace robotweax::srt::compat
