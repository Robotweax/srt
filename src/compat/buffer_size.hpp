#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace robotweax::srt::compat {

[[nodiscard]] constexpr std::optional<std::int32_t> checked_srt_buffer_bytes(
    std::size_t packets, std::int32_t maximum_segment_size) noexcept
{
    if (maximum_segment_size <= 28)
        return std::nullopt;
    const auto unit = static_cast<std::uint32_t>(maximum_segment_size - 28);
    if (packets
        > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())
            / unit)
        return std::nullopt;
    return static_cast<std::int32_t>(packets * unit);
}

} // namespace robotweax::srt::compat
