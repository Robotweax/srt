// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Robotweax GmbH and contributors
#pragma once

#include "srt/srt.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace robotweax::srt::compat {

// Registry-lock protected, allocation-free status history, not ownership.
// Expired handles remain invalid; allocators never recycle their numeric IDs.
//
// The ring keeps the insertion order for eviction; an open-addressing table
// twice its size answers membership in O(1) expected instead of scanning the
// whole history on every lookup of an unknown handle.
class ClosedHandleHistory {
public:
    static constexpr std::size_t capacity = 4096;

    void remember(SRTSOCKET handle) noexcept
    {
        if (handle <= 0) {
            return;
        }
        const SRTSOCKET evicted = handles_[next_];
        handles_[next_] = 0;
        if (evicted > 0) {
            erase(evicted); // may rebuild from the ring: slot already freed
        }
        handles_[next_] = handle;
        next_ = (next_ + 1U) % capacity;
        insert(handle);
    }

    [[nodiscard]] bool contains(SRTSOCKET handle) const noexcept
    {
        if (handle <= 0) {
            return false;
        }
        for (std::size_t probe = slot_of(handle), step = 0; step < table_size;
            probe = (probe + 1U) % table_size, ++step) {
            const SRTSOCKET entry = table_[probe];
            if (entry == empty) {
                return false;
            }
            if (entry == handle) {
                return true;
            }
        }
        return false;
    }

    void clear() noexcept
    {
        handles_.fill(0);
        table_.fill(empty);
        next_ = 0;
        tombstones_ = 0;
    }

private:
    static constexpr std::size_t table_size = capacity * 2U;
    static constexpr SRTSOCKET empty = 0;
    static constexpr SRTSOCKET tombstone = -1;

    [[nodiscard]] static std::size_t slot_of(SRTSOCKET handle) noexcept
    {
        // Handles are allocated in sequence and carry a group mask bit;
        // a multiplicative mix spreads both over the table.
        const auto mixed =
            static_cast<std::uint64_t>(static_cast<std::uint32_t>(handle))
            * 0x9E37'79B9'7F4A'7C15ULL;
        return static_cast<std::size_t>(mixed >> 51U) % table_size;
    }

    void insert(SRTSOCKET handle) noexcept
    {
        for (std::size_t probe = slot_of(handle), step = 0; step < table_size;
            probe = (probe + 1U) % table_size, ++step) {
            if (table_[probe] == empty || table_[probe] == tombstone) {
                if (table_[probe] == tombstone) {
                    --tombstones_;
                }
                table_[probe] = handle;
                return;
            }
        }
    }

    void erase(SRTSOCKET handle) noexcept
    {
        for (std::size_t probe = slot_of(handle), step = 0; step < table_size;
            probe = (probe + 1U) % table_size, ++step) {
            if (table_[probe] == empty) {
                return;
            }
            if (table_[probe] == handle) {
                table_[probe] = tombstone;
                if (++tombstones_ > capacity) {
                    rebuild();
                }
                return;
            }
        }
    }

    // Tombstones from steady-state churn would otherwise lengthen every
    // probe; rebuilding from the ring restores short chains.
    void rebuild() noexcept
    {
        table_.fill(empty);
        tombstones_ = 0;
        for (const SRTSOCKET handle : handles_) {
            if (handle > 0) {
                insert(handle);
            }
        }
    }

    std::array<SRTSOCKET, capacity> handles_ {};
    std::array<SRTSOCKET, table_size> table_ {};
    std::size_t next_ = 0;
    std::size_t tombstones_ = 0;
};

} // namespace robotweax::srt::compat
