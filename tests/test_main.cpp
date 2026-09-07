#include "test.hpp"

#include <exception>
#include <iostream>
#include <string_view>

int main(int argc, char** argv)
{
    const std::string_view filter =
        argc > 1 ? std::string_view{argv[1]} : std::string_view{};
    std::size_t failures = 0;
    std::size_t selected = 0;
    for (const auto& test : robotweax::srt::test::cases()) {
        if (!filter.empty()
            && std::string_view{test.name}.find(filter) == std::string_view::npos) {
            continue;
        }
        ++selected;
        std::cerr << "[RUN] " << test.name << '\n' << std::flush;
        try {
            test.function();
            std::cout << "[PASS] " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": " << error.what() << '\n';
        } catch (...) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": unknown exception\n";
        }
    }
    std::cout << selected - failures << '/' << selected << " tests passed\n";
    return failures == 0 ? 0 : 1;
}
