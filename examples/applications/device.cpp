/* SPDX-License-Identifier: MIT */
#include "common.hpp"

int main(int argc, char** argv)
{
    using namespace samples;
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--help") {
            std::cout << "device device|operator --host IPv4 --port BASE\n"
                         "Device sends synthetic video on BASE and sensor "
                         "samples on BASE+1;\n"
                         "operator sends commands on BASE+2 and waits for "
                         "execution acknowledgements.\n"
                      << common_help;
            return 0;
        }
        Arguments args(
            argc, argv, std::string(common_options) + " --host  --port ");
        if (args.role != "device" && args.role != "operator")
            throw std::runtime_error("role must be device or operator");
        const Settings settings(args);
        const auto host = args.get("--host", "127.0.0.1");
        const int port = args.integer("--port", 9100, 1, 65533);
        Runtime runtime;
        std::array<std::exception_ptr, 3> errors {};
        std::vector<std::jthread> workers;
        const std::array<SRT_TRANSTYPE, 3> profiles {
            SRTT_LIVE, SRTT_SENSOR, SRTT_CONTROL};
        const std::array<std::string, 3> names {"video", "sensor", "control"};
        for (std::size_t i = 0; i < profiles.size(); ++i) {
            workers.emplace_back([&, i] {
                try {
                    Settings lane = settings;
                    if (i == 2) {
                        lane.count = std::max(1, settings.count / 20);
                        lane.interval_ms =
                            std::min(1000, settings.interval_ms * 20);
                    }
                    const auto endpoint =
                        host + ':' + std::to_string(port + static_cast<int>(i));
                    const bool sensor = i == 1, commands = i == 2;
                    if (args.role == "device") {
                        auto server =
                            listener(endpoint, lane, profiles[i], names[i]);
                        auto peer = accept(server.id, lane);
                        if (commands)
                            receive_stream(
                                peer.id, lane, names[i], false, true);
                        else
                            send_stream(peer.id, lane, names[i], sensor);
                    } else {
                        auto peer = connect(endpoint, lane, profiles[i]);
                        if (commands)
                            send_stream(peer.id, lane, names[i], false, true);
                        else
                            receive_stream(peer.id, lane, names[i], sensor);
                    }
                } catch (...) {
                    errors[i] = std::current_exception();
                }
            });
        }
        for (auto& worker : workers)
            worker.join();
        for (const auto& error : errors)
            if (error)
                std::rethrow_exception(error);
        event("\"event\":\"complete\",\"application\":\"device\"");
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "device: " << e.what() << '\n';
        return 1;
    }
}
