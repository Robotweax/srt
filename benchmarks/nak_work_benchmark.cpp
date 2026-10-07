#include "robotweax/srt/codec.hpp"
#include "robotweax/srt/session.hpp"

#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <vector>

using namespace robotweax::srt;

int main()
{
    constexpr unsigned repetitions = 20000;
    for (const unsigned capacity : {1024U, 8192U, 65536U}) {
        const SequenceNumber initial {100};
        ReliabilitySession sender {{.local_initial_sequence = initial,
            .send_capacity_packets = capacity,
            .receive_capacity_packets = 16,
            .maximum_payload_size = 1}};
        const std::vector<std::byte> message(capacity);
        if (sender.queue_message(message, PacketTimestamp {0}) != Error::none)
            return EXIT_FAILURE;
        for (unsigned i = 0; i < capacity; ++i)
            if (!sender.next_data_packet())
                return EXIT_FAILURE;
        const ReliabilityAction action {
            .kind = ReliabilityActionKind::loss_report,
            .loss = {initial, initial.advanced(capacity - 1U)}};
        std::array<std::byte, 64> storage {};
        const auto encoded =
            encode_reliability_action(action, PacketTimestamp {0}, 1, storage);
        if (!encoded)
            return EXIT_FAILURE;
        const auto decoded =
            decode_packet(std::span {storage}.first(encoded.bytes_written));
        if (!decoded)
            return EXIT_FAILURE;
        // Fill the retransmission queue before timing repeated compact reports.
        // Each report must still succeed; no retransmissions are drained here.
        for (unsigned i = 0; i < capacity; i += 256U)
            if (!sender.receive(decoded.packet, i + 1U))
                return EXIT_FAILURE;
        const auto started = std::chrono::steady_clock::now();
        std::size_t queued = 0;
        for (unsigned i = 0; i < repetitions; ++i) {
            const auto result =
                sender.receive(decoded.packet, capacity + i + 1U);
            if (!result)
                return EXIT_FAILURE;
            queued += result.sender_loss_packets;
        }
        const auto elapsed = std::chrono::duration<double, std::micro>(
            std::chrono::steady_clock::now() - started)
                                 .count();
        if (queued != 0U)
            return EXIT_FAILURE;
        std::cout << "{\"capacity\":" << capacity
                  << ",\"reports\":" << repetitions
                  << ",\"microseconds_per_report\":" << elapsed / repetitions
                  << "}\n";
    }
}
