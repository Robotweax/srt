#pragma once

#include "srt/srt.h"

#include <cstdint>

namespace robotweax::srt::compat {

void epoll_initialize() noexcept;
[[nodiscard]] int epoll_create() noexcept;
[[nodiscard]] int epoll_clear_usocks(int eid) noexcept;
[[nodiscard]] int epoll_add_usock(
    int eid, SRTSOCKET socket, const int* events) noexcept;
[[nodiscard]] int epoll_add_ssock(
    int eid, SYSSOCKET socket, const int* events) noexcept;
[[nodiscard]] int epoll_remove_usock(
    int eid, SRTSOCKET socket) noexcept;
[[nodiscard]] int epoll_remove_ssock(
    int eid, SYSSOCKET socket) noexcept;
[[nodiscard]] int epoll_update_usock(
    int eid, SRTSOCKET socket, const int* events) noexcept;
[[nodiscard]] int epoll_update_ssock(
    int eid, SYSSOCKET socket, const int* events) noexcept;
[[nodiscard]] int epoll_wait(
    int eid,
    SRTSOCKET* readfds, int* read_count,
    SRTSOCKET* writefds, int* write_count,
    std::int64_t timeout_milliseconds,
    SYSSOCKET* system_readfds, int* system_read_count,
    SYSSOCKET* system_writefds, int* system_write_count) noexcept;
[[nodiscard]] int epoll_uwait(
    int eid, SRT_EPOLL_EVENT* events, int event_count,
    std::int64_t timeout_milliseconds) noexcept;
[[nodiscard]] std::int32_t epoll_set(
    int eid, std::int32_t flags) noexcept;
[[nodiscard]] int epoll_release(int eid) noexcept;
void epoll_clear_all() noexcept;

} // namespace robotweax::srt::compat
