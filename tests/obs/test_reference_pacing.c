// SPDX-License-Identifier: MIT
#include "reference_pacing.h"

#include <assert.h>

static uint64_t simulate(uint64_t oversleep)
{
    uint64_t now = 0;
    struct obs_reference_pacer pacer = obs_reference_pacer_start(now);
    unsigned burst = 0;
    for (unsigned packet = 0; packet < 625; ++packet) {
        uint64_t delay;
        while ((delay = obs_reference_pacer_delay(&pacer, now)) != 0) {
            now += delay + oversleep;
            burst = 0;
        }
        assert(++burst <= OBS_REFERENCE_BURST);
    }
    return now;
}

int main(void)
{
    assert(simulate(0) == 624 * OBS_REFERENCE_INTERVAL_NS);
    // Repeated late wakeups must not turn a ~9.4s fixture into a ~40s replay.
    assert(simulate(UINT64_C(45000000)) <= UINT64_C(9500000000));
    struct obs_reference_pacer pacer = obs_reference_pacer_start(0);
    assert(obs_reference_pacer_delay(&pacer, 0) == 0);
    assert(obs_reference_pacer_delay(&pacer, 0) == OBS_REFERENCE_INTERVAL_NS);
    const uint64_t late = UINT64_C(10000000000);
    for (unsigned i = 0; i < OBS_REFERENCE_BURST; ++i)
        assert(obs_reference_pacer_delay(&pacer, late) == 0);
    assert(
        obs_reference_pacer_delay(&pacer, late) == OBS_REFERENCE_INTERVAL_NS);
    assert(obs_reference_pacer_delay(&pacer, late - 1)
        == OBS_REFERENCE_INTERVAL_NS);
    return 0;
}
