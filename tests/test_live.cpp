#include "test.hpp"

#include "robotweax/srt/live.hpp"

using namespace robotweax::srt;

TEST(arrival_rate_estimator_reports_ack_rate_fields)
{
    ArrivalRateEstimator estimator;
    estimator.observe(1'000, 1'000);
    estimator.observe(1'000, 2'000);
    estimator.observe(1'000, 3'000);
    estimator.observe_probe_pair(4'000, 4'500);

    const auto rates = estimator.rates();
    REQUIRE(rates.valid);
    REQUIRE_EQ(rates.packets_per_second, 1'000U);
    REQUIRE_EQ(rates.bytes_per_second, 1'044'000U);
    REQUIRE_EQ(rates.link_capacity_packets_per_second, 2'000U);
}

TEST(live_rate_controller_defaults_to_reference_one_gigabit_ceiling)
{
    LiveRateController controller{{}};
    REQUIRE_EQ(controller.pacing_rate_bytes_per_second(), 125'000'000U);
}

TEST(live_rate_controller_applies_overhead_cap_and_packet_size)
{
    LiveRateController controller{{
        .input_bandwidth_bytes_per_second = 1'000'000,
        .maximum_bandwidth_bytes_per_second = 1'100'000,
        .overhead_percent = 25,
        .maximum_payload_size = 1'000,
        .packet_header_size = 40,
    }};
    REQUIRE_EQ(controller.pacing_rate_bytes_per_second(), 1'100'000U);
    REQUIRE_EQ(controller.packet_interval_microseconds(), 946U);
    controller.set_maximum_bandwidth(0);
    REQUIRE_EQ(controller.pacing_rate_bytes_per_second(), 1'250'000U);
}

TEST(live_rate_controller_uses_minimum_input_only_for_auto_estimation)
{
    LiveRateController controller{{
        .minimum_input_bandwidth_bytes_per_second = 400'000,
        .maximum_bandwidth_bytes_per_second = 0,
        .overhead_percent = 25,
    }};
    REQUIRE_EQ(controller.pacing_rate_bytes_per_second(), 500'000U);

    controller.set_input_bandwidth(600'000);
    REQUIRE_EQ(controller.pacing_rate_bytes_per_second(), 750'000U);
    controller.set_maximum_bandwidth(700'000);
    REQUIRE_EQ(controller.pacing_rate_bytes_per_second(), 700'000U);
    controller.set_maximum_bandwidth(-1);
    REQUIRE_EQ(controller.pacing_rate_bytes_per_second(), 125'000'000U);
}

TEST(live_rate_controller_estimates_input_with_fixed_memory)
{
    LiveRateController controller{{
        .maximum_bandwidth_bytes_per_second = 0,
        .overhead_percent = 0,
    }};
    controller.observe_input(1'000, 1);
    controller.observe_input(1'000, 100'001);
    REQUIRE_EQ(controller.pacing_rate_bytes_per_second(), 20'000U);
}

TEST(live_rate_controller_absolute_bandwidth_ignores_relative_inputs)
{
    LiveRateController controller {{
        .input_bandwidth_bytes_per_second = 1'000'000,
        .minimum_input_bandwidth_bytes_per_second = 2'000'000,
        .maximum_bandwidth_bytes_per_second = 1'250'000'000,
        .overhead_percent = 25,
    }};
    REQUIRE_EQ(controller.pacing_rate_bytes_per_second(), 1'250'000'000U);
    controller.set_input_bandwidth(0);
    controller.observe_input(1'000, 1);
    controller.observe_input(1'000, 100'001);
    controller.set_overhead_percent(50);
    REQUIRE_EQ(controller.pacing_rate_bytes_per_second(), 1'250'000'000U);

    controller.set_maximum_bandwidth(0);
    REQUIRE_EQ(controller.pacing_rate_bytes_per_second(), 3'000'000U);
    controller.set_minimum_input_bandwidth(0);
    REQUIRE_EQ(controller.pacing_rate_bytes_per_second(), 30'000U);
    controller.set_maximum_bandwidth(10'000);
    REQUIRE_EQ(controller.pacing_rate_bytes_per_second(), 10'000U);
    controller.set_maximum_bandwidth(1'250'000'000);
    REQUIRE_EQ(controller.pacing_rate_bytes_per_second(), 1'250'000'000U);
    controller.set_maximum_bandwidth(-1);
    REQUIRE_EQ(controller.pacing_rate_bytes_per_second(), 125'000'000U);
}

TEST(live_rate_controller_framed_estimate_preserves_explicit_bandwidth)
{
    LiveRateController controller {{}};
    controller.observe_input(64, 1, 32);
    controller.observe_input(64, 50'001, 32);
    controller.observe_input(64, 100'001, 32);
    REQUIRE_EQ(controller.pacing_rate_bytes_per_second(), 3'600U);

    // A configured minimum uses the original payload estimate, whether or
    // not the floor binds. Explicit input, fixed and unlimited modes win.
    controller.set_minimum_input_bandwidth(1'000);
    REQUIRE_EQ(controller.pacing_rate_bytes_per_second(), 2'400U);
    controller.set_minimum_input_bandwidth(2'000);
    REQUIRE_EQ(controller.pacing_rate_bytes_per_second(), 2'500U);
    controller.set_input_bandwidth(1'600);
    REQUIRE_EQ(controller.pacing_rate_bytes_per_second(), 2'000U);
    controller.set_maximum_bandwidth(1'200);
    REQUIRE_EQ(controller.pacing_rate_bytes_per_second(), 1'200U);
    controller.set_maximum_bandwidth(-1);
    REQUIRE_EQ(controller.pacing_rate_bytes_per_second(), 125'000'000U);

    controller.set_input_bandwidth(0);
    controller.set_minimum_input_bandwidth(0);
    controller.set_maximum_bandwidth(0);
    REQUIRE_EQ(controller.pacing_rate_bytes_per_second(), 3'600U);
}

TEST(drift_sampler_bounds_each_timebase_correction)
{
    DriftSampler sampler{4, 5'000};
    for (int index = 0; index < 3; ++index) {
        REQUIRE(!sampler.observe(8'000).window_complete);
    }
    const auto positive = sampler.observe(8'000);
    REQUIRE(positive.window_complete);
    REQUIRE_EQ(positive.correction_microseconds, 5'000);
    REQUIRE_EQ(positive.residual_drift_microseconds, 3'000);

    for (int index = 0; index < 3; ++index) {
        static_cast<void>(sampler.observe(-7'000));
    }
    const auto negative = sampler.observe(-7'000);
    REQUIRE_EQ(negative.correction_microseconds, -5'000);
    REQUIRE_EQ(negative.residual_drift_microseconds, -7'000);

    for (int index = 0; index < 3; ++index) {
        static_cast<void>(sampler.observe(-7'000));
    }
    const auto converging = sampler.observe(-7'000);
    REQUIRE_EQ(converging.correction_microseconds, -5'000);
    REQUIRE_EQ(converging.residual_drift_microseconds, -2'000);
}
