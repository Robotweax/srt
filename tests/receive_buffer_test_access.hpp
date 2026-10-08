#pragma once

#include "robotweax/srt/receive_buffer.hpp"

namespace robotweax::srt::detail {
struct ReceiveBufferTestAccess {
    static std::optional<std::size_t> next(const ReceiveBuffer& buffer,
        std::size_t begin, std::size_t& word_reads) noexcept
    {
        return buffer.next_occupied_offset(begin, &word_reads);
    }
    static std::size_t levels(const ReceiveBuffer& buffer) noexcept
    {
        return buffer.occupied_levels_;
    }
    static std::size_t index_bytes(const ReceiveBuffer& buffer) noexcept
    {
        return buffer.occupied_index_.size() * sizeof(std::uint64_t);
    }

    // Independent, uncached reconstruction over the logical receive window.
    // This deliberately ignores the optimized bounds and cached answers.
    static std::optional<BufferedMessageInfo> reference(
        const ReceiveBuffer& buffer, bool messages) noexcept
    {
        for (std::size_t offset = 0; offset < buffer.capacity(); ++offset) {
            const auto sequence = buffer.first_stored_sequence().advanced(
                static_cast<std::uint32_t>(offset));
            const auto* first = buffer.find(sequence);
            if (first == nullptr)
                continue;
            if (!messages || first->header.boundary == MessageBoundary::solo)
                return BufferedMessageInfo {
                    sequence, sequence, first->header.timestamp};
            if (first->header.boundary != MessageBoundary::first)
                continue;
            for (auto next = sequence.next();
                next.distance_from(buffer.first_stored_sequence())
                < static_cast<std::int32_t>(buffer.capacity());
                next = next.next()) {
                const auto* fragment = buffer.find(next);
                if (fragment == nullptr
                    || fragment->header.message_number
                        != first->header.message_number)
                    break;
                if (fragment->header.boundary == MessageBoundary::last)
                    return BufferedMessageInfo {
                        sequence, next, first->header.timestamp};
                if (fragment->header.boundary != MessageBoundary::subsequent)
                    break;
            }
        }
        return std::nullopt;
    }

    static std::optional<BufferedMessageInfo> message(
        const ReceiveBuffer& buffer, std::size_t& inspected) noexcept
    {
        return buffer.query_first_complete_message(&inspected);
    }
    static std::optional<BufferedMessageInfo> packet(
        const ReceiveBuffer& buffer, std::size_t& inspected) noexcept
    {
        return buffer.query_first_buffered_packet(&inspected);
    }
};
}
