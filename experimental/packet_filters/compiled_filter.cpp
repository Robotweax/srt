#include "compiled_filter.hpp"

#include <algorithm>

namespace robotweax::srt::extensions {
namespace {
bool valid(const CompiledFilter& filter) noexcept
{
    if (!filter.name.starts_with("rwx-") || filter.name.size() < 5
        || filter.name.size() > 32)
        return false;
    for (const auto c : filter.name)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
            return false;
    const auto& impl = filter.implementation;
    return impl.interface_version == 1 && impl.construct && impl.on_event
        && impl.destroy && impl.state_alignment != 0
        && impl.state_alignment <= alignof(std::max_align_t)
        && (impl.state_alignment & (impl.state_alignment - 1)) == 0
        && static_cast<unsigned>(impl.descriptor.scope)
        <= static_cast<unsigned>(FilterScope::logical_stream)
        && static_cast<unsigned>(impl.descriptor.feedback)
        <= static_cast<unsigned>(FeedbackRequirement::required)
        && impl.descriptor.maximum_outputs_per_batch != 0
        && impl.descriptor.maximum_output_bytes_per_batch != 0;
}
}

FilterRegistry::FilterRegistry(std::span<const CompiledFilter> filters) noexcept
{
    if (filters.size() > filters_.size()) {
        error_ = Error::invalid_state;
        return;
    }
    for (const auto& filter : filters) {
        if (!valid(filter)) {
            error_ = Error::invalid_state;
            size_ = 0;
            return;
        }
        for (std::size_t i = 0; i < size_; ++i)
            if (filters_[i].name == filter.name) {
                error_ = Error::invalid_state;
                size_ = 0;
                return;
            }
        std::copy(
            filter.name.begin(), filter.name.end(), names_[size_].begin());
        filters_[size_] = filter;
        filters_[size_].name = {names_[size_].data(), filter.name.size()};
        ++size_;
    }
}

const CompiledFilter* FilterRegistry::find(std::string_view name) const noexcept
{
    if (error_ == Error::none)
        for (std::size_t i = 0; i < size_; ++i)
            if (filters_[i].name == name)
                return &filters_[i];
    return nullptr;
}

const CompiledFilter* compiled_filter(std::string_view name) noexcept
{
    static const FilterRegistry registry {compiled_filters()};
    return registry.find(name);
}
}
