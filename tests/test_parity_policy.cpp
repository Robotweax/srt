#include "test.hpp"

#include "robotweax/srt/parity_policy.hpp"

#include <cstdint>

using namespace robotweax::srt;

// Reference table computed from the sealed reader's Python functions
// (scripts/tail_probe_budget_comparison.py of the adaptive recovery study:
// arq_meets_deadline, decide, probe_interval, CONSTANTS) with
//   python3 -I parity_table.py tail_probe_budget_comparison.py
// on 3 November 2026. CONSTANTS = (120000, 2000, 20000, 5000, 100000,
// 1000000, 2); only latency_us (column 1) is varied, as the integration
// takes it from the negotiated peer receive delay. Level codes: 0 full,
// 1 row_only, 2 none; mode codes: 0 fixed, 2 deadline_only.
namespace {

struct DeadlineRow {
    std::uint64_t latency_us;
    bool rtt_known;
    std::uint32_t srtt_us;
    std::uint32_t rttvar_us;
    std::uint64_t bound_us;
    bool meets_deadline;
    int deadline_only_level;
    int fixed_level;
};

constexpr DeadlineRow deadline_rows[] = {
    {120000, false, 0, 0, 27000, false, 0, 0},
    {120000, false, 5000, 0, 37000, false, 0, 0},
    {120000, false, 20000, 0, 67000, false, 0, 0},
    {120000, false, 20000, 13250, 120000, false, 0, 0},
    {120000, false, 60000, 0, 147000, false, 0, 0},
    {120000, false, 80000, 0, 187000, false, 0, 0},
    {120000, false, 100000, 0, 227000, false, 0, 0},
    {120000, true, 0, 0, 27000, true, 2, 0},
    {120000, true, 0, 25000, 127000, false, 0, 0},
    {120000, true, 5000, 0, 37000, true, 2, 0},
    {120000, true, 5000, 25000, 137000, false, 0, 0},
    {120000, true, 20000, 0, 67000, true, 2, 0},
    {120000, true, 20000, 13249, 119996, true, 2, 0},
    {120000, true, 20000, 13250, 120000, false, 0, 0},
    {120000, true, 20000, 13251, 120004, false, 0, 0},
    {120000, true, 20000, 25000, 167000, false, 0, 0},
    {120000, true, 60000, 0, 147000, false, 0, 0},
    {120000, true, 60000, 25000, 247000, false, 0, 0},
    {120000, true, 80000, 0, 187000, false, 0, 0},
    {120000, true, 80000, 25000, 287000, false, 0, 0},
    {120000, true, 100000, 0, 227000, false, 0, 0},
    {120000, true, 100000, 25000, 327000, false, 0, 0},
    {60000, false, 0, 0, 27000, false, 0, 0},
    {60000, false, 5000, 0, 37000, false, 0, 0},
    {60000, false, 20000, 0, 67000, false, 0, 0},
    {60000, false, 60000, 0, 147000, false, 0, 0},
    {60000, false, 80000, 0, 187000, false, 0, 0},
    {60000, false, 100000, 0, 227000, false, 0, 0},
    {60000, true, 0, 0, 27000, true, 2, 0},
    {60000, true, 0, 25000, 127000, false, 0, 0},
    {60000, true, 5000, 0, 37000, true, 2, 0},
    {60000, true, 5000, 25000, 137000, false, 0, 0},
    {60000, true, 20000, 0, 67000, false, 0, 0},
    {60000, true, 20000, 25000, 167000, false, 0, 0},
    {60000, true, 60000, 0, 147000, false, 0, 0},
    {60000, true, 60000, 25000, 247000, false, 0, 0},
    {60000, true, 80000, 0, 187000, false, 0, 0},
    {60000, true, 80000, 25000, 287000, false, 0, 0},
    {60000, true, 100000, 0, 227000, false, 0, 0},
    {60000, true, 100000, 25000, 327000, false, 0, 0},
    {200000, false, 0, 0, 27000, false, 0, 0},
    {200000, false, 5000, 0, 37000, false, 0, 0},
    {200000, false, 20000, 0, 67000, false, 0, 0},
    {200000, false, 60000, 0, 147000, false, 0, 0},
    {200000, false, 60000, 13250, 200000, false, 0, 0},
    {200000, false, 80000, 0, 187000, false, 0, 0},
    {200000, false, 100000, 0, 227000, false, 0, 0},
    {200000, true, 0, 0, 27000, true, 2, 0},
    {200000, true, 0, 25000, 127000, true, 2, 0},
    {200000, true, 5000, 0, 37000, true, 2, 0},
    {200000, true, 5000, 25000, 137000, true, 2, 0},
    {200000, true, 20000, 0, 67000, true, 2, 0},
    {200000, true, 20000, 25000, 167000, true, 2, 0},
    {200000, true, 60000, 0, 147000, true, 2, 0},
    {200000, true, 60000, 13249, 199996, true, 2, 0},
    {200000, true, 60000, 13250, 200000, false, 0, 0},
    {200000, true, 60000, 13251, 200004, false, 0, 0},
    {200000, true, 60000, 25000, 247000, false, 0, 0},
    {200000, true, 80000, 0, 187000, true, 2, 0},
    {200000, true, 80000, 25000, 287000, false, 0, 0},
    {200000, true, 100000, 0, 227000, false, 0, 0},
    {200000, true, 100000, 25000, 327000, false, 0, 0},

};

struct ProbeIntervalRow {
    std::uint32_t srtt_us;
    std::uint64_t interval_us;
};

constexpr ProbeIntervalRow probe_interval_rows[] = {
    {0, 10000},
    {5000, 10000},
    {20000, 40000},
    {60000, 120000},
    {80000, 120000},
    {100000, 120000},
    {4999, 10000},
    {5001, 10002},
    {59999, 119998},
    {60001, 120000},

};

constexpr int level_code(parity::Level level) noexcept
{
    return static_cast<int>(level);
}

} // namespace

TEST(parity_policy_constants_match_the_sealed_reader)
{
    const parity::Constants c;
    REQUIRE_EQ(c.latency_us, 120'000U);
    REQUIRE_EQ(c.detect_us, 2'000U);
    REQUIRE_EQ(c.nak_interval_us, 20'000U);
    REQUIRE_EQ(c.guard_us, 5'000U);
    REQUIRE_EQ(c.clean_hold_us, 100'000U);
    REQUIRE_EQ(c.burst_memory_us, 1'000'000U);
    REQUIRE_EQ(c.burst_threshold, 2U);
    REQUIRE_EQ(static_cast<int>(parity::Mode::fixed), 0);
    REQUIRE_EQ(static_cast<int>(parity::Mode::deadline_only), 2);
    REQUIRE_EQ(level_code(parity::Level::full), 0);
    REQUIRE_EQ(level_code(parity::Level::row_only), 1);
    REQUIRE_EQ(level_code(parity::Level::none), 2);
    REQUIRE_EQ(parity::probe_limit, 3U);
}

TEST(parity_policy_deadline_test_matches_the_sealed_reader_table)
{
    std::size_t boundary_rows = 0;
    std::size_t unknown_rtt_rows = 0;
    for (const auto& row : deadline_rows) {
        parity::Constants c;
        c.latency_us = row.latency_us;
        const parity::Inputs in {
            .now_us = 0,
            .rtt_known = row.rtt_known,
            .srtt_us = row.srtt_us,
            .rttvar_us = row.rttvar_us,
        };
        const std::uint64_t bound = c.detect_us + 2ULL * row.srtt_us
            + 4ULL * row.rttvar_us + c.nak_interval_us + c.guard_us;
        REQUIRE_EQ(bound, row.bound_us);
        REQUIRE_EQ(parity::arq_meets_deadline(in, c), row.meets_deadline);
        REQUIRE_EQ(level_code(parity::decide(parity::Mode::deadline_only, in, c)),
            row.deadline_only_level);
        REQUIRE_EQ(level_code(parity::decide(parity::Mode::fixed, in, c)),
            row.fixed_level);
        // deadline_only never thins protection below full parity.
        REQUIRE(parity::decide(parity::Mode::deadline_only, in, c)
            != parity::Level::row_only);
        if (row.bound_us == row.latency_us) {
            // The strict inequality: an exact bound does not meet the deadline.
            REQUIRE(!row.meets_deadline);
            ++boundary_rows;
        }
        if (!row.rtt_known) {
            REQUIRE(!row.meets_deadline);
            ++unknown_rtt_rows;
        }
    }
    REQUIRE(boundary_rows >= 2U);
    REQUIRE(unknown_rtt_rows >= 6U);
}

TEST(parity_policy_emit_and_probe_interval_match_the_sealed_reader)
{
    REQUIRE(parity::emit(parity::Level::full, parity::Kind::row));
    REQUIRE(parity::emit(parity::Level::full, parity::Kind::column));
    REQUIRE(parity::emit(parity::Level::row_only, parity::Kind::row));
    REQUIRE(!parity::emit(parity::Level::row_only, parity::Kind::column));
    REQUIRE(!parity::emit(parity::Level::none, parity::Kind::row));
    REQUIRE(!parity::emit(parity::Level::none, parity::Kind::column));
    for (const auto& row : probe_interval_rows) {
        REQUIRE_EQ(parity::probe_interval(row.srtt_us), row.interval_us);
    }
    // The 10 ms floor and the 120 ms cap as named by the probe rules.
    REQUIRE_EQ(parity::probe_interval(0), parity::probe_interval_floor_us);
    REQUIRE_EQ(parity::probe_interval(5'000), parity::probe_interval_floor_us);
    REQUIRE_EQ(parity::probe_interval(60'000), parity::probe_interval_cap_us);
    REQUIRE_EQ(parity::probe_interval(100'000), parity::probe_interval_cap_us);
    REQUIRE_EQ(parity::probe_interval(0xffff'ffffU),
        parity::probe_interval_cap_us);
}
