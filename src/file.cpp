#include "robotweax/srt/file.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace robotweax::srt {
namespace {

[[nodiscard]] double smoothed(double current, std::uint32_t sample) noexcept
{
    if (sample == 0U) {
        return current;
    }
    if (current == 0.0) {
        return static_cast<double>(sample);
    }
    return current * 0.875 + static_cast<double>(sample) * 0.125;
}

} // namespace

FileRateController::FileRateController(
    SequenceNumber initial_acknowledgement) noexcept
    : FileRateController(initial_acknowledgement, Configuration{})
{
}

FileRateController::FileRateController(
    SequenceNumber initial_acknowledgement,
    Configuration configuration) noexcept
    : configuration_({
          .maximum_congestion_window_packets =
              std::max<std::size_t>(
                  configuration.maximum_congestion_window_packets, 16U),
          .packet_size_bytes =
              std::max<std::size_t>(configuration.packet_size_bytes, 1U),
          .maximum_bandwidth_bytes_per_second =
              configuration.maximum_bandwidth_bytes_per_second,
          .rate_control_interval_microseconds =
              std::max<std::uint64_t>(
                  configuration.rate_control_interval_microseconds, 1U),
          .start_microseconds = configuration.start_microseconds,
      })
    , last_acknowledgement_(initial_acknowledgement)
    , observed_acknowledgement_(initial_acknowledgement)
    , last_decrease_sequence_(initial_acknowledgement.advanced(
          SequenceNumber::mask))
    , last_rate_control_microseconds_(configuration_.start_microseconds)
    , random_state_(0x6d2b'79f5U ^ initial_acknowledgement.value())
{
}

void FileRateController::observe_acknowledgement_rate(
    SequenceNumber next_sequence,
    std::uint64_t now_microseconds) noexcept
{
    const std::int32_t progress =
        next_sequence.distance_from(observed_acknowledgement_);
    if (progress > 0) {
        const auto acknowledged = static_cast<std::uint64_t>(progress);
        const auto maximum =
            std::numeric_limits<std::uint64_t>::max();
        if (acknowledged_packets_ > maximum - acknowledged) {
            acknowledged_packets_ = maximum;
        } else {
            acknowledged_packets_ += acknowledged;
        }
        observed_acknowledgement_ = next_sequence;
    }
    if (now_microseconds <= configuration_.start_microseconds
        || acknowledged_packets_ == 0U) {
        return;
    }
    const std::uint64_t elapsed =
        now_microseconds - configuration_.start_microseconds;
    acknowledged_rate_packets_per_second_ =
        static_cast<double>(acknowledged_packets_)
        * 1'000'000.0 / static_cast<double>(elapsed);
}

void FileRateController::update_feedback(
    std::uint32_t receive_rate_packets_per_second,
    std::uint32_t estimated_link_capacity_packets_per_second) noexcept
{
    receive_rate_packets_per_second_ = smoothed(
        receive_rate_packets_per_second_,
        receive_rate_packets_per_second);
    estimated_link_capacity_packets_per_second_ = smoothed(
        estimated_link_capacity_packets_per_second_,
        estimated_link_capacity_packets_per_second);
}

void FileRateController::leave_slow_start(
    std::uint32_t receive_rate_packets_per_second,
    std::uint32_t round_trip_time_microseconds) noexcept
{
    if (!slow_start_) {
        return;
    }
    slow_start_ = false;
    const double peer_rate =
        receive_rate_packets_per_second != 0U
        ? static_cast<double>(receive_rate_packets_per_second)
        : receive_rate_packets_per_second_;
    const double feedback_interval =
        static_cast<double>(round_trip_time_microseconds)
        + static_cast<double>(
            configuration_.rate_control_interval_microseconds);
    const double window_rate =
        congestion_window_packets_ * 1'000'000.0
        / std::max(feedback_interval, 1.0);
    // A peer may publish a non-zero arrival-rate field before its estimator
    // has a representative sample. Locally acknowledged progress proves an
    // already sustained rate, but cap that evidence at one congestion window
    // per RTT plus SYN interval so an ACK burst cannot inflate the result.
    const double acknowledgement_floor =
        std::min(acknowledged_rate_packets_per_second_, window_rate);
    const double receiving_rate =
        std::max(peer_rate, acknowledgement_floor);
    if (receiving_rate > 0.0) {
        packet_sending_period_microseconds_ =
            1'000'000.0 / receiving_rate;
    } else {
        // This is the fallback formula specified by SRT FileCC and used by
        // the reference implementation when no receiver rate is available.
        packet_sending_period_microseconds_ =
            1'000'000.0 / window_rate;
    }
    packet_sending_period_microseconds_ =
        std::max(packet_sending_period_microseconds_, 1.0);
}

void FileRateController::on_ack(
    SequenceNumber next_sequence,
    std::uint64_t now_microseconds,
    std::uint32_t receive_rate_packets_per_second,
    std::uint32_t estimated_link_capacity_packets_per_second,
    std::uint32_t round_trip_time_microseconds) noexcept
{
    if (slow_start_) {
        observe_acknowledgement_rate(next_sequence, now_microseconds);
    }
    update_feedback(receive_rate_packets_per_second,
        estimated_link_capacity_packets_per_second);
    if (now_microseconds < last_rate_control_microseconds_
        || now_microseconds - last_rate_control_microseconds_
            < configuration_.rate_control_interval_microseconds) {
        return;
    }
    last_rate_control_microseconds_ = now_microseconds;

    if (slow_start_) {
        const std::int32_t acknowledged =
            next_sequence.distance_from(last_acknowledgement_);
        if (acknowledged > 0) {
            congestion_window_packets_ +=
                static_cast<double>(acknowledged);
            last_acknowledgement_ = next_sequence;
        }
        if (congestion_window_packets_
            > static_cast<double>(
                configuration_.maximum_congestion_window_packets)) {
            leave_slow_start(receive_rate_packets_per_second,
                round_trip_time_microseconds);
        }
    } else {
        congestion_window_packets_ =
            receive_rate_packets_per_second_
                * (static_cast<double>(round_trip_time_microseconds)
                    + static_cast<double>(
                        configuration_.rate_control_interval_microseconds))
                / 1'000'000.0
            + 16.0;
        congestion_window_packets_ = std::clamp(
            congestion_window_packets_, 16.0,
            static_cast<double>(
                configuration_.maximum_congestion_window_packets));

        if (loss_since_rate_increase_) {
            loss_since_rate_increase_ = false;
        } else {
            const double effective_period =
                packet_sending_period_microseconds();
            const double loss_bandwidth =
                2.0 * (1'000'000.0
                    / std::max(last_decrease_period_microseconds_, 1.0));
            const double link_capacity =
                estimated_link_capacity_packets_per_second_ > 0.0
                ? std::min(loss_bandwidth,
                      estimated_link_capacity_packets_per_second_)
                : loss_bandwidth;
            double available = link_capacity
                - 1'000'000.0
                    / std::max(effective_period, 1.0);
            if (effective_period > last_decrease_period_microseconds_
                && link_capacity / 9.0 < available) {
                available = link_capacity / 9.0;
            }

            const double inverse_packet_size =
                1.0 / static_cast<double>(
                    configuration_.packet_size_bytes);
            double increase = inverse_packet_size;
            if (available > 0.0) {
                increase = std::pow(10.0,
                    std::ceil(std::log10(
                        available
                        * static_cast<double>(
                            configuration_.packet_size_bytes)
                        * 8.0)))
                    * 0.0000015
                    / static_cast<double>(
                        configuration_.packet_size_bytes);
                increase = std::max(increase, inverse_packet_size);
            }
            const double interval = static_cast<double>(
                configuration_.rate_control_interval_microseconds);
            packet_sending_period_microseconds_ =
                (packet_sending_period_microseconds_ * interval)
                / (packet_sending_period_microseconds_ * increase
                    + interval);
        }
    }
}

void FileRateController::on_loss(
    SequenceNumber first_lost_sequence,
    SequenceNumber last_sent_sequence,
    std::size_t lost_packets,
    std::size_t packets_in_flight,
    std::uint32_t receive_rate_packets_per_second,
    std::uint32_t round_trip_time_microseconds) noexcept
{
    update_feedback(receive_rate_packets_per_second, 0);
    leave_slow_start(receive_rate_packets_per_second,
        round_trip_time_microseconds);
    loss_since_rate_increase_ = true;
    const double effective_period =
        packet_sending_period_microseconds();

    const std::size_t loss_ratio_times_ten =
        packets_in_flight == 0U
        ? 0U
        : lost_packets * 1'000U / packets_in_flight;
    if (loss_ratio_times_ten < 20U) {
        last_decrease_period_microseconds_ =
            effective_period;
        return;
    }

    if (first_lost_sequence.is_after(last_decrease_sequence_)) {
        last_decrease_period_microseconds_ =
            effective_period;
        packet_sending_period_microseconds_ = std::ceil(
            effective_period * 1.03);
        average_nak_count_ = static_cast<std::uint32_t>(std::ceil(
            static_cast<double>(average_nak_count_) * 0.97
            + static_cast<double>(nak_count_) * 0.03));
        nak_count_ = 1;
        decrease_count_ = 1;
        last_decrease_sequence_ = last_sent_sequence;
        decrease_random_threshold_ =
            average_nak_count_ > 1U
            ? next_random(average_nak_count_)
            : 1U;
    } else {
        ++decrease_count_;
        ++nak_count_;
        if (decrease_count_ <= 5U
            && nak_count_ % decrease_random_threshold_ == 0U) {
            packet_sending_period_microseconds_ = std::ceil(
                std::max(
                    packet_sending_period_microseconds_,
                    effective_period)
                * 1.03);
            last_decrease_sequence_ = last_sent_sequence;
        }
    }
}

void FileRateController::on_timeout(
    std::uint32_t receive_rate_packets_per_second,
    std::uint32_t round_trip_time_microseconds) noexcept
{
    update_feedback(receive_rate_packets_per_second, 0);
    leave_slow_start(receive_rate_packets_per_second,
        round_trip_time_microseconds);
}

void FileRateController::set_maximum_bandwidth(
    std::uint64_t bytes_per_second) noexcept
{
    configuration_.maximum_bandwidth_bytes_per_second =
        bytes_per_second;
}

double FileRateController::packet_sending_period_microseconds() const noexcept
{
    if (configuration_.maximum_bandwidth_bytes_per_second == 0U) {
        return packet_sending_period_microseconds_;
    }
    const double minimum_period =
        static_cast<double>(configuration_.packet_size_bytes)
        * 1'000'000.0
        / static_cast<double>(
            configuration_.maximum_bandwidth_bytes_per_second);
    return std::max(
        packet_sending_period_microseconds_, minimum_period);
}

std::size_t FileRateController::congestion_window_packets() const noexcept
{
    const double limited = std::clamp(
        congestion_window_packets_, 1.0,
        static_cast<double>(std::numeric_limits<std::size_t>::max()));
    return static_cast<std::size_t>(limited);
}

std::uint64_t FileRateController::pacing_rate_bytes_per_second() const noexcept
{
    const long double rate =
        static_cast<long double>(configuration_.packet_size_bytes)
        * 1'000'000.0L
        / static_cast<long double>(
            std::max(packet_sending_period_microseconds(), 1.0));
    return static_cast<std::uint64_t>(std::clamp<long double>(
        rate, 1.0L,
        static_cast<long double>(
            std::numeric_limits<std::uint64_t>::max())));
}

std::uint32_t FileRateController::next_random(
    std::uint32_t upper_inclusive) noexcept
{
    random_state_ ^= random_state_ << 13U;
    random_state_ ^= random_state_ >> 17U;
    random_state_ ^= random_state_ << 5U;
    return random_state_ % upper_inclusive + 1U;
}

} // namespace robotweax::srt
