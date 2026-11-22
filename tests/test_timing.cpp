#include "test.hpp"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

#include "robotweax/srt/timing.hpp"
#include "compat/platform_timer.hpp"
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

TEST(packet_pacer_carries_fractions_of_a_microsecond)
{
    // 1,250 bytes at 500 MB/s: one slot every 2.5 us. Whole-microsecond
    // slots must alternate so that 1,000 packets take 2,500 us, not the
    // 3,000 us of an interval rounded up for every packet.
    PacketPacer pacer {500'000'000, 64};
    std::uint64_t now = 10'000;
    pacer.on_packet_sent(1'250, now);
    for (int index = 1; index < 1'000; ++index) {
        const auto next = pacer.query(now, 0).next_ready_microseconds;
        REQUIRE(next - now == 2U || next - now == 3U);
        now = next;
        pacer.on_packet_sent(1'250, now);
    }
    REQUIRE_EQ(pacer.query(now, 0).next_ready_microseconds, 12'500U);
}

TEST(packet_pacer_follows_a_rate_change_without_losing_the_schedule)
{
    PacketPacer pacer {500'000'000, 64};
    std::uint64_t now = 0;
    for (int index = 0; index < 100; ++index) {
        pacer.on_packet_sent(1'250, now);
        now = pacer.query(now, 0).next_ready_microseconds;
    }
    pacer.set_rate(300'000'000); // 4.1666 us per packet.
    const std::uint64_t changed = now;
    for (int index = 0; index < 600; ++index) {
        pacer.on_packet_sent(1'250, now);
        now = pacer.query(now, 0).next_ready_microseconds;
    }
    REQUIRE(now - changed >= 2'500U && now - changed <= 2'501U);
    // Faster than one packet per microsecond is still capped there.
    pacer.set_rate(5'000'000'000ULL);
    pacer.on_packet_sent(1'250, now);
    REQUIRE_EQ(pacer.query(now, 0).next_ready_microseconds, now + 1U);
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

TEST(packet_pacer_coalescing_batches_sends_and_keeps_the_rate)
{
    // 204 bytes (a 188-byte message with its header) at 1 MB/s: one slot
    // every 204 us, about 4,900 packets per second. A 2 ms quantum lets one
    // wake-up serve the slot that is due and the nine that follow it.
    PacketPacer pacer {1'000'000, 8'192};
    pacer.set_coalescing_quantum(2'000);
    REQUIRE_EQ(pacer.coalescing_quantum_microseconds(), 2'000U);
    pacer.on_packet_sent(204, 0);
    REQUIRE_EQ(pacer.query(0, 0).next_ready_microseconds, 204U);
    REQUIRE_EQ(pacer.coalesced_ready_microseconds(1'000), 204U + 9U * 204U);

    constexpr std::uint64_t duration = 1'000'000;
    constexpr std::uint64_t lateness = 300; // every timer wake-up is late
    std::size_t packets = 1;
    std::size_t wakes = 0;
    std::size_t largest_batch = 0;
    for (;;) {
        const std::uint64_t now =
            pacer.coalesced_ready_microseconds(1'000) + lateness;
        if (now > duration) {
            break;
        }
        ++wakes;
        std::size_t batch = 0;
        while (pacer.query(now, 0).ready) {
            pacer.on_packet_sent(204, now);
            ++batch;
        }
        REQUIRE(batch >= 10U);
        largest_batch = std::max(largest_batch, batch);
        packets += batch;
    }
    // The schedule is kept across every late, batched wake-up.
    REQUIRE(packets >= duration / 204U - 12U);
    REQUIRE(packets <= duration / 204U + 1U);
    // One wake-up per batch instead of one per packet.
    REQUIRE(wakes <= 500U);
    REQUIRE(wakes >= 400U);
    // A batch never exceeds the quantum plus the lateness, in slots.
    REQUIRE(largest_batch <= (2'000U + lateness) / 204U + 1U);
}

TEST(packet_pacer_coalescing_waits_only_for_packets_it_can_add)
{
    PacketPacer pacer {1'000'000, 8'192};
    pacer.on_packet_sent(204, 0);
    // No quantum: the next slot, whatever waits.
    REQUIRE_EQ(pacer.coalesced_ready_microseconds(1'000), 204U);
    pacer.set_coalescing_quantum(2'000);
    // Nothing or a single packet waiting: no reason to be late.
    REQUIRE_EQ(pacer.coalesced_ready_microseconds(0), 204U);
    REQUIRE_EQ(pacer.coalesced_ready_microseconds(1), 204U);
    // Three packets waiting: the slot of the third, not the whole quantum.
    REQUIRE_EQ(pacer.coalesced_ready_microseconds(3), 204U + 2U * 204U);
    // The quantum never moves the slot itself.
    REQUIRE_EQ(pacer.query(0, 0).next_ready_microseconds, 204U);
    REQUIRE(pacer.query(204, 0).ready);

    // An interval longer than the quantum is not delayed at all: 1,332 bytes
    // at 250 kB/s is one slot every 5,328 us.
    PacketPacer slow {250'000, 8'192};
    slow.set_coalescing_quantum(2'000);
    slow.on_packet_sent(1'332, 0);
    REQUIRE_EQ(slow.query(0, 0).next_ready_microseconds, 5'328U);
    REQUIRE_EQ(slow.coalesced_ready_microseconds(1'000), 5'328U);

    // Before the first send there is no schedule to be late against.
    PacketPacer fresh {1'000'000, 8'192};
    fresh.set_coalescing_quantum(2'000);
    REQUIRE_EQ(fresh.coalesced_ready_microseconds(1'000),
        fresh.query(0, 0).next_ready_microseconds);

    // The quantum is bounded.
    fresh.set_coalescing_quantum(1'000'000);
    REQUIRE_EQ(fresh.coalescing_quantum_microseconds(),
        PacketPacer::maximum_coalescing_quantum_microseconds);
}

TEST(packet_pacer_coalescing_bounds_the_burst_after_a_pause)
{
    // 100 us slots, the default 1 ms credit and a 2 ms quantum.
    PacketPacer pacer {2'000'000, 8'192};
    pacer.set_coalescing_quantum(2'000);
    pacer.on_packet_sent(200, 10'000);
    // Late by exactly quantum plus credit: the schedule is kept and the
    // missed slots are sent, no more than that many.
    const std::uint64_t late = 10'100 + 2'000 + 1'000;
    std::size_t sent = 0;
    while (pacer.query(late, 0).ready && sent < 100U) {
        pacer.on_packet_sent(200, late);
        ++sent;
    }
    REQUIRE_EQ(sent, 31U);
    // One microsecond later than quantum plus credit: a fresh schedule, so a
    // pause is not followed by a burst.
    const std::uint64_t next = pacer.query(late, 0).next_ready_microseconds;
    const std::uint64_t resumed = next + 2'000 + 1'000 + 1;
    pacer.on_packet_sent(200, resumed);
    REQUIRE(!pacer.query(resumed + 99, 0).ready);
    REQUIRE(pacer.query(resumed + 100, 0).ready);

    // Without a quantum the credit alone applies, as before.
    PacketPacer plain {2'000'000, 8'192};
    plain.on_packet_sent(200, 10'000);
    plain.on_packet_sent(200, 10'100 + 1'001);
    REQUIRE(!plain.query(10'100 + 1'001 + 99, 0).ready);
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

TEST(coarse_timer_probe_spacing_is_short_for_a_stall_and_backs_off)
{
    using Spacing = CoarseTimerProbeSpacing;
    Spacing spacing;
    // A first episode probes closely, and punctual probes keep it close, so
    // a short stall is left after a few milliseconds of resubmission.
    spacing.on_enter(10'000'000);
    REQUIRE_EQ(spacing.interval_microseconds(), Spacing::initial_microseconds);
    spacing.on_wake(TimerWakeMonitor::punctual_threshold_microseconds);
    REQUIRE_EQ(spacing.interval_microseconds(), Spacing::initial_microseconds);
    // Probes that are still not punctual double the spacing up to the
    // maximum, where a host with coarse timers throughout settles.
    spacing.on_wake(TimerWakeMonitor::punctual_threshold_microseconds + 1);
    REQUIRE_EQ(
        spacing.interval_microseconds(), Spacing::initial_microseconds * 2U);
    for (int probe = 0; probe < 16; ++probe) {
        spacing.on_wake(5'000);
    }
    REQUIRE_EQ(spacing.interval_microseconds(), Spacing::maximum_microseconds);
}

TEST(coarse_timer_probe_spacing_widens_for_episodes_in_quick_succession)
{
    using Spacing = CoarseTimerProbeSpacing;
    Spacing spacing;
    std::uint64_t now = 10'000'000;
    spacing.on_enter(now);
    spacing.on_leave(now += 20'000);
    // Re-entering within the window starts each episode twice as wide.
    spacing.on_enter(now += Spacing::reentry_window_microseconds - 1);
    REQUIRE_EQ(
        spacing.interval_microseconds(), Spacing::initial_microseconds * 2U);
    spacing.on_leave(now += 40'000);
    spacing.on_enter(now += 1'000);
    REQUIRE_EQ(
        spacing.interval_microseconds(), Spacing::initial_microseconds * 4U);
    for (int episode = 0; episode < 16; ++episode) {
        spacing.on_leave(now += 1'000);
        spacing.on_enter(now += 1'000);
    }
    REQUIRE_EQ(spacing.interval_microseconds(), Spacing::maximum_microseconds);
    // A calm period at least as long as the window starts over.
    spacing.on_leave(now += 1'000);
    spacing.on_enter(now += Spacing::reentry_window_microseconds);
    REQUIRE_EQ(spacing.interval_microseconds(), Spacing::initial_microseconds);
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

TEST(control_timer_cadence_change_retains_data_send_anchor_and_clamps_zero)
{
    ControlTimerScheduler timer {0};
    timer.on_packet_sent(100'000);
    timer.set_keepalive_interval(500'000);
    REQUIRE_EQ(timer.next_deadline(100'000), 600'000U);
    REQUIRE_EQ(timer.poll(599'999).size, 0U);
    REQUIRE_EQ(timer.poll(600'000).size, 1U);
    timer.set_keepalive_interval(0);
    REQUIRE_EQ(timer.poll(600'000).size, 0U);
    REQUIRE_EQ(timer.poll(600'001).size, 1U);
}

// Not a pass/fail timing check: it reports what a short sleep and a short
// timed wait really take on this host, first as the process finds it and then
// while a PlatformTimerResolution is held, as the runtime scheduler holds one.
// Hosted Windows runners took about 14 ms for a 100 us or 1 ms wait at the
// default timer period. The only requirement is that a sleep or timed-out
// wait never returns early.
TEST(platform_timing_report_measures_short_sleeps_and_timed_waits)
{
    using Clock = std::chrono::steady_clock;
    const auto report = [](const char* phase, const char* kind,
                            std::chrono::microseconds asked,
                            std::vector<std::int64_t> taken) {
        std::sort(taken.begin(), taken.end());
        std::int64_t sum = 0;
        for (const auto value : taken)
            sum += value;
        REQUIRE(!taken.empty());
        REQUIRE(taken.front() >= asked.count());
        std::fprintf(stderr,
            "[PLATFORM_TIMING] phase=%s kind=%s asked_us=%lld samples=%zu "
            "min_us=%lld median_us=%lld mean_us=%lld max_us=%lld\n",
            phase, kind, static_cast<long long>(asked.count()), taken.size(),
            static_cast<long long>(taken.front()),
            static_cast<long long>(taken[taken.size() / 2]),
            static_cast<long long>(
                sum / static_cast<std::int64_t>(taken.size())),
            static_cast<long long>(taken.back()));
    };
    const auto elapsed = [](Clock::time_point start) {
        return std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now() - start)
            .count();
    };
    std::mutex mutex;
    std::condition_variable wake;
    const auto measure = [&](const char* phase) {
        for (const auto asked :
            {std::chrono::microseconds {100}, std::chrono::microseconds {1000},
                std::chrono::microseconds {2000},
                std::chrono::microseconds {10000}}) {
            const std::size_t samples = asked.count() >= 10000 ? 30 : 100;
            std::vector<std::int64_t> slept, waited;
            for (std::size_t i = 0; i < samples; ++i) {
                auto start = Clock::now();
                std::this_thread::sleep_for(asked);
                slept.push_back(elapsed(start));
                std::unique_lock lock(mutex);
                start = Clock::now();
                // Nothing notifies: the predicate keeps the wait going through a
                // spurious wake until the timeout.
                (void)wake.wait_for(lock, asked, [] {
                    return false;
                });
                waited.push_back(elapsed(start));
            }
            report(phase, "sleep_for", asked, std::move(slept));
            report(phase, "wait_for", asked, std::move(waited));
        }
    };
    measure("default");
    {
        const compat::PlatformTimerResolution resolution;
        std::fprintf(stderr, "[PLATFORM_TIMING] phase=held finer_period=%d\n",
            resolution.held() ? 1 : 0);
        measure("held");
    }
    std::fflush(stderr);
}
