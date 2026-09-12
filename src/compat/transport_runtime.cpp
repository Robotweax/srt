#include "compat/transport_runtime.hpp"

#include "robotweax/srt/codec.hpp"
#include "compat/readiness.hpp"
#include "compat/group_registry.hpp"
#include "compat/runtime_scheduler_service.hpp"

#include <algorithm>
#include <atomic>
#include <limits>
#include <new>

namespace robotweax::srt::compat {
namespace {

constexpr std::size_t maximum_receive_batch = 64;

[[nodiscard]] std::uint64_t next_channel_affinity() noexcept
{
    // Aligned object addresses would collapse onto one shard under modulo
    // placement. A process-local sequence distributes channel ownership.
    static std::atomic<std::uint64_t> next_affinity {0U};
    return next_affinity.fetch_add(1U, std::memory_order_relaxed);
}

constexpr std::size_t maximum_send_batch = 64;

[[nodiscard]] constexpr std::array<std::byte, 4>
crypto_state_payload(CryptoState state) noexcept
{
    const std::uint32_t value = static_cast<std::uint32_t>(state);
    return {
        static_cast<std::byte>((value >> 24U) & 0xffU),
        static_cast<std::byte>((value >> 16U) & 0xffU),
        static_cast<std::byte>((value >> 8U) & 0xffU),
        static_cast<std::byte>(value & 0xffU),
    };
}

[[nodiscard]] bool has_valid_runtime_key_material_payload(
    std::uint16_t subtype, std::span<const std::byte> payload) noexcept
{
    if (subtype == key_material_request_subtype) {
        return static_cast<bool>(decode_key_material(payload));
    }
    if (subtype != key_material_response_subtype) {
        return true;
    }
    if (payload.size() != sizeof(std::uint32_t)) {
        return static_cast<bool>(decode_key_material(payload));
    }
    const std::uint32_t state =
        (std::to_integer<std::uint32_t>(payload[0]) << 24U)
        | (std::to_integer<std::uint32_t>(payload[1]) << 16U)
        | (std::to_integer<std::uint32_t>(payload[2]) << 8U)
        | std::to_integer<std::uint32_t>(payload[3]);
    return state <= static_cast<std::uint32_t>(CryptoState::bad_crypto_mode);
}

[[nodiscard]] std::size_t effective_receive_capacity(
    const ConnectionRuntime::Configuration& configuration) noexcept
{
    return std::min<std::size_t>(
        configuration.options.receive_buffer_packets(),
        configuration.flow_window_packets);
}

[[nodiscard]] std::size_t effective_peer_flow_window(
    const ConnectionRuntime::Configuration& configuration) noexcept
{
    return configuration.peer_flow_window_packets != 0U
        ? configuration.peer_flow_window_packets
        : configuration.flow_window_packets;
}

[[nodiscard]] std::size_t effective_maximum_payload_size(
    const ConnectionRuntime::Configuration& configuration) noexcept
{
    const std::size_t configured = configuration.options.maximum_payload_size();
    if (configuration.crypto == nullptr
        || !configuration.crypto->authenticated_data_enabled()) {
        return configured;
    }
    const auto budget = configuration.options.payload_budget(
        configuration.peer.wire_family(), CryptoMode::aes_gcm);
    return budget ? std::min(configured, budget.maximum_plaintext_payload_size)
                  : configured;
}

[[nodiscard]] std::size_t effective_fec_payload_size(
    const ConnectionRuntime::Configuration& configuration,
    const std::shared_ptr<CryptoSession>& crypto) noexcept
{
    if (crypto == nullptr || !crypto->authenticated_data_enabled()) {
        return configuration.options.maximum_payload_size();
    }
    const auto budget = configuration.options.payload_budget(
        configuration.peer.wire_family(), CryptoMode::aes_gcm);
    if (!budget) {
        return configuration.options.maximum_payload_size();
    }
    const std::size_t plaintext =
        std::min(configuration.options.maximum_payload_size(),
            budget.maximum_plaintext_payload_size);
    return std::min(budget.maximum_wire_payload_size,
        plaintext + budget.authentication_tag_size);
}

[[nodiscard]] std::optional<ControlType> control_type(
    ReliabilityActionKind kind) noexcept
{
    switch (kind) {
    case ReliabilityActionKind::acknowledgement:
        return ControlType::acknowledgement;
    case ReliabilityActionKind::acknowledgement_of_ack:
        return ControlType::acknowledgement_of_ack;
    case ReliabilityActionKind::loss_report:
        return ControlType::negative_acknowledgement;
    case ReliabilityActionKind::drop_request:
        return ControlType::drop_request;
    case ReliabilityActionKind::keepalive:
        return ControlType::keepalive;
    case ReliabilityActionKind::shutdown:
        return ControlType::shutdown;
    }
    return std::nullopt;
}

[[nodiscard]] constexpr std::size_t saturated_multiply(
    std::size_t left, std::size_t right) noexcept
{
    return left != 0U
            && right > std::numeric_limits<std::size_t>::max() / left
        ? std::numeric_limits<std::size_t>::max()
        : left * right;
}

[[nodiscard]] constexpr std::size_t saturated_add(
    std::size_t left, std::size_t right) noexcept
{
    return left > std::numeric_limits<std::size_t>::max() - right
        ? std::numeric_limits<std::size_t>::max()
        : left + right;
}

[[nodiscard]] std::uint64_t elapsed_microseconds(
    ConnectionRuntime::Clock::time_point origin) noexcept
{
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        ConnectionRuntime::Clock::now() - origin).count();
    return elapsed <= 0 ? 0U : static_cast<std::uint64_t>(elapsed);
}

[[nodiscard]] ConnectionRuntime::Clock::time_point
deadline_from_origin_microseconds(ConnectionRuntime::Clock::time_point origin,
    std::uint64_t target_microseconds) noexcept
{
    const auto remaining =
        std::chrono::duration_cast<std::chrono::microseconds>(
            ConnectionRuntime::Clock::time_point::max() - origin)
            .count();
    const auto bounded = std::min<std::uint64_t>(target_microseconds,
        remaining <= 0 ? 0U : static_cast<std::uint64_t>(remaining));
    return origin + std::chrono::microseconds {bounded};
}

[[nodiscard]] ConnectionRuntime::Clock::time_point
deadline_after_relative_microseconds(
    std::uint64_t now_microseconds,
    std::uint64_t target_microseconds) noexcept
{
    const auto steady_now = ConnectionRuntime::Clock::now();
    if (target_microseconds <= now_microseconds) {
        return steady_now;
    }
    const std::uint64_t requested =
        target_microseconds - now_microseconds;
    const auto remaining = std::chrono::duration_cast<
        std::chrono::microseconds>(
            ConnectionRuntime::Clock::time_point::max()
            - steady_now).count();
    const auto bounded = std::min<std::uint64_t>(
        requested,
        remaining <= 0
            ? 0U
            : static_cast<std::uint64_t>(remaining));
    return steady_now + std::chrono::microseconds{bounded};
}

[[nodiscard]] std::int64_t epoch_microseconds(
    ConnectionRuntime::Clock::time_point time) noexcept
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
        time.time_since_epoch()).count();
}

[[nodiscard]] std::uint64_t message_expiration_microseconds(
    std::uint64_t now_microseconds,
    std::int64_t source_time_microseconds,
    std::int64_t origin_epoch_microseconds,
    std::int32_t ttl_milliseconds) noexcept
{
    if (ttl_milliseconds < 0) {
        return 0;
    }

    std::uint64_t ttl_origin_microseconds = now_microseconds;
    if (source_time_microseconds > 0) {
        const std::int64_t relative_source_time =
            source_time_microseconds - origin_epoch_microseconds;
        // Zero is reserved as the unlimited-TTL sentinel.
        ttl_origin_microseconds = relative_source_time > 0
            ? static_cast<std::uint64_t>(relative_source_time)
            : 1U;
    }

    const std::uint64_t ttl_microseconds =
        static_cast<std::uint64_t>(ttl_milliseconds) * 1'000U;
    if (ttl_origin_microseconds
        > std::numeric_limits<std::uint64_t>::max()
            - ttl_microseconds) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    const std::uint64_t expiration =
        ttl_origin_microseconds + ttl_microseconds;
    return expiration == 0U ? 1U : expiration;
}

} // namespace

std::size_t HandshakeRouteKeyHash::operator()(
    const HandshakeRouteKey& key) const noexcept
{
    std::uint64_t packed_address = key.peer_socket_id;
    for (const std::uint8_t octet : key.peer.address) {
        packed_address = (packed_address * 1'099'511'628'211ULL)
            ^ static_cast<std::uint64_t>(octet);
    }
    packed_address = (packed_address * 1'099'511'628'211ULL)
        ^ static_cast<std::uint64_t>(key.peer.port);
    packed_address = (packed_address * 1'099'511'628'211ULL)
        ^ static_cast<std::uint64_t>(key.peer.scope_id);
    packed_address = (packed_address * 1'099'511'628'211ULL)
        ^ static_cast<std::uint64_t>(key.peer.family);
    return static_cast<std::size_t>(
        packed_address ^ (packed_address >> 32U));
}

HandshakeInbox::HandshakeInbox(std::size_t capacity)
    : entries_(std::max<std::size_t>(capacity, 1U))
{
}

bool HandshakeInbox::push(const HandshakeEnvelope& envelope) noexcept
{
    ReadyFunction handler = nullptr;
    std::shared_ptr<void> handler_context;
    {
        std::lock_guard lock(mutex_);
        if (closed_ || size_ == entries_.size()) {
            return false;
        }
        const bool became_ready = size_ == 0U;
        entries_[(head_ + size_) % entries_.size()] = envelope;
        ++size_;
        if (became_ready && ready_handler_ != nullptr) {
            handler_context = ready_context_.lock();
            if (handler_context != nullptr) {
                handler = ready_handler_;
            }
        }
    }
    ready_.notify_one();
    ReadinessSignal::notify();
    if (handler != nullptr) {
        handler(handler_context.get());
    }
    return true;
}

InboxPopStatus HandshakeInbox::pop_for(
    HandshakeEnvelope& envelope,
    std::chrono::milliseconds timeout) noexcept
{
    std::unique_lock lock(mutex_);
    if (!ready_.wait_for(lock, timeout, [this] {
            return size_ != 0U || closed_;
        })) {
        return InboxPopStatus::timeout;
    }
    if (size_ == 0U) {
        return InboxPopStatus::closed;
    }
    envelope = entries_[head_];
    head_ = (head_ + 1U) % entries_.size();
    --size_;
    lock.unlock();
    ReadinessSignal::notify();
    return InboxPopStatus::received;
}

InboxPopStatus HandshakeInbox::pop_matching(HandshakeEnvelope& envelope,
    IpEndpoint peer, std::uint32_t peer_socket_id) noexcept
{
    std::unique_lock lock(mutex_);
    for (std::size_t offset = 0; offset < size_; ++offset) {
        const std::size_t index = (head_ + offset) % entries_.size();
        if (entries_[index].peer != peer
            || entries_[index].message.packet.socket_id != peer_socket_id) {
            continue;
        }
        envelope = entries_[index];
        for (std::size_t position = offset; position + 1U < size_; ++position) {
            const std::size_t destination =
                (head_ + position) % entries_.size();
            const std::size_t source =
                (head_ + position + 1U) % entries_.size();
            entries_[destination] = std::move(entries_[source]);
        }
        --size_;
        lock.unlock();
        ReadinessSignal::notify();
        return InboxPopStatus::received;
    }
    return closed_ ? InboxPopStatus::closed : InboxPopStatus::timeout;
}

bool HandshakeInbox::set_ready_handler(
    ReadyFunction function, std::weak_ptr<void> context) noexcept
{
    if (function == nullptr) {
        return false;
    }
    std::shared_ptr<void> handler_context = context.lock();
    if (handler_context == nullptr) {
        return false;
    }
    bool notify = false;
    {
        std::lock_guard lock(mutex_);
        if (ready_handler_ != nullptr) {
            return false;
        }
        ready_handler_ = function;
        ready_context_ = std::move(context);
        notify = size_ != 0U || closed_;
    }
    if (notify) {
        function(handler_context.get());
    }
    return true;
}

void HandshakeInbox::clear_ready_handler() noexcept
{
    std::lock_guard lock(mutex_);
    ready_handler_ = nullptr;
    ready_context_.reset();
}

bool HandshakeInbox::ready() noexcept
{
    std::lock_guard lock(mutex_);
    return size_ != 0U || closed_;
}

void HandshakeInbox::close() noexcept
{
    ReadyFunction handler = nullptr;
    std::shared_ptr<void> handler_context;
    {
        std::lock_guard lock(mutex_);
        if (closed_) {
            return;
        }
        closed_ = true;
        if (ready_handler_ != nullptr) {
            handler_context = ready_context_.lock();
            if (handler_context != nullptr) {
                handler = ready_handler_;
            }
        }
    }
    ready_.notify_all();
    ReadinessSignal::notify();
    if (handler != nullptr) {
        handler(handler_context.get());
    }
}

DatagramInbox::DatagramInbox(std::size_t capacity)
    : entries_(std::max<std::size_t>(capacity, 1U))
{
}

bool DatagramInbox::push(
    std::span<const std::byte> datagram,
    IpEndpoint peer) noexcept
{
    if (datagram.empty()
        || datagram.size()
            > DatagramEnvelope::maximum_size) {
        return false;
    }
    ReadyFunction handler = nullptr;
    std::shared_ptr<void> handler_context;
    {
        std::lock_guard lock(mutex_);
        if (closed_ || size_ == entries_.size()) {
            return false;
        }
        const bool became_ready = size_ == 0U;
        const std::size_t tail = (head_ + size_) % entries_.size();
        DatagramEnvelope& envelope = entries_[tail];
        std::copy(datagram.begin(), datagram.end(), envelope.bytes.begin());
        envelope.size = datagram.size();
        envelope.peer = peer;
        ++size_;
        if (became_ready && ready_handler_ != nullptr) {
            handler_context = ready_context_.lock();
            if (handler_context != nullptr) {
                handler = ready_handler_;
            }
        }
    }
    ready_.notify_one();
    ReadinessSignal::notify();
    if (handler != nullptr) {
        handler(handler_context.get());
    }
    return true;
}

InboxPopStatus DatagramInbox::pop_for(
    DatagramEnvelope& envelope,
    std::chrono::milliseconds timeout) noexcept
{
    std::unique_lock lock(mutex_);
    if (!ready_.wait_for(lock, timeout, [this] {
            return size_ != 0U || closed_;
        })) {
        return InboxPopStatus::timeout;
    }
    if (size_ == 0U) {
        return InboxPopStatus::closed;
    }
    envelope = entries_[head_];
    head_ = (head_ + 1U) % entries_.size();
    --size_;
    lock.unlock();
    ReadinessSignal::notify();
    return InboxPopStatus::received;
}

bool DatagramInbox::set_ready_handler(
    ReadyFunction function, std::weak_ptr<void> context) noexcept
{
    if (function == nullptr) {
        return false;
    }
    std::shared_ptr<void> handler_context = context.lock();
    if (handler_context == nullptr) {
        return false;
    }
    bool notify = false;
    {
        std::lock_guard lock(mutex_);
        if (ready_handler_ != nullptr) {
            return false;
        }
        ready_handler_ = function;
        ready_context_ = std::move(context);
        notify = size_ != 0U || closed_;
    }
    if (notify) {
        function(handler_context.get());
    }
    return true;
}

void DatagramInbox::clear_ready_handler() noexcept
{
    std::lock_guard lock(mutex_);
    ready_handler_ = nullptr;
    ready_context_.reset();
}

bool DatagramInbox::ready() noexcept
{
    std::lock_guard lock(mutex_);
    return size_ != 0U || closed_;
}

void DatagramInbox::close() noexcept
{
    ReadyFunction handler = nullptr;
    std::shared_ptr<void> handler_context;
    {
        std::lock_guard lock(mutex_);
        if (closed_) {
            return;
        }
        closed_ = true;
        if (ready_handler_ != nullptr) {
            handler_context = ready_context_.lock();
            if (handler_context != nullptr) {
                handler = ready_handler_;
            }
        }
    }
    ready_.notify_all();
    ReadinessSignal::notify();
    if (handler != nullptr) {
        handler(handler_context.get());
    }
}

DatagramChannel::DatagramChannel(
    IpAddressFamily family) noexcept
    : socket(family)
{
}

DatagramChannel::DatagramChannel(
    UdpSocket acquired_socket) noexcept
    : socket(std::move(acquired_socket))
{
}

DatagramChannel::~DatagramChannel()
{
    stop();
}

UdpIoResult DatagramChannel::send_datagram(
    std::span<const std::byte> bytes,
    IpEndpoint peer) noexcept
{
    std::lock_guard lock(send_mutex_);
    if (send_hook_ != nullptr) {
        return send_hook_(bytes, peer, send_hook_context_);
    }
    return socket.send_to(bytes, peer);
}

void DatagramChannel::set_send_hook_for_testing(
    SendHook hook, void* context) noexcept
{
    std::lock_guard lock(send_mutex_);
    send_hook_ = hook;
    send_hook_context_ = context;
}

bool DatagramChannel::register_connection(
    std::uint32_t protocol_socket_id,
    std::shared_ptr<ConnectionRuntime> runtime) noexcept
{
    if (protocol_socket_id == 0U || runtime == nullptr) {
        return false;
    }
    try {
        std::lock_guard lock(routes_mutex_);
        return register_connection_locked(
            protocol_socket_id, runtime);
    } catch (const std::bad_alloc&) {
        return false;
    } catch (...) {
        return false;
    }
}

bool DatagramChannel::register_connection_locked(
    std::uint32_t protocol_socket_id,
    const std::shared_ptr<ConnectionRuntime>& runtime)
{
    const auto replay_key = runtime->handshake_replay_key();
    const auto inserted = routes_.emplace(
        protocol_socket_id, runtime);
    if (!inserted.second) {
        return false;
    }
    if (replay_key.has_value()) {
        try {
            if (!handshake_routes_.emplace(
                    *replay_key, runtime).second) {
                routes_.erase(inserted.first);
                return false;
            }
        } catch (...) {
            routes_.erase(inserted.first);
            throw;
        }
    }
    return true;
}

void DatagramChannel::drain_setup_inbox_locked(
    const std::shared_ptr<DatagramInbox>& inbox,
    const std::shared_ptr<ConnectionRuntime>& runtime,
    IpEndpoint peer) noexcept
{
    DatagramEnvelope envelope;
    while (inbox->pop_for(
               envelope, std::chrono::milliseconds{0})
        == InboxPopStatus::received) {
        if (envelope.peer != peer) {
            continue;
        }
        const auto decoded = decode_packet(
            std::span{envelope.bytes}.first(envelope.size));
        if (!decoded) {
            continue;
        }
        if (decoded.packet.kind == PacketKind::control
            && decoded.packet.control.type
                == ControlType::handshake) {
            const auto handshake = decode_handshake_datagram(
                std::span{envelope.bytes}.first(envelope.size));
            if (handshake) {
                (void)runtime->process_handshake(
                    handshake.message, peer);
            }
            continue;
        }
        runtime->process_packet(decoded.packet, peer);
    }
}

bool DatagramChannel::promote_setup_connection(
    std::uint32_t protocol_socket_id,
    const std::shared_ptr<DatagramInbox>& inbox,
    std::shared_ptr<ConnectionRuntime> runtime) noexcept
{
    if (protocol_socket_id == 0U
        || inbox == nullptr
        || runtime == nullptr) {
        return false;
    }
    try {
        std::lock_guard lock(routes_mutex_);
        const auto setup = setup_routes_.find(protocol_socket_id);
        if (setup == setup_routes_.end()
            || setup->second.inbox != inbox
            || !register_connection_locked(
                protocol_socket_id, runtime)) {
            return false;
        }
        const IpEndpoint peer = setup->second.peer;
        setup_routes_.erase(setup);
        // Keep the routing lock until the bounded setup queue has drained.
        // This preserves wire arrival order: newer datagrams cannot enter the
        // runtime before packets received immediately behind the handshake.
        drain_setup_inbox_locked(inbox, runtime, peer);
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    } catch (...) {
        return false;
    }
}

void DatagramChannel::unregister_connection(
    std::uint32_t protocol_socket_id) noexcept
{
    std::lock_guard lock(routes_mutex_);
    const auto route = routes_.find(protocol_socket_id);
    if (route == routes_.end()) {
        return;
    }
    const auto replay_key = route->second->handshake_replay_key();
    if (replay_key.has_value()) {
        handshake_routes_.erase(*replay_key);
    }
    routes_.erase(route);
}

bool DatagramChannel::replay_established_handshake(
    const HandshakeEnvelope& envelope) noexcept
{
    std::shared_ptr<ConnectionRuntime> runtime;
    {
        std::lock_guard lock(routes_mutex_);
        const auto replay = handshake_routes_.find({
            .peer = envelope.peer,
            .peer_socket_id = envelope.message.packet.socket_id,
        });
        if (replay != handshake_routes_.end()) {
            runtime = replay->second;
        }
    }
    return runtime != nullptr
        && runtime->process_handshake(envelope.message, envelope.peer);
}

bool DatagramChannel::register_setup_inbox(std::uint32_t protocol_socket_id,
    IpEndpoint peer, std::shared_ptr<DatagramInbox> inbox,
    std::uint32_t peer_socket_id) noexcept
{
    if (protocol_socket_id == 0U || inbox == nullptr) {
        return false;
    }
    try {
        std::lock_guard lock(routes_mutex_);
        return setup_routes_
            .emplace(protocol_socket_id,
                SetupRoute {
                    .peer = peer,
                    .inbox = std::move(inbox),
                    .peer_socket_id = peer_socket_id,
                })
            .second;
    } catch (...) {
        return false;
    }
}

void DatagramChannel::unregister_setup_inbox(
    std::uint32_t protocol_socket_id,
    const std::shared_ptr<DatagramInbox>& inbox) noexcept
{
    std::lock_guard lock(routes_mutex_);
    const auto route =
        setup_routes_.find(protocol_socket_id);
    if (route != setup_routes_.end()
        && route->second.inbox == inbox) {
        setup_routes_.erase(route);
    }
}

bool DatagramChannel::set_listener_inbox(
    std::shared_ptr<HandshakeInbox> inbox) noexcept
{
    if (inbox == nullptr) {
        return false;
    }
    std::lock_guard lock(routes_mutex_);
    if (!listener_inbox_.expired()) {
        return false;
    }
    listener_inbox_ = std::move(inbox);
    return true;
}

void DatagramChannel::clear_listener_inbox(
    const std::shared_ptr<HandshakeInbox>& inbox) noexcept
{
    std::lock_guard lock(routes_mutex_);
    if (listener_inbox_.lock() == inbox) {
        listener_inbox_.reset();
    }
}

bool DatagramChannel::start() noexcept
{
    return start(acquire_runtime_scheduler(), next_channel_affinity());
}

bool DatagramChannel::start(std::shared_ptr<RuntimeScheduler> scheduler,
    std::uint64_t affinity) noexcept
{
    std::shared_ptr<ScheduledWorkContext> context;
    try {
        context = std::make_shared<ScheduledWorkContext>();
        context->owner = shared_from_this();
    } catch (...) {
        return false;
    }

    std::lock_guard lifecycle_lock(lifecycle_mutex_);
    if (running()) {
        return true;
    }
    if (scheduler == nullptr || !scheduler->snapshot().accepting) {
        return false;
    }
    scheduler_ = std::move(scheduler);
    scheduled_work_context_ = std::move(context);
    affinity_ = affinity;
    scheduled_timer_ = {};
    task_active_ = false;
    send_work_notification_pending_ = false;
    active_thread_ = {};
    running_.store(true, std::memory_order_release);
    if (schedule_next_locked(true, {})) {
        return true;
    }
    running_.store(false, std::memory_order_release);
    scheduler_.reset();
    scheduled_work_context_.reset();
    return false;
}

void DatagramChannel::notify_send_work() noexcept
{
    bool scheduling_failed = false;
    {
        std::lock_guard lifecycle_lock(lifecycle_mutex_);
        if (!running_.load(std::memory_order_relaxed)
            || scheduler_ == nullptr) {
            return;
        }
        if (!scheduled_timer_.valid()) {
            if (task_active_) {
                send_work_notification_pending_ = true;
            }
            return;
        }
        if (!scheduler_->cancel_timer(scheduled_timer_)) {
            // The worker already owns the continuation. Record a follow-up in
            // case it passed this connection before the enqueue completed.
            send_work_notification_pending_ = true;
            return;
        }
        scheduled_timer_ = {};
        send_work_notification_pending_ = false;
        if (!schedule_next_locked(true, {})
            && !schedule_next_locked(false, {})) {
            running_.store(false, std::memory_order_release);
            scheduling_failed = true;
        }
    }
    if (scheduling_failed) {
        mark_connections_broken(0);
    }
}

void DatagramChannel::set_idle_wait_for_testing(
    std::chrono::milliseconds timeout) noexcept
{
    std::lock_guard lifecycle_lock(lifecycle_mutex_);
    if (!running_.load(std::memory_order_relaxed)) {
        idle_wait_ = std::clamp(timeout, std::chrono::milliseconds {1},
            std::chrono::milliseconds {std::numeric_limits<int>::max()});
    }
}

void DatagramChannel::stop() noexcept
{
    std::shared_ptr<RuntimeScheduler> scheduler;
    RuntimeScheduler::TimerToken timer;
    {
        std::lock_guard lifecycle_lock(lifecycle_mutex_);
        running_.store(false, std::memory_order_release);
        scheduler = scheduler_;
        timer = scheduled_timer_;
        scheduled_timer_ = {};
    }
    if (scheduler != nullptr && timer.valid()) {
        (void)scheduler->cancel_timer(timer);
    }
    {
        std::unique_lock lifecycle_lock(lifecycle_mutex_);
        if (task_active_ && active_thread_ != std::this_thread::get_id()) {
            lifecycle_idle_.wait(lifecycle_lock, [this] {
                return !task_active_;
            });
        }
        scheduler_.reset();
        scheduled_work_context_.reset();
    }
}

bool DatagramChannel::schedule_next_locked(
    bool immediate, std::chrono::steady_clock::time_point deadline) noexcept
{
    if (!running_.load(std::memory_order_relaxed) || scheduler_ == nullptr
        || scheduled_work_context_ == nullptr) {
        return false;
    }
    RuntimeScheduler::Task task {
        .function = run_scheduled,
        .context = scheduled_work_context_,
    };
    if (immediate) {
        const RuntimeScheduler::SubmitStatus status =
            scheduler_->submit(affinity_, std::move(task));
        if (status != RuntimeScheduler::SubmitStatus::accepted) {
            return false;
        }
        scheduled_timer_ = {};
        return true;
    }
    const RuntimeScheduler::ScheduleResult scheduled =
        scheduler_->schedule_at(affinity_, deadline, std::move(task));
    if (scheduled.status != RuntimeScheduler::SubmitStatus::accepted) {
        return false;
    }
    scheduled_timer_ = scheduled.token;
    return true;
}

void DatagramChannel::run_scheduled(void* context) noexcept
{
    const auto& work = *static_cast<ScheduledWorkContext*>(context);
    const std::shared_ptr<DatagramChannel> owner = work.owner.lock();
    if (owner != nullptr) {
        owner->run_scheduled(&work);
    }
}

void DatagramChannel::run_scheduled(
    const ScheduledWorkContext* context) noexcept
{
    {
        std::lock_guard lifecycle_lock(lifecycle_mutex_);
        if (context == nullptr || scheduled_work_context_.get() != context
            || !running_.load(std::memory_order_relaxed)) {
            return;
        }
        scheduled_timer_ = {};
        task_active_ = true;
        active_thread_ = std::this_thread::get_id();
    }

    const RuntimePollResult result = run_once();
    bool scheduling_failed = false;
    {
        std::lock_guard lifecycle_lock(lifecycle_mutex_);
        task_active_ = false;
        active_thread_ = {};
        if (running_.load(std::memory_order_relaxed)
            && scheduled_work_context_.get() == context) {
            const bool send_work_notification_pending =
                send_work_notification_pending_;
            send_work_notification_pending_ = false;
            if (!schedule_next_locked(
                    result.immediate_work || send_work_notification_pending,
                    result.next_work_deadline.value_or(
                        std::chrono::steady_clock::time_point {}))) {
                running_.store(false, std::memory_order_release);
                scheduling_failed = true;
            }
        }
    }
    lifecycle_idle_.notify_all();
    if (scheduling_failed) {
        mark_connections_broken(0);
    }
}

void DatagramChannel::dispatch(const PacketView& packet,
    std::span<const std::byte> datagram, IpEndpoint peer) noexcept
{
    if (packet.kind == PacketKind::control
        && packet.control.type == ControlType::handshake) {
        const auto decoded = decode_handshake_datagram(datagram);
        if (!decoded) {
            return;
        }
        std::shared_ptr<ConnectionRuntime> replay_runtime;
        std::shared_ptr<DatagramInbox> setup_inbox;
        std::shared_ptr<HandshakeInbox> inbox;
        {
            std::lock_guard lock(routes_mutex_);
            const auto replay = handshake_routes_.find({
                .peer = peer,
                .peer_socket_id = decoded.message.packet.socket_id,
            });
            if (replay != handshake_routes_.end()) {
                replay_runtime = replay->second;
            }
            const std::uint32_t destination_socket_id =
                packet.control.destination_socket_id;
            if (destination_socket_id != 0U) {
                const auto setup =
                    setup_routes_.find(destination_socket_id);
                if (setup != setup_routes_.end() && setup->second.peer == peer
                    && (setup->second.peer_socket_id == 0U
                        || setup->second.peer_socket_id
                            == decoded.message.packet.socket_id)) {
                    setup_inbox = setup->second.inbox;
                }
            }
            bool exact_setup_ambiguous = false;
            if (setup_inbox == nullptr) {
                for (const auto& setup : setup_routes_) {
                    if (setup.second.peer != peer
                        || setup.second.peer_socket_id == 0U
                        || setup.second.peer_socket_id
                            != decoded.message.packet.socket_id) {
                        continue;
                    }
                    if (setup_inbox != nullptr) {
                        setup_inbox.reset();
                        exact_setup_ambiguous = true;
                        break;
                    }
                    setup_inbox = setup.second.inbox;
                }
            }
            if (setup_inbox == nullptr && !exact_setup_ambiguous
                && destination_socket_id == 0U) {
                const auto matching_socket =
                    setup_routes_.find(
                        decoded.message.packet.socket_id);
                if (matching_socket
                        != setup_routes_.end()
                    && matching_socket->second.peer
                        == peer) {
                    setup_inbox =
                        matching_socket->second.inbox;
                } else {
                    for (const auto& setup : setup_routes_) {
                        if (setup.second.peer != peer) {
                            continue;
                        }
                        if (setup_inbox != nullptr) {
                            setup_inbox.reset();
                            break;
                        }
                        setup_inbox =
                            setup.second.inbox;
                    }
                }
            }
            inbox = listener_inbox_.lock();
        }
        if (replay_runtime != nullptr
            && replay_runtime->process_handshake(
                decoded.message, peer)) {
            return;
        }
        if (setup_inbox != nullptr) {
            (void)setup_inbox->push(datagram, peer);
            return;
        }
        if (inbox != nullptr) {
            (void)inbox->push({
                .message = decoded.message,
                .control = decoded.control,
                .peer = peer,
            });
        }
        return;
    }

    const std::uint32_t destination_socket_id =
        packet.kind == PacketKind::data
        ? packet.data.destination_socket_id
        : packet.control.destination_socket_id;
    std::shared_ptr<ConnectionRuntime> runtime;
    std::shared_ptr<DatagramInbox> setup_inbox;
    {
        std::lock_guard lock(routes_mutex_);
        const auto route = routes_.find(
            destination_socket_id);
        if (route != routes_.end()) {
            runtime = route->second;
        } else {
            const auto setup = setup_routes_.find(
                destination_socket_id);
            if (setup != setup_routes_.end()
                && setup->second.peer == peer) {
                setup_inbox = setup->second.inbox;
            }
        }
    }
    if (runtime != nullptr) {
        runtime->process_packet(packet, peer);
    } else if (setup_inbox != nullptr) {
        (void)setup_inbox->push(datagram, peer);
    }
}

void DatagramChannel::mark_connections_broken(int system_error) noexcept
{
    std::lock_guard lock(routes_mutex_);
    for (const auto& route : routes_) {
        route.second->mark_broken(system_error);
    }
}

RuntimePollResult DatagramChannel::run_once() noexcept
{
    std::array<std::byte, 1500> datagram {};
    bool received_any = false;
    for (std::size_t index = 0; index < maximum_receive_batch; ++index) {
        const UdpIoResult received = socket.receive_from(datagram);
        if (received.error == Error::would_block) {
            break;
        }
        if (received.error == Error::buffer_too_small) {
            received_any = true;
            continue;
        }
        if (!received) {
            mark_connections_broken(received.system_error);
            break;
        }
        received_any = true;
        const auto decoded = decode_packet(
            std::span {datagram}.first(received.bytes_transferred));
        if (decoded) {
            dispatch(decoded.packet,
                std::span {datagram}.first(received.bytes_transferred),
                received.peer);
        }
    }

    bool send_work = false;
    std::optional<std::chrono::steady_clock::time_point> next_work_deadline;
    {
        std::lock_guard lock(routes_mutex_);
        for (const auto& route : routes_) {
            const RuntimePollResult result = route.second->poll();
            send_work = result.immediate_work || send_work;
            if (result.next_work_deadline.has_value()
                && (!next_work_deadline.has_value()
                    || *result.next_work_deadline < *next_work_deadline)) {
                next_work_deadline = result.next_work_deadline;
            }
        }
    }
    if (send_work || received_any) {
        return {
            .immediate_work = true,
            .next_work_deadline = std::nullopt,
        };
    }
    // Retain the pacer's absolute deadline, including sub-millisecond slices.
    // Reconstructing it from a remaining delay would add poll/scheduling time.
    // Keep the existing idle bound for inbound/control work on this channel.
    const auto idle_deadline = std::chrono::steady_clock::now() + idle_wait_;
    return {
        .next_work_deadline = next_work_deadline.has_value()
            ? std::min(*next_work_deadline, idle_deadline)
            : idle_deadline,
    };
}

ConnectionRuntime::ConnectionRuntime(Configuration configuration)
    : channel_(std::move(configuration.channel))
    , peer_(configuration.peer)
    , peer_socket_id_(configuration.peer_socket_id)
    , session_({
          .local_initial_sequence = configuration.initial_sequence,
          .peer_initial_sequence =
              configuration.has_distinct_peer_initial_sequence
              ? configuration.peer_initial_sequence
              : configuration.initial_sequence,
          .peer_socket_id = configuration.peer_socket_id,
          .send_capacity_packets = configuration.options.send_buffer_packets(),
          .receive_capacity_packets = effective_receive_capacity(configuration),
          .maximum_payload_size = effective_maximum_payload_size(configuration),
          .start_microseconds = elapsed_microseconds(configuration.origin),
      })
    , pacer_(100'000'000U, effective_peer_flow_window(configuration))
    , options_(configuration.options)
    , statistics_(configuration.handshake_arrival_microseconds,
          configuration.peer.wire_family() == IpAddressFamily::ipv6
              ? ipv6_statistics_packet_header_bytes
              : ipv4_statistics_packet_header_bytes)
    , origin_(configuration.origin)
    , origin_epoch_microseconds_(epoch_microseconds(configuration.origin))
    , peer_idle_timeout_microseconds_(
          static_cast<std::uint64_t>(
              configuration.peer_idle_timeout_milliseconds)
          * 1'000U)
    , last_peer_activity_microseconds_(
          configuration.handshake_arrival_microseconds)
    , handshake_replay_response_(configuration.handshake_replay_enabled
              ? configuration.handshake_replay_response
              : HandshakeAction {})
    , handshake_replay_peer_cookie_(configuration.handshake_replay_peer_cookie)
    , crypto_(std::move(configuration.crypto))
    , now_function_(configuration.now_function)
    , now_context_(configuration.now_context)
    , receive_pop_hook_for_testing_(configuration.receive_pop_hook_for_testing)
    , receive_pop_context_for_testing_(
          configuration.receive_pop_context_for_testing)
    , flow_window_packets_(effective_peer_flow_window(configuration))
{
    const std::size_t receive_capacity =
        effective_receive_capacity(configuration);
    if (configuration.options.congestion_controller()
        == CongestionController::file) {
        const auto maximum_bandwidth = configuration.options.get(
            SocketOption::maximum_bandwidth_bytes_per_second);
        session_.configure_file(
            configuration.options.message_api(), {
                .maximum_congestion_window_packets =
                    configuration.flow_window_packets,
                .packet_size_bytes =
                    configuration.options.maximum_segment_size(),
                .maximum_bandwidth_bytes_per_second =
                    maximum_bandwidth.value > 0
                    ? static_cast<std::uint64_t>(
                          maximum_bandwidth.value)
                    : 0U,
                .rate_control_interval_microseconds = 10'000,
                .start_microseconds = elapsed_microseconds(
                    configuration.origin),
            });
    } else {
        session_.configure_live(
            configuration.negotiated_options,
            configuration.handshake_arrival_microseconds,
            configuration.peer_handshake_timestamp,
            configuration.options
                .live_rate_configuration());
        session_.set_message_api(configuration.options.message_api());
        if (configuration.group && session_.tsbpd_clock_) {
            std::lock_guard lock(configuration.group->mutex);
            if (!configuration.group->closed) {
                session_.tsbpd_clock_->share_group_clock(
                    configuration.group->receive_clock);
            }
        }
    }
    const auto& filter = configuration.options
        .packet_filter_configuration();
    const bool row_only_fec =
        configuration.options.congestion_controller()
            == CongestionController::live
        && supports_row_only_fec(filter);
    const bool column_only_fec =
        configuration.options.congestion_controller()
            == CongestionController::live
        && supports_column_only_fec(filter);
    const bool matrix_fec =
        configuration.options.congestion_controller()
            == CongestionController::live
        && supports_matrix_fec(filter);
    const SequenceNumber peer_initial_sequence =
        configuration.has_distinct_peer_initial_sequence
        ? configuration.peer_initial_sequence
        : configuration.initial_sequence;
    const std::size_t fec_payload_size =
        effective_fec_payload_size(configuration, crypto_);
    if (row_only_fec) {
        row_fec_encoder_.emplace(
            filter, configuration.initial_sequence, fec_payload_size);
        if (filter.columns
            <= receive_capacity) {
            row_fec_decoder_.emplace(filter, peer_initial_sequence,
                receive_capacity, fec_payload_size);
        }
    } else if (column_only_fec) {
        column_fec_encoder_.emplace(
            filter, configuration.initial_sequence, fec_payload_size);
        const std::uint64_t rows =
            static_cast<std::uint64_t>(
                -static_cast<std::int64_t>(
                    filter.rows));
        const std::uint64_t matrix_size =
            static_cast<std::uint64_t>(
                filter.columns)
            * rows;
        if (matrix_size
            <= receive_capacity) {
            column_fec_decoder_.emplace(filter, peer_initial_sequence,
                receive_capacity, fec_payload_size);
        }
    } else if (matrix_fec) {
        matrix_fec_encoder_.emplace(
            filter, configuration.initial_sequence, fec_payload_size);
        const std::uint64_t matrix_size =
            static_cast<std::uint64_t>(
                filter.columns)
            * static_cast<std::uint64_t>(
                filter.rows);
        if (matrix_size
            <= receive_capacity) {
            matrix_fec_decoder_.emplace(filter, peer_initial_sequence,
                receive_capacity, fec_payload_size);
        }
    }
    session_.configure_packet_filter(
        filter, row_fec_decoder_.has_value()
            || column_fec_decoder_.has_value()
            || matrix_fec_decoder_.has_value());
    (void)session_.apply_dynamic_options(configuration.options);
    statistics_.update_reorder_state(
        session_.reorder_distance_packets(),
        session_.reorder_tolerance_packets());
}

std::uint64_t ConnectionRuntime::now_microseconds() const noexcept
{
    if (now_function_ != nullptr) {
        return now_function_(now_context_);
    }
    return elapsed_microseconds(origin_);
}

PacketTimestamp ConnectionRuntime::packet_timestamp(
    std::int64_t source_time_microseconds) const noexcept
{
    const std::int64_t relative = source_time_microseconds == 0
        ? static_cast<std::int64_t>(now_microseconds())
        : source_time_microseconds - origin_epoch_microseconds_;
    return PacketTimestamp{static_cast<std::uint32_t>(relative)};
}

void ConnectionRuntime::sample_sender_buffer_statistics(
    std::uint64_t now) noexcept
{
    const auto& sender = session_.send_buffer();
    statistics_.sample_sender_buffer(
        now, sender.size(),
        sender.buffered_payload_bytes(),
        sender.buffered_span_milliseconds());
}

void ConnectionRuntime::sample_receiver_buffer_statistics(
    std::uint64_t now) noexcept
{
    const auto& receiver = session_.receive_buffer();
    statistics_.sample_receiver_buffer(
        now, receiver.occupied(),
        receiver.buffered_payload_bytes(),
        receiver.buffered_span_milliseconds());
}

MessageIoResult ConnectionRuntime::queue_message(
    std::span<const std::byte> message,
    std::int64_t source_time_microseconds,
    bool in_order,
    bool blocking,
    std::int32_t timeout_milliseconds,
    std::int32_t ttl_milliseconds) noexcept
{
    std::unique_lock lock(mutex_);
    const bool has_deadline = timeout_milliseconds >= 0;
    const Clock::time_point deadline = has_deadline
        ? Clock::now() + std::chrono::milliseconds{timeout_milliseconds}
        : Clock::time_point{};
    for (;;) {
        if (locally_closed_) {
            return {.status = MessageIoStatus::local_closed};
        }
        if (peer_closed_) {
            return {.status = MessageIoStatus::peer_closed};
        }
        if (broken_) {
            return {
                .status = MessageIoStatus::broken,
                .system_error = system_error_,
            };
        }
        if (peer_error_pending_) {
            peer_error_pending_ = false;
            return {.status = MessageIoStatus::peer_error};
        }

        const std::uint64_t now = now_microseconds();
        statistics_.update_send_duration(
            now, session_.send_buffer().size() != 0U);
        const std::uint64_t expiration_microseconds =
            message_expiration_microseconds(
                now, source_time_microseconds,
                origin_epoch_microseconds_, ttl_milliseconds);
        const std::uint32_t message_number =
            session_.next_message_number();
        const SequenceNumber first_sequence =
            session_.send_buffer().next_sequence();
        const Error queued = session_.queue_message(message,
            packet_timestamp(source_time_microseconds), in_order,
            now, expiration_microseconds);
        if (queued == Error::none) {
            statistics_.update_send_duration(now, true);
            sample_sender_buffer_statistics(now);
            const MessageIoResult result {
                .status = MessageIoStatus::success,
                .bytes = message.size(),
                .message_number = message_number,
                .first_sequence = first_sequence,
                .next_sequence = session_.send_buffer().next_sequence(),
            };
            lock.unlock();
            notify_channel_send_work();
            ReadinessSignal::notify();
            return result;
        }
        if (queued != Error::buffer_too_small) {
            return {.status = MessageIoStatus::invalid_state};
        }
        if (!blocking) {
            return {.status = MessageIoStatus::would_block};
        }
        if (has_deadline) {
            if (Clock::now() >= deadline
                || send_ready_.wait_until(lock, deadline)
                    == std::cv_status::timeout) {
                return {.status = MessageIoStatus::timeout};
            }
        } else {
            send_ready_.wait(lock);
        }
    }
}

MessageIoResult ConnectionRuntime::queue_group_message(
    std::span<const std::byte> message,
    SequenceNumber first_sequence,
    std::uint32_t message_number,
    std::int64_t source_time_microseconds,
    bool in_order,
    std::int32_t ttl_milliseconds) noexcept
{
    std::unique_lock lock(mutex_);
    if (locally_closed_) {
        return {.status = MessageIoStatus::local_closed};
    }
    if (peer_closed_) {
        return {.status = MessageIoStatus::peer_closed};
    }
    if (broken_) {
        return {
            .status = MessageIoStatus::broken,
            .system_error = system_error_,
        };
    }
    if (peer_error_pending_) {
        peer_error_pending_ = false;
        return {.status = MessageIoStatus::peer_error};
    }

    const std::uint64_t now = now_microseconds();
    statistics_.update_send_duration(
        now, session_.send_buffer().size() != 0U);
    const std::uint64_t expiration_microseconds =
        message_expiration_microseconds(
            now, source_time_microseconds,
            origin_epoch_microseconds_, ttl_milliseconds);
    const Error queued = session_.queue_group_message(message, first_sequence,
        message_number, packet_timestamp(source_time_microseconds), in_order,
        now, expiration_microseconds);
    if (queued == Error::buffer_too_small) {
        return {.status = MessageIoStatus::would_block};
    }
    if (queued != Error::none) {
        return {.status = MessageIoStatus::invalid_state};
    }
    statistics_.update_send_duration(now, true);
    sample_sender_buffer_statistics(now);
    const MessageIoResult result {
        .status = MessageIoStatus::success,
        .bytes = message.size(),
        .message_number = message_number,
        .first_sequence = first_sequence,
        .next_sequence = session_.send_buffer().next_sequence(),
    };
    lock.unlock();
    notify_channel_send_work();
    ReadinessSignal::notify();
    return result;
}

MessageIoResult ConnectionRuntime::skip_group_sequences(
    SequenceNumber next_sequence) noexcept
{
    std::lock_guard lock(mutex_);
    if (locally_closed_) {
        return {.status = MessageIoStatus::local_closed};
    }
    if (peer_closed_) {
        return {.status = MessageIoStatus::peer_closed};
    }
    if (broken_) {
        return {
            .status = MessageIoStatus::broken,
            .system_error = system_error_,
        };
    }
    const Error skipped = session_.skip_group_sequences(next_sequence);
    if (skipped == Error::buffer_too_small) {
        return {.status = MessageIoStatus::would_block};
    }
    if (skipped != Error::none) {
        return {.status = MessageIoStatus::invalid_state};
    }
    return {
        .status = MessageIoStatus::success,
        .next_sequence = next_sequence,
    };
}

MessageIoResult ConnectionRuntime::receive_message(
    std::span<std::byte> destination,
    bool blocking,
    std::int32_t timeout_milliseconds) noexcept
{
    std::unique_lock lock(mutex_);
    const bool has_deadline = timeout_milliseconds >= 0;
    const Clock::time_point deadline = has_deadline
        ? Clock::now() + std::chrono::milliseconds{timeout_milliseconds}
        : Clock::time_point{};
    for (;;) {
        const std::uint64_t now = now_microseconds();
        if (!service_receiver_tlpktdrop_locked(now)) {
            return {
                .status = MessageIoStatus::broken,
                .system_error = system_error_,
            };
        }
        const auto received =
            session_.pop_message_at(destination, now);
        if (received) {
            if (receive_pop_hook_for_testing_ != nullptr) {
                receive_pop_hook_for_testing_(receive_pop_context_for_testing_);
            }
            session_.note_receive_buffer_released(now);
            sample_receiver_buffer_statistics(now);
            std::int64_t source_time = 0;
            const auto delivery = received.delivery_time_microseconds;
            if (delivery.has_value()
                && *delivery <= static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max())) {
                source_time = origin_epoch_microseconds_
                    + static_cast<std::int64_t>(*delivery);
            }
            ReadinessSignal::notify();
            return {
                .status = MessageIoStatus::success,
                .bytes = received.bytes_written,
                .message_number = received.message_number,
                .first_sequence = received.first_sequence,
                .next_sequence =
                    session_.receive_buffer().first_stored_sequence(),
                .source_time_microseconds = source_time,
            };
        }
        if (received.error == Error::buffer_too_small) {
            return {.status = MessageIoStatus::buffer_too_small};
        }
        if (locally_closed_) {
            return {.status = MessageIoStatus::local_closed};
        }
        // SHUTDOWN ends the peer's send side, but complete messages already
        // in the receive buffer still have to pass through the TSBPD gate.
        const bool delivery_pending =
            session_.receive_buffer().has_complete_message()
            || session_.next_receive_delivery_time().has_value();
        if (peer_closed_ && !delivery_pending) {
            return {.status = MessageIoStatus::peer_closed};
        }
        if (broken_ && !peer_closed_) {
            return {
                .status = MessageIoStatus::broken,
                .system_error = system_error_,
            };
        }
        if (received.error != Error::would_block) {
            return {.status = MessageIoStatus::invalid_state};
        }
        if (!blocking) {
            return {.status = MessageIoStatus::would_block};
        }
        if (has_deadline && Clock::now() >= deadline) {
            return {.status = MessageIoStatus::timeout};
        }
        const auto delivery_wakeup =
            next_receive_wakeup_locked(now);
        if (delivery_wakeup.has_value()) {
            const auto next_check = has_deadline
                ? std::min(deadline, *delivery_wakeup)
                : *delivery_wakeup;
            (void)receive_ready_.wait_until(lock, next_check);
        } else if (has_deadline) {
            (void)receive_ready_.wait_until(lock, deadline);
        } else {
            receive_ready_.wait(lock);
        }
    }
}

std::optional<SequenceNumber>
ConnectionRuntime::next_readable_message_sequence() noexcept
{
    std::lock_guard lock(mutex_);
    const std::uint64_t now = now_microseconds();
    if (!service_receiver_tlpktdrop_locked(now)) {
        return std::nullopt;
    }
    if (!session_.message_ready_at(now)) {
        return std::nullopt;
    }
    return session_.receive_buffer().first_stored_sequence();
}

bool ConnectionRuntime::discard_received_before(
    SequenceNumber next_sequence) noexcept
{
    std::lock_guard lock(mutex_);
    const SequenceNumber previous_sequence =
        session_.receive_buffer().first_stored_sequence();
    const Error result = session_.discard_received_before(
        next_sequence, now_microseconds());
    if (result != Error::none) {
        return false;
    }
    if (session_.receive_buffer().first_stored_sequence()
        == previous_sequence) {
        return true;
    }
    sample_receiver_buffer_statistics(now_microseconds());
    ReadinessSignal::notify();
    return true;
}

MessageIoResult ConnectionRuntime::queue_stream(
    std::span<const std::byte> bytes,
    bool blocking,
    std::int32_t timeout_milliseconds) noexcept
{
    std::unique_lock lock(mutex_);
    const bool has_deadline = timeout_milliseconds >= 0;
    const Clock::time_point deadline = has_deadline
        ? Clock::now() + std::chrono::milliseconds{timeout_milliseconds}
        : Clock::time_point{};
    for (;;) {
        if (locally_closed_) {
            return {.status = MessageIoStatus::local_closed};
        }
        if (peer_closed_) {
            return {.status = MessageIoStatus::peer_closed};
        }
        if (broken_) {
            return {
                .status = MessageIoStatus::broken,
                .system_error = system_error_,
            };
        }
        if (peer_error_pending_) {
            peer_error_pending_ = false;
            return {.status = MessageIoStatus::peer_error};
        }

        const std::uint64_t now = now_microseconds();
        statistics_.update_send_duration(
            now, session_.send_buffer().size() != 0U);
        const std::uint32_t message_number =
            session_.next_message_number();
        const auto queued = session_.queue_stream(bytes, packet_timestamp(0),
            now);
        if (queued) {
            statistics_.update_send_duration(now, true);
            sample_sender_buffer_statistics(now);
            const MessageIoResult result {
                .status = MessageIoStatus::success,
                .bytes = queued.bytes_accepted,
                .message_number = message_number,
            };
            lock.unlock();
            notify_channel_send_work();
            ReadinessSignal::notify();
            return result;
        }
        if (queued.error != Error::buffer_too_small) {
            return {.status = MessageIoStatus::invalid_state};
        }
        if (!blocking) {
            return {.status = MessageIoStatus::would_block};
        }
        if (has_deadline) {
            if (Clock::now() >= deadline
                || send_ready_.wait_until(lock, deadline)
                    == std::cv_status::timeout) {
                return {.status = MessageIoStatus::timeout};
            }
        } else {
            send_ready_.wait(lock);
        }
    }
}

MessageIoResult ConnectionRuntime::receive_stream(
    std::span<std::byte> destination,
    bool blocking,
    std::int32_t timeout_milliseconds) noexcept
{
    std::unique_lock lock(mutex_);
    const bool has_deadline = timeout_milliseconds >= 0;
    const Clock::time_point deadline = has_deadline
        ? Clock::now() + std::chrono::milliseconds{timeout_milliseconds}
        : Clock::time_point{};
    for (;;) {
        const auto received = session_.pop_stream(destination);
        if (received) {
            const std::uint64_t now = now_microseconds();
            session_.note_receive_buffer_released(now);
            sample_receiver_buffer_statistics(now);
            ReadinessSignal::notify();
            return {
                .status = MessageIoStatus::success,
                .bytes = received.bytes_written,
                .message_number = received.message_number,
                .first_sequence = received.first_sequence,
            };
        }
        if (locally_closed_) {
            return {.status = MessageIoStatus::local_closed};
        }
        if (peer_closed_) {
            return {
                .status = MessageIoStatus::success,
                .bytes = 0,
            };
        }
        if (broken_) {
            return {
                .status = MessageIoStatus::broken,
                .system_error = system_error_,
            };
        }
        if (received.error != Error::would_block) {
            return {.status = MessageIoStatus::invalid_state};
        }
        if (!blocking) {
            return {.status = MessageIoStatus::would_block};
        }
        if (has_deadline && Clock::now() >= deadline) {
            return {.status = MessageIoStatus::timeout};
        }
        if (has_deadline) {
            (void)receive_ready_.wait_until(lock, deadline);
        } else {
            receive_ready_.wait(lock);
        }
    }
}

bool ConnectionRuntime::service_receiver_tlpktdrop_locked(
    std::uint64_t now) noexcept
{
    const auto dropped =
        session_.drop_too_late_receiver(now);
    if (!dropped) {
        break_locked(0);
        return false;
    }
    if (dropped.receiver_drop_packets == 0U) {
        return true;
    }

    std::size_t average_payload =
        statistics_.average_received_payload_bytes();
    if (average_payload == 0U) {
        average_payload = options_.maximum_payload_size();
    }
    statistics_.note_receiver_drop(
        dropped.receiver_drop_packets,
        saturated_multiply(
            dropped.receiver_drop_packets,
            average_payload));
    sample_receiver_buffer_statistics(now);
    if (!send_actions(dropped.actions, now)) {
        return false;
    }
    receive_ready_.notify_all();
    ReadinessSignal::notify();
    return true;
}

std::optional<ConnectionRuntime::Clock::time_point>
ConnectionRuntime::next_receive_wakeup_locked(
    std::uint64_t now) noexcept
{
    const auto delivery =
        session_.next_receive_delivery_time();
    if (!delivery.has_value()) {
        return std::nullopt;
    }
    // The default protocol clock is defined relative to origin_. Rebuilding
    // its steady-clock deadline from a second Clock::now() sample adds any
    // preemption between the samples to the TSBPD delay. Preserve the exact
    // protocol deadline instead. Injected clocks have no steady-clock origin
    // mapping and therefore retain relative conversion.
    return now_function_ == nullptr
        ? std::optional<Clock::time_point> {deadline_from_origin_microseconds(
              origin_, *delivery)}
        : std::optional<Clock::time_point> {
              deadline_after_relative_microseconds(now, *delivery)};
}

bool ConnectionRuntime::send_actions(
    const ReliabilityActions& actions,
    std::uint64_t now) noexcept
{
    for (std::size_t index = 0; index < actions.size; ++index) {
        const auto& action = actions.values[index];
        switch (action.sender_drop_reason) {
        case SenderDropReason::message_ttl:
            statistics_.note_sender_message_ttl_drop(
                action.dropped_packets, action.dropped_bytes);
            break;
        case SenderDropReason::too_late_packet_drop:
            statistics_.note_sender_tlpktdrop(
                action.dropped_packets, action.dropped_bytes);
            break;
        case SenderDropReason::none:
            break;
        }
    }

    const auto channel = channel_.lock();
    if (channel == nullptr) {
        break_locked(0);
        return false;
    }
    for (std::size_t index = 0; index < actions.size; ++index) {
        std::array<std::byte, 1500> datagram{};
        const auto encoded = encode_reliability_action(
            actions.values[index],
            actions.loss_ranges(actions.values[index]),
            PacketTimestamp{static_cast<std::uint32_t>(now)},
            peer_socket_id_,
            datagram);
        if (!encoded) {
            break_locked(0);
            return false;
        }
        const UdpIoResult sent = channel->send_datagram(
            std::span{datagram}.first(encoded.bytes_written), peer_);
        if (!sent) {
            break_locked(sent.system_error);
            return false;
        }
        const auto type =
            control_type(actions.values[index].kind);
        if (type.has_value()) {
            statistics_.note_control_sent(*type);
        }
        session_.note_packet_sent(now);
    }
    return true;
}

bool ConnectionRuntime::send_data(
    const OutboundPacket& packet,
    std::uint64_t now) noexcept
{
    const auto channel = channel_.lock();
    if (channel == nullptr) {
        break_locked(0);
        return false;
    }
    MutablePacketView view;
    view.kind = PacketKind::data;
    view.data = packet.header;
    const bool encrypted_retransmission =
        packet.header.retransmitted
        && packet.header.encryption_key != EncryptionKey::none;
    const bool authenticated_data =
        crypto_ != nullptr && crypto_->authenticated_data_enabled();
    if (crypto_ != nullptr && crypto_->enabled()
        && !encrypted_retransmission) {
        view.data.encryption_key =
            crypto_->active_sender_key();
    }
    std::array<std::byte, 1500> datagram{};
    std::size_t datagram_size = 0U;
    if (authenticated_data && !encrypted_retransmission) {
        const auto budget =
            options_.payload_budget(peer_.wire_family(), CryptoMode::aes_gcm);
        const auto prepared = prepare_protected_payload(
            std::span {datagram}.subspan(packet_header_size),
            packet.payload.size(), budget);
        if (!prepared) {
            break_locked(0);
            return false;
        }
        // Encode the exact header before sealing because all canonical header
        // bytes except the retransmission flag are authenticated as AAD.
        const auto encoded_header = encode_packet(view, datagram);
        EncryptionKey key = EncryptionKey::none;
        if (!encoded_header
            || encoded_header.bytes_written != packet_header_size
            || crypto_->seal(view.data, packet.payload,
                   prepared.payload.ciphertext,
                   prepared.payload.authentication_tag, key)
                != Error::none
            || key != view.data.encryption_key) {
            break_locked(0);
            return false;
        }
        const auto protected_payload = std::span {datagram}.subspan(
            packet_header_size, prepared.bytes_written);
        if (session_.preserve_protected_payload(packet.header.sequence, key,
                CryptoMode::aes_gcm, protected_payload)
            != Error::none) {
            break_locked(0);
            return false;
        }
        datagram_size = packet_header_size + prepared.bytes_written;
    } else {
        view.payload = packet.payload;
        const auto encoded = encode_packet(view, datagram);
        if (!encoded) {
            break_locked(0);
            return false;
        }
        datagram_size = encoded.bytes_written;
        if (crypto_ != nullptr && crypto_->enabled()
            && !encrypted_retransmission) {
            auto payload = std::span {datagram}.subspan(
                packet_header_size, packet.payload.size());
            EncryptionKey key = EncryptionKey::none;
            if (crypto_->encrypt(packet.header.sequence, payload, payload, key)
                    != Error::none
                || key != view.data.encryption_key) {
                break_locked(0);
                return false;
            }
            if (session_.preserve_encrypted_payload(
                    packet.header.sequence, key, payload)
                != Error::none) {
                break_locked(0);
                return false;
            }
        }
    }
    if (fec_encoder_active()
        && !packet.header.retransmitted) {
        if (authenticated_data
            && packet.header.boundary != MessageBoundary::solo) {
            // The built-in FEC wire contract can reconstruct only Live/Solo
            // headers. Never emit parity for an authenticated fragmented
            // message whose AAD could not be recovered exactly.
            break_locked(0);
            return false;
        }
        const PacketView wire_packet {
            .kind = PacketKind::data,
            .data = view.data,
            .payload = std::span {datagram}.subspan(
                packet_header_size, datagram_size - packet_header_size),
        };
        if (feed_fec_source(wire_packet)
            != Error::none) {
            break_locked(0);
            return false;
        }
    }
    const UdpIoResult sent = channel->send_datagram(
        std::span {datagram}.first(datagram_size), peer_);
    if (!sent) {
        break_locked(sent.system_error);
        return false;
    }
    const std::size_t application_payload_size = authenticated_data
            && encrypted_retransmission
            && packet.payload.size() >= srt_gcm_authentication_tag_size
        ? packet.payload.size() - srt_gcm_authentication_tag_size
        : packet.payload.size();
    statistics_.note_data_sent(
        application_payload_size, packet.header.retransmitted);
    session_.note_data_packet_sent(now);
    if (crypto_ != nullptr
        && !packet.header.retransmitted
        && crypto_->note_data_packet_sent() != Error::none) {
        break_locked(0);
        return false;
    }
    return true;
}

bool ConnectionRuntime::send_filter_control(
    std::uint64_t now) noexcept
{
    if (!fec_encoder_active()) {
        return false;
    }
    const auto packet = fec_control_packet();
    if (!packet.has_value()) {
        return false;
    }
    const auto channel = channel_.lock();
    if (channel == nullptr) {
        break_locked(0);
        return false;
    }

    MutablePacketView view;
    view.kind = PacketKind::data;
    view.data = packet->header;
    if (crypto_ != nullptr && crypto_->enabled()) {
        // The recovery payload already protects ciphertext and is not
        // encrypted a second time. Haivision still stamps the active key
        // selector into the outer data header.
        view.data.encryption_key =
            crypto_->active_sender_key();
    }
    view.payload = packet->payload;
    std::array<std::byte, 1500> datagram{};
    const auto encoded = encode_packet(view, datagram);
    if (!encoded) {
        break_locked(0);
        return false;
    }
    const UdpIoResult sent = channel->send_datagram(
        std::span{datagram}.first(
            encoded.bytes_written),
        peer_);
    if (!sent) {
        break_locked(sent.system_error);
        return false;
    }
    statistics_.note_sender_filter_extra(
        packet->payload.size());
    session_.note_packet_sent(now);
    consume_fec_control();
    return true;
}

bool ConnectionRuntime::fec_encoder_active() const noexcept
{
    return row_fec_encoder_.has_value()
        || column_fec_encoder_.has_value()
        || matrix_fec_encoder_.has_value();
}

Error ConnectionRuntime::feed_fec_source(
    const PacketView& wire_packet) noexcept
{
    if (row_fec_encoder_.has_value()) {
        return row_fec_encoder_->feed_source(
            wire_packet);
    }
    if (column_fec_encoder_.has_value()) {
        return column_fec_encoder_->feed_source(
            wire_packet);
    }
    if (matrix_fec_encoder_.has_value()) {
        return matrix_fec_encoder_->feed_source(
            wire_packet);
    }
    return Error::invalid_state;
}

bool ConnectionRuntime::fec_control_ready() const noexcept
{
    if (row_fec_encoder_.has_value()) {
        return row_fec_encoder_
            ->control_packet_ready();
    }
    if (column_fec_encoder_.has_value()) {
        return column_fec_encoder_
            ->control_packet_ready();
    }
    return matrix_fec_encoder_.has_value()
        && matrix_fec_encoder_
            ->control_packet_ready();
}

std::optional<OutboundPacket>
ConnectionRuntime::fec_control_packet() const noexcept
{
    if (row_fec_encoder_.has_value()) {
        return row_fec_encoder_->control_packet();
    }
    if (column_fec_encoder_.has_value()) {
        return column_fec_encoder_
            ->control_packet();
    }
    return matrix_fec_encoder_.has_value()
        ? matrix_fec_encoder_->control_packet()
        : std::nullopt;
}

void ConnectionRuntime::consume_fec_control() noexcept
{
    if (row_fec_encoder_.has_value()) {
        row_fec_encoder_->consume_control_packet();
    } else if (column_fec_encoder_.has_value()) {
        column_fec_encoder_->consume_control_packet();
    } else if (matrix_fec_encoder_.has_value()) {
        matrix_fec_encoder_
            ->consume_control_packet();
    }
}

bool ConnectionRuntime::fec_decoder_active() const noexcept
{
    return row_fec_decoder_.has_value()
        || column_fec_decoder_.has_value()
        || matrix_fec_decoder_.has_value();
}

ConnectionRuntime::FecReceiveBatch
ConnectionRuntime::receive_fec_packet(
    const PacketView& wire_packet) noexcept
{
    if (matrix_fec_decoder_.has_value()) {
        const auto result =
            matrix_fec_decoder_->receive(
                wire_packet);
        return {
            .error = result.error,
            .consume_control_packet =
                result.consume_control_packet,
            .reconstructed_packets =
                result.reconstructed_packets,
            .irrecoverable_losses =
                result.irrecoverable_losses,
        };
    }
    RowFecReceiveResult result{
        .error = Error::invalid_state};
    if (row_fec_decoder_.has_value()) {
        result = row_fec_decoder_->receive(
            wire_packet);
    } else if (column_fec_decoder_.has_value()) {
        result = column_fec_decoder_->receive(
            wire_packet);
    }
    std::span<const PacketView> reconstructed;
    if (result.has_reconstructed_packet) {
        single_fec_reconstructed_packet_[0] =
            result.reconstructed_packet;
        reconstructed =
            single_fec_reconstructed_packet_;
    }
    return {
        .error = result.error,
        .consume_control_packet =
            result.consume_control_packet,
        .reconstructed_packets = reconstructed,
        .irrecoverable_losses =
            result.irrecoverable_losses,
    };
}

bool ConnectionRuntime::send_key_material(
    std::uint16_t subtype,
    std::span<const std::byte> key_material,
    std::uint64_t now) noexcept
{
    const auto channel = channel_.lock();
    if (channel == nullptr || key_material.empty()) {
        break_locked(0);
        return false;
    }
    MutablePacketView view;
    view.kind = PacketKind::control;
    view.control.type = ControlType::user_defined;
    view.control.subtype = subtype;
    view.control.timestamp =
        PacketTimestamp{static_cast<std::uint32_t>(now)};
    view.control.destination_socket_id = peer_socket_id_;
    view.payload = key_material;
    std::array<std::byte, 1500> datagram{};
    const auto encoded = encode_packet(view, datagram);
    if (!encoded) {
        break_locked(0);
        return false;
    }
    const UdpIoResult sent = channel->send_datagram(
        std::span{datagram}.first(encoded.bytes_written), peer_);
    if (!sent) {
        break_locked(sent.system_error);
        return false;
    }
    session_.note_packet_sent(now);
    return true;
}

bool ConnectionRuntime::send_peer_error_locked(
    std::int32_t error_code,
    std::uint64_t now) noexcept
{
    const auto channel = channel_.lock();
    if (channel == nullptr) {
        break_locked(0);
        return false;
    }

    const std::array<std::byte, sizeof(std::uint32_t)> padding{};
    MutablePacketView packet;
    packet.kind = PacketKind::control;
    packet.control.type = ControlType::peer_error;
    packet.control.type_specific =
        static_cast<std::uint32_t>(error_code);
    packet.control.timestamp =
        PacketTimestamp{static_cast<std::uint32_t>(now)};
    packet.control.destination_socket_id = peer_socket_id_;
    packet.payload = padding;

    std::array<std::byte, packet_header_size
            + sizeof(std::uint32_t)>
        datagram{};
    const auto encoded = encode_packet(packet, datagram);
    if (!encoded) {
        break_locked(0);
        return false;
    }
    const UdpIoResult sent = channel->send_datagram(
        std::span{datagram}.first(encoded.bytes_written), peer_);
    if (!sent) {
        break_locked(sent.system_error);
        return false;
    }
    session_.note_packet_sent(now);
    return true;
}

bool ConnectionRuntime::service_key_rotation(
    std::uint64_t now) noexcept
{
    if (crypto_ == nullptr || !crypto_->enabled()) {
        return true;
    }
    // A data receiver owns a CryptoSession so it can accept KMREQ, but it must
    // never manufacture a transmit key or initiate rotation for the unused
    // reverse direction.
    if (crypto_->sender_state() == CryptoState::unsecured) {
        return true;
    }
    if (crypto_->prepare_rotation() != Error::none) {
        break_locked(0);
        return false;
    }

    const auto key_material = crypto_->pending_key_material();
    if (key_material.empty()) {
        return true;
    }
    constexpr std::uint64_t retry_interval_microseconds = 100'000;
    const bool clock_moved_backwards =
        now < last_key_material_send_microseconds_;
    const bool retry_due =
        last_key_material_send_microseconds_ == 0U
        || clock_moved_backwards
        || now - last_key_material_send_microseconds_
            >= retry_interval_microseconds;
    if (!retry_due) {
        return true;
    }
    if (!send_key_material(
            key_material_request_subtype, key_material, now)) {
        return false;
    }
    last_key_material_send_microseconds_ = now;
    return true;
}

bool ConnectionRuntime::process_reliability_packet_locked(
    const PacketView& packet,
    std::uint64_t now,
    ReliabilityReceiveContext context,
    bool wire_received) noexcept
{
    const std::size_t send_size_before =
        session_.send_buffer().size();
    PacketView clear_packet = packet;
    std::array<std::byte, maximum_data_payload_size>
        clear_payload{};
    const auto filter_disposition =
        session_.packet_filter_policy().inspect(packet);
    if (filter_disposition
        == PacketFilterReceiveDisposition::
            invalid_filter_control) {
        return false;
    }
    const bool consume_filter_control =
        filter_disposition
        == PacketFilterReceiveDisposition::
            consume_filter_control;
    if (packet.kind == PacketKind::data) {
        if (packet.payload.size() > clear_payload.size()) {
            break_locked(0);
            return false;
        }
        if (consume_filter_control) {
            // FEC control payloads contain recovery bytes for ciphertext;
            // the outer KK selector is descriptive and does not request a
            // second decryption pass.
        } else if (crypto_ != nullptr && crypto_->enabled()) {
            std::span<std::byte> destination;
            Error decrypted = Error::none;
            if (crypto_->authenticated_data_enabled()) {
                const auto budget = options_.payload_budget(
                    peer_.wire_family(), CryptoMode::aes_gcm);
                const auto protected_payload =
                    decode_protected_payload(packet.payload, budget);
                if (!protected_payload) {
                    statistics_.note_receiver_undecryptable(
                        packet.payload.size());
                    // Missing or over-budget authenticated DATA is a bounded
                    // nonterminal packet drop. A later valid sequence exposes
                    // the gap to ordinary ARQ without publishing plaintext.
                    return false;
                }
                destination = std::span {clear_payload}.first(
                    protected_payload.payload.ciphertext.size());
                decrypted = crypto_->open(packet.data,
                    protected_payload.payload.ciphertext,
                    protected_payload.payload.authentication_tag, destination);
            } else {
                destination =
                    std::span {clear_payload}.first(packet.payload.size());
                decrypted = crypto_->decrypt(packet.data.encryption_key,
                    packet.data.sequence, packet.payload, destination);
            }
            if (decrypted != Error::none) {
                statistics_.note_receiver_undecryptable(
                    packet.payload.size());
                if (crypto_->authenticated_data_enabled()
                    && (decrypted == Error::authentication_failure
                        || decrypted == Error::cryptographic_failure
                        || decrypted == Error::invalid_payload_size)) {
                    // Authentication failures do not refresh peer liveness and
                    // never enter reliability state. Repetition is bounded by
                    // the normal receive path and peer-idle timeout.
                    return false;
                }
                break_locked(0);
                return false;
            }
            clear_packet.data.encryption_key =
                EncryptionKey::none;
            clear_packet.payload = destination;
        } else if (packet.data.encryption_key
            != EncryptionKey::none) {
            break_locked(0);
            return false;
        }
    }

    const SequenceNumber first_unacknowledged =
        session_.send_buffer().first_sequence();
    const auto processed = session_.receive(
        clear_packet, now, context);
    if (!processed) {
        return false;
    }
    if (processed.peer_available_receive_buffer_packets.has_value()) {
        const std::size_t previous_flow_window = flow_window_packets_;
        flow_window_packets_ = *processed.peer_available_receive_buffer_packets;
        pacer_.set_flow_window(flow_window_packets_);
        if (flow_window_packets_ > previous_flow_window) {
            send_ready_.notify_all();
        }
    } else if (clear_packet.kind == PacketKind::control
        && clear_packet.control.type == ControlType::acknowledgement) {
        // Lite ACKs advance the cumulative ACK but do not advertise newly
        // freed receive space. Consume that progress from the last window
        // budget, or removing the acknowledged flight would permit new data
        // beyond an undrained receiver's window. Rejected/stale/duplicate ACKs
        // cannot advance this validated send-buffer boundary.
        const auto progress =
            session_.send_buffer().first_sequence().distance_from(
                first_unacknowledged);
        if (progress > 0) {
            flow_window_packets_ -= std::min(
                flow_window_packets_, static_cast<std::size_t>(progress));
            pacer_.set_flow_window(flow_window_packets_);
        }
    }
    if (packet.kind == PacketKind::data) {
        sample_receiver_buffer_statistics(now);
    } else if (packet.control.type
        == ControlType::acknowledgement) {
        sample_sender_buffer_statistics(now);
    } else if (packet.control.type
        == ControlType::drop_request) {
        sample_receiver_buffer_statistics(now);
    }

    if (packet.kind == PacketKind::data) {
        if (wire_received) {
            statistics_.note_data_received(
                clear_packet.payload.size(),
                processed
                    .receiver_packet_accepted_unique,
                packet.data.retransmitted);
        }
        if (context.filter_supplied) {
            statistics_.note_receiver_filter_supply(
                clear_packet.payload.size(),
                processed
                    .receiver_packet_accepted_unique);
        }
        if (processed.receiver_filter_control_packet) {
            statistics_.note_receiver_filter_extra();
        }
        statistics_.update_reorder_state(
            session_.reorder_distance_packets(),
            session_.reorder_tolerance_packets());
        if (processed.receiver_packet_belated) {
            statistics_.note_receiver_belated(
                clear_packet.payload.size(),
                processed
                    .receiver_belated_delay_microseconds);
        }
        if (processed.receiver_loss_packets != 0U) {
            std::size_t average_payload =
                statistics_
                    .average_received_payload_bytes();
            if (average_payload == 0U) {
                average_payload =
                    clear_packet.payload.size();
            }
            statistics_.note_receiver_loss(
                processed.receiver_loss_packets,
                saturated_multiply(
                    processed.receiver_loss_packets,
                    average_payload));
        }
    } else {
        statistics_.note_control_received(
            packet.control.type);
    }
    if (processed.sender_loss_packets != 0U) {
        statistics_.note_sender_loss(
            processed.sender_loss_packets,
            processed.sender_loss_bytes);
    }
    if (processed.receiver_drop_packets != 0U) {
        std::size_t average_payload =
            statistics_.average_received_payload_bytes();
        if (average_payload == 0U) {
            average_payload =
                options_.maximum_payload_size();
        }
        statistics_.note_receiver_drop(
            processed.receiver_drop_packets,
            saturated_multiply(
                processed.receiver_drop_packets,
                average_payload));
    }
    if (!send_actions(processed.actions, now)) {
        return false;
    }
    if (packet.kind == PacketKind::data
        && !processed.receiver_filter_control_packet) {
        receive_ready_.notify_all();
    } else if (packet.control.type
        == ControlType::shutdown) {
        peer_closed_ = true;
        broken_ = true;
        receive_ready_.notify_all();
        send_ready_.notify_all();
    } else if (packet.control.type
        == ControlType::peer_error) {
        peer_error_pending_ = true;
        send_ready_.notify_all();
    }
    if (session_.send_buffer().size()
        < send_size_before) {
        send_ready_.notify_all();
    }
    statistics_.update_send_duration(
        now, session_.send_buffer().size() != 0U);
    last_readable_state_ =
        session_.data_ready_at(now);
    ReadinessSignal::notify();
    return true;
}

bool ConnectionRuntime::report_filter_losses_locked(
    std::span<const SequenceRange> losses,
    std::uint64_t now) noexcept
{
    while (!losses.empty()) {
        const std::size_t batch_size = std::min(
            losses.size(),
            maximum_loss_ranges_per_report);
        const auto processed =
            session_.report_filter_losses(
                losses.first(batch_size), now);
        if (!processed) {
            return false;
        }
        if (processed.receiver_filter_loss_packets
            != 0U) {
            statistics_.note_receiver_filter_loss(
                processed
                    .receiver_filter_loss_packets);
        }
        if (!send_actions(processed.actions, now)) {
            return false;
        }
        losses = losses.subspan(batch_size);
    }
    return true;
}

void ConnectionRuntime::process_packet(
    const PacketView& packet,
    IpEndpoint peer) noexcept
{
    if (peer != peer_) {
        return;
    }
    std::lock_guard lock(mutex_);
    if (locally_closed_ || broken_) {
        return;
    }
    if (packet.kind == PacketKind::control
        && packet.control.type == ControlType::handshake) {
        return;
    }
    if (packet.kind == PacketKind::control
        && !has_valid_control_payload_shape(packet.payload)) {
        return;
    }
    if (packet.kind == PacketKind::control
        && packet.control.type == ControlType::user_defined
        && (packet.control.subtype == key_material_request_subtype
            || packet.control.subtype == key_material_response_subtype)
        && !has_valid_runtime_key_material_payload(
            packet.control.subtype, packet.payload)) {
        return;
    }

    const std::uint64_t now = now_microseconds();
    statistics_.update_send_duration(
        now, session_.send_buffer().size() != 0U);
    if (packet.kind == PacketKind::control
        && packet.control.type == ControlType::user_defined
        && (packet.control.subtype == key_material_request_subtype
            || packet.control.subtype
                == key_material_response_subtype)) {
        if (crypto_ == nullptr || !crypto_->enabled()) {
            if (packet.control.subtype
                    == key_material_request_subtype
                && !options_.enforced_encryption()) {
                const auto response = crypto_state_payload(
                    CryptoState::no_secret);
                if (!send_key_material(
                        key_material_response_subtype,
                        response, now)) {
                    break_locked(0);
                    return;
                }
                last_peer_activity_microseconds_ = now;
                return;
            }
            break_locked(0);
            return;
        }
        if (packet.control.subtype
            == key_material_request_subtype) {
            const CryptoState previous_state =
                crypto_->receiver_state();
            const Error accepted = crypto_->accept_key_material(
                packet.payload, false);
            if (accepted != Error::none) {
                // Runtime KM control packets are not authenticated. A forged
                // request must neither downgrade a secured receiver nor tear
                // down its otherwise valid encrypted session. A legitimate
                // peer will retry a request that was damaged in transit.
                if ((previous_state == CryptoState::secured
                        || previous_state
                            == CryptoState::securing)
                    && crypto_->receiver_state()
                        == previous_state) {
                    return;
                }
                break_locked(0);
                return;
            }
            if (!send_key_material(
                    key_material_response_subtype,
                    crypto_->key_material_response(), now)) {
                break_locked(0);
                return;
            }
        } else {
            const CryptoState previous_state =
                crypto_->sender_state();
            const Error acknowledged =
                crypto_->acknowledge_key_material(
                    packet.payload, false);
            if (acknowledged != Error::none) {
                if (previous_state == CryptoState::securing
                    && crypto_->sender_state() != previous_state
                    && !options_.enforced_encryption()) {
                    // Only the initial, explicitly optional exchange may
                    // downgrade. During rotation acknowledge_key_material()
                    // preserves SECURING, so unauthenticated stale/failure
                    // responses cannot remove established keys.
                    crypto_.reset();
                    last_peer_activity_microseconds_ = now;
                    ReadinessSignal::notify();
                    return;
                }
                // Apply the same fail-closed rule to an unauthenticated or
                // stale KMRSP. The outstanding request remains eligible for
                // retry; only an exact response can advance rotation.
                if ((previous_state == CryptoState::secured
                        || previous_state
                            == CryptoState::securing)
                    && crypto_->sender_state()
                        == previous_state) {
                    return;
                }
                break_locked(0);
                return;
            }
            // The retry timestamp belongs to the request that was just
            // acknowledged, not to the next rotation. Keeping it would
            // delay a new KMREQ until the 100 ms retransmission interval
            // elapsed, even though no request for that rotation had been
            // sent. Aggressive but valid refresh rates would then stall
            // Live source pacing and eventually miss TSBPD deadlines.
            last_key_material_send_microseconds_ = 0U;
        }
        last_peer_activity_microseconds_ = now;
        ReadinessSignal::notify();
        return;
    }

    if (packet.kind == PacketKind::data
        && fec_decoder_active()) {
        const auto filtered =
            receive_fec_packet(packet);
        if (!filtered) {
            return;
        }
        last_peer_activity_microseconds_ = now;
        const auto process_reconstructed =
            [&](std::span<const PacketView> packets)
                noexcept {
                for (std::size_t index = 0;
                     index < packets.size(); ++index) {
                    if (!process_reliability_packet_locked(
                            packets[index], now,
                            {
                                .filter_supplied = true,
                                .defer_feedback =
                                    index + 1U
                                        < packets.size(),
                            },
                            false)) {
                        return false;
                    }
                }
                return true;
            };
        if (filtered.consume_control_packet) {
            statistics_.note_data_received(
                packet.payload.size(), false,
                packet.data.retransmitted);
            statistics_.note_receiver_filter_extra();
            if (filtered.reconstructed_packets.empty()) {
                statistics_.update_send_duration(
                    now,
                    session_.send_buffer().size() != 0U);
                last_readable_state_ =
                    session_.data_ready_at(now);
                ReadinessSignal::notify();
            } else if (!process_reconstructed(
                           filtered
                               .reconstructed_packets)) {
                return;
            }
            (void)report_filter_losses_locked(
                filtered.irrecoverable_losses, now);
            return;
        }
        if (!filtered.reconstructed_packets.empty()) {
            if (!process_reliability_packet_locked(
                    packet, now,
                    {.defer_feedback = true})) {
                return;
            }
            if (!process_reconstructed(
                    filtered.reconstructed_packets)) {
                return;
            }
            (void)report_filter_losses_locked(
                filtered.irrecoverable_losses, now);
            return;
        }
        if (!process_reliability_packet_locked(
                packet, now)) {
            return;
        }
        (void)report_filter_losses_locked(
            filtered.irrecoverable_losses, now);
        return;
    }
    if (process_reliability_packet_locked(packet, now)) {
        last_peer_activity_microseconds_ = now;
    }
}

bool ConnectionRuntime::process_handshake(
    const HandshakeMessage& message,
    IpEndpoint peer) noexcept
{
    std::lock_guard lock(mutex_);
    if (peer != peer_
        || locally_closed_
        || broken_
        || handshake_replay_response_.kind
            != HandshakeActionKind::send
        || message.packet.request
            != HandshakeRequest::conclusion
        || message.packet.socket_id != peer_socket_id_
        || message.packet.syn_cookie
            != (handshake_replay_peer_cookie_ != 0U
                    ? handshake_replay_peer_cookie_
                    : handshake_replay_response_
                          .packet.syn_cookie)) {
        return false;
    }

    const std::uint64_t now = now_microseconds();
    statistics_.update_send_duration(
        now, session_.send_buffer().size() != 0U);
    last_peer_activity_microseconds_ = now;
    const auto channel = channel_.lock();
    if (channel == nullptr) {
        break_locked(0);
        return true;
    }
    std::array<std::byte, 1500> datagram{};
    const auto encoded = encode_handshake_datagram(
        handshake_replay_response_,
        PacketTimestamp{static_cast<std::uint32_t>(now)},
        peer_socket_id_,
        datagram);
    if (!encoded) {
        break_locked(0);
        return true;
    }
    const auto sent = channel->send_datagram(
        std::span{datagram}.first(encoded.bytes_written), peer_);
    if (!sent) {
        break_locked(sent.system_error);
        return true;
    }
    session_.note_packet_sent(now);
    ReadinessSignal::notify();
    return true;
}

RuntimePollResult ConnectionRuntime::poll() noexcept
{
    std::lock_guard lock(mutex_);
    if (locally_closed_ || peer_closed_ || broken_) {
        return {};
    }
    const std::uint64_t now = now_microseconds();
    if (now > last_peer_activity_microseconds_
        && now - last_peer_activity_microseconds_
            > peer_idle_timeout_microseconds_) {
        break_locked(0);
        return {};
    }
    if (!service_key_rotation(now)) {
        return {};
    }
    if (!service_receiver_tlpktdrop_locked(now)) {
        return {};
    }
    const bool readable_now = session_.data_ready_at(now);
    if (readable_now != last_readable_state_) {
        last_readable_state_ = readable_now;
        ReadinessSignal::notify();
    }
    const std::size_t send_size_before_drop =
        session_.send_buffer().size();
    if (!send_actions(
            session_.drop_expired_sender_message(now), now)
        || !send_actions(session_.drop_too_late_sender(now), now)
        || !send_actions(session_.poll_timers(now), now)) {
        return {};
    }
    std::size_t pending_drop_requests_sent = 0;
    while (pending_drop_requests_sent < maximum_send_batch
        && session_.has_pending_drop_requests()) {
        const auto pending = session_.take_pending_drop_requests();
        if (pending.size == 0U) {
            break;
        }
        if (!send_actions(pending, now)) {
            return {};
        }
        pending_drop_requests_sent += pending.size;
    }
    if (session_.send_buffer().size() < send_size_before_drop) {
        sample_sender_buffer_statistics(now);
        send_ready_.notify_all();
        ReadinessSignal::notify();
    }
    statistics_.update_send_duration(
        now, session_.send_buffer().size() != 0U);
    sample_receiver_buffer_statistics(now);
    (void)session_.poll_sender_retransmission_timeout(now);

    for (std::size_t index = 0; index < maximum_send_batch; ++index) {
        const std::uint64_t packet_time = now_microseconds();
        if (crypto_ != nullptr && crypto_->enabled()) {
            if (!service_key_rotation(packet_time)) {
                return {};
            }
        }
        if (fec_control_ready()
            && !session_.has_pending_retransmission()) {
            const std::uint64_t live_rate =
                session_
                    .live_pacing_rate_bytes_per_second();
            const std::uint64_t file_rate =
                session_
                    .file_pacing_rate_bytes_per_second();
            if (live_rate != 0U) {
                pacer_.set_rate(live_rate);
            } else if (file_rate != 0U) {
                pacer_.set_rate(file_rate);
                pacer_.set_flow_window(
                    session_
                        .file_congestion_window_packets());
            }
            if (!pacer_.query(packet_time, 0U).ready) {
                break;
            }
            const auto control =
                fec_control_packet();
            if (!control.has_value()) {
                break_locked(0);
                return {};
            }
            const std::size_t wire_size =
                packet_header_size
                + control->payload.size();
            if (!send_filter_control(packet_time)) {
                return {};
            }
            pacer_.on_packet_sent(
                wire_size, packet_time);
            continue;
        }
        // New data needs the next key acknowledgement. A retransmission
        // already owns its original ciphertext and does not depend on it.
        if (crypto_ != nullptr && crypto_->enabled()
            && !crypto_->ready_to_send_data()
            && !session_.has_pending_retransmission()) {
            break;
        }
        const bool retransmission = session_.has_pending_retransmission();
        if (!retransmission
            && session_.send_buffer().packets_in_flight()
                >= flow_window_packets_) {
            break;
        }
        const std::size_t new_packet_wire_overhead =
            crypto_ != nullptr && crypto_->authenticated_data_enabled()
            ? srt_gcm_authentication_tag_size
            : 0U;
        const auto packet = session_.next_paced_data_packet(
            pacer_, packet_time, new_packet_wire_overhead);
        if (!packet.has_value()) {
            break;
        }
        if (!send_data(*packet, packet_time)) {
            return {};
        }
    }

    const bool retransmission =
        session_.has_pending_retransmission();
    const bool pending = session_.has_pending_send_work();
    const bool flow_blocked = !retransmission
        && session_.send_buffer().packets_in_flight()
            >= flow_window_packets_;
    const bool filter_pending =
        fec_control_ready();
    if (session_.has_pending_drop_requests()) {
        return {
            .immediate_work = true,
            .next_work_deadline = std::nullopt,
        };
    }

    // A retransmission already owns its immutable wire ciphertext. New DATA,
    // however, must wait for the outstanding KMRSP. Treating that wait as
    // runnable work makes the channel spin and can starve the peer that must
    // produce the response, especially when another group member is behind a
    // black hole. Flow-window waits have the same inbound-event dependency.
    const bool crypto_blocked = !retransmission && crypto_ != nullptr
        && crypto_->enabled() && !crypto_->ready_to_send_data();
    const bool paced_work =
        filter_pending || (pending && !flow_blocked && !crypto_blocked);
    if (!paced_work) {
        return {};
    }

    const std::uint64_t current = now_microseconds();
    const PaceDecision pace = pacer_.query(current,
        filter_pending || retransmission
            ? 0U
            : session_.send_buffer().packets_in_flight());
    if (pace.ready || pace.next_ready_microseconds <= current) {
        return {
            .immediate_work = true,
            .next_work_deadline = std::nullopt,
        };
    }
    return {
        .next_work_deadline = deadline_from_origin_microseconds(
            origin_, pace.next_ready_microseconds),
    };
}

void ConnectionRuntime::apply_options(
    const SocketOptions& options) noexcept
{
    std::lock_guard lock(mutex_);
    options_ = options;
    (void)session_.apply_dynamic_options(options_);
    statistics_.update_reorder_state(
        session_.reorder_distance_packets(),
        session_.reorder_tolerance_packets());
}

void ConnectionRuntime::break_locked(int system_error) noexcept
{
    const std::uint64_t now = now_microseconds();
    statistics_.update_send_duration(
        now, session_.send_buffer().size() != 0U);
    statistics_.update_send_duration(now, false);
    broken_ = true;
    system_error_ = system_error;
    receive_ready_.notify_all();
    send_ready_.notify_all();
    ReadinessSignal::notify();
}

void ConnectionRuntime::notify_channel_send_work() noexcept
{
    if (const auto channel = channel_.lock(); channel != nullptr) {
        channel->notify_send_work();
    }
}

void ConnectionRuntime::mark_broken(int system_error) noexcept
{
    std::lock_guard lock(mutex_);
    break_locked(system_error);
}

bool ConnectionRuntime::report_peer_error(
    std::int32_t error_code) noexcept
{
    std::lock_guard lock(mutex_);
    if (locally_closed_ || peer_closed_ || broken_) {
        return false;
    }
    return send_peer_error_locked(
        error_code, now_microseconds());
}

void ConnectionRuntime::close() noexcept
{
    std::lock_guard lock(mutex_);
    if (locally_closed_) {
        return;
    }
    const std::uint64_t now = now_microseconds();
    if (!peer_closed_ && !broken_) {
        ReliabilityActions shutdown;
        shutdown.push({.kind = ReliabilityActionKind::shutdown});
        (void)send_actions(shutdown, now);
    }
    statistics_.update_send_duration(
        now, session_.send_buffer().size() != 0U);
    statistics_.update_send_duration(now, false);
    locally_closed_ = true;
    receive_ready_.notify_all();
    send_ready_.notify_all();
    ReadinessSignal::notify();
}

bool ConnectionRuntime::broken() const noexcept
{
    std::lock_guard lock(mutex_);
    return broken_;
}

bool ConnectionRuntime::peer_closed() const noexcept
{
    std::lock_guard lock(mutex_);
    return peer_closed_;
}

bool ConnectionRuntime::readable() noexcept
{
    std::lock_guard lock(mutex_);
    if (locally_closed_) {
        return false;
    }
    const std::uint64_t now = now_microseconds();
    if (!service_receiver_tlpktdrop_locked(now)) {
        return true;
    }
    if (session_.data_ready_at(now)) {
        return true;
    }
    // Report end-of-stream only after no complete TSBPD-delayed message
    // remains ahead of it.
    return peer_closed_
        && !session_.receive_buffer().has_complete_message();
}

std::optional<ConnectionRuntime::Clock::time_point>
ConnectionRuntime::next_readable_deadline() noexcept
{
    std::lock_guard lock(mutex_);
    if (locally_closed_) {
        return std::nullopt;
    }
    const std::uint64_t now = now_microseconds();
    if (session_.data_ready_at(now)) {
        return Clock::now();
    }
    return next_receive_wakeup_locked(now);
}

bool ConnectionRuntime::writable() const noexcept
{
    std::lock_guard lock(mutex_);
    return !locally_closed_ && !peer_closed_ && !broken_
        && (peer_error_pending_
            || session_.send_buffer().available() != 0U);
}

bool ConnectionRuntime::wait_for_send_drain(
    std::chrono::milliseconds timeout) noexcept
{
    std::unique_lock lock(mutex_);
    const auto complete = [this] {
        return session_.send_buffer().size() == 0U
            || locally_closed_ || peer_closed_ || broken_;
    };
    if (!complete() && timeout.count() > 0) {
        (void)send_ready_.wait_for(lock, timeout, complete);
    }
    return session_.send_buffer().size() == 0U;
}

CryptoState ConnectionRuntime::sender_crypto_state() const noexcept
{
    std::lock_guard lock(mutex_);
    return crypto_ == nullptr
        ? CryptoState::unsecured
        : crypto_->sender_state();
}

CryptoState ConnectionRuntime::receiver_crypto_state() const noexcept
{
    std::lock_guard lock(mutex_);
    return crypto_ == nullptr
        ? CryptoState::unsecured
        : crypto_->receiver_state();
}

std::size_t ConnectionRuntime::crypto_key_length() const noexcept
{
    std::lock_guard lock(mutex_);
    if (crypto_ == nullptr || !crypto_->enabled()) {
        return 0U;
    }
    return crypto_->key_length();
}

#ifdef ENABLE_AEAD_API_PREVIEW
CryptoMode ConnectionRuntime::crypto_mode() const noexcept
{
    std::lock_guard lock(mutex_);
    if (crypto_ == nullptr || !crypto_->enabled()) {
        return static_cast<CryptoMode>(
            options_.get(SocketOption::crypto_mode).value);
    }
    return crypto_->effective_mode();
}
#endif

RuntimeStatisticsSnapshot ConnectionRuntime::statistics(
    bool clear_interval, bool instantaneous) noexcept
{
    std::lock_guard lock(mutex_);
    const std::uint64_t now = now_microseconds();
    statistics_.update_send_duration(
        now, session_.send_buffer().size() != 0U);
    sample_sender_buffer_statistics(now);
    sample_receiver_buffer_statistics(now);
    statistics_.update_reorder_state(
        session_.reorder_distance_packets(),
        session_.reorder_tolerance_packets());

    const auto& send_buffer = session_.send_buffer();
    const auto& receive_buffer = session_.receive_buffer();
    const std::size_t sender_packets = send_buffer.size();
    const std::size_t receiver_packets = receive_buffer.occupied();
    const std::size_t maximum_segment =
        options_.maximum_segment_size();
    const std::size_t packet_header_bytes =
        peer_.wire_family() == IpAddressFamily::ipv6
        ? ipv6_statistics_packet_header_bytes
        : ipv4_statistics_packet_header_bytes;
    const auto rates = session_.arrival_rates();
    const double bandwidth = rates.valid
        ? static_cast<double>(rates.bytes_per_second) * 8.0
            / 1'000'000.0
        : 0.0;
    const std::uint64_t pacing_rate =
        options_.congestion_controller()
            == CongestionController::file
        ? session_.file_pacing_rate_bytes_per_second()
        : session_.live_pacing_rate_bytes_per_second();
    const std::size_t file_congestion_window =
        session_.file_congestion_window_packets();
    const auto& live_options = session_.live_options();

    RuntimeStatisticsInstantaneous values{
        .packet_send_period_microseconds =
            session_.packet_sending_period_microseconds(),
        .flow_window_packets = flow_window_packets_,
        .congestion_window_packets =
            file_congestion_window != 0U
            ? file_congestion_window : flow_window_packets_,
        .packets_in_flight =
            send_buffer.packets_in_flight(),
        .round_trip_time_milliseconds =
            static_cast<double>(
                session_.rtt().smoothed_microseconds())
            / 1'000.0,
        .bandwidth_megabits_per_second = bandwidth,
        .maximum_bandwidth_megabits_per_second =
            static_cast<double>(pacing_rate) * 8.0
            / 1'000'000.0,
        .maximum_segment_size_bytes = maximum_segment,
        .sender_buffer_packets = sender_packets,
        .sender_buffer_bytes = saturated_add(
            send_buffer.buffered_payload_bytes(),
            saturated_multiply(sender_packets,
                packet_header_bytes)),
        .sender_buffer_milliseconds =
            send_buffer.buffered_span_milliseconds(),
        .sender_buffer_available_bytes =
            saturated_multiply(
                send_buffer.capacity() - sender_packets,
                maximum_segment),
        .sender_tsbpd_delay_milliseconds =
            live_options.peer_receive_delay_milliseconds,
        .receiver_buffer_packets = receiver_packets,
        .receiver_buffer_bytes =
            receive_buffer.buffered_payload_bytes(),
        .receiver_buffer_milliseconds =
            receive_buffer.buffered_span_milliseconds(),
        .receiver_buffer_available_bytes =
            saturated_multiply(
                receive_buffer.available(), maximum_segment),
        .receiver_tsbpd_delay_milliseconds =
            live_options.receive_delay_milliseconds,
    };
    if (!instantaneous) {
        statistics_.apply_average_buffers(values);
        values.sender_buffer_available_bytes =
            saturated_multiply(
                send_buffer.capacity()
                    - std::min(send_buffer.capacity(),
                        values.sender_buffer_packets),
                maximum_segment);
    }
    return statistics_.snapshot(
        now, values, clear_interval);
}

RuntimeResponseHealth ConnectionRuntime::response_health() const noexcept
{
    std::lock_guard lock(mutex_);
    const auto& live_options = session_.live_options();
    return {
        .now_microseconds = now_microseconds(),
        .last_response_microseconds = last_peer_activity_microseconds_,
        .peer_idle_timeout_microseconds = peer_idle_timeout_microseconds_,
        .smoothed_rtt_microseconds =
            session_.rtt().smoothed_microseconds(),
        .rtt_variation_microseconds =
            session_.rtt().variation_microseconds(),
        .peer_latency_microseconds =
            static_cast<std::uint32_t>(
                live_options.peer_receive_delay_milliseconds)
            * 1'000U,
        .acknowledged_sequence =
            session_.send_buffer().first_sequence(),
        .next_send_sequence =
            session_.send_buffer().next_sequence(),
        .send_buffer_empty =
            session_.send_buffer().sequence_span() == 0U,
    };
}

SenderBufferStatus ConnectionRuntime::sender_buffer_status() noexcept
{
    std::lock_guard lock(mutex_);
    const auto& send_buffer = session_.send_buffer();
    return {
        .packets = send_buffer.size(),
        .bytes = send_buffer.buffered_payload_bytes(),
        .milliseconds =
            send_buffer.buffered_span_milliseconds(),
    };
}

RuntimeBufferPacketCounts
ConnectionRuntime::buffer_packet_counts() const noexcept
{
    std::lock_guard lock(mutex_);
    return {
        .unacknowledged_send = session_.send_buffer().size(),
        .available_receive = session_.receive_buffer().occupied(),
    };
}

std::optional<HandshakeRouteKey>
ConnectionRuntime::handshake_replay_key() const noexcept
{
    std::lock_guard lock(mutex_);
    if (handshake_replay_response_.kind
        != HandshakeActionKind::send) {
        return std::nullopt;
    }
    return HandshakeRouteKey{
        .peer = peer_,
        .peer_socket_id = peer_socket_id_,
    };
}

} // namespace robotweax::srt::compat
