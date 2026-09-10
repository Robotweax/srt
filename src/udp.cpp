#include "robotweax/srt/udp.hpp"
#include "udp_buffer_policy.hpp"

#include "compat/platform_networking.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstring>
#include <utility>

#if defined(_WIN32)
#  if !defined(NOMINMAX)
#    define NOMINMAX
#  endif
#  if !defined(WIN32_LEAN_AND_MEAN)
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#  include <fcntl.h>
#  if defined(__linux__)
#    include <net/if.h>
#  endif
#  include <netinet/in.h>
#  include <poll.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

namespace robotweax::srt {
namespace {

#if defined(__linux__)
static_assert(maximum_network_device_name_size == IFNAMSIZ,
    "The compatible SO_BINDTODEVICE ABI requires IFNAMSIZ bytes");
#endif

#if defined(_WIN32)
using NativeSocket = SOCKET;
using SocketLength = int;
constexpr NativeSocket invalid_socket = INVALID_SOCKET;
constexpr int invalid_socket_type_error = WSAEPROTOTYPE;
constexpr int invalid_address_family_error = WSAEAFNOSUPPORT;
constexpr int connected_socket_error = WSAEISCONN;
constexpr int invalid_socket_argument_error = WSAEINVAL;

[[nodiscard]] int last_socket_error() noexcept { return WSAGetLastError(); }
[[nodiscard]] bool is_would_block(int error) noexcept { return error == WSAEWOULDBLOCK; }
void close_socket(NativeSocket socket) noexcept { (void)closesocket(socket); }

[[nodiscard]] bool make_nonblocking(NativeSocket socket) noexcept
{
    u_long enabled = 1;
    return ioctlsocket(socket, FIONBIO, &enabled) == 0;
}

[[nodiscard]] bool is_not_connected_error(int error) noexcept
{
    return error == WSAENOTCONN;
}
#else
using NativeSocket = int;
using SocketLength = socklen_t;
constexpr NativeSocket invalid_socket = -1;
constexpr int invalid_socket_type_error = EPROTOTYPE;
constexpr int invalid_address_family_error = EAFNOSUPPORT;
constexpr int connected_socket_error = EISCONN;
constexpr int invalid_socket_argument_error = EINVAL;

[[nodiscard]] int last_socket_error() noexcept { return errno; }
[[nodiscard]] bool is_would_block(int error) noexcept
{
    return error == EAGAIN || error == EWOULDBLOCK;
}
void close_socket(NativeSocket socket) noexcept { (void)::close(socket); }
[[nodiscard]] bool make_nonblocking(NativeSocket socket) noexcept
{
    const int flags = fcntl(socket, F_GETFL, 0);
    return flags >= 0 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
}

[[nodiscard]] bool is_not_connected_error(int error) noexcept
{
    return error == ENOTCONN;
}
#endif

[[nodiscard]] NativeSocket to_native(std::uintptr_t value) noexcept
{
    return static_cast<NativeSocket>(value);
}

struct SocketAddress {
    sockaddr_storage value{};
    SocketLength size = 0;
};

[[nodiscard]] SocketAddress to_sockaddr(IpEndpoint endpoint) noexcept
{
    SocketAddress result;
    if (endpoint.is_ipv6()) {
        sockaddr_in6 address{};
#if defined(__APPLE__)
        address.sin6_len = static_cast<std::uint8_t>(sizeof(address));
#endif
        address.sin6_family = AF_INET6;
        address.sin6_port = htons(endpoint.port);
        address.sin6_scope_id = endpoint.scope_id;
        std::copy(endpoint.address.begin(), endpoint.address.end(),
            address.sin6_addr.s6_addr);
        std::memcpy(&result.value, &address, sizeof(address));
        result.size = static_cast<SocketLength>(sizeof(address));
        return result;
    }

    sockaddr_in address{};
#if defined(__APPLE__)
    address.sin_len = static_cast<std::uint8_t>(sizeof(address));
#endif
    address.sin_family = AF_INET;
    address.sin_port = htons(endpoint.port);
    const std::uint32_t host_address =
        (static_cast<std::uint32_t>(endpoint.address[0]) << 24U)
        | (static_cast<std::uint32_t>(endpoint.address[1]) << 16U)
        | (static_cast<std::uint32_t>(endpoint.address[2]) << 8U)
        | static_cast<std::uint32_t>(endpoint.address[3]);
    address.sin_addr.s_addr = htonl(host_address);
    std::memcpy(&result.value, &address, sizeof(address));
    result.size = static_cast<SocketLength>(sizeof(address));
    return result;
}

[[nodiscard]] IpEndpoint from_sockaddr(
    const sockaddr_storage& storage) noexcept
{
    if (storage.ss_family == AF_INET6) {
        const auto& address =
            reinterpret_cast<const sockaddr_in6&>(storage);
        IpEndpoint endpoint = IpEndpoint::ipv6_any(
            ntohs(address.sin6_port), address.sin6_scope_id);
        std::copy(std::begin(address.sin6_addr.s6_addr),
            std::end(address.sin6_addr.s6_addr),
            endpoint.address.begin());
        return endpoint;
    }

    const auto& address =
        reinterpret_cast<const sockaddr_in&>(storage);
    const std::uint32_t host_address = ntohl(address.sin_addr.s_addr);
    return {
        .address = {
            static_cast<std::uint8_t>((host_address >> 24U) & 0xffU),
            static_cast<std::uint8_t>((host_address >> 16U) & 0xffU),
            static_cast<std::uint8_t>((host_address >> 8U) & 0xffU),
            static_cast<std::uint8_t>(host_address & 0xffU),
        },
        .port = ntohs(address.sin_port),
    };
}

} // namespace

UdpSocket::UdpSocket() noexcept
    : UdpSocket(IpAddressFamily::ipv4)
{
}

UdpSocket::UdpSocket(IpAddressFamily family) noexcept
    : family_(family)
{
    const int startup_error = compat::initialize_platform_networking();
    if (startup_error != 0) {
        open_system_error_ = startup_error;
        return;
    }
    const int native_family =
        family == IpAddressFamily::ipv4 ? AF_INET : AF_INET6;
    const NativeSocket socket =
        ::socket(native_family, SOCK_DGRAM, IPPROTO_UDP);
    if (socket == invalid_socket) {
        open_system_error_ = last_socket_error();
        return;
    }
    if (!make_nonblocking(socket)) {
        open_system_error_ = last_socket_error();
        close_socket(socket);
        return;
    }
    native_ = static_cast<std::uintptr_t>(socket);
}

UdpSocket UdpSocket::acquire_native(
    std::uintptr_t native_socket) noexcept
{
    UdpSocket acquired{UnopenedTag{}};
    const int startup_error = compat::initialize_platform_networking();
    if (startup_error != 0) {
        acquired.open_system_error_ = startup_error;
        return acquired;
    }

    const NativeSocket socket = to_native(native_socket);
    if (socket == invalid_socket) {
#if defined(_WIN32)
        acquired.open_system_error_ = WSAENOTSOCK;
#else
        acquired.open_system_error_ = EBADF;
#endif
        return acquired;
    }

    int type = 0;
    SocketLength type_size =
        static_cast<SocketLength>(sizeof(type));
#if defined(_WIN32)
    const int type_result = ::getsockopt(socket, SOL_SOCKET, SO_TYPE,
        reinterpret_cast<char*>(&type), &type_size);
#else
    const int type_result = ::getsockopt(
        socket, SOL_SOCKET, SO_TYPE, &type, &type_size);
#endif
    if (type_result != 0 || type != SOCK_DGRAM) {
        acquired.open_system_error_ =
            type_result == 0
            ? invalid_socket_type_error
            : last_socket_error();
        return acquired;
    }

    sockaddr_storage local{};
    SocketLength local_size =
        static_cast<SocketLength>(sizeof(local));
    if (::getsockname(socket,
            reinterpret_cast<sockaddr*>(&local),
            &local_size)
        != 0) {
        acquired.open_system_error_ =
            last_socket_error();
        return acquired;
    }
    const bool ipv4 = local.ss_family == AF_INET
        && local_size >= static_cast<SocketLength>(sizeof(sockaddr_in));
    const bool ipv6 = local.ss_family == AF_INET6
        && local_size >= static_cast<SocketLength>(sizeof(sockaddr_in6));
    const bool has_port = ipv4
        ? reinterpret_cast<const sockaddr_in*>(&local)->sin_port != 0
        : ipv6
        && reinterpret_cast<const sockaddr_in6*>(&local)->sin6_port != 0;
    if ((!ipv4 && !ipv6) || !has_port) {
        acquired.open_system_error_ =
            !ipv4 && !ipv6
            ? invalid_address_family_error
            : invalid_socket_argument_error;
        return acquired;
    }

    sockaddr_storage peer{};
    SocketLength peer_size =
        static_cast<SocketLength>(sizeof(peer));
    if (::getpeername(socket,
            reinterpret_cast<sockaddr*>(&peer),
            &peer_size)
        == 0) {
        acquired.open_system_error_ =
            connected_socket_error;
        return acquired;
    }
    const int peer_error = last_socket_error();
    if (!is_not_connected_error(peer_error)) {
        acquired.open_system_error_ = peer_error;
        return acquired;
    }

    if (!make_nonblocking(socket)) {
        acquired.open_system_error_ = last_socket_error();
        return acquired;
    }
    acquired.native_ = native_socket;
    acquired.family_ = ipv4
        ? IpAddressFamily::ipv4
        : IpAddressFamily::ipv6;
    return acquired;
}

UdpSocket::~UdpSocket() { close(); }

UdpSocket::UdpSocket(UdpSocket&& other) noexcept
    : native_(std::exchange(other.native_, invalid_native))
    , open_system_error_(std::exchange(other.open_system_error_, 0))
    , last_system_error_(std::exchange(other.last_system_error_, 0))
    , family_(std::exchange(
          other.family_, IpAddressFamily::ipv4))
{
}

UdpSocket& UdpSocket::operator=(UdpSocket&& other) noexcept
{
    if (this != &other) {
        close();
        native_ = std::exchange(other.native_, invalid_native);
        open_system_error_ = std::exchange(other.open_system_error_, 0);
        last_system_error_ = std::exchange(other.last_system_error_, 0);
        family_ = std::exchange(
            other.family_, IpAddressFamily::ipv4);
    }
    return *this;
}

bool UdpSocket::valid() const noexcept { return native_ != invalid_native; }

std::uintptr_t UdpSocket::release_native() noexcept
{
    return std::exchange(native_, invalid_native);
}

void UdpSocket::close() noexcept
{
    if (valid()) {
        close_socket(to_native(native_));
        native_ = invalid_native;
    }
}

Error UdpSocket::set_send_buffer_size(std::int32_t bytes) noexcept
{
    if (!valid()) {
        last_system_error_ = open_system_error_;
        return Error::io_error;
    }
    if (bytes <= 0) {
        last_system_error_ = 0;
        return Error::invalid_state;
    }
#if defined(_WIN32)
    const int result = ::setsockopt(to_native(native_), SOL_SOCKET, SO_SNDBUF,
        reinterpret_cast<const char*>(&bytes), static_cast<int>(sizeof(bytes)));
#else
    const int error = detail::configure_udp_buffer(
        bytes,
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)         \
    || defined(__NetBSD__) || defined(__DragonFly__)
        true,
#else
        false,
#endif
        [&](std::int32_t value) {
            return ::setsockopt(to_native(native_), SOL_SOCKET, SO_SNDBUF,
                       &value, sizeof(value))
                    == 0
                ? 0
                : last_socket_error();
        },
        [&](std::int32_t& value) {
            SocketLength size = sizeof(value);
            return ::getsockopt(
                       to_native(native_), SOL_SOCKET, SO_SNDBUF, &value, &size)
                    == 0
                ? 0
                : last_socket_error();
        });
    last_system_error_ = error;
    return error == 0 ? Error::none : Error::io_error;
#endif
#if defined(_WIN32)
    last_system_error_ = result == 0 ? 0 : last_socket_error();
    return result == 0 ? Error::none : Error::io_error;
#endif
}

Error UdpSocket::set_receive_buffer_size(std::int32_t bytes) noexcept
{
    if (!valid()) {
        last_system_error_ = open_system_error_;
        return Error::io_error;
    }
    if (bytes <= 0) {
        last_system_error_ = 0;
        return Error::invalid_state;
    }
#if defined(_WIN32)
    const int result = ::setsockopt(to_native(native_), SOL_SOCKET, SO_RCVBUF,
        reinterpret_cast<const char*>(&bytes), static_cast<int>(sizeof(bytes)));
#else
    const int error = detail::configure_udp_buffer(
        bytes,
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)         \
    || defined(__NetBSD__) || defined(__DragonFly__)
        true,
#else
        false,
#endif
        [&](std::int32_t value) {
            return ::setsockopt(to_native(native_), SOL_SOCKET, SO_RCVBUF,
                       &value, sizeof(value))
                    == 0
                ? 0
                : last_socket_error();
        },
        [&](std::int32_t& value) {
            SocketLength size = sizeof(value);
            return ::getsockopt(
                       to_native(native_), SOL_SOCKET, SO_RCVBUF, &value, &size)
                    == 0
                ? 0
                : last_socket_error();
        });
    last_system_error_ = error;
    return error == 0 ? Error::none : Error::io_error;
#endif
#if defined(_WIN32)
    last_system_error_ = result == 0 ? 0 : last_socket_error();
    return result == 0 ? Error::none : Error::io_error;
#endif
}

Error UdpSocket::set_ipv6_only(bool enabled) noexcept
{
    if (!valid()) {
        last_system_error_ = open_system_error_;
        return Error::io_error;
    }
    if (family_ != IpAddressFamily::ipv6) {
        last_system_error_ = invalid_address_family_error;
        return Error::invalid_state;
    }
    const int value = enabled ? 1 : 0;
#if defined(_WIN32)
    const int result = ::setsockopt(to_native(native_), IPPROTO_IPV6,
        IPV6_V6ONLY, reinterpret_cast<const char*>(&value),
        static_cast<int>(sizeof(value)));
#else
    const int result = ::setsockopt(to_native(native_), IPPROTO_IPV6,
        IPV6_V6ONLY, &value, static_cast<SocketLength>(sizeof(value)));
#endif
    last_system_error_ = result == 0 ? 0 : last_socket_error();
    return result == 0 ? Error::none : Error::io_error;
}

UdpIntegerOptionResult UdpSocket::ipv6_only() const noexcept
{
    if (!valid()) {
        return {
            .error = Error::io_error,
            .value = -1,
            .system_error = open_system_error_,
        };
    }
    if (family_ != IpAddressFamily::ipv6) {
        return {
            .error = Error::invalid_state,
            .value = -1,
            .system_error = invalid_address_family_error,
        };
    }
    int value = 0;
    SocketLength size = static_cast<SocketLength>(sizeof(value));
#if defined(_WIN32)
    const int result = ::getsockopt(to_native(native_), IPPROTO_IPV6,
        IPV6_V6ONLY, reinterpret_cast<char*>(&value), &size);
#else
    const int result = ::getsockopt(to_native(native_), IPPROTO_IPV6,
        IPV6_V6ONLY, &value, &size);
#endif
    if (result != 0) {
        return {
            .error = Error::io_error,
            .value = -1,
            .system_error = last_socket_error(),
        };
    }
    return {.value = value != 0 ? 1 : 0};
}

Error UdpSocket::set_ip_time_to_live(std::int32_t value) noexcept
{
    if (!valid()) {
        last_system_error_ = open_system_error_;
        return Error::io_error;
    }
    if (value < 1 || value > 255) {
        last_system_error_ = 0;
        return Error::invalid_state;
    }
    const int level = family_ == IpAddressFamily::ipv4
        ? IPPROTO_IP : IPPROTO_IPV6;
    const int option = family_ == IpAddressFamily::ipv4
        ? IP_TTL : IPV6_UNICAST_HOPS;
#if defined(_WIN32)
    const int result = ::setsockopt(to_native(native_), level, option,
        reinterpret_cast<const char*>(&value),
        static_cast<int>(sizeof(value)));
#else
    const int result = ::setsockopt(to_native(native_), level, option,
        &value, static_cast<SocketLength>(sizeof(value)));
#endif
    last_system_error_ = result == 0 ? 0 : last_socket_error();
    return result == 0 ? Error::none : Error::io_error;
}

UdpIntegerOptionResult UdpSocket::ip_time_to_live() const noexcept
{
    if (!valid()) {
        return {.error = Error::io_error, .system_error = open_system_error_};
    }
    int value = 0;
    SocketLength size = static_cast<SocketLength>(sizeof(value));
    const int level = family_ == IpAddressFamily::ipv4
        ? IPPROTO_IP : IPPROTO_IPV6;
    const int option = family_ == IpAddressFamily::ipv4
        ? IP_TTL : IPV6_UNICAST_HOPS;
#if defined(_WIN32)
    const int result = ::getsockopt(to_native(native_), level, option,
        reinterpret_cast<char*>(&value), &size);
#else
    const int result = ::getsockopt(to_native(native_), level, option,
        &value, &size);
#endif
    if (result != 0) {
        return {.error = Error::io_error, .system_error = last_socket_error()};
    }
    return {.value = value};
}

Error UdpSocket::set_ip_type_of_service(std::int32_t value) noexcept
{
    if (!valid()) {
        last_system_error_ = open_system_error_;
        return Error::io_error;
    }
    if (value < 0 || value > 255) {
        last_system_error_ = 0;
        return Error::invalid_state;
    }
#if defined(_WIN32)
    if (family_ == IpAddressFamily::ipv6) {
        last_system_error_ = 0;
        return Error::unsupported;
    }
#endif
    const int level = family_ == IpAddressFamily::ipv4
        ? IPPROTO_IP : IPPROTO_IPV6;
    const int option = family_ == IpAddressFamily::ipv4
        ? IP_TOS : IPV6_TCLASS;
#if defined(_WIN32)
    const int result = ::setsockopt(to_native(native_), level, option,
        reinterpret_cast<const char*>(&value),
        static_cast<int>(sizeof(value)));
#else
    const int result = ::setsockopt(to_native(native_), level, option,
        &value, static_cast<SocketLength>(sizeof(value)));
#endif
    last_system_error_ = result == 0 ? 0 : last_socket_error();
    return result == 0 ? Error::none : Error::io_error;
}

UdpIntegerOptionResult UdpSocket::ip_type_of_service() const noexcept
{
    if (!valid()) {
        return {.error = Error::io_error, .system_error = open_system_error_};
    }
#if defined(_WIN32)
    if (family_ == IpAddressFamily::ipv6) {
        return {.error = Error::unsupported};
    }
#endif
    int value = 0;
    SocketLength size = static_cast<SocketLength>(sizeof(value));
    const int level = family_ == IpAddressFamily::ipv4
        ? IPPROTO_IP : IPPROTO_IPV6;
    const int option = family_ == IpAddressFamily::ipv4
        ? IP_TOS : IPV6_TCLASS;
#if defined(_WIN32)
    const int result = ::getsockopt(to_native(native_), level, option,
        reinterpret_cast<char*>(&value), &size);
#else
    const int result = ::getsockopt(to_native(native_), level, option,
        &value, &size);
#endif
    if (result != 0) {
        return {.error = Error::io_error, .system_error = last_socket_error()};
    }
    return {.value = value};
}

Error UdpSocket::set_bind_to_device(std::string_view name) noexcept
{
#if defined(__linux__) && defined(SO_BINDTODEVICE)
    if (!valid()) {
        last_system_error_ = open_system_error_;
        return Error::io_error;
    }
    if (name.size() >= maximum_network_device_name_size
        || name.find('\0') != std::string_view::npos) {
        last_system_error_ = 0;
        return Error::invalid_state;
    }
    std::array<char, maximum_network_device_name_size> terminated{};
    std::copy(name.begin(), name.end(), terminated.begin());
    const auto size = static_cast<SocketLength>(
        name.empty() ? 0U : name.size() + 1U);
    const int result = ::setsockopt(to_native(native_), SOL_SOCKET,
        SO_BINDTODEVICE, terminated.data(), size);
    last_system_error_ = result == 0 ? 0 : last_socket_error();
    return result == 0 ? Error::none : Error::io_error;
#else
    (void)name;
    last_system_error_ = 0;
    return Error::unsupported;
#endif
}

UdpDeviceOptionResult UdpSocket::bound_device() const noexcept
{
#if defined(__linux__) && defined(SO_BINDTODEVICE)
    if (!valid()) {
        return {.error = Error::io_error, .system_error = open_system_error_};
    }
    UdpDeviceOptionResult result;
    SocketLength size = static_cast<SocketLength>(result.name.size());
    if (::getsockopt(to_native(native_), SOL_SOCKET, SO_BINDTODEVICE,
            result.name.data(), &size) != 0) {
        return {.error = Error::io_error, .system_error = last_socket_error()};
    }
    const auto available = std::min<std::size_t>(
        static_cast<std::size_t>(size), result.name.size());
    const auto end = std::find(result.name.begin(),
        result.name.begin() + static_cast<std::ptrdiff_t>(available), '\0');
    result.size = static_cast<std::size_t>(end - result.name.begin());
    return result;
#else
    return {.error = Error::unsupported};
#endif
}

Error UdpSocket::bind(IpEndpoint endpoint) noexcept
{
    if (!valid()) {
        last_system_error_ = open_system_error_;
        return Error::io_error;
    }
    if (endpoint.family != family_) {
        last_system_error_ = invalid_address_family_error;
        return Error::invalid_state;
    }
    const SocketAddress address = to_sockaddr(endpoint);
    const int result = ::bind(to_native(native_),
        reinterpret_cast<const sockaddr*>(&address.value),
        address.size);
    last_system_error_ = result == 0 ? 0 : last_socket_error();
    return result == 0 ? Error::none : Error::io_error;
}

EndpointResult UdpSocket::local_endpoint() const noexcept
{
    if (!valid()) {
        return {.error = Error::io_error, .system_error = open_system_error_};
    }
    sockaddr_storage address{};
    SocketLength size = static_cast<SocketLength>(sizeof(address));
    if (::getsockname(to_native(native_),
            reinterpret_cast<sockaddr*>(&address), &size) != 0) {
        return {.error = Error::io_error, .system_error = last_socket_error()};
    }
    if (address.ss_family != AF_INET
        && address.ss_family != AF_INET6) {
        return {
            .error = Error::io_error,
            .system_error = invalid_address_family_error,
        };
    }
    return {.endpoint = from_sockaddr(address)};
}

UdpWaitResult UdpSocket::wait_readable(int timeout_milliseconds) noexcept
{
    if (!valid() || timeout_milliseconds < -1) {
        return {.error = Error::io_error, .system_error = open_system_error_};
    }
#if defined(_WIN32)
    WSAPOLLFD descriptor{};
    descriptor.fd = to_native(native_);
    descriptor.events = POLLRDNORM;
    const int result = WSAPoll(&descriptor, 1, timeout_milliseconds);
#else
    pollfd descriptor{};
    descriptor.fd = to_native(native_);
    descriptor.events = POLLIN;
    const int result = ::poll(&descriptor, 1, timeout_milliseconds);
#endif
    if (result < 0) {
        last_system_error_ = last_socket_error();
#if defined(_WIN32)
        if (last_system_error_ == WSAEINTR) {
            return {};
        }
#else
        if (last_system_error_ == EINTR) {
            return {};
        }
#endif
        return {.error = Error::io_error, .system_error = last_system_error_};
    }
    return {.ready = result > 0};
}

UdpIoResult UdpSocket::send_to(
    std::span<const std::byte> bytes,
    IpEndpoint destination) noexcept
{
    if (!valid() || bytes.size() > static_cast<std::size_t>(INT_MAX)) {
        return {.error = Error::io_error, .system_error = open_system_error_};
    }
    if (destination.family != family_) {
        return {
            .error = Error::invalid_state,
            .system_error = invalid_address_family_error,
        };
    }
    const SocketAddress address = to_sockaddr(destination);
#if defined(_WIN32)
    const int result = ::sendto(to_native(native_),
        reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()), 0,
        reinterpret_cast<const sockaddr*>(&address.value),
        static_cast<int>(address.size));
#else
    const auto result = ::sendto(to_native(native_), bytes.data(), bytes.size(), 0,
        reinterpret_cast<const sockaddr*>(&address.value), address.size);
#endif
    if (result < 0) {
        const int error = last_socket_error();
        return {.error = is_would_block(error) ? Error::would_block : Error::io_error,
            .system_error = error};
    }
    return {.bytes_transferred = static_cast<std::size_t>(result)};
}

UdpIoResult UdpSocket::receive_from(std::span<std::byte> destination) noexcept
{
    if (!valid() || destination.size() > static_cast<std::size_t>(INT_MAX)) {
        return {.error = Error::io_error, .system_error = open_system_error_};
    }
    sockaddr_storage peer{};
    SocketLength peer_size = static_cast<SocketLength>(sizeof(peer));
#if defined(_WIN32)
    const int result = ::recvfrom(to_native(native_),
        reinterpret_cast<char*>(destination.data()), static_cast<int>(destination.size()), 0,
        reinterpret_cast<sockaddr*>(&peer), &peer_size);
    if (result < 0) {
        const int error = last_socket_error();
        if (error == WSAEMSGSIZE) {
            return {
                .error = Error::buffer_too_small,
                .bytes_transferred = destination.size(),
                .peer = from_sockaddr(peer),
                .system_error = error,
            };
        }
        return {.error = is_would_block(error) ? Error::would_block
                                               : Error::io_error,
            .system_error = error};
    }
#else
    iovec buffer {
        .iov_base = destination.data(),
        .iov_len = destination.size(),
    };
    msghdr message {};
    message.msg_name = &peer;
    message.msg_namelen = peer_size;
    message.msg_iov = &buffer;
    message.msg_iovlen = 1;
    const auto result = ::recvmsg(to_native(native_), &message, 0);
    if (result < 0) {
        const int error = last_socket_error();
        return {.error = is_would_block(error) ? Error::would_block : Error::io_error,
            .system_error = error};
    }
    if ((message.msg_flags & MSG_TRUNC) != 0) {
        return {
            .error = Error::buffer_too_small,
            .bytes_transferred = destination.size(),
            .peer = from_sockaddr(peer),
            .system_error = EMSGSIZE,
        };
    }
#endif
    return {
        .bytes_transferred = static_cast<std::size_t>(result),
        .peer = from_sockaddr(peer),
    };
}

} // namespace robotweax::srt
