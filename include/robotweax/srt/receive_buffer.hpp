#pragma once

#include "robotweax/srt/error.hpp"
#include "robotweax/srt/packet.hpp"
#include "robotweax/srt/reliability.hpp"
#include "robotweax/srt/send_buffer.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace robotweax::srt {

struct ReceiveInsertResult {
    Error error = Error::none;
    ReceiveStatus status = ReceiveStatus::accepted_in_order;
    SequenceRange gap_before_packet{};
    bool has_gap = false;
    std::uint32_t contiguous_advance = 0;

    [[nodiscard]] constexpr explicit operator bool() const noexcept
    {
        return error == Error::none;
    }
};

struct ReceivedMessageResult {
    Error error = Error::none;
    std::size_t bytes_written = 0;
    std::uint32_t message_number = 0;
    SequenceNumber first_sequence{};
    PacketTimestamp timestamp{};
    // Present only after a successful timed session pop. This is the exact
    // deadline used to authorize that pop, not a later clock observation.
    std::optional<std::uint64_t> delivery_time_microseconds {};

    [[nodiscard]] constexpr explicit operator bool() const noexcept
    {
        return error == Error::none;
    }
};

struct BufferedMessageInfo {
    SequenceNumber first_sequence{};
    SequenceNumber last_sequence{};
    PacketTimestamp timestamp{};
};

class ReceiveBuffer {
public:
    ReceiveBuffer(SequenceNumber initial_sequence, std::size_t capacity_packets);

    [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size(); }
    [[nodiscard]] std::size_t occupied() const noexcept { return occupied_; }
    [[nodiscard]] std::size_t available() const noexcept { return capacity() - occupied_; }
    [[nodiscard]] std::size_t buffered_payload_bytes() const noexcept;
    [[nodiscard]] std::uint64_t
    buffered_span_milliseconds() const noexcept;
    [[nodiscard]] SequenceNumber next_ack_sequence() const noexcept { return next_ack_sequence_; }
    [[nodiscard]] SequenceNumber first_stored_sequence() const noexcept
    {
        return first_stored_sequence_;
    }
    [[nodiscard]] bool has_complete_message() const noexcept;
    [[nodiscard]] std::optional<BufferedMessageInfo>
    first_complete_message() const noexcept;
    [[nodiscard]] bool has_stream_data() const noexcept
    {
        return find(first_stored_sequence_) != nullptr;
    }
    [[nodiscard]] std::optional<PacketTimestamp> next_message_timestamp() const noexcept;

    [[nodiscard]] ReceiveInsertResult insert(const PacketView& packet) noexcept;
    [[nodiscard]] ReceivedMessageResult pop_message(
        std::span<std::byte> destination) noexcept;
    [[nodiscard]] ReceivedMessageResult pop_stream(
        std::span<std::byte> destination) noexcept;
    [[nodiscard]] Error drop_range(SequenceRange range,
        std::uint32_t message_number = 0,
        std::size_t* newly_dropped_packets = nullptr) noexcept;
    [[nodiscard]] Error discard_before(
        SequenceNumber next_sequence) noexcept;

private:
    struct Slot {
        DataHeader header{};
        std::array<std::byte, maximum_data_payload_size> payload{};
        std::uint16_t payload_size = 0;
        std::uint16_t payload_offset = 0;
        bool occupied = false;
        bool dropped = false;
    };

    [[nodiscard]] Slot* find(SequenceNumber sequence) noexcept;
    [[nodiscard]] const Slot* find(SequenceNumber sequence) const noexcept;
    void refresh_first_buffered_timestamp() noexcept;
    void refresh_buffered_timestamp_bounds() noexcept;
    void advance_acknowledgement() noexcept;
    void trim_dropped_prefix() noexcept;

    std::vector<Slot> slots_;
    SequenceNumber first_stored_sequence_;
    SequenceNumber next_ack_sequence_;
    std::size_t head_ = 0;
    std::size_t occupied_ = 0;
    std::size_t buffered_payload_bytes_ = 0;
    SequenceNumber first_buffered_sequence_ {};
    SequenceNumber last_buffered_sequence_ {};
    PacketTimestamp first_buffered_timestamp_ {};
    PacketTimestamp last_buffered_timestamp_ {};
};

} // namespace robotweax::srt
