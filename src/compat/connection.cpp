#include "compat/connection.hpp"

#include "robotweax/srt/codec.hpp"
#include "robotweax/srt/handshake.hpp"
#include "robotweax/srt/handshake_datagram.hpp"
#include "robotweax/srt/rendezvous.hpp"
#include "compat/caller_handshake_events.hpp"
#include "compat/caller_handshake_sources.hpp"
#include "compat/connect_handshake_operation.hpp"
#include "compat/error_state.hpp"
#include "compat/group_registry.hpp"
#include "compat/listener_connection_setup.hpp"
#include "compat/listener_connection_setup_sources.hpp"
#include "compat/listener_handshake_router.hpp"
#include "compat/listener_handshake_sources.hpp"
#include "compat/readiness.hpp"
#include "compat/runtime_scheduler_service.hpp"
#include "compat/runtime_work_executor_service.hpp"
#include "compat/socket_io.hpp"
#include "robotweax/srt/crypto_provider.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <thread>
#include <utility>

namespace robotweax::srt::compat {
namespace {

using Clock = std::chrono::steady_clock;

[[nodiscard]] Clock::time_point group_timestamp_origin(
    const std::shared_ptr<GroupRecord>& group, Clock::time_point fallback)
{
    if (!group) {
        return fallback;
    }
    std::lock_guard lock(group->mutex);
    if (!group->timestamp_origin) {
        group->timestamp_origin = fallback;
    }
    return *group->timestamp_origin;
}

[[nodiscard]] Clock::time_point socket_timestamp_origin(
    SocketRecord& socket, Clock::time_point fallback)
{
    SRTSOCKET group_id;
    {
        std::lock_guard lock(socket.mutex);
        group_id = socket.group_id;
    }
    return group_timestamp_origin(
        GroupRegistry::instance().find(group_id), fallback);
}

constexpr std::uint32_t retry_interval_milliseconds = 250U;
constexpr int close_poll_interval_milliseconds = 50;
constexpr auto listener_cookie_window = std::chrono::seconds{60};
// A connected Rendezvous peer may release a complete initial DATA/FEC flight
// immediately behind AGREEMENT. Keep this temporary queue large enough for
// that handoff burst while retaining a strict per-setup memory bound.
constexpr std::size_t setup_inbox_capacity = 256U;

std::mutex accept_bond_scope_mutex;
std::uint64_t next_accept_bond_scope = 1U;

[[nodiscard]] std::uint64_t allocate_accept_bond_scope() noexcept
{
    const std::uint64_t result = next_accept_bond_scope++;
    if (next_accept_bond_scope == 0U) {
        next_accept_bond_scope = 1U;
    }
    return result;
}

[[nodiscard]] constexpr int socket_address_size(
    IpAddressFamily family) noexcept
{
    return family == IpAddressFamily::ipv4
        ? static_cast<int>(sizeof(sockaddr_in))
        : static_cast<int>(sizeof(sockaddr_in6));
}

enum class ActionOutcome : std::uint8_t {
    running,
    connected,
    rejected,
    failed,
    io_error,
};

struct ActionResult {
    ActionOutcome outcome = ActionOutcome::running;
    int system_error = 0;
    HandshakeAction last_sent{};
    bool has_last_sent = false;
    int rejection_reason = SRT_REJ_UNKNOWN;
};

[[nodiscard]] int crypto_rejection_reason(
    const CryptoSession& crypto, int fallback) noexcept
{
#ifdef ENABLE_AEAD_API_PREVIEW
    if (crypto.sender_state() == CryptoState::bad_crypto_mode
        || crypto.receiver_state() == CryptoState::bad_crypto_mode) {
        return SRT_REJ_CRYPTO;
    }
#else
    (void)crypto;
#endif
    return fallback;
}

struct ApprovedCookieContext {
    std::uint32_t cookie = 0;
};

void publish_group_member_state(SocketRecord& socket) noexcept;

void publish_listener_group_update(SRTSOCKET listener) noexcept
{
    const auto record =
        SocketRegistry::instance().find(listener);
    if (record == nullptr) {
        return;
    }
    {
        std::lock_guard lock(record->mutex);
        ++record->group_update_version;
        if (record->group_update_version == 0U) {
            record->group_update_version = 1U;
        }
    }
    ReadinessSignal::notify();
}

[[nodiscard]] constexpr SRT_GROUP_TYPE compatible_group_type(
    GroupType type) noexcept
{
    switch (type) {
    case GroupType::broadcast:
        return SRT_GTYPE_BROADCAST;
    case GroupType::backup:
        return SRT_GTYPE_BACKUP;
    default:
        return SRT_GTYPE_UNDEFINED;
    }
}

// A group request is reserved while the listener handshake can still reject
// it. Only the allocation-free socket-state publication happens after the
// transport runtime has attached. The destructor rolls back every uncommitted
// reservation, including a newly created empty mirror.
struct ListenerGroupAdmission {
    SRTSOCKET listener = SRT_INVALID_SOCK;
    std::uint32_t initial_sequence = 0;
    bool enabled = false;
    SRTSOCKET member_handle = SRT_INVALID_SOCK;
    std::shared_ptr<SocketRecord> member;
    IpEndpoint endpoint{};
    GroupRegistry::MirrorDescription mirror{};
    GroupMembership peer{};
    std::uint64_t member_generation = 0;
    bool first_member = false;
    bool committed = false;

    ~ListenerGroupAdmission()
    {
        rollback();
    }

    [[nodiscard]] bool prepare(
        const GroupMembership& proposed,
        GroupMembership& response) noexcept
    {
        constexpr std::uint16_t maximum_weight = 32'767U;
        const SRT_GROUP_TYPE type =
            compatible_group_type(proposed.type);
        if (!enabled || type == SRT_GTYPE_UNDEFINED
            || proposed.flags != 0U
            || proposed.weight > maximum_weight) {
            return false;
        }
        if (mirror.group != SRT_INVALID_SOCK) {
            if (proposed.group_id != peer.group_id
                || proposed.type != peer.type
                || proposed.flags != peer.flags
                || proposed.weight != peer.weight) {
                return false;
            }
        } else if (!GroupRegistry::instance().prepare_mirror(
                       listener,
                       static_cast<SRTSOCKET>(proposed.group_id),
                       type, initial_sequence, mirror)) {
            return false;
        } else {
            peer = proposed;
        }
        if (member_generation == 0U) {
            sockaddr_storage peer_address{};
            int peer_address_size =
                static_cast<int>(sizeof(peer_address));
            std::uint64_t group_generation = 0;
            if (write_ip_endpoint(endpoint,
                    reinterpret_cast<sockaddr*>(&peer_address),
                    &peer_address_size) == SRT_ERROR
                || !GroupRegistry::instance().add_member(
                    mirror.group, member_handle, peer_address,
                    peer.weight, -1, group_generation,
                    member_generation, &first_member)
                || group_generation != mirror.generation) {
                if (member_generation != 0U) {
                    GroupRegistry::instance().remove_member(
                        mirror.group, mirror.generation,
                        member_handle, member_generation);
                    member_generation = 0U;
                }
                return false;
            }
        }
        response = {
            .group_id = static_cast<std::uint32_t>(mirror.group),
            .type = proposed.type,
            .flags = proposed.flags,
            .weight = proposed.weight,
        };
        return true;
    }

    [[nodiscard]] bool activate() noexcept
    {
        if (mirror.group == SRT_INVALID_SOCK
            || member == nullptr || member_generation == 0U) {
            return false;
        }
        {
            std::lock_guard lock(member->mutex);
            if (member->state == SRTS_CLOSING
                || member->state == SRTS_CLOSED) {
                return false;
            }
            member->group_id = mirror.group;
            member->group_generation = mirror.generation;
            member->member_generation = member_generation;
            member->group_type =
                compatible_group_type(peer.type);
            member->group_weight = peer.weight;
            member->group_accept_result = first_member;
        }
        const bool drift_tracer = mirror.drift_tracer;
        const std::int64_t minimum_input =
            mirror.minimum_input_bandwidth_bytes_per_second;
        if (set_socket_option(*member, SRTO_DRIFTTRACER,
                &drift_tracer,
                static_cast<int>(sizeof(drift_tracer))) == SRT_ERROR
            || set_socket_option(*member, SRTO_MININPUTBW,
                &minimum_input,
                static_cast<int>(sizeof(minimum_input))) == SRT_ERROR) {
            return false;
        }
        std::shared_ptr<ConnectionRuntime> runtime;
        SocketOptions native_options;
        {
            std::lock_guard lock(member->mutex);
            runtime = member->runtime;
            native_options = member->native_options;
        }
        if (runtime != nullptr) {
            runtime->apply_options(native_options);
        }
        publish_group_member_state(*member);
        if (!first_member) {
            publish_listener_group_update(listener);
        }
        return true;
    }

    void commit() noexcept
    {
        if (mirror.group != SRT_INVALID_SOCK) {
            GroupRegistry::instance().mark_opened(
                mirror.group, mirror.generation);
        }
        committed = true;
    }

    void rollback() noexcept
    {
        if (committed) {
            return;
        }
        if (member != nullptr && member_generation != 0U) {
            {
                std::lock_guard lock(member->mutex);
                if (member->group_id == mirror.group
                    && member->group_generation
                        == mirror.generation
                    && member->member_generation
                        == member_generation) {
                    member->group_id = SRT_INVALID_SOCK;
                    member->group_generation = 0U;
                    member->member_generation = 0U;
                    member->group_type = SRT_GTYPE_UNDEFINED;
                    member->group_weight = 0U;
                    member->group_accept_result = false;
                }
            }
            GroupRegistry::instance().remove_member(
                mirror.group, mirror.generation,
                member_handle, member_generation);
            member.reset();
            member_generation = 0U;
        }
        if (mirror.created) {
            GroupRegistry::instance().release_empty_mirror(
                mirror.group, mirror.generation);
            mirror = {};
        }
    }
};

[[nodiscard]] bool negotiate_listener_group(
    const GroupMembership& peer, GroupMembership& local,
    void* context) noexcept
{
    return context != nullptr
        && static_cast<ListenerGroupAdmission*>(context)
            ->prepare(peer, local);
}

[[nodiscard]] int fail(SRT_ERRNO error, int system_error = 0) noexcept
{
    set_last_error(error, system_error);
    return SRT_ERROR;
}

[[nodiscard]] SRTSOCKET fail_accept(
    SRT_ERRNO error, int system_error = 0) noexcept
{
    set_last_error(error, system_error);
    return SRT_INVALID_SOCK;
}

[[nodiscard]] std::uint32_t mix(std::uint32_t value) noexcept
{
    value ^= value >> 16U;
    value *= 0x7feb'352dU;
    value ^= value >> 15U;
    value *= 0x846c'a68bU;
    value ^= value >> 16U;
    return value;
}

[[nodiscard]] std::uint32_t endpoint_fingerprint(
    IpEndpoint endpoint) noexcept
{
    std::uint32_t result = 2'166'136'261U;
    for (std::size_t index = 0;
         index < endpoint.address_size(); ++index) {
        result ^= endpoint.address[index];
        result *= 16'777'619U;
    }
    result ^= endpoint.port;
    result *= 16'777'619U;
    result ^= endpoint.scope_id;
    result *= 16'777'619U;
    result ^= static_cast<std::uint32_t>(endpoint.family);
    return result;
}

[[nodiscard]] std::uint32_t approved_connection_cookie(
    const Handshake&, void* opaque_context) noexcept
{
    const auto& context =
        *static_cast<const ApprovedCookieContext*>(opaque_context);
    return context.cookie;
}

[[nodiscard]] bool generate_listener_cookie_secret(
    std::array<std::byte, 16>& secret) noexcept
{
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (default_crypto_provider().random_bytes(secret)
            != Error::none) {
            return false;
        }
        if (std::any_of(secret.begin(), secret.end(),
                [](std::byte value) {
                    return value != std::byte{0};
                })) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::uint64_t listener_cookie_time_window() noexcept
{
    const auto elapsed = std::chrono::duration_cast<
        std::chrono::seconds>(Clock::now().time_since_epoch());
    return static_cast<std::uint64_t>(
        elapsed.count() / listener_cookie_window.count());
}

[[nodiscard]] std::uint64_t listener_cookie_time_window_for_actor(
    void*) noexcept
{
    return listener_cookie_time_window();
}

[[nodiscard]] IpEndpoint ipv4_mapped_endpoint(
    IpEndpoint ipv4) noexcept
{
    IpEndpoint endpoint = IpEndpoint::ipv6_any(ipv4.port);
    endpoint.address[10] = 0xffU;
    endpoint.address[11] = 0xffU;
    std::copy_n(ipv4.address.begin(), 4U,
        endpoint.address.begin() + 12);
    return endpoint;
}

[[nodiscard]] std::uint32_t make_rendezvous_cookie(
    std::uint32_t socket_id,
    IpEndpoint local,
    IpEndpoint peer) noexcept
{
    const auto minutes = std::chrono::duration_cast<
        std::chrono::minutes>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const std::uint32_t local_value =
        endpoint_fingerprint(local);
    const std::uint32_t peer_value =
        endpoint_fingerprint(peer);
    const std::uint32_t cookie = mix(socket_id
        ^ local_value
        ^ ((peer_value << 13U) | (peer_value >> 19U))
        ^ static_cast<std::uint32_t>(minutes));
    return cookie == 0U ? 1U : cookie;
}

[[nodiscard]] PacketTimestamp timestamp_since(
    Clock::time_point origin) noexcept
{
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        Clock::now() - origin).count();
    return PacketTimestamp{static_cast<std::uint32_t>(elapsed)};
}

[[nodiscard]] std::uint64_t microseconds_since(
    Clock::time_point origin) noexcept
{
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        Clock::now() - origin).count();
    return elapsed <= 0 ? 0U : static_cast<std::uint64_t>(elapsed);
}

[[nodiscard]] bool same_endpoint(
    IpEndpoint left, IpEndpoint right) noexcept
{
    return left == right;
}

[[nodiscard]] std::array<std::uint32_t, 4>
handshake_peer_address(IpEndpoint endpoint) noexcept
{
    std::array<std::uint32_t, 4> result{};
    const std::size_t word_count =
        endpoint.is_ipv4() ? 1U : result.size();
    for (std::size_t index = 0; index < word_count; ++index) {
        std::memcpy(&result[index],
            endpoint.address.data() + index * sizeof(std::uint32_t),
            sizeof(std::uint32_t));
    }
    return result;
}

[[nodiscard]] ActionResult send_action(
    DatagramChannel& channel,
    IpEndpoint peer,
    const HandshakeAction& action,
    std::uint32_t destination_socket_id,
    Clock::time_point origin) noexcept
{
    std::array<std::byte, 1500> datagram{};
    HandshakeAction wire_action = action;
    wire_action.packet.peer_address =
        handshake_peer_address(peer);
    const auto encoded = encode_handshake_datagram(
        wire_action, timestamp_since(origin),
        destination_socket_id, datagram);
    if (!encoded) {
        return {.outcome = ActionOutcome::failed};
    }
    const auto sent = channel.send_datagram(
        std::span{datagram}.first(encoded.bytes_written), peer);
    if (!sent) {
        return {
            .outcome = ActionOutcome::io_error,
            .system_error = sent.system_error,
        };
    }
    return {};
}

struct CallerHandshakeSendContext {
    DatagramChannel* channel = nullptr;
    IpEndpoint peer {};
    Clock::time_point origin {};
};

[[nodiscard]] CallerHandshakeSendResult send_caller_handshake_action(
    void* opaque_context, const HandshakeAction& action,
    std::uint32_t destination_socket_id) noexcept
{
    if (opaque_context == nullptr) {
        return {.status = CallerHandshakeSendStatus::failed};
    }
    auto& context = *static_cast<CallerHandshakeSendContext*>(opaque_context);
    if (context.channel == nullptr) {
        return {.status = CallerHandshakeSendStatus::failed};
    }
    const ActionResult sent = send_action(*context.channel, context.peer,
        action, destination_socket_id, context.origin);
    if (sent.outcome == ActionOutcome::io_error) {
        return {
            .status = CallerHandshakeSendStatus::io_error,
            .system_error = sent.system_error,
        };
    }
    if (sent.outcome != ActionOutcome::running) {
        return {.status = CallerHandshakeSendStatus::failed};
    }
    return {};
}

[[nodiscard]] ActionResult apply_actions(
    DatagramChannel& channel,
    IpEndpoint peer,
    std::uint32_t destination_socket_id,
    const HandshakeActions& actions,
    Clock::time_point origin,
    Clock::time_point& retry_deadline) noexcept
{
    ActionResult result;
    for (std::size_t index = 0; index < actions.size; ++index) {
        const HandshakeAction& action = actions.values[index];
        switch (action.kind) {
        case HandshakeActionKind::send: {
            const ActionResult sent = send_action(
                channel, peer, action, destination_socket_id, origin);
            if (sent.outcome != ActionOutcome::running) {
                return sent;
            }
            result.last_sent = action;
            result.last_sent.packet.peer_address =
                handshake_peer_address(peer);
            result.has_last_sent = true;
            break;
        }
        case HandshakeActionKind::connected:
            result.outcome = ActionOutcome::connected;
            return result;
        case HandshakeActionKind::rejected:
            result.outcome = ActionOutcome::rejected;
            result.rejection_reason = action.rejection_reason;
            return result;
        case HandshakeActionKind::failed:
            result.outcome = ActionOutcome::failed;
            return result;
        case HandshakeActionKind::arm_timer:
            retry_deadline = Clock::now()
                + std::chrono::milliseconds{action.timeout_milliseconds};
            break;
        case HandshakeActionKind::none:
            break;
        }
    }
    return result;
}

[[nodiscard]] std::uint32_t retry_budget(
    std::int32_t connection_timeout_milliseconds) noexcept
{
    const auto timeout =
        static_cast<std::uint32_t>(connection_timeout_milliseconds);
    return std::max(1U, timeout / retry_interval_milliseconds + 1U);
}

[[nodiscard]] int wait_duration(
    Clock::time_point retry_deadline,
    Clock::time_point overall_deadline) noexcept
{
    const Clock::time_point now = Clock::now();
    const Clock::time_point next =
        std::min(retry_deadline, overall_deadline);
    if (next <= now) {
        return 0;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        next - now).count();
    return static_cast<int>(std::clamp<std::int64_t>(
        remaining, 1, close_poll_interval_milliseconds));
}

[[nodiscard]] bool socket_was_closed(
    const SocketRecord& socket) noexcept
{
    std::lock_guard lock(socket.mutex);
    return socket.state == SRTS_CLOSING
        || socket.state == SRTS_CLOSED;
}

void publish_group_member_state(SocketRecord& socket) noexcept
{
    SRTSOCKET group = SRT_INVALID_SOCK;
    SRTSOCKET handle = SRT_INVALID_SOCK;
    std::uint64_t group_generation = 0;
    std::uint64_t member_generation = 0;
    SRT_SOCKSTATUS state = SRTS_NONEXIST;
    int result = SRT_SUCCESS;
    {
        std::lock_guard lock(socket.mutex);
        group = socket.group_id;
        group_generation = socket.group_generation;
        member_generation = socket.member_generation;
        handle = static_cast<SRTSOCKET>(socket.protocol_socket_id);
        state = socket.state;
        result = state == SRTS_CONNECTED
            ? SRT_SUCCESS : socket.connect_error;
    }
    if (group == SRT_INVALID_SOCK
        || member_generation == 0U) {
        return;
    }
    GroupRegistry::instance().update_member(
        group, group_generation, handle, member_generation,
        state, result);
}

struct ConnectedNegotiation {
    std::uint32_t peer_srt_version = 0;
    NegotiatedLiveOptions live_options{};
    PacketFilterConfiguration packet_filter{};
    StreamId peer_stream_id{};
    bool has_peer_stream_id = false;
    SequenceNumber initial_sequence{};
    std::uint32_t peer_flow_window = 0;
    bool has_transport_parameters = false;
};

[[nodiscard]] bool stage_connected_transport(
    SocketRecord& socket,
    IpEndpoint peer,
    std::uint32_t peer_socket_id,
    std::uint32_t peer_maximum_segment_size,
    const ConnectedNegotiation& negotiation) noexcept
{
    std::lock_guard lock(socket.mutex);
    if (socket.state == SRTS_CLOSING || socket.state == SRTS_CLOSED
        || negotiation.peer_flow_window == 0U) {
        return false;
    }
    if (negotiation.has_transport_parameters) {
        socket.connection_initial_sequence =
            negotiation.initial_sequence.value();
        socket.peer_connection_initial_sequence =
            negotiation.initial_sequence.value();
    }
    const std::int32_t effective_maximum_segment_size =
        static_cast<std::int32_t>(std::min<std::uint32_t>(
            static_cast<std::uint32_t>(
                socket.public_options.maximum_segment_size),
            peer_maximum_segment_size));
    (void)socket.native_options.set(
        SocketOption::maximum_segment_size,
        effective_maximum_segment_size);
    socket.native_options.constrain_to_address_family(
        peer.wire_family());
    socket.public_options.maximum_segment_size =
        effective_maximum_segment_size;
    const auto buffer_unit =
        effective_maximum_segment_size - 28;
    socket.public_options.send_buffer_bytes =
        static_cast<std::int32_t>(
            socket.native_options.send_buffer_packets())
        * buffer_unit;
    socket.public_options.receive_buffer_bytes =
        static_cast<std::int32_t>(
            socket.native_options.receive_buffer_packets())
        * buffer_unit;
    socket.peer_endpoint = peer;
    socket.has_peer_endpoint = true;
    socket.peer_protocol_socket_id = peer_socket_id;
    socket.peer_flow_window_packets = negotiation.peer_flow_window;
    socket.peer_srt_version = negotiation.peer_srt_version;
    socket.negotiated_live_options = negotiation.live_options;
    socket.native_options.set_effective_packet_filter(
        negotiation.packet_filter);
    if (negotiation.has_peer_stream_id) {
        socket.public_options.stream_id =
            negotiation.peer_stream_id;
    }
    socket.public_options.maximum_payload_size =
        static_cast<std::int32_t>(
            socket.native_options.maximum_payload_size());
    return true;
}

void publish_connected_state(
    SocketRecord& socket,
    SRT_SOCKSTATUS state) noexcept
{
    {
        std::lock_guard lock(socket.mutex);
        if (socket.state == SRTS_CLOSING
            || socket.state == SRTS_CLOSED) {
            return;
        }
        socket.state = state;
        socket.connect_error = SRT_SUCCESS;
        socket.connect_system_error = 0;
        socket.rejection_reason = SRT_REJ_UNKNOWN;
    }
    publish_group_member_state(socket);
    ReadinessSignal::notify();
}

template <typename Machine>
void finish_connect(
    SocketRecord& socket,
    SRT_SOCKSTATUS state,
    IpEndpoint peer,
    std::uint32_t peer_socket_id,
    std::uint32_t peer_maximum_segment_size,
    const Machine& machine) noexcept
{
    ConnectedNegotiation negotiation;
    if (machine.has_peer_extension_parameters()) {
        HandshakeExtensionParameters local_parameters;
        {
            std::lock_guard lock(socket.mutex);
            local_parameters =
                socket.native_options.handshake_parameters();
        }
        negotiation.peer_srt_version =
            machine.peer_extension_parameters().srt_version;
        negotiation.live_options = negotiate_live_options(
            local_parameters,
            machine.peer_extension_parameters());
    }
    negotiation.packet_filter =
        machine.negotiated_packet_filter();
    if (machine.has_peer_stream_id()) {
        negotiation.peer_stream_id = machine.peer_stream_id();
        negotiation.has_peer_stream_id = true;
    }
    negotiation.peer_flow_window = machine.peer_flow_window();
    if (stage_connected_transport(socket, peer,
            peer_socket_id, peer_maximum_segment_size,
            negotiation)) {
        publish_connected_state(socket, state);
    }
}

void invoke_connect_callback(
    const std::shared_ptr<SocketRecord>& socket,
    IpEndpoint peer,
    int error_code) noexcept
{
    srt_connect_callback_fn* callback = nullptr;
    void* opaque = nullptr;
    SRTSOCKET handle = SRT_INVALID_SOCK;
    int token = -1;
    {
        std::lock_guard lock(socket->mutex);
        callback = socket->connect_callback;
        opaque = socket->connect_callback_opaque;
        handle = static_cast<SRTSOCKET>(
            socket->protocol_socket_id);
        token = socket->connect_callback_token;
    }
    if (callback == nullptr) {
        return;
    }

    sockaddr_storage peer_address{};
    int peer_address_size = socket_address_size(peer.family);
    const sockaddr* callback_peer = nullptr;
    if (write_ip_endpoint(peer,
            reinterpret_cast<sockaddr*>(&peer_address),
            &peer_address_size)
        == 0) {
        callback_peer = reinterpret_cast<const sockaddr*>(
            &peer_address);
    }

    try {
        callback(opaque, handle, error_code, callback_peer, token);
    } catch (...) {
        // A user callback must never terminate the connection worker or
        // escape across the public C ABI boundary.
    }
}

[[nodiscard]] int attach_runtime(SocketRecord& socket,
    const std::shared_ptr<DatagramChannel>& channel, Clock::time_point origin,
    std::uint64_t handshake_arrival_microseconds,
    PacketTimestamp peer_handshake_timestamp,
    const HandshakeAction* handshake_replay_response = nullptr,
    std::uint32_t handshake_replay_peer_cookie = 0U,
    const std::shared_ptr<DatagramInbox>& setup_inbox = {},
    const std::shared_ptr<GroupRecord>& admitted_group = {}) noexcept
{
    std::shared_ptr<ConnectionRuntime> runtime;
    try {
        std::lock_guard lock(socket.mutex);
        SocketOptions runtime_options = socket.native_options;
        runtime_options.constrain_to_address_family(
            socket.peer_endpoint.wire_family());
        runtime = std::make_shared<ConnectionRuntime>(
            ConnectionRuntime::Configuration {
                .channel = channel,
                .group = admitted_group
                    ? admitted_group
                    : GroupRegistry::instance().find(socket.group_id),
                .peer = socket.peer_endpoint,
                .peer_socket_id = socket.peer_protocol_socket_id,
                .initial_sequence =
                    SequenceNumber {socket.connection_initial_sequence},
                .peer_initial_sequence =
                    SequenceNumber {socket.peer_connection_initial_sequence},
                .has_distinct_peer_initial_sequence =
                    socket.peer_connection_initial_sequence
                    != socket.connection_initial_sequence,
                .flow_window_packets = static_cast<std::uint32_t>(
                    socket.public_options.flow_window_packets),
                .peer_flow_window_packets = socket.peer_flow_window_packets,
                .options = runtime_options,
                .negotiated_options = socket.negotiated_live_options,
                .origin = origin,
                .handshake_arrival_microseconds =
                    handshake_arrival_microseconds,
                .peer_handshake_timestamp = peer_handshake_timestamp,
                .peer_idle_timeout_milliseconds = static_cast<std::uint32_t>(
                    socket.public_options.peer_idle_timeout_milliseconds),
                .handshake_replay_response =
                    handshake_replay_response == nullptr
                    ? HandshakeAction {}
                    : *handshake_replay_response,
                .handshake_replay_enabled =
                    handshake_replay_response != nullptr,
                .handshake_replay_peer_cookie = handshake_replay_peer_cookie,
                .crypto = socket.crypto,
            });
    } catch (const std::bad_alloc&) {
        return fail(SRT_ENOBUF);
    } catch (...) {
        return fail(SRT_ESYSOBJ);
    }

    const bool route_registered = setup_inbox != nullptr
        ? channel->promote_setup_connection(
              socket.protocol_socket_id, setup_inbox, runtime)
        : channel->register_connection(
              socket.protocol_socket_id, runtime);
    if (!route_registered) {
        return fail(SRT_ENOBUF);
    }
    if (!channel->start()) {
        channel->unregister_connection(socket.protocol_socket_id);
        runtime->close();
        return fail(SRT_ETHREAD);
    }
    if (runtime->broken() || runtime->peer_closed()) {
        channel->unregister_connection(socket.protocol_socket_id);
        runtime->close();
        return fail(SRT_ECONNSETUP);
    }
    {
        std::lock_guard lock(socket.mutex);
        if (socket.state == SRTS_CLOSING
            || socket.state == SRTS_CLOSED) {
            channel->unregister_connection(socket.protocol_socket_id);
            runtime->close();
            return fail(SRT_ESCLOSED);
        }
        socket.runtime = std::move(runtime);
    }
    ReadinessSignal::notify();
    return 0;
}

[[nodiscard]] int fail_connect(
    SocketRecord& socket,
    SRT_ERRNO error,
    int system_error = 0,
    bool asynchronous = false,
    int rejection_reason = -1) noexcept
{
    if (rejection_reason < 0) {
        switch (error) {
        case SRT_ESOCKFAIL:
        case SRT_ETHREAD:
        case SRT_ENOBUF:
        case SRT_ESYSOBJ:
            rejection_reason = SRT_REJ_SYSTEM;
            break;
        case SRT_ENOSERVER:
            rejection_reason = SRT_REJ_TIMEOUT;
            break;
        case SRT_ECONNSETUP:
            rejection_reason = SRT_REJ_ROGUE;
            break;
        case SRT_ECONNREJ:
            rejection_reason = SRT_REJ_PEER;
            break;
        default:
            rejection_reason = SRT_REJ_UNKNOWN;
            break;
        }
    }
    {
        std::lock_guard lock(socket.mutex);
        if (socket.state == SRTS_CLOSING
            || socket.state == SRTS_CLOSED) {
            return fail(SRT_ESCLOSED);
        }
        const bool group_member =
            socket.group_id != SRT_INVALID_SOCK;
        socket.state = asynchronous && !group_member
            ? SRTS_CONNECTING : SRTS_BROKEN;
        socket.connect_error = error;
        socket.connect_system_error = system_error;
        socket.rejection_reason = rejection_reason;
    }
    publish_group_member_state(socket);
    ReadinessSignal::notify();
    return fail(error, system_error);
}

[[nodiscard]] bool listener_is_open(
    const SocketRecord& listener) noexcept
{
    std::lock_guard lock(listener.mutex);
    return listener.state == SRTS_LISTENING;
}

} // namespace

int listen_socket(
    std::shared_ptr<SocketRecord> socket, int backlog) noexcept
{
    if (backlog <= 0) {
        return fail(SRT_EINVPARAM);
    }
    if (socket == nullptr) {
        return fail(SRT_EINVSOCK);
    }

    std::shared_ptr<DatagramChannel> channel;
    std::shared_ptr<HandshakeInbox> inbox;
    std::shared_ptr<ListenerRuntime> listener_runtime;
    {
        std::lock_guard lock(socket->mutex);
        if (socket->public_options.rendezvous) {
            return fail(SRT_ERDVNOSERV);
        }
        switch (socket->state) {
        case SRTS_INIT:
            return fail(SRT_EUNBOUNDSOCK);
        case SRTS_OPENED:
            if (socket->channel == nullptr || !socket->has_local_endpoint) {
                return fail(SRT_EUNBOUNDSOCK);
            }
            if (!generate_listener_cookie_secret(
                    socket->listener_cookie_secret)) {
                return fail(SRT_ESECFAIL);
            }
            try {
                const std::size_t requested = std::min<std::size_t>(
                    static_cast<std::size_t>(backlog), 512U) * 8U;
                inbox = std::make_shared<HandshakeInbox>(
                    std::clamp<std::size_t>(requested, 16U, 4'096U));
                listener_runtime = std::make_shared<ListenerRuntime>(
                    socket, std::min<std::size_t>(
                        static_cast<std::size_t>(backlog), 4'096U));
            } catch (const std::bad_alloc&) {
                default_crypto_provider().secure_erase(
                    socket->listener_cookie_secret);
                return fail(SRT_ENOBUF);
            } catch (...) {
                default_crypto_provider().secure_erase(
                    socket->listener_cookie_secret);
                return fail(SRT_ESYSOBJ);
            }
            channel = socket->channel;
            socket->listener_inbox = inbox;
            socket->listener_runtime = listener_runtime;
            socket->listen_backlog = backlog;
            socket->state = SRTS_LISTENING;
            break;
        case SRTS_LISTENING:
            return fail(SRT_EDUPLISTEN);
        case SRTS_CONNECTING:
        case SRTS_CONNECTED:
            return fail(SRT_ECONNSOCK);
        case SRTS_CLOSED:
            return fail(SRT_ESCLOSED);
        default:
            return fail(SRT_EINVOP);
        }
    }

    if (!channel->set_listener_inbox(inbox)) {
        inbox->close();
        listener_runtime->close();
        std::lock_guard lock(socket->mutex);
        if (socket->state == SRTS_LISTENING) {
            socket->listener_inbox.reset();
            socket->listener_runtime.reset();
            default_crypto_provider().secure_erase(
                socket->listener_cookie_secret);
            socket->listen_backlog = 0;
            socket->state = SRTS_OPENED;
        }
        return fail(SRT_EDUPLISTEN);
    }
    if (!channel->start() || !listener_runtime->start()) {
        inbox->close();
        listener_runtime->close();
        channel->clear_listener_inbox(inbox);
        bool was_closed = false;
        std::lock_guard lock(socket->mutex);
        was_closed = socket->state == SRTS_CLOSED;
        if (socket->state == SRTS_LISTENING) {
            socket->listener_inbox.reset();
            socket->listener_runtime.reset();
            default_crypto_provider().secure_erase(
                socket->listener_cookie_secret);
            socket->listen_backlog = 0;
            socket->state = SRTS_OPENED;
        }
        return fail(was_closed ? SRT_ESCLOSED : SRT_ETHREAD);
    }
    ReadinessSignal::notify();
    return 0;
}

int set_listen_callback(
    std::shared_ptr<SocketRecord> socket,
    srt_listen_callback_fn* callback,
    void* opaque) noexcept
{
    if (socket == nullptr) {
        return fail(SRT_EINVSOCK);
    }

    std::lock_guard lock(socket->mutex);
    switch (socket->state) {
    case SRTS_INIT:
    case SRTS_OPENED:
        socket->listen_callback = callback;
        socket->listen_callback_opaque =
            callback == nullptr ? nullptr : opaque;
        return 0;
    case SRTS_LISTENING:
    case SRTS_CONNECTING:
    case SRTS_CONNECTED:
        return fail(SRT_ECONNSOCK);
    case SRTS_CLOSED:
        return fail(SRT_EINVSOCK);
    default:
        return fail(SRT_EINVOP);
    }
}

int set_connect_callback(
    std::shared_ptr<SocketRecord> socket,
    srt_connect_callback_fn* callback,
    void* opaque) noexcept
{
    if (socket == nullptr) {
        return fail(SRT_EINVSOCK);
    }

    std::lock_guard lock(socket->mutex);
    switch (socket->state) {
    case SRTS_INIT:
    case SRTS_OPENED:
        socket->connect_callback = callback;
        socket->connect_callback_opaque =
            callback == nullptr ? nullptr : opaque;
        return 0;
    case SRTS_LISTENING:
    case SRTS_CONNECTING:
    case SRTS_CONNECTED:
        return fail(SRT_ECONNSOCK);
    case SRTS_CLOSED:
        return fail(SRT_EINVSOCK);
    default:
        return fail(SRT_EINVOP);
    }
}

namespace {

struct CallerSetup {
    std::shared_ptr<DatagramChannel> channel;
    HandshakeMachine::Configuration configuration;
    SocketOptions options;
    std::shared_ptr<CryptoSession> crypto;
    std::size_t crypto_key_length = 0;
    bool enforced_encryption = true;
    bool data_sender = false;
    std::int32_t timeout_milliseconds = 0;
};

struct RendezvousSetup {
    std::shared_ptr<DatagramChannel> channel;
    RendezvousHandshakeMachine::Configuration configuration;
    SocketOptions options;
    std::shared_ptr<CryptoSession> crypto;
    std::size_t crypto_key_length = 0;
    bool enforced_encryption = true;
    std::int32_t timeout_milliseconds = 0;
};

[[nodiscard]] CallerHandshakeDispatchStatus connect_submission_status(
    RuntimeScheduler::SubmitStatus status) noexcept
{
    switch (status) {
    case RuntimeScheduler::SubmitStatus::accepted:
        return CallerHandshakeDispatchStatus::completed;
    case RuntimeScheduler::SubmitStatus::full:
        return CallerHandshakeDispatchStatus::full;
    case RuntimeScheduler::SubmitStatus::stopped:
        return CallerHandshakeDispatchStatus::stopped;
    case RuntimeScheduler::SubmitStatus::invalid:
        return CallerHandshakeDispatchStatus::invalid;
    }
    return CallerHandshakeDispatchStatus::invalid;
}

class SetupRouteGuard {
public:
    SetupRouteGuard(
        std::shared_ptr<DatagramChannel> channel,
        std::uint32_t protocol_socket_id,
        std::shared_ptr<DatagramInbox> inbox) noexcept
        : channel_(std::move(channel))
        , protocol_socket_id_(protocol_socket_id)
        , inbox_(std::move(inbox))
    {
    }

    ~SetupRouteGuard()
    {
        inbox_->close();
        channel_->unregister_setup_inbox(
            protocol_socket_id_, inbox_);
    }

    SetupRouteGuard(const SetupRouteGuard&) = delete;
    SetupRouteGuard& operator=(
        const SetupRouteGuard&) = delete;

private:
    std::shared_ptr<DatagramChannel> channel_;
    std::uint32_t protocol_socket_id_ = 0;
    std::shared_ptr<DatagramInbox> inbox_;
};

class ListenerSetupOperation final : public ConnectHandshakeOperation {
public:
    explicit ListenerSetupOperation(
        std::shared_ptr<ListenerConnectionSetupActor> actor) noexcept
        : actor_(std::move(actor))
    {
    }

    void close() noexcept override
    {
        actor_->close();
    }

    void wait() noexcept override
    {
        actor_->stop();
    }

    [[nodiscard]] bool running_on_current_thread() const noexcept override
    {
        return false;
    }

private:
    std::shared_ptr<ListenerConnectionSetupActor> actor_;
};

class CallerEventSourceGuard {
public:
    explicit CallerEventSourceGuard(
        std::shared_ptr<CallerHandshakeEventSource> source) noexcept
        : source_(std::move(source))
    {
    }

    ~CallerEventSourceGuard()
    {
        stop();
    }

    CallerEventSourceGuard(const CallerEventSourceGuard&) = delete;
    CallerEventSourceGuard& operator=(const CallerEventSourceGuard&) = delete;

    void stop() noexcept
    {
        if (source_ == nullptr) {
            return;
        }
        source_->stop();
        source_.reset();
    }

private:
    std::shared_ptr<CallerHandshakeEventSource> source_;
};

class HandshakeInboxAcknowledgement {
public:
    void arm(std::shared_ptr<CallerHandshakeEventSource> source) noexcept
    {
        source_ = std::move(source);
    }

    ~HandshakeInboxAcknowledgement()
    {
        if (source_ != nullptr) {
            source_->acknowledge_inbox();
        }
    }

    HandshakeInboxAcknowledgement() = default;
    HandshakeInboxAcknowledgement(
        const HandshakeInboxAcknowledgement&) = delete;
    HandshakeInboxAcknowledgement& operator=(
        const HandshakeInboxAcknowledgement&) = delete;

private:
    std::shared_ptr<CallerHandshakeEventSource> source_;
};

[[nodiscard]] std::shared_ptr<DatagramInbox>
create_setup_inbox() noexcept
{
    try {
        return std::make_shared<DatagramInbox>(
            setup_inbox_capacity);
    } catch (...) {
        return {};
    }
}

[[nodiscard]] bool enqueue_setup_handshake(
    DatagramInbox& inbox, const HandshakeEnvelope& envelope) noexcept
{
    if (envelope.message.has_unknown_extension) {
        return false;
    }
    const HandshakeAction action {
        .kind = HandshakeActionKind::send,
        .packet = envelope.message.packet,
        .has_handshake_extension = envelope.message.has_handshake_extension,
        .extension_type = envelope.message.extension_type,
        .extension_parameters = envelope.message.extension_parameters,
        .has_key_material_extension =
            envelope.message.has_key_material_extension,
        .key_material_extension_type =
            envelope.message.key_material_extension_type,
        .key_material = envelope.message.key_material,
        .has_stream_id_extension = envelope.message.has_stream_id_extension,
        .stream_id = envelope.message.stream_id,
        .has_congestion_extension = envelope.message.has_congestion_extension,
        .congestion_controller = envelope.message.congestion_controller,
        .has_packet_filter_extension =
            envelope.message.has_packet_filter_extension,
        .packet_filter_configuration =
            envelope.message.packet_filter_configuration,
        .has_group_membership = envelope.message.has_group_membership,
        .group_membership = envelope.message.group_membership,
    };
    std::array<std::byte, DatagramEnvelope::maximum_size> bytes {};
    const HandshakeDatagramEncodeResult encoded =
        encode_handshake_datagram(action, envelope.control.timestamp,
            envelope.control.destination_socket_id, bytes);
    return encoded
        && inbox.push(
            std::span {bytes}.first(encoded.bytes_written), envelope.peer);
}

// Shared HSv5 Caller establishment boundary. Blocking and asynchronous
// transports own different wait/readiness mechanics, but protocol mutation,
// crypto policy, terminal error mapping, group admission, and runtime
// publication must remain one contract.
class CallerHsv5Establishment {
public:
    CallerHsv5Establishment(SocketRecord& socket, IpEndpoint peer,
        CallerSetup& setup, std::shared_ptr<DatagramInbox> inbox,
        Clock::time_point origin, CallerHandshakeSender sender,
        void* sender_context, bool asynchronous) noexcept
        : socket_(socket)
        , peer_(peer)
        , setup_(setup)
        , inbox_(std::move(inbox))
        , origin_(origin)
        , asynchronous_(asynchronous)
        , driver_(setup.configuration, sender, sender_context)
    {
    }

    [[nodiscard]] CallerHandshakeEventResult dispatch(
        const CallerHandshakeEvent& event) noexcept
    {
        if (event.has_not_after && event.kind != CallerHandshakeEventKind::close
            && event.kind != CallerHandshakeEventKind::overall_timeout
            && Clock::now() >= event.not_after) {
            return {.outcome = CallerHandshakeEventOutcome::timed_out};
        }
        return driver_.dispatch(event);
    }

    [[nodiscard]] int validate_peer_protocol(
        const HandshakeMessage& message) noexcept
    {
        if (driver_.protocol().is_stale_induction(message)) {
            return 0;
        }
        if (message.packet.request == HandshakeRequest::induction
            && message.packet.syn_cookie != 0U
            && classify_caller_induction_response(message)
                == ListenerHandshakeProtocol::unsupported_hsv4) {
            return fail_connect(
                socket_, SRT_ECONNREJ, 0, asynchronous_, SRT_REJ_VERSION);
        }
        return 0;
    }

    [[nodiscard]] int apply_crypto_policy(const HandshakeMessage& message,
        Clock::time_point overall_deadline) noexcept
    {
        if (driver_.protocol().is_stale_induction(message)) {
            return 0;
        }
        if (message.packet.request == HandshakeRequest::induction
            && setup_.crypto != nullptr
            && message.packet.encryption_field != 0U) {
            const std::size_t peer_key_length = key_length_for_encryption_field(
                message.packet.encryption_field);
            if (peer_key_length == 0U) {
                return fail_connect(
                    socket_, SRT_ESECFAIL, 0, asynchronous_, SRT_REJ_BADSECRET);
            }
            if (peer_key_length != setup_.crypto_key_length) {
                const int replaced = replace_crypto(peer_key_length,
                    message.packet.encryption_field, overall_deadline);
                if (replaced == SRT_ERROR) {
                    return SRT_ERROR;
                }
            }
        }
        if (message.packet.request != HandshakeRequest::conclusion) {
            return 0;
        }
        if (setup_.crypto != nullptr) {
            if (key_length_for_encryption_field(message.packet.encryption_field)
                    != setup_.crypto_key_length
                || !message.has_key_material_extension
                || message.key_material_extension_type
                    != HandshakeExtensionType::key_material_response
                || setup_.crypto->acknowledge_key_material(
                       message.key_material.view(), true)
                    != Error::none) {
                if (setup_.enforced_encryption
                    || !setup_.crypto->allows_plaintext_fallback()) {
                    return fail_connect(socket_, SRT_ESECFAIL, 0, asynchronous_,
                        crypto_rejection_reason(
                            *setup_.crypto, SRT_REJ_BADSECRET));
                }
                setup_.crypto.reset();
                std::lock_guard lock(socket_.mutex);
                socket_.crypto.reset();
            }
        } else if (message.has_key_material_extension
            && setup_.enforced_encryption) {
            return fail_connect(
                socket_, SRT_ESECFAIL, 0, asynchronous_, SRT_REJ_UNSECURE);
        }
        return 0;
    }

    [[nodiscard]] int publish_connection(const HandshakeMessage& message,
        PacketTimestamp peer_handshake_timestamp) noexcept
    {
        const HandshakeMachine& protocol = driver_.protocol();
        if (setup_.configuration.has_group_membership) {
            SRTSOCKET group = SRT_INVALID_SOCK;
            std::uint64_t group_generation = 0;
            {
                std::lock_guard lock(socket_.mutex);
                group = socket_.group_id;
                group_generation = socket_.group_generation;
            }
            if (!protocol.has_peer_group_membership()
                || !GroupRegistry::instance().set_peer_group(group,
                    group_generation,
                    static_cast<SRTSOCKET>(
                        protocol.peer_group_membership().group_id))) {
                return fail_connect(
                    socket_, SRT_ECONNREJ, 0, asynchronous_, SRT_REJ_GROUP);
            }
        }
        finish_connect(socket_, SRTS_CONNECTED, peer_, driver_.peer_socket_id(),
            message.packet.maximum_transmission_unit, protocol);
        if (socket_was_closed(socket_)) {
            return fail(SRT_ESCLOSED);
        }
        if (attach_runtime(socket_, setup_.channel, origin_,
                microseconds_since(origin_), peer_handshake_timestamp, nullptr,
                0U, inbox_)
            == SRT_ERROR) {
            return fail_connect(socket_,
                static_cast<SRT_ERRNO>(last_error().code),
                last_error().system_error, asynchronous_);
        }
        return 0;
    }

    [[nodiscard]] int fail_dispatch(
        CallerHandshakeDispatchStatus status) noexcept
    {
        return fail_connect(socket_,
            status == CallerHandshakeDispatchStatus::full
                    || status
                        == CallerHandshakeDispatchStatus::allocation_failed
                ? SRT_ENOBUF
                : SRT_ETHREAD,
            0, asynchronous_);
    }

    [[nodiscard]] int fail_event(
        const CallerHandshakeEventResult& result) noexcept
    {
        switch (result.outcome) {
        case CallerHandshakeEventOutcome::rejected:
            return fail_connect(socket_, SRT_ECONNREJ, 0, asynchronous_,
                result.rejection_reason);
        case CallerHandshakeEventOutcome::failed:
            return fail_connect(socket_, SRT_ECONNSETUP, 0, asynchronous_);
        case CallerHandshakeEventOutcome::io_error:
            return fail_connect(
                socket_, SRT_ESOCKFAIL, result.system_error, asynchronous_);
        case CallerHandshakeEventOutcome::closed:
            return fail_connect(socket_, SRT_ESCLOSED, 0, asynchronous_);
        case CallerHandshakeEventOutcome::timed_out:
            return fail_connect(socket_, SRT_ENOSERVER, 0, asynchronous_);
        case CallerHandshakeEventOutcome::invalid:
        case CallerHandshakeEventOutcome::connected:
        case CallerHandshakeEventOutcome::running:
            return fail_connect(socket_, SRT_ECONNSETUP, 0, asynchronous_);
        }
        return fail_connect(socket_, SRT_ECONNSETUP, 0, asynchronous_);
    }

private:
    [[nodiscard]] int replace_crypto(std::size_t peer_key_length,
        std::uint16_t encryption_field,
        Clock::time_point overall_deadline) noexcept
    {
        std::shared_ptr<CryptoSession> replacement;
        try {
            auto configuration = setup_.options.crypto_configuration();
            configuration.key_length = peer_key_length;
            replacement = std::make_shared<CryptoSession>(configuration);
        } catch (const std::bad_alloc&) {
            return fail_connect(socket_, SRT_ENOBUF, 0, asynchronous_);
        } catch (...) {
            return fail_connect(socket_, SRT_ESYSOBJ, 0, asynchronous_);
        }
        if (replacement->start_initiator() != Error::none) {
            return fail_connect(socket_, SRT_ESECFAIL, 0, asynchronous_,
                crypto_rejection_reason(*replacement, SRT_REJ_BADSECRET));
        }
        KeyMaterialBuffer request;
        const auto material = replacement->pending_key_material();
        std::copy(material.begin(), material.end(), request.bytes.begin());
        request.size = material.size();
        const CallerHandshakeEventResult result = dispatch({
            .kind = CallerHandshakeEventKind::key_material,
            .key_material = request,
            .not_after = overall_deadline,
            .encryption_field = encryption_field,
            .has_not_after = true,
        });
        if (result.error != Error::none) {
            return fail_connect(
                socket_, SRT_ESECFAIL, 0, asynchronous_, SRT_REJ_BADSECRET);
        }
        if (result.outcome != CallerHandshakeEventOutcome::running) {
            return fail_event(result);
        }
        setup_.crypto = std::move(replacement);
        setup_.crypto_key_length = peer_key_length;
        std::lock_guard lock(socket_.mutex);
        socket_.crypto = setup_.crypto;
        return 0;
    }

    SocketRecord& socket_;
    IpEndpoint peer_ {};
    CallerSetup& setup_;
    std::shared_ptr<DatagramInbox> inbox_;
    Clock::time_point origin_ {};
    bool asynchronous_ = false;
    CallerHandshakeEventDriver driver_;
};

// Serial, affinity-bound owner for one asynchronous HSv5 Caller handshake.
// Source readiness, protocol mutation, crypto policy, transport publication,
// callback ordering, and teardown all execute as one actor. Arbitrary
// application callback code leaves the shared shard only after the terminal
// transition. A genuine HSv4 discovery response terminates the actor with the
// public Version rejection; no downgrade state machine is entered.
class AsyncCallerHandshakeActor final
    : public ConnectHandshakeOperation
    , public std::enable_shared_from_this<AsyncCallerHandshakeActor> {
public:
    AsyncCallerHandshakeActor(std::shared_ptr<RuntimeScheduler> scheduler,
        std::shared_ptr<SocketRecord> socket, IpEndpoint peer,
        IpEndpoint callback_peer, CallerSetup setup) noexcept
        : scheduler_(std::move(scheduler))
        , socket_(std::move(socket))
        , peer_(peer)
        , callback_peer_(callback_peer)
        , setup_(std::move(setup))
    {
    }

    [[nodiscard]] CallerHandshakeDispatchStatus start() noexcept
    {
        std::shared_ptr<AsyncCallerHandshakeActor> owner;
        try {
            owner = shared_from_this();
        } catch (...) {
            return CallerHandshakeDispatchStatus::allocation_failed;
        }
        {
            std::lock_guard lock(state_mutex_);
            if (scheduled_ || completed_ || finishing_) {
                return CallerHandshakeDispatchStatus::invalid;
            }
            pending_ = true;
            scheduled_ = true;
        }
        const RuntimeScheduler::SubmitStatus submitted =
            scheduler_->submit(setup_.configuration.local_socket_id,
                {
                    .function = run_task,
                    .context = std::move(owner),
                });
        if (submitted == RuntimeScheduler::SubmitStatus::accepted) {
            return CallerHandshakeDispatchStatus::completed;
        }
        {
            std::lock_guard lock(state_mutex_);
            pending_ = false;
            scheduled_ = false;
            completed_ = true;
        }
        completed_signal_.notify_all();
        return connect_submission_status(submitted);
    }

    void close() noexcept override
    {
        {
            std::lock_guard lock(state_mutex_);
            if (completed_ || finishing_) {
                return;
            }
            close_requested_ = true;
        }
        request_run();
    }

    void wait() noexcept override
    {
        std::unique_lock lock(state_mutex_);
        completed_signal_.wait(lock, [this] {
            return completed_;
        });
    }

    [[nodiscard]] bool running_on_current_thread() const noexcept override
    {
        std::lock_guard lock(state_mutex_);
        const std::thread::id current = std::this_thread::get_id();
        return active_thread_ == current || callback_thread_ == current;
    }

private:
    static void run_task(void* context) noexcept
    {
        static_cast<AsyncCallerHandshakeActor*>(context)->run();
    }

    static void source_ready(void* context) noexcept
    {
        static_cast<AsyncCallerHandshakeActor*>(context)->request_run();
    }

    [[nodiscard]] CallerHandshakeEventResult dispatch(
        const CallerHandshakeEvent& event) noexcept
    {
        return establishment_->dispatch(event);
    }

    [[nodiscard]] CallerHandshakeDispatchStatus arm_retry(
        const CallerHandshakeEventResult& result) noexcept
    {
        if (!result.arm_retry_timer) {
            return CallerHandshakeDispatchStatus::completed;
        }
        return source_->arm_retry(result.retry_timeout_milliseconds);
    }

    void request_run() noexcept
    {
        std::shared_ptr<AsyncCallerHandshakeActor> owner;
        {
            std::lock_guard lock(state_mutex_);
            if (completed_ || finishing_) {
                return;
            }
            pending_ = true;
            if (scheduled_) {
                return;
            }
            scheduled_ = true;
        }
        try {
            owner = shared_from_this();
        } catch (...) {
            handle_submission_failure(
                CallerHandshakeDispatchStatus::allocation_failed);
            return;
        }
        const RuntimeScheduler::SubmitStatus submitted =
            scheduler_->submit(setup_.configuration.local_socket_id,
                {
                    .function = run_task,
                    .context = std::move(owner),
                });
        if (submitted != RuntimeScheduler::SubmitStatus::accepted) {
            handle_submission_failure(connect_submission_status(submitted));
        }
    }

    void handle_submission_failure(
        CallerHandshakeDispatchStatus status) noexcept
    {
        {
            std::lock_guard lock(state_mutex_);
            if (completed_ || finishing_) {
                return;
            }
            pending_ = false;
            scheduled_ = false;
            active_thread_ = std::this_thread::get_id();
        }
        finish_dispatch_failure(status);
        std::lock_guard lock(state_mutex_);
        active_thread_ = {};
    }

    void run() noexcept
    {
        {
            std::lock_guard lock(state_mutex_);
            active_thread_ = std::this_thread::get_id();
        }
        for (;;) {
            bool close_requested = false;
            {
                std::lock_guard lock(state_mutex_);
                pending_ = false;
                close_requested = close_requested_;
            }
            if (close_requested) {
                if (establishment_.has_value()) {
                    (void)dispatch({.kind = CallerHandshakeEventKind::close});
                }
                finish(fail_connect(*socket_, SRT_ESCLOSED, 0, true));
            } else if (!initialized_) {
                initialize();
            } else {
                drain_events();
            }

            std::lock_guard lock(state_mutex_);
            if (completed_ || finishing_) {
                scheduled_ = false;
                active_thread_ = {};
                return;
            }
            if (!pending_) {
                scheduled_ = false;
                active_thread_ = {};
                return;
            }
        }
    }

    void initialize() noexcept
    {
        if (socket_was_closed(*socket_)) {
            finish(fail(SRT_ESCLOSED));
            return;
        }
        inbox_ = create_setup_inbox();
        if (inbox_ == nullptr) {
            finish(fail_connect(*socket_, SRT_ENOBUF, 0, true));
            return;
        }
        if (!setup_.channel->register_setup_inbox(
                setup_.configuration.local_socket_id, peer_, inbox_)) {
            finish(fail_connect(*socket_, SRT_ECONNSETUP, 0, true));
            return;
        }
        route_registered_ = true;
        if (!setup_.channel->start()) {
            finish(fail_connect(*socket_, SRT_ETHREAD, 0, true));
            return;
        }

        const auto connection_start = Clock::now();
        origin_ = socket_timestamp_origin(*socket_, connection_start);
        overall_deadline_ = connection_start
            + std::chrono::milliseconds {setup_.timeout_milliseconds};
        send_context_ = {
            .channel = setup_.channel.get(),
            .peer = peer_,
            .origin = origin_,
        };
        establishment_.emplace(*socket_, peer_, setup_, inbox_, origin_,
            send_caller_handshake_action, &send_context_, true);
        try {
            source_ = std::make_shared<CallerHandshakeEventSource>(
                *scheduler_, setup_.configuration.local_socket_id, inbox_);
        } catch (...) {
            finish(fail_connect(*socket_, SRT_ENOBUF, 0, true));
            return;
        }
        if (!source_->set_event_ready_handler(
                source_ready, shared_from_this())) {
            finish(fail_connect(*socket_, SRT_ECONNSETUP, 0, true));
            return;
        }
        const CallerHandshakeDispatchStatus started =
            source_->start(overall_deadline_);
        if (started != CallerHandshakeDispatchStatus::completed) {
            finish_dispatch_failure(started);
            return;
        }
        initialized_ = true;

        const CallerHandshakeEventResult result = dispatch({
            .kind = CallerHandshakeEventKind::start,
            .not_after = overall_deadline_,
            .has_not_after = true,
        });
        const CallerHandshakeDispatchStatus armed = arm_retry(result);
        if (armed != CallerHandshakeDispatchStatus::completed) {
            finish_dispatch_failure(armed);
            return;
        }
        if (result.outcome != CallerHandshakeEventOutcome::running) {
            finish_event(result);
        }
    }

    void drain_events() noexcept
    {
        CallerHandshakeSourceEvent event;
        while (source_ != nullptr && source_->try_pop(event)) {
            if (!process_event(event)) {
                return;
            }
        }
    }

    [[nodiscard]] bool process_event(
        const CallerHandshakeSourceEvent& event) noexcept
    {
        switch (event.kind) {
        case CallerHandshakeSourceEventKind::failure:
            finish_dispatch_failure(event.failure);
            return false;
        case CallerHandshakeSourceEventKind::closed:
            (void)dispatch({.kind = CallerHandshakeEventKind::close});
            finish(fail_connect(*socket_, SRT_ESCLOSED, 0, true));
            return false;
        case CallerHandshakeSourceEventKind::overall_timeout:
            (void)dispatch({.kind = CallerHandshakeEventKind::overall_timeout});
            finish(fail_connect(*socket_, SRT_ENOSERVER, 0, true));
            return false;
        case CallerHandshakeSourceEventKind::retry_timer: {
            const CallerHandshakeEventResult result = dispatch({
                .kind = CallerHandshakeEventKind::retry_timer,
                .not_after = overall_deadline_,
                .has_not_after = true,
            });
            const CallerHandshakeDispatchStatus armed = arm_retry(result);
            if (armed != CallerHandshakeDispatchStatus::completed) {
                finish_dispatch_failure(armed);
                return false;
            }
            if (result.outcome != CallerHandshakeEventOutcome::running) {
                finish_event(result);
                return false;
            }
            return true;
        }
        case CallerHandshakeSourceEventKind::inbox_ready:
            return process_inbox();
        }
        finish(fail_connect(*socket_, SRT_ECONNSETUP, 0, true));
        return false;
    }

    [[nodiscard]] bool process_inbox() noexcept
    {
        HandshakeInboxAcknowledgement acknowledgement;
        acknowledgement.arm(source_);
        DatagramEnvelope envelope;
        const InboxPopStatus received =
            inbox_->pop_for(envelope, std::chrono::milliseconds {0});
        if (received == InboxPopStatus::timeout) {
            return true;
        }
        if (received == InboxPopStatus::closed) {
            source_->close();
            (void)dispatch({.kind = CallerHandshakeEventKind::close});
            finish(fail_connect(*socket_, SRT_ESCLOSED, 0, true));
            return false;
        }
        if (!same_endpoint(envelope.peer, peer_)) {
            return true;
        }
        const auto decoded = decode_handshake_datagram(
            std::span {envelope.bytes}.first(envelope.size));
        if (!decoded) {
            return true;
        }

        if (establishment_->validate_peer_protocol(decoded.message)
            == SRT_ERROR) {
            finish(SRT_ERROR);
            return false;
        }
        if (establishment_->apply_crypto_policy(
                decoded.message, overall_deadline_)
            == SRT_ERROR) {
            finish(SRT_ERROR);
            return false;
        }

        const CallerHandshakeEventResult result = dispatch({
            .kind = CallerHandshakeEventKind::inbox_ready,
            .message = decoded.message,
            .not_after = overall_deadline_,
            .has_not_after = true,
        });
        const CallerHandshakeDispatchStatus armed = arm_retry(result);
        if (armed != CallerHandshakeDispatchStatus::completed) {
            finish_dispatch_failure(armed);
            return false;
        }
        if (result.outcome == CallerHandshakeEventOutcome::connected) {
            finish(establishment_->publish_connection(
                decoded.message, decoded.control.timestamp));
            return false;
        }
        if (result.outcome != CallerHandshakeEventOutcome::running) {
            finish_event(result);
            return false;
        }
        return true;
    }

    void finish_dispatch_failure(CallerHandshakeDispatchStatus status) noexcept
    {
        if (establishment_.has_value()) {
            finish(establishment_->fail_dispatch(status));
            return;
        }
        finish(fail_connect(*socket_,
            status == CallerHandshakeDispatchStatus::full
                    || status
                        == CallerHandshakeDispatchStatus::allocation_failed
                ? SRT_ENOBUF
                : SRT_ETHREAD,
            0, true));
    }

    void finish_event(const CallerHandshakeEventResult& result) noexcept
    {
        finish(establishment_->fail_event(result));
    }

    void finish(int result) noexcept
    {
        {
            std::lock_guard lock(state_mutex_);
            if (completed_ || finishing_) {
                return;
            }
            finishing_ = true;
        }
        const int error_code = result == 0 ? SRT_SUCCESS : last_error().code;
        if (source_ != nullptr) {
            source_->clear_event_ready_handler();
            source_->stop();
            source_.reset();
        }
        if (inbox_ != nullptr) {
            inbox_->close();
        }
        if (route_registered_) {
            setup_.channel->unregister_setup_inbox(
                setup_.configuration.local_socket_id, inbox_);
            route_registered_ = false;
        }
        bool callback_registered = false;
        {
            std::lock_guard lock(socket_->mutex);
            callback_registered = socket_->connect_callback != nullptr;
        }
        if (callback_registered && start_callback_worker(error_code)) {
            return;
        }
        run_callback(error_code);
    }

    [[nodiscard]] bool start_callback_worker(int error_code) noexcept
    {
        std::thread worker;
        {
            std::lock_guard lock(socket_->mutex);
            try {
                worker = std::thread([owner = shared_from_this(), error_code] {
                    owner->run_callback(error_code);
                });
            } catch (...) {
                return false;
            }
            socket_->connect_worker = std::move(worker);
        }
        return true;
    }

    void run_callback(int error_code) noexcept
    {
        {
            std::lock_guard lock(state_mutex_);
            callback_thread_ = std::this_thread::get_id();
        }
        invoke_connect_callback(socket_, callback_peer_, error_code);
        {
            std::lock_guard lock(socket_->mutex);
            if (socket_->connect_worker.joinable()
                && socket_->connect_worker.get_id()
                    == std::this_thread::get_id()) {
                socket_->connect_worker.detach();
            }
            if (socket_->connect_handshake_operation.get() == this) {
                socket_->connect_handshake_operation.reset();
            }
        }
        {
            std::lock_guard lock(state_mutex_);
            finishing_ = false;
            completed_ = true;
            callback_thread_ = {};
        }
        completed_signal_.notify_all();
    }

    std::shared_ptr<RuntimeScheduler> scheduler_;
    std::shared_ptr<SocketRecord> socket_;
    IpEndpoint peer_ {};
    IpEndpoint callback_peer_ {};
    CallerSetup setup_;
    std::shared_ptr<DatagramInbox> inbox_;
    std::shared_ptr<CallerHandshakeEventSource> source_;
    std::optional<CallerHsv5Establishment> establishment_;
    CallerHandshakeSendContext send_context_;
    Clock::time_point origin_ {};
    Clock::time_point overall_deadline_ {};
    mutable std::mutex state_mutex_;
    std::condition_variable completed_signal_;
    std::thread::id active_thread_ {};
    std::thread::id callback_thread_ {};
    bool pending_ = false;
    bool scheduled_ = false;
    bool initialized_ = false;
    bool route_registered_ = false;
    bool close_requested_ = false;
    bool finishing_ = false;
    bool completed_ = false;
};

[[nodiscard]] int run_caller_handshake(
    const std::shared_ptr<SocketRecord>& socket, IpEndpoint peer,
    CallerSetup setup) noexcept
{
    const auto inbox = create_setup_inbox();
    if (inbox == nullptr) {
        return fail_connect(*socket, SRT_ENOBUF);
    }
    if (!setup.channel->register_setup_inbox(
            setup.configuration.local_socket_id,
            peer, inbox)) {
        return fail_connect(*socket, SRT_ECONNSETUP);
    }
    SetupRouteGuard setup_route{
        setup.channel,
        setup.configuration.local_socket_id,
        inbox,
    };
    if (!setup.channel->start()) {
        return fail_connect(*socket, SRT_ETHREAD);
    }

    const Clock::time_point connection_start = Clock::now();
    const Clock::time_point origin =
        socket_timestamp_origin(*socket, connection_start);
    const Clock::time_point overall_deadline = connection_start
        + std::chrono::milliseconds {setup.timeout_milliseconds};
    Clock::time_point retry_deadline = connection_start
        + std::chrono::milliseconds {retry_interval_milliseconds};
    CallerHandshakeSendContext send_context {
        .channel = setup.channel.get(),
        .peer = peer,
        .origin = origin,
    };
    CallerHsv5Establishment establishment {*socket, peer, setup, inbox, origin,
        send_caller_handshake_action, &send_context, false};
    const auto arm_retry = [&retry_deadline](
                               const CallerHandshakeEventResult& result) {
        if (!result.arm_retry_timer) {
            return;
        }
        retry_deadline = Clock::now()
            + std::chrono::milliseconds {result.retry_timeout_milliseconds};
    };

    CallerHandshakeEventResult action_result = establishment.dispatch({
        .kind = CallerHandshakeEventKind::start,
        .not_after = overall_deadline,
        .has_not_after = true,
    });
    arm_retry(action_result);
    if (action_result.outcome != CallerHandshakeEventOutcome::running) {
        return establishment.fail_event(action_result);
    }

    for (;;) {
        if (socket_was_closed(*socket)) {
            (void)establishment.dispatch(
                {.kind = CallerHandshakeEventKind::close});
            return fail(SRT_ESCLOSED);
        }

        if (Clock::now() >= overall_deadline) {
            break;
        }
        if (Clock::now() >= retry_deadline) {
            action_result = establishment.dispatch({
                .kind = CallerHandshakeEventKind::retry_timer,
                .not_after = overall_deadline,
                .has_not_after = true,
            });
            arm_retry(action_result);
            if (action_result.outcome != CallerHandshakeEventOutcome::running) {
                return establishment.fail_event(action_result);
            }
        }

        DatagramEnvelope envelope;
        const InboxPopStatus received = inbox->pop_for(envelope,
            std::chrono::milliseconds {
                wait_duration(retry_deadline, overall_deadline)});
        if (received == InboxPopStatus::timeout) {
            continue;
        }
        if (received == InboxPopStatus::closed) {
            (void)establishment.dispatch(
                {.kind = CallerHandshakeEventKind::close});
            return fail_connect(*socket, SRT_ESCLOSED);
        }
        const auto decoded = decode_handshake_datagram(
            std::span{envelope.bytes}.first(
                envelope.size));
        if (!decoded
            || !same_endpoint(envelope.peer, peer)) {
            continue;
        }

        if (establishment.validate_peer_protocol(decoded.message) == SRT_ERROR
            || establishment.apply_crypto_policy(
                   decoded.message, overall_deadline)
                == SRT_ERROR) {
            return SRT_ERROR;
        }
        action_result = establishment.dispatch({
            .kind = CallerHandshakeEventKind::inbox_ready,
            .message = decoded.message,
            .not_after = overall_deadline,
            .has_not_after = true,
        });
        arm_retry(action_result);
        if (action_result.outcome == CallerHandshakeEventOutcome::connected) {
            return establishment.publish_connection(
                decoded.message, decoded.control.timestamp);
        }
        if (action_result.outcome != CallerHandshakeEventOutcome::running) {
            return establishment.fail_event(action_result);
        }
    }
    action_result = establishment.dispatch({
        .kind = CallerHandshakeEventKind::overall_timeout,
    });
    return establishment.fail_event(action_result);
}

class RendezvousHandshakeSession {
public:
    RendezvousHandshakeSession(std::shared_ptr<SocketRecord> socket,
        IpEndpoint peer, RendezvousSetup setup,
        std::shared_ptr<DatagramInbox> inbox, bool asynchronous) noexcept
        : socket_(std::move(socket))
        , peer_(peer)
        , setup_(std::move(setup))
        , inbox_(std::move(inbox))
        , machine_(setup_.configuration)
        , asynchronous_(asynchronous)
        , origin_(Clock::now())
        , overall_deadline_(
              origin_ + std::chrono::milliseconds {setup_.timeout_milliseconds})
        , retry_deadline_(
              origin_ + std::chrono::milliseconds {retry_interval_milliseconds})
    {
    }

    [[nodiscard]] Clock::time_point overall_deadline() const noexcept
    {
        return overall_deadline_;
    }

    [[nodiscard]] Clock::time_point retry_deadline() const noexcept
    {
        return retry_deadline_;
    }

    [[nodiscard]] std::optional<int> start() noexcept
    {
        const ActionResult result = apply_actions(*setup_.channel, peer_, 0U,
            machine_.start(), origin_, retry_deadline_);
        if (result.outcome == ActionOutcome::io_error) {
            return fail_connect(
                *socket_, SRT_ESOCKFAIL, result.system_error, asynchronous_);
        }
        remember_replay(result);
        return std::nullopt;
    }

    [[nodiscard]] std::optional<int> timeout() noexcept
    {
        const ActionResult result = apply_actions(*setup_.channel, peer_,
            peer_socket_id_, machine_.timeout(), origin_, retry_deadline_);
        if (result.outcome == ActionOutcome::io_error) {
            return fail_connect(
                *socket_, SRT_ESOCKFAIL, result.system_error, asynchronous_);
        }
        if (result.outcome == ActionOutcome::failed) {
            return fail_connect(*socket_, SRT_ENOSERVER, 0, asynchronous_);
        }
        remember_replay(result);
        return std::nullopt;
    }

    [[nodiscard]] std::optional<int> receive(
        const DatagramEnvelope& envelope) noexcept
    {
        if (!same_endpoint(envelope.peer, peer_)) {
            return std::nullopt;
        }
        const auto wire_packet =
            decode_packet(std::span {envelope.bytes}.first(envelope.size));
        if (!wire_packet) {
            return std::nullopt;
        }
        if (wire_packet.packet.kind != PacketKind::control
            || wire_packet.packet.control.type != ControlType::handshake) {
            const std::uint32_t destination_socket_id =
                wire_packet.packet.kind == PacketKind::data
                ? wire_packet.packet.data.destination_socket_id
                : wire_packet.packet.control.destination_socket_id;
            if (destination_socket_id != setup_.configuration.local_socket_id
                || machine_.role() != RendezvousRole::responder
                || (machine_.state() != RendezvousState::initiated
                    && machine_.state() != RendezvousState::fine)) {
                return std::nullopt;
            }
            const ActionResult result =
                apply_actions(*setup_.channel, peer_, peer_socket_id_,
                    machine_.note_connected_packet(), origin_, retry_deadline_);
            if (result.outcome == ActionOutcome::connected) {
                return complete_connection(
                    peer_handshake_timestamp_, &wire_packet.packet);
            }
            if (result.outcome == ActionOutcome::io_error) {
                return fail_connect(*socket_, SRT_ESOCKFAIL,
                    result.system_error, asynchronous_);
            }
            if (result.outcome == ActionOutcome::failed) {
                return fail_connect(*socket_, SRT_ECONNSETUP, 0, asynchronous_);
            }
            return std::nullopt;
        }

        const auto decoded = decode_handshake_datagram(
            std::span {envelope.bytes}.first(envelope.size));
        if (!decoded) {
            return std::nullopt;
        }
        peer_maximum_segment_size_ =
            decoded.message.packet.maximum_transmission_unit;
        peer_handshake_timestamp_ = decoded.control.timestamp;
        peer_socket_id_ = decoded.message.packet.socket_id;

        const bool wire_rejection =
            static_cast<std::int32_t>(decoded.message.packet.request) >= 1'000;
        RendezvousRole event_role = machine_.role();
        if (!role_prepared_ && !wire_rejection) {
            const RendezvousRole resolved =
                resolve_rendezvous_role(setup_.configuration.local_cookie,
                    decoded.message.packet.syn_cookie);
            if (resolved != RendezvousRole::unresolved
                && setup_.crypto != nullptr) {
                const std::uint16_t advertised =
                    decoded.message.packet.encryption_field;
                const std::size_t peer_key_length = advertised == 0U
                    ? 0U
                    : key_length_for_encryption_field(advertised);
                if (advertised != 0U && peer_key_length == 0U) {
                    return fail_connect(*socket_, SRT_ESECFAIL, 0,
                        asynchronous_, SRT_REJ_BADSECRET);
                }
                const std::size_t selected_key_length =
                    resolved == RendezvousRole::initiator
                        && peer_key_length != 0U
                    ? peer_key_length
                    : setup_.crypto_key_length;
                if (install_crypto(selected_key_length,
                        resolved == RendezvousRole::initiator)
                    == SRT_ERROR) {
                    return SRT_ERROR;
                }
                if (resolved == RendezvousRole::initiator) {
                    KeyMaterialBuffer request;
                    const auto material = setup_.crypto->pending_key_material();
                    std::copy(material.begin(), material.end(),
                        request.bytes.begin());
                    request.size = material.size();
                    if (machine_.set_key_material_request(request,
                            encryption_field_for_key_length(
                                selected_key_length))
                        != Error::none) {
                        return fail_connect(*socket_, SRT_ESECFAIL, 0,
                            asynchronous_, SRT_REJ_BADSECRET);
                    }
                } else {
                    const KeyMaterialBuffer empty_request;
                    if (machine_.set_key_material_request(empty_request,
                            encryption_field_for_key_length(
                                selected_key_length))
                        != Error::none) {
                        return fail_connect(*socket_, SRT_ESECFAIL, 0,
                            asynchronous_, SRT_REJ_BADSECRET);
                    }
                }
            }
            role_prepared_ = resolved != RendezvousRole::unresolved;
            event_role = resolved;
        }

        const bool handshake_request = decoded.message.has_handshake_extension
            && decoded.message.extension_type
                == HandshakeExtensionType::handshake_request;
        const bool handshake_response = decoded.message.has_handshake_extension
            && decoded.message.extension_type
                == HandshakeExtensionType::handshake_response;
        if (event_role == RendezvousRole::responder && handshake_request) {
            const bool valid_key_material_request =
                decoded.message.has_key_material_extension
                && decoded.message.key_material_extension_type
                    == HandshakeExtensionType::key_material_request;
            if (setup_.crypto == nullptr) {
                if (valid_key_material_request) {
                    if (setup_.enforced_encryption) {
                        return fail_connect(*socket_, SRT_ESECFAIL, 0,
                            asynchronous_, SRT_REJ_UNSECURE);
                    }
                    if (set_key_material_failure(CryptoState::no_secret)
                        == SRT_ERROR) {
                        return SRT_ERROR;
                    }
                } else if (decoded.message.has_key_material_extension) {
                    return fail_connect(*socket_, SRT_ESECFAIL, 0,
                        asynchronous_, SRT_REJ_BADSECRET);
                }
            } else if (!valid_key_material_request) {
                if (setup_.enforced_encryption
                    || !setup_.crypto->allows_plaintext_fallback()) {
                    return fail_connect(*socket_, SRT_ESECFAIL, 0,
                        asynchronous_, SRT_REJ_BADSECRET);
                }
                if (clear_crypto() == SRT_ERROR) {
                    return SRT_ERROR;
                }
            } else {
                const auto material =
                    decode_key_material(decoded.message.key_material.view());
                const bool invalid_material = !material
                    || material.key_material.key_length
                        != setup_.crypto_key_length;
                const Error accepted = invalid_material
                    ? Error::invalid_key_material
                    : setup_.crypto->accept_key_material(
                          decoded.message.key_material.view(), true);
                if (accepted != Error::none) {
                    if (!setup_.enforced_encryption
                        && setup_.crypto->allows_plaintext_fallback()) {
                        if (set_key_material_failure(invalid_material
                                    ? CryptoState::bad_crypto_mode
                                    : CryptoState::bad_secret)
                                == SRT_ERROR
                            || clear_crypto() == SRT_ERROR) {
                            return SRT_ERROR;
                        }
                    } else {
                        return fail_connect(*socket_, SRT_ESECFAIL, 0,
                            asynchronous_,
                            crypto_rejection_reason(
                                *setup_.crypto, SRT_REJ_BADSECRET));
                    }
                }
            }
        }
        if (event_role == RendezvousRole::initiator && handshake_response) {
            const bool valid_key_material_response =
                decoded.message.has_key_material_extension
                && decoded.message.key_material_extension_type
                    == HandshakeExtensionType::key_material_response;
            if (setup_.crypto != nullptr) {
                const Error acknowledged = valid_key_material_response
                    ? setup_.crypto->acknowledge_key_material(
                          decoded.message.key_material.view(), true)
                    : Error::invalid_key_material;
                if (acknowledged != Error::none) {
                    if (setup_.enforced_encryption
                        || !setup_.crypto->allows_plaintext_fallback()) {
                        return fail_connect(*socket_, SRT_ESECFAIL, 0,
                            asynchronous_,
                            crypto_rejection_reason(
                                *setup_.crypto, SRT_REJ_BADSECRET));
                    }
                    if (clear_crypto() == SRT_ERROR) {
                        return SRT_ERROR;
                    }
                }
            } else if (decoded.message.has_key_material_extension
                && setup_.enforced_encryption) {
                return fail_connect(
                    *socket_, SRT_ESECFAIL, 0, asynchronous_, SRT_REJ_UNSECURE);
            }
        }

        const ActionResult result =
            apply_actions(*setup_.channel, peer_, peer_socket_id_,
                machine_.receive(decoded.message), origin_, retry_deadline_);
        remember_replay(result);
        switch (result.outcome) {
        case ActionOutcome::connected:
            return complete_connection(peer_handshake_timestamp_, nullptr);
        case ActionOutcome::rejected:
            return fail_connect(*socket_, SRT_ECONNREJ, 0, asynchronous_,
                result.rejection_reason);
        case ActionOutcome::failed:
            return fail_connect(*socket_, SRT_ECONNSETUP, 0, asynchronous_);
        case ActionOutcome::io_error:
            return fail_connect(
                *socket_, SRT_ESOCKFAIL, result.system_error, asynchronous_);
        case ActionOutcome::running:
            return std::nullopt;
        }
        return fail_connect(*socket_, SRT_ECONNSETUP, 0, asynchronous_);
    }

private:
    [[nodiscard]] int install_crypto(
        std::size_t key_length, bool start_initiator) noexcept
    {
        std::shared_ptr<CryptoSession> replacement;
        try {
            auto configuration = setup_.options.crypto_configuration();
            configuration.key_length = key_length;
            configuration.negotiation_context =
                CryptoNegotiationContext::rendezvous;
            replacement = std::make_shared<CryptoSession>(configuration);
        } catch (const std::bad_alloc&) {
            return fail_connect(*socket_, SRT_ENOBUF, 0, asynchronous_);
        } catch (...) {
            return fail_connect(*socket_, SRT_ESYSOBJ, 0, asynchronous_);
        }
        if (start_initiator && replacement->start_initiator() != Error::none) {
            return fail_connect(*socket_, SRT_ESECFAIL, 0, asynchronous_,
                crypto_rejection_reason(*replacement, SRT_REJ_BADSECRET));
        }
        setup_.crypto = std::move(replacement);
        setup_.crypto_key_length = key_length;
        (void)setup_.options.set(SocketOption::encryption_key_length,
            static_cast<std::int64_t>(key_length));
        {
            std::lock_guard lock(socket_->mutex);
            socket_->crypto = setup_.crypto;
            (void)socket_->native_options.set(
                SocketOption::encryption_key_length,
                static_cast<std::int64_t>(key_length));
        }
        return 0;
    }

    [[nodiscard]] int clear_crypto() noexcept
    {
        const KeyMaterialBuffer empty_request;
        if (machine_.set_key_material_request(empty_request, 0U)
            != Error::none) {
            return fail_connect(
                *socket_, SRT_ESECFAIL, 0, asynchronous_, SRT_REJ_BADSECRET);
        }
        setup_.crypto.reset();
        setup_.crypto_key_length = 0U;
        (void)setup_.options.set(SocketOption::encryption_key_length, 0);
        {
            std::lock_guard lock(socket_->mutex);
            socket_->crypto.reset();
            (void)socket_->native_options.set(
                SocketOption::encryption_key_length, 0);
        }
        return 0;
    }

    [[nodiscard]] int set_key_material_failure(CryptoState state) noexcept
    {
        KeyMaterialBuffer response;
        response.size = 4U;
        const std::uint32_t value = static_cast<std::uint32_t>(state);
        response.bytes[0] = static_cast<std::byte>((value >> 24U) & 0xffU);
        response.bytes[1] = static_cast<std::byte>((value >> 16U) & 0xffU);
        response.bytes[2] = static_cast<std::byte>((value >> 8U) & 0xffU);
        response.bytes[3] = static_cast<std::byte>(value & 0xffU);
        if (machine_.set_key_material_response(response) != Error::none) {
            return fail_connect(
                *socket_, SRT_ESECFAIL, 0, asynchronous_, SRT_REJ_BADSECRET);
        }
        return 0;
    }

    void remember_replay(const ActionResult& result) noexcept
    {
        if (result.has_last_sent
            && ((machine_.role() == RendezvousRole::initiator
                    && result.last_sent.packet.request
                        == HandshakeRequest::agreement)
                || (machine_.role() == RendezvousRole::responder
                    && result.last_sent.has_handshake_extension
                    && result.last_sent.extension_type
                        == HandshakeExtensionType::handshake_response))) {
            replay_response_ = result.last_sent;
        }
    }

    [[nodiscard]] int complete_connection(PacketTimestamp handshake_timestamp,
        const PacketView* first_connected_packet) noexcept
    {
        {
            std::lock_guard lock(socket_->mutex);
            socket_->peer_connection_initial_sequence =
                machine_.peer_initial_sequence().value();
        }
        finish_connect(*socket_, SRTS_CONNECTED, peer_, peer_socket_id_,
            peer_maximum_segment_size_, machine_);
        if (socket_was_closed(*socket_)) {
            return fail(SRT_ESCLOSED);
        }
        const HandshakeAction* replay =
            replay_response_.kind == HandshakeActionKind::send
            ? &replay_response_
            : nullptr;
        if (attach_runtime(*socket_, setup_.channel, origin_,
                microseconds_since(origin_), handshake_timestamp, replay,
                machine_.peer_cookie(), inbox_)
            == SRT_ERROR) {
            return fail_connect(*socket_,
                static_cast<SRT_ERRNO>(last_error().code),
                last_error().system_error, asynchronous_);
        }
        if (first_connected_packet != nullptr) {
            std::shared_ptr<ConnectionRuntime> runtime;
            {
                std::lock_guard lock(socket_->mutex);
                runtime = socket_->runtime;
            }
            if (runtime != nullptr) {
                runtime->process_packet(*first_connected_packet, peer_);
            }
        }
        return 0;
    }

    std::shared_ptr<SocketRecord> socket_;
    IpEndpoint peer_ {};
    RendezvousSetup setup_;
    std::shared_ptr<DatagramInbox> inbox_;
    RendezvousHandshakeMachine machine_;
    bool asynchronous_ = false;
    Clock::time_point origin_ {};
    Clock::time_point overall_deadline_ {};
    Clock::time_point retry_deadline_ {};
    std::uint32_t peer_socket_id_ = 0;
    std::uint32_t peer_maximum_segment_size_ = default_maximum_segment_size;
    bool role_prepared_ = false;
    HandshakeAction replay_response_;
    PacketTimestamp peer_handshake_timestamp_ {};
};

[[nodiscard]] int run_rendezvous_handshake(
    const std::shared_ptr<SocketRecord>& socket, IpEndpoint peer,
    RendezvousSetup setup, bool asynchronous) noexcept
{
    const auto inbox = create_setup_inbox();
    if (inbox == nullptr) {
        return fail_connect(*socket, SRT_ENOBUF, 0, asynchronous);
    }
    if (!setup.channel->register_setup_inbox(
            setup.configuration.local_socket_id, peer, inbox)) {
        return fail_connect(*socket, SRT_ECONNSETUP, 0, asynchronous);
    }
    SetupRouteGuard setup_route {
        setup.channel,
        setup.configuration.local_socket_id,
        inbox,
    };
    if (!setup.channel->start()) {
        return fail_connect(*socket, SRT_ETHREAD, 0, asynchronous);
    }

    RendezvousHandshakeSession session(
        socket, peer, std::move(setup), inbox, asynchronous);
    if (const std::optional<int> result = session.start(); result.has_value()) {
        return *result;
    }
    while (Clock::now() < session.overall_deadline()) {
        if (socket_was_closed(*socket)) {
            return fail(SRT_ESCLOSED);
        }
        if (Clock::now() >= session.retry_deadline()) {
            if (const std::optional<int> result = session.timeout();
                result.has_value()) {
                return *result;
            }
        }

        DatagramEnvelope envelope;
        const InboxPopStatus received = inbox->pop_for(envelope,
            std::chrono::milliseconds {wait_duration(
                session.retry_deadline(), session.overall_deadline())});
        if (received == InboxPopStatus::timeout) {
            continue;
        }
        if (received == InboxPopStatus::closed) {
            return fail_connect(*socket, SRT_ESCLOSED, 0, asynchronous);
        }
        if (const std::optional<int> result = session.receive(envelope);
            result.has_value()) {
            return *result;
        }
    }
    return fail_connect(*socket, SRT_ENOSERVER, 0, asynchronous);
}

// Serial, affinity-bound owner for one asynchronous Rendezvous handshake.
// Inbox readiness, retry and overall deadlines, protocol mutation, crypto
// policy, runtime publication, callback ordering, and teardown all share the
// socket-ID actor. Arbitrary application callbacks leave the scheduler shard
// only after the terminal transition.
class AsyncRendezvousHandshakeActor final
    : public ConnectHandshakeOperation
    , public std::enable_shared_from_this<AsyncRendezvousHandshakeActor> {
public:
    AsyncRendezvousHandshakeActor(std::shared_ptr<RuntimeScheduler> scheduler,
        std::shared_ptr<SocketRecord> socket, IpEndpoint peer,
        IpEndpoint callback_peer, RendezvousSetup setup) noexcept
        : scheduler_(std::move(scheduler))
        , socket_(std::move(socket))
        , peer_(peer)
        , callback_peer_(callback_peer)
        , channel_(setup.channel)
        , local_socket_id_(setup.configuration.local_socket_id)
        , setup_(std::move(setup))
    {
    }

    [[nodiscard]] CallerHandshakeDispatchStatus start() noexcept
    {
        std::shared_ptr<AsyncRendezvousHandshakeActor> owner;
        try {
            owner = shared_from_this();
        } catch (...) {
            return CallerHandshakeDispatchStatus::allocation_failed;
        }
        {
            std::lock_guard lock(state_mutex_);
            if (scheduled_ || completed_ || finishing_) {
                return CallerHandshakeDispatchStatus::invalid;
            }
            pending_ = true;
            scheduled_ = true;
        }
        const RuntimeScheduler::SubmitStatus submitted =
            scheduler_->submit(local_socket_id_,
                {
                    .function = run_task,
                    .context = std::move(owner),
                });
        if (submitted == RuntimeScheduler::SubmitStatus::accepted) {
            return CallerHandshakeDispatchStatus::completed;
        }
        {
            std::lock_guard lock(state_mutex_);
            pending_ = false;
            scheduled_ = false;
            completed_ = true;
        }
        completed_signal_.notify_all();
        return connect_submission_status(submitted);
    }

    void close() noexcept override
    {
        {
            std::lock_guard lock(state_mutex_);
            if (completed_ || finishing_) {
                return;
            }
            close_requested_ = true;
        }
        request_run();
    }

    void wait() noexcept override
    {
        std::unique_lock lock(state_mutex_);
        completed_signal_.wait(lock, [this] {
            return completed_;
        });
    }

    [[nodiscard]] bool running_on_current_thread() const noexcept override
    {
        std::lock_guard lock(state_mutex_);
        const std::thread::id current = std::this_thread::get_id();
        return active_thread_ == current || callback_thread_ == current;
    }

private:
    static void run_task(void* context) noexcept
    {
        static_cast<AsyncRendezvousHandshakeActor*>(context)->run();
    }

    static void source_ready(void* context) noexcept
    {
        static_cast<AsyncRendezvousHandshakeActor*>(context)->request_run();
    }

    void request_run() noexcept
    {
        std::shared_ptr<AsyncRendezvousHandshakeActor> owner;
        {
            std::lock_guard lock(state_mutex_);
            if (completed_ || finishing_) {
                return;
            }
            pending_ = true;
            if (scheduled_) {
                return;
            }
            scheduled_ = true;
        }
        try {
            owner = shared_from_this();
        } catch (...) {
            handle_submission_failure(
                CallerHandshakeDispatchStatus::allocation_failed);
            return;
        }
        const RuntimeScheduler::SubmitStatus submitted =
            scheduler_->submit(local_socket_id_,
                {
                    .function = run_task,
                    .context = std::move(owner),
                });
        if (submitted != RuntimeScheduler::SubmitStatus::accepted) {
            handle_submission_failure(connect_submission_status(submitted));
        }
    }

    void handle_submission_failure(
        CallerHandshakeDispatchStatus status) noexcept
    {
        {
            std::lock_guard lock(state_mutex_);
            if (completed_ || finishing_) {
                return;
            }
            pending_ = false;
            scheduled_ = false;
            active_thread_ = std::this_thread::get_id();
        }
        finish_dispatch_failure(status);
        std::lock_guard lock(state_mutex_);
        active_thread_ = {};
    }

    void run() noexcept
    {
        {
            std::lock_guard lock(state_mutex_);
            active_thread_ = std::this_thread::get_id();
        }
        for (;;) {
            bool close_requested = false;
            {
                std::lock_guard lock(state_mutex_);
                pending_ = false;
                close_requested = close_requested_;
            }
            if (close_requested) {
                finish(fail_connect(*socket_, SRT_ESCLOSED, 0, true));
            } else if (!initialized_) {
                initialize();
            } else {
                drain_events();
            }

            std::lock_guard lock(state_mutex_);
            if (completed_ || finishing_) {
                scheduled_ = false;
                active_thread_ = {};
                return;
            }
            if (!pending_) {
                scheduled_ = false;
                active_thread_ = {};
                return;
            }
        }
    }

    void initialize() noexcept
    {
        if (socket_was_closed(*socket_)) {
            finish(fail(SRT_ESCLOSED));
            return;
        }
        inbox_ = create_setup_inbox();
        if (inbox_ == nullptr) {
            finish(fail_connect(*socket_, SRT_ENOBUF, 0, true));
            return;
        }
        if (!channel_->register_setup_inbox(local_socket_id_, peer_, inbox_)) {
            finish(fail_connect(*socket_, SRT_ECONNSETUP, 0, true));
            return;
        }
        route_registered_ = true;
        if (!channel_->start()) {
            finish(fail_connect(*socket_, SRT_ETHREAD, 0, true));
            return;
        }

        session_.emplace(socket_, peer_, std::move(setup_), inbox_, true);
        try {
            source_ = std::make_shared<CallerHandshakeEventSource>(
                *scheduler_, local_socket_id_, inbox_);
        } catch (...) {
            finish(fail_connect(*socket_, SRT_ENOBUF, 0, true));
            return;
        }
        if (!source_->set_event_ready_handler(
                source_ready, shared_from_this())) {
            finish(fail_connect(*socket_, SRT_ECONNSETUP, 0, true));
            return;
        }
        const CallerHandshakeDispatchStatus source_started =
            source_->start(session_->overall_deadline());
        if (source_started != CallerHandshakeDispatchStatus::completed) {
            finish_dispatch_failure(source_started);
            return;
        }
        initialized_ = true;

        if (const std::optional<int> result = session_->start();
            result.has_value()) {
            finish(*result);
            return;
        }
        if (!arm_retry()) {
            return;
        }
    }

    void drain_events() noexcept
    {
        CallerHandshakeSourceEvent event;
        while (source_ != nullptr && source_->try_pop(event)) {
            if (!process_event(event)) {
                return;
            }
        }
    }

    [[nodiscard]] bool process_event(
        const CallerHandshakeSourceEvent& event) noexcept
    {
        if (socket_was_closed(*socket_)) {
            finish(fail_connect(*socket_, SRT_ESCLOSED, 0, true));
            return false;
        }
        switch (event.kind) {
        case CallerHandshakeSourceEventKind::failure:
            finish_dispatch_failure(event.failure);
            return false;
        case CallerHandshakeSourceEventKind::closed:
            finish(fail_connect(*socket_, SRT_ESCLOSED, 0, true));
            return false;
        case CallerHandshakeSourceEventKind::overall_timeout:
            finish(fail_connect(*socket_, SRT_ENOSERVER, 0, true));
            return false;
        case CallerHandshakeSourceEventKind::retry_timer:
            if (const std::optional<int> result = session_->timeout();
                result.has_value()) {
                finish(*result);
                return false;
            }
            return arm_retry();
        case CallerHandshakeSourceEventKind::inbox_ready:
            return process_inbox();
        }
        finish(fail_connect(*socket_, SRT_ECONNSETUP, 0, true));
        return false;
    }

    [[nodiscard]] bool process_inbox() noexcept
    {
        HandshakeInboxAcknowledgement acknowledgement;
        acknowledgement.arm(source_);
        DatagramEnvelope envelope;
        const InboxPopStatus received =
            inbox_->pop_for(envelope, std::chrono::milliseconds {0});
        if (received == InboxPopStatus::timeout) {
            return true;
        }
        if (received == InboxPopStatus::closed) {
            source_->close();
            finish(fail_connect(*socket_, SRT_ESCLOSED, 0, true));
            return false;
        }
        const Clock::time_point previous_deadline = session_->retry_deadline();
        if (const std::optional<int> result = session_->receive(envelope);
            result.has_value()) {
            finish(*result);
            return false;
        }
        if (session_->retry_deadline() != previous_deadline) {
            return arm_retry();
        }
        return true;
    }

    [[nodiscard]] bool arm_retry() noexcept
    {
        const CallerHandshakeDispatchStatus armed =
            source_->arm_retry_at(session_->retry_deadline());
        if (armed == CallerHandshakeDispatchStatus::completed) {
            return true;
        }
        finish_dispatch_failure(armed);
        return false;
    }

    void finish_dispatch_failure(CallerHandshakeDispatchStatus status) noexcept
    {
        finish(fail_connect(*socket_,
            status == CallerHandshakeDispatchStatus::full
                    || status
                        == CallerHandshakeDispatchStatus::allocation_failed
                ? SRT_ENOBUF
                : SRT_ETHREAD,
            0, true));
    }

    void finish(int result) noexcept
    {
        {
            std::lock_guard lock(state_mutex_);
            if (completed_ || finishing_) {
                return;
            }
            finishing_ = true;
        }
        const int error_code = result == 0 ? SRT_SUCCESS : last_error().code;
        if (source_ != nullptr) {
            source_->clear_event_ready_handler();
            source_->stop();
            source_.reset();
        }
        if (inbox_ != nullptr) {
            inbox_->close();
        }
        if (route_registered_) {
            channel_->unregister_setup_inbox(local_socket_id_, inbox_);
            route_registered_ = false;
        }
        bool callback_registered = false;
        {
            std::lock_guard lock(socket_->mutex);
            callback_registered = socket_->connect_callback != nullptr;
        }
        if (callback_registered && start_callback_worker(error_code)) {
            return;
        }
        run_callback(error_code);
    }

    [[nodiscard]] bool start_callback_worker(int error_code) noexcept
    {
        std::thread worker;
        {
            std::lock_guard lock(socket_->mutex);
            try {
                worker = std::thread([owner = shared_from_this(), error_code] {
                    owner->run_callback(error_code);
                });
            } catch (...) {
                return false;
            }
            socket_->connect_worker = std::move(worker);
        }
        return true;
    }

    void run_callback(int error_code) noexcept
    {
        {
            std::lock_guard lock(state_mutex_);
            callback_thread_ = std::this_thread::get_id();
        }
        invoke_connect_callback(socket_, callback_peer_, error_code);
        {
            std::lock_guard lock(socket_->mutex);
            if (socket_->connect_worker.joinable()
                && socket_->connect_worker.get_id()
                    == std::this_thread::get_id()) {
                socket_->connect_worker.detach();
            }
            if (socket_->connect_handshake_operation.get() == this) {
                socket_->connect_handshake_operation.reset();
            }
        }
        {
            std::lock_guard lock(state_mutex_);
            finishing_ = false;
            completed_ = true;
            callback_thread_ = {};
        }
        completed_signal_.notify_all();
    }

    std::shared_ptr<RuntimeScheduler> scheduler_;
    std::shared_ptr<SocketRecord> socket_;
    IpEndpoint peer_ {};
    IpEndpoint callback_peer_ {};
    std::shared_ptr<DatagramChannel> channel_;
    std::uint32_t local_socket_id_ = 0;
    RendezvousSetup setup_;
    std::shared_ptr<DatagramInbox> inbox_;
    std::shared_ptr<CallerHandshakeEventSource> source_;
    std::optional<RendezvousHandshakeSession> session_;
    mutable std::mutex state_mutex_;
    std::condition_variable completed_signal_;
    std::thread::id active_thread_ {};
    std::thread::id callback_thread_ {};
    bool pending_ = false;
    bool scheduled_ = false;
    bool initialized_ = false;
    bool route_registered_ = false;
    bool close_requested_ = false;
    bool finishing_ = false;
    bool completed_ = false;
};

} // namespace

int connect_socket(
    std::shared_ptr<SocketRecord> socket,
    const sockaddr* name,
    int name_size,
    std::int32_t forced_initial_sequence) noexcept
{
    IpEndpoint peer;
    if (decode_ip_endpoint(
            name, name_size, peer, false) == SRT_ERROR) {
        return SRT_ERROR;
    }
    if (socket == nullptr) {
        return fail(SRT_EINVSOCK);
    }
    const IpEndpoint callback_peer = peer;

    bool asynchronous = false;
    bool rendezvous = false;
    {
        std::lock_guard lock(socket->mutex);
        if (socket->state == SRTS_CLOSED) {
            return fail(SRT_EINVSOCK);
        }
        if (socket->state == SRTS_BROKEN
            || socket->state == SRTS_CLOSING) {
            return fail(SRT_ESCLOSED);
        }
        if (socket->state != SRTS_INIT
            && socket->state != SRTS_OPENED) {
            return fail(socket->state == SRTS_LISTENING
                    || socket->state == SRTS_CONNECTING
                    || socket->state == SRTS_CONNECTED
                ? SRT_ECONNSOCK
                : SRT_EINVOP);
        }
        asynchronous =
            !socket->public_options.receive_synchronous;
        rendezvous =
            socket->public_options.rendezvous;
        if (socket->has_local_endpoint
            && socket->local_endpoint.family != peer.family) {
            const bool can_map_ipv4 =
                !rendezvous
                && socket->local_endpoint.is_ipv6()
                && socket->local_endpoint.is_wildcard()
                && socket->effective_ipv6_only == 0
                && peer.is_ipv4();
            if (!can_map_ipv4) {
                return fail(SRT_EINVPARAM);
            }
            peer = ipv4_mapped_endpoint(peer);
        }
        if (rendezvous
            && socket->state == SRTS_INIT) {
            return fail(SRT_ERDVUNBOUND);
        }
        if (forced_initial_sequence < SRT_SEQNO_NONE) {
            return fail(SRT_EINVPARAM);
        }
    }
    if (!rendezvous
        && bind_any_socket(*socket, peer.family) == SRT_ERROR) {
        return SRT_ERROR;
    }

    if (rendezvous) {
        RendezvousSetup setup;
        {
            std::lock_guard lock(socket->mutex);
            if (socket->state != SRTS_OPENED
                || socket->channel == nullptr
                || !socket->has_local_endpoint) {
                return fail(
                    socket->state == SRTS_CLOSED
                    ? SRT_ESCLOSED
                    : SRT_ERDVUNBOUND);
            }
            if (forced_initial_sequence != SRT_SEQNO_NONE) {
                socket->connection_initial_sequence =
                    static_cast<std::uint32_t>(
                        forced_initial_sequence);
                socket->peer_connection_initial_sequence =
                    socket->connection_initial_sequence;
            }
            setup.channel = socket->channel;
            const std::int64_t expanded_timeout =
                static_cast<std::int64_t>(
                    socket->public_options
                        .connection_timeout_milliseconds)
                * 10;
            setup.timeout_milliseconds =
                static_cast<std::int32_t>(
                    std::min<std::int64_t>(
                        expanded_timeout,
                        std::numeric_limits<
                            std::int32_t>::max()));
            setup.configuration = {
                .local_socket_id =
                    socket->protocol_socket_id,
                .local_cookie = make_rendezvous_cookie(
                    socket->protocol_socket_id,
                    socket->local_endpoint, peer),
                .initial_sequence = SequenceNumber{
                    socket->connection_initial_sequence},
                .maximum_transmission_unit =
                    static_cast<std::uint32_t>(
                        socket->public_options
                            .maximum_segment_size),
                .flow_window =
                    static_cast<std::uint32_t>(
                        socket->public_options
                            .flow_window_packets),
                .timeout_milliseconds =
                    retry_interval_milliseconds,
                .maximum_retries =
                    retry_budget(
                        setup.timeout_milliseconds),
                .extension_parameters =
                    socket->native_options
                        .handshake_parameters(),
                .minimum_peer_srt_version =
                    static_cast<std::uint32_t>(
                        socket->public_options
                            .minimum_peer_srt_version),
                .stream_id =
                    socket->public_options.stream_id,
                .congestion_controller =
                    socket->native_options
                        .congestion_controller(),
                .packet_filter_configuration =
                    socket->native_options
                        .packet_filter_configuration(),
            };
            setup.options = socket->native_options;
            setup.enforced_encryption =
                socket->native_options
                    .enforced_encryption();
            socket->state = SRTS_CONNECTING;
            socket->connect_error = SRT_SUCCESS;
            socket->connect_system_error = 0;
            socket->rejection_reason =
                SRT_REJ_UNKNOWN;
        }
        ReadinessSignal::notify();

        if (setup.options.encryption_enabled()) {
            try {
                auto crypto_configuration =
                    setup.options.crypto_configuration();
                crypto_configuration.negotiation_context =
                    CryptoNegotiationContext::rendezvous;
                setup.crypto =
                    std::make_shared<CryptoSession>(crypto_configuration);
            } catch (const std::bad_alloc&) {
                return fail_connect(
                    *socket, SRT_ENOBUF);
            } catch (...) {
                return fail_connect(
                    *socket, SRT_ESYSOBJ);
            }
            setup.crypto_key_length =
                setup.options.crypto_configuration()
                    .key_length;
            setup.configuration.encryption_field =
                encryption_field_for_key_length(
                    setup.crypto_key_length);
            {
                std::lock_guard lock(socket->mutex);
                socket->crypto = setup.crypto;
            }
        }

        if (!asynchronous) {
            return run_rendezvous_handshake(
                socket, peer, std::move(setup), false);
        }

        std::shared_ptr<RuntimeScheduler> scheduler =
            acquire_runtime_scheduler();
        if (scheduler == nullptr) {
            return fail_connect(*socket, SRT_ETHREAD, 0, true);
        }
        std::shared_ptr<AsyncRendezvousHandshakeActor> actor;
        try {
            actor = std::make_shared<AsyncRendezvousHandshakeActor>(
                scheduler, socket, peer, callback_peer, std::move(setup));
        } catch (...) {
            return fail_connect(*socket, SRT_ENOBUF, 0, true);
        }
        CallerHandshakeDispatchStatus started =
            CallerHandshakeDispatchStatus::invalid;
        {
            std::lock_guard lock(socket->mutex);
            if (socket->state == SRTS_CLOSING || socket->state == SRTS_CLOSED) {
                return fail(SRT_ESCLOSED);
            }
            socket->connect_handshake_operation = actor;
            started = actor->start();
            if (started != CallerHandshakeDispatchStatus::completed) {
                socket->connect_handshake_operation.reset();
            }
        }
        if (started != CallerHandshakeDispatchStatus::completed) {
            return fail_connect(*socket,
                started == CallerHandshakeDispatchStatus::full
                        || started
                            == CallerHandshakeDispatchStatus::allocation_failed
                    ? SRT_ENOBUF
                    : SRT_ETHREAD,
                0, true);
        }
        return 0;
    }

    CallerSetup setup;
    {
        std::lock_guard lock(socket->mutex);
        if (socket->state != SRTS_OPENED
            || socket->channel == nullptr) {
            return fail(socket->state == SRTS_CLOSED
                    ? SRT_ESCLOSED
                    : SRT_EINVOP);
        }
        if (forced_initial_sequence != SRT_SEQNO_NONE) {
            socket->connection_initial_sequence =
                static_cast<std::uint32_t>(
                    forced_initial_sequence);
            socket->peer_connection_initial_sequence =
                socket->connection_initial_sequence;
        }
        setup.channel = socket->channel;
        setup.timeout_milliseconds =
            socket->public_options.connection_timeout_milliseconds;
        setup.configuration = {
            .role = ConnectionRole::caller,
            .local_socket_id = socket->protocol_socket_id,
            .initial_sequence =
                SequenceNumber{socket->connection_initial_sequence},
            .maximum_transmission_unit =
                static_cast<std::uint32_t>(
                    socket->public_options.maximum_segment_size),
            .flow_window = static_cast<std::uint32_t>(
                socket->public_options.flow_window_packets),
            .timeout_milliseconds = retry_interval_milliseconds,
            .maximum_retries =
                retry_budget(setup.timeout_milliseconds),
            .extension_parameters =
                socket->native_options.handshake_parameters(),
            .minimum_peer_srt_version =
                static_cast<std::uint32_t>(
                    socket->public_options
                        .minimum_peer_srt_version),
            .stream_id = socket->public_options.stream_id,
            .congestion_controller =
                socket->native_options
                    .congestion_controller(),
            .packet_filter_configuration =
                socket->native_options
                    .packet_filter_configuration(),
            .has_group_membership =
                socket->group_id != SRT_INVALID_SOCK,
            .group_membership = {
                .group_id = static_cast<std::uint32_t>(
                    socket->group_id),
                .type = static_cast<GroupType>(
                    socket->group_type),
                .flags = 0U,
                .weight = socket->group_weight,
            },
        };
        setup.options = socket->native_options;
        setup.enforced_encryption =
            socket->native_options.enforced_encryption();
        setup.data_sender = socket->public_options.data_sender;
        socket->state = SRTS_CONNECTING;
        socket->connect_error = SRT_SUCCESS;
        socket->connect_system_error = 0;
        socket->rejection_reason = SRT_REJ_UNKNOWN;
    }
    ReadinessSignal::notify();

    if (setup.options.encryption_enabled()) {
        try {
            setup.crypto = std::make_shared<CryptoSession>(
                setup.options.crypto_configuration());
        } catch (const std::bad_alloc&) {
            return fail_connect(*socket, SRT_ENOBUF);
        } catch (...) {
            return fail_connect(*socket, SRT_ESYSOBJ);
        }
        if (setup.crypto->start_initiator() != Error::none) {
            return fail_connect(*socket, SRT_ESECFAIL, 0, asynchronous,
                crypto_rejection_reason(*setup.crypto, SRT_REJ_BADSECRET));
        }
        const auto request =
            setup.crypto->pending_key_material();
        std::copy(request.begin(), request.end(),
            setup.configuration.key_material_request.bytes.begin());
        setup.configuration.key_material_request.size =
            request.size();
        setup.configuration.encryption_field =
            encryption_field_for_key_length(
                setup.options.crypto_configuration().key_length);
        setup.crypto_key_length =
            setup.options.crypto_configuration().key_length;
        {
            std::lock_guard lock(socket->mutex);
            socket->crypto = setup.crypto;
        }
    }

    if (!asynchronous) {
        return run_caller_handshake(socket, peer, std::move(setup));
    }

    std::shared_ptr<RuntimeScheduler> scheduler = acquire_runtime_scheduler();
    if (scheduler == nullptr) {
        return fail_connect(*socket, SRT_ETHREAD, 0, true);
    }
    std::shared_ptr<AsyncCallerHandshakeActor> actor;
    try {
        actor = std::make_shared<AsyncCallerHandshakeActor>(
            scheduler, socket, peer, callback_peer, std::move(setup));
    } catch (...) {
        return fail_connect(*socket, SRT_ENOBUF, 0, true);
    }
    CallerHandshakeDispatchStatus started =
        CallerHandshakeDispatchStatus::invalid;
    {
        std::lock_guard lock(socket->mutex);
        if (socket->state == SRTS_CLOSING || socket->state == SRTS_CLOSED) {
            return fail(SRT_ESCLOSED);
        }
        socket->connect_handshake_operation = actor;
        started = actor->start();
        if (started != CallerHandshakeDispatchStatus::completed) {
            socket->connect_handshake_operation.reset();
        }
    }
    if (started != CallerHandshakeDispatchStatus::completed) {
        return fail_connect(*socket,
            started == CallerHandshakeDispatchStatus::full
                    || started
                        == CallerHandshakeDispatchStatus::allocation_failed
                ? SRT_ENOBUF
                : SRT_ETHREAD,
            0, true);
    }
    return 0;
}

int connect_bound_socket(
    std::shared_ptr<SocketRecord> socket,
    const sockaddr* source,
    const sockaddr* target,
    int name_size) noexcept
{
    IpEndpoint source_endpoint;
    if (decode_ip_endpoint(source, name_size,
            source_endpoint, true) == SRT_ERROR) {
        return SRT_ERROR;
    }
    IpEndpoint target_endpoint;
    if (decode_ip_endpoint(target, name_size,
            target_endpoint, false) == SRT_ERROR) {
        return SRT_ERROR;
    }
    if (source_endpoint.family != target_endpoint.family) {
        return fail(SRT_EINVPARAM);
    }
    if (socket == nullptr) {
        return fail(SRT_EINVSOCK);
    }

    if (bind_socket(socket.get(), source, name_size) == SRT_ERROR) {
        return SRT_ERROR;
    }
    return connect_socket(socket, target, name_size);
}

struct ListenerRuntime::SetupContext {
    std::shared_ptr<SocketRecord> listener;
    std::shared_ptr<SocketRecord> accepted;
    std::shared_ptr<DatagramChannel> channel;
    std::shared_ptr<DatagramInbox> setup_inbox;
    std::shared_ptr<ListenerConnectionSetupActor> actor;
    std::shared_ptr<ListenerSetupOperation> operation;
    std::unique_ptr<SetupRouteGuard> setup_route;
    ListenerGroupAdmission group_admission;
    ApprovedCookieContext cookie_context;
    HandshakeEnvelope initial;
    SocketOptions native_options;
    std::shared_ptr<CryptoSession> crypto;
    Clock::time_point origin {};
    SRTSOCKET accepted_handle = SRT_INVALID_SOCK;
    std::uint32_t caller_socket_id = 0U;
    SRT_ERRNO policy_error = SRT_ECONNREJ;
    ListenerHandshakeProtocol selected_protocol =
        ListenerHandshakeProtocol::invalid;
    bool policy_rejected = false;
};

SRTSOCKET ListenerRuntime::start_admitted_socket(
    std::shared_ptr<SocketRecord> listener,
    const ListenerHandshakeAdmission& admitted) noexcept
{
    if (listener == nullptr) {
        return fail_accept(SRT_EINVSOCK);
    }

    std::shared_ptr<DatagramChannel> channel;
    std::shared_ptr<HandshakeInbox> listener_inbox;
    PublicSocketOptions public_options;
    SocketOptions native_options;
    IpEndpoint local_endpoint;
    std::int32_t effective_ipv6_only = -1;
    std::uint32_t listener_socket_id = 0;
    std::int32_t timeout_milliseconds = 0;
    srt_listen_callback_fn* listen_callback = nullptr;
    void* listen_callback_opaque = nullptr;
    {
        std::lock_guard lock(listener->mutex);
        if (listener->state == SRTS_CLOSED) {
            return fail_accept(SRT_EINVSOCK);
        }
        if (listener->state != SRTS_LISTENING) {
            return fail_accept(SRT_ENOLISTEN);
        }
        channel = listener->channel;
        listener_inbox = listener->listener_inbox;
        public_options = listener->public_options;
        native_options = listener->native_options;
        local_endpoint = listener->local_endpoint;
        effective_ipv6_only = listener->effective_ipv6_only;
        listener_socket_id = listener->protocol_socket_id;
        timeout_milliseconds = public_options.connection_timeout_milliseconds;
        listen_callback = listener->listen_callback;
        listen_callback_opaque = listener->listen_callback_opaque;
    }
    if (channel == nullptr || listener_inbox == nullptr) {
        return fail_accept(SRT_EUNBOUNDSOCK);
    }

    ListenerHandshakeAdmission admission = admitted;
    const HandshakeEnvelope initial = admission.initial;
    const ListenerHandshakeProtocol selected_protocol = admission.protocol;
    if (selected_protocol != ListenerHandshakeProtocol::hsv5) {
        return fail_accept(SRT_ECONNSETUP);
    }
    const std::uint32_t caller_socket_id = initial.message.packet.socket_id;
    const std::uint32_t connection_sequence =
        initial.message.packet.initial_sequence.value();
    const Clock::time_point origin = Clock::now();

    SocketRegistry& registry = SocketRegistry::instance();
    const SRTSOCKET accepted_handle = registry.create();
    if (accepted_handle == SRT_INVALID_SOCK) {
        return fail_accept(SRT_ENOBUF);
    }
    const auto accepted = registry.find(accepted_handle);
    if (accepted == nullptr) {
        registry.close(accepted_handle);
        return fail_accept(SRT_ESYSOBJ);
    }
    const auto close_with_error = [&](SRT_ERRNO error,
                                      int system_error = 0) -> SRTSOCKET {
        registry.close(accepted_handle);
        return fail_accept(error, system_error);
    };

    std::shared_ptr<SetupContext> setup_context;
    try {
        setup_context = std::make_shared<SetupContext>();
    } catch (...) {
        return close_with_error(SRT_ENOBUF);
    }
    setup_context->listener = listener;
    setup_context->accepted = accepted;
    setup_context->channel = channel;
    setup_context->initial = initial;
    setup_context->origin = origin;
    setup_context->accepted_handle = accepted_handle;
    setup_context->caller_socket_id = caller_socket_id;
    setup_context->selected_protocol = selected_protocol;
    ListenerGroupAdmission& group_admission = setup_context->group_admission;
    group_admission.listener = static_cast<SRTSOCKET>(listener_socket_id);
    group_admission.initial_sequence = connection_sequence;
    group_admission.enabled = public_options.group_connect;
    group_admission.member_handle = accepted_handle;
    group_admission.member = accepted;
    group_admission.endpoint = initial.peer;
    const HandshakeMessage& conclusion = admission.conclusion.message;
    const bool has_stream_id =
        selected_protocol == ListenerHandshakeProtocol::hsv5
        && conclusion.has_stream_id_extension
        && conclusion.has_handshake_extension
        && conclusion.extension_type
            == HandshakeExtensionType::handshake_request;
    {
        std::lock_guard lock(accepted->mutex);
        accepted->state = SRTS_CONNECTING;
        accepted->public_options = public_options;
        accepted->public_options.stream_id =
            has_stream_id ? conclusion.stream_id : StreamId {};
        accepted->native_options = native_options;
        accepted->channel = channel;
        accepted->local_endpoint = local_endpoint;
        accepted->effective_ipv6_only = effective_ipv6_only;
        accepted->has_local_endpoint = true;
        accepted->peer_endpoint = initial.peer;
        accepted->has_peer_endpoint = true;
        accepted->peer_protocol_socket_id = caller_socket_id;
        accepted->connection_initial_sequence = connection_sequence;
        accepted->peer_connection_initial_sequence = connection_sequence;
    }

    int policy_rejection = SRT_REJ_UNKNOWN;
    SRT_ERRNO policy_error = SRT_ECONNREJ;
    bool policy_rejected = false;
    if (listen_callback != nullptr) {
        sockaddr_storage peer_address {};
        int peer_address_size = static_cast<int>(sizeof(peer_address));
        if (write_ip_endpoint(initial.peer,
                reinterpret_cast<sockaddr*>(&peer_address), &peer_address_size)
            == SRT_ERROR) {
            return close_with_error(SRT_ECONNSETUP);
        }
        std::array<char, maximum_stream_id_size + 1U> stream_id {};
        if (has_stream_id) {
            std::copy_n(conclusion.stream_id.bytes.begin(),
                conclusion.stream_id.size, stream_id.begin());
        }
        {
            std::lock_guard lock(accepted->mutex);
            accepted->listen_callback_active = true;
            accepted->incoming_group_type =
                selected_protocol == ListenerHandshakeProtocol::hsv5
                    && conclusion.has_group_membership
                ? compatible_group_type(conclusion.group_membership.type)
                : SRT_GTYPE_UNDEFINED;
        }
        int callback_result = SRT_ERROR;
        try {
            callback_result = listen_callback(listen_callback_opaque,
                accepted_handle, static_cast<int>(conclusion.packet.version),
                reinterpret_cast<const sockaddr*>(&peer_address),
                stream_id.data());
        } catch (...) {
            callback_result = SRT_ERROR;
        }
        bool callback_socket_closed = false;
        {
            std::lock_guard lock(accepted->mutex);
            accepted->listen_callback_active = false;
            accepted->incoming_group_type = SRT_GTYPE_UNDEFINED;
            policy_rejection = accepted->rejection_reason;
            callback_socket_closed = accepted->state == SRTS_CLOSING
                || accepted->state == SRTS_CLOSED;
        }
        if (callback_socket_closed) {
            return close_with_error(SRT_ESCLOSED);
        }
        if (callback_result != 0) {
            policy_rejected = true;
            policy_error = SRT_ECONNREJ;
        } else {
            policy_rejection = SRT_REJ_UNKNOWN;
        }
    }

    {
        std::lock_guard lock(accepted->mutex);
        public_options = accepted->public_options;
        native_options = accepted->native_options;
        timeout_milliseconds = public_options.connection_timeout_milliseconds;
    }
    group_admission.enabled = public_options.group_connect;

    std::shared_ptr<CryptoSession> crypto;
    if (!policy_rejected) {
        if (native_options.encryption_enabled()) {
            try {
                crypto = std::make_shared<CryptoSession>(
                    native_options.crypto_configuration());
            } catch (const std::bad_alloc&) {
                return close_with_error(SRT_ENOBUF);
            } catch (...) {
                return close_with_error(SRT_ESYSOBJ);
            }
        }
        const bool has_request = conclusion.has_key_material_extension
            && conclusion.key_material_extension_type
                == HandshakeExtensionType::key_material_request;
        const KeyMaterialDecodeResult decoded_key_material = has_request
            ? decode_key_material(conclusion.key_material.view())
            : KeyMaterialDecodeResult {
                  .error = Error::invalid_key_material,
              };
        const std::size_t handshake_key_length =
            key_length_for_encryption_field(conclusion.packet.encryption_field);
        const bool key_length_matches = has_request
            && static_cast<bool>(decoded_key_material)
            && handshake_key_length
                == decoded_key_material.key_material.key_length;
        const Error accepted_key_material =
            crypto != nullptr && key_length_matches
            ? crypto->accept_key_material(conclusion.key_material.view(), true)
            : Error::invalid_key_material;
        if (crypto != nullptr
            && (!key_length_matches || accepted_key_material != Error::none)) {
            if (native_options.enforced_encryption()
                || !crypto->allows_plaintext_fallback()) {
                policy_rejected = true;
                policy_rejection = crypto_rejection_reason(*crypto,
                    has_request ? SRT_REJ_BADSECRET : SRT_REJ_UNSECURE);
                policy_error = SRT_ESECFAIL;
            } else {
                crypto.reset();
                admission.conclusion.message.has_key_material_extension = false;
            }
        } else if (crypto == nullptr && has_request) {
            if (native_options.enforced_encryption()) {
                policy_rejected = true;
                policy_rejection = SRT_REJ_UNSECURE;
                policy_error = SRT_ESECFAIL;
            } else {
                admission.conclusion.message.has_key_material_extension = false;
            }
        }
        {
            std::lock_guard lock(accepted->mutex);
            accepted->crypto = crypto;
        }
    }

    setup_context->native_options = native_options;
    setup_context->crypto = crypto;
    setup_context->policy_error = policy_error;
    setup_context->policy_rejected = policy_rejected;
    setup_context->cookie_context.cookie = admission.validated_cookie;
    ListenerConnectionSetupSteps::Configuration configuration;
    configuration.hsv5 = {
        .role = ConnectionRole::listener,
        .local_socket_id = accepted->protocol_socket_id,
        .initial_sequence = SequenceNumber {connection_sequence},
        .maximum_transmission_unit =
            static_cast<std::uint32_t>(public_options.maximum_segment_size),
        .flow_window =
            static_cast<std::uint32_t>(public_options.flow_window_packets),
        .timeout_milliseconds = retry_interval_milliseconds,
        .maximum_retries = retry_budget(timeout_milliseconds),
        .encryption_field =
            static_cast<std::uint16_t>(encryption_field_for_key_length(
                native_options.configured_encryption_key_length())),
        .extension_parameters = native_options.handshake_parameters(),
        .minimum_peer_srt_version =
            static_cast<std::uint32_t>(public_options.minimum_peer_srt_version),
        .congestion_controller = native_options.congestion_controller(),
        .packet_filter_configuration =
            native_options.packet_filter_configuration(),
        .group_membership_negotiator =
            public_options.group_connect ? negotiate_listener_group : nullptr,
        .group_membership_context = &group_admission,
        .cookie_generator = approved_connection_cookie,
        .cookie_context = &setup_context->cookie_context,
    };

    const auto setup_inbox = create_setup_inbox();
    if (setup_inbox == nullptr) {
        return close_with_error(SRT_ENOBUF);
    }
    if (!channel->register_setup_inbox(accepted->protocol_socket_id,
            initial.peer, setup_inbox, caller_socket_id)) {
        return close_with_error(SRT_ECONNSETUP);
    }
    setup_context->setup_inbox = setup_inbox;
    try {
        setup_context->setup_route = std::make_unique<SetupRouteGuard>(
            channel, accepted->protocol_socket_id, setup_inbox);
    } catch (...) {
        setup_inbox->close();
        channel->unregister_setup_inbox(
            accepted->protocol_socket_id, setup_inbox);
        return close_with_error(SRT_ENOBUF);
    }

    HandshakeEnvelope queued_handshake;
    for (;;) {
        const InboxPopStatus queued = listener_inbox->pop_matching(
            queued_handshake, initial.peer, caller_socket_id);
        if (queued != InboxPopStatus::received) {
            break;
        }
        if (!enqueue_setup_handshake(*setup_inbox, queued_handshake)) {
            return close_with_error(SRT_ENOBUF);
        }
    }

    const std::shared_ptr<RuntimeScheduler> scheduler =
        acquire_runtime_scheduler();
    if (scheduler == nullptr) {
        return close_with_error(SRT_ETHREAD);
    }
    std::shared_ptr<ListenerConnectionSetupActor> setup_actor;
    std::shared_ptr<ListenerSetupOperation> setup_operation;
    try {
        setup_actor = std::make_shared<ListenerConnectionSetupActor>(scheduler,
            listener_socket_id, setup_inbox, admission, configuration, 1U);
        setup_operation = std::make_shared<ListenerSetupOperation>(setup_actor);
    } catch (const std::bad_alloc&) {
        return close_with_error(SRT_ENOBUF);
    } catch (...) {
        return close_with_error(SRT_ESYSOBJ);
    }
    if (policy_rejected && !setup_actor->reject(policy_rejection)) {
        return close_with_error(SRT_ECONNSETUP);
    }
    setup_context->actor = setup_actor;
    setup_context->operation = setup_operation;

    bool listener_closed = false;
    std::shared_ptr<ResultContext> result_context;
    {
        std::lock_guard lock(mutex_);
        listener_closed = closed_ || setup_context_ != nullptr;
        if (!listener_closed) {
            setup_actor_ = setup_actor;
            setup_context_ = setup_context;
            result_context = result_context_;
        }
    }
    {
        std::lock_guard lock(accepted->mutex);
        if (accepted->state == SRTS_CLOSING || accepted->state == SRTS_CLOSED) {
            setup_actor->close();
        } else {
            accepted->connect_handshake_operation = setup_operation;
        }
    }
    const auto abandon_start = [&](SRT_ERRNO error,
                                   int system_error = 0) -> SRTSOCKET {
        setup_actor->clear_result_ready_handler();
        setup_actor->close();
        setup_actor->stop();
        {
            std::lock_guard lock(accepted->mutex);
            if (accepted->connect_handshake_operation == setup_operation) {
                accepted->connect_handshake_operation.reset();
            }
        }
        {
            std::lock_guard lock(mutex_);
            if (setup_actor_ == setup_actor) {
                setup_actor_.reset();
            }
            if (setup_context_ == setup_context) {
                setup_context_.reset();
            }
        }
        setup_context->setup_route.reset();
        return close_with_error(error, system_error);
    };

    if (listener_closed || result_context == nullptr
        || !setup_actor->set_result_ready_handler(
            result_ready, std::move(result_context))) {
        return abandon_start(listener_closed ? SRT_ESCLOSED : SRT_ETHREAD);
    }

    const ListenerConnectionSetupDispatchStatus started = setup_actor->start(
        origin + std::chrono::milliseconds {timeout_milliseconds});
    if (started != ListenerConnectionSetupDispatchStatus::completed) {
        return abandon_start(
            started == ListenerConnectionSetupDispatchStatus::full
                    || started
                        == ListenerConnectionSetupDispatchStatus::
                            allocation_failed
                ? SRT_ENOBUF
                : SRT_ETHREAD);
    }
    return accepted_handle;
}

struct ListenerRuntime::ResultContext {
    std::weak_ptr<ListenerRuntime> owner;
};

ListenerRuntime::ListenerRuntime(
    std::weak_ptr<SocketRecord> listener, std::size_t capacity)
    : listener_(std::move(listener))
    , accepts_(capacity)
{
}

ListenerRuntime::~ListenerRuntime()
{
    close();
}

bool ListenerRuntime::start() noexcept
{
    std::shared_ptr<SocketRecord> listener;
    std::shared_ptr<HandshakeInbox> inbox;
    std::uint64_t affinity = 0U;
    StatelessListenerHandshakeRouter::Configuration configuration;
    {
        std::lock_guard lock(mutex_);
        if (handshake_actor_ != nullptr) {
            return true;
        }
        if (closed_) {
            return false;
        }
        listener = listener_.lock();
        if (listener == nullptr) {
            closed_ = true;
            return false;
        }
        std::lock_guard listener_lock(listener->mutex);
        if (listener->state != SRTS_LISTENING
            || listener->listener_inbox == nullptr) {
            closed_ = true;
            return false;
        }
        inbox = listener->listener_inbox;
        affinity = listener->protocol_socket_id;
        configuration = {
            .listener_socket_id = listener->protocol_socket_id,
            .maximum_transmission_unit = static_cast<std::uint32_t>(
                listener->public_options.maximum_segment_size),
            .flow_window = static_cast<std::uint32_t>(
                listener->public_options.flow_window_packets),
            .encryption_field = static_cast<std::uint16_t>(
                encryption_field_for_key_length(listener->native_options
                        .configured_encryption_key_length())),
            .cookie_secret = listener->listener_cookie_secret,
        };
    }
    const std::shared_ptr<RuntimeScheduler> scheduler =
        acquire_runtime_scheduler();
    const std::shared_ptr<RuntimeWorkExecutor> work_executor =
        acquire_runtime_work_executor();
    if (scheduler == nullptr || work_executor == nullptr) {
        default_crypto_provider().secure_erase(configuration.cookie_secret);
        std::lock_guard lock(mutex_);
        closed_ = true;
        return false;
    }

    std::shared_ptr<ListenerHandshakeActor> actor;
    std::shared_ptr<ResultContext> result_context;
    try {
        actor = std::make_shared<ListenerHandshakeActor>(scheduler, affinity,
            std::move(inbox), configuration,
            std::clamp<std::size_t>(
                accepts_.snapshot().capacity * 2U, 16U, 4'096U),
            listener_cookie_time_window_for_actor);
        result_context = std::make_shared<ResultContext>();
        result_context->owner = weak_from_this();
        default_crypto_provider().secure_erase(configuration.cookie_secret);
    } catch (...) {
        default_crypto_provider().secure_erase(configuration.cookie_secret);
        std::lock_guard lock(mutex_);
        closed_ = true;
        return false;
    }

    bool listener_closed = false;
    {
        std::lock_guard lock(mutex_);
        listener_closed = closed_;
        if (!listener_closed) {
            handshake_actor_ = actor;
            work_executor_ = work_executor;
            result_context_ = result_context;
        }
    }
    if (listener_closed
        || !actor->set_result_ready_handler(result_ready, result_context)) {
        actor->close();
        actor->stop();
        std::lock_guard lock(mutex_);
        if (handshake_actor_ == actor) {
            handshake_actor_.reset();
            work_executor_.reset();
            result_context_.reset();
        }
        closed_ = true;
        return false;
    }
    const ListenerHandshakeDispatchStatus started = actor->start();
    if (started == ListenerHandshakeDispatchStatus::completed) {
        return true;
    }

    actor->clear_result_ready_handler();
    actor->stop();
    {
        std::lock_guard lock(mutex_);
        if (handshake_actor_ == actor) {
            handshake_actor_.reset();
            work_executor_.reset();
            result_context_.reset();
        }
        closed_ = true;
    }
    return false;
}

ListenerAcceptPublishStatus ListenerRuntime::publish(
    ListenerAcceptedConnection connection) noexcept
{
    const auto listener = listener_.lock();
    const ListenerAcceptPublishStatus status = accepts_.publish(
        connection, listener != nullptr ? &listener->pending_accepts : nullptr);
    if (status == ListenerAcceptPublishStatus::published) {
        ReadinessSignal::notify();
    }
    return status;
}

SRTSOCKET ListenerRuntime::pop(bool blocking, IpEndpoint& peer) noexcept
{
    ListenerAcceptedConnection connection;
    const auto listener = listener_.lock();
    if (!accepts_.pop(blocking, connection,
            listener != nullptr ? &listener->pending_accepts : nullptr)) {
        return SRT_INVALID_SOCK;
    }
    peer = connection.peer;
    ReadinessSignal::notify();
    return connection.handle;
}

void ListenerRuntime::result_ready(void* context) noexcept
{
    auto& result_context = *static_cast<ResultContext*>(context);
    const std::shared_ptr<ListenerRuntime> owner = result_context.owner.lock();
    if (owner != nullptr) {
        owner->request_result_work();
    }
}

void ListenerRuntime::run_result_task(void* context) noexcept
{
    auto& result_context = *static_cast<ResultContext*>(context);
    const std::shared_ptr<ListenerRuntime> owner = result_context.owner.lock();
    if (owner != nullptr) {
        owner->drain_results();
    }
}

void ListenerRuntime::request_result_work() noexcept
{
    std::shared_ptr<RuntimeWorkExecutor> executor;
    std::shared_ptr<ResultContext> context;
    std::shared_ptr<ListenerHandshakeActor> actor;
    std::shared_ptr<SetupContext> setup_context;
    {
        std::lock_guard lock(mutex_);
        if (closed_) {
            return;
        }
        result_work_pending_ = true;
        if (result_work_scheduled_) {
            return;
        }
        result_work_scheduled_ = true;
        executor = work_executor_;
        context = result_context_;
        actor = handshake_actor_;
    }
    const RuntimeWorkExecutor::SubmitStatus submitted =
        executor != nullptr && context != nullptr
        ? executor->submit({
              .function = run_result_task,
              .context = context,
          })
        : RuntimeWorkExecutor::SubmitStatus::invalid;
    if (submitted == RuntimeWorkExecutor::SubmitStatus::accepted) {
        return;
    }

    {
        std::lock_guard lock(mutex_);
        result_work_scheduled_ = false;
        result_work_pending_ = false;
        closed_ = true;
        setup_context = std::move(setup_context_);
        if (setup_context != nullptr && setup_actor_ == setup_context->actor) {
            setup_actor_.reset();
        }
    }
    if (actor != nullptr) {
        actor->clear_result_ready_handler();
        actor->close();
    }
    abandon_setup_context(setup_context);
    const auto listener = listener_.lock();
    accepts_.close(listener != nullptr ? &listener->pending_accepts : nullptr);
    result_idle_.notify_all();
    ReadinessSignal::notify();
}

void ListenerRuntime::drain_results() noexcept
{
    for (;;) {
        std::shared_ptr<ListenerHandshakeActor> actor;
        {
            std::lock_guard lock(mutex_);
            if (closed_) {
                result_work_active_ = false;
                result_work_thread_ = {};
                result_work_scheduled_ = false;
                result_work_pending_ = false;
                result_idle_.notify_all();
                return;
            }
            result_work_active_ = true;
            result_work_thread_ = std::this_thread::get_id();
            result_work_pending_ = false;
            actor = handshake_actor_;
        }

        bool keep_running = actor != nullptr;
        ListenerHandshakeActorResult result;
        while (keep_running && actor->try_pop(result)) {
            keep_running = process_result(result);
        }
        std::shared_ptr<ListenerConnectionSetupActor> setup_actor;
        std::shared_ptr<SetupContext> setup_context;
        {
            std::lock_guard lock(mutex_);
            setup_actor = setup_actor_;
            setup_context = setup_context_;
        }
        ListenerConnectionSetupActorResult setup_result;
        while (keep_running && setup_actor != nullptr
            && setup_context != nullptr && setup_actor->try_pop(setup_result)) {
            keep_running = process_setup_result(setup_context, setup_result);
            std::lock_guard lock(mutex_);
            if (setup_context_ != setup_context) {
                break;
            }
        }
        if (!keep_running) {
            finish_result_consumer();
        }

        std::unique_lock lock(mutex_);
        if (!closed_ && result_work_pending_) {
            result_work_pending_ = false;
            lock.unlock();
            continue;
        }
        result_work_active_ = false;
        result_work_thread_ = {};
        result_work_scheduled_ = false;
        result_work_pending_ = false;
        lock.unlock();
        result_idle_.notify_all();
        return;
    }
}

bool ListenerRuntime::process_result(
    const ListenerHandshakeActorResult& result) noexcept
{
    if (result.kind != ListenerHandshakeActorResultKind::step) {
        return false;
    }
    const auto listener = listener_.lock();
    if (listener == nullptr) {
        return false;
    }
    if (result.step.kind == ListenerHandshakeAdmissionStepKind::send) {
        std::shared_ptr<DatagramChannel> channel;
        {
            std::lock_guard lock(listener->mutex);
            if (listener->state != SRTS_LISTENING) {
                return false;
            }
            channel = listener->channel;
        }
        if (channel == nullptr) {
            return false;
        }
        (void)send_action(*channel, result.step.peer, result.step.response,
            result.step.destination_socket_id, Clock::now());
        return true;
    }
    if (result.step.kind != ListenerHandshakeAdmissionStepKind::admit) {
        return true;
    }

    const SRTSOCKET accepted =
        start_admitted_socket(listener, result.step.admission);
    if (accepted != SRT_INVALID_SOCK) {
        return true;
    }
    std::shared_ptr<ListenerHandshakeActor> actor;
    {
        std::lock_guard lock(mutex_);
        actor = handshake_actor_;
    }
    const bool resumed = actor != nullptr && actor->complete_admission();
    std::lock_guard lock(listener->mutex);
    return resumed && listener->state == SRTS_LISTENING;
}

bool ListenerRuntime::process_setup_result(
    const std::shared_ptr<SetupContext>& context,
    const ListenerConnectionSetupActorResult& result) noexcept
{
    if (context == nullptr || context->actor == nullptr
        || context->listener == nullptr || context->accepted == nullptr
        || context->channel == nullptr || context->setup_inbox == nullptr) {
        return finish_setup_context(context, false, SRT_ECONNSETUP);
    }
    if (result.kind != ListenerConnectionSetupActorResultKind::setup) {
        return finish_setup_context(context, false,
            result.failure == ListenerConnectionSetupDispatchStatus::full
                    || result.failure
                        == ListenerConnectionSetupDispatchStatus::
                            allocation_failed
                ? SRT_ENOBUF
                : SRT_ETHREAD);
    }
    const auto admitted_group =
        GroupRegistry::instance().find(context->group_admission.mirror.group);
    context->origin = group_timestamp_origin(admitted_group, context->origin);
    if (!listener_is_open(*context->listener)
        || socket_was_closed(*context->accepted)) {
        return finish_setup_context(context, false, SRT_ESCLOSED);
    }

    Clock::time_point ignored_retry_deadline = Clock::now();
    if (result.setup.outcome == ListenerConnectionSetupOutcome::connected) {
        if (!result.runtime.has_peer_extension_parameters
            || !result.has_peer_handshake) {
            return finish_setup_context(context, false, SRT_ECONNSETUP);
        }
            const ConnectedNegotiation negotiation {
                .peer_srt_version =
                    result.runtime.peer_extension_parameters.srt_version,
                .live_options = negotiate_live_options(
                    context->native_options.handshake_parameters(),
                    result.runtime.peer_extension_parameters),
                .packet_filter = result.runtime.packet_filter,
                .peer_stream_id = result.runtime.peer_stream_id,
                .has_peer_stream_id = result.runtime.has_peer_stream_id,
                .peer_flow_window = result.runtime.peer_flow_window,
            };
            if (!stage_connected_transport(*context->accepted,
                    context->initial.peer, context->caller_socket_id,
                    result.peer_handshake.packet.maximum_transmission_unit,
                    negotiation)) {
                return finish_setup_context(context, false, SRT_ESCLOSED);
            }
            publish_connected_state(*context->accepted, SRTS_CONNECTED);

            const HandshakeAction* replay_response = nullptr;
            for (std::size_t index = 0; index < result.setup.hsv5_actions.size;
                ++index) {
                if (result.setup.hsv5_actions.values[index].kind
                    == HandshakeActionKind::send) {
                    replay_response = &result.setup.hsv5_actions.values[index];
                }
            }
            if (attach_runtime(*context->accepted, context->channel,
                    context->origin, microseconds_since(context->origin),
                    result.peer_control.timestamp, replay_response, 0U,
                    context->setup_inbox, admitted_group)
                == SRT_ERROR) {
                const ErrorState error = last_error();
                return finish_setup_context(context, false,
                    static_cast<SRT_ERRNO>(error.code), error.system_error);
            }
            if (result.runtime.has_peer_group_membership
                && !context->group_admission.activate()) {
                return finish_setup_context(context, false, SRT_ESCLOSED);
            }
            const ActionResult actions =
                apply_actions(*context->channel, context->initial.peer,
                    context->caller_socket_id, result.setup.hsv5_actions,
                    context->origin, ignored_retry_deadline);
            if (actions.outcome == ActionOutcome::io_error) {
                return finish_setup_context(
                    context, false, SRT_ESOCKFAIL, actions.system_error);
            }
            if (actions.outcome != ActionOutcome::connected) {
                return finish_setup_context(context, false, SRT_ECONNSETUP);
            }
        return finish_setup_context(context, true);
    }

    const ActionResult actions = apply_actions(*context->channel,
        context->initial.peer, context->caller_socket_id,
        result.setup.hsv5_actions, context->origin, ignored_retry_deadline);
    if (actions.outcome == ActionOutcome::io_error) {
        return finish_setup_context(
            context, false, SRT_ESOCKFAIL, actions.system_error);
    }
    if (result.setup.outcome == ListenerConnectionSetupOutcome::running) {
        if (actions.outcome == ActionOutcome::failed
            || actions.outcome == ActionOutcome::rejected) {
            return finish_setup_context(context, false, SRT_ECONNSETUP);
        }
        if (!context->actor->complete_result()) {
            return finish_setup_context(context, false, SRT_ETHREAD);
        }
        return true;
    }
    if (result.setup.outcome == ListenerConnectionSetupOutcome::rejected) {
        return finish_setup_context(context, false,
            context->policy_rejected ? context->policy_error : SRT_ECONNREJ);
    }
    if (result.setup.outcome == ListenerConnectionSetupOutcome::timed_out) {
        return finish_setup_context(context, false, SRT_ENOSERVER);
    }
    if (result.setup.outcome == ListenerConnectionSetupOutcome::closed) {
        return finish_setup_context(context, false, SRT_ESCLOSED);
    }
    return finish_setup_context(context, false,
        result.setup.error == Error::unsupported ? SRT_ECONNREJ
                                                 : SRT_ECONNSETUP);
}

bool ListenerRuntime::finish_setup_context(
    const std::shared_ptr<SetupContext>& context, bool connected,
    SRT_ERRNO error, int system_error) noexcept
{
    if (context == nullptr) {
        return false;
    }
    if (context->actor != nullptr) {
        context->actor->clear_result_ready_handler();
        if (!connected) {
            context->actor->close();
        }
        context->actor->stop();
    }
    if (context->accepted != nullptr) {
        std::lock_guard lock(context->accepted->mutex);
        if (context->accepted->connect_handshake_operation
            == context->operation) {
            context->accepted->connect_handshake_operation.reset();
        }
    }
    std::shared_ptr<ListenerHandshakeActor> handshake_actor;
    bool listener_closed = false;
    {
        std::lock_guard lock(mutex_);
        if (setup_context_ != context) {
            return false;
        }
        setup_context_.reset();
        if (setup_actor_ == context->actor) {
            setup_actor_.reset();
        }
        handshake_actor = handshake_actor_;
        listener_closed = closed_;
    }
    context->setup_route.reset();
    if (connected) {
        context->group_admission.commit();
    } else {
        SocketRegistry::instance().close(context->accepted_handle);
        (void)fail_accept(error, system_error);
    }

    const bool resumed = !listener_closed && handshake_actor != nullptr
        && handshake_actor->complete_admission();
    const bool listener_open =
        context->listener != nullptr && listener_is_open(*context->listener);
    if (!resumed || !listener_open) {
        if (connected) {
            SocketRegistry::instance().close(context->accepted_handle);
        }
        return false;
    }
    if (!connected) {
        return true;
    }

    IpEndpoint peer;
    SRTSOCKET public_handle = context->accepted_handle;
    bool publish_accept = true;
    const auto record =
        SocketRegistry::instance().find(context->accepted_handle);
    if (record == nullptr) {
        return true;
    }
    {
        std::lock_guard lock(record->mutex);
        peer = record->peer_endpoint;
        if (record->group_id != SRT_INVALID_SOCK) {
            public_handle = record->group_id;
            publish_accept = record->group_accept_result;
        }
    }
    if (!publish_accept) {
        return true;
    }
    const ListenerAcceptPublishStatus published = publish({
        .handle = public_handle,
        .peer = peer,
    });
    if (published == ListenerAcceptPublishStatus::published) {
        return true;
    }
    if (is_group_handle(public_handle)) {
        GroupRegistry::instance().close(public_handle);
    } else {
        SocketRegistry::instance().close(public_handle);
    }
    return published != ListenerAcceptPublishStatus::closed;
}

void ListenerRuntime::abandon_setup_context(
    const std::shared_ptr<SetupContext>& context) noexcept
{
    if (context == nullptr) {
        return;
    }
    if (context->actor != nullptr) {
        context->actor->clear_result_ready_handler();
        context->actor->close();
        context->actor->stop();
    }
    if (context->accepted != nullptr) {
        std::lock_guard lock(context->accepted->mutex);
        if (context->accepted->connect_handshake_operation
            == context->operation) {
            context->accepted->connect_handshake_operation.reset();
        }
    }
    context->setup_route.reset();
    SocketRegistry::instance().close(context->accepted_handle);
}

void ListenerRuntime::finish_result_consumer() noexcept
{
    std::shared_ptr<ListenerHandshakeActor> actor;
    std::shared_ptr<SetupContext> setup_context;
    {
        std::lock_guard lock(mutex_);
        closed_ = true;
        actor = handshake_actor_;
        setup_context = std::move(setup_context_);
        if (setup_context != nullptr && setup_actor_ == setup_context->actor) {
            setup_actor_.reset();
        }
    }
    if (actor != nullptr) {
        actor->clear_result_ready_handler();
    }
    abandon_setup_context(setup_context);
    const auto listener = listener_.lock();
    accepts_.close(listener != nullptr ? &listener->pending_accepts : nullptr);
    ReadinessSignal::notify();
}

void ListenerRuntime::close() noexcept
{
    std::shared_ptr<ListenerHandshakeActor> actor;
    std::shared_ptr<ListenerConnectionSetupActor> setup_actor;
    std::shared_ptr<SetupContext> setup_context;
    {
        std::lock_guard lock(mutex_);
        closed_ = true;
        actor = handshake_actor_;
        setup_actor = setup_actor_;
        setup_context = setup_context_;
    }
    if (actor != nullptr) {
        actor->clear_result_ready_handler();
        actor->close();
    }
    if (setup_actor != nullptr) {
        setup_actor->close();
    }
    const auto listener = listener_.lock();
    accepts_.close(listener != nullptr ? &listener->pending_accepts : nullptr);
    {
        std::unique_lock lock(mutex_);
        if (result_work_active_
            && result_work_thread_ != std::this_thread::get_id()) {
            result_idle_.wait(lock, [this] {
                return !result_work_active_;
            });
        }
    }
    if (actor != nullptr) {
        actor->stop();
    }
    if (setup_actor != nullptr) {
        setup_actor->stop();
    }
    {
        std::lock_guard lock(mutex_);
        if (handshake_actor_ == actor) {
            handshake_actor_.reset();
        }
        if (setup_actor_ == setup_actor) {
            setup_actor_.reset();
        }
        if (setup_context_ == setup_context) {
            setup_context_.reset();
        }
        work_executor_.reset();
        result_context_.reset();
    }
    abandon_setup_context(setup_context);

    ListenerAcceptedConnection abandoned;
    while (accepts_.pop(false, abandoned,
        listener != nullptr ? &listener->pending_accepts : nullptr)) {
        if (is_group_handle(abandoned.handle)) {
            GroupRegistry::instance().close(abandoned.handle);
        } else {
            SocketRegistry::instance().close(abandoned.handle);
        }
    }
    ReadinessSignal::notify();
}

SRTSOCKET accept_socket(
    std::shared_ptr<SocketRecord> listener,
    sockaddr* peer_name,
    int* peer_name_size) noexcept
{
    if (peer_name != nullptr && peer_name_size == nullptr) {
        return fail_accept(SRT_EINVPARAM);
    }
    if (listener == nullptr) {
        return fail_accept(SRT_EINVSOCK);
    }

    std::shared_ptr<ListenerRuntime> runtime;
    bool blocking = true;
    IpAddressFamily family = IpAddressFamily::ipv4;
    {
        std::lock_guard lock(listener->mutex);
        if (listener->state == SRTS_CLOSED) {
            return fail_accept(SRT_EINVSOCK);
        }
        if (listener->state != SRTS_LISTENING) {
            return fail_accept(SRT_ENOLISTEN);
        }
        runtime = listener->listener_runtime;
        blocking = listener->public_options.receive_synchronous;
        family = listener->local_endpoint.family;
    }
    if (peer_name != nullptr
        && *peer_name_size < socket_address_size(family)) {
        return fail_accept(SRT_EINVPARAM);
    }
    if (runtime == nullptr) {
        return fail_accept(SRT_EUNBOUNDSOCK);
    }

    IpEndpoint peer;
    const SRTSOCKET accepted = runtime->pop(blocking, peer);
    if (accepted == SRT_INVALID_SOCK) {
        std::lock_guard lock(listener->mutex);
        return fail_accept(listener->state == SRTS_LISTENING
                ? SRT_EASYNCRCV
                : SRT_ESCLOSED);
    }
    if (peer_name != nullptr
        && write_ip_endpoint(
            peer, peer_name, peer_name_size) == SRT_ERROR) {
        SocketRegistry::instance().close(accepted);
        return SRT_INVALID_SOCK;
    }
    return accepted;
}

SRTSOCKET accept_bond_sockets(
    const SRTSOCKET* listeners, int listener_count,
    std::int64_t timeout_milliseconds) noexcept
{
    if (listeners == nullptr || listener_count < 1
        || timeout_milliseconds < -1) {
        return fail_accept(SRT_EINVPARAM);
    }

    struct BondListener {
        SRTSOCKET handle = SRT_INVALID_SOCK;
        std::shared_ptr<SocketRecord> socket;
        std::shared_ptr<ListenerRuntime> runtime;
    };

    std::vector<BondListener> bonded;
    try {
        bonded.reserve(static_cast<std::size_t>(listener_count));
        for (int index = 0; index < listener_count; ++index) {
            const SRTSOCKET handle = listeners[index];
            const auto socket =
                SocketRegistry::instance().find(handle);
            if (socket == nullptr || is_group_handle(handle)) {
                return fail_accept(SRT_EINVSOCK);
            }
            std::shared_ptr<ListenerRuntime> runtime;
            {
                std::lock_guard lock(socket->mutex);
                if (socket->state == SRTS_CLOSING
                    || socket->state == SRTS_CLOSED) {
                    return fail_accept(SRT_EINVSOCK);
                }
                if (socket->state != SRTS_LISTENING) {
                    return fail_accept(SRT_ENOLISTEN);
                }
                runtime = socket->listener_runtime;
            }
            if (runtime == nullptr) {
                return fail_accept(SRT_ENOLISTEN);
            }
            bonded.push_back({handle, socket, std::move(runtime)});
        }
        std::sort(bonded.begin(), bonded.end(),
            [](const BondListener& left, const BondListener& right) {
                return left.handle < right.handle;
            });
        bonded.erase(std::unique(
            bonded.begin(), bonded.end(),
            [](const BondListener& left, const BondListener& right) {
                return left.handle == right.handle;
            }), bonded.end());
    } catch (const std::bad_alloc&) {
        return fail_accept(SRT_ENOBUF);
    } catch (...) {
        return fail_accept(SRT_ESYSOBJ);
    }

    {
        std::lock_guard coordinator_lock(accept_bond_scope_mutex);
        std::uint64_t scope = 0U;
        for (const auto& listener : bonded) {
            std::lock_guard lock(listener.socket->mutex);
            const std::uint64_t current =
                listener.socket->accept_bond_scope;
            if (current == 0U) {
                continue;
            }
            if (scope != 0U && scope != current) {
                // Joining two active domains would make already admitted
                // mirror groups ambiguous. Require an explicit clean
                // listener set instead of silently merging ownership.
                return fail_accept(SRT_EINVOP);
            }
            scope = current;
        }
        if (scope == 0U) {
            scope = allocate_accept_bond_scope();
        }
        for (const auto& listener : bonded) {
            std::lock_guard lock(listener.socket->mutex);
            listener.socket->accept_bond_scope = scope;
        }
    }

    const Clock::time_point deadline = [&] {
        if (timeout_milliseconds < 0) {
            return Clock::time_point::max();
        }
        const Clock::time_point now = Clock::now();
        const auto maximum = std::chrono::duration_cast<
            std::chrono::milliseconds>(
            Clock::time_point::max() - now).count();
        return now
            + std::chrono::milliseconds {
                std::min<std::int64_t>(timeout_milliseconds, maximum)};
    }();

    for (;;) {
        const std::uint64_t generation =
            ReadinessSignal::generation();
        for (const auto& listener : bonded) {
            {
                std::lock_guard lock(listener.socket->mutex);
                if (listener.socket->state == SRTS_CLOSING
                    || listener.socket->state == SRTS_CLOSED) {
                    return fail_accept(SRT_EINVSOCK);
                }
                if (listener.socket->state != SRTS_LISTENING
                    || listener.socket->listener_runtime
                        != listener.runtime) {
                    return fail_accept(SRT_ENOLISTEN);
                }
            }
            IpEndpoint peer;
            const SRTSOCKET accepted =
                listener.runtime->pop(false, peer);
            if (accepted != SRT_INVALID_SOCK) {
                return accepted;
            }
        }

        if (timeout_milliseconds >= 0
            && Clock::now() >= deadline) {
            return fail_accept(SRT_ETIMEOUT);
        }
        ReadinessSignal::wait_until(generation, deadline);
    }
}

int get_peer_name(
    SocketRecord& socket, sockaddr* name, int* name_size) noexcept
{
    if (name == nullptr || name_size == nullptr) {
        return fail(SRT_EINVPARAM);
    }
    std::lock_guard lock(socket.mutex);
    if (socket.state == SRTS_CLOSED) {
        return fail(SRT_EINVSOCK);
    }
    if (socket.state != SRTS_CONNECTED || !socket.has_peer_endpoint) {
        return fail(SRT_ENOCONN);
    }
    return write_ip_endpoint(socket.peer_endpoint, name, name_size);
}

} // namespace robotweax::srt::compat
