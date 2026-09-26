// SPDX-License-Identifier: MIT
#pragma once

#include "robotweax/srt/packet.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace robotweax::srt::detail {

// Fixed-capacity backing store. Reserve all payload bytes at construction, but
// initialize only bytes supplied by the caller. Reuse payload indices independently
// of the sequence ring so low occupancy does not touch the entire arena over time.
class PayloadPool {
public:
    explicit PayloadPool(std::size_t capacity)
        : payload_(new std::byte[checked_bytes(capacity)])
        , sizes_(capacity)
        , free_indices_(capacity)
    {
    }

    PayloadPool(const PayloadPool& other)
        : PayloadPool(other.sizes_.size())
    {
        sizes_ = other.sizes_;
        free_indices_ = other.free_indices_;
        next_unused_ = other.next_unused_;
        free_count_ = other.free_count_;
        // Released and never-used bytes are deliberately not read or copied.
        for (std::size_t index = 0; index < next_unused_; ++index) {
            std::copy_n(other.data(index), sizes_[index], data(index));
        }
    }
    PayloadPool(PayloadPool&&) noexcept = default;
    PayloadPool& operator=(PayloadPool&&) noexcept = default;
    PayloadPool& operator=(const PayloadPool& other)
    {
        if (this != &other) {
            PayloadPool copy(other);
            *this = std::move(copy);
        }
        return *this;
    }

    [[nodiscard]] std::uint32_t acquire(
        std::span<const std::byte> bytes) noexcept
    {
        assert(free_count_ != 0U || next_unused_ < sizes_.size());
        const auto index =
            free_count_ != 0U ? free_indices_[--free_count_] : next_unused_++;
        store(index, bytes);
        return index;
    }

    void release(std::uint32_t index) noexcept
    {
        assert(index < next_unused_ && free_count_ < free_indices_.size());
        sizes_[index] = 0U;
        free_indices_[free_count_++] = index;
    }

    void store(std::uint32_t index, std::span<const std::byte> bytes) noexcept
    {
        assert(
            index < next_unused_ && bytes.size() <= maximum_data_payload_size);
        std::copy(bytes.begin(), bytes.end(), data(index));
        sizes_[index] = static_cast<std::uint16_t>(bytes.size());
    }

    [[nodiscard]] std::span<const std::byte> get(
        std::uint32_t index) const noexcept
    {
        assert(index < next_unused_);
        return {data(index), sizes_[index]};
    }

private:
    static std::size_t checked_bytes(std::size_t capacity)
    {
        if (capacity == 0U || capacity >= SequenceNumber::half_range
            || capacity > std::numeric_limits<std::size_t>::max()
                    / maximum_data_payload_size) {
            throw std::invalid_argument("invalid payload-pool capacity");
        }
        return capacity * maximum_data_payload_size;
    }
    std::byte* data(std::size_t index) noexcept
    {
        return payload_.get() + index * maximum_data_payload_size;
    }
    const std::byte* data(std::size_t index) const noexcept
    {
        return payload_.get() + index * maximum_data_payload_size;
    }

    std::unique_ptr<std::byte[]> payload_;
    std::vector<std::uint16_t> sizes_;
    std::vector<std::uint32_t> free_indices_;
    std::uint32_t next_unused_ = 0;
    std::size_t free_count_ = 0;
};

} // namespace robotweax::srt::detail
