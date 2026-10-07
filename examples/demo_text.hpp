/* SPDX-License-Identifier: MIT */
#pragma once

#include <string>
#include <string_view>

namespace srt_demo {

// Display bytes as a single printable ASCII field. Preserve printable ASCII
// (including literal backslashes); this is diagnostic text, not a reversible
// serialization. Idempotence lets exception handlers safely escape it again.
inline std::string escape_terminal_text(std::string_view value)
{
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(value.size());
    for (const unsigned char byte : value) {
        if (byte >= 0x20U && byte <= 0x7eU) {
            result.push_back(static_cast<char>(byte));
        } else {
            result += "\\x";
            result.push_back(hex[byte >> 4U]);
            result.push_back(hex[byte & 0x0fU]);
        }
    }
    return result;
}

} // namespace srt_demo
