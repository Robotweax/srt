#pragma once

#include "compat/transport_runtime.hpp"

#include <mutex>
#include <optional>
#include <vector>
#include <utility>

namespace robotweax::srt::compat {

// Receive-only ownership: no socket, runtime, channel, crypto or scheduler.
struct RetainedGroupReceiveBatch {
    BufferedMessageCopies copies;
    std::optional<TsbpdClock> clock;
    ConnectionRuntime::Clock::time_point origin {};
    std::int64_t origin_epoch_microseconds = 0;
    std::uint64_t retired_at_microseconds = 0;
    ConnectionRuntime::NowFunction now_function = nullptr;
    void* now_context = nullptr;
};

// Bounded receive-only storage for complete unread messages of a member that
// was closed explicitly. The bounds (packets, bytes, batches, unread age)
// are enforced by discarding retained data, oldest batch first, never by
// failing the group: data that cannot be retained is receiver-side loss and
// the group skips past it, exactly as it skips a gap no member can supply.
class GroupReceiveRetention {
public:
    explicit GroupReceiveRetention(
        std::weak_ptr<ReadinessSource> source = {}) noexcept
        : source_(std::move(source))
    {
    }
    static constexpr std::size_t maximum_packets = 8'192;
    static constexpr std::size_t maximum_bytes = 11'927'552;
    static constexpr std::size_t maximum_batches = 16;
    static constexpr std::uint64_t maximum_age_microseconds = 120'000'000;

    void retain(
        RetainedGroupReceiveBatch batch, SequenceNumber consumed) noexcept;
    [[nodiscard]] RuntimeReceiveSnapshot snapshot(
        SequenceNumber expected, bool retire_consumed_prefix) noexcept;
    [[nodiscard]] MessageIoResult receive_message(
        std::span<std::byte> destination, SequenceNumber expected) noexcept;
    void discard_before(SequenceNumber consumed) noexcept;
    void close() noexcept;

private:
    [[nodiscard]] std::uint64_t now(
        const RetainedGroupReceiveBatch& batch) const noexcept;
    [[nodiscard]] std::uint64_t delivery(RetainedGroupReceiveBatch& batch,
        const BufferedMessageCopy& message) noexcept;
    void expire_locked() noexcept;
    void discard_before_locked(SequenceNumber consumed) noexcept;
    void drop_batch_locked(std::size_t index) noexcept;
    std::weak_ptr<ReadinessSource> source_;
    std::mutex mutex_;
    std::vector<RetainedGroupReceiveBatch> batches_;
    std::size_t packets_ = 0;
    std::size_t bytes_ = 0;
    bool closed_ = false;
};

} // namespace robotweax::srt::compat
