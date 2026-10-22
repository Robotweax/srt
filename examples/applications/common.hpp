/* SPDX-License-Identifier: MIT */
#pragma once

#include <srt/srt.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#ifndef _WIN32
#include <arpa/inet.h>
#endif

namespace samples {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
inline std::mutex output_mutex;

inline void event(const std::string& fields)
{
    std::lock_guard lock(output_mutex);
    std::cout << '{' << fields << "}\n" << std::flush;
}

[[noreturn]] inline void fail(const char* operation)
{
    throw std::runtime_error(
        std::string(operation) + ": " + srt_getlasterror_str());
}

inline int number(std::string_view text, int low, int high)
{
    int value = 0;
    const auto result =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc {} || result.ptr != text.data() + text.size()
        || value < low || value > high) {
        throw std::runtime_error("invalid integer: " + std::string(text));
    }
    return value;
}

struct Arguments {
    std::string role;
    std::map<std::string, std::vector<std::string>> values;
    Arguments(int argc, char** argv, std::string_view allowed)
    {
        if (argc < 2)
            throw std::runtime_error("missing role; use --help");
        role = argv[1];
        for (int i = 2; i < argc; i += 2) {
            const std::string key = argv[i];
            if (i + 1 >= argc
                || allowed.find(" " + key + " ") == std::string_view::npos)
                throw std::runtime_error(
                    "unknown option or missing value: " + key);
            if (key != "--path" && values.contains(key))
                throw std::runtime_error("duplicate option: " + key);
            values[key].emplace_back(argv[i + 1]);
        }
    }
    std::string get(
        const std::string& key, const std::string& fallback = "") const
    {
        const auto it = values.find(key);
        return it == values.end() ? fallback : it->second.front();
    }
    int integer(const std::string& key, int fallback, int low, int high) const
    {
        return number(get(key, std::to_string(fallback)), low, high);
    }
};

struct Runtime {
    Runtime()
    {
        if (srt_startup() == SRT_ERROR)
            fail("startup");
    }
    ~Runtime()
    {
        (void)srt_cleanup();
    }
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
};

struct Socket {
    SRTSOCKET id = SRT_INVALID_SOCK;
    explicit Socket(SRTSOCKET value = SRT_INVALID_SOCK)
        : id(value)
    {
    }
    ~Socket()
    {
        if (id != SRT_INVALID_SOCK)
            (void)srt_close(id);
    }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept
        : id(std::exchange(other.id, SRT_INVALID_SOCK))
    {
    }
};

template <class T>
inline void option(SRTSOCKET s, SRT_SOCKOPT name, const T& value)
{
    if (srt_setsockflag(s, name, &value, sizeof(value)) == SRT_ERROR)
        fail("socket option");
}
inline void text_option(SRTSOCKET s, SRT_SOCKOPT name, const std::string& value)
{
    if (srt_setsockflag(s, name, value.data(), static_cast<int>(value.size()))
        == SRT_ERROR)
        fail("string socket option");
}

inline sockaddr_in address(const std::string& text, bool allow_zero = false)
{
    const auto colon = text.find(':');
    if (colon == std::string::npos)
        throw std::runtime_error("expected numeric IPv4:port");
    sockaddr_in result {};
    result.sin_family = AF_INET;
    result.sin_port = htons(static_cast<std::uint16_t>(number(
        std::string_view(text).substr(colon + 1), allow_zero ? 0 : 1, 65535)));
    if (inet_pton(AF_INET, text.substr(0, colon).c_str(), &result.sin_addr)
        != 1)
        throw std::runtime_error("expected numeric IPv4 address");
    return result;
}

struct Settings {
    int count = 600;
    int interval_ms = 10;
    int timeout_ms = 10000;
    int latency_ms = 120;
    std::string secret_environment;
    std::string crypto = "ctr";
    std::string filter;
    explicit Settings(const Arguments& args)
        : count(args.integer("--count", 600, 1, 1000000))
        , interval_ms(args.integer("--interval-ms", 10, 1, 1000))
        , timeout_ms(args.integer("--timeout-ms", 10000, 1000, 60000))
        , latency_ms(args.integer("--latency-ms", 120, 20, 2000))
        , secret_environment(args.get("--passphrase-env"))
        , crypto(args.get("--crypto", "ctr"))
        , filter(args.get("--filter"))
    {
        if (crypto != "ctr" && crypto != "gcm")
            throw std::runtime_error("crypto must be ctr or gcm");
#ifndef ENABLE_AEAD_API_PREVIEW
        if (crypto == "gcm")
            throw std::runtime_error("GCM requires ENABLE_AEAD_API_PREVIEW=ON");
#endif
        if (crypto == "gcm" && secret_environment.empty())
            throw std::runtime_error("GCM requires --passphrase-env");
        if (timeout_ms <= latency_ms * 2 + 1000)
            throw std::runtime_error(
                "timeout-ms must exceed 2 * latency-ms + 1000 for completion");
    }
};

inline void configure(SRTSOCKET s, const Settings& settings,
    SRT_TRANSTYPE profile, bool group = false)
{
    if (s == SRT_INVALID_SOCK)
        fail("create socket");
    if (!group) {
        option(s, SRTO_TRANSTYPE, profile);
        option(s, SRTO_CONNTIMEO, settings.timeout_ms);
    }
    option(s, SRTO_SNDTIMEO, settings.timeout_ms);
    option(s, SRTO_RCVTIMEO, 100);
    if (profile == SRTT_LIVE) {
        option(s, SRTO_LATENCY, settings.latency_ms);
        if (!settings.filter.empty())
            text_option(s, SRTO_PACKETFILTER, settings.filter);
    }
    if (!settings.secret_environment.empty()) {
        const char* secret = std::getenv(settings.secret_environment.c_str());
        if (!secret || !*secret)
            throw std::runtime_error("missing passphrase environment value");
        text_option(s, SRTO_PASSPHRASE, secret);
        option(s, SRTO_PBKEYLEN, 32);
    }
#ifdef ENABLE_AEAD_API_PREVIEW
    option(s, SRTO_CRYPTOMODE, std::int32_t(settings.crypto == "gcm" ? 2 : 1));
#endif
}

inline Socket listener(const std::string& endpoint, const Settings& settings,
    SRT_TRANSTYPE profile, const std::string& channel, bool groups = false)
{
    Socket socket(srt_create_socket());
    configure(socket.id, settings, profile);
    if (groups)
        option(socket.id, SRTO_GROUPCONNECT, 1);
    option(socket.id, SRTO_RCVSYN, false);
    const auto local = address(endpoint);
    if (srt_bind(
            socket.id, reinterpret_cast<const sockaddr*>(&local), sizeof(local))
        == SRT_ERROR)
        fail("bind");
    if (srt_listen(socket.id, 8) == SRT_ERROR)
        fail("listen");
    event("\"event\":\"ready\",\"channel\":\"" + channel + "\"");
    return socket;
}

inline Socket accept(SRTSOCKET listener_id, const Settings& settings)
{
    const auto deadline =
        Clock::now() + std::chrono::milliseconds(settings.timeout_ms);
    while (Clock::now() < deadline) {
        Socket peer(srt_accept(listener_id, nullptr, nullptr));
        if (peer.id != SRT_INVALID_SOCK) {
            option(peer.id, SRTO_RCVSYN, true);
            option(peer.id, SRTO_RCVTIMEO, 100);
            return peer;
        }
        if (srt_getlasterror(nullptr) != SRT_EASYNCRCV)
            fail("accept");
        std::this_thread::sleep_for(10ms);
    }
    throw std::runtime_error("accept deadline exceeded");
}

inline Socket connect(const std::string& endpoint, const Settings& settings,
    SRT_TRANSTYPE profile)
{
    Socket peer(srt_create_socket());
    configure(peer.id, settings, profile);
    const auto remote = address(endpoint);
    if (srt_connect(
            peer.id, reinterpret_cast<const sockaddr*>(&remote), sizeof(remote))
        == SRT_ERROR)
        fail("connect");
    return peer;
}

inline void stats(SRTSOCKET s, const std::string& channel)
{
    SRT_TRACEBSTATS b {};
    if (srt_bistats(s, &b, 0, 1) == SRT_ERROR)
        fail("statistics");
    event("\"event\":\"stats\",\"channel\":\"" + channel + "\",\"socket\":"
        + std::to_string(s) + ",\"sent\":" + std::to_string(b.pktSentTotal)
        + ",\"received\":" + std::to_string(b.pktRecvTotal)
        + ",\"retransmitted\":" + std::to_string(b.pktRetransTotal)
        + ",\"fec_supplied\":" + std::to_string(b.pktRcvFilterSupplyTotal)
        + ",\"fec_extra\":" + std::to_string(b.pktSndFilterExtraTotal)
        + ",\"receive_drops\":" + std::to_string(b.pktRcvDropTotal)
        + ",\"rtt_ms\":" + std::to_string(b.msRTT)
        + ",\"send_buffer_packets\":" + std::to_string(b.pktSndBuf));
}

// Demo-specific binary framing, independent of the SRT wire protocol. The
// deterministic body validates every byte without trusting peer lengths.
constexpr std::size_t frame_size = 1316;
using Frame = std::array<char, frame_size>;
inline Frame frame(std::uint32_t sequence, char kind = 'D')
{
    Frame bytes {};
    bytes[0] = 'R';
    bytes[1] = 'W';
    bytes[2] = 'A';
    bytes[3] = kind;
    for (int i = 0; i < 4; ++i)
        bytes[4 + i] = static_cast<char>(sequence >> (24 - i * 8));
    for (std::size_t i = 8; i < bytes.size(); ++i)
        bytes[i] = static_cast<char>(
            (sequence * 17U + static_cast<std::uint32_t>(i)) & 255U);
    return bytes;
}
inline std::uint32_t sequence(const Frame& bytes)
{
    std::uint32_t value = 0;
    for (int i = 4; i < 8; ++i)
        value = (value << 8U) | static_cast<unsigned char>(bytes[i]);
    if (bytes != frame(value, bytes[3]))
        throw std::runtime_error("payload integrity failure");
    return value;
}

inline void send(SRTSOCKET s, const Frame& bytes, bool sensor = false)
{
    auto control = srt_msgctrl_default;
    if (sensor)
        control.msgttl = 100;
    if (srt_sendmsg2(s, bytes.data(), static_cast<int>(bytes.size()), &control)
        != static_cast<int>(bytes.size()))
        fail("send message");
}
inline bool receive(SRTSOCKET s, Frame& bytes)
{
    auto control = srt_msgctrl_default;
    const int result =
        srt_recvmsg2(s, bytes.data(), static_cast<int>(bytes.size()), &control);
    if (result == SRT_ERROR) {
        const int error = srt_getlasterror(nullptr);
        if (error == SRT_ETIMEOUT || error == SRT_EASYNCRCV)
            return false;
        fail("receive message");
    }
    if (result != static_cast<int>(bytes.size()))
        throw std::runtime_error("invalid frame size");
    (void)sequence(bytes);
    return true;
}

inline void send_stream(SRTSOCKET s, const Settings& settings,
    const std::string& channel, bool sensor = false, bool commands = false,
    const std::function<void(int)>& hook = {})
{
    auto next = Clock::now();
    auto report = next;
    for (int i = 0; i < settings.count; ++i) {
        if (hook)
            hook(i);
        send(s, frame(static_cast<std::uint32_t>(i)), sensor);
        if (commands) {
            Frame reply {};
            const auto deadline =
                Clock::now() + std::chrono::milliseconds(settings.timeout_ms);
            bool acknowledged = false;
            while (Clock::now() < deadline) {
                if (receive(s, reply)) {
                    if (reply[3] != 'A'
                        || sequence(reply) != static_cast<std::uint32_t>(i))
                        throw std::runtime_error(
                            "unexpected execution acknowledgement");
                    acknowledged = true;
                    break;
                }
            }
            if (!acknowledged)
                throw std::runtime_error(
                    "command execution acknowledgement deadline exceeded");
            event("\"event\":\"executed_ack\",\"channel\":\"control\","
                  "\"request_id\":"
                + std::to_string(i));
        }
        if (Clock::now() >= report) {
            stats(s, channel);
            report = Clock::now() + 1s;
        }
        next = std::max(next + std::chrono::milliseconds(settings.interval_ms),
            Clock::now());
        std::this_thread::sleep_until(next);
    }
    // Finite Live traffic has no reliable tail guarantee. Repeat an explicit
    // application end marker until the receiver acknowledges its drain window.
    const auto deadline =
        Clock::now() + std::chrono::milliseconds(settings.timeout_ms);
    bool complete = false;
    while (Clock::now() < deadline) {
        send(s, frame(static_cast<std::uint32_t>(settings.count), 'E'), sensor);
        Frame reply {};
        if (receive(s, reply)) {
            if (reply[3] != 'F'
                || sequence(reply)
                    != static_cast<std::uint32_t>(settings.count))
                throw std::runtime_error("invalid completion acknowledgement");
            complete = true;
            break;
        }
        std::this_thread::sleep_for(20ms);
    }
    if (!complete)
        throw std::runtime_error("completion deadline exceeded");
    stats(s, channel);
    event("\"event\":\"sent_summary\",\"channel\":\"" + channel
        + "\",\"sent\":" + std::to_string(settings.count));
}

inline void receive_stream(SRTSOCKET s, const Settings& settings,
    const std::string& channel, bool sensor = false, bool commands = false)
{
    std::vector<bool> seen(static_cast<std::size_t>(settings.count), false);
    int unique = 0, duplicate = 0, reordered = 0, highest = -1, executed = 0;
    double maximum_gap_ms = 0;
    auto last_data = Clock::time_point {};
    auto last_activity = Clock::now();
    auto end = Clock::time_point {};
    auto report = Clock::now();
    while (end == Clock::time_point {} || Clock::now() < end) {
        if (Clock::now() - last_activity
            > std::chrono::milliseconds(settings.timeout_ms))
            throw std::runtime_error("receiver idle deadline exceeded");
        Frame bytes {};
        if (!receive(s, bytes))
            continue;
        last_activity = Clock::now();
        const auto index = sequence(bytes);
        if (bytes[3] == 'E') {
            if (index != static_cast<std::uint32_t>(settings.count))
                throw std::runtime_error("peer count differs");
            if (end == Clock::time_point {})
                end = Clock::now()
                    + std::chrono::milliseconds(settings.latency_ms * 2 + 500);
        } else if (bytes[3] == 'D' && index < seen.size()) {
            if (seen[index])
                ++duplicate;
            else {
                if (commands && index != static_cast<std::uint32_t>(executed))
                    throw std::runtime_error("command executed out of order");
                if (static_cast<int>(index) < highest)
                    ++reordered;
                highest = std::max(highest, static_cast<int>(index));
                seen[index] = true;
                ++unique;
                if (commands)
                    ++executed; // Simulated actuator increments once per request.
                if (last_data != Clock::time_point {})
                    maximum_gap_ms = std::max(maximum_gap_ms,
                        std::chrono::duration<double, std::milli>(
                            Clock::now() - last_data)
                            .count());
                last_data = Clock::now();
            }
            if (commands)
                send(s, frame(index, 'A'));
        } else
            throw std::runtime_error("unexpected application frame");
        if (Clock::now() >= report) {
            stats(s, channel);
            report = Clock::now() + 1s;
        }
    }
    if (commands && executed != settings.count)
        throw std::runtime_error("incomplete command execution");
    stats(s, channel);
    event("\"event\":\"summary\",\"channel\":\"" + channel
        + "\",\"expected\":" + std::to_string(settings.count)
        + ",\"unique\":" + std::to_string(unique)
        + ",\"missing\":" + std::to_string(settings.count - unique)
        + ",\"duplicates\":" + std::to_string(duplicate) + ",\"reordered\":"
        + std::to_string(reordered) + ",\"highest_sequence\":"
        + std::to_string(highest) + ",\"executed\":" + std::to_string(executed)
        + ",\"max_receive_gap_ms\":" + std::to_string(maximum_gap_ms));
    send(s, frame(static_cast<std::uint32_t>(settings.count), 'F'), sensor);
    // Allow the final response to pass TSBPD before closing this bounded sample.
    std::this_thread::sleep_for(
        std::chrono::milliseconds(settings.latency_ms * 2 + 200));
}

inline constexpr auto common_options =
    " --count  --interval-ms  --timeout-ms  --latency-ms  --passphrase-env  "
    "--crypto ";
inline constexpr auto common_help =
    "  --count N (600, max 1000000) --interval-ms N (10)\n"
    "  --latency-ms N (120) --timeout-ms N (10000)\n"
    "  --passphrase-env NAME --crypto ctr|gcm\n"
    "Output: JSONL statistics and integrity/continuity summaries. IPv4 only.\n";
} // namespace samples
