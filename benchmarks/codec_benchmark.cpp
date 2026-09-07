#include "robotweax/srt/codec.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>

int main()
{
    constexpr std::size_t iterations = 20'000'000;
    std::array<std::byte, 1'332> packet{};
    packet[3] = std::byte{1};
    packet[4] = std::byte{0xc0};
    packet[15] = std::byte{7};

    std::uint64_t checksum = 0;
    const auto started = std::chrono::steady_clock::now();
    for (std::size_t index = 0; index < iterations; ++index) {
        const auto decoded = robotweax::srt::decode_packet(packet);
        if (!decoded) {
            return 1;
        }
        checksum += decoded.packet.data.sequence.value();
        checksum += decoded.packet.payload.size();
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const double seconds = std::chrono::duration<double>(elapsed).count();
    std::cout << "decoded " << iterations << " packets in " << seconds << " s\n"
              << static_cast<double>(iterations) / seconds / 1'000'000.0
              << " million packets/s; checksum=" << checksum << '\n';
}
