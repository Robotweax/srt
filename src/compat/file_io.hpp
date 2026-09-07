#pragma once

#include "compat/socket_registry.hpp"

#include <cstdint>
#include <ostream>

namespace robotweax::srt::compat {

[[nodiscard]] std::int64_t send_file(
    SocketRecord* socket,
    const char* path,
    std::int64_t* offset,
    std::int64_t size,
    int block_size) noexcept;
[[nodiscard]] std::int64_t receive_file(
    SocketRecord* socket,
    const char* path,
    std::int64_t* offset,
    std::int64_t size,
    int block_size) noexcept;
[[nodiscard]] std::int64_t receive_file_stream(
    SocketRecord* socket,
    std::ostream& output,
    std::int64_t* offset,
    std::int64_t size,
    int block_size) noexcept;

} // namespace robotweax::srt::compat
