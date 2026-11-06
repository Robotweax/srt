#pragma once

#include "robotweax/srt/error.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace robotweax::srt::npd {

// Provisional identifiers; no socket negotiation uses these codecs yet.
inline constexpr std::uint16_t extension_command = 0x7f15;
inline constexpr std::size_t unit_size = 188;
inline constexpr std::size_t maximum_units = 7;
inline constexpr std::array<std::byte, 8> record_content {std::byte {0x53},
    std::byte {0x4e}, std::byte {0x50}, std::byte {0x44}, std::byte {1},
    std::byte {0}, std::byte {0}, std::byte {0}};

struct Result {
    Error error = Error::none;
    std::size_t bytes_written = 0;
    std::size_t null_units = 0;

    [[nodiscard]] constexpr explicit operator bool() const noexcept
    {
        return error == Error::none;
    }
};

// Encode/decode one complete CONFIG record, including its four-byte header.
// Transaction-level policy, duplicates and the CONFIG bit belong to negotiation.
[[nodiscard]] Result encode_record(std::span<std::byte> destination) noexcept;
[[nodiscard]] Error decode_record(std::span<const std::byte> record) noexcept;

// Allocation-free DATA transformations. Input and output must not overlap.
// Failures leave the entire destination unchanged. Empty source payloads are
// invalid. The sender deletes all canonical null units in eligible payloads;
// the receiver also accepts peers that retain any subset of those units.
[[nodiscard]] Result encode_payload(std::span<const std::byte> application,
    std::span<std::byte> destination) noexcept;
[[nodiscard]] Result decode_payload(
    std::span<const std::byte> wire, std::span<std::byte> destination) noexcept;

} // namespace robotweax::srt::npd
