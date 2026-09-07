#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace robotweax::srt {

inline constexpr std::uint32_t minimum_nak_interval_microseconds = 60'000;

class AcknowledgementNumberGenerator {
public:
    explicit constexpr AcknowledgementNumberGenerator(
        std::uint32_t first = 1U) noexcept
        : next_(first == 0U ? 1U : first)
    {
    }

    [[nodiscard]] constexpr std::uint32_t take() noexcept
    {
        const std::uint32_t current = next_;
        next_ = current == std::numeric_limits<std::uint32_t>::max()
            ? 1U
            : current + 1U;
        return current;
    }

private:
    std::uint32_t next_ = 1U;
};

class AcknowledgementTracker {
public:
    explicit AcknowledgementTracker(std::size_t capacity);

    void record(std::uint32_t acknowledgement_number,
        std::uint64_t sent_at_microseconds) noexcept;
    [[nodiscard]] std::optional<std::uint32_t> acknowledge(
        std::uint32_t acknowledgement_number,
        std::uint64_t received_at_microseconds) noexcept;

private:
    struct Entry {
        std::uint32_t acknowledgement_number = 0;
        std::uint64_t sent_at_microseconds = 0;
        bool active = false;
    };

    std::vector<Entry> entries_;
};

class RttEstimator {
public:
    void observe(std::uint32_t sample_microseconds) noexcept;
    void observe_peer_estimate(
        std::uint32_t smoothed_microseconds,
        std::uint32_t variation_microseconds,
        bool bidirectional_traffic) noexcept;

    [[nodiscard]] std::uint32_t smoothed_microseconds() const noexcept
    {
        return smoothed_microseconds_;
    }
    [[nodiscard]] std::uint32_t variation_microseconds() const noexcept
    {
        return variation_microseconds_;
    }
    [[nodiscard]] std::uint32_t nak_interval_microseconds() const noexcept;

private:
    std::uint32_t smoothed_microseconds_ = 100'000;
    std::uint32_t variation_microseconds_ = 50'000;
    bool has_sample_ = false;
};

} // namespace robotweax::srt
