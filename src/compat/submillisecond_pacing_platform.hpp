#pragma once

// Keep the timer-wait and pacing-credit change on the two qualified desktop
// platforms. Apple mobile targets and Android retain the previous pacing path.
#if defined(__linux__) && !defined(__ANDROID__)
#define ROBOTWEAX_SRT_SUBMILLISECOND_TIMER_PACING 1
#elif defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_OSX
#define ROBOTWEAX_SRT_SUBMILLISECOND_TIMER_PACING 1
#else
#define ROBOTWEAX_SRT_SUBMILLISECOND_TIMER_PACING 0
#endif
#else
#define ROBOTWEAX_SRT_SUBMILLISECOND_TIMER_PACING 0
#endif
