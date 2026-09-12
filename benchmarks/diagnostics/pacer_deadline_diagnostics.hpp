#pragma once

// Diagnostic exports only. No public peer/API changes and no per-packet I/O.
#include <array>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#include <sys/syscall.h>
#endif

// clang-format off
#define RWX_DD_COUNTERS(X) \
    X(ready_tasks) X(timer_tasks) X(wait_calls) X(wait_returns) \
    X(timed_wait_calls) X(timed_wait_returns) X(early_wait_returns) \
    X(channel_polls) X(connection_polls) X(rx_attempts) X(rx_eagain) \
    X(rx_datagrams) X(flow_block) X(crypto_block) X(pacer_block) \
    X(selection_empty) X(data_packets) X(original_packets) \
    X(retransmitted_packets) X(data_without_pacer_deadline)
#define RWX_DD_HISTOGRAMS(X) \
    X(timer_remaining_on_submit_ns) X(timer_overdue_on_submit_ns) \
    X(timer_dispatch_lateness_ns) X(ready_queue_ns) X(timer_queue_ns) \
    X(wait_requested_ns) X(wait_elapsed_ns) X(wait_lateness_ns) \
    X(task_elapsed_ns) X(task_to_send_ns) X(pacer_send_lateness_ns) \
    X(pacer_lateness_at_task_start_ns) X(pacer_remaining_at_task_start_ns) \
    X(pacer_send_earliness_ns) X(data_per_task) X(data_per_poll)
// clang-format on

namespace robotweax::srt::deadline_diagnostics {
using Clock = std::chrono::steady_clock;
using Time = Clock::time_point;
#define RWX_DD_ENUM(name) name,
enum class Count { RWX_DD_COUNTERS(RWX_DD_ENUM) size };
enum class Metric { RWX_DD_HISTOGRAMS(RWX_DD_ENUM) size };
#undef RWX_DD_ENUM
#define RWX_DD_NAME(name) #name,
inline constexpr const char* count_names[] = {RWX_DD_COUNTERS(RWX_DD_NAME)};
inline constexpr const char* metric_names[] = {RWX_DD_HISTOGRAMS(RWX_DD_NAME)};
#undef RWX_DD_NAME
// Inclusive upper bounds; the last bucket is larger than the last bound.
inline constexpr std::array<std::uint64_t, 18> bounds {0, 1, 2, 4, 16, 64, 256,
    1'000, 2'000, 5'000, 10'000, 20'000, 50'000, 100'000, 250'000, 500'000,
    1'000'000, 10'000'000};
inline std::atomic<unsigned> serial_source {0};

inline std::uint64_t elapsed(Time end, Time begin) noexcept
{
    if (end <= begin) {
        return 0;
    }
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin)
            .count());
}

struct Histogram {
    std::array<std::uint64_t, bounds.size() + 1U> buckets {};
    std::uint64_t count = 0, sum = 0, maximum = 0;
};

class Recorder {
public:
    void start(std::size_t shard, std::size_t shards) noexcept
    {
        if (started_) {
            return;
        }
        started_ = true;
        const char* directory = std::getenv("ROBOTWEAX_PACER_DEADLINE_DIR");
        if (directory == nullptr || *directory == '\0') {
            return;
        }
        pid_ = static_cast<std::uint64_t>(getpid());
#ifdef __linux__
        tid_ = static_cast<std::uint64_t>(syscall(SYS_gettid));
        timer_slack_ns_ = prctl(PR_GET_TIMERSLACK, 0UL, 0UL, 0UL, 0UL);
#else
        if (pthread_threadid_np(nullptr, &tid_) != 0) {
            return;
        }
#endif
        serial_ = serial_source.fetch_add(1U, std::memory_order_relaxed);
        shard_ = shard;
        shards_ = shards;
        std::array<char, 4096> path {};
        const int n = std::snprintf(path.data(), path.size(),
            "%s/%" PRIu64 "-%" PRIu64 "-%u.jsonl", directory, pid_, tid_,
            serial_);
        if (n <= 0 || static_cast<std::size_t>(n) >= path.size()) {
            return;
        }
        file_ = std::fopen(path.data(), "wx");
        if (file_ == nullptr) {
            std::fputs("pacer diagnostics: cannot register worker\n", stderr);
            return;
        }
        std::fprintf(file_,
            "{\"schema\":1,\"event\":\"register\",\"pid\":%" PRIu64
            ",\"tid\":%" PRIu64 ",\"serial\":%u,\"shard\":%zu,"
            "\"shards\":%zu,\"timer_slack_ns\":%ld}\n",
            pid_, tid_, serial_, shard_, shards_, timer_slack_ns_);
        std::fflush(file_);
        registered_at_ = checkpoint_at_ = Clock::now();
    }

    void add(Count name, std::uint64_t amount = 1) noexcept
    {
        if (file_ != nullptr) {
            increment(counts_[static_cast<std::size_t>(name)], amount);
        }
    }

    void observe(Metric name, std::uint64_t value) noexcept
    {
        if (file_ == nullptr) {
            return;
        }
        auto& h = histograms_[static_cast<std::size_t>(name)];
        std::size_t bucket = 0;
        while (bucket < bounds.size() && value > bounds[bucket]) {
            ++bucket;
        }
        increment(h.buckets[bucket], 1);
        increment(h.count, 1);
        increment(h.sum, value);
        h.maximum = value > h.maximum ? value : h.maximum;
    }

    // At most one checkpoint per second, after a callback and outside its locks.
    // Keep the last complete line if termination interrupts a later write.
    void checkpoint(Time now) noexcept
    {
        if (file_ != nullptr
            && now - checkpoint_at_ >= std::chrono::seconds {1}) {
            checkpoint_at_ = now;
            if (snapshots_ < 128U) {
                snapshot(false, now);
            } else {
                checkpoint_limit_ = true;
            }
        }
    }

    ~Recorder()
    {
        if (file_ != nullptr) {
            snapshot(true, Clock::now());
            if (std::fclose(file_) != 0) {
                std::fputs("pacer diagnostics: incomplete output\n", stderr);
            }
        }
    }

private:
    void increment(std::uint64_t& value, std::uint64_t amount) noexcept
    {
        if (amount > std::numeric_limits<std::uint64_t>::max() - value) {
            overflow_ = true;
            value = std::numeric_limits<std::uint64_t>::max();
        } else {
            value += amount;
        }
    }

    void snapshot(bool complete, Time now) noexcept
    {
        ++snapshots_;
        std::fprintf(file_,
            "{\"schema\":1,\"event\":\"snapshot\",\"ordinal\":%u,"
            "\"complete\":%s,\"overflow\":%s,\"checkpoint_limit\":%s,"
            "\"elapsed_ns\":%" PRIu64 ",\"counters\":{",
            snapshots_, complete ? "true" : "false",
            overflow_ ? "true" : "false", checkpoint_limit_ ? "true" : "false",
            elapsed(now, registered_at_));
        for (std::size_t i = 0; i < counts_.size(); ++i) {
            std::fprintf(file_, "%s\"%s\":%" PRIu64, i == 0 ? "" : ",",
                count_names[i], counts_[i]);
        }
        std::fputs("},\"histograms\":{", file_);
        for (std::size_t i = 0; i < histograms_.size(); ++i) {
            const auto& h = histograms_[i];
            std::fprintf(file_,
                "%s\"%s\":{\"count\":%" PRIu64 ",\"sum\":%" PRIu64
                ",\"max\":%" PRIu64 ",\"buckets\":[",
                i == 0 ? "" : ",", metric_names[i], h.count, h.sum, h.maximum);
            for (std::size_t j = 0; j < h.buckets.size(); ++j) {
                std::fprintf(
                    file_, "%s%" PRIu64, j == 0 ? "" : ",", h.buckets[j]);
            }
            std::fputs("]}", file_);
        }
        std::fputs("}}\n", file_);
        if (std::fflush(file_) != 0 || std::ferror(file_) != 0) {
            std::fputs("pacer diagnostics: incomplete snapshot\n", stderr);
        }
    }

    std::array<std::uint64_t, static_cast<std::size_t>(Count::size)> counts_ {};
    std::array<Histogram, static_cast<std::size_t>(Metric::size)>
        histograms_ {};
    std::FILE* file_ = nullptr;
    Time registered_at_ {}, checkpoint_at_ {};
    std::uint64_t pid_ = 0, tid_ = 0;
    std::size_t shard_ = 0, shards_ = 0;
    unsigned serial_ = 0, snapshots_ = 0;
    long timer_slack_ns_ = -1;
    bool started_ = false, overflow_ = false, checkpoint_limit_ = false;
};

inline Recorder& recorder() noexcept
{
    static thread_local Recorder own;
    return own;
}

inline thread_local Time task_start {};
inline thread_local std::uint64_t task_packets = 0;

struct TaskScope {
    TaskScope(Time submitted, Time deadline, bool timed) noexcept
    {
        task_start = Clock::now();
        task_packets = 0;
        auto& r = recorder();
        r.add(timed ? Count::timer_tasks : Count::ready_tasks);
        r.observe(timed ? Metric::timer_queue_ns : Metric::ready_queue_ns,
            elapsed(task_start, submitted));
        if (timed) {
            r.observe(Metric::timer_remaining_on_submit_ns,
                elapsed(deadline, submitted));
            r.observe(Metric::timer_overdue_on_submit_ns,
                elapsed(submitted, deadline));
            r.observe(Metric::timer_dispatch_lateness_ns,
                elapsed(task_start, deadline));
        }
    }
    ~TaskScope()
    {
        auto& r = recorder();
        r.observe(Metric::data_per_task, task_packets);
        const auto now = Clock::now();
        r.observe(Metric::task_elapsed_ns, elapsed(now, task_start));
        r.checkpoint(now);
        task_start = {};
    }
};

struct WaitScope {
    Time start = Clock::now(), deadline {};
    bool timed = false;
    explicit WaitScope(Time target = {}, bool has_target = false) noexcept
        : deadline(target)
        , timed(has_target)
    {
        recorder().add(timed ? Count::timed_wait_calls : Count::wait_calls);
        if (timed) {
            recorder().observe(
                Metric::wait_requested_ns, elapsed(deadline, start));
        }
    }
    ~WaitScope()
    {
        const auto now = Clock::now();
        auto& r = recorder();
        r.add(timed ? Count::timed_wait_returns : Count::wait_returns);
        if (timed) {
            r.observe(Metric::wait_elapsed_ns, elapsed(now, start));
            r.observe(Metric::wait_lateness_ns, elapsed(now, deadline));
            if (now < deadline) {
                r.add(Count::early_wait_returns);
            }
        }
    }
};

struct PollScope {
    std::uint64_t packets = 0;
    PollScope() noexcept
    {
        recorder().add(Count::connection_polls);
    }
    ~PollScope()
    {
        recorder().observe(Metric::data_per_poll, packets);
    }
};

inline void sent(Time deadline, bool has_deadline, bool retransmitted) noexcept
{
    const auto now = Clock::now();
    auto& r = recorder();
    r.add(Count::data_packets);
    r.add(
        retransmitted ? Count::retransmitted_packets : Count::original_packets);
    ++task_packets;
    if (task_start != Time {}) {
        r.observe(Metric::task_to_send_ns, elapsed(now, task_start));
    }
    if (has_deadline) {
        r.observe(Metric::pacer_lateness_at_task_start_ns,
            elapsed(task_start, deadline));
        r.observe(Metric::pacer_remaining_at_task_start_ns,
            elapsed(deadline, task_start));
        r.observe(Metric::pacer_send_lateness_ns, elapsed(now, deadline));
        r.observe(Metric::pacer_send_earliness_ns, elapsed(deadline, now));
    } else {
        r.add(Count::data_without_pacer_deadline);
    }
}
} // namespace robotweax::srt::deadline_diagnostics

#define RWX_DD_COUNT(name)                                                     \
    ::robotweax::srt::deadline_diagnostics::recorder().add(                    \
        ::robotweax::srt::deadline_diagnostics::Count::name)
