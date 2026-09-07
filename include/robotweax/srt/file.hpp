#pragma once

#include "robotweax/srt/sequence.hpp"

#include <cstddef>
#include <cstdint>

namespace robotweax::srt {

// SRT FileCC is a packet-window and packet-period controller. It intentionally
// remains independent from the socket/runtime layer so its state transitions
// can be tested with a deterministic clock and synthetic ACK/NAK feedback.
class FileRateController {
public:
    struct Configuration {
        std::size_t maximum_congestion_window_packets = 25'600;
        std::size_t packet_size_bytes = 1'500;
        std::uint64_t maximum_bandwidth_bytes_per_second = 0;
        std::uint64_t rate_control_interval_microseconds = 10'000;
        std::uint64_t start_microseconds = 0;
    };

    explicit FileRateController(
        SequenceNumber initial_acknowledgement = {}) noexcept;
    FileRateController(
        SequenceNumber initial_acknowledgement,
        Configuration configuration) noexcept;

    void on_ack(
        SequenceNumber next_sequence,
        std::uint64_t now_microseconds,
        std::uint32_t receive_rate_packets_per_second,
        std::uint32_t estimated_link_capacity_packets_per_second,
        std::uint32_t round_trip_time_microseconds) noexcept;
    void on_loss(
        SequenceNumber first_lost_sequence,
        SequenceNumber last_sent_sequence,
        std::size_t lost_packets,
        std::size_t packets_in_flight,
        std::uint32_t receive_rate_packets_per_second,
        std::uint32_t round_trip_time_microseconds) noexcept;
    void on_timeout(
        std::uint32_t receive_rate_packets_per_second,
        std::uint32_t round_trip_time_microseconds) noexcept;
    void set_maximum_bandwidth(
        std::uint64_t bytes_per_second) noexcept;

    [[nodiscard]] bool slow_start() const noexcept
    {
        return slow_start_;
    }
    [[nodiscard]] std::size_t congestion_window_packets() const noexcept;
    [[nodiscard]] double packet_sending_period_microseconds() const noexcept;
    [[nodiscard]] std::uint64_t pacing_rate_bytes_per_second() const noexcept;

private:
    void observe_acknowledgement_rate(
        SequenceNumber next_sequence,
        std::uint64_t now_microseconds) noexcept;
    void update_feedback(
        std::uint32_t receive_rate_packets_per_second,
        std::uint32_t estimated_link_capacity_packets_per_second) noexcept;
    void leave_slow_start(
        std::uint32_t receive_rate_packets_per_second,
        std::uint32_t round_trip_time_microseconds) noexcept;
    [[nodiscard]] std::uint32_t next_random(
        std::uint32_t upper_inclusive) noexcept;

    Configuration configuration_{};
    double congestion_window_packets_ = 16.0;
    double packet_sending_period_microseconds_ = 1.0;
    double last_decrease_period_microseconds_ = 1.0;
    double receive_rate_packets_per_second_ = 0.0;
    double estimated_link_capacity_packets_per_second_ = 0.0;
    double acknowledged_rate_packets_per_second_ = 0.0;
    SequenceNumber last_acknowledgement_{};
    SequenceNumber observed_acknowledgement_{};
    SequenceNumber last_decrease_sequence_{};
    std::uint64_t acknowledged_packets_ = 0;
    std::uint64_t last_rate_control_microseconds_ = 0;
    std::uint32_t random_state_ = 0x6d2b'79f5U;
    std::uint32_t nak_count_ = 0;
    std::uint32_t average_nak_count_ = 0;
    std::uint32_t decrease_random_threshold_ = 1;
    std::uint32_t decrease_count_ = 0;
    bool slow_start_ = true;
    bool loss_since_rate_increase_ = false;
};

} // namespace robotweax::srt
