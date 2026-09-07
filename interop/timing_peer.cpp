// Public-API Live receiver that records TSBPD release and UDP handoff times.

#include "srt.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#  include <cerrno>
#if defined(__linux__)
#include <linux/errqueue.h>
#include <linux/net_tstamp.h>
#include <poll.h>
#include <time.h>
#endif
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#endif

namespace {

enum class UdpTimestampSource {
    userspace,
    linux_software,
};

struct Configuration {
    std::string host = "127.0.0.1";
    std::uint16_t port = 0;
    bool rendezvous = false;
    std::string peer_host;
    std::uint16_t peer_port = 0;
    std::size_t group_members = 0;
    std::string udp_host = "127.0.0.1";
    std::uint16_t udp_port = 0;
    std::size_t message_count = 0;
    int maximum_message_size = 1'316;
    int latency_milliseconds = 120;
    int timeout_milliseconds = 10'000;
    int shutdown_grace_milliseconds = 250;
    std::string passphrase_environment;
    int pbkeylen = 0;
    int crypto_mode = -1;
    std::string packet_filter;
    UdpTimestampSource udp_timestamp_source = UdpTimestampSource::userspace;
    bool phase_timing = false;
};

struct TimingObservation {
    std::size_t message_index = 0;
    std::int64_t tsbpd_deadline_microseconds = 0;
    std::int64_t srt_release_microseconds = 0;
    std::int64_t udp_egress_microseconds = 0;
    std::int64_t udp_userspace_realtime_nanoseconds = 0;
    std::int64_t udp_kernel_software_tx_realtime_nanoseconds = 0;
    int payload_bytes = 0;
    std::int32_t packet_sequence = 0;
    std::int32_t message_number = 0;
    SRTSOCKET running_group_member = SRT_INVALID_SOCK;
    std::size_t running_group_member_count = 0;
    std::size_t group_member_count = 0;
    std::int64_t receive_start_microseconds = 0;
    std::int64_t udp_send_start_microseconds = 0;
};

struct EndpointAddress {
    sockaddr_storage storage{};
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
};

struct SrtSocket {
    SRTSOCKET value = SRT_INVALID_SOCK;

    SrtSocket() = default;
    explicit SrtSocket(SRTSOCKET socket) noexcept : value(socket) {}
    SrtSocket(const SrtSocket&) = delete;
    SrtSocket& operator=(const SrtSocket&) = delete;

    ~SrtSocket()
    {
        if (value != SRT_INVALID_SOCK) {
            (void)srt_close(value);
        }
    }
};

#if defined(_WIN32)
using NativeSocketValue = SOCKET;
constexpr NativeSocketValue invalid_native_socket = INVALID_SOCKET;
#else
using NativeSocketValue = int;
constexpr NativeSocketValue invalid_native_socket = -1;
#endif

struct NativeSocket {
    NativeSocketValue value = invalid_native_socket;

    NativeSocket() = default;
    explicit NativeSocket(NativeSocketValue socket) noexcept : value(socket) {}
    NativeSocket(const NativeSocket&) = delete;
    NativeSocket& operator=(const NativeSocket&) = delete;

    ~NativeSocket()
    {
        if (value == invalid_native_socket) {
            return;
        }
#if defined(_WIN32)
        (void)closesocket(value);
#else
        (void)close(value);
#endif
    }
};

template <class Integer>
bool parse_integer(std::string_view text, Integer& value)
{
    const auto parsed = std::from_chars(
        text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{}
        && parsed.ptr == text.data() + text.size();
}

bool take_value(int& index, int argc, char** argv, std::string_view& value)
{
    if (index + 1 >= argc) {
        return false;
    }
    value = argv[++index];
    return true;
}

bool parse_arguments(int argc, char** argv, Configuration& configuration)
{
    for (int index = 1; index < argc; ++index) {
        const std::string_view option{argv[index]};
        std::string_view value;
        if (option == "--host" && take_value(index, argc, argv, value)) {
            configuration.host = value;
        } else if (option == "--port"
            && take_value(index, argc, argv, value)) {
            unsigned parsed = 0;
            if (!parse_integer(value, parsed) || parsed == 0U
                || parsed > 65'535U) {
                return false;
            }
            configuration.port = static_cast<std::uint16_t>(parsed);
        } else if (option == "--rendezvous") {
            configuration.rendezvous = true;
        } else if (option == "--peer-host"
            && take_value(index, argc, argv, value)) {
            configuration.peer_host = value;
        } else if (option == "--peer-port"
            && take_value(index, argc, argv, value)) {
            unsigned parsed = 0;
            if (!parse_integer(value, parsed) || parsed == 0U
                || parsed > 65'535U) {
                return false;
            }
            configuration.peer_port = static_cast<std::uint16_t>(parsed);
        } else if (option == "--group-members"
            && take_value(index, argc, argv, value)) {
            if (!parse_integer(value, configuration.group_members)
                || configuration.group_members < 2U
                || configuration.group_members > 4U) {
                return false;
            }
        } else if (option == "--udp-host"
            && take_value(index, argc, argv, value)) {
            configuration.udp_host = value;
        } else if (option == "--udp-port"
            && take_value(index, argc, argv, value)) {
            unsigned parsed = 0;
            if (!parse_integer(value, parsed) || parsed == 0U
                || parsed > 65'535U) {
                return false;
            }
            configuration.udp_port = static_cast<std::uint16_t>(parsed);
        } else if (option == "--messages"
            && take_value(index, argc, argv, value)) {
            if (!parse_integer(value, configuration.message_count)
                || configuration.message_count == 0U
                || configuration.message_count > 1'000'000U) {
                return false;
            }
        } else if (option == "--max-message-size"
            && take_value(index, argc, argv, value)) {
            if (!parse_integer(value, configuration.maximum_message_size)
                || configuration.maximum_message_size <= 0
                || configuration.maximum_message_size > 65'507) {
                return false;
            }
        } else if (option == "--latency-ms"
            && take_value(index, argc, argv, value)) {
            if (!parse_integer(value, configuration.latency_milliseconds)
                || configuration.latency_milliseconds < 0) {
                return false;
            }
        } else if (option == "--timeout-ms"
            && take_value(index, argc, argv, value)) {
            if (!parse_integer(value, configuration.timeout_milliseconds)
                || configuration.timeout_milliseconds <= 0) {
                return false;
            }
        } else if (option == "--shutdown-grace-ms"
            && take_value(index, argc, argv, value)) {
            if (!parse_integer(
                    value, configuration.shutdown_grace_milliseconds)
                || configuration.shutdown_grace_milliseconds < 0
                || configuration.shutdown_grace_milliseconds > 60'000) {
                return false;
            }
        } else if (option == "--passphrase-env"
            && take_value(index, argc, argv, value)) {
            if (value.empty() || value.size() > 128U) {
                return false;
            }
            configuration.passphrase_environment = value;
        } else if (option == "--pbkeylen"
            && take_value(index, argc, argv, value)) {
            if (!parse_integer(value, configuration.pbkeylen)
                || (configuration.pbkeylen != 16 && configuration.pbkeylen != 24
                    && configuration.pbkeylen != 32)) {
                return false;
            }
#ifdef ENABLE_AEAD_API_PREVIEW
        } else if (option == "--crypto-mode"
            && take_value(index, argc, argv, value)) {
            if (value == "ctr") {
                configuration.crypto_mode = 1;
            } else if (value == "gcm") {
                configuration.crypto_mode = 2;
            } else {
                return false;
            }
#endif
        } else if (option == "--packet-filter"
            && take_value(index, argc, argv, value)) {
            if (value.empty() || value.size() > 512U) {
                return false;
            }
            configuration.packet_filter = value;
        } else if (option == "--phase-timing") {
            configuration.phase_timing = true;
        } else if (option == "--udp-timestamp-source"
            && take_value(index, argc, argv, value)) {
            if (value == "userspace") {
                configuration.udp_timestamp_source =
                    UdpTimestampSource::userspace;
            } else if (value == "linux-software") {
                configuration.udp_timestamp_source =
                    UdpTimestampSource::linux_software;
            } else {
                return false;
            }
        } else {
            return false;
        }
    }
    const bool encryption_configured =
        !configuration.passphrase_environment.empty();
    return configuration.port != 0 && configuration.udp_port != 0
        && configuration.message_count != 0
        && encryption_configured == (configuration.pbkeylen != 0)
        && (encryption_configured || configuration.crypto_mode == -1)
        && (configuration.group_members == 0U
            || (!configuration.rendezvous
                && configuration.packet_filter.empty()))
        && ((configuration.rendezvous && !configuration.peer_host.empty()
                && configuration.peer_port != 0)
            || (!configuration.rendezvous && configuration.peer_host.empty()
                && configuration.peer_port == 0));
}

bool make_endpoint(
    const std::string& host, std::uint16_t port,
    EndpointAddress& endpoint)
{
    endpoint = {};
    sockaddr_in ipv4{};
    ipv4.sin_family = AF_INET;
    ipv4.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &ipv4.sin_addr) == 1) {
        std::memcpy(&endpoint.storage, &ipv4, sizeof(ipv4));
        endpoint.size = static_cast<int>(sizeof(ipv4));
        return true;
    }

    sockaddr_in6 ipv6{};
    ipv6.sin6_family = AF_INET6;
    ipv6.sin6_port = htons(port);
    if (inet_pton(AF_INET6, host.c_str(), &ipv6.sin6_addr) == 1) {
        std::memcpy(&endpoint.storage, &ipv6, sizeof(ipv6));
        endpoint.size = static_cast<int>(sizeof(ipv6));
        return true;
    }
    return false;
}

template <class Value>
bool set_option(SRTSOCKET socket, SRT_SOCKOPT option, const Value& value)
{
    if (srt_setsockflag(socket, option, &value,
            static_cast<int>(sizeof(value))) == SRT_ERROR) {
        std::cerr << "setsockflag " << static_cast<int>(option)
                  << " failed: " << srt_getlasterror_str() << '\n';
        return false;
    }
    return true;
}

bool set_string_option(
    SRTSOCKET socket, SRT_SOCKOPT option, std::string_view value)
{
    if (srt_setsockflag(
            socket, option, value.data(), static_cast<int>(value.size()))
        == SRT_ERROR) {
        std::cerr << "setsockflag " << static_cast<int>(option)
                  << " failed: " << srt_getlasterror_str() << '\n';
        return false;
    }
    return true;
}

bool effective_latency(SRTSOCKET socket, int& latency_milliseconds)
{
    int size = static_cast<int>(sizeof(latency_milliseconds));
    return srt_getsockflag(socket, SRTO_RCVLATENCY,
               &latency_milliseconds, &size) != SRT_ERROR
        && size == static_cast<int>(sizeof(latency_milliseconds))
        && latency_milliseconds >= 0;
}

bool effective_receive_security(
    SRTSOCKET socket, int& key_length, int& key_state)
{
    int key_length_size = static_cast<int>(sizeof(key_length));
    int key_state_size = static_cast<int>(sizeof(key_state));
    return srt_getsockflag(socket, SRTO_PBKEYLEN, &key_length, &key_length_size)
        != SRT_ERROR
        && key_length_size == static_cast<int>(sizeof(key_length))
        && srt_getsockflag(socket, SRTO_RCVKMSTATE, &key_state, &key_state_size)
        != SRT_ERROR
        && key_state_size == static_cast<int>(sizeof(key_state));
}

bool effective_group_receive_security(
    const std::vector<SRT_SOCKGROUPDATA>& members, int expected_key_length,
    int& key_length, int& key_state)
{
    key_length = expected_key_length;
    key_state = static_cast<int>(SRT_KM_S_SECURED);
    return !members.empty()
        && std::all_of(members.begin(), members.end(),
            [expected_key_length](const SRT_SOCKGROUPDATA& member) {
                int member_key_length = 0;
                int member_key_state = static_cast<int>(SRT_KM_S_UNSECURED);
                return effective_receive_security(
                           member.id, member_key_length, member_key_state)
                    && member_key_length == expected_key_length
                    && member_key_state == static_cast<int>(SRT_KM_S_SECURED);
            });
}

bool effective_packet_filter(SRTSOCKET socket, std::string& packet_filter)
{
    std::array<char, 513> value {};
    int size = static_cast<int>(value.size() - 1U);
    if (srt_getsockflag(socket, SRTO_PACKETFILTER, value.data(), &size)
            == SRT_ERROR
        || size < 0 || size > static_cast<int>(value.size() - 1U)) {
        return false;
    }
    packet_filter.assign(value.data(), static_cast<std::size_t>(size));
    return true;
}

bool wait_for_group_members(SRTSOCKET group, std::size_t expected,
    std::vector<SRT_SOCKGROUPDATA>& members)
{
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds {5};
    while (std::chrono::steady_clock::now() < deadline) {
        std::size_t size = 0;
        if (srt_group_data(group, nullptr, &size) != SRT_ERROR
            && size == expected) {
            members.resize(size);
            std::size_t capacity = members.size();
            if (srt_group_data(group, members.data(), &capacity) != SRT_ERROR
                && capacity == expected
                && std::all_of(members.begin(), members.end(),
                    [](const SRT_SOCKGROUPDATA& member) {
                        return member.id != SRT_INVALID_SOCK
                            && member.sockstate == SRTS_CONNECTED;
                    })) {
                return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds {5});
    }
    return false;
}

int send_udp(
    NativeSocketValue socket, const EndpointAddress& peer,
    const char* data, int size)
{
#if defined(_WIN32)
    return sendto(socket, data, size, 0,
        peer.data(), peer.size);
#else
    return static_cast<int>(sendto(socket, data,
        static_cast<std::size_t>(size), 0,
        peer.data(), static_cast<socklen_t>(peer.size)));
#endif
}

#if defined(__linux__)
bool configure_linux_software_tx_timestamps(NativeSocketValue socket)
{
    const int flags = SOF_TIMESTAMPING_TX_SOFTWARE | SOF_TIMESTAMPING_SOFTWARE
        | SOF_TIMESTAMPING_OPT_ID | SOF_TIMESTAMPING_OPT_TSONLY;
    const int receive_buffer_bytes = 4 * 1024 * 1024;
    return setsockopt(socket, SOL_SOCKET, SO_TIMESTAMPING, &flags,
               static_cast<socklen_t>(sizeof(flags)))
        == 0
        && setsockopt(socket, SOL_SOCKET, SO_RCVBUF, &receive_buffer_bytes,
               static_cast<socklen_t>(sizeof(receive_buffer_bytes)))
        == 0;
}

std::int64_t realtime_nanoseconds()
{
    timespec now {};
    if (clock_gettime(CLOCK_REALTIME, &now) != 0 || now.tv_sec < 0
        || now.tv_nsec < 0 || now.tv_nsec >= 1'000'000'000L
        || static_cast<std::uint64_t>(now.tv_sec) > static_cast<std::uint64_t>(
               std::numeric_limits<std::int64_t>::max() / 1'000'000'000LL)) {
        return 0;
    }
    return static_cast<std::int64_t>(now.tv_sec) * 1'000'000'000LL
        + static_cast<std::int64_t>(now.tv_nsec);
}

bool drain_linux_software_tx_timestamps(NativeSocketValue socket,
    std::vector<std::int64_t>& timestamps, std::size_t& received)
{
    for (;;) {
        std::array<char, 1> payload {};
        std::array<char, 1'024> ancillary {};
        iovec vector {payload.data(), payload.size()};
        msghdr message {};
        message.msg_iov = &vector;
        message.msg_iovlen = 1;
        message.msg_control = ancillary.data();
        message.msg_controllen = ancillary.size();
        const ssize_t result =
            recvmsg(socket, &message, MSG_ERRQUEUE | MSG_DONTWAIT);
        if (result < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return true;
            }
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if ((message.msg_flags & (MSG_CTRUNC | MSG_TRUNC)) != 0) {
            return false;
        }

        const scm_timestamping* timestamp = nullptr;
        const sock_extended_err* extended_error = nullptr;
        for (cmsghdr* control = CMSG_FIRSTHDR(&message); control != nullptr;
            control = CMSG_NXTHDR(&message, control)) {
            if (control->cmsg_level == SOL_SOCKET
                && control->cmsg_type == SCM_TIMESTAMPING) {
                if (timestamp != nullptr
                    || control->cmsg_len < CMSG_LEN(sizeof(scm_timestamping))) {
                    return false;
                }
                timestamp = reinterpret_cast<const scm_timestamping*>(
                    CMSG_DATA(control));
            } else if (((control->cmsg_level == SOL_IP
                            && control->cmsg_type == IP_RECVERR)
                           || (control->cmsg_level == SOL_IPV6
                               && control->cmsg_type == IPV6_RECVERR))) {
                if (extended_error != nullptr
                    || control->cmsg_len
                        < CMSG_LEN(sizeof(sock_extended_err))) {
                    return false;
                }
                extended_error = reinterpret_cast<const sock_extended_err*>(
                    CMSG_DATA(control));
            }
        }
        if (timestamp == nullptr || extended_error == nullptr
            || extended_error->ee_errno != ENOMSG
            || extended_error->ee_origin != SO_EE_ORIGIN_TIMESTAMPING
            || extended_error->ee_info != SCM_TSTAMP_SND
            || extended_error->ee_data >= timestamps.size()
            || timestamps[extended_error->ee_data] != 0
            || timestamp->ts[0].tv_sec <= 0 || timestamp->ts[0].tv_nsec < 0
            || timestamp->ts[0].tv_nsec >= 1'000'000'000L
            || timestamp->ts[1].tv_sec != 0 || timestamp->ts[1].tv_nsec != 0
            || timestamp->ts[2].tv_sec != 0 || timestamp->ts[2].tv_nsec != 0
            || static_cast<std::uint64_t>(timestamp->ts[0].tv_sec)
                > static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max()
                    / 1'000'000'000LL)) {
            return false;
        }
        timestamps[extended_error->ee_data] =
            static_cast<std::int64_t>(timestamp->ts[0].tv_sec) * 1'000'000'000LL
            + static_cast<std::int64_t>(timestamp->ts[0].tv_nsec);
        ++received;
    }
}

bool collect_linux_software_tx_timestamps(NativeSocketValue socket,
    std::vector<std::int64_t>& timestamps, std::size_t& received)
{
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds {2};
    while (received < timestamps.size()) {
        if (!drain_linux_software_tx_timestamps(socket, timestamps, received)) {
            return false;
        }
        if (received == timestamps.size()) {
            return true;
        }
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) {
            return false;
        }
        pollfd descriptor {socket, POLLERR, 0};
        const int timeout = static_cast<int>(std::min<std::int64_t>(
            remaining.count(), std::numeric_limits<int>::max()));
        const int result = poll(&descriptor, 1, timeout);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0 || (descriptor.revents & POLLERR) == 0) {
            return false;
        }
    }
    return true;
}
#endif

void usage()
{
    std::cerr << "usage: robotweax_srt_timing_peer"
                 " --host IP --port PORT --udp-host IP --udp-port PORT"
                 " [--rendezvous --peer-host IP --peer-port PORT]"
                 " [--group-members 2..4]"
                 " --messages N [--max-message-size N] [--latency-ms N]"
                 " [--timeout-ms N] [--shutdown-grace-ms N]"
                 " [--passphrase-env NAME --pbkeylen 16|24|32]"
#ifdef ENABLE_AEAD_API_PREVIEW
                 " [--crypto-mode ctr|gcm]"
#endif
                 " [--packet-filter CONFIG] [--phase-timing]"
                 " [--udp-timestamp-source userspace|linux-software]\n";
}

void emit_phase_clock_anchor()
{
    // Bound the realtime observation in the SRT monotonic clock domain.
    // This runs outside the message loop, for correlation with strace -ttt.
    const auto before = srt_time_now();
    const auto realtime = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch())
                              .count();
    const auto after = srt_time_now();
    std::cout << ",\"phase_clock_monotonic_before_microseconds\":" << before
              << ",\"phase_clock_realtime_microseconds\":" << realtime
              << ",\"phase_clock_monotonic_after_microseconds\":" << after;
}

int run(const Configuration& configuration)
{
    EndpointAddress srt_address;
    EndpointAddress peer_address;
    EndpointAddress udp_address;
    if (!make_endpoint(configuration.host, configuration.port, srt_address)
        || (configuration.rendezvous
            && !make_endpoint(
                configuration.peer_host, configuration.peer_port, peer_address))
        || !make_endpoint(
            configuration.udp_host, configuration.udp_port, udp_address)) {
        std::cerr << "invalid numeric SRT or UDP endpoint\n";
        return 3;
    }

    NativeSocket udp_socket{
        ::socket(udp_address.family(), SOCK_DGRAM, IPPROTO_UDP)};
    SrtSocket endpoint {srt_create_socket()};
    const SRT_TRANSTYPE transport = SRTT_LIVE;
    const bool receiver = false;
    const bool tsbpd = true;
    const int allow_groups = 1;
    const char* passphrase = nullptr;
    if (!configuration.passphrase_environment.empty()) {
        passphrase = std::getenv(configuration.passphrase_environment.c_str());
        if (passphrase == nullptr || *passphrase == '\0') {
            std::cerr << "passphrase environment variable is unset\n";
            return 3;
        }
    }
    bool crypto_mode_configured = true;
#ifdef ENABLE_AEAD_API_PREVIEW
    if (configuration.crypto_mode >= 0) {
        crypto_mode_configured = set_option(
            endpoint.value, SRTO_CRYPTOMODE, configuration.crypto_mode);
    }
#endif
#if defined(__linux__)
    if (configuration.udp_timestamp_source == UdpTimestampSource::linux_software
        && (udp_socket.value == invalid_native_socket
            || !configure_linux_software_tx_timestamps(udp_socket.value))) {
        std::cerr << "Linux software UDP TX timestamp setup failed: "
                  << std::strerror(errno) << '\n';
        return 3;
    }
#else
    if (configuration.udp_timestamp_source
        == UdpTimestampSource::linux_software) {
        std::cerr << "Linux software UDP TX timestamps are unavailable on "
                     "this platform\n";
        return 3;
    }
#endif
    if (udp_socket.value == invalid_native_socket
        || endpoint.value == SRT_INVALID_SOCK || !crypto_mode_configured
        || !set_option(endpoint.value, SRTO_TRANSTYPE, transport)
        || !set_option(endpoint.value, SRTO_SENDER, receiver)
        || !set_option(endpoint.value, SRTO_TSBPDMODE, tsbpd)
        || (configuration.group_members != 0U
            && !set_option(endpoint.value, SRTO_GROUPCONNECT, allow_groups))
        || (configuration.rendezvous
            && !set_option(
                endpoint.value, SRTO_RENDEZVOUS, configuration.rendezvous))
        || (passphrase != nullptr
            && !set_string_option(endpoint.value, SRTO_PASSPHRASE, passphrase))
        || (configuration.pbkeylen != 0
            && !set_option(
                endpoint.value, SRTO_PBKEYLEN, configuration.pbkeylen))
        || (!configuration.packet_filter.empty()
            && !set_string_option(
                endpoint.value, SRTO_PACKETFILTER, configuration.packet_filter))
        || !set_option(
            endpoint.value, SRTO_LATENCY, configuration.latency_milliseconds)
        || !set_option(
            endpoint.value, SRTO_RCVTIMEO, configuration.timeout_milliseconds)
        || srt_bind(endpoint.value, srt_address.data(), srt_address.size)
            == SRT_ERROR) {
        std::cerr << "timing endpoint setup failed: " << srt_getlasterror_str()
                  << '\n';
        return 3;
    }

    if (!configuration.rendezvous
        && srt_listen(endpoint.value,
               configuration.group_members == 0U
                   ? 1
                   : static_cast<int>(configuration.group_members))
            == SRT_ERROR) {
        std::cerr << "timing listen failed: " << srt_getlasterror_str() << '\n';
        return 3;
    }
    std::cout << "{\"event\":\"ready\",\"port\":"
              << configuration.port << "}\n" << std::flush;
    SrtSocket connected;
    if (configuration.rendezvous) {
        if (srt_connect(endpoint.value, peer_address.data(), peer_address.size)
            == SRT_ERROR) {
            std::cerr << "timing rendezvous connect failed: "
                      << srt_getlasterror_str() << '\n';
            return 4;
        }
        connected.value = endpoint.value;
        endpoint.value = SRT_INVALID_SOCK;
    } else {
        connected.value = srt_accept(endpoint.value, nullptr, nullptr);
    }
    std::vector<SRT_SOCKGROUPDATA> initial_group_members;
    if (configuration.group_members != 0U
        && (connected.value == SRT_INVALID_SOCK
            || (connected.value & SRTGROUP_MASK) == 0
            || !wait_for_group_members(connected.value,
                configuration.group_members, initial_group_members))) {
        std::cerr << "timing group did not acquire all members\n";
        return 4;
    }
    int latency_milliseconds = 0;
    int receive_key_length = 0;
    int receive_key_state = static_cast<int>(SRT_KM_S_UNSECURED);
    std::string receive_packet_filter;
    bool latency_valid = false;
    if (configuration.group_members == 0U) {
        latency_valid =
            effective_latency(connected.value, latency_milliseconds);
    } else {
        latency_milliseconds = configuration.latency_milliseconds;
        latency_valid = std::all_of(initial_group_members.begin(),
            initial_group_members.end(),
            [latency_milliseconds](const SRT_SOCKGROUPDATA& member) {
                int member_latency = 0;
                return effective_latency(member.id, member_latency)
                    && member_latency == latency_milliseconds;
            });
    }
    const bool security_valid = configuration.pbkeylen == 0
        || (configuration.group_members == 0U
                   ? effective_receive_security(
                         connected.value, receive_key_length, receive_key_state)
                   : effective_group_receive_security(initial_group_members,
                         configuration.pbkeylen, receive_key_length,
                         receive_key_state))
            && receive_key_length == configuration.pbkeylen
            && receive_key_state == static_cast<int>(SRT_KM_S_SECURED);
    bool crypto_mode_valid = true;
#ifdef ENABLE_AEAD_API_PREVIEW
    if (configuration.crypto_mode >= 0) {
        const auto mode_matches = [&](SRTSOCKET socket) {
            int observed_mode = -1;
            int mode_size = static_cast<int>(sizeof(observed_mode));
            return srt_getsockflag(
                       socket, SRTO_CRYPTOMODE, &observed_mode, &mode_size)
                != SRT_ERROR
                && mode_size == static_cast<int>(sizeof(observed_mode))
                && observed_mode == configuration.crypto_mode;
        };
        crypto_mode_valid = configuration.group_members == 0U
            ? mode_matches(connected.value)
            : std::all_of(initial_group_members.begin(),
                  initial_group_members.end(),
                  [&](const SRT_SOCKGROUPDATA& member) {
                      return mode_matches(member.id);
                  });
    }
#endif
    if (connected.value == SRT_INVALID_SOCK || !latency_valid || !security_valid
        || !crypto_mode_valid) {
        std::cerr << "timing connection or connected-option query failed: "
                  << srt_getlasterror_str() << '\n';
        return 4;
    }
    if (!configuration.packet_filter.empty()
        && (!effective_packet_filter(connected.value, receive_packet_filter)
            || receive_packet_filter != configuration.packet_filter)) {
        std::cerr << "timing packet-filter negotiation failed: "
                  << srt_getlasterror_str() << '\n';
        return 4;
    }
    std::cout << "{\"event\":\"connected\","
                 "\"receiver_latency_microseconds\":"
              << static_cast<std::int64_t>(latency_milliseconds) * 1'000;
    if (configuration.phase_timing) {
        emit_phase_clock_anchor();
    }
    if (configuration.group_members != 0U) {
        std::cout << ",\"group_members\":" << configuration.group_members;
    }
    if (configuration.pbkeylen != 0) {
        std::cout << ",\"receiver_key_length\":" << receive_key_length
                  << ",\"receiver_key_state\":" << receive_key_state;
    }
    if (configuration.crypto_mode >= 0) {
        std::cout << ",\"receiver_crypto_mode\":" << configuration.crypto_mode;
    }
    if (!configuration.packet_filter.empty()) {
        std::cout << ",\"receiver_packet_filter_matched\":true"
                  << ",\"receiver_packet_filter_bytes\":"
                  << receive_packet_filter.size();
    }
    std::cout << "}\n" << std::flush;

    std::string buffer(
        static_cast<std::size_t>(configuration.maximum_message_size), '\0');
    std::vector<TimingObservation> observations;
    observations.reserve(configuration.message_count);
#if defined(__linux__)
    std::vector<std::int64_t> kernel_tx_timestamps(
        configuration.udp_timestamp_source == UdpTimestampSource::linux_software
            ? configuration.message_count
            : 0U,
        0);
    std::size_t kernel_tx_timestamps_received = 0;
#endif
    for (std::size_t index = 0;
         index < configuration.message_count; ++index) {
        std::array<SRT_SOCKGROUPDATA, 4> group_members {};
        SRT_MSGCTRL control = srt_msgctrl_default;
        if (configuration.group_members != 0U) {
            control.grpdata = group_members.data();
            control.grpdata_size = group_members.size();
        }
        // Optional wall-clock boundaries, on the same clock as the existing
        // release/egress observations. No logging inside the measured loop.
        const std::int64_t receive_start =
            configuration.phase_timing ? srt_time_now() : 0;
        const int received = srt_recvmsg2(connected.value, buffer.data(),
            static_cast<int>(buffer.size()), &control);
        const std::int64_t released = srt_time_now();
        if (received == SRT_ERROR || received <= 0
            || control.srctime <= 0) {
            std::cerr << "timed receive " << index << " failed: "
                      << srt_getlasterror_str() << '\n';
            return 5;
        }
        SRTSOCKET running_group_member = SRT_INVALID_SOCK;
        std::size_t running_group_member_count = 0;
        if (configuration.group_members != 0U) {
            if (control.grpdata == nullptr || control.grpdata_size == 0U
                || control.grpdata_size > group_members.size()) {
                std::cerr << "timed group receive " << index
                          << " lacks membership evidence\n";
                return 5;
            }
            for (std::size_t member_index = 0;
                member_index < control.grpdata_size; ++member_index) {
                const auto& member = control.grpdata[member_index];
                if (member.memberstate != SRT_GST_RUNNING) {
                    continue;
                }
                if (member.id == SRT_INVALID_SOCK) {
                    std::cerr << "timed group receive " << index
                              << " has an invalid running member\n";
                    return 5;
                }
                ++running_group_member_count;
                if (running_group_member_count == 1U) {
                    running_group_member = member.id;
                } else {
                    // Haivision exposes every directionally RUNNING member,
                    // but not which one supplied this receive result. Keep
                    // that limitation explicit instead of inventing source
                    // attribution from list order.
                    running_group_member = SRT_INVALID_SOCK;
                }
            }
            if (running_group_member_count == 0U) {
                std::cerr << "timed group receive " << index
                          << " has no running member\n";
                return 5;
            }
        }
        const std::int64_t udp_send_start =
            configuration.phase_timing ? srt_time_now() : 0;
        const int sent = send_udp(
            udp_socket.value, udp_address, buffer.data(), received);
        const std::int64_t egress = srt_time_now();
#if defined(__linux__)
        const std::int64_t userspace_realtime =
            configuration.udp_timestamp_source
                == UdpTimestampSource::linux_software
            ? realtime_nanoseconds()
            : 0;
#else
        const std::int64_t userspace_realtime = 0;
#endif
        if (sent != received) {
            std::cerr << "UDP handoff " << index << " failed; sent="
                      << sent << " expected=" << received << '\n';
            return 6;
        }
#if defined(__linux__)
        if (configuration.udp_timestamp_source
                == UdpTimestampSource::linux_software
            && (userspace_realtime == 0
                || !drain_linux_software_tx_timestamps(udp_socket.value,
                    kernel_tx_timestamps, kernel_tx_timestamps_received))) {
            std::cerr << "UDP TX timestamp collection failed after message "
                      << index << '\n';
            return 6;
        }
#endif
        observations.push_back(TimingObservation {
            index,
            control.srctime,
            released,
            egress,
            userspace_realtime,
            0,
            received,
            control.pktseq,
            control.msgno,
            running_group_member,
            running_group_member_count,
            control.grpdata_size,
            receive_start,
            udp_send_start,
        });
    }
#if defined(__linux__)
    if (configuration.udp_timestamp_source == UdpTimestampSource::linux_software
        && !collect_linux_software_tx_timestamps(udp_socket.value,
            kernel_tx_timestamps, kernel_tx_timestamps_received)) {
        std::cerr << "UDP TX timestamp collection was incomplete; received="
                  << kernel_tx_timestamps_received
                  << " expected=" << configuration.message_count << '\n';
        return 6;
    }
    if (configuration.udp_timestamp_source
        == UdpTimestampSource::linux_software) {
        for (std::size_t index = 0; index < observations.size(); ++index) {
            observations[index].udp_kernel_software_tx_realtime_nanoseconds =
                kernel_tx_timestamps[index];
        }
    }
#endif
    if (configuration.shutdown_grace_milliseconds != 0) {
        // Application delivery can precede the peer's observation of the
        // final cumulative ACK. Keep the connection alive outside the
        // measured interval so a complete transfer never becomes a teardown
        // race in the sender's drain validation.
        std::this_thread::sleep_for(std::chrono::milliseconds{
            configuration.shutdown_grace_milliseconds});
    }
    SRT_TRACEBSTATS statistics {};
    std::int64_t receiver_undecryptable_packets = 0;
    const bool statistics_required =
        configuration.pbkeylen != 0 || !configuration.packet_filter.empty();
    if (statistics_required) {
        if (configuration.group_members == 0U) {
            if (srt_bstats(connected.value, &statistics, 0) == SRT_ERROR) {
                std::cerr << "timing statistics failed: "
                          << srt_getlasterror_str() << '\n';
                return 7;
            }
            receiver_undecryptable_packets = statistics.pktRcvUndecryptTotal;
        } else {
            for (const auto& member : initial_group_members) {
                SRT_TRACEBSTATS member_statistics {};
                if (srt_bstats(member.id, &member_statistics, 0) == SRT_ERROR) {
                    std::cerr << "timing group member statistics failed: "
                              << srt_getlasterror_str() << '\n';
                    return 7;
                }
                receiver_undecryptable_packets +=
                    member_statistics.pktRcvUndecryptTotal;
            }
        }
        if (configuration.pbkeylen != 0
            && receiver_undecryptable_packets != 0) {
            std::cerr << "encrypted timing reported undecryptable packets: "
                      << receiver_undecryptable_packets << '\n';
            return 7;
        }
    }
    // Emit after the measured loop so JSON formatting and pipe backpressure
    // cannot perturb the next message's release or UDP handoff.
    for (const auto& observation : observations) {
        std::cout << "{\"event\":\"timing\",\"message_index\":"
                  << observation.message_index
                  << ",\"tsbpd_deadline_microseconds\":"
                  << observation.tsbpd_deadline_microseconds
                  << ",\"srt_release_microseconds\":"
                  << observation.srt_release_microseconds
                  << ",\"udp_egress_microseconds\":"
                  << observation.udp_egress_microseconds
                  << ",\"payload_bytes\":" << observation.payload_bytes
                  << ",\"packet_sequence\":" << observation.packet_sequence
                  << ",\"message_number\":" << observation.message_number;
        if (configuration.phase_timing) {
            std::cout << ",\"receive_start_microseconds\":"
                      << observation.receive_start_microseconds
                      << ",\"udp_send_start_microseconds\":"
                      << observation.udp_send_start_microseconds;
        }
        if (configuration.udp_timestamp_source
            == UdpTimestampSource::linux_software) {
            std::cout << ",\"udp_userspace_realtime_nanoseconds\":"
                      << observation.udp_userspace_realtime_nanoseconds
                      << ",\"udp_kernel_software_tx_realtime_nanoseconds\":"
                      << observation.udp_kernel_software_tx_realtime_nanoseconds
                      << ",\"udp_kernel_timestamp_id\":"
                      << observation.message_index;
        }
        if (configuration.group_members != 0U) {
            std::cout << ",\"group_running_member\":"
                      << (observation.running_group_member == SRT_INVALID_SOCK
                                 ? 0
                                 : observation.running_group_member)
                      << ",\"group_running_member_count\":"
                      << observation.running_group_member_count
                      << ",\"group_member_count\":"
                      << observation.group_member_count;
        }
        std::cout << "}\n";
    }
    std::cout << "{\"event\":\"complete\",\"messages\":"
              << configuration.message_count;
    if (configuration.phase_timing) {
        emit_phase_clock_anchor();
    }
    if (configuration.udp_timestamp_source
        == UdpTimestampSource::linux_software) {
        std::cout << ",\"udp_kernel_timestamp_source\":"
                     "\"linux-software-tx\""
                  << ",\"udp_kernel_timestamp_clock\":\"CLOCK_REALTIME\""
                  << ",\"udp_kernel_timestamp_hardware\":false"
                  << ",\"udp_kernel_timestamps\":"
                  << configuration.message_count;
    }
    if (configuration.pbkeylen != 0) {
        std::cout << ",\"receiver_undecryptable_packets\":"
                  << receiver_undecryptable_packets;
    }
    if (!configuration.packet_filter.empty()) {
        std::cout << ",\"receiver_filter_extra_packets\":"
                  << statistics.pktRcvFilterExtraTotal
                  << ",\"receiver_filter_supply_packets\":"
                  << statistics.pktRcvFilterSupplyTotal
                  << ",\"receiver_filter_loss_packets\":"
                  << statistics.pktRcvFilterLossTotal;
    }
    std::cout << "}\n" << std::flush;
    return 0;
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
    const int result = run(configuration);
    (void)srt_cleanup();
    return result;
}
