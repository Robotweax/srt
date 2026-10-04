#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace robotweax::srt::compat {

class SocketReadiness;

// Physical persistent service-table storage, shared across scheduler generations.
// Ordinary queues/timers, allocator bookkeeping and callback metadata are excluded.
class SchedulerServiceStorageBudget {
public:
    struct Snapshot {
        std::size_t bytes = 0;
        std::size_t generations = 0;
    };
    SchedulerServiceStorageBudget(
        std::size_t maximum_bytes, std::size_t maximum_generations) noexcept;
    [[nodiscard]] Snapshot snapshot() const noexcept;

private:
    friend class RuntimeScheduler;
    [[nodiscard]] bool reserve(std::size_t bytes) noexcept;
    void release(std::size_t bytes) noexcept;
    const std::size_t maximum_bytes_;
    const std::size_t maximum_generations_;
    mutable std::mutex mutex_;
    Snapshot reserved_;
};

class RuntimeScheduler {
public:
    using TaskFunction = void (*)(void*) noexcept;

    struct Configuration {
        std::size_t shard_count = 0;
        std::size_t queue_capacity_per_shard = 0;
        std::size_t timer_capacity_per_shard = 0;
        // Independent, preallocated persistent service slots. The production
        // service defaults to zero; internal preview can opt in to bounded slots.
        std::size_t service_capacity_per_shard = 0;
        // Optional shared generation/table budget, reserved before table allocation.
        std::shared_ptr<SchedulerServiceStorageBudget> service_storage_budget;
    };

    [[nodiscard]] static std::optional<std::size_t> service_storage_bytes(
        const Configuration& configuration) noexcept;

    struct Task {
        TaskFunction function = nullptr;
        std::shared_ptr<void> context;
        // Timers can distinguish an idle clock wake from a deadline reached
        // while the shard was executing other work. Immediate jobs retain
        // the ordinary function contract.
        void (*timer_function)(void*,
            std::optional<std::chrono::steady_clock::time_point>) noexcept =
            nullptr;
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

    struct ServiceToken {
        std::uint64_t scope = 0;
        std::size_t shard = 0;
        std::size_t slot = 0;
        std::uint64_t generation = 0;

        [[nodiscard]] constexpr bool valid() const noexcept
        {
            return scope != 0U && generation != 0U;
        }
    };

    struct ServiceResult {
        SubmitStatus status = SubmitStatus::invalid;
        ServiceToken token {};
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
        std::size_t service_capacity = 0;
        // Slot-table bytes; shared-context storage is owned by the client.
        std::size_t service_storage_bytes = 0;
        std::size_t services_reserved = 0;
        std::size_t services_pending = 0;
        std::size_t services_executing = 0;
        std::size_t service_timers = 0;
        std::uint64_t service_wakes = 0;
        std::uint64_t service_coalesced = 0;
        std::uint64_t service_runs = 0;
    };

    explicit RuntimeScheduler(Configuration configuration);
    ~RuntimeScheduler();

    RuntimeScheduler(const RuntimeScheduler&) = delete;
    RuntimeScheduler& operator=(const RuntimeScheduler&) = delete;

    [[nodiscard]] bool start() noexcept;
    // Identifies affinity workers without walking scheduler instances or
    // taking their locks. Blocking cleanup belongs on the work executor.
    [[nodiscard]] static bool on_worker_thread() noexcept;
    [[nodiscard]] SubmitStatus submit(
        std::uint64_t affinity, Task task) noexcept;
    [[nodiscard]] ScheduleResult schedule_at(std::uint64_t affinity,
        std::chrono::steady_clock::time_point deadline, Task task) noexcept;
    [[nodiscard]] bool cancel_timer(TimerToken token) noexcept;
    // Reservation pins the context until retirement, not until each wake.
    // Notifications coalesce and cannot be rejected by ordinary queue/timer
    // saturation. Clients must store work before notifying and bound each turn.
    [[nodiscard]] ServiceResult reserve_service(
        std::uint64_t affinity, Task task) noexcept;
    [[nodiscard]] SubmitStatus notify_service(ServiceToken token) noexcept;
    // One independently reserved deadline per service. Immediate notifications
    // do not remove a future deadline. Due hints coalesce with pending work;
    // callbacks must recheck their own protocol deadlines before effects.
    [[nodiscard]] SubmitStatus schedule_service_at(ServiceToken token,
        std::chrono::steady_clock::time_point deadline) noexcept;
    [[nodiscard]] bool cancel_service_timer(ServiceToken token) noexcept;
    // Nonblocking, including from its callback. A running callback retains the
    // context and prevents slot reuse until it returns. No callback is revoked
    // after dispatch: the client must enforce its own admission/close barrier.
    [[nodiscard]] bool release_service(ServiceToken token) noexcept;
    [[nodiscard]] bool service_quiescent(ServiceToken token) const noexcept;
    // Called outside this scheduler's worker threads. Worker callbacks retire
    // their service nonblockingly; process cleanup uses its existing executor.
    void stop() noexcept;
    [[nodiscard]] std::shared_ptr<SocketReadiness>
    acquire_socket_readiness() noexcept;

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

        struct ServiceSlot {
            Task task;
            std::uint64_t generation = 0;
            bool reserved = false;
            bool pending = false;
            bool executing = false;
            std::optional<std::chrono::steady_clock::time_point> deadline;
        };

        Shard(std::size_t queue_capacity, std::size_t timer_capacity,
            std::size_t service_capacity);

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
        std::vector<ServiceSlot> service_slots;
        std::size_t service_pending = 0;
        std::size_t next_service = 0;
        bool service_turn = true;
        std::size_t head = 0;
        std::size_t size = 0;
        std::uint64_t next_timer_order = 1U;
        bool executing = false;
        bool stop_requested = false;
        std::thread worker;
    };

    void run(std::size_t shard_index) noexcept;

    struct ServiceStorageCredit {
        std::shared_ptr<SchedulerServiceStorageBudget> budget;
        std::size_t bytes = 0;
        ~ServiceStorageCredit()
        {
            if (budget != nullptr)
                budget->release(bytes);
        }
    };
    Configuration configuration_;
    // Reverse destruction frees shard vectors before returning physical credit.
    ServiceStorageCredit service_storage_credit_;
    std::uint64_t service_scope_ = 0;
    std::vector<std::unique_ptr<Shard>> shards_;
    std::mutex lifecycle_mutex_;
    std::condition_variable lifecycle_changed_;
    bool stop_in_progress_ = false;
    bool stop_complete_ = false;
    std::shared_ptr<SocketReadiness> socket_readiness_;
    std::atomic_bool accepting_ = false;
    bool start_attempted_ = false;
    std::atomic<std::uint64_t> accepted_ = 0;
    std::atomic<std::uint64_t> completed_ = 0;
    std::atomic<std::uint64_t> rejected_full_ = 0;
    std::atomic<std::uint64_t> rejected_stopped_ = 0;
    std::atomic<std::uint64_t> rejected_invalid_ = 0;
    std::atomic<std::uint64_t> timers_scheduled_ = 0;
    std::atomic<std::uint64_t> timers_canceled_ = 0;
    std::atomic<std::uint64_t> service_wakes_ = 0;
    std::atomic<std::uint64_t> service_coalesced_ = 0;
    std::atomic<std::uint64_t> service_runs_ = 0;
};

} // namespace robotweax::srt::compat
