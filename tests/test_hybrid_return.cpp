// SPDX-License-Identifier: MIT
#include "test.hpp"
#include "compat/hybrid_return.hpp"
#include "compat/hybrid_control.hpp"
#include "compat/hybrid_progress.hpp"
using namespace robotweax::srt;
namespace hr = robotweax::srt::compat::hybrid;
namespace {
hr::Context context(bool returning)
{
    hr::Context c;
    c.dispatch = {.group_id = group_handle_mask | 1U,
        .multicast_group = {.address = {239, 255, 1, 1}, .port = 6000},
        .initial_sequence = 42,
        .time_to_live = 1};
    c.source = {.kind = MulticastSourceKind::announcement,
        .group_id = c.dispatch.group_id,
        .endpoint = {.address = {192, 0, 2, 1}, .port = 9000}};
    c.offer.supported =
        returning ? hr::return_features : hr::policy_query_features;
    c.selection = c.offer;
    c.selection.kind = hr::CapabilityKind::selection;
    c.selection.stream = 71;
    c.selection.member = 97;
    c.selection.initial_position = 100;
    REQUIRE(hr::valid(c));
    return c;
}
}
TEST(
    hybrid_return_policy_requires_explicit_bounded_time_packets_coverage_and_age)
{
    REQUIRE(!hr::valid(hr::ReturnPolicy {}));
    hr::ReturnPolicy p {1000, 16, 2, 10, 500};
    REQUIRE(hr::valid(p));
    for (unsigned i = 0; i < 6; ++i) {
        auto bad = p;
        if (i == 0)
            bad.maximum_overlap_us = hr::maximum_age_us + 1;
        if (i == 1)
            bad.maximum_overlap_packets = 257;
        if (i == 2)
            bad.minimum_coverage_packets = 17;
        if (i == 3)
            bad.minimum_coverage_us = 1000;
        if (i == 4)
            bad.maximum_proof_age_us = 1001;
        if (i == 5)
            bad.minimum_coverage_packets = 0;
        REQUIRE(!hr::valid(bad));
    }
}
TEST(
    hybrid_return_capability_negotiates_dependencies_without_reinterpreting_legacy_status)
{
    auto c = context(true);
    REQUIRE_EQ(c.selection.supported, 127U);
    auto bad = c.offer;
    bad.supported &= ~hr::overlap;
    REQUIRE(!hr::valid(bad));
    bad = c.offer;
    bad.supported |= 256;
    REQUIRE(!hr::valid(bad));
    hr::Capability selected;
    REQUIRE(hr::negotiate(c.offer, c.offer, 71, 97, 100, selected));
    REQUIRE(selected.supported & hr::return_control);
    const auto legacy = context(false);
    REQUIRE(hr::negotiate(legacy.offer, c.offer, 71, 97, 100, selected));
    REQUIRE(!(selected.supported & hr::return_control));
}
TEST(hybrid_return_status_carries_effective_boundary_and_echo_in_distinct_words)
{
    const auto c = context(true);
    hr::Message m {.kind = hr::Kind::delivery_status,
        .stream = 71,
        .member = 97,
        .serial = 1,
        .revision = 2,
        .next_position = 105,
        .next_sequence = 47,
        .mode = hr::Mode::overlap,
        .retained_floor = 100,
        .multicast_next = 101,
        .request_serial = 9};
    REQUIRE(hr::valid(m, c));
    REQUIRE(!hr::valid(m, context(false)));
    std::array<std::byte, 96> wire {};
    REQUIRE(hr::encode(m, wire));
    REQUIRE_EQ(wire[71], std::byte {101});
    REQUIRE_EQ(wire[79], std::byte {9});
    hr::Message decoded;
    REQUIRE(hr::decode(wire, decoded));
    REQUIRE_EQ(decoded, m);
    m.kind = hr::Kind::delivery_commit;
    REQUIRE(!hr::valid(m, c));
    m.multicast_next = m.next_position;
    REQUIRE(hr::valid(m, c));
}
TEST(
    hybrid_return_ready_distinguishes_empty_overlap_intent_from_coverage_interval)
{
    const auto c = context(true);
    hr::Message m {.kind = hr::Kind::multicast_ready,
        .stream = 71,
        .member = 97,
        .serial = 1,
        .revision = 1,
        .next_position = 100,
        .next_sequence = 42,
        .mode = hr::Mode::overlap,
        .multicast_next = 100,
        .interval_us = 1};
    REQUIRE(hr::valid(m, c));
    m.mode = hr::Mode::unicast;
    REQUIRE(!hr::valid(m, c));
    m.mode = hr::Mode::multicast;
    m.next_position = 103;
    m.next_sequence = 45;
    m.interval_us = 100;
    REQUIRE(hr::valid(m, c));
    m.multicast_next = 104;
    REQUIRE(!hr::valid(m, c));
}

TEST(
    hybrid_return_exact_cache_proves_contiguous_mc_coverage_and_never_refreshes_old_copies)
{
    hr::ReceiveProgress p(SequenceNumber {0});
    const std::array<std::byte, 1> payload {std::byte {1}};
    const auto packet = [&](std::uint32_t n) {
        return PacketView {.kind = PacketKind::data,
            .data = {.sequence = SequenceNumber {n},
                .message_number = n + 1,
                .boundary = MessageBoundary::solo},
            .payload = payload};
    };
    p.receive(packet(0), true, hr::DataPath::multicast, 100);
    p.receive(packet(1), true, hr::DataPath::unicast, 110);
    p.receive(packet(2), true, hr::DataPath::multicast, 120);
    REQUIRE(!p.coverage(1000, 1003, 1000));
    p.receive(packet(1), false, hr::DataPath::multicast, 130);
    auto proof = p.coverage(1000, 1003, 1000);
    REQUIRE(proof);
    REQUIRE_EQ(proof->first_at_us, 100U);
    REQUIRE_EQ(proof->last_at_us, 130U);
    p.sync_floor(SequenceNumber {3});
    p.receive(packet(1), false, hr::DataPath::multicast, 300);
    REQUIRE_EQ(p.coverage(1000, 1003, 1000)->last_at_us, 130U);
    p.receive(packet(40), true, hr::DataPath::unicast, 400);
    REQUIRE(!p.coverage(1000, 1003, 1000));
    REQUIRE(!p.coverage(999, 1003, 1000));
}

TEST(
    hybrid_membership_capability_requires_policy_queries_and_preserves_old_masks)
{
    hr::Capability offer;
    offer.supported = hr::core_features | hr::startup_fallback;
    REQUIRE(!hr::valid(offer));
    offer.supported |= hr::policy_query;
    REQUIRE(hr::valid(offer));
    offer.required |= hr::startup_fallback;
    hr::Capability selection;
    auto legacy = context(true).offer;
    REQUIRE(!hr::negotiate(offer, legacy, 71, 97, 100, selection));
    offer.required = hr::core_features;
    REQUIRE(hr::negotiate(offer, legacy, 71, 97, 100, selection));
    REQUIRE(!(selection.supported & hr::startup_fallback));
    REQUIRE(hr::negotiate(offer, offer, 71, 97, 100, selection));
    REQUIRE(selection.supported & hr::startup_fallback);
    REQUIRE_EQ(hr::return_features, 127U);
    REQUIRE_EQ(hr::known_features, 255U);
    std::array<std::byte, 48> wire {};
    REQUIRE(hr::encode(selection, wire));
    hr::Capability decoded;
    REQUIRE(hr::decode(wire, decoded));
    REQUIRE_EQ(decoded, selection);
}
