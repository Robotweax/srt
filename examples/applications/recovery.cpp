/* SPDX-License-Identifier: MIT */
#include "common.hpp"

int main(int argc, char** argv)
{
    using namespace samples;
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--help") {
            std::cout
                << "recovery send|receive --endpoint IPv4:port [--filter FEC]\n"
                << srt_demo::arq_first_help << common_help;
            return 0;
        }
        Arguments args(argc, argv,
            std::string(common_options) + " --endpoint  --filter  --arq-first ",
            " --arq-first ");
        if (args.role != "send" && args.role != "receive")
            throw std::runtime_error("role must be send or receive");
        Settings settings(args);
        if (args.role != "send" && settings.arq_first)
            throw std::runtime_error("--arq-first is a sender option");
        Runtime runtime;
        const auto endpoint = args.get("--endpoint", "127.0.0.1:9000");
        if (args.role == "receive") {
            auto server = listener(endpoint, settings, SRTT_LIVE, "recovery");
            auto peer = accept(server.id, settings);
            receive_stream(peer.id, settings, "recovery");
        } else {
            auto peer = connect(endpoint, settings, SRTT_LIVE);
            std::int32_t policy = -1;
            bool probe = false;
            int size = sizeof(policy);
            if (srt_getsockflag(
                    peer.id, SRTO_ROBOTWEAX_FECPOLICY, &policy, &size)
                == SRT_ERROR)
                fail("read FEC policy");
            size = sizeof(probe);
            if (srt_getsockflag(
                    peer.id, SRTO_ROBOTWEAX_TAILPROBE, &probe, &size)
                == SRT_ERROR)
                fail("read tail probe option");
            event("\"event\":\"recovery_mode\",\"fec_policy\":"
                + std::to_string(policy)
                + ",\"tail_probe\":" + (probe ? "true" : "false"));
            send_stream(peer.id, settings, "recovery");
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "recovery: " << e.what() << '\n';
        return 1;
    }
}
