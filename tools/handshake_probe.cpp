#include "robotweax/srt/codec.hpp"
#include "robotweax/srt/handshake.hpp"
#include "robotweax/srt/handshake_datagram.hpp"
#include "robotweax/srt/session.hpp"
#include "robotweax/srt/udp.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <span>
#include <string_view>
#include <thread>

namespace {

using namespace robotweax::srt;
using Clock = std::chrono::steady_clock;

enum class ProbeOperation : std::uint8_t {
    handshake_only,
    send,
    send_after_drop,
    send_rate,
    send_dynamic_rate,
    receive,
};

struct ProbeRequest {
    ProbeOperation operation = ProbeOperation::handshake_only;
    std::string_view payload{};
    bool drop_second_initial_packet = false;
    std::size_t expected_receive_bytes = 0;
    std::uint16_t receiver_latency_milliseconds = 120;
    std::size_t generated_send_bytes = 0;
    std::uint64_t input_bandwidth_bytes_per_second = 100'000'000;
    std::uint64_t second_input_bandwidth_bytes_per_second = 0;
    std::string_view expected_token{};
    std::int32_t clock_skew_parts_per_million = 0;
};

std::uint32_t probe_cookie(const Handshake& packet, void* context) noexcept
{
    const auto secret = *static_cast<const std::uint32_t*>(context);
    std::uint32_t value = packet.socket_id ^ packet.initial_sequence.value() ^ secret;
    value ^= value >> 16U;
    value *= 0x7feb'352dU;
    value ^= value >> 15U;
    return value == 0U ? 1U : value;
}

bool parse_port(std::string_view text, std::uint16_t& port)
{
    unsigned value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()
        || value == 0U || value > 65'535U) {
        return false;
    }
    port = static_cast<std::uint16_t>(value);
    return true;
}

bool send_action(UdpSocket& socket, Ipv4Endpoint peer,
    const HandshakeAction& action, std::uint32_t peer_socket_id,
    Clock::time_point origin)
{
    std::array<std::byte, 1500> datagram{};
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        Clock::now() - origin).count();
    const auto encoded = encode_handshake_datagram(action,
        PacketTimestamp{static_cast<std::uint32_t>(elapsed)},
        peer_socket_id, datagram);
    if (!encoded) {
        std::cerr << "encode failed: " << describe(encoded.error) << '\n';
        return false;
    }
    const auto sent = socket.send_to(
        std::span{datagram}.first(encoded.bytes_written), peer);
    if (!sent) {
        std::cerr << "send failed: " << describe(sent.error)
                  << " (system error " << sent.system_error << ")\n";
        return false;
    }
    return true;
}

PacketTimestamp timestamp_since(Clock::time_point origin)
{
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        Clock::now() - origin).count();
    return PacketTimestamp{static_cast<std::uint32_t>(elapsed)};
}

std::uint64_t session_time_since(Clock::time_point origin,
    std::int32_t skew_parts_per_million)
{
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        Clock::now() - origin).count();
    const std::int64_t scale = 1'000'000LL + skew_parts_per_million;
    return static_cast<std::uint64_t>(elapsed * scale / 1'000'000LL);
}

bool send_reliability_action(UdpSocket& socket, Ipv4Endpoint peer,
    const ReliabilityAction& action, std::uint32_t peer_socket_id,
    Clock::time_point origin)
{
    std::array<std::byte, 1500> datagram{};
    const auto encoded = encode_reliability_action(action,
        timestamp_since(origin), peer_socket_id, datagram);
    if (!encoded) {
        std::cerr << "control encode failed: " << describe(encoded.error) << '\n';
        return false;
    }
    return static_cast<bool>(socket.send_to(
        std::span{datagram}.first(encoded.bytes_written), peer));
}

int run_data_probe(UdpSocket& socket, Ipv4Endpoint peer,
    std::uint32_t peer_socket_id, SequenceNumber connection_sequence,
    Clock::time_point origin, ProbeRequest request,
    const NegotiatedLiveOptions& live_options,
    PacketTimestamp peer_handshake_timestamp,
    std::uint64_t handshake_arrival_microseconds)
{
    ReliabilitySession session{{
        .local_initial_sequence = connection_sequence,
        .peer_initial_sequence = connection_sequence,
        .peer_socket_id = peer_socket_id,
        .send_capacity_packets = 256,
        .receive_capacity_packets = 256,
    }};
    session.configure_live(live_options, handshake_arrival_microseconds,
        peer_handshake_timestamp, {
            .input_bandwidth_bytes_per_second =
                request.input_bandwidth_bytes_per_second,
        });
    PacketPacer pacer{100'000'000, 256};
    bool replacement_queued = request.operation != ProbeOperation::send_after_drop;
    std::size_t paced_payload_bytes = 0;
    bool dynamic_rate_applied = false;
    bool intentionally_dropped = false;
    std::size_t initial_packet_index = 0;
    const auto send_pending_data = [&]() -> bool {
        for (;;) {
            const auto now = session_time_since(origin,
                request.clock_skew_parts_per_million);
            const auto outbound = session.next_paced_data_packet(pacer, now);
            if (!outbound.has_value()) {
                break;
            }
            const bool drop = request.drop_second_initial_packet
                && !intentionally_dropped
                && !outbound->header.retransmitted
                && initial_packet_index == 1U;
            ++initial_packet_index;
            if (drop) {
                intentionally_dropped = true;
                continue;
            }
            MutablePacketView packet;
            packet.kind = PacketKind::data;
            packet.data = outbound->header;
            packet.payload = outbound->payload;
            std::array<std::byte, 1500> outgoing{};
            const auto encoded = encode_packet(packet, outgoing);
            if (!encoded || !socket.send_to(
                    std::span{outgoing}.first(encoded.bytes_written), peer)) {
                return false;
            }
            session.note_packet_sent(session_time_since(origin,
                request.clock_skew_parts_per_million));
            paced_payload_bytes += outbound->payload.size();
            if (request.operation == ProbeOperation::send_dynamic_rate
                && !dynamic_rate_applied
                && paced_payload_bytes >= request.generated_send_bytes / 2U) {
                SocketOptions dynamic_options;
                if (dynamic_options.set(SocketOption::input_bandwidth_bytes_per_second,
                        static_cast<std::int64_t>(
                            request.second_input_bandwidth_bytes_per_second))
                        != Error::none
                    || dynamic_options.set(
                           SocketOption::maximum_bandwidth_bytes_per_second,
                           0)
                        != Error::none
                    || session.apply_dynamic_options(dynamic_options) != Error::none) {
                    return false;
                }
                dynamic_rate_applied = true;
            }
        }
        return true;
    };
    if (request.operation == ProbeOperation::send_rate
        || request.operation == ProbeOperation::send_dynamic_rate) {
        constexpr std::size_t chunk_size = 1'000;
        const std::array<std::byte, chunk_size> chunk{};
        std::size_t queued = 0;
        while (queued < request.generated_send_bytes) {
            const auto size = std::min(chunk_size,
                request.generated_send_bytes - queued);
            const auto now = session_time_since(origin,
                request.clock_skew_parts_per_million);
            if (session.queue_message(std::span{chunk}.first(size),
                    timestamp_since(origin), true, now) != Error::none) {
                std::cerr << "failed to queue rate-test data\n";
                return 5;
            }
            queued += size;
        }
        if (!send_pending_data()) {
            return 5;
        }
    } else if (request.operation == ProbeOperation::send
        || request.operation == ProbeOperation::send_after_drop) {
        const auto payload = std::as_bytes(std::span{
            request.payload.data(), request.payload.size()});
        const auto queued_at = session_time_since(origin,
            request.clock_skew_parts_per_million);
        const std::array<std::byte, 1> expired_payload{std::byte{'x'}};
        const auto queued_payload = request.operation == ProbeOperation::send_after_drop
            ? std::span<const std::byte>{expired_payload}
            : payload;
        const auto enqueue_time = request.operation == ProbeOperation::send_after_drop
            ? 1U : queued_at;
        if (session.queue_message(queued_payload, timestamp_since(origin), true,
                enqueue_time) != Error::none) {
            std::cerr << "failed to queue message\n";
            return 5;
        }
        if (replacement_queued && !send_pending_data()) {
            std::cerr << "failed to send data packet\n";
            return 5;
        }
    }

    std::array<std::byte, 65'536> message{};
    std::array<std::byte, 65'536> aggregate{};
    std::size_t aggregate_size = 0;
    std::optional<std::uint64_t> first_data_arrival_microseconds;
    std::optional<Clock::time_point> all_acknowledged_at;
    const auto deliver_ready = [&](std::uint64_t now_microseconds) -> int {
        for (;;) {
            const auto delivered = session.pop_message_at(message, now_microseconds);
            if (!delivered) {
                return delivered.error == Error::would_block ? 0 : -1;
            }
            if (!request.expected_token.empty()) {
                const auto expected = std::as_bytes(std::span{
                    request.expected_token.data(), request.expected_token.size()});
                const auto actual = std::span{message}.first(delivered.bytes_written);
                if (actual.size() != expected.size()
                    || !std::equal(actual.begin(), actual.end(), expected.begin())) {
                    continue;
                }
            }
            if (aggregate.size() - aggregate_size < delivered.bytes_written) {
                return -1;
            }
            std::copy_n(message.begin(), delivered.bytes_written,
                aggregate.begin() + static_cast<std::ptrdiff_t>(aggregate_size));
            aggregate_size += delivered.bytes_written;
            if (request.expected_receive_bytes == 0U
                || aggregate_size >= request.expected_receive_bytes) {
                std::cout << "RECEIVED ";
                std::cout.write(reinterpret_cast<const char*>(aggregate.data()),
                    static_cast<std::streamsize>(aggregate_size));
                if (first_data_arrival_microseconds.has_value()) {
                    std::cout << " GATE_US="
                              << now_microseconds - *first_data_arrival_microseconds;
                }
                std::cout << " DRIFT_CORRECTIONS="
                          << session.drift_correction_count()
                          << " DRIFT_TOTAL_US="
                          << session.total_drift_correction_microseconds();
                std::cout << '\n';
                return 1;
            }
        }
    };
    std::array<std::byte, 1500> datagram{};
    const auto deadline = Clock::now() + std::chrono::seconds{10};
    while (Clock::now() < deadline) {
        const auto now_microseconds = session_time_since(origin,
            request.clock_skew_parts_per_million);
        const auto received = socket.receive_from(datagram);
        if (received.error == Error::would_block) {
            const auto due = session.poll_timers(now_microseconds);
            for (std::size_t index = 0; index < due.size; ++index) {
                if (!send_reliability_action(socket, peer, due.values[index],
                        peer_socket_id, origin)) {
                    return 5;
                }
                session.note_packet_sent(now_microseconds);
            }
            const auto dropped = session.drop_too_late_sender(now_microseconds);
            for (std::size_t index = 0; index < dropped.size; ++index) {
                if (!send_reliability_action(socket, peer, dropped.values[index],
                        peer_socket_id, origin)) {
                    return 5;
                }
            }
            if (!replacement_queued && dropped.size != 0U) {
                const auto payload = std::as_bytes(std::span{
                    request.payload.data(), request.payload.size()});
                if (session.queue_message(payload, timestamp_since(origin), true,
                        now_microseconds) != Error::none) {
                    return 5;
                }
                replacement_queued = true;
            }
            if (replacement_queued && !send_pending_data()) {
                return 5;
            }
            if (all_acknowledged_at.has_value()
                && Clock::now() - *all_acknowledged_at
                    >= std::chrono::milliseconds{300}) {
                std::cout << "DELIVERED_ACKED\n";
                return 0;
            }
            if (request.operation == ProbeOperation::receive) {
                const int delivery = deliver_ready(now_microseconds);
                if (delivery > 0) {
                    return 0;
                }
                if (delivery < 0) {
                    return 5;
                }
            }
            std::this_thread::yield();
            continue;
        }
        if (!received) {
            std::cerr << "data receive failed\n";
            return 5;
        }
        const auto decoded = decode_packet(
            std::span{datagram}.first(received.bytes_transferred));
        if (!decoded) {
            continue;
        }
        if (decoded.packet.kind == PacketKind::control
            && decoded.packet.control.type == ControlType::handshake) {
            continue;
        }
        if (decoded.packet.kind == PacketKind::control
            && decoded.packet.control.type == ControlType::shutdown
            && request.operation == ProbeOperation::send_after_drop
            && replacement_queued) {
            std::cout << "DROPREQ_ACCEPTED\n";
            return 0;
        }
        if (decoded.packet.kind == PacketKind::control
            && decoded.packet.control.type == ControlType::drop_request) {
            const auto drop = decode_drop_request(decoded.packet);
            if (drop) {
                std::cout << "DROPREQ_RX="
                          << drop.request.sequences.first.value() << '-'
                          << drop.request.sequences.last.value() << '\n';
            }
        }
        if (decoded.packet.kind == PacketKind::data
            && !first_data_arrival_microseconds.has_value()) {
            first_data_arrival_microseconds = now_microseconds;
        }
        const auto processed = session.receive(decoded.packet, now_microseconds);
        if (!processed) {
            std::cerr << "reliability processing failed: "
                      << describe(processed.error);
            if (decoded.packet.kind == PacketKind::control) {
                std::cerr << " control_type="
                          << static_cast<std::uint16_t>(decoded.packet.control.type)
                          << " payload_bytes=" << decoded.packet.payload.size();
            }
            std::cerr << '\n';
            return 5;
        }
        for (std::size_t index = 0; index < processed.actions.size; ++index) {
            if (!send_reliability_action(socket, peer,
                    processed.actions.values[index], peer_socket_id, origin)) {
                return 5;
            }
            session.note_packet_sent(now_microseconds);
        }
        if (replacement_queued && !send_pending_data()) {
            std::cerr << "failed to send retransmission\n";
            return 5;
        }
        if ((request.operation == ProbeOperation::send
                || request.operation == ProbeOperation::send_rate
                || request.operation == ProbeOperation::send_dynamic_rate
                || request.operation == ProbeOperation::send_after_drop)
            && session.send_buffer().size() == 0U) {
            if (request.operation != ProbeOperation::send_rate
                && request.operation != ProbeOperation::send_dynamic_rate) {
                std::cout << "DELIVERED_ACKED\n";
                return 0;
            }
            if (!all_acknowledged_at.has_value()) {
                all_acknowledged_at = Clock::now();
            } else if (Clock::now() - *all_acknowledged_at
                >= std::chrono::milliseconds{300}) {
                std::cout << "DELIVERED_ACKED\n";
                return 0;
            }
        }
        if (request.operation == ProbeOperation::receive) {
            const int delivery = deliver_ready(now_microseconds);
            if (delivery > 0) {
                return 0;
            }
            if (delivery < 0) {
                std::cerr << "message delivery failed\n";
                return 5;
            }
        }
    }
    std::cerr << "data operation timed out\n";
    return 5;
}

int run(ConnectionRole role, std::uint16_t port, ProbeRequest request)
{
    constexpr std::uint32_t local_socket_id = 0x1357'2468U;
    constexpr SequenceNumber local_initial_sequence{0x0102'0304U};
    std::uint32_t cookie_secret = 0x829a'741dU;
    HandshakeExtensionParameters local_extension_parameters;
    local_extension_parameters.receiver_tsbpd_delay_milliseconds =
        request.receiver_latency_milliseconds;
    HandshakeMachine machine{{
        .role = role,
        .local_socket_id = local_socket_id,
        .initial_sequence = local_initial_sequence,
        .timeout_milliseconds = 250,
        .extension_parameters = local_extension_parameters,
        .cookie_generator = role == ConnectionRole::listener ? probe_cookie : nullptr,
        .cookie_context = role == ConnectionRole::listener ? &cookie_secret : nullptr,
    }};
    UdpSocket socket;
    if (!socket.valid()) {
        std::cerr << "socket creation failed: " << socket.open_system_error() << '\n';
        return 2;
    }
    const auto bind_address = role == ConnectionRole::listener
        ? Ipv4Endpoint::loopback(port)
        : Ipv4Endpoint::loopback();
    if (socket.bind(bind_address) != Error::none) {
        std::cerr << "bind failed\n";
        return 2;
    }

    Ipv4Endpoint peer = Ipv4Endpoint::loopback(port);
    std::uint32_t peer_socket_id = 0;
    SequenceNumber connection_sequence = local_initial_sequence;
    const auto origin = Clock::now();
    auto retry_deadline = origin + std::chrono::milliseconds{250};
    if (role == ConnectionRole::caller) {
        const auto actions = machine.start();
        if (!send_action(socket, peer, actions.values[0], 0, origin)) {
            return 2;
        }
    }

    std::array<std::byte, 1500> incoming{};
    const auto deadline = origin + std::chrono::seconds{5};
    while (Clock::now() < deadline) {
        const auto received = socket.receive_from(incoming);
        if (received.error == Error::would_block) {
            if (Clock::now() >= retry_deadline
                && (role == ConnectionRole::caller || peer_socket_id != 0U)) {
                const auto retries = machine.timeout();
                for (std::size_t index = 0; index < retries.size; ++index) {
                    const auto& action = retries.values[index];
                    if (action.kind == HandshakeActionKind::send) {
                        const std::uint32_t destination_socket_id =
                            role == ConnectionRole::caller ? 0U : peer_socket_id;
                        if (!send_action(socket, peer, action,
                                destination_socket_id, origin)) {
                            return 2;
                        }
                    } else if (action.kind == HandshakeActionKind::arm_timer) {
                        retry_deadline = Clock::now()
                            + std::chrono::milliseconds{action.timeout_milliseconds};
                    } else if (action.kind == HandshakeActionKind::failed) {
                        std::cerr << "handshake retry budget exhausted\n";
                        return 4;
                    }
                }
            }
            std::this_thread::yield();
            continue;
        }
        if (!received) {
            std::cerr << "receive failed: " << describe(received.error) << '\n';
            return 2;
        }
        const auto decoded = decode_handshake_datagram(
            std::span{incoming}.first(received.bytes_transferred));
        if (!decoded) {
            std::cerr << "decode failed: " << describe(decoded.error) << '\n';
            continue;
        }
        peer = received.peer;
        peer_socket_id = decoded.message.packet.socket_id;
        if (role == ConnectionRole::listener
            && decoded.message.packet.request == HandshakeRequest::induction) {
            connection_sequence = decoded.message.packet.initial_sequence;
        }
        const auto actions = machine.receive(decoded.message);
        for (std::size_t index = 0; index < actions.size; ++index) {
            const auto& action = actions.values[index];
            const std::uint32_t destination_socket_id =
                role == ConnectionRole::caller ? 0U : peer_socket_id;
            if (action.kind == HandshakeActionKind::send
                && !send_action(socket, peer, action, destination_socket_id, origin)) {
                return 2;
            }
            if (action.kind == HandshakeActionKind::arm_timer) {
                retry_deadline = Clock::now()
                    + std::chrono::milliseconds{action.timeout_milliseconds};
            }
            if (action.kind == HandshakeActionKind::connected) {
                std::cout << "CONNECTED peer_socket_id=" << peer_socket_id << '\n';
                if (request.operation == ProbeOperation::handshake_only) {
                    return 0;
                }
                if (!machine.has_peer_extension_parameters()) {
                    std::cerr << "peer omitted required HSv5 parameters\n";
                    return 3;
                }
                const auto live_options = negotiate_live_options(
                    local_extension_parameters,
                    machine.peer_extension_parameters());
                const auto handshake_arrival = session_time_since(origin,
                    request.clock_skew_parts_per_million);
                return run_data_probe(socket, peer, peer_socket_id,
                    connection_sequence, origin, request, live_options,
                    decoded.control.timestamp, handshake_arrival);
            }
            if (action.kind == HandshakeActionKind::rejected
                || action.kind == HandshakeActionKind::failed) {
                std::cerr << "handshake rejected or failed\n";
                return 3;
            }
        }
    }
    std::cerr << "handshake timed out\n";
    return 4;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3 || argc > 4) {
        std::cerr << "usage: robotweax_srt_handshake_probe caller|listener PORT "
                     "[send=TEXT|send-loss=TEXT|send-drop=TEXT|send-rate=BYTES,RATE|send-dynamic-rate=BYTES,RATE1,RATE2|receive|receive-bytes=N|receive-token=TEXT|receive-tsbpd=MS|receive-skew=PPM,BYTES]\n";
        return 1;
    }
    std::uint16_t port = 0;
    if (!parse_port(argv[2], port)) {
        std::cerr << "invalid UDP port\n";
        return 1;
    }
    const std::string_view mode{argv[1]};
    ProbeRequest request;
    if (argc == 4) {
        const std::string_view operation{argv[3]};
        if (operation == "receive") {
            request.operation = ProbeOperation::receive;
        } else if (operation.starts_with("receive-token=")) {
            request.operation = ProbeOperation::receive;
            request.expected_token = operation.substr(14);
        } else if (operation.starts_with("receive-tsbpd=")) {
            request.operation = ProbeOperation::receive;
            unsigned latency = 0;
            const auto number = operation.substr(14);
            const auto parsed = std::from_chars(
                number.data(), number.data() + number.size(), latency);
            if (parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size()
                || latency > 65'535U) {
                std::cerr << "invalid receiver latency\n";
                return 1;
            }
            request.receiver_latency_milliseconds =
                static_cast<std::uint16_t>(latency);
        } else if (operation.starts_with("receive-skew=")) {
            request.operation = ProbeOperation::receive;
            const auto values = operation.substr(13);
            const auto comma = values.find(',');
            if (comma == std::string_view::npos) {
                return 1;
            }
            int skew = 0;
            unsigned bytes = 0;
            const auto skew_text = values.substr(0, comma);
            const auto byte_text = values.substr(comma + 1U);
            const auto skew_parsed = std::from_chars(skew_text.data(),
                skew_text.data() + skew_text.size(), skew);
            const auto bytes_parsed = std::from_chars(byte_text.data(),
                byte_text.data() + byte_text.size(), bytes);
            if (skew_parsed.ec != std::errc{}
                || skew_parsed.ptr != skew_text.data() + skew_text.size()
                || bytes_parsed.ec != std::errc{}
                || bytes_parsed.ptr != byte_text.data() + byte_text.size()
                || skew < -500'000 || skew > 500'000 || bytes == 0U) {
                return 1;
            }
            request.clock_skew_parts_per_million = skew;
            request.expected_receive_bytes = bytes;
        } else if (operation.starts_with("receive-bytes=")) {
            request.operation = ProbeOperation::receive;
            unsigned expected = 0;
            const auto number = operation.substr(14);
            const auto parsed = std::from_chars(
                number.data(), number.data() + number.size(), expected);
            if (parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size()) {
                std::cerr << "invalid receive byte count\n";
                return 1;
            }
            request.expected_receive_bytes = expected;
        } else if (operation.starts_with("send=")) {
            request.operation = ProbeOperation::send;
            request.payload = operation.substr(5);
        } else if (operation.starts_with("send-loss=")) {
            request.operation = ProbeOperation::send;
            request.payload = operation.substr(10);
            request.drop_second_initial_packet = true;
        } else if (operation.starts_with("send-drop=")) {
            request.operation = ProbeOperation::send_after_drop;
            request.payload = operation.substr(10);
        } else if (operation.starts_with("send-rate=")) {
            request.operation = ProbeOperation::send_rate;
            const auto values = operation.substr(10);
            const auto comma = values.find(',');
            if (comma == std::string_view::npos) {
                return 1;
            }
            unsigned bytes = 0;
            unsigned long long rate = 0;
            const auto byte_text = values.substr(0, comma);
            const auto rate_text = values.substr(comma + 1);
            const auto bytes_parsed = std::from_chars(byte_text.data(),
                byte_text.data() + byte_text.size(), bytes);
            const auto rate_parsed = std::from_chars(rate_text.data(),
                rate_text.data() + rate_text.size(), rate);
            if (bytes_parsed.ec != std::errc{}
                || bytes_parsed.ptr != byte_text.data() + byte_text.size()
                || rate_parsed.ec != std::errc{}
                || rate_parsed.ptr != rate_text.data() + rate_text.size()
                || bytes == 0U || bytes > 250'000U || rate == 0U) {
                return 1;
            }
            request.generated_send_bytes = bytes;
            request.input_bandwidth_bytes_per_second = rate;
        } else if (operation.starts_with("send-dynamic-rate=")) {
            request.operation = ProbeOperation::send_dynamic_rate;
            const auto values = operation.substr(18);
            const auto first_comma = values.find(',');
            const auto second_comma = first_comma == std::string_view::npos
                ? std::string_view::npos : values.find(',', first_comma + 1U);
            if (first_comma == std::string_view::npos
                || second_comma == std::string_view::npos) {
                return 1;
            }
            unsigned bytes = 0;
            unsigned long long first_rate = 0;
            unsigned long long second_rate = 0;
            const auto byte_text = values.substr(0, first_comma);
            const auto first_text = values.substr(
                first_comma + 1U, second_comma - first_comma - 1U);
            const auto second_text = values.substr(second_comma + 1U);
            const auto bytes_parsed = std::from_chars(byte_text.data(),
                byte_text.data() + byte_text.size(), bytes);
            const auto first_parsed = std::from_chars(first_text.data(),
                first_text.data() + first_text.size(), first_rate);
            const auto second_parsed = std::from_chars(second_text.data(),
                second_text.data() + second_text.size(), second_rate);
            if (bytes_parsed.ec != std::errc{}
                || bytes_parsed.ptr != byte_text.data() + byte_text.size()
                || first_parsed.ec != std::errc{}
                || first_parsed.ptr != first_text.data() + first_text.size()
                || second_parsed.ec != std::errc{}
                || second_parsed.ptr != second_text.data() + second_text.size()
                || bytes == 0U || bytes > 250'000U
                || first_rate == 0U || second_rate == 0U) {
                return 1;
            }
            request.generated_send_bytes = bytes;
            request.input_bandwidth_bytes_per_second = first_rate;
            request.second_input_bandwidth_bytes_per_second = second_rate;
        } else {
            std::cerr << "unsupported probe operation\n";
            return 1;
        }
    }
    if (mode == "caller") {
        return run(robotweax::srt::ConnectionRole::caller, port, request);
    }
    if (mode == "listener") {
        return run(robotweax::srt::ConnectionRole::listener, port, request);
    }
    std::cerr << "mode must be caller or listener\n";
    return 1;
}
