#pragma once

#include "compat/socket_registry.hpp"

namespace robotweax::srt::compat {

enum class BoundChannelRetirementStatus {
    retired,
    busy,
    timeout,
    worker_thread
};
// Detach binding identities and capture old channels using admission-prepared
// storage. Failed finishes retain ownership for an off-worker retry.
void prepare_bound_channel_registry() noexcept;
void prepare_bound_channel_retirement() noexcept;
[[nodiscard]] BoundChannelRetirementStatus finish_bound_channel_retirement(
    std::chrono::steady_clock::time_point deadline) noexcept;

[[nodiscard]] int decode_ip_endpoint(const sockaddr* name, int name_size,
    IpEndpoint& endpoint, bool allow_zero_port) noexcept;
[[nodiscard]] int write_ip_endpoint(
    IpEndpoint endpoint, sockaddr* name, int* name_size) noexcept;
[[nodiscard]] int bind_socket(
    SocketRecord* socket, const sockaddr* name, int name_size) noexcept;
[[nodiscard]] int bind_acquired_socket(
    SocketRecord* socket, UDPSOCKET native_socket) noexcept;
[[nodiscard]] int bind_any_socket(
    SocketRecord& socket, IpAddressFamily family) noexcept;
[[nodiscard]] int get_socket_name(
    SocketRecord& socket, sockaddr* name, int* name_size) noexcept;

} // namespace robotweax::srt::compat
