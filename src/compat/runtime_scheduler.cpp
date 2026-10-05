#include "compat/runtime_scheduler.hpp"
#include "compat/socket_registry.hpp"
#include "compat/socket_readiness.hpp"

#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace robotweax::srt::compat {

namespace {

constexpr std::size_t invalid_timer_position =
    std::numeric_limits<std::size_t>::max();

thread_local bool scheduler_worker_thread = false;

// Tokens must not alias a slot in a later scheduler instance. Scope exhaustion
// fails service-enabled startup instead of wrapping into a previous lifetime.
std::atomic<std::uint64_t> next_service_scope {0};
std::uint64_t acquire_service_scope() noexcept
{
    auto previous = next_service_scope.load(std::memory_order_relaxed);
    while (previous != (std::numeric_limits<std::uint64_t>::max)()) {
        if (next_service_scope.compare_exchange_weak(
                previous, previous + 1U, std::memory_order_relaxed))
            return previous + 1U;
    }
    return 0;
}

struct SchedulerStopScope;
thread_local SchedulerStopScope* active_stop = nullptr;
struct SchedulerStopScope {
    RuntimeScheduler* owner;
    SchedulerStopScope* previous = active_stop;
    explicit SchedulerStopScope(RuntimeScheduler* scheduler) noexcept
        : owner(scheduler)
    {
        active_stop = this;
    }
    ~SchedulerStopScope()
    {
        active_stop = previous;
    }
};

struct SchedulerWorkerScope {
    bool previous = scheduler_worker_thread;

    SchedulerWorkerScope() noexcept
    {
        scheduler_worker_thread = true;
    }
    ~SchedulerWorkerScope()
    {
        scheduler_worker_thread = previous;
    }
};

} // namespace

RuntimeScheduler::Shard::Shard(std::size_t queue_capacity,
    std::size_t timer_capacity, std::size_t service_capacity)
    : entries(queue_capacity)
    , timer_slots(timer_capacity)
    , service_slots(service_capacity)
{
    timer_heap.reserve(timer_capacity);
    free_timer_slots.reserve(timer_capacity);
    for (std::size_t index = timer_capacity; index != 0U; --index) {
        free_timer_slots.push_back(index - 1U);
        timer_slots[index - 1U].heap_position = invalid_timer_position;
    }
}

void RuntimeScheduler::Shard::trim_service_scan_limit() noexcept
{
    while (service_scan_limit != 0U) {
        const auto& slot = service_slots[service_scan_limit - 1U];
        if (slot.reserved || slot.executing)
            break;
        --service_scan_limit;
    }
    if (next_service >= service_scan_limit)
        next_service = 0U;
}

bool RuntimeScheduler::Shard::timer_less(
    std::size_t left_slot, std::size_t right_slot) const noexcept
{
    const TimerSlot& left = timer_slots[left_slot];
    const TimerSlot& right = timer_slots[right_slot];
    return left.deadline < right.deadline
        || (left.deadline == right.deadline && left.order < right.order);
}

void RuntimeScheduler::Shard::timer_heap_swap(
    std::size_t left, std::size_t right) noexcept
{
    std::swap(timer_heap[left], timer_heap[right]);
    timer_slots[timer_heap[left]].heap_position = left;
    timer_slots[timer_heap[right]].heap_position = right;
}

void RuntimeScheduler::Shard::timer_sift_up(std::size_t position) noexcept
{
    while (position != 0U) {
        const std::size_t parent = (position - 1U) / 2U;
        if (!timer_less(timer_heap[position], timer_heap[parent])) {
            return;
        }
        timer_heap_swap(position, parent);
        position = parent;
    }
}

void RuntimeScheduler::Shard::timer_sift_down(std::size_t position) noexcept
{
    for (;;) {
        const std::size_t left = position * 2U + 1U;
        if (left >= timer_heap.size()) {
            return;
        }
        const std::size_t right = left + 1U;
        std::size_t smallest = left;
        if (right < timer_heap.size()
            && timer_less(timer_heap[right], timer_heap[left])) {
            smallest = right;
        }
        if (!timer_less(timer_heap[smallest], timer_heap[position])) {
            return;
        }
        timer_heap_swap(position, smallest);
        position = smallest;
    }
}

std::size_t RuntimeScheduler::Shard::add_timer(
    Task task, std::chrono::steady_clock::time_point deadline) noexcept
{
    const std::size_t slot_index = free_timer_slots.back();
    free_timer_slots.pop_back();
    TimerSlot& slot = timer_slots[slot_index];
    slot.task = std::move(task);
    slot.deadline = deadline;
    ++slot.generation;
    if (slot.generation == 0U) {
        ++slot.generation;
    }
    slot.order = next_timer_order++;
    if (next_timer_order == 0U) {
        next_timer_order = 1U;
    }
    slot.active = true;
    slot.heap_position = timer_heap.size();
    timer_heap.push_back(slot_index);
    timer_sift_up(slot.heap_position);
    return slot_index;
}

RuntimeScheduler::Task RuntimeScheduler::Shard::remove_timer(
    std::size_t position) noexcept
{
    const std::size_t slot_index = timer_heap[position];
    const std::size_t last = timer_heap.size() - 1U;
    if (position != last) {
        timer_heap_swap(position, last);
    }
    timer_heap.pop_back();
    if (position < timer_heap.size()) {
        if (position != 0U
            && timer_less(
                timer_heap[position], timer_heap[(position - 1U) / 2U])) {
            timer_sift_up(position);
        } else {
            timer_sift_down(position);
        }
    }

    TimerSlot& slot = timer_slots[slot_index];
    Task task = std::move(slot.task);
    slot.task = {};
    slot.active = false;
    slot.heap_position = invalid_timer_position;
    free_timer_slots.push_back(slot_index);
    return task;
}

SchedulerServiceStorageBudget::SchedulerServiceStorageBudget(
    std::size_t maximum_bytes, std::size_t maximum_generations) noexcept
    : maximum_bytes_(maximum_bytes)
    , maximum_generations_(maximum_generations)
{
}

SchedulerServiceStorageBudget::Snapshot
SchedulerServiceStorageBudget::snapshot() const noexcept
{
    std::lock_guard lock(mutex_);
    return reserved_;
}

bool SchedulerServiceStorageBudget::reserve(std::size_t bytes) noexcept
{
    std::lock_guard lock(mutex_);
    if (reserved_.generations >= maximum_generations_
        || bytes > maximum_bytes_ - reserved_.bytes)
        return false;
    reserved_.bytes += bytes;
    ++reserved_.generations;
    return true;
}

void SchedulerServiceStorageBudget::release(std::size_t bytes) noexcept
{
    std::lock_guard lock(mutex_);
    reserved_.bytes -= bytes;
    --reserved_.generations;
}

std::optional<std::size_t> RuntimeScheduler::service_storage_bytes(
    const Configuration& configuration) noexcept
{
    if (configuration.service_capacity_per_shard == 0U)
        return 0U;
    const auto maximum = (std::numeric_limits<std::size_t>::max)();
    if (configuration.shard_count
            > maximum / configuration.service_capacity_per_shard
        || configuration.shard_count * configuration.service_capacity_per_shard
            > maximum / sizeof(Shard::ServiceSlot))
        return std::nullopt;
    return configuration.shard_count * configuration.service_capacity_per_shard
        * sizeof(Shard::ServiceSlot);
}

RuntimeScheduler::RuntimeScheduler(Configuration configuration)
    : configuration_(configuration)
{
    if (configuration_.timer_capacity_per_shard == 0U) {
        configuration_.timer_capacity_per_shard =
            configuration_.queue_capacity_per_shard;
    }
    if (configuration_.service_capacity_per_shard != 0U) {
        const auto bytes = service_storage_bytes(configuration_);
        if (!bytes.has_value())
            throw std::length_error {"scheduler service storage size overflow"};
        if (*bytes != 0U && configuration_.service_storage_budget != nullptr) {
            if (!configuration_.service_storage_budget->reserve(*bytes))
                throw std::bad_alloc {};
            service_storage_credit_.budget =
                configuration_.service_storage_budget;
            service_storage_credit_.bytes = *bytes;
        }
        service_scope_ = acquire_service_scope();
    }
    shards_.reserve(configuration_.shard_count);
    for (std::size_t index = 0; index < configuration_.shard_count; ++index) {
        shards_.push_back(
            std::make_unique<Shard>(configuration_.queue_capacity_per_shard,
                configuration_.timer_capacity_per_shard,
                configuration_.service_capacity_per_shard));
    }
}

RuntimeScheduler::~RuntimeScheduler()
{
    stop();
}

bool RuntimeScheduler::start() noexcept
{
    std::lock_guard lifecycle_lock(lifecycle_mutex_);
    if (accepting_.load(std::memory_order_acquire)) {
        return true;
    }
    if (start_attempted_ || shards_.empty()
        || configuration_.queue_capacity_per_shard == 0U
        || (configuration_.service_capacity_per_shard != 0U
            && service_scope_ == 0U)) {
        return false;
    }
    start_attempted_ = true;

    try {
        for (std::size_t index = 0; index < shards_.size(); ++index) {
            shards_[index]->worker = std::thread([this, index] {
                mark_runtime_cleanup_worker_thread();
                run(index);
            });
        }
    } catch (...) {
        for (const auto& shard : shards_) {
            {
                std::lock_guard lock(shard->mutex);
                shard->stop_requested = true;
            }
            shard->ready.notify_all();
        }
        for (const auto& shard : shards_) {
            if (shard->worker.joinable()) {
                shard->worker.join();
            }
        }
        return false;
    }

    accepting_.store(true, std::memory_order_release);
    return true;
}

RuntimeScheduler::SubmitStatus RuntimeScheduler::submit(
    std::uint64_t affinity, Task task) noexcept
{
    if (task.function == nullptr) {
        rejected_invalid_.fetch_add(1U, std::memory_order_relaxed);
        return SubmitStatus::invalid;
    }
    if (!accepting_.load(std::memory_order_acquire)) {
        rejected_stopped_.fetch_add(1U, std::memory_order_relaxed);
        return SubmitStatus::stopped;
    }

    Shard& shard = *shards_[shard_for(affinity)];
    {
        std::lock_guard lock(shard.mutex);
        if (!accepting_.load(std::memory_order_relaxed)
            || shard.stop_requested) {
            rejected_stopped_.fetch_add(1U, std::memory_order_relaxed);
            return SubmitStatus::stopped;
        }
        if (shard.size == shard.entries.size()) {
            rejected_full_.fetch_add(1U, std::memory_order_relaxed);
            return SubmitStatus::full;
        }
        const std::size_t tail =
            (shard.head + shard.size) % shard.entries.size();
        shard.entries[tail] = std::move(task);
        ++shard.size;
        accepted_.fetch_add(1U, std::memory_order_relaxed);
    }
    shard.ready.notify_one();
    return SubmitStatus::accepted;
}

RuntimeScheduler::ScheduleResult RuntimeScheduler::schedule_at(
    std::uint64_t affinity, std::chrono::steady_clock::time_point deadline,
    Task task) noexcept
{
    if (task.function == nullptr) {
        rejected_invalid_.fetch_add(1U, std::memory_order_relaxed);
        return {.status = SubmitStatus::invalid};
    }
    if (!accepting_.load(std::memory_order_acquire)) {
        rejected_stopped_.fetch_add(1U, std::memory_order_relaxed);
        return {.status = SubmitStatus::stopped};
    }

    const std::size_t shard_index = shard_for(affinity);
    Shard& shard = *shards_[shard_index];
    TimerToken token;
    {
        std::lock_guard lock(shard.mutex);
        if (!accepting_.load(std::memory_order_relaxed)
            || shard.stop_requested) {
            rejected_stopped_.fetch_add(1U, std::memory_order_relaxed);
            return {.status = SubmitStatus::stopped};
        }
        if (shard.free_timer_slots.empty()) {
            rejected_full_.fetch_add(1U, std::memory_order_relaxed);
            return {.status = SubmitStatus::full};
        }
        const std::size_t slot = shard.add_timer(std::move(task), deadline);
        token = {
            .shard = shard_index,
            .slot = slot,
            .generation = shard.timer_slots[slot].generation,
        };
        accepted_.fetch_add(1U, std::memory_order_relaxed);
        timers_scheduled_.fetch_add(1U, std::memory_order_relaxed);
    }
    shard.ready.notify_one();
    return {
        .status = SubmitStatus::accepted,
        .token = token,
    };
}

bool RuntimeScheduler::cancel_timer(TimerToken token) noexcept
{
    if (!token.valid() || token.shard >= shards_.size()) {
        return false;
    }
    Shard& shard = *shards_[token.shard];
    Task canceled;
    {
        std::lock_guard lock(shard.mutex);
        if (token.slot >= shard.timer_slots.size()) {
            return false;
        }
        const Shard::TimerSlot& slot = shard.timer_slots[token.slot];
        if (!slot.active || slot.generation != token.generation) {
            return false;
        }
        canceled = shard.remove_timer(slot.heap_position);
        timers_canceled_.fetch_add(1U, std::memory_order_relaxed);
    }
    // Removing a timer cannot make the next deadline earlier. A worker
    // sleeping until the old deadline may wake early, but cannot miss work.
    // New tasks/timers and stop still signal under their own protocols. In
    // particular, cancel-then-submit must not wake the worker before the new
    // task has even been queued. Keep canceled-context destruction outside
    // the shard lock, as before.
    return true;
}

std::shared_ptr<SocketReadiness>
RuntimeScheduler::acquire_socket_readiness() noexcept
{
    std::lock_guard lock(lifecycle_mutex_);
    if (!accepting_.load(std::memory_order_acquire)) {
        return {};
    }
    if (socket_readiness_ == nullptr) {
        try {
            auto watcher =
                std::make_shared<SocketReadiness>(configuration_.shard_count
                    * configuration_.queue_capacity_per_shard);
            if (!watcher->start()) {
                return {};
            }
            socket_readiness_ = std::move(watcher);
        } catch (...) {
            return {};
        }
    }
    return socket_readiness_;
}

RuntimeScheduler::ServiceResult RuntimeScheduler::reserve_service(
    std::uint64_t affinity, Task task) noexcept
{
    if (task.function == nullptr)
        return {.status = SubmitStatus::invalid};
    if (!accepting_.load(std::memory_order_acquire))
        return {.status = SubmitStatus::stopped};
    const auto shard_index = shard_for(affinity);
    auto& shard = *shards_[shard_index];
    std::lock_guard lock(shard.mutex);
    if (!accepting_.load(std::memory_order_relaxed) || shard.stop_requested)
        return {.status = SubmitStatus::stopped};
    for (std::size_t index = 0; index < shard.service_slots.size(); ++index) {
        auto& slot = shard.service_slots[index];
        if (slot.reserved || slot.executing
            || slot.generation == (std::numeric_limits<std::uint64_t>::max)())
            continue;
        slot.task = std::move(task);
        ++slot.generation;
        slot.reserved = true;
        shard.service_scan_limit =
            std::max(shard.service_scan_limit, index + 1U);
        return {.status = SubmitStatus::accepted,
            .token = {service_scope_, shard_index, index, slot.generation}};
    }
    return {.status = SubmitStatus::full};
}

RuntimeScheduler::SubmitStatus RuntimeScheduler::notify_service(
    ServiceToken token) noexcept
{
    if (!token.valid() || token.scope != service_scope_
        || token.shard >= shards_.size())
        return SubmitStatus::invalid;
    if (!accepting_.load(std::memory_order_acquire))
        return SubmitStatus::stopped;
    auto& shard = *shards_[token.shard];
    {
        std::lock_guard lock(shard.mutex);
        if (!accepting_.load(std::memory_order_relaxed) || shard.stop_requested)
            return SubmitStatus::stopped;
        if (token.slot >= shard.service_slots.size())
            return SubmitStatus::invalid;
        auto& slot = shard.service_slots[token.slot];
        if (!slot.reserved || slot.generation != token.generation)
            return SubmitStatus::invalid;
        service_wakes_.fetch_add(1U, std::memory_order_relaxed);
        if (slot.pending)
            service_coalesced_.fetch_add(1U, std::memory_order_relaxed);
        else {
            slot.pending = true;
            ++shard.service_pending;
        }
    }
    shard.ready.notify_one();
    return SubmitStatus::accepted;
}

RuntimeScheduler::SubmitStatus RuntimeScheduler::schedule_service_at(
    ServiceToken token, std::chrono::steady_clock::time_point deadline) noexcept
{
    if (!token.valid() || token.scope != service_scope_
        || token.shard >= shards_.size())
        return SubmitStatus::invalid;
    if (!accepting_.load(std::memory_order_acquire))
        return SubmitStatus::stopped;
    auto& shard = *shards_[token.shard];
    {
        std::lock_guard lock(shard.mutex);
        if (!accepting_.load(std::memory_order_relaxed) || shard.stop_requested)
            return SubmitStatus::stopped;
        if (token.slot >= shard.service_slots.size())
            return SubmitStatus::invalid;
        auto& slot = shard.service_slots[token.slot];
        if (!slot.reserved || slot.generation != token.generation)
            return SubmitStatus::invalid;
        slot.deadline = deadline;
    }
    shard.ready.notify_one();
    return SubmitStatus::accepted;
}

bool RuntimeScheduler::cancel_service_timer(ServiceToken token) noexcept
{
    if (!token.valid() || token.scope != service_scope_
        || token.shard >= shards_.size())
        return false;
    auto& shard = *shards_[token.shard];
    std::lock_guard lock(shard.mutex);
    if (token.slot >= shard.service_slots.size())
        return false;
    auto& slot = shard.service_slots[token.slot];
    if (!slot.reserved || slot.generation != token.generation
        || !slot.deadline.has_value())
        return false;
    slot.deadline.reset();
    // Cancellation cannot advance a deadline; an early clock wake is harmless.
    return true;
}

bool RuntimeScheduler::release_service(ServiceToken token) noexcept
{
    if (!token.valid() || token.scope != service_scope_
        || token.shard >= shards_.size())
        return false;
    auto& shard = *shards_[token.shard];
    Task retired;
    {
        std::lock_guard lock(shard.mutex);
        if (token.slot >= shard.service_slots.size())
            return false;
        auto& slot = shard.service_slots[token.slot];
        if (!slot.reserved || slot.generation != token.generation)
            return false;
        slot.reserved = false;
        slot.deadline.reset();
        if (slot.pending) {
            slot.pending = false;
            --shard.service_pending;
        }
        if (!slot.executing) {
            retired = std::move(slot.task);
            slot.task = {};
        }
        shard.trim_service_scan_limit();
    }
    // Context destructors may call back into scheduler APIs.
    return true;
}

bool RuntimeScheduler::service_quiescent(ServiceToken token) const noexcept
{
    if (!token.valid() || token.scope != service_scope_
        || token.shard >= shards_.size())
        return false;
    const auto& shard = *shards_[token.shard];
    std::lock_guard lock(shard.mutex);
    if (token.slot >= shard.service_slots.size())
        return false;
    const auto& slot = shard.service_slots[token.slot];
    return slot.generation != token.generation
        || (!slot.reserved && !slot.executing);
}

void RuntimeScheduler::stop() noexcept
{
    // A context destructor can reenter stop on this same thread. The outer
    // stop still owns completion; recursively waiting would wait on itself.
    for (auto* scope = active_stop; scope != nullptr; scope = scope->previous)
        if (scope->owner == this)
            return;
    const SchedulerStopScope stop_scope {this};
    std::unique_lock lifecycle_lock(lifecycle_mutex_);
    lifecycle_changed_.wait(lifecycle_lock, [this] {
        return !stop_in_progress_;
    });
    if (stop_complete_)
        return;
    stop_in_progress_ = true;
    start_attempted_ = true;
    accepting_.store(false, std::memory_order_release);
    const auto readiness = socket_readiness_;
    lifecycle_lock.unlock();
    // Joining and context destruction must not hold the lifecycle mutex:
    // callbacks/destructors can take it to observe fail-closed startup.
    if (readiness != nullptr)
        readiness->stop();
    for (const auto& shard : shards_) {
        std::unique_lock lock(shard->mutex);
        shard->stop_requested = true;
        // Admission closes immediately. Future service turns are canceled;
        // executing callbacks retain their copied context until return.
        for (auto& slot : shard->service_slots) {
            slot.reserved = false;
            slot.pending = false;
            slot.deadline.reset();
        }
        shard->service_pending = 0;
        shard->trim_service_scan_limit();
        while (!shard->timer_heap.empty()) {
            Task canceled = shard->remove_timer(0U);
            timers_canceled_.fetch_add(1U, std::memory_order_relaxed);
            lock.unlock();
            canceled = {};
            lock.lock();
        }
        lock.unlock();
        shard->ready.notify_all();
    }
    for (const auto& shard : shards_) {
        if (shard->worker.joinable()) {
            shard->worker.join();
        }
    }
    for (const auto& shard : shards_) {
        for (std::size_t index = 0; index < shard->service_slots.size();
            ++index) {
            Task retired;
            {
                std::lock_guard lock(shard->mutex);
                retired = std::move(shard->service_slots[index].task);
                shard->service_slots[index].task = {};
            }
        }
    }
    lifecycle_lock.lock();
    stop_complete_ = true;
    stop_in_progress_ = false;
    lifecycle_lock.unlock();
    lifecycle_changed_.notify_all();
}

std::size_t RuntimeScheduler::shard_for(std::uint64_t affinity) const noexcept
{
    if (shards_.empty()) {
        return 0U;
    }
    return static_cast<std::size_t>(
        affinity % static_cast<std::uint64_t>(shards_.size()));
}

RuntimeScheduler::Snapshot RuntimeScheduler::snapshot() const noexcept
{
    Snapshot result {
        .shard_count = configuration_.shard_count,
        .queue_capacity = configuration_.shard_count
            * configuration_.queue_capacity_per_shard,
        .timer_capacity = configuration_.shard_count
            * configuration_.timer_capacity_per_shard,
        .accepted = accepted_.load(std::memory_order_relaxed),
        .completed = completed_.load(std::memory_order_acquire),
        .rejected_full = rejected_full_.load(std::memory_order_relaxed),
        .rejected_stopped = rejected_stopped_.load(std::memory_order_relaxed),
        .rejected_invalid = rejected_invalid_.load(std::memory_order_relaxed),
        .timers_scheduled = timers_scheduled_.load(std::memory_order_relaxed),
        .timers_canceled = timers_canceled_.load(std::memory_order_relaxed),
        .accepting = accepting_.load(std::memory_order_acquire),
        .service_capacity = configuration_.shard_count
            * configuration_.service_capacity_per_shard,
        .service_storage_bytes = configuration_.shard_count
            * configuration_.service_capacity_per_shard
            * sizeof(Shard::ServiceSlot),
        .service_wakes = service_wakes_.load(std::memory_order_relaxed),
        .service_coalesced = service_coalesced_.load(std::memory_order_relaxed),
        .service_runs = service_runs_.load(std::memory_order_acquire),
    };
    for (const auto& shard : shards_) {
        std::lock_guard lock(shard->mutex);
        result.queued += shard->size;
        result.service_slot_inspections += shard->service_slot_inspections;
        result.timers += shard->timer_heap.size();
        result.executing += shard->executing ? 1U : 0U;
        for (const auto& slot : shard->service_slots) {
            result.services_reserved += slot.reserved ? 1U : 0U;
            result.services_pending += slot.pending ? 1U : 0U;
            result.services_executing += slot.executing ? 1U : 0U;
            result.service_timers += slot.deadline.has_value() ? 1U : 0U;
        }
    }
    return result;
}

bool RuntimeScheduler::on_worker_thread() noexcept
{
    return scheduler_worker_thread;
}

void RuntimeScheduler::run(std::size_t shard_index) noexcept
{
    const SchedulerWorkerScope worker_scope;
    Shard& shard = *shards_[shard_index];
    std::optional<std::chrono::steady_clock::time_point> idle_timer_wake;
    std::uint64_t idle_timer_order_limit = 0;
    for (;;) {
        Task task;
        bool timer_dispatch = false;
        std::optional<std::size_t> service_dispatch;
        std::optional<std::chrono::steady_clock::time_point> timer_wake;
        {
            std::unique_lock lock(shard.mutex);
            for (;;) {
                std::optional<std::chrono::steady_clock::time_point>
                    wake_deadline;
                std::optional<std::chrono::steady_clock::time_point>
                    service_now;
                for (std::size_t index = 0; index < shard.service_scan_limit;
                    ++index) {
                    auto& slot = shard.service_slots[index];
                    ++shard.service_slot_inspections;
                    if (!slot.deadline.has_value())
                        continue;
                    if (!service_now.has_value())
                        service_now = std::chrono::steady_clock::now();
                    if (*service_now >= *slot.deadline) {
                        slot.deadline.reset();
                        if (!slot.pending) {
                            slot.pending = true;
                            ++shard.service_pending;
                        }
                    } else if (!wake_deadline.has_value()
                        || *slot.deadline < *wake_deadline)
                        wake_deadline = slot.deadline;
                }
                const bool timer_due = shard.service_pending != 0U
                    && !shard.timer_heap.empty()
                    && std::chrono::steady_clock::now()
                        >= shard.timer_slots[shard.timer_heap.front()].deadline;
                // Alternate a service turn with ordinary work when both are
                // ready. With no pending service, legacy timer/FIFO order holds.
                if (shard.service_pending != 0U
                    && (shard.service_turn
                        || (!timer_due && shard.size == 0U))) {
                    for (std::size_t count = 0;
                        count < shard.service_scan_limit; ++count) {
                        ++shard.service_slot_inspections;
                        const auto index = shard.next_service;
                        shard.next_service =
                            (index + 1U) % shard.service_scan_limit;
                        auto& slot = shard.service_slots[index];
                        if (!slot.pending)
                            continue;
                        slot.pending = false;
                        slot.executing = true;
                        --shard.service_pending;
                        task = slot.task;
                        service_dispatch = index;
                        shard.service_turn = false;
                        idle_timer_wake.reset();
                        break;
                    }
                    break;
                }
                if (!shard.timer_heap.empty()) {
                    const std::size_t timer_slot = shard.timer_heap.front();
                    const auto deadline =
                        shard.timer_slots[timer_slot].deadline;
                    if (std::chrono::steady_clock::now() >= deadline) {
                        // Reuse the same idle wake for the timers already due
                        // at that wake. Time spent in preceding callbacks is
                        // not a measurement of the host timer resolution.
                        if (idle_timer_wake.has_value()
                            && deadline <= *idle_timer_wake
                            && shard.timer_slots[timer_slot].order
                                < idle_timer_order_limit
                            && shard.next_timer_order
                                >= idle_timer_order_limit) {
                            timer_wake = idle_timer_wake;
                        } else {
                            idle_timer_wake.reset();
                        }
                        task = shard.remove_timer(0U);
                        shard.service_turn = true;
                        timer_dispatch = true;
                        break;
                    }
                }
                if (shard.size != 0U) {
                    idle_timer_wake.reset();
                    task = std::move(shard.entries[shard.head]);
                    shard.entries[shard.head] = {};
                    shard.head = (shard.head + 1U) % shard.entries.size();
                    --shard.size;
                    shard.service_turn = true;
                    break;
                }
                if (shard.stop_requested) {
                    return;
                }
                if (!shard.timer_heap.empty()) {
                    const auto deadline =
                        shard.timer_slots[shard.timer_heap.front()].deadline;
                    if (!wake_deadline.has_value() || deadline < *wake_deadline)
                        wake_deadline = deadline;
                }
                if (!wake_deadline.has_value()) {
                    idle_timer_wake.reset();
                    shard.ready.wait(lock);
                    continue;
                }
                shard.ready.wait_until(lock, *wake_deadline);
                const auto woke_at = std::chrono::steady_clock::now();
                idle_timer_wake = woke_at >= *wake_deadline
                    ? std::optional {woke_at}
                    : std::nullopt;
                // A callback can install an already overdue timer. That new
                // timer was not present at this wake and must not inherit it.
                // Counter wrap conservatively invalidates the observation.
                idle_timer_order_limit = shard.next_timer_order;
            }
            shard.executing = true;
        }

        if (timer_dispatch && task.timer_function != nullptr) {
            task.timer_function(task.context.get(), timer_wake);
        } else {
            task.function(task.context.get());
        }
        Task retired;
        {
            std::lock_guard lock(shard.mutex);
            shard.executing = false;
            if (service_dispatch.has_value()) {
                auto& slot = shard.service_slots[*service_dispatch];
                slot.executing = false;
                if (!slot.reserved) {
                    retired = std::move(slot.task);
                    slot.task = {};
                    shard.trim_service_scan_limit();
                }
            }
        }
        if (service_dispatch.has_value())
            service_runs_.fetch_add(1U, std::memory_order_release);
        else
            completed_.fetch_add(1U, std::memory_order_release);
    }
}

} // namespace robotweax::srt::compat
