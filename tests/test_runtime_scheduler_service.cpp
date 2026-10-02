#include "test.hpp"
#include "compat/runtime_scheduler_service.hpp"
#include "srt/srt.h"

#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

using namespace robotweax::srt::compat;

namespace {
constexpr const char* setting_name = "ROBOTWEAX_SRT_SCHEDULER_SHARDS";

int set_setting(const char* value)
{
#if defined(_WIN32)
    return _putenv_s(setting_name, value == nullptr ? "" : value);
#else
    return value == nullptr ? unsetenv(setting_name)
                            : setenv(setting_name, value, 1);
#endif
}

struct EnvironmentGuard {
    std::optional<std::string> original;
    EnvironmentGuard()
    {
#if defined(_WIN32)
        char* value = nullptr;
        std::size_t size = 0;
        REQUIRE_EQ(_dupenv_s(&value, &size, setting_name), 0);
        const std::unique_ptr<char, decltype(&std::free)> owned_value(
            value, &std::free);
#else
        const char* value = std::getenv(setting_name);
#endif
        if (value != nullptr) {
            original = value;
        }
    }
    ~EnvironmentGuard()
    {
        stop_runtime_scheduler();
        (void)set_setting(original ? original->c_str() : nullptr);
    }
};
}

TEST(scheduler_service_parses_bounded_decimal_setting)
{
    for (const auto value : {"1", "2", "4", "8", "64", "002"}) {
        REQUIRE(parse_runtime_scheduler_shards(value).has_value());
    }
    REQUIRE_EQ(*parse_runtime_scheduler_shards("64"), 64U);
    REQUIRE_EQ(*parse_runtime_scheduler_shards("002"), 2U);
    for (const auto value : {"", "0", "65", "-1", "+2", " 2", "2 ", "2x", "1.5",
             "999999999999999999999999999999999999999999"}) {
        REQUIRE(!parse_runtime_scheduler_shards(value).has_value());
    }
}

TEST(scheduler_service_configuration_is_fixed_until_final_cleanup)
{
    EnvironmentGuard restore;
    REQUIRE_EQ(set_setting(nullptr), 0);
    REQUIRE_EQ(srt_startup(), 0);
    const auto first = acquire_runtime_scheduler();
    REQUIRE(first != nullptr);
    REQUIRE_EQ(first->snapshot().shard_count, 2U);
    REQUIRE_EQ(set_setting("4"), 0);
    REQUIRE_EQ(acquire_runtime_scheduler().get(), first.get());
    // A nested startup/cleanup does not retire the active generation.
    REQUIRE_EQ(srt_startup(), 0);
    REQUIRE_EQ(srt_cleanup(), 0);
    REQUIRE(first->snapshot().accepting);
    REQUIRE_EQ(srt_cleanup(), 0);
    REQUIRE(!first->snapshot().accepting);

    REQUIRE_EQ(srt_startup(), 0);
    const auto second = acquire_runtime_scheduler();
    REQUIRE(second != nullptr);
    REQUIRE(second.get() != first.get());
    REQUIRE_EQ(second->snapshot().shard_count, 4U);
    REQUIRE_EQ(second->snapshot().queue_capacity, 4U * 4096U);
    REQUIRE_EQ(second->snapshot().timer_capacity, 4U * 4096U);
    REQUIRE_EQ(set_setting("invalid"), 0);
    REQUIRE_EQ(acquire_runtime_scheduler().get(), second.get());
    REQUIRE_EQ(srt_cleanup(), 0);
}

TEST(scheduler_service_invalid_setting_fails_without_poisoning_retry)
{
    EnvironmentGuard restore;
    stop_runtime_scheduler();
#if !defined(_WIN32)
    REQUIRE_EQ(set_setting(""), 0);
    REQUIRE(acquire_runtime_scheduler() == nullptr);
#endif
    for (const auto value : {"0", "65", "no", "-2", "2 "}) {
        REQUIRE_EQ(set_setting(value), 0);
        REQUIRE(acquire_runtime_scheduler() == nullptr);
    }
    REQUIRE_EQ(set_setting("1"), 0);
    const auto scheduler = acquire_runtime_scheduler();
    REQUIRE(scheduler != nullptr);
    REQUIRE_EQ(scheduler->snapshot().shard_count, 1U);
}
