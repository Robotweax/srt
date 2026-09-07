#include "test.hpp"

#include "compat/runtime_scheduler.hpp"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

using namespace robotweax::srt::compat;

namespace {

struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    bool started = false;
    bool released = false;
};

void wait_at_gate(void* context) noexcept
{
    auto& gate = *static_cast<Gate*>(context);
    std::unique_lock lock(gate.mutex);
    gate.started = true;
    gate.changed.notify_all();
    gate.changed.wait(lock, [&gate] {
        return gate.released;
    });
}

void release_gate(const std::shared_ptr<Gate>& gate)
{
    {
        std::lock_guard lock(gate->mutex);
        gate->released = true;
    }
    gate->changed.notify_all();
}

struct GateRelease {
    std::shared_ptr<Gate> gate;

    ~GateRelease()
    {
        release_gate(gate);
    }
};

void wait_until_started(const std::shared_ptr<Gate>& gate)
{
    std::unique_lock lock(gate->mutex);
    REQUIRE(gate->changed.wait_for(lock, std::chrono::seconds {2}, [&gate] {
        return gate->started;
    }));
}

struct OrderedRecord {
    std::mutex mutex;
    std::condition_variable changed;
    std::array<int, 2> values {};
    std::array<std::thread::id, 2> threads {};
    std::size_t size = 0;
};

struct OrderedTask {
    std::shared_ptr<OrderedRecord> record;
    int value = 0;
};

void record_order(void* context) noexcept
{
    auto& task = *static_cast<OrderedTask*>(context);
    std::lock_guard lock(task.record->mutex);
    const std::size_t index = task.record->size;
    if (index < task.record->values.size()) {
        task.record->values[index] = task.value;
        task.record->threads[index] = std::this_thread::get_id();
        ++task.record->size;
    }
    task.record->changed.notify_all();
}

struct Completion {
    std::mutex mutex;
    std::condition_variable changed;
    bool complete = false;
    std::thread::id thread;
};

void record_completion(void* context) noexcept
{
    auto& completion = *static_cast<Completion*>(context);
    {
        std::lock_guard lock(completion.mutex);
        completion.complete = true;
        completion.thread = std::this_thread::get_id();
    }
    completion.changed.notify_all();
}

void wait_until_complete(const std::shared_ptr<Completion>& completion)
{
    std::unique_lock lock(completion->mutex);
    REQUIRE(completion->changed.wait_for(
        lock, std::chrono::seconds {2}, [&completion] {
            return completion->complete;
        }));
}

void wait_until_completed(
    const RuntimeScheduler& scheduler, std::uint64_t expected)
{
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds {2};
    while (scheduler.snapshot().completed < expected
        && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    REQUIRE(scheduler.snapshot().completed >= expected);
}

} // namespace

TEST(compat_runtime_scheduler_rejects_invalid_configuration_and_tasks)
{
    RuntimeScheduler empty({
        .shard_count = 0,
        .queue_capacity_per_shard = 4,
    });
    REQUIRE(!empty.start());
    REQUIRE_EQ(empty.submit(1, {}), RuntimeScheduler::SubmitStatus::invalid);
    REQUIRE_EQ(
        empty.schedule_at(1, std::chrono::steady_clock::now(), {}).status,
        RuntimeScheduler::SubmitStatus::invalid);

    RuntimeScheduler scheduler({
        .shard_count = 1,
        .queue_capacity_per_shard = 1,
    });
    REQUIRE_EQ(scheduler.submit(1,
                   {
                       .function = record_completion,
                       .context = std::make_shared<Completion>(),
                   }),
        RuntimeScheduler::SubmitStatus::stopped);
    REQUIRE_EQ(scheduler
                   .schedule_at(1, std::chrono::steady_clock::now(),
                       {
                           .function = record_completion,
                           .context = std::make_shared<Completion>(),
                       })
                   .status,
        RuntimeScheduler::SubmitStatus::stopped);
    REQUIRE(scheduler.start());
    REQUIRE(scheduler.start());
    scheduler.stop();
    REQUIRE(!scheduler.start());

    RuntimeScheduler stopped_before_start({
        .shard_count = 1,
        .queue_capacity_per_shard = 1,
    });
    stopped_before_start.stop();
    REQUIRE(!stopped_before_start.start());

    const auto snapshot = scheduler.snapshot();
    REQUIRE_EQ(snapshot.shard_count, 1U);
    REQUIRE_EQ(snapshot.queue_capacity, 1U);
    REQUIRE_EQ(snapshot.rejected_stopped, 2U);
    REQUIRE(!snapshot.accepting);
}

TEST(compat_runtime_scheduler_bounds_cancels_and_runs_affine_timers)
{
    RuntimeScheduler scheduler({
        .shard_count = 2,
        .queue_capacity_per_shard = 2,
        .timer_capacity_per_shard = 2,
    });
    REQUIRE(scheduler.start());

    auto canceled = std::make_shared<Completion>();
    std::weak_ptr<Completion> canceled_lifetime = canceled;
    const auto canceled_timer = scheduler.schedule_at(2,
        std::chrono::steady_clock::now() + std::chrono::seconds {30},
        {
            .function = record_completion,
            .context = canceled,
        });
    REQUIRE_EQ(canceled_timer.status, RuntimeScheduler::SubmitStatus::accepted);
    canceled.reset();
    REQUIRE(!canceled_lifetime.expired());

    const auto due = std::make_shared<Completion>();
    const auto earliest =
        std::chrono::steady_clock::now() + std::chrono::milliseconds {40};
    const auto due_timer = scheduler.schedule_at(2, earliest,
        {
            .function = record_completion,
            .context = due,
        });
    REQUIRE_EQ(due_timer.status, RuntimeScheduler::SubmitStatus::accepted);
    REQUIRE_EQ(scheduler
                   .schedule_at(2, earliest,
                       {
                           .function = record_completion,
                           .context = due,
                       })
                   .status,
        RuntimeScheduler::SubmitStatus::full);
    REQUIRE(scheduler.cancel_timer(canceled_timer.token));
    REQUIRE(!scheduler.cancel_timer(canceled_timer.token));
    REQUIRE(canceled_lifetime.expired());

    wait_until_complete(due);
    REQUIRE(std::chrono::steady_clock::now() >= earliest);
    const auto immediate = std::make_shared<Completion>();
    REQUIRE_EQ(scheduler.submit(2,
                   {
                       .function = record_completion,
                       .context = immediate,
                   }),
        RuntimeScheduler::SubmitStatus::accepted);
    wait_until_complete(immediate);
    REQUIRE_EQ(due->thread, immediate->thread);

    scheduler.stop();
    const auto snapshot = scheduler.snapshot();
    REQUIRE_EQ(snapshot.timer_capacity, 4U);
    REQUIRE_EQ(snapshot.timers, 0U);
    REQUIRE_EQ(snapshot.timers_scheduled, 2U);
    REQUIRE_EQ(snapshot.timers_canceled, 1U);
    REQUIRE_EQ(snapshot.accepted, 3U);
    REQUIRE_EQ(snapshot.completed, 2U);
    REQUIRE_EQ(snapshot.rejected_full, 1U);
}

TEST(compat_runtime_scheduler_stop_cancels_future_timers_without_waiting)
{
    RuntimeScheduler scheduler({
        .shard_count = 1,
        .queue_capacity_per_shard = 1,
        .timer_capacity_per_shard = 1,
    });
    REQUIRE(scheduler.start());
    auto completion = std::make_shared<Completion>();
    std::weak_ptr<Completion> lifetime = completion;
    REQUIRE_EQ(
        scheduler
            .schedule_at(0,
                std::chrono::steady_clock::now() + std::chrono::seconds {30},
                {
                    .function = record_completion,
                    .context = completion,
                })
            .status,
        RuntimeScheduler::SubmitStatus::accepted);
    completion.reset();

    const auto started = std::chrono::steady_clock::now();
    scheduler.stop();
    REQUIRE(
        std::chrono::steady_clock::now() - started < std::chrono::seconds {1});
    REQUIRE(lifetime.expired());
    const auto snapshot = scheduler.snapshot();
    REQUIRE_EQ(snapshot.completed, 0U);
    REQUIRE_EQ(snapshot.timers_canceled, 1U);
}

TEST(compat_runtime_scheduler_preserves_equal_deadline_timer_order)
{
    RuntimeScheduler scheduler({
        .shard_count = 1,
        .queue_capacity_per_shard = 1,
        .timer_capacity_per_shard = 2,
    });
    REQUIRE(scheduler.start());
    const auto ordered = std::make_shared<OrderedRecord>();
    const auto first = std::make_shared<OrderedTask>(
        OrderedTask {.record = ordered, .value = 10});
    const auto second = std::make_shared<OrderedTask>(
        OrderedTask {.record = ordered, .value = 20});
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds {30};
    REQUIRE_EQ(scheduler
                   .schedule_at(0U, deadline,
                       {
                           .function = record_order,
                           .context = first,
                       })
                   .status,
        RuntimeScheduler::SubmitStatus::accepted);
    REQUIRE_EQ(scheduler
                   .schedule_at(0U, deadline,
                       {
                           .function = record_order,
                           .context = second,
                       })
                   .status,
        RuntimeScheduler::SubmitStatus::accepted);
    {
        std::unique_lock lock(ordered->mutex);
        REQUIRE(ordered->changed.wait_for(
            lock, std::chrono::seconds {2}, [&ordered] {
                return ordered->size == 2U;
            }));
    }
    scheduler.stop();
    REQUIRE_EQ(ordered->values[0], 10);
    REQUIRE_EQ(ordered->values[1], 20);
    REQUIRE_EQ(ordered->threads[0], ordered->threads[1]);
}

TEST(compat_runtime_scheduler_bounds_and_serializes_affinity_shards)
{
    RuntimeScheduler scheduler({
        .shard_count = 2,
        .queue_capacity_per_shard = 2,
    });
    REQUIRE(scheduler.start());
    REQUIRE_EQ(scheduler.shard_for(0), 0U);
    REQUIRE_EQ(scheduler.shard_for(1), 1U);
    REQUIRE_EQ(scheduler.shard_for(2), 0U);

    const auto gate = std::make_shared<Gate>();
    REQUIRE_EQ(scheduler.submit(2,
                   {
                       .function = wait_at_gate,
                       .context = gate,
                   }),
        RuntimeScheduler::SubmitStatus::accepted);
    wait_until_started(gate);
    const GateRelease release_on_exit {gate};

    const auto ordered = std::make_shared<OrderedRecord>();
    const auto first = std::make_shared<OrderedTask>(
        OrderedTask {.record = ordered, .value = 10});
    const auto second = std::make_shared<OrderedTask>(
        OrderedTask {.record = ordered, .value = 20});
    REQUIRE_EQ(scheduler.submit(2,
                   {
                       .function = record_order,
                       .context = first,
                   }),
        RuntimeScheduler::SubmitStatus::accepted);
    REQUIRE_EQ(scheduler.submit(2,
                   {
                       .function = record_order,
                       .context = second,
                   }),
        RuntimeScheduler::SubmitStatus::accepted);
    REQUIRE_EQ(scheduler.submit(2,
                   {
                       .function = record_order,
                       .context = first,
                   }),
        RuntimeScheduler::SubmitStatus::full);

    const auto independent = std::make_shared<Completion>();
    REQUIRE_EQ(scheduler.submit(1,
                   {
                       .function = record_completion,
                       .context = independent,
                   }),
        RuntimeScheduler::SubmitStatus::accepted);
    wait_until_complete(independent);
    wait_until_completed(scheduler, 1U);

    const auto blocked = scheduler.snapshot();
    REQUIRE_EQ(blocked.queued, 2U);
    REQUIRE_EQ(blocked.executing, 1U);
    REQUIRE_EQ(blocked.rejected_full, 1U);

    release_gate(gate);
    scheduler.stop();

    const auto complete = scheduler.snapshot();
    REQUIRE_EQ(complete.accepted, 4U);
    REQUIRE_EQ(complete.completed, 4U);
    REQUIRE_EQ(complete.queued, 0U);
    REQUIRE_EQ(complete.executing, 0U);
    REQUIRE_EQ(ordered->size, 2U);
    REQUIRE_EQ(ordered->values[0], 10);
    REQUIRE_EQ(ordered->values[1], 20);
    REQUIRE_EQ(ordered->threads[0], ordered->threads[1]);
    REQUIRE(ordered->threads[0] != independent->thread);
}

TEST(compat_runtime_scheduler_drains_owned_tasks_before_stop_returns)
{
    RuntimeScheduler scheduler({
        .shard_count = 1,
        .queue_capacity_per_shard = 1,
    });
    REQUIRE(scheduler.start());
    const auto gate = std::make_shared<Gate>();
    REQUIRE_EQ(scheduler.submit(7,
                   {
                       .function = wait_at_gate,
                       .context = gate,
                   }),
        RuntimeScheduler::SubmitStatus::accepted);
    wait_until_started(gate);
    const GateRelease release_on_exit {gate};

    auto completion = std::make_shared<Completion>();
    std::weak_ptr<Completion> lifetime = completion;
    REQUIRE_EQ(scheduler.submit(7,
                   {
                       .function = record_completion,
                       .context = completion,
                   }),
        RuntimeScheduler::SubmitStatus::accepted);
    completion.reset();
    REQUIRE(!lifetime.expired());

    release_gate(gate);
    scheduler.stop();
    REQUIRE(lifetime.expired());
    REQUIRE_EQ(scheduler.submit(7,
                   {
                       .function = record_completion,
                       .context = std::make_shared<Completion>(),
                   }),
        RuntimeScheduler::SubmitStatus::stopped);
    const auto snapshot = scheduler.snapshot();
    REQUIRE_EQ(snapshot.completed, 2U);
    REQUIRE_EQ(snapshot.rejected_stopped, 1U);
}
