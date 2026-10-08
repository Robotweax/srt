#include "test.hpp"

#include <exception>
#include <iostream>
#include <string_view>

int main(int argc, char** argv)
{
    const bool include = argc > 1 && std::string_view {argv[1]} == "--include";
    const int first_argument = include ? 3 : 1;
    const bool exclude = argc > first_argument
        && std::string_view {argv[first_argument]} == "--exclude";
    if ((include
            && (argc < 3 || std::string_view {argv[2]}.empty()
                || std::string_view {argv[2]}.starts_with("--")))
        || (exclude && argc < first_argument + 2)
        || (!exclude && argc > first_argument + 1)
        || (argc > first_argument && !exclude
            && std::string_view {argv[first_argument]}.starts_with("--"))) {
        std::cerr << "usage: " << argv[0]
                  << " [--include substring] [--exclude substring ...]\n";
        return 2;
    }
    std::string_view filter;
    if (argc > first_argument) {
        filter = argv[exclude ? first_argument + 1 : first_argument];
    }
    std::size_t failures = 0;
    std::size_t skipped = 0;
    std::size_t selected = 0;
    for (const auto& test : robotweax::srt::test::cases()) {
        const auto name = std::string_view {test.name};
        if (include && name.find(argv[2]) == std::string_view::npos) {
            continue;
        }
        bool matches = name.find(filter) != std::string_view::npos;
        if (exclude) {
            for (int index = first_argument + 2; index < argc && !matches;
                ++index) {
                matches = name.find(argv[index]) != std::string_view::npos;
            }
        }
        if (matches == exclude) {
            continue;
        }
        ++selected;
        std::cerr << "[RUN] " << test.name << '\n' << std::flush;
        try {
            test.function();
            std::cout << "[PASS] " << test.name << '\n';
        } catch (const robotweax::srt::test::Skipped& reason) {
            ++skipped;
            std::cout << "[SKIP] " << test.name << ": " << reason.what()
                      << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": " << error.what() << '\n';
        } catch (...) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": unknown exception\n";
        }
    }
    std::cout << selected - failures - skipped << '/' << selected
              << " tests passed";
    if (skipped != 0U) {
        std::cout << " (" << skipped << " skipped)";
    }
    std::cout << '\n';
    if (selected == 0U) {
        std::cerr << "No tests selected\n";
        return 2;
    }
    return failures == 0 ? 0 : 1;
}
