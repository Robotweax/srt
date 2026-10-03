#include "compat/connection_work_binding.hpp"

#include <mutex>
#include <utility>

namespace robotweax::srt::compat {

struct ConnectionWorkBinding::State {
    std::mutex mutex;
    ConnectionWorkHints pending;
    bool retired = false;
    Function function = nullptr;
    std::shared_ptr<void> context;

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
        }
        // A hint published while this callback runs remains pending and wakes
        // another turn. No client code executes under the publication mutex.
        if (hints.send || hints.receive_release) {
            state.function(state.context.get(), hints);
        }
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
    Function function, std::shared_ptr<void> context) noexcept
{
    if (scheduler == nullptr || function == nullptr) {
        return nullptr;
    }
    RuntimeScheduler::ServiceToken token;
    try {
        auto state = std::make_shared<State>();
        state->function = function;
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
    if (state_->retired || (!hints.send && !hints.receive_release)) {
        return RuntimeScheduler::SubmitStatus::invalid;
    }
    state_->pending.send |= hints.send;
    state_->pending.receive_release |= hints.receive_release;
    // Serialize notification with retirement. The scheduler never calls the
    // service inline and releases its shard lock before client dispatch.
    const auto status = scheduler->notify_service(token_);
    if (status != RuntimeScheduler::SubmitStatus::accepted) {
        state_->pending = {};
    }
    return status;
}

void ConnectionWorkBinding::retire() noexcept
{
    {
        std::lock_guard lock(state_->mutex);
        if (state_->retired) {
            return;
        }
        state_->retired = true;
        state_->pending = {};
    }
    if (const auto scheduler = scheduler_.lock(); scheduler != nullptr) {
        (void)scheduler->release_service(token_);
    }
}

bool ConnectionWorkBinding::quiescent() const noexcept
{
    const auto scheduler = scheduler_.lock();
    return scheduler == nullptr || scheduler->service_quiescent(token_);
}

} // namespace robotweax::srt::compat
