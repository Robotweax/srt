#include "compat/message_io.hpp"

#include "compat/error_state.hpp"
#include "compat/group_registry.hpp"
#include "compat/readiness.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <vector>

namespace robotweax::srt::compat {
namespace {

[[nodiscard]] int fail(SRT_ERRNO error, int system_error = 0) noexcept
{
    set_last_error(error, system_error);
    return SRT_ERROR;
}

[[nodiscard]] int map_send_result(
    const MessageIoResult& result,
    SRT_MSGCTRL* control) noexcept
{
    switch (result.status) {
    case MessageIoStatus::success:
        if (control != nullptr) {
            control->msgno =
                static_cast<std::int32_t>(result.message_number);
        }
        return static_cast<int>(result.bytes);
    case MessageIoStatus::would_block:
        return fail(SRT_EASYNCSND);
    case MessageIoStatus::timeout:
        return fail(SRT_ETIMEOUT);
    case MessageIoStatus::local_closed:
        return fail(SRT_ESCLOSED);
    case MessageIoStatus::peer_closed:
    case MessageIoStatus::broken:
        return fail(SRT_ECONNLOST, result.system_error);
    case MessageIoStatus::peer_error:
        return fail(SRT_EPEERERR);
    case MessageIoStatus::buffer_too_small:
        return fail(SRT_ELARGEMSG);
    case MessageIoStatus::invalid_state:
        return fail(SRT_EINVOP);
    }
    return fail(SRT_EUNKNOWN);
}

[[nodiscard]] int map_receive_result(
    const MessageIoResult& result,
    SRT_MSGCTRL* control) noexcept
{
    switch (result.status) {
    case MessageIoStatus::success:
        if (control != nullptr) {
            control->srctime = result.source_time_microseconds;
            control->pktseq = static_cast<std::int32_t>(
                result.first_sequence.value());
            control->msgno =
                static_cast<std::int32_t>(result.message_number);
        }
        return static_cast<int>(result.bytes);
    case MessageIoStatus::peer_closed:
        return fail(SRT_ECONNLOST);
    case MessageIoStatus::peer_error:
        return fail(SRT_EPEERERR);
    case MessageIoStatus::would_block:
        return fail(SRT_EASYNCRCV);
    case MessageIoStatus::timeout:
        return fail(SRT_ETIMEOUT);
    case MessageIoStatus::local_closed:
        return fail(SRT_ESCLOSED);
    case MessageIoStatus::broken:
        return fail(SRT_ECONNLOST, result.system_error);
    case MessageIoStatus::buffer_too_small:
        return fail(SRT_ELARGEMSG);
    case MessageIoStatus::invalid_state:
        return fail(SRT_EINVOP);
    }
    return fail(SRT_EUNKNOWN);
}

[[nodiscard]] bool valid_single_socket_control(
    const SRT_MSGCTRL& control) noexcept
{
    return control.flags == 0
        && control.boundary == 0
        && control.grpdata == nullptr
        && control.grpdata_size == 0U;
}

[[nodiscard]] bool valid_group_control(
    const SRT_MSGCTRL& control) noexcept
{
    return control.flags == 0 && control.boundary == 0;
}

struct GroupIoMember {
    SRTSOCKET id = SRT_INVALID_SOCK;
    std::uint64_t generation = 0;
    std::uint16_t weight = 0;
    std::shared_ptr<ConnectionRuntime> runtime;
    std::int32_t maximum_payload_size = 0;
    bool message_api = true;
    bool tsbpd_mode = true;
    bool terminal = false;
    RuntimeResponseHealth response_health {};
};

struct FailedGroupIoMember {
    GroupIoMember member;
    MessageIoResult result;
};

[[nodiscard]] std::vector<GroupIoMember> group_members(
    const std::shared_ptr<GroupRecord>& group,
    bool include_terminal_receivers = false)
{
    std::vector<GroupMemberSnapshot> snapshots;
    {
        std::lock_guard lock(group->mutex);
        snapshots = group->members;
    }
    std::vector<GroupIoMember> result;
    result.reserve(snapshots.size());
    for (const auto& snapshot : snapshots) {
        if (snapshot.public_data.sockstate != SRTS_CONNECTED
            && (!include_terminal_receivers
                || snapshot.public_data.sockstate != SRTS_BROKEN)) {
            continue;
        }
        const auto socket =
            SocketRegistry::instance().find(snapshot.public_data.id);
        if (socket == nullptr) {
            continue;
        }
        GroupIoMember member;
        member.id = snapshot.public_data.id;
        member.generation = snapshot.generation;
        member.weight = snapshot.public_data.weight;
        {
            std::lock_guard lock(socket->mutex);
            if ((socket->state != SRTS_CONNECTED
                    && (!include_terminal_receivers
                        || socket->state != SRTS_BROKEN))
                || socket->runtime == nullptr
                || socket->group_id != group->handle
                || socket->group_generation != group->generation
                || socket->member_generation != snapshot.generation) {
                continue;
            }
            member.runtime = socket->runtime;
            member.maximum_payload_size =
                socket->public_options.maximum_payload_size;
            member.message_api = socket->public_options.message_api;
            member.tsbpd_mode = socket->public_options.tsbpd_mode;
            member.terminal = socket->state == SRTS_BROKEN;
        }
        result.push_back(std::move(member));
    }
    return result;
}

[[nodiscard]] std::uint64_t backup_stability_timeout(
    const GroupIoMember& member,
    std::int32_t minimum_milliseconds) noexcept
{
    const std::uint64_t minimum =
        static_cast<std::uint64_t>(minimum_milliseconds) * 1'000U;
    const std::uint64_t rtt_timeout =
        2ULL * member.response_health.smoothed_rtt_microseconds
        + 4ULL * member.response_health.rtt_variation_microseconds;
    std::uint64_t result = std::max(minimum, rtt_timeout);
    if (member.response_health.peer_latency_microseconds != 0U) {
        result = std::min(result,
            static_cast<std::uint64_t>(
                member.response_health.peer_latency_microseconds));
    }
    if (member.response_health.peer_idle_timeout_microseconds != 0U) {
        result = std::min(result,
            member.response_health.peer_idle_timeout_microseconds);
    }
    return result;
}

struct BackupSendPlan {
    SRTSOCKET probe_member = SRT_INVALID_SOCK;
    std::uint64_t probe_generation = 0;
};

void acknowledge_backup_replay(
    const std::vector<GroupIoMember>& members,
    const std::shared_ptr<GroupRecord>& group) noexcept
{
    SRTSOCKET active_id = SRT_INVALID_SOCK;
    std::uint64_t active_generation = 0;
    SRTSOCKET probe_id = SRT_INVALID_SOCK;
    std::uint64_t probe_generation = 0;
    SequenceNumber acknowledged{};
    {
        std::lock_guard lock(group->mutex);
        active_id = group->active_send_member;
        active_generation = group->active_send_generation;
        probe_id = group->probe_send_member;
        probe_generation = group->probe_send_generation;
        acknowledged = SequenceNumber{
            group->replay_acknowledged_sequence};
    }

    const auto active = std::find_if(
        members.begin(), members.end(),
        [active_id, active_generation](const auto& member) {
            return member.id == active_id
                && member.generation == active_generation;
        });
    if (active == members.end()) {
        return;
    }
    const auto active_health = active->runtime->response_health();
    std::int32_t common_progress = std::max(
        0, active_health.acknowledged_sequence.distance_from(
               acknowledged));

    const auto probe = std::find_if(
        members.begin(), members.end(),
        [probe_id, probe_generation](const auto& member) {
            return member.id == probe_id
                && member.generation == probe_generation;
        });
    if (probe != members.end()) {
        const auto probe_health = probe->runtime->response_health();
        common_progress = std::min(common_progress,
            std::max(0,
                probe_health.acknowledged_sequence.distance_from(
                    acknowledged)));
    }
    if (common_progress == 0) {
        return;
    }

    acknowledged = acknowledged.advanced(
        static_cast<std::uint32_t>(common_progress));
    group->replay_history.acknowledge_before(acknowledged);
    {
        std::lock_guard lock(group->mutex);
        group->replay_acknowledged_sequence = acknowledged.value();
    }
}

[[nodiscard]] MessageIoResult prepare_backup_member(
    const GroupIoMember& member,
    const std::shared_ptr<GroupRecord>& group,
    SequenceNumber target_sequence) noexcept
{
    const RuntimeResponseHealth health =
        member.runtime->response_health();
    SequenceNumber acknowledged{};
    {
        std::lock_guard lock(group->mutex);
        acknowledged = SequenceNumber{
            group->replay_acknowledged_sequence};
    }

    SequenceNumber cursor = health.next_send_sequence;
    if (health.send_buffer_empty) {
        // A late member may already have been created at the current group
        // sequence. Otherwise an empty sender can be safely synchronized to
        // the oldest sequence that the group has not cumulatively retired.
        // The member receiver has not observed the retired prefix, so advance
        // it with an ordered DROPREQ before this path sends its first DATA.
        const std::int32_t member_progress =
            cursor.distance_from(acknowledged);
        if (member_progress < 0) {
            const auto skipped =
                member.runtime->skip_group_sequences(acknowledged);
            if (skipped.status != MessageIoStatus::success) {
                return skipped;
            }
            cursor = skipped.next_sequence;
        }
    } else if (cursor.distance_from(acknowledged) < 0) {
        return {.status = MessageIoStatus::invalid_state};
    }

    if (cursor == target_sequence) {
        return {
            .status = MessageIoStatus::success,
            .next_sequence = target_sequence,
        };
    }
    if (target_sequence.distance_from(cursor) <= 0) {
        return {.status = MessageIoStatus::invalid_state};
    }

    std::array<std::byte, maximum_data_payload_size> scratch{};
    GroupReplayBuffer::Cursor replay_cursor;
    if (!group->replay_history.begin_at(cursor, replay_cursor)) {
        return {.status = MessageIoStatus::invalid_state};
    }
    while (cursor != target_sequence) {
        GroupReplayMessage replay;
        std::size_t payload_size = 0;
        if (!group->replay_history.copy_next(
                replay_cursor, scratch, replay, payload_size)
            || replay.first_sequence != cursor
            || replay.next_sequence.distance_from(target_sequence) > 0) {
            // The bounded history no longer covers this member's gap. Sending
            // newer data would create an unrecoverable hole at the receiver.
            return {.status = MessageIoStatus::invalid_state};
        }
        const auto result = member.runtime->queue_group_message(
            std::span {scratch}.first(payload_size), replay.first_sequence,
            replay.message_number, replay.source_time_microseconds,
            replay.in_order, replay.ttl_milliseconds);
        if (result.status != MessageIoStatus::success) {
            return result;
        }
        if (result.next_sequence != replay.next_sequence) {
            return {.status = MessageIoStatus::invalid_state};
        }
        cursor = result.next_sequence;
    }
    return {
        .status = MessageIoStatus::success,
        .next_sequence = target_sequence,
    };
}

void clear_backup_probe(GroupRecord& group) noexcept
{
    group.probe_send_member = SRT_INVALID_SOCK;
    group.probe_send_generation = 0;
    group.probe_send_since_microseconds = 0;
    group.probe_send_start_sequence = 0;
}

[[nodiscard]] BackupSendPlan order_backup_members(
    std::vector<GroupIoMember>& members,
    const std::shared_ptr<GroupRecord>& group,
    SequenceNumber first_sequence) noexcept
{
    const auto preferred = [](const GroupIoMember& left,
                               const GroupIoMember& right) {
        return left.weight != right.weight
            ? left.weight > right.weight
            : left.id < right.id;
    };
    std::sort(members.begin(), members.end(), preferred);

    SRTSOCKET active_id = SRT_INVALID_SOCK;
    std::uint64_t active_generation = 0;
    std::uint64_t active_since = 0;
    SRTSOCKET probe_id = SRT_INVALID_SOCK;
    std::uint64_t probe_generation = 0;
    std::uint64_t probe_since = 0;
    SequenceNumber probe_start{};
    std::int32_t minimum_milliseconds = 60;
    {
        std::lock_guard lock(group->mutex);
        active_id = group->active_send_member;
        active_generation = group->active_send_generation;
        active_since = group->active_send_since_microseconds;
        probe_id = group->probe_send_member;
        probe_generation = group->probe_send_generation;
        probe_since = group->probe_send_since_microseconds;
        probe_start = SequenceNumber{
            group->probe_send_start_sequence};
        minimum_milliseconds =
            group->minimum_stability_timeout_milliseconds;
    }
    const auto active = std::find_if(
        members.begin(), members.end(),
        [active_id, active_generation](const auto& member) {
            return member.id == active_id
                && member.generation == active_generation;
        });
    if (active == members.end()) {
        std::lock_guard lock(group->mutex);
        clear_backup_probe(*group);
        return {};
    }

    active->response_health = active->runtime->response_health();

    const std::uint64_t last_response = std::max(
        active_since, active->response_health.last_response_microseconds);
    const std::uint64_t now = active->response_health.now_microseconds;
    const bool responsive = now <= last_response
        || now - last_response
            <= backup_stability_timeout(*active, minimum_milliseconds);
    // Keep the current authority first even after its response deadline. A
    // locally accepted send is not evidence that another path recovered, so
    // failover and failback both use the same parallel ACK-qualified probe.
    // This also prevents an unreachable higher-weight path from repeatedly
    // stealing authority from a healthy replacement merely because its local
    // send buffer still accepts packets.
    std::rotate(members.begin(), active, std::next(active));

    auto probe = std::find_if(members.begin(), members.end(),
        [probe_id, probe_generation](const auto& member) {
            return member.id == probe_id
                && member.generation == probe_generation;
        });
    if (probe != members.end()) {
        probe->response_health = probe->runtime->response_health();
        const std::uint64_t timeout =
            backup_stability_timeout(*probe, minimum_milliseconds);
        const std::uint64_t probe_now = probe->response_health.now_microseconds;
        const bool elapsed =
            probe_now >= probe_since && probe_now - probe_since >= timeout;
        const bool acknowledged =
            probe->response_health.acknowledged_sequence.distance_from(
                probe_start)
            > 0;
        const std::uint64_t last_probe_response =
            probe->response_health.last_response_microseconds;
        const bool response_recent = probe_now <= last_probe_response
            || probe_now - last_probe_response <= timeout;
        if (elapsed && acknowledged && response_recent) {
            const SRTSOCKET promoted_id = probe->id;
            const std::uint64_t promoted_generation = probe->generation;
            bool promoted = false;
            {
                std::lock_guard lock(group->mutex);
                if (!group->closed && group->active_send_member == active_id
                    && group->active_send_generation == active_generation
                    && group->probe_send_member == promoted_id
                    && group->probe_send_generation == promoted_generation) {
                    group->active_send_member = promoted_id;
                    group->active_send_generation = promoted_generation;
                    group->active_send_since_microseconds = probe_now;
                    clear_backup_probe(*group);
                    promoted = true;
                }
            }
            if (promoted) {
                probe = std::find_if(members.begin(), members.end(),
                    [promoted_id, promoted_generation](const auto& member) {
                        return member.id == promoted_id
                            && member.generation == promoted_generation;
                    });
                std::rotate(members.begin(), probe, std::next(probe));
                return {};
            }

            // A concurrent group update invalidated the qualification
            // snapshot. Keep the old active member authoritative for this
            // send; the next call will build a fresh plan.
            return {};
        }

        probe = std::find_if(
            members.begin(), members.end(),
            [probe_id, probe_generation](const auto& member) {
                return member.id == probe_id
                    && member.generation == probe_generation;
            });
        if (probe != members.end() && probe != std::next(members.begin())) {
            std::rotate(
                std::next(members.begin()), probe, std::next(probe));
        }
        return {
            .probe_member = probe_id,
            .probe_generation = probe_generation,
        };
    }

    // A better path is first exercised in parallel while an active path is
    // responsive. Once the active response deadline expires, exercise the
    // best available alternative even when its configured weight is lower.
    // In either case the candidate must acknowledge group DATA and remain
    // responsive for one complete stability interval before promotion.
    const auto candidate = responsive
        ? std::find_if(std::next(members.begin()), members.end(),
              [&members, &preferred](const auto& member) {
                  return preferred(member, members.front());
              })
        : std::next(members.begin());
    if (candidate == members.end()) {
        std::lock_guard lock(group->mutex);
        clear_backup_probe(*group);
        return {};
    }
    const auto candidate_health = candidate->runtime->response_health();
    {
        std::lock_guard lock(group->mutex);
        if (group->closed
            || group->active_send_member != active_id
            || group->active_send_generation != active_generation) {
            return {};
        }
        group->probe_send_member = candidate->id;
        group->probe_send_generation = candidate->generation;
        group->probe_send_since_microseconds =
            candidate_health.now_microseconds;
        group->probe_send_start_sequence = first_sequence.value();
    }
    const SRTSOCKET candidate_id = candidate->id;
    const std::uint64_t candidate_generation = candidate->generation;
    auto relocated = std::find_if(
        members.begin(), members.end(),
        [candidate_id, candidate_generation](const auto& member) {
            return member.id == candidate_id
                && member.generation == candidate_generation;
        });
    if (relocated != std::next(members.begin())) {
        std::rotate(
            std::next(members.begin()), relocated, std::next(relocated));
    }
    return {
        .probe_member = candidate_id,
        .probe_generation = candidate_generation,
    };
}

[[nodiscard]] SRT_ERRNO result_error(
    const MessageIoResult& result, bool sending) noexcept
{
    switch (result.status) {
    case MessageIoStatus::would_block:
        return sending ? SRT_EASYNCSND : SRT_EASYNCRCV;
    case MessageIoStatus::timeout:
        return SRT_ETIMEOUT;
    case MessageIoStatus::local_closed:
        return SRT_ESCLOSED;
    case MessageIoStatus::peer_closed:
    case MessageIoStatus::broken:
        return SRT_ECONNLOST;
    case MessageIoStatus::peer_error:
        return SRT_EPEERERR;
    case MessageIoStatus::buffer_too_small:
        return SRT_ELARGEMSG;
    case MessageIoStatus::invalid_state:
        return SRT_EINVOP;
    case MessageIoStatus::success:
        return SRT_SUCCESS;
    }
    return SRT_EUNKNOWN;
}

void fill_group_data(
    const std::shared_ptr<GroupRecord>& group,
    SRT_MSGCTRL* control,
    SRT_SOCKGROUPDATA* requested_data,
    std::size_t requested_capacity,
    bool report_size_without_buffer) noexcept
{
    if (control == nullptr) {
        return;
    }
    control->grpdata = nullptr;
    control->grpdata_size = 0U;
    if (requested_data == nullptr && !report_size_without_buffer) {
        return;
    }
    std::lock_guard lock(group->mutex);
    control->grpdata_size = group->members.size();
    if (requested_data == nullptr
        || requested_capacity < group->members.size()) {
        return;
    }
    std::transform(group->members.begin(), group->members.end(),
        requested_data, [](const GroupMemberSnapshot& member) {
            return member.public_data;
        });
    control->grpdata = requested_data;
}

[[nodiscard]] ReadinessSignal::Clock::time_point io_deadline(
    std::int32_t timeout_milliseconds) noexcept
{
    return timeout_milliseconds < 0
        ? ReadinessSignal::Clock::time_point::max()
        : ReadinessSignal::Clock::now()
            + std::chrono::milliseconds{timeout_milliseconds};
}

} // namespace

int send_message(
    SocketRecord* socket,
    const char* buffer,
    int length,
    SRT_MSGCTRL* control) noexcept
{
    if (buffer == nullptr || length <= 0) {
        return fail(SRT_EINVPARAM);
    }
    if (socket == nullptr) {
        return fail(SRT_EINVSOCK);
    }

    SRT_MSGCTRL local_control = control == nullptr
        ? srt_msgctrl_default
        : *control;
    if (!valid_single_socket_control(local_control)
        || local_control.srctime < 0) {
        return fail(SRT_EINVALMSGAPI);
    }
    std::shared_ptr<ConnectionRuntime> runtime;
    bool blocking = true;
    std::int32_t timeout_milliseconds = -1;
    std::int32_t maximum_payload_size = 0;
    bool message_api = true;
    bool tsbpd_mode = true;
    {
        std::lock_guard lock(socket->mutex);
        if (socket->state == SRTS_CLOSED) {
            return fail(SRT_EINVSOCK);
        }
        if (socket->state == SRTS_BROKEN) {
            return fail(SRT_ECONNLOST);
        }
        if (socket->state != SRTS_CONNECTED
            || socket->runtime == nullptr) {
            return fail(SRT_ENOCONN);
        }
        runtime = socket->runtime;
        blocking = socket->public_options.send_synchronous;
        timeout_milliseconds =
            socket->public_options.send_timeout_milliseconds;
        maximum_payload_size =
            socket->public_options.maximum_payload_size;
        message_api =
            socket->public_options.message_api;
        tsbpd_mode =
            socket->public_options.tsbpd_mode;
    }
    if (message_api && tsbpd_mode
        && length > maximum_payload_size) {
        return fail(SRT_EINVALMSGAPI);
    }

    const auto bytes = std::as_bytes(std::span{
        buffer, static_cast<std::size_t>(length)});
    const auto result = message_api
        ? runtime->queue_message(bytes,
              local_control.srctime,
              local_control.inorder != 0,
              blocking, timeout_milliseconds,
              local_control.msgttl)
        : runtime->queue_stream(bytes, blocking,
              timeout_milliseconds);
    return map_send_result(result,
        control);
}

int receive_message(
    SocketRecord* socket,
    char* buffer,
    int length,
    SRT_MSGCTRL* control) noexcept
{
    if (buffer == nullptr || length <= 0) {
        return fail(SRT_EINVPARAM);
    }
    if (socket == nullptr) {
        return fail(SRT_EINVSOCK);
    }
    if (control != nullptr
        && !valid_single_socket_control(*control)) {
        return fail(SRT_EINVALMSGAPI);
    }

    std::shared_ptr<ConnectionRuntime> runtime;
    bool blocking = true;
    std::int32_t timeout_milliseconds = -1;
    std::int32_t maximum_payload_size = 0;
    bool message_api = true;
    bool tsbpd_mode = true;
    {
        std::lock_guard lock(socket->mutex);
        if (socket->state == SRTS_CLOSED) {
            return fail(SRT_EINVSOCK);
        }
        // A terminal peer state does not invalidate already buffered input.
        // Let the runtime drain it before exposing message ECONNLOST or
        // stream EOF to the application.
        if (socket->state == SRTS_BROKEN && socket->runtime == nullptr) {
            return fail(SRT_ECONNLOST);
        }
        if ((socket->state != SRTS_CONNECTED && socket->state != SRTS_BROKEN)
            || socket->runtime == nullptr) {
            return fail(SRT_ENOCONN);
        }
        runtime = socket->runtime;
        blocking = socket->public_options.receive_synchronous;
        timeout_milliseconds =
            socket->public_options.receive_timeout_milliseconds;
        maximum_payload_size = socket->public_options.maximum_payload_size;
        message_api = socket->public_options.message_api;
        tsbpd_mode = socket->public_options.tsbpd_mode;
    }
    // Haivision's Live Message API validates the application buffer against
    // the configured maximum payload before inspecting the receive queue.
    // Reject without consuming the due message so a correctly sized retry
    // can still drain it, including after peer shutdown.
    if (message_api && tsbpd_mode && length < maximum_payload_size) {
        return fail(SRT_EINVALMSGAPI);
    }

    auto bytes = std::as_writable_bytes(std::span{
        buffer, static_cast<std::size_t>(length)});
    const auto result = message_api
        ? runtime->receive_message(bytes, blocking,
              timeout_milliseconds)
        : runtime->receive_stream(bytes, blocking,
              timeout_milliseconds);
    return map_receive_result(result, control);
}

int send_group_message_implementation(
    const std::shared_ptr<GroupRecord>& group,
    const char* buffer, int length,
    SRT_MSGCTRL* control)
{
    if (buffer == nullptr || length <= 0) {
        return fail(SRT_EINVPARAM);
    }
    if (group == nullptr) {
        return fail(SRT_EINVSOCK);
    }
    SRT_MSGCTRL local_control = control == nullptr
        ? srt_msgctrl_default : *control;
    if (!valid_group_control(local_control)
        || local_control.srctime < 0) {
        return fail(SRT_EINVALMSGAPI);
    }
    if (local_control.srctime == 0) {
        local_control.srctime =
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count();
    }
    SRT_SOCKGROUPDATA* const requested_data =
        local_control.grpdata;
    const std::size_t requested_capacity =
        local_control.grpdata_size;

    std::unique_lock io_lock(group->send_mutex);
    bool blocking = true;
    std::int32_t timeout = -1;
    std::uint64_t generation = 0;
    SRT_GROUP_TYPE group_type = SRT_GTYPE_UNDEFINED;
    {
        std::lock_guard lock(group->mutex);
        if (group->closed) {
            return fail(SRT_ESCLOSED);
        }
        if (group->type != SRT_GTYPE_BROADCAST
            && group->type != SRT_GTYPE_BACKUP) {
            return fail(SRT_EINVOP);
        }
        group_type = group->type;
        blocking = group->send_synchronous;
        timeout = group->send_timeout_milliseconds;
        generation = group->generation;
    }
    const auto deadline = io_deadline(timeout);
    const auto bytes = std::as_bytes(std::span{
        buffer, static_cast<std::size_t>(length)});

    for (;;) {
        const std::uint64_t observed = ReadinessSignal::generation();
        auto members = group_members(group);
        if (members.empty()) {
            return fail(SRT_ENOCONN);
        }
        if (group_type == SRT_GTYPE_BACKUP) {
            acknowledge_backup_replay(members, group);
        }
        std::uint32_t first_sequence = 0;
        std::uint32_t message_number = 0;
        {
            std::lock_guard lock(group->mutex);
            if (group->closed || group->generation != generation) {
                return fail(SRT_ESCLOSED);
            }
            first_sequence = group->next_send_sequence;
            message_number = group->next_send_message;
        }
        BackupSendPlan backup_plan;
        if (group_type == SRT_GTYPE_BACKUP) {
            // Backup policy belongs to the group coordinator, not to the
            // member transports. The initial choice is weight ordered. A
            // responsive active member stays sticky, while a member that has
            // exceeded its RTT-derived response timeout is moved behind the
            // weight-ordered alternatives.
            backup_plan = order_backup_members(
                members, group, SequenceNumber{first_sequence});
        }
        if (std::any_of(members.begin(), members.end(),
                [length](const GroupIoMember& member) {
                    return !member.message_api || !member.tsbpd_mode
                        || length > member.maximum_payload_size;
                })) {
            return fail(SRT_EINVALMSGAPI);
        }
        std::vector<FailedGroupIoMember> failed_members;
        std::vector<std::pair<SRTSOCKET, std::uint64_t>>
            successful_members;
        bool succeeded = false;
        bool would_block = false;
        bool hard_failure = false;
        MessageIoResult first_failure{
            .status = MessageIoStatus::invalid_state};
        bool have_failure = false;
        SRTSOCKET successful_member = SRT_INVALID_SOCK;
        std::uint64_t successful_generation = 0;
        SequenceNumber next_sequence{first_sequence};
        for (const auto& member : members) {
            MessageIoResult result;
            if (group_type == SRT_GTYPE_BACKUP) {
                result = prepare_backup_member(
                    member, group, SequenceNumber{first_sequence});
            }
            if (group_type != SRT_GTYPE_BACKUP
                || result.status == MessageIoStatus::success) {
                result = member.runtime->queue_group_message(bytes,
                    SequenceNumber {first_sequence}, message_number,
                    local_control.srctime, local_control.inorder != 0,
                    local_control.msgttl);
            }
            if (result.status == MessageIoStatus::success) {
                if (!succeeded) {
                    next_sequence = result.next_sequence;
                    successful_member = member.id;
                    successful_generation = member.generation;
                } else if (result.next_sequence != next_sequence) {
                    result.status = MessageIoStatus::invalid_state;
                    failed_members.push_back({member, result});
                    continue;
                }
                succeeded = true;
                successful_members.emplace_back(
                    member.id, member.generation);
                GroupRegistry::instance().note_io_result(
                    group->handle, generation, member.id,
                    member.generation, SRT_GST_RUNNING, length);
                if (group_type == SRT_GTYPE_BACKUP) {
                    const bool is_probe =
                        member.id == backup_plan.probe_member
                        && member.generation
                            == backup_plan.probe_generation;
                    if (backup_plan.probe_member == SRT_INVALID_SOCK
                        || is_probe) {
                        break;
                    }
                    continue;
                }
                continue;
            }
            if (result.status == MessageIoStatus::would_block) {
                would_block = true;
            } else {
                hard_failure = true;
            }
            if (!have_failure
                || (first_failure.status == MessageIoStatus::would_block
                    && result.status != MessageIoStatus::would_block)) {
                first_failure = result;
                have_failure = true;
            }
            failed_members.push_back({member, result});
        }

        if (succeeded) {
            bool committed = false;
            {
                std::lock_guard lock(group->mutex);
                if (!group->closed && group->generation == generation
                    && group->next_send_sequence == first_sequence
                    && group->next_send_message == message_number) {
                    group->next_send_sequence = next_sequence.value();
                    group->next_send_message =
                        (message_number + 1U) & 0x03ff'ffffU;
                    if (group->next_send_message == 0U) {
                        group->next_send_message = 1U;
                    }
                    committed = true;
                }
            }
            if (committed && group_type == SRT_GTYPE_BACKUP) {
                const auto appended = group->replay_history.append(
                    bytes,
                    {
                        .first_sequence =
                            SequenceNumber{first_sequence},
                        .next_sequence = next_sequence,
                        .message_number = message_number,
                        .source_time_microseconds =
                            local_control.srctime,
                        .ttl_milliseconds = local_control.msgttl,
                        .in_order = local_control.inorder != 0,
                    });
                if (appended != GroupReplayBuffer::AppendResult::success) {
                    // The message has already been accepted by a member. Keep
                    // normal delivery intact, but make future failover fail
                    // closed if this missing range is still required.
                    group->replay_history.clear();
                }
            }
            for (const auto& failure : failed_members) {
                const bool failed_probe =
                    group_type == SRT_GTYPE_BACKUP
                    && failure.member.id == backup_plan.probe_member
                    && failure.member.generation
                        == backup_plan.probe_generation;
                if (group_type == SRT_GTYPE_BACKUP
                    && failure.result.status
                        == MessageIoStatus::would_block) {
                    GroupRegistry::instance().note_io_result(
                        group->handle, generation, failure.member.id,
                        failure.member.generation, SRT_GST_IDLE,
                        SRT_EASYNCSND);
                    if (failed_probe && succeeded) {
                        std::lock_guard lock(group->mutex);
                        if (group->probe_send_member
                                == failure.member.id
                            && group->probe_send_generation
                                == failure.member.generation) {
                            clear_backup_probe(*group);
                        }
                    }
                    continue;
                }
                if (failed_probe && succeeded
                    && failure.result.status
                        == MessageIoStatus::invalid_state) {
                    GroupRegistry::instance().note_io_result(
                        group->handle, generation, failure.member.id,
                        failure.member.generation, SRT_GST_IDLE,
                        SRT_EINVOP);
                    std::lock_guard lock(group->mutex);
                    if (group->probe_send_member == failure.member.id
                        && group->probe_send_generation
                            == failure.member.generation) {
                        clear_backup_probe(*group);
                    }
                    continue;
                }
                GroupRegistry::instance().note_io_result(group->handle,
                    generation, failure.member.id, failure.member.generation,
                    SRT_GST_BROKEN, result_error(failure.result, true));
                SocketRegistry::instance().close_failed(failure.member.id);
            }
            if (group_type == SRT_GTYPE_BACKUP) {
                const auto selected =
                    std::find_if(members.begin(), members.end(),
                        [successful_member, successful_generation](
                            const auto& member) {
                            return member.id == successful_member
                                && member.generation == successful_generation;
                        });
                const bool selected_unqualified_probe =
                    backup_plan.probe_member != SRT_INVALID_SOCK
                    && successful_member == backup_plan.probe_member
                    && successful_generation == backup_plan.probe_generation;
                if (selected != members.end() && !selected_unqualified_probe) {
                    const std::uint64_t selected_now =
                        selected->runtime->response_health().now_microseconds;
                    std::lock_guard lock(group->mutex);
                    if (!group->closed && group->generation == generation) {
                        const bool changed =
                            group->active_send_member != successful_member
                            || group->active_send_generation
                                != successful_generation;
                        group->active_send_member = successful_member;
                        group->active_send_generation = successful_generation;
                        if (changed) {
                            group->active_send_since_microseconds =
                                selected_now;
                            clear_backup_probe(*group);
                        }
                    }
                }
                for (const auto& member : members) {
                    const bool member_succeeded = std::any_of(
                        successful_members.begin(),
                        successful_members.end(),
                        [&member](const auto& candidate) {
                            return candidate.first == member.id
                                && candidate.second
                                    == member.generation;
                        });
                    if (member_succeeded) {
                        continue;
                    }
                    const bool failed = std::any_of(
                        failed_members.begin(), failed_members.end(),
                        [&member](const FailedGroupIoMember& candidate) {
                            return candidate.member.id == member.id
                                && candidate.member.generation
                                    == member.generation;
                        });
                    if (!failed) {
                        GroupRegistry::instance().note_io_result(
                            group->handle, generation, member.id,
                            member.generation, SRT_GST_IDLE,
                            SRT_SUCCESS);
                    }
                }
            }
            if (control != nullptr) {
                control->srctime = local_control.srctime;
                control->pktseq = static_cast<std::int32_t>(first_sequence);
                control->msgno = static_cast<std::int32_t>(message_number);
            }
            fill_group_data(group, control,
                requested_data, requested_capacity, true);
            return length;
        }
        if (group_type == SRT_GTYPE_BACKUP && would_block) {
            // A replacement may have accepted only part of the bounded replay
            // range before its ordinary member send buffer filled. Let its
            // own pacer/ACK cycle create capacity, then resume from the
            // member's exact next sequence. A hard failure on the old active
            // path must not turn that recoverable catch-up into ECONNLOST.
            if (!blocking) {
                return fail(SRT_EASYNCSND);
            }
            if (ReadinessSignal::Clock::now() >= deadline) {
                return fail(SRT_ETIMEOUT);
            }
            ReadinessSignal::wait_until(observed, deadline);
            continue;
        }
        if (hard_failure || !would_block || !blocking) {
            return fail(result_error(first_failure, true),
                first_failure.system_error);
        }
        if (ReadinessSignal::Clock::now() >= deadline) {
            return fail(SRT_ETIMEOUT);
        }
        ReadinessSignal::wait_until(observed, deadline);
    }
}

int receive_group_message_implementation(
    const std::shared_ptr<GroupRecord>& group,
    char* buffer, int length,
    SRT_MSGCTRL* control)
{
    if (buffer == nullptr || length <= 0) {
        return fail(SRT_EINVPARAM);
    }
    if (group == nullptr) {
        return fail(SRT_EINVSOCK);
    }
    SRT_MSGCTRL local_control = control == nullptr
        ? srt_msgctrl_default : *control;
    if (!valid_group_control(local_control)) {
        return fail(SRT_EINVALMSGAPI);
    }
    SRT_SOCKGROUPDATA* const requested_data =
        local_control.grpdata;
    const std::size_t requested_capacity =
        local_control.grpdata_size;

    std::unique_lock io_lock(group->receive_mutex);
    bool blocking = true;
    std::int32_t timeout = -1;
    std::uint64_t generation = 0;
    SRT_GROUP_TYPE group_type = SRT_GTYPE_UNDEFINED;
    {
        std::lock_guard lock(group->mutex);
        if (group->closed) {
            return fail(SRT_ESCLOSED);
        }
        if (group->type != SRT_GTYPE_BROADCAST
            && group->type != SRT_GTYPE_BACKUP) {
            return fail(SRT_EINVOP);
        }
        group_type = group->type;
        blocking = group->receive_synchronous;
        timeout = group->receive_timeout_milliseconds;
        generation = group->generation;
    }
    const auto deadline = io_deadline(timeout);
    auto bytes = std::as_writable_bytes(std::span{
        buffer, static_cast<std::size_t>(length)});

    for (;;) {
        const std::uint64_t observed = ReadinessSignal::generation();
        const auto members = group_members(group, true);
        if (members.empty()) {
            return fail(SRT_ENOCONN);
        }
        const bool live_member = std::any_of(
            members.begin(), members.end(), [](const GroupIoMember& member) {
                return !member.terminal;
            });
        std::uint32_t expected = 0;
        {
            std::lock_guard lock(group->mutex);
            if (group->closed || group->generation != generation) {
                return fail(SRT_ESCLOSED);
            }
            expected = group->next_receive_sequence;
        }

        const GroupIoMember* selected = nullptr;
        std::optional<ReadinessSignal::Clock::time_point> next_delivery;
        for (const auto& member : members) {
            if (!member.message_api) {
                continue;
            }
            // Late members may have completed their handshake with an older
            // receive-buffer base. Advance them before looking for the next
            // logical group message so missing historical traffic cannot
            // block a newly joined redundant path.
            (void)member.runtime->discard_received_before(
                SequenceNumber {expected});
            const auto candidate =
                member.runtime->next_readable_message_sequence();
            if (!candidate.has_value()) {
                const auto member_delivery =
                    member.runtime->next_readable_deadline();
                if (member_delivery.has_value()
                    && (!next_delivery.has_value()
                        || *member_delivery < *next_delivery)) {
                    next_delivery = member_delivery;
                }
                continue;
            }
            const std::int32_t distance =
                candidate->distance_from(SequenceNumber {expected});
            if (distance < 0) {
                (void)member.runtime->discard_received_before(
                    SequenceNumber {expected});
                continue;
            }
            // A complete message beyond the logical group prefix is not
            // deliverable yet. Releasing it would expose a gap whenever a
            // replacement path starts at a later sequence than the failed
            // member. Keep it buffered until another member supplies the
            // expected message or the receive operation reaches its terminal
            // error/timeout boundary.
            if (distance == 0 && selected == nullptr) {
                selected = &member;
            }
        }
        if (selected != nullptr) {
            const auto result =
                selected->runtime->receive_message(bytes, false, -1);
            if (result.status == MessageIoStatus::success) {
                {
                    std::lock_guard lock(group->mutex);
                    if (!group->closed && group->generation == generation
                        && group->next_receive_sequence == expected) {
                        group->next_receive_sequence =
                            result.next_sequence.value();
                    }
                }
                for (const auto& member : members) {
                    if (member.id != selected->id) {
                        (void)member.runtime->discard_received_before(
                            result.next_sequence);
                    }
                }
                GroupRegistry::instance().note_io_result(group->handle,
                    generation, selected->id, selected->generation,
                    selected->terminal ? SRT_GST_BROKEN : SRT_GST_RUNNING,
                    static_cast<int>(result.bytes));
                if (group_type == SRT_GTYPE_BACKUP) {
                    for (const auto& member : members) {
                        if (!member.terminal
                            && (member.id != selected->id
                                || member.generation != selected->generation)) {
                            GroupRegistry::instance().note_io_result(
                                group->handle, generation, member.id,
                                member.generation, SRT_GST_IDLE, SRT_SUCCESS);
                        }
                    }
                }
                if (control != nullptr) {
                    control->srctime = result.source_time_microseconds;
                    control->pktseq = static_cast<std::int32_t>(
                        result.first_sequence.value());
                    control->msgno =
                        static_cast<std::int32_t>(result.message_number);
                }
                fill_group_data(
                    group, control, requested_data, requested_capacity, false);
                return static_cast<int>(result.bytes);
            }
            if (result.status == MessageIoStatus::buffer_too_small) {
                return fail(SRT_ELARGEMSG);
            }
        }
        if (!live_member && !next_delivery.has_value()) {
            return fail(SRT_ECONNLOST);
        }
        if (!blocking) {
            return fail(SRT_EASYNCRCV);
        }
        if (ReadinessSignal::Clock::now() >= deadline) {
            return fail(SRT_ETIMEOUT);
        }
        const auto wake_deadline = next_delivery.has_value()
            ? std::min(deadline, *next_delivery)
            : deadline;
        ReadinessSignal::wait_until(
            observed, wake_deadline);
    }
}

int send_group_message(
    const std::shared_ptr<GroupRecord>& group,
    const char* buffer, int length,
    SRT_MSGCTRL* control) noexcept
{
    try {
        return send_group_message_implementation(
            group, buffer, length, control);
    } catch (const std::bad_alloc&) {
        return fail(SRT_ENOBUF);
    } catch (...) {
        return fail(SRT_EUNKNOWN);
    }
}

int receive_group_message(
    const std::shared_ptr<GroupRecord>& group,
    char* buffer, int length,
    SRT_MSGCTRL* control) noexcept
{
    try {
        return receive_group_message_implementation(
            group, buffer, length, control);
    } catch (const std::bad_alloc&) {
        return fail(SRT_ENOBUF);
    } catch (...) {
        return fail(SRT_EUNKNOWN);
    }
}

} // namespace robotweax::srt::compat
