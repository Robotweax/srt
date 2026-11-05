#pragma once

#include "compat/readiness.hpp"
#include "compat/runtime_scheduler.hpp"
#include "robotweax/srt/sequence.hpp"
#include "robotweax/srt/packet.hpp"
#include "robotweax/srt/udp.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace robotweax::srt::compat {

class DatagramChannel;

// Group-owned original DATA work, independent of member ACK/window state.
// The coordinator must encode canonical protected packets and commit member
// history separately. This component never changes headers, ciphertext or keys.
// Activation requires a reserved scheduler service; it does not change global
// scheduler budgets or the existing multicast profiles' admission semantics.
class GroupMulticastEmitter {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using ClockFunction = TimePoint (*)(void*) noexcept;
    static constexpr std::size_t maximum_capacity = 256;
    static constexpr std::size_t maximum_datagram_size =
        packet_header_size + maximum_data_payload_size;
    static constexpr std::size_t maximum_batch = 64;
    static constexpr std::uint32_t maximum_retry_attempts = 64;

    struct Configuration {
        std::shared_ptr<DatagramChannel> channel;
        IpEndpoint destination {};
        std::uint32_t group_id = 0;
        std::uint64_t generation = 0;
        SequenceNumber initial_sequence {};
        std::size_t capacity = 64;
        std::size_t batch_size = 16;
        std::size_t maximum_transmission_unit = 1500;
        // Includes IP/UDP overhead. No catch-up burst credit after a late wake.
        std::uint64_t wire_bytes_per_second = 0;
        std::uint32_t maximum_attempts = 8;
        Clock::duration retry_delay = std::chrono::milliseconds {1};
        std::uint64_t affinity = 0;
        // Borrowed injectable monotonic clock, called under the emitter mutex.
        // Must outlive the emitter and must not reenter it.
        ClockFunction clock = nullptr;
        void* clock_context = nullptr;
    };

    enum class Admission : std::uint8_t {
        accepted,
        full,
        wrong_generation,
        wrong_sequence,
        invalid_packet,
        expired,
        closed,
        failed,
        cancelled,
    };

    enum class Failure : std::uint8_t {
        none,
        transport,
        partial_datagram,
        scheduler,
    };

    struct Snapshot {
        std::size_t queued = 0;
        std::size_t queued_bytes = 0;
        std::size_t capacity = 0;
        SequenceNumber accepted_next_sequence {};
        std::optional<SequenceNumber> last_submitted_sequence;
        std::optional<TimePoint> next_wakeup;
        std::uint64_t accepted_packets = 0;
        std::uint64_t submitted_packets = 0;
        std::uint64_t submitted_bytes = 0;
        std::uint64_t send_attempts = 0;
        std::uint64_t expired_packets = 0;
        std::uint64_t retry_exhausted_packets = 0;
        std::uint64_t cancelled_packets = 0;
        Failure failure = Failure::none;
        int system_error = 0;
        bool closed = false;
        bool writable = false;
    };

    // A short, thread-bound transaction holding the emitter admission mutex.
    // Reserve before sealing or modifying any receiver history. The owner
    // keeps its encoder/histories serialized until publish returns. No socket
    // I/O or registry work belongs in this interval. Destruction cancels an
    // unpublished reservation without consuming sequence/capacity.
    class Reservation {
    public:
        Reservation(Reservation&&) noexcept = default;
        Reservation& operator=(Reservation&&) = delete;
        ~Reservation();
        [[nodiscard]] Admission status() const noexcept
        {
            return status_ == Admission::accepted && !owner_
                ? Admission::cancelled
                : status_;
        }
        // Called only after packet validation and a successful scheduler wake,
        // while the worker remains excluded. A false result MUST have no
        // effects; a true result publishes all coordinator/history state with
        // no remaining fallible work. Must not reenter the emitter or do I/O.
        using Commit = bool (*)(void*) noexcept;
        [[nodiscard]] Admission publish(std::span<const std::byte> packet,
            std::shared_ptr<const void> generation_owner = {},
            Commit commit = nullptr, void* context = nullptr) noexcept;

    private:
        friend class GroupMulticastEmitter;
        explicit Reservation(Admission status) noexcept
            : status_(status)
        {
        }
        Reservation(std::shared_ptr<GroupMulticastEmitter>,
            std::unique_lock<std::mutex>, TimePoint) noexcept;
        std::shared_ptr<GroupMulticastEmitter> owner_;
        std::unique_lock<std::mutex> lock_;
        TimePoint deadline_ {};
        Admission status_ = Admission::failed;
    };

    // Fails without publishing an emitter if configuration, allocation or
    // service reservation fails. The scheduler must provide service capacity;
    // its owner must close emitters before stopping the scheduler.
    [[nodiscard]] static std::shared_ptr<GroupMulticastEmitter> create(
        Configuration configuration,
        std::shared_ptr<RuntimeScheduler> scheduler) noexcept;
    [[nodiscard]] static std::shared_ptr<GroupMulticastEmitter>
    create_for_testing(Configuration configuration) noexcept;
    ~GroupMulticastEmitter();

    GroupMulticastEmitter(const GroupMulticastEmitter&) = delete;
    GroupMulticastEmitter& operator=(const GroupMulticastEmitter&) = delete;

    // Copies exactly one immutable encoded DATA packet. Encrypted packets
    // require an ownership lease for the encoder's immutable crypto generation
    // until submission/drop/close. Leases must have bounded teardown and are
    // always released outside the emitter mutex. Queue acceptance
    // is not multicast emission, remote delivery or an ACK over unseen DATA.
    [[nodiscard]] Admission enqueue(std::uint64_t generation,
        std::span<const std::byte> packet, TimePoint deadline,
        std::shared_ptr<const void> generation_owner = {}) noexcept;
    // A deadline is checked at reservation. If it elapses during preparation,
    // accepted work is still committed and then expires through ordinary drive
    // accounting; sealing cannot leave a rejected nonce pinned to the caller.
    [[nodiscard]] Reservation reserve(std::uint64_t generation,
        SequenceNumber sequence, TimePoint deadline) noexcept;
    [[nodiscard]] Snapshot snapshot() const noexcept;
    [[nodiscard]] const std::shared_ptr<ReadinessSource>&
    readiness_source() const noexcept
    {
        return readiness_;
    }
    // Serializes with admitted socket I/O; no further emission after return.
    // Service retirement is nonblocking even on a scheduler worker.
    void close() noexcept;
    void pump_for_testing() noexcept;

private:
    struct WorkContext {
        std::weak_ptr<GroupMulticastEmitter> owner;
    };
    struct Entry {
        std::array<std::byte, maximum_datagram_size> bytes {};
        std::size_t size = 0;
        SequenceNumber sequence {};
        TimePoint deadline {};
        std::uint32_t attempts = 0;
        std::shared_ptr<const void> generation_owner;
    };
    using RetiredOwners =
        std::array<std::shared_ptr<const void>, maximum_capacity>;

    explicit GroupMulticastEmitter(Configuration configuration);
    [[nodiscard]] static bool valid_configuration(
        const Configuration&) noexcept;
    [[nodiscard]] bool valid_packet(
        std::span<const std::byte>, bool has_owner) const noexcept;
    static void run(void*) noexcept;
    void drive() noexcept;
    [[nodiscard]] TimePoint now_locked() noexcept;
    [[nodiscard]] TimePoint wakeup_locked() const noexcept;
    [[nodiscard]] static TimePoint add_saturated(
        TimePoint, Clock::duration) noexcept;
    void pop_locked(RetiredOwners&, std::size_t&) noexcept;
    void fail_locked(Failure, int, RetiredOwners&, std::size_t&) noexcept;

    mutable std::mutex mutex_;
    Configuration configuration_;
    std::vector<Entry> entries_;
    std::size_t head_ = 0;
    Snapshot state_;
    TimePoint last_now_ = TimePoint::min();
    TimePoint rate_ready_ = TimePoint::min();
    TimePoint retry_ready_ = TimePoint::min();
    const std::size_t network_header_bytes_;
    std::shared_ptr<ReadinessSource> readiness_;
    std::shared_ptr<WorkContext> work_;
    std::shared_ptr<RuntimeScheduler> scheduler_;
    RuntimeScheduler::ServiceToken service_ {};
    bool manual_ = false;
};

} // namespace robotweax::srt::compat
