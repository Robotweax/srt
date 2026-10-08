#pragma once

#include "srt/srt.h"
#include "robotweax/srt/udp.hpp"

#include <chrono>
#include <cstddef>

namespace robotweax::srt::compat {

void set_log_level(int level) noexcept;
void add_log_functional_area(int area) noexcept;
void delete_log_functional_area(int area) noexcept;
void reset_log_functional_areas(
    const int* areas, std::size_t area_count) noexcept;
void set_log_handler(void* opaque, SRT_LOG_HANDLER_FN* handler) noexcept;
void set_log_flags(int flags) noexcept;

[[nodiscard]] bool admit_handshake_warning(
    std::chrono::steady_clock::time_point& next,
    std::chrono::steady_clock::time_point now) noexcept;
void emit_handshake_rejection(bool listener, std::uint32_t socket_id,
    std::uint32_t peer_socket_id, IpEndpoint peer, int reason) noexcept;

void emit_compatibility_log(
    int level,
    int functional_area,
    const char* severity,
    const char* logger,
    const char* file,
    int line,
    const char* area,
    const char* message) noexcept;

} // namespace robotweax::srt::compat

#define ROBOTWEAX_SRT_COMPAT_LOG(level, functional_area, severity, logger, message) \
    ::robotweax::srt::compat::emit_compatibility_log(                        \
        (level), (functional_area), (severity), (logger),                    \
        __FILE__, __LINE__, __func__, (message))
