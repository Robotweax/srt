#include "compat/group_registry.hpp"

#include "compat/error_state.hpp"
#include "compat/readiness.hpp"
#include "compat/socket_registry.hpp"
#include "robotweax/srt/sequence.hpp"

#include <algorithm>
#include <cstring>
#include <new>

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
            && member.public_data.sockstate != SRTS_BROKEN) {
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

[[nodiscard]] bool read_boolean_option(
    const void* value, int value_size, bool& result) noexcept
{
    static_assert(sizeof(bool) == 1, "The compatible SRT ABI requires 1-byte bool");
    if (value == nullptr || value_size < 0) {
        return false;
    }
    if (value_size == static_cast<int>(sizeof(bool))) {
        unsigned char raw = 0;
        std::memcpy(&raw, value, sizeof(raw));
        if (raw > 1U) {
            return false;
        }
        result = raw != 0U;
        return true;
    }
    if (value_size != static_cast<int>(sizeof(std::int32_t))) {
        return false;
    }
    std::int32_t compatible_integer = 0;
    std::memcpy(&compatible_integer, value, sizeof(compatible_integer));
    if (compatible_integer != 0 && compatible_integer != 1) {
        return false;
    }
    result = compatible_integer != 0;
    return true;
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
    static GroupRegistry registry;
    return registry;
}

SRTSOCKET GroupRegistry::create(SRT_GROUP_TYPE type) noexcept
{
    if (type != SRT_GTYPE_BROADCAST && type != SRT_GTYPE_BACKUP) {
        return SRT_INVALID_SOCK;
    }

    try {
        auto record = std::make_shared<GroupRecord>();
        std::lock_guard lock(mutex_);
        if (clearing_ || next_group_ == SRT_INVALID_SOCK) {
            return SRT_INVALID_SOCK;
        }

        const SRTSOCKET first_candidate = next_group_;
        do {
            const SRTSOCKET base = next_group_;
            next_group_ = next_registry_handle(next_group_);
            const SRTSOCKET candidate = base | SRTGROUP_MASK;
            if (groups_.find(candidate) == groups_.end()) {
                record->handle = candidate;
                record->type = type;
                record->generation = next_generation_++;
                record->initial_sequence =
                    (static_cast<std::uint32_t>(candidate)
                        * 2'654'435'761U)
                    & SequenceNumber::mask;
                record->next_send_sequence = record->initial_sequence;
                record->replay_acknowledged_sequence = record->initial_sequence;
                record->next_receive_sequence = record->initial_sequence;
                if (next_generation_ == 0) {
                    next_generation_ = 1;
                }
                groups_.emplace(candidate, std::move(record));
                return candidate;
            }
        } while (
            next_group_ != first_candidate && next_group_ != SRT_INVALID_SOCK);
    } catch (const std::bad_alloc&) {
        return SRT_INVALID_SOCK;
    } catch (...) {
        return SRT_INVALID_SOCK;
    }
    return SRT_INVALID_SOCK;
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
    output.block_until_connected = !record->opened;
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
    SRTSOCKET group, SRT_SOCKOPT option,
    void* value, int* value_size) noexcept
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
    if (option == SRTO_PEERVERSION) {
        SRTSOCKET first_member = SRT_INVALID_SOCK;
        {
            std::lock_guard lock(record->mutex);
            if (record->closed) {
                set_last_error(SRT_EINVSOCK);
                return SRT_ERROR;
            }
            if (!record->members.empty()) {
                first_member = record->members.front().public_data.id;
            }
        }
        std::int32_t peer_version = 0;
        const auto member =
            SocketRegistry::instance().find(first_member);
        if (member != nullptr) {
            std::lock_guard lock(member->mutex);
            peer_version = static_cast<std::int32_t>(
                member->peer_srt_version);
        }
        if (*value_size < static_cast<int>(sizeof(peer_version))) {
            set_last_error(SRT_EINVPARAM);
            return SRT_ERROR;
        }
        std::memcpy(value, &peer_version, sizeof(peer_version));
        *value_size = static_cast<int>(sizeof(peer_version));
        return 0;
    }
    if (option == SRTO_MININPUTBW) {
        std::int64_t minimum_input = 0;
        {
            std::lock_guard lock(record->mutex);
            if (record->closed) {
                set_last_error(SRT_EINVSOCK);
                return SRT_ERROR;
            }
            minimum_input =
                record->minimum_input_bandwidth_bytes_per_second;
        }
        if (*value_size < static_cast<int>(sizeof(minimum_input))) {
            set_last_error(SRT_EINVPARAM);
            return SRT_ERROR;
        }
        std::memcpy(value, &minimum_input, sizeof(minimum_input));
        *value_size = static_cast<int>(sizeof(minimum_input));
        return 0;
    }
    if (option == SRTO_PASSPHRASE) {
        set_last_error(SRT_EINVOP);
        return SRT_ERROR;
    }
    if (option == SRTO_PBKEYLEN || option == SRTO_KMREFRESHRATE
        || option == SRTO_KMPREANNOUNCE || option == SRTO_ENFORCEDENCRYPTION
#ifdef ENABLE_AEAD_API_PREVIEW
        || option == SRTO_CRYPTOMODE
#endif
    ) {
        std::int32_t integer_result = 0;
        bool boolean_result = false;
        bool boolean_option = option == SRTO_ENFORCEDENCRYPTION;
        {
            std::lock_guard lock(record->mutex);
            if (record->closed) {
                set_last_error(SRT_EINVSOCK);
                return SRT_ERROR;
            }
            if (option == SRTO_PBKEYLEN) {
                integer_result =
                    static_cast<std::int32_t>(record->member_native_options
                            .configured_encryption_key_length());
#ifdef ENABLE_AEAD_API_PREVIEW
            } else if (option == SRTO_CRYPTOMODE) {
                integer_result = static_cast<std::int32_t>(
                    record->member_native_options.get(SocketOption::crypto_mode)
                        .value);
#endif
            } else if (option == SRTO_KMREFRESHRATE) {
                integer_result =
                    static_cast<std::int32_t>(record->member_native_options
                            .get(SocketOption::key_refresh_rate_packets)
                            .value);
            } else if (option == SRTO_KMPREANNOUNCE) {
                integer_result =
                    static_cast<std::int32_t>(record->member_native_options
                            .get(SocketOption::key_preannouncement_packets)
                            .value);
            } else {
                boolean_result =
                    record->member_native_options.enforced_encryption();
            }
        }
        const int required = boolean_option
            ? static_cast<int>(sizeof(boolean_result))
            : static_cast<int>(sizeof(integer_result));
        if (*value_size < required) {
            set_last_error(SRT_EINVPARAM);
            return SRT_ERROR;
        }
        if (boolean_option) {
            std::memcpy(value, &boolean_result, sizeof(boolean_result));
        } else {
            std::memcpy(value, &integer_result, sizeof(integer_result));
        }
        *value_size = required;
        return 0;
    }

    std::int32_t result = 0;
    bool boolean_result = false;
    bool boolean_option = false;
    {
        std::lock_guard lock(record->mutex);
        if (record->closed) {
            set_last_error(SRT_EINVSOCK);
            return SRT_ERROR;
        }
        switch (option) {
        case SRTO_SNDSYN:
            boolean_result = record->send_synchronous;
            boolean_option = true;
            break;
        case SRTO_RCVSYN:
            boolean_result = record->receive_synchronous;
            boolean_option = true;
            break;
        case SRTO_DRIFTTRACER:
            boolean_result = record->drift_tracer;
            boolean_option = true;
            break;
        case SRTO_SNDTIMEO:
            result = record->send_timeout_milliseconds;
            break;
        case SRTO_RCVTIMEO:
            result = record->receive_timeout_milliseconds;
            break;
        case SRTO_GROUPMINSTABLETIMEO:
            if (record->type != SRT_GTYPE_BACKUP) {
                set_last_error(SRT_EINVPARAM);
                return SRT_ERROR;
            }
            result = record->minimum_stability_timeout_milliseconds;
            break;
        case SRTO_IPTTL:
            result = record->ip_time_to_live;
            break;
        case SRTO_IPTOS:
            result = record->ip_type_of_service;
            break;
        case SRTO_MINVERSION:
            result = record->minimum_peer_srt_version;
            break;
        case SRTO_PEERIDLETIMEO:
            result = record->peer_idle_timeout_milliseconds;
            break;
        case SRTO_BINDTODEVICE:
        case SRTO_EVENT:
        case SRTO_SNDDATA:
        case SRTO_RCVDATA:
        case SRTO_GROUPTYPE:
            set_last_error(SRT_EINVOP);
            return SRT_ERROR;
        default:
            set_last_error(SRT_EINVPARAM);
            return SRT_ERROR;
        }
    }
    const int required = boolean_option
        ? static_cast<int>(sizeof(boolean_result))
        : static_cast<int>(sizeof(result));
    if (*value_size < required) {
        set_last_error(SRT_EINVPARAM);
        return SRT_ERROR;
    }
    if (boolean_option) {
        std::memcpy(value, &boolean_result, sizeof(boolean_result));
    } else {
        std::memcpy(value, &result, sizeof(result));
    }
    *value_size = required;
    return 0;
}

int GroupRegistry::set_io_option(
    SRTSOCKET group, SRT_SOCKOPT option,
    const void* value, int value_size) noexcept
{
    if (value == nullptr) {
        set_last_error(SRT_EINVPARAM);
        return SRT_ERROR;
    }
    if (option == SRTO_PEERVERSION || option == SRTO_GROUPTYPE
        || option == SRTO_BINDTODEVICE) {
        set_last_error(SRT_EINVOP);
        return SRT_ERROR;
    }
    if (option == SRTO_PASSPHRASE || option == SRTO_PBKEYLEN
        || option == SRTO_KMREFRESHRATE || option == SRTO_KMPREANNOUNCE
        || option == SRTO_ENFORCEDENCRYPTION
#ifdef ENABLE_AEAD_API_PREVIEW
        || option == SRTO_CRYPTOMODE
#endif
    ) {
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
        if (record->opened) {
            set_last_error(SRT_ECONNSOCK);
            return SRT_ERROR;
        }
        Error result = Error::invalid_state;
        if (option == SRTO_PASSPHRASE) {
            const std::string_view passphrase {static_cast<const char*>(value),
                static_cast<std::size_t>(value_size)};
            result = record->member_native_options.set_passphrase(passphrase);
        } else if (option == SRTO_ENFORCEDENCRYPTION) {
            bool parsed = false;
            if (read_boolean_option(value, value_size, parsed)) {
                result = record->member_native_options.set(
                    SocketOption::enforced_encryption, parsed ? 1 : 0);
            }
        } else {
            std::int32_t parsed = 0;
            if (value_size == static_cast<int>(sizeof(parsed))) {
                std::memcpy(&parsed, value, sizeof(parsed));
                if (parsed >= 0) {
                    SocketOption native_option =
                        SocketOption::key_preannouncement_packets;
                    if (option == SRTO_PBKEYLEN) {
                        native_option = SocketOption::encryption_key_length;
                    } else if (option == SRTO_KMREFRESHRATE) {
                        native_option = SocketOption::key_refresh_rate_packets;
#ifdef ENABLE_AEAD_API_PREVIEW
                    } else if (option == SRTO_CRYPTOMODE) {
                        native_option = SocketOption::crypto_mode;
#endif
                    }
                    result = record->member_native_options.set(
                        native_option, parsed);
                }
            }
        }
        if (result != Error::none) {
            set_last_error(SRT_EINVPARAM);
            return SRT_ERROR;
        }
        return 0;
    }
    if (option == SRTO_DRIFTTRACER || option == SRTO_MININPUTBW
        || option == SRTO_MINVERSION) {
        bool drift_tracer = true;
        std::int64_t minimum_input = 0;
        std::int32_t minimum_version = 0;
        bool valid = false;
        if (option == SRTO_DRIFTTRACER) {
            valid = read_boolean_option(value, value_size, drift_tracer);
        } else if (option == SRTO_MININPUTBW
            && value_size == static_cast<int>(sizeof(minimum_input))) {
            std::memcpy(&minimum_input, value, sizeof(minimum_input));
            valid = minimum_input >= 0;
        } else if (option == SRTO_MINVERSION
            && value_size == static_cast<int>(sizeof(minimum_version))) {
            std::memcpy(&minimum_version, value, sizeof(minimum_version));
            valid = minimum_version >= 0
                && minimum_version <= 0x00ff'ffff;
        }
        if (!valid) {
            set_last_error(SRT_EINVPARAM);
            return SRT_ERROR;
        }
        const auto record = find(group);
        if (record == nullptr) {
            set_last_error(SRT_EINVSOCK);
            return SRT_ERROR;
        }
        std::vector<SRTSOCKET> members;
        {
            std::lock_guard lock(record->mutex);
            if (record->closed) {
                set_last_error(SRT_EINVSOCK);
                return SRT_ERROR;
            }
            if (option == SRTO_MINVERSION && record->opened) {
                set_last_error(SRT_ECONNSOCK);
                return SRT_ERROR;
            }
            try {
                members.reserve(record->members.size());
                for (const auto& member : record->members) {
                    members.push_back(member.public_data.id);
                }
            } catch (...) {
                set_last_error(SRT_ENOBUF);
                return SRT_ERROR;
            }
            if (option == SRTO_DRIFTTRACER) {
                record->drift_tracer = drift_tracer;
            } else if (option == SRTO_MININPUTBW) {
                record->minimum_input_bandwidth_bytes_per_second =
                    minimum_input;
            } else {
                record->minimum_peer_srt_version = minimum_version;
            }
        }
        if (option != SRTO_MINVERSION) {
            for (const SRTSOCKET member_handle : members) {
                const auto member =
                    SocketRegistry::instance().find(member_handle);
                if (member == nullptr) {
                    continue;
                }
                const void* member_value = option == SRTO_DRIFTTRACER
                    ? static_cast<const void*>(&drift_tracer)
                    : static_cast<const void*>(&minimum_input);
                const int member_size = option == SRTO_DRIFTTRACER
                    ? static_cast<int>(sizeof(drift_tracer))
                    : static_cast<int>(sizeof(minimum_input));
                if (set_socket_option(*member, option,
                        member_value, member_size) == 0) {
                    std::shared_ptr<ConnectionRuntime> runtime;
                    SocketOptions native_options;
                    {
                        std::lock_guard member_lock(member->mutex);
                        runtime = member->runtime;
                        native_options = member->native_options;
                    }
                    if (runtime != nullptr) {
                        runtime->apply_options(native_options);
                    }
                }
            }
        }
        return 0;
    }
    std::int32_t parsed = 0;
    if (option == SRTO_SNDSYN || option == SRTO_RCVSYN) {
        bool boolean_value = false;
        if (!read_boolean_option(value, value_size, boolean_value)) {
            set_last_error(SRT_EINVPARAM);
            return SRT_ERROR;
        }
        parsed = boolean_value ? 1 : 0;
    } else {
        if (value_size != static_cast<int>(sizeof(parsed))) {
            set_last_error(SRT_EINVPARAM);
            return SRT_ERROR;
        }
        std::memcpy(&parsed, value, sizeof(parsed));
    }
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
    switch (option) {
    case SRTO_SNDSYN:
        record->send_synchronous = parsed != 0;
        break;
    case SRTO_RCVSYN:
        record->receive_synchronous = parsed != 0;
        break;
    case SRTO_SNDTIMEO:
        if (parsed < -1) {
            set_last_error(SRT_EINVPARAM);
            return SRT_ERROR;
        }
        record->send_timeout_milliseconds = parsed;
        break;
    case SRTO_RCVTIMEO:
        if (parsed < -1) {
            set_last_error(SRT_EINVPARAM);
            return SRT_ERROR;
        }
        record->receive_timeout_milliseconds = parsed;
        break;
    case SRTO_GROUPMINSTABLETIMEO:
        if (record->type != SRT_GTYPE_BACKUP
            || parsed < 60 || parsed > 5'000) {
            set_last_error(SRT_EINVPARAM);
            return SRT_ERROR;
        }
        if (record->opened) {
            set_last_error(SRT_ECONNSOCK);
            return SRT_ERROR;
        }
        record->minimum_stability_timeout_milliseconds = parsed;
        break;
    case SRTO_PEERIDLETIMEO:
        if (parsed < 0) {
            set_last_error(SRT_EINVPARAM);
            return SRT_ERROR;
        }
        if (record->opened) {
            set_last_error(SRT_ECONNSOCK);
            return SRT_ERROR;
        }
        record->peer_idle_timeout_milliseconds = parsed;
        break;
    case SRTO_IPTTL:
    case SRTO_IPTOS:
        if ((option == SRTO_IPTTL
                && (parsed < 1 || parsed > 255))
            || (option == SRTO_IPTOS
                && (parsed < 0 || parsed > 255))) {
            set_last_error(SRT_EINVPARAM);
            return SRT_ERROR;
        }
        if (record->opened) {
            set_last_error(SRT_ECONNSOCK);
            return SRT_ERROR;
        }
        if (option == SRTO_IPTTL) {
            record->ip_time_to_live = parsed;
        } else {
            record->ip_type_of_service = parsed;
            record->ip_type_of_service_explicit = true;
        }
        break;
    default:
        set_last_error(SRT_EINVPARAM);
        return SRT_ERROR;
    }
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
    const auto member = std::find_if(
        record->members.begin(), record->members.end(),
        [socket, member_generation](const auto& candidate) {
            return candidate.public_data.id == socket
                && candidate.generation == member_generation;
        });
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
        || (type != SRT_GTYPE_BROADCAST
            && type != SRT_GTYPE_BACKUP)) {
        return false;
    }

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
                if (record->type != type
                    || record->next_receive_sequence
                        != initial_sequence) {
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

        if (next_group_ == SRT_INVALID_SOCK) {
            return false;
        }
        auto prepared = std::make_shared<GroupRecord>();
        const SRTSOCKET first_candidate = next_group_;
        do {
            const SRTSOCKET base = next_group_;
            next_group_ = next_registry_handle(next_group_);
            const SRTSOCKET candidate = base | SRTGROUP_MASK;
            if (groups_.find(candidate) != groups_.end()) {
                continue;
            }
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
            prepared->send_synchronous = listener_send_synchronous;
            prepared->receive_synchronous = listener_receive_synchronous;
            prepared->send_timeout_milliseconds =
                listener_send_timeout_milliseconds;
            prepared->receive_timeout_milliseconds =
                listener_receive_timeout_milliseconds;
            prepared->drift_tracer = listener_drift_tracer;
            prepared->minimum_input_bandwidth_bytes_per_second =
                listener_minimum_input_bandwidth;
            prepared->minimum_peer_srt_version =
                listener_minimum_peer_version;
            if (next_generation_ == 0U) {
                next_generation_ = 1U;
            }
            output.group = candidate;
            output.generation = prepared->generation;
            output.created = true;
            output.drift_tracer = prepared->drift_tracer;
            output.minimum_input_bandwidth_bytes_per_second =
                prepared->minimum_input_bandwidth_bytes_per_second;
            output.minimum_peer_srt_version =
                prepared->minimum_peer_srt_version;
            groups_.emplace(candidate, std::move(prepared));
            return true;
        } while (
            next_group_ != first_candidate && next_group_ != SRT_INVALID_SOCK);
    } catch (...) {
        return false;
    }
    return false;
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
        record->members.push_back(member);
        ++record->snapshot_version;
        group_generation = record->generation;
        member_generation = member.generation;
        if (first_member != nullptr) {
            *first_member = was_empty && !record->opened;
        }
    } catch (...) {
        return false;
    }
    ReadinessSignal::notify();
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
        if (record->closed
            || record->generation != group_generation) {
            return;
        }
        const auto member = std::find_if(
            record->members.begin(), record->members.end(),
            [socket, member_generation](const auto& candidate) {
                return candidate.public_data.id == socket
                    && candidate.generation == member_generation;
            });
        if (member == record->members.end()) {
            return;
        }
        const SRT_SOCKSTATUS previous_state =
            member->public_data.sockstate;
        if (previous_state != state
            || member->public_data.result != result) {
            member->public_data.sockstate = state;
            member->public_data.memberstate =
                group_member_status(state);
            member->public_data.result = result;
            advance_version(record->snapshot_version);
            if (broken_connection
                && previous_state == SRTS_CONNECTED
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
        ReadinessSignal::notify();
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
        ReadinessSignal::notify();
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
        // Match the data-path lock order: send coordinator before group
        // metadata. In-flight shared owners may outlive registry retirement;
        // release replay storage and secrets before handing off closure.
        std::lock_guard send_lock(record->send_mutex);
        std::lock_guard lock(record->mutex);
        if (record->closed) {
            return;
        }
        record->closed = true;
        record->receive_clock.reset();
        record->replay_history.release_storage();
        (void)record->member_native_options.set_passphrase({});
        record->member_native_options = {};
        members.swap(record->members);
        ++record->snapshot_version;
    }
    for (const auto& member : members) {
        SocketRegistry::instance().close(
            member.public_data.id);
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
        std::lock_guard send_lock(record.send_mutex);
        std::lock_guard lock(record.mutex);
        record.closed = true;
        record.receive_clock.reset();
        record.replay_history.release_storage();
        (void)record.member_native_options.set_passphrase({});
        record.member_native_options = {};
        record.members.clear();
        ++record.snapshot_version;
    }
    {
        std::lock_guard lock(mutex_);
        clearing_ = false;
    }
    ReadinessSignal::notify();
}

} // namespace robotweax::srt::compat
