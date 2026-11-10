#pragma once

namespace robotweax::srt::compat {

// Holds the platform's timer period at one millisecond while it lives.
//
// Windows rounds every sleep and timed wait up to the timer period, 15.6 ms
// by default: a 100 us or 1 ms wait takes about 14 ms, measured on hosted
// runners. Pacing, retry gates and scheduler deadlines of this library are in
// the millisecond range and below, so the runtime scheduler holds one of
// these for its lifetime. Requests nest and each is paired with its release.
// Since Windows 10 version 2004 the request affects only this process;
// earlier versions apply the finest period any process has requested to the
// whole system. A refused request is not an error: waits stay coarse.
//
// Other platforms need nothing; the object is empty there. It is also empty
// on Windows when the library is built with
// ROBOTWEAX_SRT_WINDOWS_TIMER_PERIOD=OFF, for integrators who manage the
// timer period themselves.
class PlatformTimerResolution {
public:
    PlatformTimerResolution() noexcept;
    ~PlatformTimerResolution();
    PlatformTimerResolution(const PlatformTimerResolution&) = delete;
    PlatformTimerResolution& operator=(const PlatformTimerResolution&) = delete;

    // Whether a finer period is held. Always false off Windows.
    [[nodiscard]] bool held() const noexcept
    {
        return held_;
    }

private:
    bool held_ = false;
};

} // namespace robotweax::srt::compat
