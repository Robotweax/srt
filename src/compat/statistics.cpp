#include "compat/statistics.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>

namespace robotweax::srt::compat {
namespace {

[[nodiscard]] constexpr std::uint64_t saturated_add(
    std::uint64_t left, std::uint64_t right) noexcept
{
    return left > std::numeric_limits<std::uint64_t>::max() - right
        ? std::numeric_limits<std::uint64_t>::max()
        : left + right;
}

[[nodiscard]] constexpr std::uint64_t saturated_multiply(
    std::uint64_t left, std::uint64_t right) noexcept
{
    return left != 0U
            && right > std::numeric_limits<std::uint64_t>::max() / left
        ? std::numeric_limits<std::uint64_t>::max()
        : left * right;
}

template<class Destination>
[[nodiscard]] constexpr Destination narrowed(
    std::uint64_t value) noexcept
{
    static_assert(std::is_integral_v<Destination>);
    const auto maximum = static_cast<std::uint64_t>(
        std::numeric_limits<Destination>::max());
    return static_cast<Destination>(std::min(value, maximum));
}

[[nodiscard]] PacketByteCounter combined_sender_drop(
    const StatisticsCounters& counters) noexcept
{
    return {
        .packets = saturated_add(
            counters.sender_message_ttl_dropped.packets,
            counters.sender_tlpktdrop_dropped.packets),
        .payload_bytes = saturated_add(
            counters.sender_message_ttl_dropped.payload_bytes,
            counters.sender_tlpktdrop_dropped.payload_bytes),
    };
}

template<class Destination>
[[nodiscard]] Destination rounded_nonnegative(
    double value) noexcept
{
    static_assert(std::is_integral_v<Destination>);
    if (!(value > 0.0)) {
        return 0;
    }
    const double maximum = static_cast<double>(
        std::numeric_limits<Destination>::max());
    if (value >= maximum) {
        return std::numeric_limits<Destination>::max();
    }
    return static_cast<Destination>(std::round(value));
}

} // namespace

void PacketByteCounter::add(
    std::uint64_t packet_count, std::uint64_t bytes) noexcept
{
    packets = saturated_add(packets, packet_count);
    payload_bytes = saturated_add(payload_bytes, bytes);
}

std::uint64_t PacketByteCounter::wire_bytes(
    std::uint64_t packet_header_bytes) const noexcept
{
    return saturated_add(payload_bytes,
        saturated_multiply(packets, packet_header_bytes));
}

RuntimeStatisticsState::RuntimeStatisticsState(
    std::uint64_t start_microseconds,
    std::uint64_t packet_header_bytes) noexcept
    : start_microseconds_(start_microseconds)
    , interval_start_microseconds_(start_microseconds)
    , duration_update_microseconds_(start_microseconds)
    , packet_header_bytes_(packet_header_bytes)
{
}

void RuntimeStatisticsState::add(
    PacketMember member,
    std::uint64_t packets,
    std::uint64_t payload_bytes) noexcept
{
    (total_.*member).add(packets, payload_bytes);
    (interval_.*member).add(packets, payload_bytes);
}

void RuntimeStatisticsState::increment(CountMember member) noexcept
{
    add_count(member, 1U);
}

void RuntimeStatisticsState::add_count(
    CountMember member, std::uint64_t count) noexcept
{
    total_.*member =
        saturated_add(total_.*member, count);
    interval_.*member =
        saturated_add(interval_.*member, count);
}

void RuntimeStatisticsState::note_data_sent(
    std::size_t payload_bytes, bool retransmitted) noexcept
{
    add(&StatisticsCounters::sent, 1U, payload_bytes);
    if (retransmitted) {
        add(&StatisticsCounters::sent_retransmitted,
            1U, payload_bytes);
    } else {
        add(&StatisticsCounters::sent_unique,
            1U, payload_bytes);
    }
}

void RuntimeStatisticsState::note_data_received(
    std::size_t payload_bytes,
    bool accepted_unique,
    bool retransmitted) noexcept
{
    add(&StatisticsCounters::received, 1U, payload_bytes);
    if (accepted_unique) {
        add(&StatisticsCounters::received_unique,
            1U, payload_bytes);
    }
    if (retransmitted) {
        add(&StatisticsCounters::received_retransmitted,
            1U, payload_bytes);
    }
}

void RuntimeStatisticsState::note_sender_loss(
    std::size_t packets, std::size_t payload_bytes) noexcept
{
    add(&StatisticsCounters::sender_loss,
        packets, payload_bytes);
}

void RuntimeStatisticsState::note_receiver_loss(
    std::size_t packets,
    std::size_t estimated_payload_bytes) noexcept
{
    add(&StatisticsCounters::receiver_loss,
        packets, estimated_payload_bytes);
}

void RuntimeStatisticsState::note_sender_message_ttl_drop(
    std::size_t packets, std::size_t payload_bytes) noexcept
{
    add(&StatisticsCounters::sender_message_ttl_dropped,
        packets, payload_bytes);
}

void RuntimeStatisticsState::note_sender_tlpktdrop(
    std::size_t packets, std::size_t payload_bytes) noexcept
{
    add(&StatisticsCounters::sender_tlpktdrop_dropped,
        packets, payload_bytes);
}

void RuntimeStatisticsState::note_receiver_drop(
    std::size_t packets,
    std::size_t estimated_payload_bytes) noexcept
{
    add(&StatisticsCounters::receiver_dropped,
        packets, estimated_payload_bytes);
}

void RuntimeStatisticsState::note_receiver_undecryptable(
    std::size_t payload_bytes) noexcept
{
    add(&StatisticsCounters::receiver_undecryptable,
        1U, payload_bytes);
    add(&StatisticsCounters::receiver_dropped,
        1U, payload_bytes);
}

void RuntimeStatisticsState::note_receiver_belated(
    std::size_t payload_bytes,
    std::uint64_t delay_microseconds) noexcept
{
    add(&StatisticsCounters::receiver_belated,
        1U, payload_bytes);
    const double sample_milliseconds =
        static_cast<double>(delay_microseconds) / 1'000.0;
    if (average_belated_milliseconds_ == 0.0) {
        average_belated_milliseconds_ =
            sample_milliseconds;
    } else {
        constexpr double new_sample_weight = 0.2;
        average_belated_milliseconds_ +=
            (sample_milliseconds
                - average_belated_milliseconds_)
            * new_sample_weight;
    }
}

void RuntimeStatisticsState::note_sender_filter_extra(
    std::size_t payload_bytes) noexcept
{
    add(&StatisticsCounters::sent, 1U, payload_bytes);
    increment(&StatisticsCounters::sender_filter_extra);
}

void RuntimeStatisticsState::note_receiver_filter_extra() noexcept
{
    increment(&StatisticsCounters::receiver_filter_extra);
}

void RuntimeStatisticsState::note_receiver_filter_supply(
    std::size_t payload_bytes,
    bool accepted_unique) noexcept
{
    increment(&StatisticsCounters::receiver_filter_supply);
    if (accepted_unique) {
        add(&StatisticsCounters::received_unique,
            1U, payload_bytes);
    }
}

void RuntimeStatisticsState::note_receiver_filter_loss(
    std::size_t packets) noexcept
{
    add_count(&StatisticsCounters::receiver_filter_loss,
        packets);
}

void RuntimeStatisticsState::note_reorder_distance(
    std::size_t distance_packets) noexcept
{
    reorder_distance_packets_ = std::max(
        reorder_distance_packets_, distance_packets);
}

void RuntimeStatisticsState::update_reorder_state(
    std::size_t distance_packets,
    std::size_t tolerance_packets) noexcept
{
    reorder_distance_packets_ = distance_packets;
    reorder_tolerance_packets_ = tolerance_packets;
}

void RuntimeStatisticsState::note_control_sent(
    ControlType type) noexcept
{
    switch (type) {
    case ControlType::acknowledgement:
        increment(&StatisticsCounters::sent_acknowledgements);
        break;
    case ControlType::negative_acknowledgement:
        increment(&StatisticsCounters::sent_loss_reports);
        break;
    default:
        break;
    }
}

void RuntimeStatisticsState::note_control_received(
    ControlType type) noexcept
{
    switch (type) {
    case ControlType::acknowledgement:
        increment(&StatisticsCounters::received_acknowledgements);
        break;
    case ControlType::negative_acknowledgement:
        increment(&StatisticsCounters::received_loss_reports);
        break;
    default:
        break;
    }
}

void RuntimeStatisticsState::update_send_duration(
    std::uint64_t now_microseconds,
    bool sender_buffer_nonempty) noexcept
{
    if (sender_busy_
        && now_microseconds >= duration_update_microseconds_) {
        const std::uint64_t elapsed =
            now_microseconds - duration_update_microseconds_;
        total_.send_duration_microseconds = saturated_add(
            total_.send_duration_microseconds, elapsed);
        interval_.send_duration_microseconds = saturated_add(
            interval_.send_duration_microseconds, elapsed);
    }
    duration_update_microseconds_ = now_microseconds;
    sender_busy_ = sender_buffer_nonempty;
}

void RuntimeStatisticsState::BufferMovingAverage::sample(
    std::uint64_t now_microseconds,
    std::size_t packets,
    std::size_t payload_bytes,
    std::uint64_t milliseconds) noexcept
{
    if (!initialized_ || now_microseconds < last_sample_microseconds_) {
        last_sample_microseconds_ = now_microseconds;
        packets_ = static_cast<double>(packets);
        payload_bytes_ = static_cast<double>(payload_bytes);
        milliseconds_ = static_cast<double>(milliseconds);
        initialized_ = true;
        return;
    }

    constexpr std::uint64_t sampling_period_microseconds = 25'000;
    const std::uint64_t elapsed_microseconds =
        now_microseconds - last_sample_microseconds_;
    if (elapsed_microseconds < sampling_period_microseconds) {
        return;
    }

    const std::uint64_t elapsed_milliseconds =
        elapsed_microseconds / 1'000U;
    last_sample_microseconds_ = now_microseconds;
    if (elapsed_milliseconds > 1'000U) {
        packets_ = static_cast<double>(packets);
        payload_bytes_ = static_cast<double>(payload_bytes);
        milliseconds_ = static_cast<double>(milliseconds);
        return;
    }

    const double new_weight =
        static_cast<double>(elapsed_milliseconds) / 1'000.0;
    packets_ += (static_cast<double>(packets) - packets_)
        * new_weight;
    payload_bytes_ +=
        (static_cast<double>(payload_bytes) - payload_bytes_)
        * new_weight;
    milliseconds_ +=
        (static_cast<double>(milliseconds) - milliseconds_)
        * new_weight;
}

std::size_t
RuntimeStatisticsState::BufferMovingAverage::packets() const noexcept
{
    return rounded_nonnegative<std::size_t>(packets_);
}

std::size_t
RuntimeStatisticsState::BufferMovingAverage::payload_bytes() const noexcept
{
    return rounded_nonnegative<std::size_t>(payload_bytes_);
}

std::uint64_t
RuntimeStatisticsState::BufferMovingAverage::milliseconds() const noexcept
{
    return rounded_nonnegative<std::uint64_t>(milliseconds_);
}

void RuntimeStatisticsState::sample_buffers(
    std::uint64_t now_microseconds,
    BufferStatisticsSample sample) noexcept
{
    sample_sender_buffer(
        now_microseconds,
        sample.sender_packets,
        sample.sender_payload_bytes,
        sample.sender_milliseconds);
    sample_receiver_buffer(
        now_microseconds,
        sample.receiver_packets,
        sample.receiver_payload_bytes,
        sample.receiver_milliseconds);
}

void RuntimeStatisticsState::sample_sender_buffer(
    std::uint64_t now_microseconds,
    std::size_t packets,
    std::size_t payload_bytes,
    std::uint64_t milliseconds) noexcept
{
    sender_buffer_average_.sample(
        now_microseconds, packets,
        payload_bytes, milliseconds);
}

void RuntimeStatisticsState::sample_receiver_buffer(
    std::uint64_t now_microseconds,
    std::size_t packets,
    std::size_t payload_bytes,
    std::uint64_t milliseconds) noexcept
{
    receiver_buffer_average_.sample(
        now_microseconds, packets,
        payload_bytes, milliseconds);
}

void RuntimeStatisticsState::apply_average_buffers(
    RuntimeStatisticsInstantaneous& values) const noexcept
{
    values.sender_buffer_packets =
        sender_buffer_average_.packets();
    values.sender_buffer_bytes = narrowed<std::size_t>(
        saturated_add(sender_buffer_average_.payload_bytes(),
            saturated_multiply(
                values.sender_buffer_packets, packet_header_bytes_)));
    values.sender_buffer_milliseconds =
        sender_buffer_average_.milliseconds();
    values.receiver_buffer_packets =
        receiver_buffer_average_.packets();
    values.receiver_buffer_bytes =
        receiver_buffer_average_.payload_bytes();
    values.receiver_buffer_milliseconds =
        receiver_buffer_average_.milliseconds();
}

std::size_t RuntimeStatisticsState::average_received_payload_bytes() const noexcept
{
    if (total_.received_unique.packets == 0U) {
        return 0U;
    }
    return static_cast<std::size_t>(
        total_.received_unique.payload_bytes
        / total_.received_unique.packets);
}

RuntimeStatisticsSnapshot RuntimeStatisticsState::snapshot(
    std::uint64_t now_microseconds,
    RuntimeStatisticsInstantaneous instantaneous,
    bool clear_interval) noexcept
{
    RuntimeStatisticsSnapshot result{
        .timestamp_milliseconds =
            now_microseconds >= start_microseconds_
            ? (now_microseconds - start_microseconds_) / 1'000U
            : 0U,
        .interval_microseconds =
            now_microseconds >= interval_start_microseconds_
            ? now_microseconds - interval_start_microseconds_
            : 0U,
        .total = total_,
        .interval = interval_,
        .instantaneous = instantaneous,
        .reorder_distance_packets =
            reorder_distance_packets_,
        .reorder_tolerance_packets =
            reorder_tolerance_packets_,
        .average_belated_milliseconds =
            average_belated_milliseconds_,
        .packet_header_bytes = packet_header_bytes_,
    };
    if (clear_interval) {
        interval_ = {};
        interval_start_microseconds_ = now_microseconds;
    }
    return result;
}

void populate_trace_statistics(
    const RuntimeStatisticsSnapshot& source,
    SRT_TRACEBSTATS& destination) noexcept
{
    destination = {};
    const auto total_drop = combined_sender_drop(source.total);
    const auto interval_drop = combined_sender_drop(source.interval);

    destination.msTimeStamp =
        narrowed<std::int64_t>(source.timestamp_milliseconds);
    destination.pktSentTotal =
        narrowed<std::int64_t>(source.total.sent.packets);
    destination.pktRecvTotal =
        narrowed<std::int64_t>(source.total.received.packets);
    destination.pktSndLossTotal =
        narrowed<int>(source.total.sender_loss.packets);
    destination.pktRcvLossTotal =
        narrowed<int>(source.total.receiver_loss.packets);
    destination.pktRetransTotal =
        narrowed<int>(source.total.sent_retransmitted.packets);
    destination.pktSentACKTotal =
        narrowed<int>(source.total.sent_acknowledgements);
    destination.pktRecvACKTotal =
        narrowed<int>(source.total.received_acknowledgements);
    destination.pktSentNAKTotal =
        narrowed<int>(source.total.sent_loss_reports);
    destination.pktRecvNAKTotal =
        narrowed<int>(source.total.received_loss_reports);
    destination.usSndDurationTotal =
        narrowed<std::int64_t>(
            source.total.send_duration_microseconds);
    destination.pktSndDropTotal =
        narrowed<int>(total_drop.packets);
    destination.pktRcvDropTotal =
        narrowed<int>(source.total.receiver_dropped.packets);
    destination.pktRcvUndecryptTotal =
        narrowed<int>(
            source.total.receiver_undecryptable.packets);
    destination.pktRcvFilterExtraTotal =
        narrowed<int>(
            source.total.receiver_filter_extra);
    destination.pktSndFilterExtraTotal =
        narrowed<int>(
            source.total.sender_filter_extra);
    destination.pktRcvFilterSupplyTotal =
        narrowed<int>(
            source.total.receiver_filter_supply);
    destination.pktRcvFilterLossTotal =
        narrowed<int>(
            source.total.receiver_filter_loss);
    const std::uint64_t packet_header_bytes =
        source.packet_header_bytes;
    destination.byteSentTotal =
        source.total.sent.wire_bytes(packet_header_bytes);
    destination.byteRecvTotal =
        source.total.received.wire_bytes(packet_header_bytes);
    destination.byteRcvLossTotal =
        source.total.receiver_loss.wire_bytes(packet_header_bytes);
    destination.byteRetransTotal =
        source.total.sent_retransmitted.wire_bytes(
            packet_header_bytes);
    destination.byteSndDropTotal =
        total_drop.wire_bytes(packet_header_bytes);
    destination.byteRcvDropTotal =
        source.total.receiver_dropped.wire_bytes(
            packet_header_bytes);
    destination.byteRcvUndecryptTotal =
        source.total.receiver_undecryptable.payload_bytes;

    destination.pktSent =
        narrowed<std::int64_t>(source.interval.sent.packets);
    destination.pktRecv =
        narrowed<std::int64_t>(source.interval.received.packets);
    destination.pktSndLoss =
        narrowed<int>(source.interval.sender_loss.packets);
    destination.pktRcvLoss =
        narrowed<int>(source.interval.receiver_loss.packets);
    destination.pktRetrans =
        narrowed<int>(source.interval.sent_retransmitted.packets);
    destination.pktRcvRetrans =
        narrowed<int>(
            source.interval.received_retransmitted.packets);
    destination.pktReorderDistance =
        narrowed<int>(source.reorder_distance_packets);
    destination.pktReorderTolerance =
        narrowed<int>(source.reorder_tolerance_packets);
    destination.pktRcvAvgBelatedTime =
        source.average_belated_milliseconds;
    destination.pktRcvBelated =
        narrowed<std::int64_t>(
            source.interval.receiver_belated.packets);
    destination.pktSentACK =
        narrowed<int>(source.interval.sent_acknowledgements);
    destination.pktRecvACK =
        narrowed<int>(source.interval.received_acknowledgements);
    destination.pktSentNAK =
        narrowed<int>(source.interval.sent_loss_reports);
    destination.pktRecvNAK =
        narrowed<int>(source.interval.received_loss_reports);
    if (source.interval_microseconds != 0U) {
        destination.mbpsSendRate =
            static_cast<double>(
                source.interval.sent.wire_bytes(
                    packet_header_bytes)) * 8.0
            / static_cast<double>(source.interval_microseconds);
        destination.mbpsRecvRate =
            static_cast<double>(
                source.interval.received.wire_bytes(
                    packet_header_bytes)) * 8.0
            / static_cast<double>(source.interval_microseconds);
    }
    destination.usSndDuration =
        narrowed<std::int64_t>(
            source.interval.send_duration_microseconds);
    destination.pktSndDrop =
        narrowed<int>(interval_drop.packets);
    destination.pktRcvDrop =
        narrowed<int>(source.interval.receiver_dropped.packets);
    destination.pktRcvUndecrypt =
        narrowed<int>(
            source.interval.receiver_undecryptable.packets);
    destination.pktRcvFilterExtra =
        narrowed<int>(
            source.interval.receiver_filter_extra);
    destination.pktSndFilterExtra =
        narrowed<int>(
            source.interval.sender_filter_extra);
    destination.pktRcvFilterSupply =
        narrowed<int>(
            source.interval.receiver_filter_supply);
    destination.pktRcvFilterLoss =
        narrowed<int>(
            source.interval.receiver_filter_loss);
    destination.byteSent =
        source.interval.sent.wire_bytes(packet_header_bytes);
    destination.byteRecv =
        source.interval.received.wire_bytes(packet_header_bytes);
    destination.byteRcvLoss =
        source.interval.receiver_loss.wire_bytes(
            packet_header_bytes);
    destination.byteRetrans =
        source.interval.sent_retransmitted.wire_bytes(
            packet_header_bytes);
    destination.byteSndDrop =
        interval_drop.wire_bytes(packet_header_bytes);
    destination.byteRcvDrop =
        source.interval.receiver_dropped.wire_bytes(
            packet_header_bytes);
    destination.byteRcvUndecrypt =
        source.interval.receiver_undecryptable.payload_bytes;

    const auto& instant = source.instantaneous;
    destination.usPktSndPeriod =
        instant.packet_send_period_microseconds;
    destination.pktFlowWindow =
        narrowed<int>(instant.flow_window_packets);
    destination.pktCongestionWindow =
        narrowed<int>(instant.congestion_window_packets);
    destination.pktFlightSize =
        narrowed<int>(instant.packets_in_flight);
    destination.msRTT = instant.round_trip_time_milliseconds;
    destination.mbpsBandwidth =
        instant.bandwidth_megabits_per_second;
    destination.byteAvailSndBuf =
        narrowed<int>(instant.sender_buffer_available_bytes);
    destination.byteAvailRcvBuf =
        narrowed<int>(instant.receiver_buffer_available_bytes);
    destination.mbpsMaxBW =
        instant.maximum_bandwidth_megabits_per_second;
    destination.byteMSS =
        narrowed<int>(instant.maximum_segment_size_bytes);
    destination.pktSndBuf =
        narrowed<int>(instant.sender_buffer_packets);
    destination.byteSndBuf =
        narrowed<int>(instant.sender_buffer_bytes);
    destination.msSndBuf =
        narrowed<int>(instant.sender_buffer_milliseconds);
    destination.msSndTsbPdDelay =
        narrowed<int>(instant.sender_tsbpd_delay_milliseconds);
    destination.pktRcvBuf =
        narrowed<int>(instant.receiver_buffer_packets);
    destination.byteRcvBuf =
        narrowed<int>(instant.receiver_buffer_bytes);
    destination.msRcvBuf =
        narrowed<int>(instant.receiver_buffer_milliseconds);
    destination.msRcvTsbPdDelay =
        narrowed<int>(instant.receiver_tsbpd_delay_milliseconds);

    destination.pktSentUniqueTotal =
        narrowed<std::int64_t>(source.total.sent_unique.packets);
    destination.pktRecvUniqueTotal =
        narrowed<std::int64_t>(
            source.total.received_unique.packets);
    destination.byteSentUniqueTotal =
        source.total.sent_unique.wire_bytes(packet_header_bytes);
    destination.byteRecvUniqueTotal =
        source.total.received_unique.wire_bytes(
            packet_header_bytes);
    destination.pktSentUnique =
        narrowed<std::int64_t>(
            source.interval.sent_unique.packets);
    destination.pktRecvUnique =
        narrowed<std::int64_t>(
            source.interval.received_unique.packets);
    destination.byteSentUnique =
        source.interval.sent_unique.wire_bytes(
            packet_header_bytes);
    destination.byteRecvUnique =
        source.interval.received_unique.wire_bytes(
            packet_header_bytes);
}

} // namespace robotweax::srt::compat
