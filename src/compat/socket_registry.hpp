#pragma once

#include "compat/closed_handle_history.hpp"

#include "robotweax/srt/socket_options.hpp"
#include "robotweax/srt/handshake_extensions.hpp"
#include "compat/transport_runtime.hpp"
#include "srt/srt.h"

#include <atomic>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace robotweax::srt::compat {

class ListenerRuntime;
class ConnectHandshakeOperation;

struct PublicSocketOptions {
    bool send_synchronous = true;
    bool receive_synchronous = true;
    std::int32_t send_timeout_milliseconds = -1;
    std::int32_t receive_timeout_milliseconds = -1;
    std::int32_t flow_window_packets = 25'600;
    std::int32_t send_buffer_bytes = 12'058'624;
    std::int32_t receive_buffer_bytes = 12'058'624;
    std::int32_t udp_send_buffer_bytes = 65'536;
    std::int32_t udp_receive_buffer_bytes = 12'288'000;
    std::int64_t maximum_bandwidth_bytes_per_second = -1;
    std::int64_t input_bandwidth_bytes_per_second = 0;
    std::int64_t minimum_input_bandwidth_bytes_per_second = 0;
    std::int32_t overhead_bandwidth_percent = 25;
    bool tsbpd_mode = true;
    bool drift_tracer = true;
    bool too_late_packet_drop = true;
    std::int32_t sender_drop_delay_milliseconds = 0;
    bool periodic_nak = true;
    std::int32_t maximum_reorder_tolerance_packets = 0;
    std::int32_t receiver_latency_milliseconds = 120;
    std::int32_t peer_latency_milliseconds = 0;
    std::int32_t maximum_segment_size = 1'500;
    std::int32_t maximum_payload_size = SRT_LIVE_DEF_PLSIZE;
    std::int32_t retransmission_algorithm = 1;
    bool message_api = true;
    bool data_sender = false;
    SRT_TRANSTYPE transmission_type = SRTT_LIVE;
    std::int32_t connection_timeout_milliseconds = 3'000;
    std::int32_t minimum_peer_srt_version = 0x0001'0000;
    std::int32_t peer_idle_timeout_milliseconds = 5'000;
    bool linger_enabled = false;
    std::int32_t linger_seconds = 0;
    bool rendezvous = false;
    bool reuse_address = true;
    bool group_connect = false;
    std::int32_t ipv6_only = -1;
    std::int32_t ip_time_to_live = 64;
    std::int32_t ip_type_of_service = 0xB8;
    bool ip_type_of_service_explicit = false;
    std::array<char, maximum_network_device_name_size> bound_device{};
    std::uint8_t bound_device_size = 0;
    StreamId stream_id{};
};

struct SocketRecord {
    SocketRecord();

    mutable std::mutex mutex;
    SRT_SOCKSTATUS state = SRTS_INIT;
    PublicSocketOptions public_options;
    SocketOptions native_options;
    std::shared_ptr<DatagramChannel> channel;
    std::shared_ptr<ConnectionRuntime> runtime;
    std::shared_ptr<CryptoSession> crypto;
    std::shared_ptr<HandshakeInbox> listener_inbox;
    std::shared_ptr<ListenerRuntime> listener_runtime;
    std::shared_ptr<ConnectHandshakeOperation> connect_handshake_operation;
    std::thread connect_worker;
    IpEndpoint local_endpoint{};
    IpEndpoint peer_endpoint{};
    NegotiatedLiveOptions negotiated_live_options{};
    std::uint32_t protocol_socket_id = 0;
    std::uint32_t peer_protocol_socket_id = 0;
    std::uint32_t peer_flow_window_packets = 0;
    std::uint32_t connection_initial_sequence = 0;
    std::uint32_t peer_connection_initial_sequence = 0;
    std::uint32_t peer_srt_version = 0;
    // Generated from the cryptographic provider when this socket becomes a
    // listener. An all-zero value means no stateless cookie service is active.
    std::array<std::byte, 16> listener_cookie_secret{};
    std::int64_t open_time_microseconds = 0;
    std::int32_t effective_ipv6_only = -1;
    int listen_backlog = 0;
    int connect_error = SRT_SUCCESS;
    int connect_system_error = 0;
    int rejection_reason = SRT_REJ_UNKNOWN;
    srt_listen_callback_fn* listen_callback = nullptr;
    void* listen_callback_opaque = nullptr;
    srt_connect_callback_fn* connect_callback = nullptr;
    void* connect_callback_opaque = nullptr;
    int connect_callback_token = -1;
    std::atomic_size_t pending_accepts = 0;
    std::uint64_t group_update_version = 0;
    // Zero keeps mirror identity local to this listener. A nonzero value is
    // assigned by srt_accept_bond and shared by its explicit listener set.
    std::uint64_t accept_bond_scope = 0;
    bool has_local_endpoint = false;
    bool has_peer_endpoint = false;
    bool listen_callback_active = false;
    // Valid only while the listener callback is running. This is deliberately
    // separate from group_type, which describes committed membership.
    SRT_GROUP_TYPE incoming_group_type = SRT_GTYPE_UNDEFINED;
    // Non-owning, generation-checked membership identity.  Group records do
    // not own SocketRecord pointers, and sockets do not own group records.
    SRTSOCKET group_id = SRT_INVALID_SOCK;
    std::uint64_t group_generation = 0;
    std::uint64_t member_generation = 0;
    SRT_GROUP_TYPE group_type = SRT_GTYPE_UNDEFINED;
    std::uint16_t group_weight = 0;
    bool group_accept_result = false;
};

class SocketRegistry {
public:
    [[nodiscard]] static SocketRegistry& instance() noexcept;

    [[nodiscard]] SRTSOCKET create() noexcept;
    [[nodiscard]] std::shared_ptr<SocketRecord> find(SRTSOCKET socket) noexcept;
    [[nodiscard]] SRT_SOCKSTATUS state(SRTSOCKET socket) noexcept;
    void close(SRTSOCKET socket) noexcept;
    // Preserve the unexpected-failure cause while tearing down a member so
    // its group can publish a partial-path UPDATE. Public/application close
    // continues through close() and must not create that edge.
    void close_failed(SRTSOCKET socket) noexcept;
    // Called only after terminal state publication. In-flight shared owners
    // keep the object alive without retaining it in the public registry.
    void retire_closed(const std::shared_ptr<SocketRecord>& record) noexcept;
    void clear() noexcept;

private:
    SocketRegistry() = default;
    ~SocketRegistry();

    std::mutex mutex_;
    std::unordered_map<SRTSOCKET, std::shared_ptr<SocketRecord>> sockets_;
    ClosedHandleHistory closed_handles_;
    SRTSOCKET next_socket_ = 1;
    bool clearing_ = false;
};

void runtime_start() noexcept;
[[nodiscard]] SRTSOCKET runtime_create_socket() noexcept;
[[nodiscard]] SRTSOCKET runtime_create_group(
    SRT_GROUP_TYPE type) noexcept;
[[nodiscard]] int runtime_create_epoll() noexcept;
void runtime_cleanup() noexcept;

[[nodiscard]] std::size_t service_deferred_closes(
    std::chrono::steady_clock::time_point now) noexcept;

[[nodiscard]] int get_socket_option(
    SRTSOCKET handle, SocketRecord& socket, SRT_SOCKOPT option,
    void* value, int* value_size) noexcept;
[[nodiscard]] int set_socket_option(
    SocketRecord& socket, SRT_SOCKOPT option,
    const void* value, int value_size) noexcept;

} // namespace robotweax::srt::compat
