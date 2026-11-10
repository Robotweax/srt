// SPDX-License-Identifier: MIT
#pragma once
#include "robotweax/srt/udp.hpp"
#include <array>
#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace robotweax::srt::test {
// Test-local link model. Accepted sends may be delayed/lost by the modeled
// network; bodies and original UDP source sockets remain unchanged.
class HybridFaultQueue {
public:
    static constexpr std::size_t capacity = 64, datagram_capacity = 1500;
    struct Rules {
        std::uint32_t delay_us = 0, alternating_delay_us = 0, drop_every = 0,
                      duplicate_every = 0, retry_every = 0, drop_first = 0;
    };
    enum class Result { accepted, retry, invalid };
    struct Packet {
        std::array<std::byte, datagram_capacity> bytes {};
        IpEndpoint peer;
        std::size_t size = 0;
        std::uint64_t due_us = 0, ordinal = 0;
        bool occupied = false;
    };
    struct Snapshot {
        std::uint64_t attempts = 0, accepted = 0, dropped = 0, duplicates = 0,
                      retries = 0, overflows = 0, delivered = 0;
        std::size_t queued = 0, peak = 0;
    };
    [[nodiscard]] static bool valid(Rules r) noexcept
    {
        return r.delay_us <= 250'000 && r.alternating_delay_us <= 250'000;
    }
    [[nodiscard]] Result enqueue(std::span<const std::byte> bytes,
        IpEndpoint peer, std::uint64_t now, Rules r) noexcept
    {
        if (!valid(r) || bytes.empty() || bytes.size() > datagram_capacity
            || state_.attempts == UINT64_MAX || ordinal_ >= UINT64_MAX - 2)
            return Result::invalid;
        const auto delay = static_cast<std::uint64_t>(r.delay_us)
            + ((state_.accepted % 2 == 0) ? r.alternating_delay_us : 0);
        if (now > UINT64_MAX - delay - 1)
            return Result::invalid;
        const auto attempt = ++state_.attempts;
        if (r.retry_every && attempt % r.retry_every == 0) {
            ++state_.retries;
            return Result::retry;
        }
        const auto accepted = state_.accepted + 1;
        if (accepted <= r.drop_first
            || (r.drop_every && accepted % r.drop_every == 0)) {
            ++state_.accepted;
            ++state_.dropped;
            return Result::accepted;
        }
        const bool duplicate =
            r.duplicate_every && accepted % r.duplicate_every == 0;
        const auto copies = duplicate ? 2U : 1U;
        if (capacity - state_.queued < copies) {
            ++state_.overflows;
            return Result::retry;
        }
        ++state_.accepted;
        if (duplicate)
            ++state_.duplicates;
        for (unsigned copy = 0; copy < copies; ++copy) {
            auto& p = *std::find_if(
                packets_->begin(), packets_->end(), [](const auto& p) {
                    return !p.occupied;
                });
            std::copy(bytes.begin(), bytes.end(), p.bytes.begin());
            p.peer = peer;
            p.size = bytes.size();
            p.due_us = now + delay + copy;
            p.ordinal = ++ordinal_;
            p.occupied = true;
            ++state_.queued;
        }
        state_.peak = std::max(state_.peak, state_.queued);
        return Result::accepted;
    }
    [[nodiscard]] std::optional<Packet> take(std::uint64_t now) noexcept
    {
        Packet* first = nullptr;
        for (auto& p : *packets_)
            if (p.occupied && p.due_us <= now
                && (!first || p.due_us < first->due_us
                    || (p.due_us == first->due_us
                        && p.ordinal < first->ordinal)))
                first = &p;
        if (!first)
            return {};
        auto p = *first;
        first->occupied = false;
        --state_.queued;
        ++state_.delivered;
        return p;
    }
    [[nodiscard]] Snapshot snapshot() const noexcept
    {
        return state_;
    }

private:
    // Allocate the fixed pool once, before installing any send hook. Multiple
    // links otherwise put nearly 1 MiB in a sanitizer-instrumented stack frame.
    // Enqueue/take still allocate nothing and retain the same hard capacity.
    std::unique_ptr<std::array<Packet, capacity>> packets_ =
        std::make_unique<std::array<Packet, capacity>>();
    Snapshot state_;
    std::uint64_t ordinal_ = 0;
};
}
