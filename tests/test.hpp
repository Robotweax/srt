#pragma once

#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace robotweax::srt::test {

struct Case {
    const char* name;
    void (*function)();
};

inline std::vector<Case>& cases()
{
    static std::vector<Case> registered;
    return registered;
}

struct Register {
    Register(const char* name, void (*function)()) { cases().push_back({name, function}); }
};

template<class Left, class Right>
void equal(const Left& left, const Right& right, const char* left_text,
    const char* right_text, const char* file, int line)
{
    if (!(left == right)) {
        std::ostringstream message;
        message << file << ':' << line << ": expected " << left_text
                << " == " << right_text;
        throw std::runtime_error(message.str());
    }
}

inline void require(bool value, const char* text, const char* file, int line)
{
    if (!value) {
        std::ostringstream message;
        message << file << ':' << line << ": requirement failed: " << text;
        throw std::runtime_error(message.str());
    }
}

} // namespace robotweax::srt::test

#define ROBOTWEAX_TEST_CONCAT_INNER(a, b) a##b
#define ROBOTWEAX_TEST_CONCAT(a, b) ROBOTWEAX_TEST_CONCAT_INNER(a, b)
#define TEST(name) \
    static void name(); \
    static ::robotweax::srt::test::Register ROBOTWEAX_TEST_CONCAT(register_, name){#name, &name}; \
    static void name()
#define REQUIRE(expression) ::robotweax::srt::test::require(!!(expression), #expression, __FILE__, __LINE__)
#define REQUIRE_EQ(left, right) ::robotweax::srt::test::equal((left), (right), #left, #right, __FILE__, __LINE__)
