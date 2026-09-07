#include "test.hpp"

#include "robotweax/srt/socket_options.hpp"

using namespace robotweax::srt;

TEST(socket_options_use_reference_srt_buffer_capacities)
{
    const SocketOptions options;
    REQUIRE_EQ(options.send_buffer_packets(), 8'192U);
    REQUIRE_EQ(options.receive_buffer_packets(), 8'192U);
}

TEST(socket_options_validate_every_public_value_category)
{
    SocketOptions options;
    REQUIRE_EQ(options.get(
            SocketOption::minimum_input_bandwidth_bytes_per_second).value,
        0);
    REQUIRE_EQ(options.get(
            SocketOption::maximum_bandwidth_bytes_per_second).value,
        -1);
    REQUIRE_EQ(options.get(SocketOption::drift_tracer).value, 1);
    REQUIRE_EQ(options.set(SocketOption::input_bandwidth_bytes_per_second,
                   200'000),
        Error::none);
    REQUIRE_EQ(options.set(
                   SocketOption::minimum_input_bandwidth_bytes_per_second,
                   150'000),
        Error::none);
    REQUIRE_EQ(options.set(SocketOption::maximum_bandwidth_bytes_per_second,
                   240'000),
        Error::none);
    REQUIRE_EQ(options.set(SocketOption::overhead_bandwidth_percent, 20),
        Error::none);
    REQUIRE_EQ(options.set(SocketOption::receiver_latency_milliseconds, 300),
        Error::none);
    REQUIRE_EQ(options.set(SocketOption::maximum_payload_size, 1'316),
        Error::none);
    REQUIRE_EQ(options.set(
                   SocketOption::maximum_reorder_tolerance_packets,
                   10),
        Error::none);
    REQUIRE_EQ(options.set(SocketOption::overhead_bandwidth_percent, 101),
        Error::invalid_state);
    REQUIRE_EQ(options.set(SocketOption::tsbpd_mode, 2), Error::invalid_state);
    REQUIRE_EQ(options.set(SocketOption::drift_tracer, 2),
        Error::invalid_state);
    REQUIRE_EQ(options.set(
                   SocketOption::minimum_input_bandwidth_bytes_per_second,
                   -1),
        Error::invalid_state);
    REQUIRE_EQ(options.set(
                   SocketOption::maximum_bandwidth_bytes_per_second,
                   -2),
        Error::invalid_state);
    REQUIRE_EQ(options.set(SocketOption::maximum_payload_size, 2'000),
        Error::invalid_state);
    REQUIRE_EQ(options.set(
                   SocketOption::maximum_reorder_tolerance_packets,
                   -1),
        Error::invalid_state);

    REQUIRE_EQ(options.get(SocketOption::input_bandwidth_bytes_per_second).value,
        200'000);
    const auto rate = options.live_rate_configuration();
    REQUIRE_EQ(rate.maximum_bandwidth_bytes_per_second, 240'000U);
    REQUIRE_EQ(rate.minimum_input_bandwidth_bytes_per_second, 150'000U);
    REQUIRE_EQ(rate.overhead_percent, 20U);
    REQUIRE_EQ(rate.maximum_payload_size, 1'316U);
    REQUIRE_EQ(options.get(
            SocketOption::maximum_reorder_tolerance_packets).value,
        10);
}

TEST(socket_options_apply_mss_and_ipv6_header_limits)
{
    SocketOptions options;
    REQUIRE_EQ(options.maximum_segment_size(), 1'500U);
    REQUIRE_EQ(options.maximum_payload_size_limit(), 1'456U);
    REQUIRE_EQ(options.maximum_payload_size_limit(
                   IpAddressFamily::ipv6),
        1'436U);
    const auto ipv4_gcm = options.payload_budget(CryptoMode::aes_gcm);
    REQUIRE(ipv4_gcm);
    REQUIRE_EQ(ipv4_gcm.maximum_datagram_payload_size, 1'456U);
    REQUIRE_EQ(ipv4_gcm.maximum_wire_payload_size, 1'456U);
    REQUIRE_EQ(ipv4_gcm.maximum_plaintext_payload_size, 1'440U);
    const auto ipv6_gcm =
        options.payload_budget(IpAddressFamily::ipv6, CryptoMode::aes_gcm);
    REQUIRE(ipv6_gcm);
    REQUIRE_EQ(ipv6_gcm.maximum_datagram_payload_size, 1'436U);
    REQUIRE_EQ(ipv6_gcm.maximum_plaintext_payload_size, 1'420U);
    REQUIRE_EQ(options.maximum_payload_size_limit(CryptoMode::automatic), 0U);

    REQUIRE_EQ(options.set(
                   SocketOption::maximum_segment_size, 1'280),
        Error::none);
    REQUIRE_EQ(options.maximum_segment_size(), 1'280U);
    REQUIRE_EQ(options.maximum_payload_size_limit(), 1'236U);
    REQUIRE_EQ(options.maximum_payload_size_limit(
                   IpAddressFamily::ipv6),
        1'216U);
    REQUIRE_EQ(options.maximum_payload_size(), 1'236U);

    REQUIRE_EQ(options.set(
                   SocketOption::maximum_segment_size, 1'500),
        Error::none);
    REQUIRE_EQ(options.maximum_payload_size(), 1'456U);
    REQUIRE_EQ(options.set(
                   SocketOption::maximum_segment_size, 1'280),
        Error::none);
    options.constrain_to_address_family(
        IpAddressFamily::ipv6);
    REQUIRE_EQ(options.maximum_payload_size(), 1'216U);
    REQUIRE_EQ(
        options.live_rate_configuration().packet_header_size,
        ipv6_srt_packet_overhead);
    REQUIRE_EQ(options.set_packet_filter(
                   "fec,cols:10,rows:5"),
        Error::none);
    REQUIRE_EQ(options.maximum_payload_size(), 1'212U);
    const auto ipv6_fec_gcm =
        options.payload_budget(IpAddressFamily::ipv6, CryptoMode::aes_gcm);
    REQUIRE(ipv6_fec_gcm);
    REQUIRE_EQ(ipv6_fec_gcm.maximum_datagram_payload_size, 1'216U);
    REQUIRE_EQ(ipv6_fec_gcm.packet_filter_overhead_size, 4U);
    REQUIRE_EQ(ipv6_fec_gcm.maximum_wire_payload_size, 1'212U);
    REQUIRE_EQ(ipv6_fec_gcm.maximum_plaintext_payload_size, 1'196U);
    REQUIRE_EQ(options.set_packet_filter({}), Error::none);
    REQUIRE_EQ(options.maximum_payload_size(), 1'216U);
    REQUIRE_EQ(options.set(
                   SocketOption::maximum_segment_size, 75),
        Error::invalid_state);
    REQUIRE_EQ(options.set(
                   SocketOption::maximum_segment_size, 1'501),
        Error::invalid_state);
}

TEST(socket_options_generate_directional_hsv5_capabilities)
{
    SocketOptions options;
    REQUIRE_EQ(options.set(SocketOption::tsbpd_mode, 0), Error::none);
    REQUIRE_EQ(options.set(SocketOption::periodic_nak, 0), Error::none);
    REQUIRE_EQ(options.set(SocketOption::receiver_latency_milliseconds, 450),
        Error::none);
    const auto handshake = options.handshake_parameters();
    REQUIRE_EQ(handshake.flags
            & static_cast<std::uint32_t>(HandshakeExtensionFlag::tsbpd_send),
        0U);
    REQUIRE_EQ(handshake.flags
            & static_cast<std::uint32_t>(HandshakeExtensionFlag::periodic_nak),
        0U);
    REQUIRE_EQ(handshake.flags
            & static_cast<std::uint32_t>(HandshakeExtensionFlag::crypt),
        0U);
    REQUIRE((handshake.flags
        & static_cast<std::uint32_t>(
            HandshakeExtensionFlag::packet_filter)) != 0U);
    REQUIRE_EQ(handshake.receiver_tsbpd_delay_milliseconds, 450U);
}

TEST(socket_options_bound_refresh_rates_below_the_nonce_period)
{
    SocketOptions options;
    REQUIRE_EQ(options.set(SocketOption::key_refresh_rate_packets,
                   SequenceNumber::mask),
        Error::none);
    const auto before = options.crypto_configuration();
    REQUIRE_EQ(before.refresh_rate_packets, SequenceNumber::mask);
    for (const std::int64_t refresh :
        {0x8000'0000LL, 0xffff'ffffLL, 0x1'0000'0000LL}) {
        REQUIRE_EQ(options.set(SocketOption::key_refresh_rate_packets, refresh),
            Error::invalid_state);
        const auto after = options.crypto_configuration();
        REQUIRE_EQ(after.refresh_rate_packets, before.refresh_rate_packets);
        REQUIRE_EQ(
            after.preannouncement_packets, before.preannouncement_packets);
    }
    REQUIRE_EQ(
        options.set(SocketOption::key_refresh_rate_packets, 0), Error::none);
    REQUIRE_EQ(options.crypto_configuration().refresh_rate_packets,
        default_key_refresh_rate);
}

TEST(socket_options_validate_encryption_and_rotation_contract)
{
    SocketOptions options;
    REQUIRE_EQ(options.set_passphrase("short"),
        Error::invalid_state);
    REQUIRE_EQ(options.set_passphrase(
                   "correct horse battery"),
        Error::none);
    REQUIRE(options.encryption_enabled());
    REQUIRE_EQ(options.set(
                   SocketOption::encryption_key_length, 24),
        Error::none);
    REQUIRE_EQ(options.set(
                   SocketOption::encryption_key_length, 20),
        Error::invalid_state);
    REQUIRE_EQ(options.set(
                   SocketOption::key_refresh_rate_packets, 10'000),
        Error::none);
    REQUIRE_EQ(options.set(
                   SocketOption::key_preannouncement_packets, 1'000),
        Error::none);
    REQUIRE_EQ(options.set(
                   SocketOption::key_preannouncement_packets, 5'000),
        Error::invalid_state);
    const auto crypto = options.crypto_configuration();
    REQUIRE_EQ(crypto.key_length, 24U);
    REQUIRE_EQ(crypto.refresh_rate_packets, 10'000U);
    REQUIRE_EQ(crypto.preannouncement_packets, 1'000U);
    REQUIRE((options.handshake_parameters().flags
        & static_cast<std::uint32_t>(
            HandshakeExtensionFlag::crypt)) != 0U);
    REQUIRE_EQ(options.set_passphrase({}), Error::none);
    REQUIRE(!options.encryption_enabled());
}

#ifdef ENABLE_AEAD_API_PREVIEW
TEST(socket_options_enable_gcm_only_through_the_preview_contract)
{
    SocketOptions options;
    REQUIRE_EQ(options.get(SocketOption::crypto_mode).value, 0);
    REQUIRE_EQ(options.set(SocketOption::crypto_mode, 3), Error::invalid_state);
    REQUIRE_EQ(options.set(SocketOption::crypto_mode, 2), Error::none);
    REQUIRE_EQ(options.get(SocketOption::crypto_mode).value, 2);
    REQUIRE_EQ(options.maximum_payload_size_limit(), 1'440U);
    REQUIRE_EQ(options.maximum_payload_size(), 1'440U);
    REQUIRE_EQ(options.set(SocketOption::tsbpd_mode, 0), Error::invalid_state);
    REQUIRE_EQ(options.set(SocketOption::rendezvous, 1), Error::none);
    REQUIRE_EQ(options.set(SocketOption::rendezvous, 0), Error::none);
    REQUIRE_EQ(options.set(SocketOption::message_api, 0), Error::invalid_state);
    REQUIRE_EQ(options.set(SocketOption::transmission_type,
                   static_cast<std::int64_t>(TransmissionType::file)),
        Error::none);
    REQUIRE_EQ(options.transmission_type(), TransmissionType::file);
    REQUIRE_EQ(options.congestion_controller(), CongestionController::file);
    REQUIRE(!options.message_api());
    REQUIRE_EQ(options.get(SocketOption::tsbpd_mode).value, 0);
    REQUIRE_EQ(options.maximum_payload_size_limit(), 1'440U);
    REQUIRE_EQ(options.maximum_payload_size(), 1'440U);
    REQUIRE_EQ(options.set(SocketOption::message_api, 1), Error::invalid_state);
    REQUIRE_EQ(options.set(SocketOption::tsbpd_mode, 1), Error::invalid_state);
    REQUIRE_EQ(options.set_congestion_controller("live"), Error::invalid_state);
    REQUIRE_EQ(options.set(SocketOption::transmission_type,
                   static_cast<std::int64_t>(TransmissionType::live)),
        Error::none);
    REQUIRE(options.message_api());
    REQUIRE_EQ(options.get(SocketOption::tsbpd_mode).value, 1);
    REQUIRE_EQ(options.set_packet_filter("fec,cols:4,rows:1"), Error::none);
    REQUIRE_EQ(options.maximum_payload_size_limit(), 1'436U);

    const auto crypto = options.crypto_configuration();
    REQUIRE_EQ(crypto.mode, CryptoMode::aes_gcm);
    REQUIRE(crypto.enable_aes_gcm);

    REQUIRE_EQ(options.set(SocketOption::crypto_mode, 0), Error::none);
    REQUIRE_EQ(options.maximum_payload_size_limit(), 1'452U);
    REQUIRE_EQ(options.maximum_payload_size(), 1'316U);
    REQUIRE_EQ(options.set(SocketOption::tsbpd_mode, 0), Error::none);
    REQUIRE_EQ(options.set(SocketOption::crypto_mode, 2), Error::invalid_state);
    REQUIRE_EQ(options.set(SocketOption::crypto_mode, 1), Error::none);

    SocketOptions later_scope;
    REQUIRE_EQ(later_scope.set(SocketOption::rendezvous, 1), Error::none);
    REQUIRE_EQ(later_scope.set(SocketOption::crypto_mode, 2), Error::none);
    REQUIRE_EQ(later_scope.set(SocketOption::crypto_mode, 0), Error::none);
    REQUIRE_EQ(later_scope.set(SocketOption::rendezvous, 0), Error::none);
    REQUIRE_EQ(later_scope.set(SocketOption::message_api, 0), Error::none);
    REQUIRE_EQ(
        later_scope.set(SocketOption::crypto_mode, 2), Error::invalid_state);
    REQUIRE_EQ(later_scope.set(SocketOption::message_api, 1), Error::none);
    REQUIRE_EQ(later_scope.set(SocketOption::transmission_type,
                   static_cast<std::int64_t>(TransmissionType::file)),
        Error::none);
    REQUIRE_EQ(later_scope.set(SocketOption::crypto_mode, 2), Error::none);
    REQUIRE_EQ(later_scope.set(SocketOption::crypto_mode, 0), Error::none);
    REQUIRE_EQ(later_scope.set(SocketOption::transmission_type,
                   static_cast<std::int64_t>(TransmissionType::live)),
        Error::none);
    REQUIRE_EQ(later_scope.set_packet_filter("fec,cols:4,rows:1"), Error::none);
    REQUIRE_EQ(later_scope.set(SocketOption::crypto_mode, 2), Error::none);
    REQUIRE_EQ(later_scope.maximum_payload_size_limit(), 1'436U);
    REQUIRE_EQ(later_scope.set(SocketOption::transmission_type,
                   static_cast<std::int64_t>(TransmissionType::file)),
        Error::invalid_state);
}
#endif

TEST(socket_options_expose_rendezvous_as_a_validated_boolean)
{
    SocketOptions options;
    REQUIRE(!options.rendezvous());
    REQUIRE_EQ(options.get(
            SocketOption::rendezvous).value,
        0);
    REQUIRE_EQ(options.set(
            SocketOption::rendezvous, 1),
        Error::none);
    REQUIRE(options.rendezvous());
    REQUIRE_EQ(options.get(
            SocketOption::rendezvous).value,
        1);
    REQUIRE_EQ(options.set(
            SocketOption::rendezvous, 2),
        Error::invalid_state);
}

TEST(file_transmission_type_applies_stream_and_filecc_defaults)
{
    SocketOptions options;
    REQUIRE_EQ(options.set(
                   SocketOption::transmission_type,
                   static_cast<std::int64_t>(
                       TransmissionType::file)),
        Error::none);
    REQUIRE_EQ(options.transmission_type(),
        TransmissionType::file);
    REQUIRE_EQ(options.congestion_controller(),
        CongestionController::file);
    REQUIRE(!options.message_api());
    REQUIRE_EQ(options.get(SocketOption::tsbpd_mode).value, 0);
    REQUIRE_EQ(options.get(
            SocketOption::too_late_packet_drop).value,
        0);
    REQUIRE_EQ(options.get(SocketOption::periodic_nak).value, 0);
    REQUIRE_EQ(options.get(SocketOption::retransmit_flag).value, 0);
    REQUIRE_EQ(options.maximum_payload_size(),
        maximum_data_payload_size);
    REQUIRE((options.handshake_parameters().flags
        & static_cast<std::uint32_t>(
            HandshakeExtensionFlag::stream)) != 0U);

    REQUIRE_EQ(options.set(SocketOption::message_api, 1),
        Error::none);
    REQUIRE((options.handshake_parameters().flags
        & static_cast<std::uint32_t>(
            HandshakeExtensionFlag::stream)) == 0U);
}

TEST(congestion_controller_selection_is_independent_of_the_mode_bundle)
{
    SocketOptions options;
    REQUIRE_EQ(options.congestion_controller(),
        CongestionController::live);
    REQUIRE_EQ(options.set_congestion_controller("file"),
        Error::none);
    REQUIRE_EQ(options.congestion_controller(),
        CongestionController::file);
    REQUIRE_EQ(options.transmission_type(),
        TransmissionType::live);
    REQUIRE(options.message_api());
    REQUIRE_EQ(options.set_congestion_controller("cubic"),
        Error::invalid_state);
    REQUIRE_EQ(options.set(
            SocketOption::congestion_controller,
            static_cast<std::int64_t>(
                CongestionController::live)),
        Error::none);
    REQUIRE_EQ(options.get(
            SocketOption::congestion_controller).value,
        static_cast<std::int64_t>(
            CongestionController::live));
}

TEST(packet_filter_option_reserves_the_fec_header)
{
    SocketOptions options;
    REQUIRE_EQ(options.set(
            SocketOption::maximum_payload_size,
            maximum_data_payload_size),
        Error::none);
    REQUIRE_EQ(options.set_packet_filter(
            "fec,cols:10,rows:5"),
        Error::none);
    REQUIRE_EQ(options.maximum_payload_size_limit(),
        maximum_data_payload_size - fec_filter_header_size);
    REQUIRE_EQ(options.maximum_payload_size(),
        maximum_data_payload_size - fec_filter_header_size);
    REQUIRE_EQ(options.set(
            SocketOption::maximum_payload_size,
            maximum_data_payload_size),
        Error::invalid_state);
    REQUIRE_EQ(options.packet_filter(),
        std::string_view{"fec,cols:10,rows:5"});

    REQUIRE_EQ(options.set_packet_filter({}), Error::none);
    REQUIRE_EQ(options.maximum_payload_size_limit(),
        maximum_data_payload_size);
}
