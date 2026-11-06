// SPDX-License-Identifier: MIT
#include "test.hpp"
#include "compat/hybrid_reporting.hpp"
using namespace robotweax::srt;
namespace hr = robotweax::srt::compat::hybrid;
namespace {
hr::Context context()
{
    hr::Context c;
    c.dispatch = {.group_id = group_handle_mask | 1U,
        .multicast_group = {.address = {239, 255, 1, 1}, .port = 6000},
        .initial_sequence = 42,
        .time_to_live = 1};
    c.source = {.kind = MulticastSourceKind::announcement,
        .group_id = c.dispatch.group_id,
        .endpoint = {.address = {192, 0, 2, 1}, .port = 9000}};
    c.offer.supported = hr::policy_query_features;
    c.selection = c.offer;
    c.selection.kind = hr::CapabilityKind::selection;
    c.selection.stream = 71;
    c.selection.member = 97;
    c.selection.initial_position = 100;
    REQUIRE(hr::valid(c));
    return c;
}
hr::SourceDataProgress source()
{
    return {.stream = 71,
        .member = 97,
        .member_origin = 100,
        .admitted_next = 105,
        .history_floor = 100,
        .observed_at_us = 200};
}
hr::Message report(std::uint64_t serial = 1, std::uint64_t position = 105,
    std::uint64_t mc = 103)
{
    return {.kind = hr::Kind::stream_progress,
        .stream = 71,
        .member = 97,
        .serial = serial,
        .next_position = position,
        .next_sequence = 42,
        .retained_floor = 100,
        .multicast_next = mc,
        .produced_next = 110,
        .activity = hr::Activity::producing,
        .interval_us = 100,
        .age_us = 10};
}
}
TEST(
    hybrid_reporting_activity_is_explicit_monotonic_expiring_and_ended_is_terminal)
{
    hr::SourceActivity activity;
    REQUIRE(!activity.observe(100, 0));
    REQUIRE(activity.declare(hr::Activity::startup, 0, 100, 100, 0));
    REQUIRE(!activity.declare(hr::Activity::producing, 1, 100, 99, 0));
    REQUIRE(activity.declare(hr::Activity::producing, 5, 100, 110, 1));
    REQUIRE(!activity.declare(hr::Activity::startup, 0, 100, 111, 0));
    REQUIRE(!activity.declare(hr::Activity::paused, 4, 100, 111, 1));
    REQUIRE(activity.declare(hr::Activity::paused, 5, 100, 120, 2));
    REQUIRE(activity.observe(130, 5));
    REQUIRE(!activity.observe(130, 6));
    REQUIRE(!activity.observe(220, 5));
    REQUIRE(activity.declare(hr::Activity::ended, 5, 100, 230, 5));
    REQUIRE(!activity.declare(hr::Activity::producing, 6, 100, 240, 5));
    REQUIRE(!activity.declare(hr::Activity::ended, 6, 100, 240, 5));
    REQUIRE(activity.declare(hr::Activity::ended, 5, 100, 240, 5));
}
TEST(
    hybrid_reporting_activity_invalid_lifetimes_failure_assertions_and_overflow_are_refused)
{
    hr::SourceActivity activity;
    REQUIRE(!activity.declare(hr::Activity::producing, 0, 0, 100, 0));
    REQUIRE(!activity.declare(
        hr::Activity::producing, 0, hr::maximum_age_us + 1, 100, 0));
    REQUIRE(!activity.declare(hr::Activity::producing, 0, 1, UINT64_MAX, 0));
    REQUIRE(!activity.declare(hr::Activity::startup, 1, 100, 100, 0));
    REQUIRE(!activity.declare(hr::Activity::source_failed, 1, 100, 100, 0));
    REQUIRE(!activity.declare(hr::Activity::producing, 1, 100, 100, 2));
    REQUIRE(!activity.observe(100, 0));
}
TEST(
    hybrid_reporting_source_distinguishes_production_admission_and_actual_submissions)
{
    auto c = context();
    auto s = source();
    hr::ActivityObservation activity {hr::Activity::producing, 110, 150, 500};
    auto p = hr::stream_report(
        s, activity, c, hr::Mode::multicast, 1, {100, 300}, 200);
    REQUIRE(p);
    REQUIRE_EQ(p->message.produced_next, 110U);
    REQUIRE_EQ(p->message.next_position, 100U);
    REQUIRE_EQ(p->message.multicast_next, 100U);
    REQUIRE_EQ(p->observed_at_us, 150U);
    REQUIRE_EQ(p->not_after_us, 450U);
    REQUIRE(s.multicast.note(101, 190));
    REQUIRE(s.unicast.note(104, 195));
    p = hr::stream_report(
        s, activity, c, hr::Mode::unicast, 2, {100, 300}, 200);
    REQUIRE(p);
    REQUIRE_EQ(p->message.next_position, 105U);
    REQUIRE_EQ(p->message.multicast_next, 102U);
    REQUIRE_EQ(p->message.next_sequence, 47U);
    REQUIRE_EQ(p->message.mode, hr::Mode::unicast);
}
TEST(
    hybrid_reporting_source_prejoin_marks_expired_assertions_and_inconsistent_inputs)
{
    auto c = context();
    auto s = source();
    hr::ActivityObservation a {hr::Activity::paused, 105, 150, 500};
    REQUIRE(s.multicast.note(50, 190));
    auto p =
        hr::stream_report(s, a, c, hr::Mode::multicast, 1, {100, 300}, 200);
    REQUIRE(p);
    REQUIRE_EQ(p->message.multicast_next, 100U);
    REQUIRE(!hr::stream_report(s, a, c, hr::Mode::multicast, 1, {}, 200));
    REQUIRE(
        !hr::stream_report(s, a, c, hr::Mode::multicast, 1, {100, 300}, 450));
    a.produced_next = 104;
    REQUIRE(
        !hr::stream_report(s, a, c, hr::Mode::multicast, 1, {100, 300}, 200));
    a.produced_next = 105;
    s.source_failed = true;
    REQUIRE(
        !hr::stream_report(s, a, c, hr::Mode::multicast, 1, {100, 300}, 200));
    s.source_failed = false;
    s.member_origin = 99;
    REQUIRE(
        !hr::stream_report(s, a, c, hr::Mode::multicast, 1, {100, 300}, 200));
    s.member_origin = 100;
    REQUIRE(s.multicast.note(106, 190));
    REQUIRE(
        !hr::stream_report(s, a, c, hr::Mode::multicast, 1, {100, 300}, 200));
}
TEST(
    hybrid_reporting_path_counts_data_high_waters_without_promoting_the_window_floor)
{
    auto c = context();
    hr::ReceiveDataProgress s {.origin = 100, .window_floor = 120};
    auto p = hr::path_report(s, c, hr::Mode::multicast, 0, 1, {100, 300}, 200);
    REQUIRE(p);
    REQUIRE_EQ(p->message.next_position, 100U);
    REQUIRE(s.all.note(108, 190));
    REQUIRE(s.multicast.note(102, 180));
    p = hr::path_report(s, c, hr::Mode::unicast, 1, 2, {100, 300}, 200);
    REQUIRE(p);
    REQUIRE_EQ(p->message.next_position, 109U);
    REQUIRE_EQ(p->message.multicast_next, 103U);
    REQUIRE_EQ(p->observed_at_us, 200U);
    REQUIRE_EQ(p->message.retained_floor, 0U);
    REQUIRE(s.multicast.note(110, 190));
    REQUIRE(!hr::path_report(s, c, hr::Mode::unicast, 1, 3, {100, 300}, 200));
    s.valid = false;
    REQUIRE(!hr::path_report(s, c, hr::Mode::unicast, 1, 3, {100, 300}, 200));
}
TEST(hybrid_reporting_tracker_fresh_metadata_does_not_renew_data_advancement)
{
    hr::ReportTracker t(hr::Kind::stream_progress, 100);
    auto m = report();
    REQUIRE(t.accept(m, false, 200, 210));
    auto s = t.snapshot(220, 100);
    REQUIRE(s);
    REQUIRE(s->fresh);
    REQUIRE_EQ(s->effective_age_us, 30U);
    REQUIRE_EQ(s->multicast_advanced_at_us, 200U);
    m.serial = 2;
    m.activity = hr::Activity::paused;
    REQUIRE(t.accept(m, false, 250, 260));
    s = t.snapshot(270, 100);
    REQUIRE(s->fresh);
    REQUIRE_EQ(s->multicast_advanced_at_us, 200U);
    REQUIRE_EQ(s->next_advanced_at_us, 200U);
    m.serial = 3;
    m.next_position = 106;
    REQUIRE(t.accept(m, false, 280, 280));
    s = t.snapshot(290, 100);
    REQUIRE_EQ(s->next_advanced_at_us, 280U);
    REQUIRE_EQ(s->multicast_advanced_at_us, 200U);
    REQUIRE(!t.snapshot(370, 100)->fresh);
}
TEST(
    hybrid_reporting_tracker_duplicate_delayed_old_or_conflicting_reports_cannot_renew)
{
    hr::ReportTracker t(hr::Kind::stream_progress, 100);
    auto m = report();
    REQUIRE(t.accept(m, false, 100, 100));
    REQUIRE(!t.accept(m, true, 100, 150));
    REQUIRE(!t.accept(m, false, 160, 160));
    for (unsigned field = 0; field < 6; ++field) {
        auto changed = m;
        changed.serial = 2;
        if (field == 0)
            changed.next_position = 104;
        if (field == 1)
            changed.multicast_next = 102;
        if (field == 2)
            changed.produced_next = 109;
        if (field == 3)
            changed.retained_floor = 99;
        if (field == 4)
            changed.mode = hr::Mode::unicast;
        if (field == 5)
            changed.member = 98;
        REQUIRE(!t.accept(changed, false, 170, 170));
    }
    REQUIRE_EQ(t.snapshot(180, 100)->first_received_us, 100U);
    REQUIRE(!t.snapshot(190, 100)->fresh);
    m.serial = 3;
    m.age_us = 100;
    REQUIRE(t.accept(m, false, 195, 300));
    REQUIRE(!t.snapshot(300, 100)->fresh);
}
TEST(hybrid_reporting_tracker_clock_regression_and_ended_rollback_fail_closed)
{
    hr::ReportTracker t(hr::Kind::stream_progress, 100);
    auto m = report();
    m.activity = hr::Activity::ended;
    REQUIRE(t.accept(m, false, 200, 200));
    m.serial = 2;
    m.activity = hr::Activity::producing;
    REQUIRE(!t.accept(m, false, 210, 210));
    REQUIRE(!t.snapshot(199, 100)->fresh);
    REQUIRE(!t.snapshot(220, 100)->fresh);
    m.activity = hr::Activity::ended;
    REQUIRE(!t.accept(m, false, 230, 230));
    hr::ReportTracker path(hr::Kind::path_report, 100);
    m = report();
    m.kind = hr::Kind::path_report;
    m.produced_next = 0;
    m.retained_floor = 0;
    m.activity = hr::Activity::startup;
    m.next_position = m.multicast_next = 100;
    REQUIRE(path.accept(m, false, 100, 100));
    REQUIRE_EQ(path.snapshot(110, 100)->next_advanced_at_us, 0U);
}
TEST(hybrid_reporting_configuration_and_age_bounds_are_explicit)
{
    REQUIRE(hr::valid(hr::ReportingConfiguration {}));
    REQUIRE(hr::valid(hr::ReportingConfiguration {1, 1}));
    REQUIRE(!hr::valid(hr::ReportingConfiguration {101, 100}));
    REQUIRE(!hr::valid(hr::ReportingConfiguration {1, 0}));
    REQUIRE(!hr::valid(hr::ReportingConfiguration {
        hr::maximum_interval_us + 1, hr::maximum_age_us}));
    hr::ReportTracker t;
    auto m = report();
    REQUIRE(t.accept(m, false, 1, 1));
    REQUIRE(!t.snapshot(2, 0)->fresh);
    REQUIRE(!t.snapshot(2, hr::maximum_age_us + 1)->fresh);
    REQUIRE_EQ(t.snapshot(UINT64_MAX, 100)->effective_age_us, UINT64_MAX);
}
