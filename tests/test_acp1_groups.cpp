// SPDX-License-Identifier: MIT
#include "test.hpp"
#include "compat/acp1_group_context.hpp"
#include "compat/socket_registry.hpp"
#include "compat/group_registry.hpp"
#include "compat/transport_runtime.hpp"
#include "srt/srt.h"
#include "robotweax/srt/handshake_datagram.hpp"
#include <atomic>
#include <array>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>
using namespace robotweax::srt;
using namespace robotweax::srt::compat;
#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_ACP1
namespace {
struct Sockets {
    std::vector<SRTSOCKET> handles;
    Sockets()
    {
        REQUIRE_EQ(srt_startup(), 0);
    }
    ~Sockets()
    {
        for (const auto s : handles)
            (void)srt_close(s);
        (void)srt_cleanup();
    }
    SRTSOCKET own(SRTSOCKET s)
    {
        REQUIRE(s != SRT_INVALID_SOCK);
        handles.push_back(s);
        return s;
    }
};
void set(SRTSOCKET s, SRT_SOCKOPT o, int v)
{
    REQUIRE_EQ(srt_setsockflag(s, o, &v, sizeof(v)), 0);
}
bool flag(SRTSOCKET s, SRT_SOCKOPT o)
{
    bool v = false;
    int n = sizeof(v);
    REQUIRE_EQ(srt_getsockflag(s, o, &v, &n), 0);
    return v;
}
void configure(SRTSOCKET s, bool acp = true, bool gcm = false)
{
    set(s, SRTO_ROBOTWEAX_SESSIONAUTH, 1);
    if (acp)
        set(s, SRTO_ROBOTWEAX_ACP1_REQUIRED, 1);
    constexpr char pass[] = "acp1-group-password";
    REQUIRE_EQ(srt_setsockflag(s, SRTO_PASSPHRASE, pass, sizeof(pass) - 1), 0);
    set(s, SRTO_LATENCY, 20);
    set(s, SRTO_CONNTIMEO, 700);
    set(s, SRTO_SNDTIMEO, 2000);
    set(s, SRTO_RCVTIMEO, 2000);
    set(s, SRTO_KMREFRESHRATE, 8);
    set(s, SRTO_KMPREANNOUNCE, 1);
#ifdef ENABLE_AEAD_API_PREVIEW
    if (gcm)
        set(s, SRTO_CRYPTOMODE, 2);
#else
    (void)gcm;
#endif
}
std::shared_ptr<ConnectionRuntime> runtime(SRTSOCKET s)
{
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds {2};
    do {
        const auto record = SocketRegistry::instance().find(s);
        if (record) {
            std::lock_guard lock(record->mutex);
            if (record->runtime)
                return record->runtime;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds {1});
    } while (std::chrono::steady_clock::now() < deadline);
    REQUIRE(false);
    return {};
}
void transfer(SRTSOCKET from, SRTSOCKET to, unsigned first, unsigned count)
{
    for (unsigned i = first; i < first + count; ++i) {
        std::array<char, 256> payload {};
        payload.fill(static_cast<char>(i));
        REQUIRE_EQ(srt_sendmsg(from, payload.data(), payload.size(), -1, 1),
            static_cast<int>(payload.size()));
        std::array<char, 1500> received {};
        REQUIRE_EQ(srt_recvmsg(to, received.data(), received.size()),
            static_cast<int>(payload.size()));
        REQUIRE(std::equal(payload.begin(), payload.end(), received.begin()));
    }
}
}
TEST(acp1_groups_broadcast_backup_rotation_late_join_replacement_and_policy)
{
    for (const auto type : {SRT_GTYPE_BROADCAST, SRT_GTYPE_BACKUP}) {
        for (const bool gcm : {false, true}) {
#ifndef ENABLE_AEAD_API_PREVIEW
            if (gcm)
                continue;
#endif
            Sockets sockets;
            const auto listener = sockets.own(srt_create_socket());
            const auto group = sockets.own(srt_create_group(type));
            configure(listener, true, gcm);
            configure(group, true, gcm);
            set(listener, SRTO_GROUPCONNECT, 1);
            set(listener, SRTO_ROBOTWEAX_PATHID_REQUIRED, 1);
            set(group, SRTO_ROBOTWEAX_PATHID_REQUIRED, 1);
            if (type == SRT_GTYPE_BACKUP) {
                set(listener, SRTO_ROBOTWEAX_STANDBYRTT_REQUIRED, 1);
                set(group, SRTO_ROBOTWEAX_STANDBYRTT_REQUIRED, 1);
            }
            REQUIRE(!flag(group, SRTO_ROBOTWEAX_ACP1_ACTIVE));
            sockaddr_in address {};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            REQUIRE_EQ(srt_bind(listener, reinterpret_cast<sockaddr*>(&address),
                           sizeof(address)),
                0);
            REQUIRE_EQ(srt_listen(listener, 8), 0);
            int size = sizeof(address);
            REQUIRE_EQ(srt_getsockname(listener,
                           reinterpret_cast<sockaddr*>(&address), &size),
                0);
            bool first_join = true;
            const auto join = [&](const char* path) {
                auto* config = srt_create_config();
                REQUIRE(config);
                REQUIRE_EQ(srt_config_add(config, SRTO_ROBOTWEAX_PATHID, path,
                               std::strlen(path)),
                    0);
                auto endpoint = srt_prepare_endpoint(nullptr,
                    reinterpret_cast<const sockaddr*>(&address),
                    sizeof(address));
                endpoint.config = config;
                const auto result = srt_connect_group(group, &endpoint, 1);
                srt_delete_config(config);
                REQUIRE(result != SRT_ERROR);
                // Only the first synchronous join blocks. Later joins must
                // wait for CONNECTED, which itself must imply an active runtime.
                if (!first_join) {
                    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::seconds {2};
                    while (srt_getsockstate(endpoint.id) != SRTS_CONNECTED
                        && std::chrono::steady_clock::now() < deadline)
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds {1});
                }
                first_join = false;
                REQUIRE_EQ(srt_getsockstate(endpoint.id), SRTS_CONNECTED);
                REQUIRE(flag(endpoint.id, SRTO_ROBOTWEAX_ACP1_ACTIVE));
                REQUIRE(flag(group, SRTO_ROBOTWEAX_ACP1_ACTIVE));
                return endpoint.id;
            };
            const auto first = join("path-a");
            const auto mirror =
                sockets.own(srt_accept(listener, nullptr, nullptr));
            REQUIRE(flag(mirror, SRTO_ROBOTWEAX_ACP1_REQUIRED));
            REQUIRE(flag(group, SRTO_ROBOTWEAX_ACP1_ACTIVE));
            REQUIRE(flag(mirror, SRTO_ROBOTWEAX_ACP1_ACTIVE));
            transfer(group, mirror, 1, 20);
            transfer(mirror, group, 31, 20);
            const auto second = join("path-b");
            transfer(group, mirror, 61, 20);
            transfer(mirror, group, 91, 20);
            REQUIRE_EQ(srt_close(first), 0);
            transfer(group, mirror, 121, 20);
            transfer(mirror, group, 151, 20);
            const auto replacement = join("path-a");
            REQUIRE(replacement != first);
            transfer(group, mirror, 181, 20);
            REQUIRE(flag(second, SRTO_ROBOTWEAX_ACP1_ACTIVE));
            REQUIRE(flag(group, SRTO_ROBOTWEAX_ACP1_ACTIVE));
            const int disabled = 0;
            REQUIRE_EQ(srt_setsockflag(group, SRTO_ROBOTWEAX_ACP1_REQUIRED,
                           &disabled, sizeof(disabled)),
                SRT_ERROR);
            REQUIRE_EQ(srt_setsockflag(second, SRTO_ROBOTWEAX_ACP1_REQUIRED,
                           &disabled, sizeof(disabled)),
                SRT_ERROR);
            auto* config = srt_create_config();
            REQUIRE(config);
            REQUIRE_EQ(srt_config_add(config, SRTO_ROBOTWEAX_ACP1_REQUIRED,
                           &disabled, sizeof(disabled)),
                0);
            auto endpoint = srt_prepare_endpoint(nullptr,
                reinterpret_cast<const sockaddr*>(&address), sizeof(address));
            endpoint.config = config;
            REQUIRE_EQ(srt_connect_group(group, &endpoint, 1), SRT_ERROR);
            REQUIRE_EQ(endpoint.errorcode, SRT_EINVPARAM);
            srt_delete_config(config);
        }
    }
}
TEST(acp1_groups_reject_legacy_member_without_fallback)
{
    for (const bool protected_sender : {true, false}) {
        Sockets sockets;
        const auto listener = sockets.own(srt_create_socket());
        const auto group = sockets.own(srt_create_group(SRT_GTYPE_BROADCAST));
        configure(listener, !protected_sender);
        configure(group, protected_sender);
        set(listener, SRTO_GROUPCONNECT, 1);
        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        REQUIRE_EQ(srt_bind(listener, reinterpret_cast<sockaddr*>(&address),
                       sizeof(address)),
            0);
        REQUIRE_EQ(srt_listen(listener, 4), 0);
        int size = sizeof(address);
        REQUIRE_EQ(srt_getsockname(
                       listener, reinterpret_cast<sockaddr*>(&address), &size),
            0);
        auto endpoint = srt_prepare_endpoint(nullptr,
            reinterpret_cast<const sockaddr*>(&address), sizeof(address));
        REQUIRE_EQ(srt_connect_group(group, &endpoint, 1), SRT_ERROR);
        REQUIRE(!flag(group, SRTO_ROBOTWEAX_ACP1_ACTIVE));
    }
}

TEST(acp1_groups_standby_controls_authenticate_before_monitoring_and_liveness)
{
    std::uint64_t now = 1000;
    const auto clock = [](void* p) noexcept {
        return *static_cast<std::uint64_t*>(p);
    };
    auto service = std::make_shared<StandbyRttService>(clock, &now);
    auto group = std::make_shared<GroupRecord>();
    group->type = SRT_GTYPE_BACKUP;
    auto channel = std::make_shared<DatagramChannel>();
    std::vector<std::byte> sent;
    channel->set_send_hook_for_testing(
        [](std::span<const std::byte> bytes, IpEndpoint, void* p) noexcept {
            auto& out = *static_cast<std::vector<std::byte>*>(p);
            out.assign(bytes.begin(), bytes.end());
            return UdpIoResult {.bytes_transferred = bytes.size()};
        },
        &sent);
    StandbyRttIdentity identity {};
    identity.caller.fill(std::byte {1});
    identity.listener.fill(std::byte {2});
    const auto context =
        acp1_group_context({0x40000001, GroupType::backup, 0, 1},
            {0x40000002, GroupType::backup, 0, 1}, {}, true, identity);
    auto local = std::make_shared<CryptoSession>(
        CryptoConfiguration {.passphrase = "acp1-group-password"});
    CryptoSession remote({.passphrase = "acp1-group-password"});
    REQUIRE_EQ(local->start_session_authentication(true, 5), Error::none);
    REQUIRE_EQ(remote.start_session_authentication(false, 5), Error::none);
    REQUIRE_EQ(remote.establish_session_authentication(
                   local->session_authentication()->parameters(), 77, 66, 0, 0,
                   context),
        Error::none);
    REQUIRE_EQ(local->establish_session_authentication(
                   remote.session_authentication()->parameters(), 66, 77, 0, 0,
                   context),
        Error::none);
    SocketOptions options;
    REQUIRE_EQ(options.set(SocketOption::experimental_acp1, 1), Error::none);
    const auto peer = IpEndpoint::loopback(9022);
    auto runtime = std::make_shared<ConnectionRuntime>(
        ConnectionRuntime::Configuration {.channel = channel,
            .group = group,
            .peer = peer,
            .local_socket_id = 66,
            .peer_socket_id = 77,
            .initial_sequence = SequenceNumber {900},
            .standby_rtt_identity = identity,
            .standby_rtt_caller = true,
            .standby_rtt_service_for_testing = service,
            .options = options,
            .origin = ConnectionRuntime::Clock::now(),
            .crypto = local,
            .now_function = clock,
            .now_context = &now});
    const auto health = runtime->response_health();
    (void)runtime->poll();
    REQUIRE_EQ(sent.size(),
        standby_rtt_datagram_size + authenticated_control_overhead);
    const auto decoded = decode_packet(sent);
    REQUIRE(decoded);
    PacketView opened;
    REQUIRE(
        remote.session_authentication()->open_control(decoded.packet, opened));
    const auto request = decode_standby_rtt_packet(opened);
    REQUIRE(request);
    auto echo = request.message;
    echo.kind = StandbyRttKind::echo;
    std::array<std::byte, standby_rtt_datagram_size> bare {};
    REQUIRE(encode_standby_rtt_datagram(echo, PacketTimestamp {10}, 66, bare));
    now = 5000;
    runtime->process_packet(decode_packet(bare).packet, peer);
    REQUIRE_EQ(runtime->standby_rtt_data().samples, 0U);
    std::array<std::byte, 1500> protected_echo {};
    std::size_t written = 0;
    REQUIRE_EQ(remote.session_authentication()->seal_control(
                   bare, protected_echo, written),
        Error::none);
    auto damaged = protected_echo;
    damaged[written - 1] ^= std::byte {1};
    runtime->process_packet(
        decode_packet(std::span {damaged}.first(written)).packet, peer);
    REQUIRE_EQ(runtime->standby_rtt_data().samples, 0U);
    runtime->process_packet(
        decode_packet(std::span {protected_echo}.first(written)).packet, peer);
    REQUIRE_EQ(runtime->standby_rtt_data().samples, 1U);
    runtime->process_packet(
        decode_packet(std::span {protected_echo}.first(written)).packet, peer);
    REQUIRE_EQ(runtime->standby_rtt_data().samples, 1U);
    REQUIRE_EQ(runtime->response_health().last_response_microseconds,
        health.last_response_microseconds);
    REQUIRE_EQ(group->send_authority_revision, 0U);
}

namespace {
struct GroupTamper {
    std::shared_ptr<DatagramChannel> channel;
    unsigned mode = 0;
    std::atomic_uint altered {0};
    ~GroupTamper()
    {
        if (channel)
            channel->set_send_hook_for_testing(nullptr, nullptr);
    }
};
UdpIoResult tamper_group_response(
    std::span<const std::byte> bytes, IpEndpoint peer, void* opaque) noexcept
{
    auto& state = *static_cast<GroupTamper*>(opaque);
    const auto handshake = decode_handshake_datagram(bytes);
    std::array<std::byte, 1500> copy {};
    if (handshake
        && handshake.message.packet.request == HandshakeRequest::conclusion
        && handshake.message.has_session_authentication
        && handshake.message.has_group_membership) {
        std::copy(bytes.begin(), bytes.end(), copy.begin());
        if (state.mode == 4) {
            copy[28] = copy[29] = copy[30] = std::byte {0};
            copy[31] = std::byte {128};
            ++state.altered;
            return state.channel->socket.send_to(
                std::span {copy}.first(bytes.size()), peer);
        }
        auto extensions = bytes.subspan(packet_header_size + 48);
        while (!extensions.empty()) {
            const auto ext = decode_extension(extensions);
            if (!ext)
                break;
            const auto target = state.mode < 2 ? HandshakeExtensionType::group
                : state.mode == 2 ? HandshakeExtensionType::path_identifier
                                  : HandshakeExtensionType::standby_rtt;
            if (ext.extension.type == target) {
                const auto offset = ext.extension.content.data() - bytes.data();
                const auto field = state.mode == 0 ? 3
                    : state.mode == 1              ? 7
                    : state.mode == 2              ? 8
                                                   : 24;
                copy[static_cast<std::size_t>(offset) + field] ^= std::byte {1};
                ++state.altered;
                return state.channel->socket.send_to(
                    std::span {copy}.first(bytes.size()), peer);
            }
            extensions = extensions.subspan(ext.bytes_consumed);
        }
    }
    return state.channel->socket.send_to(bytes, peer);
}
}
TEST(acp1_groups_reject_wire_tampering_of_membership_path_and_standby)
{
    for (unsigned mode = 0; mode < 5; ++mode) {
        Sockets sockets;
        const auto listener = sockets.own(srt_create_socket());
        const auto group = sockets.own(srt_create_group(SRT_GTYPE_BACKUP));
        configure(listener);
        configure(group);
        set(listener, SRTO_GROUPCONNECT, 1);
        for (const auto handle : {listener, group}) {
            set(handle, SRTO_ROBOTWEAX_PATHID_REQUIRED, 1);
            set(handle, SRTO_ROBOTWEAX_STANDBYRTT_REQUIRED, 1);
        }
        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        REQUIRE_EQ(srt_bind(listener, reinterpret_cast<sockaddr*>(&address),
                       sizeof(address)),
            0);
        REQUIRE_EQ(srt_listen(listener, 4), 0);
        int size = sizeof(address);
        REQUIRE_EQ(srt_getsockname(
                       listener, reinterpret_cast<sockaddr*>(&address), &size),
            0);
        GroupTamper fault;
        fault.mode = mode;
        const auto record = SocketRegistry::instance().find(listener);
        REQUIRE(record);
        {
            std::lock_guard lock(record->mutex);
            fault.channel = record->channel;
        }
        REQUIRE(fault.channel);
        fault.channel->set_send_hook_for_testing(tamper_group_response, &fault);
        auto* config = srt_create_config();
        REQUIRE(config);
        constexpr char path[] = "wire-path";
        REQUIRE_EQ(srt_config_add(
                       config, SRTO_ROBOTWEAX_PATHID, path, sizeof(path) - 1),
            0);
        auto endpoint = srt_prepare_endpoint(nullptr,
            reinterpret_cast<const sockaddr*>(&address), sizeof(address));
        endpoint.config = config;
        const auto result = srt_connect_group(group, &endpoint, 1);
        srt_delete_config(config);
        REQUIRE_EQ(result, SRT_ERROR);
        REQUIRE(fault.altered > 0);
        REQUIRE(!flag(group, SRTO_ROBOTWEAX_ACP1_ACTIVE));
    }
}
TEST(acp1_groups_listener_callback_cannot_downgrade_required_policy)
{
    Sockets sockets;
    const auto listener = sockets.own(srt_create_socket());
    const auto group = sockets.own(srt_create_group(SRT_GTYPE_BROADCAST));
    configure(listener);
    configure(group, false);
    set(listener, SRTO_GROUPCONNECT, 1);
    REQUIRE_EQ(
        srt_listen_callback(
            listener,
            [](void*, SRTSOCKET child, int, const sockaddr*, const char*) {
                const int disabled = 0;
                const std::uint64_t domain = 1;
                if (srt_setsockflag(child, SRTO_ROBOTWEAX_GROUPDOMAIN, &domain,
                        sizeof(domain))
                    != 0)
                    return -1;
                return srt_setsockflag(child, SRTO_ROBOTWEAX_ACP1_REQUIRED,
                    &disabled, sizeof(disabled));
            },
            nullptr),
        0);
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE_EQ(srt_bind(listener, reinterpret_cast<sockaddr*>(&address),
                   sizeof(address)),
        0);
    REQUIRE_EQ(srt_listen(listener, 4), 0);
    int size = sizeof(address);
    REQUIRE_EQ(
        srt_getsockname(listener, reinterpret_cast<sockaddr*>(&address), &size),
        0);
    auto endpoint = srt_prepare_endpoint(
        nullptr, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    REQUIRE_EQ(srt_connect_group(group, &endpoint, 1), SRT_ERROR);
    REQUIRE(!flag(group, SRTO_ROBOTWEAX_ACP1_ACTIVE));
}

TEST(acp1_groups_allow_members_without_optional_path_or_standby_profiles)
{
    for (const auto type : {SRT_GTYPE_BROADCAST, SRT_GTYPE_BACKUP}) {
        Sockets sockets;
        const auto listener = sockets.own(srt_create_socket());
        const auto group = sockets.own(srt_create_group(type));
        configure(listener);
        configure(group);
        set(listener, SRTO_GROUPCONNECT, 1);
        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        REQUIRE_EQ(srt_bind(listener, reinterpret_cast<sockaddr*>(&address),
                       sizeof(address)),
            0);
        REQUIRE_EQ(srt_listen(listener, 4), 0);
        int size = sizeof(address);
        REQUIRE_EQ(srt_getsockname(
                       listener, reinterpret_cast<sockaddr*>(&address), &size),
            0);
        auto endpoint = srt_prepare_endpoint(nullptr,
            reinterpret_cast<const sockaddr*>(&address), sizeof(address));
        REQUIRE(srt_connect_group(group, &endpoint, 1) != SRT_ERROR);
        (void)runtime(endpoint.id);
        const auto mirror = sockets.own(srt_accept(listener, nullptr, nullptr));
        REQUIRE(flag(group, SRTO_ROBOTWEAX_ACP1_ACTIVE));
        REQUIRE(flag(mirror, SRTO_ROBOTWEAX_ACP1_ACTIVE));
        transfer(group, mirror, 1, 10);
        transfer(mirror, group, 11, 10);
        REQUIRE_EQ(srt_close(endpoint.id), 0);
        REQUIRE(!flag(group, SRTO_ROBOTWEAX_ACP1_ACTIVE));
    }
}
TEST(acp1_groups_bonded_mirror_rejects_different_required_policies)
{
    Sockets sockets;
    const auto protected_listener = sockets.own(srt_create_socket());
    const auto legacy_listener = sockets.own(srt_create_socket());
    configure(protected_listener);
    configure(legacy_listener, false);
    for (const auto handle : {protected_listener, legacy_listener}) {
        const auto record = SocketRegistry::instance().find(handle);
        REQUIRE(record);
        std::lock_guard lock(record->mutex);
        record->accept_bond_scope = 123;
    }
    GroupRegistry::MirrorDescription description;
    REQUIRE(GroupRegistry::instance().prepare_mirror(
        protected_listener, 0x40000001, SRT_GTYPE_BACKUP, 900, description));
    sockets.own(description.group);
    GroupRegistry::MirrorDescription different;
    REQUIRE(!GroupRegistry::instance().prepare_mirror(
        legacy_listener, 0x40000001, SRT_GTYPE_BACKUP, 900, different));
}

TEST(acp1_group_rendezvous_paired_groups_preserve_directional_sequences)
{
    for (const auto type : {SRT_GTYPE_BROADCAST, SRT_GTYPE_BACKUP}) {
        Sockets sockets;
        const auto left = sockets.own(srt_create_group(type));
        const auto right = sockets.own(srt_create_group(type));
        for (const auto group : {left, right}) {
            configure(group);
            set(group, SRTO_RENDEZVOUS, 1);
            set(group, SRTO_RCVSYN, 0);
        }
        const auto seed = [](SRTSOCKET group, std::uint32_t seq) {
            const auto record = GroupRegistry::instance().find(group);
            REQUIRE(record);
            std::lock_guard lock(record->mutex);
            record->initial_sequence = record->next_send_sequence =
                record->replay_acknowledged_sequence = seq;
            record->next_receive_sequence = seq;
        };
        seed(left, 500);
        seed(right, 900);
        std::array<sockaddr_in, 4> addresses {};
        for (auto& address : addresses) {
            const auto socket = sockets.own(srt_create_socket());
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            REQUIRE_EQ(srt_bind(socket, reinterpret_cast<sockaddr*>(&address),
                           sizeof(address)),
                0);
            int size = sizeof(address);
            REQUIRE_EQ(srt_getsockname(socket,
                           reinterpret_cast<sockaddr*>(&address), &size),
                0);
            REQUIRE_EQ(srt_close(socket), 0);
        }
        std::array<SRT_SOCKGROUPCONFIG, 2> a {}, b {};
        for (unsigned i = 0; i < 2; ++i) {
            a[i] = srt_prepare_endpoint(
                reinterpret_cast<sockaddr*>(&addresses[2 * i]),
                reinterpret_cast<sockaddr*>(&addresses[2 * i + 1]),
                sizeof(sockaddr_in));
            b[i] = srt_prepare_endpoint(
                reinterpret_cast<sockaddr*>(&addresses[2 * i + 1]),
                reinterpret_cast<sockaddr*>(&addresses[2 * i]),
                sizeof(sockaddr_in));
        }
        REQUIRE(srt_connect_group(left, a.data(), a.size()) != SRT_ERROR);
        REQUIRE(srt_connect_group(right, b.data(), b.size()) != SRT_ERROR);
        for (auto& endpoint : a) {
            (void)runtime(endpoint.id);
            REQUIRE(flag(endpoint.id, SRTO_ROBOTWEAX_ACP1_ACTIVE));
        }
        for (auto& endpoint : b) {
            (void)runtime(endpoint.id);
            REQUIRE(flag(endpoint.id, SRTO_ROBOTWEAX_ACP1_ACTIVE));
        }
        REQUIRE(flag(left, SRTO_ROBOTWEAX_ACP1_ACTIVE));
        REQUIRE(flag(right, SRTO_ROBOTWEAX_ACP1_ACTIVE));
        set(left, SRTO_RCVSYN, 1);
        set(right, SRTO_RCVSYN, 1);
        transfer(left, right, 1, 16);
        transfer(right, left, 21, 16);
    }
}
#endif
