#pragma once

#include "filter.hpp"

#include <array>

namespace robotweax::srt::extensions {

// These bound the prototype's snapshot storage, not a group-mode enumeration.
inline constexpr std::size_t maximum_filter_members = 64;
inline constexpr std::size_t maximum_filter_receivers = 256;

enum class FilterEventKind : std::uint8_t { input, tick, environment_changed };

struct FilterEvent {
    FilterEventKind kind = FilterEventKind::tick;
    std::uint64_t now_microseconds = 0;
    FilterEnvironment environment {};
    // Present only during environment_changed. Generations identify joins,
    // departures and replacements without resetting shared stream state.
    const FilterEnvironment* previous_environment = nullptr;
    const FilterInput* input = nullptr;
};

class FilterInstance;

// Copies proposed bytes into owner-supplied storage. An ignored append failure
// still fails the entire callback batch. No outputs are visible before validation.
class FilterOutputWriter {
public:
    [[nodiscard]] Error append(const FilterOutput& output) noexcept;

private:
    friend class FilterInstance;
    FilterOutputWriter(
        std::span<FilterOutput> outputs, std::span<std::byte> bytes) noexcept
        : outputs_(outputs)
        , bytes_(bytes)
    {
    }
    std::span<FilterOutput> outputs_;
    std::span<std::byte> bytes_;
    std::size_t count_ = 0;
    std::size_t used_ = 0;
    Error error_ = Error::none;
};

struct FilterImplementation {
    FilterDescriptor descriptor;
    std::uint32_t interface_version = 1;
    std::size_t state_alignment = 1;
    // State contains base bytes followed by reserved receiver capacity. The
    // implementation owns its bounded receiver-to-slot map and initialization.
    // Failed construction must clean up partial state before returning/throwing.
    Error (*construct)(std::span<std::byte>, const FilterOwner&, FilterLimits,
        const FilterEnvironment&) = nullptr;
    Error (*on_event)(std::span<std::byte>, const FilterEvent&,
        FilterOutputWriter&) = nullptr;
    void (*destroy)(std::span<std::byte>) noexcept = nullptr;
};

struct FilterDispatchResult {
    Error error = Error::none;
    // Borrowed until the next dispatch/update/close. The owner must reserve
    // queue and rate capacity before atomically committing a complete batch.
    // Output atomicity does not roll back callback state. Reserve before the
    // callback or terminate on commit failure; do not replay the event blindly.
    std::span<const FilterOutput> outputs {};
};

// One member or group executor serializes an instance. Callbacks must not
// block, reenter APIs, allocate on event paths or retain borrowed references.
// All supplied storage must outlive the instance; callback code must stay loaded.
// Different instances may execute the same callbacks concurrently. Ownership
// may move between threads after synchronization; no thread affinity is promised.
class FilterInstance {
public:
    FilterInstance() = default;
    ~FilterInstance() noexcept
    {
        close();
    }
    FilterInstance(const FilterInstance&) = delete;
    FilterInstance& operator=(const FilterInstance&) = delete;
    FilterInstance(FilterInstance&&) = delete;
    FilterInstance& operator=(FilterInstance&&) = delete;

    [[nodiscard]] Error start(const FilterImplementation& implementation,
        FilterLimits limits, FilterOwner owner,
        const FilterEnvironment& environment, std::span<std::byte> state,
        std::span<FilterOutput> outputs,
        std::span<std::byte> output_bytes) noexcept;
    [[nodiscard]] FilterDispatchResult process(
        const FilterInput& input, std::uint64_t now) noexcept;
    [[nodiscard]] FilterDispatchResult poll(std::uint64_t now) noexcept;
    // Context changes (epoch, direction, configuration or protection) require a
    // fresh instance. Path/receiver churn and retention advance preserve state.
    [[nodiscard]] FilterDispatchResult update_environment(
        const FilterEnvironment& environment, std::uint64_t now) noexcept;
    void close() noexcept;

private:
    [[nodiscard]] FilterDispatchResult dispatch(
        const FilterEvent& event) noexcept;
    void snapshot(const FilterEnvironment& environment) noexcept;
    void clear_outputs() noexcept;
    FilterImplementation implementation_ {};
    FilterLimits limits_ {};
    FilterOwner owner_ {};
    FilterEnvironment environment_ {};
    std::array<PathBudget, maximum_filter_members> paths_ {};
    std::array<ReceiverBudget, maximum_filter_receivers> receivers_ {};
    std::span<std::byte> state_;
    std::span<FilterOutput> outputs_;
    std::span<std::byte> output_bytes_;
    std::size_t used_outputs_ = 0;
    std::size_t used_bytes_ = 0;
    Error failure_ = Error::none;
    std::uint64_t last_time_ = 0;
    bool started_ = false;
    bool constructed_ = false;
    bool has_event_ = false;
};

} // namespace robotweax::srt::extensions
