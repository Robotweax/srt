#pragma once

#include "compat/group_runtime_admission.hpp"

namespace robotweax::srt::compat {

// Single-packet Live dispatch actor for the coordinated sender profile.
// Owns the publication/close gate and authoritative advertised cursor. Callers
// must not hold registry/group metadata or runtime locks when entering it.
// Factory construction alone may reserve idle services under a registry lock
// with an already running scheduler and bound source; it performs no I/O.
// Lock order: publication -> emitter -> distinct runtimes (try-lock only).
// Its two reserved services require explicit scheduler capacity; close before
// scheduler stop. The borrowed crypto provider outlives the actor and its work.
class GroupSendCoordinator {
public:
    using Clock = GroupMulticastEmitter::Clock;
    static constexpr std::size_t maximum_members = 64;
    struct Configuration {
        GroupMulticastEmitter::Configuration source;
        Clock::time_point timestamp_origin {};
        std::shared_ptr<SharedTransmitKeySchedule> key_schedule;
        std::uint32_t initial_message = 1;
        std::size_t member_capacity = 16;
        std::size_t fanout_batch = 64;
        // Busy completion attempts are bounded independently of source slots.
        std::uint32_t maximum_busy_attempts = 8;
        Clock::duration poll_interval = std::chrono::milliseconds {1};
        // Optional transient runtime/KM busy grace. No sealing while retrying.
        Clock::duration admission_busy_grace {};
        std::shared_ptr<ReadinessSource> readiness;
        // Borrowed monotonic admission clock, serialized by the actor gate.
        GroupMulticastEmitter::ClockFunction admission_clock = nullptr;
        void* admission_clock_context = nullptr;
    };
    enum class Status : std::uint8_t {
        accepted,
        advanced_without_receivers,
        full,
        busy,
        expired,
        invalid,
        failed,
        closed
    };
    enum class Detachment : std::uint8_t {
        none,
        admission_busy,
        history_full,
        stale,
        closed,
        invalid,
        expired,
        crypto_error,
        source_not_submitted,
        completion_busy,
        invalid_completion
    };
    struct Member {
        std::uint64_t id = 0;
        Detachment detachment = Detachment::none;
    };
    struct Snapshot {
        SequenceNumber next_sequence {};
        std::uint32_t next_message = 1;
        std::size_t active_members = 0;
        std::size_t detached_members = 0;
        std::size_t inflight_frames = 0;
        std::uint64_t accepted_frames = 0;
        std::uint64_t unused_frames = 0;
        std::uint64_t applied_completions = 0;
        std::uint64_t abandoned_completions = 0;
        bool writable = false;
        bool failed = false;
        bool closed = false;
    };
    [[nodiscard]] static std::shared_ptr<GroupSendCoordinator> create(
        Configuration, std::shared_ptr<RuntimeScheduler>,
        CryptoProvider& = default_crypto_provider()) noexcept;
    [[nodiscard]] static std::shared_ptr<GroupSendCoordinator>
    create_for_testing(
        Configuration, CryptoProvider& = default_crypto_provider()) noexcept;
    ~GroupSendCoordinator();

    // Empty history and exact current advertised ISN required. Failed addition
    // preserves the runtime. Detached members stay visible until removal;
    // they cannot resume across a skipped frame. Removal revokes ownership,
    // but leaves the established session available for explicit recovery/close.
    [[nodiscard]] std::optional<Member> add_member(
        const std::shared_ptr<ConnectionRuntime>&) noexcept;
    void remove_member(std::uint64_t id) noexcept;
    [[nodiscard]] std::size_t members(std::span<Member>) const noexcept;
    // Admission uses detach-on-unavailable, including KM busy after the optional
    // grace. Full source/fanout capacity rejects before member mutation or
    // sealing. A source failure never authorizes a receiver's history.
    // A key-horizon limit rejects before KM preparation can replace a selector
    // used by a still-pending original. This fences local backlog only; it does
    // not prove remote receipt or qualify reordered/delayed network KM traffic.
    [[nodiscard]] Status send(std::span<const std::byte>, PacketTimestamp,
        Clock::time_point deadline = Clock::time_point::max(),
        bool in_order = true) noexcept;
    [[nodiscard]] Snapshot snapshot() const noexcept;
    [[nodiscard]] const std::shared_ptr<ReadinessSource>&
    readiness_source() const noexcept
    {
        return readiness_;
    }
    void close() noexcept;
    void pump_for_testing() noexcept;

private:
    friend struct GroupSendCoordinatorTestAccess;
    struct Membership {
        Member state;
        std::weak_ptr<ConnectionRuntime> runtime;
        GroupRuntimeAdmission::Binding binding;
        std::optional<Clock::time_point> busy_since;
    };
    struct Recipient {
        std::uint64_t member = 0;
        std::optional<GroupRuntimeAdmission::Ticket> ticket;
        std::uint32_t busy_attempts = 0;
    };
    struct Frame {
        std::vector<Recipient> recipients;
        GroupMulticastEmitter::Outcome outcome;
        std::size_t count = 0;
        std::size_t remaining = 0;
        bool occupied = false;
        bool completed = false;
        std::optional<std::uint64_t> crypto_generation;
    };
    struct Work {
        std::weak_ptr<GroupSendCoordinator> owner;
    };
    struct Publication;
    explicit GroupSendCoordinator(Configuration);
    [[nodiscard]] static std::shared_ptr<GroupSendCoordinator> make(
        Configuration, std::shared_ptr<RuntimeScheduler>, CryptoProvider&,
        bool) noexcept;
    static void run(void*) noexcept;
    static bool publish(void*) noexcept;
    static bool skip(void*) noexcept;
    void drive() noexcept;
    void advance_cursor() noexcept;
    void detach(std::uint64_t, Detachment) noexcept;
    void update_readiness() noexcept;
    [[nodiscard]] bool crypto_writable() const noexcept;
    [[nodiscard]] Status advance_empty(Clock::time_point) noexcept;
    [[nodiscard]] Clock::time_point admission_now() const noexcept;

    mutable std::mutex publication_;
    Configuration configuration_;
    Snapshot state_;
    std::vector<Membership> members_;
    std::vector<std::shared_ptr<Frame>> frames_;
    std::size_t fanout_cursor_ = 0;
    std::uint64_t next_member_ = 1;
    std::uint64_t next_position_ = 0;
    std::size_t maximum_payload_ = 0;
    std::unique_ptr<GroupPacketEncoder> encoder_;
    std::shared_ptr<GroupMulticastEmitter> emitter_;
    std::shared_ptr<RuntimeScheduler> scheduler_;
    RuntimeScheduler::ServiceToken service_ {};
    std::shared_ptr<Work> work_;
    std::shared_ptr<ReadinessSource> readiness_;
    bool manual_ = false;
    std::optional<Clock::time_point> retry_at_;
};
} // namespace robotweax::srt::compat
