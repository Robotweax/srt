/* SPDX-License-Identifier: MIT */
#include "common.hpp"
#include "media.hpp"

namespace {
using namespace samples;
struct Path {
    sockaddr_in source {}, destination {};
    int weight = 1;
};

std::vector<SRT_SOCKGROUPDATA> members(SRTSOCKET group)
{
    // This sample admits at most eight paths. A fixed buffer also handles
    // membership changes between size discovery and retrieval.
    std::vector<SRT_SOCKGROUPDATA> result(8);
    std::size_t size = result.size();
    if (srt_group_data(group, result.data(), &size) == SRT_ERROR)
        fail("group data");
    result.resize(size);
    return result;
}

void paths(SRTSOCKET group)
{
    for (const auto& member : members(group)) {
        event("\"event\":\"path\",\"member\":" + std::to_string(member.id)
            + ",\"token\":" + std::to_string(member.token)
            + ",\"weight\":" + std::to_string(member.weight)
            + ",\"socket_state\":" + std::to_string(member.sockstate)
            + ",\"member_state\":" + std::to_string(member.memberstate));
        if (member.sockstate == SRTS_CONNECTED)
            stats(member.id, "path");
    }
}

Path parse_path(const std::string& value)
{
    const auto a = value.find(','), b = value.rfind(',');
    if (a == std::string::npos || a == b)
        throw std::runtime_error(
            "path format: sourceIPv4:port,destinationIPv4:port,weight");
    return {address(value.substr(0, a), true),
        address(value.substr(a + 1, b - a - 1)),
        number(std::string_view(value).substr(b + 1), 1, 65535)};
}

SRTSOCKET add_path(SRTSOCKET group, const Path& path, int token, int timeout)
{
    struct Config {
        SRT_SOCKOPT_CONFIG* value = srt_create_config();
        ~Config()
        {
            if (value)
                srt_delete_config(value);
        }
    } config;
    if (!config.value)
        fail("create endpoint config");
    if (srt_config_add(config.value, SRTO_CONNTIMEO, &timeout, sizeof(timeout))
        == SRT_ERROR)
        fail("endpoint timeout");
    auto endpoint =
        srt_prepare_endpoint(reinterpret_cast<const sockaddr*>(&path.source),
            reinterpret_cast<const sockaddr*>(&path.destination),
            sizeof(path.source));
    if (endpoint.errorcode != SRT_SUCCESS)
        throw std::runtime_error("invalid group endpoint");
    endpoint.weight = static_cast<std::uint16_t>(path.weight);
    endpoint.token = token;
    endpoint.config = config.value;
    if (srt_connect_group(group, &endpoint, 1) == SRT_ERROR
        || endpoint.errorcode != SRT_SUCCESS)
        fail("connect group path");
    return endpoint.id;
}

void ready_paths(SRTSOCKET group, int count, int timeout)
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout);
    while (Clock::now() < deadline) {
        const auto state = members(group);
        if (static_cast<int>(state.size()) == count
            && std::all_of(state.begin(), state.end(), [](const auto& m) {
                   return m.sockstate == SRTS_CONNECTED;
               }))
            return;
        std::this_thread::sleep_for(10ms);
    }
    throw std::runtime_error("group path setup deadline exceeded");
}
} // namespace

int main(int argc, char** argv)
{
    using namespace samples;
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--help") {
            std::cout << "contribution receive --endpoint IPv4:port\n"
                         "contribution send --policy backup|broadcast\n"
                         "  --path sourceIPv4:port,destinationIPv4:port,weight "
                         "(repeat 2..8 times)\n"
                         "  --fail-after N --restore-after N (message indexes; "
                         "disabled by default)\n"
                         "  --media-udp IPv4:port --duration SECONDS [--record "
                         "OUTPUT.ts]\n"
                         "Media mode forwards MPEG-TS datagrams; default "
                         "traffic is synthetic.\n"
                      << common_help;
            return 0;
        }
        Arguments args(argc, argv,
            std::string(common_options)
                + " --endpoint  --policy  --path  --fail-after  "
                  "--restore-after  --media-udp  --duration  --record ");
        if (args.role != "send" && args.role != "receive")
            throw std::runtime_error("role must be send or receive");
        const Settings settings(args);
        const bool media = !args.get("--media-udp").empty();
        if (!media
            && (!args.get("--record").empty()
                || !args.get("--duration").empty()))
            throw std::runtime_error(
                "--record and --duration require --media-udp");
        const auto policy = args.get("--policy", "backup");
        if (policy != "backup" && policy != "broadcast")
            throw std::runtime_error("policy must be backup or broadcast");
        const int fail_after = args.integer(
            "--fail-after", 0, 0, media ? 1000000 : settings.count - 1);
        const int restore_after = args.integer(
            "--restore-after", 0, 0, media ? 1000000 : settings.count - 1);
        if (restore_after && (!fail_after || restore_after <= fail_after))
            throw std::runtime_error(
                "restore-after must follow a nonzero fail-after");
        Runtime runtime;
        if (args.role == "receive") {
            auto server = listener(args.get("--endpoint", "0.0.0.0:9000"),
                settings, SRTT_LIVE, "contribution", true);
            auto group = accept(server.id, settings);
            if (args.get("--media-udp").empty())
                receive_stream(group.id, settings, "contribution");
            else
                media_stream(group.id, args, false, {}, [&] {
                    paths(group.id);
                });
            paths(group.id);
        } else {
            const auto found = args.values.find("--path");
            if (found == args.values.end() || found->second.size() < 2
                || found->second.size() > 8)
                throw std::runtime_error("configure 2..8 explicit paths");
            std::vector<Path> endpoints;
            for (const auto& value : found->second)
                endpoints.push_back(parse_path(value));
            Socket group(srt_create_group(
                policy == "backup" ? SRT_GTYPE_BACKUP : SRT_GTYPE_BROADCAST));
            configure(group.id, settings, SRTT_LIVE, true);
            std::vector<SRTSOCKET> ids;
            for (std::size_t i = 0; i < endpoints.size(); ++i)
                ids.push_back(add_path(group.id, endpoints[i],
                    static_cast<int>(i + 1), settings.timeout_ms));
            ready_paths(
                group.id, static_cast<int>(ids.size()), settings.timeout_ms);
            auto next_report = Clock::now();
            int failed_index = -1;
            const auto on_frame = [&](int index) {
                if (fail_after && index == fail_after) {
                    auto state = members(group.id);
                    auto active = std::find_if(
                        state.begin(), state.end(), [](const auto& m) {
                            return m.memberstate == SRT_GST_RUNNING;
                        });
                    if (active == state.end())
                        throw std::runtime_error("no running member to close");
                    const auto it =
                        std::find(ids.begin(), ids.end(), active->id);
                    if (it == ids.end())
                        throw std::runtime_error("unknown active member");
                    failed_index = static_cast<int>(it - ids.begin());
                    if (srt_close(active->id) == SRT_ERROR)
                        fail("close path");
                    event("\"event\":\"path_closed\",\"index\":"
                        + std::to_string(index)
                        + ",\"token\":" + std::to_string(failed_index + 1));
                }
                if (restore_after && index == restore_after) {
                    ids[failed_index] =
                        add_path(group.id, endpoints[failed_index],
                            failed_index + 1, settings.timeout_ms);
                    event("\"event\":\"path_rejoining\",\"index\":"
                        + std::to_string(index)
                        + ",\"token\":" + std::to_string(failed_index + 1));
                }
                if (Clock::now() >= next_report || index == fail_after
                    || index == restore_after) {
                    paths(group.id);
                    next_report = Clock::now() + 1s;
                }
            };
            if (args.get("--media-udp").empty())
                send_stream(
                    group.id, settings, "contribution", false, false, on_frame);
            else
                media_stream(group.id, args, true, on_frame);
            paths(group.id);
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "contribution: " << e.what() << '\n';
        return 1;
    }
}
