// Single-process, many-socket benchmark peer using only the public SRT API.
//
// The same source is linked against Robotweax SRT and pinned Haivision SRT.
// Application work remains on one thread and uses SRT epoll, so observed
// runtime threads belong to the selected library rather than this driver.

#include "srt.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <system_error>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::array<char, 4> payload_magic {'R', 'W', 'X', 'S'};
constexpr std::size_t payload_header_size = 16U;
constexpr int maximum_burst_messages = 64;
constexpr int maximum_round_messages = 128;
constexpr std::size_t maximum_pending_packets_per_socket = 64U;

enum class Role { listener, caller };

struct Configuration {
    Role role = Role::caller;
    std::string host;
    std::uint16_t port = 0;
    std::uint32_t connections = 0;
    std::uint64_t messages_per_connection = 0;
    int message_size = 0;
    int timeout_milliseconds = 60'000;
    int latency_milliseconds = 20;
    int shutdown_grace_milliseconds = 50;
};

struct EndpointAddress {
    sockaddr_storage storage {};
    int size = 0;

    sockaddr* data() noexcept
    {
        return reinterpret_cast<sockaddr*>(&storage);
    }

    const sockaddr* data() const noexcept
    {
        return reinterpret_cast<const sockaddr*>(&storage);
    }
};

class SocketSet {
public:
    SocketSet() = default;
    SocketSet(const SocketSet&) = delete;
    SocketSet& operator=(const SocketSet&) = delete;

    ~SocketSet()
    {
        close_all();
    }

    void add(SRTSOCKET socket)
    {
        sockets_.push_back(socket);
    }

    std::vector<SRTSOCKET>& values() noexcept
    {
        return sockets_;
    }

    const std::vector<SRTSOCKET>& values() const noexcept
    {
        return sockets_;
    }

    void close_all() noexcept
    {
        for (const SRTSOCKET socket : sockets_) {
            if (socket != SRT_INVALID_SOCK) {
                (void)srt_close(socket);
            }
        }
        sockets_.clear();
    }

private:
    std::vector<SRTSOCKET> sockets_;
};

class PollHandle {
public:
    PollHandle()
        : value_(srt_epoll_create())
    {
    }
    PollHandle(const PollHandle&) = delete;
    PollHandle& operator=(const PollHandle&) = delete;

    ~PollHandle()
    {
        if (value_ >= 0) {
            (void)srt_epoll_release(value_);
        }
    }

    int value() const noexcept
    {
        return value_;
    }

private:
    int value_ = -1;
};

struct AggregateStatistics {
    bool available = true;
    std::int64_t packets_sent = 0;
    std::int64_t packets_received = 0;
    std::int64_t packets_sent_unique = 0;
    std::int64_t packets_received_unique = 0;
    std::int64_t packets_retransmitted = 0;
    std::int64_t sender_loss = 0;
    std::int64_t receiver_loss = 0;
    std::int64_t bytes_sent_unique = 0;
    std::int64_t bytes_received_unique = 0;
};

template <class Integer>
bool parse_integer(std::string_view text, Integer& output)
{
    const auto parsed =
        std::from_chars(text.data(), text.data() + text.size(), output);
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

bool parse_arguments(int argc, char** argv, Configuration& configuration)
{
    if (argc < 2) {
        return false;
    }
    const std::string_view role {argv[1]};
    if (role == "listener") {
        configuration.role = Role::listener;
        configuration.host = "0.0.0.0";
    } else if (role == "caller") {
        configuration.role = Role::caller;
        configuration.host = "127.0.0.1";
    } else {
        return false;
    }

    for (int index = 2; index < argc; ++index) {
        const std::string_view option {argv[index]};
        std::string_view value;
        if (option == "--host" && take_value(index, argc, argv, value)) {
            configuration.host = value;
        } else if (option == "--port" && take_value(index, argc, argv, value)) {
            unsigned parsed = 0;
            if (!parse_integer(value, parsed) || parsed == 0U
                || parsed > 65'535U) {
                return false;
            }
            configuration.port = static_cast<std::uint16_t>(parsed);
        } else if (option == "--connections"
            && take_value(index, argc, argv, value)) {
            if (!parse_integer(value, configuration.connections)
                || configuration.connections == 0U
                || configuration.connections > 4'096U) {
                return false;
            }
        } else if (option == "--messages"
            && take_value(index, argc, argv, value)) {
            if (!parse_integer(value, configuration.messages_per_connection)
                || configuration.messages_per_connection == 0U) {
                return false;
            }
        } else if (option == "--message-size"
            && take_value(index, argc, argv, value)) {
            if (!parse_integer(value, configuration.message_size)
                || configuration.message_size
                    < static_cast<int>(payload_header_size)
                || configuration.message_size > 1'456) {
                return false;
            }
        } else if (option == "--timeout-ms"
            && take_value(index, argc, argv, value)) {
            if (!parse_integer(value, configuration.timeout_milliseconds)
                || configuration.timeout_milliseconds <= 0) {
                return false;
            }
        } else if (option == "--latency-ms"
            && take_value(index, argc, argv, value)) {
            if (!parse_integer(value, configuration.latency_milliseconds)
                || configuration.latency_milliseconds < 0) {
                return false;
            }
        } else if (option == "--shutdown-grace-ms"
            && take_value(index, argc, argv, value)) {
            if (!parse_integer(value, configuration.shutdown_grace_milliseconds)
                || configuration.shutdown_grace_milliseconds < 0) {
                return false;
            }
        } else {
            return false;
        }
    }

    return !configuration.host.empty() && configuration.port != 0
        && configuration.connections != 0U
        && configuration.messages_per_connection != 0U
        && configuration.message_size >= static_cast<int>(payload_header_size);
}

bool make_endpoint(
    const std::string& host, std::uint16_t port, EndpointAddress& endpoint)
{
    endpoint = {};
    sockaddr_in ipv4 {};
    ipv4.sin_family = AF_INET;
    ipv4.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &ipv4.sin_addr) == 1) {
        std::memcpy(&endpoint.storage, &ipv4, sizeof(ipv4));
        endpoint.size = static_cast<int>(sizeof(ipv4));
        return true;
    }

    sockaddr_in6 ipv6 {};
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
    if (srt_setsockflag(socket, option, &value, sizeof(value)) == SRT_ERROR) {
        std::cerr << "setsockflag " << static_cast<int>(option)
                  << " failed: " << srt_getlasterror_str() << '\n';
        return false;
    }
    return true;
}

bool configure_socket(
    SRTSOCKET socket, const Configuration& configuration, bool sender)
{
    const SRT_TRANSTYPE transport = SRTT_LIVE;
    const bool message_api = true;
    const int flow_window = 1'024;
    const int send_buffer = 4 * 1'024 * 1'024;
    const int maximum_segment_size = 1'500;
    const std::int64_t maximum_bandwidth = -1;
    const bool too_late_packet_drop = false;
    return set_option(socket, SRTO_TRANSTYPE, transport)
        && set_option(socket, SRTO_MESSAGEAPI, message_api)
        && set_option(socket, SRTO_SENDER, sender)
        && set_option(socket, SRTO_SNDTIMEO, configuration.timeout_milliseconds)
        && set_option(socket, SRTO_RCVTIMEO, configuration.timeout_milliseconds)
        && set_option(socket, SRTO_FC, flow_window)
        && set_option(socket, SRTO_SNDBUF, send_buffer)
        && set_option(socket, SRTO_MSS, maximum_segment_size)
        && set_option(socket, SRTO_PAYLOADSIZE, configuration.message_size)
        && set_option(socket, SRTO_LATENCY, configuration.latency_milliseconds)
        && set_option(socket, SRTO_TLPKTDROP, too_late_packet_drop)
        && set_option(socket, SRTO_MAXBW, maximum_bandwidth);
}

bool make_nonblocking(SRTSOCKET socket)
{
    const bool synchronous = false;
    return set_option(socket, SRTO_SNDSYN, synchronous)
        && set_option(socket, SRTO_RCVSYN, synchronous);
}

void store_u32(char* output, std::uint32_t value) noexcept
{
    output[0] = static_cast<char>((value >> 24U) & 0xffU);
    output[1] = static_cast<char>((value >> 16U) & 0xffU);
    output[2] = static_cast<char>((value >> 8U) & 0xffU);
    output[3] = static_cast<char>(value & 0xffU);
}

void store_u64(char* output, std::uint64_t value) noexcept
{
    for (unsigned index = 0; index < 8U; ++index) {
        output[index] =
            static_cast<char>((value >> ((7U - index) * 8U)) & 0xffU);
    }
}

std::uint32_t load_u32(const char* input) noexcept
{
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(input[0]))
               << 24U)
        | (static_cast<std::uint32_t>(static_cast<unsigned char>(input[1]))
            << 16U)
        | (static_cast<std::uint32_t>(static_cast<unsigned char>(input[2]))
            << 8U)
        | static_cast<std::uint32_t>(static_cast<unsigned char>(input[3]));
}

std::uint64_t load_u64(const char* input) noexcept
{
    std::uint64_t value = 0;
    for (unsigned index = 0; index < 8U; ++index) {
        value = (value << 8U)
            | static_cast<std::uint64_t>(
                static_cast<unsigned char>(input[index]));
    }
    return value;
}

std::vector<char> base_payload(int size)
{
    std::vector<char> payload(static_cast<std::size_t>(size));
    std::copy(payload_magic.begin(), payload_magic.end(), payload.begin());
    for (std::size_t index = payload_header_size; index < payload.size();
        ++index) {
        payload[index] = static_cast<char>((index * 131U + 17U) & 0xffU);
    }
    return payload;
}

void prepare_payload(
    std::vector<char>& payload, std::uint32_t connection, std::uint64_t message)
{
    store_u32(payload.data() + 4, connection);
    store_u64(payload.data() + 8, message);
}

bool validate_payload_body(const std::vector<char>& payload) noexcept
{
    if (!std::equal(
            payload_magic.begin(), payload_magic.end(), payload.begin())) {
        return false;
    }
    for (std::size_t index = payload_header_size; index < payload.size();
        ++index) {
        if (static_cast<unsigned char>(payload[index])
            != static_cast<unsigned char>((index * 131U + 17U) & 0xffU)) {
            return false;
        }
    }
    return true;
}

double percentile(std::vector<std::int64_t> values, double quantile)
{
    std::sort(values.begin(), values.end());
    const double position = static_cast<double>(values.size() - 1U) * quantile;
    const auto lower = static_cast<std::size_t>(position);
    const auto upper = std::min(lower + 1U, values.size() - 1U);
    const double fraction = position - static_cast<double>(lower);
    return static_cast<double>(values[lower])
        + static_cast<double>(values[upper] - values[lower]) * fraction;
}

void print_distribution(const std::vector<std::int64_t>& values)
{
    const auto [minimum, maximum] =
        std::minmax_element(values.begin(), values.end());
    std::cout << "{\"minimum\":" << *minimum
              << ",\"p50\":" << percentile(values, 0.50)
              << ",\"p99\":" << percentile(values, 0.99)
              << ",\"p99_9\":" << percentile(values, 0.999)
              << ",\"maximum\":" << *maximum << '}';
}

AggregateStatistics capture_statistics(const std::vector<SRTSOCKET>& sockets)
{
    AggregateStatistics aggregate;
    for (const SRTSOCKET socket : sockets) {
        SRT_TRACEBSTATS statistics {};
        if (srt_bstats(socket, &statistics, 0) == SRT_ERROR) {
            aggregate.available = false;
            continue;
        }
        aggregate.packets_sent += statistics.pktSentTotal;
        aggregate.packets_received += statistics.pktRecvTotal;
        aggregate.packets_sent_unique += statistics.pktSentUniqueTotal;
        aggregate.packets_received_unique += statistics.pktRecvUniqueTotal;
        aggregate.packets_retransmitted += statistics.pktRetransTotal;
        aggregate.sender_loss += statistics.pktSndLossTotal;
        aggregate.receiver_loss += statistics.pktRcvLossTotal;
        aggregate.bytes_sent_unique += statistics.byteSentUniqueTotal;
        aggregate.bytes_received_unique += statistics.byteRecvUniqueTotal;
    }
    return aggregate;
}

void print_complete(const char* role, const Configuration& configuration,
    Clock::duration elapsed, Clock::duration establishment,
    const std::vector<std::int64_t>& connection_completion,
    const AggregateStatistics& statistics)
{
    const auto elapsed_microseconds =
        std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
    const auto establishment_microseconds =
        std::chrono::duration_cast<std::chrono::microseconds>(establishment)
            .count();
    const std::uint64_t messages =
        static_cast<std::uint64_t>(configuration.connections)
        * configuration.messages_per_connection;
    const std::uint64_t bytes =
        messages * static_cast<std::uint64_t>(configuration.message_size);
    std::cout << "{\"event\":\"complete\",\"role\":\"" << role
              << "\",\"connections\":" << configuration.connections
              << ",\"messages\":" << messages << ",\"bytes\":" << bytes
              << ",\"elapsed_us\":" << elapsed_microseconds
              << ",\"establishment_elapsed_us\":" << establishment_microseconds
              << ",\"srt_version\":" << srt_getversion()
              << ",\"integrity\":true,\"connection_completion_us\":";
    print_distribution(connection_completion);
    if (statistics.available) {
        std::cout << ",\"stats\":{\"pktSentTotal\":" << statistics.packets_sent
                  << ",\"pktRecvTotal\":" << statistics.packets_received
                  << ",\"pktSentUniqueTotal\":"
                  << statistics.packets_sent_unique
                  << ",\"pktRecvUniqueTotal\":"
                  << statistics.packets_received_unique
                  << ",\"pktRetransTotal\":" << statistics.packets_retransmitted
                  << ",\"pktSndLossTotal\":" << statistics.sender_loss
                  << ",\"pktRcvLossTotal\":" << statistics.receiver_loss
                  << ",\"byteSentUniqueTotal\":" << statistics.bytes_sent_unique
                  << ",\"byteRecvUniqueTotal\":"
                  << statistics.bytes_received_unique << '}';
    }
    std::cout << "}\n" << std::flush;
}

bool wait_for_epoll(PollHandle& poll, std::vector<SRT_EPOLL_EVENT>& events,
    int& event_count, Clock::time_point deadline)
{
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - Clock::now());
    if (remaining <= std::chrono::milliseconds::zero()) {
        std::cerr << "many-socket transfer timed out\n";
        return false;
    }
    event_count = srt_epoll_uwait(poll.value(), events.data(),
        static_cast<int>(events.size()),
        std::max<std::int64_t>(1, remaining.count()));
    if (event_count != SRT_ERROR) {
        return true;
    }
    const int error = srt_getlasterror(nullptr);
    if (error == SRT_ETIMEOUT && Clock::now() < deadline) {
        event_count = 0;
        return true;
    }
    std::cerr << "epoll wait failed: " << srt_getlasterror_str() << '\n';
    return false;
}

bool add_connections_to_poll(
    PollHandle& poll, const std::vector<SRTSOCKET>& sockets, int interest)
{
    if (poll.value() < 0) {
        std::cerr << "epoll creation failed: " << srt_getlasterror_str()
                  << '\n';
        return false;
    }
    for (const SRTSOCKET socket : sockets) {
        if (srt_epoll_add_usock(poll.value(), socket, &interest) == SRT_ERROR) {
            std::cerr << "epoll socket registration failed: "
                      << srt_getlasterror_str() << '\n';
            return false;
        }
    }
    return true;
}

bool run_sender(const Configuration& configuration,
    const std::vector<SRTSOCKET>& sockets, Clock::duration establishment)
{
    PollHandle poll;
    if (!add_connections_to_poll(
            poll, sockets, SRT_EPOLL_OUT | SRT_EPOLL_ERR)) {
        return false;
    }
    std::unordered_map<SRTSOCKET, std::size_t> indices;
    for (std::size_t index = 0; index < sockets.size(); ++index) {
        indices.emplace(sockets[index], index);
    }
    std::vector<std::uint64_t> progress(sockets.size(), 0U);
    // A socket may already be writable before it is registered with epoll.
    // Neither implementation is required to replay that readiness edge, so
    // probe every newly registered nonblocking socket once.  Backpressure then
    // clears the flag until epoll makes the socket runnable again.
    std::vector<bool> runnable(sockets.size(), true);
    std::vector<std::int64_t> completion(sockets.size(), 0);
    std::vector<bool> completed(sockets.size(), false);
    std::vector<std::vector<char>> payloads;
    payloads.reserve(sockets.size());
    for (std::size_t index = 0; index < sockets.size(); ++index) {
        payloads.push_back(base_payload(configuration.message_size));
    }
    std::vector<SRT_EPOLL_EVENT> events(sockets.size());
    const std::uint64_t expected_messages =
        static_cast<std::uint64_t>(sockets.size())
        * configuration.messages_per_connection;
    std::uint64_t sent_messages = 0;
    std::size_t cursor = 0;
    const auto started = Clock::now();
    const auto deadline = started
        + std::chrono::milliseconds {configuration.timeout_milliseconds};

    while (sent_messages < expected_messages) {
        if (std::none_of(runnable.begin(), runnable.end(), [](bool value) {
                return value;
            })) {
            int event_count = 0;
            if (!wait_for_epoll(poll, events, event_count, deadline)) {
                std::cerr << "sender progress=" << sent_messages << '/'
                          << expected_messages << '\n';
                return false;
            }
            for (int event_index = 0; event_index < event_count;
                ++event_index) {
                const auto found = indices.find(events[event_index].fd);
                if (found == indices.end()) {
                    std::cerr << "epoll reported an unknown sender socket\n";
                    return false;
                }
                runnable[found->second] = true;
            }
        }
        int round_messages = 0;
        bool application_window_blocked = false;
        std::size_t visited = 0;
        while (visited < sockets.size()
            && round_messages < maximum_round_messages) {
            const std::size_t index = cursor;
            cursor = (cursor + 1U) % sockets.size();
            ++visited;
            if (!runnable[index]) {
                continue;
            }
            std::size_t pending_blocks = 0;
            std::size_t pending_bytes = 0;
            if (srt_getsndbuffer(
                    sockets[index], &pending_blocks, &pending_bytes)
                == SRT_ERROR) {
                std::cerr << "send-buffer query failed: socket_index=" << index
                          << ' ' << srt_getlasterror_str() << '\n';
                return false;
            }
            if (pending_blocks >= maximum_pending_packets_per_socket) {
                application_window_blocked = true;
                continue;
            }
            const int socket_budget =
                static_cast<int>(std::min<std::size_t>(maximum_burst_messages,
                    maximum_pending_packets_per_socket - pending_blocks));
            int burst = 0;
            while (progress[index] < configuration.messages_per_connection
                && burst < socket_budget
                && round_messages < maximum_round_messages) {
                prepare_payload(payloads[index],
                    static_cast<std::uint32_t>(index), progress[index]);
                const int sent = srt_sendmsg(sockets[index],
                    payloads[index].data(), configuration.message_size, -1, 1);
                if (sent == SRT_ERROR) {
                    int system_error = 0;
                    const int srt_error = srt_getlasterror(&system_error);
                    if (srt_error == SRT_EASYNCSND) {
                        runnable[index] = false;
                        break;
                    }
                    std::cerr << "message send failed: socket_index=" << index
                              << " progress=" << progress[index]
                              << " state=" << srt_getsockstate(sockets[index])
                              << " srt_error=" << srt_error
                              << " system_error=" << system_error << ' '
                              << srt_getlasterror_str() << '\n';
                    return false;
                }
                if (sent != configuration.message_size) {
                    std::cerr << "message send was unexpectedly partial\n";
                    return false;
                }
                ++progress[index];
                ++sent_messages;
                ++burst;
                ++round_messages;
            }
            if (progress[index] == configuration.messages_per_connection
                && !completed[index]) {
                runnable[index] = false;
                completion[index] =
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        Clock::now() - started)
                        .count();
                completed[index] = true;
                if (srt_epoll_remove_usock(poll.value(), sockets[index])
                    == SRT_ERROR) {
                    std::cerr << "sender epoll removal failed: "
                              << srt_getlasterror_str() << '\n';
                    return false;
                }
            }
        }
        if (round_messages != 0 && sent_messages < expected_messages) {
            // Keep a fixed application-work budget independent of socket
            // count.  Otherwise one busy application thread can enqueue
            // N-times more work per round and starve the transport workers on
            // small-core systems, which is the opposite of a fair scalability
            // profile.
            std::this_thread::yield();
        } else if (application_window_blocked
            && sent_messages < expected_messages) {
            // srt_getsndbuffer is the portable acknowledgement/backpressure
            // signal shared by both libraries. A short sleep lets their
            // transport workers reduce the bounded application flight without
            // turning this application thread into a busy loop.
            std::this_thread::sleep_for(std::chrono::microseconds {100});
        }
    }

    std::vector<std::size_t> pending_blocks(sockets.size(), 0U);
    std::vector<std::size_t> pending_bytes(sockets.size(), 0U);
    for (;;) {
        bool drained = true;
        for (std::size_t index = 0; index < sockets.size(); ++index) {
            if (srt_getsndbuffer(sockets[index], &pending_blocks[index],
                    &pending_bytes[index])
                == SRT_ERROR) {
                std::cerr << "send-buffer query failed: socket_index=" << index
                          << ' ' << srt_getlasterror_str() << '\n';
                return false;
            }
            drained = drained && pending_blocks[index] == 0U;
        }
        if (drained) {
            break;
        }
        if (Clock::now() >= deadline) {
            std::cerr << "many-socket send buffers did not drain\n";
            for (std::size_t index = 0; index < sockets.size(); ++index) {
                std::cerr << "sender socket_index=" << index
                          << " progress=" << progress[index]
                          << " pending_blocks=" << pending_blocks[index]
                          << " pending_bytes=" << pending_bytes[index]
                          << " state=" << srt_getsockstate(sockets[index])
                          << '\n';
            }
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds {1});
    }

    const AggregateStatistics statistics = capture_statistics(sockets);
    print_complete("caller", configuration, Clock::now() - started,
        establishment, completion, statistics);
    // An empty sender buffer means that the peer acknowledged every packet;
    // it does not mean that TSBPD has released the final Live messages to the
    // receiving application. Keep the process alive after recording transfer
    // completion so shutdown cannot overtake playout and the grace does not
    // contaminate the measured completion time.
    if (configuration.shutdown_grace_milliseconds > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds {
            configuration.shutdown_grace_milliseconds});
    }
    return true;
}

bool run_receiver(const Configuration& configuration,
    const std::vector<SRTSOCKET>& sockets, Clock::duration establishment)
{
    PollHandle poll;
    if (!add_connections_to_poll(poll, sockets, SRT_EPOLL_IN | SRT_EPOLL_ERR)) {
        return false;
    }
    std::unordered_map<SRTSOCKET, std::size_t> indices;
    for (std::size_t index = 0; index < sockets.size(); ++index) {
        indices.emplace(sockets[index], index);
    }
    const std::uint32_t unassigned = std::numeric_limits<std::uint32_t>::max();
    std::vector<std::uint32_t> logical_ids(sockets.size(), unassigned);
    std::vector<bool> claimed_ids(sockets.size(), false);
    std::vector<std::uint64_t> progress(sockets.size(), 0U);
    // Probe once after registration so data that became readable before the
    // epoll add is drained without relying on a replayed readiness edge.
    std::vector<bool> runnable(sockets.size(), true);
    std::vector<std::int64_t> completion(sockets.size(), 0);
    std::vector<bool> completed(sockets.size(), false);
    std::vector<char> payload = base_payload(configuration.message_size);
    std::vector<SRT_EPOLL_EVENT> events(sockets.size());
    const std::uint64_t expected_messages =
        static_cast<std::uint64_t>(sockets.size())
        * configuration.messages_per_connection;
    std::uint64_t received_messages = 0;
    std::size_t cursor = 0;
    const auto started = Clock::now();
    const auto deadline = started
        + std::chrono::milliseconds {configuration.timeout_milliseconds};

    while (received_messages < expected_messages) {
        if (std::none_of(runnable.begin(), runnable.end(), [](bool value) {
                return value;
            })) {
            int event_count = 0;
            if (!wait_for_epoll(poll, events, event_count, deadline)) {
                std::cerr << "receiver progress=" << received_messages << '/'
                          << expected_messages << '\n';
                return false;
            }
            for (int event_index = 0; event_index < event_count;
                ++event_index) {
                const auto found = indices.find(events[event_index].fd);
                if (found == indices.end()) {
                    std::cerr << "epoll reported an unknown receiver socket\n";
                    return false;
                }
                runnable[found->second] = true;
            }
        }
        int round_messages = 0;
        std::size_t visited = 0;
        while (visited < sockets.size()
            && round_messages < maximum_round_messages) {
            const std::size_t index = cursor;
            cursor = (cursor + 1U) % sockets.size();
            ++visited;
            if (!runnable[index]) {
                continue;
            }
            int burst = 0;
            while (progress[index] < configuration.messages_per_connection
                && burst < maximum_burst_messages
                && round_messages < maximum_round_messages) {
                const int received = srt_recvmsg(
                    sockets[index], payload.data(), configuration.message_size);
                if (received == SRT_ERROR) {
                    int system_error = 0;
                    const int srt_error = srt_getlasterror(&system_error);
                    if (srt_error == SRT_EASYNCRCV) {
                        runnable[index] = false;
                        break;
                    }
                    std::cerr
                        << "message receive failed: socket_index=" << index
                        << " progress=" << progress[index]
                        << " state=" << srt_getsockstate(sockets[index])
                        << " srt_error=" << srt_error
                        << " system_error=" << system_error << ' '
                        << srt_getlasterror_str() << '\n';
                    return false;
                }
                if (received != configuration.message_size
                    || !validate_payload_body(payload)) {
                    std::cerr << "received payload failed validation\n";
                    return false;
                }
                const std::uint32_t logical_id = load_u32(payload.data() + 4);
                const std::uint64_t message = load_u64(payload.data() + 8);
                if (logical_id >= sockets.size()
                    || message != progress[index]) {
                    std::cerr
                        << "received payload sequence is invalid: socket_index="
                        << index
                        << " assigned_logical_id=" << logical_ids[index]
                        << " observed_logical_id=" << logical_id
                        << " expected_message=" << progress[index]
                        << " observed_message=" << message << '\n';
                    return false;
                }
                if (logical_ids[index] == unassigned) {
                    if (claimed_ids[logical_id]) {
                        std::cerr << "connection identity was duplicated\n";
                        return false;
                    }
                    logical_ids[index] = logical_id;
                    claimed_ids[logical_id] = true;
                } else if (logical_ids[index] != logical_id) {
                    std::cerr << "connection identity changed\n";
                    return false;
                }
                ++progress[index];
                ++received_messages;
                ++burst;
                ++round_messages;
            }
            if (progress[index] == configuration.messages_per_connection
                && !completed[index]) {
                runnable[index] = false;
                completion[index] =
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        Clock::now() - started)
                        .count();
                completed[index] = true;
                if (srt_epoll_remove_usock(poll.value(), sockets[index])
                    == SRT_ERROR) {
                    std::cerr << "receiver epoll removal failed: "
                              << srt_getlasterror_str() << '\n';
                    return false;
                }
            }
        }
        if (round_messages != 0 && received_messages < expected_messages) {
            std::this_thread::yield();
        }
    }

    if (!std::all_of(claimed_ids.begin(), claimed_ids.end(), [](bool value) {
            return value;
        })) {
        std::cerr << "not every logical connection was observed\n";
        return false;
    }
    const AggregateStatistics statistics = capture_statistics(sockets);
    print_complete("listener", configuration, Clock::now() - started,
        establishment, completion, statistics);
    if (configuration.shutdown_grace_milliseconds > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds {
            configuration.shutdown_grace_milliseconds});
    }
    return true;
}

int run_listener(const Configuration& configuration)
{
    const auto establishment_started = Clock::now();
    const SRTSOCKET listener = srt_create_socket();
    if (listener == SRT_INVALID_SOCK) {
        std::cerr << "listener socket creation failed: "
                  << srt_getlasterror_str() << '\n';
        return 3;
    }
    SocketSet listener_owner;
    listener_owner.add(listener);
    EndpointAddress endpoint;
    if (!make_endpoint(configuration.host, configuration.port, endpoint)
        || !configure_socket(listener, configuration, false)
        || srt_bind(listener, endpoint.data(), endpoint.size) == SRT_ERROR
        || srt_listen(listener, static_cast<int>(configuration.connections))
            == SRT_ERROR) {
        std::cerr << "listener setup failed: " << srt_getlasterror_str()
                  << '\n';
        return 3;
    }
    std::cout << "{\"event\":\"ready\",\"role\":\"listener\","
                 "\"port\":"
              << configuration.port << ",\"srt_version\":" << srt_getversion()
              << "}\n"
              << std::flush;

    SocketSet accepted;
    accepted.values().reserve(configuration.connections);
    for (std::uint32_t index = 0; index < configuration.connections; ++index) {
        sockaddr_storage peer {};
        int peer_size = static_cast<int>(sizeof(peer));
        const SRTSOCKET socket = srt_accept(
            listener, reinterpret_cast<sockaddr*>(&peer), &peer_size);
        if (socket == SRT_INVALID_SOCK || !make_nonblocking(socket)) {
            if (socket != SRT_INVALID_SOCK) {
                (void)srt_close(socket);
            }
            std::cerr << "accept failed: " << srt_getlasterror_str() << '\n';
            return 4;
        }
        accepted.add(socket);
    }
    const auto establishment = Clock::now() - establishment_started;
    std::cout << "{\"event\":\"established\",\"role\":\"listener\","
                 "\"connections\":"
              << configuration.connections << "}\n"
              << std::flush;
    return run_receiver(configuration, accepted.values(), establishment) ? 0
                                                                         : 5;
}

int run_caller(const Configuration& configuration)
{
    const auto establishment_started = Clock::now();
    EndpointAddress endpoint;
    if (!make_endpoint(configuration.host, configuration.port, endpoint)) {
        std::cerr << "caller endpoint is invalid\n";
        return 3;
    }
    SocketSet connected;
    connected.values().reserve(configuration.connections);
    for (std::uint32_t index = 0; index < configuration.connections; ++index) {
        const SRTSOCKET socket = srt_create_socket();
        if (socket == SRT_INVALID_SOCK
            || !configure_socket(socket, configuration, true)
            || srt_connect(socket, endpoint.data(), endpoint.size) == SRT_ERROR
            || !make_nonblocking(socket)) {
            if (socket != SRT_INVALID_SOCK) {
                (void)srt_close(socket);
            }
            std::cerr << "caller connection failed: " << srt_getlasterror_str()
                      << '\n';
            return 4;
        }
        connected.add(socket);
    }
    const auto establishment = Clock::now() - establishment_started;
    std::cout << "{\"event\":\"established\",\"role\":\"caller\","
                 "\"connections\":"
              << configuration.connections << "}\n"
              << std::flush;
    return run_sender(configuration, connected.values(), establishment) ? 0 : 5;
}

void usage()
{
    std::cerr << "usage: robotweax_srt_scalability_peer listener|caller"
                 " --host IP --port PORT --connections N --messages N"
                 " --message-size 16..1456 [--timeout-ms N] [--latency-ms N]"
                 " [--shutdown-grace-ms N]\n";
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
    const int result = configuration.role == Role::listener
        ? run_listener(configuration)
        : run_caller(configuration);
    if (srt_cleanup() == SRT_ERROR && result == 0) {
        std::cerr << "srt_cleanup failed\n";
        return 6;
    }
    return result;
}
