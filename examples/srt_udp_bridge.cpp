/* SPDX-License-Identifier: MIT */

// A deliberately small public-API live bridge. No private wire framing is added:
// one complete UDP datagram is one SRT Message and one output UDP datagram.
#include <srt/srt.h>

#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <sys/select.h>
#include <unistd.h>
#endif

namespace {

volatile std::sig_atomic_t stopping = 0;
constexpr int max_payload =
    1316; // Seven 188-byte MPEG-TS packets; no RTP header.

void stop(int)
{
    stopping = 1;
}

[[noreturn]] void srt_error(const char* operation)
{
    throw std::runtime_error(
        std::string {operation} + ": " + srt_getlasterror_str());
}

class TransportFailure : public std::runtime_error {
public:
    TransportFailure(const std::string& message, int code)
        : std::runtime_error(message)
        , code(code)
    {
    }
    int code;
};

[[noreturn]] void transport_error(const char* operation)
{
    const int code = srt_getlasterror(nullptr);
    throw TransportFailure(
        std::string {operation} + ": " + srt_getlasterror_str(), code);
}

[[nodiscard]] bool retryable(int code)
{
    // Rejection (including authentication/configuration errors) is deliberately
    // fatal. Do not turn every public API error into an infinite retry loop.
    return code == SRT_ENOSERVER || code == SRT_ECONNLOST || code == SRT_ENOCONN
        || code == SRT_ETIMEOUT;
}

[[nodiscard]] int udp_error_code()
{
#if defined(_WIN32)
    return WSAGetLastError();
#else
    return errno;
#endif
}

[[noreturn]] void udp_error(const char* operation)
{
    throw std::runtime_error(std::string {operation} + ": socket error "
        + std::to_string(udp_error_code()));
}

class Runtime {
public:
    Runtime()
    {
        if (srt_startup() == SRT_ERROR) {
            srt_error("srt_startup");
        }
    }
    ~Runtime()
    {
        (void)srt_cleanup();
    }
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
};

class SrtSocket {
public:
    explicit SrtSocket(SRTSOCKET value)
        : value_(value)
    {
    }
    ~SrtSocket()
    {
        if (value_ != SRT_INVALID_SOCK) {
            (void)srt_close(value_);
        }
    }
    SrtSocket(const SrtSocket&) = delete;
    SrtSocket& operator=(const SrtSocket&) = delete;
    [[nodiscard]] SRTSOCKET get() const
    {
        return value_;
    }

private:
    SRTSOCKET value_;
};

class UdpSocket {
public:
    UdpSocket()
        : value_(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP))
    {
        if (value_ == SYSSOCKET_INVALID) {
            udp_error("UDP socket");
        }
    }
    ~UdpSocket()
    {
#if defined(_WIN32)
        (void)closesocket(value_);
#else
        (void)::close(value_);
#endif
    }
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;
    [[nodiscard]] SYSSOCKET get() const
    {
        return value_;
    }

private:
    SYSSOCKET value_;
};

struct Configuration {
    bool sender = false;
    bool listener = false;
    bool rendezvous = false;
    std::string udp_host = "127.0.0.1";
    std::string srt_host;
    std::string srt_bind_host;
    int srt_local_port = 0;
    int udp_port = 0;
    int srt_port = 0;
    int latency_ms = 120;
    int connect_timeout_ms = 10000;
    std::string udp_interface = "0.0.0.0";
    int udp_ttl = 1;
    bool reconnect = false;
    int reconnect_delay_ms = 1000;
    int peer_idle_timeout_ms = 5000;
    int stats_interval_ms = 1000;
    int input_idle_ms = 2000;
    std::string passphrase_env;
    std::string stream_id;
};

void usage()
{
    std::cout
        << "Usage: robotweax_srt_udp_bridge send|receive [options]\n"
        << "  --udp-host IPv4      UDP input bind (send) / destination "
           "(receive)\n"
        << "                      Default: 127.0.0.1; multicast groups "
           "supported\n"
        << "  --udp-interface IPv4 Multicast local interface address (default "
           "0.0.0.0: OS choice)\n"
        << "  --udp-ttl N         Multicast output TTL, 1..255 (default 1)\n"
        << "  --udp-port PORT      Required UDP input/output port\n"
        << "  --srt-host IPv4      SRT remote (caller/rendezvous) / bind "
           "(listener)\n"
        << "                      Default: 127.0.0.1 caller/rendezvous / "
           "0.0.0.0 "
           "listener\n"
        << "  --srt-port PORT      Required SRT port\n"
        << "  --srt-mode MODE      caller|listener|rendezvous; default "
           "send=caller, "
           "receive=listener\n"
        << "  --srt-bind-host IPv4 Rendezvous local bind address (default "
           "0.0.0.0)\n"
        << "  --srt-local-port PORT Required local port for rendezvous\n"
        << "  --latency-ms MS      SRT latency, 20..60000 (default 120)\n"
        << "  --connect-timeout-ms MS  Caller/rendezvous timeout (default "
           "10000)\n"
        << "  --stream-id ID       Caller Stream ID (optional)\n"
        << "  --reconnect on|off   Retry transient SRT failures (default off)\n"
        << "  --reconnect-delay-ms MS Retry delay, 100..60000 (default 1000)\n"
        << "  --peer-idle-timeout-ms MS SRT peer timeout, 1000..60000 (default "
           "5000)\n"
        << "  --stats-interval-ms MS Status interval, 200..60000 (default "
           "1000)\n"
        << "  --input-idle-ms MS   Input inactivity threshold, 200..60000 "
           "(default 2000)\n"
        << "  --passphrase-env VAR AES-CTR, 256-bit key; read passphrase from "
           "VAR\n"
        << "Payload: raw MPEG-TS, 1..7 aligned 188-byte packets per UDP "
           "datagram.\n"
        << "Start the UDP source after CONNECTED. Ctrl+C stops the bridge.\n"
        << "No RTP, transcoding, or hard-real-time "
           "guarantee.\n";
}

[[nodiscard]] int integer(std::string_view text, int minimum, int maximum)
{
    int value = 0;
    const auto result =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc {} || result.ptr != text.data() + text.size()
        || value < minimum || value > maximum) {
        throw std::runtime_error(
            "invalid numeric option: " + std::string {text});
    }
    return value;
}

[[nodiscard]] Configuration parse(int argc, char** argv)
{
    if (argc < 2
        || (std::string_view {argv[1]} != "send"
            && std::string_view {argv[1]} != "receive")) {
        throw std::runtime_error(
            "choose send or receive; use --help for options");
    }
    Configuration c;
    c.sender = std::string_view {argv[1]} == "send";
    c.listener = !c.sender;
    bool ttl_explicit = false;
    for (int i = 2; i < argc; ++i) {
        const std::string_view option {argv[i]};
        if (++i == argc) {
            throw std::runtime_error(
                "missing value for " + std::string {option});
        }
        const std::string value {argv[i]};
        if (option == "--udp-host") {
            c.udp_host = value;
        } else if (option == "--udp-interface") {
            c.udp_interface = value;
        } else if (option == "--udp-ttl") {
            c.udp_ttl = integer(value, 1, 255);
            ttl_explicit = true;
        } else if (option == "--reconnect") {
            if (value != "on" && value != "off") {
                throw std::runtime_error("--reconnect must be on or off");
            }
            c.reconnect = value == "on";
        } else if (option == "--reconnect-delay-ms") {
            c.reconnect_delay_ms = integer(value, 100, 60000);
        } else if (option == "--peer-idle-timeout-ms") {
            c.peer_idle_timeout_ms = integer(value, 1000, 60000);
        } else if (option == "--stats-interval-ms") {
            c.stats_interval_ms = integer(value, 200, 60000);
        } else if (option == "--input-idle-ms") {
            c.input_idle_ms = integer(value, 200, 60000);
        } else if (option == "--udp-port") {
            c.udp_port = integer(value, 1, 65535);
        } else if (option == "--srt-host") {
            c.srt_host = value;
        } else if (option == "--srt-port") {
            c.srt_port = integer(value, 1, 65535);
        } else if (option == "--srt-bind-host") {
            if (value.empty()) {
                throw std::runtime_error(
                    "--srt-bind-host requires an IPv4 address");
            }
            c.srt_bind_host = value;
        } else if (option == "--srt-local-port") {
            c.srt_local_port = integer(value, 1, 65535);
        } else if (option == "--latency-ms") {
            c.latency_ms = integer(value, 20, 60000);
        } else if (option == "--connect-timeout-ms") {
            c.connect_timeout_ms = integer(value, 1, 60000);
        } else if (option == "--srt-mode") {
            if (value != "caller" && value != "listener"
                && value != "rendezvous") {
                throw std::runtime_error(
                    "--srt-mode must be caller, listener or rendezvous");
            }
            c.listener = value == "listener";
            c.rendezvous = value == "rendezvous";
        } else if (option == "--passphrase-env") {
            if (value.empty()) {
                throw std::runtime_error(
                    "--passphrase-env requires a variable name");
            }
            c.passphrase_env = value;
        } else if (option == "--stream-id") {
            c.stream_id = value;
        } else {
            throw std::runtime_error("unknown option: " + std::string {option});
        }
    }
    if (c.udp_port == 0 || c.srt_port == 0) {
        throw std::runtime_error("--udp-port and --srt-port are required");
    }
    in_addr udp_ip {};
    const bool multicast = inet_pton(AF_INET, c.udp_host.c_str(), &udp_ip) == 1
        && (ntohl(udp_ip.s_addr) & 0xf0000000U) == 0xe0000000U;
    if ((ttl_explicit && (c.sender || !multicast))
        || (c.udp_interface != "0.0.0.0" && !multicast)) {
        throw std::runtime_error(
            "--udp-interface requires multicast; --udp-ttl requires multicast "
            "output (receive)");
    }
    if ((c.listener || c.rendezvous) && !c.stream_id.empty()) {
        throw std::runtime_error(
            "--stream-id is only supported in caller mode");
    }
    if (c.srt_host.empty()) {
        c.srt_host = c.listener ? "0.0.0.0" : "127.0.0.1";
    }
    if (c.rendezvous && c.srt_local_port == 0) {
        throw std::runtime_error("rendezvous requires --srt-local-port");
    }
    if (!c.rendezvous && (c.srt_local_port != 0 || !c.srt_bind_host.empty())) {
        throw std::runtime_error(
            "--srt-local-port and --srt-bind-host require rendezvous mode");
    }
    if (c.srt_bind_host.empty()) {
        c.srt_bind_host = "0.0.0.0";
    }
    return c;
}

[[nodiscard]] sockaddr_in endpoint(const std::string& host, int port,
    bool binding, bool allow_multicast = false)
{
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(port));
    if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
        throw std::runtime_error("a numeric IPv4 address is required: " + host);
    }
    const auto ip = ntohl(address.sin_addr.s_addr);
    if (ip >= 0xf0000000U || (ip >= 0xe0000000U && !allow_multicast)
        || (!binding && ip == 0)) {
        throw std::runtime_error("unicast IPv4 address required: " + host);
    }
    return address;
}

template <typename T>
void option(SRTSOCKET socket, SRT_SOCKOPT name, const T& value)
{
    if (srt_setsockflag(socket, name, &value, static_cast<int>(sizeof(value)))
        == SRT_ERROR) {
        srt_error("srt_setsockflag");
    }
}

void configure(SRTSOCKET socket, const Configuration& c)
{
    option(socket, SRTO_TRANSTYPE, SRTT_LIVE);
    option(socket, SRTO_MESSAGEAPI, true);
    option(socket, SRTO_SENDER, c.sender);
    option(socket, SRTO_TSBPDMODE, true);
    option(socket, SRTO_LATENCY, c.latency_ms);
    option(socket, SRTO_PAYLOADSIZE, max_payload);
    option(socket, SRTO_CONNTIMEO, c.connect_timeout_ms);
    option(socket, SRTO_PEERIDLETIMEO, c.peer_idle_timeout_ms);
    option(socket, SRTO_SNDTIMEO, 1000);
    option(socket, SRTO_RCVTIMEO, 250);
    if (!c.stream_id.empty()
        && srt_setsockflag(socket, SRTO_STREAMID, c.stream_id.data(),
               static_cast<int>(c.stream_id.size()))
            == SRT_ERROR) {
        srt_error("Stream ID");
    }
    if (!c.passphrase_env.empty()) {
        const char* secret = std::getenv(c.passphrase_env.c_str());
        if (secret == nullptr || std::string_view {secret}.size() < 10
            || std::string_view {secret}.size() > 79) {
            throw std::runtime_error(
                "passphrase environment variable must contain 10..79 bytes");
        }
        if (srt_setsockflag(socket, SRTO_PASSPHRASE, secret,
                static_cast<int>(std::string_view {secret}.size()))
            == SRT_ERROR) {
            // Never include secret contents in diagnostics.
            throw std::runtime_error("cannot configure passphrase");
        }
        option(socket, SRTO_PBKEYLEN, 32);
    }
#ifdef ENABLE_AEAD_API_PREVIEW
    // This demo selects CTR explicitly even in an extension-enabled build.
    option(socket, SRTO_CRYPTOMODE, 1);
#endif
}

[[nodiscard]] SRTSOCKET accept_connection(SRTSOCKET listener)
{
    option(listener, SRTO_RCVSYN, false);
    if (srt_listen(listener, 1) == SRT_ERROR) {
        srt_error("srt_listen");
    }
    std::cout << "READY SRT listener" << std::endl;
    while (!stopping) {
        const auto accepted = srt_accept(listener, nullptr, nullptr);
        if (accepted != SRT_INVALID_SOCK) {
            return accepted;
        }
        if (srt_getlasterror(nullptr) != SRT_EASYNCRCV) {
            srt_error("srt_accept");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds {50});
    }
    return SRT_INVALID_SOCK;
}

void validate_ts(const char* bytes, int size)
{
    if (size <= 0 || size > max_payload || size % 188 != 0) {
        throw std::runtime_error(
            "invalid MPEG-TS datagram: need 188..1316 bytes, multiple of 188");
    }
    for (int offset = 0; offset < size; offset += 188) {
        if (static_cast<unsigned char>(bytes[offset]) != 0x47U) {
            throw std::runtime_error(
                "invalid MPEG-TS sync byte (RTP is not supported)");
        }
    }
}

[[nodiscard]] bool udp_readable(SYSSOCKET socket)
{
#if !defined(_WIN32)
    if (socket >= FD_SETSIZE) {
        throw std::runtime_error("UDP descriptor exceeds select capacity");
    }
#endif
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(socket, &readable);
    timeval timeout {0, 250000};
#if defined(_WIN32)
    const int result = select(0, &readable, nullptr, nullptr, &timeout);
#else
    const int result =
        select(socket + 1, &readable, nullptr, nullptr, &timeout);
#endif
    if (result < 0) {
        if (stopping) {
            return false;
        }
        udp_error("UDP select");
    }
    return result > 0;
}

template <typename T>
void udp_option(SYSSOCKET socket, int level, int name, const T& value)
{
    if (setsockopt(socket, level, name, reinterpret_cast<const char*>(&value),
            static_cast<int>(sizeof(value)))
        != 0) {
        udp_error("UDP setsockopt");
    }
}

void configure_udp(
    SYSSOCKET udp, const Configuration& c, const sockaddr_in& address)
{
#if defined(_WIN32)
    const DWORD send_timeout = 1000;
#else
    const timeval send_timeout {1, 0};
#endif
    udp_option(udp, SOL_SOCKET, SO_SNDTIMEO, send_timeout);
    const bool multicast =
        (ntohl(address.sin_addr.s_addr) & 0xf0000000U) == 0xe0000000U;
    const auto local_interface = endpoint(c.udp_interface, 0, true).sin_addr;
    if (c.sender) {
        auto binding = address;
        if (multicast) {
            udp_option(udp, SOL_SOCKET, SO_REUSEADDR, 1);
            binding.sin_addr.s_addr = htonl(INADDR_ANY);
        }
        if (::bind(udp, reinterpret_cast<const sockaddr*>(&binding),
                sizeof(binding))
            != 0) {
            udp_error("UDP bind");
        }
        if (multicast) {
            ip_mreq membership {};
            membership.imr_multiaddr = address.sin_addr;
            membership.imr_interface = local_interface;
            udp_option(udp, IPPROTO_IP, IP_ADD_MEMBERSHIP, membership);
#if defined(__linux__) && defined(IP_MULTICAST_ALL)
            // A shared port must not subscribe this socket to other sockets' groups.
            udp_option(udp, IPPROTO_IP, IP_MULTICAST_ALL, 0);
#endif
        }
    } else if (multicast) {
        udp_option(udp, IPPROTO_IP, IP_MULTICAST_IF, local_interface);
#if defined(_WIN32)
        const DWORD ttl = static_cast<DWORD>(c.udp_ttl);
#else
        const auto ttl = static_cast<unsigned char>(c.udp_ttl);
#endif
        udp_option(udp, IPPROTO_IP, IP_MULTICAST_TTL, ttl);
    }
}

using Clock = std::chrono::steady_clock;

struct ConnectionHistory {
    unsigned generation = 0;
    std::optional<Clock::time_point> disconnected;
};

void relay(SRTSOCKET srt, const Configuration& c, ConnectionHistory& history)
{
    const bool sender = c.sender;
    const auto destination = endpoint(c.udp_host, c.udp_port, sender, true);
    // Do not buffer live input while disconnected. Rejoining/binding only after
    // SRT establishment prevents stale UDP backlog crossing connection epochs.
    UdpSocket udp_socket;
    const auto udp = udp_socket.get();
    configure_udp(udp, c, destination);
    // Receive a full UDP datagram before validating, never a silently truncated TS payload.
    std::array<char, 65536> buffer {};
    std::uint64_t packets = 0;
    std::uint64_t bytes = 0;
    auto last_report = Clock::now();
    auto last_input = last_report;
    std::uint64_t previous_bytes = 0;
    bool received_input = false;
    ++history.generation;
    std::cout << "CONNECTED direction=" << (sender ? "UDP->SRT" : "SRT->UDP")
              << " generation=" << history.generation;
    if (history.disconnected) {
        std::cout << " recovery_ms="
                  << std::chrono::duration_cast<std::chrono::milliseconds>(
                         last_report - *history.disconnected)
                         .count();
        history.disconnected.reset();
    }
    std::cout << std::endl;
    while (!stopping) {
        const auto now = Clock::now();
        if (now - last_report
            >= std::chrono::milliseconds {c.stats_interval_ms}) {
            const auto idle_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - last_input)
                    .count();
            const double rate = static_cast<double>(bytes - previous_bytes)
                * 8.0 / std::chrono::duration<double>(now - last_report).count()
                / 1000000.0;
            std::cout << "STATUS connection=CONNECTED generation="
                      << history.generation << " input="
                      << (idle_ms >= c.input_idle_ms
                                 ? "IDLE"
                                 : (received_input ? "ACTIVE" : "WAITING"))
                      << " input_kind=" << (sender ? "UDP" : "SRT")
                      << " input_age_ms=" << idle_ms
                      << " forwarded_datagrams=" << packets
                      << " forwarded_bytes=" << bytes
                      << " input_payload_mbps=" << rate
                      << " output_payload_mbps=" << rate;
            // Non-destructive snapshot: never reset the application's counters.
            SRT_TRACEBSTATS stats {};
            if (srt_bistats(srt, &stats, 0, 1) == SRT_ERROR) {
                std::cout << " stats_available=false";
            } else {
                std::cout << " stats_available=true rtt_ms=" << stats.msRTT
                          << " retrans_total=" << stats.pktRetransTotal
                          << " snd_drop_total=" << stats.pktSndDropTotal
                          << " rcv_drop_total=" << stats.pktRcvDropTotal
                          << " snd_buffer_bytes=" << stats.byteSndBuf
                          << " rcv_buffer_bytes=" << stats.byteRcvBuf
                          << " snd_buffer_ms=" << stats.msSndBuf
                          << " rcv_buffer_ms=" << stats.msRcvBuf
                          << " snd_latency_ms=" << stats.msSndTsbPdDelay
                          << " rcv_latency_ms=" << stats.msRcvTsbPdDelay;
            }
            std::cout << std::endl;
            last_report = now;
            previous_bytes = bytes;
        }
        if (sender) {
            if (srt_getsockstate(srt) != SRTS_CONNECTED) {
                throw TransportFailure("SRT disconnected", SRT_ECONNLOST);
            }
            if (!udp_readable(udp)) {
                continue;
            }
            const int count = static_cast<int>(
                recv(udp, buffer.data(), static_cast<int>(buffer.size()), 0));
            if (count < 0) {
                if (stopping) {
                    break;
                }
                udp_error("UDP receive");
            }
            validate_ts(buffer.data(), count);
            const int sent = srt_sendmsg(srt, buffer.data(), count, -1, 1);
            if (sent == SRT_ERROR) {
                transport_error("SRT send (backpressure or connection failure; "
                                "current datagram delivery unknown)");
            }
            if (sent != count) {
                throw std::runtime_error("unexpected partial SRT Message send");
            }
            bytes += static_cast<std::uint64_t>(count);
        } else {
            const int count = srt_recvmsg(
                srt, buffer.data(), static_cast<int>(buffer.size()));
            if (count == SRT_ERROR) {
                if (srt_getlasterror(nullptr) == SRT_ETIMEOUT) {
                    continue;
                }
                transport_error("SRT receive");
            }
            validate_ts(buffer.data(), count);
            const int sent = static_cast<int>(sendto(udp, buffer.data(), count,
                0, reinterpret_cast<const sockaddr*>(&destination),
                sizeof(destination)));
            if (sent < 0) {
                udp_error("UDP send");
            }
            if (sent != count) {
                throw std::runtime_error(
                    "unexpected partial UDP datagram send");
            }
            bytes += static_cast<std::uint64_t>(count);
        }
        ++packets;
        received_input = true;
        last_input = Clock::now();
    }
    std::cout << "STOPPED datagrams=" << packets << " bytes=" << bytes
              << std::endl;
}

void session(const Configuration& c, ConnectionHistory& history)
{
    const auto srt_address = endpoint(c.srt_host, c.srt_port, c.listener);
    SrtSocket socket {srt_create_socket()};
    if (socket.get() == SRT_INVALID_SOCK) {
        srt_error("srt_create_socket");
    }
    configure(socket.get(), c);
    if (c.rendezvous) {
        const auto local = endpoint(c.srt_bind_host, c.srt_local_port, true);
        std::cout << "CONNECTING SRT rendezvous local=" << c.srt_bind_host
                  << ':' << c.srt_local_port << " peer=" << c.srt_host << ':'
                  << c.srt_port << std::endl;
        // The public helper enables rendezvous and binds before connecting.
        // Both processes must connect within their connection timeout window.
        if (srt_rendezvous(socket.get(),
                reinterpret_cast<const sockaddr*>(&local), sizeof(local),
                reinterpret_cast<const sockaddr*>(&srt_address),
                sizeof(srt_address))
            == SRT_ERROR) {
            transport_error("srt_rendezvous");
        }
        relay(socket.get(), c, history);
    } else if (c.listener) {
        if (srt_bind(socket.get(),
                reinterpret_cast<const sockaddr*>(&srt_address),
                sizeof(srt_address))
            == SRT_ERROR) {
            srt_error("srt_bind");
        }
        SrtSocket accepted {accept_connection(socket.get())};
        if (accepted.get() != SRT_INVALID_SOCK) {
            option(accepted.get(), SRTO_RCVSYN, true);
            option(accepted.get(), SRTO_RCVTIMEO, 250);
            relay(accepted.get(), c, history);
        }
    } else {
        std::cout << "CONNECTING SRT caller peer=" << c.srt_host << ':'
                  << c.srt_port << std::endl;
        if (srt_connect(socket.get(),
                reinterpret_cast<const sockaddr*>(&srt_address),
                sizeof(srt_address))
            == SRT_ERROR) {
            transport_error("srt_connect");
        }
        relay(socket.get(), c, history);
    }
}

int run(const Configuration& c)
{
    Runtime runtime;
    // Fail invalid addresses before entering the reconnect loop.
    (void)endpoint(c.udp_host, c.udp_port, c.sender, true);
    (void)endpoint(c.udp_interface, 0, true);
    ConnectionHistory history;
    unsigned retries = 0;
    while (!stopping) {
        try {
            session(c, history);
            break;
        } catch (const TransportFailure& error) {
            if (stopping) {
                break;
            }
            if (!history.disconnected) {
                history.disconnected = Clock::now();
            }
            std::cout << "DISCONNECTED generation=" << history.generation
                      << " srt_error=" << error.code
                      << " reason=" << error.what() << std::endl;
            if (!c.reconnect || !retryable(error.code)) {
                throw;
            }
            std::cout << "RECONNECTING attempt=" << ++retries
                      << " delay_ms=" << c.reconnect_delay_ms << std::endl;
            const auto deadline =
                Clock::now() + std::chrono::milliseconds {c.reconnect_delay_ms};
            while (!stopping && Clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds {50});
            }
        }
    }
    std::cout << "STOPPED connections=" << history.generation
              << " retries=" << retries << std::endl;
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc == 2 && std::string_view {argv[1]} == "--help") {
        usage();
        return 0;
    }
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
    try {
        return run(parse(argc, argv));
    } catch (const std::exception& error) {
        std::cerr << "ERROR " << error.what() << '\n';
        return 1;
    }
}
