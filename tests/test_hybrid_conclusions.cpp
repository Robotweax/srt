// SPDX-License-Identifier: MIT
#include "test.hpp"
#include "compat/hybrid_conclusions.hpp"
#include "compat/transport_runtime.hpp"
#include "robotweax/srt/handshake_datagram.hpp"
#include <algorithm>
#include <vector>

using namespace robotweax::srt;
namespace h = robotweax::srt::compat::hybrid;
namespace {
struct Fixture {
    h::Context ctx;
    HandshakeAction caller, listener;
    Fixture()
    {
        ctx.dispatch = {.group_id = group_handle_mask | 1U,
            .multicast_group = {.address = {239, 255, 1, 1}, .port = 6000},
            .initial_sequence = SequenceNumber::mask - 1,
            .time_to_live = 1};
        ctx.source = {.kind = MulticastSourceKind::announcement,
            .group_id = ctx.dispatch.group_id,
            .endpoint = {.address = {192, 0, 2, 1}, .port = 9000}};
        REQUIRE(
            h::negotiate(ctx.offer, ctx.offer, 71, 97, 1234, ctx.selection));
        for (auto* a : {&caller, &listener}) {
            a->kind = HandshakeActionKind::send;
            a->packet.request = HandshakeRequest::conclusion;
            a->packet.extension_field = handshake_extension_flag_handshake
                | handshake_extension_flag_config;
            a->has_handshake_extension = true;
            a->has_group_membership = true;
            a->group_membership.type = GroupType::multicast;
            a->has_multicast_source = true;
        }
        caller.packet.socket_id = 107;
        caller.packet.initial_sequence = SequenceNumber {1};
        caller.extension_type = HandshakeExtensionType::handshake_request;
        caller.group_membership.group_id = group_handle_mask | 2U;
        caller.multicast_source.kind = MulticastSourceKind::offer;
        listener.packet.socket_id = 108;
        listener.packet.initial_sequence = SequenceNumber {2};
        listener.extension_type = HandshakeExtensionType::handshake_response;
        listener.group_membership.group_id = ctx.dispatch.group_id;
        listener.has_multicast_dispatch = true;
        listener.multicast_dispatch = ctx.dispatch;
        listener.multicast_source = ctx.source;
    }
};
std::vector<std::byte> datagram(const HandshakeAction& action)
{
    std::array<std::byte, 1500> bytes {};
    const auto encoded =
        encode_handshake_datagram(action, PacketTimestamp {91}, 17, bytes);
    REQUIRE(encoded);
    return {bytes.begin(),
        bytes.begin() + static_cast<std::ptrdiff_t>(encoded.bytes_written)};
}
h::ConclusionCapture captured(const Fixture& f)
{
    h::ConclusionCapture c;
    REQUIRE(c.observe(datagram(f.caller)));
    REQUIRE(c.observe(datagram(f.listener)));
    return c;
}
}
TEST(hybrid_conclusions_missing_or_wrong_socket_pair_cannot_publish)
{
    Fixture f;
    h::ConclusionCapture c;
    REQUIRE(!c.freeze(true, 107, 108));
    REQUIRE(c.observe(datagram(f.caller)));
    REQUIRE(!c.freeze(true, 107, 108));
    REQUIRE(c.observe(datagram(f.listener)));
    REQUIRE(!c.freeze(false, 107, 108));
    REQUIRE(!c.freeze(true, 107, 109));
    REQUIRE(c.freeze(true, 107, 108));
    REQUIRE(c.freeze(false, 108, 107));
}
TEST(hybrid_conclusions_owns_exact_bytes_and_immutable_publication)
{
    Fixture f;
    auto c = captured(f);
    const auto frozen = c.freeze(true, 107, 108);
    REQUIRE(frozen);
    const auto original = datagram(f.caller);
    REQUIRE(std::equal(original.begin() + packet_header_size, original.end(),
        frozen->caller_payload().begin(), frozen->caller_payload().end()));
    f.caller.packet.peer_address[0] = 123;
    REQUIRE(c.observe(datagram(f.caller)));
    const auto next = c.freeze(true, 107, 108);
    REQUIRE(next);
    REQUIRE(!std::equal(next->caller_payload().begin(),
        next->caller_payload().end(), frozen->caller_payload().begin()));
    REQUIRE(std::equal(original.begin() + packet_header_size, original.end(),
        frozen->caller_payload().begin()));
}
TEST(hybrid_conclusions_preserves_ignored_group_suffix_bytes_without_reencoding)
{
    Fixture f;
    auto bytes = datagram(f.caller);
    // Fixed 48-byte handshake + 16-byte HSREQ, followed by GROUP.
    const auto group = packet_header_size + handshake_size + 16;
    REQUIRE_EQ(bytes[group + 1], std::byte {8});
    bytes[group + 3] = std::byte {3};
    bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(group + 12),
        {std::byte {0xa1}, std::byte {0xb2}, std::byte {0xc3},
            std::byte {0xd4}});
    h::ConclusionCapture c;
    REQUIRE(c.observe(bytes));
    REQUIRE(c.observe(datagram(f.listener)));
    const auto frozen = c.freeze(true, 107, 108);
    REQUIRE(frozen);
    REQUIRE(std::equal(bytes.begin() + packet_header_size, bytes.end(),
        frozen->caller_payload().begin(), frozen->caller_payload().end()));
}
TEST(
    hybrid_conclusions_invalid_lengths_and_nonconclusion_records_cannot_replace_capture)
{
    Fixture f;
    auto c = captured(f);
    const auto before = c.freeze(true, 107, 108);
    REQUIRE(before);
    auto bytes = datagram(f.caller);
    for (std::size_t n = 0; n < packet_header_size + handshake_size; ++n)
        REQUIRE(!c.observe(std::span {bytes}.first(n)));
    bytes.resize(1501);
    REQUIRE(!c.observe(bytes));
    HandshakeAction agreement;
    agreement.kind = HandshakeActionKind::send;
    agreement.packet.request = HandshakeRequest::agreement;
    REQUIRE(!c.observe(datagram(agreement)));
    const auto after = c.freeze(true, 107, 108);
    REQUIRE(after);
    REQUIRE(std::equal(before->caller_payload().begin(),
        before->caller_payload().end(), after->caller_payload().begin()));
}
TEST(hybrid_conclusions_partial_submission_invalidation_is_terminal)
{
    Fixture f;
    auto c = captured(f);
    c.invalidate();
    REQUIRE(!c.freeze(true, 107, 108));
    REQUIRE(c.observe(datagram(f.caller)));
    REQUIRE(!c.freeze(true, 107, 108));
}
TEST(
    hybrid_conclusions_core_sequence_origins_are_bound_independently_of_dispatch_data_origin)
{
    Fixture f;
    const auto c = captured(f).freeze(true, 107, 108);
    REQUIRE(c);
    h::Authorization grant {.caller_group = f.caller.group_membership.group_id,
        .listener_group = f.listener.group_membership.group_id,
        .generation = 1,
        .expires_at_us = 1000};
    grant.credential_id.fill(std::byte {1});
    h::AdmissionTranscript t;
    REQUIRE(h::admission_transcript(
        f.ctx, grant, c->caller_payload(), c->listener_payload(), t));
    h::Message m {.kind = h::Kind::fallback_request,
        .stream = 71,
        .member = 97,
        .serial = 1,
        .next_position = 1234,
        .next_sequence = f.ctx.dispatch.initial_sequence,
        .mode = h::Mode::unicast};
    REQUIRE(h::valid(m, f.ctx));
    m.next_sequence = 1;
    REQUIRE(!h::valid(m, f.ctx));
    REQUIRE_EQ(c->dispatch(), f.ctx.dispatch);
    REQUIRE_EQ(c->source(), f.ctx.source);
}
TEST(
    hybrid_conclusions_outer_timestamp_and_destination_are_excluded_from_frozen_payload)
{
    Fixture f;
    auto c = captured(f);
    const auto first = c.freeze(true, 107, 108);
    REQUIRE(first);
    auto bytes = datagram(f.caller);
    bytes[11] ^= std::byte {1};
    bytes[15] ^= std::byte {1};
    REQUIRE(c.observe(bytes));
    const auto next = c.freeze(true, 107, 108);
    REQUIRE(next);
    REQUIRE(std::equal(first->caller_payload().begin(),
        first->caller_payload().end(), next->caller_payload().begin()));
}
#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_HYBRID_CONTROL
TEST(
    hybrid_conclusions_runtime_publication_is_single_assignment_and_lifetime_fenced)
{
    Fixture f;
    const auto pair = captured(f).freeze(true, 107, 108);
    REQUIRE(pair);
    compat::ConnectionRuntime r(
        {.local_socket_id = 107, .peer_socket_id = 108});
    REQUIRE(!r.hybrid_conclusions());
    REQUIRE(!r.publish_hybrid_conclusions(pair, false));
    REQUIRE(r.publish_hybrid_conclusions(pair, true));
    REQUIRE_EQ(r.hybrid_conclusions(), pair);
    REQUIRE(!r.publish_hybrid_conclusions(pair, true));
    r.mark_broken(1);
    REQUIRE(!r.hybrid_conclusions());
    auto closed = std::make_shared<compat::ConnectionRuntime>(
        compat::ConnectionRuntime::Configuration {
            .local_socket_id = 107, .peer_socket_id = 108});
    compat::close_connection_runtime(closed);
    REQUIRE(!closed->publish_hybrid_conclusions(pair, true));
}
TEST(hybrid_conclusions_listener_inbox_handoff_preserves_original_datagram)
{
    Fixture f;
    const auto bytes = datagram(f.caller);
    const auto decoded = decode_handshake_datagram(bytes);
    REQUIRE(decoded);
    compat::HandshakeEnvelope envelope {.message = decoded.message,
        .control = decoded.control,
        .peer = IpEndpoint::loopback(9000)};
    std::copy(bytes.begin(), bytes.end(), envelope.raw_datagram.begin());
    envelope.raw_size = bytes.size();
    compat::HandshakeInbox inbox(2);
    REQUIRE(inbox.push(envelope));
    compat::HandshakeEnvelope popped;
    REQUIRE_EQ(inbox.pop_for(popped, std::chrono::milliseconds {0}),
        compat::InboxPopStatus::received);
    REQUIRE_EQ(popped.raw_size, bytes.size());
    REQUIRE(
        std::equal(bytes.begin(), bytes.end(), popped.raw_datagram.begin()));
}
#endif
