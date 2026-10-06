#include "test.hpp"

#include "compat/runtime_scheduler.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <limits>
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

struct TimerCompletion : Completion {
    bool timer_callback = false;
    bool idle_wake = false;
};

void record_regular_timer_completion(void* context) noexcept
{
    auto& completion = *static_cast<TimerCompletion*>(context);
    record_completion(static_cast<Completion*>(&completion));
}

void record_timer_completion(void* context,
    std::optional<std::chrono::steady_clock::time_point> wake) noexcept
{
    auto& completion = *static_cast<TimerCompletion*>(context);
    {
        std::lock_guard lock(completion.mutex);
        completion.timer_callback = true;
        completion.idle_wake = wake.has_value();
        completion.complete = true;
    }
    completion.changed.notify_all();
}

void wait_at_timer_gate(void* context,
    std::optional<std::chrono::steady_clock::time_point>) noexcept
{
    wait_at_gate(context);
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

TEST(compat_runtime_scheduler_does_not_report_busy_dispatch_as_idle_timer_wake)
{
    RuntimeScheduler scheduler({1, 4, 4});
    REQUIRE(scheduler.start());
    const auto gate = std::make_shared<Gate>();
    GateRelease release {gate};
    REQUIRE_EQ(scheduler.submit(0, {wait_at_gate, gate}),
        RuntimeScheduler::SubmitStatus::accepted);
    wait_until_started(gate);
    const auto timer = std::make_shared<TimerCompletion>();
    // The deadline is already due while the shard is inside another job.
    // Releasing the gate needs no sleep or assumption about host timing.
    REQUIRE_EQ(scheduler
                   .schedule_at(0, std::chrono::steady_clock::now(),
                       {record_regular_timer_completion, timer,
                           record_timer_completion})
                   .status,
        RuntimeScheduler::SubmitStatus::accepted);
    const auto immediate = std::make_shared<TimerCompletion>();
    REQUIRE_EQ(scheduler.submit(0,
                   {record_regular_timer_completion, immediate,
                       record_timer_completion}),
        RuntimeScheduler::SubmitStatus::accepted);
    release_gate(gate);
    wait_until_complete(timer);
    wait_until_complete(immediate);
    REQUIRE(timer->timer_callback);
    REQUIRE(!timer->idle_wake);
    REQUIRE(!immediate->timer_callback);
    scheduler.stop();
}

TEST(compat_runtime_scheduler_new_overdue_timer_cannot_inherit_an_old_idle_wake)
{
    RuntimeScheduler scheduler({1, 4, 4});
    REQUIRE(scheduler.start());
    const auto gate = std::make_shared<Gate>();
    GateRelease release {gate};
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds {5};
    REQUIRE_EQ(
        scheduler
            .schedule_at(0, deadline, {wait_at_gate, gate, wait_at_timer_gate})
            .status,
        RuntimeScheduler::SubmitStatus::accepted);
    wait_until_started(gate);
    const auto timer = std::make_shared<TimerCompletion>();
    // Regardless of whether the host waited for the first timer, this timer
    // was inserted during its callback, after any recorded idle wake.
    REQUIRE_EQ(scheduler
                   .schedule_at(0, deadline,
                       {record_regular_timer_completion, timer,
                           record_timer_completion})
                   .status,
        RuntimeScheduler::SubmitStatus::accepted);
    release_gate(gate);
    wait_until_complete(timer);
    REQUIRE(timer->timer_callback);
    REQUIRE(!timer->idle_wake);
    scheduler.stop();
}

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

TEST(compat_runtime_scheduler_canceling_last_timer_preserves_new_work_and_stop)
{
    for (const bool use_timer : {false, true}) {
        RuntimeScheduler scheduler({
            .shard_count = 1,
            .queue_capacity_per_shard = 1,
            .timer_capacity_per_shard = 1,
        });
        REQUIRE(scheduler.start());
        auto canceled = std::make_shared<Completion>();
        std::weak_ptr<Completion> lifetime = canceled;
        const auto old = scheduler.schedule_at(0,
            std::chrono::steady_clock::now() + std::chrono::seconds {30},
            {.function = record_completion, .context = canceled});
        REQUIRE_EQ(old.status, RuntimeScheduler::SubmitStatus::accepted);
        canceled.reset();
        REQUIRE(scheduler.cancel_timer(old.token));
        REQUIRE(lifetime.expired());

        const auto completion = std::make_shared<Completion>();
        if (use_timer) {
            const auto deadline = std::chrono::steady_clock::now()
                + std::chrono::milliseconds {20};
            const auto next = scheduler.schedule_at(0, deadline,
                {.function = record_completion, .context = completion});
            REQUIRE_EQ(next.status, RuntimeScheduler::SubmitStatus::accepted);
            REQUIRE_EQ(next.token.slot, old.token.slot);
            REQUIRE(next.token.generation != old.token.generation);
            REQUIRE(!scheduler.cancel_timer(old.token));
            wait_until_complete(completion);
            REQUIRE(std::chrono::steady_clock::now() >= deadline);
        } else {
            REQUIRE_EQ(
                scheduler.submit(
                    0, {.function = record_completion, .context = completion}),
                RuntimeScheduler::SubmitStatus::accepted);
            wait_until_complete(completion);
        }

        const auto last = scheduler.schedule_at(0,
            std::chrono::steady_clock::now() + std::chrono::seconds {30},
            {.function = record_completion, .context = completion});
        REQUIRE_EQ(last.status, RuntimeScheduler::SubmitStatus::accepted);
        REQUIRE(scheduler.cancel_timer(last.token));
        const auto before_stop = std::chrono::steady_clock::now();
        scheduler.stop();
        REQUIRE(std::chrono::steady_clock::now() - before_stop
            < std::chrono::seconds {1});
        REQUIRE_EQ(scheduler.snapshot().completed, 1U);
    }
}

TEST(compat_runtime_scheduler_canceling_head_preserves_successor_deadline)
{
    RuntimeScheduler scheduler({
        .shard_count = 1,
        .queue_capacity_per_shard = 1,
        .timer_capacity_per_shard = 2,
    });
    REQUIRE(scheduler.start());
    const auto gate = std::make_shared<Gate>();
    REQUIRE_EQ(scheduler.submit(0, {.function = wait_at_gate, .context = gate}),
        RuntimeScheduler::SubmitStatus::accepted);
    wait_until_started(gate);
    const GateRelease release_on_exit {gate};
    const auto now = std::chrono::steady_clock::now();
    const auto canceled = std::make_shared<Completion>();
    const auto first =
        scheduler.schedule_at(0, now + std::chrono::milliseconds {40},
            {.function = record_completion, .context = canceled});
    REQUIRE_EQ(first.status, RuntimeScheduler::SubmitStatus::accepted);
    const auto due = std::make_shared<Completion>();
    const auto deadline = now + std::chrono::milliseconds {80};
    REQUIRE_EQ(scheduler
                   .schedule_at(0, deadline,
                       {.function = record_completion, .context = due})
                   .status,
        RuntimeScheduler::SubmitStatus::accepted);
    REQUIRE(scheduler.cancel_timer(first.token));
    release_gate(gate);
    wait_until_complete(due);
    REQUIRE(std::chrono::steady_clock::now() >= deadline);
    scheduler.stop();
    REQUIRE(!canceled->complete);
    REQUIRE_EQ(scheduler.snapshot().completed, 2U);
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

TEST(compat_runtime_scheduler_service_wakes_survive_full_queue_and_timer_pool)
{
    RuntimeScheduler scheduler({2, 1, 1, 1});
    REQUIRE(scheduler.start());
    const auto gate = std::make_shared<Gate>();
    const GateRelease release {gate};
    REQUIRE_EQ(scheduler.submit(0, {wait_at_gate, gate}),
        RuntimeScheduler::SubmitStatus::accepted);
    wait_until_started(gate);
    const auto ordinary = std::make_shared<Completion>();
    REQUIRE_EQ(scheduler.submit(0, {record_completion, ordinary}),
        RuntimeScheduler::SubmitStatus::accepted);
    REQUIRE_EQ(scheduler.submit(0, {record_completion, ordinary}),
        RuntimeScheduler::SubmitStatus::full);
    const auto future = scheduler.schedule_at(0,
        std::chrono::steady_clock::now() + std::chrono::hours {1},
        {record_completion, ordinary});
    REQUIRE_EQ(future.status, RuntimeScheduler::SubmitStatus::accepted);
    REQUIRE_EQ(scheduler
                   .schedule_at(0, std::chrono::steady_clock::now(),
                       {record_completion, ordinary})
                   .status,
        RuntimeScheduler::SubmitStatus::full);
    const auto service = std::make_shared<Completion>();
    const auto reservation =
        scheduler.reserve_service(0, {record_completion, service});
    REQUIRE_EQ(reservation.status, RuntimeScheduler::SubmitStatus::accepted);
    REQUIRE_EQ(
        scheduler.reserve_service(0, {record_completion, service}).status,
        RuntimeScheduler::SubmitStatus::full);
    REQUIRE_EQ(scheduler.notify_service(reservation.token),
        RuntimeScheduler::SubmitStatus::accepted);
    REQUIRE_EQ(scheduler.notify_service(reservation.token),
        RuntimeScheduler::SubmitStatus::accepted);
    const auto independent = std::make_shared<Completion>();
    const auto other =
        scheduler.reserve_service(1, {record_completion, independent});
    REQUIRE_EQ(other.status, RuntimeScheduler::SubmitStatus::accepted);
    REQUIRE_EQ(scheduler.notify_service(other.token),
        RuntimeScheduler::SubmitStatus::accepted);
    wait_until_complete(independent);
    REQUIRE(independent->thread != std::this_thread::get_id());
    release_gate(gate);
    wait_until_complete(service);
    wait_until_complete(ordinary);
    REQUIRE_EQ(service->thread, ordinary->thread);
    REQUIRE(service->thread != independent->thread);
    REQUIRE(scheduler.release_service(reservation.token));
    REQUIRE(scheduler.release_service(other.token));
    scheduler.stop();
    const auto snapshot = scheduler.snapshot();
    REQUIRE_EQ(snapshot.service_capacity, 2U);
    REQUIRE(snapshot.service_storage_bytes > 0U);
    REQUIRE_EQ(snapshot.service_wakes, 3U);
    REQUIRE_EQ(snapshot.service_coalesced, 1U);
    REQUIRE_EQ(snapshot.service_runs, 2U);
    REQUIRE_EQ(snapshot.completed, 2U);
    REQUIRE_EQ(snapshot.services_reserved, 0U);
    REQUIRE_EQ(snapshot.services_pending, 0U);
    REQUIRE_EQ(snapshot.services_executing, 0U);
}

TEST(
    compat_runtime_scheduler_service_releases_are_nonblocking_and_delay_slot_reuse)
{
    RuntimeScheduler scheduler({1, 1, 1, 1});
    REQUIRE(scheduler.start());
    auto gate = std::make_shared<Gate>();
    const GateRelease release {gate};
    struct OwnedGate {
        std::shared_ptr<Gate> gate;
        std::shared_ptr<Completion> retired;
        ~OwnedGate()
        {
            record_completion(retired.get());
        }
    };
    const auto retired = std::make_shared<Completion>();
    auto context = std::make_shared<OwnedGate>();
    context->gate = gate;
    context->retired = retired;
    const std::weak_ptr<OwnedGate> lifetime = context;
    const auto old = scheduler.reserve_service(0,
        {[](void* pointer) noexcept {
             wait_at_gate(static_cast<OwnedGate*>(pointer)->gate.get());
         },
            context});
    REQUIRE_EQ(old.status, RuntimeScheduler::SubmitStatus::accepted);
    REQUIRE_EQ(scheduler.notify_service(old.token),
        RuntimeScheduler::SubmitStatus::accepted);
    wait_until_started(gate);
    context.reset();
    REQUIRE(!lifetime.expired());
    REQUIRE(!scheduler.service_quiescent(old.token));
    REQUIRE(scheduler.release_service(old.token));
    REQUIRE(!scheduler.release_service(old.token));
    REQUIRE(!scheduler.service_quiescent(old.token));
    REQUIRE_EQ(scheduler.notify_service(old.token),
        RuntimeScheduler::SubmitStatus::invalid);
    const auto completion = std::make_shared<Completion>();
    REQUIRE_EQ(
        scheduler.reserve_service(0, {record_completion, completion}).status,
        RuntimeScheduler::SubmitStatus::full);
    release_gate(gate);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds {2};
    while (!scheduler.service_quiescent(old.token)
        && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    REQUIRE(scheduler.service_quiescent(old.token));
    const auto next =
        scheduler.reserve_service(0, {record_completion, completion});
    REQUIRE_EQ(next.status, RuntimeScheduler::SubmitStatus::accepted);
    REQUIRE_EQ(next.token.slot, old.token.slot);
    REQUIRE(next.token.generation != old.token.generation);
    REQUIRE_EQ(scheduler.notify_service(old.token),
        RuntimeScheduler::SubmitStatus::invalid);
    REQUIRE(!scheduler.release_service(old.token));
    REQUIRE(scheduler.service_quiescent(old.token));
    REQUIRE_EQ(scheduler.notify_service(next.token),
        RuntimeScheduler::SubmitStatus::accepted);
    wait_until_complete(completion);
    scheduler.stop();
    REQUIRE(scheduler.service_quiescent(next.token));
}

namespace {
struct RearmingService {
    RuntimeScheduler* scheduler = nullptr;
    RuntimeScheduler::ServiceToken token;
    std::mutex mutex;
    std::condition_variable changed;
    std::size_t calls = 0;
    bool statuses_valid = true;
    bool ordinary_progress = false;
};
void run_rearming_service(void* context) noexcept
{
    auto& service = *static_cast<RearmingService*>(context);
    std::lock_guard lock(service.mutex);
    ++service.calls;
    if (service.calls < 3) {
        service.statuses_valid &=
            service.scheduler->notify_service(service.token)
            == RuntimeScheduler::SubmitStatus::accepted;
        service.statuses_valid &=
            service.scheduler->notify_service(service.token)
            == RuntimeScheduler::SubmitStatus::accepted;
    } else
        service.statuses_valid &=
            service.scheduler->release_service(service.token);
    service.changed.notify_all();
}
void record_service_peer_progress(void* context) noexcept
{
    auto& service = *static_cast<RearmingService*>(context);
    std::lock_guard lock(service.mutex);
    service.ordinary_progress = service.calls > 0 && service.calls < 3;
    service.changed.notify_all();
}
}

TEST(
    compat_runtime_scheduler_service_can_rearm_and_retire_itself_without_starving_fifo)
{
    RuntimeScheduler scheduler({1, 1, 1, 1});
    REQUIRE(scheduler.start());
    auto service = std::make_shared<RearmingService>();
    service->scheduler = &scheduler;
    auto gate = std::make_shared<Gate>();
    const GateRelease release {gate};
    REQUIRE_EQ(scheduler.submit(0, {wait_at_gate, gate}),
        RuntimeScheduler::SubmitStatus::accepted);
    wait_until_started(gate);
    const auto reserved =
        scheduler.reserve_service(0, {run_rearming_service, service});
    REQUIRE_EQ(reserved.status, RuntimeScheduler::SubmitStatus::accepted);
    service->token = reserved.token;
    REQUIRE_EQ(scheduler.notify_service(reserved.token),
        RuntimeScheduler::SubmitStatus::accepted);
    REQUIRE_EQ(scheduler.submit(0, {record_service_peer_progress, service}),
        RuntimeScheduler::SubmitStatus::accepted);
    release_gate(gate);
    {
        std::unique_lock lock(service->mutex);
        REQUIRE(service->changed.wait_for(lock, std::chrono::seconds {2}, [&] {
            return service->calls == 3;
        }));
        REQUIRE(service->statuses_valid);
        REQUIRE(service->ordinary_progress);
    }
    scheduler.stop();
    REQUIRE(scheduler.service_quiescent(reserved.token));
    REQUIRE_EQ(scheduler.snapshot().service_runs, 3U);
    REQUIRE_EQ(scheduler.snapshot().service_coalesced, 2U);
}

TEST(
    compat_runtime_scheduler_service_tokens_do_not_alias_other_scheduler_generations)
{
    const auto completion = std::make_shared<Completion>();
    RuntimeScheduler first({1, 1, 1, 1});
    RuntimeScheduler second({1, 1, 1, 1});
    REQUIRE_EQ(first.reserve_service(0, {record_completion, completion}).status,
        RuntimeScheduler::SubmitStatus::stopped);
    REQUIRE(first.start());
    REQUIRE(second.start());
    const auto old = first.reserve_service(0, {record_completion, completion});
    const auto fresh =
        second.reserve_service(0, {record_completion, completion});
    REQUIRE_EQ(old.status, RuntimeScheduler::SubmitStatus::accepted);
    REQUIRE_EQ(fresh.status, RuntimeScheduler::SubmitStatus::accepted);
    REQUIRE_EQ(old.token.slot, fresh.token.slot);
    REQUIRE_EQ(old.token.generation, fresh.token.generation);
    REQUIRE(old.token.scope != fresh.token.scope);
    REQUIRE_EQ(second.notify_service(old.token),
        RuntimeScheduler::SubmitStatus::invalid);
    REQUIRE(!second.release_service(old.token));
    REQUIRE(!second.service_quiescent(old.token));
    REQUIRE_EQ(second.notify_service(fresh.token),
        RuntimeScheduler::SubmitStatus::accepted);
    wait_until_complete(completion);
    first.stop();
    REQUIRE_EQ(first.notify_service(old.token),
        RuntimeScheduler::SubmitStatus::stopped);
    REQUIRE(first.service_quiescent(old.token));
    second.stop();
}

TEST(
    compat_runtime_scheduler_service_retirement_destroys_context_outside_scheduler_locks)
{
    RuntimeScheduler scheduler({1, 1, 1, 1});
    REQUIRE(scheduler.start());
    struct ReentrantDestruction {
        RuntimeScheduler* scheduler;
        bool* destroyed;
        ~ReentrantDestruction()
        {
            (void)scheduler->snapshot();
            (void)scheduler->start();
            *destroyed = true;
        }
    };
    for (bool stop : {false, true}) {
        bool destroyed = false;
        auto context = std::make_shared<ReentrantDestruction>();
        context->scheduler = &scheduler;
        context->destroyed = &destroyed;
        const auto reserved =
            scheduler.reserve_service(0, {[](void*) noexcept { }, context});
        REQUIRE_EQ(reserved.status, RuntimeScheduler::SubmitStatus::accepted);
        context.reset();
        REQUIRE(!destroyed);
        if (stop)
            scheduler.stop();
        else
            REQUIRE(scheduler.release_service(reserved.token));
        REQUIRE(destroyed);
    }
}

TEST(
    compat_runtime_scheduler_service_pending_wake_is_canceled_before_stop_returns)
{
    RuntimeScheduler scheduler({1, 1, 1, 1});
    REQUIRE(scheduler.start());
    auto gate = std::make_shared<Gate>();
    std::jthread stopper;
    const GateRelease release {gate};
    REQUIRE_EQ(scheduler.submit(0, {wait_at_gate, gate}),
        RuntimeScheduler::SubmitStatus::accepted);
    wait_until_started(gate);
    auto completion = std::make_shared<Completion>();
    const auto reserved =
        scheduler.reserve_service(0, {record_completion, completion});
    REQUIRE_EQ(reserved.status, RuntimeScheduler::SubmitStatus::accepted);
    REQUIRE_EQ(scheduler.notify_service(reserved.token),
        RuntimeScheduler::SubmitStatus::accepted);
    std::weak_ptr<Completion> lifetime = completion;
    completion.reset();
    stopper = std::jthread([&] {
        scheduler.stop();
    });
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds {2};
    while (scheduler.snapshot().services_reserved != 0
        && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    REQUIRE_EQ(scheduler.snapshot().services_reserved, 0U);
    REQUIRE_EQ(scheduler.snapshot().services_pending, 0U);
    REQUIRE_EQ(scheduler.notify_service(reserved.token),
        RuntimeScheduler::SubmitStatus::stopped);
    release_gate(gate);
    stopper.join();
    REQUIRE(lifetime.expired());
    REQUIRE_EQ(scheduler.snapshot().service_runs, 0U);
    REQUIRE_EQ(scheduler.snapshot().completed, 1U);
}

TEST(
    compat_runtime_scheduler_service_default_off_and_invalid_tokens_fail_closed)
{
    RuntimeScheduler scheduler({1, 1, 1});
    REQUIRE(scheduler.start());
    auto completion = std::make_shared<Completion>();
    REQUIRE_EQ(
        scheduler.reserve_service(0, {record_completion, completion}).status,
        RuntimeScheduler::SubmitStatus::full);
    REQUIRE_EQ(scheduler.reserve_service(0, {}).status,
        RuntimeScheduler::SubmitStatus::invalid);
    REQUIRE_EQ(scheduler.snapshot().service_capacity, 0U);
    REQUIRE_EQ(scheduler.snapshot().service_storage_bytes, 0U);
    bool overflow_rejected = false;
    try {
        RuntimeScheduler oversized(
            {2, 1, 1, (std::numeric_limits<std::size_t>::max)()});
    } catch (const std::length_error&) {
        overflow_rejected = true;
    }
    REQUIRE(overflow_rejected);
    REQUIRE_EQ(
        scheduler.notify_service({}), RuntimeScheduler::SubmitStatus::invalid);
    REQUIRE(!scheduler.release_service({}));
    REQUIRE(!scheduler.service_quiescent({}));
    scheduler.stop();
}

TEST(
    compat_runtime_scheduler_service_publish_during_running_and_after_idle_is_not_lost)
{
    RuntimeScheduler scheduler({1, 1, 1, 1});
    REQUIRE(scheduler.start());
    const auto gate = std::make_shared<Gate>();
    const GateRelease release {gate};
    const auto reservation = scheduler.reserve_service(0, {wait_at_gate, gate});
    REQUIRE_EQ(reservation.status, RuntimeScheduler::SubmitStatus::accepted);
    REQUIRE_EQ(scheduler.notify_service(reservation.token),
        RuntimeScheduler::SubmitStatus::accepted);
    wait_until_started(gate);
    REQUIRE_EQ(scheduler.snapshot().services_executing, 1U);
    REQUIRE_EQ(scheduler.notify_service(reservation.token),
        RuntimeScheduler::SubmitStatus::accepted);
    REQUIRE_EQ(scheduler.notify_service(reservation.token),
        RuntimeScheduler::SubmitStatus::accepted);
    REQUIRE_EQ(scheduler.snapshot().services_pending, 1U);
    release_gate(gate);
    const auto await = [&](std::uint64_t runs) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds {2};
        while (scheduler.snapshot().service_runs < runs
            && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        REQUIRE_EQ(scheduler.snapshot().service_runs, runs);
    };
    await(2);
    REQUIRE_EQ(scheduler.snapshot().services_pending, 0U);
    REQUIRE_EQ(scheduler.notify_service(reservation.token),
        RuntimeScheduler::SubmitStatus::accepted);
    await(3);
    REQUIRE(scheduler.release_service(reservation.token));
    scheduler.stop();
}

namespace {
struct ServiceRound {
    RuntimeScheduler* scheduler = nullptr;
    std::array<RuntimeScheduler::ServiceToken, 2> tokens;
    std::array<int, 6> order {};
    std::array<std::size_t, 2> turns {};
    std::mutex mutex;
    std::condition_variable changed;
    std::size_t count = 0;
    bool valid = true;
};
struct RoundPeer {
    std::shared_ptr<ServiceRound> round;
    std::size_t index = 0;
};
void run_service_round(void* pointer) noexcept
{
    auto& peer = *static_cast<RoundPeer*>(pointer);
    auto& round = *peer.round;
    std::lock_guard lock(round.mutex);
    if (round.count >= round.order.size()) {
        round.valid = false;
        return;
    }
    round.order[round.count++] = static_cast<int>(peer.index);
    if (++round.turns[peer.index] < 3)
        round.valid &= round.scheduler->notify_service(round.tokens[peer.index])
            == RuntimeScheduler::SubmitStatus::accepted;
    else
        round.valid &=
            round.scheduler->release_service(round.tokens[peer.index]);
    round.changed.notify_all();
}
}

TEST(compat_runtime_scheduler_service_round_robin_bounds_hot_peer_turns)
{
    RuntimeScheduler scheduler({1, 1, 1, 2});
    REQUIRE(scheduler.start());
    const auto gate = std::make_shared<Gate>();
    const GateRelease release {gate};
    REQUIRE_EQ(scheduler.submit(0, {wait_at_gate, gate}),
        RuntimeScheduler::SubmitStatus::accepted);
    wait_until_started(gate);
    const auto round = std::make_shared<ServiceRound>();
    round->scheduler = &scheduler;
    for (std::size_t index = 0; index < 2; ++index) {
        auto peer = std::make_shared<RoundPeer>(RoundPeer {round, index});
        const auto reservation =
            scheduler.reserve_service(0, {run_service_round, peer});
        REQUIRE_EQ(
            reservation.status, RuntimeScheduler::SubmitStatus::accepted);
        round->tokens[index] = reservation.token;
        REQUIRE_EQ(scheduler.notify_service(reservation.token),
            RuntimeScheduler::SubmitStatus::accepted);
    }
    release_gate(gate);
    {
        std::unique_lock lock(round->mutex);
        REQUIRE(round->changed.wait_for(lock, std::chrono::seconds {2}, [&] {
            return round->count == 6;
        }));
        REQUIRE(round->valid);
        REQUIRE_EQ(round->order, (std::array {0, 1, 0, 1, 0, 1}));
    }
    scheduler.stop();
    REQUIRE_EQ(scheduler.snapshot().service_runs, 6U);
}

TEST(
    compat_runtime_scheduler_service_stop_waits_for_running_callback_and_cancels_rearm)
{
    RuntimeScheduler scheduler({1, 1, 1, 1});
    REQUIRE(scheduler.start());
    const auto gate = std::make_shared<Gate>();
    std::jthread stopper;
    const GateRelease release {gate};
    const auto reservation = scheduler.reserve_service(0, {wait_at_gate, gate});
    REQUIRE_EQ(reservation.status, RuntimeScheduler::SubmitStatus::accepted);
    REQUIRE_EQ(scheduler.notify_service(reservation.token),
        RuntimeScheduler::SubmitStatus::accepted);
    wait_until_started(gate);
    REQUIRE_EQ(scheduler.notify_service(reservation.token),
        RuntimeScheduler::SubmitStatus::accepted);
    stopper = std::jthread([&] {
        scheduler.stop();
    });
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds {2};
    while (scheduler.snapshot().services_reserved != 0
        && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    REQUIRE_EQ(scheduler.snapshot().services_reserved, 0U);
    REQUIRE_EQ(scheduler.snapshot().services_pending, 0U);
    REQUIRE_EQ(scheduler.snapshot().services_executing, 1U);
    REQUIRE(!scheduler.service_quiescent(reservation.token));
    REQUIRE_EQ(scheduler.notify_service(reservation.token),
        RuntimeScheduler::SubmitStatus::stopped);
    release_gate(gate);
    stopper.join();
    REQUIRE(scheduler.service_quiescent(reservation.token));
    REQUIRE_EQ(scheduler.snapshot().service_runs, 1U);
    REQUIRE_EQ(scheduler.snapshot().services_executing, 0U);
}

TEST(
    compat_runtime_scheduler_service_context_retirement_serializes_concurrent_stop)
{
    RuntimeScheduler scheduler({1, 1, 1, 1});
    REQUIRE(scheduler.start());
    const auto gate = std::make_shared<Gate>();
    const auto second_finished = std::make_shared<Completion>();
    std::jthread first;
    std::jthread second;
    const GateRelease release {gate};
    struct Retirement {
        RuntimeScheduler* scheduler;
        std::shared_ptr<Gate> gate;
        ~Retirement()
        {
            (void)scheduler->start();
            scheduler->stop(); // Same-thread reentrancy must not self-wait.
            wait_at_gate(gate.get());
        }
    };
    auto context = std::make_shared<Retirement>();
    context->scheduler = &scheduler;
    context->gate = gate;
    REQUIRE_EQ(
        scheduler.reserve_service(0, {[](void*) noexcept { }, context}).status,
        RuntimeScheduler::SubmitStatus::accepted);
    context.reset();
    first = std::jthread([&] {
        scheduler.stop();
    });
    wait_until_started(gate); // The first stop is paused inside retirement.
    const auto second_started = std::make_shared<Completion>();
    second = std::jthread([&] {
        record_completion(second_started.get());
        scheduler.stop();
        record_completion(second_finished.get());
    });
    wait_until_complete(second_started);
    REQUIRE(!scheduler.start());
    {
        std::lock_guard lock(second_finished->mutex);
        REQUIRE(!second_finished->complete);
    }
    release_gate(gate);
    first.join();
    second.join();
    wait_until_complete(second_finished);
}

TEST(
    compat_runtime_scheduler_service_deadline_is_reserved_outside_full_timer_pool)
{
    RuntimeScheduler scheduler({1, 1, 1, 1});
    REQUIRE(scheduler.start());
    const auto gate = std::make_shared<Gate>();
    const GateRelease release {gate};
    REQUIRE_EQ(scheduler.submit(0, {wait_at_gate, gate}),
        RuntimeScheduler::SubmitStatus::accepted);
    wait_until_started(gate);
    const auto completion = std::make_shared<TimerCompletion>();
    const auto future =
        std::chrono::steady_clock::now() + std::chrono::hours {1};
    REQUIRE_EQ(scheduler
                   .schedule_at(
                       0, future, {record_regular_timer_completion, completion})
                   .status,
        RuntimeScheduler::SubmitStatus::accepted);
    REQUIRE_EQ(scheduler
                   .schedule_at(
                       0, future, {record_regular_timer_completion, completion})
                   .status,
        RuntimeScheduler::SubmitStatus::full);
    const auto reserved = scheduler.reserve_service(0,
        {record_regular_timer_completion, completion, record_timer_completion});
    REQUIRE_EQ(reserved.status, RuntimeScheduler::SubmitStatus::accepted);
    REQUIRE_EQ(scheduler.schedule_service_at(reserved.token, future),
        RuntimeScheduler::SubmitStatus::accepted);
    REQUIRE_EQ(scheduler.snapshot().service_timers, 1U);
    REQUIRE(scheduler.cancel_service_timer(reserved.token));
    REQUIRE(!scheduler.cancel_service_timer(reserved.token));
    REQUIRE_EQ(scheduler.schedule_service_at(
                   reserved.token, std::chrono::steady_clock::now()),
        RuntimeScheduler::SubmitStatus::accepted);
    release_gate(gate);
    wait_until_complete(completion);
    scheduler.stop();
    // Service hints use the ordinary function contract.
    REQUIRE(!completion->timer_callback);
    REQUIRE_EQ(scheduler.snapshot().service_runs, 1U);
    REQUIRE_EQ(scheduler.snapshot().service_timers, 0U);
    REQUIRE_EQ(scheduler.schedule_service_at(reserved.token, future),
        RuntimeScheduler::SubmitStatus::stopped);
    REQUIRE(!scheduler.cancel_service_timer(reserved.token));
}

TEST(
    compat_runtime_scheduler_service_immediate_wake_preserves_future_deadline_and_cancel)
{
    RuntimeScheduler scheduler({1, 1, 1, 1});
    REQUIRE(scheduler.start());
    const auto completion = std::make_shared<Completion>();
    const auto reserved =
        scheduler.reserve_service(0, {record_completion, completion});
    REQUIRE_EQ(reserved.status, RuntimeScheduler::SubmitStatus::accepted);
    REQUIRE_EQ(scheduler.schedule_service_at(reserved.token,
                   std::chrono::steady_clock::now() + std::chrono::hours {1}),
        RuntimeScheduler::SubmitStatus::accepted);
    REQUIRE_EQ(scheduler.notify_service(reserved.token),
        RuntimeScheduler::SubmitStatus::accepted);
    wait_until_complete(completion);
    REQUIRE_EQ(scheduler.snapshot().service_timers, 1U);
    REQUIRE(scheduler.cancel_service_timer(reserved.token));
    REQUIRE_EQ(scheduler.snapshot().service_timers, 0U);
    REQUIRE(scheduler.release_service(reserved.token));
    const auto quiescence_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds {2};
    while (!scheduler.service_quiescent(reserved.token)
        && std::chrono::steady_clock::now() < quiescence_deadline)
        std::this_thread::yield();
    REQUIRE(scheduler.service_quiescent(reserved.token));
    const auto fresh =
        scheduler.reserve_service(0, {record_completion, completion});
    REQUIRE_EQ(fresh.status, RuntimeScheduler::SubmitStatus::accepted);
    REQUIRE_EQ(scheduler.schedule_service_at(
                   reserved.token, std::chrono::steady_clock::now()),
        RuntimeScheduler::SubmitStatus::invalid);
    REQUIRE(!scheduler.cancel_service_timer(reserved.token));
    auto malformed = fresh.token;
    ++malformed.slot;
    REQUIRE_EQ(scheduler.schedule_service_at(
                   malformed, std::chrono::steady_clock::now()),
        RuntimeScheduler::SubmitStatus::invalid);
    REQUIRE_EQ(scheduler.notify_service(malformed),
        RuntimeScheduler::SubmitStatus::invalid);
    REQUIRE(!scheduler.release_service(malformed));
    REQUIRE(!scheduler.service_quiescent(malformed));
    REQUIRE_EQ(scheduler.snapshot().service_timers, 0U);
    scheduler.stop();
}

TEST(
    compat_runtime_scheduler_service_deadline_wakes_an_idle_shard_at_its_due_time)
{
    RuntimeScheduler scheduler({1, 1, 1, 1});
    REQUIRE(scheduler.start());
    const auto completion = std::make_shared<Completion>();
    const auto reserved =
        scheduler.reserve_service(0, {record_completion, completion});
    REQUIRE_EQ(reserved.status, RuntimeScheduler::SubmitStatus::accepted);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds {20};
    REQUIRE_EQ(scheduler.schedule_service_at(reserved.token, deadline),
        RuntimeScheduler::SubmitStatus::accepted);
    wait_until_complete(completion);
    REQUIRE(std::chrono::steady_clock::now() >= deadline);
    scheduler.stop();
    REQUIRE_EQ(scheduler.snapshot().service_runs, 1U);
    REQUIRE_EQ(scheduler.snapshot().service_timers, 0U);
}

TEST(
    compat_runtime_scheduler_service_scan_skips_unused_tail_and_reclaims_released_slots)
{
    for (const bool release_tail : {false, true}) {
        RuntimeScheduler scheduler({.shard_count = 1,
            .queue_capacity_per_shard = 4,
            .timer_capacity_per_shard = 1,
            .service_capacity_per_shard = 1024});
        REQUIRE(scheduler.start());
        auto blocked = std::make_shared<Gate>();
        GateRelease blocked_release {blocked};
        REQUIRE_EQ(scheduler.submit(0, {wait_at_gate, blocked}),
            RuntimeScheduler::SubmitStatus::accepted);
        wait_until_started(blocked);
        auto complete = std::make_shared<Completion>();
        const auto service =
            scheduler.reserve_service(0, {record_completion, complete});
        REQUIRE_EQ(service.status, RuntimeScheduler::SubmitStatus::accepted);
        if (release_tail) {
            const auto middle =
                scheduler.reserve_service(0, {record_completion, complete});
            const auto tail =
                scheduler.reserve_service(0, {record_completion, complete});
            REQUIRE_EQ(tail.status, RuntimeScheduler::SubmitStatus::accepted);
            REQUIRE(scheduler.release_service(middle.token));
            REQUIRE(scheduler.release_service(tail.token));
        }
        REQUIRE_EQ(
            scheduler.schedule_service_at(service.token,
                std::chrono::steady_clock::now() + std::chrono::hours {1}),
            RuntimeScheduler::SubmitStatus::accepted);
        REQUIRE_EQ(scheduler.notify_service(service.token),
            RuntimeScheduler::SubmitStatus::accepted);
        auto after = std::make_shared<Gate>();
        GateRelease after_release {after};
        REQUIRE_EQ(scheduler.submit(0, {wait_at_gate, after}),
            RuntimeScheduler::SubmitStatus::accepted);
        const auto before = scheduler.snapshot().service_slot_inspections;
        release_gate(blocked);
        wait_until_started(after);
        wait_until_complete(complete);
        const auto snapshot = scheduler.snapshot();
        // One deadline check before service dispatch, one ready-slot check,
        // then one deadline check before the following FIFO callback.
        REQUIRE_EQ(snapshot.service_slot_inspections - before, 3U);
        REQUIRE_EQ(snapshot.service_runs, 1U);
        REQUIRE_EQ(snapshot.service_timers, 1U);
        REQUIRE(scheduler.release_service(service.token));
        release_gate(after);
        scheduler.stop();
    }
}

TEST(compat_runtime_scheduler_service_scan_keeps_live_tail_timer_across_holes)
{
    RuntimeScheduler scheduler({1, 4, 1, 1024});
    REQUIRE(scheduler.start());
    auto blocked = std::make_shared<Gate>();
    GateRelease blocked_release {blocked};
    REQUIRE_EQ(scheduler.submit(0, {wait_at_gate, blocked}),
        RuntimeScheduler::SubmitStatus::accepted);
    wait_until_started(blocked);
    auto complete = std::make_shared<Completion>();
    const auto first =
        scheduler.reserve_service(0, {record_completion, complete});
    const auto middle =
        scheduler.reserve_service(0, {record_completion, complete});
    const auto tail =
        scheduler.reserve_service(0, {record_completion, complete});
    REQUIRE_EQ(tail.status, RuntimeScheduler::SubmitStatus::accepted);
    REQUIRE(scheduler.release_service(first.token));
    REQUIRE(scheduler.release_service(middle.token));
    REQUIRE_EQ(scheduler.schedule_service_at(
                   tail.token, std::chrono::steady_clock::time_point {}),
        RuntimeScheduler::SubmitStatus::accepted);
    auto after = std::make_shared<Gate>();
    GateRelease after_release {after};
    REQUIRE_EQ(scheduler.submit(0, {wait_at_gate, after}),
        RuntimeScheduler::SubmitStatus::accepted);
    const auto before = scheduler.snapshot().service_slot_inspections;
    release_gate(blocked);
    wait_until_started(after);
    wait_until_complete(complete);
    REQUIRE_EQ(scheduler.snapshot().service_slot_inspections - before, 9U);
    REQUIRE(scheduler.release_service(tail.token));
    const auto replacement =
        scheduler.reserve_service(0, {record_completion, complete});
    REQUIRE_EQ(replacement.status, RuntimeScheduler::SubmitStatus::accepted);
    REQUIRE_EQ(replacement.token.slot, first.token.slot);
    REQUIRE(replacement.token.generation != first.token.generation);
    REQUIRE_EQ(scheduler.notify_service(first.token),
        RuntimeScheduler::SubmitStatus::invalid);
    REQUIRE(scheduler.release_service(replacement.token));
    release_gate(after);
    scheduler.stop();
}

TEST(compat_runtime_scheduler_service_scan_trims_released_tail_after_callback)
{
    RuntimeScheduler scheduler({1, 4, 1, 1024});
    REQUIRE(scheduler.start());
    auto complete = std::make_shared<Completion>();
    const auto first =
        scheduler.reserve_service(0, {record_completion, complete});
    auto blocked = std::make_shared<Gate>();
    GateRelease blocked_release {blocked};
    const auto tail = scheduler.reserve_service(0, {wait_at_gate, blocked});
    REQUIRE_EQ(tail.status, RuntimeScheduler::SubmitStatus::accepted);
    REQUIRE_EQ(scheduler.notify_service(tail.token),
        RuntimeScheduler::SubmitStatus::accepted);
    wait_until_started(blocked);
    REQUIRE(scheduler.release_service(tail.token));
    REQUIRE(!scheduler.service_quiescent(tail.token));
    auto after = std::make_shared<Gate>();
    GateRelease after_release {after};
    REQUIRE_EQ(scheduler.notify_service(first.token),
        RuntimeScheduler::SubmitStatus::accepted);
    REQUIRE_EQ(scheduler.submit(0, {wait_at_gate, after}),
        RuntimeScheduler::SubmitStatus::accepted);
    const auto before = scheduler.snapshot().service_slot_inspections;
    release_gate(blocked);
    // The callback is a service turn: alternate into queued FIFO work first.
    wait_until_started(after);
    REQUIRE(scheduler.service_quiescent(tail.token));
    REQUIRE_EQ(scheduler.snapshot().service_slot_inspections - before, 1U);
    release_gate(after);
    wait_until_complete(complete);
    REQUIRE(scheduler.release_service(first.token));
    scheduler.stop();
}

namespace {
struct NotificationPublication {
    std::shared_ptr<Gate> pause = std::make_shared<Gate>();
    std::atomic<unsigned> before = 0;
    std::atomic<unsigned> published = 0;
    bool pause_first = false;
    std::atomic_bool first_published = false;
    static void observe(void* ptr, bool after) noexcept
    {
        auto& self = *static_cast<NotificationPublication*>(ptr);
        static thread_local bool first_notification = false;
        if (after) {
            self.published.fetch_add(1);
            if (first_notification)
                self.first_published = true;
        } else {
            first_notification = self.before.fetch_add(1) == 0;
            if (first_notification && self.pause_first)
                wait_at_gate(self.pause.get());
        }
    }
};
RuntimeScheduler::Configuration notification_configuration(
    NotificationPublication& publication, std::size_t shards = 1)
{
    return {.shard_count = shards,
        .queue_capacity_per_shard = 4,
        .timer_capacity_per_shard = 4,
        .service_capacity_per_shard = 4,
        .service_notification_hook_for_testing =
            NotificationPublication::observe,
        .service_notification_context_for_testing = &publication};
}
}

TEST(compat_runtime_scheduler_service_paused_publication_precedes_coalescing)
{
    NotificationPublication publication;
    publication.pause_first = true;
    RuntimeScheduler scheduler(notification_configuration(publication, 2));
    REQUIRE(scheduler.start());
    const auto worker = std::make_shared<Gate>();
    const GateRelease release_worker {worker};
    REQUIRE_EQ(scheduler.submit(0, {wait_at_gate, worker}),
        RuntimeScheduler::SubmitStatus::accepted);
    wait_until_started(worker);
    const auto completion = std::make_shared<Completion>();
    const auto reservation =
        scheduler.reserve_service(0, {record_completion, completion});
    REQUIRE_EQ(reservation.status, RuntimeScheduler::SubmitStatus::accepted);
    const auto independent = std::make_shared<Completion>();
    const auto other =
        scheduler.reserve_service(1, {record_completion, independent});
    REQUIRE_EQ(other.status, RuntimeScheduler::SubmitStatus::accepted);
    std::atomic<RuntimeScheduler::SubmitStatus> first {
        RuntimeScheduler::SubmitStatus::invalid};
    std::atomic<RuntimeScheduler::SubmitStatus> second {
        RuntimeScheduler::SubmitStatus::invalid};
    std::jthread publisher([&] {
        first = scheduler.notify_service(reservation.token);
    });
    const GateRelease release_publisher {publication.pause};
    wait_until_started(publication.pause);
    REQUIRE_EQ(publication.published.load(), 0U);
    // Only the owning shard is held by the paused publisher.
    REQUIRE_EQ(scheduler.notify_service(other.token),
        RuntimeScheduler::SubmitStatus::accepted);
    wait_until_complete(independent);
    const auto follower_started = std::make_shared<Gate>();
    std::atomic_bool follower_observed_publication = false;
    std::jthread follower([&] {
        {
            std::lock_guard lock(follower_started->mutex);
            follower_started->started = true;
        }
        follower_started->changed.notify_all();
        second = scheduler.notify_service(reservation.token);
        follower_observed_publication = publication.first_published.load();
    });
    const GateRelease release_follower {publication.pause};
    wait_until_started(follower_started);
    release_gate(publication.pause);
    publisher.join();
    follower.join();
    REQUIRE(follower_observed_publication.load());
    REQUIRE_EQ(first.load(), RuntimeScheduler::SubmitStatus::accepted);
    REQUIRE_EQ(second.load(), RuntimeScheduler::SubmitStatus::accepted);
    REQUIRE_EQ(publication.before.load(), 2U);
    REQUIRE_EQ(publication.published.load(), 2U);
    REQUIRE_EQ(scheduler.snapshot().service_coalesced, 1U);
    release_gate(worker);
    wait_until_complete(completion);
    scheduler.stop();
    REQUIRE_EQ(scheduler.snapshot().service_runs, 2U);
}

TEST(compat_runtime_scheduler_service_coalescing_preserves_each_slot_and_reuse)
{
    NotificationPublication publication;
    RuntimeScheduler scheduler(notification_configuration(publication));
    REQUIRE(scheduler.start());
    const auto worker = std::make_shared<Gate>();
    const GateRelease release {worker};
    REQUIRE_EQ(scheduler.submit(0, {wait_at_gate, worker}),
        RuntimeScheduler::SubmitStatus::accepted);
    wait_until_started(worker);
    const auto discarded = std::make_shared<Completion>();
    const auto old =
        scheduler.reserve_service(0, {record_completion, discarded});
    REQUIRE_EQ(old.status, RuntimeScheduler::SubmitStatus::accepted);
    REQUIRE_EQ(scheduler.notify_service(old.token),
        RuntimeScheduler::SubmitStatus::accepted);
    REQUIRE_EQ(scheduler.notify_service(old.token),
        RuntimeScheduler::SubmitStatus::accepted);
    REQUIRE(scheduler.release_service(old.token));
    const auto current = std::make_shared<Completion>();
    const auto fresh =
        scheduler.reserve_service(0, {record_completion, current});
    REQUIRE_EQ(fresh.status, RuntimeScheduler::SubmitStatus::accepted);
    REQUIRE_EQ(fresh.token.slot, old.token.slot);
    REQUIRE(fresh.token.generation != old.token.generation);
    REQUIRE_EQ(scheduler.notify_service(old.token),
        RuntimeScheduler::SubmitStatus::invalid);
    REQUIRE_EQ(scheduler.notify_service(fresh.token),
        RuntimeScheduler::SubmitStatus::accepted);
    const auto neighbor = std::make_shared<Completion>();
    const auto other =
        scheduler.reserve_service(0, {record_completion, neighbor});
    REQUIRE_EQ(other.status, RuntimeScheduler::SubmitStatus::accepted);
    REQUIRE_EQ(scheduler.notify_service(other.token),
        RuntimeScheduler::SubmitStatus::accepted);
    REQUIRE_EQ(scheduler.notify_service(other.token),
        RuntimeScheduler::SubmitStatus::accepted);
    REQUIRE_EQ(publication.published.load(), 3U);
    release_gate(worker);
    wait_until_complete(current);
    wait_until_complete(neighbor);
    scheduler.stop();
    REQUIRE(!discarded->complete);
    REQUIRE_EQ(scheduler.snapshot().service_runs, 2U);
    REQUIRE_EQ(scheduler.snapshot().service_coalesced, 2U);
    REQUIRE_EQ(scheduler.notify_service(fresh.token),
        RuntimeScheduler::SubmitStatus::stopped);
}

namespace {
struct TimerCreatedPending {
    RuntimeScheduler* scheduler = nullptr;
    RuntimeScheduler::ServiceToken token;
    std::shared_ptr<Gate> worker = std::make_shared<Gate>();
    std::shared_ptr<Completion> complete = std::make_shared<Completion>();
    std::atomic<unsigned> calls = 0;
    std::atomic_bool valid = true;
    static void dispatch(void* ptr) noexcept
    {
        auto& self = *static_cast<TimerCreatedPending*>(ptr);
        if (self.calls.fetch_add(1) == 0) {
            self.valid = self.scheduler->schedule_service_at(
                             self.token, std::chrono::steady_clock::now())
                == RuntimeScheduler::SubmitStatus::accepted;
            self.valid = self.valid.load()
                && self.scheduler->submit(0, {wait_at_gate, self.worker})
                    == RuntimeScheduler::SubmitStatus::accepted;
        } else {
            record_completion(self.complete.get());
        }
    }
};
}

TEST(
    compat_runtime_scheduler_service_timer_pending_needs_no_second_notification)
{
    NotificationPublication publication;
    RuntimeScheduler scheduler(notification_configuration(publication));
    REQUIRE(scheduler.start());
    const auto state = std::make_shared<TimerCreatedPending>();
    state->scheduler = &scheduler;
    const GateRelease release {state->worker};
    const auto reservation =
        scheduler.reserve_service(0, {TimerCreatedPending::dispatch, state});
    REQUIRE_EQ(reservation.status, RuntimeScheduler::SubmitStatus::accepted);
    state->token = reservation.token;
    REQUIRE_EQ(scheduler.notify_service(reservation.token),
        RuntimeScheduler::SubmitStatus::accepted);
    wait_until_started(state->worker);
    REQUIRE(state->valid.load());
    REQUIRE_EQ(scheduler.snapshot().services_pending, 1U);
    REQUIRE_EQ(scheduler.notify_service(reservation.token),
        RuntimeScheduler::SubmitStatus::accepted);
    REQUIRE_EQ(publication.published.load(), 1U);
    REQUIRE_EQ(scheduler.snapshot().service_coalesced, 1U);
    release_gate(state->worker);
    wait_until_complete(state->complete);
    scheduler.stop();
    REQUIRE_EQ(scheduler.snapshot().service_runs, 2U);
}
