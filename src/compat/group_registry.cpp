#include "compat/group_registry.hpp"
#include "compat/process_owned.hpp"
#include "compat/random_identity.hpp"

#include "compat/error_state.hpp"
#include "compat/readiness.hpp"
#include "compat/socket_registry.hpp"
#include "robotweax/srt/sequence.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <new>
#include <span>

namespace robotweax::srt::compat {
namespace {

[[nodiscard]] SRT_SOCKSTATUS aggregate_state(
    const GroupRecord& group) noexcept
{
    if (group.closed) {
        return SRTS_CLOSED;
    }

    SRT_SOCKSTATUS pending = SRTS_NONEXIST;
    for (const auto& member : group.members) {
        if (member.public_data.sockstate == SRTS_CONNECTED) {
            return SRTS_CONNECTED;
        }
        if (pending == SRTS_NONEXIST
            && member.public_data.sockstate != SRTS_BROKEN
            && member.public_data.sockstate != SRTS_CLOSING
            && member.public_data.sockstate != SRTS_CLOSED
            && member.public_data.sockstate != SRTS_NONEXIST) {
            pending = member.public_data.sockstate;
        }
    }
    // This is the observable v1.5.7 result for a newly created empty group.
    return pending == SRTS_NONEXIST ? SRTS_BROKEN : pending;
}

[[nodiscard]] SRT_MEMBERSTATUS group_member_status(
    SRT_SOCKSTATUS state) noexcept
{
    switch (state) {
    case SRTS_CONNECTED:
        return SRT_GST_IDLE;
    case SRTS_BROKEN:
    case SRTS_CLOSING:
    case SRTS_CLOSED:
    case SRTS_NONEXIST:
        return SRT_GST_BROKEN;
    default:
        return SRT_GST_PENDING;
    }
}

[[nodiscard]] constexpr bool terminal_member_state(
    SRT_SOCKSTATUS state) noexcept
{
    return state == SRTS_BROKEN || state == SRTS_CLOSING
        || state == SRTS_CLOSED || state == SRTS_NONEXIST;
}

// Callers hold group.mutex. Erasure keeps insertion order; successful append
// tracks whether generation wrap invalidated the binary-search invariant.
[[nodiscard]] auto find_member(
    GroupRecord& group, SRTSOCKET socket, std::uint64_t generation) noexcept
{
    if (!group.member_generations_ordered) {
        return std::find_if(group.members.begin(), group.members.end(),
            [socket, generation](const auto& member) {
                return member.generation == generation
                    && member.public_data.id == socket;
            });
    }
    const auto member =
        std::lower_bound(group.members.begin(), group.members.end(), generation,
            [](const auto& candidate, std::uint64_t sought) {
                return candidate.generation < sought;
            });
    return member != group.members.end() && member->generation == generation
            && member->public_data.id == socket
        ? member
        : group.members.end();
}

// Refresh outside the group lock: state publication takes the socket lock
// first, then updates the generation-checked group snapshot.
void refresh_member_states(const std::shared_ptr<GroupRecord>& record) noexcept
{
    std::array<SRTSOCKET, 16> inline_sockets {};
    std::vector<SRTSOCKET> overflow;
    std::span<const SRTSOCKET> sockets;
    try {
        {
            std::lock_guard lock(record->mutex);
            if (record->members.size() <= inline_sockets.size()) {
                std::transform(record->members.begin(), record->members.end(),
                    inline_sockets.begin(), [](const auto& member) {
                        return member.public_data.id;
                    });
                sockets =
                    std::span {inline_sockets}.first(record->members.size());
            } else {
                overflow.reserve(record->members.size());
                for (const auto& member : record->members) {
                    overflow.push_back(member.public_data.id);
                }
                sockets = overflow;
            }
        }
        for (const auto socket : sockets) {
            (void)SocketRegistry::instance().state(socket);
        }
    } catch (const std::bad_alloc&) {
        // Keep the last published snapshot if allocation is unavailable.
    }
}

void advance_version(std::uint64_t& version) noexcept
{
    ++version;
    if (version == 0U) {
        version = 1U;
    }
}

} // namespace

bool is_group_handle(SRTSOCKET handle) noexcept
{
    return handle >= 0 && (handle & SRTGROUP_MASK) != 0;
}

GroupRegistry& GroupRegistry::instance() noexcept
{
    static ProcessOwned<GroupRegistry> registry;
    return registry.get();
}

namespace {

// Group handles carry SRTGROUP_MASK on top of a permuted base. The permutation
// never repeats a base; the lookup only guards the registry invariant.
[[nodiscard]] SRTSOCKET next_group_handle(
    const std::unordered_map<SRTSOCKET, std::shared_ptr<GroupRecord>>& groups,
    std::uint32_t& index) noexcept
{
    for (;;) {
        const SRTSOCKET base = next_registry_handle(HandleSpace::group, index);
        if (base == SRT_INVALID_SOCK) {
            return SRT_INVALID_SOCK;
        }
        const SRTSOCKET candidate = base | SRTGROUP_MASK;
        if (groups.find(candidate) == groups.end()) {
            return candidate;
        }
    }
}

} // namespace

SRTSOCKET GroupRegistry::create(SRT_GROUP_TYPE type) noexcept
{
    if (type != SRT_GTYPE_BROADCAST && type != SRT_GTYPE_BACKUP) {
        return SRT_INVALID_SOCK;
    }

    try {
        auto record = std::make_shared<GroupRecord>();
        (void)record->member_native_options.set(
            SocketOption::maximum_payload_size, SRT_LIVE_DEF_PLSIZE);
        std::lock_guard lock(mutex_);
        if (clearing_) {
            return SRT_INVALID_SOCK;
        }

        const auto initial_sequence = random_initial_sequence();
        if (!initial_sequence) {
            return SRT_INVALID_SOCK;
        }
        const SRTSOCKET candidate =
            next_group_handle(groups_, next_group_index_);
        if (candidate == SRT_INVALID_SOCK) {
            return SRT_INVALID_SOCK;
        }
        record->handle = candidate;
        record->type = type;
        record->generation = next_generation_++;
        record->initial_sequence = *initial_sequence;
        record->next_send_sequence = record->initial_sequence;
        record->replay_acknowledged_sequence = record->initial_sequence;
        record->next_receive_sequence = record->initial_sequence;
        if (next_generation_ == 0) {
            next_generation_ = 1;
        }
        groups_.emplace(candidate, std::move(record));
        return candidate;
    } catch (const std::bad_alloc&) {
        return SRT_INVALID_SOCK;
    } catch (...) {
        return SRT_INVALID_SOCK;
    }
}

std::uint64_t group_statistics_now_microseconds() noexcept
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// Callers hold group.mutex.
void GroupRegistry::note_group_sent(
    GroupRecord& group, std::uint64_t payload_bytes) noexcept
{
    group.statistics.total.sent_unique.add(1U, payload_bytes);
    group.statistics.interval.sent_unique.add(1U, payload_bytes);
}

void GroupRegistry::note_group_received(GroupRecord& group,
    std::uint64_t packets, std::uint64_t payload_bytes) noexcept
{
    group.statistics.total.received_unique.add(packets, payload_bytes);
    group.statistics.interval.received_unique.add(packets, payload_bytes);
    if (packets != 0U) {
        const auto sample = payload_bytes / packets;
        if (!group.statistics.received_payload_sample) {
            group.statistics.average_received_payload_bytes = sample;
            group.statistics.received_payload_sample = true;
        } else {
            group.statistics.average_received_payload_bytes =
                (3U * group.statistics.average_received_payload_bytes + sample)
                / 4U;
        }
    }
}

void GroupRegistry::note_group_dropped(
    GroupRecord& group, std::uint64_t packets) noexcept
{
    const std::uint64_t average = group.statistics.received_payload_sample
        ? group.statistics.average_received_payload_bytes
        : static_cast<std::uint64_t>(SRT_LIVE_DEF_PLSIZE);
    group.statistics.total.receiver_dropped.add(packets, packets * average);
    group.statistics.interval.receiver_dropped.add(packets, packets * average);
}

int GroupRegistry::trace_statistics(
    SRTSOCKET group, SRT_TRACEBSTATS& output, bool clear_interval) noexcept
{
    const auto record = find(group);
    if (record == nullptr) {
        set_last_error(SRT_EINVSOCK);
        return SRT_ERROR;
    }
    RuntimeStatisticsSnapshot snapshot;
    {
        std::lock_guard lock(record->mutex);
        if (record->closed) {
            set_last_error(SRT_EINVSOCK);
            return SRT_ERROR;
        }
        if (!record->statistics.activated) {
            set_last_error(SRT_ENOCONN);
            return SRT_ERROR;
        }
        const auto now = group_statistics_now_microseconds();
        auto& statistics = record->statistics;
        snapshot.timestamp_milliseconds =
            (now - statistics.start_microseconds) / 1'000U;
        snapshot.interval_microseconds =
            now - statistics.interval_start_microseconds;
        snapshot.total = statistics.total;
        snapshot.interval = statistics.interval;
        if (clear_interval) {
            statistics.interval = {};
            statistics.interval_start_microseconds = now;
        }
    }
    populate_trace_statistics(snapshot, output);
    return 0;
}

std::shared_ptr<GroupRecord> GroupRegistry::find(SRTSOCKET group) noexcept
{
    if (!is_group_handle(group)) {
        return nullptr;
    }
    std::lock_guard lock(mutex_);
    const auto entry = groups_.find(group);
    return entry == groups_.end() ? nullptr : entry->second;
}

SRT_SOCKSTATUS GroupRegistry::state(SRTSOCKET group) noexcept
{
    std::shared_ptr<GroupRecord> record;
    {
        std::lock_guard lock(mutex_);
        const auto entry = groups_.find(group);
        if (entry == groups_.end()) {
            return closed_handles_.contains(group) ? SRTS_CLOSED
                                                   : SRTS_NONEXIST;
        }
        record = entry->second;
    }
    refresh_member_states(record);
    std::lock_guard lock(record->mutex);
    return aggregate_state(*record);
}

int GroupRegistry::data(
    SRTSOCKET group, SRT_SOCKGROUPDATA* output,
    std::size_t* inout_size) noexcept
{
    if (inout_size == nullptr || !is_group_handle(group)) {
        set_last_error(SRT_EINVPARAM);
        return SRT_ERROR;
    }
    const auto record = find(group);
    if (record == nullptr) {
        set_last_error(SRT_EINVPARAM);
        return SRT_ERROR;
    }

    refresh_member_states(record);
    std::lock_guard lock(record->mutex);
    if (record->closed) {
        set_last_error(SRT_EINVPARAM);
        return SRT_ERROR;
    }
    const std::size_t capacity = *inout_size;
    *inout_size = record->members.size();
    if (output == nullptr) {
        return 0;
    }
    if (capacity < record->members.size()) {
        set_last_error(SRT_ELARGEMSG);
        return SRT_ERROR;
    }
    std::transform(record->members.begin(), record->members.end(), output,
        [](const GroupMemberSnapshot& member) {
            return member.public_data;
        });
    return static_cast<int>(record->members.size());
}

bool GroupRegistry::describe_connect(
    SRTSOCKET group, ConnectDescription& output) noexcept
{
    const auto record = find(group);
    if (record == nullptr) {
        return false;
    }
    std::lock_guard lock(record->mutex);
    if (record->closed) {
        return false;
    }
    output.type = record->type;
    output.generation = record->generation;
    // A member added after payload transmission must join at the group's
    // current logical sequence, not replay the creation-time ISN.
    output.initial_sequence = record->next_send_sequence;
    output.connect_callback = record->connect_callback;
    output.connect_callback_opaque = record->connect_callback_opaque;
    output.block_until_connected =
        !record->opened && record->receive_synchronous;
    output.ip_time_to_live = record->ip_time_to_live;
    output.ip_type_of_service = record->ip_type_of_service;
    output.ip_type_of_service_explicit = record->ip_type_of_service_explicit;
    output.drift_tracer = record->drift_tracer;
    output.minimum_input_bandwidth_bytes_per_second =
        record->minimum_input_bandwidth_bytes_per_second;
    output.minimum_peer_srt_version = record->minimum_peer_srt_version;
    output.peer_idle_timeout_milliseconds =
        record->peer_idle_timeout_milliseconds;
    output.member_native_options = record->member_native_options;
    output.member_public_options = record->member_public_options;
    return true;
}

int GroupRegistry::set_connect_callback(
    SRTSOCKET group, srt_connect_callback_fn* callback, void* opaque) noexcept
{
    const auto record = find(group);
    if (record == nullptr) {
        set_last_error(SRT_EINVSOCK);
        return SRT_ERROR;
    }
    std::lock_guard lock(record->mutex);
    if (record->closed) {
        set_last_error(SRT_EINVSOCK);
        return SRT_ERROR;
    }
    record->connect_callback = callback;
    record->connect_callback_opaque =
        callback == nullptr ? nullptr : opaque;
    return 0;
}

int GroupRegistry::get_io_option(
    SRTSOCKET group, SRT_SOCKOPT option, void* value, int* value_size) noexcept
{
    if (value == nullptr || value_size == nullptr || *value_size < 0) {
        set_last_error(SRT_EINVPARAM);
        return SRT_ERROR;
    }
    const auto record = find(group);
    if (record == nullptr) {
        set_last_error(SRT_EINVSOCK);
        return SRT_ERROR;
    }
    if (option == SRTO_STATE)
        refresh_member_states(record);
    SocketRecord configured {SocketRecord::Purpose::option_template};
    SRTSOCKET first_member_handle = SRT_INVALID_SOCK;
    const bool group_owned = option == SRTO_SNDSYN || option == SRTO_RCVSYN
        || option == SRTO_SNDTIMEO || option == SRTO_RCVTIMEO;
    {
        std::lock_guard lock(record->mutex);
        if (record->closed) {
            set_last_error(SRT_EINVSOCK);
            return SRT_ERROR;
        }
        if (option == SRTO_BINDTODEVICE || option == SRTO_EVENT
            || option == SRTO_SNDDATA || option == SRTO_RCVDATA
            || option == SRTO_GROUPTYPE) {
            set_last_error(SRT_EINVOP);
            return SRT_ERROR;
        }
        if (option == SRTO_STATE || option == SRTO_GROUPMINSTABLETIMEO) {
            if (option == SRTO_GROUPMINSTABLETIMEO
                && record->type != SRT_GTYPE_BACKUP) {
                set_last_error(SRT_EINVPARAM);
                return SRT_ERROR;
            }
            const std::int32_t result = option == SRTO_STATE
                ? static_cast<std::int32_t>(aggregate_state(*record))
                : record->minimum_stability_timeout_milliseconds;
            if (*value_size < static_cast<int>(sizeof(result))) {
                set_last_error(SRT_EINVPARAM);
                return SRT_ERROR;
            }
            std::memcpy(value, &result, sizeof(result));
            *value_size = sizeof(result);
            return 0;
        }
        configured.public_options = record->member_public_options;
        configured.native_options = record->member_native_options;
        configured.public_options.send_synchronous = record->send_synchronous;
        configured.public_options.receive_synchronous =
            record->receive_synchronous;
        configured.public_options.send_timeout_milliseconds =
            record->send_timeout_milliseconds;
        configured.public_options.receive_timeout_milliseconds =
            record->receive_timeout_milliseconds;
        if (!group_owned) {
            for (const auto& member : record->members) {
                if (member.public_data.sockstate != SRTS_CLOSED
                    && member.public_data.sockstate != SRTS_NONEXIST) {
                    first_member_handle = member.public_data.id;
                    break;
                }
            }
        }
    }
    const auto first_member =
        SocketRegistry::instance().find(first_member_handle);
    return first_member != nullptr
        ? get_socket_option(group, *first_member, option, value, value_size)
        : get_socket_option(group, configured, option, value, value_size);
}

int GroupRegistry::set_io_option(SRTSOCKET group, SRT_SOCKOPT option,
    const void* value, int value_size) noexcept
{
    const auto record = find(group);
    if (record == nullptr) {
        set_last_error(SRT_EINVSOCK);
        return SRT_ERROR;
    }
    std::lock_guard option_lock(record->option_mutex);
    SocketRecord configured {SocketRecord::Purpose::option_template};
    std::vector<std::pair<SRTSOCKET, std::shared_ptr<SocketRecord>>> members;
    const bool group_owned = option == SRTO_SNDSYN || option == SRTO_RCVSYN
        || option == SRTO_SNDTIMEO || option == SRTO_RCVTIMEO;
    {
        std::lock_guard lock(record->mutex);
        if (record->closed) {
            set_last_error(SRT_EINVSOCK);
            return SRT_ERROR;
        }
        if (option == SRTO_GROUPMINSTABLETIMEO) {
            std::int32_t parsed = 0;
            if (value == nullptr
                || value_size != static_cast<int>(sizeof(parsed))) {
                set_last_error(SRT_EINVPARAM);
                return SRT_ERROR;
            }
            std::memcpy(&parsed, value, sizeof(parsed));
            if (record->type != SRT_GTYPE_BACKUP || parsed < 60
                || parsed > 5'000) {
                set_last_error(SRT_EINVPARAM);
                return SRT_ERROR;
            }
            if (record->opened) {
                set_last_error(SRT_ECONNSOCK);
                return SRT_ERROR;
            }
            record->minimum_stability_timeout_milliseconds = parsed;
            return 0;
        }
        if (option == SRTO_BINDTODEVICE || option == SRTO_CONGESTION
            || option == SRTO_GROUPCONNECT || option == SRTO_RENDEZVOUS
            || option == SRTO_GROUPTYPE) {
            set_last_error(SRT_EINVOP);
            return SRT_ERROR;
        }
        configured.public_options = record->member_public_options;
        configured.native_options = record->member_native_options;
        configured.state = record->opened ? SRTS_CONNECTED : SRTS_INIT;
        // Use the ordinary socket rules for validation and PRE/POST stages.
        if (set_socket_option(configured, option, value, value_size)
            == SRT_ERROR) {
            return SRT_ERROR;
        }
        if (configured.native_options.transmission_type()
                != TransmissionType::live
            || configured.native_options.get(SocketOption::tsbpd_mode).value
                == 0) {
            set_last_error(SRT_EINVPARAM);
            return SRT_ERROR;
        }
        try {
            if (!group_owned) {
                members.reserve(record->members.size());
                for (const auto& member : record->members) {
                    members.emplace_back(member.public_data.id, nullptr);
                }
            }
        } catch (...) {
            set_last_error(SRT_ENOBUF);
            return SRT_ERROR;
        }
    }
    for (auto& [handle, member] : members) {
        member = SocketRegistry::instance().find(handle);
    }
    // Preflight every extant member before changing any member or template.
    // A concurrently closed member may still fail during commit; report that
    // error rather than claiming all links accepted the update.
    for (const auto& [handle, member] : members) {
        if (member == nullptr)
            continue;
        SocketRecord candidate {SocketRecord::Purpose::option_template};
        {
            std::lock_guard member_lock(member->mutex);
            candidate.public_options = member->public_options;
            candidate.native_options = member->native_options;
            candidate.state = member->state;
        }
        if (set_socket_option(candidate, option, value, value_size)
            == SRT_ERROR) {
            return SRT_ERROR;
        }
    }
    for (const auto& [handle, member] : members) {
        if (member == nullptr)
            continue;
        if (set_socket_option(*member, option, value, value_size)
            == SRT_ERROR) {
            return SRT_ERROR;
        }
        std::shared_ptr<ConnectionRuntime> runtime;
        SocketOptions native_options;
        {
            std::lock_guard member_lock(member->mutex);
            runtime = member->runtime;
            native_options = member->native_options;
        }
        if (runtime != nullptr)
            runtime->apply_options(native_options);
    }
    {
        std::lock_guard lock(record->mutex);
        if (record->closed) {
            set_last_error(SRT_EINVSOCK);
            return SRT_ERROR;
        }
        record->member_public_options = configured.public_options;
        record->member_native_options = configured.native_options;
        const auto& options = record->member_public_options;
        record->send_synchronous = options.send_synchronous;
        record->receive_synchronous = options.receive_synchronous;
        record->send_timeout_milliseconds = options.send_timeout_milliseconds;
        record->receive_timeout_milliseconds =
            options.receive_timeout_milliseconds;
        record->drift_tracer = options.drift_tracer;
        record->minimum_input_bandwidth_bytes_per_second =
            options.minimum_input_bandwidth_bytes_per_second;
        record->minimum_peer_srt_version = options.minimum_peer_srt_version;
        record->peer_idle_timeout_milliseconds =
            options.peer_idle_timeout_milliseconds;
        record->ip_time_to_live = options.ip_time_to_live;
        record->ip_type_of_service = options.ip_type_of_service;
        record->ip_type_of_service_explicit =
            options.ip_type_of_service_explicit;
    }
    ReadinessSignal::notify(*record->readiness_source);
    return 0;
}

void GroupRegistry::note_io_result(
    SRTSOCKET group, std::uint64_t group_generation,
    SRTSOCKET socket, std::uint64_t member_generation,
    SRT_MEMBERSTATUS state, int result) noexcept
{
    const auto record = find(group);
    if (record == nullptr) {
        return;
    }
    std::lock_guard lock(record->mutex);
    if (record->closed || record->generation != group_generation) {
        return;
    }
    const auto member = find_member(*record, socket, member_generation);
    if (member == record->members.end()) {
        return;
    }
    if (member->public_data.memberstate != state
        || member->public_data.result != result) {
        member->public_data.memberstate = state;
        member->public_data.result = result;
        advance_version(record->snapshot_version);
    }
}

bool GroupRegistry::prepare_mirror(
    SRTSOCKET listener, SRTSOCKET peer_group,
    SRT_GROUP_TYPE type, std::uint32_t initial_sequence,
    MirrorDescription& output) noexcept
{
    output = {};
    if (listener < 0 || is_group_handle(listener)
        || !is_group_handle(peer_group)
        || initial_sequence > SequenceNumber::mask
        || (type != SRT_GTYPE_BROADCAST && type != SRT_GTYPE_BACKUP)) {
        return false;
    }

    PublicSocketOptions listener_public_options;
    SocketOptions listener_native_options;
    std::uint64_t bond_scope = 0U;
    bool listener_send_synchronous = true;
    bool listener_receive_synchronous = true;
    std::int32_t listener_send_timeout_milliseconds = -1;
    std::int32_t listener_receive_timeout_milliseconds = -1;
    bool listener_drift_tracer = true;
    std::int64_t listener_minimum_input_bandwidth = 0;
    std::int32_t listener_minimum_peer_version = 0x0001'0000;
    const auto listener_record =
        SocketRegistry::instance().find(listener);
    if (listener_record == nullptr) {
        return false;
    }
    {
        std::lock_guard lock(listener_record->mutex);
        listener_public_options = listener_record->public_options;
        listener_native_options = listener_record->native_options;
        bond_scope = listener_record->accept_bond_scope;
        listener_send_synchronous =
            listener_record->public_options.send_synchronous;
        listener_receive_synchronous =
            listener_record->public_options.receive_synchronous;
        listener_send_timeout_milliseconds =
            listener_record->public_options.send_timeout_milliseconds;
        listener_receive_timeout_milliseconds =
            listener_record->public_options.receive_timeout_milliseconds;
        listener_drift_tracer =
            listener_record->public_options.drift_tracer;
        listener_minimum_input_bandwidth = listener_record->public_options
            .minimum_input_bandwidth_bytes_per_second;
        listener_minimum_peer_version = listener_record->public_options
            .minimum_peer_srt_version;
    }

    try {
        std::lock_guard registry_lock(mutex_);
        if (clearing_) {
            return false;
        }
        for (const auto& entry : groups_) {
            const auto& record = entry.second;
            std::lock_guard record_lock(record->mutex);
            if (!record->closed
                && ((bond_scope != 0U
                        && record->mirror_bond_scope == bond_scope)
                    || (bond_scope == 0U
                        && record->mirror_bond_scope == 0U
                        && record->mirror_listener == listener))
                && record->peer_group == peer_group) {
                // The handshake carries the sender's next packet sequence.
                // The mirror cursor advances when the application receives,
                // so unread packets put it behind that handshake sequence.
                // A delayed handshake can also arrive after the mirror has
                // consumed packets beyond its starting sequence. Keep both
                // directions within an unambiguous wrap-aware window.
                constexpr std::int32_t maximum_join_lag =
                    static_cast<std::int32_t>(SequenceNumber::half_range / 2U);
                const auto offset =
                    SequenceNumber {initial_sequence}.distance_from(
                        SequenceNumber {record->next_receive_sequence});
                if (record->type != type || offset <= -maximum_join_lag) {
                    return false;
                }
                output.group = record->handle;
                output.generation = record->generation;
                output.drift_tracer = record->drift_tracer;
                output.minimum_input_bandwidth_bytes_per_second =
                    record->minimum_input_bandwidth_bytes_per_second;
                output.minimum_peer_srt_version =
                    record->minimum_peer_srt_version;
                return true;
            }
        }

        const SRTSOCKET candidate =
            next_group_handle(groups_, next_group_index_);
        if (candidate == SRT_INVALID_SOCK) {
            return false;
        }
        auto prepared = std::make_shared<GroupRecord>();
        prepared->handle = candidate;
        prepared->type = type;
        prepared->generation = next_generation_++;
        prepared->initial_sequence = initial_sequence;
        prepared->next_send_sequence = initial_sequence;
        prepared->replay_acknowledged_sequence = initial_sequence;
        prepared->next_receive_sequence = initial_sequence;
        prepared->peer_group = peer_group;
        prepared->mirror_listener = listener;
        prepared->mirror_bond_scope = bond_scope;
        prepared->member_public_options = listener_public_options;
        prepared->member_native_options = listener_native_options;
        prepared->peer_idle_timeout_milliseconds =
            listener_public_options.peer_idle_timeout_milliseconds;
        prepared->ip_time_to_live = listener_public_options.ip_time_to_live;
        prepared->ip_type_of_service =
            listener_public_options.ip_type_of_service;
        prepared->ip_type_of_service_explicit =
            listener_public_options.ip_type_of_service_explicit;
        prepared->send_synchronous = listener_send_synchronous;
        prepared->receive_synchronous = listener_receive_synchronous;
        prepared->send_timeout_milliseconds =
            listener_send_timeout_milliseconds;
        prepared->receive_timeout_milliseconds =
            listener_receive_timeout_milliseconds;
        prepared->drift_tracer = listener_drift_tracer;
        prepared->minimum_input_bandwidth_bytes_per_second =
            listener_minimum_input_bandwidth;
        prepared->minimum_peer_srt_version = listener_minimum_peer_version;
        if (next_generation_ == 0U) {
            next_generation_ = 1U;
        }
        output.group = candidate;
        output.generation = prepared->generation;
        output.created = true;
        output.drift_tracer = prepared->drift_tracer;
        output.minimum_input_bandwidth_bytes_per_second =
            prepared->minimum_input_bandwidth_bytes_per_second;
        output.minimum_peer_srt_version = prepared->minimum_peer_srt_version;
        groups_.emplace(candidate, std::move(prepared));
        return true;
    } catch (...) {
        return false;
    }
}

void GroupRegistry::release_empty_mirror(
    SRTSOCKET group, std::uint64_t generation) noexcept
{
    std::lock_guard registry_lock(mutex_);
    const auto entry = groups_.find(group);
    if (entry == groups_.end()) {
        return;
    }
    const auto record = entry->second;
    std::lock_guard record_lock(record->mutex);
    if (record->generation != generation
        || record->mirror_listener == SRT_INVALID_SOCK
        || record->opened || !record->members.empty()) {
        return;
    }
    record->closed = true;
    groups_.erase(entry);
}

bool GroupRegistry::add_member(
    SRTSOCKET group, SRTSOCKET socket,
    const sockaddr_storage& peer, std::uint16_t weight,
    int token, std::uint64_t& group_generation,
    std::uint64_t& member_generation,
    bool* first_member) noexcept
{
    if (first_member != nullptr) {
        *first_member = false;
    }
    const auto record = find(group);
    if (record == nullptr) {
        return false;
    }
    try {
        std::lock_guard lock(record->mutex);
        if (record->closed) {
            return false;
        }
        const bool was_empty = record->members.empty();
        const bool was_terminal =
            record->opened && aggregate_state(*record) == SRTS_BROKEN;
        GroupMemberSnapshot member;
        member.generation = record->next_member_generation++;
        if (record->next_member_generation == 0U) {
            record->next_member_generation = 1U;
        }
        member.public_data.id = socket;
        member.public_data.peeraddr = peer;
        member.public_data.sockstate = SRTS_CONNECTING;
        member.public_data.weight = weight;
        member.public_data.memberstate = SRT_GST_PENDING;
        member.public_data.result = SRT_SUCCESS;
        member.public_data.token = token;
        const bool remains_ordered = was_empty
            || (record->member_generations_ordered
                && record->members.back().generation < member.generation);
        record->members.push_back(member);
        record->member_generations_ordered = remains_ordered;
        if (was_terminal) {
            // A pending replacement clears the group's terminal OUT/ERR
            // level even if it fails before the next epoll state sample.
            record->readiness_source->note_not_ready(
                SRT_EPOLL_OUT | SRT_EPOLL_ERR);
        }
        ++record->snapshot_version;
        group_generation = record->generation;
        member_generation = member.generation;
        if (first_member != nullptr) {
            *first_member = was_empty && !record->opened;
        }
    } catch (...) {
        return false;
    }
    // Membership is the group handle's own readiness input.
    ReadinessSignal::notify(*record->readiness_source);
    return true;
}

void GroupRegistry::mark_opened(
    SRTSOCKET group, std::uint64_t group_generation) noexcept
{
    const auto record = find(group);
    if (record == nullptr) {
        return;
    }
    std::lock_guard lock(record->mutex);
    if (!record->closed
        && record->generation == group_generation) {
        record->opened = true;
    }
}

void GroupRegistry::update_member(
    SRTSOCKET group, std::uint64_t group_generation,
    SRTSOCKET socket, std::uint64_t member_generation,
    SRT_SOCKSTATUS state, int result,
    bool broken_connection) noexcept
{
    const auto record = find(group);
    if (record == nullptr) {
        return;
    }
    bool changed = false;
    {
        std::lock_guard lock(record->mutex);
        if (record->closed || record->generation != group_generation) {
            return;
        }
        const auto member = find_member(*record, socket, member_generation);
        if (member == record->members.end()) {
            return;
        }
        const SRT_SOCKSTATUS previous_state = member->public_data.sockstate;
        if (state == SRTS_CONNECTED && !record->statistics.activated) {
            const auto start = record->timestamp_origin.value_or(
                std::chrono::steady_clock::now());
            const auto start_microseconds =
                std::chrono::duration_cast<std::chrono::microseconds>(
                    start.time_since_epoch())
                    .count();
            record->statistics.start_microseconds =
                static_cast<std::uint64_t>(start_microseconds);
            record->statistics.interval_start_microseconds =
                record->statistics.start_microseconds;
            record->statistics.activated = true;
        }
        if (previous_state != state || member->public_data.result != result) {
            member->public_data.sockstate = state;
            member->public_data.memberstate = group_member_status(state);
            member->public_data.result = result;
            advance_version(record->snapshot_version);
            if (broken_connection && previous_state == SRTS_CONNECTED
                && terminal_member_state(state)) {
                const bool still_usable = std::any_of(
                    record->members.begin(), record->members.end(),
                    [socket, member_generation](const auto& candidate) {
                        return (candidate.public_data.id != socket
                                || candidate.generation
                                    != member_generation)
                            && !terminal_member_state(
                                candidate.public_data.sockstate);
                    });
                if (still_usable) {
                    advance_version(record->update_version);
                }
            }
            changed = true;
        }
    }
    if (changed) {
        ReadinessSignal::notify(*record->readiness_source);
    }
}

void GroupRegistry::remove_member(
    SRTSOCKET group, std::uint64_t group_generation,
    SRTSOCKET socket, std::uint64_t member_generation) noexcept
{
    const auto record = find(group);
    if (record == nullptr) {
        return;
    }
    bool changed = false;
    {
        std::lock_guard lock(record->mutex);
        if (record->closed
            || record->generation != group_generation) {
            return;
        }
        const auto previous = record->members.size();
        record->members.erase(std::remove_if(
            record->members.begin(), record->members.end(),
            [socket, member_generation](const auto& candidate) {
                return candidate.public_data.id == socket
                    && candidate.generation == member_generation;
            }), record->members.end());
        changed = record->members.size() != previous;
        if (changed) {
            if (record->active_send_member == socket
                && record->active_send_generation
                    == member_generation) {
                record->active_send_member = SRT_INVALID_SOCK;
                record->active_send_generation = 0;
                record->active_send_since_microseconds = 0;
            }
            if (record->probe_send_member == socket
                && record->probe_send_generation
                    == member_generation) {
                record->probe_send_member = SRT_INVALID_SOCK;
                record->probe_send_generation = 0;
                record->probe_send_since_microseconds = 0;
                record->probe_send_start_sequence = 0;
            }
            ++record->snapshot_version;
        }
    }
    if (changed) {
        ReadinessSignal::notify(*record->readiness_source);
    }
}

bool GroupRegistry::set_peer_group(
    SRTSOCKET group, std::uint64_t group_generation,
    SRTSOCKET peer_group) noexcept
{
    if (!is_group_handle(peer_group)) {
        return false;
    }
    const auto record = find(group);
    if (record == nullptr) {
        return false;
    }
    std::lock_guard lock(record->mutex);
    if (record->closed
        || record->generation != group_generation) {
        return false;
    }
    if (record->peer_group != SRT_INVALID_SOCK
        && record->peer_group != peer_group) {
        return false;
    }
    record->peer_group = peer_group;
    return true;
}

void GroupRegistry::close(SRTSOCKET group) noexcept
{
    const auto record = find(group);
    if (record == nullptr) {
        return;
    }
    std::vector<GroupMemberSnapshot> members;
    {
        // Mark closure under the metadata lock only. A group send blocked in
        // its readiness wait holds `send_mutex`; taking it here would wait
        // for that send to finish on its own, which never happens while the
        // peer keeps the member buffers full.
        std::lock_guard lock(record->mutex);
        if (record->closed) {
            return;
        }
        record->closed = true;
        record->receive_clock.reset();
        (void)record->member_native_options.set_passphrase({});
        record->member_native_options = {};
        members.swap(record->members);
        ++record->snapshot_version;
    }
    // Wake blocked group I/O so it observes the closure and returns before
    // the members go away underneath it.
    ReadinessSignal::notify();
    for (const auto& member : members) {
        SocketRegistry::instance().close(
            member.public_data.id);
    }
    {
        // Replay history is owned by the send coordinator. In-flight shared
        // owners may outlive registry retirement; release its storage once
        // the coordinator has left.
        std::lock_guard send_lock(record->send_mutex);
        std::lock_guard lock(record->mutex);
        record->replay_history.release_storage();
    }
    {
        std::lock_guard lock(mutex_);
        const auto entry = groups_.find(group);
        if (entry != groups_.end() && entry->second == record) {
            closed_handles_.remember(group);
            groups_.erase(entry);
        }
    }
    ReadinessSignal::notify();
}

void GroupRegistry::clear() noexcept
{
    std::unordered_map<SRTSOCKET, std::shared_ptr<GroupRecord>> records;
    {
        std::lock_guard lock(mutex_);
        clearing_ = true;
        records.swap(groups_);
        closed_handles_.clear();
    }
    for (const auto& entry : records) {
        auto& record = *entry.second;
        std::lock_guard lock(record.mutex);
        record.closed = true;
        record.receive_clock.reset();
        (void)record.member_native_options.set_passphrase({});
        record.member_native_options = {};
        record.members.clear();
        ++record.snapshot_version;
    }
    // Blocked group I/O must observe the closure before the send coordinator
    // lock can be taken to release replay storage.
    ReadinessSignal::notify();
    for (const auto& entry : records) {
        auto& record = *entry.second;
        std::lock_guard send_lock(record.send_mutex);
        std::lock_guard lock(record.mutex);
        record.replay_history.release_storage();
    }
    {
        std::lock_guard lock(mutex_);
        clearing_ = false;
    }
    ReadinessSignal::notify();
}

} // namespace robotweax::srt::compat
