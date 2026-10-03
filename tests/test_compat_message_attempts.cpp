#include "test.hpp"

#include "compat/error_state.hpp"
#include "compat/message_io.hpp"
#include "compat/transport_runtime.hpp"
#include "robotweax/srt/codec.hpp"
#include "srt/srt.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

using namespace robotweax::srt;
using namespace robotweax::srt::compat;

namespace {

struct MessageAttemptFixture {
    static constexpr std::size_t payload_size = 800;
    static constexpr std::uint32_t first_sequence = 1000;
    std::vector<std::vector<std::byte>> sent;
    std::shared_ptr<DatagramChannel> channel =
        std::make_shared<DatagramChannel>();
    SocketRecord socket;
    const Ipv4Endpoint peer {.address = {192, 0, 2, 90}, .port = 14900};
    std::uint64_t now = 1000;

    static std::uint64_t clock(void* context) noexcept
    {
        return *static_cast<std::uint64_t*>(context);
    }

    static UdpIoResult capture(
        std::span<const std::byte> bytes, Ipv4Endpoint, void* context) noexcept
    {
        auto& fixture = *static_cast<MessageAttemptFixture*>(context);
        try {
            fixture.sent.emplace_back(bytes.begin(), bytes.end());
            return {.bytes_transferred = bytes.size()};
        } catch (...) {
            return {.error = Error::io_error};
        }
    }

    MessageAttemptFixture()
    {
        channel->set_send_hook_for_testing(capture, this);
        REQUIRE_EQ(socket.native_options.set(
                       SocketOption::maximum_payload_size, payload_size),
            Error::none);
        REQUIRE_EQ(
            socket.native_options.set(SocketOption::send_buffer_packets, 3),
            Error::none);
        REQUIRE_EQ(
            socket.native_options.set(SocketOption::receive_buffer_packets, 4),
            Error::none);
        REQUIRE_EQ(socket.native_options.set(SocketOption::tsbpd_mode, 0),
            Error::none);
        socket.state = SRTS_CONNECTED;
        socket.public_options.maximum_payload_size = payload_size;
        socket.public_options.tsbpd_mode = false;
        socket.public_options.send_synchronous = false;
        socket.public_options.receive_synchronous = false;
        socket.public_options.send_timeout_milliseconds = 0;
        socket.public_options.receive_timeout_milliseconds = 0;
        socket.runtime = std::make_shared<ConnectionRuntime>(
            ConnectionRuntime::Configuration {
                .channel = channel,
                .peer = peer,
                .peer_socket_id = 90,
                .initial_sequence = SequenceNumber {first_sequence},
                .flow_window_packets = 256,
                .options = socket.native_options,
                .origin = ConnectionRuntime::Clock::now(),
                .now_function = clock,
                .now_context = &now,
            });
    }

    void publish_message(std::span<const std::byte> payload)
    {
        REQUIRE_EQ(payload.size(), 2U * payload_size);
        for (std::size_t index = 0; index != 2U; ++index) {
            PacketView packet;
            packet.kind = PacketKind::data;
            packet.data.sequence = SequenceNumber {
                first_sequence + static_cast<std::uint32_t>(index)};
            packet.data.message_number = 7;
            packet.data.in_order = true;
            packet.data.boundary =
                index == 0U ? MessageBoundary::first : MessageBoundary::last;
            packet.payload =
                payload.subspan(index * payload_size, payload_size);
            socket.runtime->process_packet(packet, peer);
        }
    }

    void publish_control(ControlType type)
    {
        const std::array<std::byte, 4> padding {};
        PacketView packet;
        packet.kind = PacketKind::control;
        packet.control.type = type;
        packet.control.type_specific = SRT_EFILE;
        packet.payload = padding;
        socket.runtime->process_packet(packet, peer);
    }
};

} // namespace

TEST(compat_message_attempt_fragmented_send_is_atomic_and_owns_payload)
{
    MessageAttemptFixture fixture;
    std::array<char, 2U * MessageAttemptFixture::payload_size> input;
    input.fill('a');
    SRT_MSGCTRL first = srt_msgctrl_default;
    // A zero configured timeout does not override an immediately available
    // nonblocking commit, including a message larger than one datagram.
    REQUIRE_EQ(
        send_message(&fixture.socket, input.data(), input.size(), &first),
        static_cast<int>(input.size()));
    REQUIRE_EQ(first.msgno, 1);
    input.fill('b'); // Caller ownership returns at success.
    SRT_MSGCTRL rejected = srt_msgctrl_default;
    rejected.msgno = 77;
    REQUIRE_EQ(
        send_message(&fixture.socket, input.data(), input.size(), &rejected),
        SRT_ERROR);
    REQUIRE_EQ(last_error().code, SRT_EASYNCSND);
    REQUIRE_EQ(rejected.msgno, 77);
    REQUIRE_EQ(
        fixture.socket.runtime->buffer_packet_counts().unacknowledged_send, 2U);

    fixture.socket.public_options.send_synchronous = true;
    REQUIRE_EQ(
        send_message(&fixture.socket, input.data(), input.size(), &rejected),
        SRT_ERROR);
    REQUIRE_EQ(last_error().code, SRT_ETIMEOUT);
    REQUIRE_EQ(rejected.msgno, 77);
    REQUIRE_EQ(
        fixture.socket.runtime->buffer_packet_counts().unacknowledged_send, 2U);

    SRT_MSGCTRL last = srt_msgctrl_default;
    REQUIRE_EQ(send_message(&fixture.socket, input.data(),
                   MessageAttemptFixture::payload_size, &last),
        static_cast<int>(MessageAttemptFixture::payload_size));
    REQUIRE_EQ(last.msgno, 2); // Rejected attempts consumed no message number.
    for (std::size_t turn = 0; turn != 8U; ++turn) {
        (void)fixture.socket.runtime->poll();
        fixture.now += 1000;
    }
    std::size_t data_count = 0;
    for (const auto& bytes : fixture.sent) {
        const auto decoded = decode_packet(bytes);
        REQUIRE(decoded);
        if (decoded.packet.kind != PacketKind::data)
            continue;
        REQUIRE_EQ(decoded.packet.data.sequence,
            SequenceNumber {MessageAttemptFixture::first_sequence
                + static_cast<std::uint32_t>(data_count)});
        REQUIRE_EQ(
            decoded.packet.data.message_number, data_count < 2U ? 1U : 2U);
        REQUIRE_EQ(
            decoded.packet.payload.size(), MessageAttemptFixture::payload_size);
        REQUIRE(std::all_of(decoded.packet.payload.begin(),
            decoded.packet.payload.end(), [data_count](std::byte value) {
                return value
                    == (data_count < 2U ? std::byte {'a'} : std::byte {'b'});
            }));
        ++data_count;
    }
    REQUIRE_EQ(data_count, 3U);
}

TEST(compat_message_attempt_receive_timeout_does_not_retain_caller_storage)
{
    MessageAttemptFixture fixture;
    std::array<char, 1601> expired;
    expired.fill('x');
    SRT_MSGCTRL control = srt_msgctrl_default;
    control.msgno = 77;
    control.pktseq = 88;
    control.srctime = 99;
    REQUIRE_EQ(receive_message(
                   &fixture.socket, expired.data(), expired.size(), &control),
        SRT_ERROR);
    REQUIRE_EQ(last_error().code, SRT_EASYNCRCV);
    fixture.socket.public_options.receive_synchronous = true;
    REQUIRE_EQ(receive_message(
                   &fixture.socket, expired.data(), expired.size(), &control),
        SRT_ERROR);
    REQUIRE_EQ(last_error().code, SRT_ETIMEOUT);
    REQUIRE_EQ(control.msgno, 77);
    REQUIRE_EQ(control.pktseq, 88);
    REQUIRE_EQ(control.srctime, 99);

    std::array<std::byte, 1600> input;
    input.fill(std::byte {'c'});
    fixture.publish_message(input);
    input.fill(std::byte {'d'});
    REQUIRE(std::all_of(expired.begin(), expired.end(), [](char value) {
        return value == 'x';
    }));

    std::array<char, 1599> small;
    small.fill('y');
    REQUIRE_EQ(
        receive_message(&fixture.socket, small.data(), small.size(), &control),
        SRT_ERROR);
    REQUIRE_EQ(last_error().code, SRT_ELARGEMSG);
    REQUIRE_EQ(control.msgno, 77);
    REQUIRE_EQ(control.pktseq, 88);
    REQUIRE_EQ(control.srctime, 99);
    REQUIRE(std::all_of(small.begin(), small.end(), [](char value) {
        return value == 'y';
    }));
    REQUIRE_EQ(
        fixture.socket.runtime->buffer_packet_counts().available_receive, 2U);

    std::array<char, 1601> received;
    received.fill('z');
    REQUIRE_EQ(receive_message(
                   &fixture.socket, received.data(), received.size(), &control),
        1600);
    REQUIRE_EQ(control.msgno, 7);
    REQUIRE_EQ(control.pktseq,
        static_cast<int>(MessageAttemptFixture::first_sequence));
    REQUIRE_EQ(received.back(), 'z');
    REQUIRE(
        std::all_of(received.begin(), received.begin() + 1600, [](char value) {
            return value == 'c';
        }));
    REQUIRE_EQ(
        fixture.socket.runtime->buffer_packet_counts().available_receive, 0U);
    REQUIRE(std::all_of(expired.begin(), expired.end(), [](char value) {
        return value == 'x';
    }));
}

TEST(compat_message_attempt_peer_error_precedes_pressure_and_is_consumed_once)
{
    MessageAttemptFixture fixture;
    const std::array<char, 2400> input {};
    REQUIRE_EQ(
        send_message(&fixture.socket, input.data(), input.size(), nullptr),
        2400);
    fixture.publish_control(ControlType::peer_error);
    fixture.socket.public_options.send_synchronous = true;
    SRT_MSGCTRL control = srt_msgctrl_default;
    control.msgno = 77;
    REQUIRE_EQ(
        send_message(&fixture.socket, input.data(), 1, &control), SRT_ERROR);
    REQUIRE_EQ(last_error().code, SRT_EPEERERR);
    REQUIRE_EQ(control.msgno, 77);
    REQUIRE_EQ(
        send_message(&fixture.socket, input.data(), 1, &control), SRT_ERROR);
    REQUIRE_EQ(last_error().code, SRT_ETIMEOUT);
    fixture.socket.public_options.send_synchronous = false;
    REQUIRE_EQ(
        send_message(&fixture.socket, input.data(), 1, &control), SRT_ERROR);
    REQUIRE_EQ(last_error().code, SRT_EASYNCSND);
    REQUIRE_EQ(
        fixture.socket.runtime->buffer_packet_counts().unacknowledged_send, 3U);
}

TEST(compat_message_attempt_peer_shutdown_preserves_message_tail)
{
    MessageAttemptFixture fixture;
    std::array<std::byte, 1600> input;
    input.fill(std::byte {'t'});
    fixture.publish_message(input);
    fixture.publish_control(ControlType::shutdown);
    fixture.socket.state = SRTS_BROKEN;
    fixture.socket.public_options.receive_synchronous = true;
    std::array<char, 1600> output {};
    SRT_MSGCTRL control = srt_msgctrl_default;
    REQUIRE_EQ(receive_message(
                   &fixture.socket, output.data(), output.size(), &control),
        1600);
    REQUIRE_EQ(control.msgno, 7);
    REQUIRE(std::all_of(output.begin(), output.end(), [](char value) {
        return value == 't';
    }));
    REQUIRE_EQ(receive_message(
                   &fixture.socket, output.data(), output.size(), &control),
        SRT_ERROR);
    REQUIRE_EQ(last_error().code, SRT_ECONNLOST);
    REQUIRE_EQ(control.msgno, 7);
}
