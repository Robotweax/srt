#pragma once

#include "compat/connection_work_binding.hpp"
#include "robotweax/srt/udp.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>

namespace robotweax::srt::compat {

struct DatagramEnvelope;
class ConnectionRuntime;
class ConnectionDatagramDispatcher;

// Charges preallocated ring storage, including empty and retired inboxes.
// Allocator bookkeeping, inbox metadata and consumer-owned copies are excluded.
class DatagramStorageBudget {
public:
    explicit DatagramStorageBudget(std::size_t maximum_bytes) noexcept;
    [[nodiscard]] std::size_t reserved_bytes() const noexcept;

private:
    friend class ConnectionDatagramInbox;
    [[nodiscard]] bool reserve(std::size_t bytes) noexcept;
    void release(std::size_t bytes) noexcept;
    const std::size_t maximum_bytes_;
    mutable std::mutex mutex_;
    std::size_t reserved_bytes_ = 0;
};

class ConnectionDatagramInbox {
public:
    struct Configuration {
        std::size_t capacity = 64;
        std::size_t control_reserve = 16;
    };
    struct Token {
        std::uint64_t incarnation = 0;
        RuntimeScheduler::ServiceToken service {};
    };
    enum class Status {
        accepted,
        empty,
        full,
        stale,
        invalid,
        closed,
        wake_failed,
        busy,
        exhausted
    };
    struct Snapshot {
        std::size_t queued = 0;
        std::size_t data_queued = 0;
        std::size_t highwater = 0;
        std::uint64_t data_rejections = 0;
        std::uint64_t control_rejections = 0;
        std::uint64_t wake_failures = 0;
        std::uint64_t maximum_queue_delay = 0;
        bool closed = false;
        bool in_flight = false;
        // Bound inbox FIFO progress: completion follows protocol effects.
        // Rejected publications and discarded close entries never advance it.
        std::uint64_t admitted = 0;
        std::uint64_t completed = 0;
    };

    [[nodiscard]] static std::optional<std::size_t> storage_bytes(
        std::size_t capacity) noexcept;
    [[nodiscard]] static std::shared_ptr<ConnectionDatagramInbox> create(
        const std::shared_ptr<DatagramStorageBudget>& budget,
        const std::shared_ptr<ConnectionWorkBinding>& binding, IpEndpoint peer,
        Configuration configuration) noexcept;
    ~ConnectionDatagramInbox();
    ConnectionDatagramInbox(const ConnectionDatagramInbox&) = delete;
    ConnectionDatagramInbox& operator=(const ConnectionDatagramInbox&) = delete;

    [[nodiscard]] Token token() const noexcept
    {
        return token_;
    }
    // Bytes are copied before returning; control admission never bypasses FIFO.
    // A dispatcher-bound inbox fences even captured publishers through its
    // target runtime mutex. Generic inboxes retain their original admission.
    // A failed wake closes admission and discards queued work instead of leaving
    // accepted bytes without a runnable consumer. Caller treats it as terminal.
    [[nodiscard]] Status publish(Token token, std::span<const std::byte> bytes,
        IpEndpoint peer, std::uint64_t now_microseconds) noexcept;
    // Bound inboxes have one dispatcher consumer; its popped copy remains
    // in-flight through protocol completion. Further pops return busy.
    [[nodiscard]] Status pop(Token token, DatagramEnvelope& envelope,
        std::uint64_t now_microseconds) noexcept;
    // A bounded consumer turn must rearm if it leaves queued work behind.
    [[nodiscard]] Status rearm(Token token) noexcept;
    // Nonblocking admission barrier. An already popped packet still requires
    // the runtime close/generation check before protocol effects.
    void close() noexcept;
    [[nodiscard]] Snapshot snapshot() const noexcept;

private:
    friend class ConnectionRuntime;
    friend class ConnectionDatagramDispatcher;
    // Attached once by the dispatcher factory, before any publication/wake.
    void bind_runtime(std::weak_ptr<ConnectionRuntime> runtime) noexcept;
    [[nodiscard]] bool bound_to(
        const ConnectionRuntime* runtime) const noexcept;
    [[nodiscard]] Status publish_unfenced(Token token,
        std::span<const std::byte> bytes, IpEndpoint peer,
        std::uint64_t now_microseconds) noexcept;
    // One popped copy at a time for a bound inbox. Completion follows protocol
    // effects; close does not revoke this record or imply callback quiescence.
    [[nodiscard]] Status complete(Token token) noexcept;
    struct Entry;
    ConnectionDatagramInbox(std::shared_ptr<DatagramStorageBudget> budget,
        std::weak_ptr<ConnectionWorkBinding> binding, Token token,
        IpEndpoint peer, Configuration configuration,
        std::unique_ptr<Entry[]> entries, std::size_t storage_bytes) noexcept;
    [[nodiscard]] bool matches(Token token) const noexcept;
    [[nodiscard]] Status notify_locked() noexcept;
    void close_locked() noexcept;

    const std::shared_ptr<DatagramStorageBudget> budget_;
    const std::weak_ptr<ConnectionWorkBinding> binding_;
    const Token token_;
    const IpEndpoint peer_;
    const Configuration configuration_;
    std::unique_ptr<Entry[]> entries_;
    const std::size_t storage_bytes_;
    std::weak_ptr<ConnectionRuntime> ingress_runtime_;
    const ConnectionRuntime* ingress_identity_ = nullptr;
    bool ingress_bound_ = false;
    mutable std::mutex mutex_;
    std::size_t head_ = 0;
    Snapshot snapshot_ {};
};

} // namespace robotweax::srt::compat
