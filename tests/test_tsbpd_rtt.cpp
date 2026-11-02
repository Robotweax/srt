#include "test.hpp"

#include "robotweax/srt/codec.hpp"
#include "robotweax/srt/session.hpp"
#include "robotweax/srt/timing.hpp"

#include <array>
#include <cstring>
#include <deque>
#include <vector>

using namespace robotweax::srt;

TEST(tsbpd_rtt_reference_waits_for_a_valid_estimate)
{
    for (const std::uint32_t rtt : {10'000U, 80'000U, 100'000U, 200'000U}) {
        TsbpdClock clock {1'000'000, PacketTimestamp {0}, 120'000};
        for (std::uint32_t index = 0; index < 3'000; ++index) {
            const PacketTimestamp timestamp {index * 1'000};
            const auto arrival = 1'000'000ULL + timestamp.value();
            const auto estimate =
                index < 1'500 ? std::nullopt : std::optional {rtt};
            const auto update =
                clock.observe_arrival(timestamp, arrival, estimate);
            REQUIRE_EQ(update.correction_microseconds, 0);
            REQUIRE_EQ(clock.delivery_time(timestamp), arrival + 120'000);
        }
    }
}

TEST(tsbpd_rtt_real_100ms_reference_survives_unmeasured_samples)
{
    TsbpdClock clock {1'000'000, PacketTimestamp {0}, 120'000};
    (void)clock.observe_arrival(PacketTimestamp {0}, 1'000'000, 100'000);
    for (std::uint32_t index = 1; index < 2'000; ++index) {
        const PacketTimestamp timestamp {index * 1'000};
        const auto estimate =
            index < 100 ? std::nullopt : std::optional {80'000U};
        const auto arrival = 1'000'000ULL + timestamp.value()
            - (estimate.has_value() ? 10'000U : 0U);
        const auto update = clock.observe_arrival(timestamp, arrival, estimate);
        REQUIRE_EQ(update.correction_microseconds, 0);
        REQUIRE_EQ(
            clock.delivery_time(timestamp), 1'120'000ULL + timestamp.value());
    }
}

TEST(tsbpd_rtt_unknown_estimate_preserves_physical_drift_sampling)
{
    TsbpdClock clock {1'000'000, PacketTimestamp {0}, 120'000};
    DriftUpdate update;
    for (std::uint32_t index = 0; index < 2'000; ++index) {
        const PacketTimestamp timestamp {index * 1'000};
        update =
            clock.observe_arrival(timestamp, 1'008'000ULL + timestamp.value(),
                index < 1'000 ? std::nullopt : std::optional {80'000U});
        if (index == 999) {
            REQUIRE(update.window_complete);
            REQUIRE_EQ(update.correction_microseconds, 5'000);
        }
    }
    REQUIRE(update.window_complete);
    REQUIRE_EQ(update.correction_microseconds, 3'000);
}

namespace {

constexpr std::uint32_t cadence = 1'000;
constexpr std::uint64_t latency = 120'000;

struct Flight {
    std::uint64_t due = 0;
    std::size_t destination = 0;
    std::vector<std::byte> bytes;
};

// Both clocks run at exactly the same rate. DATA, ACK and ACKACK traverse the
// same constant-delay path, with no sleeps, sockets or future-time callbacks.
std::array<std::uint32_t, 2> require_drift_free_live_stream(std::uint32_t rtt,
    bool bidirectional, bool tracer = true, bool delay_first_sender = false)
{
    const std::size_t count = bidirectional ? 3'000U : 12'000U;
    const std::uint64_t one_way = rtt / 2U;
    std::array<ReliabilitySession, 2> sessions {
        ReliabilitySession {{.local_initial_sequence = SequenceNumber {10},
            .peer_initial_sequence = SequenceNumber {100},
            .send_capacity_packets = 512,
            .receive_capacity_packets = 512}},
        ReliabilitySession {{.local_initial_sequence = SequenceNumber {100},
            .peer_initial_sequence = SequenceNumber {10},
            .send_capacity_packets = 512,
            .receive_capacity_packets = 512}},
    };
    for (auto& session : sessions) {
        session.configure_live({.send_tsbpd = true,
                                   .receive_tsbpd = true,
                                   .retransmit_flag = true,
                                   .receive_delay_milliseconds = 120,
                                   .peer_receive_delay_milliseconds = 120},
            one_way, PacketTimestamp {0});
        SocketOptions options;
        REQUIRE_EQ(options.set(SocketOption::drift_tracer, tracer ? 1 : 0),
            Error::none);
        REQUIRE_EQ(session.apply_dynamic_options(options), Error::none);
    }
    const std::array<std::uint64_t, 2> start {
        delay_first_sender ? 3ULL * rtt : 0U, 0U};
    std::array<std::size_t, 2> sent {};
    std::array<std::size_t, 2> delivered {};
    std::array<bool, 2> received_data {};
    std::array<std::uint32_t, 2> first_data_rtt_source {};
    std::deque<Flight> flights;
    const auto control = [&](const ReliabilityAction& action,
                             std::size_t sender, std::uint64_t now) {
        std::array<std::byte, 64> bytes {};
        const auto encoded = encode_reliability_action(action,
            PacketTimestamp {static_cast<std::uint32_t>(now)}, 77, bytes);
        REQUIRE(encoded);
        flights.push_back({now + one_way, 1U - sender,
            {bytes.begin(), bytes.begin() + encoded.bytes_written}});
    };
    const auto stop =
        start[0] + count * cadence + one_way + latency + 2ULL * rtt;
    for (std::uint64_t now = 0; now <= stop; now += cadence) {
        for (std::size_t side = 0; side < (bidirectional ? 2U : 1U); ++side) {
            if (now < start[side] || sent[side] == count)
                continue;
            const std::array<std::uint32_t, 2> payload {
                static_cast<std::uint32_t>(side),
                static_cast<std::uint32_t>(sent[side])};
            const auto timestamp =
                PacketTimestamp {static_cast<std::uint32_t>(now)};
            REQUIRE_EQ(sessions[side].queue_message(
                           std::as_bytes(std::span {payload}), timestamp),
                Error::none);
            const auto packet = sessions[side].next_data_packet(now);
            REQUIRE(packet.has_value());
            std::array<std::byte, 64> bytes {};
            const auto encoded = encode_packet({.kind = PacketKind::data,
                                                   .data = packet->header,
                                                   .payload = packet->payload},
                bytes);
            REQUIRE(encoded);
            sessions[side].note_packet_submitted(packet->header.sequence);
            flights.push_back({now + one_way, 1U - side,
                {bytes.begin(), bytes.begin() + encoded.bytes_written}});
            ++sent[side];
        }
        while (!flights.empty() && flights.front().due <= now) {
            const auto flight = std::move(flights.front());
            flights.pop_front();
            REQUIRE_EQ(flight.due, now);
            const auto packet = decode_packet(flight.bytes);
            REQUIRE(packet);
            auto& session = sessions[flight.destination];
            if (packet.packet.kind == PacketKind::data
                && !received_data[flight.destination]) {
                received_data[flight.destination] = true;
                first_data_rtt_source[flight.destination] =
                    session.rtt().last_observation_source();
            }
            const auto received = session.receive(packet.packet, now);
            REQUIRE(received);
            for (std::size_t index = 0; index < received.actions.size; ++index)
                control(
                    received.actions.values[index], flight.destination, now);
        }
        for (std::size_t side = 0; side < 2; ++side) {
            const auto actions = sessions[side].poll_timers(now);
            for (std::size_t index = 0; index < actions.size; ++index)
                control(actions.values[index], side, now);
            for (;;) {
                std::array<std::uint32_t, 2> payload {};
                const auto message = sessions[side].pop_message_at(
                    std::as_writable_bytes(std::span {payload}), now);
                if (!message) {
                    REQUIRE_EQ(message.error, Error::would_block);
                    break;
                }
                const std::size_t sender = 1U - side;
                REQUIRE_EQ(message.bytes_written, sizeof(payload));
                REQUIRE_EQ(payload[0], sender);
                REQUIRE_EQ(payload[1], delivered[side]);
                REQUIRE(delivered[side] < count);
                const auto nominal = start[sender] + delivered[side] * cadence
                    + one_way + latency;
                REQUIRE(message.delivery_time_microseconds.has_value());
                REQUIRE_EQ(*message.delivery_time_microseconds, nominal);
                REQUIRE_EQ(now, nominal);
                ++delivered[side];
            }
        }
    }
    REQUIRE_EQ(delivered[1], count);
    REQUIRE_EQ(delivered[0], bidirectional ? count : 0U);
    for (std::size_t side = bidirectional ? 0U : 1U; side < 2; ++side) {
        REQUIRE(sessions[side].rtt().has_sample());
        REQUIRE_EQ(sessions[side].rtt().smoothed_microseconds(), rtt);
        REQUIRE_EQ(sessions[side].drift_correction_count(), 0U);
        REQUIRE_EQ(sessions[side].total_drift_correction_microseconds(), 0);
    }
    return first_data_rtt_source;
}

} // namespace

TEST(tsbpd_rtt_live_stream_keeps_deadlines_when_the_first_measurement_arrives)
{
    for (const std::uint32_t rtt : {10'000U, 80'000U, 100'000U, 200'000U})
        (void)require_drift_free_live_stream(rtt, false);
}

TEST(tsbpd_rtt_bidirectional_stream_keeps_deadlines)
{
    for (const std::uint32_t rtt : {10'000U, 80'000U, 100'000U, 200'000U})
        (void)require_drift_free_live_stream(rtt, true);
}

TEST(tsbpd_rtt_peer_estimate_before_first_data_is_a_valid_reference)
{
    const auto source =
        require_drift_free_live_stream(80'000, true, true, true);
    REQUIRE_EQ(source[1], 2U);
}

TEST(tsbpd_rtt_disabled_tracer_keeps_the_same_live_deadlines)
{
    (void)require_drift_free_live_stream(80'000, false, false);
}
