#include "test.hpp"

#include "robotweax/srt/codec.hpp"
#include "robotweax/srt/handshake.hpp"
#include "robotweax/srt/handshake_extensions.hpp"
#include "robotweax/srt/udp.hpp"
#include "udp_buffer_policy.hpp"
#include "srt.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cerrno>
#include <limits>
#include <vector>
#include <cstddef>
#include <thread>

using namespace robotweax::srt;

namespace {

struct BufferNotices {
    BufferNotices()
    {
        detail::collect_udp_buffer_notices = true;
        detail::udp_buffer_notice_count = 0;
    }
    ~BufferNotices()
    {
        detail::collect_udp_buffer_notices = false;
        detail::udp_buffer_notice_count = 0;
    }
};

struct BufferModel {
    std::int32_t current = 8192;
    std::int32_t maximum = 1'048'576;
    bool clamp = false;
    detail::UdpBufferPolicy policy {false, ENOBUFS, EINVAL};
    std::vector<std::int32_t> requests;
    unsigned reads = 0;
    int set_error = 0;
    int read_error = 0;

    int set(std::int32_t value)
    {
        requests.push_back(value);
        if (set_error != 0)
            return set_error;
        if (value > maximum && !clamp)
            return policy.capacity_error;
        current = std::min(value, maximum);
        return 0;
    }
    int get(std::int32_t& value)
    {
        ++reads;
        value = policy.doubled_readback ? current * 2 : current;
        return read_error;
    }
    int configure(std::int32_t requested,
        detail::UdpBufferKind kind = detail::UdpBufferKind::receive)
    {
        return detail::configure_udp_buffer(
            requested, kind, policy,
            [&](std::int32_t value) {
                return set(value);
            },
            [&](std::int32_t& value) {
                return get(value);
            });
    }
};

} // namespace

TEST(udp_buffer_success_reads_back_clamping_and_linux_bookkeeping)
{
    for (bool doubled : {false, true}) {
        for (std::int32_t maximum : {32'768, 65'536, 131'072}) {
            BufferNotices notices;
            BufferModel model;
            model.clamp = true;
            model.maximum = maximum;
            model.policy.doubled_readback = doubled;
            REQUIRE_EQ(model.configure(65'536), 0);
            REQUIRE_EQ(model.requests.size(), 1U);
            REQUIRE_EQ(model.reads, 1U);
            REQUIRE_EQ(detail::udp_buffer_notice_count, 1U);
            const auto notice = detail::udp_buffer_notices[0];
            REQUIRE_EQ(notice.kind, detail::UdpBufferKind::receive);
            REQUIRE_EQ(notice.requested, 65'536);
            REQUIRE_EQ(notice.effective, std::min(maximum, 65'536));
            REQUIRE_EQ(
                notice.kernel_bytes, notice.effective * (doubled ? 2 : 1));
            REQUIRE_EQ(notice.attempts, 1U);
            REQUIRE(!notice.fallback);
        }
    }
}

TEST(udp_buffer_capacity_search_grows_to_supported_size_without_shrinking)
{
    // Native Windows WSAENOBUFS is not POSIX ENOBUFS. Exercise both identities.
    for (int capacity_error : {ENOBUFS, 10055}) {
        for (std::int32_t maximum : {64'000, 262'145, 1'048'576}) {
            BufferNotices notices;
            BufferModel model;
            model.policy.capacity_error = capacity_error;
            model.maximum = maximum;
            const auto original = model.current;
            REQUIRE_EQ(model.configure(12'288'000), 0);
            REQUIRE_EQ(model.current, maximum);
            REQUIRE(model.requests.size() <= 32U);
            REQUIRE(std::all_of(
                model.requests.begin(), model.requests.end(), [&](auto value) {
                    return value > original && value <= 12'288'000;
                }));
            const auto notice = detail::udp_buffer_notices[0];
            REQUIRE_EQ(notice.effective, maximum);
            REQUIRE_EQ(notice.attempts, model.requests.size());
            REQUIRE(notice.fallback);
        }
    }
}

TEST(udp_buffer_capacity_search_preserves_existing_buffer_and_is_bounded)
{
    BufferNotices notices;
    BufferModel model;
    model.current = model.maximum = 262'144;
    REQUIRE_EQ(model.configure((std::numeric_limits<std::int32_t>::max)()), 0);
    REQUIRE_EQ(model.current, 262'144);
    REQUIRE(model.requests.size() <= 32U);
    REQUIRE_EQ(detail::udp_buffer_notices[0].effective, 262'144);
    REQUIRE(detail::udp_buffer_notices[0].fallback);
}

TEST(udp_buffer_rejected_shrink_does_not_silently_exceed_user_request)
{
    BufferNotices notices;
    BufferModel model;
    model.set_error = ENOBUFS;
    REQUIRE_EQ(model.configure(4096), ENOBUFS);
    REQUIRE_EQ(model.requests.size(), 1U);
    REQUIRE_EQ(model.current, 8192);
    REQUIRE_EQ(detail::udp_buffer_notice_count, 0U);
}

TEST(udp_buffer_unrelated_errors_and_failed_readbacks_remain_errors)
{
    for (int error : {EBADF, EINVAL, EACCES}) {
        BufferNotices notices;
        BufferModel model;
        model.set_error = error;
        REQUIRE_EQ(model.configure(65'536), error);
        REQUIRE_EQ(model.requests.size(), 1U);
        REQUIRE_EQ(model.reads, 0U);
        REQUIRE_EQ(detail::udp_buffer_notice_count, 0U);
    }
    for (bool reject : {false, true}) {
        BufferNotices notices;
        BufferModel model;
        model.set_error = reject ? ENOBUFS : 0;
        model.read_error = EBADF;
        REQUIRE_EQ(model.configure(65'536), EBADF);
        REQUIRE_EQ(model.requests.size(), 1U);
        REQUIRE_EQ(detail::udp_buffer_notice_count, 0U);
    }
}

TEST(udp_buffer_search_does_not_hide_later_set_or_read_errors)
{
    for (bool read_failure : {false, true}) {
        BufferNotices notices;
        BufferModel model;
        REQUIRE_EQ(detail::configure_udp_buffer(
                       65'536, detail::UdpBufferKind::send, model.policy,
                       [&](std::int32_t value) {
                           if (model.requests.empty()) {
                               model.requests.push_back(value);
                               return ENOBUFS;
                           }
                           if (!read_failure)
                               return EACCES;
                           model.read_error = EBADF;
                           return model.set(value);
                       },
                       [&](std::int32_t& value) {
                           return model.get(value);
                       }),
            read_failure ? EBADF : EACCES);
        REQUIRE_EQ(detail::udp_buffer_notice_count, 0U);
    }
}

TEST(udp_buffer_invalid_kernel_sizes_are_not_success)
{
    for (std::int32_t raw : {-1, 0, 1}) {
        BufferNotices notices;
        REQUIRE_EQ(
            detail::configure_udp_buffer(
                65'536, detail::UdpBufferKind::send, {true, ENOBUFS, EINVAL},
                [](std::int32_t) {
                    return 0;
                },
                [&](std::int32_t& value) {
                    value = raw;
                    return 0;
                }),
            EINVAL);
        REQUIRE_EQ(detail::udp_buffer_notice_count, 0U);
    }
}

TEST(udp_buffer_search_retains_clamped_readback_within_attempt_bound)
{
    BufferNotices notices;
    BufferModel model;
    model.maximum = 65'536;
    REQUIRE_EQ(detail::configure_udp_buffer(
                   1'048'576, detail::UdpBufferKind::send, model.policy,
                   [&](std::int32_t value) {
                       if (!model.requests.empty())
                           model.clamp = true;
                       return model.set(value);
                   },
                   [&](std::int32_t& value) {
                       return model.get(value);
                   }),
        0);
    REQUIRE(model.requests.size() <= 32U);
    REQUIRE_EQ(detail::udp_buffer_notices[0].effective, 65'536);
}

TEST(udp_buffer_search_does_not_mistake_rounding_for_the_os_limit)
{
    BufferNotices notices;
    BufferModel model;
    REQUIRE_EQ(detail::configure_udp_buffer(
                   12'288'000, detail::UdpBufferKind::send, model.policy,
                   [&](std::int32_t value) {
                       const int error = model.set(value);
                       if (error == 0)
                           model.current = (model.current / 4096) * 4096;
                       return error;
                   },
                   [&](std::int32_t& value) {
                       return model.get(value);
                   }),
        0);
    REQUIRE(model.requests.size() <= 32U);
    REQUIRE_EQ(detail::udp_buffer_notices[0].effective, model.maximum);
}

TEST(udp_buffer_search_requires_valid_final_readback)
{
    for (bool lost_capacity : {false, true}) {
        BufferNotices notices;
        unsigned reads = 0;
        REQUIRE_EQ(
            detail::configure_udp_buffer(
                8193, detail::UdpBufferKind::send, {false, ENOBUFS, EINVAL},
                [](std::int32_t) {
                    return ENOBUFS;
                },
                [&](std::int32_t& value) {
                    value = ++reads == 1 ? 8192 : 4096;
                    return reads == 1 || lost_capacity ? 0 : EBADF;
                }),
            lost_capacity ? ENOBUFS : EBADF);
        REQUIRE_EQ(detail::udp_buffer_notice_count, 0U);
    }
}

TEST(udp_buffer_native_readbacks_match_send_and_receive_diagnostics)
{
    for (auto family : {IpAddressFamily::ipv4, IpAddressFamily::ipv6}) {
        UdpSocket original {family};
        REQUIRE(original.valid());
        REQUIRE_EQ(original.bind(family == IpAddressFamily::ipv4
                           ? IpEndpoint::loopback(0)
                           : IpEndpoint::ipv6_loopback(0)),
            Error::none);
        const auto handle = original.release_native();
        UdpSocket socket = UdpSocket::acquire_native(handle);
        REQUIRE(socket.valid());
        BufferNotices notices;
        REQUIRE_EQ(socket.set_send_buffer_size(65'536), Error::none);
        REQUIRE_EQ(socket.set_receive_buffer_size(262'144), Error::none);
        REQUIRE_EQ(detail::udp_buffer_notice_count, 2U);
        for (unsigned i = 0; i < 2; ++i) {
            int raw = 0;
#if defined(_WIN32)
            int size = sizeof(raw);
            const int status = ::getsockopt(static_cast<SOCKET>(handle),
                SOL_SOCKET, i == 0 ? SO_SNDBUF : SO_RCVBUF,
                reinterpret_cast<char*>(&raw), &size);
#else
            socklen_t size = sizeof(raw);
            const int status = ::getsockopt(static_cast<int>(handle),
                SOL_SOCKET, i == 0 ? SO_SNDBUF : SO_RCVBUF, &raw, &size);
#endif
            REQUIRE_EQ(status, 0);
            const auto notice = detail::udp_buffer_notices[i];
            REQUIRE_EQ(notice.kernel_bytes, raw);
            REQUIRE_EQ(notice.kind,
                i == 0 ? detail::UdpBufferKind::send
                       : detail::UdpBufferKind::receive);
            REQUIRE_EQ(notice.requested, i == 0 ? 65'536 : 262'144);
#if defined(__linux__)
            REQUIRE_EQ(notice.effective, raw / 2);
#else
            REQUIRE_EQ(notice.effective, raw);
#endif
            REQUIRE(notice.effective > 0);
        }
    }
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
