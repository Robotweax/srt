// Minimal black-box connection-group probe using only the public SRT API.
// It is linked once against Robotweax and once against pinned Haivision SRT.

#include "srt.h"

#if defined(ROBOTWEAX_SRT_REFERENCE_GROUP_RECEIVE)                             \
    && defined(ROBOTWEAX_SRT_COMPAT_SRT_H)
#error "Reference receive workaround must not be enabled for the Robotweax peer"
#endif

#if defined(_WIN32)
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#endif

#include <array>
#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

// Opt-in probe instrumentation, never a transport change. No payload, keys or
// SRT API calls here: preserve the API's thread-local error for failure reports.
void group_phase(const char* operation, const char* edge, std::size_t index = 0,
    int result = 0)
{
    static const bool enabled = [] {
        const char* value = std::getenv("ROBOTWEAX_SRT_GROUP_PHASE_TRACE");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    if (!enabled) {
        return;
    }
    const int saved_errno = errno;
    const auto monotonic =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    const auto unix_time =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    std::fprintf(stderr,
        "{\"event\":\"group_phase\",\"operation\":\"%s\",\"edge\":\"%s\","
        "\"index\":%zu,\"result\":%d,\"monotonic_us\":%lld,\"unix_us\":%lld}\n",
        operation, edge, index, result, static_cast<long long>(monotonic),
        static_cast<long long>(unix_time));
    std::fflush(stderr);
    errno = saved_errno;
}

constexpr std::int32_t pinned_srt_version =
    static_cast<std::int32_t>(SRT_VERSION_VALUE);

struct SocketHandle {
    SRTSOCKET value = SRT_INVALID_SOCK;
    ~SocketHandle()
    {
        if (value != SRT_INVALID_SOCK) {
            (void)srt_close(value);
        }
    }
};

struct EpollHandle {
    int value = -1;
    ~EpollHandle()
    {
        if (value >= 0) {
            (void)srt_epoll_release(value);
        }
    }
};

struct ConnectEvent {
    SRTSOCKET socket = SRT_INVALID_SOCK;
    int error_code = SRT_ERROR;
    int peer_family = AF_UNSPEC;
    int token = -1;
    SRT_SOCKSTATUS socket_state = SRTS_NONEXIST;
};

struct GroupConnectCallbacks {
    std::mutex mutex;
    std::vector<ConnectEvent> events;
};

struct ListenerMetadataObservation {
    std::mutex mutex;
    SRT_GROUP_TYPE expected_group_type = SRT_GTYPE_UNDEFINED;
    std::size_t calls = 0;
    bool valid = true;
};

bool read_integer_option(
    SRTSOCKET socket, SRT_SOCKOPT option, std::int32_t& value)
{
    int size = static_cast<int>(sizeof(value));
    return srt_getsockflag(socket, option, &value, &size) != SRT_ERROR
        && size == static_cast<int>(sizeof(value));
}

int observe_group_listener(
    void* opaque, SRTSOCKET socket, int,
    const sockaddr*, const char*)
{
    auto& observation =
        *static_cast<ListenerMetadataObservation*>(opaque);
    std::int32_t peer_version = 0;
    std::int32_t group_type = SRT_GTYPE_UNDEFINED;
    const std::int32_t probe = 0;
    const bool valid =
        read_integer_option(socket, SRTO_PEERVERSION, peer_version)
        // The provisional callback socket has not completed its handshake.
        // Pinned v1.5.5 therefore exposes zero until admission succeeds.
        && peer_version == 0
        && read_integer_option(socket, SRTO_GROUPTYPE, group_type)
        && group_type == static_cast<std::int32_t>(
            observation.expected_group_type)
        && srt_setsockflag(socket, SRTO_PEERVERSION,
               &probe, static_cast<int>(sizeof(probe))) == SRT_ERROR
        && srt_setsockflag(socket, SRTO_GROUPTYPE,
               &probe, static_cast<int>(sizeof(probe))) == SRT_ERROR;
    {
        std::lock_guard lock(observation.mutex);
        ++observation.calls;
        observation.valid = observation.valid && valid;
    }
    return valid ? 0 : SRT_ERROR;
}

void observe_group_connect(
    void* opaque, SRTSOCKET socket, int error_code,
    const sockaddr* peer_address, int token)
{
    auto& callbacks = *static_cast<GroupConnectCallbacks*>(opaque);
    ConnectEvent event;
    event.socket = socket;
    event.error_code = error_code;
    event.peer_family = peer_address == nullptr
        ? AF_UNSPEC : peer_address->sa_family;
    event.token = token;
    event.socket_state = srt_getsockstate(socket);
    std::lock_guard lock(callbacks.mutex);
    callbacks.events.push_back(event);
}

bool wait_for_group_callbacks(
    GroupConnectCallbacks& callbacks, std::size_t expected)
{
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard lock(callbacks.mutex);
            if (callbacks.events.size() >= expected) {
                return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    return false;
}

bool wait_for_listener_callbacks(
    ListenerMetadataObservation& observation, std::size_t expected)
{
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard lock(observation.mutex);
            if (observation.calls >= expected) {
                return observation.valid;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    return false;
}

bool callbacks_match(
    GroupConnectCallbacks& callbacks,
    const SRT_SOCKGROUPCONFIG* endpoints,
    std::size_t endpoint_count)
{
    std::lock_guard lock(callbacks.mutex);
    if (callbacks.events.size() != endpoint_count) {
        return false;
    }
    for (std::size_t index = 0; index < endpoint_count; ++index) {
        const auto& endpoint = endpoints[index];
        const auto matching = std::count_if(
            callbacks.events.begin(), callbacks.events.end(),
            [&endpoint](const ConnectEvent& event) {
                return event.socket == endpoint.id
                    && event.error_code == SRT_SUCCESS
                    && event.peer_family == AF_INET
                    && event.token == endpoint.token
                    && event.socket_state == SRTS_CONNECTED;
            });
        if (matching != 1) {
            return false;
        }
    }
    return true;
}

bool make_address(
    const std::string& host, std::uint16_t port,
    sockaddr_storage& storage, int& size)
{
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
        return false;
    }
    std::memcpy(&storage, &address, sizeof(address));
    size = static_cast<int>(sizeof(address));
    return true;
}

bool parse_port(const char* text, std::uint16_t& port)
{
    try {
        const unsigned long value = std::stoul(text);
        if (value == 0UL || value > 65'535UL) {
            return false;
        }
        port = static_cast<std::uint16_t>(value);
        return true;
    } catch (...) {
        return false;
    }
}

struct SecurityConfiguration {
    bool valid = true;
    bool enabled = false;
    const char* passphrase = nullptr;
    std::int32_t key_length = 16;
    std::int32_t crypto_mode = 2;
    std::int32_t key_refresh_rate = 4;
    std::int32_t key_preannouncement = 1;
};

SecurityConfiguration read_security_configuration()
{
    SecurityConfiguration configuration;
    const char* mode = std::getenv("ROBOTWEAX_SRT_GROUP_CRYPTO_MODE");
    if (mode == nullptr) {
        return configuration;
    }
#ifdef ENABLE_AEAD_API_PREVIEW
    configuration.enabled = std::strcmp(mode, "gcm") == 0;
    configuration.passphrase = std::getenv("ROBOTWEAX_SRT_GROUP_PASSPHRASE");
    configuration.valid = configuration.enabled
        && configuration.passphrase != nullptr
        && *configuration.passphrase != '\0';
#else
    configuration.valid = false;
#endif
    return configuration;
}

bool configure_security(
    SRTSOCKET socket, const SecurityConfiguration& configuration, bool sender)
{
    if (!configuration.enabled) {
        return true;
    }
#ifdef ENABLE_AEAD_API_PREVIEW
    const bool enforced = true;
    return srt_setsockflag(socket, SRTO_PASSPHRASE, configuration.passphrase,
               static_cast<int>(std::strlen(configuration.passphrase)))
        != SRT_ERROR
        && srt_setsockflag(socket, SRTO_PBKEYLEN, &configuration.key_length,
               static_cast<int>(sizeof(configuration.key_length)))
        != SRT_ERROR
        && srt_setsockflag(socket, SRTO_CRYPTOMODE, &configuration.crypto_mode,
               static_cast<int>(sizeof(configuration.crypto_mode)))
        != SRT_ERROR
        && srt_setsockflag(socket, SRTO_ENFORCEDENCRYPTION, &enforced,
               static_cast<int>(sizeof(enforced)))
        != SRT_ERROR
        && (!sender
            || (srt_setsockflag(socket, SRTO_KMREFRESHRATE,
                    &configuration.key_refresh_rate,
                    static_cast<int>(sizeof(configuration.key_refresh_rate)))
                    != SRT_ERROR
                && srt_setsockflag(socket, SRTO_KMPREANNOUNCE,
                       &configuration.key_preannouncement,
                       static_cast<int>(
                           sizeof(configuration.key_preannouncement)))
                    != SRT_ERROR));
#else
    (void)socket;
    (void)sender;
    return false;
#endif
}

bool members_match_security(SRTSOCKET group, std::size_t expected_count,
    const SecurityConfiguration& configuration)
{
    if (!configuration.enabled) {
        return true;
    }
#ifdef ENABLE_AEAD_API_PREVIEW
    std::vector<SRT_SOCKGROUPDATA> members(expected_count);
    std::size_t size = members.size();
    if (srt_group_data(group, members.data(), &size) == SRT_ERROR
        || size != expected_count) {
        return false;
    }
    return std::all_of(
        members.begin(), members.end(), [&](const SRT_SOCKGROUPDATA& member) {
            std::int32_t mode = -1;
            std::int32_t key_length = 0;
            return member.id != SRT_INVALID_SOCK
                && member.sockstate == SRTS_CONNECTED
                && read_integer_option(member.id, SRTO_CRYPTOMODE, mode)
                && read_integer_option(member.id, SRTO_PBKEYLEN, key_length)
                && mode == configuration.crypto_mode
                && key_length == configuration.key_length;
        });
#else
    (void)group;
    (void)expected_count;
    return false;
#endif
}

constexpr std::size_t baseline_message_count = 16;
constexpr std::size_t late_join_prefix = 4;
constexpr std::size_t peer_error_prefix = 1;
constexpr std::size_t initial_member_count = 2;
constexpr std::size_t late_join_member_count = 3;
constexpr std::size_t replacement_callback_count = 3;
constexpr std::chrono::milliseconds late_join_stability{100};
constexpr std::chrono::milliseconds path_outage_stability {100};
constexpr std::chrono::milliseconds path_outage_pacing {20};
constexpr int path_outage_send_timeout_milliseconds = 5'000;
constexpr int receive_contract_timeout_milliseconds = 150;

struct PathOutageSendObservation {
    std::size_t sent_messages = 0;
    SRTSOCKET first_active_member = SRT_INVALID_SOCK;
    SRTSOCKET final_active_member = SRT_INVALID_SOCK;
    std::size_t active_member_transitions = 0;
    int error_code = 0;
};

bool wait_for_members(
    SRTSOCKET group, std::size_t expected_count,
    const std::vector<SRTSOCKET>& expected_handles = {},
    std::chrono::milliseconds stable_for = std::chrono::milliseconds{0})
{
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds{5};
    auto stable_since = std::chrono::steady_clock::time_point{};
    while (std::chrono::steady_clock::now() < deadline) {
        bool ready = false;
        std::size_t size = 0;
        if (srt_group_data(group, nullptr, &size) != SRT_ERROR
            && size >= expected_count) {
            std::vector<SRT_SOCKGROUPDATA> data(size);
            std::size_t capacity = data.size();
            if (srt_group_data(group, data.data(), &capacity) != SRT_ERROR
                && capacity >= expected_count) {
                bool all_connected = true;
                for (const auto& member : data) {
                    all_connected = all_connected
                        && member.sockstate == SRTS_CONNECTED;
                }
                bool all_expected = true;
                for (const SRTSOCKET expected : expected_handles) {
                    bool found = false;
                    for (const auto& member : data) {
                        found = found || member.id == expected;
                    }
                    all_expected = all_expected && found;
                }
                ready = all_connected && all_expected;
            }
        }
        const auto now = std::chrono::steady_clock::now();
        if (ready) {
            if (stable_since == std::chrono::steady_clock::time_point{}) {
                stable_since = now;
            }
            if (now - stable_since >= stable_for) {
                return true;
            }
        } else {
            stable_since = std::chrono::steady_clock::time_point{};
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    return false;
}

bool is_terminal_member_state(SRT_SOCKSTATUS state)
{
    return state == SRTS_BROKEN
        || state == SRTS_CLOSING
        || state == SRTS_CLOSED
        || state == SRTS_NONEXIST;
}

bool replacement_members_are_ready(
    const std::vector<SRT_SOCKGROUPDATA>& members,
    SRTSOCKET healthy, SRTSOCKET failed, SRTSOCKET replacement)
{
    bool healthy_connected = false;
    bool replacement_connected = false;
    for (const auto& member : members) {
        if (member.id == healthy) {
            healthy_connected = member.sockstate == SRTS_CONNECTED;
        } else if (member.id == replacement) {
            replacement_connected = member.sockstate == SRTS_CONNECTED;
        } else if (member.id == failed) {
            if (!is_terminal_member_state(member.sockstate)) {
                return false;
            }
        } else if (!is_terminal_member_state(member.sockstate)) {
            return false;
        }
    }
    return healthy_connected && replacement_connected;
}

bool wait_for_replacement_members(
    SRTSOCKET group, SRTSOCKET healthy,
    SRTSOCKET failed, SRTSOCKET replacement,
    std::chrono::milliseconds stable_for)
{
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds{5};
    auto stable_since = std::chrono::steady_clock::time_point{};
    while (std::chrono::steady_clock::now() < deadline) {
        bool ready = false;
        std::size_t size = 0;
        if (srt_group_data(group, nullptr, &size) != SRT_ERROR
            && size >= initial_member_count) {
            std::vector<SRT_SOCKGROUPDATA> data(size);
            std::size_t capacity = data.size();
            if (srt_group_data(group, data.data(), &capacity) != SRT_ERROR
                && capacity <= data.size()) {
                data.resize(capacity);
                ready = replacement_members_are_ready(
                    data, healthy, failed, replacement);
            }
        }
        const auto now = std::chrono::steady_clock::now();
        if (ready) {
            if (stable_since == std::chrono::steady_clock::time_point{}) {
                stable_since = now;
            }
            if (now - stable_since >= stable_for) {
                return true;
            }
        } else {
            stable_since = std::chrono::steady_clock::time_point{};
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    return false;
}

std::vector<char> group_payload(std::size_t index)
{
    // Exercise both one MPEG-TS packet and the common seven-packet UDP
    // payload without making either size a library restriction.
    const std::size_t size = index % 2U == 0U ? 188U : 1'316U;
    std::vector<char> payload(size);
    for (std::size_t offset = 0; offset < size; ++offset) {
        payload[offset] = static_cast<char>(
            (index * 131U + offset * 17U + 29U) & 0xffU);
    }
    const std::uint32_t ordinal = static_cast<std::uint32_t>(index);
    std::memcpy(payload.data(), &ordinal, sizeof(ordinal));
    return payload;
}

std::uint64_t hash_bytes(
    std::uint64_t hash, const char* data, std::size_t size)
{
    constexpr std::uint64_t prime = 1'099'511'628'211ULL;
    for (std::size_t index = 0; index < size; ++index) {
        hash ^= static_cast<unsigned char>(data[index]);
        hash *= prime;
    }
    return hash;
}

std::uint64_t expected_payload_hash(
    std::size_t message_count = baseline_message_count)
{
    std::uint64_t hash = 14'695'981'039'346'656'037ULL;
    for (std::size_t index = 0; index < message_count; ++index) {
        const auto payload = group_payload(index);
        hash = hash_bytes(hash, payload.data(), payload.size());
    }
    return hash;
}

bool valid_group_member_states(
    const SRT_SOCKGROUPDATA* members, std::size_t size,
    std::size_t expected_members, SRT_GROUP_TYPE group_type)
{
    if (members == nullptr || size < expected_members) {
        return false;
    }
    if (group_type != SRT_GTYPE_BACKUP) {
        return true;
    }
    std::size_t running = 0;
    std::size_t idle = 0;
    for (std::size_t index = 0; index < expected_members; ++index) {
        running += members[index].memberstate == SRT_GST_RUNNING ? 1U : 0U;
        idle += members[index].memberstate == SRT_GST_IDLE ? 1U : 0U;
    }
    return running == 1U && idle + running == expected_members;
}

// Failure-only diagnostics: capture the operation's error before any snapshot
// API can replace the thread-local error. A successful API call followed by a
// contract mismatch must not be attributed to an old SRT error.
void report_group_transfer_failure(const char* operation, const char* reason,
    SRTSOCKET group, std::size_t message_index, int result,
    std::size_t expected_bytes, const SRT_MSGCTRL* control = nullptr,
    const SRT_SOCKGROUPDATA* supplied_members = nullptr,
    std::size_t supplied_capacity = 0)
{
    int system_error = 0;
    const int error =
        result == SRT_ERROR ? srt_getlasterror(&system_error) : SRT_SUCCESS;
    std::cerr << "{\"event\":\"group_transfer_failure\",\"operation\":\""
              << operation << "\",\"reason\":\"" << reason
              << "\",\"group\":" << group
              << ",\"message_index\":" << message_index
              << ",\"result\":" << result
              << ",\"expected_bytes\":" << expected_bytes
              << ",\"api_error\":" << error
              << ",\"system_error\":" << system_error;
    const auto emit_members = [](const SRT_SOCKGROUPDATA* members,
                                  std::size_t count) {
        std::cerr << '[';
        for (std::size_t index = 0; index < count; ++index) {
            if (index != 0) {
                std::cerr << ',';
            }
            const auto& member = members[index];
            std::cerr << "{\"id\":" << member.id
                      << ",\"sockstate\":" << member.sockstate
                      << ",\"memberstate\":" << member.memberstate
                      << ",\"result\":" << member.result
                      << ",\"token\":" << member.token
                      << ",\"weight\":" << member.weight << '}';
        }
        std::cerr << ']';
    };
    if (control != nullptr) {
        std::cerr << ",\"pktseq\":" << control->pktseq
                  << ",\"msgno\":" << control->msgno
                  << ",\"reported_members\":" << control->grpdata_size
                  << ",\"member_buffer_valid\":"
                  << (control->grpdata == supplied_members ? "true" : "false")
                  << ",\"operation_members\":";
        // Only read the caller-owned buffer, never an unexpected API pointer.
        emit_members(supplied_members,
            supplied_members == nullptr
                ? 0
                : std::min(control->grpdata_size, supplied_capacity));
    }
    std::array<SRT_SOCKGROUPDATA, 16> snapshot {};
    std::array<SRT_TRACEBSTATS, 16> statistics {};
    std::array<int, 16> statistics_results {};
    std::array<int, 16> statistics_errors {};
    statistics_results.fill(SRT_ERROR);
    std::size_t size = snapshot.size();
    int snapshot_result = SRT_ERROR;
    int snapshot_error = SRT_SUCCESS;
    bool snapshot_available = false;
    // Existing callers also report/classify their thread-local error. Query
    // on another thread so diagnostics cannot replace it. Failure path only.
    try {
        std::thread query([&] {
            snapshot_result = srt_group_data(group, snapshot.data(), &size);
            snapshot_error = snapshot_result == SRT_ERROR
                ? srt_getlasterror(nullptr)
                : SRT_SUCCESS;
            if (snapshot_result != SRT_ERROR) {
                for (std::size_t index = 0;
                    index < std::min(size, snapshot.size()); ++index) {
                    // Never clear counters or perform an additional receive.
                    statistics_results[index] = srt_bistats(
                        snapshot[index].id, &statistics[index], 0, 1);
                    statistics_errors[index] =
                        statistics_results[index] == SRT_ERROR
                        ? srt_getlasterror(nullptr)
                        : SRT_SUCCESS;
                }
            }
        });
        query.join();
        snapshot_available = true;
    } catch (...) {
        // Resource exhaustion must not obscure the original transfer failure.
    }
    std::cerr << ",\"snapshot_available\":"
              << (snapshot_available ? "true" : "false")
              << ",\"snapshot_result\":" << snapshot_result
              << ",\"snapshot_error\":" << snapshot_error
              << ",\"snapshot_reported_members\":" << size
              << ",\"snapshot_members\":";
    emit_members(snapshot.data(),
        snapshot_result == SRT_ERROR ? 0 : std::min(size, snapshot.size()));
    std::cerr << ",\"member_statistics\":[";
    const std::size_t statistics_count =
        snapshot_result == SRT_ERROR ? 0 : std::min(size, snapshot.size());
    for (std::size_t index = 0; index < statistics_count; ++index) {
        if (index != 0) {
            std::cerr << ',';
        }
        std::cerr << "{\"id\":" << snapshot[index].id
                  << ",\"result\":" << statistics_results[index]
                  << ",\"error\":" << statistics_errors[index];
        if (statistics_results[index] != SRT_ERROR) {
            const auto& stats = statistics[index];
            std::cerr << ",\"packets_sent\":" << stats.pktSentTotal
                      << ",\"packets_received\":" << stats.pktRecvTotal
                      << ",\"bytes_received\":" << stats.byteRecvTotal
                      << ",\"receive_loss\":" << stats.pktRcvLossTotal
                      << ",\"receive_drop\":" << stats.pktRcvDropTotal
                      << ",\"receive_undecrypt\":" << stats.pktRcvUndecryptTotal
                      << ",\"send_buffer_packets\":" << stats.pktSndBuf
                      << ",\"receive_buffer_packets\":" << stats.pktRcvBuf
                      << ",\"receive_tsbpd_ms\":" << stats.msRcvTsbPdDelay
                      << ",\"received_acks\":" << stats.pktRecvACKTotal
                      << ",\"sent_naks\":" << stats.pktSentNAKTotal;
        }
        std::cerr << '}';
    }
    std::cerr << ']';
    std::cerr << "}\n";
}

#if defined(ROBOTWEAX_SRT_REFERENCE_GROUP_RECEIVE)
// Haivision 1.5.7's blocking group receive can remain in its internal
// read-readiness wait with payload already received, until peer shutdown.
// Use the public nonblocking API for payload verification only. Keep the
// dedicated blocking receive-contract probes independent of this workaround.
class ReferencePayloadReceiveMode {
public:
    explicit ReferencePayloadReceiveMode(SRTSOCKET group)
        : group_(group)
    {
    }
    bool enable()
    {
        int size = static_cast<int>(sizeof(original_));
        if (srt_getsockopt(group_, 0, SRTO_RCVSYN, &original_, &size)
            == SRT_ERROR) {
            return false;
        }
        const bool asynchronous = false;
        active_ = srt_setsockopt(group_, 0, SRTO_RCVSYN, &asynchronous,
                      static_cast<int>(sizeof(asynchronous)))
            != SRT_ERROR;
        return active_;
    }
    ~ReferencePayloadReceiveMode()
    {
        if (active_) {
            (void)srt_setsockopt(group_, 0, SRTO_RCVSYN, &original_,
                static_cast<int>(sizeof(original_)));
        }
    }

private:
    SRTSOCKET group_;
    bool original_ = true;
    bool active_ = false;
};
#endif

bool receive_group_range(SRTSOCKET group, std::size_t first, std::size_t last,
    std::size_t expected_members, SRT_GROUP_TYPE group_type,
    std::uint64_t& hash)
{
    constexpr int timeout_milliseconds = 5'000;
    if (srt_setsockopt(group, 0, SRTO_RCVTIMEO, &timeout_milliseconds,
            static_cast<int>(sizeof(timeout_milliseconds)))
        == SRT_ERROR) {
        report_group_transfer_failure(
            "receive-range/set-timeout", "api", group, first, SRT_ERROR, 0);
        return false;
    }
    std::array<char, 1'500> buffer {};
#if defined(ROBOTWEAX_SRT_REFERENCE_GROUP_RECEIVE)
    ReferencePayloadReceiveMode receive_mode(group);
    if (!receive_mode.enable()) {
        return false;
    }
#endif
    for (std::size_t index = first; index < last; ++index) {
        std::array<SRT_SOCKGROUPDATA, 4> members {};
        SRT_MSGCTRL control = srt_msgctrl_default;
        control.grpdata = members.data();
        control.grpdata_size = members.size();
        group_phase("receive-payload", "begin", index);
        int received = srt_recvmsg2(
            group, buffer.data(), static_cast<int>(buffer.size()), &control);
#if defined(ROBOTWEAX_SRT_REFERENCE_GROUP_RECEIVE)
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(timeout_milliseconds);
        while (received == SRT_ERROR
            && srt_getlasterror(nullptr) == SRT_EASYNCRCV
            && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            control = srt_msgctrl_default;
            control.grpdata = members.data();
            control.grpdata_size = members.size();
            received = srt_recvmsg2(group, buffer.data(),
                static_cast<int>(buffer.size()), &control);
        }
#endif
        group_phase("receive-payload", "end", index, received);
        const auto expected = group_payload(index);
        if (received != static_cast<int>(expected.size())
            || !std::equal(expected.begin(), expected.end(), buffer.begin())
            || control.pktseq < 0 || control.msgno <= 0
            || control.grpdata == nullptr
            || !valid_group_member_states(control.grpdata, control.grpdata_size,
                expected_members, group_type)) {
            report_group_transfer_failure("receive-range",
                received == SRT_ERROR                               ? "api"
                    : received != static_cast<int>(expected.size()) ? "size"
                    : !std::equal(
                          expected.begin(), expected.end(), buffer.begin())
                    ? "payload"
                    : "message-or-member-metadata",
                group, index, received, expected.size(), &control,
                members.data(), members.size());
            return false;
        }
        hash = hash_bytes(hash, buffer.data(),
            static_cast<std::size_t>(received));
    }
    return true;
}

bool group_has_connected_member(SRTSOCKET group)
{
    std::size_t size = 0;
    if (srt_group_data(group, nullptr, &size) == SRT_ERROR || size == 0) {
        return false;
    }
    std::vector<SRT_SOCKGROUPDATA> members(size);
    std::size_t capacity = members.size();
    if (srt_group_data(group, members.data(), &capacity) == SRT_ERROR
        || capacity > members.size()) {
        return false;
    }
    for (std::size_t index = 0; index < capacity; ++index) {
        if (members[index].sockstate == SRTS_CONNECTED) {
            return true;
        }
    }
    return false;
}

bool receive_group_range_after_member_failure(
    SRTSOCKET group, std::size_t first, std::size_t last,
    std::uint64_t& hash, std::size_t& transient_receive_errors)
{
    constexpr int timeout_milliseconds = 5'000;
    if (srt_setsockopt(group, 0, SRTO_RCVTIMEO,
            &timeout_milliseconds,
            static_cast<int>(sizeof(timeout_milliseconds))) == SRT_ERROR) {
        return false;
    }
    std::array<char, 1'500> buffer{};
    for (std::size_t index = first; index < last; ++index) {
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds{timeout_milliseconds};
        SRT_MSGCTRL control = srt_msgctrl_default;
        int received = SRT_ERROR;
        while (true) {
            control = srt_msgctrl_default;
            received = srt_recvmsg2(group, buffer.data(),
                static_cast<int>(buffer.size()), &control);
            if (received != SRT_ERROR) {
                break;
            }
            const int receive_error = srt_getlasterror(nullptr);
            if (receive_error != SRT_ECONNLOST
                || std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            const bool connected_member = group_has_connected_member(group);
            if (!connected_member) {
                return false;
            }
            // Pinned Haivision v1.5.5 can surface the removed member's
            // terminal state through group receive even though another path
            // remains connected. Retry only while the public group snapshot
            // still proves aggregate availability. A latched terminal group
            // state is classified explicitly by the black-box driver.
            ++transient_receive_errors;
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        const auto expected = group_payload(index);
        if (received != static_cast<int>(expected.size())
            || !std::equal(expected.begin(), expected.end(), buffer.begin())
            || control.pktseq < 0 || control.msgno <= 0) {
            return false;
        }
        hash = hash_bytes(hash, buffer.data(),
            static_cast<std::size_t>(received));
    }
    return true;
}

bool send_group_range(
    SRTSOCKET group, std::size_t first, std::size_t last,
    std::size_t expected_members, SRT_GROUP_TYPE group_type)
{
    constexpr int timeout_milliseconds = 5'000;
    if (srt_setsockopt(group, 0, SRTO_SNDTIMEO,
            &timeout_milliseconds,
            static_cast<int>(sizeof(timeout_milliseconds))) == SRT_ERROR) {
        report_group_transfer_failure(
            "send-range/set-timeout", "api", group, first, SRT_ERROR, 0);
        return false;
    }
    for (std::size_t index = first; index < last; ++index) {
        const auto payload = group_payload(index);
        std::array<SRT_SOCKGROUPDATA, 4> members{};
        SRT_MSGCTRL control = srt_msgctrl_default;
        control.grpdata = members.data();
        control.grpdata_size = members.size();
        const int sent = srt_sendmsg2(
            group, payload.data(), static_cast<int>(payload.size()), &control);
        if (sent != static_cast<int>(payload.size()) || control.pktseq < 0
            || control.msgno <= 0 || control.grpdata == nullptr
            || !valid_group_member_states(control.grpdata, control.grpdata_size,
                expected_members, group_type)) {
            report_group_transfer_failure("send-range",
                sent == SRT_ERROR ? "api"
                    : sent != static_cast<int>(payload.size())
                    ? "size"
                    : "message-or-member-metadata",
                group, index, sent, payload.size(), &control, members.data(),
                members.size());
            return false;
        }
    }
    return true;
}

SRTSOCKET path_outage_running_member(
    const SRT_SOCKGROUPDATA* members, std::size_t size)
{
    SRTSOCKET running = SRT_INVALID_SOCK;
    for (std::size_t index = 0; index < size; ++index) {
        if (members[index].memberstate != SRT_GST_RUNNING) {
            continue;
        }
        if (running != SRT_INVALID_SOCK
            || members[index].id == SRT_INVALID_SOCK) {
            return SRT_INVALID_SOCK;
        }
        running = members[index].id;
    }
    return running;
}

PathOutageSendObservation send_path_outage_range(SRTSOCKET group)
{
    PathOutageSendObservation observation;
    const auto started = std::chrono::steady_clock::now();
    for (std::size_t index = 0; index < baseline_message_count; ++index) {
        std::this_thread::sleep_until(
            started + path_outage_pacing * static_cast<int>(index));
        const auto payload = group_payload(index);
        std::array<SRT_SOCKGROUPDATA, 4> members {};
        SRT_MSGCTRL control = srt_msgctrl_default;
        control.grpdata = members.data();
        control.grpdata_size = members.size();
        const int sent = srt_sendmsg2(
            group, payload.data(), static_cast<int>(payload.size()), &control);
        if (sent != static_cast<int>(payload.size()) || control.pktseq < 0
            || control.msgno <= 0) {
            observation.error_code = srt_getlasterror(nullptr);
            break;
        }
        ++observation.sent_messages;
        const SRTSOCKET active =
            control.grpdata == nullptr || control.grpdata_size > members.size()
            ? SRT_INVALID_SOCK
            : path_outage_running_member(control.grpdata, control.grpdata_size);
        if (observation.first_active_member == SRT_INVALID_SOCK) {
            observation.first_active_member = active;
        } else if (active != observation.final_active_member) {
            ++observation.active_member_transitions;
        }
        observation.final_active_member = active;
    }
    return observation;
}

bool receive_path_outage_range(
    SRTSOCKET group, std::size_t& received_messages, int& error_code)
{
    constexpr int timeout_milliseconds = 5'000;
    if (srt_setsockopt(group, 0, SRTO_RCVTIMEO, &timeout_milliseconds,
            static_cast<int>(sizeof(timeout_milliseconds)))
        == SRT_ERROR) {
        error_code = srt_getlasterror(nullptr);
        return false;
    }
    std::array<char, 1'500> buffer {};
    for (std::size_t index = 0; index < baseline_message_count; ++index) {
        SRT_MSGCTRL control = srt_msgctrl_default;
        const int received = srt_recvmsg2(
            group, buffer.data(), static_cast<int>(buffer.size()), &control);
        if (received == SRT_ERROR) {
            error_code = srt_getlasterror(nullptr);
            return false;
        }
        const auto expected = group_payload(index);
        if (received != static_cast<int>(expected.size())
            || !std::equal(expected.begin(), expected.end(), buffer.begin())
            || control.pktseq < 0 || control.msgno <= 0) {
            return false;
        }
        ++received_messages;
    }
    return true;
}

void emit_path_outage_sender_result(const char* event, SRTSOCKET group,
    SRTSOCKET primary, SRTSOCKET backup,
    const PathOutageSendObservation& observation)
{
    std::cout << "{\"event\":\"" << event
              << "\","
                 "\"role\":\"caller\",\"sent_messages\":"
              << observation.sent_messages
              << ",\"first_active_member\":" << observation.first_active_member
              << ",\"final_active_member\":" << observation.final_active_member
              << ",\"active_member_transitions\":"
              << observation.active_member_transitions
              << ",\"group_state\":" << srt_getsockstate(group)
              << ",\"primary_state\":" << srt_getsockstate(primary)
              << ",\"backup_state\":" << srt_getsockstate(backup)
              << ",\"error_code\":" << observation.error_code
              << ",\"srt_version\":" << srt_getversion() << "}\n"
              << std::flush;
}

bool send_group_range_after_member_failure(
    SRTSOCKET group, std::size_t first, std::size_t last)
{
    constexpr int timeout_milliseconds = 5'000;
    if (srt_setsockopt(group, 0, SRTO_SNDTIMEO, &timeout_milliseconds,
            static_cast<int>(sizeof(timeout_milliseconds)))
        == SRT_ERROR) {
        return false;
    }
    for (std::size_t index = first; index < last; ++index) {
        const auto payload = group_payload(index);
        SRT_MSGCTRL control = srt_msgctrl_default;
        if (srt_sendmsg2(group, payload.data(),
                static_cast<int>(payload.size()), &control)
                != static_cast<int>(payload.size())
            || control.pktseq < 0 || control.msgno <= 0
            || srt_getsockstate(group) != SRTS_CONNECTED) {
            return false;
        }
    }
    return true;
}

bool member_is_isolated(SRTSOCKET failed, SRTSOCKET healthy)
{
    const SRT_SOCKSTATUS failed_state = srt_getsockstate(failed);
    return (failed_state == SRTS_BROKEN
               || failed_state == SRTS_CLOSING
               || failed_state == SRTS_CLOSED
               || failed_state == SRTS_NONEXIST)
        && srt_getsockstate(healthy) == SRTS_CONNECTED;
}

bool send_hash_reply(SRTSOCKET group, std::uint64_t hash)
{
    std::array<char, sizeof(hash)> bytes{};
    std::memcpy(bytes.data(), &hash, sizeof(hash));
    group_phase("send-hash-reply", "begin");
    const int sent =
        srt_send(group, bytes.data(), static_cast<int>(bytes.size()));
    group_phase("send-hash-reply", "end", 0, sent);
    if (sent != static_cast<int>(bytes.size())) {
        report_group_transfer_failure("send-hash-reply",
            sent == SRT_ERROR ? "api" : "size", group, 0, sent, bytes.size());
        return false;
    }
    return true;
}

bool receive_hash_reply(SRTSOCKET group, std::uint64_t expected)
{
    constexpr int timeout_milliseconds = 5'000;
    (void)srt_setsockopt(group, 0, SRTO_RCVTIMEO,
        &timeout_milliseconds,
        static_cast<int>(sizeof(timeout_milliseconds)));
    // Haivision v1.5.5 validates a group receive buffer against the maximum
    // configured Live payload, not merely the next message's actual size.
    std::array<char, 1'500> bytes{};
    group_phase("receive-hash-reply", "begin");
    const int result =
        srt_recv(group, bytes.data(), static_cast<int>(bytes.size()));
    group_phase("receive-hash-reply", "end", 0, result);
    if (result != static_cast<int>(sizeof(expected))) {
        report_group_transfer_failure("receive-hash-reply",
            result == SRT_ERROR ? "api" : "size", group, 0, result,
            sizeof(expected));
        return false;
    }
    std::uint64_t received = 0;
    std::memcpy(&received, bytes.data(), sizeof(received));
    if (received != expected) {
        report_group_transfer_failure(
            "receive-hash-reply", "hash", group, 0, result, sizeof(expected));
        return false;
    }
    return true;
}

bool probe_group_receive_contract(SRTSOCKET group, bool reference_timeout_error)
{
    std::array<char, 1'500> bytes {};
    const bool asynchronous = false;
    const bool synchronous = true;
    const auto nonblocking_started = std::chrono::steady_clock::now();
    const bool nonblocking_option =
        srt_setsockopt(group, 0, SRTO_RCVSYN, &asynchronous,
            static_cast<int>(sizeof(asynchronous)))
        != SRT_ERROR;
    const int nonblocking_result = nonblocking_option
        ? srt_recvmsg(group, bytes.data(), static_cast<int>(bytes.size()))
        : SRT_ERROR;
    const int nonblocking_error = nonblocking_result == SRT_ERROR
        ? srt_getlasterror(nullptr)
        : SRT_SUCCESS;
    const auto nonblocking_elapsed =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - nonblocking_started);

    const bool blocking_options =
        srt_setsockopt(group, 0, SRTO_RCVSYN, &synchronous,
            static_cast<int>(sizeof(synchronous)))
            != SRT_ERROR
        && srt_setsockopt(group, 0, SRTO_RCVTIMEO,
               &receive_contract_timeout_milliseconds,
               static_cast<int>(sizeof(receive_contract_timeout_milliseconds)))
            != SRT_ERROR;
    const auto timeout_started = std::chrono::steady_clock::now();
    const int timeout_result = blocking_options
        ? srt_recvmsg(group, bytes.data(), static_cast<int>(bytes.size()))
        : SRT_ERROR;
    const int timeout_error =
        timeout_result == SRT_ERROR ? srt_getlasterror(nullptr) : SRT_SUCCESS;
    const auto timeout_elapsed =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - timeout_started);

    constexpr int payload_timeout_milliseconds = 5'000;
    const bool restored =
        srt_setsockopt(group, 0, SRTO_RCVTIMEO, &payload_timeout_milliseconds,
            static_cast<int>(sizeof(payload_timeout_milliseconds)))
        != SRT_ERROR;
    const int expected_timeout_error =
        reference_timeout_error ? SRT_EASYNCRCV : SRT_ETIMEOUT;
    const bool matched = nonblocking_option && blocking_options && restored
        && nonblocking_result == SRT_ERROR && nonblocking_error == SRT_EASYNCRCV
        && nonblocking_elapsed < std::chrono::milliseconds {500}
        && timeout_result == SRT_ERROR
        && timeout_error == expected_timeout_error
        && timeout_elapsed >= std::chrono::milliseconds {50}
        && timeout_elapsed < std::chrono::seconds {5};
    std::cout << "{\"event\":\"receive_contract\","
                 "\"role\":\"listener\",\"matched\":"
              << (matched ? "true" : "false") << ",\"profile\":\""
              << (reference_timeout_error ? "reference" : "strict") << "\""
              << ",\"nonblocking_result\":" << nonblocking_result
              << ",\"nonblocking_error\":" << nonblocking_error
              << ",\"nonblocking_elapsed_us\":" << nonblocking_elapsed.count()
              << ",\"timeout_result\":" << timeout_result
              << ",\"timeout_error\":" << timeout_error
              << ",\"timeout_elapsed_us\":" << timeout_elapsed.count()
              << ",\"timeout_configured_ms\":"
              << receive_contract_timeout_milliseconds << "}\n"
              << std::flush;
    return matched;
}

bool drain_group_updates(int poll)
{
    for (std::size_t attempt = 0; attempt < 16U; ++attempt) {
        std::array<SRT_EPOLL_EVENT, 2> events{};
        const int count = srt_epoll_uwait(
            poll, events.data(), static_cast<int>(events.size()), 0);
        if (count == SRT_ERROR) {
            return false;
        }
        if (count == 0) {
            return true;
        }
    }
    return false;
}

bool wait_for_group_update(int poll, SRTSOCKET group)
{
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < deadline) {
        std::array<SRT_EPOLL_EVENT, 2> events{};
        const int count = srt_epoll_uwait(
            poll, events.data(), static_cast<int>(events.size()), 100);
        if (count == SRT_ERROR) {
            return false;
        }
        bool observed = false;
        for (int index = 0; index < count; ++index) {
            const auto& event = events[static_cast<std::size_t>(index)];
            if (event.fd == group
                && (event.events & SRT_EPOLL_UPDATE) != 0) {
                if (observed) {
                    return false;
                }
                observed = true;
            }
        }
        if (observed) {
            return srt_epoll_uwait(
                       poll, events.data(),
                       static_cast<int>(events.size()), 0)
                == 0;
        }
    }
    return false;
}

bool group_update_is_absent(int poll)
{
    std::array<SRT_EPOLL_EVENT, 2> events{};
    return srt_epoll_uwait(
               poll, events.data(), static_cast<int>(events.size()), 0)
        == 0;
}

bool configure_listener(SocketHandle& listener, const sockaddr_storage& address,
    int address_size, ListenerMetadataObservation& metadata,
    const SecurityConfiguration& security)
{
    // Pinned Haivision v1.5.5 requires the four-byte representation here,
    // despite documenting SRTO_GROUPCONNECT as a boolean option.
    const int allow_groups = 1;
    return listener.value != SRT_INVALID_SOCK
        && configure_security(listener.value, security, false)
        && srt_setsockopt(listener.value, 0, SRTO_GROUPCONNECT, &allow_groups,
               static_cast<int>(sizeof(allow_groups)))
        != SRT_ERROR
        && srt_listen_callback(
               listener.value, observe_group_listener, &metadata)
        != SRT_ERROR
        && srt_bind(listener.value, reinterpret_cast<const sockaddr*>(&address),
               address_size)
        != SRT_ERROR
        && srt_listen(listener.value, 4) != SRT_ERROR;
}

int run_listener(const sockaddr_storage& address, int address_size,
    SRT_GROUP_TYPE group_type, bool late_join, bool peer_error,
    bool replacement, bool path_outage, bool receive_contract,
    bool reference_receive_contract, const SecurityConfiguration& security,
    const sockaddr_storage* bonded_address = nullptr,
    int bonded_address_size = 0)
{
    ListenerMetadataObservation listener_metadata;
    listener_metadata.expected_group_type = group_type;
    SocketHandle listener {srt_create_socket()};
    SocketHandle bonded_listener {
        bonded_address == nullptr ? SRT_INVALID_SOCK : srt_create_socket()};
    EpollHandle poll{srt_epoll_create()};
    if (poll.value < 0
        || !configure_listener(
            listener, address, address_size, listener_metadata, security)
        || (bonded_address != nullptr
            && !configure_listener(bonded_listener, *bonded_address,
                bonded_address_size, listener_metadata, security))) {
        std::cerr << "listener setup failed: "
                  << srt_getlasterror_str() << '\n';
        return 3;
    }
    const int watched = SRT_EPOLL_IN | SRT_EPOLL_UPDATE;
    if (srt_epoll_add_usock(
            poll.value, listener.value, &watched) == SRT_ERROR
        || (bonded_address != nullptr
            && srt_epoll_add_usock(
                poll.value, bonded_listener.value, &watched)
                == SRT_ERROR)) {
        std::cerr << "listener epoll setup failed: "
                  << srt_getlasterror_str() << '\n';
        return 3;
    }
    const std::array<SRTSOCKET, 2> bonded_listeners{
        listener.value, bonded_listener.value};
    if (bonded_address != nullptr
        && srt_accept_bond(bonded_listeners.data(),
               static_cast<int>(bonded_listeners.size()), 0)
            != SRT_INVALID_SOCK) {
        std::cerr << "empty listener bond unexpectedly accepted a socket\n";
        return 3;
    }
    std::cout << "{\"event\":\"ready\"}\n" << std::flush;

    bool listener_update = false;
    SocketHandle group;
    if (bonded_address != nullptr) {
        group_phase("accept-bond", "begin");
        group.value = srt_accept_bond(
            bonded_listeners.data(),
            static_cast<int>(bonded_listeners.size()), 5'000);
        group_phase("accept-bond", "end", 0, group.value);
    } else {
        group_phase("accept-ready", "begin");
        bool accept_ready = false;
        const auto event_deadline = std::chrono::steady_clock::now()
            + std::chrono::seconds{5};
        while (!accept_ready
            && std::chrono::steady_clock::now() < event_deadline) {
            std::array<SRT_EPOLL_EVENT, 2> events{};
            const int count = srt_epoll_uwait(
                poll.value, events.data(),
                static_cast<int>(events.size()), 500);
            if (count == SRT_ERROR) {
                std::cerr << "listener epoll wait failed: "
                          << srt_getlasterror_str() << '\n';
                return 4;
            }
            for (int index = 0; index < count; ++index) {
                if (events[static_cast<std::size_t>(index)].fd
                    != listener.value) {
                    continue;
                }
                accept_ready = accept_ready
                    || (events[static_cast<std::size_t>(index)].events
                        & SRT_EPOLL_IN) != 0;
                listener_update = listener_update
                    || (events[static_cast<std::size_t>(index)].events
                        & SRT_EPOLL_UPDATE) != 0;
            }
        }
        if (!accept_ready) {
            std::cerr << "listener did not receive group accept readiness\n";
            return 4;
        }
        group_phase("accept-ready", "end");
        group_phase("accept", "begin");
        group.value = srt_accept(listener.value, nullptr, nullptr);
        group_phase("accept", "end", 0, group.value);
    }
    if (group.value == SRT_INVALID_SOCK
        || (group.value & SRTGROUP_MASK) == 0) {
        std::cerr << "group accept failed: "
                  << srt_getlasterror_str() << '\n';
        return 4;
    }
    std::uint64_t payload_hash = 14'695'981'039'346'656'037ULL;
    bool payload_valid = true;
    bool replacement_attached = false;
    std::size_t transient_group_receive_errors = 0;
    bool initial_listener_update = listener_update;
    if (group_type == SRT_GTYPE_BROADCAST && late_join) {
        // Pinned Haivision v1.5.5 can publish CONNECTED before a newly
        // accepted mirror group is ready for payload I/O. Require both initial
        // members to remain connected for a bounded continuous interval. This
        // observes real state transitions instead of hiding them with a sleep
        // or retrying a failed transfer.
        if (!wait_for_members(
                group.value, initial_member_count, {}, late_join_stability)
            || !members_match_security(
                group.value, initial_member_count, security)) {
            std::cerr
                << "accepted group did not retain stable initial members\n";
            return 5;
        }
        payload_valid = receive_group_range(
                group.value, 0U, late_join_prefix,
                initial_member_count, group_type, payload_hash)
            && payload_hash
                == expected_payload_hash(late_join_prefix);
        if (!payload_valid) {
            std::cerr << "late-join prefix receive failed: "
                      << srt_getlasterror_str() << '\n';
            return 5;
        }

        // Record and consume the initial membership UPDATE edge, then
        // acknowledge the prefix only when the Listener is ready for the
        // genuinely late third member. Pinned Haivision v1.5.5 leaves the
        // edge's permanent state bit set after consumption and consequently
        // cannot re-arm a second UPDATE for the third member. Keep the two
        // observations separate so the harness can require Robotweax's
        // versioned repeated edge without blocking reference payload I/O.
        listener_update = false;
        while (true) {
            std::array<SRT_EPOLL_EVENT, 2> events{};
            const int count = srt_epoll_uwait(
                poll.value, events.data(),
                static_cast<int>(events.size()), 0);
            if (count == SRT_ERROR) {
                std::cerr << "initial listener update drain failed: "
                          << srt_getlasterror_str() << '\n';
                return 5;
            }
            if (count == 0) {
                break;
            }
            for (int index = 0; index < count; ++index) {
                const auto& event =
                    events[static_cast<std::size_t>(index)];
                initial_listener_update = initial_listener_update
                    || ((event.fd == listener.value
                            || event.fd == bonded_listener.value)
                        && (event.events & SRT_EPOLL_UPDATE) != 0);
            }
        }
        if (!send_hash_reply(group.value, payload_hash)) {
            std::cerr << "late-join prefix reply failed: "
                      << srt_getlasterror_str() << '\n';
            return 5;
        }
    } else if (group_type == SRT_GTYPE_BROADCAST && peer_error) {
        if (!wait_for_members(
                group.value, initial_member_count, {},
                late_join_stability)) {
            std::cerr << "peer-error receiver members did not stabilize\n";
            return 5;
        }
        std::cout
            << "{\"event\":\"peer_error_receiver_ready\","
               "\"role\":\"listener\"}\n"
            << std::flush;
        if (!receive_group_range(
                group.value, 0U, peer_error_prefix,
                initial_member_count, group_type, payload_hash)
            || payload_hash != expected_payload_hash(peer_error_prefix)
            || !send_hash_reply(group.value, payload_hash)) {
            std::cerr << "peer-error prefix receive/reply failed: "
                      << srt_getlasterror_str() << '\n';
            return 5;
        }
    }

    const std::size_t expected_members = late_join
        ? late_join_member_count
        : initial_member_count;
    group_phase("member-readiness", "begin");
    if (!wait_for_members(group.value, expected_members, {},
            path_outage ? path_outage_stability : std::chrono::milliseconds {0})
        || !members_match_security(group.value, expected_members, security)) {
        std::cerr << "accepted group did not acquire all expected members\n";
        return 5;
    }
    group_phase("member-readiness", "end");
    if (receive_contract
        && !probe_group_receive_contract(
            group.value, reference_receive_contract)) {
        std::cerr << "group receive contract probe failed: "
                  << srt_getlasterror_str() << '\n';
        return 5;
    }
    if (path_outage) {
        std::cout << "{\"event\":\"path_outage_receiver_ready\","
                     "\"role\":\"listener\",\"srt_version\":"
                  << srt_getversion() << "}\n"
                  << std::flush;
        std::size_t received_messages = 0;
        int receive_error = 0;
        payload_valid = receive_path_outage_range(
            group.value, received_messages, receive_error);
        if (!payload_valid) {
            std::cout << "{\"event\":\"path_outage_receiver_failure\","
                         "\"role\":\"listener\",\"received_messages\":"
                      << received_messages
                      << ",\"group_state\":" << srt_getsockstate(group.value)
                      << ",\"error_code\":" << receive_error
                      << ",\"srt_version\":" << srt_getversion() << "}\n"
                      << std::flush;
            std::cerr << "path-outage receive failed after "
                      << received_messages
                      << " messages: " << srt_getlasterror_str() << '\n';
            return 6;
        }
        payload_hash = expected_payload_hash();
        std::cout << "{\"event\":\"path_outage_receiver_complete\","
                     "\"role\":\"listener\",\"received_messages\":"
                  << received_messages
                  << ",\"group_state\":" << srt_getsockstate(group.value)
                  << ",\"srt_version\":" << srt_getversion() << "}\n"
                  << std::flush;
        // This profile measures one-way payload survival. Requiring a group
        // reply would route the verdict through the deliberately black-holed
        // primary path and conflate forward failover with reverse selection.
        listener_update = true;
    }
    const auto update_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds {2};
    while (!listener_update
        && std::chrono::steady_clock::now() < update_deadline) {
        std::array<SRT_EPOLL_EVENT, 2> events {};
        const int count = srt_epoll_uwait(
            poll.value, events.data(), static_cast<int>(events.size()), 250);
        if (count == SRT_ERROR) {
            std::cerr << "listener update wait failed: "
                      << srt_getlasterror_str() << '\n';
            return 5;
        }
        for (int index = 0; index < count; ++index) {
            const SRTSOCKET event_socket =
                events[static_cast<std::size_t>(index)].fd;
            listener_update = listener_update
                || ((event_socket == listener.value
                        || event_socket == bonded_listener.value)
                    && (events[static_cast<std::size_t>(index)].events
                        & SRT_EPOLL_UPDATE) != 0);
        }
    }
    if (!listener_update && !late_join) {
        std::cerr << "listener did not receive later-member update\n";
        return 5;
    }
    if (!listener_update && late_join) {
        std::cerr
            << "listener did not re-arm later-member update; "
               "continuing with membership and payload validation\n";
    }
    const bool baseline_payload =
        !late_join && !peer_error && !path_outage && !receive_contract;
    if (baseline_payload) {
        std::cout << "{\"event\":\"payload_receiver_ready\","
                     "\"role\":\"listener\"}\n"
                  << std::flush;
    }
    if (peer_error) {
        if (replacement) {
            payload_valid = receive_group_range_after_member_failure(
                group.value, peer_error_prefix,
                peer_error_prefix + 1U, payload_hash,
                transient_group_receive_errors);
            replacement_attached = payload_valid
                && wait_for_listener_callbacks(
                    listener_metadata, replacement_callback_count)
                && wait_for_members(
                    group.value, initial_member_count, {},
                    late_join_stability);
            if (replacement_attached) {
                std::cout
                    << "{\"event\":\"peer_error_replacement_ready\","
                       "\"role\":\"listener\"}\n"
                    << std::flush;
                // Keep the pinned listener alive while the Robotweax caller
                // validates its distinct member and stable membership. A
                // receive attempted in that interval can expose the old
                // member's broken state and close the reference process before
                // the restored group is ready to transfer the suffix.
                std::string replacement_release;
                if (!std::getline(std::cin, replacement_release)
                    || replacement_release != "resume") {
                    std::cerr
                        << "peer-error replacement listener barrier "
                           "was not released\n";
                    return 6;
                }
            }
            payload_valid = payload_valid && replacement_attached
                && receive_group_range_after_member_failure(
                    group.value, peer_error_prefix + 1U,
                    baseline_message_count, payload_hash,
                    transient_group_receive_errors);
        } else {
            payload_valid = receive_group_range_after_member_failure(
                group.value, peer_error_prefix,
                baseline_message_count, payload_hash,
                transient_group_receive_errors);
        }
        payload_valid = payload_valid
            && payload_hash == expected_payload_hash()
            && send_hash_reply(group.value, payload_hash);
        if (!payload_valid) {
            std::cerr << "peer-error surviving-path receive/reply failed: "
                      << srt_getlasterror_str() << '\n';
            return 6;
        }
    } else if (!path_outage
        && (group_type == SRT_GTYPE_BROADCAST
            || group_type == SRT_GTYPE_BACKUP)) {
        payload_valid =
            receive_group_range(group.value, late_join ? late_join_prefix : 0U,
                baseline_message_count, expected_members, group_type,
                payload_hash)
            && payload_hash == expected_payload_hash()
            && send_hash_reply(group.value, payload_hash);
        if (!payload_valid) {
            std::cerr << (group_type == SRT_GTYPE_BACKUP ? "backup"
                                                         : "broadcast")
                      << " receive/reply failed: " << srt_getlasterror_str()
                      << '\n';
            return 6;
        }
    }
    std::size_t size = 0;
    (void)srt_group_data(group.value, nullptr, &size);
    std::int32_t peer_version = 0;
    const bool peer_version_valid =
        read_integer_option(
            group.value, SRTO_PEERVERSION, peer_version)
        && peer_version == pinned_srt_version;
    std::size_t listener_callback_calls = 0;
    bool listener_metadata_valid = false;
    {
        std::lock_guard lock(listener_metadata.mutex);
        listener_callback_calls = listener_metadata.calls;
        listener_metadata_valid = listener_metadata.valid
            && listener_metadata.calls
                == (replacement ? replacement_callback_count
                                : peer_error ? initial_member_count : size);
    }
    std::cout << "{\"event\":\"complete\",\"role\":\"listener\","
              << "\"group\":" << group.value
              << ",\"members\":" << size
              << ",\"listener_update\":"
              << ((initial_listener_update || listener_update)
                      ? "true" : "false")
              << ",\"initial_listener_update\":"
              << (initial_listener_update ? "true" : "false")
              << ",\"late_listener_update\":"
              << (listener_update ? "true" : "false")
              << ",\"bonded_listeners\":"
              << (bonded_address == nullptr ? 1 : 2)
              << ",\"payload_valid\":"
              << (payload_valid ? "true" : "false")
              << ",\"late_join\":"
              << (late_join ? "true" : "false")
              << ",\"peer_error\":"
              << (peer_error ? "true" : "false")
              << ",\"peer_error_replacement\":"
              << (replacement ? "true" : "false")
              << ",\"replacement_attached\":"
              << (replacement_attached ? "true" : "false")
              << ",\"transient_group_receive_errors\":"
              << transient_group_receive_errors
              << ",\"group_connected\":"
              << (srt_getsockstate(group.value) == SRTS_CONNECTED
                      ? "true" : "false")
              << ",\"payload_hash\":" << payload_hash
              << ",\"peer_version\":" << peer_version
              << ",\"peer_version_valid\":"
              << (peer_version_valid ? "true" : "false")
              << ",\"listener_callback_calls\":"
              << listener_callback_calls
              << ",\"listener_metadata_valid\":"
              << (listener_metadata_valid ? "true" : "false")
              << "}\n" << std::flush;
    // srt_send() confirms local queue admission, not remote application
    // delivery. The interop driver releases this process only after the
    // caller has consumed the final hash reply, so group teardown cannot race
    // that observation on a busy CI runner.
    std::string completion_release;
    if (!std::getline(std::cin, completion_release)
        || completion_release != "continue") {
        std::cerr << "listener completion barrier was not released\n";
        return 7;
    }
    return 0;
}

int run_caller(const sockaddr_storage& address, int address_size,
    SRT_GROUP_TYPE group_type, const char* policy_name, bool late_join,
    bool peer_error, bool replacement, bool path_outage, bool receive_contract,
    const SecurityConfiguration& security,
    const sockaddr_storage* bonded_address = nullptr,
    int bonded_address_size = 0,
    const sockaddr_storage* replacement_address = nullptr,
    int replacement_address_size = 0)
{
    GroupConnectCallbacks callbacks;
    SocketHandle group {srt_create_group(group_type)};
    if (group.value == SRT_INVALID_SOCK) {
        std::cerr << "group creation failed: " << srt_getlasterror_str()
                  << '\n';
        return 3;
    }
    if (!configure_security(group.value, security, true)) {
        std::cerr << "group security setup failed: " << srt_getlasterror_str()
                  << '\n';
        return 3;
    }
    if (srt_connect_callback(
            group.value, observe_group_connect, &callbacks)
        == SRT_ERROR) {
        std::cerr << "group callback registration failed: "
                  << srt_getlasterror_str() << '\n';
        return 3;
    }
    if (path_outage
        && srt_setsockflag(group.value, SRTO_SNDTIMEO,
               &path_outage_send_timeout_milliseconds,
               static_cast<int>(sizeof(path_outage_send_timeout_milliseconds)))
            == SRT_ERROR) {
        std::cerr << "path-outage group option setup failed: "
                  << srt_getlasterror_str() << '\n';
        return 3;
    }
    std::array<SRT_SOCKGROUPCONFIG, 3> endpoints {
        srt_prepare_endpoint(
            nullptr, reinterpret_cast<const sockaddr*>(&address), address_size),
        srt_prepare_endpoint(nullptr,
            reinterpret_cast<const sockaddr*>(
                bonded_address == nullptr ? &address : bonded_address),
            bonded_address == nullptr ? address_size : bonded_address_size),
        srt_prepare_endpoint(nullptr,
            reinterpret_cast<const sockaddr*>(replacement_address == nullptr
                    ? &address
                    : replacement_address),
            replacement_address == nullptr ? address_size
                                           : replacement_address_size),
    };
    endpoints[0].weight = 17U;
    endpoints[1].weight = 11U;
    endpoints[2].weight = 7U;
    endpoints[0].token = 4'001;
    endpoints[1].token = 4'002;
    endpoints[2].token = 4'003;
    constexpr int initial_connect_count =
        static_cast<int>(initial_member_count);
    const std::size_t final_member_count = late_join
        ? late_join_member_count
        : initial_member_count;
    const std::size_t final_callback_count = late_join
        ? late_join_member_count
        : replacement ? replacement_callback_count : initial_member_count;
    const SRTSOCKET member = srt_connect_group(
        group.value, endpoints.data(),
        initial_connect_count);
    std::vector<SRTSOCKET> expected_handles{
        endpoints[0].id, endpoints[1].id};
    if (member == SRT_INVALID_SOCK
        || std::find(expected_handles.begin(), expected_handles.end(), member)
            == expected_handles.end()
        || endpoints[0].id == SRT_INVALID_SOCK
        || endpoints[1].id == SRT_INVALID_SOCK
        || !wait_for_members(group.value,
            static_cast<std::size_t>(initial_connect_count), expected_handles,
            late_join         ? late_join_stability
                : path_outage ? path_outage_stability
                              : std::chrono::milliseconds {0})
        || !members_match_security(
            group.value, initial_member_count, security)) {
        std::cerr << "group connect failed: "
                  << srt_getlasterror_str()
                  << "; member=" << member
                  << "; endpoint_ids=" << endpoints[0].id
                  << ',' << endpoints[1].id
                  << "; endpoint_errors=" << endpoints[0].errorcode
                  << ',' << endpoints[1].errorcode << '\n';
        return 4;
    }

    EpollHandle update_poll;
    if (peer_error) {
        update_poll.value = srt_epoll_create();
        const int watched = SRT_EPOLL_UPDATE;
        if (update_poll.value < 0
            || srt_epoll_add_usock(
                update_poll.value, group.value, &watched) == SRT_ERROR
            || !drain_group_updates(update_poll.value)) {
            std::cerr << "group update epoll setup failed: "
                      << srt_getlasterror_str() << '\n';
            return 4;
        }
    }

    bool payload_valid = true;
    bool peer_error_member_isolated = false;
    bool peer_error_group_update = false;
    bool replacement_distinct = false;
    bool replacement_group_update_absent = false;
    bool replacement_connected = false;
    bool post_error_send_succeeded = false;
    std::int32_t peer_version = 0;
    const bool peer_version_valid =
        read_integer_option(
            group.value, SRTO_PEERVERSION, peer_version)
        && peer_version == pinned_srt_version;
    const bool baseline_payload =
        !late_join && !peer_error && !path_outage && !receive_contract;
    if (baseline_payload) {
        std::cout << "{\"event\":\"payload_sender_ready\","
                     "\"role\":\"caller\"}\n"
                  << std::flush;
        std::string send_release;
        if (!std::getline(std::cin, send_release) || send_release != "send") {
            std::cerr << "baseline group sender barrier was not released\n";
            return 5;
        }
    }
    if (receive_contract) {
        std::string send_release;
        if (!std::getline(std::cin, send_release) || send_release != "send") {
            std::cerr << "group receive contract sender barrier was not "
                         "released\n";
            return 5;
        }
    }
    if (path_outage) {
        std::cout << "{\"event\":\"path_outage_sender_ready\","
                     "\"role\":\"caller\",\"primary_member\":"
                  << endpoints[0].id << ",\"backup_member\":" << endpoints[1].id
                  << ",\"srt_version\":" << srt_getversion() << "}\n"
                  << std::flush;
        std::string send_release;
        if (!std::getline(std::cin, send_release) || send_release != "send") {
            std::cerr << "path-outage sender barrier was not released\n";
            return 5;
        }
        std::cout << "{\"event\":\"path_outage_sender_released\","
                     "\"role\":\"caller\"}\n"
                  << std::flush;
        auto observation = send_path_outage_range(group.value);
        payload_valid = observation.sent_messages == baseline_message_count
            && observation.first_active_member == endpoints[0].id
            && observation.final_active_member == endpoints[1].id
            && observation.active_member_transitions >= 1;
        if (!payload_valid) {
            if (observation.error_code == 0) {
                observation.error_code = srt_getlasterror(nullptr);
            }
            emit_path_outage_sender_result("path_outage_sender_failure",
                group.value, endpoints[0].id, endpoints[1].id, observation);
            std::cerr << "path-outage failover invariant failed: backup path "
                         "did not become active\n";
            return 5;
        }
        emit_path_outage_sender_result("path_outage_sender_complete",
            group.value, endpoints[0].id, endpoints[1].id, observation);
    } else if (group_type == SRT_GTYPE_BROADCAST && late_join) {
        payload_valid =
            send_group_range(group.value, 0U, late_join_prefix,
                static_cast<std::size_t>(initial_connect_count), group_type)
            && receive_hash_reply(
                group.value, expected_payload_hash(late_join_prefix));
        if (!payload_valid) {
            std::cerr << "late-join prefix send/reply failed: "
                      << srt_getlasterror_str() << '\n';
            return 5;
        }
        const SRTSOCKET late_member =
            srt_connect_group(group.value, endpoints.data() + 2, 1);
        expected_handles.push_back(endpoints[2].id);
        if (late_member == SRT_INVALID_SOCK || late_member != endpoints[2].id
            || endpoints[2].id == SRT_INVALID_SOCK
            || !wait_for_members(
                group.value, final_member_count, expected_handles)
            || !members_match_security(
                group.value, final_member_count, security)) {
            std::cerr << "late group member connect failed: "
                      << srt_getlasterror_str()
                      << "; member=" << late_member
                      << "; endpoint_id=" << endpoints[2].id
                      << "; endpoint_error="
                      << endpoints[2].errorcode << '\n';
            return 4;
        }
    } else if (group_type == SRT_GTYPE_BROADCAST && peer_error) {
        std::cout
            << "{\"event\":\"peer_error_sender_ready\","
               "\"role\":\"caller\",\"healthy_member\":"
            << endpoints[0].id
            << ",\"failed_member\":" << endpoints[1].id << "}\n"
            << std::flush;
        std::string send_release;
        if (!std::getline(std::cin, send_release)
            || send_release != "send") {
            std::cerr << "peer-error sender barrier was not released\n";
            return 5;
        }
        payload_valid = send_group_range(
            group.value, 0U, peer_error_prefix,
            initial_member_count, group_type);
        if (!payload_valid) {
            std::cerr << "peer-error prefix send failed: "
                      << srt_getlasterror_str() << '\n';
            return 5;
        }
        std::cout
            << "{\"event\":\"peer_error_barrier\",\"role\":\"caller\"}\n"
            << std::flush;
        std::string release;
        if (!std::getline(std::cin, release) || release != "continue") {
            std::cerr << "peer-error barrier was not released\n";
            return 5;
        }
        // The relay releases this phase only after observing the causal ACK
        // and injecting PEERERROR. Read the prefix reply over the surviving
        // path before the next group send consumes the member failure.
        payload_valid = receive_hash_reply(
            group.value, expected_payload_hash(peer_error_prefix));
        if (!payload_valid) {
            std::cerr << "peer-error prefix reply failed: "
                      << srt_getlasterror_str() << '\n';
            return 5;
        }
        // Let one group send consume the injected member error, then observe
        // the partial-path transition before the listener can finish the
        // payload and close the still-healthy connection.
        const std::size_t first_surviving_message = peer_error_prefix;
        post_error_send_succeeded = send_group_range_after_member_failure(
            group.value, first_surviving_message,
            first_surviving_message + 1U);
        peer_error_group_update = wait_for_group_update(
            update_poll.value, group.value);
        peer_error_member_isolated = member_is_isolated(
            endpoints[1].id, endpoints[0].id);
        payload_valid = post_error_send_succeeded
            && peer_error_group_update
            && peer_error_member_isolated;
        if (payload_valid && replacement) {
            const SRTSOCKET replacement_member = srt_connect_group(
                group.value, endpoints.data() + 2, 1);
            replacement_distinct = replacement_member != SRT_INVALID_SOCK
                && replacement_member == endpoints[2].id
                && endpoints[2].id != SRT_INVALID_SOCK
                && endpoints[2].id != endpoints[0].id
                && endpoints[2].id != endpoints[1].id;
            replacement_connected = replacement_distinct
                && wait_for_replacement_members(
                    group.value, endpoints[0].id, endpoints[1].id,
                    endpoints[2].id,
                    late_join_stability);
            replacement_group_update_absent = replacement_connected
                && group_update_is_absent(update_poll.value);
            payload_valid = replacement_group_update_absent;
            if (payload_valid) {
                std::cout
                    << "{\"event\":\"peer_error_replacement_ready\","
                       "\"role\":\"caller\"}\n"
                    << std::flush;
                std::string replacement_release;
                if (!std::getline(std::cin, replacement_release)
                    || replacement_release != "resume") {
                    std::cerr
                        << "peer-error replacement barrier was not released\n";
                    return 5;
                }
            }
        }
        if (payload_valid) {
            payload_valid = send_group_range_after_member_failure(
                group.value, first_surviving_message + 1U,
                baseline_message_count);
        }
        payload_valid = payload_valid
            && receive_hash_reply(group.value, expected_payload_hash());
        if (!payload_valid) {
            const std::string failure_reason =
                post_error_send_succeeded
                    && (!peer_error_group_update
                        || !peer_error_member_isolated)
                ? "group member isolation/update invariant failed"
                : srt_getlasterror_str();
            const SRT_SOCKSTATUS group_state =
                srt_getsockstate(group.value);
            const SRT_SOCKSTATUS failed_state =
                srt_getsockstate(endpoints[1].id);
            const SRT_SOCKSTATUS healthy_state =
                srt_getsockstate(endpoints[0].id);
            const SRT_SOCKSTATUS replacement_state =
                endpoints[2].id == SRT_INVALID_SOCK
                ? SRTS_NONEXIST
                : srt_getsockstate(endpoints[2].id);
            if (replacement) {
                std::cout
                    << "{\"event\":\"peer_error_replacement_unavailable\","
                       "\"role\":\"caller\","
                       "\"post_error_send_succeeded\":"
                    << (post_error_send_succeeded ? "true" : "false")
                    << ",\"peer_error_group_update\":"
                    << (peer_error_group_update ? "true" : "false")
                    << ",\"peer_error_member_isolated\":"
                    << (peer_error_member_isolated ? "true" : "false")
                    << ",\"replacement_distinct\":"
                    << (replacement_distinct ? "true" : "false")
                    << ",\"replacement_connected\":"
                    << (replacement_connected ? "true" : "false")
                    << ",\"group_state\":" << group_state
                    << ",\"failed_state\":" << failed_state
                    << ",\"healthy_state\":" << healthy_state
                    << ",\"replacement_state\":" << replacement_state
                    << "}\n"
                    << std::flush;
            }
            std::cerr << "peer-error surviving-path send/reply failed: "
                      << failure_reason
                      << "; update=" << peer_error_group_update
                      << "; isolated=" << peer_error_member_isolated
                      << "; group_state=" << group_state
                      << "; failed_state=" << failed_state
                      << "; healthy_state=" << healthy_state
                      << "; replacement_state=" << replacement_state
                      << "; replacement=" << replacement
                      << "; replacement_distinct=" << replacement_distinct
                      << "; replacement_update_absent="
                      << replacement_group_update_absent
                      << "; replacement_connected=" << replacement_connected
                      << '\n';
            return 5;
        }
    }
    const bool callback_complete =
        wait_for_group_callbacks(callbacks, final_callback_count);
    const bool callback_valid = callback_complete
        && callbacks_match(
            callbacks, endpoints.data(), final_callback_count);
    if (!peer_error && !path_outage
        && (group_type == SRT_GTYPE_BROADCAST
            || group_type == SRT_GTYPE_BACKUP)) {
        payload_valid = send_group_range(
                group.value,
                late_join ? late_join_prefix : 0U,
                baseline_message_count, final_member_count, group_type)
            && receive_hash_reply(
                group.value, expected_payload_hash());
        if (!payload_valid) {
            std::cerr << policy_name << " send/reply failed: "
                      << srt_getlasterror_str() << '\n';
            return 5;
        }
    }
    std::size_t callback_calls = 0;
    {
        std::lock_guard lock(callbacks.mutex);
        callback_calls = callbacks.events.size();
    }
    std::cout << "{\"event\":\"complete\",\"role\":\"caller\","
              << "\"group\":" << group.value
              << ",\"member\":" << member
              << ",\"members\":" << final_member_count
              << ",\"policy\":\"" << policy_name << "\""
              << ",\"callback_calls\":" << callback_calls
              << ",\"callback_valid\":"
              << (callback_valid ? "true" : "false")
              << ",\"payload_valid\":"
              << (payload_valid ? "true" : "false")
              << ",\"late_join\":"
              << (late_join ? "true" : "false")
              << ",\"peer_error\":"
              << (peer_error ? "true" : "false")
              << ",\"peer_error_replacement\":"
              << (replacement ? "true" : "false")
              << ",\"peer_error_member_isolated\":"
              << (peer_error_member_isolated ? "true" : "false")
              << ",\"peer_error_group_update\":"
              << (peer_error_group_update ? "true" : "false")
              << ",\"replacement_member\":" << endpoints[2].id
              << ",\"replacement_distinct\":"
              << (replacement_distinct ? "true" : "false")
              << ",\"replacement_group_update_absent\":"
              << (replacement_group_update_absent ? "true" : "false")
              << ",\"replacement_connected\":"
              << (replacement_connected ? "true" : "false")
              << ",\"healthy_member\":" << endpoints[0].id
              << ",\"failed_member\":" << endpoints[1].id
              << ",\"group_connected\":"
              << (srt_getsockstate(group.value) == SRTS_CONNECTED
                      ? "true" : "false")
              << ",\"peer_version\":" << peer_version
              << ",\"peer_version_valid\":"
              << (peer_version_valid ? "true" : "false")
              << ",\"tokens\":[" << endpoints[0].token
              << ',' << endpoints[1].token;
    if (late_join || replacement) {
        std::cout << ',' << endpoints[2].token;
    }
    std::cout << "]}\n"
              << std::flush;
    // The replacement interop driver observes the new path's cumulative ACK
    // before it permits transport teardown. Keep every caller-side member
    // alive after application completion so socket destruction cannot race
    // that wire evidence on a busy runner.
    if (replacement || path_outage) {
        std::string completion_release;
        if (!std::getline(std::cin, completion_release)
            || completion_release != "continue") {
            std::cerr << "caller completion barrier was not released\n";
            return 6;
        }
    }
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 4 || argc > 8
        || (std::string{argv[1]} != "listener"
            && std::string{argv[1]} != "caller")) {
        std::cerr << "usage: group-peer listener|caller HOST PORT "
                     "[broadcast|backup] [BONDED_PORT|late-join] "
                     "[REPLACEMENT_PORT] "
                     "[peer-error|peer-error-replacement|path-outage|"
                     "receive-contract|reference-receive-contract]\n";
        return 2;
    }
    std::uint16_t port = 0;
    sockaddr_storage address {};
    int address_size = 0;
    if (!parse_port(argv[3], port)
        || !make_address(argv[2], port, address, address_size)
        || srt_startup() == SRT_ERROR) {
        return 2;
    }
    const std::string policy = argc >= 5
        ? std::string{argv[4]} : "broadcast";
    const SRT_GROUP_TYPE group_type = policy == "broadcast"
        ? SRT_GTYPE_BROADCAST
        : policy == "backup" ? SRT_GTYPE_BACKUP
                             : SRT_GTYPE_UNDEFINED;
    if (group_type == SRT_GTYPE_UNDEFINED) {
        std::cerr << "unsupported group policy: " << policy << '\n';
        (void)srt_cleanup();
        return 2;
    }
    const SecurityConfiguration security = read_security_configuration();
    if (!security.valid) {
        std::cerr << "invalid group security environment\n";
        (void)srt_cleanup();
        return 2;
    }
    sockaddr_storage bonded_address{};
    int bonded_address_size = 0;
    std::uint16_t bonded_port = 0;
    const bool listener_role = std::string{argv[1]} == "listener";
    const bool late_join = (argc == 6
            && std::string{argv[5]} == "late-join")
        || (listener_role && argc == 7
            && std::string{argv[5]} == "late-join"
            && std::string{argv[6]} == "late-join");
    const bool replacement = (listener_role && argc == 7
            && std::string{argv[6]} == "peer-error-replacement")
        || (!listener_role && argc == 8
            && std::string{argv[7]} == "peer-error-replacement");
    const bool peer_error = replacement || (argc == 7
        && std::string{argv[6]} == "peer-error");
    const bool path_outage =
        (listener_role && argc == 6 && std::string {argv[5]} == "path-outage")
        || (!listener_role && argc == 7
            && std::string {argv[6]} == "path-outage");
    const bool receive_contract = argc == 6
        && (std::string {argv[5]} == "receive-contract"
            || std::string {argv[5]} == "reference-receive-contract");
    const bool reference_receive_contract = receive_contract
        && std::string {argv[5]} == "reference-receive-contract";
    const bool bonded = argc >= 6 && !late_join && !receive_contract
        && !(listener_role && path_outage);
    if ((late_join || peer_error) && group_type != SRT_GTYPE_BROADCAST) {
        std::cerr << "selected payload profile requires broadcast\n";
        (void)srt_cleanup();
        return 2;
    }
    if (path_outage && group_type != SRT_GTYPE_BACKUP) {
        std::cerr << "path-outage profile requires backup\n";
        (void)srt_cleanup();
        return 2;
    }
    if ((argc == 7 && !peer_error && !late_join && !path_outage)
        || argc == 8 && !replacement) {
        std::cerr << "unsupported group profile: " << argv[argc - 1] << '\n';
        (void)srt_cleanup();
        return 2;
    }
    if (bonded
        && (!parse_port(argv[5], bonded_port)
            || !make_address(
                argv[2], bonded_port, bonded_address, bonded_address_size))) {
        (void)srt_cleanup();
        return 2;
    }
    sockaddr_storage replacement_address {};
    int replacement_address_size = 0;
    std::uint16_t replacement_port = 0;
    if (replacement && !listener_role
        && (!parse_port(argv[6], replacement_port)
            || !make_address(argv[2], replacement_port, replacement_address,
                replacement_address_size))) {
        (void)srt_cleanup();
        return 2;
    }
    const int result = listener_role
        ? run_listener(address, address_size, group_type, late_join, peer_error,
              replacement, path_outage, receive_contract,
              reference_receive_contract, security,
              bonded ? &bonded_address : nullptr, bonded_address_size)
        : run_caller(address, address_size, group_type, policy.c_str(),
              late_join, peer_error, replacement, path_outage, receive_contract,
              security, bonded ? &bonded_address : nullptr, bonded_address_size,
              replacement ? &replacement_address : nullptr,
              replacement_address_size);
    (void)srt_cleanup();
    return result;
}
