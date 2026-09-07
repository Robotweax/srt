#include "test.hpp"

#include "robotweax/srt/timing.hpp"

using namespace robotweax::srt;

TEST(control_timer_uses_reference_ack_and_keepalive_intervals)
{
    ControlTimerScheduler scheduler{0};
    scheduler.on_data_received(1);
    REQUIRE_EQ(scheduler.poll(9'999).size, 0U);
    const auto ack = scheduler.poll(10'000);
    REQUIRE_EQ(ack.size, 1U);
    REQUIRE_EQ(ack.values[0], TimerActionKind::full_acknowledgement);

    const auto keepalive = scheduler.poll(1'000'000);
    REQUIRE_EQ(keepalive.size, 1U);
    REQUIRE_EQ(keepalive.values[0], TimerActionKind::keepalive);
}

TEST(control_timer_immediately_advertises_reopened_receive_window)
{
    ControlTimerScheduler scheduler{0};
    scheduler.on_data_received(1);
    const auto full_buffer_ack = scheduler.poll(10'000);
    REQUIRE_EQ(full_buffer_ack.size, 1U);
    REQUIRE_EQ(full_buffer_ack.values[0],
        TimerActionKind::full_acknowledgement);

    scheduler.on_receive_buffer_released(10'001);
    const auto window_update = scheduler.poll(10'001);
    REQUIRE_EQ(window_update.size, 1U);
    REQUIRE_EQ(window_update.values[0],
        TimerActionKind::full_acknowledgement);
    REQUIRE_EQ(scheduler.poll(10'002).size, 0U);
}

TEST(control_timer_emits_self_clocked_lite_ack_and_periodic_nak)
{
    ControlTimerScheduler scheduler{0};
    for (std::size_t index = 0; index < 64; ++index) {
        scheduler.on_data_received(index);
    }
    const auto lite = scheduler.poll(1'000);
    REQUIRE_EQ(lite.values[0], TimerActionKind::lite_acknowledgement);

    ControlTimerScheduler loss_scheduler{2'000};
    loss_scheduler.set_loss_state(true, 2'000, 5'000);
    const auto immediate_nak = loss_scheduler.poll(2'000);
    REQUIRE_EQ(immediate_nak.values[0], TimerActionKind::periodic_loss_report);
    REQUIRE_EQ(loss_scheduler.poll(61'999).size, 0U);
    const auto repeated_nak = loss_scheduler.poll(62'000);
    REQUIRE_EQ(repeated_nak.values[0], TimerActionKind::periodic_loss_report);

    ControlTimerScheduler deferred_loss_scheduler{2'000};
    deferred_loss_scheduler.set_loss_state(
        true, 2'000, 5'000, true);
    REQUIRE_EQ(
        deferred_loss_scheduler.poll(2'000).size, 0U);
    REQUIRE_EQ(
        deferred_loss_scheduler.poll(61'999).size, 0U);
    const auto deferred_nak =
        deferred_loss_scheduler.poll(62'000);
    REQUIRE_EQ(deferred_nak.values[0],
        TimerActionKind::periodic_loss_report);
}

TEST(sender_retransmission_timer_uses_srt_formula_and_linear_backoff)
{
    SenderRetransmissionTimer timer{0};
    REQUIRE(!timer.armed());
    REQUIRE(!timer.next_deadline(100'000, 50'000).has_value());

    timer.on_data_packet_sent(100);
    REQUIRE(timer.armed());
    REQUIRE_EQ(*timer.next_deadline(100'000, 50'000), 330'100U);
    REQUIRE(!timer.poll(330'099, 100'000, 50'000));
    REQUIRE(timer.poll(330'100, 100'000, 50'000));
    REQUIRE_EQ(timer.timeout_multiplier(), 2U);
    REQUIRE_EQ(*timer.next_deadline(100'000, 50'000), 650'100U);
    REQUIRE(!timer.poll(650'099, 100'000, 50'000));
    REQUIRE(timer.poll(650'100, 100'000, 50'000));
    REQUIRE_EQ(timer.timeout_multiplier(), 3U);
}

TEST(sender_retransmission_timer_resets_on_ack_and_disarms_when_idle)
{
    SenderRetransmissionTimer timer{0};
    timer.on_data_packet_sent(100);
    REQUIRE(timer.poll(330'100, 100'000, 50'000));

    timer.on_acknowledgement_received(400'000, true);
    REQUIRE_EQ(timer.timeout_multiplier(), 1U);
    REQUIRE_EQ(*timer.next_deadline(100'000, 50'000), 730'000U);
    REQUIRE(!timer.poll(729'999, 100'000, 50'000));

    timer.on_acknowledgement_received(500'000, false);
    REQUIRE(!timer.armed());
    REQUIRE(!timer.poll(1'000'000, 100'000, 50'000));
}

TEST(packet_pacer_enforces_rate_and_flow_window)
{
    PacketPacer pacer{1'000, 4};
    REQUIRE(pacer.query(0, 0).ready);
    pacer.on_packet_sent(100, 0);
    REQUIRE(!pacer.query(99'999, 1).ready);
    REQUIRE(pacer.query(100'000, 1).ready);
    REQUIRE(!pacer.query(100'000, 4).ready);
}

TEST(tsbpd_clock_schedules_delivery_and_unwraps_timestamp_rollover)
{
    TsbpdClock clock{1'000'000, PacketTimestamp{100'000}, 120'000};
    REQUIRE_EQ(clock.delivery_time(PacketTimestamp{150'000}), 1'170'000U);
    REQUIRE(!clock.ready(PacketTimestamp{160'000}, 1'179'999));
    REQUIRE(clock.ready(PacketTimestamp{160'000}, 1'180'000));

    TsbpdClock wrap_clock{5'000'000, PacketTimestamp{0xffff'f000U}, 0};
    REQUIRE_EQ(wrap_clock.delivery_time(PacketTimestamp{0x0000'1000U}),
        5'008'192U);
}

TEST(tsbpd_clock_slews_bounded_drift_without_a_deadline_step)
{
    TsbpdClock clock{1'000'000, PacketTimestamp{100'000}, 120'000};
    DriftUpdate update;
    for (std::uint32_t index = 0; index < 1'000; ++index) {
        update = clock.observe_arrival(PacketTimestamp{100'000U + index},
            1'008'000U + index, 20'000);
    }
    REQUIRE(update.window_complete);
    REQUIRE_EQ(update.correction_microseconds, 5'000);
    REQUIRE_EQ(update.residual_drift_microseconds, 3'000);
    REQUIRE_EQ(clock.delivery_time(PacketTimestamp{101'000}), 1'121'001U);
    REQUIRE_EQ(clock.delivery_time(PacketTimestamp{102'000}), 1'122'002U);
}

TEST(tsbpd_clock_deadlines_stay_monotonic_across_drift_and_timestamp_wrap)
{
    constexpr std::uint32_t start_timestamp = 0xffff'f000U;
    TsbpdClock clock{5'000'000, PacketTimestamp{start_timestamp}, 0};
    DriftUpdate update;
    std::size_t completed_windows = 0;
    std::size_t correction_windows = 0;
    std::int64_t total_correction = 0;
    std::uint64_t previous_delivery = 0;
    for (std::uint32_t index = 0; index < 20'000; ++index) {
        const std::int64_t physical_drift = index < 10'000U ? 8'000 : -7'000;
        constexpr std::uint64_t timestamp_step = 1'400;
        const auto source_offset =
            static_cast<std::uint64_t>(index) * timestamp_step;
        const auto timestamp = PacketTimestamp{
            start_timestamp + static_cast<std::uint32_t>(source_offset)};
        const auto arrival = static_cast<std::uint64_t>(
            5'000'000 + source_offset + physical_drift);
        update = clock.observe_arrival(timestamp, arrival, 20'000);
        if (update.window_complete) {
            ++completed_windows;
            REQUIRE(update.correction_microseconds <= 5'000);
            REQUIRE(update.correction_microseconds >= -5'000);
            if (update.correction_microseconds != 0) {
                ++correction_windows;
                total_correction += update.correction_microseconds;
            }
        }
        const auto delivery = clock.delivery_time(timestamp);
        if (index != 0U) {
            REQUIRE(delivery >= previous_delivery);
            const auto interval = delivery - previous_delivery;
            REQUIRE(interval >= 1'398U);
            REQUIRE(interval <= 1'402U);
        }
        previous_delivery = delivery;
    }
    REQUIRE_EQ(completed_windows, 20U);
    REQUIRE_EQ(correction_windows, 5U);
    REQUIRE_EQ(total_correction, -7'000);
}

TEST(tsbpd_clock_sustained_source_rate_is_preserved_while_phase_converges)
{
    constexpr std::uint64_t timestamp_step = 1'400;
    constexpr std::uint32_t sample_count = 4'096;
    TsbpdClock clock{5'000'000, PacketTimestamp{0}, 600'000};

    std::uint64_t first_delivery = 0;
    std::uint64_t last_delivery = 0;
    for (std::uint32_t index = 0; index < sample_count; ++index) {
        const auto source_offset =
            static_cast<std::uint64_t>(index) * timestamp_step;
        const auto timestamp = PacketTimestamp{
            static_cast<std::uint32_t>(source_offset)};
        static_cast<void>(clock.observe_arrival(timestamp,
            4'960'000U + source_offset, 20'000));
        const auto delivery = clock.delivery_time(timestamp);
        if (index == 0U) {
            first_delivery = delivery;
        }
        last_delivery = delivery;
    }

    const std::uint64_t source_span =
        static_cast<std::uint64_t>(sample_count - 1U) * timestamp_step;
    const std::uint64_t delivery_span = last_delivery - first_delivery;
    const std::uint64_t absolute_span_error = source_span > delivery_span
        ? source_span - delivery_span
        : delivery_span - source_span;
    const std::uint64_t rate_error_parts_per_million =
        absolute_span_error * 1'000'000U / source_span;
    REQUIRE(rate_error_parts_per_million <= 1'000U);
}
