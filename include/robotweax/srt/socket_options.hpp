#pragma once

#include "robotweax/srt/error.hpp"
#include "robotweax/srt/crypto.hpp"
#include "robotweax/srt/handshake_extensions.hpp"
#include "robotweax/srt/live.hpp"
#include "robotweax/srt/packet_filter.hpp"
#include "robotweax/srt/send_buffer.hpp"
#include "robotweax/srt/udp.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>

namespace robotweax::srt {

inline constexpr std::size_t default_srt_buffer_capacity_packets = 8'192;
inline constexpr std::size_t default_maximum_segment_size = 1'500;
inline constexpr std::size_t minimum_maximum_segment_size = 76;
inline constexpr std::size_t ipv4_srt_packet_overhead = 44;
inline constexpr std::size_t ipv6_srt_packet_overhead = 64;

enum class TransmissionType : std::uint8_t {
    live = 0,
    file = 1,
};

// Public options for every transport feature currently implemented by Robotweax.
// Integer values make this API straightforward to expose through a stable C ABI.
enum class SocketOption : std::uint16_t {
    input_bandwidth_bytes_per_second,
    minimum_input_bandwidth_bytes_per_second,
    maximum_bandwidth_bytes_per_second,
    overhead_bandwidth_percent,
    receiver_latency_milliseconds,
    peer_latency_milliseconds,
    sender_drop_delay_milliseconds,
    drift_tracer,
    tsbpd_mode,
    too_late_packet_drop,
    periodic_nak,
    retransmit_flag,
    maximum_reorder_tolerance_packets,
    send_buffer_packets,
    receive_buffer_packets,
    flow_window_packets,
    maximum_segment_size,
    maximum_payload_size,
    encryption_key_length,
    key_refresh_rate_packets,
    key_preannouncement_packets,
    enforced_encryption,
    rendezvous,
    message_api,
    transmission_type,
    congestion_controller,
#ifdef ENABLE_AEAD_API_PREVIEW
    crypto_mode,
#endif
};

struct SocketOptionResult {
    Error error = Error::none;
    std::int64_t value = 0;

    [[nodiscard]] constexpr explicit operator bool() const noexcept
    {
        return error == Error::none;
    }
};

class SocketOptions {
public:
    SocketOptions() = default;
    ~SocketOptions();
    SocketOptions(const SocketOptions&) = default;
    SocketOptions& operator=(const SocketOptions&) = default;

    [[nodiscard]] Error set(SocketOption option, std::int64_t value) noexcept;
    [[nodiscard]] SocketOptionResult get(SocketOption option) const noexcept;
    [[nodiscard]] Error set_passphrase(
        std::string_view passphrase) noexcept;
    [[nodiscard]] Error set_packet_filter(
        std::string_view configuration) noexcept;
    [[nodiscard]] Error set_congestion_controller(
        std::string_view name) noexcept;
    void set_effective_packet_filter(
        const PacketFilterConfiguration&
            configuration) noexcept;
    [[nodiscard]] std::string_view passphrase() const noexcept
    {
        return {passphrase_.data(), passphrase_size_};
    }
    [[nodiscard]] std::string_view packet_filter() const noexcept
    {
        return packet_filter_configuration_.view();
    }
    [[nodiscard]] const PacketFilterConfiguration&
    packet_filter_configuration() const noexcept
    {
        return packet_filter_configuration_;
    }

    [[nodiscard]] HandshakeExtensionParameters handshake_parameters() const noexcept;
    [[nodiscard]] LiveRateController::Configuration live_rate_configuration() const noexcept;
    [[nodiscard]] CryptoConfiguration crypto_configuration() const noexcept;
    [[nodiscard]] bool encryption_enabled() const noexcept
    {
        return passphrase_size_ != 0U;
    }
    [[nodiscard]] bool enforced_encryption() const noexcept
    {
        return enforced_encryption_;
    }
    [[nodiscard]] bool rendezvous() const noexcept
    {
        return rendezvous_;
    }
    [[nodiscard]] bool message_api() const noexcept
    {
        return message_api_;
    }
    [[nodiscard]] TransmissionType transmission_type() const noexcept
    {
        return transmission_type_;
    }
    [[nodiscard]] CongestionController
    congestion_controller() const noexcept
    {
        return congestion_controller_;
    }
    [[nodiscard]] std::size_t configured_encryption_key_length() const noexcept
    {
        return encryption_key_length_;
    }
    [[nodiscard]] std::uint64_t reorder_tolerance_revision() const noexcept
    {
        return reorder_tolerance_revision_;
    }

    [[nodiscard]] std::size_t send_buffer_packets() const noexcept
    {
        return send_buffer_packets_;
    }
    [[nodiscard]] std::size_t receive_buffer_packets() const noexcept
    {
        return receive_buffer_packets_;
    }
    [[nodiscard]] std::size_t maximum_payload_size() const noexcept
    {
        return maximum_payload_size_;
    }
    [[nodiscard]] std::size_t maximum_segment_size() const noexcept
    {
        return maximum_segment_size_;
    }
    [[nodiscard]] std::size_t
    maximum_payload_size_limit() const noexcept
    {
        return maximum_payload_size_limit(
#ifdef ENABLE_AEAD_API_PREVIEW
            crypto_mode_ == CryptoMode::aes_gcm ? CryptoMode::aes_gcm
                                                : CryptoMode::aes_ctr
#else
            CryptoMode::aes_ctr
#endif
        );
    }
    [[nodiscard]] CryptoPayloadBudget payload_budget(
        CryptoMode effective_mode) const noexcept
    {
        return make_crypto_payload_budget(effective_mode, maximum_segment_size_,
            packet_header_size_,
            packet_filter_configuration_.extra_header_size());
    }
    [[nodiscard]] std::size_t maximum_payload_size_limit(
        CryptoMode effective_mode) const noexcept
    {
        const auto budget = payload_budget(effective_mode);
        return budget ? budget.maximum_plaintext_payload_size : 0U;
    }
    [[nodiscard]] std::size_t
    maximum_payload_size_limit(
        IpAddressFamily family) const noexcept
    {
        return maximum_payload_size_limit(family,
#ifdef ENABLE_AEAD_API_PREVIEW
            crypto_mode_ == CryptoMode::aes_gcm ? CryptoMode::aes_gcm
                                                : CryptoMode::aes_ctr
#else
            CryptoMode::aes_ctr
#endif
        );
    }
    [[nodiscard]] CryptoPayloadBudget payload_budget(
        IpAddressFamily family, CryptoMode effective_mode) const noexcept
    {
        const std::size_t overhead =
            family == IpAddressFamily::ipv6
            ? ipv6_srt_packet_overhead
            : ipv4_srt_packet_overhead;
        return make_crypto_payload_budget(effective_mode, maximum_segment_size_,
            overhead, packet_filter_configuration_.extra_header_size());
    }
    [[nodiscard]] std::size_t maximum_payload_size_limit(
        IpAddressFamily family, CryptoMode effective_mode) const noexcept
    {
        const auto budget = payload_budget(family, effective_mode);
        return budget ? budget.maximum_plaintext_payload_size : 0U;
    }
    void constrain_to_address_family(
        IpAddressFamily family) noexcept;

private:
#ifdef ENABLE_AEAD_API_PREVIEW
    [[nodiscard]] bool supports_aes_gcm_transport_bundle(
        TransmissionType transmission_type,
        CongestionController congestion_controller, bool tsbpd_mode,
        bool message_api) const noexcept;
#endif

    std::uint64_t input_bandwidth_ = 0;
    std::uint64_t minimum_input_bandwidth_ = 0;
    std::int64_t maximum_bandwidth_ = -1;
    std::uint32_t overhead_percent_ = 25;
    std::uint16_t receiver_latency_milliseconds_ = 120;
    std::uint16_t peer_latency_milliseconds_ = 0;
    std::int32_t sender_drop_delay_milliseconds_ = 0;
    std::uint32_t maximum_reorder_tolerance_packets_ = 0;
    std::size_t send_buffer_packets_ =
        default_srt_buffer_capacity_packets;
    std::size_t receive_buffer_packets_ =
        default_srt_buffer_capacity_packets;
    std::uint32_t flow_window_packets_ = 25'600;
    std::size_t maximum_segment_size_ =
        default_maximum_segment_size;
    std::size_t requested_maximum_payload_size_ =
        maximum_data_payload_size;
    std::size_t maximum_payload_size_ = maximum_data_payload_size;
    std::size_t packet_header_size_ = ipv4_srt_packet_overhead;
    bool tsbpd_mode_ = true;
    bool drift_tracer_ = true;
    bool too_late_packet_drop_ = true;
    bool periodic_nak_ = true;
    bool retransmit_flag_ = true;
    std::uint64_t reorder_tolerance_revision_ = 0;
    std::array<char, maximum_passphrase_size> passphrase_{};
    std::size_t passphrase_size_ = 0;
    std::size_t encryption_key_length_ = 0;
    std::uint32_t key_refresh_rate_packets_ = 0;
    std::uint32_t key_preannouncement_packets_ = 0;
    PacketFilterConfiguration
        packet_filter_configuration_{};
    bool enforced_encryption_ = true;
    bool rendezvous_ = false;
    bool message_api_ = true;
    TransmissionType transmission_type_ = TransmissionType::live;
    CongestionController congestion_controller_ =
        CongestionController::live;
#ifdef ENABLE_AEAD_API_PREVIEW
    CryptoMode crypto_mode_ = CryptoMode::automatic;
#endif
};

} // namespace robotweax::srt
