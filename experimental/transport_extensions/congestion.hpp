#pragma once

#include "robotweax/srt/error.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace robotweax::srt::extensions {

inline constexpr std::size_t maximum_controller_state_bytes = 1'024;
inline constexpr std::size_t maximum_controllers = 16;

struct ControllerLimits {
    std::uint64_t maximum_rate_bytes_per_second = 0;
    std::uint32_t maximum_window_packets = 0;
};

enum class ControllerEventKind : std::uint8_t {
    tick,
    acknowledgement,
    loss,
    timeout
};

// Feedback has already passed the owning transport's ACK/window checks.
struct ControllerEvent {
    ControllerEventKind kind = ControllerEventKind::tick;
    std::uint64_t now_microseconds = 0;
    std::uint64_t acknowledged_bytes = 0;
    std::uint32_t lost_packets = 0;
    std::uint32_t round_trip_time_microseconds = 0;
    ControllerLimits limits {};
};

struct ControllerDecision {
    std::uint64_t rate_bytes_per_second = 0;
    std::uint32_t window_packets = 0;
};

struct ControllerResult {
    Error error = Error::none;
    ControllerDecision decision {};
};

// Callback code must outlive every instance. Construction
// failure must release partially constructed state before returning/throwing.
// No callback may block, reenter the transport or retain event references.
// Different instances can call the same provider concurrently. Serialization
// covers one instance, not a fixed thread or provider-global mutable state.
struct ControllerDescriptor {
    std::string_view name;
    std::uint32_t interface_version = 1;
    std::size_t state_bytes = 0;
    std::size_t state_alignment = 1;
    Error (*construct)(std::span<std::byte>, ControllerLimits) = nullptr;
    Error (*on_event)(std::span<std::byte>, const ControllerEvent&,
        ControllerDecision&) = nullptr;
    void (*destroy)(std::span<std::byte>) noexcept = nullptr;
};

// Copies a bounded descriptor table and names at setup; never mutated during dispatch.
class ControllerRegistry {
public:
    explicit ControllerRegistry(
        std::span<const ControllerDescriptor> descriptors) noexcept;
    ControllerRegistry(const ControllerRegistry&) = delete;
    ControllerRegistry& operator=(const ControllerRegistry&) = delete;
    ControllerRegistry(ControllerRegistry&&) = delete;
    ControllerRegistry& operator=(ControllerRegistry&&) = delete;
    [[nodiscard]] Error error() const noexcept
    {
        return error_;
    }
    [[nodiscard]] const ControllerDescriptor* find(
        std::string_view name) const noexcept;
    [[nodiscard]] const ControllerDescriptor* at(
        std::size_t index) const noexcept
    {
        return error_ == Error::none && index < size_ ? &descriptors_[index]
                                                      : nullptr;
    }

private:
    std::array<ControllerDescriptor, maximum_controllers> descriptors_ {};
    std::array<std::array<char, 32>, maximum_controllers> names_ {};
    std::size_t size_ = 0;
    Error error_ = Error::none;
};

// One instance is serialized by its owner. No allocation on setup or dispatch.
// Descriptor callbacks are copied at start. Close is terminal and idempotent.
class ControllerInstance {
public:
    ControllerInstance() = default;
    ~ControllerInstance() noexcept
    {
        close();
    }
    ControllerInstance(const ControllerInstance&) = delete;
    ControllerInstance& operator=(const ControllerInstance&) = delete;
    ControllerInstance(ControllerInstance&&) = delete;
    ControllerInstance& operator=(ControllerInstance&&) = delete;

    [[nodiscard]] Error start(const ControllerDescriptor& descriptor,
        ControllerLimits limits) noexcept;
    [[nodiscard]] ControllerResult dispatch(
        const ControllerEvent& event) noexcept;
    [[nodiscard]] Error set_limits(ControllerLimits limits) noexcept;
    void close() noexcept;

private:
    [[nodiscard]] std::span<std::byte> storage() noexcept;
    alignas(std::max_align_t)
        std::array<std::byte, maximum_controller_state_bytes> state_ {};
    ControllerDescriptor descriptor_ {};
    ControllerLimits limits_ {};
    Error failure_ = Error::none;
    std::uint64_t last_time_ = 0;
    bool started_ = false;
    bool constructed_ = false;
    bool has_event_ = false;
};

} // namespace robotweax::srt::extensions
