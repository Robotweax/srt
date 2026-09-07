#include "compat/caller_handshake_sources.hpp"

#include <utility>

namespace robotweax::srt::compat {
namespace {

[[nodiscard]] CallerHandshakeDispatchStatus dispatch_status(
    RuntimeScheduler::SubmitStatus status) noexcept
{
    switch (status) {
    case RuntimeScheduler::SubmitStatus::accepted:
        return CallerHandshakeDispatchStatus::completed;
    case RuntimeScheduler::SubmitStatus::full:
        return CallerHandshakeDispatchStatus::full;
    case RuntimeScheduler::SubmitStatus::stopped:
        return CallerHandshakeDispatchStatus::stopped;
    case RuntimeScheduler::SubmitStatus::invalid:
        return CallerHandshakeDispatchStatus::invalid;
    }
    return CallerHandshakeDispatchStatus::invalid;
}

} // namespace

struct CallerHandshakeEventSource::TimerContext {
    std::weak_ptr<CallerHandshakeEventSource> source;
    CallerHandshakeSourceEventKind kind =
        CallerHandshakeSourceEventKind::failure;
    std::uint64_t generation = 0;
};

CallerHandshakeEventSource::CallerHandshakeEventSource(
    RuntimeScheduler& scheduler, std::uint64_t affinity,
    std::shared_ptr<DatagramInbox> inbox) noexcept
    : scheduler_(scheduler)
    , affinity_(affinity)
    , inbox_(std::move(inbox))
{
}

CallerHandshakeEventSource::~CallerHandshakeEventSource()
{
    stop();
}

CallerHandshakeDispatchStatus CallerHandshakeEventSource::start(
    std::chrono::steady_clock::time_point overall_deadline) noexcept
{
    const std::shared_ptr<CallerHandshakeEventSource> owner =
        weak_from_this().lock();
    if (owner == nullptr || inbox_ == nullptr) {
        return CallerHandshakeDispatchStatus::invalid;
    }
    {
        std::lock_guard lock(mutex_);
        if (started_ || stopped_ || closed_) {
            return CallerHandshakeDispatchStatus::invalid;
        }
        started_ = true;
    }
    if (!inbox_->set_ready_handler(inbox_ready, owner)) {
        stop();
        return CallerHandshakeDispatchStatus::invalid;
    }
    CallerHandshakeDispatchStatus readiness_status =
        CallerHandshakeDispatchStatus::completed;
    {
        std::lock_guard lock(mutex_);
        handler_registered_ = true;
        readiness_status = failure_;
    }
    if (readiness_status != CallerHandshakeDispatchStatus::completed) {
        stop();
        return readiness_status;
    }

    std::shared_ptr<TimerContext> timer;
    try {
        timer = std::make_shared<TimerContext>(TimerContext {
            .source = owner,
            .kind = CallerHandshakeSourceEventKind::overall_timeout,
        });
    } catch (...) {
        stop();
        return CallerHandshakeDispatchStatus::allocation_failed;
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
        return CallerHandshakeDispatchStatus::stopped;
    }
    return CallerHandshakeDispatchStatus::completed;
}

CallerHandshakeDispatchStatus CallerHandshakeEventSource::arm_retry(
    std::uint32_t timeout_milliseconds) noexcept
{
    return arm_retry_at(std::chrono::steady_clock::now()
        + std::chrono::milliseconds {timeout_milliseconds});
}

CallerHandshakeDispatchStatus CallerHandshakeEventSource::arm_retry_at(
    std::chrono::steady_clock::time_point deadline) noexcept
{
    RuntimeScheduler::TimerToken previous;
    std::uint64_t generation = 0;
    {
        std::lock_guard lock(mutex_);
        if (!started_ || stopped_ || closed_) {
            return CallerHandshakeDispatchStatus::stopped;
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
            .kind = CallerHandshakeSourceEventKind::retry_timer,
            .generation = generation,
        });
    } catch (...) {
        fail(CallerHandshakeDispatchStatus::allocation_failed);
        return CallerHandshakeDispatchStatus::allocation_failed;
    }
    const RuntimeScheduler::ScheduleResult scheduled =
        scheduler_.schedule_at(affinity_, deadline,
            {
                .function = publish_timer,
                .context = std::move(timer),
            });
    if (scheduled.status != RuntimeScheduler::SubmitStatus::accepted) {
        const CallerHandshakeDispatchStatus failure =
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
        return CallerHandshakeDispatchStatus::stopped;
    }
    return CallerHandshakeDispatchStatus::completed;
}

void CallerHandshakeEventSource::cancel_retry() noexcept
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

CallerHandshakeSourceEvent CallerHandshakeEventSource::wait() noexcept
{
    std::unique_lock lock(mutex_);
    for (;;) {
        ready_.wait(lock, [this] {
            return failure_ != CallerHandshakeDispatchStatus::completed
                || closed_ || size_ != 0U || stopped_;
        });
        if (failure_ != CallerHandshakeDispatchStatus::completed) {
            return {
                .kind = CallerHandshakeSourceEventKind::failure,
                .failure = failure_,
            };
        }
        if (closed_) {
            return {.kind = CallerHandshakeSourceEventKind::closed};
        }
        if (size_ == 0U) {
            return {.kind = CallerHandshakeSourceEventKind::closed};
        }
        const CallerHandshakeSourceEvent event = events_[head_];
        head_ = (head_ + 1U) % events_.size();
        --size_;
        if (event.kind == CallerHandshakeSourceEventKind::retry_timer
            && event.generation != retry_generation_) {
            continue;
        }
        return event;
    }
}

bool CallerHandshakeEventSource::try_pop(
    CallerHandshakeSourceEvent& event) noexcept
{
    std::lock_guard lock(mutex_);
    if (failure_ != CallerHandshakeDispatchStatus::completed) {
        event = {
            .kind = CallerHandshakeSourceEventKind::failure,
            .failure = failure_,
        };
        return true;
    }
    if (closed_) {
        event = {.kind = CallerHandshakeSourceEventKind::closed};
        return true;
    }
    while (size_ != 0U) {
        event = events_[head_];
        head_ = (head_ + 1U) % events_.size();
        --size_;
        if (event.kind != CallerHandshakeSourceEventKind::retry_timer
            || event.generation == retry_generation_) {
            return true;
        }
    }
    return false;
}

bool CallerHandshakeEventSource::set_event_ready_handler(
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
        notify = failure_ != CallerHandshakeDispatchStatus::completed || closed_
            || size_ != 0U;
    }
    if (notify) {
        notify_event_ready();
    }
    return true;
}

void CallerHandshakeEventSource::clear_event_ready_handler() noexcept
{
    std::lock_guard lock(mutex_);
    event_ready_handler_ = nullptr;
    event_ready_context_.reset();
}

void CallerHandshakeEventSource::acknowledge_inbox() noexcept
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

void CallerHandshakeEventSource::close() noexcept
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

void CallerHandshakeEventSource::stop() noexcept
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

void CallerHandshakeEventSource::inbox_ready(void* context) noexcept
{
    static_cast<CallerHandshakeEventSource*>(context)->request_inbox();
}

void CallerHandshakeEventSource::publish_inbox_ready(void* context) noexcept
{
    auto& source = *static_cast<CallerHandshakeEventSource*>(context);
    source.enqueue({
        .kind = CallerHandshakeSourceEventKind::inbox_ready,
    });
}

void CallerHandshakeEventSource::publish_timer(void* context) noexcept
{
    auto& timer = *static_cast<TimerContext*>(context);
    if (const auto source = timer.source.lock(); source != nullptr) {
        source->enqueue({
            .kind = timer.kind,
            .generation = timer.generation,
        });
    }
}

void CallerHandshakeEventSource::request_inbox() noexcept
{
    const std::shared_ptr<CallerHandshakeEventSource> owner =
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

void CallerHandshakeEventSource::enqueue(
    CallerHandshakeSourceEvent event) noexcept
{
    {
        std::lock_guard lock(mutex_);
        if (stopped_ || closed_) {
            return;
        }
        if (size_ == events_.size()) {
            failure_ = CallerHandshakeDispatchStatus::full;
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

void CallerHandshakeEventSource::fail(
    CallerHandshakeDispatchStatus failure) noexcept
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

void CallerHandshakeEventSource::notify_event_ready() noexcept
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

void CallerHandshakeEventSource::cancel_timer(
    RuntimeScheduler::TimerToken token) noexcept
{
    if (token.valid()) {
        (void)scheduler_.cancel_timer(token);
    }
}

} // namespace robotweax::srt::compat
