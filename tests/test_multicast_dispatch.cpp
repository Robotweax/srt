// End-to-end multicast dispatch groups over loopback multicast: one Listener
// sender, several Caller receivers, control on unicast, payload once to the
// group. The administratively scoped group 239.255.133.0/24 with hop limit 0
// and the loopback interface keeps every datagram on this host.
#include "test.hpp"

#include "compat/group_registry.hpp"
#include "compat/socket_registry.hpp"
#include "compat/transport_runtime.hpp"
#include "robotweax/srt/udp.hpp"
#include "srt/srt.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace robotweax::srt;
using robotweax::srt::compat::GroupCategory;
using robotweax::srt::compat::GroupRegistry;
using robotweax::srt::compat::SocketRegistry;
using Egress = robotweax::srt::compat::ConnectionRuntime::GroupDataEgress;

namespace {

constexpr std::uint16_t multicast_port = 25'999;

[[nodiscard]] sockaddr_in ipv4_sockaddr(
    const std::array<std::uint8_t, 4>& address, std::uint16_t port) noexcept
{
    sockaddr_in result {};
    result.sin_family = AF_INET;
    result.sin_port = htons(port);
    result.sin_addr.s_addr = htonl(
        (static_cast<std::uint32_t>(address[0]) << 24U)
        | (static_cast<std::uint32_t>(address[1]) << 16U)
        | (static_cast<std::uint32_t>(address[2]) << 8U)
        | static_cast<std::uint32_t>(address[3]));
    return result;
}

// Loopback multicast needs an explicit interface on both sides; a host
// without a multicast-capable loopback skips the test instead of failing.
[[nodiscard]] bool loopback_multicast_available()
{
    UdpSocket receiver;
    UdpSocket sender;
    if (!receiver.valid() || !sender.valid()
        || receiver.set_reuse_address(true) != Error::none
        || receiver.bind(IpEndpoint::any(multicast_port)) != Error::none
        || receiver.join_multicast({.address = {239, 255, 133, 12}},
               IpEndpoint::loopback())
            != Error::none
        || sender.bind(IpEndpoint::loopback()) != Error::none
        || sender.set_multicast_interface(IpEndpoint::loopback())
            != Error::none
        || sender.set_multicast_time_to_live(0) != Error::none) {
        return false;
    }
    const std::array<std::byte, 1> probe {std::byte {1}};
    if (!sender.send_to(probe,
            {.address = {239, 255, 133, 12}, .port = multicast_port})) {
        return false;
    }
    const UdpWaitResult ready = receiver.wait_readable(500);
    return ready && ready.ready;
}

struct Peers {
    std::vector<SRTSOCKET> handles;
    ~Peers()
    {
        for (const SRTSOCKET handle : handles) {
            (void)srt_close(handle);
        }
    }
    SRTSOCKET keep(SRTSOCKET handle)
    {
        handles.push_back(handle);
        return handle;
    }
    void drop(SRTSOCKET handle)
    {
        handles.erase(std::remove(handles.begin(), handles.end(), handle),
            handles.end());
        (void)srt_close(handle);
    }
};

struct AdmissionLog {
    std::atomic<int> calls {0};
    std::atomic<int> multicast_proposals {0};
};

int admit_dispatch(void* opaque, SRTSOCKET socket, int, const sockaddr*,
    const char*)
{
    auto& log = *static_cast<AdmissionLog*>(opaque);
    ++log.calls;
    std::int32_t type = -1;
    int size = sizeof(type);
    if (srt_getsockflag(socket, SRTO_GROUPTYPE, &type, &size) != 0) {
        return -1;
    }
    if (type != static_cast<std::int32_t>(ROBOTWEAX_SRT_GTYPE_MULTICAST)) {
        return -1;
    }
    ++log.multicast_proposals;
    const std::uint64_t domain = 7;
    if (srt_setsockflag(socket, SRTO_ROBOTWEAX_GROUPDOMAIN, &domain,
            sizeof(domain))
        != 0) {
        return -1;
    }
    const char key[] = "camera-1";
    return srt_setsockflag(socket, SRTO_ROBOTWEAX_MULTICAST, key,
        static_cast<int>(sizeof(key) - 1U));
}

// Admits any multicast proposal under a fixed key; used where the listener
// configuration itself is what must refuse the join.
int admit_dispatch_plain(void*, SRTSOCKET socket, int, const sockaddr*,
    const char*)
{
    const std::uint64_t domain = 7;
    if (srt_setsockflag(socket, SRTO_ROBOTWEAX_GROUPDOMAIN, &domain,
            sizeof(domain))
        != 0) {
        return -1;
    }
    const char key[] = "camera-1";
    return srt_setsockflag(socket, SRTO_ROBOTWEAX_MULTICAST, key,
        static_cast<int>(sizeof(key) - 1U));
}

// A member is usable once its socket is CONNECTED and its runtime attached.
[[nodiscard]] bool wait_for_members(SRTSOCKET group, std::size_t expected,
    std::chrono::milliseconds budget = std::chrono::seconds {5})
{
    const auto record = GroupRegistry::instance().find(group);
    if (record == nullptr) {
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + budget;
    for (;;) {
        // Member snapshots are refreshed on observation, as srt_group_data
        // and srt_getsockstate do.
        (void)GroupRegistry::instance().state(group);
        std::vector<SRTSOCKET> connected;
        {
            std::lock_guard lock(record->mutex);
            for (const auto& member : record->members) {
                if (member.public_data.sockstate == SRTS_CONNECTED) {
                    connected.push_back(member.public_data.id);
                }
            }
        }
        std::size_t ready = 0;
        for (const SRTSOCKET member : connected) {
            const auto socket = SocketRegistry::instance().find(member);
            if (socket == nullptr) {
                continue;
            }
            std::lock_guard lock(socket->mutex);
            if (socket->state == SRTS_CONNECTED && socket->runtime != nullptr) {
                ++ready;
            }
        }
        if (ready == expected && connected.size() == expected) {
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds {2});
    }
}

[[nodiscard]] SRTSOCKET make_receiver(Peers& peers, const sockaddr_in& listener)
{
    const SRTSOCKET group =
        peers.keep(srt_create_group(ROBOTWEAX_SRT_GTYPE_MULTICAST));
    REQUIRE(group != SRT_INVALID_SOCK);
    const std::int32_t receive_timeout = 3'000;
    REQUIRE_EQ(srt_setsockflag(group, SRTO_RCVTIMEO, &receive_timeout,
                   sizeof(receive_timeout)),
        0);
    const sockaddr_in interface = ipv4_sockaddr({127, 0, 0, 1}, 0);
    REQUIRE_EQ(srt_setsockflag(group, SRTO_ROBOTWEAX_MCAST_IFACE, &interface,
                   sizeof(interface)),
        0);
    SRT_SOCKGROUPCONFIG endpoint = srt_prepare_endpoint(nullptr,
        reinterpret_cast<const sockaddr*>(&listener), sizeof(listener));
    REQUIRE(srt_connect_group(group, &endpoint, 1) != SRT_ERROR);
    return group;
}

[[nodiscard]] std::string message(int index)
{
    return "dispatch payload " + std::to_string(index);
}

void expect_messages(SRTSOCKET receiver, int first, int count)
{
    std::array<char, 256> buffer {};
    SRT_MSGCTRL control = srt_msgctrl_default;
    for (int index = first; index < first + count; ++index) {
        const std::string expected = message(index);
        const int received =
            srt_recvmsg2(receiver, buffer.data(), buffer.size(), &control);
        REQUIRE_EQ(received, static_cast<int>(expected.size()));
        REQUIRE(std::equal(expected.begin(), expected.end(), buffer.begin()));
    }
}

} // namespace

TEST(multicast_dispatch_group_delivers_one_stream_to_several_receivers)
{
    SKIP_UNLESS(loopback_multicast_available(),
        "no loopback multicast on this host");
    Peers peers;
    AdmissionLog admissions;

    // Listener as dispatch sender on loopback.
    const SRTSOCKET listener = peers.keep(srt_create_socket());
    REQUIRE(listener != SRT_INVALID_SOCK);
    const sockaddr_in group_address =
        ipv4_sockaddr({239, 255, 133, 11}, multicast_port);
    const sockaddr_in interface = ipv4_sockaddr({127, 0, 0, 1}, 0);
    const std::int32_t hops = 0;
    REQUIRE_EQ(srt_setsockflag(listener, SRTO_ROBOTWEAX_MCAST_GROUP,
                   &group_address, sizeof(group_address)),
        0);
    REQUIRE_EQ(srt_setsockflag(listener, SRTO_ROBOTWEAX_MCAST_IFACE,
                   &interface, sizeof(interface)),
        0);
    REQUIRE_EQ(
        srt_setsockflag(listener, SRTO_ROBOTWEAX_MCAST_TTL, &hops, sizeof(hops)),
        0);
    sockaddr_in readback {};
    int readback_size = sizeof(readback);
    REQUIRE_EQ(srt_getsockflag(listener, SRTO_ROBOTWEAX_MCAST_GROUP,
                   &readback, &readback_size),
        0);
    REQUIRE_EQ(readback.sin_port, group_address.sin_port);
    REQUIRE_EQ(srt_listen_callback(listener, admit_dispatch, &admissions), 0);
    sockaddr_in bound = ipv4_sockaddr({127, 0, 0, 1}, 0);
    REQUIRE_EQ(srt_bind(listener, reinterpret_cast<const sockaddr*>(&bound),
                   sizeof(bound)),
        0);
    REQUIRE_EQ(srt_listen(listener, 8), 0);
    int bound_size = sizeof(bound);
    REQUIRE_EQ(srt_getsockname(listener, reinterpret_cast<sockaddr*>(&bound),
                   &bound_size),
        0);

    // Three receivers join; srt_accept yields the sender group once.
    const SRTSOCKET receiver_a = make_receiver(peers, bound);
    const SRTSOCKET sender = peers.keep(srt_accept(listener, nullptr, nullptr));
    REQUIRE(sender != SRT_INVALID_SOCK);
    REQUIRE((sender & SRTGROUP_MASK) != 0);
    const SRTSOCKET receiver_b = make_receiver(peers, bound);
    const SRTSOCKET receiver_c = make_receiver(peers, bound);
    REQUIRE(wait_for_members(sender, 3));
    REQUIRE_EQ(admissions.multicast_proposals.load(), 3);
    for (const SRTSOCKET receiver : {receiver_a, receiver_b, receiver_c}) {
        REQUIRE(wait_for_members(receiver, 1));
        REQUIRE_EQ(srt_getsockstate(receiver), SRTS_CONNECTED);
        const auto record = GroupRegistry::instance().find(receiver);
        REQUIRE(record != nullptr);
        std::lock_guard lock(record->mutex);
        REQUIRE(record->category == GroupCategory::dispatch);
        REQUIRE(record->dispatch != nullptr);
        REQUIRE(!record->dispatch->sender);
        REQUIRE(record->dispatch->joined);
        REQUIRE(record->dispatch->receive_channel != nullptr);
        REQUIRE_EQ(record->dispatch->ingress_group_id,
            static_cast<std::uint32_t>(sender));
        REQUIRE_EQ(record->dispatch->multicast_group.port, multicast_port);
    }
    {
        const auto record = GroupRegistry::instance().find(sender);
        REQUIRE(record != nullptr);
        std::lock_guard lock(record->mutex);
        REQUIRE(record->category == GroupCategory::dispatch);
        REQUIRE(record->dispatch->sender);
        REQUIRE_EQ(record->dispatch->admission_key, std::string {"camera-1"});
        REQUIRE_EQ(record->members.size(), 3U);
        REQUIRE_EQ(record->admission_domain, 7U);
    }
    // The sender does not receive; a receiver does not send.
    std::array<char, 16> scratch {};
    REQUIRE_EQ(srt_recvmsg(sender, scratch.data(), scratch.size()), SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVOP);
    REQUIRE_EQ(srt_sendmsg(receiver_a, "x", 1, -1, 1), SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVOP);

    // One send, three deliveries, in order.
    constexpr int first_batch = 20;
    for (int index = 0; index < first_batch; ++index) {
        const std::string payload = message(index);
        REQUIRE_EQ(srt_sendmsg(sender, payload.data(),
                       static_cast<int>(payload.size()), -1, 1),
            static_cast<int>(payload.size()));
    }
    for (const SRTSOCKET receiver : {receiver_a, receiver_b, receiver_c}) {
        expect_messages(receiver, 0, first_batch);
    }
    // Exactly one member transmits the multicast copy; the others only
    // record the packets. Each message was counted once on the group row.
    {
        const auto record = GroupRegistry::instance().find(sender);
        std::vector<SRTSOCKET> members;
        {
            std::lock_guard lock(record->mutex);
            for (const auto& member : record->members) {
                members.push_back(member.public_data.id);
            }
        }
        int redirecting = 0;
        int suppressed = 0;
        for (const SRTSOCKET member : members) {
            const auto socket = SocketRegistry::instance().find(member);
            REQUIRE(socket != nullptr);
            std::shared_ptr<robotweax::srt::compat::ConnectionRuntime> runtime;
            {
                std::lock_guard lock(socket->mutex);
                runtime = socket->runtime;
            }
            REQUIRE(runtime != nullptr);
            const Egress egress = runtime->group_data_egress();
            if (egress.mode == Egress::Mode::redirect) {
                ++redirecting;
                REQUIRE_EQ(egress.destination_socket_id,
                    static_cast<std::uint32_t>(sender));
                REQUIRE_EQ(egress.destination.port, multicast_port);
            } else if (egress.mode == Egress::Mode::suppress) {
                ++suppressed;
            }
        }
        REQUIRE_EQ(redirecting, 1);
        REQUIRE_EQ(suppressed, 2);
        SRT_TRACEBSTATS statistics {};
        REQUIRE_EQ(srt_bstats(sender, &statistics, 0), 0);
        REQUIRE_EQ(statistics.pktSentUniqueTotal,
            static_cast<std::int64_t>(first_batch));
    }

    // A receiver leaves: the sender stays connected, the others continue,
    // and a late joiner picks up the live stream.
    peers.drop(receiver_a);
    REQUIRE(wait_for_members(sender, 2));
    REQUIRE_EQ(srt_getsockstate(sender), SRTS_CONNECTED);
    constexpr int second_batch = 10;
    for (int index = first_batch; index < first_batch + second_batch; ++index) {
        const std::string payload = message(index);
        REQUIRE_EQ(srt_sendmsg(sender, payload.data(),
                       static_cast<int>(payload.size()), -1, 1),
            static_cast<int>(payload.size()));
    }
    expect_messages(receiver_b, first_batch, second_batch);
    expect_messages(receiver_c, first_batch, second_batch);

    const SRTSOCKET receiver_d = make_receiver(peers, bound);
    REQUIRE(wait_for_members(sender, 3));
    REQUIRE(wait_for_members(receiver_d, 1));
    // Let the joiner's TSBPD clock settle before the first live packet.
    std::this_thread::sleep_for(std::chrono::milliseconds {50});
    constexpr int third_batch = 10;
    const int third_first = first_batch + second_batch;
    for (int index = third_first; index < third_first + third_batch; ++index) {
        const std::string payload = message(index);
        REQUIRE_EQ(srt_sendmsg(sender, payload.data(),
                       static_cast<int>(payload.size()), -1, 1),
            static_cast<int>(payload.size()));
    }
    expect_messages(receiver_b, third_first, third_batch);
    expect_messages(receiver_c, third_first, third_batch);
    expect_messages(receiver_d, third_first, third_batch);

    // Every receiver gone: the sender keeps its stream open and keeps
    // accepting payload, which a bonding group would refuse as ENOCONN.
    peers.drop(receiver_b);
    peers.drop(receiver_c);
    peers.drop(receiver_d);
    REQUIRE(wait_for_members(sender, 0));
    REQUIRE_EQ(srt_getsockstate(sender), SRTS_CONNECTED);
    const std::string idle = message(999);
    REQUIRE_EQ(
        srt_sendmsg(sender, idle.data(), static_cast<int>(idle.size()), -1, 1),
        static_cast<int>(idle.size()));
}

TEST(multicast_dispatch_listener_rejects_joins_without_key_or_as_bonding)
{
    SKIP_UNLESS(loopback_multicast_available(),
        "no loopback multicast on this host");
    Peers peers;
    const SRTSOCKET listener = peers.keep(srt_create_socket());
    const sockaddr_in group_address =
        ipv4_sockaddr({239, 255, 133, 11}, multicast_port);
    const sockaddr_in interface = ipv4_sockaddr({127, 0, 0, 1}, 0);
    REQUIRE_EQ(srt_setsockflag(listener, SRTO_ROBOTWEAX_MCAST_GROUP,
                   &group_address, sizeof(group_address)),
        0);
    REQUIRE_EQ(srt_setsockflag(listener, SRTO_ROBOTWEAX_MCAST_IFACE,
                   &interface, sizeof(interface)),
        0);
    // Domain set, key not set: the join must be refused.
    REQUIRE_EQ(srt_listen_callback(
                   listener,
                   [](void*, SRTSOCKET socket, int, const sockaddr*,
                       const char*) {
                       const std::uint64_t domain = 7;
                       return srt_setsockflag(socket,
                           SRTO_ROBOTWEAX_GROUPDOMAIN, &domain, sizeof(domain));
                   },
                   nullptr),
        0);
    sockaddr_in bound = ipv4_sockaddr({127, 0, 0, 1}, 0);
    REQUIRE_EQ(srt_bind(listener, reinterpret_cast<const sockaddr*>(&bound),
                   sizeof(bound)),
        0);
    REQUIRE_EQ(srt_listen(listener, 4), 0);
    int bound_size = sizeof(bound);
    REQUIRE_EQ(srt_getsockname(listener, reinterpret_cast<sockaddr*>(&bound),
                   &bound_size),
        0);
    const std::int32_t connect_timeout = 1'500;

    {
        const SRTSOCKET receiver =
            peers.keep(srt_create_group(ROBOTWEAX_SRT_GTYPE_MULTICAST));
        REQUIRE_EQ(srt_setsockflag(receiver, SRTO_CONNTIMEO, &connect_timeout,
                       sizeof(connect_timeout)),
            0);
        SRT_SOCKGROUPCONFIG endpoint = srt_prepare_endpoint(nullptr,
            reinterpret_cast<const sockaddr*>(&bound), sizeof(bound));
        REQUIRE_EQ(srt_connect_group(receiver, &endpoint, 1), SRT_ERROR);
        REQUIRE_EQ(srt_getsockstate(receiver), SRTS_BROKEN);
    }
    // A bonding group on a dispatch-only listener (no SRTO_GROUPCONNECT) is
    // refused as well; a dispatch listener does not become a bonding one.
    {
        const SRTSOCKET bonding =
            peers.keep(srt_create_group(SRT_GTYPE_BROADCAST));
        REQUIRE_EQ(srt_setsockflag(bonding, SRTO_CONNTIMEO, &connect_timeout,
                       sizeof(connect_timeout)),
            0);
        SRT_SOCKGROUPCONFIG endpoint = srt_prepare_endpoint(nullptr,
            reinterpret_cast<const sockaddr*>(&bound), sizeof(bound));
        REQUIRE_EQ(srt_connect_group(bonding, &endpoint, 1), SRT_ERROR);
    }
    // An encrypted dispatch listener is refused at admission: one shared
    // multicast copy needs one group key, which this release does not have.
    {
        const SRTSOCKET encrypted = peers.keep(srt_create_socket());
        REQUIRE_EQ(srt_setsockflag(encrypted, SRTO_ROBOTWEAX_MCAST_GROUP,
                       &group_address, sizeof(group_address)),
            0);
        REQUIRE_EQ(srt_setsockflag(encrypted, SRTO_ROBOTWEAX_MCAST_IFACE,
                       &interface, sizeof(interface)),
            0);
        const char passphrase[] = "sixteen-char-key";
        REQUIRE_EQ(srt_setsockflag(encrypted, SRTO_PASSPHRASE, passphrase,
                       static_cast<int>(sizeof(passphrase) - 1U)),
            0);
        REQUIRE_EQ(srt_listen_callback(encrypted, admit_dispatch_plain, nullptr),
            0);
        sockaddr_in encrypted_bound = ipv4_sockaddr({127, 0, 0, 1}, 0);
        REQUIRE_EQ(srt_bind(encrypted,
                       reinterpret_cast<const sockaddr*>(&encrypted_bound),
                       sizeof(encrypted_bound)),
            0);
        REQUIRE_EQ(srt_listen(encrypted, 4), 0);
        int encrypted_size = sizeof(encrypted_bound);
        REQUIRE_EQ(srt_getsockname(encrypted,
                       reinterpret_cast<sockaddr*>(&encrypted_bound),
                       &encrypted_size),
            0);
        const SRTSOCKET receiver =
            peers.keep(srt_create_group(ROBOTWEAX_SRT_GTYPE_MULTICAST));
        REQUIRE_EQ(srt_setsockflag(receiver, SRTO_PASSPHRASE, passphrase,
                       static_cast<int>(sizeof(passphrase) - 1U)),
            0);
        REQUIRE_EQ(srt_setsockflag(receiver, SRTO_CONNTIMEO, &connect_timeout,
                       sizeof(connect_timeout)),
            0);
        SRT_SOCKGROUPCONFIG endpoint = srt_prepare_endpoint(nullptr,
            reinterpret_cast<const sockaddr*>(&encrypted_bound),
            sizeof(encrypted_bound));
        REQUIRE_EQ(srt_connect_group(receiver, &endpoint, 1), SRT_ERROR);
    }
    // The option itself is rejected outside a multicast admission.
    const SRTSOCKET plain = peers.keep(srt_create_socket());
    REQUIRE_EQ(srt_setsockflag(plain, SRTO_ROBOTWEAX_MULTICAST, "k", 1),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);
    const sockaddr_in unicast = ipv4_sockaddr({10, 1, 2, 3}, 5'000);
    REQUIRE_EQ(srt_setsockflag(plain, SRTO_ROBOTWEAX_MCAST_GROUP, &unicast,
                   sizeof(unicast)),
        SRT_ERROR);
    const std::int32_t bad_hops = 256;
    REQUIRE_EQ(srt_setsockflag(plain, SRTO_ROBOTWEAX_MCAST_TTL, &bad_hops,
                   sizeof(bad_hops)),
        SRT_ERROR);
}
