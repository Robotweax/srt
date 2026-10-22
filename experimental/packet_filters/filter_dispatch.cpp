#include "filter_dispatch.hpp"

#include <algorithm>
#include <cstring>

namespace robotweax::srt::extensions {
namespace {

bool disjoint(std::span<std::byte> first, std::span<std::byte> second) noexcept
{
    if (first.empty() || second.empty())
        return true;
    const auto a = reinterpret_cast<std::uintptr_t>(first.data());
    const auto b = reinterpret_cast<std::uintptr_t>(second.data());
    return a <= b ? b - a >= first.size() : a - b >= second.size();
}

} // namespace

Error FilterOutputWriter::append(const FilterOutput& output) noexcept
{
    if (error_ != Error::none)
        return error_;
    if (count_ == outputs_.size() || output.payload.empty()
        || output.payload.size() > bytes_.size() - used_) {
        error_ = Error::invalid_state;
        return error_;
    }
    auto copied = bytes_.subspan(used_, output.payload.size());
    std::memmove(copied.data(), output.payload.data(), copied.size());
    outputs_[count_] = output;
    outputs_[count_].payload = copied;
    ++count_;
    used_ += copied.size();
    return Error::none;
}

Error FilterInstance::start(const FilterImplementation& implementation,
    FilterLimits limits, FilterOwner owner,
    const FilterEnvironment& environment, std::span<std::byte> state,
    std::span<FilterOutput> outputs, std::span<std::byte> output_bytes) noexcept
{
    if (started_)
        return Error::invalid_state;
    started_ = true;
    const auto& descriptor = implementation.descriptor;
    if (implementation.interface_version != 1
        || implementation.construct == nullptr
        || implementation.on_event == nullptr
        || implementation.destroy == nullptr
        || implementation.state_alignment == 0
        || implementation.state_alignment > alignof(std::max_align_t)
        || (implementation.state_alignment
               & (implementation.state_alignment - 1))
            != 0
        || limits.maximum_members > paths_.size()
        || limits.maximum_receivers > receivers_.size()
        || admit_filter(descriptor, limits, owner, environment) != Error::none
        || (descriptor.state_bytes_per_receiver != 0
            && limits.maximum_receivers
                > (limits.maximum_state_bytes - descriptor.state_bytes)
                    / descriptor.state_bytes_per_receiver)
        || outputs.size() < descriptor.maximum_outputs_per_batch
        || output_bytes.size() < descriptor.maximum_output_bytes_per_batch)
        return failure_ = Error::invalid_state;
    const auto reserved = descriptor.state_bytes
        + descriptor.state_bytes_per_receiver * limits.maximum_receivers;
    if (state.size() < reserved
        || (reserved != 0
            && reinterpret_cast<std::uintptr_t>(state.data())
                    % implementation.state_alignment
                != 0))
        return failure_ = Error::invalid_state;
    state = state.first(reserved);
    outputs = outputs.first(descriptor.maximum_outputs_per_batch);
    output_bytes =
        output_bytes.first(descriptor.maximum_output_bytes_per_batch);
    const auto metadata = std::as_writable_bytes(outputs);
    if (!disjoint(state, output_bytes) || !disjoint(state, metadata)
        || !disjoint(output_bytes, metadata))
        return failure_ = Error::invalid_state;
    implementation_ = implementation;
    limits_ = limits;
    owner_ = owner;
    state_ = state;
    outputs_ = outputs;
    output_bytes_ = output_bytes;
    snapshot(environment);
    std::fill(state_.begin(), state_.end(), std::byte {});
    used_outputs_ = outputs_.size();
    used_bytes_ = output_bytes_.size();
    clear_outputs();
    try {
        failure_ =
            implementation_.construct(state_, owner_, limits_, environment_);
    } catch (...) {
        failure_ = Error::invalid_state;
    }
    constructed_ = failure_ == Error::none;
    return failure_;
}

void FilterInstance::snapshot(const FilterEnvironment& environment) noexcept
{
    std::copy(
        environment.paths.begin(), environment.paths.end(), paths_.begin());
    std::copy(environment.receivers.begin(), environment.receivers.end(),
        receivers_.begin());
    environment_ = environment;
    environment_.paths = std::span {paths_}.first(environment.paths.size());
    environment_.receivers =
        std::span {receivers_}.first(environment.receivers.size());
}

void FilterInstance::clear_outputs() noexcept
{
    std::fill_n(outputs_.begin(), used_outputs_, FilterOutput {});
    std::fill_n(output_bytes_.begin(), used_bytes_, std::byte {});
    used_outputs_ = 0;
    used_bytes_ = 0;
}

FilterDispatchResult FilterInstance::dispatch(const FilterEvent& event) noexcept
{
    clear_outputs();
    if (!constructed_)
        return {.error = Error::invalid_state};
    if (failure_ != Error::none)
        return {.error = failure_};
    if (has_event_ && event.now_microseconds < last_time_)
        return {.error = failure_ = Error::invalid_state};
    FilterOutputWriter writer {outputs_, output_bytes_};
    try {
        failure_ = implementation_.on_event(state_, event, writer);
    } catch (...) {
        failure_ = Error::invalid_state;
    }
    used_outputs_ = writer.count_;
    used_bytes_ = writer.used_;
    if (failure_ == Error::none)
        failure_ = writer.error_;
    const auto proposed = outputs_.first(writer.count_);
    if (failure_ == Error::none)
        failure_ = validate_filter_outputs(implementation_.descriptor, limits_,
            owner_, event.environment, proposed);
    if (failure_ != Error::none) {
        clear_outputs();
        return {.error = failure_};
    }
    last_time_ = event.now_microseconds;
    has_event_ = true;
    return {.outputs = proposed};
}

FilterDispatchResult FilterInstance::process(
    const FilterInput& input, std::uint64_t now) noexcept
{
    if (!constructed_)
        return {.error = Error::invalid_state};
    if (failure_ != Error::none)
        return {.error = failure_};
    if (validate_filter_input(
            implementation_.descriptor, limits_, owner_, environment_, input)
        != Error::none) {
        clear_outputs();
        return {.error = Error::invalid_state};
    }
    return dispatch({.kind = FilterEventKind::input,
        .now_microseconds = now,
        .environment = environment_,
        .input = &input});
}

FilterDispatchResult FilterInstance::poll(std::uint64_t now) noexcept
{
    return dispatch({.kind = FilterEventKind::tick,
        .now_microseconds = now,
        .environment = environment_});
}

FilterDispatchResult FilterInstance::update_environment(
    const FilterEnvironment& environment, std::uint64_t now) noexcept
{
    if (!constructed_)
        return {.error = Error::invalid_state};
    if (failure_ != Error::none)
        return {.error = failure_};
    if (environment.context != environment_.context
        || environment.first_sequence < environment_.first_sequence
        || admit_filter(
               implementation_.descriptor, limits_, owner_, environment)
            != Error::none) {
        clear_outputs();
        return {.error = Error::invalid_state};
    }
    const auto result = dispatch({.kind = FilterEventKind::environment_changed,
        .now_microseconds = now,
        .environment = environment,
        .previous_environment = &environment_});
    if (result.error == Error::none)
        snapshot(environment);
    return result;
}

void FilterInstance::close() noexcept
{
    if (constructed_) {
        implementation_.destroy(state_);
        constructed_ = false;
    }
    std::fill(state_.begin(), state_.end(), std::byte {});
    clear_outputs();
    started_ = true;
}

} // namespace robotweax::srt::extensions
