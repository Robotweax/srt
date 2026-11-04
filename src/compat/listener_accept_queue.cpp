#include "compat/listener_accept_queue.hpp"

#include <algorithm>
#include <utility>

namespace robotweax::srt::compat {

struct ListenerAcceptQueue::State {
    explicit State(std::size_t capacity)
        : entries(std::max<std::size_t>(capacity, 1U))
    {
    }

    void publish_size(std::atomic_size_t* observed_size) const noexcept
    {
        if (observed_size != nullptr) {
            observed_size->store(size, std::memory_order_release);
        }
    }

    std::vector<ListenerAcceptedConnection> entries;
    std::mutex mutex;
    std::condition_variable changed;
    std::size_t head = 0;
    std::size_t size = 0;
    std::size_t reserved = 0;
    bool closed = false;
};

ListenerAcceptQueue::Reservation::Reservation(
    std::shared_ptr<State> state) noexcept
    : state_(std::move(state))
{
}

ListenerAcceptQueue::Reservation::~Reservation()
{
    release();
}

ListenerAcceptQueue::Reservation::Reservation(Reservation&& other) noexcept
    : state_(std::move(other.state_))
    , reserved_(std::exchange(other.reserved_, false))
{
}

ListenerAcceptQueue::Reservation& ListenerAcceptQueue::Reservation::operator=(
    Reservation&& other) noexcept
{
    if (this != &other) {
        release();
        state_ = std::move(other.state_);
        reserved_ = std::exchange(other.reserved_, false);
    }
    return *this;
}

bool ListenerAcceptQueue::Reservation::try_acquire() noexcept
{
    if (state_ == nullptr)
        return false;
    std::lock_guard lock(state_->mutex);
    if (state_->closed)
        return false;
    if (reserved_)
        return true;
    if (state_->reserved == state_->entries.size() - state_->size)
        return false;
    ++state_->reserved;
    reserved_ = true;
    return true;
}

void ListenerAcceptQueue::Reservation::release() noexcept
{
    if (!reserved_)
        return;
    std::lock_guard lock(state_->mutex);
    --state_->reserved;
    reserved_ = false;
}

ListenerAcceptQueue::ListenerAcceptQueue(std::size_t capacity)
    : state_(std::make_shared<State>(capacity))
{
}

ListenerAcceptQueue::~ListenerAcceptQueue()
{
    close();
}

ListenerAcceptQueue::Reservation ListenerAcceptQueue::reserve() noexcept
{
    Reservation reservation {state_};
    (void)reservation.try_acquire();
    return reservation;
}

ListenerAcceptPublishStatus ListenerAcceptQueue::publish(
    ListenerAcceptedConnection connection,
    std::atomic_size_t* observed_size) noexcept
{
    return publish(reserve(), connection, observed_size);
}

ListenerAcceptPublishStatus ListenerAcceptQueue::publish(
    Reservation&& reservation, ListenerAcceptedConnection connection,
    std::atomic_size_t* observed_size) noexcept
{
    if (reservation.state_ != state_)
        return ListenerAcceptPublishStatus::full;
    {
        std::lock_guard lock(state_->mutex);
        const bool reserved = reservation.reserved_;
        if (reserved) {
            --state_->reserved;
            reservation.reserved_ = false;
        }
        if (state_->closed)
            return ListenerAcceptPublishStatus::closed;
        if (!reserved)
            return ListenerAcceptPublishStatus::full;
        state_
            ->entries[(state_->head + state_->size) % state_->entries.size()] =
            connection;
        ++state_->size;
        state_->publish_size(observed_size);
    }
    state_->changed.notify_all();
    return ListenerAcceptPublishStatus::published;
}

bool ListenerAcceptQueue::pop(bool blocking,
    ListenerAcceptedConnection& connection,
    std::atomic_size_t* observed_size) noexcept
{
    std::unique_lock lock(state_->mutex);
    if (blocking) {
        state_->changed.wait(lock, [this] {
            return state_->closed || state_->size != 0U;
        });
    }
    if (state_->size == 0U)
        return false;
    connection = state_->entries[state_->head];
    state_->head = (state_->head + 1U) % state_->entries.size();
    --state_->size;
    state_->publish_size(observed_size);
    lock.unlock();
    state_->changed.notify_all();
    return true;
}

void ListenerAcceptQueue::close(std::atomic_size_t* observed_size) noexcept
{
    {
        std::lock_guard lock(state_->mutex);
        state_->closed = true;
        state_->publish_size(observed_size);
    }
    state_->changed.notify_all();
}

ListenerAcceptQueueSnapshot ListenerAcceptQueue::snapshot() const noexcept
{
    std::lock_guard lock(state_->mutex);
    return {.capacity = state_->entries.size(),
        .size = state_->size,
        .reserved = state_->reserved,
        .closed = state_->closed};
}

} // namespace robotweax::srt::compat
