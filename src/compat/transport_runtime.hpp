#pragma once

#include "robotweax/srt/codec.hpp"
#include "robotweax/srt/fec.hpp"
#include "robotweax/srt/handshake_datagram.hpp"
#include "robotweax/srt/session.hpp"
#include "robotweax/srt/socket_options.hpp"
#include "robotweax/srt/udp.hpp"
#include "compat/runtime_scheduler.hpp"
#include "compat/connection_work_binding.hpp"
#include "compat/connection_datagram_inbox.hpp"
#include "compat/readiness.hpp"
#include "compat/socket_readiness.hpp"
#include "compat/statistics.hpp"
#ifdef ENABLE_MAXREXMITBW
#include "compat/retransmission_budget.hpp"
#endif

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <unordered_map>
#include <vector>

namespace robotweax::srt::compat {

class ConnectionRuntime;
class ConnectionDatagramDispatcher;
class RuntimeWorkExecutor;
struct GroupRecord;
struct RetainedGroupReceiveBatch;

struct HandshakeRouteKey {
    IpEndpoint peer{};
    std::uint32_t peer_socket_id = 0;

    [[nodiscard]] friend constexpr bool operator==(
        const HandshakeRouteKey&,
        const HandshakeRouteKey&) noexcept = default;
};

struct HandshakeRouteKeyHash {
    [[nodiscard]] std::size_t operator()(
        const HandshakeRouteKey& key) const noexcept;
};

struct HandshakeEnvelope {
    HandshakeMessage message{};
    ControlHeader control{};
    IpEndpoint peer{};
};

struct DatagramEnvelope {
    static constexpr std::size_t maximum_size = 1500U;

    std::array<std::byte, maximum_size> bytes{};
    std::size_t size = 0;
    IpEndpoint peer{};
};

enum class InboxPopStatus : std::uint8_t {
    received,
    timeout,
    closed,
};

class HandshakeInbox {
public:
    using ReadyFunction = void (*)(void*) noexcept;

    explicit HandshakeInbox(std::size_t capacity);

    [[nodiscard]] bool push(const HandshakeEnvelope& envelope) noexcept;
    [[nodiscard]] InboxPopStatus pop_for(
        HandshakeEnvelope& envelope,
        std::chrono::milliseconds timeout) noexcept;
    [[nodiscard]] InboxPopStatus pop_matching(HandshakeEnvelope& envelope,
        IpEndpoint peer, std::uint32_t peer_socket_id) noexcept;
    [[nodiscard]] bool set_ready_handler(
        ReadyFunction function, std::weak_ptr<void> context) noexcept;
    void clear_ready_handler() noexcept;
    [[nodiscard]] bool ready() noexcept;
    void close() noexcept;

private:
    std::mutex mutex_;
    std::condition_variable ready_;
    std::vector<HandshakeEnvelope> entries_;
    std::size_t head_ = 0;
    std::size_t size_ = 0;
    ReadyFunction ready_handler_ = nullptr;
    std::weak_ptr<void> ready_context_;
    bool closed_ = false;
};

class DatagramInbox {
public:
    using ReadyFunction = void (*)(void*) noexcept;

    explicit DatagramInbox(std::size_t capacity);

    [[nodiscard]] bool push(
        std::span<const std::byte> datagram,
        IpEndpoint peer) noexcept;
    [[nodiscard]] InboxPopStatus pop_for(
        DatagramEnvelope& envelope,
        std::chrono::milliseconds timeout) noexcept;
    [[nodiscard]] bool set_ready_handler(
        ReadyFunction function, std::weak_ptr<void> context) noexcept;
    void clear_ready_handler() noexcept;
    [[nodiscard]] bool ready() noexcept;
    void close() noexcept;
    // Terminal publication fence, including a captured promoted handle.
    // Ordinary setup close preserves its established forwarding contract.
    void retire() noexcept;

private:
    void close_publication(bool terminal) noexcept;
    friend class DatagramChannel;
    friend class ConnectionDatagramDispatcher;
    // Only the dispatcher consumes a queued, sealed setup prefix.
    [[nodiscard]] bool pop_dispatch_prefix(DatagramEnvelope& envelope) noexcept;
    [[nodiscard]] bool dispatch_prefix_empty() noexcept;
    // Complete the sealed setup prefix before established protocol work.
    void finish_promotion() noexcept;
    void drain_promotion_locked() noexcept;
    void deliver_promoted(
        std::span<const std::byte> datagram, IpEndpoint peer) noexcept;
    std::mutex promotion_mutex_;
    std::shared_ptr<ConnectionRuntime> promoted_runtime_;
    IpEndpoint promoted_peer_ {};
    std::weak_ptr<ConnectionDatagramDispatcher> promoted_dispatcher_;
    bool queued_promotion_ = false;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::vector<DatagramEnvelope> entries_;
    std::size_t head_ = 0;
    std::size_t size_ = 0;
    ReadyFunction ready_handler_ = nullptr;
    std::weak_ptr<void> ready_context_;
    bool closed_ = false;
    bool admission_retired_ = false;
};

enum class MessageIoStatus : std::uint8_t {
    success,
    would_block,
    timeout,
    local_closed,
    peer_closed,
    peer_error,
    broken,
    buffer_too_small,
    invalid_state,
};

struct RuntimeResponseHealth {
    std::uint64_t now_microseconds = 0;
    std::uint64_t last_response_microseconds = 0;
    std::uint64_t peer_idle_timeout_microseconds = 0;
    std::uint32_t smoothed_rtt_microseconds = 0;
    std::uint32_t rtt_variation_microseconds = 0;
    std::uint32_t peer_latency_microseconds = 0;
    SequenceNumber acknowledged_sequence{};
    SequenceNumber next_send_sequence{};
    bool send_buffer_empty = true;
};

struct RuntimeBufferPacketCounts {
    std::size_t unacknowledged_send = 0;
    std::size_t available_receive = 0;
};

struct RuntimeReceiveSnapshot {
    std::optional<SequenceNumber> readable_sequence = std::nullopt;
    std::optional<std::chrono::steady_clock::time_point> next_delivery =
        std::nullopt;
    SequenceNumber floor_sequence {};
    bool complete_expected = false;
    bool buffered = false;
    bool terminal = false;
};

struct RuntimePollResult {
    bool immediate_work = false;
    std::optional<std::chrono::microseconds> next_work_delay = std::nullopt;
    bool receive_wait_safe = false;
    bool coarse_timer_probe = false;
    std::optional<std::chrono::steady_clock::time_point> next_work_deadline =
        std::nullopt;
};

struct MessageIoResult {
    MessageIoStatus status = MessageIoStatus::success;
    std::size_t bytes = 0;
    std::uint32_t message_number = 0;
    SequenceNumber first_sequence{};
    SequenceNumber next_sequence{};
    std::int64_t source_time_microseconds = 0;
    int system_error = 0;
};

// Counts budgeted native channel ownership, including shutdown timeouts.
// Original constructors and caller-owned descriptors are outside this scope.
class NativeChannelBudget {
public:
    explicit NativeChannelBudget(std::size_t maximum_channels) noexcept
        : maximum_channels_(maximum_channels)
    {
    }
    [[nodiscard]] std::size_t maximum_channels() const noexcept
    {
        return maximum_channels_;
    }
    [[nodiscard]] std::size_t reserved_channels() const noexcept
    {
        return reserved_.load(std::memory_order_relaxed);
    }

private:
    friend class DatagramChannel;
    [[nodiscard]] bool reserve() noexcept
    {
        auto previous = reserved_.load(std::memory_order_relaxed);
        while (previous < maximum_channels_) {
            if (reserved_.compare_exchange_weak(
                    previous, previous + 1U, std::memory_order_relaxed))
                return true;
        }
        return false;
    }
    void release() noexcept
    {
        reserved_.fetch_sub(1U, std::memory_order_relaxed);
    }
    const std::size_t maximum_channels_;
    std::atomic<std::size_t> reserved_ {0};
};

class DatagramChannel : public std::enable_shared_from_this<DatagramChannel> {
    struct NativeCredit {
        std::shared_ptr<NativeChannelBudget> budget;
        ~NativeCredit()
        {
            reset();
        }
        void reset() noexcept
        {
            if (budget != nullptr) {
                budget->release();
                budget.reset();
            }
        }
    };
    // Declared before socket: implicit destruction closes the descriptor before
    // returning the credit, preserving the existing member teardown order.
    NativeCredit native_credit_;

public:
    using SendHook = UdpIoResult (*)(
        std::span<const std::byte>,
        IpEndpoint,
        void*) noexcept;

    DatagramChannel() = default;
    explicit DatagramChannel(IpAddressFamily family) noexcept;
    explicit DatagramChannel(UdpSocket acquired_socket) noexcept;
    ~DatagramChannel();
    // Internal prototype admission before native open/ownership transfer.
    // Adoption leaves an acquired descriptor with its caller on any failure.
    // Ownership moves only after all factory allocations have succeeded.
    [[nodiscard]] static std::shared_ptr<DatagramChannel> create_budgeted(
        IpAddressFamily family,
        const std::shared_ptr<NativeChannelBudget>& budget) noexcept;
    [[nodiscard]] static std::shared_ptr<DatagramChannel> adopt_budgeted(
        UdpSocket& acquired_socket,
        const std::shared_ptr<NativeChannelBudget>& budget) noexcept;

    DatagramChannel(const DatagramChannel&) = delete;
    DatagramChannel& operator=(const DatagramChannel&) = delete;

    UdpSocket socket;
    std::mutex receive_mutex;

    [[nodiscard]] UdpIoResult send_datagram(
        std::span<const std::byte> bytes,
        IpEndpoint peer) noexcept;
    void set_send_hook_for_testing(
        SendHook hook, void* context) noexcept;
    [[nodiscard]] bool register_connection(std::uint32_t protocol_socket_id,
        std::shared_ptr<ConnectionRuntime> runtime,
        std::shared_ptr<ConnectionDatagramDispatcher> dispatcher =
            nullptr) noexcept;
    [[nodiscard]] bool promote_setup_connection(
        std::uint32_t protocol_socket_id,
        const std::shared_ptr<DatagramInbox>& inbox,
        std::shared_ptr<ConnectionRuntime> runtime,
        std::shared_ptr<ConnectionDatagramDispatcher> dispatcher =
            nullptr) noexcept;
    // Detach this incarnation and retire admission outside the route lock.
    // The returned dispatcher is an optional drain receipt: retain it to retry
    // finish_retirement after a timeout. No runtime close is implied.
    [[nodiscard]] std::shared_ptr<ConnectionDatagramDispatcher>
    retire_connection(std::uint32_t protocol_socket_id) noexcept;
    // Legacy detach with an off-worker, non-waiting reclamation attempt.
    void unregister_connection(std::uint32_t protocol_socket_id) noexcept;
    [[nodiscard]] bool replay_established_handshake(
        const HandshakeEnvelope& envelope) noexcept;
    [[nodiscard]] bool register_setup_inbox(std::uint32_t protocol_socket_id,
        IpEndpoint peer, std::shared_ptr<DatagramInbox> inbox,
        std::uint32_t peer_socket_id = 0U) noexcept;
    void unregister_setup_inbox(
        std::uint32_t protocol_socket_id,
        const std::shared_ptr<DatagramInbox>& inbox) noexcept;
    [[nodiscard]] bool set_listener_inbox(
        std::shared_ptr<HandshakeInbox> inbox) noexcept;
    void clear_listener_inbox(
        const std::shared_ptr<HandshakeInbox>& inbox) noexcept;
    [[nodiscard]] bool start() noexcept;
    [[nodiscard]] bool start(std::shared_ptr<RuntimeScheduler> scheduler,
        std::uint64_t affinity) noexcept;
    enum class ShutdownStatus { retired, busy, timeout, worker_thread };
    // Terminal owning channel boundary. Off-worker only: stop channel work,
    // detach all routes, close runtimes/inboxes and retire the native socket.
    // Connection callbacks finish their ring reclamation asynchronously.
    // Concurrent/reentrant calls report busy instead of waiting on callbacks.
    // Deadline covers the active channel task barrier. Runtime close retains
    // its existing mutex/network retry behavior; this is not a global deadline.
    [[nodiscard]] ShutdownStatus shutdown(
        std::chrono::steady_clock::time_point deadline =
            std::chrono::steady_clock::now()
            + std::chrono::milliseconds {250}) noexcept;
    void notify_send_work() noexcept;
    void notify_receive_release() noexcept;
    void set_idle_wait_for_testing(std::chrono::milliseconds timeout) noexcept;
    [[nodiscard]] bool coarse_timer_mode_for_testing() const noexcept
    {
        return coarse_timer_mode_.load(std::memory_order_relaxed);
    }
    void observe_timer_wake_for_testing(
        std::optional<std::uint64_t> lateness_microseconds) noexcept;
    [[nodiscard]] std::uint64_t
    coarse_timer_probe_wakes_for_testing() const noexcept
    {
        return coarse_timer_probe_wakes_.load(std::memory_order_relaxed);
    }
    // Drive one complete receive/poll slice without starting the scheduler.
    [[nodiscard]] RuntimePollResult run_once_for_testing() noexcept
    {
        return run_once();
    }
    // Exercise the same slice with a deterministic datagram source. The native
    // path is statically bound to UdpSocket; no receive hook is stored or polled.
    template <typename Receive>
    [[nodiscard]] RuntimePollResult run_once_for_testing(
        Receive& receive) noexcept
    {
        return run_receive_slice(receive);
    }
    // Drive the connection slice without socket I/O or a running scheduler.
    [[nodiscard]] RuntimePollResult poll_connections_for_testing(
        std::optional<std::chrono::steady_clock::time_point> now = std::nullopt,
        std::chrono::steady_clock::time_point (*clock)(
            void*) noexcept = nullptr,
        void* clock_context = nullptr) noexcept
    {
        return poll_connections(now, clock, clock_context);
    }
    [[nodiscard]] bool running() const noexcept
    {
        return running_.load(std::memory_order_acquire);
    }

private:
    struct UnopenedTag { };
    // Container construction may allocate; let the factory catch failures
    // before any native descriptor is transferred.
    explicit DatagramChannel(UnopenedTag);
    void release_native_credit() noexcept;

    struct ScheduledWorkContext {
        std::weak_ptr<DatagramChannel> owner;
    };

    [[nodiscard]] bool register_connection_locked(
        std::uint32_t protocol_socket_id,
        const std::shared_ptr<ConnectionRuntime>& runtime,
        const std::optional<HandshakeRouteKey>& replay_key);
    void unregister_connection_locked(std::uint32_t protocol_socket_id,
        std::shared_ptr<ConnectionRuntime>& runtime,
        std::shared_ptr<ConnectionDatagramDispatcher>& dispatcher,
        std::shared_ptr<DatagramInbox>& prefix) noexcept;
    [[nodiscard]] bool start_with_affinity(
        std::shared_ptr<RuntimeScheduler> scheduler,
        std::optional<std::uint64_t> affinity) noexcept;
    static void run_scheduled(void* context) noexcept;
    static void run_scheduled_timer(void* context,
        std::optional<std::chrono::steady_clock::time_point>
            idle_wake) noexcept;
    static void socket_readable(void* context) noexcept;
    void run_scheduled(const ScheduledWorkContext* context,
        std::optional<std::chrono::steady_clock::time_point> idle_wake =
            std::nullopt) noexcept;
    [[nodiscard]] RuntimePollResult run_once() noexcept;
    template <typename Receive>
    [[nodiscard]] RuntimePollResult run_receive_slice(
        Receive&& receive) noexcept
    {
        constexpr std::size_t maximum_receive_batch = 64;
        std::array<std::byte, 1500> datagram {};
        std::size_t received_count = 0;
        for (; received_count < maximum_receive_batch; ++received_count) {
            const UdpIoResult received = receive(datagram);
            if (received.error == Error::would_block) {
                break;
            }
            if (received.error == Error::buffer_too_small) {
                continue;
            }
            if (received.error == Error::io_error
                && UdpSocket::is_transient_receive_error(
                    received.system_error)) {
                // A queued ICMP report or interrupted call for one peer
                // is not a fault of the shared socket.
                continue;
            }
            if (!received) {
                mark_connections_broken(received.system_error);
                break;
            }
            const auto decoded = decode_packet(
                std::span {datagram}.first(received.bytes_transferred));
            if (decoded) {
                dispatch(decoded.packet,
                    std::span {datagram}.first(received.bytes_transferred),
                    received.peer);
            }
        }

        auto result = poll_connections();
        // A full slice can leave UDP input unread. After would_block, however,
        // the receive queue is drained: preserve the connection poll deadline
        // instead of forcing another empty receive and complete route sweep.
        if (received_count == maximum_receive_batch) {
            result.immediate_work = true;
            result.next_work_delay.reset();
        }
        return result;
    }
    [[nodiscard]] RuntimePollResult poll_connections(
        std::optional<std::chrono::steady_clock::time_point> injected_now =
            std::nullopt,
        std::chrono::steady_clock::time_point (*clock)(
            void*) noexcept = nullptr,
        void* clock_context = nullptr) noexcept;
    [[nodiscard]] bool schedule_next_locked(bool immediate,
        std::chrono::microseconds delay, bool coarse_timer_probe = false,
        std::optional<std::chrono::steady_clock::time_point> deadline =
            std::nullopt) noexcept;
    void observe_timer_wake_locked(
        std::optional<std::uint64_t> lateness_microseconds,
        std::chrono::steady_clock::time_point now) noexcept;
    void dispatch(
        const PacketView& packet,
        std::span<const std::byte> datagram,
        IpEndpoint peer) noexcept;
    void mark_connections_broken(int system_error) noexcept;
    bool stop(std::chrono::steady_clock::time_point deadline =
                  std::chrono::steady_clock::time_point::max()) noexcept;
    void notify_work(bool receive_release) noexcept;

    std::mutex shutdown_mutex_;
    std::atomic_bool shutdown_requested_ = false;
    bool shutdown_finished_ = false;
    std::mutex send_mutex_;
    bool send_closed_ = false;
    std::mutex lifecycle_mutex_;
    std::condition_variable lifecycle_idle_;
    SendHook send_hook_ = nullptr;
    void* send_hook_context_ = nullptr;
    std::mutex routes_mutex_;
    struct ConnectionRoute {
        std::shared_ptr<ConnectionRuntime> runtime;
        std::optional<HandshakeRouteKey> replay_key = std::nullopt;
        std::shared_ptr<DatagramInbox> setup_prefix = nullptr;
        std::shared_ptr<ConnectionDatagramDispatcher> dispatcher = nullptr;
        bool fault_pending = false;
        ConnectionRoute* previous = nullptr;
        ConnectionRoute* next = nullptr;
    };
    std::unordered_map<std::uint32_t, ConnectionRoute> routes_;
    std::size_t queued_routes_ = 0;
    // Fatal channel failure is terminal for established-route admission. The
    // intrusive cursor follows erasure without allocating a fanout snapshot.
    bool channel_faulted_ = false;
    ConnectionRoute* next_fault_route_ = nullptr;
    std::size_t fault_routes_remaining_ = 0;
    // unordered_map rehash preserves element addresses. Erasure unlinks the
    // node and advances this cursor under routes_mutex_.
    ConnectionRoute* next_poll_route_ = nullptr;
    std::size_t poll_round_remaining_ = 0;
    bool poll_round_immediate_ = false;
    bool poll_round_receive_wait_safe_ = true;
    std::optional<std::chrono::steady_clock::time_point> poll_round_deadline_;
    std::unordered_map<HandshakeRouteKey, ConnectionRoute*,
        HandshakeRouteKeyHash>
        handshake_routes_;
    struct SetupRoute {
        IpEndpoint peer{};
        std::shared_ptr<DatagramInbox> inbox;
        std::uint32_t peer_socket_id = 0U;
    };
    std::unordered_map<std::uint32_t, SetupRoute>
        setup_routes_;
    std::weak_ptr<HandshakeInbox> listener_inbox_;
    std::shared_ptr<RuntimeScheduler> scheduler_;
    std::shared_ptr<SocketReadiness> socket_readiness_;
    SocketReadiness::Token socket_watch_ {};
    bool readiness_parked_ = false;
    std::atomic_bool readiness_available_ = false;
    bool force_timer_polling_for_testing_ = false;
    std::shared_ptr<ScheduledWorkContext> scheduled_work_context_;
    RuntimeScheduler::TimerToken scheduled_timer_ {};
    std::chrono::steady_clock::time_point scheduled_deadline_ {};
    bool scheduled_coarse_timer_probe_ = false;
    std::chrono::steady_clock::time_point next_coarse_timer_probe_ {};
    static constexpr auto coarse_timer_probe_interval_ =
        std::chrono::milliseconds {100};
    std::atomic<std::uint64_t> coarse_timer_probe_wakes_ {0};
    // Sub-millisecond deadlines are timer waits unless the host's timer
    // wake-ups have proven too coarse for the pacer's schedule credit.
    TimerWakeMonitor timer_wake_monitor_ {};
    std::atomic_bool coarse_timer_mode_ {false};
    std::uint64_t affinity_ = 0;
    std::chrono::milliseconds idle_wait_ {2};
    std::thread::id active_thread_ {};
    bool task_active_ = false;
    bool send_work_notification_pending_ = false;
    bool receive_release_pending_ = false;
    std::atomic_bool running_ = false;
};

class ConnectionRuntime {
    friend class ConnectionDatagramDispatcher;
    friend class ConnectionDatagramInbox;
    friend class DatagramChannel;

public:
    [[nodiscard]] const std::shared_ptr<ReadinessSource>&
    readiness_source() const noexcept
    {
        return readiness_source_;
    }
    using Clock = std::chrono::steady_clock;
    using NowFunction = std::uint64_t (*)(void*) noexcept;
    using ReceivePopHook = void (*)(void*) noexcept;
    using CloseRetryHook = void (*)(void*) noexcept;

    struct Configuration {
        std::weak_ptr<DatagramChannel> channel;
        std::shared_ptr<GroupRecord> group;
        IpEndpoint peer{};
        std::uint32_t peer_socket_id = 0;
        SequenceNumber initial_sequence{};
        SequenceNumber peer_initial_sequence{};
        bool has_distinct_peer_initial_sequence = false;
        // SRTO_FC is the local receive window. The peer's advertised window
        // independently limits new outbound DATA. Zero keeps direct/internal
        // construction backward compatible by falling back to the local
        // value.
        std::uint32_t flow_window_packets = 25'600;
        std::uint32_t peer_flow_window_packets = 0;
        SocketOptions options{};
        NegotiatedLiveOptions negotiated_options{};
        bool efficient_retransmission = true;
        Clock::time_point origin{};
        std::uint64_t handshake_arrival_microseconds = 0;
        PacketTimestamp peer_handshake_timestamp{};
        std::uint32_t peer_idle_timeout_milliseconds = 5'000;
        HandshakeAction handshake_replay_response{};
        bool handshake_replay_enabled = false;
        std::uint32_t handshake_replay_peer_cookie = 0;
        std::shared_ptr<CryptoSession> crypto;
        // Receive-direction key-material state recorded during setup when no
        // local session exists, e.g. NOSECRET after answering the peer's KMREQ
        // without a passphrase under optional encryption.
        CryptoState receiver_key_state = CryptoState::unsecured;
        NowFunction now_function = nullptr;
        void* now_context = nullptr;
        // Internal deterministic-test seam, invoked after a successful pop
        // under this runtime's lock. Never installed by the public API.
        ReceivePopHook receive_pop_hook_for_testing = nullptr;
        void* receive_pop_context_for_testing = nullptr;
        // Internal deterministic-test seam before a close retry pause.
        // Never installed by the public API.
        CloseRetryHook close_retry_hook_for_testing = nullptr;
        void* close_retry_context_for_testing = nullptr;
        // Internal opt-in, one binding per connection, fixed before publication.
        // Public setup leaves it null until transport admission/close gates
        // are integrated. Keep the legacy channel wake during that transition.
        std::shared_ptr<ConnectionWorkBinding> work_binding = nullptr;
    };

    explicit ConnectionRuntime(Configuration configuration);
    ~ConnectionRuntime();

    [[nodiscard]] MessageIoResult queue_message(
        std::span<const std::byte> message,
        std::int64_t source_time_microseconds,
        bool in_order,
        bool blocking,
        std::int32_t timeout_milliseconds,
        std::int32_t ttl_milliseconds = -1) noexcept;
    [[nodiscard]] MessageIoResult queue_group_message(
        std::span<const std::byte> message,
        SequenceNumber first_sequence,
        std::uint32_t message_number,
        std::int64_t source_time_microseconds,
        bool in_order,
        std::int32_t ttl_milliseconds = -1) noexcept;
    [[nodiscard]] MessageIoResult skip_group_sequences(
        SequenceNumber next_sequence) noexcept;
    [[nodiscard]] std::int64_t timestamp_origin_microseconds() const noexcept
    {
        return origin_epoch_microseconds_;
    }
    [[nodiscard]] MessageIoResult queue_stream(
        std::span<const std::byte> bytes,
        bool blocking,
        std::int32_t timeout_milliseconds) noexcept;
    [[nodiscard]] MessageIoResult receive_message(
        std::span<std::byte> destination,
        bool blocking,
        std::int32_t timeout_milliseconds) noexcept;
    [[nodiscard]] std::optional<SequenceNumber>
    next_readable_message_sequence() noexcept;
    [[nodiscard]] bool discard_received_before(
        SequenceNumber next_sequence) noexcept;
    // Oldest sequence the receive buffer can still hold or deliver. Any
    // earlier sequence has been consumed, discarded, or dropped and can no
    // longer arrive through this connection.
    [[nodiscard]] SequenceNumber receive_floor_sequence() noexcept;
    [[nodiscard]] bool has_complete_buffered_message_at(
        SequenceNumber sequence) noexcept;
    [[nodiscard]] bool has_buffered_receive_data() noexcept;
    [[nodiscard]] RuntimeReceiveSnapshot receive_snapshot(
        SequenceNumber expected, bool retire_consumed_prefix) noexcept;
    [[nodiscard]] RetainedGroupReceiveBatch
    copy_group_receive_prefix() noexcept;
    [[nodiscard]] SocketReadinessSnapshot readiness_snapshot(
        bool socket_broken) noexcept;
    [[nodiscard]] MessageIoResult receive_stream(
        std::span<std::byte> destination,
        bool blocking,
        std::int32_t timeout_milliseconds) noexcept;
    void process_packet(
        const PacketView& packet,
        IpEndpoint peer) noexcept;
    [[nodiscard]] bool process_handshake(
        const HandshakeMessage& message,
        IpEndpoint peer) noexcept;
    // Advisory replay admission; authoritative check is repeated by the handler.
    [[nodiscard]] bool accepts_handshake_replay(
        const HandshakeMessage& message, IpEndpoint peer) const noexcept;
    [[nodiscard]] RuntimePollResult poll() noexcept;
    [[nodiscard]] RuntimePollResult poll(std::size_t& remaining_send_attempts,
        const ConnectionDatagramInbox* ingress = nullptr,
        std::uint64_t maximum_ingress_wait_microseconds = 0,
        bool coordinate_ingress = false) noexcept;
    void apply_options(const SocketOptions& options) noexcept;
    void mark_broken(int system_error) noexcept;
    [[nodiscard]] bool report_peer_error(
        std::int32_t error_code) noexcept;
    void close() noexcept;

    [[nodiscard]] bool broken() const noexcept;
    [[nodiscard]] bool peer_closed() const noexcept;
    [[nodiscard]] bool terminal() const noexcept;
    // Advisory ingress snapshot; protocol methods still check local close.
    // Matches the packet-handler local-close/broken admission checks.
    [[nodiscard]] bool accepts_datagrams() const noexcept;
    [[nodiscard]] bool readable() noexcept;
    [[nodiscard]] std::optional<Clock::time_point>
    next_readable_deadline() noexcept;
    [[nodiscard]] bool writable() const noexcept;
    [[nodiscard]] bool wait_for_send_drain(
        std::chrono::milliseconds timeout) noexcept;
    [[nodiscard]] CryptoState sender_crypto_state() const noexcept;
    [[nodiscard]] CryptoState receiver_crypto_state() const noexcept;
    [[nodiscard]] std::size_t crypto_key_length() const noexcept;
#ifdef ENABLE_AEAD_API_PREVIEW
    [[nodiscard]] CryptoMode crypto_mode() const noexcept;
#endif
    [[nodiscard]] RuntimeStatisticsSnapshot statistics(
        bool clear_interval, bool instantaneous) noexcept;
    [[nodiscard]] RuntimeResponseHealth response_health() const noexcept;
    [[nodiscard]] SenderBufferStatus sender_buffer_status() noexcept;
    [[nodiscard]] RuntimeBufferPacketCounts
    buffer_packet_counts() const noexcept;
    [[nodiscard]] std::optional<HandshakeRouteKey>
        handshake_replay_key() const noexcept;

private:
    friend void close_connection_runtime(
        const std::shared_ptr<ConnectionRuntime>& runtime,
        std::shared_ptr<RuntimeWorkExecutor> executor,
        std::shared_ptr<ConnectionDatagramDispatcher> retirement) noexcept;
    [[nodiscard]] bool begin_close() noexcept;
    void finish_close() noexcept;
    // The group this member belongs to, if any: notified alongside the
    // member's own source so group watches need no process-wide rescans.
    std::shared_ptr<ReadinessSource> group_readiness_source_;
    std::shared_ptr<ReadinessSource> readiness_source_ =
        std::make_shared<ReadinessSource>();
    void note_not_ready(int events) noexcept;
    void notify_readiness() noexcept;
    struct FecReceiveBatch {
        Error error = Error::none;
        bool consume_control_packet = false;
        std::span<const PacketView>
            reconstructed_packets{};
        std::span<const SequenceRange>
            irrecoverable_losses{};

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return error == Error::none;
        }
    };

    enum class DatagramKind : std::uint8_t {
        other,
        data,
        filter,
        control,
        key_request,
    };

    struct DatagramCompletion {
        DatagramKind kind = DatagramKind::other;
        DataHeader data {};
        ControlType control = ControlType::keepalive;
        std::size_t payload_size = 0;
    };

    struct PendingDatagram {
        std::array<std::byte, DatagramEnvelope::maximum_size> bytes {};
        std::size_t size = 0;
        DatagramCompletion completion {};
        std::unique_ptr<PendingDatagram> next;
    };

    // DATA/FEC preparation stops on the first deferred send. Incoming control
    // responses may still queue while it waits; overflow fails explicitly.
    static constexpr std::size_t pending_datagram_capacity = 128;
    [[nodiscard]] bool submit_datagram(std::span<const std::byte> bytes,
        DatagramCompletion completion, std::uint64_t now) noexcept;
    [[nodiscard]] bool complete_datagram(std::span<const std::byte> bytes,
        const DatagramCompletion& completion, std::uint64_t now) noexcept;
    [[nodiscard]] bool flush_pending_datagrams(std::uint64_t now) noexcept;
    [[nodiscard]] bool defer_send_error(
        const UdpIoResult& failure, std::uint64_t now) noexcept;
    [[nodiscard]] RuntimePollResult pending_send_poll_result(
        std::uint64_t now) const noexcept;

    // Single message attempt while mutex_ is held: no condition-variable wait,
    // retained caller span, or channel wake. The calling wrapper owns waiting
    // and success notification after unlocking, preserving the wait predicate.
    // Send arguments have already passed the control-profile validation.
    [[nodiscard]] MessageIoResult try_queue_message_locked(
        std::span<const std::byte> message,
        std::int64_t source_time_microseconds, bool in_order,
        std::int32_t ttl_milliseconds) noexcept;
    [[nodiscard]] MessageIoResult try_receive_message_locked(
        std::span<std::byte> destination, std::uint64_t now) noexcept;
    // Prefix protocol effects must finish before ordinary polling. This path
    // enforces only its terminal peer-idle bound, without sending or delivery.
    [[nodiscard]] RuntimePollResult poll_setup_prefix_deadline(
        std::uint64_t maximum_ingress_wait_microseconds,
        bool& terminal) noexcept;
    [[nodiscard]] RuntimePollResult poll_locked(
        const ConnectionDatagramInbox* ingress = nullptr,
        std::uint64_t maximum_ingress_wait_microseconds = 0,
        bool coordinate_ingress = false) noexcept;
    [[nodiscard]] ConnectionDatagramInbox::Status admit_datagram(
        ConnectionDatagramInbox& inbox, ConnectionDatagramInbox::Token token,
        std::span<const std::byte> bytes, IpEndpoint peer,
        std::uint64_t now_microseconds) noexcept;
    [[nodiscard]] std::uint64_t now_microseconds() const noexcept;
    [[nodiscard]] PacketTimestamp packet_timestamp(
        std::int64_t source_time_microseconds) const noexcept;
    void sample_sender_buffer_statistics(
        std::uint64_t now_microseconds) noexcept;
    void sample_receiver_buffer_statistics(
        std::uint64_t now_microseconds) noexcept;
    [[nodiscard]] bool matches_handshake_replay(
        const HandshakeMessage& message, IpEndpoint peer) const noexcept;
    void notify_channel_send_work() noexcept;
    void notify_channel_receive_release() noexcept;
    [[nodiscard]] bool send_actions(
        const ReliabilityActions& actions,
        std::uint64_t now_microseconds) noexcept;
    [[nodiscard]] bool send_data(
        const OutboundPacket& packet,
        std::uint64_t now_microseconds) noexcept;
    [[nodiscard]] bool send_filter_control(
        std::uint64_t now_microseconds) noexcept;
    [[nodiscard]] bool fec_encoder_active() const noexcept;
    [[nodiscard]] Error feed_fec_source(
        const PacketView& wire_packet) noexcept;
    [[nodiscard]] bool fec_control_ready() const noexcept;
    [[nodiscard]] std::optional<OutboundPacket>
    fec_control_packet() const noexcept;
    void consume_fec_control() noexcept;
    [[nodiscard]] bool fec_decoder_active() const noexcept;
    [[nodiscard]] FecReceiveBatch receive_fec_packet(
        const PacketView& wire_packet) noexcept;
    [[nodiscard]] bool process_reliability_packet_locked(
        const PacketView& packet,
        std::uint64_t now_microseconds,
        ReliabilityReceiveContext context = {},
        bool wire_received = true) noexcept;
    [[nodiscard]] bool report_filter_losses_locked(
        std::span<const SequenceRange> losses,
        std::uint64_t now_microseconds) noexcept;
    [[nodiscard]] bool send_key_material(
        std::uint16_t subtype,
        std::span<const std::byte> key_material,
        std::uint64_t now_microseconds) noexcept;
    void send_key_material_error_locked(
        CryptoState state, std::uint64_t now_microseconds) noexcept;
    [[nodiscard]] bool discard_received_before_locked(
        SequenceNumber next_sequence) noexcept;
    [[nodiscard]] bool service_key_rotation(
        std::uint64_t now_microseconds) noexcept;
    [[nodiscard]] bool service_receiver_tlpktdrop_locked(
        std::uint64_t now_microseconds) noexcept;
    [[nodiscard]] std::optional<Clock::time_point>
    next_receive_wakeup_locked(
        std::uint64_t now_microseconds) noexcept;
    [[nodiscard]] bool send_peer_error_locked(
        std::int32_t error_code,
        std::uint64_t now_microseconds) noexcept;
    void break_locked(int system_error) noexcept;

    mutable std::mutex mutex_;
    std::condition_variable receive_ready_;
    std::condition_variable send_ready_;
    std::weak_ptr<DatagramChannel> channel_;
    const std::shared_ptr<ConnectionWorkBinding> work_binding_;
    const IpEndpoint peer_ {};
    const std::uint32_t peer_socket_id_ = 0;
    ReliabilitySession session_;
    PacketPacer pacer_;
    SocketOptions options_;
#ifdef ENABLE_MAXREXMITBW
    RetransmissionBudget retransmission_budget_;

#endif
    [[nodiscard]] bool retransmission_ready(std::uint64_t now) noexcept;
    RuntimeStatisticsState statistics_;
    Clock::time_point origin_{};
    std::int64_t origin_epoch_microseconds_ = 0;
    std::uint64_t peer_idle_timeout_microseconds_ = 5'000'000;
    std::uint64_t last_peer_activity_microseconds_ = 0;
    static constexpr std::uint64_t maximum_ingress_poll_wait_microseconds =
        2000;
    // Runtime mutex protects the one optional channel-poll ingress cohort.
    // Expiry releases polling until that original cohort completes; additional
    // publications cannot restart its wait or starve protocol timers/sends.
    std::uint64_t poll_ingress_incarnation_ = 0;
    std::uint64_t poll_ingress_cutoff_ = 0;
    std::uint64_t poll_ingress_deadline_ = 0;
    bool poll_ingress_active_ = false;
    bool poll_ingress_expired_ = false;
    const HandshakeAction handshake_replay_response_ {};
    const std::uint32_t handshake_replay_peer_cookie_ = 0;
    std::shared_ptr<CryptoSession> crypto_;
    CryptoState receiver_key_state_ = CryptoState::unsecured;
    // Non-null only during poll(), while mutex_ is held. Counts actual UDP
    // attempts; yielding for the channel budget is not UDP backpressure.
    std::size_t* poll_send_budget_ = nullptr;
    std::unique_ptr<PendingDatagram> pending_datagram_head_;
    PendingDatagram* pending_datagram_tail_ = nullptr;
    std::size_t pending_datagram_size_ = 0;
    std::uint64_t next_datagram_retry_microseconds_ = 0;
    std::optional<std::uint64_t> transient_send_failure_since_;
    int transient_send_system_error_ = 0;
    std::optional<RowFecEncoder> row_fec_encoder_;
    std::optional<RowFecDecoder> row_fec_decoder_;
    std::optional<ColumnFecEncoder> column_fec_encoder_;
    std::optional<ColumnFecDecoder> column_fec_decoder_;
    std::optional<MatrixFecEncoder> matrix_fec_encoder_;
    std::optional<MatrixFecDecoder> matrix_fec_decoder_;
    std::array<PacketView, 1>
        single_fec_reconstructed_packet_{};
    std::optional<std::uint64_t> last_key_material_send_microseconds_;
    void observe_key_sequence_position(
        std::uint64_t position, std::uint64_t now) noexcept;
    std::optional<std::uint64_t> key_rate_sample_microseconds_;
    std::uint64_t key_rate_sample_position_ = 0;
    std::uint64_t key_peak_sequence_rate_ = 0;
    std::optional<std::uint64_t> last_key_material_error_microseconds_;
    // Admission of KMREQs that need a PBKDF2 derivation (fresh salt). The
    // derivation is the expensive, unauthenticated work a flood can target.
    // A token bucket bounds first-seen salts; a salt seen again (a peer
    // retrying its request) uses a separate lane. A rotating Bloom filter
    // retains recent candidates without packet-count-based eviction. False
    // positives and saturation can still create retry-lane contention.
    struct FreshSaltAdmission {
        static constexpr std::uint64_t refill_interval_microseconds = 100'000;
        static constexpr std::uint32_t bucket_capacity = 4;
        static constexpr std::size_t salt_window_count = 3;
        static constexpr std::size_t salt_window_bits = 32'768;
        std::uint32_t tokens = bucket_capacity;
        std::optional<std::uint64_t> last_refill_microseconds;
        std::optional<std::uint64_t> last_repeat_derivation_microseconds;
        std::array<std::array<std::uint64_t, salt_window_bits / 64U>,
            salt_window_count>
            salt_windows {};
        std::optional<std::uint64_t> salt_epoch;
    };
    std::unique_ptr<FreshSaltAdmission> fresh_salt_admission_;
    [[nodiscard]] bool admit_fresh_salt_key_request(
        std::span<const std::byte> payload, std::uint64_t now) noexcept;
    NowFunction now_function_ = nullptr;
    void* now_context_ = nullptr;
    ReceivePopHook receive_pop_hook_for_testing_ = nullptr;
    void* receive_pop_context_for_testing_ = nullptr;
    CloseRetryHook close_retry_hook_for_testing_ = nullptr;
    void* close_retry_context_for_testing_ = nullptr;
    std::size_t flow_window_packets_ = 1;
    int system_error_ = 0;
    bool locally_closed_ = false;
    bool peer_closed_ = false;
    bool peer_error_pending_ = false;
    bool broken_ = false;
    bool last_readable_state_ = false;
};

// Owning close boundary used by compatibility call sites. Affinity workers
// hand the final bounded drain to the fixed work executor after quiescing the
// runtime. Queue/resource failure preserves the synchronous bounded cleanup.
// An explicit executor permits deterministic capacity/lifetime tests.
void close_connection_runtime(const std::shared_ptr<ConnectionRuntime>& runtime,
    std::shared_ptr<RuntimeWorkExecutor> executor = {},
    std::shared_ptr<ConnectionDatagramDispatcher> retirement = {}) noexcept;

} // namespace robotweax::srt::compat
