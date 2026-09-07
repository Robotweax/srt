#include "compat/listener_handshake_router.hpp"

#include "robotweax/srt/crypto_provider.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <span>

namespace robotweax::srt::compat {
namespace {

[[nodiscard]] constexpr bool
has_integrated_extensions(const HandshakeMessage& message) noexcept
{
    return message.has_handshake_extension || message.has_key_material_extension
           || message.has_stream_id_extension || message.has_congestion_extension
           || message.has_packet_filter_extension || message.has_group_membership
           || message.has_unknown_extension;
}

[[nodiscard]] std::uint64_t load_u64_le(const std::byte* source) noexcept
{
    std::uint64_t result = 0;
    for (unsigned index = 0; index < 8U; ++index) {
        result
            |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(source[index]))
               << (index * 8U);
    }
    return result;
}

void sip_round(std::uint64_t& v0,
               std::uint64_t& v1,
               std::uint64_t& v2,
               std::uint64_t& v3) noexcept
{
    v0 += v1;
    v1 = std::rotl(v1, 13);
    v1 ^= v0;
    v0 = std::rotl(v0, 32);
    v2 += v3;
    v3 = std::rotl(v3, 16);
    v3 ^= v2;
    v0 += v3;
    v3 = std::rotl(v3, 21);
    v3 ^= v0;
    v2 += v1;
    v1 = std::rotl(v1, 17);
    v1 ^= v2;
    v2 = std::rotl(v2, 32);
}

} // namespace

std::uint64_t
detail::listener_cookie_siphash24(std::span<const std::byte> message,
                                  const std::array<std::byte, 16>& key) noexcept
{
    const std::uint64_t k0 = load_u64_le(key.data());
    const std::uint64_t k1 = load_u64_le(key.data() + 8U);
    std::uint64_t v0 = 0x736f'6d65'7073'6575ULL ^ k0;
    std::uint64_t v1 = 0x646f'7261'6e64'6f6dULL ^ k1;
    std::uint64_t v2 = 0x6c79'6765'6e65'7261ULL ^ k0;
    std::uint64_t v3 = 0x7465'6462'7974'6573ULL ^ k1;

    std::size_t offset = 0;
    while (message.size() - offset >= 8U) {
        const std::uint64_t word = load_u64_le(message.data() + offset);
        v3 ^= word;
        sip_round(v0, v1, v2, v3);
        sip_round(v0, v1, v2, v3);
        v0 ^= word;
        offset += 8U;
    }

    std::uint64_t tail = static_cast<std::uint64_t>(message.size()) << 56U;
    for (std::size_t index = 0; offset + index < message.size(); ++index) {
        tail |= static_cast<std::uint64_t>(
                    std::to_integer<std::uint8_t>(message[offset + index]))
                << (index * 8U);
    }
    v3 ^= tail;
    sip_round(v0, v1, v2, v3);
    sip_round(v0, v1, v2, v3);
    v0 ^= tail;
    v2 ^= 0xffU;
    sip_round(v0, v1, v2, v3);
    sip_round(v0, v1, v2, v3);
    sip_round(v0, v1, v2, v3);
    sip_round(v0, v1, v2, v3);
    return v0 ^ v1 ^ v2 ^ v3;
}

namespace {

template <std::size_t Size>
void append_u32_le(std::array<std::byte, Size>& message,
                   std::size_t& offset,
                   std::uint32_t value) noexcept
{
    for (unsigned index = 0; index < 4U; ++index) {
        message[offset++] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
    }
}

template <std::size_t Size>
void append_u64_le(std::array<std::byte, Size>& message,
                   std::size_t& offset,
                   std::uint64_t value) noexcept
{
    for (unsigned index = 0; index < 8U; ++index) {
        message[offset++] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
    }
}

[[nodiscard]] constexpr ListenerHandshakeProtocol
classify_induction_request(const HandshakeMessage& message) noexcept
{
    const Handshake& packet = message.packet;
    if (packet.request != HandshakeRequest::induction
        || packet.version != handshake_version_4 || packet.socket_id == 0U
        || packet.syn_cookie != 0U || packet.maximum_transmission_unit < 76U
        || packet.flow_window == 0U || has_integrated_extensions(message)) {
        return ListenerHandshakeProtocol::invalid;
    }
    if (packet.extension_field == 0U
        || (packet.extension_field == udt_datagram_socket_type
            && packet.encryption_field == 0U)) {
        // Haivision HSv5 and genuine HSv4 callers both deploy the latter
        // Version-4/UDT_DGRAM discovery shape. The generation therefore remains
        // deliberately unresolved until the structurally distinct conclusion.
        return ListenerHandshakeProtocol::hsv5;
    }
    if (packet.extension_field == udt_stream_socket_type
        && packet.encryption_field == 0U) {
        return ListenerHandshakeProtocol::unsupported_hsv4;
    }
    return ListenerHandshakeProtocol::invalid;
}

[[nodiscard]] constexpr ListenerHandshakeProtocol
classify_conclusion_request(const HandshakeMessage& message) noexcept
{
    const Handshake& packet = message.packet;
    if (packet.request != HandshakeRequest::conclusion || packet.socket_id == 0U
        || packet.syn_cookie == 0U || packet.maximum_transmission_unit < 76U
        || packet.flow_window == 0U) {
        return ListenerHandshakeProtocol::invalid;
    }
    if (packet.version == handshake_version_5) {
        return ListenerHandshakeProtocol::hsv5;
    }
    if (packet.version == handshake_version_4 && packet.encryption_field == 0U
        && (packet.extension_field == udt_stream_socket_type
            || packet.extension_field == udt_datagram_socket_type)
        && !has_integrated_extensions(message)) {
        return ListenerHandshakeProtocol::unsupported_hsv4;
    }
    return ListenerHandshakeProtocol::invalid;
}

} // namespace

StatelessListenerHandshakeRouter::StatelessListenerHandshakeRouter(
    Configuration configuration) noexcept
    : configuration_(configuration)
{
    default_crypto_provider().secure_erase(configuration.cookie_secret);
}

StatelessListenerHandshakeRouter::~StatelessListenerHandshakeRouter()
{
    default_crypto_provider().secure_erase(configuration_.cookie_secret);
}

ListenerHandshakeRoute
StatelessListenerHandshakeRouter::route(const HandshakeMessage& message,
                                        IpEndpoint peer,
                                        std::uint64_t time_window) const noexcept
{
    if (configuration_.listener_socket_id == 0U
        || configuration_.maximum_transmission_unit < 76U
        || configuration_.flow_window == 0U
        || std::none_of(
            configuration_.cookie_secret.begin(),
            configuration_.cookie_secret.end(),
            [](std::byte value) { return value != std::byte{0}; })) {
        return {};
    }

    if (message.packet.request == HandshakeRequest::induction) {
        const auto protocol = classify_induction_request(message);
        return protocol == ListenerHandshakeProtocol::invalid
                   ? ListenerHandshakeRoute{}
                   : route_induction(message, peer, time_window, protocol);
    }
    if (message.packet.request == HandshakeRequest::conclusion) {
        const auto protocol = classify_conclusion_request(message);
        return protocol == ListenerHandshakeProtocol::invalid
                   ? ListenerHandshakeRoute{}
                   : route_conclusion(message, peer, time_window, protocol);
    }
    return {};
}

ListenerHandshakeRoute StatelessListenerHandshakeRouter::route_induction(
    const HandshakeMessage& message,
    IpEndpoint peer,
    std::uint64_t time_window,
    ListenerHandshakeProtocol protocol) const noexcept
{
    const Handshake& incoming = message.packet;
    if (protocol == ListenerHandshakeProtocol::unsupported_hsv4) {
        // A UDT_STREAM induction is unambiguously legacy but carries no
        // authenticated routing token. Dropping it is the bounded, stateless
        // response permitted by the 0.2 downgrade contract.
        return {};
    }
    Handshake response;
    response.version = handshake_version_5;
    response.encryption_field = configuration_.encryption_field != 0U
        ? configuration_.encryption_field
        : incoming.encryption_field;
    response.extension_field = handshake_srt_magic;
    response.initial_sequence = incoming.initial_sequence;
    response.maximum_transmission_unit = configuration_.maximum_transmission_unit;
    response.flow_window = configuration_.flow_window;
    response.request = HandshakeRequest::induction;
    response.socket_id = configuration_.listener_socket_id;
    const CookieDomain cookie_domain =
        incoming.extension_field == udt_datagram_socket_type
            && incoming.encryption_field == 0U
        ? CookieDomain::ambiguous
        : CookieDomain::hsv5;
    response.syn_cookie = cookie(incoming, peer, time_window, cookie_domain);

    return {
        .kind = ListenerHandshakeRouteKind::send_induction_response,
        .protocol = protocol,
        .response =
            {
                .kind = HandshakeActionKind::send,
                .packet = response,
            },
        .validated_cookie = response.syn_cookie,
    };
}

ListenerHandshakeRoute StatelessListenerHandshakeRouter::route_conclusion(
    const HandshakeMessage& message,
    IpEndpoint peer,
    std::uint64_t time_window,
    ListenerHandshakeProtocol protocol) const noexcept
{
    const bool valid_cookie =
        protocol == ListenerHandshakeProtocol::unsupported_hsv4
        ? message.packet.extension_field == udt_datagram_socket_type
            && matches_cookie(
                message.packet, peer, time_window, CookieDomain::ambiguous)
        : matches_cookie(message.packet, peer, time_window, CookieDomain::hsv5)
            || matches_cookie(
                message.packet, peer, time_window, CookieDomain::ambiguous);
    if (!valid_cookie) {
        return {};
    }

    if (protocol == ListenerHandshakeProtocol::unsupported_hsv4) {
        Handshake rejection;
        rejection.version = handshake_version_5;
        rejection.initial_sequence = message.packet.initial_sequence;
        rejection.maximum_transmission_unit =
            configuration_.maximum_transmission_unit;
        rejection.flow_window = configuration_.flow_window;
        rejection.request = static_cast<HandshakeRequest>(1'008);
        rejection.socket_id = configuration_.listener_socket_id;
        rejection.syn_cookie = message.packet.syn_cookie;
        return {
            .kind = ListenerHandshakeRouteKind::send_induction_response,
            .protocol = protocol,
            .response =
                {
                    .kind = HandshakeActionKind::send,
                    .packet = rejection,
                },
            .validated_cookie = message.packet.syn_cookie,
        };
    }

    HandshakeMessage induction;
    induction.packet = message.packet;
    induction.packet.version = handshake_version_4;
    induction.packet.encryption_field = 0U;
    induction.packet.extension_field = 0U;
    induction.packet.request = HandshakeRequest::induction;
    induction.packet.syn_cookie = 0U;

    return {
        .kind = ListenerHandshakeRouteKind::admit_conclusion,
        .protocol = protocol,
        .reconstructed_induction = induction,
        .validated_cookie = message.packet.syn_cookie,
    };
}

std::uint32_t StatelessListenerHandshakeRouter::cookie(
    const Handshake& packet,
    IpEndpoint peer,
    std::uint64_t time_window,
    CookieDomain domain) const noexcept
{
    constexpr std::uint32_t hsv5_domain = 0x4853'5635U;
    constexpr std::uint32_t ambiguous_domain = 0x5544'5444U;
    const std::uint32_t protocol_domain =
        domain == CookieDomain::ambiguous ? ambiguous_domain : hsv5_domain;
    std::array<std::byte, 51> message{};
    std::size_t offset = 0;
    append_u32_le(message, offset, protocol_domain);
    append_u64_le(message, offset, time_window);
    append_u32_le(message, offset, packet.socket_id);
    append_u32_le(message, offset, packet.initial_sequence.value());
    append_u32_le(message, offset, packet.maximum_transmission_unit);
    append_u32_le(message, offset, packet.flow_window);
    message[offset++] = static_cast<std::byte>(peer.family);
    for (std::uint8_t byte : peer.address) {
        message[offset++] = static_cast<std::byte>(byte);
    }
    message[offset++] = static_cast<std::byte>(peer.port & 0xffU);
    message[offset++] = static_cast<std::byte>(peer.port >> 8U);
    append_u32_le(message, offset, peer.scope_id);

    const std::uint32_t result = static_cast<std::uint32_t>(
        detail::listener_cookie_siphash24(message, configuration_.cookie_secret));
    return result == 0U ? 1U : result;
}

bool StatelessListenerHandshakeRouter::matches_cookie(
    const Handshake& packet,
    IpEndpoint peer,
    std::uint64_t time_window,
    CookieDomain domain) const noexcept
{
    const std::uint32_t current = cookie(packet, peer, time_window, domain);
    if (packet.syn_cookie == current) {
        return true;
    }
    return time_window != 0U
           && packet.syn_cookie == cookie(packet, peer, time_window - 1U, domain);
}

ListenerHandshakeProtocol
classify_caller_induction_response(const HandshakeMessage& message) noexcept
{
    const Handshake& packet = message.packet;
    if (packet.request != HandshakeRequest::induction || packet.socket_id == 0U
        || packet.syn_cookie == 0U) {
        return ListenerHandshakeProtocol::invalid;
    }
    if (packet.version == handshake_version_5
        && packet.extension_field == handshake_srt_magic
        && !has_integrated_extensions(message)) {
        return ListenerHandshakeProtocol::hsv5;
    }
    if (packet.version == handshake_version_4 && packet.encryption_field == 0U
        && (packet.extension_field == 0U
            || packet.extension_field == udt_stream_socket_type
            || packet.extension_field == udt_datagram_socket_type)
        && !has_integrated_extensions(message)) {
        // Historical Haivision SRT releases answer the shared Version-4
        // discovery with a zero socket-type field. The response is still
        // unambiguously HSv4: HSv5 responses use Version 5 and the SRT magic.
        // Keep this exception local to caller-side discovery; Version-4
        // conclusions remain exact configured UDT_STREAM or UDT_DGRAM packets.
        return ListenerHandshakeProtocol::unsupported_hsv4;
    }
    return ListenerHandshakeProtocol::invalid;
}

} // namespace robotweax::srt::compat
