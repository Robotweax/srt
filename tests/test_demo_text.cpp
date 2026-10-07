#include "test.hpp"
#include "../examples/demo_text.hpp"

#include <string>

TEST(demo_text_keeps_printable_ascii_and_escapes_terminal_controls)
{
    const std::string printable =
        "Stream !#$%&'()*+,-./012:;<=>?@AZ[\\]^_`az{|}~";
    REQUIRE_EQ(srt_demo::escape_terminal_text(printable), printable);
    constexpr char bytes[] = "a\0b\n\r\t\x1b]52;c;payload\x07\x7f\x80\xff";
    const std::string input {bytes, sizeof(bytes) - 1U};
    const auto escaped = srt_demo::escape_terminal_text(input);
    REQUIRE_EQ(escaped,
        "a\\x00b\\x0a\\x0d\\x09\\x1b]52;c;payload\\x07\\x7f\\x80\\xff");
    REQUIRE_EQ(srt_demo::escape_terminal_text(escaped), escaped);
}

TEST(demo_text_all_bytes_remain_single_line_printable_ascii)
{
    std::string input;
    for (unsigned byte = 0; byte < 256; ++byte)
        input.push_back(static_cast<char>(byte));
    const auto escaped = srt_demo::escape_terminal_text(input);
    for (const unsigned char byte : escaped)
        REQUIRE(byte >= 0x20U && byte <= 0x7eU);
    REQUIRE_EQ(escaped.size(), 95U + (256U - 95U) * 4U);
    REQUIRE_EQ(srt_demo::escape_terminal_text(escaped), escaped);
}
