#pragma once

#include "robotweax/srt/error.hpp"
#include "robotweax/srt/packet.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace robotweax::srt {

inline constexpr std::size_t maximum_packet_filter_configuration_size = 512;
inline constexpr std::size_t fec_filter_header_size = 4;

struct FecControlHeader {
    std::int8_t group_index = -1;
    std::uint8_t flags_recovery = 0;
    std::uint16_t length_recovery = 0;
};

struct FecControlDecodeResult {
    Error error = Error::none;
    FecControlHeader header{};
    // Borrows the suffix of the payload supplied to
    // decode_fec_control_payload().
    std::span<const std::byte> payload_recovery{};

    [[nodiscard]] constexpr explicit operator bool() const noexcept
    {
        return error == Error::none;
    }
};

[[nodiscard]] Error encode_fec_control_header(
    const FecControlHeader& header,
    std::span<std::byte> destination) noexcept;

[[nodiscard]] FecControlDecodeResult
decode_fec_control_payload(
    std::span<const std::byte> payload) noexcept;

enum class PacketFilterLayout : std::uint8_t {
    even,
    staircase,
};

enum class PacketFilterArqLevel : std::uint8_t {
    never,
    on_request,
    always,
};

struct PacketFilterConfiguration {
    bool enabled = false;
    std::uint32_t columns = 0;
    std::int32_t rows = 1;
    PacketFilterLayout layout =
        PacketFilterLayout::staircase;
    PacketFilterArqLevel arq =
        PacketFilterArqLevel::on_request;
    bool columns_specified = false;
    bool rows_specified = false;
    bool layout_specified = false;
    bool arq_specified = false;
    std::array<char,
        maximum_packet_filter_configuration_size + 1U>
        text{};
    std::size_t text_size = 0;

    [[nodiscard]] constexpr std::string_view view() const noexcept
    {
        // The returned view aliases this configuration object.
        return {text.data(), text_size};
    }

    [[nodiscard]] constexpr std::size_t extra_header_size() const noexcept
    {
        return enabled ? fec_filter_header_size : 0U;
    }
};

struct PacketFilterConfigurationResult {
    Error error = Error::none;
    PacketFilterConfiguration configuration{};

    [[nodiscard]] constexpr explicit operator bool() const noexcept
    {
        return error == Error::none;
    }
};

[[nodiscard]] PacketFilterConfigurationResult
parse_packet_filter_configuration(
    std::string_view text) noexcept;

[[nodiscard]] PacketFilterConfigurationResult
negotiate_packet_filter_configuration(
    const PacketFilterConfiguration& local,
    const PacketFilterConfiguration& peer,
    bool rendezvous = false) noexcept;

// The listener has already checked compatibility before producing HSRSP.
// On the caller, explicitly returned listener parameters take precedence,
// matching the HSv5 packet-filter negotiation contract.
[[nodiscard]] PacketFilterConfigurationResult
apply_packet_filter_response(
    const PacketFilterConfiguration& local,
    const PacketFilterConfiguration& response) noexcept;

enum class PacketFilterReceiveDisposition : std::uint8_t {
    pass_through,
    consume_filter_control,
    invalid_filter_control,
};

// This is the allocation-free seam used by the reliability path. Until an FEC
// reconstructor is installed, ONREQ deliberately falls back to ALWAYS so loss
// recovery remains reliable; an explicit NEVER policy remains authoritative.
class PacketFilterPolicy {
public:
    explicit constexpr PacketFilterPolicy(
        PacketFilterConfiguration configuration = {},
        bool reconstruction_available = false) noexcept
        : configuration_(configuration)
        , reconstruction_available_(
              reconstruction_available)
    {
    }

    [[nodiscard]] constexpr bool enabled() const noexcept
    {
        return configuration_.enabled;
    }

    [[nodiscard]] constexpr PacketFilterArqLevel
    configured_arq_level() const noexcept
    {
        return configuration_.arq;
    }

    [[nodiscard]] constexpr PacketFilterArqLevel
    effective_arq_level() const noexcept
    {
        if (!configuration_.enabled) {
            return PacketFilterArqLevel::always;
        }
        if (configuration_.arq
                == PacketFilterArqLevel::on_request
            && !reconstruction_available_) {
            return PacketFilterArqLevel::always;
        }
        return configuration_.arq;
    }

    [[nodiscard]] constexpr bool
    report_detected_loss_immediately() const noexcept
    {
        return effective_arq_level()
            == PacketFilterArqLevel::always;
    }

    [[nodiscard]] constexpr PacketFilterReceiveDisposition
    inspect(const PacketView& packet) const noexcept
    {
        if (!configuration_.enabled
            || packet.kind != PacketKind::data
            || packet.data.message_number != 0U) {
            return PacketFilterReceiveDisposition::
                pass_through;
        }
        const auto control =
            decode_fec_control_payload(packet.payload);
        if (!control
            || (control.header.group_index >= 0
                && (configuration_.rows == 1
                    || static_cast<std::uint32_t>(
                        control.header.group_index)
                        >= configuration_.columns))
            || (control.header.group_index == -1
                && configuration_.rows < 0)) {
            return PacketFilterReceiveDisposition::
                invalid_filter_control;
        }
        return PacketFilterReceiveDisposition::
            consume_filter_control;
    }

private:
    PacketFilterConfiguration configuration_{};
    bool reconstruction_available_ = false;
};

} // namespace robotweax::srt
