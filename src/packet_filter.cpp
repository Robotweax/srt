#include "robotweax/srt/packet_filter.hpp"

#include <charconv>
#include <limits>

namespace robotweax::srt {
namespace {

[[nodiscard]] constexpr bool valid_fec_geometry(
    std::uint32_t columns,
    std::int32_t rows) noexcept
{
    if (rows == 1) {
        return true;
    }
    if (rows == 0 || rows == -1) {
        return false;
    }
    const std::uint64_t column_size = rows < 0
        ? static_cast<std::uint64_t>(
              -static_cast<std::int64_t>(rows))
        : static_cast<std::uint64_t>(rows);
    return columns <= 128U
        && static_cast<std::uint64_t>(columns)
                * column_size
            < SequenceNumber::half_range;
}

[[nodiscard]] bool store_text(
    PacketFilterConfiguration& configuration,
    std::string_view text) noexcept
{
    if (text.size()
        > maximum_packet_filter_configuration_size) {
        return false;
    }
    configuration.text.fill('\0');
    for (std::size_t index = 0; index < text.size();
         ++index) {
        if (text[index] == '\0') {
            return false;
        }
        configuration.text[index] = text[index];
    }
    configuration.text_size = text.size();
    return true;
}

template <typename Integer>
[[nodiscard]] bool parse_integer(
    std::string_view text, Integer& value) noexcept
{
    if (text.empty()) {
        return false;
    }
    const auto parsed = std::from_chars(
        text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{}
        && parsed.ptr == text.data() + text.size();
}

[[nodiscard]] bool append(
    PacketFilterConfiguration& destination,
    std::string_view text) noexcept
{
    if (text.size()
        > maximum_packet_filter_configuration_size
            - destination.text_size) {
        return false;
    }
    for (const char character : text) {
        destination.text[
            destination.text_size++] = character;
    }
    destination.text[destination.text_size] = '\0';
    return true;
}

template <typename Integer>
[[nodiscard]] bool append_integer(
    PacketFilterConfiguration& destination,
    Integer value) noexcept
{
    std::array<char, 32> buffer{};
    const auto converted = std::to_chars(
        buffer.data(), buffer.data() + buffer.size(),
        value);
    return converted.ec == std::errc{}
        && append(destination, {
            buffer.data(),
            static_cast<std::size_t>(
                converted.ptr - buffer.data())});
}

[[nodiscard]] bool format_effective(
    PacketFilterConfiguration& configuration) noexcept
{
    configuration.text.fill('\0');
    configuration.text_size = 0;
    const std::string_view arq =
        configuration.arq == PacketFilterArqLevel::never
        ? "never"
        : configuration.arq
                == PacketFilterArqLevel::always
            ? "always" : "onreq";
    const std::string_view layout =
        configuration.layout == PacketFilterLayout::even
        ? "even" : "staircase";
    return append(configuration, "fec,arq:")
        && append(configuration, arq)
        && append(configuration, ",cols:")
        && append_integer(configuration,
            configuration.columns)
        && append(configuration, ",layout:")
        && append(configuration, layout)
        && append(configuration, ",rows:")
        && append_integer(configuration,
            configuration.rows);
}

template <typename Value>
[[nodiscard]] bool merge_value(
    bool local_specified, Value local,
    bool peer_specified, Value peer,
    Value default_value, Value& result) noexcept
{
    if (local_specified && peer_specified
        && local != peer) {
        return false;
    }
    result = local_specified ? local
        : peer_specified ? peer : default_value;
    return true;
}

template <typename Value>
[[nodiscard]] Value response_value(
    bool local_specified, Value local,
    bool response_specified, Value response,
    Value default_value) noexcept
{
    return response_specified ? response
        : local_specified ? local : default_value;
}

[[nodiscard]] PacketFilterConfigurationResult
resolve_effective_configuration(
    const PacketFilterConfiguration& local,
    const PacketFilterConfiguration& peer,
    bool peer_overrides) noexcept
{
    if (!local.enabled && !peer.enabled) {
        return {};
    }

    PacketFilterConfiguration result;
    result.enabled = true;
    if (peer_overrides) {
        result.columns = response_value(
            local.columns_specified, local.columns,
            peer.columns_specified, peer.columns,
            std::uint32_t{0});
        result.rows = response_value(
            local.rows_specified, local.rows,
            peer.rows_specified, peer.rows,
            std::int32_t{1});
        result.layout = response_value(
            local.layout_specified, local.layout,
            peer.layout_specified, peer.layout,
            PacketFilterLayout::staircase);
        result.arq = response_value(
            local.arq_specified, local.arq,
            peer.arq_specified, peer.arq,
            PacketFilterArqLevel::on_request);
    } else if (!merge_value(
                   local.columns_specified,
                   local.columns,
                   peer.columns_specified,
                   peer.columns,
                   std::uint32_t{0},
                   result.columns)
        || !merge_value(
            local.rows_specified, local.rows,
            peer.rows_specified, peer.rows,
            std::int32_t{1}, result.rows)
        || !merge_value(
            local.layout_specified, local.layout,
            peer.layout_specified, peer.layout,
            PacketFilterLayout::staircase,
            result.layout)
        || !merge_value(
            local.arq_specified, local.arq,
            peer.arq_specified, peer.arq,
            PacketFilterArqLevel::on_request,
            result.arq)) {
        return {.error = Error::invalid_state};
    }
    if (result.columns < 2U) {
        return {.error = Error::invalid_state};
    }
    if (!valid_fec_geometry(
            result.columns, result.rows)) {
        return {.error = Error::invalid_state};
    }
    result.columns_specified = true;
    result.rows_specified = true;
    result.layout_specified = true;
    result.arq_specified = true;
    if (!format_effective(result)) {
        return {.error = Error::invalid_state};
    }
    return {.configuration = result};
}

} // namespace

Error encode_fec_control_header(
    const FecControlHeader& header,
    std::span<std::byte> destination) noexcept
{
    if (header.group_index < -1
        || header.flags_recovery > 3U) {
        return Error::invalid_control_payload;
    }
    if (destination.size() < fec_filter_header_size) {
        return Error::buffer_too_small;
    }
    destination[0] = static_cast<std::byte>(
        static_cast<std::uint8_t>(
            header.group_index));
    destination[1] = static_cast<std::byte>(
        header.flags_recovery);
    destination[2] = static_cast<std::byte>(
        (header.length_recovery >> 8U) & 0xffU);
    destination[3] = static_cast<std::byte>(
        header.length_recovery & 0xffU);
    return Error::none;
}

FecControlDecodeResult decode_fec_control_payload(
    std::span<const std::byte> payload) noexcept
{
    if (payload.size() < fec_filter_header_size) {
        return {.error = Error::invalid_control_payload};
    }
    const auto raw_index =
        std::to_integer<std::uint8_t>(payload[0]);
    if ((raw_index > 127U && raw_index != 0xffU)
        || std::to_integer<std::uint8_t>(
            payload[1]) > 3U) {
        return {.error = Error::invalid_control_payload};
    }
    const auto group_index = raw_index == 0xffU
        ? std::int8_t{-1}
        : static_cast<std::int8_t>(raw_index);
    return {
        .header = {
            .group_index = group_index,
            .flags_recovery =
                std::to_integer<std::uint8_t>(
                    payload[1]),
            .length_recovery =
                static_cast<std::uint16_t>(
                    (std::to_integer<std::uint16_t>(
                         payload[2])
                        << 8U)
                    | std::to_integer<std::uint16_t>(
                        payload[3])),
        },
        .payload_recovery =
            payload.subspan(fec_filter_header_size),
    };
}

PacketFilterConfigurationResult
parse_packet_filter_configuration(
    std::string_view text) noexcept
{
    PacketFilterConfiguration configuration;
    if (text.empty()) {
        return {.configuration = configuration};
    }
    if (!store_text(configuration, text)) {
        return {.error = Error::invalid_state};
    }

    const std::size_t first_comma = text.find(',');
    const std::string_view type = text.substr(
        0, first_comma);
    if (type != "fec") {
        return {.error = Error::unsupported};
    }
    configuration.enabled = true;
    if (first_comma == std::string_view::npos) {
        return {.configuration = configuration};
    }

    std::size_t offset = first_comma + 1U;
    while (offset < text.size()) {
        const std::size_t comma = text.find(',', offset);
        const std::size_t end =
            comma == std::string_view::npos
            ? text.size() : comma;
        const std::string_view item =
            text.substr(offset, end - offset);
        const std::size_t colon = item.find(':');
        if (item.empty()
            || colon == std::string_view::npos
            || colon == 0U
            || colon + 1U == item.size()) {
            return {.error = Error::invalid_state};
        }
        const std::string_view key =
            item.substr(0, colon);
        const std::string_view value =
            item.substr(colon + 1U);
        if (key == "cols") {
            if (configuration.columns_specified
                || !parse_integer(
                    value, configuration.columns)
                || configuration.columns < 2U
                || configuration.columns
                    > static_cast<std::uint32_t>(
                        std::numeric_limits<
                            std::int32_t>::max())) {
                return {.error = Error::invalid_state};
            }
            configuration.columns_specified = true;
        } else if (key == "rows") {
            if (configuration.rows_specified
                || !parse_integer(value, configuration.rows)
                || (configuration.rows < 1
                    && configuration.rows > -2)) {
                return {.error = Error::invalid_state};
            }
            configuration.rows_specified = true;
        } else if (key == "layout") {
            if (configuration.layout_specified
                || (value != "even"
                    && value != "staircase")) {
                return {.error = Error::invalid_state};
            }
            configuration.layout =
                value == "even"
                ? PacketFilterLayout::even
                : PacketFilterLayout::staircase;
            configuration.layout_specified = true;
        } else if (key == "arq") {
            if (configuration.arq_specified) {
                return {.error = Error::invalid_state};
            }
            if (value == "never") {
                configuration.arq =
                    PacketFilterArqLevel::never;
            } else if (value == "onreq") {
                configuration.arq =
                    PacketFilterArqLevel::on_request;
            } else if (value == "always") {
                configuration.arq =
                    PacketFilterArqLevel::always;
            } else {
                return {.error = Error::invalid_state};
            }
            configuration.arq_specified = true;
        } else {
            return {.error = Error::invalid_state};
        }
        if (comma == std::string_view::npos) {
            break;
        }
        offset = comma + 1U;
        if (offset == text.size()) {
            return {.error = Error::invalid_state};
        }
    }
    if (configuration.columns_specified
        && configuration.rows_specified
        && !valid_fec_geometry(
            configuration.columns,
            configuration.rows)) {
        return {.error = Error::invalid_state};
    }
    return {.configuration = configuration};
}

PacketFilterConfigurationResult
negotiate_packet_filter_configuration(
    const PacketFilterConfiguration& local,
    const PacketFilterConfiguration& peer,
    bool rendezvous) noexcept
{
    if (!local.enabled && !peer.enabled) {
        return {};
    }
    if (rendezvous && local.enabled && peer.enabled
        && local.view() != peer.view()) {
        return {.error = Error::invalid_state};
    }

    auto resolved = resolve_effective_configuration(
        local, peer, false);
    if (!resolved) {
        return resolved;
    }
    // Rendezvous peers compare the exact configured text. Preserve it on the
    // wire after expanding the effective fields locally, otherwise the
    // responder's canonicalized reply would reject a matching non-canonical
    // request.
    if (rendezvous) {
        const std::string_view wire_text =
            local.enabled ? local.view() : peer.view();
        if (!store_text(
                resolved.configuration,
                wire_text)) {
            return {.error = Error::invalid_state};
        }
    }
    return resolved;
}

PacketFilterConfigurationResult
apply_packet_filter_response(
    const PacketFilterConfiguration& local,
    const PacketFilterConfiguration& response) noexcept
{
    if (!response.enabled) {
        return {.error = Error::invalid_state};
    }
    return resolve_effective_configuration(
        local, response, true);
}

} // namespace robotweax::srt
