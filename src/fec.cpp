#include "robotweax/srt/fec.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace robotweax::srt {
namespace {

[[nodiscard]] constexpr bool checked_add(
    std::size_t left, std::size_t right, std::size_t& result) noexcept
{
    if (right > std::numeric_limits<std::size_t>::max() - left) {
        return false;
    }
    result = left + right;
    return true;
}

[[nodiscard]] constexpr bool checked_multiply(
    std::size_t left, std::size_t right, std::size_t& result) noexcept
{
    if (left != 0U && right > std::numeric_limits<std::size_t>::max() / left) {
        return false;
    }
    result = left * right;
    return true;
}

[[nodiscard]] bool assign_resource_bytes(std::size_t count,
    std::size_t element_size, std::size_t& component,
    std::size_t& total) noexcept
{
    std::size_t bytes = 0;
    std::size_t next_total = 0;
    if (!checked_multiply(count, element_size, bytes)
        || !checked_add(total, bytes, next_total)) {
        return false;
    }
    component = bytes;
    total = next_total;
    return true;
}

[[nodiscard]] bool append_resource_usage(
    FecResourceUsage& destination, const FecResourceUsage& source) noexcept
{
    FecResourceUsage next = destination;
    if (!checked_add(next.group_slots, source.group_slots, next.group_slots)
        || !checked_add(next.group_metadata_bytes, source.group_metadata_bytes,
            next.group_metadata_bytes)
        || !checked_add(next.recovery_payload_bytes,
            source.recovery_payload_bytes, next.recovery_payload_bytes)
        || !checked_add(next.source_tracking_bytes,
            source.source_tracking_bytes, next.source_tracking_bytes)
        || !checked_add(next.loss_tracking_bytes, source.loss_tracking_bytes,
            next.loss_tracking_bytes)
        || !checked_add(next.reconstruction_bytes, source.reconstruction_bytes,
            next.reconstruction_bytes)
        || !checked_add(
            next.dynamic_bytes, source.dynamic_bytes, next.dynamic_bytes)) {
        return false;
    }
    destination = next;
    return true;
}

[[nodiscard]] FecResourceUsage measured_usage(std::size_t group_slots,
    std::size_t group_metadata_bytes, std::size_t recovery_payload_bytes,
    std::size_t source_tracking_bytes, std::size_t loss_tracking_bytes,
    std::size_t reconstruction_bytes) noexcept
{
    FecResourceUsage usage {
        .group_slots = group_slots,
        .group_metadata_bytes = group_metadata_bytes,
        .recovery_payload_bytes = recovery_payload_bytes,
        .source_tracking_bytes = source_tracking_bytes,
        .loss_tracking_bytes = loss_tracking_bytes,
        .reconstruction_bytes = reconstruction_bytes,
    };
    const bool valid = checked_add(usage.dynamic_bytes, group_metadata_bytes,
                           usage.dynamic_bytes)
        && checked_add(
            usage.dynamic_bytes, recovery_payload_bytes, usage.dynamic_bytes)
        && checked_add(
            usage.dynamic_bytes, source_tracking_bytes, usage.dynamic_bytes)
        && checked_add(
            usage.dynamic_bytes, loss_tracking_bytes, usage.dynamic_bytes)
        && checked_add(
            usage.dynamic_bytes, reconstruction_bytes, usage.dynamic_bytes);
    if (!valid) {
        usage.dynamic_bytes = std::numeric_limits<std::size_t>::max();
    }
    return usage;
}

[[nodiscard]] constexpr std::uint32_t xor_through(
    std::uint32_t value) noexcept
{
    switch (value & 3U) {
    case 0U: return value;
    case 1U: return 1U;
    case 2U: return value + 1U;
    default: return 0U;
    }
}

// The SRT message-number space is one-based: zero identifies a packet-filter
// control packet and DATA wraps from 0x03ff'ffff back to one. Built-in FEC is
// defined for Live/Solo DATA, so every source packet advances both sequence
// and message number by one. Recover the authenticated message-number field
// from any surviving source in the same FEC group instead of inheriting the
// upstream implementation's unauthenticatable constant value of one.
[[nodiscard]] constexpr std::uint32_t advance_message_number(
    std::uint32_t anchor, std::int32_t distance) noexcept
{
    constexpr std::int64_t maximum = 0x03ff'ffffLL;
    const std::int64_t zero_based = static_cast<std::int64_t>(anchor) - 1LL;
    std::int64_t advanced =
        (zero_based + static_cast<std::int64_t>(distance)) % maximum;
    if (advanced < 0) {
        advanced += maximum;
    }
    return static_cast<std::uint32_t>(advanced + 1LL);
}

[[nodiscard]] std::optional<std::uint64_t> unwrap_sequence(
    SequenceNumber initial_sequence,
    SequenceNumber sequence,
    SequenceNumber& latest_sequence,
    std::uint64_t& latest_index,
    bool& has_latest) noexcept
{
    if (!has_latest) {
        const std::uint32_t forward =
            (sequence.value() - initial_sequence.value())
            & SequenceNumber::mask;
        if (forward >= SequenceNumber::half_range) {
            return std::nullopt;
        }
        latest_sequence = sequence;
        latest_index = forward;
        has_latest = true;
        return latest_index;
    }

    const std::int32_t distance =
        sequence.distance_from(latest_sequence);
    if (distance > 0) {
        latest_index += static_cast<std::uint32_t>(
            distance);
        latest_sequence = sequence;
        return latest_index;
    }
    const std::uint64_t backwards =
        static_cast<std::uint64_t>(
            -static_cast<std::int64_t>(distance));
    if (backwards > latest_index) {
        return std::nullopt;
    }
    return latest_index - backwards;
}

void xor_payload(
    std::span<std::byte> recovery,
    std::span<const std::byte> payload) noexcept
{
    for (std::size_t index = 0; index < payload.size();
         ++index) {
        recovery[index] ^= payload[index];
    }
}

[[nodiscard]] std::uint8_t encryption_flag(
    EncryptionKey key) noexcept
{
    return static_cast<std::uint8_t>(key);
}

[[nodiscard]] EncryptionKey encryption_key(
    std::uint8_t flag) noexcept
{
    return static_cast<EncryptionKey>(flag);
}

[[nodiscard]] PacketFilterConfiguration
matrix_row_configuration(
    const PacketFilterConfiguration& configuration)
{
    if (!supports_matrix_fec(configuration)) {
        throw std::invalid_argument(
            "invalid matrix FEC configuration");
    }
    auto row = configuration;
    row.rows = 1;
    row.rows_specified = true;
    row.arq = PacketFilterArqLevel::never;
    row.arq_specified = true;
    return row;
}

[[nodiscard]] PacketFilterConfiguration
matrix_column_configuration(
    const PacketFilterConfiguration& configuration)
{
    if (!supports_matrix_fec(configuration)) {
        throw std::invalid_argument(
            "invalid matrix FEC configuration");
    }
    auto column = configuration;
    column.rows = -configuration.rows;
    column.rows_specified = true;
    return column;
}

} // namespace

RowFecEncoder::RowFecEncoder(
    const PacketFilterConfiguration& configuration,
    SequenceNumber initial_sequence,
    std::size_t maximum_payload_size)
    : initial_sequence_(initial_sequence)
    , columns_(configuration.columns)
    , maximum_payload_size_(maximum_payload_size)
{
    if (!supports_row_only_fec(configuration)
        || maximum_payload_size == 0U
        || maximum_payload_size
            > maximum_data_payload_size
                - fec_filter_header_size) {
        throw std::invalid_argument(
            "invalid row-only FEC encoder configuration");
    }
}

std::optional<std::uint64_t> RowFecEncoder::unwrap(
    SequenceNumber sequence) noexcept
{
    return unwrap_sequence(
        initial_sequence_, sequence, latest_sequence_,
        latest_index_, has_latest_);
}

void RowFecEncoder::reset_group(
    std::uint64_t row) noexcept
{
    current_row_ = row;
    collected_ = 0;
    flags_recovery_ = 0;
    length_recovery_ = 0;
    timestamp_recovery_ = 0;
    std::fill_n(
        control_payload_.begin() + fec_filter_header_size,
        maximum_payload_size_, std::byte{0});
    has_group_ = true;
    group_complete_ = false;
}

void RowFecEncoder::finish_group(
    const DataHeader& last_header) noexcept
{
    const FecControlHeader recovery_header{
        .group_index = -1,
        .flags_recovery = flags_recovery_,
        .length_recovery = length_recovery_,
    };
    const Error encoded = encode_fec_control_header(
        recovery_header, control_payload_);
    if (encoded != Error::none) {
        return;
    }
    control_header_ = {
        .sequence = last_header.sequence,
        .message_number = 0,
        .boundary = MessageBoundary::solo,
        .in_order = false,
        .encryption_key = last_header.encryption_key,
        .retransmitted = false,
        .timestamp =
            PacketTimestamp{timestamp_recovery_},
        .destination_socket_id =
            last_header.destination_socket_id,
    };
    group_complete_ = true;
    control_ready_ = true;
}

Error RowFecEncoder::feed_source(
    const PacketView& wire_packet) noexcept
{
    if (wire_packet.kind != PacketKind::data
        || wire_packet.data.message_number == 0U
        || wire_packet.data.retransmitted
        || wire_packet.payload.size()
            > maximum_payload_size_) {
        return Error::invalid_packet_type;
    }
    if (control_ready_) {
        return Error::would_block;
    }

    const auto index = unwrap(
        wire_packet.data.sequence);
    if (!index.has_value()) {
        return Error::invalid_state;
    }
    const std::uint64_t row = *index / columns_;
    const std::uint32_t position =
        static_cast<std::uint32_t>(
            *index % columns_);
    if (!has_group_ || row != current_row_) {
        if (has_group_ && row < current_row_) {
            return Error::invalid_state;
        }
        reset_group(row);
    }
    if (group_complete_) {
        return Error::invalid_state;
    }

    flags_recovery_ ^= encryption_flag(
        wire_packet.data.encryption_key);
    length_recovery_ ^=
        static_cast<std::uint16_t>(
            wire_packet.payload.size());
    timestamp_recovery_ ^=
        wire_packet.data.timestamp.value();
    xor_payload(
        std::span{control_payload_}.subspan(
            fec_filter_header_size,
            maximum_payload_size_),
        wire_packet.payload);
    ++collected_;

    if (position == columns_ - 1U) {
        if (collected_ == columns_) {
            finish_group(wire_packet.data);
        } else {
            group_complete_ = true;
        }
    }
    return Error::none;
}

std::optional<OutboundPacket>
RowFecEncoder::control_packet() const noexcept
{
    if (!control_ready_) {
        return std::nullopt;
    }
    return OutboundPacket{
        .header = control_header_,
        .payload = std::span{control_payload_}.first(
            fec_filter_header_size
            + maximum_payload_size_),
    };
}

void RowFecEncoder::consume_control_packet() noexcept
{
    control_ready_ = false;
}

RowFecDecoder::RowFecDecoder(
    const PacketFilterConfiguration& configuration,
    SequenceNumber initial_sequence,
    std::size_t receive_capacity_packets,
    std::size_t maximum_payload_size,
    bool expire_rows)
    : initial_sequence_(initial_sequence)
    , columns_(configuration.columns)
    , receive_capacity_packets_(
          receive_capacity_packets)
    , maximum_payload_size_(maximum_payload_size)
    , arq_(configuration.arq)
    , expire_rows_(expire_rows)
{
    const auto resources = estimate_resources(
        configuration, receive_capacity_packets, maximum_payload_size);
    if (!resources) {
        throw std::invalid_argument(
            "invalid row-only FEC decoder configuration");
    }
    const std::size_t group_count = resources.usage.group_slots;
    groups_.resize(group_count);
    recovery_payloads_.resize(
        group_count * maximum_payload_size_);
    received_words_per_group_ =
        (columns_ + 63U) / 64U;
    received_source_words_.resize(
        group_count * received_words_per_group_);
    irrecoverable_losses_.resize(
        receive_capacity_packets * 2U);
}

FecResourceEstimate RowFecDecoder::estimate_resources(
    const PacketFilterConfiguration& configuration,
    std::size_t receive_capacity_packets,
    std::size_t maximum_payload_size) noexcept
{
    if (!supports_row_only_fec(configuration) || receive_capacity_packets == 0U
        || receive_capacity_packets >= SequenceNumber::half_range
        || configuration.columns > receive_capacity_packets
        || maximum_payload_size == 0U
        || maximum_payload_size
            > maximum_data_payload_size - fec_filter_header_size) {
        return {.error = Error::invalid_state};
    }

    std::size_t rounded_capacity = 0;
    std::size_t group_count = 0;
    if (!checked_add(receive_capacity_packets, configuration.columns - 1U,
            rounded_capacity)
        || !checked_add(
            rounded_capacity / configuration.columns, 3U, group_count)) {
        return {.error = Error::buffer_too_small};
    }
    const std::size_t words_per_group =
        (static_cast<std::size_t>(configuration.columns) + 63U) / 64U;
    std::size_t source_word_count = 0;
    std::size_t loss_count = 0;
    FecResourceUsage usage {
        .group_slots = group_count,
    };
    if (!checked_multiply(group_count, words_per_group, source_word_count)
        || !checked_multiply(receive_capacity_packets, 2U, loss_count)
        || !assign_resource_bytes(group_count, sizeof(Group),
            usage.group_metadata_bytes, usage.dynamic_bytes)
        || !assign_resource_bytes(group_count, maximum_payload_size,
            usage.recovery_payload_bytes, usage.dynamic_bytes)
        || !assign_resource_bytes(source_word_count, sizeof(std::uint64_t),
            usage.source_tracking_bytes, usage.dynamic_bytes)
        || !assign_resource_bytes(loss_count, sizeof(SequenceRange),
            usage.loss_tracking_bytes, usage.dynamic_bytes)) {
        return {.error = Error::buffer_too_small};
    }
    return {.usage = usage};
}

FecResourceUsage RowFecDecoder::resource_usage() const noexcept
{
    return measured_usage(groups_.capacity(),
        groups_.capacity() * sizeof(Group),
        recovery_payloads_.capacity() * sizeof(std::byte),
        received_source_words_.capacity() * sizeof(std::uint64_t),
        irrecoverable_losses_.capacity() * sizeof(SequenceRange), 0U);
}

std::optional<std::uint64_t> RowFecDecoder::unwrap(
    SequenceNumber sequence) noexcept
{
    if (!has_latest_) {
        const std::uint32_t forward =
            (sequence.value()
                - initial_sequence_.value())
            & SequenceNumber::mask;
        if (forward >= receive_capacity_packets_) {
            return std::nullopt;
        }
    } else {
        const std::int32_t distance =
            sequence.distance_from(latest_sequence_);
        if (distance > 0
            && static_cast<std::uint32_t>(distance)
                >= receive_capacity_packets_) {
            return std::nullopt;
        }
    }
    return unwrap_sequence(
        initial_sequence_, sequence, latest_sequence_,
        latest_index_, has_latest_);
}

RowFecDecoder::Group* RowFecDecoder::group_for(
    std::uint64_t row) noexcept
{
    Group& group =
        groups_[static_cast<std::size_t>(
            row % groups_.size())];
    if (!group.active || group.row != row) {
        if (row < minimum_retained_row_
            || (group.active && group.row > row)) {
            return nullptr;
        }
        reset_group(group, row);
    }
    return &group;
}

RowFecDecoder::Group* RowFecDecoder::find_group(
    std::uint64_t row) noexcept
{
    Group& group =
        groups_[static_cast<std::size_t>(
            row % groups_.size())];
    return group.active && group.row == row
        ? &group : nullptr;
}

void RowFecDecoder::reset_group(
    Group& group, std::uint64_t row) noexcept
{
    group = {
        .row = row,
        .missing_position_xor =
            xor_through(columns_ - 1U),
        .active = true,
    };
    std::fill(
        recovery_payload(group).begin(),
        recovery_payload(group).end(),
        std::byte{0});
    std::fill(
        received_words(group).begin(),
        received_words(group).end(), 0U);
}

std::span<std::byte> RowFecDecoder::recovery_payload(
    const Group& group) noexcept
{
    const std::size_t group_index =
        static_cast<std::size_t>(&group - groups_.data());
    return std::span{recovery_payloads_}.subspan(
        group_index * maximum_payload_size_,
        maximum_payload_size_);
}

std::span<std::uint64_t>
RowFecDecoder::received_words(
    const Group& group) noexcept
{
    const std::size_t group_index =
        static_cast<std::size_t>(
            &group - groups_.data());
    return std::span{received_source_words_}.subspan(
        group_index * received_words_per_group_,
        received_words_per_group_);
}

bool RowFecDecoder::source_was_seen(
    const Group& group,
    std::uint32_t position) noexcept
{
    const auto words = received_words(group);
    const std::size_t word = position / 64U;
    const std::uint32_t bit = position % 64U;
    return (words[word] & (std::uint64_t{1} << bit))
        != 0U;
}

void RowFecDecoder::mark_source_seen(
    Group& group,
    std::uint32_t position) noexcept
{
    auto words = received_words(group);
    const std::size_t word = position / 64U;
    const std::uint32_t bit = position % 64U;
    words[word] |= std::uint64_t{1} << bit;
}

void RowFecDecoder::clip_source(
    Group& group, const PacketView& packet,
    std::uint32_t position) noexcept
{
    group.flags_recovery ^=
        encryption_flag(packet.data.encryption_key);
    group.length_recovery ^=
        static_cast<std::uint16_t>(
            packet.payload.size());
    group.timestamp_recovery ^=
        packet.data.timestamp.value();
    group.missing_position_xor ^= position;
    group.destination_socket_id =
        packet.data.destination_socket_id;
    if (!group.has_message_anchor) {
        group.message_anchor_sequence = packet.data.sequence;
        group.message_anchor_number = packet.data.message_number;
        group.has_message_anchor = true;
    }
    group.in_order = packet.data.in_order;
    xor_payload(recovery_payload(group),
        packet.payload);
    ++group.collected;
}

Error RowFecDecoder::clip_control(
    Group& group, const PacketView& packet,
    const FecControlDecodeResult& control) noexcept
{
    if (control.header.group_index != -1
        || control.payload_recovery.size()
            != maximum_payload_size_) {
        return Error::invalid_control_payload;
    }
    if (group.has_control) {
        return Error::none;
    }
    group.flags_recovery ^=
        control.header.flags_recovery;
    group.length_recovery ^=
        control.header.length_recovery;
    group.timestamp_recovery ^=
        packet.data.timestamp.value();
    group.destination_socket_id =
        packet.data.destination_socket_id;
    xor_payload(recovery_payload(group),
        control.payload_recovery);
    group.has_control = true;
    return Error::none;
}

RowFecReceiveResult RowFecDecoder::try_reconstruct(
    Group& group) noexcept
{
    if (!group.has_control || group.reconstructed
        || group.collected != columns_ - 1U) {
        return {};
    }
    if (group.missing_position_xor >= columns_
        || group.length_recovery > maximum_payload_size_
        || group.flags_recovery > 3U || !group.has_message_anchor
        || group.message_anchor_number == 0U) {
        return {.error = Error::invalid_control_payload};
    }

    const std::uint64_t missing_index =
        group.row * columns_
        + group.missing_position_xor;
    if (missing_index / columns_ != group.row) {
        return {.error = Error::invalid_control_payload};
    }
    const auto payload = recovery_payload(group);
    std::copy_n(
        payload.begin(), group.length_recovery,
        reconstructed_payload_.begin());
    mark_source_seen(
        group, group.missing_position_xor);
    ++group.collected;
    group.reconstructed = true;

    const SequenceNumber sequence =
        initial_sequence_.advanced(
            static_cast<std::uint32_t>(
                missing_index
                & SequenceNumber::mask));
    const std::uint32_t message_number =
        advance_message_number(group.message_anchor_number,
            sequence.distance_from(group.message_anchor_sequence));
    return {
        .has_reconstructed_packet = true,
        .reconstructed_packet =
            {
                .kind = PacketKind::data,
                .data =
                    {
                        .sequence = sequence,
                        .message_number = message_number,
                        .boundary = MessageBoundary::solo,
                        .in_order = group.in_order,
                        .encryption_key = encryption_key(group.flags_recovery),
                        .retransmitted = true,
                        .timestamp = PacketTimestamp {group.timestamp_recovery},
                        .destination_socket_id = group.destination_socket_id,
                    },
                .payload = std::span {reconstructed_payload_}.first(
                    group.length_recovery),
            },
    };
}

void RowFecDecoder::append_irrecoverable_range(
    std::uint64_t row,
    std::uint32_t first_position,
    std::uint32_t last_position) noexcept
{
    if (row
        > std::numeric_limits<std::uint64_t>::max()
            / columns_) {
        irrecoverable_loss_overflow_ = true;
        return;
    }
    const std::uint64_t base = row * columns_;
    const SequenceRange range{
        .first = initial_sequence_.advanced(
            static_cast<std::uint32_t>(
                (base + first_position)
                & SequenceNumber::mask)),
        .last = initial_sequence_.advanced(
            static_cast<std::uint32_t>(
                (base + last_position)
                & SequenceNumber::mask)),
    };
    if (irrecoverable_loss_count_ != 0U) {
        auto& previous = irrecoverable_losses_[
            irrecoverable_loss_count_ - 1U];
        if (range.first == previous.last.next()) {
            previous.last = range.last;
            return;
        }
    }
    if (irrecoverable_loss_count_
        == irrecoverable_losses_.size()) {
        irrecoverable_loss_overflow_ = true;
        return;
    }
    irrecoverable_losses_[
        irrecoverable_loss_count_++] = range;
}

void RowFecDecoder::expire_row(
    std::uint64_t row) noexcept
{
    Group* group = find_group(row);
    if (arq_ == PacketFilterArqLevel::on_request) {
        if (group == nullptr) {
            append_irrecoverable_range(
                row, 0U, columns_ - 1U);
        } else {
            std::uint32_t position = 0;
            while (position < columns_) {
                while (position < columns_
                    && source_was_seen(
                        *group, position)) {
                    ++position;
                }
                if (position == columns_) {
                    break;
                }
                const std::uint32_t first = position;
                while (position + 1U < columns_
                    && !source_was_seen(
                        *group, position + 1U)) {
                    ++position;
                }
                append_irrecoverable_range(
                    row, first, position);
                ++position;
            }
        }
    }
    if (group != nullptr) {
        group->active = false;
    }
}

void RowFecDecoder::advance_expiry(
    std::uint64_t current_row,
    std::uint32_t current_position) noexcept
{
    if (!expire_rows_) {
        return;
    }
    std::uint64_t retain_from = minimum_retained_row_;
    if (current_row >= 2U) {
        retain_from = current_row - 1U;
    }
    if (current_row >= 1U
        && current_position > columns_ / 3U) {
        retain_from = current_row;
    }
    if (retain_from <= minimum_retained_row_) {
        return;
    }
    for (std::uint64_t row = minimum_retained_row_;
         row < retain_from; ++row) {
        expire_row(row);
    }
    minimum_retained_row_ = retain_from;
}

RowFecReceiveResult RowFecDecoder::finish_result(
    RowFecReceiveResult result) noexcept
{
    if (irrecoverable_loss_overflow_) {
        result.error = Error::buffer_too_small;
        return result;
    }
    result.irrecoverable_losses =
        std::span{irrecoverable_losses_}.first(
            irrecoverable_loss_count_);
    return result;
}

RowFecReceiveResult RowFecDecoder::receive(
    const PacketView& wire_packet) noexcept
{
    irrecoverable_loss_count_ = 0;
    irrecoverable_loss_overflow_ = false;
    if (wire_packet.kind != PacketKind::data) {
        return {.error = Error::invalid_packet_type};
    }
    if (wire_packet.data.message_number == 0U) {
        const auto control =
            decode_fec_control_payload(
                wire_packet.payload);
        if (!control
            || control.header.group_index != -1
            || control.payload_recovery.size()
                != maximum_payload_size_) {
            return {
                .error =
                    Error::invalid_control_payload,
                .consume_control_packet = true,
            };
        }
        const auto index = unwrap(
            wire_packet.data.sequence);
        if (!index.has_value()
            || *index % columns_ != columns_ - 1U) {
            return {
                .error =
                    Error::invalid_control_payload,
                .consume_control_packet = true,
            };
        }
        const std::uint64_t row = *index / columns_;
        advance_expiry(
            row, columns_ - 1U);
        Group* group = group_for(
            row);
        if (group == nullptr) {
            return finish_result({
                .consume_control_packet = true});
        }
        const Error error = clip_control(
            *group, wire_packet, control);
        if (error != Error::none) {
            return {
                .error = error,
                .consume_control_packet = true,
            };
        }
        auto result = try_reconstruct(*group);
        result.consume_control_packet = true;
        return finish_result(result);
    }

    if (wire_packet.payload.size()
        > maximum_payload_size_) {
        return {.error = Error::invalid_packet_type};
    }
    const auto index = unwrap(
        wire_packet.data.sequence);
    if (!index.has_value()) {
        return {};
    }
    if (latest_index_ > *index
        && latest_index_ - *index
            >= receive_capacity_packets_) {
        return {};
    }
    const std::uint64_t row = *index / columns_;
    const std::uint32_t position =
        static_cast<std::uint32_t>(
            *index % columns_);
    advance_expiry(row, position);
    Group* group = group_for(row);
    if (group == nullptr) {
        return finish_result();
    }
    if (source_was_seen(*group, position)) {
        return finish_result();
    }
    mark_source_seen(*group, position);
    clip_source(
        *group, wire_packet, position);
    return finish_result(
        try_reconstruct(*group));
}

ColumnFecEncoder::ColumnFecEncoder(
    const PacketFilterConfiguration& configuration,
    SequenceNumber initial_sequence,
    std::size_t maximum_payload_size)
    : initial_sequence_(initial_sequence)
    , columns_(configuration.columns)
    , rows_(static_cast<std::uint32_t>(
          -static_cast<std::int64_t>(
              configuration.rows)))
    , maximum_payload_size_(maximum_payload_size)
    , layout_(configuration.layout)
{
    if (!supports_column_only_fec(configuration)
        || maximum_payload_size == 0U
        || maximum_payload_size
            > maximum_data_payload_size
                - fec_filter_header_size) {
        throw std::invalid_argument(
            "invalid column-only FEC encoder configuration");
    }
    matrix_size_ =
        static_cast<std::uint64_t>(columns_) * rows_;
    groups_.resize(columns_);
    recovery_payloads_.resize(
        static_cast<std::size_t>(columns_)
        * maximum_payload_size_);
}

std::optional<std::uint64_t>
ColumnFecEncoder::unwrap(
    SequenceNumber sequence) noexcept
{
    return unwrap_sequence(
        initial_sequence_, sequence, latest_sequence_,
        latest_index_, has_latest_);
}

std::uint64_t ColumnFecEncoder::first_base(
    std::uint32_t column) const noexcept
{
    if (layout_ == PacketFilterLayout::even) {
        return column;
    }
    return static_cast<std::uint64_t>(column)
        + static_cast<std::uint64_t>(
              column % rows_)
            * columns_;
}

std::optional<ColumnFecEncoder::Location>
ColumnFecEncoder::locate_source(
    std::uint64_t index) const noexcept
{
    const std::uint32_t column =
        static_cast<std::uint32_t>(
            index % columns_);
    const std::uint64_t first =
        first_base(column);
    if (index < first) {
        return std::nullopt;
    }
    const std::uint64_t distance = index - first;
    const std::uint64_t within =
        distance % matrix_size_;
    if (within % columns_ != 0U) {
        return std::nullopt;
    }
    const std::uint32_t position =
        static_cast<std::uint32_t>(
            within / columns_);
    if (position >= rows_) {
        return std::nullopt;
    }
    return Location{
        .series = distance / matrix_size_,
        .column = column,
        .position = position,
        .base_index =
            first
            + (distance / matrix_size_)
                * matrix_size_,
    };
}

std::span<std::byte>
ColumnFecEncoder::recovery_payload(
    const Group& group) noexcept
{
    const std::size_t index =
        static_cast<std::size_t>(
            &group - groups_.data());
    return std::span{recovery_payloads_}.subspan(
        index * maximum_payload_size_,
        maximum_payload_size_);
}

void ColumnFecEncoder::reset_group(
    Group& group,
    std::uint64_t series) noexcept
{
    group = {
        .series = series,
        .active = true,
    };
    auto payload = recovery_payload(group);
    std::fill(
        payload.begin(), payload.end(), std::byte{0});
}

void ColumnFecEncoder::finish_group(
    const Group& group,
    std::uint32_t column,
    const DataHeader& last_header) noexcept
{
    if (column > 127U) {
        return;
    }
    const Error encoded = encode_fec_control_header({
        .group_index =
            static_cast<std::int8_t>(column),
        .flags_recovery = group.flags_recovery,
        .length_recovery = group.length_recovery,
    }, control_payload_);
    if (encoded != Error::none) {
        return;
    }
    const auto source = recovery_payload(group);
    std::copy(
        source.begin(), source.end(),
        control_payload_.begin()
            + fec_filter_header_size);
    control_header_ = {
        .sequence = last_header.sequence,
        .message_number = 0,
        .boundary = MessageBoundary::solo,
        .in_order = false,
        .encryption_key =
            last_header.encryption_key,
        .retransmitted = false,
        .timestamp =
            PacketTimestamp{
                group.timestamp_recovery},
        .destination_socket_id =
            last_header.destination_socket_id,
    };
    control_ready_ = true;
}

Error ColumnFecEncoder::feed_source(
    const PacketView& wire_packet) noexcept
{
    if (wire_packet.kind != PacketKind::data
        || wire_packet.data.message_number == 0U
        || wire_packet.data.retransmitted
        || wire_packet.payload.size()
            > maximum_payload_size_) {
        return Error::invalid_packet_type;
    }
    if (control_ready_) {
        return Error::would_block;
    }
    const auto index = unwrap(
        wire_packet.data.sequence);
    if (!index.has_value()) {
        return Error::invalid_state;
    }
    const auto location = locate_source(*index);
    if (!location.has_value()) {
        return Error::none;
    }
    Group& group = groups_[location->column];
    if (!group.active
        || group.series != location->series) {
        if (group.active
            && group.series > location->series) {
            return Error::invalid_state;
        }
        reset_group(group, location->series);
    }
    if (group.collected != location->position) {
        return Error::invalid_state;
    }
    group.flags_recovery ^=
        encryption_flag(
            wire_packet.data.encryption_key);
    group.length_recovery ^=
        static_cast<std::uint16_t>(
            wire_packet.payload.size());
    group.timestamp_recovery ^=
        wire_packet.data.timestamp.value();
    xor_payload(
        recovery_payload(group),
        wire_packet.payload);
    ++group.collected;
    if (group.collected == rows_) {
        finish_group(
            group, location->column,
            wire_packet.data);
    }
    return Error::none;
}

std::optional<OutboundPacket>
ColumnFecEncoder::control_packet() const noexcept
{
    if (!control_ready_) {
        return std::nullopt;
    }
    return OutboundPacket{
        .header = control_header_,
        .payload =
            std::span{control_payload_}.first(
                fec_filter_header_size
                + maximum_payload_size_),
    };
}

ColumnFecDecoder::ColumnFecDecoder(
    const PacketFilterConfiguration& configuration,
    SequenceNumber initial_sequence,
    std::size_t receive_capacity_packets,
    std::size_t maximum_payload_size)
    : initial_sequence_(initial_sequence)
    , columns_(configuration.columns)
    , rows_(static_cast<std::uint32_t>(
          -static_cast<std::int64_t>(
              configuration.rows)))
    , receive_capacity_packets_(
          receive_capacity_packets)
    , maximum_payload_size_(maximum_payload_size)
    , layout_(configuration.layout)
    , arq_(configuration.arq)
{
    const auto resources = estimate_resources(
        configuration, receive_capacity_packets, maximum_payload_size);
    if (!resources) {
        throw std::invalid_argument(
            "invalid column-only FEC decoder configuration");
    }
    matrix_size_ =
        static_cast<std::uint64_t>(columns_) * rows_;
    const std::size_t group_count = resources.usage.group_slots;
    groups_.resize(group_count);
    recovery_payloads_.resize(
        group_count * maximum_payload_size_);
    received_words_per_group_ =
        (rows_ + 63U) / 64U;
    received_source_words_.resize(
        group_count * received_words_per_group_);
    loss_indices_.resize(
        receive_capacity_packets_ * 2U);
    irrecoverable_losses_.resize(
        receive_capacity_packets_ * 2U);
}

FecResourceEstimate ColumnFecDecoder::estimate_resources(
    const PacketFilterConfiguration& configuration,
    std::size_t receive_capacity_packets,
    std::size_t maximum_payload_size) noexcept
{
    if (!supports_column_only_fec(configuration)
        || receive_capacity_packets == 0U
        || receive_capacity_packets >= SequenceNumber::half_range
        || maximum_payload_size == 0U
        || maximum_payload_size
            > maximum_data_payload_size - fec_filter_header_size) {
        return {.error = Error::invalid_state};
    }
    const auto columns = static_cast<std::size_t>(configuration.columns);
    const auto rows = static_cast<std::size_t>(
        -static_cast<std::int64_t>(configuration.rows));
    std::size_t matrix_size = 0;
    if (!checked_multiply(columns, rows, matrix_size)
        || matrix_size > receive_capacity_packets) {
        return {.error = Error::invalid_state};
    }

    std::size_t rounded_capacity = 0;
    std::size_t column_margin = 0;
    std::size_t group_count = 0;
    if (!checked_add(receive_capacity_packets, rows - 1U, rounded_capacity)
        || !checked_multiply(columns, 3U, column_margin)
        || !checked_add(rounded_capacity / rows, column_margin, group_count)) {
        return {.error = Error::buffer_too_small};
    }
    const std::size_t words_per_group = (rows + 63U) / 64U;
    std::size_t source_word_count = 0;
    std::size_t loss_count = 0;
    FecResourceUsage usage {
        .group_slots = group_count,
    };
    if (!checked_multiply(group_count, words_per_group, source_word_count)
        || !checked_multiply(receive_capacity_packets, 2U, loss_count)
        || !assign_resource_bytes(group_count, sizeof(Group),
            usage.group_metadata_bytes, usage.dynamic_bytes)
        || !assign_resource_bytes(group_count, maximum_payload_size,
            usage.recovery_payload_bytes, usage.dynamic_bytes)
        || !assign_resource_bytes(source_word_count, sizeof(std::uint64_t),
            usage.source_tracking_bytes, usage.dynamic_bytes)
        || !assign_resource_bytes(loss_count, sizeof(std::uint64_t),
            usage.loss_tracking_bytes, usage.dynamic_bytes)) {
        return {.error = Error::buffer_too_small};
    }
    std::size_t range_bytes = 0;
    std::size_t next_loss_bytes = 0;
    if (!checked_multiply(loss_count, sizeof(SequenceRange), range_bytes)
        || !checked_add(usage.loss_tracking_bytes, range_bytes, next_loss_bytes)
        || !checked_add(
            usage.dynamic_bytes, range_bytes, usage.dynamic_bytes)) {
        return {.error = Error::buffer_too_small};
    }
    usage.loss_tracking_bytes = next_loss_bytes;
    return {.usage = usage};
}

FecResourceUsage ColumnFecDecoder::resource_usage() const noexcept
{
    return measured_usage(groups_.capacity(),
        groups_.capacity() * sizeof(Group),
        recovery_payloads_.capacity() * sizeof(std::byte),
        received_source_words_.capacity() * sizeof(std::uint64_t),
        loss_indices_.capacity() * sizeof(std::uint64_t)
            + irrecoverable_losses_.capacity() * sizeof(SequenceRange),
        0U);
}

std::optional<std::uint64_t>
ColumnFecDecoder::unwrap(
    SequenceNumber sequence) noexcept
{
    if (!has_latest_) {
        const std::uint32_t forward =
            (sequence.value()
                - initial_sequence_.value())
            & SequenceNumber::mask;
        if (forward >= receive_capacity_packets_) {
            return std::nullopt;
        }
    } else {
        const std::int32_t distance =
            sequence.distance_from(latest_sequence_);
        if (distance > 0
            && static_cast<std::uint32_t>(distance)
                >= receive_capacity_packets_) {
            return std::nullopt;
        }
    }
    return unwrap_sequence(
        initial_sequence_, sequence, latest_sequence_,
        latest_index_, has_latest_);
}

std::uint64_t ColumnFecDecoder::first_base(
    std::uint32_t column) const noexcept
{
    if (layout_ == PacketFilterLayout::even) {
        return column;
    }
    return static_cast<std::uint64_t>(column)
        + static_cast<std::uint64_t>(
              column % rows_)
            * columns_;
}

std::optional<ColumnFecDecoder::Location>
ColumnFecDecoder::locate_source(
    std::uint64_t index) const noexcept
{
    const std::uint32_t column =
        static_cast<std::uint32_t>(
            index % columns_);
    const std::uint64_t first =
        first_base(column);
    if (index < first) {
        return std::nullopt;
    }
    const std::uint64_t distance = index - first;
    const std::uint64_t within =
        distance % matrix_size_;
    if (within % columns_ != 0U) {
        return std::nullopt;
    }
    const std::uint32_t position =
        static_cast<std::uint32_t>(
            within / columns_);
    if (position >= rows_) {
        return std::nullopt;
    }
    return Location{
        .series = distance / matrix_size_,
        .column = column,
        .position = position,
        .base_index =
            first
            + (distance / matrix_size_)
                * matrix_size_,
    };
}

std::optional<ColumnFecDecoder::Location>
ColumnFecDecoder::locate_control(
    std::uint64_t index,
    std::uint32_t column) const noexcept
{
    const std::uint64_t tail =
        static_cast<std::uint64_t>(
            rows_ - 1U)
        * columns_;
    if (index < tail) {
        return std::nullopt;
    }
    const std::uint64_t base = index - tail;
    const std::uint64_t first =
        first_base(column);
    if (base < first
        || (base - first) % matrix_size_ != 0U) {
        return std::nullopt;
    }
    return Location{
        .series =
            (base - first) / matrix_size_,
        .column = column,
        .position = rows_ - 1U,
        .base_index = base,
    };
}

ColumnFecDecoder::Group*
ColumnFecDecoder::find_group(
    std::uint64_t series,
    std::uint32_t column) noexcept
{
    const std::uint64_t ordinal =
        series * columns_ + column;
    Group& group = groups_[
        static_cast<std::size_t>(
            ordinal % groups_.size())];
    return group.active
            && group.ordinal == ordinal
        ? &group : nullptr;
}

ColumnFecDecoder::Group*
ColumnFecDecoder::group_for(
    const Location& location) noexcept
{
    const std::uint64_t ordinal =
        location.series * columns_
        + location.column;
    Group& group = groups_[
        static_cast<std::size_t>(
            ordinal % groups_.size())];
    if (group.active
        && group.ordinal == ordinal) {
        return &group;
    }
    if (group.active
        && group.series
            >= minimum_retained_series_) {
        loss_overflow_ = true;
        return nullptr;
    }
    reset_group(group, location);
    return &group;
}

void ColumnFecDecoder::reset_group(
    Group& group,
    const Location& location) noexcept
{
    group = {
        .ordinal =
            location.series * columns_
                + location.column,
        .series = location.series,
        .column = location.column,
        .missing_position_xor =
            xor_through(rows_ - 1U),
        .active = true,
    };
    auto payload = recovery_payload(group);
    std::fill(
        payload.begin(), payload.end(), std::byte{0});
    auto words = received_words(group);
    std::fill(words.begin(), words.end(), 0U);
}

std::span<std::byte>
ColumnFecDecoder::recovery_payload(
    const Group& group) noexcept
{
    const std::size_t index =
        static_cast<std::size_t>(
            &group - groups_.data());
    return std::span{recovery_payloads_}.subspan(
        index * maximum_payload_size_,
        maximum_payload_size_);
}

std::span<std::uint64_t>
ColumnFecDecoder::received_words(
    const Group& group) noexcept
{
    const std::size_t index =
        static_cast<std::size_t>(
            &group - groups_.data());
    return std::span{received_source_words_}.subspan(
        index * received_words_per_group_,
        received_words_per_group_);
}

bool ColumnFecDecoder::source_was_seen(
    const Group& group,
    std::uint32_t position) noexcept
{
    const auto words = received_words(group);
    const std::size_t word = position / 64U;
    const std::uint32_t bit = position % 64U;
    return (words[word]
            & (std::uint64_t{1} << bit))
        != 0U;
}

void ColumnFecDecoder::mark_source_seen(
    Group& group,
    std::uint32_t position) noexcept
{
    auto words = received_words(group);
    const std::size_t word = position / 64U;
    const std::uint32_t bit = position % 64U;
    words[word] |= std::uint64_t{1} << bit;
}

void ColumnFecDecoder::clip_source(
    Group& group,
    const PacketView& packet,
    std::uint32_t position) noexcept
{
    group.flags_recovery ^=
        encryption_flag(
            packet.data.encryption_key);
    group.length_recovery ^=
        static_cast<std::uint16_t>(
            packet.payload.size());
    group.timestamp_recovery ^=
        packet.data.timestamp.value();
    group.missing_position_xor ^= position;
    group.destination_socket_id =
        packet.data.destination_socket_id;
    if (!group.has_message_anchor) {
        group.message_anchor_sequence = packet.data.sequence;
        group.message_anchor_number = packet.data.message_number;
        group.has_message_anchor = true;
    }
    group.in_order = packet.data.in_order;
    xor_payload(
        recovery_payload(group),
        packet.payload);
    ++group.collected;
}

Error ColumnFecDecoder::clip_control(
    Group& group,
    const PacketView& packet,
    const FecControlDecodeResult& control) noexcept
{
    if (control.header.group_index < 0
        || static_cast<std::uint32_t>(
               control.header.group_index)
            != group.column
        || control.payload_recovery.size()
            != maximum_payload_size_) {
        return Error::invalid_control_payload;
    }
    if (group.has_control) {
        return Error::none;
    }
    group.flags_recovery ^=
        control.header.flags_recovery;
    group.length_recovery ^=
        control.header.length_recovery;
    group.timestamp_recovery ^=
        packet.data.timestamp.value();
    group.destination_socket_id =
        packet.data.destination_socket_id;
    xor_payload(
        recovery_payload(group),
        control.payload_recovery);
    group.has_control = true;
    return Error::none;
}

ColumnFecReceiveResult
ColumnFecDecoder::try_reconstruct(
    Group& group,
    const Location& location) noexcept
{
    if (!group.has_control
        || group.reconstructed
        || group.dismissed
        || group.collected != rows_ - 1U) {
        return {};
    }
    if (group.missing_position_xor >= rows_
        || group.length_recovery > maximum_payload_size_
        || group.flags_recovery > 3U || !group.has_message_anchor
        || group.message_anchor_number == 0U) {
        return {
            .error =
                Error::invalid_control_payload};
    }
    const std::uint64_t missing_index =
        location.base_index
        + static_cast<std::uint64_t>(
              group.missing_position_xor)
            * columns_;
    const auto payload = recovery_payload(group);
    std::copy_n(
        payload.begin(), group.length_recovery,
        reconstructed_payload_.begin());
    mark_source_seen(
        group, group.missing_position_xor);
    ++group.collected;
    group.reconstructed = true;
    const SequenceNumber sequence = initial_sequence_.advanced(
        static_cast<std::uint32_t>(missing_index & SequenceNumber::mask));
    const std::uint32_t message_number =
        advance_message_number(group.message_anchor_number,
            sequence.distance_from(group.message_anchor_sequence));
    return {
        .has_reconstructed_packet = true,
        .reconstructed_packet =
            {
                .kind = PacketKind::data,
                .data =
                    {
                        .sequence = sequence,
                        .message_number = message_number,
                        .boundary = MessageBoundary::solo,
                        .in_order = group.in_order,
                        .encryption_key = encryption_key(group.flags_recovery),
                        .retransmitted = true,
                        .timestamp = PacketTimestamp {group.timestamp_recovery},
                        .destination_socket_id = group.destination_socket_id,
                    },
                .payload = std::span {reconstructed_payload_}.first(
                    group.length_recovery),
            },
    };
}

void ColumnFecDecoder::append_loss_index(
    std::uint64_t index) noexcept
{
    if (loss_index_count_
        == loss_indices_.size()) {
        loss_overflow_ = true;
        return;
    }
    loss_indices_[loss_index_count_++] = index;
}

void ColumnFecDecoder::expire_group(
    std::uint64_t series,
    std::uint32_t column) noexcept
{
    const Location location{
        .series = series,
        .column = column,
        .base_index =
            first_base(column)
            + series * matrix_size_,
    };
    Group* group = group_for(location);
    if (group == nullptr || group->dismissed) {
        return;
    }
    if (arq_ == PacketFilterArqLevel::on_request) {
        for (std::uint32_t position = 0;
             position < rows_; ++position) {
            if (!source_was_seen(
                    *group, position)) {
                append_loss_index(
                    location.base_index
                    + static_cast<std::uint64_t>(
                          position)
                        * columns_);
            }
        }
    }
    group->dismissed = true;
}

void ColumnFecDecoder::advance_expiry(
    std::uint64_t current_series,
    std::uint64_t current_index) noexcept
{
    for (std::uint64_t series =
             minimum_retained_series_;
         series < current_series; ++series) {
        for (std::uint32_t column = 0;
             column < columns_; ++column) {
            const std::uint64_t last =
                first_base(column)
                + series * matrix_size_
                + static_cast<std::uint64_t>(
                      rows_ - 1U)
                    * columns_;
            if (last <= current_index) {
                expire_group(series, column);
            }
        }
    }

    while (minimum_retained_series_
        < current_series) {
        bool all_dismissed = true;
        for (std::uint32_t column = 0;
             column < columns_; ++column) {
            const Group* group = find_group(
                minimum_retained_series_,
                column);
            if (group == nullptr
                || !group->dismissed) {
                all_dismissed = false;
                break;
            }
        }
        if (!all_dismissed) {
            break;
        }
        ++minimum_retained_series_;
    }
}

void ColumnFecDecoder::sort_loss_indices() noexcept
{
    const auto sift_down =
        [this](std::size_t root,
            std::size_t count) noexcept {
            if (count < 2U) {
                return;
            }
            while (root
                <= (count - 2U) / 2U) {
                std::size_t child =
                    root * 2U + 1U;
                if (child + 1U < count
                    && loss_indices_[child]
                        < loss_indices_[child + 1U]) {
                    ++child;
                }
                if (loss_indices_[root]
                    >= loss_indices_[child]) {
                    return;
                }
                std::swap(
                    loss_indices_[root],
                    loss_indices_[child]);
                root = child;
            }
        };
    if (loss_index_count_ < 2U) {
        return;
    }
    for (std::size_t start =
             loss_index_count_ / 2U;
         start != 0U; --start) {
        sift_down(start - 1U, loss_index_count_);
    }
    for (std::size_t end = loss_index_count_;
         end > 1U; --end) {
        std::swap(
            loss_indices_[0],
            loss_indices_[end - 1U]);
        sift_down(0U, end - 1U);
    }
}

void ColumnFecDecoder::build_loss_ranges() noexcept
{
    irrecoverable_loss_count_ = 0;
    sort_loss_indices();
    std::uint64_t previous = 0;
    bool has_previous = false;
    for (std::size_t index = 0;
         index < loss_index_count_; ++index) {
        const std::uint64_t loss =
            loss_indices_[index];
        if (has_previous && loss == previous) {
            continue;
        }
        const SequenceNumber sequence =
            initial_sequence_.advanced(
                static_cast<std::uint32_t>(
                    loss & SequenceNumber::mask));
        if (has_previous
            && previous
                != std::numeric_limits<
                    std::uint64_t>::max()
            && loss == previous + 1U
            && irrecoverable_loss_count_ != 0U) {
            irrecoverable_losses_[
                irrecoverable_loss_count_ - 1U]
                .last = sequence;
        } else {
            if (irrecoverable_loss_count_
                == irrecoverable_losses_.size()) {
                loss_overflow_ = true;
                return;
            }
            irrecoverable_losses_[
                irrecoverable_loss_count_++] = {
                    .first = sequence,
                    .last = sequence,
                };
        }
        previous = loss;
        has_previous = true;
    }
}

ColumnFecReceiveResult
ColumnFecDecoder::finish_result(
    ColumnFecReceiveResult result) noexcept
{
    if (!loss_overflow_) {
        build_loss_ranges();
    }
    if (loss_overflow_) {
        result.error = Error::buffer_too_small;
        return result;
    }
    result.irrecoverable_losses =
        std::span{irrecoverable_losses_}.first(
            irrecoverable_loss_count_);
    return result;
}

ColumnFecReceiveResult ColumnFecDecoder::receive(
    const PacketView& wire_packet) noexcept
{
    loss_index_count_ = 0;
    irrecoverable_loss_count_ = 0;
    loss_overflow_ = false;
    if (wire_packet.kind != PacketKind::data) {
        return {
            .error = Error::invalid_packet_type};
    }

    if (wire_packet.data.message_number == 0U) {
        const auto control =
            decode_fec_control_payload(
                wire_packet.payload);
        if (!control
            || control.header.group_index < 0
            || static_cast<std::uint32_t>(
                   control.header.group_index)
                >= columns_
            || control.payload_recovery.size()
                != maximum_payload_size_) {
            return {
                .error =
                    Error::invalid_control_payload,
                .consume_control_packet = true,
            };
        }
        const auto index = unwrap(
            wire_packet.data.sequence);
        if (!index.has_value()) {
            return {
                .error =
                    Error::invalid_control_payload,
                .consume_control_packet = true,
            };
        }
        const auto location = locate_control(
            *index,
            static_cast<std::uint32_t>(
                control.header.group_index));
        if (!location.has_value()) {
            return {
                .error =
                    Error::invalid_control_payload,
                .consume_control_packet = true,
            };
        }
        if (location->series
            < minimum_retained_series_) {
            return finish_result({
                .consume_control_packet = true});
        }
        Group* group = group_for(*location);
        if (group == nullptr) {
            return finish_result({
                .consume_control_packet = true});
        }
        if (!group->dismissed) {
            const Error error = clip_control(
                *group, wire_packet, control);
            if (error != Error::none) {
                return {
                    .error = error,
                    .consume_control_packet = true,
                };
            }
        }
        auto result =
            try_reconstruct(*group, *location);
        result.consume_control_packet = true;
        advance_expiry(
            location->series, *index);
        return finish_result(result);
    }

    if (wire_packet.payload.size()
        > maximum_payload_size_) {
        return {
            .error = Error::invalid_packet_type};
    }
    const auto index = unwrap(
        wire_packet.data.sequence);
    if (!index.has_value()) {
        return finish_result();
    }
    if (latest_index_ > *index
        && latest_index_ - *index
            >= receive_capacity_packets_) {
        return finish_result();
    }
    const auto location =
        locate_source(*index);
    if (!location.has_value()
        || location->series
            < minimum_retained_series_) {
        return finish_result();
    }
    Group* group = group_for(*location);
    if (group == nullptr) {
        return finish_result();
    }
    if (group->dismissed
        || source_was_seen(
            *group, location->position)) {
        return finish_result();
    }
    mark_source_seen(
        *group, location->position);
    clip_source(
        *group, wire_packet,
        location->position);
    auto result =
        try_reconstruct(*group, *location);
    advance_expiry(
        location->series, *index);
    return finish_result(result);
}

MatrixFecEncoder::MatrixFecEncoder(
    const PacketFilterConfiguration& configuration,
    SequenceNumber initial_sequence,
    std::size_t maximum_payload_size)
    : row_(
          matrix_row_configuration(configuration),
          initial_sequence, maximum_payload_size)
    , column_(
          matrix_column_configuration(configuration),
          initial_sequence, maximum_payload_size)
{
}

Error MatrixFecEncoder::feed_source(
    const PacketView& wire_packet) noexcept
{
    if (control_packet_ready()) {
        return Error::would_block;
    }
    const Error row_error =
        row_.feed_source(wire_packet);
    if (row_error != Error::none) {
        return row_error;
    }
    return column_.feed_source(wire_packet);
}

bool MatrixFecEncoder::control_packet_ready() const noexcept
{
    return column_.control_packet_ready()
        || row_.control_packet_ready();
}

std::optional<OutboundPacket>
MatrixFecEncoder::control_packet() const noexcept
{
    if (column_.control_packet_ready()) {
        return column_.control_packet();
    }
    return row_.control_packet();
}

void MatrixFecEncoder::consume_control_packet() noexcept
{
    if (column_.control_packet_ready()) {
        column_.consume_control_packet();
    } else {
        row_.consume_control_packet();
    }
}

MatrixFecDecoder::MatrixFecDecoder(
    const PacketFilterConfiguration& configuration,
    SequenceNumber initial_sequence, std::size_t receive_capacity_packets,
    std::size_t maximum_payload_size)
    : MatrixFecDecoder(require_resources(configuration,
                           receive_capacity_packets, maximum_payload_size),
          configuration, initial_sequence, receive_capacity_packets,
          maximum_payload_size)
{
}

MatrixFecDecoder::MatrixFecDecoder(ValidatedResources,
    const PacketFilterConfiguration& configuration,
    SequenceNumber initial_sequence, std::size_t receive_capacity_packets,
    std::size_t maximum_payload_size)
    : row_(matrix_row_configuration(configuration), initial_sequence,
          receive_capacity_packets, maximum_payload_size, false)
    , column_(matrix_column_configuration(configuration), initial_sequence,
          receive_capacity_packets, maximum_payload_size)
    , maximum_payload_size_(maximum_payload_size)
{
    reconstructed_packets_.resize(
        receive_capacity_packets);
    reconstructed_payloads_.resize(
        receive_capacity_packets
        * maximum_payload_size_);
    irrecoverable_losses_.resize(
        receive_capacity_packets * 2U);
}

FecResourceEstimate MatrixFecDecoder::estimate_resources(
    const PacketFilterConfiguration& configuration,
    std::size_t receive_capacity_packets,
    std::size_t maximum_payload_size) noexcept
{
    if (!supports_matrix_fec(configuration) || receive_capacity_packets == 0U
        || receive_capacity_packets >= SequenceNumber::half_range
        || maximum_payload_size == 0U
        || maximum_payload_size
            > maximum_data_payload_size - fec_filter_header_size) {
        return {.error = Error::invalid_state};
    }
    std::size_t matrix_size = 0;
    if (!checked_multiply(static_cast<std::size_t>(configuration.columns),
            static_cast<std::size_t>(configuration.rows), matrix_size)
        || matrix_size > receive_capacity_packets) {
        return {.error = Error::invalid_state};
    }

    auto row_configuration = configuration;
    row_configuration.rows = 1;
    row_configuration.rows_specified = true;
    row_configuration.arq = PacketFilterArqLevel::never;
    row_configuration.arq_specified = true;
    auto column_configuration = configuration;
    column_configuration.rows = -configuration.rows;
    column_configuration.rows_specified = true;
    const auto row = RowFecDecoder::estimate_resources(
        row_configuration, receive_capacity_packets, maximum_payload_size);
    const auto column = ColumnFecDecoder::estimate_resources(
        column_configuration, receive_capacity_packets, maximum_payload_size);
    if (!row) {
        return {.error = row.error};
    }
    if (!column) {
        return {.error = column.error};
    }

    FecResourceUsage usage {};
    std::size_t loss_count = 0;
    if (!append_resource_usage(usage, row.usage)
        || !append_resource_usage(usage, column.usage)
        || !assign_resource_bytes(receive_capacity_packets, sizeof(PacketView),
            usage.reconstruction_bytes, usage.dynamic_bytes)) {
        return {.error = Error::buffer_too_small};
    }
    std::size_t payload_bytes = 0;
    std::size_t next_reconstruction = 0;
    if (!checked_multiply(
            receive_capacity_packets, maximum_payload_size, payload_bytes)
        || !checked_add(
            usage.reconstruction_bytes, payload_bytes, next_reconstruction)
        || !checked_add(usage.dynamic_bytes, payload_bytes, usage.dynamic_bytes)
        || !checked_multiply(receive_capacity_packets, 2U, loss_count)) {
        return {.error = Error::buffer_too_small};
    }
    usage.reconstruction_bytes = next_reconstruction;
    std::size_t matrix_loss_bytes = 0;
    std::size_t next_loss_bytes = 0;
    if (!checked_multiply(loss_count, sizeof(SequenceRange), matrix_loss_bytes)
        || !checked_add(
            usage.loss_tracking_bytes, matrix_loss_bytes, next_loss_bytes)
        || !checked_add(
            usage.dynamic_bytes, matrix_loss_bytes, usage.dynamic_bytes)) {
        return {.error = Error::buffer_too_small};
    }
    usage.loss_tracking_bytes = next_loss_bytes;
    return {.usage = usage};
}

MatrixFecDecoder::ValidatedResources MatrixFecDecoder::require_resources(
    const PacketFilterConfiguration& configuration,
    std::size_t receive_capacity_packets, std::size_t maximum_payload_size)
{
    const auto estimate = estimate_resources(
        configuration, receive_capacity_packets, maximum_payload_size);
    if (!estimate) {
        throw std::invalid_argument("invalid matrix FEC decoder configuration");
    }
    return {};
}

FecResourceUsage MatrixFecDecoder::resource_usage() const noexcept
{
    FecResourceUsage usage {};
    const auto row = row_.resource_usage();
    const auto column = column_.resource_usage();
    if (!append_resource_usage(usage, row)
        || !append_resource_usage(usage, column)) {
        usage.dynamic_bytes = std::numeric_limits<std::size_t>::max();
        return usage;
    }
    const auto own = measured_usage(0U, 0U, 0U, 0U,
        irrecoverable_losses_.capacity() * sizeof(SequenceRange),
        reconstructed_packets_.capacity() * sizeof(PacketView)
            + reconstructed_payloads_.capacity() * sizeof(std::byte));
    if (!append_resource_usage(usage, own)) {
        usage.dynamic_bytes = std::numeric_limits<std::size_t>::max();
    }
    return usage;
}

Error MatrixFecDecoder::append_losses(
    std::span<const SequenceRange> losses) noexcept
{
    for (const auto& loss : losses) {
        if (irrecoverable_loss_count_ != 0U) {
            auto& previous = irrecoverable_losses_[
                irrecoverable_loss_count_ - 1U];
            if (previous.first == loss.first
                && previous.last == loss.last) {
                continue;
            }
            if (previous.last.next() == loss.first) {
                previous.last = loss.last;
                continue;
            }
        }
        if (irrecoverable_loss_count_
            == irrecoverable_losses_.size()) {
            return Error::buffer_too_small;
        }
        irrecoverable_losses_[
            irrecoverable_loss_count_++] = loss;
    }
    return Error::none;
}

Error MatrixFecDecoder::append_reconstructed(
    const PacketView& packet) noexcept
{
    if (packet.kind != PacketKind::data
        || packet.payload.size()
            > maximum_payload_size_
        || reconstructed_packet_count_
            == reconstructed_packets_.size()) {
        return Error::buffer_too_small;
    }
    const std::size_t index =
        reconstructed_packet_count_++;
    auto payload =
        std::span{reconstructed_payloads_}.subspan(
            index * maximum_payload_size_,
            maximum_payload_size_);
    std::copy(
        packet.payload.begin(), packet.payload.end(),
        payload.begin());
    reconstructed_packets_[index] = packet;
    reconstructed_packets_[index].payload =
        payload.first(packet.payload.size());
    return Error::none;
}

Error MatrixFecDecoder::drain_reconstruction_chain(
    const RowFecReceiveResult& first,
    Dimension source_dimension) noexcept
{
    RowFecReceiveResult current = first;
    Dimension source = source_dimension;
    while (true) {
        if (!current) {
            return current.error;
        }
        if (source == Dimension::column) {
            const Error loss_error =
                append_losses(
                    current.irrecoverable_losses);
            if (loss_error != Error::none) {
                return loss_error;
            }
        }
        if (!current.has_reconstructed_packet) {
            return Error::none;
        }
        if (current_source_sequence_.has_value()
            && current.reconstructed_packet
                    .data.sequence
                == *current_source_sequence_) {
            // The physical source was deliberately fed to the first
            // dimension before the recovery chain started. If the other
            // dimension reconstructs that same source before its normal
            // feed, both dimensions already contain it and the synthetic
            // duplicate must not escape to reliability processing.
            return Error::none;
        }
        const Error append_error =
            append_reconstructed(
                current.reconstructed_packet);
        if (append_error != Error::none) {
            return append_error;
        }
        const PacketView rebuilt =
            reconstructed_packets_[
                reconstructed_packet_count_ - 1U];
        if (source == Dimension::row) {
            current = column_.receive(rebuilt);
            source = Dimension::column;
        } else {
            current = row_.receive(rebuilt);
            source = Dimension::row;
        }
    }
}

MatrixFecReceiveResult MatrixFecDecoder::finish_result(
    bool consume_control_packet) noexcept
{
    return {
        .error = error_,
        .consume_control_packet =
            consume_control_packet,
        .reconstructed_packets =
            std::span{reconstructed_packets_}.first(
                reconstructed_packet_count_),
        .irrecoverable_losses =
            std::span{irrecoverable_losses_}.first(
                irrecoverable_loss_count_),
    };
}

MatrixFecReceiveResult MatrixFecDecoder::receive(
    const PacketView& wire_packet) noexcept
{
    reconstructed_packet_count_ = 0;
    irrecoverable_loss_count_ = 0;
    error_ = Error::none;
    current_source_sequence_.reset();
    if (wire_packet.kind != PacketKind::data) {
        error_ = Error::invalid_packet_type;
        return finish_result(false);
    }

    if (wire_packet.data.message_number == 0U) {
        const auto control =
            decode_fec_control_payload(
                wire_packet.payload);
        if (!control) {
            error_ =
                Error::invalid_control_payload;
            return finish_result(true);
        }
        if (control.header.group_index == -1) {
            error_ = drain_reconstruction_chain(
                row_.receive(wire_packet),
                Dimension::row);
        } else if (control.header.group_index >= 0) {
            error_ = drain_reconstruction_chain(
                column_.receive(wire_packet),
                Dimension::column);
        } else {
            error_ =
                Error::invalid_control_payload;
        }
        return finish_result(true);
    }

    current_source_sequence_ =
        wire_packet.data.sequence;
    error_ = drain_reconstruction_chain(
        row_.receive(wire_packet),
        Dimension::row);
    if (error_ != Error::none) {
        return finish_result(false);
    }
    error_ = drain_reconstruction_chain(
        column_.receive(wire_packet),
        Dimension::column);
    return finish_result(false);
}

} // namespace robotweax::srt
