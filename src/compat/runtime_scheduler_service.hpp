#pragma once

#include "compat/runtime_scheduler.hpp"

#include <memory>
#include <optional>
#include <string_view>

namespace robotweax::srt::compat {

// Strict decimal process-setting parser; no whitespace, signs or trailing
// characters. The bound keeps worker count and preallocated storage finite.
[[nodiscard]] std::optional<std::size_t> parse_runtime_scheduler_shards(
    std::string_view value) noexcept;

// Constructs the lifecycle owner early enough that socket-registry teardown
// always precedes scheduler destruction. It does not start worker threads.
void prepare_runtime_scheduler_service() noexcept;

// Returns the process-wide bounded scheduler, starting a fresh generation on
// demand after final srt_cleanup(). A null result is fail-closed startup.
[[nodiscard]] std::shared_ptr<RuntimeScheduler>
acquire_runtime_scheduler() noexcept;

// Stops the current generation after socket teardown has joined every event
// source. A later srt_startup()/socket generation may acquire a new instance.
void stop_runtime_scheduler() noexcept;

} // namespace robotweax::srt::compat
