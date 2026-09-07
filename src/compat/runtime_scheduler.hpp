#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace robotweax::srt::compat {

class RuntimeScheduler {
public:
    using TaskFunction = void (*)(void*) noexcept;

    struct Configuration {
        std::size_t shard_count = 0;
        std::size_t queue_capacity_per_shard = 0;
        std::size_t timer_capacity_per_shard = 0;
    };

    struct Task {
        TaskFunction function = nullptr;
        std::shared_ptr<void> context;
    };

    enum class SubmitStatus : std::uint8_t {
        accepted,
        full,
        stopped,
        invalid,
    };

    struct TimerToken {
        std::size_t shard = 0;
        std::size_t slot = 0;
        std::uint64_t generation = 0;

        [[nodiscard]] constexpr bool valid() const noexcept
        {
            return generation != 0U;
        }
    };

    struct ScheduleResult {
        SubmitStatus status = SubmitStatus::invalid;
        TimerToken token {};
    };

    struct Snapshot {
        std::size_t shard_count = 0;
        std::size_t queue_capacity = 0;
        std::size_t timer_capacity = 0;
        std::size_t queued = 0;
        std::size_t timers = 0;
        std::size_t executing = 0;
        std::uint64_t accepted = 0;
        std::uint64_t completed = 0;
        std::uint64_t rejected_full = 0;
        std::uint64_t rejected_stopped = 0;
        std::uint64_t rejected_invalid = 0;
        std::uint64_t timers_scheduled = 0;
        std::uint64_t timers_canceled = 0;
        bool accepting = false;
    };

    explicit RuntimeScheduler(Configuration configuration);
    ~RuntimeScheduler();

    RuntimeScheduler(const RuntimeScheduler&) = delete;
    RuntimeScheduler& operator=(const RuntimeScheduler&) = delete;

    [[nodiscard]] bool start() noexcept;
    [[nodiscard]] SubmitStatus submit(
        std::uint64_t affinity, Task task) noexcept;
    [[nodiscard]] ScheduleResult schedule_at(std::uint64_t affinity,
        std::chrono::steady_clock::time_point deadline, Task task) noexcept;
    [[nodiscard]] bool cancel_timer(TimerToken token) noexcept;
    void stop() noexcept;

    [[nodiscard]] std::size_t shard_for(std::uint64_t affinity) const noexcept;
    [[nodiscard]] Snapshot snapshot() const noexcept;

private:
    struct Shard {
        struct TimerSlot {
            Task task;
            std::chrono::steady_clock::time_point deadline {};
            std::uint64_t generation = 0;
            std::uint64_t order = 0;
            std::size_t heap_position = 0;
            bool active = false;
        };

        Shard(std::size_t queue_capacity, std::size_t timer_capacity);

        [[nodiscard]] bool timer_less(
            std::size_t left_slot, std::size_t right_slot) const noexcept;
        void timer_heap_swap(std::size_t left, std::size_t right) noexcept;
        void timer_sift_up(std::size_t position) noexcept;
        void timer_sift_down(std::size_t position) noexcept;
        [[nodiscard]] std::size_t add_timer(
            Task task, std::chrono::steady_clock::time_point deadline) noexcept;
        [[nodiscard]] Task remove_timer(std::size_t position) noexcept;

        mutable std::mutex mutex;
        std::condition_variable ready;
        std::vector<Task> entries;
        std::vector<TimerSlot> timer_slots;
        std::vector<std::size_t> timer_heap;
        std::vector<std::size_t> free_timer_slots;
        std::size_t head = 0;
        std::size_t size = 0;
        std::uint64_t next_timer_order = 1U;
        bool executing = false;
        bool stop_requested = false;
        std::thread worker;
    };

    void run(std::size_t shard_index) noexcept;

    Configuration configuration_;
    std::vector<std::unique_ptr<Shard>> shards_;
    std::mutex lifecycle_mutex_;
    std::atomic_bool accepting_ = false;
    bool start_attempted_ = false;
    std::atomic<std::uint64_t> accepted_ = 0;
    std::atomic<std::uint64_t> completed_ = 0;
    std::atomic<std::uint64_t> rejected_full_ = 0;
    std::atomic<std::uint64_t> rejected_stopped_ = 0;
    std::atomic<std::uint64_t> rejected_invalid_ = 0;
    std::atomic<std::uint64_t> timers_scheduled_ = 0;
    std::atomic<std::uint64_t> timers_canceled_ = 0;
};

} // namespace robotweax::srt::compat
