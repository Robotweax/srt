// SPDX-License-Identifier: MIT
#include "test.hpp"
#include "compat/acp1_group_context.hpp"
#include "robotweax/srt/codec.hpp"
#include "robotweax/srt/session_authentication.hpp"
#include "srt/srt.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <string_view>
#include <vector>

using namespace robotweax::srt;
namespace {
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
        const auto socket = srt_create_socket();
        REQUIRE(socket != SRT_INVALID_SOCK);
        handles.push_back(socket);
        return socket;
    }
};
#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_ACP1
void set(SRTSOCKET socket, SRT_SOCKOPT option, int value)
{
    REQUIRE_EQ(srt_setsockflag(socket, option, &value, sizeof(value)), 0);
}
bool flag(SRTSOCKET socket, SRT_SOCKOPT option)
{
    bool result = true;
    int size = sizeof(result);
    REQUIRE_EQ(srt_getsockflag(socket, option, &result, &size), 0);
    REQUIRE_EQ(size, static_cast<int>(sizeof(result)));
    return result;
}
// Synthetic deterministic nonces permit independent Python hashlib/hmac KATs.
class Provider final : public CryptoProvider {
public:
    unsigned next_byte = 0, calls = 0, fail_call = 0;
    Error random_bytes(std::span<std::byte> out) noexcept override
    {
        for (auto& value : out)
            value = std::byte {static_cast<unsigned char>(next_byte++)};
        return Error::none;
    }
    Error pbkdf2_hmac_sha1(std::span<const std::byte> pass,
        std::span<const std::byte> salt, std::uint32_t iterations,
        std::span<std::byte> out) noexcept override
    {
        return default_crypto_provider().pbkdf2_hmac_sha1(
            pass, salt, iterations, out);
    }
    Error hmac_sha256(std::span<const std::byte> key,
        std::span<const std::byte> in,
        std::span<std::byte, 32> out) noexcept override
    {
        if (++calls == fail_call) {
            secure_erase(out);
            return Error::cryptographic_failure;
        }
        return default_crypto_provider().hmac_sha256(key, in, out);
    }
    Error wrap_key(std::span<const std::byte>, std::span<const std::byte>,
        std::span<std::byte>, std::size_t& written) noexcept override
    {
        written = 0;
        return Error::unsupported;
    }
    Error unwrap_key(std::span<const std::byte>, std::span<const std::byte>,
        std::span<std::byte>, std::size_t& written) noexcept override
    {
        written = 0;
        return Error::unsupported;
    }
    Error make_aes_ctr_cipher(std::span<const std::byte>,
        std::unique_ptr<PayloadCipher>& out) noexcept override
    {
        out.reset();
        return Error::unsupported;
    }
    bool supports_aes_gcm() const noexcept override
    {
        return false;
    }
    Error make_aes_gcm_cipher(std::span<const std::byte>,
        std::unique_ptr<AuthenticatedPayloadCipher>& out) noexcept override
    {
        out.reset();
        return Error::unsupported;
    }
    void secure_erase(std::span<std::byte> out) noexcept override
    {
        default_crypto_provider().secure_erase(out);
    }
};
void establish(SessionAuthentication& caller, SessionAuthentication& listener)
{
    constexpr std::string_view secret = "acp1-test-password";
    const auto bytes = std::as_bytes(std::span {secret.data(), secret.size()});
    REQUIRE_EQ(caller.start(true, acp1_session_version), Error::none);
    REQUIRE_EQ(listener.start(false, acp1_session_version), Error::none);
    REQUIRE_EQ(
        listener.establish(bytes, caller.parameters(), 102, 101), Error::none);
    REQUIRE_EQ(
        caller.establish(bytes, listener.parameters(), 101, 102), Error::none);
}
std::vector<std::byte> control(ControlType type = ControlType::keepalive,
    std::span<const std::byte> payload = {})
{
    std::array<std::byte, 1500> bytes {};
    const auto encoded = encode_packet({.kind = PacketKind::control,
                                           .control = {.type = type,
                                               .timestamp = PacketTimestamp {7},
                                               .destination_socket_id = 102},
                                           .payload = payload},
        bytes);
    REQUIRE(encoded);
    return {bytes.begin(),
        bytes.begin() + static_cast<std::ptrdiff_t>(encoded.bytes_written)};
}
std::vector<std::byte> seal(
    SessionAuthentication& sender, std::span<const std::byte> bytes)
{
    std::array<std::byte, 1500> output {};
    std::size_t size = 0;
    REQUIRE_EQ(sender.seal_control(bytes, output, size), Error::none);
    return {output.begin(), output.begin() + static_cast<std::ptrdiff_t>(size)};
}
bool open(SessionAuthentication& receiver, std::span<const std::byte> wire)
{
    const auto decoded = decode_packet(wire);
    if (!decoded)
        return false;
    PacketView inner;
    return receiver.open_control(decoded.packet, inner);
}
#endif
}

TEST(acp1_build_gate_and_private_option_contract)
{
    Sockets sockets;
    const auto socket = sockets.create();
    const int enabled = 1;
#ifndef ROBOTWEAX_SRT_EXPERIMENTAL_ACP1
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_ROBOTWEAX_ACP1_REQUIRED, &enabled,
                   sizeof(enabled)),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVOP);
    auto* config = srt_create_config();
    REQUIRE(config != nullptr);
    const auto added = srt_config_add(
        config, SRTO_ROBOTWEAX_ACP1_REQUIRED, &enabled, sizeof(enabled));
    srt_delete_config(config);
    REQUIRE_EQ(added, SRT_ERROR);
    bool active = true;
    int size = sizeof(active);
    REQUIRE_EQ(
        srt_getsockflag(socket, SRTO_ROBOTWEAX_ACP1_ACTIVE, &active, &size),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVOP);
#else
    REQUIRE(!flag(socket, SRTO_ROBOTWEAX_ACP1_REQUIRED));
    REQUIRE(!flag(socket, SRTO_ROBOTWEAX_ACP1_ACTIVE));
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_ROBOTWEAX_ACP1_ACTIVE, &enabled,
                   sizeof(enabled)),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVOP);
    set(socket, SRTO_ROBOTWEAX_ACP1_REQUIRED, 1);
    const int too_small = 128;
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_MSS, &too_small, sizeof(too_small)),
        SRT_ERROR);
    const auto small_mss = sockets.create();
    set(small_mss, SRTO_MSS, too_small);
    REQUIRE_EQ(srt_setsockflag(small_mss, SRTO_ROBOTWEAX_ACP1_REQUIRED,
                   &enabled, sizeof(enabled)),
        SRT_ERROR);
    set(socket, SRTO_GROUPCONNECT, 1);
    set(socket, SRTO_RENDEZVOUS, 1);
    set(socket, SRTO_ROBOTWEAX_SESSIONAUTH, 1);
    const int disabled = 0;
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_ROBOTWEAX_ACP1_REQUIRED, &disabled,
                   sizeof(disabled)),
        SRT_ERROR);
    for (auto option : {SRTO_GROUPCONNECT}) {
        const auto other = sockets.create();
        set(other, option, 1);
        REQUIRE_EQ(srt_setsockflag(other, SRTO_ROBOTWEAX_ACP1_REQUIRED,
                       &enabled, sizeof(enabled)),
            0);
    }
    const auto group = srt_create_group(SRT_GTYPE_BROADCAST);
    REQUIRE(group != SRT_INVALID_SOCK);
    sockets.handles.push_back(group);
    REQUIRE_EQ(srt_setsockflag(group, SRTO_ROBOTWEAX_ACP1_REQUIRED, &enabled,
                   sizeof(enabled)),
        0);
    REQUIRE(flag(group, SRTO_ROBOTWEAX_ACP1_REQUIRED));
    REQUIRE(!flag(group, SRTO_ROBOTWEAX_ACP1_ACTIVE));
    REQUIRE(!flag(socket, SRTO_ROBOTWEAX_ACP1_ACTIVE));
#endif
}

#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_ACP1
TEST(acp1_wire_known_answer_and_every_byte_tamper)
{
    Provider provider;
    SessionAuthentication caller(provider), listener(provider);
    establish(caller, listener);
    const auto wire = seal(caller, control());
    constexpr std::string_view expected =
        "ffff7f0500000000000000070000006600000001000000000000000180010000000000"
        "00000000070000006600000000"
        "87070f95120a921336a49ed6402f98f8f7b177c4235f3b874fccf27fae863a3d";
    constexpr char digits[] = "0123456789abcdef";
    std::string hex;
    for (auto byte : wire) {
        hex += digits[std::to_integer<unsigned>(byte) >> 4];
        hex += digits[std::to_integer<unsigned>(byte) & 15];
    }
    REQUIRE_EQ(hex, expected);
    for (std::size_t i = 0; i < wire.size(); ++i) {
        auto changed = wire;
        changed[i] ^= std::byte {1};
        REQUIRE(!open(listener, changed));
    }
    for (std::size_t size = 0; size < wire.size(); size += 4)
        REQUIRE(!open(listener, std::span {wire}.first(size)));
    REQUIRE(!open(caller, wire)); // reflection
    SessionAuthentication foreign_caller(provider), foreign_listener(provider);
    establish(foreign_caller, foreign_listener);
    REQUIRE(!open(foreign_listener, wire));
    REQUIRE(open(listener, wire)); // failed opens did not advance the window
    REQUIRE(!open(listener, wire));
    const auto reply = seal(listener, control(ControlType::shutdown));
    REQUIRE(open(caller, reply));
}
TEST(acp1_replay_window_boundary_and_forged_future_counter)
{
    Provider provider;
    SessionAuthentication caller(provider), listener(provider);
    establish(caller, listener);
    std::vector<std::vector<std::byte>> wires;
    for (unsigned i = 0; i < 130; ++i)
        wires.push_back(seal(caller, control()));
    REQUIRE(open(listener, wires[63])); // counter 64
    REQUIRE(open(listener, wires[0])); // age 63
    REQUIRE(!open(listener, wires[0]));
    auto forged = wires[64];
    std::fill(forged.begin() + 20, forged.begin() + 28, std::byte {0xff});
    REQUIRE(!open(listener, forged));
    REQUIRE(open(listener, wires[64])); // forged counter did not advance state
    REQUIRE(open(listener, wires[129]));
    REQUIRE(!open(listener, wires[65])); // age 64
    REQUIRE(open(listener, wires[66])); // age 63
    REQUIRE(!open(
        listener, wires[63])); // permanently retired after window eviction
}
TEST(acp1_profile_binding_bounds_and_provider_failures)
{
    Provider provider;
    SessionAuthentication caller(provider), listener(provider),
        legacy(provider);
    establish(caller, listener);
    REQUIRE_EQ(legacy.start(false), Error::none);
    constexpr std::string_view secret = "acp1-test-password";
    REQUIRE_EQ(legacy.establish(
                   std::as_bytes(std::span {secret.data(), secret.size()}),
                   caller.parameters(), 102, 101),
        Error::invalid_key_material);
    SessionAuthenticationParameters proof;
    const std::array material {
        std::byte {1}, std::byte {2}, std::byte {3}, std::byte {4}};
    REQUIRE_EQ(caller.handshake_proof(true, material, proof), Error::none);
    REQUIRE(listener.verify_handshake(true, material, proof));
    proof.version = 2;
    REQUIRE(!listener.verify_handshake(true, material, proof));
    std::array<std::byte, 1500> output {};
    std::size_t written = 99;
    REQUIRE_EQ(
        legacy.seal_control(control(), output, written), Error::invalid_state);
    REQUIRE_EQ(written, 0U);
    REQUIRE(
        caller.seal_control(control(), std::span {output}.first(79), written)
        != Error::none);
    REQUIRE_EQ(written, 0U);
    provider.fail_call = provider.calls + 1;
    REQUIRE(caller.seal_control(control(), output, written) != Error::none);
    REQUIRE_EQ(written, 0U);
    provider.fail_call = 0;
    const auto valid = seal(caller, control());
    REQUIRE_EQ(
        valid[27], std::byte {1}); // failures did not consume a send counter
    provider.fail_call = provider.calls + 1;
    REQUIRE(!open(listener, valid));
    provider.fail_call = 0;
    REQUIRE(open(listener, valid));
    std::vector<std::byte> body(1424, std::byte {1});
    REQUIRE_EQ(
        seal(caller, control(ControlType::negative_acknowledgement, body))
            .size(),
        1500U);
    body.resize(1428);
    REQUIRE(caller.seal_control(
                control(ControlType::negative_acknowledgement, body), output,
                written)
        != Error::none);
    REQUIRE_EQ(written, 0U);
    auto nested = valid;
    REQUIRE(caller.seal_control(nested, output, written) != Error::none);
    auto handshake = control(ControlType::handshake, material);
    REQUIRE(caller.seal_control(handshake, output, written) != Error::none);
    for (unsigned failure : {1U, 2U}) {
        Provider failing;
        failing.fail_call = failure;
        SessionAuthentication offer(failing), response(failing);
        REQUIRE_EQ(offer.start(true, 3), Error::none);
        REQUIRE_EQ(response.start(false, 3), Error::none);
        REQUIRE(response.establish(
                    std::as_bytes(std::span {secret.data(), secret.size()}),
                    offer.parameters(), 102, 101)
            != Error::none);
        REQUIRE(!response.ready());
    }
}
TEST(acp1_strict_negotiation_rejects_legacy_and_missing_dependencies)
{
    Sockets sockets;
    constexpr std::string_view secret = "acp1-public-test-password";
    for (bool protected_listener : {true, false}) {
        for (bool legacy_session : {true, false}) {
            const auto listener = sockets.create(), caller = sockets.create();
            for (const auto socket : {listener, caller}) {
                REQUIRE_EQ(srt_setsockflag(socket, SRTO_PASSPHRASE,
                               secret.data(), static_cast<int>(secret.size())),
                    0);
                const bool protected_socket =
                    (socket == listener) == protected_listener;
                set(socket, SRTO_ROBOTWEAX_SESSIONAUTH,
                    protected_socket || legacy_session ? 1 : 0);
                set(socket, SRTO_ROBOTWEAX_ACP1_REQUIRED,
                    protected_socket ? 1 : 0);
                set(socket, SRTO_CONNTIMEO, 500);
            }
            sockaddr_in address {};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            REQUIRE_EQ(srt_bind(listener, reinterpret_cast<sockaddr*>(&address),
                           sizeof(address)),
                0);
            int size = sizeof(address);
            REQUIRE_EQ(srt_getsockname(listener,
                           reinterpret_cast<sockaddr*>(&address), &size),
                0);
            REQUIRE_EQ(srt_listen(listener, 1), 0);
            REQUIRE_EQ(
                srt_connect(caller, reinterpret_cast<sockaddr*>(&address),
                    sizeof(address)),
                SRT_ERROR);
            REQUIRE(!flag(caller, SRTO_ROBOTWEAX_ACP1_ACTIVE));
            REQUIRE_EQ(srt_close(listener), 0);
            REQUIRE_EQ(srt_close(caller), 0);
        }
    }
    const auto incomplete = sockets.create();
    set(incomplete, SRTO_ROBOTWEAX_ACP1_REQUIRED, 1);
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(9);
    REQUIRE_EQ(srt_connect(incomplete, reinterpret_cast<sockaddr*>(&address),
                   sizeof(address)),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ESECFAIL);
    REQUIRE(!flag(incomplete, SRTO_ROBOTWEAX_ACP1_ACTIVE));
}

TEST(acp1_file_stream_transfer_and_authenticated_remote_close)
{
    Sockets sockets;
    constexpr std::string_view secret = "acp1-file-stream-password";
    const auto listener = sockets.create(), caller = sockets.create();
    for (auto socket : {listener, caller}) {
        set(socket, SRTO_TRANSTYPE, SRTT_FILE);
        set(socket, SRTO_MESSAGEAPI, 0);
        set(socket, SRTO_ROBOTWEAX_SESSIONAUTH, 1);
        set(socket, SRTO_ROBOTWEAX_ACP1_REQUIRED, 1);
        set(socket, SRTO_RCVTIMEO, 3000);
        set(socket, SRTO_SNDTIMEO, 3000);
        REQUIRE_EQ(srt_setsockflag(socket, SRTO_PASSPHRASE, secret.data(),
                       static_cast<int>(secret.size())),
            0);
    }
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE_EQ(srt_bind(listener, reinterpret_cast<sockaddr*>(&address),
                   sizeof(address)),
        0);
    int size = sizeof(address);
    REQUIRE_EQ(
        srt_getsockname(listener, reinterpret_cast<sockaddr*>(&address), &size),
        0);
    REQUIRE_EQ(srt_listen(listener, 1), 0);
    REQUIRE_EQ(
        srt_connect(caller, reinterpret_cast<sockaddr*>(&address), size), 0);
    const auto accepted = srt_accept(listener, nullptr, nullptr);
    REQUIRE(accepted != SRT_INVALID_SOCK);
    sockets.handles.push_back(accepted);
    REQUIRE(flag(caller, SRTO_ROBOTWEAX_ACP1_ACTIVE));
    REQUIRE(flag(accepted, SRTO_ROBOTWEAX_ACP1_ACTIVE));
    const std::array<char, 4096> payload {};
    for (auto sender : {caller, accepted}) {
        const auto receiver = sender == caller ? accepted : caller;
        REQUIRE_EQ(srt_send(sender, payload.data(), payload.size()),
            static_cast<int>(payload.size()));
        std::array<char, 4096> received;
        received.fill('x');
        std::size_t read = 0;
        while (read < received.size()) {
            const auto count = srt_recv(receiver, received.data() + read,
                static_cast<int>(received.size() - read));
            REQUIRE(count > 0);
            read += static_cast<std::size_t>(count);
        }
        REQUIRE_EQ(received, payload);
    }
    REQUIRE_EQ(srt_close(caller), 0);
    std::array<char, 1> remaining {};
    REQUIRE(srt_recv(accepted, remaining.data(), remaining.size()) <= 0);
    REQUIRE(!flag(accepted, SRTO_ROBOTWEAX_ACP1_ACTIVE));
}

TEST(acp1_rendezvous_cookie_binding_known_answer_and_profile_separation)
{
    constexpr std::string_view secret = "acp1-test-password";
    const auto pass = std::as_bytes(std::span {secret.data(), secret.size()});
    Provider provider;
    SessionAuthentication initiator(provider), responder(provider);
    REQUIRE_EQ(initiator.start(true, 4), Error::none);
    REQUIRE_EQ(responder.start(false, 4), Error::none);
    REQUIRE_EQ(responder.establish(pass, initiator.parameters(), 102, 101),
        Error::invalid_key_material);
    REQUIRE_EQ(
        responder.establish(pass, initiator.parameters(), 102, 101, 100, 100),
        Error::invalid_key_material);
    REQUIRE_EQ(
        responder.establish(pass, initiator.parameters(), 102, 101, 100, 200),
        Error::none);
    REQUIRE_EQ(
        initiator.establish(pass, responder.parameters(), 101, 102, 200, 100),
        Error::none);
    const auto wire = seal(initiator, control());
    constexpr std::string_view expected_tag =
        "86b248221679c32fa6e8b25ca2a1f99a53310181263781c9c37f5209d844ba3e";
    constexpr char digits[] = "0123456789abcdef";
    std::string tag;
    for (auto byte : std::span {wire}.last(32)) {
        tag += digits[std::to_integer<unsigned>(byte) >> 4];
        tag += digits[std::to_integer<unsigned>(byte) & 15];
    }
    REQUIRE_EQ(tag, expected_tag);
    REQUIRE(open(responder, wire));
    REQUIRE(!open(responder, wire));
    REQUIRE(!open(initiator, wire));
    for (auto cookies : {std::pair {201U, 100U}, std::pair {200U, 101U}}) {
        Provider changed;
        SessionAuthentication other_initiator(changed),
            other_responder(changed);
        REQUIRE_EQ(other_initiator.start(true, 4), Error::none);
        REQUIRE_EQ(other_responder.start(false, 4), Error::none);
        REQUIRE_EQ(other_responder.establish(pass, other_initiator.parameters(),
                       102, 101, cookies.second, cookies.first),
            Error::none);
        REQUIRE_EQ(other_initiator.establish(pass, other_responder.parameters(),
                       101, 102, cookies.first, cookies.second),
            Error::none);
        REQUIRE(!open(
            other_responder, wire)); // identical nonces/IDs, changed cookie
    }
    Provider legacy_provider;
    SessionAuthentication legacy_initiator(legacy_provider),
        legacy_responder(legacy_provider);
    establish(
        legacy_initiator, legacy_responder); // identical nonces/IDs, version 3
    REQUIRE(!open(legacy_responder, wire));
}
TEST(acp1_groups_context_known_answer_tamper_and_cross_member_replay)
{
    using namespace robotweax::srt::compat;
    constexpr std::string_view secret = "acp1-test-password";
    const auto pass = std::as_bytes(std::span {secret.data(), secret.size()});
    PathIdentifier path {};
    path.size = 3;
    path.bytes[0] = std::byte {'o'};
    path.bytes[1] = std::byte {'n'};
    path.bytes[2] = std::byte {'e'};
    StandbyRttIdentity standby {};
    standby.caller.fill(std::byte {1});
    standby.listener.fill(std::byte {2});
    const auto context =
        acp1_group_context({0x40000001, GroupType::backup, 0, 7},
            {0x40000002, GroupType::backup, 0, 7}, path, true, standby);
    Provider provider;
    SessionAuthentication caller(provider), listener(provider);
    REQUIRE_EQ(caller.start(true, 5), Error::none);
    REQUIRE_EQ(listener.start(false, 5), Error::none);
    REQUIRE_EQ(listener.establish(pass, caller.parameters(), 102, 101),
        Error::invalid_key_material);
    REQUIRE_EQ(
        listener.establish(pass, caller.parameters(), 102, 101, 0, 0, context),
        Error::none);
    REQUIRE_EQ(
        caller.establish(pass, listener.parameters(), 101, 102, 0, 0, context),
        Error::none);
    const auto wire = seal(caller, control());
    constexpr std::string_view expected_tag =
        "3ed2f4f8b4c00ddd5f6b1216fc112ae7529ac6ba6ec5453458f7a1a0825c4206";
    constexpr char digits[] = "0123456789abcdef";
    std::string tag;
    for (const auto byte : std::span {wire}.last(32)) {
        tag += digits[std::to_integer<unsigned>(byte) >> 4];
        tag += digits[std::to_integer<unsigned>(byte) & 15];
    }
    REQUIRE_EQ(tag, expected_tag);
    REQUIRE(open(listener, wire));
    REQUIRE(!open(listener, wire));
    SessionAuthenticationParameters proof;
    constexpr std::array<std::byte, 4> initial {};
    REQUIRE_EQ(listener.handshake_proof(false, initial, proof), Error::none);
    REQUIRE(caller.verify_handshake(false, initial, proof));
    for (std::size_t i = 0; i < context.size(); ++i) {
        auto changed = context;
        changed[i] ^= std::byte {1};
        Provider other_provider;
        SessionAuthentication other_caller(other_provider),
            other_listener(other_provider);
        REQUIRE_EQ(other_caller.start(true, 5), Error::none);
        REQUIRE_EQ(other_listener.start(false, 5), Error::none);
        REQUIRE_EQ(other_listener.establish(pass, other_caller.parameters(),
                       102, 101, 0, 0, changed),
            Error::none);
        REQUIRE_EQ(other_caller.establish(pass, other_listener.parameters(),
                       101, 102, 0, 0, changed),
            Error::none);
        REQUIRE(!open(other_listener, wire));
        REQUIRE(!other_caller.verify_handshake(false, initial, proof));
    }
    Provider replacement_provider;
    replacement_provider.next_byte = 64;
    SessionAuthentication replacement_caller(replacement_provider),
        replacement_listener(replacement_provider);
    REQUIRE_EQ(replacement_caller.start(true, 5), Error::none);
    REQUIRE_EQ(replacement_listener.start(false, 5), Error::none);
    REQUIRE_EQ(replacement_listener.establish(pass,
                   replacement_caller.parameters(), 102, 101, 0, 0, context),
        Error::none);
    REQUIRE_EQ(replacement_caller.establish(pass,
                   replacement_listener.parameters(), 101, 102, 0, 0, context),
        Error::none);
    REQUIRE(!open(replacement_listener, wire));
}
#endif
