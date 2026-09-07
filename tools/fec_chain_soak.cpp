#include "fec_chain_fixtures.hpp"

#include "robotweax/srt/fec.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <span>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using robotweax::srt::DataHeader;
using robotweax::srt::EncryptionKey;
using robotweax::srt::Error;
using robotweax::srt::MatrixFecDecoder;
using robotweax::srt::MatrixFecEncoder;
using robotweax::srt::MessageBoundary;
using robotweax::srt::PacketFilterArqLevel;
using robotweax::srt::PacketFilterConfiguration;
using robotweax::srt::PacketFilterLayout;
using robotweax::srt::PacketKind;
using robotweax::srt::PacketTimestamp;
using robotweax::srt::PacketView;
using robotweax::srt::SequenceNumber;
using robotweax::srt::fec_chain_fixture::Fixture;

constexpr std::size_t maximum_iterations = 1'000'000U;
constexpr std::uint64_t invalid_column_group =
    std::numeric_limits<std::uint64_t>::max();

struct Options {
    std::size_t iterations = 16U;
    std::uint64_t seed = 0x5eed'fec0'2026U;
};

class DeterministicRandom {
public:
    explicit DeterministicRandom(std::uint64_t seed) noexcept
        : state_(seed == 0U ? 1U : seed)
    {
    }

    [[nodiscard]] std::uint64_t next() noexcept
    {
        state_ ^= state_ << 13U;
        state_ ^= state_ >> 7U;
        state_ ^= state_ << 17U;
        return state_;
    }

    [[nodiscard]] std::size_t below(std::size_t exclusive_maximum) noexcept
    {
        return static_cast<std::size_t>(next() % exclusive_maximum);
    }

private:
    std::uint64_t state_;
};

struct OwnedSource {
    DataHeader header {};
    std::array<std::byte, 257U> payload {};
    std::size_t payload_size = 0U;

    [[nodiscard]] PacketView view() const noexcept
    {
        return {
            .kind = PacketKind::data,
            .data = header,
            .payload = std::span {payload}.first(payload_size),
        };
    }
};

struct OwnedControl {
    DataHeader header {};
    std::vector<std::byte> payload;

    [[nodiscard]] PacketView view() const noexcept
    {
        return {
            .kind = PacketKind::data,
            .data = header,
            .payload = payload,
        };
    }
};

struct Statistics {
    std::uint64_t scenarios = 0U;
    std::uint64_t encoded_sources = 0U;
    std::uint64_t delivered_sources = 0U;
    std::uint64_t controls = 0U;
    std::uint64_t reconstructed = 0U;
    std::uint64_t checksum = 0U;
    std::size_t maximum_chain = 0U;
    std::size_t maximum_decoder_bytes = 0U;
};

[[nodiscard]] bool parse_unsigned(
    std::string_view text, std::uint64_t& value) noexcept
{
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto parsed = std::from_chars(begin, end, value);
    return parsed.ec == std::errc {} && parsed.ptr == end;
}

[[nodiscard]] bool parse_options(int argc, char** argv, Options& options)
{
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument {argv[index]};
        if ((argument == "--iterations" || argument == "--seed")
            && index + 1 < argc) {
            std::uint64_t value = 0U;
            if (!parse_unsigned(argv[++index], value) || value == 0U) {
                return false;
            }
            if (argument == "--iterations") {
                if (value > maximum_iterations) {
                    return false;
                }
                options.iterations = static_cast<std::size_t>(value);
            } else {
                options.seed = value;
            }
            continue;
        }
        return false;
    }
    return options.iterations != 0U;
}

[[nodiscard]] PacketFilterConfiguration configuration(const Fixture& fixture)
{
    return {
        .enabled = true,
        .columns = fixture.columns,
        .rows = static_cast<std::int32_t>(fixture.rows),
        .layout = fixture.layout,
        .arq = PacketFilterArqLevel::on_request,
        .columns_specified = true,
        .rows_specified = true,
        .layout_specified = true,
        .arq_specified = true,
    };
}

[[nodiscard]] std::uint64_t column_group(
    const Fixture& fixture, std::uint32_t source_index)
{
    const std::uint32_t column = source_index % fixture.columns;
    const std::uint32_t row = source_index / fixture.columns;
    const std::uint32_t offset =
        fixture.layout == PacketFilterLayout::even ? 0U : column % fixture.rows;
    if (row < offset) {
        return invalid_column_group;
    }
    const std::uint64_t series = (row - offset) / fixture.rows;
    return series * fixture.columns + column;
}

[[nodiscard]] bool validate_fixture(const Fixture& fixture)
{
    if (fixture.columns < 2U || fixture.columns > 128U || fixture.rows < 2U
        || fixture.rows > static_cast<std::uint32_t>(
               std::numeric_limits<std::int32_t>::max())
        || fixture.maximum_payload_size == 0U
        || fixture.maximum_payload_size > 257U || fixture.source_count == 0U
        || fixture.source_count > static_cast<std::size_t>(
               std::numeric_limits<std::uint32_t>::max())
        || fixture.source_count > fixture.receive_capacity
        || fixture.receive_capacity >= SequenceNumber::half_range
        || static_cast<std::uint64_t>(fixture.columns) * fixture.rows
            > fixture.receive_capacity) {
        return false;
    }
    std::vector<bool> seen(fixture.source_count, false);
    for (std::size_t index = 0U; index < fixture.cycle.size(); ++index) {
        const std::uint32_t source = fixture.cycle[index];
        if (source >= fixture.source_count || seen[source]) {
            return false;
        }
        seen[source] = true;
        const std::uint32_t next =
            fixture.cycle[(index + 1U) % fixture.cycle.size()];
        if (index % 2U == 0U) {
            if (source / fixture.columns != next / fixture.columns) {
                return false;
            }
        } else if (column_group(fixture, source) == invalid_column_group
            || column_group(fixture, source) != column_group(fixture, next)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] EncryptionKey encryption_key(std::size_t index) noexcept
{
    switch (index % 3U) {
    case 1U:
        return EncryptionKey::even;
    case 2U:
        return EncryptionKey::odd;
    default:
        return EncryptionKey::none;
    }
}

void shuffle(std::vector<std::uint32_t>& values, DeterministicRandom& random)
{
    for (std::size_t index = values.size(); index > 1U; --index) {
        const std::size_t replacement = random.below(index);
        std::swap(values[index - 1U], values[replacement]);
    }
}

[[nodiscard]] bool fail(
    const Fixture& fixture, std::uint64_t seed, std::string_view description)
{
    std::cerr << "FEC chain failure: fixture=" << fixture.name
              << " seed=" << seed << " reason=" << description << '\n';
    return false;
}

[[nodiscard]] bool run_fixture(
    const Fixture& fixture, std::uint64_t seed, Statistics& statistics)
{
    if (!validate_fixture(fixture)) {
        return fail(fixture, seed, "invalid retained fixture");
    }
    const auto filter = configuration(fixture);
    MatrixFecEncoder encoder {
        filter, fixture.initial_sequence, fixture.maximum_payload_size};
    MatrixFecDecoder decoder {filter, fixture.initial_sequence,
        fixture.receive_capacity, fixture.maximum_payload_size};
    const auto resources = decoder.resource_usage();
    statistics.maximum_decoder_bytes =
        std::max(statistics.maximum_decoder_bytes, resources.dynamic_bytes);

    DeterministicRandom payload_random {seed ^ 0x7061'796c'6f61'6401U};
    std::vector<OwnedSource> sources(fixture.source_count);
    std::vector<OwnedControl> controls;
    controls.reserve(fixture.expected_control_count);
    for (std::size_t index = 0U; index < sources.size(); ++index) {
        auto& source = sources[index];
        source.payload_size =
            1U + payload_random.below(fixture.maximum_payload_size);
        for (std::size_t byte = 0U; byte < source.payload_size; ++byte) {
            source.payload[byte] =
                static_cast<std::byte>(payload_random.next());
        }
        source.header = {
            .sequence = fixture.initial_sequence.advanced(
                static_cast<std::uint32_t>(index)),
            .message_number = static_cast<std::uint32_t>(index) + 1U,
            .boundary = MessageBoundary::solo,
            .in_order = false,
            .encryption_key = encryption_key(index),
            .retransmitted = false,
            .timestamp = PacketTimestamp {static_cast<std::uint32_t>(
                payload_random.next())},
            .destination_socket_id = 77U,
        };
        if (encoder.feed_source(source.view()) != Error::none) {
            return fail(fixture, seed, "encoder rejected source");
        }
        while (encoder.control_packet_ready()) {
            const auto control = encoder.control_packet();
            if (!control.has_value()) {
                return fail(fixture, seed, "ready encoder omitted control");
            }
            controls.push_back({
                .header = control->header,
                .payload = {control->payload.begin(), control->payload.end()},
            });
            encoder.consume_control_packet();
        }
    }
    if (controls.size() != fixture.expected_control_count) {
        return fail(fixture, seed, "unexpected control count");
    }

    std::vector<bool> withheld(fixture.source_count, false);
    for (const auto index : fixture.cycle) {
        withheld[index] = true;
    }
    std::vector<std::uint32_t> delivery_order;
    delivery_order.reserve(fixture.source_count - fixture.cycle.size());
    for (std::uint32_t index = 0U; index < fixture.source_count; ++index) {
        const std::uint64_t group = column_group(fixture, index);
        const bool first_column_series =
            group == invalid_column_group || group / fixture.columns == 0U;
        if (!withheld[index] && first_column_series) {
            delivery_order.push_back(index);
        }
    }
    DeterministicRandom order_random {seed ^ 0x6f72'6465'7200'0001U};
    shuffle(delivery_order, order_random);

    for (const auto index : delivery_order) {
        const auto accepted = decoder.receive(sources[index].view());
        if (!accepted || accepted.consume_control_packet
            || !accepted.reconstructed_packets.empty()
            || !accepted.irrecoverable_losses.empty()) {
            return fail(fixture, seed, "source phase changed chain state");
        }
    }
    for (const auto& control : controls) {
        const auto accepted = decoder.receive(control.view());
        if (!accepted || !accepted.consume_control_packet
            || !accepted.reconstructed_packets.empty()
            || !accepted.irrecoverable_losses.empty()) {
            return fail(fixture, seed, "control phase changed chain state");
        }
    }

    const auto rebuilt = decoder.receive(sources[fixture.cycle.front()].view());
    if (!rebuilt || rebuilt.consume_control_packet
        || !rebuilt.irrecoverable_losses.empty()
        || rebuilt.reconstructed_packets.size() != fixture.cycle.size() - 1U) {
        return fail(fixture, seed, "trigger did not complete exact chain");
    }
    for (std::size_t index = 0U; index < rebuilt.reconstructed_packets.size();
        ++index) {
        const std::uint32_t source_index = fixture.cycle[index + 1U];
        const auto& expected = sources[source_index];
        const auto& actual = rebuilt.reconstructed_packets[index];
        if (actual.kind != PacketKind::data
            || actual.data.sequence != expected.header.sequence
            || actual.data.message_number != expected.header.message_number
            || actual.data.boundary != expected.header.boundary
            || actual.data.in_order != expected.header.in_order
            || actual.data.encryption_key != expected.header.encryption_key
            || !actual.data.retransmitted
            || actual.data.timestamp != expected.header.timestamp
            || actual.data.destination_socket_id
                != expected.header.destination_socket_id
            || !std::equal(actual.payload.begin(), actual.payload.end(),
                expected.payload.begin(),
                expected.payload.begin()
                    + static_cast<std::ptrdiff_t>(expected.payload_size))) {
            return fail(fixture, seed, "reconstructed packet mismatch");
        }
        statistics.checksum ^=
            static_cast<std::uint64_t>(actual.data.sequence.value())
            << (index % 29U);
        for (const auto byte : actual.payload) {
            statistics.checksum = statistics.checksum * 1'099'511'628'211ULL
                ^ std::to_integer<std::uint8_t>(byte);
        }
    }

    for (const auto index : fixture.cycle) {
        const auto duplicate = decoder.receive(sources[index].view());
        if (!duplicate || !duplicate.reconstructed_packets.empty()
            || !duplicate.irrecoverable_losses.empty()) {
            return fail(fixture, seed, "late source was not a duplicate");
        }
    }
    for (const auto& control : controls) {
        const auto duplicate = decoder.receive(control.view());
        if (!duplicate || !duplicate.consume_control_packet
            || !duplicate.reconstructed_packets.empty()
            || !duplicate.irrecoverable_losses.empty()) {
            return fail(fixture, seed, "late control was not a duplicate");
        }
    }
    const std::array malformed {std::byte {0x7f}, std::byte {0xff}};
    const auto rejected = decoder.receive({
        .kind = PacketKind::data,
        .data =
            {
                .sequence = fixture.initial_sequence,
                .message_number = 0U,
                .boundary = MessageBoundary::solo,
                .destination_socket_id = 77U,
            },
        .payload = malformed,
    });
    if (rejected || !rejected.consume_control_packet
        || rejected.error != Error::invalid_control_payload
        || !(resources == decoder.resource_usage())) {
        return fail(fixture, seed, "post-chain boundary invariant failed");
    }

    ++statistics.scenarios;
    statistics.encoded_sources += fixture.source_count;
    statistics.delivered_sources += delivery_order.size() + 1U;
    statistics.controls += controls.size();
    statistics.reconstructed += fixture.cycle.size() - 1U;
    statistics.maximum_chain =
        std::max(statistics.maximum_chain, fixture.cycle.size() - 1U);
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    Options options;
    if (!parse_options(argc, argv, options)) {
        std::cerr << "usage: robotweax_srt_fec_chain_soak "
                     "[--iterations N] [--seed N]\n";
        return 2;
    }
    Statistics statistics;
    for (std::size_t iteration = 0U; iteration < options.iterations;
        ++iteration) {
        const std::uint64_t seed = options.seed + iteration;
        for (const auto& fixture :
            robotweax::srt::fec_chain_fixture::fixtures) {
            if (!run_fixture(fixture, seed, statistics)) {
                return 1;
            }
        }
    }
    std::cout << "FEC chain soak passed"
              << ": first_seed=" << options.seed
              << " iterations=" << options.iterations
              << " scenarios=" << statistics.scenarios
              << " encoded_sources=" << statistics.encoded_sources
              << " delivered_sources=" << statistics.delivered_sources
              << " controls=" << statistics.controls
              << " reconstructed=" << statistics.reconstructed
              << " maximum_chain=" << statistics.maximum_chain
              << " maximum_decoder_bytes=" << statistics.maximum_decoder_bytes
              << " checksum=" << statistics.checksum << '\n';
    return 0;
}
