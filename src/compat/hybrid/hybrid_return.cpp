// SPDX-License-Identifier: MIT
#include "compat/hybrid/hybrid_return.hpp"
#include "compat/hybrid/hybrid_control.hpp"
namespace robotweax::srt::compat::hybrid {
bool valid(ReturnPolicy p) noexcept
{
    return p.maximum_overlap_us && p.maximum_overlap_us <= maximum_age_us
        && p.maximum_overlap_packets
        && p.maximum_overlap_packets <= maximum_prefix
        && p.minimum_coverage_packets
        && p.minimum_coverage_packets <= p.maximum_overlap_packets
        && p.minimum_coverage_us && p.minimum_coverage_us <= maximum_interval_us
        && p.minimum_coverage_us < p.maximum_overlap_us
        && p.maximum_proof_age_us
        && p.maximum_proof_age_us <= p.maximum_overlap_us;
}
}
