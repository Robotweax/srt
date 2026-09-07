#pragma once

#include "robotweax/srt/live.hpp"
#include "robotweax/srt/rtt.hpp"
#include "robotweax/srt/sequence.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace robotweax::srt {

struct TsbpdClockState;
namespace compat {
class ConnectionRuntime;
}

enum class TimerActionKind : std::uint8_t {
    full_acknowledgement,
    lite_acknowledgement,
    periodic_loss_report,
    keepalive,
};

struct TimerActions {
    std::array<TimerActionKind, 4> values{};
    std::size_t size = 0;

    constexpr void push(TimerActionKind action) noexcept
    {
        if (size < values.size()) {
            values[size++] = action;
        }
    }
};

class ControlTimerScheduler {
public:
    struct Configuration {
        std::uint64_t acknowledgement_interval_microseconds = 10'000;
        std::uint64_t keepalive_interval_microseconds = 1'000'000;
        std::uint32_t lite_acknowledgement_packet_interval = 64;
    };

    explicit ControlTimerScheduler(std::uint64_t start_microseconds);
    ControlTimerScheduler(std::uint64_t start_microseconds,
        Configuration configuration);

    void on_data_received(std::uint64_t now_microseconds) noexcept;
    void on_receive_buffer_released(
        std::uint64_t now_microseconds) noexcept;
    void on_packet_sent(std::uint64_t now_microseconds) noexcept;
    void set_loss_state(bool has_loss, std::uint64_t now_microseconds,
        std::uint32_t nak_interval_microseconds,
        bool defer_initial_report = false) noexcept;
    [[nodiscard]] TimerActions poll(std::uint64_t now_microseconds) noexcept;

private:
    Configuration configuration_;
    std::uint64_t next_ack_microseconds_ = 0;
    std::uint64_t next_nak_microseconds_ = 0;
    std::uint64_t last_send_microseconds_ = 0;
    std::uint32_t packets_since_full_ack_ = 0;
    std::uint32_t next_lite_ack_threshold_ = 64;
    std::uint32_t nak_interval_microseconds_ =
        minimum_nak_interval_microseconds;
    bool acknowledgement_dirty_ = false;
    bool has_loss_ = false;
};

// Tracks the sender-side acknowledgement timeout independently from loss
// detection and congestion control. SRT grows the timeout linearly after
// every consecutive expiration:
//
//   timeout = count * (SRTT + 4 * RTTVar + 2 * SYN) + SYN
//
// where SYN is the 10 ms control interval. An accepted cumulative ACK resets
// the count, while an idle sender disarms the timer.
class SenderRetransmissionTimer {
public:
    explicit SenderRetransmissionTimer(
        std::uint64_t start_microseconds = 0) noexcept
        : last_acknowledgement_microseconds_(start_microseconds)
    {
    }

    void on_data_packet_sent(std::uint64_t now_microseconds) noexcept;
    void on_acknowledgement_received(
        std::uint64_t now_microseconds,
        bool has_packets_in_flight) noexcept;
    void on_no_packets_in_flight() noexcept;

    [[nodiscard]] bool poll(
        std::uint64_t now_microseconds,
        std::uint32_t smoothed_round_trip_time_microseconds,
        std::uint32_t round_trip_time_variation_microseconds) noexcept;
    [[nodiscard]] std::optional<std::uint64_t> next_deadline(
        std::uint32_t smoothed_round_trip_time_microseconds,
        std::uint32_t round_trip_time_variation_microseconds) const noexcept;
    [[nodiscard]] std::uint32_t timeout_multiplier() const noexcept
    {
        return timeout_multiplier_;
    }
    [[nodiscard]] bool armed() const noexcept { return armed_; }

private:
    [[nodiscard]] std::uint64_t timeout_interval(
        std::uint32_t smoothed_round_trip_time_microseconds,
        std::uint32_t round_trip_time_variation_microseconds) const noexcept;

    std::uint64_t last_acknowledgement_microseconds_ = 0;
    std::uint32_t timeout_multiplier_ = 1;
    bool armed_ = false;
};

struct PaceDecision {
    bool ready = false;
    std::uint64_t next_ready_microseconds = 0;
};

class PacketPacer {
public:
    PacketPacer(std::uint64_t bytes_per_second,
        std::size_t flow_window_packets) noexcept;

    [[nodiscard]] PaceDecision query(std::uint64_t now_microseconds,
        std::size_t packets_in_flight) const noexcept;
    void on_packet_sent(std::size_t bytes,
        std::uint64_t now_microseconds) noexcept;
    void set_rate(std::uint64_t bytes_per_second) noexcept;
    void set_flow_window(std::size_t packets) noexcept;

private:
    std::uint64_t bytes_per_second_ = 1;
    std::size_t flow_window_packets_ = 1;
    std::uint64_t next_send_microseconds_ = 0;
};

class TsbpdClock {
public:
    TsbpdClock(std::uint64_t handshake_arrival_microseconds,
        PacketTimestamp handshake_timestamp,
        std::uint32_t delay_microseconds) noexcept;

    [[nodiscard]] std::uint64_t delivery_time(
        PacketTimestamp timestamp) noexcept;
    [[nodiscard]] bool ready(PacketTimestamp timestamp,
        std::uint64_t now_microseconds) noexcept;
    [[nodiscard]] bool too_late(PacketTimestamp timestamp,
        std::uint64_t now_microseconds,
        std::uint32_t tolerance_microseconds) noexcept;
    [[nodiscard]] DriftUpdate observe_arrival(PacketTimestamp timestamp,
        std::uint64_t arrival_microseconds,
        std::uint32_t round_trip_time_microseconds) noexcept;

private:
    friend class compat::ConnectionRuntime;
    // Only the compatibility runtime opts group members into this shared
    // clock. Ordinary connections retain their allocation-free local clock.
    void share_group_clock(std::shared_ptr<TsbpdClockState>& state);
    std::shared_ptr<TsbpdClockState> shared_state_;
    [[nodiscard]] std::uint64_t unwrap(PacketTimestamp timestamp) noexcept;
    void advance_applied_drift(std::uint64_t unwrapped_timestamp) noexcept;

    std::int64_t time_base_microseconds_ = 0;
    std::uint64_t latest_unwrapped_timestamp_ = 0;
    std::uint64_t last_slew_timestamp_ = 0;
    std::uint32_t delay_microseconds_ = 0;
    std::int64_t target_drift_microseconds_ = 0;
    std::int64_t applied_drift_microseconds_ = 0;
    std::uint64_t slew_remainder_ = 0;
    DriftSampler drift_sampler_{};
    std::optional<std::uint32_t> first_round_trip_time_microseconds_;
};

} // namespace robotweax::srt
