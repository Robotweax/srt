#include "test.hpp"

#include "robotweax/srt/file.hpp"

#include <cmath>

using namespace robotweax::srt;

TEST(filecc_starts_with_specified_window_and_rate_control_period)
{
    FileRateController controller{SequenceNumber{100}, {
        .maximum_congestion_window_packets = 20,
        .packet_size_bytes = 1'500,
    }};
    REQUIRE(controller.slow_start());
    REQUIRE_EQ(controller.congestion_window_packets(), 16U);
    REQUIRE_EQ(controller.packet_sending_period_microseconds(), 1.0);

    controller.on_ack(SequenceNumber{110}, 9'999, 1'000, 2'000, 50'000);
    REQUIRE_EQ(controller.congestion_window_packets(), 16U);
    controller.on_ack(SequenceNumber{110}, 10'000, 1'000, 2'000, 50'000);
    REQUIRE(!controller.slow_start());
    REQUIRE_EQ(controller.congestion_window_packets(), 26U);
    REQUIRE_EQ(controller.packet_sending_period_microseconds(), 1'000.0);
    REQUIRE_EQ(controller.pacing_rate_bytes_per_second(), 1'500'000U);
}

TEST(filecc_tolerates_sub_two_percent_loss_and_reduces_on_congestion)
{
    FileRateController tolerant{SequenceNumber{10}, {
        .maximum_congestion_window_packets = 16,
    }};
    tolerant.on_ack(SequenceNumber{11}, 10'000, 1'000, 2'000, 50'000);
    REQUIRE(!tolerant.slow_start());
    const double initial_period =
        tolerant.packet_sending_period_microseconds();
    tolerant.on_loss(SequenceNumber{11}, SequenceNumber{30},
        1, 100, 1'000, 50'000);
    REQUIRE_EQ(tolerant.packet_sending_period_microseconds(),
        initial_period);

    FileRateController congested{SequenceNumber{10}, {
        .maximum_congestion_window_packets = 16,
    }};
    congested.on_ack(SequenceNumber{11}, 10'000, 1'000, 2'000, 50'000);
    congested.on_loss(SequenceNumber{11}, SequenceNumber{30},
        2, 100, 1'000, 50'000);
    REQUIRE(congested.packet_sending_period_microseconds()
        > initial_period);
}

TEST(filecc_honors_an_explicit_maximum_bandwidth)
{
    FileRateController controller{SequenceNumber{0}, {
        .packet_size_bytes = 1'500,
        .maximum_bandwidth_bytes_per_second = 150'000,
    }};
    REQUIRE_EQ(controller.packet_sending_period_microseconds(), 10'000.0);
    REQUIRE_EQ(controller.pacing_rate_bytes_per_second(), 150'000U);
}

TEST(filecc_runtime_bandwidth_changes_preserve_unlimited_controller_state)
{
    FileRateController controller{SequenceNumber{0}, {
        .maximum_congestion_window_packets = 16,
        .packet_size_bytes = 1'500,
        .maximum_bandwidth_bytes_per_second = 150'000,
    }};
    controller.on_ack(
        SequenceNumber{17}, 10'000, 1'000, 2'000, 50'000);
    REQUIRE(!controller.slow_start());
    REQUIRE_EQ(controller.packet_sending_period_microseconds(), 10'000.0);
    REQUIRE_EQ(controller.pacing_rate_bytes_per_second(), 150'000U);

    controller.set_maximum_bandwidth(600'000);
    REQUIRE_EQ(controller.packet_sending_period_microseconds(), 2'500.0);
    REQUIRE_EQ(controller.pacing_rate_bytes_per_second(), 600'000U);

    controller.set_maximum_bandwidth(0);
    REQUIRE_EQ(controller.packet_sending_period_microseconds(), 1'000.0);
    REQUIRE_EQ(controller.pacing_rate_bytes_per_second(), 1'500'000U);

    controller.set_maximum_bandwidth(75'000);
    REQUIRE_EQ(controller.packet_sending_period_microseconds(), 20'000.0);
    REQUIRE_EQ(controller.pacing_rate_bytes_per_second(), 75'000U);
}

TEST(filecc_delayed_ack_and_burst_loss_update_only_on_observed_feedback)
{
    FileRateController controller{SequenceNumber{100}, {
        .maximum_congestion_window_packets = 16,
        .packet_size_bytes = 1'500,
    }};
    controller.on_ack(
        SequenceNumber{117}, 10'000, 1'000, 2'000, 50'000);
    REQUIRE(!controller.slow_start());
    const double before_delay =
        controller.packet_sending_period_microseconds();

    controller.on_ack(
        SequenceNumber{118}, 19'999, 1'000, 2'000, 50'000);
    REQUIRE_EQ(
        controller.packet_sending_period_microseconds(), before_delay);

    controller.on_loss(
        SequenceNumber{118}, SequenceNumber{217},
        3, 100, 1'000, 50'000);
    REQUIRE_EQ(
        controller.packet_sending_period_microseconds(),
        std::ceil(before_delay * 1.03));
}

TEST(filecc_slow_start_exit_rejects_an_immature_peer_rate)
{
    const SequenceNumber initial{SequenceNumber::mask - 24U};
    const SequenceNumber acknowledged = initial.advanced(50U);
    FileRateController controller{initial, {
        .maximum_congestion_window_packets = 100,
        .packet_size_bytes = 1'500,
    }};
    controller.on_ack(
        acknowledged, 10'000, 5, 10, 10'000);
    REQUIRE(controller.slow_start());

    controller.on_loss(
        acknowledged, initial.advanced(100U),
        1, 100, 0, 10'000);
    REQUIRE(!controller.slow_start());
    REQUIRE(controller.packet_sending_period_microseconds() > 303.0);
    REQUIRE(controller.packet_sending_period_microseconds() < 304.0);
    REQUIRE(controller.pacing_rate_bytes_per_second() > 4'900'000U);
}

TEST(filecc_slow_start_ack_floor_preserves_a_genuinely_slow_path)
{
    FileRateController controller{SequenceNumber{0}, {
        .maximum_congestion_window_packets = 100,
        .packet_size_bytes = 1'500,
    }};
    controller.on_ack(
        SequenceNumber{1}, 1'000'000, 1, 1, 90'000);
    controller.on_loss(
        SequenceNumber{1}, SequenceNumber{100},
        1, 100, 0, 90'000);

    REQUIRE_EQ(
        controller.packet_sending_period_microseconds(), 1'000'000.0);
    REQUIRE_EQ(controller.pacing_rate_bytes_per_second(), 1'500U);
}

TEST(filecc_slow_start_without_rate_uses_window_per_feedback_time)
{
    FileRateController controller{SequenceNumber{0}, {
        .maximum_congestion_window_packets = 100,
        .packet_size_bytes = 1'500,
    }};
    controller.on_timeout(0, 90'000);

    REQUIRE_EQ(
        controller.packet_sending_period_microseconds(), 6'250.0);
    REQUIRE_EQ(controller.pacing_rate_bytes_per_second(), 240'000U);
}
