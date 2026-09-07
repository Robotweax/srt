#include "test.hpp"

#include "robotweax/srt/handshake_extensions.hpp"

#include "hsv5_version_policy.hpp"

#include <array>
#include <cstddef>
#include <string_view>

using namespace robotweax::srt;

TEST(hsv5_peer_version_contract_checks_protocol_coherence_before_policy)
{
    REQUIRE_EQ(minimum_hsv5_srt_version, 0x0001'0300U);
    REQUIRE_EQ(detail::classify_hsv5_peer_version(0x0001'02ffU, 0x0001'0000U),
        detail::Hsv5PeerVersionStatus::pre_hsv5);
    REQUIRE_EQ(detail::classify_hsv5_peer_version(0x0001'02ffU, 0x0001'0500U),
        detail::Hsv5PeerVersionStatus::pre_hsv5);
    REQUIRE_EQ(detail::classify_hsv5_peer_version(0x0001'0300U, 0x0001'0000U),
        detail::Hsv5PeerVersionStatus::compatible);
    REQUIRE_EQ(detail::classify_hsv5_peer_version(0x0001'04ffU, 0x0001'0500U),
        detail::Hsv5PeerVersionStatus::below_configured_minimum);
    REQUIRE_EQ(detail::classify_hsv5_peer_version(0x0001'0500U, 0x0001'0500U),
        detail::Hsv5PeerVersionStatus::compatible);
}

TEST(live_options_are_negotiated_per_direction_and_latency_uses_maximum)
{
    HandshakeExtensionParameters local;
    local.receiver_tsbpd_delay_milliseconds = 300;
    local.sender_tsbpd_delay_milliseconds = 80;
    HandshakeExtensionParameters peer;
    peer.receiver_tsbpd_delay_milliseconds = 200;
    peer.sender_tsbpd_delay_milliseconds = 150;

    const auto negotiated = negotiate_live_options(local, peer);
    REQUIRE(negotiated.send_tsbpd);
    REQUIRE(negotiated.receive_tsbpd);
    REQUIRE(negotiated.too_late_packet_drop);
    REQUIRE(negotiated.periodic_nak);
    REQUIRE_EQ(negotiated.receive_delay_milliseconds, 300U);
    REQUIRE_EQ(negotiated.peer_receive_delay_milliseconds, 200U);

    peer.flags &= ~static_cast<std::uint32_t>(HandshakeExtensionFlag::tsbpd_receive);
    const auto one_way = negotiate_live_options(local, peer);
    REQUIRE(!one_way.send_tsbpd);
    REQUIRE(one_way.receive_tsbpd);
    REQUIRE(!one_way.too_late_packet_drop);
}

TEST(handshake_extension_parameters_have_exact_wire_layout)
{
    HandshakeExtensionParameters parameters;
    parameters.srt_version = 0x0001'0505U;
    parameters.flags = 0x0000'003fU;
    parameters.receiver_tsbpd_delay_milliseconds = 120;
    parameters.sender_tsbpd_delay_milliseconds = 240;

    std::array<std::byte, 16> bytes{};
    const auto encoded = encode_handshake_parameters(
        HandshakeExtensionType::handshake_request, parameters, bytes);
    REQUIRE(encoded);
    REQUIRE_EQ(encoded.bytes_written, bytes.size());
    REQUIRE_EQ(bytes[0], std::byte{0});
    REQUIRE_EQ(bytes[1], std::byte{1});
    REQUIRE_EQ(bytes[2], std::byte{0});
    REQUIRE_EQ(bytes[3], std::byte{3});
    REQUIRE_EQ(bytes[12], std::byte{0});
    REQUIRE_EQ(bytes[13], std::byte{120});
    REQUIRE_EQ(bytes[14], std::byte{0});
    REQUIRE_EQ(bytes[15], std::byte{240});

    const auto extension = decode_extension(bytes);
    REQUIRE(extension);
    REQUIRE_EQ(extension.bytes_consumed, bytes.size());
    const auto decoded = decode_handshake_parameters(extension.extension);
    REQUIRE(decoded);
    REQUIRE_EQ(decoded.parameters.srt_version, parameters.srt_version);
    REQUIRE_EQ(decoded.parameters.flags, parameters.flags);
    REQUIRE_EQ(decoded.parameters.receiver_tsbpd_delay_milliseconds, 120U);
    REQUIRE_EQ(decoded.parameters.sender_tsbpd_delay_milliseconds, 240U);
}

TEST(extension_decoder_walks_chained_records_without_allocation)
{
    std::array<std::byte, 32> bytes{};
    const HandshakeExtensionParameters parameters{};
    const auto first = encode_handshake_parameters(
        HandshakeExtensionType::handshake_request, parameters, bytes);
    REQUIRE(first);
    const auto second = encode_stream_id("live/test", std::span{bytes}.subspan(first.bytes_written));
    REQUIRE(second);

    const auto decoded_first = decode_extension(bytes);
    REQUIRE(decoded_first);
    const auto decoded_second = decode_extension(
        std::span{bytes}.subspan(decoded_first.bytes_consumed));
    REQUIRE(decoded_second);
    REQUIRE_EQ(decoded_second.extension.type, HandshakeExtensionType::stream_id);
    REQUIRE_EQ(decoded_second.extension.content[0], std::byte{'e'});
    REQUIRE_EQ(decoded_second.extension.content[11], std::byte{'t'});
}

TEST(stream_id_is_padded_to_complete_words)
{
    std::array<std::byte, 16> bytes{std::byte{0xff}};
    const auto encoded = encode_stream_id("abcde", bytes);
    REQUIRE(encoded);
    REQUIRE_EQ(encoded.bytes_written, 12U);
    REQUIRE_EQ(bytes[2], std::byte{0});
    REQUIRE_EQ(bytes[3], std::byte{2});
    REQUIRE_EQ(bytes[4], std::byte{'d'});
    REQUIRE_EQ(bytes[7], std::byte{'a'});
    REQUIRE_EQ(bytes[8], std::byte{0});
    REQUIRE_EQ(bytes[11], std::byte{'e'});
}

TEST(packet_filter_text_uses_the_hsv5_word_swapped_layout)
{
    constexpr std::string_view configuration =
        "fec,cols:10,arq:onreq";
    std::array<std::byte, 32> bytes{std::byte{0xff}};
    const auto encoded = encode_extension_text(
        HandshakeExtensionType::packet_filter,
        configuration, bytes);
    REQUIRE(encoded);
    REQUIRE_EQ(encoded.bytes_written, 28U);
    REQUIRE_EQ(bytes[0], std::byte{0});
    REQUIRE_EQ(bytes[1], std::byte{7});
    REQUIRE_EQ(bytes[2], std::byte{0});
    REQUIRE_EQ(bytes[3], std::byte{6});
    REQUIRE_EQ(bytes[4], std::byte{','});
    REQUIRE_EQ(bytes[7], std::byte{'f'});

    const auto extension = decode_extension(
        std::span{bytes}.first(encoded.bytes_written));
    REQUIRE(extension);
    const auto decoded =
        decode_extension_text(extension.extension);
    REQUIRE(decoded);
    REQUIRE_EQ(decoded.view(), configuration);
}

TEST(extension_decoder_rejects_truncated_content)
{
    const std::array<std::byte, 7> bytes{
        std::byte{0}, std::byte{1}, std::byte{0}, std::byte{1},
        std::byte{0}, std::byte{0}, std::byte{0},
    };
    REQUIRE_EQ(decode_extension(bytes).error, Error::invalid_extension);
}
