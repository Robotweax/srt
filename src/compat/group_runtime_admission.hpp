#pragma once

#include "compat/group_multicast_emitter.hpp"
#include "compat/group_packet_encoder.hpp"
#include "compat/transport_runtime.hpp"

namespace robotweax::srt::compat {

class GroupSendBinding {
    friend class GroupRuntimeAdmission;
    std::weak_ptr<ConnectionRuntime> runtime_;
    std::uint32_t group_id_ = 0;
    std::uint64_t stream_generation_ = 0;
    std::uint64_t member_generation_ = 0;
    std::shared_ptr<SharedTransmitKeySchedule> schedule_;
    CryptoMode mode_ = CryptoMode::automatic;
};

// Optional state; ordinary connections incur only one null pointer.
struct GroupRuntimeSendState {
    std::uint64_t generation = 0;
    std::shared_ptr<const GroupSendBinding> binding;
};

// Internal single-original Live admission for coordinated group sending.
// Binding/revocation run outside emitter/registry locks. For an admission,
// reserve the emitter first, then try-reserve distinct selected runtimes in
// stable member order. Busy/full returns before sealing; release all guards
// before waiting. Never acquire registry/group metadata or do I/O while guards live.
class GroupRuntimeAdmission {
public:
    using Binding = std::shared_ptr<const GroupSendBinding>;
    using Clock = ConnectionRuntime::Clock;
    struct Configuration {
        std::uint32_t group_id = 0;
        std::uint64_t stream_generation = 0;
        Clock::time_point timestamp_origin {};
        IpAddressFamily family = IpAddressFamily::ipv4;
        std::shared_ptr<SharedTransmitKeySchedule> key_schedule;
        std::optional<SequenceNumber> initial_sequence;
    };
    struct Ticket {
        Binding binding;
        SendBuffer::GroupOriginalSubmission original;
    };
    enum class Status : std::uint8_t {
        ready,
        busy,
        full,
        stale,
        closed,
        invalid,
        expired,
        crypto_error,
    };
    enum class Application : std::uint8_t {
        applied,
        busy,
        stale,
        not_submitted,
        invalid_time,
        duplicate_or_retired,
    };

    class Guard {
    public:
        Guard(Guard&&) noexcept = default;
        Guard& operator=(Guard&&) = delete;
        ~Guard();
        [[nodiscard]] Status status() const noexcept;
        // Stage every selected member before invoking the publication commit.
        // A staged guard's commit is allocation-free and cannot lose capacity
        // or be closed/rebound until release. Call once within that transaction.
        [[nodiscard]] bool stage(
            std::shared_ptr<const GroupPacketEncoder::Packet>) noexcept;
        [[nodiscard]] std::optional<Ticket> commit() noexcept;
        void release() noexcept;

    private:
        friend class GroupRuntimeAdmission;
        explicit Guard(Status status) noexcept
            : status_(status)
        {
        }
        Guard(std::shared_ptr<ConnectionRuntime>, Binding,
            std::unique_lock<std::mutex>, DataHeader, std::size_t,
            std::uint64_t now, std::uint64_t expiration,
            std::optional<std::uint64_t> crypto_generation) noexcept;
        std::shared_ptr<ConnectionRuntime> runtime_;
        Binding binding_;
        std::unique_lock<std::mutex> lock_;
        std::shared_ptr<const GroupPacketEncoder::Packet> packet_;
        DataHeader header_ {};
        std::size_t plaintext_size_ = 0;
        std::uint64_t now_ = 0;
        std::uint64_t expiration_ = 0;
        std::optional<std::uint64_t> crypto_generation_;
        Status status_ = Status::invalid;
        bool committed_ = false;
    };

    // Require an empty Live message sender, matching timestamp/family/schedule,
    // with packet filters and session authentication disabled. Replacements
    // never reuse member generations; failure preserves the old binding.
    // Revocation retains external ownership.
    [[nodiscard]] static Binding bind(const std::shared_ptr<ConnectionRuntime>&,
        const Configuration&) noexcept;
    static void revoke(const Binding&) noexcept;
    // Busy also covers initial KM readiness or a not-yet-dispatched KM request;
    // polling must capture that request before a later admission may replace it.
    [[nodiscard]] static Guard try_reserve(const Binding&, DataHeader original,
        std::size_t plaintext_size,
        Clock::time_point expiration = Clock::time_point::max()) noexcept;
    // Call AFTER releasing all emitter/runtime guards, even after cancellation
    // if crypto preparation created a KM announcement. No original is emitted.
    static void notify_work(const Binding&) noexcept;
    // Call only after draining outcomes (no emitter/registry locks). Busy means
    // retain/retry the captured ticket in bounded coordinator storage; do not
    // resolve a replacement ticket by sequence. Non-submitted outcomes require
    // explicit policy and never authorize receiver progress here.
    [[nodiscard]] static Application try_apply(
        const Ticket&, const GroupMulticastEmitter::Outcome&) noexcept;

private:
    [[nodiscard]] static bool active_locked(
        const ConnectionRuntime&, const Binding&) noexcept;
};

} // namespace robotweax::srt::compat
