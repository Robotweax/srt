/* SPDX-License-Identifier: MIT */
#include "common.hpp"

int main(int argc, char** argv)
{
    using namespace samples;
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--help") {
            std::cout
                << "recovery send|receive --endpoint IPv4:port [--filter FEC]\n"
                << common_help;
            return 0;
        }
        Arguments args(
            argc, argv, std::string(common_options) + " --endpoint  --filter ");
        if (args.role != "send" && args.role != "receive")
            throw std::runtime_error("role must be send or receive");
        Settings settings(args);
        Runtime runtime;
        const auto endpoint = args.get("--endpoint", "127.0.0.1:9000");
        if (args.role == "receive") {
            auto server = listener(endpoint, settings, SRTT_LIVE, "recovery");
            auto peer = accept(server.id, settings);
            receive_stream(peer.id, settings, "recovery");
        } else {
            auto peer = connect(endpoint, settings, SRTT_LIVE);
            send_stream(peer.id, settings, "recovery");
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "recovery: " << e.what() << '\n';
        return 1;
    }
}
