/* SPDX-License-Identifier: MIT */
#pragma once

#include <srt/srt.h>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace srt_demo {

inline constexpr std::string_view matrix_filter =
    "fec,cols:8,rows:8,layout:even,arq:always";
inline constexpr std::string_view arq_first_help =
    "  --arq-first            Enable ARQ-first FEC (default off)\n"
    "    Default filter: Matrix 8x8 even with arq:always.\n";

inline void configure_arq_first(SRTSOCKET socket)
{
    const std::int32_t policy = ROBOTWEAX_SRT_FECPOLICY_ARQFIRST;
    if (srt_setsockflag(socket, SRTO_ROBOTWEAX_FECPOLICY, &policy,
            static_cast<int>(sizeof(policy)))
        == SRT_ERROR)
        throw std::runtime_error(
            std::string("--arq-first: ") + srt_getlasterror_str());
}

} // namespace srt_demo
