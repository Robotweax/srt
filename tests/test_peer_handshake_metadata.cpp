#include "test.hpp"

#include "compat/socket_registry.hpp"
#include "robotweax/srt/handshake_datagram.hpp"
#include "srt/srt.h"
#include "srt/access_control.h"

#include <array>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace {

using Snapshot = ROBOTWEAX_SRT_PEER_HANDSHAKE_V1;
using namespace robotweax::srt;

struct Sockets {
    std::vector<SRTSOCKET> handles;
    Sockets()
    {
        REQUIRE_EQ(srt_startup(), 0);
    }
    ~Sockets()
    {
        for (const auto socket : handles)
            (void)srt_close(socket);
        (void)srt_cleanup();
    }
    SRTSOCKET create()
    {
        const auto socket = srt_create_socket();
        REQUIRE(socket != SRT_INVALID_SOCK);
        handles.push_back(socket);
        return socket;
    }
};

void set_integer(SRTSOCKET socket, SRT_SOCKOPT option, std::int32_t value)
{
    REQUIRE_EQ(srt_setsockflag(socket, option, &value, sizeof(value)), 0);
}

std::int32_t integer(SRTSOCKET socket, SRT_SOCKOPT option)
{
    std::int32_t value = -1;
    int size = sizeof(value);
    REQUIRE_EQ(srt_getsockflag(socket, option, &value, &size), 0);
    return value;
}

Snapshot snapshot(SRTSOCKET socket)
{
    Snapshot value;
    std::memset(&value, 0xff, sizeof(value));
    int size = sizeof(value);
    REQUIRE_EQ(
        srt_getsockflag(socket, SRTO_ROBOTWEAX_PEER_HANDSHAKE, &value, &size),
        0);
    REQUIRE_EQ(size, static_cast<int>(sizeof(value)));
    REQUIRE_EQ(value.abi_version, ROBOTWEAX_SRT_PEER_HANDSHAKE_VERSION);
    return value;
}

void unavailable(const Snapshot& value)
{
    REQUIRE_EQ(value.abi_version, ROBOTWEAX_SRT_PEER_HANDSHAKE_VERSION);
    REQUIRE_EQ(value.available, 0U);
    REQUIRE_EQ(value.srt_version, 0U);
    REQUIRE_EQ(value.flags, 0U);
    REQUIRE_EQ(value.receiver_delay_milliseconds, 0U);
    REQUIRE_EQ(value.sender_delay_milliseconds, 0U);
}

constexpr std::uint32_t live_flags = ROBOTWEAX_SRT_HSFLAG_TSBPD_SEND
    | ROBOTWEAX_SRT_HSFLAG_TSBPD_RECEIVE | ROBOTWEAX_SRT_HSFLAG_TLPKTDROP
    | ROBOTWEAX_SRT_HSFLAG_NAKREPORT | ROBOTWEAX_SRT_HSFLAG_RETRANSMIT
    | ROBOTWEAX_SRT_HSFLAG_PACKET_FILTER;

enum class Exit { accept, reject, exception, close };
struct Observation {
    Snapshot before {};
    Snapshot after {};
    std::shared_ptr<compat::SocketRecord> retained;
    SRTSOCKET handle = SRT_INVALID_SOCK;
    Exit exit = Exit::accept;
    int calls = 0;
    int protocol = 0;
    std::int32_t peer_version = -1;
    std::int32_t local_receive_delay = -1;
    std::int32_t local_peer_delay = -1;
    bool read_only = false;
};

int observe(
    void* opaque, SRTSOCKET socket, int protocol, const sockaddr*, const char*)
{
    auto& test = *static_cast<Observation*>(opaque);
    ++test.calls;
    test.protocol = protocol;
    test.handle = socket;
    test.retained = compat::SocketRegistry::instance().find(socket);
    test.before = snapshot(socket);
    test.peer_version = integer(socket, SRTO_PEERVERSION);
    test.local_receive_delay = integer(socket, SRTO_RCVLATENCY);
    test.local_peer_delay = integer(socket, SRTO_PEERLATENCY);
    test.read_only = srt_setsockflag(socket, SRTO_ROBOTWEAX_PEER_HANDSHAKE,
                         &test.before, sizeof(test.before))
            == SRT_ERROR
        && srt_getlasterror(nullptr) == SRT_EINVOP;
    set_integer(socket, SRTO_RCVLATENCY, 500);
    set_integer(socket, SRTO_PEERLATENCY, 600);
    test.after = snapshot(socket);
    if (test.exit == Exit::exception)
        throw std::runtime_error("admission failure");
    if (test.exit == Exit::close) {
        REQUIRE_EQ(srt_close(socket), 0);
        return 0;
    }
    if (test.exit == Exit::reject) {
        REQUIRE_EQ(srt_setrejectreason(socket, SRT_REJX_OVERLOAD), 0);
        return -1;
    }
    return 0;
}

enum class Extension { request, missing, response };
struct Incoming {
    std::shared_ptr<compat::DatagramChannel> channel;
    HandshakeExtensionParameters parameters;
    Extension extension = Extension::request;
    ~Incoming()
    {
        channel->set_send_hook_for_testing(nullptr, nullptr);
    }
};

UdpIoResult replace_request(
    std::span<const std::byte> bytes, IpEndpoint peer, void* context) noexcept
{
    auto& test = *static_cast<Incoming*>(context);
    const auto decoded = decode_handshake_datagram(bytes);
    if (!decoded
        || decoded.message.packet.request != HandshakeRequest::conclusion
        || decoded.message.extension_type
            != HandshakeExtensionType::handshake_request)
        return test.channel->socket.send_to(bytes, peer);
    HandshakeAction action;
    action.kind = HandshakeActionKind::send;
    action.packet = decoded.message.packet;
    action.has_handshake_extension = test.extension != Extension::missing;
    action.extension_type = test.extension == Extension::response
        ? HandshakeExtensionType::handshake_response
        : HandshakeExtensionType::handshake_request;
    action.extension_parameters = test.parameters;
    action.has_congestion_extension = decoded.message.has_congestion_extension;
    action.congestion_controller = decoded.message.congestion_controller;
    std::array<std::byte, 1'500> datagram {};
    const auto encoded =
        encode_handshake_datagram(action, decoded.control.timestamp,
            decoded.control.destination_socket_id, datagram);
    if (!encoded)
        return {.error = encoded.error};
    return test.channel->socket.send_to(
        std::span {datagram}.first(encoded.bytes_written), peer);
}

sockaddr_in listen(SRTSOCKET socket, Observation& observation)
{
    set_integer(socket, SRTO_CONNTIMEO, 1'000);
    set_integer(socket, SRTO_RCVLATENCY, 300);
    set_integer(socket, SRTO_PEERLATENCY, 400);
    REQUIRE_EQ(srt_listen_callback(socket, observe, &observation), 0);
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
    REQUIRE_EQ(srt_listen(socket, 4), 0);
    return address;
}

void cleared(const Observation& observation)
{
    REQUIRE(observation.retained != nullptr);
    std::lock_guard lock(observation.retained->mutex);
    REQUIRE(!observation.retained->listen_callback_active);
    const Snapshot empty {};
    REQUIRE_EQ(std::memcmp(&observation.retained->incoming_peer_handshake,
                   &empty, sizeof(empty)),
        0);
}

} // namespace

TEST(peer_handshake_metadata_public_api_validates_buffers_and_context)
{
    Sockets sockets;
    const auto socket = sockets.create();
    unavailable(snapshot(socket));
    std::array<unsigned char, sizeof(Snapshot) + 8> bytes;
    bytes.fill(0xa5);
    int size = sizeof(bytes);
    REQUIRE_EQ(srt_getsockopt(socket, 0, SRTO_ROBOTWEAX_PEER_HANDSHAKE,
                   bytes.data(), &size),
        0);
    REQUIRE_EQ(size, static_cast<int>(sizeof(Snapshot)));
    for (std::size_t index = sizeof(Snapshot); index < bytes.size(); ++index)
        REQUIRE_EQ(bytes[index], 0xa5);
    bytes.fill(0xa5);
    size = sizeof(Snapshot) - 1;
    REQUIRE_EQ(srt_getsockflag(
                   socket, SRTO_ROBOTWEAX_PEER_HANDSHAKE, bytes.data(), &size),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);
    for (auto byte : bytes)
        REQUIRE_EQ(byte, 0xa5);
    REQUIRE_EQ(size, static_cast<int>(sizeof(Snapshot) - 1));
    size = sizeof(Snapshot);
    REQUIRE_EQ(
        srt_getsockflag(socket, SRTO_ROBOTWEAX_PEER_HANDSHAKE, nullptr, &size),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);
    REQUIRE_EQ(srt_getsockflag(socket, SRTO_ROBOTWEAX_PEER_HANDSHAKE,
                   bytes.data(), nullptr),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_ROBOTWEAX_PEER_HANDSHAKE,
                   bytes.data(), sizeof(Snapshot)),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVOP);
    const auto group = srt_create_group(SRT_GTYPE_BROADCAST);
    REQUIRE(group != SRT_INVALID_SOCK);
    sockets.handles.push_back(group);
    REQUIRE_EQ(srt_getsockflag(
                   group, SRTO_ROBOTWEAX_PEER_HANDSHAKE, bytes.data(), &size),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVOP);
    REQUIRE_EQ(srt_setsockflag(group, SRTO_ROBOTWEAX_PEER_HANDSHAKE,
                   bytes.data(), sizeof(Snapshot)),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVOP);
    REQUIRE_EQ(srt_close(socket), 0);
    REQUIRE_EQ(srt_getsockflag(
                   socket, SRTO_ROBOTWEAX_PEER_HANDSHAKE, bytes.data(), &size),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVSOCK);
}

TEST(
    peer_handshake_metadata_preserves_advertisements_and_negotiates_after_callback)
{
    Observation observation;
    Sockets sockets;
    const auto listener = sockets.create();
    const auto address = listen(listener, observation);
    // Reuse one listener with distinct peer versions/latencies/unknown flags.
    for (unsigned index = 0; index < 2; ++index) {
        observation = {};
        const auto caller = sockets.create();
        set_integer(caller, SRTO_CONNTIMEO, 1'000);
        sockaddr_in source = address;
        source.sin_port = 0;
        REQUIRE_EQ(srt_bind(caller, reinterpret_cast<const sockaddr*>(&source),
                       sizeof(source)),
            0);
        Incoming incoming {
            compat::SocketRegistry::instance().find(caller)->channel,
            {.srt_version = index == 0 ? 0x0001'0402U : 0x0001'0507U,
                .flags = live_flags | (index == 0 ? 0x8000'0000U : 0U),
                .receiver_tsbpd_delay_milliseconds =
                    static_cast<std::uint16_t>(111 + index),
                .sender_tsbpd_delay_milliseconds =
                    static_cast<std::uint16_t>(222 + index)}};
        incoming.channel->set_send_hook_for_testing(replace_request, &incoming);
        REQUIRE_EQ(
            srt_connect(caller, reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)),
            0);
        const auto accepted = srt_accept(listener, nullptr, nullptr);
        REQUIRE(accepted != SRT_INVALID_SOCK);
        sockets.handles.push_back(accepted);
        REQUIRE_EQ(observation.calls, 1);
        REQUIRE_EQ(observation.protocol, 5);
        REQUIRE_EQ(observation.peer_version, 0);
        REQUIRE_EQ(observation.local_receive_delay, 300);
        REQUIRE_EQ(observation.local_peer_delay, 400);
        REQUIRE(observation.read_only);
        REQUIRE_EQ(observation.before.available, 1U);
        REQUIRE_EQ(
            observation.before.srt_version, incoming.parameters.srt_version);
        REQUIRE_EQ(observation.before.flags, incoming.parameters.flags);
        REQUIRE_EQ(observation.before.receiver_delay_milliseconds, 111 + index);
        REQUIRE_EQ(observation.before.sender_delay_milliseconds, 222 + index);
        REQUIRE_EQ(std::memcmp(&observation.before, &observation.after,
                       sizeof(Snapshot)),
            0);
        REQUIRE_EQ(integer(accepted, SRTO_PEERVERSION),
            incoming.parameters.srt_version);
        REQUIRE_EQ(integer(accepted, SRTO_RCVLATENCY), 500);
        REQUIRE_EQ(integer(accepted, SRTO_PEERLATENCY), 600);
        REQUIRE_EQ(integer(caller, SRTO_RCVLATENCY), 600);
        REQUIRE_EQ(integer(caller, SRTO_PEERLATENCY), 500);
        unavailable(snapshot(accepted));
        unavailable(snapshot(caller));
        unavailable(snapshot(listener));
        cleared(observation);
        REQUIRE_EQ(srt_close(accepted), 0);
        REQUIRE_EQ(srt_close(caller), 0);
    }
}

TEST(peer_handshake_metadata_rejection_and_missing_extensions_clear_every_exit)
{
    Observation observation;
    Sockets sockets;
    const auto listener = sockets.create();
    const auto address = listen(listener, observation);
    for (unsigned variant = 0; variant < 5; ++variant) {
        observation = {};
        observation.exit = variant == 0 ? Exit::reject
            : variant == 1              ? Exit::exception
            : variant == 4              ? Exit::close
                                        : Exit::accept;
        const auto caller = sockets.create();
        set_integer(caller, SRTO_CONNTIMEO, 500);
        sockaddr_in source = address;
        source.sin_port = 0;
        REQUIRE_EQ(srt_bind(caller, reinterpret_cast<const sockaddr*>(&source),
                       sizeof(source)),
            0);
        Incoming incoming {
            compat::SocketRegistry::instance().find(caller)->channel,
            {.srt_version = 0x0001'0409U,
                .flags = variant == 1 ? 0U : 0xffff'ffffU,
                .receiver_tsbpd_delay_milliseconds = 123,
                .sender_tsbpd_delay_milliseconds = 456},
            variant == 2       ? Extension::missing
                : variant == 3 ? Extension::response
                               : Extension::request};
        incoming.channel->set_send_hook_for_testing(replace_request, &incoming);
        REQUIRE_EQ(
            srt_connect(caller, reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)),
            SRT_ERROR);
        if (variant == 4) {
            // Close sends no rejection; caller retries can create fresh children.
            REQUIRE_EQ(srt_close(listener), 0);
            REQUIRE(observation.calls >= 1);
        } else {
            REQUIRE_EQ(observation.calls, 1);
        }
        REQUIRE(observation.read_only);
        if (variant == 2 || variant == 3) {
            unavailable(observation.before);
            REQUIRE_EQ(srt_getrejectreason(caller), SRT_REJ_ROGUE);
        } else {
            REQUIRE_EQ(observation.before.available, 1U);
            REQUIRE_EQ(observation.before.srt_version,
                incoming.parameters.srt_version);
            REQUIRE_EQ(observation.before.flags, incoming.parameters.flags);
            REQUIRE_EQ(observation.before.receiver_delay_milliseconds, 123U);
            REQUIRE_EQ(observation.before.sender_delay_milliseconds, 456U);
        }
        REQUIRE_EQ(std::memcmp(&observation.before, &observation.after,
                       sizeof(Snapshot)),
            0);
        cleared(observation);
        if (variant != 4)
            unavailable(snapshot(listener));
        int size = sizeof(Snapshot);
        Snapshot value {};
        REQUIRE_EQ(srt_getsockflag(observation.handle,
                       SRTO_ROBOTWEAX_PEER_HANDSHAKE, &value, &size),
            SRT_ERROR);
        REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVSOCK);
        REQUIRE_EQ(srt_close(caller), 0);
    }
}
