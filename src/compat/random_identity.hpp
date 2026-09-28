// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Robotweax GmbH and contributors
#pragma once

#include "srt/srt.h"

#include <cstdint>
#include <optional>

namespace robotweax::srt::compat {

// Socket and group handles double as the SRT destination socket ID on the
// wire, and the initial sequence number bounds which DATA a receiver accepts.
// Neither may be predictable from values an observer has already seen. Both
// derive from keys drawn once from the cryptographic random source; without
// them, allocation fails closed instead of falling back to a predictable
// value.

// Handle bases occupy [1, SRTGROUP_MASK - 1]; group handles add SRTGROUP_MASK.
inline constexpr std::uint32_t registry_handle_space =
    static_cast<std::uint32_t>(SRTGROUP_MASK);

enum class HandleSpace : std::uint8_t {
    socket = 1,
    group = 2,
};

// Seeds the generator from the calling thread if it is not seeded yet.
// Called on the application's path into the runtime so that seeding normally
// happens there rather than on a runtime worker thread. Returns false if the
// random source failed; a later call retries.
bool prepare_random_identity() noexcept;

// Returns the handle base at allocation position `index` and advances
// `index` past it. The mapping is a keyed permutation of the 30-bit space:
// successive positions never repeat a handle, so a stale handle is never
// aliased, yet the sequence reveals nothing about the next handle. Returns
// SRT_INVALID_SOCK, leaving `index` unchanged, when the generator is not
// seeded, and SRT_INVALID_SOCK once all positions are used.
[[nodiscard]] SRTSOCKET next_registry_handle(
    HandleSpace space, std::uint32_t& index) noexcept;

// Uniform over the 31-bit SRT sequence space; no uniqueness is implied.
[[nodiscard]] std::optional<std::uint32_t> random_initial_sequence() noexcept;

} // namespace robotweax::srt::compat
