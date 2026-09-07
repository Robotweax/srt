#pragma once

#include "srt/srt.h"

namespace robotweax::srt::compat {

struct ErrorState {
    SRT_ERRNO code = SRT_SUCCESS;
    int system_error = 0;
};

void set_last_error(SRT_ERRNO code, int system_error = 0) noexcept;
void clear_last_error() noexcept;
[[nodiscard]] ErrorState last_error() noexcept;
[[nodiscard]] const char* error_text(int code) noexcept;

} // namespace robotweax::srt::compat
