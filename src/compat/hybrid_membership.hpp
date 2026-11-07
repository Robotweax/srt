// SPDX-License-Identifier: MIT
#pragma once
#include "compat/hybrid_native.hpp"
#include "robotweax/srt/udp.hpp"
#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_HYBRID_CONTROL
namespace robotweax::srt::compat {
class DatagramChannel;
}
namespace robotweax::srt::compat::hybrid {
// Internal staged-setup owner. It never bypasses ordinary connect admission.
class MembershipRecovery
    : public std::enable_shared_from_this<MembershipRecovery> {
public:
    struct Configuration {
        IpEndpoint interface;
        std::uint32_t maximum_attempts = 0, timeout_us = 0,
                      initial_backoff_us = 0, maximum_backoff_us = 0;
    };
    struct Request {
        Context context;
        IpEndpoint interface;
        std::uint64_t generation = 0, attempt = 0, started_at_us = 0,
                      not_after_us = 0;
    };
    struct Provider {
        // Trusted local native join preparation. No locks held; nonblocking.
        // Return a newly bound, unconnected, actually joined UDP socket. Never
        // publish/start a route here. Weak back references avoid ownership cycles.
        std::shared_ptr<void> owner;
        std::optional<UdpSocket> (*prepare)(
            void*, const Request&) noexcept = nullptr;
    };
    enum class Phase { waiting, preparing, joined, exhausted, expired, closed };
    struct Snapshot {
        Phase phase = Phase::waiting;
        bool closed = false, joined = false, budget_exhausted = false;
        std::uint64_t attempts = 0, failures = 0, not_after_us = 0,
                      next_wakeup_us = UINT64_MAX;
    };
    [[nodiscard]] static std::shared_ptr<MembershipRecovery> create(
        std::shared_ptr<NativeService>, Configuration, Provider) noexcept;
    [[nodiscard]] static std::shared_ptr<MembershipRecovery> create(
        std::shared_ptr<NativeService>, Configuration) noexcept;
    // Default provider: reuse/bind/join on the explicit local interface.
    [[nodiscard]] static std::optional<UdpSocket> prepare(
        const Request&) noexcept;
    ~MembershipRecovery();
    void pump() noexcept;
    // Revokes only this owner's MC route and observations; UC session continues.
    // Does not wait for a provider; late prepared sockets are discarded.
    void close() noexcept;
    [[nodiscard]] Snapshot snapshot() const noexcept;

private:
    MembershipRecovery(std::shared_ptr<NativeService>, Configuration, Provider);
    [[nodiscard]] bool live() const noexcept;
    std::shared_ptr<NativeService> service_;
    std::shared_ptr<ConnectionRuntime> runtime_;
    std::shared_ptr<const void> identity_;
    std::shared_ptr<DatagramChannel> channel_;
    Configuration configuration_;
    Provider provider_;
    Request request_;
    mutable std::mutex mutex_;
    std::atomic_flag pumping_ = ATOMIC_FLAG_INIT;
    Snapshot state_;
    std::uint64_t next_attempt_ = 0, backoff_ = 0;
    mutable std::uint64_t last_now_ = 0;
    mutable bool clock_failed_ = false;
};
}
#endif
