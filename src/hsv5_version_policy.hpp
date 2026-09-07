#pragma once

#include "robotweax/srt/handshake_extensions.hpp"

#include <cstdint>

namespace robotweax::srt::detail {

enum class Hsv5PeerVersionStatus : std::uint8_t {
    compatible,
    pre_hsv5,
    below_configured_minimum,
};

// HSv5 coherence is a protocol constraint, while SRTO_MINVERSION is an
// independent application policy. Check coherence first so a Version-5
// exchange claiming a pre-1.3.0 SRT implementation is classified as rogue
// even when the configured minimum would also reject it.
[[nodiscard]] constexpr Hsv5PeerVersionStatus classify_hsv5_peer_version(
    std::uint32_t advertised_version, std::uint32_t configured_minimum) noexcept
{
    if (advertised_version < minimum_hsv5_srt_version) {
        return Hsv5PeerVersionStatus::pre_hsv5;
    }
    if (advertised_version < configured_minimum) {
        return Hsv5PeerVersionStatus::below_configured_minimum;
    }
    return Hsv5PeerVersionStatus::compatible;
}

} // namespace robotweax::srt::detail
