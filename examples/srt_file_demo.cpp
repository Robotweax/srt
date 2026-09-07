/* SPDX-License-Identifier: MIT */

#include <srt/srt.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
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

constexpr std::array<char, 8> transfer_magic {
    'R', 'W', 'S', 'R', 'T', 'F', '0', '1'};
constexpr std::string_view transfer_acknowledgement = "OK";
constexpr std::string_view completion_message = "DONE";

enum class Role {
    caller,
    listener,
    rendezvous,
};

enum class Direction {
    send,
    receive,
};

enum class CryptoMode {
    ctr,
    gcm,
};

struct Configuration {
    Role role = Role::caller;
    Direction direction = Direction::send;
    std::string bind_address = "127.0.0.1";
    std::string remote_address = "127.0.0.1";
    std::uint16_t port = 0;
    std::uint16_t local_port = 0;
    std::optional<std::string> input_path;
    std::optional<std::string> output_path;
    std::optional<std::string> stream_id;
    std::optional<std::string> expected_stream_id;
    std::optional<std::string> passphrase_environment;
    CryptoMode crypto_mode = CryptoMode::ctr;
    std::int32_t key_length = 32;
    std::int32_t timeout_milliseconds = 7'000;
    std::int32_t block_size = 64 * 1'024;
    std::int64_t maximum_receive_bytes = 1024LL * 1024LL * 1024LL;
    bool key_length_requested = false;
    bool crypto_requested = false;
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

[[noreturn]] void fail(std::string_view operation)
{
    throw std::runtime_error(
        std::string {operation} + " failed: " + srt_getlasterror_str());
}

void print_usage(std::ostream& stream)
{
    stream
        << "Usage:\n"
        << "  robotweax_srt_file_demo listener --port PORT --output FILE "
           "[options]\n"
        << "  robotweax_srt_file_demo caller --port PORT --input FILE "
           "[options]\n"
        << "  robotweax_srt_file_demo rendezvous --local-port PORT "
           "--port PORT (--input FILE | --output FILE) [options]\n\n"
        << "Endpoint and transfer options:\n"
        << "  --bind ADDRESS          Local numeric IP (default 127.0.0.1)\n"
        << "  --host ADDRESS          Remote numeric IP (default 127.0.0.1)\n"
        << "  --port PORT             Listener or remote UDP port\n"
        << "  --local-port PORT       Local Rendezvous UDP port\n"
        << "  --input FILE            Send this file\n"
        << "  --output FILE           Receive into this file (overwritten)\n"
        << "  --max-bytes VALUE       Reject larger incoming files "
           "(default 1 GiB)\n"
        << "  --block-size VALUE      File-helper buffer size "
           "(default 65536)\n"
        << "  --stream-id TEXT        Outgoing Stream ID\n"
        << "  --expect-stream-id TEXT Required Listener Stream ID\n"
        << "  --timeout-ms VALUE      Connect and I/O timeout (default "
           "7000)\n\n"
        << "Security options:\n"
        << "  --passphrase-env NAME   Read the passphrase from environment\n"
        << "  --pbkeylen 16|24|32     AES key length (default 32)\n"
        << "  --crypto ctr|gcm        Payload encryption mode (default ctr)\n\n"
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
        } else if (option == "--input") {
            configuration.input_path = require_value(argc, argv, index, option);
        } else if (option == "--output") {
            configuration.output_path =
                require_value(argc, argv, index, option);
        } else if (option == "--max-bytes") {
            configuration.maximum_receive_bytes = parse_integer<std::int64_t>(
                require_value(argc, argv, index, option), option);
        } else if (option == "--block-size") {
            configuration.block_size = parse_integer<std::int32_t>(
                require_value(argc, argv, index, option), option);
        } else if (option == "--stream-id") {
            configuration.stream_id = require_value(argc, argv, index, option);
        } else if (option == "--expect-stream-id") {
            configuration.expected_stream_id =
                require_value(argc, argv, index, option);
        } else if (option == "--passphrase-env") {
            configuration.passphrase_environment =
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
        throw std::runtime_error("Rendezvous requires --local-port");
    }
    if (configuration.role != Role::rendezvous
        && configuration.local_port != 0) {
        throw std::runtime_error("--local-port is only valid for rendezvous");
    }
    if (configuration.input_path.has_value()
        == configuration.output_path.has_value()) {
        throw std::runtime_error("select exactly one of --input and --output");
    }
    configuration.direction = configuration.input_path.has_value()
        ? Direction::send
        : Direction::receive;
    if (configuration.role == Role::caller
        && configuration.direction != Direction::send) {
        throw std::runtime_error("the Caller requires --input");
    }
    if (configuration.role == Role::listener
        && configuration.direction != Direction::receive) {
        throw std::runtime_error("the Listener requires --output");
    }
    if (configuration.role == Role::listener
        && configuration.stream_id.has_value()) {
        throw std::runtime_error(
            "--stream-id is only valid for outgoing peers");
    }
    if (configuration.role != Role::listener
        && configuration.expected_stream_id.has_value()) {
        throw std::runtime_error(
            "--expect-stream-id is only valid for listeners");
    }
    if (configuration.timeout_milliseconds <= 0) {
        throw std::runtime_error("--timeout-ms must be positive");
    }
    if (configuration.block_size <= 0) {
        throw std::runtime_error("--block-size must be positive");
    }
    if (configuration.maximum_receive_bytes <= 0) {
        throw std::runtime_error("--max-bytes must be positive");
    }
    if (configuration.key_length != 16 && configuration.key_length != 24
        && configuration.key_length != 32) {
        throw std::runtime_error("--pbkeylen must be 16, 24, or 32");
    }
    if (!configuration.passphrase_environment.has_value()
        && (configuration.key_length_requested
            || configuration.crypto_requested)) {
        throw std::runtime_error(
            "--crypto and --pbkeylen require --passphrase-env");
    }
#ifndef ENABLE_AEAD_API_PREVIEW
    if (configuration.crypto_mode == CryptoMode::gcm) {
        throw std::runtime_error(
            "AES-GCM requires a build with ENABLE_AEAD_API_PREVIEW=ON");
    }
#endif
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
    const SRT_TRANSTYPE transport_type = SRTT_FILE;
    const bool message_api = false;
    set_option(socket, SRTO_TRANSTYPE, transport_type);
    set_option(socket, SRTO_MESSAGEAPI, message_api);
    set_option(socket, SRTO_CONNTIMEO, configuration.timeout_milliseconds);
    set_option(socket, SRTO_SNDTIMEO, configuration.timeout_milliseconds);
    set_option(socket, SRTO_RCVTIMEO, configuration.timeout_milliseconds);

    linger close_linger {};
    close_linger.l_onoff = 1;
    close_linger.l_linger =
        std::max(1, configuration.timeout_milliseconds / 1'000);
    set_option(socket, SRTO_LINGER, close_linger);

    if (configuration.stream_id.has_value()) {
        set_string_option(socket, SRTO_STREAMID, *configuration.stream_id);
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

[[nodiscard]] SrtSocket establish_connection(const Configuration& configuration)
{
    SrtSocket socket = create_configured_socket(configuration);
    if (configuration.role == Role::listener) {
        const Endpoint local =
            resolve_endpoint(configuration.bind_address, configuration.port);
        if (srt_bind(socket.get(), local.address(), local.size) == SRT_ERROR) {
            fail("srt_bind");
        }
        if (srt_listen(socket.get(), 1) == SRT_ERROR) {
            fail("srt_listen");
        }
        std::cout << "READY role=listener address="
                  << configuration.bind_address
                  << " port=" << configuration.port << '\n'
                  << std::flush;
        SrtSocket accepted {srt_accept(socket.get(), nullptr, nullptr)};
        if (accepted.get() == SRT_INVALID_SOCK) {
            fail("srt_accept");
        }
        verify_stream_id(accepted.get(), configuration.expected_stream_id);
        return accepted;
    }

    if (configuration.role == Role::caller) {
        const Endpoint remote =
            resolve_endpoint(configuration.remote_address, configuration.port);
        if (srt_connect(socket.get(), remote.address(), remote.size)
            == SRT_ERROR) {
            fail("srt_connect");
        }
        return socket;
    }

    const Endpoint local =
        resolve_endpoint(configuration.bind_address, configuration.local_port);
    const Endpoint remote =
        resolve_endpoint(configuration.remote_address, configuration.port);
    if (local.storage.ss_family != remote.storage.ss_family) {
        throw std::runtime_error(
            "Rendezvous local and remote address families must match");
    }
    if (srt_rendezvous(socket.get(), local.address(), local.size,
            remote.address(), remote.size)
        == SRT_ERROR) {
        fail("srt_rendezvous");
    }
    return socket;
}

void send_all(SRTSOCKET socket, std::string_view bytes)
{
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto remaining = bytes.size() - offset;
        const int requested = static_cast<int>(std::min<std::size_t>(remaining,
            static_cast<std::size_t>(std::numeric_limits<int>::max())));
        const int result = srt_send(socket, bytes.data() + offset, requested);
        if (result == SRT_ERROR) {
            fail("srt_send");
        }
        if (result <= 0) {
            throw std::runtime_error("srt_send made no Stream API progress");
        }
        offset += static_cast<std::size_t>(result);
    }
}

void receive_exact(SRTSOCKET socket, char* destination, std::size_t size)
{
    std::size_t offset = 0;
    while (offset < size) {
        const auto remaining = size - offset;
        const int requested = static_cast<int>(std::min<std::size_t>(remaining,
            static_cast<std::size_t>(std::numeric_limits<int>::max())));
        const int result = srt_recv(socket, destination + offset, requested);
        if (result == SRT_ERROR) {
            fail("srt_recv");
        }
        if (result == 0) {
            throw std::runtime_error(
                "peer closed before the transfer completed");
        }
        offset += static_cast<std::size_t>(result);
    }
}

[[nodiscard]] std::string receive_exact(SRTSOCKET socket, std::size_t size)
{
    std::string result(size, '\0');
    receive_exact(socket, result.data(), result.size());
    return result;
}

[[nodiscard]] std::array<char, 16> encode_transfer_header(
    std::uint64_t file_size)
{
    std::array<char, 16> header {};
    std::copy(transfer_magic.begin(), transfer_magic.end(), header.begin());
    for (std::size_t index = 0; index < 8; ++index) {
        const unsigned int shift = static_cast<unsigned int>((7 - index) * 8);
        header[8 + index] = static_cast<char>((file_size >> shift) & 0xffU);
    }
    return header;
}

[[nodiscard]] std::uint64_t decode_transfer_header(
    const std::array<char, 16>& header)
{
    if (!std::equal(
            transfer_magic.begin(), transfer_magic.end(), header.begin())) {
        throw std::runtime_error("peer sent an invalid file-transfer header");
    }
    std::uint64_t file_size = 0;
    for (std::size_t index = 0; index < 8; ++index) {
        const auto byte = static_cast<unsigned char>(header[8 + index]);
        file_size = (file_size << 8U) | static_cast<std::uint64_t>(byte);
    }
    return file_size;
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

[[nodiscard]] std::int64_t input_file_size(const std::string& path)
{
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (error) {
        throw std::runtime_error(
            "cannot inspect input file: " + path + ": " + error.message());
    }
    if (size > static_cast<std::uintmax_t>(
            std::numeric_limits<std::int64_t>::max())) {
        throw std::runtime_error("input file is too large for srt_sendfile");
    }
    return static_cast<std::int64_t>(size);
}

void run_sender(SRTSOCKET socket, const Configuration& configuration)
{
    const std::string& path = *configuration.input_path;
    const std::int64_t file_size = input_file_size(path);
    const auto header =
        encode_transfer_header(static_cast<std::uint64_t>(file_size));
    send_all(socket, std::string_view {header.data(), header.size()});

    std::int64_t offset = 0;
    const std::int64_t transferred = srt_sendfile(
        socket, path.c_str(), &offset, file_size, configuration.block_size);
    if (transferred == SRT_ERROR) {
        fail("srt_sendfile");
    }
    if (transferred != file_size || offset != file_size) {
        throw std::runtime_error("srt_sendfile completed a short transfer");
    }

    const std::string acknowledgement =
        receive_exact(socket, transfer_acknowledgement.size());
    if (acknowledgement != transfer_acknowledgement) {
        throw std::runtime_error("receiver did not acknowledge the file");
    }
    print_statistics(socket);
    send_all(socket, completion_message);
    const std::string completion =
        receive_exact(socket, completion_message.size());
    if (completion != completion_message) {
        throw std::runtime_error("receiver did not complete the demo protocol");
    }
    std::cout << "TRANSFER sent_bytes=" << transferred << " input=" << path
              << '\n'
              << "COMPLETE direction=send\n";
}

void run_receiver(SRTSOCKET socket, const Configuration& configuration)
{
    std::array<char, 16> header {};
    receive_exact(socket, header.data(), header.size());
    const std::uint64_t unsigned_size = decode_transfer_header(header);
    if (unsigned_size
        > static_cast<std::uint64_t>(configuration.maximum_receive_bytes)) {
        throw std::runtime_error("incoming file exceeds --max-bytes");
    }
    const auto file_size = static_cast<std::int64_t>(unsigned_size);
    const std::string& path = *configuration.output_path;
    std::int64_t offset = 0;
    const std::int64_t transferred = srt_recvfile(
        socket, path.c_str(), &offset, file_size, configuration.block_size);
    if (transferred == SRT_ERROR) {
        fail("srt_recvfile");
    }
    if (transferred != file_size || offset != file_size) {
        throw std::runtime_error("srt_recvfile completed a short transfer");
    }

    send_all(socket, transfer_acknowledgement);
    print_statistics(socket);
    const std::string completion =
        receive_exact(socket, completion_message.size());
    if (completion != completion_message) {
        throw std::runtime_error("sender did not complete the demo protocol");
    }
    send_all(socket, completion_message);
    wait_for_send_drain(socket, configuration.timeout_milliseconds);
    std::cout << "TRANSFER received_bytes=" << transferred << " output=" << path
              << '\n'
              << "COMPLETE direction=receive\n";
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        const Configuration configuration = parse_arguments(argc, argv);
        SrtRuntime runtime;
        SrtSocket connection = establish_connection(configuration);
        if (configuration.direction == Direction::send) {
            run_sender(connection.get(), configuration);
        } else {
            run_receiver(connection.get(), configuration);
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
