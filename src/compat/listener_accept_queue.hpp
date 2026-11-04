#pragma once

#include "robotweax/srt/udp.hpp"
#include "srt/srt.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <memory>
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
    std::size_t reserved = 0;
    bool closed = false;
};

// Construction-sized completed-accept handoff. Producers never wait for an
// application accept call: saturation is explicit and therefore cannot block a
// bounded completion owner or a future affinity-shard owner.
class ListenerAcceptQueue {
    struct State;

public:
    // Move-only admission ownership. Shared queue state survives listener close
    // until every setup has released its slot, without retaining the listener.
    class Reservation {
    public:
        Reservation() noexcept = default;
        ~Reservation();
        Reservation(Reservation&& other) noexcept;
        Reservation& operator=(Reservation&& other) noexcept;
        Reservation(const Reservation&) = delete;
        Reservation& operator=(const Reservation&) = delete;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return reserved_;
        }
        [[nodiscard]] bool try_acquire() noexcept;
        void release() noexcept;

    private:
        friend class ListenerAcceptQueue;
        explicit Reservation(std::shared_ptr<State> state) noexcept;
        std::shared_ptr<State> state_;
        bool reserved_ = false;
    };
    explicit ListenerAcceptQueue(std::size_t capacity);
    ~ListenerAcceptQueue();

    ListenerAcceptQueue(const ListenerAcceptQueue&) = delete;
    ListenerAcceptQueue& operator=(const ListenerAcceptQueue&) = delete;

    [[nodiscard]] Reservation reserve() noexcept;
    [[nodiscard]] ListenerAcceptPublishStatus publish(Reservation&& reservation,
        ListenerAcceptedConnection connection,
        std::atomic_size_t* observed_size = nullptr) noexcept;
    [[nodiscard]] ListenerAcceptPublishStatus publish(
        ListenerAcceptedConnection connection,
        std::atomic_size_t* observed_size = nullptr) noexcept;
    [[nodiscard]] bool pop(bool blocking,
        ListenerAcceptedConnection& connection,
        std::atomic_size_t* observed_size = nullptr) noexcept;
    void close(std::atomic_size_t* observed_size = nullptr) noexcept;

    [[nodiscard]] ListenerAcceptQueueSnapshot snapshot() const noexcept;

private:
    std::shared_ptr<State> state_;
};

} // namespace robotweax::srt::compat
