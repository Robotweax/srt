#pragma once

#include "compat/closed_handle_history.hpp"
#include "compat/public_socket_options.hpp"

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

template <typename T> class ProcessOwned;

class ListenerRuntime;
class ConnectHandshakeOperation;


struct SocketRecord {
    enum class Purpose { transport, option_template };
    explicit SocketRecord(Purpose purpose = Purpose::transport);

    mutable std::mutex mutex;
    SRT_SOCKSTATUS state = SRTS_INIT;
    PublicSocketOptions public_options;
    SocketOptions native_options;
    std::shared_ptr<DatagramChannel> channel;
    std::shared_ptr<ConnectionRuntime> runtime;
    std::shared_ptr<CryptoSession> crypto;
    // Receive-direction key-material state from connection setup when no
    // local session exists (optional encryption without a usable secret).
    CryptoState receiver_key_state = CryptoState::unsecured;
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
    // Bound by epoll watches on a listening socket: accept-queue changes
    // notify it instead of every observer in the process.
    std::shared_ptr<ReadinessSource> readiness_source;
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
    // Refresh terminal state and publish group changes for an already held
    // record. The caller retains responsibility for membership validation.
    [[nodiscard]] SRT_SOCKSTATUS refresh_state(
        const std::shared_ptr<SocketRecord>& record) noexcept;
    // Number of registered sockets, for leak and allocation checks.
    [[nodiscard]] std::size_t size() noexcept;
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
    friend class ProcessOwned<SocketRegistry>;

    SocketRegistry() = default;
    ~SocketRegistry();

    std::mutex mutex_;
    std::unordered_map<SRTSOCKET, std::shared_ptr<SocketRecord>> sockets_;
    ClosedHandleHistory closed_handles_;
    // Allocation position in the handle permutation; never reset, so no
    // handle is issued twice during the loaded runtime's lifetime.
    std::uint32_t next_socket_index_ = 0;
    bool clearing_ = false;
};

// Marks application callbacks that may be awaited by final cleanup. If they
// reenter startup/creation during that cleanup, fail promptly instead of
// waiting on themselves through the cleanup barrier.
class RuntimeCallbackScope {
public:
    RuntimeCallbackScope() noexcept;
    ~RuntimeCallbackScope();
    RuntimeCallbackScope(const RuntimeCallbackScope&) = delete;
    RuntimeCallbackScope& operator=(const RuntimeCallbackScope&) = delete;
};

// Mark a worker that final cleanup can join. The marker lasts through thread
// local destruction after its run function returns.
void mark_runtime_cleanup_worker_thread() noexcept;

[[nodiscard]] bool runtime_start() noexcept;
// Consumes the calling thread's last creation rejection, so the public API
// can report EINVOP even if cleanup finishes before it maps the error.
[[nodiscard]] bool runtime_creation_blocked_by_cleanup() noexcept;
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
