// Isolate original-packet selection with a full unacknowledged window.
// Construction, enqueue, and ACK processing are outside the timed interval.
#include "robotweax/srt/send_buffer.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>

int main()
{
    using namespace robotweax::srt;
    using Clock = std::chrono::steady_clock;
    const std::array<std::byte, 1'316> payload {};
    std::uint64_t checksum = 0;
    for (const std::size_t window : {64U, 256U, 1'024U, 4'096U, 8'192U}) {
        SendBuffer buffer {SequenceNumber {0}, window, payload.size()};
        std::array<double, 9> samples {};
        // Reuse storage for 16 windows per sample; the first sample warms it.
        for (std::size_t sample = 0; sample <= samples.size(); ++sample) {
            double elapsed_nanoseconds = 0;
            for (std::size_t repeat = 0; repeat < 16U; ++repeat) {
                const auto first = buffer.next_sequence();
                for (std::size_t index = 0; index < window; ++index) {
                    if (buffer.enqueue_message(
                            payload, 1, PacketTimestamp {0}, 1)
                        != Error::none) {
                        return 1;
                    }
                }
                const auto start = Clock::now();
                for (std::size_t index = 0; index < window; ++index) {
                    const auto packet = buffer.next_packet();
                    if (!packet.has_value()) {
                        return 2;
                    }
                    checksum += packet->header.sequence.value();
                }
                elapsed_nanoseconds += std::chrono::duration<double, std::nano>(
                    Clock::now() - start)
                                           .count();
                if (buffer.next_packet().has_value()
                    || buffer.acknowledge_before(
                           first.advanced(static_cast<std::uint32_t>(window)))
                        != Error::none) {
                    return 3;
                }
            }
            if (sample != 0U) {
                samples[sample - 1U] = elapsed_nanoseconds / (16U * window);
            }
        }
        std::sort(samples.begin(), samples.end());
        std::cout << "window_packets=" << window
                  << " median_ns_per_packet=" << samples[samples.size() / 2U]
                  << " min_ns_per_packet=" << samples.front()
                  << " max_ns_per_packet=" << samples.back() << '\n';
    }
    std::cout << "checksum=" << checksum << '\n';
}
