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

namespace detail {
struct ReceiveBufferTestAccess;
}

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

struct BufferedMessageCopy {
    SequenceNumber first_sequence {};
    SequenceNumber next_sequence {};
    PacketTimestamp timestamp {};
    std::uint32_t message_number = 0;
    std::vector<std::byte> payload {};
};

struct BufferedMessageCopies {
    Error error = Error::none;
    std::vector<BufferedMessageCopy> messages {};
};

// Calls, including cached const delivery queries, require external synchronization.
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
    // Sequence space available beyond the furthest received or resolved slot.
    // This is stricter than payload storage availability while a leading gap
    // keeps the receive window anchored.
    [[nodiscard]] std::size_t window_available() const noexcept;
    [[nodiscard]] bool contains_data(SequenceNumber sequence) const noexcept
    {
        return find(sequence) != nullptr;
    }
    // Received or locally discarded sequences need no FEC retransmission,
    // even when their payload is unavailable to application reads.
    [[nodiscard]] bool is_settled(SequenceNumber sequence) const noexcept;
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
    // A bounded, non-consuming copy of complete messages, including those
    // behind a gap. Keep the earliest messages that fit the aggregate budget;
    // skip a message that alone exceeds it. Incomplete or rejected payload is
    // never published. Allocation failure reports an error without partial data.
    [[nodiscard]] BufferedMessageCopies copy_complete_messages(
        std::size_t maximum_packets, std::size_t maximum_bytes) const noexcept;
    [[nodiscard]] ReceivedMessageResult pop_message(
        std::span<std::byte> destination) noexcept;
    [[nodiscard]] ReceivedMessageResult pop_message_unordered(
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
    // In stream mode every received packet is deliverable independently of
    // its message boundary, including an unread suffix of the head packet.
    [[nodiscard]] Error drop_peer_requested_stream_range(SequenceRange range,
        std::uint32_t message_number = 0,
        std::size_t* newly_dropped_packets = nullptr) noexcept;
    // The sender no longer retransmits a DROPREQ range. Advance cumulative
    // feedback without closing the receive slots to late original packets.
    [[nodiscard]] bool acknowledge_peer_drop_range(
        SequenceRange range) noexcept;
    [[nodiscard]] Error discard_before(
        SequenceNumber next_sequence) noexcept;
    void mark_gap(
        SequenceRange range, std::uint64_t deadline_microseconds) noexcept;
    [[nodiscard]] std::optional<std::uint64_t>
    next_gap_deadline() const noexcept;
    [[nodiscard]] std::optional<SequenceNumber> next_expired_gap(
        std::uint64_t now_microseconds) const noexcept;

private:
    friend struct detail::ReceiveBufferTestAccess;
    friend class ReliabilitySession;
    bool defer_drop_timestamp_refresh_ = false;
    bool drop_timestamp_refresh_pending_ = false;
    [[nodiscard]] std::optional<BufferedMessageInfo>
    query_first_complete_message(std::size_t* inspected) const noexcept;
    [[nodiscard]] std::optional<BufferedMessageInfo>
    query_first_buffered_packet(std::size_t* inspected) const noexcept;

    void index_occupied(std::size_t position, bool occupied) noexcept;
    [[nodiscard]] std::optional<std::size_t> next_occupied_bit(
        std::size_t level, std::size_t begin, std::size_t end,
        std::size_t* word_reads) const noexcept;
    [[nodiscard]] std::optional<std::size_t> next_occupied_offset(
        std::size_t begin, std::size_t* word_reads = nullptr) const noexcept;

    void invalidate_delivery_queries() noexcept
    {
        message_query_valid_ = false;
        packet_query_valid_ = false;
    }

    enum class DropPreservation {
        none,
        complete_messages,
        received_packets,
    };

    struct Slot {
        DataHeader header{};
        std::uint32_t payload_index = 0;
        std::uint16_t payload_size = 0;
        std::uint16_t payload_offset = 0;
        bool rejected_payload = false;
        bool occupied = false;
        bool dropped = false;
        std::uint64_t gap_deadline_microseconds = 0;
    };

    [[nodiscard]] Slot* find(SequenceNumber sequence) noexcept;
    [[nodiscard]] const Slot* find(SequenceNumber sequence) const noexcept;
    void refresh_first_buffered_timestamp(
        std::size_t* inspected = nullptr) noexcept;
    void refresh_buffered_timestamp_bounds(
        std::size_t* inspected = nullptr) noexcept;
    void advance_acknowledgement() noexcept;
    void trim_dropped_prefix() noexcept;
    [[nodiscard]] std::optional<std::size_t> complete_message_last_offset(
        std::size_t offset) const noexcept;
    [[nodiscard]] Error drop_range_impl(SequenceRange range,
        std::uint32_t message_number, std::size_t* newly_dropped_packets,
        DropPreservation preservation) noexcept;

    // Results depend only on retained DATA and the receive frontier, not on
    // ACKs, duplicate input, gap deadlines, or the current playout clock.
    mutable bool message_query_valid_ = false;
    mutable bool packet_query_valid_ = false;
    mutable std::optional<BufferedMessageInfo> message_query_;
    mutable std::optional<BufferedMessageInfo> packet_query_;
    bool has_rejected_payload_ = false;
    std::optional<std::uint32_t> discarding_message_;
    detail::PayloadPool payloads_;
    std::vector<Slot> slots_;
    // One leaf bit per readable DATA slot; each next level marks nonempty
    // words below it. Physical positions keep frontier movement allocation-free.
    // The sequence-space capacity limit needs at most five 64-way levels.
    std::vector<std::uint64_t> occupied_index_;
    static_assert(SequenceNumber::half_range <= (std::uint64_t {1} << 30U));
    std::array<std::size_t, 5> occupied_level_offsets_ {};
    std::size_t occupied_levels_ = 0;
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
