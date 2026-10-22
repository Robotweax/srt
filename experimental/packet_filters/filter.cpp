#include "filter.hpp"

#include <algorithm>

namespace robotweax::srt::extensions {
namespace {

const ReceiverBudget* active_receiver(
    Identity identity, const FilterEnvironment& environment) noexcept
{
    for (const auto& receiver : environment.receivers) {
        if (identity.valid() && receiver.receiver == identity)
            return &receiver;
    }
    return nullptr;
}

const PathBudget* active_path(
    Identity identity, const FilterEnvironment& environment) noexcept
{
    for (const auto& path : environment.paths) {
        if (path.member == identity && identity.valid())
            return &path;
    }
    return nullptr;
}

bool absent(Identity identity) noexcept
{
    return identity == Identity {};
}

bool protection_matches(
    const PathBudget& path, const FilterContext& context) noexcept
{
    return context.payload_domain == FilterPayloadDomain::logical_payload
        || path.protection == context.protection;
}

} // namespace

static Error validate_binding(const FilterDescriptor& descriptor,
    FilterLimits limits, const FilterOwner& owner,
    const FilterEnvironment& environment) noexcept
{
    if (!environment.context.session.valid()
        || !environment.context.stream.valid()
        || environment.context.configuration_generation == 0
        || environment.context.direction > 1
        || static_cast<unsigned>(environment.context.payload_domain)
            > static_cast<unsigned>(FilterPayloadDomain::protected_symbols)
        || (environment.context.payload_domain
                    == FilterPayloadDomain::protected_symbols
                ? !environment.context.protection.valid()
                : !absent(environment.context.protection))
        || environment.paths.size() > limits.maximum_members
        || environment.receivers.size() > limits.maximum_receivers
        || environment.first_sequence >= environment.end_sequence
        || environment.maximum_recovered_payload_bytes == 0
        || static_cast<unsigned>(descriptor.scope)
            > static_cast<unsigned>(FilterScope::logical_stream)
        || static_cast<unsigned>(descriptor.feedback)
            > static_cast<unsigned>(FeedbackRequirement::required)
        || descriptor.scope != owner.scope
        || (descriptor.feedback == FeedbackRequirement::required
            && !environment.feedback_available)
        || descriptor.maximum_outputs_per_batch == 0
        || descriptor.maximum_outputs_per_batch
            > limits.maximum_outputs_per_batch
        || descriptor.maximum_output_bytes_per_batch == 0
        || descriptor.maximum_output_bytes_per_batch
            > limits.maximum_output_bytes_per_batch
        || descriptor.state_bytes > limits.maximum_state_bytes)
        return Error::invalid_state;

    if (descriptor.state_bytes_per_receiver != 0
        && environment.receivers.size()
            > (limits.maximum_state_bytes - descriptor.state_bytes)
                / descriptor.state_bytes_per_receiver)
        return Error::invalid_state;
    if ((owner.scope == FilterScope::member_transport
            && active_path(owner.member, environment) == nullptr)
        || (owner.scope == FilterScope::logical_stream && !absent(owner.member))
        || (!absent(owner.receiver)
            && !active_receiver(owner.receiver, environment)))
        return Error::invalid_state;
    return Error::none;
}

Error admit_filter(const FilterDescriptor& descriptor, FilterLimits limits,
    const FilterOwner& owner, const FilterEnvironment& environment) noexcept
{
    if (validate_binding(descriptor, limits, owner, environment) != Error::none)
        return Error::invalid_state;
    for (std::size_t index = 0; index < environment.paths.size(); ++index) {
        const auto& path = environment.paths[index];
        if (!path.member.valid() || path.maximum_payload_bytes == 0)
            return Error::invalid_state;
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (environment.paths[previous].member.value == path.member.value)
                return Error::invalid_state;
        }
    }
    for (std::size_t index = 0; index < environment.receivers.size(); ++index) {
        if (!environment.receivers[index].receiver.valid())
            return Error::invalid_state;
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (environment.receivers[previous].receiver.value
                == environment.receivers[index].receiver.value)
                return Error::invalid_state;
        }
    }
    return Error::none;
}

Error validate_filter_input(const FilterDescriptor& descriptor,
    FilterLimits limits, const FilterOwner& owner,
    const FilterEnvironment& environment, const FilterInput& input) noexcept
{
    const auto domain = owner.scope == FilterScope::logical_stream
        ? SequenceDomain::logical_stream
        : SequenceDomain::member_transport;
    if (validate_binding(descriptor, limits, owner, environment) != Error::none
        || input.context != environment.context || input.domain != domain
        || static_cast<unsigned>(input.kind)
            > static_cast<unsigned>(FilterInputKind::feedback)
        || (input.kind == FilterInputKind::feedback
            && (!environment.feedback_available
                || descriptor.feedback == FeedbackRequirement::none))
        || input.sequence < environment.first_sequence
        || input.sequence >= environment.end_sequence || input.payload.empty()
        || input.payload.size() > environment.maximum_recovered_payload_bytes
        || (!absent(input.origin)
            && active_path(input.origin, environment) == nullptr)
        || (owner.scope == FilterScope::member_transport
            && input.origin != owner.member)
        || (!absent(input.receiver)
            && !active_receiver(input.receiver, environment))
        || (!absent(owner.receiver) && input.receiver != owner.receiver))
        return Error::invalid_state;
    if (!absent(input.origin)
        && !protection_matches(
            *active_path(input.origin, environment), input.context))
        return Error::invalid_state;
    if (!absent(input.receiver)
        && input.context.payload_domain
            == FilterPayloadDomain::protected_symbols
        && active_receiver(input.receiver, environment)->protection
            != input.context.protection)
        return Error::invalid_state;
    return Error::none;
}

Error validate_filter_outputs(const FilterDescriptor& descriptor,
    FilterLimits limits, const FilterOwner& owner,
    const FilterEnvironment& environment,
    std::span<const FilterOutput> outputs) noexcept
{
    if (validate_binding(descriptor, limits, owner, environment) != Error::none
        || outputs.size() > descriptor.maximum_outputs_per_batch)
        return Error::invalid_state;
    std::size_t remaining_bytes = descriptor.maximum_output_bytes_per_batch;
    const auto domain = owner.scope == FilterScope::logical_stream
        ? SequenceDomain::logical_stream
        : SequenceDomain::member_transport;
    for (const auto& output : outputs) {
        if (output.context != environment.context || output.domain != domain
            || static_cast<unsigned>(output.kind)
                > static_cast<unsigned>(FilterOutputKind::feedback)
            || output.sequence < environment.first_sequence
            || output.sequence >= environment.end_sequence
            || output.payload.empty() || output.payload.size() > remaining_bytes
            || (!absent(output.receiver)
                && !active_receiver(output.receiver, environment))
            || (!absent(owner.receiver) && output.receiver != owner.receiver)
            || (output.kind == FilterOutputKind::feedback
                && (!environment.feedback_available
                    || descriptor.feedback == FeedbackRequirement::none)))
            return Error::invalid_state;
        if (!absent(output.receiver)
            && output.context.payload_domain
                == FilterPayloadDomain::protected_symbols
            && active_receiver(output.receiver, environment)->protection
                != output.context.protection)
            return Error::invalid_state;
        if (!absent(output.destination)) {
            const auto* path = active_path(output.destination, environment);
            if (path == nullptr || !protection_matches(*path, output.context)
                || output.payload.size() > path->maximum_payload_bytes
                || (owner.scope == FilterScope::member_transport
                    && output.destination != owner.member))
                return Error::invalid_state;
        } else if (output.kind != FilterOutputKind::recovered_data) {
            const bool fits = std::any_of(environment.paths.begin(),
                environment.paths.end(), [&](const auto& path) {
                    return output.payload.size() <= path.maximum_payload_bytes
                        && protection_matches(path, output.context)
                        && (owner.scope == FilterScope::logical_stream
                            || path.member == owner.member);
                });
            if (!fits)
                return Error::invalid_state;
        }
        if (output.kind == FilterOutputKind::recovered_data
            && (!absent(output.destination)
                || output.payload.size()
                    > environment.maximum_recovered_payload_bytes))
            return Error::invalid_state;
        remaining_bytes -= output.payload.size();
    }
    return Error::none;
}

bool filter_payload_budget(std::size_t transport_payload,
    std::size_t group_envelope, std::size_t crypto_overhead,
    std::span<const std::size_t> filter_overheads, std::size_t& result) noexcept
{
    result = 0;
    if (group_envelope >= transport_payload)
        return false;
    std::size_t remaining = transport_payload - group_envelope;
    if (crypto_overhead >= remaining)
        return false;
    remaining -= crypto_overhead;
    for (const auto overhead : filter_overheads) {
        if (overhead >= remaining)
            return false;
        remaining -= overhead;
    }
    result = remaining;
    return true;
}

} // namespace robotweax::srt::extensions
