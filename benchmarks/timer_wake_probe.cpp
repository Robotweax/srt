#include "compat/runtime_scheduler.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <vector>

using robotweax::srt::compat::RuntimeScheduler;

namespace {

using Clock = std::chrono::steady_clock;

struct Wake {
    std::mutex mutex;
    std::condition_variable ready;
    Clock::time_point observed {};
    bool complete = false;
};

void record_wake(void* context) noexcept
{
    auto& wake = *static_cast<Wake*>(context);
    const auto observed = Clock::now();
    {
        std::lock_guard lock(wake.mutex);
        wake.observed = observed;
        wake.complete = true;
    }
    wake.ready.notify_one();
}

std::int64_t percentile(const std::vector<std::int64_t>& sorted,
    std::size_t numerator, std::size_t denominator)
{
    return sorted[(sorted.size() - 1U) * numerator / denominator];
}

bool measure(RuntimeScheduler& scheduler, std::int64_t delay_microseconds)
{
    constexpr std::size_t warmup = 20;
    constexpr std::size_t samples = 1'000;
    std::vector<std::int64_t> lateness;
    lateness.reserve(samples);
    for (std::size_t index = 0; index < warmup + samples; ++index) {
        auto wake = std::make_shared<Wake>();
        const auto deadline =
            Clock::now() + std::chrono::microseconds {delay_microseconds};
        const auto scheduled = scheduler.schedule_at(
            0, deadline, RuntimeScheduler::Task {record_wake, wake});
        if (scheduled.status != RuntimeScheduler::SubmitStatus::accepted) {
            return false;
        }
        std::unique_lock lock(wake->mutex);
        if (!wake->ready.wait_for(lock, std::chrono::seconds {2}, [&] {
                return wake->complete;
            })) {
            return false;
        }
        if (index >= warmup) {
            const auto late =
                std::chrono::duration_cast<std::chrono::microseconds>(
                    wake->observed - deadline)
                    .count();
            lateness.push_back(std::max<std::int64_t>(0, late));
        }
    }
    std::sort(lateness.begin(), lateness.end());
    const auto over_credit =
        std::count_if(lateness.begin(), lateness.end(), [](std::int64_t late) {
            return late > 1'000;
        });
    std::cout << "{\"requested_delay_us\":" << delay_microseconds
              << ",\"samples\":" << samples
              << ",\"lateness_p50_us\":" << percentile(lateness, 50, 100)
              << ",\"lateness_p95_us\":" << percentile(lateness, 95, 100)
              << ",\"lateness_p99_us\":" << percentile(lateness, 99, 100)
              << ",\"lateness_max_us\":" << lateness.back()
              << ",\"over_credit_count\":" << over_credit << "}\n";
    return true;
}

} // namespace

int main()
{
    RuntimeScheduler scheduler({1, 16, 16});
    if (!scheduler.start()) {
        std::cerr << "scheduler start failed\n";
        return 1;
    }
    for (const auto delay : std::array<std::int64_t, 3> {200, 500, 1'000}) {
        if (!measure(scheduler, delay)) {
            std::cerr << "timer measurement failed\n";
            return 1;
        }
    }
}
