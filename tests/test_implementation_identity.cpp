#include "test.hpp"
#include "compat/socket_registry.hpp"
#include "robotweax/srt/handshake_datagram.hpp"
#include "robotweax/srt/implementation_identity.hpp"
#include "robotweax/srt/rendezvous.hpp"
#include "srt/access_control.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace robotweax::srt;
namespace {
using Snapshot = ROBOTWEAX_SRT_PEER_IDENTITY_V1;
static_assert(sizeof(Snapshot) == 132);
static_assert(offsetof(Snapshot, reserved) == 116);

std::uint32_t cookie(const Handshake&, void*) noexcept
{
    return 1234;
}
ImplementationIdentity identity()
{
    return make_implementation_identity("Robotweax SRT", "0.2.9");
}
HandshakeAction offer()
{
    HandshakeAction action;
    action.kind = HandshakeActionKind::send;
    action.packet.version = handshake_version_5;
    action.packet.request = HandshakeRequest::conclusion;
    action.packet.extension_field = handshake_extension_flag_handshake;
    action.has_handshake_extension = true;
    action.implementation_identity = identity();
    return action;
}
std::vector<std::byte> wire(const HandshakeAction& action)
{
    std::array<std::byte, 1500> bytes {};
    const auto encoded =
        encode_handshake_datagram(action, PacketTimestamp {7}, 0, bytes);
    REQUIRE(encoded);
    return {bytes.begin(),
        bytes.begin() + static_cast<std::ptrdiff_t>(encoded.bytes_written)};
}
HandshakeMessage message(const HandshakeAction& action)
{
    const auto bytes = wire(action);
    const auto decoded = decode_handshake_datagram(bytes);
    REQUIRE(decoded);
    return decoded.message;
}
HandshakeAction sent(const HandshakeActions& actions)
{
    for (std::size_t i = 0; i < actions.size; ++i)
        if (actions.values[i].kind == HandshakeActionKind::send)
            return actions.values[i];
    throw std::runtime_error("no send action");
}
struct Sockets {
    std::vector<SRTSOCKET> handles;
    Sockets()
    {
        REQUIRE_EQ(srt_startup(), 0);
    }
    ~Sockets()
    {
        for (auto socket : handles)
            (void)srt_close(socket);
        (void)srt_cleanup();
    }
    SRTSOCKET create()
    {
        auto socket = srt_create_socket();
        REQUIRE(socket != SRT_INVALID_SOCK);
        handles.push_back(socket);
        return socket;
    }
};
void set(SRTSOCKET socket, SRT_SOCKOPT option, std::int32_t value)
{
    REQUIRE_EQ(srt_setsockflag(socket, option, &value, sizeof(value)), 0);
}
Snapshot snapshot(SRTSOCKET socket)
{
    Snapshot value;
    std::memset(&value, 0xff, sizeof(value));
    int size = sizeof(value);
    REQUIRE_EQ(
        srt_getsockflag(socket, SRTO_ROBOTWEAX_PEER_IDENTITY, &value, &size),
        0);
    REQUIRE_EQ(size, static_cast<int>(sizeof(value)));
    REQUIRE_EQ(value.abi_version, ROBOTWEAX_SRT_PEER_IDENTITY_VERSION);
    for (auto reserved : value.reserved)
        REQUIRE_EQ(reserved, 0U);
    for (auto padding : value.padding)
        REQUIRE_EQ(padding, 0U);
    return value;
}
void check(const Snapshot& value, int policy)
{
    const auto expected = policy == ROBOTWEAX_SRT_IDENTITY_OFF ? 0U
        : policy == ROBOTWEAX_SRT_IDENTITY_NAME
        ? ROBOTWEAX_SRT_PEER_IDENTITY_NAME
        : ROBOTWEAX_SRT_PEER_IDENTITY_NAME
            | ROBOTWEAX_SRT_PEER_IDENTITY_RELEASE;
    REQUIRE_EQ(value.available_fields, expected);
    REQUIRE_EQ(std::string(value.name), policy == 0 ? "" : "Robotweax SRT");
    REQUIRE_EQ(std::string(value.release),
        policy == 2 ? ROBOTWEAX_SRT_VERSION_STRING : "");
    REQUIRE_EQ(value.name_length, std::strlen(value.name));
    REQUIRE_EQ(value.release_length, std::strlen(value.release));
}
struct Observation {
    Snapshot incoming {};
    std::shared_ptr<compat::SocketRecord> retained;
    SRTSOCKET handle = SRT_INVALID_SOCK;
    int exit = 0;
    int calls = 0;
};
int observe(void* context, SRTSOCKET socket, int, const sockaddr*, const char*)
{
    auto& observation = *static_cast<Observation*>(context);
    ++observation.calls;
    observation.incoming = snapshot(socket);
    observation.handle = socket;
    observation.retained = compat::SocketRegistry::instance().find(socket);
    if (observation.exit == 1)
        return -1;
    if (observation.exit == 2)
        throw std::runtime_error("reject admission");
    if (observation.exit == 3) {
        (void)srt_close(socket);
        return 0;
    }
    return 0;
}
sockaddr_in listen(SRTSOCKET socket, Observation& observation)
{
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
struct Mutation {
    std::shared_ptr<compat::DatagramChannel> channel;
    unsigned variant = 0;
    unsigned calls = 0;
    ~Mutation()
    {
        channel->set_send_hook_for_testing(nullptr, nullptr);
    }
};
UdpIoResult mutate(
    std::span<const std::byte> bytes, IpEndpoint peer, void* context) noexcept
{
    auto& mutation = *static_cast<Mutation*>(context);
    const auto decoded = decode_handshake_datagram(bytes);
    if (!decoded
        || decoded.message.packet.request != HandshakeRequest::conclusion
        || !decoded.message.implementation_identity.available())
        return mutation.channel->socket.send_to(bytes, peer);
    std::array<std::byte, 1500> altered {};
    std::copy(bytes.begin(), bytes.end(), altered.begin());
    std::size_t size = bytes.size();
    // Plain live HSREQ: 64-byte prefix, 16-byte HSREQ, then 36-byte identity.
    if (size != 116U)
        return {.error = Error::invalid_extension};
    ++mutation.calls;
    if (mutation.variant == 0)
        altered[84] = std::byte {'X'};
    if (mutation.variant == 1)
        altered[88] = std::byte {2};
    if (mutation.variant == 2) {
        std::copy_n(altered.begin() + 80, 36,
            altered.begin() + static_cast<std::ptrdiff_t>(size));
        size += 36;
    }
    if (mutation.variant == 3)
        altered[23] &= std::byte {0xfb};
    if (mutation.variant == 4)
        altered[90] = std::byte {255};
    if (mutation.variant == 5)
        altered[115] = std::byte {1};
    return mutation.channel->socket.send_to(
        std::span {altered}.first(size), peer);
}
}

TEST(implementation_identity_wire_vector_and_bounds)
{
    std::array<std::byte, 112> bytes {};
    const auto result = encode_implementation_identity(identity(), bytes);
    REQUIRE(result);
    REQUIRE_EQ(result.bytes_written, 36U);
    const std::array<std::byte, 16> prefix {std::byte {0x7f}, std::byte {0x13},
        std::byte {0}, std::byte {8}, std::byte {'S'}, std::byte {'R'},
        std::byte {'T'}, std::byte {'I'}, std::byte {1}, std::byte {0},
        std::byte {13}, std::byte {5}, std::byte {0}, std::byte {0},
        std::byte {0}, std::byte {0}};
    REQUIRE(std::equal(prefix.begin(), prefix.end(), bytes.begin()));
    REQUIRE_EQ(std::memcmp(bytes.data() + 16, "Robotweax SRT0.2.9", 18), 0);
    REQUIRE_EQ(bytes[34], std::byte {0});
    REQUIRE_EQ(bytes[35], std::byte {0});
    REQUIRE(decode_implementation_identity(std::span {bytes}.subspan(4, 32))
        == identity());
    const auto maximum = make_implementation_identity(
        std::string(32, 'N'), std::string(64, 'R'));
    const auto largest = encode_implementation_identity(maximum, bytes);
    REQUIRE(largest);
    REQUIRE_EQ(largest.bytes_written, bytes.size());
    REQUIRE(decode_implementation_identity(std::span {bytes}.subspan(4))
        == maximum);
    REQUIRE(
        !make_implementation_identity(std::string(33, 'N'), "1").available());
    REQUIRE(!make_implementation_identity("Name", std::string(65, 'R'))
            .available());
    REQUIRE(!make_implementation_identity("", "1").available());
    REQUIRE(make_implementation_identity("Name", "").available());
    for (auto text :
        {"bad\nname", " bad", "bad ", "bad/name", "bad\x1b", "\xc3\xa9"})
        REQUIRE(!make_implementation_identity(text, "1").available());
}

TEST(implementation_identity_rejects_invalid_content_without_protocol_failure)
{
    std::array<std::byte, 36> record {};
    REQUIRE(encode_implementation_identity(identity(), record));
    const auto content = std::span {record}.subspan(4);
    for (std::size_t length = 0; length < content.size(); ++length)
        REQUIRE(
            !decode_implementation_identity(content.first(length)).available());
    for (auto offset :
        {0U, 4U, 5U, 6U, 7U, 8U, 9U, 10U, 11U, 12U, 25U, 30U, 31U}) {
        auto bad = record;
        bad[4 + offset] =
            offset == 12U || offset == 25U ? std::byte {0} : std::byte {255};
        REQUIRE(!decode_implementation_identity(std::span {bad}.subspan(4))
                .available());
    }
    auto extended = std::vector<std::byte>(content.begin(), content.end());
    extended.resize(36);
    REQUIRE(!decode_implementation_identity(extended).available());
}

TEST(implementation_identity_datagram_order_duplicates_and_optional_budget)
{
    const auto action = offer();
    auto bytes = wire(action);
    REQUIRE_EQ(bytes.size(), 116U);
    REQUIRE(message(action).implementation_identity == identity());
    // Identity before mandatory HSREQ is equally valid.
    auto reordered = bytes;
    std::copy_n(bytes.begin() + 80, 36, reordered.begin() + 64);
    std::copy_n(bytes.begin() + 64, 16, reordered.begin() + 100);
    const auto decoded = decode_handshake_datagram(reordered);
    REQUIRE(decoded);
    REQUIRE(decoded.message.implementation_identity == identity());
    const auto duplicate =
        std::vector<std::byte>(bytes.begin() + 80, bytes.end());
    bytes.insert(bytes.end(), duplicate.begin(), duplicate.end());
    const auto duplicates = decode_handshake_datagram(bytes);
    REQUIRE(duplicates);
    REQUIRE(!duplicates.message.implementation_identity.available());
    bytes = wire(action);
    bytes[23] &= std::byte {0xfb};
    const auto unadvertised = decode_handshake_datagram(bytes);
    REQUIRE(unadvertised);
    REQUIRE(!unadvertised.message.implementation_identity.available());
    auto disabled = action;
    disabled.implementation_identity = {};
    const auto core = wire(disabled);
    std::array<std::byte, 80> small {};
    const auto omitted =
        encode_handshake_datagram(action, PacketTimestamp {7}, 0, small);
    REQUIRE(omitted);
    REQUIRE_EQ(omitted.bytes_written, core.size());
    REQUIRE(std::equal(core.begin(), core.end(), small.begin()));
    auto low_mss = action;
    low_mss.packet.maximum_transmission_unit = 128;
    REQUIRE_EQ(wire(low_mss).size(), 80U);
    low_mss.packet.maximum_transmission_unit = 164;
    REQUIRE_EQ(wire(low_mss).size(), 116U);
    auto crowded = action;
    crowded.has_stream_id_extension = true;
    crowded.packet.extension_field |= handshake_extension_flag_config;
    REQUIRE(crowded.stream_id.assign(std::string(512, 'S')));
    crowded.implementation_identity = make_implementation_identity(
        std::string(32, 'N'), std::string(64, 'R'));
    crowded.packet.maximum_transmission_unit = 644;
    REQUIRE_EQ(wire(crowded).size(), 596U);
    REQUIRE(!message(crowded).implementation_identity.available());
    REQUIRE_EQ(message(crowded).stream_id.view().size(), 512U);
    crowded.packet.maximum_transmission_unit = 756;
    REQUIRE_EQ(wire(crowded).size(), 708U);
    REQUIRE(message(crowded).implementation_identity
        == crowded.implementation_identity);
    auto agreement = action;
    agreement.packet.request = HandshakeRequest::agreement;
    REQUIRE(!message(agreement).implementation_identity.available());
    bytes = wire(action);
    bytes.pop_back();
    REQUIRE(!decode_handshake_datagram(bytes));
}

TEST(implementation_identity_caller_listener_freezes_observations_and_retries)
{
    HandshakeMachine caller {{.role = ConnectionRole::caller,
        .local_socket_id = 10,
        .implementation_identity =
            make_implementation_identity("Caller", "1")}};
    HandshakeMachine listener {{.role = ConnectionRole::listener,
        .local_socket_id = 20,
        .cookie_generator = cookie,
        .implementation_identity =
            make_implementation_identity("Listener", "2")}};
    const auto induction = listener.receive(message(sent(caller.start())));
    const auto request = sent(caller.receive(message(sent(induction))));
    const auto retry = sent(caller.timeout());
    REQUIRE(wire(request) == wire(retry));
    const auto response = sent(listener.receive(message(request)));
    (void)caller.receive(message(response));
    REQUIRE_EQ(caller.state(), HandshakeState::connected);
    REQUIRE_EQ(listener.state(), HandshakeState::connected);
    REQUIRE(caller.peer_identity()
        == make_implementation_identity("Listener", "2"));
    REQUIRE(listener.peer_identity()
        == make_implementation_identity("Caller", "1"));
    auto changed = message(request);
    changed.implementation_identity =
        make_implementation_identity("Forged", "99");
    (void)listener.receive(changed);
    REQUIRE(listener.peer_identity()
        == make_implementation_identity("Caller", "1"));
}

TEST(implementation_identity_rendezvous_both_cookie_roles_and_empty_agreement)
{
    for (const bool swap : {false, true}) {
        RendezvousHandshakeMachine sender {{.local_socket_id = 10U,
            .local_cookie = swap ? 100U : 200U,
            .implementation_identity =
                make_implementation_identity("Sender", "1")}};
        RendezvousHandshakeMachine receiver {{.local_socket_id = 20U,
            .local_cookie = swap ? 200U : 100U,
            .implementation_identity =
                make_implementation_identity("Receiver", "2")}};
        auto& initiator = swap ? receiver : sender;
        auto& responder = swap ? sender : receiver;
        const auto expected_initiator = swap
            ? make_implementation_identity("Receiver", "2")
            : make_implementation_identity("Sender", "1");
        const auto a = initiator.start();
        const auto b = responder.start();
        const auto request = initiator.receive(message(sent(b)));
        (void)responder.receive(message(sent(a)));
        const auto response = responder.receive(message(sent(request)));
        const auto agreement = initiator.receive(message(sent(response)));
        (void)responder.receive(message(sent(agreement)));
        REQUIRE_EQ(sender.state(), RendezvousState::connected);
        REQUIRE_EQ(receiver.state(), RendezvousState::connected);
        REQUIRE(sender.peer_identity()
            == make_implementation_identity("Receiver", "2"));
        REQUIRE(receiver.peer_identity()
            == make_implementation_identity("Sender", "1"));
        auto late = message(sent(request));
        late.implementation_identity =
            make_implementation_identity("Forged", "99");
        (void)responder.receive(late);
        REQUIRE(responder.peer_identity() == expected_initiator);
    }
}

TEST(implementation_identity_public_api_buffers_stages_and_group_handles)
{
    Sockets sockets;
    const auto socket = sockets.create();
    check(snapshot(socket), 0);
    std::int32_t policy = -1;
    int size = sizeof(policy);
    REQUIRE_EQ(srt_getsockflag(socket, SRTO_ROBOTWEAX_IDENTITY_ADVERTISEMENT,
                   &policy, &size),
        0);
    REQUIRE_EQ(policy, ROBOTWEAX_SRT_IDENTITY_OFF);
    for (auto invalid : {-1, 3})
        REQUIRE_EQ(
            srt_setsockflag(socket, SRTO_ROBOTWEAX_IDENTITY_ADVERTISEMENT,
                &invalid, sizeof(invalid)),
            SRT_ERROR);
    std::array<unsigned char, sizeof(Snapshot) + 8> bytes;
    bytes.fill(0xa5);
    size = sizeof(Snapshot) - 1;
    REQUIRE_EQ(srt_getsockflag(
                   socket, SRTO_ROBOTWEAX_PEER_IDENTITY, bytes.data(), &size),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);
    REQUIRE(std::all_of(bytes.begin(), bytes.end(), [](auto b) {
        return b == 0xa5;
    }));
    size = sizeof(bytes);
    REQUIRE_EQ(srt_getsockflag(
                   socket, SRTO_ROBOTWEAX_PEER_IDENTITY, bytes.data(), &size),
        0);
    REQUIRE_EQ(size, static_cast<int>(sizeof(Snapshot)));
    for (std::size_t i = sizeof(Snapshot); i < bytes.size(); ++i)
        REQUIRE_EQ(bytes[i], 0xa5);
    REQUIRE_EQ(
        srt_getsockflag(socket, SRTO_ROBOTWEAX_PEER_IDENTITY, nullptr, &size),
        SRT_ERROR);
    REQUIRE_EQ(srt_getsockflag(
                   socket, SRTO_ROBOTWEAX_PEER_IDENTITY, bytes.data(), nullptr),
        SRT_ERROR);
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_ROBOTWEAX_PEER_IDENTITY,
                   bytes.data(), sizeof(Snapshot)),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVOP);
    const auto group = srt_create_group(SRT_GTYPE_BROADCAST);
    REQUIRE(group != SRT_INVALID_SOCK);
    sockets.handles.push_back(group);
    for (auto option :
        {SRTO_ROBOTWEAX_PEER_IDENTITY, SRTO_ROBOTWEAX_IDENTITY_ADVERTISEMENT}) {
        size = sizeof(bytes);
        REQUIRE_EQ(
            srt_getsockflag(group, option, bytes.data(), &size), SRT_ERROR);
        REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVOP);
        REQUIRE_EQ(
            srt_setsockflag(group, option, &policy, sizeof(policy)), SRT_ERROR);
        REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVOP);
    }
    REQUIRE_EQ(srt_close(socket), 0);
    size = sizeof(bytes);
    REQUIRE_EQ(srt_getsockflag(
                   socket, SRTO_ROBOTWEAX_PEER_IDENTITY, bytes.data(), &size),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVSOCK);
}

TEST(implementation_identity_public_admission_and_connected_policy_matrix)
{
    Sockets sockets;
    for (int listener_policy = 0; listener_policy <= 2; ++listener_policy)
        for (int caller_policy = 0; caller_policy <= 2; ++caller_policy) {
            Observation observation;
            const auto listener = sockets.create();
            set(listener, SRTO_ROBOTWEAX_IDENTITY_ADVERTISEMENT,
                listener_policy);
            const auto address = listen(listener, observation);
            const auto caller = sockets.create();
            set(caller, SRTO_ROBOTWEAX_IDENTITY_ADVERTISEMENT, caller_policy);
            set(caller, SRTO_CONNTIMEO, 1000);
            REQUIRE_EQ(
                srt_connect(caller, reinterpret_cast<const sockaddr*>(&address),
                    sizeof(address)),
                0);
            const auto accepted = srt_accept(listener, nullptr, nullptr);
            REQUIRE(accepted != SRT_INVALID_SOCK);
            sockets.handles.push_back(accepted);
            REQUIRE_EQ(observation.calls, 1);
            check(observation.incoming, caller_policy);
            check(snapshot(accepted), caller_policy);
            check(snapshot(caller), listener_policy);
            check(snapshot(listener), 0);
            REQUIRE_EQ(
                srt_setsockflag(caller, SRTO_ROBOTWEAX_IDENTITY_ADVERTISEMENT,
                    &caller_policy, sizeof(caller_policy)),
                SRT_ERROR);
            REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ECONNSOCK);
            REQUIRE_EQ(srt_close(caller), 0);
            check(snapshot(accepted), caller_policy);
            REQUIRE_EQ(srt_close(accepted), 0);
            REQUIRE_EQ(srt_close(listener), 0);
        }
}

TEST(
    implementation_identity_invalid_optional_advertisements_preserve_connection)
{
    Sockets sockets;
    for (unsigned variant = 0; variant < 6; ++variant) {
        Observation observation;
        const auto listener = sockets.create();
        const auto address = listen(listener, observation);
        const auto caller = sockets.create();
        set(caller, SRTO_ROBOTWEAX_IDENTITY_ADVERTISEMENT,
            ROBOTWEAX_SRT_IDENTITY_NAME_AND_RELEASE);
        set(caller, SRTO_CONNTIMEO, 1000);
        sockaddr_in source = address;
        source.sin_port = 0;
        REQUIRE_EQ(srt_bind(caller, reinterpret_cast<const sockaddr*>(&source),
                       sizeof(source)),
            0);
        Mutation mutation {
            compat::SocketRegistry::instance().find(caller)->channel, variant};
        mutation.channel->set_send_hook_for_testing(mutate, &mutation);
        REQUIRE_EQ(
            srt_connect(caller, reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)),
            0);
        const auto accepted = srt_accept(listener, nullptr, nullptr);
        REQUIRE(accepted != SRT_INVALID_SOCK);
        sockets.handles.push_back(accepted);
        REQUIRE(mutation.calls > 0);
        check(observation.incoming, 0);
        check(snapshot(accepted), 0);
        REQUIRE_EQ(srt_close(caller), 0);
        REQUIRE_EQ(srt_close(accepted), 0);
        REQUIRE_EQ(srt_close(listener), 0);
    }
}

TEST(implementation_identity_rejected_admission_clears_provisional_state)
{
    Sockets sockets;
    for (int exit = 1; exit <= 3; ++exit) {
        Observation observation;
        observation.exit = exit;
        const auto listener = sockets.create();
        const auto address = listen(listener, observation);
        const auto caller = sockets.create();
        set(caller, SRTO_ROBOTWEAX_IDENTITY_ADVERTISEMENT,
            ROBOTWEAX_SRT_IDENTITY_NAME_AND_RELEASE);
        set(caller, SRTO_CONNTIMEO, 300);
        REQUIRE_EQ(
            srt_connect(caller, reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)),
            SRT_ERROR);
        REQUIRE_EQ(srt_close(listener), 0);
        REQUIRE(observation.calls >= 1);
        check(observation.incoming, 2);
        REQUIRE(observation.retained != nullptr);
        {
            std::lock_guard lock(observation.retained->mutex);
            REQUIRE(!observation.retained->incoming_peer_identity.available());
            REQUIRE(!observation.retained->peer_identity.available());
        }
        REQUIRE_EQ(srt_close(caller), 0);
    }
}

TEST(
    implementation_identity_encrypted_snapshot_survives_authentication_confirmation)
{
    Sockets sockets;
    for (const int mode : {1, 2}) {
#ifndef ENABLE_AEAD_API_PREVIEW
        if (mode == 2)
            continue;
#endif
        for (const int auth : {0, 1}) {
            for (const int key : {16, 24, 32}) {
                Observation observation;
                const auto listener = sockets.create();
                const auto caller = sockets.create();
                for (const auto socket : {listener, caller}) {
                    constexpr char secret[] =
                        "synthetic-identity-qualification";
                    REQUIRE_EQ(srt_setsockflag(socket, SRTO_PASSPHRASE, secret,
                                   sizeof(secret) - 1),
                        0);
                    set(socket, SRTO_PBKEYLEN, key);
                    set(socket, SRTO_ROBOTWEAX_SESSIONAUTH, auth);
                    set(socket, SRTO_ROBOTWEAX_IDENTITY_ADVERTISEMENT,
                        ROBOTWEAX_SRT_IDENTITY_NAME_AND_RELEASE);
                    set(socket, SRTO_CONNTIMEO, 1000);
#ifdef ENABLE_AEAD_API_PREVIEW
                    set(socket, SRTO_CRYPTOMODE, mode);
#endif
                }
                const auto address = listen(listener, observation);
                REQUIRE_EQ(srt_connect(caller,
                               reinterpret_cast<const sockaddr*>(&address),
                               sizeof(address)),
                    0);
                const auto accepted = srt_accept(listener, nullptr, nullptr);
                REQUIRE(accepted != SRT_INVALID_SOCK);
                sockets.handles.push_back(accepted);
                check(observation.incoming, 2);
                check(snapshot(accepted), 2);
                check(snapshot(caller), 2);
                REQUIRE_EQ(srt_close(caller), 0);
                REQUIRE_EQ(srt_close(accepted), 0);
                REQUIRE_EQ(srt_close(listener), 0);
            }
        }
    }
}
