#include "robotweax/srt/reliability.hpp"

#include <algorithm>
#include <stdexcept>

namespace robotweax::srt {

ReceiveWindow::ReceiveWindow(SequenceNumber first_expected, std::size_t capacity)
    : first_expected_(first_expected), slots_(capacity, 0)
{
    if (capacity == 0 || capacity >= SequenceNumber::half_range) {
        throw std::invalid_argument("receive-window capacity is outside sequence space");
    }
}

ReceiveDecision ReceiveWindow::observe(SequenceNumber sequence) noexcept
{
    const std::int32_t signed_offset = sequence.distance_from(first_expected_);
    if (signed_offset < 0) {
        return {.status = ReceiveStatus::older_than_window};
    }
    const auto offset = static_cast<std::uint32_t>(signed_offset);
    if (offset >= slots_.size()) {
        return {.status = ReceiveStatus::beyond_window};
    }

    const std::size_t slot = (head_ + offset) % slots_.size();
    if (slots_[slot] != 0U) {
        return {.status = ReceiveStatus::duplicate};
    }
    slots_[slot] = 1U;

    ReceiveDecision decision;
    decision.status = offset == 0U
        ? ReceiveStatus::accepted_in_order
        : ReceiveStatus::accepted_out_of_order;
    if (offset > 0U) {
        decision.gap_before_packet = SequenceRange{
            .first = first_expected_,
            .last = sequence.advanced(SequenceNumber::mask),
        };
        return decision;
    }

    while (slots_[head_] != 0U) {
        slots_[head_] = 0U;
        head_ = (head_ + 1U) % slots_.size();
        first_expected_ = first_expected_.next();
        ++decision.contiguous_advance;
    }
    return decision;
}

ReceiveLossList::ReceiveLossList(std::size_t capacity)
    : entries_(capacity)
{
    if (capacity == 0U || capacity >= SequenceNumber::half_range) {
        throw std::invalid_argument(
            "receive-loss capacity is outside sequence space");
    }
}

void ReceiveLossList::note_added(const Entry& entry) noexcept
{
    if (entry.fresh) {
        ++fresh_count_;
    }
    if (entry.initial_report_pending || entry.periodic_report_pending) {
        ++pending_count_;
    }
}

void ReceiveLossList::note_removed(const Entry& entry) noexcept
{
    if (entry.fresh) {
        --fresh_count_;
    }
    if (entry.initial_report_pending || entry.periodic_report_pending) {
        --pending_count_;
    }
}

std::size_t ReceiveLossList::lower_bound(SequenceNumber sequence) const noexcept
{
    std::size_t low = 0;
    std::size_t high = size_;
    while (low < high) {
        const std::size_t middle = low + (high - low) / 2U;
        if (sequence.distance_from(entries_[middle].range.last) > 0) {
            low = middle + 1U;
        } else {
            high = middle;
        }
    }
    return low;
}

bool ReceiveLossList::add(
    SequenceRange range, std::uint32_t initial_ttl) noexcept
{
    return add_all(std::span<const SequenceRange> {&range, 1U}, initial_ttl);
}

bool ReceiveLossList::add_all(
    std::span<const SequenceRange> ranges, std::uint32_t initial_ttl) noexcept
{
    if (!can_append(ranges)) {
        return false;
    }

    for (const auto& range : ranges) {
        entries_[size_] = {
            .range = range,
            .ttl = initial_ttl,
            .fresh = initial_ttl != 0U,
            .initial_report_pending = initial_ttl == 0U,
        };
        note_added(entries_[size_]);
        ++size_;
    }
    return true;
}

bool ReceiveLossList::insert_sorted(
    SequenceRange range, std::uint32_t initial_ttl) noexcept
{
    if (range.last.distance_from(range.first) < 0) {
        return false;
    }
    std::size_t position = 0;
    while (position < size_) {
        const SequenceRange& entry = entries_[position].range;
        if (entry.first.distance_from(range.first) >= 0) {
            break;
        }
        ++position;
    }
    // Skip fully covered ranges, but reject partial overlaps without mutation.
    if (position > 0U) {
        const SequenceRange& previous = entries_[position - 1U].range;
        if (range.first.distance_from(previous.last) <= 0) {
            return range.last.distance_from(previous.last) <= 0;
        }
    }
    if (position < size_) {
        const SequenceRange& following = entries_[position].range;
        if (following.first.distance_from(range.last) <= 0) {
            return range.first == following.first
                && range.last.distance_from(following.last) <= 0;
        }
    }
    if (size_ == entries_.size()) {
        return false;
    }
    for (std::size_t source = size_; source > position; --source) {
        entries_[source] = entries_[source - 1U];
    }
    entries_[position] = {
        .range = range,
        .ttl = initial_ttl,
        .fresh = initial_ttl != 0U,
        .initial_report_pending = initial_ttl == 0U,
    };
    note_added(entries_[position]);
    ++size_;
    return true;
}

namespace {

[[nodiscard]] bool ranges_overlap(
    const SequenceRange& left, const SequenceRange& right) noexcept
{
    const bool left_starts_by_right_end =
        left.first.distance_from(right.last) <= 0;
    const bool right_starts_by_left_end =
        right.first.distance_from(left.last) <= 0;
    return left_starts_by_right_end && right_starts_by_left_end;
}

} // namespace

bool ReceiveLossList::can_insert_all_sorted(
    std::span<const SequenceRange> ranges) const noexcept
{
    if (ranges.size() > entries_.size() - size_) {
        return false;
    }
    for (std::size_t index = 0; index < ranges.size(); ++index) {
        const SequenceRange& range = ranges[index];
        if (range.last.distance_from(range.first) < 0) {
            return false;
        }
        for (std::size_t entry = 0; entry < size_; ++entry) {
            if (ranges_overlap(range, entries_[entry].range)) {
                return false;
            }
        }
        for (std::size_t earlier = 0; earlier < index; ++earlier) {
            if (ranges_overlap(range, ranges[earlier])) {
                return false;
            }
        }
    }
    return true;
}

bool ReceiveLossList::can_append(
    std::span<const SequenceRange> ranges) const noexcept
{
    if (ranges.size() > entries_.size() - size_) {
        return false;
    }

    bool has_previous = size_ != 0U;
    SequenceNumber previous_last {};
    if (has_previous) {
        previous_last = entries_[size_ - 1U].range.last;
    }

    for (const auto& range : ranges) {
        if (range.last.distance_from(range.first) < 0
            || (has_previous
                && range.first.distance_from(previous_last) <= 0)) {
            return false;
        }
        previous_last = range.last;
        has_previous = true;
    }
    return true;
}

void ReceiveLossList::erase(std::size_t index) noexcept
{
    note_removed(entries_[index]);
    for (std::size_t source = index + 1U; source < size_; ++source) {
        entries_[source - 1U] = entries_[source];
    }
    --size_;
    entries_[size_] = {};
}

ReceiveLossRemoval ReceiveLossList::remove(
    SequenceNumber sequence) noexcept
{
    // Sorted, disjoint entries: only the first entry ending at or after the
    // sequence can contain it.
    const std::size_t index = lower_bound(sequence);
    if (index < size_) {
        auto& entry = entries_[index];
        const std::int32_t from_first =
            sequence.distance_from(entry.range.first);
        const std::int32_t from_last =
            sequence.distance_from(entry.range.last);
        if (from_first < 0 || from_last > 0) {
            return {};
        }

        const ReceiveLossRemoval result{
            .removed = true,
            .remaining_ttl = entry.fresh ? entry.ttl : 0U,
        };
        if (from_first == 0 && from_last == 0) {
            erase(index);
            return result;
        }
        if (from_first == 0) {
            entry.range.first = sequence.next();
            return result;
        }
        if (from_last == 0) {
            entry.range.last =
                sequence.advanced(SequenceNumber::mask);
            return result;
        }

        if (size_ == entries_.size()) {
            return {};
        }
        const Entry upper = entry;
        for (std::size_t source = size_; source > index + 1U; --source) {
            entries_[source] = entries_[source - 1U];
        }
        entry.range.last =
            sequence.advanced(SequenceNumber::mask);
        entries_[index + 1U] = upper;
        entries_[index + 1U].range.first = sequence.next();
        note_added(entries_[index + 1U]);
        ++size_;
        return result;
    }
    return {};
}

void ReceiveLossList::remove_through(
    SequenceNumber last) noexcept
{
    std::size_t removed = 0;
    while (removed < size_) {
        auto& entry = entries_[removed];
        if (last.distance_from(entry.range.first) < 0) {
            break;
        }
        if (last.distance_from(entry.range.last) < 0) {
            entry.range.first = last.next();
            break;
        }
        ++removed;
    }
    if (removed == 0U) {
        return;
    }
    for (std::size_t index = 0; index < removed; ++index) {
        note_removed(entries_[index]);
    }

    // Compact the surviving suffix once. Erasing each covered range in turn
    // repeatedly moves the same survivors and is quadratic for fragmented loss.
    const std::size_t previous_size = size_;
    size_ -= removed;
    for (std::size_t index = 0; index < size_; ++index) {
        entries_[index] = entries_[index + removed];
    }
    for (std::size_t index = size_; index < previous_size; ++index) {
        entries_[index] = {};
    }
}

void ReceiveLossList::remove_range(SequenceRange range) noexcept
{
    if (range.last.distance_from(range.first) < 0) {
        return;
    }
    std::size_t index = 0;
    while (index < size_) {
        auto& entry = entries_[index];
        if (range.last.distance_from(entry.range.first) < 0) {
            // Entries are sorted; nothing later can overlap.
            return;
        }
        if (range.first.distance_from(entry.range.last) > 0) {
            ++index;
            continue;
        }
        const bool covers_head =
            range.first.distance_from(entry.range.first) <= 0;
        const bool covers_tail =
            range.last.distance_from(entry.range.last) >= 0;
        if (covers_head && covers_tail) {
            erase(index);
            continue;
        }
        if (covers_head) {
            entry.range.first = range.last.next();
            ++index;
            continue;
        }
        if (covers_tail) {
            entry.range.last = range.first.advanced(SequenceNumber::mask);
            ++index;
            continue;
        }
        // The range lies strictly inside this entry: split it.
        if (size_ == entries_.size()) {
            return;
        }
        const Entry upper = entry;
        for (std::size_t source = size_; source > index + 1U; --source) {
            entries_[source] = entries_[source - 1U];
        }
        entry.range.last = range.first.advanced(SequenceNumber::mask);
        entries_[index + 1U] = upper;
        entries_[index + 1U].range.first = range.last.next();
        note_added(entries_[index + 1U]);
        ++size_;
        return;
    }
}

void ReceiveLossList::age_fresh() noexcept
{
    if (fresh_count_ == 0U) {
        return;
    }
    for (std::size_t index = 0; index < size_; ++index) {
        auto& entry = entries_[index];
        if (!entry.fresh) {
            continue;
        }
        if (entry.ttl == 0U) {
            entry.fresh = false;
            --fresh_count_;
            if (!entry.initial_report_pending
                && !entry.periodic_report_pending) {
                ++pending_count_;
            }
            entry.initial_report_pending = true;
        } else {
            --entry.ttl;
        }
    }
}

void ReceiveLossList::mark_periodic_reports() noexcept
{
    for (std::size_t index = 0; index < size_; ++index) {
        entries_[index].periodic_report_pending = true;
    }
    pending_count_ = size_;
}

std::optional<SequenceRange>
ReceiveLossList::take_pending_report() noexcept
{
    SequenceRange report;
    return take_pending_reports({&report, 1U}) == 1U
        ? std::optional<SequenceRange>{report}
        : std::nullopt;
}

std::size_t ReceiveLossList::take_pending_reports(
    std::span<SequenceRange> destination) noexcept
{
    std::size_t written = 0;
    for (std::size_t index = 0;
        index < size_ && pending_count_ != 0U && written < destination.size();
        ++index) {
        auto& entry = entries_[index];
        if (!entry.initial_report_pending
            && !entry.periodic_report_pending) {
            continue;
        }
        destination[written++] = entry.range;
        entry.initial_report_pending = false;
        entry.periodic_report_pending = false;
        --pending_count_;
    }
    return written;
}

bool ReceiveLossList::has_pending_report() const noexcept
{
    return pending_count_ != 0U;
}

} // namespace robotweax::srt
