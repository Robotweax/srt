// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
namespace robotweax::srt::compat::hybrid {
struct AutomaticRequest {
    std::uint64_t stream = 0, member = 0, generation = 0, revision = 0,
                  initial_position = 0, manual_epoch = 0, started_at_us = 0,
                  admission_deadline_us = 0, route_epoch = 0;
};
struct AutomaticCursor {
    std::uint64_t position = 0, observed_at_us = 0, generation = 0;
};
}
