#pragma once

#include "robotweax/srt/error.hpp"
#include "robotweax/srt/packet.hpp"
#include "robotweax/srt/reliability.hpp"
#include "robotweax/srt/send_buffer.hpp"

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
    ReceiveBuffer(const ReceiveBuffer&) = default;
    ReceiveBuffer(ReceiveBuffer&&) noexcept = default;
    ReceiveBuffer& operator=(ReceiveBuffer&&) noexcept = default;
    ReceiveBuffer& operator=(const ReceiveBuffer& other);

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
    // The first buffered packet regardless of message boundaries: the unit
    // the stream API delivers and drops by, as libsrt does without the
    // message API.
    [[nodiscard]] std::optional<BufferedMessageInfo>
    first_buffered_packet() const noexcept;
    [[nodiscard]] std::optional<PacketTimestamp> next_message_timestamp() const noexcept;

    [[nodiscard]] ReceiveInsertResult insert(const PacketView& packet) noexcept;
    [[nodiscard]] ReceivedMessageResult pop_message(
        std::span<std::byte> destination) noexcept;
    // Reads bytes from the head packets in order. With `due` set, stops in
    // front of the first packet for which due(timestamp, context) is false,
    // so a TSBPD stream never hands out bytes ahead of their delivery time.
    using PacketDuePredicate = bool (*)(PacketTimestamp, void*);
    [[nodiscard]] ReceivedMessageResult pop_stream(
        std::span<std::byte> destination, PacketDuePredicate due = nullptr,
        void* due_context = nullptr) noexcept;
    [[nodiscard]] Error drop_range(SequenceRange range,
        std::uint32_t message_number = 0,
        std::size_t* newly_dropped_packets = nullptr) noexcept;
    // Reject an entire message, including fragments arriving later.
    void discard_message_payload(SequenceNumber sequence) noexcept;
    // A peer DROPREQ must not erase a complete message that arrived before
    // the control packet. Missing packets and incomplete messages are dropped.
    [[nodiscard]] Error drop_peer_requested_range(SequenceRange range,
        std::uint32_t message_number = 0,
        std::size_t* newly_dropped_packets = nullptr) noexcept;
    // The sender no longer retransmits a DROPREQ range. Advance cumulative
    // feedback without closing the receive slots to late original packets.
    [[nodiscard]] bool acknowledge_peer_drop_range(
        SequenceRange range) noexcept;
    [[nodiscard]] Error discard_before(
        SequenceNumber next_sequence) noexcept;

private:
    struct Slot {
        DataHeader header{};
        std::uint32_t payload_index = 0;
        std::uint16_t payload_size = 0;
        std::uint16_t payload_offset = 0;
        bool rejected_payload = false;
        bool occupied = false;
        bool dropped = false;
    };

    [[nodiscard]] Slot* find(SequenceNumber sequence) noexcept;
    [[nodiscard]] const Slot* find(SequenceNumber sequence) const noexcept;
    void refresh_first_buffered_timestamp() noexcept;
    void refresh_buffered_timestamp_bounds() noexcept;
    void advance_acknowledgement() noexcept;
    void trim_dropped_prefix() noexcept;
    [[nodiscard]] std::optional<std::size_t> complete_message_last_offset(
        std::size_t offset) const noexcept;
    [[nodiscard]] Error drop_range_impl(SequenceRange range,
        std::uint32_t message_number, std::size_t* newly_dropped_packets,
        bool preserve_existing_complete) noexcept;

    bool has_rejected_payload_ = false;
    std::optional<std::uint32_t> discarding_message_;
    detail::PayloadPool payloads_;
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
