#include "compat/runtime_scheduler_service.hpp"
#include "compat/process_owned.hpp"
#include "compat/connection_datagram_inbox.hpp"
#include "compat/transport_runtime.hpp"

#include <charconv>
#include <cstdlib>
#include <mutex>
#include <new>
#include <utility>

namespace robotweax::srt::compat {
namespace {

constexpr std::size_t default_runtime_scheduler_shards = 2U;
// The public many-socket scorecard admits up to 4,096 caller channels. Keep
// one bounded queue/timer slot per possible channel on each shard so channel
// polling and concurrent handshake timers cannot exhaust the scheduler merely
// because all sockets start in one burst.
constexpr std::size_t runtime_scheduler_queue_capacity = 4'096U;
constexpr std::size_t runtime_scheduler_timer_capacity = 4'096U;

constexpr std::size_t default_inbox_storage_mib = 64U;
constexpr std::size_t runtime_inbox_count_limit = 4'096U;
constexpr std::size_t bytes_per_mib = 1024U * 1024U;

class RuntimeSchedulerService {
public:
    ~RuntimeSchedulerService()
    {
        stop();
    }

    [[nodiscard]] std::shared_ptr<RuntimeScheduler> acquire() noexcept
    {
        std::lock_guard lock(mutex_);
        if (scheduler_ != nullptr) {
            return scheduler_;
        }
#if defined(_WIN32)
        char* setting = nullptr;
        std::size_t setting_size = 0;
        if (_dupenv_s(&setting, &setting_size, "ROBOTWEAX_SRT_SCHEDULER_SHARDS")
            != 0) {
            return {};
        }
        const std::unique_ptr<char, decltype(&std::free)> owned_setting(
            setting, &std::free);
#else
        const char* setting = std::getenv("ROBOTWEAX_SRT_SCHEDULER_SHARDS");
#endif
        const auto shard_count = setting == nullptr
            ? std::optional<std::size_t> {default_runtime_scheduler_shards}
            : parse_runtime_scheduler_shards(setting);
        if (!shard_count.has_value()) {
            return {};
        }
#if defined(_WIN32)
        char* service_setting = nullptr;
        std::size_t service_setting_size = 0;
        if (_dupenv_s(&service_setting, &service_setting_size,
                "ROBOTWEAX_SRT_SCHEDULER_SERVICES_PER_SHARD")
            != 0)
            return {};
        const std::unique_ptr<char, decltype(&std::free)> owned_service_setting(
            service_setting, &std::free);
#else
        const char* service_setting =
            std::getenv("ROBOTWEAX_SRT_SCHEDULER_SERVICES_PER_SHARD");
#endif
        const auto affinity = connection_affinity_locked();
        if (!affinity.has_value())
            return {};
        const auto service_count = service_setting == nullptr
            ? std::optional<std::size_t> {*affinity ? 1024U : 0U}
            : parse_runtime_scheduler_services(service_setting);
        if (!service_count.has_value() || (*affinity && *service_count == 0U))
            return {};
        try {
            auto service_budget = *service_count == 0U
                ? nullptr
                : acquire_service_budget_locked();
            if (*service_count != 0U && service_budget == nullptr)
                return {};
            auto scheduler = std::make_shared<RuntimeScheduler>(
                RuntimeScheduler::Configuration {
                    .shard_count = *shard_count,
                    .queue_capacity_per_shard =
                        runtime_scheduler_queue_capacity,
                    .timer_capacity_per_shard =
                        runtime_scheduler_timer_capacity,
                    .service_capacity_per_shard = *service_count,
                    .service_storage_budget = std::move(service_budget),
                });
            if (!scheduler->start()) {
                return {};
            }
            scheduler_ = std::move(scheduler);
            return scheduler_;
        } catch (...) {
            return {};
        }
    }

    [[nodiscard]] std::shared_ptr<DatagramStorageBudget>
    acquire_budget() noexcept
    {
        std::lock_guard lock(mutex_);
        if (inbox_budget_ != nullptr)
            return inbox_budget_;
#if defined(_WIN32)
        char* setting = nullptr;
        std::size_t setting_size = 0;
        if (_dupenv_s(
                &setting, &setting_size, "ROBOTWEAX_SRT_INBOX_STORAGE_MIB")
            != 0)
            return {};
        const std::unique_ptr<char, decltype(&std::free)> owned_setting(
            setting, &std::free);
#else
        const char* setting = std::getenv("ROBOTWEAX_SRT_INBOX_STORAGE_MIB");
#endif
        const auto mib = setting == nullptr
            ? std::optional<std::size_t> {default_inbox_storage_mib}
            : parse_runtime_inbox_storage_mib(setting);
        if (!mib.has_value())
            return {};
        try {
            inbox_budget_ = std::make_shared<DatagramStorageBudget>(
                *mib * bytes_per_mib, runtime_inbox_count_limit);
            return inbox_budget_;
        } catch (...) {
            return {};
        }
    }

    [[nodiscard]] std::shared_ptr<NativeChannelBudget>
    acquire_channel_budget() noexcept
    {
        std::lock_guard lock(mutex_);
        if (channel_budget_ != nullptr)
            return channel_budget_;
        try {
            channel_budget_ = std::make_shared<NativeChannelBudget>(4096U);
            return channel_budget_;
        } catch (...) {
            return {};
        }
    }

    [[nodiscard]] std::optional<bool> bounded_bind() noexcept
    {
        std::lock_guard lock(mutex_);
        if (bounded_bind_.has_value())
            return bounded_bind_;
#if defined(_WIN32)
        char* setting = nullptr;
        std::size_t setting_size = 0;
        if (_dupenv_s(&setting, &setting_size, "ROBOTWEAX_SRT_BOUNDED_BIND")
            != 0)
            return std::nullopt;
        const std::unique_ptr<char, decltype(&std::free)> owned_setting(
            setting, &std::free);
#else
        const char* setting = std::getenv("ROBOTWEAX_SRT_BOUNDED_BIND");
#endif
        const auto selected = setting == nullptr
            ? std::optional<bool> {false}
            : parse_runtime_bounded_bind(setting);
        if (selected.has_value())
            bounded_bind_ = selected;
        return selected;
    }

    [[nodiscard]] std::optional<bool> connection_affinity() noexcept
    {
        std::lock_guard lock(mutex_);
        return connection_affinity_locked();
    }

    [[nodiscard]] std::shared_ptr<SchedulerServiceStorageBudget>
    acquire_service_budget() noexcept
    {
        std::lock_guard lock(mutex_);
        return acquire_service_budget_locked();
    }

    void stop() noexcept
    {
        std::shared_ptr<RuntimeScheduler> scheduler;
        {
            std::lock_guard lock(mutex_);
            scheduler = std::move(scheduler_);
        }
        if (scheduler != nullptr) {
            scheduler->stop();
        }
    }

private:
    [[nodiscard]] std::optional<bool> connection_affinity_locked() noexcept
    {
        if (connection_affinity_.has_value())
            return connection_affinity_;
#if defined(_WIN32)
        char* setting = nullptr;
        std::size_t setting_size = 0;
        if (_dupenv_s(
                &setting, &setting_size, "ROBOTWEAX_SRT_CONNECTION_AFFINITY")
            != 0)
            return std::nullopt;
        const std::unique_ptr<char, decltype(&std::free)> owned_setting(
            setting, &std::free);
#else
        const char* setting = std::getenv("ROBOTWEAX_SRT_CONNECTION_AFFINITY");
#endif
        const auto selected = setting == nullptr
            ? std::optional<bool> {false}
            : parse_runtime_bounded_bind(setting);
        if (selected.has_value())
            connection_affinity_ = selected;
        return selected;
    }
    [[nodiscard]] std::shared_ptr<SchedulerServiceStorageBudget>
    acquire_service_budget_locked() noexcept
    {
        if (service_budget_ == nullptr) {
            try {
                service_budget_ =
                    std::make_shared<SchedulerServiceStorageBudget>(
                        32U * 1024U * 1024U, 8U);
            } catch (...) {
                return {};
            }
        }
        return service_budget_;
    }
    std::mutex mutex_;
    std::shared_ptr<SchedulerServiceStorageBudget> service_budget_;
    std::shared_ptr<RuntimeScheduler> scheduler_;
    // Never reset at scheduler stop: old callbacks can retain ring charges.
    std::shared_ptr<DatagramStorageBudget> inbox_budget_;
    std::shared_ptr<NativeChannelBudget> channel_budget_;
    std::optional<bool> bounded_bind_;
    std::optional<bool> connection_affinity_;
};

[[nodiscard]] RuntimeSchedulerService& scheduler_service() noexcept
{
    static ProcessOwned<RuntimeSchedulerService> service;
    return service.get();
}

} // namespace

std::optional<std::size_t> parse_runtime_scheduler_shards(
    std::string_view value) noexcept
{
    if (value.empty()) {
        return std::nullopt;
    }
    std::size_t count = 0;
    const auto parsed =
        std::from_chars(value.data(), value.data() + value.size(), count);
    if (parsed.ec != std::errc {} || parsed.ptr != value.data() + value.size()
        || count == 0U || count > 64U) {
        return std::nullopt;
    }
    return count;
}

std::optional<std::size_t> parse_runtime_scheduler_services(
    std::string_view value) noexcept
{
    if (value.empty())
        return std::nullopt;
    std::size_t count = 0;
    const auto parsed =
        std::from_chars(value.data(), value.data() + value.size(), count);
    if (parsed.ec != std::errc {} || parsed.ptr != value.data() + value.size()
        || count > 4096U)
        return std::nullopt;
    return count;
}

std::optional<std::size_t> parse_runtime_inbox_storage_mib(
    std::string_view value) noexcept
{
    if (value.empty())
        return std::nullopt;
    std::size_t mib = 0;
    const auto parsed =
        std::from_chars(value.data(), value.data() + value.size(), mib);
    if (parsed.ec != std::errc {} || parsed.ptr != value.data() + value.size()
        || mib == 0U || mib > 1024U)
        return std::nullopt;
    return mib;
}

std::shared_ptr<SchedulerServiceStorageBudget>
acquire_runtime_service_storage_budget() noexcept
{
    if (!stateful_process_available())
        return {};
    return scheduler_service().acquire_service_budget();
}

std::shared_ptr<DatagramStorageBudget>
acquire_runtime_inbox_storage_budget() noexcept
{
    // Reject a fork child before touching any inherited owner/budget mutex.
    if (!stateful_process_available())
        return {};
    return scheduler_service().acquire_budget();
}

std::shared_ptr<NativeChannelBudget>
acquire_runtime_native_channel_budget() noexcept
{
    if (!stateful_process_available())
        return {};
    return scheduler_service().acquire_channel_budget();
}

std::optional<bool> parse_runtime_bounded_bind(std::string_view value) noexcept
{
    if (value == "0")
        return false;
    if (value == "1")
        return true;
    return std::nullopt;
}

std::optional<bool> runtime_bounded_bind_enabled() noexcept
{
    if (!stateful_process_available())
        return std::nullopt;
    return scheduler_service().bounded_bind();
}

std::optional<bool> runtime_connection_affinity_enabled() noexcept
{
    if (!stateful_process_available())
        return std::nullopt;
    return scheduler_service().connection_affinity();
}

void prepare_runtime_scheduler_service() noexcept
{
    (void)scheduler_service();
}

std::shared_ptr<RuntimeScheduler> acquire_runtime_scheduler() noexcept
{
    return scheduler_service().acquire();
}

void stop_runtime_scheduler() noexcept
{
    scheduler_service().stop();
}

} // namespace robotweax::srt::compat
