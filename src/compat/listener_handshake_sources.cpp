#include "compat/listener_handshake_sources.hpp"

#include <algorithm>
#include <utility>

namespace robotweax::srt::compat {
namespace {

[[nodiscard]] ListenerHandshakeDispatchStatus dispatch_status(
    RuntimeScheduler::SubmitStatus status) noexcept
{
    switch (status) {
    case RuntimeScheduler::SubmitStatus::accepted:
        return ListenerHandshakeDispatchStatus::completed;
    case RuntimeScheduler::SubmitStatus::full:
        return ListenerHandshakeDispatchStatus::full;
    case RuntimeScheduler::SubmitStatus::stopped:
        return ListenerHandshakeDispatchStatus::stopped;
    case RuntimeScheduler::SubmitStatus::invalid:
        return ListenerHandshakeDispatchStatus::invalid;
    }
    return ListenerHandshakeDispatchStatus::invalid;
}

} // namespace

ListenerHandshakeAdmissionSteps::ListenerHandshakeAdmissionSteps(
    StatelessListenerHandshakeRouter::Configuration configuration) noexcept
    : router_(configuration)
{
}

ListenerHandshakeAdmissionStep ListenerHandshakeAdmissionSteps::receive(
    const HandshakeEnvelope& envelope, std::uint64_t time_window) noexcept
{
    if (closed_) {
        return {.kind = ListenerHandshakeAdmissionStepKind::closed};
    }
    const ListenerHandshakeRoute route =
        router_.route(envelope.message, envelope.peer, time_window);
    if (route.kind == ListenerHandshakeRouteKind::send_induction_response) {
        return {
            .kind = ListenerHandshakeAdmissionStepKind::send,
            .peer = envelope.peer,
            .destination_socket_id = envelope.message.packet.socket_id,
            .response = route.response,
        };
    }
    if (route.kind != ListenerHandshakeRouteKind::admit_conclusion) {
        return {};
    }

    ListenerHandshakeAdmission admission {
        .initial = envelope,
        .conclusion = envelope,
        .protocol = route.protocol,
        .validated_cookie = route.validated_cookie,
    };
    admission.initial.message = route.reconstructed_induction;
    return {
        .kind = ListenerHandshakeAdmissionStepKind::admit,
        .peer = envelope.peer,
        .destination_socket_id = envelope.message.packet.socket_id,
        .admission = admission,
    };
}

ListenerHandshakeAdmissionStep ListenerHandshakeAdmissionSteps::close() noexcept
{
    closed_ = true;
    return {.kind = ListenerHandshakeAdmissionStepKind::closed};
}

ListenerHandshakeEventSource::ListenerHandshakeEventSource(
    RuntimeScheduler& scheduler, std::uint64_t affinity,
    std::shared_ptr<HandshakeInbox> inbox) noexcept
    : scheduler_(scheduler)
    , affinity_(affinity)
    , inbox_(std::move(inbox))
{
}

ListenerHandshakeEventSource::~ListenerHandshakeEventSource()
{
    stop();
}

ListenerHandshakeDispatchStatus ListenerHandshakeEventSource::start() noexcept
{
    const std::shared_ptr<ListenerHandshakeEventSource> owner =
        weak_from_this().lock();
    if (owner == nullptr || inbox_ == nullptr) {
        return ListenerHandshakeDispatchStatus::invalid;
    }
    if (!scheduler_.snapshot().accepting) {
        return ListenerHandshakeDispatchStatus::stopped;
    }
    {
        std::lock_guard lock(mutex_);
        if (started_ || stopped_ || closed_) {
            return ListenerHandshakeDispatchStatus::invalid;
        }
        started_ = true;
    }
    if (!inbox_->set_ready_handler(inbox_ready, owner)) {
        stop();
        return ListenerHandshakeDispatchStatus::invalid;
    }

    ListenerHandshakeDispatchStatus readiness_status =
        ListenerHandshakeDispatchStatus::completed;
    {
        std::lock_guard lock(mutex_);
        handler_registered_ = true;
        readiness_status = failure_;
    }
    if (readiness_status != ListenerHandshakeDispatchStatus::completed) {
        stop();
        return readiness_status;
    }
    return ListenerHandshakeDispatchStatus::completed;
}

ListenerHandshakeSourceEvent ListenerHandshakeEventSource::wait() noexcept
{
    std::unique_lock lock(mutex_);
    ready_.wait(lock, [this] {
        return failure_ != ListenerHandshakeDispatchStatus::completed || closed_
            || size_ != 0U || stopped_;
    });
    if (failure_ != ListenerHandshakeDispatchStatus::completed) {
        return {
            .kind = ListenerHandshakeSourceEventKind::failure,
            .failure = failure_,
        };
    }
    if (closed_ || size_ == 0U) {
        return {.kind = ListenerHandshakeSourceEventKind::closed};
    }
    const ListenerHandshakeSourceEvent event = events_[head_];
    head_ = (head_ + 1U) % events_.size();
    --size_;
    return event;
}

bool ListenerHandshakeEventSource::try_pop(
    ListenerHandshakeSourceEvent& event) noexcept
{
    std::lock_guard lock(mutex_);
    if (failure_ != ListenerHandshakeDispatchStatus::completed) {
        event = {
            .kind = ListenerHandshakeSourceEventKind::failure,
            .failure = failure_,
        };
        return true;
    }
    if (closed_) {
        event = {.kind = ListenerHandshakeSourceEventKind::closed};
        return true;
    }
    if (size_ == 0U) {
        return false;
    }
    event = events_[head_];
    head_ = (head_ + 1U) % events_.size();
    --size_;
    return true;
}

bool ListenerHandshakeEventSource::set_event_ready_handler(
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
        notify = failure_ != ListenerHandshakeDispatchStatus::completed
            || closed_ || size_ != 0U;
    }
    if (notify) {
        notify_event_ready();
    }
    return true;
}

void ListenerHandshakeEventSource::clear_event_ready_handler() noexcept
{
    std::lock_guard lock(mutex_);
    event_ready_handler_ = nullptr;
    event_ready_context_.reset();
}

void ListenerHandshakeEventSource::acknowledge_inbox() noexcept
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

void ListenerHandshakeEventSource::close() noexcept
{
    bool clear_handler = false;
    {
        std::lock_guard lock(mutex_);
        if (closed_) {
            return;
        }
        closed_ = true;
        clear_handler = handler_registered_;
        handler_registered_ = false;
    }
    if (clear_handler) {
        inbox_->clear_ready_handler();
    }
    ready_.notify_all();
    notify_event_ready();
}

void ListenerHandshakeEventSource::stop() noexcept
{
    bool clear_handler = false;
    {
        std::lock_guard lock(mutex_);
        if (stopped_) {
            return;
        }
        stopped_ = true;
        clear_handler = handler_registered_;
        handler_registered_ = false;
        event_ready_handler_ = nullptr;
        event_ready_context_.reset();
    }
    if (clear_handler && inbox_ != nullptr) {
        inbox_->clear_ready_handler();
    }
    ready_.notify_all();
}

void ListenerHandshakeEventSource::inbox_ready(void* context) noexcept
{
    static_cast<ListenerHandshakeEventSource*>(context)->request_inbox();
}

void ListenerHandshakeEventSource::publish_inbox_ready(void* context) noexcept
{
    auto& source = *static_cast<ListenerHandshakeEventSource*>(context);
    source.enqueue({
        .kind = ListenerHandshakeSourceEventKind::inbox_ready,
    });
}

void ListenerHandshakeEventSource::request_inbox() noexcept
{
    const std::shared_ptr<ListenerHandshakeEventSource> owner =
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

void ListenerHandshakeEventSource::enqueue(
    ListenerHandshakeSourceEvent event) noexcept
{
    {
        std::lock_guard lock(mutex_);
        if (stopped_ || closed_) {
            return;
        }
        if (size_ == events_.size()) {
            failure_ = ListenerHandshakeDispatchStatus::full;
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

void ListenerHandshakeEventSource::fail(
    ListenerHandshakeDispatchStatus failure) noexcept
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

void ListenerHandshakeEventSource::notify_event_ready() noexcept
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

ListenerHandshakeActor::ListenerHandshakeActor(
    std::shared_ptr<RuntimeScheduler> scheduler, std::uint64_t affinity,
    std::shared_ptr<HandshakeInbox> inbox,
    StatelessListenerHandshakeRouter::Configuration configuration,
    std::size_t result_capacity, TimeWindowFunction time_window,
    std::shared_ptr<void> time_window_context)
    : scheduler_(std::move(scheduler))
    , affinity_(affinity)
    , inbox_(std::move(inbox))
    , steps_(configuration)
    , time_window_(time_window)
    , time_window_context_(std::move(time_window_context))
    , results_(std::max<std::size_t>(result_capacity, 1U))
{
}

ListenerHandshakeActor::~ListenerHandshakeActor()
{
    stop();
}

ListenerHandshakeDispatchStatus ListenerHandshakeActor::start() noexcept
{
    std::shared_ptr<ListenerHandshakeActor> owner;
    try {
        owner = shared_from_this();
    } catch (...) {
        return ListenerHandshakeDispatchStatus::invalid;
    }
    if (scheduler_ == nullptr || inbox_ == nullptr || time_window_ == nullptr) {
        return ListenerHandshakeDispatchStatus::invalid;
    }
    std::shared_ptr<ListenerHandshakeEventSource> source;
    try {
        source = std::make_shared<ListenerHandshakeEventSource>(
            *scheduler_, affinity_, inbox_);
    } catch (...) {
        finish_failure(ListenerHandshakeDispatchStatus::full);
        return ListenerHandshakeDispatchStatus::full;
    }
    {
        std::lock_guard lock(mutex_);
        if (started_ || terminal_) {
            return ListenerHandshakeDispatchStatus::invalid;
        }
        source_ = source;
        started_ = true;
    }
    if (!source->set_event_ready_handler(source_ready, std::move(owner))) {
        finish_failure(ListenerHandshakeDispatchStatus::invalid);
        return ListenerHandshakeDispatchStatus::invalid;
    }
    const ListenerHandshakeDispatchStatus status = source->start();
    if (status != ListenerHandshakeDispatchStatus::completed) {
        finish_failure(status);
    }
    return status;
}

void ListenerHandshakeActor::close() noexcept
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
    request_run();
}

void ListenerHandshakeActor::stop() noexcept
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

ListenerHandshakeActorResult ListenerHandshakeActor::wait() noexcept
{
    std::unique_lock lock(mutex_);
    ready_.wait(lock, [this] {
        return size_ != 0U || terminal_;
    });
    if (size_ == 0U) {
        return {
            .kind = failure_ == ListenerHandshakeDispatchStatus::completed
                ? ListenerHandshakeActorResultKind::closed
                : ListenerHandshakeActorResultKind::failure,
            .failure = failure_,
        };
    }
    const ListenerHandshakeActorResult result = results_[head_];
    head_ = (head_ + 1U) % results_.size();
    --size_;
    return result;
}

bool ListenerHandshakeActor::try_pop(
    ListenerHandshakeActorResult& result) noexcept
{
    std::lock_guard lock(mutex_);
    if (size_ == 0U) {
        return false;
    }
    result = results_[head_];
    head_ = (head_ + 1U) % results_.size();
    --size_;
    return true;
}

bool ListenerHandshakeActor::set_result_ready_handler(
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

void ListenerHandshakeActor::clear_result_ready_handler() noexcept
{
    std::lock_guard lock(mutex_);
    result_ready_handler_ = nullptr;
    result_ready_context_.reset();
}

bool ListenerHandshakeActor::complete_admission() noexcept
{
    std::shared_ptr<ListenerHandshakeEventSource> source;
    {
        std::lock_guard lock(mutex_);
        if (terminal_ || !admission_pending_ || source_ == nullptr) {
            return false;
        }
        admission_pending_ = false;
        source = source_;
    }
    source->acknowledge_inbox();
    return true;
}

ListenerHandshakeActorSnapshot ListenerHandshakeActor::snapshot() const noexcept
{
    std::lock_guard lock(mutex_);
    return {
        .queued_results = size_,
        .failure = failure_,
        .started = started_,
        .admission_pending = admission_pending_,
        .terminal = terminal_,
    };
}

void ListenerHandshakeActor::run_task(void* context) noexcept
{
    static_cast<ListenerHandshakeActor*>(context)->run();
}

void ListenerHandshakeActor::source_ready(void* context) noexcept
{
    static_cast<ListenerHandshakeActor*>(context)->request_run();
}

void ListenerHandshakeActor::request_run() noexcept
{
    std::shared_ptr<ListenerHandshakeActor> owner;
    {
        std::lock_guard lock(mutex_);
        if (terminal_) {
            return;
        }
        pending_ = true;
        if (scheduled_) {
            return;
        }
        scheduled_ = true;
    }
    try {
        owner = shared_from_this();
    } catch (...) {
        finish_failure(ListenerHandshakeDispatchStatus::invalid);
        return;
    }
    const RuntimeScheduler::SubmitStatus status = scheduler_->submit(affinity_,
        {
            .function = run_task,
            .context = std::move(owner),
        });
    if (status != RuntimeScheduler::SubmitStatus::accepted) {
        finish_failure(dispatch_status(status));
    }
}

void ListenerHandshakeActor::run() noexcept
{
    {
        std::lock_guard lock(mutex_);
        active_thread_ = std::this_thread::get_id();
    }
    for (;;) {
        bool close_requested = false;
        {
            std::lock_guard lock(mutex_);
            pending_ = false;
            close_requested = close_requested_;
        }
        if (close_requested) {
            finish_closed();
        } else {
            drain_source();
        }

        std::lock_guard lock(mutex_);
        if (terminal_) {
            scheduled_ = false;
            active_thread_ = {};
            completed_.notify_all();
            return;
        }
        if (!pending_) {
            scheduled_ = false;
            active_thread_ = {};
            return;
        }
    }
}

void ListenerHandshakeActor::drain_source() noexcept
{
    std::shared_ptr<ListenerHandshakeEventSource> source;
    {
        std::lock_guard lock(mutex_);
        source = source_;
    }
    ListenerHandshakeSourceEvent event;
    while (source != nullptr && source->try_pop(event)) {
        switch (event.kind) {
        case ListenerHandshakeSourceEventKind::inbox_ready:
            process_inbox();
            break;
        case ListenerHandshakeSourceEventKind::failure:
            finish_failure(event.failure);
            return;
        case ListenerHandshakeSourceEventKind::closed:
            finish_closed();
            return;
        }
        if (snapshot().terminal) {
            return;
        }
    }
}

void ListenerHandshakeActor::process_inbox() noexcept
{
    std::shared_ptr<ListenerHandshakeEventSource> source;
    {
        std::lock_guard lock(mutex_);
        source = source_;
    }
    if (source == nullptr) {
        finish_failure(ListenerHandshakeDispatchStatus::invalid);
        return;
    }
    HandshakeEnvelope envelope;
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
    ListenerHandshakeAdmissionStep step =
        steps_.receive(envelope, time_window_(time_window_context_.get()));
    if (step.kind == ListenerHandshakeAdmissionStepKind::admit) {
        (void)publish_step(std::move(step));
        return;
    }
    if (step.kind == ListenerHandshakeAdmissionStepKind::send) {
        if (!publish_step(std::move(step))) {
            return;
        }
    }
    source->acknowledge_inbox();
}

bool ListenerHandshakeActor::publish_step(
    ListenerHandshakeAdmissionStep step) noexcept
{
    ListenerHandshakeDispatchStatus failure =
        ListenerHandshakeDispatchStatus::full;
    bool published = false;
    {
        std::lock_guard lock(mutex_);
        if (terminal_) {
            return false;
        }
        if (size_ == results_.size()) {
            // The consumer is unable to keep up. Discard every unconsumed
            // action and expose one terminal failure instead of silently
            // losing a response or admitted connection.
        } else {
            const bool admission =
                step.kind == ListenerHandshakeAdmissionStepKind::admit;
            if (admission && admission_pending_) {
                failure = ListenerHandshakeDispatchStatus::invalid;
            } else {
                results_[(head_ + size_) % results_.size()] = {
                    .kind = ListenerHandshakeActorResultKind::step,
                    .step = std::move(step),
                };
                admission_pending_ = admission;
                ++size_;
                published = true;
            }
        }
    }
    if (published) {
        ready_.notify_one();
        notify_result_ready();
        return true;
    }
    finish_failure(failure);
    return false;
}

void ListenerHandshakeActor::finish_failure(
    ListenerHandshakeDispatchStatus failure) noexcept
{
    finish({
        .kind = ListenerHandshakeActorResultKind::failure,
        .failure = failure,
    });
}

void ListenerHandshakeActor::finish_closed() noexcept
{
    (void)steps_.close();
    finish({.kind = ListenerHandshakeActorResultKind::closed});
}

void ListenerHandshakeActor::finish(
    ListenerHandshakeActorResult terminal) noexcept
{
    std::shared_ptr<ListenerHandshakeEventSource> source;
    {
        std::lock_guard lock(mutex_);
        if (terminal_) {
            return;
        }
        terminal_ = true;
        failure_ = terminal.kind == ListenerHandshakeActorResultKind::failure
            ? terminal.failure
            : ListenerHandshakeDispatchStatus::completed;
        head_ = 0U;
        size_ = 1U;
        admission_pending_ = false;
        results_[0] = terminal;
        source = std::move(source_);
    }
    if (source != nullptr) {
        source->clear_event_ready_handler();
        source->stop();
    }
    if (inbox_ != nullptr) {
        inbox_->close();
    }
    ready_.notify_all();
    completed_.notify_all();
    notify_result_ready();
}

void ListenerHandshakeActor::notify_result_ready() noexcept
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

} // namespace robotweax::srt::compat
