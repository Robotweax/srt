#pragma once

// Desktop policy retains its qualified coarse-timer fallback. Mobile timer
// pacing is a separate, default-off experiment and never spins for a future
// pacing deadline, even when timers are coarse. Windows/BSD retain legacy pacing.
#if defined(__ANDROID__)
#define ROBOTWEAX_SRT_MOBILE_PACING_PLATFORM 1
#elif defined(__APPLE__)
#include <TargetConditionals.h>
#define ROBOTWEAX_SRT_MOBILE_PACING_PLATFORM TARGET_OS_IOS
#else
#define ROBOTWEAX_SRT_MOBILE_PACING_PLATFORM 0
#endif

#if ROBOTWEAX_SRT_MOBILE_PACING_PLATFORM \
    && defined(ROBOTWEAX_SRT_EXPERIMENTAL_MOBILE_PACING)
#define ROBOTWEAX_SRT_MOBILE_TIMER_PACING 1
#else
#define ROBOTWEAX_SRT_MOBILE_TIMER_PACING 0
#endif

#if ROBOTWEAX_SRT_MOBILE_TIMER_PACING \
    || (defined(__linux__) && !defined(__ANDROID__))
#define ROBOTWEAX_SRT_SUBMILLISECOND_TIMER_PACING 1
#elif defined(__APPLE__)
#if TARGET_OS_OSX
#define ROBOTWEAX_SRT_SUBMILLISECOND_TIMER_PACING 1
#else
#define ROBOTWEAX_SRT_SUBMILLISECOND_TIMER_PACING 0
#endif
#else
#define ROBOTWEAX_SRT_SUBMILLISECOND_TIMER_PACING 0
#endif
