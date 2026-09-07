#include "robotweax_srt.h"

#include "robotweax/srt/codec.hpp"
#include "robotweax/srt/socket_options.hpp"

#include <cstddef>
#include <span>
#include <new>

struct robotweax_srt_options {
    robotweax::srt::SocketOptions value;
};

namespace {

[[nodiscard]] robotweax_srt_error translate(robotweax::srt::Error error) noexcept
{
    using robotweax::srt::Error;
    switch (error) {
    case Error::none: return ROBOTWEAX_SRT_OK;
    case Error::buffer_too_small: return ROBOTWEAX_SRT_BUFFER_TOO_SMALL;
    case Error::packet_too_short: return ROBOTWEAX_SRT_PACKET_TOO_SHORT;
    case Error::invalid_packet_type: return ROBOTWEAX_SRT_INVALID_PACKET_TYPE;
    case Error::invalid_control_type: return ROBOTWEAX_SRT_INVALID_CONTROL_TYPE;
    case Error::invalid_state: return ROBOTWEAX_SRT_INVALID_OPTION_VALUE;
    case Error::unsupported: return ROBOTWEAX_SRT_INVALID_OPTION;
    default: return ROBOTWEAX_SRT_INVALID_PACKET_TYPE;
    }
}

[[nodiscard]] bool translate_option(int source,
    robotweax::srt::SocketOption& destination) noexcept
{
    using robotweax::srt::SocketOption;
    switch (source) {
    case ROBOTWEAX_SRT_OPT_INPUT_BANDWIDTH: destination = SocketOption::input_bandwidth_bytes_per_second; return true;
    case ROBOTWEAX_SRT_OPT_MAXIMUM_BANDWIDTH: destination = SocketOption::maximum_bandwidth_bytes_per_second; return true;
    case ROBOTWEAX_SRT_OPT_OVERHEAD_PERCENT: destination = SocketOption::overhead_bandwidth_percent; return true;
    case ROBOTWEAX_SRT_OPT_RECEIVER_LATENCY: destination = SocketOption::receiver_latency_milliseconds; return true;
    case ROBOTWEAX_SRT_OPT_PEER_LATENCY: destination = SocketOption::peer_latency_milliseconds; return true;
    case ROBOTWEAX_SRT_OPT_SENDER_DROP_DELAY: destination = SocketOption::sender_drop_delay_milliseconds; return true;
    case ROBOTWEAX_SRT_OPT_TSBPD_MODE: destination = SocketOption::tsbpd_mode; return true;
    case ROBOTWEAX_SRT_OPT_TOO_LATE_PACKET_DROP: destination = SocketOption::too_late_packet_drop; return true;
    case ROBOTWEAX_SRT_OPT_PERIODIC_NAK: destination = SocketOption::periodic_nak; return true;
    case ROBOTWEAX_SRT_OPT_RETRANSMIT_FLAG: destination = SocketOption::retransmit_flag; return true;
    case ROBOTWEAX_SRT_OPT_SEND_BUFFER_PACKETS: destination = SocketOption::send_buffer_packets; return true;
    case ROBOTWEAX_SRT_OPT_RECEIVE_BUFFER_PACKETS: destination = SocketOption::receive_buffer_packets; return true;
    case ROBOTWEAX_SRT_OPT_FLOW_WINDOW_PACKETS: destination = SocketOption::flow_window_packets; return true;
    case ROBOTWEAX_SRT_OPT_MAXIMUM_PAYLOAD_SIZE: destination = SocketOption::maximum_payload_size; return true;
    case ROBOTWEAX_SRT_OPT_RENDEZVOUS: destination = SocketOption::rendezvous; return true;
    case ROBOTWEAX_SRT_OPT_MESSAGE_API: destination = SocketOption::message_api; return true;
    case ROBOTWEAX_SRT_OPT_TRANSMISSION_TYPE: destination = SocketOption::transmission_type; return true;
    case ROBOTWEAX_SRT_OPT_MAXIMUM_REORDER_TOLERANCE:
        destination =
            SocketOption::maximum_reorder_tolerance_packets;
        return true;
    }
    return false;
}

} // namespace

extern "C" robotweax_srt_error robotweax_srt_inspect_packet(
    const uint8_t* bytes,
    size_t size,
    robotweax_srt_packet_info* info)
{
    if (bytes == nullptr || info == nullptr) {
        return ROBOTWEAX_SRT_INVALID_ARGUMENT;
    }
    if (info->struct_size < sizeof(robotweax_srt_packet_info)
        || info->abi_version != ROBOTWEAX_SRT_ABI_VERSION) {
        return ROBOTWEAX_SRT_ABI_MISMATCH;
    }
    const auto input = std::as_bytes(std::span{bytes, size});
    const auto decoded = robotweax::srt::decode_packet(input);
    if (!decoded) {
        return translate(decoded.error);
    }

    info->kind = decoded.packet.kind == robotweax::srt::PacketKind::data
        ? ROBOTWEAX_SRT_DATA_PACKET
        : ROBOTWEAX_SRT_CONTROL_PACKET;
    info->sequence_number = decoded.packet.data.sequence.value();
    info->message_number = decoded.packet.data.message_number;
    info->control_type = static_cast<uint16_t>(decoded.packet.control.type);
    info->control_subtype = decoded.packet.control.subtype;
    info->timestamp = decoded.packet.kind == robotweax::srt::PacketKind::data
        ? decoded.packet.data.timestamp.value()
        : decoded.packet.control.timestamp.value();
    info->destination_socket_id = decoded.packet.kind == robotweax::srt::PacketKind::data
        ? decoded.packet.data.destination_socket_id
        : decoded.packet.control.destination_socket_id;
    info->payload_offset = robotweax::srt::packet_header_size;
    info->payload_size = decoded.packet.payload.size();
    return ROBOTWEAX_SRT_OK;
}

extern "C" const char* robotweax_srt_error_string(robotweax_srt_error error)
{
    switch (error) {
    case ROBOTWEAX_SRT_OK: return "no error";
    case ROBOTWEAX_SRT_BUFFER_TOO_SMALL: return "buffer too small";
    case ROBOTWEAX_SRT_PACKET_TOO_SHORT: return "packet too short";
    case ROBOTWEAX_SRT_INVALID_PACKET_TYPE: return "invalid packet type";
    case ROBOTWEAX_SRT_INVALID_CONTROL_TYPE: return "invalid control type";
    case ROBOTWEAX_SRT_INVALID_ARGUMENT: return "invalid argument";
    case ROBOTWEAX_SRT_ABI_MISMATCH: return "ABI version or structure size mismatch";
    case ROBOTWEAX_SRT_INVALID_OPTION: return "invalid socket option";
    case ROBOTWEAX_SRT_INVALID_OPTION_VALUE: return "invalid socket option value";
    case ROBOTWEAX_SRT_ALLOCATION_FAILURE: return "allocation failure";
    }
    return "unknown error";
}

extern "C" robotweax_srt_error robotweax_srt_options_create(
    robotweax_srt_options** options)
{
    if (options == nullptr) {
        return ROBOTWEAX_SRT_INVALID_ARGUMENT;
    }
    *options = new (std::nothrow) robotweax_srt_options{};
    return *options == nullptr ? ROBOTWEAX_SRT_ALLOCATION_FAILURE : ROBOTWEAX_SRT_OK;
}

extern "C" void robotweax_srt_options_destroy(robotweax_srt_options* options)
{
    delete options;
}

extern "C" robotweax_srt_error robotweax_srt_options_set(
    robotweax_srt_options* options,
    int option,
    int64_t value)
{
    if (options == nullptr) {
        return ROBOTWEAX_SRT_INVALID_ARGUMENT;
    }
    robotweax::srt::SocketOption translated{};
    if (!translate_option(option, translated)) {
        return ROBOTWEAX_SRT_INVALID_OPTION;
    }
    return translate(options->value.set(translated, value));
}

extern "C" robotweax_srt_error robotweax_srt_options_get(
    const robotweax_srt_options* options,
    int option,
    int64_t* value)
{
    if (options == nullptr || value == nullptr) {
        return ROBOTWEAX_SRT_INVALID_ARGUMENT;
    }
    robotweax::srt::SocketOption translated{};
    if (!translate_option(option, translated)) {
        return ROBOTWEAX_SRT_INVALID_OPTION;
    }
    const auto result = options->value.get(translated);
    if (!result) {
        return translate(result.error);
    }
    *value = result.value;
    return ROBOTWEAX_SRT_OK;
}
