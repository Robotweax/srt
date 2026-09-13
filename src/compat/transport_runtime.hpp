#pragma once

#include "robotweax/srt/fec.hpp"
#include "robotweax/srt/handshake_datagram.hpp"
#include "robotweax/srt/session.hpp"
#include "robotweax/srt/socket_options.hpp"
#include "robotweax/srt/udp.hpp"
#include "compat/runtime_scheduler.hpp"
#include "compat/statistics.hpp"

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
struct GroupRecord;

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

private:
    std::mutex mutex_;
    std::condition_variable ready_;
    std::vector<DatagramEnvelope> entries_;
    std::size_t head_ = 0;
    std::size_t size_ = 0;
    ReadyFunction ready_handler_ = nullptr;
    std::weak_ptr<void> ready_context_;
    bool closed_ = false;
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

struct RuntimePollResult {
    bool immediate_work = false;
    std::optional<std::chrono::microseconds> next_work_delay;
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

class DatagramChannel : public std::enable_shared_from_this<DatagramChannel> {
public:
    using SendHook = UdpIoResult (*)(
        std::span<const std::byte>,
        IpEndpoint,
        void*) noexcept;

    DatagramChannel() = default;
    explicit DatagramChannel(IpAddressFamily family) noexcept;
    explicit DatagramChannel(UdpSocket acquired_socket) noexcept;
    ~DatagramChannel();

    DatagramChannel(const DatagramChannel&) = delete;
    DatagramChannel& operator=(const DatagramChannel&) = delete;

    UdpSocket socket;
    std::mutex receive_mutex;

    [[nodiscard]] UdpIoResult send_datagram(
        std::span<const std::byte> bytes,
        IpEndpoint peer) noexcept;
    void set_send_hook_for_testing(
        SendHook hook, void* context) noexcept;
    [[nodiscard]] bool register_connection(
        std::uint32_t protocol_socket_id,
        std::shared_ptr<ConnectionRuntime> runtime) noexcept;
    [[nodiscard]] bool promote_setup_connection(
        std::uint32_t protocol_socket_id,
        const std::shared_ptr<DatagramInbox>& inbox,
        std::shared_ptr<ConnectionRuntime> runtime) noexcept;
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
    void notify_send_work() noexcept;
    void set_idle_wait_for_testing(std::chrono::milliseconds timeout) noexcept;
    [[nodiscard]] bool running() const noexcept
    {
        return running_.load(std::memory_order_acquire);
    }

private:
    struct ScheduledWorkContext {
        std::weak_ptr<DatagramChannel> owner;
    };

    [[nodiscard]] bool register_connection_locked(
        std::uint32_t protocol_socket_id,
        const std::shared_ptr<ConnectionRuntime>& runtime);
    void drain_setup_inbox_locked(
        const std::shared_ptr<DatagramInbox>& inbox,
        const std::shared_ptr<ConnectionRuntime>& runtime,
        IpEndpoint peer) noexcept;
    static void run_scheduled(void* context) noexcept;
    void run_scheduled(const ScheduledWorkContext* context) noexcept;
    [[nodiscard]] RuntimePollResult run_once() noexcept;
    [[nodiscard]] bool schedule_next_locked(
        bool immediate, std::chrono::microseconds delay) noexcept;
    void dispatch(
        const PacketView& packet,
        std::span<const std::byte> datagram,
        IpEndpoint peer) noexcept;
    void mark_connections_broken(int system_error) noexcept;
    void stop() noexcept;

    std::mutex send_mutex_;
    std::mutex lifecycle_mutex_;
    std::condition_variable lifecycle_idle_;
    SendHook send_hook_ = nullptr;
    void* send_hook_context_ = nullptr;
    std::mutex routes_mutex_;
    std::unordered_map<std::uint32_t,
        std::shared_ptr<ConnectionRuntime>> routes_;
    std::unordered_map<HandshakeRouteKey,
        std::shared_ptr<ConnectionRuntime>,
        HandshakeRouteKeyHash> handshake_routes_;
    struct SetupRoute {
        IpEndpoint peer{};
        std::shared_ptr<DatagramInbox> inbox;
        std::uint32_t peer_socket_id = 0U;
    };
    std::unordered_map<std::uint32_t, SetupRoute>
        setup_routes_;
    std::weak_ptr<HandshakeInbox> listener_inbox_;
    std::shared_ptr<RuntimeScheduler> scheduler_;
    std::shared_ptr<ScheduledWorkContext> scheduled_work_context_;
    RuntimeScheduler::TimerToken scheduled_timer_ {};
    std::uint64_t affinity_ = 0;
    std::chrono::milliseconds idle_wait_ {2};
    std::thread::id active_thread_ {};
    bool task_active_ = false;
    bool send_work_notification_pending_ = false;
    std::atomic_bool running_ = false;
};

class ConnectionRuntime {
public:
    using Clock = std::chrono::steady_clock;
    using NowFunction = std::uint64_t (*)(void*) noexcept;
    using ReceivePopHook = void (*)(void*) noexcept;

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
        Clock::time_point origin{};
        std::uint64_t handshake_arrival_microseconds = 0;
        PacketTimestamp peer_handshake_timestamp{};
        std::uint32_t peer_idle_timeout_milliseconds = 5'000;
        HandshakeAction handshake_replay_response{};
        bool handshake_replay_enabled = false;
        std::uint32_t handshake_replay_peer_cookie = 0;
        std::shared_ptr<CryptoSession> crypto;
        NowFunction now_function = nullptr;
        void* now_context = nullptr;
        // Internal deterministic-test seam, invoked after a successful pop
        // under this runtime's lock. Never installed by the public API.
        ReceivePopHook receive_pop_hook_for_testing = nullptr;
        void* receive_pop_context_for_testing = nullptr;
    };

    explicit ConnectionRuntime(Configuration configuration);

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
    [[nodiscard]] RuntimePollResult poll() noexcept;
    void apply_options(const SocketOptions& options) noexcept;
    void mark_broken(int system_error) noexcept;
    [[nodiscard]] bool report_peer_error(
        std::int32_t error_code) noexcept;
    void close() noexcept;

    [[nodiscard]] bool broken() const noexcept;
    [[nodiscard]] bool peer_closed() const noexcept;
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

    [[nodiscard]] std::uint64_t now_microseconds() const noexcept;
    [[nodiscard]] PacketTimestamp packet_timestamp(
        std::int64_t source_time_microseconds) const noexcept;
    void sample_sender_buffer_statistics(
        std::uint64_t now_microseconds) noexcept;
    void sample_receiver_buffer_statistics(
        std::uint64_t now_microseconds) noexcept;
    void notify_channel_send_work() noexcept;
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
    IpEndpoint peer_{};
    std::uint32_t peer_socket_id_ = 0;
    ReliabilitySession session_;
    PacketPacer pacer_;
    SocketOptions options_;
    RuntimeStatisticsState statistics_;
    Clock::time_point origin_{};
    std::int64_t origin_epoch_microseconds_ = 0;
    std::uint64_t peer_idle_timeout_microseconds_ = 5'000'000;
    std::uint64_t last_peer_activity_microseconds_ = 0;
    HandshakeAction handshake_replay_response_{};
    std::uint32_t handshake_replay_peer_cookie_ = 0;
    std::shared_ptr<CryptoSession> crypto_;
    std::optional<RowFecEncoder> row_fec_encoder_;
    std::optional<RowFecDecoder> row_fec_decoder_;
    std::optional<ColumnFecEncoder> column_fec_encoder_;
    std::optional<ColumnFecDecoder> column_fec_decoder_;
    std::optional<MatrixFecEncoder> matrix_fec_encoder_;
    std::optional<MatrixFecDecoder> matrix_fec_decoder_;
    std::array<PacketView, 1>
        single_fec_reconstructed_packet_{};
    std::uint64_t last_key_material_send_microseconds_ = 0;
    NowFunction now_function_ = nullptr;
    void* now_context_ = nullptr;
    ReceivePopHook receive_pop_hook_for_testing_ = nullptr;
    void* receive_pop_context_for_testing_ = nullptr;
    std::size_t flow_window_packets_ = 1;
    int system_error_ = 0;
    bool locally_closed_ = false;
    bool peer_closed_ = false;
    bool peer_error_pending_ = false;
    bool broken_ = false;
    bool last_readable_state_ = false;
};

} // namespace robotweax::srt::compat
