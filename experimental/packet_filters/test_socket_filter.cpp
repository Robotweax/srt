#include "test.hpp"
#include "member_filter.hpp"
#include "robotweax/srt/udp.hpp"
#include "robotweax/srt/socket_options.hpp"
#include "robotweax/srt/handshake_datagram.hpp"
#include "srt/srt.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <optional>
#include <thread>
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
void select_filter(SRTSOCKET socket, std::string_view filter = "rwx-repeat-v1")
{
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_PACKETFILTER, filter.data(),
                   static_cast<int>(filter.size())),
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

namespace {
void bidirectional(std::string_view filter)
{
    SocketOptions selection;
    SKIP_UNLESS(selection.set_packet_filter(filter) == Error::none,
        "test uses bundled provider");
    Sockets sockets;
    select_filter(sockets.listener, filter);
    select_filter(sockets.caller, filter);
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
    REQUIRE_EQ(std::string_view(name.data()), filter);
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
}

TEST(
    srt_compat_experimental_filter_negotiates_and_delivers_bidirectionally_without_duplicates)
{
    bidirectional("rwx-repeat-v1");
}

TEST(
    srt_compat_experimental_xor8_negotiates_and_delivers_bidirectionally_without_duplicates)
{
    bidirectional("rwx-xor8-v1");
}

TEST(srt_compat_experimental_xor8_rejects_different_provider_names)
{
    bundled();
    SocketOptions selection;
    SKIP_UNLESS(selection.set_packet_filter("rwx-xor8-v1") == Error::none,
        "test uses bundled XOR provider");
    for (const bool reverse : {false, true}) {
        Sockets sockets;
        select_filter(
            sockets.listener, reverse ? "rwx-xor8-v1" : "rwx-repeat-v1");
        select_filter(
            sockets.caller, reverse ? "rwx-repeat-v1" : "rwx-xor8-v1");
        const auto address = listen_filter(sockets.listener);
        REQUIRE_EQ(
            srt_connect(sockets.caller,
                reinterpret_cast<const sockaddr*>(&address), sizeof(address)),
            SRT_ERROR);
    }
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

namespace {
// Bounded real-UDP impairment fixture. Choose erasures by wire sequence so
// random initial sequences and partial first blocks cannot change the plan.
class XorLossRelay {
public:
    explicit XorLossRelay(IpEndpoint listener, bool use_repairs)
        : listener_(listener)
        , use_repairs_(use_repairs)
    {
        REQUIRE(socket_.valid());
        REQUIRE_EQ(socket_.bind(IpEndpoint::loopback()), Error::none);
        const auto local = socket_.local_endpoint();
        REQUIRE(local);
        endpoint = local.endpoint;
        worker_ = std::jthread([this](std::stop_token stop) {
            run(stop);
        });
    }
    void stop()
    {
        worker_.request_stop();
        worker_.join();
    }
    IpEndpoint endpoint;
    std::atomic<unsigned> sources {0};
    std::atomic<unsigned> erasures {0};
    std::atomic<unsigned> repairs {0};
    std::atomic<unsigned> retransmissions {0};
    std::atomic<unsigned> errors {0};

private:
    void forward(std::span<const std::byte> bytes, IpEndpoint destination)
    {
        const auto sent = socket_.send_to(bytes, destination);
        if (!sent || sent.bytes_transferred != bytes.size())
            ++errors;
    }
    void run(std::stop_token stop)
    {
        std::array<std::byte, 65536> datagram;
        std::optional<SequenceNumber> first_block;
        std::optional<IpEndpoint> caller;
        const auto block_index = [&](SequenceNumber sequence) -> int {
            if (!first_block)
                return -1;
            const auto offset = sequence.distance_from(*first_block);
            return offset >= 0 && offset < 64 ? offset / 8 : -1;
        };
        while (!stop.stop_requested()) {
            const auto ready = socket_.wait_readable(10);
            if (!ready) {
                ++errors;
                break;
            }
            if (!ready.ready)
                continue;
            const auto received = socket_.receive_from(datagram);
            if (!received) {
                ++errors;
                continue;
            }
            const auto bytes = std::span<const std::byte> {datagram}.first(
                received.bytes_transferred);
            if (received.peer == listener_) {
                if (caller)
                    forward(bytes, *caller);
                else
                    ++errors;
                continue;
            }
            caller = received.peer;
            const auto decoded = decode_packet(bytes);
            if (decoded && decoded.packet.kind == PacketKind::data) {
                const auto& header = decoded.packet.data;
                if (header.retransmitted) {
                    ++retransmissions;
                    if (!use_repairs_)
                        forward(bytes, listener_);
                    continue;
                }
                ++sources;
                if (!first_block && (header.sequence.value() & 7U) == 0)
                    first_block = header.sequence;
                const auto block = block_index(header.sequence);
                if (block >= 0
                    && (header.sequence.value() & 7U)
                        == static_cast<unsigned>(block)) {
                    ++erasures;
                    continue;
                }
            } else if (decoded && decoded.packet.kind == PacketKind::control
                && decoded.packet.control.type == ControlType::user_defined
                && decoded.packet.control.subtype
                    == extensions::member_filter_subtype) {
                ++repairs;
                if (!use_repairs_)
                    continue;
                // Originals remain erased for the entire transfer. Replaying
                // repair cannot substitute physical DATA for a reconstruction.
                forward(bytes, listener_);
                forward(bytes, listener_);
                continue;
            }
            forward(bytes, listener_);
        }
    }
    UdpSocket socket_;
    IpEndpoint listener_;
    bool use_repairs_;
    std::jthread
        worker_; // Join before any storage borrowed by the worker dies.
};

void xor_udp_recovery(bool use_repairs)
{
    SocketOptions selection;
    SKIP_UNLESS(selection.set_packet_filter("rwx-xor8-v1") == Error::none,
        "test uses bundled XOR provider");
    Sockets sockets;
    select_filter(sockets.listener, "rwx-xor8-v1");
    select_filter(sockets.caller, "rwx-xor8-v1");
    const int payload_limit = 1433;
    REQUIRE_EQ(srt_setsockflag(sockets.caller, SRTO_PAYLOADSIZE, &payload_limit,
                   sizeof(payload_limit)),
        0);
    const auto listener = listen_filter(sockets.listener);
    XorLossRelay relay(
        IpEndpoint::loopback(ntohs(listener.sin_port)), use_repairs);
    auto address = listener;
    address.sin_port = htons(relay.endpoint.port);
    REQUIRE_EQ(
        srt_connect(sockets.caller, reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)),
        0);
    sockets.accepted = srt_accept(sockets.listener, nullptr, nullptr);
    REQUIRE(sockets.accepted != SRT_INVALID_SOCK);
    constexpr unsigned messages = 80; // Eight complete blocks after any prefix.
    const auto payload = [](unsigned message) {
        std::array<char, 1433> bytes;
        for (std::size_t i = 0; i < bytes.size(); ++i)
            bytes[i] = static_cast<char>((message * 37U + i) & 255U);
        return bytes;
    };
    const auto size = [](unsigned message) -> int {
        return message % 3 == 2 ? 1433 : 1 + message % 31;
    };
    for (unsigned i = 0; i < messages; ++i) {
        const auto bytes = payload(i);
        REQUIRE_EQ(
            srt_sendmsg(sockets.caller, bytes.data(), size(i), -1, 1), size(i));
    }
    std::array<char, 1500> received;
    for (unsigned i = 0; i < messages; ++i) {
        REQUIRE_EQ(
            srt_recvmsg(sockets.accepted, received.data(), received.size()),
            size(i));
        const auto expected = payload(i);
        REQUIRE_EQ(std::memcmp(received.data(), expected.data(), size(i)), 0);
    }
    relay.stop();
    REQUIRE_EQ(relay.errors.load(), 0U);
    REQUIRE_EQ(relay.sources.load(), messages);
    REQUIRE_EQ(relay.erasures.load(), 8U);
    REQUIRE(relay.repairs.load() >= 9U);
    SRT_TRACEBSTATS sender {};
    SRT_TRACEBSTATS receiver {};
    REQUIRE_EQ(srt_bstats(sockets.caller, &sender, 0), 0);
    REQUIRE_EQ(srt_bstats(sockets.accepted, &receiver, 0), 0);
    REQUIRE_EQ(sender.pktSentUniqueTotal, messages);
    REQUIRE_EQ(receiver.pktRecvUniqueTotal, messages);
    REQUIRE_EQ(receiver.pktRcvFilterSupplyTotal, use_repairs ? 8 : 0);
    if (use_repairs) {
        // Filter supply counts candidates, including duplicates. The physical
        // DATA deficit and complete unique delivery prove accepted recovery.
        REQUIRE_EQ(receiver.pktRecvTotal - receiver.pktRcvFilterExtraTotal,
            messages - 8);
    } else {
        REQUIRE(relay.retransmissions.load() >= 8U);
        REQUIRE(sender.pktRetransTotal >= 8);
    }
    const bool synchronous = false;
    REQUIRE_EQ(srt_setsockflag(sockets.accepted, SRTO_RCVSYN, &synchronous,
                   sizeof(synchronous)),
        0);
    REQUIRE_EQ(srt_recvmsg(sockets.accepted, received.data(), received.size()),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EASYNCRCV);
}
}

TEST(srt_compat_experimental_xor8_udp_recovers_each_erasure_with_arq_blocked)
{
    xor_udp_recovery(true);
}

TEST(srt_compat_experimental_xor8_udp_falls_back_to_arq_when_repairs_are_lost)
{
    xor_udp_recovery(false);
}
