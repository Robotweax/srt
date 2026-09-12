#include "test.hpp"

#include "robotweax/srt/codec.hpp"
#include "compat/socket_registry.hpp"
#include "compat/statistics.hpp"
#include "compat/transport_runtime.hpp"
#include "srt/srt.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>

using namespace robotweax::srt;
using namespace robotweax::srt::compat;

namespace {

std::uint64_t statistics_now(void* context) noexcept
{
    return *static_cast<const std::uint64_t*>(context);
}

UdpIoResult accept_statistics_datagram(
    std::span<const std::byte> bytes,
    Ipv4Endpoint,
    void*) noexcept
{
    return {.bytes_transferred = bytes.size()};
}

} // namespace

TEST(statistics_snapshot_maps_totals_interval_and_clear_semantics)
{
    RuntimeStatisticsState state{100};
    state.note_data_sent(100, false);
    state.note_data_sent(60, true);
    state.note_data_received(80, true, false);
    state.note_data_received(80, false, true);
    state.note_sender_loss(3, 180);
    state.note_receiver_loss(2, 160);
    state.note_sender_message_ttl_drop(2, 200);
    state.note_sender_tlpktdrop(1, 50);
    state.note_receiver_drop(1, 80);
    state.note_receiver_belated(80, 10'000);
    state.note_receiver_belated(80, 20'000);
    state.note_receiver_filter_extra();
    state.note_reorder_distance(4);
    state.note_reorder_distance(2);
    state.update_reorder_state(4, 3);
    state.note_control_sent(ControlType::acknowledgement);
    state.note_control_received(ControlType::acknowledgement);
    state.note_control_sent(ControlType::negative_acknowledgement);
    state.note_control_received(ControlType::negative_acknowledgement);
    state.update_send_duration(120, true);
    state.update_send_duration(220, true);

    RuntimeStatisticsInstantaneous instantaneous{
        .packet_send_period_microseconds = 125.0,
        .flow_window_packets = 256,
        .congestion_window_packets = 32,
        .packets_in_flight = 2,
        .round_trip_time_milliseconds = 42.5,
        .bandwidth_megabits_per_second = 12.25,
        .maximum_bandwidth_megabits_per_second = 20.0,
        .maximum_segment_size_bytes = 1'500,
        .sender_buffer_packets = 2,
        .sender_buffer_bytes = 248,
        .sender_buffer_milliseconds = 7,
        .sender_buffer_available_bytes = 4'096,
        .sender_tsbpd_delay_milliseconds = 120,
        .receiver_buffer_packets = 1,
        .receiver_buffer_bytes = 124,
        .receiver_buffer_milliseconds = 3,
        .receiver_buffer_available_bytes = 8'192,
        .receiver_tsbpd_delay_milliseconds = 140,
    };
    const auto first = state.snapshot(
        1'100, instantaneous, true);
    REQUIRE_EQ(
        first.total.sender_message_ttl_dropped.packets, 2U);
    REQUIRE_EQ(
        first.total.sender_tlpktdrop_dropped.packets, 1U);

    SRT_TRACEBSTATS public_first{};
    populate_trace_statistics(first, public_first);
    REQUIRE_EQ(public_first.msTimeStamp, 1);
    REQUIRE_EQ(public_first.pktSentTotal, 2);
    REQUIRE_EQ(public_first.pktRecvTotal, 2);
    REQUIRE_EQ(public_first.pktSndLossTotal, 3);
    REQUIRE_EQ(public_first.pktRcvLossTotal, 2);
    REQUIRE_EQ(public_first.pktRetransTotal, 1);
    REQUIRE_EQ(public_first.pktRcvRetrans, 1);
    REQUIRE_EQ(public_first.pktReorderDistance, 4);
    REQUIRE_EQ(public_first.pktReorderTolerance, 3);
    REQUIRE_EQ(public_first.pktRcvAvgBelatedTime, 12.0);
    REQUIRE_EQ(public_first.pktRcvBelated, 2);
    REQUIRE_EQ(public_first.pktRcvFilterExtraTotal, 1);
    REQUIRE_EQ(public_first.pktRcvFilterExtra, 1);
    REQUIRE_EQ(public_first.pktSentACKTotal, 1);
    REQUIRE_EQ(public_first.pktRecvACKTotal, 1);
    REQUIRE_EQ(public_first.pktSentNAKTotal, 1);
    REQUIRE_EQ(public_first.pktRecvNAKTotal, 1);
    REQUIRE_EQ(public_first.usSndDurationTotal, 100);
    REQUIRE_EQ(public_first.pktSndDropTotal, 3);
    REQUIRE_EQ(public_first.byteSentTotal, 248U);
    REQUIRE_EQ(public_first.byteRecvTotal, 248U);
    REQUIRE_EQ(public_first.byteRcvLossTotal, 248U);
    REQUIRE_EQ(public_first.byteRetransTotal, 104U);
    REQUIRE_EQ(public_first.byteSndDropTotal, 382U);
    REQUIRE_EQ(public_first.byteRcvDropTotal, 124U);
    REQUIRE_EQ(public_first.pktSentUniqueTotal, 1);
    REQUIRE_EQ(public_first.pktRecvUniqueTotal, 1);
    REQUIRE_EQ(public_first.byteSentUniqueTotal, 144U);
    REQUIRE_EQ(public_first.byteRecvUniqueTotal, 124U);
    REQUIRE_EQ(public_first.usPktSndPeriod, 125.0);
    REQUIRE_EQ(public_first.pktFlowWindow, 256);
    REQUIRE_EQ(public_first.pktCongestionWindow, 32);
    REQUIRE_EQ(public_first.pktFlightSize, 2);
    REQUIRE_EQ(public_first.msRTT, 42.5);
    REQUIRE_EQ(public_first.pktSndBuf, 2);
    REQUIRE_EQ(public_first.byteSndBuf, 248);
    REQUIRE_EQ(public_first.msSndBuf, 7);
    REQUIRE_EQ(public_first.pktRcvBuf, 1);
    REQUIRE_EQ(public_first.byteRcvBuf, 124);

    const auto after_clear = state.snapshot(
        1'200, {}, false);
    SRT_TRACEBSTATS public_after_clear{};
    populate_trace_statistics(
        after_clear, public_after_clear);
    REQUIRE_EQ(public_after_clear.pktSentTotal, 2);
    REQUIRE_EQ(public_after_clear.pktSndDropTotal, 3);
    REQUIRE_EQ(public_after_clear.pktSent, 0);
    REQUIRE_EQ(public_after_clear.pktSndDrop, 0);
    REQUIRE_EQ(public_after_clear.pktRcvBelated, 0);
    REQUIRE_EQ(public_after_clear.pktRcvFilterExtraTotal, 1);
    REQUIRE_EQ(public_after_clear.pktRcvFilterExtra, 0);
    REQUIRE_EQ(public_after_clear.pktReorderDistance, 4);
    REQUIRE_EQ(public_after_clear.pktReorderTolerance, 3);
    REQUIRE_EQ(
        public_after_clear.pktRcvAvgBelatedTime, 12.0);
    REQUIRE_EQ(public_after_clear.byteSent, 0U);
}

TEST(statistics_buffer_average_saturates_at_size_t_limit)
{
    RuntimeStatisticsState state {1'000};
    state.sample_buffers(1'000,
        {
            .sender_packets = 1,
            .sender_payload_bytes = std::numeric_limits<std::size_t>::max(),
        });
    RuntimeStatisticsInstantaneous values {};
    state.apply_average_buffers(values);
    REQUIRE_EQ(
        values.sender_buffer_bytes, std::numeric_limits<std::size_t>::max());
}

TEST(statistics_buffer_average_uses_reference_time_weighting)
{
    RuntimeStatisticsState state{1'000};
    state.sample_buffers(1'000, {
        .sender_packets = 4,
        .sender_payload_bytes = 400,
        .sender_milliseconds = 10,
        .receiver_packets = 2,
        .receiver_payload_bytes = 100,
        .receiver_milliseconds = 5,
    });
    state.sample_buffers(21'000, {});

    RuntimeStatisticsInstantaneous selected{};
    state.apply_average_buffers(selected);
    REQUIRE_EQ(selected.sender_buffer_packets, 4U);
    REQUIRE_EQ(selected.sender_buffer_bytes, 576U);
    REQUIRE_EQ(selected.receiver_buffer_packets, 2U);
    REQUIRE_EQ(selected.receiver_buffer_bytes, 100U);

    state.sample_buffers(501'000, {});
    selected = {};
    state.apply_average_buffers(selected);
    REQUIRE_EQ(selected.sender_buffer_packets, 2U);
    REQUIRE_EQ(selected.sender_buffer_bytes, 288U);
    REQUIRE_EQ(selected.sender_buffer_milliseconds, 5U);
    REQUIRE_EQ(selected.receiver_buffer_packets, 1U);
    REQUIRE_EQ(selected.receiver_buffer_bytes, 50U);
    REQUIRE_EQ(selected.receiver_buffer_milliseconds, 3U);

    state.sample_buffers(1'502'000, {
        .sender_packets = 1,
        .sender_payload_bytes = 10,
        .sender_milliseconds = 1,
    });
    selected = {};
    state.apply_average_buffers(selected);
    REQUIRE_EQ(selected.sender_buffer_packets, 1U);
    REQUIRE_EQ(selected.sender_buffer_bytes, 54U);
    REQUIRE_EQ(selected.receiver_buffer_packets, 0U);
}

TEST(statistics_ipv6_wire_bytes_include_the_larger_ip_header)
{
    RuntimeStatisticsState state{
        0, ipv6_statistics_packet_header_bytes};
    state.note_data_sent(100U, false);
    state.note_data_received(80U, true, false);
    state.sample_sender_buffer(1'000, 1U, 100U, 1U);

    RuntimeStatisticsInstantaneous instantaneous{
        .maximum_segment_size_bytes = 1'280,
        .sender_buffer_packets = 1,
        .sender_buffer_bytes = 164,
    };
    const auto snapshot = state.snapshot(
        1'000, instantaneous, false);
    SRT_TRACEBSTATS public_statistics{};
    populate_trace_statistics(snapshot, public_statistics);

    REQUIRE_EQ(public_statistics.byteSentTotal, 164U);
    REQUIRE_EQ(public_statistics.byteRecvTotal, 144U);
    REQUIRE_EQ(public_statistics.byteSentUniqueTotal, 164U);
    REQUIRE_EQ(public_statistics.byteRecvUniqueTotal, 144U);
    REQUIRE_EQ(public_statistics.byteMSS, 1'280);

    RuntimeStatisticsInstantaneous averaged{};
    state.apply_average_buffers(averaged);
    REQUIRE_EQ(averaged.sender_buffer_bytes, 164U);
}

TEST(statistics_maps_sender_supply_and_loss_filter_counters)
{
    RuntimeStatisticsState state;
    state.note_sender_filter_extra(100U);
    state.note_data_received(104U, false, false);
    state.note_receiver_filter_extra();
    state.note_receiver_filter_supply(20U, true);
    state.note_receiver_filter_loss(2U);

    const auto snapshot = state.snapshot(1'000, {}, false);
    SRT_TRACEBSTATS public_statistics{};
    populate_trace_statistics(
        snapshot, public_statistics);
    REQUIRE_EQ(public_statistics.pktSentTotal, 1);
    REQUIRE_EQ(public_statistics.pktSentUniqueTotal, 0);
    REQUIRE_EQ(public_statistics.pktRecvTotal, 1);
    REQUIRE_EQ(public_statistics.pktRecvUniqueTotal, 1);
    REQUIRE_EQ(
        public_statistics.pktSndFilterExtraTotal, 1);
    REQUIRE_EQ(
        public_statistics.pktRcvFilterExtraTotal, 1);
    REQUIRE_EQ(
        public_statistics.pktRcvFilterSupplyTotal, 1);
    REQUIRE_EQ(
        public_statistics.pktRcvFilterLossTotal, 2);
    REQUIRE_EQ(public_statistics.pktSndFilterExtra, 1);
    REQUIRE_EQ(public_statistics.pktRcvFilterExtra, 1);
    REQUIRE_EQ(public_statistics.pktRcvFilterSupply, 1);
    REQUIRE_EQ(public_statistics.pktRcvFilterLoss, 2);
}

TEST(srt_statistics_api_reports_runtime_data_and_sender_buffer)
{
    REQUIRE_EQ(srt_startup(), 0);
    const SRTSOCKET socket = srt_create_socket();
    REQUIRE(socket != SRT_INVALID_SOCK);

    SRT_TRACEBSTATS statistics{};
    REQUIRE_EQ(srt_bstats(socket, &statistics, 0), SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ENOCONN);
    REQUIRE_EQ(
        srt_getsndbuffer(socket, nullptr, nullptr), SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ENOCONN);

    const auto record =
        SocketRegistry::instance().find(socket);
    REQUIRE(record != nullptr);
    const auto channel = std::make_shared<DatagramChannel>();
    channel->set_send_hook_for_testing(
        accept_statistics_datagram, nullptr);
    std::uint64_t now = 1'000;
    SocketOptions options;
    REQUIRE_EQ(options.set(
                   SocketOption::maximum_reorder_tolerance_packets,
                   5),
        Error::none);
    const auto runtime =
        std::make_shared<ConnectionRuntime>(
            ConnectionRuntime::Configuration{
                .channel = channel,
                .peer = {
                    .address = {192, 0, 2, 90},
                    .port = 19'000,
                },
                .peer_socket_id = 900,
                .initial_sequence = SequenceNumber{90},
                .flow_window_packets = 256,
                .options = options,
                .origin = ConnectionRuntime::Clock::now(),
                .now_function = statistics_now,
                .now_context = &now,
            });
    {
        std::lock_guard lock(record->mutex);
        record->state = SRTS_CONNECTED;
        record->runtime = runtime;
        record->channel = channel;
    }

    const std::array<std::byte, 3> payload{
        std::byte{1}, std::byte{2}, std::byte{3}};
    REQUIRE_EQ(runtime->queue_message(
                   payload, 0, true, false, -1)
                   .status,
        MessageIoStatus::success);
    now = 6'000;

    std::size_t blocks = 0;
    std::size_t bytes = 0;
    REQUIRE_EQ(
        srt_getsndbuffer(socket, &blocks, &bytes), 1);
    REQUIRE_EQ(blocks, 1U);
    REQUIRE_EQ(bytes, payload.size());

    (void)runtime->poll();
    REQUIRE_EQ(srt_bistats(
                   socket, &statistics, 1, 1),
        0);
    REQUIRE_EQ(statistics.pktSentTotal, 1);
    REQUIRE_EQ(statistics.pktSent, 1);
    REQUIRE_EQ(statistics.pktSentUniqueTotal, 1);
    REQUIRE_EQ(statistics.byteSentTotal, 47U);
    REQUIRE_EQ(statistics.pktSndBuf, 1);
    REQUIRE_EQ(statistics.byteSndBuf, 47);
    REQUIRE_EQ(statistics.msSndBuf, 1);
    REQUIRE_EQ(statistics.pktReorderTolerance, 0);

    std::int32_t maximum_reorder_tolerance = 3;
    REQUIRE_EQ(srt_setsockflag(
                   socket, SRTO_LOSSMAXTTL,
                   &maximum_reorder_tolerance,
                   static_cast<int>(
                       sizeof(maximum_reorder_tolerance))),
        0);
    SRT_TRACEBSTATS after_dynamic_option{};
    REQUIRE_EQ(srt_bistats(
                   socket, &after_dynamic_option, 0, 1),
        0);
    REQUIRE_EQ(
        after_dynamic_option.pktReorderTolerance, 0);

    SRT_TRACEBSTATS after_clear{};
    REQUIRE_EQ(srt_bstats(
                   socket, &after_clear, 0),
        0);
    REQUIRE_EQ(after_clear.pktSentTotal, 1);
    REQUIRE_EQ(after_clear.pktSent, 0);
    REQUIRE_EQ(after_clear.byteSent, 0U);

    ReliabilityAction acknowledgement;
    acknowledgement.kind =
        ReliabilityActionKind::acknowledgement;
    acknowledgement.acknowledgement.kind =
        AcknowledgementKind::lite;
    acknowledgement.acknowledgement.next_sequence =
        SequenceNumber{91};
    std::array<std::byte, 1'500> acknowledgement_storage{};
    const auto encoded_acknowledgement =
        encode_reliability_action(
            acknowledgement, PacketTimestamp{0}, 900,
            acknowledgement_storage);
    REQUIRE(encoded_acknowledgement);
    const auto decoded_acknowledgement = decode_packet(
        std::span{acknowledgement_storage}.first(
            encoded_acknowledgement.bytes_written));
    REQUIRE(decoded_acknowledgement);
    now = 251'000;
    runtime->process_packet(
        decoded_acknowledgement.packet, {
            .address = {192, 0, 2, 90},
            .port = 19'000,
        });

    SRT_TRACEBSTATS instantaneous{};
    REQUIRE_EQ(srt_bistats(
                   socket, &instantaneous, 0, 1),
        0);
    REQUIRE_EQ(instantaneous.pktSndBuf, 0);
    REQUIRE_EQ(instantaneous.byteSndBuf, 0);
    SRT_TRACEBSTATS averaged{};
    REQUIRE_EQ(srt_bstats(socket, &averaged, 0), 0);
    REQUIRE_EQ(averaged.pktSndBuf, 1);
    REQUIRE_EQ(averaged.byteSndBuf, 46);

    REQUIRE_EQ(srt_close(socket), 0);
    REQUIRE_EQ(srt_cleanup(), 0);
}
