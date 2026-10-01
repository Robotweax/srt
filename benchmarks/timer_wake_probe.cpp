#include "compat/runtime_scheduler.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <system_error>
#include <vector>

using robotweax::srt::compat::RuntimeScheduler;

namespace {

using Clock = std::chrono::steady_clock;

struct Configuration {
    std::size_t shards = 1;
    std::size_t fanout = 1;
    std::size_t rounds = 1'000;
    std::size_t warmup = 20;
    std::uint64_t callback_work_microseconds = 0;
};

struct Batch {
    std::mutex mutex;
    std::condition_variable ready;
    std::size_t remaining = 0;
};

struct Wake {
    std::shared_ptr<Batch> batch;
    std::optional<Clock::time_point> idle_wake;
    Clock::time_point entered {};
    Clock::time_point finished {};
    std::uint64_t work_microseconds = 0;
};

void record_timer_wake(
    void* context, std::optional<Clock::time_point> idle_wake) noexcept
{
    auto& wake = *static_cast<Wake*>(context);
    wake.entered = Clock::now();
    wake.idle_wake = idle_wake;
    if (wake.work_microseconds != 0U) {
        const auto until =
            wake.entered + std::chrono::microseconds {wake.work_microseconds};
        // Deliberate callback CPU work, without another OS timer wait.
        while (Clock::now() < until) {
            std::atomic_signal_fence(std::memory_order_seq_cst);
        }
    }
    wake.finished = Clock::now();
    bool complete = false;
    {
        std::lock_guard lock(wake.batch->mutex);
        complete = --wake.batch->remaining == 0U;
    }
    if (complete) {
        wake.batch->ready.notify_one();
    }
}

void record_wake(void* context) noexcept
{
    record_timer_wake(context, std::nullopt);
}

std::int64_t elapsed_microseconds(
    Clock::time_point later, Clock::time_point earlier)
{
    return std::max<std::int64_t>(0,
        std::chrono::duration_cast<std::chrono::microseconds>(later - earlier)
            .count());
}

void print_distribution(
    std::string_view prefix, std::vector<std::int64_t>& values)
{
    std::sort(values.begin(), values.end());
    const auto percentile = [&](std::size_t numerator) {
        return values[(values.size() - 1U) * numerator / 100U];
    };
    std::cout << ",\"" << prefix << "_samples\":" << values.size();
    if (values.empty()) {
        // An empty idle-wake distribution means the shard was busy, not a
        // punctual host clock. Keep that distinction in machine-readable data.
        std::cout << ",\"" << prefix << "_p50_us\":null,\"" << prefix
                  << "_p95_us\":null,\"" << prefix << "_p99_us\":null,\""
                  << prefix << "_max_us\":null";
        return;
    }
    std::cout << ",\"" << prefix << "_p50_us\":" << percentile(50) << ",\""
              << prefix << "_p95_us\":" << percentile(95) << ",\"" << prefix
              << "_p99_us\":" << percentile(99) << ",\"" << prefix
              << "_max_us\":" << values.back();
}

bool measure(RuntimeScheduler& scheduler, const Configuration& config,
    std::int64_t delay_microseconds)
{
    std::vector<std::int64_t> lateness, idle_lateness, idle_to_callback,
        busy_lateness, callback_duration;
    const auto samples = config.rounds * config.fanout;
    for (auto* values : {&lateness, &idle_lateness, &idle_to_callback,
             &busy_lateness, &callback_duration}) {
        values->reserve(samples);
    }
    for (std::size_t index = 0; index < config.warmup + config.rounds;
        ++index) {
        auto batch = std::make_shared<Batch>();
        batch->remaining = config.fanout;
        std::vector<std::shared_ptr<Wake>> wakes;
        wakes.reserve(config.fanout);
        for (std::size_t member = 0; member < config.fanout; ++member) {
            auto wake = std::make_shared<Wake>();
            wake->batch = batch;
            wake->work_microseconds = config.callback_work_microseconds;
            wakes.push_back(std::move(wake));
        }
        const auto deadline =
            Clock::now() + std::chrono::microseconds {delay_microseconds};
        for (std::size_t member = 0; member < wakes.size(); ++member) {
            const auto scheduled = scheduler.schedule_at(member, deadline,
                RuntimeScheduler::Task {
                    record_wake, wakes[member], record_timer_wake});
            if (scheduled.status != RuntimeScheduler::SubmitStatus::accepted) {
                return false;
            }
        }
        std::unique_lock lock(batch->mutex);
        if (!batch->ready.wait_for(lock, std::chrono::seconds {5}, [&] {
                return batch->remaining == 0U;
            })) {
            return false;
        }
        if (index < config.warmup) {
            continue;
        }
        for (const auto& wake : wakes) {
            const auto late = elapsed_microseconds(wake->entered, deadline);
            lateness.push_back(late);
            callback_duration.push_back(
                elapsed_microseconds(wake->finished, wake->entered));
            if (wake->idle_wake.has_value()) {
                idle_lateness.push_back(
                    elapsed_microseconds(*wake->idle_wake, deadline));
                idle_to_callback.push_back(
                    elapsed_microseconds(wake->entered, *wake->idle_wake));
            } else {
                busy_lateness.push_back(late);
            }
        }
    }
    const auto over_credit =
        std::count_if(lateness.begin(), lateness.end(), [](std::int64_t late) {
            return late > 1'000;
        });
    std::cout << "{\"requested_delay_us\":" << delay_microseconds
              << ",\"samples\":" << samples << ",\"rounds\":" << config.rounds
              << ",\"warmup_rounds\":" << config.warmup
              << ",\"fanout\":" << config.fanout
              << ",\"shards\":" << config.shards
              << ",\"callback_work_us\":" << config.callback_work_microseconds
              << ",\"over_credit_count\":" << over_credit;
    // Preserve the original callback-entry lateness fields for serial users.
    print_distribution("lateness", lateness);
    // Timers due at the same wake repeat its timestamp: these distributions
    // are callback-weighted, not a count of independent OS wake-ups.
    print_distribution("idle_lateness", idle_lateness);
    print_distribution("idle_to_callback", idle_to_callback);
    print_distribution("busy_lateness", busy_lateness);
    print_distribution("callback_duration", callback_duration);
    std::cout << "}\n";
    return true;
}

bool parse_arguments(int argc, char** argv, Configuration& config)
{
    for (int index = 1; index < argc; index += 2) {
        if (index + 1 >= argc) {
            return false;
        }
        const std::string_view option {argv[index]};
        const std::string_view value {argv[index + 1]};
        std::uint64_t number = 0;
        const auto parsed =
            std::from_chars(value.data(), value.data() + value.size(), number);
        if (parsed.ec != std::errc {}
            || parsed.ptr != value.data() + value.size()) {
            return false;
        }
        if (option == "--shards" && number >= 1 && number <= 64) {
            config.shards = static_cast<std::size_t>(number);
        } else if (option == "--fanout" && number >= 1 && number <= 256) {
            config.fanout = static_cast<std::size_t>(number);
        } else if (option == "--rounds" && number >= 1 && number <= 10'000) {
            config.rounds = static_cast<std::size_t>(number);
        } else if (option == "--warmup" && number <= 1'000) {
            config.warmup = static_cast<std::size_t>(number);
        } else if (option == "--callback-work-us" && number <= 1'000) {
            config.callback_work_microseconds = number;
        } else {
            return false;
        }
    }
    return config.rounds * config.fanout <= 1'000'000U;
}

} // namespace

int main(int argc, char** argv)
{
    Configuration config;
    if (!parse_arguments(argc, argv, config)) {
        std::cerr << "usage: robotweax_srt_timer_wake_probe"
                     " [--shards 1..64] [--fanout 1..256] [--rounds 1..10000]"
                     " [--warmup 0..1000] [--callback-work-us 0..1000]\n";
        return 2;
    }
    try {
        RuntimeScheduler scheduler(
            {config.shards, config.fanout + 16U, config.fanout + 16U});
        if (!scheduler.start()) {
            std::cerr << "scheduler start failed\n";
            return 1;
        }
        for (const auto delay : std::array<std::int64_t, 3> {200, 500, 1'000}) {
            if (!measure(scheduler, config, delay)) {
                std::cerr << "timer measurement failed\n";
                return 1;
            }
        }
    } catch (const std::exception& error) {
        std::cerr << "timer measurement failed: " << error.what() << '\n';
        return 1;
    }
}
