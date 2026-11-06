// SPDX-License-Identifier: MIT
#include "test.hpp"
#include "compat/hybrid_detector.hpp"
#include <algorithm>
using namespace robotweax::srt;
namespace hd = robotweax::srt::compat::hybrid;
namespace {
hd::Context context(std::uint64_t origin = 1000)
{
    hd::Context c;
    c.dispatch = {.group_id = group_handle_mask | 1U,
        .multicast_group = {.address = {239, 255, 1, 1}, .port = 6000},
        .initial_sequence = 42,
        .time_to_live = 1};
    c.source = {.kind = MulticastSourceKind::announcement,
        .group_id = c.dispatch.group_id,
        .endpoint = {.address = {192, 0, 2, 1}, .port = 9000}};
    c.offer.supported = hd::policy_query_features;
    c.selection = c.offer;
    c.selection.kind = hd::CapabilityKind::selection;
    c.selection.stream = 71;
    c.selection.member = 97;
    c.selection.initial_position = origin;
    REQUIRE(hd::valid(c));
    return c;
}
hd::DetectionConfiguration configuration()
{
    return {.report_age_us = 1000,
        .maximum_transit_us = 10,
        .stall_us = 50,
        .emission_lifetime_us = 300,
        .recovery_us = 40,
        .recovery_gap_us = 30,
        .hold_down_us = 80,
        .sample_gap_us = 20,
        .recovery_advances = 3};
}
struct Fixture {
    hd::Context c = context();
    hd::PathDetector detector {configuration(), c, 7};
    hd::PathDetector::Input input {.now_us = 100,
        .generation = 7,
        .source =
            hd::ReportObservation {
                .message = {.kind = hd::Kind::stream_progress,
                    .stream = 71,
                    .member = 97,
                    .serial = 1,
                    .next_position = 1000,
                    .next_sequence = 42,
                    .retained_floor = 1000,
                    .multicast_next = 1000,
                    .produced_next = 5000,
                    .activity = hd::Activity::producing,
                    .interval_us = 10},
                .first_received_us = 100,
                .fresh = true},
        .data = hd::ReceiveDataProgress {.origin = 1000,
            .window_floor = 1000,
            .all = {.next = 1000},
            .unicast = {.next = 1000},
            .multicast = {.next = 1000}},
        .policy = {.confirmed = true, .expires_at_us = 10000}};
    void mode(hd::Mode mode, std::uint64_t revision = 1)
    {
        input.policy.mode = mode;
        input.policy.revision = revision;
        input.source->message.mode = mode;
        input.source->message.revision = revision;
    }
    void report(std::uint64_t mc, std::uint64_t total = UINT64_MAX)
    {
        if (total == UINT64_MAX)
            total = mc;
        auto& m = input.source->message;
        ++m.serial;
        m.multicast_next = 1000 + mc;
        m.next_position = 1000 + total;
        m.next_sequence =
            static_cast<std::uint32_t>((42 + total) & SequenceNumber::mask);
        input.source->first_received_us = input.now_us;
    }
    void receive(std::uint64_t mc, std::uint64_t total = UINT64_MAX)
    {
        if (total == UINT64_MAX)
            total = mc;
        auto& d = *input.data;
        if (1000 + mc > d.multicast.next)
            REQUIRE(d.multicast.note(999 + mc, input.now_us));
        if (1000 + total > d.all.next)
            REQUIRE(d.all.note(999 + total, input.now_us));
        if (total > mc && 1000 + total > d.unicast.next)
            REQUIRE(d.unicast.note(999 + total, input.now_us));
    }
    hd::PathDetector::Assessment observe()
    {
        detector.observe(input);
        return detector.snapshot(input.now_us);
    }
    hd::PathDetector::Assessment tick(std::uint64_t now)
    {
        input.now_us = now;
        return observe();
    }
    void until(std::uint64_t now)
    {
        while (input.now_us < now)
            tick(std::min(now, input.now_us + 10));
    }
    hd::PathDetector::Assessment advance(
        std::uint64_t now, std::uint64_t position, bool data = true)
    {
        until(now - 10);
        input.now_us = now;
        report(position);
        if (data)
            receive(position);
        return observe();
    }
};
}
TEST(
    hybrid_detector_configuration_has_no_implicit_thresholds_and_bounds_every_window)
{
    REQUIRE(!hd::valid(hd::DetectionConfiguration {}));
    REQUIRE(hd::valid(configuration()));
    for (unsigned field = 0; field < 7; ++field) {
        auto c = configuration();
        if (field == 0)
            c.report_age_us = 0;
        if (field == 1)
            c.maximum_transit_us = 51;
        if (field == 2)
            c.emission_lifetime_us = 80;
        if (field == 3)
            c.recovery_gap_us = 19;
        if (field == 4)
            c.recovery_advances = 1;
        if (field == 5)
            c.recovery_advances = 65;
        if (field == 6)
            c.hold_down_us = hd::maximum_age_us + 1;
        REQUIRE(!hd::valid(c));
    }
    Fixture f;
    hd::PathDetector invalid({}, f.c, 7);
    invalid.observe(f.input);
    REQUIRE_EQ(
        invalid.snapshot(100).reason, hd::PathDetector::Reason::configuration);
}
TEST(
    hybrid_detector_first_positive_report_and_fresh_unchanged_reports_do_not_prove_advancing_emission)
{
    Fixture f;
    f.report(10);
    REQUIRE_EQ(f.observe().state, hd::PathDetector::State::observing);
    for (unsigned n = 0; n < 20; ++n) {
        f.input.now_us += 10;
        f.report(10);
        auto a = f.observe();
        REQUIRE(!a.fallback_candidate);
        REQUIRE(!a.multicast_stalled);
    }
    REQUIRE_EQ(f.observe().state, hd::PathDetector::State::awaiting_emission);
}
TEST(
    hybrid_detector_multicast_blackout_requires_actual_emission_and_exclusive_stall_budget)
{
    Fixture f;
    f.observe();
    auto a = f.advance(110, 1, false);
    REQUIRE_EQ(a.state, hd::PathDetector::State::multicast_suspect);
    f.until(160);
    a = f.observe();
    REQUIRE(!a.fallback_candidate);
    REQUIRE_EQ(a.next_wakeup_us, 170U);
    a = f.tick(170);
    REQUIRE(a.multicast_stalled);
    REQUIRE(a.all_data_stalled);
    REQUIRE(a.fallback_candidate);
    REQUIRE(!a.return_candidate);
    REQUIRE_EQ(a.state, hd::PathDetector::State::all_data_stalled);
}
TEST(
    hybrid_detector_unicast_progress_distinguishes_multicast_stall_from_aggregate_data_stall)
{
    Fixture f;
    f.observe();
    f.input.now_us = 110;
    f.report(1);
    f.receive(0, 1);
    f.observe();
    f.until(170);
    auto a = f.observe();
    REQUIRE(a.multicast_stalled);
    REQUIRE(!a.all_data_stalled);
    REQUIRE_EQ(a.state, hd::PathDetector::State::multicast_stalled);
    Fixture uc;
    uc.mode(hd::Mode::unicast);
    uc.observe();
    uc.input.now_us = 110;
    uc.report(0, 1);
    uc.observe();
    uc.until(170);
    a = uc.observe();
    REQUIRE(a.all_data_stalled);
    REQUIRE(!a.multicast_stalled);
    REQUIRE(!a.fallback_candidate);
    REQUIRE(!a.return_candidate);
}
TEST(
    hybrid_detector_absent_stale_or_unconfirmed_control_is_unknown_and_rearms_from_a_new_baseline)
{
    for (unsigned loss = 0; loss < 3; ++loss) {
        Fixture f;
        f.observe();
        f.advance(110, 1, false);
        f.until(170);
        REQUIRE(f.observe().fallback_candidate);
        auto source = f.input.source;
        if (loss == 0)
            f.input.source.reset();
        if (loss == 1)
            f.input.source->fresh = false;
        if (loss == 2)
            f.input.policy.confirmed = false;
        auto a = f.tick(180);
        REQUIRE_EQ(a.state, hd::PathDetector::State::unknown);
        REQUIRE(!a.fallback_candidate);
        f.input.source = source;
        f.input.policy.confirmed = true;
        a = f.tick(190);
        REQUIRE_EQ(a.state, hd::PathDetector::State::observing);
        f.until(260);
        REQUIRE(!f.observe().fallback_candidate);
    }
}
TEST(
    hybrid_detector_source_startup_pause_end_and_failure_are_distinct_and_suppress_candidates)
{
    for (auto activity : {hd::Activity::startup, hd::Activity::paused,
             hd::Activity::ended, hd::Activity::source_failed}) {
        Fixture f;
        f.observe();
        f.advance(110, 1, false);
        f.until(170);
        REQUIRE(f.observe().fallback_candidate);
        ++f.input.source->message.serial;
        f.input.source->message.activity = activity;
        f.input.now_us = 180;
        const auto a = f.observe();
        REQUIRE(!a.multicast_stalled);
        REQUIRE(!a.all_data_stalled);
        REQUIRE(!a.fallback_candidate);
        REQUIRE_EQ(a.state,
            activity == hd::Activity::startup ? hd::PathDetector::State::startup
                : activity == hd::Activity::paused
                ? hd::PathDetector::State::paused
                : activity == hd::Activity::ended
                ? hd::PathDetector::State::ended
                : hd::PathDetector::State::source_failed);
    }
}
TEST(
    hybrid_detector_admitted_production_and_receive_window_skips_are_not_emission_or_data)
{
    Fixture f;
    f.observe();
    for (unsigned i = 0; i < 20; ++i) {
        f.input.now_us += 10;
        f.report(0);
        f.input.source->message.produced_next += 10;
        f.input.data->window_floor += 10;
        auto a = f.observe();
        REQUIRE(!a.multicast_stalled);
        REQUIRE(!a.fallback_candidate);
        REQUIRE_EQ(a.received_next, 1000U);
    }
}
TEST(
    hybrid_detector_old_emission_cannot_be_renewed_by_fresh_stationary_metadata)
{
    Fixture f;
    f.observe();
    f.advance(110, 1, false);
    f.until(170);
    REQUIRE(f.observe().fallback_candidate);
    while (f.input.now_us < 420) {
        f.input.now_us += 10;
        f.report(1);
        f.observe();
    }
    auto a = f.observe();
    REQUIRE(!a.multicast_stalled);
    REQUIRE(!a.fallback_candidate);
    REQUIRE_EQ(a.state, hd::PathDetector::State::awaiting_emission);
}
TEST(hybrid_detector_unmeasured_transit_allowance_counts_against_report_age)
{
    Fixture f;
    f.input.source->message.age_us = 990;
    auto a = f.observe();
    REQUIRE_EQ(a.reason, hd::PathDetector::Reason::stale_report);
    Fixture future;
    future.input.source->first_received_us = 101;
    REQUIRE_EQ(future.observe().reason, hd::PathDetector::Reason::stale_report);
}
TEST(
    hybrid_detector_sampling_gap_clock_regression_and_root_replacement_cannot_preserve_decisions)
{
    Fixture f;
    f.observe();
    f.advance(110, 1, false);
    auto a = f.tick(200);
    REQUIRE_EQ(a.reason, hd::PathDetector::Reason::sampling_gap);
    REQUIRE(!a.fallback_candidate);
    a = f.tick(199);
    REQUIRE_EQ(a.reason, hd::PathDetector::Reason::clock);
    a = f.tick(210);
    REQUIRE_EQ(a.reason, hd::PathDetector::Reason::clock);
    Fixture replacement;
    replacement.observe();
    replacement.input.generation = 8;
    REQUIRE_EQ(
        replacement.tick(110).reason, hd::PathDetector::Reason::lifetime);
    replacement.input.generation = 7;
    REQUIRE_EQ(
        replacement.tick(120).reason, hd::PathDetector::Reason::lifetime);
}
TEST(
    hybrid_detector_conflicting_or_backwards_evidence_never_replaces_the_last_good_horizon)
{
    for (unsigned field = 0; field < 6; ++field) {
        Fixture f;
        f.observe();
        f.advance(110, 1);
        f.input.now_us = 120;
        if (field == 0)
            f.input.data->multicast.next = 1000;
        if (field == 1)
            f.input.data->multicast.last_advance_us = 120;
        if (field == 2)
            f.input.source->message.multicast_next = 1000;
        if (field == 3)
            f.input.source->message.serial = 1;
        if (field == 4)
            f.input.source->message.member = 98;
        if (field == 5)
            f.input.source->first_received_us = 120;
        auto a = f.observe();
        REQUIRE_EQ(a.reason, hd::PathDetector::Reason::invalid_evidence);
        REQUIRE(!a.fallback_candidate);
        REQUIRE(!a.return_candidate);
    }
}
TEST(
    hybrid_detector_recovery_requires_spaced_data_and_source_advances_and_hold_down)
{
    Fixture f;
    f.mode(hd::Mode::unicast);
    f.observe();
    auto a = f.advance(110, 1);
    REQUIRE_EQ(a.state, hd::PathDetector::State::recovering);
    f.advance(130, 2);
    a = f.advance(150, 3);
    REQUIRE(!a.return_candidate);
    REQUIRE_EQ(a.recovery_data_advances, 3U);
    a = f.advance(170, 4);
    REQUIRE(!a.return_candidate);
    a = f.tick(180);
    REQUIRE(a.return_candidate);
    REQUIRE_EQ(a.state, hd::PathDetector::State::multicast_ready);
    auto stale = f.detector.snapshot(200);
    REQUIRE_EQ(stale.state, hd::PathDetector::State::unknown);
    REQUIRE(!stale.return_candidate);
}
TEST(
    hybrid_detector_duplicate_copies_metadata_and_recovery_gaps_cannot_satisfy_hysteresis)
{
    Fixture f;
    f.mode(hd::Mode::unicast);
    f.observe();
    f.advance(110, 1);
    while (f.input.now_us < 200) {
        f.input.now_us += 10;
        f.report(1);
        auto a = f.observe();
        REQUIRE(!a.return_candidate);
    }
    REQUIRE_EQ(f.observe().recovery_data_advances, 0U);
    f.advance(210, 2);
    f.advance(230, 3);
    auto a = f.advance(250, 4);
    REQUIRE_EQ(a.recovery_data_advances, 3U);
    REQUIRE(a.return_candidate);
}
TEST(hybrid_detector_policy_revision_change_discards_old_recovery_evidence)
{
    Fixture f;
    f.mode(hd::Mode::unicast);
    f.observe();
    f.advance(110, 1);
    f.advance(130, 2);
    f.advance(150, 3);
    f.input.now_us = 160;
    ++f.input.policy.revision;
    auto a = f.observe();
    REQUIRE_EQ(a.reason, hd::PathDetector::Reason::policy);
    f.input.source->message.revision = f.input.policy.revision;
    ++f.input.source->message.serial;
    a = f.tick(170);
    REQUIRE_EQ(a.state, hd::PathDetector::State::observing);
    REQUIRE_EQ(a.recovery_data_advances, 0U);
    REQUIRE(!a.return_candidate);
}
