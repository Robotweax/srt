#include "robotweax/srt/fec.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

using namespace robotweax::srt;

namespace {

template <class Decoder>
void exercise_decoder(Decoder& decoder, SequenceNumber initial,
    std::span<const std::byte> control_payload, std::uint8_t sequence_offset,
    std::span<const std::byte> source_payload)
{
    const auto before = decoder.resource_usage();
    const PacketView source {
        .kind = PacketKind::data,
        .data =
            {
                .sequence = initial,
                .message_number = 1,
                .boundary = MessageBoundary::solo,
                .destination_socket_id = 7,
            },
        .payload = source_payload,
    };
    (void)decoder.receive(source);
    const PacketView control {
        .kind = PacketKind::data,
        .data =
            {
                .sequence = initial.advanced(sequence_offset),
                .message_number = 0,
                .boundary = MessageBoundary::solo,
                .destination_socket_id = 7,
            },
        .payload = control_payload,
    };
    (void)decoder.receive(control);
    (void)decoder.receive(control);
    if (!(before == decoder.resource_usage())) {
        __builtin_trap();
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(
    const std::uint8_t* data, std::size_t size)
{
    const auto bytes = std::as_bytes(std::span {data, size});
    (void)decode_fec_control_payload(bytes);
    if (size < 5U) {
        return 0;
    }

    const auto columns = static_cast<std::uint32_t>(2U + data[0] % 15U);
    const auto rows = static_cast<std::int32_t>(2U + data[1] % 7U);
    const std::size_t maximum_payload_size = 1U + data[2] % 64U;
    const auto mode = data[3] % 3U;
    PacketFilterConfiguration configuration {
        .enabled = true,
        .columns = columns,
        .rows = mode == 0U ? 1 : (mode == 1U ? -rows : rows),
        .layout = (data[4] & 1U) == 0U ? PacketFilterLayout::even
                                       : PacketFilterLayout::staircase,
        .arq = PacketFilterArqLevel::on_request,
        .columns_specified = true,
        .rows_specified = true,
        .layout_specified = true,
        .arq_specified = true,
    };
    const std::size_t matrix_size =
        static_cast<std::size_t>(columns) * static_cast<std::size_t>(rows);
    const std::size_t receive_capacity =
        mode == 0U ? 128U : (matrix_size < 128U ? 128U : matrix_size);
    std::array<std::byte, 64> source {};
    const auto source_payload = std::span {source}.first(maximum_payload_size);
    constexpr SequenceNumber initial {1'000U};
    const auto control_payload = bytes.subspan(5U);
    const std::uint8_t sequence_offset = data[4];

    if (mode == 0U) {
        RowFecDecoder decoder {
            configuration, initial, receive_capacity, maximum_payload_size};
        exercise_decoder(
            decoder, initial, control_payload, sequence_offset, source_payload);
    } else if (mode == 1U) {
        ColumnFecDecoder decoder {
            configuration, initial, receive_capacity, maximum_payload_size};
        exercise_decoder(
            decoder, initial, control_payload, sequence_offset, source_payload);
    } else {
        MatrixFecDecoder decoder {
            configuration, initial, receive_capacity, maximum_payload_size};
        exercise_decoder(
            decoder, initial, control_payload, sequence_offset, source_payload);
    }
    return 0;
}
