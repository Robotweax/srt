#include "robotweax/srt/socket_options.hpp"

#include <algorithm>
#include <limits>

namespace robotweax::srt {
namespace {

[[nodiscard]] constexpr bool is_boolean(std::int64_t value) noexcept
{
    return value == 0 || value == 1;
}

} // namespace

#ifdef ENABLE_AEAD_API_PREVIEW
bool SocketOptions::supports_aes_gcm_transport_bundle(
    TransmissionType transmission_type,
    CongestionController congestion_controller, bool tsbpd_mode,
    bool message_api) const noexcept
{
    const bool live_message = transmission_type == TransmissionType::live
        && congestion_controller == CongestionController::live && tsbpd_mode
        && message_api;
    const bool file_stream = transmission_type == TransmissionType::file
        && congestion_controller == CongestionController::file && !tsbpd_mode
        && !message_api;
    return packet_filter_configuration_.enabled ? live_message
                                                : (live_message || file_stream);
}
#endif

SocketOptions::~SocketOptions()
{
    volatile char* output = passphrase_.data();
    for (std::size_t index = 0; index < passphrase_.size(); ++index) {
        output[index] = 0;
    }
}

Error SocketOptions::set(SocketOption option, std::int64_t value) noexcept
{
    switch (option) {
    case SocketOption::input_bandwidth_bytes_per_second:
        if (value < 0) return Error::invalid_state;
        input_bandwidth_ = static_cast<std::uint64_t>(value);
        return Error::none;
    case SocketOption::minimum_input_bandwidth_bytes_per_second:
        if (value < 0) return Error::invalid_state;
        minimum_input_bandwidth_ = static_cast<std::uint64_t>(value);
        return Error::none;
    case SocketOption::maximum_bandwidth_bytes_per_second:
        if (value < -1) return Error::invalid_state;
        maximum_bandwidth_ = value;
        return Error::none;
    case SocketOption::overhead_bandwidth_percent:
        if (value < 0 || value > 100) return Error::invalid_state;
        overhead_percent_ = static_cast<std::uint32_t>(value);
        return Error::none;
    case SocketOption::receiver_latency_milliseconds:
        if (value < 0 || value > std::numeric_limits<std::uint16_t>::max()) {
            return Error::invalid_state;
        }
        receiver_latency_milliseconds_ = static_cast<std::uint16_t>(value);
        return Error::none;
    case SocketOption::peer_latency_milliseconds:
        if (value < 0 || value > std::numeric_limits<std::uint16_t>::max()) {
            return Error::invalid_state;
        }
        peer_latency_milliseconds_ = static_cast<std::uint16_t>(value);
        return Error::none;
    case SocketOption::sender_drop_delay_milliseconds:
        if (value < -1 || value > std::numeric_limits<std::int32_t>::max()) {
            return Error::invalid_state;
        }
        sender_drop_delay_milliseconds_ = static_cast<std::int32_t>(value);
        return Error::none;
    case SocketOption::drift_tracer:
        if (!is_boolean(value)) return Error::invalid_state;
        drift_tracer_ = value != 0;
        return Error::none;
    case SocketOption::tsbpd_mode:
        if (!is_boolean(value)) return Error::invalid_state;
#ifdef ENABLE_AEAD_API_PREVIEW
        if (crypto_mode_ == CryptoMode::aes_gcm
            && !supports_aes_gcm_transport_bundle(transmission_type_,
                congestion_controller_, value != 0, message_api_)) {
            return Error::invalid_state;
        }
#endif
        tsbpd_mode_ = value != 0;
        return Error::none;
    case SocketOption::too_late_packet_drop:
        if (!is_boolean(value)) return Error::invalid_state;
        too_late_packet_drop_ = value != 0;
        return Error::none;
    case SocketOption::periodic_nak:
        if (!is_boolean(value)) return Error::invalid_state;
        periodic_nak_ = value != 0;
        return Error::none;
    case SocketOption::retransmit_flag:
        if (!is_boolean(value)) return Error::invalid_state;
        retransmit_flag_ = value != 0;
        return Error::none;
    case SocketOption::maximum_reorder_tolerance_packets:
        if (value < 0
            || value > std::numeric_limits<std::int32_t>::max()) {
            return Error::invalid_state;
        }
        maximum_reorder_tolerance_packets_ =
            static_cast<std::uint32_t>(value);
        if (reorder_tolerance_revision_
            != std::numeric_limits<std::uint64_t>::max()) {
            ++reorder_tolerance_revision_;
        }
        return Error::none;
    case SocketOption::send_buffer_packets:
        if (value <= 0 || value >= SequenceNumber::half_range) return Error::invalid_state;
        send_buffer_packets_ = static_cast<std::size_t>(value);
        return Error::none;
    case SocketOption::receive_buffer_packets:
        if (value <= 0 || value >= SequenceNumber::half_range) return Error::invalid_state;
        receive_buffer_packets_ = static_cast<std::size_t>(value);
        return Error::none;
    case SocketOption::flow_window_packets:
        if (value <= 0 || value >= SequenceNumber::half_range) return Error::invalid_state;
        flow_window_packets_ = static_cast<std::uint32_t>(value);
        return Error::none;
    case SocketOption::maximum_segment_size:
        if (value < static_cast<std::int64_t>(
                minimum_maximum_segment_size)
            || value > static_cast<std::int64_t>(
                default_maximum_segment_size)) {
            return Error::invalid_state;
        }
        maximum_segment_size_ = static_cast<std::size_t>(value);
        maximum_payload_size_ = std::min(
            requested_maximum_payload_size_,
            maximum_payload_size_limit());
        return Error::none;
    case SocketOption::maximum_payload_size:
        if (value <= 0
            || value > static_cast<std::int64_t>(
                maximum_payload_size_limit())) {
            return Error::invalid_state;
        }
        requested_maximum_payload_size_ =
            static_cast<std::size_t>(value);
        maximum_payload_size_ = requested_maximum_payload_size_;
        return Error::none;
    case SocketOption::encryption_key_length:
        if (value != 0 && value != 16 && value != 24 && value != 32) {
            return Error::invalid_state;
        }
        encryption_key_length_ = static_cast<std::size_t>(value);
        return Error::none;
    case SocketOption::key_refresh_rate_packets: {
        if (value < 0 || value > maximum_key_refresh_rate) {
            return Error::invalid_state;
        }
        const auto raw = static_cast<std::uint32_t>(value);
        const std::uint32_t effective_refresh =
            raw == 0U ? default_key_refresh_rate : raw;
        if (effective_refresh < 2U) {
            return Error::invalid_state;
        }
        const std::uint32_t effective_preannouncement =
            key_preannouncement_packets_ == 0U
            ? default_key_preannouncement
            : key_preannouncement_packets_;
        key_refresh_rate_packets_ = raw;
        if (effective_preannouncement
            > (effective_refresh - 1U) / 2U) {
            key_preannouncement_packets_ =
                (effective_refresh - 1U) / 2U;
        }
        return Error::none;
    }
    case SocketOption::key_preannouncement_packets: {
        if (value < 0
            || value > std::numeric_limits<std::uint32_t>::max()) {
            return Error::invalid_state;
        }
        const auto raw = static_cast<std::uint32_t>(value);
        const std::uint32_t effective =
            raw == 0U ? default_key_preannouncement : raw;
        const std::uint32_t refresh =
            key_refresh_rate_packets_ == 0U
            ? default_key_refresh_rate
            : key_refresh_rate_packets_;
        if (effective == 0U
            || effective > (refresh - 1U) / 2U) {
            return Error::invalid_state;
        }
        key_preannouncement_packets_ = raw;
        return Error::none;
    }
    case SocketOption::enforced_encryption:
        if (!is_boolean(value)) return Error::invalid_state;
        enforced_encryption_ = value != 0;
        return Error::none;
    case SocketOption::rendezvous:
        if (!is_boolean(value)) return Error::invalid_state;
        rendezvous_ = value != 0;
        return Error::none;
    case SocketOption::message_api:
        if (!is_boolean(value)) return Error::invalid_state;
#ifdef ENABLE_AEAD_API_PREVIEW
        if (crypto_mode_ == CryptoMode::aes_gcm
            && !supports_aes_gcm_transport_bundle(transmission_type_,
                congestion_controller_, tsbpd_mode_, value != 0)) {
            return Error::invalid_state;
        }
#endif
        message_api_ = value != 0;
        return Error::none;
    case SocketOption::transmission_type:
        if (value == static_cast<std::int64_t>(TransmissionType::live)) {
            transmission_type_ = TransmissionType::live;
            congestion_controller_ = CongestionController::live;
            tsbpd_mode_ = true;
            receiver_latency_milliseconds_ = 120;
            peer_latency_milliseconds_ = 0;
            too_late_packet_drop_ = true;
            sender_drop_delay_milliseconds_ = 0;
            message_api_ = true;
            periodic_nak_ = true;
            retransmit_flag_ = true;
            requested_maximum_payload_size_ = 1'316;
            maximum_payload_size_ = std::min(
                requested_maximum_payload_size_,
                maximum_payload_size_limit());
            return Error::none;
        }
        if (value == static_cast<std::int64_t>(TransmissionType::file)) {
#ifdef ENABLE_AEAD_API_PREVIEW
            if (crypto_mode_ == CryptoMode::aes_gcm
                && packet_filter_configuration_.enabled) {
                return Error::invalid_state;
            }
#endif
            transmission_type_ = TransmissionType::file;
            congestion_controller_ = CongestionController::file;
            tsbpd_mode_ = false;
            receiver_latency_milliseconds_ = 0;
            peer_latency_milliseconds_ = 0;
            too_late_packet_drop_ = false;
            sender_drop_delay_milliseconds_ = -1;
            message_api_ = false;
            periodic_nak_ = false;
            retransmit_flag_ = false;
            requested_maximum_payload_size_ =
                maximum_data_payload_size;
            maximum_payload_size_ = std::min(
                requested_maximum_payload_size_,
                maximum_payload_size_limit());
            return Error::none;
        }
        return Error::invalid_state;
    case SocketOption::congestion_controller:
        if (value == static_cast<std::int64_t>(
                CongestionController::live)
            || value == static_cast<std::int64_t>(
                CongestionController::file)) {
#ifdef ENABLE_AEAD_API_PREVIEW
            if (crypto_mode_ == CryptoMode::aes_gcm
                && !supports_aes_gcm_transport_bundle(transmission_type_,
                    static_cast<CongestionController>(value), tsbpd_mode_,
                    message_api_)) {
                return Error::invalid_state;
            }
#endif
            congestion_controller_ =
                static_cast<CongestionController>(value);
            return Error::none;
        }
        return Error::invalid_state;
#ifdef ENABLE_AEAD_API_PREVIEW
    case SocketOption::crypto_mode:
        if (value < static_cast<std::int64_t>(CryptoMode::automatic)
            || value > static_cast<std::int64_t>(CryptoMode::aes_gcm)) {
            return Error::invalid_state;
        }
        if (value == static_cast<std::int64_t>(CryptoMode::aes_gcm)
            && !supports_aes_gcm_transport_bundle(transmission_type_,
                congestion_controller_, tsbpd_mode_, message_api_)) {
            return Error::invalid_state;
        }
        crypto_mode_ = static_cast<CryptoMode>(value);
        maximum_payload_size_ = std::min(
            requested_maximum_payload_size_, maximum_payload_size_limit());
        return Error::none;
#endif
    }
    return Error::unsupported;
}

SocketOptionResult SocketOptions::get(SocketOption option) const noexcept
{
    switch (option) {
    case SocketOption::input_bandwidth_bytes_per_second: return {.value = static_cast<std::int64_t>(input_bandwidth_)};
    case SocketOption::minimum_input_bandwidth_bytes_per_second: return {.value = static_cast<std::int64_t>(minimum_input_bandwidth_)};
    case SocketOption::maximum_bandwidth_bytes_per_second: return {.value = maximum_bandwidth_};
    case SocketOption::overhead_bandwidth_percent: return {.value = overhead_percent_};
    case SocketOption::receiver_latency_milliseconds: return {.value = receiver_latency_milliseconds_};
    case SocketOption::peer_latency_milliseconds: return {.value = peer_latency_milliseconds_};
    case SocketOption::sender_drop_delay_milliseconds: return {.value = sender_drop_delay_milliseconds_};
    case SocketOption::drift_tracer: return {.value = drift_tracer_ ? 1 : 0};
    case SocketOption::tsbpd_mode: return {.value = tsbpd_mode_ ? 1 : 0};
    case SocketOption::too_late_packet_drop: return {.value = too_late_packet_drop_ ? 1 : 0};
    case SocketOption::periodic_nak: return {.value = periodic_nak_ ? 1 : 0};
    case SocketOption::retransmit_flag: return {.value = retransmit_flag_ ? 1 : 0};
    case SocketOption::maximum_reorder_tolerance_packets:
        return {.value = maximum_reorder_tolerance_packets_};
    case SocketOption::send_buffer_packets: return {.value = static_cast<std::int64_t>(send_buffer_packets_)};
    case SocketOption::receive_buffer_packets: return {.value = static_cast<std::int64_t>(receive_buffer_packets_)};
    case SocketOption::flow_window_packets: return {.value = flow_window_packets_};
    case SocketOption::maximum_segment_size:
        return {.value = static_cast<std::int64_t>(
            maximum_segment_size_)};
    case SocketOption::maximum_payload_size: return {.value = static_cast<std::int64_t>(maximum_payload_size_)};
    case SocketOption::encryption_key_length: return {.value = static_cast<std::int64_t>(encryption_key_length_)};
    case SocketOption::key_refresh_rate_packets: return {.value = key_refresh_rate_packets_};
    case SocketOption::key_preannouncement_packets: return {.value = key_preannouncement_packets_};
    case SocketOption::enforced_encryption: return {.value = enforced_encryption_ ? 1 : 0};
    case SocketOption::rendezvous: return {.value = rendezvous_ ? 1 : 0};
    case SocketOption::message_api: return {.value = message_api_ ? 1 : 0};
    case SocketOption::transmission_type:
        return {.value = static_cast<std::int64_t>(transmission_type_)};
    case SocketOption::congestion_controller:
        return {.value = static_cast<std::int64_t>(
            congestion_controller_)};
#ifdef ENABLE_AEAD_API_PREVIEW
    case SocketOption::crypto_mode:
        return {.value = static_cast<std::int64_t>(crypto_mode_)};
#endif
    }
    return {.error = Error::unsupported};
}

Error SocketOptions::set_passphrase(
    std::string_view passphrase) noexcept
{
    if (!passphrase.empty()
        && (passphrase.size() < minimum_passphrase_size
            || passphrase.size() > maximum_passphrase_size)) {
        return Error::invalid_state;
    }
    volatile char* old = passphrase_.data();
    for (std::size_t index = 0; index < passphrase_.size(); ++index) {
        old[index] = 0;
    }
    std::copy(passphrase.begin(), passphrase.end(),
        passphrase_.begin());
    passphrase_size_ = passphrase.size();
    return Error::none;
}

Error SocketOptions::set_packet_filter(
    std::string_view configuration) noexcept
{
    const auto parsed =
        parse_packet_filter_configuration(
            configuration);
    if (!parsed) {
        return parsed.error;
    }
#ifdef ENABLE_AEAD_API_PREVIEW
    if (parsed.configuration.enabled && crypto_mode_ == CryptoMode::aes_gcm
        && (transmission_type_ != TransmissionType::live
            || congestion_controller_ != CongestionController::live
            || !tsbpd_mode_ || !message_api_)) {
        return Error::invalid_state;
    }
#endif
    packet_filter_configuration_ =
        parsed.configuration;
    maximum_payload_size_ = std::min(
        requested_maximum_payload_size_,
        maximum_payload_size_limit());
    return Error::none;
}

Error SocketOptions::set_congestion_controller(
    std::string_view name) noexcept
{
    CongestionController parsed =
        CongestionController::live;
    if (!parse_congestion_controller(name, parsed)) {
        return Error::invalid_state;
    }
#ifdef ENABLE_AEAD_API_PREVIEW
    if (crypto_mode_ == CryptoMode::aes_gcm
        && !supports_aes_gcm_transport_bundle(
            transmission_type_, parsed, tsbpd_mode_, message_api_)) {
        return Error::invalid_state;
    }
#endif
    congestion_controller_ = parsed;
    return Error::none;
}

void SocketOptions::set_effective_packet_filter(
    const PacketFilterConfiguration&
        configuration) noexcept
{
    packet_filter_configuration_ = configuration;
    maximum_payload_size_ = std::min(
        requested_maximum_payload_size_,
        maximum_payload_size_limit());
}

void SocketOptions::constrain_to_address_family(
    IpAddressFamily family) noexcept
{
    packet_header_size_ =
        family == IpAddressFamily::ipv6
        ? ipv6_srt_packet_overhead
        : ipv4_srt_packet_overhead;
    maximum_payload_size_ = std::min(
        requested_maximum_payload_size_,
        maximum_payload_size_limit(family));
}

HandshakeExtensionParameters SocketOptions::handshake_parameters() const noexcept
{
    HandshakeExtensionParameters result;
    result.flags = 0;
    if (tsbpd_mode_) {
        result.flags |= static_cast<std::uint32_t>(HandshakeExtensionFlag::tsbpd_send)
            | static_cast<std::uint32_t>(HandshakeExtensionFlag::tsbpd_receive);
    }
    if (too_late_packet_drop_) {
        result.flags |= static_cast<std::uint32_t>(HandshakeExtensionFlag::too_late_packet_drop);
    }
    if (periodic_nak_) {
        result.flags |= static_cast<std::uint32_t>(HandshakeExtensionFlag::periodic_nak);
    }
    if (retransmit_flag_) {
        result.flags |= static_cast<std::uint32_t>(HandshakeExtensionFlag::retransmit_flag);
    }
    if (!message_api_) {
        result.flags |= static_cast<std::uint32_t>(
            HandshakeExtensionFlag::stream);
    }
    if (encryption_enabled()) {
        result.flags |= static_cast<std::uint32_t>(
            HandshakeExtensionFlag::crypt);
    }
    result.flags |= static_cast<std::uint32_t>(
        HandshakeExtensionFlag::packet_filter);
    result.receiver_tsbpd_delay_milliseconds = receiver_latency_milliseconds_;
    result.sender_tsbpd_delay_milliseconds = peer_latency_milliseconds_;
    return result;
}

CryptoConfiguration SocketOptions::crypto_configuration() const noexcept
{
    return {
        .passphrase = passphrase(),
#ifdef ENABLE_AEAD_API_PREVIEW
        .mode = crypto_mode_,
        .enable_aes_gcm = true,
#endif
        .key_length = encryption_key_length_ == 0U ? minimum_aes_key_size
                                                   : encryption_key_length_,
        .refresh_rate_packets = key_refresh_rate_packets_ == 0U
            ? default_key_refresh_rate
            : key_refresh_rate_packets_,
        .preannouncement_packets = key_preannouncement_packets_ == 0U
            ? default_key_preannouncement
            : key_preannouncement_packets_,
    };
}

LiveRateController::Configuration SocketOptions::live_rate_configuration() const noexcept
{
    return {
        .input_bandwidth_bytes_per_second = input_bandwidth_,
        .minimum_input_bandwidth_bytes_per_second =
            minimum_input_bandwidth_,
        .maximum_bandwidth_bytes_per_second = maximum_bandwidth_,
        .overhead_percent = overhead_percent_,
        .maximum_payload_size = maximum_payload_size_,
        .packet_header_size = packet_header_size_,
    };
}

} // namespace robotweax::srt
