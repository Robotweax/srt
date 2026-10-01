#pragma once

#include "robotweax/srt/sequence.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace robotweax::srt {

struct SequenceRange {
    SequenceNumber first{};
    SequenceNumber last{};
};

enum class ReceiveStatus : std::uint8_t {
    accepted_in_order,
    accepted_out_of_order,
    duplicate,
    older_than_window,
    beyond_window,
};

struct ReceiveDecision {
    ReceiveStatus status = ReceiveStatus::accepted_in_order;
    std::optional<SequenceRange> gap_before_packet{};
    std::uint32_t contiguous_advance = 0;
};

class ReceiveWindow {
public:
    explicit ReceiveWindow(SequenceNumber first_expected, std::size_t capacity);

    [[nodiscard]] SequenceNumber first_expected() const noexcept { return first_expected_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size(); }
    [[nodiscard]] ReceiveDecision observe(SequenceNumber sequence) noexcept;

private:
    SequenceNumber first_expected_;
    std::vector<std::uint8_t> slots_;
    std::size_t head_ = 0;
};

struct ReceiveLossRemoval {
    bool removed = false;
    std::uint32_t remaining_ttl = 0;
    bool capacity_exhausted = false;
};

// Tracks disjoint missing sequence ranges in preallocated storage. New ranges
// are limited to `capacity`; another `capacity` slots are reserved for splits
// when a received or dropped sequence bisects an existing range. A fresh range
// delays its first loss report by a packet-count TTL; periodic reports continue
// to cover every outstanding range after that initial delay.
class ReceiveLossList {
public:
    explicit ReceiveLossList(std::size_t capacity);

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0U; }
    [[nodiscard]] bool contains(SequenceNumber sequence) const noexcept;
    [[nodiscard]] bool add(
        SequenceRange range, std::uint32_t initial_ttl) noexcept;
    [[nodiscard]] bool add_all(std::span<const SequenceRange> ranges,
        std::uint32_t initial_ttl) noexcept;
    // Insert one range at its sorted position, shifting later entries. Unlike
    // add_all this accepts a range that precedes existing entries, which the
    // FEC filter loss list needs because column groups close out of sequence
    // order. A range already covered by an existing entry is skipped and
    // reported as success. Partial overlaps, invalid ranges, and exhausted
    // capacity fail without modifying the list.
    [[nodiscard]] bool insert_sorted(
        SequenceRange range, std::uint32_t initial_ttl) noexcept;
    // Transactional feasibility check for a sorted insert of the whole batch:
    // every range must be valid, fit the remaining capacity, and not overlap an
    // existing entry or another range in the batch. Order within the batch does
    // not matter.
    [[nodiscard]] bool can_insert_all_sorted(
        std::span<const SequenceRange> ranges) const noexcept;
    [[nodiscard]] ReceiveLossRemoval remove(
        SequenceNumber sequence) noexcept;
    void remove_through(SequenceNumber last) noexcept;
    // Remove exactly the sequences inside `range`, trimming or splitting the
    // entries that overlap it. Entries outside the range are untouched. A
    // split that would exhaust the reserved slots returns false and leaves
    // that entry unchanged; callers must handle the failed removal.
    [[nodiscard]] bool remove_range(SequenceRange range) noexcept;
    // Retry a report that was prepared but never submitted. Only surviving
    // non-fresh losses become eligible; recovered/dropped ranges stay absent.
    void rearm_report(SequenceRange range) noexcept;
    void age_fresh() noexcept;
    // Each emitted range waits its own retry interval. Newly discovered
    // ranges remain eligible for their first report independently.
    void mark_periodic_reports(std::uint64_t now_microseconds = 0,
        std::uint64_t retry_interval_microseconds = 0) noexcept;
    [[nodiscard]] std::size_t take_pending_reports(
        std::span<SequenceRange> destination,
        std::uint64_t now_microseconds = 0) noexcept;
    [[nodiscard]] std::optional<SequenceRange> take_pending_report(
        std::uint64_t now_microseconds = 0) noexcept;
    [[nodiscard]] bool has_pending_report() const noexcept;

private:
    struct Entry {
        SequenceRange range{};
        std::uint32_t ttl = 0;
        bool fresh = false;
        bool initial_report_pending = false;
        bool periodic_report_pending = false;
        bool reported = false;
        std::uint64_t last_report_microseconds = 0;
    };

    void erase(std::size_t index) noexcept;
    [[nodiscard]] bool can_append(
        std::span<const SequenceRange> ranges) const noexcept;
    // Index of the first entry whose range ends at or after `sequence`
    // (entries are sorted and disjoint), or size_ when none does.
    [[nodiscard]] std::size_t lower_bound(
        SequenceNumber sequence) const noexcept;
    void note_added(const Entry& entry) noexcept;
    void note_removed(const Entry& entry) noexcept;

    std::vector<Entry> entries_;
    std::size_t admission_capacity_ = 0;
    std::size_t size_ = 0;
    // Entries still counting down their reorder TTL, and entries with a
    // report pending. Both let the per-packet paths skip a full walk.
    std::size_t fresh_count_ = 0;
    std::size_t pending_count_ = 0;
};

} // namespace robotweax::srt
