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
        try {
            auto scheduler = std::make_shared<RuntimeScheduler>(
                RuntimeScheduler::Configuration {
                    .shard_count = *shard_count,
                    .queue_capacity_per_shard =
                        runtime_scheduler_queue_capacity,
                    .timer_capacity_per_shard =
                        runtime_scheduler_timer_capacity,
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
    std::mutex mutex_;
    std::shared_ptr<RuntimeScheduler> scheduler_;
    // Never reset at scheduler stop: old callbacks can retain ring charges.
    std::shared_ptr<DatagramStorageBudget> inbox_budget_;
    std::shared_ptr<NativeChannelBudget> channel_budget_;
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
