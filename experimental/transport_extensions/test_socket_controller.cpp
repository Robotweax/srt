#include "test.hpp"
#include "robotweax/srt/handshake_datagram.hpp"
#include "robotweax/srt/socket_options.hpp"
#include "srt/srt.h"
#include "compat/socket_registry.hpp"

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

void select(SRTSOCKET socket)
{
    constexpr char name[] = "rwx-fixed-v1";
    REQUIRE_EQ(
        srt_setsockflag(socket, SRTO_CONGESTION, name, sizeof(name) - 1), 0);
}

sockaddr_in listen(SRTSOCKET socket)
{
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE_EQ(srt_bind(socket, reinterpret_cast<const sockaddr*>(&address),
                   sizeof(address)),
        0);
    int size = sizeof(address);
    REQUIRE_EQ(
        srt_getsockname(socket, reinterpret_cast<sockaddr*>(&address), &size),
        0);
    REQUIRE_EQ(srt_listen(socket, 1), 0);
    return address;
}

HandshakeMessage message(const HandshakeAction& action)
{
    std::array<std::byte, 1'500> storage {};
    const auto encoded =
        encode_handshake_datagram(action, PacketTimestamp {}, 0, storage);
    REQUIRE(encoded);
    const auto decoded = decode_handshake_datagram(
        std::span {storage}.first(encoded.bytes_written));
    REQUIRE(decoded);
    return decoded.message;
}

void require_bundled_provider()
{
    SocketOptions options;
    SKIP_UNLESS(
        options.set_congestion_controller("rwx-fixed-v1") == Error::none,
        "test uses the bundled provider");
}
}

TEST(srt_compat_experimental_controller_transfers_both_directions_clear_and_ctr)
{
    require_bundled_provider();
    for (const bool encrypted : {false, true}) {
        Sockets sockets;
        for (const auto socket : {sockets.listener, sockets.caller}) {
            select(socket);
            const int timeout = 2'000;
            REQUIRE_EQ(srt_setsockflag(
                           socket, SRTO_RCVTIMEO, &timeout, sizeof(timeout)),
                0);
            if (encrypted) {
                constexpr char secret[] = "controller-test-secret";
                REQUIRE_EQ(srt_setsockflag(socket, SRTO_PASSPHRASE, secret,
                               sizeof(secret) - 1),
                    0);
            }
        }
        const auto address = listen(sockets.listener);
        REQUIRE_EQ(
            srt_connect(sockets.caller,
                reinterpret_cast<const sockaddr*>(&address), sizeof(address)),
            0);
        sockets.accepted = srt_accept(sockets.listener, nullptr, nullptr);
        REQUIRE(sockets.accepted != SRT_INVALID_SOCK);
        const auto accepted =
            compat::SocketRegistry::instance().find(sockets.accepted);
        REQUIRE(accepted != nullptr);
        {
            std::lock_guard lock(accepted->mutex);
            REQUIRE_EQ(congestion_controller_name(
                           accepted->native_options.congestion_controller()),
                "rwx-fixed-v1");
        }
        SRT_TRACEBSTATS statistics {};
        REQUIRE_EQ(srt_bstats(sockets.caller, &statistics, 0), 0);
        REQUIRE_EQ(statistics.pktCongestionWindow, 32);
        std::array<char, 100> payload {};
        std::array<char, 1'500> received {};
        for (unsigned index = 0; index < 64; ++index) {
            payload[0] = static_cast<char>(index);
            REQUIRE_EQ(srt_sendmsg(sockets.caller, payload.data(),
                           payload.size(), -1, 1),
                payload.size());
        }
        for (unsigned index = 0; index < 64; ++index) {
            payload[0] = static_cast<char>(index);
            const auto count =
                srt_recvmsg(sockets.accepted, received.data(), received.size());
            if (count < 0)
                throw std::runtime_error(srt_getlasterror_str());
            REQUIRE_EQ(count, payload.size());
            REQUIRE_EQ(
                std::memcmp(payload.data(), received.data(), payload.size()),
                0);
        }
        REQUIRE_EQ(srt_sendmsg(
                       sockets.accepted, payload.data(), payload.size(), -1, 1),
            payload.size());
        REQUIRE_EQ(
            srt_recvmsg(sockets.caller, received.data(), received.size()),
            payload.size());
    }
}

TEST(srt_compat_experimental_controller_rejects_one_sided_selection)
{
    require_bundled_provider();
    for (const bool custom_caller : {false, true}) {
        Sockets sockets;
        select(custom_caller ? sockets.caller : sockets.listener);
        const auto address = listen(sockets.listener);
        REQUIRE_EQ(
            srt_connect(sockets.caller,
                reinterpret_cast<const sockaddr*>(&address), sizeof(address)),
            SRT_ERROR);
        REQUIRE(srt_getsockstate(sockets.caller) != SRTS_CONNECTED);
    }
}

TEST(experimental_controller_requires_explicit_handshake_echo)
{
    require_bundled_provider();
    CongestionController controller {};
    REQUIRE(parse_congestion_controller("rwx-fixed-v1", controller));
    HandshakeMachine caller {{.role = ConnectionRole::caller,
        .local_socket_id = 100,
        .congestion_controller = controller}};
    HandshakeMachine listener {{.role = ConnectionRole::listener,
        .local_socket_id = 200,
        .congestion_controller = controller,
        .cookie_generator = [](const Handshake&,
                                void*) noexcept -> std::uint32_t {
            return 0x12345678;
        }}};
    auto induction = caller.start();
    auto response = listener.receive(message(induction.values[0]));
    auto conclusion = caller.receive(message(response.values[0]));
    auto completed = listener.receive(message(conclusion.values[0]));
    auto reply = message(completed.values[0]);
    REQUIRE(reply.has_congestion_extension);
    reply.has_congestion_extension = false;
    (void)caller.receive(reply);
    REQUIRE_EQ(caller.state(), HandshakeState::rejected);
}

TEST(srt_compat_experimental_controller_rejects_unqualified_option_combinations)
{
    require_bundled_provider();
    Sockets sockets;
    select(sockets.caller);
    const bool enabled = true;
    const bool disabled = false;
    for (const auto option : {SRTO_RENDEZVOUS, SRTO_GROUPCONNECT})
        REQUIRE_EQ(
            srt_setsockflag(sockets.caller, option, &enabled, sizeof(enabled)),
            SRT_ERROR);
    for (const auto option : {SRTO_TSBPDMODE, SRTO_MESSAGEAPI})
        REQUIRE_EQ(srt_setsockflag(
                       sockets.caller, option, &disabled, sizeof(disabled)),
            SRT_ERROR);
    constexpr char filter[] = "fec,cols:4,rows:1";
    REQUIRE_EQ(srt_setsockflag(sockets.caller, SRTO_PACKETFILTER, filter,
                   sizeof(filter) - 1),
        SRT_ERROR);
}

TEST(
    srt_compat_experimental_controller_restrictions_apply_in_both_option_orders)
{
    require_bundled_provider();
    constexpr char name[] = "rwx-fixed-v1";
    for (const auto option :
        {SRTO_RENDEZVOUS, SRTO_GROUPCONNECT, SRTO_TSBPDMODE, SRTO_MESSAGEAPI}) {
        Sockets sockets;
        const bool value =
            option == SRTO_RENDEZVOUS || option == SRTO_GROUPCONNECT;
        REQUIRE_EQ(
            srt_setsockflag(sockets.caller, option, &value, sizeof(value)), 0);
        REQUIRE_EQ(srt_setsockflag(
                       sockets.caller, SRTO_CONGESTION, name, sizeof(name) - 1),
            SRT_ERROR);
    }
    Sockets sockets;
    constexpr char filter[] = "fec,cols:4,rows:1";
    REQUIRE_EQ(srt_setsockflag(sockets.caller, SRTO_PACKETFILTER, filter,
                   sizeof(filter) - 1),
        0);
    REQUIRE_EQ(srt_setsockflag(
                   sockets.caller, SRTO_CONGESTION, name, sizeof(name) - 1),
        SRT_ERROR);
#ifdef ENABLE_AEAD_API_PREVIEW
    Sockets encrypted;
    const int mode = static_cast<int>(CryptoMode::aes_gcm);
    REQUIRE_EQ(
        srt_setsockflag(encrypted.caller, SRTO_CRYPTOMODE, &mode, sizeof(mode)),
        0);
    REQUIRE_EQ(srt_setsockflag(
                   encrypted.caller, SRTO_CONGESTION, name, sizeof(name) - 1),
        SRT_ERROR);
    select(encrypted.listener);
    REQUIRE_EQ(srt_setsockflag(
                   encrypted.listener, SRTO_CRYPTOMODE, &mode, sizeof(mode)),
        SRT_ERROR);
#endif
}

#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_FILTERS
TEST(
    srt_compat_experimental_controller_and_filter_remain_exclusive_in_both_orders)
{
    require_bundled_provider();
    SocketOptions probe;
    SKIP_UNLESS(probe.set_packet_filter("rwx-repeat-v1") == Error::none,
        "test uses the bundled filter");
    constexpr char filter[] = "rwx-repeat-v1";
    constexpr char controller[] = "rwx-fixed-v1";
    for (const bool controller_first : {false, true}) {
        SocketOptions options;
        if (controller_first) {
            REQUIRE_EQ(
                options.set_congestion_controller(controller), Error::none);
            REQUIRE_EQ(options.set_packet_filter(filter), Error::invalid_state);
            REQUIRE_EQ(
                congestion_controller_name(options.congestion_controller()),
                controller);
            REQUIRE(!options.packet_filter_configuration().enabled);
            REQUIRE_EQ(options.set_congestion_controller("live"), Error::none);
            REQUIRE_EQ(options.set_packet_filter(filter), Error::none);
        } else {
            REQUIRE_EQ(options.set_packet_filter(filter), Error::none);
            REQUIRE_EQ(options.set_congestion_controller(controller),
                Error::invalid_state);
            REQUIRE_EQ(
                options.congestion_controller(), CongestionController::live);
            REQUIRE_EQ(options.packet_filter_configuration().view(), filter);
            REQUIRE_EQ(options.set_packet_filter(""), Error::none);
            REQUIRE_EQ(
                options.set_congestion_controller(controller), Error::none);
        }
        Sockets sockets;
        const auto first =
            controller_first ? SRTO_CONGESTION : SRTO_PACKETFILTER;
        const auto second =
            controller_first ? SRTO_PACKETFILTER : SRTO_CONGESTION;
        const auto selected = controller_first ? std::string_view {controller}
                                               : std::string_view {filter};
        const auto rejected = controller_first ? std::string_view {filter}
                                               : std::string_view {controller};
        REQUIRE_EQ(srt_setsockflag(sockets.caller, first, selected.data(),
                       static_cast<int>(selected.size())),
            0);
        REQUIRE_EQ(srt_setsockflag(sockets.caller, second, rejected.data(),
                       static_cast<int>(rejected.size())),
            SRT_ERROR);
        const auto retained =
            compat::SocketRegistry::instance().find(sockets.caller);
        REQUIRE(retained != nullptr);
        {
            std::lock_guard lock(retained->mutex);
            if (controller_first) {
                REQUIRE_EQ(
                    congestion_controller_name(
                        retained->native_options.congestion_controller()),
                    selected);
                REQUIRE(!retained->native_options.packet_filter_configuration()
                        .enabled);
            } else {
                REQUIRE_EQ(
                    retained->native_options.packet_filter_configuration()
                        .view(),
                    selected);
                REQUIRE_EQ(retained->native_options.congestion_controller(),
                    CongestionController::live);
            }
        }
        if (controller_first) {
            constexpr char live[] = "live";
            REQUIRE_EQ(srt_setsockflag(sockets.caller, SRTO_CONGESTION, live,
                           sizeof(live) - 1),
                0);
            REQUIRE_EQ(srt_setsockflag(sockets.caller, second, rejected.data(),
                           static_cast<int>(rejected.size())),
                0);
        } else {
            REQUIRE_EQ(srt_setsockflag(sockets.caller, first, selected.data(),
                           static_cast<int>(selected.size())),
                0);
        }
    }
}
#endif
