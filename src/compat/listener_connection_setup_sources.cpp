#include "compat/listener_connection_setup_sources.hpp"

#include "robotweax/srt/codec.hpp"

#include <algorithm>
#include <utility>

namespace robotweax::srt::compat {
namespace {

[[nodiscard]] ListenerConnectionSetupDispatchStatus dispatch_status(
    RuntimeScheduler::SubmitStatus status) noexcept
{
    switch (status) {
    case RuntimeScheduler::SubmitStatus::accepted:
        return ListenerConnectionSetupDispatchStatus::completed;
    case RuntimeScheduler::SubmitStatus::full:
        return ListenerConnectionSetupDispatchStatus::full;
    case RuntimeScheduler::SubmitStatus::stopped:
        return ListenerConnectionSetupDispatchStatus::stopped;
    case RuntimeScheduler::SubmitStatus::invalid:
        return ListenerConnectionSetupDispatchStatus::invalid;
    }
    return ListenerConnectionSetupDispatchStatus::invalid;
}

} // namespace

struct ListenerConnectionSetupEventSource::TimerContext {
    std::weak_ptr<ListenerConnectionSetupEventSource> source;
    ListenerConnectionSetupSourceEventKind kind =
        ListenerConnectionSetupSourceEventKind::failure;
    std::uint64_t generation = 0;
};

ListenerConnectionSetupEventSource::ListenerConnectionSetupEventSource(
    RuntimeScheduler& scheduler, std::uint64_t affinity,
    std::shared_ptr<DatagramInbox> inbox) noexcept
    : scheduler_(scheduler)
    , affinity_(affinity)
    , inbox_(std::move(inbox))
{
}

ListenerConnectionSetupEventSource::~ListenerConnectionSetupEventSource()
{
    stop();
}

ListenerConnectionSetupDispatchStatus ListenerConnectionSetupEventSource::start(
    std::chrono::steady_clock::time_point overall_deadline) noexcept
{
    const std::shared_ptr<ListenerConnectionSetupEventSource> owner =
        weak_from_this().lock();
    if (owner == nullptr || inbox_ == nullptr) {
        return ListenerConnectionSetupDispatchStatus::invalid;
    }
    {
        std::lock_guard lock(mutex_);
        if (started_ || stopped_ || closed_) {
            return ListenerConnectionSetupDispatchStatus::invalid;
        }
        started_ = true;
    }
    if (!inbox_->set_ready_handler(inbox_ready, owner)) {
        stop();
        return ListenerConnectionSetupDispatchStatus::invalid;
    }
    ListenerConnectionSetupDispatchStatus readiness_status =
        ListenerConnectionSetupDispatchStatus::completed;
    {
        std::lock_guard lock(mutex_);
        handler_registered_ = true;
        readiness_status = failure_;
    }
    if (readiness_status != ListenerConnectionSetupDispatchStatus::completed) {
        stop();
        return readiness_status;
    }

    std::shared_ptr<TimerContext> timer;
    try {
        timer = std::make_shared<TimerContext>(TimerContext {
            .source = owner,
            .kind = ListenerConnectionSetupSourceEventKind::overall_timeout,
        });
    } catch (...) {
        stop();
        return ListenerConnectionSetupDispatchStatus::allocation_failed;
    }
    const RuntimeScheduler::ScheduleResult scheduled =
        scheduler_.schedule_at(affinity_, overall_deadline,
            {
                .function = publish_timer,
                .context = std::move(timer),
            });
    if (scheduled.status != RuntimeScheduler::SubmitStatus::accepted) {
        stop();
        return dispatch_status(scheduled.status);
    }

    bool cancel = false;
    {
        std::lock_guard lock(mutex_);
        if (stopped_) {
            cancel = true;
        } else {
            overall_timer_ = scheduled.token;
        }
    }
    if (cancel) {
        cancel_timer(scheduled.token);
        return ListenerConnectionSetupDispatchStatus::stopped;
    }
    return ListenerConnectionSetupDispatchStatus::completed;
}

ListenerConnectionSetupDispatchStatus
ListenerConnectionSetupEventSource::arm_retry(
    std::uint32_t timeout_milliseconds) noexcept
{
    return arm_retry_at(std::chrono::steady_clock::now()
        + std::chrono::milliseconds {timeout_milliseconds});
}

ListenerConnectionSetupDispatchStatus
ListenerConnectionSetupEventSource::arm_retry_at(
    std::chrono::steady_clock::time_point deadline) noexcept
{
    RuntimeScheduler::TimerToken previous;
    std::uint64_t generation = 0;
    {
        std::lock_guard lock(mutex_);
        if (!started_ || stopped_ || closed_) {
            return ListenerConnectionSetupDispatchStatus::stopped;
        }
        previous = retry_timer_;
        retry_timer_ = {};
        generation = ++retry_generation_;
        if (generation == 0U) {
            generation = ++retry_generation_;
        }
    }
    cancel_timer(previous);

    std::shared_ptr<TimerContext> timer;
    try {
        timer = std::make_shared<TimerContext>(TimerContext {
            .source = weak_from_this(),
            .kind = ListenerConnectionSetupSourceEventKind::retry_timer,
            .generation = generation,
        });
    } catch (...) {
        fail(ListenerConnectionSetupDispatchStatus::allocation_failed);
        return ListenerConnectionSetupDispatchStatus::allocation_failed;
    }
    const RuntimeScheduler::ScheduleResult scheduled =
        scheduler_.schedule_at(affinity_, deadline,
            {
                .function = publish_timer,
                .context = std::move(timer),
            });
    if (scheduled.status != RuntimeScheduler::SubmitStatus::accepted) {
        const ListenerConnectionSetupDispatchStatus failure =
            dispatch_status(scheduled.status);
        fail(failure);
        return failure;
    }

    bool cancel = false;
    {
        std::lock_guard lock(mutex_);
        if (stopped_ || closed_ || retry_generation_ != generation) {
            cancel = true;
        } else {
            retry_timer_ = scheduled.token;
        }
    }
    if (cancel) {
        cancel_timer(scheduled.token);
        return ListenerConnectionSetupDispatchStatus::stopped;
    }
    return ListenerConnectionSetupDispatchStatus::completed;
}

void ListenerConnectionSetupEventSource::cancel_retry() noexcept
{
    RuntimeScheduler::TimerToken previous;
    {
        std::lock_guard lock(mutex_);
        previous = retry_timer_;
        retry_timer_ = {};
        ++retry_generation_;
        if (retry_generation_ == 0U) {
            ++retry_generation_;
        }
    }
    cancel_timer(previous);
}

ListenerConnectionSetupSourceEvent
ListenerConnectionSetupEventSource::wait() noexcept
{
    std::unique_lock lock(mutex_);
    for (;;) {
        ready_.wait(lock, [this] {
            return failure_ != ListenerConnectionSetupDispatchStatus::completed
                || closed_ || size_ != 0U || stopped_;
        });
        if (failure_ != ListenerConnectionSetupDispatchStatus::completed) {
            return {
                .kind = ListenerConnectionSetupSourceEventKind::failure,
                .failure = failure_,
            };
        }
        if (closed_ || size_ == 0U) {
            return {.kind = ListenerConnectionSetupSourceEventKind::closed};
        }
        const ListenerConnectionSetupSourceEvent event = events_[head_];
        head_ = (head_ + 1U) % events_.size();
        --size_;
        if (event.kind == ListenerConnectionSetupSourceEventKind::retry_timer
            && event.generation != retry_generation_) {
            continue;
        }
        return event;
    }
}

bool ListenerConnectionSetupEventSource::try_pop(
    ListenerConnectionSetupSourceEvent& event) noexcept
{
    std::lock_guard lock(mutex_);
    if (failure_ != ListenerConnectionSetupDispatchStatus::completed) {
        event = {
            .kind = ListenerConnectionSetupSourceEventKind::failure,
            .failure = failure_,
        };
        return true;
    }
    if (closed_) {
        event = {.kind = ListenerConnectionSetupSourceEventKind::closed};
        return true;
    }
    while (size_ != 0U) {
        event = events_[head_];
        head_ = (head_ + 1U) % events_.size();
        --size_;
        if (event.kind != ListenerConnectionSetupSourceEventKind::retry_timer
            || event.generation == retry_generation_) {
            return true;
        }
    }
    return false;
}

bool ListenerConnectionSetupEventSource::set_event_ready_handler(
    EventReadyFunction function, std::shared_ptr<void> context) noexcept
{
    if (function == nullptr || context == nullptr) {
        return false;
    }
    bool notify = false;
    {
        std::lock_guard lock(mutex_);
        if (event_ready_handler_ != nullptr || stopped_) {
            return false;
        }
        event_ready_handler_ = function;
        event_ready_context_ = std::move(context);
        notify = failure_ != ListenerConnectionSetupDispatchStatus::completed
            || closed_ || size_ != 0U;
    }
    if (notify) {
        notify_event_ready();
    }
    return true;
}

void ListenerConnectionSetupEventSource::clear_event_ready_handler() noexcept
{
    std::lock_guard lock(mutex_);
    event_ready_handler_ = nullptr;
    event_ready_context_.reset();
}

void ListenerConnectionSetupEventSource::acknowledge_inbox() noexcept
{
    {
        std::lock_guard lock(mutex_);
        inbox_pending_ = false;
        if (stopped_ || closed_) {
            return;
        }
    }
    if (inbox_->ready()) {
        request_inbox();
    }
}

void ListenerConnectionSetupEventSource::close() noexcept
{
    RuntimeScheduler::TimerToken retry;
    RuntimeScheduler::TimerToken overall;
    bool clear_handler = false;
    {
        std::lock_guard lock(mutex_);
        if (closed_) {
            return;
        }
        closed_ = true;
        retry = retry_timer_;
        overall = overall_timer_;
        retry_timer_ = {};
        overall_timer_ = {};
        clear_handler = handler_registered_;
        handler_registered_ = false;
    }
    if (clear_handler) {
        inbox_->clear_ready_handler();
    }
    cancel_timer(retry);
    cancel_timer(overall);
    ready_.notify_all();
    notify_event_ready();
}

void ListenerConnectionSetupEventSource::stop() noexcept
{
    RuntimeScheduler::TimerToken retry;
    RuntimeScheduler::TimerToken overall;
    bool clear_handler = false;
    {
        std::lock_guard lock(mutex_);
        if (stopped_) {
            return;
        }
        stopped_ = true;
        retry = retry_timer_;
        overall = overall_timer_;
        retry_timer_ = {};
        overall_timer_ = {};
        clear_handler = handler_registered_;
        handler_registered_ = false;
        event_ready_handler_ = nullptr;
        event_ready_context_.reset();
    }
    if (clear_handler && inbox_ != nullptr) {
        inbox_->clear_ready_handler();
    }
    cancel_timer(retry);
    cancel_timer(overall);
    ready_.notify_all();
}

void ListenerConnectionSetupEventSource::inbox_ready(void* context) noexcept
{
    static_cast<ListenerConnectionSetupEventSource*>(context)->request_inbox();
}

void ListenerConnectionSetupEventSource::publish_inbox_ready(
    void* context) noexcept
{
    auto& source = *static_cast<ListenerConnectionSetupEventSource*>(context);
    source.enqueue({
        .kind = ListenerConnectionSetupSourceEventKind::inbox_ready,
    });
}

void ListenerConnectionSetupEventSource::publish_timer(void* context) noexcept
{
    auto& timer = *static_cast<TimerContext*>(context);
    if (const auto source = timer.source.lock(); source != nullptr) {
        source->enqueue({
            .kind = timer.kind,
            .generation = timer.generation,
        });
    }
}

void ListenerConnectionSetupEventSource::request_inbox() noexcept
{
    const std::shared_ptr<ListenerConnectionSetupEventSource> owner =
        weak_from_this().lock();
    if (owner == nullptr) {
        return;
    }
    {
        std::lock_guard lock(mutex_);
        if (stopped_ || closed_ || inbox_pending_) {
            return;
        }
        inbox_pending_ = true;
    }
    const RuntimeScheduler::SubmitStatus submitted =
        scheduler_.submit(affinity_,
            {
                .function = publish_inbox_ready,
                .context = owner,
            });
    if (submitted == RuntimeScheduler::SubmitStatus::accepted) {
        return;
    }
    {
        std::lock_guard lock(mutex_);
        inbox_pending_ = false;
    }
    fail(dispatch_status(submitted));
}

void ListenerConnectionSetupEventSource::enqueue(
    ListenerConnectionSetupSourceEvent event) noexcept
{
    {
        std::lock_guard lock(mutex_);
        if (stopped_ || closed_) {
            return;
        }
        if (size_ == events_.size()) {
            failure_ = ListenerConnectionSetupDispatchStatus::full;
            head_ = 0U;
            size_ = 0U;
        } else {
            events_[(head_ + size_) % events_.size()] = event;
            ++size_;
        }
    }
    ready_.notify_one();
    notify_event_ready();
}

void ListenerConnectionSetupEventSource::fail(
    ListenerConnectionSetupDispatchStatus failure) noexcept
{
    {
        std::lock_guard lock(mutex_);
        if (stopped_ || closed_) {
            return;
        }
        failure_ = failure;
        head_ = 0U;
        size_ = 0U;
    }
    ready_.notify_all();
    notify_event_ready();
}

void ListenerConnectionSetupEventSource::cancel_timer(
    RuntimeScheduler::TimerToken token) noexcept
{
    if (token.valid()) {
        (void)scheduler_.cancel_timer(token);
    }
}

void ListenerConnectionSetupEventSource::notify_event_ready() noexcept
{
    EventReadyFunction handler = nullptr;
    std::shared_ptr<void> context;
    {
        std::lock_guard lock(mutex_);
        handler = event_ready_handler_;
        context = event_ready_context_;
    }
    if (handler != nullptr && context != nullptr) {
        handler(context.get());
    }
}

ListenerConnectionSetupActor::ListenerConnectionSetupActor(
    std::shared_ptr<RuntimeScheduler> scheduler, std::uint64_t affinity,
    std::shared_ptr<DatagramInbox> inbox, ListenerHandshakeAdmission admission,
    ListenerConnectionSetupSteps::Configuration configuration,
    std::size_t result_capacity)
    : scheduler_(std::move(scheduler))
    , affinity_(affinity)
    , inbox_(std::move(inbox))
    , admission_(std::move(admission))
    , steps_(admission_, configuration)
    , local_socket_id_(configuration.hsv5.local_socket_id)
    , results_(std::max<std::size_t>(result_capacity, 1U))
{
}

ListenerConnectionSetupActor::~ListenerConnectionSetupActor()
{
    stop();
}

ListenerConnectionSetupDispatchStatus ListenerConnectionSetupActor::start(
    std::chrono::steady_clock::time_point overall_deadline) noexcept
{
    std::shared_ptr<ListenerConnectionSetupActor> owner;
    try {
        owner = shared_from_this();
    } catch (...) {
        return ListenerConnectionSetupDispatchStatus::invalid;
    }
    if (scheduler_ == nullptr || inbox_ == nullptr) {
        return ListenerConnectionSetupDispatchStatus::invalid;
    }
    std::shared_ptr<ListenerConnectionSetupEventSource> source;
    try {
        source = std::make_shared<ListenerConnectionSetupEventSource>(
            *scheduler_, affinity_, inbox_);
    } catch (...) {
        finish_failure(
            ListenerConnectionSetupDispatchStatus::allocation_failed);
        return ListenerConnectionSetupDispatchStatus::allocation_failed;
    }
    {
        std::lock_guard lock(mutex_);
        if (started_ || terminal_) {
            return ListenerConnectionSetupDispatchStatus::invalid;
        }
        source_ = source;
        started_ = true;
    }
    if (!source->set_event_ready_handler(source_ready, std::move(owner))) {
        finish_failure(ListenerConnectionSetupDispatchStatus::invalid);
        return ListenerConnectionSetupDispatchStatus::invalid;
    }
    const ListenerConnectionSetupDispatchStatus source_status =
        source->start(overall_deadline);
    if (source_status != ListenerConnectionSetupDispatchStatus::completed) {
        finish_failure(source_status);
        return source_status;
    }
    return request_run();
}

bool ListenerConnectionSetupActor::reject(int rejection_reason) noexcept
{
    std::lock_guard lock(mutex_);
    if (started_ || terminal_ || initial_rejection_reason_.has_value()) {
        return false;
    }
    initial_rejection_reason_ = rejection_reason;
    return true;
}

void ListenerConnectionSetupActor::close() noexcept
{
    bool started = false;
    {
        std::lock_guard lock(mutex_);
        if (terminal_) {
            return;
        }
        close_requested_ = true;
        started = started_;
    }
    if (!started) {
        finish_closed();
        return;
    }
    (void)request_run();
}

void ListenerConnectionSetupActor::stop() noexcept
{
    close();
    std::unique_lock lock(mutex_);
    if (active_thread_ == std::this_thread::get_id()) {
        return;
    }
    completed_.wait(lock, [this] {
        return terminal_;
    });
}

ListenerConnectionSetupActorResult ListenerConnectionSetupActor::wait() noexcept
{
    std::unique_lock lock(mutex_);
    ready_.wait(lock, [this] {
        return size_ != 0U || terminal_;
    });
    if (size_ == 0U) {
        return {
            .kind = ListenerConnectionSetupActorResultKind::failure,
            .failure = failure_,
        };
    }
    const ListenerConnectionSetupActorResult result = results_[head_];
    head_ = (head_ + 1U) % results_.size();
    --size_;
    if (result.kind == ListenerConnectionSetupActorResultKind::setup
        && result.setup.outcome == ListenerConnectionSetupOutcome::running
        && result_pending_) {
        result_consumed_ = true;
    }
    return result;
}

bool ListenerConnectionSetupActor::try_pop(
    ListenerConnectionSetupActorResult& result) noexcept
{
    std::lock_guard lock(mutex_);
    if (size_ == 0U) {
        return false;
    }
    result = results_[head_];
    head_ = (head_ + 1U) % results_.size();
    --size_;
    if (result.kind == ListenerConnectionSetupActorResultKind::setup
        && result.setup.outcome == ListenerConnectionSetupOutcome::running
        && result_pending_) {
        result_consumed_ = true;
    }
    return true;
}

bool ListenerConnectionSetupActor::set_result_ready_handler(
    ResultReadyFunction function, std::shared_ptr<void> context) noexcept
{
    if (function == nullptr || context == nullptr) {
        return false;
    }
    bool notify = false;
    {
        std::lock_guard lock(mutex_);
        if (result_ready_handler_ != nullptr) {
            return false;
        }
        result_ready_handler_ = function;
        result_ready_context_ = std::move(context);
        notify = size_ != 0U || terminal_;
    }
    if (notify) {
        notify_result_ready();
    }
    return true;
}

void ListenerConnectionSetupActor::clear_result_ready_handler() noexcept
{
    std::lock_guard lock(mutex_);
    result_ready_handler_ = nullptr;
    result_ready_context_.reset();
}

bool ListenerConnectionSetupActor::complete_result() noexcept
{
    std::shared_ptr<ListenerConnectionSetupEventSource> source;
    std::optional<std::chrono::microseconds> retry;
    bool acknowledge_inbox = false;
    {
        std::lock_guard lock(mutex_);
        if (terminal_ || !result_pending_ || !result_consumed_
            || source_ == nullptr) {
            return false;
        }
        result_pending_ = false;
        result_consumed_ = false;
        acknowledge_inbox = pending_inbox_acknowledgement_;
        pending_inbox_acknowledgement_ = false;
        retry = pending_retry_delay_;
        pending_retry_delay_.reset();
        source = source_;
    }
    if (retry.has_value()) {
        const ListenerConnectionSetupDispatchStatus status =
            source->arm_retry_at(std::chrono::steady_clock::now() + *retry);
        if (status != ListenerConnectionSetupDispatchStatus::completed) {
            finish_failure(status);
            return false;
        }
    }
    if (acknowledge_inbox) {
        source->acknowledge_inbox();
    }
    return request_run() == ListenerConnectionSetupDispatchStatus::completed;
}

ListenerConnectionSetupActorSnapshot
ListenerConnectionSetupActor::snapshot() const noexcept
{
    std::lock_guard lock(mutex_);
    return {
        .queued_results = size_,
        .failure = failure_,
        .started = started_,
        .result_pending = result_pending_,
        .terminal = terminal_,
    };
}

void ListenerConnectionSetupActor::run_task(void* context) noexcept
{
    static_cast<ListenerConnectionSetupActor*>(context)->run();
}

void ListenerConnectionSetupActor::source_ready(void* context) noexcept
{
    (void)static_cast<ListenerConnectionSetupActor*>(context)->request_run();
}

ListenerConnectionSetupDispatchStatus
ListenerConnectionSetupActor::request_run() noexcept
{
    std::shared_ptr<ListenerConnectionSetupActor> owner;
    {
        std::lock_guard lock(mutex_);
        if (terminal_) {
            return failure_ == ListenerConnectionSetupDispatchStatus::completed
                ? ListenerConnectionSetupDispatchStatus::stopped
                : failure_;
        }
        pending_ = true;
        if (scheduled_) {
            return ListenerConnectionSetupDispatchStatus::completed;
        }
        scheduled_ = true;
    }
    try {
        owner = shared_from_this();
    } catch (...) {
        finish_failure(ListenerConnectionSetupDispatchStatus::invalid);
        return ListenerConnectionSetupDispatchStatus::invalid;
    }
    const RuntimeScheduler::SubmitStatus submitted =
        scheduler_->submit(affinity_,
            {
                .function = run_task,
                .context = std::move(owner),
            });
    const ListenerConnectionSetupDispatchStatus status =
        dispatch_status(submitted);
    if (status != ListenerConnectionSetupDispatchStatus::completed) {
        finish_failure(status);
    }
    return status;
}

void ListenerConnectionSetupActor::run() noexcept
{
    {
        std::lock_guard lock(mutex_);
        active_thread_ = std::this_thread::get_id();
    }
    for (;;) {
        bool close_requested = false;
        bool blocked = false;
        bool initialize_actor = false;
        {
            std::lock_guard lock(mutex_);
            pending_ = false;
            close_requested = close_requested_;
            blocked = result_pending_;
            initialize_actor = !initialized_;
        }
        if (close_requested) {
            finish_closed();
        } else if (!blocked && initialize_actor) {
            initialize();
        } else if (!blocked) {
            drain_source();
        }

        std::lock_guard lock(mutex_);
        if (terminal_) {
            scheduled_ = false;
            active_thread_ = {};
            completed_.notify_all();
            return;
        }
        if (result_pending_ || !pending_) {
            scheduled_ = false;
            active_thread_ = {};
            return;
        }
    }
}

void ListenerConnectionSetupActor::initialize() noexcept
{
    std::optional<int> rejection_reason;
    {
        std::lock_guard lock(mutex_);
        initialized_ = true;
        rejection_reason = initial_rejection_reason_;
    }
    const ListenerConnectionSetupResult started = steps_.dispatch({
        .kind = ListenerConnectionSetupEventKind::start,
    });
    if (started.outcome != ListenerConnectionSetupOutcome::running) {
        handle_result(started, false);
        return;
    }
    if (rejection_reason.has_value()) {
        handle_result(steps_.dispatch({
                          .kind = ListenerConnectionSetupEventKind::reject,
                          .rejection_reason = *rejection_reason,
                      }),
            false);
        return;
    }
    handle_result(steps_.dispatch({
                      .kind = ListenerConnectionSetupEventKind::admit,
                      .handshake = admission_.conclusion.message,
                      .has_handshake = true,
                  }),
        false, &admission_.conclusion.message, &admission_.conclusion.control);
}

void ListenerConnectionSetupActor::drain_source() noexcept
{
    std::shared_ptr<ListenerConnectionSetupEventSource> source;
    {
        std::lock_guard lock(mutex_);
        source = source_;
    }
    ListenerConnectionSetupSourceEvent event;
    while (source != nullptr && source->try_pop(event)) {
        process_source_event(event);
        const ListenerConnectionSetupActorSnapshot state = snapshot();
        if (state.terminal || state.result_pending) {
            return;
        }
    }
}

void ListenerConnectionSetupActor::process_source_event(
    const ListenerConnectionSetupSourceEvent& event) noexcept
{
    switch (event.kind) {
    case ListenerConnectionSetupSourceEventKind::inbox_ready:
        process_inbox();
        return;
    case ListenerConnectionSetupSourceEventKind::retry_timer:
        handle_result(steps_.dispatch({
                          .kind = ListenerConnectionSetupEventKind::retry_timer,
                      }),
            false);
        return;
    case ListenerConnectionSetupSourceEventKind::overall_timeout:
        handle_result(
            steps_.dispatch({
                .kind = ListenerConnectionSetupEventKind::overall_timeout,
            }),
            false);
        return;
    case ListenerConnectionSetupSourceEventKind::closed:
        finish_closed();
        return;
    case ListenerConnectionSetupSourceEventKind::failure:
        finish_failure(event.failure);
        return;
    }
}

void ListenerConnectionSetupActor::process_inbox() noexcept
{
    std::shared_ptr<ListenerConnectionSetupEventSource> source;
    {
        std::lock_guard lock(mutex_);
        source = source_;
    }
    if (source == nullptr) {
        finish_failure(ListenerConnectionSetupDispatchStatus::invalid);
        return;
    }
    DatagramEnvelope envelope;
    const InboxPopStatus received =
        inbox_->pop_for(envelope, std::chrono::milliseconds {0});
    if (received == InboxPopStatus::closed) {
        finish_closed();
        return;
    }
    if (received != InboxPopStatus::received) {
        source->acknowledge_inbox();
        return;
    }
    if (envelope.peer != admission_.conclusion.peer) {
        source->acknowledge_inbox();
        return;
    }

    const std::span<const std::byte> bytes =
        std::span {envelope.bytes}.first(envelope.size);
    const DecodeResult packet = decode_packet(bytes);
    if (!packet || packet.packet.kind != PacketKind::control) {
        source->acknowledge_inbox();
        return;
    }
    if (packet.packet.control.type == ControlType::handshake) {
        const std::uint32_t destination_socket_id =
            packet.packet.control.destination_socket_id;
        if (destination_socket_id != local_socket_id_
            && destination_socket_id
                != admission_.conclusion.control.destination_socket_id) {
            source->acknowledge_inbox();
            return;
        }
        const HandshakeDatagramDecodeResult handshake =
            decode_handshake_datagram(bytes);
        if (!handshake
            || handshake.message.packet.socket_id
                != admission_.conclusion.message.packet.socket_id) {
            source->acknowledge_inbox();
            return;
        }
        handle_result(steps_.dispatch({
                          .kind = ListenerConnectionSetupEventKind::handshake,
                          .handshake = handshake.message,
                          .has_handshake = true,
                      }),
            true, &handshake.message, &handshake.control);
        return;
    }
    if (packet.packet.control.type == ControlType::user_defined) {
        source->acknowledge_inbox();
        return;
    }
    source->acknowledge_inbox();
}

void ListenerConnectionSetupActor::handle_result(
    ListenerConnectionSetupResult result, bool acknowledge_inbox,
    const HandshakeMessage* peer_handshake,
    const ControlHeader* peer_control) noexcept
{
    if (result.outcome == ListenerConnectionSetupOutcome::invalid) {
        finish_failure(ListenerConnectionSetupDispatchStatus::invalid);
        return;
    }
    ListenerConnectionSetupActorResult actor_result {
        .kind = ListenerConnectionSetupActorResultKind::setup,
        .setup = std::move(result),
        .failure = ListenerConnectionSetupDispatchStatus::completed,
    };
    if (actor_result.setup.outcome
        == ListenerConnectionSetupOutcome::connected) {
        actor_result.runtime = steps_.runtime_state();
        if (!actor_result.runtime) {
            finish_failure(ListenerConnectionSetupDispatchStatus::invalid);
            return;
        }
    }
    if (peer_handshake != nullptr) {
        actor_result.peer_handshake = *peer_handshake;
        actor_result.has_peer_handshake = true;
    }
    if (peer_control != nullptr) {
        actor_result.peer_control = *peer_control;
    }
    if (terminal_outcome(actor_result.setup.outcome)) {
        finish_setup(std::move(actor_result));
        return;
    }
    if (!has_actions(actor_result.setup)) {
        if (acknowledge_inbox) {
            std::shared_ptr<ListenerConnectionSetupEventSource> source;
            {
                std::lock_guard lock(mutex_);
                source = source_;
            }
            if (source != nullptr) {
                source->acknowledge_inbox();
            }
        }
        return;
    }
    const std::optional<std::chrono::microseconds> retry =
        retry_delay(actor_result.setup);
    publish_running(std::move(actor_result), acknowledge_inbox, retry);
}

void ListenerConnectionSetupActor::publish_running(
    ListenerConnectionSetupActorResult result, bool acknowledge_inbox,
    std::optional<std::chrono::microseconds> retry) noexcept
{
    ListenerConnectionSetupDispatchStatus failure =
        ListenerConnectionSetupDispatchStatus::full;
    bool published = false;
    {
        std::lock_guard lock(mutex_);
        if (terminal_) {
            return;
        }
        if (result_pending_) {
            failure = ListenerConnectionSetupDispatchStatus::invalid;
        } else if (size_ != results_.size()) {
            results_[(head_ + size_) % results_.size()] = std::move(result);
            ++size_;
            result_pending_ = true;
            result_consumed_ = false;
            pending_inbox_acknowledgement_ = acknowledge_inbox;
            pending_retry_delay_ = retry;
            published = true;
        }
    }
    if (published) {
        ready_.notify_one();
        notify_result_ready();
        return;
    }
    finish_failure(failure);
}

void ListenerConnectionSetupActor::finish_setup(
    ListenerConnectionSetupActorResult result) noexcept
{
    finish(std::move(result));
}

void ListenerConnectionSetupActor::finish_failure(
    ListenerConnectionSetupDispatchStatus failure) noexcept
{
    finish({
        .kind = ListenerConnectionSetupActorResultKind::failure,
        .failure = failure,
    });
}

void ListenerConnectionSetupActor::finish_closed() noexcept
{
    ListenerConnectionSetupResult result = steps_.dispatch({
        .kind = ListenerConnectionSetupEventKind::close,
    });
    if (result.outcome == ListenerConnectionSetupOutcome::invalid) {
        result = {
            .outcome = ListenerConnectionSetupOutcome::closed,
            .protocol = steps_.protocol(),
        };
    }
    finish({
        .kind = ListenerConnectionSetupActorResultKind::setup,
        .setup = std::move(result),
        .failure = ListenerConnectionSetupDispatchStatus::completed,
    });
}

void ListenerConnectionSetupActor::finish(
    ListenerConnectionSetupActorResult terminal) noexcept
{
    std::shared_ptr<ListenerConnectionSetupEventSource> source;
    {
        std::lock_guard lock(mutex_);
        if (terminal_) {
            return;
        }
        terminal_ = true;
        failure_ =
            terminal.kind == ListenerConnectionSetupActorResultKind::failure
            ? terminal.failure
            : ListenerConnectionSetupDispatchStatus::completed;
        head_ = 0U;
        size_ = 1U;
        result_pending_ = false;
        result_consumed_ = false;
        pending_inbox_acknowledgement_ = false;
        pending_retry_delay_.reset();
        results_[0] = std::move(terminal);
        source = std::move(source_);
    }
    if (source != nullptr) {
        source->clear_event_ready_handler();
        source->stop();
    }
    ready_.notify_all();
    completed_.notify_all();
    notify_result_ready();
}

void ListenerConnectionSetupActor::notify_result_ready() noexcept
{
    ResultReadyFunction handler = nullptr;
    std::shared_ptr<void> context;
    {
        std::lock_guard lock(mutex_);
        handler = result_ready_handler_;
        context = result_ready_context_;
    }
    if (handler != nullptr && context != nullptr) {
        handler(context.get());
    }
}

std::optional<std::chrono::microseconds>
ListenerConnectionSetupActor::retry_delay(
    const ListenerConnectionSetupResult& result) noexcept
{
    for (std::size_t index = 0; index < result.hsv5_actions.size; ++index) {
        const HandshakeAction& action = result.hsv5_actions.values[index];
        if (action.kind == HandshakeActionKind::arm_timer) {
            return std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::milliseconds {action.timeout_milliseconds});
        }
    }
    return std::nullopt;
}

bool ListenerConnectionSetupActor::has_actions(
    const ListenerConnectionSetupResult& result) noexcept
{
    return result.hsv5_actions.size != 0U;
}

bool ListenerConnectionSetupActor::terminal_outcome(
    ListenerConnectionSetupOutcome outcome) noexcept
{
    return outcome == ListenerConnectionSetupOutcome::connected
        || outcome == ListenerConnectionSetupOutcome::rejected
        || outcome == ListenerConnectionSetupOutcome::failed
        || outcome == ListenerConnectionSetupOutcome::closed
        || outcome == ListenerConnectionSetupOutcome::timed_out;
}

} // namespace robotweax::srt::compat
