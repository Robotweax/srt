// SPDX-License-Identifier: MIT
#include "compat/hybrid_automatic.hpp"
#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_HYBRID_CONTROL
namespace robotweax::srt::compat::hybrid {
static_assert(sizeof(AutomaticFallback) < 2048);
AutomaticFallback::AutomaticFallback(std::shared_ptr<ReceiverBridge> receiver,
    Configuration c, Provider p) noexcept
    : receiver_(std::move(receiver))
    , configuration_(c)
    , provider_(std::move(p))
{
}
std::shared_ptr<AutomaticFallback> AutomaticFallback::create(
    std::shared_ptr<ReceiverBridge> receiver, Configuration c,
    Provider p) noexcept
{
    if (!receiver || !p.owner || !p.resolve || !c.maximum_operations
        || c.maximum_operations > 8 || !c.cursor_lifetime_us
        || c.cursor_lifetime_us > maximum_age_us)
        return {};
    try {
        auto controller = std::shared_ptr<AutomaticFallback>(
            new AutomaticFallback(std::move(receiver), c, std::move(p)));
        if (!controller->receiver_->claim_automatic())
            return {};
        controller->attached_ = true;
        return controller;
    } catch (...) {
        return {};
    }
}
AutomaticFallback::~AutomaticFallback()
{
    close();
}
void AutomaticFallback::close() noexcept
{
    std::lock_guard lock(mutex_);
    state_.closed = true;
    state_.phase = Phase::closed;
}
void AutomaticFallback::refresh() noexcept
{
    const auto receipt = receiver_->automatic_receipt();
    if (receipt.ticket == state_.ticket && state_.ticket) {
        state_.result = receipt.result;
        state_.outcome = receipt.outcome;
        if (!state_.closed) {
            if (receipt.result == ReceiverBridge::RequestState::confirmed)
                state_.phase = Phase::confirmed;
            if (receipt.result == ReceiverBridge::RequestState::denied)
                state_.phase = Phase::denied;
            if (receipt.result == ReceiverBridge::RequestState::unknown)
                state_.phase = Phase::unknown;
        }
    }
    if (receiver_->snapshot().closed) {
        state_.closed = true;
        state_.phase = Phase::closed;
    }
    state_.budget_exhausted =
        state_.operations >= configuration_.maximum_operations;
}
AutomaticFallback::Snapshot AutomaticFallback::snapshot() const noexcept
{
    std::lock_guard lock(mutex_);
    // Receipt projection is read-only; it never admits or pumps another intent.
    auto result = state_;
    const auto receipt = receiver_->automatic_receipt();
    if (result.ticket && receipt.ticket == result.ticket) {
        result.result = receipt.result;
        result.outcome = receipt.outcome;
        if (!result.closed) {
            if (receipt.result == ReceiverBridge::RequestState::confirmed)
                result.phase = Phase::confirmed;
            if (receipt.result == ReceiverBridge::RequestState::denied)
                result.phase = Phase::denied;
            if (receipt.result == ReceiverBridge::RequestState::unknown)
                result.phase = Phase::unknown;
        }
    }
    if (receiver_->snapshot().closed)
        result.closed = true;
    if (result.closed)
        result.phase = Phase::closed;
    result.budget_exhausted =
        result.operations >= configuration_.maximum_operations;
    return result;
}
void AutomaticFallback::pump() noexcept
{
    const auto lifetime = weak_from_this().lock();
    if (!lifetime || pumping_.test_and_set())
        return;
    struct Release {
        std::atomic_flag& flag;
        ~Release()
        {
            flag.clear();
        }
    } release {pumping_};
    {
        std::lock_guard lock(mutex_);
        if (state_.closed)
            return;
    }
    receiver_->pump();
    Request request;
    {
        std::lock_guard lock(mutex_);
        refresh();
        if (state_.closed || !attached_)
            return;
        const auto candidate = receiver_->automatic_candidate();
        if (!candidate)
            return;
        if (has_revision_ && candidate->revision <= state_.revision)
            return;
        if (state_.operations >= configuration_.maximum_operations) {
            state_.phase = Phase::exhausted;
            return;
        }
        request = *candidate;
        has_revision_ = true;
        state_.revision = request.revision;
        ++state_.operations;
        ++state_.provider_calls;
        state_.ticket = state_.operations;
        state_.result = ReceiverBridge::RequestState::none;
        state_.outcome = Outcome::ok;
        state_.position = 0;
        state_.not_after_us = 0;
        state_.phase = Phase::preparing;
    }
    const auto cursor = provider_.resolve(provider_.owner.get(), request);
    // Nothing above this lock held an application/native/transport lock during
    // the provider. Close and explicit manual admission can win this boundary.
    std::lock_guard lock(mutex_);
    refresh();
    if (state_.closed)
        return;
    if (!cursor) {
        state_.phase = Phase::unavailable;
        return;
    }
    const auto deadline = receiver_->admit_automatic(
        request, *cursor, state_.ticket, configuration_.cursor_lifetime_us);
    if (!deadline) {
        state_.phase = Phase::refused;
        return;
    }
    ++state_.admitted;
    state_.position = cursor->position;
    state_.not_after_us = *deadline;
    state_.result = ReceiverBridge::RequestState::pending;
    state_.phase = Phase::posted;
    state_.budget_exhausted =
        state_.operations >= configuration_.maximum_operations;
}
}
#endif
