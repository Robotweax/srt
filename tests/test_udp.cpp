#include "test.hpp"

#include "robotweax/srt/codec.hpp"
#include "robotweax/srt/handshake.hpp"
#include "robotweax/srt/handshake_extensions.hpp"
#include "robotweax/srt/udp.hpp"
#include "udp_buffer_policy.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <thread>

using namespace robotweax::srt;

TEST(udp_buffer_fallback_preserves_existing_buffer)
{
    int calls = 0;
    REQUIRE_EQ(detail::configure_udp_buffer(
                   12'288'000, true,
                   [&](std::int32_t) {
                       ++calls;
                       return ENOBUFS;
                   },
                   [](std::int32_t& value) {
                       value = 262'144;
                       return 0;
                   }),
        0);
    REQUIRE_EQ(calls, 1);
}

TEST(udp_buffer_fallback_retries_small_buffer)
{
    int calls = 0;
    REQUIRE_EQ(detail::configure_udp_buffer(
                   12'288'000, true,
                   [&](std::int32_t value) {
                       ++calls;
                       return value == 64'000 ? 0 : ENOBUFS;
                   },
                   [](std::int32_t& value) {
                       value = 8192;
                       return 0;
                   }),
        0);
    REQUIRE_EQ(calls, 2);
}

TEST(udp_buffer_fallback_does_not_hide_other_errors)
{
    for (const int error : {EBADF, EINVAL, EACCES}) {
        REQUIRE_EQ(detail::configure_udp_buffer(
                       262'144, true,
                       [=](std::int32_t) {
                           return error;
                       },
                       [](std::int32_t&) {
                           return 0;
                       }),
            error);
    }
    REQUIRE_EQ(detail::configure_udp_buffer(
                   262'144, false,
                   [](std::int32_t) {
                       return ENOBUFS;
                   },
                   [](std::int32_t&) {
                       return 0;
                   }),
        ENOBUFS);
    REQUIRE_EQ(detail::configure_udp_buffer(
                   262'144, true,
                   [](std::int32_t) {
                       return ENOBUFS;
                   },
                   [](std::int32_t&) {
                       return EBADF;
                   }),
        EBADF);
    REQUIRE_EQ(detail::configure_udp_buffer(
                   262'144, true,
                   [](std::int32_t) {
                       return ENOBUFS;
                   },
                   [](std::int32_t& value) {
                       value = 8192;
                       return 0;
                   }),
        ENOBUFS);
}

TEST(udp_buffer_fallback_records_actual_size_and_respects_small_requests)
{
    detail::collect_udp_buffer_notices = true;
    detail::udp_buffer_notice_count = 0;
    const int result = detail::configure_udp_buffer(
        12'288'000, true,
        [](std::int32_t) {
            return ENOBUFS;
        },
        [](std::int32_t& value) {
            value = 262'144;
            return 0;
        });
    detail::collect_udp_buffer_notices = false;
    REQUIRE_EQ(result, 0);
    REQUIRE_EQ(detail::udp_buffer_notice_count, 1U);
    REQUIRE_EQ(detail::udp_buffer_notices[0].requested, 12'288'000);
    REQUIRE_EQ(detail::udp_buffer_notices[0].effective, 262'144);
    detail::udp_buffer_notice_count = 0;
    REQUIRE_EQ(detail::configure_udp_buffer(
                   4096, true,
                   [](std::int32_t) {
                       return ENOBUFS;
                   },
                   [](std::int32_t& value) {
                       value = 8192;
                       return 0;
                   }),
        ENOBUFS);
}

namespace {

UdpIoResult receive_until_ready(UdpSocket& socket, std::span<std::byte> destination)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
    for (;;) {
        auto result = socket.receive_from(destination);
        if (result.error != Error::would_block) {
            return result;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return result;
        }
        std::this_thread::yield();
    }
}

} // namespace

TEST(udp_readiness_wait_is_bounded_and_validated)
{
    UdpSocket socket;
    REQUIRE(socket.valid());
    const UdpWaitResult immediate = socket.wait_readable(0);
    REQUIRE(immediate);
    REQUIRE(!immediate.ready);
    REQUIRE_EQ(socket.wait_readable(-2).error, Error::io_error);
}

TEST(udp_socket_accepts_native_buffer_configuration)
{
    UdpSocket socket;
    UdpSocket ipv6_socket{IpAddressFamily::ipv6};
    REQUIRE(socket.valid());
    REQUIRE(ipv6_socket.valid());
    REQUIRE_EQ(socket.set_send_buffer_size(65'536), Error::none);
    REQUIRE_EQ(socket.set_receive_buffer_size(262'144), Error::none);
    REQUIRE_EQ(socket.set_send_buffer_size(0), Error::invalid_state);
    REQUIRE_EQ(socket.set_receive_buffer_size(0), Error::invalid_state);
    REQUIRE_EQ(socket.set_ipv6_only(true), Error::invalid_state);
    REQUIRE_EQ(ipv6_socket.set_ipv6_only(true), Error::none);
    const UdpIntegerOptionResult enabled =
        ipv6_socket.ipv6_only();
    REQUIRE(enabled);
    REQUIRE_EQ(enabled.value, 1);
    REQUIRE_EQ(ipv6_socket.set_ipv6_only(false), Error::none);
    const UdpIntegerOptionResult disabled =
        ipv6_socket.ipv6_only();
    REQUIRE(disabled);
    REQUIRE_EQ(disabled.value, 0);
}

TEST(udp_socket_maps_ipv4_and_ipv6_traffic_class_options)
{
    UdpSocket ipv4;
    UdpSocket ipv6{IpAddressFamily::ipv6};
    REQUIRE(ipv4.valid());
    REQUIRE(ipv6.valid());

    REQUIRE_EQ(ipv4.set_ip_time_to_live(42), Error::none);
    REQUIRE_EQ(ipv4.set_ip_type_of_service(0x88), Error::none);
    REQUIRE_EQ(ipv6.set_ip_time_to_live(43), Error::none);
#if defined(_WIN32)
    REQUIRE_EQ(ipv6.set_ip_type_of_service(0x90), Error::unsupported);
#else
    REQUIRE_EQ(ipv6.set_ip_type_of_service(0x90), Error::none);
#endif
    REQUIRE_EQ(ipv4.ip_time_to_live().value, 42);
    REQUIRE_EQ(ipv4.ip_type_of_service().value, 0x88);
    REQUIRE_EQ(ipv6.ip_time_to_live().value, 43);
#if defined(_WIN32)
    REQUIRE_EQ(ipv6.ip_type_of_service().error, Error::unsupported);
#else
    REQUIRE_EQ(ipv6.ip_type_of_service().value, 0x90);
#endif

    REQUIRE_EQ(ipv4.set_ip_time_to_live(0), Error::invalid_state);
    REQUIRE_EQ(ipv4.set_ip_time_to_live(256), Error::invalid_state);
    REQUIRE_EQ(ipv4.set_ip_type_of_service(-1), Error::invalid_state);
    REQUIRE_EQ(ipv4.set_ip_type_of_service(256), Error::invalid_state);
}

TEST(udp_bind_to_device_has_an_explicit_platform_boundary)
{
    UdpSocket socket;
    REQUIRE(socket.valid());
#if defined(__linux__)
    const UdpDeviceOptionResult initial = socket.bound_device();
    REQUIRE(initial);
    REQUIRE(initial.view().empty());
    REQUIRE_EQ(socket.set_bind_to_device({}), Error::none);
#else
    REQUIRE_EQ(socket.set_bind_to_device({}), Error::unsupported);
    REQUIRE_EQ(socket.bound_device().error, Error::unsupported);
#endif
#if defined(__linux__)
    constexpr Error long_name_result = Error::invalid_state;
#else
    constexpr Error long_name_result = Error::unsupported;
#endif
    REQUIRE_EQ(socket.set_bind_to_device(
                   "network-device-name-is-too-long"),
        long_name_result);
}

TEST(ip_endpoint_distinguishes_ipv4_ipv6_and_scope)
{
    const IpEndpoint ipv4 = IpEndpoint::loopback(4'000);
    const IpEndpoint ipv6 = IpEndpoint::ipv6_loopback(4'000);
    const IpEndpoint scoped =
        IpEndpoint::ipv6_loopback(4'000, 7);

    REQUIRE(ipv4.is_ipv4());
    REQUIRE(!ipv4.is_ipv6());
    REQUIRE_EQ(ipv4.address_size(), 4U);
    REQUIRE(ipv6.is_ipv6());
    REQUIRE_EQ(ipv6.address_size(), 16U);
    REQUIRE(!ipv4.is_wildcard());
    REQUIRE(!ipv6.is_wildcard());
    REQUIRE(IpEndpoint::any().is_wildcard());
    REQUIRE(IpEndpoint::ipv6_any().is_wildcard());
    REQUIRE(ipv4 != ipv6);
    REQUIRE(ipv6 != scoped);

    IpEndpoint mapped = IpEndpoint::ipv6_any(4'000);
    mapped.address[10] = 0xffU;
    mapped.address[11] = 0xffU;
    mapped.address[12] = 127U;
    mapped.address[15] = 1U;
    REQUIRE(mapped.is_ipv4_mapped_ipv6());
    REQUIRE_EQ(mapped.wire_family(), IpAddressFamily::ipv4);
    REQUIRE(!ipv6.is_ipv4_mapped_ipv6());
    REQUIRE_EQ(ipv6.wire_family(), IpAddressFamily::ipv6);
    REQUIRE_EQ(ipv4.wire_family(), IpAddressFamily::ipv4);
}

TEST(nonblocking_udp_reports_would_block)
{
    UdpSocket socket;
    REQUIRE(socket.valid());
    REQUIRE_EQ(socket.bind(Ipv4Endpoint::loopback()), Error::none);
    std::array<std::byte, 16> bytes{};
    REQUIRE_EQ(socket.receive_from(bytes).error, Error::would_block);
}

TEST(ipv6_udp_round_trips_peer_and_scope_aware_endpoints)
{
    UdpSocket sender{IpAddressFamily::ipv6};
    UdpSocket receiver{IpAddressFamily::ipv6};
    REQUIRE(sender.valid());
    REQUIRE(receiver.valid());
    REQUIRE_EQ(sender.bind(IpEndpoint::ipv6_loopback()), Error::none);
    REQUIRE_EQ(receiver.bind(IpEndpoint::ipv6_loopback()), Error::none);

    const EndpointResult sender_address = sender.local_endpoint();
    const EndpointResult receiver_address = receiver.local_endpoint();
    REQUIRE(sender_address);
    REQUIRE(receiver_address);
    REQUIRE(sender_address.endpoint.is_ipv6());
    REQUIRE(receiver_address.endpoint.is_ipv6());

    const std::array payload{
        std::byte{0x52}, std::byte{0x57}, std::byte{0x58}};
    const UdpIoResult sent =
        sender.send_to(payload, receiver_address.endpoint);
    REQUIRE(sent);
    REQUIRE_EQ(sent.bytes_transferred, payload.size());

    std::array<std::byte, 16> received_bytes{};
    const UdpIoResult received =
        receive_until_ready(receiver, received_bytes);
    REQUIRE(received);
    REQUIRE_EQ(received.bytes_transferred, payload.size());
    REQUIRE_EQ(received.peer, sender_address.endpoint);
    REQUIRE(std::equal(payload.begin(), payload.end(),
        received_bytes.begin()));

    REQUIRE_EQ(
        sender.send_to(payload, IpEndpoint::loopback(9'000)).error,
        Error::invalid_state);
}

TEST(hsv5_extension_round_trips_over_loopback_udp)
{
    UdpSocket sender;
    UdpSocket receiver;
    REQUIRE(sender.valid());
    REQUIRE(receiver.valid());
    REQUIRE_EQ(sender.bind(Ipv4Endpoint::loopback()), Error::none);
    REQUIRE_EQ(receiver.bind(Ipv4Endpoint::loopback()), Error::none);
    const auto sender_address = sender.local_endpoint();
    const auto receiver_address = receiver.local_endpoint();
    REQUIRE(sender_address);
    REQUIRE(receiver_address);

    std::array<std::byte, 16> extension{};
    HandshakeExtensionParameters parameters;
    parameters.flags |= static_cast<std::uint32_t>(HandshakeExtensionFlag::tsbpd_send)
        | static_cast<std::uint32_t>(HandshakeExtensionFlag::tsbpd_receive);
    const auto extension_encoded = encode_handshake_parameters(
        HandshakeExtensionType::handshake_request, parameters, extension);
    REQUIRE(extension_encoded);

    Handshake handshake;
    handshake.version = handshake_version_5;
    handshake.extension_field = 1;
    handshake.request = HandshakeRequest::conclusion;
    handshake.socket_id = 100;
    handshake.syn_cookie = 0x1020'3040U;

    std::array<std::byte, 64> handshake_payload{};
    const auto handshake_encoded = encode_handshake_payload(
        handshake, extension, handshake_payload);
    REQUIRE(handshake_encoded);

    MutablePacketView packet;
    packet.kind = PacketKind::control;
    packet.control.type = ControlType::handshake;
    packet.control.destination_socket_id = 200;
    packet.payload = std::span{handshake_payload}.first(handshake_encoded.bytes_written);
    std::array<std::byte, 80> datagram{};
    const auto packet_encoded = encode_packet(packet, datagram);
    REQUIRE(packet_encoded);

    const auto sent = sender.send_to(
        std::span{datagram}.first(packet_encoded.bytes_written), receiver_address.endpoint);
    REQUIRE(sent);
    REQUIRE_EQ(sent.bytes_transferred, packet_encoded.bytes_written);

    std::array<std::byte, 1500> received_bytes{};
    const auto received = receive_until_ready(receiver, received_bytes);
    REQUIRE(received);
    REQUIRE_EQ(received.peer, sender_address.endpoint);

    const auto decoded_packet = decode_packet(
        std::span{received_bytes}.first(received.bytes_transferred));
    REQUIRE(decoded_packet);
    REQUIRE_EQ(decoded_packet.packet.control.type, ControlType::handshake);
    REQUIRE_EQ(decoded_packet.packet.control.destination_socket_id, 200U);
    const auto decoded_handshake = decode_handshake_payload(decoded_packet.packet.payload);
    REQUIRE(decoded_handshake);
    REQUIRE_EQ(decoded_handshake.handshake.request, HandshakeRequest::conclusion);
    const auto decoded_extension = decode_extension(decoded_handshake.extensions);
    REQUIRE(decoded_extension);
    const auto decoded_parameters = decode_handshake_parameters(decoded_extension.extension);
    REQUIRE(decoded_parameters);
    REQUIRE_EQ(decoded_parameters.parameters.srt_version, parameters.srt_version);
}

namespace {

void require_oversized_datagram_is_discarded(IpAddressFamily family)
{
    UdpSocket sender {family};
    UdpSocket receiver {family};
    REQUIRE(sender.valid());
    REQUIRE(receiver.valid());
    const IpEndpoint bind_address = family == IpAddressFamily::ipv4
        ? IpEndpoint::loopback()
        : IpEndpoint::ipv6_loopback();
    REQUIRE_EQ(sender.bind(bind_address), Error::none);
    REQUIRE_EQ(receiver.bind(bind_address), Error::none);
    const auto sender_address = sender.local_endpoint();
    const auto receiver_address = receiver.local_endpoint();
    REQUIRE(sender_address);
    REQUIRE(receiver_address);

    std::array<std::byte, 1'500> receive_buffer {};
    std::array<std::byte, 1'500> boundary_payload {};
    boundary_payload.front() = std::byte {0x31};
    boundary_payload.back() = std::byte {0x32};
    REQUIRE(sender.send_to(boundary_payload, receiver_address.endpoint));
    const UdpIoResult boundary = receive_until_ready(receiver, receive_buffer);
    REQUIRE(boundary);
    REQUIRE_EQ(boundary.bytes_transferred, boundary_payload.size());
    REQUIRE_EQ(boundary.peer, sender_address.endpoint);
    REQUIRE_EQ(receive_buffer.front(), boundary_payload.front());
    REQUIRE_EQ(receive_buffer.back(), boundary_payload.back());

    std::array<std::byte, 1'501> oversized_payload {};
    const UdpIoResult sent_oversized =
        sender.send_to(oversized_payload, receiver_address.endpoint);
    REQUIRE(sent_oversized);
    REQUIRE_EQ(sent_oversized.bytes_transferred, oversized_payload.size());

    const UdpIoResult oversized = receive_until_ready(receiver, receive_buffer);
    REQUIRE_EQ(oversized.error, Error::buffer_too_small);
    REQUIRE_EQ(oversized.bytes_transferred, receive_buffer.size());
    REQUIRE_EQ(oversized.peer, sender_address.endpoint);
    REQUIRE(oversized.system_error != 0);

    const std::array valid_payload {
        std::byte {0x11}, std::byte {0x22}, std::byte {0x33}};
    REQUIRE(sender.send_to(valid_payload, receiver_address.endpoint));
    const UdpIoResult valid = receive_until_ready(receiver, receive_buffer);
    REQUIRE(valid);
    REQUIRE_EQ(valid.bytes_transferred, valid_payload.size());
    REQUIRE_EQ(valid.peer, sender_address.endpoint);
    REQUIRE(std::equal(
        valid_payload.begin(), valid_payload.end(), receive_buffer.begin()));
}

} // namespace

TEST(udp_reports_and_discards_oversized_ipv4_datagrams)
{
    require_oversized_datagram_is_discarded(IpAddressFamily::ipv4);
}

TEST(udp_reports_and_discards_oversized_ipv6_datagrams)
{
    require_oversized_datagram_is_discarded(IpAddressFamily::ipv6);
}
