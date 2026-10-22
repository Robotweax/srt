#pragma once

#include "robotweax/srt/error.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace robotweax::srt::extensions {

struct Identity {
    std::uint64_t value = 0;
    std::uint64_t generation = 0;
    [[nodiscard]] constexpr bool valid() const noexcept
    {
        return value != 0 && generation != 0;
    }
    friend constexpr bool operator==(Identity, Identity) = default;
};

enum class FilterScope : std::uint8_t { member_transport, logical_stream };
enum class SequenceDomain : std::uint8_t { member_transport, logical_stream };
enum class FeedbackRequirement : std::uint8_t { none, optional, required };
enum class FilterOutputKind : std::uint8_t { repair, recovered_data, feedback };
enum class FilterInputKind : std::uint8_t { source, repair, feedback };
enum class FilterPayloadDomain : std::uint8_t {
    logical_payload,
    protected_symbols
};

struct FilterContext {
    Identity session {};
    Identity stream {};
    std::uint64_t configuration_generation = 0;
    std::uint8_t direction = 0;
    FilterPayloadDomain payload_domain = FilterPayloadDomain::logical_payload;
    // Opaque core-issued protection/key-epoch identity; contains no key bytes.
    Identity protection {};
    friend constexpr bool operator==(FilterContext, FilterContext) = default;
};

// Receiver ownership is independent of processing scope. A zero identity means
// shared source state; receiver-specific state names an admitted generation.
struct FilterOwner {
    FilterScope scope = FilterScope::member_transport;
    Identity member {};
    Identity receiver {};
};

struct FilterDescriptor {
    FilterScope scope = FilterScope::member_transport;
    FeedbackRequirement feedback = FeedbackRequirement::optional;
    std::size_t state_bytes = 0;
    std::size_t state_bytes_per_receiver = 0;
    std::size_t maximum_outputs_per_batch = 0;
    std::size_t maximum_output_bytes_per_batch = 0;
};

struct PathBudget {
    Identity member {};
    // Computed by the core after envelope, crypto and filter-stack overhead.
    std::size_t maximum_payload_bytes = 0;
    Identity protection {};
};

struct FilterLimits {
    std::size_t maximum_members = 64;
    std::size_t maximum_receivers = 256;
    std::size_t maximum_state_bytes = 0;
    std::size_t maximum_outputs_per_batch = 0;
    std::size_t maximum_output_bytes_per_batch = 0;
};

struct ReceiverBudget {
    Identity receiver {};
    Identity protection {};
};

// The owner supplies a current bounded snapshot. Views are borrowed only for
// validation; the boundary keeps no member/socket/receiver pointers.
struct FilterEnvironment {
    FilterContext context {};
    std::span<const PathBudget> paths {};
    std::span<const ReceiverBudget> receivers {};
    bool feedback_available = false;
    // Positions are lifted into the owner's epoch; core handles wire rollover.
    std::uint64_t first_sequence = 0;
    std::uint64_t end_sequence = 0;
    std::size_t maximum_recovered_payload_bytes = 0;
};

struct FilterOutput {
    FilterContext context {};
    FilterOutputKind kind = FilterOutputKind::repair;
    SequenceDomain domain = SequenceDomain::member_transport;
    std::uint64_t sequence = 0;
    // Zero leaves repair routing to the coordinator. A concrete destination
    // constrains the route; logical repair can target a different input path.
    Identity destination {};
    Identity receiver {};
    std::span<const std::byte> payload {};
};

struct FilterInput {
    FilterContext context {};
    SequenceDomain domain = SequenceDomain::member_transport;
    std::uint64_t sequence = 0;
    // Optional transport provenance is not the logical identity or owner.
    // No origin represents an application symbol before path assignment.
    Identity origin {};
    std::uint64_t member_sequence = 0;
    Identity receiver {};
    std::span<const std::byte> payload {};
    FilterInputKind kind = FilterInputKind::source;
};

[[nodiscard]] Error admit_filter(const FilterDescriptor& descriptor,
    FilterLimits limits, const FilterOwner& owner,
    const FilterEnvironment& environment) noexcept;

// Admit each immutable membership snapshot before use. Per-input/output checks
// revalidate context, ownership and budgets without rescanning all receivers.
[[nodiscard]] Error validate_filter_input(const FilterDescriptor& descriptor,
    FilterLimits limits, const FilterOwner& owner,
    const FilterEnvironment& environment, const FilterInput& input) noexcept;

// Validates a complete batch before the owner commits any output. Caller-owned
// core storage is required when outputs must survive the callback. Authentication,
// logical receipt/release, deduplication and scheduling remain core decisions.
[[nodiscard]] Error validate_filter_outputs(const FilterDescriptor& descriptor,
    FilterLimits limits, const FilterOwner& owner,
    const FilterEnvironment& environment,
    std::span<const FilterOutput> outputs) noexcept;

// Returns false on overflow, an empty path budget or insufficient capacity.
[[nodiscard]] bool filter_payload_budget(std::size_t transport_payload,
    std::size_t group_envelope, std::size_t crypto_overhead,
    std::span<const std::size_t> filter_overheads,
    std::size_t& result) noexcept;

} // namespace robotweax::srt::extensions
