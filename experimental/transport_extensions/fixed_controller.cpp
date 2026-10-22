#include "compiled_congestion.hpp"

#include <array>
#include <memory>

namespace robotweax::srt::extensions {
namespace {
struct FixedController {
    std::uint64_t desired_rate = 1'000'000;
};
}

std::span<const ControllerDescriptor> compiled_controllers() noexcept
{
    static const std::array descriptors {ControllerDescriptor {
        .name = "rwx-fixed-v1",
        .state_bytes = sizeof(FixedController),
        .state_alignment = alignof(FixedController),
        .construct =
            [](std::span<std::byte> state, ControllerLimits) {
                std::construct_at(
                    reinterpret_cast<FixedController*>(state.data()));
                return Error::none;
            },
        .on_event =
            [](std::span<std::byte> state, const ControllerEvent&,
                ControllerDecision& result) {
                result = {reinterpret_cast<FixedController*>(state.data())
                              ->desired_rate,
                    32};
                return Error::none;
            },
        .destroy =
            [](std::span<std::byte> state) noexcept {
                std::destroy_at(
                    reinterpret_cast<FixedController*>(state.data()));
            },
    }};
    return descriptors;
}
}
