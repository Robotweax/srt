#pragma once

#include "robotweax/srt/packet.hpp"
#include "robotweax/srt/reliability.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace robotweax::srt::detail {

inline constexpr std::size_t maximum_range_drop_terms =
    maximum_data_payload_size / sizeof(std::uint32_t);

// Sequence-only replies carry coverage, not individual message identities.
// Store the exact union in canonical wire order. A wrapping range uses two
// intervals; joining the end intervals on emission preserves its wire form.
// Packet-sized bounds make admission independent of the sender window size.
class RangeDropQueue {
public:
    static constexpr std::size_t capacity = maximum_range_drop_terms + 1U;

    RangeDropQueue()
        : ranges_(capacity)
    {
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return size_ == 0U;
    }
    [[nodiscard]] std::size_t size() const noexcept
    {
        return size_;
    }

    [[nodiscard]] bool queue(std::span<const SequenceRange> incoming,
        std::size_t* work = nullptr) noexcept
    {
        if (incoming.size() > maximum_range_drop_terms
            || ranges_.size() != capacity)
            return false;

        if (incoming.empty())
            return true;

        std::array<SequenceRange, 2U * maximum_range_drop_terms> split {};
        std::size_t split_size = 0;
        for (const auto range : incoming) {
            count(work);
            if (range.last.distance_from(range.first) < 0)
                return false;
            if (range.first.value() <= range.last.value())
                split[split_size++] = range;
            else {
                split[split_size++] = {SequenceNumber {0}, range.last};
                split[split_size++] = {
                    range.first, SequenceNumber {SequenceNumber::mask}};
            }
        }
        std::sort(split.begin(), split.begin() + split_size,
            [work](const auto& a, const auto& b) {
                count(work);
                return a.first.value() < b.first.value();
            });

        // Preview the whole union before touching the queue. Even a bridge
        // arriving late in a full packet may reduce an otherwise full queue.
        std::array<SequenceRange, capacity + 2U * maximum_range_drop_terms>
            merged {};
        std::size_t kept = 0;
        std::size_t old_index = 0;
        std::size_t new_index = 0;
        while (old_index < size_ || new_index < split_size) {
            count(work);
            const bool use_old = new_index == split_size
                || (old_index < size_
                    && at(old_index).first.value()
                        <= split[new_index].first.value());
            const auto range = use_old ? at(old_index++) : split[new_index++];
            if (kept != 0U
                && range.first.value() <= merged[kept - 1U].last.value() + 1U) {
                auto& last = merged[kept - 1U].last;
                if (range.last.value() > last.value())
                    last = range.last;
            } else
                merged[kept++] = range;
        }
        if (kept > capacity)
            return false;
        for (std::size_t i = 0; i < kept; ++i) {
            count(work);
            ranges_[i] = merged[i];
        }
        head_ = 0;
        size_ = kept;
        return true;
    }

    [[nodiscard]] std::optional<SequenceRange> next() noexcept
    {
        if (empty())
            return std::nullopt;
        if (size_ > 1U && at(0).first == SequenceNumber {0}
            && at(size_ - 1U).last == SequenceNumber {SequenceNumber::mask}) {
            const SequenceRange wrap {at(size_ - 1U).first, at(0).last};
            if (wrap.last.distance_from(wrap.first) >= 0) {
                head_ = (head_ + 1U) % capacity;
                size_ -= 2U;
                return wrap;
            }
        }
        auto& first = ranges_[head_];
        // The union of individually valid ranges may cover half the wire
        // space or more. Emit only unambiguous ranges without enumerating
        // their sequence positions.
        if (first.last.value() - first.first.value()
            >= SequenceNumber::half_range) {
            const SequenceRange result {first.first,
                first.first.advanced(SequenceNumber::half_range - 1U)};
            first.first = result.last.next();
            return result;
        }
        const auto result = first;
        head_ = (head_ + 1U) % capacity;
        --size_;
        return result;
    }

private:
    static void count(std::size_t* work) noexcept
    {
        if (work != nullptr)
            ++*work;
    }
    [[nodiscard]] const SequenceRange& at(std::size_t index) const noexcept
    {
        return ranges_[(head_ + index) % capacity];
    }

    std::vector<SequenceRange> ranges_;
    std::size_t head_ = 0;
    std::size_t size_ = 0;
};

} // namespace robotweax::srt::detail
