#include "compat/listener_accept_queue.hpp"

#include <algorithm>

namespace robotweax::srt::compat {

ListenerAcceptQueue::ListenerAcceptQueue(std::size_t capacity)
    : entries_(std::max<std::size_t>(capacity, 1U))
{
}

ListenerAcceptPublishStatus ListenerAcceptQueue::publish(
    ListenerAcceptedConnection connection,
    std::atomic_size_t* observed_size) noexcept
{
    {
        std::lock_guard lock(mutex_);
        if (closed_) {
            return ListenerAcceptPublishStatus::closed;
        }
        if (size_ == entries_.size()) {
            return ListenerAcceptPublishStatus::full;
        }
        entries_[(head_ + size_) % entries_.size()] = connection;
        ++size_;
        publish_size(observed_size);
    }
    changed_.notify_all();
    return ListenerAcceptPublishStatus::published;
}

bool ListenerAcceptQueue::pop(bool blocking,
    ListenerAcceptedConnection& connection,
    std::atomic_size_t* observed_size) noexcept
{
    std::unique_lock lock(mutex_);
    if (blocking) {
        changed_.wait(lock, [this] {
            return closed_ || size_ != 0U;
        });
    }
    if (size_ == 0U) {
        return false;
    }
    connection = entries_[head_];
    head_ = (head_ + 1U) % entries_.size();
    --size_;
    publish_size(observed_size);
    lock.unlock();
    changed_.notify_all();
    return true;
}

void ListenerAcceptQueue::close(std::atomic_size_t* observed_size) noexcept
{
    {
        std::lock_guard lock(mutex_);
        closed_ = true;
        publish_size(observed_size);
    }
    changed_.notify_all();
}

ListenerAcceptQueueSnapshot ListenerAcceptQueue::snapshot() const noexcept
{
    std::lock_guard lock(mutex_);
    return {
        .capacity = entries_.size(),
        .size = size_,
        .closed = closed_,
    };
}

void ListenerAcceptQueue::publish_size(
    std::atomic_size_t* observed_size) const noexcept
{
    if (observed_size != nullptr) {
        observed_size->store(size_, std::memory_order_release);
    }
}

} // namespace robotweax::srt::compat
