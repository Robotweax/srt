#include "test.hpp"

#include "compat/transport_runtime.hpp"
#include "robotweax/srt/codec.hpp"
#include "robotweax/srt/control.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <vector>

using namespace robotweax::srt;
using namespace robotweax::srt::compat;
using namespace std::chrono_literals;

namespace {

struct PollClock {
    std::atomic<std::uint64_t> now {1'000};
    std::uint64_t step = 100;

    static std::uint64_t read(void* context) noexcept
    {
        auto& clock = *static_cast<PollClock*>(context);
        return clock.now.fetch_add(clock.step, std::memory_order_relaxed);
    }
};

struct PollOutput {
    std::mutex mutex;
    std::condition_variable changed;
    bool block_first_data = false;
    bool entered = false;
    bool released = false;
    bool timed_out = false;
    PollClock* advance_on_send = nullptr;
    PollClock* observe_clock = nullptr;
    std::vector<std::uint64_t> data_times;
    std::size_t data_attempts = 0;
    std::size_t fail_data_attempt = 0;
    std::vector<std::vector<std::byte>> datagrams;

    static UdpIoResult send(std::span<const std::byte> bytes,
        IpEndpoint, void* context) noexcept
    {
        auto& output = *static_cast<PollOutput*>(context);
        std::unique_lock lock(output.mutex);
        const auto decoded = decode_packet(bytes);
        if (!decoded) {
            return {.error = Error::invalid_state};
        }
        if (decoded.packet.kind == PacketKind::data) {
            ++output.data_attempts;
            if (output.observe_clock != nullptr) {
                output.data_times.push_back(output.observe_clock->now.load(
                    std::memory_order_relaxed));
            }
            if (output.block_first_data && output.data_attempts == 1U) {
                output.entered = true;
                output.changed.notify_all();
                // Bound failures even if a regression retains mutex_ while
                // the test's producer/protocol thread tries to acquire it.
                if (!output.changed.wait_for(lock, 2s,
                        [&output] { return output.released; })) {
                    output.timed_out = true;
                    return {.error = Error::io_error, .system_error = 123};
                }
            }
            if (output.data_attempts == output.fail_data_attempt) {
                return {.error = Error::io_error, .system_error = 456};
            }
        }
        output.datagrams.emplace_back(bytes.begin(), bytes.end());
        if (output.advance_on_send != nullptr) {
            output.advance_on_send->now.fetch_add(1, std::memory_order_relaxed);
        }
        return {.bytes_transferred = bytes.size()};
    }

    bool await_output()
    {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, 2s, [this] { return entered; });
    }

    void release()
    {
        std::lock_guard lock(mutex);
        released = true;
        changed.notify_all();
    }
};

struct PollFixture {
    static constexpr IpEndpoint peer = IpEndpoint::loopback(12'343);
    PollClock clock;
    PollOutput output;
    std::shared_ptr<DatagramChannel> channel =
        std::make_shared<DatagramChannel>();
    std::unique_ptr<ConnectionRuntime> runtime;

    explicit PollFixture(std::uint32_t flow_window = 256)
    {
        channel->set_send_hook_for_testing(PollOutput::send, &output);
        SocketOptions options;
        REQUIRE_EQ(options.set(SocketOption::send_buffer_packets, 256),
            Error::none);
        REQUIRE_EQ(options.set(SocketOption::maximum_bandwidth_bytes_per_second,
                       1'250'000'000),
            Error::none);
        runtime = std::make_unique<ConnectionRuntime>(
            ConnectionRuntime::Configuration {
                .channel = channel,
                .peer = peer,
                .peer_socket_id = 343,
                .initial_sequence = SequenceNumber {700},
                .flow_window_packets = flow_window,
                .options = options,
                .origin = ConnectionRuntime::Clock::now(),
                .now_function = PollClock::read,
                .now_context = &clock,
            });
    }

    void queue(std::size_t count)
    {
        for (std::size_t index = 0; index < count; ++index) {
            std::array<std::byte, 100> bytes;
            bytes.fill(static_cast<std::byte>(index));
            REQUIRE_EQ(runtime->queue_message(bytes, 0, true, false, 0).status,
                MessageIoStatus::success);
        }
    }

    void acknowledge(std::uint32_t next, std::uint32_t window = 256)
    {
        std::array<std::byte, 32> bytes {};
        const auto encoded = encode_acknowledgement_payload(
            Acknowledgement {
                .kind = AcknowledgementKind::full,
                .acknowledgement_number = 1,
                .next_sequence = SequenceNumber {next},
                .available_receive_buffer_packets = window,
            }, bytes);
        REQUIRE(encoded);
        runtime->process_packet(PacketView {
                                    .kind = PacketKind::control,
                                    .control = {
                                        .type = ControlType::acknowledgement,
                                        .type_specific = 1,
                                        .destination_socket_id = 343,
                                    },
                                    .payload = std::span {bytes}.first(
                                        encoded.bytes_written),
                                },
            peer);
    }

    std::vector<std::vector<std::byte>> data()
    {
        std::lock_guard lock(output.mutex);
        std::vector<std::vector<std::byte>> result;
        for (const auto& bytes : output.datagrams) {
            if (decode_packet(bytes).packet.kind == PacketKind::data) {
                result.push_back(bytes);
            }
        }
        return result;
    }
};

} // namespace

TEST(sender_poll_allows_enqueue_during_output_and_pins_channel)
{
    PollFixture fixture;
    fixture.queue(4);
    fixture.output.block_first_data = true;
    auto poll = std::async(std::launch::async,
        [&] { return fixture.runtime->poll(); });
    REQUIRE(fixture.output.await_output());
    const std::weak_ptr<DatagramChannel> weak = fixture.channel;
    fixture.channel.reset();
    REQUIRE(!weak.expired());
    auto producer = std::async(std::launch::async, [&] {
        std::array<std::byte, 100> bytes;
        bytes.fill(std::byte {4});
        return fixture.runtime->queue_message(bytes, 0, true, false, 0);
    });
    const auto producer_status = producer.wait_for(250ms);
    fixture.output.release();
    const auto queued = producer.get();
    (void)poll.get();
    REQUIRE_EQ(producer_status, std::future_status::ready);
    REQUIRE_EQ(queued.status, MessageIoStatus::success);
    REQUIRE(!fixture.output.timed_out);
    REQUIRE(weak.expired());
    const auto packets = fixture.data();
    REQUIRE_EQ(packets.size(), 5U);
    for (std::size_t index = 0; index < packets.size(); ++index) {
        const auto decoded = decode_packet(packets[index]);
        REQUIRE(decoded);
        REQUIRE_EQ(decoded.packet.data.sequence,
            SequenceNumber {static_cast<std::uint32_t>(700 + index)});
        REQUIRE_EQ(decoded.packet.payload.size(), 100U);
        REQUIRE(std::all_of(decoded.packet.payload.begin(),
            decoded.packet.payload.end(), [index](std::byte value) {
                return value == static_cast<std::byte>(index);
            }));
    }
    REQUIRE_EQ(fixture.runtime->statistics(false, true).total.sent.packets, 5U);
}

TEST(sender_poll_serializes_ack_and_competing_poll_until_commit)
{
    PollFixture fixture;
    fixture.queue(4);
    fixture.output.block_first_data = true;
    auto first = std::async(std::launch::async,
        [&] { return fixture.runtime->poll(); });
    REQUIRE(fixture.output.await_output());
    std::promise<void> ack_started;
    auto ack = std::async(std::launch::async, [&] {
        ack_started.set_value();
        fixture.acknowledge(704);
    });
    ack_started.get_future().wait();
    std::promise<void> second_started;
    auto second = std::async(std::launch::async, [&] {
        second_started.set_value();
        return fixture.runtime->poll();
    });
    second_started.get_future().wait();
    const auto ack_status = ack.wait_for(30ms);
    const auto poll_status = second.wait_for(30ms);
    // Waiting protocol calls must release mutex_, not block on the channel's
    // send mutex while retaining the connection lock needed for the commit.
    auto writable = std::async(std::launch::async,
        [&] { return fixture.runtime->writable(); });
    const auto writable_status = writable.wait_for(250ms);
    fixture.output.release();
    (void)first.get();
    ack.get();
    (void)second.get();
    REQUIRE_EQ(ack_status, std::future_status::timeout);
    REQUIRE_EQ(poll_status, std::future_status::timeout);
    REQUIRE_EQ(writable_status, std::future_status::ready);
    REQUIRE(writable.get());
    REQUIRE(!fixture.output.timed_out);
    REQUIRE_EQ(fixture.data().size(), 4U);
    const auto statistics = fixture.runtime->statistics(false, true);
    REQUIRE_EQ(statistics.total.sent.packets, 4U);
    REQUIRE_EQ(statistics.total.received_acknowledgements, 1U);
    REQUIRE_EQ(statistics.instantaneous.sender_buffer_packets, 0U);
    REQUIRE_EQ(statistics.instantaneous.packets_in_flight, 0U);
}

TEST(sender_poll_orders_close_after_prepared_output)
{
    PollFixture fixture;
    fixture.queue(4);
    fixture.output.block_first_data = true;
    auto poll = std::async(std::launch::async,
        [&] { return fixture.runtime->poll(); });
    REQUIRE(fixture.output.await_output());
    std::promise<void> close_started;
    auto close = std::async(std::launch::async, [&] {
        close_started.set_value();
        fixture.runtime->close();
    });
    close_started.get_future().wait();
    const auto close_status = close.wait_for(30ms);
    fixture.output.release();
    (void)poll.get();
    close.get();
    REQUIRE_EQ(close_status, std::future_status::timeout);
    REQUIRE(!fixture.output.timed_out);
    REQUIRE_EQ(fixture.data().size(), 4U);
    REQUIRE_EQ(fixture.output.datagrams.size(), 5U);
    const auto last = decode_packet(fixture.output.datagrams.back());
    REQUIRE(last);
    REQUIRE_EQ(last.packet.control.type, ControlType::shutdown);
    const std::array<std::byte, 1> bytes {};
    REQUIRE_EQ(fixture.runtime->queue_message(bytes, 0, true, false, 0).status,
        MessageIoStatus::local_closed);
}

TEST(sender_poll_commits_only_successful_prefix_and_releases_waiters_on_error)
{
    PollFixture fixture;
    fixture.queue(5);
    fixture.output.block_first_data = true;
    fixture.output.fail_data_attempt = 3;
    auto poll = std::async(std::launch::async,
        [&] { return fixture.runtime->poll(); });
    REQUIRE(fixture.output.await_output());
    auto snapshot = std::async(std::launch::async,
        [&] { return fixture.runtime->statistics(false, true); });
    fixture.output.release();
    (void)poll.get();
    const auto statistics = snapshot.get();
    REQUIRE(!fixture.output.timed_out);
    REQUIRE(fixture.runtime->broken());
    REQUIRE_EQ(fixture.output.data_attempts, 3U);
    REQUIRE_EQ(fixture.data().size(), 2U);
    REQUIRE_EQ(statistics.total.sent.packets, 2U);
    REQUIRE_EQ(statistics.total.sent.payload_bytes, 200U);
    const std::array<std::byte, 1> bytes {};
    const auto queued = fixture.runtime->queue_message(bytes, 0, true, false, 0);
    REQUIRE_EQ(queued.status, MessageIoStatus::broken);
    REQUIRE_EQ(queued.system_error, 456);
    (void)fixture.runtime->poll();
    fixture.runtime->close();
    REQUIRE_EQ(fixture.output.data_attempts, 3U);
    REQUIRE_EQ(fixture.output.datagrams.size(), 2U);
}

TEST(sender_poll_preserves_budget_flow_window_and_retransmission_priority)
{
    PollFixture fixture(64);
    fixture.queue(80);
    const auto first = fixture.runtime->poll();
    REQUIRE_EQ(fixture.data().size(), 64U);
    REQUIRE(!first.immediate_work);
    REQUIRE(!first.next_work_delay.has_value());
    (void)fixture.runtime->poll();
    REQUIRE_EQ(fixture.data().size(), 64U);

    std::array<std::byte, 8> bytes {};
    const std::array losses {SequenceRange {
        .first = SequenceNumber {700}, .last = SequenceNumber {701}}};
    const auto encoded = encode_loss_ranges(losses, bytes);
    REQUIRE(encoded);
    fixture.runtime->process_packet(PacketView {
                                        .kind = PacketKind::control,
                                        .control = {
                                            .type = ControlType::negative_acknowledgement,
                                            .destination_socket_id = 343,
                                        },
                                        .payload = std::span {bytes}.first(
                                            encoded.bytes_written),
                                    },
        PollFixture::peer);
    (void)fixture.runtime->poll();
    auto packets = fixture.data();
    REQUIRE_EQ(packets.size(), 66U);
    for (std::size_t index = 64; index < 66; ++index) {
        const auto decoded = decode_packet(packets[index]);
        REQUIRE(decoded.packet.data.retransmitted);
        REQUIRE_EQ(decoded.packet.data.sequence,
            SequenceNumber {static_cast<std::uint32_t>(700 + index - 64)});
    }
    fixture.acknowledge(764);
    (void)fixture.runtime->poll();
    packets = fixture.data();
    REQUIRE_EQ(packets.size(), 82U);
    REQUIRE_EQ(fixture.runtime->statistics(false, true).total.sent_unique.packets,
        80U);
}

TEST(sender_poll_unlocked_sends_share_one_poll_budget)
{
    PollFixture fixture;
    // Only a completed output advances the clock. Every preparation pass can
    // reserve one packet, so 64 unlocked sends must exhaust one poll budget.
    fixture.clock.step = 0;
    fixture.output.advance_on_send = &fixture.clock;
    fixture.queue(130);
    REQUIRE(fixture.runtime->poll().immediate_work);
    REQUIRE_EQ(fixture.data().size(), 64U);
    REQUIRE(fixture.runtime->poll().immediate_work);
    REQUIRE_EQ(fixture.data().size(), 128U);
    const auto last = fixture.runtime->poll();
    REQUIRE(!last.immediate_work);
    REQUIRE(!last.next_work_delay.has_value());
    REQUIRE_EQ(fixture.data().size(), 130U);
}

TEST(sender_poll_does_not_turn_preparation_time_into_burst_credit)
{
    PollFixture fixture;
    fixture.output.observe_clock = &fixture.clock;
    fixture.queue(4);
    (void)fixture.runtime->poll();
    REQUIRE_EQ(fixture.output.data_times.size(), 4U);
    for (std::size_t index = 1; index < 4; ++index) {
        // A multi-packet preparation followed by one output burst would have
        // identical wire times in this clock, despite distinct prepare times.
        REQUIRE(fixture.output.data_times[index]
            > fixture.output.data_times[index - 1]);
    }
}
