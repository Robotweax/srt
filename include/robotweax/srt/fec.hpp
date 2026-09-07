#pragma once

#include "robotweax/srt/packet_filter.hpp"
#include "robotweax/srt/send_buffer.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace robotweax::srt {

// Describes every dynamically allocated byte owned by an FEC decoder. The
// estimate is derived exclusively from the locally configured receive window,
// payload ceiling, and negotiated geometry. Remote control packets cannot
// grow these stores after construction.
struct FecResourceUsage {
    std::size_t group_slots = 0;
    std::size_t group_metadata_bytes = 0;
    std::size_t recovery_payload_bytes = 0;
    std::size_t source_tracking_bytes = 0;
    std::size_t loss_tracking_bytes = 0;
    std::size_t reconstruction_bytes = 0;
    std::size_t dynamic_bytes = 0;

    [[nodiscard]] constexpr bool operator==(
        const FecResourceUsage&) const noexcept = default;
};

struct FecResourceEstimate {
    Error error = Error::none;
    FecResourceUsage usage {};

    [[nodiscard]] constexpr explicit operator bool() const noexcept
    {
        return error == Error::none;
    }
};

[[nodiscard]] constexpr bool supports_row_only_fec(
    const PacketFilterConfiguration& configuration) noexcept
{
    return configuration.enabled
        && configuration.columns >= 2U
        && configuration.rows == 1;
}

[[nodiscard]] constexpr bool supports_column_only_fec(
    const PacketFilterConfiguration& configuration) noexcept
{
    if (!configuration.enabled
        || configuration.columns < 2U
        || configuration.columns > 128U
        || configuration.rows > -2) {
        return false;
    }
    const std::uint64_t rows =
        static_cast<std::uint64_t>(
            -static_cast<std::int64_t>(
                configuration.rows));
    return static_cast<std::uint64_t>(
               configuration.columns)
            * rows
        < SequenceNumber::half_range;
}

[[nodiscard]] constexpr bool supports_matrix_fec(
    const PacketFilterConfiguration& configuration) noexcept
{
    return configuration.enabled
        && configuration.columns >= 2U
        && configuration.columns <= 128U
        && configuration.rows >= 2
        && static_cast<std::uint64_t>(
               configuration.columns)
            * static_cast<std::uint64_t>(
                configuration.rows)
        < SequenceNumber::half_range;
}

// Accumulates one row of already encrypted wire packets. Storage is fixed at
// construction and feed_source() performs no allocation. Construction rejects
// invalid geometry with std::invalid_argument; instances require external
// serialization.
class RowFecEncoder {
public:
    RowFecEncoder(
        const PacketFilterConfiguration& configuration,
        SequenceNumber initial_sequence,
        std::size_t maximum_payload_size);

    [[nodiscard]] Error feed_source(
        const PacketView& wire_packet) noexcept;
    [[nodiscard]] bool control_packet_ready() const noexcept
    {
        return control_ready_;
    }
    [[nodiscard]] std::optional<OutboundPacket>
    control_packet() const noexcept;
    void consume_control_packet() noexcept;

private:
    [[nodiscard]] std::optional<std::uint64_t> unwrap(
        SequenceNumber sequence) noexcept;
    void reset_group(std::uint64_t row) noexcept;
    void finish_group(const DataHeader& last_header) noexcept;

    SequenceNumber initial_sequence_{};
    SequenceNumber latest_sequence_{};
    std::uint64_t latest_index_ = 0;
    std::uint64_t current_row_ = 0;
    std::uint32_t columns_ = 0;
    std::uint32_t collected_ = 0;
    std::uint8_t flags_recovery_ = 0;
    std::uint16_t length_recovery_ = 0;
    std::uint32_t timestamp_recovery_ = 0;
    std::size_t maximum_payload_size_ = 0;
    DataHeader control_header_{};
    std::array<std::byte, maximum_data_payload_size>
        control_payload_{};
    bool has_latest_ = false;
    bool has_group_ = false;
    bool group_complete_ = true;
    bool control_ready_ = false;
};

struct RowFecReceiveResult {
    Error error = Error::none;
    bool consume_control_packet = false;
    bool has_reconstructed_packet = false;
    // The reconstructed packet payload borrows decoder-owned storage.
    PacketView reconstructed_packet{};
    // Both the reconstructed payload and this span remain valid until the next
    // receive() call on the same decoder or decoder destruction.
    std::span<const SequenceRange> irrecoverable_losses{};

    [[nodiscard]] constexpr explicit operator bool() const noexcept
    {
        return error == Error::none;
    }
};

using ColumnFecReceiveResult = RowFecReceiveResult;

// Keeps a bounded ring of row XOR states. The ring, source duplicate cache,
// and recovery payloads are allocated once by the constructor; receive()
// performs no allocation and can reconstruct at most one packet per call.
// Construction rejects invalid geometry with std::invalid_argument and may
// throw std::bad_alloc. Instances require external serialization.
class RowFecDecoder {
public:
    RowFecDecoder(
        const PacketFilterConfiguration& configuration,
        SequenceNumber initial_sequence,
        std::size_t receive_capacity_packets,
        std::size_t maximum_payload_size,
        bool expire_rows = true);

    [[nodiscard]] RowFecReceiveResult receive(
        const PacketView& wire_packet) noexcept;
    [[nodiscard]] static FecResourceEstimate estimate_resources(
        const PacketFilterConfiguration& configuration,
        std::size_t receive_capacity_packets,
        std::size_t maximum_payload_size) noexcept;
    [[nodiscard]] FecResourceUsage resource_usage() const noexcept;

private:
    struct Group {
        std::uint64_t row = 0;
        std::uint32_t collected = 0;
        std::uint32_t missing_position_xor = 0;
        SequenceNumber message_anchor_sequence {};
        std::uint32_t message_anchor_number = 0;
        std::uint8_t flags_recovery = 0;
        std::uint16_t length_recovery = 0;
        std::uint32_t timestamp_recovery = 0;
        std::uint32_t destination_socket_id = 0;
        bool active = false;
        bool has_control = false;
        bool has_message_anchor = false;
        bool reconstructed = false;
        bool in_order = false;
    };

    [[nodiscard]] std::optional<std::uint64_t> unwrap(
        SequenceNumber sequence) noexcept;
    [[nodiscard]] Group* group_for(
        std::uint64_t row) noexcept;
    [[nodiscard]] Group* find_group(
        std::uint64_t row) noexcept;
    void reset_group(Group& group, std::uint64_t row) noexcept;
    [[nodiscard]] std::span<std::byte> recovery_payload(
        const Group& group) noexcept;
    [[nodiscard]] std::span<std::uint64_t> received_words(
        const Group& group) noexcept;
    [[nodiscard]] bool source_was_seen(
        const Group& group,
        std::uint32_t position) noexcept;
    void mark_source_seen(
        Group& group,
        std::uint32_t position) noexcept;
    void clip_source(
        Group& group, const PacketView& packet,
        std::uint32_t position) noexcept;
    [[nodiscard]] Error clip_control(
        Group& group, const PacketView& packet,
        const FecControlDecodeResult& control) noexcept;
    [[nodiscard]] RowFecReceiveResult try_reconstruct(
        Group& group) noexcept;
    void advance_expiry(
        std::uint64_t current_row,
        std::uint32_t current_position) noexcept;
    void expire_row(std::uint64_t row) noexcept;
    void append_irrecoverable_range(
        std::uint64_t row,
        std::uint32_t first_position,
        std::uint32_t last_position) noexcept;
    [[nodiscard]] RowFecReceiveResult finish_result(
        RowFecReceiveResult result = {}) noexcept;

    SequenceNumber initial_sequence_{};
    SequenceNumber latest_sequence_{};
    std::uint64_t latest_index_ = 0;
    std::uint32_t columns_ = 0;
    std::size_t receive_capacity_packets_ = 0;
    std::size_t maximum_payload_size_ = 0;
    std::size_t received_words_per_group_ = 0;
    std::vector<Group> groups_;
    std::vector<std::byte> recovery_payloads_;
    std::vector<std::uint64_t> received_source_words_;
    std::vector<SequenceRange> irrecoverable_losses_;
    std::array<std::byte, maximum_data_payload_size>
        reconstructed_payload_{};
    std::uint64_t minimum_retained_row_ = 0;
    std::size_t irrecoverable_loss_count_ = 0;
    PacketFilterArqLevel arq_ =
        PacketFilterArqLevel::always;
    bool has_latest_ = false;
    bool irrecoverable_loss_overflow_ = false;
    bool expire_rows_ = true;
};

// Accumulates one independently advancing XOR group per configured column.
// The groups and their recovery payloads are allocated at construction;
// feed_source() performs direct arithmetic placement and no allocation.
// Construction may throw std::invalid_argument or std::bad_alloc; instances
// require external serialization.
class ColumnFecEncoder {
public:
    ColumnFecEncoder(
        const PacketFilterConfiguration& configuration,
        SequenceNumber initial_sequence,
        std::size_t maximum_payload_size);

    [[nodiscard]] Error feed_source(
        const PacketView& wire_packet) noexcept;
    [[nodiscard]] bool control_packet_ready() const noexcept
    {
        return control_ready_;
    }
    [[nodiscard]] std::optional<OutboundPacket>
    control_packet() const noexcept;
    void consume_control_packet() noexcept
    {
        control_ready_ = false;
    }

private:
    struct Location {
        std::uint64_t series = 0;
        std::uint32_t column = 0;
        std::uint32_t position = 0;
        std::uint64_t base_index = 0;
    };

    struct Group {
        std::uint64_t series = 0;
        std::uint32_t collected = 0;
        std::uint8_t flags_recovery = 0;
        std::uint16_t length_recovery = 0;
        std::uint32_t timestamp_recovery = 0;
        bool active = false;
    };

    [[nodiscard]] std::optional<std::uint64_t> unwrap(
        SequenceNumber sequence) noexcept;
    [[nodiscard]] std::uint64_t first_base(
        std::uint32_t column) const noexcept;
    [[nodiscard]] std::optional<Location> locate_source(
        std::uint64_t index) const noexcept;
    [[nodiscard]] std::span<std::byte> recovery_payload(
        const Group& group) noexcept;
    void reset_group(
        Group& group, std::uint64_t series) noexcept;
    void finish_group(
        const Group& group,
        std::uint32_t column,
        const DataHeader& last_header) noexcept;

    SequenceNumber initial_sequence_{};
    SequenceNumber latest_sequence_{};
    std::uint64_t latest_index_ = 0;
    std::uint64_t matrix_size_ = 0;
    std::uint32_t columns_ = 0;
    std::uint32_t rows_ = 0;
    std::size_t maximum_payload_size_ = 0;
    PacketFilterLayout layout_ =
        PacketFilterLayout::staircase;
    std::vector<Group> groups_;
    std::vector<std::byte> recovery_payloads_;
    DataHeader control_header_{};
    std::array<std::byte, maximum_data_payload_size>
        control_payload_{};
    bool has_latest_ = false;
    bool control_ready_ = false;
};

// Column-only receive groups are addressed by (series, column) and retained
// in a bounded construction-time-sized ring. Every receive operation is
// allocation-free and can rebuild at most one packet. Construction may throw
// std::invalid_argument or std::bad_alloc; instances require external
// serialization.
class ColumnFecDecoder {
public:
    ColumnFecDecoder(
        const PacketFilterConfiguration& configuration,
        SequenceNumber initial_sequence,
        std::size_t receive_capacity_packets,
        std::size_t maximum_payload_size);

    [[nodiscard]] ColumnFecReceiveResult receive(
        const PacketView& wire_packet) noexcept;
    [[nodiscard]] static FecResourceEstimate estimate_resources(
        const PacketFilterConfiguration& configuration,
        std::size_t receive_capacity_packets,
        std::size_t maximum_payload_size) noexcept;
    [[nodiscard]] FecResourceUsage resource_usage() const noexcept;

private:
    struct Location {
        std::uint64_t series = 0;
        std::uint32_t column = 0;
        std::uint32_t position = 0;
        std::uint64_t base_index = 0;
    };

    struct Group {
        std::uint64_t ordinal = 0;
        std::uint64_t series = 0;
        std::uint32_t column = 0;
        std::uint32_t collected = 0;
        std::uint32_t missing_position_xor = 0;
        SequenceNumber message_anchor_sequence {};
        std::uint32_t message_anchor_number = 0;
        std::uint8_t flags_recovery = 0;
        std::uint16_t length_recovery = 0;
        std::uint32_t timestamp_recovery = 0;
        std::uint32_t destination_socket_id = 0;
        bool active = false;
        bool has_control = false;
        bool has_message_anchor = false;
        bool reconstructed = false;
        bool dismissed = false;
        bool in_order = false;
    };

    [[nodiscard]] std::optional<std::uint64_t> unwrap(
        SequenceNumber sequence) noexcept;
    [[nodiscard]] std::uint64_t first_base(
        std::uint32_t column) const noexcept;
    [[nodiscard]] std::optional<Location> locate_source(
        std::uint64_t index) const noexcept;
    [[nodiscard]] std::optional<Location> locate_control(
        std::uint64_t index,
        std::uint32_t column) const noexcept;
    [[nodiscard]] Group* group_for(
        const Location& location) noexcept;
    [[nodiscard]] Group* find_group(
        std::uint64_t series,
        std::uint32_t column) noexcept;
    void reset_group(
        Group& group,
        const Location& location) noexcept;
    [[nodiscard]] std::span<std::byte> recovery_payload(
        const Group& group) noexcept;
    [[nodiscard]] std::span<std::uint64_t> received_words(
        const Group& group) noexcept;
    [[nodiscard]] bool source_was_seen(
        const Group& group,
        std::uint32_t position) noexcept;
    void mark_source_seen(
        Group& group,
        std::uint32_t position) noexcept;
    void clip_source(
        Group& group,
        const PacketView& packet,
        std::uint32_t position) noexcept;
    [[nodiscard]] Error clip_control(
        Group& group,
        const PacketView& packet,
        const FecControlDecodeResult& control) noexcept;
    [[nodiscard]] ColumnFecReceiveResult try_reconstruct(
        Group& group,
        const Location& location) noexcept;
    void advance_expiry(
        std::uint64_t current_series,
        std::uint64_t current_index) noexcept;
    void expire_group(
        std::uint64_t series,
        std::uint32_t column) noexcept;
    void append_loss_index(
        std::uint64_t index) noexcept;
    void sort_loss_indices() noexcept;
    void build_loss_ranges() noexcept;
    [[nodiscard]] ColumnFecReceiveResult finish_result(
        ColumnFecReceiveResult result = {}) noexcept;

    SequenceNumber initial_sequence_{};
    SequenceNumber latest_sequence_{};
    std::uint64_t latest_index_ = 0;
    std::uint64_t matrix_size_ = 0;
    std::uint64_t minimum_retained_series_ = 0;
    std::uint32_t columns_ = 0;
    std::uint32_t rows_ = 0;
    std::size_t receive_capacity_packets_ = 0;
    std::size_t maximum_payload_size_ = 0;
    std::size_t received_words_per_group_ = 0;
    PacketFilterLayout layout_ =
        PacketFilterLayout::staircase;
    PacketFilterArqLevel arq_ =
        PacketFilterArqLevel::always;
    std::vector<Group> groups_;
    std::vector<std::byte> recovery_payloads_;
    std::vector<std::uint64_t> received_source_words_;
    std::vector<std::uint64_t> loss_indices_;
    std::vector<SequenceRange> irrecoverable_losses_;
    std::array<std::byte, maximum_data_payload_size>
        reconstructed_payload_{};
    std::size_t loss_index_count_ = 0;
    std::size_t irrecoverable_loss_count_ = 0;
    bool has_latest_ = false;
    bool loss_overflow_ = false;
};

// Feeds every source into the row and column engines used by a positive
// multi-row FEC configuration. Ready column controls are exposed before a
// simultaneously ready row control, matching the wire scheduling rule.
// Construction may throw std::invalid_argument or std::bad_alloc; instances
// require external serialization.
class MatrixFecEncoder {
public:
    MatrixFecEncoder(
        const PacketFilterConfiguration& configuration,
        SequenceNumber initial_sequence,
        std::size_t maximum_payload_size);

    [[nodiscard]] Error feed_source(
        const PacketView& wire_packet) noexcept;
    [[nodiscard]] bool control_packet_ready() const noexcept;
    [[nodiscard]] std::optional<OutboundPacket>
    control_packet() const noexcept;
    void consume_control_packet() noexcept;

private:
    RowFecEncoder row_;
    ColumnFecEncoder column_;
};

struct MatrixFecReceiveResult {
    Error error = Error::none;
    bool consume_control_packet = false;
    // Both spans and every PacketView payload they expose borrow decoder-owned
    // storage and remain valid until the next receive() call or destruction.
    std::span<const PacketView> reconstructed_packets{};
    std::span<const SequenceRange> irrecoverable_losses{};

    [[nodiscard]] constexpr explicit operator bool() const noexcept
    {
        return error == Error::none;
    }
};

// Combines the independently bounded row and column decoders. Every rebuilt
// packet is copied into fixed construction-time storage, fed into the
// intersecting dimension, and may trigger the next iteration of a recovery
// chain. receive() performs no allocation and returns every packet rebuilt by
// that chain. Construction may throw std::invalid_argument or std::bad_alloc;
// instances require external serialization.
class MatrixFecDecoder {
public:
    MatrixFecDecoder(
        const PacketFilterConfiguration& configuration,
        SequenceNumber initial_sequence,
        std::size_t receive_capacity_packets,
        std::size_t maximum_payload_size);

    [[nodiscard]] MatrixFecReceiveResult receive(
        const PacketView& wire_packet) noexcept;
    [[nodiscard]] static FecResourceEstimate estimate_resources(
        const PacketFilterConfiguration& configuration,
        std::size_t receive_capacity_packets,
        std::size_t maximum_payload_size) noexcept;
    [[nodiscard]] FecResourceUsage resource_usage() const noexcept;

private:
    struct ValidatedResources { };

    MatrixFecDecoder(ValidatedResources resources,
        const PacketFilterConfiguration& configuration,
        SequenceNumber initial_sequence, std::size_t receive_capacity_packets,
        std::size_t maximum_payload_size);
    [[nodiscard]] static ValidatedResources require_resources(
        const PacketFilterConfiguration& configuration,
        std::size_t receive_capacity_packets, std::size_t maximum_payload_size);

    enum class Dimension : std::uint8_t {
        row,
        column,
    };

    [[nodiscard]] Error append_losses(
        std::span<const SequenceRange> losses) noexcept;
    [[nodiscard]] Error append_reconstructed(
        const PacketView& packet) noexcept;
    [[nodiscard]] Error drain_reconstruction_chain(
        const RowFecReceiveResult& first,
        Dimension source_dimension) noexcept;
    [[nodiscard]] MatrixFecReceiveResult finish_result(
        bool consume_control_packet) noexcept;

    RowFecDecoder row_;
    ColumnFecDecoder column_;
    std::size_t maximum_payload_size_ = 0;
    std::vector<PacketView> reconstructed_packets_;
    std::vector<std::byte> reconstructed_payloads_;
    std::vector<SequenceRange> irrecoverable_losses_;
    std::size_t reconstructed_packet_count_ = 0;
    std::size_t irrecoverable_loss_count_ = 0;
    Error error_ = Error::none;
    std::optional<SequenceNumber>
        current_source_sequence_{};
};

} // namespace robotweax::srt
