// SPDX-License-Identifier: MIT
#pragma once
#include "compat/hybrid/hybrid_receiver.hpp"

#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_HYBRID_CONTROL
namespace robotweax::srt::compat::hybrid {
// Explicit internal opt-in. Candidate is evidence, never a merge cursor.
class AutomaticFallback
    : public std::enable_shared_from_this<AutomaticFallback> {
public:
    struct Configuration {
        std::uint32_t maximum_operations = 0, cursor_lifetime_us = 0;
    };
    using Request = AutomaticRequest;
    using Cursor = AutomaticCursor;
    struct Provider {
        // Nonblocking trusted local merge state. Called outside all controller,
        // receiver, native protocol, runtime, group and queue locks. Use weak
        // back references. No wire cursor/high water supplies this authority.
        std::shared_ptr<void> owner;
        std::optional<Cursor> (*resolve)(
            void*, const Request&) noexcept = nullptr;
    };
    enum class Phase {
        armed,
        preparing,
        posted,
        confirmed,
        denied,
        unknown,
        unavailable,
        refused,
        exhausted,
        closed
    };
    struct Snapshot {
        Phase phase = Phase::armed;
        ReceiverBridge::RequestState result =
            ReceiverBridge::RequestState::none;
        Outcome outcome = Outcome::ok;
        bool closed = false, budget_exhausted = false;
        std::uint64_t operations = 0, provider_calls = 0, admitted = 0,
                      revision = 0, ticket = 0, position = 0, not_after_us = 0;
    };
    [[nodiscard]] static std::shared_ptr<AutomaticFallback> create(
        std::shared_ptr<ReceiverBridge>, Configuration, Provider) noexcept;
    ~AutomaticFallback();
    // Advances receiver first. At most one provider call/automatic admission;
    // reentrant/concurrent calls return. Host continues pumping receiver/actor.
    void pump() noexcept;
    // Admission is a handoff: already posted receiver transactions retain their
    // original bound and may commit after this closes. No policy rollback.
    void close() noexcept;
    [[nodiscard]] Snapshot snapshot() const noexcept;

private:
    AutomaticFallback(
        std::shared_ptr<ReceiverBridge>, Configuration, Provider) noexcept;
    void refresh() noexcept;
    std::shared_ptr<ReceiverBridge> receiver_;
    Configuration configuration_;
    Provider provider_;
    mutable std::mutex mutex_;
    std::atomic_flag pumping_ = ATOMIC_FLAG_INIT;
    Snapshot state_;
    bool attached_ = false, has_revision_ = false;
};
}
#endif
