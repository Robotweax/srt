#include "test.hpp"

#include "robotweax/srt/timing.hpp"
#include "compat/submillisecond_pacing_platform.hpp"

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
    // A 5 ms interval is clamped to libsrt's 20 ms minimum.
    REQUIRE_EQ(loss_scheduler.poll(21'999).size, 0U);
    const auto repeated_nak = loss_scheduler.poll(22'000);
    REQUIRE_EQ(repeated_nak.values[0], TimerActionKind::periodic_loss_report);

    ControlTimerScheduler deferred_loss_scheduler{2'000};
    deferred_loss_scheduler.set_loss_state(
        true, 2'000, 5'000, true);
    REQUIRE_EQ(
        deferred_loss_scheduler.poll(2'000).size, 0U);
    REQUIRE_EQ(deferred_loss_scheduler.poll(21'999).size, 0U);
    const auto deferred_nak = deferred_loss_scheduler.poll(22'000);
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

#if ROBOTWEAX_SRT_SUBMILLISECOND_TIMER_PACING
TEST(packet_pacer_credit_keeps_the_average_rate_across_late_wake_ups)
{
    // 200 bytes at 2 MB/s: one slot every 100 us. Every send is 70 us late
    // (a late timer wake-up). The ideal schedule is kept, so the slots stay
    // 100 us apart and the tenth packet is still due at 1,000 us.
    PacketPacer pacer {2'000'000, 64};
    pacer.on_packet_sent(200, 0);
    std::uint64_t slot = 100;
    for (int index = 0; index < 10; ++index) {
        REQUIRE(!pacer.query(slot - 1, 0).ready);
        const auto decision = pacer.query(slot + 70, 0);
        REQUIRE(decision.ready);
        REQUIRE_EQ(decision.next_ready_microseconds, slot);
        pacer.on_packet_sent(200, slot + 70);
        slot += 100;
    }
    REQUIRE_EQ(pacer.query(slot, 0).next_ready_microseconds, slot);
}

TEST(packet_pacer_catch_up_is_bounded_by_the_credit_and_restarts_after_a_pause)
{
    PacketPacer pacer {2'000'000, 64};
    pacer.set_schedule_credit(300);
    // The first send starts the schedule at the send time: no burst from a
    // schedule that begins at protocol time zero.
    pacer.on_packet_sent(200, 5'000);
    REQUIRE(!pacer.query(5'099, 0).ready);
    REQUIRE(pacer.query(5'100, 0).ready);

    // Late by exactly the credit: the missed slots may be caught up back to
    // back, but no more than the credit's worth (three 100 us slots plus the
    // one that is due).
    const std::uint64_t late = 5'100 + 300;
    std::size_t sent = 0;
    while (pacer.query(late, 0).ready && sent < 10U) {
        pacer.on_packet_sent(200, late);
        ++sent;
    }
    REQUIRE_EQ(sent, 4U);
    REQUIRE_EQ(pacer.query(late, 0).next_ready_microseconds, 5'500U);

    // Later than the credit: a fresh schedule starts at the send time, so
    // the pause is not followed by a burst.
    const std::uint64_t resumed = 5'500 + 301;
    pacer.on_packet_sent(200, resumed);
    REQUIRE(!pacer.query(resumed + 99, 0).ready);
    REQUIRE(pacer.query(resumed + 100, 0).ready);
    REQUIRE_EQ(pacer.schedule_credit_microseconds(), 300U);
}

#else
TEST(packet_pacer_preserves_previous_schedule_on_windows_and_other_platforms)
{
    PacketPacer pacer {2'000'000, 64};
    pacer.set_schedule_credit(300);
    pacer.on_packet_sent(200, 5'000);
    REQUIRE_EQ(pacer.query(5'100, 0).next_ready_microseconds, 5'100U);

    // Even with credit configured, a late send starts the next slot at its
    // actual send time instead of catching up missed slots.
    pacer.on_packet_sent(200, 5'400);
    REQUIRE(!pacer.query(5'499, 0).ready);
    REQUIRE_EQ(pacer.query(5'500, 0).next_ready_microseconds, 5'500U);
    REQUIRE(pacer.query(5'500, 0).ready);
}
#endif

TEST(timer_wake_monitor_enters_coarse_mode_on_late_streaks_and_recovers)
{
    TimerWakeMonitor monitor;
    REQUIRE(!monitor.coarse());
    // Isolated late wake-ups do not flip the mode.
    monitor.observe(1'500);
    monitor.observe(1'500);
    monitor.observe(100);
    REQUIRE(!monitor.coarse());
    for (unsigned index = 0; index < TimerWakeMonitor::late_streak_to_enter;
        ++index) {
        monitor.observe(TimerWakeMonitor::late_threshold_microseconds + 1);
    }
    REQUIRE(monitor.coarse());
    // Wake-ups between the thresholds neither confirm nor clear the mode.
    monitor.observe(600);
    REQUIRE(monitor.coarse());
    for (unsigned index = 0;
        index + 1 < TimerWakeMonitor::punctual_streak_to_leave; ++index) {
        monitor.observe(0);
    }
    REQUIRE(monitor.coarse());
    monitor.observe(TimerWakeMonitor::punctual_threshold_microseconds);
    REQUIRE(!monitor.coarse());
    // One late wake-up resets the punctual streak but not the mode.
    monitor.observe(5'000);
    REQUIRE(!monitor.coarse());
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

TEST(tsbpd_clock_backwards_timestamp_does_not_shift_later_deliveries)
{
    // A single timestamp before the connection origin (a source time that
    // wrapped to a large 32-bit value) must not push the clock a full period
    // ahead and delay every later packet by ~71.6 minutes.
    TsbpdClock clock {1'000'000, PacketTimestamp {1'000'000}, 120'000};
    REQUIRE_EQ(clock.delivery_time(PacketTimestamp {1'100'000}), 1'220'000U);

    // Source time 500 ms before the origin wraps to a near-maximum uint32.
    const std::uint32_t before_origin = static_cast<std::uint32_t>(-500'000);
    const std::uint64_t poisoned =
        clock.delivery_time(PacketTimestamp {before_origin});
    REQUIRE(poisoned <= 1'220'000U);

    // A later, normal timestamp is still scheduled around its true time.
    REQUIRE_EQ(clock.delivery_time(PacketTimestamp {1'200'000}), 1'320'000U);
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

TEST(control_timer_next_deadline_tracks_ack_lite_nak_and_keepalive)
{
    ControlTimerScheduler timer {0};
    REQUIRE_EQ(timer.next_deadline(0), 1'000'000U);
    timer.on_data_received(100);
    REQUIRE_EQ(timer.next_deadline(100), 10'000U);
    REQUIRE_EQ(
        timer.poll(10'000).values[0], TimerActionKind::full_acknowledgement);
    REQUIRE_EQ(timer.next_deadline(10'000), 1'000'000U);
    for (unsigned index = 0; index < 64; ++index) {
        timer.on_data_received(11'000);
    }
    REQUIRE_EQ(timer.next_deadline(11'000), 11'000U);
    REQUIRE_EQ(
        timer.poll(11'000).values[0], TimerActionKind::lite_acknowledgement);
    REQUIRE_EQ(timer.next_deadline(11'000), 20'000U);
    (void)timer.poll(20'000);
    timer.set_loss_state(true, 30'000, 100'000, true);
    REQUIRE_EQ(timer.next_deadline(30'000), 130'000U);
    REQUIRE_EQ(
        timer.poll(130'000).values[0], TimerActionKind::periodic_loss_report);
    REQUIRE_EQ(timer.next_deadline(130'000), 230'000U);
    timer.set_loss_state(false, 131'000, 20'000);
    timer.on_packet_sent(131'000);
    REQUIRE_EQ(timer.next_deadline(131'000), 1'131'000U);
    timer.on_receive_buffer_released(132'000);
    REQUIRE(timer.next_deadline(132'000) <= 132'000U);
}

TEST(control_timer_shorter_rtt_advances_pending_nak_without_postponing_it)
{
    ControlTimerScheduler timer {0};
    timer.set_loss_state(true, 200, 150'000, true);
    REQUIRE_EQ(timer.next_deadline(200), 150'200U);
    timer.set_loss_state(true, 11'000, 60'000, true);
    REQUIRE_EQ(timer.next_deadline(11'000), 71'000U);
    timer.set_loss_state(true, 20'000, 60'000, true);
    REQUIRE_EQ(timer.next_deadline(20'000), 71'000U);
    timer.set_loss_state(true, 25'000, 100'000, true);
    timer.set_loss_state(true, 30'000, 60'000, true);
    REQUIRE_EQ(timer.next_deadline(30'000), 71'000U);
    REQUIRE_EQ(timer.poll(70'999).size, 0U);
    const auto due = timer.poll(71'000);
    REQUIRE_EQ(due.size, 1U);
    REQUIRE_EQ(due.values[0], TimerActionKind::periodic_loss_report);
}

TEST(tsbpd_control_projection_cannot_advance_data_epoch)
{
    TsbpdClock clock {1'000'000, PacketTimestamp {0}, 100'000};
    for (const auto stamp : {0x7fffffffU, 0xfffffffeU}) {
        (void)clock.control_delivery_time(PacketTimestamp {stamp});
        (void)clock.observe_arrival(
            PacketTimestamp {stamp}, 6'000'000, 100'000, false);
    }
    REQUIRE_EQ(clock.delivery_time(PacketTimestamp {5'000'000}), 6'100'000U);
}

TEST(tsbpd_keepalive_uses_local_epoch_after_long_idle_and_multiple_wraps)
{
    for (const std::uint64_t elapsed :
        {2'200'000'000ULL, 4'300'000'000ULL, 9'000'000'000ULL}) {
        TsbpdClock clock {1'000'000, PacketTimestamp {0}, 100'000};
        // A forged control timestamp during the idle interval cannot choose
        // the epoch. Genuine DATA at the local elapsed time still unwraps.
        (void)clock.observe_arrival(
            PacketTimestamp {0x7fffffffU}, 1'000'000 + elapsed, 100'000, false);
        REQUIRE_EQ(clock.delivery_time(
                       PacketTimestamp {static_cast<std::uint32_t>(elapsed)}),
            1'100'000 + elapsed);
    }
}
