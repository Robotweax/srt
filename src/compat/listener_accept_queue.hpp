#pragma once

#include "robotweax/srt/udp.hpp"
#include "srt/srt.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <vector>

namespace robotweax::srt::compat {

struct ListenerAcceptedConnection {
    SRTSOCKET handle = SRT_INVALID_SOCK;
    IpEndpoint peer {};
};

enum class ListenerAcceptPublishStatus {
    published,
    full,
    closed,
};

struct ListenerAcceptQueueSnapshot {
    std::size_t capacity = 0;
    std::size_t size = 0;
    bool closed = false;
};

// Construction-sized completed-accept handoff. Producers never wait for an
// application accept call: saturation is explicit and therefore cannot block a
// bounded completion owner or a future affinity-shard owner.
class ListenerAcceptQueue {
public:
    explicit ListenerAcceptQueue(std::size_t capacity);

    ListenerAcceptQueue(const ListenerAcceptQueue&) = delete;
    ListenerAcceptQueue& operator=(const ListenerAcceptQueue&) = delete;

    [[nodiscard]] ListenerAcceptPublishStatus publish(
        ListenerAcceptedConnection connection,
        std::atomic_size_t* observed_size = nullptr) noexcept;
    [[nodiscard]] bool pop(bool blocking,
        ListenerAcceptedConnection& connection,
        std::atomic_size_t* observed_size = nullptr) noexcept;
    void close(std::atomic_size_t* observed_size = nullptr) noexcept;

    [[nodiscard]] ListenerAcceptQueueSnapshot snapshot() const noexcept;

private:
    void publish_size(std::atomic_size_t* observed_size) const noexcept;

    std::vector<ListenerAcceptedConnection> entries_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::size_t head_ = 0;
    std::size_t size_ = 0;
    bool closed_ = false;
};

} // namespace robotweax::srt::compat
