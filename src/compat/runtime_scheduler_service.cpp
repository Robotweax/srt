#include "compat/runtime_scheduler_service.hpp"

#include <mutex>
#include <new>
#include <utility>

namespace robotweax::srt::compat {
namespace {

constexpr std::size_t runtime_scheduler_shards = 2U;
// The public many-socket scorecard admits up to 4,096 caller channels. Keep
// one bounded queue/timer slot per possible channel on each shard so channel
// polling and concurrent handshake timers cannot exhaust the scheduler merely
// because all sockets start in one burst.
constexpr std::size_t runtime_scheduler_queue_capacity = 4'096U;
constexpr std::size_t runtime_scheduler_timer_capacity = 4'096U;

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
        try {
            auto scheduler = std::make_shared<RuntimeScheduler>(
                RuntimeScheduler::Configuration {
                    .shard_count = runtime_scheduler_shards,
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
};

[[nodiscard]] RuntimeSchedulerService& scheduler_service() noexcept
{
    static RuntimeSchedulerService service;
    return service;
}

} // namespace

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
