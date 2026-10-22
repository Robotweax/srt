#pragma once

#include "filter_dispatch.hpp"

#include <string_view>

namespace robotweax::srt::extensions {

inline constexpr std::size_t maximum_compiled_filters = 16;

// A versioned name identifies a complete, fixed configuration in this first
// private provider revision. No FEC geometry or implicit parameter merging.
struct CompiledFilter {
    std::string_view name;
    FilterImplementation implementation;
};

class FilterRegistry {
public:
    explicit FilterRegistry(std::span<const CompiledFilter> filters) noexcept;
    FilterRegistry(const FilterRegistry&) = delete;
    FilterRegistry& operator=(const FilterRegistry&) = delete;
    [[nodiscard]] const CompiledFilter* find(
        std::string_view name) const noexcept;
    [[nodiscard]] Error error() const noexcept
    {
        return error_;
    }

private:
    std::array<CompiledFilter, maximum_compiled_filters> filters_ {};
    std::array<std::array<char, 32>, maximum_compiled_filters> names_ {};
    std::size_t size_ = 0;
    Error error_ = Error::none;
};

// Supplied by the trusted source selected at build time. Callback code must
// outlive every instance. Names and implementation descriptors are copied.
[[nodiscard]] std::span<const CompiledFilter> compiled_filters() noexcept;
[[nodiscard]] const CompiledFilter* compiled_filter(
    std::string_view name) noexcept;

} // namespace robotweax::srt::extensions
