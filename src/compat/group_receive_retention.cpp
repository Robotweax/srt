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

void GroupReceiveRetention::drop_batch_locked(std::size_t index) noexcept
{
    auto& batch = batches_[index];
    for (const auto& message : batch.copies.messages) {
        packets_ -= static_cast<std::size_t>(
            message.next_sequence.distance_from(message.first_sequence));
        bytes_ -= message.payload.size();
    }
    batches_.erase(batches_.begin() + static_cast<std::ptrdiff_t>(index));
}

void GroupReceiveRetention::expire_locked() noexcept
{
    bool dropped = false;
    for (std::size_t index = 0; index < batches_.size();) {
        const auto& batch = batches_[index];
        const auto current = now(batch);
        if (current >= batch.retired_at_microseconds
            && current - batch.retired_at_microseconds
                >= maximum_age_microseconds) {
            // Unread for the whole retention lifetime: the data is lost for
            // this receiver and the group skips it like any other gap.
            drop_batch_locked(index);
            dropped = true;
            continue;
        }
        ++index;
    }
    if (dropped) {
        if (const auto source = source_.lock()) {
            ReadinessSignal::notify(*source);
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
    if (closed_) {
        return;
    }
    expire_locked();
    discard_before_locked(consumed);
    if (batch.copies.error != Error::none) {
        // The copy is incomplete; what could not be copied is lost for this
        // receiver, like data that never reached a member.
        return;
    }
    std::vector<std::pair<std::uint32_t, std::uint32_t>> identities;
    try {
        identities.reserve(packets_);
        for (const auto& existing : batches_) {
            for (const auto& retained : existing.copies.messages) {
                identities.emplace_back(retained.first_sequence.value(),
                    retained.next_sequence.value());
            }
        }
    } catch (...) {
        return;
    }
    std::sort(identities.begin(), identities.end());
    auto& messages = batch.copies.messages;
    std::size_t packets = 0;
    std::size_t bytes = 0;
    messages.erase(
        std::remove_if(messages.begin(), messages.end(),
            [&](const auto& message) {
                if (message.first_sequence.distance_from(consumed) < 0) {
                    return true;
                }
                if (std::binary_search(identities.begin(), identities.end(),
                        std::pair {message.first_sequence.value(),
                            message.next_sequence.value()})) {
                    return true;
                }
                const auto count =
                    message.next_sequence.distance_from(message.first_sequence);
                // A message that can never fit the storage is dropped on
                // its own; the rest of the batch stays retainable.
                if (count <= 0
                    || static_cast<std::size_t>(count) > maximum_packets
                    || message.payload.size() > maximum_bytes) {
                    return true;
                }
                packets += static_cast<std::size_t>(count);
                bytes += message.payload.size();
                return false;
            }),
        messages.end());
    // Keep the newest closed member's prefix: it is the one the group
    // cursor reaches last. Oldest retained batches make room for it.
    while (!messages.empty()
        && (packets > maximum_packets - packets_
            || bytes > maximum_bytes - bytes_)) {
        if (batches_.empty()) {
            // The batch alone exceeds the bounds: drop its highest messages.
            const auto& last = messages.back();
            packets -= static_cast<std::size_t>(
                last.next_sequence.distance_from(last.first_sequence));
            bytes -= last.payload.size();
            messages.pop_back();
            continue;
        }
        drop_batch_locked(0);
    }
    while (!messages.empty() && batches_.size() >= maximum_batches) {
        drop_batch_locked(0);
    }
    if (messages.empty()) {
        return;
    }
    try {
        batches_.push_back(std::move(batch));
        packets_ += packets;
        bytes_ += bytes;
    } catch (...) {
        // Allocation failure: the prefix is lost for this receiver.
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

void GroupReceiveRetention::close() noexcept
{
    std::lock_guard lock(mutex_);
    batches_.clear();
    packets_ = 0;
    bytes_ = 0;
    closed_ = true;
}

} // namespace robotweax::srt::compat
