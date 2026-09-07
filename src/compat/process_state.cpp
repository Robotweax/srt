#include "compat/process_state.hpp"

#if !defined(_WIN32)
#  include <atomic>
#  include <cstdint>
#  include <unistd.h>
#endif

namespace robotweax::srt::compat {
namespace {

#if !defined(_WIN32)
static_assert(std::atomic_uint32_t::is_always_lock_free);
std::atomic_uint32_t owner_process_id = 0;

[[nodiscard]] std::uint32_t current_process_id() noexcept
{
    return static_cast<std::uint32_t>(getpid());
}
#endif

} // namespace

bool stateful_process_available() noexcept
{
#if !defined(_WIN32)
    const std::uint32_t current = current_process_id();
    std::uint32_t owner =
        owner_process_id.load(std::memory_order_relaxed);
    if (owner == 0U) {
        std::uint32_t unowned = 0;
        if (owner_process_id.compare_exchange_strong(
                unowned, current, std::memory_order_relaxed)) {
            owner = current;
        } else {
            owner = unowned;
        }
    }
    return owner == current;
#else
    return true;
#endif
}

} // namespace robotweax::srt::compat
