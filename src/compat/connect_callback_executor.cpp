#include "compat/connect_callback_executor.hpp"

#include "compat/error_state.hpp"

#include <condition_variable>
#include <list>
#include <mutex>
#include <thread>
#include <utility>

namespace robotweax::srt::compat {

struct ConnectCallbackExecutor::Worker {
    Task task;
    std::thread thread;
    std::condition_variable ready;
    bool idle = false;
    bool joining = false;
};

struct ConnectCallbackExecutor::State {
    explicit State(std::size_t limit)
        : maximum_idle(limit)
    {
    }
    const std::size_t maximum_idle;
    std::mutex mutex;
    std::list<std::shared_ptr<Worker>> workers;
    std::size_t idle = 0;
    std::uint64_t created = 0;
    std::uint64_t reused = 0;
    std::uint64_t completed = 0;
    bool stopping = false;
};

ConnectCallbackExecutor::ConnectCallbackExecutor(std::size_t maximum_idle)
    : state_(std::make_shared<State>(maximum_idle))
{
}

ConnectCallbackExecutor::~ConnectCallbackExecutor()
{
    stop();
}

bool ConnectCallbackExecutor::submit(Task task) noexcept
{
    if (task.function == nullptr || task.context == nullptr)
        return false;
    const auto state = state_;
    std::lock_guard lock(state->mutex);
    if (state->stopping)
        return false;
    for (const auto& worker : state->workers) {
        if (!worker->idle)
            continue;
        worker->idle = false;
        --state->idle;
        worker->task = std::move(task);
        ++state->reused;
        worker->ready.notify_one();
        return true;
    }
    try {
        auto worker = std::make_shared<Worker>();
        worker->task = task;
        state->workers.push_back(worker);
        try {
            worker->thread = std::thread([state, worker] {
                run(state, worker);
            });
        } catch (...) {
            state->workers.pop_back();
            return false;
        }
        ++state->created;
        return true;
    } catch (...) {
        return false;
    }
}

void ConnectCallbackExecutor::run(
    std::shared_ptr<State> state, std::shared_ptr<Worker> worker) noexcept
{
    std::unique_lock lock(state->mutex);
    for (;;) {
        Task task = std::move(worker->task);
        worker->task = {};
        lock.unlock();
        // A reused worker must not expose a previous callback's SRT error.
        clear_last_error();
        task.function(task.context.get(), task.error_code);
        task.context.reset();
        lock.lock();
        ++state->completed;
        if (state->stopping || state->idle == state->maximum_idle)
            break;
        worker->idle = true;
        ++state->idle;
        worker->ready.wait(lock, [&] {
            return state->stopping || worker->task.function != nullptr;
        });
        if (worker->idle) {
            worker->idle = false;
            --state->idle;
        }
        // A handoff already accepted before stop must still run.
        if (worker->task.function == nullptr)
            break;
    }
    if (!worker->joining) {
        // Match the existing callback-worker self-retirement. Never join a
        // retiring thread on the protocol shard or under this mutex: user
        // thread-local destructors can invoke SRT again.
        worker->thread.detach();
        state->workers.remove(worker);
    }
}

void ConnectCallbackExecutor::request_stop() noexcept
{
    const auto state = state_;
    std::lock_guard lock(state->mutex);
    state->stopping = true;
    for (const auto& worker : state->workers) {
        // Keep ownership until stop() joins, including TLS destruction.
        worker->joining = true;
        worker->ready.notify_one();
    }
}

void ConnectCallbackExecutor::stop() noexcept
{
    request_stop();
    const auto state = state_;
    std::list<std::shared_ptr<Worker>> workers;
    {
        std::lock_guard lock(state->mutex);
        workers.swap(state->workers);
    }
    for (const auto& worker : workers) {
        if (worker->thread.get_id() == std::this_thread::get_id()) {
            worker->thread.detach();
        } else if (worker->thread.joinable()) {
            worker->thread.join();
        }
    }
}

ConnectCallbackExecutor::Snapshot
ConnectCallbackExecutor::snapshot() const noexcept
{
    std::lock_guard lock(state_->mutex);
    return {state_->workers.size(), state_->idle, state_->created,
        state_->reused, state_->completed, state_->stopping};
}

namespace {
class Service {
public:
    ~Service()
    {
        const auto executor = retire();
        if (executor)
            executor->stop();
    }
    std::shared_ptr<ConnectCallbackExecutor> acquire() noexcept
    {
        std::lock_guard lock(mutex_);
        if (!executor_) {
            try {
                executor_ = std::make_shared<ConnectCallbackExecutor>(4);
            } catch (...) {
                return {};
            }
        }
        return executor_;
    }
    std::shared_ptr<ConnectCallbackExecutor> retire() noexcept
    {
        std::lock_guard lock(mutex_);
        auto executor = std::move(executor_);
        if (executor)
            executor->request_stop();
        return executor;
    }

private:
    std::mutex mutex_;
    std::shared_ptr<ConnectCallbackExecutor> executor_;
};
Service& service() noexcept
{
    static Service instance;
    return instance;
}
} // namespace

void prepare_connect_callback_executor() noexcept
{
    (void)service();
}
std::shared_ptr<ConnectCallbackExecutor>
acquire_connect_callback_executor() noexcept
{
    return service().acquire();
}
std::shared_ptr<ConnectCallbackExecutor>
retire_connect_callback_executor() noexcept
{
    return service().retire();
}

} // namespace robotweax::srt::compat
