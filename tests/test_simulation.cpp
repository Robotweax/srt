#include "test.hpp"

#include "robotweax/srt/simulation.hpp"

#include <array>
#include <cstddef>

using namespace robotweax::srt::simulation;

TEST(virtual_network_orders_by_delivery_time)
{
    Network network{{.base_delay_microseconds = 100, .seed = 7}};
    const std::array first{std::byte{1}};
    const std::array second{std::byte{2}};
    network.send(1, 2, first);
    network.send(2, 1, second);
    const auto delivered_first = network.deliver_next();
    const auto delivered_second = network.deliver_next();
    REQUIRE_EQ(delivered_first.delivery_time, 100U);
    REQUIRE_EQ(delivered_second.delivery_time, 100U);
    REQUIRE_EQ(delivered_first.bytes[0], std::byte{1});
}

TEST(virtual_network_can_deterministically_drop_everything)
{
    Network network{{.loss_per_million = 1'000'000, .seed = 9}};
    const std::array bytes{std::byte{0x47}};
    network.send(1, 2, bytes);
    REQUIRE(network.empty());
}

TEST(virtual_network_can_duplicate_everything)
{
    Network network{{.duplicate_per_million = 1'000'000, .seed = 9}};
    const std::array bytes{std::byte{0x47}};
    network.send(1, 2, bytes);
    REQUIRE(!network.empty());
    (void)network.deliver_next();
    REQUIRE(!network.empty());
    (void)network.deliver_next();
    REQUIRE(network.empty());
}
