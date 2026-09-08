#pragma once

#include "robotweax/srt/control.hpp"
#include "robotweax/srt/file.hpp"
#include "robotweax/srt/handshake_extensions.hpp"
#include "robotweax/srt/live.hpp"
#include "robotweax/srt/receive_buffer.hpp"
#include "robotweax/srt/rtt.hpp"
#include "robotweax/srt/send_buffer.hpp"
#include "robotweax/srt/socket_options.hpp"
#include "robotweax/srt/timing.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace robotweax::srt {

enum class ReliabilityActionKind : std::uint8_t {
    acknowledgement,
    acknowledgement_of_ack,
    loss_report,
    drop_request,
    keepalive,
    shutdown,
};

enum class SenderDropReason : std::uint8_t {
    none,
    message_ttl,
    too_late_packet_drop,
};

struct ReliabilityAction {
    ReliabilityActionKind kind = ReliabilityActionKind::acknowledgement;
    Acknowledgement acknowledgement{};
    std::uint32_t acknowledgement_number = 0;
    SequenceRange loss{};
    std::size_t loss_range_offset = 0;
    std::size_t loss_range_count = 0;
    DropRequest drop{};
    SenderDropReason sender_drop_reason =
        SenderDropReason::none;
    std::size_t dropped_packets = 0;
    std::size_t dropped_bytes = 0;
};

// A range needs at most two 32-bit loss-list words. Limiting a batch by the
// worst case guarantees that every encoded NAK fits one maximum-sized SRT
// payload while keeping the per-packet action result allocation-free.
inline constexpr std::size_t maximum_loss_ranges_per_report =
    maximum_data_payload_size / (2U * sizeof(std::uint32_t));

struct ReliabilityActions {
    std::array<ReliabilityAction, 4> values{};
    std::size_t size = 0;
    std::array<SequenceRange, maximum_loss_ranges_per_report>
        loss_range_storage{};
    std::size_t loss_range_storage_size = 0;

    constexpr void push(ReliabilityAction action) noexcept
    {
        if (size < values.size()) {
            values[size++] = action;
        }
    }

    [[nodiscard]] constexpr std::span<SequenceRange>
    available_loss_range_storage() noexcept
    {
        return std::span{loss_range_storage}.subspan(
            loss_range_storage_size);
    }

    constexpr void commit_loss_report(
        std::size_t range_count) noexcept
    {
        const std::size_t available =
            loss_range_storage.size() - loss_range_storage_size;
        if (size == values.size() || range_count == 0U
            || range_count > available) {
            return;
        }
        const std::size_t offset = loss_range_storage_size;
        loss_range_storage_size += range_count;
        push({
            .kind = ReliabilityActionKind::loss_report,
            .loss = loss_range_storage[offset],
            .loss_range_offset = offset,
            .loss_range_count = range_count,
        });
    }

    [[nodiscard]] constexpr std::span<const SequenceRange>
    loss_ranges(const ReliabilityAction& action) const noexcept
    {
        if (action.loss_range_count == 0U) {
            return action.kind == ReliabilityActionKind::loss_report
                ? std::span<const SequenceRange>{&action.loss, 1U}
                : std::span<const SequenceRange>{};
        }
        if (action.loss_range_offset > loss_range_storage_size
            || action.loss_range_count
                > loss_range_storage_size
                    - action.loss_range_offset) {
            return {};
        }
        return std::span{loss_range_storage}.subspan(
            action.loss_range_offset, action.loss_range_count);
    }
};

struct ReliabilityProcessResult {
    Error error = Error::none;
    ReliabilityActions actions{};
    std::optional<std::uint32_t> peer_available_receive_buffer_packets =
        std::nullopt;
    std::size_t sender_loss_packets = 0;
    std::size_t sender_loss_bytes = 0;
    std::size_t receiver_loss_packets = 0;
    std::size_t receiver_filter_loss_packets = 0;
    std::size_t receiver_drop_packets = 0;
    std::size_t receiver_reorder_distance_packets = 0;
    std::uint64_t receiver_belated_delay_microseconds = 0;
    bool receiver_packet_accepted_unique = false;
    bool receiver_packet_belated = false;
    bool receiver_filter_control_packet = false;

    [[nodiscard]] constexpr explicit operator bool() const noexcept
    {
        return error == Error::none;
    }
};

struct ReliabilityReceiveContext {
    // A filter-supplied packet was not observed on the wire. It must repair
    // receive-buffer/loss state without changing physical arrival tracking.
    bool filter_supplied = false;
    // A source packet and its FEC reconstruction are applied as one logical
    // receive batch. Feedback is emitted by the final packet in that batch.
    bool defer_feedback = false;
};

/**
 * Stateful reliability engine with construction-time bounded storage. Returned
 * packet/action spans borrow this instance and are invalidated by subsequent
 * mutating operations. One instance is not internally synchronized.
 */
class ReliabilitySession {
public:
    struct Configuration {
        SequenceNumber local_initial_sequence{};
        SequenceNumber peer_initial_sequence{};
        std::uint32_t peer_socket_id = 0;
        std::size_t send_capacity_packets =
            default_srt_buffer_capacity_packets;
        std::size_t receive_capacity_packets =
            default_srt_buffer_capacity_packets;
        std::size_t maximum_payload_size = maximum_data_payload_size;
        std::uint64_t start_microseconds = 0;
        // Experiment opt-in only; no production reserve has been selected.
        std::optional<std::uint64_t> experimental_recovery_reserve;
        bool experimental_recovery_budget = false;
    };

    explicit ReliabilitySession(Configuration configuration);

    void set_peer_socket_id(std::uint32_t socket_id) noexcept { peer_socket_id_ = socket_id; }
    [[nodiscard]] Error queue_message(std::span<const std::byte> message,
        PacketTimestamp timestamp, bool in_order = true,
        std::uint64_t enqueue_microseconds = 0,
        std::uint64_t expiration_microseconds = 0) noexcept;
    [[nodiscard]] Error queue_group_message(
        std::span<const std::byte> message,
        SequenceNumber first_sequence,
        std::uint32_t message_number,
        PacketTimestamp timestamp, bool in_order = true,
        std::uint64_t enqueue_microseconds = 0,
        std::uint64_t expiration_microseconds = 0) noexcept;
    [[nodiscard]] Error skip_group_sequences(
        SequenceNumber next_sequence) noexcept;
    [[nodiscard]] StreamEnqueueResult queue_stream(
        std::span<const std::byte> bytes,
        PacketTimestamp timestamp,
        std::uint64_t enqueue_microseconds = 0) noexcept;
    [[nodiscard]] std::optional<OutboundPacket> next_data_packet() noexcept
    {
        return send_buffer_.next_packet();
    }
    [[nodiscard]] std::optional<OutboundPacket> next_paced_data_packet(
        PacketPacer& pacer, std::uint64_t now_microseconds,
        std::size_t new_packet_wire_overhead = 0U) noexcept;
    [[nodiscard]] Error preserve_encrypted_payload(
        SequenceNumber sequence,
        EncryptionKey key,
        std::span<const std::byte> ciphertext) noexcept
    {
        return send_buffer_.preserve_encrypted_payload(
            sequence, key, ciphertext);
    }
    [[nodiscard]] Error preserve_protected_payload(SequenceNumber sequence,
        EncryptionKey key, CryptoMode mode,
        std::span<const std::byte> protected_payload) noexcept
    {
        return send_buffer_.preserve_protected_payload(
            sequence, key, mode, protected_payload);
    }
    void configure_live(const NegotiatedLiveOptions& options,
        std::uint64_t handshake_arrival_microseconds,
        PacketTimestamp peer_handshake_timestamp,
        LiveRateController::Configuration rate_configuration = {}) noexcept;
    void configure_file(
        bool message_api,
        FileRateController::Configuration rate_configuration = {}) noexcept;
    void configure_packet_filter(
        const PacketFilterConfiguration& configuration,
        bool reconstruction_available = false) noexcept
    {
        packet_filter_policy_ = PacketFilterPolicy{
            configuration, reconstruction_available};
    }
    [[nodiscard]] const PacketFilterPolicy&
    packet_filter_policy() const noexcept
    {
        return packet_filter_policy_;
    }
    void set_message_api(bool enabled) noexcept
    {
        message_api_ = enabled;
    }
    [[nodiscard]] Error apply_dynamic_options(
        const SocketOptions& options) noexcept;
    [[nodiscard]] std::uint64_t live_pacing_rate_bytes_per_second() const noexcept
    {
        return live_rate_controller_.has_value()
            ? live_rate_controller_->pacing_rate_bytes_per_second() : 0U;
    }
    [[nodiscard]] std::uint64_t file_pacing_rate_bytes_per_second() const noexcept
    {
        return file_rate_controller_.has_value()
            ? file_rate_controller_->pacing_rate_bytes_per_second()
            : 0U;
    }
    [[nodiscard]] std::size_t file_congestion_window_packets() const noexcept
    {
        return file_rate_controller_.has_value()
            ? file_rate_controller_->congestion_window_packets()
            : 0U;
    }
    [[nodiscard]] bool file_slow_start() const noexcept
    {
        return file_rate_controller_.has_value()
            && file_rate_controller_->slow_start();
    }
    [[nodiscard]] double packet_sending_period_microseconds() const noexcept
    {
        if (file_rate_controller_.has_value()) {
            return file_rate_controller_
                ->packet_sending_period_microseconds();
        }
        return live_rate_controller_.has_value()
            ? static_cast<double>(
                live_rate_controller_->packet_interval_microseconds())
            : 0.0;
    }
    [[nodiscard]] ArrivalRates arrival_rates() const noexcept
    {
        return arrival_rate_estimator_.rates();
    }
    [[nodiscard]] std::uint32_t next_message_number() const noexcept
    {
        return next_message_number_;
    }
    [[nodiscard]] bool has_pending_send_work() noexcept
    {
        return send_buffer_.has_pending_drop_request()
            || send_buffer_.has_pending_retransmission()
            || send_buffer_.size() > send_buffer_.packets_in_flight();
    }
    [[nodiscard]] bool has_pending_retransmission() noexcept
    {
        return send_buffer_.has_pending_retransmission();
    }
    [[nodiscard]] ReliabilityProcessResult receive(
        const PacketView& packet,
        std::uint64_t now_microseconds,
        ReliabilityReceiveContext context = {}) noexcept;
    [[nodiscard]] ReliabilityProcessResult
    report_filter_losses(
        std::span<const SequenceRange> losses,
        std::uint64_t now_microseconds) noexcept;
    [[nodiscard]] ReceivedMessageResult pop_message(
        std::span<std::byte> destination) noexcept
    {
        return receive_buffer_.pop_message(destination);
    }
    [[nodiscard]] ReceivedMessageResult pop_stream(
        std::span<std::byte> destination) noexcept
    {
        return receive_buffer_.pop_stream(destination);
    }
    void enable_tsbpd(std::uint64_t handshake_arrival_microseconds,
        PacketTimestamp handshake_timestamp,
        std::uint32_t delay_microseconds) noexcept;
    [[nodiscard]] ReceivedMessageResult pop_message_at(
        std::span<std::byte> destination,
        std::uint64_t now_microseconds) noexcept;
    [[nodiscard]] Error discard_received_before(
        SequenceNumber next_sequence,
        std::uint64_t now_microseconds) noexcept;
    [[nodiscard]] bool message_ready_at(
        std::uint64_t now_microseconds) noexcept;
    [[nodiscard]] std::optional<std::uint64_t>
    next_receive_delivery_time() noexcept;
    [[nodiscard]] ReliabilityProcessResult
    drop_too_late_receiver(
        std::uint64_t now_microseconds) noexcept;
    [[nodiscard]] bool data_ready_at(
        std::uint64_t now_microseconds) noexcept
    {
        return message_api_
            ? message_ready_at(now_microseconds)
            : receive_buffer_.has_stream_data();
    }
    [[nodiscard]] std::optional<std::uint64_t> message_delivery_time(
        PacketTimestamp timestamp) noexcept
    {
        return tsbpd_clock_.has_value()
            ? std::optional<std::uint64_t>{
                tsbpd_clock_->delivery_time(timestamp)}
            : std::nullopt;
    }
    [[nodiscard]] ReliabilityActions poll_timers(
        std::uint64_t now_microseconds) noexcept;
    [[nodiscard]] std::uint64_t next_fresh_deadline() const noexcept
    {
        return receive_loss_list_.next_fresh_deadline();
    }
    [[nodiscard]] bool poll_sender_retransmission_timeout(
        std::uint64_t now_microseconds) noexcept;
    [[nodiscard]] ReliabilityActions drop_too_late_sender(
        std::uint64_t now_microseconds,
        std::uint32_t threshold_microseconds) noexcept;
    [[nodiscard]] ReliabilityActions drop_too_late_sender(
        std::uint64_t now_microseconds) noexcept;
    [[nodiscard]] ReliabilityActions drop_expired_sender_message(
        std::uint64_t now_microseconds) noexcept;
    [[nodiscard]] ReliabilityActions take_pending_drop_requests() noexcept;
    [[nodiscard]] bool has_pending_drop_requests() noexcept
    {
        return send_buffer_.has_pending_drop_request();
    }
    void note_receive_buffer_released(
        std::uint64_t now_microseconds) noexcept
    {
        timer_scheduler_.on_receive_buffer_released(
            now_microseconds);
    }
    void note_packet_sent(std::uint64_t now_microseconds) noexcept
    {
        timer_scheduler_.on_packet_sent(now_microseconds);
    }
    void note_data_packet_sent(
        std::uint64_t now_microseconds) noexcept;
    // Produce a full ACK for bytes that an owning transport layer has already
    // copied into bounded staging storage but cannot yet publish to the
    // receive buffer. Sharing the regular ACK number/tracker keeps ACKACK and
    // RTT handling coherent with ordinary receive acknowledgements.
    [[nodiscard]] ReliabilityAction
    make_staged_receive_acknowledgement(
        SequenceNumber next_sequence,
        std::size_t available_receive_buffer_packets,
        std::uint64_t now_microseconds) noexcept;

    [[nodiscard]] const SendBuffer& send_buffer() const noexcept { return send_buffer_; }
    [[nodiscard]] const ReceiveBuffer& receive_buffer() const noexcept { return receive_buffer_; }
    [[nodiscard]] const RttEstimator& rtt() const noexcept { return rtt_; }
    [[nodiscard]] const NegotiatedLiveOptions& live_options() const noexcept
    {
        return live_options_;
    }
    [[nodiscard]] std::uint64_t drift_correction_count() const noexcept
    {
        return drift_correction_count_;
    }
    [[nodiscard]] std::int64_t total_drift_correction_microseconds() const noexcept
    {
        return total_drift_correction_microseconds_;
    }
    [[nodiscard]] std::size_t reorder_distance_packets() const noexcept
    {
        return reorder_distance_packets_;
    }
    [[nodiscard]] std::uint32_t reorder_tolerance_packets() const noexcept
    {
        return reorder_tolerance_packets_;
    }

private:
    std::optional<std::uint64_t> experimental_recovery_reserve_;
    bool experimental_recovery_budget_ = false;
    void constrain_fresh_loss_wait(std::uint64_t now) noexcept;
    friend class compat::ConnectionRuntime;
    [[nodiscard]] ReliabilityAction make_acknowledgement(
        std::uint64_t now_microseconds,
        AcknowledgementKind kind = AcknowledgementKind::full) noexcept;
    void set_maximum_reorder_tolerance(
        std::uint32_t packets) noexcept;
    void note_reordered_packet(
        std::size_t distance_packets,
        std::uint32_t remaining_ttl) noexcept;
    void note_ordered_packet() noexcept;
    void append_pending_loss_report(
        ReliabilityActions& actions,
        bool action_slot_available) noexcept;
    void append_pending_drop_requests(ReliabilityActions& actions) noexcept;
    void update_loss_timer(
        std::uint64_t now_microseconds) noexcept;
    void observe_tsbpd_drift(PacketTimestamp timestamp,
        std::uint64_t arrival_microseconds) noexcept;

    SendBuffer send_buffer_;
    ReceiveBuffer receive_buffer_;
    ReceiveLossList receive_loss_list_;
    ReceiveLossList filter_loss_list_;
    AcknowledgementTracker acknowledgement_tracker_{64};
    RttEstimator rtt_;
    ControlTimerScheduler timer_scheduler_;
    SenderRetransmissionTimer sender_retransmission_timer_;
    ArrivalRateEstimator arrival_rate_estimator_;
    std::optional<TsbpdClock> tsbpd_clock_;
    std::optional<LiveRateController> live_rate_controller_;
    std::optional<FileRateController> file_rate_controller_;
    SequenceNumber highest_received_sequence_{};
    std::optional<SequenceNumber> probe_first_sequence_;
    std::uint64_t probe_first_arrival_microseconds_ = 0;
    NegotiatedLiveOptions live_options_{};
    std::uint32_t sender_drop_threshold_microseconds_ = 0;
    std::uint32_t maximum_reorder_tolerance_packets_ = 0;
    std::uint32_t reorder_tolerance_packets_ = 0;
    std::uint64_t reorder_tolerance_revision_ = 0;
    std::size_t reorder_distance_packets_ = 0;
    std::uint32_t consecutive_early_reorders_ = 0;
    std::uint32_t consecutive_ordered_packets_ = 0;
    bool periodic_nak_enabled_ = true;
    bool drift_tracer_enabled_ = true;
    PacketFilterPolicy packet_filter_policy_{};
    std::uint64_t drift_correction_count_ = 0;
    std::int64_t total_drift_correction_microseconds_ = 0;
    std::uint32_t peer_socket_id_ = 0;
    std::uint32_t next_message_number_ = 1;
    AcknowledgementNumberGenerator acknowledgement_numbers_;
    bool has_received_data_ = false;
    bool message_api_ = true;
};

[[nodiscard]] ControlEncodeResult encode_reliability_action(
    const ReliabilityAction& action,
    PacketTimestamp timestamp,
    std::uint32_t destination_socket_id,
    std::span<std::byte> destination) noexcept;

[[nodiscard]] ControlEncodeResult encode_reliability_action(
    const ReliabilityAction& action,
    std::span<const SequenceRange> loss_ranges,
    PacketTimestamp timestamp,
    std::uint32_t destination_socket_id,
    std::span<std::byte> destination) noexcept;

} // namespace robotweax::srt
