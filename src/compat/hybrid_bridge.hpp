// SPDX-License-Identifier: MIT
#pragma once
#include "compat/hybrid_native.hpp"

#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_HYBRID_CONTROL
namespace robotweax::srt::compat {
class GroupSendCoordinator;
}
namespace robotweax::srt::compat::hybrid {
// Local policy-store fence, never a wire claim. Revoke BEFORE changing the
// provider's corresponding authorization. Revocation and actor commit serialize;
// revoke also cancels the native owner after releasing this mutex. Single use.
class AuthorizationLease {
public:
    explicit AuthorizationLease(Authorization grant) noexcept
        : grant_(std::move(grant))
    {
    }
    void revoke() noexcept;

private:
    friend class CoordinatorBridge;
    std::mutex mutex_;
    Authorization grant_;
    std::weak_ptr<NativeService> service_;
    bool revoked_ = false, claimed_ = false;
};

// Explicit sender-side fallback adapter. Own the service's application queue
// exclusively; pump outside registry/runtime/group locks. No timer, detector,
// multicast-return policy or automatic attachment is supplied by this adapter.
class CoordinatorBridge {
public:
    struct Identity {
        std::uint64_t stream = 0, member = 0, initial_position = 0;
    };
    struct Configuration {
        std::uint32_t command_timeout_us = 1'000'000;
        std::uint32_t maximum_attempts = 8;
    };
    struct Snapshot {
        bool enrolled = false, closed = false;
        std::uint64_t applied = 0, denied = 0, duplicates = 0, expired = 0,
                      ignored = 0, replies = 0;
        std::uint64_t revision = 0;
    };
    [[nodiscard]] static std::optional<Identity> identity(
        const std::shared_ptr<GroupSendCoordinator>&,
        const std::shared_ptr<ConnectionRuntime>&) noexcept;
    [[nodiscard]] static std::shared_ptr<CoordinatorBridge> create(
        std::shared_ptr<GroupSendCoordinator>, std::shared_ptr<NativeService>,
        std::shared_ptr<AuthorizationLease>, Configuration) noexcept;
    ~CoordinatorBridge();
    void pump() noexcept;
    void close() noexcept;
    [[nodiscard]] Snapshot snapshot() const noexcept;

private:
    friend class NativeService;
    struct Entry {
        Message request, reply;
        std::uint64_t first_received = 0, deadline = 0;
        std::uint32_t sends = 0, busy = 0;
        bool occupied = false, complete = false, pending = false;
    };
    CoordinatorBridge(std::shared_ptr<GroupSendCoordinator>,
        std::shared_ptr<NativeService>, std::shared_ptr<AuthorizationLease>,
        Configuration) noexcept;
    void work() noexcept;
    // Caller retains native protocol ownership. Takes policy and native queue
    // fences, then try-locks publication -> emitter -> runtime -> wire budget.
    // No callbacks, blocking lock acquisition or I/O after the queue fence.
    [[nodiscard]] bool transact(Entry*) noexcept;
    void event(const NativeService::Event&) noexcept;
    void replies() noexcept;
    std::shared_ptr<GroupSendCoordinator> actor_;
    std::shared_ptr<NativeService> service_;
    std::shared_ptr<AuthorizationLease> authorization_;
    Configuration configuration_;
    mutable std::mutex mutex_;
    Snapshot state_;
    std::atomic_bool cancelled_ = false;
    std::array<Entry, 64> entries_ {};
    std::uint64_t highest_ = 0, next_reply_ = 1, source_revision_ = 0;
};
}
#endif
