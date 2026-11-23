#include "compat/process_state.hpp"

#if defined(_WIN32) && defined(SRT_EXPORTS)
#include <windows.h>

namespace {
bool process_terminating = false;
}

// During ExitProcess the loader calls DllMain before CRT destructors and
// after terminating the other threads. Worker-backed objects cannot drain.
extern "C" BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_DETACH)
        process_terminating = reserved != nullptr;
    return TRUE;
}
#elif !defined(_WIN32)
#  include <atomic>
#  include <cstdint>
#  include <unistd.h>
#endif

namespace robotweax::srt::compat {
namespace {

#if !defined(_WIN32)
static_assert(std::atomic_uint32_t::is_always_lock_free);
std::atomic_uint32_t owner_process_id = 0;
#endif

} // namespace

std::uint32_t current_process_id() noexcept
{
#if !defined(_WIN32)
    return static_cast<std::uint32_t>(getpid());
#else
    return 0;
#endif
}

bool can_destroy_process_state(std::uint32_t owner) noexcept
{
#if defined(_WIN32) && defined(SRT_EXPORTS)
    if (process_terminating)
        return false;
#endif
    return owner == current_process_id();
}

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
