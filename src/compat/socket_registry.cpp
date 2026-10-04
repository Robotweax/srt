#include "compat/socket_registry.hpp"
#include "compat/process_owned.hpp"
#include "compat/connect_callback_executor.hpp"

#include "compat/connect_handshake_operation.hpp"
#include "compat/connection.hpp"
#include "compat/epoll.hpp"
#include "compat/group_registry.hpp"
#include "compat/random_identity.hpp"
#include "compat/readiness.hpp"
#include "compat/runtime_scheduler_service.hpp"
#include "compat/runtime_work_executor_service.hpp"
#include "robotweax/srt/crypto_provider.hpp"

#include <chrono>
#include <condition_variable>
#include <limits>
#include <new>
#include <thread>
#include <utility>
#include <vector>

namespace robotweax::srt::compat {
namespace {

constexpr auto deferred_close_poll_interval =
    std::chrono::milliseconds{10};

struct RuntimeLifecycle {
    std::mutex mutex;
    std::condition_variable changed;
    std::uint32_t startup_count = 0;
    bool cleaning = false;
};

thread_local std::uint32_t callback_depth = 0;
thread_local bool cleanup_worker_thread = false;
thread_local bool creation_blocked_by_cleanup = false;

[[nodiscard]] RuntimeLifecycle& runtime_lifecycle()
{
    static ProcessOwned<RuntimeLifecycle> lifecycle;
    return lifecycle.get();
}

struct DeferredClose {
    std::shared_ptr<SocketRecord> record;
    std::shared_ptr<DatagramChannel> channel;
    std::shared_ptr<ConnectionRuntime> runtime;
    std::uint32_t protocol_socket_id = 0;
    std::chrono::steady_clock::time_point deadline{};
};

void publish_group_state(
    const std::shared_ptr<SocketRecord>& record,
    bool broken_connection = false) noexcept
{
    SRTSOCKET group = SRT_INVALID_SOCK;
    SRTSOCKET socket = SRT_INVALID_SOCK;
    std::uint64_t group_generation = 0;
    std::uint64_t member_generation = 0;
    SRT_SOCKSTATUS state = SRTS_NONEXIST;
    int result = SRT_SUCCESS;
    {
        std::lock_guard lock(record->mutex);
        group = record->group_id;
        group_generation = record->group_generation;
        member_generation = record->member_generation;
        socket = static_cast<SRTSOCKET>(
            record->protocol_socket_id);
        state = record->state;
        result = state == SRTS_CONNECTED
            ? SRT_SUCCESS
            : record->connect_error != SRT_SUCCESS
                ? record->connect_error
                : state == SRTS_BROKEN
                    ? SRT_ECONNLOST : SRT_ESCLOSED;
    }
    if (group != SRT_INVALID_SOCK
        && member_generation != 0U) {
        GroupRegistry::instance().update_member(
            group, group_generation, socket,
            member_generation, state, result,
            broken_connection);
    }
}

void finish_close(DeferredClose close) noexcept
{
    auto retirement = close.channel != nullptr && close.runtime != nullptr
        ? close.channel->retire_connection(close.protocol_socket_id)
        : nullptr;
    if (close.runtime != nullptr) {
        close_connection_runtime(close.runtime, {}, std::move(retirement));
    }
    if (close.record != nullptr) {
        std::lock_guard lock(close.record->mutex);
        close.record->state = SRTS_CLOSED;
    }
    if (close.record != nullptr) {
        publish_group_state(close.record);
        SocketRegistry::instance().retire_closed(close.record);
    }
    ReadinessSignal::notify();
}

class DeferredCloseManager {
public:
    ~DeferredCloseManager()
    {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
            accepting_ = false;
        }
        changed_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
        finish_all();
    }

    [[nodiscard]] bool schedule(
        DeferredClose& close) noexcept
    {
        {
            std::lock_guard lock(mutex_);
            if (!accepting_ || stopping_) {
                return false;
            }
            if (!worker_.joinable()) {
                try {
                    worker_ =
                        std::thread([this] { run(); });
                } catch (...) {
                    return false;
                }
            }
            try {
                closes_.push_back(std::move(close));
            } catch (...) {
                return false;
            }
        }
        changed_.notify_one();
        return true;
    }

    [[nodiscard]] std::size_t service(
        std::chrono::steady_clock::time_point now) noexcept
    {
        std::lock_guard service_lock(service_mutex_);
        std::size_t completed = 0;
        for (;;) {
            DeferredClose close;
            bool found = false;
            {
                std::lock_guard lock(mutex_);
                for (auto entry = closes_.begin();
                     entry != closes_.end(); ++entry) {
                    if (now >= entry->deadline
                        || entry->runtime == nullptr
                        || entry->runtime->wait_for_send_drain(
                            std::chrono::milliseconds{0})) {
                        close = std::move(*entry);
                        closes_.erase(entry);
                        found = true;
                        break;
                    }
                }
            }
            if (!found) {
                return completed;
            }
            finish_close(std::move(close));
            ++completed;
        }
    }

    void pause() noexcept
    {
        std::lock_guard lock(mutex_);
        accepting_ = false;
    }

    void resume() noexcept
    {
        std::lock_guard lock(mutex_);
        if (!stopping_) {
            accepting_ = true;
        }
    }

    void finish_all() noexcept
    {
        std::lock_guard service_lock(service_mutex_);
        for (;;) {
            DeferredClose close;
            {
                std::lock_guard lock(mutex_);
                if (closes_.empty()) {
                    return;
                }
                close = std::move(closes_.back());
                closes_.pop_back();
            }
            finish_close(std::move(close));
        }
    }

private:
    void run() noexcept
    {
        std::unique_lock lock(mutex_);
        while (!stopping_) {
            if (closes_.empty()) {
                changed_.wait(lock, [this] {
                    return stopping_
                        || !closes_.empty();
                });
            } else {
                (void)changed_.wait_for(
                    lock, deferred_close_poll_interval,
                    [this] { return stopping_; });
            }
            if (stopping_) {
                break;
            }
            lock.unlock();
            (void)service(
                std::chrono::steady_clock::now());
            lock.lock();
        }
    }

    std::mutex mutex_;
    std::mutex service_mutex_;
    std::condition_variable changed_;
    std::vector<DeferredClose> closes_;
    std::thread worker_;
    bool accepting_ = true;
    bool stopping_ = false;
};

DeferredCloseManager& deferred_close_manager()
{
    static ProcessOwned<DeferredCloseManager> manager;
    return manager.get();
}

void shutdown_record(
    const std::shared_ptr<SocketRecord>& record,
    bool honor_linger, bool broken_connection) noexcept
{
    std::shared_ptr<DatagramChannel> channel;
    std::shared_ptr<ConnectionRuntime> runtime;
    std::shared_ptr<CryptoSession> crypto;
    std::shared_ptr<HandshakeInbox> listener_inbox;
    std::shared_ptr<ListenerRuntime> listener_runtime;
    std::shared_ptr<ConnectHandshakeOperation> connect_handshake_operation;
    std::thread connect_worker;
    std::uint32_t protocol_socket_id = 0;
    bool synchronous_linger = false;
    bool asynchronous_linger = false;
    std::int32_t linger_seconds = 0;
    {
        std::lock_guard lock(record->mutex);
        if (record->state == SRTS_CLOSING
            || record->state == SRTS_CLOSED) {
            return;
        }
        synchronous_linger =
            record->state == SRTS_CONNECTED
            && record->public_options.send_synchronous
            && record->public_options.linger_enabled;
        asynchronous_linger =
            record->state == SRTS_CONNECTED
            && !record->public_options.send_synchronous
            && record->public_options.linger_enabled;
        linger_seconds =
            record->public_options.linger_seconds;
        // Publish a receive-only prefix before this socket disappears from
        // group resolution. The registry rechecks group/member generations;
        // no group metadata lock is held across the bounded runtime copy.
        if (record->group_id != SRT_INVALID_SOCK
            && record->member_generation != 0U) {
            GroupRegistry::instance().retain_member_receive(record->group_id,
                record->group_generation,
                static_cast<SRTSOCKET>(record->protocol_socket_id),
                record->member_generation, record->runtime);
        }
        record->state = SRTS_CLOSING;
        protocol_socket_id = record->protocol_socket_id;
        channel = std::move(record->channel);
        runtime = std::move(record->runtime);
        crypto = std::move(record->crypto);
        listener_inbox = std::move(record->listener_inbox);
        listener_runtime = std::move(record->listener_runtime);
        connect_handshake_operation =
            std::move(record->connect_handshake_operation);
        default_crypto_provider().secure_erase(
            record->listener_cookie_secret);
        connect_worker = std::move(record->connect_worker);
        record->pending_accepts.store(0U, std::memory_order_release);
    }
    publish_group_state(record, broken_connection);
    // A closed socket is no longer a member, even while its runtime lingers.
    // Publish failure first so UPDATE remains observable for surviving paths.
    SRTSOCKET group_id = SRT_INVALID_SOCK;
    std::uint64_t group_generation = 0;
    std::uint64_t member_generation = 0;
    {
        std::lock_guard lock(record->mutex);
        group_id = record->group_id;
        group_generation = record->group_generation;
        member_generation = record->member_generation;
    }
    if (group_id != SRT_INVALID_SOCK && member_generation != 0U) {
        GroupRegistry::instance().remove_member(group_id, group_generation,
            static_cast<SRTSOCKET>(protocol_socket_id), member_generation);
    }

    if (listener_inbox != nullptr) {
        listener_inbox->close();
    }
    if (channel != nullptr && listener_inbox != nullptr) {
        channel->clear_listener_inbox(listener_inbox);
    }
    if (listener_runtime != nullptr) {
        listener_runtime->close();
    }
    if (connect_handshake_operation != nullptr) {
        connect_handshake_operation->close();
        if (!connect_handshake_operation->running_on_current_thread()) {
            connect_handshake_operation->wait();
        }
    }
    if (connect_worker.joinable()) {
        if (connect_worker.get_id()
            == std::this_thread::get_id()) {
            connect_worker.detach();
        } else {
            connect_worker.join();
        }
    }
    {
        // Setup workers have been cancelled/joined (or this is their own
        // terminal callback). Established runtimes own their crypto state;
        // asynchronous linger does not need the configured passphrase.
        std::lock_guard lock(record->mutex);
        (void)record->native_options.set_passphrase({});
    }
    if (honor_linger
        && runtime != nullptr && synchronous_linger
        && linger_seconds > 0) {
        (void)runtime->wait_for_send_drain(
            std::chrono::seconds{linger_seconds});
    }
    if (honor_linger
        && runtime != nullptr
        && asynchronous_linger
        && linger_seconds > 0
        && !runtime->wait_for_send_drain(
            std::chrono::milliseconds{0})) {
        DeferredClose close{
            .record = record,
            .channel = channel,
            .runtime = runtime,
            .protocol_socket_id = protocol_socket_id,
            .deadline = std::chrono::steady_clock::now()
                + std::chrono::seconds{linger_seconds},
        };
        if (deferred_close_manager().schedule(close)) {
            ReadinessSignal::notify();
            return;
        }
    }
    finish_close({
        .record = record,
        .channel = std::move(channel),
        .runtime = std::move(runtime),
        .protocol_socket_id = protocol_socket_id,
    });
}

} // namespace

SocketRecord::SocketRecord(Purpose purpose)
    : readiness_source(purpose == Purpose::transport
              ? std::make_shared<ReadinessSource>()
              : nullptr)
{
    open_time_microseconds =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    (void)native_options.set(
        SocketOption::maximum_payload_size, SRT_LIVE_DEF_PLSIZE);
}

SocketRegistry& SocketRegistry::instance() noexcept
{
    // Construct teardown dependencies before the registry. Function-local
    // statics are destroyed in reverse construction order, so the registry
    // can close workers, encrypted sessions, and poll records safely during
    // normal process exit even when the application omits srt_cleanup().
    // The crypto provider comes first so that it outlives everything that
    // holds provider objects, including the socket identity generator.
    (void)default_crypto_provider();
    (void)prepare_random_identity();
    (void)deferred_close_manager();
    prepare_runtime_scheduler_service();
    prepare_runtime_work_executor_service();
    prepare_connect_callback_executor();
    epoll_initialize();
    (void)GroupRegistry::instance();
    static ProcessOwned<SocketRegistry> registry;
    return registry.get();
}

SocketRegistry::~SocketRegistry()
{
    clear();
}

SRTSOCKET SocketRegistry::create() noexcept
{
    try {
        auto record = std::make_shared<SocketRecord>();
        std::lock_guard lock(mutex_);
        if (clearing_) {
            return SRT_INVALID_SOCK;
        }

        const auto initial_sequence = random_initial_sequence();
        if (!initial_sequence) {
            return SRT_INVALID_SOCK;
        }
        // The permutation never repeats a handle; the lookup only guards
        // the registry invariant.
        SRTSOCKET candidate = SRT_INVALID_SOCK;
        do {
            candidate =
                next_registry_handle(HandleSpace::socket, next_socket_index_);
            if (candidate == SRT_INVALID_SOCK) {
                return SRT_INVALID_SOCK;
            }
        } while (sockets_.find(candidate) != sockets_.end());
        record->protocol_socket_id = static_cast<std::uint32_t>(candidate);
        record->connection_initial_sequence = *initial_sequence;
        record->peer_connection_initial_sequence = *initial_sequence;
        sockets_.emplace(candidate, std::move(record));
        return candidate;
    } catch (const std::bad_alloc&) {
        return SRT_INVALID_SOCK;
    } catch (...) {
        return SRT_INVALID_SOCK;
    }
}

std::shared_ptr<SocketRecord> SocketRegistry::find(SRTSOCKET socket) noexcept
{
    std::lock_guard lock(mutex_);
    const auto entry = sockets_.find(socket);
    return entry == sockets_.end() ? nullptr : entry->second;
}

std::size_t SocketRegistry::size() noexcept
{
    std::lock_guard lock(mutex_);
    return sockets_.size();
}

SRT_SOCKSTATUS SocketRegistry::state(SRTSOCKET socket) noexcept
{
    std::shared_ptr<SocketRecord> record;
    {
        std::lock_guard lock(mutex_);
        const auto entry = sockets_.find(socket);
        if (entry == sockets_.end()) {
            return closed_handles_.contains(socket) ? SRTS_CLOSED
                                                    : SRTS_NONEXIST;
        }
        record = entry->second;
    }
    return refresh_state(record);
}

SRT_SOCKSTATUS SocketRegistry::refresh_state(
    const std::shared_ptr<SocketRecord>& record) noexcept
{
    if (record == nullptr) {
        return SRTS_NONEXIST;
    }
    SRT_SOCKSTATUS state = SRTS_NONEXIST;
    bool changed = false;
    {
        std::lock_guard lock(record->mutex);
        if (record->state == SRTS_CONNECTED && record->runtime != nullptr
            && record->runtime->terminal()) {
            record->state = SRTS_BROKEN;
            changed = true;
        }
        state = record->state;
    }
    if (changed) {
        publish_group_state(record, true);
    }
    return state;
}

void SocketRegistry::close(SRTSOCKET socket) noexcept
{
    const auto record = find(socket);
    if (record == nullptr) {
        return;
    }
    shutdown_record(record, true, false);
}

void SocketRegistry::close_failed(SRTSOCKET socket) noexcept
{
    const auto record = find(socket);
    if (record == nullptr) {
        return;
    }
    shutdown_record(record, true, true);
}

void SocketRegistry::clear() noexcept
{
    auto& close_manager = deferred_close_manager();
    close_manager.pause();
    std::unordered_map<SRTSOCKET,
        std::shared_ptr<SocketRecord>> records;
    {
        std::lock_guard lock(mutex_);
        clearing_ = true;
        records.swap(sockets_);
        closed_handles_.clear();
    }
    for (const auto& entry : records) {
        shutdown_record(entry.second, false, false);
    }
    close_manager.finish_all();
    {
        std::lock_guard lock(mutex_);
        clearing_ = false;
    }
    close_manager.resume();
    epoll_clear_all();
}

void SocketRegistry::retire_closed(
    const std::shared_ptr<SocketRecord>& record) noexcept
{
    {
        std::lock_guard lock(record->mutex);
        if (record->state != SRTS_CLOSED) {
            return;
        }
    }
    const auto handle = static_cast<SRTSOCKET>(record->protocol_socket_id);
    std::lock_guard lock(mutex_);
    const auto entry = sockets_.find(handle);
    // Final cleanup may already have removed this generation. A delayed
    // completion must not resurrect its status in the next generation.
    if (entry != sockets_.end() && entry->second == record) {
        closed_handles_.remember(handle);
        sockets_.erase(entry);
    }
}

std::size_t service_deferred_closes(
    std::chrono::steady_clock::time_point now) noexcept
{
    return deferred_close_manager().service(now);
}

RuntimeCallbackScope::RuntimeCallbackScope() noexcept
{
    ++callback_depth;
}

RuntimeCallbackScope::~RuntimeCallbackScope()
{
    --callback_depth;
}

void mark_runtime_cleanup_worker_thread() noexcept
{
    cleanup_worker_thread = true;
}

bool runtime_creation_blocked_by_cleanup() noexcept
{
    const bool blocked = creation_blocked_by_cleanup;
    creation_blocked_by_cleanup = false;
    return blocked;
}

bool runtime_start() noexcept
{
    auto& lifecycle = runtime_lifecycle();
    // Finish constructing cleanup dependencies before an application RAII
    // owner finishes its constructor. Creating the first socket later must
    // not register those destructors after the owner's cleanup destructor.
    // These owners are dormant: no socket or worker is started here.
    (void)SocketRegistry::instance();
    std::unique_lock lifecycle_lock(lifecycle.mutex);
    if (lifecycle.cleaning && (callback_depth != 0U || cleanup_worker_thread)) {
        return false;
    }
    lifecycle.changed.wait(lifecycle_lock, [&] {
        return !lifecycle.cleaning;
    });
    if (lifecycle.startup_count
        != std::numeric_limits<std::uint32_t>::max()) {
        ++lifecycle.startup_count;
    }
    return true;
}

SRTSOCKET runtime_create_socket() noexcept
{
    creation_blocked_by_cleanup = false;
    auto& lifecycle = runtime_lifecycle();
    std::unique_lock lifecycle_lock(lifecycle.mutex);
    if (lifecycle.cleaning && (callback_depth != 0U || cleanup_worker_thread)) {
        creation_blocked_by_cleanup = true;
        return SRT_INVALID_SOCK;
    }
    lifecycle.changed.wait(lifecycle_lock, [&] {
        return !lifecycle.cleaning;
    });
    if (lifecycle.startup_count == 0U) {
        lifecycle.startup_count = 1U;
    }
    return SocketRegistry::instance().create();
}

SRTSOCKET runtime_create_group(SRT_GROUP_TYPE type) noexcept
{
    creation_blocked_by_cleanup = false;
    auto& lifecycle = runtime_lifecycle();
    (void)SocketRegistry::instance();
    std::unique_lock lifecycle_lock(lifecycle.mutex);
    if (lifecycle.cleaning && (callback_depth != 0U || cleanup_worker_thread)) {
        creation_blocked_by_cleanup = true;
        return SRT_INVALID_SOCK;
    }
    lifecycle.changed.wait(lifecycle_lock, [&] {
        return !lifecycle.cleaning;
    });
    if (lifecycle.startup_count == 0U) {
        lifecycle.startup_count = 1U;
    }
    return GroupRegistry::instance().create(type);
}

int runtime_create_epoll() noexcept
{
    creation_blocked_by_cleanup = false;
    auto& lifecycle = runtime_lifecycle();
    (void)SocketRegistry::instance();
    std::unique_lock lifecycle_lock(lifecycle.mutex);
    if (lifecycle.cleaning && (callback_depth != 0U || cleanup_worker_thread)) {
        creation_blocked_by_cleanup = true;
        return SRT_ERROR;
    }
    lifecycle.changed.wait(lifecycle_lock, [&] {
        return !lifecycle.cleaning;
    });
    if (lifecycle.startup_count == 0U) {
        lifecycle.startup_count = 1U;
    }
    return epoll_create();
}

void runtime_cleanup() noexcept
{
    auto& lifecycle = runtime_lifecycle();
    std::shared_ptr<ConnectCallbackExecutor> retired_callbacks;
    std::unique_lock lifecycle_lock(lifecycle.mutex);
    if (lifecycle.cleaning) {
        // A callback or worker retiring under final cleanup may call cleanup
        // again. There is no generation reference left for it to release.
        return;
    }
    if (lifecycle.startup_count == 0U) {
        return;
    }
    --lifecycle.startup_count;
    if (lifecycle.startup_count == 0U) {
        lifecycle.cleaning = true;
        lifecycle_lock.unlock();
        GroupRegistry::instance().clear();
        SocketRegistry::instance().clear();
        stop_runtime_work_executor();
        retired_callbacks = retire_connect_callback_executor();
        stop_runtime_scheduler();
        // The callback executor has been detached from the service. Its
        // workers may run application TLS destructors that start a new
        // generation, so finish their joins after reopening admission.
        lifecycle_lock.lock();
        lifecycle.cleaning = false;
        lifecycle_lock.unlock();
        lifecycle.changed.notify_all();
        if (retired_callbacks) {
            retired_callbacks->stop();
        }
    }
}

} // namespace robotweax::srt::compat
