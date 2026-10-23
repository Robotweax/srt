#include "member_filter.hpp"
#include "compat/transport_runtime.hpp"
#include "xor_filter.hpp"
#include "test.hpp"

#include <algorithm>

using namespace robotweax::srt;
using namespace robotweax::srt::extensions;
namespace {
struct XorPair {
    MemberFilterRuntime sender;
    MemberFilterRuntime receiver;
    SequenceNumber initial;
    std::array<std::array<std::byte, 1433>, 8> payloads {};
    std::array<std::byte, 1500> wire {};
    std::size_t wire_size = 0;
    std::uint64_t now = 0;
    explicit XorPair(SequenceNumber first = SequenceNumber {0})
        : initial(first)
    {
        const auto filter = xor8_filter();
        REQUIRE_EQ(sender.start(filter, 11, 22, first, first, 4096, 4096, 1433),
            Error::none);
        REQUIRE_EQ(
            receiver.start(filter, 22, 11, first, first, 4096, 4096, 1433),
            Error::none);
        for (std::size_t i = 0; i < payloads.size(); ++i)
            for (std::size_t j = 0; j < payloads[i].size(); ++j)
                payloads[i][j] = static_cast<std::byte>((i * 37 + j) & 255U);
    }
    PacketView source(std::uint32_t offset) const
    {
        constexpr std::array<std::size_t, 8> lengths {
            1, 1433, 5, 1316, 27, 2, 1432, 101};
        return {.kind = PacketKind::data,
            .data = {.sequence = initial.advanced(offset),
                .message_number = offset + 1,
                .boundary = MessageBoundary::solo,
                .in_order = true,
                .timestamp = PacketTimestamp {0xfffffff0U + offset},
                .destination_socket_id = 22},
            .payload =
                std::span {payloads[offset % 8]}.first(lengths[offset % 8])};
    }
    void send(std::uint32_t offset)
    {
        REQUIRE_EQ(
            sender.source_sent(source(offset), initial, now++), Error::none);
        if (const auto pending = sender.pending()) {
            wire_size = pending->size();
            std::copy(pending->begin(), pending->end(), wire.begin());
            sender.consume();
        }
    }
    void block(std::uint32_t offset = 0)
    {
        wire_size = 0;
        for (std::uint32_t i = 0; i < 8; ++i)
            send(offset + i);
        REQUIRE(wire_size != 0);
    }
    MemberFilterReceiveResult receive(
        std::uint32_t offset, bool retransmitted = false)
    {
        auto packet = source(offset);
        packet.data.retransmitted = retransmitted;
        return receiver.receive_source(packet, initial, now++);
    }
    MemberFilterReceiveResult repair()
    {
        const auto decoded = decode_packet(std::span {wire}.first(wire_size));
        REQUIRE(decoded);
        return receiver.receive_repair(decoded.packet, initial, now++);
    }
    void check(const MemberFilterReceiveResult& result, std::uint32_t offset)
    {
        REQUIRE_EQ(result.error, Error::none);
        REQUIRE_EQ(result.recovered.size(), std::size_t {1});
        const auto expected = source(offset);
        const auto& actual = result.recovered[0];
        REQUIRE_EQ(actual.data.sequence, expected.data.sequence);
        REQUIRE_EQ(actual.data.message_number, expected.data.message_number);
        REQUIRE_EQ(actual.data.timestamp, expected.data.timestamp);
        REQUIRE_EQ(actual.data.boundary, expected.data.boundary);
        REQUIRE_EQ(actual.data.in_order, expected.data.in_order);
        REQUIRE_EQ(actual.data.destination_socket_id, 22U);
        REQUIRE(!actual.data.retransmitted);
        REQUIRE(std::equal(actual.payload.begin(), actual.payload.end(),
            expected.payload.begin(), expected.payload.end()));
    }
};
}

TEST(
    experimental_xor8_recovers_each_single_erasure_with_variable_lengths_at_maximum_mss)
{
    for (std::uint32_t lost = 0; lost < 8; ++lost) {
        XorPair pair;
        pair.block();
        REQUIRE_EQ(pair.wire_size, std::size_t {1472});
        for (std::uint32_t i = 0; i < 8; ++i)
            if (i != lost) {
                const auto result = pair.receive(i);
                REQUIRE_EQ(result.error, Error::none);
                REQUIRE(result.recovered.empty());
            }
        pair.check(pair.repair(), lost);
        REQUIRE(pair.repair().recovered.empty());
    }
}

TEST(
    experimental_xor8_accepts_repair_first_reversed_sources_duplicates_and_arq_flags)
{
    XorPair pair;
    pair.block();
    REQUIRE(pair.repair().recovered.empty());
    for (std::uint32_t i = 7; i > 0; --i) {
        const auto result = pair.receive(i, true);
        if (i == 1)
            pair.check(result, 0);
        else {
            REQUIRE_EQ(result.error, Error::none);
            REQUIRE(result.recovered.empty());
        }
        REQUIRE(pair.receive(i, true).recovered.empty());
    }
}

TEST(
    experimental_xor8_two_erasures_wait_for_arq_and_complete_blocks_emit_no_duplicates)
{
    XorPair pair;
    pair.block();
    for (std::uint32_t i = 2; i < 8; ++i)
        REQUIRE(pair.receive(i).recovered.empty());
    REQUIRE(pair.repair().recovered.empty());
    pair.check(pair.receive(1, true), 0);
    REQUIRE(pair.receive(0).recovered.empty());
    XorPair complete;
    complete.block();
    for (std::uint32_t i = 0; i < 8; ++i)
        REQUIRE(complete.receive(i).recovered.empty());
    REQUIRE(complete.repair().recovered.empty());
}

TEST(
    experimental_xor8_skips_partial_initial_block_and_handles_sequence_and_timestamp_rollover)
{
    XorPair partial(SequenceNumber {5});
    for (std::uint32_t i = 0; i < 10; ++i) {
        partial.send(i);
        REQUIRE_EQ(partial.wire_size, std::size_t {0});
    }
    partial.send(10);
    REQUIRE(partial.wire_size != 0);
    XorPair rollover(SequenceNumber {SequenceNumber::mask - 7U});
    for (std::uint32_t block = 0; block < 2; ++block) {
        rollover.block(block * 8);
        for (std::uint32_t i = 1; i < 8; ++i)
            REQUIRE(rollover.receive(block * 8 + i).recovered.empty());
        rollover.check(rollover.repair(), block * 8);
    }
}

TEST(
    experimental_xor8_rejects_bad_magic_length_and_reconstructed_destination_without_terminal_error)
{
    for (const auto field :
        {std::size_t {0}, std::size_t {1}, std::size_t {2}, std::size_t {15}}) {
        XorPair pair;
        pair.block();
        for (std::uint32_t i = 1; i < 8; ++i)
            REQUIRE(pair.receive(i).recovered.empty());
        // The member envelope precedes the provider's symbol by four bytes.
        pair.wire[packet_header_size + 4 + field] ^= std::byte {0x80};
        const auto bad = pair.repair();
        REQUIRE_EQ(bad.error, Error::none);
        REQUIRE(bad.recovered.empty());
        REQUIRE_EQ(pair.receive(0).error, Error::none);
    }
}

TEST(
    experimental_xor8_cache_collisions_ignore_old_traffic_without_corrupting_new_blocks)
{
    XorPair pair;
    pair.block();
    const auto old_wire = pair.wire;
    const auto old_size = pair.wire_size;
    for (std::uint32_t i = 1; i < 8; ++i)
        REQUIRE(pair.receive(i).recovered.empty());
    for (std::uint32_t block = 1; block <= 64; ++block) {
        pair.block(block * 8);
        for (std::uint32_t i = block == 64 ? 1U : 0U; i < 8; ++i)
            REQUIRE(pair.receive(block * 8 + i).recovered.empty());
    }
    const auto latest = pair.wire;
    const auto latest_size = pair.wire_size;
    pair.wire = old_wire;
    pair.wire_size = old_size;
    REQUIRE(pair.repair().recovered.empty());
    REQUIRE(pair.receive(0).recovered.empty());
    pair.wire = latest;
    pair.wire_size = latest_size;
    pair.check(pair.repair(), 512);
}

TEST(
    experimental_filter_member_state_budget_rejects_excess_and_failed_large_construction)
{
    auto filter = xor8_filter();
    filter.implementation.descriptor.state_bytes =
        MemberFilterRuntime::maximum_state + 1;
    MemberFilterRuntime oversized;
    REQUIRE_EQ(oversized.start(filter, 11, 22, SequenceNumber {0},
                   SequenceNumber {0}, 32, 32, 1433),
        Error::unsupported);
    filter = xor8_filter();
    filter.implementation.construct = [](std::span<std::byte>,
                                          const FilterOwner&, FilterLimits,
                                          const FilterEnvironment&) {
        return Error::unsupported;
    };
    MemberFilterRuntime rejected;
    REQUIRE_EQ(rejected.start(filter, 11, 22, SequenceNumber {0},
                   SequenceNumber {0}, 32, 32, 1433),
        Error::unsupported);
}

TEST(
    experimental_xor8_runtime_recovers_only_missing_data_and_preserves_tsbpd_and_deduplication)
{
    const auto* filter = compiled_filter("rwx-xor8-v1");
    SKIP_UNLESS(filter != nullptr, "test uses bundled XOR provider");
    const SequenceNumber initial {0};
    MemberFilterRuntime sender;
    REQUIRE_EQ(sender.start(*filter, 11, 22, initial, initial, 32, 32, 1433),
        Error::none);
    std::array<std::array<std::byte, 32>, 8> payloads {};
    std::array<PacketView, 8> sources {};
    for (std::uint32_t i = 0; i < 8; ++i) {
        payloads[i].fill(static_cast<std::byte>(i + 1));
        sources[i] = {.kind = PacketKind::data,
            .data = {.sequence = initial.advanced(i),
                .message_number = i + 1,
                .boundary = MessageBoundary::solo,
                .in_order = true,
                .timestamp = PacketTimestamp {0},
                .destination_socket_id = 22},
            .payload = payloads[i]};
        REQUIRE_EQ(sender.source_sent(sources[i], initial, i), Error::none);
    }
    REQUIRE(sender.pending());
    const auto repair = decode_packet(*sender.pending());
    REQUIRE(repair);
    SocketOptions options;
    REQUIRE_EQ(options.set_packet_filter("rwx-xor8-v1"), Error::none);
    const auto peer = IpEndpoint::loopback(9000);
    const auto channel = std::make_shared<compat::DatagramChannel>();
    channel->set_send_hook_for_testing(
        [](std::span<const std::byte> bytes, IpEndpoint,
            void*) noexcept -> UdpIoResult {
            return {.bytes_transferred = bytes.size()};
        },
        nullptr);
    std::uint64_t now = 1;
    compat::ConnectionRuntime runtime {{.channel = channel,
        .peer = peer,
        .local_socket_id = 22,
        .peer_socket_id = 11,
        .initial_sequence = initial,
        .flow_window_packets = 32,
        .options = options,
        .negotiated_options = {.receive_tsbpd = true,
            .receive_delay_milliseconds = 120},
        .now_function =
            [](void* value) noexcept {
                return *static_cast<std::uint64_t*>(value);
            },
        .now_context = &now}};
    for (std::uint32_t i = 0; i < 8; ++i)
        if (i != 3)
            runtime.process_packet(sources[i], peer);
    runtime.process_packet(repair.packet, peer);
    runtime.process_packet(repair.packet, peer);
    REQUIRE(!runtime.broken());
    std::array<std::byte, 1500> received {};
    REQUIRE_EQ(runtime.receive_message(received, false, 0).status,
        compat::MessageIoStatus::would_block);
    now = 200000;
    for (std::size_t i = 0; i < 8; ++i) {
        const auto result = runtime.receive_message(received, false, 0);
        REQUIRE_EQ(result.status, compat::MessageIoStatus::success);
        REQUIRE_EQ(result.bytes, payloads[i].size());
        REQUIRE(std::equal(
            payloads[i].begin(), payloads[i].end(), received.begin()));
    }
    runtime.process_packet(sources[3], peer);
    REQUIRE_EQ(runtime.receive_message(received, false, 0).status,
        compat::MessageIoStatus::would_block);
    REQUIRE_EQ(runtime.statistics(false, true).total.receiver_filter_supply,
        std::uint64_t {1});
}

TEST(experimental_xor8_has_a_fixed_known_answer_repair_symbol)
{
    MemberFilterRuntime sender;
    const auto filter = xor8_filter();
    const SequenceNumber initial {0};
    REQUIRE_EQ(sender.start(filter, 11, 22, initial, initial, 32, 32, 1433),
        Error::none);
    for (std::uint32_t i = 0; i < 8; ++i) {
        const std::array payload {static_cast<std::byte>(i + 1)};
        const PacketView packet {.kind = PacketKind::data,
            .data = {.sequence = initial.advanced(i),
                .message_number = 1,
                .boundary = MessageBoundary::solo,
                .destination_socket_id = 22},
            .payload = payload};
        REQUIRE_EQ(sender.source_sent(packet, initial, i), Error::none);
        REQUIRE_EQ(sender.pending().has_value(), i == 7);
    }
    const auto decoded = decode_packet(*sender.pending());
    REQUIRE(decoded);
    // Identical header words XOR to zero; 1^2^3^4^5^6^7^8 is8.
    const std::array<std::byte, 17> expected {std::byte {0x58}, std::byte {1},
        std::byte {}, std::byte {}, std::byte {}, std::byte {}, std::byte {},
        std::byte {}, std::byte {}, std::byte {}, std::byte {}, std::byte {},
        std::byte {}, std::byte {}, std::byte {}, std::byte {}, std::byte {8}};
    const auto symbol = decoded.packet.payload.subspan(4, expected.size());
    REQUIRE(std::equal(symbol.begin(), symbol.end(), expected.begin()));
}

TEST(experimental_xor8_preserves_the_oldest_of_sixty_four_live_blocks)
{
    XorPair pair;
    pair.block();
    const auto oldest = pair.wire;
    const auto oldest_size = pair.wire_size;
    for (std::uint32_t i = 1; i < 8; ++i)
        REQUIRE(pair.receive(i).recovered.empty());
    for (std::uint32_t block = 1; block < 64; ++block) {
        pair.block(block * 8);
        for (std::uint32_t i = 0; i < 8; ++i)
            REQUIRE(pair.receive(block * 8 + i).recovered.empty());
    }
    pair.wire = oldest;
    pair.wire_size = oldest_size;
    pair.check(pair.repair(), 0);
}
