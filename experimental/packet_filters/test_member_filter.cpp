#include "member_filter.hpp"
#include "test.hpp"
#include "compat/transport_runtime.hpp"

#include <algorithm>

using namespace robotweax::srt;
using namespace robotweax::srt::extensions;

TEST(
    experimental_filter_member_submits_only_initialized_prefix_and_zero_padding)
{
    const auto* filter = compiled_filter("rwx-repeat-v1");
    SKIP_UNLESS(filter != nullptr, "test uses bundled filter provider");
    MemberFilterRuntime sender;
    const SequenceNumber initial {SequenceNumber::mask - 1U};
    REQUIRE_EQ(sender.start(*filter, 11, 22, initial, initial, 32, 32, 1433),
        Error::none);
    // Reuse each queue slot with a much shorter packet. Unused capacity from a
    // previous repair must never appear in the next submitted wire prefix.
    const std::array<std::size_t, 10> lengths {
        1433, 1433, 1433, 1433, 1, 2, 3, 4, 100, 1316};
    std::array<std::byte, 1433> payload;
    for (std::size_t index = 0; index < lengths.size(); ++index) {
        payload.fill(static_cast<std::byte>(index + 37U));
        const auto sequence =
            initial.advanced(static_cast<std::uint32_t>(index));
        const PacketView packet {.kind = PacketKind::data,
            .data = {.sequence = sequence,
                .message_number = static_cast<std::uint32_t>(index + 1),
                .boundary = MessageBoundary::solo,
                .in_order = true,
                .timestamp = PacketTimestamp {static_cast<std::uint32_t>(
                    0x89abcdefU + index)},
                .destination_socket_id = 22},
            .payload = std::span {payload}.first(lengths[index])};
        REQUIRE_EQ(sender.source_sent(packet, sequence, index), Error::none);
        REQUIRE(sender.pending());
        const auto wire = *sender.pending();
        const auto decoded = decode_packet(wire);
        REQUIRE(decoded);
        std::array<std::byte, 1500> source;
        const auto encoded = encode_packet({.kind = PacketKind::data,
                                               .data = packet.data,
                                               .payload = packet.payload},
            source);
        REQUIRE(encoded);
        const auto symbol_size = encoded.bytes_written;
        const auto framed_size = (symbol_size + 7U) & ~std::size_t {3};
        REQUIRE_EQ(wire.size(), packet_header_size + framed_size);
        REQUIRE_EQ(decoded.packet.control.type, ControlType::user_defined);
        REQUIRE_EQ(decoded.packet.control.subtype, member_filter_subtype);
        REQUIRE_EQ(decoded.packet.control.type_specific, sequence.value());
        const auto body = decoded.packet.payload;
        REQUIRE_EQ(body[0], std::byte {1});
        REQUIRE_EQ(body[1], std::byte {0});
        REQUIRE_EQ(body[2], static_cast<std::byte>(symbol_size >> 8U));
        REQUIRE_EQ(body[3], static_cast<std::byte>(symbol_size & 255U));
        REQUIRE(std::equal(source.begin(),
            source.begin() + static_cast<std::ptrdiff_t>(symbol_size),
            body.begin() + 4));
        for (const auto padding : body.subspan(4 + symbol_size))
            REQUIRE_EQ(padding, std::byte {});
        sender.consume();
    }
}

namespace {
const CompiledFilter& bundled()
{
    const auto* filter = compiled_filter("rwx-repeat-v1");
    SKIP_UNLESS(filter != nullptr, "test uses bundled filter provider");
    return *filter;
}
struct Pair {
    MemberFilterRuntime sender;
    MemberFilterRuntime receiver;
    SequenceNumber first {SequenceNumber::mask};
    std::array<std::byte, 5> payload {std::byte {1}, std::byte {2},
        std::byte {3}, std::byte {4}, std::byte {5}};
    PacketView source() const
    {
        return {.kind = PacketKind::data,
            .data = {.sequence = first,
                .message_number = 1,
                .boundary = MessageBoundary::solo,
                .in_order = true,
                .timestamp = PacketTimestamp {123},
                .destination_socket_id = 22},
            .payload = payload};
    }
    void start()
    {
        REQUIRE_EQ(sender.start(bundled(), 11, 22, first, first, 8, 8, 1400),
            Error::none);
        REQUIRE_EQ(receiver.start(bundled(), 22, 11, first, first, 8, 8, 1400),
            Error::none);
    }
};
}

TEST(
    experimental_filter_member_repairs_dropped_source_and_preserves_metadata_across_rollover)
{
    Pair pair;
    pair.start();
    for (unsigned i = 0; i < 3; ++i) {
        const auto source = pair.source();
        REQUIRE_EQ(
            pair.sender.source_sent(source, pair.first, i * 10), Error::none);
        REQUIRE(pair.sender.pending());
        const auto decoded = decode_packet(*pair.sender.pending());
        REQUIRE(decoded);
        // Original DATA is deliberately absent. Only the generated repair arrives.
        const auto result = pair.receiver.receive_repair(
            decoded.packet, pair.first, i * 10 + 1);
        REQUIRE_EQ(result.error, Error::none);
        REQUIRE(result.admitted);
        REQUIRE_EQ(result.recovered.size(), 1U);
        const auto& recovered = result.recovered.front();
        REQUIRE_EQ(recovered.data.sequence, source.data.sequence);
        REQUIRE_EQ(recovered.data.message_number, source.data.message_number);
        REQUIRE_EQ(recovered.data.timestamp, source.data.timestamp);
        REQUIRE_EQ(recovered.data.boundary, MessageBoundary::solo);
        REQUIRE(recovered.data.in_order);
        REQUIRE(std::equal(pair.payload.begin(), pair.payload.end(),
            recovered.payload.begin()));
        pair.sender.consume();
        REQUIRE(!pair.sender.pending());
        REQUIRE_EQ(
            pair.receiver.receive_source(source, pair.first, i * 10 + 2).error,
            Error::none);
        pair.first = pair.first.next();
    }
}

TEST(
    experimental_filter_member_drops_malformed_frames_before_callbacks_and_remains_usable)
{
    Pair pair;
    pair.start();
    REQUIRE_EQ(
        pair.sender.source_sent(pair.source(), pair.first, 0), Error::none);
    for (unsigned invalid = 0; invalid != 7; ++invalid) {
        std::array<std::byte, 1500> storage {};
        const auto wire = *pair.sender.pending();
        std::copy(wire.begin(), wire.end(), storage.begin());
        auto decoded = decode_packet(std::span {storage}.first(wire.size()));
        REQUIRE(decoded);
        auto packet = decoded.packet;
        switch (invalid) {
        case 0:
            storage[16] = std::byte {2};
            break;
        case 1:
            storage[17] = std::byte {1};
            break;
        case 2:
            storage[18] = std::byte {255};
            break;
        case 3:
            storage[wire.size() - 1] = std::byte {1};
            break; // Padding must be zero.
        case 4:
            packet.control.type_specific = pair.first.advanced(8).value();
            break;
        case 5:
            packet.control.destination_socket_id = 33;
            break;
        case 6:
            packet.payload = packet.payload.first(packet.payload.size() - 1);
            break;
        }
        const auto result =
            pair.receiver.receive_repair(packet, pair.first, invalid + 1);
        REQUIRE_EQ(result.error, Error::none);
        REQUIRE(!result.admitted);
        REQUIRE(result.recovered.empty());
    }
    const auto decoded = decode_packet(*pair.sender.pending());
    const auto valid =
        pair.receiver.receive_repair(decoded.packet, pair.first, 20);
    REQUIRE_EQ(valid.error, Error::none);
    REQUIRE_EQ(valid.recovered.size(), 1U);
}

TEST(experimental_filter_member_queues_and_amplification_are_bounded_atomically)
{
    Pair pair;
    pair.start();
    for (unsigned i = 0; i < MemberFilterRuntime::maximum_outputs; ++i)
        REQUIRE_EQ(
            pair.sender.source_sent(pair.source(), pair.first, i), Error::none);
    REQUIRE_EQ(pair.sender.source_sent(pair.source(), pair.first, 5),
        Error::buffer_too_small);
    REQUIRE(!pair.sender.pending());
    auto noisy = bundled();
    noisy.implementation.on_event = [](std::span<std::byte>,
                                        const FilterEvent& event,
                                        FilterOutputWriter& writer) {
        const std::array payload {std::byte {1}};
        return writer.append({.context = event.environment.context,
            .domain = SequenceDomain::member_transport,
            .sequence = event.environment.first_sequence,
            .payload = payload});
    };
    MemberFilterRuntime unfunded;
    REQUIRE_EQ(
        unfunded.start(noisy, 11, 22, pair.first, pair.first, 8, 8, 1400),
        Error::none);
    // Polling cannot manufacture repair credit without submitted source traffic.
    REQUIRE_EQ(unfunded.poll_sender(pair.first, 1), Error::invalid_state);
    REQUIRE(!unfunded.pending());
    auto amplifying = bundled();
    amplifying.implementation.descriptor.maximum_outputs_per_batch = 2;
    amplifying.implementation.descriptor.maximum_output_bytes_per_batch = 2944;
    amplifying.implementation.on_event = [](std::span<std::byte>,
                                             const FilterEvent& event,
                                             FilterOutputWriter& writer) {
        if (!event.input)
            return Error::none;
        const FilterOutput output {.context = event.environment.context,
            .domain = SequenceDomain::member_transport,
            .sequence = event.input->sequence,
            .payload = event.input->payload};
        if (writer.append(output) != Error::none)
            return Error::invalid_state;
        return writer.append(output);
    };
    MemberFilterRuntime excessive;
    REQUIRE_EQ(
        excessive.start(amplifying, 11, 22, pair.first, pair.first, 8, 8, 1400),
        Error::none);
    REQUIRE_EQ(excessive.source_sent(pair.source(), pair.first, 1),
        Error::invalid_state);
    REQUIRE(!excessive.pending());
}

TEST(
    experimental_filter_member_example_drops_invalid_untrusted_symbols_without_failing)
{
    Pair pair;
    pair.start();
    REQUIRE_EQ(
        pair.sender.source_sent(pair.source(), pair.first, 0), Error::none);
    for (unsigned invalid = 0; invalid < 5; ++invalid) {
        std::array<std::byte, 1500> changed {};
        const auto wire = *pair.sender.pending();
        std::copy(wire.begin(), wire.end(), changed.begin());
        // Framing is valid, but the opaque symbol is not acceptable DATA.
        if (invalid == 0)
            changed[20] |= std::byte {128};
        if (invalid == 1)
            changed[27] = std::byte {0};
        if (invalid == 2)
            changed[35] = std::byte {33};
        if (invalid == 3)
            changed[24] |= std::byte {8};
        if (invalid == 4)
            changed[23] ^= std::byte {1};
        const auto packet =
            decode_packet(std::span {changed}.first(wire.size()));
        REQUIRE(packet);
        const auto result = pair.receiver.receive_repair(
            packet.packet, pair.first, invalid + 1);
        REQUIRE_EQ(result.error, Error::none);
        REQUIRE(result.recovered.empty());
    }
    const auto packet = decode_packet(*pair.sender.pending());
    REQUIRE_EQ(pair.receiver.receive_repair(packet.packet, pair.first, 10)
                   .recovered.size(),
        1U);
}

TEST(
    experimental_filter_member_rejects_invalid_recovered_headers_as_one_terminal_batch)
{
    Pair pair;
    pair.start();
    REQUIRE_EQ(
        pair.sender.source_sent(pair.source(), pair.first, 0), Error::none);
    for (unsigned invalid = 0; invalid != 4; ++invalid) {
        auto bad = bundled();
        // The trusted fixture emits a valid candidate followed by an invalid one.
        static unsigned selected;
        selected = invalid;
        bad.implementation.descriptor.maximum_outputs_per_batch = 2;
        bad.implementation.descriptor.maximum_output_bytes_per_batch = 2944;
        bad.implementation.on_event = [](std::span<std::byte>,
                                          const FilterEvent& event,
                                          FilterOutputWriter& writer) {
            if (!event.input)
                return Error::none;
            FilterOutput output {.context = event.environment.context,
                .kind = FilterOutputKind::recovered_data,
                .domain = SequenceDomain::member_transport,
                .sequence = event.input->sequence,
                .payload = event.input->payload};
            if (writer.append(output) != Error::none)
                return Error::invalid_state;
            std::array<std::byte, 1500> changed {};
            const auto bytes = event.input->payload;
            std::copy(bytes.begin(), bytes.end(), changed.begin());
            if (selected == 0)
                changed[15] = std::byte {33}; // Foreign destination.
            if (selected == 1)
                changed[7] = std::byte {0}; // Reserved message number.
            if (selected == 2)
                changed[3] ^= std::byte {1}; // Different sequence.
            if (selected == 3)
                changed[4] |= std::byte {8}; // Encrypted candidate.
            output.payload = std::span {changed}.first(bytes.size());
            return writer.append(output);
        };
        MemberFilterRuntime receiver;
        REQUIRE_EQ(
            receiver.start(bad, 22, 11, pair.first, pair.first, 8, 8, 1400),
            Error::none);
        const auto decoded = decode_packet(*pair.sender.pending());
        const auto result =
            receiver.receive_repair(decoded.packet, pair.first, 1);
        REQUIRE(result.error != Error::none);
        REQUIRE(result.recovered.empty());
        REQUIRE(receiver.receive_repair(decoded.packet, pair.first, 2).error
            != Error::none);
    }
}

TEST(experimental_filter_registry_copies_names_and_rejects_conflicting_tables)
{
    auto filter = bundled();
    std::array<char, 13> name {
        'r', 'w', 'x', '-', 'c', 'o', 'p', 'y', '-', 'v', '1'};
    filter.name = {name.data(), 11};
    const FilterRegistry registry {std::span {&filter, 1}};
    REQUIRE_EQ(registry.error(), Error::none);
    name.fill('x');
    REQUIRE(registry.find("rwx-copy-v1") != nullptr);
    REQUIRE(registry.find("rwx-repeat-v1") == nullptr);
    const std::array duplicates {bundled(), bundled()};
    const FilterRegistry invalid {duplicates};
    REQUIRE_EQ(invalid.error(), Error::invalid_state);
    REQUIRE(invalid.find("rwx-repeat-v1") == nullptr);
}

TEST(
    experimental_filter_runtime_recovers_a_lost_packet_through_core_tsbpd_and_deduplication)
{
    Pair pair;
    pair.start();
    auto source = pair.source();
    source.data.timestamp = PacketTimestamp {};
    REQUIRE_EQ(pair.sender.source_sent(source, pair.first, 0), Error::none);
    const auto repair = decode_packet(*pair.sender.pending());
    REQUIRE(repair);
    SocketOptions options;
    REQUIRE_EQ(options.set_packet_filter("rwx-repeat-v1"), Error::none);
    const auto peer = IpEndpoint::loopback(9000);
    std::uint64_t now = 1;
    compat::ConnectionRuntime runtime {{.peer = peer,
        .local_socket_id = 22,
        .peer_socket_id = 11,
        .initial_sequence = pair.first,
        .flow_window_packets = 8,
        .options = options,
        .negotiated_options = {.receive_tsbpd = true,
            .receive_delay_milliseconds = 120},
        .now_function =
            [](void* value) noexcept {
                return *static_cast<std::uint64_t*>(value);
            },
        .now_context = &now}};
    // Drop the original. Repeated repair and a late original must still yield
    // exactly one application message, released by the core's TSBPD clock.
    runtime.process_packet(repair.packet, peer);
    REQUIRE(!runtime.broken());
    now = 2;
    runtime.process_packet(repair.packet, peer);
    now = 3;
    runtime.process_packet(source, peer);
    std::array<std::byte, 1500> received {};
    REQUIRE_EQ(runtime.receive_message(received, false, 0).status,
        compat::MessageIoStatus::would_block);
    now = 200000;
    const auto result = runtime.receive_message(received, false, 0);
    REQUIRE_EQ(result.status, compat::MessageIoStatus::success);
    REQUIRE_EQ(result.bytes, pair.payload.size());
    REQUIRE(
        std::equal(pair.payload.begin(), pair.payload.end(), received.begin()));
    REQUIRE_EQ(runtime.receive_message(received, false, 0).status,
        compat::MessageIoStatus::would_block);
    const auto before_late = runtime.statistics(false, true).total;
    now = 200001;
    runtime.process_packet(source, peer);
    const auto after_late = runtime.statistics(false, true).total;
    REQUIRE_EQ(after_late.received.packets, before_late.received.packets + 1);
    REQUIRE_EQ(after_late.receiver_belated.packets,
        before_late.receiver_belated.packets + 1);
    REQUIRE_EQ(runtime.receive_message(received, false, 0).status,
        compat::MessageIoStatus::would_block);
    compat::ConnectionRuntime unselected {{.peer = peer,
        .local_socket_id = 22,
        .peer_socket_id = 11,
        .initial_sequence = pair.first,
        .flow_window_packets = 8,
        .now_function =
            [](void* value) noexcept {
                return *static_cast<std::uint64_t*>(value);
            },
        .now_context = &now}};
    unselected.process_packet(repair.packet, peer);
    REQUIRE(!unselected.broken());
    REQUIRE_EQ(unselected.receive_message(received, false, 0).status,
        compat::MessageIoStatus::would_block);
}

TEST(
    experimental_filter_runtime_retries_original_and_repair_once_under_shared_pacing)
{
    (void)bundled();
    struct Capture {
        unsigned data_attempts = 0;
        unsigned repair_attempts = 0;
        unsigned originals = 0;
        unsigned repairs = 0;
        std::array<SequenceNumber, 3> source_sequences {};
        std::array<SequenceNumber, 3> repair_sequences {};
    } captured;
    const auto channel = std::make_shared<compat::DatagramChannel>();
    channel->set_send_hook_for_testing(
        [](std::span<const std::byte> bytes, IpEndpoint,
            void* context) noexcept -> UdpIoResult {
            auto& seen = *static_cast<Capture*>(context);
            const auto packet = decode_packet(bytes);
            if (!packet)
                return {.error = Error::invalid_state};
            if (packet.packet.kind == PacketKind::data) {
                if (++seen.data_attempts == 1)
                    return {.error = Error::would_block};
                if (seen.originals < seen.source_sequences.size())
                    seen.source_sequences[seen.originals] =
                        packet.packet.data.sequence;
                ++seen.originals;
            } else if (packet.packet.control.type == ControlType::user_defined
                && packet.packet.control.subtype == member_filter_subtype) {
                if (++seen.repair_attempts == 1)
                    return {.error = Error::would_block};
                if (seen.repairs < seen.repair_sequences.size())
                    seen.repair_sequences[seen.repairs] =
                        SequenceNumber {packet.packet.control.type_specific};
                ++seen.repairs;
            }
            return {.bytes_transferred = bytes.size()};
        },
        &captured);
    SocketOptions options;
    REQUIRE_EQ(options.set_packet_filter("rwx-repeat-v1"), Error::none);
    REQUIRE_EQ(
        options.set(SocketOption::maximum_bandwidth_bytes_per_second, 1000),
        Error::none);
    std::uint64_t now = 0;
    compat::ConnectionRuntime runtime {{.channel = channel,
        .peer = IpEndpoint::loopback(9000),
        .local_socket_id = 11,
        .peer_socket_id = 22,
        .initial_sequence = SequenceNumber {10},
        .flow_window_packets = 8,
        .options = options,
        .negotiated_options = {.peer_periodic_nak = true},
        .now_function =
            [](void* value) noexcept {
                return *static_cast<std::uint64_t*>(value);
            },
        .now_context = &now}};
    const std::array<std::byte, 4> payload {};
    for (unsigned i = 0; i < 3; ++i)
        REQUIRE_EQ(runtime.queue_message(payload, 0, true, false, -1).status,
            compat::MessageIoStatus::success);
    (void)runtime.poll();
    REQUIRE_EQ(captured.data_attempts, 1U);
    REQUIRE_EQ(captured.originals, 0U);
    REQUIRE_EQ(captured.repair_attempts, 0U);
    now = 2000;
    (void)runtime.poll();
    REQUIRE_EQ(captured.originals, 1U);
    REQUIRE_EQ(captured.repair_attempts,
        0U); // Source submission consumed this pacing slot.
    for (unsigned tick = 0; tick < 20 && captured.repairs < 3; ++tick) {
        now += 100000;
        (void)runtime.poll();
        REQUIRE(!runtime.broken());
    }
    REQUIRE_EQ(captured.originals, 3U);
    REQUIRE_EQ(captured.repairs, 3U);
    REQUIRE_EQ(captured.data_attempts, 4U);
    REQUIRE_EQ(captured.repair_attempts, 4U);
    REQUIRE_EQ(captured.source_sequences, captured.repair_sequences);
    runtime.close();
    (void)runtime.poll();
    REQUIRE_EQ(captured.data_attempts, 4U);
    REQUIRE_EQ(captured.repair_attempts, 4U);
}

TEST(
    experimental_filter_runtime_keeps_outside_window_originals_in_core_loss_detection)
{
    (void)bundled();
    for (const auto initial :
        {SequenceNumber {100}, SequenceNumber {SequenceNumber::mask - 2}}) {
        struct Observation {
            SequenceNumber initial;
            bool saw_loss = false;
            bool invalid_loss = false;
        } seen {initial};
        const auto channel = std::make_shared<compat::DatagramChannel>();
        channel->set_send_hook_for_testing(
            [](std::span<const std::byte> bytes, IpEndpoint,
                void* context) noexcept -> UdpIoResult {
                auto& output = *static_cast<Observation*>(context);
                const auto decoded = decode_packet(bytes);
                if (decoded && decoded.packet.kind == PacketKind::control
                    && decoded.packet.control.type
                        == ControlType::negative_acknowledgement) {
                    const auto loss = decode_loss_range(decoded.packet.payload);
                    output.saw_loss = true;
                    output.invalid_loss = output.invalid_loss || !loss
                        || loss.range.first != output.initial
                        || loss.range.last != output.initial.advanced(7);
                }
                return {.bytes_transferred = bytes.size()};
            },
            &seen);
        SocketOptions options;
        REQUIRE_EQ(options.set_packet_filter("rwx-repeat-v1"), Error::none);
        const auto peer = IpEndpoint::loopback(9000);
        std::uint64_t now = 100;
        compat::ConnectionRuntime runtime {{.channel = channel,
            .peer = peer,
            .local_socket_id = 22,
            .peer_socket_id = 11,
            .initial_sequence = initial,
            .flow_window_packets = 8,
            .options = options,
            .negotiated_options = {.periodic_nak = true},
            .now_function =
                [](void* value) noexcept {
                    return *static_cast<std::uint64_t*>(value);
                },
            .now_context = &now}};
        const std::array payload {std::byte {1}};
        const PacketView source {.kind = PacketKind::data,
            .data = {.sequence = initial.advanced(8),
                .message_number = 9,
                .boundary = MessageBoundary::solo,
                .destination_socket_id = 22},
            .payload = payload};
        // This source cannot enter the filter's bounded symbol window, but it
        // still proves the missing current receive window to ordinary ARQ.
        runtime.process_packet(source, peer);
        REQUIRE(!runtime.broken());
        REQUIRE(seen.saw_loss);
        REQUIRE(!seen.invalid_loss);
    }
}
