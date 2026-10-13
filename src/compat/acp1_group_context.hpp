// SPDX-License-Identifier: MIT
#pragma once
#include "robotweax/srt/connection_group.hpp"
#include "robotweax/srt/handshake_extensions.hpp"
#include "robotweax/srt/session_authentication.hpp"
#include "robotweax/srt/standby_rtt.hpp"
#include <algorithm>
namespace robotweax::srt::compat {
// Canonical v5 root binding. Local admission domains are deliberately absent.
inline std::array<std::byte, acp1_group_context_size> acp1_group_context(
    GroupMembership caller, GroupMembership listener,
    const PathIdentifier& path, bool has_standby,
    const StandbyRttIdentity& standby) noexcept
{
    std::array<std::byte, acp1_group_context_size> bytes {};
    const auto put32 = [&](std::size_t offset, std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i)
            bytes[offset + i] = std::byte(value >> (24U - 8U * i));
    };
    const auto membership = [&](std::size_t offset, GroupMembership value) {
        put32(offset, value.group_id);
        bytes[offset + 4] = std::byte(value.type);
        bytes[offset + 5] = std::byte(value.flags);
        bytes[offset + 6] = std::byte(value.weight >> 8U);
        bytes[offset + 7] = std::byte(value.weight);
    };
    membership(0, caller);
    membership(8, listener);
    put32(16, path.size);
    std::copy_n(path.bytes.begin(), std::min<std::size_t>(path.size, 32),
        bytes.begin() + 20);
    put32(52, has_standby ? 1U : 0U);
    if (has_standby) {
        std::copy(
            standby.caller.begin(), standby.caller.end(), bytes.begin() + 56);
        std::copy(standby.listener.begin(), standby.listener.end(),
            bytes.begin() + 72);
    }
    return bytes;
}
} // namespace robotweax::srt::compat
