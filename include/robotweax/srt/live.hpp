#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace robotweax::srt {

struct ArrivalRates {
    std::uint32_t packets_per_second = 0;
    std::uint32_t bytes_per_second = 0;
    std::uint32_t link_capacity_packets_per_second = 0;
    bool valid = false;
};

// A fixed-memory estimator for the rate fields carried by a full SRT ACK.
class ArrivalRateEstimator {
public:
    explicit ArrivalRateEstimator(std::size_t header_bytes = 44) noexcept
        : packet_header_size_(header_bytes)
    {
    }
    void observe(std::size_t payload_bytes,
        std::uint64_t arrival_microseconds) noexcept;
    void observe_probe_pair(std::uint64_t first_arrival_microseconds,
        std::uint64_t second_arrival_microseconds) noexcept;
    [[nodiscard]] ArrivalRates rates() const noexcept;

private:
    static constexpr std::size_t sample_count = 16;
    std::array<std::uint32_t, sample_count> intervals_{};
    std::array<std::uint32_t, sample_count> payload_sizes_{};
    std::array<std::uint32_t, sample_count> probe_intervals_{};
    std::size_t sample_index_ = 0;
    std::size_t samples_ = 0;
    std::size_t probe_index_ = 0;
    std::size_t probes_ = 0;
    std::uint64_t previous_arrival_microseconds_ = 0;
    bool has_previous_arrival_ = false;
    std::size_t packet_header_size_ = 44;
};

class LiveRateController {
public:
    struct Configuration {
        std::uint64_t input_bandwidth_bytes_per_second = 0;
        std::uint64_t minimum_input_bandwidth_bytes_per_second = 0;
        // Zero selects the relative/input-rate mode for direct core users.
        // SocketOptions supplies the public SRTO_MAXBW default of -1.
        std::int64_t maximum_bandwidth_bytes_per_second = 0;
        std::uint32_t overhead_percent = 25;
        std::size_t maximum_payload_size = 1316;
        std::size_t packet_header_size = 44;
    };

    explicit LiveRateController(Configuration configuration) noexcept;
    void observe_input(std::size_t payload_bytes,
        std::uint64_t enqueue_microseconds) noexcept;
    void observe_payload(std::size_t payload_bytes) noexcept;
    void set_input_bandwidth(std::uint64_t bytes_per_second) noexcept;
    void set_minimum_input_bandwidth(
        std::uint64_t bytes_per_second) noexcept;
    void set_maximum_bandwidth(std::int64_t bytes_per_second) noexcept;
    void set_overhead_percent(std::uint32_t percent) noexcept;
    [[nodiscard]] std::uint64_t pacing_rate_bytes_per_second() const noexcept;
    [[nodiscard]] std::uint64_t packet_interval_microseconds() const noexcept;
    [[nodiscard]] std::size_t average_payload_size() const noexcept
    {
        return average_payload_size_;
    }

private:
    Configuration configuration_;
    std::size_t average_payload_size_ = 1;
    std::uint64_t estimated_input_bandwidth_ = 0;
    std::uint64_t input_window_start_microseconds_ = 0;
    std::uint64_t input_window_bytes_ = 0;
    bool input_window_started_ = false;
};

struct DriftUpdate {
    bool window_complete = false;
    std::int64_t correction_microseconds = 0;
    std::int64_t residual_drift_microseconds = 0;
};

class DriftSampler {
public:
    explicit DriftSampler(std::size_t samples_per_window = 1000,
        std::int64_t maximum_correction_microseconds = 5'000) noexcept;
    [[nodiscard]] DriftUpdate observe(std::int64_t drift_microseconds) noexcept;
    [[nodiscard]] std::int64_t residual_drift_microseconds() const noexcept
    {
        return residual_drift_microseconds_;
    }

private:
    std::size_t samples_per_window_ = 1000;
    std::int64_t maximum_correction_microseconds_ = 5'000;
    std::size_t samples_ = 0;
    std::int64_t sum_microseconds_ = 0;
    std::int64_t estimated_drift_microseconds_ = 0;
    std::int64_t residual_drift_microseconds_ = 0;
};

} // namespace robotweax::srt
