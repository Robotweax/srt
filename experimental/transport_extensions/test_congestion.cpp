#include "congestion.hpp"
#include "test.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <memory>

using namespace robotweax::srt;
using namespace robotweax::srt::extensions;

namespace {

struct SampleState {
    std::uint64_t rate = 1'000'000;
    std::uint32_t window = 64;
};

std::size_t destructions = 0;
std::size_t invocations = 0;

ControllerDescriptor sample_descriptor()
{
    return {
        .name = "sample-v1",
        .state_bytes = sizeof(SampleState),
        .state_alignment = alignof(SampleState),
        .construct =
            [](std::span<std::byte> storage, ControllerLimits) {
                std::construct_at(
                    reinterpret_cast<SampleState*>(storage.data()));
                return Error::none;
            },
        .on_event =
            [](std::span<std::byte> storage, const ControllerEvent& event,
                ControllerDecision& decision) {
                ++invocations;
                auto& state = *reinterpret_cast<SampleState*>(storage.data());
                if (event.kind == ControllerEventKind::loss
                    || event.kind == ControllerEventKind::timeout) {
                    state.rate = std::max<std::uint64_t>(1, state.rate / 2);
                    state.window = std::max<std::uint32_t>(1, state.window / 2);
                }
                decision = {state.rate, state.window};
                return Error::none;
            },
        .destroy =
            [](std::span<std::byte> storage) noexcept {
                std::destroy_at(reinterpret_cast<SampleState*>(storage.data()));
                ++destructions;
            },
    };
}

} // namespace

TEST(extension_controller_compiled_registry_dispatches_and_enforces_core_limits)
{
    const std::array descriptors {sample_descriptor()};
    const ControllerRegistry registry {descriptors};
    REQUIRE_EQ(registry.error(), Error::none);
    REQUIRE(registry.find("unknown-v1") == nullptr);
    const auto* descriptor = registry.find("sample-v1");
    REQUIRE(descriptor != nullptr);
    const auto before = destructions;
    ControllerInstance instance;
    REQUIRE_EQ(instance.start(*descriptor, {750'000, 32}), Error::none);
    auto result = instance.dispatch({.now_microseconds = 100});
    REQUIRE_EQ(result.error, Error::none);
    REQUIRE_EQ(result.decision.rate_bytes_per_second, 750'000U);
    REQUIRE_EQ(result.decision.window_packets, 32U);
    result = instance.dispatch(
        {.kind = ControllerEventKind::loss, .now_microseconds = 200});
    REQUIRE_EQ(result.error, Error::none);
    REQUIRE_EQ(result.decision.rate_bytes_per_second, 500'000U);
    REQUIRE_EQ(result.decision.window_packets, 32U);
    instance.close();
    instance.close();
    REQUIRE_EQ(destructions, before + 1);
    REQUIRE_EQ(instance.dispatch({}).error, Error::invalid_state);
    REQUIRE_EQ(instance.start(*descriptor, {1, 1}), Error::invalid_state);
}

TEST(extension_controller_copies_setup_names_and_callback_lifetimes)
{
    ControllerInstance instance;
    {
        std::array<char, 9> name {'s', 'a', 'm', 'p', 'l', 'e', '-', 'v', '1'};
        auto descriptor = sample_descriptor();
        descriptor.name = {name.data(), name.size()};
        const std::array descriptors {descriptor};
        const ControllerRegistry registry {descriptors};
        std::fill(name.begin(), name.end(), 'x');
        const auto* selected = registry.find("sample-v1");
        REQUIRE(selected != nullptr);
        REQUIRE_EQ(instance.start(*selected, {500, 10}), Error::none);
    }
    const auto result = instance.dispatch({});
    REQUIRE_EQ(result.error, Error::none);
    REQUIRE_EQ(result.decision.rate_bytes_per_second, 500U);
}

TEST(extension_controller_registry_rejects_invalid_or_conflicting_descriptors)
{
    for (int invalid = 0; invalid < 9; ++invalid) {
        auto descriptor = sample_descriptor();
        switch (invalid) {
        case 0:
            descriptor.name = "live";
            break;
        case 1:
            descriptor.name = "Bad Name";
            break;
        case 2:
            descriptor.name = {};
            break;
        case 3:
            descriptor.interface_version = 2;
            break;
        case 4:
            descriptor.state_bytes = maximum_controller_state_bytes + 1;
            break;
        case 5:
            descriptor.state_alignment = 3;
            break;
        case 6:
            descriptor.construct = nullptr;
            break;
        case 7:
            descriptor.on_event = nullptr;
            break;
        case 8:
            descriptor.destroy = nullptr;
            break;
        }
        const std::array descriptors {sample_descriptor(), descriptor};
        const ControllerRegistry registry {descriptors};
        REQUIRE_EQ(registry.error(), Error::invalid_state);
        REQUIRE(registry.find("sample-v1") == nullptr);
    }
    const std::array duplicates {sample_descriptor(), sample_descriptor()};
    REQUIRE_EQ(ControllerRegistry {duplicates}.error(), Error::invalid_state);
    std::array<ControllerDescriptor, maximum_controllers + 1> excess {};
    REQUIRE_EQ(ControllerRegistry {excess}.error(), Error::invalid_state);
}

TEST(extension_controller_failure_is_terminal_and_destroys_state_once)
{
    for (const bool throws : {false, true}) {
        auto descriptor = sample_descriptor();
        descriptor.on_event =
            throws ? +[](std::span<std::byte>, const ControllerEvent&,
                          ControllerDecision&) -> Error {
            ++invocations;
            throw 1;
        }
        : +[](std::span<std::byte>, const ControllerEvent&,
               ControllerDecision& decision) {
              ++invocations;
              decision = {0, 1};
              return Error::none;
          };
        const auto before = destructions;
        const auto calls = invocations;
        {
            ControllerInstance instance;
            REQUIRE_EQ(instance.start(descriptor, {1'000, 10}), Error::none);
            const auto failed = instance.dispatch({});
            REQUIRE_EQ(failed.error, Error::invalid_state);
            REQUIRE_EQ(failed.decision.rate_bytes_per_second, 0U);
            REQUIRE_EQ(instance.dispatch({}).error, Error::invalid_state);
            REQUIRE_EQ(invocations, calls + 1);
        }
        REQUIRE_EQ(destructions, before + 1);
    }
}

TEST(extension_controller_rejects_backwards_time_and_invalid_events)
{
    const auto descriptor = sample_descriptor();
    for (const bool invalid_kind : {false, true}) {
        ControllerInstance instance;
        REQUIRE_EQ(instance.start(descriptor, {1'000, 10}), Error::none);
        REQUIRE_EQ(
            instance.dispatch({.now_microseconds = 10}).error, Error::none);
        const auto event = invalid_kind
            ? ControllerEvent {.kind = static_cast<ControllerEventKind>(255),
                  .now_microseconds = 11}
            : ControllerEvent {.now_microseconds = 9};
        REQUIRE_EQ(instance.dispatch(event).error, Error::invalid_state);
    }
}

TEST(extension_controller_constructor_failure_and_extreme_outputs_are_bounded)
{
    auto descriptor = sample_descriptor();
    const auto before = destructions;
    descriptor.construct = [](std::span<std::byte>, ControllerLimits) -> Error {
        throw 1;
    };
    {
        ControllerInstance instance;
        REQUIRE_EQ(instance.start(descriptor, {1, 1}), Error::invalid_state);
        REQUIRE_EQ(instance.dispatch({}).error, Error::invalid_state);
    }
    REQUIRE_EQ(destructions, before);
    descriptor = sample_descriptor();
    descriptor.on_event = [](std::span<std::byte>, const ControllerEvent&,
                              ControllerDecision& result) {
        result = {std::numeric_limits<std::uint64_t>::max(),
            std::numeric_limits<std::uint32_t>::max()};
        return Error::none;
    };
    ControllerInstance instance;
    REQUIRE_EQ(instance.start(descriptor, {999, 17}), Error::none);
    const auto result = instance.dispatch({});
    REQUIRE_EQ(result.error, Error::none);
    REQUIRE_EQ(result.decision.rate_bytes_per_second, 999U);
    REQUIRE_EQ(result.decision.window_packets, 17U);
}
