// End-to-end multicast dispatch groups over loopback multicast: one Listener
// sender, several Caller receivers, control on unicast, payload once to the
// group. The administratively scoped group 239.255.133.0/24 with hop limit 0
// and the loopback interface keeps every datagram on this host.
#include "test.hpp"
#include "hybrid_fault_link.hpp"

#include "compat/group_registry.hpp"
#include "compat/group_send_coordinator.hpp"
#include "compat/hybrid_automatic.hpp"
#include "compat/hybrid_automatic_return.hpp"
#include "compat/hybrid_membership.hpp"
#include "compat/hybrid_setup.hpp"
#include "compat/runtime_scheduler_service.hpp"
#include "compat/socket_registry.hpp"
#include "compat/transport_runtime.hpp"
#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_HYBRID_CONTROL
#include "compat/hybrid_exchange.hpp"
#include "compat/hybrid_native.hpp"
#include "compat/hybrid_bridge.hpp"
#include "compat/hybrid_receiver.hpp"
#endif
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
#include <cstdio>
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
    bool independent = false, bool asynchronous = false,
    bool session_authentication = false,
    void (*configure)(void*, SRTSOCKET, bool) = nullptr,
    void* context = nullptr, int* connect_result = nullptr)
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
    if (session_authentication)
        REQUIRE_EQ(srt_setsockflag(group, SRTO_ROBOTWEAX_SESSIONAUTH,
                       &session_authentication, sizeof(session_authentication)),
            0);
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
    if (configure)
        configure(context, group, true);
    const auto connected = srt_connect_group(group, &endpoint, 1);
    if (connect_result)
        *connect_result = connected;
    else
        REQUIRE(connected != SRT_ERROR);
    if (asynchronous && !connect_result) {
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
    bool session_authentication = false;
};

#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_HYBRID_CONTROL
struct NativeSnapshotTestProvider {
    static bool resolve(void*,
        const robotweax::srt::compat::hybrid::CredentialRequest& request,
        robotweax::srt::compat::hybrid::Authorization& out,
        std::span<std::byte, 32> secret) noexcept
    {
        out = {.credential_id = request.credential_id,
            .caller_group = request.caller_group,
            .listener_group = request.listener_group,
            .stream_id = request.stream_id,
            .generation = 1,
            .expires_at_us = 10000};
        std::fill(secret.begin(), secret.end(), std::byte {0x22});
        return true;
    }
    static bool revalidate(void*,
        const robotweax::srt::compat::hybrid::CredentialRequest&,
        const robotweax::srt::compat::hybrid::Authorization& expected,
        robotweax::srt::compat::hybrid::Authorization& current) noexcept
    {
        current = expected;
        return true;
    }
};
void verify_native_conclusions(
    SRTSOCKET sender, SRTSOCKET receiver, bool independent)
{
    namespace h = robotweax::srt::compat::hybrid;
    struct Member {
        std::shared_ptr<robotweax::srt::compat::ConnectionRuntime> runtime;
        std::uint32_t local = 0, peer = 0;
        IpEndpoint endpoint;
    };
    const auto members = [](SRTSOCKET group) {
        const auto g = GroupRegistry::instance().find(group);
        REQUIRE(g);
        std::vector<SRTSOCKET> ids;
        {
            std::lock_guard lock(g->mutex);
            for (const auto& m : g->members)
                ids.push_back(m.public_data.id);
        }
        std::vector<Member> out;
        for (auto id : ids) {
            const auto socket = SocketRegistry::instance().find(id);
            REQUIRE(socket);
            std::lock_guard lock(socket->mutex);
            out.push_back({socket->runtime, socket->protocol_socket_id,
                socket->peer_protocol_socket_id, socket->peer_endpoint});
        }
        return out;
    };
    const auto receivers = members(receiver), senders = members(sender);
    REQUIRE_EQ(receivers.size(), 1U);
    REQUIRE(receivers.front().runtime);
    const auto pair = receivers.front().runtime->hybrid_conclusions();
    REQUIRE_EQ(pair != nullptr, independent);
    if (!independent)
        return;
    REQUIRE(
        pair->matches(true, receivers.front().local, receivers.front().peer));
    unsigned matches = 0;
    for (const auto& member : senders) {
        if (member.local != receivers.front().peer)
            continue;
        REQUIRE(member.runtime);
        auto peer = member.runtime->hybrid_conclusions();
        const auto until =
            std::chrono::steady_clock::now() + std::chrono::seconds {1};
        while (!peer && std::chrono::steady_clock::now() < until) {
            std::this_thread::sleep_for(std::chrono::milliseconds {1});
            peer = member.runtime->hybrid_conclusions();
        }
        REQUIRE(peer);
        REQUIRE(peer->matches(false, member.local, member.peer));
        REQUIRE(std::equal(pair->caller_payload().begin(),
            pair->caller_payload().end(), peer->caller_payload().begin(),
            peer->caller_payload().end()));
        REQUIRE(std::equal(pair->listener_payload().begin(),
            pair->listener_payload().end(), peer->listener_payload().begin(),
            peer->listener_payload().end()));
        // The native snapshots must be usable directly, including ordinary
        // core sequence proposals that differ from the dispatch DATA origin.
        // This in-memory exchange is not a native carrier or policy grant.
        h::Exchange c(default_crypto_provider(), true),
            l(default_crypto_provider(), false);
        h::Exchange::Configuration cc {
            .dispatch = pair->dispatch(), .source = pair->source()};
        cc.credential_id.fill(std::byte {0x11});
        cc.established_peer = receivers.front().endpoint;
        cc.connection_generation = 1;
        auto lc = cc;
        lc.established_peer = member.endpoint;
        lc.stream = 1;
        lc.member = 1;
        h::CredentialProvider provider {
            .resolve = NativeSnapshotTestProvider::resolve,
            .revalidate = NativeSnapshotTestProvider::revalidate};
        REQUIRE(
            !l.start(lc, provider, peer->caller_payload(),
                  peer->listener_payload(), 0)
                .has_output);
        const auto offer = c.start(
            cc, provider, pair->caller_payload(), pair->listener_payload(), 0);
        REQUIRE(offer.has_output);
        const auto selection = l.receive(offer.output, 1);
        REQUIRE(selection.has_output);
        const auto proof = c.receive(selection.output, 2);
        REQUIRE(proof.has_output);
        const auto receipt = l.receive(proof.output, 3);
        REQUIRE(receipt.has_output);
        REQUIRE_EQ(
            c.receive(receipt.output, 4).status, h::Exchange::Status::active);
        REQUIRE(c.authentication());
        REQUIRE(l.authentication());
        ++matches;
    }
    REQUIRE_EQ(matches, 1U);
}
#endif

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
    if (scenario.session_authentication)
        REQUIRE_EQ(srt_setsockflag(listener, SRTO_ROBOTWEAX_SESSIONAUTH,
                       &scenario.session_authentication,
                       sizeof(scenario.session_authentication)),
            0);
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
    const SRTSOCKET receiver_a = make_receiver(peers, bound, scenario.encrypted,
        scenario.aes_gcm, 120, scenario.independent,
        scenario.asynchronous_receiver, scenario.session_authentication);
    SRTSOCKET sender = SRT_INVALID_SOCK;
    if (!scenario.fill_accept_backlog) {
        sender = peers.keep(srt_accept(listener, nullptr, nullptr));
        REQUIRE(sender != SRT_INVALID_SOCK);
    }
    const SRTSOCKET receiver_b = make_receiver(peers, bound, scenario.encrypted,
        scenario.aes_gcm, 120, scenario.independent,
        scenario.asynchronous_receiver, scenario.session_authentication);
    const SRTSOCKET receiver_c = make_receiver(peers, bound, scenario.encrypted,
        scenario.aes_gcm, 120, scenario.independent,
        scenario.asynchronous_receiver, scenario.session_authentication);
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
#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_HYBRID_CONTROL
    for (auto receiver : {receiver_a, receiver_b, receiver_c})
        verify_native_conclusions(sender, receiver, scenario.independent);
#endif
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

    const SRTSOCKET receiver_d = make_receiver(peers, bound, scenario.encrypted,
        scenario.aes_gcm, 120, scenario.independent,
        scenario.asynchronous_receiver, scenario.session_authentication);
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
        const auto joined = make_receiver(peers, bound, scenario.encrypted,
            scenario.aes_gcm, 120, scenario.independent,
            scenario.asynchronous_receiver, scenario.session_authentication);
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

#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_HYBRID_CONTROL
TEST(multicast_dispatch_native_conclusions_plain_ctr_gcm_and_async_capture)
{
    SKIP_UNLESS(
        loopback_multicast_available(), "no loopback multicast on this host");
    for (unsigned mode = 0; mode < 3; ++mode) {
#ifndef ENABLE_AEAD_API_PREVIEW
        if (mode == 2)
            continue;
#endif
        run_dispatch_stream({.encrypted = mode != 0,
            .first_batch = 3,
            .second_batch = 2,
            .third_batch = 2,
            .aes_gcm = mode == 2,
            .independent = true,
            .asynchronous_receiver = mode != 0});
    }
}
TEST(
    multicast_dispatch_native_conclusions_keep_hsreq_hsrsp_after_authenticated_agreement)
{
    SKIP_UNLESS(
        loopback_multicast_available(), "no loopback multicast on this host");
    for (const bool asynchronous : {false, true})
        run_dispatch_stream({.encrypted = true,
            .first_batch = 3,
            .second_batch = 2,
            .third_batch = 2,
            .independent = true,
            .asynchronous_receiver = asynchronous,
            .session_authentication = true});
}
#endif

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
        int latency = 120, bool coordinated = true,
        bool session_authentication = false,
        void (*configure)(void*, SRTSOCKET, bool) = nullptr,
        void* context = nullptr)
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
        if (coordinated)
            REQUIRE_EQ(
                srt_setsockflag(listener, SRTO_ROBOTWEAX_MCAST_COORDINATED,
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
        if (session_authentication) {
            REQUIRE_EQ(srt_setsockflag(listener, SRTO_ROBOTWEAX_SESSIONAUTH,
                           &enabled, sizeof(enabled)),
                0);
        }
        REQUIRE_EQ(
            srt_listen_callback(listener, admit_dispatch, &admissions), 0);
        REQUIRE_EQ(srt_bind(listener, reinterpret_cast<const sockaddr*>(&local),
                       sizeof(local)),
            0);
        if (configure)
            configure(context, listener, false);
        REQUIRE_EQ(srt_listen(listener, 8), 0);
        int size = sizeof(bound);
        REQUIRE_EQ(srt_getsockname(
                       listener, reinterpret_cast<sockaddr*>(&bound), &size),
            0);
        receiver = make_receiver(peers, bound, encrypted, gcm, latency, true,
            false, session_authentication, configure, context);
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
#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_HYBRID_CONTROL
namespace hn = robotweax::srt::compat::hybrid;
std::shared_ptr<robotweax::srt::compat::ConnectionRuntime> hybrid_runtime(
    SRTSOCKET group)
{
    const auto g = GroupRegistry::instance().find(group);
    REQUIRE(g);
    SRTSOCKET id = SRT_INVALID_SOCK;
    {
        std::lock_guard lock(g->mutex);
        REQUIRE_EQ(g->members.size(), 1U);
        id = g->members.front().public_data.id;
    }
    const auto socket = SocketRegistry::instance().find(id);
    REQUIRE(socket);
    std::lock_guard lock(socket->mutex);
    REQUIRE(socket->runtime);
    return socket->runtime;
}
struct HybridNativeOwner {
    std::weak_ptr<robotweax::srt::compat::ConnectionRuntime> runtime;
    std::atomic<std::uint64_t> generation = 7, expiry = 0;
    std::atomic<unsigned> resolves = 0, enrolls = 0;
    bool approve = true, close_on_enroll = false;
    std::mutex grant_mutex;
    hn::Authorization resolved_grant;
    static bool resolve(void* p, const hn::CredentialRequest& request,
        hn::Authorization& grant, std::span<std::byte, 32> secret) noexcept
    {
        auto& self = *static_cast<HybridNativeOwner*>(p);
        ++self.resolves;
        const auto runtime = self.runtime.lock();
        if (!runtime || !runtime->hybrid_conclusions())
            return false;
        grant = {.credential_id = request.credential_id,
            .caller_group = request.caller_group,
            .listener_group = request.listener_group,
            .stream_id = request.stream_id,
            .generation = self.generation.load(),
            .expires_at_us = self.expiry.load()};
        std::fill(secret.begin(), secret.end(), std::byte {0x22});
        {
            std::lock_guard lock(self.grant_mutex);
            self.resolved_grant = grant;
        }
        return true;
    }
    static bool revalidate(void* p, const hn::CredentialRequest&,
        const hn::Authorization& expected, hn::Authorization& grant) noexcept
    {
        auto& self = *static_cast<HybridNativeOwner*>(p);
        const auto runtime = self.runtime.lock();
        if (!runtime || !runtime->hybrid_conclusions())
            return false;
        grant = expected;
        grant.generation = self.generation.load();
        grant.expires_at_us = self.expiry.load();
        return true;
    }
    static bool enroll(void* p, const hn::Context&, const hn::Authorization&,
        std::uint64_t) noexcept
    {
        auto& self = *static_cast<HybridNativeOwner*>(p);
        ++self.enrolls;
        const auto runtime = self.runtime.lock();
        if (!runtime || !runtime->hybrid_conclusions())
            return false;
        if (self.close_on_enroll)
            runtime->mark_broken(0);
        return self.approve;
    }
};
struct NativeHybridPair {
    std::shared_ptr<robotweax::srt::compat::ConnectionRuntime> caller, listener;
    std::shared_ptr<HybridNativeOwner>
        co = std::make_shared<HybridNativeOwner>(),
        lo = std::make_shared<HybridNativeOwner>();
    std::shared_ptr<hn::NativeService> c, l;
    explicit NativeHybridPair(
        CoordinatedNative& f, std::uint64_t lifetime = 2'000'000)
        : caller(hybrid_runtime(f.receiver))
        , listener(hybrid_runtime(f.sender))
    {
        co->runtime = caller;
        lo->runtime = listener;
        co->expiry = lo->expiry = hn::NativeService::monotonic_now() + lifetime;
    }
    hn::NativeOptions options(bool is_caller)
    {
        hn::NativeOptions o;
        o.credential_id.fill(std::byte {0x11});
        o.owner = is_caller ? co : lo;
        o.resolve = HybridNativeOwner::resolve;
        o.revalidate = HybridNativeOwner::revalidate;
        o.enroll = HybridNativeOwner::enroll;
        o.retry_interval_us = 20'000;
        o.maximum_sends_per_stage = 8;
        if (!is_caller) {
            o.stream = 71;
            o.member = 97;
        }
        return o;
    }
    void start(
        std::optional<hn::CoordinatorBridge::Identity> identity = std::nullopt,
        bool queries = false, bool returning = false, bool startup = false)
    {
        auto o = options(false);
        auto caller_options = options(true);
        if (queries) {
            o.policy.supported |= hn::policy_query;
            o.policy.required |= hn::policy_query;
            caller_options.policy.supported |= hn::policy_query;
            caller_options.policy.required |= hn::policy_query;
        }
        if (returning) {
            o.policy.supported |=
                hn::return_control | hn::overlap | hn::policy_query;
            o.policy.required |=
                hn::return_control | hn::overlap | hn::policy_query;
            caller_options.policy.supported |=
                hn::return_control | hn::overlap | hn::policy_query;
            caller_options.policy.required |=
                hn::return_control | hn::overlap | hn::policy_query;
        }
        if (startup) {
            o.policy.supported |= hn::startup_fallback | hn::policy_query;
            o.policy.required |= hn::startup_fallback | hn::policy_query;
            caller_options.policy.supported |=
                hn::startup_fallback | hn::policy_query;
            caller_options.policy.required |=
                hn::startup_fallback | hn::policy_query;
        }
        if (identity) {
            o.stream = identity->stream;
            o.member = identity->member;
            o.initial_position = identity->initial_position;
        }
        l = hn::attach_native_control(listener, std::move(o));
        REQUIRE(l);
        c = hn::attach_native_control(caller, std::move(caller_options));
        REQUIRE(c);
    }
    bool wait_active()
    {
        const auto until =
            std::chrono::steady_clock::now() + std::chrono::seconds {2};
        while (std::chrono::steady_clock::now() < until) {
            if (c->snapshot().active && l->snapshot().active)
                return true;
            if (c->snapshot().closed || l->snapshot().closed)
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds {1});
        }
        return false;
    }
    bool event(const std::shared_ptr<hn::NativeService>& s,
        hn::NativeService::Event& e)
    {
        const auto until =
            std::chrono::steady_clock::now() + std::chrono::seconds {1};
        while (std::chrono::steady_clock::now() < until) {
            if (s->pop(e))
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds {1});
        }
        return false;
    }
    hn::Message request(std::uint64_t serial = 1)
    {
        const auto context = c->snapshot().context;
        return {.kind = hn::Kind::fallback_request,
            .stream = context.selection.stream,
            .member = context.selection.member,
            .serial = serial,
            .next_position = context.selection.initial_position,
            .next_sequence = context.dispatch.initial_sequence,
            .mode = hn::Mode::unicast};
    }
    ~NativeHybridPair()
    {
        if (c)
            c->close();
        if (l)
            l->close();
    }
};
struct HybridWireHook {
    std::shared_ptr<robotweax::srt::compat::DatagramChannel> channel;
    bool drop_selection = false;
    bool drop_first_control = false;
    std::atomic_bool drop_commits = false, drop_controls = false;
    std::atomic<unsigned> commits_dropped = 0;
    std::atomic<unsigned> control_drops = 0;
    std::atomic<unsigned> dropped = 0;
    std::mutex mutex;
    std::vector<std::byte> last_control;
    static UdpIoResult send(
        std::span<const std::byte> bytes, IpEndpoint peer, void* p) noexcept
    {
        auto& self = *static_cast<HybridWireHook*>(p);
        const auto decoded = decode_packet(bytes);
        hn::ExchangeRecord record;
        if (decoded && hn::native_candidate(decoded.packet)
            && decoded.packet.control.subtype
                == authenticated_control_subtype) {
            if (self.drop_controls.load())
                return {.bytes_transferred = bytes.size()};
            const auto payload = decoded.packet.payload;
            if (self.drop_commits.load() && payload.size() >= 44) {
                const auto inner =
                    decode_packet(payload.subspan(12, payload.size() - 44));
                hn::Message message;
                if (inner && hn::decode(inner.packet.payload, message)
                    && message.kind == hn::Kind::delivery_commit) {
                    ++self.commits_dropped;
                    return {.bytes_transferred = bytes.size()};
                }
            }
        }
        if (decoded && self.drop_first_control
            && decoded.packet.control.subtype == authenticated_control_subtype
            && hn::native_candidate(decoded.packet)
            && self.control_drops.fetch_add(1) == 0)
            return {.bytes_transferred = bytes.size()};
        if (decoded && self.drop_selection
            && hn::decode_exchange_packet(decoded.packet, record)
            && record.kind == hn::ExchangeKind::selection
            && self.dropped.fetch_add(1) == 0)
            return {.bytes_transferred = bytes.size()};
        const auto sent = self.channel->socket.send_to(bytes, peer);
        if (sent && decoded && decoded.packet.kind == PacketKind::control
            && decoded.packet.control.subtype == authenticated_control_subtype
            && hn::native_candidate(decoded.packet)) {
            std::lock_guard lock(self.mutex);
            self.last_control.assign(bytes.begin(), bytes.end());
        }
        return sent;
    }
};
#endif
}

#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_HYBRID_CONTROL
struct StartupProvision {
    bool caller = false, allow = true, join_success = false;
    std::byte secret = std::byte {0x22};
    SRTSOCKET id = SRT_INVALID_SOCK;
    std::atomic<unsigned> enrolls = 0, joins = 0;
    std::atomic_bool hidden = true, io_hidden = true;
    std::uint64_t expiry = hn::NativeService::monotonic_now() + 5'000'000;
    std::atomic<unsigned> recovery_failures {0};
    static bool resolve(void* p, const hn::CredentialRequest& r,
        hn::Authorization& a, std::span<std::byte, 32> secret) noexcept
    {
        auto& self = *static_cast<StartupProvision*>(p);
        if (!self.allow)
            return false;
        a = {.credential_id = r.credential_id,
            .caller_group = r.caller_group,
            .listener_group = r.listener_group,
            .stream_id = r.stream_id,
            .generation = 7,
            .expires_at_us = self.expiry};
        std::fill(secret.begin(), secret.end(), self.secret);
        return true;
    }
    static bool revalidate(void* p, const hn::CredentialRequest&,
        const hn::Authorization& old, hn::Authorization& a) noexcept
    {
        auto& self = *static_cast<StartupProvision*>(p);
        a = old;
        return self.allow && a.expires_at_us == self.expiry;
    }
    static bool enroll(void* p, const hn::Context&, const hn::Authorization&,
        std::uint64_t) noexcept
    {
        auto& self = *static_cast<StartupProvision*>(p);
        ++self.enrolls;
        if (self.caller) {
            const auto group = GroupRegistry::instance().find(self.id);
            if (!group)
                return false;
            std::vector<SRTSOCKET> ids;
            {
                std::lock_guard lock(group->mutex);
                for (const auto& member : group->members)
                    ids.push_back(member.public_data.id);
            }
            for (auto id : ids) {
                if (srt_getsockstate(id) != SRTS_CONNECTING)
                    self.hidden = false;
                if (srt_sendmsg(id, "x", 1, -1, 1) != SRT_ERROR
                    || srt_getlasterror(nullptr) != SRT_ENOCONN)
                    self.io_hidden = false;
            }
        }
        return self.allow;
    }
    static std::optional<UdpSocket> join(
        void* p, const hn::MembershipRecovery::Request& request) noexcept
    {
        auto& self = *static_cast<StartupProvision*>(p);
        ++self.joins;
        if (request.recovery && self.recovery_failures.load()) {
            --self.recovery_failures;
            return {};
        }
        if (srt_getsockstate(self.id) != SRTS_CONNECTED)
            self.hidden = false;
        return self.join_success ? hn::MembershipRecovery::prepare(request)
                                 : std::nullopt;
    }
    hn::StartupOptions options(const std::shared_ptr<StartupProvision>& self)
    {
        hn::StartupOptions o;
        o.native.owner = self;
        o.native.resolve = resolve;
        o.native.revalidate = revalidate;
        o.native.enroll = enroll;
        o.native.credential_id.fill(std::byte {0x11});
        o.native.policy.supported |= hn::startup_fallback | hn::policy_query;
        o.native.policy.required |= hn::startup_fallback | hn::policy_query;
        o.native.retry_interval_us = 10'000;
        o.native.timeout_us = 300'000;
        o.native.maximum_sends_per_stage = 8;
        o.admission_timeout_us = 600'000;
        o.poll_interval_us = 2'000;
        o.membership = {IpEndpoint::loopback(), 3, 500'000, 20'000, 40'000};
        o.join = {self, join};
        o.receiver = {.retry_interval_us = 10'000,
            .round_timeout_us = 40'000,
            .operation_timeout_us = 240'000,
            .query_interval_us = 50'000,
            .confirmation_lifetime_us = 80'000,
            .maximum_sends = 2,
            .maximum_rounds = 4};
        return o;
    }
};
struct StartupHooks {
    std::shared_ptr<StartupProvision> source =
                                          std::make_shared<StartupProvision>(),
                                      caller =
                                          std::make_shared<StartupProvision>();
    HybridWireHook wire;
    std::int32_t connect_timeout = 0;
    hn::ReportingConfiguration reporting;
    std::uint32_t maximum_recoveries = 0;
    std::optional<hn::ReturnPolicy> returning;
    bool network_timing = false;
    static void configure(void* p, SRTSOCKET id, bool caller)
    {
        auto& self = *static_cast<StartupHooks*>(p);
        const auto owner = caller ? self.caller : self.source;
        owner->caller = caller;
        owner->id = id;
        auto options = owner->options(owner);
        options.source.reporting = options.receiver.reporting = self.reporting;
        options.membership.maximum_recoveries = self.maximum_recoveries;
        if (self.returning) {
            options.native.policy.supported |= hn::return_control | hn::overlap;
            options.source.returning = self.returning;
        }
        if (self.network_timing) {
            options.admission_timeout_us = 1'500'000;
            options.native.timeout_us = 600'000;
            options.native.retry_interval_us = 20'000;
            options.receiver.retry_interval_us = 20'000;
            options.receiver.round_timeout_us = 120'000;
            options.receiver.operation_timeout_us = 600'000;
            options.receiver.confirmation_lifetime_us = 250'000;
            options.membership.timeout_us = 1'000'000;
        }
        if (self.connect_timeout) {
            options.admission_timeout_us = 1'000'000;
            options.native.timeout_us = 900'000;
            if (caller)
                REQUIRE_EQ(
                    srt_setsockflag(id, SRTO_CONNTIMEO, &self.connect_timeout,
                        sizeof(self.connect_timeout)),
                    0);
        }
        REQUIRE(hn::provision_startup(id, std::move(options)));
        if (!caller) {
            const auto socket = SocketRegistry::instance().find(id);
            {
                std::lock_guard lock(socket->mutex);
                self.wire.channel = socket->channel;
            }
            self.wire.drop_commits = true;
            self.wire.channel->set_send_hook_for_testing(
                HybridWireHook::send, &self.wire);
        }
    }
    ~StartupHooks()
    {
        if (wire.channel)
            wire.channel->set_send_hook_for_testing(nullptr, nullptr);
    }
};
struct StartupServer {
    Peers peers;
    AdmissionLog admissions;
    SRTSOCKET listener = SRT_INVALID_SOCK;
    sockaddr_in bound {};
    StartupServer(StartupHooks* hooks, bool encrypted = false,
        bool authentication = false)
    {
        listener = peers.keep(srt_create_socket());
        const bool yes = true;
        auto local = ipv4_sockaddr({127, 0, 0, 1}, 0);
        auto group = ipv4_sockaddr({239, 255, 133, 11}, multicast_port);
        for (auto option : {SRTO_ROBOTWEAX_MCAST_INDEPENDENT,
                 SRTO_ROBOTWEAX_MCAST_COORDINATED})
            REQUIRE_EQ(srt_setsockflag(listener, option, &yes, sizeof(yes)), 0);
        REQUIRE_EQ(srt_setsockflag(listener, SRTO_ROBOTWEAX_MCAST_SOURCE,
                       &local, sizeof(local)),
            0);
        REQUIRE_EQ(srt_setsockflag(listener, SRTO_ROBOTWEAX_MCAST_IFACE, &local,
                       sizeof(local)),
            0);
        REQUIRE_EQ(srt_setsockflag(listener, SRTO_ROBOTWEAX_MCAST_GROUP, &group,
                       sizeof(group)),
            0);
        const std::int32_t ttl = 0;
        const std::int64_t rate = 1'000'000;
        REQUIRE_EQ(srt_setsockflag(
                       listener, SRTO_ROBOTWEAX_MCAST_TTL, &ttl, sizeof(ttl)),
            0);
        REQUIRE_EQ(
            srt_setsockflag(listener, SRTO_MAXBW, &rate, sizeof(rate)), 0);
        if (encrypted) {
            REQUIRE_EQ(srt_setsockflag(listener, SRTO_PASSPHRASE,
                           group_passphrase, sizeof(group_passphrase) - 1),
                0);
            const std::int32_t refresh = 32, preannounce = 8;
            REQUIRE_EQ(srt_setsockflag(listener, SRTO_KMREFRESHRATE, &refresh,
                           sizeof(refresh)),
                0);
            REQUIRE_EQ(srt_setsockflag(listener, SRTO_KMPREANNOUNCE,
                           &preannounce, sizeof(preannounce)),
                0);
        }
        if (authentication)
            REQUIRE_EQ(srt_setsockflag(listener, SRTO_ROBOTWEAX_SESSIONAUTH,
                           &yes, sizeof(yes)),
                0);
        REQUIRE_EQ(
            srt_listen_callback(listener, admit_dispatch, &admissions), 0);
        REQUIRE_EQ(srt_bind(listener, reinterpret_cast<const sockaddr*>(&local),
                       sizeof(local)),
            0);
        if (hooks)
            StartupHooks::configure(hooks, listener, false);
        REQUIRE_EQ(srt_listen(listener, 8), 0);
        int size = sizeof(bound);
        REQUIRE_EQ(srt_getsockname(
                       listener, reinterpret_cast<sockaddr*>(&bound), &size),
            0);
    }
};
std::shared_ptr<hn::StartupSession> startup_owner(SRTSOCKET id)
{
    auto group = GroupRegistry::instance().find(id);
    REQUIRE(group);
    SRTSOCKET member = SRT_INVALID_SOCK;
    {
        std::lock_guard lock(group->mutex);
        REQUIRE(!group->members.empty());
        member = group->members.front().public_data.id;
    }
    const auto socket = SocketRegistry::instance().find(member);
    REQUIRE(socket);
    std::lock_guard lock(socket->mutex);
    return socket->hybrid_startup_session;
}
TEST(
    multicast_dispatch_native_startup_setup_waits_for_uc_before_connected_plain_ctr_gcm)
{
    SKIP_UNLESS(loopback_multicast_available(), "no loopback multicast");
    for (unsigned mode = 0; mode < 3; ++mode) {
#ifndef ENABLE_AEAD_API_PREVIEW
        if (mode == 2)
            continue;
#endif
        StartupHooks hooks;
        CoordinatedNative f(1'000'000, mode != 0, mode == 2, 0, 120, true,
            false, StartupHooks::configure, &hooks);
        REQUIRE(hooks.caller->hidden.load());
        REQUIRE(hooks.caller->io_hidden.load());
        REQUIRE_EQ(hooks.caller->enrolls.load(), 1U);
        REQUIRE(hooks.wire.commits_dropped.load() > 0);
        REQUIRE_EQ(f.actor()->snapshot().unicast_members, 1U);
        const auto group = GroupRegistry::instance().find(f.receiver);
        std::shared_ptr<hn::StartupSession> startup;
        {
            std::lock_guard lock(group->mutex);
            const auto socket = SocketRegistry::instance().find(
                group->members.front().public_data.id);
            std::lock_guard record(socket->mutex);
            startup = socket->hybrid_startup_session;
        }
        REQUIRE(startup);
        REQUIRE(startup->snapshot().published);
        REQUIRE(startup->receiver());
        for (int i = 0; i < 80; ++i)
            REQUIRE_EQ(f.send(i), static_cast<int>(message(i).size()));
        expect_messages(f.receiver, 0, 80);
        REQUIRE_EQ(srt_getsockstate(f.receiver), SRTS_CONNECTED);
        REQUIRE(hooks.caller->joins.load() > 0);
        REQUIRE(!startup->membership()->snapshot().joined);
        if (mode == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds {650});
            REQUIRE_EQ(startup->snapshot().phase,
                hn::StartupSession::Phase::connected);
            REQUIRE(startup->receiver()->snapshot().fresh);
            REQUIRE_EQ(f.send(80), static_cast<int>(message(80).size()));
            expect_messages(f.receiver, 80, 1);
        }
    }
}
TEST(
    multicast_dispatch_native_startup_setup_async_real_join_and_group_encryption)
{
    SKIP_UNLESS(loopback_multicast_available(), "no loopback multicast");
    StartupHooks hooks;
    hooks.caller->join_success = true;
    StartupServer server(&hooks, true, false);
    int result = 0;
    const auto receiver = make_receiver(server.peers, server.bound, true, false,
        120, true, true, false, StartupHooks::configure, &hooks, &result);
    REQUIRE(result != SRT_ERROR);
    REQUIRE(srt_getsockstate(receiver) != SRTS_CONNECTED);
    const auto sender =
        server.peers.keep(srt_accept(server.listener, nullptr, nullptr));
    REQUIRE(sender != SRT_INVALID_SOCK);
    REQUIRE(wait_for_members(receiver, 1, true));
    auto owner = startup_owner(receiver);
    REQUIRE(owner);
    REQUIRE(owner->snapshot().published);
    const auto until =
        std::chrono::steady_clock::now() + std::chrono::seconds {1};
    while (!owner->membership()->snapshot().joined
        && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(std::chrono::milliseconds {1});
    REQUIRE(owner->membership()->snapshot().joined);
    REQUIRE(hooks.caller->hidden.load());
    const bool blocking = true;
    REQUIRE_EQ(
        srt_setsockflag(receiver, SRTO_RCVSYN, &blocking, sizeof(blocking)), 0);
    for (int i = 0; i < 80; ++i) {
        const auto data = message(i);
        REQUIRE_EQ(srt_sendmsg(sender, data.data(), data.size(), -1, 1),
            static_cast<int>(data.size()));
    }
    expect_messages(receiver, 0, 80);
}
TEST(multicast_dispatch_native_startup_setup_rejects_missing_or_bad_credentials)
{
    SKIP_UNLESS(loopback_multicast_available(), "no loopback multicast");
    for (unsigned mode = 0; mode < 3; ++mode) {
        StartupHooks hooks;
        if (mode == 1)
            hooks.source->allow = false;
        if (mode == 2)
            hooks.caller->secret = std::byte {0x23};
        StartupServer server(mode == 0 ? nullptr : &hooks);
        int result = 0;
        const auto receiver =
            make_receiver(server.peers, server.bound, false, false, 120, true,
                false, false, StartupHooks::configure, &hooks, &result);
        REQUIRE_EQ(result, SRT_ERROR);
        REQUIRE(srt_getsockstate(receiver) != SRTS_CONNECTED);
        REQUIRE_EQ(hooks.caller->joins.load(), 0U);
    }
}
TEST(multicast_dispatch_native_startup_setup_rejects_exhausted_sender_budget)
{
    SKIP_UNLESS(loopback_multicast_available(), "no loopback multicast");
    StartupHooks hooks;
    StartupServer server(&hooks);
    std::vector<SRTSOCKET> receivers;
    for (unsigned i = 0; i < 4; ++i) {
        hooks.caller = std::make_shared<StartupProvision>();
        receivers.push_back(make_receiver(server.peers, server.bound, false,
            false, 120, true, false, false, StartupHooks::configure, &hooks));
    }
    const auto sender =
        server.peers.keep(srt_accept(server.listener, nullptr, nullptr));
    REQUIRE(sender != SRT_INVALID_SOCK);
    hooks.caller = std::make_shared<StartupProvision>();
    int result = 0;
    const auto rejected =
        make_receiver(server.peers, server.bound, false, false, 120, true,
            false, false, StartupHooks::configure, &hooks, &result);
    REQUIRE_EQ(result, SRT_ERROR);
    REQUIRE(srt_getsockstate(rejected) != SRTS_CONNECTED);
    REQUIRE_EQ(hooks.caller->joins.load(), 0U);
    const auto group = GroupRegistry::instance().find(sender);
    std::shared_ptr<robotweax::srt::compat::GroupSendCoordinator> actor;
    {
        std::lock_guard lock(group->mutex);
        actor = group->dispatch->coordinator;
    }
    REQUIRE_EQ(actor->snapshot().unicast_members, 4U);
    for (int i = 0; i < 20; ++i) {
        const auto data = message(i);
        REQUIRE_EQ(srt_sendmsg(sender, data.data(), data.size(), -1, 1),
            static_cast<int>(data.size()));
    }
    for (auto receiver : receivers)
        expect_messages(receiver, 0, 20);
}
TEST(multicast_dispatch_native_startup_setup_close_and_reply_loss_never_publish)
{
    SKIP_UNLESS(loopback_multicast_available(), "no loopback multicast");
    for (bool close : {false, true}) {
        StartupHooks hooks;
        StartupServer server(&hooks);
        hooks.wire.drop_controls = true;
        int result = 0;
        const auto receiver =
            make_receiver(server.peers, server.bound, false, false, 120, true,
                true, false, StartupHooks::configure, &hooks, &result);
        REQUIRE(result != SRT_ERROR);
        const auto sender =
            server.peers.keep(srt_accept(server.listener, nullptr, nullptr));
        REQUIRE(sender != SRT_INVALID_SOCK);
        std::shared_ptr<hn::StartupSession> owner;
        const auto until =
            std::chrono::steady_clock::now() + std::chrono::seconds {1};
        while (std::chrono::steady_clock::now() < until) {
            owner = startup_owner(receiver);
            if (owner
                && owner->snapshot().phase
                    == hn::StartupSession::Phase::admitting)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds {1});
        }
        REQUIRE(owner);
        REQUIRE(!owner->snapshot().published);
        REQUIRE(srt_getsockstate(receiver) != SRTS_CONNECTED);
        const auto source_group = GroupRegistry::instance().find(sender);
        std::shared_ptr<robotweax::srt::compat::GroupSendCoordinator> actor;
        {
            std::lock_guard lock(source_group->mutex);
            actor = source_group->dispatch->coordinator;
        }
        while (!actor->snapshot().unicast_members
            && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds {1});
        REQUIRE_EQ(actor->snapshot().unicast_members, 1U);
        REQUIRE(!owner->snapshot().published);
        const auto pending_data = message(0);
        REQUIRE_EQ(srt_sendmsg(
                       sender, pending_data.data(), pending_data.size(), -1, 1),
            static_cast<int>(pending_data.size()));
        const auto caller_group = GroupRegistry::instance().find(receiver);
        SRTSOCKET child = SRT_INVALID_SOCK;
        {
            std::lock_guard lock(caller_group->mutex);
            child = caller_group->members.front().public_data.id;
        }
        const auto record = SocketRegistry::instance().find(child);
        std::shared_ptr<robotweax::srt::compat::ConnectionRuntime> runtime;
        {
            std::lock_guard lock(record->mutex);
            runtime = record->runtime;
        }
        while (!runtime->statistics(false, false).total.received_unique.packets
            && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds {1});
        REQUIRE(runtime->statistics(false, false).total.received_unique.packets
            > 0);
        char buffer[64];
        REQUIRE_EQ(srt_recvmsg(child, buffer, sizeof(buffer)), SRT_ERROR);
        REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ENOCONN);
        REQUIRE(!(robotweax::srt::compat::socket_readiness(child).events
            & (SRT_EPOLL_IN | SRT_EPOLL_OUT)));
        REQUIRE(!(robotweax::srt::compat::socket_readiness(receiver).events
            & (SRT_EPOLL_IN | SRT_EPOLL_OUT)));
        if (close)
            server.peers.drop(receiver);
        else {
            while (srt_getsockstate(receiver) == SRTS_CONNECTING
                && std::chrono::steady_clock::now() < until)
                std::this_thread::sleep_for(std::chrono::milliseconds {1});
        }
        hooks.wire.drop_controls = false;
        std::this_thread::sleep_for(std::chrono::milliseconds {20});
        REQUIRE(!owner->snapshot().published);
        REQUIRE(srt_getsockstate(receiver) != SRTS_CONNECTED);
        REQUIRE_EQ(hooks.caller->joins.load(), 0U);
    }
}
TEST(multicast_dispatch_native_startup_setup_keeps_original_connect_deadline)
{
    SKIP_UNLESS(loopback_multicast_available(), "no loopback multicast");
    StartupHooks hooks;
    hooks.connect_timeout = 120;
    StartupServer server(&hooks);
    hooks.wire.drop_controls = true;
    int result = 0;
    const auto begin = std::chrono::steady_clock::now();
    const auto receiver =
        make_receiver(server.peers, server.bound, false, false, 120, true,
            false, false, StartupHooks::configure, &hooks, &result);
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    REQUIRE_EQ(result, SRT_ERROR);
    REQUIRE(elapsed < std::chrono::milliseconds {600});
    hooks.wire.drop_controls = false;
    std::this_thread::sleep_for(std::chrono::milliseconds {50});
    REQUIRE(srt_getsockstate(receiver) != SRTS_CONNECTED);
    REQUIRE_EQ(hooks.caller->joins.load(), 0U);
}
TEST(
    multicast_dispatch_native_startup_setup_preserves_unprovisioned_join_failure)
{
    SKIP_UNLESS(loopback_multicast_available(), "no loopback multicast");
    StartupServer server(nullptr);
    int result = 0;
    auto bad_interface = +[](void*, SRTSOCKET id, bool caller) {
        if (caller) {
            const auto address = ipv4_sockaddr({192, 0, 2, 77}, 0);
            REQUIRE_EQ(srt_setsockflag(id, SRTO_ROBOTWEAX_MCAST_IFACE, &address,
                           sizeof(address)),
                0);
        }
    };
    const auto receiver = make_receiver(server.peers, server.bound, false,
        false, 120, true, false, false, bad_interface, nullptr, &result);
    REQUIRE_EQ(result, SRT_ERROR);
    REQUIRE(srt_getsockstate(receiver) != SRTS_CONNECTED);
}
TEST(
    multicast_dispatch_native_hybrid_transport_plain_ctr_gcm_keeps_delivery_policy_explicit)
{
    SKIP_UNLESS(loopback_multicast_available(), "no loopback multicast");
    for (unsigned mode = 0; mode < 3; ++mode) {
#ifndef ENABLE_AEAD_API_PREVIEW
        if (mode == 2)
            continue;
#endif
        CoordinatedNative f(1'000'000, mode != 0, mode == 2);
        NativeHybridPair pair(f);
        pair.start();
        REQUIRE(pair.wait_active());
        REQUIRE_EQ(pair.co->resolves.load(), 1U);
        REQUIRE_EQ(pair.lo->resolves.load(), 1U);
        REQUIRE(pair.c->snapshot().generation != pair.l->snapshot().generation);
        REQUIRE_EQ(pair.c->send(pair.request(),
                       hn::NativeService::monotonic_now() + 500'000),
            hn::NativeService::Admission::accepted);
        hn::NativeService::Event e;
        REQUIRE(pair.event(pair.l, e));
        REQUIRE_EQ(e.received.status, hn::Session::Status::accepted);
        REQUIRE_EQ(e.message.kind, hn::Kind::fallback_request);
        std::array<robotweax::srt::compat::GroupSendCoordinator::Member, 1>
            member;
        REQUIRE_EQ(f.actor()->members(member), 1U);
        REQUIRE_EQ(member[0].delivery, GroupOriginalDelivery::multicast);
        REQUIRE(!hn::attach_native_control(pair.caller, pair.options(true)));
        for (int i = 0; i < 5; ++i)
            REQUIRE_EQ(f.send(i), static_cast<int>(message(i).size()));
        expect_messages(f.receiver, 0, 5);
    }
}
TEST(
    multicast_dispatch_native_hybrid_loss_and_provenance_fences_preserve_control_session)
{
    SKIP_UNLESS(loopback_multicast_available(), "no loopback multicast");
    CoordinatedNative f;
    NativeHybridPair pair(f);
    const auto source = SocketRegistry::instance().find(f.listener);
    REQUIRE(source);
    std::shared_ptr<robotweax::srt::compat::DatagramChannel> channel;
    {
        std::lock_guard lock(source->mutex);
        channel = source->channel;
    }
    HybridWireHook hook;
    hook.channel = channel;
    hook.drop_selection = true;
    SendHookReset reset {channel};
    channel->set_send_hook_for_testing(HybridWireHook::send, &hook);
    pair.start();
    REQUIRE(pair.wait_active());
    REQUIRE(hook.dropped.load() >= 1U);
    REQUIRE_EQ(pair.lo->resolves.load(), 1U);
    REQUIRE_EQ(pair.co->resolves.load(), 1U);
    const auto caller_group = GroupRegistry::instance().find(f.receiver);
    SRTSOCKET caller_id = SRT_INVALID_SOCK;
    {
        std::lock_guard lock(caller_group->mutex);
        caller_id = caller_group->members.front().public_data.id;
    }
    const auto caller_socket = SocketRegistry::instance().find(caller_id);
    std::shared_ptr<robotweax::srt::compat::DatagramChannel> cc;
    IpEndpoint peer;
    {
        std::lock_guard lock(caller_socket->mutex);
        cc = caller_socket->channel;
        peer = caller_socket->local_endpoint;
    }
    HybridWireHook sender_hook;
    sender_hook.channel = cc;
    SendHookReset cr {cc};
    cc->set_send_hook_for_testing(HybridWireHook::send, &sender_hook);
    REQUIRE_EQ(pair.c->send(pair.request(),
                   hn::NativeService::monotonic_now() + 500'000),
        hn::NativeService::Admission::accepted);
    hn::NativeService::Event e;
    REQUIRE(pair.event(pair.l, e));
    std::vector<std::byte> control;
    {
        std::lock_guard lock(sender_hook.mutex);
        control = sender_hook.last_control;
    }
    REQUIRE(!control.empty());
    const auto packet = decode_packet(control);
    REQUIRE(packet);
    const auto received = pair.l->snapshot().received;
    pair.listener->process_packet(packet.packet, peer, std::nullopt,
        robotweax::srt::compat::PacketIngress::multicast_data);
    auto foreign = peer;
    ++foreign.port;
    pair.listener->process_packet(packet.packet, foreign);
    std::this_thread::sleep_for(std::chrono::milliseconds {10});
    REQUIRE_EQ(pair.l->snapshot().received, received);
    for (int i = 0; i < 3; ++i)
        REQUIRE_EQ(f.send(i), static_cast<int>(message(i).size()));
    expect_messages(f.receiver, 0, 3);
}
TEST(
    multicast_dispatch_native_hybrid_expiry_denial_and_reentrant_close_are_visible)
{
    SKIP_UNLESS(loopback_multicast_available(), "no loopback multicast");
    {
        CoordinatedNative f;
        NativeHybridPair pair(f, 150'000);
        pair.start();
        REQUIRE(pair.wait_active());
        const auto until =
            std::chrono::steady_clock::now() + std::chrono::seconds {2};
        while (!pair.c->closed() && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds {1});
        REQUIRE(pair.c->closed());
        REQUIRE_EQ(srt_getsockstate(f.receiver), SRTS_CONNECTED);
        REQUIRE_EQ(f.send(0), static_cast<int>(message(0).size()));
        expect_messages(f.receiver, 0, 1);
    }
    for (bool close : {false, true}) {
        CoordinatedNative f;
        NativeHybridPair pair(f);
        pair.lo->approve = false;
        pair.lo->close_on_enroll = close;
        pair.start();
        REQUIRE(!pair.wait_active());
        REQUIRE(pair.l->snapshot().closed);
        REQUIRE_EQ(pair.lo->enrolls.load(), 1U);
    }
}
TEST(
    multicast_dispatch_native_hybrid_coexists_with_primary_session_authentication_and_km)
{
    SKIP_UNLESS(loopback_multicast_available(), "no loopback multicast");
    CoordinatedNative f(1'000'000, true, false, 0, 120, false, true);
    NativeHybridPair pair(f);
    pair.start();
    REQUIRE(pair.wait_active());
    REQUIRE_EQ(pair.c->send(pair.request(),
                   hn::NativeService::monotonic_now() + 500'000),
        hn::NativeService::Admission::accepted);
    hn::NativeService::Event e;
    REQUIRE(pair.event(pair.l, e));
    REQUIRE_EQ(e.received.status, hn::Session::Status::accepted);
    for (int i = 0; i < 48; ++i) {
        REQUIRE_EQ(f.send(i), static_cast<int>(message(i).size()));
        std::this_thread::sleep_for(std::chrono::milliseconds {1});
    }
    expect_messages(f.receiver, 0, 48);
    REQUIRE(pair.c->snapshot().active);
    REQUIRE(pair.l->snapshot().active);
}
#endif

#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_HYBRID_CONTROL
TEST(
    multicast_dispatch_native_coordinator_bridge_fallback_and_lost_commit_plain_ctr_gcm)
{
    SKIP_UNLESS(loopback_multicast_available(), "no loopback multicast");
    for (unsigned mode = 0; mode < 3; ++mode) {
#ifndef ENABLE_AEAD_API_PREVIEW
        if (mode == 2)
            continue;
#endif
        CoordinatedNative f(1'000'000, mode != 0, mode == 2);
        NativeHybridPair pair(f);
        const auto identity =
            hn::CoordinatorBridge::identity(f.actor(), pair.listener);
        REQUIRE(identity);
        pair.start(identity);
        REQUIRE(pair.wait_active());
        hn::Authorization grant;
        {
            std::lock_guard lock(pair.lo->grant_mutex);
            grant = pair.lo->resolved_grant;
        }
        auto lease = std::make_shared<hn::AuthorizationLease>(grant);
        std::shared_ptr<hn::CoordinatorBridge> bridge;
        const auto until =
            std::chrono::steady_clock::now() + std::chrono::seconds {1};
        while (!bridge && std::chrono::steady_clock::now() < until) {
            bridge =
                hn::CoordinatorBridge::create(f.actor(), pair.l, lease, {});
            if (!bridge)
                std::this_thread::sleep_for(std::chrono::milliseconds {1});
        }
        REQUIRE(bridge);
        const auto record = SocketRegistry::instance().find(f.listener);
        std::shared_ptr<robotweax::srt::compat::DatagramChannel> control;
        {
            std::lock_guard lock(record->mutex);
            control = record->channel;
        }
        HybridWireHook hook;
        hook.channel = control;
        hook.drop_first_control = true;
        SendHookReset reset {control};
        control->set_send_hook_for_testing(HybridWireHook::send, &hook);
        const auto request = pair.request();
        REQUIRE_EQ(
            pair.c->send(request, hn::NativeService::monotonic_now() + 500'000),
            hn::NativeService::Admission::accepted);
        const auto first =
            std::chrono::steady_clock::now() + std::chrono::milliseconds {500};
        while (!hook.control_drops.load()
            && std::chrono::steady_clock::now() < first) {
            bridge->pump();
            std::this_thread::sleep_for(std::chrono::milliseconds {1});
        }
        REQUIRE(hook.control_drops.load());
        REQUIRE_EQ(bridge->snapshot().applied, 1U);
        REQUIRE_EQ(
            pair.c->send(request, hn::NativeService::monotonic_now() + 500'000),
            hn::NativeService::Admission::accepted);
        hn::NativeService::Event event;
        bool received = false;
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds {500};
        while (!received && std::chrono::steady_clock::now() < deadline) {
            bridge->pump();
            while (pair.c->pop(event))
                if (event.kind == hn::NativeService::Event::Kind::received) {
                    received = true;
                    break;
                }
            if (!received)
                std::this_thread::sleep_for(std::chrono::milliseconds {1});
        }
        REQUIRE(received);
        REQUIRE_EQ(event.message.kind, hn::Kind::delivery_commit);
        REQUIRE_EQ(event.message.revision, 1U);
        REQUIRE_EQ(bridge->snapshot().applied, 1U);
        REQUIRE_EQ(bridge->snapshot().duplicates, 1U);
        REQUIRE_EQ(pair.co->resolves.load(), 1U);
        REQUIRE_EQ(pair.lo->resolves.load(), 1U);
        auto source = f.source();
        SendHookReset sr {source};
        source->set_send_hook_for_testing(
            [](std::span<const std::byte> b, IpEndpoint,
                void*) noexcept -> UdpIoResult {
                return {.bytes_transferred = b.size()};
            },
            nullptr);
        for (int i = 0; i < 48; ++i)
            REQUIRE_EQ(f.send(i), static_cast<int>(message(i).size()));
        expect_messages(f.receiver, 0, 48);
        REQUIRE_EQ(f.actor()->snapshot().unicast_members, 1U);
        lease->revoke();
        REQUIRE(pair.l->closed());
        REQUIRE_EQ(f.send(48), static_cast<int>(message(48).size()));
        expect_messages(f.receiver, 48, 1);
    }
}
#endif

#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_HYBRID_CONTROL
namespace {
struct NativeReceiverOwners {
    NativeHybridPair pair;
    std::shared_ptr<hn::AuthorizationLease> lease;
    std::shared_ptr<hn::CoordinatorBridge> sender;
    std::shared_ptr<hn::ReceiverBridge> receiver;
    explicit NativeReceiverOwners(CoordinatedNative& f,
        hn::ReportingConfiguration reporting = {},
        std::optional<hn::DetectionConfiguration> detection = std::nullopt,
        std::optional<hn::ReturnPolicy> returning = std::nullopt,
        bool startup = false)
        : pair(f)
    {
        const auto identity =
            hn::CoordinatorBridge::identity(f.actor(), pair.listener);
        REQUIRE(identity);
        pair.start(identity, true, returning.has_value(), startup);
        REQUIRE(pair.wait_active());
        hn::Authorization grant;
        {
            std::lock_guard lock(pair.lo->grant_mutex);
            grant = pair.lo->resolved_grant;
        }
        lease = std::make_shared<hn::AuthorizationLease>(grant);
        const auto until =
            std::chrono::steady_clock::now() + std::chrono::seconds {1};
        while (!sender && std::chrono::steady_clock::now() < until) {
            sender = hn::CoordinatorBridge::create(f.actor(), pair.l, lease,
                {.reporting = reporting, .returning = returning});
            if (!sender)
                std::this_thread::sleep_for(std::chrono::milliseconds {1});
        }
        REQUIRE(sender);
        while (!receiver && std::chrono::steady_clock::now() < until) {
            receiver = hn::ReceiverBridge::create(pair.c,
                {.reporting = reporting,
                    .detection = detection,
                    .retry_interval_us = 10'000,
                    .round_timeout_us = 40'000,
                    .operation_timeout_us = 240'000,
                    .query_interval_us = 50'000,
                    .confirmation_lifetime_us = 80'000,
                    .maximum_sends = 2,
                    .maximum_rounds = 4});
            if (!receiver)
                std::this_thread::sleep_for(std::chrono::milliseconds {1});
        }
        REQUIRE(receiver);
    }
    template <class F> void wait(F predicate)
    {
        const auto until =
            std::chrono::steady_clock::now() + std::chrono::seconds {1};
        while (std::chrono::steady_clock::now() < until) {
            if (predicate())
                return;
            receiver->pump();
            sender->pump();
            std::this_thread::sleep_for(std::chrono::milliseconds {1});
        }
        // Observation getters may intentionally return no sample while a
        // runtime is busy. A successful sample completes the wait once.
        if (predicate())
            return;
        throw std::runtime_error("native receiver wait: sender reports="
            + std::to_string(sender->snapshot().reports_queued) + " skips="
            + std::to_string(sender->snapshot().reports_skipped) + " closed="
            + std::to_string(sender->snapshot().closed) + " receiver reports="
            + std::to_string(receiver->snapshot().reports_queued)
            + " skips=" + std::to_string(receiver->snapshot().reports_skipped)
            + " closed=" + std::to_string(receiver->snapshot().closed));
    }
};
}
TEST(multicast_dispatch_native_receiver_reconciles_lost_commits_plain_ctr_gcm)
{
    SKIP_UNLESS(loopback_multicast_available(), "no loopback multicast");
    for (unsigned mode = 0; mode < 3; ++mode) {
#ifndef ENABLE_AEAD_API_PREVIEW
        if (mode == 2)
            continue;
#endif
        CoordinatedNative f(1'000'000, mode != 0, mode == 2);
        NativeReceiverOwners owners(f);
        const auto socket = SocketRegistry::instance().find(f.listener);
        std::shared_ptr<robotweax::srt::compat::DatagramChannel> control;
        {
            std::lock_guard lock(socket->mutex);
            control = socket->channel;
        }
        HybridWireHook hook;
        hook.channel = control;
        hook.drop_commits = true;
        SendHookReset reset {control};
        control->set_send_hook_for_testing(HybridWireHook::send, &hook);
        REQUIRE_EQ(owners.receiver->request_fallback(
                       owners.receiver->snapshot().cursor),
            hn::ReceiverBridge::Admission::accepted);
        owners.wait([&] {
            const auto s = owners.receiver->snapshot();
            return s.fresh && s.mode == hn::Mode::unicast;
        });
        REQUIRE(hook.commits_dropped.load() > 0);
        REQUIRE(owners.receiver->snapshot().queries > 0);
        REQUIRE(!owners.receiver->snapshot().has_commit);
        REQUIRE_EQ(owners.sender->snapshot().applied, 1U);
        const auto original_revision = owners.receiver->snapshot().revision;
        const auto initial_confirmations =
            owners.receiver->snapshot().confirmed;
        owners.wait([&] {
            return owners.receiver->snapshot().confirmed
                > initial_confirmations;
        });
        REQUIRE_EQ(owners.sender->snapshot().applied, 1U);
        REQUIRE_EQ(owners.receiver->snapshot().revision, original_revision);
        auto source = f.source();
        SendHookReset source_reset {source};
        source->set_send_hook_for_testing(
            [](std::span<const std::byte> b, IpEndpoint,
                void*) noexcept -> UdpIoResult {
                return {.bytes_transferred = b.size()};
            },
            nullptr);
        for (int i = 0; i < 48; ++i)
            REQUIRE_EQ(f.send(i), static_cast<int>(message(i).size()));
        expect_messages(f.receiver, 0, 48);
        owners.receiver->close();
        REQUIRE_EQ(srt_getsockstate(f.receiver), SRTS_CONNECTED);
        REQUIRE_EQ(f.send(48), static_cast<int>(message(48).size()));
        expect_messages(f.receiver, 48, 1);
    }
}
TEST(
    multicast_dispatch_native_receiver_missing_queries_remain_unknown_without_fallback)
{
    SKIP_UNLESS(loopback_multicast_available(), "no loopback multicast");
    CoordinatedNative f;
    NativeReceiverOwners owners(f);
    owners.wait([&] {
        return owners.receiver->snapshot().fresh;
    });
    const auto socket = SocketRegistry::instance().find(f.listener);
    std::shared_ptr<robotweax::srt::compat::DatagramChannel> control;
    {
        std::lock_guard lock(socket->mutex);
        control = socket->channel;
    }
    HybridWireHook hook;
    hook.channel = control;
    hook.drop_controls = true;
    SendHookReset reset {control};
    control->set_send_hook_for_testing(HybridWireHook::send, &hook);
    owners.wait([&] {
        const auto s = owners.receiver->snapshot();
        return !s.fresh && s.exhausted > 0;
    });
    REQUIRE_EQ(owners.sender->snapshot().applied, 0U);
    REQUIRE_EQ(f.actor()->snapshot().unicast_members, 0U);
    for (int i = 0; i < 3; ++i)
        REQUIRE_EQ(f.send(i), static_cast<int>(message(i).size()));
    expect_messages(f.receiver, 0, 3);
    hook.drop_controls = false;
    owners.wait([&] {
        return owners.receiver->snapshot().fresh;
    });
    REQUIRE_EQ(owners.sender->snapshot().applied, 0U);
    REQUIRE_EQ(owners.receiver->snapshot().mode, hn::Mode::multicast);
}
#endif

#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_HYBRID_CONTROL
namespace {
struct HeldMulticastData {
    std::shared_ptr<robotweax::srt::compat::DatagramChannel> channel;
    std::mutex mutex;
    std::vector<std::vector<std::byte>> frames;
    static UdpIoResult send(std::span<const std::byte> bytes, IpEndpoint peer,
        void* context) noexcept
    {
        auto& self = *static_cast<HeldMulticastData*>(context);
        const auto packet = decode_packet(bytes);
        if (packet && packet.packet.kind == PacketKind::data) {
            std::lock_guard lock(self.mutex);
            self.frames.emplace_back(bytes.begin(), bytes.end());
            return {.bytes_transferred = bytes.size()};
        }
        return self.channel->socket.send_to(bytes, peer);
    }
    std::vector<std::vector<std::byte>> copy()
    {
        std::lock_guard lock(mutex);
        return frames;
    }
};
}
namespace {
struct NativeMergeCursor {
    std::atomic<std::uint64_t> next_required = 0;
    unsigned calls = 0;
    std::uint64_t stream = 0, member = 0, generation = 0;
    static std::optional<hn::AutomaticFallback::Cursor> resolve(
        void* p, const hn::AutomaticFallback::Request& request) noexcept
    {
        auto& self = *static_cast<NativeMergeCursor*>(p);
        ++self.calls;
        if (request.stream != self.stream || request.member != self.member
            || request.generation != self.generation)
            return {};
        // This test host has not consumed application DATA before fallback.
        // Only the host updates next_required after actual application reads.
        return hn::AutomaticFallback::Cursor {self.next_required.load(),
            hn::NativeService::monotonic_now(), request.generation};
    }
};
}
TEST(multicast_dispatch_native_automatic_fallback_lost_commits_plain_ctr_gcm)
{
    SKIP_UNLESS(loopback_multicast_available(), "no loopback multicast");
    for (unsigned mode = 0; mode < 3; ++mode) {
#ifndef ENABLE_AEAD_API_PREVIEW
        if (mode == 2)
            continue;
#endif
        CoordinatedNative f(1'000'000, mode != 0, mode == 2);
        const hn::DetectionConfiguration policy {.report_age_us = 800'000,
            .maximum_transit_us = 5'000,
            .stall_us = 40'000,
            .emission_lifetime_us = 400'000,
            .recovery_us = 80'000,
            .recovery_gap_us = 60'000,
            .hold_down_us = 100'000,
            .sample_gap_us = 20'000,
            .recovery_advances = 3};
        NativeReceiverOwners owners(f, {10'000, 600'000}, policy);
        const auto native = owners.pair.c->snapshot();
        auto cursor = std::make_shared<NativeMergeCursor>();
        cursor->stream = native.context.selection.stream;
        cursor->member = native.context.selection.member;
        cursor->generation = native.generation;
        auto automatic = hn::AutomaticFallback::create(owners.receiver,
            {1, 500'000}, {cursor, NativeMergeCursor::resolve});
        REQUIRE(automatic);
        const auto socket = SocketRegistry::instance().find(f.listener);
        std::shared_ptr<robotweax::srt::compat::DatagramChannel> control;
        {
            std::lock_guard lock(socket->mutex);
            control = socket->channel;
        }
        REQUIRE(control);
        HybridWireHook hook;
        hook.channel = control;
        hook.drop_commits = true;
        SendHookReset control_reset {control};
        control->set_send_hook_for_testing(HybridWireHook::send, &hook);
        HeldMulticastData held;
        held.channel = f.source();
        SendHookReset data_reset {held.channel};
        held.channel->set_send_hook_for_testing(HeldMulticastData::send, &held);
        REQUIRE(f.actor()->declare_source_activity(
            hn::Activity::producing, 0, 1'000'000));
        owners.wait([&] {
            return owners.receiver->snapshot().fresh
                && owners.receiver->peer_report(800'000).has_value();
        });
        int next = 0;
        auto send_at = std::chrono::steady_clock::now();
        const auto until = send_at + std::chrono::seconds {1};
        while (automatic->snapshot().result
                != hn::ReceiverBridge::RequestState::confirmed
            && std::chrono::steady_clock::now() < until) {
            REQUIRE(next < 64);
            if (std::chrono::steady_clock::now() >= send_at) {
                REQUIRE(
                    f.actor()->declare_source_activity(hn::Activity::producing,
                        static_cast<std::uint64_t>(next + 1), 1'000'000));
                REQUIRE_EQ(
                    f.send(next), static_cast<int>(message(next).size()));
                ++next;
                send_at = std::chrono::steady_clock::now()
                    + std::chrono::milliseconds {12};
            }
            automatic->pump();
            owners.sender->pump();
            std::this_thread::sleep_for(std::chrono::milliseconds {1});
        }
        const auto state = automatic->snapshot();
        REQUIRE_EQ(state.result, hn::ReceiverBridge::RequestState::confirmed);
        REQUIRE_EQ(state.admitted, 1U);
        REQUIRE_EQ(state.position, 0U);
        REQUIRE_EQ(cursor->calls, 1U);
        REQUIRE(hook.commits_dropped.load() > 0);
        REQUIRE_EQ(owners.sender->snapshot().applied, 1U);
        REQUIRE_EQ(f.actor()->snapshot().unicast_members, 1U);
        expect_messages(f.receiver, 0, next);
        cursor->next_required = static_cast<std::uint64_t>(next);
        const auto first = next;
        for (int i = 0; i < 40; ++i) {
            REQUIRE(f.actor()->declare_source_activity(hn::Activity::producing,
                static_cast<std::uint64_t>(next + 1), 1'000'000));
            REQUIRE_EQ(f.send(next), static_cast<int>(message(next).size()));
            ++next;
            automatic->pump();
            owners.sender->pump();
            std::this_thread::sleep_for(std::chrono::milliseconds {1});
        }
        expect_messages(f.receiver, first, 40);
        cursor->next_required = static_cast<std::uint64_t>(next);
        automatic->pump();
        REQUIRE_EQ(cursor->calls, 1U);
        REQUIRE_EQ(owners.sender->snapshot().applied, 1U);
        automatic->close();
        REQUIRE(!owners.receiver->snapshot().closed);
        REQUIRE_EQ(f.actor()->snapshot().unicast_members, 1U);
    }
}
TEST(
    multicast_dispatch_native_budgeted_overlap_return_lost_commits_plain_ctr_gcm)
{
    SKIP_UNLESS(loopback_multicast_available(), "no loopback multicast");
    for (unsigned profile = 0; profile < 3; ++profile) {
#ifndef ENABLE_AEAD_API_PREVIEW
        if (profile == 2)
            continue;
#endif
        CoordinatedNative f(1'000'000, profile != 0, profile == 2);
        const hn::ReturnPolicy returning {1'000'000, 32, 4, 10'000, 200'000};
        NativeReceiverOwners owners(f, {}, std::nullopt, returning);
        const auto socket = SocketRegistry::instance().find(f.listener);
        std::shared_ptr<robotweax::srt::compat::DatagramChannel> control;
        {
            std::lock_guard lock(socket->mutex);
            control = socket->channel;
        }
        REQUIRE(control);
        HybridWireHook hook;
        hook.channel = control;
        hook.drop_commits = true;
        SendHookReset reset {control};
        control->set_send_hook_for_testing(HybridWireHook::send, &hook);
        for (int i = 0; i < 4; ++i)
            REQUIRE_EQ(f.send(i), static_cast<int>(message(i).size()));
        expect_messages(f.receiver, 0, 4);
        REQUIRE_EQ(owners.receiver->request_fallback(4),
            hn::ReceiverBridge::Admission::accepted);
        owners.wait([&] {
            const auto s = owners.receiver->snapshot();
            return s.fresh && s.mode == hn::Mode::unicast;
        });
        REQUIRE_EQ(owners.receiver->request_overlap(),
            hn::ReceiverBridge::Admission::accepted);
        owners.wait([&] {
            const auto s = owners.receiver->snapshot();
            return s.fresh && s.mode == hn::Mode::overlap
                && s.returning == hn::ReceiverBridge::RequestState::confirmed;
        });
        const auto boundary = owners.receiver->snapshot().commit_position;
        REQUIRE_EQ(boundary, 4U);
        for (int i = 4; i < 12; ++i) {
            REQUIRE_EQ(f.send(i), static_cast<int>(message(i).size()));
            owners.receiver->pump();
            owners.sender->pump();
            std::this_thread::sleep_for(std::chrono::milliseconds {3});
        }
        std::optional<hn::CoverageProof> proof;
        owners.wait([&] {
            proof = owners.receiver->coverage_proof(12, 200'000);
            return proof.has_value();
        });
        REQUIRE(proof);
        REQUIRE_EQ(proof->boundary, boundary);
        REQUIRE_EQ(owners.receiver->request_multicast(*proof),
            hn::ReceiverBridge::Admission::accepted);
        owners.wait([&] {
            const auto s = owners.receiver->snapshot();
            return s.fresh && s.mode == hn::Mode::multicast && s.revision == 3
                && s.returning == hn::ReceiverBridge::RequestState::confirmed;
        });
        REQUIRE_EQ(owners.receiver->snapshot().commit_position, 12U);
        REQUIRE_EQ(owners.sender->snapshot().applied, 3U);
        REQUIRE_EQ(f.actor()->snapshot().unicast_members, 0U);
        REQUIRE(hook.commits_dropped.load() >= 3U);
        expect_messages(f.receiver, 4, 8);
        for (int i = 12; i < 52; ++i) {
            REQUIRE_EQ(f.send(i), static_cast<int>(message(i).size()));
            owners.receiver->pump();
            owners.sender->pump();
            std::this_thread::sleep_for(std::chrono::milliseconds {1});
        }
        expect_messages(f.receiver, 12, 40);
        std::optional<hn::SourceDataProgress> progress;
        owners.wait([&] {
            progress = owners.sender->data_progress();
            return progress.has_value();
        });
        REQUIRE(progress);
        REQUIRE(progress->unicast.next <= 12U);
        REQUIRE_EQ(owners.sender->snapshot().applied, 3U);
    }
}
TEST(multicast_dispatch_native_automatic_return_lost_commits_plain_ctr_gcm)
{
    SKIP_UNLESS(loopback_multicast_available(), "no loopback multicast");
    for (unsigned profile = 0; profile < 3; ++profile) {
#ifndef ENABLE_AEAD_API_PREVIEW
        if (profile == 2)
            continue;
#endif
        CoordinatedNative f(1'000'000, profile != 0, profile == 2);
        const hn::DetectionConfiguration detection {.report_age_us = 800'000,
            .maximum_transit_us = 5'000,
            .stall_us = 40'000,
            .emission_lifetime_us = 400'000,
            .recovery_us = 80'000,
            .recovery_gap_us = 60'000,
            .hold_down_us = 100'000,
            .sample_gap_us = 20'000,
            .recovery_advances = 3};
        NativeReceiverOwners owners(f, {10'000, 600'000}, detection,
            hn::ReturnPolicy {1'000'000, 32, 4, 10'000, 200'000});
        auto automatic = hn::AutomaticReturn::create(
            owners.receiver, {1, 220'000, 8, 10'000, 160'000});
        REQUIRE(automatic);
        const auto socket = SocketRegistry::instance().find(f.listener);
        std::shared_ptr<robotweax::srt::compat::DatagramChannel> control;
        {
            std::lock_guard lock(socket->mutex);
            control = socket->channel;
        }
        REQUIRE(control);
        HybridWireHook hook;
        hook.channel = control;
        hook.drop_commits = true;
        SendHookReset reset {control};
        control->set_send_hook_for_testing(HybridWireHook::send, &hook);
        REQUIRE(f.actor()->declare_source_activity(
            hn::Activity::producing, 4, 1'000'000));
        for (int i = 0; i < 4; ++i)
            REQUIRE_EQ(f.send(i), static_cast<int>(message(i).size()));
        expect_messages(f.receiver, 0, 4);
        owners.wait([&] {
            return owners.receiver->snapshot().fresh;
        });
        REQUIRE_EQ(owners.receiver->request_fallback(4),
            hn::ReceiverBridge::Admission::accepted);
        owners.wait([&] {
            const auto s = owners.receiver->snapshot();
            return s.fresh && s.mode == hn::Mode::unicast;
        });
        int next = 4;
        auto send_at = std::chrono::steady_clock::now();
        const auto until = send_at + std::chrono::seconds {2};
        while (automatic->snapshot().phase
                != hn::AutomaticReturn::Phase::collecting
            && std::chrono::steady_clock::now() < until) {
            if (std::chrono::steady_clock::now() >= send_at) {
                REQUIRE(next < 64);
                REQUIRE(
                    f.actor()->declare_source_activity(hn::Activity::producing,
                        static_cast<std::uint64_t>(next + 1), 1'000'000));
                REQUIRE_EQ(
                    f.send(next), static_cast<int>(message(next).size()));
                ++next;
                send_at = std::chrono::steady_clock::now()
                    + std::chrono::milliseconds {12};
            }
            automatic->pump();
            owners.sender->pump();
            std::this_thread::sleep_for(std::chrono::milliseconds {1});
        }
        const auto overlap = automatic->snapshot();
        REQUIRE_EQ(overlap.phase, hn::AutomaticReturn::Phase::collecting);
        REQUIRE(overlap.boundary >= 4);
        const auto through = overlap.boundary + 8;
        REQUIRE(static_cast<std::uint64_t>(next) <= through);
        while (static_cast<std::uint64_t>(next) < through) {
            REQUIRE(f.actor()->declare_source_activity(hn::Activity::producing,
                static_cast<std::uint64_t>(next + 1), 1'000'000));
            REQUIRE_EQ(f.send(next), static_cast<int>(message(next).size()));
            ++next;
            automatic->pump();
            owners.sender->pump();
            std::this_thread::sleep_for(std::chrono::milliseconds {3});
        }
        owners.wait([&] {
            automatic->pump();
            return automatic->snapshot().result
                == hn::ReceiverBridge::RequestState::confirmed;
        });
        const auto returned = automatic->snapshot();
        REQUIRE_EQ(returned.operations, 1U);
        REQUIRE_EQ(returned.overlap_admitted, 1U);
        REQUIRE_EQ(returned.multicast_admitted, 1U);
        REQUIRE_EQ(returned.through, through);
        REQUIRE_EQ(returned.commit_position, through);
        REQUIRE_EQ(owners.receiver->snapshot().commit_position, through);
        REQUIRE_EQ(f.actor()->snapshot().unicast_members, 0U);
        REQUIRE_EQ(owners.sender->snapshot().applied, 3U);
        REQUIRE(hook.commits_dropped.load() >= 3);
        expect_messages(f.receiver, 4, next - 4);
        const auto first = next;
        automatic->close();
        REQUIRE(!owners.receiver->snapshot().closed);
        for (unsigned i = 0; i < 40; ++i) {
            REQUIRE_EQ(f.send(next), static_cast<int>(message(next).size()));
            ++next;
            owners.receiver->pump();
            owners.sender->pump();
            std::this_thread::sleep_for(std::chrono::milliseconds {1});
        }
        expect_messages(f.receiver, first, 40);
        std::optional<hn::SourceDataProgress> progress;
        owners.wait([&] {
            progress = owners.sender->data_progress();
            return progress.has_value();
        });
        REQUIRE(progress->unicast.next <= through);
        REQUIRE_EQ(owners.sender->snapshot().applied, 3U);
    }
}
namespace {
struct NativeJoinPreparation {
    unsigned calls = 0;
    static std::optional<UdpSocket> prepare(
        void* p, const hn::MembershipRecovery::Request& r) noexcept
    {
        auto& self = *static_cast<NativeJoinPreparation*>(p);
        ++self.calls;
        if (self.calls <= 2)
            return {};
        return hn::MembershipRecovery::prepare(r);
    }
};
}
TEST(
    multicast_dispatch_native_startup_join_retry_fallback_and_return_plain_ctr_gcm)
{
    SKIP_UNLESS(loopback_multicast_available(), "no loopback multicast");
    for (unsigned profile = 0; profile < 3; ++profile) {
#ifndef ENABLE_AEAD_API_PREVIEW
        if (profile == 2)
            continue;
#endif
        CoordinatedNative f(1'000'000, profile != 0, profile == 2);
        const hn::DetectionConfiguration detection {.report_age_us = 800'000,
            .maximum_transit_us = 5'000,
            .stall_us = 40'000,
            .emission_lifetime_us = 400'000,
            .recovery_us = 80'000,
            .recovery_gap_us = 60'000,
            .hold_down_us = 100'000,
            .sample_gap_us = 20'000,
            .recovery_advances = 3};
        NativeReceiverOwners owners(f, {10'000, 600'000}, detection,
            hn::ReturnPolicy {1'000'000, 32, 4, 10'000, 200'000}, true);
        // The normal connect path remains strict. Stage local membership absence
        // before any DATA to exercise the separately negotiated internal path.
        const auto group = GroupRegistry::instance().find(f.receiver);
        std::shared_ptr<robotweax::srt::compat::DatagramChannel> original;
        {
            std::lock_guard lock(group->mutex);
            original = group->dispatch->receive_channel;
        }
        REQUIRE(original);
        original->unregister_connection(
            owners.pair.c->origin()->dispatch().group_id);
        REQUIRE(!owners.receiver->data_progress());
        auto joiner = std::make_shared<NativeJoinPreparation>();
        auto membership = hn::MembershipRecovery::create(owners.pair.c,
            {IpEndpoint::loopback(), 4, 1'000'000, 80'000, 160'000},
            {joiner, NativeJoinPreparation::prepare});
        REQUIRE(membership);
        auto cursor = std::make_shared<NativeMergeCursor>();
        const auto native = owners.pair.c->snapshot();
        cursor->stream = native.context.selection.stream;
        cursor->member = native.context.selection.member;
        cursor->generation = native.generation;
        auto fallback = hn::AutomaticFallback::create(owners.receiver,
            {1, 500'000}, {cursor, NativeMergeCursor::resolve});
        auto returning = hn::AutomaticReturn::create(
            owners.receiver, {1, 220'000, 8, 10'000, 160'000});
        REQUIRE(fallback);
        REQUIRE(returning);
        const auto socket = SocketRegistry::instance().find(f.listener);
        std::shared_ptr<robotweax::srt::compat::DatagramChannel> control;
        {
            std::lock_guard lock(socket->mutex);
            control = socket->channel;
        }
        HybridWireHook hook;
        hook.channel = control;
        hook.drop_commits = true;
        SendHookReset reset {control};
        control->set_send_hook_for_testing(HybridWireHook::send, &hook);
        REQUIRE(f.actor()->declare_source_activity(
            hn::Activity::producing, 0, 1'000'000));
        owners.wait([&] {
            return owners.receiver->snapshot().fresh
                && owners.receiver->peer_report(800'000).has_value();
        });
        int next = 0;
        auto send_at = std::chrono::steady_clock::now();
        const auto until = send_at + std::chrono::seconds {2};
        bool fallback_confirmed = false, joined = false;
        while (returning->snapshot().phase
                != hn::AutomaticReturn::Phase::collecting
            && std::chrono::steady_clock::now() < until) {
            if (std::chrono::steady_clock::now() >= send_at) {
                REQUIRE(next < 96);
                REQUIRE(
                    f.actor()->declare_source_activity(hn::Activity::producing,
                        static_cast<std::uint64_t>(next + 1), 1'000'000));
                REQUIRE_EQ(
                    f.send(next), static_cast<int>(message(next).size()));
                ++next;
                send_at = std::chrono::steady_clock::now()
                    + std::chrono::milliseconds {12};
            }
            fallback->pump();
            if (!fallback_confirmed
                && fallback->snapshot().result
                    == hn::ReceiverBridge::RequestState::confirmed) {
                fallback_confirmed = true;
                REQUIRE(!membership->snapshot().joined);
                REQUIRE_EQ(owners.receiver->snapshot().mode, hn::Mode::unicast);
                REQUIRE(!owners.receiver->data_progress()->multicast.observed);
            }
            if (membership->snapshot().attempts < 2 || fallback_confirmed)
                membership->pump();
            joined = joined || membership->snapshot().joined;
            returning->pump();
            owners.sender->pump();
            std::this_thread::sleep_for(std::chrono::milliseconds {1});
        }
        REQUIRE(fallback_confirmed);
        REQUIRE(joined);
        REQUIRE_EQ(joiner->calls, 3U);
        REQUIRE_EQ(membership->snapshot().failures, 2U);
        REQUIRE_EQ(cursor->calls, 1U);
        const auto overlap = returning->snapshot();
        REQUIRE_EQ(overlap.phase, hn::AutomaticReturn::Phase::collecting);
        const auto through = overlap.boundary + 8;
        REQUIRE(static_cast<std::uint64_t>(next) <= through);
        while (static_cast<std::uint64_t>(next) < through) {
            REQUIRE(f.actor()->declare_source_activity(hn::Activity::producing,
                static_cast<std::uint64_t>(next + 1), 1'000'000));
            REQUIRE_EQ(f.send(next), static_cast<int>(message(next).size()));
            ++next;
            returning->pump();
            owners.sender->pump();
            std::this_thread::sleep_for(std::chrono::milliseconds {3});
        }
        owners.wait([&] {
            returning->pump();
            return returning->snapshot().result
                == hn::ReceiverBridge::RequestState::confirmed;
        });
        REQUIRE_EQ(owners.sender->snapshot().applied, 3U);
        REQUIRE(hook.commits_dropped.load() >= 3);
        expect_messages(f.receiver, 0, next);
        cursor->next_required = next;
        const auto first = next;
        for (unsigned i = 0; i < 40; ++i) {
            REQUIRE_EQ(f.send(next), static_cast<int>(message(next).size()));
            ++next;
            owners.receiver->pump();
            owners.sender->pump();
            std::this_thread::sleep_for(std::chrono::milliseconds {1});
        }
        expect_messages(f.receiver, first, 40);
        cursor->next_required = next;
        REQUIRE_EQ(
            owners.receiver->request_fallback(static_cast<std::uint64_t>(next)),
            hn::ReceiverBridge::Admission::accepted);
        owners.wait([&] {
            const auto s = owners.receiver->snapshot();
            return s.fresh && s.mode == hn::Mode::unicast;
        });
        membership->close();
        REQUIRE(!owners.receiver->data_progress());
        REQUIRE_EQ(srt_getsockstate(f.receiver), SRTS_CONNECTED);
        const auto uc_first = next;
        for (unsigned i = 0; i < 40; ++i) {
            REQUIRE_EQ(f.send(next), static_cast<int>(message(next).size()));
            ++next;
            std::this_thread::sleep_for(std::chrono::milliseconds {1});
        }
        expect_messages(f.receiver, uc_first, 40);
        REQUIRE_EQ(owners.sender->snapshot().applied, 4U);
    }
}
TEST(
    multicast_dispatch_native_passive_detector_outage_explicit_fallback_and_recovery)
{
    SKIP_UNLESS(loopback_multicast_available(), "no loopback multicast");
    CoordinatedNative f;
    const hn::DetectionConfiguration policy {.report_age_us = 800'000,
        .maximum_transit_us = 5'000,
        .stall_us = 40'000,
        .emission_lifetime_us = 400'000,
        .recovery_us = 80'000,
        .recovery_gap_us = 60'000,
        .hold_down_us = 100'000,
        .sample_gap_us = 20'000,
        .recovery_advances = 3};
    NativeReceiverOwners owners(f, {10'000, 600'000}, policy);
    REQUIRE(f.actor()->declare_source_activity(
        hn::Activity::producing, 0, 1'000'000));
    owners.wait([&] {
        return owners.receiver->snapshot().fresh
            && owners.receiver->peer_report(800'000).has_value();
    });
    HeldMulticastData held;
    held.channel = f.source();
    SendHookReset reset {held.channel};
    held.channel->set_send_hook_for_testing(HeldMulticastData::send, &held);
    int next = 0;
    const auto run = [&](auto predicate) {
        const auto until =
            std::chrono::steady_clock::now() + std::chrono::seconds {1};
        auto send_at = std::chrono::steady_clock::now();
        while (!predicate() && std::chrono::steady_clock::now() < until) {
            REQUIRE(next < 128);
            if (std::chrono::steady_clock::now() >= send_at) {
                REQUIRE(
                    f.actor()->declare_source_activity(hn::Activity::producing,
                        static_cast<std::uint64_t>(next + 1), 1'000'000));
                REQUIRE_EQ(
                    f.send(next), static_cast<int>(message(next).size()));
                ++next;
                send_at = std::chrono::steady_clock::now()
                    + std::chrono::milliseconds {12};
            }
            owners.receiver->pump();
            owners.sender->pump();
            std::this_thread::sleep_for(std::chrono::milliseconds {1});
        }
        if (!predicate()) {
            const auto a = owners.receiver->path_assessment();
            throw std::runtime_error("passive detector: state="
                + std::to_string(a ? static_cast<unsigned>(a->state) : 99)
                + " reason="
                + std::to_string(a ? static_cast<unsigned>(a->reason) : 99));
        }
    };
    run([&] {
        const auto a = owners.receiver->path_assessment();
        return a && a->fallback_candidate;
    });
    auto a = owners.receiver->path_assessment();
    REQUIRE(a->multicast_stalled);
    REQUIRE(a->all_data_stalled);
    REQUIRE_EQ(owners.sender->snapshot().applied, 0U);
    REQUIRE_EQ(owners.receiver->snapshot().fallback,
        hn::ReceiverBridge::RequestState::none);
    REQUIRE_EQ(owners.receiver->data_progress()->unique_packets, 0U);
    REQUIRE_EQ(owners.receiver->request_fallback(0),
        hn::ReceiverBridge::Admission::accepted);
    owners.wait([&] {
        const auto s = owners.receiver->snapshot();
        return s.fresh && s.mode == hn::Mode::unicast;
    });
    held.channel->set_send_hook_for_testing(nullptr, nullptr);
    run([&] {
        const auto ready = owners.receiver->path_assessment();
        return ready && ready->return_candidate;
    });
    REQUIRE_EQ(owners.sender->snapshot().applied, 1U);
    REQUIRE_EQ(f.actor()->snapshot().unicast_members, 1U);
    REQUIRE(f.actor()->declare_source_activity(
        hn::Activity::paused, static_cast<std::uint64_t>(next), 1'000'000));
    owners.wait([&] {
        const auto p = owners.receiver->path_assessment();
        return p && p->state == hn::PathDetector::State::paused;
    });
    REQUIRE(!owners.receiver->path_assessment()->return_candidate);
    owners.receiver->close();
    REQUIRE(!owners.receiver->path_assessment());
}
TEST(multicast_dispatch_native_authenticated_reporting_plain_ctr_gcm)
{
    SKIP_UNLESS(loopback_multicast_available(), "no loopback multicast");
    for (unsigned mode = 0; mode < 3; ++mode) {
#ifndef ENABLE_AEAD_API_PREVIEW
        if (mode == 2)
            continue;
#endif
        CoordinatedNative f(1'000'000, mode != 0, mode == 2);
        NativeReceiverOwners owners(f, {10'000, 100'000});
        owners.sender->pump();
        REQUIRE_EQ(owners.sender->snapshot().reports_queued, 0U);
        REQUIRE(f.actor()->declare_source_activity(
            hn::Activity::producing, 3, 1'000'000));
        for (int i = 0; i < 3; ++i)
            REQUIRE_EQ(f.send(i), static_cast<int>(message(i).size()));
        expect_messages(f.receiver, 0, 3);
        REQUIRE(f.actor()->declare_source_activity(
            hn::Activity::producing, 3, 1'000'000));
        owners.wait([&] {
            const auto stream = owners.receiver->peer_report(100'000);
            const auto path = owners.sender->peer_report(100'000);
            return stream && path && stream->fresh && path->fresh
                && stream->message.next_position == 3
                && stream->message.multicast_next == 3
                && path->message.next_position == 3
                && path->message.multicast_next == 3;
        });
        const auto previous = owners.receiver->peer_report(100'000);
        REQUIRE(f.actor()->declare_source_activity(
            hn::Activity::paused, 3, 1'000'000));
        owners.wait([&] {
            const auto r = owners.receiver->peer_report(100'000);
            return r && r->message.activity == hn::Activity::paused;
        });
        REQUIRE_EQ(owners.receiver->peer_report(100'000)->next_advanced_at_us,
            previous->next_advanced_at_us);
        REQUIRE_EQ(owners.sender->snapshot().applied, 0U);
        REQUIRE_EQ(owners.receiver->snapshot().fallback,
            hn::ReceiverBridge::RequestState::none);
        owners.sender->close();
        owners.receiver->close();
        REQUIRE(!owners.sender->peer_report(100'000));
        REQUIRE(!owners.receiver->peer_report(100'000));
    }
}
TEST(
    multicast_dispatch_native_data_observations_distinguish_late_paths_plain_ctr_gcm)
{
    SKIP_UNLESS(loopback_multicast_available(), "no loopback multicast");
    for (unsigned mode = 0; mode < 3; ++mode) {
#ifndef ENABLE_AEAD_API_PREVIEW
        if (mode == 2)
            continue;
#endif
        CoordinatedNative f(1'000'000, mode != 0, mode == 2);
        NativeReceiverOwners owners(f);
        for (int i = 0; i < 3; ++i)
            REQUIRE_EQ(f.send(i), static_cast<int>(message(i).size()));
        expect_messages(f.receiver, 0, 3);
        owners.wait([&] {
            const auto p = owners.receiver->data_progress();
            return p && p->all.next == 3 && p->multicast.next == 3;
        });
        auto received = owners.receiver->data_progress();
        REQUIRE(received);
        REQUIRE(!received->unicast.observed);
        REQUIRE_EQ(received->unique_packets, 3U);
        REQUIRE_EQ(received->window_floor, 3U);
        REQUIRE_EQ(owners.receiver->request_fallback(received->window_floor),
            hn::ReceiverBridge::Admission::accepted);
        owners.wait([&] {
            const auto s = owners.receiver->snapshot();
            return s.fresh && s.mode == hn::Mode::unicast;
        });
        HeldMulticastData held;
        held.channel = f.source();
        SendHookReset reset {held.channel};
        held.channel->set_send_hook_for_testing(HeldMulticastData::send, &held);
        for (int i = 3; i < 6; ++i)
            REQUIRE_EQ(f.send(i), static_cast<int>(message(i).size()));
        expect_messages(f.receiver, 3, 3);
        owners.wait([&] {
            const auto r = owners.receiver->data_progress();
            const auto t = owners.sender->data_progress();
            return r && t && r->unicast.next == 6 && t->unicast.next == 6
                && held.copy().size() == 3;
        });
        received = owners.receiver->data_progress();
        REQUIRE_EQ(received->all.next, 6U);
        REQUIRE_EQ(received->multicast.next, 3U);
        const auto frames = held.copy();
        const auto context = owners.pair.c->snapshot().context;
        auto changed = frames.front();
        changed.back() ^= std::byte {1};
        REQUIRE(held.channel->socket.send_to(
            changed, context.dispatch.multicast_group));
        const auto decoded = decode_packet(frames.front());
        REQUIRE(decoded);
        owners.pair.caller->process_packet(decoded.packet,
            context.source.endpoint, std::nullopt,
            robotweax::srt::compat::PacketIngress::multicast_data);
        owners.pair.caller->process_packet(
            decoded.packet, IpEndpoint::loopback(1));
        std::this_thread::sleep_for(std::chrono::milliseconds {10});
        REQUIRE_EQ(owners.receiver->data_progress()->multicast.next, 3U);
        for (const auto& frame : frames)
            REQUIRE(held.channel->socket.send_to(
                frame, context.dispatch.multicast_group));
        owners.wait([&] {
            const auto p = owners.receiver->data_progress();
            return p && p->multicast.next == 6;
        });
        const auto after = owners.receiver->data_progress();
        REQUIRE_EQ(after->unique_packets, 6U);
        REQUIRE_EQ(after->all.next, 6U);
        REQUIRE(after->matched_copies >= 3U);
        for (const auto& frame : frames)
            REQUIRE(held.channel->socket.send_to(
                frame, context.dispatch.multicast_group));
        std::this_thread::sleep_for(std::chrono::milliseconds {10});
        REQUIRE_EQ(owners.receiver->data_progress()->multicast.last_advance_us,
            after->multicast.last_advance_us);
        REQUIRE_EQ(
            owners.pair.caller->buffer_packet_counts().available_receive, 0U);
        owners.receiver->close();
        REQUIRE(!owners.receiver->data_progress());
    }
}
TEST(
    multicast_dispatch_native_data_observations_are_fenced_by_route_and_control_lifetimes)
{
    SKIP_UNLESS(loopback_multicast_available(), "no loopback multicast");
    CoordinatedNative f;
    NativeReceiverOwners owners(f);
    REQUIRE_EQ(f.send(0), static_cast<int>(message(0).size()));
    expect_messages(f.receiver, 0, 1);
    owners.wait([&] {
        const auto p = owners.receiver->data_progress();
        return p && p->multicast.next == 1;
    });
    const auto group = GroupRegistry::instance().find(f.receiver);
    std::shared_ptr<robotweax::srt::compat::DatagramChannel> channel;
    {
        std::lock_guard lock(group->mutex);
        channel = group->dispatch->receive_channel;
    }
    REQUIRE(channel);
    const auto context = owners.pair.c->snapshot().context;
    channel->unregister_connection(context.dispatch.group_id);
    REQUIRE(!owners.receiver->data_progress());
    REQUIRE(channel->register_ingress_route(context.dispatch.group_id,
        owners.pair.caller,
        robotweax::srt::compat::PacketIngress::multicast_data,
        robotweax::srt::compat::MulticastIngressConfiguration {
            .source = context.source}));
    REQUIRE_EQ(f.send(1), static_cast<int>(message(1).size()));
    expect_messages(f.receiver, 1, 1);
    REQUIRE(!owners.receiver->data_progress());
    REQUIRE(owners.sender->data_progress());
    owners.lease->revoke();
    REQUIRE(!owners.sender->data_progress());
}
#endif

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
    // Consume the immediate first paced submission before filling capacity.
    // Otherwise its late completion can legitimately restore EPOLL_OUT between
    // the full-queue snapshot and the zero-time readiness assertion below.
    REQUIRE_EQ(f.send(0), static_cast<int>(message(0).size()));
    const auto primed =
        std::chrono::steady_clock::now() + std::chrono::seconds {2};
    while (f.actor()->snapshot().inflight_frames
        && std::chrono::steady_clock::now() < primed)
        std::this_thread::sleep_for(std::chrono::milliseconds {1});
    REQUIRE_EQ(f.actor()->snapshot().applied_completions, 1U);
    int accepted = 1;
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

#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_HYBRID_CONTROL
TEST(
    multicast_dispatch_native_source_error_reports_and_preserves_preapproved_uc_plain_ctr_gcm)
{
    SKIP_UNLESS(loopback_multicast_available(), "no loopback multicast");
    for (unsigned mode = 0; mode < 3; ++mode) {
#ifndef ENABLE_AEAD_API_PREVIEW
        if (mode == 2)
            continue;
#endif
        CoordinatedNative f(1'000'000, mode != 0, mode == 2, 64);
        const hn::DetectionConfiguration detection {.report_age_us = 800'000,
            .maximum_transit_us = 5'000,
            .stall_us = 40'000,
            .emission_lifetime_us = 400'000,
            .recovery_us = 80'000,
            .recovery_gap_us = 60'000,
            .hold_down_us = 100'000,
            .sample_gap_us = 20'000,
            .recovery_advances = 3};
        NativeReceiverOwners owners(f, {10'000, 600'000}, detection);
        // Lost commits must reconcile before this member becomes a survivor.
        const auto socket = SocketRegistry::instance().find(f.listener);
        std::shared_ptr<robotweax::srt::compat::DatagramChannel> control;
        {
            std::lock_guard lock(socket->mutex);
            control = socket->channel;
        }
        HybridWireHook hook;
        hook.channel = control;
        hook.drop_commits = true;
        SendHookReset control_reset {control};
        control->set_send_hook_for_testing(HybridWireHook::send, &hook);
        REQUIRE_EQ(owners.receiver->request_fallback(0),
            hn::ReceiverBridge::Admission::accepted);
        owners.wait([&] {
            auto s = owners.receiver->snapshot();
            return s.fresh && s.mode == hn::Mode::unicast;
        });
        REQUIRE_EQ(owners.receiver->snapshot().revision, 1U);
        FailedSourceProbe probe;
        probe.fatal = true;
        SendHookReset source_reset {f.source()};
        f.source()->set_send_hook_for_testing(FailedSourceProbe::send, &probe);
        REQUIRE_EQ(f.send(0), static_cast<int>(message(0).size()));
        wait_for_source_failure(f.actor());
        expect_messages(f.receiver, 0, 1);
        owners.wait([&] {
            auto r = owners.receiver->peer_report(800'000);
            auto a = owners.receiver->path_assessment();
            return r && r->fresh
                && r->message.activity == hn::Activity::source_failed && a
                && a->state == hn::PathDetector::State::source_failed;
        });
        REQUIRE(!owners.sender->snapshot().closed);
        REQUIRE_EQ(srt_getsockstate(f.sender), SRTS_BROKEN);
        const auto identity = owners.pair.l->snapshot().context.selection;
        const std::array selected {
            PolicyActor::RecoveryMember {identity.member, 1}};
        REQUIRE_EQ(recover_native_source(f.actor(), selected).status,
            PolicyActor::TransitionStatus::applied);
        owners.wait([&] {
            auto s = owners.receiver->snapshot();
            return s.fresh && s.revision == 2 && s.mode == hn::Mode::unicast;
        });
        REQUIRE_EQ(owners.sender->snapshot().source_recoveries, 1U);
        REQUIRE(owners.sender->snapshot().unicast_only);
        const auto attempts = probe.attempts.load();
        for (int i = 1; i < 81; ++i) {
            REQUIRE_EQ(f.send(i), static_cast<int>(message(i).size()));
            owners.receiver->pump();
            owners.sender->pump();
            std::this_thread::sleep_for(std::chrono::milliseconds {2});
        }
        expect_messages(f.receiver, 1, 80);
        owners.wait([&] {
            auto r = owners.receiver->peer_report(800'000);
            return r && r->fresh && r->message.revision == 2
                && r->message.next_position == 81
                && r->message.mode == hn::Mode::unicast
                && r->message.activity == hn::Activity::source_failed;
        });
        REQUIRE_EQ(probe.attempts.load(), attempts);
        REQUIRE_EQ(owners.sender->snapshot().applied, 1U);
        REQUIRE_EQ(srt_getsockstate(f.sender), SRTS_CONNECTED);
        owners.lease->revoke();
        owners.receiver->pump();
        owners.sender->pump();
        REQUIRE(owners.sender->snapshot().closed);
        REQUIRE_EQ(f.send(81), static_cast<int>(message(81).size()));
        expect_messages(f.receiver, 81, 1);
    }
}
TEST(multicast_dispatch_native_source_error_is_diagnostic_without_uc_admission)
{
    SKIP_UNLESS(loopback_multicast_available(), "no loopback multicast");
    CoordinatedNative f;
    NativeReceiverOwners owners(f, {10'000, 100'000});
    FailedSourceProbe probe;
    probe.fatal = true;
    SendHookReset reset {f.source()};
    f.source()->set_send_hook_for_testing(FailedSourceProbe::send, &probe);
    REQUIRE_EQ(f.send(0), static_cast<int>(message(0).size()));
    wait_for_source_failure(f.actor());
    owners.wait([&] {
        auto r = owners.receiver->peer_report(100'000);
        return r && r->fresh
            && r->message.activity == hn::Activity::source_failed;
    });
    REQUIRE_EQ(owners.receiver->snapshot().mode, hn::Mode::multicast);
    REQUIRE_EQ(owners.receiver->request_fallback(0),
        hn::ReceiverBridge::Admission::accepted);
    owners.wait([&] {
        return owners.receiver->snapshot().fallback
            == hn::ReceiverBridge::RequestState::denied;
    });
    REQUIRE(!owners.sender->snapshot().closed);
    REQUIRE_EQ(f.actor()->snapshot().unicast_reservations, 0U);
    REQUIRE_EQ(owners.sender->snapshot().applied, 0U);
    REQUIRE_EQ(srt_getsockstate(f.receiver), SRTS_CONNECTED);
}
#endif

#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_HYBRID_CONTROL
TEST(
    multicast_dispatch_native_source_error_provisioned_startup_keeps_owned_control)
{
    SKIP_UNLESS(loopback_multicast_available(), "no loopback multicast");
    StartupHooks hooks;
    hooks.reporting = {10'000, 100'000};
    CoordinatedNative f(1'000'000, false, false, 0, 120, true, false,
        StartupHooks::configure, &hooks);
    auto owner = startup_owner(f.receiver);
    REQUIRE(owner);
    auto receiver = owner->receiver();
    REQUIRE(receiver);
    FailedSourceProbe probe;
    probe.fatal = true;
    SendHookReset reset {f.source()};
    f.source()->set_send_hook_for_testing(FailedSourceProbe::send, &probe);
    REQUIRE_EQ(f.send(0), static_cast<int>(message(0).size()));
    wait_for_source_failure(f.actor());
    expect_messages(f.receiver, 0, 1);
    auto until = std::chrono::steady_clock::now() + std::chrono::seconds {1};
    bool reported = false;
    while (std::chrono::steady_clock::now() < until) {
        auto r = receiver->peer_report(100'000);
        if (r && r->fresh
            && r->message.activity == hn::Activity::source_failed) {
            reported = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds {1});
    }
    REQUIRE(reported);
    std::array<PolicyActor::Member, 1> members;
    REQUIRE_EQ(f.actor()->members(members), 1U);
    const std::array selected {
        PolicyActor::RecoveryMember {members[0].id, members[0].revision}};
    REQUIRE_EQ(recover_native_source(f.actor(), selected).status,
        PolicyActor::TransitionStatus::applied);
    until = std::chrono::steady_clock::now() + std::chrono::seconds {1};
    bool confirmed = false;
    while (std::chrono::steady_clock::now() < until) {
        auto s = receiver->snapshot();
        if (s.fresh && s.revision == 2) {
            confirmed = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds {1});
    }
    REQUIRE(confirmed);
    REQUIRE_EQ(owner->snapshot().phase, hn::StartupSession::Phase::connected);
    REQUIRE_EQ(f.send(1), static_cast<int>(message(1).size()));
    expect_messages(f.receiver, 1, 1);
}
#endif

#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_HYBRID_CONTROL
TEST(
    multicast_dispatch_native_rejoin_loss_fallback_and_fresh_return_plain_ctr_gcm)
{
    SKIP_UNLESS(loopback_multicast_available(), "no loopback multicast");
    for (unsigned mode = 0; mode < 3; ++mode) {
#ifndef ENABLE_AEAD_API_PREVIEW
        if (mode == 2)
            continue;
#endif
        StartupHooks hooks;
        hooks.maximum_recoveries = 1;
        hooks.reporting = {10'000, 600'000};
        hooks.caller->join_success = true;
        hooks.returning = hn::ReturnPolicy {1'000'000, 32, 4, 10'000, 200'000};
        CoordinatedNative f(1'000'000, mode != 0, mode == 2, 0, 120, true,
            false, StartupHooks::configure, &hooks);
        auto owner = startup_owner(f.receiver);
        REQUIRE(owner);
        auto receiver = owner->receiver();
        auto membership = owner->membership();
        const auto wait = [&](auto predicate) {
            const auto until =
                std::chrono::steady_clock::now() + std::chrono::seconds {2};
            while (!predicate() && std::chrono::steady_clock::now() < until)
                std::this_thread::sleep_for(std::chrono::milliseconds {1});
            REQUIRE(predicate());
        };
        wait([&] {
            return membership->snapshot().joined;
        });
        int next = 0;
        const auto return_mc = [&] {
            REQUIRE_EQ(receiver->request_overlap(),
                hn::ReceiverBridge::Admission::accepted);
            wait([&] {
                auto s = receiver->snapshot();
                return s.fresh && s.mode == hn::Mode::overlap;
            });
            const auto boundary = receiver->snapshot().commit_position;
            const int first = next;
            REQUIRE_EQ(boundary, static_cast<std::uint64_t>(next));
            for (unsigned i = 0; i < 8; ++i) {
                REQUIRE_EQ(
                    f.send(next), static_cast<int>(message(next).size()));
                ++next;
                std::this_thread::sleep_for(std::chrono::milliseconds {3});
            }
            expect_messages(f.receiver, first, 8);
            std::optional<hn::CoverageProof> proof;
            wait([&] {
                proof = receiver->coverage_proof(boundary + 8, 160'000);
                return proof.has_value();
            });
            REQUIRE(proof->route_epoch);
            REQUIRE_EQ(receiver->request_multicast(*proof),
                hn::ReceiverBridge::Admission::accepted);
            wait([&] {
                auto s = receiver->snapshot();
                return s.fresh && s.mode == hn::Mode::multicast;
            });
        };
        return_mc();
        const auto old_epoch = membership->snapshot().route_epoch;
        const auto attempts = membership->snapshot().attempts;
        REQUIRE(membership->report_loss());
        REQUIRE(!membership->snapshot().joined);
        std::this_thread::sleep_for(std::chrono::milliseconds {10});
        REQUIRE_EQ(
            membership->snapshot().attempts, attempts); // No UC approval yet.
        hooks.caller->recovery_failures = 2;
        REQUIRE_EQ(receiver->request_fallback(next),
            hn::ReceiverBridge::Admission::accepted);
        wait([&] {
            auto s = receiver->snapshot();
            return s.fresh && s.mode == hn::Mode::unicast;
        });
        const int first = next;
        for (unsigned i = 0; i < 40; ++i) {
            REQUIRE_EQ(f.send(next), static_cast<int>(message(next).size()));
            ++next;
            std::this_thread::sleep_for(std::chrono::milliseconds {2});
        }
        expect_messages(f.receiver, first, 40);
        wait([&] {
            return membership->snapshot().joined;
        });
        REQUIRE_EQ(membership->snapshot().recoveries, 1U);
        REQUIRE_EQ(membership->snapshot().failures, 2U);
        REQUIRE(membership->snapshot().route_epoch > old_epoch);
        return_mc();
        const int restored = next;
        for (unsigned i = 0; i < 40; ++i) {
            REQUIRE_EQ(f.send(next), static_cast<int>(message(next).size()));
            ++next;
            std::this_thread::sleep_for(std::chrono::milliseconds {2});
        }
        expect_messages(f.receiver, restored, 40);
        REQUIRE_EQ(f.actor()->snapshot().unicast_members, 0U);
        REQUIRE(hooks.wire.commits_dropped.load() >= 6);
        REQUIRE_EQ(membership->snapshot().losses, 1U);
    }
}
#endif

#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_HYBRID_CONTROL
namespace {
struct HybridFaultLink {
    using Queue = robotweax::srt::test::HybridFaultQueue;
    std::shared_ptr<robotweax::srt::compat::DatagramChannel> channel;
    mutable std::mutex mutex;
    Queue auxiliary, data;
    Queue::Rules auxiliary_rules, data_rules;
    std::atomic<bool> drop_commits = true, fatal_data = false;
    std::atomic<std::uint32_t> policy_delay_us = 0;
    std::optional<IpEndpoint> selected_aux_peer;
    std::atomic<bool> drop_selected_aux = false;
    std::atomic<unsigned> selected_aux_drops = 0, bypassed_aux = 0;
    void select_aux_peer(IpEndpoint peer)
    {
        std::lock_guard lock(mutex);
        selected_aux_peer = peer;
    }
    std::atomic<unsigned> commits_dropped = 0, io_errors = 0,
                          policy_released = 0, reordered = 0;
    std::atomic<std::uint64_t> max_lateness_us = 0;
    std::array<std::uint64_t, 2> last_ordinal {};
    static std::optional<hn::Kind> body_kind(std::span<const std::byte> bytes)
    {
        const auto p = decode_packet(bytes);
        if (!p || p.packet.kind != PacketKind::control
            || p.packet.control.subtype != authenticated_control_subtype
            || !hn::native_candidate(p.packet))
            return {};
        const auto body = p.packet.payload;
        if (body.size() < 44)
            return {};
        const auto inner = decode_packet(body.subspan(12, body.size() - 44));
        hn::Message message;
        if (inner && hn::decode(inner.packet.payload, message))
            return message.kind;
        return {};
    }
    std::jthread worker;
    static std::uint64_t now()
    {
        return hn::NativeService::monotonic_now();
    }
    void install(std::shared_ptr<robotweax::srt::compat::DatagramChannel> c)
    {
        REQUIRE(!channel);
        channel = std::move(c);
        channel->set_send_hook_for_testing(send, this);
        worker = std::jthread([this](std::stop_token stop) {
            while (!stop.stop_requested()) {
                for (unsigned i = 0; i < 16; ++i) {
                    std::optional<Queue::Packet> p;
                    unsigned path = 0;
                    {
                        std::lock_guard lock(mutex);
                        p = auxiliary.take(now());
                        if (!p) {
                            p = data.take(now());
                            path = 1;
                        }
                    }
                    if (!p)
                        break;
                    if (p->ordinal < last_ordinal[path])
                        ++reordered;
                    last_ordinal[path] =
                        std::max(last_ordinal[path], p->ordinal);
                    max_lateness_us =
                        std::max(max_lateness_us.load(), now() - p->due_us);
                    const auto bytes = std::span {p->bytes}.first(p->size);
                    const auto result = channel->socket.send_to(bytes, p->peer);
                    if (!result)
                        ++io_errors;
                    else if (const auto kind = body_kind(bytes); kind
                        && (*kind == hn::Kind::delivery_commit
                            || *kind == hn::Kind::delivery_status))
                        ++policy_released;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds {1});
            }
        });
    }
    void rules(Queue::Rules aux, Queue::Rules payload)
    {
        REQUIRE(Queue::valid(aux));
        REQUIRE(Queue::valid(payload));
        std::lock_guard lock(mutex);
        auxiliary_rules = aux;
        data_rules = payload;
    }
    Queue::Rules payload_rules() const
    {
        std::lock_guard lock(mutex);
        return data_rules;
    }
    void payload_rules(Queue::Rules r)
    {
        std::lock_guard lock(mutex);
        data_rules = r;
    }
    Queue::Snapshot stats(bool payload = false) const
    {
        std::lock_guard lock(mutex);
        return payload ? data.snapshot() : auxiliary.snapshot();
    }
    static UdpIoResult send(
        std::span<const std::byte> bytes, IpEndpoint peer, void* p) noexcept
    {
        auto& self = *static_cast<HybridFaultLink*>(p);
        const auto packet = decode_packet(bytes);
        const bool payload = packet && packet.packet.kind == PacketKind::data;
        const bool aux = packet && hn::native_candidate(packet.packet);
        if (!payload && !aux)
            return self.channel->socket.send_to(
                bytes, peer); // HS/KM/ACK/NAK unchanged.
        bool selected = false, matches = true;
        if (aux) {
            std::lock_guard lock(self.mutex);
            selected = self.selected_aux_peer.has_value();
            matches = !selected || peer == *self.selected_aux_peer;
        }
        if (aux && selected && matches && self.drop_selected_aux.load()) {
            ++self.selected_aux_drops;
            return {.bytes_transferred = bytes.size()};
        }
        if (payload && self.fatal_data.load())
            return {.error = Error::io_error, .system_error = EIO};
        bool policy_reply = false;
        if (aux
            && packet.packet.control.subtype == authenticated_control_subtype) {
            const auto body = packet.packet.payload;
            if (body.size() >= 44) {
                const auto inner =
                    decode_packet(body.subspan(12, body.size() - 44));
                hn::Message message;
                if (inner && hn::decode(inner.packet.payload, message)) {
                    policy_reply = message.kind == hn::Kind::delivery_commit
                        || message.kind == hn::Kind::delivery_status;
                    if (message.kind == hn::Kind::delivery_commit
                        && self.drop_commits.load()) {
                        ++self.commits_dropped;
                        return {.bytes_transferred = bytes.size()};
                    }
                }
            }
        }
        if (aux && selected && !matches) {
            ++self.bypassed_aux;
            return self.channel->socket.send_to(bytes, peer);
        }
        std::lock_guard lock(self.mutex);
        auto rules = payload ? self.data_rules : self.auxiliary_rules;
        if (policy_reply && self.policy_delay_us.load()) {
            rules.delay_us = self.policy_delay_us.load();
            rules.alternating_delay_us = 0;
        }
        const auto r = (payload ? self.data : self.auxiliary)
                           .enqueue(bytes, peer, now(), rules);
        if (r == Queue::Result::accepted)
            return {.bytes_transferred = bytes.size()};
        return {.error = r == Queue::Result::retry ? Error::would_block
                                                   : Error::io_error};
    }
    ~HybridFaultLink()
    {
        if (channel)
            channel->set_send_hook_for_testing(nullptr, nullptr);
        if (worker.joinable()) {
            worker.request_stop();
            worker.join();
        }
    }
};
struct HybridNetworkFixture {
    StartupHooks startup;
    HybridFaultLink source, caller, multicast;
    HybridNetworkFixture()
    {
        startup.network_timing = true;
        startup.maximum_recoveries = 1;
        startup.reporting = {20'000, 600'000};
        startup.caller->join_success = true;
        startup.returning =
            hn::ReturnPolicy {1'500'000, 32, 4, 10'000, 300'000};
        source.rules({8'000, 16'000, 7, 3, 29, 1}, {4'000, 6'000, 13, 5, 29});
        caller.rules({1'000, 2'000, 11, 4, 31}, {});
        multicast.rules({}, {2'000, 6'000, 11, 3, 29});
    }
    static void configure(void* p, SRTSOCKET id, bool is_caller)
    {
        auto& self = *static_cast<HybridNetworkFixture*>(p);
        StartupHooks::configure(&self.startup, id, is_caller);
        if (!is_caller)
            self.source.install(self.startup.wire.channel);
    }
    void install_caller(SRTSOCKET id)
    {
        const auto group = GroupRegistry::instance().find(id);
        SRTSOCKET child;
        {
            std::lock_guard lock(group->mutex);
            child = group->members.front().public_data.id;
        }
        const auto socket = SocketRegistry::instance().find(child);
        std::shared_ptr<robotweax::srt::compat::DatagramChannel> c;
        {
            std::lock_guard lock(socket->mutex);
            c = socket->channel;
        }
        caller.install(std::move(c));
    }
};
template <class F> void hybrid_network_wait(F predicate)
{
    const auto until =
        std::chrono::steady_clock::now() + std::chrono::seconds {3};
    while (!predicate() && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(std::chrono::milliseconds {1});
    REQUIRE(predicate());
}
}
TEST(
    hybrid_network_asymmetric_impairment_preserves_integrated_lifecycle_plain_ctr_gcm)
{
    SKIP_UNLESS(loopback_multicast_available(), "no loopback multicast");
    for (unsigned mode = 0; mode < 3; ++mode) {
#ifndef ENABLE_AEAD_API_PREVIEW
        if (mode == 2)
            continue;
#endif
        HybridNetworkFixture net;
        CoordinatedNative f(1'000'000, mode != 0, mode == 2, 0, 200, true,
            false, HybridNetworkFixture::configure, &net);
        net.install_caller(f.receiver);
        net.multicast.install(f.source());
        auto owner = startup_owner(f.receiver);
        REQUIRE(owner);
        auto receiver = owner->receiver();
        auto membership = owner->membership();
        const auto await = [&](const char* label, auto predicate) {
            try {
                hybrid_network_wait(predicate);
            } catch (const std::exception&) {
                const auto r = receiver->snapshot();
                const auto m = membership->snapshot();
                throw std::runtime_error(std::string("network stage=") + label
                    + " mode=" + std::to_string(static_cast<unsigned>(r.mode))
                    + " revision=" + std::to_string(r.revision)
                    + " fresh=" + std::to_string(r.fresh)
                    + " closed=" + std::to_string(r.closed) + " return="
                    + std::to_string(static_cast<unsigned>(r.returning))
                    + " fallback="
                    + std::to_string(static_cast<unsigned>(r.fallback))
                    + " member_phase="
                    + std::to_string(static_cast<unsigned>(m.phase))
                    + " source_fail="
                    + std::to_string(f.actor()->snapshot().failed) + " queued="
                    + std::to_string(net.source.stats().queued) + " source_io="
                    + std::to_string(net.source.io_errors.load()));
            }
        };
        await("initial join", [&] {
            return membership->snapshot().joined;
        });
        int next = 0;
        const auto read = [&](int first, unsigned n) {
            for (int index = first; index < first + static_cast<int>(n);
                ++index) {
                std::array<char, 256> bytes {};
                const auto expected = message(index);
                const int received =
                    srt_recvmsg(f.receiver, bytes.data(), bytes.size());
                if (received != static_cast<int>(expected.size())
                    || !std::equal(
                        expected.begin(), expected.end(), bytes.begin())) {
                    const auto r = receiver->snapshot();
                    throw std::runtime_error(
                        "network DATA first=" + std::to_string(first)
                        + " expected=" + std::to_string(index)
                        + " received=" + std::to_string(received) + " error="
                        + std::to_string(srt_getlasterror(nullptr)) + " text="
                        + std::string(bytes.data(), received > 0 ? received : 0)
                        + " mode="
                        + std::to_string(static_cast<unsigned>(r.mode))
                        + " revision=" + std::to_string(r.revision)
                        + " UCdrops="
                        + std::to_string(net.source.stats(true).dropped)
                        + " MCdrops="
                        + std::to_string(net.multicast.stats(true).dropped));
                }
            }
        };
        const auto burst = [&](unsigned n, bool drain = true) {
            const int first = next;
            const auto saved_uc = net.source.payload_rules(),
                       saved_mc = net.multicast.payload_rules();
            for (unsigned i = 0; i < n; ++i) {
                if (i + 2 >= n) {
                    auto uc = saved_uc, mc = saved_mc;
                    uc.drop_every = mc.drop_every = 0;
                    uc.drop_first = mc.drop_first = 0;
                    net.source.payload_rules(uc);
                    net.multicast.payload_rules(mc);
                }
                REQUIRE_EQ(
                    f.send(next), static_cast<int>(message(next).size()));
                ++next;
                std::this_thread::sleep_for(std::chrono::milliseconds {3});
            }
            if (drain)
                read(first, n);
            net.source.payload_rules(saved_uc);
            net.multicast.payload_rules(saved_mc);
        };
        const auto uc = [&] {
            REQUIRE_EQ(receiver->request_fallback(next),
                hn::ReceiverBridge::Admission::accepted);
            await("UC confirmation", [&] {
                auto s = receiver->snapshot();
                return s.fresh && s.mode == hn::Mode::unicast;
            });
        };
        const auto mc = [&] {
            net.multicast.rules({},
                {2'000, 6'000, 0, 3,
                    29}); // Complete new-route proof still mandatory.
            REQUIRE_EQ(receiver->request_overlap(),
                hn::ReceiverBridge::Admission::accepted);
            await("overlap confirmation", [&] {
                auto s = receiver->snapshot();
                return s.fresh && s.mode == hn::Mode::overlap;
            });
            const auto b = receiver->snapshot().commit_position;
            REQUIRE_EQ(b, static_cast<std::uint64_t>(next));
            const int first = next;
            burst(8, false);
            std::optional<hn::CoverageProof> proof;
            await("exact MC proof", [&] {
                proof = receiver->coverage_proof(b + 8, 200'000);
                return proof.has_value();
            });
            REQUIRE_EQ(receiver->request_multicast(*proof),
                hn::ReceiverBridge::Admission::accepted);
            await("MC confirmation", [&] {
                auto s = receiver->snapshot();
                return s.fresh && s.mode == hn::Mode::multicast;
            });
            read(first, 8);
            net.multicast.rules({}, {2'000, 6'000, 11, 3, 29});
        };
        mc();
        burst(40);
        const auto epoch = membership->snapshot().route_epoch;
        REQUIRE(membership->report_loss());
        net.startup.caller->recovery_failures = 2;
        uc();
        burst(40);
        await("rejoin", [&] {
            return membership->snapshot().joined;
        });
        REQUIRE(membership->snapshot().route_epoch > epoch);
        REQUIRE_EQ(membership->snapshot().failures, 2U);
        mc();
        burst(40);
        uc();
        net.multicast.fatal_data = true;
        burst(1);
        wait_for_source_failure(f.actor());
        await("source error report", [&] {
            auto r = receiver->peer_report(800'000);
            return r && r->fresh
                && r->message.activity == hn::Activity::source_failed;
        });
        std::array<PolicyActor::Member, 1> members;
        REQUIRE_EQ(f.actor()->members(members), 1U);
        const auto old_revision = members[0].revision;
        const std::array selected {
            PolicyActor::RecoveryMember {members[0].id, old_revision}};
        REQUIRE_EQ(recover_native_source(f.actor(), selected).status,
            PolicyActor::TransitionStatus::applied);
        await("source recovery confirmation", [&] {
            auto s = receiver->snapshot();
            return s.fresh && s.revision == old_revision + 1
                && s.mode == hn::Mode::unicast;
        });
        burst(40);
        REQUIRE_EQ(f.actor()->snapshot().source_revision, 1U);
        REQUIRE_EQ(f.actor()->snapshot().unicast_members, 1U);
        for (auto* link : {&net.source, &net.caller, &net.multicast}) {
            REQUIRE_EQ(link->io_errors.load(), 0U);
            for (bool payload : {false, true})
                REQUIRE_EQ(link->stats(payload).overflows, 0U);
        }
        REQUIRE(net.source.stats().dropped > 0);
        REQUIRE(net.source.stats().duplicates > 0);
        REQUIRE(net.caller.stats().duplicates > 0);
        REQUIRE(net.multicast.stats(true).dropped > 0);
        REQUIRE(net.source.stats(true).dropped > 0);
        REQUIRE(net.source.commits_dropped.load() >= 7);
        REQUIRE(net.source.reordered.load() > 0);
        REQUIRE(net.multicast.reordered.load() > 0);
        REQUIRE(net.startup.caller->hidden.load());
        REQUIRE(net.startup.caller->io_hidden.load());
        for (unsigned index = 0;
            auto* link : {&net.source, &net.caller, &net.multicast}) {
            const auto a = link->stats(), d = link->stats(true);
            std::fprintf(stdout,
                "[HYBRID_NETWORK] "
                "{\"profile\":%u,\"link\":%u,\"messages\":%d,\"aux_accepted\":%"
                "llu,\"aux_dropped\":%llu,\"aux_duplicates\":%llu,\"aux_"
                "retries\":%llu,\"aux_peak\":%zu,\"data_accepted\":%llu,\"data_"
                "dropped\":%llu,\"data_duplicates\":%llu,\"data_retries\":%llu,"
                "\"data_peak\":%zu,\"reordered\":%u,\"max_lateness_us\":%llu}"
                "\n",
                mode, index++, next,
                static_cast<unsigned long long>(a.accepted),
                static_cast<unsigned long long>(a.dropped),
                static_cast<unsigned long long>(a.duplicates),
                static_cast<unsigned long long>(a.retries), a.peak,
                static_cast<unsigned long long>(d.accepted),
                static_cast<unsigned long long>(d.dropped),
                static_cast<unsigned long long>(d.duplicates),
                static_cast<unsigned long long>(d.retries), d.peak,
                link->reordered.load(),
                static_cast<unsigned long long>(link->max_lateness_us.load()));
        }
    }
}
TEST(hybrid_network_delayed_policy_replies_cannot_revive_closed_startup)
{
    SKIP_UNLESS(loopback_multicast_available(), "no loopback multicast");
    HybridNetworkFixture net;
    net.source.drop_commits = false;
    net.source.policy_delay_us = 250'000;
    StartupServer server(&net.startup);
    net.source.install(net.startup.wire.channel);
    int result = 0;
    const auto receiver =
        make_receiver(server.peers, server.bound, false, false, 120, true, true,
            false, HybridNetworkFixture::configure, &net, &result);
    REQUIRE(result != SRT_ERROR);
    const auto sender =
        server.peers.keep(srt_accept(server.listener, nullptr, nullptr));
    REQUIRE(sender != SRT_INVALID_SOCK);
    std::shared_ptr<hn::StartupSession> owner;
    hybrid_network_wait([&] {
        owner = startup_owner(receiver);
        return owner
            && owner->snapshot().phase == hn::StartupSession::Phase::admitting;
    });
    const auto group = GroupRegistry::instance().find(sender);
    std::shared_ptr<PolicyActor> actor;
    {
        std::lock_guard lock(group->mutex);
        actor = group->dispatch->coordinator;
    }
    hybrid_network_wait([&] {
        return actor->snapshot().unicast_members == 1;
    });
    REQUIRE(!owner->snapshot().published);
    REQUIRE(srt_getsockstate(receiver) != SRTS_CONNECTED);
    REQUIRE_EQ(net.source.policy_released.load(), 0U);
    server.peers.drop(receiver);
    hybrid_network_wait([&] {
        return net.source.policy_released.load() > 0;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds {50});
    REQUIRE(!owner->snapshot().published);
    REQUIRE(owner->snapshot().closed);
    REQUIRE(srt_getsockstate(receiver) != SRTS_CONNECTED);
    REQUIRE_EQ(net.startup.caller->joins.load(), 0U);
}
TEST(hybrid_network_original_connect_deadline_survives_delayed_exchange)
{
    SKIP_UNLESS(loopback_multicast_available(), "no loopback multicast");
    HybridNetworkFixture net;
    net.startup.connect_timeout = 120;
    net.source.drop_commits = false;
    net.source.rules({200'000}, {});
    StartupServer server(&net.startup);
    net.source.install(net.startup.wire.channel);
    int result = 0;
    const auto begin = std::chrono::steady_clock::now();
    const auto receiver =
        make_receiver(server.peers, server.bound, false, false, 120, true,
            false, false, HybridNetworkFixture::configure, &net, &result);
    REQUIRE_EQ(result, SRT_ERROR);
    REQUIRE(std::chrono::steady_clock::now() - begin
        < std::chrono::milliseconds {600});
    hybrid_network_wait([&] {
        return net.source.stats().delivered > 0;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds {250});
    REQUIRE(srt_getsockstate(receiver) != SRTS_CONNECTED);
    REQUIRE_EQ(net.startup.caller->joins.load(), 0U);
}
#endif

#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_HYBRID_CONTROL
namespace {
struct HybridMultiReceiver {
    SRTSOCKET id = SRT_INVALID_SOCK;
    std::shared_ptr<hn::StartupSession> owner;
    std::shared_ptr<StartupProvision> provider;
    std::uint64_t first = 0, read_next = 0, initial_epoch = 0;
};
struct HybridMultiScenario {
    HybridNetworkFixture net;
    std::unique_ptr<CoordinatedNative> stream;
    std::vector<HybridMultiReceiver> peers;
    unsigned mode;
    int next = 0;
    unsigned healthy_checks = 0;
    std::vector<std::size_t> monitored;
    explicit HybridMultiScenario(unsigned profile)
        : mode(profile)
    {
        net.startup.source->expiry =
            hn::NativeService::monotonic_now() + 10'000'000;
        net.startup.caller->expiry = net.startup.source->expiry;
        stream =
            std::make_unique<CoordinatedNative>(1'000'000, mode != 0, mode == 2,
                0, 200, true, false, HybridNetworkFixture::configure, &net);
        peers.push_back(describe(stream->receiver, net.startup.caller));
        net.install_caller(stream->receiver);
        net.multicast.install(stream->source());
    }
    HybridMultiReceiver describe(
        SRTSOCKET id, std::shared_ptr<StartupProvision> provider)
    {
        auto owner = startup_owner(id);
        REQUIRE(owner);
        hybrid_network_wait([&] {
            return owner->membership()->snapshot().joined;
        });
        const auto data = owner->receiver()->data_progress();
        REQUIRE(data);
        return {id, owner, std::move(provider), data->origin, data->origin,
            owner->membership()->snapshot().route_epoch};
    }
    IpEndpoint endpoint(SRTSOCKET id)
    {
        const auto group = GroupRegistry::instance().find(id);
        SRTSOCKET child;
        {
            std::lock_guard lock(group->mutex);
            child = group->members.front().public_data.id;
        }
        const auto r = SocketRegistry::instance().find(child);
        std::lock_guard lock(r->mutex);
        return r->has_connected_local_endpoint ? r->connected_local_endpoint
                                               : r->local_endpoint;
    }
    bool key_ready(SRTSOCKET id)
    {
        if (!mode)
            return true;
        const auto target = endpoint(id);
        const auto group = GroupRegistry::instance().find(stream->sender);
        std::vector<SRTSOCKET> children;
        {
            std::lock_guard lock(group->mutex);
            for (const auto& m : group->members)
                children.push_back(m.public_data.id);
        }
        for (auto child : children) {
            auto r = SocketRegistry::instance().find(child);
            if (!r)
                continue;
            bool matches = false;
            {
                std::lock_guard lock(r->mutex);
                matches = r->peer_endpoint == target;
            }
            if (matches) {
                int state = -1, size = sizeof(state);
                return srt_getsockflag(child, SRTO_SNDKMSTATE, &state, &size)
                    == 0
                    && state == SRT_KM_S_SECURED;
            }
        }
        return false;
    }
    SRTSOCKET attempt(int* result = nullptr)
    {
        net.startup.caller = std::make_shared<StartupProvision>();
        net.startup.caller->join_success = true;
        net.startup.caller->expiry =
            hn::NativeService::monotonic_now() + 10'000'000;
        return make_receiver(stream->peers, stream->bound, mode != 0, mode == 2,
            200, true, false, false, HybridNetworkFixture::configure, &net,
            result);
    }
    void add()
    {
        const auto id = attempt();
        auto p = describe(id, net.startup.caller);
        REQUIRE_EQ(p.first, static_cast<std::uint64_t>(next));
        hybrid_network_wait([&] {
            return key_ready(id);
        });
        peers.push_back(std::move(p));
    }
    void monitor()
    {
        for (auto index : monitored) {
            const auto r = peers[index].owner->receiver()->snapshot();
            REQUIRE(r.fresh);
            REQUIRE(!r.closed);
            REQUIRE_EQ(srt_getsockstate(peers[index].id), SRTS_CONNECTED);
            ++healthy_checks;
        }
    }
    void read_all()
    {
        for (auto& p : peers) {
            while (p.read_next < static_cast<std::uint64_t>(next)) {
                std::array<char, 256> data {};
                const auto expected = message(static_cast<int>(p.read_next));
                const auto n = srt_recvmsg(p.id, data.data(), data.size());
                if (n != static_cast<int>(expected.size())
                    || !std::equal(
                        expected.begin(), expected.end(), data.begin()))
                    throw std::runtime_error(
                        "multi DATA peer=" + std::to_string(&p - peers.data())
                        + " expected=" + std::to_string(p.read_next)
                        + " received=" + std::to_string(n) + " error="
                        + std::to_string(srt_getlasterror(nullptr)));
                ++p.read_next;
                monitor();
            }
        }
    }
    void burst(unsigned count, bool drain = true)
    {
        const auto uc_saved = net.source.payload_rules(),
                   mc_saved = net.multicast.payload_rules();
        for (unsigned i = 0; i < count; ++i) {
            if (i + 2 >= count) {
                auto uc = uc_saved, mc = mc_saved;
                uc.drop_every = mc.drop_every = 0;
                uc.drop_first = mc.drop_first = 0;
                net.source.payload_rules(uc);
                net.multicast.payload_rules(mc);
            }
            REQUIRE_EQ(
                stream->send(next), static_cast<int>(message(next).size()));
            ++next;
            monitor();
            std::this_thread::sleep_for(std::chrono::milliseconds {3});
        }
        if (drain)
            read_all();
        net.source.payload_rules(uc_saved);
        net.multicast.payload_rules(mc_saved);
    }
    void multicast(std::size_t index)
    {
        auto receiver = peers[index].owner->receiver();
        net.multicast.rules({}, {2'000, 6'000, 0, 3, 29});
        REQUIRE_EQ(receiver->request_overlap(),
            hn::ReceiverBridge::Admission::accepted);
        hybrid_network_wait([&] {
            auto s = receiver->snapshot();
            return s.fresh && s.mode == hn::Mode::overlap;
        });
        const auto b = receiver->snapshot().commit_position;
        REQUIRE_EQ(b, static_cast<std::uint64_t>(next));
        burst(8, false);
        std::optional<hn::CoverageProof> proof;
        hybrid_network_wait([&] {
            proof = receiver->coverage_proof(b + 8, 200'000);
            return proof.has_value();
        });
        REQUIRE_EQ(receiver->request_multicast(*proof),
            hn::ReceiverBridge::Admission::accepted);
        hybrid_network_wait([&] {
            auto s = receiver->snapshot();
            return s.fresh && s.mode == hn::Mode::multicast;
        });
        read_all();
        net.multicast.rules({}, {2'000, 6'000, 11, 3, 29});
    }
    void isolate_first()
    {
        net.source.select_aux_peer(endpoint(peers[0].id));
        net.source.drop_selected_aux = true;
        hybrid_network_wait([&] {
            return !peers[0].owner->receiver()->snapshot().fresh;
        });
        REQUIRE(net.source.selected_aux_drops.load() > 0);
    }
    void check_links()
    {
        for (auto* link : {&net.source, &net.caller, &net.multicast}) {
            REQUIRE_EQ(link->io_errors.load(), 0U);
            REQUIRE_EQ(link->stats().overflows, 0U);
            REQUIRE_EQ(link->stats(true).overflows, 0U);
        }
        REQUIRE(net.source.bypassed_aux.load() > 0);
    }
};
}
TEST(
    hybrid_network_multi_receiver_control_loss_does_not_stall_healthy_peers_plain_ctr_gcm)
{
    SKIP_UNLESS(loopback_multicast_available(), "no loopback multicast");
    for (unsigned mode = 0; mode < 3; ++mode) {
#ifndef ENABLE_AEAD_API_PREVIEW
        if (mode == 2)
            continue;
#endif
        HybridMultiScenario f(mode);
        f.add();
        f.add();
        REQUIRE(wait_for_members(f.stream->sender, 3, mode != 0));
        f.multicast(2);
        f.monitored = {1, 2};
        f.burst(40);
        const auto b_epoch =
            f.peers[1].owner->membership()->snapshot().route_epoch;
        const auto c_epoch =
            f.peers[2].owner->membership()->snapshot().route_epoch;
        const auto b_revision =
            f.peers[1].owner->receiver()->snapshot().revision;
        const auto c_revision =
            f.peers[2].owner->receiver()->snapshot().revision;
        const auto b_runtime = hybrid_runtime(f.peers[1].id),
                   c_runtime = hybrid_runtime(f.peers[2].id);
        f.isolate_first();
        f.burst(40);
        auto a = f.peers[0].owner;
        const auto attempts = a->membership()->snapshot().attempts;
        REQUIRE(a->membership()->report_loss());
        f.peers[0].provider->recovery_failures = 2;
        f.burst(40);
        REQUIRE_EQ(a->membership()->snapshot().attempts, attempts);
        REQUIRE(!a->membership()->snapshot().joined);
        f.net.source.drop_selected_aux = false;
        hybrid_network_wait([&] {
            return a->membership()->snapshot().joined;
        });
        REQUIRE_EQ(a->membership()->snapshot().recoveries, 1U);
        REQUIRE_EQ(a->membership()->snapshot().failures, 2U);
        f.burst(40);
        f.check_links();
        REQUIRE_EQ(f.next, 168);
        REQUIRE_EQ(f.stream->actor()->snapshot().unicast_members, 2U);
        REQUIRE_EQ(
            f.peers[1].owner->membership()->snapshot().route_epoch, b_epoch);
        REQUIRE_EQ(
            f.peers[2].owner->membership()->snapshot().route_epoch, c_epoch);
        REQUIRE_EQ(f.stream->actor()->snapshot().detached_members, 0U);
        REQUIRE_EQ(
            f.peers[1].owner->receiver()->snapshot().mode, hn::Mode::unicast);
        REQUIRE_EQ(
            f.peers[2].owner->receiver()->snapshot().mode, hn::Mode::multicast);
        REQUIRE_EQ(
            f.peers[1].owner->receiver()->snapshot().revision, b_revision);
        REQUIRE_EQ(
            f.peers[2].owner->receiver()->snapshot().revision, c_revision);
        REQUIRE(hybrid_runtime(f.peers[1].id) == b_runtime);
        REQUIRE(hybrid_runtime(f.peers[2].id) == c_runtime);
        for (const auto& p : f.peers) {
            REQUIRE_EQ(p.read_next, 168U);
            REQUIRE_EQ(hybrid_runtime(p.id)
                           ->statistics(false, false)
                           .total.received_unique.packets,
                168U);
        }
        std::fprintf(stdout,
            "[HYBRID_MULTI] "
            "{\"scenario\":0,\"profile\":%u,\"source_messages\":168,\"received_"
            "messages\":504,\"data_receivers\":3,\"healthy_control_checks\":%u,"
            "\"isolated_aux_drops\":%u,\"bypassed_aux\":%u,\"rejoin_failures\":"
            "2,\"uc_members\":2}\n",
            mode, f.healthy_checks, f.net.source.selected_aux_drops.load(),
            f.net.source.bypassed_aux.load());
    }
}
TEST(
    hybrid_network_multi_receiver_stale_control_retains_quota_until_real_return_plain_ctr_gcm)
{
    SKIP_UNLESS(loopback_multicast_available(), "no loopback multicast");
    for (unsigned mode = 0; mode < 3; ++mode) {
#ifndef ENABLE_AEAD_API_PREVIEW
        if (mode == 2)
            continue;
#endif
        HybridMultiScenario f(mode);
        f.add();
        f.add();
        f.add();
        REQUIRE(wait_for_members(f.stream->sender, 4, mode != 0));
        f.monitored = {1, 2, 3};
        f.isolate_first();
        REQUIRE_EQ(f.stream->actor()->snapshot().unicast_reservations, 4U);
        int result = 0;
        const auto rejected = f.attempt(&result);
        auto rejected_provider = f.net.startup.caller;
        REQUIRE_EQ(result, SRT_ERROR);
        REQUIRE(srt_getsockstate(rejected) != SRTS_CONNECTED);
        REQUIRE_EQ(rejected_provider->joins.load(), 0U);
        REQUIRE_EQ(f.stream->actor()->snapshot().unicast_members, 4U);
        f.burst(40);
        f.monitored = {2, 3};
        f.multicast(1);
        hybrid_network_wait([&] {
            return f.stream->actor()->snapshot().unicast_reservations == 3;
        });
        f.add();
        REQUIRE_EQ(f.peers.back().first, 48U);
        f.monitored = {1, 2, 3, 4};
        REQUIRE_EQ(f.stream->actor()->snapshot().unicast_members, 4U);
        result = 0;
        const auto refused_again = f.attempt(&result);
        REQUIRE_EQ(result, SRT_ERROR);
        REQUIRE(srt_getsockstate(refused_again) != SRTS_CONNECTED);
        REQUIRE_EQ(f.net.startup.caller->joins.load(), 0U);
        f.burst(40);
        f.check_links();
        REQUIRE_EQ(f.next, 88);
        REQUIRE_EQ(f.stream->actor()->snapshot().unicast_reservations, 4U);
        REQUIRE(!f.peers[0].owner->receiver()->snapshot().fresh);
        for (std::size_t i = 0; i < f.peers.size(); ++i) {
            const auto& p = f.peers[i];
            REQUIRE_EQ(p.read_next, 88U);
            REQUIRE_EQ(p.owner->receiver()->snapshot().mode,
                i == 1 ? hn::Mode::multicast : hn::Mode::unicast);
            REQUIRE_EQ(hybrid_runtime(p.id)
                           ->statistics(false, false)
                           .total.received_unique.packets,
                88U - p.first);
        }
        std::fprintf(stdout,
            "[HYBRID_MULTI] "
            "{\"scenario\":1,\"profile\":%u,\"source_messages\":88,\"received_"
            "messages\":392,\"data_receivers\":5,\"healthy_control_checks\":%u,"
            "\"isolated_aux_drops\":%u,\"bypassed_aux\":%u,\"rejected_"
            "callers\":2,\"late_origin\":48,\"uc_members\":4,\"uc_"
            "reservations\":4}\n",
            mode, f.healthy_checks, f.net.source.selected_aux_drops.load(),
            f.net.source.bypassed_aux.load());
    }
}
#endif
