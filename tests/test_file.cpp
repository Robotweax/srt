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

TEST(filecc_runtime_bandwidth_increase_preserves_loss_response)
{
    FileRateController controller {SequenceNumber {0},
        {
            .packet_size_bytes = 1'500,
            .maximum_bandwidth_bytes_per_second = 150'000,
        }};
    controller.on_loss(
        SequenceNumber {0}, SequenceNumber {10}, 1, 10, 100, 50'000);
    REQUIRE(!controller.slow_start());
    const double loss_period = controller.packet_sending_period_microseconds();
    REQUIRE(loss_period > 10'000.0);

    controller.set_maximum_bandwidth(600'000);
    REQUIRE(!controller.slow_start());
    REQUIRE_EQ(controller.packet_sending_period_microseconds(), loss_period);
    controller.set_maximum_bandwidth(0);
    REQUIRE_EQ(controller.packet_sending_period_microseconds(), loss_period);

    // A lower cap still applies, and removing it restores the loss response.
    controller.set_maximum_bandwidth(75'000);
    REQUIRE_EQ(controller.packet_sending_period_microseconds(), 20'000.0);
    controller.set_maximum_bandwidth(600'000);
    REQUIRE_EQ(controller.packet_sending_period_microseconds(), loss_period);
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

TEST(filecc_sparse_ack_without_peer_rate_preserves_startup_fallback)
{
    // Exact inputs at slow-start exit from the 32-stream diagnostic capture:
    // one cumulative ACK after 212278 us, with absent peer rate metrics.
    const SequenceNumber initial {1'913'962'258};
    FileRateController controller {initial,
        {
            .maximum_congestion_window_packets = 16'384,
            .packet_size_bytes = 1'500,
            .maximum_bandwidth_bytes_per_second = 1'250'000'000,
            .rate_control_interval_microseconds = 10'000,
            .start_microseconds = 96'435,
        }};
    controller.on_ack(initial.next(), 308'713, 0, 0, 100'000);
    REQUIRE(controller.slow_start());
    REQUIRE_EQ(controller.congestion_window_packets(), 17U);
    controller.on_loss(
        initial.advanced(8), initial.advanced(63), 2, 17, 0, 100'000);
    REQUIRE(!controller.slow_start());
    REQUIRE_EQ(controller.packet_sending_period_microseconds(),
        std::ceil((110'000.0 / 17.0) * 1.03));
}

TEST(filecc_missing_rate_fallback_ignores_idle_time_and_respects_bandwidth_cap)
{
    for (const auto delay : {100'000ULL, 60'000'000ULL}) {
        FileRateController controller {SequenceNumber {0},
            {
                .packet_size_bytes = 1'500,
                .maximum_bandwidth_bytes_per_second = 75'000,
            }};
        controller.on_ack(SequenceNumber {1}, delay, 0, 0, 90'000);
        controller.on_timeout(0, 90'000);
        REQUIRE(!controller.slow_start());
        REQUIRE_EQ(controller.packet_sending_period_microseconds(), 20'000.0);
        controller.set_maximum_bandwidth(0);
        REQUIRE_EQ(
            controller.packet_sending_period_microseconds(), 100'000.0 / 17.0);
    }
}

TEST(filecc_sparse_ack_with_low_peer_rate_keeps_slow_path_and_loss_response)
{
    for (const auto rate : {1U, 8U}) {
        FileRateController controller {SequenceNumber {0}};
        controller.on_ack(SequenceNumber {1}, 1'000'000, rate, rate, 90'000);
        // on_loss has no new rate sample, but the existing low peer estimate
        // is still valid. It must not be replaced by the no-feedback fallback.
        controller.on_loss(
            SequenceNumber {1}, SequenceNumber {16}, 1, 16, 0, 90'000);
        const auto first_period = std::ceil(1'000'000.0 / rate * 1.03);
        REQUIRE_EQ(
            controller.packet_sending_period_microseconds(), first_period);
        controller.on_loss(
            SequenceNumber {17}, SequenceNumber {32}, 1, 16, 0, 90'000);
        REQUIRE_EQ(controller.packet_sending_period_microseconds(),
            std::ceil(first_period * 1.03));
    }
}
