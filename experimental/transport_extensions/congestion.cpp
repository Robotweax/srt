#include "congestion.hpp"

#include <algorithm>

namespace robotweax::srt::extensions {
namespace {

bool valid_descriptor(const ControllerDescriptor& descriptor) noexcept
{
    if (descriptor.name.empty() || descriptor.name.size() > 32
        || descriptor.name == "live" || descriptor.name == "file"
        || descriptor.name == "control-v1" || descriptor.interface_version != 1
        || descriptor.state_bytes == 0
        || descriptor.state_bytes > maximum_controller_state_bytes
        || descriptor.state_alignment == 0
        || descriptor.state_alignment > alignof(std::max_align_t)
        || (descriptor.state_alignment & (descriptor.state_alignment - 1)) != 0
        || descriptor.construct == nullptr || descriptor.on_event == nullptr
        || descriptor.destroy == nullptr)
        return false;
    for (const char character : descriptor.name) {
        if (!((character >= 'a' && character <= 'z')
                || (character >= '0' && character <= '9') || character == '-'))
            return false;
    }
    return true;
}

} // namespace

ControllerRegistry::ControllerRegistry(
    std::span<const ControllerDescriptor> descriptors) noexcept
{
    if (descriptors.size() > descriptors_.size()) {
        error_ = Error::invalid_state;
        return;
    }
    for (const auto& descriptor : descriptors) {
        if (!valid_descriptor(descriptor)) {
            error_ = Error::invalid_state;
            size_ = 0;
            return;
        }
        for (std::size_t index = 0; index < size_; ++index) {
            if (descriptors_[index].name == descriptor.name) {
                error_ = Error::invalid_state;
                size_ = 0;
                return;
            }
        }
        auto& copied = descriptors_[size_];
        copied = descriptor;
        std::copy(descriptor.name.begin(), descriptor.name.end(),
            names_[size_].begin());
        copied.name = {names_[size_].data(), descriptor.name.size()};
        ++size_;
    }
}

const ControllerDescriptor* ControllerRegistry::find(
    std::string_view name) const noexcept
{
    if (error_ != Error::none)
        return nullptr;
    for (std::size_t index = 0; index < size_; ++index) {
        if (descriptors_[index].name == name)
            return &descriptors_[index];
    }
    return nullptr;
}

std::span<std::byte> ControllerInstance::storage() noexcept
{
    return std::span {state_}.first(descriptor_.state_bytes);
}

Error ControllerInstance::start(
    const ControllerDescriptor& descriptor, ControllerLimits limits) noexcept
{
    if (started_)
        return Error::invalid_state;
    started_ = true;
    if (!valid_descriptor(descriptor)
        || limits.maximum_rate_bytes_per_second == 0
        || limits.maximum_window_packets == 0) {
        failure_ = Error::invalid_state;
        return failure_;
    }
    descriptor_ = descriptor;
    descriptor_.name = {};
    limits_ = limits;
    try {
        failure_ = descriptor_.construct(storage(), limits);
    } catch (...) {
        failure_ = Error::invalid_state;
    }
    constructed_ = failure_ == Error::none;
    return failure_;
}

ControllerResult ControllerInstance::dispatch(
    const ControllerEvent& event) noexcept
{
    if (!constructed_)
        return {.error = Error::invalid_state};
    if (failure_ != Error::none)
        return {.error = failure_};
    if ((has_event_ && event.now_microseconds < last_time_)
        || static_cast<unsigned>(event.kind)
            > static_cast<unsigned>(ControllerEventKind::timeout)) {
        failure_ = Error::invalid_state;
        return {.error = failure_};
    }
    ControllerDecision proposed;
    try {
        auto supplied = event;
        supplied.limits = limits_;
        failure_ = descriptor_.on_event(storage(), supplied, proposed);
    } catch (...) {
        failure_ = Error::invalid_state;
    }
    if (failure_ == Error::none
        && (proposed.rate_bytes_per_second == 0
            || proposed.window_packets == 0))
        failure_ = Error::invalid_state;
    if (failure_ != Error::none)
        return {.error = failure_};
    last_time_ = event.now_microseconds;
    has_event_ = true;
    return {
        .decision = {std::min(proposed.rate_bytes_per_second,
                         limits_.maximum_rate_bytes_per_second),
            std::min(proposed.window_packets, limits_.maximum_window_packets)}};
}

Error ControllerInstance::set_limits(ControllerLimits limits) noexcept
{
    if (!constructed_ || failure_ != Error::none
        || limits.maximum_rate_bytes_per_second == 0
        || limits.maximum_window_packets == 0)
        return Error::invalid_state;
    limits_ = limits;
    return Error::none;
}

void ControllerInstance::close() noexcept
{
    if (constructed_) {
        descriptor_.destroy(storage());
        constructed_ = false;
    }
    std::fill(state_.begin(), state_.end(), std::byte {});
    started_ = true;
}

} // namespace robotweax::srt::extensions
