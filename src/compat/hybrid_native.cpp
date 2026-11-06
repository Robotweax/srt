// SPDX-License-Identifier: MIT
#include "compat/hybrid_native.hpp"
#include "compat/transport_runtime.hpp"
#include "compat/runtime_scheduler_service.hpp"
#include <algorithm>
#include <limits>

namespace robotweax::srt::compat::hybrid {
bool native_candidate(const PacketView& p) noexcept
{
    if (p.kind != PacketKind::control
        || p.control.type != ControlType::user_defined)
        return false;
    if (p.control.subtype == exchange_subtype
        || p.control.subtype == control_subtype)
        return true;
    if (p.control.subtype != authenticated_control_subtype
        || p.payload.size() < 12 + packet_header_size)
        return false;
    const auto inner = decode_packet(p.payload.subspan(12,
        p.payload.size() >= 44 ? p.payload.size() - 44
                               : p.payload.size() - 12));
    return inner && inner.packet.kind == PacketKind::control
        && inner.packet.control.type == ControlType::user_defined
        && inner.packet.control.subtype == control_subtype;
}
bool decode_exchange_packet(const PacketView& p, ExchangeRecord& r) noexcept
{
    if (p.kind != PacketKind::control
        || p.control.type != ControlType::user_defined
        || p.control.subtype != exchange_subtype || p.control.type_specific
        || !decode(p.payload, r))
        return false;
    const bool caller =
        r.kind == ExchangeKind::offer || r.kind == ExchangeKind::proof;
    return p.control.destination_socket_id
        == (caller ? r.listener_socket : r.caller_socket);
}
bool encode_exchange_packet(std::span<const std::byte> body,
    PacketTimestamp timestamp, std::uint32_t destination,
    std::span<std::byte> out) noexcept
{
    ExchangeRecord r;
    if (out.size() != maximum_native_control_size || !decode(body, r))
        return false;
    const bool caller =
        r.kind == ExchangeKind::offer || r.kind == ExchangeKind::proof;
    if (destination != (caller ? r.listener_socket : r.caller_socket))
        return false;
    std::array<std::byte, maximum_native_control_size> wire {};
    if (!encode_packet({.kind = PacketKind::control,
                           .control = {.type = ControlType::user_defined,
                               .subtype = exchange_subtype,
                               .timestamp = timestamp,
                               .destination_socket_id = destination},
                           .payload = body},
            wire))
        return false;
    std::copy(wire.begin(), wire.end(), out.begin());
    return true;
}

#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_HYBRID_CONTROL
namespace {
std::uint64_t add(std::uint64_t a, std::uint64_t b) noexcept
{
    return b > UINT64_MAX - a ? UINT64_MAX : a + b;
}
bool timed(Kind k) noexcept
{
    return k == Kind::stream_progress || k == Kind::path_report
        || k == Kind::multicast_ready;
}
struct ProtocolScope {
    NativeService* owner;
    ProtocolScope* previous;
    static thread_local ProtocolScope* top;
    explicit ProtocolScope(NativeService* s)
        : owner(s)
        , previous(top)
    {
        top = this;
    }
    ~ProtocolScope()
    {
        top = previous;
    }
    static bool owns(NativeService* s)
    {
        for (auto* p = top; p; p = p->previous)
            if (p->owner == s)
                return true;
        return false;
    }
};
thread_local ProtocolScope* ProtocolScope::top = nullptr;
}
static_assert(sizeof(NativeService) < 64 * 1024);
bool NativeService::valid(const Configuration& c) noexcept
{
    return c.conclusions
        && c.conclusions->matches(c.caller, c.local_socket, c.peer_socket)
        && c.exchange.dispatch == c.conclusions->dispatch()
        && c.exchange.source == c.conclusions->source()
        && c.exchange.connection_generation && c.callbacks.owner
        && c.callbacks.resolve && c.callbacks.revalidate && c.callbacks.enroll
        && c.transport_owner && c.bound && c.transmit;
}
NativeService::NativeService(Configuration c, CryptoProvider& provider)
    : configuration_(std::move(c))
    , exchange_(provider, configuration_.caller)
    , work_(std::make_shared<Work>())
{
    state_.generation = configuration_.exchange.connection_generation;
}
std::shared_ptr<NativeService> NativeService::create(Configuration c,
    std::shared_ptr<RuntimeScheduler> scheduler,
    CryptoProvider& provider) noexcept
{
    if (!scheduler || c.clock || !valid(c))
        return {};
    try {
        auto s = std::shared_ptr<NativeService>(
            new NativeService(std::move(c), provider));
        s->work_->owner = s;
        auto reserved = scheduler->reserve_service(
            s->configuration_.affinity, {run, s->work_});
        if (reserved.status != RuntimeScheduler::SubmitStatus::accepted)
            return {};
        s->scheduler_ = std::move(scheduler);
        s->service_ = reserved.token;
        return s;
    } catch (...) {
        return {};
    }
}
std::shared_ptr<NativeService> NativeService::create_for_testing(
    Configuration c, CryptoProvider& provider) noexcept
{
    if (!valid(c))
        return {};
    try {
        auto s = std::shared_ptr<NativeService>(
            new NativeService(std::move(c), provider));
        s->manual_ = true;
        return s;
    } catch (...) {
        return {};
    }
}
NativeService::~NativeService()
{
    close();
}
std::uint64_t NativeService::monotonic_now() noexcept
{
    const auto n = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch())
                       .count();
    return static_cast<std::uint64_t>(std::max<std::int64_t>(0, n));
}
std::uint64_t NativeService::now() const noexcept
{
    return configuration_.clock
        ? configuration_.clock(configuration_.clock_context)
        : monotonic_now();
}
bool NativeService::wake() noexcept
{
    if (manual_)
        return !closed();
    if (scheduler_->notify_service(service_)
        == RuntimeScheduler::SubmitStatus::accepted)
        return true;
    close(Failure::scheduler);
    return false;
}
bool NativeService::activate() noexcept
{
    {
        std::lock_guard lock(queue_mutex_);
        if (activated_ || closed())
            return false;
        activated_ = true;
    }
    return wake();
}
void NativeService::cleanup() noexcept
{
    if (session_)
        session_->close();
    exchange_.close();
    if (scheduler_)
        (void)scheduler_->release_service(service_);
}
void NativeService::close(Failure reason) noexcept
{
    cancelled_.store(true);
    {
        std::lock_guard lock(queue_mutex_);
        state_.closed = true;
        state_.active = false;
        if (state_.failure == Failure::none)
            state_.failure = reason;
        size_ = 0;
        event_size_ = 0;
        state_.queued = 0;
        state_.events = 0;
    }
    // Never wait for a credential callback. The active worker observes cancel
    // before publication and erases its keys on exit.
    if (ProtocolScope::owns(this))
        return;
    std::unique_lock protocol(protocol_mutex_, std::try_to_lock);
    if (protocol.owns_lock()) {
        ProtocolScope scope(this);
        cleanup();
    }
}
bool NativeService::enqueue(const PacketView& p, std::uint64_t arrival) noexcept
{
    if (p.kind != PacketKind::control
        || p.control.type != ControlType::user_defined
        || p.control.type_specific
        || p.control.destination_socket_id != configuration_.local_socket)
        return false;
    if (p.control.subtype == exchange_subtype) {
        ExchangeRecord r;
        if (!decode_exchange_packet(p, r))
            return false;
        const bool sender =
            r.kind == ExchangeKind::offer || r.kind == ExchangeKind::proof;
        if (sender == configuration_.caller)
            return false;
    } else if (p.control.subtype != authenticated_control_subtype
        || !native_candidate(p)
        || p.payload.size() + packet_header_size
            != packet_header_size + message_size
                + authenticated_control_overhead)
        return false;
    Job job;
    job.packet = true;
    job.arrival = arrival;
    auto encoded = encode_packet(
        {.kind = p.kind, .control = p.control, .payload = p.payload},
        job.bytes);
    if (!encoded)
        return false;
    job.size = encoded.bytes_written;
    {
        std::lock_guard lock(queue_mutex_);
        if (closed() || !activated_)
            return false;
        if (size_ == capacity) {
            ++state_.dropped;
            return false;
        }
        jobs_[(head_ + size_) % capacity] = job;
        ++size_;
        state_.queued = size_;
    }
    return wake();
}
NativeService::Admission NativeService::send(const Message& m,
    std::uint64_t deadline, std::optional<std::uint64_t> observed) noexcept
{
    const auto at = now();
    if (at >= expires_at_.load()) {
        close(Failure::exchange);
        return Admission::closed;
    }
    {
        std::lock_guard lock(queue_mutex_);
        if (closed())
            return Admission::closed;
        if (!state_.active)
            return Admission::not_ready;
        if (!hybrid::valid(m, state_.context)
            || sender_is_caller(m.kind) != configuration_.caller
            || deadline <= at || deadline > add(at, maximum_age_us)
            || (timed(m.kind) ? (!observed || *observed > at || m.age_us != 0)
                              : observed.has_value()))
            return Admission::invalid;
        if (size_ == capacity)
            return Admission::full;
        Job job;
        job.message = m;
        job.not_after = deadline;
        job.observed = observed;
        jobs_[(head_ + size_) % capacity] = job;
        ++size_;
        state_.queued = size_;
    }
    return wake() ? Admission::accepted : Admission::closed;
}
bool NativeService::pop(Event& out) noexcept
{
    if (now() >= expires_at_.load()) {
        close(Failure::exchange);
        return false;
    }
    std::lock_guard lock(queue_mutex_);
    if (closed() || !event_size_)
        return false;
    out = events_[event_head_];
    event_head_ = (event_head_ + 1) % event_capacity;
    --event_size_;
    state_.events = event_size_;
    return true;
}
NativeService::Snapshot NativeService::snapshot() const noexcept
{
    std::lock_guard lock(queue_mutex_);
    auto state = state_;
    if (now() >= state.expires_at_us) {
        state.active = false;
        state.closed = true;
        if (state.failure == Failure::none)
            state.failure = Failure::exchange;
    }
    return state;
}
void NativeService::publish(Event e) noexcept
{
    bool full = false;
    {
        std::lock_guard lock(queue_mutex_);
        if (closed())
            return;
        if (event_size_ == event_capacity)
            full = true;
        else {
            e.generation = state_.generation;
            events_[(event_head_ + event_size_) % event_capacity] = e;
            ++event_size_;
            state_.events = event_size_;
        }
    }
    if (full)
        close(Failure::events_full);
}
bool NativeService::live() noexcept
{
    if (closed())
        return false;
    if (!configuration_.bound(configuration_.transport_owner.get(), this)) {
        close(Failure::lifetime);
        return false;
    }
    if (started_) {
        const auto before = now();
        if (!exchange_.revalidate(before)) {
            close(Failure::exchange);
            return false;
        }
        const auto after = now();
        const auto* grant = exchange_.authorization();
        if (after < before
            || (exchange_.state() != Exchange::State::active
                && after >= setup_deadline_)
            || (grant && after >= grant->expires_at_us)) {
            close(Failure::exchange);
            return false;
        }
        // Revalidation is user code. Recheck connection binding after it.
        if (!closed()
            && !configuration_.bound(
                configuration_.transport_owner.get(), this)) {
            close(Failure::lifetime);
            return false;
        }
    }
    return !closed();
}
bool NativeService::ensure_active() noexcept
{
    if (exchange_.state() != Exchange::State::active)
        return true;
    if (session_)
        return session_->ready();
    const auto* grant = exchange_.authorization();
    if (!grant
        || !configuration_.callbacks.enroll(
            configuration_.callbacks.owner.get(), exchange_.context(), *grant,
            state_.generation)) {
        close(Failure::enrollment);
        return false;
    }
    if (!live())
        return false;
    session_.emplace(*exchange_.authentication(), exchange_.context(),
        configuration_.caller);
    if (!session_->ready()) {
        close(Failure::exchange);
        return false;
    }
    {
        std::lock_guard lock(queue_mutex_);
        if (closed())
            return false;
        state_.active = true;
        state_.context = exchange_.context();
        state_.expires_at_us = grant->expires_at_us;
        expires_at_.store(grant->expires_at_us);
    }
    return true;
}
void NativeService::transmit(std::span<const std::byte> bytes,
    std::uint64_t deadline, const Message* m) noexcept
{
    if (!live())
        return;
    if (const auto* g = exchange_.authorization())
        deadline = std::min(deadline, g->expires_at_us);
    if (now() >= deadline) {
        if (m)
            publish({.kind = Event::Kind::expired, .message = *m});
        return;
    }
    const auto result = configuration_.transmit(
        configuration_.transport_owner.get(), this, bytes, deadline);
    if (closed())
        return;
    if (now() >= deadline) {
        if (m)
            publish({.kind = Event::Kind::expired, .message = *m});
        (void)live();
        return;
    }
    const bool retry = result.bytes_transferred == 0
        && (result.error == Error::would_block
            || (result.error == Error::io_error
                && UdpSocket::is_transient_send_error(result.system_error)));
    if (result && result.bytes_transferred == bytes.size()) {
        {
            std::lock_guard lock(queue_mutex_);
            ++state_.submitted;
        }
        if (m && live())
            publish({.kind = Event::Kind::submitted, .message = *m});
    } else if (retry) {
        {
            std::lock_guard lock(queue_mutex_);
            ++state_.retryable;
        }
        if (m)
            publish({.kind = Event::Kind::retryable, .message = *m});
    } else
        close(Failure::transport);
}
void NativeService::handle(const Exchange::Step& step) noexcept
{
    if (step.status == Exchange::Status::closed) {
        close(Failure::exchange);
        return;
    }
    if (!live() || !ensure_active() || !step.has_output)
        return;
    std::array<std::byte, maximum_native_control_size> wire {};
    const auto at = now();
    const auto stamp = at >= configuration_.timestamp_origin_us
        ? at - configuration_.timestamp_origin_us
        : 0;
    if (!encode_exchange_packet(step.output,
            PacketTimestamp {static_cast<std::uint32_t>(stamp)},
            configuration_.peer_socket, wire)) {
        close(Failure::exchange);
        return;
    }
    transmit(wire, setup_deadline_, nullptr);
}
void NativeService::process(const Job& job) noexcept
{
    if (!live())
        return;
    if (job.packet) {
        const auto p = decode_packet(std::span {job.bytes}.first(job.size));
        if (!p)
            return;
        if (p.packet.control.subtype == exchange_subtype) {
            handle(exchange_.receive(p.packet.payload, now()));
            return;
        }
        if (!session_)
            return;
        auto result = session_->receive(p.packet, job.arrival);
        if (result.status == Session::Status::accepted
            || result.status == Session::Status::duplicate) {
            if (!live())
                return;
            {
                std::lock_guard lock(queue_mutex_);
                ++state_.received;
            }
            publish({.kind = Event::Kind::received,
                .received = result,
                .message = result.message});
        } else {
            std::lock_guard lock(queue_mutex_);
            ++state_.rejected;
        }
        return;
    }
    if (!session_)
        return;
    auto m = job.message;
    const auto at = now();
    if (at >= job.not_after) {
        publish({.kind = Event::Kind::expired, .message = m});
        return;
    }
    if (timed(m.kind)) {
        const auto index = m.kind == Kind::stream_progress ? 0U
            : m.kind == Kind::path_report                  ? 1U
                                                           : 2U;
        if (m.serial <= last_report_[index] || !job.observed
            || at < *job.observed || at - *job.observed > maximum_age_us) {
            publish({.kind = Event::Kind::rejected, .message = m});
            return;
        }
        m.age_us = static_cast<std::uint32_t>(at - *job.observed);
        last_report_[index] = m.serial;
    }
    std::array<std::byte, maximum_native_control_size> wire {};
    std::size_t written = 0;
    const auto stamp = at >= configuration_.timestamp_origin_us
        ? at - configuration_.timestamp_origin_us
        : 0;
    const auto result = session_->seal(
        m, PacketTimestamp {static_cast<std::uint32_t>(stamp)}, wire, written);
    if (result != Session::Status::accepted
        && result != Session::Status::duplicate) {
        publish({.kind = Event::Kind::rejected, .message = m});
        return;
    }
    transmit(std::span {wire}.first(written), job.not_after, &m);
}
void NativeService::run(void* opaque) noexcept
{
    if (auto owner = static_cast<Work*>(opaque)->owner.lock())
        owner->pump();
}
void NativeService::pump_for_testing() noexcept
{
    if (manual_ && !ProtocolScope::owns(this))
        pump();
}
void NativeService::pump() noexcept
{
    std::lock_guard protocol(protocol_mutex_);
    ProtocolScope scope(this);
    if (closed()) {
        cleanup();
        return;
    }
    if (!started_) {
        bool active = false;
        {
            std::lock_guard lock(queue_mutex_);
            active = activated_;
        }
        if (!active)
            return;
        const auto at = now();
        setup_deadline_ = add(at, configuration_.exchange.timeout_us);
        const auto& c = configuration_.callbacks;
        auto step = exchange_.start(configuration_.exchange,
            {.context = c.owner.get(),
                .resolve = c.resolve,
                .revalidate = c.revalidate},
            configuration_.conclusions->caller_payload(),
            configuration_.conclusions->listener_payload(), at);
        started_ = true;
        handle(step);
    }
    for (std::size_t i = 0; i < batch && !closed(); ++i) {
        Job job;
        bool found = false;
        {
            std::lock_guard lock(queue_mutex_);
            if (size_) {
                job = jobs_[head_];
                head_ = (head_ + 1) % capacity;
                --size_;
                state_.queued = size_;
                found = true;
            }
        }
        if (!found)
            break;
        process(job);
    }
    if (!closed())
        handle(exchange_.tick(now()));
    if (closed()) {
        cleanup();
        return;
    }
    if (!manual_) {
        const auto deadline = exchange_.next_deadline();
        if (deadline != UINT64_MAX) {
            using Clock = std::chrono::steady_clock;
            const auto max =
                std::chrono::duration_cast<std::chrono::microseconds>(
                    Clock::duration::max())
                    .count();
            const auto at = deadline > static_cast<std::uint64_t>(max)
                ? Clock::time_point::max()
                : Clock::time_point {
                      std::chrono::duration_cast<Clock::duration>(
                          std::chrono::microseconds {deadline})};
            if (scheduler_->schedule_service_at(service_, at)
                != RuntimeScheduler::SubmitStatus::accepted)
                close(Failure::scheduler);
        }
        bool more = false;
        {
            std::lock_guard lock(queue_mutex_);
            more = size_ != 0;
        }
        if (more)
            (void)wake();
    }
    if (closed())
        cleanup();
}

std::shared_ptr<NativeService> attach_native_control(
    const std::shared_ptr<ConnectionRuntime>& runtime,
    NativeOptions options) noexcept
{
    if (!runtime)
        return {};
    NativeService::Configuration c;
    {
        std::lock_guard lock(runtime->mutex_);
        if (runtime->locally_closed_ || runtime->broken_
            || runtime->peer_closed_ || runtime->peer_error_pending_
            || runtime->hybrid_attempted_ || !runtime->hybrid_conclusions_)
            return {};
        c.caller = runtime->hybrid_caller_;
        c.local_socket = runtime->hybrid_local_socket_id_;
        c.peer_socket = runtime->peer_socket_id_;
        c.conclusions = runtime->hybrid_conclusions_;
        c.exchange.established_peer = runtime->peer_;
        c.timestamp_origin_us =
            static_cast<std::uint64_t>(std::max<std::int64_t>(0,
                std::chrono::duration_cast<std::chrono::microseconds>(
                    runtime->origin_.time_since_epoch())
                    .count()));
    }
    static std::atomic<std::uint64_t> generations {1};
    auto generation = generations.load();
    do {
        if (generation == UINT64_MAX)
            return {};
    } while (!generations.compare_exchange_weak(generation, generation + 1));
    c.exchange.connection_generation = generation;
    c.affinity = c.local_socket;
    c.exchange.dispatch = c.conclusions->dispatch();
    c.exchange.source = c.conclusions->source();
    c.exchange.policy = options.policy;
    c.exchange.credential_id = options.credential_id;
    c.exchange.stream = options.stream;
    c.exchange.member = options.member;
    c.exchange.initial_position = options.initial_position;
    c.exchange.retry_interval_us = options.retry_interval_us;
    c.exchange.timeout_us = options.timeout_us;
    c.exchange.maximum_sends_per_stage = options.maximum_sends_per_stage;
    c.callbacks = std::move(options);
    using Owner = std::weak_ptr<ConnectionRuntime>;
    try {
        c.transport_owner = std::make_shared<Owner>(runtime);
    } catch (...) {
        return {};
    }
    c.bound = [](void* p, const NativeService* service) noexcept {
        const auto r = static_cast<Owner*>(p)->lock();
        if (!r)
            return false;
        std::lock_guard lock(r->mutex_);
        return !r->locally_closed_ && !r->broken_ && !r->peer_closed_
            && !r->peer_error_pending_ && r->hybrid_service_.get() == service
            && r->hybrid_conclusions_ == service->origin();
    };
    c.transmit = [](void* p, const NativeService* service,
                     std::span<const std::byte> bytes,
                     std::uint64_t deadline) noexcept -> UdpIoResult {
        const auto r = static_cast<Owner*>(p)->lock();
        if (!r)
            return {.error = Error::invalid_state};
        std::lock_guard lock(r->mutex_);
        if (r->locally_closed_ || r->broken_ || r->peer_closed_
            || r->peer_error_pending_ || service->closed()
            || r->hybrid_service_.get() != service
            || r->hybrid_conclusions_ != service->origin())
            return {.error = Error::invalid_state};
        if (NativeService::monotonic_now() >= deadline)
            return {.error = Error::would_block};
        const auto channel = r->channel_.lock();
        if (!channel)
            return {.error = Error::invalid_state};
        return channel->send_datagram(bytes, r->peer_);
    };
    auto service =
        NativeService::create(std::move(c), acquire_runtime_scheduler());
    if (!service)
        return {};
    {
        std::lock_guard lock(runtime->mutex_);
        if (runtime->locally_closed_ || runtime->broken_
            || runtime->peer_closed_ || runtime->peer_error_pending_
            || runtime->hybrid_attempted_
            || runtime->hybrid_conclusions_ != service->origin()) {
            service->close(NativeService::Failure::lifetime);
            return {};
        }
        runtime->hybrid_attempted_ = true;
        runtime->hybrid_service_ = service;
    }
    if (!service->activate())
        return {};
    return service;
}
#endif
} // namespace robotweax::srt::compat::hybrid
