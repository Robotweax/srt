// SPDX-License-Identifier: MIT
#ifndef ROBOTWEAX_OBS_REFERENCE_PACING_H
#define ROBOTWEAX_OBS_REFERENCE_PACING_H

#include <stdint.h>

#define OBS_REFERENCE_INTERVAL_NS UINT64_C(15000000)
#define OBS_REFERENCE_BURST 4

struct obs_reference_pacer {
    uint64_t last_ns;
    uint64_t credit_ns;
};

static inline struct obs_reference_pacer obs_reference_pacer_start(uint64_t now)
{
    struct obs_reference_pacer pacer = {now, OBS_REFERENCE_INTERVAL_NS};
    return pacer;
}

// Acquire one packet using elapsed monotonic time. A positive result means
// sleep and retry; no packet credit is consumed until the result is zero.
static inline uint64_t obs_reference_pacer_delay(
    struct obs_reference_pacer* pacer, uint64_t now)
{
    const uint64_t capacity = OBS_REFERENCE_BURST * OBS_REFERENCE_INTERVAL_NS;
    if (now >= pacer->last_ns) {
        uint64_t elapsed = now - pacer->last_ns;
        pacer->credit_ns = elapsed >= capacity - pacer->credit_ns
            ? capacity
            : pacer->credit_ns + elapsed;
        pacer->last_ns = now;
    }
    if (pacer->credit_ns < OBS_REFERENCE_INTERVAL_NS)
        return OBS_REFERENCE_INTERVAL_NS - pacer->credit_ns;
    pacer->credit_ns -= OBS_REFERENCE_INTERVAL_NS;
    return 0;
}

#endif
