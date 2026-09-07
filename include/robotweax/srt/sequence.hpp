#pragma once

#include <compare>
#include <cstdint>

namespace robotweax::srt {

/**
 * Wrap-aware 31-bit SRT packet sequence number. Ordering/distance comparisons
 * are meaningful only when the values are known to be less than half the
 * sequence space apart.
 */
class SequenceNumber {
public:
    static constexpr std::uint32_t mask = 0x7fff'ffffU;
    static constexpr std::uint32_t modulus = 0x8000'0000U;
    static constexpr std::uint32_t half_range = modulus / 2U;

    constexpr SequenceNumber() noexcept = default;
    explicit constexpr SequenceNumber(std::uint32_t value) noexcept : value_(value & mask) {}

    [[nodiscard]] constexpr std::uint32_t value() const noexcept { return value_; }

    [[nodiscard]] constexpr SequenceNumber next() const noexcept
    {
        return SequenceNumber{value_ + 1U};
    }

    [[nodiscard]] constexpr SequenceNumber advanced(std::uint32_t count) const noexcept
    {
        return SequenceNumber{value_ + count};
    }

    [[nodiscard]] constexpr std::int32_t distance_from(SequenceNumber origin) const noexcept
    {
        const std::uint32_t forward = (value_ - origin.value_) & mask;
        if (forward >= half_range) {
            return -static_cast<std::int32_t>(modulus - forward);
        }
        return static_cast<std::int32_t>(forward);
    }

    [[nodiscard]] constexpr bool is_after(SequenceNumber other) const noexcept
    {
        return distance_from(other) > 0;
    }

    friend constexpr bool operator==(SequenceNumber, SequenceNumber) noexcept = default;

private:
    std::uint32_t value_ = 0;
};

/**
 * Wrap-aware 32-bit SRT timestamp measured in connection-relative
 * microseconds. Distance comparisons require values less than 2^31 ticks
 * apart.
 */
class PacketTimestamp {
public:
    constexpr PacketTimestamp() noexcept = default;
    explicit constexpr PacketTimestamp(std::uint32_t value) noexcept : value_(value) {}

    [[nodiscard]] constexpr std::uint32_t value() const noexcept { return value_; }

    [[nodiscard]] constexpr std::int64_t distance_from(PacketTimestamp origin) const noexcept
    {
        const std::uint32_t forward = value_ - origin.value_;
        if (forward >= 0x8000'0000U) {
            return -static_cast<std::int64_t>(0x1'0000'0000ULL - forward);
        }
        return static_cast<std::int64_t>(forward);
    }

    friend constexpr bool operator==(PacketTimestamp, PacketTimestamp) noexcept = default;

private:
    std::uint32_t value_ = 0;
};

} // namespace robotweax::srt
