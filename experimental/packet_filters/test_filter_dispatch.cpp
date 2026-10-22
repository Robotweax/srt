#include "filter_dispatch.hpp"
#include "test.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <stdexcept>

using namespace robotweax::srt;
using namespace robotweax::srt::extensions;

namespace {

enum class Behavior {
    normal,
    invalid_second,
    overflow,
    throws,
    fails,
    feedback,
    stale_path
};
struct Observation {
    unsigned constructs = 0;
    unsigned destroys = 0;
    unsigned calls = 0;
    unsigned changes = 0;
    std::uint64_t old_generation = 0;
    std::uint64_t new_generation = 0;
    Behavior behavior = Behavior::normal;
    bool fail_construct = false;
} observed;

struct State {
    std::array<std::byte, 4> parity {};
    unsigned inputs = 0;
};

FilterImplementation implementation()
{
    return {.descriptor = {.scope = FilterScope::logical_stream,
                .feedback = FeedbackRequirement::none,
                .state_bytes = sizeof(State),
                .state_bytes_per_receiver = 8,
                .maximum_outputs_per_batch = 2,
                .maximum_output_bytes_per_batch = 8},
        .state_alignment = alignof(State),
        .construct =
            [](std::span<std::byte> state, const FilterOwner&, FilterLimits,
                const FilterEnvironment&) {
                ++observed.constructs;
                if (observed.fail_construct)
                    return Error::invalid_state;
                std::construct_at(reinterpret_cast<State*>(state.data()));
                return Error::none;
            },
        .on_event =
            [](std::span<std::byte> storage, const FilterEvent& event,
                FilterOutputWriter& writer) {
                ++observed.calls;
                if (observed.behavior == Behavior::throws)
                    throw std::runtime_error("fixture");
                if (observed.behavior == Behavior::fails)
                    return Error::io_error;
                auto& state = *reinterpret_cast<State*>(storage.data());
                if (event.kind == FilterEventKind::environment_changed) {
                    REQUIRE(event.previous_environment != nullptr);
                    ++observed.changes;
                    observed.old_generation =
                        event.previous_environment->paths.front()
                            .member.generation;
                    observed.new_generation =
                        event.environment.paths.front().member.generation;
                    return Error::none;
                }
                if (event.kind == FilterEventKind::tick)
                    return Error::none;
                REQUIRE(event.input != nullptr);
                REQUIRE(event.input->payload.size() == state.parity.size());
                for (std::size_t index = 0; index < state.parity.size();
                    ++index)
                    state.parity[index] ^= event.input->payload[index];
                if (++state.inputs != 2)
                    return Error::none;
                FilterOutput output {.context = event.environment.context,
                    .domain = SequenceDomain::logical_stream,
                    .sequence = 100,
                    .destination = event.environment.paths.back().member,
                    .receiver = event.input->receiver,
                    .payload = state.parity};
                if (observed.behavior == Behavior::feedback)
                    output.kind = FilterOutputKind::feedback;
                if (observed.behavior == Behavior::stale_path)
                    output.destination.generation += 7;
                (void)writer.append(output);
                if (observed.behavior == Behavior::invalid_second) {
                    output.sequence = event.environment.end_sequence;
                    (void)writer.append(output);
                }
                if (observed.behavior == Behavior::overflow) {
                    (void)writer.append(output);
                    (void)writer.append(
                        output); // Deliberately ignore a capacity failure.
                }
                return Error::none;
            },
        .destroy =
            [](std::span<std::byte> state) noexcept {
                ++observed.destroys;
                std::destroy_at(reinterpret_cast<State*>(state.data()));
            }};
}

struct Fixture {
    std::array<PathBudget, 2> paths {{{{1, 1}, 4}, {{2, 1}, 8}}};
    std::array<ReceiverBudget, 2> receivers {{{{101, 1}, {}}, {{102, 1}, {}}}};
    FilterEnvironment environment {.context = {{1, 1}, {1, 1}, 1, 0},
        .paths = paths,
        .receivers = receivers,
        .first_sequence = 100,
        .end_sequence = 200,
        .maximum_recovered_payload_bytes = 8};
    FilterLimits limits {.maximum_members = 2,
        .maximum_receivers = 4,
        .maximum_state_bytes = 128,
        .maximum_outputs_per_batch = 2,
        .maximum_output_bytes_per_batch = 8};
    alignas(std::max_align_t) std::array<std::byte, 128> storage {};
    std::array<FilterOutput, 2> outputs {};
    std::array<std::byte, 8> bytes {};
    std::array<std::byte, 4> payload {
        std::byte {1}, std::byte {2}, std::byte {3}, std::byte {4}};
    FilterInstance instance; // Destroy before borrowed storage.

    Error start(FilterOwner owner = {.scope = FilterScope::logical_stream})
    {
        return instance.start(implementation(), limits, owner, environment,
            storage, outputs, bytes);
    }
    FilterInput input(
        std::uint64_t sequence = 100, Identity receiver = {}) const
    {
        return {.context = environment.context,
            .domain = SequenceDomain::logical_stream,
            .sequence = sequence,
            .origin = paths[0].member,
            .member_sequence = 900,
            .receiver = receiver,
            .payload = payload};
    }
};

} // namespace

TEST(
    extension_filter_dispatch_retains_shared_state_across_path_and_receiver_churn)
{
    observed = {};
    Fixture fixture;
    REQUIRE_EQ(fixture.start(), Error::none);
    REQUIRE_EQ(fixture.instance.process(fixture.input(), 1).error, Error::none);
    // The instance owns its membership snapshot, despite mutation of the source.
    fixture.paths[0].member.generation = 2;
    auto stale = fixture.input();
    REQUIRE_EQ(fixture.instance.process(stale, 2).error, Error::invalid_state);
    REQUIRE_EQ(observed.calls, 1U);
    fixture.environment.receivers = std::span {fixture.receivers}.first(1);
    REQUIRE_EQ(
        fixture.instance.update_environment(fixture.environment, 3).error,
        Error::none);
    REQUIRE_EQ(observed.old_generation, 1U);
    REQUIRE_EQ(observed.new_generation, 2U);
    REQUIRE_EQ(observed.constructs, 1U);
    REQUIRE_EQ(observed.changes, 1U);
    fixture.payload = {
        std::byte {9}, std::byte {8}, std::byte {7}, std::byte {6}};
    auto second = fixture.input(101);
    second.origin = fixture.paths[1].member;
    second.member_sequence = 700;
    const auto result = fixture.instance.process(second, 4);
    REQUIRE_EQ(result.error, Error::none);
    REQUIRE_EQ(result.outputs.size(), 1U);
    REQUIRE_EQ(result.outputs[0].destination, fixture.paths[1].member);
    REQUIRE_EQ(result.outputs[0].payload.data(), fixture.bytes.data());
    const std::array expected {
        std::byte {8}, std::byte {10}, std::byte {4}, std::byte {2}};
    REQUIRE(std::equal(
        expected.begin(), expected.end(), result.outputs[0].payload.begin()));
    fixture.payload.fill(std::byte {0});
    REQUIRE(std::equal(
        expected.begin(), expected.end(), result.outputs[0].payload.begin()));
    fixture.instance.close();
    fixture.instance.close();
    REQUIRE_EQ(observed.destroys, 1U);
    REQUIRE(
        std::all_of(fixture.storage.begin(), fixture.storage.end(), [](auto b) {
            return b == std::byte {};
        }));
    REQUIRE(std::all_of(fixture.bytes.begin(), fixture.bytes.end(), [](auto b) {
        return b == std::byte {};
    }));
}

TEST(extension_filter_dispatch_receiver_owned_instances_remain_independent)
{
    observed = {};
    Fixture a;
    Fixture b;
    const Identity receiver_a {101, 1}, receiver_b {102, 1};
    REQUIRE_EQ(
        a.start({.scope = FilterScope::logical_stream, .receiver = receiver_a}),
        Error::none);
    REQUIRE_EQ(
        b.start({.scope = FilterScope::logical_stream, .receiver = receiver_b}),
        Error::none);
    REQUIRE_EQ(
        a.instance.process(a.input(100, receiver_a), 1).error, Error::none);
    REQUIRE_EQ(
        b.instance.process(b.input(100, receiver_b), 1).error, Error::none);
    a.environment.receivers = std::span {a.receivers}.first(1);
    REQUIRE_EQ(
        a.instance.update_environment(a.environment, 2).error, Error::none);
    REQUIRE_EQ(a.instance.process(a.input(101, receiver_b), 3).error,
        Error::invalid_state);
    const auto result_a = a.instance.process(a.input(101, receiver_a), 4);
    REQUIRE_EQ(result_a.error, Error::none);
    REQUIRE_EQ(result_a.outputs[0].receiver, receiver_a);
    const auto result_b = b.instance.process(b.input(101, receiver_b), 4);
    REQUIRE_EQ(result_b.error, Error::none);
    REQUIRE_EQ(result_b.outputs[0].receiver, receiver_b);
}

TEST(extension_filter_dispatch_optional_feedback_keeps_silent_receivers_active)
{
    observed = {};
    Fixture fixture;
    fixture.environment.feedback_available = true;
    auto selected = implementation();
    selected.descriptor.feedback = FeedbackRequirement::optional;
    selected.on_event = [](std::span<std::byte> storage,
                            const FilterEvent& event,
                            FilterOutputWriter& writer) {
        auto& state = *reinterpret_cast<State*>(storage.data());
        if (event.kind == FilterEventKind::input) {
            if (event.input->kind == FilterInputKind::source)
                std::copy(event.input->payload.begin(),
                    event.input->payload.end(), state.parity.begin());
            else if (event.input->kind == FilterInputKind::feedback) {
                REQUIRE_EQ(event.input->receiver, (Identity {102, 1}));
                ++state.inputs;
            }
        } else if (event.kind == FilterEventKind::tick) {
            for (const auto& receiver : event.environment.receivers) {
                const auto error =
                    writer.append({.context = event.environment.context,
                        .domain = SequenceDomain::logical_stream,
                        .sequence = event.environment.first_sequence,
                        .receiver = receiver.receiver,
                        .payload = state.parity});
                if (error != Error::none)
                    return error;
            }
        }
        return Error::none;
    };
    REQUIRE_EQ(fixture.instance.start(selected, fixture.limits,
                   {.scope = FilterScope::logical_stream}, fixture.environment,
                   fixture.storage, fixture.outputs, fixture.bytes),
        Error::none);
    REQUIRE_EQ(fixture.instance.process(fixture.input(), 1).error, Error::none);
    auto feedback = fixture.input(100, {102, 1});
    feedback.kind = FilterInputKind::feedback;
    REQUIRE_EQ(fixture.instance.process(feedback, 2).error, Error::none);
    const auto both = fixture.instance.poll(3);
    REQUIRE_EQ(both.error, Error::none);
    REQUIRE_EQ(both.outputs.size(), 2U);
    REQUIRE_EQ(both.outputs[0].receiver, (Identity {101, 1}));
    REQUIRE_EQ(both.outputs[1].receiver, (Identity {102, 1}));
    // The only feedback participant leaves. Shared source state and repair
    // for the silent receiver continue without reconstruction or a new instance.
    fixture.environment.receivers = std::span {fixture.receivers}.first(1);
    fixture.environment.feedback_available = false;
    REQUIRE_EQ(
        fixture.instance.update_environment(fixture.environment, 4).error,
        Error::none);
    REQUIRE_EQ(
        fixture.instance.process(feedback, 5).error, Error::invalid_state);
    const auto silent = fixture.instance.poll(6);
    REQUIRE_EQ(silent.error, Error::none);
    REQUIRE_EQ(silent.outputs.size(), 1U);
    REQUIRE_EQ(silent.outputs[0].receiver, (Identity {101, 1}));
    REQUIRE(std::equal(fixture.payload.begin(), fixture.payload.end(),
        silent.outputs[0].payload.begin(), silent.outputs[0].payload.end()));
    REQUIRE_EQ(observed.constructs, 1U);
}

TEST(
    extension_filter_dispatch_invalid_output_rejects_the_complete_batch_and_is_terminal)
{
    for (const auto behavior : {Behavior::invalid_second, Behavior::overflow,
             Behavior::feedback, Behavior::stale_path}) {
        observed = {.behavior = behavior};
        Fixture fixture;
        REQUIRE_EQ(fixture.start(), Error::none);
        REQUIRE_EQ(
            fixture.instance.process(fixture.input(), 1).error, Error::none);
        const auto result = fixture.instance.process(fixture.input(101), 2);
        REQUIRE_EQ(result.error, Error::invalid_state);
        REQUIRE(result.outputs.empty());
        REQUIRE(fixture.outputs[0].payload.empty());
        REQUIRE(
            std::all_of(fixture.bytes.begin(), fixture.bytes.end(), [](auto b) {
                return b == std::byte {};
            }));
        const auto calls = observed.calls;
        REQUIRE_EQ(fixture.instance.poll(3).error, Error::invalid_state);
        REQUIRE_EQ(observed.calls, calls);
    }
}

TEST(extension_filter_dispatch_contains_callback_errors_and_backwards_time)
{
    for (const auto behavior :
        {Behavior::throws, Behavior::fails, Behavior::normal}) {
        observed = {.behavior = behavior};
        Fixture fixture;
        REQUIRE_EQ(fixture.start(), Error::none);
        auto result = fixture.instance.process(fixture.input(), 10);
        if (behavior == Behavior::normal) {
            REQUIRE_EQ(result.error, Error::none);
            result = fixture.instance.poll(9);
        }
        REQUIRE(result.error != Error::none);
        REQUIRE(result.outputs.empty());
        const auto calls = observed.calls;
        REQUIRE(fixture.instance.poll(11).error != Error::none);
        REQUIRE_EQ(observed.calls, calls);
        fixture.instance.close();
        REQUIRE_EQ(observed.destroys, 1U);
    }
}

TEST(
    extension_filter_dispatch_rejects_epoch_changes_without_reinterpreting_active_state)
{
    observed = {};
    Fixture fixture;
    REQUIRE_EQ(fixture.start(), Error::none);
    ++fixture.environment.context.configuration_generation;
    REQUIRE_EQ(
        fixture.instance.update_environment(fixture.environment, 1).error,
        Error::invalid_state);
    REQUIRE_EQ(observed.calls, 0U);
    REQUIRE_EQ(fixture.instance.poll(2).error, Error::none);
    REQUIRE_EQ(observed.constructs, 1U);
}

TEST(
    extension_filter_dispatch_reserves_receiver_capacity_and_rejects_invalid_storage)
{
    for (unsigned invalid = 0; invalid != 7; ++invalid) {
        observed = {};
        Fixture fixture;
        auto selected = implementation();
        std::span<std::byte> state = fixture.storage;
        std::span<std::byte> bytes = fixture.bytes;
        switch (invalid) {
        case 0:
            fixture.limits.maximum_receivers = 100;
            break;
        case 1:
            selected.state_alignment = 3;
            break;
        case 2:
            selected.interface_version = 2;
            break;
        case 3:
            selected.destroy = nullptr;
            break;
        case 4:
            bytes = bytes.first(7);
            break;
        case 5:
            fixture.limits.maximum_members = maximum_filter_members + 1;
            break;
        case 6:
            bytes = state.first(8);
            break;
        }
        REQUIRE_EQ(fixture.instance.start(selected, fixture.limits,
                       {.scope = FilterScope::logical_stream},
                       fixture.environment, state, fixture.outputs, bytes),
            Error::invalid_state);
        REQUIRE_EQ(observed.constructs, 0U);
        REQUIRE_EQ(observed.destroys, 0U);
    }
    observed = {.fail_construct = true};
    Fixture fixture;
    REQUIRE_EQ(fixture.start(), Error::invalid_state);
    REQUIRE_EQ(fixture.start(), Error::invalid_state);
    fixture.instance.close();
    REQUIRE_EQ(observed.constructs, 1U);
    REQUIRE_EQ(observed.destroys, 0U);
}
