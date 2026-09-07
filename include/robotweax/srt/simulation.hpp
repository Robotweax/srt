#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace robotweax::srt::simulation {

using Endpoint = std::uint32_t;
using Time = std::uint64_t;

struct NetworkConfiguration {
    Time base_delay_microseconds = 1'000;
    Time jitter_microseconds = 0;
    std::uint32_t loss_per_million = 0;
    std::uint32_t duplicate_per_million = 0;
    std::uint64_t seed = 1;
};

struct Datagram {
    Endpoint source = 0;
    Endpoint destination = 0;
    Time delivery_time = 0;
    std::vector<std::byte> bytes;
};

class Network {
public:
    explicit Network(NetworkConfiguration configuration);

    [[nodiscard]] Time now() const noexcept { return now_; }
    void send(Endpoint source, Endpoint destination, std::span<const std::byte> bytes);
    [[nodiscard]] bool empty() const noexcept { return queue_.empty(); }
    [[nodiscard]] Datagram deliver_next();

private:
    [[nodiscard]] std::uint32_t random_million() noexcept;
    [[nodiscard]] Time delivery_delay() noexcept;
    void enqueue(Datagram datagram);

    NetworkConfiguration configuration_;
    Time now_ = 0;
    std::uint64_t random_state_ = 1;
    std::vector<Datagram> queue_;
};

} // namespace robotweax::srt::simulation
