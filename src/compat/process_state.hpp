#pragma once

#include <cstdint>

namespace robotweax::srt::compat {

// Stateless identity check for process-owned teardown. Never claims runtime
// ownership or touches an inherited mutex. Non-POSIX processes use zero.
[[nodiscard]] std::uint32_t current_process_id() noexcept;

// Claims process-wide runtime ownership on first use. Returns false in a
// POSIX child that inherited state owned by its parent. No at-fork handler or
// inherited lock is used. On non-POSIX platforms it always returns true.
[[nodiscard]] bool stateful_process_available() noexcept;

} // namespace robotweax::srt::compat
