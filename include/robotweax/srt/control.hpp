#pragma once

#include "robotweax/srt/error.hpp"
#include "robotweax/srt/packet.hpp"
#include "robotweax/srt/reliability.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace robotweax::srt {

enum class AcknowledgementKind : std::uint8_t { lite, small, full };

struct Acknowledgement {
    AcknowledgementKind kind = AcknowledgementKind::full;
    std::uint32_t acknowledgement_number = 0;
    SequenceNumber next_sequence{};
    std::uint32_t round_trip_time_microseconds = 100'000;
    std::uint32_t round_trip_time_variance_microseconds = 50'000;
    std::uint32_t available_receive_buffer_packets = 0;
    std::uint32_t receive_rate_packets_per_second = 0;
    std::uint32_t estimated_link_capacity_packets_per_second = 0;
    std::uint32_t receive_rate_bytes_per_second = 0;
    bool has_rate_metrics = false;
};

struct AcknowledgementResult {
    Error error = Error::none;
    Acknowledgement acknowledgement{};

    [[nodiscard]] constexpr explicit operator bool() const noexcept
    {
        return error == Error::none;
    }
};

struct ControlEncodeResult {
    Error error = Error::none;
    std::size_t bytes_written = 0;

    [[nodiscard]] constexpr explicit operator bool() const noexcept
    {
        return error == Error::none;
    }
};

struct DropRequest {
    std::uint32_t message_number = 0;
    SequenceRange sequences{};
};

struct DropRequestResult {
    Error error = Error::none;
    DropRequest request{};

    [[nodiscard]] constexpr explicit operator bool() const noexcept
    {
        return error == Error::none;
    }
};

[[nodiscard]] DropRequestResult decode_drop_request(
    const PacketView& packet) noexcept;

[[nodiscard]] ControlEncodeResult encode_drop_request_payload(
    const DropRequest& request,
    std::span<std::byte> destination) noexcept;

[[nodiscard]] AcknowledgementResult decode_acknowledgement(
    const PacketView& packet) noexcept;

[[nodiscard]] ControlEncodeResult encode_acknowledgement_payload(
    const Acknowledgement& acknowledgement,
    std::span<std::byte> destination) noexcept;

[[nodiscard]] constexpr std::uint32_t decode_ackack_number(
    const PacketView& packet) noexcept
{
    return packet.control.type_specific;
}

struct LossRangeResult {
    Error error = Error::none;
    SequenceRange range{};
    std::size_t bytes_consumed = 0;

    [[nodiscard]] constexpr explicit operator bool() const noexcept
    {
        return error == Error::none;
    }
};

[[nodiscard]] LossRangeResult decode_loss_range(
    std::span<const std::byte> payload) noexcept;

[[nodiscard]] ControlEncodeResult encode_loss_ranges(
    std::span<const SequenceRange> ranges,
    std::span<std::byte> destination) noexcept;

} // namespace robotweax::srt
