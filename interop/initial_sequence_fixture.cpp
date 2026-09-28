// SPDX-License-Identifier: MIT
// Linked only into the static, test-enabled interoperability executable.
#include "compat/socket_registry.hpp"

SRTSOCKET create_socket_with_test_isn(std::int32_t sequence)
{
    if (sequence < 0) {
        return SRT_INVALID_SOCK;
    }
    const auto socket = srt_create_socket();
    const auto record =
        robotweax::srt::compat::SocketRegistry::instance().find(socket);
    if (record == nullptr) {
        return SRT_INVALID_SOCK;
    }
    // This socket was just created and has not been published to any worker.
    std::lock_guard lock(record->mutex);
    record->connection_initial_sequence = static_cast<std::uint32_t>(sequence);
    record->peer_connection_initial_sequence =
        static_cast<std::uint32_t>(sequence);
    return socket;
}
