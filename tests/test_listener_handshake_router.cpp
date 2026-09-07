#include "test.hpp"

#include "compat/listener_handshake_router.hpp"

using namespace robotweax::srt;
using namespace robotweax::srt::compat;

namespace {

[[nodiscard]] StatelessListenerHandshakeRouter make_router()
{
    constexpr std::array<std::byte, 16> cookie_secret{
        std::byte{0x00},
        std::byte{0x01},
        std::byte{0x02},
        std::byte{0x03},
        std::byte{0x04},
        std::byte{0x05},
        std::byte{0x06},
        std::byte{0x07},
        std::byte{0x08},
        std::byte{0x09},
        std::byte{0x0a},
        std::byte{0x0b},
        std::byte{0x0c},
        std::byte{0x0d},
        std::byte{0x0e},
        std::byte{0x0f},
    };
    return StatelessListenerHandshakeRouter{{
        .listener_socket_id = 900U,
        .maximum_transmission_unit = 1'400U,
        .flow_window = 8'192U,
        .encryption_field = 3U,
        .cookie_secret = cookie_secret,
    }};
}

[[nodiscard]] HandshakeMessage induction(std::uint32_t socket_id = 100U,
                                         bool legacy = false)
{
    HandshakeMessage message;
    message.packet.version = handshake_version_4;
    message.packet.encryption_field = legacy ? 0U : 2U;
    message.packet.extension_field = legacy ? udt_datagram_socket_type : 0U;
    message.packet.initial_sequence = SequenceNumber{1'234U};
    message.packet.maximum_transmission_unit = 1'500U;
    message.packet.flow_window = 25'600U;
    message.packet.request = HandshakeRequest::induction;
    message.packet.socket_id = socket_id;
    return message;
}

[[nodiscard]] HandshakeMessage conclusion_from(const HandshakeMessage& request,
                                               std::uint32_t cookie,
                                               bool legacy = false)
{
    HandshakeMessage result = request;
    result.packet.version = legacy ? handshake_version_4 : handshake_version_5;
    result.packet.encryption_field = legacy ? 0U : 3U;
    result.packet.extension_field = legacy ? udt_datagram_socket_type : 1U;
    result.packet.request = HandshakeRequest::conclusion;
    result.packet.syn_cookie = cookie;
    if (!legacy) {
        result.has_handshake_extension = true;
    }
    return result;
}

[[nodiscard]] HandshakeMessage stream_induction(std::uint32_t socket_id = 100U)
{
    auto message = induction(socket_id, true);
    message.packet.extension_field = udt_stream_socket_type;
    return message;
}

} // namespace

TEST(listener_cookie_siphash_matches_the_standard_vectors)
{
    std::array<std::byte, 16> key{};
    std::array<std::byte, 15> message{};
    for (std::size_t index = 0; index < key.size(); ++index) {
        key[index] = static_cast<std::byte>(index);
    }
    for (std::size_t index = 0; index < message.size(); ++index) {
        message[index] = static_cast<std::byte>(index);
    }

    REQUIRE_EQ(detail::listener_cookie_siphash24(std::span<const std::byte>{}, key),
               0x726f'db47'dd0e'0e31ULL);
    REQUIRE_EQ(detail::listener_cookie_siphash24(message, key),
               0xa129'ca61'49be'45e5ULL);
}

TEST(listener_handshake_router_selects_hsv5_without_connection_state)
{
    constexpr std::uint64_t window = 42U;
    const auto peer = IpEndpoint::loopback(9'000U);
    const auto request = induction();
    const auto routed = make_router().route(request, peer, window);

    REQUIRE_EQ(routed.kind, ListenerHandshakeRouteKind::send_induction_response);
    REQUIRE_EQ(routed.protocol, ListenerHandshakeProtocol::hsv5);
    REQUIRE_EQ(routed.response.packet.version, handshake_version_5);
    REQUIRE_EQ(routed.response.packet.extension_field, handshake_srt_magic);
    REQUIRE_EQ(routed.response.packet.socket_id, 900U);
    REQUIRE_EQ(routed.response.packet.initial_sequence,
               request.packet.initial_sequence);
    REQUIRE_EQ(routed.response.packet.maximum_transmission_unit, 1'400U);
    REQUIRE_EQ(routed.response.packet.flow_window, 8'192U);
    REQUIRE(routed.validated_cookie != 0U);

    const auto accepted = make_router().route(
        conclusion_from(request, routed.validated_cookie), peer, window);
    REQUIRE_EQ(accepted.kind, ListenerHandshakeRouteKind::admit_conclusion);
    REQUIRE_EQ(accepted.protocol, ListenerHandshakeProtocol::hsv5);
    REQUIRE_EQ(accepted.validated_cookie, routed.validated_cookie);
    REQUIRE_EQ(accepted.reconstructed_induction.packet.version, handshake_version_4);
    REQUIRE_EQ(accepted.reconstructed_induction.packet.extension_field, 0U);
    REQUIRE_EQ(accepted.reconstructed_induction.packet.request,
               HandshakeRequest::induction);
    REQUIRE_EQ(accepted.reconstructed_induction.packet.syn_cookie, 0U);
}

TEST(listener_handshake_router_prefers_hsv5_for_ambiguous_udt_dgram_discovery)
{
    constexpr std::uint64_t window = 9U;
    const auto peer = IpEndpoint::ipv6_loopback(9'001U, 7U);
    const auto request = induction(101U, true);
    const auto routed = make_router().route(request, peer, window);

    REQUIRE_EQ(routed.kind, ListenerHandshakeRouteKind::send_induction_response);
    REQUIRE_EQ(routed.protocol, ListenerHandshakeProtocol::hsv5);
    REQUIRE_EQ(routed.response.packet.version, handshake_version_5);
    REQUIRE_EQ(routed.response.packet.encryption_field, 3U);
    REQUIRE_EQ(routed.response.packet.extension_field, handshake_srt_magic);
    REQUIRE_EQ(routed.response.packet.initial_sequence,
               request.packet.initial_sequence);
    REQUIRE_EQ(routed.response.packet.maximum_transmission_unit,
               1'400U);
    REQUIRE_EQ(routed.response.packet.flow_window, 8'192U);

    // The response stays HSv5, as required by deployed Listener behaviour.
    // Its cookie remains sufficient to authenticate a subsequent legacy
    // conclusion, but that conclusion is rejected without admission.
    const auto legacy_rejected = make_router().route(
        conclusion_from(request, routed.validated_cookie, true), peer, window);
    REQUIRE_EQ(legacy_rejected.kind,
        ListenerHandshakeRouteKind::send_induction_response);
    REQUIRE_EQ(
        legacy_rejected.protocol, ListenerHandshakeProtocol::unsupported_hsv4);
    REQUIRE_EQ(legacy_rejected.response.packet.version, handshake_version_5);
    REQUIRE_EQ(
        static_cast<std::int32_t>(legacy_rejected.response.packet.request),
        1'008);
    REQUIRE_EQ(
        legacy_rejected.response.packet.syn_cookie, routed.validated_cookie);

    const auto modern_accepted = make_router().route(
        conclusion_from(request, routed.validated_cookie), peer, window);
    REQUIRE_EQ(modern_accepted.kind,
               ListenerHandshakeRouteKind::admit_conclusion);
    REQUIRE_EQ(modern_accepted.protocol, ListenerHandshakeProtocol::hsv5);
}

TEST(listener_handshake_router_drops_unambiguous_udt_stream_without_state)
{
    constexpr std::uint64_t window = 10U;
    const auto peer = IpEndpoint::loopback(9'012U);
    const auto request = stream_induction(102U);
    const auto routed = make_router().route(request, peer, window);

    REQUIRE_EQ(routed.kind, ListenerHandshakeRouteKind::ignore);
    REQUIRE_EQ(routed.protocol, ListenerHandshakeProtocol::invalid);
}

TEST(listener_handshake_router_cookie_is_bounded_and_context_bound)
{
    constexpr std::uint64_t window = 100U;
    const auto peer = IpEndpoint::loopback(9'002U);
    const auto request = induction();
    const auto response = make_router().route(request, peer, window);
    const auto conclusion = conclusion_from(request, response.validated_cookie);

    REQUIRE_EQ(make_router().route(conclusion, peer, window + 1U).kind,
               ListenerHandshakeRouteKind::admit_conclusion);
    REQUIRE_EQ(make_router().route(conclusion, peer, window + 2U).kind,
               ListenerHandshakeRouteKind::ignore);
    REQUIRE_EQ(
        make_router().route(conclusion, IpEndpoint::loopback(9'003U), window).kind,
        ListenerHandshakeRouteKind::ignore);

    auto changed_socket = conclusion;
    ++changed_socket.packet.socket_id;
    REQUIRE_EQ(make_router().route(changed_socket, peer, window).kind,
               ListenerHandshakeRouteKind::ignore);
    auto changed_sequence = conclusion;
    changed_sequence.packet.initial_sequence = SequenceNumber{1'235U};
    REQUIRE_EQ(make_router().route(changed_sequence, peer, window).kind,
               ListenerHandshakeRouteKind::ignore);

    auto changed_mtu = conclusion;
    changed_mtu.packet.maximum_transmission_unit = 1'400U;
    REQUIRE_EQ(make_router().route(changed_mtu, peer, window).kind,
               ListenerHandshakeRouteKind::ignore);
    auto changed_window = conclusion;
    changed_window.packet.flow_window = 8'192U;
    REQUIRE_EQ(make_router().route(changed_window, peer, window).kind,
               ListenerHandshakeRouteKind::ignore);
}

TEST(listener_handshake_router_separates_protocol_cookie_domains)
{
    constexpr std::uint64_t window = 7U;
    const auto peer = IpEndpoint::loopback(9'004U);
    const auto modern = induction();
    const auto modern_response = make_router().route(modern, peer, window);
    const auto legacy = induction(100U, true);
    REQUIRE_EQ(
        make_router()
            .route(conclusion_from(legacy,
                                   modern_response.validated_cookie,
                                   true),
                   peer,
                   window)
            .kind,
        ListenerHandshakeRouteKind::ignore);
}

TEST(listener_handshake_router_handles_interleaved_candidates_statelessly)
{
    constexpr std::uint64_t window = 11U;
    const auto first_peer = IpEndpoint::loopback(9'010U);
    const auto second_peer = IpEndpoint::loopback(9'011U);
    const auto first_request = induction(110U);
    const auto second_request = induction(111U);
    const auto first_response = make_router().route(first_request, first_peer, window);
    const auto second_response
        = make_router().route(second_request, second_peer, window);

    REQUIRE_EQ(
        make_router()
            .route(conclusion_from(first_request, first_response.validated_cookie),
                   first_peer,
                   window)
            .kind,
        ListenerHandshakeRouteKind::admit_conclusion);
    REQUIRE_EQ(
        make_router()
            .route(conclusion_from(second_request, second_response.validated_cookie),
                   second_peer,
                   window)
            .kind,
        ListenerHandshakeRouteKind::admit_conclusion);
}

TEST(listener_handshake_router_rejects_malformed_or_extended_discovery)
{
    const auto peer = IpEndpoint::loopback(9'005U);
    auto malformed = induction();
    malformed.packet.extension_field = 99U;
    REQUIRE_EQ(make_router().route(malformed, peer, 1U).kind,
               ListenerHandshakeRouteKind::ignore);

    malformed = induction();
    malformed.has_unknown_extension = true;
    REQUIRE_EQ(make_router().route(malformed, peer, 1U).kind,
               ListenerHandshakeRouteKind::ignore);

    malformed = induction(100U, true);
    malformed.packet.encryption_field = 1U;
    REQUIRE_EQ(make_router().route(malformed, peer, 1U).kind,
               ListenerHandshakeRouteKind::ignore);

    malformed = induction();
    malformed.packet.syn_cookie = 1U;
    REQUIRE_EQ(make_router().route(malformed, peer, 1U).kind,
               ListenerHandshakeRouteKind::ignore);

    malformed = induction();
    malformed.packet.maximum_transmission_unit = 75U;
    REQUIRE_EQ(make_router().route(malformed, peer, 1U).kind,
               ListenerHandshakeRouteKind::ignore);

    malformed = induction();
    malformed.packet.flow_window = 0U;
    REQUIRE_EQ(make_router().route(malformed, peer, 1U).kind,
               ListenerHandshakeRouteKind::ignore);
}

TEST(caller_induction_response_classifier_is_fail_closed)
{
    auto response = induction();
    response.packet.version = handshake_version_5;
    response.packet.extension_field = handshake_srt_magic;
    response.packet.syn_cookie = 1U;
    response.packet.socket_id = 200U;
    REQUIRE_EQ(classify_caller_induction_response(response),
               ListenerHandshakeProtocol::hsv5);

    response.has_unknown_extension = true;
    REQUIRE_EQ(classify_caller_induction_response(response),
               ListenerHandshakeProtocol::invalid);

    response = induction(200U, true);
    response.packet.syn_cookie = 1U;
    REQUIRE_EQ(classify_caller_induction_response(response),
        ListenerHandshakeProtocol::unsupported_hsv4);

    response.has_unknown_extension = true;
    REQUIRE_EQ(classify_caller_induction_response(response),
               ListenerHandshakeProtocol::invalid);

    response = induction(200U, true);
    response.packet.extension_field = 0U;
    response.packet.syn_cookie = 1U;
    REQUIRE_EQ(classify_caller_induction_response(response),
        ListenerHandshakeProtocol::unsupported_hsv4);

    response.has_unknown_extension = true;
    REQUIRE_EQ(classify_caller_induction_response(response),
               ListenerHandshakeProtocol::invalid);

    response.has_unknown_extension = false;
    response.packet.extension_field = udt_stream_socket_type;
    REQUIRE_EQ(classify_caller_induction_response(response),
        ListenerHandshakeProtocol::unsupported_hsv4);

    response.packet.extension_field = 0U;
    response.packet.encryption_field = 1U;
    REQUIRE_EQ(classify_caller_induction_response(response),
               ListenerHandshakeProtocol::invalid);

    response = induction();
    response.packet.version = handshake_version_5;
    response.packet.extension_field = 0U;
    response.packet.syn_cookie = 1U;
    REQUIRE_EQ(classify_caller_induction_response(response),
               ListenerHandshakeProtocol::invalid);
}
