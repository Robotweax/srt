#include "test.hpp"
#include "robotweax/srt/udp.hpp"
#include "robotweax/srt/codec.hpp"
#include "compat/transport_runtime.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

using namespace robotweax::srt;
using namespace robotweax::srt::compat;

// Observe actual dispatch through peer-bound routes, including valid bytes
// in a slot marked truncated so ignoring its error would cause delivery.
TEST(receive_slice_dispatches_batch_and_skips_truncated_slot)
{
    const auto channel = std::make_shared<DatagramChannel>();
    const std::array peers {
        IpEndpoint::loopback(20000), IpEndpoint::loopback(20001)};
    const std::array inboxes {
        std::make_shared<DatagramInbox>(7), std::make_shared<DatagramInbox>(7)};
    REQUIRE(channel->register_setup_inbox(42, peers[0], inboxes[0]));
    REQUIRE(channel->register_setup_inbox(43, peers[1], inboxes[1]));
    std::array<std::vector<std::byte>, 7> datagrams;
    std::array<UdpIoResult, 7> statuses;
    for (std::size_t index = 0; index < datagrams.size(); ++index) {
        const bool second_route = index == 1U || index == 4U;
        const std::array payload {
            static_cast<std::byte>(index), std::byte {0xa5}};
        MutablePacketView packet;
        packet.kind = PacketKind::data;
        packet.data.sequence =
            SequenceNumber {static_cast<std::uint32_t>(index)};
        packet.data.message_number = 1;
        packet.data.boundary = MessageBoundary::solo;
        packet.data.destination_socket_id = second_route ? 43U : 42U;
        packet.payload = payload;
        std::array<std::byte, 64> bytes {};
        const auto encoded = encode_packet(packet, bytes);
        REQUIRE(encoded);
        datagrams[index].assign(
            bytes.begin(), bytes.begin() + encoded.bytes_written);
        statuses[index] = {
            .error = index == 2U ? Error::buffer_too_small : Error::none,
            .bytes_transferred = encoded.bytes_written,
            // Slot 5 has the right destination but the wrong peer.
            .peer = peers[second_route || index == 5U ? 1U : 0U],
        };
    }
    std::size_t calls = 0;
    auto receive = [&](std::span<DatagramChannel::ReceiveSlot> slots) noexcept
        -> DatagramChannel::ReceiveBatch {
        ++calls;
        if (calls > 1U) {
            return {.terminal = {.error = Error::would_block}};
        }
        for (std::size_t index = 0; index < datagrams.size(); ++index) {
            std::copy(datagrams[index].begin(), datagrams[index].end(),
                slots[index].bytes.begin());
            slots[index].status = statuses[index];
        }
        return {.count = datagrams.size(),
            .terminal = {.error = Error::would_block}};
    };
    const auto result = channel->run_batch_for_testing(receive);
    REQUIRE_EQ(calls, 1U);
    REQUIRE(!result.immediate_work);
    const auto verify = [&](std::size_t route,
                            std::span<const std::size_t> expected) {
        for (const auto index : expected) {
            DatagramEnvelope received;
            REQUIRE_EQ(inboxes[route]->pop_for(
                           received, std::chrono::milliseconds {0}),
                InboxPopStatus::received);
            REQUIRE_EQ(received.peer, peers[route]);
            REQUIRE_EQ(received.size, datagrams[index].size());
            REQUIRE(std::equal(datagrams[index].begin(), datagrams[index].end(),
                received.bytes.begin()));
        }
        REQUIRE(!inboxes[route]->ready());
    };
    const std::array<std::size_t, 3> first {0, 3, 6};
    const std::array<std::size_t, 2> second {1, 4};
    verify(0, first);
    verify(1, second);
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
