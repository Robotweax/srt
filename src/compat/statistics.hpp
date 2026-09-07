#pragma once

#include "robotweax/srt/packet.hpp"
#include "srt/srt.h"

#include <cstddef>
#include <cstdint>

namespace robotweax::srt::compat {

inline constexpr std::uint64_t
    ipv4_statistics_packet_header_bytes = 44;
inline constexpr std::uint64_t
    ipv6_statistics_packet_header_bytes = 64;

struct PacketByteCounter {
    std::uint64_t packets = 0;
    std::uint64_t payload_bytes = 0;

    void add(std::uint64_t packet_count,
        std::uint64_t bytes) noexcept;
    [[nodiscard]] std::uint64_t wire_bytes(
        std::uint64_t packet_header_bytes =
            ipv4_statistics_packet_header_bytes) const noexcept;
};

struct StatisticsCounters {
    PacketByteCounter sent;
    PacketByteCounter received;
    PacketByteCounter sent_unique;
    PacketByteCounter received_unique;
    PacketByteCounter sent_retransmitted;
    PacketByteCounter received_retransmitted;
    PacketByteCounter sender_loss;
    PacketByteCounter receiver_loss;
    PacketByteCounter sender_message_ttl_dropped;
    PacketByteCounter sender_tlpktdrop_dropped;
    PacketByteCounter receiver_dropped;
    PacketByteCounter receiver_undecryptable;
    PacketByteCounter receiver_belated;
    std::uint64_t sent_acknowledgements = 0;
    std::uint64_t received_acknowledgements = 0;
    std::uint64_t sent_loss_reports = 0;
    std::uint64_t received_loss_reports = 0;
    std::uint64_t sender_filter_extra = 0;
    std::uint64_t receiver_filter_extra = 0;
    std::uint64_t receiver_filter_supply = 0;
    std::uint64_t receiver_filter_loss = 0;
    std::uint64_t send_duration_microseconds = 0;
};

struct BufferStatisticsSample {
    std::size_t sender_packets = 0;
    std::size_t sender_payload_bytes = 0;
    std::uint64_t sender_milliseconds = 0;
    std::size_t receiver_packets = 0;
    std::size_t receiver_payload_bytes = 0;
    std::uint64_t receiver_milliseconds = 0;
};

struct RuntimeStatisticsInstantaneous {
    double packet_send_period_microseconds = 0.0;
    std::size_t flow_window_packets = 0;
    std::size_t congestion_window_packets = 0;
    std::size_t packets_in_flight = 0;
    double round_trip_time_milliseconds = 0.0;
    double bandwidth_megabits_per_second = 0.0;
    double maximum_bandwidth_megabits_per_second = 0.0;
    std::size_t maximum_segment_size_bytes = 1'500;
    std::size_t sender_buffer_packets = 0;
    std::size_t sender_buffer_bytes = 0;
    std::uint64_t sender_buffer_milliseconds = 0;
    std::size_t sender_buffer_available_bytes = 0;
    std::uint32_t sender_tsbpd_delay_milliseconds = 0;
    std::size_t receiver_buffer_packets = 0;
    std::size_t receiver_buffer_bytes = 0;
    std::uint64_t receiver_buffer_milliseconds = 0;
    std::size_t receiver_buffer_available_bytes = 0;
    std::uint32_t receiver_tsbpd_delay_milliseconds = 0;
};

struct RuntimeStatisticsSnapshot {
    std::uint64_t timestamp_milliseconds = 0;
    std::uint64_t interval_microseconds = 0;
    StatisticsCounters total;
    StatisticsCounters interval;
    RuntimeStatisticsInstantaneous instantaneous;
    std::size_t reorder_distance_packets = 0;
    std::size_t reorder_tolerance_packets = 0;
    double average_belated_milliseconds = 0.0;
    std::uint64_t packet_header_bytes =
        ipv4_statistics_packet_header_bytes;
};

struct SenderBufferStatus {
    std::size_t packets = 0;
    std::size_t bytes = 0;
    std::uint64_t milliseconds = 0;
};

class RuntimeStatisticsState {
public:
    explicit RuntimeStatisticsState(
        std::uint64_t start_microseconds = 0,
        std::uint64_t packet_header_bytes =
            ipv4_statistics_packet_header_bytes) noexcept;

    void note_data_sent(
        std::size_t payload_bytes, bool retransmitted) noexcept;
    void note_data_received(
        std::size_t payload_bytes,
        bool accepted_unique,
        bool retransmitted) noexcept;
    void note_sender_loss(
        std::size_t packets, std::size_t payload_bytes) noexcept;
    void note_receiver_loss(
        std::size_t packets, std::size_t estimated_payload_bytes) noexcept;
    void note_sender_message_ttl_drop(
        std::size_t packets, std::size_t payload_bytes) noexcept;
    void note_sender_tlpktdrop(
        std::size_t packets, std::size_t payload_bytes) noexcept;
    void note_receiver_drop(
        std::size_t packets, std::size_t estimated_payload_bytes) noexcept;
    void note_receiver_undecryptable(
        std::size_t payload_bytes) noexcept;
    void note_receiver_belated(
        std::size_t payload_bytes,
        std::uint64_t delay_microseconds) noexcept;
    void note_sender_filter_extra(
        std::size_t payload_bytes) noexcept;
    void note_receiver_filter_extra() noexcept;
    void note_receiver_filter_supply(
        std::size_t payload_bytes,
        bool accepted_unique) noexcept;
    void note_receiver_filter_loss(
        std::size_t packets) noexcept;
    void note_reorder_distance(
        std::size_t distance_packets) noexcept;
    void update_reorder_state(
        std::size_t distance_packets,
        std::size_t tolerance_packets) noexcept;
    void note_control_sent(ControlType type) noexcept;
    void note_control_received(ControlType type) noexcept;
    void update_send_duration(
        std::uint64_t now_microseconds, bool sender_buffer_nonempty) noexcept;
    void sample_buffers(
        std::uint64_t now_microseconds,
        BufferStatisticsSample sample) noexcept;
    void sample_sender_buffer(
        std::uint64_t now_microseconds,
        std::size_t packets,
        std::size_t payload_bytes,
        std::uint64_t milliseconds) noexcept;
    void sample_receiver_buffer(
        std::uint64_t now_microseconds,
        std::size_t packets,
        std::size_t payload_bytes,
        std::uint64_t milliseconds) noexcept;
    void apply_average_buffers(
        RuntimeStatisticsInstantaneous& values) const noexcept;

    [[nodiscard]] std::size_t average_received_payload_bytes() const noexcept;
    [[nodiscard]] RuntimeStatisticsSnapshot snapshot(
        std::uint64_t now_microseconds,
        RuntimeStatisticsInstantaneous instantaneous,
        bool clear_interval) noexcept;

private:
    class BufferMovingAverage {
    public:
        void sample(
            std::uint64_t now_microseconds,
            std::size_t packets,
            std::size_t payload_bytes,
            std::uint64_t milliseconds) noexcept;
        [[nodiscard]] std::size_t packets() const noexcept;
        [[nodiscard]] std::size_t payload_bytes() const noexcept;
        [[nodiscard]] std::uint64_t milliseconds() const noexcept;

    private:
        std::uint64_t last_sample_microseconds_ = 0;
        double packets_ = 0.0;
        double payload_bytes_ = 0.0;
        double milliseconds_ = 0.0;
        bool initialized_ = false;
    };

    using PacketMember = PacketByteCounter StatisticsCounters::*;
    using CountMember = std::uint64_t StatisticsCounters::*;

    void add(PacketMember member,
        std::uint64_t packets, std::uint64_t payload_bytes) noexcept;
    void increment(CountMember member) noexcept;
    void add_count(
        CountMember member, std::uint64_t count) noexcept;

    StatisticsCounters total_{};
    StatisticsCounters interval_{};
    std::uint64_t start_microseconds_ = 0;
    std::uint64_t interval_start_microseconds_ = 0;
    std::uint64_t duration_update_microseconds_ = 0;
    BufferMovingAverage sender_buffer_average_;
    BufferMovingAverage receiver_buffer_average_;
    std::size_t reorder_distance_packets_ = 0;
    std::size_t reorder_tolerance_packets_ = 0;
    double average_belated_milliseconds_ = 0.0;
    std::uint64_t packet_header_bytes_ =
        ipv4_statistics_packet_header_bytes;
    bool sender_busy_ = false;
};

void populate_trace_statistics(
    const RuntimeStatisticsSnapshot& source,
    SRT_TRACEBSTATS& destination) noexcept;

} // namespace robotweax::srt::compat
