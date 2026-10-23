#include "test.hpp"
#include "ipv6_probe.hpp"

#include "compat/readiness.hpp"
#include "compat/socket_io.hpp"
#include "compat/socket_registry.hpp"
#include "robotweax/srt/udp.hpp"
#include "srt/srt.h"

#include <array>
#include <chrono>
#include <cstring>

#if defined(__APPLE__)
#include <net/if.h>
#endif
#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace {
using namespace robotweax::srt;
using namespace robotweax::srt::compat;
constexpr UDPSOCKET invalid_udp = static_cast<UDPSOCKET>(-1);

bool descriptor_open(UDPSOCKET descriptor)
{
    // Winsock getsockname rejects a valid socket before bind. Query its type
    // so ownership checks also cover descriptors whose adoption was rejected
    // specifically because the application left them unbound.
    int type = 0;
#if defined(_WIN32)
    int size = sizeof(type);
    return ::getsockopt(descriptor, SOL_SOCKET, SO_TYPE,
               reinterpret_cast<char*>(&type), &size)
        == 0;
#else
    socklen_t size = sizeof(type);
    return ::getsockopt(descriptor, SOL_SOCKET, SO_TYPE, &type, &size) == 0;
#endif
}

struct Fixture {
    std::array<UDPSOCKET, 2> descriptors {invalid_udp, invalid_udp};
    std::array<SRT_SOCKGROUPCONFIG, 2> endpoints {};
    std::array<SRT_SOCKOPT_CONFIG*, 2> configs {};
    SRTSOCKET group = SRT_INVALID_SOCK;
    SRTSOCKET listener = SRT_INVALID_SOCK;
    SRTSOCKET mirror = SRT_INVALID_SOCK;
    IpEndpoint peer = IpEndpoint::loopback(9'000);

    Fixture()
    {
        REQUIRE_EQ(srt_startup(), 0);
    }
    ~Fixture()
    {
        for (auto socket : {group, mirror, listener})
            if (socket != SRT_INVALID_SOCK)
                (void)srt_close(socket);
        (void)srt_cleanup();
        for (auto descriptor : descriptors) {
            if (descriptor == invalid_udp)
                continue;
#if defined(_WIN32)
            (void)::closesocket(descriptor);
#else
            (void)::close(descriptor);
#endif
        }
        for (auto* config : configs)
            srt_delete_config(config);
    }

    void create_group(SRT_GROUP_TYPE type = SRT_GTYPE_BACKUP)
    {
        group = srt_create_group(type);
        REQUIRE(group != SRT_INVALID_SOCK);
        const int timeout = 2'000;
        const int latency = 20;
        REQUIRE_EQ(
            srt_setsockflag(group, SRTO_CONNTIMEO, &timeout, sizeof(timeout)),
            0);
        REQUIRE_EQ(
            srt_setsockflag(group, SRTO_RCVTIMEO, &timeout, sizeof(timeout)),
            0);
        REQUIRE_EQ(
            srt_setsockflag(group, SRTO_LATENCY, &latency, sizeof(latency)), 0);
    }

    void create_listener(IpAddressFamily family = IpAddressFamily::ipv4)
    {
        listener = srt_create_socket();
        REQUIRE(listener != SRT_INVALID_SOCK);
        const bool enabled = true;
        const int timeout = 2'000;
        REQUIRE_EQ(srt_setsockflag(
                       listener, SRTO_GROUPCONNECT, &enabled, sizeof(enabled)),
            0);
        REQUIRE_EQ(
            srt_setsockflag(listener, SRTO_RCVTIMEO, &timeout, sizeof(timeout)),
            0);
        sockaddr_storage address {};
        int size = sizeof(address);
        REQUIRE_EQ(write_ip_endpoint(family == IpAddressFamily::ipv4
                           ? IpEndpoint::loopback()
                           : IpEndpoint::ipv6_loopback(),
                       reinterpret_cast<sockaddr*>(&address), &size),
            0);
        REQUIRE_EQ(
            srt_bind(listener, reinterpret_cast<sockaddr*>(&address), size), 0);
        REQUIRE_EQ(srt_listen(listener, 4), 0);
        REQUIRE_EQ(srt_getsockname(
                       listener, reinterpret_cast<sockaddr*>(&address), &size),
            0);
        REQUIRE_EQ(decode_ip_endpoint(reinterpret_cast<sockaddr*>(&address),
                       size, peer, false),
            0);
    }

    IpEndpoint bind_native(
        std::size_t index, IpEndpoint local = IpEndpoint::loopback())
    {
        UdpSocket socket {local.family};
        REQUIRE(socket.valid());
        REQUIRE_EQ(socket.bind(local), Error::none);
        const auto bound = socket.local_endpoint();
        REQUIRE(bound);
        descriptors[index] = static_cast<UDPSOCKET>(socket.release_native());
        return bound.endpoint;
    }

    void prepare(std::size_t index)
    {
        sockaddr_storage address {};
        int size = sizeof(address);
        REQUIRE_EQ(write_ip_endpoint(
                       peer, reinterpret_cast<sockaddr*>(&address), &size),
            0);
        endpoints[index] = srt_prepare_endpoint(
            nullptr, reinterpret_cast<sockaddr*>(&address), size);
        REQUIRE_EQ(endpoints[index].errorcode, SRT_SUCCESS);
        endpoints[index].weight = static_cast<std::uint16_t>(10 - index);
        endpoints[index].token = static_cast<int>(100 + index);
    }

    int connect(int count)
    {
        return robotweax_srt_connect_group_acquire(
            group, endpoints.data(), descriptors.data(), count);
    }

    void wait_connected(std::size_t index)
    {
        const auto deadline =
            ReadinessSignal::Clock::now() + std::chrono::seconds {2};
        while (true) {
            const auto generation = ReadinessSignal::generation();
            if (srt_getsockstate(endpoints[index].id) != SRTS_CONNECTING)
                break;
            REQUIRE(ReadinessSignal::Clock::now() < deadline);
            ReadinessSignal::wait_until(generation, deadline);
        }
        REQUIRE_EQ(srt_getsockstate(endpoints[index].id), SRTS_CONNECTED);
    }

    void transfer(const char* payload, int length)
    {
        REQUIRE_EQ(srt_sendmsg(group, payload, length, -1, 1), length);
        std::array<char, SRT_LIVE_MAX_PLSIZE> received {};
        REQUIRE_EQ(
            srt_recvmsg(mirror, received.data(), received.size()), length);
        REQUIRE_EQ(std::memcmp(received.data(), payload, length), 0);
    }
};

void check_connected_channels(SRT_GROUP_TYPE type, IpAddressFamily family)
{
    Fixture f;
    f.create_group(type);
    f.create_listener(family);
    const auto local = family == IpAddressFamily::ipv4
        ? IpEndpoint::loopback()
        : IpEndpoint::ipv6_loopback();
    const std::array bound {f.bind_native(0, local), f.bind_native(1, local)};
    const auto originals = f.descriptors;
    for (std::size_t index = 0; index < 2; ++index)
        f.prepare(index);
    REQUIRE(f.connect(2) != SRT_ERROR);
    f.mirror = srt_accept(f.listener, nullptr, nullptr);
    REQUIRE(f.mirror != SRT_INVALID_SOCK);
    std::array<std::shared_ptr<DatagramChannel>, 2> channels;
    for (std::size_t index = 0; index < 2; ++index) {
        f.wait_connected(index);
        REQUIRE_EQ(f.descriptors[index], invalid_udp);
        REQUIRE_EQ(srt_groupof(f.endpoints[index].id), f.group);
        const auto record =
            SocketRegistry::instance().find(f.endpoints[index].id);
        REQUIRE(record != nullptr);
        std::lock_guard lock(record->mutex);
        REQUIRE_EQ(record->local_endpoint, bound[index]);
        channels[index] = record->channel;
        REQUIRE_EQ(channels[index]->socket.native_handle(),
            static_cast<std::uintptr_t>(originals[index]));
    }
    REQUIRE(channels[0] != channels[1]);
    std::array<SRT_SOCKGROUPDATA, 2> members {};
    std::size_t size = members.size();
    REQUIRE_EQ(srt_group_data(f.group, members.data(), &size), 2);
    for (const auto& member : members) {
        const auto index = member.id == f.endpoints[0].id ? 0U : 1U;
        REQUIRE_EQ(member.id, f.endpoints[index].id);
        REQUIRE_EQ(member.token, f.endpoints[index].token);
        REQUIRE_EQ(member.weight, f.endpoints[index].weight);
    }
    constexpr char before[] = "native member payload";
    f.transfer(before, sizeof(before));
    REQUIRE_EQ(srt_close(f.endpoints[0].id), 0);
    constexpr char after[] = "surviving native member";
    f.transfer(after, sizeof(after));
    channels = {};
    REQUIRE_EQ(srt_close(f.group), 0);
    f.group = SRT_INVALID_SOCK;
    REQUIRE_EQ(srt_cleanup(), 0);
    for (const auto descriptor : originals)
        REQUIRE(!descriptor_open(descriptor));
}
} // namespace

TEST(srt_compat_group_acquire_backup_ipv4_channels_and_failover)
{
    check_connected_channels(SRT_GTYPE_BACKUP, IpAddressFamily::ipv4);
}
TEST(srt_compat_group_acquire_broadcast_ipv4_channels_and_survivor)
{
    check_connected_channels(SRT_GTYPE_BROADCAST, IpAddressFamily::ipv4);
}
TEST(srt_compat_group_acquire_backup_ipv6_channels_and_failover)
{
    SKIP_WITHOUT_IPV6_LOOPBACK();
    check_connected_channels(SRT_GTYPE_BACKUP, IpAddressFamily::ipv6);
}
TEST(srt_compat_group_acquire_broadcast_ipv6_channels_and_survivor)
{
    SKIP_WITHOUT_IPV6_LOOPBACK();
    check_connected_channels(SRT_GTYPE_BROADCAST, IpAddressFamily::ipv6);
}

TEST(srt_compat_group_acquire_rejects_arguments_without_taking_ownership)
{
    Fixture f;
    f.create_group();
    f.bind_native(0);
    f.prepare(0);
    const auto original = f.descriptors[0];
    for (const auto count : {0, -1}) {
        REQUIRE_EQ(f.connect(count), SRT_ERROR);
        REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);
        REQUIRE_EQ(f.descriptors[0], original);
    }
    REQUIRE_EQ(robotweax_srt_connect_group_acquire(
                   f.group, nullptr, f.descriptors.data(), 1),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);
    REQUIRE_EQ(robotweax_srt_connect_group_acquire(
                   f.group, f.endpoints.data(), nullptr, 1),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);
    REQUIRE_EQ(robotweax_srt_connect_group_acquire(SRT_INVALID_SOCK,
                   f.endpoints.data(), f.descriptors.data(), 1),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVSOCK);
    f.prepare(1);
    // A later invalid slot rejects the entire batch before adopting slot 0.
    REQUIRE_EQ(f.connect(2), SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);
    REQUIRE_EQ(f.descriptors[0], original);
    REQUIRE(descriptor_open(original));
}

TEST(srt_compat_group_acquire_rejects_duplicate_descriptors_before_adoption)
{
    Fixture f;
    f.create_group();
    f.bind_native(0);
    f.prepare(0);
    f.prepare(1);
    auto duplicates = std::array {f.descriptors[0], f.descriptors[0]};
    REQUIRE_EQ(robotweax_srt_connect_group_acquire(
                   f.group, f.endpoints.data(), duplicates.data(), 2),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);
    REQUIRE_EQ(duplicates[0], f.descriptors[0]);
    REQUIRE_EQ(duplicates[1], f.descriptors[0]);
    REQUIRE(descriptor_open(f.descriptors[0]));
}

TEST(srt_compat_group_acquire_rejects_explicit_source_before_adoption)
{
    Fixture f;
    f.create_group();
    f.bind_native(0);
    const auto local = f.bind_native(1);
    f.prepare(0);
    f.prepare(1);
    int size = sizeof(f.endpoints[1].srcaddr);
    REQUIRE_EQ(write_ip_endpoint(local,
                   reinterpret_cast<sockaddr*>(&f.endpoints[1].srcaddr), &size),
        0);
    REQUIRE_EQ(f.connect(2), SRT_ERROR);
    REQUIRE_EQ(f.endpoints[1].errorcode, SRT_EINVPARAM);
    REQUIRE(descriptor_open(f.descriptors[0]));
    REQUIRE(descriptor_open(f.descriptors[1]));
}

TEST(srt_compat_group_acquire_option_failure_leaves_descriptor_caller_owned)
{
    Fixture f;
    f.create_group();
    f.bind_native(0);
    f.prepare(0);
    f.configs[0] = srt_create_config();
    REQUIRE(f.configs[0] != nullptr);
    const int invalid = -1;
    REQUIRE_EQ(srt_config_add(
                   f.configs[0], SRTO_UDP_SNDBUF, &invalid, sizeof(invalid)),
        0);
    f.endpoints[0].config = f.configs[0];
    REQUIRE_EQ(f.connect(1), SRT_ERROR);
    REQUIRE_EQ(f.endpoints[0].id, SRT_INVALID_SOCK);
    REQUIRE_EQ(f.endpoints[0].errorcode, SRT_EINVPARAM);
    REQUIRE(descriptor_open(f.descriptors[0]));
}

TEST(srt_compat_group_acquire_partial_failure_preserves_unbound_descriptor)
{
    Fixture f;
    f.create_group();
    f.create_listener();
    f.bind_native(0);
    UdpSocket unbound;
    REQUIRE(unbound.valid());
    f.descriptors[1] = static_cast<UDPSOCKET>(unbound.release_native());
    const auto original = f.descriptors[1];
    f.prepare(0);
    f.prepare(1);
    REQUIRE(f.connect(2) != SRT_ERROR);
    REQUIRE_EQ(f.descriptors[0], invalid_udp);
    REQUIRE_EQ(f.descriptors[1], original);
    REQUIRE_EQ(f.endpoints[1].id, SRT_INVALID_SOCK);
    REQUIRE_EQ(f.endpoints[1].errorcode, SRT_EINVPARAM);
    REQUIRE(descriptor_open(original));
    f.mirror = srt_accept(f.listener, nullptr, nullptr);
    REQUIRE(f.mirror != SRT_INVALID_SOCK);
    f.wait_connected(0);
    constexpr char payload[] = "partial adoption still connects";
    f.transfer(payload, sizeof(payload));
}

TEST(srt_compat_group_acquire_later_setup_failure_does_not_return_ownership)
{
    SKIP_WITHOUT_IPV6_LOOPBACK();
    Fixture f;
    f.create_group();
    // A bound IPv4 channel cannot connect to a native IPv6 destination.
    f.peer = IpEndpoint::ipv6_loopback(9'000);
    f.bind_native(0);
    const auto original = f.descriptors[0];
    f.prepare(0);
    REQUIRE_EQ(f.connect(1), SRT_ERROR);
    REQUIRE_EQ(f.endpoints[0].id, SRT_INVALID_SOCK);
    REQUIRE_EQ(f.endpoints[0].errorcode, SRT_EINVPARAM);
    REQUIRE_EQ(f.descriptors[0], invalid_udp);
    REQUIRE_EQ(srt_cleanup(), 0);
    REQUIRE(!descriptor_open(original));
}

TEST(srt_compat_group_acquire_asynchronous_member_options_and_native_interface)
{
    Fixture f;
    f.create_group();
    f.create_listener();
    const bool synchronous = false;
    REQUIRE_EQ(srt_setsockflag(
                   f.group, SRTO_RCVSYN, &synchronous, sizeof(synchronous)),
        0);
    f.bind_native(0);
    const auto original = f.descriptors[0];
#if defined(__APPLE__) && defined(IP_BOUND_IF)
    const unsigned interface_index = if_nametoindex("lo0");
    REQUIRE(interface_index != 0);
    REQUIRE_EQ(::setsockopt(original, IPPROTO_IP, IP_BOUND_IF, &interface_index,
                   sizeof(interface_index)),
        0);
#endif
    f.prepare(0);
    f.configs[0] = srt_create_config();
    REQUIRE(f.configs[0] != nullptr);
    const int stability = 240;
    const int ttl = 42;
    REQUIRE_EQ(srt_config_add(f.configs[0], SRTO_GROUPMINSTABLETIMEO,
                   &stability, sizeof(stability)),
        0);
    REQUIRE_EQ(srt_config_add(f.configs[0], SRTO_IPTTL, &ttl, sizeof(ttl)), 0);
    f.endpoints[0].config = f.configs[0];
    REQUIRE(f.connect(1) != SRT_ERROR);
    REQUIRE_EQ(f.descriptors[0], invalid_udp);
    srt_delete_config(f.configs[0]);
    f.configs[0] = nullptr;
    f.endpoints[0].config = nullptr;
    f.mirror = srt_accept(f.listener, nullptr, nullptr);
    REQUIRE(f.mirror != SRT_INVALID_SOCK);
    f.wait_connected(0);
    const auto record = SocketRegistry::instance().find(f.endpoints[0].id);
    REQUIRE(record != nullptr);
    {
        std::lock_guard lock(record->mutex);
        REQUIRE_EQ(
            record->group_minimum_stability_timeout_milliseconds, stability);
        const auto native_ttl = record->channel->socket.ip_time_to_live();
        REQUIRE(native_ttl);
        REQUIRE_EQ(native_ttl.value, ttl);
    }
#if defined(__APPLE__) && defined(IP_BOUND_IF)
    unsigned reported = 0;
    socklen_t size = sizeof(reported);
    REQUIRE_EQ(
        ::getsockopt(original, IPPROTO_IP, IP_BOUND_IF, &reported, &size), 0);
    REQUIRE_EQ(reported, interface_index);
#else
    REQUIRE(descriptor_open(original));
#endif
    constexpr char payload[] = "independent endpoint options";
    f.transfer(payload, sizeof(payload));
}

TEST(srt_compat_group_acquire_blocking_timeout_retains_library_ownership)
{
    Fixture f;
    f.create_group();
    // Reserve a destination port without an SRT listener; no other process can
    // accidentally admit this caller during its deliberately short timeout.
    UdpSocket silent_peer;
    REQUIRE(silent_peer.valid());
    REQUIRE_EQ(silent_peer.bind(IpEndpoint::loopback()), Error::none);
    const auto bound = silent_peer.local_endpoint();
    REQUIRE(bound);
    f.peer = bound.endpoint;
    const int timeout = 40;
    REQUIRE_EQ(
        srt_setsockflag(f.group, SRTO_CONNTIMEO, &timeout, sizeof(timeout)), 0);
    f.bind_native(0);
    const auto original = f.descriptors[0];
    f.prepare(0);
    REQUIRE_EQ(f.connect(1), SRT_ERROR);
    REQUIRE_EQ(f.descriptors[0], invalid_udp);
    REQUIRE(f.endpoints[0].id != SRT_INVALID_SOCK);
    REQUIRE_EQ(srt_groupof(f.endpoints[0].id), f.group);
    REQUIRE_EQ(srt_close(f.group), 0);
    f.group = SRT_INVALID_SOCK;
    REQUIRE_EQ(srt_cleanup(), 0);
    REQUIRE(!descriptor_open(original));
}
