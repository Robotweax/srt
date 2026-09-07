#include "robotweax/srt/simulation.hpp"

#include <algorithm>
#include <stdexcept>

namespace robotweax::srt::simulation {

Network::Network(NetworkConfiguration configuration)
    : configuration_(configuration), random_state_(configuration.seed == 0 ? 1 : configuration.seed)
{
    if (configuration.loss_per_million > 1'000'000U
        || configuration.duplicate_per_million > 1'000'000U) {
        throw std::invalid_argument("probabilities must be in parts per million");
    }
}

std::uint32_t Network::random_million() noexcept
{
    random_state_ ^= random_state_ << 13U;
    random_state_ ^= random_state_ >> 7U;
    random_state_ ^= random_state_ << 17U;
    return static_cast<std::uint32_t>(random_state_ % 1'000'000U);
}

Time Network::delivery_delay() noexcept
{
    if (configuration_.jitter_microseconds == 0U) {
        return configuration_.base_delay_microseconds;
    }
    const Time width = configuration_.jitter_microseconds * 2U + 1U;
    const Time sample = random_state_ % width;
    (void)random_million();
    if (sample <= configuration_.jitter_microseconds) {
        const Time reduction = configuration_.jitter_microseconds - sample;
        return reduction > configuration_.base_delay_microseconds
            ? 0U
            : configuration_.base_delay_microseconds - reduction;
    }
    return configuration_.base_delay_microseconds
        + (sample - configuration_.jitter_microseconds);
}

void Network::enqueue(Datagram datagram)
{
    const auto position = std::upper_bound(queue_.begin(), queue_.end(), datagram.delivery_time,
        [](Time time, const Datagram& queued) { return time < queued.delivery_time; });
    queue_.insert(position, std::move(datagram));
}

void Network::send(Endpoint source, Endpoint destination, std::span<const std::byte> bytes)
{
    if (random_million() < configuration_.loss_per_million) {
        return;
    }
    Datagram datagram{
        .source = source,
        .destination = destination,
        .delivery_time = now_ + delivery_delay(),
        .bytes = std::vector<std::byte>(bytes.begin(), bytes.end()),
    };
    enqueue(datagram);
    if (random_million() < configuration_.duplicate_per_million) {
        datagram.delivery_time += 1U;
        enqueue(std::move(datagram));
    }
}

Datagram Network::deliver_next()
{
    if (queue_.empty()) {
        throw std::logic_error("cannot deliver from an empty virtual network");
    }
    Datagram datagram = std::move(queue_.front());
    queue_.erase(queue_.begin());
    now_ = datagram.delivery_time;
    return datagram;
}

} // namespace robotweax::srt::simulation
