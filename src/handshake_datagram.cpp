#include "robotweax/srt/handshake_datagram.hpp"

#include "robotweax/srt/codec.hpp"
#include "robotweax/srt/connection_group.hpp"
#include "robotweax/srt/handshake_extensions.hpp"

#include <array>
#include <algorithm>

namespace robotweax::srt {
namespace {

[[nodiscard]] constexpr bool extension_is_advertised(
    HandshakeExtensionType type,
    std::uint16_t flags) noexcept
{
    switch (type) {
    case HandshakeExtensionType::handshake_request:
    case HandshakeExtensionType::handshake_response:
        return (flags & handshake_extension_flag_handshake) != 0U;
    case HandshakeExtensionType::key_material_request:
    case HandshakeExtensionType::key_material_response:
        return (flags & handshake_extension_flag_key_material) != 0U;
    case HandshakeExtensionType::stream_id:
    case HandshakeExtensionType::congestion:
    case HandshakeExtensionType::packet_filter:
    case HandshakeExtensionType::group:
        return (flags & handshake_extension_flag_config) != 0U;
    }

    // Unknown record types are deliberately opaque. Requiring one of today's
    // three category bits would prevent a future extension from defining a
    // new category while older implementations could otherwise skip it.
    return true;
}

} // namespace

HandshakeDatagramDecodeResult decode_handshake_datagram(
    std::span<const std::byte> datagram) noexcept
{
    const auto packet = decode_packet(datagram);
    if (!packet) {
        return {.error = packet.error};
    }
    if (packet.packet.kind != PacketKind::control
        || packet.packet.control.type != ControlType::handshake) {
        return {.error = Error::invalid_packet_type};
    }
    const auto payload = decode_handshake_payload(packet.packet.payload);
    if (!payload) {
        return {.error = payload.error};
    }

    HandshakeMessage message{.packet = payload.handshake};
    auto extensions = payload.extensions;
    while (!extensions.empty()) {
        const auto decoded = decode_extension(extensions);
        if (!decoded) {
            return {.error = decoded.error};
        }
        if (!extension_is_advertised(
                decoded.extension.type,
                message.packet.extension_field)) {
            return {.error = Error::invalid_extension};
        }
        if (decoded.extension.type == HandshakeExtensionType::handshake_request
            || decoded.extension.type == HandshakeExtensionType::handshake_response) {
            if (message.has_handshake_extension) {
                return {.error = Error::invalid_extension};
            }
            const auto parameters = decode_handshake_parameters(decoded.extension);
            if (!parameters) {
                return {.error = parameters.error};
            }
            message.has_handshake_extension = true;
            message.extension_type = decoded.extension.type;
            message.extension_parameters = parameters.parameters;
        } else if (decoded.extension.type
                == HandshakeExtensionType::key_material_request
            || decoded.extension.type
                == HandshakeExtensionType::key_material_response) {
            if (message.has_key_material_extension
                || decoded.extension.content.size()
                    > message.key_material.bytes.size()) {
                return {.error = Error::invalid_extension};
            }
            const bool failure_response =
                decoded.extension.type
                    == HandshakeExtensionType::
                        key_material_response
                && decoded.extension.content.size() == 4U;
            if (!failure_response) {
                const auto key_material =
                    decode_key_material(decoded.extension.content);
                if (!key_material) {
                    return {.error = key_material.error};
                }
            }
            message.has_key_material_extension = true;
            message.key_material_extension_type =
                decoded.extension.type;
            std::copy(decoded.extension.content.begin(),
                decoded.extension.content.end(),
                message.key_material.bytes.begin());
            message.key_material.size =
                decoded.extension.content.size();
        } else if (decoded.extension.type
                == HandshakeExtensionType::stream_id) {
            if (message.has_stream_id_extension) {
                return {.error = Error::invalid_extension};
            }
            const auto text =
                decode_extension_text(decoded.extension);
            if (!text
                || !message.stream_id.assign(text.view())) {
                return {.error = Error::invalid_extension};
            }
            message.has_stream_id_extension = true;
        } else if (decoded.extension.type
                == HandshakeExtensionType::congestion) {
            if (message.has_congestion_extension) {
                return {.error = Error::invalid_extension};
            }
            const auto text =
                decode_extension_text(decoded.extension);
            CongestionController controller =
                CongestionController::live;
            if (!text || !parse_congestion_controller(
                    text.view(), controller)) {
                return {.error = Error::invalid_extension};
            }
            message.has_congestion_extension = true;
            message.congestion_controller = controller;
        } else if (decoded.extension.type
                == HandshakeExtensionType::packet_filter) {
            if (message.has_packet_filter_extension) {
                return {.error = Error::invalid_extension};
            }
            const auto text =
                decode_extension_text(decoded.extension);
            if (!text || text.size == 0U) {
                return {.error = Error::invalid_extension};
            }
            const auto configuration =
                parse_packet_filter_configuration(
                    text.view());
            if (!configuration
                || !configuration.configuration.enabled) {
                return {.error = Error::invalid_extension};
            }
            message.has_packet_filter_extension = true;
            message.packet_filter_configuration =
                configuration.configuration;
        } else if (decoded.extension.type
                == HandshakeExtensionType::group) {
            if (message.has_group_membership) {
                return {.error = Error::invalid_extension};
            }
            const auto membership =
                decode_group_membership(decoded.extension);
            if (!membership) {
                return {.error = membership.error};
            }
            message.has_group_membership = true;
            message.group_membership = membership.membership;
        } else {
            message.has_unknown_extension = true;
        }
        extensions = extensions.subspan(decoded.bytes_consumed);
    }
    return {.message = message, .control = packet.packet.control};
}

HandshakeDatagramEncodeResult encode_handshake_datagram(
    const HandshakeAction& action,
    PacketTimestamp timestamp,
    std::uint32_t destination_socket_id,
    std::span<std::byte> destination) noexcept
{
    if (action.kind != HandshakeActionKind::send) {
        return {.error = Error::invalid_state};
    }
    const std::size_t handshake_extension_size =
        action.has_handshake_extension
        ? extension_header_size + handshake_extension_content_size
        : 0U;
    const std::size_t key_material_extension_size =
        action.has_key_material_extension
        ? extension_header_size + action.key_material.size
        : 0U;
    const std::size_t stream_id_content_size =
        action.has_stream_id_extension
        ? ((action.stream_id.size + 3U) & ~std::size_t{3U})
        : 0U;
    const std::size_t stream_id_extension_size =
        action.has_stream_id_extension
        ? extension_header_size + stream_id_content_size
        : 0U;
    const std::size_t packet_filter_content_size =
        action.has_packet_filter_extension
        ? ((action.packet_filter_configuration.text_size
              + 3U) & ~std::size_t{3U})
        : 0U;
    const std::size_t packet_filter_extension_size =
        action.has_packet_filter_extension
        ? extension_header_size
            + packet_filter_content_size
        : 0U;
    const std::size_t congestion_content_size =
        action.has_congestion_extension
        ? ((congestion_controller_name(
                action.congestion_controller).size()
              + 3U) & ~std::size_t{3U})
        : 0U;
    const std::size_t congestion_extension_size =
        action.has_congestion_extension
        ? extension_header_size
            + congestion_content_size
        : 0U;
    const std::size_t group_extension_size =
        action.has_group_membership
        ? extension_header_size + group_membership_content_size
        : 0U;
    const std::size_t required = packet_header_size + handshake_size
        + handshake_extension_size
        + key_material_extension_size
        + stream_id_extension_size
        + congestion_extension_size
        + packet_filter_extension_size
        + group_extension_size;
    if (destination.size() < required) {
        return {.error = Error::buffer_too_small};
    }

    MutablePacketView packet;
    packet.kind = PacketKind::control;
    packet.control.type = ControlType::handshake;
    packet.control.timestamp = timestamp;
    packet.control.destination_socket_id = destination_socket_id;
    const auto packet_header = encode_packet(packet, destination.first(packet_header_size));
    if (!packet_header) {
        return {.error = packet_header.error};
    }

    const auto handshake_error = encode_handshake(
        action.packet,
        destination.subspan(packet_header_size, handshake_size));
    if (handshake_error != Error::none) {
        return {.error = handshake_error};
    }
    if (action.has_handshake_extension) {
        const auto extension = encode_handshake_parameters(
            action.extension_type,
            action.extension_parameters,
            destination.subspan(
                packet_header_size + handshake_size,
                handshake_extension_size));
        if (!extension) {
            return {.error = extension.error};
        }
    }
    if (action.has_key_material_extension) {
        if ((action.key_material_extension_type
                    != HandshakeExtensionType::key_material_request
                && action.key_material_extension_type
                    != HandshakeExtensionType::key_material_response)
            || action.key_material.size == 0U
            || (action.key_material.size != 4U
                && !decode_key_material(
                    action.key_material.view()))
            || (action.key_material.size == 4U
                && action.key_material_extension_type
                    != HandshakeExtensionType::
                        key_material_response)) {
            return {.error = Error::invalid_key_material};
        }
        const auto extension = encode_extension(
            action.key_material_extension_type,
            action.key_material.view(),
            destination.subspan(packet_header_size + handshake_size
                + handshake_extension_size,
                key_material_extension_size));
        if (!extension) {
            return {.error = extension.error};
        }
    }
    if (action.has_stream_id_extension) {
        const auto extension = encode_stream_id(
            action.stream_id.view(),
            destination.subspan(
                packet_header_size + handshake_size
                    + handshake_extension_size
                    + key_material_extension_size,
                stream_id_extension_size));
        if (!extension) {
            return {.error = extension.error};
        }
    }
    if (action.has_congestion_extension) {
        const auto name = congestion_controller_name(
            action.congestion_controller);
        if (name.empty()) {
            return {.error = Error::invalid_extension};
        }
        const auto extension = encode_extension_text(
            HandshakeExtensionType::congestion,
            name,
            destination.subspan(
                packet_header_size + handshake_size
                    + handshake_extension_size
                    + key_material_extension_size
                    + stream_id_extension_size,
                congestion_extension_size));
        if (!extension) {
            return {.error = extension.error};
        }
    }
    if (action.has_packet_filter_extension) {
        if (!action.packet_filter_configuration.enabled
            || action.packet_filter_configuration
                .view().empty()) {
            return {.error = Error::invalid_extension};
        }
        const auto extension = encode_extension_text(
            HandshakeExtensionType::packet_filter,
            action.packet_filter_configuration.view(),
            destination.subspan(
                packet_header_size + handshake_size
                    + handshake_extension_size
                    + key_material_extension_size
                    + stream_id_extension_size
                    + congestion_extension_size,
                packet_filter_extension_size));
        if (!extension) {
            return {.error = extension.error};
        }
    }
    if (action.has_group_membership) {
        if (validate_group_membership(action.group_membership)
            != GroupMembershipValidation::compatible) {
            return {.error = Error::invalid_extension};
        }
        const auto extension = encode_group_membership(
            action.group_membership,
            destination.subspan(
                packet_header_size + handshake_size
                    + handshake_extension_size
                    + key_material_extension_size
                    + stream_id_extension_size
                    + congestion_extension_size
                    + packet_filter_extension_size,
                group_extension_size));
        if (!extension) {
            return {.error = extension.error};
        }
    }
    return {.bytes_written = required};
}

} // namespace robotweax::srt
