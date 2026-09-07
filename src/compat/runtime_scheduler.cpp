#include "compat/runtime_scheduler.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace robotweax::srt::compat {

namespace {

constexpr std::size_t invalid_timer_position =
    std::numeric_limits<std::size_t>::max();

} // namespace

RuntimeScheduler::Shard::Shard(
    std::size_t queue_capacity, std::size_t timer_capacity)
    : entries(queue_capacity)
    , timer_slots(timer_capacity)
{
    timer_heap.reserve(timer_capacity);
    free_timer_slots.reserve(timer_capacity);
    for (std::size_t index = timer_capacity; index != 0U; --index) {
        free_timer_slots.push_back(index - 1U);
        timer_slots[index - 1U].heap_position = invalid_timer_position;
    }
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

RuntimeScheduler::RuntimeScheduler(Configuration configuration)
    : configuration_(configuration)
{
    if (configuration_.timer_capacity_per_shard == 0U) {
        configuration_.timer_capacity_per_shard =
            configuration_.queue_capacity_per_shard;
    }
    shards_.reserve(configuration_.shard_count);
    for (std::size_t index = 0; index < configuration_.shard_count; ++index) {
        shards_.push_back(
            std::make_unique<Shard>(configuration_.queue_capacity_per_shard,
                configuration_.timer_capacity_per_shard));
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
        || configuration_.queue_capacity_per_shard == 0U) {
        return false;
    }
    start_attempted_ = true;

    try {
        for (std::size_t index = 0; index < shards_.size(); ++index) {
            shards_[index]->worker = std::thread([this, index] {
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
    shard.ready.notify_one();
    return true;
}

void RuntimeScheduler::stop() noexcept
{
    std::lock_guard lifecycle_lock(lifecycle_mutex_);
    start_attempted_ = true;
    accepting_.store(false, std::memory_order_release);
    for (const auto& shard : shards_) {
        std::unique_lock lock(shard->mutex);
        shard->stop_requested = true;
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
    };
    for (const auto& shard : shards_) {
        std::lock_guard lock(shard->mutex);
        result.queued += shard->size;
        result.timers += shard->timer_heap.size();
        result.executing += shard->executing ? 1U : 0U;
    }
    return result;
}

void RuntimeScheduler::run(std::size_t shard_index) noexcept
{
    Shard& shard = *shards_[shard_index];
    for (;;) {
        Task task;
        {
            std::unique_lock lock(shard.mutex);
            for (;;) {
                if (!shard.timer_heap.empty()) {
                    const std::size_t timer_slot = shard.timer_heap.front();
                    const auto deadline =
                        shard.timer_slots[timer_slot].deadline;
                    if (std::chrono::steady_clock::now() >= deadline) {
                        task = shard.remove_timer(0U);
                        break;
                    }
                }
                if (shard.size != 0U) {
                    task = std::move(shard.entries[shard.head]);
                    shard.entries[shard.head] = {};
                    shard.head = (shard.head + 1U) % shard.entries.size();
                    --shard.size;
                    break;
                }
                if (shard.stop_requested) {
                    return;
                }
                if (shard.timer_heap.empty()) {
                    shard.ready.wait(lock);
                    continue;
                }
                const std::size_t timer_slot = shard.timer_heap.front();
                const auto deadline = shard.timer_slots[timer_slot].deadline;
                shard.ready.wait_until(lock, deadline);
            }
            shard.executing = true;
        }

        task.function(task.context.get());
        {
            std::lock_guard lock(shard.mutex);
            shard.executing = false;
        }
        completed_.fetch_add(1U, std::memory_order_release);
    }
}

} // namespace robotweax::srt::compat
