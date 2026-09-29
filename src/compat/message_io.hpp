#pragma once

#include "compat/socket_registry.hpp"

#include <chrono>
#include <memory>
#include <optional>

namespace robotweax::srt::compat {

struct GroupRecord;

// The same logical receive decision used by srt_recvmsg2, without consuming a
// message or advancing the group's receive cursor.
struct GroupReceiveReadiness {
    bool message_ready = false;
    bool terminal_error = false;
    std::optional<std::chrono::steady_clock::time_point> next_delivery;
};

[[nodiscard]] GroupReceiveReadiness group_receive_readiness(
    const std::shared_ptr<GroupRecord>& group);

[[nodiscard]] int send_message(
    SocketRecord* socket,
    const char* buffer,
    int length,
    SRT_MSGCTRL* control) noexcept;
[[nodiscard]] int receive_message(
    SocketRecord* socket,
    char* buffer,
    int length,
    SRT_MSGCTRL* control) noexcept;
[[nodiscard]] int send_group_message(
    const std::shared_ptr<GroupRecord>& group,
    const char* buffer, int length,
    SRT_MSGCTRL* control) noexcept;
[[nodiscard]] int receive_group_message(
    const std::shared_ptr<GroupRecord>& group,
    char* buffer, int length,
    SRT_MSGCTRL* control) noexcept;

} // namespace robotweax::srt::compat
