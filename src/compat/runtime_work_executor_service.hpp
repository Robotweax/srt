#pragma once

#include "compat/runtime_work_executor.hpp"

#include <memory>

namespace robotweax::srt::compat {

// Establishes process-exit destruction order before SocketRegistry exists.
void prepare_runtime_work_executor_service() noexcept;

// Returns the bounded process-wide off-shard executor, creating a fresh
// generation after final cleanup when necessary.
[[nodiscard]] std::shared_ptr<RuntimeWorkExecutor>
acquire_runtime_work_executor() noexcept;

// Stops the active generation after socket teardown has closed every producer.
void stop_runtime_work_executor() noexcept;

} // namespace robotweax::srt::compat
