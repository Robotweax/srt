#include "robotweax/srt/fec.hpp"
#include "robotweax/srt/packet_filter.hpp"

#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <string_view>

using namespace robotweax::srt;

namespace {

constexpr std::size_t payload_size = 1'312U;
constexpr std::size_t receive_capacity = 8'192U;

PacketFilterConfiguration configuration(
    std::uint32_t columns, std::int32_t rows, PacketFilterLayout layout)
{
    return {
        .enabled = true,
        .columns = columns,
        .rows = rows,
        .layout = layout,
        .arq = PacketFilterArqLevel::on_request,
        .columns_specified = true,
        .rows_specified = true,
        .layout_specified = true,
        .arq_specified = true,
    };
}

template <class Encoder, class Decoder>
bool run_profile(std::string_view name, const PacketFilterConfiguration& filter,
    std::size_t packet_count)
{
    constexpr SequenceNumber initial {100'000U};
    Encoder encoder {filter, initial, payload_size};
    Decoder decoder {filter, initial, receive_capacity, payload_size};
    const auto resources = decoder.resource_usage();
    std::array<std::byte, payload_size> payload {};
    std::uint64_t checksum = 0;
    std::size_t control_count = 0;

    const auto started = std::chrono::steady_clock::now();
    for (std::size_t index = 0; index < packet_count; ++index) {
        payload[0] = static_cast<std::byte>(index & 0xffU);
        const PacketView source {
            .kind = PacketKind::data,
            .data =
                {
                    .sequence = initial.advanced(static_cast<std::uint32_t>(
                        index & SequenceNumber::mask)),
                    .message_number =
                        static_cast<std::uint32_t>(index & SequenceNumber::mask)
                        + 1U,
                    .boundary = MessageBoundary::solo,
                    .timestamp = PacketTimestamp {static_cast<std::uint32_t>(
                        index * 1'000U)},
                    .destination_socket_id = 7,
                },
            .payload = payload,
        };
        if (encoder.feed_source(source) != Error::none
            || !decoder.receive(source)) {
            return false;
        }
        while (encoder.control_packet_ready()) {
            const auto control = encoder.control_packet();
            if (!control.has_value()) {
                return false;
            }
            const auto result = decoder.receive({
                .kind = PacketKind::data,
                .data = control->header,
                .payload = control->payload,
            });
            if (!result) {
                return false;
            }
            checksum += control->header.timestamp.value();
            ++control_count;
            encoder.consume_control_packet();
        }
    }
    const double seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started)
                               .count();
    if (!(resources == decoder.resource_usage())) {
        return false;
    }
    checksum += resources.dynamic_bytes;
    std::cout << "profile=" << name << " source_packets=" << packet_count
              << " control_packets=" << control_count
              << " payload_bytes=" << payload_size
              << " receive_capacity=" << receive_capacity
              << " decoder_dynamic_bytes=" << resources.dynamic_bytes
              << " source_packets_per_second="
              << static_cast<double>(packet_count) / seconds
              << " checksum=" << checksum << '\n';
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    std::size_t packet_count = 500'000U;
    if (argc == 3 && std::string_view {argv[1]} == "--packets") {
        const std::string_view text {argv[2]};
        const auto parsed = std::from_chars(
            text.data(), text.data() + text.size(), packet_count);
        if (parsed.ec != std::errc {} || parsed.ptr != text.data() + text.size()
            || packet_count == 0U) {
            std::cerr << "invalid --packets value\n";
            return 2;
        }
    } else if (argc != 1) {
        std::cerr << "usage: " << argv[0] << " [--packets COUNT]\n";
        return 2;
    }

    const auto row = configuration(7U, 1, PacketFilterLayout::staircase);
    const auto column = configuration(10U, -5, PacketFilterLayout::even);
    const auto matrix_even = configuration(10U, 5, PacketFilterLayout::even);
    const auto matrix_staircase =
        configuration(10U, 5, PacketFilterLayout::staircase);
    if (!run_profile<RowFecEncoder, RowFecDecoder>("row-7", row, packet_count)
        || !run_profile<ColumnFecEncoder, ColumnFecDecoder>(
            "column-10x5-even", column, packet_count)
        || !run_profile<MatrixFecEncoder, MatrixFecDecoder>(
            "matrix-10x5-even", matrix_even, packet_count)
        || !run_profile<MatrixFecEncoder, MatrixFecDecoder>(
            "matrix-10x5-staircase", matrix_staircase, packet_count)) {
        std::cerr << "FEC benchmark invariant failed\n";
        return 1;
    }
    return 0;
}
