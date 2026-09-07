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
};

// Tracks disjoint missing sequence ranges in preallocated storage. A fresh
// range delays its first loss report by a packet-count TTL; periodic reports
// continue to cover every outstanding range after that initial delay.
class ReceiveLossList {
public:
    explicit ReceiveLossList(std::size_t capacity);

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0U; }
    [[nodiscard]] bool add(
        SequenceRange range, std::uint32_t initial_ttl) noexcept;
    [[nodiscard]] bool add_all(std::span<const SequenceRange> ranges,
        std::uint32_t initial_ttl) noexcept;
    [[nodiscard]] ReceiveLossRemoval remove(
        SequenceNumber sequence) noexcept;
    void remove_through(SequenceNumber last) noexcept;
    void age_fresh() noexcept;
    void mark_periodic_reports() noexcept;
    [[nodiscard]] std::size_t take_pending_reports(
        std::span<SequenceRange> destination) noexcept;
    [[nodiscard]] std::optional<SequenceRange>
    take_pending_report() noexcept;
    [[nodiscard]] bool has_pending_report() const noexcept;

private:
    struct Entry {
        SequenceRange range{};
        std::uint32_t ttl = 0;
        bool fresh = false;
        bool initial_report_pending = false;
        bool periodic_report_pending = false;
    };

    void erase(std::size_t index) noexcept;
    [[nodiscard]] bool can_append(
        std::span<const SequenceRange> ranges) const noexcept;

    std::vector<Entry> entries_;
    std::size_t size_ = 0;
};

} // namespace robotweax::srt
