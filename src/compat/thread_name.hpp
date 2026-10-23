/* SPDX-License-Identifier: MIT */
#pragma once

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstddef>

#if defined(__linux__) || defined(__APPLE__)
#include <pthread.h>
#endif

namespace robotweax::srt::compat {

// Diagnostics only: name the calling library worker once, before dispatch.
// Keep Linux's 15 visible bytes on every platform and preserve native errno.
template <std::size_t Size>
void name_current_thread(const char (&name)[Size]) noexcept
{
    static_assert(Size <= 16U);
#if defined(__linux__) || defined(__APPLE__)
    const int previous_errno = errno;
#if defined(__APPLE__)
    (void)pthread_setname_np(name);
#else
    (void)pthread_setname_np(pthread_self(), name);
#endif
    errno = previous_errno;
#else
    (void)name;
#endif
}

template <std::size_t Size>
void name_current_thread(const char (&prefix)[Size], std::size_t index) noexcept
{
    static_assert(Size <= 16U);
    char name[16] {};
    std::copy_n(prefix, Size - 1U, name);
    const auto result =
        std::to_chars(name + Size - 1U, name + sizeof(name) - 1U, index);
    if (result.ec == std::errc {}) {
        name_current_thread(name);
    }
}

} // namespace robotweax::srt::compat
