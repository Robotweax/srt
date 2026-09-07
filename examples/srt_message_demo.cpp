/* SPDX-License-Identifier: MIT */

#include <srt/srt.h>

#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#if !defined(_WIN32)
#include <netdb.h>
#endif

namespace {

constexpr std::string_view completion_message =
    "__robotweax_srt_demo_complete__";

enum class Role {
    caller,
    listener,
    rendezvous,
};

enum class CryptoMode {
    ctr,
    gcm,
};

struct Configuration {
    Role role = Role::caller;
    std::string bind_address = "127.0.0.1";
    std::string remote_address = "127.0.0.1";
    std::uint16_t port = 0;
    std::uint16_t local_port = 0;
    std::string message = "hello from Robotweax SRT";
    std::optional<std::string> expected_message;
    std::optional<std::string> stream_id;
    std::optional<std::string> expected_stream_id;
    std::optional<std::string> passphrase_environment;
    std::optional<std::string> packet_filter;
    CryptoMode crypto_mode = CryptoMode::ctr;
    std::int32_t key_length = 32;
    std::int32_t timeout_milliseconds = 5'000;
    bool message_requested = false;
    bool key_length_requested = false;
    bool crypto_requested = false;
    bool nonblocking = false;
};

struct Endpoint {
    sockaddr_storage storage {};
    int size = 0;

    [[nodiscard]] const sockaddr* address() const noexcept
    {
        return reinterpret_cast<const sockaddr*>(&storage);
    }
};

class SrtRuntime {
public:
    SrtRuntime()
    {
        if (srt_startup() == SRT_ERROR) {
            throw std::runtime_error(
                std::string {"srt_startup failed: "} + srt_getlasterror_str());
        }
    }

    ~SrtRuntime()
    {
        (void)srt_cleanup();
    }

    SrtRuntime(const SrtRuntime&) = delete;
    SrtRuntime& operator=(const SrtRuntime&) = delete;
};

class SrtSocket {
public:
    SrtSocket() = default;
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

    SrtSocket(SrtSocket&& other) noexcept
        : value_(std::exchange(other.value_, SRT_INVALID_SOCK))
    {
    }

    SrtSocket& operator=(SrtSocket&& other) noexcept
    {
        if (this != &other) {
            if (value_ != SRT_INVALID_SOCK) {
                (void)srt_close(value_);
            }
            value_ = std::exchange(other.value_, SRT_INVALID_SOCK);
        }
        return *this;
    }

    [[nodiscard]] SRTSOCKET get() const noexcept
    {
        return value_;
    }

private:
    SRTSOCKET value_ = SRT_INVALID_SOCK;
};

class SrtEpoll {
public:
    SrtEpoll()
        : value_(srt_epoll_create())
    {
        if (value_ == SRT_ERROR) {
            throw std::runtime_error(std::string {"srt_epoll_create failed: "}
                + srt_getlasterror_str());
        }
    }

    ~SrtEpoll()
    {
        (void)srt_epoll_release(value_);
    }

    SrtEpoll(const SrtEpoll&) = delete;
    SrtEpoll& operator=(const SrtEpoll&) = delete;

    [[nodiscard]] int get() const noexcept
    {
        return value_;
    }

private:
    int value_ = SRT_ERROR;
};

[[noreturn]] void fail(std::string_view operation)
{
    throw std::runtime_error(
        std::string {operation} + " failed: " + srt_getlasterror_str());
}

void print_usage(std::ostream& stream)
{
    stream
        << "Usage:\n"
        << "  robotweax_srt_message_demo listener --port PORT [options]\n"
        << "  robotweax_srt_message_demo caller --port PORT [options]\n"
        << "  robotweax_srt_message_demo rendezvous --local-port PORT "
           "--port PORT [options]\n\n"
        << "Endpoint options:\n"
        << "  --bind ADDRESS          Local numeric IP (default 127.0.0.1)\n"
        << "  --host ADDRESS          Remote numeric IP (default 127.0.0.1)\n"
        << "  --port PORT             Listener or remote UDP port\n"
        << "  --local-port PORT       Local Rendezvous UDP port\n"
        << "  --message TEXT          Caller/Rendezvous payload\n"
        << "  --expect-message TEXT   Expected Rendezvous peer payload\n"
        << "  --stream-id TEXT        Outgoing Stream ID\n"
        << "  --expect-stream-id TEXT Required Listener Stream ID\n"
        << "  --nonblocking           Use epoll-driven nonblocking calls\n\n"
        << "Security and transport options:\n"
        << "  --passphrase-env NAME   Read the passphrase from environment\n"
        << "  --pbkeylen 16|24|32     AES key length (default 32)\n"
        << "  --crypto ctr|gcm        Payload encryption mode (default ctr)\n"
        << "  --packet-filter CONFIG  SRTO_PACKETFILTER value\n"
        << "  --timeout-ms VALUE      Connect and I/O timeout (default "
           "5000)\n\n"
        << "Passphrases are deliberately not accepted as command-line "
           "values.\n";
}

[[nodiscard]] std::string_view require_value(
    int argc, char* argv[], int& index, std::string_view option)
{
    if (index + 1 >= argc) {
        throw std::runtime_error(std::string {option} + " requires a value");
    }
    ++index;
    return argv[index];
}

template <typename Integer>
[[nodiscard]] Integer parse_integer(
    std::string_view text, std::string_view option)
{
    Integer value {};
    const auto result =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc {} || result.ptr != text.data() + text.size()) {
        throw std::runtime_error(std::string {option} + " requires an integer");
    }
    return value;
}

[[nodiscard]] std::uint16_t parse_port(
    std::string_view text, std::string_view option)
{
    const int value = parse_integer<int>(text, option);
    if (value < 1 || value > 65'535) {
        throw std::runtime_error(
            std::string {option} + " must be between 1 and 65535");
    }
    return static_cast<std::uint16_t>(value);
}

[[nodiscard]] Configuration parse_arguments(int argc, char* argv[])
{
    if (argc < 2) {
        print_usage(std::cerr);
        throw std::runtime_error("a role is required");
    }
    const std::string_view role = argv[1];
    if (role == "--help" || role == "-h") {
        print_usage(std::cout);
        std::exit(0);
    }

    Configuration configuration;
    if (role == "caller") {
        configuration.role = Role::caller;
    } else if (role == "listener") {
        configuration.role = Role::listener;
    } else if (role == "rendezvous") {
        configuration.role = Role::rendezvous;
    } else {
        throw std::runtime_error(
            "role must be caller, listener, or rendezvous");
    }

    for (int index = 2; index < argc; ++index) {
        const std::string_view option = argv[index];
        if (option == "--bind") {
            configuration.bind_address =
                require_value(argc, argv, index, option);
        } else if (option == "--host") {
            configuration.remote_address =
                require_value(argc, argv, index, option);
        } else if (option == "--port") {
            configuration.port =
                parse_port(require_value(argc, argv, index, option), option);
        } else if (option == "--local-port") {
            configuration.local_port =
                parse_port(require_value(argc, argv, index, option), option);
        } else if (option == "--message") {
            configuration.message = require_value(argc, argv, index, option);
            configuration.message_requested = true;
        } else if (option == "--expect-message") {
            configuration.expected_message =
                require_value(argc, argv, index, option);
        } else if (option == "--stream-id") {
            configuration.stream_id = require_value(argc, argv, index, option);
        } else if (option == "--expect-stream-id") {
            configuration.expected_stream_id =
                require_value(argc, argv, index, option);
        } else if (option == "--passphrase-env") {
            configuration.passphrase_environment =
                require_value(argc, argv, index, option);
        } else if (option == "--packet-filter") {
            configuration.packet_filter =
                require_value(argc, argv, index, option);
        } else if (option == "--pbkeylen") {
            configuration.key_length = parse_integer<std::int32_t>(
                require_value(argc, argv, index, option), option);
            configuration.key_length_requested = true;
        } else if (option == "--crypto") {
            const std::string_view mode =
                require_value(argc, argv, index, option);
            if (mode == "ctr") {
                configuration.crypto_mode = CryptoMode::ctr;
            } else if (mode == "gcm") {
                configuration.crypto_mode = CryptoMode::gcm;
            } else {
                throw std::runtime_error("--crypto must be ctr or gcm");
            }
            configuration.crypto_requested = true;
        } else if (option == "--timeout-ms") {
            configuration.timeout_milliseconds = parse_integer<std::int32_t>(
                require_value(argc, argv, index, option), option);
        } else if (option == "--nonblocking") {
            configuration.nonblocking = true;
        } else if (option == "--help" || option == "-h") {
            print_usage(std::cout);
            std::exit(0);
        } else {
            throw std::runtime_error("unknown option: " + std::string {option});
        }
    }

    if (configuration.port == 0) {
        throw std::runtime_error("--port is required");
    }
    if (configuration.role == Role::rendezvous
        && configuration.local_port == 0) {
        throw std::runtime_error("rendezvous requires --local-port");
    }
    if (configuration.timeout_milliseconds < 1) {
        throw std::runtime_error("--timeout-ms must be positive");
    }
    if (configuration.key_length != 16 && configuration.key_length != 24
        && configuration.key_length != 32) {
        throw std::runtime_error("--pbkeylen must be 16, 24, or 32");
    }
    if (configuration.message.empty()) {
        throw std::runtime_error("--message must not be empty");
    }
    if (configuration.message.size()
        > static_cast<std::size_t>(SRT_LIVE_DEF_PLSIZE)) {
        throw std::runtime_error(
            "--message exceeds the default Live payload size");
    }
    if (configuration.crypto_mode == CryptoMode::gcm
        && !configuration.passphrase_environment.has_value()) {
        throw std::runtime_error("AES-GCM requires --passphrase-env");
    }
    if ((configuration.crypto_requested || configuration.key_length_requested)
        && !configuration.passphrase_environment.has_value()) {
        throw std::runtime_error(
            "--crypto and --pbkeylen require --passphrase-env");
    }
#ifndef ENABLE_AEAD_API_PREVIEW
    if (configuration.crypto_mode == CryptoMode::gcm) {
        throw std::runtime_error(
            "AES-GCM requires a build with ENABLE_AEAD_API_PREVIEW=ON");
    }
#endif
    if (configuration.role != Role::rendezvous
        && configuration.expected_message.has_value()) {
        throw std::runtime_error(
            "--expect-message is only valid for rendezvous");
    }
    if (configuration.role != Role::listener
        && configuration.expected_stream_id.has_value()) {
        throw std::runtime_error(
            "--expect-stream-id is only valid for listeners");
    }
    if (configuration.role == Role::listener
        && configuration.stream_id.has_value()) {
        throw std::runtime_error(
            "--stream-id is only valid for outgoing peers");
    }
    if (configuration.role == Role::listener
        && configuration.message_requested) {
        throw std::runtime_error(
            "--message is only valid for caller and rendezvous roles");
    }
    if (configuration.role != Role::rendezvous
        && configuration.local_port != 0) {
        throw std::runtime_error("--local-port is only valid for rendezvous");
    }
    return configuration;
}

[[nodiscard]] Endpoint resolve_endpoint(
    std::string_view address, std::uint16_t port)
{
    addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;

    addrinfo* result = nullptr;
    const std::string address_text {address};
    const std::string port_text = std::to_string(port);
    const int status =
        getaddrinfo(address_text.c_str(), port_text.c_str(), &hints, &result);
    if (status != 0 || result == nullptr) {
        throw std::runtime_error("cannot resolve numeric endpoint "
            + address_text + ":" + port_text + ": " + gai_strerror(status));
    }

    Endpoint endpoint;
    if (result->ai_addrlen > sizeof(endpoint.storage)) {
        freeaddrinfo(result);
        throw std::runtime_error("resolved endpoint is too large");
    }
    std::memcpy(&endpoint.storage, result->ai_addr, result->ai_addrlen);
    endpoint.size = static_cast<int>(result->ai_addrlen);
    freeaddrinfo(result);
    return endpoint;
}

template <typename Value>
void set_option(SRTSOCKET socket, SRT_SOCKOPT option, const Value& value)
{
    if (srt_setsockflag(socket, option, &value, static_cast<int>(sizeof(value)))
        == SRT_ERROR) {
        fail("srt_setsockflag");
    }
}

void set_string_option(
    SRTSOCKET socket, SRT_SOCKOPT option, std::string_view value)
{
    if (srt_setsockflag(
            socket, option, value.data(), static_cast<int>(value.size()))
        == SRT_ERROR) {
        fail("srt_setsockflag");
    }
}

void configure_socket(SRTSOCKET socket, const Configuration& configuration)
{
    const SRT_TRANSTYPE transport_type = SRTT_LIVE;
    const bool message_api = true;
    set_option(socket, SRTO_TRANSTYPE, transport_type);
    set_option(socket, SRTO_MESSAGEAPI, message_api);
    set_option(socket, SRTO_CONNTIMEO, configuration.timeout_milliseconds);
    set_option(socket, SRTO_SNDTIMEO, configuration.timeout_milliseconds);
    set_option(socket, SRTO_RCVTIMEO, configuration.timeout_milliseconds);

    if (configuration.nonblocking) {
        const bool synchronous = false;
        set_option(socket, SRTO_SNDSYN, synchronous);
        set_option(socket, SRTO_RCVSYN, synchronous);
    }
    if (configuration.stream_id.has_value()) {
        set_string_option(socket, SRTO_STREAMID, *configuration.stream_id);
    }
    if (configuration.packet_filter.has_value()) {
        set_string_option(
            socket, SRTO_PACKETFILTER, *configuration.packet_filter);
    }
    if (configuration.passphrase_environment.has_value()) {
        const char* passphrase =
            std::getenv(configuration.passphrase_environment->c_str());
        if (passphrase == nullptr || passphrase[0] == '\0') {
            throw std::runtime_error(
                "passphrase environment variable is missing or empty: "
                + *configuration.passphrase_environment);
        }
        set_string_option(socket, SRTO_PASSPHRASE, passphrase);
        set_option(socket, SRTO_PBKEYLEN, configuration.key_length);
    }
#ifdef ENABLE_AEAD_API_PREVIEW
    // The CLI selects a concrete mode; leaving CTR at AUTO would let a
    // listener accept GCM despite an explicit --crypto ctr request.
    const std::int32_t mode =
        configuration.crypto_mode == CryptoMode::gcm ? 2 : 1;
    set_option(socket, SRTO_CRYPTOMODE, mode);
#endif
}

[[nodiscard]] SrtSocket create_configured_socket(
    const Configuration& configuration)
{
    SrtSocket socket {srt_create_socket()};
    if (socket.get() == SRT_INVALID_SOCK) {
        fail("srt_create_socket");
    }
    configure_socket(socket.get(), configuration);
    return socket;
}

void wait_for_event(
    SRTSOCKET socket, int interest, std::int32_t timeout_milliseconds)
{
    SrtEpoll poll;
    const int events = interest | SRT_EPOLL_ERR;
    if (srt_epoll_add_usock(poll.get(), socket, &events) == SRT_ERROR) {
        fail("srt_epoll_add_usock");
    }
    std::array<SRT_EPOLL_EVENT, 2> ready {};
    const int count = srt_epoll_uwait(poll.get(), ready.data(),
        static_cast<int>(ready.size()), timeout_milliseconds);
    if (count == SRT_ERROR) {
        fail("srt_epoll_uwait");
    }
    if (count == 0) {
        throw std::runtime_error("SRT operation timed out while waiting");
    }
    for (int index = 0; index < count; ++index) {
        if (ready[static_cast<std::size_t>(index)].fd == socket
            && (ready[static_cast<std::size_t>(index)].events & interest)
                != 0) {
            return;
        }
    }
    throw std::runtime_error("SRT socket reported an error event");
}

void finish_connect(
    SRTSOCKET socket, const Configuration& configuration, int result)
{
    if (!configuration.nonblocking) {
        if (result == SRT_ERROR) {
            fail("srt_connect");
        }
        return;
    }
    if (result == SRT_ERROR && srt_getlasterror(nullptr) != SRT_EASYNCRCV) {
        fail("srt_connect");
    }
    wait_for_event(socket, SRT_EPOLL_OUT, configuration.timeout_milliseconds);
    if (srt_getsockstate(socket) != SRTS_CONNECTED) {
        throw std::runtime_error("nonblocking SRT connect did not complete");
    }
}

void send_message(SRTSOCKET socket, std::string_view message,
    const Configuration& configuration)
{
    SRT_MSGCTRL control {};
    srt_msgctrl_init(&control);
    for (;;) {
        const int result = srt_sendmsg2(
            socket, message.data(), static_cast<int>(message.size()), &control);
        if (result == static_cast<int>(message.size())) {
            return;
        }
        if (result != SRT_ERROR) {
            throw std::runtime_error("srt_sendmsg2 returned a short message");
        }
        if (!configuration.nonblocking
            || srt_getlasterror(nullptr) != SRT_EASYNCSND) {
            fail("srt_sendmsg2");
        }
        wait_for_event(
            socket, SRT_EPOLL_OUT, configuration.timeout_milliseconds);
    }
}

[[nodiscard]] std::string receive_message(
    SRTSOCKET socket, const Configuration& configuration)
{
    std::array<char, 65'536> buffer {};
    for (;;) {
        SRT_MSGCTRL control {};
        srt_msgctrl_init(&control);
        const int result = srt_recvmsg2(
            socket, buffer.data(), static_cast<int>(buffer.size()), &control);
        if (result > 0) {
            return std::string {
                buffer.data(), static_cast<std::size_t>(result)};
        }
        if (result != SRT_ERROR) {
            throw std::runtime_error(
                "SRT peer closed before sending a message");
        }
        if (!configuration.nonblocking
            || srt_getlasterror(nullptr) != SRT_EASYNCRCV) {
            fail("srt_recvmsg2");
        }
        wait_for_event(
            socket, SRT_EPOLL_IN, configuration.timeout_milliseconds);
    }
}

void print_statistics(SRTSOCKET socket)
{
    SRT_TRACEBSTATS statistics {};
    if (srt_bistats(socket, &statistics, 0, 1) == SRT_ERROR) {
        fail("srt_bistats");
    }
    std::cout << "STATS sent_packets=" << statistics.pktSentTotal
              << " received_packets=" << statistics.pktRecvTotal
              << " retransmitted_packets=" << statistics.pktRetransTotal
              << " rtt_ms=" << statistics.msRTT << '\n';
}

void wait_for_send_drain(SRTSOCKET socket, std::int32_t timeout_milliseconds)
{
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds {timeout_milliseconds};
    for (;;) {
        std::size_t packets = 0;
        std::size_t bytes = 0;
        if (srt_getsndbuffer(socket, &packets, &bytes) == SRT_ERROR) {
            fail("srt_getsndbuffer");
        }
        if (packets == 0 && bytes == 0) {
            return;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error(
                "SRT send buffer did not drain before shutdown");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds {1});
    }
}

void verify_stream_id(
    SRTSOCKET socket, const std::optional<std::string>& expected)
{
    if (!expected.has_value()) {
        return;
    }
    std::array<char, 1'024> value {};
    int size = static_cast<int>(value.size());
    if (srt_getsockflag(socket, SRTO_STREAMID, value.data(), &size)
        == SRT_ERROR) {
        fail("srt_getsockflag(SRTO_STREAMID)");
    }
    const std::string observed {value.data(), static_cast<std::size_t>(size)};
    if (observed != *expected) {
        throw std::runtime_error("unexpected Stream ID: " + observed);
    }
}

[[nodiscard]] SrtSocket accept_connection(
    SRTSOCKET listener, const Configuration& configuration)
{
    for (;;) {
        SrtSocket accepted {srt_accept(listener, nullptr, nullptr)};
        if (accepted.get() != SRT_INVALID_SOCK) {
            if (configuration.nonblocking) {
                const bool synchronous = false;
                set_option(accepted.get(), SRTO_SNDSYN, synchronous);
                set_option(accepted.get(), SRTO_RCVSYN, synchronous);
            }
            return accepted;
        }
        if (!configuration.nonblocking
            || srt_getlasterror(nullptr) != SRT_EASYNCRCV) {
            fail("srt_accept");
        }
        wait_for_event(
            listener, SRT_EPOLL_IN, configuration.timeout_milliseconds);
    }
}

int run_listener(const Configuration& configuration)
{
    SrtSocket listener = create_configured_socket(configuration);
    const Endpoint local =
        resolve_endpoint(configuration.bind_address, configuration.port);
    if (srt_bind(listener.get(), local.address(), local.size) == SRT_ERROR) {
        fail("srt_bind");
    }
    if (srt_listen(listener.get(), 1) == SRT_ERROR) {
        fail("srt_listen");
    }
    std::cout << "READY role=listener address=" << configuration.bind_address
              << " port=" << configuration.port << '\n'
              << std::flush;

    SrtSocket connected = accept_connection(listener.get(), configuration);
    verify_stream_id(connected.get(), configuration.expected_stream_id);
    const std::string received =
        receive_message(connected.get(), configuration);
    std::cout << "RECEIVED bytes=" << received.size() << " message=" << received
              << '\n';
    send_message(connected.get(), received, configuration);
    std::cout << "ECHOED bytes=" << received.size() << '\n';
    print_statistics(connected.get());
    const std::string completion =
        receive_message(connected.get(), configuration);
    if (completion != completion_message) {
        throw std::runtime_error("caller did not complete the demo protocol");
    }
    send_message(connected.get(), completion_message, configuration);
    wait_for_send_drain(connected.get(), configuration.timeout_milliseconds);
    std::cout << "COMPLETE role=listener\n";
    return 0;
}

int run_caller(const Configuration& configuration)
{
    SrtSocket socket = create_configured_socket(configuration);
    const Endpoint remote =
        resolve_endpoint(configuration.remote_address, configuration.port);
    const int result = srt_connect(socket.get(), remote.address(), remote.size);
    finish_connect(socket.get(), configuration, result);

    send_message(socket.get(), configuration.message, configuration);
    const std::string echoed = receive_message(socket.get(), configuration);
    if (echoed != configuration.message) {
        throw std::runtime_error("echo payload did not match the sent message");
    }
    std::cout << "ECHO verified bytes=" << echoed.size()
              << " message=" << echoed << '\n';
    print_statistics(socket.get());
    send_message(socket.get(), completion_message, configuration);
    const std::string completion = receive_message(socket.get(), configuration);
    if (completion != completion_message) {
        throw std::runtime_error("listener did not complete the demo protocol");
    }
    std::cout << "COMPLETE role=caller\n";
    return 0;
}

int run_rendezvous(const Configuration& configuration)
{
    SrtSocket socket = create_configured_socket(configuration);
    const Endpoint local =
        resolve_endpoint(configuration.bind_address, configuration.local_port);
    const Endpoint remote =
        resolve_endpoint(configuration.remote_address, configuration.port);
    if (local.storage.ss_family != remote.storage.ss_family) {
        throw std::runtime_error(
            "Rendezvous local and remote address families must match");
    }

    const int result = srt_rendezvous(socket.get(), local.address(), local.size,
        remote.address(), remote.size);
    finish_connect(socket.get(), configuration, result);

    send_message(socket.get(), configuration.message, configuration);
    const std::string received = receive_message(socket.get(), configuration);
    if (configuration.expected_message.has_value()
        && received != *configuration.expected_message) {
        throw std::runtime_error(
            "Rendezvous payload did not match --expect-message");
    }
    std::cout << "RECEIVED bytes=" << received.size() << " message=" << received
              << '\n';
    print_statistics(socket.get());
    send_message(socket.get(), completion_message, configuration);
    const std::string peer_completion =
        receive_message(socket.get(), configuration);
    if (peer_completion != completion_message) {
        throw std::runtime_error(
            "peer did not complete the Rendezvous demo protocol");
    }
    std::cout << "COMPLETE role=rendezvous\n";
    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        const Configuration configuration = parse_arguments(argc, argv);
        SrtRuntime runtime;
        switch (configuration.role) {
        case Role::caller:
            return run_caller(configuration);
        case Role::listener:
            return run_listener(configuration);
        case Role::rendezvous:
            return run_rendezvous(configuration);
        }
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
    return 1;
}
