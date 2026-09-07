#include "compat/socket_io.hpp"

#include "compat/error_state.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#  include <winsock2.h>
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#endif

namespace robotweax::srt::compat {
namespace {

[[nodiscard]] int fail(SRT_ERRNO error, int system_error = 0) noexcept
{
    set_last_error(error, system_error);
    return SRT_ERROR;
}

[[nodiscard]] int apply_native_network_options(
    const PublicSocketOptions& options,
    UdpSocket& socket) noexcept
{
    if (socket.set_ip_time_to_live(options.ip_time_to_live)
        != Error::none) {
        return fail(SRT_ESOCKFAIL, socket.last_system_error());
    }
    const Error tos_result =
        socket.set_ip_type_of_service(options.ip_type_of_service);
    if (tos_result == Error::unsupported) {
        return options.ip_type_of_service_explicit
            ? fail(SRT_EINVOP) : 0;
    }
    if (tos_result != Error::none) {
        return fail(SRT_ESOCKFAIL, socket.last_system_error());
    }
    return 0;
}

[[nodiscard]] IpEndpoint to_endpoint(const sockaddr_in& address) noexcept
{
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

[[nodiscard]] IpEndpoint to_endpoint(const sockaddr_in6& address) noexcept
{
    IpEndpoint endpoint = IpEndpoint::ipv6_any(
        ntohs(address.sin6_port), address.sin6_scope_id);
    std::copy(std::begin(address.sin6_addr.s6_addr),
        std::end(address.sin6_addr.s6_addr),
        endpoint.address.begin());
    return endpoint;
}

[[nodiscard]] sockaddr_in to_ipv4_sockaddr(
    IpEndpoint endpoint) noexcept
{
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
    return address;
}

[[nodiscard]] sockaddr_in6 to_ipv6_sockaddr(
    IpEndpoint endpoint) noexcept
{
    sockaddr_in6 address{};
#if defined(__APPLE__)
    address.sin6_len = static_cast<std::uint8_t>(sizeof(address));
#endif
    address.sin6_family = AF_INET6;
    address.sin6_port = htons(endpoint.port);
    address.sin6_scope_id = endpoint.scope_id;
    std::copy(endpoint.address.begin(), endpoint.address.end(),
        address.sin6_addr.s6_addr);
    return address;
}

[[nodiscard]] bool conflicts_with(
    IpEndpoint requested,
    std::int32_t requested_ipv6_only,
    IpEndpoint existing,
    std::int32_t existing_ipv6_only) noexcept
{
    if (requested.port != existing.port) {
        return false;
    }
    if (requested.family == existing.family) {
        if (requested.is_wildcard()
            || existing.is_wildcard()) {
            return true;
        }
        return requested.address == existing.address
            && (requested.is_ipv4()
                || requested.scope_id == existing.scope_id);
    }
    const auto covers_ipv4 = [](IpEndpoint endpoint,
                                 std::int32_t ipv6_only) {
        return endpoint.is_ipv6()
            && endpoint.is_wildcard()
            && ipv6_only != 1;
    };
    return covers_ipv4(requested, requested_ipv6_only)
        || covers_ipv4(existing, existing_ipv6_only);
}

[[nodiscard]] bool identical_binding(
    IpEndpoint requested,
    std::int32_t requested_ipv6_only,
    IpEndpoint existing,
    std::int32_t existing_ipv6_only) noexcept
{
    return requested == existing
        && (!requested.is_ipv6()
            || requested_ipv6_only == existing_ipv6_only);
}

struct BindingEntry {
    IpEndpoint endpoint{};
    std::int32_t ipv6_only = -1;
    std::int32_t udp_send_buffer_bytes = 0;
    std::int32_t udp_receive_buffer_bytes = 0;
    std::int32_t ip_time_to_live = 64;
    std::int32_t ip_type_of_service = 0xB8;
    std::array<char, maximum_network_device_name_size> bound_device{};
    std::uint8_t bound_device_size = 0;
    bool reusable = false;
    std::weak_ptr<DatagramChannel> channel;
};

class BindingRegistry {
public:
    [[nodiscard]] int bind_new(
        SocketRecord& socket,
        IpEndpoint requested,
        bool explicit_binding,
        std::shared_ptr<DatagramChannel>& channel,
        IpEndpoint& local) noexcept
    {
        std::lock_guard lock(mutex_);
        remove_expired();

        std::shared_ptr<DatagramChannel> candidate;
        try {
            candidate = std::make_shared<DatagramChannel>(
                requested.family);
        } catch (const std::bad_alloc&) {
            return fail(SRT_ENOBUF);
        } catch (...) {
            return fail(SRT_ESYSOBJ);
        }

        std::int32_t effective_ipv6_only = -1;
        if (configure_socket(socket, requested, explicit_binding,
                *candidate, effective_ipv6_only)
            == SRT_ERROR) {
            return SRT_ERROR;
        }

        if (requested.port != 0U) {
            for (const BindingEntry& entry : entries_) {
                if (!conflicts_with(requested,
                        effective_ipv6_only,
                        entry.endpoint,
                        entry.ipv6_only)) {
                    continue;
                }
                if (identical_binding(requested,
                        effective_ipv6_only,
                        entry.endpoint,
                        entry.ipv6_only)
                    && socket.public_options.reuse_address
                    && entry.reusable
                    && socket.public_options
                            .udp_send_buffer_bytes
                        == entry.udp_send_buffer_bytes
                    && socket.public_options
                            .udp_receive_buffer_bytes
                        == entry.udp_receive_buffer_bytes
                    && socket.public_options.ip_time_to_live
                        == entry.ip_time_to_live
                    && socket.public_options.ip_type_of_service
                        == entry.ip_type_of_service
                    && socket.public_options.bound_device_size
                        == entry.bound_device_size
                    && socket.public_options.bound_device
                        == entry.bound_device) {
                    channel = entry.channel.lock();
                    if (channel != nullptr) {
                        local = entry.endpoint;
                        socket.effective_ipv6_only =
                            entry.ipv6_only;
                        return 0;
                    }
                    continue;
                }
                return fail(SRT_EBINDCONFLICT);
            }
        }

        if (candidate->socket.bind(requested)
            != Error::none) {
            return fail(
                SRT_ESOCKFAIL,
                candidate->socket.last_system_error());
        }
        const EndpointResult endpoint =
            candidate->socket.local_endpoint();
        if (!endpoint) {
            return fail(
                SRT_ESOCKFAIL,
                endpoint.system_error);
        }
        local = endpoint.endpoint;
        channel = std::move(candidate);
        socket.effective_ipv6_only =
            effective_ipv6_only;
        if (remember(socket, local, effective_ipv6_only, channel)
            == SRT_ERROR) {
            channel.reset();
            return SRT_ERROR;
        }
        return 0;
    }

    [[nodiscard]] int register_acquired(
        SocketRecord& socket,
        IpEndpoint local,
        std::int32_t effective_ipv6_only,
        const std::shared_ptr<DatagramChannel>& channel)
        noexcept
    {
        std::lock_guard lock(mutex_);
        remove_expired();
        for (const BindingEntry& entry : entries_) {
            if (conflicts_with(local, effective_ipv6_only,
                    entry.endpoint, entry.ipv6_only)) {
                return fail(SRT_EBINDCONFLICT);
            }
        }
        return remember(
            socket, local, effective_ipv6_only, channel);
    }

private:
    static int configure_socket(
        SocketRecord& socket,
        IpEndpoint requested,
        bool explicit_binding,
        DatagramChannel& channel,
        std::int32_t& effective_ipv6_only) noexcept
    {
        if (explicit_binding
            && requested.is_ipv6()
            && requested.is_wildcard()
            && socket.public_options.ipv6_only == -1) {
            return fail(SRT_EINVPARAM);
        }
        if (!channel.socket.valid()) {
            return fail(
                SRT_ECONNSETUP,
                channel.socket.open_system_error());
        }
        if (channel.socket.set_send_buffer_size(
                socket.public_options
                    .udp_send_buffer_bytes)
                != Error::none
            || channel.socket.set_receive_buffer_size(
                   socket.public_options
                       .udp_receive_buffer_bytes)
                != Error::none) {
            return fail(
                SRT_ESOCKFAIL,
                channel.socket.last_system_error());
        }
        if (apply_native_network_options(
                socket.public_options, channel.socket)
            == SRT_ERROR) {
            return SRT_ERROR;
        }
        if (socket.public_options.bound_device_size != 0U) {
            const std::string_view device{
                socket.public_options.bound_device.data(),
                socket.public_options.bound_device_size};
            const Error result = channel.socket.set_bind_to_device(device);
            if (result == Error::unsupported) {
                return fail(SRT_EINVOP);
            }
            if (result != Error::none) {
                return fail(
                    SRT_ESOCKFAIL,
                    channel.socket.last_system_error());
            }
        }

        if (requested.is_ipv6()) {
            if (socket.public_options.ipv6_only != -1
                && channel.socket.set_ipv6_only(
                       socket.public_options.ipv6_only != 0)
                    != Error::none) {
                return fail(SRT_ESOCKFAIL,
                    channel.socket.last_system_error());
            }
            const UdpIntegerOptionResult actual =
                channel.socket.ipv6_only();
            if (!actual) {
                return fail(
                    SRT_ESOCKFAIL, actual.system_error);
            }
            effective_ipv6_only = actual.value;
        }
        return 0;
    }

    [[nodiscard]] int remember(
        const SocketRecord& socket,
        IpEndpoint endpoint,
        std::int32_t effective_ipv6_only,
        const std::shared_ptr<DatagramChannel>& channel)
        noexcept
    {
        try {
            entries_.push_back({
                .endpoint = endpoint,
                .ipv6_only = effective_ipv6_only,
                .udp_send_buffer_bytes =
                    socket.public_options
                        .udp_send_buffer_bytes,
                .udp_receive_buffer_bytes =
                    socket.public_options
                        .udp_receive_buffer_bytes,
                .ip_time_to_live =
                    socket.public_options.ip_time_to_live,
                .ip_type_of_service =
                    socket.public_options.ip_type_of_service,
                .bound_device =
                    socket.public_options.bound_device,
                .bound_device_size =
                    socket.public_options.bound_device_size,
                .reusable =
                    socket.public_options.reuse_address,
                .channel = channel,
            });
            return 0;
        } catch (const std::bad_alloc&) {
            return fail(SRT_ENOBUF);
        } catch (...) {
            return fail(SRT_ESYSOBJ);
        }
    }

    void remove_expired() noexcept
    {
        entries_.erase(
            std::remove_if(
                entries_.begin(), entries_.end(),
                [](const BindingEntry& entry) {
                    return entry.channel.expired();
                }),
            entries_.end());
    }

    std::mutex mutex_;
    std::vector<BindingEntry> entries_;
};

[[nodiscard]] BindingRegistry& binding_registry()
{
    static BindingRegistry registry;
    return registry;
}

[[nodiscard]] int bind_endpoint(
    SocketRecord& socket,
    IpEndpoint endpoint,
    bool explicit_binding) noexcept
{
    std::shared_ptr<DatagramChannel> channel;
    IpEndpoint local;
    if (binding_registry().bind_new(
            socket, endpoint, explicit_binding, channel, local)
        == SRT_ERROR) {
        return SRT_ERROR;
    }

    socket.channel = std::move(channel);
    socket.local_endpoint = local;
    socket.has_local_endpoint = true;
    socket.state = SRTS_OPENED;
    return 0;
}

[[nodiscard]] int adopt_endpoint(
    SocketRecord& socket,
    UDPSOCKET native_socket) noexcept
{
    UdpSocket acquired = UdpSocket::acquire_native(
        static_cast<std::uintptr_t>(native_socket));
    if (!acquired.valid()) {
        return fail(
            SRT_EINVPARAM,
            acquired.open_system_error());
    }

    std::shared_ptr<DatagramChannel> channel;
    try {
        channel = std::make_shared<DatagramChannel>(
            std::move(acquired));
    } catch (const std::bad_alloc&) {
        (void)acquired.release_native();
        return fail(SRT_ENOBUF);
    } catch (...) {
        (void)acquired.release_native();
        return fail(SRT_ESYSOBJ);
    }
    if (channel->socket.set_send_buffer_size(
            socket.public_options.udp_send_buffer_bytes)
            != Error::none
        || channel->socket.set_receive_buffer_size(
               socket.public_options.udp_receive_buffer_bytes)
            != Error::none) {
        (void)channel->socket.release_native();
        return fail(SRT_ESOCKFAIL, channel->socket.last_system_error());
    }
    if (apply_native_network_options(
            socket.public_options, channel->socket)
        == SRT_ERROR) {
        (void)channel->socket.release_native();
        return SRT_ERROR;
    }
    if (socket.public_options.bound_device_size != 0U) {
        const std::string_view device{
            socket.public_options.bound_device.data(),
            socket.public_options.bound_device_size};
        const Error result = channel->socket.set_bind_to_device(device);
        if (result != Error::none) {
            (void)channel->socket.release_native();
            return fail(result == Error::unsupported
                    ? SRT_EINVOP : SRT_ESOCKFAIL,
                channel->socket.last_system_error());
        }
    } else if (UdpSocket::bind_to_device_supported()) {
        const UdpDeviceOptionResult actual =
            channel->socket.bound_device();
        if (!actual) {
            (void)channel->socket.release_native();
            return fail(SRT_ESOCKFAIL, actual.system_error);
        }
        socket.public_options.bound_device.fill('\0');
        std::copy(actual.name.begin(),
            actual.name.begin()
                + static_cast<std::ptrdiff_t>(actual.size),
            socket.public_options.bound_device.begin());
        socket.public_options.bound_device_size =
            static_cast<std::uint8_t>(actual.size);
    }
    const EndpointResult local = channel->socket.local_endpoint();
    if (!local) {
        (void)channel->socket.release_native();
        return fail(SRT_ESOCKFAIL, local.system_error);
    }
    std::int32_t effective_ipv6_only = -1;
    if (local.endpoint.is_ipv6()) {
        const UdpIntegerOptionResult actual =
            channel->socket.ipv6_only();
        if (!actual) {
            (void)channel->socket.release_native();
            return fail(SRT_ESOCKFAIL, actual.system_error);
        }
        effective_ipv6_only = actual.value;
        socket.effective_ipv6_only =
            effective_ipv6_only;
    }
    if (binding_registry().register_acquired(
            socket, local.endpoint, effective_ipv6_only,
            channel)
        == SRT_ERROR) {
        (void)channel->socket.release_native();
        return SRT_ERROR;
    }

    socket.channel = std::move(channel);
    socket.local_endpoint = local.endpoint;
    socket.has_local_endpoint = true;
    socket.state = SRTS_OPENED;
    return 0;
}

} // namespace

int decode_ip_endpoint(
    const sockaddr* name, int name_size, IpEndpoint& endpoint,
    bool allow_zero_port) noexcept
{
    if (name == nullptr
        || name_size < static_cast<int>(sizeof(sockaddr))) {
        return fail(SRT_EINVPARAM);
    }

    if (name->sa_family == AF_INET) {
        if (name_size < static_cast<int>(sizeof(sockaddr_in))) {
            return fail(SRT_EINVPARAM);
        }
        sockaddr_in address{};
        std::memcpy(&address, name, sizeof(address));
        endpoint = to_endpoint(address);
    } else if (name->sa_family == AF_INET6) {
        if (name_size < static_cast<int>(sizeof(sockaddr_in6))) {
            return fail(SRT_EINVPARAM);
        }
        sockaddr_in6 address{};
        std::memcpy(&address, name, sizeof(address));
        endpoint = to_endpoint(address);
    } else {
        return fail(SRT_EINVPARAM);
    }
    if (!allow_zero_port && endpoint.port == 0U) {
        return fail(SRT_EINVPARAM);
    }
    return 0;
}

int write_ip_endpoint(
    IpEndpoint endpoint, sockaddr* name, int* name_size) noexcept
{
    if (name == nullptr || name_size == nullptr) {
        return fail(SRT_EINVPARAM);
    }
    if (endpoint.is_ipv4()) {
        if (*name_size < static_cast<int>(sizeof(sockaddr_in))) {
            return fail(SRT_EINVPARAM);
        }
        const sockaddr_in address = to_ipv4_sockaddr(endpoint);
        std::memcpy(name, &address, sizeof(address));
        *name_size = static_cast<int>(sizeof(address));
    } else {
        if (*name_size < static_cast<int>(sizeof(sockaddr_in6))) {
            return fail(SRT_EINVPARAM);
        }
        const sockaddr_in6 address = to_ipv6_sockaddr(endpoint);
        std::memcpy(name, &address, sizeof(address));
        *name_size = static_cast<int>(sizeof(address));
    }
    return 0;
}

int bind_socket(
    SocketRecord* socket, const sockaddr* name, int name_size) noexcept
{
    IpEndpoint endpoint;
    if (decode_ip_endpoint(
            name, name_size, endpoint, true) == SRT_ERROR) {
        return SRT_ERROR;
    }
    if (socket == nullptr) {
        return fail(SRT_EINVSOCK);
    }

    std::lock_guard lock(socket->mutex);
    if (socket->state == SRTS_CLOSED) {
        return fail(SRT_EINVSOCK);
    }
    if (socket->state != SRTS_INIT) {
        return fail(SRT_EINVOP);
    }

    return bind_endpoint(*socket, endpoint, true);
}

int bind_acquired_socket(
    SocketRecord* socket,
    UDPSOCKET native_socket) noexcept
{
    if (socket == nullptr) {
        return fail(SRT_EINVSOCK);
    }

    std::lock_guard lock(socket->mutex);
    if (socket->state == SRTS_CLOSED) {
        return fail(SRT_EINVSOCK);
    }
    if (socket->state != SRTS_INIT) {
        return fail(SRT_EINVOP);
    }
    return adopt_endpoint(*socket, native_socket);
}

int bind_any_socket(
    SocketRecord& socket, IpAddressFamily family) noexcept
{
    std::lock_guard lock(socket.mutex);
    if (socket.state == SRTS_CLOSED) {
        return fail(SRT_EINVSOCK);
    }
    if (socket.state != SRTS_INIT) {
        return socket.state == SRTS_OPENED
            ? 0
            : fail(SRT_EINVOP);
    }
    return bind_endpoint(socket,
        family == IpAddressFamily::ipv4
        ? IpEndpoint::any()
        : IpEndpoint::ipv6_any(),
        false);
}

int get_socket_name(
    SocketRecord& socket, sockaddr* name, int* name_size) noexcept
{
    if (name == nullptr || name_size == nullptr) {
        return fail(SRT_EINVPARAM);
    }

    std::lock_guard lock(socket.mutex);
    if (socket.state == SRTS_CLOSED) {
        return fail(SRT_EINVSOCK);
    }
    if (!socket.has_local_endpoint || socket.channel == nullptr) {
        return fail(SRT_ENOCONN);
    }
    return write_ip_endpoint(socket.local_endpoint, name, name_size);
}

} // namespace robotweax::srt::compat
