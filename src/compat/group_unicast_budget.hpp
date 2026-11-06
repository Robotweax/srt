#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>
#include <utility>

namespace robotweax::srt::compat {
namespace hybrid {
class CoordinatorBridge;
}

// Aggregate canonical unicast DATA (originals AND repairs), including IP/UDP.
// One MTU of burst credit, initially full. No mutex is held across socket I/O.
// A single borrowed permit excludes concurrent sends/refill; successful UDP
// charges its reserved bytes, failure restores them. Time spent in socket I/O
// earns no credit. The owner must outlive every permit and borrowed clock.
// The clock runs under this mutex and must not reenter the budget.
class GroupUnicastBudget {
public:
    using Clock = std::chrono::steady_clock;
    using ClockFunction = Clock::time_point (*)(void*) noexcept;
    struct Configuration {
        std::uint64_t wire_bytes_per_second = 0;
        std::uint64_t maximum_wire_datagram = 1500;
        std::uint64_t ip_udp_overhead = 28;
        ClockFunction clock = nullptr;
        void* clock_context = nullptr;
    };
    struct Snapshot {
        std::uint64_t wire_bytes_per_second = 0;
        std::uint64_t burst_bytes = 0;
        std::uint64_t available_bytes = 0;
        std::uint64_t original_wire_bytes = 0;
        std::uint64_t repair_wire_bytes = 0;
        std::uint64_t submitted_packets = 0;
        std::uint64_t waits = 0;
        std::uint64_t cancelled_attempts = 0;
        bool inflight = false;
        bool closed = false;
    };
    class Permit {
    public:
        Permit() noexcept = default;
        Permit(const Permit&) = delete;
        Permit& operator=(const Permit&) = delete;
        Permit& operator=(Permit&&) = delete;
        Permit(Permit&& other) noexcept
            : owner_(std::exchange(other.owner_, nullptr))
            , bytes_(other.bytes_)
        {
        }
        ~Permit()
        {
            finish(false, false);
        }
        explicit operator bool() const noexcept
        {
            return owner_ != nullptr;
        }
        // Partial UDP success is a fatal transport violation, conservatively
        // charged as the full reserved datagram. Errors/cancellation charge 0.
        void finish(bool submitted, bool repair) noexcept
        {
            if (auto* owner = std::exchange(owner_, nullptr))
                owner->finish(bytes_, submitted, repair);
        }

    private:
        friend class GroupUnicastBudget;
        Permit(GroupUnicastBudget* owner, std::uint64_t bytes) noexcept
            : owner_(owner)
            , bytes_(bytes)
        {
        }
        GroupUnicastBudget* owner_ = nullptr;
        std::uint64_t bytes_ = 0;
    };
    explicit GroupUnicastBudget(Configuration c) noexcept
        : config_(c)
    {
        state_.wire_bytes_per_second = c.wire_bytes_per_second;
        state_.burst_bytes = state_.available_bytes = c.maximum_wire_datagram;
        state_.closed = !c.wire_bytes_per_second
            || c.maximum_wire_datagram > 65535U
            || c.ip_udp_overhead >= c.maximum_wire_datagram;
        last_ = now();
    }
    [[nodiscard]] Permit acquire(std::uint64_t datagram_bytes) noexcept
    {
        std::lock_guard lock(mutex_);
        refill();
        if (state_.closed || !datagram_bytes || !config_.wire_bytes_per_second
            || datagram_bytes > config_.maximum_wire_datagram
            || config_.ip_udp_overhead
                > config_.maximum_wire_datagram - datagram_bytes)
            return {};
        const auto bytes = datagram_bytes + config_.ip_udp_overhead;
        if (state_.inflight || bytes > state_.available_bytes) {
            add(state_.waits, 1);
            return {};
        }
        state_.available_bytes -= bytes;
        state_.inflight = true;
        return {this, bytes};
    }
    // A bounded retry also covers a concurrent native send, closure or a bad
    // shape. The runtime fences closure/history before touching UDP again.
    [[nodiscard]] std::chrono::microseconds delay(
        std::uint64_t datagram_bytes) noexcept
    {
        std::lock_guard lock(mutex_);
        refill();
        if (state_.inflight || state_.closed || !datagram_bytes
            || !config_.wire_bytes_per_second
            || datagram_bytes > config_.maximum_wire_datagram
            || config_.ip_udp_overhead
                > config_.maximum_wire_datagram - datagram_bytes)
            return std::chrono::milliseconds {1};
        const auto bytes = datagram_bytes + config_.ip_udp_overhead;
        if (bytes <= state_.available_bytes)
            return {};
        // Configuration is bounded to an IP datagram (<=65535 bytes).
        const auto numerator =
            (bytes - state_.available_bytes) * 1'000'000U - fraction_;
        const auto rate = config_.wire_bytes_per_second;
        return std::chrono::microseconds {
            numerator / rate + (numerator % rate != 0)};
    }
    [[nodiscard]] Snapshot snapshot() noexcept
    {
        std::lock_guard lock(mutex_);
        refill();
        return state_;
    }
    void close() noexcept
    {
        std::lock_guard lock(mutex_);
        state_.closed = true;
    }

private:
    static void add(std::uint64_t& value, std::uint64_t count) noexcept
    {
        value +=
            std::min(count, std::numeric_limits<std::uint64_t>::max() - value);
    }
    std::uint64_t now() const noexcept
    {
        const auto time =
            config_.clock ? config_.clock(config_.clock_context) : Clock::now();
        const auto count =
            std::chrono::duration_cast<std::chrono::microseconds>(
                time.time_since_epoch())
                .count();
        return count > 0 ? static_cast<std::uint64_t>(count) : 0;
    }
    void refill() noexcept
    {
        if (state_.inflight)
            return;
        const auto current = now();
        if (current <= last_)
            return;
        const auto elapsed = current - last_;
        last_ = current;
        auto room = state_.burst_bytes - state_.available_bytes;
        const auto credit = [&](std::uint64_t count, std::uint64_t rate) {
            if (rate && count > room / rate) {
                state_.available_bytes = state_.burst_bytes;
                fraction_ = 0;
                return false;
            }
            const auto bytes = count * rate;
            state_.available_bytes += bytes;
            room -= bytes;
            return true;
        };
        const auto rate = config_.wire_bytes_per_second;
        if (!credit(elapsed / 1'000'000U, rate)
            || !credit(elapsed % 1'000'000U, rate / 1'000'000U))
            return;
        const auto fractional =
            (elapsed % 1'000'000U) * (rate % 1'000'000U) + fraction_;
        if (!credit(1, fractional / 1'000'000U))
            return;
        fraction_ = room ? fractional % 1'000'000U : 0;
    }
    void finish(std::uint64_t bytes, bool submitted, bool repair) noexcept
    {
        std::lock_guard lock(mutex_);
        last_ = std::max(last_, now()); // No credit for time in UDP.
        state_.inflight = false;
        if (submitted) {
            add(repair ? state_.repair_wire_bytes : state_.original_wire_bytes,
                bytes);
            add(state_.submitted_packets, 1);
        } else {
            state_.available_bytes += bytes;
            add(state_.cancelled_attempts, 1);
        }
    }
    Configuration config_;
    friend class hybrid::CoordinatorBridge;
    std::mutex mutex_;
    Snapshot state_;
    std::uint64_t last_ = 0;
    std::uint64_t fraction_ = 0;
};
} // namespace robotweax::srt::compat
