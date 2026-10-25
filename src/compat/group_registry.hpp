#pragma once

#include "compat/closed_handle_history.hpp"
#include "compat/public_socket_options.hpp"

#include "compat/group_replay_buffer.hpp"
#include "compat/readiness.hpp"
#include "compat/statistics.hpp"
#include "robotweax/srt/socket_options.hpp"
#include "robotweax/srt/connection_group.hpp"
#include "robotweax/srt/crypto.hpp"
#include "robotweax/srt/handshake_extensions.hpp"
#include "robotweax/srt/udp.hpp"
#include "srt/srt.h"

#include <cstdint>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace robotweax::srt {
struct TsbpdClockState;
}
namespace robotweax::srt::compat {

template <typename T> class ProcessOwned;
class ConnectionRuntime;
class DatagramChannel;
class GroupReceiveRetention;

// Numeric identities plus generations deliberately replace reciprocal owning
// pointers between sockets and groups.  The coordinator introduced by the
// data-plane slice will be the only writer of these snapshots.
struct GroupMemberSnapshot {
    SRT_SOCKGROUPDATA public_data{};
    std::uint64_t generation = 0;
    // Receive activity survives send-path idling and logical deduplication.
    bool received_data = false;
    // Member retransmissions already folded into the group counters.
    PacketByteCounter retransmitted_folded;
    // This caller member accepted GroupRecord::peer_group during its own
    // handshake. A live holder keeps the pin from being replaced.
    bool holds_peer_group = false;
    ROBOTWEAX_SRT_GROUP_PATHDATA_V1 path_data {};
};

[[nodiscard]] std::uint64_t group_statistics_now_microseconds() noexcept;

// Group-level counters for srt_bstats on a group handle. Like libsrt they
// count messages sent and received through the group once (not once per
// member) and gaps the group receiver skipped. In addition, a group row
// reports the retransmissions of its members, including members that have
// since left. Protected by GroupRecord::mutex. Bytes are payload sizes; the
// trace conversion adds the IPv4 wire header like the socket-level
// statistics do.
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

// Bonding groups (Broadcast, Backup) send one signal between two endpoints
// over several links and pin exactly one peer group. A dispatch group
// (multicast) has one Listener-side sender and any number of Caller
// receivers that never see each other; payload travels once on a multicast
// socket and only control traffic uses the member connections. The category
// decides which registry rules apply; it never changes after creation.
enum class GroupCategory : std::uint8_t {
    bonding,
    dispatch,
};

[[nodiscard]] constexpr GroupCategory group_category_of(
    SRT_GROUP_TYPE type) noexcept
{
    return type == ROBOTWEAX_SRT_GTYPE_MULTICAST
        ? GroupCategory::dispatch
        : GroupCategory::bonding;
}

// Whether `type` names a group this build can create. Balancing (3) and
// every undefined value stay rejected; they are never downgraded.
[[nodiscard]] constexpr bool creatable_group_type(SRT_GROUP_TYPE type) noexcept
{
    return type == SRT_GTYPE_BROADCAST || type == SRT_GTYPE_BACKUP
        || type == ROBOTWEAX_SRT_GTYPE_MULTICAST;
}

// Dispatch-group state that bonding records never use. Kept in one block so
// the bonding fields of GroupRecord stay untouched.
struct DispatchGroupState {
    // Listener side: the admission key the listen callback set through
    // SRTO_ROBOTWEAX_MULTICAST. One dispatch group per (bond, key, domain).
    std::string admission_key;
    // Multicast group the sender transmits to and receivers join.
    IpEndpoint multicast_group{};
    // Local interface selector: IPv4 address or IPv6 scope_id; unspecified
    // lets the operating system choose.
    IpEndpoint multicast_interface{};
    std::int32_t multicast_time_to_live = 1;
    // Whether this record is the sending (Listener) side. The Caller side
    // has exactly one member and a multicast receive channel. The sender
    // transmits from the Listener's own channel socket, so no extra socket
    // is held here.
    bool sender = false;
    // Caller side only: the channel over the socket joined to the group,
    // routing datagrams addressed to `ingress_group_id` (the Listener's
    // dispatch group handle) into the member session.
    std::shared_ptr<DatagramChannel> receive_channel;
    // Caller side only: the control link's channel, which also routes the
    // dispatch group ID so unicast retransmissions reach the member.
    std::weak_ptr<DatagramChannel> control_channel;
    std::uint32_t ingress_group_id = 0;
    bool joined = false;
    // Listener side only, when the listener encrypts: the one traffic-key
    // schedule every member session of this sender draws from.
    std::shared_ptr<SharedTransmitKeySchedule> key_schedule;
};

struct GroupRecord {
    mutable std::mutex mutex;
    GroupCategory category = GroupCategory::bonding;
    // Present exactly when category == dispatch; a bonding record never
    // allocates it.
    std::unique_ptr<DispatchGroupState> dispatch;
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
    // Set once a member that holds the pin has an attached runtime. Before
    // that, a pin whose holders all failed may be replaced by a new peer
    // group; afterwards the peer group is fixed for the group's lifetime.
    bool peer_group_confirmed = false;
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
    // Observation bookkeeping uses the process monotonic clock, independently
    // of the injectable coordinator clock. Protected by mutex, no policy use.
    std::uint64_t send_authority_revision = 0;
    std::uint64_t active_send_observed_since_microseconds = 0;
    std::uint64_t probe_send_observed_since_microseconds = 0;
    SRTSOCKET active_send_member = SRT_INVALID_SOCK;
    std::uint64_t active_send_generation = 0;
    std::uint64_t active_send_since_microseconds = 0;
    SRTSOCKET probe_send_member = SRT_INVALID_SOCK;
    std::uint64_t probe_send_generation = 0;
    std::uint64_t probe_send_since_microseconds = 0;
    std::uint32_t probe_send_start_sequence = 0;
    std::vector<GroupMemberSnapshot> members;
};

// Callers hold GroupRecord::mutex. Explicit now permits deterministic age tests.
[[nodiscard]] ROBOTWEAX_SRT_GROUP_SEND_STATE_V1 group_send_state_snapshot(
    const GroupRecord& group, std::uint64_t now_microseconds) noexcept;
[[nodiscard]] std::uint64_t group_statistics_now_microseconds() noexcept;

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
    [[nodiscard]] int path_data(SRTSOCKET group,
        ROBOTWEAX_SRT_GROUP_PATHDATA_V1* output,
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
    // Bilateral extensions negotiated for the member being admitted. Every
    // member of one mirror carries the same setting, so a listener with an
    // IF_OFFERED policy never mixes labeled and unlabeled members.
    struct MirrorExtensions {
        bool path_identifier = false;
        bool standby_rtt = false;
    };
    // Finds or creates the dispatch sender group a Listener admits a
    // multicast Caller into. One record exists per (listener or bond,
    // admission key, admission domain); the key never crosses a domain.
    // `announcement` is filled for the conclusion handshake: the record's
    // handle as group ID, its multicast endpoint, and its current send
    // sequence. The transmit side uses the Listener's own channel socket,
    // so payload datagrams carry the Listener's address as their source.
    struct DispatchDescription {
        SRTSOCKET group = SRT_INVALID_SOCK;
        std::uint64_t generation = 0;
        bool created = false;
        MulticastDispatch announcement {};
        bool drift_tracer = true;
        std::int64_t minimum_input_bandwidth_bytes_per_second = 0;
        std::int32_t minimum_peer_srt_version = 0x0001'0000;
        // Null when the listener does not encrypt.
        std::shared_ptr<SharedTransmitKeySchedule> key_schedule;
    };
    [[nodiscard]] bool prepare_dispatch(SRTSOCKET listener,
        std::string_view admission_key, std::uint64_t admission_domain,
        std::uint32_t initial_sequence, DispatchDescription& output) noexcept;
    [[nodiscard]] bool prepare_mirror(SRTSOCKET listener, SRTSOCKET peer_group,
        SRT_GROUP_TYPE type, std::uint32_t initial_sequence,
        MirrorDescription& output, std::uint64_t admission_domain = 0,
        std::optional<MirrorExtensions> extensions = std::nullopt) noexcept;
    void release_empty_mirror(
        SRTSOCKET group, std::uint64_t generation) noexcept;
    // Records the accept bond `scope` for `listeners` and moves mirrors those
    // listeners admitted before they were bonded into the bond, so a member
    // that arrives on another bonded listener finds the existing mirror.
    // Mirror admission reads the bond under the same lock, so a member is
    // either admitted before the bond and moved, or admitted into it.
    [[nodiscard]] bool bond_listeners(
        const std::vector<SRTSOCKET>& listeners, std::uint64_t scope) noexcept;
    void forget_listener(SRTSOCKET listener) noexcept;
    [[nodiscard]] bool add_member(SRTSOCKET group, SRTSOCKET socket,
        const sockaddr_storage& peer, std::uint16_t weight, int token,
        std::uint64_t& group_generation, std::uint64_t& member_generation,
        bool* first_member = nullptr, const PathIdentifier& identifier = {},
        bool local_offer = false) noexcept;
    void mark_opened(SRTSOCKET group, std::uint64_t group_generation) noexcept;
    void update_member(SRTSOCKET group, std::uint64_t group_generation,
        SRTSOCKET socket, std::uint64_t member_generation, SRT_SOCKSTATUS state,
        int result, bool broken_connection = false) noexcept;
    void retain_member_receive(SRTSOCKET group, std::uint64_t group_generation,
        SRTSOCKET socket, std::uint64_t member_generation,
        const std::shared_ptr<ConnectionRuntime>& runtime) noexcept;
    void remove_member(SRTSOCKET group, std::uint64_t group_generation,
        SRTSOCKET socket, std::uint64_t member_generation,
        RetiredGroupMemberStatistics* retirement = nullptr) noexcept;
    // Final accounting after the runtime's linger and close drain have ended.
    // Consumes the transferred baseline; stale group generations are ignored.
    void note_retired_member_retransmitted(
        RetiredGroupMemberStatistics& retirement,
        const PacketByteCounter& total) noexcept;
    // Folds a member's cumulative retransmissions into the group counters.
    // Values older than what was already folded are ignored.
    void note_member_retransmitted(SRTSOCKET group,
        std::uint64_t group_generation, SRTSOCKET socket,
        std::uint64_t member_generation,
        const PacketByteCounter& total) noexcept;
    // Claims `peer_group` for one caller member. Fails when another peer
    // group is confirmed or still held by a live member.
    [[nodiscard]] bool set_peer_group(SRTSOCKET group,
        std::uint64_t group_generation, SRTSOCKET socket,
        std::uint64_t member_generation, SRTSOCKET peer_group) noexcept;
    // Marks the claimed peer group as established once the member's
    // runtime is attached.
    void confirm_peer_group(SRTSOCKET group, std::uint64_t group_generation,
        SRTSOCKET socket, std::uint64_t member_generation) noexcept;
    void close(SRTSOCKET group) noexcept;
    void clear() noexcept;

private:
    friend class ProcessOwned<GroupRegistry>;

    GroupRegistry() = default;
    ~GroupRegistry() = default;

    std::mutex mutex_;
    std::unordered_map<SRTSOCKET, std::shared_ptr<GroupRecord>> groups_;
    // One entry per bonded open listener; removed when the listener closes.
    std::unordered_map<SRTSOCKET, std::uint64_t> listener_bond_scopes_;
    ClosedHandleHistory closed_handles_;
    // Allocation position in the group handle permutation; never reset.
    std::uint32_t next_group_index_ = 0;
    std::uint64_t next_generation_ = 1;
    bool clearing_ = false;
};

[[nodiscard]] bool is_group_handle(SRTSOCKET handle) noexcept;
// Whether a caller group may bind `peer_group`. Requires `group.mutex`.
[[nodiscard]] bool peer_group_admits(
    const GroupRecord& group, SRTSOCKET peer_group) noexcept;

} // namespace robotweax::srt::compat
