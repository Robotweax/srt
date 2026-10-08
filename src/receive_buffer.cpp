#include "robotweax/srt/receive_buffer.hpp"

#include <algorithm>
#include <bit>
#include <stdexcept>

namespace robotweax::srt {

ReceiveBuffer& ReceiveBuffer::operator=(const ReceiveBuffer& other)
{
    if (this != &other) {
        ReceiveBuffer copy(other);
        *this = std::move(copy);
    }
    return *this;
}

ReceiveBuffer::ReceiveBuffer(
    SequenceNumber initial_sequence, std::size_t capacity_packets)
    : payloads_(capacity_packets)
    , slots_(capacity_packets)
    , first_stored_sequence_(initial_sequence)
    , next_ack_sequence_(initial_sequence)
{
    if (capacity_packets == 0U || capacity_packets >= SequenceNumber::half_range) {
        throw std::invalid_argument("invalid receive-buffer capacity");
    }
    std::size_t words = (capacity_packets + 63U) / 64U;
    std::size_t total_words = 0;
    for (;;) {
        occupied_level_offsets_[occupied_levels_++] = total_words;
        total_words += words;
        if (words == 1U)
            break;
        words = (words + 63U) / 64U;
    }
    occupied_index_.resize(total_words);
}

void ReceiveBuffer::index_occupied(std::size_t position, bool occupied) noexcept
{
    for (std::size_t level = 0; level < occupied_levels_; ++level) {
        const auto word_position = position / 64U;
        auto& word =
            occupied_index_[occupied_level_offsets_[level] + word_position];
        const bool was_nonempty = word != 0U;
        const auto bit = std::uint64_t {1} << (position % 64U);
        if (occupied)
            word |= bit;
        else
            word &= ~bit;
        const bool nonempty = word != 0U;
        if (was_nonempty == nonempty)
            break;
        position = word_position;
        occupied = nonempty;
    }
}

std::optional<std::size_t> ReceiveBuffer::next_occupied_bit(std::size_t level,
    std::size_t begin, std::size_t end, std::size_t* word_reads) const noexcept
{
    if (begin >= end)
        return std::nullopt;
    const auto word_position = begin / 64U;
    if (word_reads != nullptr)
        ++*word_reads;
    const auto word =
        occupied_index_[occupied_level_offsets_[level] + word_position]
        & (~std::uint64_t {0} << (begin % 64U));
    if (word != 0U) {
        const auto found = word_position * 64U
            + static_cast<std::size_t>(std::countr_zero(word));
        return found < end ? std::optional {found} : std::nullopt;
    }
    if (level + 1U == occupied_levels_)
        return std::nullopt;
    const auto next_word = next_occupied_bit(
        level + 1U, word_position + 1U, (end + 63U) / 64U, word_reads);
    if (!next_word)
        return std::nullopt;
    if (word_reads != nullptr)
        ++*word_reads;
    const auto next =
        occupied_index_[occupied_level_offsets_[level] + *next_word];
    const auto found =
        *next_word * 64U + static_cast<std::size_t>(std::countr_zero(next));
    return found < end ? std::optional {found} : std::nullopt;
}

std::optional<std::size_t> ReceiveBuffer::next_occupied_offset(
    std::size_t begin, std::size_t* word_reads) const noexcept
{
    if (begin >= capacity())
        return std::nullopt;
    const auto position = (head_ + begin) % capacity();
    const auto end = std::min(capacity(), position + capacity() - begin);
    auto found = next_occupied_bit(0, position, end, word_reads);
    if (!found && position >= head_ && head_ != 0U)
        found = next_occupied_bit(0, 0, head_, word_reads);
    if (!found)
        return std::nullopt;
    return (*found + capacity() - head_) % capacity();
}

ReceiveBuffer::Slot* ReceiveBuffer::find(SequenceNumber sequence) noexcept
{
    const std::int32_t signed_offset = sequence.distance_from(first_stored_sequence_);
    if (signed_offset < 0 || static_cast<std::size_t>(signed_offset) >= capacity()) {
        return nullptr;
    }
    auto& slot = slots_[(head_ + static_cast<std::size_t>(signed_offset)) % capacity()];
    return slot.occupied && !slot.rejected_payload
            && slot.header.sequence == sequence
        ? &slot
        : nullptr;
}

const ReceiveBuffer::Slot* ReceiveBuffer::find(SequenceNumber sequence) const noexcept
{
    const std::int32_t signed_offset = sequence.distance_from(first_stored_sequence_);
    if (signed_offset < 0 || static_cast<std::size_t>(signed_offset) >= capacity()) {
        return nullptr;
    }
    const auto& slot = slots_[(head_ + static_cast<std::size_t>(signed_offset)) % capacity()];
    return slot.occupied && !slot.rejected_payload
            && slot.header.sequence == sequence
        ? &slot
        : nullptr;
}

bool ReceiveBuffer::is_settled(SequenceNumber sequence) const noexcept
{
    const auto offset = sequence.distance_from(first_stored_sequence_);
    if (offset < 0 || static_cast<std::size_t>(offset) >= capacity()) {
        return false;
    }
    const auto& slot =
        slots_[(head_ + static_cast<std::size_t>(offset)) % capacity()];
    // A dropped empty slot may have no DATA header; its logical sequence is
    // already fixed by the receive-window position.
    return slot.dropped || (slot.occupied && slot.header.sequence == sequence);
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
    // Keep rejection state across gaps and fragments arriving later. Only
    // actually received slots are removed, so ACK/NAK state still describes
    // packet reception rather than message delivery. Storage stays bounded
    // by the receive window; no unbounded message-number tombstone list.
    for (;;) {
        auto& slot = slots_[head_];
        if (slot.dropped) {
            slot.dropped = false;
        } else {
            if (!slot.occupied) {
                return;
            }
            if (discarding_message_.has_value()
                && slot.header.message_number != *discarding_message_) {
                discarding_message_.reset();
            }
            if (!discarding_message_.has_value() && has_rejected_payload_) {
                bool any_rejected = false;
                for (const auto& candidate : slots_) {
                    if (candidate.occupied && candidate.rejected_payload) {
                        any_rejected = true;
                        if (candidate.header.message_number
                            == slot.header.message_number) {
                            discarding_message_ = slot.header.message_number;
                            break;
                        }
                    }
                }
                has_rejected_payload_ = any_rejected;
            }
            if (!discarding_message_.has_value()) {
                return;
            }
            buffered_payload_bytes_ -= slot.payload_size - slot.payload_offset;
            payloads_.release(slot.payload_index);
            slot.occupied = false;
            --occupied_;
            if (slot.header.boundary == MessageBoundary::last
                || slot.header.boundary == MessageBoundary::solo) {
                discarding_message_.reset();
            }
        }
        index_occupied(head_, false);
        invalidate_delivery_queries();
        slot.rejected_payload = false;
        slot.payload_size = 0;
        slot.payload_offset = 0;
        slot.gap_deadline_microseconds = 0;
        head_ = (head_ + 1U) % capacity();
        first_stored_sequence_ = first_stored_sequence_.next();
    }
}

void ReceiveBuffer::discard_message_payload(SequenceNumber sequence) noexcept
{
    if (auto* slot = find(sequence)) {
        invalidate_delivery_queries();
        index_occupied((head_
                           + static_cast<std::size_t>(
                               sequence.distance_from(first_stored_sequence_)))
                % capacity(),
            false);
        slot->rejected_payload = true;
        has_rejected_payload_ = true;
        trim_dropped_prefix();
        refresh_buffered_timestamp_bounds();
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

    invalidate_delivery_queries();
    slot.header = packet.data;
    slot.payload_index = payloads_.acquire(packet.payload);
    slot.payload_size = static_cast<std::uint16_t>(packet.payload.size());
    slot.payload_offset = 0;
    slot.rejected_payload = false;
    slot.occupied = true;
    index_occupied((head_ + offset) % capacity(), true);
    slot.dropped = false;
    slot.gap_deadline_microseconds = 0;
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
    if (has_rejected_payload_ || discarding_message_.has_value()) {
        trim_dropped_prefix();
        refresh_buffered_timestamp_bounds();
    }
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

BufferedMessageCopies ReceiveBuffer::copy_complete_messages(
    std::size_t maximum_packets, std::size_t maximum_bytes) const noexcept
{
    BufferedMessageCopies result;
    std::size_t packets = 0;
    std::size_t bytes = 0;
    try {
        for (std::size_t offset = 0; offset < capacity() && occupied_ != 0U;
            ++offset) {
            const auto first_sequence = first_stored_sequence_.advanced(
                static_cast<std::uint32_t>(offset));
            const auto* first = find(first_sequence);
            if (first == nullptr || first->payload_offset != 0U
                || (first->header.boundary != MessageBoundary::solo
                    && first->header.boundary != MessageBoundary::first)) {
                continue;
            }
            const auto last = complete_message_last_offset(offset);
            if (!last.has_value()) {
                continue;
            }
            const auto packet_count = *last - offset + 1U;
            if (packet_count > maximum_packets) {
                offset = *last;
                continue;
            }
            std::size_t message_bytes = 0;
            bool oversized = false;
            for (std::size_t index = offset; index <= *last; ++index) {
                const auto* slot = find(first_stored_sequence_.advanced(
                    static_cast<std::uint32_t>(index)));
                if (slot == nullptr || slot->payload_offset != 0U) {
                    return {.error = Error::invalid_state};
                }
                if (slot->payload_size > maximum_bytes - message_bytes) {
                    oversized = true;
                    break;
                }
                message_bytes += slot->payload_size;
            }
            if (oversized) {
                offset = *last;
                continue;
            }
            // Keep the earliest complete messages within the copy budget.
            // An individually oversized message does not erase that prefix
            // or prevent later, retainable messages from being considered.
            if (packet_count > maximum_packets - packets
                || message_bytes > maximum_bytes - bytes) {
                break;
            }
            BufferedMessageCopy copy {
                .first_sequence = first_sequence,
                .next_sequence = first_stored_sequence_.advanced(
                    static_cast<std::uint32_t>(*last + 1U)),
                .timestamp = first->header.timestamp,
                .message_number = first->header.message_number,
            };
            copy.payload.resize(message_bytes);
            std::size_t written = 0;
            for (std::size_t index = offset; index <= *last; ++index) {
                const auto* slot = find(first_stored_sequence_.advanced(
                    static_cast<std::uint32_t>(index)));
                if (slot == nullptr) {
                    return {.error = Error::invalid_state};
                }
                std::copy_n(payloads_.get(slot->payload_index).begin(),
                    slot->payload_size,
                    copy.payload.begin()
                        + static_cast<std::ptrdiff_t>(written));
                written += slot->payload_size;
            }
            result.messages.push_back(std::move(copy));
            packets += packet_count;
            bytes += message_bytes;
            offset = *last;
        }
    } catch (...) {
        return {.error = Error::buffer_too_small};
    }
    return result;
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

    invalidate_delivery_queries();
    std::size_t written = 0;
    for (std::size_t index = 0; index < packet_count; ++index) {
        auto& slot = slots_[(head_ + index) % capacity()];
        std::copy_n(payloads_.get(slot.payload_index).begin(),
            slot.payload_size,
            destination.begin() + static_cast<std::ptrdiff_t>(written));
        written += slot.payload_size;
        buffered_payload_bytes_ -= slot.payload_size;
        payloads_.release(slot.payload_index);
        index_occupied((head_ + index) % capacity(), false);
        slot.occupied = false;
        slot.dropped = false;
        slot.payload_size = 0;
        slot.payload_offset = 0;
        slot.gap_deadline_microseconds = 0;
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

ReceivedMessageResult ReceiveBuffer::pop_message_unordered(
    std::span<std::byte> destination) noexcept
{
    const auto message = first_complete_message();
    if (!message.has_value()) {
        return {.error = Error::would_block};
    }
    if (message->first_sequence == first_stored_sequence_) {
        return pop_message(destination);
    }
    if (message->first_sequence != message->last_sequence) {
        return {.error = Error::invalid_state};
    }
    auto* slot = find(message->first_sequence);
    if (slot == nullptr || slot->header.boundary != MessageBoundary::solo
        || slot->payload_offset != 0U) {
        return {.error = Error::invalid_state};
    }
    if (destination.size() < slot->payload_size) {
        return {.error = Error::buffer_too_small};
    }

    invalidate_delivery_queries();
    const std::size_t size = slot->payload_size;
    const std::uint32_t message_number = slot->header.message_number;
    const PacketTimestamp timestamp = slot->header.timestamp;
    std::copy_n(
        payloads_.get(slot->payload_index).begin(), size, destination.begin());
    payloads_.release(slot->payload_index);
    buffered_payload_bytes_ -= size;
    --occupied_;
    index_occupied(
        (head_
            + static_cast<std::size_t>(
                message->first_sequence.distance_from(first_stored_sequence_)))
            % capacity(),
        false);
    slot->occupied = false;
    // A delivered marker remains in the fixed sequence slot until the
    // cumulative frontier reaches it. It suppresses duplicates and lets ACK
    // progression distinguish a resolved sample from an unobserved gap.
    slot->dropped = true;
    slot->payload_size = 0;
    slot->payload_offset = 0;
    slot->gap_deadline_microseconds = 0;
    advance_acknowledgement();
    trim_dropped_prefix();
    refresh_buffered_timestamp_bounds();
    return {
        .bytes_written = size,
        .message_number = message_number,
        .first_sequence = message->first_sequence,
        .timestamp = timestamp,
    };
}

ReceivedMessageResult ReceiveBuffer::pop_stream(
    std::span<std::byte> destination, PacketDuePredicate due,
    void* due_context) noexcept
{
    if (destination.empty()) {
        return {.error = Error::invalid_state};
    }
    const auto* first = find(first_stored_sequence_);
    if (first == nullptr
        || (due != nullptr && !due(first->header.timestamp, due_context))) {
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
        if (slot == nullptr
            || (due != nullptr && !due(slot->header.timestamp, due_context))) {
            break;
        }
        const std::size_t remaining =
            static_cast<std::size_t>(slot->payload_size)
            - static_cast<std::size_t>(slot->payload_offset);
        const std::size_t copied = std::min(
            remaining, destination.size() - written);
        std::copy_n(payloads_.get(slot->payload_index).begin()
                + static_cast<std::ptrdiff_t>(slot->payload_offset),
            copied, destination.begin() + static_cast<std::ptrdiff_t>(written));
        written += copied;
        buffered_payload_bytes_ -= copied;
        slot->payload_offset = static_cast<std::uint16_t>(
            static_cast<std::size_t>(slot->payload_offset) + copied);

        if (slot->payload_offset != slot->payload_size) {
            break;
        }
        invalidate_delivery_queries();
        payloads_.release(slot->payload_index);
        index_occupied(head_, false);
        slot->occupied = false;
        slot->dropped = false;
        slot->payload_size = 0;
        slot->payload_offset = 0;
        slot->gap_deadline_microseconds = 0;
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

std::size_t ReceiveBuffer::window_available() const noexcept
{
    for (std::size_t offset = capacity(); offset > 0U; --offset) {
        const auto& slot = slots_[(head_ + offset - 1U) % capacity()];
        if (slot.occupied || slot.dropped) {
            return capacity() - offset;
        }
    }
    return capacity();
}

void ReceiveBuffer::mark_gap(
    SequenceRange range, std::uint64_t deadline_microseconds) noexcept
{
    if (deadline_microseconds == 0U
        || range.last.distance_from(range.first) < 0) {
        return;
    }
    SequenceNumber sequence = range.first;
    for (;;) {
        const std::int32_t signed_offset =
            sequence.distance_from(first_stored_sequence_);
        if (signed_offset >= 0
            && static_cast<std::size_t>(signed_offset) < capacity()) {
            auto& slot =
                slots_[(head_ + static_cast<std::size_t>(signed_offset))
                    % capacity()];
            if (!slot.occupied && !slot.dropped
                && slot.gap_deadline_microseconds == 0U) {
                slot.gap_deadline_microseconds = deadline_microseconds;
            }
        }
        if (sequence == range.last) {
            break;
        }
        sequence = sequence.next();
    }
}

std::optional<std::uint64_t> ReceiveBuffer::next_gap_deadline() const noexcept
{
    std::optional<std::uint64_t> earliest;
    for (const auto& slot : slots_) {
        if (slot.gap_deadline_microseconds != 0U
            && (!earliest.has_value()
                || slot.gap_deadline_microseconds < *earliest)) {
            earliest = slot.gap_deadline_microseconds;
        }
    }
    return earliest;
}

std::optional<SequenceNumber> ReceiveBuffer::next_expired_gap(
    std::uint64_t now_microseconds) const noexcept
{
    for (std::size_t offset = 0U; offset < capacity(); ++offset) {
        const auto& slot = slots_[(head_ + offset) % capacity()];
        if (!slot.occupied && !slot.dropped
            && slot.gap_deadline_microseconds != 0U
            && slot.gap_deadline_microseconds <= now_microseconds) {
            return first_stored_sequence_.advanced(
                static_cast<std::uint32_t>(offset));
        }
    }
    return std::nullopt;
}

bool ReceiveBuffer::has_complete_message() const noexcept
{
    // Only a message starting at the head qualifies. A missing head packet
    // decides the answer without scanning the buffer behind the gap.
    const auto* head = find(first_stored_sequence_);
    if (head == nullptr
        || (head->header.boundary != MessageBoundary::solo
            && head->header.boundary != MessageBoundary::first)) {
        return false;
    }
    const auto message = first_complete_message();
    return message.has_value()
        && message->first_sequence == first_stored_sequence_;
}

std::optional<BufferedMessageInfo>
ReceiveBuffer::first_buffered_packet() const noexcept
{
    return query_first_buffered_packet(nullptr);
}

std::optional<BufferedMessageInfo> ReceiveBuffer::query_first_buffered_packet(
    std::size_t* inspected) const noexcept
{
    if (packet_query_valid_) {
        return packet_query_;
    }
    const auto remember = [this](std::optional<BufferedMessageInfo> packet) {
        packet_query_ = packet;
        packet_query_valid_ = true;
        return packet;
    };
    if (occupied_ == 0U) {
        return remember(std::nullopt);
    }
    if (const auto offset = next_occupied_offset(0); offset.has_value()) {
        const auto candidate = first_stored_sequence_.advanced(
            static_cast<std::uint32_t>(*offset));
        if (inspected != nullptr)
            ++*inspected;
        const auto* slot = find(candidate);
        if (slot != nullptr)
            return remember(BufferedMessageInfo {
                candidate, candidate, slot->header.timestamp});
    }
    return remember(std::nullopt);
}

std::optional<BufferedMessageInfo>
ReceiveBuffer::first_complete_message() const noexcept
{
    return query_first_complete_message(nullptr);
}

std::optional<BufferedMessageInfo> ReceiveBuffer::query_first_complete_message(
    std::size_t* inspected) const noexcept
{
    if (message_query_valid_) {
        return message_query_;
    }
    const auto remember = [this](std::optional<BufferedMessageInfo> message) {
        message_query_ = message;
        message_query_valid_ = true;
        return message;
    };
    // Runtime readiness also queries idle receive sides of active senders.
    // No occupied packet means no complete message, regardless of capacity.
    if (occupied_ == 0U) {
        return remember(std::nullopt);
    }
    // Walk retained DATA, not empty sequence positions. A new incomplete
    // packet must not invalidate the cache into another capacity-sized gap scan.
    for (auto offset = next_occupied_offset(0); offset.has_value();
        offset = next_occupied_offset(*offset + 1U)) {
        const auto candidate = first_stored_sequence_.advanced(
            static_cast<std::uint32_t>(*offset));
        if (inspected != nullptr)
            ++*inspected;
        const auto* first = find(candidate);
        if (first == nullptr
            || (first->header.boundary != MessageBoundary::solo
                && first->header.boundary != MessageBoundary::first)) {
            continue;
        }

        if (first->header.boundary == MessageBoundary::solo) {
            return remember(BufferedMessageInfo {
                .first_sequence = candidate,
                .last_sequence = candidate,
                .timestamp = first->header.timestamp,
            });
        }

        const std::uint32_t message_number =
            first->header.message_number;
        SequenceNumber sequence = candidate.next();
        const std::size_t remaining = capacity() - *offset - 1U;
        for (std::size_t fragment = 0;
             fragment < remaining;
             ++fragment) {
            if (inspected != nullptr)
                ++*inspected;
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
                return remember(BufferedMessageInfo {
                    .first_sequence = candidate,
                    .last_sequence = sequence,
                    .timestamp = first->header.timestamp,
                });
            }
            sequence = sequence.next();
        }
    }
    return remember(std::nullopt);
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

    invalidate_delivery_queries();
    const auto count = static_cast<std::uint32_t>(distance);
    const std::size_t inspected = std::min<std::size_t>(
        count, slots_.size());
    for (std::size_t index = 0; index < inspected; ++index) {
        auto& slot = slots_[(head_ + index) % capacity()];
        if (slot.occupied) {
            buffered_payload_bytes_ -=
                static_cast<std::size_t>(slot.payload_size)
                - static_cast<std::size_t>(slot.payload_offset);
            payloads_.release(slot.payload_index);
            --occupied_;
        }
        index_occupied((head_ + index) % capacity(), false);
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
    std::uint32_t message_number, std::size_t* newly_dropped_packets) noexcept
{
    return drop_range_impl(
        range, message_number, newly_dropped_packets, DropPreservation::none);
}

Error ReceiveBuffer::drop_peer_requested_range(SequenceRange range,
    std::uint32_t message_number, std::size_t* newly_dropped_packets) noexcept
{
    return drop_range_impl(range, message_number, newly_dropped_packets,
        DropPreservation::complete_messages);
}

Error ReceiveBuffer::drop_peer_requested_stream_range(SequenceRange range,
    std::uint32_t message_number, std::size_t* newly_dropped_packets) noexcept
{
    return drop_range_impl(range, message_number, newly_dropped_packets,
        DropPreservation::received_packets);
}

bool ReceiveBuffer::acknowledge_peer_drop_range(SequenceRange range) noexcept
{
    if (range.last.distance_from(range.first) < 0
        || range.first.distance_from(next_ack_sequence_) > 0
        || range.last.distance_from(next_ack_sequence_) < 0) {
        return false;
    }
    next_ack_sequence_ = range.last.next();
    advance_acknowledgement();
    return true;
}

std::optional<std::size_t> ReceiveBuffer::complete_message_last_offset(
    std::size_t offset) const noexcept
{
    const auto slot_at = [this](std::size_t index) -> const Slot* {
        const auto& slot = slots_[(head_ + index) % capacity()];
        return slot.occupied
                && slot.header.sequence
                    == first_stored_sequence_.advanced(
                        static_cast<std::uint32_t>(index))
            ? &slot
            : nullptr;
    };
    const Slot* member = slot_at(offset);
    if (member == nullptr || member->payload_offset != 0U) {
        return std::nullopt;
    }
    if (member->header.boundary == MessageBoundary::solo) {
        return offset;
    }

    const auto message_number = member->header.message_number;
    std::size_t first = offset;
    while (first > 0U) {
        const auto* current = slot_at(first);
        if (current == nullptr
            || current->header.message_number != message_number) {
            return std::nullopt;
        }
        if (current->header.boundary == MessageBoundary::first) {
            break;
        }
        if (first != offset
            && current->header.boundary != MessageBoundary::subsequent) {
            return std::nullopt;
        }
        if (first == offset
            && current->header.boundary != MessageBoundary::subsequent
            && current->header.boundary != MessageBoundary::last) {
            return std::nullopt;
        }
        --first;
    }
    const auto* start = slot_at(first);
    if (start == nullptr || start->header.boundary != MessageBoundary::first
        || start->header.message_number != message_number) {
        return std::nullopt;
    }
    for (std::size_t index = first + 1U; index < capacity(); ++index) {
        const auto* fragment = slot_at(index);
        if (fragment == nullptr || fragment->payload_offset != 0U
            || fragment->header.message_number != message_number) {
            return std::nullopt;
        }
        if (fragment->header.boundary == MessageBoundary::last) {
            return index;
        }
        if (fragment->header.boundary != MessageBoundary::subsequent) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

Error ReceiveBuffer::drop_range_impl(SequenceRange range,
    std::uint32_t message_number, std::size_t* newly_dropped_packets,
    DropPreservation preservation) noexcept
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

    const std::size_t first_offset =
        start_offset > 0 ? static_cast<std::size_t>(start_offset) : 0U;
    const std::size_t final_offset = std::min<std::size_t>(
        static_cast<std::size_t>(end_offset), capacity() - 1U);
    std::optional<SequenceNumber> first_preserved_sequence;
    std::optional<std::size_t> preserve_through_offset;
    for (std::size_t offset = first_offset; offset <= final_offset; ++offset) {
        auto& slot = slots_[(head_ + offset) % capacity()];
        if (preservation == DropPreservation::received_packets && slot.occupied
            && !slot.rejected_payload
            && slot.header.sequence
                == first_stored_sequence_.advanced(
                    static_cast<std::uint32_t>(offset))) {
            if (!first_preserved_sequence.has_value()) {
                first_preserved_sequence = slot.header.sequence;
            }
            continue;
        }
        if (preservation == DropPreservation::complete_messages) {
            if (preserve_through_offset.has_value()
                && offset <= *preserve_through_offset) {
                continue;
            }
            if (const auto last = complete_message_last_offset(offset);
                last.has_value()) {
                if (!first_preserved_sequence.has_value()) {
                    first_preserved_sequence = slot.header.sequence;
                }
                preserve_through_offset = *last;
                continue;
            }
        }
        if (!slot.dropped && newly_dropped_packets != nullptr) {
            ++*newly_dropped_packets;
        }
        if (slot.occupied) {
            invalidate_delivery_queries();
            buffered_payload_bytes_ -=
                static_cast<std::size_t>(slot.payload_size)
                - static_cast<std::size_t>(slot.payload_offset);
            payloads_.release(slot.payload_index);
            index_occupied((head_ + offset) % capacity(), false);
            slot.occupied = false;
            slot.payload_size = 0;
            slot.payload_offset = 0;
            --occupied_;
        }
        slot.dropped = true;
        slot.gap_deadline_microseconds = 0;
    }

    advance_acknowledgement();
    trim_dropped_prefix();
    if (start_offset <= 0
        && range.last.distance_from(first_stored_sequence_) >= 0) {
        SequenceNumber target = range.last.next();
        if (first_preserved_sequence.has_value()
            && first_preserved_sequence->distance_from(first_stored_sequence_)
                >= 0
            && target.distance_from(*first_preserved_sequence) > 0) {
            target = *first_preserved_sequence;
        }
        const auto additional_advance = static_cast<std::size_t>(
            target.distance_from(first_stored_sequence_));
        if (newly_dropped_packets != nullptr) {
            *newly_dropped_packets += additional_advance;
        }
        if (additional_advance != 0U) {
            invalidate_delivery_queries();
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
