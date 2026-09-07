#pragma once

#include "srt/srt.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace robotweax::srt::compat {

inline constexpr std::size_t maximum_group_config_options = 32;
inline constexpr std::size_t maximum_group_option_bytes = 256;

struct GroupOptionValue {
    SRT_SOCKOPT option = SRTO_MSS;
    std::uint16_t size = 0;
    std::array<std::byte, maximum_group_option_bytes> bytes{};
};

struct GroupConfigSnapshot {
    std::array<GroupOptionValue, maximum_group_config_options> options{};
    std::size_t size = 0;

    ~GroupConfigSnapshot() noexcept;
};

class GroupSocketConfiguration {
public:
    ~GroupSocketConfiguration() noexcept;

    [[nodiscard]] bool add(
        SRT_SOCKOPT option, const void* value, int value_size) noexcept;
    [[nodiscard]] GroupConfigSnapshot snapshot() const noexcept;

private:
    mutable std::mutex mutex_;
    std::array<GroupOptionValue, maximum_group_config_options> options_{};
    std::size_t size_ = 0;
};

[[nodiscard]] bool is_group_member_option(SRT_SOCKOPT option) noexcept;

} // namespace robotweax::srt::compat

// This completes the public header's opaque forward declaration without
// exposing any private storage through the installed ABI.
struct SRT_SocketOptionObject {
    robotweax::srt::compat::GroupSocketConfiguration storage;
};
