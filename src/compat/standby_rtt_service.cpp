#include "compat/standby_rtt_service.hpp"
#include "compat/process_owned.hpp"
#include <chrono>

namespace robotweax::srt::compat {
namespace {
struct Owner {
    std::mutex mutex;
    std::shared_ptr<StandbyRttService> service;
};
Owner& owner()
{
    static ProcessOwned<Owner> value;
    return value.get();
}
}
std::shared_ptr<StandbyRttService> acquire_standby_rtt_service()
{
    auto& state = owner();
    std::lock_guard lock(state.mutex);
    if (!state.service)
        state.service = std::make_shared<StandbyRttService>();
    return state.service;
}
void prepare_standby_rtt_service() noexcept
{
    (void)owner();
}
void clear_standby_rtt_service() noexcept
{
    auto& state = owner();
    std::lock_guard lock(state.mutex);
    if (state.service)
        state.service->clear();
}
std::uint64_t StandbyRttService::now() const noexcept
{
    if (clock_ != nullptr)
        return clock_(context_);
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}
std::optional<StandbyRttService::Lease> StandbyRttService::admit() noexcept
{
    std::lock_guard lock(mutex_);
    return budget_.admit();
}
void StandbyRttService::release(Lease lease) noexcept
{
    std::lock_guard lock(mutex_);
    if (selected_ == lease)
        selected_.reset();
    budget_.release(lease);
}
bool StandbyRttService::request(Lease lease, bool eligible) noexcept
{
    std::lock_guard lock(mutex_);
    if (!budget_.valid_lease(lease))
        return false;
    budget_.set_eligible(lease, eligible);
    if (!eligible) {
        if (selected_ == lease)
            selected_.reset();
        return false;
    }
    const auto current = now();
    if (current < next_claim_ || current > UINT64_MAX - 31'250)
        return false;
    if (!selected_)
        selected_ = budget_.grant_request(current);
    if (selected_ != lease)
        return false;
    selected_.reset();
    next_claim_ = current + 31'250;
    budget_.set_eligible(lease, false);
    return true;
}
bool StandbyRttService::echo(Lease lease) noexcept
{
    std::lock_guard lock(mutex_);
    return budget_.grant_echo(lease, now());
}
void StandbyRttService::clear() noexcept
{
    std::lock_guard lock(mutex_);
    selected_.reset();
    budget_.clear();
}
} // namespace robotweax::srt::compat
