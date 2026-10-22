#include "test.hpp"
#include "robotweax/srt/socket_options.hpp"
#include "robotweax/srt/handshake_datagram.hpp"
#include "srt/srt.h"

#include <array>
#include <cstring>

using namespace robotweax::srt;
namespace {
struct Sockets {
    SRTSOCKET listener = srt_create_socket();
    SRTSOCKET caller = srt_create_socket();
    SRTSOCKET accepted = SRT_INVALID_SOCK;
    ~Sockets()
    {
        srt_close(accepted);
        srt_close(caller);
        srt_close(listener);
    }
};
void bundled()
{
    SocketOptions options;
    SKIP_UNLESS(options.set_packet_filter("rwx-repeat-v1") == Error::none,
        "test uses bundled provider");
}
void select_filter(SRTSOCKET socket)
{
    constexpr char filter[] = "rwx-repeat-v1";
    REQUIRE_EQ(
        srt_setsockflag(socket, SRTO_PACKETFILTER, filter, sizeof(filter) - 1),
        0);
    const int timeout = 2000;
    REQUIRE_EQ(
        srt_setsockflag(socket, SRTO_RCVTIMEO, &timeout, sizeof(timeout)), 0);
}
sockaddr_in listen_filter(SRTSOCKET socket)
{
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE_EQ(srt_bind(socket, reinterpret_cast<sockaddr*>(&address),
                   sizeof(address)),
        0);
    int size = sizeof(address);
    REQUIRE_EQ(
        srt_getsockname(socket, reinterpret_cast<sockaddr*>(&address), &size),
        0);
    REQUIRE_EQ(srt_listen(socket, 1), 0);
    return address;
}
}

TEST(
    srt_compat_experimental_filter_negotiates_and_delivers_bidirectionally_without_duplicates)
{
    bundled();
    Sockets sockets;
    select_filter(sockets.listener);
    select_filter(sockets.caller);
    const int sender_payload = 1433;
    REQUIRE_EQ(srt_setsockflag(sockets.caller, SRTO_PAYLOADSIZE,
                   &sender_payload, sizeof(sender_payload)),
        0);
    const auto address = listen_filter(sockets.listener);
    REQUIRE_EQ(
        srt_connect(sockets.caller, reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)),
        0);
    sockets.accepted = srt_accept(sockets.listener, nullptr, nullptr);
    REQUIRE(sockets.accepted != SRT_INVALID_SOCK);
    std::array<char, 64> name {};
    int size = name.size();
    REQUIRE_EQ(srt_getsockflag(
                   sockets.accepted, SRTO_PACKETFILTER, name.data(), &size),
        0);
    REQUIRE_EQ(std::string_view(name.data()), "rwx-repeat-v1");
    std::array<char, 1433> payload {};
    std::array<char, 1500> received {};
    for (const auto pair : {std::pair {sockets.caller, sockets.accepted},
             std::pair {sockets.accepted, sockets.caller}}) {
        const auto payload_size =
            pair.first == sockets.caller ? payload.size() : std::size_t {1316};
        for (unsigned i = 0; i < 64; ++i) {
            payload[0] = static_cast<char>(i);
            REQUIRE_EQ(
                srt_sendmsg(pair.first, payload.data(), payload_size, -1, 1),
                payload_size);
        }
        for (unsigned i = 0; i < 64; ++i) {
            payload[0] = static_cast<char>(i);
            REQUIRE_EQ(
                srt_recvmsg(pair.second, received.data(), received.size()),
                payload_size);
            REQUIRE_EQ(
                std::memcmp(payload.data(), received.data(), payload_size), 0);
        }
    }
    SRT_TRACEBSTATS statistics {};
    REQUIRE_EQ(srt_bstats(sockets.caller, &statistics, 0), 0);
    REQUIRE(statistics.pktSndFilterExtraTotal > 0);
    REQUIRE(statistics.pktRcvFilterExtraTotal > 0);
}

TEST(srt_compat_experimental_filter_rejects_one_sided_selection)
{
    bundled();
    for (const bool caller : {false, true}) {
        Sockets sockets;
        select_filter(caller ? sockets.caller : sockets.listener);
        const auto address = listen_filter(sockets.listener);
        REQUIRE_EQ(
            srt_connect(sockets.caller,
                reinterpret_cast<const sockaddr*>(&address), sizeof(address)),
            SRT_ERROR);
    }
}

TEST(
    srt_compat_experimental_filter_rejects_unsupported_profiles_in_both_option_orders)
{
    bundled();
    constexpr char filter[] = "rwx-repeat-v1";
    for (const auto option :
        {SRTO_GROUPCONNECT, SRTO_RENDEZVOUS, SRTO_TSBPDMODE, SRTO_MESSAGEAPI}) {
        for (const bool first : {false, true}) {
            Sockets sockets;
            const bool value =
                option == SRTO_GROUPCONNECT || option == SRTO_RENDEZVOUS;
            if (first)
                select_filter(sockets.caller);
            REQUIRE_EQ(
                srt_setsockflag(sockets.caller, option, &value, sizeof(value)),
                first ? SRT_ERROR : 0);
            if (!first)
                REQUIRE_EQ(srt_setsockflag(sockets.caller, SRTO_PACKETFILTER,
                               filter, sizeof(filter) - 1),
                    SRT_ERROR);
        }
    }
    for (const bool first : {false, true}) {
        Sockets sockets;
        constexpr char secret[] = "filter-test-secret";
        if (first)
            select_filter(sockets.caller);
        REQUIRE_EQ(srt_setsockflag(sockets.caller, SRTO_PASSPHRASE, secret,
                       sizeof(secret) - 1),
            first ? SRT_ERROR : 0);
        if (!first)
            REQUIRE_EQ(srt_setsockflag(sockets.caller, SRTO_PACKETFILTER,
                           filter, sizeof(filter) - 1),
                SRT_ERROR);
    }
#ifdef ENABLE_AEAD_API_PREVIEW
    for (const bool first : {false, true}) {
        Sockets sockets;
        const auto mode = static_cast<int>(CryptoMode::aes_gcm);
        if (first)
            select_filter(sockets.caller);
        REQUIRE_EQ(srt_setsockflag(
                       sockets.caller, SRTO_CRYPTOMODE, &mode, sizeof(mode)),
            first ? SRT_ERROR : 0);
        if (!first)
            REQUIRE_EQ(srt_setsockflag(sockets.caller, SRTO_PACKETFILTER,
                           filter, sizeof(filter) - 1),
                SRT_ERROR);
    }
#endif
    const auto group = srt_create_group(SRT_GTYPE_BACKUP);
    REQUIRE(group != SRT_INVALID_SOCK);
    const auto status =
        srt_setsockflag(group, SRTO_PACKETFILTER, filter, sizeof(filter) - 1);
    srt_close(group);
    REQUIRE_EQ(status, SRT_ERROR);
}

TEST(
    experimental_filter_requires_matching_configuration_and_explicit_handshake_response)
{
    bundled();
    const auto filter =
        parse_packet_filter_configuration("rwx-repeat-v1").configuration;
    const auto message = [](const HandshakeAction& action) {
        std::array<std::byte, 1500> bytes {};
        const auto encoded =
            encode_handshake_datagram(action, PacketTimestamp {}, 0, bytes);
        REQUIRE(encoded);
        const auto decoded = decode_handshake_datagram(
            std::span {bytes}.first(encoded.bytes_written));
        REQUIRE(decoded);
        return decoded.message;
    };
    for (unsigned invalid = 0; invalid < 4; ++invalid) {
        HandshakeMachine caller {{.role = ConnectionRole::caller,
            .local_socket_id = 11,
            .packet_filter_configuration = filter}};
        HandshakeMachine listener {{.role = ConnectionRole::listener,
            .local_socket_id = 22,
            .packet_filter_configuration = filter,
            .cookie_generator = [](const Handshake&,
                                    void*) noexcept -> std::uint32_t {
                return 1234;
            }}};
        const auto start = caller.start();
        const auto response = listener.receive(message(start.values[0]));
        const auto conclusion = caller.receive(message(response.values[0]));
        auto request = message(conclusion.values[0]);
        if (invalid == 3) {
            request.has_handshake_extension = false;
            (void)listener.receive(request);
            REQUIRE_EQ(listener.state(), HandshakeState::rejected);
            continue;
        }
        const auto completed = listener.receive(request);
        auto reply = message(completed.values[0]);
        REQUIRE(reply.has_packet_filter_extension);
        if (invalid == 0)
            reply.has_packet_filter_extension = false;
        if (invalid == 1)
            reply.has_handshake_extension = false;
        if (invalid == 2)
            reply.packet_filter_configuration =
                parse_packet_filter_configuration("fec,cols:4,rows:1")
                    .configuration;
        (void)caller.receive(reply);
        REQUIRE_EQ(caller.state(), HandshakeState::rejected);
    }
}
