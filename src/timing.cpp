#include "robotweax/srt/timing.hpp"

#include <algorithm>
#include <limits>
#include <mutex>

namespace robotweax::srt {

struct TsbpdClockState {
    explicit TsbpdClockState(const TsbpdClock& initial)
        : clock(initial)
    {
    }
    std::mutex mutex;
    TsbpdClock clock;
};

void TsbpdClock::share_group_clock(std::shared_ptr<TsbpdClockState>& state)
{
    if (!state) {
        state = std::make_shared<TsbpdClockState>(*this);
    } else {
        std::lock_guard lock(state->mutex);
        // A late member must not shorten any member's negotiated buffering.
        state->clock.delay_microseconds_ =
            std::max(state->clock.delay_microseconds_, delay_microseconds_);
    }
    shared_state_ = state;
}

namespace {

// Correcting receiver clock phase as a step can move queued TSBPD deadlines
// backwards and release a burst of already-due messages. A 1000 ppm slew is
// fast enough to track realistic oscillator error while preserving the source
// timeline to within one microsecond per millisecond.
constexpr std::uint64_t maximum_tsbpd_slew_parts_per_million = 1'000;
constexpr std::uint64_t parts_per_million = 1'000'000;

} // namespace

ControlTimerScheduler::ControlTimerScheduler(std::uint64_t start_microseconds)
    : ControlTimerScheduler(start_microseconds, Configuration{})
{
}

ControlTimerScheduler::ControlTimerScheduler(std::uint64_t start_microseconds,
    Configuration configuration)
    : configuration_({
          .acknowledgement_interval_microseconds = std::max<std::uint64_t>(
              configuration.acknowledgement_interval_microseconds, 1U),
          .keepalive_interval_microseconds = std::max<std::uint64_t>(
              configuration.keepalive_interval_microseconds, 1U),
          .lite_acknowledgement_packet_interval = std::max<std::uint32_t>(
              configuration.lite_acknowledgement_packet_interval, 1U),
      })
    , next_ack_microseconds_(start_microseconds
          + configuration_.acknowledgement_interval_microseconds)
    , next_nak_microseconds_(
          start_microseconds + minimum_nak_interval_microseconds)
    , last_send_microseconds_(start_microseconds)
    , next_lite_ack_threshold_(configuration_.lite_acknowledgement_packet_interval)
{
}

void ControlTimerScheduler::on_data_received(std::uint64_t) noexcept
{
    acknowledgement_dirty_ = true;
    ++packets_since_full_ack_;
}

void ControlTimerScheduler::on_receive_buffer_released(
    std::uint64_t now_microseconds) noexcept
{
    // A peer that stopped at an advertised zero receive window needs a fresh
    // full ACK as soon as the application makes space available. This event
    // does not count as a newly received packet and therefore must not advance
    // the lite-ACK threshold.
    acknowledgement_dirty_ = true;
    next_ack_microseconds_ =
        std::min(next_ack_microseconds_, now_microseconds);
}

void ControlTimerScheduler::on_packet_sent(std::uint64_t now_microseconds) noexcept
{
    last_send_microseconds_ = now_microseconds;
}

void ControlTimerScheduler::set_loss_state(bool has_loss,
    std::uint64_t now_microseconds,
    std::uint32_t nak_interval_microseconds,
    bool defer_initial_report) noexcept
{
    nak_interval_microseconds_ =
        std::max(
            nak_interval_microseconds,
            minimum_nak_interval_microseconds);
    if (has_loss && !has_loss_) {
        next_nak_microseconds_ = now_microseconds;
        if (defer_initial_report) {
            next_nak_microseconds_ =
                now_microseconds
                    > std::numeric_limits<std::uint64_t>::max()
                        - nak_interval_microseconds_
                ? std::numeric_limits<std::uint64_t>::max()
                : now_microseconds + nak_interval_microseconds_;
        }
    }
    has_loss_ = has_loss;
}

TimerActions ControlTimerScheduler::poll(std::uint64_t now_microseconds) noexcept
{
    TimerActions actions;
    if (acknowledgement_dirty_
        && now_microseconds >= next_ack_microseconds_) {
        actions.push(TimerActionKind::full_acknowledgement);
        acknowledgement_dirty_ = false;
        packets_since_full_ack_ = 0;
        next_lite_ack_threshold_ = configuration_.lite_acknowledgement_packet_interval;
        next_ack_microseconds_ = now_microseconds
            + configuration_.acknowledgement_interval_microseconds;
    } else if (acknowledgement_dirty_
        && packets_since_full_ack_ >= next_lite_ack_threshold_) {
        actions.push(TimerActionKind::lite_acknowledgement);
        next_lite_ack_threshold_ += configuration_.lite_acknowledgement_packet_interval;
    }
    if (has_loss_ && now_microseconds >= next_nak_microseconds_) {
        actions.push(TimerActionKind::periodic_loss_report);
        next_nak_microseconds_ = now_microseconds + nak_interval_microseconds_;
    }
    if (now_microseconds >= last_send_microseconds_
            + configuration_.keepalive_interval_microseconds) {
        actions.push(TimerActionKind::keepalive);
        last_send_microseconds_ = now_microseconds;
    }
    return actions;
}

void SenderRetransmissionTimer::on_data_packet_sent(
    std::uint64_t now_microseconds) noexcept
{
    if (armed_) {
        return;
    }
    last_acknowledgement_microseconds_ = now_microseconds;
    timeout_multiplier_ = 1;
    armed_ = true;
}

void SenderRetransmissionTimer::on_acknowledgement_received(
    std::uint64_t now_microseconds,
    bool has_packets_in_flight) noexcept
{
    last_acknowledgement_microseconds_ = now_microseconds;
    timeout_multiplier_ = 1;
    armed_ = has_packets_in_flight;
}

void SenderRetransmissionTimer::on_no_packets_in_flight() noexcept
{
    timeout_multiplier_ = 1;
    armed_ = false;
}

std::uint64_t SenderRetransmissionTimer::timeout_interval(
    std::uint32_t smoothed_round_trip_time_microseconds,
    std::uint32_t round_trip_time_variation_microseconds) const noexcept
{
    constexpr std::uint64_t synchronization_interval_microseconds = 10'000;
    const std::uint64_t round_trip_timeout =
        static_cast<std::uint64_t>(smoothed_round_trip_time_microseconds)
        + 4ULL * round_trip_time_variation_microseconds
        + 2ULL * synchronization_interval_microseconds;
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    if (round_trip_timeout != 0U
        && timeout_multiplier_
            > (maximum - synchronization_interval_microseconds)
                / round_trip_timeout) {
        return maximum;
    }
    return static_cast<std::uint64_t>(timeout_multiplier_)
            * round_trip_timeout
        + synchronization_interval_microseconds;
}

std::optional<std::uint64_t> SenderRetransmissionTimer::next_deadline(
    std::uint32_t smoothed_round_trip_time_microseconds,
    std::uint32_t round_trip_time_variation_microseconds) const noexcept
{
    if (!armed_) {
        return std::nullopt;
    }
    const auto interval = timeout_interval(
        smoothed_round_trip_time_microseconds,
        round_trip_time_variation_microseconds);
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    if (interval > maximum - last_acknowledgement_microseconds_) {
        return maximum;
    }
    return last_acknowledgement_microseconds_ + interval;
}

bool SenderRetransmissionTimer::poll(
    std::uint64_t now_microseconds,
    std::uint32_t smoothed_round_trip_time_microseconds,
    std::uint32_t round_trip_time_variation_microseconds) noexcept
{
    const auto deadline = next_deadline(
        smoothed_round_trip_time_microseconds,
        round_trip_time_variation_microseconds);
    if (!deadline.has_value() || now_microseconds < *deadline) {
        return false;
    }
    if (timeout_multiplier_ != std::numeric_limits<std::uint32_t>::max()) {
        ++timeout_multiplier_;
    }
    return true;
}

PacketPacer::PacketPacer(std::uint64_t bytes_per_second,
    std::size_t flow_window_packets) noexcept
    : bytes_per_second_(std::max<std::uint64_t>(bytes_per_second, 1U))
    , flow_window_packets_(std::max<std::size_t>(flow_window_packets, 1U))
{
}

PaceDecision PacketPacer::query(std::uint64_t now_microseconds,
    std::size_t packets_in_flight) const noexcept
{
    return {
        .ready = packets_in_flight < flow_window_packets_
            && now_microseconds >= next_send_microseconds_,
        .next_ready_microseconds = next_send_microseconds_,
    };
}

void PacketPacer::on_packet_sent(std::size_t bytes,
    std::uint64_t now_microseconds) noexcept
{
    const std::uint64_t interval = std::max<std::uint64_t>(1U,
        (static_cast<std::uint64_t>(bytes) * 1'000'000ULL
            + bytes_per_second_ - 1U) / bytes_per_second_);
    next_send_microseconds_ = std::max(next_send_microseconds_, now_microseconds)
        + interval;
}

void PacketPacer::set_rate(std::uint64_t bytes_per_second) noexcept
{
    bytes_per_second_ = std::max<std::uint64_t>(bytes_per_second, 1U);
}

void PacketPacer::set_flow_window(std::size_t packets) noexcept
{
    flow_window_packets_ = std::max<std::size_t>(packets, 1U);
}

TsbpdClock::TsbpdClock(std::uint64_t handshake_arrival_microseconds,
    PacketTimestamp handshake_timestamp,
    std::uint32_t delay_microseconds) noexcept
    : time_base_microseconds_(
          static_cast<std::int64_t>(handshake_arrival_microseconds)
          - static_cast<std::int64_t>(handshake_timestamp.value()))
    , latest_unwrapped_timestamp_(handshake_timestamp.value())
    , last_slew_timestamp_(handshake_timestamp.value())
    , delay_microseconds_(delay_microseconds)
{
}

std::uint64_t TsbpdClock::unwrap(PacketTimestamp timestamp) noexcept
{
    constexpr std::uint64_t modulus = 0x1'0000'0000ULL;
    constexpr std::uint64_t half = modulus / 2ULL;
    const std::uint64_t epoch = latest_unwrapped_timestamp_ & ~(modulus - 1ULL);
    std::uint64_t candidate = epoch + timestamp.value();
    if (candidate + half < latest_unwrapped_timestamp_) {
        candidate += modulus;
    } else if (candidate > latest_unwrapped_timestamp_ + half
        && candidate >= modulus) {
        candidate -= modulus;
    }
    latest_unwrapped_timestamp_ = std::max(latest_unwrapped_timestamp_, candidate);
    return candidate;
}

void TsbpdClock::advance_applied_drift(
    std::uint64_t unwrapped_timestamp) noexcept
{
    if (unwrapped_timestamp <= last_slew_timestamp_) {
        return;
    }
    const std::uint64_t elapsed =
        unwrapped_timestamp - last_slew_timestamp_;
    last_slew_timestamp_ = unwrapped_timestamp;

    if (applied_drift_microseconds_ == target_drift_microseconds_) {
        slew_remainder_ = 0;
        return;
    }

    const std::uint64_t scaled_budget =
        elapsed * maximum_tsbpd_slew_parts_per_million + slew_remainder_;
    const std::uint64_t budget = scaled_budget / parts_per_million;
    slew_remainder_ = scaled_budget % parts_per_million;
    if (budget == 0U) {
        return;
    }

    const std::int64_t difference =
        target_drift_microseconds_ - applied_drift_microseconds_;
    const std::uint64_t magnitude = difference < 0
        ? static_cast<std::uint64_t>(-difference)
        : static_cast<std::uint64_t>(difference);
    const auto step = static_cast<std::int64_t>(
        std::min(budget, magnitude));
    applied_drift_microseconds_ += difference < 0 ? -step : step;
    if (applied_drift_microseconds_ == target_drift_microseconds_) {
        slew_remainder_ = 0;
    }
}

std::uint64_t TsbpdClock::delivery_time(PacketTimestamp timestamp) noexcept
{
    if (shared_state_) {
        std::lock_guard lock(shared_state_->mutex);
        return shared_state_->clock.delivery_time(timestamp);
    }
    const auto unwrapped = unwrap(timestamp);
    advance_applied_drift(unwrapped);
    const std::int64_t signed_delivery = time_base_microseconds_
        + static_cast<std::int64_t>(unwrapped)
        + delay_microseconds_ + applied_drift_microseconds_;
    return signed_delivery <= 0 ? 0U : static_cast<std::uint64_t>(signed_delivery);
}

bool TsbpdClock::ready(PacketTimestamp timestamp,
    std::uint64_t now_microseconds) noexcept
{
    return now_microseconds >= delivery_time(timestamp);
}

bool TsbpdClock::too_late(PacketTimestamp timestamp,
    std::uint64_t now_microseconds,
    std::uint32_t tolerance_microseconds) noexcept
{
    const auto delivery = delivery_time(timestamp);
    return now_microseconds > delivery
        && now_microseconds - delivery > tolerance_microseconds;
}

DriftUpdate TsbpdClock::observe_arrival(PacketTimestamp timestamp,
    std::uint64_t arrival_microseconds,
    std::uint32_t round_trip_time_microseconds) noexcept
{
    if (shared_state_) {
        std::lock_guard lock(shared_state_->mutex);
        return shared_state_->clock.observe_arrival(
            timestamp, arrival_microseconds, round_trip_time_microseconds);
    }
    if (!first_round_trip_time_microseconds_.has_value()) {
        first_round_trip_time_microseconds_ = round_trip_time_microseconds;
    }
    const auto unwrapped = unwrap(timestamp);
    const auto rtt_delta = (static_cast<std::int64_t>(round_trip_time_microseconds)
        - static_cast<std::int64_t>(*first_round_trip_time_microseconds_)) / 2;
    const auto expected_arrival = time_base_microseconds_
        + static_cast<std::int64_t>(unwrapped) + rtt_delta;
    const auto update = drift_sampler_.observe(
        static_cast<std::int64_t>(arrival_microseconds) - expected_arrival);
    if (update.window_complete) {
        target_drift_microseconds_ += update.correction_microseconds;
    }
    return update;
}

} // namespace robotweax::srt
