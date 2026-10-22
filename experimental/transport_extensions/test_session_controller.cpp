#include "compiled_congestion.hpp"
#include "test.hpp"
#include "robotweax/srt/session.hpp"
#include "robotweax/srt/codec.hpp"
#include "compat/transport_runtime.hpp"

#include <array>
#include <memory>

using namespace robotweax::srt;
using namespace robotweax::srt::extensions;

namespace {
struct Observer {
    std::uint64_t acknowledged = 0;
    unsigned callbacks = 0;
    bool fail = false;
} observed;

ControllerDescriptor descriptor()
{
    return {.name = "test-adaptive-v1",
        .state_bytes = 1,
        .construct =
            [](std::span<std::byte>, ControllerLimits) {
                return Error::none;
            },
        .on_event =
            [](std::span<std::byte>, const ControllerEvent& event,
                ControllerDecision& result) {
                ++observed.callbacks;
                observed.acknowledged += event.acknowledged_bytes;
                if (observed.fail)
                    return Error::invalid_state;
                result = {2'000, 2};
                return Error::none;
            },
        .destroy = [](std::span<std::byte>) noexcept { }};
}

PacketView ack(SequenceNumber next, std::array<std::byte, 64>& storage)
{
    const auto encoded = encode_reliability_action(
        {.kind = ReliabilityActionKind::acknowledgement,
            .acknowledgement = {.kind = AcknowledgementKind::lite,
                .next_sequence = next}},
        PacketTimestamp {}, 0, storage);
    REQUIRE(encoded);
    const auto decoded =
        decode_packet(std::span {storage}.first(encoded.bytes_written));
    REQUIRE(decoded);
    return decoded.packet;
}
}

TEST(
    experimental_controller_session_paces_caps_and_accepts_only_validated_feedback)
{
    observed = {};
    ReliabilitySession session {{.local_initial_sequence = SequenceNumber {10},
        .send_capacity_packets = 8,
        .receive_capacity_packets = 8}};
    session.configure_live({}, 0, PacketTimestamp {});
    REQUIRE_EQ(
        session.configure_experimental_controller(descriptor(), 1'000, 1, 0),
        Error::none);
    REQUIRE_EQ(session.live_pacing_rate_bytes_per_second(), 1'000U);
    REQUIRE_EQ(session.experimental_window_packets(), 1U);
    const std::array<std::byte, 100> payload {};
    REQUIRE_EQ(session.queue_message(payload, PacketTimestamp {}), Error::none);
    REQUIRE_EQ(session.queue_message(payload, PacketTimestamp {}), Error::none);
    PacketPacer pacer {10'000, 8};
    REQUIRE(session.next_paced_data_packet(pacer, 0));
    session.note_packet_submitted(SequenceNumber {10});
    // Time alone cannot bypass the controller's one-packet window.
    REQUIRE(!session.next_paced_data_packet(pacer, 1'000'000));
    std::array<std::byte, 64> storage {};
    const auto before = observed.callbacks;
    REQUIRE(!session.receive(ack(SequenceNumber {99}, storage), 1'000'001));
    REQUIRE_EQ(observed.callbacks, before);
    REQUIRE(session.receive(ack(SequenceNumber {11}, storage), 1'000'002));
    REQUIRE_EQ(observed.acknowledged, payload.size());
    REQUIRE(session.next_paced_data_packet(pacer, 1'000'003));
    SocketOptions options;
    REQUIRE_EQ(
        options.set(SocketOption::maximum_bandwidth_bytes_per_second, 500),
        Error::none);
    REQUIRE_EQ(session.apply_dynamic_options(options), Error::none);
    REQUIRE_EQ(session.live_pacing_rate_bytes_per_second(), 500U);
    REQUIRE_EQ(
        options.set(SocketOption::maximum_bandwidth_bytes_per_second, 1'500),
        Error::none);
    REQUIRE_EQ(session.apply_dynamic_options(options), Error::none);
    REQUIRE_EQ(session.poll_experimental_controller(1'000'004), Error::none);
    REQUIRE_EQ(session.live_pacing_rate_bytes_per_second(), 1'500U);
}

TEST(experimental_controller_session_failure_does_not_fall_back_to_livecc)
{
    observed = {};
    ReliabilitySession session {
        {.send_capacity_packets = 8, .receive_capacity_packets = 8}};
    session.configure_live({}, 0, PacketTimestamp {});
    REQUIRE_EQ(
        session.configure_experimental_controller(descriptor(), 1'000, 8, 0),
        Error::none);
    const std::array payload {std::byte {'x'}};
    REQUIRE_EQ(session.queue_message(payload, PacketTimestamp {}), Error::none);
    observed.fail = true;
    PacketPacer pacer {1'000, 8};
    REQUIRE(!session.next_paced_data_packet(pacer, 1));
    REQUIRE_EQ(session.experimental_controller_error(), Error::invalid_state);
    const auto before = observed.callbacks;
    REQUIRE(!session.next_paced_data_packet(pacer, 2));
    REQUIRE_EQ(observed.callbacks, before);
    REQUIRE_EQ(session.queue_message(payload, PacketTimestamp {}),
        Error::invalid_state);
    REQUIRE_EQ(session.queue_stream(payload, PacketTimestamp {}).error,
        Error::invalid_state);
    REQUIRE(!session.next_data_packet());
}

TEST(experimental_controller_runtime_failure_becomes_terminal)
{
    SocketOptions options;
    SKIP_UNLESS(
        options.set_congestion_controller("rwx-fixed-v1") == Error::none,
        "test uses the bundled provider");
    std::uint64_t now = 100;
    compat::ConnectionRuntime runtime {{.peer = IpEndpoint::loopback(9'000),
        .peer_socket_id = 77,
        .flow_window_packets = 8,
        .options = options,
        .now_function =
            [](void* context) noexcept {
                return *static_cast<std::uint64_t*>(context);
            },
        .now_context = &now}};
    REQUIRE(!runtime.broken());
    REQUIRE_EQ(
        runtime.statistics(false, true).instantaneous.congestion_window_packets,
        8U);
    now = 99;
    (void)runtime.poll();
    REQUIRE(runtime.broken());
}
