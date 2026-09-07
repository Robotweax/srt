#include "robotweax/srt/live.hpp"

#include <algorithm>
#include <limits>

namespace robotweax::srt {
namespace {

[[nodiscard]] std::uint32_t saturating_u32(std::uint64_t value) noexcept
{
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(
        value, std::numeric_limits<std::uint32_t>::max()));
}

} // namespace

void ArrivalRateEstimator::observe(std::size_t payload_bytes,
    std::uint64_t arrival_microseconds) noexcept
{
    if (has_previous_arrival_ && arrival_microseconds > previous_arrival_microseconds_) {
        const auto interval = arrival_microseconds - previous_arrival_microseconds_;
        intervals_[sample_index_] = saturating_u32(interval);
        payload_sizes_[sample_index_] = saturating_u32(payload_bytes);
        sample_index_ = (sample_index_ + 1U) % sample_count;
        samples_ = std::min(samples_ + 1U, sample_count);
    }
    previous_arrival_microseconds_ = arrival_microseconds;
    has_previous_arrival_ = true;
}

void ArrivalRateEstimator::observe_probe_pair(
    std::uint64_t first_arrival_microseconds,
    std::uint64_t second_arrival_microseconds) noexcept
{
    if (second_arrival_microseconds <= first_arrival_microseconds) {
        return;
    }
    probe_intervals_[probe_index_] = saturating_u32(
        second_arrival_microseconds - first_arrival_microseconds);
    probe_index_ = (probe_index_ + 1U) % sample_count;
    probes_ = std::min(probes_ + 1U, sample_count);
}

ArrivalRates ArrivalRateEstimator::rates() const noexcept
{
    if (samples_ == 0U) {
        return {};
    }
    std::uint64_t interval_sum = 0;
    std::uint64_t byte_sum = 0;
    for (std::size_t index = 0; index < samples_; ++index) {
        interval_sum += intervals_[index];
        byte_sum += payload_sizes_[index] + packet_header_size_;
    }
    ArrivalRates result;
    result.valid = interval_sum != 0U;
    if (!result.valid) {
        return result;
    }
    result.packets_per_second = saturating_u32(
        static_cast<std::uint64_t>(samples_) * 1'000'000ULL / interval_sum);
    result.bytes_per_second = saturating_u32(byte_sum * 1'000'000ULL / interval_sum);

    std::uint64_t probe_sum = 0;
    for (std::size_t index = 0; index < probes_; ++index) {
        probe_sum += probe_intervals_[index];
    }
    if (probe_sum != 0U) {
        result.link_capacity_packets_per_second = saturating_u32(
            static_cast<std::uint64_t>(probes_) * 1'000'000ULL / probe_sum);
    }
    return result;
}

LiveRateController::LiveRateController(Configuration configuration) noexcept
    : configuration_(configuration)
    , average_payload_size_(std::max<std::size_t>(
          configuration.maximum_payload_size, 1U))
{
}

void LiveRateController::observe_input(
    std::size_t payload_bytes,
    std::uint64_t enqueue_microseconds) noexcept
{
    // Keep a fixed-memory, application-input estimate. A 100 ms minimum
    // window avoids converting same-tick bursts into an artificial line-rate
    // spike while still following normal live bitrate changes quickly.
    constexpr std::uint64_t minimum_window_microseconds = 100'000U;
    if (!input_window_started_) {
        input_window_start_microseconds_ = enqueue_microseconds;
        input_window_started_ = true;
    }
    const auto bytes = static_cast<std::uint64_t>(payload_bytes);
    const auto room = std::numeric_limits<std::uint64_t>::max()
        - input_window_bytes_;
    input_window_bytes_ += std::min(bytes, room);
    if (enqueue_microseconds <= input_window_start_microseconds_
        || enqueue_microseconds - input_window_start_microseconds_
            < minimum_window_microseconds) {
        return;
    }
    const std::uint64_t elapsed =
        enqueue_microseconds - input_window_start_microseconds_;
    const std::uint64_t measured = input_window_bytes_
        > std::numeric_limits<std::uint64_t>::max() / 1'000'000U
        ? std::numeric_limits<std::uint64_t>::max()
        : input_window_bytes_ * 1'000'000U / elapsed;
    estimated_input_bandwidth_ = estimated_input_bandwidth_ == 0U
        ? measured
        : estimated_input_bandwidth_
            - estimated_input_bandwidth_ / 8U
            + measured / 8U;
    input_window_start_microseconds_ = enqueue_microseconds;
    input_window_bytes_ = 0U;
}

void LiveRateController::observe_payload(std::size_t payload_bytes) noexcept
{
    // The reference implementation uses a 1/128 IIR for live payload size.
    average_payload_size_ = (average_payload_size_ * 127U + payload_bytes) / 128U;
    average_payload_size_ = std::max<std::size_t>(average_payload_size_, 1U);
}

void LiveRateController::set_input_bandwidth(std::uint64_t bytes_per_second) noexcept
{
    configuration_.input_bandwidth_bytes_per_second = bytes_per_second;
}

void LiveRateController::set_minimum_input_bandwidth(
    std::uint64_t bytes_per_second) noexcept
{
    configuration_.minimum_input_bandwidth_bytes_per_second =
        bytes_per_second;
}

void LiveRateController::set_maximum_bandwidth(std::int64_t bytes_per_second) noexcept
{
    configuration_.maximum_bandwidth_bytes_per_second = bytes_per_second;
}

void LiveRateController::set_overhead_percent(std::uint32_t percent) noexcept
{
    configuration_.overhead_percent = std::min(percent, 100U);
}

std::uint64_t LiveRateController::pacing_rate_bytes_per_second() const noexcept
{
    constexpr std::uint64_t default_unlimited_rate = 125'000'000;
    if (configuration_.maximum_bandwidth_bytes_per_second < 0) {
        return default_unlimited_rate;
    }
    std::uint64_t rate = configuration_.input_bandwidth_bytes_per_second;
    if (rate == 0U) {
        rate = std::max(estimated_input_bandwidth_,
            configuration_.minimum_input_bandwidth_bytes_per_second);
    }
    if (rate != 0U) {
        const auto multiplier = 100U + configuration_.overhead_percent;
        if (rate > std::numeric_limits<std::uint64_t>::max() / multiplier) {
            rate = std::numeric_limits<std::uint64_t>::max();
        } else {
            rate = rate * multiplier / 100U;
        }
    }
    if (configuration_.maximum_bandwidth_bytes_per_second > 0
        && (rate == 0U || rate > static_cast<std::uint64_t>(
                configuration_.maximum_bandwidth_bytes_per_second))) {
        rate = static_cast<std::uint64_t>(
            configuration_.maximum_bandwidth_bytes_per_second);
    }
    return rate == 0U ? default_unlimited_rate : rate;
}

std::uint64_t LiveRateController::packet_interval_microseconds() const noexcept
{
    const std::uint64_t packet_size = average_payload_size_
        + configuration_.packet_header_size;
    const std::uint64_t rate = pacing_rate_bytes_per_second();
    return std::max<std::uint64_t>(1U,
        (packet_size * 1'000'000ULL + rate - 1U) / rate);
}

DriftSampler::DriftSampler(std::size_t samples_per_window,
    std::int64_t maximum_correction_microseconds) noexcept
    : samples_per_window_(std::max<std::size_t>(samples_per_window, 1U))
    , maximum_correction_microseconds_(std::max<std::int64_t>(
          maximum_correction_microseconds, 1))
{
}

DriftUpdate DriftSampler::observe(std::int64_t drift_microseconds) noexcept
{
    sum_microseconds_ += drift_microseconds;
    ++samples_;
    if (samples_ < samples_per_window_) {
        return {.residual_drift_microseconds = residual_drift_microseconds_};
    }

    const std::int64_t observed_drift = sum_microseconds_
        / static_cast<std::int64_t>(samples_);
    samples_ = 0;
    sum_microseconds_ = 0;

    const std::int64_t estimation_error =
        observed_drift - estimated_drift_microseconds_;
    const auto correction = std::clamp(estimation_error,
        -maximum_correction_microseconds_, maximum_correction_microseconds_);
    estimated_drift_microseconds_ += correction;
    residual_drift_microseconds_ =
        observed_drift - estimated_drift_microseconds_;
    return {
        .window_complete = true,
        .correction_microseconds = correction,
        .residual_drift_microseconds = residual_drift_microseconds_,
    };
}

} // namespace robotweax::srt
