#pragma once

#include "compat/closed_handle_history.hpp"
#include "compat/public_socket_options.hpp"

#include "compat/group_replay_buffer.hpp"
#include "compat/readiness.hpp"
#include "compat/statistics.hpp"
#include "robotweax/srt/socket_options.hpp"
#include "robotweax/srt/handshake_extensions.hpp"
#include "srt/srt.h"

#include <cstdint>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace robotweax::srt {
struct TsbpdClockState;
}
namespace robotweax::srt::compat {

template <typename T> class ProcessOwned;
class ConnectionRuntime;
class GroupReceiveRetention;

// Numeric identities plus generations deliberately replace reciprocal owning
// pointers between sockets and groups.  The coordinator introduced by the
// data-plane slice will be the only writer of these snapshots.
struct GroupMemberSnapshot {
    SRT_SOCKGROUPDATA public_data{};
    std::uint64_t generation = 0;
    // Receive activity survives send-path idling and logical deduplication.
    bool received_data = false;
};

[[nodiscard]] std::uint64_t group_statistics_now_microseconds() noexcept;

// Group-level counters for srt_bstats on a group handle, mirroring what
// libsrt reports there: messages sent and received through the group once
// (not once per member) and gaps the group receiver skipped. Protected by
// GroupRecord::mutex. Bytes are payload sizes; the trace conversion adds
// the IPv4 wire header like the socket-level statistics do.
struct GroupStatistics {
    bool activated = false;
    std::uint64_t start_microseconds = 0;
    std::uint64_t interval_start_microseconds = 0;
    StatisticsCounters total;
    StatisticsCounters interval;
    // Smoothed received payload size, used to size skipped gaps.
    std::uint64_t average_received_payload_bytes = 0;
    bool received_payload_sample = false;
};

struct GroupRecord {
    mutable std::mutex mutex;
    GroupStatistics statistics;
    // Bound by epoll watches on the group handle; members notify it with
    // their own readiness, membership changes notify it from the registry.
    std::shared_ptr<ReadinessSource> readiness_source =
        std::make_shared<ReadinessSource>();
    // Protected by mutex during connection setup; the shared receive clock
    // serializes its own short timestamp/drift operations, never socket I/O.
    std::optional<std::chrono::steady_clock::time_point> timestamp_origin;
    std::shared_ptr<TsbpdClockState> receive_clock;
    // Group I/O is serialized independently from membership mutation. No
    // potentially blocking member operation may hold `mutex`.
    // Serialize option updates without holding metadata across member I/O.
    mutable std::mutex option_mutex;
    mutable std::mutex send_mutex;
    mutable std::mutex receive_mutex;
    // At most one bounded payload-copy staging batch per group during close.
    mutable std::mutex receive_retirement_mutex;
    SRTSOCKET handle = SRT_INVALID_SOCK;
    SRT_GROUP_TYPE type = SRT_GTYPE_UNDEFINED;
    std::uint64_t generation = 0;
    std::uint64_t snapshot_version = 0;
    std::uint64_t update_version = 0;
    std::uint64_t next_member_generation = 1;
    // Generations increase in insertion order until wrap. Protected by mutex;
    // a wrapped live membership retains the original exact linear lookup.
    bool member_generations_ordered = true;
    std::uint32_t initial_sequence = 0;
    std::uint32_t next_send_sequence = 0;
    std::uint32_t next_send_message = 1;
    // Replay history is accessed only while `send_mutex` is held. The ACK
    // point is the oldest group sequence that may still be needed by a
    // replacement member.
    std::uint32_t replay_acknowledged_sequence = 0;
    GroupReplayBuffer replay_history;
    // Receive-only ownership detached from explicitly closed members.
    std::shared_ptr<GroupReceiveRetention> retained_receive;
    std::uint32_t next_receive_sequence = 0;
    // The first completed caller handshake establishes the reverse sequence
    // space. Later members use the group's receive cursor independently of
    // the caller-to-listener wire ISN.
    bool receive_sequence_initialized = false;
    SRTSOCKET peer_group = SRT_INVALID_SOCK;
    SRTSOCKET mirror_listener = SRT_INVALID_SOCK;
    // Snapshot of the listener bond that owned this mirror at admission.
    std::uint64_t mirror_bond_scope = 0;
    std::uint64_t admission_domain = 0;
    srt_connect_callback_fn* connect_callback = nullptr;
    void* connect_callback_opaque = nullptr;
    bool opened = false;
    bool closed = false;
    bool send_synchronous = true;
    bool receive_synchronous = true;
    std::int32_t send_timeout_milliseconds = -1;
    std::int32_t receive_timeout_milliseconds = -1;
    std::int32_t minimum_stability_timeout_milliseconds = 60;
    std::int32_t ip_time_to_live = 64;
    std::int32_t ip_type_of_service = 0xB8;
    bool ip_type_of_service_explicit = false;
    bool drift_tracer = true;
    std::int64_t minimum_input_bandwidth_bytes_per_second = 0;
    std::int32_t minimum_peer_srt_version = 0x0001'0000;
    std::int32_t peer_idle_timeout_milliseconds = 5'000;
    // Fixed-schema socket configuration: every supported option shares the
    // normal socket validation and normalization, without an unbounded map.
    SocketOptions member_native_options;
    PublicSocketOptions member_public_options;
    SRTSOCKET active_send_member = SRT_INVALID_SOCK;
    std::uint64_t active_send_generation = 0;
    std::uint64_t active_send_since_microseconds = 0;
    SRTSOCKET probe_send_member = SRT_INVALID_SOCK;
    std::uint64_t probe_send_generation = 0;
    std::uint64_t probe_send_since_microseconds = 0;
    std::uint32_t probe_send_start_sequence = 0;
    std::vector<GroupMemberSnapshot> members;
};

class GroupRegistry {
public:
    struct ConnectDescription {
        SRT_GROUP_TYPE type = SRT_GTYPE_UNDEFINED;
        std::uint64_t generation = 0;
        std::uint32_t initial_sequence = 0;
        srt_connect_callback_fn* connect_callback = nullptr;
        void* connect_callback_opaque = nullptr;
        bool block_until_connected = false;
        std::int32_t ip_time_to_live = 64;
        std::int32_t ip_type_of_service = 0xB8;
        bool ip_type_of_service_explicit = false;
        bool drift_tracer = true;
        std::int64_t minimum_input_bandwidth_bytes_per_second = 0;
        std::int32_t minimum_peer_srt_version = 0x0001'0000;
        std::int32_t peer_idle_timeout_milliseconds = 5'000;
        SocketOptions member_native_options;
        PublicSocketOptions member_public_options;
    };

    struct MirrorDescription {
        SRTSOCKET group = SRT_INVALID_SOCK;
        std::uint64_t generation = 0;
        bool created = false;
#ifdef ENABLE_MAXREXMITBW
        std::int64_t maximum_retransmission_bandwidth_bytes_per_second = -1;
#endif
        bool drift_tracer = true;
        std::int64_t minimum_input_bandwidth_bytes_per_second = 0;
        std::int32_t minimum_peer_srt_version = 0x0001'0000;
    };

    [[nodiscard]] static GroupRegistry& instance() noexcept;

    [[nodiscard]] SRTSOCKET create(SRT_GROUP_TYPE type) noexcept;
    [[nodiscard]] std::shared_ptr<GroupRecord> find(
        SRTSOCKET group) noexcept;
    [[nodiscard]] SRT_SOCKSTATUS state(SRTSOCKET group) noexcept;
    [[nodiscard]] int data(
        SRTSOCKET group, SRT_SOCKGROUPDATA* output,
        std::size_t* inout_size) noexcept;
    // srt_bistats for a group handle: SRT_EINVSOCK when the group does not
    // exist or is closed. Only the group-level counters are non-zero.
    [[nodiscard]] int trace_statistics(
        SRTSOCKET group, SRT_TRACEBSTATS& output, bool clear_interval) noexcept;
    static void note_group_sent(
        GroupRecord& group, std::uint64_t payload_bytes) noexcept;
    static void note_group_received(GroupRecord& group, std::uint64_t packets,
        std::uint64_t payload_bytes) noexcept;
    static void note_group_dropped(
        GroupRecord& group, std::uint64_t packets) noexcept;
    [[nodiscard]] bool describe_connect(
        SRTSOCKET group, ConnectDescription& output) noexcept;
    [[nodiscard]] int set_connect_callback(
        SRTSOCKET group, srt_connect_callback_fn* callback,
        void* opaque) noexcept;
    [[nodiscard]] int get_io_option(
        SRTSOCKET group, SRT_SOCKOPT option,
        void* value, int* value_size) noexcept;
    [[nodiscard]] int set_io_option(
        SRTSOCKET group, SRT_SOCKOPT option,
        const void* value, int value_size) noexcept;
    void note_receive_activity(SRTSOCKET group, std::uint64_t group_generation,
        SRTSOCKET socket, std::uint64_t member_generation) noexcept;
    void note_io_result(
        SRTSOCKET group, std::uint64_t group_generation,
        SRTSOCKET socket, std::uint64_t member_generation,
        SRT_MEMBERSTATUS state, int result) noexcept;
    [[nodiscard]] bool prepare_mirror(SRTSOCKET listener, SRTSOCKET peer_group,
        SRT_GROUP_TYPE type, std::uint32_t initial_sequence,
        MirrorDescription& output, std::uint64_t admission_domain = 0) noexcept;
    void release_empty_mirror(
        SRTSOCKET group, std::uint64_t generation) noexcept;
    [[nodiscard]] bool add_member(
        SRTSOCKET group, SRTSOCKET socket,
        const sockaddr_storage& peer, std::uint16_t weight,
        int token, std::uint64_t& group_generation,
        std::uint64_t& member_generation,
        bool* first_member = nullptr) noexcept;
    void mark_opened(SRTSOCKET group, std::uint64_t group_generation) noexcept;
    void update_member(SRTSOCKET group, std::uint64_t group_generation,
        SRTSOCKET socket, std::uint64_t member_generation, SRT_SOCKSTATUS state,
        int result, bool broken_connection = false) noexcept;
    void retain_member_receive(SRTSOCKET group, std::uint64_t group_generation,
        SRTSOCKET socket, std::uint64_t member_generation,
        const std::shared_ptr<ConnectionRuntime>& runtime) noexcept;
    void remove_member(SRTSOCKET group, std::uint64_t group_generation,
        SRTSOCKET socket, std::uint64_t member_generation) noexcept;
    [[nodiscard]] bool set_peer_group(
        SRTSOCKET group, std::uint64_t group_generation,
        SRTSOCKET peer_group) noexcept;
    void close(SRTSOCKET group) noexcept;
    void clear() noexcept;

private:
    friend class ProcessOwned<GroupRegistry>;

    GroupRegistry() = default;
    ~GroupRegistry() = default;

    std::mutex mutex_;
    std::unordered_map<SRTSOCKET, std::shared_ptr<GroupRecord>> groups_;
    ClosedHandleHistory closed_handles_;
    // Allocation position in the group handle permutation; never reset.
    std::uint32_t next_group_index_ = 0;
    std::uint64_t next_generation_ = 1;
    bool clearing_ = false;
};

[[nodiscard]] bool is_group_handle(SRTSOCKET handle) noexcept;

} // namespace robotweax::srt::compat
