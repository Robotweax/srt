#include "compat/connection_datagram_inbox.hpp"

#include "compat/transport_runtime.hpp"
#include "robotweax/srt/codec.hpp"

#include <algorithm>
#include <atomic>
#include <limits>
#include <utility>

namespace robotweax::srt::compat {
namespace {
std::atomic<std::uint64_t> next_incarnation {1};

std::uint64_t reserve_incarnation() noexcept
{
    auto value = next_incarnation.load(std::memory_order_relaxed);
    while (value != std::numeric_limits<std::uint64_t>::max()) {
        if (next_incarnation.compare_exchange_weak(
                value, value + 1, std::memory_order_relaxed)) {
            return value;
        }
    }
    return 0;
}

void increment(std::uint64_t& value) noexcept
{
    if (value != std::numeric_limits<std::uint64_t>::max()) {
        ++value;
    }
}
} // namespace

DatagramStorageBudget::DatagramStorageBudget(std::size_t maximum_bytes) noexcept
    : maximum_bytes_(maximum_bytes)
{
}

bool DatagramStorageBudget::reserve(std::size_t bytes) noexcept
{
    std::lock_guard lock(mutex_);
    if (bytes > maximum_bytes_ - reserved_bytes_) {
        return false;
    }
    reserved_bytes_ += bytes;
    return true;
}

void DatagramStorageBudget::release(std::size_t bytes) noexcept
{
    std::lock_guard lock(mutex_);
    reserved_bytes_ -= bytes;
}

std::size_t DatagramStorageBudget::reserved_bytes() const noexcept
{
    std::lock_guard lock(mutex_);
    return reserved_bytes_;
}

struct ConnectionDatagramInbox::Entry {
    DatagramEnvelope envelope;
    std::uint64_t published_at = 0;
    bool data = false;
};

std::optional<std::size_t> ConnectionDatagramInbox::storage_bytes(
    std::size_t capacity) noexcept
{
    if (capacity == 0
        || capacity > std::numeric_limits<std::size_t>::max() / sizeof(Entry)) {
        return std::nullopt;
    }
    return capacity * sizeof(Entry);
}

ConnectionDatagramInbox::ConnectionDatagramInbox(
    std::shared_ptr<DatagramStorageBudget> budget,
    std::weak_ptr<ConnectionWorkBinding> binding, Token token, IpEndpoint peer,
    Configuration configuration, std::unique_ptr<Entry[]> entries,
    std::size_t storage_bytes,
    std::shared_ptr<DatagramStorageBudget> process_budget) noexcept
    : budget_(std::move(budget))
    , process_budget_(std::move(process_budget))
    , binding_(std::move(binding))
    , token_(token)
    , peer_(peer)
    , configuration_(configuration)
    , entries_(std::move(entries))
    , storage_bytes_(storage_bytes)
{
}

std::shared_ptr<ConnectionDatagramInbox> ConnectionDatagramInbox::create(
    const std::shared_ptr<DatagramStorageBudget>& budget,
    const std::shared_ptr<ConnectionWorkBinding>& binding, IpEndpoint peer,
    Configuration configuration,
    const std::shared_ptr<DatagramStorageBudget>& process_budget) noexcept
{
    const auto bytes = storage_bytes(configuration.capacity);
    if (budget == nullptr || binding == nullptr || !binding->token().valid()
        || !bytes.has_value()
        || configuration.control_reserve > configuration.capacity
        || !budget->reserve(*bytes)) {
        return nullptr;
    }
    const auto process = process_budget != budget ? process_budget : nullptr;
    if (process != nullptr && !process->reserve(*bytes)) {
        budget->release(*bytes);
        return nullptr;
    }
    const auto rollback = [&] {
        budget->release(*bytes);
        if (process != nullptr) {
            process->release(*bytes);
        }
    };
    // Keep reservation ownership explicit across array/object/control-block
    // allocation failures. A constructed object's destructor owns its charge.
    bool reservation_owned = true;
    try {
        const auto incarnation = reserve_incarnation();
        if (incarnation == 0) {
            rollback();
            return nullptr;
        }
        auto entries = std::make_unique<Entry[]>(configuration.capacity);
        auto inbox = std::unique_ptr<ConnectionDatagramInbox>(
            new ConnectionDatagramInbox(budget, binding,
                {.incarnation = incarnation, .service = binding->token()}, peer,
                configuration, std::move(entries), *bytes, process));
        reservation_owned = false;
        return std::shared_ptr<ConnectionDatagramInbox>(std::move(inbox));
    } catch (...) {
        if (reservation_owned) {
            rollback();
        }
        return nullptr;
    }
}

ConnectionDatagramInbox::~ConnectionDatagramInbox()
{
    // Physical storage must be gone before making the credit available again.
    entries_.reset();
    if (!snapshot_.storage_released) {
        release_storage_credit();
    }
}

void ConnectionDatagramInbox::release_storage_credit() noexcept
{
    budget_->release(storage_bytes_);
    if (process_budget_ != nullptr) {
        process_budget_->release(storage_bytes_);
    }
}

bool ConnectionDatagramInbox::matches(Token token) const noexcept
{
    return token.incarnation == token_.incarnation
        && token.service.scope == token_.service.scope
        && token.service.shard == token_.service.shard
        && token.service.slot == token_.service.slot
        && token.service.generation == token_.service.generation;
}

void ConnectionDatagramInbox::close_locked() noexcept
{
    snapshot_.closed = true;
    snapshot_.queued = 0;
    snapshot_.data_queued = 0;
    head_ = 0;
}

ConnectionDatagramInbox::Status
ConnectionDatagramInbox::notify_locked() noexcept
{
    const auto binding = binding_.lock();
    if (binding != nullptr
        && binding->notify({.datagrams = true})
            == RuntimeScheduler::SubmitStatus::accepted) {
        return Status::accepted;
    }
    increment(snapshot_.wake_failures);
    close_locked();
    return Status::wake_failed;
}

void ConnectionDatagramInbox::bind_runtime(
    std::weak_ptr<ConnectionRuntime> runtime) noexcept
{
    ingress_identity_ = runtime.lock().get();
    ingress_runtime_ = std::move(runtime);
    ingress_bound_ = true;
}

bool ConnectionDatagramInbox::bound_to(
    const ConnectionRuntime* runtime) const noexcept
{
    // Identity checks must not pin/destroy a foreign runtime under this
    // caller's runtime mutex. Expiry also rejects allocator address reuse.
    return ingress_bound_ && ingress_identity_ == runtime
        && !ingress_runtime_.expired();
}

ConnectionDatagramInbox::Status ConnectionDatagramInbox::publish(Token token,
    std::span<const std::byte> bytes, IpEndpoint peer,
    std::uint64_t now_microseconds) noexcept
{
    if (ingress_bound_) {
        const auto runtime = ingress_runtime_.lock();
        if (runtime == nullptr) {
            close();
            return Status::closed;
        }
        return runtime->admit_datagram(
            *this, token, bytes, peer, now_microseconds);
    }
    return publish_unfenced(token, bytes, peer, now_microseconds);
}

ConnectionDatagramInbox::Status ConnectionDatagramInbox::publish_unfenced(
    Token token, std::span<const std::byte> bytes, IpEndpoint peer,
    std::uint64_t now_microseconds) noexcept
{
    // Decode before classifying admission; a caller cannot label DATA control.
    if (bytes.empty() || bytes.size() > DatagramEnvelope::maximum_size
        || peer != peer_) {
        return Status::invalid;
    }
    const auto decoded = decode_packet(bytes);
    if (!decoded) {
        return Status::invalid;
    }
    const bool data = decoded.packet.kind == PacketKind::data;
    std::lock_guard lock(mutex_);
    if (!matches(token)) {
        return Status::stale;
    }
    if (snapshot_.closed) {
        return Status::closed;
    }
    if (snapshot_.queued == configuration_.capacity
        || (data
            && snapshot_.data_queued
                >= configuration_.capacity - configuration_.control_reserve)) {
        increment(
            data ? snapshot_.data_rejections : snapshot_.control_rejections);
        return Status::full;
    }
    if (ingress_bound_
        && snapshot_.admitted == std::numeric_limits<std::uint64_t>::max()) {
        // A completion fence must never alias an earlier cohort after wrap.
        close_locked();
        return Status::exhausted;
    }
    const auto tail = (head_ + snapshot_.queued) % configuration_.capacity;
    auto& entry = entries_[tail];
    std::copy(bytes.begin(), bytes.end(), entry.envelope.bytes.begin());
    entry.envelope.size = bytes.size();
    entry.envelope.peer = peer;
    entry.published_at = now_microseconds;
    entry.data = data;
    ++snapshot_.queued;
    if (ingress_bound_) {
        ++snapshot_.admitted;
    }
    snapshot_.data_queued += data ? 1U : 0U;
    snapshot_.highwater = std::max(snapshot_.highwater, snapshot_.queued);
    // Notify under the publication mutex, including when already nonempty.
    // A running consumer can never finish its turn across an unseen insertion.
    return notify_locked();
}

ConnectionDatagramInbox::Status ConnectionDatagramInbox::pop(Token token,
    DatagramEnvelope& envelope, std::uint64_t now_microseconds) noexcept
{
    std::lock_guard lock(mutex_);
    if (!matches(token)) {
        return Status::stale;
    }
    if (snapshot_.closed) {
        return Status::closed;
    }
    if (snapshot_.in_flight) {
        return Status::busy;
    }
    if (snapshot_.queued == 0) {
        return Status::empty;
    }
    const auto& entry = entries_[head_];
    envelope = entry.envelope;
    if (now_microseconds >= entry.published_at) {
        snapshot_.maximum_queue_delay = std::max(snapshot_.maximum_queue_delay,
            now_microseconds - entry.published_at);
    }
    snapshot_.data_queued -= entry.data ? 1U : 0U;
    head_ = (head_ + 1U) % configuration_.capacity;
    --snapshot_.queued;
    snapshot_.in_flight = ingress_bound_;
    return Status::accepted;
}

ConnectionDatagramInbox::Status ConnectionDatagramInbox::complete(
    Token token) noexcept
{
    std::lock_guard lock(mutex_);
    if (!matches(token)) {
        return Status::stale;
    }
    if (!snapshot_.in_flight) {
        return Status::invalid;
    }
    snapshot_.in_flight = false;
    ++snapshot_.completed;
    return Status::accepted;
}

bool ConnectionDatagramInbox::reclaim_retired_storage() noexcept
{
    // Multiple off-worker drainers must all observe actual credit return,
    // not only another caller's intent to reclaim the physical ring.
    std::lock_guard reclamation_lock(reclamation_mutex_);
    std::unique_ptr<Entry[]> released;
    {
        std::lock_guard lock(mutex_);
        if (!snapshot_.closed || snapshot_.in_flight) {
            return false;
        }
        if (snapshot_.storage_released) {
            return true;
        }
        released = std::move(entries_);
    }
    // Physical ring destruction precedes credit return, outside inbox locks.
    released.reset();
    release_storage_credit();
    {
        std::lock_guard lock(mutex_);
        snapshot_.storage_released = true;
    }
    return true;
}

ConnectionDatagramInbox::Status ConnectionDatagramInbox::rearm(
    Token token) noexcept
{
    std::lock_guard lock(mutex_);
    if (!matches(token)) {
        return Status::stale;
    }
    if (snapshot_.closed) {
        return Status::closed;
    }
    return snapshot_.queued == 0 ? Status::empty : notify_locked();
}

void ConnectionDatagramInbox::close() noexcept
{
    std::lock_guard lock(mutex_);
    close_locked();
}

ConnectionDatagramInbox::Snapshot
ConnectionDatagramInbox::snapshot() const noexcept
{
    std::lock_guard lock(mutex_);
    return snapshot_;
}

} // namespace robotweax::srt::compat
