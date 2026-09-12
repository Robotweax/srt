#pragma once

#include <cerrno>
#include <cstdint>
#include <array>

namespace robotweax::srt::detail {

struct UdpBufferNotice {
    std::int32_t requested = 0;
    std::int32_t effective = 0;
};
inline thread_local bool collect_udp_buffer_notices = false;
inline thread_local std::array<UdpBufferNotice, 2> udp_buffer_notices {};
inline thread_local unsigned udp_buffer_notice_count = 0;

inline void note_udp_buffer(std::int32_t requested, std::int32_t effective)
{
    if (collect_udp_buffer_notices
        && udp_buffer_notice_count < udp_buffer_notices.size()) {
        udp_buffer_notices[udp_buffer_notice_count++] = {requested, effective};
    }
}

// Set/Get return a system error (zero on success). Keep the policy injectable
// so a restrictive BSD kernel can be tested without changing host sysctls.
template <class Set, class Get>
int configure_udp_buffer(std::int32_t requested, bool bsd, Set set, Get get)
{
    const int error = set(requested);
    if (error == 0 || !bsd || error != ENOBUFS) {
        return error;
    }
    std::int32_t current = 0;
    const int read_error = get(current);
    if (read_error != 0) {
        return read_error;
    }
    // An acquired socket may already have a useful, application-selected
    // buffer. Never shrink it to Haivision's 64,000-byte fallback.
    if (current >= 64'000) {
        note_udp_buffer(requested, current);
        return 0;
    }
    // Do not turn a small explicit request into a larger allocation.
    if (requested < 64'000) {
        return error;
    }
    const int fallback_error = set(64'000);
    if (fallback_error != 0) {
        return fallback_error;
    }
    const int effective_error = get(current);
    if (effective_error != 0) {
        return effective_error;
    }
    note_udp_buffer(requested, current);
    return 0;
}

} // namespace robotweax::srt::detail
