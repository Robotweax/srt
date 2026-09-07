#include "robotweax/srt/handshake_extensions.hpp"

#include <algorithm>
#include <array>
#include <limits>

namespace robotweax::srt {
namespace {

[[nodiscard]] constexpr bool has_flag(std::uint32_t flags,
    HandshakeExtensionFlag flag) noexcept
{
    return (flags & static_cast<std::uint32_t>(flag)) != 0U;
}

[[nodiscard]] std::uint16_t read_u16(const std::byte* bytes) noexcept
{
    return static_cast<std::uint16_t>(
        (std::to_integer<std::uint16_t>(bytes[0]) << 8U)
        | std::to_integer<std::uint16_t>(bytes[1]));
}

[[nodiscard]] std::uint32_t read_u32(const std::byte* bytes) noexcept
{
    return (std::to_integer<std::uint32_t>(bytes[0]) << 24U)
        | (std::to_integer<std::uint32_t>(bytes[1]) << 16U)
        | (std::to_integer<std::uint32_t>(bytes[2]) << 8U)
        | std::to_integer<std::uint32_t>(bytes[3]);
}

void write_u16(std::byte* bytes, std::uint16_t value) noexcept
{
    bytes[0] = static_cast<std::byte>((value >> 8U) & 0xffU);
    bytes[1] = static_cast<std::byte>(value & 0xffU);
}

void write_u32(std::byte* bytes, std::uint32_t value) noexcept
{
    bytes[0] = static_cast<std::byte>((value >> 24U) & 0xffU);
    bytes[1] = static_cast<std::byte>((value >> 16U) & 0xffU);
    bytes[2] = static_cast<std::byte>((value >> 8U) & 0xffU);
    bytes[3] = static_cast<std::byte>(value & 0xffU);
}

[[nodiscard]] bool is_known_extension(std::uint16_t type) noexcept
{
    return type >= static_cast<std::uint16_t>(HandshakeExtensionType::handshake_request)
        && type <= static_cast<std::uint16_t>(HandshakeExtensionType::group);
}

} // namespace

bool parse_congestion_controller(
    std::string_view name,
    CongestionController& controller) noexcept
{
    if (name == "live") {
        controller = CongestionController::live;
        return true;
    }
    if (name == "file") {
        controller = CongestionController::file;
        return true;
    }
    return false;
}

NegotiatedLiveOptions negotiate_live_options(
    const HandshakeExtensionParameters& local,
    const HandshakeExtensionParameters& peer) noexcept
{
    const bool send_tsbpd = has_flag(local.flags, HandshakeExtensionFlag::tsbpd_send)
        && has_flag(peer.flags, HandshakeExtensionFlag::tsbpd_receive);
    const bool receive_tsbpd = has_flag(local.flags, HandshakeExtensionFlag::tsbpd_receive)
        && has_flag(peer.flags, HandshakeExtensionFlag::tsbpd_send);
    const bool both_too_late = has_flag(local.flags,
        HandshakeExtensionFlag::too_late_packet_drop)
        && has_flag(peer.flags, HandshakeExtensionFlag::too_late_packet_drop);
    return {
        .send_tsbpd = send_tsbpd,
        .receive_tsbpd = receive_tsbpd,
        .too_late_packet_drop = send_tsbpd && both_too_late,
        .periodic_nak = has_flag(local.flags, HandshakeExtensionFlag::periodic_nak)
            && has_flag(peer.flags, HandshakeExtensionFlag::periodic_nak),
        .retransmit_flag = has_flag(local.flags, HandshakeExtensionFlag::retransmit_flag)
            && has_flag(peer.flags, HandshakeExtensionFlag::retransmit_flag),
        .receive_delay_milliseconds = static_cast<std::uint16_t>(receive_tsbpd
                ? std::max(local.receiver_tsbpd_delay_milliseconds,
                      peer.sender_tsbpd_delay_milliseconds)
                : 0),
        .peer_receive_delay_milliseconds = static_cast<std::uint16_t>(send_tsbpd
                ? std::max(local.sender_tsbpd_delay_milliseconds,
                      peer.receiver_tsbpd_delay_milliseconds)
                : 0),
    };
}

ExtensionResult decode_extension(std::span<const std::byte> bytes) noexcept
{
    if (bytes.size() < extension_header_size) {
        return {.error = Error::invalid_extension};
    }
    const auto raw_type = read_u16(bytes.data());
    const auto length_words = read_u16(bytes.data() + 2);
    const std::size_t content_size = static_cast<std::size_t>(length_words) * 4U;
    const std::size_t total_size = extension_header_size + content_size;
    if (total_size > bytes.size()) {
        return {.error = Error::invalid_extension};
    }
    return {
        .extension = {
            .type = static_cast<HandshakeExtensionType>(raw_type),
            .content = bytes.subspan(extension_header_size, content_size),
        },
        .bytes_consumed = total_size,
    };
}

ExtensionEncodeResult encode_extension(
    HandshakeExtensionType type,
    std::span<const std::byte> content,
    std::span<std::byte> destination) noexcept
{
    if ((content.size() % 4U) != 0U
        || content.size() / 4U > std::numeric_limits<std::uint16_t>::max()) {
        return {.error = Error::invalid_extension};
    }
    const std::size_t required = extension_header_size + content.size();
    if (destination.size() < required) {
        return {.error = Error::buffer_too_small};
    }
    const auto raw_type = static_cast<std::uint16_t>(type);
    if (!is_known_extension(raw_type)) {
        return {.error = Error::unsupported};
    }
    write_u16(destination.data(), raw_type);
    write_u16(destination.data() + 2, static_cast<std::uint16_t>(content.size() / 4U));
    std::copy(content.begin(), content.end(), destination.begin() + extension_header_size);
    return {.bytes_written = required};
}

HandshakeParametersResult decode_handshake_parameters(
    const HandshakeExtensionView& extension) noexcept
{
    if ((extension.type != HandshakeExtensionType::handshake_request
            && extension.type != HandshakeExtensionType::handshake_response)
        || extension.content.size() != handshake_extension_content_size) {
        return {.error = Error::invalid_extension};
    }
    HandshakeExtensionParameters parameters;
    parameters.srt_version = read_u32(extension.content.data());
    parameters.flags = read_u32(extension.content.data() + 4);
    parameters.receiver_tsbpd_delay_milliseconds = read_u16(extension.content.data() + 8);
    parameters.sender_tsbpd_delay_milliseconds = read_u16(extension.content.data() + 10);
    return {.parameters = parameters};
}

ExtensionEncodeResult encode_handshake_parameters(
    HandshakeExtensionType type,
    const HandshakeExtensionParameters& parameters,
    std::span<std::byte> destination) noexcept
{
    if (type != HandshakeExtensionType::handshake_request
        && type != HandshakeExtensionType::handshake_response) {
        return {.error = Error::invalid_extension};
    }
    std::array<std::byte, handshake_extension_content_size> content{};
    write_u32(content.data(), parameters.srt_version);
    write_u32(content.data() + 4, parameters.flags);
    write_u16(content.data() + 8, parameters.receiver_tsbpd_delay_milliseconds);
    write_u16(content.data() + 10, parameters.sender_tsbpd_delay_milliseconds);
    return encode_extension(type, content, destination);
}

ExtensionEncodeResult encode_stream_id(
    std::string_view stream_id,
    std::span<std::byte> destination) noexcept
{
    return encode_extension_text(
        HandshakeExtensionType::stream_id,
        stream_id, destination);
}

ExtensionEncodeResult encode_extension_text(
    HandshakeExtensionType type,
    std::string_view text,
    std::span<std::byte> destination) noexcept
{
    if ((type != HandshakeExtensionType::stream_id
            && type
                != HandshakeExtensionType::congestion
            && type
                != HandshakeExtensionType::packet_filter)
        || text.size() > maximum_stream_id_size) {
        return {.error = Error::invalid_extension};
    }
    const std::size_t padded_size =
        (text.size() + 3U) & ~std::size_t{3U};
    const std::size_t required = extension_header_size + padded_size;
    if (destination.size() < required) {
        return {.error = Error::buffer_too_small};
    }
    write_u16(destination.data(),
        static_cast<std::uint16_t>(type));
    write_u16(destination.data() + 2, static_cast<std::uint16_t>(padded_size / 4U));
    auto content = destination.subspan(extension_header_size, padded_size);
    std::fill(content.begin(), content.end(), std::byte{0});
    for (std::size_t index = 0; index < text.size(); ++index) {
        const std::size_t word = index / 4U;
        const std::size_t byte_in_word = index % 4U;
        const std::size_t wire_index = word * 4U + (3U - byte_in_word);
        content[wire_index] = static_cast<std::byte>(
            static_cast<unsigned char>(text[index]));
    }
    return {.bytes_written = required};
}

ExtensionTextResult decode_extension_text(
    const HandshakeExtensionView& extension) noexcept
{
    if ((extension.type != HandshakeExtensionType::stream_id
            && extension.type
                != HandshakeExtensionType::congestion
            && extension.type
                != HandshakeExtensionType::packet_filter)
        || extension.content.size()
            > maximum_stream_id_size
        || (extension.content.size() % 4U) != 0U) {
        return {.error = Error::invalid_extension};
    }
    ExtensionTextResult result;
    for (std::size_t index = 0;
         index < extension.content.size(); ++index) {
        const std::size_t word = index / 4U;
        const std::size_t byte_in_word = index % 4U;
        const std::size_t wire_index =
            word * 4U + (3U - byte_in_word);
        result.text[index] = static_cast<char>(
            std::to_integer<unsigned char>(
                extension.content[wire_index]));
    }
    result.size = extension.content.size();
    while (result.size != 0U
        && result.text[result.size - 1U] == '\0') {
        --result.size;
    }
    return result;
}

} // namespace robotweax::srt
