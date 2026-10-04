#pragma once

#include "compat/runtime_scheduler.hpp"

#include <cstdint>
#include <memory>

namespace robotweax::srt::compat {

// Fixed-size hints describe work already committed under the runtime mutex.
// They contain neither application spans nor a new API completion contract.
struct ConnectionWorkHints {
    bool send = false;
    bool receive_release = false;
    bool datagrams = false;
};

class ConnectionWorkBinding {
public:
    using Function = void (*)(void*, ConnectionWorkHints) noexcept;
    using CompletionFunction = void (*)(void*) noexcept;

    // One reservation per binding; affinity and scope/generation never change.
    // Failure is setup failure, never application-buffer pressure. The supplied
    // context must not strongly own the binding (or its owning runtime).
    [[nodiscard]] static std::shared_ptr<ConnectionWorkBinding> create(
        const std::shared_ptr<RuntimeScheduler>& scheduler,
        std::uint64_t affinity, Function function,
        std::shared_ptr<void> context,
        // Optional bounded completion work, outside binding locks. Runs after
        // client dispatch and when explicitly requested at retirement.
        CompletionFunction completion = nullptr) noexcept;
    ~ConnectionWorkBinding();
    ConnectionWorkBinding(const ConnectionWorkBinding&) = delete;
    ConnectionWorkBinding& operator=(const ConnectionWorkBinding&) = delete;

    [[nodiscard]] RuntimeScheduler::ServiceToken token() const noexcept
    {
        return token_;
    }
    [[nodiscard]] RuntimeScheduler::SubmitStatus notify(
        ConnectionWorkHints hints) noexcept;
    // Closes admission, discards queued hints and retires the reservation.
    // A previously dispatched callback can finish; callers must independently
    // enforce the runtime close barrier before protocol effects. Never waits.
    // Request completion only for owning resource teardown. A concurrent
    // callback includes that request in its completion barrier.
    void retire(bool request_completion = false) noexcept;
    [[nodiscard]] bool quiescent() const noexcept;
    enum class DrainStatus { quiescent, timeout, worker_thread, not_retired };
    // Client callback barrier independent of scheduler lifetime. Retire first;
    // never call while holding a runtime/route/inbox lock needed by a callback.
    // All affinity workers reject this wait, including an already quiet one.
    [[nodiscard]] DrainStatus wait_quiescent(
        std::chrono::steady_clock::time_point deadline) const noexcept;

private:
    struct State;
    ConnectionWorkBinding(std::weak_ptr<RuntimeScheduler> scheduler,
        RuntimeScheduler::ServiceToken token,
        std::shared_ptr<State> state) noexcept;

    const std::weak_ptr<RuntimeScheduler> scheduler_;
    const RuntimeScheduler::ServiceToken token_;
    const std::shared_ptr<State> state_;
};

} // namespace robotweax::srt::compat
