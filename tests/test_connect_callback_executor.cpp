#include "compat/connect_callback_executor.hpp"
#include "compat/error_state.hpp"
#include "test.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using namespace robotweax::srt::compat;
namespace {
struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool released = false;
    int initial_error = -1;
    int delivered_error = -1;
    std::thread::id thread;
};
void block(void* context, int error) noexcept
{
    auto& gate = *static_cast<Gate*>(context);
    std::unique_lock lock(gate.mutex);
    gate.initial_error = last_error().code;
    gate.delivered_error = error;
    gate.thread = std::this_thread::get_id();
    set_last_error(SRT_EINVSOCK);
    gate.entered = true;
    gate.changed.notify_all();
    gate.changed.wait(lock, [&] {
        return gate.released;
    });
}
bool entered(const std::shared_ptr<Gate>& gate)
{
    std::unique_lock lock(gate->mutex);
    return gate->changed.wait_for(lock, std::chrono::seconds {3}, [&] {
        return gate->entered;
    });
}
void release(const std::shared_ptr<Gate>& gate)
{
    std::lock_guard lock(gate->mutex);
    gate->released = true;
    gate->changed.notify_all();
}
bool settled(ConnectCallbackExecutor& executor, std::uint64_t count)
{
    const auto end =
        std::chrono::steady_clock::now() + std::chrono::seconds {3};
    while (std::chrono::steady_clock::now() < end) {
        if (executor.snapshot().completed == count)
            return true;
        std::this_thread::yield();
    }
    return false;
}
struct Nested {
    ConnectCallbackExecutor* executor;
    std::shared_ptr<Gate> child;
    std::atomic_bool succeeded = false;
};
void nested(void* context, int) noexcept
{
    auto& n = *static_cast<Nested*>(context);
    const bool submitted = n.executor->submit({block, n.child, SRT_SUCCESS});
    n.succeeded = submitted && entered(n.child);
    release(n.child);
}
void self_stop(void* context, int) noexcept
{
    static_cast<ConnectCallbackExecutor*>(context)->stop();
}
} // namespace

TEST(connect_callback_executor_reuses_idle_worker_and_resets_error)
{
    ConnectCallbackExecutor executor(1);
    std::thread::id first;
    for (unsigned i = 0; i != 32; ++i) {
        auto gate = std::make_shared<Gate>();
        REQUIRE(executor.submit({block, gate, SRT_ETIMEOUT}));
        const bool started = entered(gate);
        release(gate);
        REQUIRE(started);
        REQUIRE(settled(executor, i + 1));
        REQUIRE_EQ(gate->initial_error, SRT_SUCCESS);
        REQUIRE_EQ(gate->delivered_error, SRT_ETIMEOUT);
        if (i == 0)
            first = gate->thread;
        REQUIRE_EQ(first, gate->thread);
    }
    REQUIRE_EQ(executor.snapshot().created, 1U);
    REQUIRE_EQ(executor.snapshot().reused, 31U);
    executor.stop();
    REQUIRE(!executor.submit({block, std::make_shared<Gate>(), 0}));
}

TEST(connect_callback_executor_saturated_workers_do_not_queue_dependencies)
{
    ConnectCallbackExecutor executor(1);
    std::vector<std::shared_ptr<Gate>> gates;
    bool started = true;
    for (unsigned i = 0; i != 8; ++i) {
        auto gate = std::make_shared<Gate>();
        gates.push_back(gate);
        started = executor.submit({block, gate, 0}) && entered(gate) && started;
    }
    auto n = std::make_shared<Nested>();
    n->executor = &executor;
    n->child = std::make_shared<Gate>();
    const bool submitted = executor.submit({nested, n, 0});
    // A callback synchronously waiting on another completion must progress
    // even while every previously created worker is blocked.
    const bool child_started = entered(n->child);
    for (auto& gate : gates)
        release(gate);
    release(n->child);
    executor.stop();
    REQUIRE(started);
    REQUIRE(submitted);
    REQUIRE(child_started);
    REQUIRE(n->succeeded.load());
    REQUIRE_EQ(executor.snapshot().completed, 10U);
}

TEST(connect_callback_executor_idle_retention_and_external_stop)
{
    ConnectCallbackExecutor executor(2);
    std::vector<std::shared_ptr<Gate>> gates;
    bool started = true;
    for (unsigned i = 0; i != 8; ++i) {
        auto gate = std::make_shared<Gate>();
        gates.push_back(gate);
        started = executor.submit({block, gate, 0}) && entered(gate) && started;
    }
    for (auto& gate : gates)
        release(gate);
    REQUIRE(started);
    REQUIRE(settled(executor, 8));
    REQUIRE_EQ(executor.snapshot().idle, 2U);
    REQUIRE_EQ(executor.snapshot().workers, 2U);
    executor.stop();
    REQUIRE_EQ(executor.snapshot().idle, 0U);
    REQUIRE_EQ(executor.snapshot().workers, 0U);
}

TEST(connect_callback_executor_can_stop_from_its_own_task)
{
    auto executor = std::make_shared<ConnectCallbackExecutor>(1);
    REQUIRE(executor->submit({self_stop, executor, 0}));
    REQUIRE(settled(*executor, 1));
    REQUIRE(executor->snapshot().stopping);
    REQUIRE(!executor->submit({}));
    auto fresh = std::make_shared<ConnectCallbackExecutor>(1);
    auto gate = std::make_shared<Gate>();
    REQUIRE(fresh->submit({block, gate, 0}));
    const bool started = entered(gate);
    release(gate);
    fresh->stop();
    REQUIRE(started);
}

TEST(connect_callback_executor_retirement_preserves_accepted_task)
{
    ConnectCallbackExecutor executor(1);
    auto gate = std::make_shared<Gate>();
    REQUIRE(executor.submit({block, gate, SRT_ETIMEOUT}));
    const bool started = entered(gate);
    executor.request_stop();
    const bool rejected =
        !executor.submit({block, std::make_shared<Gate>(), 0});
    release(gate);
    executor.stop();
    REQUIRE(started);
    REQUIRE(rejected);
    REQUIRE_EQ(gate->delivered_error, SRT_ETIMEOUT);
    REQUIRE_EQ(executor.snapshot().completed, 1U);
    REQUIRE_EQ(executor.snapshot().workers, 0U);
    REQUIRE_EQ(executor.snapshot().idle, 0U);
}
