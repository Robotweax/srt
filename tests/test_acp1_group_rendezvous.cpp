// SPDX-License-Identifier: MIT
#include "test.hpp"
#include "compat/acp1_group_context.hpp"
#include "compat/group_registry.hpp"
#include "compat/socket_registry.hpp"
#include "compat/transport_runtime.hpp"
#include "robotweax/srt/handshake_datagram.hpp"
#include "robotweax/srt/rendezvous.hpp"
#include "srt/srt.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>
using namespace robotweax::srt;
using namespace robotweax::srt::compat;
#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_ACP1
namespace {
void set(SRTSOCKET s, SRT_SOCKOPT option, int value)
{
    REQUIRE_EQ(srt_setsockflag(s, option, &value, sizeof(value)), 0);
}
bool active(SRTSOCKET s)
{
    bool value = false;
    int size = sizeof(value);
    REQUIRE_EQ(
        srt_getsockflag(s, SRTO_ROBOTWEAX_ACP1_ACTIVE, &value, &size), 0);
    return value;
}
std::shared_ptr<ConnectionRuntime> runtime(SRTSOCKET s)
{
    const auto record = SocketRegistry::instance().find(s);
    REQUIRE(record);
    std::lock_guard lock(record->mutex);
    return record->runtime;
}
void wait_connected(SRTSOCKET s)
{
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds {3};
    while (std::chrono::steady_clock::now() < deadline) {
        if (srt_getsockstate(s) == SRTS_CONNECTED && active(s))
            return;
        std::this_thread::sleep_for(std::chrono::milliseconds {1});
    }
    REQUIRE_EQ(srt_getsockstate(s), SRTS_CONNECTED);
    REQUIRE(active(s));
}
struct Address {
    sockaddr_storage storage {};
    int size = 0;
};
struct Pair {
    std::array<SRT_SOCKGROUPCONFIG, 2> endpoints {};
};
struct Groups {
    std::vector<SRTSOCKET> handles;
    std::array<SRTSOCKET, 2> groups {};
    bool ipv6 = false, paths = false, sync = false;
    Groups(SRT_GROUP_TYPE type, bool gcm = false, bool v6 = false,
        bool labels = false, bool standby = false, bool blocking = false)
        : ipv6(v6)
        , paths(labels)
        , sync(blocking)
    {
        REQUIRE_EQ(srt_startup(), 0);
        for (unsigned i = 0; i < 2; ++i) {
            groups[i] = own(srt_create_group(type));
            configure(groups[i], gcm);
            if (paths)
                set(groups[i], SRTO_ROBOTWEAX_PATHID_REQUIRED, 1);
            if (standby)
                set(groups[i], SRTO_ROBOTWEAX_STANDBYRTT_REQUIRED, 1);
            set(groups[i], SRTO_RENDEZVOUS, 1);
            set(groups[i], SRTO_RCVSYN, sync ? 1 : 0);
            const auto record = GroupRegistry::instance().find(groups[i]);
            REQUIRE(record);
            std::lock_guard lock(record->mutex);
            const auto initial = i == 0 ? 500U : 900U;
            record->initial_sequence = record->next_send_sequence =
                record->replay_acknowledged_sequence = initial;
            record->next_receive_sequence = initial;
            record->timestamp_origin =
                ConnectionRuntime::Clock::now() - std::chrono::seconds {10};
        }
    }
    ~Groups()
    {
        for (auto s : handles)
            (void)srt_close(s);
        (void)srt_cleanup();
    }
    SRTSOCKET own(SRTSOCKET s)
    {
        REQUIRE(s != SRT_INVALID_SOCK);
        handles.push_back(s);
        return s;
    }
    void configure(SRTSOCKET s, bool gcm)
    {
        set(s, SRTO_ROBOTWEAX_ACP1_REQUIRED, 1);
        set(s, SRTO_ROBOTWEAX_SESSIONAUTH, 1);
        constexpr char secret[] = "acp1-group-rendezvous-password";
        REQUIRE_EQ(
            srt_setsockflag(s, SRTO_PASSPHRASE, secret, sizeof(secret) - 1), 0);
        set(s, SRTO_CONNTIMEO, 2000);
        set(s, SRTO_RCVTIMEO, 2000);
        set(s, SRTO_SNDTIMEO, 2000);
        set(s, SRTO_LATENCY, 20);
        set(s, SRTO_KMREFRESHRATE, 8);
        set(s, SRTO_KMPREANNOUNCE, 1);
        if (ipv6)
            set(s, SRTO_IPV6ONLY, 1);
#ifdef ENABLE_AEAD_API_PREVIEW
        if (gcm)
            set(s, SRTO_CRYPTOMODE, 2);
#else
        (void)gcm;
#endif
    }
    Address address()
    {
        const auto socket = own(srt_create_socket());
        Address address;
        if (ipv6) {
            set(socket, SRTO_IPV6ONLY, 1);
            sockaddr_in6 local {};
            local.sin6_family = AF_INET6;
            local.sin6_addr = in6addr_loopback;
            REQUIRE_EQ(srt_bind(socket, reinterpret_cast<sockaddr*>(&local),
                           sizeof(local)),
                0);
        } else {
            sockaddr_in local {};
            local.sin_family = AF_INET;
            local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            REQUIRE_EQ(srt_bind(socket, reinterpret_cast<sockaddr*>(&local),
                           sizeof(local)),
                0);
        }
        address.size = sizeof(address.storage);
        REQUIRE_EQ(
            srt_getsockname(socket,
                reinterpret_cast<sockaddr*>(&address.storage), &address.size),
            0);
        REQUIRE_EQ(srt_close(socket), 0);
        return address;
    }
    Pair prepare(std::string label = "path-a")
    {
        const std::array addresses {address(), address()};
        Pair pair;
        for (unsigned i = 0; i < 2; ++i) {
            pair.endpoints[i] = srt_prepare_endpoint(
                reinterpret_cast<const sockaddr*>(&addresses[i].storage),
                reinterpret_cast<const sockaddr*>(&addresses[1 - i].storage),
                addresses[i].size);
            pair.endpoints[i].weight =
                static_cast<std::uint16_t>(i == 0 ? 7 : 9);
            if (paths) {
                auto* config = srt_create_config();
                REQUIRE(config);
                REQUIRE_EQ(srt_config_add(config, SRTO_ROBOTWEAX_PATHID,
                               label.data(), label.size()),
                    0);
                pair.endpoints[i].config = config;
            }
        }
        return pair;
    }
    void connect_side(Pair& pair, unsigned side)
    {
        const auto result =
            srt_connect_group(groups[side], &pair.endpoints[side], 1);
        if (pair.endpoints[side].config) {
            srt_delete_config(pair.endpoints[side].config);
            pair.endpoints[side].config = nullptr;
        }
        REQUIRE(result != SRT_ERROR);
    }
    Pair join(std::string label = "path-a")
    {
        auto pair = prepare(label);
        if (sync) {
            std::array<int, 2> results {};
            std::thread left([&] {
                results[0] =
                    srt_connect_group(groups[0], &pair.endpoints[0], 1);
            });
            std::thread right([&] {
                results[1] =
                    srt_connect_group(groups[1], &pair.endpoints[1], 1);
            });
            left.join();
            right.join();
            for (unsigned i = 0; i < 2; ++i) {
                if (pair.endpoints[i].config) {
                    srt_delete_config(pair.endpoints[i].config);
                    pair.endpoints[i].config = nullptr;
                }
                REQUIRE(results[i] != SRT_ERROR);
            }
        } else {
            connect_side(pair, 0);
            connect_side(pair, 1);
        }
        for (auto& endpoint : pair.endpoints)
            wait_connected(endpoint.id);
        for (auto group : groups) {
            set(group, SRTO_RCVSYN, 1);
            REQUIRE(active(group));
        }
        return pair;
    }
};
void transfer(SRTSOCKET sender, SRTSOCKET receiver, unsigned first = 1,
    unsigned count = 12)
{
    for (unsigned n = first; n < first + count; ++n) {
        std::array<char, 256> sent {};
        sent.fill(static_cast<char>(n));
        REQUIRE_EQ(srt_sendmsg(sender, sent.data(), sent.size(), -1, 1),
            static_cast<int>(sent.size()));
        std::array<char, 1500> received {};
        REQUIRE_EQ(srt_recvmsg(receiver, received.data(), received.size()),
            static_cast<int>(sent.size()));
        REQUIRE(std::equal(sent.begin(), sent.end(), received.begin()));
    }
}
}
TEST(acp1_group_rendezvous_ctr_gcm_ipv4_ipv6_sync_async)
{
    for (const auto type : {SRT_GTYPE_BROADCAST, SRT_GTYPE_BACKUP})
        for (const bool gcm : {false, true})
            for (const bool ipv6 : {false, true})
                for (const bool synchronous : {false, true}) {
#ifndef ENABLE_AEAD_API_PREVIEW
                    if (gcm)
                        continue;
#endif
                    Groups peers(type, gcm, ipv6, true,
                        type == SRT_GTYPE_BACKUP, synchronous);
                    const auto pair = peers.join();
                    for (unsigned i = 0; i < 2; ++i) {
                        const auto record =
                            GroupRegistry::instance().find(peers.groups[i]);
                        REQUIRE(record);
                        std::int64_t origin = 0;
                        {
                            std::lock_guard lock(record->mutex);
                            REQUIRE_EQ(record->peer_group, peers.groups[1 - i]);
                            origin = std::chrono::duration_cast<
                                std::chrono::microseconds>(
                                record->timestamp_origin->time_since_epoch())
                                         .count();
                        }
                        REQUIRE_EQ(runtime(pair.endpoints[i].id)
                                       ->timestamp_origin_microseconds(),
                            origin);
                        REQUIRE_EQ(SocketRegistry::instance()
                                       .find(pair.endpoints[i].id)
                                       ->crypto->session_authentication()
                                       ->parameters()
                                       .version,
                            6U);
                        if (type == SRT_GTYPE_BACKUP)
                            REQUIRE(runtime(pair.endpoints[i].id)
                                        ->standby_rtt_data()
                                        .flags
                                & ROBOTWEAX_SRT_STANDBY_RTT_NEGOTIATED);
                    }
                    transfer(peers.groups[0], peers.groups[1]);
                    transfer(peers.groups[1], peers.groups[0], 31);
                }
}
TEST(acp1_group_rendezvous_late_join_replacement_rotation_and_origin)
{
    for (const auto type : {SRT_GTYPE_BROADCAST, SRT_GTYPE_BACKUP})
        for (const bool gcm : {false, true}) {
#ifndef ENABLE_AEAD_API_PREVIEW
            if (gcm)
                continue;
#endif
            Groups peers(type, gcm, false, true, type == SRT_GTYPE_BACKUP);
            const auto first = peers.join("path-a");
            transfer(peers.groups[0], peers.groups[1]);
            transfer(peers.groups[1], peers.groups[0], 31);
            const auto second = peers.join("path-b");
            for (unsigned i = 0; i < 2; ++i)
                REQUIRE_EQ(runtime(first.endpoints[i].id)
                               ->timestamp_origin_microseconds(),
                    runtime(second.endpoints[i].id)
                        ->timestamp_origin_microseconds());
            transfer(peers.groups[0], peers.groups[1], 61);
            transfer(peers.groups[1], peers.groups[0], 91);
            for (const auto& endpoint : first.endpoints)
                REQUIRE_EQ(srt_close(endpoint.id), 0);
            for (auto group : peers.groups)
                REQUIRE(active(group));
            transfer(peers.groups[0], peers.groups[1], 121);
            transfer(peers.groups[1], peers.groups[0], 151);
            const auto replacement = peers.join("path-a");
            for (unsigned i = 0; i < 2; ++i)
                REQUIRE(replacement.endpoints[i].id != first.endpoints[i].id);
            transfer(peers.groups[0], peers.groups[1], 181);
            transfer(peers.groups[1], peers.groups[0], 211);
        }
}

namespace {
HandshakeMessage message(const HandshakeActions& actions)
{
    for (std::size_t i = 0; i < actions.size; ++i)
        if (actions.values[i].kind == HandshakeActionKind::send) {
            std::array<std::byte, 1500> bytes {};
            const auto encoded = encode_handshake_datagram(
                actions.values[i], PacketTimestamp {7}, 0, bytes);
            REQUIRE(encoded);
            const auto decoded = decode_handshake_datagram(
                std::span {bytes}.first(encoded.bytes_written));
            REQUIRE(decoded);
            return decoded.message;
        }
    throw std::runtime_error("missing handshake send");
}
bool connected(const HandshakeActions& actions)
{
    for (std::size_t i = 0; i < actions.size; ++i)
        if (actions.values[i].kind == HandshakeActionKind::connected)
            return true;
    return false;
}
RendezvousHandshakeMachine::Configuration config(unsigned side, bool initiator)
{
    RendezvousHandshakeMachine::Configuration c;
    c.local_socket_id = 101 + side;
    c.local_cookie = initiator ? 200 : 100;
    c.initial_sequence = SequenceNumber {side == 0 ? 500U : 900U};
    c.require_session_authentication = true;
    c.has_group_membership = true;
    c.group_membership = {.group_id = 0x40000001U + side,
        .type = GroupType::backup,
        .weight = static_cast<std::uint16_t>(side == 0 ? 7 : 9)};
    c.require_path_identifier = true;
    c.path_identifier.size = 1;
    c.path_identifier.bytes[0] = std::byte {'p'};
    c.require_standby_rtt = true;
    c.standby_rtt_nonce.fill(std::byte {static_cast<unsigned char>(side + 1)});
    return c;
}
struct Exchange {
    bool flipped = false;
    RendezvousHandshakeMachine::Configuration a, b;
    RendezvousHandshakeMachine left, right;
    SessionAuthentication initiator {default_crypto_provider()},
        responder {default_crypto_provider()};
    HandshakeMessage request, response;
    KeyMaterialBuffer material;
    Exchange(bool flip = false)
        : flipped(flip)
        , a(config(0, !flip))
        , b(config(1, flip))
        , left(a)
        , right(b)
    {
        constexpr std::string_view secret = "acp1-group-rendezvous-machine";
        const auto pass =
            std::as_bytes(std::span {secret.data(), secret.size()});
        CryptoSession keys({.passphrase = secret});
        REQUIRE_EQ(keys.start_initiator(), Error::none);
        const auto km = keys.pending_key_material();
        std::copy(km.begin(), km.end(), material.bytes.begin());
        material.size = km.size();
        const auto wave_a = message(left.start()),
                   wave_b = message(right.start());
        REQUIRE(!wave_a.has_group_membership && !wave_b.has_group_membership);
        auto& i = flip ? right : left;
        auto& r = flip ? left : right;
        const auto& ic = flip ? b : a;
        const auto& rc = flip ? a : b;
        REQUIRE_EQ(initiator.start(true, 6), Error::none);
        REQUIRE_EQ(responder.start(false, 6), Error::none);
        REQUIRE_EQ(i.set_key_material_request(
                       material, encryption_field_for_key_length(16)),
            Error::none);
        REQUIRE_EQ(
            i.set_session_authentication(initiator.parameters()), Error::none);
        request = message(i.receive(flip ? wave_a : wave_b));
        (void)message(r.receive(flip ? wave_b : wave_a));
        const StandbyRttIdentity standby {
            .caller = ic.standby_rtt_nonce, .listener = rc.standby_rtt_nonce};
        const auto context = acp1_group_rendezvous_context(ic.group_membership,
            rc.group_membership, ic.path_identifier, true, standby,
            ic.initial_sequence, rc.initial_sequence);
        REQUIRE_EQ(
            responder.establish(pass, request.session_authentication,
                rc.local_socket_id, ic.local_socket_id, 100, 200, context),
            Error::none);
        SessionAuthenticationParameters server, confirmation;
        REQUIRE_EQ(responder.handshake_proof(false, material.view(), server),
            Error::none);
        REQUIRE_EQ(
            responder.handshake_proof(true, material.view(), confirmation),
            Error::none);
        REQUIRE_EQ(
            r.set_session_authentication(server, confirmation), Error::none);
        response = message(r.receive(request));
        REQUIRE_EQ(
            initiator.establish(pass, response.session_authentication,
                ic.local_socket_id, rc.local_socket_id, 200, 100, context),
            Error::none);
        REQUIRE(initiator.verify_handshake(
            false, material.view(), response.session_authentication));
        SessionAuthenticationParameters client;
        REQUIRE_EQ(initiator.handshake_proof(true, material.view(), client),
            Error::none);
        REQUIRE_EQ(i.set_session_authentication(
                       client, response.session_authentication),
            Error::none);
    }
    RendezvousHandshakeMachine& i()
    {
        return flipped ? right : left;
    }
    RendezvousHandshakeMachine& r()
    {
        return flipped ? left : right;
    }
    HandshakeMessage agreement()
    {
        const auto actions = i().receive(response);
        REQUIRE(connected(actions));
        return message(actions);
    }
};
}
TEST(acp1_group_rendezvous_machine_mixed_roles_final_proof_and_retry)
{
    for (bool flip : {false, true}) {
        Exchange exchange(flip);
        const auto retry = message(exchange.i().timeout());
        REQUIRE(retry.session_authentication.is_offer());
        REQUIRE_EQ(retry.session_authentication.version, 6U);
        REQUIRE(retry.standby_rtt.is_offer());
        REQUIRE_EQ(retry.group_membership.group_id,
            exchange.request.group_membership.group_id);
        (void)exchange.agreement();
        REQUIRE(!connected(exchange.r().note_connected_packet()));
        REQUIRE(exchange.r().state() != RendezvousState::connected);
        const auto response = message(exchange.r().timeout());
        REQUIRE(response.session_authentication.matches(
            exchange.response.session_authentication));
        const auto agreement = message(exchange.i().receive(response));
        REQUIRE(connected(exchange.r().receive(agreement)));
        REQUIRE_EQ(exchange.left.peer_group_membership().group_id,
            exchange.b.group_membership.group_id);
        REQUIRE_EQ(exchange.right.peer_group_membership().group_id,
            exchange.a.group_membership.group_id);
        REQUIRE_EQ(
            exchange.left.peer_initial_sequence(), exchange.b.initial_sequence);
        REQUIRE_EQ(exchange.right.peer_initial_sequence(),
            exchange.a.initial_sequence);
        REQUIRE(!connected(exchange.r().receive(agreement)));
    }
}
TEST(acp1_group_rendezvous_machine_rejects_final_metadata_and_proof_tampering)
{
    for (unsigned field = 0; field < 11; ++field) {
        Exchange exchange;
        auto agreement = exchange.agreement();
        switch (field) {
        case 0:
            agreement.has_group_membership = false;
            break;
        case 1:
            ++agreement.group_membership.group_id;
            break;
        case 2:
            agreement.group_membership.type = GroupType::broadcast;
            break;
        case 3:
            ++agreement.group_membership.weight;
            break;
        case 4:
            agreement.path_identifier.bytes[0] ^= std::byte {1};
            break;
        case 5:
            agreement.standby_rtt.caller[0] ^= std::byte {1};
            break;
        case 6:
            agreement.standby_rtt.listener[0] ^= std::byte {1};
            break;
        case 7:
            agreement.packet.initial_sequence =
                agreement.packet.initial_sequence.advanced(1);
            break;
        case 8:
            ++agreement.packet.syn_cookie;
            break;
        case 9:
            agreement.session_authentication.version = 4;
            break;
        case 10:
            agreement.session_authentication.proof[0] ^= std::byte {1};
            break;
        }
        REQUIRE(!connected(exchange.r().receive(agreement)));
        REQUIRE(exchange.r().state() != RendezvousState::connected);
    }
}

namespace {
struct FaultState {
    std::atomic_bool flow {false}, final {false}, probes {false};
    std::atomic_int initiator {-1};
    std::atomic_uint changed {0};
    int tamper = -1;
};
struct Fault {
    std::shared_ptr<DatagramChannel> channel;
    std::shared_ptr<SocketRecord> record;
    std::shared_ptr<FaultState> state;
    unsigned index = 0;
    ~Fault()
    {
        if (channel)
            channel->set_send_hook_for_testing(nullptr, nullptr);
    }
};
UdpIoResult fault_send(
    std::span<const std::byte> bytes, IpEndpoint peer, void* context) noexcept
{
    auto& fault = *static_cast<Fault*>(context);
    const auto decoded = decode_handshake_datagram(bytes);
    if (decoded && !fault.state->flow.load())
        return {.bytes_transferred = bytes.size()};
    if (decoded
        && decoded.message.packet.request == HandshakeRequest::conclusion
        && decoded.message.has_handshake_extension
        && decoded.message.extension_type
            == HandshakeExtensionType::handshake_response
        && decoded.message.has_group_membership && fault.state->tamper >= 0) {
        std::array<std::byte, 1500> copy {};
        std::copy(bytes.begin(), bytes.end(), copy.begin());
        const auto mode = fault.state->tamper;
        if (mode == 4) {
            copy[27] ^= std::byte {1};
        } else if (mode == 5) {
            copy[47] ^= std::byte {1};
        } else if (mode == 7) {
            copy[28] = copy[29] = copy[30] = std::byte {0};
            copy[31] = std::byte {128};
        } else {
            auto extensions =
                bytes.subspan(packet_header_size + handshake_size);
            const auto target = mode < 2 ? HandshakeExtensionType::group
                : mode == 2 ? HandshakeExtensionType::path_identifier
                : mode == 3 ? HandshakeExtensionType::standby_rtt
                            : HandshakeExtensionType::session_authentication;
            while (!extensions.empty()) {
                const auto ext = decode_extension(extensions);
                if (!ext)
                    break;
                if (ext.extension.type == target) {
                    const auto offset =
                        ext.extension.content.data() - bytes.data();
                    const auto field = mode == 0 ? 3
                        : mode == 1              ? 7
                        : mode == 2              ? 8
                        : mode == 3              ? 24
                                                 : 3;
                    copy[static_cast<std::size_t>(offset) + field] ^=
                        static_cast<std::byte>(mode == 6 ? 2U : 1U);
                    break;
                }
                extensions = extensions.subspan(ext.bytes_consumed);
            }
        }
        ++fault.state->changed;
        return fault.channel->socket.send_to(
            std::span {copy}.first(bytes.size()), peer);
    }
    if (decoded && decoded.message.packet.request == HandshakeRequest::agreement
        && decoded.message.has_group_membership && !fault.state->final.load()) {
        int unresolved = -1;
        fault.state->initiator.compare_exchange_strong(
            unresolved, static_cast<int>(fault.index));
        if (fault.state->initiator.load() == static_cast<int>(fault.index)) {
            if (fault.state->changed.fetch_add(1) == 0) {
                std::array<std::byte, 1500> clear {}, signed_control {},
                    data {};
                const auto encoded = encode_packet(
                    {.kind = PacketKind::control,
                        .control = {.type = ControlType::keepalive,
                            .destination_socket_id =
                                decoded.control.destination_socket_id}},
                    clear);
                if (!encoded)
                    return {.error = Error::io_error};
                const auto wire =
                    std::span {clear}.first(encoded.bytes_written);
                if (!fault.channel->socket.send_to(wire, peer))
                    return {.error = Error::io_error};
                std::size_t written = 0;
                // The setup owner holds this session until runtime promotion.
                if (fault.record->crypto->session_authentication()
                            ->seal_control(wire, signed_control, written)
                        != Error::none
                    || !fault.channel->socket.send_to(
                        std::span {signed_control}.first(written), peer))
                    return {.error = Error::io_error};
                const std::array<std::byte, 4> payload {};
                const auto packet = encode_packet(
                    {.kind = PacketKind::data,
                        .data = {.sequence =
                                     decoded.message.packet.initial_sequence,
                            .message_number = 1,
                            .boundary = MessageBoundary::solo,
                            .destination_socket_id =
                                decoded.control.destination_socket_id},
                        .payload = payload},
                    data);
                if (!packet
                    || !fault.channel->socket.send_to(
                        std::span {data}.first(packet.bytes_written), peer))
                    return {.error = Error::io_error};
                fault.state->probes = true;
            }
            return {.bytes_transferred = bytes.size()};
        }
    }
    return fault.channel->socket.send_to(bytes, peer);
}
void hook(Fault& fault, SRTSOCKET member, std::shared_ptr<FaultState> state,
    unsigned index)
{
    fault.state = std::move(state);
    fault.index = index;
    fault.record = SocketRegistry::instance().find(member);
    REQUIRE(fault.record);
    {
        std::lock_guard lock(fault.record->mutex);
        fault.channel = fault.record->channel;
    }
    REQUIRE(fault.channel);
    fault.channel->set_send_hook_for_testing(fault_send, &fault);
}
void wait_failed(SRTSOCKET member)
{
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds {2};
    while (srt_getsockstate(member) == SRTS_CONNECTING
        && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds {1});
    REQUIRE(srt_getsockstate(member) != SRTS_CONNECTED);
    REQUIRE(!active(member));
}
}
TEST(
    acp1_group_rendezvous_public_final_proof_blocks_bare_signed_controls_and_data)
{
    Groups peers(SRT_GTYPE_BACKUP, false, false, true, true);
    auto pair = peers.prepare();
    std::array<Fault, 2> faults;
    const auto state = std::make_shared<FaultState>();
    peers.connect_side(pair, 0);
    hook(faults[0], pair.endpoints[0].id, state, 0);
    peers.connect_side(pair, 1);
    hook(faults[1], pair.endpoints[1].id, state, 1);
    state->flow = true;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds {3};
    while (!state->probes.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds {1});
    REQUIRE(state->probes.load());
    REQUIRE(state->initiator.load() >= 0);
    const auto responder = 1U - static_cast<unsigned>(state->initiator.load());
    std::this_thread::sleep_for(std::chrono::milliseconds {50});
    REQUIRE_EQ(srt_getsockstate(pair.endpoints[responder].id), SRTS_CONNECTING);
    REQUIRE(!active(peers.groups[responder]));
    REQUIRE(!active(pair.endpoints[responder].id));
    state->final = true;
    for (const auto& endpoint : pair.endpoints)
        wait_connected(endpoint.id);
    for (auto group : peers.groups)
        set(group, SRTO_RCVSYN, 1);
    transfer(peers.groups[0], peers.groups[1], 1, 4);
    transfer(peers.groups[1], peers.groups[0], 11, 4);
}
TEST(acp1_group_rendezvous_public_rejects_wire_metadata_and_bounds_tampering)
{
    for (int mode = 0; mode < 8; ++mode) {
        Groups peers(SRT_GTYPE_BACKUP, false, false, true, true);
        for (auto group : peers.groups)
            set(group, SRTO_CONNTIMEO, 50);
        auto pair = peers.prepare();
        std::array<Fault, 2> faults;
        const auto state = std::make_shared<FaultState>();
        state->tamper = mode;
        state->final = true;
        peers.connect_side(pair, 0);
        hook(faults[0], pair.endpoints[0].id, state, 0);
        peers.connect_side(pair, 1);
        hook(faults[1], pair.endpoints[1].id, state, 1);
        state->flow = true;
        for (const auto& endpoint : pair.endpoints)
            wait_failed(endpoint.id);
        REQUIRE(state->changed.load() > 0);
        for (auto group : peers.groups)
            REQUIRE(!active(group));
    }
}
TEST(acp1_group_rendezvous_public_rejects_wrong_secret_and_different_peer_group)
{
    {
        Groups peers(SRT_GTYPE_BROADCAST);
        constexpr char bad[] = "different-group-rendezvous-password";
        REQUIRE_EQ(srt_setsockflag(
                       peers.groups[1], SRTO_PASSPHRASE, bad, sizeof(bad) - 1),
            0);
        for (auto group : peers.groups)
            set(group, SRTO_CONNTIMEO, 50);
        auto pair = peers.prepare();
        peers.connect_side(pair, 0);
        peers.connect_side(pair, 1);
        for (const auto& endpoint : pair.endpoints)
            wait_failed(endpoint.id);
        for (auto group : peers.groups)
            REQUIRE(!active(group));
    }
    Groups peers(SRT_GTYPE_BROADCAST);
    (void)peers.join();
    const auto third = peers.own(srt_create_group(SRT_GTYPE_BROADCAST));
    peers.configure(third, false);
    set(third, SRTO_RENDEZVOUS, 1);
    set(third, SRTO_RCVSYN, 0);
    set(third, SRTO_CONNTIMEO, 50);
    set(peers.groups[0], SRTO_RCVSYN, 0);
    auto pair = peers.prepare();
    const auto old_right = peers.groups[1];
    peers.groups[1] = third;
    peers.connect_side(pair, 0);
    peers.connect_side(pair, 1);
    peers.groups[1] = old_right;
    for (const auto& endpoint : pair.endpoints)
        wait_failed(endpoint.id);
    REQUIRE(active(peers.groups[0]));
    REQUIRE(active(old_right));
    REQUIRE(!active(third));
    set(peers.groups[0], SRTO_RCVSYN, 1);
    transfer(peers.groups[0], old_right, 1, 4);
    transfer(old_right, peers.groups[0], 11, 4);
}
#endif
