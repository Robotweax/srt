// End-to-end multicast dispatch groups over loopback multicast: one Listener
// sender, several Caller receivers, control on unicast, payload once to the
// group. The administratively scoped group 239.255.133.0/24 with hop limit 0
// and the loopback interface keeps every datagram on this host.
#include "test.hpp"

#include "compat/group_registry.hpp"
#include "compat/group_send_coordinator.hpp"
#include "compat/runtime_scheduler_service.hpp"
#include "compat/socket_registry.hpp"
#include "compat/transport_runtime.hpp"
#include "robotweax/srt/codec.hpp"
#include "robotweax/srt/udp.hpp"
#include "srt/srt.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <ifaddrs.h>
#include <net/if.h>
#endif

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

    // Model all three requests arriving before UDP accepts the elected
    // repair. The third request must get unicast, not premature suppression.
    const SequenceNumber shared {43};
    REQUIRE(!state.note_repeat(shared, 1, 200, 2, 100'000).multicast);
    const auto elected = state.note_repeat(shared, 2, 201, 2, 100'000);
    REQUIRE(elected.multicast);
    const auto pending = state.note_repeat(shared, 3, 202, 2, 100'000);
    REQUIRE(!pending.multicast && !pending.skip);
    state.complete_repeat(shared, elected.attempt, true);
    const auto retry = state.note_repeat(shared, 3, 203, 2, 100'000);
    REQUIRE(!retry.multicast && !retry.skip);
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
    bool encrypted = false, bool aes_gcm = false, int latency_ms = 120,
    bool independent = false, bool asynchronous = false)
{
    const SRTSOCKET group =
        peers.keep(srt_create_group(ROBOTWEAX_SRT_GTYPE_MULTICAST));
    REQUIRE(group != SRT_INVALID_SOCK);
    if (independent)
        REQUIRE_EQ(srt_setsockflag(group, SRTO_ROBOTWEAX_MCAST_INDEPENDENT,
                       &independent, sizeof(independent)),
            0);
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
    REQUIRE_EQ(
        srt_setsockflag(group, SRTO_LATENCY, &latency_ms, sizeof(latency_ms)),
        0);
    const std::int32_t receive_timeout = 3'000;
    REQUIRE_EQ(srt_setsockflag(group, SRTO_RCVTIMEO, &receive_timeout,
                   sizeof(receive_timeout)),
        0);
    const sockaddr_in interface = ipv4_sockaddr({127, 0, 0, 1}, 0);
    REQUIRE_EQ(srt_setsockflag(group, SRTO_ROBOTWEAX_MCAST_IFACE, &interface,
                   sizeof(interface)),
        0);
    if (asynchronous) {
        const bool nonblocking = false;
        REQUIRE_EQ(srt_setsockflag(
                       group, SRTO_RCVSYN, &nonblocking, sizeof(nonblocking)),
            0);
    }
    SRT_SOCKGROUPCONFIG endpoint = srt_prepare_endpoint(nullptr,
        reinterpret_cast<const sockaddr*>(&listener), sizeof(listener));
    REQUIRE(srt_connect_group(group, &endpoint, 1) != SRT_ERROR);
    if (asynchronous) {
        REQUIRE(wait_for_members(group, 1));
        const bool blocking = true;
        REQUIRE_EQ(
            srt_setsockflag(group, SRTO_RCVSYN, &blocking, sizeof(blocking)),
            0);
    }
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
    bool fill_accept_backlog = false;
    bool multicast_shutdown_probe = false;
    bool independent = false;
    std::array<std::uint8_t, 4> control_address {127, 0, 0, 1};
    bool asynchronous_receiver = false;
    bool coordinated = false;
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
    if (scenario.independent) {
        REQUIRE_EQ(srt_setsockflag(listener, SRTO_ROBOTWEAX_MCAST_INDEPENDENT,
                       &scenario.independent, sizeof(scenario.independent)),
            0);
        const auto source = ipv4_sockaddr({127, 0, 0, 1}, 0);
        REQUIRE_EQ(srt_setsockflag(listener, SRTO_ROBOTWEAX_MCAST_SOURCE,
                       &source, sizeof(source)),
            0);
    }
    if (scenario.coordinated) {
        const std::int64_t rate = 1'000'000;
        REQUIRE_EQ(
            srt_setsockflag(listener, SRTO_MAXBW, &rate, sizeof(rate)), 0);
        REQUIRE_EQ(srt_setsockflag(listener, SRTO_ROBOTWEAX_MCAST_COORDINATED,
                       &scenario.coordinated, sizeof(scenario.coordinated)),
            0);
    }
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
    sockaddr_in bound = ipv4_sockaddr(scenario.control_address, 0);
    REQUIRE_EQ(srt_bind(listener, reinterpret_cast<const sockaddr*>(&bound),
                   sizeof(bound)),
        0);
    REQUIRE_EQ(srt_listen(listener, scenario.fill_accept_backlog ? 1 : 8), 0);
    int bound_size = sizeof(bound);
    REQUIRE_EQ(srt_getsockname(
                   listener, reinterpret_cast<sockaddr*>(&bound), &bound_size),
        0);

    if (scenario.independent) {
        sockaddr_in source {};
        int source_size = sizeof(source);
        REQUIRE_EQ(srt_getsockflag(listener, SRTO_ROBOTWEAX_MCAST_SOURCE,
                       &source, &source_size),
            0);
        REQUIRE(source.sin_port != 0);
        REQUIRE(source.sin_port != bound.sin_port
            || source.sin_addr.s_addr != bound.sin_addr.s_addr);
        const auto record = SocketRegistry::instance().find(listener);
        std::lock_guard lock(record->mutex);
        REQUIRE(record->multicast_transmit_channel);
        REQUIRE(record->multicast_transmit_channel != record->channel);
        REQUIRE(!record->multicast_transmit_channel->running());
    }
    // Three receivers join; srt_accept yields the sender group once.
    const SRTSOCKET receiver_a =
        make_receiver(peers, bound, scenario.encrypted, scenario.aes_gcm, 120,
            scenario.independent, scenario.asynchronous_receiver);
    SRTSOCKET sender = SRT_INVALID_SOCK;
    if (!scenario.fill_accept_backlog) {
        sender = peers.keep(srt_accept(listener, nullptr, nullptr));
        REQUIRE(sender != SRT_INVALID_SOCK);
    }
    const SRTSOCKET receiver_b =
        make_receiver(peers, bound, scenario.encrypted, scenario.aes_gcm, 120,
            scenario.independent, scenario.asynchronous_receiver);
    const SRTSOCKET receiver_c =
        make_receiver(peers, bound, scenario.encrypted, scenario.aes_gcm, 120,
            scenario.independent, scenario.asynchronous_receiver);
    if (scenario.fill_accept_backlog) {
        const auto listener_record = SocketRegistry::instance().find(listener);
        REQUIRE(listener_record);
        REQUIRE_EQ(listener_record->pending_accepts.load(), 1U);
        sender = peers.keep(srt_accept(listener, nullptr, nullptr));
        REQUIRE(sender != SRT_INVALID_SOCK);
    }
    REQUIRE((sender & SRTGROUP_MASK) != 0);
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
        REQUIRE_EQ(record->dispatch->transmit_channel != nullptr,
            scenario.independent);
        REQUIRE_EQ(record->dispatch->admission_key, std::string {"camera-1"});
        REQUIRE_EQ(record->members.size(), 3U);
        REQUIRE_EQ(record->admission_domain, 7U);
        REQUIRE_EQ(
            record->dispatch->key_schedule != nullptr, scenario.encrypted);
    }
    if (scenario.multicast_shutdown_probe) {
        // Use the actual listener socket so the packet has the expected source
        // endpoint. Only ingress provenance can reject this control packet.
        const auto listener_record = SocketRegistry::instance().find(listener);
        REQUIRE(listener_record);
        std::shared_ptr<robotweax::srt::compat::DatagramChannel> channel;
        {
            std::lock_guard lock(listener_record->mutex);
            channel = listener_record->channel;
        }
        REQUIRE(channel);
        MutablePacketView shutdown;
        shutdown.kind = PacketKind::control;
        shutdown.control.type = ControlType::shutdown;
        shutdown.control.destination_socket_id =
            static_cast<std::uint32_t>(sender);
        const std::array<std::byte, 4> payload {};
        shutdown.payload = payload;
        std::array<std::byte, 64> bytes {};
        const auto encoded = encode_packet(shutdown, bytes);
        REQUIRE(encoded);
        REQUIRE(channel->send_datagram(
            std::span {bytes}.first(encoded.bytes_written),
            IpEndpoint {
                .address = {239, 255, 133, 11}, .port = multicast_port}));
        for (const auto receiver : {receiver_a, receiver_b, receiver_c}) {
            const auto record = GroupRegistry::instance().find(receiver);
            std::shared_ptr<robotweax::srt::compat::DatagramChannel> ingress;
            {
                std::lock_guard lock(record->mutex);
                ingress = record->dispatch->receive_channel;
            }
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds {2};
            while (ingress->routed_datagrams() == 0
                && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds {2});
            REQUIRE(ingress->routed_datagrams() != 0);
            REQUIRE_EQ(srt_getsockstate(receiver), SRTS_CONNECTED);
        }
    }
    if (scenario.independent) {
        sockaddr_in listener_source {}, group_source {};
        int source_size = sizeof(listener_source);
        REQUIRE_EQ(srt_getsockflag(listener, SRTO_ROBOTWEAX_MCAST_SOURCE,
                       &listener_source, &source_size),
            0);
        source_size = sizeof(group_source);
        REQUIRE_EQ(srt_getsockflag(sender, SRTO_ROBOTWEAX_MCAST_SOURCE,
                       &group_source, &source_size),
            0);
        REQUIRE_EQ(listener_source.sin_port, group_source.sin_port);
        REQUIRE_EQ(
            listener_source.sin_addr.s_addr, group_source.sin_addr.s_addr);
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
        int external = 0;
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
            } else if (egress.mode == Egress::Mode::external_originals) {
                ++external;
                REQUIRE_EQ(
                    runtime->statistics(false, true).total.sent_unique.packets,
                    0U);
            }
        }
        REQUIRE_EQ(redirecting, scenario.coordinated ? 0 : 1);
        REQUIRE_EQ(suppressed, scenario.coordinated ? 0 : 2);
        REQUIRE_EQ(external, scenario.coordinated ? 3 : 0);
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
        make_receiver(peers, bound, scenario.encrypted, scenario.aes_gcm, 120,
            scenario.independent, scenario.asynchronous_receiver);
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
    // More than the retained key window passes with no receiver. Repeat
    // the transition to catch a schedule that only repairs the first join.
    for (int cycle = 0; cycle < 2; ++cycle) {
        for (int index = 0; index < 512; ++index) {
            REQUIRE_EQ(srt_sendmsg(sender, idle.data(),
                           static_cast<int>(idle.size()), -1, 1),
                static_cast<int>(idle.size()));
        }
        const auto joined =
            make_receiver(peers, bound, scenario.encrypted, scenario.aes_gcm,
                120, scenario.independent, scenario.asynchronous_receiver);
        REQUIRE(wait_for_members(sender, 1, scenario.encrypted));
        REQUIRE(wait_for_members(joined, 1));
        const auto resumed = message(1000 + cycle);
        REQUIRE_EQ(srt_sendmsg(sender, resumed.data(),
                       static_cast<int>(resumed.size()), -1, 1),
            static_cast<int>(resumed.size()));
        expect_messages(joined, 1000 + cycle, 1);
        peers.drop(joined);
        REQUIRE(wait_for_members(sender, 0));
    }
    if (scenario.independent) {
        std::weak_ptr<robotweax::srt::compat::DatagramChannel> source;
        {
            const auto record = SocketRegistry::instance().find(listener);
            std::lock_guard lock(record->mutex);
            source = record->multicast_transmit_channel;
        }
        peers.drop(sender);
        peers.drop(listener);
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds {2};
        while (!source.expired() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds {2});
        REQUIRE(source.expired());
    }
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
    bool transient_multicast_repair = false;
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
                if (hook.transient_multicast_repair) {
#if defined(_WIN32)
                    return {.error = Error::would_block,
                        .system_error = WSAEWOULDBLOCK};
#else
                    return {
                        .error = Error::would_block, .system_error = EAGAIN};
#endif
                }
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

void run_dispatch_shared_loss(bool lose_multicast_repair,
    bool independent = false, bool transient_multicast_repair = false)
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
    if (independent) {
        const auto source = ipv4_sockaddr({127, 0, 0, 1}, 0);
        REQUIRE_EQ(srt_setsockflag(listener, SRTO_ROBOTWEAX_MCAST_INDEPENDENT,
                       &independent, sizeof(independent)),
            0);
        REQUIRE_EQ(srt_setsockflag(listener, SRTO_ROBOTWEAX_MCAST_SOURCE,
                       &source, sizeof(source)),
            0);
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
    hook.transient_multicast_repair = transient_multicast_repair;
    {
        const auto record = SocketRegistry::instance().find(listener);
        REQUIRE(record != nullptr);
        std::lock_guard lock(record->mutex);
        hook.channel =
            independent ? record->multicast_transmit_channel : record->channel;
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

    const SRTSOCKET receiver_a =
        make_receiver(peers, bound, false, false, 120, independent);
    const SRTSOCKET sender = peers.keep(srt_accept(listener, nullptr, nullptr));
    REQUIRE(sender != SRT_INVALID_SOCK);
    const SRTSOCKET receiver_b =
        make_receiver(peers, bound, false, false, 120, independent);
    const SRTSOCKET receiver_c =
        make_receiver(peers, bound, false, false, 120, independent);
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
    // The second distinct request elects one multicast repeat. The third
    // receiver may recover before requesting, request before UDP acceptance
    // (requiring unicast), or request after acceptance (allowing one skip).
    // Exact suppression ordering is covered by deterministic state tests.
    std::shared_ptr<robotweax::srt::compat::DispatchTransmitState> transmit;
    {
        const auto record = GroupRegistry::instance().find(sender);
        std::lock_guard lock(record->mutex);
        transmit = record->dispatch->transmit;
    }
    REQUIRE(transmit != nullptr);
    const std::uint64_t accepted_repeats = transient_multicast_repair ? 0U : 1U;
    REQUIRE_EQ(transmit->multicast_repeats(), accepted_repeats);
    REQUIRE_EQ(hook.multicast_repeats.load(), 1);
    if (!lose_multicast_repair) {
        REQUIRE(transmit->skipped_repeats() <= 1U);
    }
    REQUIRE_EQ(transmit->multicast_packets(),
        static_cast<std::uint64_t>(count) + accepted_repeats);
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
    REQUIRE_EQ(stats.mcast_pkt_sent,
        static_cast<std::uint64_t>(count) + accepted_repeats);
    REQUIRE_EQ(stats.mcast_pkt_rexmit, accepted_repeats);
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
        static_cast<std::uint64_t>(count
            + ((lose_multicast_repair || transient_multicast_repair) ? 0 : 1)));
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

TEST(multicast_dispatch_group_joins_with_a_full_accept_backlog)
{
    SKIP_UNLESS(
        loopback_multicast_available(), "no loopback multicast on this host");
    run_dispatch_stream({.fill_accept_backlog = true});
}

TEST(multicast_dispatch_multicast_control_does_not_close_receivers)
{
    SKIP_UNLESS(
        loopback_multicast_available(), "no loopback multicast on this host");
    run_dispatch_stream({.multicast_shutdown_probe = true});
}

// Investigation-only wire rerouting; no production-library changes.
namespace {
struct HybridWireProbe {
    std::shared_ptr<robotweax::srt::compat::DatagramChannel> channel;
    IpEndpoint unicast;
    bool isolated = false;
    std::atomic<int> phase {
        0}; // 0 multicast, 1 blackhole, 2 unicast, 3 overlap
    std::atomic<int> dropped {0};
    std::atomic<int> repairs {0};
    std::atomic<int> redirected {0};
};
UdpIoResult hybrid_wire_probe(
    std::span<const std::byte> bytes, IpEndpoint peer, void* context) noexcept
{
    auto& probe = *static_cast<HybridWireProbe*>(context);
    auto decoded = decode_packet(bytes);
    if (decoded && decoded.packet.kind == PacketKind::data) {
        if (!peer.is_multicast())
            ++probe.repairs;
        if (peer.is_multicast()) {
            auto phase = probe.phase.load();
            if (phase == 1) {
                ++probe.dropped;
                if (!probe.isolated)
                    return {.bytes_transferred = bytes.size()};
            }
            if (phase >= 2) {
                ++probe.redirected;
                auto result =
                    probe.channel->socket.send_to(bytes, probe.unicast);
                if ((phase == 2 && !probe.isolated) || !result)
                    return result;
            }
        }
    }
    return probe.channel->socket.send_to(bytes, peer);
}
void investigate_hybrid_wire(bool encrypted, bool gcm, bool isolated = false)
{
    SKIP_UNLESS(loopback_multicast_available(), "no loopback multicast");
    Peers peers;
    AdmissionLog admissions;
    auto listener = peers.keep(srt_create_socket());
    auto group_address = ipv4_sockaddr({239, 255, 133, 11}, multicast_port);
    auto interface = ipv4_sockaddr({127, 0, 0, 1}, 0);
    std::int32_t hops = 0;
    if (encrypted) {
        REQUIRE_EQ(srt_setsockflag(listener, SRTO_PASSPHRASE, group_passphrase,
                       sizeof(group_passphrase) - 1),
            0);
        if (gcm) {
            std::int32_t mode = 2;
            REQUIRE_EQ(
                srt_setsockflag(listener, SRTO_CRYPTOMODE, &mode, sizeof(mode)),
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
    REQUIRE_EQ(srt_listen_callback(listener, admit_dispatch, &admissions), 0);
    auto bound = interface;
    REQUIRE_EQ(
        srt_bind(listener, reinterpret_cast<sockaddr*>(&bound), sizeof(bound)),
        0);
    REQUIRE_EQ(srt_listen(listener, 8), 0);
    int size = sizeof(bound);
    REQUIRE_EQ(
        srt_getsockname(listener, reinterpret_cast<sockaddr*>(&bound), &size),
        0);
    auto receiver = make_receiver(peers, bound, encrypted, gcm, 2000);
    auto sender = peers.keep(srt_accept(listener, nullptr, nullptr));
    REQUIRE(sender != SRT_INVALID_SOCK);
    REQUIRE(wait_for_members(sender, 1, encrypted));
    REQUIRE(wait_for_members(receiver, 1));
    SRTSOCKET healthy = SRT_INVALID_SOCK;
    if (isolated) {
        healthy = make_receiver(peers, bound, encrypted, gcm, 2000);
        REQUIRE(wait_for_members(sender, 2, encrypted));
        REQUIRE(wait_for_members(healthy, 1));
    }
    HybridWireProbe probe;
    probe.isolated = isolated;
    std::shared_ptr<robotweax::srt::compat::DatagramChannel> ingress;
    std::shared_ptr<robotweax::srt::compat::ConnectionRuntime> runtime;
    std::uint32_t route_id = 0;
    if (isolated) {
        auto r = GroupRegistry::instance().find(receiver);
        SRTSOCKET m;
        {
            std::lock_guard lock(r->mutex);
            ingress = r->dispatch->receive_channel;
            route_id = r->dispatch->ingress_group_id;
            m = r->members.front().public_data.id;
        }
        auto socket = SocketRegistry::instance().find(m);
        {
            std::lock_guard lock(socket->mutex);
            runtime = socket->runtime;
        }
    }
    auto listener_record = SocketRegistry::instance().find(listener);
    {
        std::lock_guard lock(listener_record->mutex);
        probe.channel = listener_record->channel;
    }
    auto group_record = GroupRegistry::instance().find(sender);
    SRTSOCKET member;
    {
        std::lock_guard lock(group_record->mutex);
        member = group_record->members.front().public_data.id;
    }
    auto member_record = SocketRegistry::instance().find(member);
    {
        std::lock_guard lock(member_record->mutex);
        probe.unicast = member_record->peer_endpoint;
    }
    probe.channel->set_send_hook_for_testing(hybrid_wire_probe, &probe);
    struct Cleanup {
        HybridWireProbe& p;
        ~Cleanup()
        {
            p.channel->set_send_hook_for_testing(nullptr, nullptr);
        }
    } cleanup {probe};
    auto send = [&](int i) {
        auto payload = message(i);
        REQUIRE_EQ(srt_sendmsg(sender, payload.data(), payload.size(), -1, 1),
            static_cast<int>(payload.size()));
        std::this_thread::sleep_for(std::chrono::milliseconds {10});
    };
    send(0);
    send(1);
    if (isolated)
        ingress->unregister_connection(route_id);
    probe.phase = 1;
    for (int i = 2; i < 8; ++i)
        send(i);
    std::this_thread::sleep_for(std::chrono::milliseconds {350});
    REQUIRE_EQ(probe.dropped.load(), 6);
    REQUIRE_EQ(probe.repairs.load(), 0);
    REQUIRE_EQ(srt_getsockstate(receiver), SRTS_CONNECTED);
    // An externally forced unicast original supplies a new sequence horizon.
    probe.phase = 2;
    for (int i = 8; i < 16; ++i)
        send(i);
    if (isolated)
        REQUIRE(ingress->register_ingress_route(route_id, runtime,
            robotweax::srt::compat::PacketIngress::multicast_data));
    probe.phase = 3;
    for (int i = 16; i < 20; ++i)
        send(i);
    probe.phase = 0;
    for (int i = 20; i < 24; ++i)
        send(i);
    expect_messages(receiver, 0, 24);
    if (isolated)
        expect_messages(healthy, 0, 24);
    REQUIRE(probe.repairs.load() >= 6);
    REQUIRE_EQ(probe.redirected.load(), 12);
    const bool nonblocking = false;
    REQUIRE_EQ(srt_setsockflag(
                   receiver, SRTO_RCVSYN, &nonblocking, sizeof(nonblocking)),
        0);
    std::array<char, 256> output {};
    REQUIRE_EQ(srt_recvmsg(receiver, output.data(), output.size()), SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EASYNCRCV);
}
}
TEST(investigation_hybrid_wire_plain)
{
    investigate_hybrid_wire(false, false);
}
TEST(investigation_hybrid_wire_ctr)
{
    investigate_hybrid_wire(true, false);
}
#ifdef ENABLE_AEAD_API_PREVIEW
TEST(investigation_hybrid_wire_gcm)
{
    investigate_hybrid_wire(true, true);
}
#endif

TEST(investigation_hybrid_one_receiver_falls_back_while_other_keeps_multicast)
{
    investigate_hybrid_wire(false, false, true);
}

TEST(multicast_dispatch_independent_source_port_distributes_and_rejoins)
{
    SKIP_UNLESS(
        loopback_multicast_available(), "no loopback multicast on this host");
    run_dispatch_stream({.independent = true});
}

TEST(multicast_dispatch_independent_source_ip_distributes_and_rejoins)
{
    SKIP_UNLESS(
        loopback_multicast_available(), "no loopback multicast on this host");
    std::optional<std::array<std::uint8_t, 4>> address;
#if !defined(_WIN32)
    ifaddrs* interfaces = nullptr;
    if (getifaddrs(&interfaces) == 0) {
        std::unique_ptr<ifaddrs, decltype(&freeifaddrs)> cleanup {
            interfaces, freeifaddrs};
        for (auto* entry = interfaces; entry != nullptr;
            entry = entry->ifa_next) {
            if (entry->ifa_addr == nullptr
                || entry->ifa_addr->sa_family != AF_INET
                || (entry->ifa_flags & IFF_UP) == 0)
                continue;
            std::array<std::uint8_t, 4> candidate {};
            std::memcpy(candidate.data(),
                &reinterpret_cast<const sockaddr_in*>(entry->ifa_addr)
                    ->sin_addr,
                4);
            if (candidate[0] == 0 || candidate[0] == 127)
                continue;
            UdpSocket probe;
            if (probe.bind(IpEndpoint {.address = {candidate[0], candidate[1],
                                           candidate[2], candidate[3]}})
                == Error::none) {
                address = candidate;
                break;
            }
        }
    }
#endif
    SKIP_UNLESS(
        address.has_value(), "no second assigned IPv4 address available");
    // Control stays on an already assigned local address. Multicast remains
    // on loopback with hop limit zero; no host interface is changed.
    run_dispatch_stream({.independent = true, .control_address = *address});
}

TEST(multicast_dispatch_independent_ctr_preserves_group_rotations)
{
    SKIP_UNLESS(
        loopback_multicast_available(), "no loopback multicast on this host");
    run_dispatch_stream({.encrypted = true,
        .first_batch = 200,
        .second_batch = 40,
        .third_batch = 40,
        .refresh_rate = 64,
        .preannouncement = 24,
        .send_gap = std::chrono::microseconds {500},
        .independent = true});
}

#ifdef ENABLE_AEAD_API_PREVIEW
TEST(multicast_dispatch_independent_gcm_preserves_group_rotations)
{
    SKIP_UNLESS(
        loopback_multicast_available(), "no loopback multicast on this host");
    run_dispatch_stream({.encrypted = true,
        .first_batch = 120,
        .second_batch = 20,
        .third_batch = 20,
        .refresh_rate = 64,
        .preannouncement = 24,
        .send_gap = std::chrono::microseconds {500},
        .aes_gcm = true,
        .independent = true});
}
#endif

TEST(multicast_dispatch_independent_shared_loss_uses_dedicated_repair_source)
{
    run_dispatch_shared_loss(false, true);
}

TEST(multicast_dispatch_independent_lost_multicast_repair_retries_on_unicast)
{
    run_dispatch_shared_loss(true, true);
}

TEST(multicast_dispatch_independent_options_fail_closed_and_freeze_at_bind)
{
    Peers peers;
    const bool enabled = true;
    for (const auto type : {SRT_GTYPE_BROADCAST, SRT_GTYPE_BACKUP}) {
        const auto group = peers.keep(srt_create_group(type));
        REQUIRE_EQ(srt_setsockflag(group, SRTO_ROBOTWEAX_MCAST_INDEPENDENT,
                       &enabled, sizeof(enabled)),
            SRT_ERROR);
        REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVOP);
    }
    const auto receiver =
        peers.keep(srt_create_group(ROBOTWEAX_SRT_GTYPE_MULTICAST));
    const auto source = ipv4_sockaddr({127, 0, 0, 1}, 0);
    REQUIRE_EQ(srt_setsockflag(receiver, SRTO_ROBOTWEAX_MCAST_SOURCE, &source,
                   sizeof(source)),
        SRT_ERROR);
    const auto listener = peers.keep(srt_create_socket());
    bool actual = true;
    int size = sizeof(actual);
    REQUIRE_EQ(srt_getsockflag(
                   listener, SRTO_ROBOTWEAX_MCAST_INDEPENDENT, &actual, &size),
        0);
    REQUIRE(!actual);
    sockaddr_in6 ipv6 {};
    ipv6.sin6_family = AF_INET6;
    ipv6.sin6_addr.s6_addr[15] = 1;
    REQUIRE_EQ(srt_setsockflag(
                   listener, SRTO_ROBOTWEAX_MCAST_SOURCE, &ipv6, sizeof(ipv6)),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVOP);
    const auto caller = peers.keep(srt_create_socket());
    REQUIRE_EQ(srt_setsockflag(caller, SRTO_ROBOTWEAX_MCAST_INDEPENDENT,
                   &enabled, sizeof(enabled)),
        0);
    const auto peer = ipv4_sockaddr({127, 0, 0, 1}, 9000);
    REQUIRE_EQ(srt_connect(caller, reinterpret_cast<const sockaddr*>(&peer),
                   sizeof(peer)),
        SRT_ERROR);
    REQUIRE_EQ(srt_getsockstate(caller), SRTS_INIT);
    const auto any = ipv4_sockaddr({0, 0, 0, 0}, 0);
    REQUIRE_EQ(srt_setsockflag(
                   listener, SRTO_ROBOTWEAX_MCAST_SOURCE, &any, sizeof(any)),
        SRT_ERROR);
    REQUIRE_EQ(srt_setsockflag(listener, SRTO_ROBOTWEAX_MCAST_INDEPENDENT,
                   &enabled, sizeof(enabled)),
        0);
    REQUIRE_EQ(srt_setsockflag(listener, SRTO_ROBOTWEAX_MCAST_SOURCE, &source,
                   sizeof(source)),
        0);
    REQUIRE_EQ(srt_bind(listener, reinterpret_cast<const sockaddr*>(&source),
                   sizeof(source)),
        0);
    REQUIRE_EQ(srt_setsockflag(listener, SRTO_ROBOTWEAX_MCAST_INDEPENDENT,
                   &enabled, sizeof(enabled)),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EBOUNDSOCK);
    REQUIRE_EQ(srt_setsockflag(listener, SRTO_ROBOTWEAX_MCAST_SOURCE, &source,
                   sizeof(source)),
        SRT_ERROR);
    // Incomplete profile: no multicast destination. No transmitter is published.
    REQUIRE_EQ(srt_listen(listener, 1), SRT_ERROR);
    REQUIRE_EQ(srt_getsockstate(listener), SRTS_OPENED);
    const auto record = SocketRegistry::instance().find(listener);
    std::lock_guard lock(record->mutex);
    REQUIRE(!record->multicast_transmit_channel);
}

TEST(
    multicast_dispatch_independent_bind_failure_rolls_back_and_close_releases_source)
{
    Peers peers;
    auto occupied = std::make_unique<UdpSocket>();
    REQUIRE_EQ(occupied->bind(IpEndpoint::loopback()), Error::none);
    const auto endpoint = occupied->local_endpoint();
    REQUIRE(endpoint);
    const auto source = ipv4_sockaddr({127, 0, 0, 1}, endpoint.endpoint.port);
    const auto listener = peers.keep(srt_create_socket());
    const bool enabled = true;
    const auto group = ipv4_sockaddr({239, 255, 133, 11}, multicast_port);
    const auto local = ipv4_sockaddr({127, 0, 0, 1}, 0);
    REQUIRE_EQ(srt_setsockflag(listener, SRTO_ROBOTWEAX_MCAST_INDEPENDENT,
                   &enabled, sizeof(enabled)),
        0);
    REQUIRE_EQ(srt_setsockflag(listener, SRTO_ROBOTWEAX_MCAST_SOURCE, &source,
                   sizeof(source)),
        0);
    REQUIRE_EQ(srt_setsockflag(
                   listener, SRTO_ROBOTWEAX_MCAST_GROUP, &group, sizeof(group)),
        0);
    REQUIRE_EQ(srt_bind(listener, reinterpret_cast<const sockaddr*>(&local),
                   sizeof(local)),
        0);
    REQUIRE_EQ(srt_listen(listener, 1), SRT_ERROR);
    REQUIRE_EQ(srt_getsockstate(listener), SRTS_OPENED);
    occupied.reset();
    REQUIRE_EQ(srt_listen(listener, 1), 0);
    const std::array bonded {listener, listener};
    REQUIRE_EQ(
        srt_accept_bond(bonded.data(), static_cast<int>(bonded.size()), 0),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVOP);
    std::weak_ptr<robotweax::srt::compat::DatagramChannel> channel;
    {
        const auto record = SocketRegistry::instance().find(listener);
        std::lock_guard lock(record->mutex);
        channel = record->multicast_transmit_channel;
        REQUIRE(!channel.expired());
    }
    peers.drop(listener);
    REQUIRE(channel.expired());
    UdpSocket rebound;
    REQUIRE_EQ(rebound.bind(endpoint.endpoint), Error::none);
}

TEST(multicast_dispatch_independent_one_sided_selection_is_rejected)
{
    SKIP_UNLESS(
        loopback_multicast_available(), "no loopback multicast on this host");
    for (const bool listener_requires : {false, true}) {
        Peers peers;
        AdmissionLog admissions;
        const auto listener = peers.keep(srt_create_socket());
        const auto group = ipv4_sockaddr({239, 255, 133, 11}, multicast_port);
        auto local = ipv4_sockaddr({127, 0, 0, 1}, 0);
        const std::int32_t hops = 0;
        REQUIRE_EQ(srt_setsockflag(listener, SRTO_ROBOTWEAX_MCAST_GROUP, &group,
                       sizeof(group)),
            0);
        REQUIRE_EQ(srt_setsockflag(listener, SRTO_ROBOTWEAX_MCAST_IFACE, &local,
                       sizeof(local)),
            0);
        REQUIRE_EQ(srt_setsockflag(
                       listener, SRTO_ROBOTWEAX_MCAST_TTL, &hops, sizeof(hops)),
            0);
        if (listener_requires) {
            REQUIRE_EQ(
                srt_setsockflag(listener, SRTO_ROBOTWEAX_MCAST_INDEPENDENT,
                    &listener_requires, sizeof(listener_requires)),
                0);
            REQUIRE_EQ(srt_setsockflag(listener, SRTO_ROBOTWEAX_MCAST_SOURCE,
                           &local, sizeof(local)),
                0);
        }
        const bool nonblocking = false;
        REQUIRE_EQ(srt_setsockflag(listener, SRTO_RCVSYN, &nonblocking,
                       sizeof(nonblocking)),
            0);
        REQUIRE_EQ(
            srt_listen_callback(listener, admit_dispatch, &admissions), 0);
        REQUIRE_EQ(srt_bind(listener, reinterpret_cast<const sockaddr*>(&local),
                       sizeof(local)),
            0);
        REQUIRE_EQ(srt_listen(listener, 4), 0);
        int size = sizeof(local);
        REQUIRE_EQ(srt_getsockname(
                       listener, reinterpret_cast<sockaddr*>(&local), &size),
            0);
        const auto receiver =
            peers.keep(srt_create_group(ROBOTWEAX_SRT_GTYPE_MULTICAST));
        const bool caller_requires = !listener_requires;
        REQUIRE_EQ(srt_setsockflag(receiver, SRTO_ROBOTWEAX_MCAST_INDEPENDENT,
                       &caller_requires, sizeof(caller_requires)),
            0);
        const std::int32_t timeout = 1000;
        REQUIRE_EQ(srt_setsockflag(
                       receiver, SRTO_CONNTIMEO, &timeout, sizeof(timeout)),
            0);
        auto endpoint = srt_prepare_endpoint(
            nullptr, reinterpret_cast<const sockaddr*>(&local), sizeof(local));
        REQUIRE_EQ(srt_connect_group(receiver, &endpoint, 1), SRT_ERROR);
        REQUIRE_EQ(srt_accept(listener, nullptr, nullptr), SRT_ERROR);
        REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EASYNCRCV);
    }
}

TEST(multicast_dispatch_independent_async_receiver_negotiates_source)
{
    SKIP_UNLESS(
        loopback_multicast_available(), "no loopback multicast on this host");
    run_dispatch_stream({.independent = true, .asynchronous_receiver = true});
}

TEST(multicast_dispatch_coordinated_plaintext_public_stream_and_lifecycle)
{
    SKIP_UNLESS(
        loopback_multicast_available(), "no loopback multicast on this host");
    run_dispatch_stream({.independent = true, .coordinated = true});
}

namespace {
struct CoordinatedNative {
    Peers peers;
    AdmissionLog admissions;
    SRTSOCKET listener = SRT_INVALID_SOCK;
    SRTSOCKET sender = SRT_INVALID_SOCK;
    SRTSOCKET receiver = SRT_INVALID_SOCK;
    sockaddr_in bound {};
    explicit CoordinatedNative(std::int64_t rate = 1'000'000,
        bool encrypted = false, bool gcm = false, int send_packets = 0,
        int latency = 120)
    {
        listener = peers.keep(srt_create_socket());
        const bool enabled = true;
        auto local = ipv4_sockaddr({127, 0, 0, 1}, 0);
        const auto multicast =
            ipv4_sockaddr({239, 255, 133, 11}, multicast_port);
        const std::int32_t hops = 0;
        REQUIRE_EQ(srt_setsockflag(listener, SRTO_ROBOTWEAX_MCAST_INDEPENDENT,
                       &enabled, sizeof(enabled)),
            0);
        REQUIRE_EQ(srt_setsockflag(listener, SRTO_ROBOTWEAX_MCAST_COORDINATED,
                       &enabled, sizeof(enabled)),
            0);
        REQUIRE_EQ(srt_setsockflag(listener, SRTO_ROBOTWEAX_MCAST_SOURCE,
                       &local, sizeof(local)),
            0);
        REQUIRE_EQ(srt_setsockflag(listener, SRTO_ROBOTWEAX_MCAST_GROUP,
                       &multicast, sizeof(multicast)),
            0);
        REQUIRE_EQ(srt_setsockflag(listener, SRTO_ROBOTWEAX_MCAST_IFACE, &local,
                       sizeof(local)),
            0);
        REQUIRE_EQ(srt_setsockflag(
                       listener, SRTO_ROBOTWEAX_MCAST_TTL, &hops, sizeof(hops)),
            0);
        REQUIRE_EQ(
            srt_setsockflag(listener, SRTO_MAXBW, &rate, sizeof(rate)), 0);
        if (send_packets) {
            const std::int32_t bytes = send_packets * 1472;
            REQUIRE_EQ(
                srt_setsockflag(listener, SRTO_SNDBUF, &bytes, sizeof(bytes)),
                0);
            const bool drop = false;
            REQUIRE_EQ(
                srt_setsockflag(listener, SRTO_TLPKTDROP, &drop, sizeof(drop)),
                0);
            // This fixture suppresses ACKs to exhaust retention, rather than
            // testing the independent peer-idle timeout.
            const std::int32_t idle = 20'000;
            REQUIRE_EQ(srt_setsockflag(
                           listener, SRTO_PEERIDLETIMEO, &idle, sizeof(idle)),
                0);
        }
        if (encrypted) {
            REQUIRE_EQ(srt_setsockflag(listener, SRTO_PASSPHRASE,
                           group_passphrase, sizeof(group_passphrase) - 1),
                0);
            const std::int32_t refresh = 32, preannounce = 8;
            REQUIRE_EQ(srt_setsockflag(listener, SRTO_KMPREANNOUNCE,
                           &preannounce, sizeof(preannounce)),
                0);
            REQUIRE_EQ(srt_setsockflag(listener, SRTO_KMREFRESHRATE, &refresh,
                           sizeof(refresh)),
                0);
#ifdef ENABLE_AEAD_API_PREVIEW
            if (gcm) {
                const std::int32_t mode = 2;
                REQUIRE_EQ(srt_setsockflag(
                               listener, SRTO_CRYPTOMODE, &mode, sizeof(mode)),
                    0);
            }
#endif
        }
        REQUIRE_EQ(
            srt_listen_callback(listener, admit_dispatch, &admissions), 0);
        REQUIRE_EQ(srt_bind(listener, reinterpret_cast<const sockaddr*>(&local),
                       sizeof(local)),
            0);
        REQUIRE_EQ(srt_listen(listener, 8), 0);
        int size = sizeof(bound);
        REQUIRE_EQ(srt_getsockname(
                       listener, reinterpret_cast<sockaddr*>(&bound), &size),
            0);
        receiver = make_receiver(peers, bound, encrypted, gcm, latency, true);
        sender = peers.keep(srt_accept(listener, nullptr, nullptr));
        REQUIRE(sender != SRT_INVALID_SOCK);
        REQUIRE(wait_for_members(sender, 1, encrypted));
        REQUIRE(wait_for_members(receiver, 1));
    }
    std::shared_ptr<robotweax::srt::compat::GroupSendCoordinator> actor()
    {
        const auto record = GroupRegistry::instance().find(sender);
        std::lock_guard lock(record->mutex);
        return record->dispatch->coordinator;
    }
    std::shared_ptr<robotweax::srt::compat::DatagramChannel> source()
    {
        const auto record = SocketRegistry::instance().find(listener);
        std::lock_guard lock(record->mutex);
        return record->multicast_transmit_channel;
    }
    int send(int index)
    {
        const auto p = message(index);
        return srt_sendmsg(sender, p.data(), p.size(), -1, 1);
    }
};
struct SendHookReset {
    std::shared_ptr<robotweax::srt::compat::DatagramChannel> channel;
    ~SendHookReset()
    {
        channel->set_send_hook_for_testing(nullptr, nullptr);
    }
};
}

TEST(
    multicast_dispatch_coordinated_options_freeze_transport_but_allow_group_io_flags)
{
    SKIP_UNLESS(
        loopback_multicast_available(), "no loopback multicast on this host");
    CoordinatedNative f;
    for (const auto handle : {f.listener, f.sender}) {
        bool enabled = false;
        int size = sizeof(enabled);
        REQUIRE_EQ(srt_getsockflag(handle, SRTO_ROBOTWEAX_MCAST_COORDINATED,
                       &enabled, &size),
            0);
        REQUIRE(enabled);
        const std::int64_t rate = 1234;
        REQUIRE_EQ(srt_setsockflag(handle, SRTO_MAXBW, &rate, sizeof(rate)),
            SRT_ERROR);
        REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ECONNSOCK);
        std::int64_t actual = 0;
        size = sizeof(actual);
        REQUIRE_EQ(srt_getsockflag(handle, SRTO_MAXBW, &actual, &size), 0);
        REQUIRE_EQ(actual, 1'000'000);
    }
    const bool yes = true, no = false;
    REQUIRE_EQ(srt_setsockflag(f.receiver, SRTO_ROBOTWEAX_MCAST_COORDINATED,
                   &yes, sizeof(yes)),
        SRT_ERROR);
    std::int32_t threshold = 2;
    REQUIRE_EQ(srt_setsockflag(f.sender, SRTO_ROBOTWEAX_MCAST_REXMIT_THRESHOLD,
                   &threshold, sizeof(threshold)),
        SRT_ERROR);
    REQUIRE_EQ(srt_setsockflag(f.sender, SRTO_SNDSYN, &no, sizeof(no)), 0);
    REQUIRE_EQ(srt_setsockflag(f.sender, SRTO_SNDSYN, &yes, sizeof(yes)), 0);
    const std::int32_t timeout = 500;
    REQUIRE_EQ(
        srt_setsockflag(f.sender, SRTO_SNDTIMEO, &timeout, sizeof(timeout)), 0);
    REQUIRE_EQ(f.send(0), static_cast<int>(message(0).size()));
    expect_messages(f.receiver, 0, 1);
    SRT_SOCKGROUPDATA row[1] {};
    std::size_t count = 1;
    REQUIRE_EQ(srt_group_data(f.sender, row, &count), 1);
    REQUIRE_EQ(srt_sendmsg(row[0].id, "x", 1, -1, 1), SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVOP);
    const std::int64_t changed_rate = 17;
    REQUIRE_EQ(srt_setsockflag(
                   row[0].id, SRTO_MAXBW, &changed_rate, sizeof(changed_rate)),
        SRT_ERROR);
    REQUIRE_EQ(srt_setsockflag(row[0].id, SRTO_SNDSYN, &no, sizeof(no)), 0);
    REQUIRE_EQ(srt_setsockflag(row[0].id, SRTO_SNDSYN, &yes, sizeof(yes)), 0);
    const auto before = f.actor()->snapshot();
    auto control = srt_msgctrl_default;
    control.msgttl = 0;
    control.srctime = 1;
    REQUIRE_EQ(srt_sendmsg2(f.sender, "x", 1, &control), SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ETIMEOUT);
    REQUIRE_EQ(f.actor()->snapshot().next_sequence, before.next_sequence);
    control = srt_msgctrl_default;
    control.grpdata = row;
    control.grpdata_size = 1;
    const auto body = message(1);
    REQUIRE_EQ(srt_sendmsg2(f.sender, body.data(), body.size(), &control),
        static_cast<int>(body.size()));
    REQUIRE_EQ(control.pktseq,
        static_cast<std::int32_t>(before.next_sequence.value()));
    REQUIRE_EQ(control.msgno, static_cast<std::int32_t>(before.next_message));
    REQUIRE_EQ(row[0].memberstate, SRT_GST_RUNNING);
    expect_messages(f.receiver, 1, 1);
    ROBOTWEAX_SRT_MCAST_STATS_V1 stats {};
    int size = sizeof(stats);
    REQUIRE_EQ(
        srt_getsockflag(f.sender, SRTO_ROBOTWEAX_MCAST_STATS, &stats, &size),
        0);
    REQUIRE_EQ(stats.mcast_pkt_sent, 2U);
    REQUIRE_EQ(stats.mcast_byte_sent,
        2U * packet_header_size + message(0).size() + body.size());
}

TEST(
    multicast_dispatch_coordinated_backpressure_epoll_and_close_wake_blocked_send)
{
    SKIP_UNLESS(
        loopback_multicast_available(), "no loopback multicast on this host");
    CoordinatedNative f(1);
    const bool no = false, yes = true;
    REQUIRE_EQ(srt_setsockflag(f.sender, SRTO_SNDSYN, &no, sizeof(no)), 0);
    const int epoll = srt_epoll_create();
    REQUIRE(epoll >= 0);
    struct EpollRelease {
        int id;
        ~EpollRelease()
        {
            (void)srt_epoll_release(id);
        }
    } cleanup {epoll};
    const int out = SRT_EPOLL_OUT;
    REQUIRE_EQ(srt_epoll_add_usock(epoll, f.sender, &out), 0);
    SRT_EPOLL_EVENT events[1] {};
    int accepted = 0;
    const auto fill_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds {2};
    while (accepted < 128) {
        const int sent = f.send(accepted);
        if (sent == SRT_ERROR) {
            REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EASYNCSND);
            if (f.actor()->snapshot().inflight_frames == 64)
                break;
            REQUIRE(std::chrono::steady_clock::now() < fill_deadline);
            REQUIRE_EQ(srt_epoll_uwait(epoll, events, 1, 1000), 1);
            continue;
        }
        REQUIRE_EQ(sent, static_cast<int>(message(accepted).size()));
        ++accepted;
    }
    REQUIRE(accepted >= 64 && accepted < 128);
    REQUIRE(!f.actor()->snapshot().writable);
    REQUIRE_EQ(srt_epoll_uwait(epoll, events, 1, 0), 0);
    REQUIRE_EQ(srt_setsockflag(f.sender, SRTO_SNDSYN, &yes, sizeof(yes)), 0);
    std::future<int> pending;
    struct Unblock {
        CoordinatedNative& f;
        ~Unblock()
        {
            f.peers.drop(f.sender);
        }
    } unblock {f};
    std::promise<void> requested;
    auto started = requested.get_future();
    pending = std::async(std::launch::async, [&] {
        requested.set_value();
        return f.send(129);
    });
    REQUIRE_EQ(
        started.wait_for(std::chrono::seconds {2}), std::future_status::ready);
    REQUIRE_EQ(pending.wait_for(std::chrono::milliseconds {10}),
        std::future_status::timeout);
    f.peers.drop(f.sender);
    REQUIRE_EQ(
        pending.wait_for(std::chrono::seconds {2}), std::future_status::ready);
    REQUIRE_EQ(pending.get(), SRT_ERROR);
}

TEST(
    multicast_dispatch_coordinated_source_failure_preserves_unicast_sessions_and_reports_error)
{
    SKIP_UNLESS(
        loopback_multicast_available(), "no loopback multicast on this host");
    CoordinatedNative f;
    const auto source = f.source();
    SendHookReset cleanup {source};
    source->set_send_hook_for_testing(
        +[](std::span<const std::byte>, IpEndpoint, void*) noexcept {
            return UdpIoResult {.error = Error::io_error};
        },
        nullptr);
    REQUIRE_EQ(f.send(0), static_cast<int>(message(0).size()));
    const int epoll = srt_epoll_create();
    REQUIRE(epoll >= 0);
    const int error = SRT_EPOLL_ERR;
    REQUIRE_EQ(srt_epoll_add_usock(epoll, f.sender, &error), 0);
    SRT_EPOLL_EVENT events[1] {};
    const int ready = srt_epoll_uwait(epoll, events, 1, 2000);
    (void)srt_epoll_release(epoll);
    REQUIRE_EQ(ready, 1);
    REQUIRE(events[0].events & SRT_EPOLL_ERR);
    REQUIRE_EQ(srt_getsockstate(f.sender), SRTS_BROKEN);
    std::int32_t state = 0;
    int size = sizeof(state);
    REQUIRE_EQ(srt_getsockflag(f.sender, SRTO_STATE, &state, &size), 0);
    REQUIRE_EQ(state, SRTS_BROKEN);
    REQUIRE_EQ(f.send(1), SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ESOCKFAIL);
    SRT_SOCKGROUPDATA data[1] {};
    std::size_t count = 1;
    REQUIRE_EQ(srt_group_data(f.sender, data, &count), 1);
    REQUIRE_EQ(data[0].sockstate, SRTS_CONNECTED);
    REQUIRE_EQ(data[0].memberstate, SRT_GST_BROKEN);
    REQUIRE_EQ(data[0].result, SRT_ECONNLOST);
    REQUIRE_EQ(srt_getsockstate(f.receiver), SRTS_CONNECTED);
    ROBOTWEAX_SRT_MCAST_STATS_V1 stats {};
    size = sizeof(stats);
    REQUIRE_EQ(
        srt_getsockflag(f.sender, SRTO_ROBOTWEAX_MCAST_STATS, &stats, &size),
        0);
    REQUIRE_EQ(stats.mcast_pkt_sent, 0U);
}

TEST(
    multicast_dispatch_coordinated_lost_originals_repair_over_unicast_with_canonical_keys)
{
    SKIP_UNLESS(
        loopback_multicast_available(), "no loopback multicast on this host");
    for (unsigned mode = 0; mode < 3; ++mode) {
#ifndef ENABLE_AEAD_API_PREVIEW
        if (mode == 2)
            continue;
#endif
        CoordinatedNative f(1'000'000, mode != 0, mode == 2);
        DropOneOriginal probe;
        probe.channel = f.source();
        probe.multicast = {
            .address = {239, 255, 133, 11}, .port = multicast_port};
        probe.drop_sequence = static_cast<std::int32_t>(
            f.actor()->snapshot().next_sequence.advanced(3).value());
        SendHookReset cleanup {probe.channel};
        probe.channel->set_send_hook_for_testing(drop_one_original, &probe);
        for (int i = 0; i < 10; ++i) {
            REQUIRE_EQ(f.send(i), static_cast<int>(message(i).size()));
            std::this_thread::sleep_for(std::chrono::milliseconds {2});
        }
        expect_messages(f.receiver, 0, 10);
        REQUIRE_EQ(probe.dropped.load(), 1);
        REQUIRE_EQ(probe.multicast_repeats.load(), 0);
        SRT_TRACEBSTATS stats {};
        REQUIRE_EQ(srt_bstats(f.sender, &stats, 0), 0);
        REQUIRE(stats.pktRetransTotal > 0);
        REQUIRE_EQ(f.actor()->snapshot().detached_members, 0U);
    }
}

TEST(
    multicast_dispatch_coordinated_slow_receiver_is_visible_and_healthy_stream_continues)
{
    SKIP_UNLESS(
        loopback_multicast_available(), "no loopback multicast on this host");
    CoordinatedNative f(1'000'000, false, false, 32);
    const auto healthy =
        make_receiver(f.peers, f.bound, false, false, 120, true);
    REQUIRE(wait_for_members(f.sender, 2));
    std::shared_ptr<robotweax::srt::compat::DatagramChannel> control;
    {
        const auto record = GroupRegistry::instance().find(f.receiver);
        std::lock_guard lock(record->mutex);
        control = record->dispatch->control_channel.lock();
    }
    REQUIRE(control);
    SendHookReset cleanup {control};
    control->set_send_hook_for_testing(
        +[](std::span<const std::byte> bytes, IpEndpoint peer,
             void* p) noexcept {
            const auto decoded = decode_packet(bytes);
            if (decoded && decoded.packet.kind == PacketKind::control
                && (decoded.packet.control.type == ControlType::acknowledgement
                    || decoded.packet.control.type
                        == ControlType::negative_acknowledgement))
                return UdpIoResult {.bytes_transferred = bytes.size()};
            return static_cast<robotweax::srt::compat::DatagramChannel*>(p)
                ->socket.send_to(bytes, peer);
        },
        control.get());
    for (int i = 0; i < 40; ++i) {
        REQUIRE_EQ(f.send(i), static_cast<int>(message(i).size()));
        expect_messages(healthy, i, 1);
        std::this_thread::sleep_for(std::chrono::milliseconds {20});
    }
    SRT_SOCKGROUPDATA data[2] {};
    std::size_t count = 2;
    REQUIRE_EQ(srt_group_data(f.sender, data, &count), 2);
    int detached = 0, running = 0;
    for (auto& row : data) {
        REQUIRE_EQ(row.sockstate, SRTS_CONNECTED);
        detached +=
            row.memberstate == SRT_GST_BROKEN && row.result == SRT_ECONNLOST;
        running +=
            row.memberstate == SRT_GST_RUNNING && row.result == SRT_SUCCESS;
    }
    REQUIRE_EQ(detached, 1);
    REQUIRE_EQ(running, 1);
    REQUIRE_EQ(f.actor()->snapshot().detached_members, 1U);
    std::array<robotweax::srt::compat::GroupSendCoordinator::Member, 2> members;
    REQUIRE_EQ(f.actor()->members(members), 2U);
    REQUIRE(std::any_of(members.begin(), members.end(), [](const auto& m) {
        return m.detachment
            == robotweax::srt::compat::GroupSendCoordinator::Detachment::
                history_full;
    }));
    REQUIRE_EQ(srt_getsockstate(f.receiver), SRTS_CONNECTED);
}

TEST(
    multicast_dispatch_coordinated_validation_requires_fixed_source_and_positive_wire_rate)
{
    const bool yes = true;
    for (const auto type :
        {SRT_GTYPE_BROADCAST, ROBOTWEAX_SRT_GTYPE_MULTICAST}) {
        Peers peers;
        const auto group = peers.keep(srt_create_group(type));
        REQUIRE_EQ(srt_setsockflag(group, SRTO_ROBOTWEAX_MCAST_COORDINATED,
                       &yes, sizeof(yes)),
            SRT_ERROR);
        REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVOP);
    }
    for (unsigned mutation = 0; mutation < 3; ++mutation) {
        Peers peers;
        const auto listener = peers.keep(srt_create_socket());
        bool selected = true;
        int size = sizeof(selected);
        REQUIRE_EQ(srt_getsockflag(listener, SRTO_ROBOTWEAX_MCAST_COORDINATED,
                       &selected, &size),
            0);
        REQUIRE(!selected);
        REQUIRE_EQ(srt_setsockflag(listener, SRTO_ROBOTWEAX_MCAST_COORDINATED,
                       &yes, sizeof(yes)),
            0);
        auto local = ipv4_sockaddr({127, 0, 0, 1}, 0);
        const auto group = ipv4_sockaddr({239, 255, 133, 11}, multicast_port);
        REQUIRE_EQ(srt_setsockflag(listener, SRTO_ROBOTWEAX_MCAST_GROUP, &group,
                       sizeof(group)),
            0);
        REQUIRE_EQ(srt_setsockflag(listener, SRTO_ROBOTWEAX_MCAST_SOURCE,
                       &local, sizeof(local)),
            0);
        if (mutation != 0)
            REQUIRE_EQ(srt_setsockflag(listener,
                           SRTO_ROBOTWEAX_MCAST_INDEPENDENT, &yes, sizeof(yes)),
                0);
        const std::int64_t rate = mutation == 0 ? 1000 : mutation == 1 ? 0 : -1;
        REQUIRE_EQ(
            srt_setsockflag(listener, SRTO_MAXBW, &rate, sizeof(rate)), 0);
        REQUIRE_EQ(srt_bind(listener, reinterpret_cast<const sockaddr*>(&local),
                       sizeof(local)),
            0);
        REQUIRE_EQ(srt_listen(listener, 2), SRT_ERROR);
        REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);
        REQUIRE_EQ(srt_getsockstate(listener), SRTS_OPENED);
    }
    Peers peers;
    const auto caller = peers.keep(srt_create_socket());
    REQUIRE_EQ(srt_setsockflag(
                   caller, SRTO_ROBOTWEAX_MCAST_COORDINATED, &yes, sizeof(yes)),
        0);
    const auto peer = ipv4_sockaddr({127, 0, 0, 1}, 9);
    REQUIRE_EQ(srt_connect(caller, reinterpret_cast<const sockaddr*>(&peer),
                   sizeof(peer)),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);
}

TEST(multicast_dispatch_coordinated_final_cleanup_retires_live_source_services)
{
    SKIP_UNLESS(
        loopback_multicast_available(), "no loopback multicast on this host");
    // Prior socket creation owns an implicit startup reference. Retire that
    // generation before establishing this test's explicit final reference.
    REQUIRE_EQ(srt_cleanup(), 0);
    REQUIRE_EQ(srt_startup(), 0);
    struct Cleanup {
        bool active = true;
        ~Cleanup()
        {
            if (active)
                (void)srt_cleanup();
        }
    } cleanup;
    CoordinatedNative f(1);
    auto actor = f.actor();
    auto pool = robotweax::srt::compat::acquire_runtime_scheduler();
    REQUIRE_EQ(f.send(0), static_cast<int>(message(0).size()));
    REQUIRE_EQ(f.send(1), static_cast<int>(message(1).size()));
    REQUIRE(pool->snapshot().services_reserved >= 2U);
    REQUIRE_EQ(srt_cleanup(), 0);
    cleanup.active = false;
    REQUIRE(actor->snapshot().closed);
    REQUIRE_EQ(pool->snapshot().services_reserved, 0U);
    REQUIRE(!pool->snapshot().accepting);
    REQUIRE_EQ(srt_startup(), 0);
    cleanup.active = true;
    auto fresh = robotweax::srt::compat::acquire_runtime_scheduler();
    REQUIRE(fresh && fresh != pool);
    REQUIRE_EQ(fresh->snapshot().services_reserved, 0U);
}

TEST(multicast_dispatch_coordinated_ctr_public_rotations_and_empty_rejoin)
{
    SKIP_UNLESS(
        loopback_multicast_available(), "no loopback multicast on this host");
    run_dispatch_stream({.encrypted = true,
        .first_batch = 80,
        .second_batch = 40,
        .third_batch = 40,
        .refresh_rate = 32,
        .preannouncement = 8,
        .send_gap = std::chrono::milliseconds {2},
        .independent = true,
        .coordinated = true});
}

#ifdef ENABLE_AEAD_API_PREVIEW
TEST(multicast_dispatch_coordinated_gcm_public_rotations_and_empty_rejoin)
{
    SKIP_UNLESS(
        loopback_multicast_available(), "no loopback multicast on this host");
    run_dispatch_stream({.encrypted = true,
        .first_batch = 80,
        .second_batch = 40,
        .third_batch = 40,
        .refresh_rate = 32,
        .preannouncement = 8,
        .send_gap = std::chrono::milliseconds {2},
        .aes_gcm = true,
        .independent = true,
        .coordinated = true});
}
#endif

TEST(multicast_dispatch_independent_transient_repair_uses_unicast_retry)
{
    run_dispatch_shared_loss(false, true, true);
}

namespace {
using PolicyActor = robotweax::srt::compat::GroupSendCoordinator;
using PolicyDelivery = GroupOriginalDelivery;
struct PolicyWireTrace {
    std::shared_ptr<robotweax::srt::compat::DatagramChannel> source, control;
    IpEndpoint target;
    std::mutex mutex;
    std::vector<std::vector<std::byte>> multicast, unicast;
    static UdpIoResult source_send(
        std::span<const std::byte> bytes, IpEndpoint peer, void* p) noexcept
    {
        auto& self = *static_cast<PolicyWireTrace*>(p);
        const auto packet = decode_packet(bytes);
        if (packet && packet.packet.kind == PacketKind::data) {
            std::lock_guard lock(self.mutex);
            self.multicast.emplace_back(bytes.begin(), bytes.end());
        }
        return self.source->socket.send_to(bytes, peer);
    }
    static UdpIoResult control_send(
        std::span<const std::byte> bytes, IpEndpoint peer, void* p) noexcept
    {
        auto& self = *static_cast<PolicyWireTrace*>(p);
        const auto packet = decode_packet(bytes);
        if (peer == self.target && packet
            && packet.packet.kind == PacketKind::data) {
            std::lock_guard lock(self.mutex);
            self.unicast.emplace_back(bytes.begin(), bytes.end());
        }
        return self.control->socket.send_to(bytes, peer);
    }
};
std::vector<std::byte> normalized_original(const std::vector<std::byte>& bytes)
{
    const auto packet = decode_packet(bytes);
    REQUIRE(packet);
    auto header = packet.packet.data;
    header.retransmitted = false;
    std::vector<std::byte> output(bytes.size());
    REQUIRE(encode_packet({.kind = PacketKind::data,
                              .data = header,
                              .payload = packet.packet.payload},
        output));
    return output;
}
PolicyActor::Transition force_policy(const std::shared_ptr<PolicyActor>& actor,
    std::uint64_t member, std::uint64_t revision, PolicyDelivery delivery,
    bool recover = false)
{
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds {2};
    for (;;) {
        auto result = actor->set_delivery(member, revision, delivery, recover);
        if (result.status != PolicyActor::TransitionStatus::busy)
            return result;
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds {1});
    }
}
}

TEST(
    multicast_dispatch_delivery_policy_forced_blackout_recovery_overlap_and_return)
{
    SKIP_UNLESS(
        loopback_multicast_available(), "no loopback multicast on this host");
    for (unsigned mode = 0; mode < 3; ++mode) {
#ifndef ENABLE_AEAD_API_PREVIEW
        if (mode == 2)
            continue;
#endif
        CoordinatedNative f(1'000'000, mode != 0, mode == 2);
        const auto healthy =
            make_receiver(f.peers, f.bound, mode != 0, mode == 2, 120, true);
        REQUIRE(wait_for_members(f.sender, 2, mode != 0));
        auto actor = f.actor();
        std::uint64_t member = 0;
        SRTSOCKET sender_socket = SRT_INVALID_SOCK,
                  receiver_socket = SRT_INVALID_SOCK;
        {
            auto record = GroupRegistry::instance().find(f.sender);
            std::lock_guard lock(record->mutex);
            member = record->members.front().coordinator_member;
            sender_socket = record->members.front().public_data.id;
        }
        PolicyWireTrace trace;
        trace.source = f.source();
        {
            auto record = SocketRegistry::instance().find(sender_socket);
            std::lock_guard lock(record->mutex);
            trace.control = record->channel;
            trace.target = record->peer_endpoint;
        }
        std::shared_ptr<robotweax::srt::compat::DatagramChannel> ingress;
        std::shared_ptr<robotweax::srt::compat::ConnectionRuntime> runtime;
        std::uint32_t route = 0;
        {
            auto record = GroupRegistry::instance().find(f.receiver);
            std::lock_guard lock(record->mutex);
            ingress = record->dispatch->receive_channel;
            route = record->dispatch->ingress_group_id;
            receiver_socket = record->members.front().public_data.id;
        }
        {
            auto record = SocketRegistry::instance().find(receiver_socket);
            std::lock_guard lock(record->mutex);
            runtime = record->runtime;
        }
        SendHookReset reset_source {trace.source},
            reset_control {trace.control};
        trace.source->set_send_hook_for_testing(
            PolicyWireTrace::source_send, &trace);
        trace.control->set_send_hook_for_testing(
            PolicyWireTrace::control_send, &trace);
        ingress->unregister_connection(
            route); // Complete receiver-local MC blackout, including startup.
        const auto start = actor->snapshot().next_sequence;
        const auto send_range = [&](int first, int count) {
            for (int i = first; i < first + count; ++i) {
                REQUIRE_EQ(f.send(i), static_cast<int>(message(i).size()));
                std::this_thread::sleep_for(std::chrono::milliseconds {2});
            }
        };
        send_range(0, 4);
        expect_messages(healthy, 0, 4);
        auto policy =
            force_policy(actor, member, 0, PolicyDelivery::unicast, true);
        REQUIRE_EQ(policy.status, PolicyActor::TransitionStatus::applied);
        REQUIRE_EQ(policy.member.effective_sequence, start.advanced(4));
        REQUIRE_EQ(policy.prefix.repairs, 4U);
        send_range(4, 48);
        expect_messages(f.receiver, 0, 52);
        expect_messages(healthy, 4, 48);
        const auto bound_source = trace.source->socket.local_endpoint();
        REQUIRE(bound_source);
        REQUIRE(ingress->register_ingress_route(route, runtime,
            robotweax::srt::compat::PacketIngress::multicast_data,
            robotweax::srt::compat::MulticastIngressConfiguration {
                .source = {.kind = MulticastSourceKind::announcement,
                    .group_id = route,
                    .endpoint = bound_source.endpoint}}));
        policy = force_policy(actor, member, 1, PolicyDelivery::overlap);
        REQUIRE_EQ(policy.status, PolicyActor::TransitionStatus::applied);
        REQUIRE_EQ(policy.member.effective_sequence, start.advanced(52));
        send_range(52, 12);
        expect_messages(f.receiver, 52, 12);
        expect_messages(healthy, 52, 12);
        policy = force_policy(actor, member, 2, PolicyDelivery::multicast);
        REQUIRE_EQ(policy.status, PolicyActor::TransitionStatus::applied);
        REQUIRE_EQ(policy.member.effective_sequence, start.advanced(64));
        send_range(64, 12);
        expect_messages(f.receiver, 64, 12);
        expect_messages(healthy, 64, 12);
        std::vector<std::vector<std::byte>> multicast, unicast;
        {
            std::lock_guard lock(trace.mutex);
            multicast = trace.multicast;
            unicast = trace.unicast;
        }
        REQUIRE_EQ(multicast.size(), 76U);
        std::size_t originals = 0, repairs = 0;
        for (const auto& bytes : unicast) {
            const auto decoded = decode_packet(bytes);
            REQUIRE(decoded);
            const auto offset =
                decoded.packet.data.sequence.distance_from(start);
            REQUIRE(offset >= 0 && offset < 76);
            REQUIRE_EQ(normalized_original(bytes),
                multicast[static_cast<std::size_t>(offset)]);
            if (decoded.packet.data.retransmitted)
                ++repairs;
            else {
                ++originals;
                REQUIRE(offset >= 4 && offset < 64);
            }
        }
        // A multicast ACK may retire a queued overlap copy before native UDP.
        // Every unicast-only original must still have reached the unicast path.
        REQUIRE(originals >= 48U && originals <= 60U);
        REQUIRE(repairs >= 4U);
        REQUIRE_EQ(actor->snapshot().detached_members, 0U);
        SRT_TRACEBSTATS received {};
        REQUIRE_EQ(srt_bstats(f.receiver, &received, 0), 0);
        REQUIRE_EQ(received.pktRecvUniqueTotal, 76);
        REQUIRE_EQ(srt_getsockstate(f.receiver), SRTS_CONNECTED);
    }
}

TEST(multicast_dispatch_delivery_policy_committed_unicast_survives_source_error)
{
    SKIP_UNLESS(
        loopback_multicast_available(), "no loopback multicast on this host");
    for (unsigned mode = 0; mode < 3; ++mode) {
#ifndef ENABLE_AEAD_API_PREVIEW
        if (mode == 2)
            continue;
#endif
        CoordinatedNative f(1'000'000, mode != 0, mode == 2);
        auto actor = f.actor();
        std::array<PolicyActor::Member, 1> rows;
        REQUIRE_EQ(actor->members(rows), 1U);
        REQUIRE_EQ(
            force_policy(actor, rows[0].id, 0, PolicyDelivery::unicast).status,
            PolicyActor::TransitionStatus::applied);
        auto source = f.source();
        SendHookReset cleanup {source};
        source->set_send_hook_for_testing(
            +[](std::span<const std::byte>, IpEndpoint, void*) noexcept {
                return UdpIoResult {.error = Error::io_error};
            },
            nullptr);
        REQUIRE_EQ(f.send(0), static_cast<int>(message(0).size()));
        expect_messages(f.receiver, 0, 1);
        REQUIRE_EQ(srt_getsockstate(f.receiver), SRTS_CONNECTED);
        REQUIRE_EQ(srt_getsockstate(f.sender), SRTS_BROKEN);
        REQUIRE_EQ(f.send(1),
            SRT_ERROR); // Accepted work survives; new source admission still fails.
        REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ESOCKFAIL);
        REQUIRE_EQ(actor->snapshot().detached_members, 0U);
    }
}

TEST(
    multicast_dispatch_coordinated_aggregate_budget_bounds_correlated_prefix_repairs)
{
    SKIP_UNLESS(
        loopback_multicast_available(), "no loopback multicast on this host");
    for (const bool gcm : {false, true}) {
#ifndef ENABLE_AEAD_API_PREVIEW
        if (gcm)
            continue;
#endif
        CoordinatedNative f(30'000, gcm, gcm, 64, 1000);
        std::vector<SRTSOCKET> receivers {f.receiver};
        for (unsigned i = 1; i < 6; ++i)
            receivers.push_back(
                make_receiver(f.peers, f.bound, gcm, gcm, 1000, true));
        REQUIRE(wait_for_members(f.sender, 6, gcm));
        for (const auto handle : receivers) {
            const auto record = GroupRegistry::instance().find(handle);
            std::shared_ptr<robotweax::srt::compat::DatagramChannel> ingress;
            std::uint32_t route = 0;
            {
                std::lock_guard lock(record->mutex);
                ingress = record->dispatch->receive_channel;
                route = record->dispatch->ingress_group_id;
            }
            ingress->unregister_connection(route);
        }
        auto actor = f.actor();
        std::array<PolicyActor::Member, 6> members;
        REQUIRE_EQ(actor->members(members), 6U);
        std::shared_ptr<robotweax::srt::compat::DatagramChannel> control;
        {
            auto group = GroupRegistry::instance().find(f.sender);
            SRTSOCKET member = SRT_INVALID_SOCK;
            {
                std::lock_guard lock(group->mutex);
                member = group->members.front().public_data.id;
            }
            auto socket = SocketRegistry::instance().find(member);
            std::lock_guard lock(socket->mutex);
            control = socket->channel;
        }
        struct Trace {
            std::shared_ptr<robotweax::srt::compat::DatagramChannel> channel;
            std::mutex mutex;
            std::vector<
                std::pair<std::chrono::steady_clock::time_point, std::size_t>>
                submissions;
            bool nonrepair = false;
            static UdpIoResult send(std::span<const std::byte> bytes,
                IpEndpoint peer, void* p) noexcept
            {
                auto& self = *static_cast<Trace*>(p);
                const auto sent = self.channel->socket.send_to(bytes, peer);
                const auto decoded = decode_packet(bytes);
                if (sent && sent.bytes_transferred == bytes.size() && decoded
                    && decoded.packet.kind == PacketKind::data) {
                    std::lock_guard lock(self.mutex);
                    self.submissions.emplace_back(
                        std::chrono::steady_clock::now(), bytes.size() + 28U);
                    self.nonrepair |= !decoded.packet.data.retransmitted;
                }
                return sent;
            }
        } trace;
        trace.channel = control;
        SendHookReset cleanup {control};
        control->set_send_hook_for_testing(Trace::send, &trace);
        for (int i = 0; i < 30; ++i)
            REQUIRE_EQ(f.send(i), static_cast<int>(message(i).size()));
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds {3};
        while (actor->snapshot().inflight_frames) {
            REQUIRE(std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds {1});
        }
        const auto start = std::chrono::steady_clock::now();
        for (const auto member : members) {
            const auto result = force_policy(
                actor, member.id, 0, PolicyDelivery::multicast, true);
            REQUIRE_EQ(result.status, PolicyActor::TransitionStatus::applied);
            REQUIRE_EQ(result.prefix.repairs, 30U);
        }
        for (const auto handle : receivers)
            expect_messages(handle, 0, 30);
        const auto state = actor->snapshot();
        REQUIRE_EQ(state.detached_members, 0U);
        REQUIRE_EQ(state.unicast_budget.wire_bytes_per_second, 120'000U);
        REQUIRE_EQ(state.unicast_budget.original_wire_bytes, 0U);
        REQUIRE(state.unicast_budget.waits > 0);
        std::lock_guard lock(trace.mutex);
        REQUIRE(!trace.nonrepair);
        REQUIRE(trace.submissions.size() >= 180U);
        std::uint64_t wire = 0;
        for (const auto& [time, bytes] : trace.submissions) {
            wire += bytes;
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::microseconds>(
                    time - start)
                    .count();
            REQUIRE(elapsed >= 0);
            // One MTU burst plus the common rate; a microsecond of allowance
            // covers floor quantization at the observer/budget clock boundary.
            REQUIRE(wire <= 1500U
                    + 120'000U * static_cast<std::uint64_t>(elapsed + 1)
                        / 1'000'000U);
        }
        REQUIRE(state.unicast_budget.repair_wire_bytes >= wire);
    }
}

namespace {
PolicyActor::SourceRecovery recover_native_source(
    const std::shared_ptr<PolicyActor>& actor,
    std::span<const PolicyActor::RecoveryMember> members)
{
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds {2};
    for (;;) {
        auto result = actor->recover_source_to_unicast(0, members);
        if (result.status != PolicyActor::TransitionStatus::busy)
            return result;
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds {1});
    }
}
struct FailedSourceProbe {
    std::atomic<bool> fatal {false};
    std::atomic<unsigned> attempts {0};
    std::mutex mutex;
    std::vector<std::byte> first;
    static UdpIoResult send(
        std::span<const std::byte> bytes, IpEndpoint, void* p) noexcept
    {
        auto& self = *static_cast<FailedSourceProbe*>(p);
        ++self.attempts;
        {
            std::lock_guard lock(self.mutex);
            if (self.first.empty())
                self.first.assign(bytes.begin(), bytes.end());
        }
        return self.fatal.load()
            ? UdpIoResult {.error = Error::io_error, .system_error = EIO}
            : UdpIoResult {.error = Error::would_block};
    }
};
void wait_for_source_failure(const std::shared_ptr<PolicyActor>& actor)
{
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds {2};
    while (!actor->snapshot().failed || actor->snapshot().inflight_frames) {
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds {1});
    }
}
}

TEST(
    multicast_dispatch_source_recovery_restores_cached_members_and_continues_canonical_rotations)
{
    SKIP_UNLESS(
        loopback_multicast_available(), "no loopback multicast on this host");
    for (unsigned mode = 0; mode < 3; ++mode) {
#ifndef ENABLE_AEAD_API_PREVIEW
        if (mode == 2)
            continue;
#endif
        CoordinatedNative f(1'000'000, mode != 0, mode == 2, 64);
        const auto second =
            make_receiver(f.peers, f.bound, mode != 0, mode == 2, 120, true);
        REQUIRE(wait_for_members(f.sender, 2, mode != 0));
        auto actor = f.actor();
        std::array<PolicyActor::Member, 2> members;
        REQUIRE_EQ(actor->members(members), 2U);
        FailedSourceProbe probe;
        SendHookReset source_reset {f.source()};
        f.source()->set_send_hook_for_testing(FailedSourceProbe::send, &probe);
        PolicyWireTrace trace;
        {
            auto group = GroupRegistry::instance().find(f.sender);
            SRTSOCKET member = SRT_INVALID_SOCK;
            {
                std::lock_guard lock(group->mutex);
                member = group->members.front().public_data.id;
            }
            auto socket = SocketRegistry::instance().find(member);
            std::lock_guard lock(socket->mutex);
            trace.control = socket->channel;
            trace.target = socket->peer_endpoint;
        }
        SendHookReset control_reset {trace.control};
        trace.control->set_send_hook_for_testing(
            PolicyWireTrace::control_send, &trace);
        for (int i = 0; i < 4; ++i)
            REQUIRE_EQ(f.send(i), static_cast<int>(message(i).size()));
        probe.fatal = true;
        wait_for_source_failure(actor);
        SRT_SOCKGROUPDATA rows[2] {};
        std::size_t count = 2;
        REQUIRE_EQ(srt_group_data(f.sender, rows, &count), 2);
        for (const auto& row : rows) {
            REQUIRE_EQ(row.sockstate, SRTS_CONNECTED);
            REQUIRE_EQ(row.memberstate, SRT_GST_BROKEN);
            REQUIRE_EQ(row.result, SRT_ECONNLOST);
        }
        REQUIRE_EQ(srt_getsockstate(f.sender), SRTS_BROKEN);
        const int epoll = srt_epoll_create();
        REQUIRE(epoll >= 0);
        struct ReleaseRecoveryWatch {
            int id;
            ~ReleaseRecoveryWatch()
            {
                (void)srt_epoll_release(id);
            }
        } release_watch {epoll};
        const int watched = SRT_EPOLL_OUT | SRT_EPOLL_ERR;
        REQUIRE_EQ(srt_epoll_add_usock(epoll, f.sender, &watched), 0);
        SRT_EPOLL_EVENT ready[1] {};
        REQUIRE_EQ(srt_epoll_uwait(epoll, ready, 1, 1000), 1);
        REQUIRE(ready[0].events & SRT_EPOLL_ERR);
        const auto boundary = actor->snapshot().next_sequence;
        const std::array selected {
            PolicyActor::RecoveryMember {members[0].id, 0},
            PolicyActor::RecoveryMember {members[1].id, 0}};
        const auto recovered = recover_native_source(actor, selected);
        REQUIRE_EQ(recovered.status, PolicyActor::TransitionStatus::applied);
        REQUIRE_EQ(recovered.prefix.originals, 8U);
        REQUIRE_EQ(recovered.prefix.repairs, 0U);
        REQUIRE_EQ(recovered.effective_sequence, boundary);
        REQUIRE_EQ(srt_getsockstate(f.sender), SRTS_CONNECTED);
        REQUIRE_EQ(srt_epoll_uwait(epoll, ready, 1, 1000), 1);
        REQUIRE(ready[0].events & SRT_EPOLL_OUT);
        REQUIRE(!(ready[0].events & SRT_EPOLL_ERR));
        count = 2;
        REQUIRE_EQ(srt_group_data(f.sender, rows, &count), 2);
        for (const auto& row : rows) {
            REQUIRE_EQ(row.memberstate, SRT_GST_RUNNING);
            REQUIRE_EQ(row.result, 0);
        }
        const auto stopped_attempts = probe.attempts.load();
        expect_messages(f.receiver, 0, 4);
        expect_messages(second, 0, 4);
        for (int i = 4; i < 84; ++i) {
            REQUIRE_EQ(f.send(i), static_cast<int>(message(i).size()));
            std::this_thread::sleep_for(std::chrono::milliseconds {2});
        }
        expect_messages(f.receiver, 4, 80);
        expect_messages(second, 4, 80);
        REQUIRE_EQ(probe.attempts.load(), stopped_attempts);
        REQUIRE_EQ(actor->snapshot().unicast_only_frames, 80U);
        REQUIRE_EQ(actor->snapshot().detached_members, 0U);
        {
            std::scoped_lock lock(probe.mutex, trace.mutex);
            REQUIRE(trace.unicast.size() >= 84U);
            REQUIRE_EQ(trace.unicast.front(), probe.first);
            REQUIRE(!decode_packet(trace.unicast.front())
                    .packet.data.retransmitted);
        }
        ROBOTWEAX_SRT_MCAST_STATS_V1 stats {};
        int size = sizeof(stats);
        REQUIRE_EQ(srt_getsockflag(
                       f.sender, SRTO_ROBOTWEAX_MCAST_STATS, &stats, &size),
            0);
        REQUIRE_EQ(stats.mcast_pkt_sent, 0U);
        SRT_TRACEBSTATS group_stats {};
        REQUIRE_EQ(srt_bstats(f.sender, &group_stats, 0), 0);
        REQUIRE_EQ(group_stats.pktSentUniqueTotal, 84);
        REQUIRE_EQ(srt_getsockstate(f.receiver), SRTS_CONNECTED);
        REQUIRE_EQ(srt_getsockstate(second), SRTS_CONNECTED);
    }
}

TEST(
    multicast_dispatch_source_recovery_preserves_preapproved_survivor_and_refuses_late_join)
{
    SKIP_UNLESS(
        loopback_multicast_available(), "no loopback multicast on this host");
    CoordinatedNative f;
    const auto other = make_receiver(f.peers, f.bound, false, false, 120, true);
    REQUIRE(wait_for_members(f.sender, 2));
    auto actor = f.actor();
    std::array<PolicyActor::Member, 2> members;
    REQUIRE_EQ(actor->members(members), 2U);
    REQUIRE_EQ(
        force_policy(actor, members[0].id, 0, PolicyDelivery::unicast).status,
        PolicyActor::TransitionStatus::applied);
    FailedSourceProbe probe;
    probe.fatal = true;
    SendHookReset reset {f.source()};
    f.source()->set_send_hook_for_testing(FailedSourceProbe::send, &probe);
    REQUIRE_EQ(f.send(0), static_cast<int>(message(0).size()));
    wait_for_source_failure(actor);
    expect_messages(f.receiver, 0, 1);
    const std::array selected {PolicyActor::RecoveryMember {members[0].id, 1}};
    REQUIRE_EQ(recover_native_source(actor, selected).status,
        PolicyActor::TransitionStatus::applied);
    const auto attempts = probe.attempts.load();
    const auto late =
        f.peers.keep(srt_create_group(ROBOTWEAX_SRT_GTYPE_MULTICAST));
    REQUIRE(late != SRT_INVALID_SOCK);
    const bool yes = true;
    REQUIRE_EQ(srt_setsockflag(
                   late, SRTO_ROBOTWEAX_MCAST_INDEPENDENT, &yes, sizeof(yes)),
        0);
    const auto local = ipv4_sockaddr({127, 0, 0, 1}, 0);
    REQUIRE_EQ(srt_setsockflag(
                   late, SRTO_ROBOTWEAX_MCAST_IFACE, &local, sizeof(local)),
        0);
    const std::int32_t timeout = 1000;
    REQUIRE_EQ(
        srt_setsockflag(late, SRTO_CONNTIMEO, &timeout, sizeof(timeout)), 0);
    auto endpoint = srt_prepare_endpoint(
        nullptr, reinterpret_cast<const sockaddr*>(&f.bound), sizeof(f.bound));
    REQUIRE_EQ(srt_connect_group(late, &endpoint, 1), SRT_ERROR);
    for (int i = 1; i < 11; ++i)
        REQUIRE_EQ(f.send(i), static_cast<int>(message(i).size()));
    expect_messages(f.receiver, 1, 10);
    SRT_SOCKGROUPDATA rows[2] {};
    std::size_t count = 2;
    REQUIRE_EQ(srt_group_data(f.sender, rows, &count), 2);
    REQUIRE_EQ(rows[0].memberstate, SRT_GST_RUNNING);
    REQUIRE_EQ(rows[1].memberstate, SRT_GST_BROKEN);
    REQUIRE_EQ(actor->snapshot().active_members, 1U);
    REQUIRE_EQ(actor->snapshot().detached_members, 1U);
    REQUIRE_EQ(probe.attempts.load(), attempts);
    REQUIRE_EQ(srt_getsockstate(other), SRTS_CONNECTED);
    f.peers.drop(f.sender);
    REQUIRE(actor->snapshot().closed);
    REQUIRE_EQ(probe.attempts.load(), attempts);
}
