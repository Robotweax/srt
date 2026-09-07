#include "robotweax/srt/control.hpp"

namespace robotweax::srt {
namespace {

[[nodiscard]] std::uint32_t read_u32(const std::byte* bytes) noexcept
{
    return (std::to_integer<std::uint32_t>(bytes[0]) << 24U)
        | (std::to_integer<std::uint32_t>(bytes[1]) << 16U)
        | (std::to_integer<std::uint32_t>(bytes[2]) << 8U)
        | std::to_integer<std::uint32_t>(bytes[3]);
}

void write_u32(std::byte* bytes, std::uint32_t value) noexcept
{
    bytes[0] = static_cast<std::byte>((value >> 24U) & 0xffU);
    bytes[1] = static_cast<std::byte>((value >> 16U) & 0xffU);
    bytes[2] = static_cast<std::byte>((value >> 8U) & 0xffU);
    bytes[3] = static_cast<std::byte>(value & 0xffU);
}

[[nodiscard]] constexpr bool valid_sequence_word(std::uint32_t value) noexcept
{
    return (value & ~SequenceNumber::mask) == 0U;
}

} // namespace

AcknowledgementResult decode_acknowledgement(const PacketView& packet) noexcept
{
    if (packet.kind != PacketKind::control
        || packet.control.type != ControlType::acknowledgement) {
        return {.error = Error::invalid_control_type};
    }
    const auto payload = packet.payload;
    if (payload.size() == 4U) {
        if (packet.control.type_specific != 0U) {
            return {.error = Error::invalid_control_payload};
        }
        const std::uint32_t next_sequence = read_u32(payload.data());
        if (!valid_sequence_word(next_sequence)) {
            return {.error = Error::invalid_control_payload};
        }
        return {.acknowledgement = {
                    .kind = AcknowledgementKind::lite,
                    .acknowledgement_number = packet.control.type_specific,
                    .next_sequence = SequenceNumber {next_sequence},
                }};
    }
    if ((payload.size() != 16U && payload.size() != 24U
            && payload.size() != 28U && payload.size() != 32U)
        || (payload.size() % 4U) != 0U) {
        return {.error = Error::invalid_control_payload};
    }

    Acknowledgement acknowledgement;
    const std::uint32_t next_sequence = read_u32(payload.data());
    if (!valid_sequence_word(next_sequence)) {
        return {.error = Error::invalid_control_payload};
    }
    acknowledgement.kind = payload.size() == 16U ? AcknowledgementKind::small
                                                 : AcknowledgementKind::full;
    if (acknowledgement.kind == AcknowledgementKind::full
        && packet.control.type_specific == 0U) {
        return {.error = Error::invalid_control_payload};
    }
    acknowledgement.acknowledgement_number = packet.control.type_specific;
    acknowledgement.next_sequence = SequenceNumber {next_sequence};
    acknowledgement.round_trip_time_microseconds = read_u32(payload.data() + 4);
    acknowledgement.round_trip_time_variance_microseconds = read_u32(payload.data() + 8);
    acknowledgement.available_receive_buffer_packets = read_u32(payload.data() + 12);
    if (payload.size() >= 24U) {
        acknowledgement.receive_rate_packets_per_second = read_u32(payload.data() + 16);
        acknowledgement.estimated_link_capacity_packets_per_second = read_u32(payload.data() + 20);
        acknowledgement.has_rate_metrics = true;
    }
    if (payload.size() >= 28U) {
        acknowledgement.receive_rate_bytes_per_second = read_u32(payload.data() + 24);
    }
    return {.acknowledgement = acknowledgement};
}

ControlEncodeResult encode_acknowledgement_payload(
    const Acknowledgement& acknowledgement,
    std::span<std::byte> destination) noexcept
{
    std::size_t required = 28U;
    if (acknowledgement.kind == AcknowledgementKind::lite) {
        required = 4U;
    } else if (acknowledgement.kind == AcknowledgementKind::small) {
        required = 16U;
    }
    if (destination.size() < required) {
        return {.error = Error::buffer_too_small};
    }
    write_u32(destination.data(), acknowledgement.next_sequence.value());
    if (acknowledgement.kind == AcknowledgementKind::lite) {
        return {.bytes_written = required};
    }
    write_u32(destination.data() + 4, acknowledgement.round_trip_time_microseconds);
    write_u32(destination.data() + 8, acknowledgement.round_trip_time_variance_microseconds);
    write_u32(destination.data() + 12,
        acknowledgement.available_receive_buffer_packets);
    if (acknowledgement.kind == AcknowledgementKind::full) {
        write_u32(destination.data() + 16,
            acknowledgement.receive_rate_packets_per_second);
        write_u32(destination.data() + 20,
            acknowledgement.estimated_link_capacity_packets_per_second);
        write_u32(destination.data() + 24,
            acknowledgement.receive_rate_bytes_per_second);
    }
    return {.bytes_written = required};
}

DropRequestResult decode_drop_request(const PacketView& packet) noexcept
{
    if (packet.kind != PacketKind::control
        || packet.control.type != ControlType::drop_request) {
        return {.error = Error::invalid_control_type};
    }
    if (packet.payload.size() != 8U) {
        return {.error = Error::invalid_control_payload};
    }
    const std::uint32_t first_word = read_u32(packet.payload.data());
    const std::uint32_t last_word = read_u32(packet.payload.data() + 4);
    if (!valid_sequence_word(first_word) || !valid_sequence_word(last_word)) {
        return {.error = Error::invalid_control_payload};
    }
    const SequenceNumber first {first_word};
    const SequenceNumber last {last_word};
    if (last.distance_from(first) < 0) {
        return {.error = Error::invalid_control_payload};
    }
    return {.request = {
        .message_number = packet.control.type_specific & 0x03ff'ffffU,
        .sequences = {.first = first, .last = last},
    }};
}

ControlEncodeResult encode_drop_request_payload(
    const DropRequest& request,
    std::span<std::byte> destination) noexcept
{
    if (request.sequences.last.distance_from(request.sequences.first) < 0) {
        return {.error = Error::invalid_control_payload};
    }
    if (destination.size() < 8U) {
        return {.error = Error::buffer_too_small};
    }
    write_u32(destination.data(), request.sequences.first.value());
    write_u32(destination.data() + 4, request.sequences.last.value());
    return {.bytes_written = 8U};
}

LossRangeResult decode_loss_range(std::span<const std::byte> payload) noexcept
{
    if (payload.size() < 4U) {
        return {.error = Error::invalid_control_payload};
    }
    const std::uint32_t first_word = read_u32(payload.data());
    const bool is_range = (first_word & 0x8000'0000U) != 0U;
    const SequenceNumber first{first_word};
    if (!is_range) {
        return {
            .range = {.first = first, .last = first},
            .bytes_consumed = 4U,
        };
    }
    if (payload.size() < 8U) {
        return {.error = Error::invalid_control_payload};
    }
    const std::uint32_t last_word = read_u32(payload.data() + 4);
    if ((last_word & 0x8000'0000U) != 0U) {
        return {.error = Error::invalid_control_payload};
    }
    const SequenceNumber last{last_word};
    if (last.distance_from(first) < 0) {
        return {.error = Error::invalid_control_payload};
    }
    return {
        .range = {.first = first, .last = last},
        .bytes_consumed = 8U,
    };
}

ControlEncodeResult encode_loss_ranges(
    std::span<const SequenceRange> ranges,
    std::span<std::byte> destination) noexcept
{
    std::size_t written = 0;
    for (const auto& range : ranges) {
        if (range.last.distance_from(range.first) < 0) {
            return {.error = Error::invalid_control_payload};
        }
        const bool single = range.first == range.last;
        const std::size_t required = single ? 4U : 8U;
        if (destination.size() - written < required) {
            return {.error = Error::buffer_too_small};
        }
        write_u32(destination.data() + written,
            range.first.value() | (single ? 0U : 0x8000'0000U));
        written += 4U;
        if (!single) {
            write_u32(destination.data() + written, range.last.value());
            written += 4U;
        }
    }
    return {.bytes_written = written};
}

} // namespace robotweax::srt
