// Black-box shared-binding peer using only the public Haivision srt.h surface.
//
// The same source is linked against Robotweax SRT and the checksum-pinned
// Haivision library. It exercises same-port multiplexing, dual-stack binding,
// and native UDP-socket acquisition without private implementation access.

#include "srt.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;

constexpr int receiver_delivery_grace_milliseconds = 300;
constexpr int acquired_release_timeout_milliseconds = 3'000;

#if defined(_WIN32)
using NativeSocketLength = int;
constexpr UDPSOCKET invalid_udp_socket = INVALID_SOCKET;
#else
using NativeSocketLength = socklen_t;
constexpr UDPSOCKET invalid_udp_socket = -1;
#endif

enum class Role {
    listener,
    shared_caller,
    acquired_caller,
};

struct Configuration {
    Role role = Role::listener;
    std::string host;
    std::string local_host;
    std::uint16_t port = 0;
    std::uint16_t second_port = 0;
    std::uint16_t local_port = 0;
    std::string input_path;
    std::string second_input_path;
    std::string output_path;
    std::uint64_t byte_count = 0;
    std::uint64_t second_byte_count = 0;
    int chunk_size = 1'200;
    int second_chunk_size = 1'316;
    int timeout_milliseconds = 15'000;
    int ipv6_only = -1;
};

struct Endpoint {
    sockaddr_storage storage {};
    int size = 0;

    [[nodiscard]] sockaddr* data() noexcept
    {
        return reinterpret_cast<sockaddr*>(&storage);
    }

    [[nodiscard]] const sockaddr* data() const noexcept
    {
        return reinterpret_cast<const sockaddr*>(&storage);
    }

    [[nodiscard]] int family() const noexcept
    {
        return storage.ss_family;
    }

    [[nodiscard]] std::uint16_t port() const noexcept
    {
        if (family() == AF_INET) {
            return ntohs(
                reinterpret_cast<const sockaddr_in*>(&storage)->sin_port);
        }
        if (family() == AF_INET6) {
            return ntohs(
                reinterpret_cast<const sockaddr_in6*>(&storage)->sin6_port);
        }
        return 0;
    }
};

struct SrtSocket {
    SRTSOCKET value = SRT_INVALID_SOCK;

    SrtSocket() = default;
    explicit SrtSocket(SRTSOCKET socket)
        : value(socket)
    {
    }
    SrtSocket(const SrtSocket&) = delete;
    SrtSocket& operator=(const SrtSocket&) = delete;

    ~SrtSocket()
    {
        close();
    }

    void close() noexcept
    {
        if (value != SRT_INVALID_SOCK) {
            (void)srt_close(value);
            value = SRT_INVALID_SOCK;
        }
    }
};

void close_udp_socket(UDPSOCKET socket) noexcept
{
    if (socket == invalid_udp_socket) {
        return;
    }
#if defined(_WIN32)
    (void)::closesocket(socket);
#else
    (void)::close(socket);
#endif
}

struct NativeSocket {
    UDPSOCKET value = invalid_udp_socket;

    NativeSocket() = default;
    explicit NativeSocket(UDPSOCKET socket)
        : value(socket)
    {
    }
    NativeSocket(const NativeSocket&) = delete;
    NativeSocket& operator=(const NativeSocket&) = delete;

    ~NativeSocket()
    {
        close_udp_socket(value);
    }

    [[nodiscard]] UDPSOCKET release() noexcept
    {
        const UDPSOCKET socket = value;
        value = invalid_udp_socket;
        return socket;
    }
};

template <class Integer>
bool parse_integer(std::string_view text, Integer& value)
{
    const auto parsed =
        std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc {} && parsed.ptr == text.data() + text.size();
}

bool take_value(int& index, int argc, char** argv, std::string_view& value)
{
    if (index + 1 >= argc) {
        return false;
    }
    value = argv[++index];
    return true;
}

bool parse_port(std::string_view text, std::uint16_t& port)
{
    unsigned parsed = 0;
    if (!parse_integer(text, parsed) || parsed == 0U || parsed > 65'535U) {
        return false;
    }
    port = static_cast<std::uint16_t>(parsed);
    return true;
}

bool parse_arguments(int argc, char** argv, Configuration& configuration)
{
    if (argc < 2) {
        return false;
    }
    const std::string_view role {argv[1]};
    if (role == "listener") {
        configuration.role = Role::listener;
    } else if (role == "shared-caller") {
        configuration.role = Role::shared_caller;
    } else if (role == "acquired-caller") {
        configuration.role = Role::acquired_caller;
    } else {
        return false;
    }

    for (int index = 2; index < argc; ++index) {
        const std::string_view option {argv[index]};
        std::string_view value;
        if (option == "--host" && take_value(index, argc, argv, value)) {
            configuration.host = value;
        } else if (option == "--local-host"
            && take_value(index, argc, argv, value)) {
            configuration.local_host = value;
        } else if (option == "--port" && take_value(index, argc, argv, value)) {
            if (!parse_port(value, configuration.port)) {
                return false;
            }
        } else if (option == "--second-port"
            && take_value(index, argc, argv, value)) {
            if (!parse_port(value, configuration.second_port)) {
                return false;
            }
        } else if (option == "--local-port"
            && take_value(index, argc, argv, value)) {
            if (!parse_port(value, configuration.local_port)) {
                return false;
            }
        } else if (option == "--input"
            && take_value(index, argc, argv, value)) {
            configuration.input_path = value;
        } else if (option == "--second-input"
            && take_value(index, argc, argv, value)) {
            configuration.second_input_path = value;
        } else if (option == "--output"
            && take_value(index, argc, argv, value)) {
            configuration.output_path = value;
        } else if (option == "--bytes"
            && take_value(index, argc, argv, value)) {
            if (!parse_integer(value, configuration.byte_count)
                || configuration.byte_count == 0U) {
                return false;
            }
        } else if (option == "--second-bytes"
            && take_value(index, argc, argv, value)) {
            if (!parse_integer(value, configuration.second_byte_count)
                || configuration.second_byte_count == 0U) {
                return false;
            }
        } else if (option == "--chunk-size"
            && take_value(index, argc, argv, value)) {
            if (!parse_integer(value, configuration.chunk_size)
                || configuration.chunk_size <= 0
                || configuration.chunk_size > SRT_LIVE_MAX_PLSIZE) {
                return false;
            }
        } else if (option == "--second-chunk-size"
            && take_value(index, argc, argv, value)) {
            if (!parse_integer(value, configuration.second_chunk_size)
                || configuration.second_chunk_size <= 0
                || configuration.second_chunk_size > SRT_LIVE_MAX_PLSIZE) {
                return false;
            }
        } else if (option == "--timeout-ms"
            && take_value(index, argc, argv, value)) {
            if (!parse_integer(value, configuration.timeout_milliseconds)
                || configuration.timeout_milliseconds <= 0) {
                return false;
            }
        } else if (option == "--ipv6-only"
            && take_value(index, argc, argv, value)) {
            if (!parse_integer(value, configuration.ipv6_only)
                || configuration.ipv6_only < 0 || configuration.ipv6_only > 1) {
                return false;
            }
        } else {
            return false;
        }
    }

    if (configuration.host.empty() || configuration.port == 0
        || configuration.byte_count == 0U) {
        return false;
    }
    if (configuration.role == Role::listener) {
        return !configuration.output_path.empty();
    }
    if (configuration.local_host.empty() || configuration.input_path.empty()) {
        return false;
    }
    if (configuration.role == Role::shared_caller) {
        return configuration.local_port != 0 && configuration.second_port != 0
            && !configuration.second_input_path.empty()
            && configuration.second_byte_count != 0U;
    }
    return true;
}

bool make_endpoint(std::string_view host, std::uint16_t port, Endpoint& result)
{
    sockaddr_in ipv4 {};
    if (::inet_pton(AF_INET, std::string {host}.c_str(), &ipv4.sin_addr) == 1) {
        ipv4.sin_family = AF_INET;
        ipv4.sin_port = htons(port);
        std::memcpy(&result.storage, &ipv4, sizeof(ipv4));
        result.size = static_cast<int>(sizeof(ipv4));
        return true;
    }
    sockaddr_in6 ipv6 {};
    if (::inet_pton(AF_INET6, std::string {host}.c_str(), &ipv6.sin6_addr)
        == 1) {
        ipv6.sin6_family = AF_INET6;
        ipv6.sin6_port = htons(port);
        std::memcpy(&result.storage, &ipv6, sizeof(ipv6));
        result.size = static_cast<int>(sizeof(ipv6));
        return true;
    }
    return false;
}

bool same_endpoint(const Endpoint& first, const Endpoint& second) noexcept
{
    if (first.family() != second.family() || first.port() != second.port()) {
        return false;
    }
    if (first.family() == AF_INET) {
        return reinterpret_cast<const sockaddr_in*>(&first.storage)
                   ->sin_addr.s_addr
            == reinterpret_cast<const sockaddr_in*>(&second.storage)
                   ->sin_addr.s_addr;
    }
    if (first.family() == AF_INET6) {
        const auto* first_ipv6 =
            reinterpret_cast<const sockaddr_in6*>(&first.storage);
        const auto* second_ipv6 =
            reinterpret_cast<const sockaddr_in6*>(&second.storage);
        return first_ipv6->sin6_scope_id == second_ipv6->sin6_scope_id
            && std::memcmp(&first_ipv6->sin6_addr, &second_ipv6->sin6_addr,
                   sizeof(first_ipv6->sin6_addr))
            == 0;
    }
    return false;
}

Endpoint srt_socket_name(SRTSOCKET socket)
{
    Endpoint result;
    result.size = static_cast<int>(sizeof(result.storage));
    if (srt_getsockname(socket, result.data(), &result.size) == SRT_ERROR) {
        result.size = 0;
    }
    return result;
}

Endpoint native_socket_name(UDPSOCKET socket)
{
    Endpoint result;
    NativeSocketLength size =
        static_cast<NativeSocketLength>(sizeof(result.storage));
    if (::getsockname(socket, result.data(), &size) != 0) {
        result.size = 0;
    } else {
        result.size = static_cast<int>(size);
    }
    return result;
}

template <class Value>
bool set_option(SRTSOCKET socket, SRT_SOCKOPT option, const Value& value)
{
    return srt_setsockflag(
               socket, option, &value, static_cast<int>(sizeof(value)))
        != SRT_ERROR;
}

bool configure_socket(SRTSOCKET socket, const Configuration& configuration)
{
    const SRT_TRANSTYPE transport = SRTT_LIVE;
    const bool reuse = true;
    return set_option(socket, SRTO_TRANSTYPE, transport)
        && set_option(socket, SRTO_SNDTIMEO, configuration.timeout_milliseconds)
        && set_option(socket, SRTO_RCVTIMEO, configuration.timeout_milliseconds)
        && set_option(socket, SRTO_REUSEADDR, reuse)
        && (configuration.ipv6_only < 0
            || set_option(socket, SRTO_IPV6ONLY, configuration.ipv6_only));
}

int reported_ipv6_only(SRTSOCKET socket) noexcept
{
    std::int32_t value = -1;
    int size = static_cast<int>(sizeof(value));
    if (srt_getsockflag(socket, SRTO_IPV6ONLY, &value, &size) == SRT_ERROR) {
        return -2;
    }
    return value;
}

std::vector<char> read_payload(const std::string& path, std::uint64_t bytes)
{
    std::ifstream input {path, std::ios::binary};
    if (!input
        || bytes > static_cast<std::uint64_t>(
               std::numeric_limits<std::size_t>::max())) {
        return {};
    }
    std::vector<char> payload(static_cast<std::size_t>(bytes));
    input.read(payload.data(), static_cast<std::streamsize>(payload.size()));
    if (input.gcount() != static_cast<std::streamsize>(payload.size())) {
        return {};
    }
    return payload;
}

bool send_chunk(SRTSOCKET socket, const std::vector<char>& payload,
    std::size_t& offset, std::size_t limit, int chunk_size)
{
    if (offset >= limit) {
        return true;
    }
    const auto selected = static_cast<int>(std::min<std::size_t>(
        static_cast<std::size_t>(chunk_size), limit - offset));
    const int sent =
        srt_sendmsg(socket, payload.data() + offset, selected, -1, 1);
    if (sent != selected) {
        return false;
    }
    offset += static_cast<std::size_t>(sent);
    return true;
}

bool drain_send_buffer(SRTSOCKET socket, int timeout_milliseconds)
{
    const auto deadline =
        Clock::now() + std::chrono::milliseconds {timeout_milliseconds};
    while (Clock::now() < deadline) {
        std::size_t blocks = 0;
        std::size_t bytes = 0;
        if (srt_getsndbuffer(socket, &blocks, &bytes) == SRT_ERROR) {
            return false;
        }
        if (blocks == 0U) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds {5});
    }
    return false;
}

int run_listener(const Configuration& configuration)
{
    Endpoint address;
    SrtSocket listener {srt_create_socket()};
    if (listener.value == SRT_INVALID_SOCK
        || !make_endpoint(configuration.host, configuration.port, address)
        || !configure_socket(listener.value, configuration)
        || srt_bind(listener.value, address.data(), address.size) == SRT_ERROR
        || srt_listen(listener.value, 1) == SRT_ERROR) {
        std::cerr << "listener setup failed: " << srt_getlasterror_str()
                  << '\n';
        return 3;
    }
    std::cout << "{\"event\":\"ready\",\"port\":" << configuration.port
              << ",\"srt_version\":" << srt_getversion() << "}\n"
              << std::flush;

    sockaddr_storage peer {};
    int peer_size = static_cast<int>(sizeof(peer));
    SrtSocket connected {srt_accept(
        listener.value, reinterpret_cast<sockaddr*>(&peer), &peer_size)};
    if (connected.value == SRT_INVALID_SOCK) {
        std::cerr << "accept failed: " << srt_getlasterror_str() << '\n';
        return 4;
    }

    std::ofstream output {
        configuration.output_path, std::ios::binary | std::ios::trunc};
    if (!output) {
        std::cerr << "cannot open output file\n";
        return 5;
    }
    std::array<char, 65'536> buffer {};
    std::uint64_t received = 0;
    while (received < configuration.byte_count) {
        const int result = srt_recvmsg(
            connected.value, buffer.data(), static_cast<int>(buffer.size()));
        if (result <= 0
            || received + static_cast<std::uint64_t>(result)
                > configuration.byte_count) {
            std::cerr << "receive failed after " << received
                      << " bytes: " << srt_getlasterror_str() << '\n';
            return 6;
        }
        output.write(buffer.data(), result);
        if (!output) {
            return 5;
        }
        received += static_cast<std::uint64_t>(result);
    }
    output.flush();
    if (!output) {
        return 5;
    }
    std::cout << "{\"event\":\"complete\",\"role\":\"listener\","
                 "\"bytes\":"
              << received << ",\"local_family\":" << address.family()
              << ",\"ipv6_only\":" << reported_ipv6_only(listener.value)
              << ",\"srt_version\":" << srt_getversion() << "}\n"
              << std::flush;
    return 0;
}

int run_shared_caller(const Configuration& configuration)
{
    const auto first_payload =
        read_payload(configuration.input_path, configuration.byte_count);
    const auto second_payload = read_payload(
        configuration.second_input_path, configuration.second_byte_count);
    if (first_payload.empty() || second_payload.empty()) {
        std::cerr << "cannot read shared-caller payloads\n";
        return 5;
    }

    Endpoint local;
    Endpoint first_peer;
    Endpoint second_peer;
    SrtSocket first {srt_create_socket()};
    SrtSocket second {srt_create_socket()};
    if (first.value == SRT_INVALID_SOCK || second.value == SRT_INVALID_SOCK
        || !make_endpoint(
            configuration.local_host, configuration.local_port, local)
        || !make_endpoint(configuration.host, configuration.port, first_peer)
        || !make_endpoint(
            configuration.host, configuration.second_port, second_peer)
        || !configure_socket(first.value, configuration)
        || !configure_socket(second.value, configuration)
        || srt_bind(first.value, local.data(), local.size) == SRT_ERROR
        || srt_bind(second.value, local.data(), local.size) == SRT_ERROR) {
        std::cerr << "shared bind failed: " << srt_getlasterror_str() << '\n';
        return 3;
    }

    const Endpoint first_local = srt_socket_name(first.value);
    const Endpoint second_local = srt_socket_name(second.value);
    if (first_local.size == 0 || !same_endpoint(first_local, second_local)) {
        std::cerr << "shared sockets did not retain one local endpoint\n";
        return 3;
    }
    if (srt_connect(first.value, first_peer.data(), first_peer.size)
            == SRT_ERROR
        || srt_connect(second.value, second_peer.data(), second_peer.size)
            == SRT_ERROR) {
        std::cerr << "shared connect failed: " << srt_getlasterror_str()
                  << '\n';
        return 4;
    }

    std::size_t first_offset = 0;
    std::size_t second_offset = 0;
    const std::size_t second_split = second_payload.size() / 2U;
    while (
        first_offset < first_payload.size() || second_offset < second_split) {
        if (!send_chunk(first.value, first_payload, first_offset,
                first_payload.size(), configuration.chunk_size)
            || !send_chunk(second.value, second_payload, second_offset,
                second_split, configuration.second_chunk_size)) {
            std::cerr << "shared send failed: " << srt_getlasterror_str()
                      << '\n';
            return 6;
        }
    }
    if (!drain_send_buffer(first.value, configuration.timeout_milliseconds)
        || !drain_send_buffer(
            second.value, configuration.timeout_milliseconds)) {
        std::cerr << "shared send buffers did not drain\n";
        return 6;
    }

    // A drained sender buffer proves acknowledgement, not that a reference
    // Live receiver has crossed its TSBPD release deadline and returned from
    // the public receive call. Keep the first owner alive beyond the default
    // latency before closing it, then prove that the second owner survives.
    std::this_thread::sleep_for(
        std::chrono::milliseconds {receiver_delivery_grace_milliseconds});
    first.close();
    std::this_thread::sleep_for(std::chrono::milliseconds {25});
    while (second_offset < second_payload.size()) {
        if (!send_chunk(second.value, second_payload, second_offset,
                second_payload.size(), configuration.second_chunk_size)) {
            std::cerr << "surviving owner send failed: "
                      << srt_getlasterror_str() << '\n';
            return 6;
        }
    }
    if (!drain_send_buffer(second.value, configuration.timeout_milliseconds)) {
        std::cerr << "surviving owner send buffer did not drain\n";
        return 6;
    }
    std::this_thread::sleep_for(
        std::chrono::milliseconds {receiver_delivery_grace_milliseconds});

    std::cout << "{\"event\":\"shared_complete\",\"first_bytes\":"
              << first_offset << ",\"second_bytes\":" << second_offset
              << ",\"local_port\":" << first_local.port()
              << ",\"local_family\":" << first_local.family()
              << ",\"ipv6_only\":" << reported_ipv6_only(second.value)
              << ",\"first_owner_closed\":true,\"srt_version\":"
              << srt_getversion() << "}\n"
              << std::flush;
    return 0;
}

bool native_socket_is_closed(UDPSOCKET socket) noexcept
{
    sockaddr_storage ignored {};
    NativeSocketLength size = static_cast<NativeSocketLength>(sizeof(ignored));
    if (::getsockname(socket, reinterpret_cast<sockaddr*>(&ignored), &size)
        == 0) {
        return false;
    }
#if defined(_WIN32)
    return WSAGetLastError() == WSAENOTSOCK;
#else
    return errno == EBADF;
#endif
}

bool set_native_ipv6_only(UDPSOCKET socket, int value) noexcept
{
#if defined(_WIN32)
    return ::setsockopt(socket, IPPROTO_IPV6, IPV6_V6ONLY,
               reinterpret_cast<const char*>(&value),
               static_cast<int>(sizeof(value)))
        == 0;
#else
    return ::setsockopt(socket, IPPROTO_IPV6, IPV6_V6ONLY, &value,
               static_cast<socklen_t>(sizeof(value)))
        == 0;
#endif
}

bool rebind_native_endpoint(const Endpoint& endpoint, int ipv6_only) noexcept
{
    NativeSocket replacement {
        ::socket(endpoint.family(), SOCK_DGRAM, IPPROTO_UDP)};
    if (replacement.value == invalid_udp_socket) {
        return false;
    }
    if (endpoint.family() == AF_INET6 && ipv6_only >= 0
        && !set_native_ipv6_only(replacement.value, ipv6_only)) {
        return false;
    }
    return ::bind(replacement.value, endpoint.data(),
               static_cast<NativeSocketLength>(endpoint.size))
        == 0;
}

int run_acquired_caller(const Configuration& configuration)
{
    const auto payload =
        read_payload(configuration.input_path, configuration.byte_count);
    Endpoint requested;
    Endpoint peer;
    if (payload.empty()
        || !make_endpoint(configuration.local_host, 0, requested)
        || !make_endpoint(configuration.host, configuration.port, peer)) {
        std::cerr << "acquired caller endpoint setup failed\n";
        return 3;
    }

    NativeSocket native {::socket(requested.family(), SOCK_DGRAM, IPPROTO_UDP)};
    if (native.value == invalid_udp_socket
        || (requested.family() == AF_INET6 && configuration.ipv6_only >= 0
            && !set_native_ipv6_only(native.value, configuration.ipv6_only))
        || ::bind(native.value, requested.data(),
               static_cast<NativeSocketLength>(requested.size))
            != 0) {
        std::cerr << "native UDP bind failed\n";
        return 3;
    }
    const Endpoint native_local = native_socket_name(native.value);
    if (native_local.size == 0 || native_local.port() == 0U) {
        std::cerr << "native UDP name query failed\n";
        return 3;
    }

    SrtSocket socket {srt_create_socket()};
    if (socket.value == SRT_INVALID_SOCK
        || !configure_socket(socket.value, configuration)) {
        std::cerr << "SRT acquisition setup failed: " << srt_getlasterror_str()
                  << '\n';
        return 3;
    }
    const UDPSOCKET acquired = native.release();
    if (srt_bind_acquire(socket.value, acquired) == SRT_ERROR) {
        std::cerr << "srt_bind_acquire failed: " << srt_getlasterror_str()
                  << '\n';
        close_udp_socket(acquired);
        return 3;
    }
    const Endpoint srt_local = srt_socket_name(socket.value);
    if (srt_local.size == 0 || !same_endpoint(native_local, srt_local)
        || srt_connect(socket.value, peer.data(), peer.size) == SRT_ERROR) {
        std::cerr << "acquired caller connect failed: "
                  << srt_getlasterror_str() << '\n';
        return 4;
    }

    std::size_t offset = 0;
    while (offset < payload.size()) {
        if (!send_chunk(socket.value, payload, offset, payload.size(),
                configuration.chunk_size)) {
            std::cerr << "acquired caller send failed: "
                      << srt_getlasterror_str() << '\n';
            return 6;
        }
    }
    if (!drain_send_buffer(socket.value, configuration.timeout_milliseconds)) {
        std::cerr << "acquired caller send buffer did not drain\n";
        return 6;
    }
    const int observed_ipv6_only = reported_ipv6_only(socket.value);
    std::this_thread::sleep_for(
        std::chrono::milliseconds {receiver_delivery_grace_milliseconds});
    const auto release_started = Clock::now();
    socket.close();
    const auto release_deadline = release_started
        + std::chrono::milliseconds {acquired_release_timeout_milliseconds};
    bool native_closed = false;
    bool rebound = false;
    auto release_observed = release_started;
    for (;;) {
        const auto check_started = Clock::now();
        if (check_started > release_deadline) {
            release_observed = check_started;
            break;
        }
        native_closed = native_socket_is_closed(acquired);
        if (native_closed) {
            rebound = rebind_native_endpoint(native_local,
                requested.family() == AF_INET6 ? observed_ipv6_only : -1);
            if (rebound) {
                release_observed = Clock::now();
                if (release_observed <= release_deadline) {
                    break;
                }
                rebound = false;
                break;
            }
        }
        const auto now = Clock::now();
        if (now >= release_deadline) {
            release_observed = now;
            break;
        }
        std::this_thread::sleep_until(
            std::min(release_deadline, now + std::chrono::milliseconds {10}));
    }
    const auto release_wait_microseconds =
        std::chrono::duration_cast<std::chrono::microseconds>(
            release_observed - release_started)
            .count();

    std::cout << "{\"event\":\"acquired_complete\",\"bytes\":" << offset
              << ",\"local_port\":" << native_local.port()
              << ",\"local_family\":" << native_local.family()
              << ",\"ipv6_only\":" << observed_ipv6_only
              << ",\"native_closed\":" << (native_closed ? "true" : "false")
              << ",\"port_rebound\":" << (rebound ? "true" : "false")
              << ",\"release_wait_us\":" << release_wait_microseconds
              << ",\"srt_version\":" << srt_getversion() << "}\n"
              << std::flush;
    return native_closed && rebound ? 0 : 7;
}

void usage()
{
    std::cerr << "usage: binding_peer listener|shared-caller|acquired-caller"
                 " --host IP --port PORT --bytes N"
                 " (--output PATH | --input PATH)"
                 " [--local-host IP --local-port PORT]"
                 " [--second-port PORT --second-input PATH --second-bytes N]"
                 " [--chunk-size N --second-chunk-size N]"
                 " [--timeout-ms N] [--ipv6-only 0|1]\n";
}

} // namespace

int main(int argc, char** argv)
{
    Configuration configuration;
    if (!parse_arguments(argc, argv, configuration)) {
        usage();
        return 2;
    }
    if (srt_startup() == SRT_ERROR) {
        std::cerr << "srt_startup failed\n";
        return 2;
    }

    int result = 0;
    switch (configuration.role) {
    case Role::listener:
        result = run_listener(configuration);
        break;
    case Role::shared_caller:
        result = run_shared_caller(configuration);
        break;
    case Role::acquired_caller:
        result = run_acquired_caller(configuration);
        break;
    }
    if (srt_cleanup() == SRT_ERROR && result == 0) {
        std::cerr << "srt_cleanup failed\n";
        return 8;
    }
    return result;
}
