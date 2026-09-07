#include "compat/group_replay_buffer.hpp"

#include <algorithm>
#include <new>
#include <utility>

namespace robotweax::srt::compat {

GroupReplayBuffer::GroupReplayBuffer(
    std::size_t byte_capacity,
    std::size_t message_capacity) noexcept
    : byte_capacity_(byte_capacity)
    , message_capacity_(message_capacity)
{
}

bool GroupReplayBuffer::ensure_storage() noexcept
{
    if (bytes_ != nullptr && entries_ != nullptr) {
        return true;
    }
    if (byte_capacity_ == 0U || message_capacity_ == 0U) {
        return false;
    }
    try {
        auto bytes = std::make_unique_for_overwrite<std::byte[]>(
            byte_capacity_);
        auto entries = std::make_unique<Entry[]>(message_capacity_);
        bytes_ = std::move(bytes);
        entries_ = std::move(entries);
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    } catch (...) {
        return false;
    }
}

const GroupReplayBuffer::Entry& GroupReplayBuffer::entry_at(
    std::size_t index) const noexcept
{
    return entries_[(entry_head_ + index) % message_capacity_];
}

void GroupReplayBuffer::discard_front() noexcept
{
    if (entry_size_ == 0U) {
        return;
    }
    const Entry& entry = entry_at(0U);
    byte_head_ =
        (entry.payload_offset + entry.payload_size) % byte_capacity_;
    byte_size_ -= entry.payload_size;
    entry_head_ = (entry_head_ + 1U) % message_capacity_;
    --entry_size_;
    if (entry_size_ == 0U) {
        byte_head_ = 0U;
        byte_size_ = 0U;
        entry_head_ = 0U;
    }
}

GroupReplayBuffer::AppendResult GroupReplayBuffer::append(
    std::span<const std::byte> payload,
    const GroupReplayMessage& message) noexcept
{
    if (payload.empty() || payload.size() > byte_capacity_
        || message.message_number == 0U
        || message.next_sequence.distance_from(
               message.first_sequence)
            <= 0) {
        return AppendResult::invalid_message;
    }
    if (!ensure_storage()) {
        return AppendResult::allocation_failure;
    }
    while (entry_size_ == message_capacity_
        || byte_size_ + payload.size() > byte_capacity_) {
        discard_front();
    }

    const std::size_t payload_offset =
        (byte_head_ + byte_size_) % byte_capacity_;
    const std::size_t first_part = std::min(
        payload.size(), byte_capacity_ - payload_offset);
    std::copy_n(payload.begin(), first_part, bytes_.get() + payload_offset);
    if (first_part != payload.size()) {
        std::copy(payload.begin() + static_cast<std::ptrdiff_t>(first_part),
            payload.end(), bytes_.get());
    }

    const std::size_t tail =
        (entry_head_ + entry_size_) % message_capacity_;
    entries_[tail] = {
        .message = message,
        .payload_offset = payload_offset,
        .payload_size = payload.size(),
    };
    byte_size_ += payload.size();
    ++entry_size_;
    return AppendResult::success;
}

void GroupReplayBuffer::acknowledge_before(
    SequenceNumber sequence) noexcept
{
    while (entry_size_ != 0U
        && sequence.distance_from(
               entry_at(0U).message.next_sequence)
            >= 0) {
        discard_front();
    }
}

void GroupReplayBuffer::clear() noexcept
{
    byte_head_ = 0U;
    byte_size_ = 0U;
    entry_head_ = 0U;
    entry_size_ = 0U;
}

void GroupReplayBuffer::release_storage() noexcept
{
    clear();
    bytes_.reset();
    entries_.reset();
}

bool GroupReplayBuffer::copy_starting_at(
    SequenceNumber sequence,
    std::span<std::byte> destination,
    GroupReplayMessage& message,
    std::size_t& payload_size) const noexcept
{
    Cursor cursor;
    return begin_at(sequence, cursor)
        && copy_next(cursor, destination, message, payload_size);
}

bool GroupReplayBuffer::begin_at(
    SequenceNumber sequence, Cursor& cursor) const noexcept
{
    for (std::size_t index = 0; index < entry_size_; ++index) {
        const Entry& entry = entry_at(index);
        if (entry.message.first_sequence != sequence) {
            continue;
        }
        cursor.index_ = index;
        cursor.remaining_ = entry_size_ - index;
        return true;
    }
    cursor = Cursor{};
    return false;
}

bool GroupReplayBuffer::copy_next(
    Cursor& cursor,
    std::span<std::byte> destination,
    GroupReplayMessage& message,
    std::size_t& payload_size) const noexcept
{
    if (cursor.remaining_ == 0U || cursor.index_ >= entry_size_) {
        return false;
    }
    const Entry& entry = entry_at(cursor.index_);
    if (!copy_entry(entry, destination, message, payload_size)) {
        return false;
    }
    ++cursor.index_;
    --cursor.remaining_;
    return true;
}

bool GroupReplayBuffer::copy_entry(
    const Entry& entry,
    std::span<std::byte> destination,
    GroupReplayMessage& message,
    std::size_t& payload_size) const noexcept
{
    if (destination.size() < entry.payload_size) {
        return false;
    }
    const std::size_t first_part = std::min(
        entry.payload_size, byte_capacity_ - entry.payload_offset);
    std::copy_n(bytes_.get() + entry.payload_offset,
        first_part, destination.begin());
    if (first_part != entry.payload_size) {
        std::copy_n(bytes_.get(), entry.payload_size - first_part,
            destination.begin()
                + static_cast<std::ptrdiff_t>(first_part));
    }
    message = entry.message;
    payload_size = entry.payload_size;
    return true;
}

std::optional<SequenceNumber>
GroupReplayBuffer::first_sequence() const noexcept
{
    if (entry_size_ == 0U) {
        return std::nullopt;
    }
    return entry_at(0U).message.first_sequence;
}

std::optional<SequenceNumber>
GroupReplayBuffer::next_sequence() const noexcept
{
    if (entry_size_ == 0U) {
        return std::nullopt;
    }
    return entry_at(entry_size_ - 1U).message.next_sequence;
}

} // namespace robotweax::srt::compat
