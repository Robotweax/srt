// SPDX-License-Identifier: MIT
// Cost of the per-poll send buffer probes that every session evaluates:
// peek_new_packet() (behind has_pending_send_work()) and
// new_packet_flow_count(), across window sizes and buffer states.
#include "robotweax/srt/send_buffer.hpp"

#include <array>
#include <chrono>
#include <cstdio>

int main()
{
    using namespace robotweax::srt;
    using Clock = std::chrono::steady_clock;
    const std::array<std::byte, 1'316> payload {};
    std::uint64_t checksum = 0;
    constexpr std::size_t iterations = 200'000;
    for (const std::size_t window : {64U, 1'024U, 8'192U}) {
        for (const char* state : {"all-sent", "half-sent", "empty"}) {
            SendBuffer buffer {SequenceNumber {0}, window, payload.size()};
            const bool fill = state[0] != 'e';
            if (fill)
                for (std::size_t i = 0; i < window; ++i)
                    (void)buffer.enqueue_message(payload,
                        static_cast<std::uint32_t>(i + 1), PacketTimestamp {0},
                        1);
            const std::size_t to_send = state[0] == 'a' ? window : window / 2;
            for (std::size_t i = 0; fill && i < to_send; ++i)
                if (const auto packet = buffer.next_packet(false, false, 10))
                    buffer.note_packet_submitted(packet->header.sequence);
            const auto begin = Clock::now();
            for (std::size_t i = 0; i < iterations; ++i) {
                checksum += buffer.peek_new_packet().has_value();
                checksum += buffer.new_packet_flow_count();
            }
            const auto elapsed = Clock::now() - begin;
            std::printf("{\"window\":%zu,\"state\":\"%s\",\"probe_pair_"
                        "nanoseconds\":%.1f}\n",
                window, state,
                std::chrono::duration<double, std::nano>(elapsed).count()
                    / iterations);
        }
    }
    return checksum == 0 ? 1 : 0;
}
