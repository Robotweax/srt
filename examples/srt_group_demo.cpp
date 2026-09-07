/* SPDX-License-Identifier: MIT */

#include <srt/srt.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <netdb.h>
#endif

namespace {

constexpr std::size_t maximum_group_members = 8;
constexpr std::size_t maximum_message_size = 2'048;

enum class Role {
    caller,
    listener,
};

enum class Policy {
    broadcast,
    backup,
};

enum class CryptoMode {
    ctr,
    gcm,
};

struct Configuration {
    Role role = Role::caller;
    Policy policy = Policy::broadcast;
    std::string bind_address = "127.0.0.1";
    std::string remote_address = "127.0.0.1";
    std::uint16_t port = 0;
    std::size_t member_count = 2;
    std::size_t message_count = 4;
    std::size_t failover_after = 0;
    std::string message = "hello over an SRT connection group";
    std::optional<std::string> passphrase_environment;
    CryptoMode crypto_mode = CryptoMode::ctr;
    std::int32_t key_length = 32;
    std::int32_t timeout_milliseconds = 7'000;
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

struct MemberSummary {
    std::size_t connected = 0;
    std::size_t running = 0;
    std::size_t idle = 0;
    std::size_t broken = 0;
};

struct MessageResult {
    std::string payload;
    std::vector<SRT_SOCKGROUPDATA> members;
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

class SrtConfig {
public:
    SrtConfig()
        : value_(srt_create_config())
    {
        if (value_ == nullptr) {
            throw std::runtime_error(std::string {"srt_create_config failed: "}
                + srt_getlasterror_str());
        }
    }

    ~SrtConfig()
    {
        srt_delete_config(value_);
    }

    SrtConfig(const SrtConfig&) = delete;
    SrtConfig& operator=(const SrtConfig&) = delete;

    [[nodiscard]] SRT_SOCKOPT_CONFIG* get() const noexcept
    {
        return value_;
    }

private:
    SRT_SOCKOPT_CONFIG* value_ = nullptr;
};

class SrtEpoll {
public:
    SrtEpoll()
        : value_(srt_epoll_create())
    {
        if (value_ < 0) {
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
    int value_ = -1;
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
        << "  robotweax_srt_group_demo listener --port PORT [options]\n"
        << "  robotweax_srt_group_demo caller --port PORT [options]\n\n"
        << "Group options:\n"
        << "  --policy broadcast|backup  Group policy (default broadcast)\n"
        << "  --members VALUE             Member count, 2..8 (default 2)\n"
        << "  --messages VALUE            Message count (default 4)\n"
        << "  --failover-after VALUE      Close the active Backup member after "
           "this message\n"
        << "  --message TEXT              Payload prefix\n\n"
        << "Endpoint options:\n"
        << "  --bind ADDRESS              Listener bind address\n"
        << "  --host ADDRESS              Caller destination address\n"
        << "  --port PORT                 Listener UDP port\n"
        << "  --timeout-ms VALUE          Connect and I/O timeout (default "
           "7000)\n\n"
        << "Security options:\n"
        << "  --passphrase-env NAME       Read the passphrase from "
           "environment\n"
        << "  --pbkeylen 16|24|32         AES key length (default 32)\n"
        << "  --crypto ctr|gcm            Encryption mode (default ctr)\n\n"
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
    } else {
        throw std::runtime_error("role must be caller or listener");
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
        } else if (option == "--policy") {
            const std::string_view policy =
                require_value(argc, argv, index, option);
            if (policy == "broadcast") {
                configuration.policy = Policy::broadcast;
            } else if (policy == "backup") {
                configuration.policy = Policy::backup;
            } else {
                throw std::runtime_error(
                    "--policy must be broadcast or backup");
            }
        } else if (option == "--members") {
            configuration.member_count = parse_integer<std::size_t>(
                require_value(argc, argv, index, option), option);
        } else if (option == "--messages") {
            configuration.message_count = parse_integer<std::size_t>(
                require_value(argc, argv, index, option), option);
        } else if (option == "--failover-after") {
            configuration.failover_after = parse_integer<std::size_t>(
                require_value(argc, argv, index, option), option);
        } else if (option == "--message") {
            configuration.message = require_value(argc, argv, index, option);
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
    if (configuration.member_count < 2
        || configuration.member_count > maximum_group_members) {
        throw std::runtime_error("--members must be between 2 and 8");
    }
    if (configuration.message_count == 0
        || configuration.message_count > 1'000) {
        throw std::runtime_error("--messages must be between 1 and 1000");
    }
    if (configuration.message.empty()
        || configuration.message.size() + 32 > maximum_message_size) {
        throw std::runtime_error("--message is empty or too large");
    }
    if (configuration.timeout_milliseconds <= 0) {
        throw std::runtime_error("--timeout-ms must be positive");
    }
    if (configuration.failover_after != 0
        && configuration.policy != Policy::backup) {
        throw std::runtime_error(
            "--failover-after is only valid for Backup groups");
    }
    if (configuration.failover_after >= configuration.message_count) {
        throw std::runtime_error(
            "--failover-after must leave at least one later message");
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

[[nodiscard]] const char* policy_name(Policy policy) noexcept
{
    return policy == Policy::broadcast ? "broadcast" : "backup";
}

[[nodiscard]] SRT_GROUP_TYPE group_type(Policy policy) noexcept
{
    return policy == Policy::broadcast ? SRT_GTYPE_BROADCAST : SRT_GTYPE_BACKUP;
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

void configure_transport(
    SRTSOCKET socket, const Configuration& configuration, bool group)
{
    if (!group) {
        const SRT_TRANSTYPE transport_type = SRTT_LIVE;
        const bool message_api = true;
        set_option(socket, SRTO_TRANSTYPE, transport_type);
        set_option(socket, SRTO_MESSAGEAPI, message_api);
        set_option(socket, SRTO_CONNTIMEO, configuration.timeout_milliseconds);
    }
    set_option(socket, SRTO_SNDTIMEO, configuration.timeout_milliseconds);
    set_option(socket, SRTO_RCVTIMEO, configuration.timeout_milliseconds);

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

[[nodiscard]] std::vector<SRT_SOCKGROUPDATA> group_members(SRTSOCKET group)
{
    for (std::size_t attempt = 0; attempt < 4; ++attempt) {
        std::size_t size = 0;
        if (srt_group_data(group, nullptr, &size) == SRT_ERROR) {
            fail("srt_group_data(size)");
        }
        if (size == 0) {
            return {};
        }
        std::vector<SRT_SOCKGROUPDATA> members(size);
        std::size_t capacity = members.size();
        if (srt_group_data(group, members.data(), &capacity) != SRT_ERROR
            && capacity <= members.size()) {
            members.resize(capacity);
            return members;
        }
    }
    fail("srt_group_data(snapshot)");
}

[[nodiscard]] MemberSummary summarize_members(
    const std::vector<SRT_SOCKGROUPDATA>& members)
{
    MemberSummary result;
    for (const auto& member : members) {
        result.connected += member.sockstate == SRTS_CONNECTED ? 1U : 0U;
        result.running += member.memberstate == SRT_GST_RUNNING ? 1U : 0U;
        result.idle += member.memberstate == SRT_GST_IDLE ? 1U : 0U;
        result.broken += member.memberstate == SRT_GST_BROKEN ? 1U : 0U;
    }
    return result;
}

[[nodiscard]] bool valid_ready_members(
    const std::vector<SRT_SOCKGROUPDATA>& members,
    const Configuration& configuration)
{
    if (members.size() != configuration.member_count) {
        return false;
    }
    if (!std::all_of(members.begin(), members.end(), [](const auto& member) {
            return member.sockstate == SRTS_CONNECTED;
        })) {
        return false;
    }
    // Backup members remain PENDING until the first group I/O selects the
    // active path. Connectivity is therefore the complete readiness contract;
    // later send/receive metadata demonstrates RUNNING versus IDLE state.
    return true;
}

[[nodiscard]] std::vector<SRT_SOCKGROUPDATA> wait_for_members(
    SRTSOCKET group, const Configuration& configuration)
{
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds {configuration.timeout_milliseconds};
    auto stable_since = std::chrono::steady_clock::time_point {};
    while (std::chrono::steady_clock::now() < deadline) {
        const auto members = group_members(group);
        const auto now = std::chrono::steady_clock::now();
        if (valid_ready_members(members, configuration)) {
            if (stable_since == std::chrono::steady_clock::time_point {}) {
                stable_since = now;
            }
            if (now - stable_since >= std::chrono::milliseconds {100}) {
                return members;
            }
        } else {
            stable_since = std::chrono::steady_clock::time_point {};
        }
        std::this_thread::sleep_for(std::chrono::milliseconds {10});
    }
    throw std::runtime_error("group members did not become stably connected");
}

[[nodiscard]] SRTSOCKET running_member(
    const std::vector<SRT_SOCKGROUPDATA>& members) noexcept
{
    SRTSOCKET running = SRT_INVALID_SOCK;
    for (const auto& member : members) {
        if (member.memberstate != SRT_GST_RUNNING
            || member.sockstate != SRTS_CONNECTED) {
            continue;
        }
        if (running != SRT_INVALID_SOCK) {
            return SRT_INVALID_SOCK;
        }
        running = member.id;
    }
    return running;
}

[[nodiscard]] SRTSOCKET wait_for_replacement(
    SRTSOCKET group, SRTSOCKET closed, const Configuration& configuration)
{
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds {configuration.timeout_milliseconds};
    while (std::chrono::steady_clock::now() < deadline) {
        const SRTSOCKET active = running_member(group_members(group));
        if (active != SRT_INVALID_SOCK && active != closed) {
            return active;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds {5});
    }
    throw std::runtime_error("Backup member did not become active");
}

void print_members(
    std::string_view phase, const std::vector<SRT_SOCKGROUPDATA>& members)
{
    const MemberSummary summary = summarize_members(members);
    std::cout << "MEMBERS phase=" << phase << " total=" << members.size()
              << " connected=" << summary.connected
              << " running=" << summary.running << " idle=" << summary.idle
              << " broken=" << summary.broken << '\n';
    for (const auto& member : members) {
        std::cout << "MEMBER phase=" << phase << " id=" << member.id
                  << " token=" << member.token << " weight=" << member.weight
                  << " socket_state=" << member.sockstate
                  << " member_state=" << member.memberstate << '\n';
    }
}

void enable_member_linger(const std::vector<SRT_SOCKGROUPDATA>& members,
    const Configuration& configuration)
{
    linger close_linger {};
    close_linger.l_onoff = 1;
    close_linger.l_linger =
        std::max(1, configuration.timeout_milliseconds / 1'000);
    for (const auto& member : members) {
        set_option(member.id, SRTO_LINGER, close_linger);
    }
}

[[nodiscard]] std::string payload_for(
    const Configuration& configuration, std::size_t index)
{
    return configuration.message + " #" + std::to_string(index + 1);
}

[[nodiscard]] std::string acknowledgement_for(std::size_t index)
{
    return "ACK #" + std::to_string(index + 1);
}

[[nodiscard]] SRTSOCKET send_message(SRTSOCKET group, std::string_view payload)
{
    std::array<SRT_SOCKGROUPDATA, maximum_group_members> members {};
    SRT_MSGCTRL control = srt_msgctrl_default;
    control.grpdata = members.data();
    control.grpdata_size = members.size();
    const int sent = srt_sendmsg2(
        group, payload.data(), static_cast<int>(payload.size()), &control);
    if (sent == SRT_ERROR) {
        fail("srt_sendmsg2(group)");
    }
    if (sent != static_cast<int>(payload.size()) || control.pktseq < 0
        || control.msgno <= 0 || control.grpdata == nullptr
        || control.grpdata_size > members.size()) {
        throw std::runtime_error("group send returned incomplete metadata");
    }
    const std::vector<SRT_SOCKGROUPDATA> snapshot(
        control.grpdata, control.grpdata + control.grpdata_size);
    return running_member(snapshot);
}

[[nodiscard]] bool has_connected_member(SRTSOCKET group)
{
    const auto members = group_members(group);
    return std::any_of(members.begin(), members.end(), [](const auto& member) {
        return member.sockstate == SRTS_CONNECTED;
    });
}

[[nodiscard]] MessageResult receive_message(
    SRTSOCKET group, const Configuration& configuration)
{
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds {configuration.timeout_milliseconds};
    for (;;) {
        std::array<char, maximum_message_size> buffer {};
        std::array<SRT_SOCKGROUPDATA, maximum_group_members> members {};
        SRT_MSGCTRL control = srt_msgctrl_default;
        control.grpdata = members.data();
        control.grpdata_size = members.size();
        const int received = srt_recvmsg2(
            group, buffer.data(), static_cast<int>(buffer.size()), &control);
        if (received != SRT_ERROR) {
            if (control.pktseq < 0 || control.msgno <= 0
                || control.grpdata == nullptr
                || control.grpdata_size > members.size()) {
                throw std::runtime_error(
                    "group receive returned incomplete metadata");
            }
            return MessageResult {
                std::string {buffer.data(), static_cast<std::size_t>(received)},
                std::vector<SRT_SOCKGROUPDATA>(
                    control.grpdata, control.grpdata + control.grpdata_size)};
        }
        const int error = srt_getlasterror(nullptr);
        if (error != SRT_ECONNLOST
            || std::chrono::steady_clock::now() >= deadline
            || !has_connected_member(group)) {
            fail("srt_recvmsg2(group)");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds {1});
    }
}

[[nodiscard]] bool wait_for_group_update(
    int poll, SRTSOCKET group, const Configuration& configuration)
{
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds {configuration.timeout_milliseconds};
    while (std::chrono::steady_clock::now() < deadline) {
        std::array<SRT_EPOLL_EVENT, 4> events {};
        const int count = srt_epoll_uwait(
            poll, events.data(), static_cast<int>(events.size()), 250);
        if (count == SRT_ERROR) {
            fail("srt_epoll_uwait");
        }
        for (int index = 0; index < count; ++index) {
            const auto& event = events[static_cast<std::size_t>(index)];
            if (event.fd == group && (event.events & SRT_EPOLL_UPDATE) != 0) {
                return true;
            }
        }
    }
    return false;
}

void print_member_statistics(SRTSOCKET group)
{
    const auto members = group_members(group);
    for (const auto& member : members) {
        SRT_TRACEBSTATS statistics {};
        if (srt_bistats(member.id, &statistics, 0, 1) == SRT_ERROR) {
            std::cout << "MEMBER_STATS id=" << member.id
                      << " available=false\n";
            continue;
        }
        std::cout << "MEMBER_STATS id=" << member.id
                  << " available=true sent_packets=" << statistics.pktSentTotal
                  << " received_packets=" << statistics.pktRecvTotal
                  << " rtt_ms=" << statistics.msRTT << '\n';
    }
}

[[nodiscard]] SrtSocket establish_listener(const Configuration& configuration)
{
    SrtSocket listener {srt_create_socket()};
    if (listener.get() == SRT_INVALID_SOCK) {
        fail("srt_create_socket");
    }
    configure_transport(listener.get(), configuration, false);
    const int allow_groups = 1;
    set_option(listener.get(), SRTO_GROUPCONNECT, allow_groups);
    const Endpoint local =
        resolve_endpoint(configuration.bind_address, configuration.port);
    if (srt_bind(listener.get(), local.address(), local.size) == SRT_ERROR) {
        fail("srt_bind");
    }
    if (srt_listen(
            listener.get(), static_cast<int>(configuration.member_count + 1))
        == SRT_ERROR) {
        fail("srt_listen");
    }
    return listener;
}

[[nodiscard]] SrtSocket accept_group(
    SRTSOCKET listener, const Configuration& configuration)
{
    std::cout << "READY role=listener policy="
              << policy_name(configuration.policy)
              << " address=" << configuration.bind_address
              << " port=" << configuration.port << '\n'
              << std::flush;
    SrtSocket group {srt_accept(listener, nullptr, nullptr)};
    if (group.get() == SRT_INVALID_SOCK) {
        fail("srt_accept(group)");
    }
    return group;
}

[[nodiscard]] SrtSocket connect_group(const Configuration& configuration)
{
    SrtSocket group {srt_create_group(group_type(configuration.policy))};
    if (group.get() == SRT_INVALID_SOCK) {
        fail("srt_create_group");
    }
    configure_transport(group.get(), configuration, true);
    if (configuration.policy == Policy::backup) {
        constexpr std::int32_t stable_milliseconds = 60;
        set_option(group.get(), SRTO_GROUPMINSTABLETIMEO, stable_milliseconds);
    }

    SrtConfig endpoint_options;
    if (srt_config_add(endpoint_options.get(), SRTO_CONNTIMEO,
            &configuration.timeout_milliseconds,
            static_cast<int>(sizeof(configuration.timeout_milliseconds)))
        == SRT_ERROR) {
        fail("srt_config_add(SRTO_CONNTIMEO)");
    }
    const Endpoint remote =
        resolve_endpoint(configuration.remote_address, configuration.port);
    std::vector<SRT_SOCKGROUPCONFIG> endpoints;
    endpoints.reserve(configuration.member_count);
    for (std::size_t index = 0; index < configuration.member_count; ++index) {
        auto endpoint =
            srt_prepare_endpoint(nullptr, remote.address(), remote.size);
        if (endpoint.errorcode != SRT_SUCCESS) {
            throw std::runtime_error("srt_prepare_endpoint rejected endpoint");
        }
        endpoint.config = endpoint_options.get();
        endpoint.weight = configuration.policy == Policy::backup
            ? static_cast<std::uint16_t>(100 - index * 10)
            : static_cast<std::uint16_t>(1);
        endpoint.token = static_cast<int>(1'001 + index);
        endpoints.push_back(endpoint);
    }

    const SRTSOCKET first = srt_connect_group(
        group.get(), endpoints.data(), static_cast<int>(endpoints.size()));
    if (first == SRT_ERROR) {
        const std::string error = srt_getlasterror_str();
        // The group-level error can be generic. Preserve each member's
        // handshake rejection reason before the group owns their cleanup.
        for (const auto& endpoint : endpoints) {
            if (endpoint.id != SRT_INVALID_SOCK) {
                const int reason = srt_getrejectreason(endpoint.id);
                if (reason != SRT_REJ_UNKNOWN) {
                    std::cerr << "ENDPOINT_REJECT token=" << endpoint.token
                              << " reason=" << reason
                              << " description=" << srt_rejectreason_str(reason)
                              << '\n';
                }
            }
        }
        throw std::runtime_error("srt_connect_group failed: " + error);
    }
    for (const auto& endpoint : endpoints) {
        if (endpoint.id == SRT_INVALID_SOCK
            || endpoint.errorcode != SRT_SUCCESS) {
            throw std::runtime_error(
                "a configured group endpoint did not start successfully");
        }
    }
    return group;
}

void run_listener(const Configuration& configuration)
{
    SrtSocket listener = establish_listener(configuration);
    SrtSocket group = accept_group(listener.get(), configuration);
    const auto ready_members = wait_for_members(group.get(), configuration);
    enable_member_linger(ready_members, configuration);
    print_members("ready", ready_members);

    for (std::size_t index = 0; index < configuration.message_count; ++index) {
        const MessageResult message =
            receive_message(group.get(), configuration);
        const std::string expected = payload_for(configuration, index);
        if (message.payload != expected) {
            throw std::runtime_error("received an unexpected group payload");
        }
        const MemberSummary summary = summarize_members(message.members);
        std::cout << "DELIVERY index=" << index + 1
                  << " bytes=" << message.payload.size()
                  << " members=" << message.members.size()
                  << " running=" << summary.running << " idle=" << summary.idle
                  << '\n';

        SRTSOCKET closed_primary = SRT_INVALID_SOCK;
        if (configuration.failover_after == index + 1) {
            const SRTSOCKET primary =
                running_member(group_members(group.get()));
            if (primary == SRT_INVALID_SOCK) {
                throw std::runtime_error(
                    "could not identify the active Backup member");
            }
            if (srt_close(primary) == SRT_ERROR) {
                fail("srt_close(active Backup member)");
            }
            closed_primary = primary;
        }

        const std::string acknowledgement = acknowledgement_for(index);
        (void)send_message(group.get(), acknowledgement);
        if (closed_primary != SRT_INVALID_SOCK) {
            const SRTSOCKET replacement = wait_for_replacement(
                group.get(), closed_primary, configuration);
            std::cout << "FAILOVER closed_member=" << closed_primary
                      << " replacement_member=" << replacement << '\n'
                      << std::flush;
        }
    }

    const MessageResult completion =
        receive_message(group.get(), configuration);
    if (completion.payload != "DONE") {
        throw std::runtime_error("caller did not complete the demo protocol");
    }
    (void)send_message(group.get(), "DONE");
    print_members("complete", group_members(group.get()));
    print_member_statistics(group.get());
    std::cout << "COMPLETE role=listener policy="
              << policy_name(configuration.policy)
              << " messages=" << configuration.message_count
              << " members=" << configuration.member_count << '\n';
}

void run_caller(const Configuration& configuration)
{
    SrtSocket group = connect_group(configuration);
    const auto ready_members = wait_for_members(group.get(), configuration);
    enable_member_linger(ready_members, configuration);
    print_members("ready", ready_members);

    std::optional<SrtEpoll> update_poll;
    if (configuration.failover_after != 0) {
        update_poll.emplace();
        const int watched = SRT_EPOLL_UPDATE;
        if (srt_epoll_add_usock(update_poll->get(), group.get(), &watched)
            == SRT_ERROR) {
            fail("srt_epoll_add_usock(group)");
        }
    }

    SRTSOCKET first_active = SRT_INVALID_SOCK;
    SRTSOCKET current_active = SRT_INVALID_SOCK;
    std::size_t active_transitions = 0;
    for (std::size_t index = 0; index < configuration.message_count; ++index) {
        const std::string payload = payload_for(configuration, index);
        const SRTSOCKET active = send_message(group.get(), payload);
        if (configuration.policy == Policy::backup) {
            if (active == SRT_INVALID_SOCK) {
                throw std::runtime_error(
                    "Backup send did not identify one active member");
            }
            if (first_active == SRT_INVALID_SOCK) {
                first_active = active;
            } else if (active != current_active) {
                ++active_transitions;
            }
            current_active = active;
        }
        const MessageResult acknowledgement =
            receive_message(group.get(), configuration);
        if (acknowledgement.payload != acknowledgement_for(index)) {
            throw std::runtime_error(
                "listener sent an unexpected acknowledgement");
        }
        std::cout << "SEND index=" << index + 1 << " bytes=" << payload.size();
        if (configuration.policy == Policy::backup) {
            std::cout << " active_member=" << active;
        }
        std::cout << '\n';

        if (configuration.failover_after == index + 1) {
            const bool observed = wait_for_group_update(
                update_poll->get(), group.get(), configuration);
            std::cout << "UPDATE observed=" << (observed ? "true" : "false")
                      << '\n';
            if (!observed) {
                throw std::runtime_error(
                    "Backup failure did not publish SRT_EPOLL_UPDATE");
            }
        }
    }

    (void)send_message(group.get(), "DONE");
    const MessageResult completion =
        receive_message(group.get(), configuration);
    if (completion.payload != "DONE") {
        throw std::runtime_error("listener did not complete the demo protocol");
    }
    if (configuration.failover_after != 0
        && (active_transitions == 0 || first_active == current_active)) {
        throw std::runtime_error(
            "Backup traffic did not move to another member");
    }
    print_members("complete", group_members(group.get()));
    print_member_statistics(group.get());
    std::cout << "COMPLETE role=caller policy="
              << policy_name(configuration.policy)
              << " messages=" << configuration.message_count
              << " members=" << configuration.member_count
              << " active_transitions=" << active_transitions << '\n';
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        std::cout << std::unitbuf;
        const Configuration configuration = parse_arguments(argc, argv);
        SrtRuntime runtime;
        if (configuration.role == Role::listener) {
            run_listener(configuration);
        } else {
            run_caller(configuration);
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
