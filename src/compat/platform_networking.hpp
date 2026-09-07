#pragma once

namespace robotweax::srt::compat {

// Initializes process-wide networking support when the platform requires it.
// Returns zero on success or the native platform error code on failure.
[[nodiscard]] int initialize_platform_networking() noexcept;

} // namespace robotweax::srt::compat
