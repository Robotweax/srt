#include "test.hpp"
#include "robotweax/srt/handshake_datagram.hpp"
#include "srt/srt.h"
#include "compat/socket_registry.hpp"
#include "compat/transport_runtime.hpp"
#include "robotweax/srt/codec.hpp"
#include <atomic>
#include <mutex>

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

using namespace robotweax::srt;

namespace {
HandshakeMessage exchange(const HandshakeAction& action)
{
    std::array<std::byte, 1500> bytes {};
    const auto encoded = encode_handshake_datagram(action, {}, 0, bytes);
    REQUIRE(encoded);
    const auto decoded = decode_handshake_datagram(
        std::span {bytes}.first(encoded.bytes_written));
    REQUIRE(decoded);
    return decoded.message;
}
std::uint32_t cookie(const Handshake&, void*) noexcept
{
    return 123U;
}
struct Pair {
    HandshakeMachine caller;
    HandshakeMachine listener;
    Pair(bool offered, bool required)
        : caller(config(ConnectionRole::caller, offered))
        , listener(config(ConnectionRole::listener, required))
    {
    }
    static HandshakeMachine::Configuration config(
        ConnectionRole role, bool required)
    {
        HandshakeMachine::Configuration c;
        c.role = role;
        c.local_socket_id = role == ConnectionRole::caller ? 10 : 20;
        c.cookie_generator = cookie;
        c.require_npd = required;
        return c;
    }
    HandshakeMessage offer()
    {
        const auto start = caller.start();
        REQUIRE_EQ(start.size, 2U);
        REQUIRE(!exchange(start.values[0]).has_npd);
        const auto induction = listener.receive(exchange(start.values[0]));
        REQUIRE_EQ(induction.size, 2U);
        REQUIRE(!exchange(induction.values[0]).has_npd);
        const auto request = caller.receive(exchange(induction.values[0]));
        REQUIRE_EQ(request.size, 2U);
        return exchange(request.values[0]);
    }
};
struct Sockets {
    SRTSOCKET listener = SRT_INVALID_SOCK, caller = SRT_INVALID_SOCK,
              accepted = SRT_INVALID_SOCK;
    Sockets()
    {
        REQUIRE_EQ(srt_startup(), 0);
        listener = srt_create_socket();
        caller = srt_create_socket();
    }
    ~Sockets()
    {
        if (accepted != SRT_INVALID_SOCK)
            (void)srt_close(accepted);
        (void)srt_close(caller);
        (void)srt_close(listener);
        (void)srt_cleanup();
    }
};
void set(SRTSOCKET s, SRT_SOCKOPT option, int value)
{
    REQUIRE_EQ(srt_setsockflag(s, option, &value, sizeof(value)), 0);
}
int get(SRTSOCKET s, SRT_SOCKOPT option)
{
    int value = -1, size = sizeof(value);
    REQUIRE_EQ(srt_getsockflag(s, option, &value, &size), 0);
    return value;
}
sockaddr_in listen(SRTSOCKET s)
{
    sockaddr_in a {};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(0x7f000001U);
    REQUIRE_EQ(srt_bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)), 0);
    int size = sizeof(a);
    REQUIRE_EQ(srt_getsockname(s, reinterpret_cast<sockaddr*>(&a), &size), 0);
    REQUIRE_EQ(srt_listen(s, 4), 0);
    return a;
}
std::vector<char> transport_stream(unsigned units)
{
    std::vector<char> result(188 * units, static_cast<char>(0xff));
    for (unsigned i = 0; i < units; ++i) {
        result[188 * i] = 0x47;
        result[188 * i + 1] = 0x1f;
        result[188 * i + 3] = 0x10;
    }
    return result;
}
ROBOTWEAX_SRT_NPD_STATS_V1 stats(SRTSOCKET s)
{
    ROBOTWEAX_SRT_NPD_STATS_V1 result {};
    int size = sizeof(result);
    REQUIRE_EQ(srt_getsockflag(s, SRTO_ROBOTWEAX_NPD_STATS, &result, &size), 0);
    REQUIRE_EQ(result.abi_version, 1U);
    return result;
}
}

TEST(npd_handshake_required_echo_retries_and_mismatch)
{
    for (int defect = 0; defect < 3; ++defect) {
        Pair pair(true, true);
        auto offer = pair.offer();
        REQUIRE(offer.has_npd && offer.npd_valid);
        REQUIRE(exchange(pair.caller.timeout().values[0]).has_npd);
        const auto reply = pair.listener.receive(offer);
        REQUIRE_EQ(pair.listener.state(), HandshakeState::connected);
        auto echo = exchange(reply.values[0]);
        REQUIRE(echo.has_npd && echo.npd_valid);
        REQUIRE(exchange(pair.listener.receive(offer).values[0]).has_npd);
        if (defect == 1)
            echo.has_npd = false;
        if (defect == 2)
            echo.npd_valid = false;
        (void)pair.caller.receive(echo);
        REQUIRE_EQ(pair.caller.state(),
            defect == 0 ? HandshakeState::connected : HandshakeState::rejected);
    }
    Pair missing(false, true);
    (void)missing.listener.receive(missing.offer());
    REQUIRE_EQ(missing.listener.state(), HandshakeState::rejected);
    Pair ignored(true, false);
    auto offered = ignored.offer();
    offered.npd_valid = false; // OFF ignores even malformed content.
    auto reply = ignored.listener.receive(offered);
    REQUIRE_EQ(ignored.listener.state(), HandshakeState::connected);
    REQUIRE(!exchange(reply.values[0]).has_npd);
    (void)ignored.caller.receive(exchange(reply.values[0]));
    REQUIRE_EQ(ignored.caller.state(), HandshakeState::rejected);
}

TEST(npd_datagram_tracks_invalid_duplicate_and_unadvertised_record)
{
    Pair pair(true, true);
    (void)pair.offer();
    auto action = pair.caller.timeout().values[0];
    std::array<std::byte, 1500> bytes {};
    auto encoded = encode_handshake_datagram(action, {}, 0, bytes);
    REQUIRE(encoded);
    const auto size = encoded.bytes_written;
    bytes[size - 1] = std::byte {1};
    auto decoded = decode_handshake_datagram(std::span {bytes}.first(size));
    REQUIRE(decoded);
    REQUIRE(decoded.message.has_npd && !decoded.message.npd_valid);
    bytes[size - 1] = std::byte {0};
    std::copy_n(bytes.data() + size - 12, 12, bytes.data() + size);
    decoded = decode_handshake_datagram(std::span {bytes}.first(size + 12));
    REQUIRE(decoded);
    REQUIRE(!decoded.message.npd_valid);
    action.packet.extension_field &= ~handshake_extension_flag_config;
    REQUIRE(!encode_handshake_datagram(action, {}, 0, bytes));
    action.packet.extension_field |= handshake_extension_flag_config;
    action.packet.maximum_transmission_unit = 100;
    REQUIRE(!encode_handshake_datagram(action, {}, 0, bytes));
}

TEST(npd_socket_policy_rejects_incompatible_settings_in_either_order)
{
    Sockets sockets;
    REQUIRE_EQ(get(sockets.caller, SRTO_ROBOTWEAX_NPD_POLICY), 0);
    for (auto [option, value] : {std::pair {SRTO_MESSAGEAPI, 0},
             {SRTO_RENDEZVOUS, 1}, {SRTO_GROUPCONNECT, 1},
             {SRTO_TRANSTYPE, static_cast<int>(SRTT_FILE)}}) {
        const auto a = srt_create_socket();
        const auto b = srt_create_socket();
        set(a, SRTO_ROBOTWEAX_NPD_POLICY, ROBOTWEAX_SRT_EXTENSION_REQUIRED);
        REQUIRE_EQ(
            srt_setsockflag(a, option, &value, sizeof(value)), SRT_ERROR);
        set(b, option, value);
        const int required = ROBOTWEAX_SRT_EXTENSION_REQUIRED;
        REQUIRE_EQ(srt_setsockflag(b, SRTO_ROBOTWEAX_NPD_POLICY, &required,
                       sizeof(required)),
            SRT_ERROR);
        REQUIRE_EQ(srt_close(a), 0);
        REQUIRE_EQ(srt_close(b), 0);
    }
    const auto group = srt_create_group(SRT_GTYPE_BROADCAST);
    int value = 1, size = sizeof(value);
    REQUIRE_EQ(srt_setsockflag(group, SRTO_ROBOTWEAX_NPD_POLICY, &value, size),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVOP);
    REQUIRE_EQ(srt_getsockflag(group, SRTO_ROBOTWEAX_NPD_POLICY, &value, &size),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVOP);
    REQUIRE_EQ(srt_close(group), 0);
    set(sockets.caller, SRTO_ROBOTWEAX_NPD_POLICY,
        ROBOTWEAX_SRT_EXTENSION_IF_OFFERED);
    const auto address = listen(sockets.listener);
    REQUIRE_EQ(
        srt_connect(sockets.caller, reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);
}

TEST(npd_socket_clear_ctr_fec_and_authenticated_round_trips)
{
    for (int mode = 0; mode < 5; ++mode) {
#ifndef ENABLE_AEAD_API_PREVIEW
        if (mode >= 3)
            continue;
#endif
        for (const char* filter : {"", "fec,cols:2,rows:1",
                 "fec,cols:2,rows:-2", "fec,cols:2,rows:2"}) {
            Sockets sockets;
            for (auto s : {sockets.caller, sockets.listener}) {
                set(s, SRTO_ROBOTWEAX_NPD_POLICY,
                    ROBOTWEAX_SRT_EXTENSION_REQUIRED);
                set(s, SRTO_RCVTIMEO, 2000);
                set(s, SRTO_SNDTIMEO, 2000);
                set(s, SRTO_LATENCY, 20);
                if (*filter)
                    REQUIRE_EQ(srt_setsockflag(s, SRTO_PACKETFILTER, filter,
                                   static_cast<int>(std::strlen(filter))),
                        0);
                if (mode != 0) {
                    const char pass[] = "npd-test-passphrase";
                    REQUIRE_EQ(srt_setsockflag(
                                   s, SRTO_PASSPHRASE, pass, sizeof(pass) - 1),
                        0);
                    set(s, SRTO_PBKEYLEN,
                        std::strstr(filter, "rows:-2")      ? 24
                            : std::strstr(filter, "rows:2") ? 32
                                                            : 16);
                    set(s, SRTO_KMREFRESHRATE, 64);
                    set(s, SRTO_KMPREANNOUNCE, 16);
                    if (mode == 2 || mode == 4)
                        set(s, SRTO_ROBOTWEAX_SESSIONAUTH, 1);
#ifdef ENABLE_AEAD_API_PREVIEW
                    if (mode >= 3)
                        set(s, SRTO_CRYPTOMODE, 2);
#endif
                }
            }
            set(sockets.listener, SRTO_ROBOTWEAX_NPD_POLICY,
                ROBOTWEAX_SRT_EXTENSION_IF_OFFERED);
            const auto address = listen(sockets.listener);
            REQUIRE_EQ(srt_connect(sockets.caller,
                           reinterpret_cast<const sockaddr*>(&address),
                           sizeof(address)),
                0);
            sockets.accepted = srt_accept(sockets.listener, nullptr, nullptr);
            REQUIRE(sockets.accepted != SRT_INVALID_SOCK);
            REQUIRE_EQ(get(sockets.accepted, SRTO_ROBOTWEAX_NPD_POLICY),
                ROBOTWEAX_SRT_EXTENSION_REQUIRED);
            REQUIRE_EQ(get(sockets.listener, SRTO_ROBOTWEAX_NPD_POLICY),
                ROBOTWEAX_SRT_EXTENSION_IF_OFFERED);
            std::uint64_t expected = 0;
            for (unsigned i = 0; i < 140; ++i)
                expected += i % 3 == 0 ? 6U : i % 3 == 1 ? 7U : 0U;
            for (auto endpoints : {std::pair {sockets.caller, sockets.accepted},
                     std::pair {sockets.accepted, sockets.caller}}) {
                for (unsigned i = 0; i < 140; ++i) {
                    auto payload = transport_stream(7);
                    if (i % 3 == 0)
                        payload[1] = 0;
                    if (i % 3 == 2)
                        payload[188] = 0;
                    REQUIRE_EQ(srt_sendmsg(endpoints.first, payload.data(),
                                   static_cast<int>(payload.size()), -1, true),
                        static_cast<int>(payload.size()));
                }
                for (unsigned i = 0; i < 140; ++i) {
                    auto payload = transport_stream(7);
                    if (i % 3 == 0)
                        payload[1] = 0;
                    if (i % 3 == 2)
                        payload[188] = 0;
                    std::array<char, 1500> received {};
                    REQUIRE_EQ(srt_recvmsg(endpoints.second, received.data(),
                                   static_cast<int>(received.size())),
                        static_cast<int>(payload.size()));
                    REQUIRE(std::equal(
                        payload.begin(), payload.end(), received.begin()));
                }
            }
            for (auto s : {sockets.caller, sockets.accepted}) {
                const auto snapshot = stats(s);
                REQUIRE_EQ(snapshot.active, 1U);
                REQUIRE_EQ(snapshot.deleted_units, expected);
                REQUIRE_EQ(snapshot.restored_units, expected);
                REQUIRE_EQ(snapshot.malformed_payloads, 0U);
            }
        }
    }
}

TEST(npd_listener_if_offered_accepts_off_without_prefix)
{
    Sockets sockets;
    set(sockets.listener, SRTO_ROBOTWEAX_NPD_POLICY,
        ROBOTWEAX_SRT_EXTENSION_IF_OFFERED);
    set(sockets.listener, SRTO_RCVTIMEO, 2000);
    const auto address = listen(sockets.listener);
    REQUIRE_EQ(
        srt_connect(sockets.caller, reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)),
        0);
    sockets.accepted = srt_accept(sockets.listener, nullptr, nullptr);
    REQUIRE(sockets.accepted != SRT_INVALID_SOCK);
    REQUIRE_EQ(get(sockets.accepted, SRTO_ROBOTWEAX_NPD_POLICY),
        ROBOTWEAX_SRT_EXTENSION_OFF);
    const auto payload = transport_stream(7);
    REQUIRE_EQ(srt_sendmsg(sockets.caller, payload.data(),
                   static_cast<int>(payload.size()), -1, true),
        static_cast<int>(payload.size()));
    std::array<char, 1500> received {};
    REQUIRE_EQ(srt_recvmsg(sockets.accepted, received.data(),
                   static_cast<int>(received.size())),
        static_cast<int>(payload.size()));
    REQUIRE(std::equal(payload.begin(), payload.end(), received.begin()));
    REQUIRE_EQ(stats(sockets.accepted).active, 0U);
}

namespace {
struct Loss {
    std::shared_ptr<compat::DatagramChannel> channel;
    std::atomic<unsigned> originals {0}, repairs {0}, parity {0};
    std::mutex mutex;
    std::vector<std::byte> lost_payload;
    std::uint32_t lost_sequence = 0;
    bool exact_retransmission = true;
    bool corrupt_first = false;
    std::size_t parity_size = 0;
    ~Loss()
    {
        if (channel)
            channel->set_send_hook_for_testing(nullptr, nullptr);
    }
};
UdpIoResult drop_first(
    std::span<const std::byte> bytes, IpEndpoint peer, void* context) noexcept
{
    auto& loss = *static_cast<Loss*>(context);
    const auto packet = decode_packet(bytes);
    if (packet && packet.packet.kind == PacketKind::data) {
        std::lock_guard lock(loss.mutex);
        const auto& data = packet.packet.data;
        if (data.message_number == 0) {
            ++loss.parity;
            loss.parity_size = packet.packet.payload.size();
        } else if (data.retransmitted) {
            ++loss.repairs;
            if (data.sequence.value() == loss.lost_sequence)
                loss.exact_retransmission = std::equal(
                    loss.lost_payload.begin(), loss.lost_payload.end(),
                    packet.packet.payload.begin(), packet.packet.payload.end());
        } else if (loss.originals.fetch_add(1) == 0) {
            loss.lost_sequence = data.sequence.value();
            loss.lost_payload.assign(
                packet.packet.payload.begin(), packet.packet.payload.end());
            if (loss.corrupt_first) {
                std::vector<std::byte> corrupted(bytes.begin(), bytes.end());
                corrupted[16] ^= std::byte {0x80};
                return loss.channel->socket.send_to(corrupted, peer);
            }
            return {.bytes_transferred = bytes.size()};
        }
    }
    return loss.channel->socket.send_to(bytes, peer);
}
}

TEST(npd_socket_loss_recovers_full_payload_via_arq_and_each_fec_geometry)
{
    for (int mode = 0; mode < 3; ++mode) {
#ifndef ENABLE_AEAD_API_PREVIEW
        if (mode == 2)
            continue;
#endif
        for (const char* filter :
            {"", "fec,cols:2,rows:1,arq:never", "fec,cols:2,rows:-2,arq:never",
                "fec,cols:2,rows:2,arq:never"}) {
            Sockets sockets;
            for (auto s : {sockets.caller, sockets.listener}) {
                set(s, SRTO_ROBOTWEAX_NPD_POLICY,
                    ROBOTWEAX_SRT_EXTENSION_REQUIRED);
                set(s, SRTO_RCVTIMEO, 3000);
                set(s, SRTO_SNDTIMEO, 3000);
                set(s, SRTO_LATENCY, 300);
                if (*filter)
                    REQUIRE_EQ(srt_setsockflag(s, SRTO_PACKETFILTER, filter,
                                   static_cast<int>(std::strlen(filter))),
                        0);
                if (mode) {
                    const char pass[] = "npd-loss-passphrase";
                    REQUIRE_EQ(srt_setsockflag(
                                   s, SRTO_PASSPHRASE, pass, sizeof(pass) - 1),
                        0);
#ifdef ENABLE_AEAD_API_PREVIEW
                    if (mode == 2)
                        set(s, SRTO_CRYPTOMODE, 2);
#endif
                }
            }
            const auto address = listen(sockets.listener);
            REQUIRE_EQ(srt_connect(sockets.caller,
                           reinterpret_cast<const sockaddr*>(&address),
                           sizeof(address)),
                0);
            sockets.accepted = srt_accept(sockets.listener, nullptr, nullptr);
            REQUIRE(sockets.accepted != SRT_INVALID_SOCK);
            Loss loss;
            {
                auto record =
                    compat::SocketRegistry::instance().find(sockets.caller);
                std::lock_guard lock(record->mutex);
                loss.channel = record->channel;
            }
            loss.channel->set_send_hook_for_testing(drop_first, &loss);
            auto payload = transport_stream(7);
            payload[188] =
                0; // Full-size passthrough exercises the +1 FEC capacity.
            for (unsigned i = 0; i < 16; ++i)
                REQUIRE_EQ(srt_sendmsg(sockets.caller, payload.data(),
                               static_cast<int>(payload.size()), -1, true),
                    static_cast<int>(payload.size()));
            for (unsigned i = 0; i < 16; ++i) {
                std::array<char, 1500> received {};
                REQUIRE_EQ(srt_recvmsg(sockets.accepted, received.data(),
                               static_cast<int>(received.size())),
                    static_cast<int>(payload.size()));
                REQUIRE(std::equal(
                    payload.begin(), payload.end(), received.begin()));
            }
            SRT_TRACEBSTATS measured {};
            REQUIRE_EQ(srt_bstats(sockets.accepted, &measured, 0), 0);
            std::lock_guard lock(loss.mutex);
            if (*filter) {
                REQUIRE(measured.pktRcvFilterSupplyTotal > 0);
                REQUIRE(loss.parity > 0);
                REQUIRE_EQ(
                    loss.parity_size, 1317U + 4U + (mode == 2 ? 16U : 0U));
            } else {
                REQUIRE(loss.repairs > 0);
                REQUIRE(loss.exact_retransmission);
            }
        }
    }
}

TEST(npd_policy_mismatch_fails_real_connection_in_both_roles)
{
    for (bool caller_requires : {false, true}) {
        Sockets sockets;
        set(caller_requires ? sockets.caller : sockets.listener,
            SRTO_ROBOTWEAX_NPD_POLICY, ROBOTWEAX_SRT_EXTENSION_REQUIRED);
        set(sockets.caller, SRTO_CONNTIMEO, 500);
        const auto address = listen(sockets.listener);
        REQUIRE_EQ(
            srt_connect(sockets.caller,
                reinterpret_cast<const sockaddr*>(&address), sizeof(address)),
            SRT_ERROR);
        REQUIRE(srt_getsockstate(sockets.caller) != SRTS_CONNECTED);
    }
}

TEST(npd_payload_ceiling_reservation_tracks_mss_fec_and_restores_on_off)
{
    Sockets sockets;
    auto s = sockets.caller;
    set(s, SRTO_PAYLOADSIZE, 1456);
    set(s, SRTO_ROBOTWEAX_NPD_POLICY, ROBOTWEAX_SRT_EXTENSION_REQUIRED);
    REQUIRE_EQ(get(s, SRTO_PAYLOADSIZE), 1455);
    int excessive = 1456;
    REQUIRE_EQ(
        srt_setsockflag(s, SRTO_PAYLOADSIZE, &excessive, sizeof(excessive)),
        SRT_ERROR);
    const char filter[] = "fec,cols:2,rows:1";
    REQUIRE_EQ(
        srt_setsockflag(s, SRTO_PACKETFILTER, filter, sizeof(filter) - 1), 0);
    REQUIRE_EQ(get(s, SRTO_PAYLOADSIZE), 1451);
    set(s, SRTO_MSS, 1400);
    REQUIRE_EQ(get(s, SRTO_PAYLOADSIZE), 1351);
    set(s, SRTO_ROBOTWEAX_NPD_POLICY, ROBOTWEAX_SRT_EXTENSION_OFF);
    REQUIRE_EQ(get(s, SRTO_PAYLOADSIZE), 1352);
}

TEST(npd_input_estimator_counts_application_bytes_after_null_runs)
{
    SocketOptions options;
    REQUIRE_EQ(options.set(SocketOption::maximum_bandwidth_bytes_per_second, 0),
        Error::none);
    ReliabilitySession plain({.send_capacity_packets = 32,
        .receive_capacity_packets = 32,
        .maximum_payload_size = 1316});
    ReliabilitySession compressed({.send_capacity_packets = 32,
        .receive_capacity_packets = 32,
        .maximum_payload_size = 1317,
        .npd_enabled = true});
    const NegotiatedLiveOptions negotiated {};
    plain.configure_live(
        negotiated, 0, {}, {.maximum_bandwidth_bytes_per_second = 0});
    compressed.configure_live(
        negotiated, 0, {}, {.maximum_bandwidth_bytes_per_second = 0});
    auto payload = transport_stream(7);
    for (unsigned i = 0; i < 16; ++i) {
        const auto bytes = std::as_bytes(std::span {payload});
        REQUIRE_EQ(
            plain.queue_message(bytes, {}, false, i * 100000ULL), Error::none);
        REQUIRE_EQ(compressed.queue_message(bytes, {}, false, i * 100000ULL),
            Error::none);
        REQUIRE_EQ(plain.live_pacing_rate_bytes_per_second(),
            compressed.live_pacing_rate_bytes_per_second());
    }
    payload[188] = 0;
    const auto bytes = std::as_bytes(std::span {payload});
    REQUIRE_EQ(plain.queue_message(bytes, {}, false, 1600000), Error::none);
    REQUIRE_EQ(
        compressed.queue_message(bytes, {}, false, 1600000), Error::none);
    REQUIRE_EQ(plain.live_pacing_rate_bytes_per_second(),
        compressed.live_pacing_rate_bytes_per_second());
    REQUIRE_EQ(compressed.npd_deleted_units(), 112U);
}

TEST(npd_malformed_payload_is_counted_and_retransmission_restores_delivery)
{
    Sockets sockets;
    for (auto s : {sockets.caller, sockets.listener}) {
        set(s, SRTO_ROBOTWEAX_NPD_POLICY, ROBOTWEAX_SRT_EXTENSION_REQUIRED);
        set(s, SRTO_RCVTIMEO, 3000);
        set(s, SRTO_LATENCY, 300);
    }
    const auto address = listen(sockets.listener);
    REQUIRE_EQ(
        srt_connect(sockets.caller, reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)),
        0);
    sockets.accepted = srt_accept(sockets.listener, nullptr, nullptr);
    REQUIRE(sockets.accepted != SRT_INVALID_SOCK);
    Loss loss;
    loss.corrupt_first = true;
    {
        const auto record =
            compat::SocketRegistry::instance().find(sockets.caller);
        std::lock_guard lock(record->mutex);
        loss.channel = record->channel;
    }
    loss.channel->set_send_hook_for_testing(drop_first, &loss);
    const auto payload = transport_stream(7);
    for (unsigned i = 0; i < 16; ++i)
        REQUIRE_EQ(srt_sendmsg(sockets.caller, payload.data(),
                       static_cast<int>(payload.size()), -1, true),
            static_cast<int>(payload.size()));
    for (unsigned i = 0; i < 16; ++i) {
        std::array<char, 1500> received {};
        REQUIRE_EQ(srt_recvmsg(sockets.accepted, received.data(),
                       static_cast<int>(received.size())),
            static_cast<int>(payload.size()));
        REQUIRE(std::equal(payload.begin(), payload.end(), received.begin()));
    }
    REQUIRE_EQ(stats(sockets.accepted).malformed_payloads, 1U);
    REQUIRE_EQ(stats(sockets.accepted).restored_units, 112U);
    REQUIRE_EQ(srt_getsockstate(sockets.accepted), SRTS_CONNECTED);
}

TEST(npd_authenticated_agreement_must_repeat_negotiation)
{
    for (bool missing : {false, true}) {
        auto c = Pair::config(ConnectionRole::caller, true);
        c.require_session_authentication = true;
        c.session_authentication.caller_nonce[0] = std::byte {1};
        auto l = Pair::config(ConnectionRole::listener, true);
        l.require_session_authentication = true;
        l.session_authentication.caller_nonce[0] = std::byte {1};
        l.session_authentication.listener_nonce[0] = std::byte {2};
        l.session_authentication.proof[0] = std::byte {3};
        l.session_authentication_confirmation = l.session_authentication;
        l.session_authentication_confirmation.proof[0] = std::byte {4};
        HandshakeMachine caller(c), listener(l);
        const auto start = caller.start();
        const auto induction = listener.receive(exchange(start.values[0]));
        const auto offer = caller.receive(exchange(induction.values[0]));
        const auto response = listener.receive(exchange(offer.values[0]));
        REQUIRE_EQ(listener.state(),
            HandshakeState::awaiting_authentication_confirmation);
        REQUIRE_EQ(caller.set_session_authentication(
                       l.session_authentication_confirmation),
            Error::none);
        const auto completion = caller.receive(exchange(response.values[0]));
        REQUIRE_EQ(caller.state(), HandshakeState::connected);
        auto agreement = exchange(completion.values[0]);
        REQUIRE(agreement.has_npd && agreement.npd_valid);
        if (missing)
            agreement.has_npd = false;
        (void)listener.receive(agreement);
        REQUIRE_EQ(listener.state(),
            missing ? HandshakeState::awaiting_authentication_confirmation
                    : HandshakeState::connected);
        if (missing) {
            agreement.has_npd = true;
            (void)listener.receive(agreement);
            REQUIRE_EQ(listener.state(), HandshakeState::connected);
        }
    }
}

TEST(npd_private_statistics_abi_and_short_getter_are_bounded)
{
    static_assert(sizeof(ROBOTWEAX_SRT_NPD_STATS_V1) == 32);
    Sockets sockets;
    std::array<unsigned char, 32> bytes;
    bytes.fill(0xa5);
    const auto original = bytes;
    int size = 31;
    REQUIRE_EQ(srt_getsockflag(sockets.caller, SRTO_ROBOTWEAX_NPD_STATS,
                   bytes.data(), &size),
        SRT_ERROR);
    REQUIRE_EQ(bytes, original);
    const auto initial = stats(sockets.caller);
    REQUIRE_EQ(initial.active, 0U);
    REQUIRE_EQ(initial.deleted_units, 0U);
    REQUIRE_EQ(initial.restored_units, 0U);
    REQUIRE_EQ(initial.malformed_payloads, 0U);
    REQUIRE_EQ(srt_setsockflag(sockets.caller, SRTO_ROBOTWEAX_NPD_STATS,
                   &initial, sizeof(initial)),
        SRT_ERROR);
#ifdef ENABLE_AEAD_API_PREVIEW
    SocketOptions options;
    REQUIRE_EQ(options.reserve_npd_prefix(true), Error::none);
    REQUIRE_EQ(options.set_packet_filter("fec,cols:2,rows:1"), Error::none);
    REQUIRE_EQ(options.set(SocketOption::crypto_mode, 2), Error::none);
    REQUIRE_EQ(
        options.set(SocketOption::session_authentication, 1), Error::none);
    // 44 bytes IP/UDP/SRT + 4 FEC + 16 GCM + 40 authenticated parity + 1 NPD.
    REQUIRE_EQ(options.set(SocketOption::maximum_segment_size, 105),
        Error::invalid_state);
    REQUIRE_EQ(options.maximum_segment_size(), 1500U);
    REQUIRE_EQ(
        options.set(SocketOption::maximum_segment_size, 106), Error::none);
    REQUIRE_EQ(options.maximum_payload_size(), 1U);
#endif
}
