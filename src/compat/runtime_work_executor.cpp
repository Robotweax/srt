#include "compat/runtime_work_executor.hpp"

#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace robotweax::srt::compat {

struct RuntimeWorkExecutor::State {
    explicit State(Configuration requested)
        : configuration(requested)
        , entries(requested.queue_capacity)
    {
        workers.reserve(requested.worker_count);
    }

    Configuration configuration;
    std::vector<Task> entries;
    std::vector<std::thread> workers;
    mutable std::mutex mutex;
    std::mutex lifecycle_mutex;
    std::condition_variable ready;
    std::size_t head = 0;
    std::size_t size = 0;
    std::size_t executing = 0;
    std::uint64_t accepted = 0;
    std::uint64_t completed = 0;
    std::uint64_t rejected_full = 0;
    std::uint64_t rejected_stopped = 0;
    std::uint64_t rejected_invalid = 0;
    bool start_attempted = false;
    bool accepting = false;
    bool stop_requested = false;
};

RuntimeWorkExecutor::RuntimeWorkExecutor(Configuration configuration)
    : state_(std::make_shared<State>(configuration))
{
}

RuntimeWorkExecutor::~RuntimeWorkExecutor()
{
    stop();
}

bool RuntimeWorkExecutor::start() noexcept
{
    const std::shared_ptr<State> state = state_;
    if (state == nullptr) {
        return false;
    }
    std::lock_guard lifecycle_lock(state->lifecycle_mutex);
    {
        std::lock_guard lock(state->mutex);
        if (state->start_attempted || state->stop_requested
            || state->configuration.worker_count == 0U
            || state->configuration.queue_capacity == 0U) {
            return false;
        }
        state->start_attempted = true;
        state->accepting = true;
    }

    try {
        for (std::size_t index = 0; index < state->configuration.worker_count;
            ++index) {
            state->workers.emplace_back([state] {
                run(state);
            });
        }
        return true;
    } catch (...) {
        {
            std::lock_guard lock(state->mutex);
            state->accepting = false;
            state->stop_requested = true;
        }
        state->ready.notify_all();
        for (auto& worker : state->workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        return false;
    }
}

RuntimeWorkExecutor::SubmitStatus RuntimeWorkExecutor::submit(
    Task task) noexcept
{
    const std::shared_ptr<State> state = state_;
    if (state == nullptr) {
        return SubmitStatus::stopped;
    }
    {
        std::lock_guard lock(state->mutex);
        if (task.function == nullptr || task.context == nullptr) {
            ++state->rejected_invalid;
            return SubmitStatus::invalid;
        }
        if (!state->accepting || state->stop_requested) {
            ++state->rejected_stopped;
            return SubmitStatus::stopped;
        }
        if (state->size == state->entries.size()) {
            ++state->rejected_full;
            return SubmitStatus::full;
        }
        state->entries[(state->head + state->size) % state->entries.size()] =
            std::move(task);
        ++state->size;
        ++state->accepted;
    }
    state->ready.notify_one();
    return SubmitStatus::accepted;
}

void RuntimeWorkExecutor::stop() noexcept
{
    const std::shared_ptr<State> state = state_;
    if (state == nullptr) {
        return;
    }
    std::vector<std::thread> workers;
    {
        std::lock_guard lifecycle_lock(state->lifecycle_mutex);
        {
            std::lock_guard lock(state->mutex);
            state->accepting = false;
            state->stop_requested = true;
        }
        workers.swap(state->workers);
    }
    state->ready.notify_all();
    const std::thread::id current = std::this_thread::get_id();
    for (auto& worker : workers) {
        if (!worker.joinable()) {
            continue;
        }
        if (worker.get_id() == current) {
            worker.detach();
        } else {
            worker.join();
        }
    }
}

RuntimeWorkExecutor::Snapshot RuntimeWorkExecutor::snapshot() const noexcept
{
    const std::shared_ptr<State> state = state_;
    if (state == nullptr) {
        return {};
    }
    std::lock_guard lock(state->mutex);
    return {
        .worker_count = state->configuration.worker_count,
        .queue_capacity = state->configuration.queue_capacity,
        .queued = state->size,
        .executing = state->executing,
        .accepted = state->accepted,
        .completed = state->completed,
        .rejected_full = state->rejected_full,
        .rejected_stopped = state->rejected_stopped,
        .rejected_invalid = state->rejected_invalid,
        .accepting = state->accepting,
    };
}

void RuntimeWorkExecutor::run(std::shared_ptr<State> state) noexcept
{
    for (;;) {
        Task task;
        {
            std::unique_lock lock(state->mutex);
            state->ready.wait(lock, [&state] {
                return state->stop_requested || state->size != 0U;
            });
            if (state->size == 0U) {
                if (state->stop_requested) {
                    return;
                }
                continue;
            }
            task = std::move(state->entries[state->head]);
            state->entries[state->head] = {};
            state->head = (state->head + 1U) % state->entries.size();
            --state->size;
            ++state->executing;
        }

        task.function(task.context.get());
        task.context.reset();

        {
            std::lock_guard lock(state->mutex);
            --state->executing;
            ++state->completed;
        }
    }
}

} // namespace robotweax::srt::compat
