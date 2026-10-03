#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>

namespace robotweax::srt::compat::model {

// Step-driven B0 model. Only the dedicated test executable builds this file;
// there is no transport, scheduler, public-API or callback integration.
class ConnectionAffinity {
public:
    static constexpr std::size_t maximum_connections = 64;
    static constexpr std::size_t maximum_shards = 64;
    static constexpr std::size_t datagram_capacity = 64;
    static constexpr std::size_t data_capacity = 48;
    static constexpr std::size_t command_capacity = 8;
    static constexpr std::size_t maximum_bytes = 1500;
    static constexpr std::size_t turn_budget = 16;

    struct Configuration {
        std::size_t shards = 2;
        std::size_t memory_budget = 16U * 1024U * 1024U;
        std::size_t reserved_wake_slots = 2;
    };
    struct Key {
        std::uint64_t runtime = 0;
        std::uint64_t channel = 0;
        std::uint64_t route = 0;
        std::size_t slot = maximum_connections;
        friend bool operator==(const Key&, const Key&) = default;
    };
    enum class Phase { setup, idle, ready, running, closing, closed };
    enum class Admission { accepted, full, closed, stale, invalid };
    enum class Kind { data, control, command, send, receive_release, timer };
    enum class CommandState {
        stale,
        queued,
        issued,
        waiting,
        completed,
        cancelled
    };
    enum class WakeResult { accepted, full, stopped };
    struct Command {
        Key key;
        std::size_t slot = command_capacity;
        std::uint64_t serial = 0;
        friend bool operator==(const Command&, const Command&) = default;
    };
    struct Timer {
        Key key;
        std::uint64_t serial = 0;
    };
    struct Turn {
        Key key;
        std::uint64_t serial = 0;
    };
    struct Drain {
        Key key;
        std::uint64_t serial = 0;
    };
    struct Event {
        Kind kind = Kind::data;
        std::uint64_t ticket = 0;
        std::uint64_t published_at = 0;
        std::array<std::byte, maximum_bytes> bytes {};
        std::size_t size = 0;
        Command command;
        Timer timer;
    };
    struct Readiness {
        bool work = false;
        bool terminal = false;
        std::uint64_t low_epoch = 0;
        std::uint64_t change_epoch = 0;
    };
    struct Snapshot {
        Phase phase = Phase::closed;
        std::size_t shard = 0;
        std::size_t datagrams = 0;
        std::size_t data = 0;
        std::size_t commands = 0;
        std::size_t inbox_highwater = 0;
        std::size_t command_highwater = 0;
        std::uint64_t data_rejections = 0;
        std::uint64_t control_rejections = 0;
        std::uint64_t max_queue_delay = 0;
        std::uint64_t turns = 0;
        std::uint64_t close_quiescence_delay = 0;
        std::uint64_t close_completion_delay = 0;
        bool owner_active = false;
        bool drain_active = false;
        Readiness readiness;
    };
    struct WakeSnapshot {
        std::size_t ready = 0;
        bool doorbell = false;
        bool fallback = false;
        std::uint64_t full_rejections = 0;
    };

    explicit ConnectionAffinity(Configuration configuration);
    ~ConnectionAffinity();
    ConnectionAffinity(const ConnectionAffinity&) = delete;
    ConnectionAffinity& operator=(const ConnectionAffinity&) = delete;
    // Fixed typed storage for this model and one channel-credit ledger;
    // excludes allocator bookkeeping and caller-retained Event copies.
    [[nodiscard]] static std::size_t storage_bytes() noexcept;
    [[nodiscard]] bool valid() const noexcept;
    // channel is the caller's non-reused channel-generation token, not a UDP
    // descriptor. Wire IDs are unique only within that channel generation.
    [[nodiscard]] std::optional<Key> admit(std::uint64_t channel,
        std::uint32_t wire_id, bool setup = false) noexcept;
    [[nodiscard]] Admission publish(Key key, Kind kind,
        std::span<const std::byte> bytes, std::uint64_t now,
        bool setup_publisher = false) noexcept;
    [[nodiscard]] bool seal_setup(Key key) noexcept;
    [[nodiscard]] bool finish_promotion(Key key) noexcept;
    [[nodiscard]] std::optional<Command> publish_command(Key key,
        std::span<const std::byte> bytes, std::uint64_t now,
        std::optional<std::uint64_t> deadline = std::nullopt) noexcept;
    [[nodiscard]] bool cancel(Command command) noexcept;
    [[nodiscard]] bool commit(Turn turn, Command command, std::uint64_t now,
        std::int64_t result) noexcept;
    [[nodiscard]] bool wait(Turn turn, Command command) noexcept;
    [[nodiscard]] bool resume(Command command, std::uint64_t now) noexcept;
    [[nodiscard]] CommandState command_state(Command command) const noexcept;
    [[nodiscard]] std::optional<std::int64_t> take_result(
        Command command) noexcept;
    [[nodiscard]] bool notify(Key key, Kind kind, std::uint64_t now) noexcept;
    [[nodiscard]] std::optional<Timer> arm_timer(
        Key key, std::uint64_t deadline) noexcept;
    [[nodiscard]] bool cancel_timer(Timer timer) noexcept;
    [[nodiscard]] bool publish_timer(Timer timer, std::uint64_t now) noexcept;
    [[nodiscard]] bool commit_timer(
        Turn turn, Timer timer, std::uint64_t now) noexcept;
    void observe_wake(
        std::size_t shard, WakeResult result, std::uint64_t now) noexcept;
    [[nodiscard]] WakeSnapshot wake_snapshot(std::size_t shard) const noexcept;
    [[nodiscard]] std::optional<Turn> begin_turn(std::size_t shard) noexcept;
    [[nodiscard]] std::optional<Event> next(
        Turn turn, std::uint64_t now) noexcept;
    [[nodiscard]] bool finish_turn(Turn turn, std::uint64_t now) noexcept;
    [[nodiscard]] bool close(Key key, std::uint64_t now) noexcept;
    [[nodiscard]] std::optional<Drain> begin_drain(Key key) noexcept;
    [[nodiscard]] bool finish_drain(Drain drain, std::uint64_t now) noexcept;
    [[nodiscard]] bool release(Key key) noexcept;
    // A paused owner must acknowledge its close barrier before a generation
    // can restart. Retained Event values have no pointers into the pool.
    void stop(std::uint64_t now) noexcept;
    [[nodiscard]] bool restart() noexcept;
    [[nodiscard]] Snapshot snapshot(Key key) const noexcept;

private:
    struct State;
    Configuration configuration_;
    std::unique_ptr<State> state_;
};

// No stack-borrowed send budget. Attempts include failed/EAGAIN submissions;
// caller-owned leases and the round are invalidated together on shutdown.
class ChannelCredits {
public:
    static constexpr std::size_t attempts_per_round = 64;
    static constexpr std::size_t bytes_per_round = 96000;
    struct Lease {
        std::uint64_t generation = 0;
        std::uint64_t serial = 0;
        std::size_t slot = 64;
    };
    struct Snapshot {
        std::size_t active = 0;
        std::size_t attempts = 0;
        std::size_t bytes = 0;
        std::size_t available_attempts = attempts_per_round;
        std::size_t available_bytes = bytes_per_round;
    };
    [[nodiscard]] std::optional<Lease> grant(std::uint64_t owner) noexcept;
    [[nodiscard]] bool attempt(Lease lease, std::size_t bytes) noexcept;
    [[nodiscard]] bool release(Lease lease) noexcept;
    [[nodiscard]] bool next_round() noexcept;
    void stop() noexcept;
    [[nodiscard]] Snapshot snapshot() const noexcept;

private:
    struct Entry {
        std::uint64_t owner = 0;
        std::uint64_t serial = 0;
        std::size_t attempts = 0;
        std::size_t bytes = 0;
    };
    mutable std::mutex mutex_;
    std::array<Entry, 64> entries_ {};
    Snapshot snapshot_;
    std::uint64_t generation_ = 1;
    std::uint64_t serial_ = 0;
    bool stopped_ = false;
};

} // namespace robotweax::srt::compat::model
