#include "test.hpp"
#include "robotweax/srt/udp.hpp"
#include "compat/transport_runtime.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

using namespace robotweax::srt;
using namespace robotweax::srt::compat;

// the receive slice consumes a batch and dispatches each datagram in
// order; a truncated slot is skipped without ending the slice.
TEST(receive_slice_dispatches_batch_and_skips_truncated_slot)
{
    const auto channel = std::make_shared<DatagramChannel>();
    std::size_t calls = 0;
    std::size_t delivered = 0;
    auto receive = [&](std::span<DatagramChannel::ReceiveSlot> slots) noexcept
        -> DatagramChannel::ReceiveBatch {
        ++calls;
        if (calls > 1) {
            return {.count = 0, .terminal = {.error = Error::would_block}};
        }
        const std::size_t count = std::min<std::size_t>(5U, slots.size());
        for (std::size_t index = 0; index < count; ++index) {
            slots[index].status = index == 2U
                ? UdpIoResult {.error = Error::buffer_too_small}
                : UdpIoResult {.bytes_transferred = 3U};
            ++delivered;
        }
        return {.count = count, .terminal = {.error = Error::would_block}};
    };
    const auto result = channel->run_batch_for_testing(receive);
    REQUIRE_EQ(calls, 1U);
    REQUIRE_EQ(delivered, 5U);
    REQUIRE(!result.immediate_work);
}

// a full batch still asks the scheduler for an immediate re-run.
TEST(receive_slice_full_batch_requests_immediate_rerun)
{
    const auto channel = std::make_shared<DatagramChannel>();
    auto receive = [&](std::span<DatagramChannel::ReceiveSlot> slots) noexcept
        -> DatagramChannel::ReceiveBatch {
        for (auto& slot : slots) {
            slot.status = UdpIoResult {.bytes_transferred = 3U};
        }
        return {.count = slots.size(), .terminal = {}};
    };
    const auto result = channel->run_batch_for_testing(receive);
    REQUIRE(result.immediate_work);
}

// native batch receive returns datagrams in arrival order with peers,
// reports truncation per datagram, and stops at would_block.
TEST(udp_receive_batch_reads_queued_datagrams_in_order)
{
    UdpSocket receiver;
    UdpSocket sender;
    REQUIRE(receiver.valid());
    REQUIRE(sender.valid());
    REQUIRE_EQ(receiver.bind(IpEndpoint::loopback()), Error::none);
    REQUIRE_EQ(sender.bind(IpEndpoint::loopback()), Error::none);
    const auto destination = receiver.local_endpoint();
    REQUIRE(destination);
    const auto source = sender.local_endpoint();
    REQUIRE(source);
    for (std::uint8_t value = 0; value < 6U; ++value) {
        std::vector<std::byte> bytes(
            value == 3U ? 1'600U : 10U + value, static_cast<std::byte>(value));
        REQUIRE(sender.send_to(bytes, destination.endpoint));
    }
    REQUIRE(receiver.wait_readable(1'000));
    std::array<UdpSocket::ReceiveSlot, 8> slots {};
    std::size_t total = 0;
    for (int attempt = 0; attempt < 50 && total < 6U; ++attempt) {
        const auto batch =
            receiver.receive_batch(std::span {slots}.subspan(total));
        total += batch.count;
        if (total < 6U) {
            (void)receiver.wait_readable(100);
        }
    }
    REQUIRE_EQ(total, 6U);
    for (std::size_t index = 0; index < 6U; ++index) {
        if (index == 3U) {
            REQUIRE_EQ(slots[index].status.error, Error::buffer_too_small);
            continue;
        }
        REQUIRE(slots[index].status);
        REQUIRE_EQ(slots[index].status.bytes_transferred, 10U + index);
        REQUIRE(slots[index].status.peer == source.endpoint);
        REQUIRE_EQ(slots[index].bytes[0], static_cast<std::byte>(index));
    }
    const auto empty = receiver.receive_batch(slots);
    REQUIRE_EQ(empty.count, 0U);
    REQUIRE_EQ(empty.terminal.error, Error::would_block);
}
