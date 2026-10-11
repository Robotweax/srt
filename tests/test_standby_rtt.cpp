#include "test.hpp"
#include "robotweax/srt/handshake_datagram.hpp"
#include "robotweax/srt/standby_rtt.hpp"

#include <algorithm>
#include <array>
#include <cstring>

using namespace robotweax::srt;

namespace {
StandbyRttNonce nonce(unsigned value)
{
    StandbyRttNonce n {};
    n[0] = static_cast<std::byte>(value);
    n[15] = static_cast<std::byte>(value + 1);
    return n;
}
StandbyRttIdentity identity()
{
    return {.caller = nonce(3), .listener = nonce(5)};
}
StandbyRttMessage echo(StandbyRttMessage m)
{
    m.kind = StandbyRttKind::echo;
    return m;
}
std::uint32_t cookie(const Handshake&, void*) noexcept
{
    return 123;
}
bool group(const GroupMembership& peer, GroupMembership& local, void*) noexcept
{
    local = peer;
    local.group_id = 0x40000002;
    return true;
}
HandshakeMachine::Configuration config(
    ConnectionRole role, bool enabled, bool auth = false)
{
    HandshakeMachine::Configuration c;
    c.role = role;
    c.local_socket_id = role == ConnectionRole::caller ? 10 : 20;
    c.cookie_generator = cookie;
    c.group_membership_negotiator = group;
    c.has_group_membership = role == ConnectionRole::caller;
    c.group_membership = {.group_id = 0x40000001, .type = GroupType::backup};
    c.require_standby_rtt = enabled;
    if (enabled)
        c.standby_rtt_nonce =
            role == ConnectionRole::caller ? nonce(3) : nonce(5);
    c.require_session_authentication = auth;
    if (auth) {
        c.session_authentication.caller_nonce[0] = std::byte {1};
        if (role == ConnectionRole::listener) {
            c.session_authentication.listener_nonce[0] = std::byte {2};
            c.session_authentication.proof[0] = std::byte {3};
            c.session_authentication_confirmation = c.session_authentication;
            c.session_authentication_confirmation.proof[0] = std::byte {4};
        }
    }
    return c;
}
HandshakeMessage wire(const HandshakeAction& action)
{
    std::array<std::byte, 1500> bytes {};
    const auto encoded = encode_handshake_datagram(action, {}, 0, bytes);
    REQUIRE(encoded);
    const auto decoded = decode_handshake_datagram(
        std::span {bytes}.first(encoded.bytes_written));
    REQUIRE(decoded);
    return decoded.message;
}
struct Peers {
    HandshakeMachine caller, listener;
    Peers(bool c = true, bool l = true, bool auth = false)
        : caller(config(ConnectionRole::caller, c, auth))
        , listener(config(ConnectionRole::listener, l, auth))
    {
    }
    HandshakeMessage offer()
    {
        const auto first = caller.start();
        REQUIRE_EQ(first.size, 2U);
        const auto induction = listener.receive(wire(first.values[0]));
        REQUIRE_EQ(induction.size, 2U);
        const auto conclusion = caller.receive(wire(induction.values[0]));
        REQUIRE_EQ(conclusion.size, 2U);
        return wire(conclusion.values[0]);
    }
};
StandbyRttMessage begin(StandbyRttMeasurement& m, std::uint64_t now = 100)
{
    const auto request = m.prepare_request(now, 20'000'000, 9, true);
    REQUIRE(request);
    return *request;
}
}

TEST(standby_rtt_codec_fixed_network_order_and_exact_shape)
{
    StandbyRttMessage m {
        .identity = identity(), .probe_id = 0x0102030405060708};
    std::array<std::byte, 65> bytes {};
    const auto written = encode_standby_rtt_datagram(
        m, PacketTimestamp {0xfffffff0}, 0x11223344, bytes);
    REQUIRE(written);
    REQUIRE_EQ(written.bytes_written, 64U);
    const std::array<unsigned char, 64> golden {0xff, 0xff, 0x7f, 0x12, 0, 0, 0,
        0, 0xff, 0xff, 0xff, 0xf0, 0x11, 0x22, 0x33, 0x44, 0, 0, 0, 1, 0, 0, 0,
        1, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 5, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 6, 1, 2, 3, 4, 5, 6, 7, 8};
    REQUIRE_EQ(std::memcmp(bytes.data(), golden.data(), 64), 0);
    const auto decoded =
        decode_standby_rtt_datagram(std::span {bytes}.first(64));
    REQUIRE(decoded);
    REQUIRE_EQ(decoded.message.identity, identity());
    REQUIRE_EQ(decoded.message.probe_id, m.probe_id);
    REQUIRE_EQ(decoded.destination_socket_id, 0x11223344U);
    REQUIRE(!decode_standby_rtt_datagram(bytes));
    for (std::size_t length = 0; length < 64; ++length)
        REQUIRE(!decode_standby_rtt_datagram(std::span {bytes}.first(length)));
    for (std::size_t offset : {3U, 7U, 19U, 23U}) {
        auto corrupt = bytes;
        corrupt[offset] = std::byte {9};
        REQUIRE(!decode_standby_rtt_datagram(std::span {corrupt}.first(64)));
    }
    auto corrupt = bytes;
    std::fill(corrupt.begin() + 56, corrupt.begin() + 64, std::byte {});
    REQUIRE(!decode_standby_rtt_datagram(std::span {corrupt}.first(64)));
    m.identity.listener = {};
    REQUIRE(!encode_standby_rtt_datagram(m, {}, 0, bytes));
    m.identity = identity();
    m.probe_id = 0;
    REQUIRE(!encode_standby_rtt_datagram(m, {}, 0, bytes));
    m.probe_id = 1;
    REQUIRE(
        !encode_standby_rtt_datagram(m, {}, 0, std::span {bytes}.first(63)));
    m.kind = StandbyRttKind::echo;
    REQUIRE(encode_standby_rtt_datagram(m, {}, 0, bytes));
    REQUIRE_EQ(
        decode_standby_rtt_datagram(std::span {bytes}.first(64)).message.kind,
        StandbyRttKind::echo);
}

TEST(standby_rtt_handshake_codec_rejects_duplicate_reserved_version_and_mtu)
{
    std::array<std::byte, 44> extension {};
    REQUIRE(encode_standby_rtt_identity(identity(), extension));
    REQUIRE_EQ(std::to_integer<unsigned>(extension[0]), 0x7fU);
    REQUIRE_EQ(std::to_integer<unsigned>(extension[1]), 0x12U);
    for (std::size_t offset : {7U, 11U}) {
        auto invalid = extension;
        invalid[offset] = std::byte {2};
        const auto view = decode_extension(invalid);
        REQUIRE(view);
        REQUIRE(!decode_standby_rtt_identity(view.extension));
    }
    REQUIRE(!encode_standby_rtt_identity({}, extension));
    auto equal = identity();
    equal.listener = equal.caller;
    REQUIRE(!encode_standby_rtt_identity(equal, extension));
    Peers peers;
    const auto offered = peers.offer();
    HandshakeAction action {.kind = HandshakeActionKind::send,
        .packet = offered.packet,
        .has_handshake_extension = true,
        .has_group_membership = true,
        .group_membership = offered.group_membership,
        .has_standby_rtt = true,
        .standby_rtt = offered.standby_rtt};
    std::array<std::byte, 1500> bytes {};
    const auto result = encode_handshake_datagram(action, {}, 0, bytes);
    REQUIRE(result);
    std::copy_n(bytes.begin() + result.bytes_written - 44, 44,
        bytes.begin() + result.bytes_written);
    REQUIRE(!decode_handshake_datagram(
        std::span {bytes}.first(result.bytes_written + 44)));
    action.packet.extension_field &= ~handshake_extension_flag_config;
    REQUIRE(!encode_handshake_datagram(action, {}, 0, bytes));
    action.packet.extension_field |= handshake_extension_flag_config;
    action.packet.maximum_transmission_unit = 100;
    REQUIRE(!encode_handshake_datagram(action, {}, 0, bytes));
}

TEST(standby_rtt_handshake_bilateral_offer_pair_retry_and_replay)
{
    Peers peers;
    auto offer = peers.offer();
    REQUIRE(offer.standby_rtt.is_offer());
    REQUIRE_EQ(
        wire(peers.caller.timeout().values[0]).standby_rtt, offer.standby_rtt);
    const auto response = peers.listener.receive(offer);
    REQUIRE_EQ(response.size, 2U);
    const auto reply = wire(response.values[0]);
    REQUIRE_EQ(reply.standby_rtt, identity());
    REQUIRE_EQ(peers.caller.receive(reply).size, 1U);
    REQUIRE_EQ(peers.caller.state(), HandshakeState::connected);
    REQUIRE_EQ(peers.caller.standby_rtt_identity(),
        peers.listener.standby_rtt_identity());
    REQUIRE_EQ(
        wire(peers.listener.receive(offer).values[0]).standby_rtt, identity());
    offer.standby_rtt.caller[0] = std::byte {99};
    REQUIRE_EQ(peers.listener.receive(offer).size, 0U);
}

TEST(standby_rtt_handshake_no_downgrade_unsolicited_or_wrong_identity)
{
    for (const auto mode :
        {std::array {true, false}, std::array {false, true}}) {
        Peers peers(mode[0], mode[1]);
        const auto response = peers.listener.receive(peers.offer());
        REQUIRE_EQ(peers.listener.state(), HandshakeState::rejected);
        REQUIRE_EQ(response.values[1].rejection_reason, 15);
    }
    for (int defect = 0; defect < 5; ++defect) {
        Peers peers;
        auto offer = peers.offer();
        auto reply = wire(peers.listener.receive(offer).values[0]);
        if (defect == 0)
            reply.has_standby_rtt = false;
        if (defect == 1)
            reply.standby_rtt.caller = nonce(9);
        if (defect == 2)
            reply.standby_rtt.listener = {};
        if (defect == 3)
            reply.standby_rtt.listener = reply.standby_rtt.caller;
        if (defect == 4)
            reply.extension_parameters.flags = 0;
        REQUIRE_EQ(peers.caller.receive(reply).values[0].rejection_reason, 15);
        REQUIRE_EQ(peers.caller.state(), HandshakeState::rejected);
    }
    Peers off(false, false);
    const auto offer = off.offer();
    REQUIRE(!offer.has_standby_rtt);
    auto reply = wire(off.listener.receive(offer).values[0]);
    REQUIRE(!reply.has_standby_rtt);
    reply.has_standby_rtt = true;
    reply.standby_rtt = identity();
    REQUIRE_EQ(off.caller.receive(reply).values[0].rejection_reason, 15);
}

TEST(standby_rtt_handshake_requires_backup_live_tsbpd_and_nonzero_nonce)
{
    for (int defect = 0; defect < 5; ++defect) {
        auto c = config(ConnectionRole::caller, true);
        if (defect == 0)
            c.standby_rtt_nonce = {};
        if (defect == 1)
            c.has_group_membership = false;
        if (defect == 2)
            c.group_membership.type = GroupType::broadcast;
        if (defect == 3)
            c.congestion_controller = CongestionController::file;
        if (defect == 4)
            c.extension_parameters.flags = 0;
        HandshakeMachine caller(c);
        REQUIRE_EQ(caller.start().values[0].kind, HandshakeActionKind::failed);
    }
    Peers peers;
    auto offer = peers.offer();
    offer.group_membership.type = GroupType::broadcast;
    REQUIRE_EQ(peers.listener.receive(offer).values[1].rejection_reason, 15);
}

TEST(standby_rtt_handshake_auth_confirmation_and_replay_preserve_pair)
{
    Peers peers(true, true, true);
    auto offer = peers.offer();
    const auto challenge = peers.listener.receive(offer);
    REQUIRE_EQ(peers.listener.state(),
        HandshakeState::awaiting_authentication_confirmation);
    REQUIRE_EQ(
        wire(peers.listener.timeout().values[0]).standby_rtt, identity());
    REQUIRE_EQ(
        wire(peers.listener.receive(offer).values[0]).standby_rtt, identity());
    auto response = wire(challenge.values[0]);
    response.session_authentication.proof[0] = std::byte {4};
    REQUIRE_EQ(peers.caller.set_session_authentication(
                   response.session_authentication),
        Error::none);
    // Listener proof is separately checked by the embedding crypto boundary.
    const auto completion = peers.caller.receive(wire(challenge.values[0]));
    REQUIRE_EQ(completion.size, 2U);
    auto agreement = wire(completion.values[0]);
    REQUIRE_EQ(agreement.standby_rtt, identity());
    auto changed = agreement;
    changed.standby_rtt.listener[0] = std::byte {99};
    REQUIRE_EQ(peers.listener.receive(changed).size, 0U);
    REQUIRE_EQ(peers.listener.receive(agreement).size, 2U);
    REQUIRE_EQ(peers.listener.state(), HandshakeState::connected);
}

TEST(standby_rtt_bidirectional_codec_and_reflection_isolation)
{
    StandbyRttMeasurement caller, listener;
    REQUIRE_EQ(caller.configure(identity(), 9, 0), Error::none);
    REQUIRE_EQ(listener.configure(identity(), 10, 0, 1, false), Error::none);
    const auto c = caller.prepare_request(0, 10'000, 9, true);
    const auto l = listener.prepare_request(0, 10'000, 10, true);
    REQUIRE(c);
    REQUIRE(l);
    REQUIRE_EQ(c->probe_id, l->probe_id);
    caller.finish_send(c->probe_id, 9, 100);
    listener.finish_send(l->probe_id, 10, 200);
    // Both sides may have probe 1, but only the corresponding direction matches.
    caller.receive_echo(echo(*l), 400, 9, true);
    listener.receive_echo(echo(*c), 400, 10, true);
    REQUIRE_EQ(caller.snapshot(400).samples, 0U);
    REQUIRE_EQ(listener.snapshot(400).samples, 0U);
    REQUIRE(!caller.make_echo(*c, 400, 9, true, true));
    REQUIRE(!listener.make_echo(*l, 400, 10, true, true));
    const auto c_response = listener.make_echo(*c, 400, 10, true, true);
    const auto l_response = caller.make_echo(*l, 400, 9, true, true);
    REQUIRE(c_response);
    REQUIRE(l_response);
    std::array<std::byte, 64> bytes {};
    REQUIRE(encode_standby_rtt_datagram(*l_response, {}, 20, bytes));
    const auto decoded = decode_standby_rtt_datagram(bytes);
    REQUIRE(decoded);
    REQUIRE(!decoded.message.caller_initiated);
    REQUIRE_EQ(decoded.message.kind, StandbyRttKind::echo);
    caller.receive_echo(*c_response, 500, 9, true);
    listener.receive_echo(decoded.message, 600, 10, true);
    REQUIRE_EQ(caller.snapshot(600).rtt_microseconds, 400U);
    REQUIRE_EQ(listener.snapshot(600).rtt_microseconds, 400U);
}

TEST(standby_rtt_measurement_native_send_time_and_sample_age)
{
    StandbyRttMeasurement m;
    REQUIRE_EQ(m.configure(identity(), 9, 100), Error::none);
    REQUIRE_EQ(m.snapshot(0).age_microseconds, UINT64_MAX);
    const auto request = begin(m);
    REQUIRE(m.outstanding());
    REQUIRE(!m.prepare_request(200, 20'000'000, 9, true));
    m.finish_send(request.probe_id, 9, 400);
    m.receive_echo(echo(request), 700, 9, true);
    const auto sample = m.snapshot(800);
    REQUIRE_EQ(sample.samples, 1U);
    REQUIRE_EQ(sample.rtt_microseconds, 300U);
    REQUIRE_EQ(sample.age_microseconds, 100U);
    REQUIRE(sample.fresh);
    REQUIRE(!m.outstanding());
    m.receive_echo(echo(request), 900, 9, true);
    REQUIRE_EQ(m.snapshot(900).samples, 1U);
    REQUIRE(!m.snapshot(3'000'701).fresh);
    REQUIRE_EQ(m.snapshot(600).age_microseconds, UINT64_MAX);
    m.terminate();
    REQUIRE_EQ(m.snapshot(1000).samples, 1U);
    REQUIRE(!m.prepare_request(1'000'100, 20'000'000, 9, true));
}

TEST(standby_rtt_measurement_early_echo_completion_race_and_backpressure)
{
    for (int completion = 0; completion < 4; ++completion) {
        StandbyRttMeasurement m;
        REQUIRE_EQ(m.configure(identity(), 9, 100), Error::none);
        const auto request = begin(m);
        m.receive_echo(echo(request), 500, 9, true);
        m.receive_echo(echo(request), 600, 9, true);
        if (completion == 0)
            m.finish_send(request.probe_id, 9, 400);
        if (completion == 1)
            m.finish_send(request.probe_id, 9, std::nullopt);
        if (completion == 2)
            m.finish_send(request.probe_id, 9, 99);
        if (completion == 3)
            m.finish_send(request.probe_id, 9, 700);
        REQUIRE_EQ(m.snapshot(800).samples, completion == 0 ? 1U : 0U);
        if (completion == 0)
            REQUIRE_EQ(m.snapshot(800).rtt_microseconds, 100U);
        REQUIRE(!m.prepare_request(999'999, 20'000'000, 9, true));
    }
}

TEST(standby_rtt_native_acceptance_preserves_minimum_interval)
{
    StandbyRttMeasurement m;
    REQUIRE_EQ(m.configure(identity(), 9, 0), Error::none);
    const auto request = begin(m, 0);
    m.finish_send(request.probe_id, 9, 1'500'000);
    m.receive_echo(echo(request), 1'600'000, 9, true);
    REQUIRE_EQ(m.snapshot(1'600'000).rtt_microseconds, 100'000U);
    REQUIRE(!m.prepare_request(2'000'000, 20'000'000, 9, true));
    REQUIRE(m.prepare_request(2'500'000, 20'000'000, 9, true));
}

TEST(standby_rtt_measurement_loss_deadline_identity_and_generation_isolation)
{
    StandbyRttMeasurement m;
    REQUIRE_EQ(m.configure(identity(), 9, 100), Error::none);
    const auto request = m.prepare_request(100, 800, 9, true);
    REQUIRE(request);
    m.finish_send(request->probe_id, 9, 200);
    auto wrong = echo(*request);
    wrong.identity.listener = nonce(99);
    m.receive_echo(wrong, 500, 9, true);
    wrong = echo(*request);
    ++wrong.probe_id;
    m.receive_echo(wrong, 500, 9, true);
    m.receive_echo(echo(*request), 500, 8, true);
    m.receive_echo(echo(*request), 500, 9, false);
    m.receive_echo(*request, 500, 9, true);
    REQUIRE_EQ(m.snapshot(500).samples, 0U);
    REQUIRE(m.outstanding());
    m.receive_echo(echo(*request), 800, 9, true);
    REQUIRE_EQ(m.snapshot(800).outcome, StandbyRttOutcome::timeout);
    REQUIRE(!m.outstanding());
    REQUIRE_EQ(m.snapshot(801).samples, 0U);
    auto fresh = identity();
    fresh.listener = nonce(7);
    REQUIRE_EQ(m.configure(fresh, 10, 0), Error::none);
    auto next = m.prepare_request(0, 5000, 10, true);
    REQUIRE(next);
    m.finish_send(
        next->probe_id, 9, 100); // old completion cannot start new slot
    m.receive_echo(echo(*request), 500, 10, true);
    REQUIRE_EQ(m.snapshot(500).samples, 0U);
    m.finish_send(next->probe_id, 10, 100);
    m.receive_echo(echo(*next), 500, 10, true);
    REQUIRE_EQ(m.snapshot(500).samples, 1U);
}

TEST(standby_rtt_measurement_pause_terminal_overflow_and_retained_sample)
{
    StandbyRttMeasurement m;
    REQUIRE_EQ(m.configure(identity(), 9, 0), Error::none);
    auto request = begin(m, 0);
    m.finish_send(request.probe_id, 9, 0);
    m.receive_echo(echo(request), 50, 9, true);
    request = begin(m, 1'000'000);
    m.finish_send(request.probe_id, 9, 1'000'000);
    m.expire(3'000'000);
    REQUIRE_EQ(m.snapshot(3'000'000).samples, 1U);
    REQUIRE_EQ(m.snapshot(3'000'000).outcome, StandbyRttOutcome::timeout);
    request = begin(m, 3'000'000);
    REQUIRE(!m.prepare_request(3'000'001, 20'000'000, 9, false));
    m.finish_send(request.probe_id, 9, 3'000'001);
    m.receive_echo(echo(request), 3'000'002, 9, true);
    REQUIRE_EQ(m.snapshot(3'000'002).samples, 1U);
    m.terminate();
    REQUIRE(!m.make_echo(request, 4'000'000, 9, true, true));
    REQUIRE_EQ(m.configure(identity(), 9, 0, UINT64_MAX), Error::none);
    REQUIRE(!m.prepare_request(0, UINT64_MAX, 9, true));
    REQUIRE_EQ(m.snapshot(0).outcome, StandbyRttOutcome::id_exhausted);
    REQUIRE_EQ(m.configure(identity(), 9, UINT64_MAX - 10), Error::none);
    auto end = m.prepare_request(UINT64_MAX - 10, UINT64_MAX, 9, true);
    REQUIRE(end);
    m.finish_send(end->probe_id, 9, UINT64_MAX - 10);
    m.receive_echo(echo(*end), UINT64_MAX, 9, true);
    REQUIRE_EQ(m.snapshot(UINT64_MAX).samples, 0U);
    REQUIRE_EQ(m.configure({}, 9, 0), Error::invalid_state);
    REQUIRE(!m.outstanding());
}

TEST(
    standby_rtt_responder_no_echo_loop_unnegotiated_flood_or_lifecycle_response)
{
    StandbyRttMeasurement m;
    REQUIRE_EQ(m.configure(identity(), 9, 0), Error::none);
    StandbyRttMessage request {
        .identity = identity(), .probe_id = 1, .caller_initiated = false};
    REQUIRE(!m.make_echo(echo(request), 0, 9, true, true));
    REQUIRE(!m.make_echo(request, 0, 8, true, true));
    REQUIRE(!m.make_echo(request, 0, 9, false, true));
    REQUIRE(!m.make_echo(request, 0, 9, true, false));
    const auto response = m.make_echo(request, 0, 9, true, true);
    REQUIRE(response);
    REQUIRE_EQ(response->kind, StandbyRttKind::echo);
    REQUIRE_EQ(response->probe_id, 1U);
    for (unsigned i = 0; i < 1000; ++i)
        REQUIRE(!m.make_echo(request, i, 9, true, true));
    REQUIRE(m.make_echo(request, 1'000'000, 9, true, true));
    REQUIRE_EQ(m.snapshot(1'000'000).samples, 0U);
    REQUIRE(!m.make_echo(request, UINT64_MAX, 9, true, true));
    m.terminate();
    REQUIRE(!m.make_echo(request, 2'000'000, 9, true, true));
}

TEST(
    standby_rtt_budget_capacity_fairness_independent_rates_and_cleanup_generation)
{
    StandbyRttBudget budget;
    std::array<StandbyRttBudget::Lease, 256> leases {};
    for (auto& lease : leases) {
        const auto allocated = budget.admit();
        REQUIRE(allocated);
        lease = *allocated;
        budget.set_eligible(lease, true);
    }
    REQUIRE(!budget.admit());
    for (std::size_t i = 0; i < 512; ++i) {
        const auto now = i * 31'250;
        REQUIRE_EQ(*budget.grant_request(now), leases[i % 256]);
        REQUIRE(!budget.grant_request(now));
        REQUIRE(budget.grant_echo(leases[i % 256], now));
        REQUIRE(!budget.grant_echo(leases[i % 256], now));
    }
    budget.release(leases[0]);
    const auto replacement = budget.admit();
    REQUIRE(replacement);
    REQUIRE_EQ(replacement->slot, leases[0].slot);
    REQUIRE(replacement->generation != leases[0].generation);
    budget.release(leases[0]);
    REQUIRE(!budget.grant_echo(leases[0], 20'000'000));
    REQUIRE(budget.grant_echo(*replacement, 20'000'000));
    budget.clear();
    REQUIRE(!budget.grant_echo(*replacement, 21'000'000));
    REQUIRE(!budget.grant_request(21'000'000));
    const auto after_clear = budget.admit();
    REQUIRE(after_clear);
    REQUIRE(after_clear->generation != replacement->generation);
    REQUIRE(!budget.grant_echo(*after_clear, 20'000'000));
    budget.set_eligible(*after_clear, true);
    REQUIRE(budget.grant_request(21'000'000));
    REQUIRE(!budget.grant_request(UINT64_MAX));
}
