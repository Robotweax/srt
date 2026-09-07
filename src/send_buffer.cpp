#include "robotweax/srt/send_buffer.hpp"

#include <algorithm>
#include <stdexcept>

namespace robotweax::srt {

SendBuffer::SendBuffer(SequenceNumber initial_sequence,
    std::size_t capacity_packets, std::size_t maximum_payload_size)
    : slots_(capacity_packets)
    , retransmission_queue_(capacity_packets)
    , drop_request_queue_(capacity_packets)
    , range_drop_request_queue_(
          std::max(capacity_packets, maximum_loss_words_per_packet))
    , first_sequence_(initial_sequence)
    , maximum_payload_size_(maximum_payload_size)
{
    if (capacity_packets == 0U || capacity_packets >= SequenceNumber::half_range
        || maximum_payload_size == 0U
        || maximum_payload_size > maximum_data_payload_size) {
        throw std::invalid_argument("invalid send-buffer dimensions");
    }
}

bool SendBuffer::synchronize_empty(
    SequenceNumber next_sequence) noexcept
{
    if (sequence_span_ != 0U || occupied_count_ != 0U) {
        return this->next_sequence() == next_sequence;
    }
    first_sequence_ = next_sequence;
    head_ = 0U;
    buffered_plaintext_bytes_ = 0U;
    expiring_packet_count_ = 0U;
    first_buffered_enqueue_microseconds_ = 0U;
    last_buffered_enqueue_microseconds_ = 0U;
    return true;
}

Error SendBuffer::enqueue_message(
    std::span<const std::byte> message,
    std::uint32_t message_number,
    PacketTimestamp timestamp,
    std::uint32_t destination_socket_id,
    bool in_order,
    std::uint64_t enqueue_microseconds,
    std::uint64_t expiration_microseconds) noexcept
{
    if (message.empty()) {
        return Error::invalid_state;
    }
    const std::size_t packet_count =
        (message.size() + maximum_payload_size_ - 1U) / maximum_payload_size_;
    if (packet_count > available()) {
        return Error::buffer_too_small;
    }

    std::size_t consumed = 0;
    for (std::size_t packet_index = 0; packet_index < packet_count; ++packet_index) {
        const std::size_t slot_index =
            (head_ + sequence_span_) % capacity();
        auto& slot = slots_[slot_index];
        const std::size_t payload_size = std::min(
            maximum_payload_size_, message.size() - consumed);
        const bool first = packet_index == 0U;
        const bool last = packet_index + 1U == packet_count;
        const MessageBoundary boundary = first && last
            ? MessageBoundary::solo
            : (first ? MessageBoundary::first
                     : (last ? MessageBoundary::last : MessageBoundary::subsequent));

        slot.header = {
            .sequence = first_sequence_.advanced(
                static_cast<std::uint32_t>(sequence_span_)),
            .message_number = message_number & 0x03ff'ffffU,
            .boundary = boundary,
            .in_order = in_order,
            .encryption_key = EncryptionKey::none,
            .retransmitted = false,
            .timestamp = timestamp,
            .destination_socket_id = destination_socket_id,
        };
        std::copy_n(message.begin() + static_cast<std::ptrdiff_t>(consumed),
            payload_size, slot.payload.begin());
        slot.payload_size = static_cast<std::uint16_t>(payload_size);
        slot.plaintext_size = static_cast<std::uint16_t>(payload_size);
        slot.protection_mode = CryptoMode::automatic;
        slot.occupied = true;
        slot.dropped = false;
        slot.sent = false;
        slot.retransmission_queued = false;
        slot.enqueue_microseconds = enqueue_microseconds;
        slot.expiration_microseconds = expiration_microseconds;
        consumed += payload_size;
        if (occupied_count_ == 0U) {
            first_buffered_enqueue_microseconds_ = enqueue_microseconds;
        }
        last_buffered_enqueue_microseconds_ = enqueue_microseconds;
        ++sequence_span_;
        ++occupied_count_;
        buffered_plaintext_bytes_ += payload_size;
        if (expiration_microseconds != 0U) {
            ++expiring_packet_count_;
        }
    }
    return Error::none;
}

StreamEnqueueResult SendBuffer::enqueue_stream(
    std::span<const std::byte> bytes,
    std::uint32_t message_number,
    PacketTimestamp timestamp,
    std::uint32_t destination_socket_id,
    std::uint64_t enqueue_microseconds) noexcept
{
    if (bytes.empty()) {
        return {.error = Error::invalid_state};
    }
    if (available() == 0U) {
        return {.error = Error::buffer_too_small};
    }
    const std::size_t maximum_accepted =
        available() * maximum_payload_size_;
    const std::size_t accepted =
        std::min(bytes.size(), maximum_accepted);
    const auto error = enqueue_message(bytes.first(accepted),
        message_number, timestamp, destination_socket_id, true,
        enqueue_microseconds);
    return {
        .error = error,
        .bytes_accepted = error == Error::none ? accepted : 0U,
    };
}

SendBuffer::Slot* SendBuffer::find(SequenceNumber sequence) noexcept
{
    const std::int32_t signed_offset = sequence.distance_from(first_sequence_);
    if (signed_offset < 0) {
        return nullptr;
    }
    const auto offset = static_cast<std::size_t>(signed_offset);
    if (offset >= sequence_span_) {
        return nullptr;
    }
    auto& slot = slots_[(head_ + offset) % capacity()];
    return slot.occupied ? &slot : nullptr;
}

void SendBuffer::discard_slot(
    Slot& slot, bool retain_drop_marker) noexcept
{
    if (slot.occupied) {
        if (slot.sent) {
            --packets_in_flight_;
        }
        buffered_plaintext_bytes_ -= slot.plaintext_size;
        if (slot.expiration_microseconds != 0U) {
            --expiring_packet_count_;
        }
        --occupied_count_;
        if (occupied_count_ == 0U) {
            first_buffered_enqueue_microseconds_ = 0U;
            last_buffered_enqueue_microseconds_ = 0U;
        }
    }
    slot.occupied = false;
    slot.dropped = retain_drop_marker;
    slot.drop_request_queued = false;
    slot.sent = false;
    slot.retransmission_queued = false;
    if (!retain_drop_marker) {
        slot.payload_size = 0;
        slot.plaintext_size = 0;
        slot.protection_mode = CryptoMode::automatic;
        slot.enqueue_microseconds = 0;
        slot.expiration_microseconds = 0;
    }
}

void SendBuffer::refresh_first_buffered_enqueue_time() noexcept
{
    if (occupied_count_ == 0U) {
        first_buffered_enqueue_microseconds_ = 0U;
        last_buffered_enqueue_microseconds_ = 0U;
        return;
    }
    for (std::size_t offset = 0; offset < sequence_span_; ++offset) {
        const auto& slot = slots_[(head_ + offset) % capacity()];
        if (slot.occupied) {
            first_buffered_enqueue_microseconds_ = slot.enqueue_microseconds;
            return;
        }
    }
}

void SendBuffer::refresh_buffered_enqueue_time_bounds() noexcept
{
    refresh_first_buffered_enqueue_time();
    if (occupied_count_ == 0U) {
        return;
    }
    for (std::size_t offset = sequence_span_; offset > 0U; --offset) {
        const auto& slot = slots_[(head_ + offset - 1U) % capacity()];
        if (slot.occupied) {
            last_buffered_enqueue_microseconds_ = slot.enqueue_microseconds;
            return;
        }
    }
}

void SendBuffer::compact_drop_request_queue() noexcept
{
    const std::size_t original_head = drop_request_head_;
    std::size_t kept = 0;
    for (std::size_t index = 0; index < drop_request_size_; ++index) {
        const SequenceNumber sequence =
            drop_request_queue_[(original_head + index) % capacity()];
        const std::int32_t signed_offset =
            sequence.distance_from(first_sequence_);
        if (signed_offset < 0
            || static_cast<std::size_t>(signed_offset) >= sequence_span_) {
            continue;
        }
        auto& slot = slots_[(head_ + static_cast<std::size_t>(signed_offset))
            % capacity()];
        if (slot.dropped && slot.drop_request_queued) {
            drop_request_queue_[(original_head + kept) % capacity()] = sequence;
            ++kept;
        }
    }
    if (kept == 0U) {
        drop_request_head_ = 0U;
    }
    drop_request_size_ = kept;
}

bool SendBuffer::queue_drop_request(SequenceNumber sequence) noexcept
{
    const std::int32_t signed_offset = sequence.distance_from(first_sequence_);
    if (signed_offset < 0
        || static_cast<std::size_t>(signed_offset) >= sequence_span_) {
        return false;
    }

    std::size_t first_offset = static_cast<std::size_t>(signed_offset);
    auto& candidate = slots_[(head_ + first_offset) % capacity()];
    if (!candidate.dropped) {
        return false;
    }
    while (first_offset > 0U) {
        const auto& previous = slots_[(head_ + first_offset - 1U) % capacity()];
        if (!previous.dropped
            || previous.header.message_number
                != candidate.header.message_number) {
            break;
        }
        --first_offset;
    }

    auto& first = slots_[(head_ + first_offset) % capacity()];
    if (first.drop_request_queued) {
        return true;
    }
    if (drop_request_size_ == capacity()) {
        compact_drop_request_queue();
        if (drop_request_size_ == capacity()) {
            return false;
        }
    }
    const std::size_t tail =
        (drop_request_head_ + drop_request_size_) % capacity();
    drop_request_queue_[tail] =
        first_sequence_.advanced(static_cast<std::uint32_t>(first_offset));
    ++drop_request_size_;
    first.drop_request_queued = true;
    return true;
}

void SendBuffer::compact_retransmission_queue() noexcept
{
    std::size_t kept = 0;
    for (std::size_t index = 0; index < retransmission_size_; ++index) {
        const auto sequence = retransmission_queue_[
            (retransmission_head_ + index) % capacity()];
        const auto* slot = find(sequence);
        if (slot != nullptr && slot->retransmission_queued) {
            retransmission_queue_[kept++] = sequence;
        }
    }
    retransmission_head_ = 0;
    retransmission_size_ = kept;
}

bool SendBuffer::queue_retransmission(SequenceNumber sequence) noexcept
{
    const std::int32_t signed_offset =
        sequence.distance_from(first_sequence_);
    if (signed_offset < 0
        || static_cast<std::size_t>(signed_offset) >= sequence_span_) {
        return false;
    }
    auto& stored =
        slots_[(head_ + static_cast<std::size_t>(signed_offset))
            % capacity()];
    if (stored.dropped) {
        return queue_drop_request(sequence);
    }
    if (!stored.occupied) {
        return false;
    }
    auto* slot = &stored;
    if (!slot->sent || slot->retransmission_queued) {
        return true;
    }
    if (retransmission_size_ == capacity()) {
        compact_retransmission_queue();
        if (retransmission_size_ == capacity()) {
            return false;
        }
    }
    const std::size_t tail =
        (retransmission_head_ + retransmission_size_) % capacity();
    retransmission_queue_[tail] = sequence;
    ++retransmission_size_;
    slot->retransmission_queued = true;
    return true;
}

std::optional<OutboundPacket> SendBuffer::next_packet() noexcept
{
    while (retransmission_size_ > 0U) {
        const auto sequence = retransmission_queue_[retransmission_head_];
        retransmission_head_ = (retransmission_head_ + 1U) % capacity();
        --retransmission_size_;
        auto* slot = find(sequence);
        if (slot == nullptr || !slot->retransmission_queued) {
            continue;
        }
        slot->retransmission_queued = false;
        auto header = slot->header;
        header.retransmitted = true;
        return OutboundPacket{
            .header = header,
            .payload = std::span{slot->payload}.first(slot->payload_size),
        };
    }

    for (std::size_t offset = 0; offset < sequence_span_; ++offset) {
        auto& slot = slots_[(head_ + offset) % capacity()];
        if (slot.occupied && !slot.sent) {
            slot.sent = true;
            ++packets_in_flight_;
            return OutboundPacket{
                .header = slot.header,
                .payload = std::span{slot.payload}.first(slot.payload_size),
            };
        }
    }
    return std::nullopt;
}

Error SendBuffer::preserve_encrypted_payload(
    SequenceNumber sequence,
    EncryptionKey key,
    std::span<const std::byte> ciphertext) noexcept
{
    return preserve_protected_payload(
        sequence, key, CryptoMode::aes_ctr, ciphertext);
}

Error SendBuffer::preserve_protected_payload(SequenceNumber sequence,
    EncryptionKey key, CryptoMode mode,
    std::span<const std::byte> protected_payload) noexcept
{
    Slot* slot = find(sequence);
    const CryptoSuite* suite = crypto_suite_for_mode(mode);
    if (slot == nullptr || !slot->sent || suite == nullptr
        || (key != EncryptionKey::even && key != EncryptionKey::odd)) {
        return Error::invalid_state;
    }
    if (slot->plaintext_size
        > maximum_data_payload_size - suite->authentication_tag_size) {
        return Error::buffer_too_small;
    }
    const std::size_t expected_size =
        slot->plaintext_size + suite->authentication_tag_size;
    if (protected_payload.size() != expected_size) {
        return Error::invalid_payload_size;
    }
    if (slot->header.encryption_key != EncryptionKey::none) {
        return slot->header.encryption_key == key
                && slot->protection_mode == mode
                && slot->payload_size == protected_payload.size()
                && std::equal(protected_payload.begin(),
                    protected_payload.end(), slot->payload.begin())
            ? Error::none
            : Error::invalid_state;
    }
    std::copy(protected_payload.begin(), protected_payload.end(),
        slot->payload.begin());
    slot->payload_size = static_cast<std::uint16_t>(protected_payload.size());
    slot->protection_mode = mode;
    slot->header.encryption_key = key;
    return Error::none;
}

bool SendBuffer::has_pending_retransmission() noexcept
{
    for (std::size_t index = 0; index < retransmission_size_; ++index) {
        const auto sequence = retransmission_queue_[
            (retransmission_head_ + index) % capacity()];
        const auto* slot = find(sequence);
        if (slot != nullptr && slot->retransmission_queued) {
            return true;
        }
    }
    return false;
}

Error SendBuffer::acknowledge_before(SequenceNumber sequence) noexcept
{
    const std::int32_t signed_count = sequence.distance_from(first_sequence_);
    if (signed_count < 0) {
        return Error::none;
    }
    if (static_cast<std::size_t>(signed_count) > sequence_span_) {
        return Error::invalid_control_payload;
    }
    const auto count = static_cast<std::size_t>(signed_count);
    for (std::size_t index = 0; index < count; ++index) {
        auto& slot = slots_[(head_ + index) % capacity()];
        discard_slot(slot);
    }
    head_ = (head_ + count) % capacity();
    sequence_span_ -= count;
    first_sequence_ = sequence;
    if (count != 0U) {
        refresh_first_buffered_enqueue_time();
    }
    return Error::none;
}

Error SendBuffer::validate_retransmission_range(
    SequenceRange range) const noexcept
{
    const std::int32_t signed_count = range.last.distance_from(range.first);
    const std::int32_t first_offset =
        range.first.distance_from(first_sequence_);
    const std::int32_t last_offset = range.last.distance_from(first_sequence_);
    if (signed_count < 0 || first_offset < 0 || last_offset < first_offset
        || static_cast<std::size_t>(last_offset) >= sequence_span_) {
        return Error::invalid_control_payload;
    }

    for (std::size_t offset = static_cast<std::size_t>(first_offset);
        offset <= static_cast<std::size_t>(last_offset); ++offset) {
        const auto& slot = slots_[(head_ + offset) % capacity()];
        if (!slot.dropped && (!slot.occupied || !slot.sent)) {
            return Error::invalid_control_payload;
        }
    }
    return Error::none;
}

Error SendBuffer::request_retransmission(
    SequenceRange range,
    std::size_t* newly_queued_packets,
    std::size_t* newly_queued_bytes) noexcept
{
    if (newly_queued_packets != nullptr) {
        *newly_queued_packets = 0U;
    }
    if (newly_queued_bytes != nullptr) {
        *newly_queued_bytes = 0U;
    }
    const Error validation = validate_retransmission_range(range);
    if (validation != Error::none) {
        return validation;
    }
    const auto count =
        static_cast<std::uint32_t>(range.last.distance_from(range.first)) + 1U;
    for (std::uint32_t index = 0; index < count; ++index) {
        const SequenceNumber sequence =
            range.first.advanced(index);
        const auto* slot = find(sequence);
        const bool newly_queued =
            slot != nullptr && slot->sent
            && !slot->retransmission_queued;
        const std::size_t plaintext_size =
            newly_queued ? slot->plaintext_size : 0U;
        if (!queue_retransmission(sequence)) {
            return Error::invalid_control_payload;
        }
        if (newly_queued) {
            if (newly_queued_packets != nullptr) {
                ++*newly_queued_packets;
            }
            if (newly_queued_bytes != nullptr) {
                *newly_queued_bytes += plaintext_size;
            }
        }
    }
    return Error::none;
}

std::size_t SendBuffer::request_retransmission_of_all_sent() noexcept
{
    std::size_t queued = 0;
    for (std::size_t offset = 0; offset < sequence_span_; ++offset) {
        auto& slot = slots_[(head_ + offset) % capacity()];
        if (!slot.occupied || !slot.sent
            || slot.retransmission_queued) {
            continue;
        }
        if (!queue_retransmission(slot.header.sequence)) {
            break;
        }
        ++queued;
    }
    return queued;
}

std::optional<SendDropResult> SendBuffer::next_pending_drop_request() noexcept
{
    while (drop_request_size_ > 0U) {
        const SequenceNumber sequence = drop_request_queue_[drop_request_head_];
        drop_request_head_ = (drop_request_head_ + 1U) % capacity();
        --drop_request_size_;

        const std::int32_t signed_offset =
            sequence.distance_from(first_sequence_);
        if (signed_offset < 0
            || static_cast<std::size_t>(signed_offset) >= sequence_span_) {
            continue;
        }
        const std::size_t first_offset =
            static_cast<std::size_t>(signed_offset);
        auto& first = slots_[(head_ + first_offset) % capacity()];
        if (!first.dropped || !first.drop_request_queued) {
            continue;
        }
        first.drop_request_queued = false;

        std::size_t final_offset = first_offset;
        std::size_t bytes = first.plaintext_size;
        while (final_offset + 1U < sequence_span_) {
            auto& next = slots_[(head_ + final_offset + 1U) % capacity()];
            if (!next.dropped
                || next.header.message_number != first.header.message_number) {
                break;
            }
            next.drop_request_queued = false;
            bytes += next.plaintext_size;
            ++final_offset;
        }
        return SendDropResult {
            .packets = final_offset - first_offset + 1U,
            .bytes = bytes,
            .first_message_number = first.header.message_number,
            .sequences =
                {
                    .first = sequence,
                    .last = first_sequence_.advanced(
                        static_cast<std::uint32_t>(final_offset)),
                },
        };
    }
    if (range_drop_request_size_ != 0U) {
        const SequenceRange range =
            range_drop_request_queue_[range_drop_request_head_];
        range_drop_request_head_ =
            (range_drop_request_head_ + 1U) % range_drop_request_queue_.size();
        --range_drop_request_size_;
        return SendDropResult {
            .packets =
                static_cast<std::size_t>(range.last.distance_from(range.first))
                + 1U,
            .first_message_number = 0U,
            .sequences = range,
        };
    }
    return std::nullopt;
}

bool SendBuffer::has_pending_drop_request() noexcept
{
    if (drop_request_size_ != 0U) {
        compact_drop_request_queue();
    }
    return drop_request_size_ != 0U || range_drop_request_size_ != 0U;
}

bool SendBuffer::queue_range_drop_requests(
    std::span<const SequenceRange> ranges) noexcept
{
    std::size_t additional = 0;
    for (std::size_t incoming_index = 0; incoming_index < ranges.size();
        ++incoming_index) {
        const SequenceRange range = ranges[incoming_index];
        if (range.last.distance_from(range.first) < 0) {
            return false;
        }
        bool duplicate = false;
        for (std::size_t queued_index = 0;
            queued_index < range_drop_request_size_; ++queued_index) {
            const auto& queued =
                range_drop_request_queue_[(range_drop_request_head_
                                              + queued_index)
                    % range_drop_request_queue_.size()];
            if (queued.first == range.first && queued.last == range.last) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            for (std::size_t earlier = 0; earlier < incoming_index; ++earlier) {
                if (ranges[earlier].first == range.first
                    && ranges[earlier].last == range.last) {
                    duplicate = true;
                    break;
                }
            }
        }
        if (!duplicate) {
            ++additional;
        }
    }
    if (additional
        > range_drop_request_queue_.size() - range_drop_request_size_) {
        return false;
    }

    for (const SequenceRange range : ranges) {
        bool duplicate = false;
        for (std::size_t queued_index = 0;
            queued_index < range_drop_request_size_; ++queued_index) {
            const auto& queued =
                range_drop_request_queue_[(range_drop_request_head_
                                              + queued_index)
                    % range_drop_request_queue_.size()];
            if (queued.first == range.first && queued.last == range.last) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue;
        }
        const std::size_t tail =
            (range_drop_request_head_ + range_drop_request_size_)
            % range_drop_request_queue_.size();
        range_drop_request_queue_[tail] = range;
        ++range_drop_request_size_;
    }
    return true;
}

SendDropResult SendBuffer::drop_messages_older_than(
    std::uint64_t cutoff_microseconds) noexcept
{
    SendDropResult result;
    if (sequence_span_ == 0U) {
        return result;
    }
    const auto& first = slots_[head_];
    if (first.enqueue_microseconds > cutoff_microseconds) {
        return result;
    }
    result.first_message_number = first.header.message_number;
    result.sequences.first = first_sequence_;

    while (result.packets < sequence_span_) {
        auto& slot = slots_[(head_ + result.packets) % capacity()];
        if (!slot.occupied
            || slot.enqueue_microseconds > cutoff_microseconds) {
            break;
        }
        result.bytes += slot.plaintext_size;
        discard_slot(slot);
        ++result.packets;
    }
    if (result.packets == 0U) {
        return result;
    }
    result.sequences.last = first_sequence_.advanced(
        static_cast<std::uint32_t>(result.packets - 1U));
    head_ = (head_ + result.packets) % capacity();
    sequence_span_ -= result.packets;
    first_sequence_ = result.sequences.last.next();
    refresh_first_buffered_enqueue_time();
    return result;
}

SendDropResult SendBuffer::drop_expired_message(
    std::uint64_t now_microseconds) noexcept
{
    SendDropResult result;
    if (expiring_packet_count_ == 0U) {
        return result;
    }
    for (std::size_t offset = 0; offset < sequence_span_; ++offset) {
        auto& first = slots_[(head_ + offset) % capacity()];
        if (!first.occupied
            || first.expiration_microseconds == 0U
            || now_microseconds <= first.expiration_microseconds) {
            continue;
        }

        result.first_message_number = first.header.message_number;
        result.sequences.first = first_sequence_.advanced(
            static_cast<std::uint32_t>(offset));
        std::size_t message_offset = offset;
        while (message_offset < sequence_span_) {
            auto& slot =
                slots_[(head_ + message_offset) % capacity()];
            if (!slot.occupied
                || slot.header.message_number
                    != result.first_message_number) {
                break;
            }
            result.bytes += slot.plaintext_size;
            discard_slot(slot, true);
            ++result.packets;
            ++message_offset;
        }
        result.sequences.last = result.sequences.first.advanced(
            static_cast<std::uint32_t>(result.packets - 1U));
        refresh_buffered_enqueue_time_bounds();
        return result;
    }
    return result;
}

std::size_t SendBuffer::buffered_payload_bytes() const noexcept
{
    return buffered_plaintext_bytes_;
}

std::uint64_t SendBuffer::buffered_span_milliseconds() const noexcept
{
    if (occupied_count_ == 0U) {
        return 0U;
    }
    return last_buffered_enqueue_microseconds_
            >= first_buffered_enqueue_microseconds_
        ? (last_buffered_enqueue_microseconds_
              - first_buffered_enqueue_microseconds_)
                / 1'000U
            + 1U
        : 1U;
}

} // namespace robotweax::srt
