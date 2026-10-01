#include "compat/group_receive_retention.hpp"

#include <algorithm>
#include <array>
#include <utility>
#include <limits>

namespace robotweax::srt::compat {

std::uint64_t GroupReceiveRetention::now(
    const RetainedGroupReceiveBatch& batch) const noexcept
{
    if (batch.now_function != nullptr) {
        return batch.now_function(batch.now_context);
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        ConnectionRuntime::Clock::now() - batch.origin)
                             .count();
    return elapsed > 0 ? static_cast<std::uint64_t>(elapsed) : 0U;
}

std::uint64_t GroupReceiveRetention::delivery(RetainedGroupReceiveBatch& batch,
    const BufferedMessageCopy& message) noexcept
{
    return batch.clock.has_value()
        ? batch.clock->delivery_time(message.timestamp)
        : 0U;
}

void GroupReceiveRetention::fail_locked() noexcept
{
    batches_.clear();
    packets_ = 0;
    bytes_ = 0;
    if (!failed_) {
        failed_ = true;
        if (const auto source = source_.lock()) {
            ReadinessSignal::notify(*source);
        }
    }
}

void GroupReceiveRetention::expire_locked() noexcept
{
    for (const auto& batch : batches_) {
        const auto current = now(batch);
        if (current >= batch.retired_at_microseconds
            && current - batch.retired_at_microseconds
                >= maximum_age_microseconds) {
            // Resource expiry is an explicit terminal receive failure,
            // never permission to skip unread data and continue the stream.
            fail_locked();
            return;
        }
    }
}

void GroupReceiveRetention::discard_before_locked(
    SequenceNumber consumed) noexcept
{
    for (auto& batch : batches_) {
        auto& messages = batch.copies.messages;
        messages.erase(
            std::remove_if(messages.begin(), messages.end(),
                [&](const auto& message) {
                    if (message.first_sequence.distance_from(consumed) >= 0) {
                        return false;
                    }
                    packets_ -= static_cast<std::size_t>(
                        message.next_sequence.distance_from(
                            message.first_sequence));
                    bytes_ -= message.payload.size();
                    return true;
                }),
            messages.end());
    }
    batches_.erase(std::remove_if(batches_.begin(), batches_.end(),
                       [](const auto& batch) {
                           return batch.copies.messages.empty();
                       }),
        batches_.end());
}

void GroupReceiveRetention::retain(
    RetainedGroupReceiveBatch batch, SequenceNumber consumed) noexcept
{
    std::lock_guard lock(mutex_);
    if (closed_ || failed_) {
        return;
    }
    expire_locked();
    if (failed_) {
        return;
    }
    discard_before_locked(consumed);
    if (batch.copies.error != Error::none) {
        fail_locked();
        return;
    }
    std::array<std::pair<std::uint32_t, std::uint32_t>, maximum_packets>
        identities {};
    std::size_t identity_count = 0;
    for (const auto& existing : batches_) {
        for (const auto& retained : existing.copies.messages) {
            identities[identity_count++] = {retained.first_sequence.value(),
                retained.next_sequence.value()};
        }
    }
    const auto end =
        identities.begin() + static_cast<std::ptrdiff_t>(identity_count);
    std::sort(identities.begin(), end);
    auto& messages = batch.copies.messages;
    messages.erase(
        std::remove_if(messages.begin(), messages.end(),
            [&](const auto& message) {
                if (message.first_sequence.distance_from(consumed) < 0) {
                    return true;
                }
                return std::binary_search(identities.begin(), end,
                    std::pair {message.first_sequence.value(),
                        message.next_sequence.value()});
            }),
        messages.end());
    if (messages.empty()) {
        return;
    }
    std::size_t packets = 0;
    std::size_t bytes = 0;
    for (const auto& message : messages) {
        const auto count =
            message.next_sequence.distance_from(message.first_sequence);
        if (count <= 0
            || static_cast<std::size_t>(count) > maximum_packets - packets
            || message.payload.size() > maximum_bytes - bytes) {
            fail_locked();
            return;
        }
        packets += static_cast<std::size_t>(count);
        bytes += message.payload.size();
    }
    if (batches_.size() >= maximum_batches
        || packets > maximum_packets - packets_
        || bytes > maximum_bytes - bytes_) {
        fail_locked();
        return;
    }
    try {
        batches_.push_back(std::move(batch));
        packets_ += packets;
        bytes_ += bytes;
    } catch (...) {
        fail_locked();
    }
}

RuntimeReceiveSnapshot GroupReceiveRetention::snapshot(
    SequenceNumber expected, bool retire_consumed_prefix) noexcept
{
    std::lock_guard lock(mutex_);
    expire_locked();
    if (retire_consumed_prefix) {
        discard_before_locked(expected);
    }
    RuntimeReceiveSnapshot result {
        .floor_sequence = expected, .terminal = true};
    RetainedGroupReceiveBatch* selected_batch = nullptr;
    const BufferedMessageCopy* selected = nullptr;
    for (auto& batch : batches_) {
        for (const auto& message : batch.copies.messages) {
            if (message.first_sequence.distance_from(expected) < 0) {
                continue;
            }
            if (selected == nullptr
                || message.first_sequence.distance_from(
                       selected->first_sequence)
                    < 0) {
                selected = &message;
                selected_batch = &batch;
            }
        }
    }
    if (selected != nullptr) {
        result.buffered = true;
        result.floor_sequence = selected->first_sequence;
        result.complete_expected = selected->first_sequence == expected;
        const auto due = delivery(*selected_batch, *selected);
        const auto current = now(*selected_batch);
        if (current >= due) {
            result.readable_sequence = selected->first_sequence;
        } else {
            const auto age = current >= selected_batch->retired_at_microseconds
                ? current - selected_batch->retired_at_microseconds
                : 0U;
            const auto wait = std::min(due - current,
                maximum_age_microseconds
                    - std::min(age, maximum_age_microseconds));
            result.next_delivery = ConnectionRuntime::Clock::now()
                + std::chrono::microseconds {static_cast<std::int64_t>(wait)};
        }
    }
    return result;
}

MessageIoResult GroupReceiveRetention::receive_message(
    std::span<std::byte> destination, SequenceNumber expected) noexcept
{
    std::lock_guard lock(mutex_);
    expire_locked();
    if (closed_) {
        return {.status = MessageIoStatus::local_closed};
    }
    if (failed_) {
        return {.status = MessageIoStatus::broken};
    }
    discard_before_locked(expected);
    for (auto& batch : batches_) {
        auto& messages = batch.copies.messages;
        for (auto it = messages.begin(); it != messages.end(); ++it) {
            if (it->first_sequence != expected) {
                continue;
            }
            const auto due = delivery(batch, *it);
            if (now(batch) < due) {
                return {.status = MessageIoStatus::would_block};
            }
            if (destination.size() < it->payload.size()) {
                return {.status = MessageIoStatus::buffer_too_small};
            }
            std::copy(
                it->payload.begin(), it->payload.end(), destination.begin());
            const auto source_time =
                due <= static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max())
                    && batch.origin_epoch_microseconds
                        <= std::numeric_limits<std::int64_t>::max()
                            - static_cast<std::int64_t>(due)
                ? batch.origin_epoch_microseconds
                    + static_cast<std::int64_t>(due)
                : 0;
            MessageIoResult result {
                .status = MessageIoStatus::success,
                .bytes = it->payload.size(),
                .message_number = it->message_number,
                .first_sequence = it->first_sequence,
                .next_sequence = it->next_sequence,
                .source_time_microseconds = source_time,
            };
            packets_ -= static_cast<std::size_t>(
                it->next_sequence.distance_from(it->first_sequence));
            bytes_ -= it->payload.size();
            messages.erase(it);
            batches_.erase(std::remove_if(batches_.begin(), batches_.end(),
                               [](const auto& item) {
                                   return item.copies.messages.empty();
                               }),
                batches_.end());
            return result;
        }
    }
    return {.status = MessageIoStatus::would_block};
}

void GroupReceiveRetention::discard_before(SequenceNumber consumed) noexcept
{
    std::lock_guard lock(mutex_);
    discard_before_locked(consumed);
}

bool GroupReceiveRetention::failed() noexcept
{
    std::lock_guard lock(mutex_);
    expire_locked();
    return failed_;
}

void GroupReceiveRetention::fail() noexcept
{
    std::lock_guard lock(mutex_);
    fail_locked();
}

void GroupReceiveRetention::close() noexcept
{
    std::lock_guard lock(mutex_);
    batches_.clear();
    packets_ = 0;
    bytes_ = 0;
    closed_ = true;
}

} // namespace robotweax::srt::compat
