/* SPDX-License-Identifier: MIT */
// Raw MPEG-TS over the public multicast dispatch group API. FFmpeg or a
// hardware encoder supplies UDP input; a player consumes the UDP output.
#include "common.hpp"
#include "media.hpp"

namespace {
using namespace samples;

bool multicast_address(const sockaddr_in& endpoint)
{
    return (ntohl(endpoint.sin_addr.s_addr) & 0xf0000000U) == 0xe0000000U;
}

sockaddr_in unicast_address(const std::string& text)
{
    const auto result = address(text);
    const auto value = ntohl(result.sin_addr.s_addr);
    if (value == 0 || value >= 0xe0000000U)
        throw std::runtime_error("a numeric unicast IPv4 endpoint is required");
    return result;
}

// One deliberately fixed stream and domain. A real application must supply
// its own admission policy; this callback admits every dispatch proposal.
int admit(void*, SRTSOCKET socket, int, const sockaddr*, const char*)
{
    std::int32_t type = 0;
    int size = sizeof(type);
    if (srt_getsockflag(socket, SRTO_GROUPTYPE, &type, &size) == SRT_ERROR
        || type != ROBOTWEAX_SRT_GTYPE_MULTICAST)
        return -1;
    const std::uint64_t domain = 1;
    const char key[] = "live-mpegts";
    if (srt_setsockflag(
            socket, SRTO_ROBOTWEAX_GROUPDOMAIN, &domain, sizeof(domain))
        == SRT_ERROR)
        return -1;
    return srt_setsockflag(socket, SRTO_ROBOTWEAX_MULTICAST, key,
        static_cast<int>(sizeof(key) - 1));
}

void multicast_stats(SRTSOCKET group)
{
    ROBOTWEAX_SRT_MCAST_STATS_V1 stats {};
    int size = sizeof(stats);
    if (srt_getsockflag(group, SRTO_ROBOTWEAX_MCAST_STATS, &stats, &size)
        == SRT_ERROR)
        fail("multicast statistics");
    event("\"event\":\"multicast_stats\",\"members\":"
        + std::to_string(stats.member_count)
        + ",\"multicast_packets_sent\":" + std::to_string(stats.mcast_pkt_sent)
        + ",\"multicast_bytes_sent\":" + std::to_string(stats.mcast_byte_sent)
        + ",\"multicast_repeats\":" + std::to_string(stats.mcast_pkt_rexmit)
        + ",\"unicast_repeats_skipped\":"
        + std::to_string(stats.unicast_rexmit_skipped)
        + ",\"multicast_packets_received\":"
        + std::to_string(stats.mcast_pkt_received)
        + ",\"multicast_packets_filtered\":"
        + std::to_string(stats.mcast_pkt_filtered));
}

void wait_for_receivers(SRTSOCKET group, int expected, const Settings& settings)
{
    const auto deadline =
        Clock::now() + std::chrono::milliseconds(settings.timeout_ms);
    while (!media_stopping && Clock::now() < deadline) {
        // Bound the sample's startup snapshot; this is not a supported fan-out
        // size for the library. It need only establish the requested minimum.
        std::array<SRT_SOCKGROUPDATA, 64> members {};
        std::size_t count = members.size();
        if (srt_group_data(group, members.data(), &count) == SRT_ERROR)
            fail("group membership");
        int ready = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (members[i].sockstate != SRTS_CONNECTED)
                continue;
            if (!settings.secret_environment.empty()) {
                std::int32_t state = 0;
                int size = sizeof(state);
                if (srt_getsockflag(
                        members[i].id, SRTO_SNDKMSTATE, &state, &size)
                        == SRT_ERROR
                    || state != SRT_KM_S_SECURED)
                    continue;
            }
            ++ready;
        }
        if (ready >= expected)
            return;
        std::this_thread::sleep_for(10ms);
    }
    if (!media_stopping)
        throw std::runtime_error(
            "receiver or group-key readiness deadline exceeded");
}

void send_media(const Arguments& args, const Settings& settings,
    const sockaddr_in& interface)
{
    if (!args.get("--record").empty())
        throw std::runtime_error("--record is receive-only");
    const auto local =
        unicast_address(args.get("--endpoint", "127.0.0.1:9000"));
    if (local.sin_addr.s_addr != interface.sin_addr.s_addr)
        throw std::runtime_error(
            "sender --endpoint must bind the --interface address");
    const auto multicast =
        address(args.get("--multicast", "239.255.133.11:10000"));
    if (!multicast_address(multicast))
        throw std::runtime_error(
            "--multicast must be an IPv4 multicast address and port");
    if (local.sin_port == multicast.sin_port)
        throw std::runtime_error("control and multicast ports must differ");
    const auto ttl = args.integer("--ttl", 1, 0, 255);
    const auto minimum = args.integer("--min-receivers", 1, 1, 16);
    const auto threshold = args.integer("--repeat-threshold", 0, 0, 16);

    Socket listener(srt_create_socket());
    configure(listener.id, settings, SRTT_LIVE);
    option(listener.id, SRTO_SENDER, true);
    option(listener.id, SRTO_ROBOTWEAX_MCAST_GROUP, multicast);
    option(listener.id, SRTO_ROBOTWEAX_MCAST_IFACE, interface);
    option(listener.id, SRTO_ROBOTWEAX_MCAST_TTL, ttl);
    option(listener.id, SRTO_RCVSYN, false);
    if (srt_listen_callback(listener.id, admit, nullptr) == SRT_ERROR)
        fail("listen callback");
    if (srt_bind(listener.id, reinterpret_cast<const sockaddr*>(&local),
            sizeof(local))
        == SRT_ERROR)
        fail("bind dispatch listener");
    if (srt_listen(listener.id, 16) == SRT_ERROR)
        fail("listen");
    event("\"event\":\"ready\",\"channel\":\"multicast_sender\"");
    auto group = accept(listener.id, settings);
    option(group.id, SRTO_SNDTIMEO, 250);
    option(group.id, SRTO_ROBOTWEAX_MCAST_REXMIT_THRESHOLD, threshold);
    wait_for_receivers(group.id, minimum, settings);
    if (!media_stopping) {
        media_stream(group.id, args, true, {}, [&] {
            multicast_stats(group.id);
        });
        multicast_stats(group.id);
    }
}

void receive_media(const Arguments& args, const Settings& settings,
    const sockaddr_in& interface)
{
    for (const auto* option :
        {"--multicast", "--ttl", "--min-receivers", "--repeat-threshold"})
        if (args.values.contains(option))
            throw std::runtime_error(std::string(option) + " is sender-only");
    const auto remote =
        unicast_address(args.get("--endpoint", "127.0.0.1:9000"));
    Socket group(srt_create_group(ROBOTWEAX_SRT_GTYPE_MULTICAST));
    configure(group.id, settings, SRTT_LIVE, true);
    option(group.id, SRTO_ROBOTWEAX_MCAST_IFACE, interface);
    struct Config {
        SRT_SOCKOPT_CONFIG* value = srt_create_config();
        ~Config()
        {
            if (value)
                srt_delete_config(value);
        }
    } config;
    if (!config.value
        || srt_config_add(config.value, SRTO_CONNTIMEO, &settings.timeout_ms,
               sizeof(settings.timeout_ms))
            == SRT_ERROR)
        fail("endpoint timeout");
    auto endpoint = srt_prepare_endpoint(
        nullptr, reinterpret_cast<const sockaddr*>(&remote), sizeof(remote));
    endpoint.weight = 0;
    endpoint.config = config.value;
    if (srt_connect_group(group.id, &endpoint, 1) == SRT_ERROR)
        fail("connect dispatch receiver");
    media_stream(group.id, args, false, {}, [&] {
        multicast_stats(group.id);
    });
    multicast_stats(group.id);
}
} // namespace

int main(int argc, char** argv)
{
    using namespace samples;
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--help") {
            std::cout
                << "robotweax_srt_multicast send|receive --media-udp "
                   "IPv4:port\n"
                   "  --endpoint IPv4:port    Sender bind / receiver's remote "
                   "(127.0.0.1:9000)\n"
                   "  --interface IPv4        Local multicast interface "
                   "(127.0.0.1)\n"
                   "  --multicast IPv4:port   Sender multicast destination "
                   "(239.255.133.11:10000)\n"
                   "  --ttl N                 Sender hop limit (1; 0 for "
                   "loopback tests)\n"
                   "  --min-receivers N       Sender startup minimum (1; max "
                   "16)\n"
                   "  --repeat-threshold N    Multicast repair threshold (0; "
                   "max 16)\n"
                   "  --duration SECONDS      Media run time (20; 3..3600)\n"
                   "  --record OUTPUT.ts      Receiver recording in addition "
                   "to UDP output\n"
                   "  --latency-ms N          Live latency (120; 20..2000)\n"
                   "  --timeout-ms N          Setup deadline (10000)\n"
                   "  --passphrase-env NAME   Optional AES-256 group "
                   "encryption\n"
                   "  --crypto ctr|gcm        Explicit cipher (ctr); GCM "
                   "requires matching build\n"
                   "Raw MPEG-TS only, 188..1316 bytes per datagram. No "
                   "transcoding or RTP.\n"
                   "Wait for media_ready before starting the source. Ctrl+C "
                   "stops forwarding.\n"
                   "Experimental Robotweax peers only. Shared group keys are "
                   "unreviewed.\n";
            return 0;
        }
        Arguments args(argc, argv,
            " --endpoint  --interface  --multicast  --ttl  --min-receivers "
            " --repeat-threshold  --media-udp  --duration  --record "
            " --latency-ms  --timeout-ms  --passphrase-env  --crypto ");
        if (args.role != "send" && args.role != "receive")
            throw std::runtime_error("role must be send or receive");
        // Validate media configuration before opening a network connection.
        (void)unicast_address(args.get("--media-udp"));
        (void)args.integer("--duration", 20, 3, 3600);
        const auto interface =
            address(args.get("--interface", "127.0.0.1") + ":0", true);
        if (interface.sin_addr.s_addr == 0 || multicast_address(interface)
            || ntohl(interface.sin_addr.s_addr) >= 0xf0000000U)
            throw std::runtime_error(
                "--interface requires a local unicast IPv4 address");
        const Settings settings(args);
        Runtime runtime;
        if (args.role == "send")
            send_media(args, settings, interface);
        else
            receive_media(args, settings, interface);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "multicast sample: " << error.what() << '\n';
        return 1;
    }
}
