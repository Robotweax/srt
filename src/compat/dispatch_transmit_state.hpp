#pragma once

#include "robotweax/srt/sequence.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace robotweax::srt::compat {

// State the member sessions of one multicast dispatch sender share on the
// data path: the repeat tally that turns several unicast retransmissions of
// one sequence into a single multicast repeat, and the counters of what left
// on the multicast path. Owned by the dispatch sender's group record and
// referenced from each member's egress; internally synchronized because
// members run on different channel threads. The tally is a fixed ring keyed
// by sequence, so a flood of distinct sequences evicts old entries instead
// of growing memory.
class DispatchTransmitState {
public:
    static constexpr std::size_t tally_capacity = 1'024;

    struct RepeatDecision {
        // This member should send the repeat on the multicast path.
        bool multicast = false;
        // UDP accepted another member's multicast repair inside the window;
        // suppress this member's first request only, not its later NAKs.
        bool skip = false;
        // Identifies this attempted repair across ring eviction/window reset.
        std::uint64_t attempt = 0;
    };

    // Records that `member` is about to repeat `sequence` at `now`. With
    // `threshold` 0 the answer is always a unicast repeat. Otherwise, once
    // `threshold` distinct members repeat the same sequence within
    // `window_microseconds` of the first, the member that reaches the
    // threshold attempts a multicast repair. Only a locally accepted repair
    // may suppress another member's first request. Any repeated request from
    // that member uses unicast, since the shared repair may itself be lost.
    [[nodiscard]] RepeatDecision note_repeat(SequenceNumber sequence,
        std::int32_t member, std::uint64_t now_microseconds,
        std::uint32_t threshold, std::uint64_t window_microseconds) noexcept
    {
        if (threshold == 0U) {
            return {};
        }
        std::lock_guard lock(mutex_);
        Entry& entry = entries_[sequence.value() % tally_capacity];
        if (entry.valid && entry.sequence == sequence && entry.attempt != 0U
            && (now_microseconds < entry.first_microseconds
                || now_microseconds - entry.first_microseconds
                    > window_microseconds)) {
            // Once this sequence had a multicast attempt, expiry restores
            // unicast rather than re-electing the same lossy repair forever.
            return {};
        }
        const bool fresh = !entry.valid || entry.sequence != sequence
            || now_microseconds < entry.first_microseconds
            || now_microseconds - entry.first_microseconds
                > window_microseconds;
        if (fresh) {
            entry = Entry {
                .sequence = sequence,
                .first_microseconds = now_microseconds,
                .valid = true,
            };
        }
        for (std::size_t index = 0; index < entry.member_count; ++index) {
            if (entry.members[index] == member) {
                // The same member repeating again (a second NAK) does not
                // count as another receiver.
                return {};
            }
        }
        if (entry.member_count == entry.members.size()) {
            // Do not suppress a receiver whose identity cannot be retained:
            // its next NAK must always be able to obtain a unicast repair.
            return {};
        }
        entry.members[entry.member_count++] = member;
        if (entry.multicast_done) {
            return {.skip = true};
        }
        if (entry.member_count >= threshold && entry.attempt == 0U) {
            ++next_attempt_;
            if (next_attempt_ == 0U) {
                ++next_attempt_;
            }
            entry.attempt = next_attempt_;
            return {.multicast = true, .attempt = entry.attempt};
        }
        return {};
    }

    void complete_repeat(
        SequenceNumber sequence, std::uint64_t attempt, bool accepted) noexcept
    {
        std::lock_guard lock(mutex_);
        Entry& entry = entries_[sequence.value() % tally_capacity];
        if (entry.valid && entry.sequence == sequence && attempt != 0U
            && entry.attempt == attempt) {
            entry.multicast_done = accepted;
        }
        // Keep the attempt while this sequence occupies its ring slot.
        // Subsequent requests use unicast after failure or window expiry.
    }

    void note_multicast_sent(std::size_t bytes, bool repeat) noexcept
    {
        multicast_packets_.fetch_add(1U, std::memory_order_relaxed);
        multicast_bytes_.fetch_add(bytes, std::memory_order_relaxed);
        if (repeat) {
            multicast_repeats_.fetch_add(1U, std::memory_order_relaxed);
        }
    }
    void note_unicast_repeat_skipped() noexcept
    {
        skipped_repeats_.fetch_add(1U, std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t multicast_packets() const noexcept
    {
        return multicast_packets_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t multicast_bytes() const noexcept
    {
        return multicast_bytes_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t multicast_repeats() const noexcept
    {
        return multicast_repeats_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t skipped_repeats() const noexcept
    {
        return skipped_repeats_.load(std::memory_order_relaxed);
    }

private:
    struct Entry {
        SequenceNumber sequence {};
        std::uint64_t first_microseconds = 0;
        std::uint64_t attempt = 0;
        std::array<std::int32_t, 16> members {};
        std::uint8_t member_count = 0;
        bool multicast_done = false;
        bool valid = false;
    };

    std::mutex mutex_;
    std::array<Entry, tally_capacity> entries_ {};
    std::uint64_t next_attempt_ = 0;
    std::atomic<std::uint64_t> multicast_packets_ {0};
    std::atomic<std::uint64_t> multicast_bytes_ {0};
    std::atomic<std::uint64_t> multicast_repeats_ {0};
    std::atomic<std::uint64_t> skipped_repeats_ {0};
};

} // namespace robotweax::srt::compat
