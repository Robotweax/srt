#pragma once

#include "robotweax/srt/error.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

namespace robotweax::srt {

enum class IpAddressFamily : std::uint8_t {
    ipv4,
    ipv6,
};

struct IpEndpoint {
    std::array<std::uint8_t, 16> address{};
    std::uint16_t port = 0;
    std::uint32_t scope_id = 0;
    IpAddressFamily family = IpAddressFamily::ipv4;

    [[nodiscard]] static constexpr IpEndpoint any(
        std::uint16_t port = 0) noexcept
    {
        return {.address = {0, 0, 0, 0}, .port = port};
    }

    [[nodiscard]] static constexpr IpEndpoint loopback(
        std::uint16_t port = 0) noexcept
    {
        return {.address = {127, 0, 0, 1}, .port = port};
    }

    [[nodiscard]] static constexpr IpEndpoint ipv6_any(
        std::uint16_t port = 0, std::uint32_t scope_id = 0) noexcept
    {
        return {
            .address = {},
            .port = port,
            .scope_id = scope_id,
            .family = IpAddressFamily::ipv6,
        };
    }

    [[nodiscard]] static constexpr IpEndpoint ipv6_loopback(
        std::uint16_t port = 0, std::uint32_t scope_id = 0) noexcept
    {
        IpEndpoint endpoint = ipv6_any(port, scope_id);
        endpoint.address[15] = 1;
        return endpoint;
    }

    [[nodiscard]] constexpr bool is_ipv4() const noexcept
    {
        return family == IpAddressFamily::ipv4;
    }

    [[nodiscard]] constexpr bool is_ipv6() const noexcept
    {
        return family == IpAddressFamily::ipv6;
    }

    [[nodiscard]] constexpr bool is_ipv4_mapped_ipv6() const noexcept
    {
        if (!is_ipv6() || address[10] != 0xffU
            || address[11] != 0xffU) {
            return false;
        }
        for (std::size_t index = 0; index < 10U; ++index) {
            if (address[index] != 0U) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] constexpr IpAddressFamily wire_family() const noexcept
    {
        return is_ipv4_mapped_ipv6()
            ? IpAddressFamily::ipv4
            : family;
    }

    [[nodiscard]] constexpr std::size_t address_size() const noexcept
    {
        return is_ipv4() ? 4U : 16U;
    }

    [[nodiscard]] constexpr bool is_wildcard() const noexcept
    {
        for (std::size_t index = 0; index < address_size(); ++index) {
            if (address[index] != 0U) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] friend constexpr bool operator==(
        const IpEndpoint&, const IpEndpoint&) noexcept = default;
};

// Retained as a source-compatible name for existing IPv4-only users.
using Ipv4Endpoint = IpEndpoint;

struct UdpIoResult {
    Error error = Error::none;
    std::size_t bytes_transferred = 0;
    IpEndpoint peer{};
    int system_error = 0;

    [[nodiscard]] constexpr explicit operator bool() const noexcept
    {
        return error == Error::none;
    }
};

struct EndpointResult {
    Error error = Error::none;
    IpEndpoint endpoint{};
    int system_error = 0;

    [[nodiscard]] constexpr explicit operator bool() const noexcept
    {
        return error == Error::none;
    }
};

struct UdpWaitResult {
    Error error = Error::none;
    bool ready = false;
    int system_error = 0;

    [[nodiscard]] constexpr explicit operator bool() const noexcept
    {
        return error == Error::none;
    }
};

struct UdpIntegerOptionResult {
    Error error = Error::none;
    std::int32_t value = 0;
    int system_error = 0;

    [[nodiscard]] constexpr explicit operator bool() const noexcept
    {
        return error == Error::none;
    }
};

inline constexpr std::size_t maximum_network_device_name_size = 16U;

struct UdpDeviceOptionResult {
    Error error = Error::none;
    std::array<char, maximum_network_device_name_size> name{};
    std::size_t size = 0;
    int system_error = 0;

    [[nodiscard]] constexpr explicit operator bool() const noexcept
    {
        return error == Error::none;
    }

    [[nodiscard]] constexpr std::string_view view() const noexcept
    {
        return {name.data(), size};
    }
};

/**
 * Move-only RAII owner of one nonblocking native UDP socket. Destruction closes
 * a valid socket. Instances are not internally synchronized; serialize access
 * to one instance. All result endpoints/strings are returned by value.
 */
class UdpSocket {
public:
    UdpSocket() noexcept;
    explicit UdpSocket(IpAddressFamily family) noexcept;
    ~UdpSocket();

    /**
     * Validates and adopts an already-bound, unconnected UDP socket, changing
     * it to nonblocking mode. Ownership transfers only when the returned object
     * is valid; otherwise the caller still owns the native socket.
     */
    [[nodiscard]] static UdpSocket acquire_native(
        std::uintptr_t native_socket) noexcept;

    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;
    UdpSocket(UdpSocket&& other) noexcept;
    UdpSocket& operator=(UdpSocket&& other) noexcept;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] IpAddressFamily family() const noexcept { return family_; }
    /** Relinquishes ownership without closing; the returned handle is caller-owned. */
    [[nodiscard]] std::uintptr_t release_native() noexcept;
    [[nodiscard]] int open_system_error() const noexcept { return open_system_error_; }
    [[nodiscard]] int last_system_error() const noexcept { return last_system_error_; }
    [[nodiscard]] Error set_send_buffer_size(std::int32_t bytes) noexcept;
    [[nodiscard]] Error set_receive_buffer_size(std::int32_t bytes) noexcept;
    [[nodiscard]] Error set_ipv6_only(bool enabled) noexcept;
    [[nodiscard]] UdpIntegerOptionResult ipv6_only() const noexcept;
    [[nodiscard]] Error set_ip_time_to_live(std::int32_t value) noexcept;
    [[nodiscard]] UdpIntegerOptionResult ip_time_to_live() const noexcept;
    [[nodiscard]] Error set_ip_type_of_service(std::int32_t value) noexcept;
    [[nodiscard]] UdpIntegerOptionResult ip_type_of_service() const noexcept;
    [[nodiscard]] static constexpr bool bind_to_device_supported() noexcept
    {
#if defined(__linux__)
        return true;
#else
        return false;
#endif
    }
    [[nodiscard]] Error set_bind_to_device(std::string_view name) noexcept;
    [[nodiscard]] UdpDeviceOptionResult bound_device() const noexcept;
    [[nodiscard]] Error bind(IpEndpoint endpoint) noexcept;
    [[nodiscard]] EndpointResult local_endpoint() const noexcept;
    /** Waits for readability; negative timeouts wait indefinitely. */
    [[nodiscard]] UdpWaitResult wait_readable(int timeout_milliseconds) noexcept;
    [[nodiscard]] UdpIoResult send_to(
        std::span<const std::byte> bytes,
        IpEndpoint destination) noexcept;
    // Consumes exactly one datagram. If it exceeds destination, the truncated
    // prefix is never returned as valid data: error is buffer_too_small and
    // bytes_transferred is destination.size().
    [[nodiscard]] UdpIoResult receive_from(std::span<std::byte> destination) noexcept;

private:
    struct UnopenedTag {
    };

    static constexpr std::uintptr_t invalid_native =
        (std::numeric_limits<std::uintptr_t>::max)();

    explicit UdpSocket(UnopenedTag) noexcept {}
    void close() noexcept;

    std::uintptr_t native_ = invalid_native;
    int open_system_error_ = 0;
    int last_system_error_ = 0;
    IpAddressFamily family_ = IpAddressFamily::ipv4;
};

} // namespace robotweax::srt
