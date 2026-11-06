#include "test.hpp"
#include "compat/group_unicast_budget.hpp"
#include <future>
#include <random>

using namespace robotweax::srt::compat;
using namespace std::chrono_literals;
namespace {
using Clock = GroupUnicastBudget::Clock;
struct BudgetFixture {
    Clock::time_point now = Clock::time_point {} + 1s;
    GroupUnicastBudget budget;
    explicit BudgetFixture(
        std::uint64_t rate = 1000, std::uint64_t overhead = 28)
        : budget({.wire_bytes_per_second = rate,
              .maximum_wire_datagram = 1500,
              .ip_udp_overhead = overhead,
              .clock =
                  [](void* p) noexcept {
                      return *static_cast<Clock::time_point*>(p);
                  },
              .clock_context = &now})
    {
    }
    void submit(std::uint64_t bytes, bool repair = false)
    {
        auto permit = budget.acquire(bytes);
        REQUIRE(permit);
        permit.finish(true, repair);
    }
};
}

TEST(compat_group_wire_budget_originals_and_repairs_share_ip_udp_burst_and_rate)
{
    for (const auto overhead : {28U, 48U}) {
        BudgetFixture f(1000, overhead);
        f.submit(1500 - overhead);
        REQUIRE(!f.budget.acquire(100 - overhead));
        REQUIRE_EQ(f.budget.delay(100 - overhead), 100ms);
        f.now += 99'999us;
        REQUIRE(!f.budget.acquire(100 - overhead));
        f.now += 1us;
        f.submit(100 - overhead, true);
        const auto state = f.budget.snapshot();
        REQUIRE_EQ(state.original_wire_bytes, 1500U);
        REQUIRE_EQ(state.repair_wire_bytes, 100U);
        REQUIRE_EQ(state.available_bytes, 0U);
        REQUIRE_EQ(state.submitted_packets, 2U);
    }
}

TEST(
    compat_group_wire_budget_permit_excludes_concurrent_io_without_holding_mutex)
{
    BudgetFixture f;
    auto permit = f.budget.acquire(72);
    REQUIRE(permit);
    auto contender = std::async(std::launch::async, [&] {
        REQUIRE(f.budget.snapshot().inflight);
        REQUIRE(!f.budget.acquire(72));
    });
    REQUIRE_EQ(contender.wait_for(1s), std::future_status::ready);
    contender.get();
    f.now += 24h;
    REQUIRE(!f.budget.acquire(72));
    auto moved = std::move(permit);
    REQUIRE(!permit);
    REQUIRE(moved);
    moved.finish(false, false);
    REQUIRE_EQ(f.budget.snapshot().available_bytes, 1500U);
    REQUIRE_EQ(f.budget.snapshot().cancelled_attempts, 1U);
    f.submit(72);
    REQUIRE_EQ(f.budget.snapshot().original_wire_bytes, 100U);
}

TEST(compat_group_wire_budget_socket_time_does_not_earn_credit_or_catchup_burst)
{
    BudgetFixture f(100);
    auto permit = f.budget.acquire(1472);
    REQUIRE(permit);
    f.now += 100s;
    permit.finish(true, false);
    REQUIRE_EQ(f.budget.snapshot().available_bytes, 0U);
    REQUIRE_EQ(f.budget.delay(72), 1s);
    f.now += 1s;
    f.submit(72);
    REQUIRE_EQ(f.budget.snapshot().available_bytes, 0U);
    f.now += 24h;
    REQUIRE_EQ(f.budget.snapshot().available_bytes, 1500U);
}

TEST(
    compat_group_wire_budget_fractional_refill_and_clock_regression_do_not_create_credit)
{
    BudgetFixture f(3);
    f.submit(1472);
    f.now += 333'333us;
    REQUIRE_EQ(f.budget.snapshot().available_bytes, 0U);
    f.now += 1us;
    REQUIRE_EQ(f.budget.snapshot().available_bytes, 1U);
    REQUIRE_EQ(f.budget.delay(1), 9'333'333us);
    f.now -= 333'334us;
    REQUIRE_EQ(f.budget.snapshot().available_bytes, 1U);
    f.now += 9'666'666us;
    REQUIRE(!f.budget.acquire(1));
    f.now += 1us;
    f.submit(1, true);
    REQUIRE_EQ(f.budget.snapshot().repair_wire_bytes, 29U);
}

TEST(
    compat_group_wire_budget_close_fences_new_permits_and_preserves_outstanding_charge)
{
    BudgetFixture f;
    auto permit = f.budget.acquire(1472);
    REQUIRE(permit);
    f.budget.close();
    REQUIRE(!f.budget.acquire(1));
    REQUIRE_EQ(f.budget.delay(1), 1ms);
    permit.finish(true, true);
    const auto state = f.budget.snapshot();
    REQUIRE(state.closed);
    REQUIRE(!state.inflight);
    REQUIRE_EQ(state.repair_wire_bytes, 1500U);
    REQUIRE_EQ(state.submitted_packets, 1U);
}

TEST(compat_group_wire_budget_extreme_rate_and_time_refill_without_overflow)
{
    BudgetFixture f(UINT64_MAX);
    f.submit(1472);
    f.now += 1us;
    f.submit(1472);
    f.now = Clock::time_point::max();
    f.submit(1472);
    f.now = Clock::time_point::min();
    REQUIRE(!f.budget.acquire(1));
    REQUIRE_EQ(f.budget.delay(1), 1us);
    REQUIRE(!f.budget.acquire(UINT64_MAX));
    REQUIRE(!f.budget.acquire(0));
    GroupUnicastBudget invalid(
        {.wire_bytes_per_second = 1, .maximum_wire_datagram = 65536});
    REQUIRE(invalid.snapshot().closed);
    REQUIRE(!invalid.acquire(1));
    GroupUnicastBudget disabled({.wire_bytes_per_second = 0});
    REQUIRE(disabled.snapshot().closed);
    REQUIRE(!disabled.acquire(1));
}

TEST(
    compat_group_wire_budget_mixed_trace_obeys_wire_envelope_and_failed_attempts_are_free)
{
    BudgetFixture f(731);
    std::mt19937 random(71);
    const auto start = f.now;
    std::uint64_t charged = 0;
    for (unsigned i = 0; i < 4000; ++i) {
        f.now += std::chrono::microseconds {random() % 5000};
        const auto bytes = 1U + random() % 1472;
        if (auto permit = f.budget.acquire(bytes)) {
            const bool success = random() % 3 != 0;
            permit.finish(success, i % 2);
            if (success)
                charged += bytes + 28;
        }
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::microseconds>(f.now - start)
                .count();
        REQUIRE(charged
            <= 1500U + 731U * static_cast<std::uint64_t>(elapsed) / 1'000'000U);
        const auto state = f.budget.snapshot();
        REQUIRE_EQ(
            state.original_wire_bytes + state.repair_wire_bytes, charged);
        REQUIRE(state.available_bytes <= state.burst_bytes);
    }
    REQUIRE(charged > 1500U);
}
