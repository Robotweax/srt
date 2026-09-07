#include "compat/runtime_work_executor.hpp"
#include "test.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <thread>

using namespace robotweax::srt::compat;

namespace {

struct WorkGate {
    std::mutex mutex;
    std::condition_variable changed;
    bool started = false;
    bool released = false;
};

struct WorkCounter {
    std::mutex mutex;
    std::condition_variable changed;
    std::size_t value = 0;
};

struct SelfStopContext {
    std::shared_ptr<RuntimeWorkExecutor> executor;
    std::mutex mutex;
    std::condition_variable changed;
    bool stopped = false;
};

void wait_at_work_gate(void* context) noexcept
{
    auto& gate = *static_cast<WorkGate*>(context);
    std::unique_lock lock(gate.mutex);
    gate.started = true;
    gate.changed.notify_all();
    gate.changed.wait(lock, [&gate] {
        return gate.released;
    });
}

void count_work(void* context) noexcept
{
    auto& counter = *static_cast<WorkCounter*>(context);
    {
        std::lock_guard lock(counter.mutex);
        ++counter.value;
    }
    counter.changed.notify_all();
}

void stop_from_work(void* context) noexcept
{
    auto& stop = *static_cast<SelfStopContext*>(context);
    stop.executor->stop();
    {
        std::lock_guard lock(stop.mutex);
        stop.stopped = true;
    }
    stop.changed.notify_all();
}

void release_work_gate(const std::shared_ptr<WorkGate>& gate)
{
    {
        std::lock_guard lock(gate->mutex);
        gate->released = true;
    }
    gate->changed.notify_all();
}

} // namespace

TEST(runtime_work_executor_rejects_invalid_configuration_and_tasks)
{
    auto executor = std::make_shared<RuntimeWorkExecutor>(
        RuntimeWorkExecutor::Configuration {});
    REQUIRE(!executor->start());
    REQUIRE_EQ(
        executor->submit({}), RuntimeWorkExecutor::SubmitStatus::invalid);
    const auto context = std::make_shared<WorkCounter>();
    REQUIRE_EQ(executor->submit({.function = count_work, .context = context}),
        RuntimeWorkExecutor::SubmitStatus::stopped);
    const RuntimeWorkExecutor::Snapshot snapshot = executor->snapshot();
    REQUIRE_EQ(snapshot.worker_count, 0U);
    REQUIRE_EQ(snapshot.queue_capacity, 0U);
    REQUIRE_EQ(snapshot.rejected_invalid, 1U);
    REQUIRE_EQ(snapshot.rejected_stopped, 1U);
    REQUIRE(!snapshot.accepting);
}

TEST(runtime_work_executor_bounds_submission_and_drains_on_stop)
{
    auto executor = std::make_shared<RuntimeWorkExecutor>(
        RuntimeWorkExecutor::Configuration {
            .worker_count = 1U,
            .queue_capacity = 1U,
        });
    REQUIRE(executor->start());
    const auto gate = std::make_shared<WorkGate>();
    REQUIRE_EQ(executor->submit({
                   .function = wait_at_work_gate,
                   .context = gate,
               }),
        RuntimeWorkExecutor::SubmitStatus::accepted);
    {
        std::unique_lock lock(gate->mutex);
        REQUIRE(gate->changed.wait_for(lock, std::chrono::seconds {2}, [&gate] {
            return gate->started;
        }));
    }

    const auto counter = std::make_shared<WorkCounter>();
    REQUIRE_EQ(executor->submit({
                   .function = count_work,
                   .context = counter,
               }),
        RuntimeWorkExecutor::SubmitStatus::accepted);
    REQUIRE_EQ(executor->submit({
                   .function = count_work,
                   .context = counter,
               }),
        RuntimeWorkExecutor::SubmitStatus::full);
    const RuntimeWorkExecutor::Snapshot blocked = executor->snapshot();
    REQUIRE_EQ(blocked.worker_count, 1U);
    REQUIRE_EQ(blocked.queue_capacity, 1U);
    REQUIRE_EQ(blocked.queued, 1U);
    REQUIRE_EQ(blocked.executing, 1U);
    REQUIRE_EQ(blocked.rejected_full, 1U);

    release_work_gate(gate);
    executor->stop();
    const RuntimeWorkExecutor::Snapshot stopped = executor->snapshot();
    REQUIRE_EQ(stopped.queued, 0U);
    REQUIRE_EQ(stopped.executing, 0U);
    REQUIRE_EQ(stopped.accepted, 2U);
    REQUIRE_EQ(stopped.completed, 2U);
    REQUIRE(!stopped.accepting);
    REQUIRE_EQ(counter->value, 1U);
    REQUIRE_EQ(executor->submit({
                   .function = count_work,
                   .context = counter,
               }),
        RuntimeWorkExecutor::SubmitStatus::stopped);
}

TEST(runtime_work_executor_runs_independent_work_concurrently)
{
    auto executor = std::make_shared<RuntimeWorkExecutor>(
        RuntimeWorkExecutor::Configuration {
            .worker_count = 2U,
            .queue_capacity = 2U,
        });
    REQUIRE(executor->start());
    const auto first = std::make_shared<WorkGate>();
    const auto second = std::make_shared<WorkGate>();
    REQUIRE_EQ(executor->submit({
                   .function = wait_at_work_gate,
                   .context = first,
               }),
        RuntimeWorkExecutor::SubmitStatus::accepted);
    REQUIRE_EQ(executor->submit({
                   .function = wait_at_work_gate,
                   .context = second,
               }),
        RuntimeWorkExecutor::SubmitStatus::accepted);
    {
        std::unique_lock first_lock(first->mutex);
        REQUIRE(first->changed.wait_for(
            first_lock, std::chrono::seconds {2}, [&first] {
                return first->started;
            }));
    }
    {
        std::unique_lock second_lock(second->mutex);
        REQUIRE(second->changed.wait_for(
            second_lock, std::chrono::seconds {2}, [&second] {
                return second->started;
            }));
    }
    release_work_gate(first);
    release_work_gate(second);
    executor->stop();
    REQUIRE_EQ(executor->snapshot().completed, 2U);
}

TEST(runtime_work_executor_can_stop_from_its_own_worker)
{
    auto executor = std::make_shared<RuntimeWorkExecutor>(
        RuntimeWorkExecutor::Configuration {
            .worker_count = 1U,
            .queue_capacity = 1U,
        });
    REQUIRE(executor->start());
    const auto context = std::make_shared<SelfStopContext>();
    context->executor = executor;
    REQUIRE_EQ(executor->submit({
                   .function = stop_from_work,
                   .context = context,
               }),
        RuntimeWorkExecutor::SubmitStatus::accepted);
    {
        std::unique_lock lock(context->mutex);
        REQUIRE(context->changed.wait_for(
            lock, std::chrono::seconds {2}, [&context] {
                return context->stopped;
            }));
    }
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds {2};
    while (executor->snapshot().completed != 1U
        && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    REQUIRE_EQ(executor->snapshot().completed, 1U);
    REQUIRE(!executor->snapshot().accepting);
    context->executor.reset();
}
