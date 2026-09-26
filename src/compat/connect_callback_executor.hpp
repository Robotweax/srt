#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace robotweax::srt::compat {

// A direct handoff to an idle worker, or a new worker if all are occupied.
// No callback waits behind another callback: cross-socket close may wait for
// that callback to return. Only idle retention is bounded.
class ConnectCallbackExecutor {
public:
    struct Task {
        void (*function)(void*, int) noexcept = nullptr;
        std::shared_ptr<void> context;
        int error_code = 0;
    };
    struct Snapshot {
        std::size_t workers = 0;
        std::size_t idle = 0;
        std::uint64_t created = 0;
        std::uint64_t reused = 0;
        std::uint64_t completed = 0;
        bool stopping = false;
    };

    explicit ConnectCallbackExecutor(std::size_t maximum_idle);
    ~ConnectCallbackExecutor();
    ConnectCallbackExecutor(const ConnectCallbackExecutor&) = delete;
    ConnectCallbackExecutor& operator=(const ConnectCallbackExecutor&) = delete;

    [[nodiscard]] bool submit(Task task) noexcept;
    // Retire admission without waiting for application TLS destructors.
    // The owner must subsequently stop/join outside runtime lifecycle locks.
    void request_stop() noexcept;
    void stop() noexcept;
    [[nodiscard]] Snapshot snapshot() const noexcept;

private:
    struct Worker;
    struct State;
    static void run(
        std::shared_ptr<State> state, std::shared_ptr<Worker> worker) noexcept;
    std::shared_ptr<State> state_;
};

void prepare_connect_callback_executor() noexcept;
[[nodiscard]] std::shared_ptr<ConnectCallbackExecutor>
acquire_connect_callback_executor() noexcept;
[[nodiscard]] std::shared_ptr<ConnectCallbackExecutor>
retire_connect_callback_executor() noexcept;

} // namespace robotweax::srt::compat
