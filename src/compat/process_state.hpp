#pragma once

namespace robotweax::srt::compat {

// Claims process-wide runtime ownership on first use. Returns false in a
// POSIX child that inherited state owned by its parent. No at-fork handler or
// inherited lock is used. On non-POSIX platforms it always returns true.
[[nodiscard]] bool stateful_process_available() noexcept;

} // namespace robotweax::srt::compat
