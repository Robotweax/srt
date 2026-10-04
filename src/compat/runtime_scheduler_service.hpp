#pragma once

#include "compat/runtime_scheduler.hpp"

#include <memory>
#include <optional>
#include <string_view>

namespace robotweax::srt::compat {

class DatagramStorageBudget;
class NativeChannelBudget;

// Internal process owner for up to 4,096 budgeted native channel generations.
// Retained across cleanup; callers must explicitly use the budgeted factories.
[[nodiscard]] std::shared_ptr<NativeChannelBudget>
acquire_runtime_native_channel_budget() noexcept;

// Internal prototype ring ceiling, in MiB, with strict decimal range 1..1024.
[[nodiscard]] std::optional<std::size_t> parse_runtime_inbox_storage_mib(
    std::string_view value) noexcept;

// One lazily allocated process owner, retained across scheduler stop and final
// cleanup. Null is fail-closed; invalid settings/allocation may be retried.
// The prototype must pass it explicitly to inbox/dispatcher admission.
[[nodiscard]] std::shared_ptr<DatagramStorageBudget>
acquire_runtime_inbox_storage_budget() noexcept;

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
