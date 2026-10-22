#include "filter.hpp"
#include "test.hpp"

#include <array>
#include <limits>

using namespace robotweax::srt;
using namespace robotweax::srt::extensions;

namespace {

constexpr Identity path_a {1, 1};
constexpr Identity path_b {2, 1};
constexpr Identity receiver_a {101, 1};
constexpr Identity receiver_b {102, 1};

struct Fixture {
    std::array<PathBudget, 2> paths {{{path_a, 8}, {path_b, 16}}};
    std::array<ReceiverBudget, 2> receivers {
        {{receiver_a, {}}, {receiver_b, {}}}};
    FilterEnvironment environment {
        .context = {{1, 1}, {1, 1}, 1, 0},
        .paths = paths,
        .receivers = receivers,
        .first_sequence = 100,
        .end_sequence = 200,
        .maximum_recovered_payload_bytes = 16,
    };
    FilterOwner owner {.scope = FilterScope::logical_stream};
    FilterDescriptor descriptor {
        .scope = FilterScope::logical_stream,
        .state_bytes = 8,
        .state_bytes_per_receiver = 4,
        .maximum_outputs_per_batch = 4,
        .maximum_output_bytes_per_batch = 64,
    };
    FilterLimits limits {
        .maximum_members = 8,
        .maximum_receivers = 4,
        .maximum_state_bytes = 64,
        .maximum_outputs_per_batch = 4,
        .maximum_output_bytes_per_batch = 64,
    };
    std::array<std::byte, 4> payload {};

    FilterOutput output() const
    {
        return {.context = environment.context,
            .domain = SequenceDomain::logical_stream,
            .sequence = 100,
            .destination = path_b,
            .payload = payload};
    }

    Error validate(const FilterOutput& output) const
    {
        const auto admission =
            admit_filter(descriptor, limits, owner, environment);
        if (admission != Error::none)
            return admission;
        return validate_filter_outputs(
            descriptor, limits, owner, environment, std::span {&output, 1});
    }
};

} // namespace

TEST(
    extension_filter_admission_rejects_ambiguous_membership_and_invalid_contexts)
{
    for (int invalid = 0; invalid < 9; ++invalid) {
        Fixture fixture;
        switch (invalid) {
        case 0:
            fixture.paths[1].member = fixture.paths[0].member;
            break;
        case 1:
            fixture.receivers[1].receiver = fixture.receivers[0].receiver;
            break;
        case 2:
            fixture.environment.context.session.generation = 0;
            break;
        case 3:
            fixture.environment.context.direction = 2;
            break;
        case 4:
            fixture.environment.end_sequence =
                fixture.environment.first_sequence;
            break;
        case 5:
            fixture.paths[0].maximum_payload_bytes = 0;
            break;
        case 6:
            fixture.descriptor.scope = static_cast<FilterScope>(255);
            break;
        case 7:
            fixture.owner.member = path_a;
            break;
        case 8:
            fixture.descriptor.state_bytes = 65;
            break;
        }
        REQUIRE_EQ(admit_filter(fixture.descriptor, fixture.limits,
                       fixture.owner, fixture.environment),
            Error::invalid_state);
    }
}

TEST(extension_filter_inputs_reject_stale_provenance_and_receiver_domains)
{
    Fixture fixture;
    FilterInput input {.context = fixture.environment.context,
        .domain = SequenceDomain::logical_stream,
        .sequence = 100,
        .origin = path_a,
        .member_sequence = 900,
        .payload = fixture.payload};
    REQUIRE_EQ(validate_filter_input(fixture.descriptor, fixture.limits,
                   fixture.owner, fixture.environment, input),
        Error::none);
    ++input.origin.generation;
    REQUIRE_EQ(validate_filter_input(fixture.descriptor, fixture.limits,
                   fixture.owner, fixture.environment, input),
        Error::invalid_state);
    // Group filtering can also consume an application symbol before routing.
    input.origin = {};
    input.member_sequence = 0;
    REQUIRE_EQ(validate_filter_input(fixture.descriptor, fixture.limits,
                   fixture.owner, fixture.environment, input),
        Error::none);
    fixture.owner.receiver = receiver_a;
    input.receiver = receiver_b;
    REQUIRE_EQ(validate_filter_input(fixture.descriptor, fixture.limits,
                   fixture.owner, fixture.environment, input),
        Error::invalid_state);
}

TEST(extension_filter_member_recovery_remains_scoped_to_its_path)
{
    for (const auto member : {path_a, path_b}) {
        Fixture fixture;
        fixture.owner = {
            .scope = FilterScope::member_transport, .member = member};
        fixture.descriptor.scope = FilterScope::member_transport;
        fixture.environment.first_sequence = 900;
        fixture.environment.end_sequence = 910;
        auto output = fixture.output();
        output.domain = SequenceDomain::member_transport;
        output.sequence = 901;
        output.destination = member;
        REQUIRE_EQ(fixture.validate(output), Error::none);
        output.destination = member == path_a ? path_b : path_a;
        REQUIRE_EQ(fixture.validate(output), Error::invalid_state);
        output.destination = {};
        output.kind = FilterOutputKind::recovered_data;
        REQUIRE_EQ(fixture.validate(output), Error::none);
        fixture.paths[member == path_a ? 0 : 1].member.generation = 2;
        REQUIRE_EQ(fixture.validate(output), Error::invalid_state);
    }
}

TEST(
    extension_filter_logical_xor_recovery_crosses_paths_with_independent_sequences)
{
    Fixture fixture;
    REQUIRE_EQ(admit_filter(fixture.descriptor, fixture.limits, fixture.owner,
                   fixture.environment),
        Error::none);
    const std::array<std::byte, 4> first {
        std::byte {1}, std::byte {2}, std::byte {3}, std::byte {4}};
    const std::array<std::byte, 4> second {
        std::byte {9}, std::byte {8}, std::byte {7}, std::byte {6}};
    // Fake coordinator maps member A's packet 900 and B's packet 700 to
    // logical messages 100/101. The filter sees only logical source symbols.
    const std::array inputs {
        FilterInput {.context = fixture.environment.context,
            .domain = SequenceDomain::logical_stream,
            .sequence = 101,
            .origin = path_b,
            .member_sequence = 700,
            .payload = second},
        FilterInput {.context = fixture.environment.context,
            .domain = SequenceDomain::logical_stream,
            .sequence = 100,
            .origin = path_a,
            .member_sequence = 900,
            .payload = first}};
    std::array<std::byte, 4> parity {};
    for (const auto& input : inputs) {
        REQUIRE_EQ(validate_filter_input(fixture.descriptor, fixture.limits,
                       fixture.owner, fixture.environment, input),
            Error::none);
        for (std::size_t index = 0; index < parity.size(); ++index)
            parity[index] ^= input.payload[index];
    }
    auto repair = fixture.output();
    repair.payload = parity;
    repair.destination = path_b;
    REQUIRE_EQ(fixture.validate(repair), Error::none);
    std::array<std::byte, 4> recovered {};
    for (std::size_t index = 0; index < recovered.size(); ++index)
        recovered[index] = parity[index] ^ second[index];
    REQUIRE_EQ(recovered, first);
    auto delivery = fixture.output();
    delivery.kind = FilterOutputKind::recovered_data;
    delivery.destination = {};
    delivery.payload = recovered;
    REQUIRE_EQ(fixture.validate(delivery), Error::none);
    delivery.domain = SequenceDomain::member_transport;
    REQUIRE_EQ(fixture.validate(delivery), Error::invalid_state);
}

TEST(extension_filter_group_recovery_survives_member_churn_and_zero_live_paths)
{
    Fixture fixture;
    auto output = fixture.output();
    fixture.paths[0].member.generation = 2;
    REQUIRE_EQ(fixture.validate(output), Error::none);
    output.destination = path_a;
    REQUIRE_EQ(fixture.validate(output), Error::invalid_state);
    output.kind = FilterOutputKind::recovered_data;
    output.destination = {};
    fixture.environment.paths = {};
    REQUIRE_EQ(fixture.validate(output), Error::none);
    output.kind = FilterOutputKind::repair;
    REQUIRE_EQ(fixture.validate(output), Error::invalid_state);
}

TEST(extension_filter_multicast_receivers_join_leave_and_recover_independently)
{
    Fixture fixture;
    auto output = fixture.output();
    output.receiver = receiver_a;
    REQUIRE_EQ(fixture.validate(output), Error::none);
    fixture.environment.receivers = std::span {fixture.receivers}.first(1);
    REQUIRE_EQ(fixture.validate(output), Error::none);
    output.receiver = receiver_b;
    REQUIRE_EQ(fixture.validate(output), Error::invalid_state);
    fixture.receivers[1].receiver.generation = 2;
    fixture.environment.receivers = fixture.receivers;
    REQUIRE_EQ(fixture.validate(output), Error::invalid_state);
    output.receiver = fixture.receivers[1].receiver;
    REQUIRE_EQ(fixture.validate(output), Error::none);
    // Independent decoder ownership rejects an output aimed at another peer.
    fixture.owner.receiver = receiver_a;
    REQUIRE_EQ(fixture.validate(output), Error::invalid_state);
    output.receiver = receiver_a;
    REQUIRE_EQ(fixture.validate(output), Error::none);
    fixture.receivers[0].receiver.generation = 2;
    REQUIRE_EQ(fixture.validate(output), Error::invalid_state);
}

TEST(extension_filter_feedback_free_and_feedback_required_profiles_are_distinct)
{
    Fixture fixture;
    fixture.descriptor.feedback = FeedbackRequirement::none;
    auto output = fixture.output();
    REQUIRE_EQ(fixture.validate(output), Error::none);
    output.kind = FilterOutputKind::feedback;
    REQUIRE_EQ(fixture.validate(output), Error::invalid_state);
    fixture.environment.feedback_available = true;
    REQUIRE_EQ(fixture.validate(output), Error::invalid_state);
    fixture.descriptor.feedback = FeedbackRequirement::required;
    REQUIRE_EQ(fixture.validate(output), Error::none);
    fixture.environment.feedback_available = false;
    REQUIRE_EQ(fixture.validate(output), Error::invalid_state);
}

TEST(extension_filter_rejects_stale_contexts_and_invalid_sequence_windows)
{
    for (int stale = 0; stale < 7; ++stale) {
        Fixture fixture;
        auto output = fixture.output();
        switch (stale) {
        case 0:
            ++output.context.session.generation;
            break;
        case 1:
            ++output.context.stream.value;
            break;
        case 2:
            ++output.context.configuration_generation;
            break;
        case 3:
            output.context.direction = 1;
            break;
        case 4:
            output.sequence = 99;
            break;
        case 5:
            output.sequence = 200;
            break;
        case 6:
            output.kind = static_cast<FilterOutputKind>(255);
            break;
        }
        REQUIRE_EQ(fixture.validate(output), Error::invalid_state);
    }
}

TEST(extension_filter_batches_and_receiver_memory_are_bounded_without_overflow)
{
    Fixture fixture;
    const std::array outputs {fixture.output(), fixture.output()};
    fixture.descriptor.maximum_output_bytes_per_batch = 8;
    REQUIRE_EQ(validate_filter_outputs(fixture.descriptor, fixture.limits,
                   fixture.owner, fixture.environment, outputs),
        Error::none);
    fixture.descriptor.maximum_output_bytes_per_batch = 7;
    REQUIRE_EQ(validate_filter_outputs(fixture.descriptor, fixture.limits,
                   fixture.owner, fixture.environment, outputs),
        Error::invalid_state);
    fixture.descriptor.maximum_outputs_per_batch = 1;
    REQUIRE_EQ(validate_filter_outputs(fixture.descriptor, fixture.limits,
                   fixture.owner, fixture.environment, outputs),
        Error::invalid_state);
    fixture.descriptor.maximum_outputs_per_batch = 4;
    fixture.descriptor.state_bytes_per_receiver =
        std::numeric_limits<std::size_t>::max();
    REQUIRE_EQ(fixture.validate(fixture.output()), Error::invalid_state);
    fixture.descriptor.state_bytes_per_receiver = 4;
    fixture.limits.maximum_receivers = 1;
    REQUIRE_EQ(fixture.validate(fixture.output()), Error::invalid_state);
}

TEST(extension_filter_protection_context_cannot_cross_member_or_receiver_keys)
{
    Fixture fixture;
    auto stale_plaintext = fixture.output();
    fixture.environment.context.payload_domain =
        FilterPayloadDomain::protected_symbols;
    fixture.environment.context.protection = {77, 1};
    fixture.paths[0].protection = {77, 1};
    fixture.paths[1].protection = {88, 1};
    fixture.receivers[0].protection = {77, 1};
    fixture.receivers[1].protection = {88, 1};
    REQUIRE_EQ(fixture.validate(stale_plaintext), Error::invalid_state);
    auto output = fixture.output();
    REQUIRE_EQ(fixture.validate(output), Error::invalid_state);
    output.destination = path_a;
    output.receiver = receiver_a;
    REQUIRE_EQ(fixture.validate(output), Error::none);
    output.receiver = receiver_b;
    REQUIRE_EQ(fixture.validate(output), Error::invalid_state);
    output.receiver = receiver_a;
    output.context.protection.generation = 2;
    REQUIRE_EQ(fixture.validate(output), Error::invalid_state);
    // A core-admitted shared protection context can span several paths/peers.
    fixture.paths[1].protection = {77, 1};
    fixture.receivers[1].protection = {77, 1};
    output = fixture.output();
    output.receiver = receiver_b;
    REQUIRE_EQ(fixture.validate(output), Error::none);
}

TEST(
    extension_filter_unequal_paths_and_composed_overheads_keep_distinct_budgets)
{
    Fixture fixture;
    std::array<std::byte, 12> payload {};
    auto output = fixture.output();
    output.payload = payload;
    REQUIRE_EQ(fixture.validate(output), Error::none);
    output.destination = path_a;
    REQUIRE_EQ(fixture.validate(output), Error::invalid_state);
    output.destination = {};
    REQUIRE_EQ(fixture.validate(output), Error::none);
    const std::array<std::size_t, 2> overheads {4, 8};
    std::size_t budget = 0;
    REQUIRE(filter_payload_budget(1'456, 32, 16, overheads, budget));
    REQUIRE_EQ(budget, 1'396U);
    REQUIRE(filter_payload_budget(1'200, 32, 16, overheads, budget));
    REQUIRE_EQ(budget, 1'140U);
    const std::array excessive {std::numeric_limits<std::size_t>::max()};
    REQUIRE(!filter_payload_budget(1'456, 32, 16, excessive, budget));
    REQUIRE_EQ(budget, 0U);
}
