#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace robotweax::srt::compat {

// Fixed process-work boundary for operations that may perform network I/O or
// invoke application code and therefore must not execute on an affinity shard.
// Submission is bounded and nonblocking; accepted work owns its context until
// completion.
class RuntimeWorkExecutor {
public:
    using TaskFunction = void (*)(void*) noexcept;

    struct Configuration {
        std::size_t worker_count = 0;
        std::size_t queue_capacity = 0;
    };

    struct Task {
        TaskFunction function = nullptr;
        std::shared_ptr<void> context;
    };

    enum class SubmitStatus : std::uint8_t {
        accepted,
        full,
        stopped,
        invalid,
    };

    struct Snapshot {
        std::size_t worker_count = 0;
        std::size_t queue_capacity = 0;
        std::size_t queued = 0;
        std::size_t executing = 0;
        std::uint64_t accepted = 0;
        std::uint64_t completed = 0;
        std::uint64_t rejected_full = 0;
        std::uint64_t rejected_stopped = 0;
        std::uint64_t rejected_invalid = 0;
        bool accepting = false;
    };

    explicit RuntimeWorkExecutor(Configuration configuration);
    ~RuntimeWorkExecutor();

    RuntimeWorkExecutor(const RuntimeWorkExecutor&) = delete;
    RuntimeWorkExecutor& operator=(const RuntimeWorkExecutor&) = delete;

    [[nodiscard]] bool start() noexcept;
    [[nodiscard]] SubmitStatus submit(Task task) noexcept;
    void stop() noexcept;

    [[nodiscard]] Snapshot snapshot() const noexcept;

private:
    struct State;

    static void run(std::shared_ptr<State> state) noexcept;

    std::shared_ptr<State> state_;
};

} // namespace robotweax::srt::compat
