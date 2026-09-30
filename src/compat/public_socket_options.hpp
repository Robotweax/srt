#pragma once

#include "robotweax/srt/socket_options.hpp"
#include "robotweax/srt/handshake_extensions.hpp"
#include "srt/srt.h"

#include <array>
#include <cstdint>

namespace robotweax::srt::compat {

struct PublicSocketOptions {
    bool send_synchronous = true;
    bool receive_synchronous = true;
    std::int32_t send_timeout_milliseconds = -1;
    std::int32_t receive_timeout_milliseconds = -1;
    std::int32_t flow_window_packets = 25'600;
    std::int32_t send_buffer_bytes = 12'058'624;
    std::int32_t receive_buffer_bytes = 12'058'624;
    std::int32_t udp_send_buffer_bytes = 65'536;
    std::int32_t udp_receive_buffer_bytes = 12'288'000;
    std::int64_t maximum_bandwidth_bytes_per_second = -1;
    std::int64_t input_bandwidth_bytes_per_second = 0;
    std::int64_t minimum_input_bandwidth_bytes_per_second = 0;
    std::int32_t overhead_bandwidth_percent = 25;
    bool tsbpd_mode = true;
    bool drift_tracer = true;
    bool too_late_packet_drop = true;
    std::int32_t sender_drop_delay_milliseconds = 0;
    bool periodic_nak = true;
    std::int32_t maximum_reorder_tolerance_packets = 0;
    std::int32_t receiver_latency_milliseconds = 120;
    std::int32_t peer_latency_milliseconds = 0;
    std::int32_t maximum_segment_size = 1'500;
    std::int32_t maximum_payload_size = SRT_LIVE_DEF_PLSIZE;
    std::int32_t retransmission_algorithm = 1;
    bool message_api = true;
    bool data_sender = false;
    SRT_TRANSTYPE transmission_type = SRTT_LIVE;
    std::int32_t connection_timeout_milliseconds = 3'000;
    std::int32_t minimum_peer_srt_version = 0x0001'0000;
    std::int32_t peer_idle_timeout_milliseconds = 5'000;
    bool linger_enabled = false;
    std::int32_t linger_seconds = 0;
    bool rendezvous = false;
    bool reuse_address = true;
    bool group_connect = false;
    std::int32_t ipv6_only = -1;
    std::int32_t ip_time_to_live = 64;
    std::int32_t ip_type_of_service = 0xB8;
    bool ip_type_of_service_explicit = false;
    std::array<char, maximum_network_device_name_size> bound_device {};
    std::uint8_t bound_device_size = 0;
    StreamId stream_id {};
};

} // namespace robotweax::srt::compat
