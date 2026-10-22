#include "filter_dispatch.hpp"
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

FilterImplementation filter()
{
    return {.descriptor = {.scope = FilterScope::logical_stream,
                .feedback = FeedbackRequirement::none,
                .state_bytes = sizeof(Counter),
                .maximum_outputs_per_batch = 1,
                .maximum_output_bytes_per_batch = 1},
        .state_alignment = alignof(Counter),
        .construct =
            [](std::span<std::byte> state, const FilterOwner&, FilterLimits,
                const FilterEnvironment&) {
                std::construct_at(reinterpret_cast<Counter*>(state.data()));
                return Error::none;
            },
        .on_event =
            [](std::span<std::byte> state, const FilterEvent& event,
                FilterOutputWriter& writer) {
                if (event.kind != FilterEventKind::input)
                    return Error::none;
                auto& counter = *reinterpret_cast<Counter*>(state.data());
                if (++counter.calls != event.input->sequence)
                    return Error::invalid_state;
                return writer.append({.context = event.environment.context,
                    .domain = SequenceDomain::logical_stream,
                    .sequence = event.input->sequence,
                    .receiver = event.input->receiver,
                    .payload = event.input->payload});
            },
        .destroy =
            [](std::span<std::byte> state) noexcept {
                std::destroy_at(reinterpret_cast<Counter*>(state.data()));
            }};
}

struct Decoder {
    std::array<PathBudget, 1> paths {{{{1, 1}, 1}}};
    std::array<ReceiverBudget, 2> receivers {{{{101, 1}, {}}, {{102, 1}, {}}}};
    FilterEnvironment environment {.context = {{1, 1}, {1, 1}, 1, 0},
        .paths = paths,
        .receivers = receivers,
        .first_sequence = 1,
        .end_sequence = rounds + 2,
        .maximum_recovered_payload_bytes = 1};
    alignas(std::max_align_t) std::array<std::byte, sizeof(Counter)> state {};
    std::array<FilterOutput, 1> outputs {};
    std::array<std::byte, 1> output_bytes {};
    std::array<std::byte, 1> payload {};
    Identity owner {};
    FilterInstance instance; // Destroy before borrowed storage.

    Error start(const FilterImplementation& implementation, std::size_t index)
    {
        owner = receivers[index].receiver;
        payload[0] = static_cast<std::byte>(index + 1);
        return instance.start(implementation,
            {.maximum_members = 1,
                .maximum_receivers = 2,
                .maximum_state_bytes = state.size(),
                .maximum_outputs_per_batch = outputs.size(),
                .maximum_output_bytes_per_batch = output_bytes.size()},
            {.scope = FilterScope::logical_stream, .receiver = owner},
            environment, state, outputs, output_bytes);
    }

    bool process(std::uint64_t sequence)
    {
        const auto result =
            instance.process({.context = environment.context,
                                 .domain = SequenceDomain::logical_stream,
                                 .sequence = sequence,
                                 .receiver = owner,
                                 .payload = payload},
                sequence);
        return result.error == Error::none && result.outputs.size() == 1
            && result.outputs[0].receiver == owner
            && result.outputs[0].sequence == sequence
            && result.outputs[0].payload.data() == output_bytes.data()
            && result.outputs[0].payload[0] == payload[0];
    }
};

} // namespace

TEST(extension_filter_receiver_instances_keep_storage_across_executor_handoffs)
{
    const auto implementation = filter();
    std::array<Decoder, 2> decoders;
    for (std::size_t index = 0; index < decoders.size(); ++index) {
        REQUIRE_EQ(decoders[index].start(implementation, index), Error::none);
    }
    std::array<bool, 2> healthy {true, true};
    std::barrier ready {2};
    {
        std::array<std::jthread, 2> workers;
        for (std::size_t index = 0; index < workers.size(); ++index) {
            workers[index] = std::jthread([&, index] {
                ready.arrive_and_wait();
                for (std::uint64_t sequence = 1; sequence <= rounds;
                    ++sequence) {
                    healthy[index] =
                        decoders[index].process(sequence) && healthy[index];
                }
            });
        }
    }
    for (std::size_t index = 0; index < decoders.size(); ++index) {
        REQUIRE(healthy[index]);
        REQUIRE(decoders[index].process(rounds + 1));
        decoders[index].instance.close();
        REQUIRE(std::all_of(decoders[index].state.begin(),
            decoders[index].state.end(), [](auto b) {
                return b == std::byte {};
            }));
        REQUIRE_EQ(decoders[index].instance.poll(rounds + 2).error,
            Error::invalid_state);
    }
}
