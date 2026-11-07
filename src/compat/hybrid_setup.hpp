// SPDX-License-Identifier: MIT
#pragma once
#include "compat/hybrid_bridge.hpp"
#include "compat/hybrid_receiver.hpp"
#include "compat/hybrid_membership.hpp"
#include "srt/srt.h"
#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_HYBRID_CONTROL
namespace robotweax::srt::compat {
struct SocketRecord;
}
namespace robotweax::srt::compat::hybrid {
// Explicit internal provisioning; no numeric public C option or default enable.
struct StartupOptions {
    NativeOptions native;
    ReceiverBridge::Configuration receiver;
    CoordinatorBridge::Configuration source;
    MembershipRecovery::Configuration membership;
    MembershipRecovery::Provider join;
    std::uint32_t admission_timeout_us = 0, poll_interval_us = 0;
};
[[nodiscard]] bool provision_startup(SRTSOCKET, StartupOptions) noexcept;
class StartupSession : public std::enable_shared_from_this<StartupSession> {
public:
    enum class Phase {
        authenticating,
        admitting,
        ready,
        connected,
        serving,
        failed,
        closed
    };
    struct Snapshot {
        Phase phase = Phase::authenticating;
        bool caller = false, closed = false, published = false;
        std::uint64_t not_after_us = 0;
        Outcome outcome = Outcome::ok;
    };
    [[nodiscard]] static std::shared_ptr<StartupSession> start(
        std::shared_ptr<ConnectionRuntime>,
        std::shared_ptr<const StartupOptions>,
        std::shared_ptr<GroupSendCoordinator>,
        std::uint64_t not_after_us) noexcept;
    ~StartupSession();
    void close() noexcept;
    [[nodiscard]] Snapshot snapshot() const noexcept;
    // Caller publication is serialized against owner close and SocketRecord close.
    [[nodiscard]] bool publish_ready(SocketRecord&) noexcept;
    [[nodiscard]] std::shared_ptr<ReceiverBridge> receiver() const noexcept;
    [[nodiscard]] std::shared_ptr<MembershipRecovery>
    membership() const noexcept;

private:
    StartupSession(std::shared_ptr<ConnectionRuntime>,
        std::shared_ptr<const StartupOptions>,
        std::shared_ptr<GroupSendCoordinator>, std::shared_ptr<NativeService>,
        bool);
    struct Work {
        std::weak_ptr<StartupSession> owner;
    };
    static void run(void*) noexcept;
    void drive() noexcept;
    std::shared_ptr<ConnectionRuntime> runtime_;
    std::shared_ptr<const StartupOptions> options_;
    std::shared_ptr<GroupSendCoordinator> actor_;
    std::shared_ptr<NativeService> native_;
    std::shared_ptr<DatagramChannel> control_;
    std::shared_ptr<ReceiverBridge> receiver_;
    std::shared_ptr<MembershipRecovery> membership_;
    std::shared_ptr<CoordinatorBridge> source_;
    std::shared_ptr<AuthorizationLease> lease_;
    std::shared_ptr<RuntimeScheduler> scheduler_;
    RuntimeScheduler::ServiceToken service_;
    std::shared_ptr<Work> work_;
    mutable std::mutex mutex_;
    std::atomic_bool cancelled_ = false;
    std::atomic_flag driving_ = ATOMIC_FLAG_INIT;
    Snapshot state_;
    bool requested_ = false;
};
}
#endif
