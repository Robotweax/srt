#include "congestion.hpp"
#include "robotweax/srt/socket_options.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

using namespace robotweax::srt;
using namespace robotweax::srt::extensions;

namespace {
std::uint64_t integer(
    std::span<const std::uint8_t> bytes, std::size_t offset) noexcept
{
    std::uint64_t result = 0;
    for (unsigned index = 0; index < 8; ++index)
        result = (result << 8U)
            | (offset + index < bytes.size() ? bytes[offset + index] : 0U);
    return result;
}
ControllerDescriptor descriptor()
{
    return {.name = "rwx-fuzz-v1",
        .state_bytes = 1,
        .construct =
            [](std::span<std::byte>, ControllerLimits) {
                return Error::none;
            },
        .on_event =
            [](std::span<std::byte>, const ControllerEvent& event,
                ControllerDecision& decision) {
                if (event.round_trip_time_microseconds & 1U)
                    return Error::invalid_state;
                decision = {event.acknowledged_bytes, event.lost_packets};
                return Error::none;
            },
        .destroy = [](std::span<std::byte>) noexcept { }};
}
}

extern "C" int LLVMFuzzerTestOneInput(
    const std::uint8_t* data, std::size_t size)
{
    const auto bytes = std::span {data, size};
    const auto bounded = bytes.first(std::min<std::size_t>(size, 64));
    SocketOptions options;
    (void)options.set_congestion_controller(
        {reinterpret_cast<const char*>(bounded.data()), bounded.size()});
    auto candidate = descriptor();
    candidate.name = {
        reinterpret_cast<const char*>(bounded.data()), bounded.size()};
    candidate.state_bytes = static_cast<std::size_t>(integer(bytes, 0) % 2048);
    candidate.state_alignment = std::size_t {1} << (integer(bytes, 8) % 8);
    const std::array table {candidate, descriptor()};
    const ControllerRegistry registry {table};
    if (registry.error() != Error::none && registry.find("rwx-fuzz-v1"))
        std::abort();

    const ControllerLimits limits {
        integer(bytes, 0), static_cast<std::uint32_t>(integer(bytes, 8))};
    ControllerInstance instance;
    bool failed = instance.start(descriptor(), limits) != Error::none;
    for (std::size_t index = 0; index < 16; ++index) {
        const auto offset = index * 5U;
        const ControllerEvent event {.kind = static_cast<ControllerEventKind>(
                                         integer(bytes, offset) % 6),
            .now_microseconds = integer(bytes, offset + 1),
            .acknowledged_bytes = integer(bytes, offset + 2),
            .lost_packets =
                static_cast<std::uint32_t>(integer(bytes, offset + 3)),
            .round_trip_time_microseconds =
                static_cast<std::uint32_t>(integer(bytes, offset + 4))};
        const auto result = instance.dispatch(event);
        if (result.error == Error::none) {
            if (failed || !result.decision.rate_bytes_per_second
                || !result.decision.window_packets
                || result.decision.rate_bytes_per_second
                    > limits.maximum_rate_bytes_per_second
                || result.decision.window_packets
                    > limits.maximum_window_packets)
                std::abort();
        } else
            failed = true;
    }
    instance.close();
    instance.close();
    if (instance.dispatch({}).error == Error::none)
        std::abort();
    return 0;
}
