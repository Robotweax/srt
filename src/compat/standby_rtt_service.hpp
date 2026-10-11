#pragma once

#include "robotweax/srt/standby_rtt.hpp"
#include <memory>
#include <mutex>

namespace robotweax::srt::compat {

// No callbacks, socket references, queues or worker ownership. The selected
// round-robin lease is reserved until its owner polls or becomes ineligible.
class StandbyRttService {
public:
    using Lease = StandbyRttBudget::Lease;
    using NowFunction = std::uint64_t (*)(void*) noexcept;
    explicit StandbyRttService(
        NowFunction clock = nullptr, void* context = nullptr) noexcept
        : clock_(clock)
        , context_(context)
    {
    }
    [[nodiscard]] std::optional<Lease> admit() noexcept;
    void release(Lease) noexcept;
    [[nodiscard]] bool request(Lease, bool eligible) noexcept;
    [[nodiscard]] bool echo(Lease) noexcept;
    void clear() noexcept;

private:
    [[nodiscard]] std::uint64_t now() const noexcept;
    std::mutex mutex_;
    StandbyRttBudget budget_;
    std::optional<Lease> selected_;
    std::uint64_t next_claim_ = 0;
    NowFunction clock_;
    void* context_;
};
static_assert(sizeof(StandbyRttService) <= 4096);
[[nodiscard]] std::shared_ptr<StandbyRttService> acquire_standby_rtt_service();
void prepare_standby_rtt_service() noexcept;
void clear_standby_rtt_service() noexcept;
} // namespace robotweax::srt::compat
