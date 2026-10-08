// SPDX-License-Identifier: MIT
// Same-host UDP group diagnostic, using only the public SRT API.
#include "srt.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <iomanip>
#include <memory>
#include <openssl/evp.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <sys/resource.h>
#include <poll.h>
#include <unistd.h>

namespace {
using Clock = std::chrono::steady_clock;
constexpr std::uint64_t warmup_messages = 256;
constexpr std::size_t pending_limit = 64;
constexpr int timeout_ms = 30000;
constexpr std::uint64_t continuation_flag = 1ULL << 63;
constexpr std::uint64_t continuation_limit = 4096;

void require(bool condition, const std::string& context)
{
    if (!condition) {
        throw std::runtime_error(context + ": " + srt_getlasterror_str());
    }
}
struct Socket {
    SRTSOCKET id = SRT_INVALID_SOCK;
    ~Socket()
    {
        if (id != SRT_INVALID_SOCK) {
            srt_close(id);
        }
    }
};
template <class T> void option(SRTSOCKET socket, SRT_SOCKOPT key, T value)
{
    require(srt_setsockflag(socket, key, &value, sizeof(value)) != SRT_ERROR,
        "socket option " + std::to_string(static_cast<int>(key)));
}
void configure(SRTSOCKET socket)
{
    option(socket, SRTO_TRANSTYPE, SRTT_LIVE);
    option(socket, SRTO_LATENCY, 120);
    option(socket, SRTO_TLPKTDROP, false);
    option(socket, SRTO_NAKREPORT, true);
    option(socket, SRTO_SNDTIMEO, timeout_ms);
    option(socket, SRTO_RCVTIMEO, timeout_ms);
}
std::vector<SRT_SOCKGROUPDATA> members(SRTSOCKET group)
{
    // Membership grows during handshakes. A count query followed by an exactly
    // sized fetch races with the next joining member. Reserve the maximum
    // supported by this diagnostic before the first fetch instead.
    std::size_t count = 64;
    std::vector<SRT_SOCKGROUPDATA> result(count);
    require(
        srt_group_data(group, result.data(), &count) != SRT_ERROR, "members");
    result.resize(count);
    return result;
}
bool connected(const std::vector<SRT_SOCKGROUPDATA>& data, int expected)
{
    return data.size() == static_cast<std::size_t>(expected)
        && std::all_of(data.begin(), data.end(), [](const auto& member) {
               return member.sockstate == SRTS_CONNECTED;
           });
}
std::vector<SRT_SOCKGROUPDATA> wait_members(SRTSOCKET group, int count)
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    auto stable = Clock::now();
    while (Clock::now() < deadline) {
        auto data = members(group);
        if (!connected(data, count)) {
            stable = Clock::now();
        } else if (Clock::now() - stable >= std::chrono::milliseconds(120)) {
            return data;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    throw std::runtime_error("not all expected members became connected");
}
std::size_t pending(const std::vector<SRT_SOCKGROUPDATA>& data)
{
    std::size_t maximum = 0;
    for (const auto& member : data) {
        std::size_t blocks = 0;
        std::size_t bytes = 0;
        require(srt_getsndbuffer(member.id, &blocks, &bytes) != SRT_ERROR,
            "member send buffer");
        maximum = std::max(maximum, blocks);
    }
    return maximum;
}
std::vector<SRT_TRACEBSTATS> stats(const std::vector<SRT_SOCKGROUPDATA>& data)
{
    std::vector<SRT_TRACEBSTATS> result(data.size());
    for (std::size_t i = 0; i < data.size(); ++i) {
        require(
            srt_bstats(data[i].id, &result[i], 0) != SRT_ERROR, "member stats");
    }
    return result;
}
std::uint64_t now_us()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
        Clock::now().time_since_epoch())
        .count();
}
void encode(char* buffer, std::uint64_t value)
{
    for (int i = 0; i < 8; ++i) {
        buffer[i] = static_cast<char>((value >> (8 * i)) & 255U);
    }
}
std::uint64_t decode(const char* buffer)
{
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |=
            static_cast<std::uint64_t>(static_cast<unsigned char>(buffer[i]))
            << (8 * i);
    }
    return value;
}
char pattern(std::uint64_t sequence, std::size_t position)
{
    return static_cast<char>((sequence * 29U + position * 17U + 73U) & 255U);
}
double cpu_seconds()
{
    rusage usage {};
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        throw std::runtime_error("getrusage failed");
    }
    return usage.ru_utime.tv_sec + usage.ru_stime.tv_sec
        + (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1000000.0;
}
double percentile(std::vector<double> values, double fraction)
{
    require(!values.empty(), "empty latency sample");
    std::sort(values.begin(), values.end());
    return values[static_cast<std::size_t>((values.size() - 1) * fraction)];
}
void command(const char* expected = "go")
{
    std::string line;
    require(static_cast<bool>(std::getline(std::cin, line)) && line == expected,
        "controller barrier");
}
struct PhaseResult {
    std::string digest;
    std::uint64_t continuation_messages = 0;
    std::vector<std::uint64_t> payload_copies;
    std::vector<std::uint64_t> continuation_copies;
};

bool stop_requested()
{
    pollfd descriptor {STDIN_FILENO, POLLIN, 0};
    const int result = poll(&descriptor, 1, 0);
    require(result >= 0, "controller poll");
    if (result == 0) {
        return false;
    }
    command("stop");
    return true;
}

PhaseResult phase(SRTSOCKET group, const std::vector<SRT_SOCKGROUPDATA>& data,
    bool sender, bool broadcast, std::uint64_t first, std::uint64_t count,
    int size, std::vector<double>& latency, std::uint64_t& first_delivery,
    std::uint64_t& last_delivery)
{
    std::vector<char> buffer(static_cast<std::size_t>(sender ? size : 1500));
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> digest(
        EVP_MD_CTX_new(), EVP_MD_CTX_free);
    require(
        digest && EVP_DigestInit_ex(digest.get(), EVP_sha256(), nullptr) == 1,
        "digest initialization");
    PhaseResult phase_result;
    phase_result.payload_copies.resize(data.size());
    phase_result.continuation_copies.resize(data.size());
    std::vector<SRT_SOCKGROUPDATA> outcomes(data.size());
    const auto send = [&](std::uint64_t index, bool continuation) {
        encode(buffer.data(), index);
        encode(buffer.data() + 8, now_us());
        for (int position = 16; position < size; ++position) {
            buffer[position] = pattern(index, position);
        }
        SRT_MSGCTRL control = srt_msgctrl_default;
        control.inorder = 1;
        control.grpdata = outcomes.data();
        control.grpdata_size = outcomes.size();
        const auto started = now_us();
        const int sent = srt_sendmsg2(group, buffer.data(), size, &control);
        const auto elapsed = now_us() - started;
        require(sent == size, "group send");
        require(control.grpdata == outcomes.data()
                && control.grpdata_size == data.size(),
            "member send outcomes");
        for (std::size_t i = 0; i < data.size(); ++i) {
            const auto found = std::find_if(
                outcomes.begin(), outcomes.end(), [&](const auto& member) {
                    return member.id == data[i].id;
                });
            require(found != outcomes.end(), "member send identity");
            const bool queued = found->result == size;
            if (broadcast) {
                require(queued, "missing broadcast copy");
            }
            if (queued) {
                auto& copies = continuation ? phase_result.continuation_copies
                                            : phase_result.payload_copies;
                ++copies[i];
            }
        }
        if (!continuation) {
            latency.push_back(static_cast<double>(elapsed));
        }
    };
    std::size_t budget = 0;
    for (std::uint64_t index = first; index < first + count;) {
        require(Clock::now() < deadline, "phase timeout");
        if (sender) {
            while (budget == 0) {
                const auto queued = pending(data);
                if (queued < pending_limit) {
                    budget = std::min<std::size_t>(32, pending_limit - queued);
                } else {
                    require(Clock::now() < deadline, "bounded window timeout");
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                }
            }
            --budget;
            send(index, false);
        } else {
            const int received = srt_recvmsg(
                group, buffer.data(), static_cast<int>(buffer.size()));
            const auto arrived = now_us();
            require(received == size, "payload size mismatch");
            const auto received_index = decode(buffer.data());
            const bool continuation = (received_index & continuation_flag) != 0;
            require(continuation || received_index == index,
                "payload sequence mismatch");
            const auto sent = decode(buffer.data() + 8);
            require(sent <= arrived && arrived - sent < timeout_ms * 1000ULL,
                "source timestamp mismatch");
            for (int position = 16; position < size; ++position) {
                require(buffer[position] == pattern(received_index, position),
                    "payload corruption");
            }
            if (continuation) {
                require(++phase_result.continuation_messages
                        <= 2 * continuation_limit,
                    "received continuation budget");
                continue;
            }
            if (index == first) {
                first_delivery = arrived;
            }
            last_delivery = arrived;
            latency.push_back(static_cast<double>(arrived - sent));
        }
        require(EVP_DigestUpdate(digest.get(), buffer.data(), size) == 1,
            "digest update");
        ++index;
    }
    if (sender) {
        bool stop = false;
        for (;;) {
            require(Clock::now() < deadline, "continuation timeout");
            stop = stop || stop_requested();
            bool payload_pending = false;
            for (std::size_t i = 0; i < data.size(); ++i) {
                std::size_t blocks = 0;
                std::size_t bytes = 0;
                require(
                    srt_getsndbuffer(data[i].id, &blocks, &bytes) != SRT_ERROR,
                    "member payload progress");
                // Cumulative ACK retirement is FIFO. Before the payload
                // prefix is ACKed, every queued continuation is still in
                // this buffer. The bound therefore proves prefix retirement.
                payload_pending |= blocks > phase_result.continuation_copies[i];
            }
            if (stop && !payload_pending) {
                break;
            }
            if (pending(data) < pending_limit) {
                require(phase_result.continuation_messages < continuation_limit,
                    "sent continuation budget");
                send(continuation_flag | first, true);
                ++phase_result.continuation_messages;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    unsigned char bytes[EVP_MAX_MD_SIZE] {};
    unsigned int length = 0;
    require(
        EVP_DigestFinal_ex(digest.get(), bytes, &length) == 1 && length == 32,
        "digest finalization");
    std::string result;
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned int i = 0; i < length; ++i) {
        result += hex[bytes[i] >> 4];
        result += hex[bytes[i] & 15];
    }
    phase_result.digest = std::move(result);
    return phase_result;
}
int run(int argc, char** argv)
{
    if (argc != 7) {
        throw std::runtime_error("usage: peer send|receive broadcast|backup "
                                 "port members messages size");
    }
    std::cout << std::setprecision(12);
    const bool sender = std::string(argv[1]) == "send";
    require(sender || std::string(argv[1]) == "receive", "invalid role");
    const std::string mode = argv[2];
    require(mode == "broadcast" || mode == "backup", "invalid mode");
    const int port = std::stoi(argv[3]);
    const int count = std::stoi(argv[4]);
    const auto messages = std::stoull(argv[5]);
    const int size = std::stoi(argv[6]);
    require(port >= 0 && port <= 65535 && count >= 1 && count <= 64
            && messages >= 1 && messages <= 10000000 && size >= 16
            && size <= 1316,
        "invalid numeric arguments");
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<std::uint16_t>(port));
    Socket listener;
    Socket group;
    if (sender) {
        group.id = srt_create_group(
            mode == "broadcast" ? SRT_GTYPE_BROADCAST : SRT_GTYPE_BACKUP);
        require(group.id != SRT_INVALID_SOCK, "create group");
        configure(group.id);
        if (mode == "backup") {
            option(group.id, SRTO_GROUPMINSTABLETIMEO, 60);
        }
        std::vector<SRT_SOCKGROUPCONFIG> endpoints;
        for (int i = 0; i < count; ++i) {
            auto endpoint = srt_prepare_endpoint(nullptr,
                reinterpret_cast<const sockaddr*>(&address), sizeof(address));
            endpoint.token = i + 1;
            endpoint.weight = static_cast<std::uint16_t>(count - i);
            endpoints.push_back(endpoint);
        }
        if (mode == "backup") {
            require(srt_connect_group(group.id, endpoints.data(), 1)
                    != SRT_INVALID_SOCK,
                "connect primary");
            if (count > 1) {
                require(
                    srt_connect_group(group.id, endpoints.data() + 1, count - 1)
                        != SRT_INVALID_SOCK,
                    "connect standby");
            }
        } else {
            require(srt_connect_group(group.id, endpoints.data(), count)
                    != SRT_INVALID_SOCK,
                "connect group");
        }
    } else {
        listener.id = srt_create_socket();
        require(listener.id != SRT_INVALID_SOCK, "create listener");
        configure(listener.id);
        option(listener.id, SRTO_GROUPCONNECT, 1);
        require(srt_bind(listener.id, reinterpret_cast<sockaddr*>(&address),
                    sizeof(address))
                != SRT_ERROR,
            "bind");
        require(srt_listen(listener.id, 128) != SRT_ERROR, "listen");
        int length = sizeof(address);
        require(srt_getsockname(
                    listener.id, reinterpret_cast<sockaddr*>(&address), &length)
                != SRT_ERROR,
            "bound port");
        std::cout << "{\"event\":\"listening\",\"port\":"
                  << ntohs(address.sin_port) << "}\n"
                  << std::flush;
        group.id = srt_accept(listener.id, nullptr, nullptr);
        require(group.id != SRT_INVALID_SOCK && (group.id & SRTGROUP_MASK) != 0,
            "accept group");
    }
    const auto data = wait_members(group.id, count);
    std::cout << "{\"event\":\"ready\"}\n" << std::flush;
    command();
    std::vector<double> latency;
    latency.reserve(static_cast<std::size_t>(messages));
    std::uint64_t first_delivery = 0;
    std::uint64_t last_delivery = 0;
    const auto warmup = phase(group.id, data, sender, mode == "broadcast", 0,
        warmup_messages, size, latency, first_delivery, last_delivery);
    std::cout << "{\"event\":\"warm\"}\n" << std::flush;
    command();
    latency.clear();
    const auto before = stats(data);
    std::cout << "{\"event\":\"armed\"}\n" << std::flush;
    command(); // Both baselines precede the first measured DATA packet.
    const double cpu_before = cpu_seconds();
    const auto started = now_us();
    const auto digest =
        phase(group.id, data, sender, mode == "broadcast", warmup_messages,
            messages, size, latency, first_delivery, last_delivery);
    const double seconds = (now_us() - started) / 1000000.0;
    const double cpu = cpu_seconds() - cpu_before;
    const auto after = stats(data);
    const auto final = members(group.id);
    require(connected(final, count), "member lost during transfer");
    for (const auto& member : data) {
        require(std::any_of(final.begin(), final.end(),
                    [&](const auto& current) {
                        return current.id == member.id;
                    }),
            "member replaced during transfer");
    }
    std::cout << "{\"event\":\"result\",\"messages\":" << messages
              << ",\"useful_bytes\":" << messages * size
              << ",\"members\":" << count << ",\"sha256\":\"" << digest.digest
              << "\""
              << ",\"stream_profile\":\"live-continuation-v2\""
              << ",\"continuation_messages\":" << digest.continuation_messages
              << ",\"seconds\":" << seconds << ",\"cpu_seconds\":" << cpu
              << ",\"delivery_span_seconds\":"
              << (last_delivery - first_delivery) / 1000000.0
              << ",\"latency_us_p50\":" << percentile(latency, 0.5)
              << ",\"latency_us_p99\":" << percentile(latency, 0.99)
              << ",\"member_stats\":[";
    for (std::size_t i = 0; i < data.size(); ++i) {
        if (i != 0) {
            std::cout << ',';
        }
        std::cout << "{\"payload_queued_packets\":" << digest.payload_copies[i]
                  << ",\"continuation_queued_packets\":"
                  << digest.continuation_copies[i]
                  << ",\"previous_continuation_queued_packets\":"
                  << warmup.continuation_copies[i] << ",\"sent_packets\":"
                  << after[i].pktSentTotal - before[i].pktSentTotal
                  << ",\"unique_sent_packets\":"
                  << after[i].pktSentUniqueTotal - before[i].pktSentUniqueTotal
                  << ",\"received_packets\":"
                  << after[i].pktRecvTotal - before[i].pktRecvTotal
                  << ",\"sent_bytes\":"
                  << after[i].byteSentTotal - before[i].byteSentTotal
                  << ",\"received_bytes\":"
                  << after[i].byteRecvTotal - before[i].byteRecvTotal
                  << ",\"retransmitted_packets\":"
                  << after[i].pktRetransTotal - before[i].pktRetransTotal
                  << ",\"send_drops\":"
                  << after[i].pktSndDropTotal - before[i].pktSndDropTotal
                  << ",\"receive_drops\":"
                  << after[i].pktRcvDropTotal - before[i].pktRcvDropTotal
                  << '}';
    }
    std::cout << "]}\n" << std::flush;
    command(); // Keep both sides alive until the controller has both results.
    return 0;
}
} // namespace
int main(int argc, char** argv)
{
    if (srt_startup() == SRT_ERROR) {
        return 2;
    }
    int result = 0;
    try {
        result = run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    srt_cleanup();
    return result;
}
