#include "compat/connection_work_binding.hpp"

#include <mutex>
#include <condition_variable>
#include <utility>

namespace robotweax::srt::compat {

struct ConnectionWorkBinding::State {
    std::mutex mutex;
    std::condition_variable drained;
    ConnectionWorkHints pending;
    bool callback_active = false;
    bool completion_pending = false;
    bool retired = false;
    Function function = nullptr;
    CompletionFunction completion = nullptr;
    std::shared_ptr<void> context;

    void finish() noexcept
    {
        for (;;) {
            {
                std::lock_guard lock(mutex);
                completion_pending = false;
            }
            if (completion != nullptr) {
                completion(context.get());
            }
            {
                std::lock_guard lock(mutex);
                // A request racing the hook must get another pass before the
                // barrier opens. Dispatcher teardown requests this only once.
                if (completion_pending) {
                    continue;
                }
                callback_active = false;
            }
            drained.notify_all();
            return;
        }
    }

    static void dispatch(void* pointer) noexcept
    {
        auto& state = *static_cast<State*>(pointer);
        ConnectionWorkHints hints;
        {
            std::lock_guard lock(state.mutex);
            if (state.retired) {
                return;
            }
            hints = std::exchange(state.pending, {});
            if (!hints.send && !hints.receive_release && !hints.datagrams) {
                return;
            }
            state.callback_active = true;
        }
        // A hint published while this callback runs remains pending and wakes
        // another turn. No client code executes under the publication mutex.
        state.function(state.context.get(), hints);
        state.finish();
    }
};

ConnectionWorkBinding::ConnectionWorkBinding(
    std::weak_ptr<RuntimeScheduler> scheduler,
    RuntimeScheduler::ServiceToken token, std::shared_ptr<State> state) noexcept
    : scheduler_(std::move(scheduler))
    , token_(token)
    , state_(std::move(state))
{
}

std::shared_ptr<ConnectionWorkBinding> ConnectionWorkBinding::create(
    const std::shared_ptr<RuntimeScheduler>& scheduler, std::uint64_t affinity,
    Function function, std::shared_ptr<void> context,
    CompletionFunction completion) noexcept
{
    if (scheduler == nullptr || function == nullptr) {
        return nullptr;
    }
    RuntimeScheduler::ServiceToken token;
    try {
        auto state = std::make_shared<State>();
        state->function = function;
        state->completion = completion;
        state->context = std::move(context);
        const auto result = scheduler->reserve_service(
            affinity, {.function = State::dispatch, .context = state});
        if (result.status != RuntimeScheduler::SubmitStatus::accepted) {
            return nullptr;
        }
        token = result.token;
        return std::shared_ptr<ConnectionWorkBinding>(
            new ConnectionWorkBinding(scheduler, token, std::move(state)));
    } catch (...) {
        // Includes allocation of the shared-pointer control block after the
        // reservation. Destruction/release are nonblocking and idempotent.
        if (token.valid()) {
            (void)scheduler->release_service(token);
        }
        return nullptr;
    }
}

ConnectionWorkBinding::~ConnectionWorkBinding()
{
    retire();
}

RuntimeScheduler::SubmitStatus ConnectionWorkBinding::notify(
    ConnectionWorkHints hints) noexcept
{
    const auto scheduler = scheduler_.lock();
    if (scheduler == nullptr) {
        return RuntimeScheduler::SubmitStatus::stopped;
    }
    std::lock_guard lock(state_->mutex);
    if (state_->retired
        || (!hints.send && !hints.receive_release && !hints.datagrams)) {
        return RuntimeScheduler::SubmitStatus::invalid;
    }
    state_->pending.send |= hints.send;
    state_->pending.receive_release |= hints.receive_release;
    state_->pending.datagrams |= hints.datagrams;
    // Serialize notification with retirement. The scheduler never calls the
    // service inline and releases its shard lock before client dispatch.
    const auto status = scheduler->notify_service(token_);
    if (status != RuntimeScheduler::SubmitStatus::accepted) {
        state_->pending = {};
    }
    return status;
}

void ConnectionWorkBinding::retire(bool request_completion) noexcept
{
    bool complete = false;
    {
        std::lock_guard lock(state_->mutex);
        state_->retired = true;
        state_->pending = {};
        if (request_completion && state_->completion != nullptr) {
            state_->completion_pending = true;
            if (!state_->callback_active) {
                state_->callback_active = true;
                complete = true;
            }
        }
    }
    if (const auto scheduler = scheduler_.lock(); scheduler != nullptr) {
        (void)scheduler->release_service(token_);
    }
    if (complete) {
        state_->finish();
    }
}

bool ConnectionWorkBinding::quiescent() const noexcept
{
    bool retired;
    {
        std::lock_guard lock(state_->mutex);
        if (state_->callback_active) {
            return false;
        }
        retired = state_->retired;
    }
    // An expired weak pointer alone is not a barrier: the scheduler destructor
    // can be joining a client callback after its last strong reference vanished.
    const auto scheduler = scheduler_.lock();
    return scheduler != nullptr ? scheduler->service_quiescent(token_)
                                : retired;
}

ConnectionWorkBinding::DrainStatus ConnectionWorkBinding::wait_quiescent(
    std::chrono::steady_clock::time_point deadline) const noexcept
{
    if (RuntimeScheduler::on_worker_thread()) {
        return DrainStatus::worker_thread;
    }
    std::unique_lock lock(state_->mutex);
    if (!state_->retired) {
        return DrainStatus::not_retired;
    }
    if (!state_->drained.wait_until(lock, deadline, [this] {
            return !state_->callback_active;
        })) {
        return DrainStatus::timeout;
    }
    // Retirement and dispatch use this mutex. No new client callback can enter
    // after retirement, including a service task already copied by a worker.
    return DrainStatus::quiescent;
}

} // namespace robotweax::srt::compat
