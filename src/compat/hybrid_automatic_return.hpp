// SPDX-License-Identifier: MIT
#pragma once
#include "compat/hybrid_receiver.hpp"

#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_HYBRID_CONTROL
namespace robotweax::srt::compat::hybrid {
// Explicit internal opt-in; uses only the bounded local exact receive cache.
class AutomaticReturn : public std::enable_shared_from_this<AutomaticReturn> {
public:
    struct Configuration {
        std::uint32_t maximum_operations = 0, operation_timeout_us = 0,
                      coverage_packets = 0, minimum_coverage_us = 0,
                      proof_lifetime_us = 0;
    };
    enum class Phase {
        armed,
        overlap_pending,
        collecting,
        multicast_pending,
        confirmed,
        denied,
        unknown,
        exhausted,
        closed
    };
    struct Snapshot {
        Phase phase = Phase::armed;
        ReceiverBridge::RequestState result =
                                         ReceiverBridge::RequestState::none,
                                     overlap_result =
                                         ReceiverBridge::RequestState::none,
                                     multicast_result =
                                         ReceiverBridge::RequestState::none;
        Outcome outcome = Outcome::ok;
        bool closed = false, budget_exhausted = false;
        std::uint64_t operations = 0, overlap_admitted = 0,
                      multicast_admitted = 0, revision = 0, ticket = 0,
                      boundary = 0, through = 0, commit_position = 0,
                      not_after_us = 0;
    };
    [[nodiscard]] static std::shared_ptr<AutomaticReturn> create(
        std::shared_ptr<ReceiverBridge>, Configuration) noexcept;
    ~AutomaticReturn();
    // Host continues pumping source/receiver. No callback, timer or DATA queue.
    void pump() noexcept;
    // Stops new admissions, not already handed-off operations. An unfinished
    // overlap retains UC and expires under the independently bounded source policy.
    void close() noexcept;
    [[nodiscard]] Snapshot snapshot() const noexcept;

private:
    AutomaticReturn(std::shared_ptr<ReceiverBridge>, Configuration) noexcept;
    [[nodiscard]] Snapshot project() const noexcept;
    std::shared_ptr<ReceiverBridge> receiver_;
    Configuration configuration_;
    mutable std::mutex mutex_;
    std::atomic_flag pumping_ = ATOMIC_FLAG_INIT;
    Snapshot state_;
    AutomaticRequest request_;
    bool has_revision_ = false;
};
}
#endif
