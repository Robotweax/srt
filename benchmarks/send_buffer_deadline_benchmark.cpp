// Measure the deadline query added for receive-wait-safe sender polling.
// Construction and enqueue are outside the timed interval. A single packet
// with a TTL at the tail exposes the cost of scanning a large buffered span.
#include "robotweax/srt/send_buffer.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>

int main()
{
    using namespace robotweax::srt;
    using Clock = std::chrono::steady_clock;
    const std::array<std::byte, 188> payload {};
    std::uint64_t checksum = 0;

    for (const auto window : {64U, 256U, 8'192U}) {
        for (const bool ttl : {false, true}) {
            SendBuffer buffer {SequenceNumber {0}, window, payload.size()};
            for (unsigned index = 0; index < window; ++index) {
                const auto expiration = ttl && index + 1U == window
                    ? std::uint64_t {1'000'000}
                    : std::uint64_t {0};
                if (buffer.enqueue_message(payload, index, PacketTimestamp {0},
                        1, true, 1, expiration)
                    != Error::none) {
                    return 1;
                }
            }

            std::array<double, 6> samples {};
            for (std::size_t sample = 0; sample <= samples.size(); ++sample) {
                const auto start = Clock::now();
                for (unsigned call = 0; call < 20'000; ++call) {
                    checksum +=
                        buffer.next_expiration_microseconds().value_or(1);
                }
                if (sample != 0U) {
                    samples[sample - 1U] =
                        std::chrono::duration<double, std::nano>(
                            Clock::now() - start)
                            .count()
                        / 20'000;
                }
            }
            std::sort(samples.begin(), samples.end());
            std::cout << "window_packets=" << window << " ttl=" << ttl
                      << " median_ns_per_query=" << samples[samples.size() / 2]
                      << '\n';
        }
    }
    std::cout << "checksum=" << checksum << '\n';
}
