#pragma once

#include "compat/retransmission_budget.hpp"
#include "robotweax/srt/sequence.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>

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

    // Aggregate limit for the unicast repeats of every member of a default
    // (not coordinated) dispatch sender: four times a positive group
    // SRTO_MAXBW, counted like SRTO_MAXREXMITBW as the SRT header plus the
    // protected payload without IP/UDP headers. Any other SRTO_MAXBW value
    // leaves the repeats unlimited. Members run on different channel threads
    // with different time origins, so the bucket keeps its own clock.
    static constexpr std::int64_t unicast_repeat_rate_factor = 4;
    [[nodiscard]] static std::uint64_t unicast_repeat_clock() noexcept
    {
        const auto count =
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count();
        return count > 0 ? static_cast<std::uint64_t>(count) : 0U;
    }
    void configure_unicast_repeat_limit(
        std::int64_t group_maximum_bandwidth, std::uint64_t now) noexcept
    {
        constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
        const std::int64_t limit = group_maximum_bandwidth <= 0 ? -1
            : group_maximum_bandwidth > maximum / unicast_repeat_rate_factor
            ? maximum
            : group_maximum_bandwidth * unicast_repeat_rate_factor;
        std::lock_guard lock(unicast_repeat_mutex_);
        unicast_repeat_budget_.configure(limit, now);
        unicast_repeat_limited_.store(limit > 0, std::memory_order_relaxed);
    }
    [[nodiscard]] bool unicast_repeat_limited() const noexcept
    {
        return unicast_repeat_limited_.load(std::memory_order_relaxed);
    }
    // A hint for scheduling only; another member may take the credit first.
    [[nodiscard]] bool unicast_repeat_ready(
        std::uint64_t bytes, std::uint64_t now) noexcept
    {
        if (!unicast_repeat_limited()) {
            return true;
        }
        std::lock_guard lock(unicast_repeat_mutex_);
        return unicast_repeat_budget_.ready(bytes, now);
    }
    // Serialize the nonblocking UDP attempt with credit consumption and rate
    // changes. Failed sends spend nothing, and queued retries never retain
    // credit from an earlier configuration. The callback must not reenter
    // this state and returns true only when UDP accepts the whole datagram.
    template <typename Submit>
    [[nodiscard]] bool submit_unicast_repeat(
        std::uint64_t bytes, std::uint64_t now, Submit&& submit) noexcept
    {
        std::lock_guard lock(unicast_repeat_mutex_);
        if (!unicast_repeat_budget_.ready(bytes, now)) {
            unicast_repeat_waits_.fetch_add(1U, std::memory_order_relaxed);
            return false;
        }
        if (submit() && unicast_repeat_limited()) {
            unicast_repeat_budget_.consume(bytes, now);
            unicast_repeat_bytes_.fetch_add(bytes, std::memory_order_relaxed);
        }
        return true;
    }
    [[nodiscard]] bool take_unicast_repeat(
        std::uint64_t bytes, std::uint64_t now) noexcept
    {
        return submit_unicast_repeat(bytes, now, [] {
            return true;
        });
    }
    // Microseconds until `bytes` of credit exist; 0 when ready or unlimited.
    [[nodiscard]] std::optional<std::uint64_t> unicast_repeat_delay(
        std::uint64_t bytes, std::uint64_t now) noexcept
    {
        if (!unicast_repeat_limited()) {
            return 0;
        }
        std::lock_guard lock(unicast_repeat_mutex_);
        return unicast_repeat_budget_.delay(bytes, now);
    }
    // Bytes charged to the limit, and charges refused after scheduling.
    [[nodiscard]] std::uint64_t unicast_repeat_bytes() const noexcept
    {
        return unicast_repeat_bytes_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t unicast_repeat_waits() const noexcept
    {
        return unicast_repeat_waits_.load(std::memory_order_relaxed);
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
    std::mutex unicast_repeat_mutex_;
    RetransmissionBudget unicast_repeat_budget_;
    std::atomic<bool> unicast_repeat_limited_ {false};
    std::atomic<std::uint64_t> unicast_repeat_bytes_ {0};
    std::atomic<std::uint64_t> unicast_repeat_waits_ {0};
};

} // namespace robotweax::srt::compat
