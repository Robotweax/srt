#pragma once

#include "robotweax/srt/send_buffer.hpp"
#include "robotweax/srt/sequence.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace robotweax::srt::compat {

struct GroupReplayMessage {
    SequenceNumber first_sequence{};
    SequenceNumber next_sequence{};
    std::uint32_t message_number = 0;
    std::int64_t source_time_microseconds = 0;
    std::int32_t ttl_milliseconds = -1;
    bool in_order = true;
};

class GroupReplayBuffer {
public:
    static constexpr std::size_t default_message_capacity = 8'192;
    static constexpr std::size_t default_byte_capacity =
        default_message_capacity * maximum_data_payload_size;

    enum class AppendResult {
        success,
        invalid_message,
        allocation_failure,
    };

    class Cursor {
    public:
        Cursor() noexcept = default;

    private:
        friend class GroupReplayBuffer;
        std::size_t index_ = 0;
        std::size_t remaining_ = 0;
    };

    explicit GroupReplayBuffer(
        std::size_t byte_capacity = default_byte_capacity,
        std::size_t message_capacity = default_message_capacity) noexcept;

    [[nodiscard]] AppendResult append(
        std::span<const std::byte> payload,
        const GroupReplayMessage& message) noexcept;
    void acknowledge_before(SequenceNumber sequence) noexcept;
    void clear() noexcept;
    void release_storage() noexcept;

    [[nodiscard]] bool copy_starting_at(
        SequenceNumber sequence,
        std::span<std::byte> destination,
        GroupReplayMessage& message,
        std::size_t& payload_size) const noexcept;
    [[nodiscard]] bool begin_at(
        SequenceNumber sequence, Cursor& cursor) const noexcept;
    [[nodiscard]] bool copy_next(
        Cursor& cursor,
        std::span<std::byte> destination,
        GroupReplayMessage& message,
        std::size_t& payload_size) const noexcept;

    [[nodiscard]] std::optional<SequenceNumber>
    first_sequence() const noexcept;
    [[nodiscard]] std::optional<SequenceNumber>
    next_sequence() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept
    {
        return entry_size_;
    }
    [[nodiscard]] std::size_t buffered_bytes() const noexcept
    {
        return byte_size_;
    }
    [[nodiscard]] std::size_t byte_capacity() const noexcept
    {
        return byte_capacity_;
    }
    [[nodiscard]] std::size_t message_capacity() const noexcept
    {
        return message_capacity_;
    }
    [[nodiscard]] bool storage_allocated() const noexcept
    {
        return bytes_ != nullptr && entries_ != nullptr;
    }

private:
    struct Entry {
        GroupReplayMessage message{};
        std::size_t payload_offset = 0;
        std::size_t payload_size = 0;
    };

    [[nodiscard]] bool ensure_storage() noexcept;
    void discard_front() noexcept;
    [[nodiscard]] const Entry& entry_at(std::size_t index) const noexcept;
    [[nodiscard]] bool copy_entry(
        const Entry& entry,
        std::span<std::byte> destination,
        GroupReplayMessage& message,
        std::size_t& payload_size) const noexcept;

    std::size_t byte_capacity_ = 0;
    std::size_t message_capacity_ = 0;
    std::unique_ptr<std::byte[]> bytes_;
    std::unique_ptr<Entry[]> entries_;
    std::size_t byte_head_ = 0;
    std::size_t byte_size_ = 0;
    std::size_t entry_head_ = 0;
    std::size_t entry_size_ = 0;
};

} // namespace robotweax::srt::compat
