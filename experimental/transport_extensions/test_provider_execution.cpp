#include "congestion.hpp"
#include "test.hpp"

#include <algorithm>
#include <array>
#include <barrier>
#include <memory>
#include <thread>

using namespace robotweax::srt;
using namespace robotweax::srt::extensions;

namespace {

constexpr std::uint64_t rounds = 256;

struct Counter {
    std::uint64_t calls = 0;
};

ControllerDescriptor controller()
{
    return {.name = "execution-v1",
        .state_bytes = sizeof(Counter),
        .state_alignment = alignof(Counter),
        .construct =
            [](std::span<std::byte> state, ControllerLimits) {
                std::construct_at(reinterpret_cast<Counter*>(state.data()));
                return Error::none;
            },
        .on_event =
            [](std::span<std::byte> state, const ControllerEvent&,
                ControllerDecision& decision) {
                auto& counter = *reinterpret_cast<Counter*>(state.data());
                decision = {++counter.calls, 1};
                return Error::none;
            },
        .destroy =
            [](std::span<std::byte> state) noexcept {
                std::destroy_at(reinterpret_cast<Counter*>(state.data()));
            }};
}

} // namespace

TEST(extension_controller_instances_share_code_without_thread_affinity_or_state)
{
    const auto descriptor = controller();
    std::array<ControllerInstance, 2> instances;
    for (auto& instance : instances) {
        REQUIRE_EQ(instance.start(descriptor, {1'000, 1}), Error::none);
    }
    std::array<bool, 2> healthy {true, true};
    std::barrier ready {2};
    {
        std::array<std::jthread, 2> workers;
        for (std::size_t index = 0; index < workers.size(); ++index) {
            workers[index] = std::jthread([&, index] {
                // Only the owner waits; provider callbacks never block.
                ready.arrive_and_wait();
                for (std::uint64_t tick = 1; tick <= rounds; ++tick) {
                    const auto result =
                        instances[index].dispatch({.now_microseconds = tick});
                    healthy[index] = healthy[index]
                        && result.error == Error::none
                        && result.decision.rate_bytes_per_second == tick;
                }
            });
        }
    } // Joining hands both instances back to this thread.
    for (std::size_t index = 0; index < instances.size(); ++index) {
        REQUIRE(healthy[index]);
        const auto result =
            instances[index].dispatch({.now_microseconds = rounds + 1});
        REQUIRE_EQ(result.error, Error::none);
        REQUIRE_EQ(result.decision.rate_bytes_per_second, rounds + 1);
        instances[index].close();
        REQUIRE_EQ(instances[index].dispatch({}).error, Error::invalid_state);
    }
}
