// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Robotweax GmbH
#pragma once

// Included only in a fresh diagnostic source export. Fixed thread-local
// storage; no clock query, allocation, synchronization or I/O per increment.
// PID/TID registration writes one header once. Counters flush at thread exit
// and include setup and shutdown activity.
#include <array>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <unistd.h>
#ifdef __linux__
#include <sys/syscall.h>
#else
#include <pthread.h>
#endif

// clang-format off: keep the counter schema one name per line.
#define RWX_COUNTER_NAMES(X) \
    X(worker_starts) \
    X(channel_calls) \
    X(channel_full_polls) \
    X(channel_skips) \
    X(channel_no_hint) \
    X(channel_after_skip) \
    X(rx_attempts) \
    X(rx_datagrams) \
    X(rx_eagain) \
    X(rx_oversize) \
    X(rx_errors) \
    X(rx_decode_ok) \
    X(rx_data) \
    X(channel_immediate_io) \
    X(channel_immediate_pacer) \
    X(channel_delayed) \
    X(connection_polls) \
    X(poll_packets) \
    X(batch_0) \
    X(batch_1) \
    X(batch_2_4) \
    X(batch_5_16) \
    X(batch_17_64) \
    X(batch_over_64) \
    X(tx_originals) \
    X(tx_retransmissions) \
    X(route_none) \
    X(route_multiple) \
    X(route_setup) \
    X(route_listener) \
    X(route_isolated) \
    X(arm_missing_hint) \
    X(arm_epoch_changed) \
    X(arm_calls) \
    X(arm_due) \
    X(arm_overflow) \
    X(arm_success) \
    X(take_calls) \
    X(take_no_pending) \
    X(take_epoch_changed) \
    X(take_rollback) \
    X(take_expired_cap) \
    X(take_expired_pacer) \
    X(take_success) \
    X(age_rollback) \
    X(age_lt_1us) \
    X(age_lt_2us) \
    X(age_lt_5us) \
    X(age_ge_5us)
// clang-format on

namespace robotweax::srt::diagnostics {
#define RWX_ENUM(name) name,
enum class Counter { RWX_COUNTER_NAMES(RWX_ENUM) count };
#undef RWX_ENUM
#define RWX_NAME(name) #name,
inline constexpr const char* counter_names[] = {RWX_COUNTER_NAMES(RWX_NAME)};
#undef RWX_NAME
inline std::atomic<unsigned> counter_thread_serial {0};

class ContinuationCounters {
public:
    ContinuationCounters() noexcept
    {
        const char* directory =
            std::getenv("ROBOTWEAX_CONTINUATION_COUNTER_DIR");
        if (directory == nullptr || *directory == '\0') {
            return;
        }
        pid_ = static_cast<std::uint64_t>(getpid());
#ifdef __linux__
        tid_ = static_cast<std::uint64_t>(syscall(SYS_gettid));
#else
        if (pthread_threadid_np(nullptr, &tid_) != 0) {
            std::fputs(
                "continuation counters: cannot identify thread\n", stderr);
            return;
        }
#endif
        serial_ =
            counter_thread_serial.fetch_add(1U, std::memory_order_relaxed);
        const int size = std::snprintf(path_.data(), path_.size(),
            "%s/%" PRIu64 "-%" PRIu64 "-%u.jsonl", directory, pid_, tid_,
            serial_);
        enabled_ = size > 0 && static_cast<std::size_t>(size) < path_.size();
        if (!enabled_) {
            std::fputs("continuation counters: output path too long\n", stderr);
        }
        if (enabled_) {
            file_ = std::fopen(path_.data(), "wx");
            enabled_ = file_ != nullptr;
            if (enabled_) {
                std::fprintf(file_,
                    "{\"schema\":2,\"pid\":%" PRIu64 ",\"tid\":%" PRIu64
                    ",\"serial\":%u}\n",
                    pid_, tid_, serial_);
                std::fflush(file_);
            } else {
                std::fputs(
                    "continuation counters: cannot register thread output\n",
                    stderr);
            }
        }
    }

    void worker(std::size_t shard) noexcept
    {
        shard_ = shard;
        add(Counter::worker_starts);
    }

    void add(Counter counter, std::uint64_t amount = 1) noexcept
    {
        if (!enabled_) {
            return;
        }
        auto& value = values_[static_cast<std::size_t>(counter)];
        if (amount > std::numeric_limits<std::uint64_t>::max() - value) {
            overflow_ = true;
            value = std::numeric_limits<std::uint64_t>::max();
        } else {
            value += amount;
        }
    }

    ~ContinuationCounters()
    {
        if (!enabled_) {
            return;
        }
        auto* file = file_;
        std::fprintf(file,
            "{\"schema\":2,\"pid\":%" PRIu64 ",\"tid\":%" PRIu64
            ",\"serial\":%u,\"overflow\":%s,\"worker_shard\":%zu,\"counters\":"
            "{",
            pid_, tid_, serial_, overflow_ ? "true" : "false", shard_);
        for (std::size_t i = 0; i < values_.size(); ++i) {
            std::fprintf(file, "%s\"%s\":%" PRIu64, i == 0 ? "" : ",",
                counter_names[i], values_[i]);
        }
        std::fputs("},\"complete\":true}\n", file);
        const bool failed = std::ferror(file) != 0;
        if (std::fclose(file) != 0 || failed) {
            std::fputs(
                "continuation counters: incomplete thread output\n", stderr);
        }
    }

private:
    std::array<std::uint64_t, static_cast<std::size_t>(Counter::count)>
        values_ {};
    std::array<char, 4096> path_ {};
    std::uint64_t pid_ = 0, tid_ = 0;
    unsigned serial_ = 0;
    std::size_t shard_ = std::numeric_limits<std::size_t>::max();
    std::FILE* file_ = nullptr;
    bool enabled_ = false, overflow_ = false;
};

inline ContinuationCounters& continuation_counters() noexcept
{
    static thread_local ContinuationCounters counters;
    return counters;
}

// Stack-local scope; counts successful DATA sends in this connection poll,
// including retransmissions, excluding FEC/control. No class layout changes.
struct PollScope {
    std::uint64_t packets = 0;
    PollScope() noexcept
    {
        continuation_counters().add(Counter::connection_polls);
    }
    ~PollScope()
    {
        auto& counts = continuation_counters();
        counts.add(Counter::poll_packets, packets);
        counts.add(packets == 0 ? Counter::batch_0
                : packets == 1  ? Counter::batch_1
                : packets <= 4  ? Counter::batch_2_4
                : packets <= 16 ? Counter::batch_5_16
                : packets <= 64 ? Counter::batch_17_64
                                : Counter::batch_over_64);
    }
};
} // namespace robotweax::srt::diagnostics

#define RWX_COUNT(name)                                                        \
    ::robotweax::srt::diagnostics::continuation_counters().add(                \
        ::robotweax::srt::diagnostics::Counter::name)
