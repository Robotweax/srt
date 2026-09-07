#include "robotweax/srt/rtt.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace robotweax::srt {

AcknowledgementTracker::AcknowledgementTracker(std::size_t capacity)
    : entries_(capacity)
{
    if (capacity == 0U) {
        throw std::invalid_argument("ACK tracker capacity must be non-zero");
    }
}

void AcknowledgementTracker::record(std::uint32_t acknowledgement_number,
    std::uint64_t sent_at_microseconds) noexcept
{
    if (acknowledgement_number == 0U) {
        return;
    }
    auto& entry = entries_[acknowledgement_number % entries_.size()];
    entry = {
        .acknowledgement_number = acknowledgement_number,
        .sent_at_microseconds = sent_at_microseconds,
        .active = true,
    };
}

std::optional<std::uint32_t> AcknowledgementTracker::acknowledge(
    std::uint32_t acknowledgement_number,
    std::uint64_t received_at_microseconds) noexcept
{
    if (acknowledgement_number == 0U) {
        return std::nullopt;
    }
    auto& entry = entries_[acknowledgement_number % entries_.size()];
    if (!entry.active || entry.acknowledgement_number != acknowledgement_number
        || received_at_microseconds <= entry.sent_at_microseconds) {
        return std::nullopt;
    }
    const std::uint64_t elapsed = received_at_microseconds - entry.sent_at_microseconds;
    entry.active = false;
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(
        elapsed, std::numeric_limits<std::uint32_t>::max()));
}

void RttEstimator::observe(std::uint32_t sample_microseconds) noexcept
{
    if (sample_microseconds == 0U) {
        return;
    }
    if (!has_sample_) {
        smoothed_microseconds_ = sample_microseconds;
        variation_microseconds_ = sample_microseconds / 2U;
        has_sample_ = true;
        return;
    }
    const std::uint32_t difference = smoothed_microseconds_ > sample_microseconds
        ? smoothed_microseconds_ - sample_microseconds
        : sample_microseconds - smoothed_microseconds_;
    variation_microseconds_ = static_cast<std::uint32_t>(
        (3ULL * variation_microseconds_ + difference) / 4ULL);
    smoothed_microseconds_ = static_cast<std::uint32_t>(
        (7ULL * smoothed_microseconds_ + sample_microseconds) / 8ULL);
}

void RttEstimator::observe_peer_estimate(
    std::uint32_t smoothed_microseconds,
    std::uint32_t variation_microseconds,
    bool bidirectional_traffic) noexcept
{
    if (smoothed_microseconds == 0U) {
        return;
    }
    constexpr std::uint32_t initial_smoothed_microseconds = 100'000;
    constexpr std::uint32_t initial_variation_microseconds = 50'000;
    const bool is_initial_default =
        smoothed_microseconds == initial_smoothed_microseconds
        && variation_microseconds == initial_variation_microseconds;
    if (!has_sample_) {
        if (is_initial_default) {
            return;
        }
        smoothed_microseconds_ = smoothed_microseconds;
        variation_microseconds_ = variation_microseconds;
        has_sample_ = true;
        return;
    }
    if (!bidirectional_traffic) {
        smoothed_microseconds_ = smoothed_microseconds;
        variation_microseconds_ = variation_microseconds;
        return;
    }
    if (is_initial_default) {
        return;
    }

    const std::uint32_t difference =
        smoothed_microseconds_ > smoothed_microseconds
        ? smoothed_microseconds_ - smoothed_microseconds
        : smoothed_microseconds - smoothed_microseconds_;
    variation_microseconds_ = static_cast<std::uint32_t>(
        (3ULL * variation_microseconds_ + difference) / 4ULL);
    smoothed_microseconds_ = static_cast<std::uint32_t>(
        (7ULL * smoothed_microseconds_ + smoothed_microseconds) / 8ULL);
}

std::uint32_t RttEstimator::nak_interval_microseconds() const noexcept
{
    const std::uint64_t calculated =
        (static_cast<std::uint64_t>(smoothed_microseconds_)
            + 4ULL * variation_microseconds_) / 2ULL;
    return static_cast<std::uint32_t>(
        std::min<std::uint64_t>(
            std::max<std::uint64_t>(
                calculated, minimum_nak_interval_microseconds),
            std::numeric_limits<std::uint32_t>::max()));
}

} // namespace robotweax::srt
