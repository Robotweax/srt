#pragma once

#include <array>
#include <cstdint>

namespace robotweax::srt::detail {

enum class UdpBufferKind { send, receive };

struct UdpBufferPolicy {
    bool doubled_readback = false;
    int capacity_error = 0;
    int invalid_value_error = 0;
};

struct UdpBufferNotice {
    UdpBufferKind kind = UdpBufferKind::send;
    std::int32_t requested = 0;
    std::int32_t effective = 0;
    std::int32_t kernel_bytes = 0;
    unsigned attempts = 0;
    bool fallback = false;
};
inline thread_local bool collect_udp_buffer_notices = false;
inline thread_local std::array<UdpBufferNotice, 2> udp_buffer_notices {};
inline thread_local unsigned udp_buffer_notice_count = 0;

inline void note_udp_buffer(const UdpBufferNotice& notice)
{
    if (collect_udp_buffer_notices
        && udp_buffer_notice_count < udp_buffer_notices.size()) {
        udp_buffer_notices[udp_buffer_notice_count++] = notice;
    }
}

// Set/Get return native system errors (zero on success). Linux readbacks
// include a factor of two for bookkeeping, not twice the requested payload.
// No sysctls or privileges are changed. Only capacity errors allow a fallback.
template <class Set, class Get>
int configure_udp_buffer(std::int32_t requested, UdpBufferKind kind,
    UdpBufferPolicy policy, Set set, Get get)
{
    UdpBufferNotice notice {.kind = kind, .requested = requested};
    const auto read = [&]() {
        const int error = get(notice.kernel_bytes);
        if (error != 0)
            return error;
        notice.effective = policy.doubled_readback ? notice.kernel_bytes / 2
                                                   : notice.kernel_bytes;
        return notice.effective > 0 ? 0 : policy.invalid_value_error;
    };
    const auto finish = [&]() {
        const int error = read();
        if (error == 0)
            note_udp_buffer(notice);
        return error;
    };

    ++notice.attempts;
    const int error = set(requested);
    if (error == 0)
        return finish();
    if (error != policy.capacity_error)
        return error;

    const int read_error = read();
    if (read_error != 0)
        return read_error;
    const std::int32_t original = notice.effective;
    // A failed shrink must not silently retain a buffer above the user's
    // request. For growth, retain the existing buffer as the search floor.
    if (original >= requested)
        return error;

    notice.fallback = true;
    std::int32_t lower = original;
    std::int32_t upper = requested - 1;
    // At most 31 additional sets for positive int32 requests. Every probe is
    // above the original buffer and below the rejected request. This finds
    // the largest accepted argument for a stable monotonic OS limit; the
    // final readback remains authoritative if limits or memory change.
    while (lower < upper && notice.attempts < 32U) {
        const std::int32_t probe = lower + (upper - lower + 1) / 2;
        ++notice.attempts;
        const int probe_error = set(probe);
        if (probe_error == policy.capacity_error) {
            upper = probe - 1;
            continue;
        }
        if (probe_error != 0)
            return probe_error;
        const int actual_error = read();
        if (actual_error != 0)
            return actual_error;
        if (notice.effective < original)
            return policy.capacity_error;
        lower = probe;
        // A successful but clamped/rounded readback already supplies the
        // supported value; do not keep increasing the requested argument.
        if (notice.effective < probe)
            break;
    }
    const int final_error = read();
    if (final_error != 0)
        return final_error;
    if (notice.effective < original)
        return policy.capacity_error;
    note_udp_buffer(notice);
    return 0;
}

} // namespace robotweax::srt::detail
