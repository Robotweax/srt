#include "compat/socket_registry.hpp"

#include "compat/error_state.hpp"
#include "compat/readiness.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string_view>
#include <type_traits>

namespace robotweax::srt::compat {
namespace {

constexpr std::int32_t ipv4_udp_header_bytes = 28;
constexpr std::int32_t minimum_srt_buffer_packets = 32;

template <typename Value>
[[nodiscard]] bool read_value(
    const void* source, int source_size, Value& destination) noexcept
{
    static_assert(std::is_trivially_copyable_v<Value>);
    if (source == nullptr || source_size != static_cast<int>(sizeof(Value))) {
        return false;
    }
    std::memcpy(&destination, source, sizeof(Value));
    return true;
}

[[nodiscard]] bool read_boolean(
    const void* source, int source_size, bool& destination) noexcept
{
    static_assert(sizeof(bool) == 1, "The compatible SRT ABI requires 1-byte bool");
    if (source == nullptr) {
        return false;
    }
    if (source_size == static_cast<int>(sizeof(bool))) {
        unsigned char raw = 0;
        std::memcpy(&raw, source, sizeof(raw));
        if (raw > 1U) {
            return false;
        }
        destination = raw != 0U;
        return true;
    }
    std::int32_t compatible_integer = 0;
    if (!read_value(source, source_size, compatible_integer)
        || (compatible_integer != 0 && compatible_integer != 1)) {
        return false;
    }
    destination = compatible_integer != 0;
    return true;
}

template <typename Value>
[[nodiscard]] int write_value(
    void* destination, int* destination_size, Value source) noexcept
{
    static_assert(std::is_trivially_copyable_v<Value>);
    if (destination == nullptr || destination_size == nullptr
        || *destination_size < static_cast<int>(sizeof(Value))) {
        set_last_error(SRT_EINVPARAM);
        return SRT_ERROR;
    }
    std::memcpy(destination, &source, sizeof(Value));
    *destination_size = static_cast<int>(sizeof(Value));
    return 0;
}

[[nodiscard]] int write_string(
    void* destination, int* destination_size,
    std::string_view source) noexcept
{
    if (destination == nullptr || destination_size == nullptr
        || *destination_size
            < static_cast<int>(source.size() + 1U)) {
        set_last_error(SRT_EINVPARAM);
        return SRT_ERROR;
    }
    std::memcpy(destination, source.data(), source.size());
    static_cast<char*>(destination)[source.size()] = '\0';
    *destination_size =
        static_cast<int>(source.size());
    return 0;
}

[[nodiscard]] int invalid_parameter() noexcept
{
    set_last_error(SRT_EINVPARAM);
    return SRT_ERROR;
}

[[nodiscard]] int unsupported_option() noexcept
{
    set_last_error(SRT_EINVOP);
    return SRT_ERROR;
}

[[nodiscard]] constexpr bool is_pre_bind(SRT_SOCKOPT option) noexcept
{
    return option == SRTO_SNDBUF || option == SRTO_RCVBUF
        || option == SRTO_UDP_SNDBUF || option == SRTO_UDP_RCVBUF
        || option == SRTO_MSS
        || option == SRTO_REUSEADDR
        || option == SRTO_IPV6ONLY
        || option == SRTO_IPTTL
        || option == SRTO_IPTOS
        || option == SRTO_BINDTODEVICE
        || option == SRTO_TRANSTYPE;
}

[[nodiscard]] constexpr bool is_pre_connection(
    SRT_SOCKOPT option) noexcept
{
    return option == SRTO_FC || option == SRTO_RENDEZVOUS
        || option == SRTO_CONNTIMEO || option == SRTO_PEERIDLETIMEO
        || option == SRTO_PASSPHRASE || option == SRTO_PBKEYLEN
#ifdef ENABLE_AEAD_API_PREVIEW
        || option == SRTO_CRYPTOMODE
#endif
        || option == SRTO_KMREFRESHRATE || option == SRTO_KMPREANNOUNCE
        || option == SRTO_ENFORCEDENCRYPTION || option == SRTO_CONGESTION
        || option == SRTO_MESSAGEAPI || option == SRTO_SENDER
        || option == SRTO_MINVERSION || option == SRTO_STREAMID
        || option == SRTO_GROUPCONNECT || option == SRTO_PACKETFILTER;
}

[[nodiscard]] constexpr bool is_connection_state(
    SRT_SOCKSTATUS state) noexcept
{
    return state == SRTS_LISTENING
        || state == SRTS_CONNECTING
        || state == SRTS_CONNECTED;
}

[[nodiscard]] int set_native(
    SocketOptions& options, SocketOption option, std::int64_t value) noexcept
{
    if (options.set(option, value) != Error::none) {
        return invalid_parameter();
    }
    return 0;
}

[[nodiscard]] constexpr std::int32_t srt_buffer_unit_bytes(
    std::int32_t maximum_segment_size) noexcept
{
    return maximum_segment_size - ipv4_udp_header_bytes;
}

[[nodiscard]] constexpr std::int32_t srt_buffer_packets_from_bytes(
    std::int32_t bytes, std::int32_t maximum_segment_size) noexcept
{
    return bytes / srt_buffer_unit_bytes(maximum_segment_size);
}

[[nodiscard]] constexpr std::int32_t srt_buffer_bytes_from_packets(
    std::int32_t packets, std::int32_t maximum_segment_size) noexcept
{
    return packets * srt_buffer_unit_bytes(maximum_segment_size);
}

} // namespace

int get_socket_option(
    SRTSOCKET handle, SocketRecord& socket, SRT_SOCKOPT option,
    void* value, int* value_size) noexcept
{
    if (value == nullptr || value_size == nullptr || *value_size < 0) {
        return invalid_parameter();
    }

    if (option == SRTO_EVENT || option == SRTO_SNDDATA
        || option == SRTO_RCVDATA) {
        std::shared_ptr<ConnectionRuntime> runtime;
        {
            std::lock_guard lock(socket.mutex);
            if (socket.state == SRTS_CLOSED) {
                set_last_error(SRT_EINVSOCK);
                return SRT_ERROR;
            }
            runtime = socket.runtime;
        }
        if (option == SRTO_EVENT) {
            const auto readiness = socket_readiness(handle);
            if (!readiness.exists) {
                set_last_error(SRT_EINVSOCK);
                return SRT_ERROR;
            }
            return write_value(value, value_size,
                static_cast<std::int32_t>(readiness.events));
        }
        const RuntimeBufferPacketCounts counts = runtime != nullptr
            ? runtime->buffer_packet_counts()
            : RuntimeBufferPacketCounts{};
        const std::size_t packets = option == SRTO_SNDDATA
            ? counts.unacknowledged_send
            : counts.available_receive;
        return write_value(value, value_size,
            static_cast<std::int32_t>(std::min<std::size_t>(
                packets,
                static_cast<std::size_t>(
                    std::numeric_limits<std::int32_t>::max()))));
    }

    std::lock_guard lock(socket.mutex);
    if (socket.state == SRTS_CLOSED) {
        set_last_error(SRT_EINVSOCK);
        return SRT_ERROR;
    }
    const PublicSocketOptions& options = socket.public_options;

    switch (option) {
    case SRTO_SNDSYN:
        return write_value(value, value_size, options.send_synchronous);
    case SRTO_RCVSYN:
        return write_value(value, value_size, options.receive_synchronous);
    case SRTO_SNDTIMEO:
        return write_value(
            value, value_size, options.send_timeout_milliseconds);
    case SRTO_RCVTIMEO:
        return write_value(
            value, value_size, options.receive_timeout_milliseconds);
    case SRTO_ISN:
        return write_value(
            value, value_size,
            static_cast<std::int32_t>(
                socket.connection_initial_sequence));
    case SRTO_FC:
        return write_value(value, value_size, options.flow_window_packets);
    case SRTO_MSS:
        return write_value(
            value, value_size, options.maximum_segment_size);
    case SRTO_SNDBUF:
        return write_value(value, value_size, options.send_buffer_bytes);
    case SRTO_RCVBUF:
        return write_value(value, value_size, options.receive_buffer_bytes);
    case SRTO_UDP_SNDBUF:
        return write_value(value, value_size, options.udp_send_buffer_bytes);
    case SRTO_UDP_RCVBUF:
        return write_value(value, value_size, options.udp_receive_buffer_bytes);
    case SRTO_REUSEADDR:
        return write_value(value, value_size, options.reuse_address);
    case SRTO_IPV6ONLY:
        return write_value(value, value_size,
            socket.effective_ipv6_only != -1
            ? socket.effective_ipv6_only
            : options.ipv6_only);
    case SRTO_IPTTL: {
        if (socket.channel == nullptr || socket.state == SRTS_INIT) {
            return write_value(
                value, value_size, options.ip_time_to_live);
        }
        const UdpIntegerOptionResult actual =
            socket.channel->socket.ip_time_to_live();
        if (!actual) {
            set_last_error(SRT_ESOCKFAIL, actual.system_error);
            return SRT_ERROR;
        }
        return write_value(value, value_size, actual.value);
    }
    case SRTO_IPTOS: {
        if (socket.channel == nullptr || socket.state == SRTS_INIT) {
            return write_value(
                value, value_size, options.ip_type_of_service);
        }
        const UdpIntegerOptionResult actual =
            socket.channel->socket.ip_type_of_service();
        if (actual.error == Error::unsupported
            && !options.ip_type_of_service_explicit) {
            return write_value(
                value, value_size, options.ip_type_of_service);
        }
        if (!actual) {
            set_last_error(actual.error == Error::unsupported
                    ? SRT_EINVOP : SRT_ESOCKFAIL,
                actual.system_error);
            return SRT_ERROR;
        }
        return write_value(value, value_size, actual.value);
    }
    case SRTO_BINDTODEVICE: {
        if (!UdpSocket::bind_to_device_supported()) {
            return unsupported_option();
        }
        if (*value_size
            < static_cast<int>(maximum_network_device_name_size)) {
            return invalid_parameter();
        }
        UdpDeviceOptionResult actual_device;
        std::string_view device{
            options.bound_device.data(), options.bound_device_size};
        if (socket.channel != nullptr && socket.state != SRTS_INIT) {
            actual_device =
                socket.channel->socket.bound_device();
            if (!actual_device) {
                set_last_error(
                    SRT_ESOCKFAIL, actual_device.system_error);
                return SRT_ERROR;
            }
            device = actual_device.view();
        }
        std::memcpy(value, device.data(), device.size());
        static_cast<char*>(value)[device.size()] = '\0';
        *value_size = static_cast<int>(device.size());
        return 0;
    }
    case SRTO_LINGER: {
        linger configured{};
        configured.l_onoff = options.linger_enabled ? 1 : 0;
        configured.l_linger = static_cast<decltype(linger::l_linger)>(
            options.linger_seconds);
        return write_value(value, value_size, configured);
    }
    case SRTO_MAXBW:
        return write_value(
            value, value_size, options.maximum_bandwidth_bytes_per_second);
    case SRTO_STATE:
        return write_value(
            value, value_size, static_cast<std::int32_t>(socket.state));
    case SRTO_TSBPDMODE:
        return write_value(value, value_size, options.tsbpd_mode);
#ifdef ENABLE_AEAD_API_PREVIEW
    case SRTO_CRYPTOMODE: {
        const CryptoMode mode = socket.runtime != nullptr
            ? socket.runtime->crypto_mode()
            : static_cast<CryptoMode>(
                  socket.native_options.get(SocketOption::crypto_mode).value);
        return write_value(value, value_size, static_cast<std::int32_t>(mode));
    }
#endif
    case SRTO_LATENCY:
    case SRTO_RCVLATENCY:
        return write_value(
            value, value_size, options.receiver_latency_milliseconds);
    case SRTO_INPUTBW:
        return write_value(
            value, value_size, options.input_bandwidth_bytes_per_second);
    case SRTO_MININPUTBW:
        return write_value(value, value_size,
            options.minimum_input_bandwidth_bytes_per_second);
    case SRTO_OHEADBW:
        return write_value(
            value, value_size, options.overhead_bandwidth_percent);
    case SRTO_TLPKTDROP:
        return write_value(value, value_size, options.too_late_packet_drop);
    case SRTO_DRIFTTRACER:
        return write_value(value, value_size, options.drift_tracer);
    case SRTO_SNDDROPDELAY:
        return write_value(
            value, value_size, options.sender_drop_delay_milliseconds);
    case SRTO_NAKREPORT:
        return write_value(value, value_size, options.periodic_nak);
    case SRTO_LOSSMAXTTL:
        return write_value(value, value_size,
            options.maximum_reorder_tolerance_packets);
    case SRTO_VERSION:
        return write_value(
            value, value_size, static_cast<std::int32_t>(SRT_VERSION_VALUE));
    case SRTO_ROBOTWEAX_VERSION:
        return write_value(value, value_size,
            static_cast<std::int32_t>(ROBOTWEAX_SRT_VERSION_VALUE));
    case SRTO_ROBOTWEAX_CRYPTO_BACKEND:
        return write_value(value, value_size,
            static_cast<std::int32_t>(ROBOTWEAX_SRT_COMPILED_CRYPTO_BACKEND));
    case SRTO_PEERVERSION:
        return write_value(value, value_size,
            static_cast<std::int32_t>(socket.peer_srt_version));
    case SRTO_GROUPTYPE:
        return write_value(value, value_size,
            static_cast<std::int32_t>(
                socket.listen_callback_active
                ? socket.incoming_group_type
                : SRT_GTYPE_UNDEFINED));
    case SRTO_CONNTIMEO:
        return write_value(
            value, value_size, options.connection_timeout_milliseconds);
    case SRTO_MINVERSION:
        return write_value(
            value, value_size, options.minimum_peer_srt_version);
    case SRTO_PEERIDLETIMEO:
        return write_value(
            value, value_size, options.peer_idle_timeout_milliseconds);
    case SRTO_PEERLATENCY:
        return write_value(
            value, value_size, options.peer_latency_milliseconds);
    case SRTO_PAYLOADSIZE:
        return write_value(value, value_size, options.maximum_payload_size);
    case SRTO_MESSAGEAPI:
        return write_value(value, value_size, options.message_api);
    case SRTO_SENDER:
        return write_value(value, value_size, options.data_sender);
    case SRTO_TRANSTYPE:
        return write_value(
            value, value_size,
            static_cast<std::int32_t>(options.transmission_type));
    case SRTO_RETRANSMITALGO:
        return write_value(
            value, value_size, options.retransmission_algorithm);
    case SRTO_RENDEZVOUS:
        return write_value(value, value_size, options.rendezvous);
    case SRTO_GROUPCONNECT:
        return write_value(value, value_size, options.group_connect);
    case SRTO_STREAMID:
        return write_string(
            value, value_size, options.stream_id.view());
    case SRTO_PACKETFILTER:
        return write_string(
            value, value_size,
            socket.native_options.packet_filter());
    case SRTO_PBKEYLEN: {
        const std::int32_t key_length =
            socket.runtime != nullptr
            ? static_cast<std::int32_t>(
                  socket.runtime->crypto_key_length())
            : static_cast<std::int32_t>(
                  socket.native_options
                      .configured_encryption_key_length());
        return write_value(value, value_size, key_length);
    }
    case SRTO_KMSTATE:
    case SRTO_SNDKMSTATE: {
        const auto state = socket.runtime != nullptr
            ? socket.runtime->sender_crypto_state()
            : CryptoState::unsecured;
        return write_value(value, value_size,
            static_cast<std::int32_t>(state));
    }
    case SRTO_RCVKMSTATE: {
        const auto state = socket.runtime != nullptr
            ? socket.runtime->receiver_crypto_state()
            : CryptoState::unsecured;
        return write_value(value, value_size,
            static_cast<std::int32_t>(state));
    }
    case SRTO_KMREFRESHRATE:
        return write_value(value, value_size,
            static_cast<std::int32_t>(
                socket.native_options.get(
                    SocketOption::key_refresh_rate_packets).value));
    case SRTO_KMPREANNOUNCE:
        return write_value(value, value_size,
            static_cast<std::int32_t>(
                socket.native_options.get(
                    SocketOption::key_preannouncement_packets).value));
    case SRTO_ENFORCEDENCRYPTION:
        return write_value(value, value_size,
            socket.native_options.enforced_encryption());
    default:
        return unsupported_option();
    }
}

int set_socket_option(
    SocketRecord& socket, SRT_SOCKOPT option,
    const void* value, int value_size) noexcept
{
    if ((value == nullptr && value_size != 0)
        || value_size < 0) {
        return invalid_parameter();
    }
    std::lock_guard lock(socket.mutex);
    if (socket.state == SRTS_CLOSED) {
        set_last_error(SRT_EINVSOCK);
        return SRT_ERROR;
    }
    if (option == SRTO_BINDTODEVICE
        && !UdpSocket::bind_to_device_supported()) {
        return unsupported_option();
    }
    // A lingering record is still discoverable, but closure has already
    // erased its configured secret. Do not allow a racing setter to restore
    // that secret after the erasure boundary.
    if (option == SRTO_PASSPHRASE && socket.state == SRTS_CLOSING) {
        set_last_error(SRT_ESCLOSED);
        return SRT_ERROR;
    }
    if (is_pre_bind(option) && socket.state != SRTS_INIT) {
        set_last_error(is_connection_state(socket.state)
                ? SRT_ECONNSOCK
                : SRT_EBOUNDSOCK);
        return SRT_ERROR;
    }
    if (is_pre_connection(option)
        && is_connection_state(socket.state)
        && !socket.listen_callback_active) {
        set_last_error(SRT_ECONNSOCK);
        return SRT_ERROR;
    }
    PublicSocketOptions& options = socket.public_options;

    switch (option) {
    case SRTO_SNDSYN: {
        bool parsed = false;
        if (!read_boolean(value, value_size, parsed)) return invalid_parameter();
        options.send_synchronous = parsed;
        return 0;
    }
    case SRTO_RCVSYN: {
        bool parsed = false;
        if (!read_boolean(value, value_size, parsed)) return invalid_parameter();
        options.receive_synchronous = parsed;
        return 0;
    }
    case SRTO_SNDTIMEO:
    case SRTO_RCVTIMEO: {
        std::int32_t parsed = 0;
        if (!read_value(value, value_size, parsed) || parsed < -1) {
            return invalid_parameter();
        }
        if (option == SRTO_SNDTIMEO) {
            options.send_timeout_milliseconds = parsed;
        } else {
            options.receive_timeout_milliseconds = parsed;
        }
        return 0;
    }
    case SRTO_FC: {
        std::int32_t parsed = 0;
        if (!read_value(value, value_size, parsed) || parsed < 32) {
            return invalid_parameter();
        }
        if (set_native(
                socket.native_options, SocketOption::flow_window_packets, parsed)
            == SRT_ERROR) {
            return SRT_ERROR;
        }
        options.flow_window_packets = parsed;
        return 0;
    }
    case SRTO_MSS: {
        std::int32_t parsed = 0;
        if (!read_value(value, value_size, parsed)
            || parsed < static_cast<std::int32_t>(
                minimum_maximum_segment_size)
            || parsed > static_cast<std::int32_t>(
                default_maximum_segment_size)
            || parsed > options.udp_send_buffer_bytes
            || parsed > options.udp_receive_buffer_bytes) {
            return invalid_parameter();
        }
        if (set_native(
                socket.native_options,
                SocketOption::maximum_segment_size,
                parsed) == SRT_ERROR) {
            return SRT_ERROR;
        }
        options.maximum_segment_size = parsed;
        options.send_buffer_bytes =
            srt_buffer_bytes_from_packets(
                static_cast<std::int32_t>(
                    socket.native_options.send_buffer_packets()),
                parsed);
        options.receive_buffer_bytes =
            srt_buffer_bytes_from_packets(
                static_cast<std::int32_t>(
                    socket.native_options.receive_buffer_packets()),
                parsed);
        options.maximum_payload_size =
            static_cast<std::int32_t>(
                socket.native_options.maximum_payload_size());
        return 0;
    }
    case SRTO_SNDBUF:
    case SRTO_RCVBUF: {
        std::int32_t parsed = 0;
        if (!read_value(value, value_size, parsed) || parsed <= 0) {
            return invalid_parameter();
        }
        std::int32_t packets =
            srt_buffer_packets_from_bytes(
                parsed, options.maximum_segment_size);
        if (packets < minimum_srt_buffer_packets) {
            packets = minimum_srt_buffer_packets;
        }
        if (option == SRTO_RCVBUF) {
            packets = std::min(
                packets, options.flow_window_packets);
        }
        if (option == SRTO_SNDBUF) {
            if (set_native(socket.native_options,
                    SocketOption::send_buffer_packets,
                    packets)
                == SRT_ERROR) {
                return SRT_ERROR;
            }
            options.send_buffer_bytes =
                srt_buffer_bytes_from_packets(
                    packets, options.maximum_segment_size);
        } else {
            if (set_native(socket.native_options,
                    SocketOption::receive_buffer_packets,
                    packets)
                == SRT_ERROR) {
                return SRT_ERROR;
            }
            options.receive_buffer_bytes =
                srt_buffer_bytes_from_packets(
                    packets, options.maximum_segment_size);
        }
        return 0;
    }
    case SRTO_UDP_SNDBUF:
    case SRTO_UDP_RCVBUF: {
        std::int32_t parsed = 0;
        if (!read_value(value, value_size, parsed)
            || parsed <= 0) {
            return invalid_parameter();
        }
        parsed = std::max(
            parsed, options.maximum_segment_size);
        if (option == SRTO_UDP_SNDBUF) {
            options.udp_send_buffer_bytes = parsed;
        } else {
            options.udp_receive_buffer_bytes = parsed;
        }
        return 0;
    }
    case SRTO_REUSEADDR: {
        bool parsed = false;
        if (!read_boolean(value, value_size, parsed)) {
            return invalid_parameter();
        }
        options.reuse_address = parsed;
        return 0;
    }
    case SRTO_IPV6ONLY: {
        std::int32_t parsed = 0;
        if (!read_value(value, value_size, parsed)
            || parsed < -1 || parsed > 1) {
            return invalid_parameter();
        }
        options.ipv6_only = parsed;
        return 0;
    }
    case SRTO_IPTTL:
    case SRTO_IPTOS: {
        std::int32_t parsed = 0;
        if (!read_value(value, value_size, parsed)
            || (option == SRTO_IPTTL
                ? parsed < 1 || parsed > 255
                : parsed < 0 || parsed > 255)) {
            return invalid_parameter();
        }
        if (option == SRTO_IPTTL) {
            options.ip_time_to_live = parsed;
        } else {
            options.ip_type_of_service = parsed;
            options.ip_type_of_service_explicit = true;
        }
        return 0;
    }
    case SRTO_BINDTODEVICE: {
        if (!UdpSocket::bind_to_device_supported()) {
            return unsupported_option();
        }
        if (value_size
                >= static_cast<int>(maximum_network_device_name_size)
            || (value_size != 0
                && std::memchr(value, '\0',
                       static_cast<std::size_t>(value_size))
                    != nullptr)) {
            return invalid_parameter();
        }
        options.bound_device.fill('\0');
        if (value_size != 0) {
            std::memcpy(options.bound_device.data(), value,
                static_cast<std::size_t>(value_size));
        }
        options.bound_device_size =
            static_cast<std::uint8_t>(value_size);
        return 0;
    }
    case SRTO_LINGER: {
        linger parsed{};
        if (!read_value(value, value_size, parsed)) {
            return invalid_parameter();
        }
        const auto seconds = static_cast<std::uint64_t>(
            parsed.l_linger);
        if (seconds > static_cast<std::uint64_t>(
                          std::numeric_limits<std::int32_t>::max())) {
            return invalid_parameter();
        }
        options.linger_enabled = parsed.l_onoff != 0;
        options.linger_seconds =
            static_cast<std::int32_t>(seconds);
        return 0;
    }
    case SRTO_MAXBW: {
        std::int64_t parsed = 0;
        if (!read_value(value, value_size, parsed) || parsed < -1) {
            return invalid_parameter();
        }
        if (set_native(socket.native_options,
                   SocketOption::maximum_bandwidth_bytes_per_second, parsed)
            == SRT_ERROR) {
            return SRT_ERROR;
        }
        options.maximum_bandwidth_bytes_per_second = parsed;
        return 0;
    }
    case SRTO_TSBPDMODE: {
        bool parsed = false;
        if (!read_boolean(value, value_size, parsed)) return invalid_parameter();
        if (set_native(socket.native_options, SocketOption::tsbpd_mode,
                parsed ? 1 : 0)
            == SRT_ERROR) {
            return SRT_ERROR;
        }
        options.tsbpd_mode = parsed;
        return 0;
    }
#ifdef ENABLE_AEAD_API_PREVIEW
    case SRTO_CRYPTOMODE: {
        std::int32_t parsed = 0;
        if (!read_value(value, value_size, parsed)) {
            return invalid_parameter();
        }
        return set_native(
            socket.native_options, SocketOption::crypto_mode, parsed);
    }
#endif
    case SRTO_LATENCY:
    case SRTO_RCVLATENCY: {
        std::int32_t parsed = 0;
        if (!read_value(value, value_size, parsed) || parsed < 0
            || parsed > std::numeric_limits<std::uint16_t>::max()) {
            return invalid_parameter();
        }
        if (set_native(socket.native_options,
                SocketOption::receiver_latency_milliseconds, parsed)
            == SRT_ERROR) {
            return SRT_ERROR;
        }
        options.receiver_latency_milliseconds = parsed;
        if (option == SRTO_LATENCY) {
            if (set_native(socket.native_options,
                    SocketOption::peer_latency_milliseconds, parsed)
                == SRT_ERROR) {
                return SRT_ERROR;
            }
            options.peer_latency_milliseconds = parsed;
        }
        return 0;
    }
    case SRTO_INPUTBW: {
        std::int64_t parsed = 0;
        if (!read_value(value, value_size, parsed) || parsed < 0) {
            return invalid_parameter();
        }
        if (set_native(socket.native_options,
                SocketOption::input_bandwidth_bytes_per_second, parsed)
            == SRT_ERROR) {
            return SRT_ERROR;
        }
        options.input_bandwidth_bytes_per_second = parsed;
        return 0;
    }
    case SRTO_MININPUTBW: {
        std::int64_t parsed = 0;
        if (!read_value(value, value_size, parsed) || parsed < 0) {
            return invalid_parameter();
        }
        if (set_native(socket.native_options,
                SocketOption::minimum_input_bandwidth_bytes_per_second,
                parsed) == SRT_ERROR) {
            return SRT_ERROR;
        }
        options.minimum_input_bandwidth_bytes_per_second = parsed;
        return 0;
    }
    case SRTO_OHEADBW: {
        std::int32_t parsed = 0;
        if (!read_value(value, value_size, parsed)
            || parsed < 5 || parsed > 100) {
            return invalid_parameter();
        }
        if (set_native(socket.native_options,
                SocketOption::overhead_bandwidth_percent, parsed)
            == SRT_ERROR) {
            return SRT_ERROR;
        }
        options.overhead_bandwidth_percent = parsed;
        return 0;
    }
    case SRTO_TLPKTDROP: {
        bool parsed = false;
        if (!read_boolean(value, value_size, parsed)) return invalid_parameter();
        if (set_native(socket.native_options,
                SocketOption::too_late_packet_drop, parsed ? 1 : 0)
            == SRT_ERROR) {
            return SRT_ERROR;
        }
        options.too_late_packet_drop = parsed;
        return 0;
    }
    case SRTO_DRIFTTRACER: {
        bool parsed = false;
        if (!read_boolean(value, value_size, parsed)) {
            return invalid_parameter();
        }
        if (set_native(socket.native_options,
                SocketOption::drift_tracer, parsed ? 1 : 0)
            == SRT_ERROR) {
            return SRT_ERROR;
        }
        options.drift_tracer = parsed;
        return 0;
    }
    case SRTO_SNDDROPDELAY: {
        std::int32_t parsed = 0;
        if (!read_value(value, value_size, parsed) || parsed < -1) {
            return invalid_parameter();
        }
        if (set_native(socket.native_options,
                SocketOption::sender_drop_delay_milliseconds, parsed)
            == SRT_ERROR) {
            return SRT_ERROR;
        }
        options.sender_drop_delay_milliseconds = parsed;
        return 0;
    }
    case SRTO_NAKREPORT: {
        bool parsed = false;
        if (!read_boolean(value, value_size, parsed)) return invalid_parameter();
        if (set_native(socket.native_options,
                SocketOption::periodic_nak, parsed ? 1 : 0)
            == SRT_ERROR) {
            return SRT_ERROR;
        }
        options.periodic_nak = parsed;
        return 0;
    }
    case SRTO_LOSSMAXTTL: {
        std::int32_t parsed = 0;
        if (!read_value(value, value_size, parsed) || parsed < 0) {
            return invalid_parameter();
        }
        if (set_native(socket.native_options,
                SocketOption::maximum_reorder_tolerance_packets,
                parsed)
            == SRT_ERROR) {
            return SRT_ERROR;
        }
        options.maximum_reorder_tolerance_packets = parsed;
        return 0;
    }
    case SRTO_CONNTIMEO: {
        std::int32_t parsed = 0;
        if (!read_value(value, value_size, parsed) || parsed < 0) {
            return invalid_parameter();
        }
        options.connection_timeout_milliseconds = parsed;
        return 0;
    }
    case SRTO_MINVERSION: {
        std::int32_t parsed = 0;
        constexpr std::int32_t maximum_encoded_version = 0x00ff'ffff;
        if (!read_value(value, value_size, parsed)
            || parsed < 0 || parsed > maximum_encoded_version) {
            return invalid_parameter();
        }
        options.minimum_peer_srt_version = parsed;
        return 0;
    }
    case SRTO_PEERIDLETIMEO: {
        std::int32_t parsed = 0;
        if (!read_value(value, value_size, parsed) || parsed < 0) {
            return invalid_parameter();
        }
        options.peer_idle_timeout_milliseconds = parsed;
        return 0;
    }
    case SRTO_PEERLATENCY: {
        std::int32_t parsed = 0;
        if (!read_value(value, value_size, parsed) || parsed < 0
            || parsed > std::numeric_limits<std::uint16_t>::max()) {
            return invalid_parameter();
        }
        if (set_native(socket.native_options,
                SocketOption::peer_latency_milliseconds, parsed)
            == SRT_ERROR) {
            return SRT_ERROR;
        }
        options.peer_latency_milliseconds = parsed;
        return 0;
    }
    case SRTO_PAYLOADSIZE: {
        std::int32_t parsed = 0;
        if (!read_value(value, value_size, parsed)
            || parsed < 0 || parsed > SRT_LIVE_MAX_PLSIZE) {
            return invalid_parameter();
        }
        const std::int32_t effective =
            parsed == 0
            ? std::min<std::int32_t>(
                  SRT_LIVE_DEF_PLSIZE,
                  static_cast<std::int32_t>(
                      socket.native_options
                          .maximum_payload_size_limit()))
            : parsed;
        if (set_native(socket.native_options,
                SocketOption::maximum_payload_size, effective)
            == SRT_ERROR) {
            return SRT_ERROR;
        }
        options.maximum_payload_size = effective;
        return 0;
    }
    case SRTO_MESSAGEAPI: {
        bool parsed = false;
        if (!read_boolean(value, value_size, parsed)) {
            return invalid_parameter();
        }
        if (set_native(socket.native_options,
                SocketOption::message_api, parsed ? 1 : 0)
            == SRT_ERROR) {
            return SRT_ERROR;
        }
        options.message_api = parsed;
        return 0;
    }
    case SRTO_SENDER: {
        bool parsed = false;
        if (!read_boolean(value, value_size, parsed)) {
            return invalid_parameter();
        }
        options.data_sender = parsed;
        return 0;
    }
    case SRTO_CONGESTION: {
        if (value == nullptr || value_size == 0) {
            return invalid_parameter();
        }
        const std::string_view name{
            static_cast<const char*>(value),
            static_cast<std::size_t>(value_size)};
        if (socket.native_options
                .set_congestion_controller(name)
            != Error::none) {
            return invalid_parameter();
        }
        return 0;
    }
    case SRTO_STREAMID: {
        const std::string_view stream_id{
            value_size == 0
                ? ""
                : static_cast<const char*>(value),
            static_cast<std::size_t>(value_size)};
        if (!options.stream_id.assign(stream_id)) {
            return invalid_parameter();
        }
        return 0;
    }
    case SRTO_TRANSTYPE: {
        std::int32_t parsed = 0;
        if (!read_value(value, value_size, parsed)
            || (parsed != static_cast<std::int32_t>(SRTT_LIVE)
                && parsed != static_cast<std::int32_t>(SRTT_FILE))) {
            return invalid_parameter();
        }
        if (set_native(socket.native_options,
                SocketOption::transmission_type, parsed)
            == SRT_ERROR) {
            return SRT_ERROR;
        }
        options.transmission_type =
            static_cast<SRT_TRANSTYPE>(parsed);
        if (options.transmission_type == SRTT_LIVE) {
            options.tsbpd_mode = true;
            options.receiver_latency_milliseconds = 120;
            options.peer_latency_milliseconds = 0;
            options.too_late_packet_drop = true;
            options.sender_drop_delay_milliseconds = 0;
            options.message_api = true;
            options.periodic_nak = true;
            options.retransmission_algorithm = 1;
            options.maximum_payload_size =
                static_cast<std::int32_t>(
                    socket.native_options
                        .maximum_payload_size());
            options.linger_enabled = false;
            options.linger_seconds = 0;
        } else {
            options.tsbpd_mode = false;
            options.receiver_latency_milliseconds = 0;
            options.peer_latency_milliseconds = 0;
            options.too_late_packet_drop = false;
            options.sender_drop_delay_milliseconds = -1;
            options.message_api = false;
            options.periodic_nak = false;
            options.retransmission_algorithm = 0;
            options.maximum_payload_size =
                static_cast<std::int32_t>(
                    socket.native_options
                        .maximum_payload_size());
            options.linger_enabled = true;
            options.linger_seconds = 180;
        }
        return 0;
    }
    case SRTO_PACKETFILTER: {
        if (value == nullptr || value_size == 0) {
            return invalid_parameter();
        }
        const std::string_view configuration{
            static_cast<const char*>(value),
            static_cast<std::size_t>(value_size)};
        if (socket.native_options.set_packet_filter(
                configuration) != Error::none) {
            return invalid_parameter();
        }
        options.maximum_payload_size =
            static_cast<std::int32_t>(
                socket.native_options
                    .maximum_payload_size());
        return 0;
    }
    case SRTO_RETRANSMITALGO: {
        std::int32_t parsed = 0;
        if (!read_value(value, value_size, parsed)
            || (parsed != 0 && parsed != 1)) {
            return invalid_parameter();
        }
        if (set_native(socket.native_options,
                SocketOption::retransmit_flag, parsed)
            == SRT_ERROR) {
            return SRT_ERROR;
        }
        options.retransmission_algorithm = parsed;
        return 0;
    }
    case SRTO_RENDEZVOUS: {
        bool parsed = false;
        if (!read_boolean(value, value_size, parsed)) {
            return invalid_parameter();
        }
        if (set_native(socket.native_options,
                SocketOption::rendezvous, parsed ? 1 : 0)
            == SRT_ERROR) {
            return SRT_ERROR;
        }
        options.rendezvous = parsed;
        return 0;
    }
    case SRTO_GROUPCONNECT: {
        bool parsed = false;
        if (!read_boolean(value, value_size, parsed)) {
            return invalid_parameter();
        }
        options.group_connect = parsed;
        return 0;
    }
    case SRTO_PASSPHRASE: {
        const std::string_view passphrase =
            value_size == 0
            ? std::string_view{}
            : std::string_view{
                  static_cast<const char*>(value),
                  static_cast<std::size_t>(value_size)};
        return socket.native_options.set_passphrase(passphrase)
                == Error::none
            ? 0 : invalid_parameter();
    }
    case SRTO_PBKEYLEN: {
        std::int32_t parsed = 0;
        if (!read_value(value, value_size, parsed)) {
            return invalid_parameter();
        }
        return set_native(socket.native_options,
            SocketOption::encryption_key_length, parsed);
    }
    case SRTO_KMREFRESHRATE:
    case SRTO_KMPREANNOUNCE: {
        std::int32_t parsed = 0;
        if (!read_value(value, value_size, parsed)
            || parsed < 0) {
            return invalid_parameter();
        }
        return set_native(socket.native_options,
            option == SRTO_KMREFRESHRATE
                ? SocketOption::key_refresh_rate_packets
                : SocketOption::key_preannouncement_packets,
            parsed);
    }
    case SRTO_ENFORCEDENCRYPTION: {
        bool parsed = false;
        if (!read_boolean(value, value_size, parsed)) {
            return invalid_parameter();
        }
        return set_native(socket.native_options,
            SocketOption::enforced_encryption,
            parsed ? 1 : 0);
    }
    case SRTO_ISN:
    case SRTO_STATE:
    case SRTO_EVENT:
    case SRTO_SNDDATA:
    case SRTO_RCVDATA:
    case SRTO_VERSION:
    case SRTO_PEERVERSION:
    case SRTO_GROUPTYPE:
    case SRTO_KMSTATE:
    case SRTO_SNDKMSTATE:
    case SRTO_RCVKMSTATE:
        return unsupported_option();
    default:
        return unsupported_option();
    }
}

} // namespace robotweax::srt::compat
