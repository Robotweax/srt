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

bool ReceiveLossList::add(SequenceRange range, std::uint32_t initial_ttl,
    std::uint64_t fresh_deadline_microseconds) noexcept
{
    return add_all(std::span<const SequenceRange> {&range, 1U}, initial_ttl,
        fresh_deadline_microseconds);
}

bool ReceiveLossList::add_all(std::span<const SequenceRange> ranges,
    std::uint32_t initial_ttl,
    std::uint64_t fresh_deadline_microseconds) noexcept
{
    if (!can_append(ranges)) {
        return false;
    }

    for (const auto& range : ranges) {
        entries_[size_] = {
            .range = range,
            .fresh_deadline_microseconds = fresh_deadline_microseconds,
            .ttl = initial_ttl,
            .fresh = initial_ttl != 0U,
            .initial_report_pending = initial_ttl == 0U,
        };
        ++size_;
        if (initial_ttl != 0U && fresh_deadline_microseconds != 0U
            && (next_fresh_deadline_microseconds_ == 0U
                || fresh_deadline_microseconds
                    < next_fresh_deadline_microseconds_)) {
            next_fresh_deadline_microseconds_ = fresh_deadline_microseconds;
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
    for (std::size_t source = index + 1U; source < size_; ++source) {
        entries_[source - 1U] = entries_[source];
    }
    --size_;
    entries_[size_] = {};
}

ReceiveLossRemoval ReceiveLossList::remove(
    SequenceNumber sequence) noexcept
{
    for (std::size_t index = 0; index < size_; ++index) {
        auto& entry = entries_[index];
        const std::int32_t from_first =
            sequence.distance_from(entry.range.first);
        const std::int32_t from_last =
            sequence.distance_from(entry.range.last);
        if (from_first < 0 || from_last > 0) {
            continue;
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
        ++size_;
        return result;
    }
    return {};
}

void ReceiveLossList::remove_through(
    SequenceNumber last) noexcept
{
    std::size_t index = 0;
    while (index < size_) {
        auto& entry = entries_[index];
        if (last.distance_from(entry.range.first) < 0) {
            return;
        }
        if (last.distance_from(entry.range.last) >= 0) {
            erase(index);
            continue;
        }
        entry.range.first = last.next();
        return;
    }
}

void ReceiveLossList::age_fresh() noexcept
{
    for (std::size_t index = 0; index < size_; ++index) {
        auto& entry = entries_[index];
        if (entry.fresh && entry.ttl == 0U) {
            entry.fresh = false;
            entry.initial_report_pending = true;
        }
    }
    for (std::size_t index = 0; index < size_; ++index) {
        auto& entry = entries_[index];
        if (entry.fresh && entry.ttl != 0U) {
            --entry.ttl;
        }
    }
}

void ReceiveLossList::expire_fresh(std::uint64_t now_microseconds) noexcept
{
    if (next_fresh_deadline_microseconds_ == 0U
        || now_microseconds < next_fresh_deadline_microseconds_) {
        return;
    }
    next_fresh_deadline_microseconds_ = 0U;
    for (std::size_t index = 0; index < size_; ++index) {
        auto& entry = entries_[index];
        if (entry.fresh && entry.fresh_deadline_microseconds != 0U
            && now_microseconds >= entry.fresh_deadline_microseconds) {
            entry.fresh = false;
            entry.ttl = 0;
            entry.initial_report_pending = true;
        }
        if (entry.fresh && entry.fresh_deadline_microseconds != 0U
            && (next_fresh_deadline_microseconds_ == 0U
                || entry.fresh_deadline_microseconds
                    < next_fresh_deadline_microseconds_)) {
            next_fresh_deadline_microseconds_ =
                entry.fresh_deadline_microseconds;
        }
    }
}

void ReceiveLossList::mark_periodic_reports() noexcept
{
    for (std::size_t index = 0; index < size_; ++index) {
        entries_[index].periodic_report_pending = true;
    }
}

void ReceiveLossList::tighten_fresh_deadlines(
    std::uint64_t latest_report, std::uint64_t now_microseconds) noexcept
{
    next_fresh_deadline_microseconds_ = 0;
    for (std::size_t i = 0; i < size_; ++i) {
        auto& entry = entries_[i];
        if (!entry.fresh || entry.fresh_deadline_microseconds == 0U)
            continue;
        entry.fresh_deadline_microseconds =
            std::min(entry.fresh_deadline_microseconds, latest_report);
        if (entry.fresh_deadline_microseconds <= now_microseconds) {
            entry.fresh = false;
            entry.ttl = 0;
            entry.initial_report_pending = true;
        } else if (next_fresh_deadline_microseconds_ == 0U
            || entry.fresh_deadline_microseconds
                < next_fresh_deadline_microseconds_) {
            next_fresh_deadline_microseconds_ =
                entry.fresh_deadline_microseconds;
        }
    }
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
         index < size_ && written < destination.size();
         ++index) {
        auto& entry = entries_[index];
        if (!entry.initial_report_pending
            && !entry.periodic_report_pending) {
            continue;
        }
        destination[written++] = entry.range;
        entry.initial_report_pending = false;
        entry.periodic_report_pending = false;
    }
    return written;
}

bool ReceiveLossList::has_pending_report() const noexcept
{
    return std::any_of(
        entries_.begin(),
        entries_.begin() + static_cast<std::ptrdiff_t>(size_),
        [](const Entry& entry) {
            return entry.initial_report_pending
                || entry.periodic_report_pending;
        });
}

} // namespace robotweax::srt
