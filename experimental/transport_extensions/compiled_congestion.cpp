#include "compiled_congestion.hpp"

namespace robotweax::srt::extensions {
namespace {
const ControllerRegistry& registry() noexcept
{
    static const ControllerRegistry selected {compiled_controllers()};
    return selected;
}
}

const ControllerDescriptor* compiled_controller(
    CongestionController controller) noexcept
{
    const auto value = static_cast<std::uint8_t>(controller);
    return value >= 128 ? registry().at(value - 128U) : nullptr;
}

bool parse_compiled_controller(
    std::string_view name, CongestionController& controller) noexcept
{
    for (std::size_t index = 0; index < maximum_controllers; ++index) {
        const auto* descriptor = registry().at(index);
        if (descriptor == nullptr)
            break;
        if (descriptor->name == name) {
            controller = static_cast<CongestionController>(128U + index);
            return true;
        }
    }
    return false;
}
}

namespace robotweax::srt::controller_extension_detail {
std::string_view experimental_congestion_controller_name(
    CongestionController controller) noexcept
{
    const auto* descriptor = extensions::compiled_controller(controller);
    return descriptor == nullptr ? std::string_view {} : descriptor->name;
}
}
