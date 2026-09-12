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
    X(channel_polls) \
    X(rx_attempts) \
    X(rx_datagrams) \
    X(rx_eagain) \
    X(rx_oversize) \
    X(rx_errors) \
    X(rx_decode_ok) \
    X(rx_ack) \
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
    X(poll_flow_block) \
    X(poll_crypto_block) \
    X(paced_selection_calls) \
    X(selection_pacer_block) \
    X(selection_empty) \
    X(pacer_queries) \
    X(pacer_flow_block) \
    X(pacer_time_block) \
    X(runtime_now_calls) \
    X(runtime_clock_reads) \
    X(scheduler_clock_reads) \
    X(channel_notify_calls) \
    X(channel_notify_coalesced) \
    X(channel_notify_pending) \
    X(scheduler_submit_calls) \
    X(scheduler_submit_accepted) \
    X(scheduler_notify_submit) \
    X(scheduler_notify_timer) \
    X(scheduler_notify_cancel) \
    X(scheduler_tasks) \
    X(scheduler_waits) \
    X(scheduler_wait_returns) \
    X(readiness_notify_calls) \
    X(readiness_no_waiters) \
    X(readiness_notify_all) \
    X(readiness_waits) \
    X(readiness_wait_returns)
// clang-format on

namespace robotweax::srt::diagnostics {
#define RWX_ENUM(name) name,
enum class Counter { RWX_COUNTER_NAMES(RWX_ENUM) count };
#undef RWX_ENUM
#define RWX_NAME(name) #name,
inline constexpr const char* counter_names[] = {RWX_COUNTER_NAMES(RWX_NAME)};
#undef RWX_NAME
inline std::atomic<unsigned> counter_thread_serial {0};

class PollWakeCounters {
public:
    PollWakeCounters() noexcept
    {
        const char* directory = std::getenv("ROBOTWEAX_POLL_COUNTER_DIR");
        if (directory == nullptr || *directory == '\0') {
            return;
        }
        pid_ = static_cast<std::uint64_t>(getpid());
#ifdef __linux__
        tid_ = static_cast<std::uint64_t>(syscall(SYS_gettid));
#else
        if (pthread_threadid_np(nullptr, &tid_) != 0) {
            std::fputs("poll counters: cannot identify thread\n", stderr);
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
            std::fputs("poll counters: output path too long\n", stderr);
        }
        if (enabled_) {
            file_ = std::fopen(path_.data(), "wx");
            enabled_ = file_ != nullptr;
            if (enabled_) {
                std::fprintf(file_,
                    "{\"schema\":1,\"pid\":%" PRIu64 ",\"tid\":%" PRIu64
                    ",\"serial\":%u}\n",
                    pid_, tid_, serial_);
                std::fflush(file_);
            } else {
                std::fputs(
                    "poll counters: cannot register thread output\n", stderr);
            }
        }
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

    ~PollWakeCounters()
    {
        if (!enabled_) {
            return;
        }
        auto* file = file_;
        std::fprintf(file,
            "{\"schema\":1,\"pid\":%" PRIu64 ",\"tid\":%" PRIu64
            ",\"serial\":%u,\"overflow\":%s,\"counters\":{",
            pid_, tid_, serial_, overflow_ ? "true" : "false");
        for (std::size_t i = 0; i < values_.size(); ++i) {
            std::fprintf(file, "%s\"%s\":%" PRIu64, i == 0 ? "" : ",",
                counter_names[i], values_[i]);
        }
        std::fputs("},\"complete\":true}\n", file);
        const bool failed = std::ferror(file) != 0;
        if (std::fclose(file) != 0 || failed) {
            std::fputs("poll counters: incomplete thread output\n", stderr);
        }
    }

private:
    std::array<std::uint64_t, static_cast<std::size_t>(Counter::count)>
        values_ {};
    std::array<char, 4096> path_ {};
    std::uint64_t pid_ = 0, tid_ = 0;
    unsigned serial_ = 0;
    std::FILE* file_ = nullptr;
    bool enabled_ = false, overflow_ = false;
};

inline PollWakeCounters& poll_counters() noexcept
{
    static thread_local PollWakeCounters counters;
    return counters;
}

// Stack-local scope; counts successful DATA sends in this connection poll,
// including retransmissions, excluding FEC/control. No class layout changes.
struct PollScope {
    std::uint64_t packets = 0;
    PollScope() noexcept
    {
        poll_counters().add(Counter::connection_polls);
    }
    ~PollScope()
    {
        auto& counts = poll_counters();
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
    ::robotweax::srt::diagnostics::poll_counters().add(                        \
        ::robotweax::srt::diagnostics::Counter::name)
#define RWX_CLOCK(name, expression) (RWX_COUNT(name), (expression))
