// SPDX-License-Identifier: MIT
#include "test.hpp"
#include "robotweax/srt/codec.hpp"
#include "robotweax/srt/handshake_datagram.hpp"
#include "robotweax/srt/rendezvous.hpp"
#include "compat/socket_registry.hpp"
#include "srt/srt.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <exception>
#include <memory>
#include <string_view>
#include <thread>
#include <vector>

#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_ACP1
using namespace robotweax::srt;
namespace {
HandshakeMessage sent(const HandshakeActions& actions)
{
    for (std::size_t i = 0; i < actions.size; ++i) {
        if (actions.values[i].kind != HandshakeActionKind::send)
            continue;
        std::array<std::byte, 1500> bytes {};
        const auto encoded = encode_handshake_datagram(
            actions.values[i], PacketTimestamp {7}, 0, bytes);
        REQUIRE(encoded);
        const auto decoded = decode_handshake_datagram(
            std::span {bytes}.first(encoded.bytes_written));
        REQUIRE(decoded);
        return decoded.message;
    }
    throw std::runtime_error("no handshake send action");
}
bool connected(const HandshakeActions& actions)
{
    for (std::size_t i = 0; i < actions.size; ++i)
        if (actions.values[i].kind == HandshakeActionKind::connected)
            return true;
    return false;
}
struct Exchange {
    SessionAuthentication initiator {default_crypto_provider()},
        responder {default_crypto_provider()};
    RendezvousHandshakeMachine first {{.local_socket_id = 101,
        .local_cookie = 200,
        .require_session_authentication = true}};
    RendezvousHandshakeMachine second {{.local_socket_id = 102,
        .local_cookie = 100,
        .require_session_authentication = true}};
    HandshakeMessage request, response, agreement;
    KeyMaterialBuffer material {};
    Exchange()
    {
        constexpr std::string_view secret = "acp1-rendezvous-machine-fixture";
        const auto pass =
            std::as_bytes(std::span {secret.data(), secret.size()});
        CryptoSession key_generator {{.passphrase = secret, .key_length = 16}};
        REQUIRE_EQ(key_generator.start_initiator(), Error::none);
        const auto initial = key_generator.pending_key_material();
        std::copy(initial.begin(), initial.end(), material.bytes.begin());
        material.size = initial.size();
        const auto first_wave = sent(first.start()),
                   second_wave = sent(second.start());
        REQUIRE(!first_wave.has_session_authentication
            && !second_wave.has_session_authentication);
        REQUIRE_EQ(initiator.start(true, 4), Error::none);
        REQUIRE_EQ(responder.start(false, 4), Error::none);
        REQUIRE_EQ(first.set_key_material_request(
                       material, encryption_field_for_key_length(16)),
            Error::none);
        REQUIRE_EQ(first.set_session_authentication(initiator.parameters()),
            Error::none);
        request = sent(first.receive(second_wave));
        const auto empty = sent(second.receive(first_wave));
        REQUIRE(!empty.has_session_authentication);
        REQUIRE_EQ(responder.establish(pass, request.session_authentication,
                       102, 101, 100, 200),
            Error::none);
        SessionAuthenticationParameters server, confirmation;
        REQUIRE_EQ(responder.handshake_proof(false, material.view(), server),
            Error::none);
        REQUIRE_EQ(
            responder.handshake_proof(true, material.view(), confirmation),
            Error::none);
        REQUIRE_EQ(second.set_session_authentication(server, confirmation),
            Error::none);
        response = sent(second.receive(request));
        REQUIRE_EQ(initiator.establish(pass, response.session_authentication,
                       101, 102, 200, 100),
            Error::none);
        REQUIRE(initiator.verify_handshake(
            false, material.view(), response.session_authentication));
        SessionAuthenticationParameters client;
        REQUIRE_EQ(initiator.handshake_proof(true, material.view(), client),
            Error::none);
        REQUIRE_EQ(first.set_session_authentication(
                       client, response.session_authentication),
            Error::none);
    }
    HandshakeMessage complete_initiator()
    {
        const auto actions = first.receive(response);
        REQUIRE(connected(actions));
        return sent(actions);
    }
};
struct Sockets {
    std::vector<SRTSOCKET> handles;
    Sockets()
    {
        REQUIRE_EQ(srt_startup(), 0);
    }
    ~Sockets()
    {
        for (auto handle : handles)
            (void)srt_close(handle);
        (void)srt_cleanup();
    }
    SRTSOCKET create()
    {
        const auto handle = srt_create_socket();
        REQUIRE(handle != SRT_INVALID_SOCK);
        handles.push_back(handle);
        return handle;
    }
};
void set(SRTSOCKET socket, SRT_SOCKOPT option, int value)
{
    REQUIRE_EQ(srt_setsockflag(socket, option, &value, sizeof(value)), 0);
}
bool active(SRTSOCKET socket)
{
    bool value = true;
    int size = sizeof(value);
    REQUIRE_EQ(
        srt_getsockflag(socket, SRTO_ROBOTWEAX_ACP1_ACTIVE, &value, &size), 0);
    return value;
}
void bind_loopback(
    SRTSOCKET socket, bool ipv6, sockaddr_storage& address, int& size)
{
    size = sizeof(address);
    if (ipv6) {
        set(socket, SRTO_IPV6ONLY, 1);
        sockaddr_in6 value {};
        value.sin6_family = AF_INET6;
        value.sin6_addr = in6addr_loopback;
        REQUIRE_EQ(srt_bind(socket, reinterpret_cast<sockaddr*>(&value),
                       sizeof(value)),
            0);
    } else {
        sockaddr_in value {};
        value.sin_family = AF_INET;
        value.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        REQUIRE_EQ(srt_bind(socket, reinterpret_cast<sockaddr*>(&value),
                       sizeof(value)),
            0);
    }
    REQUIRE_EQ(
        srt_getsockname(socket, reinterpret_cast<sockaddr*>(&address), &size),
        0);
}
}
TEST(acp1_rendezvous_requires_final_proof_and_recovers_lost_agreement)
{
    Exchange exchange;
    REQUIRE_EQ(exchange.first.role(), RendezvousRole::initiator);
    REQUIRE_EQ(exchange.second.role(), RendezvousRole::responder);
    REQUIRE(!connected(exchange.second.note_connected_packet()));
    REQUIRE_EQ(exchange.second.state(), RendezvousState::initiated);
    auto agreement = exchange.complete_initiator();
    REQUIRE(agreement.has_session_authentication);
    REQUIRE_EQ(agreement.session_authentication.version, 4U);
    // First final proof is lost. HSRSP retry reaches the connected Initiator.
    const auto response = sent(exchange.second.timeout());
    REQUIRE(response.session_authentication.matches(
        exchange.response.session_authentication));
    agreement = sent(exchange.first.receive(response));
    REQUIRE(exchange.responder.verify_handshake(
        true, exchange.material.view(), agreement.session_authentication));
    REQUIRE(connected(exchange.second.receive(agreement)));
    REQUIRE_EQ(exchange.second.state(), RendezvousState::connected);
    REQUIRE(!connected(exchange.second.receive(agreement)));
}
TEST(
    acp1_rendezvous_machine_rejects_missing_tampered_reflected_and_foreign_proofs)
{
    for (unsigned attack = 0; attack < 6; ++attack) {
        Exchange exchange;
        auto agreement = exchange.complete_initiator();
        switch (attack) {
        case 0:
            agreement.has_session_authentication = false;
            break;
        case 1:
            agreement.session_authentication.proof[0] ^= std::byte {1};
            break;
        case 2:
            agreement.session_authentication.version = 3;
            break;
        case 3:
            agreement.session_authentication =
                exchange.response.session_authentication;
            break;
        case 4: {
            Exchange foreign;
            agreement.session_authentication =
                foreign.complete_initiator().session_authentication;
            break;
        }
        case 5:
            ++agreement.packet.syn_cookie;
            break;
        }
        REQUIRE(!connected(exchange.second.receive(agreement)));
        REQUIRE(exchange.second.state() != RendezvousState::connected);
    }
    for (unsigned attack = 0; attack < 3; ++attack) {
        Exchange exchange;
        auto response = exchange.response;
        if (attack == 0)
            response.has_session_authentication = false;
        if (attack == 1)
            response.session_authentication.proof[0] ^= std::byte {1};
        if (attack == 2)
            response.session_authentication.version = 3;
        REQUIRE(!connected(exchange.first.receive(response)));
    }
}
TEST(acp1_rendezvous_public_rotates_ctr_gcm_ipv4_ipv6_sync_async)
{
    Sockets sockets;
    for (bool ipv6 : {false, true}) {
        for (bool asynchronous : {false, true}) {
            for (int mode : {1, 2}) {
#ifndef ENABLE_AEAD_API_PREVIEW
                if (mode == 2)
                    continue;
#endif
                const std::array handles {sockets.create(), sockets.create()};
                std::array<sockaddr_storage, 2> addresses {};
                std::array<int, 2> sizes {};
                for (unsigned i = 0; i < 2; ++i) {
                    // Exercise both supported setting orders around Rendezvous.
                    if (i == 0)
                        set(handles[i], SRTO_RENDEZVOUS, 1);
                    set(handles[i], SRTO_ROBOTWEAX_ACP1_REQUIRED, 1);
                    set(handles[i], SRTO_ROBOTWEAX_SESSIONAUTH, 1);
                    if (i == 1)
                        set(handles[i], SRTO_RENDEZVOUS, 1);
                    constexpr std::string_view secret =
                        "acp1-rendezvous-public-fixture";
                    REQUIRE_EQ(srt_setsockflag(handles[i], SRTO_PASSPHRASE,
                                   secret.data(), secret.size()),
                        0);
                    set(handles[i], SRTO_CONNTIMEO, 300);
                    set(handles[i], SRTO_RCVTIMEO, 3000);
                    set(handles[i], SRTO_SNDTIMEO, 3000);
                    set(handles[i], SRTO_LATENCY, 0);
                    set(handles[i], SRTO_KMPREANNOUNCE, 1);
                    set(handles[i], SRTO_KMREFRESHRATE, 8);
#ifdef ENABLE_AEAD_API_PREVIEW
                    set(handles[i], SRTO_CRYPTOMODE, mode);
#endif
                    if (asynchronous)
                        set(handles[i], SRTO_RCVSYN, 0);
                    bind_loopback(handles[i], ipv6, addresses[i], sizes[i]);
                    REQUIRE(!active(handles[i]));
                }
                int left = SRT_ERROR, right = SRT_ERROR;
                std::thread first([&] {
                    left = srt_connect(handles[0],
                        reinterpret_cast<sockaddr*>(&addresses[1]), sizes[1]);
                });
                std::thread second([&] {
                    right = srt_connect(handles[1],
                        reinterpret_cast<sockaddr*>(&addresses[0]), sizes[0]);
                });
                first.join();
                second.join();
                REQUIRE_EQ(left, 0);
                REQUIRE_EQ(right, 0);
                if (asynchronous) {
                    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::seconds {3};
                    while (
                        (srt_getsockstate(handles[0]) == SRTS_CONNECTING
                            || srt_getsockstate(handles[1]) == SRTS_CONNECTING)
                        && std::chrono::steady_clock::now() < deadline)
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds {1});
                    for (auto handle : handles)
                        set(handle, SRTO_RCVSYN, 1);
                }
                for (auto handle : handles) {
                    REQUIRE_EQ(srt_getsockstate(handle), SRTS_CONNECTED);
                    REQUIRE(active(handle));
                    const auto record =
                        compat::SocketRegistry::instance().find(handle);
                    REQUIRE(record != nullptr);
                    REQUIRE_EQ(record->crypto->session_authentication()
                                   ->parameters()
                                   .version,
                        4U);
                }
                for (int message = 0; message < 48; ++message) {
                    for (unsigned i = 0; i < 2; ++i) {
                        std::array<char, 256> payload;
                        payload.fill(static_cast<char>(message));
                        REQUIRE_EQ(srt_sendmsg(handles[i], payload.data(),
                                       payload.size(), -1, 1),
                            static_cast<int>(payload.size()));
                        std::array<char, 1500> received {};
                        REQUIRE_EQ(srt_recvmsg(handles[1 - i], received.data(),
                                       received.size()),
                            static_cast<int>(payload.size()));
                        REQUIRE(std::equal(
                            payload.begin(), payload.end(), received.begin()));
                    }
                }
                for (auto handle : handles)
                    REQUIRE_EQ(srt_close(handle), 0);
            }
        }
    }
}
TEST(acp1_rendezvous_public_rejects_unprotected_peer_and_wrong_secret)
{
    Sockets sockets;
    for (unsigned scenario = 0; scenario < 3; ++scenario) {
        const std::array handles {sockets.create(), sockets.create()};
        std::array<sockaddr_storage, 2> addresses {};
        std::array<int, 2> sizes {};
        for (unsigned i = 0; i < 2; ++i) {
            const bool protected_peer = scenario == 2 || i == scenario;
            if (protected_peer) {
                set(handles[i], SRTO_ROBOTWEAX_ACP1_REQUIRED, 1);
                set(handles[i], SRTO_ROBOTWEAX_SESSIONAUTH, 1);
            }
            set(handles[i], SRTO_RENDEZVOUS, 1);
            const std::string_view secret = scenario == 2 && i == 1
                ? "wrong-acp1-rendezvous-password"
                : "acp1-rendezvous-public-fixture";
            REQUIRE_EQ(srt_setsockflag(handles[i], SRTO_PASSPHRASE,
                           secret.data(), secret.size()),
                0);
            set(handles[i], SRTO_CONNTIMEO, 75);
            bind_loopback(handles[i], false, addresses[i], sizes[i]);
        }
        int left = 0, right = 0;
        std::thread first([&] {
            left = srt_connect(handles[0],
                reinterpret_cast<sockaddr*>(&addresses[1]), sizes[1]);
        });
        std::thread second([&] {
            right = srt_connect(handles[1],
                reinterpret_cast<sockaddr*>(&addresses[0]), sizes[0]);
        });
        first.join();
        second.join();
        REQUIRE_EQ(left, SRT_ERROR);
        REQUIRE_EQ(right, SRT_ERROR);
        for (auto handle : handles) {
            REQUIRE(srt_getsockstate(handle) != SRTS_CONNECTED);
            REQUIRE(!active(handle));
            REQUIRE_EQ(srt_close(handle), 0);
        }
    }
}

namespace {
struct ProofFaultState {
    std::atomic_bool allow_final_proof {false}, probes_sent {false};
    std::atomic_int initiator {-1};
    std::atomic_uint dropped {0};
};
struct ProofFault {
    std::shared_ptr<compat::DatagramChannel> channel;
    std::shared_ptr<compat::SocketRecord> record;
    std::shared_ptr<ProofFaultState> state;
    int index = 0;
    ~ProofFault()
    {
        if (channel)
            channel->set_send_hook_for_testing(nullptr, nullptr);
    }
};
UdpIoResult hold_final_proof(
    std::span<const std::byte> bytes, IpEndpoint peer, void* context) noexcept
{
    auto& fault = *static_cast<ProofFault*>(context);
    const auto decoded = decode_handshake_datagram(bytes);
    if (decoded && decoded.message.packet.request == HandshakeRequest::agreement
        && decoded.message.has_session_authentication
        && !fault.state->allow_final_proof.load()) {
        int unresolved = -1;
        fault.state->initiator.compare_exchange_strong(unresolved, fault.index);
        if (fault.state->initiator.load() == fault.index) {
            if (fault.state->dropped.fetch_add(1) == 0) {
                std::array<std::byte, 1500> control {}, protected_control {},
                    data {};
                const auto keepalive = encode_packet(
                    {.kind = PacketKind::control,
                        .control = {.type = ControlType::keepalive,
                            .destination_socket_id =
                                decoded.control.destination_socket_id}},
                    control);
                if (!keepalive)
                    return {.error = Error::io_error};
                const auto clear =
                    std::span {control}.first(keepalive.bytes_written);
                if (!fault.channel->socket.send_to(clear, peer))
                    return {.error = Error::io_error};
                std::size_t written = 0;
                // At this instant the Initiator's setup owner still serializes
                // its CryptoSession; runtime promotion follows this callback.
                if (fault.record->crypto->session_authentication()
                            ->seal_control(clear, protected_control, written)
                        != Error::none
                    || !fault.channel->socket.send_to(
                        std::span {protected_control}.first(written), peer))
                    return {.error = Error::io_error};
                const std::array<std::byte, 4> payload {};
                const auto encoded_data = encode_packet(
                    {.kind = PacketKind::data,
                        .data = {.sequence =
                                     decoded.message.packet.initial_sequence,
                            .message_number = 1,
                            .boundary = MessageBoundary::solo,
                            .destination_socket_id =
                                decoded.control.destination_socket_id},
                        .payload = payload},
                    data);
                if (!encoded_data
                    || !fault.channel->socket.send_to(
                        std::span {data}.first(encoded_data.bytes_written),
                        peer))
                    return {.error = Error::io_error};
                fault.state->probes_sent.store(true);
            }
            return {.bytes_transferred = bytes.size()};
        }
    }
    return fault.channel->socket.send_to(bytes, peer);
}
}
TEST(acp1_rendezvous_public_requires_final_proof_despite_signed_early_controls)
{
    Sockets sockets;
    const std::array handles {sockets.create(), sockets.create()};
    std::array<sockaddr_storage, 2> addresses {};
    std::array<int, 2> sizes {};
    const auto state = std::make_shared<ProofFaultState>();
    std::array<ProofFault, 2> faults;
    for (int i = 0; i < 2; ++i) {
        set(handles[i], SRTO_ROBOTWEAX_ACP1_REQUIRED, 1);
        set(handles[i], SRTO_ROBOTWEAX_SESSIONAUTH, 1);
        set(handles[i], SRTO_RENDEZVOUS, 1);
        set(handles[i], SRTO_RCVSYN, 0);
        set(handles[i], SRTO_CONNTIMEO, 300);
        constexpr std::string_view secret = "acp1-rendezvous-held-proof";
        REQUIRE_EQ(srt_setsockflag(handles[i], SRTO_PASSPHRASE, secret.data(),
                       secret.size()),
            0);
        bind_loopback(handles[i], false, addresses[i], sizes[i]);
        faults[i].record = compat::SocketRegistry::instance().find(handles[i]);
        faults[i].channel = faults[i].record->channel;
        faults[i].index = i;
        faults[i].state = state;
        faults[i].channel->set_send_hook_for_testing(
            hold_final_proof, &faults[i]);
    }
    for (int i = 0; i < 2; ++i)
        REQUIRE_EQ(
            srt_connect(handles[i],
                reinterpret_cast<sockaddr*>(&addresses[1 - i]), sizes[1 - i]),
            0);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds {2};
    while (!state->probes_sent.load()
        && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds {1});
    REQUIRE(state->probes_sent.load());
    const auto initiator = state->initiator.load();
    REQUIRE(initiator == 0 || initiator == 1);
    // Leave enough time for all three early probes to traverse the real UDP
    // route and pending handshake inbox; none may publish the Responder.
    std::this_thread::sleep_for(std::chrono::milliseconds {40});
    REQUIRE_EQ(srt_getsockstate(handles[1 - initiator]), SRTS_CONNECTING);
    REQUIRE(!active(handles[1 - initiator]));
    REQUIRE(active(handles[initiator]));
    state->allow_final_proof.store(true);
    while (!active(handles[1 - initiator])
        && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds {1});
    REQUIRE(active(handles[1 - initiator]));
    REQUIRE_EQ(srt_getsockstate(handles[1 - initiator]), SRTS_CONNECTED);
    REQUIRE(state->dropped.load() > 0);
}

TEST(acp1_rendezvous_file_stream_rotates_ctr_and_gcm)
{
    Sockets sockets;
    for (int mode : {1, 2}) {
#ifndef ENABLE_AEAD_API_PREVIEW
        if (mode == 2)
            continue;
#endif
        const std::array handles {sockets.create(), sockets.create()};
        std::array<sockaddr_storage, 2> addresses {};
        std::array<int, 2> sizes {};
        for (unsigned i = 0; i < 2; ++i) {
            set(handles[i], SRTO_TRANSTYPE, SRTT_FILE);
            set(handles[i], SRTO_MESSAGEAPI, 0);
            set(handles[i], SRTO_ROBOTWEAX_ACP1_REQUIRED, 1);
            set(handles[i], SRTO_ROBOTWEAX_SESSIONAUTH, 1);
            set(handles[i], SRTO_RENDEZVOUS, 1);
            set(handles[i], SRTO_CONNTIMEO, 300);
            set(handles[i], SRTO_RCVTIMEO, 3000);
            set(handles[i], SRTO_SNDTIMEO, 3000);
            set(handles[i], SRTO_KMPREANNOUNCE, 1);
            set(handles[i], SRTO_KMREFRESHRATE, 8);
#ifdef ENABLE_AEAD_API_PREVIEW
            set(handles[i], SRTO_CRYPTOMODE, mode);
#endif
            constexpr std::string_view secret =
                "acp1-rendezvous-stream-fixture";
            REQUIRE_EQ(srt_setsockflag(handles[i], SRTO_PASSPHRASE,
                           secret.data(), secret.size()),
                0);
            bind_loopback(handles[i], false, addresses[i], sizes[i]);
        }
        int left = SRT_ERROR, right = SRT_ERROR;
        std::thread first([&] {
            left = srt_connect(handles[0],
                reinterpret_cast<sockaddr*>(&addresses[1]), sizes[1]);
        });
        std::thread second([&] {
            right = srt_connect(handles[1],
                reinterpret_cast<sockaddr*>(&addresses[0]), sizes[0]);
        });
        first.join();
        second.join();
        REQUIRE_EQ(left, 0);
        REQUIRE_EQ(right, 0);
        for (unsigned message = 0; message < 16; ++message) {
            for (unsigned i = 0; i < 2; ++i) {
                std::array<char, 4096> payload;
                payload.fill(static_cast<char>(message + 1));
                REQUIRE_EQ(srt_send(handles[i], payload.data(), payload.size()),
                    static_cast<int>(payload.size()));
                std::array<char, 4096> received {};
                std::size_t read = 0;
                while (read < received.size()) {
                    const auto bytes = srt_recv(handles[1 - i],
                        received.data() + read, received.size() - read);
                    REQUIRE(bytes > 0);
                    read += static_cast<std::size_t>(bytes);
                }
                REQUIRE_EQ(received, payload);
            }
        }
        for (auto handle : handles)
            REQUIRE(active(handle));
        for (auto handle : handles)
            REQUIRE_EQ(srt_close(handle), 0);
    }
}
TEST(acp1_rendezvous_reordered_response_keeps_retry_offer_canonical)
{
    Exchange exchange;
    // Simulate an embedding whose first incoming conclusion already carries a
    // verified HSRSP. The wire machine still needs another CONCLUSION exchange.
    RendezvousHandshakeMachine early {{.local_socket_id = 101,
        .local_cookie = 200,
        .require_session_authentication = true}};
    (void)early.start();
    REQUIRE_EQ(early.set_key_material_request(
                   exchange.material, encryption_field_for_key_length(16)),
        Error::none);
    const auto final = exchange.complete_initiator();
    REQUIRE_EQ(early.set_session_authentication(final.session_authentication,
                   exchange.response.session_authentication),
        Error::none);
    const auto retry = sent(early.receive(exchange.response));
    REQUIRE_EQ(early.state(), RendezvousState::fine);
    REQUIRE_EQ(retry.extension_type, HandshakeExtensionType::handshake_request);
    REQUIRE(retry.session_authentication.is_offer());
    REQUIRE_EQ(retry.session_authentication.version, 4U);
    REQUIRE_EQ(retry.session_authentication.caller_nonce,
        final.session_authentication.caller_nonce);
    REQUIRE(connected(early.receive(exchange.response)));
}

namespace {
struct MssFault {
    std::shared_ptr<compat::DatagramChannel> channel;
    ~MssFault()
    {
        if (channel)
            channel->set_send_hook_for_testing(nullptr, nullptr);
    }
};
UdpIoResult lower_authenticated_handshake_mss(
    std::span<const std::byte> bytes, IpEndpoint peer, void* context) noexcept
{
    auto& fault = *static_cast<MssFault*>(context);
    const auto decoded = decode_handshake_datagram(bytes);
    if (!decoded)
        return fault.channel->socket.send_to(bytes, peer);
    std::array<std::byte, 1500> altered {};
    if (bytes.size() > altered.size())
        return {.error = Error::io_error};
    std::copy(bytes.begin(), bytes.end(), altered.begin());
    // MSS is outside the handshake proof; a valid proof must not bypass the
    // transport staging policy when an on-path actor rewrites this field.
    std::fill(altered.begin() + 28, altered.begin() + 32, std::byte {0});
    altered[31] = std::byte {128};
    return fault.channel->socket.send_to(
        std::span {altered}.first(bytes.size()), peer);
}
}
TEST(acp1_rendezvous_rejects_failed_transport_staging_before_publication)
{
    Sockets sockets;
    const std::array handles {sockets.create(), sockets.create()};
    std::array<sockaddr_storage, 2> addresses {};
    std::array<int, 2> sizes {};
    std::array<MssFault, 2> faults;
    for (unsigned i = 0; i < 2; ++i) {
        set(handles[i], SRTO_ROBOTWEAX_ACP1_REQUIRED, 1);
        set(handles[i], SRTO_ROBOTWEAX_SESSIONAUTH, 1);
        set(handles[i], SRTO_RENDEZVOUS, 1);
        set(handles[i], SRTO_CONNTIMEO, 100);
        constexpr std::string_view secret = "acp1-rendezvous-staging-fixture";
        REQUIRE_EQ(srt_setsockflag(handles[i], SRTO_PASSPHRASE, secret.data(),
                       secret.size()),
            0);
        bind_loopback(handles[i], false, addresses[i], sizes[i]);
        faults[i].channel =
            compat::SocketRegistry::instance().find(handles[i])->channel;
        faults[i].channel->set_send_hook_for_testing(
            lower_authenticated_handshake_mss, &faults[i]);
    }
    int left = 0, right = 0;
    std::thread first([&] {
        left = srt_connect(
            handles[0], reinterpret_cast<sockaddr*>(&addresses[1]), sizes[1]);
    });
    std::thread second([&] {
        right = srt_connect(
            handles[1], reinterpret_cast<sockaddr*>(&addresses[0]), sizes[0]);
    });
    first.join();
    second.join();
    REQUIRE_EQ(left, SRT_ERROR);
    REQUIRE_EQ(right, SRT_ERROR);
    for (auto handle : handles) {
        REQUIRE(srt_getsockstate(handle) != SRTS_CONNECTED);
        REQUIRE(!active(handle));
    }
}
#endif
