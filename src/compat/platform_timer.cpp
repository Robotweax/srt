#include "compat/platform_timer.hpp"

#if defined(_WIN32) && !defined(ROBOTWEAX_SRT_NO_WINDOWS_TIMER_PERIOD)
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// WIN32_LEAN_AND_MEAN leaves the multimedia timer declarations out.
#include <timeapi.h>
#if defined(_MSC_VER)
// A static library does not carry its link dependencies; this records the
// one for the timer functions in the object file.
#pragma comment(lib, "winmm.lib")
#endif
#endif

namespace robotweax::srt::compat {

#if defined(_WIN32) && !defined(ROBOTWEAX_SRT_NO_WINDOWS_TIMER_PERIOD)
PlatformTimerResolution::PlatformTimerResolution() noexcept
    : held_(timeBeginPeriod(1U) == TIMERR_NOERROR)
{
}

PlatformTimerResolution::~PlatformTimerResolution()
{
    if (held_)
        (void)timeEndPeriod(1U);
}
#else
PlatformTimerResolution::PlatformTimerResolution() noexcept = default;
PlatformTimerResolution::~PlatformTimerResolution() = default;
#endif

} // namespace robotweax::srt::compat
