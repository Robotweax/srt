#pragma once

#include "congestion.hpp"
#include "robotweax/srt/handshake_extensions.hpp"

namespace robotweax::srt::extensions {

// Implemented by the selected build-time provider. Names are wire identities;
// local table indices are never transmitted and may differ between peers.
[[nodiscard]] std::span<const ControllerDescriptor>
compiled_controllers() noexcept;
[[nodiscard]] const ControllerDescriptor* compiled_controller(
    CongestionController controller) noexcept;
[[nodiscard]] bool parse_compiled_controller(
    std::string_view name, CongestionController& controller) noexcept;

} // namespace robotweax::srt::extensions
