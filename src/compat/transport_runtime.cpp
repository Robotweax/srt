#include "compat/group_receive_retention.hpp"
#include "compat/transport_runtime.hpp"
#include "compat/connection_datagram_dispatcher.hpp"
#include "compat/submillisecond_pacing_platform.hpp"

#include "robotweax/srt/codec.hpp"
#include "compat/readiness.hpp"
#include "compat/group_registry.hpp"
#include "compat/runtime_scheduler_service.hpp"
#include "compat/process_state.hpp"
#include "compat/runtime_work_executor_service.hpp"
#include "sender_drop_trace.hpp"

#include <algorithm>
#include <atomic>
#include <limits>
#include <new>
#include <thread>

namespace robotweax::srt::compat {
namespace {

[[nodiscard]] std::uint64_t next_channel_affinity() noexcept
{
    // Aligned object addresses would collapse onto one shard under modulo
    // placement. A process-local sequence distributes channel ownership.
    static std::atomic<std::uint64_t> next_affinity {0U};
    return next_affinity.fetch_add(1U, std::memory_order_relaxed);
}

constexpr std::size_t maximum_send_batch = 64;
constexpr std::size_t maximum_connection_polls = 64;
constexpr std::uint64_t maximum_transient_send_retry_microseconds = 5'000'000U;

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
    return decode_key_material_state(payload).has_value();
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
    // Queued handshake input changes no handle's readiness; only blocked
    // accept/connect loops re-check their state.
    ReadinessSignal::notify_waiters();
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
    ReadinessSignal::notify_waiters();
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
        ReadinessSignal::notify_waiters();
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
        std::unique_lock lock(mutex_);
        if (admission_retired_) {
            return false;
        }
        if (promoted_runtime_ != nullptr) {
            if (peer != promoted_peer_) {
                return false;
            }
            if (queued_promotion_) {
                const auto dispatcher = promoted_dispatcher_.lock();
                lock.unlock();
                return dispatcher != nullptr
                    && dispatcher->publish(
                           dispatcher->inbox()->token(), datagram, peer)
                    == ConnectionDatagramInbox::Status::accepted;
            }
            lock.unlock();
            finish_promotion();
            deliver_promoted(datagram, peer);
            return true;
        }
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
    ReadinessSignal::notify_waiters();
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
    if (promoted_runtime_ != nullptr || size_ == 0U) {
        return InboxPopStatus::closed;
    }
    envelope = entries_[head_];
    head_ = (head_ + 1U) % entries_.size();
    --size_;
    lock.unlock();
    ReadinessSignal::notify_waiters();
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
    close_publication(false);
}

void DatagramInbox::retire() noexcept
{
    close_publication(true);
}

void DatagramInbox::close_publication(bool terminal) noexcept
{
    ReadyFunction handler = nullptr;
    std::shared_ptr<void> handler_context;
    {
        std::lock_guard lock(mutex_);
        admission_retired_ |= terminal;
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

DatagramChannel::DatagramChannel(UnopenedTag)
    : socket(UdpSocket::UnopenedTag {})
{
}

DatagramChannel::~DatagramChannel()
{
    stop();
}

void DatagramChannel::release_native_credit() noexcept
{
    native_credit_.reset();
}

std::shared_ptr<DatagramChannel> DatagramChannel::create_budgeted(
    IpAddressFamily family,
    const std::shared_ptr<NativeChannelBudget>& budget) noexcept
{
    if (budget == nullptr || !budget->reserve())
        return {};
    bool reservation_owned = true;
    try {
        auto channel =
            std::shared_ptr<DatagramChannel>(new DatagramChannel(family));
        channel->native_credit_.budget = budget;
        reservation_owned = false;
        if (!channel->socket.valid())
            return {};
        return channel;
    } catch (...) {
        if (reservation_owned)
            budget->release();
        return {};
    }
}

std::shared_ptr<DatagramChannel> DatagramChannel::adopt_budgeted(
    UdpSocket& acquired_socket,
    const std::shared_ptr<NativeChannelBudget>& budget) noexcept
{
    if (!acquired_socket.valid() || budget == nullptr || !budget->reserve())
        return {};
    bool reservation_owned = true;
    try {
        auto channel = std::shared_ptr<DatagramChannel>(
            new DatagramChannel(UnopenedTag {}));
        // No descriptor was opened or transferred before the object and its
        // shared control block exist. The following assignments cannot throw.
        channel->native_credit_.budget = budget;
        reservation_owned = false;
        channel->socket = std::move(acquired_socket);
        return channel;
    } catch (...) {
        if (reservation_owned)
            budget->release();
        return {};
    }
}

std::shared_ptr<ConnectionDatagramDispatcher>
DatagramChannel::create_budgeted_dispatcher(
    const std::shared_ptr<ConnectionRuntime>& runtime,
    const std::shared_ptr<RuntimeScheduler>& scheduler, std::uint64_t affinity,
    ConnectionDatagramInbox::Configuration inbox_configuration,
    ConnectionDatagramDispatcher::Configuration configuration,
    const std::shared_ptr<DatagramInbox>& setup_prefix) noexcept
{
    // Process acquisition rejects inherited state before any channel lock.
    auto process = acquire_runtime_inbox_storage_budget();
    if (process == nullptr || runtime == nullptr
        || runtime->channel_.lock().get() != this
        || shutdown_requested_.load(std::memory_order_acquire))
        return nullptr;
    std::shared_ptr<DatagramStorageBudget> local;
    try {
        std::lock_guard lock(inbox_budget_mutex_);
        if (inbox_budget_ == nullptr) {
            local = std::make_shared<DatagramStorageBudget>(
                maximum_inbox_storage_bytes, maximum_inbox_count);
            process_inbox_budget_ = std::move(process);
            inbox_budget_ = local;
        } else {
            local = inbox_budget_;
        }
        process = process_inbox_budget_;
    } catch (...) {
        return nullptr;
    }
    // No budget lock is held while prefix promotion can notify callbacks.
    // A concurrent terminal shutdown is fenced again at route registration;
    // a caller holding an unregistered dispatcher owns its retirement.
    return ConnectionDatagramDispatcher::create(runtime, scheduler, local,
        affinity, runtime->peer_, inbox_configuration, std::move(configuration),
        setup_prefix, process);
}

std::shared_ptr<ChannelPollSendBudget>
DatagramChannel::begin_poll_round() noexcept
{
    if (!stateful_process_available()
        || shutdown_requested_.load(std::memory_order_acquire))
        return nullptr;
    auto owner = weak_from_this();
    if (owner.expired())
        return nullptr;
    try {
        std::lock_guard lock(poll_budget_mutex_);
        if (shutdown_requested_.load(std::memory_order_acquire)
            || !poll_budget_.expired())
            return nullptr;
        auto budget = std::shared_ptr<ChannelPollSendBudget>(
            new ChannelPollSendBudget(std::move(owner)));
        poll_budget_ = budget;
        return budget;
    } catch (...) {
        return nullptr;
    }
}

UdpIoResult DatagramChannel::send_datagram(
    std::span<const std::byte> bytes,
    IpEndpoint peer) noexcept
{
    std::lock_guard lock(send_mutex_);
    if (send_closed_) {
        return {.error = Error::io_error};
    }
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

bool DatagramChannel::register_connection(std::uint32_t protocol_socket_id,
    std::shared_ptr<ConnectionRuntime> runtime,
    std::shared_ptr<ConnectionDatagramDispatcher> dispatcher) noexcept
{
    if (protocol_socket_id == 0U || runtime == nullptr
        || (dispatcher != nullptr && !dispatcher->targets(runtime, this))) {
        return false;
    }
    const auto replay_key = runtime->handshake_replay_key();
    std::shared_ptr<ConnectionRuntime> removed_runtime;
    std::shared_ptr<ConnectionDatagramDispatcher> removed_dispatcher;
    std::shared_ptr<DatagramInbox> removed_prefix;
    try {
        std::unique_lock lock(routes_mutex_);
        if (((dispatcher != nullptr || queued_routes_ != 0U)
                && std::any_of(routes_.begin(), routes_.end(),
                    [&](const auto& route) {
                        return route.second.runtime == runtime
                            && (dispatcher != nullptr
                                || route.second.dispatcher != nullptr);
                    }))
            || !register_connection_locked(
                protocol_socket_id, runtime, replay_key)) {
            return false;
        }
        bool claimed = false;
        bool activated = true;
        if (dispatcher != nullptr) {
            claimed = dispatcher->claim_route(nullptr);
            if (claimed) {
                routes_.find(protocol_socket_id)->second.dispatcher =
                    dispatcher;
                ++queued_routes_;
                activated = dispatcher->activate_route();
            }
            if (!claimed || !activated) {
                unregister_connection_locked(protocol_socket_id,
                    removed_runtime, removed_dispatcher, removed_prefix);
                lock.unlock();
                if (claimed) {
                    dispatcher->retire();
                    runtime->mark_broken(0);
                }
                return false;
            }
        }
        lock.unlock();
        notify_send_work();
        return true;
    } catch (...) {
        return false;
    }
}

bool DatagramChannel::register_connection_locked(
    std::uint32_t protocol_socket_id,
    const std::shared_ptr<ConnectionRuntime>& runtime,
    const std::optional<HandshakeRouteKey>& replay_key)
{
    if (channel_faulted_
        || shutdown_requested_.load(std::memory_order_acquire)) {
        return false;
    }
    const auto inserted = routes_.emplace(protocol_socket_id,
        ConnectionRoute {.runtime = runtime, .replay_key = replay_key});
    if (!inserted.second) {
        return false;
    }
    if (replay_key.has_value()) {
        try {
            if (!handshake_routes_.emplace(*replay_key, &inserted.first->second)
                    .second) {
                routes_.erase(inserted.first);
                return false;
            }
        } catch (...) {
            routes_.erase(inserted.first);
            throw;
        }
    }
    auto& route = inserted.first->second;
    if (next_poll_route_ == nullptr) {
        route.previous = route.next = &route;
        next_poll_route_ = &route;
    } else {
        // Append behind the routes already waiting for their turn.
        route.previous = next_poll_route_->previous;
        route.next = next_poll_route_;
        route.previous->next = &route;
        route.next->previous = &route;
    }
    return true;
}

void DatagramInbox::deliver_promoted(
    std::span<const std::byte> datagram, IpEndpoint peer) noexcept
{
    if (peer != promoted_peer_) {
        return;
    }
    const auto decoded = decode_packet(datagram);
    if (!decoded) {
        return;
    }
    if (decoded.packet.kind == PacketKind::control
        && decoded.packet.control.type == ControlType::handshake) {
        const auto handshake = decode_handshake_datagram(datagram);
        if (handshake) {
            (void)promoted_runtime_->process_handshake(handshake.message, peer);
        }
        return;
    }
    promoted_runtime_->process_packet(decoded.packet, peer);
}

void DatagramInbox::drain_promotion_locked() noexcept
{
    for (;;) {
        DatagramEnvelope envelope;
        {
            std::lock_guard lock(mutex_);
            if (promoted_runtime_ == nullptr || size_ == 0U) {
                return;
            }
            envelope = entries_[head_];
            head_ = (head_ + 1U) % entries_.size();
            --size_;
        }
        deliver_promoted(
            std::span {envelope.bytes}.first(envelope.size), envelope.peer);
    }
}

bool DatagramInbox::pop_dispatch_prefix(DatagramEnvelope& envelope) noexcept
{
    std::lock_guard lock(mutex_);
    if (!queued_promotion_ || size_ == 0U) {
        return false;
    }
    envelope = entries_[head_];
    head_ = (head_ + 1U) % entries_.size();
    --size_;
    return true;
}

bool DatagramInbox::dispatch_prefix_empty() noexcept
{
    std::lock_guard lock(mutex_);
    return size_ == 0U;
}

void DatagramInbox::finish_promotion() noexcept
{
    {
        std::lock_guard lock(mutex_);
        if (queued_promotion_) {
            return;
        }
    }
    // Sealing prevents further queue insertion and setup consumption. Only
    // this mutex serializes prefix draining; no route/inbox lock spans protocol
    // processing. Late publishers retain this inbox's original runtime identity.
    std::lock_guard lock(promotion_mutex_);
    drain_promotion_locked();
    ready_.notify_all();
}

bool DatagramChannel::promote_setup_connection(std::uint32_t protocol_socket_id,
    const std::shared_ptr<DatagramInbox>& inbox,
    std::shared_ptr<ConnectionRuntime> runtime,
    std::shared_ptr<ConnectionDatagramDispatcher> dispatcher) noexcept
{
    if (protocol_socket_id == 0U || inbox == nullptr || runtime == nullptr
        || (dispatcher != nullptr && !dispatcher->targets(runtime, this))) {
        return false;
    }
    const auto replay_key = runtime->handshake_replay_key();
    std::shared_ptr<ConnectionRuntime> removed_runtime;
    std::shared_ptr<ConnectionDatagramDispatcher> removed_dispatcher;
    std::shared_ptr<DatagramInbox> removed_prefix;
    try {
        std::unique_lock lock(routes_mutex_);
        const auto setup = setup_routes_.find(protocol_socket_id);
        if (setup == setup_routes_.end() || setup->second.inbox != inbox) {
            return false;
        }
        if (dispatcher != nullptr
            && !dispatcher->matches_peer(setup->second.peer)) {
            return false;
        }
        std::unique_lock inbox_lock(inbox->mutex_);
        if (inbox->admission_retired_ || inbox->promoted_runtime_ != nullptr
            || (dispatcher != nullptr && inbox->closed_)
            || ((dispatcher != nullptr || queued_routes_ != 0U)
                && std::any_of(routes_.begin(), routes_.end(),
                    [&](const auto& route) {
                        return route.second.runtime == runtime
                            && (dispatcher != nullptr
                                || route.second.dispatcher != nullptr);
                    }))
            || !register_connection_locked(
                protocol_socket_id, runtime, replay_key)) {
            return false;
        }
        if (dispatcher != nullptr && !dispatcher->claim_route(inbox)) {
            unregister_connection_locked(protocol_socket_id, removed_runtime,
                removed_dispatcher, removed_prefix);
            return false;
        }
        const IpEndpoint peer = setup->second.peer;
        inbox->promoted_runtime_ = runtime;
        inbox->promoted_peer_ = peer;
        if (dispatcher != nullptr) {
            inbox->queued_promotion_ = true;
            inbox->promoted_dispatcher_ = dispatcher;
            routes_.find(protocol_socket_id)->second.dispatcher = dispatcher;
            ++queued_routes_;
            if (!dispatcher->activate_route()) {
                inbox->promoted_runtime_.reset();
                inbox->queued_promotion_ = false;
                inbox->promoted_dispatcher_.reset();
                unregister_connection_locked(protocol_socket_id,
                    removed_runtime, removed_dispatcher, removed_prefix);
                inbox_lock.unlock();
                lock.unlock();
                dispatcher->retire();
                runtime->mark_broken(0);
                return false;
            }
            setup_routes_.erase(setup);
            inbox->ready_.notify_all();
            inbox_lock.unlock();
            lock.unlock();
            notify_send_work();
            return true;
        }
        routes_.find(protocol_socket_id)->second.setup_prefix = inbox;
        setup_routes_.erase(setup);
        inbox_lock.unlock();
        lock.unlock();
        inbox->finish_promotion();
        lock.lock();
        const auto established = routes_.find(protocol_socket_id);
        if (established != routes_.end()
            && established->second.runtime == runtime
            && established->second.setup_prefix == inbox) {
            established->second.setup_prefix.reset();
        }
        lock.unlock();
        notify_send_work();
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    } catch (...) {
        return false;
    }
}

void DatagramChannel::unregister_connection_locked(
    std::uint32_t protocol_socket_id,
    std::shared_ptr<ConnectionRuntime>& retired,
    std::shared_ptr<ConnectionDatagramDispatcher>& dispatcher,
    std::shared_ptr<DatagramInbox>& prefix) noexcept
{
    const auto route = routes_.find(protocol_socket_id);
    if (route == routes_.end()) {
        return;
    }
    if (route->second.replay_key.has_value()) {
        handshake_routes_.erase(*route->second.replay_key);
    }
    auto& node = route->second;
    if (node.fault_pending) {
        --fault_routes_remaining_;
    }
    if (next_fault_route_ == &node) {
        next_fault_route_ = node.next == &node ? nullptr : node.next;
    }
    if (node.next == &node) {
        next_poll_route_ = nullptr;
    } else {
        node.previous->next = node.next;
        node.next->previous = node.previous;
        if (next_poll_route_ == &node) {
            next_poll_route_ = node.next;
        }
    }
    retired = std::move(node.runtime);
    dispatcher = std::move(node.dispatcher);
    if (dispatcher != nullptr) {
        --queued_routes_;
    }
    prefix = std::move(node.setup_prefix);
    routes_.erase(route);
}

std::shared_ptr<ConnectionDatagramDispatcher>
DatagramChannel::retire_connection(std::uint32_t protocol_socket_id) noexcept
{
    // Last owning references and service retirement run outside the route lock.
    std::shared_ptr<ConnectionRuntime> retired;
    std::shared_ptr<ConnectionDatagramDispatcher> dispatcher;
    std::shared_ptr<DatagramInbox> prefix;
    {
        std::lock_guard lock(routes_mutex_);
        unregister_connection_locked(
            protocol_socket_id, retired, dispatcher, prefix);
    }
    if (dispatcher != nullptr) {
        dispatcher->retire();
    }
    return dispatcher;
}

void DatagramChannel::unregister_connection(
    std::uint32_t protocol_socket_id) noexcept
{
    const auto receipt = retire_connection(protocol_socket_id);
    if (receipt != nullptr && !RuntimeScheduler::on_worker_thread()) {
        (void)receipt->finish_retirement(std::chrono::steady_clock::now());
    }
}

bool DatagramChannel::replay_established_handshake(
    const HandshakeEnvelope& envelope) noexcept
{
    std::shared_ptr<ConnectionDatagramDispatcher> dispatcher;
    std::shared_ptr<ConnectionRuntime> runtime;
    std::shared_ptr<DatagramInbox> setup_prefix;
    {
        std::lock_guard lock(routes_mutex_);
        if (channel_faulted_
            || shutdown_requested_.load(std::memory_order_acquire)) {
            return false;
        }
        const auto replay = handshake_routes_.find({
            .peer = envelope.peer,
            .peer_socket_id = envelope.message.packet.socket_id,
        });
        if (replay != handshake_routes_.end()) {
            runtime = replay->second->runtime;
            setup_prefix = replay->second->setup_prefix;
            dispatcher = replay->second->dispatcher;
        }
    }
    if (setup_prefix != nullptr) {
        setup_prefix->finish_promotion();
    }
    if (dispatcher != nullptr) {
        if (!runtime->accepts_handshake_replay(
                envelope.message, envelope.peer)) {
            return false;
        }
        // Established replay only consumes the base identity fields. The
        // envelope API has already decoded framing and owns no raw datagram.
        HandshakeAction action;
        action.kind = HandshakeActionKind::send;
        action.packet = envelope.message.packet;
        std::array<std::byte, DatagramEnvelope::maximum_size> bytes {};
        const auto encoded =
            encode_handshake_datagram(action, envelope.control.timestamp,
                envelope.control.destination_socket_id, bytes);
        if (!encoded) {
            return false;
        }
        const auto status = dispatcher->publish(dispatcher->inbox()->token(),
            std::span {bytes}.first(encoded.bytes_written), envelope.peer);
        return status == ConnectionDatagramInbox::Status::accepted
            || status == ConnectionDatagramInbox::Status::full;
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
        if (shutdown_requested_.load(std::memory_order_acquire)) {
            return false;
        }
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
    if (shutdown_requested_.load(std::memory_order_acquire)
        || !listener_inbox_.expired()) {
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
    return start_with_affinity(acquire_runtime_scheduler(), std::nullopt);
}

bool DatagramChannel::start(std::shared_ptr<RuntimeScheduler> scheduler,
    std::uint64_t affinity) noexcept
{
    return start_with_affinity(std::move(scheduler), affinity);
}

bool DatagramChannel::start_with_affinity(
    std::shared_ptr<RuntimeScheduler> scheduler,
    std::optional<std::uint64_t> affinity) noexcept
{
    if (shutdown_requested_.load(std::memory_order_acquire)) {
        return false;
    }
    std::shared_ptr<ScheduledWorkContext> context;
    try {
        context = std::make_shared<ScheduledWorkContext>();
        context->owner = shared_from_this();
    } catch (...) {
        return false;
    }

    bool use_readiness = false;
    {
        std::lock_guard lock(lifecycle_mutex_);
        use_readiness = !force_timer_polling_for_testing_;
    }
    auto readiness = use_readiness && scheduler != nullptr
        ? scheduler->acquire_socket_readiness()
        : nullptr;
    std::lock_guard lifecycle_lock(lifecycle_mutex_);
    if (shutdown_requested_.load(std::memory_order_acquire)) {
        return false;
    }
    if (running()) {
        return true;
    }
    if (scheduler == nullptr || !scheduler->snapshot().accepting) {
        return false;
    }
    scheduler_ = std::move(scheduler);
    scheduled_work_context_ = std::move(context);
    socket_readiness_ = std::move(readiness);
    if (socket_readiness_ != nullptr) {
        socket_watch_ = socket_readiness_->watch(socket.native_handle(),
            {.function = socket_readable, .context = scheduled_work_context_});
    }
    readiness_available_.store(
        socket_watch_.valid(), std::memory_order_release);
    readiness_parked_ = false;
    // Setup and established-runtime installation can start the same channel.
    // Allocate a default placement only after the running check, under this
    // lifecycle lock. Consuming it for no-op starts collapses even-stride
    // connection creation onto one of the two scheduler shards.
    affinity_ = affinity.has_value() ? *affinity : next_channel_affinity();
    scheduled_timer_ = {};
    scheduled_coarse_timer_probe_ = false;
    coarse_timer_probe_wakes_.store(0, std::memory_order_relaxed);
    task_active_ = false;
    send_work_notification_pending_ = false;
    receive_release_pending_ = false;
    active_thread_ = {};
    running_.store(true, std::memory_order_release);
    if (schedule_next_locked(true, std::chrono::microseconds {0})) {
        return true;
    }
    running_.store(false, std::memory_order_release);
    readiness_available_.store(false, std::memory_order_release);
    if (socket_readiness_ != nullptr) {
        socket_readiness_->cancel(socket_watch_);
    }
    socket_watch_ = {};
    socket_readiness_.reset();
    scheduler_.reset();
    scheduled_work_context_.reset();
    return false;
}

void DatagramChannel::notify_send_work() noexcept
{
    notify_work(false);
}

void DatagramChannel::notify_receive_release() noexcept
{
    notify_work(true);
}

void DatagramChannel::notify_work(bool receive_release) noexcept
{
    bool scheduling_failed = false;
    {
        std::lock_guard lifecycle_lock(lifecycle_mutex_);
        if (!running_.load(std::memory_order_relaxed)
            || scheduler_ == nullptr) {
            return;
        }
        bool& pending = receive_release ? receive_release_pending_
                                        : send_work_notification_pending_;
        // Application reads may share the existing short poll. Do not cancel
        // and resubmit it per packet, or move its deadline on every read.
        if (receive_release && scheduled_timer_.valid()
            && scheduled_deadline_
                <= std::chrono::steady_clock::now() + idle_wait_) {
            return;
        }
        // A probe is a bounded sub-millisecond wait. Preserve it even when
        // the producer keeps enqueueing packets, or recovery can starve.
        if (scheduled_timer_.valid() && scheduled_coarse_timer_probe_) {
            pending = true;
            return;
        }
        if (!scheduled_timer_.valid()) {
            if (task_active_) {
                pending = true;
                return;
            }
            if (!readiness_parked_) {
                return;
            }
            readiness_parked_ = false;
        } else if (!scheduler_->cancel_timer(scheduled_timer_)) {
            // The worker already owns the continuation. Record a follow-up in
            // case it passed this connection before the enqueue completed.
            pending = true;
            return;
        }
        scheduled_timer_ = {};
        scheduled_coarse_timer_probe_ = false;
        send_work_notification_pending_ = false;
        receive_release_pending_ = false;
        if (!schedule_next_locked(true, std::chrono::microseconds {0})
            && !schedule_next_locked(false, std::chrono::microseconds {0})) {
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
        force_timer_polling_for_testing_ = true;
        idle_wait_ = std::clamp(timeout, std::chrono::milliseconds {1},
            std::chrono::milliseconds {std::numeric_limits<int>::max()});
    }
}

bool DatagramChannel::stop(
    std::chrono::steady_clock::time_point deadline) noexcept
{
    std::shared_ptr<RuntimeScheduler> scheduler;
    std::shared_ptr<SocketReadiness> readiness;
    SocketReadiness::Token watch;
    RuntimeScheduler::TimerToken timer;
    {
        std::lock_guard lifecycle_lock(lifecycle_mutex_);
        running_.store(false, std::memory_order_release);
        scheduler = scheduler_;
        readiness_available_.store(false, std::memory_order_release);
        readiness_parked_ = false;
        readiness = std::move(socket_readiness_);
        watch = socket_watch_;
        socket_watch_ = {};
        timer = scheduled_timer_;
        scheduled_timer_ = {};
        scheduled_coarse_timer_probe_ = false;
    }
    if (readiness != nullptr) {
        readiness->cancel(watch);
    }
    if (scheduler != nullptr && timer.valid()) {
        (void)scheduler->cancel_timer(timer);
    }
    {
        std::unique_lock lifecycle_lock(lifecycle_mutex_);
        if (task_active_ && active_thread_ != std::this_thread::get_id()) {
            const auto quiet = [this] {
                return !task_active_;
            };
            if (deadline == std::chrono::steady_clock::time_point::max()) {
                lifecycle_idle_.wait(lifecycle_lock, quiet);
            } else if (!lifecycle_idle_.wait_until(
                           lifecycle_lock, deadline, quiet)) {
                // Keep the scheduler/context until a later shutdown retry.
                return false;
            }
        }
        scheduler_.reset();
        scheduled_work_context_.reset();
    }
    return true;
}

DatagramChannel::ShutdownStatus DatagramChannel::shutdown(
    std::chrono::steady_clock::time_point deadline) noexcept
{
    if (RuntimeScheduler::on_worker_thread()) {
        return ShutdownStatus::worker_thread;
    }
    std::unique_lock shutdown_lock(shutdown_mutex_, std::try_to_lock);
    if (!shutdown_lock.owns_lock()) {
        return ShutdownStatus::busy;
    }
    if (shutdown_finished_) {
        return ShutdownStatus::retired;
    }
    {
        std::lock_guard lifecycle_lock(lifecycle_mutex_);
        shutdown_requested_.store(true, std::memory_order_release);
        running_.store(false, std::memory_order_release);
    }
    if (!stop(deadline)) {
        return ShutdownStatus::timeout;
    }
    decltype(routes_) routes;
    decltype(setup_routes_) setups;
    std::shared_ptr<HandshakeInbox> listener;
    {
        std::lock_guard route_lock(routes_mutex_);
        routes.swap(routes_);
        setups.swap(setup_routes_);
        listener = listener_inbox_.lock();
        listener_inbox_.reset();
        handshake_routes_.clear();
        queued_routes_ = 0;
        next_poll_route_ = nullptr;
        next_fault_route_ = nullptr;
        fault_routes_remaining_ = 0;
        poll_round_remaining_ = 0;
        poll_round_deadline_.reset();
    }
    // No route/lifecycle lock crosses readiness callbacks or protocol close.
    if (listener != nullptr) {
        listener->close();
    }
    for (auto& setup : setups) {
        setup.second.inbox->retire();
    }
    for (auto& route : routes) {
        if (route.second.setup_prefix != nullptr) {
            route.second.setup_prefix->retire();
        }
        close_connection_runtime(
            route.second.runtime, {}, std::move(route.second.dispatcher));
    }
    {
        std::lock_guard send_lock(send_mutex_);
        send_closed_ = true;
        // Move ownership out without opening a replacement descriptor.
        auto retired_socket = std::move(socket);
    }
    release_native_credit();
    shutdown_finished_ = true;
    return ShutdownStatus::retired;
}

bool DatagramChannel::schedule_next_locked(bool immediate,
    std::chrono::microseconds delay, bool coarse_timer_probe,
    std::optional<std::chrono::steady_clock::time_point>
        absolute_deadline) noexcept
{
    if (!running_.load(std::memory_order_relaxed) || scheduler_ == nullptr
        || scheduled_work_context_ == nullptr) {
        return false;
    }
    RuntimeScheduler::Task task {
        .function = run_scheduled,
        .context = scheduled_work_context_,
        .timer_function = run_scheduled_timer,
    };
    if (immediate) {
        const RuntimeScheduler::SubmitStatus status =
            scheduler_->submit(affinity_, std::move(task));
        if (status != RuntimeScheduler::SubmitStatus::accepted) {
            return false;
        }
        scheduled_timer_ = {};
        scheduled_coarse_timer_probe_ = false;
        return true;
    }
    const auto deadline =
        absolute_deadline.value_or(std::chrono::steady_clock::now() + delay);
    const RuntimeScheduler::ScheduleResult scheduled =
        scheduler_->schedule_at(affinity_, deadline, std::move(task));
    if (scheduled.status != RuntimeScheduler::SubmitStatus::accepted) {
        return false;
    }
    scheduled_timer_ = scheduled.token;
    scheduled_deadline_ = deadline;
    scheduled_coarse_timer_probe_ = coarse_timer_probe;
    return true;
}

void DatagramChannel::observe_timer_wake_locked(
    std::optional<std::uint64_t> lateness_microseconds,
    std::chrono::steady_clock::time_point now) noexcept
{
    const bool was_coarse = timer_wake_monitor_.coarse();
    if (lateness_microseconds.has_value()) {
        timer_wake_monitor_.observe(*lateness_microseconds);
    } else {
        // A busy shard benefits from yielding to its deadlines, even if the
        // host clock was previously coarse. Do not feed callback/queue delay
        // back into a mode that adds more immediate work to that same shard.
        timer_wake_monitor_ = {};
    }
    const bool is_coarse = timer_wake_monitor_.coarse();
    if (was_coarse != is_coarse) {
        next_coarse_timer_probe_ = is_coarse
            ? now + coarse_timer_probe_interval_
            : std::chrono::steady_clock::time_point {};
    }
    coarse_timer_mode_.store(is_coarse, std::memory_order_relaxed);
}

void DatagramChannel::observe_timer_wake_for_testing(
    std::optional<std::uint64_t> lateness_microseconds) noexcept
{
    std::lock_guard lifecycle_lock(lifecycle_mutex_);
    observe_timer_wake_locked(
        lateness_microseconds, std::chrono::steady_clock::now());
}

void DatagramChannel::socket_readable(void* context) noexcept
{
    const auto& work = *static_cast<ScheduledWorkContext*>(context);
    if (const auto owner = work.owner.lock()) {
        owner->notify_send_work();
    }
}

void DatagramChannel::run_scheduled(void* context) noexcept
{
    const auto& work = *static_cast<ScheduledWorkContext*>(context);
    const std::shared_ptr<DatagramChannel> owner = work.owner.lock();
    if (owner != nullptr) {
        owner->run_scheduled(&work);
    }
}

void DatagramChannel::run_scheduled_timer(void* context,
    std::optional<std::chrono::steady_clock::time_point> idle_wake) noexcept
{
    const auto& work = *static_cast<ScheduledWorkContext*>(context);
    if (const auto owner = work.owner.lock()) {
        owner->run_scheduled(&work, idle_wake);
    }
}

void DatagramChannel::run_scheduled(const ScheduledWorkContext* context,
    std::optional<std::chrono::steady_clock::time_point> idle_wake) noexcept
{
    {
        std::lock_guard lifecycle_lock(lifecycle_mutex_);
        if (context == nullptr || scheduled_work_context_.get() != context
            || !running_.load(std::memory_order_relaxed)) {
            return;
        }
        if (scheduled_timer_.valid()) {
            // Measure the scheduler's actual idle wake, before other timer
            // callbacks or this channel's lifecycle lock delayed dispatch.
            const auto now = std::chrono::steady_clock::now();
            std::optional<std::uint64_t> lateness;
            if (idle_wake.has_value()) {
                const auto delay = *idle_wake > scheduled_deadline_
                    ? std::chrono::duration_cast<std::chrono::microseconds>(
                          *idle_wake - scheduled_deadline_)
                    : std::chrono::microseconds::zero();
                lateness = static_cast<std::uint64_t>(delay.count());
            }
            observe_timer_wake_locked(lateness, now);
            if (scheduled_coarse_timer_probe_) {
                coarse_timer_probe_wakes_.fetch_add(
                    1, std::memory_order_relaxed);
            }
        }
        scheduled_timer_ = {};
        scheduled_coarse_timer_probe_ = false;
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
            const bool receive_release_pending = receive_release_pending_;
            receive_release_pending_ = false;
            const bool immediate = result.immediate_work
                || (send_work_notification_pending
                    && !result.coarse_timer_probe);
            bool armed = false;
            if (!immediate && result.receive_wait_safe
                && socket_readiness_ != nullptr && socket_watch_.valid()) {
                armed = socket_readiness_->arm(socket_watch_);
                // A registration failure can be transient. Keep the valid
                // watch eligible for the next slice; below we retain the
                // bounded timer fallback until arming succeeds again.
            }
            if (armed && !result.next_work_delay.has_value()
                && !receive_release_pending) {
                readiness_parked_ = true;
            } else {
                auto delay = result.next_work_delay.value_or(idle_wait_);
                auto deadline = result.next_work_deadline;
                // A read can follow this runtime's poll while the channel is
                // still active. Keep a bounded follow-up instead of parking
                // or waiting for a distant keepalive in that race.
                if (receive_release_pending
                    || (!armed && result.receive_wait_safe)) {
                    delay = std::min(delay,
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            idle_wait_));
                    if (deadline.has_value()) {
                        deadline = std::min(*deadline,
                            std::chrono::steady_clock::now() + delay);
                    }
                }
                if (!schedule_next_locked(immediate, delay,
                        result.coarse_timer_probe && !immediate, deadline)) {
                    running_.store(false, std::memory_order_release);
                    scheduling_failed = true;
                }
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
        std::shared_ptr<ConnectionDatagramDispatcher> replay_dispatcher;
        std::shared_ptr<DatagramInbox> replay_prefix;
        std::shared_ptr<DatagramInbox> setup_inbox;
        std::shared_ptr<HandshakeInbox> inbox;
        {
            std::lock_guard lock(routes_mutex_);
            if (channel_faulted_
                || shutdown_requested_.load(std::memory_order_acquire)) {
                return;
            }
            const auto replay = handshake_routes_.find({
                .peer = peer,
                .peer_socket_id = decoded.message.packet.socket_id,
            });
            if (replay != handshake_routes_.end()) {
                replay_runtime = replay->second->runtime;
                replay_prefix = replay->second->setup_prefix;
                replay_dispatcher = replay->second->dispatcher;
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
        if (replay_prefix != nullptr) {
            replay_prefix->finish_promotion();
        }
        if (replay_dispatcher != nullptr) {
            if (replay_runtime->accepts_handshake_replay(
                    decoded.message, peer)) {
                // A valid but full replay is consumed, never diverted to setup.
                (void)replay_dispatcher->publish(
                    replay_dispatcher->inbox()->token(), datagram, peer);
                return;
            }
        } else if (replay_runtime != nullptr
            && replay_runtime->process_handshake(decoded.message, peer)) {
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
    std::shared_ptr<ConnectionDatagramDispatcher> dispatcher;
    std::shared_ptr<DatagramInbox> setup_prefix;
    std::shared_ptr<DatagramInbox> setup_inbox;
    {
        std::lock_guard lock(routes_mutex_);
        if (channel_faulted_
            || shutdown_requested_.load(std::memory_order_acquire)) {
            return;
        }
        const auto route = routes_.find(
            destination_socket_id);
        if (route != routes_.end()) {
            runtime = route->second.runtime;
            dispatcher = route->second.dispatcher;
            setup_prefix = route->second.setup_prefix;
        } else {
            const auto setup = setup_routes_.find(
                destination_socket_id);
            if (setup != setup_routes_.end()
                && setup->second.peer == peer) {
                setup_inbox = setup->second.inbox;
            }
        }
    }
    if (setup_prefix != nullptr) {
        setup_prefix->finish_promotion();
    }
    if (dispatcher != nullptr) {
        (void)dispatcher->publish(dispatcher->inbox()->token(), datagram, peer);
    } else if (runtime != nullptr) {
        runtime->process_packet(packet, peer);
    } else if (setup_inbox != nullptr) {
        (void)setup_inbox->push(datagram, peer);
    }
}

void DatagramChannel::mark_connections_broken(int system_error) noexcept
{
    {
        std::lock_guard lock(routes_mutex_);
        if (channel_faulted_
            || shutdown_requested_.load(std::memory_order_acquire)) {
            return;
        }
        channel_faulted_ = true;
        next_fault_route_ = next_poll_route_;
        fault_routes_remaining_ = routes_.size();
        for (auto& route : routes_) {
            route.second.fault_pending = true;
        }
    }
    for (;;) {
        std::shared_ptr<ConnectionRuntime> runtime;
        std::shared_ptr<ConnectionDatagramDispatcher> dispatcher;
        {
            std::lock_guard lock(routes_mutex_);
            if (fault_routes_remaining_ == 0U) {
                next_fault_route_ = nullptr;
                break;
            }
            // Registration is closed. Concurrent detach updates this cursor
            // and the pending count before erasing a node.
            while (!next_fault_route_->fault_pending) {
                next_fault_route_ = next_fault_route_->next;
            }
            auto& route = *next_fault_route_;
            next_fault_route_ = route.next;
            route.fault_pending = false;
            --fault_routes_remaining_;
            runtime = route.runtime;
            dispatcher = route.dispatcher;
        }
        // Protocol clocks/readiness and service retirement must never execute
        // under the route table mutex. Strong snapshots survive detach/reuse.
        if (dispatcher != nullptr) {
            dispatcher->retire();
        }
        runtime->mark_broken(system_error);
    }
}

RuntimePollResult DatagramChannel::run_once() noexcept
{
    return run_receive_slice([this](std::span<std::byte> datagram) noexcept {
        return socket.receive_from(datagram);
    });
}

RuntimePollResult DatagramChannel::poll_connections(
    std::optional<std::chrono::steady_clock::time_point> injected_now,
    std::chrono::steady_clock::time_point (*clock)(void*) noexcept,
    void* clock_context) noexcept
{
    const auto current_time = [&] {
        return clock != nullptr        ? clock(clock_context)
            : injected_now.has_value() ? *injected_now
                                       : std::chrono::steady_clock::now();
    };
    {
        std::lock_guard lock(routes_mutex_);
        if (poll_round_remaining_ == 0U) {
            poll_round_remaining_ = routes_.size();
            poll_round_immediate_ = false;
            poll_round_receive_wait_safe_ = true;
            poll_round_deadline_.reset();
        }
    }
    std::size_t remaining_send_attempts = maximum_send_batch;
    for (std::size_t visited = 0;
        visited < maximum_connection_polls && remaining_send_attempts != 0U;
        ++visited) {
        std::shared_ptr<ConnectionRuntime> runtime;
        std::shared_ptr<ConnectionDatagramDispatcher> dispatcher;
        std::shared_ptr<DatagramInbox> setup_prefix;
        {
            std::lock_guard lock(routes_mutex_);
            if (next_poll_route_ == nullptr) {
                poll_round_remaining_ = 0U;
            }
            if (poll_round_remaining_ == 0U) {
                break;
            }
            runtime = next_poll_route_->runtime;
            dispatcher = next_poll_route_->dispatcher;
            setup_prefix = next_poll_route_->setup_prefix;
            next_poll_route_ = next_poll_route_->next;
            --poll_round_remaining_;
        }
        // Keep the runtime alive through a concurrent unregister, without
        // holding the route table across protocol work or nonblocking sends.
        if (setup_prefix != nullptr) {
            setup_prefix->finish_promotion();
        }
        // Pending prefix effects must finish before any established poll. The
        // worker wakes the channel on completion; keep only a bounded fallback.
        const auto ingress_wait = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(idle_wait_)
                .count());
        const bool prefix_pending =
            dispatcher != nullptr && !dispatcher->setup_prefix_complete();
        bool prefix_terminal = false;
        const auto result = prefix_pending
            ? runtime->poll_setup_prefix_deadline(ingress_wait, prefix_terminal)
            : runtime->poll(remaining_send_attempts,
                  dispatcher != nullptr ? dispatcher->inbox().get() : nullptr,
                  ingress_wait, true);
        // Terminal prefix handling retires the sink outside both runtime and
        // route locks. A paused protocol callback still has its close barrier.
        if (prefix_terminal) {
            dispatcher->retire();
        }
        poll_round_immediate_ |= result.immediate_work;
        const bool can_wait =
            readiness_available_.load(std::memory_order_acquire)
            && result.receive_wait_safe;
        poll_round_receive_wait_safe_ &= can_wait;
        if (can_wait && !result.next_work_delay.has_value()) {
            continue;
        }
        const auto delay = can_wait
            ? *result.next_work_delay
            : std::min(result.next_work_delay.value_or(idle_wait_),
                  std::chrono::duration_cast<std::chrono::microseconds>(
                      idle_wait_));
        // Store an absolute deadline: each continuation must not restart an
        // earlier connection's pacing/backpressure/idle wait.
        const auto relative_deadline = current_time() + delay;
        const auto deadline = !injected_now.has_value() && clock == nullptr
                && result.next_work_deadline.has_value()
            ? (can_wait
                      ? *result.next_work_deadline
                      : std::min(*result.next_work_deadline, relative_deadline))
            : relative_deadline;
        if (!poll_round_deadline_.has_value()
            || deadline < *poll_round_deadline_) {
            poll_round_deadline_ = deadline;
        }
    }
    if (poll_round_remaining_ != 0U || poll_round_immediate_) {
        return {.immediate_work = true};
    }
    const bool can_wait = poll_round_receive_wait_safe_
        && readiness_available_.load(std::memory_order_acquire);
    if (can_wait && !poll_round_deadline_.has_value()) {
        return {.receive_wait_safe = true};
    }
    const auto delay = poll_round_deadline_.has_value()
        ? std::chrono::duration_cast<std::chrono::microseconds>(
              *poll_round_deadline_ - current_time())
        : std::chrono::duration_cast<std::chrono::microseconds>(idle_wait_);
#if ROBOTWEAX_SRT_SUBMILLISECOND_TIMER_PACING
    if (delay <= std::chrono::microseconds::zero()) {
        return {.immediate_work = true};
    }
    // A sub-millisecond deadline (a pacing slot above ~11 Mbit/s at
    // 1316-byte payloads) is normally a timer wait: resubmitting until the
    // slot arrived kept a shard busy with an empty recvmsg and a route
    // sweep per pass. The pacer's schedule credit absorbs the timer's
    // wake-up latency. Only when the host's timer wake-ups have proven
    // coarser than that credit does the channel resubmit as before, so a
    // coarse timer never lowers the send rate.
    if (coarse_timer_mode_.load(std::memory_order_relaxed)
        && delay < std::chrono::milliseconds {1}) {
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard lifecycle_lock(lifecycle_mutex_);
        if (now < next_coarse_timer_probe_) {
            return {.immediate_work = true};
        }
        next_coarse_timer_probe_ = now + coarse_timer_probe_interval_;
        return {.next_work_delay = delay,
            .receive_wait_safe = can_wait,
            .coarse_timer_probe = true,
            .next_work_deadline = poll_round_deadline_};
    }
    return {.next_work_delay = delay,
        .receive_wait_safe = can_wait,
        .next_work_deadline = poll_round_deadline_};
#else
    // Keep the pre-CR-12 cooperative pacing behavior on Windows and other
    // platforms, including the immediate continuation for due deadlines.
    if (delay < std::chrono::milliseconds {1}) {
        return {.immediate_work = true};
    }
    return {.next_work_delay = delay,
        .receive_wait_safe = can_wait,
        .next_work_deadline = poll_round_deadline_};
#endif
}

ConnectionRuntime::ConnectionRuntime(Configuration configuration)
    : channel_(std::move(configuration.channel))
    , work_binding_(std::move(configuration.work_binding))
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
    , receiver_key_state_(configuration.receiver_key_state)
    , now_function_(configuration.now_function)
    , now_context_(configuration.now_context)
    , receive_pop_hook_for_testing_(configuration.receive_pop_hook_for_testing)
    , receive_pop_context_for_testing_(
          configuration.receive_pop_context_for_testing)
    , close_retry_hook_for_testing_(configuration.close_retry_hook_for_testing)
    , close_retry_context_for_testing_(
          configuration.close_retry_context_for_testing)
    , flow_window_packets_(effective_peer_flow_window(configuration))
{
    const std::size_t receive_capacity =
        effective_receive_capacity(configuration);
    session_.configure_efficient_retransmission(
        configuration.efficient_retransmission,
        configuration.negotiated_options.peer_periodic_nak
            || configuration.negotiated_options.periodic_nak);
    if (uses_file_congestion_control(
            configuration.options.congestion_controller())) {
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
        if (configuration.group) {
            group_readiness_source_ = configuration.group->readiness_source;
        }
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
#ifdef ENABLE_MAXREXMITBW
    retransmission_budget_.configure(
        configuration.options
            .get(
                SocketOption::maximum_retransmission_bandwidth_bytes_per_second)
            .value,
        now_microseconds());
#endif
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
    // A source time at or before the connection origin must not wrap to a large
    // 32-bit timestamp: the peer would read it as a value almost a full period
    // in the future and stall delivery of every later packet. Clamp it to the
    // origin instead.
    const std::int64_t bounded = relative < 0 ? 0 : relative;
    return PacketTimestamp {static_cast<std::uint32_t>(bounded)};
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

ConnectionRuntime::~ConnectionRuntime()
{
    if (work_binding_ != nullptr) {
        work_binding_->retire();
    }
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
    if (options_.control_profile() && ttl_milliseconds >= 0) {
        return {.status = MessageIoStatus::invalid_state};
    }
    in_order = in_order || options_.control_profile();
    const bool has_deadline = timeout_milliseconds >= 0;
    const Clock::time_point deadline = has_deadline
        ? Clock::now() + std::chrono::milliseconds{timeout_milliseconds}
        : Clock::time_point{};
    for (;;) {
        const auto result = try_queue_message_locked(
            message, source_time_microseconds, in_order, ttl_milliseconds);
        if (result.status == MessageIoStatus::success) {
            lock.unlock();
            notify_channel_send_work();
            notify_readiness();
            return result;
        }
        if (result.status != MessageIoStatus::would_block) {
            return result;
        }
        if (!blocking) {
            note_not_ready(SRT_EPOLL_OUT);
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

MessageIoResult ConnectionRuntime::try_queue_message_locked(
    std::span<const std::byte> message, std::int64_t source_time_microseconds,
    bool in_order, std::int32_t ttl_milliseconds) noexcept
{
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
        if (session_.send_buffer().available() == 0U) {
            note_not_ready(SRT_EPOLL_OUT);
        }
        notify_readiness();
        return {.status = MessageIoStatus::peer_error};
    }

    const std::uint64_t now = now_microseconds();
    statistics_.update_send_duration(now, session_.send_buffer().size() != 0U);
    const std::uint64_t expiration_microseconds =
        message_expiration_microseconds(now, source_time_microseconds,
            origin_epoch_microseconds_, ttl_milliseconds);
    const std::uint32_t message_number = session_.next_message_number();
    const SequenceNumber first_sequence =
        session_.send_buffer().next_sequence();
    const Error queued = session_.queue_message(message,
        packet_timestamp(source_time_microseconds), in_order, now,
        expiration_microseconds);
    if (queued == Error::none) {
        statistics_.update_send_duration(now, true);
        sample_sender_buffer_statistics(now);
        if (session_.send_buffer().available() == 0U) {
            note_not_ready(SRT_EPOLL_OUT);
        }
        const MessageIoResult result {
            .status = MessageIoStatus::success,
            .bytes = message.size(),
            .message_number = message_number,
            .first_sequence = first_sequence,
            .next_sequence = session_.send_buffer().next_sequence(),
        };
        return result;
    }
    if (queued != Error::buffer_too_small) {
        return {.status = MessageIoStatus::invalid_state};
    }
    return {.status = MessageIoStatus::would_block};
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
    if (options_.control_profile() && ttl_milliseconds >= 0) {
        return {.status = MessageIoStatus::invalid_state};
    }
    in_order = in_order || options_.control_profile();
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
        if (session_.send_buffer().available() == 0U) {
            note_not_ready(SRT_EPOLL_OUT);
        }
        notify_readiness();
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
        note_not_ready(SRT_EPOLL_OUT);
        return {.status = MessageIoStatus::would_block};
    }
    if (queued != Error::none) {
        return {.status = MessageIoStatus::invalid_state};
    }
    statistics_.update_send_duration(now, true);
    sample_sender_buffer_statistics(now);
    if (session_.send_buffer().available() == 0U) {
        note_not_ready(SRT_EPOLL_OUT);
    }
    const MessageIoResult result {
        .status = MessageIoStatus::success,
        .bytes = message.size(),
        .message_number = message_number,
        .first_sequence = first_sequence,
        .next_sequence = session_.send_buffer().next_sequence(),
    };
    lock.unlock();
    notify_channel_send_work();
    notify_readiness();
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
        const auto result = try_receive_message_locked(destination, now);
        if (result.status == MessageIoStatus::success) {
            lock.unlock();
            notify_channel_receive_release();
            notify_readiness();
            return result;
        }
        if (result.status != MessageIoStatus::would_block) {
            return result;
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

MessageIoResult ConnectionRuntime::try_receive_message_locked(
    std::span<std::byte> destination, std::uint64_t now) noexcept
{
    if (!service_receiver_tlpktdrop_locked(now)) {
        return {
            .status = MessageIoStatus::broken,
            .system_error = system_error_,
        };
    }
    const auto received = session_.pop_message_at(destination, now);
    if (received) {
        if (receive_pop_hook_for_testing_ != nullptr) {
            receive_pop_hook_for_testing_(receive_pop_context_for_testing_);
        }
        session_.note_receive_buffer_released(now);
        sample_receiver_buffer_statistics(now);
        if (!session_.data_ready_at(now)) {
            readiness_source_->note_not_ready(SRT_EPOLL_IN);
        }
        std::int64_t source_time = 0;
        const auto delivery = received.delivery_time_microseconds;
        if (delivery.has_value()
            && *delivery <= static_cast<std::uint64_t>(
                   std::numeric_limits<std::int64_t>::max())) {
            source_time = origin_epoch_microseconds_
                + static_cast<std::int64_t>(*delivery);
        }
        const MessageIoResult result {
            .status = MessageIoStatus::success,
            .bytes = received.bytes_written,
            .message_number = received.message_number,
            .first_sequence = received.first_sequence,
            .next_sequence = session_.receive_buffer().first_stored_sequence(),
            .source_time_microseconds = source_time,
        };
        return result;
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
    return {.status = MessageIoStatus::would_block};
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
    const auto message = session_.receive_buffer().first_complete_message();
    return message.has_value()
        ? std::optional<SequenceNumber> {message->first_sequence}
        : std::nullopt;
}

SequenceNumber ConnectionRuntime::receive_floor_sequence() noexcept
{
    std::lock_guard lock(mutex_);
    return session_.receive_buffer().first_stored_sequence();
}

bool ConnectionRuntime::has_complete_buffered_message_at(
    SequenceNumber sequence) noexcept
{
    std::lock_guard lock(mutex_);
    const auto& buffer = session_.receive_buffer();
    return buffer.first_stored_sequence() == sequence
        && buffer.has_complete_message();
}

bool ConnectionRuntime::has_buffered_receive_data() noexcept
{
    std::lock_guard lock(mutex_);
    return session_.receive_buffer().occupied() != 0U;
}

RuntimeReceiveSnapshot ConnectionRuntime::receive_snapshot(
    SequenceNumber expected, bool retire_consumed_prefix) noexcept
{
    std::lock_guard lock(mutex_);
    if (retire_consumed_prefix) {
        (void)discard_received_before_locked(expected);
    }
    const auto now = now_microseconds();
    const bool serviced = service_receiver_tlpktdrop_locked(now);
    const auto& buffer = session_.receive_buffer();
    RuntimeReceiveSnapshot result {
        .floor_sequence = buffer.first_stored_sequence(),
        .complete_expected = buffer.first_stored_sequence() == expected
            && buffer.has_complete_message(),
        .buffered = buffer.occupied() != 0U,
        .terminal = broken_ || peer_closed_ || locally_closed_,
    };
    if (serviced && session_.message_ready_at(now)) {
        result.readable_sequence = buffer.first_stored_sequence();
    }
    if (!result.readable_sequence.has_value() && !locally_closed_) {
        result.next_delivery = session_.data_ready_at(now)
            ? std::optional<Clock::time_point> {Clock::now()}
            : next_receive_wakeup_locked(now);
    }
    return result;
}

RetainedGroupReceiveBatch
ConnectionRuntime::copy_group_receive_prefix() noexcept
{
    std::lock_guard lock(mutex_);
    return {
        .copies = session_.receive_buffer().copy_complete_messages(
            GroupReceiveRetention::maximum_packets,
            GroupReceiveRetention::maximum_bytes),
        .clock = session_.receive_clock_snapshot(),
        .origin = origin_,
        .origin_epoch_microseconds = origin_epoch_microseconds_,
        .retired_at_microseconds = now_microseconds(),
        .now_function = now_function_,
        .now_context = now_context_,
    };
}

bool ConnectionRuntime::discard_received_before(
    SequenceNumber next_sequence) noexcept
{
    std::lock_guard lock(mutex_);
    return discard_received_before_locked(next_sequence);
}

bool ConnectionRuntime::discard_received_before_locked(
    SequenceNumber next_sequence) noexcept
{
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
    if (!session_.data_ready_at(now_microseconds())) {
        readiness_source_->note_not_ready(SRT_EPOLL_IN);
    }
    notify_readiness();
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
            if (session_.send_buffer().available() == 0U) {
                note_not_ready(SRT_EPOLL_OUT);
            }
            notify_readiness();
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
            if (session_.send_buffer().available() == 0U) {
                note_not_ready(SRT_EPOLL_OUT);
            }
            const MessageIoResult result {
                .status = MessageIoStatus::success,
                .bytes = queued.bytes_accepted,
                .message_number = message_number,
            };
            lock.unlock();
            notify_channel_send_work();
            notify_readiness();
            return result;
        }
        if (queued.error != Error::buffer_too_small) {
            return {.status = MessageIoStatus::invalid_state};
        }
        if (!blocking) {
            note_not_ready(SRT_EPOLL_OUT);
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
        const std::uint64_t now = now_microseconds();
        // A gap ahead of due bytes is dropped here as well, so a stream
        // reader is not held behind a loss whose deadline has passed.
        if (!service_receiver_tlpktdrop_locked(now)) {
            return {
                .status = MessageIoStatus::broken,
                .system_error = system_error_,
            };
        }
        const auto received = session_.pop_stream_at(destination, now);
        if (received) {
            session_.note_receive_buffer_released(now);
            sample_receiver_buffer_statistics(now);
            if (!session_.data_ready_at(now)) {
                readiness_source_->note_not_ready(SRT_EPOLL_IN);
            }
            const MessageIoResult result {
                .status = MessageIoStatus::success,
                .bytes = received.bytes_written,
                .message_number = received.message_number,
                .first_sequence = received.first_sequence,
            };
            lock.unlock();
            notify_channel_receive_release();
            notify_readiness();
            return result;
        }
        if (locally_closed_) {
            return {.status = MessageIoStatus::local_closed};
        }
        // SHUTDOWN ends the peer's send side, but buffered stream packets
        // still have to pass through the TSBPD gate before end-of-stream.
        const bool delivery_pending =
            session_.receive_buffer().has_stream_data()
            || session_.next_receive_delivery_time().has_value();
        if (peer_closed_ && !delivery_pending) {
            return {
                .status = MessageIoStatus::success,
                .bytes = 0,
            };
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
        const auto delivery_wakeup = next_receive_wakeup_locked(now);
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

bool ConnectionRuntime::service_receiver_tlpktdrop_locked(
    std::uint64_t now) noexcept
{
    const auto expired_sensor_gaps = session_.expire_sensor_receive_gaps(now);
    if (!expired_sensor_gaps) {
        break_locked(0);
        return false;
    }
    if (expired_sensor_gaps.receiver_drop_packets != 0U) {
        std::size_t average_payload =
            statistics_.average_received_payload_bytes();
        if (average_payload == 0U) {
            average_payload = options_.maximum_payload_size();
        }
        statistics_.note_receiver_drop(
            expired_sensor_gaps.receiver_drop_packets,
            saturated_multiply(
                expired_sensor_gaps.receiver_drop_packets, average_payload));
        sample_receiver_buffer_statistics(now);
        if (!send_actions(expired_sensor_gaps.actions, now)) {
            return false;
        }
        receive_ready_.notify_all();
        ReadinessSignal::notify();
    }

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
    if (!session_.data_ready_at(now)) {
        readiness_source_->note_not_ready(SRT_EPOLL_IN);
    }
    // Buffered receive data may still be inspected after local close, but
    // cannot append controls to the FIFO owned by the close drain.
    if (!locally_closed_ && !send_actions(dropped.actions, now)) {
        return false;
    }
    receive_ready_.notify_all();
    notify_readiness();
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

bool ConnectionRuntime::retransmission_ready(std::uint64_t now) noexcept
{
#ifdef ENABLE_MAXREXMITBW
    const auto packet = session_.peek_retransmission_packet();
    return packet.has_value()
        && retransmission_budget_.ready(
            packet_header_size + packet->payload.size(), now);
#else
    (void)now;
    return session_.has_pending_retransmission();
#endif
}

bool ConnectionRuntime::submit_datagram(std::span<const std::byte> bytes,
    DatagramCompletion completion, std::uint64_t now) noexcept
{
    const auto channel = channel_.lock();
    if (channel == nullptr || bytes.empty()
        || bytes.size() > DatagramEnvelope::maximum_size) {
        break_locked(0);
        return false;
    }
    if (pending_datagram_size_ == 0U
        && (poll_send_budget_ == nullptr || *poll_send_budget_ != 0U)) {
        if (poll_send_budget_ != nullptr) {
            --*poll_send_budget_;
        }
        const auto sent = channel->send_datagram(bytes, peer_);
        if (sent) {
            if (sent.bytes_transferred != bytes.size()) {
                break_locked(0);
                return false;
            }
            return complete_datagram(bytes, completion, now);
        }
        if (!defer_send_error(sent, now)) {
            return false;
        }
    }
    if (pending_datagram_size_ != 0U
        && completion.kind == DatagramKind::control) {
        // Bound periodic controls during both EAGAIN and route retries.
        // ACKs are cumulative; NAKs contain only newly due ranges, so restore
        // the superseded report to the loss list before replacing its marker.
        if (completion.control == ControlType::keepalive) {
            return true;
        }
        if (completion.control == ControlType::acknowledgement
            || completion.control == ControlType::negative_acknowledgement
            || completion.control == ControlType::acknowledgement_of_ack) {
            for (auto* pending = pending_datagram_head_.get();
                pending != nullptr; pending = pending->next.get()) {
                if (pending->completion.kind == DatagramKind::control
                    && pending->completion.control == completion.control) {
                    if (completion.control
                        == ControlType::negative_acknowledgement) {
                        session_.rearm_loss_report(std::span {pending->bytes}
                                .first(pending->size)
                                .subspan(packet_header_size));
                    }
                    std::copy(
                        bytes.begin(), bytes.end(), pending->bytes.begin());
                    pending->size = bytes.size();
                    pending->completion = completion;
                    return true;
                }
            }
        }
    }
    if (pending_datagram_size_ == pending_datagram_capacity) {
        break_locked(transient_send_system_error_);
        return false;
    }
    auto pending = std::unique_ptr<PendingDatagram> {
        new (std::nothrow) PendingDatagram {}};
    if (pending == nullptr) {
        break_locked(0);
        return false;
    }
    std::copy(bytes.begin(), bytes.end(), pending->bytes.begin());
    pending->size = bytes.size();
    pending->completion = completion;
    auto* tail = pending.get();
    if (pending_datagram_tail_ != nullptr) {
        pending_datagram_tail_->next = std::move(pending);
    } else {
        pending_datagram_head_ = std::move(pending);
    }
    pending_datagram_tail_ = tail;
    ++pending_datagram_size_;
    return true;
}

bool ConnectionRuntime::complete_datagram(std::span<const std::byte> bytes,
    const DatagramCompletion& completion, std::uint64_t now) noexcept
{
    transient_send_failure_since_.reset();
    transient_send_system_error_ = 0;
    if (completion.kind == DatagramKind::data) {
        if (fec_encoder_active() && !completion.data.retransmitted) {
            const PacketView wire_packet {
                .kind = PacketKind::data,
                .data = completion.data,
                .payload = bytes.subspan(packet_header_size),
            };
            if (feed_fec_source(wire_packet) != Error::none) {
                break_locked(0);
                return false;
            }
        }
        statistics_.note_data_sent(
            completion.payload_size, completion.data.retransmitted);
        diagnostics::trace_udp_submit(now, completion.data.sequence.value(),
            completion.data.retransmitted, completion.payload_size,
            session_.send_buffer().size(),
            session_.send_buffer().packets_in_flight());
        session_.note_data_packet_sent(now);
        if (completion.data.retransmitted) {
#ifdef ENABLE_MAXREXMITBW
            retransmission_budget_.consume(bytes.size(), now);
#endif
            session_.note_retransmission_sent(completion.data.sequence, now);
        }
        pacer_.on_packet_sent(bytes.size(), now);
        if (crypto_ != nullptr && !completion.data.retransmitted
            && crypto_->note_data_packet_sent() != Error::none) {
            break_locked(0);
            return false;
        }
    } else {
        if (completion.kind == DatagramKind::filter) {
            statistics_.note_sender_filter_extra(completion.payload_size);
            consume_fec_control();
            pacer_.on_packet_sent(bytes.size(), now);
        } else if (completion.kind == DatagramKind::control) {
            statistics_.note_control_sent(completion.control);
        } else if (completion.kind == DatagramKind::key_request) {
            last_key_material_send_microseconds_ = now;
        }
        session_.note_packet_sent(now);
    }
    return true;
}

bool ConnectionRuntime::defer_send_error(
    const UdpIoResult& failure, std::uint64_t now) noexcept
{
    const bool transient_system_error = failure.error == Error::io_error
        && UdpSocket::is_transient_send_error(failure.system_error);
    if (failure.error != Error::would_block && !transient_system_error) {
        break_locked(failure.system_error);
        return false;
    }
    if (transient_system_error) {
        if (!transient_send_failure_since_.has_value()) {
            transient_send_failure_since_ = now;
        }
        transient_send_system_error_ = failure.system_error;
    }
    if (transient_send_failure_since_.has_value()) {
        // A route can recover without replacing the UDP socket. Keep its
        // datagram in the FIFO, but do not retry a persistent failure forever
        // while inbound traffic continues to refresh the peer idle timer.
        const auto retry_window = std::min(peer_idle_timeout_microseconds_,
            maximum_transient_send_retry_microseconds);
        if (now >= *transient_send_failure_since_
            && now - *transient_send_failure_since_ >= retry_window) {
            break_locked(transient_send_system_error_);
            return false;
        }
    }
    next_datagram_retry_microseconds_ = now
        + std::min<std::uint64_t>(
            1'000U, std::numeric_limits<std::uint64_t>::max() - now);
    return true;
}

bool ConnectionRuntime::flush_pending_datagrams(std::uint64_t now) noexcept
{
    if (pending_datagram_size_ == 0U
        || now < next_datagram_retry_microseconds_) {
        return true;
    }
    const auto channel = channel_.lock();
    if (channel == nullptr) {
        break_locked(0);
        return false;
    }
    for (std::size_t count = 0;
        count < maximum_send_batch && pending_datagram_size_ != 0U; ++count) {
        auto& pending = *pending_datagram_head_;
        const auto bytes = std::span {pending.bytes}.first(pending.size);
        bool current = true;
        if (pending.completion.kind == DatagramKind::control
            && pending.completion.control
                == ControlType::negative_acknowledgement) {
            // Rebuild from surviving losses in poll_timers, rather than send
            // stale bytes for DATA that arrived or was dropped during retry.
            session_.rearm_loss_report(bytes.subspan(packet_header_size));
            current = false;
        } else if (pending.completion.kind == DatagramKind::data) {
            // ACK/TTL/TLPKTDROP can retire a prepared packet during the wait.
            current = session_.send_buffer().retains_packet(
                pending.completion.data.sequence);
        } else if (pending.completion.kind == DatagramKind::key_request) {
            // An earlier copy may have been acknowledged while this retry
            // waited. Never let its completion timestamp delay a new rotation.
            const auto material = crypto_ != nullptr
                ? crypto_->pending_key_material()
                : std::span<const std::byte> {};
            const auto queued_material = bytes.subspan(packet_header_size);
            current = !material.empty()
                && material.size() == queued_material.size()
                && std::equal(
                    material.begin(), material.end(), queued_material.begin());
        }
#ifdef ENABLE_MAXREXMITBW
        if (current && pending.completion.kind == DatagramKind::data
            && pending.completion.data.retransmitted
            && !retransmission_budget_.ready(pending.size, now)) {
            // A changed budget must not hold controls behind this DATA retry.
            // Restore loss work; the main scheduler will wait for its budget.
            session_.requeue_prepared_retransmission(
                pending.completion.data.sequence);
            current = false;
        }
#endif
        if (current) {
            if (poll_send_budget_ != nullptr) {
                if (*poll_send_budget_ == 0U) {
                    return true;
                }
                --*poll_send_budget_;
            }
            const auto sent = channel->send_datagram(bytes, peer_);
            if (!sent) {
                return defer_send_error(sent, now);
            }
            if (sent.bytes_transferred != pending.size) {
                break_locked(0);
                return false;
            }
            if (!complete_datagram(bytes, pending.completion, now)) {
                return false;
            }
        }
        auto completed = std::move(pending_datagram_head_);
        pending_datagram_head_ = std::move(completed->next);
        if (pending_datagram_head_ == nullptr) {
            pending_datagram_tail_ = nullptr;
        }
        --pending_datagram_size_;
        next_datagram_retry_microseconds_ = 0;
    }
    if (pending_datagram_size_ == 0U) {
        transient_send_failure_since_.reset();
        transient_send_system_error_ = 0;
        send_ready_.notify_all();
    }
    return true;
}

RuntimePollResult ConnectionRuntime::pending_send_poll_result(
    std::uint64_t now) const noexcept
{
    if (now >= next_datagram_retry_microseconds_) {
        return {.immediate_work = true};
    }
    // A receive-heavy channel may poll repeatedly before the retry deadline;
    // do not issue another syscall or request sub-millisecond busy polling.
    return {.next_work_delay = std::chrono::milliseconds {1}};
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
            diagnostics::trace_tlpktdrop_counter(now,
                action.drop.sequences.first.value(),
                action.drop.sequences.last.value(), action.dropped_packets,
                action.dropped_bytes, session_.send_buffer().size(),
                session_.send_buffer().packets_in_flight());
            break;
        case SenderDropReason::none:
            break;
        }
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
        const auto type = control_type(actions.values[index].kind);
        if (!type.has_value()) {
            break_locked(0);
            return false;
        }
        if (!submit_datagram(std::span {datagram}.first(encoded.bytes_written),
                {.kind = DatagramKind::control, .control = *type}, now)) {
            return false;
        }
    }
    return true;
}

bool ConnectionRuntime::send_data(
    const OutboundPacket& packet,
    std::uint64_t now) noexcept
{
    MutablePacketView view;
    view.kind = PacketKind::data;
    view.data = packet.header;
    const bool encrypted_retransmission =
        packet.header.retransmitted
        && packet.header.encryption_key != EncryptionKey::none;
    const bool authenticated_data =
        crypto_ != nullptr && crypto_->authenticated_data_enabled();
    if (fec_encoder_active() && !packet.header.retransmitted
        && authenticated_data
        && packet.header.boundary != MessageBoundary::solo) {
        break_locked(0);
        return false;
    }
    if (crypto_ != nullptr && crypto_->enabled()
        && !encrypted_retransmission) {
        view.data.encryption_key =
            crypto_->active_sender_key();
    }
    // Clear/CTR encoding writes the complete submitted prefix. GCM writes
    // its header, ciphertext and tag before submission; every failure returns
    // without sending. The unused tail is never submitted.
    std::array<std::byte, 1500> datagram;
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
    const std::size_t application_payload_size = authenticated_data
            && encrypted_retransmission
            && packet.payload.size() >= srt_gcm_authentication_tag_size
        ? packet.payload.size() - srt_gcm_authentication_tag_size
        : packet.payload.size();
    return submit_datagram(std::span {datagram}.first(datagram_size),
        {.kind = DatagramKind::data,
            .data = view.data,
            .payload_size = application_payload_size},
        now);
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
    return submit_datagram(std::span {datagram}.first(encoded.bytes_written),
        {.kind = DatagramKind::filter, .payload_size = packet->payload.size()},
        now);
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
    const SequenceNumber receive_floor =
        session_.receive_buffer().first_stored_sequence();
    if (matrix_fec_decoder_.has_value()) {
        const auto result =
            matrix_fec_decoder_->receive(wire_packet, receive_floor);
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
        result = row_fec_decoder_->receive(wire_packet, receive_floor);
    } else if (column_fec_decoder_.has_value()) {
        result = column_fec_decoder_->receive(wire_packet, receive_floor);
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

bool ConnectionRuntime::admit_fresh_salt_key_request(
    std::span<const std::byte> payload, std::uint64_t now) noexcept
{
    if (fresh_salt_admission_ == nullptr) {
        try {
            fresh_salt_admission_ = std::make_unique<FreshSaltAdmission>();
        } catch (...) {
            // Do not derive without a budget if bounded bookkeeping cannot
            // be allocated. The peer can retry initialization later.
            return false;
        }
    }
    auto& admission = *fresh_salt_admission_;
    if (admission.last_refill_microseconds.has_value()
        && now >= *admission.last_refill_microseconds) {
        const std::uint64_t refills =
            (now - *admission.last_refill_microseconds)
            / FreshSaltAdmission::refill_interval_microseconds;
        if (refills != 0U) {
            admission.tokens = static_cast<std::uint32_t>(
                std::min<std::uint64_t>(FreshSaltAdmission::bucket_capacity,
                    admission.tokens + refills));
            *admission.last_refill_microseconds +=
                refills * FreshSaltAdmission::refill_interval_microseconds;
        }
    } else {
        admission.last_refill_microseconds = now;
    }
    // Identify the request by its salt (the PBKDF2 input), not by the whole
    // payload, so a retried request matches even if its key words differ.
    const auto decoded = decode_key_material(payload);
    std::uint64_t identity = 0xcbf2'9ce4'8422'2325ULL;
    if (decoded) {
        for (const std::byte value : decoded.key_material.salt) {
            identity ^= static_cast<std::uint64_t>(value);
            identity *= 0x0000'0100'0000'01b3ULL;
        }
    }
    // Avalanche the salt hash before slicing it into Bloom-filter positions;
    // adjacent forged salts must not produce correlated high-bit positions.
    identity = (identity ^ (identity >> 30U)) * 0xbf58'476d'1ce4'e5b9ULL;
    identity = (identity ^ (identity >> 27U)) * 0x94d0'49bb'1331'11ebULL;
    identity ^= identity >> 31U;
    // Remember all recent requests, even ones denied by the token bucket.
    // Three time windows preserve a candidate for at least two retry periods;
    // packet-count churn cannot evict it. Work and storage stay fixed.
    const auto epoch = now / FreshSaltAdmission::refill_interval_microseconds;
    if (!admission.salt_epoch.has_value() || epoch < *admission.salt_epoch
        || epoch - *admission.salt_epoch
            >= FreshSaltAdmission::salt_window_count) {
        for (auto& window : admission.salt_windows) {
            window.fill(0U);
        }
    } else {
        for (auto next = *admission.salt_epoch + 1U; next <= epoch; ++next) {
            admission.salt_windows[next % FreshSaltAdmission::salt_window_count]
                .fill(0U);
        }
    }
    admission.salt_epoch = epoch;
    std::array<std::size_t, 4> positions {};
    for (std::size_t index = 0; index < positions.size(); ++index) {
        positions[index] = static_cast<std::size_t>((identity >> (index * 16U))
            & (FreshSaltAdmission::salt_window_bits - 1U));
    }
    const bool repeated = std::any_of(admission.salt_windows.begin(),
        admission.salt_windows.end(), [&](const auto& window) {
            return std::all_of(
                positions.begin(), positions.end(), [&](std::size_t position) {
                    return (window[position / 64U]
                               & (std::uint64_t {1} << (position % 64U)))
                        != 0U;
                });
        });
    auto& current =
        admission.salt_windows[epoch % FreshSaltAdmission::salt_window_count];
    for (const auto position : positions) {
        current[position / 64U] |= std::uint64_t {1} << (position % 64U);
    }
    if (repeated) {
        // A peer retries the same salt until it is answered. Give such a
        // request its own rate-limited lane. Filter false positives may
        // also use this lane; it is a bounded mitigation, not authentication.
        if (admission.last_repeat_derivation_microseconds.has_value()
            && now >= *admission.last_repeat_derivation_microseconds
            && now - *admission.last_repeat_derivation_microseconds
                < FreshSaltAdmission::refill_interval_microseconds) {
            return false;
        }
        admission.last_repeat_derivation_microseconds = now;
        return true;
    }
    if (admission.tokens == 0U) {
        return false;
    }
    --admission.tokens;
    return true;
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
    return submit_datagram(std::span {datagram}.first(encoded.bytes_written),
        {.kind = subtype == key_material_request_subtype
                ? DatagramKind::key_request
                : DatagramKind::other},
        now);
}

void ConnectionRuntime::send_key_material_error_locked(
    CryptoState state, std::uint64_t now) noexcept
{
    // Error replies are unauthenticated and can be provoked by a peer packet.
    // Permit one per normal KM retry interval, including a reply after a
    // backwards clock correction, without treating the request as liveness.
    constexpr std::uint64_t retry_interval_microseconds = 100'000;
    if (last_key_material_error_microseconds_.has_value()
        && now >= *last_key_material_error_microseconds_
        && now - *last_key_material_error_microseconds_
            < retry_interval_microseconds) {
        return;
    }
    last_key_material_error_microseconds_ = now;
    const auto response = encode_key_material_state(state);
    (void)send_key_material(key_material_response_subtype, response, now);
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
    return submit_datagram(std::span {datagram}.first(encoded.bytes_written),
        {.kind = DatagramKind::other}, now);
}

void ConnectionRuntime::observe_key_sequence_position(
    std::uint64_t position, std::uint64_t now) noexcept
{
    if (!key_rate_sample_microseconds_.has_value()
        || now < *key_rate_sample_microseconds_
        || position < key_rate_sample_position_) {
        key_rate_sample_microseconds_ = now;
        key_rate_sample_position_ = position;
        return;
    }
    const auto elapsed = now - *key_rate_sample_microseconds_;
    if (elapsed < 1'000U) {
        return;
    }
    const auto consumed = position - key_rate_sample_position_;
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    const auto scaled =
        consumed > maximum / 1'000'000U ? maximum : consumed * 1'000'000U;
    const auto rate = scaled / elapsed + (scaled % elapsed != 0U ? 1U : 0U);
    key_peak_sequence_rate_ = std::max(key_peak_sequence_rate_, rate);
    key_rate_sample_microseconds_ = now;
    key_rate_sample_position_ = position;
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
    const auto& rtt = session_.rtt();
    // Peer ACK estimates are unauthenticated. Do not let one estimate defer
    // retries beyond half the locally configured peer-idle horizon. Large
    // legitimate RTT profiles can raise that horizon without changing keys.
    const auto retry_ceiling =
        std::max<std::uint64_t>(10'000U, peer_idle_timeout_microseconds_ / 2U);
    const std::uint64_t retry_interval_microseconds = rtt.has_sample()
        ? std::min(retry_ceiling,
              std::max<std::uint64_t>(
                  10'000U, 3ULL * rtt.smoothed_microseconds() / 2ULL))
        : std::min<std::uint64_t>(100'000U, retry_ceiling);
    // Cover two lost attempts, a successful round trip and scheduling/RTT
    // variation. A bounded observed peak also counts TTL/group sequence gaps.
    const std::uint64_t horizon =
        std::min<std::uint64_t>(peer_idle_timeout_microseconds_,
            (rtt.has_sample() ? rtt.smoothed_microseconds() : 100'000U)
                + 2U * retry_interval_microseconds
                + 4ULL * rtt.variation_microseconds() + 10'000U);
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    const auto scaled = key_peak_sequence_rate_ > maximum / horizon
        ? maximum
        : key_peak_sequence_rate_ * horizon;
    crypto_->set_preannouncement_floor(
        scaled / 1'000'000U + (scaled % 1'000'000U != 0U ? 1U : 0U));
    if (crypto_->prepare_rotation() != Error::none) {
        break_locked(0);
        return false;
    }

    const auto key_material = crypto_->pending_key_material();
    if (key_material.empty()) {
        return true;
    }
    // Keep the initial cadence until an RTT observation is available. Once
    // measured, allow a round trip plus half an RTT before repeating KMREQ.
    const bool retry_due = !last_key_material_send_microseconds_.has_value()
        || now < *last_key_material_send_microseconds_
        || now - *last_key_material_send_microseconds_
            >= retry_interval_microseconds;
    if (!retry_due) {
        return true;
    }
    if (!send_key_material(
            key_material_request_subtype, key_material, now)) {
        return false;
    }
    return true;
}

bool ConnectionRuntime::process_reliability_packet_locked(
    const PacketView& packet,
    std::uint64_t now,
    ReliabilityReceiveContext context,
    bool wire_received) noexcept
{
    // Reject oversized DATA before querying delivery state or doing provider
    // work. Invalid input must neither terminate nor advance this session.
    if (packet.kind == PacketKind::data
        && packet.payload.size() > maximum_data_payload_size) {
        return false;
    }
    const std::size_t send_size_before =
        session_.send_buffer().size();
    // Readiness edges observed by blocked receivers and epoll: the next
    // delivery deadline, data readiness and send capacity. A packet that
    // changes none of them (the common in-order DATA arrival behind an
    // earlier pending message) must not wake every waiter.
    const bool readable_before = session_.data_ready_at(now);
    const auto delivery_before = session_.next_receive_delivery_time();
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
        if (options_.enforced_encryption()
            && packet.data.encryption_key != EncryptionKey::none
            && (crypto_ == nullptr || !crypto_->enabled())) {
            statistics_.note_receiver_undecryptable(packet.payload.size());
            return false;
        }
        if (consume_filter_control) {
            // FEC control payloads contain recovery bytes for ciphertext;
            // the outer KK selector is descriptive and does not request a
            // second decryption pass.
        } else if (crypto_ != nullptr && crypto_->enabled()) {
            if (options_.enforced_encryption()
                && packet.data.encryption_key == EncryptionKey::none) {
                statistics_.note_receiver_undecryptable(packet.payload.size());
                return false;
            }
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
                if (packet.data.encryption_key == EncryptionKey::none) {
                    // A plaintext packet cannot change the negotiated
                    // policy or consume a sequence on an encrypted session.
                    return false;
                }
                if (crypto_->receiver_state() == CryptoState::secured) {
                    // The peer's key material may arrive after DATA on a new
                    // selector. Leave the sequence unacknowledged so a later
                    // retransmission can be decrypted and delivered.
                    return false;
                }
                if (options_.enforced_encryption()) {
                    // Missing peer key material does not authenticate this
                    // DATA packet. Preserve the session for a later retry.
                    return false;
                }
                // Optional encryption with a key this side could not unwrap
                // (BADSECRET): acknowledge the sequence, discard the payload.
                context.discard_payload = true;
            } else {
                clear_packet.data.encryption_key = EncryptionKey::none;
                clear_packet.payload = destination;
            }
        } else if (packet.data.encryption_key
            != EncryptionKey::none) {
            // Optional encryption without a local secret (NOSECRET): the peer
            // keeps encrypting. Acknowledge the sequence, discard the payload.
            statistics_.note_receiver_undecryptable(packet.payload.size());
            context.discard_payload = true;
        }
    }

    const SequenceNumber first_unacknowledged =
        session_.send_buffer().first_sequence();
    const auto processed = session_.receive(
        clear_packet, now, context);
    if (!processed) {
        return false;
    }
    if (packet.kind == PacketKind::data && crypto_ != nullptr
        && crypto_->enabled() && !crypto_->authenticated_data_enabled()
        && !consume_filter_control && processed.receiver_packet_accepted_unique
        && packet.data.encryption_key != EncryptionKey::none) {
        // AES-CTR cannot authenticate a packet. Only a sequence the receive
        // window accepted may advance the key-generation ceilings; a spoofed
        // far-ahead sequence would otherwise misroute later packets to a
        // retired key after the next rotation.
        crypto_->note_accepted_receive_sequence(packet.data.sequence);
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
    const bool readable_now = session_.data_ready_at(now);
    if ((readable_before || last_readable_state_) && !readable_now) {
        readiness_source_->note_not_ready(SRT_EPOLL_IN);
    }
    const auto delivery_now = session_.next_receive_delivery_time();
    // Time can make the head readable between the last poll and this packet.
    // Also retain packet-local transitions after an application read drained
    // the queue before the cached state was refreshed by a poll.
    const bool receive_edge = readable_now != last_readable_state_
        || readable_now != readable_before || delivery_now != delivery_before
        || processed.receiver_drop_packets != 0U;
    const bool send_edge = session_.send_buffer().size() < send_size_before;
    bool terminal_edge = false;
    if (packet.kind == PacketKind::control
        && packet.control.type == ControlType::shutdown) {
        peer_closed_ = true;
        broken_ = true;
        terminal_edge = true;
        receive_ready_.notify_all();
        send_ready_.notify_all();
    } else if (packet.kind == PacketKind::control
        && packet.control.type == ControlType::peer_error) {
        peer_error_pending_ = true;
        terminal_edge = true;
        send_ready_.notify_all();
    } else if (receive_edge) {
        // A new or earlier delivery deadline, newly readable data, or a
        // deferred DROPREQ gives blocked receivers a new wake-up point.
        receive_ready_.notify_all();
    }
    if (send_edge) {
        send_ready_.notify_all();
    }
    statistics_.update_send_duration(
        now, session_.send_buffer().size() != 0U);
    last_readable_state_ = readable_now;
    if (receive_edge || send_edge || terminal_edge) {
        notify_readiness();
    }
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
            if (packet.control.subtype == key_material_request_subtype) {
                // Setup may have discarded an unusable crypto session even
                // though a secret was configured. Keep its failure reason
                // when the peer retries KMREQ; absence of a session alone
                // does not mean absence of a passphrase.
                if (receiver_key_state_ != CryptoState::bad_secret
                    && receiver_key_state_ != CryptoState::bad_crypto_mode) {
                    receiver_key_state_ = CryptoState::no_secret;
                }
                send_key_material_error_locked(receiver_key_state_, now);
            }
            return;
        }
        if (packet.control.subtype
            == key_material_request_subtype) {
            // A fresh salt requires PBKDF2 under this runtime lock. Bound
            // that work; rotations on the validated cached salt remain
            // available during a flood.
            if (crypto_->needs_receive_key_derivation(packet.payload)
                && !admit_fresh_salt_key_request(packet.payload, now)) {
                return;
            }
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
                if (!options_.enforced_encryption()
                    && crypto_->sending_without_peer_key()) {
                    // A peer may retry the rejected handshake key or offer a
                    // successor. Reply with the current receive failure, but
                    // keep the independent local sender and its key budget.
                    send_key_material_error_locked(
                        crypto_->receiver_state(), now);
                    return;
                }
                if (crypto_->receiver_state() == CryptoState::bad_secret
                    || crypto_->receiver_state()
                        == CryptoState::bad_crypto_mode) {
                    send_key_material_error_locked(
                        crypto_->receiver_state(), now);
                }
                return;
            }
            if (!send_key_material(
                    key_material_response_subtype,
                    crypto_->key_material_response(), now)) {
                break_locked(0);
                return;
            }
        } else {
            // Once optional setup selected local-only encryption, delayed
            // KMRSPs cannot confirm a key or change that decision. In
            // particular, duplicate failure replies must be idempotent and
            // must not restart the sender's key/sequence accounting.
            if (!options_.enforced_encryption()
                && crypto_->sending_without_peer_key()) {
                return;
            }
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
                    // react. During rotation acknowledge_key_material()
                    // preserves SECURING, so unauthenticated stale/failure
                    // responses cannot remove established keys. Even then
                    // the sender keeps encrypting with its own key instead
                    // of releasing plaintext, as the reference does.
                    if (crypto_->continue_without_peer_key(
                            crypto_->sender_state())
                        != Error::none) {
                        break_locked(0);
                        return;
                    }
                    last_peer_activity_microseconds_ = now;
                    notify_readiness();
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
            // delay a new KMREQ until the current retransmission interval
            // elapsed, even though no request for that rotation had been
            // sent. Aggressive but valid refresh rates would then stall
            // Live source pacing and eventually miss TSBPD deadlines.
            last_key_material_send_microseconds_.reset();
        }
        last_peer_activity_microseconds_ = now;
        notify_readiness();
        return;
    }

    if (packet.kind == PacketKind::data
        && fec_decoder_active()) {
        const auto filtered =
            receive_fec_packet(packet);
        if (!filtered) {
            // A malformed FEC source must not hide otherwise acceptable
            // original DATA from the reliability receive window.
            if (packet.data.message_number != 0U
                && process_reliability_packet_locked(packet, now)) {
                last_peer_activity_microseconds_ = now;
            }
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
                notify_readiness();
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

bool ConnectionRuntime::matches_handshake_replay(
    const HandshakeMessage& message, IpEndpoint peer) const noexcept
{
    return peer == peer_
        && handshake_replay_response_.kind == HandshakeActionKind::send
        && message.packet.request == HandshakeRequest::conclusion
        && message.packet.socket_id == peer_socket_id_
        && message.packet.syn_cookie
        == (handshake_replay_peer_cookie_ != 0U
                ? handshake_replay_peer_cookie_
                : handshake_replay_response_.packet.syn_cookie);
}

bool ConnectionRuntime::accepts_handshake_replay(
    const HandshakeMessage& message, IpEndpoint peer) const noexcept
{
    std::lock_guard lock(mutex_);
    return !locally_closed_ && !broken_
        && matches_handshake_replay(message, peer);
}

bool ConnectionRuntime::process_handshake(
    const HandshakeMessage& message,
    IpEndpoint peer) noexcept
{
    std::lock_guard lock(mutex_);
    if (locally_closed_ || broken_
        || !matches_handshake_replay(message, peer)) {
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
    if (!submit_datagram(
            std::span {datagram}.first(encoded.bytes_written), {}, now)) {
        return true;
    }
    notify_readiness();
    return true;
}

RuntimePollResult ConnectionRuntime::poll() noexcept
{
    std::size_t remaining_send_attempts = maximum_send_batch;
    return poll(remaining_send_attempts);
}

ConnectionDatagramInbox::Status ConnectionRuntime::admit_datagram(
    ConnectionDatagramInbox& inbox, ConnectionDatagramInbox::Token token,
    std::span<const std::byte> bytes, IpEndpoint peer,
    std::uint64_t publication_time) noexcept
{
    std::lock_guard lock(mutex_);
    if (locally_closed_ || broken_) {
        return ConnectionDatagramInbox::Status::closed;
    }
    if (peer != peer_) {
        return ConnectionDatagramInbox::Status::invalid;
    }
    // Bounded copy and pure reserved-service notification only. No injected
    // clock or protocol callback executes across this admission fence.
    const auto status =
        inbox.publish_unfenced(token, bytes, peer, publication_time);
    if (status == ConnectionDatagramInbox::Status::exhausted) {
        break_locked(0);
    }
    return status;
}

RuntimePollResult ConnectionRuntime::poll_setup_prefix_deadline(
    std::uint64_t maximum_ingress_wait_microseconds, bool& terminal) noexcept
{
    terminal = false;
    // Preserve prefix polling's nonblocking barrier when another protocol or
    // application operation owns the runtime. Retry on the existing fallback;
    // a callback holding that mutex cannot be preempted by a timeout check.
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        return {.next_work_delay = std::chrono::microseconds {
                    maximum_ingress_wait_microseconds}};
    }
    if (locally_closed_ || peer_closed_ || broken_) {
        terminal = true;
        return {.receive_wait_safe = true};
    }
    const auto now = now_microseconds();
    const auto grace = std::min(
        maximum_ingress_wait_microseconds, peer_idle_timeout_microseconds_);
    const auto idle_limit = peer_idle_timeout_microseconds_ + grace;
    if (now > last_peer_activity_microseconds_
        && now - last_peer_activity_microseconds_ > idle_limit) {
        break_locked(0);
        terminal = true;
        return {.receive_wait_safe = true};
    }
    const auto maximum = (std::numeric_limits<std::uint64_t>::max)();
    const auto deadline = last_peer_activity_microseconds_
        + std::min(idle_limit + 1U, maximum - last_peer_activity_microseconds_);
    const auto remaining = deadline > now ? deadline - now : 0U;
    return {.next_work_delay = std::chrono::microseconds {
                std::min(remaining, maximum_ingress_wait_microseconds)}};
}

RuntimePollResult ConnectionRuntime::poll(std::size_t& remaining_send_attempts,
    const ConnectionDatagramInbox* ingress,
    std::uint64_t maximum_ingress_wait_microseconds,
    bool coordinate_ingress) noexcept
{
    std::lock_guard lock(mutex_);
    poll_send_budget_ = &remaining_send_attempts;
    const auto result = poll_locked(
        ingress, maximum_ingress_wait_microseconds, coordinate_ingress);
    poll_send_budget_ = nullptr;
    return result;
}

RuntimePollResult ConnectionRuntime::poll_locked(
    const ConnectionDatagramInbox* ingress,
    std::uint64_t maximum_ingress_wait_microseconds,
    bool coordinate_ingress) noexcept
{
    if (locally_closed_ || peer_closed_ || broken_) {
        return {.receive_wait_safe = true};
    }
    if (*poll_send_budget_ == 0U) {
        return {.immediate_work = true};
    }
    const std::uint64_t now = now_microseconds();
    // Publication into a bound inbox takes this same runtime mutex, so an
    // accepted copy cannot appear between this snapshot and the timeout check.
    // A popped copy remains in-flight until its protocol handler completes.
    std::uint64_t idle_limit = peer_idle_timeout_microseconds_;
    const bool bound_ingress = ingress != nullptr && ingress->bound_to(this);
    const auto ingress_pending = bound_ingress
        ? ingress->snapshot()
        : ConnectionDatagramInbox::Snapshot {};
    if (bound_ingress) {
        if (ingress_pending.queued != 0 || ingress_pending.in_flight) {
            idle_limit += std::min(maximum_ingress_wait_microseconds,
                peer_idle_timeout_microseconds_);
        }
    }
    if (now > last_peer_activity_microseconds_
        && now - last_peer_activity_microseconds_ > idle_limit) {
        break_locked(0);
        return {};
    }
    if (coordinate_ingress) {
        const auto cohort_wait =
            std::min<std::uint64_t>(maximum_ingress_wait_microseconds,
                maximum_ingress_poll_wait_microseconds);
        const auto incarnation =
            bound_ingress ? ingress->token().incarnation : 0;
        if (!bound_ingress || ingress_pending.closed || cohort_wait == 0
            || poll_ingress_incarnation_ != incarnation) {
            poll_ingress_active_ = false;
            poll_ingress_expired_ = false;
            poll_ingress_incarnation_ = incarnation;
        }
        if (poll_ingress_active_) {
            if (ingress_pending.completed >= poll_ingress_cutoff_) {
                // Run an ordinary poll before capturing any later cohort.
                poll_ingress_active_ = false;
                poll_ingress_expired_ = false;
            } else if (now >= poll_ingress_deadline_) {
                poll_ingress_expired_ = true;
            }
        } else if (bound_ingress && !ingress_pending.closed
            && ingress_pending.admitted != ingress_pending.completed
            && cohort_wait != 0) {
            poll_ingress_cutoff_ = ingress_pending.admitted;
            const auto maximum = (std::numeric_limits<std::uint64_t>::max)();
            poll_ingress_deadline_ = now + std::min(cohort_wait, maximum - now);
            poll_ingress_active_ = true;
            poll_ingress_expired_ = now >= poll_ingress_deadline_;
        }
        if (poll_ingress_active_ && !poll_ingress_expired_) {
            const auto remaining = poll_ingress_deadline_ - now;
            const auto maximum = (std::numeric_limits<std::uint64_t>::max)();
            const auto peer_deadline = last_peer_activity_microseconds_
                + std::min(idle_limit + 1U,
                    maximum - last_peer_activity_microseconds_);
            const auto peer_remaining =
                peer_deadline > now ? peer_deadline - now : 0;
            return {.next_work_delay = std::chrono::microseconds {
                        std::min(remaining, peer_remaining)}};
        }
    }
    if (!service_receiver_tlpktdrop_locked(now)) {
        return {};
    }
    const bool readable_now = session_.data_ready_at(now);
    if (readable_now != last_readable_state_) {
        last_readable_state_ = readable_now;
        if (!readable_now) {
            readiness_source_->note_not_ready(SRT_EPOLL_IN);
        }
        notify_readiness();
    }
    const std::size_t send_size_before_drop =
        session_.send_buffer().size();
    if (!send_actions(session_.drop_expired_sender_message(now), now)
        || !send_actions(session_.drop_too_late_sender(now), now)) {
        return {};
    }
    if (session_.send_buffer().size() < send_size_before_drop) {
        sample_sender_buffer_statistics(now);
        send_ready_.notify_all();
        notify_readiness();
    }
    if (!flush_pending_datagrams(now)) {
        return {};
    }
    if (pending_datagram_size_ != 0U) {
        return pending_send_poll_result(now);
    }
    if (!service_key_rotation(now)
        || !send_actions(session_.poll_timers(now), now)) {
        return {};
    }
    std::size_t pending_drop_requests_sent = 0;
    while (pending_datagram_size_ == 0U
        && pending_drop_requests_sent < maximum_send_batch
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
    if (pending_datagram_size_ != 0U) {
        return pending_send_poll_result(now);
    }
    statistics_.update_send_duration(
        now, session_.send_buffer().size() != 0U);
    sample_receiver_buffer_statistics(now);
    (void)session_.poll_sender_retransmission_timeout(now);

    for (std::size_t index = 0;
        index < maximum_send_batch && *poll_send_budget_ != 0U; ++index) {
        const std::uint64_t packet_time = now_microseconds();
        if (crypto_ != nullptr && crypto_->enabled()) {
            if (!service_key_rotation(packet_time)) {
                return {};
            }
        }
        if (pending_datagram_size_ != 0U) {
            break;
        }
        if (fec_control_ready() && !retransmission_ready(now_microseconds())) {
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
            if (!send_filter_control(packet_time)) {
                return {};
            }
            if (pending_datagram_size_ != 0U) {
                break;
            }
            continue;
        }
        if (crypto_ != nullptr && crypto_->enabled()
            && !retransmission_ready(now_microseconds())) {
            const auto candidate = session_.send_buffer().peek_new_packet();
            if (candidate.has_value()) {
                observe_key_sequence_position(
                    candidate->sequence_position, packet_time);
                // Refresh the hint before accounting for this candidate's
                // skipped positions; preparation may already announce a key.
                if (!service_key_rotation(packet_time)
                    || crypto_->prepare_data_packet(
                           candidate->sequence_position)
                        != Error::none
                    || !service_key_rotation(packet_time)) {
                    break_locked(0);
                    return {};
                }
            }
        }
        // New data needs the next key acknowledgement. A retransmission
        // already owns its original ciphertext and does not depend on it.
        if (crypto_ != nullptr && crypto_->enabled()
            && !crypto_->ready_to_send_data()
            && !retransmission_ready(now_microseconds())) {
            break;
        }
        const bool retransmission = retransmission_ready(now_microseconds());
        if (!retransmission
            && session_.send_buffer().packets_in_flight()
                >= flow_window_packets_) {
            break;
        }
        const std::size_t new_packet_wire_overhead =
            crypto_ != nullptr && crypto_->authenticated_data_enabled()
            ? srt_gcm_authentication_tag_size
            : 0U;
        const auto packet = session_.next_paced_data_packet(pacer_, packet_time,
            new_packet_wire_overhead, true, retransmission);
        if (!packet.has_value()) {
            break;
        }
        if (!send_data(*packet, packet_time)) {
            return {};
        }
        if (pending_datagram_size_ != 0U) {
            break;
        }
    }

    if (pending_datagram_size_ != 0U) {
        return pending_send_poll_result(now_microseconds());
    }
    const bool retransmission = retransmission_ready(now_microseconds());
    const bool pending = session_.has_pending_send_work();
    const bool flow_blocked = !retransmission
        && session_.send_buffer().packets_in_flight()
            >= flow_window_packets_;
    const bool filter_pending =
        fec_control_ready();
    if (session_.has_pending_drop_requests()) {
        return {
            .immediate_work = true,
            .next_work_delay = std::nullopt,
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
    const std::uint64_t current = now_microseconds();
    auto retirement_deadline = session_.next_sender_retirement_deadline();
    const auto receive_gap_deadline =
        session_.next_sensor_receive_gap_deadline();
    if (receive_gap_deadline.has_value()
        && (!retirement_deadline.has_value()
            || *receive_gap_deadline < *retirement_deadline)) {
        retirement_deadline = receive_gap_deadline;
    }
    std::optional<std::chrono::microseconds> retirement_delay;
    if (retirement_deadline.has_value()) {
        if (*retirement_deadline <= current) {
            return {
                .immediate_work = true,
                .next_work_delay = std::nullopt,
            };
        }
        retirement_delay =
            std::chrono::microseconds {*retirement_deadline - current};
    }
#ifdef ENABLE_MAXREXMITBW
    if (!filter_pending && !retransmission
        && session_.has_pending_retransmission()
        && (!session_.send_buffer().peek_new_packet().has_value()
            || flow_blocked || crypto_blocked)) {
        const auto candidate = session_.peek_retransmission_packet();
        if (candidate.has_value()) {
            const auto wait = retransmission_budget_.delay(
                packet_header_size + candidate->payload.size(), current);
            if (wait.has_value() && *wait != 0) {
                return {.next_work_delay = std::chrono::microseconds {*wait},
                    .next_work_deadline = now_function_ == nullptr
                        ? deadline_from_origin_microseconds(
                              origin_, current + *wait)
                        : deadline_after_relative_microseconds(
                              current, current + *wait)};
            }
            if (!wait.has_value())
                return {};
        }
    }
#endif
    if (!paced_work) {
        // Buffered DATA, receive delivery/drop work and pending key exchanges
        // retain the short polling path. Sensor retirement still bounds it.
        if (!session_.idle_for_receive_wait()
            || session_.next_receive_delivery_time().has_value()
            || (crypto_ != nullptr
                && !crypto_->pending_key_material().empty())) {
            return {.next_work_delay = retirement_delay};
        }
        const auto maximum = (std::numeric_limits<std::uint64_t>::max)();
        const auto timeout = last_peer_activity_microseconds_
            + std::min(
                idle_limit + 1U, maximum - last_peer_activity_microseconds_);
        auto deadline =
            std::min(timeout, session_.next_control_deadline(current));
        // Delivered sensor markers and pending DROPREQs can outlive all
        // payloads. Socket readiness alone must not postpone their deadlines.
        if (retirement_deadline.has_value()) {
            deadline = std::min(deadline, *retirement_deadline);
        }
        const auto remaining = deadline > current ? deadline - current : 0U;
        return {.next_work_delay =
                    std::chrono::microseconds {
                        std::min<std::uint64_t>(remaining, 1'000'000U)},
            .receive_wait_safe = true};
    }

    const PaceDecision pace = pacer_.query(current,
        filter_pending || retransmission
            ? 0U
            : session_.send_buffer().packets_in_flight());
    if (pace.ready || pace.next_ready_microseconds <= current) {
        return {
            .immediate_work = true,
            .next_work_delay = std::nullopt,
        };
    }
    const auto next_ready = retirement_deadline.has_value()
        ? std::min(pace.next_ready_microseconds, *retirement_deadline)
        : pace.next_ready_microseconds;
    return {
        .next_work_delay = std::chrono::microseconds {next_ready - current},
        .next_work_deadline = now_function_ == nullptr
            ? deadline_from_origin_microseconds(origin_, next_ready)
            : deadline_after_relative_microseconds(current, next_ready),
    };
}

void ConnectionRuntime::note_not_ready(int events) noexcept
{
    readiness_source_->note_not_ready(events);
    // Group send readiness is assembled from member send capacity. Preserve
    // a member's low epoch on the source watched by group subscriptions too.
    // Group IN epochs belong to its own logical receive cursor instead.
    if (group_readiness_source_ != nullptr && (events & SRT_EPOLL_OUT) != 0) {
        group_readiness_source_->note_not_ready(SRT_EPOLL_OUT);
    }
}

void ConnectionRuntime::notify_readiness() noexcept
{
    // A member's readiness is the group's readiness; a shared group clock
    // can also move another member's delivery deadline, which group watches
    // re-evaluate through the group source. Watches on the other member
    // sockets themselves are refreshed by those members' own events.
    ReadinessSignal::notify(*readiness_source_, group_readiness_source_.get());
}

void ConnectionRuntime::apply_options(
    const SocketOptions& options) noexcept
{
    std::unique_lock lock(mutex_);
    options_ = options;
#ifdef ENABLE_MAXREXMITBW
    const auto limit =
        options
            .get(
                SocketOption::maximum_retransmission_bandwidth_bytes_per_second)
            .value;
    const bool limit_changed = retransmission_budget_.limit() != limit;
    retransmission_budget_.configure(limit, now_microseconds());
#endif
    (void)session_.apply_dynamic_options(options_);
    statistics_.update_reorder_state(
        session_.reorder_distance_packets(),
        session_.reorder_tolerance_packets());
    notify_readiness();
#ifdef ENABLE_MAXREXMITBW
    lock.unlock();
    if (limit_changed)
        notify_channel_send_work();
#endif
}

void ConnectionRuntime::break_locked(int system_error) noexcept
{
    const std::uint64_t now = now_microseconds();
    statistics_.update_send_duration(
        now, session_.send_buffer().size() != 0U);
    statistics_.update_send_duration(now, false);
    broken_ = true;
    pending_datagram_head_.reset();
    pending_datagram_tail_ = nullptr;
    pending_datagram_size_ = 0U;
    next_datagram_retry_microseconds_ = 0U;
    transient_send_failure_since_.reset();
    transient_send_system_error_ = 0;
    system_error_ = system_error;
    receive_ready_.notify_all();
    send_ready_.notify_all();
    notify_readiness();
}

void ConnectionRuntime::notify_channel_send_work() noexcept
{
    if (work_binding_ != nullptr) {
        (void)work_binding_->notify({.send = true});
    }
    if (const auto channel = channel_.lock(); channel != nullptr) {
        channel->notify_send_work();
    }
}

void ConnectionRuntime::notify_channel_receive_release() noexcept
{
    if (work_binding_ != nullptr) {
        (void)work_binding_->notify({.receive_release = true});
    }
    if (const auto channel = channel_.lock(); channel != nullptr) {
        channel->notify_receive_release();
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
    std::unique_lock lock(mutex_);
    if (locally_closed_ || peer_closed_ || broken_) {
        return false;
    }
    const bool accepted =
        send_peer_error_locked(error_code, now_microseconds());
    const bool deferred = pending_datagram_size_ != 0U;
    lock.unlock();
    if (deferred) {
        notify_channel_send_work();
    }
    return accepted;
}

void ConnectionRuntime::close() noexcept
{
    if (begin_close()) {
        finish_close();
    }
}

bool ConnectionRuntime::begin_close() noexcept
{
    std::unique_lock lock(mutex_);
    if (locally_closed_) {
        return false;
    }
    locally_closed_ = true;
    receive_ready_.notify_all();
    send_ready_.notify_all();
    notify_readiness();
    lock.unlock();
    if (work_binding_ != nullptr) {
        work_binding_->retire();
    }
    return true;
}

void close_connection_runtime(const std::shared_ptr<ConnectionRuntime>& runtime,
    std::shared_ptr<RuntimeWorkExecutor> executor,
    std::shared_ptr<ConnectionDatagramDispatcher> retirement) noexcept
{
    if (retirement != nullptr) {
        retirement->retire_and_reclaim();
    }
    if (runtime == nullptr || !runtime->begin_close()) {
        return;
    }
    if (RuntimeScheduler::on_worker_thread()) {
        struct CloseTask {
            std::shared_ptr<ConnectionRuntime> runtime;
            std::shared_ptr<DatagramChannel> channel;
            std::shared_ptr<ConnectionDatagramDispatcher> retirement;
        };
        try {
            auto task = std::make_shared<CloseTask>(CloseTask {
                .runtime = runtime,
                .channel = runtime->channel_.lock(),
                .retirement = std::move(retirement),
            });
            if (executor == nullptr) {
                executor = existing_runtime_work_executor();
            }
            if (executor != nullptr
                && executor->submit({
                       .function =
                           [](void* context) noexcept {
                               static_cast<CloseTask*>(context)
                                   ->runtime->finish_close();
                           },
                       .context = std::move(task),
                   })
                    == RuntimeWorkExecutor::SubmitStatus::accepted) {
                return;
            }
        } catch (...) {
            // Allocation/thread/queue failure cannot strand a closed runtime.
        }
    }
    runtime->finish_close();
}

void ConnectionRuntime::finish_close() noexcept
{
    std::unique_lock lock(mutex_);
    // This caller owns the final FIFO drain. Polls, queued receives, new
    // sends and duplicate close calls must stay quiescent while its retry
    // pauses let other runtimes on the channel's shard make progress.
    const std::uint64_t now = now_microseconds();
    if (!peer_closed_ && !broken_) {
        // A final cumulative ACK must reach UDP before shutdown. Retry local
        // backpressure for at most one ACK interval, using the real clock so
        // an injected/frozen protocol clock cannot stall close indefinitely.
        const auto final_ack = session_.flush_acknowledgement(now);
        bool pending_ack = final_ack.size != 0U;
        for (auto* entry = pending_datagram_head_.get(); entry != nullptr;
            entry = entry->next.get()) {
            pending_ack |= entry->completion.kind == DatagramKind::control
                && entry->completion.control == ControlType::acknowledgement;
        }
        const auto deadline = Clock::now() + std::chrono::milliseconds {10};
        const auto drain = [&]() {
            while (!broken_ && pending_datagram_size_ != 0U
                && Clock::now() < deadline) {
                // Try once before sleeping: a coarse OS timer can consume
                // the whole budget in a nominal 1 ms sleep. Subsequent blocked
                // attempts remain paced using real time, not the test clock.
                next_datagram_retry_microseconds_ = 0U;
                if (!flush_pending_datagrams(now_microseconds())) {
                    return false;
                }
                if (pending_datagram_size_ != 0U) {
                    lock.unlock();
                    if (close_retry_hook_for_testing_ != nullptr) {
                        close_retry_hook_for_testing_(
                            close_retry_context_for_testing_);
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds {1});
                    lock.lock();
                }
            }
            return !broken_ && pending_datagram_size_ == 0U;
        };
        const bool ack_submitted =
            send_actions(final_ack, now) && (!pending_ack || drain());
        if (ack_submitted && !broken_) {
            ReliabilityActions shutdown;
            shutdown.push({.kind = ReliabilityActionKind::shutdown});
            if (send_actions(shutdown, now_microseconds()) && pending_ack) {
                (void)drain();
            }
        }
    }
    statistics_.update_send_duration(
        now, session_.send_buffer().size() != 0U);
    statistics_.update_send_duration(now, false);
    pending_datagram_head_.reset();
    pending_datagram_tail_ = nullptr;
    pending_datagram_size_ = 0U;
    next_datagram_retry_microseconds_ = 0U;
    transient_send_failure_since_.reset();
    transient_send_system_error_ = 0;
    receive_ready_.notify_all();
    send_ready_.notify_all();
    notify_readiness();
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

bool ConnectionRuntime::accepts_datagrams() const noexcept
{
    std::lock_guard lock(mutex_);
    return !locally_closed_ && !broken_;
}

bool ConnectionRuntime::terminal() const noexcept
{
    std::lock_guard lock(mutex_);
    return broken_ || peer_closed_;
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
    // Report end-of-stream only after buffered data has passed its TSBPD
    // gate, using the delivery unit of the selected API.
    const bool buffered_head = options_.message_api()
        ? session_.receive_buffer().has_complete_message()
        : session_.receive_buffer().has_stream_data();
    return peer_closed_ && !buffered_head
        && !session_.next_receive_delivery_time().has_value();
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

SocketReadinessSnapshot ConnectionRuntime::readiness_snapshot(
    bool socket_broken) noexcept
{
    std::lock_guard lock(mutex_);
    SocketReadinessSnapshot readiness {};
    readiness.exists = true;
    // Fatal failures need no receive servicing. SHUTDOWN retains its tail
    // until delivery/drain, using one state observation under this lock.
    if ((socket_broken || broken_) && !peer_closed_) {
        readiness.events = SRT_EPOLL_IN | SRT_EPOLL_OUT | SRT_EPOLL_ERR;
        return readiness;
    }
    const auto now = now_microseconds();
    if (!locally_closed_ && !service_receiver_tlpktdrop_locked(now)
        && !peer_closed_) {
        readiness.events = SRT_EPOLL_IN | SRT_EPOLL_OUT | SRT_EPOLL_ERR;
        return readiness;
    }
    const bool data_ready = !locally_closed_ && session_.data_ready_at(now);
    const auto wakeup = locally_closed_ ? std::nullopt
        : data_ready ? std::optional<Clock::time_point> {Clock::now()}
                     : next_receive_wakeup_locked(now);
    if ((socket_broken || broken_) && peer_closed_) {
        if (wakeup.has_value()) {
            if (data_ready) {
                readiness.events = SRT_EPOLL_IN;
            } else {
                readiness.read_wakeup = wakeup;
            }
        } else {
            readiness.events = SRT_EPOLL_IN | SRT_EPOLL_OUT | SRT_EPOLL_ERR;
        }
        return readiness;
    }
    if (data_ready) {
        readiness.events = SRT_EPOLL_IN;
    } else {
        readiness.read_wakeup = wakeup;
    }
    if (!locally_closed_ && !peer_closed_ && !broken_
        && (peer_error_pending_ || session_.send_buffer().available() != 0U)) {
        readiness.events |= SRT_EPOLL_OUT;
    }
    return readiness;
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
        return (session_.send_buffer().size() == 0U
                   && pending_datagram_size_ == 0U)
            || locally_closed_ || peer_closed_ || broken_;
    };
    if (!complete() && timeout.count() > 0) {
        (void)send_ready_.wait_for(lock, timeout, complete);
    }
    return session_.send_buffer().size() == 0U && pending_datagram_size_ == 0U;
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
    if (crypto_ == nullptr) {
        return receiver_key_state_;
    }
    // A session kept for optional encryption may never have attempted the
    // peer's key material (e.g. a key-length mismatch); report the state
    // recorded during setup then.
    const CryptoState state = crypto_->receiver_state();
    return state == CryptoState::unsecured ? receiver_key_state_ : state;
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
    // An established connection reports the suite actually protecting DATA.
    // Without encryption in both directions (no passphrase, or a peer that
    // cannot decrypt under optional encryption) it reports AUTO (0), never the
    // configured 1 or 2: applications check for 2 after connecting to confirm
    // authenticated encryption.
    if (crypto_ == nullptr || !crypto_->enabled()
        || crypto_->sending_without_peer_key()) {
        return CryptoMode::automatic;
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
        uses_file_congestion_control(options_.congestion_controller())
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
    // Setup identity and replay response are immutable after construction.
    // Reading this key does not enter mutable protocol state or take mutex_.
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
