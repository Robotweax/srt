#include "srt/srt.h"

#include "compat/error_state.hpp"
#include "compat/connection.hpp"
#include "compat/group_config.hpp"
#include "compat/group_registry.hpp"
#include "compat/process_state.hpp"
#include "compat/readiness.hpp"
#include "compat/socket_registry.hpp"
#include "compat/socket_io.hpp"

#include <cstddef>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <limits>
#include <new>
#include <utility>
#include <vector>

namespace {

constexpr std::uint16_t maximum_group_weight = 32'767U;

[[nodiscard]] int next_group_token() noexcept
{
    static std::atomic<std::uint32_t> token{0U};
    return static_cast<int>((token.fetch_add(
        1U, std::memory_order_relaxed) + 1U)
        & 0x7fff'ffffU);
}

[[nodiscard]] bool source_address_present(
    const sockaddr_storage& storage) noexcept
{
    if (storage.ss_family == AF_INET) {
        const auto& address = reinterpret_cast<
            const sockaddr_in&>(storage);
        return address.sin_port != 0
            || address.sin_addr.s_addr != htonl(INADDR_ANY);
    }
    if (storage.ss_family == AF_INET6) {
        const auto& address = reinterpret_cast<
            const sockaddr_in6&>(storage);
        return address.sin6_port != 0
            || address.sin6_scope_id != 0
            || std::memcmp(&address.sin6_addr, &in6addr_any,
                sizeof(in6addr_any)) != 0;
    }
    return false;
}

[[nodiscard]] int storage_size(int family) noexcept
{
    return family == AF_INET
        ? static_cast<int>(sizeof(sockaddr_in))
        : family == AF_INET6
            ? static_cast<int>(sizeof(sockaddr_in6))
            : 0;
}

[[nodiscard]] bool stateful_api_available() noexcept
{
    if (!robotweax::srt::compat::stateful_process_available()) {
        robotweax::srt::compat::set_last_error(SRT_EINVOP);
        return false;
    }
    return true;
}

[[nodiscard]] std::size_t address_size(
    const sockaddr* address, int supplied_size) noexcept
{
    if (address == nullptr || supplied_size < 0) {
        return 0;
    }
    std::size_t required = 0;
    switch (address->sa_family) {
    case AF_INET:
        required = sizeof(sockaddr_in);
        break;
    case AF_INET6:
        required = sizeof(sockaddr_in6);
        break;
    default:
        return 0;
    }
    if (static_cast<std::size_t>(supplied_size) < required
        || required > sizeof(sockaddr_storage)) {
        return 0;
    }
    return required;
}

} // namespace

extern "C" {

SRTSOCKET srt_accept_bond(
    const SRTSOCKET listeners[], int listener_count,
    int64_t timeout_milliseconds)
{
    if (!stateful_api_available()) {
        return SRT_INVALID_SOCK;
    }
    return robotweax::srt::compat::accept_bond_sockets(
        listeners, listener_count, timeout_milliseconds);
}

SRTSOCKET srt_create_group(SRT_GROUP_TYPE type)
{
    if (!stateful_api_available()) {
        return SRT_INVALID_SOCK;
    }
    if (type != SRT_GTYPE_BROADCAST && type != SRT_GTYPE_BACKUP) {
        robotweax::srt::compat::set_last_error(SRT_EINVPARAM);
        return SRT_INVALID_SOCK;
    }
    const SRTSOCKET group =
        robotweax::srt::compat::runtime_create_group(type);
    if (group == SRT_INVALID_SOCK) {
        robotweax::srt::compat::set_last_error(SRT_ENOBUF);
    }
    return group;
}

SRTSOCKET srt_groupof(SRTSOCKET socket)
{
    if (!stateful_api_available()) {
        return SRT_INVALID_SOCK;
    }
    const auto record =
        robotweax::srt::compat::SocketRegistry::instance().find(socket);
    if (record == nullptr) {
        robotweax::srt::compat::set_last_error(SRT_EINVPARAM);
        return SRT_INVALID_SOCK;
    }

    SRTSOCKET group_id = SRT_INVALID_SOCK;
    std::uint64_t group_generation = 0;
    {
        std::lock_guard lock(record->mutex);
        group_id = record->group_id;
        group_generation = record->group_generation;
    }
    const auto group =
        robotweax::srt::compat::GroupRegistry::instance().find(group_id);
    if (group != nullptr) {
        std::lock_guard lock(group->mutex);
        if (!group->closed && group->generation == group_generation) {
            return group_id;
        }
    }

    robotweax::srt::compat::set_last_error(SRT_EINVPARAM);
    return SRT_INVALID_SOCK;
}

int srt_group_data(
    SRTSOCKET group, SRT_SOCKGROUPDATA* output, size_t* inout_size)
{
    if (!stateful_api_available()) {
        return SRT_ERROR;
    }
    return robotweax::srt::compat::GroupRegistry::instance().data(
        group, output, inout_size);
}

SRT_SOCKOPT_CONFIG* srt_create_config(void)
{
    try {
        auto* config = new (std::nothrow) SRT_SocketOptionObject;
        if (config == nullptr) {
            robotweax::srt::compat::set_last_error(SRT_ENOBUF);
        }
        return config;
    } catch (...) {
        robotweax::srt::compat::set_last_error(SRT_ENOBUF);
        return nullptr;
    }
}

void srt_delete_config(SRT_SOCKOPT_CONFIG* config)
{
    delete config;
}

int srt_config_add(
    SRT_SOCKOPT_CONFIG* config, SRT_SOCKOPT option,
    const void* contents, int length)
{
    if (config == nullptr
        || !config->storage.add(option, contents, length)) {
        robotweax::srt::compat::set_last_error(SRT_EINVPARAM);
        return SRT_ERROR;
    }
    return 0;
}

SRT_SOCKGROUPCONFIG srt_prepare_endpoint(
    const sockaddr* source, const sockaddr* destination, int name_length)
{
    SRT_SOCKGROUPCONFIG endpoint{};
    endpoint.id = SRT_INVALID_SOCK;
    endpoint.errorcode = SRT_EINVPARAM;
    endpoint.token = -1;

    const std::size_t destination_size =
        address_size(destination, name_length);
    if (destination_size == 0) {
        return endpoint;
    }

    if (source != nullptr) {
        const std::size_t source_size = address_size(source, name_length);
        if (source_size == 0
            || source->sa_family != destination->sa_family) {
            return endpoint;
        }
        std::memcpy(&endpoint.srcaddr, source, source_size);
    } else {
        endpoint.srcaddr.ss_family = destination->sa_family;
    }
    std::memcpy(&endpoint.peeraddr, destination, destination_size);
    endpoint.errorcode = SRT_SUCCESS;
    return endpoint;
}

int srt_connect_group(
    SRTSOCKET group, SRT_SOCKGROUPCONFIG endpoints[], int endpoint_count)
{
    using namespace robotweax::srt::compat;
    if (!stateful_api_available()) {
        return SRT_ERROR;
    }
    if (endpoint_count <= 0 || endpoints == nullptr) {
        set_last_error(SRT_EINVPARAM);
        return SRT_ERROR;
    }
    if (!is_group_handle(group)) {
        set_last_error(SRT_EINVSOCK);
        return SRT_ERROR;
    }

    GroupRegistry::ConnectDescription description;
    if (!GroupRegistry::instance().describe_connect(
            group, description)) {
        set_last_error(SRT_EINVSOCK);
        return SRT_ERROR;
    }
    for (int index = 0; index < endpoint_count; ++index) {
        auto& endpoint = endpoints[index];
        endpoint.id = SRT_INVALID_SOCK;
        endpoint.errorcode = SRT_SUCCESS;
        if (storage_size(endpoint.peeraddr.ss_family) == 0
            || endpoint.srcaddr.ss_family
                != endpoint.peeraddr.ss_family
            || endpoint.weight > maximum_group_weight) {
            endpoint.errorcode = SRT_EINVPARAM;
            set_last_error(SRT_EINVPARAM);
            return SRT_ERROR;
        }
    }

    std::vector<SRTSOCKET> spawned;
    try {
        spawned.reserve(static_cast<std::size_t>(endpoint_count));
    } catch (...) {
        set_last_error(SRT_ENOBUF);
        return SRT_ERROR;
    }
    int maximum_timeout_milliseconds = 0;
    SRTSOCKET result = SRT_INVALID_SOCK;
    for (int index = 0; index < endpoint_count; ++index) {
        auto& endpoint = endpoints[index];
        const SRTSOCKET socket = runtime_create_socket();
        const auto record = SocketRegistry::instance().find(socket);
        if (socket == SRT_INVALID_SOCK || record == nullptr) {
            endpoint.errorcode = SRT_ENOBUF;
            continue;
        }

        bool configured = true;
        const std::array group_network_options{
            std::pair{SRTO_IPTTL, description.ip_time_to_live},
            std::pair{SRTO_IPTOS, description.ip_type_of_service},
        };
        for (const auto& [option, option_value] :
             group_network_options) {
            if (set_socket_option(*record, option,
                    &option_value,
                    static_cast<int>(sizeof(option_value)))
                == SRT_ERROR) {
                endpoint.errorcode = last_error().code;
                configured = false;
                break;
            }
        }
        record->public_options.ip_type_of_service_explicit =
            description.ip_type_of_service_explicit;
        record->public_options.peer_idle_timeout_milliseconds =
            description.peer_idle_timeout_milliseconds;
        record->native_options = description.member_native_options;
        if (configured) {
            const bool drift_tracer = description.drift_tracer;
            const std::int64_t minimum_input =
                description.minimum_input_bandwidth_bytes_per_second;
            const std::int32_t minimum_version =
                description.minimum_peer_srt_version;
            if (set_socket_option(*record, SRTO_DRIFTTRACER,
                    &drift_tracer,
                    static_cast<int>(sizeof(drift_tracer))) == SRT_ERROR
                || set_socket_option(*record, SRTO_MININPUTBW,
                    &minimum_input,
                    static_cast<int>(sizeof(minimum_input))) == SRT_ERROR
                || set_socket_option(*record, SRTO_MINVERSION,
                    &minimum_version,
                    static_cast<int>(sizeof(minimum_version))) == SRT_ERROR) {
                endpoint.errorcode = last_error().code;
                configured = false;
            }
        }
        if (configured && endpoint.config != nullptr) {
            const auto config = endpoint.config->storage.snapshot();
            for (std::size_t option_index = 0;
                 option_index < config.size; ++option_index) {
                const auto& option = config.options[option_index];
                if (set_socket_option(*record, option.option,
                        option.bytes.data(), option.size)
                    == SRT_ERROR) {
                    endpoint.errorcode = last_error().code;
                    configured = false;
                    break;
                }
            }
        }
        if (!configured) {
            SocketRegistry::instance().close(socket);
            continue;
        }
        if (source_address_present(endpoint.srcaddr)
            && bind_socket(record.get(),
                reinterpret_cast<const sockaddr*>(
                    &endpoint.srcaddr),
                storage_size(endpoint.srcaddr.ss_family))
                == SRT_ERROR) {
            endpoint.errorcode = last_error().code;
            SocketRegistry::instance().close(socket);
            continue;
        }

        if (endpoint.token == -1) {
            endpoint.token = next_group_token();
        }
        std::uint64_t group_generation = 0;
        std::uint64_t member_generation = 0;
        if (!GroupRegistry::instance().add_member(
                group, socket, endpoint.peeraddr,
                endpoint.weight, endpoint.token,
                group_generation, member_generation)) {
            endpoint.errorcode = SRT_EINVSOCK;
            SocketRegistry::instance().close(socket);
            continue;
        }
        {
            std::lock_guard lock(record->mutex);
            record->public_options.send_synchronous = false;
            record->public_options.receive_synchronous = false;
            record->connection_initial_sequence =
                description.initial_sequence;
            record->peer_connection_initial_sequence =
                description.initial_sequence;
            record->group_id = group;
            record->group_generation = group_generation;
            record->member_generation = member_generation;
            record->group_type = description.type;
            record->group_weight = endpoint.weight;
            record->connect_callback = description.connect_callback;
            record->connect_callback_opaque =
                description.connect_callback_opaque;
            record->connect_callback_token = endpoint.token;
            maximum_timeout_milliseconds = std::max(
                maximum_timeout_milliseconds,
                record->public_options
                    .connection_timeout_milliseconds);
        }
        endpoint.id = socket;
        spawned.push_back(socket);
        if (connect_socket(record,
                reinterpret_cast<const sockaddr*>(
                    &endpoint.peeraddr),
                storage_size(endpoint.peeraddr.ss_family))
            == SRT_ERROR) {
            endpoint.errorcode = last_error().code;
            endpoint.id = SRT_INVALID_SOCK;
            GroupRegistry::instance().remove_member(
                group, group_generation, socket,
                member_generation);
            {
                std::lock_guard lock(record->mutex);
                record->group_id = SRT_INVALID_SOCK;
                record->group_generation = 0U;
                record->member_generation = 0U;
                record->group_type = SRT_GTYPE_UNDEFINED;
                record->group_weight = 0U;
            }
            SocketRegistry::instance().close(socket);
            continue;
        }
        GroupRegistry::instance().mark_opened(
            group, group_generation);
        result = socket;
    }

    if (result == SRT_INVALID_SOCK) {
        set_last_error(SRT_ECONNSETUP);
        return SRT_ERROR;
    }
    if (!description.block_until_connected) {
        return result;
    }

    const auto deadline = ReadinessSignal::Clock::now()
        + std::chrono::milliseconds{
            std::max(maximum_timeout_milliseconds, 1)};
    for (;;) {
        const std::uint64_t observed =
            ReadinessSignal::generation();
        bool pending = false;
        if (GroupRegistry::instance().state(group)
            == SRTS_CLOSED) {
            set_last_error(SRT_ESCLOSED);
            return SRT_ERROR;
        }
        for (const SRTSOCKET socket : spawned) {
            const SRT_SOCKSTATUS state =
                SocketRegistry::instance().state(socket);
            if (state == SRTS_CONNECTED) {
                return socket;
            }
            pending = pending || state == SRTS_CONNECTING
                || state == SRTS_OPENED || state == SRTS_INIT;
        }
        if (!pending) {
            set_last_error(SRT_ECONNSETUP);
            return SRT_ERROR;
        }
        if (ReadinessSignal::Clock::now() >= deadline) {
            set_last_error(SRT_ENOSERVER);
            return SRT_ERROR;
        }
        ReadinessSignal::wait_until(observed, deadline);
    }
}

} // extern "C"
