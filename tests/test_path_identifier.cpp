#include "test.hpp"
#include "compat/group_registry.hpp"
#include "robotweax/srt/handshake_datagram.hpp"
#include "srt/srt.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <thread>

using namespace robotweax::srt;

namespace {
PathIdentifier label(std::uint32_t size = 6)
{
    PathIdentifier result;
    result.size = size;
    for (std::uint32_t i = 0; i < size; ++i)
        result.bytes[i] = static_cast<std::byte>(i); // includes binary NUL
    return result;
}
std::uint32_t cookie(const Handshake&, void*) noexcept
{
    return 123U;
}
bool mirror(const GroupMembership& peer, GroupMembership& local, void*) noexcept
{
    local = peer;
    local.group_id = SRTGROUP_MASK | 2U;
    return true;
}
HandshakeMessage message(const HandshakeAction& action)
{
    std::array<std::byte, 1500> bytes {};
    const auto encoded =
        encode_handshake_datagram(action, PacketTimestamp {}, 0, bytes);
    REQUIRE(encoded);
    const auto decoded = decode_handshake_datagram(
        std::span {bytes}.first(encoded.bytes_written));
    REQUIRE(decoded);
    return decoded.message;
}
struct ProtocolPair {
    HandshakeMachine caller;
    HandshakeMachine listener;
    ProtocolPair(bool caller_required = true, bool listener_required = true,
        bool authenticated = false)
        : caller(caller_config(caller_required, authenticated))
        , listener(listener_config(listener_required, authenticated))
    {
    }
    static HandshakeMachine::Configuration caller_config(
        bool required, bool authenticated)
    {
        HandshakeMachine::Configuration c;
        c.local_socket_id = 10;
        c.has_group_membership = true;
        c.group_membership = {
            .group_id = SRTGROUP_MASK | 1U, .type = GroupType::broadcast};
        c.require_path_identifier = required;
        c.path_identifier = required ? label() : PathIdentifier {};
        c.require_session_authentication = authenticated;
        c.session_authentication.caller_nonce[0] = std::byte {1};
        return c;
    }
    static HandshakeMachine::Configuration listener_config(
        bool required, bool authenticated)
    {
        auto c = caller_config(false, false);
        c.role = ConnectionRole::listener;
        c.local_socket_id = 20;
        c.has_group_membership = false;
        c.group_membership_negotiator = mirror;
        c.cookie_generator = cookie;
        c.require_path_identifier = required;
        c.require_session_authentication = authenticated;
        c.session_authentication.caller_nonce[0] = std::byte {1};
        c.session_authentication.listener_nonce[0] = std::byte {2};
        c.session_authentication.proof[0] = std::byte {3};
        c.session_authentication_confirmation = c.session_authentication;
        c.session_authentication_confirmation.proof[0] = std::byte {4};
        return c;
    }
    HandshakeMessage offer()
    {
        const auto start = caller.start();
        REQUIRE_EQ(start.size, 2U);
        REQUIRE(!start.values[0].has_path_identifier);
        const auto induction = listener.receive(message(start.values[0]));
        REQUIRE_EQ(induction.size, 2U);
        REQUIRE(!induction.values[0].has_path_identifier);
        const auto conclusion = caller.receive(message(induction.values[0]));
        REQUIRE_EQ(conclusion.size, 2U);
        return message(conclusion.values[0]);
    }
};
struct Sockets {
    SRTSOCKET listener = SRT_INVALID_SOCK;
    SRTSOCKET caller = SRT_INVALID_SOCK;
    SRTSOCKET accepted = SRT_INVALID_SOCK;
    ~Sockets()
    {
        if (caller != SRT_INVALID_SOCK)
            (void)srt_close(caller);
        if (accepted != SRT_INVALID_SOCK)
            (void)srt_close(accepted);
        if (listener != SRT_INVALID_SOCK)
            (void)srt_close(listener);
        (void)srt_cleanup();
    }
};
sockaddr_in address()
{
    sockaddr_in value {};
    value.sin_family = AF_INET;
    value.sin_addr.s_addr = htonl(0x7f000001U);
    return value;
}
void enable(SRTSOCKET socket, SRT_SOCKOPT option)
{
    const bool value = true;
    REQUIRE_EQ(srt_setsockflag(socket, option, &value, sizeof(value)), 0);
}
void wait_connected(SRTSOCKET member)
{
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds {2};
    while (srt_getsockstate(member) == SRTS_CONNECTING
        && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds {1});
    REQUIRE_EQ(srt_getsockstate(member), SRTS_CONNECTED);
}
}

TEST(path_identifier_codec_fixed_record_and_malformed_chain)
{
    for (std::uint32_t size = 1; size <= 32; ++size) {
        const auto original = label(size);
        std::array<std::byte, 44> bytes {};
        const auto encoded = encode_path_identifier(original, bytes);
        REQUIRE(encoded);
        REQUIRE_EQ(encoded.bytes_written, bytes.size());
        REQUIRE_EQ(bytes[0], std::byte {0x7f});
        REQUIRE_EQ(bytes[1], std::byte {0x11});
        REQUIRE_EQ(bytes[3], std::byte {10});
        REQUIRE_EQ(bytes[7], std::byte {1});
        REQUIRE_EQ(bytes[11], static_cast<std::byte>(size));
        const auto decoded =
            decode_path_identifier(decode_extension(bytes).extension);
        REQUIRE(decoded);
        REQUIRE_EQ(decoded.identifier, original);
        for (std::size_t cut = 0; cut < bytes.size(); ++cut)
            REQUIRE(!decode_extension(std::span {bytes}.first(cut)));
        bytes[7] = std::byte {2};
        REQUIRE(!decode_path_identifier(decode_extension(bytes).extension));
        bytes[7] = std::byte {1};
        bytes[11] = std::byte {33};
        REQUIRE(!decode_path_identifier(decode_extension(bytes).extension));
        bytes[11] = std::byte {0};
        REQUIRE(!decode_path_identifier(decode_extension(bytes).extension));
        bytes[11] = static_cast<std::byte>(size);
        if (size < 32) {
            bytes[12 + size] = std::byte {1};
            REQUIRE(!decode_path_identifier(decode_extension(bytes).extension));
        }
    }
    ProtocolPair peers;
    auto offer = peers.offer();
    HandshakeAction action {.kind = HandshakeActionKind::send,
        .packet = offer.packet,
        .has_handshake_extension = true,
        .has_group_membership = true,
        .group_membership = offer.group_membership,
        .has_path_identifier = true,
        .path_identifier = offer.path_identifier};
    std::array<std::byte, 1500> bytes {};
    auto encoded = encode_handshake_datagram(action, {}, 0, bytes);
    REQUIRE(encoded);
    std::copy_n(bytes.begin() + encoded.bytes_written - 44, 44,
        bytes.begin() + encoded.bytes_written);
    REQUIRE(!decode_handshake_datagram(
        std::span {bytes}.first(encoded.bytes_written + 44)));
    action.packet.extension_field &= ~handshake_extension_flag_config;
    REQUIRE(!encode_handshake_datagram(action, {}, 0, bytes));
    action.packet.extension_field |= handshake_extension_flag_config;
    action.packet.maximum_transmission_unit = 100;
    REQUIRE(!encode_handshake_datagram(action, {}, 0, bytes));
    REQUIRE(!encode_path_identifier({}, bytes));
}

TEST(path_identifier_negotiation_strict_echo_retries_and_policy)
{
    for (int defect = 0; defect < 4; ++defect) {
        ProtocolPair peers;
        auto offer = peers.offer();
        const auto retry = peers.caller.timeout();
        REQUIRE_EQ(
            message(retry.values[0]).path_identifier, offer.path_identifier);
        auto response = peers.listener.receive(offer);
        REQUIRE_EQ(peers.listener.state(), HandshakeState::connected);
        auto reply = message(response.values[0]);
        REQUIRE_EQ(reply.path_identifier, offer.path_identifier);
        if (defect == 1)
            reply.has_path_identifier = false;
        if (defect == 2)
            reply.path_identifier.bytes[0] = std::byte {99};
        if (defect == 3)
            reply.path_identifier.size = 1;
        const auto completion = peers.caller.receive(reply);
        REQUIRE(completion.size != 0);
        REQUIRE_EQ(peers.caller.state(),
            defect == 0 ? HandshakeState::connected : HandshakeState::rejected);
        const auto replay = peers.listener.receive(offer);
        REQUIRE_EQ(
            message(replay.values[0]).path_identifier, offer.path_identifier);
        offer.path_identifier.bytes[0] = std::byte {99};
        REQUIRE_EQ(peers.listener.receive(offer).size, 0U);
    }
    for (const auto modes :
        {std::array {true, false}, std::array {false, true}}) {
        ProtocolPair peers(modes[0], modes[1]);
        const auto rejection = peers.listener.receive(peers.offer());
        REQUIRE_EQ(peers.listener.state(), HandshakeState::rejected);
        REQUIRE(rejection.size != 0);
    }
    ProtocolPair ordinary(false, false);
    const auto response = ordinary.listener.receive(ordinary.offer());
    auto unsolicited = message(response.values[0]);
    unsolicited.has_path_identifier = true;
    unsolicited.path_identifier = label();
    (void)ordinary.caller.receive(unsolicited);
    REQUIRE_EQ(ordinary.caller.state(), HandshakeState::rejected);
}

TEST(path_identifier_authenticated_agreement_cannot_change_label)
{
    ProtocolPair peers(true, true, true);
    const auto offer = peers.offer();
    const auto challenge = peers.listener.receive(offer);
    REQUIRE_EQ(peers.listener.state(),
        HandshakeState::awaiting_authentication_confirmation);
    REQUIRE_EQ(
        peers.listener.receive(offer).values[0].path_identifier, label());
    auto confirmation = message(challenge.values[0]);
    confirmation.packet.request = HandshakeRequest::agreement;
    confirmation.packet.socket_id = 10;
    confirmation.session_authentication.proof[0] = std::byte {4};
    const auto completion = peers.listener.receive(confirmation);
    REQUIRE_EQ(completion.size, 2U);
    REQUIRE_EQ(peers.listener.state(), HandshakeState::connected);
    ProtocolPair changed(true, true, true);
    auto changed_offer = changed.offer();
    (void)changed.listener.receive(changed_offer);
    changed_offer.path_identifier.bytes[0] = std::byte {99};
    (void)changed.listener.receive(changed_offer);
    REQUIRE_EQ(changed.listener.state(),
        HandshakeState::awaiting_authentication_confirmation);
    REQUIRE_EQ(changed.listener.receive(confirmation).size, 2U);
    REQUIRE_EQ(changed.listener.state(), HandshakeState::connected);
}

TEST(path_identifier_snapshot_capacity_broken_retention_and_removal)
{
    REQUIRE_EQ(srt_startup(), 0);
    Sockets sockets;
    sockets.caller = srt_create_group(SRT_GTYPE_BACKUP);
    std::size_t count = 999;
    REQUIRE_EQ(
        robotweax_srt_group_path_data_v1(sockets.caller, nullptr, &count), 0);
    REQUIRE_EQ(count, 0U);
    const auto member = srt_create_socket();
    std::uint64_t generation = 0, member_generation = 0;
    sockaddr_storage peer {};
    const auto id = label(32);
    auto& registry = compat::GroupRegistry::instance();
    REQUIRE(registry.add_member(sockets.caller, member, peer, 1, 42, generation,
        member_generation, nullptr, id, true));
    ROBOTWEAX_SRT_GROUP_PATHDATA_V1 data {};
    data.flags = 999;
    REQUIRE_EQ(robotweax_srt_group_path_data_v1(sockets.caller, &data, &count),
        SRT_ERROR);
    REQUIRE_EQ(count, 1U);
    REQUIRE_EQ(data.flags, 999U);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ELARGEMSG);
    REQUIRE_EQ(
        robotweax_srt_group_path_data_v1(sockets.caller, &data, &count), 1);
    REQUIRE_EQ(data.flags, ROBOTWEAX_SRT_PATHID_LOCAL_OFFER);
    registry.update_member(sockets.caller, generation, member,
        member_generation, SRTS_CONNECTED, 0);
    registry.update_member(
        sockets.caller, generation, member, member_generation, SRTS_BROKEN, 0);
    REQUIRE_EQ(
        robotweax_srt_group_path_data_v1(sockets.caller, &data, &count), 1);
    REQUIRE_EQ(data.flags,
        ROBOTWEAX_SRT_PATHID_LOCAL_OFFER | ROBOTWEAX_SRT_PATHID_NEGOTIATED);
    REQUIRE_EQ(std::memcmp(data.identifier, id.bytes.data(), 32), 0);
    registry.remove_member(
        sockets.caller, generation, member, member_generation);
    REQUIRE_EQ(
        robotweax_srt_group_path_data_v1(sockets.caller, &data, &count), 0);
    REQUIRE_EQ(count, 0U);
    REQUIRE_EQ(robotweax_srt_group_path_data_v1(sockets.caller, &data, nullptr),
        SRT_ERROR);
    REQUIRE_EQ(srt_close(member), 0);
}

TEST(srt_compat_group_pathid_real_connections_and_replacement_overlap)
{
    for (int profile = 0; profile < 6; ++profile) {
#ifndef ENABLE_AEAD_API_PREVIEW
        if (profile >= 4)
            continue;
#endif
        REQUIRE_EQ(srt_startup(), 0);
        Sockets sockets;
        sockets.listener = srt_create_socket();
        sockets.caller = srt_create_group(
            profile % 2 == 0 ? SRT_GTYPE_BROADCAST : SRT_GTYPE_BACKUP);
        enable(sockets.listener, SRTO_GROUPCONNECT);
        for (auto socket : {sockets.listener, sockets.caller}) {
            enable(socket, SRTO_ROBOTWEAX_PATHID_REQUIRED);
            const int timeout = 1500;
            REQUIRE_EQ(srt_setsockflag(
                           socket, SRTO_RCVTIMEO, &timeout, sizeof(timeout)),
                0);
            if (profile >= 2) {
                REQUIRE_EQ(srt_setsockflag(socket, SRTO_PASSPHRASE,
                               "pathid-test-secret", 18),
                    0);
                if (profile == 3 || profile == 5)
                    enable(socket, SRTO_ROBOTWEAX_SESSIONAUTH);
#ifdef ENABLE_AEAD_API_PREVIEW
                if (profile >= 4) {
                    const int mode = 2;
                    REQUIRE_EQ(srt_setsockflag(socket, SRTO_CRYPTOMODE, &mode,
                                   sizeof(mode)),
                        0);
                }
#endif
            }
        }
        auto target = address();
        REQUIRE_EQ(srt_bind(sockets.listener,
                       reinterpret_cast<sockaddr*>(&target), sizeof(target)),
            0);
        REQUIRE_EQ(srt_listen(sockets.listener, 8), 0);
        int size = sizeof(target);
        REQUIRE_EQ(srt_getsockname(sockets.listener,
                       reinterpret_cast<sockaddr*>(&target), &size),
            0);
        std::array<SRT_SOCKGROUPCONFIG, 2> endpoints {};
        std::array<std::array<unsigned char, 4>, 2> labels {
            {{'w', 0, 'a', 1}, {'w', 0, 'b', 2}}};
        for (std::size_t i = 0; i < endpoints.size(); ++i) {
            endpoints[i] = srt_prepare_endpoint(
                nullptr, reinterpret_cast<sockaddr*>(&target), sizeof(target));
            endpoints[i].token = 100 + static_cast<int>(i);
            endpoints[i].config = srt_create_config();
            REQUIRE_EQ(srt_config_add(endpoints[i].config,
                           SRTO_ROBOTWEAX_PATHID, labels[i].data(), 4),
                0);
        }
        const auto connected =
            srt_connect_group(sockets.caller, endpoints.data(), 2);
        for (auto& endpoint : endpoints) {
            srt_delete_config(endpoint.config);
            endpoint.config = nullptr;
        }
        REQUIRE(connected != SRT_INVALID_SOCK);
        for (auto& endpoint : endpoints)
            wait_connected(endpoint.id);
        sockets.accepted = srt_accept(sockets.listener, nullptr, nullptr);
        REQUIRE(sockets.accepted != SRT_INVALID_SOCK);
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds {2};
        std::array<ROBOTWEAX_SRT_GROUP_PATHDATA_V1, 3> remote {};
        std::size_t count = remote.size();
        do {
            count = remote.size();
            REQUIRE(robotweax_srt_group_path_data_v1(
                        sockets.accepted, remote.data(), &count)
                >= 1);
            if (count == 2 && remote[0].flags == 2 && remote[1].flags == 2)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds {1});
        } while (std::chrono::steady_clock::now() < deadline);
        REQUIRE_EQ(count, 2U);
        for (const auto& id : labels) {
            REQUIRE(std::any_of(
                remote.begin(), remote.begin() + 2, [&](const auto& row) {
                    return row.flags == 2 && row.identifier_length == 4
                        && std::memcmp(row.identifier, id.data(), 4) == 0;
                }));
        }
        std::array<SRT_SOCKGROUPDATA, 3> legacy {};
        count = legacy.size();
        REQUIRE_EQ(srt_group_data(sockets.accepted, legacy.data(), &count), 2);
        REQUIRE_EQ(legacy[0].token, -1);
        REQUIRE_EQ(legacy[1].token, -1);
        count = remote.size();
        REQUIRE_EQ(robotweax_srt_group_path_data_v1(
                       sockets.caller, remote.data(), &count),
            2);
        REQUIRE_EQ(remote[0].flags, 3U);
        REQUIRE_EQ(remote[1].flags, 3U);
        // A new source port, same path label: overlapping sockets stay distinct.
        auto source = address();
        auto replacement =
            srt_prepare_endpoint(reinterpret_cast<sockaddr*>(&source),
                reinterpret_cast<sockaddr*>(&target), sizeof(target));
        replacement.config = srt_create_config();
        REQUIRE_EQ(srt_config_add(replacement.config, SRTO_ROBOTWEAX_PATHID,
                       labels[0].data(), 4),
            0);
        const auto replaced =
            srt_connect_group(sockets.caller, &replacement, 1);
        srt_delete_config(replacement.config);
        REQUIRE(replaced != SRT_INVALID_SOCK);
        wait_connected(replacement.id);
        auto old_source = address(), new_source = address();
        int old_size = sizeof(old_source), new_size = sizeof(new_source);
        REQUIRE_EQ(srt_getsockname(endpoints[0].id,
                       reinterpret_cast<sockaddr*>(&old_source), &old_size),
            0);
        REQUIRE_EQ(srt_getsockname(replacement.id,
                       reinterpret_cast<sockaddr*>(&new_source), &new_size),
            0);
        REQUIRE(old_source.sin_port != new_source.sin_port);
        count = remote.size();
        REQUIRE_EQ(robotweax_srt_group_path_data_v1(
                       sockets.caller, remote.data(), &count),
            3);
        REQUIRE(remote[0].member_id != remote[2].member_id);
        REQUIRE_EQ(
            std::memcmp(remote[0].identifier, remote[2].identifier, 32), 0);
        REQUIRE_EQ(srt_setsockflag(replacement.id, SRTO_ROBOTWEAX_PATHID,
                       labels[1].data(), 4),
            SRT_ERROR);
        REQUIRE_EQ(srt_sendmsg(sockets.caller, "payload", 7, -1, 1), 7);
        std::array<char, 16> payload {};
        REQUIRE_EQ(
            srt_recvmsg(sockets.accepted, payload.data(), payload.size()), 7);
        REQUIRE_EQ(std::memcmp(payload.data(), "payload", 7), 0);
        REQUIRE_EQ(srt_close(endpoints[0].id), 0);
        count = remote.size();
        REQUIRE_EQ(robotweax_srt_group_path_data_v1(
                       sockets.caller, remote.data(), &count),
            2);
    }
}

TEST(srt_compat_group_pathid_mismatch_and_configuration_fail_closed)
{
    for (bool required_listener : {false, true}) {
        REQUIRE_EQ(srt_startup(), 0);
        Sockets sockets;
        sockets.listener = srt_create_socket();
        sockets.caller = srt_create_group(SRT_GTYPE_BROADCAST);
        enable(sockets.listener, SRTO_GROUPCONNECT);
        enable(required_listener ? sockets.listener : sockets.caller,
            SRTO_ROBOTWEAX_PATHID_REQUIRED);
        auto target = address();
        REQUIRE_EQ(srt_bind(sockets.listener,
                       reinterpret_cast<sockaddr*>(&target), sizeof(target)),
            0);
        REQUIRE_EQ(srt_listen(sockets.listener, 4), 0);
        int size = sizeof(target);
        REQUIRE_EQ(srt_getsockname(sockets.listener,
                       reinterpret_cast<sockaddr*>(&target), &size),
            0);
        auto endpoint = srt_prepare_endpoint(
            nullptr, reinterpret_cast<sockaddr*>(&target), sizeof(target));
        if (!required_listener) {
            endpoint.config = srt_create_config();
            REQUIRE_EQ(srt_config_add(
                           endpoint.config, SRTO_ROBOTWEAX_PATHID, "wan", 3),
                0);
        }
        const auto result = srt_connect_group(sockets.caller, &endpoint, 1);
        srt_delete_config(endpoint.config);
        REQUIRE_EQ(result, SRT_INVALID_SOCK);
        REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ECONNSETUP);
        REQUIRE_EQ(srt_getrejectreason(endpoint.id), SRT_REJ_GROUP);
        std::size_t count = 1;
        REQUIRE_EQ(
            robotweax_srt_group_path_data_v1(sockets.caller, nullptr, &count),
            0);
        REQUIRE(count <= 1U);
    }
    REQUIRE_EQ(srt_startup(), 0);
    Sockets sockets;
    sockets.caller = srt_create_group(SRT_GTYPE_BROADCAST);
    REQUIRE_EQ(srt_setsockflag(sockets.caller, SRTO_ROBOTWEAX_PATHID, "x", 1),
        SRT_ERROR);
    sockets.listener = srt_create_socket();
    enable(sockets.listener, SRTO_ROBOTWEAX_PATHID_REQUIRED);
    const bool enabled = true;
    REQUIRE_EQ(srt_setsockflag(sockets.listener, SRTO_RENDEZVOUS, &enabled,
                   sizeof(enabled)),
        SRT_ERROR);
    const int file = SRTT_FILE;
    REQUIRE_EQ(
        srt_setsockflag(sockets.listener, SRTO_TRANSTYPE, &file, sizeof(file)),
        SRT_ERROR);
}
