#include "compat/runtime_work_executor_service.hpp"

#include <mutex>
#include <utility>

namespace robotweax::srt::compat {
namespace {

constexpr std::size_t runtime_work_executor_workers = 4U;
constexpr std::size_t runtime_work_executor_queue_capacity = 1'024U;

class RuntimeWorkExecutorService {
public:
    ~RuntimeWorkExecutorService()
    {
        stop();
    }

    [[nodiscard]] std::shared_ptr<RuntimeWorkExecutor> acquire() noexcept
    {
        std::lock_guard lock(mutex_);
        if (executor_ != nullptr) {
            return executor_;
        }
        try {
            auto executor = std::make_shared<RuntimeWorkExecutor>(
                RuntimeWorkExecutor::Configuration {
                    .worker_count = runtime_work_executor_workers,
                    .queue_capacity = runtime_work_executor_queue_capacity,
                });
            if (!executor->start()) {
                return {};
            }
            executor_ = std::move(executor);
            return executor_;
        } catch (...) {
            return {};
        }
    }

    void stop() noexcept
    {
        std::shared_ptr<RuntimeWorkExecutor> executor;
        {
            std::lock_guard lock(mutex_);
            executor = std::move(executor_);
        }
        if (executor != nullptr) {
            executor->stop();
        }
    }

private:
    std::mutex mutex_;
    std::shared_ptr<RuntimeWorkExecutor> executor_;
};

[[nodiscard]] RuntimeWorkExecutorService& work_executor_service() noexcept
{
    static RuntimeWorkExecutorService service;
    return service;
}

} // namespace

void prepare_runtime_work_executor_service() noexcept
{
    (void)work_executor_service();
}

std::shared_ptr<RuntimeWorkExecutor> acquire_runtime_work_executor() noexcept
{
    return work_executor_service().acquire();
}

void stop_runtime_work_executor() noexcept
{
    work_executor_service().stop();
}

} // namespace robotweax::srt::compat
