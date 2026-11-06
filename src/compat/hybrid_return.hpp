// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
namespace robotweax::srt::compat::hybrid {
// Explicit local policy, not negotiated timing defaults. Expiry preserves UC.
struct ReturnPolicy {
    std::uint32_t maximum_overlap_us = 0, maximum_overlap_packets = 0,
                  minimum_coverage_packets = 0, minimum_coverage_us = 0,
                  maximum_proof_age_us = 0;
};
[[nodiscard]] bool valid(ReturnPolicy) noexcept;
// Trusted host assertion of contiguous validated MC coverage [boundary,through)
// from the exact overlap revision. A high-water observation is not this proof.
struct CoverageProof {
    std::uint64_t generation = 0, revision = 0, boundary = 0, through = 0,
                  observed_at_us = 0, not_after_us = 0;
    std::uint32_t interval_us = 0;
};
}
