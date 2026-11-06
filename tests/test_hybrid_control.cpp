#include "test.hpp"
#include "compat/hybrid_control.hpp"
#include "robotweax/srt/handshake_datagram.hpp"

#include <algorithm>
#include <limits>
#include <string_view>
#include <vector>

using namespace robotweax::srt;
namespace h = robotweax::srt::compat::hybrid;
namespace {
h::Context context()
{
    h::Context c;
    c.dispatch = {.group_id = group_handle_mask | 1U,
        .multicast_group = {.address = {239, 255, 1, 1}, .port = 6000},
        .initial_sequence = SequenceNumber::mask - 1U,
        .time_to_live = 1};
    c.source = {.kind = MulticastSourceKind::announcement,
        .group_id = c.dispatch.group_id,
        .endpoint = {.address = {192, 0, 2, 1}, .port = 9000}};
    c.offer.supported = h::known_features;
    REQUIRE(h::negotiate(c.offer, c.offer, 71, 97, 1234, c.selection));
    return c;
}
h::Message message(
    h::Kind kind = h::Kind::stream_progress, std::uint64_t serial = 1)
{
    auto c = context();
    h::Message m {.kind = kind,
        .stream = c.selection.stream,
        .member = c.selection.member,
        .serial = serial,
        .revision = 1,
        .next_position = 1237,
        .next_sequence = 1};
    if (kind == h::Kind::stream_progress) {
        m.retained_floor = 1234;
        m.multicast_next = 1236;
        m.produced_next = 1240;
        m.activity = h::Activity::producing;
        m.interval_us = 1000;
    } else if (kind == h::Kind::path_report
        || kind == h::Kind::multicast_ready) {
        m.multicast_next = 1236;
        m.interval_us = 1000;
    } else if (kind == h::Kind::fallback_request)
        m.mode = h::Mode::unicast;
    else
        m.retained_floor = 1234;
    return m;
}
void put32(std::byte* p, std::uint32_t n)
{
    for (unsigned i = 0; i < 4; ++i)
        p[i] = std::byte(n >> (24 - i * 8));
}
}

TEST(
    hybrid_control_capability_negotiation_is_strict_and_required_features_are_bilateral)
{
    auto c = context();
    REQUIRE(h::valid(c));
    auto local = c.offer;
    local.supported = h::core_features | h::overlap;
    local.prefix_limit = 17;
    h::Capability selected;
    REQUIRE(h::negotiate(c.offer, local, 71, 97, UINT64_MAX, selected));
    REQUIRE_EQ(selected.supported, local.supported);
    REQUIRE_EQ(selected.prefix_limit, 17U);
    c.offer.required |= h::source_recovery;
    REQUIRE(!h::negotiate(c.offer, local, 71, 97, 0, selected));
    local.required |= h::source_recovery;
    REQUIRE(!h::valid(local));
    c = context();
    c.offer.supported |= 0x80000000U;
    REQUIRE(!h::valid(c.offer));
    c = context();
    c.selection.supported = h::core_features;
    c.offer.required |= h::overlap;
    REQUIRE(!h::valid(c));
    c = context();
    REQUIRE(!h::negotiate(c.offer, c.offer, 0, 97, 0, selected));
    REQUIRE(!h::negotiate(c.offer, c.offer, 71, 0, 0, selected));
}

TEST(
    hybrid_control_capability_exact_layout_reserved_bytes_and_transactional_encoding)
{
    const auto c = context();
    std::array<std::byte, h::capability_size> wire;
    wire.fill(std::byte {0xaa});
    REQUIRE(h::encode(c.selection, wire));
    REQUIRE_EQ(wire[3], std::byte {1});
    REQUIRE_EQ(wire[7], std::byte {2});
    REQUIRE_EQ(wire[11], std::byte {31});
    REQUIRE_EQ(wire[19], std::byte {0});
    REQUIRE_EQ(wire[18], std::byte {1});
    REQUIRE_EQ(wire[31], std::byte {71});
    REQUIRE_EQ(wire[39], std::byte {97});
    REQUIRE_EQ(wire[46], std::byte {4});
    REQUIRE_EQ(wire[47], std::byte {0xd2});
    h::Capability parsed;
    REQUIRE(h::decode(wire, parsed));
    REQUIRE_EQ(parsed, c.selection);
    for (std::size_t i = 20; i < 24; ++i) {
        auto bad = wire;
        bad[i] = std::byte {1};
        REQUIRE(!h::decode(bad, parsed));
    }
    auto bad = c.selection;
    bad.prefix_limit = 257;
    auto sentinel = wire;
    REQUIRE(!h::encode(bad, sentinel));
    REQUIRE_EQ(sentinel, wire);
    for (std::size_t size = 0; size < wire.size(); ++size)
        REQUIRE(!h::decode(std::span {wire}.first(size), parsed));
    std::array<std::byte, 49> extra {};
    REQUIRE(!h::decode(extra, parsed));
    for (const auto bad_value : {0U, 2U, UINT32_MAX}) {
        auto bad_wire = wire;
        put32(bad_wire.data(), bad_value);
        REQUIRE(!h::decode(bad_wire, parsed));
    }
}

TEST(
    hybrid_control_context_binds_existing_announcement_source_and_both_capabilities)
{
    auto c = context();
    std::array<std::byte, h::context_size> wire {};
    REQUIRE(h::encode(c, wire));
    h::Context parsed;
    REQUIRE(h::decode(wire, parsed));
    REQUIRE_EQ(parsed, c);
    REQUIRE_EQ(wire[0], std::byte {'R'});
    REQUIRE_EQ(wire[1], std::byte {'W'});
    REQUIRE_EQ(wire[2], std::byte {'H'});
    REQUIRE_EQ(wire[4], std::byte {'R'});
    REQUIRE_EQ(wire[64], std::byte {'R'});
    std::array<std::byte, 64> legacy {};
    REQUIRE(encode_multicast_dispatch(c.dispatch, legacy));
    REQUIRE(std::equal(legacy.begin() + 4, legacy.end(), wire.begin() + 4));
    std::array<std::byte, 36> source {};
    REQUIRE(encode_multicast_source(c.source, source));
    REQUIRE(std::equal(source.begin() + 4, source.end(), wire.begin() + 64));
    for (const auto offset : {0U, 3U, 61U, 95U, 116U, 164U}) {
        auto changed = wire;
        changed[offset] ^= std::byte {1};
        REQUIRE(!h::decode(changed, parsed));
    }
    c.source.group_id++;
    REQUIRE(!h::valid(c));
    c = context();
    c.source.kind = MulticastSourceKind::offer;
    REQUIRE(!h::valid(c));
    c = context();
    c.source.endpoint = IpEndpoint::ipv6_any(9000);
    c.source.endpoint.address[0] = 0x20;
    c.source.endpoint.address[1] = 1;
    REQUIRE(!h::valid(c));
}

TEST(hybrid_control_all_six_message_kinds_round_trip_without_allocated_state)
{
    for (unsigned k = 1; k <= 6; ++k) {
        const auto m = message(static_cast<h::Kind>(k));
        REQUIRE(h::valid(m, context()));
        std::array<std::byte, h::message_size> bytes {};
        REQUIRE(h::encode(m, bytes));
        h::Message parsed;
        REQUIRE(h::decode(bytes, parsed));
        REQUIRE_EQ(parsed, m);
        REQUIRE_EQ(bytes[3], std::byte {1});
        REQUIRE_EQ(bytes[7], static_cast<std::byte>(k));
        REQUIRE_EQ(bytes[15], std::byte {71});
        REQUIRE_EQ(bytes[23], std::byte {97});
        REQUIRE_EQ(bytes[31], std::byte {1});
        for (std::size_t size = 0; size < bytes.size(); ++size)
            REQUIRE(!h::decode(std::span {bytes}.first(size), parsed));
        std::array<std::byte, 100> extra {};
        REQUIRE(!h::decode(extra, parsed));
    }
}

TEST(hybrid_control_unused_fields_roles_modes_and_timing_fail_closed)
{
    for (unsigned k = 1; k <= 6; ++k) {
        auto m = message(static_cast<h::Kind>(k));
        const bool caller = k == 2 || k == 3 || k == 6;
        REQUIRE_EQ(h::sender_is_caller(m.kind), caller);
        m.stream = 0;
        REQUIRE(!h::valid(m));
        m = message(static_cast<h::Kind>(k));
        m.member = 0;
        REQUIRE(!h::valid(m));
        m = message(static_cast<h::Kind>(k));
        m.serial = 0;
        REQUIRE(!h::valid(m));
        m = message(static_cast<h::Kind>(k));
        m.mode = static_cast<h::Mode>(3);
        REQUIRE(!h::valid(m));
        m = message(static_cast<h::Kind>(k));
        m.age_us = h::maximum_age_us + 1;
        REQUIRE(!h::valid(m));
        m = message(static_cast<h::Kind>(k));
        m.interval_us = h::maximum_interval_us + 1;
        REQUIRE(!h::valid(m));
        if (k != 1) {
            m = message(static_cast<h::Kind>(k));
            m.activity = h::Activity::producing;
            REQUIRE(!h::valid(m));
        }
        if (k != 5) {
            m = message(static_cast<h::Kind>(k));
            m.outcome = h::Outcome::budget;
            REQUIRE(!h::valid(m));
        }
    }
    auto m = message(h::Kind::fallback_request);
    m.mode = h::Mode::multicast;
    REQUIRE(!h::valid(m));
    m = message(h::Kind::delivery_commit);
    m.revision = 0;
    REQUIRE(!h::valid(m));
    m = message(h::Kind::delivery_commit);
    m.multicast_next = 1;
    REQUIRE(!h::valid(m));
    m = message(h::Kind::path_report);
    m.produced_next = 1;
    REQUIRE(!h::valid(m));
    m = message(h::Kind::path_report);
    m.retained_floor = 1;
    REQUIRE(!h::valid(m));
}

TEST(
    hybrid_control_sequence_position_and_identity_validation_survives_multiple_wire_wraps)
{
    auto c = context();
    auto m = message();
    for (const auto delta :
        {0ULL, 1ULL, 2ULL, 3ULL, 0x80000000ULL, 0x100000003ULL}) {
        m.next_position = c.selection.initial_position + delta;
        m.next_sequence = static_cast<std::uint32_t>(
            (c.dispatch.initial_sequence + delta) & SequenceNumber::mask);
        m.multicast_next = c.selection.initial_position;
        m.produced_next = m.next_position;
        REQUIRE(h::valid(m, c));
        auto bad = m;
        bad.next_sequence = (m.next_sequence + 1) & SequenceNumber::mask;
        REQUIRE(!h::valid(bad, c));
    }
    m = message();
    m.stream++;
    REQUIRE(!h::valid(m, c));
    m = message();
    m.member++;
    REQUIRE(!h::valid(m, c));
    m = message();
    m.next_position = c.selection.initial_position - 1;
    REQUIRE(!h::valid(m, c));
    c.offer.supported = c.selection.supported = h::core_features;
    m = message(h::Kind::multicast_ready);
    REQUIRE(!h::valid(m, c));
    m = message();
    m.mode = h::Mode::overlap;
    REQUIRE(!h::valid(m, c));
}

TEST(
    hybrid_control_admitted_production_does_not_claim_contiguous_emission_or_ack_progress)
{
    auto c = context();
    auto m = message();
    m.retained_floor = 1239;
    m.next_position = 1234;
    m.next_sequence = c.dispatch.initial_sequence;
    m.multicast_next = 1234;
    m.produced_next = 1240;
    REQUIRE(h::valid(m, c));
    m.next_position = 1241;
    REQUIRE(!h::valid(m));
    m = message();
    m.multicast_next = m.next_position + 1;
    REQUIRE(!h::valid(m));
    m = message();
    m.retained_floor = m.produced_next + 1;
    REQUIRE(!h::valid(m));
}

TEST(hybrid_control_build_gate_does_not_activate_production_handshakes)
{
    SessionAuthentication auth(default_crypto_provider());
#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_HYBRID_CONTROL
    REQUIRE_EQ(auth.start(true, h::authentication_version), Error::none);
#else
    REQUIRE_EQ(
        auth.start(true, h::authentication_version), Error::invalid_state);
#endif
    REQUIRE(!is_acp1_session_version(h::authentication_version));
    HandshakeAction action;
    action.kind = HandshakeActionKind::send;
    action.packet.request = HandshakeRequest::conclusion;
    action.packet.extension_field = handshake_extension_flag_config;
    action.has_session_authentication = true;
    action.session_authentication.version = h::authentication_version;
    std::array<std::byte, 1500> bytes {};
    const auto encoded =
        encode_handshake_datagram(action, PacketTimestamp {}, 1, bytes);
    REQUIRE(encoded);
    REQUIRE(!decode_handshake_datagram(
        std::span {bytes}.first(encoded.bytes_written)));
    action.session_authentication.version = session_authentication_version;
    const auto ordinary =
        encode_handshake_datagram(action, PacketTimestamp {}, 1, bytes);
    REQUIRE(ordinary);
    REQUIRE(decode_handshake_datagram(
        std::span {bytes}.first(ordinary.bytes_written)));
}

TEST(hybrid_control_context_rejects_lossy_local_fields_and_round_trips_ipv6)
{
    auto c = context();
    c.dispatch.multicast_group.scope_id = 7;
    REQUIRE(!h::valid(c));
    c = context();
    c.dispatch.interface_hint.port = 7;
    REQUIRE(!h::valid(c));
    c = context();
    c.dispatch.multicast_group.address[15] = 1;
    REQUIRE(!h::valid(c));
    c = context();
    c.dispatch.multicast_group = IpEndpoint::ipv6_any(6000);
    c.dispatch.multicast_group.address[0] = 0xff;
    c.dispatch.multicast_group.address[1] = 0x12;
    c.dispatch.multicast_group.address[15] = 1;
    c.dispatch.interface_hint = IpEndpoint::ipv6_any();
    c.source.endpoint = IpEndpoint::ipv6_any(9000);
    c.source.endpoint.address[0] = 0x20;
    c.source.endpoint.address[1] = 1;
    c.source.endpoint.address[15] = 1;
    REQUIRE(h::valid(c));
    std::array<std::byte, h::context_size> bytes {};
    REQUIRE(h::encode(c, bytes));
    h::Context parsed;
    REQUIRE(h::decode(bytes, parsed));
    REQUIRE_EQ(parsed, c);
    c.selection.initial_position = UINT64_MAX - 2;
    auto m = message();
    m.next_position = UINT64_MAX;
    m.next_sequence = 0;
    m.multicast_next = c.selection.initial_position;
    m.retained_floor = c.selection.initial_position;
    m.produced_next = UINT64_MAX;
    REQUIRE(h::valid(m, c));
}

#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_HYBRID_CONTROL
namespace {
class HybridProvider final : public CryptoProvider {
public:
    unsigned byte = 0;
    bool fail_next = false;
    explicit HybridProvider(unsigned seed = 0)
        : byte(seed)
    {
    }
    Error random_bytes(std::span<std::byte> out) noexcept override
    {
        for (auto& value : out)
            value = static_cast<std::byte>(byte++);
        return Error::none;
    }
    Error pbkdf2_hmac_sha1(std::span<const std::byte> p,
        std::span<const std::byte> s, std::uint32_t n,
        std::span<std::byte> out) noexcept override
    {
        return default_crypto_provider().pbkdf2_hmac_sha1(p, s, n, out);
    }
    Error hmac_sha256(std::span<const std::byte> k,
        std::span<const std::byte> in,
        std::span<std::byte, 32> out) noexcept override
    {
        if (fail_next) {
            fail_next = false;
            secure_erase(out);
            return Error::cryptographic_failure;
        }
        return default_crypto_provider().hmac_sha256(k, in, out);
    }
    Error wrap_key(std::span<const std::byte>, std::span<const std::byte>,
        std::span<std::byte>, std::size_t& n) noexcept override
    {
        n = 0;
        return Error::unsupported;
    }
    Error unwrap_key(std::span<const std::byte>, std::span<const std::byte>,
        std::span<std::byte>, std::size_t& n) noexcept override
    {
        n = 0;
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
    void secure_erase(std::span<std::byte> b) noexcept override
    {
        default_crypto_provider().secure_erase(b);
    }
};
struct Pair {
    HybridProvider provider;
    h::Context ctx = context();
    SessionAuthentication caller, listener;
    std::array<std::byte, h::context_size> binding {};
    const std::array<std::byte, 3> material {
        std::byte {1}, std::byte {2}, std::byte {3}};
    explicit Pair(bool confirm = true, unsigned seed = 0,
        h::Context caller_context = context(),
        h::Context listener_context = context(), std::uint32_t caller_id = 107,
        std::uint32_t listener_id = 108)
        : provider(seed)
        , ctx(caller_context)
        , caller(provider)
        , listener(provider)
    {
        REQUIRE(h::encode(ctx, binding));
        std::array<std::byte, h::context_size> peer {};
        REQUIRE(h::encode(listener_context, peer));
        REQUIRE_EQ(caller.start(true, h::authentication_version), Error::none);
        REQUIRE_EQ(
            listener.start(false, h::authentication_version), Error::none);
        constexpr char pass[] = "pairwise-control-fixture-key";
        const auto credential =
            std::as_bytes(std::span {pass, sizeof(pass) - 1});
        REQUIRE_EQ(listener.establish(credential, caller.parameters(),
                       listener_id, caller_id, 0, 0, peer),
            Error::none);
        REQUIRE_EQ(caller.establish(credential, listener.parameters(),
                       caller_id, listener_id, 0, 0, binding),
            Error::none);
        if (confirm)
            confirm_peer();
    }
    void confirm_peer()
    {
        SessionAuthenticationParameters proof;
        REQUIRE_EQ(
            listener.handshake_proof(false, material, proof), Error::none);
        REQUIRE(caller.verify_handshake(false, material, proof));
        REQUIRE_EQ(caller.handshake_proof(true, material, proof), Error::none);
        REQUIRE(listener.verify_handshake(true, material, proof));
    }
};
std::vector<std::byte> sealed(h::Session& session, const h::Message& m,
    PacketTimestamp timestamp = PacketTimestamp {77})
{
    std::array<std::byte, 256> bytes {};
    std::size_t n = 0;
    const auto result = session.seal(m, timestamp, bytes, n);
    REQUIRE(result == h::Session::Status::accepted
        || result == h::Session::Status::duplicate);
    REQUIRE_EQ(n,
        packet_header_size + h::message_size + authenticated_control_overhead);
    return {bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(n)};
}
h::Session::Result received(h::Session& session,
    const std::vector<std::byte>& bytes, std::uint64_t now = 100)
{
    auto packet = decode_packet(bytes);
    REQUIRE(packet);
    return session.receive(packet.packet, now);
}
using S = h::Session::Status;
static_assert(sizeof(h::Session) <= 16 * 1024,
    "hybrid session bookkeeping must remain bounded");
}

TEST(
    hybrid_control_authentication_requires_peer_confirmation_and_one_owner_per_incarnation)
{
    Pair pair(false);
    h::Session early(pair.listener, pair.ctx, false);
    REQUIRE(!early.ready());
    SessionAuthenticationParameters self;
    REQUIRE_EQ(
        pair.listener.handshake_proof(false, pair.material, self), Error::none);
    REQUIRE(!pair.listener.verify_handshake(false, pair.material, self));
    REQUIRE(!pair.listener.hybrid_control_ready(false, pair.binding));
    pair.confirm_peer();
    h::Session wrong_role(pair.listener, pair.ctx, true);
    REQUIRE(!wrong_role.ready());
    auto wrong = pair.ctx;
    wrong.selection.member++;
    h::Session wrong_context(pair.listener, wrong, false);
    REQUIRE(!wrong_context.ready());
    h::Session sender(pair.listener, pair.ctx, false),
        receiver(pair.caller, pair.ctx, true);
    REQUIRE(sender.ready());
    REQUIRE(receiver.ready());
    h::Session clone(pair.listener, pair.ctx, false);
    REQUIRE(!clone.ready());
    auto bytes = sealed(sender, message());
    REQUIRE_EQ(received(receiver, bytes).status, S::accepted);
    sender.close();
    h::Session reincarnated(pair.listener, pair.ctx, false);
    REQUIRE(!reincarnated.ready());
    std::size_t n = 9;
    std::array<std::byte, 256> output {};
    REQUIRE_EQ(
        sender.seal(message(), PacketTimestamp {}, output, n), S::closed);
    REQUIRE_EQ(n, 0U);
}

TEST(hybrid_control_all_roles_use_authenticated_controls_and_cannot_reflect)
{
    Pair pair;
    h::Session caller(pair.caller, pair.ctx, true),
        listener(pair.listener, pair.ctx, false);
    for (unsigned k = 1; k <= 6; ++k) {
        const auto kind = static_cast<h::Kind>(k);
        const auto m = message(kind, k);
        auto& sender = h::sender_is_caller(kind) ? caller : listener;
        auto& peer = h::sender_is_caller(kind) ? listener : caller;
        const auto bytes = sealed(sender, m);
        const auto opened = received(peer, bytes, 100 + k);
        REQUIRE_EQ(opened.status, S::accepted);
        REQUIRE_EQ(opened.message, m);
        REQUIRE_EQ(
            received(sender, bytes, 100 + k).status, S::authentication_failed);
        std::array<std::byte, 256> output {};
        std::size_t n = 7;
        REQUIRE_EQ(peer.seal(m, PacketTimestamp {}, output, n), S::invalid);
        REQUIRE_EQ(n, 0U);
    }
}

TEST(
    hybrid_control_every_byte_tamper_fails_without_poisoning_legitimate_replay_state)
{
    Pair pair;
    h::Session sender(pair.listener, pair.ctx, false),
        receiver(pair.caller, pair.ctx, true);
    const auto bytes = sealed(sender, message());
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        auto bad = bytes;
        bad[i] ^= std::byte {1};
        const auto parsed = decode_packet(bad);
        if (parsed)
            REQUIRE(receiver.receive(parsed.packet, 100).status != S::accepted);
    }
    REQUIRE_EQ(received(receiver, bytes).status, S::accepted);
    REQUIRE_EQ(received(receiver, bytes, 101).status, S::authentication_failed);
}

TEST(
    hybrid_control_capabilities_source_stream_and_session_are_bound_into_proofs)
{
    for (unsigned variant = 0; variant < 7; ++variant) {
        auto altered = context();
        if (variant == 0)
            altered.selection.member++;
        if (variant == 1)
            altered.selection.stream++;
        if (variant == 2)
            altered.source.endpoint.port++;
        if (variant == 3)
            altered.dispatch.initial_sequence--;
        if (variant == 4)
            altered.selection.supported &= ~h::overlap;
        if (variant == 5)
            altered.selection.prefix_limit--;
        if (variant == 6)
            altered.selection.initial_position++;
        Pair mismatched(false, 0, context(), altered);
        SessionAuthenticationParameters proof;
        REQUIRE_EQ(mismatched.listener.handshake_proof(
                       false, mismatched.material, proof),
            Error::none);
        REQUIRE(!mismatched.caller.verify_handshake(
            false, mismatched.material, proof));
        REQUIRE(
            !mismatched.caller.hybrid_control_ready(true, mismatched.binding));
    }
    Pair original;
    h::Session sender(original.listener, original.ctx, false);
    const auto bytes = sealed(sender, message());
    Pair foreign(true, 64);
    h::Session foreign_receiver(foreign.caller, foreign.ctx, true);
    REQUIRE_EQ(
        received(foreign_receiver, bytes).status, S::authentication_failed);
    Pair other_ids(true, 0, context(), context(), 307, 308);
    h::Session other_receiver(other_ids.caller, other_ids.ctx, true);
    REQUIRE_EQ(
        received(other_receiver, bytes).status, S::authentication_failed);
}

TEST(hybrid_control_semantic_duplicates_never_refresh_report_age_or_reapply)
{
    Pair pair;
    h::Session sender(pair.listener, pair.ctx, false),
        receiver(pair.caller, pair.ctx, true);
    auto report = message();
    report.age_us = 50;
    REQUIRE_EQ(
        received(receiver, sealed(sender, report), 100).status, S::accepted);
    REQUIRE(receiver.fresh(report.kind, 150, 100));
    REQUIRE(!receiver.fresh(report.kind, 151, 100));
    const auto duplicate =
        received(receiver, sealed(sender, report, PacketTimestamp {78}), 1000);
    REQUIRE_EQ(duplicate.status, S::duplicate);
    REQUIRE_EQ(duplicate.first_received_us, 100U);
    REQUIRE(!receiver.fresh(report.kind, 1000, 100));
    report.serial = 2;
    report.age_us = 0;
    REQUIRE_EQ(
        received(receiver, sealed(sender, report), 1001).status, S::accepted);
    REQUIRE(receiver.fresh(report.kind, 1001, 1));
    REQUIRE(!receiver.fresh(report.kind, 1000, 100));
    const auto pending = sealed(sender, message(h::Kind::delivery_status, 3));
    REQUIRE_EQ(received(receiver, pending, 999).status, S::invalid);
    REQUIRE_EQ(received(receiver, pending, 1002).status,
        S::accepted); // Clock rejection consumed no envelope.
    receiver.close();
    REQUIRE(!receiver.fresh(report.kind, 1002, 100));
}

TEST(
    hybrid_control_bounded_serial_window_conflicts_reordering_and_exhaustion_fail_closed)
{
    Pair pair;
    h::Session sender(pair.listener, pair.ctx, false),
        receiver(pair.caller, pair.ctx, true);
    auto first = message(h::Kind::delivery_status, 1);
    const auto one = sealed(sender, first);
    auto second = message(h::Kind::delivery_status, 2);
    const auto two = sealed(sender, second);
    REQUIRE_EQ(received(receiver, two, 100).status, S::accepted);
    REQUIRE_EQ(received(receiver, one, 101).status, S::accepted);
    auto altered = second;
    altered.next_position++;
    altered.next_sequence++;
    std::array<std::byte, 256> out {};
    std::size_t n = 0;
    REQUIRE_EQ(sender.seal(altered, PacketTimestamp {}, out, n), S::conflict);
    REQUIRE_EQ(n, 0U);
    REQUIRE_EQ(received(receiver,
                   sealed(sender, message(h::Kind::delivery_status, 65)), 102)
                   .status,
        S::accepted);
    REQUIRE_EQ(sender.seal(first, PacketTimestamp {}, out, n), S::stale);
    const auto last = message(h::Kind::delivery_status, UINT64_MAX);
    REQUIRE_EQ(
        received(receiver, sealed(sender, last), 103).status, S::accepted);
    REQUIRE_EQ(
        received(receiver, sealed(sender, last), 104).status, S::duplicate);
    REQUIRE_EQ(sender.seal(message(h::Kind::delivery_status, 0),
                   PacketTimestamp {}, out, n),
        S::invalid);
    REQUIRE_EQ(sender.seal(second, PacketTimestamp {}, out, n), S::stale);
}

TEST(
    hybrid_control_old_report_cannot_replace_a_newer_report_within_crypto_replay_window)
{
    Pair pair;
    h::Session sender(pair.listener, pair.ctx, false),
        receiver(pair.caller, pair.ctx, true);
    const auto old = sealed(sender, message(h::Kind::stream_progress, 1));
    const auto recent = sealed(sender, message(h::Kind::stream_progress, 2));
    REQUIRE_EQ(received(receiver, recent, 1000).status, S::accepted);
    REQUIRE_EQ(received(receiver, old, 1001).status, S::stale);
    REQUIRE(receiver.fresh(h::Kind::stream_progress, 1010, 10));
    REQUIRE(!receiver.fresh(h::Kind::stream_progress, 1011, 10));
}

TEST(
    hybrid_control_failed_signing_or_short_output_does_not_consume_semantic_serial)
{
    Pair pair;
    h::Session sender(pair.listener, pair.ctx, false),
        receiver(pair.caller, pair.ctx, true);
    std::array<std::byte, 256> out;
    out.fill(std::byte {0xaa});
    const auto original = out;
    std::size_t n = 5;
    pair.provider.fail_next = true;
    REQUIRE_EQ(sender.seal(message(), PacketTimestamp {77}, out, n),
        S::authentication_failed);
    REQUIRE_EQ(n, 0U);
    REQUIRE_EQ(out, original);
    REQUIRE_EQ(sender.seal(message(), PacketTimestamp {77},
                   std::span {out}.first(20), n),
        S::authentication_failed);
    REQUIRE_EQ(n, 0U);
    REQUIRE_EQ(out, original);
    const auto bytes = sealed(sender, message());
    REQUIRE_EQ(received(receiver, bytes).status, S::accepted);
    Pair reference;
    h::Session other(reference.listener, reference.ctx, false);
    REQUIRE_EQ(bytes, sealed(other, message()));
}

TEST(hybrid_control_profile_cannot_authenticate_other_control_KM_or_FEC_types)
{
    Pair pair;
    h::Session sender(pair.listener, pair.ctx, false);
    std::array<std::byte, 256> out {};
    std::size_t n = 0;
    std::array<std::byte, 20> bare {};
    REQUIRE(encode_packet({.kind = PacketKind::control,
                              .control = {.type = ControlType::keepalive}},
        bare));
    REQUIRE_EQ(
        pair.listener.seal_control(bare, out, n), Error::invalid_control_type);
    REQUIRE_EQ(
        pair.listener.seal(false, pair.material, out, n), Error::invalid_state);
    DataHeader header;
    REQUIRE_EQ(pair.listener.seal_fec(header, pair.material, out, n),
        Error::invalid_state);
    std::span<const std::byte> opened;
    REQUIRE(!pair.listener.open(false, out, opened));
    REQUIRE(!pair.listener.open_fec(header, out, opened));
    REQUIRE(!pair.listener.claim_hybrid_control(false, pair.binding));
}
#endif

#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_HYBRID_CONTROL
namespace {
std::vector<std::byte> unhex(std::string_view text)
{
    REQUIRE(text.size() % 2 == 0);
    std::vector<std::byte> out;
    const auto nibble = [](char c) {
        return c >= '0' && c <= '9' ? c - '0' : c - 'a' + 10;
    };
    for (std::size_t i = 0; i < text.size(); i += 2)
        out.push_back(static_cast<std::byte>(
            (nibble(text[i]) << 4) | nibble(text[i + 1])));
    return out;
}
std::vector<std::byte> peer_raw(
    SessionAuthentication& auth, const h::Message& m)
{
    std::array<std::byte, h::message_size> body {};
    REQUIRE(h::encode(m, body));
    std::array<std::byte, 112> packet {};
    REQUIRE(encode_packet({.kind = PacketKind::control,
                              .control = {.type = ControlType::user_defined,
                                  .subtype = h::control_subtype,
                                  .timestamp = PacketTimestamp {77},
                                  .destination_socket_id = 107},
                              .payload = body},
        packet));
    std::array<std::byte, 256> wire {};
    std::size_t written = 0;
    REQUIRE_EQ(auth.seal_control(packet, wire, written), Error::none);
    return {wire.begin(), wire.begin() + static_cast<std::ptrdiff_t>(written)};
}
}

TEST(
    hybrid_control_independent_python_hashlib_proofs_context_and_wire_known_answer)
{
    // Reproduce with ./tools/python tests/hybrid_control_vectors.py.
    Pair pair;
    const auto expected_context = unhex(
        "5257480152574d014000000104001770efff0101000000000000000000000000000000"
        "000000000000000000000000007ffffffe000000000000000001000000525753010204"
        "232840000001c000020100000000000000000000000000000000000000010000000100"
        "00001f0000000700000100000000000000000000000000000000000000000000000000"
        "0000000000000001000000020000001f00000007000001000000000000000000000000"
        "47000000000000006100000000000004d2");
    REQUIRE(std::equal(expected_context.begin(), expected_context.end(),
        pair.binding.begin(), pair.binding.end()));
    SessionAuthenticationParameters proof;
    REQUIRE_EQ(
        pair.caller.handshake_proof(true, pair.material, proof), Error::none);
    const auto caller = unhex(
        "415732160901d48f25ce4ee574e998e40cd95b4aa3d38cbb0407e4b4196dfc10");
    REQUIRE(std::equal(
        caller.begin(), caller.end(), proof.proof.begin(), proof.proof.end()));
    REQUIRE_EQ(pair.listener.handshake_proof(false, pair.material, proof),
        Error::none);
    const auto listener = unhex(
        "b9341ea6bbfb8203778d26e03b16a900a47aaa00c2ee892811bfb9e9039d0185");
    REQUIRE(std::equal(listener.begin(), listener.end(), proof.proof.begin(),
        proof.proof.end()));
    h::Session sender(pair.listener, pair.ctx, false),
        receiver(pair.caller, pair.ctx, true);
    const auto wire = sealed(sender, message());
    REQUIRE_EQ(wire,
        unhex("ffff7f05000000000000004d0000006b000000010000000000000001ffff7f16"
              "000000000000004d0000006b0000000100000001000000000000004700000000"
              "000000610000000000000001000000000000000100000000000004d500000001"
              "0000000000000000000004d200000000000004d400000000000004d800000001"
              "00000000000003e80000000022e561795481b70a10a469f65023bbe76cc1e6af"
              "aecd4c00dca5cf5987cf9957"));
    REQUIRE_EQ(received(receiver, wire).status, S::accepted);
}

TEST(
    hybrid_control_authenticated_peer_conflict_wrong_binding_and_wrong_role_publish_no_report)
{
    Pair pair;
    h::Session sender(pair.listener, pair.ctx, false),
        receiver(pair.caller, pair.ctx, true);
    REQUIRE_EQ(
        received(receiver, sealed(sender, message()), 100).status, S::accepted);
    auto conflict = message();
    conflict.next_position++;
    conflict.next_sequence++;
    REQUIRE_EQ(
        received(receiver, peer_raw(pair.listener, conflict), 101).status,
        S::conflict);
    auto wrong = message(h::Kind::stream_progress, 2);
    wrong.member++;
    const auto rejected =
        received(receiver, peer_raw(pair.listener, wrong), 102);
    REQUIRE_EQ(rejected.status, S::invalid);
    REQUIRE_EQ(rejected.message.stream, 0U);
    REQUIRE_EQ(
        received(receiver,
            peer_raw(pair.listener, message(h::Kind::path_report, 2)), 103)
            .status,
        S::invalid);
    REQUIRE(!receiver.fresh(h::Kind::path_report, 103, 100));
    REQUIRE(!receiver.fresh(h::Kind::stream_progress, 1000, 100));
    REQUIRE_EQ(received(receiver,
                   sealed(sender, message(h::Kind::stream_progress, 2)), 1001)
                   .status,
        S::accepted);
    REQUIRE(receiver.fresh(h::Kind::stream_progress, 1001, 1));
}
#endif
