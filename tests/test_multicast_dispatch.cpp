// End-to-end multicast dispatch groups over loopback multicast: one Listener
// sender, several Caller receivers, control on unicast, payload once to the
// group. The administratively scoped group 239.255.133.0/24 with hop limit 0
// and the loopback interface keeps every datagram on this host.
#include "test.hpp"

#include "compat/group_registry.hpp"
#include "compat/socket_registry.hpp"
#include "compat/transport_runtime.hpp"
#include "robotweax/srt/codec.hpp"
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

TEST(multicast_dispatch_repair_does_not_suppress_before_submission)
{
    robotweax::srt::compat::DispatchTransmitState state;
    const SequenceNumber sequence {42};
    const auto attempted = state.note_repeat(sequence, 1, 100, 1, 100'000);
    REQUIRE(attempted.multicast);
    // The elected multicast repair has not been submitted yet.
    const auto next = state.note_repeat(sequence, 2, 101, 1, 100'000);
    REQUIRE(!next.skip);
    REQUIRE(!next.multicast);
    state.complete_repeat(sequence, attempted.attempt, false);
    const auto failed = state.note_repeat(sequence, 3, 102, 1, 100'000);
    REQUIRE(!failed.skip);
    REQUIRE(!failed.multicast);
}

TEST(multicast_dispatch_repair_repeated_nak_uses_unicast)
{
    robotweax::srt::compat::DispatchTransmitState state;
    const SequenceNumber sequence {42};
    const auto attempted = state.note_repeat(sequence, 1, 100, 1, 100'000);
    REQUIRE(attempted.multicast);
    state.complete_repeat(sequence, attempted.attempt, true);
    // A repeated request must not wait for the shared tally to expire.
    const auto retry = state.note_repeat(sequence, 1, 101, 1, 100'000);
    REQUIRE(!retry.skip);
    REQUIRE(!retry.multicast);
    REQUIRE(state.note_repeat(sequence, 2, 102, 1, 100'000).skip);
    const auto suppressed_retry =
        state.note_repeat(sequence, 2, 103, 1, 100'000);
    REQUIRE(!suppressed_retry.skip);
    REQUIRE(!suppressed_retry.multicast);
}

TEST(multicast_dispatch_repair_completion_cannot_mutate_a_new_tally)
{
    robotweax::srt::compat::DispatchTransmitState state;
    const SequenceNumber sequence {SequenceNumber::mask};
    const auto old = state.note_repeat(sequence, 1, 100, 1, 10'000);
    const auto expired = state.note_repeat(sequence, 1, 10'101, 1, 10'000);
    REQUIRE(old.multicast);
    REQUIRE(!expired.multicast && !expired.skip);
    // A colliding ring slot and a wrapped sequence must not inherit completion.
    const SequenceNumber collision {sequence.value() - 1'024};
    const auto replacement = state.note_repeat(collision, 4, 10'104, 1, 10'000);
    REQUIRE(replacement.multicast);
    state.complete_repeat(sequence, old.attempt, true);
    REQUIRE(!state.note_repeat(collision, 5, 10'105, 1, 10'000).skip);
    const auto reinserted = state.note_repeat(sequence, 1, 10'106, 1, 10'000);
    REQUIRE(reinserted.multicast);
    state.complete_repeat(sequence, old.attempt, true);
    REQUIRE(!state.note_repeat(sequence, 2, 10'107, 1, 10'000).skip);
    const auto wrapped =
        state.note_repeat(sequence.next(), 6, 10'106, 1, 10'000);
    REQUIRE(wrapped.multicast);
    state.complete_repeat(sequence.next(), wrapped.attempt, true);
    REQUIRE(state.note_repeat(sequence.next(), 7, 10'107, 1, 10'000).skip);
    REQUIRE(!state.note_repeat(sequence.next(), 8, 10'108, 0, 10'000).skip);
}

TEST(multicast_dispatch_repair_member_capacity_preserves_unicast_fallback)
{
    robotweax::srt::compat::DispatchTransmitState state;
    const SequenceNumber sequence {42};
    const auto repair = state.note_repeat(sequence, 0, 100, 1, 100'000);
    state.complete_repeat(sequence, repair.attempt, true);
    for (int member = 1; member < 16; ++member) {
        REQUIRE(state.note_repeat(sequence, member, 101, 1, 100'000).skip);
    }
    const auto overflow = state.note_repeat(sequence, 16, 102, 1, 100'000);
    REQUIRE(!overflow.skip && !overflow.multicast);
    REQUIRE(!state.note_repeat(sequence, 16, 103, 1, 100'000).skip);
    REQUIRE(!state.note_repeat(sequence, 1, 104, 1, 100'000).skip);
}

TEST(multicast_dispatch_repair_threshold_counts_distinct_receivers_in_window)
{
    for (const std::uint64_t window : {10'000U, 100'000U, 1'000'000U}) {
        for (const std::uint32_t threshold : {2U, 16U}) {
            robotweax::srt::compat::DispatchTransmitState state;
            const SequenceNumber sequence {42};
            for (std::uint32_t member = 0; member < threshold; ++member) {
                const auto first = state.note_repeat(sequence,
                    static_cast<std::int32_t>(member), 100 + member, threshold,
                    window);
                REQUIRE_EQ(first.multicast, member + 1 == threshold);
                REQUIRE(!first.skip);
                const auto repeated = state.note_repeat(sequence,
                    static_cast<std::int32_t>(member), 100 + member, threshold,
                    window);
                REQUIRE(!repeated.multicast && !repeated.skip);
                if (first.multicast) {
                    state.complete_repeat(sequence, first.attempt, true);
                }
            }
            const auto expired = state.note_repeat(
                sequence, 100, 101 + window, threshold, window);
            REQUIRE(!expired.multicast && !expired.skip);
            // Before any multicast attempt, expiry starts a fresh tally.
            const SequenceNumber next = sequence.next();
            REQUIRE(!state.note_repeat(next, 1, 100, 2, window).multicast);
            REQUIRE(
                !state.note_repeat(next, 2, 101 + window, 2, window).multicast);
            REQUIRE(
                state.note_repeat(next, 3, 102 + window, 2, window).multicast);
        }
    }
}

namespace {

constexpr std::uint16_t multicast_port = 25'999;

[[nodiscard]] sockaddr_in ipv4_sockaddr(
    const std::array<std::uint8_t, 4>& address, std::uint16_t port) noexcept
{
    sockaddr_in result {};
    result.sin_family = AF_INET;
    result.sin_port = htons(port);
    result.sin_addr.s_addr =
        htonl((static_cast<std::uint32_t>(address[0]) << 24U)
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
        || receiver.join_multicast(
               {.address = {239, 255, 133, 12}}, IpEndpoint::loopback())
            != Error::none
        || sender.bind(IpEndpoint::loopback()) != Error::none
        || sender.set_multicast_interface(IpEndpoint::loopback()) != Error::none
        || sender.set_multicast_time_to_live(0) != Error::none) {
        return false;
    }
    const std::array<std::byte, 1> probe {std::byte {1}};
    if (!sender.send_to(
            probe, {.address = {239, 255, 133, 12}, .port = multicast_port})) {
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
        handles.erase(
            std::remove(handles.begin(), handles.end(), handle), handles.end());
        (void)srt_close(handle);
    }
};

struct AdmissionLog {
    std::atomic<int> calls {0};
    std::atomic<int> multicast_proposals {0};
};

int admit_dispatch(
    void* opaque, SRTSOCKET socket, int, const sockaddr*, const char*)
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
    if (srt_setsockflag(
            socket, SRTO_ROBOTWEAX_GROUPDOMAIN, &domain, sizeof(domain))
        != 0) {
        return -1;
    }
    const char key[] = "camera-1";
    return srt_setsockflag(socket, SRTO_ROBOTWEAX_MULTICAST, key,
        static_cast<int>(sizeof(key) - 1U));
}

// Admits any multicast proposal under a fixed key; used where the listener
// configuration itself is what must refuse the join.
int admit_dispatch_plain(
    void*, SRTSOCKET socket, int, const sockaddr*, const char*)
{
    const std::uint64_t domain = 7;
    if (srt_setsockflag(
            socket, SRTO_ROBOTWEAX_GROUPDOMAIN, &domain, sizeof(domain))
        != 0) {
        return -1;
    }
    const char key[] = "camera-1";
    return srt_setsockflag(socket, SRTO_ROBOTWEAX_MULTICAST, key,
        static_cast<int>(sizeof(key) - 1U));
}

// A member is usable once its socket is CONNECTED and its runtime attached.
[[nodiscard]] bool wait_for_members(SRTSOCKET group, std::size_t expected,
    bool keyed = false,
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
            bool attached = false;
            {
                std::lock_guard lock(socket->mutex);
                attached = socket->state == SRTS_CONNECTED
                    && socket->runtime != nullptr;
            }
            if (!attached) {
                continue;
            }
            if (keyed) {
                // A sender member is usable once its receiver acknowledged
                // the group key; before that its DATA would be unreadable.
                std::int32_t state = -1;
                int size = sizeof(state);
                if (srt_getsockflag(member, SRTO_SNDKMSTATE, &state, &size) != 0
                    || state != SRT_KM_S_SECURED) {
                    continue;
                }
            }
            ++ready;
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

constexpr const char group_passphrase[] = "sixteen-char-key";

[[nodiscard]] SRTSOCKET make_receiver(Peers& peers, const sockaddr_in& listener,
    bool encrypted = false, bool aes_gcm = false)
{
    const SRTSOCKET group =
        peers.keep(srt_create_group(ROBOTWEAX_SRT_GTYPE_MULTICAST));
    REQUIRE(group != SRT_INVALID_SOCK);
    if (encrypted) {
        REQUIRE_EQ(srt_setsockflag(group, SRTO_PASSPHRASE, group_passphrase,
                       static_cast<int>(sizeof(group_passphrase) - 1U)),
            0);
    }
#ifdef ENABLE_AEAD_API_PREVIEW
    if (aes_gcm) {
        const std::int32_t gcm = 2;
        REQUIRE_EQ(
            srt_setsockflag(group, SRTO_CRYPTOMODE, &gcm, sizeof(gcm)), 0);
    }
#else
    (void)aes_gcm;
#endif
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

struct StreamScenario {
    bool encrypted = false;
    // Messages per batch; the encrypted run uses a refresh rate small
    // enough that several key generations pass inside one batch.
    int first_batch = 20;
    int second_batch = 10;
    int third_batch = 10;
    std::int32_t refresh_rate = 0;
    std::int32_t preannouncement = 0;
    // Gap between sends. A key generation is selected by group position, so
    // the pre-announcement window must cover the KMREQ/KMRSP round trip at
    // the stream's packet rate; a wire-speed burst defeats a 16-packet
    // window even on loopback, as it would for any SRT receiver.
    std::chrono::microseconds send_gap {0};
    bool aes_gcm = false;
};

void run_dispatch_stream(const StreamScenario& scenario)
{
    Peers peers;
    AdmissionLog admissions;

    // Listener as dispatch sender on loopback.
    const SRTSOCKET listener = peers.keep(srt_create_socket());
    REQUIRE(listener != SRT_INVALID_SOCK);
    const sockaddr_in group_address =
        ipv4_sockaddr({239, 255, 133, 11}, multicast_port);
    const sockaddr_in interface = ipv4_sockaddr({127, 0, 0, 1}, 0);
    const std::int32_t hops = 0;
    if (scenario.encrypted) {
        REQUIRE_EQ(srt_setsockflag(listener, SRTO_PASSPHRASE, group_passphrase,
                       static_cast<int>(sizeof(group_passphrase) - 1U)),
            0);
#ifdef ENABLE_AEAD_API_PREVIEW
        if (scenario.aes_gcm) {
            const std::int32_t gcm = 2;
            REQUIRE_EQ(
                srt_setsockflag(listener, SRTO_CRYPTOMODE, &gcm, sizeof(gcm)),
                0);
        }
#endif
        if (scenario.refresh_rate != 0) {
            REQUIRE_EQ(srt_setsockflag(listener, SRTO_KMPREANNOUNCE,
                           &scenario.preannouncement,
                           sizeof(scenario.preannouncement)),
                0);
            REQUIRE_EQ(
                srt_setsockflag(listener, SRTO_KMREFRESHRATE,
                    &scenario.refresh_rate, sizeof(scenario.refresh_rate)),
                0);
        }
    }
    REQUIRE_EQ(srt_setsockflag(listener, SRTO_ROBOTWEAX_MCAST_GROUP,
                   &group_address, sizeof(group_address)),
        0);
    REQUIRE_EQ(srt_setsockflag(listener, SRTO_ROBOTWEAX_MCAST_IFACE, &interface,
                   sizeof(interface)),
        0);
    REQUIRE_EQ(srt_setsockflag(
                   listener, SRTO_ROBOTWEAX_MCAST_TTL, &hops, sizeof(hops)),
        0);
    sockaddr_in readback {};
    int readback_size = sizeof(readback);
    REQUIRE_EQ(srt_getsockflag(listener, SRTO_ROBOTWEAX_MCAST_GROUP, &readback,
                   &readback_size),
        0);
    REQUIRE_EQ(readback.sin_port, group_address.sin_port);
    REQUIRE_EQ(srt_listen_callback(listener, admit_dispatch, &admissions), 0);
    sockaddr_in bound = ipv4_sockaddr({127, 0, 0, 1}, 0);
    REQUIRE_EQ(srt_bind(listener, reinterpret_cast<const sockaddr*>(&bound),
                   sizeof(bound)),
        0);
    REQUIRE_EQ(srt_listen(listener, 8), 0);
    int bound_size = sizeof(bound);
    REQUIRE_EQ(srt_getsockname(
                   listener, reinterpret_cast<sockaddr*>(&bound), &bound_size),
        0);

    // Three receivers join; srt_accept yields the sender group once.
    const SRTSOCKET receiver_a =
        make_receiver(peers, bound, scenario.encrypted, scenario.aes_gcm);
    const SRTSOCKET sender = peers.keep(srt_accept(listener, nullptr, nullptr));
    REQUIRE(sender != SRT_INVALID_SOCK);
    REQUIRE((sender & SRTGROUP_MASK) != 0);
    const SRTSOCKET receiver_b =
        make_receiver(peers, bound, scenario.encrypted, scenario.aes_gcm);
    const SRTSOCKET receiver_c =
        make_receiver(peers, bound, scenario.encrypted, scenario.aes_gcm);
    REQUIRE(wait_for_members(sender, 3, scenario.encrypted));
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
        REQUIRE_EQ(
            record->dispatch->key_schedule != nullptr, scenario.encrypted);
    }
    // The sender does not receive; a receiver does not send.
    std::array<char, 16> scratch {};
    REQUIRE_EQ(srt_recvmsg(sender, scratch.data(), scratch.size()), SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVOP);
    REQUIRE_EQ(srt_sendmsg(receiver_a, "x", 1, -1, 1), SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVOP);

    // One send, three deliveries, in order.
    const int first_batch = scenario.first_batch;
    for (int index = 0; index < first_batch; ++index) {
        const std::string payload = message(index);
        REQUIRE_EQ(srt_sendmsg(sender, payload.data(),
                       static_cast<int>(payload.size()), -1, 1),
            static_cast<int>(payload.size()));
        if (scenario.send_gap.count() != 0) {
            std::this_thread::sleep_for(scenario.send_gap);
        }
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
    REQUIRE(wait_for_members(sender, 2, scenario.encrypted));
    REQUIRE_EQ(srt_getsockstate(sender), SRTS_CONNECTED);
    const int second_batch = scenario.second_batch;
    for (int index = first_batch; index < first_batch + second_batch; ++index) {
        const std::string payload = message(index);
        REQUIRE_EQ(srt_sendmsg(sender, payload.data(),
                       static_cast<int>(payload.size()), -1, 1),
            static_cast<int>(payload.size()));
        if (scenario.send_gap.count() != 0) {
            std::this_thread::sleep_for(scenario.send_gap);
        }
    }
    expect_messages(receiver_b, first_batch, second_batch);
    expect_messages(receiver_c, first_batch, second_batch);

    const SRTSOCKET receiver_d =
        make_receiver(peers, bound, scenario.encrypted, scenario.aes_gcm);
    REQUIRE(wait_for_members(sender, 3, scenario.encrypted));
    REQUIRE(wait_for_members(receiver_d, 1));
    // Let the joiner's TSBPD clock settle (and, encrypted, its initial key
    // exchange complete) before the first live packet.
    std::this_thread::sleep_for(
        std::chrono::milliseconds {scenario.encrypted ? 200 : 50});
    const int third_batch = scenario.third_batch;
    const int third_first = first_batch + second_batch;
    for (int index = third_first; index < third_first + third_batch; ++index) {
        const std::string payload = message(index);
        REQUIRE_EQ(srt_sendmsg(sender, payload.data(),
                       static_cast<int>(payload.size()), -1, 1),
            static_cast<int>(payload.size()));
        if (scenario.send_gap.count() != 0) {
            std::this_thread::sleep_for(scenario.send_gap);
        }
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

namespace {

// Drops the multicast original of one group sequence so every receiver
// loses the same packet, then forwards everything else unchanged.
struct DropOneOriginal {
    std::shared_ptr<robotweax::srt::compat::DatagramChannel> channel;
    IpEndpoint multicast {};
    std::atomic<std::int32_t> drop_sequence {-1};
    std::atomic<int> dropped {0};
    std::atomic<int> multicast_data {0};
    std::atomic<int> multicast_repeats {0};
    bool lose_multicast_repair = false;
};

UdpIoResult drop_one_original(
    std::span<const std::byte> bytes, IpEndpoint peer, void* context) noexcept
{
    auto& hook = *static_cast<DropOneOriginal*>(context);
    if (peer == hook.multicast) {
        const auto packet = decode_packet(bytes);
        if (packet && packet.packet.kind == PacketKind::data) {
            ++hook.multicast_data;
            if (packet.packet.data.retransmitted) {
                ++hook.multicast_repeats;
                if (hook.lose_multicast_repair) {
                    return {.bytes_transferred = bytes.size()};
                }
            } else if (hook.drop_sequence.load() >= 0
                && packet.packet.data.sequence.value()
                    == static_cast<std::uint32_t>(hook.drop_sequence.load())) {
                ++hook.dropped;
                return {.bytes_transferred = bytes.size()};
            }
        }
    }
    return hook.channel->socket.send_to(bytes, peer);
}

} // namespace

void run_dispatch_shared_loss(bool lose_multicast_repair)
{
    SKIP_UNLESS(
        loopback_multicast_available(), "no loopback multicast on this host");
    Peers peers;
    AdmissionLog admissions;
    const SRTSOCKET listener = peers.keep(srt_create_socket());
    const sockaddr_in group_address =
        ipv4_sockaddr({239, 255, 133, 11}, multicast_port);
    const sockaddr_in interface = ipv4_sockaddr({127, 0, 0, 1}, 0);
    const std::int32_t hops = 0;
    REQUIRE_EQ(srt_setsockflag(listener, SRTO_ROBOTWEAX_MCAST_GROUP,
                   &group_address, sizeof(group_address)),
        0);
    REQUIRE_EQ(srt_setsockflag(listener, SRTO_ROBOTWEAX_MCAST_IFACE, &interface,
                   sizeof(interface)),
        0);
    REQUIRE_EQ(srt_setsockflag(
                   listener, SRTO_ROBOTWEAX_MCAST_TTL, &hops, sizeof(hops)),
        0);
    REQUIRE_EQ(srt_listen_callback(listener, admit_dispatch, &admissions), 0);
    sockaddr_in bound = ipv4_sockaddr({127, 0, 0, 1}, 0);
    REQUIRE_EQ(srt_bind(listener, reinterpret_cast<const sockaddr*>(&bound),
                   sizeof(bound)),
        0);
    REQUIRE_EQ(srt_listen(listener, 8), 0);
    int bound_size = sizeof(bound);
    REQUIRE_EQ(srt_getsockname(
                   listener, reinterpret_cast<sockaddr*>(&bound), &bound_size),
        0);
    DropOneOriginal hook;
    hook.lose_multicast_repair = lose_multicast_repair;
    {
        const auto record = SocketRegistry::instance().find(listener);
        REQUIRE(record != nullptr);
        std::lock_guard lock(record->mutex);
        hook.channel = record->channel;
    }
    REQUIRE(hook.channel != nullptr);
    hook.multicast = {.address = {239, 255, 133, 11}, .port = multicast_port};
    hook.channel->set_send_hook_for_testing(drop_one_original, &hook);
    struct UnhookOnExit {
        DropOneOriginal& hook;
        ~UnhookOnExit()
        {
            hook.channel->set_send_hook_for_testing(nullptr, nullptr);
        }
    } unhook {hook};

    const SRTSOCKET receiver_a = make_receiver(peers, bound);
    const SRTSOCKET sender = peers.keep(srt_accept(listener, nullptr, nullptr));
    REQUIRE(sender != SRT_INVALID_SOCK);
    const SRTSOCKET receiver_b = make_receiver(peers, bound);
    const SRTSOCKET receiver_c = make_receiver(peers, bound);
    REQUIRE(wait_for_members(sender, 3));
    for (const SRTSOCKET receiver : {receiver_a, receiver_b, receiver_c}) {
        REQUIRE(wait_for_members(receiver, 1));
    }

    // The option belongs to the dispatch sender only.
    std::int32_t threshold = 2;
    REQUIRE_EQ(
        srt_setsockflag(receiver_a, SRTO_ROBOTWEAX_MCAST_REXMIT_THRESHOLD,
            &threshold, sizeof(threshold)),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVOP);
    const std::int32_t too_many = 17;
    REQUIRE_EQ(srt_setsockflag(sender, SRTO_ROBOTWEAX_MCAST_REXMIT_THRESHOLD,
                   &too_many, sizeof(too_many)),
        SRT_ERROR);
    REQUIRE_EQ(srt_setsockflag(sender, SRTO_ROBOTWEAX_MCAST_REXMIT_THRESHOLD,
                   &threshold, sizeof(threshold)),
        0);
    std::int32_t readback = -1;
    int readback_size = sizeof(readback);
    REQUIRE_EQ(srt_getsockflag(sender, SRTO_ROBOTWEAX_MCAST_REXMIT_THRESHOLD,
                   &readback, &readback_size),
        0);
    REQUIRE_EQ(readback, 2);

    // Drop the fifth packet's multicast original for everyone.
    {
        const auto record = GroupRegistry::instance().find(sender);
        std::lock_guard lock(record->mutex);
        hook.drop_sequence = static_cast<std::int32_t>(
            SequenceNumber {record->next_send_sequence}.advanced(4U).value());
    }
    constexpr int count = 12;
    for (int index = 0; index < count; ++index) {
        const std::string payload = message(index);
        REQUIRE_EQ(srt_sendmsg(sender, payload.data(),
                       static_cast<int>(payload.size()), -1, 1),
            static_cast<int>(payload.size()));
        std::this_thread::sleep_for(std::chrono::milliseconds {2});
    }
    for (const SRTSOCKET receiver : {receiver_a, receiver_b, receiver_c}) {
        expect_messages(receiver, 0, count);
    }
    REQUIRE_EQ(hook.dropped.load(), 1);
    // Three receivers requested the same repeat: the first was answered on
    // its unicast link, the second reached the threshold and was answered
    // once on the multicast path, the third was already covered.
    std::shared_ptr<robotweax::srt::compat::DispatchTransmitState> transmit;
    {
        const auto record = GroupRegistry::instance().find(sender);
        std::lock_guard lock(record->mutex);
        transmit = record->dispatch->transmit;
    }
    REQUIRE(transmit != nullptr);
    REQUIRE_EQ(transmit->multicast_repeats(), 1U);
    REQUIRE_EQ(hook.multicast_repeats.load(), 1);
    if (!lose_multicast_repair) {
        REQUIRE_EQ(transmit->skipped_repeats(), 1U);
    }
    REQUIRE_EQ(
        transmit->multicast_packets(), static_cast<std::uint64_t>(count + 1));
    REQUIRE_EQ(hook.multicast_data.load(), count + 1);

    // The same counters through the public read-only option, on the sender
    // and on a receiver. A stray datagram on the group port that does not
    // carry the dispatch ID is counted as filtered, never delivered.
    ROBOTWEAX_SRT_MCAST_STATS_V1 stats {};
    int stats_size = sizeof(stats);
    REQUIRE_EQ(srt_getsockflag(
                   sender, SRTO_ROBOTWEAX_MCAST_STATS, &stats, &stats_size),
        0);
    REQUIRE_EQ(stats.abi_version, ROBOTWEAX_SRT_MCAST_STATS_VERSION);
    REQUIRE_EQ(stats.member_count, 3U);
    REQUIRE_EQ(stats.mcast_pkt_sent, static_cast<std::uint64_t>(count + 1));
    REQUIRE_EQ(stats.mcast_pkt_rexmit, 1U);
    REQUIRE_EQ(stats.unicast_rexmit_skipped, transmit->skipped_repeats());
    REQUIRE(stats.mcast_byte_sent > 0U);
    REQUIRE_EQ(stats.mcast_pkt_received, 0U);
    // Read-only, and absent on bonding groups.
    REQUIRE_EQ(
        srt_setsockflag(sender, SRTO_ROBOTWEAX_MCAST_STATS, &stats, stats_size),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVOP);
    {
        const SRTSOCKET bonding =
            peers.keep(srt_create_group(SRT_GTYPE_BROADCAST));
        stats_size = sizeof(stats);
        REQUIRE_EQ(srt_getsockflag(bonding, SRTO_ROBOTWEAX_MCAST_STATS, &stats,
                       &stats_size),
            SRT_ERROR);
    }
    {
        UdpSocket stray;
        REQUIRE(stray.valid());
        REQUIRE_EQ(
            stray.set_multicast_interface(IpEndpoint::loopback()), Error::none);
        REQUIRE_EQ(stray.set_multicast_time_to_live(0), Error::none);
        // A well-formed DATA header addressed to socket 0x1234, from a
        // foreign source: no route, so it is filtered before decoding.
        std::array<std::byte, 20> datagram {};
        datagram[15] = std::byte {0x34};
        datagram[14] = std::byte {0x12};
        REQUIRE(stray.send_to(datagram, hook.multicast));
        std::this_thread::sleep_for(std::chrono::milliseconds {50});
    }
    const auto receiver_stats = [&](SRTSOCKET receiver) {
        ROBOTWEAX_SRT_MCAST_STATS_V1 result {};
        int size = sizeof(result);
        REQUIRE_EQ(srt_getsockflag(
                       receiver, SRTO_ROBOTWEAX_MCAST_STATS, &result, &size),
            0);
        return result;
    };
    const auto a = receiver_stats(receiver_a);
    REQUIRE_EQ(a.member_count, 1U);
    REQUIRE_EQ(a.mcast_pkt_sent, 0U);
    // count originals minus the dropped one, plus the multicast repeat and
    // the stray datagram.
    REQUIRE_EQ(a.mcast_pkt_received,
        static_cast<std::uint64_t>(count + (lose_multicast_repair ? 0 : 1)));
    REQUIRE_EQ(a.mcast_pkt_filtered, 1U);
    // The stray datagram never reached the application.
    std::array<char, 64> none {};
    const std::int32_t short_timeout = 150;
    REQUIRE_EQ(srt_setsockflag(receiver_a, SRTO_RCVTIMEO, &short_timeout,
                   sizeof(short_timeout)),
        0);
    REQUIRE_EQ(srt_recvmsg(receiver_a, none.data(), none.size()), SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ETIMEOUT);
}

TEST(multicast_dispatch_group_repeats_a_shared_loss_once_over_multicast)
{
    run_dispatch_shared_loss(false);
}

TEST(
    multicast_dispatch_group_recovers_when_original_and_multicast_repair_are_lost)
{
    run_dispatch_shared_loss(true);
}

TEST(multicast_dispatch_group_delivers_one_stream_to_several_receivers)
{
    SKIP_UNLESS(
        loopback_multicast_available(), "no loopback multicast on this host");
    run_dispatch_stream({});
}

#ifdef ENABLE_AEAD_API_PREVIEW
TEST(multicast_dispatch_group_authenticates_with_aes_gcm_group_key)
{
    SKIP_UNLESS(
        loopback_multicast_available(), "no loopback multicast on this host");
    // Every DATA packet of a dispatch member names the group as its
    // destination, so the authenticated header is identical on the
    // multicast copy and on a unicast repeat.
    run_dispatch_stream({
        .encrypted = true,
        .first_batch = 120,
        .second_batch = 20,
        .third_batch = 20,
        .refresh_rate = 64,
        .preannouncement = 24,
        .send_gap = std::chrono::microseconds {500},
        .aes_gcm = true,
    });
}
#endif

TEST(multicast_dispatch_group_encrypts_with_one_group_key_across_rotations)
{
    SKIP_UNLESS(
        loopback_multicast_available(), "no loopback multicast on this host");
    // Refresh every 64 group positions, announce 16 ahead: the first batch
    // crosses three key generations, the late joiner arrives after them and
    // must still read the live generation it is handed at its own exchange.
    run_dispatch_stream({
        .encrypted = true,
        .first_batch = 200,
        .second_batch = 40,
        .third_batch = 40,
        .refresh_rate = 64,
        .preannouncement = 24,
        .send_gap = std::chrono::microseconds {500},
    });
}

TEST(multicast_dispatch_listener_rejects_joins_without_key_or_as_bonding)
{
    SKIP_UNLESS(
        loopback_multicast_available(), "no loopback multicast on this host");
    Peers peers;
    const SRTSOCKET listener = peers.keep(srt_create_socket());
    const sockaddr_in group_address =
        ipv4_sockaddr({239, 255, 133, 11}, multicast_port);
    const sockaddr_in interface = ipv4_sockaddr({127, 0, 0, 1}, 0);
    REQUIRE_EQ(srt_setsockflag(listener, SRTO_ROBOTWEAX_MCAST_GROUP,
                   &group_address, sizeof(group_address)),
        0);
    REQUIRE_EQ(srt_setsockflag(listener, SRTO_ROBOTWEAX_MCAST_IFACE, &interface,
                   sizeof(interface)),
        0);
    // Domain set, key not set: the join must be refused.
    REQUIRE_EQ(
        srt_listen_callback(
            listener,
            [](void*, SRTSOCKET socket, int, const sockaddr*, const char*) {
                const std::uint64_t domain = 7;
                return srt_setsockflag(socket, SRTO_ROBOTWEAX_GROUPDOMAIN,
                    &domain, sizeof(domain));
            },
            nullptr),
        0);
    sockaddr_in bound = ipv4_sockaddr({127, 0, 0, 1}, 0);
    REQUIRE_EQ(srt_bind(listener, reinterpret_cast<const sockaddr*>(&bound),
                   sizeof(bound)),
        0);
    REQUIRE_EQ(srt_listen(listener, 4), 0);
    int bound_size = sizeof(bound);
    REQUIRE_EQ(srt_getsockname(
                   listener, reinterpret_cast<sockaddr*>(&bound), &bound_size),
        0);
    const std::int32_t connect_timeout = 1'500;

    {
        const SRTSOCKET receiver =
            peers.keep(srt_create_group(ROBOTWEAX_SRT_GTYPE_MULTICAST));
        REQUIRE_EQ(srt_setsockflag(receiver, SRTO_CONNTIMEO, &connect_timeout,
                       sizeof(connect_timeout)),
            0);
        SRT_SOCKGROUPCONFIG endpoint = srt_prepare_endpoint(
            nullptr, reinterpret_cast<const sockaddr*>(&bound), sizeof(bound));
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
        SRT_SOCKGROUPCONFIG endpoint = srt_prepare_endpoint(
            nullptr, reinterpret_cast<const sockaddr*>(&bound), sizeof(bound));
        REQUIRE_EQ(srt_connect_group(bonding, &endpoint, 1), SRT_ERROR);
    }
    // Encryption is all-or-nothing per dispatch group: a receiver without
    // the passphrase is refused by an encrypted sender.
    {
        const SRTSOCKET encrypted = peers.keep(srt_create_socket());
        REQUIRE_EQ(srt_setsockflag(encrypted, SRTO_ROBOTWEAX_MCAST_GROUP,
                       &group_address, sizeof(group_address)),
            0);
        REQUIRE_EQ(srt_setsockflag(encrypted, SRTO_ROBOTWEAX_MCAST_IFACE,
                       &interface, sizeof(interface)),
            0);
        REQUIRE_EQ(srt_setsockflag(encrypted, SRTO_PASSPHRASE, group_passphrase,
                       static_cast<int>(sizeof(group_passphrase) - 1U)),
            0);
        REQUIRE_EQ(
            srt_listen_callback(encrypted, admit_dispatch_plain, nullptr), 0);
        sockaddr_in encrypted_bound = ipv4_sockaddr({127, 0, 0, 1}, 0);
        REQUIRE_EQ(srt_bind(encrypted,
                       reinterpret_cast<const sockaddr*>(&encrypted_bound),
                       sizeof(encrypted_bound)),
            0);
        REQUIRE_EQ(srt_listen(encrypted, 4), 0);
        int encrypted_size = sizeof(encrypted_bound);
        REQUIRE_EQ(
            srt_getsockname(encrypted,
                reinterpret_cast<sockaddr*>(&encrypted_bound), &encrypted_size),
            0);
        const SRTSOCKET receiver =
            peers.keep(srt_create_group(ROBOTWEAX_SRT_GTYPE_MULTICAST));
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
    REQUIRE_EQ(
        srt_setsockflag(plain, SRTO_ROBOTWEAX_MULTICAST, "k", 1), SRT_ERROR);
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
