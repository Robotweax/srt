#include "test.hpp"
#include "ipv6_probe.hpp"

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
#include <utility>

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
        if (family == IpAddressFamily::ipv6 && !ipv6_loopback_available()) {
            continue;
        }
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
    SKIP_WITHOUT_IPV6_LOOPBACK();
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
    SKIP_WITHOUT_IPV6_LOOPBACK();
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
    SKIP_WITHOUT_IPV6_LOOPBACK();
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
    SKIP_WITHOUT_IPV6_LOOPBACK();
    require_oversized_datagram_is_discarded(IpAddressFamily::ipv6);
}

// --- Multicast primitives -------------------------------------------------

namespace {

constexpr IpEndpoint ipv4_test_group(std::uint16_t port) noexcept
{
    // 239.255.0.0/16 is administratively scoped; TTL 0 keeps it on-host.
    return {.address = {239, 255, 133, 10}, .port = port};
}

constexpr IpEndpoint ipv6_test_group(std::uint16_t port) noexcept
{
    IpEndpoint group = IpEndpoint::ipv6_any(port);
    group.address[0] = 0xff;
    group.address[1] = 0x01; // interface-local scope
    group.address[15] = 0x42;
    return group;
}

// Whether this host lets a socket join an IPv4 multicast group on the
// loopback/default interface. Containers without a multicast route refuse
// IP_ADD_MEMBERSHIP with ENODEV; that is an environment limit, not a defect.
[[nodiscard]] bool ipv4_multicast_available()
{
    UdpSocket probe;
    if (!probe.valid() || probe.bind(IpEndpoint::any()) != Error::none) {
        return false;
    }
    return probe.join_multicast(ipv4_test_group(0)) == Error::none;
}

} // namespace

TEST(ip_endpoint_classifies_multicast_addresses)
{
    REQUIRE(ipv4_test_group(1).is_multicast());
    REQUIRE((IpEndpoint{.address = {224, 0, 0, 1}}).is_multicast());
    REQUIRE((IpEndpoint{.address = {239, 255, 255, 255}}).is_multicast());
    REQUIRE(!(IpEndpoint{.address = {223, 255, 255, 255}}).is_multicast());
    REQUIRE(!(IpEndpoint{.address = {240, 0, 0, 1}}).is_multicast());
    REQUIRE(!IpEndpoint::loopback().is_multicast());
    REQUIRE(ipv6_test_group(1).is_multicast());
    REQUIRE(!IpEndpoint::ipv6_loopback().is_multicast());
    IpEndpoint almost = IpEndpoint::ipv6_any();
    almost.address[0] = 0xfe;
    almost.address[1] = 0x80;
    REQUIRE(!almost.is_multicast());
}

TEST(udp_multicast_options_validate_arguments_before_touching_the_socket)
{
    UdpSocket ipv4;
    REQUIRE(ipv4.valid());
    // Family mismatch and non-multicast groups are argument errors.
    REQUIRE_EQ(ipv4.join_multicast(ipv6_test_group(5'000)), Error::invalid_state);
    REQUIRE_EQ(ipv4.join_multicast(IpEndpoint::loopback(5'000)), Error::invalid_state);
    REQUIRE_EQ(ipv4.leave_multicast(IpEndpoint::loopback(5'000)), Error::invalid_state);
    REQUIRE_EQ(ipv4.last_system_error(), 0);
    // A multicast address is not an interface selector.
    REQUIRE_EQ(ipv4.join_multicast(ipv4_test_group(5'000), ipv4_test_group(0)),
        Error::invalid_state);
    REQUIRE_EQ(ipv4.set_multicast_interface(ipv4_test_group(0)), Error::invalid_state);
    REQUIRE_EQ(ipv4.set_multicast_interface(IpEndpoint::ipv6_loopback()),
        Error::invalid_state);
    // Hop limits are 0..255; unlike unicast TTL, 0 is meaningful (on-host).
    REQUIRE_EQ(ipv4.set_multicast_time_to_live(-1), Error::invalid_state);
    REQUIRE_EQ(ipv4.set_multicast_time_to_live(256), Error::invalid_state);

    // Invalid sockets report io_error with the open error, never success.
    UdpSocket moved_from = std::move(ipv4);
    REQUIRE(!ipv4.valid());
    REQUIRE_EQ(ipv4.set_reuse_address(true), Error::io_error);
    REQUIRE_EQ(ipv4.join_multicast(ipv4_test_group(1)), Error::io_error);
    REQUIRE_EQ(ipv4.set_multicast_time_to_live(1), Error::io_error);
    REQUIRE_EQ(ipv4.set_multicast_loopback(true), Error::io_error);
    REQUIRE_EQ(ipv4.multicast_time_to_live().error, Error::io_error);
    REQUIRE_EQ(ipv4.multicast_loopback().error, Error::io_error);
    REQUIRE(moved_from.valid());
}

TEST(udp_ipv4_multicast_ttl_and_loopback_round_trip)
{
    UdpSocket ipv4;
    REQUIRE(ipv4.valid());
    for (const std::int32_t ttl : {0, 1, 7, 255}) {
        REQUIRE_EQ(ipv4.set_multicast_time_to_live(ttl), Error::none);
        const UdpIntegerOptionResult read = ipv4.multicast_time_to_live();
        REQUIRE(read);
        REQUIRE_EQ(read.value, ttl);
    }
    REQUIRE_EQ(ipv4.set_multicast_loopback(false), Error::none);
    REQUIRE(ipv4.multicast_loopback());
    REQUIRE_EQ(ipv4.multicast_loopback().value, 0);
    REQUIRE_EQ(ipv4.set_multicast_loopback(true), Error::none);
    REQUIRE_EQ(ipv4.multicast_loopback().value, 1);
}

TEST(udp_ipv6_multicast_hops_and_loopback_round_trip)
{
    SKIP_WITHOUT_IPV6_LOOPBACK();
    UdpSocket ipv6(IpAddressFamily::ipv6);
    REQUIRE(ipv6.valid());
    for (const std::int32_t hops : {0, 1, 64, 255}) {
        REQUIRE_EQ(ipv6.set_multicast_time_to_live(hops), Error::none);
        const UdpIntegerOptionResult read = ipv6.multicast_time_to_live();
        REQUIRE(read);
        REQUIRE_EQ(read.value, hops);
    }
    REQUIRE_EQ(ipv6.set_multicast_loopback(false), Error::none);
    REQUIRE_EQ(ipv6.multicast_loopback().value, 0);
    REQUIRE_EQ(ipv6.set_multicast_loopback(true), Error::none);
    REQUIRE_EQ(ipv6.multicast_loopback().value, 1);
    // An IPv6 group on an IPv4 socket and vice versa stay argument errors.
    REQUIRE_EQ(ipv6.join_multicast(ipv4_test_group(1)), Error::invalid_state);
}

TEST(udp_multicast_join_leave_delivers_one_datagram_to_two_local_receivers)
{
    SKIP_UNLESS(ipv4_multicast_available(),
        "no IPv4 multicast membership on this host");

    // Two receivers share the group port through reuse-address, both join.
    UdpSocket first;
    UdpSocket second;
    REQUIRE(first.valid() && second.valid());
    REQUIRE_EQ(first.set_reuse_address(true), Error::none);
    REQUIRE_EQ(second.set_reuse_address(true), Error::none);
    REQUIRE_EQ(first.bind(IpEndpoint::any()), Error::none);
    const EndpointResult bound = first.local_endpoint();
    REQUIRE(bound);
    const IpEndpoint group = ipv4_test_group(bound.endpoint.port);
    REQUIRE_EQ(second.bind(IpEndpoint::any(group.port)), Error::none);
    REQUIRE_EQ(first.join_multicast(group), Error::none);
    REQUIRE_EQ(second.join_multicast(group), Error::none);

    // The sender keeps the datagram on this host and loops it back locally.
    UdpSocket sender;
    REQUIRE(sender.valid());
    REQUIRE_EQ(sender.set_multicast_time_to_live(0), Error::none);
    REQUIRE_EQ(sender.set_multicast_loopback(true), Error::none);
    const std::array<std::byte, 5> payload{std::byte{'m'}, std::byte{'c'},
        std::byte{'a'}, std::byte{'s'}, std::byte{'t'}};
    const UdpIoResult sent = sender.send_to(payload, group);
    REQUIRE(sent);
    REQUIRE_EQ(sent.bytes_transferred, payload.size());

    for (UdpSocket* receiver : {&first, &second}) {
        const UdpWaitResult ready = receiver->wait_readable(2'000);
        REQUIRE(ready);
        REQUIRE(ready.ready);
        std::array<std::byte, 64> buffer{};
        const UdpIoResult received = receiver->receive_from(buffer);
        REQUIRE(received);
        REQUIRE_EQ(received.bytes_transferred, payload.size());
        REQUIRE(std::equal(payload.begin(), payload.end(), buffer.begin()));
    }

    // After leaving, a second datagram no longer reaches that receiver.
    REQUIRE_EQ(second.leave_multicast(group), Error::none);
    REQUIRE(sender.send_to(payload, group));
    const UdpWaitResult still = first.wait_readable(2'000);
    REQUIRE(still && still.ready);
    const UdpWaitResult gone = second.wait_readable(200);
    REQUIRE(gone);
    REQUIRE(!gone.ready);
    // Leaving twice is a native failure, reported, not hidden.
    REQUIRE_EQ(second.leave_multicast(group), Error::io_error);
    REQUIRE(second.last_system_error() != 0);
}

TEST(udp_ipv6_multicast_join_and_loopback_delivery)
{
    SKIP_WITHOUT_IPV6_LOOPBACK();
    UdpSocket receiver(IpAddressFamily::ipv6);
    REQUIRE(receiver.valid());
    REQUIRE_EQ(receiver.set_reuse_address(true), Error::none);
    REQUIRE_EQ(receiver.bind(IpEndpoint::ipv6_any()), Error::none);
    const EndpointResult bound = receiver.local_endpoint();
    REQUIRE(bound);
    const IpEndpoint group = ipv6_test_group(bound.endpoint.port);
    // Interface-local scope needs an explicit interface on most stacks;
    // use the loopback interface's index when the host exposes one.
    IpEndpoint interface = IpEndpoint::ipv6_any();
    interface.scope_id = 1;
    const Error joined = receiver.join_multicast(group, interface);
    SKIP_UNLESS(joined == Error::none,
        "no IPv6 multicast membership on this host");

    UdpSocket sender(IpAddressFamily::ipv6);
    REQUIRE(sender.valid());
    REQUIRE_EQ(sender.set_multicast_interface(interface), Error::none);
    REQUIRE_EQ(sender.set_multicast_time_to_live(0), Error::none);
    REQUIRE_EQ(sender.set_multicast_loopback(true), Error::none);
    const std::array<std::byte, 3> payload{std::byte{'v'}, std::byte{'6'}, std::byte{'!'}};
    REQUIRE(sender.send_to(payload, group));
    const UdpWaitResult ready = receiver.wait_readable(2'000);
    REQUIRE(ready && ready.ready);
    std::array<std::byte, 64> buffer{};
    const UdpIoResult received = receiver.receive_from(buffer);
    REQUIRE(received);
    REQUIRE_EQ(received.bytes_transferred, payload.size());
    REQUIRE_EQ(receiver.leave_multicast(group, interface), Error::none);
}
