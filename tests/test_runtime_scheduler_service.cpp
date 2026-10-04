#include "test.hpp"
#include "compat/runtime_scheduler_service.hpp"
#include "compat/connection_datagram_inbox.hpp"
#include "compat/transport_runtime.hpp"
#include "srt/srt.h"

#include <cstdlib>
#include <barrier>
#include <future>
#include <vector>
#include <optional>
#include <string>
#include <string_view>

using namespace robotweax::srt::compat;

namespace {
constexpr const char* setting_name = "ROBOTWEAX_SRT_SCHEDULER_SHARDS";

int set_named_setting(const char* name, const char* value)
{
#if defined(_WIN32)
    return _putenv_s(name, value == nullptr ? "" : value);
#else
    return value == nullptr ? unsetenv(name) : setenv(name, value, 1);
#endif
}

int set_setting(const char* value)
{
    return set_named_setting(setting_name, value);
}

struct EnvironmentGuard {
    const char* name;
    std::optional<std::string> original;
    explicit EnvironmentGuard(const char* setting = setting_name)
        : name(setting)
    {
#if defined(_WIN32)
        char* value = nullptr;
        std::size_t size = 0;
        REQUIRE_EQ(_dupenv_s(&value, &size, name), 0);
        const std::unique_ptr<char, decltype(&std::free)> owned_value(
            value, &std::free);
#else
        const char* value = std::getenv(name);
#endif
        if (value != nullptr) {
            original = value;
        }
    }
    ~EnvironmentGuard()
    {
        stop_runtime_scheduler();
        (void)set_named_setting(name, original ? original->c_str() : nullptr);
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

TEST(scheduler_service_parses_bounded_inbox_ceiling)
{
    for (const auto value : {"1", "64", "1024", "001"})
        REQUIRE(parse_runtime_inbox_storage_mib(value).has_value());
    REQUIRE_EQ(*parse_runtime_inbox_storage_mib("1024"), 1024U);
    REQUIRE_EQ(*parse_runtime_inbox_storage_mib("001"), 1U);
    for (const auto value : {"", "0", "1025", "-1", "+1", " 1", "1 ", "1x",
             "1.5", "999999999999999999999999999999999999999999"})
        REQUIRE(!parse_runtime_inbox_storage_mib(value).has_value());
}

TEST(
    scheduler_service_process_budget_owner_survives_cleanup_and_concurrent_acquire)
{
    constexpr const char* budget_setting = "ROBOTWEAX_SRT_INBOX_STORAGE_MIB";
    EnvironmentGuard restore_budget {budget_setting};
    EnvironmentGuard restore_shards;
    // The isolated service test process has not acquired the owner yet.
    for (const auto value : {"0", "1025", "invalid", "-1", "1 "}) {
        REQUIRE_EQ(set_named_setting(budget_setting, value), 0);
        REQUIRE(acquire_runtime_inbox_storage_budget() == nullptr);
    }
    REQUIRE_EQ(set_named_setting(budget_setting, "8"), 0);
    std::barrier start {3};
    auto acquire = [&] {
        start.arrive_and_wait();
        return acquire_runtime_inbox_storage_budget();
    };
    auto left = std::async(std::launch::async, acquire);
    auto right = std::async(std::launch::async, acquire);
    start.arrive_and_wait();
    auto budget = left.get();
    REQUIRE(budget != nullptr);
    REQUIRE_EQ(right.get().get(), budget.get());
    REQUIRE_EQ(budget->reserved_bytes(), 0U);
    REQUIRE_EQ(budget->reserved_inboxes(), 0U);

    // Budget acquisition does not start scheduler workers or admit services.
    REQUIRE_EQ(set_setting("1"), 0);
    REQUIRE_EQ(srt_startup(), 0);
    const auto old_scheduler = acquire_runtime_scheduler();
    REQUIRE(old_scheduler != nullptr);
    REQUIRE_EQ(old_scheduler->snapshot().service_capacity, 0U);
    auto independent = std::make_shared<RuntimeScheduler>(
        RuntimeScheduler::Configuration {.shard_count = 1,
            .queue_capacity_per_shard = 1,
            .timer_capacity_per_shard = 1,
            .service_capacity_per_shard = 2});
    REQUIRE(independent->start());
    auto binding = ConnectionWorkBinding::create(
        independent, 0, [](void*, ConnectionWorkHints) noexcept { }, nullptr);
    REQUIRE(binding != nullptr);
    constexpr std::size_t limit = 8U * 1024U * 1024U;
    const auto entry_bytes = *ConnectionDatagramInbox::storage_bytes(1);
    const auto capacity = limit / entry_bytes;
    REQUIRE(capacity > 1U);
    const auto charged = *ConnectionDatagramInbox::storage_bytes(capacity);
    const robotweax::srt::Ipv4Endpoint peer {
        .address = {192, 0, 2, 93}, .port = 14903};
    auto channel = std::make_shared<DatagramStorageBudget>(limit);
    // Small rings exhaust the process count ceiling while byte room remains.
    std::vector<std::shared_ptr<ConnectionDatagramInbox>> small_rings;
    small_rings.reserve(4096);
    for (std::size_t i = 0; i < 4096; ++i) {
        auto ring = ConnectionDatagramInbox::create(channel, binding, peer,
            {.capacity = 1, .control_reserve = 0}, budget);
        REQUIRE(ring != nullptr);
        small_rings.push_back(std::move(ring));
    }
    REQUIRE_EQ(budget->reserved_inboxes(), 4096U);
    REQUIRE(budget->reserved_bytes() < limit - entry_bytes);
    REQUIRE(ConnectionDatagramInbox::create(channel, binding, peer,
                {.capacity = 1, .control_reserve = 0}, budget)
        == nullptr);
    REQUIRE_EQ(channel->reserved_inboxes(), 4096U);
    small_rings.clear();
    REQUIRE_EQ(budget->reserved_inboxes(), 0U);
    REQUIRE_EQ(channel->reserved_inboxes(), 0U);
    auto old_ring = ConnectionDatagramInbox::create(channel, binding, peer,
        {.capacity = capacity, .control_reserve = 1}, budget);
    REQUIRE(old_ring != nullptr);
    REQUIRE_EQ(budget->reserved_bytes(), charged);
    REQUIRE_EQ(budget->reserved_inboxes(), 1U);
    REQUIRE_EQ(srt_cleanup(), 0);
    REQUIRE(!old_scheduler->snapshot().accepting);
    REQUIRE_EQ(budget->reserved_bytes(), charged);
    REQUIRE_EQ(budget->reserved_inboxes(), 1U);

    // Neither configuration changes nor a new scheduler generation reset the
    // process owner, including when the caller drops its own owner handle.
    const auto* identity = budget.get();
    budget.reset();
    REQUIRE_EQ(set_named_setting(budget_setting, "1024"), 0);
    REQUIRE_EQ(srt_startup(), 0);
    auto fresh_scheduler = acquire_runtime_scheduler();
    REQUIRE(fresh_scheduler != nullptr);
    REQUIRE(fresh_scheduler.get() != old_scheduler.get());
    budget = acquire_runtime_inbox_storage_budget();
    REQUIRE_EQ(budget.get(), identity);
    auto fresh_channel = std::make_shared<DatagramStorageBudget>(limit);
    REQUIRE(ConnectionDatagramInbox::create(fresh_channel, binding, peer,
                {.capacity = capacity, .control_reserve = 1}, budget)
        == nullptr);
    REQUIRE_EQ(fresh_channel->reserved_bytes(), 0U);
    REQUIRE_EQ(budget->reserved_bytes(), charged);
    REQUIRE_EQ(budget->reserved_inboxes(), 1U);
    old_ring.reset();
    REQUIRE_EQ(channel->reserved_bytes(), 0U);
    REQUIRE_EQ(budget->reserved_bytes(), 0U);
    REQUIRE_EQ(budget->reserved_inboxes(), 0U);
    auto fresh_ring = ConnectionDatagramInbox::create(fresh_channel, binding,
        peer, {.capacity = capacity, .control_reserve = 1}, budget);
    REQUIRE(fresh_ring != nullptr);
    REQUIRE_EQ(budget->reserved_bytes(), charged);
    REQUIRE_EQ(budget->reserved_inboxes(), 1U);
    fresh_ring.reset();
    REQUIRE_EQ(budget->reserved_bytes(), 0U);
    REQUIRE_EQ(budget->reserved_inboxes(), 0U);
    binding->retire();
    independent->stop();
    REQUIRE_EQ(srt_cleanup(), 0);
    REQUIRE_EQ(acquire_runtime_inbox_storage_budget().get(), identity);
}

TEST(scheduler_service_native_budget_survives_cleanup_with_independent_owner)
{
    EnvironmentGuard restore;
    REQUIRE_EQ(set_setting("1"), 0);
    auto budget = acquire_runtime_native_channel_budget();
    REQUIRE(budget != nullptr);
    REQUIRE_EQ(budget->maximum_channels(), 4096U);
    REQUIRE_EQ(budget->reserved_channels(), 0U);
    auto channel = DatagramChannel::create_budgeted(
        robotweax::srt::IpAddressFamily::ipv4, budget);
    REQUIRE(channel != nullptr);
    REQUIRE_EQ(srt_startup(), 0);
    auto first = acquire_runtime_scheduler();
    REQUIRE(first != nullptr);
    REQUIRE_EQ(srt_cleanup(), 0);
    // This internal channel was never registered in the owning bind registry.
    // Its caller owns terminal shutdown; cleanup must not reset its allowance.
    REQUIRE(channel->socket.valid());
    REQUIRE_EQ(budget->reserved_channels(), 1U);
    REQUIRE_EQ(srt_startup(), 0);
    auto next = acquire_runtime_scheduler();
    REQUIRE(next != nullptr);
    REQUIRE(next.get() != first.get());
    REQUIRE_EQ(acquire_runtime_native_channel_budget().get(), budget.get());
    REQUIRE_EQ(budget->reserved_channels(), 1U);
    REQUIRE_EQ(channel->shutdown(), DatagramChannel::ShutdownStatus::retired);
    REQUIRE_EQ(budget->reserved_channels(), 0U);
    REQUIRE_EQ(srt_cleanup(), 0);
    std::weak_ptr<NativeChannelBudget> captured = budget;
    budget.reset();
    channel.reset();
    REQUIRE(!captured.expired());
}
