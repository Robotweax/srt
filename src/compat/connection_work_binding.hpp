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

    // One reservation per binding; affinity and scope/generation never change.
    // Failure is setup failure, never application-buffer pressure. The supplied
    // context must not strongly own the binding (or its owning runtime).
    [[nodiscard]] static std::shared_ptr<ConnectionWorkBinding> create(
        const std::shared_ptr<RuntimeScheduler>& scheduler,
        std::uint64_t affinity, Function function,
        std::shared_ptr<void> context) noexcept;
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
    void retire() noexcept;
    [[nodiscard]] bool quiescent() const noexcept;

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
