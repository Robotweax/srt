#include "robotweax/srt/session.hpp"

#include "robotweax/srt/codec.hpp"

#include <algorithm>
#include <limits>

namespace robotweax::srt {
namespace {

struct NakRangeSlices {
    Error error = Error::none;
    std::optional<SequenceRange> stale;
    std::optional<SequenceRange> current;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return error == Error::none;
    }
};

[[nodiscard]] NakRangeSlices split_nak_range(
    const SendBuffer& send_buffer, SequenceRange range) noexcept
{
    if (range.last.distance_from(range.first) < 0) {
        return {
            .error = Error::invalid_control_payload,
            .stale = std::nullopt,
            .current = std::nullopt,
        };
    }

    const SequenceNumber first = send_buffer.first_sequence();
    const std::int32_t first_offset = range.first.distance_from(first);
    const std::int32_t last_offset = range.last.distance_from(first);
    const std::size_t span = send_buffer.sequence_span();

    if (first_offset >= 0) {
        if (last_offset < first_offset
            || static_cast<std::size_t>(last_offset) >= span) {
            return {
                .error = Error::invalid_control_payload,
                .stale = std::nullopt,
                .current = std::nullopt,
            };
        }
        return {
            .error = Error::none,
            .stale = std::nullopt,
            .current = range,
        };
    }
    if (last_offset < 0) {
        return {
            .error = Error::none,
            .stale = range,
            .current = std::nullopt,
        };
    }
    if (static_cast<std::size_t>(last_offset) >= span) {
        return {
            .error = Error::invalid_control_payload,
            .stale = std::nullopt,
            .current = std::nullopt,
        };
    }
    return {
        .error = Error::none,
        .stale =
            SequenceRange {
                .first = range.first,
                .last = first.advanced(SequenceNumber::mask),
            },
        .current =
            SequenceRange {
                .first = first,
                .last = range.last,
            },
    };
}

} // namespace

ReliabilitySession::ReliabilitySession(Configuration configuration)
    : send_buffer_(configuration.local_initial_sequence,
        configuration.send_capacity_packets, configuration.maximum_payload_size)
    , receive_buffer_(configuration.peer_initial_sequence,
        configuration.receive_capacity_packets)
    , receive_loss_list_(configuration.receive_capacity_packets)
    , filter_loss_list_(configuration.receive_capacity_packets)
    , timer_scheduler_(configuration.start_microseconds)
    , sender_retransmission_timer_(configuration.start_microseconds)
    , highest_received_sequence_(
          configuration.peer_initial_sequence.advanced(
              SequenceNumber::mask))
    , peer_socket_id_(configuration.peer_socket_id)
{
    experimental_recovery_reserve_ =
        configuration.experimental_recovery_reserve;
    experimental_recovery_budget_ = configuration.experimental_recovery_budget;
}

Error ReliabilitySession::queue_message(std::span<const std::byte> message,
    PacketTimestamp timestamp, bool in_order,
    std::uint64_t enqueue_microseconds,
    std::uint64_t expiration_microseconds) noexcept
{
    const auto error = send_buffer_.enqueue_message(message,
        next_message_number_, timestamp, peer_socket_id_, in_order,
        enqueue_microseconds, expiration_microseconds);
    if (error == Error::none) {
        if (live_rate_controller_.has_value()) {
            live_rate_controller_->observe_input(
                message.size(), enqueue_microseconds);
        }
        next_message_number_ = (next_message_number_ + 1U) & 0x03ff'ffffU;
        if (next_message_number_ == 0U) {
            next_message_number_ = 1U;
        }
    }
    return error;
}

Error ReliabilitySession::queue_group_message(
    std::span<const std::byte> message,
    SequenceNumber first_sequence,
    std::uint32_t message_number,
    PacketTimestamp timestamp, bool in_order,
    std::uint64_t enqueue_microseconds,
    std::uint64_t expiration_microseconds) noexcept
{
    const std::uint32_t normalized_message =
        message_number & 0x03ff'ffffU;
    const bool sender_empty = send_buffer_.sequence_span() == 0U;
    if (normalized_message == 0U
        || !send_buffer_.synchronize_empty(first_sequence)
        || send_buffer_.next_sequence() != first_sequence) {
        return Error::invalid_state;
    }
    if (sender_empty) {
        next_message_number_ = normalized_message;
    } else if (next_message_number_ != normalized_message) {
        return Error::invalid_state;
    }
    const Error error = send_buffer_.enqueue_message(message,
        normalized_message, timestamp, peer_socket_id_, in_order,
        enqueue_microseconds, expiration_microseconds);
    if (error == Error::none) {
        if (live_rate_controller_.has_value()) {
            live_rate_controller_->observe_input(
                message.size(), enqueue_microseconds);
        }
        next_message_number_ =
            (normalized_message + 1U) & 0x03ff'ffffU;
        if (next_message_number_ == 0U) {
            next_message_number_ = 1U;
        }
    }
    return error;
}

Error ReliabilitySession::skip_group_sequences(
    SequenceNumber next_sequence) noexcept
{
    const SequenceNumber current = send_buffer_.next_sequence();
    const std::int32_t distance = next_sequence.distance_from(current);
    if (distance < 0 || send_buffer_.sequence_span() != 0U) {
        return Error::invalid_state;
    }
    if (distance == 0) {
        return Error::none;
    }
    const std::array skipped {
        SequenceRange {
            .first = current,
            .last = next_sequence.advanced(SequenceNumber::mask),
        },
    };
    if (!send_buffer_.queue_range_drop_requests(skipped)) {
        return Error::buffer_too_small;
    }
    return send_buffer_.synchronize_empty(next_sequence) ? Error::none
                                                         : Error::invalid_state;
}

StreamEnqueueResult ReliabilitySession::queue_stream(
    std::span<const std::byte> bytes,
    PacketTimestamp timestamp,
    std::uint64_t enqueue_microseconds) noexcept
{
    auto result = send_buffer_.enqueue_stream(bytes,
        next_message_number_, timestamp, peer_socket_id_,
        enqueue_microseconds);
    if (result) {
        if (live_rate_controller_.has_value()) {
            live_rate_controller_->observe_input(
                result.bytes_accepted, enqueue_microseconds);
        }
        next_message_number_ =
            (next_message_number_ + 1U) & 0x03ff'ffffU;
        if (next_message_number_ == 0U) {
            next_message_number_ = 1U;
        }
    }
    return result;
}

std::optional<OutboundPacket> ReliabilitySession::next_paced_data_packet(
    PacketPacer& pacer, std::uint64_t now_microseconds,
    std::size_t new_packet_wire_overhead) noexcept
{
    if (live_rate_controller_.has_value()) {
        pacer.set_rate(live_rate_controller_->pacing_rate_bytes_per_second());
    } else if (file_rate_controller_.has_value()) {
        pacer.set_rate(file_rate_controller_
                ->pacing_rate_bytes_per_second());
        pacer.set_flow_window(file_rate_controller_
                ->congestion_window_packets());
    }
    const bool retransmission = send_buffer_.has_pending_retransmission();
    const std::size_t flow_count = retransmission
        ? 0U
        : send_buffer_.packets_in_flight();
    if (!pacer.query(now_microseconds, flow_count).ready) {
        return std::nullopt;
    }
    auto packet = send_buffer_.next_packet();
    if (packet.has_value()) {
        if (live_rate_controller_.has_value()) {
            live_rate_controller_->observe_payload(packet->payload.size());
        }
        const std::size_t wire_overhead =
            retransmission ? 0U : new_packet_wire_overhead;
        pacer.on_packet_sent(
            packet_header_size + packet->payload.size() + wire_overhead,
            now_microseconds);
    }
    return packet;
}

void ReliabilitySession::configure_live(const NegotiatedLiveOptions& options,
    std::uint64_t handshake_arrival_microseconds,
    PacketTimestamp peer_handshake_timestamp,
    LiveRateController::Configuration rate_configuration) noexcept
{
    file_rate_controller_.reset();
    live_options_ = options;
    periodic_nak_enabled_ = options.periodic_nak;
    if (options.receive_tsbpd) {
        enable_tsbpd(handshake_arrival_microseconds, peer_handshake_timestamp,
            static_cast<std::uint32_t>(options.receive_delay_milliseconds) * 1'000U);
    } else {
        tsbpd_clock_.reset();
    }
    live_rate_controller_.emplace(rate_configuration);
    if (options.too_late_packet_drop) {
        constexpr std::uint32_t minimum_drop_threshold_microseconds = 1'020'000;
        const auto negotiated = static_cast<std::uint32_t>(
            options.peer_receive_delay_milliseconds) * 1'000U + 20'000U;
        sender_drop_threshold_microseconds_ = std::max(
            negotiated, minimum_drop_threshold_microseconds);
    } else {
        sender_drop_threshold_microseconds_ = 0;
    }
}

void ReliabilitySession::configure_file(
    bool message_api,
    FileRateController::Configuration rate_configuration) noexcept
{
    live_rate_controller_.reset();
    file_rate_controller_.emplace(
        send_buffer_.first_sequence(), rate_configuration);
    tsbpd_clock_.reset();
    live_options_ = {};
    sender_drop_threshold_microseconds_ = 0;
    periodic_nak_enabled_ = false;
    message_api_ = message_api;
}

Error ReliabilitySession::apply_dynamic_options(const SocketOptions& options) noexcept
{
    const auto maximum_reorder_tolerance = options.get(
        SocketOption::maximum_reorder_tolerance_packets);
    if (!maximum_reorder_tolerance) {
        return maximum_reorder_tolerance.error;
    }
    const auto configured_reorder_tolerance =
        static_cast<std::uint32_t>(
            maximum_reorder_tolerance.value);
    if (options.reorder_tolerance_revision()
            != reorder_tolerance_revision_
        || configured_reorder_tolerance
            != maximum_reorder_tolerance_packets_) {
        set_maximum_reorder_tolerance(
            configured_reorder_tolerance);
        reorder_tolerance_revision_ =
            options.reorder_tolerance_revision();
    }

    if (file_rate_controller_.has_value()) {
        const auto maximum = options.get(
            SocketOption::maximum_bandwidth_bytes_per_second);
        file_rate_controller_->set_maximum_bandwidth(
            maximum.value > 0
            ? static_cast<std::uint64_t>(maximum.value)
            : 0U);
        return Error::none;
    }
    if (!live_rate_controller_.has_value()) {
        return Error::invalid_state;
    }
    const auto input = options.get(SocketOption::input_bandwidth_bytes_per_second);
    const auto minimum_input = options.get(
        SocketOption::minimum_input_bandwidth_bytes_per_second);
    const auto maximum = options.get(SocketOption::maximum_bandwidth_bytes_per_second);
    const auto overhead = options.get(SocketOption::overhead_bandwidth_percent);
    const auto drop_delay = options.get(SocketOption::sender_drop_delay_milliseconds);
    live_rate_controller_->set_input_bandwidth(
        static_cast<std::uint64_t>(input.value));
    live_rate_controller_->set_minimum_input_bandwidth(
        static_cast<std::uint64_t>(minimum_input.value));
    live_rate_controller_->set_maximum_bandwidth(
        maximum.value);
    live_rate_controller_->set_overhead_percent(
        static_cast<std::uint32_t>(overhead.value));
    const auto drift_tracer = options.get(
        SocketOption::drift_tracer);
    drift_tracer_enabled_ = drift_tracer.value != 0;

    if (!live_options_.too_late_packet_drop || drop_delay.value < 0) {
        sender_drop_threshold_microseconds_ = 0;
    } else {
        constexpr std::uint32_t minimum_drop_threshold_microseconds = 1'020'000;
        const auto configured = (static_cast<std::uint64_t>(
            live_options_.peer_receive_delay_milliseconds)
            + static_cast<std::uint64_t>(drop_delay.value)) * 1'000ULL + 20'000ULL;
        sender_drop_threshold_microseconds_ = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(std::max<std::uint64_t>(configured,
                minimum_drop_threshold_microseconds),
                std::numeric_limits<std::uint32_t>::max()));
    }
    return Error::none;
}

void ReliabilitySession::set_maximum_reorder_tolerance(
    std::uint32_t packets) noexcept
{
    maximum_reorder_tolerance_packets_ = packets;
    reorder_tolerance_packets_ = std::min(
        reorder_tolerance_packets_, packets);
}

void ReliabilitySession::note_reordered_packet(
    std::size_t distance_packets,
    std::uint32_t remaining_ttl) noexcept
{
    reorder_distance_packets_ = std::max(
        reorder_distance_packets_, distance_packets);

    bool increased_tolerance = false;
    if (distance_packets > reorder_tolerance_packets_) {
        reorder_tolerance_packets_ =
            static_cast<std::uint32_t>(
                std::min<std::size_t>(
                    distance_packets,
                    maximum_reorder_tolerance_packets_));
        increased_tolerance = true;
    }
    if (reorder_tolerance_packets_ == 0U) {
        return;
    }

    consecutive_ordered_packets_ = 0;
    if (increased_tolerance) {
        consecutive_early_reorders_ = 0;
        return;
    }
    if (remaining_ttl <= 2U) {
        return;
    }
    ++consecutive_early_reorders_;
    if (consecutive_early_reorders_ < 10U) {
        return;
    }

    consecutive_early_reorders_ = 0;
    --reorder_tolerance_packets_;
    if (reorder_distance_packets_ != 0U) {
        --reorder_distance_packets_;
    }
}

void ReliabilitySession::note_ordered_packet() noexcept
{
    ++consecutive_ordered_packets_;
    if (consecutive_ordered_packets_ < 2000U) {
        return;
    }
    consecutive_ordered_packets_ = 0;
    if (reorder_tolerance_packets_ == 0U) {
        return;
    }
    --reorder_tolerance_packets_;
    if (reorder_distance_packets_ != 0U) {
        --reorder_distance_packets_;
    }
}

void ReliabilitySession::append_pending_loss_report(
    ReliabilityActions& actions,
    bool action_slot_available) noexcept
{
    if (!action_slot_available
        || actions.size == actions.values.size()) {
        return;
    }
    ReceiveLossList* losses = nullptr;
    switch (packet_filter_policy_.effective_arq_level()) {
    case PacketFilterArqLevel::always:
        losses = &receive_loss_list_;
        break;
    case PacketFilterArqLevel::on_request:
        losses = &filter_loss_list_;
        break;
    case PacketFilterArqLevel::never:
        return;
    }
    const std::size_t range_count =
        losses->take_pending_reports(
            actions.available_loss_range_storage());
    if (range_count != 0U) {
        actions.commit_loss_report(range_count);
    }
}

void ReliabilitySession::append_pending_drop_requests(
    ReliabilityActions& actions) noexcept
{
    while (actions.size < actions.values.size()) {
        const auto dropped = send_buffer_.next_pending_drop_request();
        if (!dropped.has_value()) {
            break;
        }
        actions.push({
            .kind = ReliabilityActionKind::drop_request,
            .drop =
                {
                    .message_number = dropped->first_message_number,
                    .sequences = dropped->sequences,
                },
        });
    }
}

void ReliabilitySession::update_loss_timer(
    std::uint64_t now_microseconds) noexcept
{
    bool losses_outstanding = false;
    switch (packet_filter_policy_.effective_arq_level()) {
    case PacketFilterArqLevel::always:
        losses_outstanding = !receive_loss_list_.empty();
        break;
    case PacketFilterArqLevel::on_request:
        losses_outstanding = !filter_loss_list_.empty();
        break;
    case PacketFilterArqLevel::never:
        break;
    }
    timer_scheduler_.set_loss_state(
        losses_outstanding && periodic_nak_enabled_,
        now_microseconds,
        rtt_.nak_interval_microseconds(),
        true);
}

ReliabilityAction ReliabilitySession::make_acknowledgement(
    std::uint64_t now_microseconds, AcknowledgementKind kind) noexcept
{
    Acknowledgement acknowledgement;
    acknowledgement.kind = kind;
    acknowledgement.next_sequence = receive_buffer_.next_ack_sequence();
    acknowledgement.round_trip_time_microseconds = rtt_.smoothed_microseconds();
    acknowledgement.round_trip_time_variance_microseconds = rtt_.variation_microseconds();
    acknowledgement.available_receive_buffer_packets =
        static_cast<std::uint32_t>(receive_buffer_.available());
    const auto rates = arrival_rate_estimator_.rates();
    if (rates.valid) {
        acknowledgement.receive_rate_packets_per_second = rates.packets_per_second;
        acknowledgement.receive_rate_bytes_per_second = rates.bytes_per_second;
        acknowledgement.estimated_link_capacity_packets_per_second =
            rates.link_capacity_packets_per_second;
        acknowledgement.has_rate_metrics = true;
    }
    if (kind == AcknowledgementKind::full) {
        acknowledgement.acknowledgement_number =
            acknowledgement_numbers_.take();
        acknowledgement_tracker_.record(
            acknowledgement.acknowledgement_number, now_microseconds);
    }
    return {
        .kind = ReliabilityActionKind::acknowledgement,
        .acknowledgement = acknowledgement,
    };
}

ReliabilityAction
ReliabilitySession::make_staged_receive_acknowledgement(
    SequenceNumber next_sequence,
    std::size_t available_receive_buffer_packets,
    std::uint64_t now_microseconds) noexcept
{
    auto action = make_acknowledgement(now_microseconds);
    action.acknowledgement.next_sequence = next_sequence;
    action.acknowledgement.available_receive_buffer_packets =
        static_cast<std::uint32_t>(std::min<std::size_t>(
            available_receive_buffer_packets,
            std::numeric_limits<std::uint32_t>::max()));
    return action;
}

ReliabilityProcessResult ReliabilitySession::receive(
    const PacketView& packet,
    std::uint64_t now_microseconds,
    ReliabilityReceiveContext context) noexcept
{
    ReliabilityProcessResult result;
    if (packet.kind == PacketKind::data) {
        const auto filter_disposition =
            packet_filter_policy_.inspect(packet);
        if (filter_disposition
            == PacketFilterReceiveDisposition::
                invalid_filter_control) {
            return {
                .error =
                    Error::invalid_control_payload};
        }
        if (filter_disposition
            == PacketFilterReceiveDisposition::
                consume_filter_control) {
            result.receiver_filter_control_packet = true;
            return result;
        }
        const std::uint32_t initial_loss_ttl =
            live_options_.retransmit_flag
            ? reorder_tolerance_packets_
            : 0U;
        const std::int32_t physical_distance =
            context.filter_supplied
            ? 0
            : packet.data.sequence.distance_from(
                highest_received_sequence_);
        if (!context.filter_supplied) {
            arrival_rate_estimator_.observe(
                packet.payload.size(), now_microseconds);
            const auto probe_position =
                packet.data.sequence.value() & 0x0fU;
            if (probe_position == 0U) {
                probe_first_sequence_ =
                    packet.data.sequence;
                probe_first_arrival_microseconds_ =
                    now_microseconds;
            } else if (probe_position == 1U
                && probe_first_sequence_.has_value()
                && packet.data.sequence
                    == probe_first_sequence_->next()
                && !packet.data.retransmitted) {
                arrival_rate_estimator_
                    .observe_probe_pair(
                        probe_first_arrival_microseconds_,
                        now_microseconds);
                probe_first_sequence_.reset();
            }
            if (packet.data.retransmitted
                && probe_first_sequence_.has_value()
                && packet.data.sequence
                    == *probe_first_sequence_) {
                probe_first_sequence_.reset();
            }
        }
        const SequenceNumber next_ack_before =
            receive_buffer_.next_ack_sequence();
        const auto inserted = receive_buffer_.insert(packet);
        if (!inserted) {
            return {.error = inserted.error};
        }
        result.receiver_packet_accepted_unique =
            inserted.status == ReceiveStatus::accepted_in_order
            || inserted.status
                == ReceiveStatus::accepted_out_of_order;
        result.receiver_packet_belated =
            inserted.status == ReceiveStatus::older_than_window
            || (inserted.status == ReceiveStatus::duplicate
                && packet.data.sequence.distance_from(
                    next_ack_before) < 0);
        // Clock recovery needs physical, forward-progressing observations.
        // Retransmissions, duplicates, late originals, and FEC reconstructions
        // carry valid source timestamps but biased arrival times.
        if (!context.filter_supplied
            && result.receiver_packet_accepted_unique
            && !packet.data.retransmitted
            && physical_distance > 0) {
            observe_tsbpd_drift(
                packet.data.timestamp, now_microseconds);
        }
        if (result.receiver_packet_belated
            && tsbpd_clock_.has_value()) {
            const std::uint64_t delivery =
                tsbpd_clock_->delivery_time(
                    packet.data.timestamp);
            if (now_microseconds > delivery) {
                result.receiver_belated_delay_microseconds =
                    now_microseconds - delivery;
            }
        }

        ReceiveLossRemoval recovered_loss;
        if (result.receiver_packet_accepted_unique) {
            (void)filter_loss_list_.remove(
                packet.data.sequence);
            if (context.filter_supplied) {
                recovered_loss = receive_loss_list_.remove(
                    packet.data.sequence);
            } else if (physical_distance > 0) {
                if (physical_distance > 1) {
                    result.receiver_loss_packets =
                        static_cast<std::size_t>(
                            physical_distance - 1);
                    const SequenceRange gap{
                        .first =
                            highest_received_sequence_.next(),
                        .last = packet.data.sequence.advanced(
                            SequenceNumber::mask),
                    };
                    // Experimental only: keep the learned tolerance but cap
                    // each fresh gap's wait at 2 ms, independent of DATA flow.
                    constexpr std::uint64_t fresh_wait_us = 2'000;
                    const auto deadline = now_microseconds
                        + std::min(fresh_wait_us,
                            std::numeric_limits<std::uint64_t>::max()
                                - now_microseconds);
                    (void)receive_loss_list_.add(gap, initial_loss_ttl,
                        initial_loss_ttl != 0U ? deadline : 0U);
                }
                highest_received_sequence_ =
                    packet.data.sequence;
            } else {
                recovered_loss = receive_loss_list_.remove(
                    packet.data.sequence);
            }
        }
        const bool reordered =
            !context.filter_supplied
            && physical_distance < 0
            && live_options_.retransmit_flag
            && !packet.data.retransmitted;
        if (reordered) {
            result.receiver_reorder_distance_packets =
                static_cast<std::size_t>(
                    -static_cast<std::int64_t>(
                        physical_distance));
            note_reordered_packet(
                result.receiver_reorder_distance_packets,
                recovered_loss.remaining_ttl);
        }

        has_received_data_ = true;
        timer_scheduler_.on_data_received(now_microseconds);
        if (!context.filter_supplied
            && result.receiver_packet_accepted_unique
            && initial_loss_ttl != 0U) {
            receive_loss_list_.age_fresh();
        }
        constrain_fresh_loss_wait(now_microseconds);
        receive_loss_list_.expire_fresh(now_microseconds);
        if (!context.defer_feedback) {
            append_pending_loss_report(
                result.actions,
                result.actions.size + 1U
                    < result.actions.values.size());
        }
        if (result.receiver_packet_accepted_unique
            && live_options_.retransmit_flag
            && !reordered) {
            note_ordered_packet();
        }
        update_loss_timer(now_microseconds);
        // Live mode uses the control scheduler's 10 ms full-ACK and
        // packet-counted lite-ACK cadence. File and unconfigured sessions
        // retain immediate acknowledgements for their synchronous flow.
        if (!context.defer_feedback
            && !live_rate_controller_.has_value()) {
            result.actions.push(
                make_acknowledgement(now_microseconds));
        }
        return result;
    }

    if (!has_valid_control_payload_shape(packet.payload)) {
        return {.error = Error::invalid_control_payload};
    }

    switch (packet.control.type) {
    case ControlType::acknowledgement: {
        const auto decoded = decode_acknowledgement(packet);
        if (!decoded) {
            return {.error = decoded.error};
        }
        const auto acknowledgement_progress =
            decoded.acknowledgement.next_sequence.distance_from(
                send_buffer_.first_sequence());
        const bool acknowledgement_is_current = acknowledgement_progress >= 0;
        const auto error = send_buffer_.acknowledge_before(
            decoded.acknowledgement.next_sequence);
        if (error != Error::none) {
            return {.error = error};
        }
        // Repeated ACKs can update the receive window while a lost flight
        // tail remains unacknowledged. Resetting RTO on those non-progress
        // ACKs can postpone recovery until the Live delivery deadline expires.
        // Keep filter-controlled recovery and FileCC unchanged: duplicate
        // ACKs can cover a gap that FEC is still reconstructing, not a tail.
        const bool live_tail_recovery = live_rate_controller_.has_value()
            && packet_filter_policy_.effective_arq_level()
                == PacketFilterArqLevel::always;
        if (acknowledgement_progress > 0
            || (acknowledgement_is_current && !live_tail_recovery)) {
            sender_retransmission_timer_.on_acknowledgement_received(
                now_microseconds, send_buffer_.packets_in_flight() != 0U);
        }
        if (decoded.acknowledgement.kind != AcknowledgementKind::lite) {
            if (acknowledgement_is_current) {
                result.peer_available_receive_buffer_packets =
                    decoded.acknowledgement.available_receive_buffer_packets;
            }
            rtt_.observe_peer_estimate(
                decoded.acknowledgement
                    .round_trip_time_microseconds,
                decoded.acknowledgement
                    .round_trip_time_variance_microseconds,
                has_received_data_);
            if (file_rate_controller_.has_value()) {
                file_rate_controller_->on_ack(
                    decoded.acknowledgement.next_sequence,
                    now_microseconds,
                    decoded.acknowledgement
                        .receive_rate_packets_per_second,
                    decoded.acknowledgement
                        .estimated_link_capacity_packets_per_second,
                    decoded.acknowledgement
                        .round_trip_time_microseconds);
            }
            // The Internet Draft reserves ACKACK for Full ACKs and requires
            // Small ACKs to carry a zero type-specific field. Haivision's
            // deployed implementation numbers its 16-byte ACKs, however, and
            // expects those packets to participate in the ACK/ACKACK exchange.
            // A non-zero number therefore identifies that compatible legacy
            // variant without weakening standard Small-ACK emission.
            if (decoded.acknowledgement.kind == AcknowledgementKind::full
                || decoded.acknowledgement.acknowledgement_number != 0U) {
                result.actions.push({
                    .kind = ReliabilityActionKind::acknowledgement_of_ack,
                    .acknowledgement_number =
                        decoded.acknowledgement.acknowledgement_number,
                });
            }
        }
        return result;
    }
    case ControlType::negative_acknowledgement: {
        auto payload = packet.payload;
        if (payload.empty()) {
            return {.error = Error::invalid_control_payload};
        }
        std::array<SequenceRange, maximum_loss_words_per_packet>
            stale_ranges {};
        std::size_t stale_range_count = 0;

        // Validate and classify the complete report first. No retransmission
        // or DROPREQ state changes until every range is known to be safe.
        auto validation_payload = payload;
        while (!validation_payload.empty()) {
            const auto loss = decode_loss_range(validation_payload);
            if (!loss) {
                return {.error = loss.error};
            }
            const auto slices = split_nak_range(send_buffer_, loss.range);
            if (!slices) {
                return {.error = slices.error};
            }
            if (slices.current.has_value()) {
                const Error error =
                    send_buffer_.validate_retransmission_range(*slices.current);
                if (error != Error::none) {
                    return {.error = error};
                }
            }
            if (slices.stale.has_value()) {
                if (stale_range_count == stale_ranges.size()) {
                    return {.error = Error::buffer_too_small};
                }
                stale_ranges[stale_range_count++] = *slices.stale;
            }
            validation_payload =
                validation_payload.subspan(loss.bytes_consumed);
        }
        if (!send_buffer_.queue_range_drop_requests(
                std::span {stale_ranges}.first(stale_range_count))) {
            return {.error = Error::buffer_too_small};
        }

        std::size_t lost_packets = 0;
        std::optional<SequenceNumber> first_lost;
        while (!payload.empty()) {
            const auto loss = decode_loss_range(payload);
            if (!loss) {
                return {.error = loss.error};
            }
            const auto slices = split_nak_range(send_buffer_, loss.range);
            if (!slices) {
                return {.error = slices.error};
            }
            std::size_t newly_queued_packets = 0;
            std::size_t newly_queued_bytes = 0;
            if (slices.current.has_value()) {
                const Error error =
                    send_buffer_.request_retransmission(*slices.current,
                        &newly_queued_packets, &newly_queued_bytes);
                if (error != Error::none) {
                    return {.error = error};
                }
            }
            result.sender_loss_packets += newly_queued_packets;
            result.sender_loss_bytes += newly_queued_bytes;
            if (slices.current.has_value() && !first_lost.has_value()) {
                first_lost = slices.current->first;
            }
            if (slices.current.has_value()) {
                lost_packets +=
                    static_cast<std::size_t>(slices.current->last.distance_from(
                        slices.current->first))
                    + 1U;
            }
            payload = payload.subspan(loss.bytes_consumed);
        }
        append_pending_drop_requests(result.actions);
        if (file_rate_controller_.has_value()
            && first_lost.has_value()) {
            file_rate_controller_->on_loss(
                *first_lost,
                send_buffer_.next_sequence().advanced(
                    SequenceNumber::mask),
                lost_packets,
                send_buffer_.packets_in_flight(),
                0,
                rtt_.smoothed_microseconds());
        }
        return result;
    }
    case ControlType::acknowledgement_of_ack: {
        if (!is_zero_word_payload(packet.payload)) {
            return {.error = Error::invalid_control_payload};
        }
        const std::uint32_t acknowledgement_number =
            decode_ackack_number(packet);
        if (acknowledgement_number == 0U) {
            return {.error = Error::invalid_control_payload};
        }
        const auto sample = acknowledgement_tracker_.acknowledge(
            acknowledgement_number, now_microseconds);
        if (sample.has_value()) {
            rtt_.observe(*sample);
        }
        return result;
    }
    case ControlType::drop_request: {
        const auto decoded = decode_drop_request(packet);
        if (!decoded) {
            return {.error = decoded.error};
        }
        const auto error = receive_buffer_.drop_range(
            decoded.request.sequences,
            decoded.request.message_number,
            &result.receiver_drop_packets);
        if (error != Error::none) {
            return {.error = error};
        }
        receive_loss_list_.remove_through(
            decoded.request.sequences.last);
        filter_loss_list_.remove_through(
            decoded.request.sequences.last);
        if (decoded.request.sequences.first.distance_from(
                highest_received_sequence_.next()) <= 0
            && decoded.request.sequences.last.distance_from(
                highest_received_sequence_) > 0) {
            highest_received_sequence_ =
                decoded.request.sequences.last;
        }
        update_loss_timer(now_microseconds);
        return result;
    }
    case ControlType::keepalive:
        if (!is_zero_word_payload(packet.payload)) {
            return {.error = Error::invalid_control_payload};
        }
        observe_tsbpd_drift(
            packet.control.timestamp, now_microseconds);
        return result;
    case ControlType::congestion_warning:
    case ControlType::shutdown:
        if (!is_zero_word_payload(packet.payload)) {
            return {.error = Error::invalid_control_payload};
        }
        return result;
    case ControlType::peer_error:
        if (!is_zero_word_payload(packet.payload)) {
            return {.error = Error::invalid_control_payload};
        }
        return result;
    default:
        return {.error = Error::unsupported};
    }
}

void ReliabilitySession::observe_tsbpd_drift(
    PacketTimestamp timestamp,
    std::uint64_t arrival_microseconds) noexcept
{
    if (!drift_tracer_enabled_ || !tsbpd_clock_.has_value()) {
        return;
    }
    const auto drift = tsbpd_clock_->observe_arrival(
        timestamp, arrival_microseconds,
        rtt_.smoothed_microseconds());
    if (drift.window_complete
        && drift.correction_microseconds != 0) {
        ++drift_correction_count_;
        total_drift_correction_microseconds_ +=
            drift.correction_microseconds;
    }
}

ReliabilityActions ReliabilitySession::drop_too_late_sender(
    std::uint64_t now_microseconds,
    std::uint32_t threshold_microseconds) noexcept
{
    ReliabilityActions actions;
    if (threshold_microseconds == 0U
        || now_microseconds <= threshold_microseconds) {
        return actions;
    }
    const auto dropped = send_buffer_.drop_messages_older_than(
        now_microseconds - threshold_microseconds);
    if (dropped) {
        actions.push({
            .kind = ReliabilityActionKind::drop_request,
            .drop = {
                .message_number = dropped.first_message_number,
                .sequences = dropped.sequences,
            },
            .sender_drop_reason =
                SenderDropReason::too_late_packet_drop,
            .dropped_packets = dropped.packets,
            .dropped_bytes = dropped.bytes,
        });
    }
    if (send_buffer_.packets_in_flight() == 0U) {
        sender_retransmission_timer_.on_no_packets_in_flight();
    }
    return actions;
}

ReliabilityActions ReliabilitySession::drop_too_late_sender(
    std::uint64_t now_microseconds) noexcept
{
    return drop_too_late_sender(now_microseconds,
        sender_drop_threshold_microseconds_);
}

ReliabilityActions ReliabilitySession::drop_expired_sender_message(
    std::uint64_t now_microseconds) noexcept
{
    ReliabilityActions actions;
    const auto dropped =
        send_buffer_.drop_expired_message(now_microseconds);
    if (dropped) {
        actions.push({
            .kind = ReliabilityActionKind::drop_request,
            .drop = {
                .message_number = dropped.first_message_number,
                .sequences = dropped.sequences,
            },
            .sender_drop_reason =
                SenderDropReason::message_ttl,
            .dropped_packets = dropped.packets,
            .dropped_bytes = dropped.bytes,
        });
    }
    if (send_buffer_.packets_in_flight() == 0U) {
        sender_retransmission_timer_.on_no_packets_in_flight();
    }
    return actions;
}

ReliabilityActions ReliabilitySession::take_pending_drop_requests() noexcept
{
    ReliabilityActions actions;
    append_pending_drop_requests(actions);
    return actions;
}

void ReliabilitySession::enable_tsbpd(
    std::uint64_t handshake_arrival_microseconds,
    PacketTimestamp handshake_timestamp,
    std::uint32_t delay_microseconds) noexcept
{
    tsbpd_clock_.emplace(handshake_arrival_microseconds,
        handshake_timestamp, delay_microseconds);
}

ReceivedMessageResult ReliabilitySession::pop_message_at(
    std::span<std::byte> destination,
    std::uint64_t now_microseconds) noexcept
{
    std::optional<std::uint64_t> delivery;
    if (tsbpd_clock_.has_value()) {
        const auto timestamp = receive_buffer_.next_message_timestamp();
        if (!timestamp.has_value()) {
            return {.error = Error::would_block};
        }
        // This clock read is the timed pop's linearization point. A group
        // member may change the shared clock after it; preserve the deadline
        // that actually authorized this message rather than reading it again.
        delivery = tsbpd_clock_->delivery_time(*timestamp);
        if (now_microseconds < *delivery) {
            return {.error = Error::would_block};
        }
    }
    auto result = receive_buffer_.pop_message(destination);
    if (result) {
        result.delivery_time_microseconds = delivery;
    }
    return result;
}

Error ReliabilitySession::discard_received_before(
    SequenceNumber next_sequence,
    std::uint64_t now_microseconds) noexcept
{
    const SequenceNumber first =
        receive_buffer_.first_stored_sequence();
    const std::int32_t distance =
        next_sequence.distance_from(first);
    if (distance <= 0) {
        return Error::none;
    }
    const Error error =
        receive_buffer_.discard_before(next_sequence);
    if (error != Error::none) {
        return error;
    }
    const SequenceNumber last = next_sequence.advanced(
        SequenceNumber::mask);
    receive_loss_list_.remove_through(last);
    filter_loss_list_.remove_through(last);
    if (last.distance_from(highest_received_sequence_) > 0) {
        highest_received_sequence_ = last;
    }
    timer_scheduler_.on_receive_buffer_released(
        now_microseconds);
    update_loss_timer(now_microseconds);
    return Error::none;
}

bool ReliabilitySession::message_ready_at(
    std::uint64_t now_microseconds) noexcept
{
    if (!receive_buffer_.has_complete_message()) {
        return false;
    }
    if (!tsbpd_clock_.has_value()) {
        return true;
    }
    const auto timestamp = receive_buffer_.next_message_timestamp();
    return timestamp.has_value()
        && tsbpd_clock_->ready(*timestamp, now_microseconds);
}

std::optional<std::uint64_t>
ReliabilitySession::next_receive_delivery_time() noexcept
{
    if (!tsbpd_clock_.has_value()) {
        return std::nullopt;
    }
    const auto message = receive_buffer_.first_complete_message();
    if (!message.has_value()) {
        return std::nullopt;
    }
    if (message->first_sequence
            != receive_buffer_.first_stored_sequence()
        && !live_options_.too_late_packet_drop) {
        return std::nullopt;
    }
    return tsbpd_clock_->delivery_time(message->timestamp);
}

ReliabilityProcessResult
ReliabilitySession::drop_too_late_receiver(
    std::uint64_t now_microseconds) noexcept
{
    ReliabilityProcessResult result;
    if (!tsbpd_clock_.has_value()
        || !live_options_.receive_tsbpd
        || !live_options_.too_late_packet_drop) {
        return result;
    }

    const auto message = receive_buffer_.first_complete_message();
    if (!message.has_value()
        || message->first_sequence
            == receive_buffer_.first_stored_sequence()
        || !tsbpd_clock_->ready(
            message->timestamp, now_microseconds)) {
        return result;
    }

    const SequenceNumber first =
        receive_buffer_.first_stored_sequence();
    const SequenceNumber last =
        message->first_sequence.advanced(SequenceNumber::mask);
    const Error error = receive_buffer_.drop_range(
        {.first = first, .last = last},
        0, &result.receiver_drop_packets);
    if (error != Error::none) {
        result.error = error;
        return result;
    }

    receive_loss_list_.remove_through(last);
    filter_loss_list_.remove_through(last);
    timer_scheduler_.on_receive_buffer_released(
        now_microseconds);
    update_loss_timer(now_microseconds);
    result.actions.push(
        make_acknowledgement(now_microseconds));
    return result;
}

ReliabilityProcessResult
ReliabilitySession::report_filter_losses(
    std::span<const SequenceRange> losses,
    std::uint64_t now_microseconds) noexcept
{
    ReliabilityProcessResult result;
    if (losses.empty()
        || packet_filter_policy_.effective_arq_level()
            != PacketFilterArqLevel::on_request) {
        return result;
    }

    for (const auto& loss : losses) {
        const std::int32_t distance =
            loss.last.distance_from(loss.first);
        if (distance < 0) {
            return {
                .error =
                    Error::invalid_control_payload};
        }
    }
    if (!filter_loss_list_.add_all(losses, 0U)) {
        return {.error = Error::buffer_too_small};
    }
    for (const auto& loss : losses) {
        const std::size_t packets =
            static_cast<std::size_t>(
                loss.last.distance_from(loss.first))
            + 1U;
        if (result.receiver_filter_loss_packets
            > std::numeric_limits<std::size_t>::max()
                - packets) {
            result.receiver_filter_loss_packets =
                std::numeric_limits<std::size_t>::max();
        } else {
            result.receiver_filter_loss_packets += packets;
        }
    }
    append_pending_loss_report(result.actions, true);
    update_loss_timer(now_microseconds);
    return result;
}

void ReliabilitySession::constrain_fresh_loss_wait(std::uint64_t now) noexcept
{
    if ((!experimental_recovery_reserve_ && !experimental_recovery_budget_)
        || receive_loss_list_.next_fresh_deadline() == 0U || !tsbpd_clock_
        || !live_options_.receive_tsbpd || !live_options_.too_late_packet_drop)
        return;
    const auto deadline = next_receive_delivery_time();
    // Evaluation branch only: conservative estimate, not a recovery guarantee.
    const auto reserve = experimental_recovery_reserve_.value_or(
        std::uint64_t {rtt_.smoothed_microseconds()}
        + 4U * std::uint64_t {rtt_.variation_microseconds()} + 2'000U);
    auto latest = now;
    if (deadline && *deadline > now && *deadline - now > reserve) {
        latest = *deadline - reserve - 1U;
    }
    receive_loss_list_.tighten_fresh_deadlines(latest, now);
}

ReliabilityActions ReliabilitySession::poll_timers(
    std::uint64_t now_microseconds) noexcept
{
    ReliabilityActions actions;
    constrain_fresh_loss_wait(now_microseconds);
    receive_loss_list_.expire_fresh(now_microseconds);
    const auto due = timer_scheduler_.poll(now_microseconds);
    const std::size_t reserved_for_timers =
        std::min(due.size, actions.values.size());
    append_pending_loss_report(
        actions,
        actions.size + reserved_for_timers
            < actions.values.size());
    for (std::size_t index = 0; index < due.size; ++index) {
        if (actions.size == actions.values.size()) {
            break;
        }
        switch (due.values[index]) {
        case TimerActionKind::full_acknowledgement:
            actions.push(make_acknowledgement(now_microseconds));
            break;
        case TimerActionKind::lite_acknowledgement:
            actions.push(make_acknowledgement(
                now_microseconds, AcknowledgementKind::lite));
            break;
        case TimerActionKind::periodic_loss_report: {
            switch (
                packet_filter_policy_
                    .effective_arq_level()) {
            case PacketFilterArqLevel::always:
                receive_loss_list_
                    .mark_periodic_reports();
                break;
            case PacketFilterArqLevel::on_request:
                filter_loss_list_
                    .mark_periodic_reports();
                break;
            case PacketFilterArqLevel::never:
                break;
            }
            const std::size_t later_timers =
                due.size - index - 1U;
            const std::size_t available =
                actions.values.size() - actions.size;
            append_pending_loss_report(
                actions, available > later_timers);
            break;
        }
        case TimerActionKind::keepalive:
            actions.push({.kind = ReliabilityActionKind::keepalive});
            break;
        }
    }
    return actions;
}

void ReliabilitySession::note_data_packet_sent(
    std::uint64_t now_microseconds) noexcept
{
    timer_scheduler_.on_packet_sent(now_microseconds);
    sender_retransmission_timer_.on_data_packet_sent(now_microseconds);
}

bool ReliabilitySession::poll_sender_retransmission_timeout(
    std::uint64_t now_microseconds) noexcept
{
    if (send_buffer_.packets_in_flight() == 0U) {
        sender_retransmission_timer_.on_no_packets_in_flight();
        return false;
    }

    if (!sender_retransmission_timer_.poll(
            now_microseconds,
            rtt_.smoothed_microseconds(),
            rtt_.variation_microseconds())) {
        return false;
    }

    // A periodic NAK can select only a gap exposed by later DATA. It cannot
    // name a lost flight tail because the receiver has not observed that
    // sequence yet. Preserve an already selected NAK/LATEREXMIT range, but
    // fall back to the sender RTO for the remaining unacknowledged flight when
    // no selective retransmission is pending. This keeps LiveCC loss recovery
    // NAK-driven without leaving a tail loss permanently stranded.
    const bool selective_retransmission = file_rate_controller_.has_value()
        || (live_rate_controller_.has_value() && periodic_nak_enabled_);
    if (!selective_retransmission
        || !send_buffer_.has_pending_retransmission()) {
        if (live_rate_controller_.has_value() && periodic_nak_enabled_
            && packet_filter_policy_.effective_arq_level()
                == PacketFilterArqLevel::always) {
            // Probe the last sent packet: its arrival exposes older gaps to
            // periodic NAK recovery and also repairs a lost flight tail.
            // Replaying the whole growing flight on every RTO amplifies an
            // outage even though the receiver can request specific losses.
            (void)send_buffer_.request_retransmission_of_last_sent();
        } else {
            (void)send_buffer_.request_retransmission_of_all_sent();
        }
    }
    if (file_rate_controller_.has_value()) {
        file_rate_controller_->on_timeout(
            0, rtt_.smoothed_microseconds());
    }
    return true;
}

ControlEncodeResult encode_reliability_action(
    const ReliabilityAction& action,
    std::span<const SequenceRange> loss_ranges,
    PacketTimestamp timestamp,
    std::uint32_t destination_socket_id,
    std::span<std::byte> destination) noexcept
{
    if (destination.size() < packet_header_size) {
        return {.error = Error::buffer_too_small};
    }
    MutablePacketView packet;
    packet.kind = PacketKind::control;
    packet.control.timestamp = timestamp;
    packet.control.destination_socket_id = destination_socket_id;

    std::size_t payload_size = 0;
    switch (action.kind) {
    case ReliabilityActionKind::acknowledgement: {
        if (action.acknowledgement.kind == AcknowledgementKind::full
            && action.acknowledgement.acknowledgement_number == 0U) {
            return {.error = Error::invalid_control_payload};
        }
        packet.control.type = ControlType::acknowledgement;
        packet.control.type_specific =
            action.acknowledgement.kind == AcknowledgementKind::full
            ? action.acknowledgement.acknowledgement_number
            : 0U;
        const auto encoded = encode_acknowledgement_payload(
            action.acknowledgement, destination.subspan(packet_header_size));
        if (!encoded) {
            return encoded;
        }
        payload_size = encoded.bytes_written;
        break;
    }
    case ReliabilityActionKind::acknowledgement_of_ack:
        if (action.acknowledgement_number == 0U) {
            return {.error = Error::invalid_control_payload};
        }
        packet.control.type = ControlType::acknowledgement_of_ack;
        packet.control.type_specific = action.acknowledgement_number;
        break;
    case ReliabilityActionKind::loss_report: {
        packet.control.type = ControlType::negative_acknowledgement;
        if (loss_ranges.empty()) {
            return {.error = Error::invalid_control_payload};
        }
        const auto encoded = encode_loss_ranges(
            loss_ranges,
            destination.subspan(packet_header_size));
        if (!encoded) {
            return encoded;
        }
        payload_size = encoded.bytes_written;
        break;
    }
    case ReliabilityActionKind::drop_request: {
        packet.control.type = ControlType::drop_request;
        packet.control.type_specific = action.drop.message_number;
        const auto encoded = encode_drop_request_payload(
            action.drop, destination.subspan(packet_header_size));
        if (!encoded) {
            return encoded;
        }
        payload_size = encoded.bytes_written;
        break;
    }
    case ReliabilityActionKind::keepalive:
        packet.control.type = ControlType::keepalive;
        break;
    case ReliabilityActionKind::shutdown:
        packet.control.type = ControlType::shutdown;
        break;
    }

    const auto header = encode_packet(packet, destination);
    if (!header) {
        return {.error = header.error};
    }
    return {.bytes_written = std::max(
                header.bytes_written, packet_header_size + payload_size)};
}

ControlEncodeResult encode_reliability_action(
    const ReliabilityAction& action,
    PacketTimestamp timestamp,
    std::uint32_t destination_socket_id,
    std::span<std::byte> destination) noexcept
{
    const std::array<SequenceRange, 1> single_loss{
        action.loss};
    const std::span<const SequenceRange> loss_ranges =
        action.kind == ReliabilityActionKind::loss_report
        ? std::span<const SequenceRange>{single_loss}
        : std::span<const SequenceRange>{};
    return encode_reliability_action(
        action, loss_ranges, timestamp,
        destination_socket_id, destination);
}

} // namespace robotweax::srt
