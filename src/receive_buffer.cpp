#include "robotweax/srt/receive_buffer.hpp"

#include <algorithm>
#include <stdexcept>

namespace robotweax::srt {

ReceiveBuffer::ReceiveBuffer(SequenceNumber initial_sequence, std::size_t capacity_packets)
    : slots_(capacity_packets)
    , first_stored_sequence_(initial_sequence)
    , next_ack_sequence_(initial_sequence)
{
    if (capacity_packets == 0U || capacity_packets >= SequenceNumber::half_range) {
        throw std::invalid_argument("invalid receive-buffer capacity");
    }
}

ReceiveBuffer::Slot* ReceiveBuffer::find(SequenceNumber sequence) noexcept
{
    const std::int32_t signed_offset = sequence.distance_from(first_stored_sequence_);
    if (signed_offset < 0 || static_cast<std::size_t>(signed_offset) >= capacity()) {
        return nullptr;
    }
    auto& slot = slots_[(head_ + static_cast<std::size_t>(signed_offset)) % capacity()];
    return slot.occupied && slot.header.sequence == sequence ? &slot : nullptr;
}

const ReceiveBuffer::Slot* ReceiveBuffer::find(SequenceNumber sequence) const noexcept
{
    const std::int32_t signed_offset = sequence.distance_from(first_stored_sequence_);
    if (signed_offset < 0 || static_cast<std::size_t>(signed_offset) >= capacity()) {
        return nullptr;
    }
    const auto& slot = slots_[(head_ + static_cast<std::size_t>(signed_offset)) % capacity()];
    return slot.occupied && slot.header.sequence == sequence ? &slot : nullptr;
}

void ReceiveBuffer::refresh_first_buffered_timestamp() noexcept
{
    if (occupied_ == 0U) {
        first_buffered_sequence_ = {};
        last_buffered_sequence_ = {};
        first_buffered_timestamp_ = {};
        last_buffered_timestamp_ = {};
        return;
    }
    for (std::size_t offset = 0; offset < capacity(); ++offset) {
        const auto& slot = slots_[(head_ + offset) % capacity()];
        if (slot.occupied) {
            first_buffered_sequence_ = slot.header.sequence;
            first_buffered_timestamp_ = slot.header.timestamp;
            return;
        }
    }
}

void ReceiveBuffer::refresh_buffered_timestamp_bounds() noexcept
{
    refresh_first_buffered_timestamp();
    if (occupied_ == 0U) {
        return;
    }
    for (std::size_t offset = capacity(); offset > 0U; --offset) {
        const auto& slot = slots_[(head_ + offset - 1U) % capacity()];
        if (slot.occupied) {
            last_buffered_sequence_ = slot.header.sequence;
            last_buffered_timestamp_ = slot.header.timestamp;
            return;
        }
    }
}

void ReceiveBuffer::advance_acknowledgement() noexcept
{
    for (;;) {
        const std::int32_t signed_offset =
            next_ack_sequence_.distance_from(first_stored_sequence_);
        if (signed_offset < 0
            || static_cast<std::size_t>(signed_offset)
                >= capacity()) {
            return;
        }
        const auto& slot =
            slots_[(head_ + static_cast<std::size_t>(signed_offset))
                % capacity()];
        if (!slot.dropped
            && !(slot.occupied
                && slot.header.sequence == next_ack_sequence_)) {
            return;
        }
        next_ack_sequence_ = next_ack_sequence_.next();
    }
}

void ReceiveBuffer::trim_dropped_prefix() noexcept
{
    while (slots_[head_].dropped) {
        auto& slot = slots_[head_];
        slot.dropped = false;
        slot.payload_size = 0;
        slot.payload_offset = 0;
        head_ = (head_ + 1U) % capacity();
        first_stored_sequence_ = first_stored_sequence_.next();
    }
}

ReceiveInsertResult ReceiveBuffer::insert(const PacketView& packet) noexcept
{
    if (packet.kind != PacketKind::data
        || packet.payload.size() > maximum_data_payload_size) {
        return {.error = Error::invalid_packet_type};
    }
    const auto sequence = packet.data.sequence;
    const std::int32_t signed_offset = sequence.distance_from(first_stored_sequence_);
    if (signed_offset < 0) {
        return {.status = ReceiveStatus::older_than_window};
    }
    const auto offset = static_cast<std::size_t>(signed_offset);
    if (offset >= capacity()) {
        return {.status = ReceiveStatus::beyond_window};
    }
    auto& slot = slots_[(head_ + offset) % capacity()];
    if (slot.occupied || slot.dropped) {
        return {.status = ReceiveStatus::duplicate};
    }

    slot.header = packet.data;
    std::copy(packet.payload.begin(), packet.payload.end(), slot.payload.begin());
    slot.payload_size = static_cast<std::uint16_t>(packet.payload.size());
    slot.payload_offset = 0;
    slot.occupied = true;
    slot.dropped = false;
    if (occupied_ == 0U) {
        first_buffered_sequence_ = sequence;
        last_buffered_sequence_ = sequence;
        first_buffered_timestamp_ = packet.data.timestamp;
        last_buffered_timestamp_ = packet.data.timestamp;
    } else {
        if (sequence.distance_from(first_buffered_sequence_) < 0) {
            first_buffered_sequence_ = sequence;
            first_buffered_timestamp_ = packet.data.timestamp;
        }
        if (sequence.distance_from(last_buffered_sequence_) > 0) {
            last_buffered_sequence_ = sequence;
            last_buffered_timestamp_ = packet.data.timestamp;
        }
    }
    ++occupied_;
    buffered_payload_bytes_ += packet.payload.size();

    ReceiveInsertResult result;
    const std::int32_t ack_offset = sequence.distance_from(next_ack_sequence_);
    result.status = ack_offset == 0
        ? ReceiveStatus::accepted_in_order
        : ReceiveStatus::accepted_out_of_order;
    if (ack_offset > 0) {
        result.has_gap = true;
        result.gap_before_packet = {
            .first = next_ack_sequence_,
            .last = sequence.advanced(SequenceNumber::mask),
        };
    }
    const auto previous_ack = next_ack_sequence_;
    advance_acknowledgement();
    const std::int32_t ack_advance =
        next_ack_sequence_.distance_from(previous_ack);
    result.contiguous_advance = ack_advance > 0
        ? static_cast<std::uint32_t>(ack_advance) : 0U;
    return result;
}

std::optional<PacketTimestamp> ReceiveBuffer::next_message_timestamp() const noexcept
{
    const auto* slot = find(first_stored_sequence_);
    if (slot == nullptr) {
        return std::nullopt;
    }
    return slot->header.timestamp;
}

ReceivedMessageResult ReceiveBuffer::pop_message(
    std::span<std::byte> destination) noexcept
{
    const auto* first = find(first_stored_sequence_);
    if (first == nullptr) {
        return {.error = Error::would_block};
    }
    const auto boundary = first->header.boundary;
    if (boundary != MessageBoundary::solo && boundary != MessageBoundary::first) {
        return {.error = Error::invalid_state};
    }
    if (first->payload_offset != 0U) {
        return {.error = Error::invalid_state};
    }

    const std::uint32_t message_number = first->header.message_number;
    const SequenceNumber message_first_sequence = first_stored_sequence_;
    const PacketTimestamp timestamp = first->header.timestamp;
    std::size_t packet_count = 0;
    std::size_t total_size = 0;
    SequenceNumber sequence = first_stored_sequence_;
    for (;;) {
        const auto* slot = find(sequence);
        if (slot == nullptr) {
            return {.error = Error::would_block};
        }
        if (slot->header.message_number != message_number
            || slot->payload_offset != 0U
            || (packet_count > 0U
                && slot->header.boundary != MessageBoundary::subsequent
                && slot->header.boundary != MessageBoundary::last)) {
            return {.error = Error::invalid_state};
        }
        total_size += slot->payload_size;
        ++packet_count;
        const bool complete = slot->header.boundary == MessageBoundary::solo
            || slot->header.boundary == MessageBoundary::last;
        if (complete) {
            break;
        }
        sequence = sequence.next();
        if (packet_count >= capacity()) {
            return {.error = Error::invalid_state};
        }
    }
    if (destination.size() < total_size) {
        return {.error = Error::buffer_too_small};
    }

    std::size_t written = 0;
    for (std::size_t index = 0; index < packet_count; ++index) {
        auto& slot = slots_[(head_ + index) % capacity()];
        std::copy_n(slot.payload.begin(), slot.payload_size,
            destination.begin() + static_cast<std::ptrdiff_t>(written));
        written += slot.payload_size;
        buffered_payload_bytes_ -= slot.payload_size;
        slot.occupied = false;
        slot.dropped = false;
        slot.payload_size = 0;
        slot.payload_offset = 0;
    }
    head_ = (head_ + packet_count) % capacity();
    occupied_ -= packet_count;
    first_stored_sequence_ = first_stored_sequence_.advanced(
        static_cast<std::uint32_t>(packet_count));
    trim_dropped_prefix();
    refresh_first_buffered_timestamp();
    return {
        .bytes_written = written,
        .message_number = message_number,
        .first_sequence = message_first_sequence,
        .timestamp = timestamp,
    };
}

ReceivedMessageResult ReceiveBuffer::pop_stream(
    std::span<std::byte> destination) noexcept
{
    if (destination.empty()) {
        return {.error = Error::invalid_state};
    }
    const auto* first = find(first_stored_sequence_);
    if (first == nullptr) {
        return {.error = Error::would_block};
    }

    const SequenceNumber first_sequence = first_stored_sequence_;
    const std::uint32_t message_number =
        first->header.message_number;
    const PacketTimestamp timestamp = first->header.timestamp;
    std::size_t written = 0;
    bool removed_packet = false;
    while (written < destination.size()) {
        auto* slot = find(first_stored_sequence_);
        if (slot == nullptr) {
            break;
        }
        const std::size_t remaining =
            static_cast<std::size_t>(slot->payload_size)
            - static_cast<std::size_t>(slot->payload_offset);
        const std::size_t copied = std::min(
            remaining, destination.size() - written);
        std::copy_n(
            slot->payload.begin()
                + static_cast<std::ptrdiff_t>(slot->payload_offset),
            copied,
            destination.begin()
                + static_cast<std::ptrdiff_t>(written));
        written += copied;
        buffered_payload_bytes_ -= copied;
        slot->payload_offset = static_cast<std::uint16_t>(
            static_cast<std::size_t>(slot->payload_offset) + copied);

        if (slot->payload_offset != slot->payload_size) {
            break;
        }
        slot->occupied = false;
        slot->dropped = false;
        slot->payload_size = 0;
        slot->payload_offset = 0;
        head_ = (head_ + 1U) % capacity();
        --occupied_;
        removed_packet = true;
        first_stored_sequence_ =
            first_stored_sequence_.next();
        trim_dropped_prefix();
    }
    if (removed_packet) {
        refresh_first_buffered_timestamp();
    }
    return {
        .bytes_written = written,
        .message_number = message_number,
        .first_sequence = first_sequence,
        .timestamp = timestamp,
    };
}

bool ReceiveBuffer::has_complete_message() const noexcept
{
    const auto message = first_complete_message();
    return message.has_value()
        && message->first_sequence == first_stored_sequence_;
}

std::optional<BufferedMessageInfo>
ReceiveBuffer::first_complete_message() const noexcept
{
    // Runtime readiness also queries idle receive sides of active senders.
    // No occupied packet means no complete message, regardless of capacity.
    if (occupied_ == 0U) {
        return std::nullopt;
    }
    SequenceNumber candidate = first_stored_sequence_;
    for (std::size_t offset = 0; offset < capacity(); ++offset) {
        const auto* first = find(candidate);
        if (first == nullptr
            || (first->header.boundary != MessageBoundary::solo
                && first->header.boundary != MessageBoundary::first)) {
            candidate = candidate.next();
            continue;
        }

        if (first->header.boundary == MessageBoundary::solo) {
            return BufferedMessageInfo{
                .first_sequence = candidate,
                .last_sequence = candidate,
                .timestamp = first->header.timestamp,
            };
        }

        const std::uint32_t message_number =
            first->header.message_number;
        SequenceNumber sequence = candidate.next();
        const std::size_t remaining = capacity() - offset - 1U;
        for (std::size_t fragment = 0;
             fragment < remaining;
             ++fragment) {
            const auto* slot = find(sequence);
            if (slot == nullptr
                || slot->header.message_number != message_number
                || (slot->header.boundary
                        != MessageBoundary::subsequent
                    && slot->header.boundary
                        != MessageBoundary::last)) {
                break;
            }
            if (slot->header.boundary == MessageBoundary::last) {
                return BufferedMessageInfo{
                    .first_sequence = candidate,
                    .last_sequence = sequence,
                    .timestamp = first->header.timestamp,
                };
            }
            sequence = sequence.next();
        }
        candidate = candidate.next();
    }
    return std::nullopt;
}

Error ReceiveBuffer::discard_before(
    SequenceNumber next_sequence) noexcept
{
    const std::int32_t distance =
        next_sequence.distance_from(first_stored_sequence_);
    if (distance <= 0) {
        return Error::none;
    }
    if (static_cast<std::uint32_t>(distance)
        >= SequenceNumber::half_range) {
        return Error::invalid_state;
    }

    const auto count = static_cast<std::uint32_t>(distance);
    const std::size_t inspected = std::min<std::size_t>(
        count, slots_.size());
    for (std::size_t index = 0; index < inspected; ++index) {
        auto& slot = slots_[(head_ + index) % capacity()];
        if (slot.occupied) {
            buffered_payload_bytes_ -=
                static_cast<std::size_t>(slot.payload_size)
                - static_cast<std::size_t>(slot.payload_offset);
            --occupied_;
        }
        slot = {};
    }
    if (count >= slots_.size()) {
        for (auto& slot : slots_) {
            slot = {};
        }
        occupied_ = 0U;
        buffered_payload_bytes_ = 0U;
    }
    head_ = (head_ + (count % capacity())) % capacity();
    first_stored_sequence_ = next_sequence;
    if (next_ack_sequence_.distance_from(next_sequence) < 0) {
        next_ack_sequence_ = next_sequence;
    }
    refresh_first_buffered_timestamp();
    return Error::none;
}

std::size_t ReceiveBuffer::buffered_payload_bytes() const noexcept
{
    return buffered_payload_bytes_;
}

std::uint64_t ReceiveBuffer::buffered_span_milliseconds() const noexcept
{
    if (occupied_ == 0U) {
        return 0U;
    }
    const std::int64_t span =
        last_buffered_timestamp_.distance_from(first_buffered_timestamp_);
    return span >= 0
        ? static_cast<std::uint64_t>(span) / 1'000U + 1U
        : 0U;
}

Error ReceiveBuffer::drop_range(SequenceRange range,
    std::uint32_t message_number,
    std::size_t* newly_dropped_packets) noexcept
{
    // The sequence range is authoritative. The message number is advisory
    // metadata used by the wire protocol and may refer to packets that have
    // not reached this receiver yet.
    (void)message_number;
    if (newly_dropped_packets != nullptr) {
        *newly_dropped_packets = 0U;
    }
    const auto last_offset = range.last.distance_from(range.first);
    if (last_offset < 0) {
        return Error::invalid_control_payload;
    }
    const auto start_offset = range.first.distance_from(first_stored_sequence_);
    const auto end_offset = range.last.distance_from(first_stored_sequence_);
    if (end_offset < 0) {
        return Error::none;
    }

    const std::size_t first_offset = start_offset > 0
        ? static_cast<std::size_t>(start_offset) : 0U;
    const std::size_t final_offset = std::min<std::size_t>(
        static_cast<std::size_t>(end_offset), capacity() - 1U);
    for (std::size_t offset = first_offset; offset <= final_offset; ++offset) {
        auto& slot = slots_[(head_ + offset) % capacity()];
        if (!slot.dropped && newly_dropped_packets != nullptr) {
            ++*newly_dropped_packets;
        }
        if (slot.occupied) {
            buffered_payload_bytes_ -=
                static_cast<std::size_t>(slot.payload_size)
                - static_cast<std::size_t>(slot.payload_offset);
            slot.occupied = false;
            slot.payload_size = 0;
            slot.payload_offset = 0;
            --occupied_;
        }
        slot.dropped = true;
    }

    advance_acknowledgement();
    trim_dropped_prefix();
    if (start_offset <= 0
        && range.last.distance_from(first_stored_sequence_) >= 0) {
        const SequenceNumber target = range.last.next();
        const auto additional_advance = static_cast<std::size_t>(
            target.distance_from(first_stored_sequence_));
        if (newly_dropped_packets != nullptr) {
            *newly_dropped_packets += additional_advance;
        }
        head_ = (head_ + additional_advance) % capacity();
        first_stored_sequence_ = target;
    }
    if (first_stored_sequence_.distance_from(next_ack_sequence_) > 0) {
        next_ack_sequence_ = first_stored_sequence_;
        advance_acknowledgement();
    }
    refresh_buffered_timestamp_bounds();
    return Error::none;
}

} // namespace robotweax::srt
