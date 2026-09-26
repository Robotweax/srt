#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

namespace robotweax::srt::compat {

// One bounded, level-triggered poll/WSAPoll watcher per scheduler. Each arm
// produces at most one notification; protocol work stays on scheduler shards.
class SocketReadiness {
public:
    struct Callback {
        void (*function)(void*) noexcept = nullptr;
        std::shared_ptr<void> context;
    };
    struct Token {
        std::size_t slot = 0;
        std::uint64_t generation = 0;
        [[nodiscard]] bool valid() const noexcept
        {
            return generation != 0U;
        }
    };
    struct Snapshot {
        std::size_t registered = 0;
        std::size_t armed = 0;
        std::uint64_t waits = 0;
        std::uint64_t notifications = 0;
        bool running = false;
    };

    explicit SocketReadiness(std::size_t capacity);
    ~SocketReadiness();
    [[nodiscard]] bool start() noexcept;
    [[nodiscard]] Token watch(
        std::uintptr_t socket, Callback callback) noexcept;
    [[nodiscard]] bool arm(Token token) noexcept;
    // Retire the watch and any poll snapshot containing it before the caller
    // closes the native socket. Already dispatched callbacks may still run.
    void cancel(Token token) noexcept;
    void stop() noexcept;
    [[nodiscard]] Snapshot snapshot() const noexcept;

private:
    struct State;
    static void run(std::shared_ptr<State> state) noexcept;
    std::shared_ptr<State> state_;
    std::mutex lifecycle_mutex_;
    std::thread worker_;
};

} // namespace robotweax::srt::compat
