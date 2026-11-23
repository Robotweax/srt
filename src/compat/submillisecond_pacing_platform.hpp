#pragma once

// Desktop policy retains its qualified coarse-timer fallback. Android and iOS
// use mobile timer pacing by default: it never spins for a future pacing
// deadline, even when timers are coarse (issue 353). Defining
// ROBOTWEAX_SRT_MOBILE_LEGACY_PACING restores the previous spinning policy
// there. Windows/BSD retain legacy pacing.
#if defined(__ANDROID__)
#define ROBOTWEAX_SRT_MOBILE_PACING_PLATFORM 1
#elif defined(__APPLE__)
#include <TargetConditionals.h>
#define ROBOTWEAX_SRT_MOBILE_PACING_PLATFORM                                   \
    (TARGET_OS_IOS && !TARGET_OS_MACCATALYST)
#else
#define ROBOTWEAX_SRT_MOBILE_PACING_PLATFORM 0
#endif

#if ROBOTWEAX_SRT_MOBILE_PACING_PLATFORM                                       \
    && !defined(ROBOTWEAX_SRT_MOBILE_LEGACY_PACING)
#define ROBOTWEAX_SRT_MOBILE_TIMER_PACING 1
#else
#define ROBOTWEAX_SRT_MOBILE_TIMER_PACING 0
#endif

#if ROBOTWEAX_SRT_MOBILE_TIMER_PACING                                          \
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

// Bounded pacing coalescing for mobile timer pacing (issue 353, experimental): a
// Live pacing wait may be served up to this many microseconds late, so that
// one wake-up sends the packets that became due meanwhile. Zero, the default,
// keeps one timer wait per pacing slot. It has no effect without mobile timer
// pacing.
#if ROBOTWEAX_SRT_MOBILE_TIMER_PACING                                          \
    && defined(ROBOTWEAX_SRT_MOBILE_PACING_COALESCE_MICROSECONDS)
#define ROBOTWEAX_SRT_PACING_COALESCE_MICROSECONDS                             \
    ROBOTWEAX_SRT_MOBILE_PACING_COALESCE_MICROSECONDS
#else
#define ROBOTWEAX_SRT_PACING_COALESCE_MICROSECONDS 0
#endif
