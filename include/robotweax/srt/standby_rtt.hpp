#pragma once

#include "robotweax/srt/codec.hpp"
#include "robotweax/srt/handshake_extensions.hpp"

#include <array>
#include <optional>

namespace robotweax::srt {

// Experimental bilateral identifiers, not registered standard SRT features.
inline constexpr std::uint16_t standby_rtt_subtype = 0x7f12;
inline constexpr std::size_t standby_rtt_extension_size = 40;
inline constexpr std::size_t standby_rtt_datagram_size = 64;
using StandbyRttNonce = std::array<std::byte, 16>;

[[nodiscard]] bool valid_standby_rtt_nonce(const StandbyRttNonce&) noexcept;

struct StandbyRttIdentity {
    StandbyRttNonce caller {}, listener {};
    [[nodiscard]] bool is_offer() const noexcept;
    [[nodiscard]] bool is_pair() const noexcept;
    friend bool operator==(
        const StandbyRttIdentity&, const StandbyRttIdentity&) = default;
};

struct StandbyRttIdentityResult {
    Error error = Error::none;
    StandbyRttIdentity identity {};
    [[nodiscard]] explicit operator bool() const noexcept
    {
        return error == Error::none;
    }
};

[[nodiscard]] StandbyRttIdentityResult decode_standby_rtt_identity(
    const HandshakeExtensionView&) noexcept;
[[nodiscard]] ExtensionEncodeResult encode_standby_rtt_identity(
    const StandbyRttIdentity&, std::span<std::byte>) noexcept;

enum class StandbyRttKind : std::uint32_t { request = 1, echo = 2 };
struct StandbyRttMessage {
    StandbyRttIdentity identity {};
    std::uint64_t probe_id = 0;
    StandbyRttKind kind = StandbyRttKind::request;
    bool caller_initiated = true;
};
struct StandbyRttDecodeResult {
    Error error = Error::none;
    StandbyRttMessage message {};
    std::uint32_t destination_socket_id = 0;
    [[nodiscard]] explicit operator bool() const noexcept
    {
        return error == Error::none;
    }
};
[[nodiscard]] StandbyRttDecodeResult decode_standby_rtt_datagram(
    std::span<const std::byte>) noexcept;
[[nodiscard]] EncodeResult encode_standby_rtt_datagram(const StandbyRttMessage&,
    PacketTimestamp, std::uint32_t destination_socket_id,
    std::span<std::byte>) noexcept;

enum class StandbyRttOutcome : std::uint8_t {
    none,
    pending,
    measured,
    timeout,
    send_skipped,
    paused,
    id_exhausted,
};
struct StandbyRttSnapshot {
    std::uint64_t rtt_microseconds = 0;
    std::uint64_t age_microseconds = UINT64_MAX;
    std::uint64_t samples = 0;
    StandbyRttOutcome outcome = StandbyRttOutcome::none;
    bool fresh = false;
};

/** Serialized, allocation-free measurement only. No Session, I/O or timer owner.
 * The transport must validate the peer endpoint before dispatch, supply a fresh
 * random negotiated pair and generation, and call finish_send for every attempt.
 * Supplied times are from one local monotonic clock; no wire timestamp is used.
 */
class StandbyRttMeasurement {
public:
    [[nodiscard]] Error configure(StandbyRttIdentity, std::uint64_t generation,
        std::uint64_t first_due, std::uint64_t first_probe_id = 1,
        bool local_caller = true) noexcept;
    [[nodiscard]] std::optional<StandbyRttMessage> prepare_request(
        std::uint64_t now, std::uint64_t idle_deadline,
        std::uint64_t generation, bool eligible) noexcept;
    void finish_send(std::uint64_t probe_id, std::uint64_t generation,
        std::optional<std::uint64_t> native_acceptance_time) noexcept;
    void receive_echo(const StandbyRttMessage&, std::uint64_t arrival,
        std::uint64_t generation, bool live) noexcept;
    [[nodiscard]] std::optional<StandbyRttMessage> make_echo(
        const StandbyRttMessage&, std::uint64_t now, std::uint64_t generation,
        bool live, bool send_budget) noexcept;
    void expire(std::uint64_t now) noexcept;
    void pause() noexcept;
    void terminate() noexcept;
    [[nodiscard]] StandbyRttSnapshot snapshot(std::uint64_t now) const noexcept;
    [[nodiscard]] bool outstanding() const noexcept
    {
        return pending_id_ != 0;
    }

private:
    void record_echo(std::uint64_t arrival) noexcept;
    StandbyRttIdentity identity_ {};
    std::uint64_t generation_ = 0, next_id_ = 1, next_due_ = 0;
    std::uint64_t pending_id_ = 0, prepared_at_ = 0, sent_at_ = 0,
                  deadline_ = 0, idle_deadline_ = 0;
    std::uint64_t early_echo_ = 0, last_sample_at_ = 0, next_echo_due_ = 0;
    StandbyRttSnapshot sample_ {};
    bool live_ = false, sent_ = false, has_early_echo_ = false,
         local_caller_ = true;
};

/** One externally serialized instance is intended for the process owner.
 * Lease generations survive clear() to reject stale work after cleanup/reuse.
 * The owner sets eligibility; grants scan at most the fixed 256 slots fairly.
 */
class StandbyRttBudget {
public:
    struct Lease {
        std::uint64_t generation = 0;
        std::uint16_t slot = 0;
        friend bool operator==(const Lease&, const Lease&) = default;
    };
    [[nodiscard]] std::optional<Lease> admit() noexcept;
    void release(Lease) noexcept;
    void set_eligible(Lease, bool) noexcept;
    [[nodiscard]] std::optional<Lease> grant_request(
        std::uint64_t now) noexcept;
    [[nodiscard]] bool grant_echo(Lease, std::uint64_t now) noexcept;
    void clear() noexcept;

private:
    [[nodiscard]] bool owns(Lease) const noexcept;
    std::array<std::uint64_t, 256> generations_ {};
    std::array<bool, 256> eligible_ {};
    std::uint64_t serial_ = 0, next_request_ = 0, next_echo_ = 0;
    std::uint16_t cursor_ = 0;
};

static_assert(sizeof(StandbyRttMeasurement) <= 256);
static_assert(sizeof(StandbyRttBudget) <= 4096);

} // namespace robotweax::srt
