#pragma once
// Sender-side parity emission policy for a fixed negotiated FEC geometry.
// The receiver already tolerates lost repair packets, so a sender may
// withhold a ready row or column parity packet without any wire change.
//
// Pure policy: no clock, socket, allocation or SRT dependency. The functions,
// constants and enumerations mirror the sealed reader
// (scripts/tail_probe_budget_comparison.py of the adaptive recovery study)
// and keep its names and arithmetic. Only Mode::fixed and Mode::deadline_only
// are reachable from the transport (SocketOption::fec_policy `always` and
// `arqfirst`); the adaptive and evidence_only branches and their constants
// stay as the study defined them and are not wired to any option.
#include <cstdint>
#include <optional>

namespace robotweax::srt::parity {

enum class Mode : std::uint8_t { fixed, adaptive, deadline_only, evidence_only };
enum class Level : std::uint8_t { full, row_only, none };
enum class Kind : std::uint8_t { row, column };

struct Constants {
    // Negotiated receive delay. A retransmission must arrive within it.
    std::uint64_t latency_us = 120'000;
    // Receiver gap detection at the workload's 1 ms source interval.
    std::uint64_t detect_us = 2'000;
    // libsrt minimum periodic NAK interval, covering one lost NAK or reply.
    std::uint64_t nak_interval_us = 20'000;
    std::uint64_t guard_us = 5'000;
    // NAK-free time with feedback flowing before protection is thinned.
    std::uint64_t clean_hold_us = 100'000;
    // A NAK whose longest range reaches burst_threshold keeps full parity
    // for burst_memory_us after it arrived.
    std::uint64_t burst_memory_us = 1'000'000;
    unsigned burst_threshold = 2;
};

struct Inputs {
    std::uint64_t now_us = 0;
    // True once the native estimator holds a non-default sample.
    bool rtt_known = false;
    std::uint32_t srtt_us = 0;
    std::uint32_t rttvar_us = 0;
    // Arrival time at the sender of the latest NAK, and of the latest NAK
    // carrying a loss range of at least burst_threshold sequences. Not
    // consulted by Mode::deadline_only.
    std::optional<std::uint64_t> last_nak_us;
    std::optional<std::uint64_t> last_burst_us;
};

// Two ARQ rounds, tolerating one lost NAK or reply, must fit the deadline.
// Without a measured RTT the bound is unknown and ARQ is not trusted.
[[nodiscard]] inline bool arq_meets_deadline(
    const Inputs& in, const Constants& c) noexcept
{
    if (!in.rtt_known) return false;
    const std::uint64_t bound = c.detect_us + 2ULL * in.srtt_us
        + 4ULL * in.rttvar_us + c.nak_interval_us + c.guard_us;
    return bound < c.latency_us;
}

[[nodiscard]] inline Level decide(
    Mode mode, const Inputs& in, const Constants& c) noexcept
{
    if (mode == Mode::fixed) return Level::full;
    if (mode != Mode::evidence_only && arq_meets_deadline(in, c)) return Level::none;
    if (mode == Mode::deadline_only) return Level::full;
    // Where ARQ cannot meet the deadline, never remove all protection: the
    // floor is row parity, which repairs one isolated loss per row.
    if (!in.rtt_known) return Level::full;
    if (in.last_nak_us && in.now_us < *in.last_nak_us) return Level::full;
    const std::uint64_t since_nak = in.now_us - in.last_nak_us.value_or(0);
    if (since_nak < c.clean_hold_us) return Level::full;
    if (in.last_burst_us && in.now_us - *in.last_burst_us < c.burst_memory_us) return Level::full;
    return Level::row_only;
}

[[nodiscard]] inline bool emit(Level level, Kind kind) noexcept
{
    return level == Level::full || (level == Level::row_only && kind == Kind::row);
}

// Tail loss probe idle interval: due when no original source has been
// submitted for max(2 * srtt, 10 ms), capped at 120 ms. Mirrors the sealed
// reader's probe_interval(srtt) = min(120000, max(10000, 2 * srtt)).
inline constexpr std::uint64_t probe_interval_floor_us = 10'000;
inline constexpr std::uint64_t probe_interval_cap_us = 120'000;
inline constexpr unsigned probe_limit = 3;

[[nodiscard]] constexpr std::uint64_t probe_interval(std::uint32_t srtt_us) noexcept
{
    const std::uint64_t doubled = 2ULL * srtt_us;
    const std::uint64_t floored =
        doubled > probe_interval_floor_us ? doubled : probe_interval_floor_us;
    return floored < probe_interval_cap_us ? floored : probe_interval_cap_us;
}

} // namespace robotweax::srt::parity
