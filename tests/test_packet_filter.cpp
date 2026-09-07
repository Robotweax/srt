#include "test.hpp"

#include "robotweax/srt/packet_filter.hpp"

#include <string>
#include <string_view>

using namespace robotweax::srt;

TEST(packet_filter_parser_accepts_the_documented_fec_parameters)
{
    const auto parsed = parse_packet_filter_configuration(
        "fec,cols:10,rows:-5,layout:even,arq:always");
    REQUIRE(parsed);
    REQUIRE(parsed.configuration.enabled);
    REQUIRE_EQ(parsed.configuration.columns, 10U);
    REQUIRE_EQ(parsed.configuration.rows, -5);
    REQUIRE_EQ(parsed.configuration.layout,
        PacketFilterLayout::even);
    REQUIRE_EQ(parsed.configuration.arq,
        PacketFilterArqLevel::always);
    REQUIRE(parsed.configuration.columns_specified);
    REQUIRE(parsed.configuration.rows_specified);
    REQUIRE(parsed.configuration.layout_specified);
    REQUIRE(parsed.configuration.arq_specified);
}

TEST(fec_control_header_has_a_bounded_network_order_codec)
{
    std::array<std::byte, 7> payload{
        std::byte{0}, std::byte{0},
        std::byte{0}, std::byte{0},
        std::byte{'f'}, std::byte{'e'},
        std::byte{'c'}};
    REQUIRE_EQ(encode_fec_control_header({
            .group_index = -1,
            .flags_recovery = 2U,
            .length_recovery = 0x1234U,
        }, payload),
        Error::none);
    REQUIRE_EQ(payload[0], std::byte{0xff});
    REQUIRE_EQ(payload[1], std::byte{2});
    REQUIRE_EQ(payload[2], std::byte{0x12});
    REQUIRE_EQ(payload[3], std::byte{0x34});

    const auto decoded =
        decode_fec_control_payload(payload);
    REQUIRE(decoded);
    REQUIRE_EQ(decoded.header.group_index, -1);
    REQUIRE_EQ(decoded.header.flags_recovery, 2U);
    REQUIRE_EQ(decoded.header.length_recovery,
        0x1234U);
    REQUIRE_EQ(decoded.payload_recovery.size(), 3U);
    REQUIRE_EQ(decoded.payload_recovery[0],
        std::byte{'f'});

    REQUIRE_EQ(decode_fec_control_payload(
            std::span{payload}.first(3)).error,
        Error::invalid_control_payload);
    payload[0] = std::byte{0xfe};
    REQUIRE_EQ(decode_fec_control_payload(payload).error,
        Error::invalid_control_payload);
    payload[0] = std::byte{0xff};
    payload[1] = std::byte{4};
    REQUIRE_EQ(decode_fec_control_payload(payload).error,
        Error::invalid_control_payload);
}

TEST(packet_filter_parser_is_strict_and_bounded)
{
    const auto disabled =
        parse_packet_filter_configuration({});
    REQUIRE(disabled);
    REQUIRE(!disabled.configuration.enabled);

    REQUIRE(!parse_packet_filter_configuration(
        "FEC,cols:10"));
    REQUIRE(!parse_packet_filter_configuration(
        "fec,cols:1"));
    REQUIRE(!parse_packet_filter_configuration(
        "fec,rows:0"));
    REQUIRE(!parse_packet_filter_configuration(
        "fec,rows:-1"));
    REQUIRE(!parse_packet_filter_configuration(
        "fec,cols:129,rows:-2"));
    REQUIRE(!parse_packet_filter_configuration(
        "fec,cols:129,rows:2"));
    REQUIRE(!parse_packet_filter_configuration(
        "fec,cols:2,rows:536870912"));
    REQUIRE(!parse_packet_filter_configuration(
        "fec,cols:2,rows:-2147483648"));
    REQUIRE(!parse_packet_filter_configuration(
        "fec,cols:10,cols:11"));
    REQUIRE(!parse_packet_filter_configuration(
        "fec,unknown:value"));
    REQUIRE(!parse_packet_filter_configuration(
        "fec,cols:10,"));
    REQUIRE(!parse_packet_filter_configuration(
        std::string(
            maximum_packet_filter_configuration_size
                + 1U,
            'x')));
}

TEST(packet_filter_negotiation_merges_parameters_and_defaults)
{
    const auto local =
        parse_packet_filter_configuration(
            "fec,cols:10");
    const auto peer =
        parse_packet_filter_configuration(
            "fec,rows:5,layout:even");
    REQUIRE(local);
    REQUIRE(peer);

    const auto negotiated =
        negotiate_packet_filter_configuration(
            local.configuration,
            peer.configuration);
    REQUIRE(negotiated);
    REQUIRE_EQ(negotiated.configuration.columns, 10U);
    REQUIRE_EQ(negotiated.configuration.rows, 5);
    REQUIRE_EQ(negotiated.configuration.layout,
        PacketFilterLayout::even);
    REQUIRE_EQ(negotiated.configuration.arq,
        PacketFilterArqLevel::on_request);
    REQUIRE_EQ(negotiated.configuration.view(),
        std::string_view{
            "fec,arq:onreq,cols:10,layout:even,rows:5"});
}

TEST(packet_filter_negotiation_rejects_missing_or_conflicting_columns)
{
    const auto no_columns =
        parse_packet_filter_configuration(
            "fec,rows:5");
    const auto ten_columns =
        parse_packet_filter_configuration(
            "fec,cols:10");
    const auto eleven_columns =
        parse_packet_filter_configuration(
            "fec,cols:11");
    REQUIRE(no_columns);
    REQUIRE(ten_columns);
    REQUIRE(eleven_columns);
    REQUIRE(!negotiate_packet_filter_configuration(
        no_columns.configuration,
        PacketFilterConfiguration{}));
    REQUIRE(!negotiate_packet_filter_configuration(
        ten_columns.configuration,
        eleven_columns.configuration));
}

TEST(packet_filter_rendezvous_preserves_exact_matching_text)
{
    const auto left =
        parse_packet_filter_configuration(
            "fec,cols:10,arq:onreq");
    const auto same =
        parse_packet_filter_configuration(
            "fec,cols:10,arq:onreq");
    const auto reordered =
        parse_packet_filter_configuration(
            "fec,arq:onreq,cols:10");
    REQUIRE(left);
    REQUIRE(same);
    REQUIRE(reordered);

    const auto negotiated =
        negotiate_packet_filter_configuration(
            left.configuration,
            same.configuration,
            true);
    REQUIRE(negotiated);
    REQUIRE_EQ(negotiated.configuration.view(),
        left.configuration.view());
    REQUIRE(!negotiate_packet_filter_configuration(
        left.configuration,
        reordered.configuration,
        true));
}

TEST(packet_filter_response_parameters_override_the_caller)
{
    const auto caller =
        parse_packet_filter_configuration(
            "fec,cols:10,rows:5,arq:always");
    const auto response =
        parse_packet_filter_configuration(
            "fec,cols:11,arq:onreq");
    REQUIRE(caller);
    REQUIRE(response);
    const auto applied = apply_packet_filter_response(
        caller.configuration,
        response.configuration);
    REQUIRE(applied);
    REQUIRE_EQ(applied.configuration.columns, 11U);
    REQUIRE_EQ(applied.configuration.rows, 5);
    REQUIRE_EQ(applied.configuration.arq,
        PacketFilterArqLevel::on_request);
}

TEST(packet_filter_policy_keeps_arq_safe_until_reconstruction_exists)
{
    const auto on_request =
        parse_packet_filter_configuration(
            "fec,cols:10,arq:onreq");
    REQUIRE(on_request);
    PacketFilterPolicy fallback{
        on_request.configuration, false};
    REQUIRE_EQ(fallback.configured_arq_level(),
        PacketFilterArqLevel::on_request);
    REQUIRE_EQ(fallback.effective_arq_level(),
        PacketFilterArqLevel::always);
    REQUIRE(fallback.report_detected_loss_immediately());

    PacketFilterPolicy with_reconstruction{
        on_request.configuration, true};
    REQUIRE_EQ(with_reconstruction.effective_arq_level(),
        PacketFilterArqLevel::on_request);
    REQUIRE(!with_reconstruction
        .report_detected_loss_immediately());

    const auto never =
        parse_packet_filter_configuration(
            "fec,cols:10,arq:never");
    REQUIRE(never);
    PacketFilterPolicy explicit_never{
        never.configuration, false};
    REQUIRE_EQ(explicit_never.effective_arq_level(),
        PacketFilterArqLevel::never);
    REQUIRE(!explicit_never
        .report_detected_loss_immediately());
}

TEST(packet_filter_policy_identifies_reserved_fec_control_packets)
{
    const auto parsed =
        parse_packet_filter_configuration(
            "fec,cols:10");
    REQUIRE(parsed);
    PacketFilterPolicy policy{parsed.configuration};

    const std::array<std::byte, 4> fec_payload{
        std::byte{0xff}, std::byte{0},
        std::byte{0}, std::byte{1}};
    PacketView control;
    control.kind = PacketKind::data;
    control.data.message_number = 0U;
    control.payload = fec_payload;
    REQUIRE_EQ(policy.inspect(control),
        PacketFilterReceiveDisposition::
            consume_filter_control);

    control.data.message_number = 1U;
    REQUIRE_EQ(policy.inspect(control),
        PacketFilterReceiveDisposition::pass_through);

    PacketFilterPolicy disabled;
    control.data.message_number = 0U;
    REQUIRE_EQ(disabled.inspect(control),
        PacketFilterReceiveDisposition::pass_through);

    control.payload = {};
    REQUIRE_EQ(policy.inspect(control),
        PacketFilterReceiveDisposition::
            invalid_filter_control);
}

TEST(packet_filter_policy_accepts_column_control_in_column_only_mode)
{
    const auto parsed =
        parse_packet_filter_configuration(
            "fec,cols:4,rows:-2");
    REQUIRE(parsed);
    PacketFilterPolicy policy{parsed.configuration};
    std::array<std::byte, 4> payload{};
    REQUIRE_EQ(encode_fec_control_header({
                   .group_index = 2,
               },
                   payload),
        Error::none);
    const PacketView packet{
        .kind = PacketKind::data,
        .data = {
            .message_number = 0,
            .boundary = MessageBoundary::solo,
        },
        .payload = payload,
    };
    REQUIRE_EQ(policy.inspect(packet),
        PacketFilterReceiveDisposition::
            consume_filter_control);
}
