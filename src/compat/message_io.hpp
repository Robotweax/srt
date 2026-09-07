#pragma once

#include "compat/socket_registry.hpp"

#include <memory>

namespace robotweax::srt::compat {

struct GroupRecord;

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
