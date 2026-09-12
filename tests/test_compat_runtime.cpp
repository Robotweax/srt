#include "test.hpp"
#include "crypto_test_helpers.hpp"

#include "robotweax/srt/codec.hpp"
#include "robotweax/srt/control.hpp"
#include "compat/readiness.hpp"
#include "compat/transport_runtime.hpp"
#include "srt/srt.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace robotweax::srt;
using namespace robotweax::srt::compat;

namespace robotweax::srt::compat {
struct DatagramChannelTestAccess {
    static RuntimePollResult poll(DatagramChannel& channel)
    {
        return channel.run_once();
    }
    static void stop(DatagramChannel& channel)
    {
        channel.stop();
    }
};
} // namespace robotweax::srt::compat

namespace {

struct InboxReadinessObserver {
    std::mutex mutex;
    std::condition_variable changed;
    std::size_t notifications = 0;
};

void record_inbox_readiness(void* context) noexcept
{
    auto& observer = *static_cast<InboxReadinessObserver*>(context);
    {
        std::lock_guard lock(observer.mutex);
        ++observer.notifications;
    }
    observer.changed.notify_all();
}

struct CapturedDatagrams {
    std::mutex mutex;
    std::vector<std::vector<std::byte>> values;
};

UdpIoResult capture_datagram(
    std::span<const std::byte> bytes,
    Ipv4Endpoint,
    void* context) noexcept
{
    auto& captured = *static_cast<CapturedDatagrams*>(context);
    try {
        std::lock_guard lock(captured.mutex);
        captured.values.emplace_back(bytes.begin(), bytes.end());
        return {.bytes_transferred = bytes.size()};
    } catch (...) {
        return {.error = Error::io_error};
    }
}

std::vector<std::vector<std::byte>> take_datagrams(
    CapturedDatagrams& captured)
{
    std::lock_guard lock(captured.mutex);
    std::vector<std::vector<std::byte>> values;
    values.swap(captured.values);
    return values;
}

void deliver(
    CapturedDatagrams& captured,
    ConnectionRuntime& destination,
    Ipv4Endpoint source)
{
    for (const auto& datagram : take_datagrams(captured)) {
        const auto decoded = decode_packet(datagram);
        REQUIRE(decoded);
        destination.process_packet(decoded.packet, source);
    }
}

std::uint64_t injected_now(void* context) noexcept
{
    return *static_cast<const std::uint64_t*>(context);
}

struct PollGate {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool released = false;
};

void wait_at_poll_gate(void* context) noexcept
{
    auto& gate = *static_cast<PollGate*>(context);
    std::unique_lock lock(gate.mutex);
    gate.entered = true;
    gate.changed.notify_all();
    gate.changed.wait(lock, [&] {
        return gate.released;
    });
}

UdpIoResult gate_datagram(
    std::span<const std::byte> bytes, Ipv4Endpoint, void* context) noexcept
{
    wait_at_poll_gate(context);
    return {.bytes_transferred = bytes.size()};
}

struct PollGateRelease {
    std::shared_ptr<PollGate> gate;
    void release()
    {
        std::lock_guard lock(gate->mutex);
        gate->released = true;
        gate->changed.notify_all();
    }
    ~PollGateRelease()
    {
        release();
    }
};

void await_poll_gate(const std::shared_ptr<PollGate>& gate)
{
    std::unique_lock lock(gate->mutex);
    REQUIRE(gate->changed.wait_for(lock, std::chrono::seconds {2}, [&] {
        return gate->entered;
    }));
}

RuntimeScheduler::Snapshot await_channel_timer(
    const RuntimeScheduler& scheduler, std::uint64_t completed)
{
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds {2};
    auto state = scheduler.snapshot();
    while ((state.timers != 1U || state.executing || state.queued != 0U
               || state.completed < completed)
        && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
        state = scheduler.snapshot();
    }
    REQUIRE_EQ(state.timers, 1U);
    REQUIRE_EQ(state.queued, 0U);
    REQUIRE_EQ(state.executing, 0U);
    return state;
}

std::shared_ptr<ConnectionRuntime> queue_paced_fixture(
    const std::shared_ptr<DatagramChannel>& channel, std::uint64_t& now)
{
    auto runtime = std::make_shared<
        ConnectionRuntime>(ConnectionRuntime::Configuration {
        .channel = channel,
        .peer = {.address = {192, 0, 2, 20}, .port = 10'020},
        .peer_socket_id = 1,
        .initial_sequence = SequenceNumber {900},
        .flow_window_packets = 256,
        // Keep the clock-controlled pacer pending while inspecting scheduling.
        .origin = ConnectionRuntime::Clock::now() + std::chrono::hours {1},
        .now_function = injected_now,
        .now_context = &now,
    });
    REQUIRE(channel->register_connection(1, runtime));
    channel->set_idle_wait_for_testing(std::chrono::seconds {5});
    const std::array<std::byte, 1'200> payload {};
    for (int i = 0; i < 2; ++i) {
        REQUIRE_EQ(runtime->queue_message(payload, 0, true, false, 0).status,
            MessageIoStatus::success);
    }
    return runtime;
}

std::vector<std::byte> encrypt_fixture(
    CryptoSession& sender,
    SequenceNumber sequence,
    std::span<const std::byte> plaintext,
    EncryptionKey& key)
{
    std::vector<std::byte> ciphertext(plaintext.size());
    REQUIRE_EQ(sender.encrypt(
                   sequence, plaintext, ciphertext, key),
        Error::none);
    return ciphertext;
}

void exercise_gcm_fec_geometry(std::string_view filter,
    std::uint32_t source_count, std::span<const std::uint32_t> dropped_sources,
    std::size_t expected_filter_packets, std::size_t expected_supplied_packets)
{
    const auto sender_channel = std::make_shared<DatagramChannel>();
    const auto receiver_channel = std::make_shared<DatagramChannel>();
    CapturedDatagrams sender_output;
    CapturedDatagrams receiver_output;
    sender_channel->set_send_hook_for_testing(capture_datagram, &sender_output);
    receiver_channel->set_send_hook_for_testing(
        capture_datagram, &receiver_output);
    const Ipv4Endpoint sender_endpoint {
        .address = {192, 0, 2, 71},
        .port = 14'501,
    };
    const Ipv4Endpoint receiver_endpoint {
        .address = {192, 0, 2, 72},
        .port = 14'502,
    };

    const CryptoConfiguration crypto_configuration {
        .passphrase = "authenticated fec geometry fixture",
        .mode = CryptoMode::aes_gcm,
        .enable_aes_gcm = true,
        .key_length = 32,
        .refresh_rate_packets = 64,
        .preannouncement_packets = 8,
    };
    auto sender_crypto = std::make_shared<CryptoSession>(crypto_configuration);
    auto receiver_crypto =
        std::make_shared<CryptoSession>(crypto_configuration);
    REQUIRE_EQ(sender_crypto->start_initiator(), Error::none);
    REQUIRE_EQ(receiver_crypto->accept_key_material(
                   sender_crypto->pending_key_material(), true),
        Error::none);
    REQUIRE_EQ(sender_crypto->acknowledge_key_material(
                   receiver_crypto->key_material_response(), true),
        Error::none);
    confirm_directional_test_keys(*sender_crypto, *receiver_crypto);

    SocketOptions options;
    REQUIRE_EQ(
        options.set(SocketOption::maximum_payload_size, 16), Error::none);
    REQUIRE_EQ(options.set(SocketOption::send_buffer_packets, 64), Error::none);
    REQUIRE_EQ(
        options.set(SocketOption::receive_buffer_packets, 64), Error::none);
    REQUIRE_EQ(options.set_packet_filter(filter), Error::none);
    const auto origin = ConnectionRuntime::Clock::now();
    std::uint64_t sender_now = 1'500'000;
    std::uint64_t receiver_now = 1'500'000;
    ConnectionRuntime sender {{
        .channel = sender_channel,
        .peer = receiver_endpoint,
        .peer_socket_id = 720,
        .initial_sequence = SequenceNumber {900},
        .flow_window_packets = 256,
        .options = options,
        .negotiated_options =
            {
                .periodic_nak = true,
                .retransmit_flag = true,
            },
        .origin = origin,
        .crypto = sender_crypto,
        .now_function = injected_now,
        .now_context = &sender_now,
    }};
    ConnectionRuntime receiver {{
        .channel = receiver_channel,
        .peer = sender_endpoint,
        .peer_socket_id = 710,
        .initial_sequence = SequenceNumber {900},
        .flow_window_packets = 256,
        .options = options,
        .negotiated_options =
            {
                .periodic_nak = true,
                .retransmit_flag = true,
            },
        .origin = origin,
        .crypto = receiver_crypto,
        .now_function = injected_now,
        .now_context = &receiver_now,
    }};

    for (std::uint32_t index = 0; index < source_count; ++index) {
        const std::array payload {
            static_cast<std::byte>(static_cast<unsigned char>('A') + index)};
        REQUIRE_EQ(sender.queue_message(payload, 0, false, false, -1).status,
            MessageIoStatus::success);
    }
    for (std::size_t attempt = 0; attempt < 80U; ++attempt) {
        (void)sender.poll();
        sender_now += 10U;
    }
    REQUIRE(!sender.broken());

    std::size_t source_packets = 0;
    std::size_t filter_packets = 0;
    for (const auto& datagram : take_datagrams(sender_output)) {
        const auto decoded = decode_packet(datagram);
        REQUIRE(decoded);
        if (decoded.packet.kind != PacketKind::data) {
            receiver.process_packet(decoded.packet, sender_endpoint);
            continue;
        }
        if (decoded.packet.data.message_number == 0U) {
            ++filter_packets;
        } else {
            ++source_packets;
            REQUIRE_EQ(decoded.packet.payload.size(),
                1U + srt_gcm_authentication_tag_size);
            const std::uint32_t index =
                (decoded.packet.data.sequence.value() - 900U)
                & SequenceNumber::mask;
            if (std::find(dropped_sources.begin(), dropped_sources.end(), index)
                != dropped_sources.end()) {
                continue;
            }
        }
        receiver.process_packet(decoded.packet, sender_endpoint);
        ++receiver_now;
    }
    REQUIRE_EQ(source_packets, source_count);
    REQUIRE_EQ(filter_packets, expected_filter_packets);

    std::array<std::byte, 16> received_bytes {};
    for (std::uint32_t index = 0; index < source_count; ++index) {
        const auto received =
            receiver.receive_message(received_bytes, false, -1);
        REQUIRE_EQ(received.status, MessageIoStatus::success);
        REQUIRE_EQ(received.message_number, index + 1U);
        REQUIRE_EQ(received.first_sequence, SequenceNumber {900U + index});
        REQUIRE_EQ(received.bytes, 1U);
        REQUIRE_EQ(received_bytes[0],
            static_cast<std::byte>(static_cast<unsigned char>('A') + index));
    }
    const auto statistics = receiver.statistics(false, true);
    REQUIRE_EQ(statistics.total.receiver_filter_extra, expected_filter_packets);
    REQUIRE_EQ(
        statistics.total.receiver_filter_supply, expected_supplied_packets);
    REQUIRE_EQ(statistics.total.receiver_filter_loss, 0U);
    REQUIRE_EQ(statistics.total.receiver_undecryptable.packets, 0U);
}

} // namespace

TEST(compat_handshake_inbox_is_bounded_ordered_and_closeable)
{
    HandshakeInbox inbox{2};
    HandshakeEnvelope first;
    first.message.packet.socket_id = 10;
    HandshakeEnvelope second;
    second.message.packet.socket_id = 20;
    REQUIRE(inbox.push(first));
    REQUIRE(inbox.push(second));
    REQUIRE(!inbox.push(first));

    HandshakeEnvelope received;
    REQUIRE_EQ(inbox.pop_for(received, std::chrono::milliseconds{0}),
        InboxPopStatus::received);
    REQUIRE_EQ(received.message.packet.socket_id, 10U);
    REQUIRE_EQ(inbox.pop_for(received, std::chrono::milliseconds{0}),
        InboxPopStatus::received);
    REQUIRE_EQ(received.message.packet.socket_id, 20U);
    REQUIRE_EQ(inbox.pop_for(received, std::chrono::milliseconds{0}),
        InboxPopStatus::timeout);
    inbox.close();
    REQUIRE_EQ(inbox.pop_for(received, std::chrono::milliseconds{0}),
        InboxPopStatus::closed);
}

TEST(compat_handshake_inbox_extracts_only_the_matching_connection)
{
    HandshakeInbox inbox {4};
    const Ipv4Endpoint first_peer {
        .address = {192, 0, 2, 1},
        .port = 10'001,
    };
    const Ipv4Endpoint second_peer {
        .address = {192, 0, 2, 2},
        .port = 10'002,
    };
    HandshakeEnvelope first;
    first.message.packet.socket_id = 10;
    first.peer = first_peer;
    HandshakeEnvelope matching;
    matching.message.packet.socket_id = 20;
    matching.peer = second_peer;
    HandshakeEnvelope last;
    last.message.packet.socket_id = 30;
    last.peer = first_peer;
    REQUIRE(inbox.push(first));
    REQUIRE(inbox.push(matching));
    REQUIRE(inbox.push(last));

    HandshakeEnvelope received;
    REQUIRE_EQ(inbox.pop_matching(received, second_peer, 20U),
        InboxPopStatus::received);
    REQUIRE_EQ(received.message.packet.socket_id, 20U);
    REQUIRE_EQ(inbox.pop_matching(received, second_peer, 30U),
        InboxPopStatus::timeout);
    REQUIRE_EQ(inbox.pop_for(received, std::chrono::milliseconds {0}),
        InboxPopStatus::received);
    REQUIRE_EQ(received.message.packet.socket_id, 10U);
    REQUIRE_EQ(inbox.pop_for(received, std::chrono::milliseconds {0}),
        InboxPopStatus::received);
    REQUIRE_EQ(received.message.packet.socket_id, 30U);
}

TEST(compat_datagram_setup_inbox_is_bounded_ordered_and_closeable)
{
    DatagramInbox inbox{2};
    const std::array<std::byte, 3> first{
        std::byte{1}, std::byte{2}, std::byte{3}};
    const std::array<std::byte, 2> second{
        std::byte{4}, std::byte{5}};
    const Ipv4Endpoint first_peer{
        .address = {192, 0, 2, 1},
        .port = 10'001,
    };
    const Ipv4Endpoint second_peer{
        .address = {192, 0, 2, 2},
        .port = 10'002,
    };
    REQUIRE(inbox.push(first, first_peer));
    REQUIRE(inbox.push(second, second_peer));
    REQUIRE(!inbox.push(first, first_peer));

    DatagramEnvelope received;
    REQUIRE_EQ(inbox.pop_for(
                   received,
                   std::chrono::milliseconds{0}),
        InboxPopStatus::received);
    REQUIRE_EQ(received.size, first.size());
    REQUIRE_EQ(received.peer, first_peer);
    REQUIRE(std::equal(
        first.begin(), first.end(),
        received.bytes.begin()));

    REQUIRE_EQ(inbox.pop_for(
                   received,
                   std::chrono::milliseconds{0}),
        InboxPopStatus::received);
    REQUIRE_EQ(received.size, second.size());
    REQUIRE_EQ(received.peer, second_peer);
    REQUIRE(std::equal(
        second.begin(), second.end(),
        received.bytes.begin()));
    REQUIRE_EQ(inbox.pop_for(
                   received,
                   std::chrono::milliseconds{0}),
        InboxPopStatus::timeout);
    inbox.close();
    REQUIRE_EQ(inbox.pop_for(
                   received,
                   std::chrono::milliseconds{0}),
        InboxPopStatus::closed);
}

TEST(compat_datagram_inbox_notifies_one_weak_readiness_subscriber)
{
    DatagramInbox inbox {2};
    const auto observer = std::make_shared<InboxReadinessObserver>();
    REQUIRE(inbox.set_ready_handler(record_inbox_readiness, observer));
    REQUIRE(!inbox.set_ready_handler(record_inbox_readiness, observer));

    constexpr std::array<std::byte, 1> payload {std::byte {1}};
    const Ipv4Endpoint peer {
        .address = {192, 0, 2, 7},
        .port = 10'007,
    };
    REQUIRE(inbox.push(payload, peer));
    REQUIRE(inbox.push(payload, peer));
    {
        std::lock_guard lock(observer->mutex);
        REQUIRE_EQ(observer->notifications, 1U);
    }

    DatagramEnvelope received;
    REQUIRE_EQ(inbox.pop_for(received, std::chrono::milliseconds {0}),
        InboxPopStatus::received);
    REQUIRE_EQ(inbox.pop_for(received, std::chrono::milliseconds {0}),
        InboxPopStatus::received);
    REQUIRE(inbox.push(payload, peer));
    {
        std::lock_guard lock(observer->mutex);
        REQUIRE_EQ(observer->notifications, 2U);
    }

    REQUIRE_EQ(inbox.pop_for(received, std::chrono::milliseconds {0}),
        InboxPopStatus::received);
    inbox.close();
    inbox.close();
    {
        std::lock_guard lock(observer->mutex);
        REQUIRE_EQ(observer->notifications, 3U);
    }
    inbox.clear_ready_handler();
    inbox.close();
    {
        std::lock_guard lock(observer->mutex);
        REQUIRE_EQ(observer->notifications, 3U);
    }
}

TEST(compat_datagram_inbox_reports_existing_readiness_when_subscribed)
{
    DatagramInbox inbox {1};
    constexpr std::array<std::byte, 1> payload {std::byte {2}};
    REQUIRE(inbox.push(payload, Ipv4Endpoint::loopback()));

    const auto observer = std::make_shared<InboxReadinessObserver>();
    REQUIRE(inbox.set_ready_handler(record_inbox_readiness, observer));
    std::lock_guard lock(observer->mutex);
    REQUIRE_EQ(observer->notifications, 1U);
}

TEST(compat_setup_route_promotion_preserves_queued_data_order)
{
    constexpr std::uint32_t local_socket_id = 100;
    const Ipv4Endpoint peer{
        .address = {192, 0, 2, 10},
        .port = 10'010,
    };
    const auto channel = std::make_shared<DatagramChannel>();
    CapturedDatagrams output;
    channel->set_send_hook_for_testing(
        capture_datagram, &output);
    const auto inbox = std::make_shared<DatagramInbox>(4);
    REQUIRE(channel->register_setup_inbox(
        local_socket_id, peer, inbox));

    constexpr std::array<std::byte, 2> first_payload{
        std::byte{'a'}, std::byte{'b'}};
    constexpr std::array<std::byte, 2> second_payload{
        std::byte{'c'}, std::byte{'d'}};
    const auto queue_packet = [&](SequenceNumber sequence,
                                  std::uint32_t message_number,
                                  std::span<const std::byte> payload) {
        MutablePacketView packet;
        packet.kind = PacketKind::data;
        packet.data.sequence = sequence;
        packet.data.message_number = message_number;
        packet.data.boundary = MessageBoundary::solo;
        packet.data.in_order = true;
        packet.data.destination_socket_id = local_socket_id;
        packet.payload = payload;
        std::array<std::byte, 64> datagram{};
        const auto encoded = encode_packet(packet, datagram);
        REQUIRE(encoded);
        REQUIRE(inbox->push(
            std::span{datagram}.first(encoded.bytes_written), peer));
    };
    queue_packet(SequenceNumber{500}, 1, first_payload);
    queue_packet(SequenceNumber{501}, 2, second_payload);

    SocketOptions options;
    const auto runtime = std::make_shared<ConnectionRuntime>(
        ConnectionRuntime::Configuration{
            .channel = channel,
            .peer = peer,
            .peer_socket_id = 200,
            .initial_sequence = SequenceNumber{700},
            .peer_initial_sequence = SequenceNumber{500},
            .has_distinct_peer_initial_sequence = true,
            .flow_window_packets = 256,
            .options = options,
            .origin = ConnectionRuntime::Clock::now(),
        });

    REQUIRE(channel->promote_setup_connection(
        local_socket_id, inbox, runtime));
    DatagramEnvelope leftover;
    REQUIRE_EQ(inbox->pop_for(
                   leftover, std::chrono::milliseconds{0}),
        InboxPopStatus::timeout);

    std::array<std::byte, 8> received{};
    const auto first = runtime->receive_message(
        received, false, -1);
    REQUIRE_EQ(first.status, MessageIoStatus::success);
    REQUIRE_EQ(first.bytes, first_payload.size());
    REQUIRE(std::equal(first_payload.begin(), first_payload.end(),
        received.begin()));
    const auto second = runtime->receive_message(
        received, false, -1);
    REQUIRE_EQ(second.status, MessageIoStatus::success);
    REQUIRE_EQ(second.bytes, second_payload.size());
    REQUIRE(std::equal(second_payload.begin(), second_payload.end(),
        received.begin()));

    channel->unregister_connection(local_socket_id);
    runtime->close();
}

TEST(compat_listener_setup_route_uses_peer_socket_identity)
{
    constexpr std::uint32_t accepted_socket_id = 701U;
    constexpr std::uint32_t listener_socket_id = 501U;
    constexpr std::uint32_t caller_socket_id = 601U;
    const auto channel = std::make_shared<DatagramChannel>();
    REQUIRE(channel->socket.valid());
    REQUIRE_EQ(channel->socket.bind(IpEndpoint::loopback()), Error::none);
    const auto channel_endpoint = channel->socket.local_endpoint();
    REQUIRE(channel_endpoint);

    UdpSocket sender;
    REQUIRE(sender.valid());
    REQUIRE_EQ(sender.bind(IpEndpoint::loopback()), Error::none);
    const auto sender_endpoint = sender.local_endpoint();
    REQUIRE(sender_endpoint);

    const auto listener_inbox = std::make_shared<HandshakeInbox>(4);
    const auto setup_inbox = std::make_shared<DatagramInbox>(4);
    REQUIRE(channel->set_listener_inbox(listener_inbox));
    REQUIRE(channel->register_setup_inbox(accepted_socket_id,
        sender_endpoint.endpoint, setup_inbox, caller_socket_id));
    REQUIRE(channel->start());

    HandshakeAction repeated;
    repeated.kind = HandshakeActionKind::send;
    repeated.packet.version = handshake_version_5;
    repeated.packet.request = HandshakeRequest::conclusion;
    repeated.packet.socket_id = caller_socket_id;
    repeated.packet.syn_cookie = 0x1234'5678U;
    std::array<std::byte, 1'500> datagram {};
    auto encoded = encode_handshake_datagram(
        repeated, PacketTimestamp {42U}, listener_socket_id, datagram);
    REQUIRE(encoded);
    REQUIRE(sender.send_to(std::span {datagram}.first(encoded.bytes_written),
        channel_endpoint.endpoint));

    DatagramEnvelope routed;
    REQUIRE_EQ(setup_inbox->pop_for(routed, std::chrono::seconds {2}),
        InboxPopStatus::received);
    const auto decoded =
        decode_handshake_datagram(std::span {routed.bytes}.first(routed.size));
    REQUIRE(decoded);
    REQUIRE_EQ(decoded.message.packet.socket_id, caller_socket_id);
    HandshakeEnvelope listener_received;
    REQUIRE_EQ(listener_inbox->pop_for(
                   listener_received, std::chrono::milliseconds {0}),
        InboxPopStatus::timeout);

    repeated.packet.socket_id = caller_socket_id + 1U;
    encoded = encode_handshake_datagram(
        repeated, PacketTimestamp {43U}, listener_socket_id, datagram);
    REQUIRE(encoded);
    REQUIRE(sender.send_to(std::span {datagram}.first(encoded.bytes_written),
        channel_endpoint.endpoint));
    REQUIRE_EQ(
        listener_inbox->pop_for(listener_received, std::chrono::seconds {2}),
        InboxPopStatus::received);
    REQUIRE_EQ(
        listener_received.message.packet.socket_id, caller_socket_id + 1U);
    REQUIRE_EQ(setup_inbox->pop_for(routed, std::chrono::milliseconds {0}),
        InboxPopStatus::timeout);

    channel->unregister_setup_inbox(accepted_socket_id, setup_inbox);
    channel->clear_listener_inbox(listener_inbox);
}

TEST(compat_dispatcher_scheduler_starts_and_stops_without_network_traffic)
{
    auto channel = std::make_shared<DatagramChannel>();
    REQUIRE(channel->socket.valid());
    REQUIRE(channel->start());
    REQUIRE(channel->running());
    channel.reset();
}

TEST(compat_dispatcher_rearms_a_timed_channel_for_send_work_notification)
{
    auto scheduler =
        std::make_shared<RuntimeScheduler>(RuntimeScheduler::Configuration {
            .shard_count = 1,
            .queue_capacity_per_shard = 8,
            .timer_capacity_per_shard = 8,
        });
    REQUIRE(scheduler->start());

    auto channel = std::make_shared<DatagramChannel>();
    channel->set_idle_wait_for_testing(std::chrono::milliseconds {5'000});
    REQUIRE(channel->start(scheduler, 0U));

    const auto idle_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds {2};
    RuntimeScheduler::Snapshot idle = scheduler->snapshot();
    while (
        idle.timers != 1U && std::chrono::steady_clock::now() < idle_deadline) {
        std::this_thread::yield();
        idle = scheduler->snapshot();
    }
    REQUIRE_EQ(idle.timers, 1U);
    channel->notify_send_work();

    const auto wake_deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds {100};
    RuntimeScheduler::Snapshot active = scheduler->snapshot();
    while ((active.timers_canceled == 0U || active.completed == idle.completed)
        && std::chrono::steady_clock::now() < wake_deadline) {
        std::this_thread::yield();
        active = scheduler->snapshot();
    }
    REQUIRE_EQ(active.timers_canceled, 1U);
    REQUIRE(active.completed > idle.completed);

    channel.reset();
    scheduler->stop();
}

TEST(compat_dispatcher_many_channels_share_fixed_scheduler_shards)
{
    constexpr std::size_t channel_count = 64U;
    constexpr std::size_t shard_count = 2U;
    auto scheduler =
        std::make_shared<RuntimeScheduler>(RuntimeScheduler::Configuration {
            .shard_count = shard_count,
            .queue_capacity_per_shard = channel_count,
            .timer_capacity_per_shard = channel_count,
        });
    REQUIRE(scheduler->start());

    std::vector<std::shared_ptr<DatagramChannel>> channels;
    channels.reserve(channel_count);
    for (std::size_t index = 0; index < channel_count; ++index) {
        auto channel = std::make_shared<DatagramChannel>();
        REQUIRE(channel->socket.valid());
        REQUIRE(channel->start(scheduler, index));
        channels.push_back(std::move(channel));
    }

    const auto running_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds {2};
    while (scheduler->snapshot().accepted < channel_count
        && std::chrono::steady_clock::now() < running_deadline) {
        std::this_thread::yield();
    }
    const RuntimeScheduler::Snapshot running = scheduler->snapshot();
    REQUIRE_EQ(running.shard_count, shard_count);
    REQUIRE(running.accepted >= channel_count);
    REQUIRE(running.executing <= shard_count);
    REQUIRE_EQ(running.rejected_full, 0U);
    for (const auto& channel : channels) {
        REQUIRE(channel->running());
    }

    channels.clear();
    const auto idle_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds {2};
    RuntimeScheduler::Snapshot idle = scheduler->snapshot();
    while ((idle.queued != 0U || idle.timers != 0U || idle.executing != 0U)
        && std::chrono::steady_clock::now() < idle_deadline) {
        std::this_thread::yield();
        idle = scheduler->snapshot();
    }
    REQUIRE_EQ(idle.queued, 0U);
    REQUIRE_EQ(idle.timers, 0U);
    REQUIRE_EQ(idle.executing, 0U);
    scheduler->stop();
}

TEST(compat_runtime_reports_the_next_paced_send_deadline)
{
    const auto channel = std::make_shared<DatagramChannel>();
    CapturedDatagrams output;
    channel->set_send_hook_for_testing(capture_datagram, &output);
    std::uint64_t now = 1'000;
    const auto origin = ConnectionRuntime::Clock::now();
    ConnectionRuntime runtime {{
        .channel = channel,
        .peer =
            {
                .address = {192, 0, 2, 20},
                .port = 10'020,
            },
        .peer_socket_id = 0x3400U,
        .initial_sequence = SequenceNumber {900},
        .flow_window_packets = 256,
        .origin = origin,
        .now_function = injected_now,
        .now_context = &now,
    }};

    const std::array<std::byte, 1'200> payload {};
    REQUIRE_EQ(runtime.queue_message(payload, 0, true, false, 0).status,
        MessageIoStatus::success);
    REQUIRE_EQ(runtime.queue_message(payload, 0, true, false, 0).status,
        MessageIoStatus::success);

    const RuntimePollResult first = runtime.poll();
    REQUIRE(!first.immediate_work);
    REQUIRE(first.next_work_deadline.has_value());
    REQUIRE_EQ(
        *first.next_work_deadline, origin + std::chrono::microseconds {1'010});
    REQUIRE_EQ(take_datagrams(output).size(), 1U);

    now += 9;
    const RuntimePollResult early = runtime.poll();
    REQUIRE(!early.immediate_work);
    REQUIRE(early.next_work_deadline.has_value());
    REQUIRE_EQ(*early.next_work_deadline, *first.next_work_deadline);
    REQUIRE(take_datagrams(output).empty());

    ++now;
    const RuntimePollResult ready = runtime.poll();
    REQUIRE(!ready.immediate_work);
    REQUIRE(!ready.next_work_deadline.has_value());
    REQUIRE_EQ(take_datagrams(output).size(), 1U);

    // A late wake keeps the original pacing rule: no catch-up credit or burst.
    REQUIRE_EQ(runtime.queue_message(payload, 0, true, false, 0).status,
        MessageIoStatus::success);
    REQUIRE_EQ(runtime.queue_message(payload, 0, true, false, 0).status,
        MessageIoStatus::success);
    now += 50;
    const auto late = runtime.poll();
    REQUIRE_EQ(*late.next_work_deadline,
        origin + std::chrono::microseconds {now + 10});
    REQUIRE_EQ(take_datagrams(output).size(), 1U);
}

TEST(compat_dispatcher_preserves_the_earliest_absolute_pacer_deadline)
{
    auto channel = std::make_shared<DatagramChannel>();
    CapturedDatagrams output;
    channel->set_send_hook_for_testing(capture_datagram, &output);
    std::uint64_t now = 1'000;
    // Controlled origins make the deadlines independent of wall-clock timing.
    // They are already due on the scheduler clock; they must not move forward
    // when the dispatcher aggregates or schedules them.
    const auto origin = ConnectionRuntime::Clock::time_point {};
    for (std::uint32_t id = 1; id <= 2; ++id) {
        auto runtime = std::make_shared<ConnectionRuntime>(
            ConnectionRuntime::Configuration {
                .channel = channel,
                .peer = {.address = {192, 0, 2, 20}, .port = 10'020},
                .peer_socket_id = id,
                .initial_sequence = SequenceNumber {900},
                .flow_window_packets = 256,
                .origin = origin + std::chrono::microseconds {id * 100},
                .now_function = injected_now,
                .now_context = &now,
            });
        REQUIRE(channel->register_connection(id, runtime));
        const std::array<std::byte, 1'200> payload {};
        for (int index = 0; index < 2; ++index) {
            REQUIRE_EQ(
                runtime->queue_message(payload, 0, true, false, 0).status,
                MessageIoStatus::success);
        }
    }
    const auto first = DatagramChannelTestAccess::poll(*channel);
    REQUIRE(!first.immediate_work);
    REQUIRE_EQ(
        *first.next_work_deadline, origin + std::chrono::microseconds {1'110});
    REQUIRE_EQ(take_datagrams(output).size(), 2U);
    now += 9;
    const auto early = DatagramChannelTestAccess::poll(*channel);
    REQUIRE(!early.immediate_work);
    REQUIRE_EQ(early.next_work_deadline, first.next_work_deadline);
    REQUIRE(take_datagrams(output).empty());
    ++now;
    const auto before = ConnectionRuntime::Clock::now();
    const auto drained = DatagramChannelTestAccess::poll(*channel);
    const auto after = ConnectionRuntime::Clock::now();
    REQUIRE_EQ(take_datagrams(output).size(), 2U);
    REQUIRE(!drained.immediate_work);
    REQUIRE(
        *drained.next_work_deadline >= before + std::chrono::milliseconds {2});
    REQUIRE(
        *drained.next_work_deadline <= after + std::chrono::milliseconds {2});
}

TEST(compat_dispatcher_pacer_notifications_rearm_once_and_survive_restart)
{
    auto scheduler =
        std::make_shared<RuntimeScheduler>(RuntimeScheduler::Configuration {
            .shard_count = 1,
            .queue_capacity_per_shard = 1,
            .timer_capacity_per_shard = 1,
        });
    REQUIRE(scheduler->start());
    auto channel = std::make_shared<DatagramChannel>();
    auto sending = std::make_shared<PollGate>();
    channel->set_send_hook_for_testing(gate_datagram, sending.get());
    std::uint64_t now = 1'000;
    auto runtime = queue_paced_fixture(channel, now);
    PollGateRelease release_sending {sending};
    REQUIRE(channel->start(scheduler, 0U));
    await_poll_gate(sending);
    // Multiple notifications during an active poll require exactly one follow-up.
    for (int i = 0; i < 16; ++i) {
        channel->notify_send_work();
    }
    release_sending.release();
    const auto timed = await_channel_timer(*scheduler, 2U);
    REQUIRE_EQ(timed.completed, 2U);
    REQUIRE_EQ(timed.timers_scheduled, 1U);

    auto busy = std::make_shared<PollGate>();
    PollGateRelease release_busy {busy};
    REQUIRE_EQ(scheduler->submit(0U, {wait_at_poll_gate, busy}),
        RuntimeScheduler::SubmitStatus::accepted);
    await_poll_gate(busy);
    // Cancel the future timer, then coalesce notifications while its replacement
    // waits in the ready queue. No second callback or timer may be created.
    for (int i = 0; i < 16; ++i) {
        channel->notify_send_work();
    }
    REQUIRE_EQ(scheduler->snapshot().queued, 1U);
    REQUIRE_EQ(scheduler->snapshot().timers, 0U);
    release_busy.release();
    const auto rearmed = await_channel_timer(*scheduler, timed.completed + 2U);
    REQUIRE_EQ(rearmed.completed, timed.completed + 2U);
    REQUIRE_EQ(rearmed.timers_canceled, 1U);
    REQUIRE_EQ(rearmed.timers_scheduled, 2U);

    DatagramChannelTestAccess::stop(*channel);
    REQUIRE_EQ(scheduler->snapshot().timers, 0U);
    REQUIRE(!channel->running());
    REQUIRE(channel->start(scheduler, 0U));
    const auto restarted =
        await_channel_timer(*scheduler, rearmed.completed + 1U);
    REQUIRE_EQ(restarted.completed, rearmed.completed + 1U);
    REQUIRE_EQ(restarted.timers_scheduled, 3U);
    DatagramChannelTestAccess::stop(*channel);
    REQUIRE_EQ(scheduler->snapshot().timers, 0U);
    REQUIRE_EQ(scheduler->snapshot().queued, 0U);
    scheduler->stop();
}

TEST(compat_dispatcher_pacer_timer_exhaustion_breaks_the_connection)
{
    auto scheduler =
        std::make_shared<RuntimeScheduler>(RuntimeScheduler::Configuration {
            .shard_count = 1,
            .queue_capacity_per_shard = 1,
            .timer_capacity_per_shard = 1,
        });
    REQUIRE(scheduler->start());
    const auto unrelated = scheduler->schedule_at(0U,
        ConnectionRuntime::Clock::now() + std::chrono::hours {1},
        {wait_at_poll_gate, std::make_shared<PollGate>()});
    REQUIRE_EQ(unrelated.status, RuntimeScheduler::SubmitStatus::accepted);
    auto channel = std::make_shared<DatagramChannel>();
    CapturedDatagrams output;
    channel->set_send_hook_for_testing(capture_datagram, &output);
    std::uint64_t now = 1'000;
    auto runtime = queue_paced_fixture(channel, now);
    REQUIRE(channel->start(scheduler, 0U));
    const auto deadline =
        ConnectionRuntime::Clock::now() + std::chrono::seconds {2};
    while (channel->running() && ConnectionRuntime::Clock::now() < deadline) {
        std::this_thread::yield();
    }
    REQUIRE(!channel->running());
    // stop waits for any active callback; broken-state publication follows the
    // scheduling failure and must be visible before another message is queued.
    scheduler->stop();
    const std::array<std::byte, 1> payload {};
    REQUIRE_EQ(runtime->queue_message(payload, 0, true, false, 0).status,
        MessageIoStatus::broken);
    REQUIRE_EQ(scheduler->snapshot().rejected_full, 1U);
    REQUIRE_EQ(scheduler->snapshot().timers, 0U);
    REQUIRE_EQ(scheduler->snapshot().queued, 0U);
}

TEST(compat_runtime_waits_for_key_response_without_runnable_send_work)
{
    const auto channel = std::make_shared<DatagramChannel>();
    CapturedDatagrams output;
    channel->set_send_hook_for_testing(capture_datagram, &output);
    auto crypto = std::make_shared<CryptoSession>(CryptoConfiguration {
        .passphrase = "poll waits for key response",
        .key_length = 16,
    });
    REQUIRE_EQ(crypto->start_initiator(), Error::none);
    std::uint64_t now = 1'000'000;
    ConnectionRuntime runtime {{
        .channel = channel,
        .peer =
            {
                .address = {192, 0, 2, 21},
                .port = 10'021,
            },
        .peer_socket_id = 0x3401U,
        .initial_sequence = SequenceNumber {901},
        .flow_window_packets = 256,
        .origin = ConnectionRuntime::Clock::now(),
        .crypto = crypto,
        .now_function = injected_now,
        .now_context = &now,
    }};
    const std::array<std::byte, 1> payload {std::byte {'k'}};
    REQUIRE_EQ(runtime.queue_message(payload, 0, true, false, 0).status,
        MessageIoStatus::success);

    const RuntimePollResult waiting = runtime.poll();
    REQUIRE(!waiting.immediate_work);
    REQUIRE(!waiting.next_work_deadline.has_value());
    const auto datagrams = take_datagrams(output);
    REQUIRE_EQ(datagrams.size(), 1U);
    const auto decoded = decode_packet(datagrams.front());
    REQUIRE(decoded);
    REQUIRE_EQ(decoded.packet.kind, PacketKind::control);
    REQUIRE_EQ(decoded.packet.control.type, ControlType::user_defined);
    REQUIRE_EQ(decoded.packet.control.subtype, key_material_request_subtype);
}

TEST(
    compat_dispatcher_discards_oversized_datagrams_without_breaking_connections)
{
    constexpr std::uint32_t local_socket_id = 0x1234U;
    const auto channel = std::make_shared<DatagramChannel>();
    REQUIRE(channel->socket.valid());
    REQUIRE_EQ(channel->socket.bind(IpEndpoint::loopback()), Error::none);
    const auto channel_endpoint = channel->socket.local_endpoint();
    REQUIRE(channel_endpoint);

    UdpSocket sender;
    REQUIRE(sender.valid());
    REQUIRE_EQ(sender.bind(IpEndpoint::loopback()), Error::none);
    const auto sender_endpoint = sender.local_endpoint();
    REQUIRE(sender_endpoint);

    const auto runtime =
        std::make_shared<ConnectionRuntime>(ConnectionRuntime::Configuration {
            .channel = channel,
            .peer = sender_endpoint.endpoint,
            .peer_socket_id = 0x5678U,
            .initial_sequence = SequenceNumber {500},
            .flow_window_packets = 256,
            .origin = ConnectionRuntime::Clock::now(),
        });
    REQUIRE(channel->register_connection(local_socket_id, runtime));
    REQUIRE(channel->start());

    std::array<std::byte,
        DatagramEnvelope::maximum_size + 1U - packet_header_size>
        oversized_payload {};
    MutablePacketView oversized_packet;
    oversized_packet.kind = PacketKind::data;
    oversized_packet.data.sequence = SequenceNumber {500};
    oversized_packet.data.boundary = MessageBoundary::solo;
    oversized_packet.data.in_order = true;
    oversized_packet.data.message_number = 1;
    oversized_packet.data.destination_socket_id = local_socket_id;
    oversized_packet.payload = oversized_payload;
    std::array<std::byte, DatagramEnvelope::maximum_size + 1U>
        oversized_datagram {};
    const auto oversized_encoded =
        encode_packet(oversized_packet, oversized_datagram);
    REQUIRE(oversized_encoded);
    REQUIRE_EQ(oversized_encoded.bytes_written, oversized_datagram.size());
    REQUIRE(sender.send_to(oversized_datagram, channel_endpoint.endpoint));

    const std::array<std::byte, 1> payload {std::byte {'v'}};
    MutablePacketView packet;
    packet.kind = PacketKind::data;
    packet.data.sequence = SequenceNumber {500};
    packet.data.boundary = MessageBoundary::solo;
    packet.data.in_order = true;
    packet.data.message_number = 1;
    packet.data.destination_socket_id = local_socket_id;
    packet.payload = payload;
    std::array<std::byte, 64> datagram {};
    const auto encoded = encode_packet(packet, datagram);
    REQUIRE(encoded);
    REQUIRE(sender.send_to(std::span {datagram}.first(encoded.bytes_written),
        channel_endpoint.endpoint));

    std::array<std::byte, 8> received_payload {};
    MessageIoResult received {
        .status = MessageIoStatus::would_block,
    };
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds {2};
    while (received.status == MessageIoStatus::would_block
        && std::chrono::steady_clock::now() < deadline) {
        received = runtime->receive_message(received_payload, false, 0);
        if (received.status == MessageIoStatus::would_block) {
            std::this_thread::sleep_for(std::chrono::milliseconds {1});
        }
    }

    REQUIRE_EQ(received.status, MessageIoStatus::success);
    REQUIRE_EQ(received.bytes, payload.size());
    REQUIRE_EQ(received_payload[0], payload[0]);
    REQUIRE(!runtime->broken());

    channel->unregister_connection(local_socket_id);
    runtime->close();
}

TEST(compat_runtime_transfers_and_acknowledges_a_live_message)
{
    const auto caller_channel = std::make_shared<DatagramChannel>();
    const auto listener_channel = std::make_shared<DatagramChannel>();
    CapturedDatagrams caller_output;
    CapturedDatagrams listener_output;
    caller_channel->set_send_hook_for_testing(
        capture_datagram, &caller_output);
    listener_channel->set_send_hook_for_testing(
        capture_datagram, &listener_output);

    const Ipv4Endpoint caller_endpoint{
        .address = {192, 0, 2, 1},
        .port = 10'001,
    };
    const Ipv4Endpoint listener_endpoint{
        .address = {192, 0, 2, 2},
        .port = 10'002,
    };
    SocketOptions options;
    REQUIRE_EQ(options.set(
                   SocketOption::maximum_payload_size, 1'316),
        Error::none);
    REQUIRE_EQ(options.set(
                   SocketOption::send_buffer_packets, 1),
        Error::none);
    NegotiatedLiveOptions negotiated;

    const auto origin = ConnectionRuntime::Clock::now();
    std::uint64_t caller_now = 1'000'000;
    std::uint64_t listener_now = 1'000'000;
    ConnectionRuntime caller{{
        .channel = caller_channel,
        .peer = listener_endpoint,
        .peer_socket_id = 200,
        .initial_sequence = SequenceNumber{1'000},
        .flow_window_packets = 256,
        .options = options,
        .negotiated_options = negotiated,
        .origin = origin,
        .now_function = injected_now,
        .now_context = &caller_now,
    }};
    ConnectionRuntime listener{{
        .channel = listener_channel,
        .peer = caller_endpoint,
        .peer_socket_id = 100,
        .initial_sequence = SequenceNumber{1'000},
        .flow_window_packets = 256,
        .options = options,
        .negotiated_options = negotiated,
        .origin = origin,
        .now_function = injected_now,
        .now_context = &listener_now,
    }};

    constexpr char text[] = "runtime message";
    const auto queued = caller.queue_message(
        std::as_bytes(std::span{text, sizeof(text)}),
        0, true, false, -1);
    REQUIRE_EQ(queued.status, MessageIoStatus::success);
    REQUIRE_EQ(queued.bytes, sizeof(text));
    REQUIRE_EQ(queued.message_number, 1U);
    const auto would_block = caller.queue_message(
        std::as_bytes(std::span{text, sizeof(text)}),
        0, true, false, -1);
    REQUIRE_EQ(would_block.status, MessageIoStatus::would_block);

    (void)caller.poll();
    deliver(caller_output, listener, caller_endpoint);
    listener_now += 10'000;
    (void)listener.poll();
    deliver(listener_output, caller, listener_endpoint);
    deliver(caller_output, listener, caller_endpoint);

    std::array<std::byte, 1'500> received_bytes{};
    const auto received =
        listener.receive_message(received_bytes, false, -1);
    REQUIRE_EQ(received.status, MessageIoStatus::success);
    REQUIRE_EQ(received.bytes, sizeof(text));
    REQUIRE_EQ(received.message_number, 1U);
    REQUIRE_EQ(received.first_sequence, SequenceNumber{1'000});
    REQUIRE(std::equal(std::as_bytes(std::span{text, sizeof(text)}).begin(),
        std::as_bytes(std::span{text, sizeof(text)}).end(),
        received_bytes.begin()));

    const auto caller_statistics =
        caller.statistics(false, true);
    REQUIRE_EQ(caller_statistics.total.sent.packets, 1U);
    REQUIRE_EQ(
        caller_statistics.total.sent_unique.packets, 1U);
    REQUIRE_EQ(
        caller_statistics.total
            .received_acknowledgements,
        1U);
    const auto listener_statistics =
        listener.statistics(false, true);
    REQUIRE_EQ(
        listener_statistics.total.received.packets, 1U);
    REQUIRE_EQ(
        listener_statistics.total.received_unique.packets,
        1U);
    REQUIRE_EQ(
        listener_statistics.total.sent_acknowledgements,
        1U);

    const auto timed_out =
        listener.receive_message(received_bytes, true, 0);
    REQUIRE_EQ(timed_out.status, MessageIoStatus::timeout);
}

TEST(compat_runtime_advertises_receive_window_after_application_read)
{
    const auto channel = std::make_shared<DatagramChannel>();
    CapturedDatagrams output;
    channel->set_send_hook_for_testing(
        capture_datagram, &output);
    const Ipv4Endpoint peer{
        .address = {192, 0, 2, 30},
        .port = 12'030,
    };
    SocketOptions options;
    REQUIRE_EQ(options.set(
                   SocketOption::receive_buffer_packets, 2),
        Error::none);
    std::uint64_t now = 100;
    ConnectionRuntime runtime{{
        .channel = channel,
        .peer = peer,
        .peer_socket_id = 300,
        .initial_sequence = SequenceNumber{10},
        .flow_window_packets = 256,
        .options = options,
        .origin = ConnectionRuntime::Clock::now(),
        .now_function = injected_now,
        .now_context = &now,
    }};

    const std::array<std::byte, 1> payload{
        std::byte{'w'}};
    PacketView packet;
    packet.kind = PacketKind::data;
    packet.data.boundary = MessageBoundary::solo;
    packet.payload = payload;
    packet.data.sequence = SequenceNumber{10};
    packet.data.message_number = 1;
    runtime.process_packet(packet, peer);
    now = 200;
    packet.data.sequence = SequenceNumber{11};
    packet.data.message_number = 2;
    runtime.process_packet(packet, peer);
    (void)take_datagrams(output);

    now = 10'000;
    (void)runtime.poll();
    (void)take_datagrams(output);

    std::array<std::byte, 1> received{};
    now = 10'001;
    REQUIRE_EQ(runtime.receive_message(
                   received, false, -1).status,
        MessageIoStatus::success);
    (void)runtime.poll();

    bool saw_reopened_window = false;
    for (const auto& datagram : take_datagrams(output)) {
        const auto decoded = decode_packet(datagram);
        REQUIRE(decoded);
        if (decoded.packet.kind != PacketKind::control
            || decoded.packet.control.type
                != ControlType::acknowledgement) {
            continue;
        }
        const auto acknowledgement =
            decode_acknowledgement(decoded.packet);
        REQUIRE(acknowledgement);
        saw_reopened_window = saw_reopened_window
            || acknowledgement.acknowledgement
                .available_receive_buffer_packets == 1U;
    }
    REQUIRE(saw_reopened_window);
}

TEST(compat_runtime_honors_peer_receive_window_from_full_ack)
{
    const auto channel = std::make_shared<DatagramChannel>();
    CapturedDatagrams output;
    channel->set_send_hook_for_testing(capture_datagram, &output);
    const Ipv4Endpoint peer {
        .address = {192, 0, 2, 31},
        .port = 12'031,
    };
    SocketOptions options;
    REQUIRE_EQ(options.set(SocketOption::send_buffer_packets, 8), Error::none);
    std::uint64_t now = 1'000;
    ConnectionRuntime runtime {{
        .channel = channel,
        .peer = peer,
        .peer_socket_id = 301,
        .initial_sequence = SequenceNumber {700},
        .flow_window_packets = 4,
        .options = options,
        .origin = ConnectionRuntime::Clock::now(),
        .now_function = injected_now,
        .now_context = &now,
    }};

    const std::array<std::byte, 1> payload {std::byte {'x'}};
    REQUIRE_EQ(runtime.queue_message(payload, 0, true, false, -1).status,
        MessageIoStatus::success);
    (void)runtime.poll();
    auto sent = take_datagrams(output);
    REQUIRE_EQ(sent.size(), 1U);
    const auto first_data = decode_packet(sent.front());
    REQUIRE(first_data);
    REQUIRE_EQ(first_data.packet.kind, PacketKind::data);

    std::array<std::byte, 32> acknowledgement_payload {};
    const auto deliver_window = [&](std::uint32_t number,
                                    SequenceNumber next_sequence,
                                    std::uint32_t packets) {
        const Acknowledgement acknowledgement {
            .kind = AcknowledgementKind::full,
            .acknowledgement_number = number,
            .next_sequence = next_sequence,
            .available_receive_buffer_packets = packets,
        };
        const auto encoded = encode_acknowledgement_payload(
            acknowledgement, acknowledgement_payload);
        REQUIRE(encoded);
        const PacketView packet {
            .kind = PacketKind::control,
            .control =
                {
                    .type = ControlType::acknowledgement,
                    .type_specific = number,
                    .destination_socket_id = 301,
                },
            .payload = std::span {acknowledgement_payload}.first(
                encoded.bytes_written),
        };
        runtime.process_packet(packet, peer);
        (void)take_datagrams(output);
    };

    deliver_window(1, SequenceNumber {700}, 0);
    REQUIRE_EQ(
        runtime.statistics(false, true).instantaneous.flow_window_packets, 0U);

    REQUIRE_EQ(runtime.queue_message(payload, 0, true, false, -1).status,
        MessageIoStatus::success);
    now += 100'000;
    (void)runtime.poll();
    REQUIRE(take_datagrams(output).empty());

    std::array<std::byte, 8> loss_payload {};
    const std::array<SequenceRange, 1> losses {{
        {
            .first = SequenceNumber {700},
            .last = SequenceNumber {700},
        },
    }};
    const auto encoded_loss = encode_loss_ranges(losses, loss_payload);
    REQUIRE(encoded_loss);
    const PacketView loss {
        .kind = PacketKind::control,
        .control =
            {
                .type = ControlType::negative_acknowledgement,
                .destination_socket_id = 301,
            },
        .payload = std::span {loss_payload}.first(encoded_loss.bytes_written),
    };
    runtime.process_packet(loss, peer);
    now += 100'000;
    (void)runtime.poll();
    sent = take_datagrams(output);
    REQUIRE_EQ(sent.size(), 1U);
    const auto retransmission = decode_packet(sent.front());
    REQUIRE(retransmission);
    REQUIRE_EQ(retransmission.packet.kind, PacketKind::data);
    REQUIRE_EQ(retransmission.packet.data.sequence, SequenceNumber {700});
    REQUIRE(retransmission.packet.data.retransmitted);

    now += 100'000;
    (void)runtime.poll();
    REQUIRE(take_datagrams(output).empty());

    // A stale Full ACK must not reopen the peer's advertised window.
    deliver_window(2, SequenceNumber {699}, 4);
    REQUIRE_EQ(
        runtime.statistics(false, true).instantaneous.flow_window_packets, 0U);

    deliver_window(3, SequenceNumber {701}, 1);
    REQUIRE_EQ(
        runtime.statistics(false, true).instantaneous.flow_window_packets, 1U);
    now += 100'000;
    (void)runtime.poll();
    sent = take_datagrams(output);
    REQUIRE_EQ(sent.size(), 1U);
    const auto decoded = decode_packet(sent.front());
    REQUIRE(decoded);
    REQUIRE_EQ(decoded.packet.kind, PacketKind::data);
    REQUIRE_EQ(decoded.packet.data.sequence, SequenceNumber {701});
}

namespace {

void exercise_lite_ack_receive_window(
    SequenceNumber initial, CongestionController controller)
{
    const auto channel = std::make_shared<DatagramChannel>();
    CapturedDatagrams output;
    channel->set_send_hook_for_testing(capture_datagram, &output);
    const Ipv4Endpoint peer {
        .address = {192, 0, 2, 31},
        .port = 12'031,
    };
    SocketOptions options;
    REQUIRE_EQ(options.set(SocketOption::send_buffer_packets, 8), Error::none);
    REQUIRE_EQ(options.set_congestion_controller(
                   controller == CongestionController::file ? "file" : "live"),
        Error::none);
    std::uint64_t now = 1'000;
    ConnectionRuntime runtime {{
        .channel = channel,
        .peer = peer,
        .peer_socket_id = 301,
        .initial_sequence = initial,
        .flow_window_packets = 4,
        .options = options,
        .origin = ConnectionRuntime::Clock::now(),
        .now_function = injected_now,
        .now_context = &now,
    }};
    const std::array<std::byte, 1> payload {std::byte {'x'}};
    for (int index = 0; index < 8; ++index) {
        REQUIRE_EQ(runtime.queue_message(payload, 0, true, false, -1).status,
            MessageIoStatus::success);
    }
    const auto poll_data = [&] {
        for (int index = 0; index < 4; ++index) {
            now += 1'000;
            (void)runtime.poll();
        }
        std::vector<SequenceNumber> sequences;
        for (const auto& datagram : take_datagrams(output)) {
            const auto decoded = decode_packet(datagram);
            REQUIRE(decoded);
            if (decoded.packet.kind == PacketKind::data) {
                REQUIRE(!decoded.packet.data.retransmitted);
                sequences.push_back(decoded.packet.data.sequence);
            }
        }
        return sequences;
    };
    std::uint32_t ack_number = 0;
    const auto acknowledge = [&](AcknowledgementKind kind,
                                 std::uint32_t advance, std::uint32_t free) {
        std::array<std::byte, 32> bytes {};
        const Acknowledgement acknowledgement {
            .kind = kind,
            .acknowledgement_number =
                kind == AcknowledgementKind::full ? ++ack_number : 0U,
            .next_sequence = initial.advanced(advance),
            .available_receive_buffer_packets = free,
        };
        const auto encoded =
            encode_acknowledgement_payload(acknowledgement, bytes);
        REQUIRE(encoded);
        runtime.process_packet(
            {
                .kind = PacketKind::control,
                .control =
                    {
                        .type = ControlType::acknowledgement,
                        .type_specific = acknowledgement.acknowledgement_number,
                        .destination_socket_id = 301,
                    },
                .payload = std::span {bytes}.first(encoded.bytes_written),
            },
            peer);
    };
    const auto window = [&] {
        return runtime.statistics(false, true)
            .instantaneous.flow_window_packets;
    };

    const auto first = poll_data();
    REQUIRE_EQ(first.size(), 4U);
    REQUIRE_EQ(first.front(), initial);
    REQUIRE_EQ(first.back(), initial.advanced(3));
    // Two packets received, two still in flight; the four-slot receiver
    // has not delivered anything to its application.
    acknowledge(AcknowledgementKind::full, 2, 2);
    REQUIRE_EQ(window(), 2U);
    REQUIRE(poll_data().empty());
    acknowledge(AcknowledgementKind::lite, 3, 0);
    REQUIRE_EQ(window(), 1U);
    REQUIRE(poll_data().empty());

    // Duplicated/stale ACKs and an invalid future ACK cannot consume credit
    // twice or reopen the window. No wall-clock sleeps or network are used.
    acknowledge(AcknowledgementKind::lite, 3, 0);
    acknowledge(AcknowledgementKind::lite, 2, 0);
    acknowledge(AcknowledgementKind::full, 2, 100);
    acknowledge(AcknowledgementKind::lite, 9, 0);
    REQUIRE_EQ(window(), 1U);
    acknowledge(AcknowledgementKind::lite, 4, 0);
    REQUIRE_EQ(window(), 0U);
    REQUIRE(poll_data().empty());

    // Only a fresh buffer advertisement permits new data again.
    acknowledge(AcknowledgementKind::full, 4, 2);
    REQUIRE_EQ(window(), 2U);
    const auto second = poll_data();
    REQUIRE_EQ(second.size(), 2U);
    REQUIRE_EQ(second.front(), initial.advanced(4));
    REQUIRE_EQ(second.back(), initial.advanced(5));
    // A shrinking advertisement can be smaller than the outstanding flight.
    // Its following Lite ACK must saturate at zero, not wrap to a huge window.
    acknowledge(AcknowledgementKind::full, 4, 1);
    acknowledge(AcknowledgementKind::lite, 6, 0);
    REQUIRE_EQ(window(), 0U);
    REQUIRE(poll_data().empty());
    acknowledge(AcknowledgementKind::small, 6, 2);
    REQUIRE_EQ(window(), 2U);
    const auto last = poll_data();
    REQUIRE_EQ(last.size(), 2U);
    REQUIRE_EQ(last.front(), initial.advanced(6));
    REQUIRE_EQ(last.back(), initial.advanced(7));
}

} // namespace

TEST(compat_runtime_lite_ack_consumes_live_receive_window_credit)
{
    exercise_lite_ack_receive_window(
        SequenceNumber {700}, CongestionController::live);
}

TEST(compat_runtime_lite_ack_consumes_file_receive_window_credit)
{
    exercise_lite_ack_receive_window(
        SequenceNumber {700}, CongestionController::file);
}

TEST(compat_runtime_lite_ack_receive_window_handles_live_sequence_rollover)
{
    exercise_lite_ack_receive_window(
        SequenceNumber {SequenceNumber::mask - 1}, CongestionController::live);
}

TEST(compat_runtime_lite_ack_receive_window_handles_file_sequence_rollover)
{
    exercise_lite_ack_receive_window(
        SequenceNumber {SequenceNumber::mask - 1}, CongestionController::file);
}

TEST(compat_runtime_initializes_sender_window_from_peer_handshake)
{
    SocketOptions options;
    REQUIRE_EQ(
        options.set(SocketOption::receive_buffer_packets, 8), Error::none);
    ConnectionRuntime runtime {{
        .peer =
            Ipv4Endpoint {
                .address = {192, 0, 2, 32},
                .port = 12'032,
            },
        .peer_socket_id = 302,
        .initial_sequence = SequenceNumber {800},
        .flow_window_packets = 8,
        .peer_flow_window_packets = 2,
        .options = options,
        .origin = ConnectionRuntime::Clock::now(),
    }};

    REQUIRE_EQ(
        runtime.statistics(false, true).instantaneous.flow_window_packets, 2U);
}

TEST(compat_runtime_message_ttl_uses_injected_clock_and_sends_dropreq)
{
    const auto channel = std::make_shared<DatagramChannel>();
    CapturedDatagrams output;
    channel->set_send_hook_for_testing(
        capture_datagram, &output);
    const Ipv4Endpoint peer{
        .address = {192, 0, 2, 20},
        .port = 12'020,
    };
    SocketOptions options;
    const auto origin = ConnectionRuntime::Clock::now();
    std::uint64_t now = 1'000;
    ConnectionRuntime runtime{{
        .channel = channel,
        .peer = peer,
        .peer_socket_id = 200,
        .initial_sequence = SequenceNumber{700},
        .flow_window_packets = 256,
        .options = options,
        .origin = origin,
        .now_function = injected_now,
        .now_context = &now,
    }};

    const std::array<std::byte, 1> payload{
        std::byte{'x'}};
    REQUIRE_EQ(runtime.queue_message(
        payload, 0, true, false, -1, 2).status,
        MessageIoStatus::success);
    now = 3'001;
    (void)runtime.poll();

    const auto expired_output = take_datagrams(output);
    bool saw_drop_request = false;
    for (const auto& datagram : expired_output) {
        const auto decoded = decode_packet(datagram);
        REQUIRE(decoded);
        REQUIRE(decoded.packet.kind != PacketKind::data);
        if (decoded.packet.kind == PacketKind::control
            && decoded.packet.control.type
                == ControlType::drop_request) {
            const auto drop =
                decode_drop_request(decoded.packet);
            REQUIRE(drop);
            REQUIRE_EQ(drop.request.message_number, 1U);
            REQUIRE_EQ(drop.request.sequences.first,
                SequenceNumber{700});
            REQUIRE_EQ(drop.request.sequences.last,
                SequenceNumber{700});
            saw_drop_request = true;
        }
    }
    REQUIRE(saw_drop_request);
    const auto after_expiration =
        runtime.statistics(false, true);
    REQUIRE_EQ(
        after_expiration.total
            .sender_message_ttl_dropped.packets,
        1U);
    REQUIRE_EQ(
        after_expiration.total
            .sender_message_ttl_dropped.payload_bytes,
        payload.size());
    REQUIRE_EQ(
        after_expiration.total
            .sender_tlpktdrop_dropped.packets,
        0U);

    // As in the reference API, every negative value means unlimited TTL.
    REQUIRE_EQ(runtime.queue_message(
        payload, 0, true, false, -1, -2).status,
        MessageIoStatus::success);
    now = 4'000'000;
    (void)runtime.poll();
    const auto unlimited_output = take_datagrams(output);
    REQUIRE(std::any_of(
        unlimited_output.begin(), unlimited_output.end(),
        [](const auto& datagram) {
            const auto decoded = decode_packet(datagram);
            return decoded
                && decoded.packet.kind == PacketKind::data
                && decoded.packet.data.sequence
                    == SequenceNumber{701};
        }));
}

TEST(compat_runtime_stamps_an_explicit_application_source_time)
{
    const auto channel = std::make_shared<DatagramChannel>();
    CapturedDatagrams output;
    channel->set_send_hook_for_testing(
        capture_datagram, &output);
    const Ipv4Endpoint peer{
        .address = {192, 0, 2, 21},
        .port = 12'021,
    };
    const auto origin = ConnectionRuntime::Clock::now();
    const auto origin_microseconds =
        std::chrono::duration_cast<std::chrono::microseconds>(
            origin.time_since_epoch()).count();
    std::uint64_t now = 200'000;
    ConnectionRuntime runtime{{
        .channel = channel,
        .peer = peer,
        .peer_socket_id = 201,
        .initial_sequence = SequenceNumber{710},
        .flow_window_packets = 256,
        .origin = origin,
        .now_function = injected_now,
        .now_context = &now,
    }};

    const std::array<std::byte, 1> payload{
        std::byte{'s'}};
    constexpr std::int64_t source_offset_microseconds = 123'456;
    REQUIRE_EQ(runtime.queue_message(
        payload,
        origin_microseconds + source_offset_microseconds,
        true, false, -1).status,
        MessageIoStatus::success);
    (void)runtime.poll();

    const auto datagrams = take_datagrams(output);
    REQUIRE_EQ(datagrams.size(), 1U);
    const auto decoded = decode_packet(datagrams.front());
    REQUIRE(decoded);
    REQUIRE_EQ(decoded.packet.kind, PacketKind::data);
    REQUIRE_EQ(decoded.packet.data.timestamp,
        PacketTimestamp{source_offset_microseconds});
}

TEST(compat_runtime_tlpktdrop_has_a_distinct_internal_counter)
{
    const auto channel = std::make_shared<DatagramChannel>();
    CapturedDatagrams output;
    channel->set_send_hook_for_testing(
        capture_datagram, &output);
    const Ipv4Endpoint peer{
        .address = {192, 0, 2, 22},
        .port = 12'022,
    };
    std::uint64_t now = 1'000;
    NegotiatedLiveOptions negotiated;
    negotiated.too_late_packet_drop = true;
    ConnectionRuntime runtime{{
        .channel = channel,
        .peer = peer,
        .peer_socket_id = 220,
        .initial_sequence = SequenceNumber{720},
        .flow_window_packets = 256,
        .negotiated_options = negotiated,
        .origin = ConnectionRuntime::Clock::now(),
        .now_function = injected_now,
        .now_context = &now,
    }};

    const std::array<std::byte, 2> payload{
        std::byte{'t'}, std::byte{'l'}};
    REQUIRE_EQ(runtime.queue_message(
                   payload, 0, true, false, -1)
                   .status,
        MessageIoStatus::success);
    now = 1'021'001;
    (void)runtime.poll();

    const auto snapshot = runtime.statistics(false, true);
    REQUIRE_EQ(
        snapshot.total.sender_tlpktdrop_dropped.packets,
        1U);
    REQUIRE_EQ(
        snapshot.total
            .sender_tlpktdrop_dropped.payload_bytes,
        payload.size());
    REQUIRE_EQ(
        snapshot.total
            .sender_message_ttl_dropped.packets,
        0U);
    SRT_TRACEBSTATS public_statistics{};
    populate_trace_statistics(
        snapshot, public_statistics);
    REQUIRE_EQ(public_statistics.pktSndDropTotal, 1);
    REQUIRE_EQ(public_statistics.byteSndDropTotal, 46U);
}

TEST(compat_runtime_counts_belated_packets_after_delivery)
{
    const auto channel = std::make_shared<DatagramChannel>();
    CapturedDatagrams output;
    channel->set_send_hook_for_testing(
        capture_datagram, &output);
    const Ipv4Endpoint peer{
        .address = {192, 0, 2, 23},
        .port = 12'023,
    };
    std::uint64_t now = 1'050;
    NegotiatedLiveOptions negotiated{
        .receive_tsbpd = true,
        .retransmit_flag = true,
    };
    ConnectionRuntime runtime{{
        .channel = channel,
        .peer = peer,
        .peer_socket_id = 230,
        .initial_sequence = SequenceNumber{10},
        .flow_window_packets = 256,
        .negotiated_options = negotiated,
        .origin = ConnectionRuntime::Clock::now(),
        .handshake_arrival_microseconds = 1'000,
        .peer_handshake_timestamp = PacketTimestamp{0},
        .now_function = injected_now,
        .now_context = &now,
    }};

    const std::array<std::byte, 1> payload{
        std::byte{'b'}};
    PacketView packet;
    packet.kind = PacketKind::data;
    packet.data.sequence = SequenceNumber{10};
    packet.data.message_number = 1;
    packet.data.boundary = MessageBoundary::solo;
    packet.data.timestamp = PacketTimestamp{50};
    packet.payload = payload;
    runtime.process_packet(packet, peer);

    std::array<std::byte, 1> received{};
    REQUIRE_EQ(runtime.receive_message(
                   received, false, -1)
                   .status,
        MessageIoStatus::success);
    (void)take_datagrams(output);

    now = 2'050;
    packet.data.retransmitted = true;
    runtime.process_packet(packet, peer);
    const auto snapshot = runtime.statistics(false, true);
    REQUIRE_EQ(snapshot.total.received.packets, 2U);
    REQUIRE_EQ(
        snapshot.total.received_unique.packets, 1U);
    REQUIRE_EQ(
        snapshot.interval.receiver_belated.packets,
        1U);
    REQUIRE_EQ(snapshot.average_belated_milliseconds, 1.0);
    SRT_TRACEBSTATS public_statistics{};
    populate_trace_statistics(
        snapshot, public_statistics);
    REQUIRE_EQ(public_statistics.pktRcvBelated, 1);
    REQUIRE_EQ(public_statistics.pktRcvAvgBelatedTime, 1.0);
}

TEST(compat_runtime_reports_physical_loss_and_reorder_distance)
{
    const auto channel = std::make_shared<DatagramChannel>();
    CapturedDatagrams output;
    channel->set_send_hook_for_testing(
        capture_datagram, &output);
    const Ipv4Endpoint peer{
        .address = {192, 0, 2, 24},
        .port = 12'024,
    };
    std::uint64_t now = 1'000;
    ConnectionRuntime runtime{{
        .channel = channel,
        .peer = peer,
        .peer_socket_id = 240,
        .initial_sequence = SequenceNumber{10},
        .flow_window_packets = 256,
        .negotiated_options = {
            .retransmit_flag = true,
        },
        .origin = ConnectionRuntime::Clock::now(),
        .now_function = injected_now,
        .now_context = &now,
    }};

    const std::array<std::byte, 1> payload{
        std::byte{'r'}};
    PacketView packet;
    packet.kind = PacketKind::data;
    packet.data.sequence = SequenceNumber{12};
    packet.data.message_number = 3;
    packet.data.boundary = MessageBoundary::solo;
    packet.payload = payload;
    runtime.process_packet(packet, peer);
    now = 1'100;
    packet.data.sequence = SequenceNumber{11};
    packet.data.message_number = 2;
    runtime.process_packet(packet, peer);

    const auto snapshot = runtime.statistics(false, true);
    REQUIRE_EQ(snapshot.total.receiver_loss.packets, 2U);
    REQUIRE_EQ(snapshot.reorder_distance_packets, 1U);
    SRT_TRACEBSTATS public_statistics{};
    populate_trace_statistics(
        snapshot, public_statistics);
    REQUIRE_EQ(public_statistics.pktRcvLossTotal, 2);
    REQUIRE_EQ(public_statistics.pktReorderDistance, 1);
}

TEST(compat_runtime_applies_lossmaxttl_and_delays_initial_nak)
{
    const auto channel = std::make_shared<DatagramChannel>();
    CapturedDatagrams output;
    channel->set_send_hook_for_testing(
        capture_datagram, &output);
    const Ipv4Endpoint peer{
        .address = {192, 0, 2, 25},
        .port = 12'025,
    };
    std::uint64_t now = 1'000;
    SocketOptions options;
    REQUIRE_EQ(options.set(
                   SocketOption::maximum_reorder_tolerance_packets,
                   2),
        Error::none);
    ConnectionRuntime runtime{{
        .channel = channel,
        .peer = peer,
        .peer_socket_id = 250,
        .initial_sequence = SequenceNumber{10},
        .flow_window_packets = 256,
        .options = options,
        .negotiated_options = {
            .periodic_nak = false,
            .retransmit_flag = true,
        },
        .origin = ConnectionRuntime::Clock::now(),
        .now_function = injected_now,
        .now_context = &now,
    }};

    const std::array<std::byte, 1> payload{
        std::byte{'n'}};
    PacketView packet;
    packet.kind = PacketKind::data;
    packet.data.boundary = MessageBoundary::solo;
    packet.payload = payload;

    const auto has_nak = [](const auto& datagrams) {
        return std::any_of(
            datagrams.begin(), datagrams.end(),
            [](const auto& datagram) {
                const auto decoded = decode_packet(datagram);
                return decoded
                    && decoded.packet.kind
                        == PacketKind::control
                    && decoded.packet.control.type
                        == ControlType::negative_acknowledgement;
            });
    };

    packet.data.sequence = SequenceNumber{12};
    packet.data.message_number = 3;
    runtime.process_packet(packet, peer);
    REQUIRE(has_nak(take_datagrams(output)));

    now = 1'100;
    packet.data.sequence = SequenceNumber{10};
    packet.data.message_number = 1;
    runtime.process_packet(packet, peer);
    (void)take_datagrams(output);

    now = 1'150;
    packet.data.sequence = SequenceNumber{11};
    packet.data.message_number = 2;
    runtime.process_packet(packet, peer);
    (void)take_datagrams(output);

    now = 1'200;
    packet.data.sequence = SequenceNumber{15};
    packet.data.message_number = 6;
    runtime.process_packet(packet, peer);
    REQUIRE(!has_nak(take_datagrams(output)));

    now = 1'250;
    packet.data.sequence = SequenceNumber{13};
    packet.data.message_number = 4;
    runtime.process_packet(packet, peer);
    REQUIRE(!has_nak(take_datagrams(output)));

    now = 1'300;
    packet.data.sequence = SequenceNumber{16};
    packet.data.message_number = 7;
    runtime.process_packet(packet, peer);
    const auto matured = take_datagrams(output);
    REQUIRE(has_nak(matured));
    bool saw_remaining_sequence = false;
    for (const auto& datagram : matured) {
        const auto decoded = decode_packet(datagram);
        REQUIRE(decoded);
        if (decoded.packet.kind != PacketKind::control
            || decoded.packet.control.type
                != ControlType::negative_acknowledgement) {
            continue;
        }
        const auto loss = decode_loss_range(
            decoded.packet.payload);
        REQUIRE(loss);
        REQUIRE_EQ(loss.range.first, SequenceNumber{14});
        REQUIRE_EQ(loss.range.last, SequenceNumber{14});
        saw_remaining_sequence = true;
    }
    REQUIRE(saw_remaining_sequence);

    auto snapshot = runtime.statistics(false, true);
    REQUIRE_EQ(snapshot.reorder_distance_packets, 2U);
    REQUIRE_EQ(snapshot.reorder_tolerance_packets, 2U);
    SRT_TRACEBSTATS public_statistics{};
    populate_trace_statistics(snapshot, public_statistics);
    REQUIRE_EQ(public_statistics.pktReorderDistance, 2);
    REQUIRE_EQ(public_statistics.pktReorderTolerance, 2);

    REQUIRE_EQ(options.set(
                   SocketOption::maximum_reorder_tolerance_packets,
                   1),
        Error::none);
    runtime.apply_options(options);
    snapshot = runtime.statistics(false, true);
    REQUIRE_EQ(snapshot.reorder_tolerance_packets, 1U);
}

TEST(compat_runtime_sends_disjoint_losses_in_one_nak_datagram)
{
    const auto channel = std::make_shared<DatagramChannel>();
    CapturedDatagrams output;
    channel->set_send_hook_for_testing(
        capture_datagram, &output);
    const Ipv4Endpoint peer{
        .address = {192, 0, 2, 26},
        .port = 12'026,
    };
    std::uint64_t now = 1'000;
    ConnectionRuntime runtime{{
        .channel = channel,
        .peer = peer,
        .peer_socket_id = 260,
        .initial_sequence = SequenceNumber{10},
        .flow_window_packets = 256,
        .negotiated_options = {
            .periodic_nak = true,
            .retransmit_flag = true,
        },
        .origin = ConnectionRuntime::Clock::now(),
        .now_function = injected_now,
        .now_context = &now,
    }};

    const std::array<std::byte, 1> payload{
        std::byte{'b'}};
    PacketView packet;
    packet.kind = PacketKind::data;
    packet.data.boundary = MessageBoundary::solo;
    packet.payload = payload;

    packet.data.sequence = SequenceNumber{12};
    packet.data.message_number = 3;
    runtime.process_packet(packet, peer);
    (void)take_datagrams(output);

    now = 1'100;
    packet.data.sequence = SequenceNumber{14};
    packet.data.message_number = 5;
    runtime.process_packet(packet, peer);
    (void)take_datagrams(output);

    now = 1'200;
    packet.data.sequence = SequenceNumber{11};
    packet.data.message_number = 2;
    runtime.process_packet(packet, peer);
    (void)take_datagrams(output);

    now = 151'100;
    (void)runtime.poll();
    std::size_t nak_datagrams = 0;
    std::array<SequenceRange, 2> decoded_ranges{};
    std::size_t decoded_range_count = 0;
    for (const auto& datagram : take_datagrams(output)) {
        const auto decoded = decode_packet(datagram);
        REQUIRE(decoded);
        if (decoded.packet.kind != PacketKind::control
            || decoded.packet.control.type
                != ControlType::negative_acknowledgement) {
            continue;
        }
        ++nak_datagrams;
        auto loss_payload = decoded.packet.payload;
        while (!loss_payload.empty()) {
            const auto loss =
                decode_loss_range(loss_payload);
            REQUIRE(loss);
            REQUIRE(decoded_range_count
                < decoded_ranges.size());
            decoded_ranges[decoded_range_count++] =
                loss.range;
            loss_payload = loss_payload.subspan(
                loss.bytes_consumed);
        }
    }
    REQUIRE_EQ(nak_datagrams, 1U);
    REQUIRE_EQ(decoded_range_count, 2U);
    REQUIRE_EQ(decoded_ranges[0].first,
        SequenceNumber{10});
    REQUIRE_EQ(decoded_ranges[0].last,
        SequenceNumber{10});
    REQUIRE_EQ(decoded_ranges[1].first,
        SequenceNumber{13});
    REQUIRE_EQ(decoded_ranges[1].last,
        SequenceNumber{13});
}

TEST(compat_runtime_file_mode_performs_partial_stream_io)
{
    const auto caller_channel =
        std::make_shared<DatagramChannel>();
    const auto listener_channel =
        std::make_shared<DatagramChannel>();
    CapturedDatagrams caller_output;
    CapturedDatagrams listener_output;
    caller_channel->set_send_hook_for_testing(
        capture_datagram, &caller_output);
    listener_channel->set_send_hook_for_testing(
        capture_datagram, &listener_output);
    const Ipv4Endpoint caller_endpoint{
        .address = {192, 0, 2, 31},
        .port = 15'001,
    };
    const Ipv4Endpoint listener_endpoint{
        .address = {192, 0, 2, 32},
        .port = 15'002,
    };

    SocketOptions options;
    REQUIRE_EQ(options.set(
                   SocketOption::transmission_type,
                   static_cast<std::int64_t>(
                       TransmissionType::file)),
        Error::none);
    REQUIRE_EQ(options.set(
                   SocketOption::maximum_payload_size,
                   4),
        Error::none);
    REQUIRE_EQ(options.set(
                   SocketOption::send_buffer_packets,
                   2),
        Error::none);
    std::uint64_t caller_now = 1'000'000;
    std::uint64_t listener_now = 1'000'000;
    const auto origin = ConnectionRuntime::Clock::now();
    ConnectionRuntime caller{{
        .channel = caller_channel,
        .peer = listener_endpoint,
        .peer_socket_id = 320,
        .initial_sequence = SequenceNumber{100},
        .flow_window_packets = 256,
        .options = options,
        .origin = origin,
        .now_function = injected_now,
        .now_context = &caller_now,
    }};
    ConnectionRuntime listener{{
        .channel = listener_channel,
        .peer = caller_endpoint,
        .peer_socket_id = 310,
        .initial_sequence = SequenceNumber{100},
        .flow_window_packets = 256,
        .options = options,
        .origin = origin,
        .now_function = injected_now,
        .now_context = &listener_now,
    }};

    const std::array<std::byte, 10> input{
        std::byte{'a'}, std::byte{'b'}, std::byte{'c'},
        std::byte{'d'}, std::byte{'e'}, std::byte{'f'},
        std::byte{'g'}, std::byte{'h'}, std::byte{'i'},
        std::byte{'j'}};
    const auto partial = caller.queue_stream(
        input, false, -1);
    REQUIRE_EQ(partial.status,
        MessageIoStatus::success);
    REQUIRE_EQ(partial.bytes, 8U);
    REQUIRE_EQ(caller.queue_stream(
                   std::span{input}.last(2),
                   false, -1)
                   .status,
        MessageIoStatus::would_block);

    (void)caller.poll();
    deliver(caller_output, listener, caller_endpoint);
    deliver(listener_output, caller, listener_endpoint);
    caller_now += 10;
    listener_now += 10;
    (void)caller.poll();
    deliver(caller_output, listener, caller_endpoint);
    deliver(listener_output, caller, listener_endpoint);

    std::array<std::byte, 3> first_read{};
    const auto first = listener.receive_stream(
        first_read, false, -1);
    REQUIRE_EQ(first.status, MessageIoStatus::success);
    REQUIRE_EQ(first.bytes, 3U);
    REQUIRE_EQ(first_read[0], std::byte{'a'});
    REQUIRE_EQ(first_read[2], std::byte{'c'});

    std::array<std::byte, 8> second_read{};
    const auto second = listener.receive_stream(
        second_read, false, -1);
    REQUIRE_EQ(second.status, MessageIoStatus::success);
    REQUIRE_EQ(second.bytes, 5U);
    REQUIRE_EQ(second_read[0], std::byte{'d'});
    REQUIRE_EQ(second_read[4], std::byte{'h'});

    REQUIRE_EQ(caller.queue_stream(
                   std::span{input}.last(2),
                   false, -1)
                   .bytes,
        2U);
}

#ifdef ENABLE_AEAD_API_PREVIEW
TEST(compat_runtime_gcm_file_mode_authenticates_partial_stream_io)
{
    const auto sender_channel = std::make_shared<DatagramChannel>();
    const auto receiver_channel = std::make_shared<DatagramChannel>();
    CapturedDatagrams sender_output;
    CapturedDatagrams receiver_output;
    sender_channel->set_send_hook_for_testing(capture_datagram, &sender_output);
    receiver_channel->set_send_hook_for_testing(
        capture_datagram, &receiver_output);
    const Ipv4Endpoint sender_endpoint {
        .address = {192, 0, 2, 33},
        .port = 15'003,
    };
    const Ipv4Endpoint receiver_endpoint {
        .address = {192, 0, 2, 34},
        .port = 15'004,
    };

    const CryptoConfiguration crypto_configuration {
        .passphrase = "authenticated file stream fixture",
        .mode = CryptoMode::aes_gcm,
        .enable_aes_gcm = true,
        .key_length = 16,
        .refresh_rate_packets = 64,
        .preannouncement_packets = 8,
    };
    auto sender_crypto = std::make_shared<CryptoSession>(crypto_configuration);
    auto receiver_crypto =
        std::make_shared<CryptoSession>(crypto_configuration);
    REQUIRE_EQ(sender_crypto->start_initiator(), Error::none);
    REQUIRE_EQ(receiver_crypto->accept_key_material(
                   sender_crypto->pending_key_material(), true),
        Error::none);
    REQUIRE_EQ(sender_crypto->acknowledge_key_material(
                   receiver_crypto->key_material_response(), true),
        Error::none);
    confirm_directional_test_keys(*sender_crypto, *receiver_crypto);

    SocketOptions options;
    REQUIRE_EQ(options.set(SocketOption::transmission_type,
                   static_cast<std::int64_t>(TransmissionType::file)),
        Error::none);
    REQUIRE_EQ(options.set(SocketOption::crypto_mode,
                   static_cast<std::int64_t>(CryptoMode::aes_gcm)),
        Error::none);
    REQUIRE_EQ(options.set(SocketOption::maximum_payload_size, 4), Error::none);
    REQUIRE_EQ(options.set(SocketOption::send_buffer_packets, 8), Error::none);

    std::uint64_t sender_now = 1'000'000;
    std::uint64_t receiver_now = 1'000'000;
    const auto origin = ConnectionRuntime::Clock::now();
    ConnectionRuntime sender {{
        .channel = sender_channel,
        .peer = receiver_endpoint,
        .peer_socket_id = 340,
        .initial_sequence = SequenceNumber {SequenceNumber::mask - 1U},
        .flow_window_packets = 256,
        .options = options,
        .origin = origin,
        .crypto = sender_crypto,
        .now_function = injected_now,
        .now_context = &sender_now,
    }};
    ConnectionRuntime receiver {{
        .channel = receiver_channel,
        .peer = sender_endpoint,
        .peer_socket_id = 330,
        .initial_sequence = SequenceNumber {SequenceNumber::mask - 1U},
        .flow_window_packets = 256,
        .options = options,
        .origin = origin,
        .crypto = receiver_crypto,
        .now_function = injected_now,
        .now_context = &receiver_now,
    }};

    const std::array<std::byte, 10> input {std::byte {'a'}, std::byte {'b'},
        std::byte {'c'}, std::byte {'d'}, std::byte {'e'}, std::byte {'f'},
        std::byte {'g'}, std::byte {'h'}, std::byte {'i'}, std::byte {'j'}};
    const auto queued = sender.queue_stream(input, false, -1);
    REQUIRE_EQ(queued.status, MessageIoStatus::success);
    REQUIRE_EQ(queued.bytes, input.size());

    std::vector<std::vector<std::byte>> protected_datagrams;
    for (std::size_t attempt = 0;
        attempt < 12U && protected_datagrams.size() < 3U; ++attempt) {
        (void)sender.poll();
        for (auto& datagram : take_datagrams(sender_output)) {
            const auto decoded = decode_packet(datagram);
            REQUIRE(decoded);
            if (decoded.packet.kind == PacketKind::data) {
                REQUIRE(decoded.packet.payload.size()
                    > srt_gcm_authentication_tag_size);
                protected_datagrams.push_back(std::move(datagram));
            } else {
                receiver.process_packet(decoded.packet, sender_endpoint);
            }
        }
        sender_now += 10'000;
    }
    REQUIRE_EQ(protected_datagrams.size(), 3U);
    std::array<std::uint32_t, 3> protected_sequences {};
    for (std::size_t index = 0; index < protected_datagrams.size(); ++index) {
        const auto decoded = decode_packet(protected_datagrams[index]);
        REQUIRE(decoded);
        protected_sequences[index] = decoded.packet.data.sequence.value();
    }
    REQUIRE_EQ(protected_sequences[0], SequenceNumber::mask - 1U);
    REQUIRE_EQ(protected_sequences[1], SequenceNumber::mask);
    REQUIRE_EQ(protected_sequences[2], 0U);

    auto forged = protected_datagrams.front();
    forged.back() ^= std::byte {0x01};
    const auto forged_packet = decode_packet(forged);
    REQUIRE(forged_packet);
    receiver.process_packet(forged_packet.packet, sender_endpoint);
    REQUIRE(!receiver.broken());

    for (const auto& datagram : protected_datagrams) {
        const auto decoded = decode_packet(datagram);
        REQUIRE(decoded);
        receiver.process_packet(decoded.packet, sender_endpoint);
    }

    std::array<std::byte, 3> first {};
    const auto first_read = receiver.receive_stream(first, false, -1);
    REQUIRE_EQ(first_read.status, MessageIoStatus::success);
    REQUIRE_EQ(first_read.bytes, first.size());
    REQUIRE(std::equal(first.begin(), first.end(), input.begin()));

    std::array<std::byte, 8> remainder {};
    const auto second_read = receiver.receive_stream(remainder, false, -1);
    REQUIRE_EQ(second_read.status, MessageIoStatus::success);
    REQUIRE_EQ(second_read.bytes, 7U);
    REQUIRE(std::equal(
        remainder.begin(), remainder.begin() + 7, input.begin() + 3));

    const auto sender_statistics = sender.statistics(false, true);
    const auto receiver_statistics = receiver.statistics(false, true);
    REQUIRE_EQ(sender_statistics.total.sent.payload_bytes, input.size());
    REQUIRE_EQ(receiver_statistics.total.received.payload_bytes, input.size());
    REQUIRE_EQ(receiver_statistics.total.receiver_undecryptable.packets, 1U);
}
#endif

TEST(compat_runtime_uses_the_directly_selected_file_controller)
{
    const auto channel =
        std::make_shared<DatagramChannel>();
    CapturedDatagrams output;
    channel->set_send_hook_for_testing(
        capture_datagram, &output);
    SocketOptions options;
    REQUIRE_EQ(options.set_congestion_controller("file"),
        Error::none);
    REQUIRE_EQ(options.transmission_type(),
        TransmissionType::live);
    REQUIRE(options.message_api());

    std::uint64_t now = 100'000;
    ConnectionRuntime runtime{{
        .channel = channel,
        .peer = {
            .address = {192, 0, 2, 41},
            .port = 16'001,
        },
        .peer_socket_id = 410,
        .initial_sequence = SequenceNumber{100},
        .flow_window_packets = 256,
        .options = options,
        .origin = ConnectionRuntime::Clock::now(),
        .now_function = injected_now,
        .now_context = &now,
    }};

    const auto snapshot =
        runtime.statistics(false, true);
    REQUIRE_EQ(snapshot.instantaneous
            .congestion_window_packets,
        16U);
    REQUIRE(snapshot.instantaneous
            .packet_send_period_microseconds > 0.0);
}

TEST(compat_runtime_file_mode_retransmits_after_injected_sender_rto)
{
    const auto channel = std::make_shared<DatagramChannel>();
    CapturedDatagrams output;
    channel->set_send_hook_for_testing(
        capture_datagram, &output);
    const Ipv4Endpoint peer{
        .address = {192, 0, 2, 42},
        .port = 16'002,
    };
    SocketOptions options;
    REQUIRE_EQ(options.set(
                   SocketOption::transmission_type,
                   static_cast<std::int64_t>(
                       TransmissionType::file)),
        Error::none);
    REQUIRE_EQ(options.set(
                   SocketOption::maximum_payload_size, 4),
        Error::none);
    // Stay below the independent one-second keepalive deadline so this test
    // observes only sender-RTO traffic.
    std::uint64_t now = 100'000;
    ConnectionRuntime runtime{{
        .channel = channel,
        .peer = peer,
        .peer_socket_id = 420,
        .initial_sequence =
            SequenceNumber{SequenceNumber::mask},
        .flow_window_packets = 256,
        .options = options,
        .origin = ConnectionRuntime::Clock::now(),
        .now_function = injected_now,
        .now_context = &now,
    }};

    const std::array<std::byte, 4> input{
        std::byte{'s'}, std::byte{'r'},
        std::byte{'t'}, std::byte{'!'}};
    REQUIRE_EQ(runtime.queue_stream(input, false, -1).bytes,
        input.size());
    (void)runtime.poll();
    const auto original_datagrams = take_datagrams(output);
    REQUIRE_EQ(original_datagrams.size(), 1U);
    const auto original = decode_packet(original_datagrams[0]);
    REQUIRE(original);
    REQUIRE_EQ(original.packet.data.sequence,
        SequenceNumber{SequenceNumber::mask});
    REQUIRE(!original.packet.data.retransmitted);

    now += 329'999;
    (void)runtime.poll();
    REQUIRE_EQ(take_datagrams(output).size(), 0U);
    ++now;
    (void)runtime.poll();
    const auto timeout_datagrams = take_datagrams(output);
    REQUIRE_EQ(timeout_datagrams.size(), 1U);
    const auto retransmission = decode_packet(timeout_datagrams[0]);
    REQUIRE(retransmission);
    REQUIRE_EQ(retransmission.packet.data.sequence,
        SequenceNumber{SequenceNumber::mask});
    REQUIRE(retransmission.packet.data.retransmitted);
}

TEST(compat_runtime_live_periodic_nak_falls_back_to_sender_tail_rto)
{
    const auto channel = std::make_shared<DatagramChannel>();
    CapturedDatagrams output;
    channel->set_send_hook_for_testing(capture_datagram, &output);
    const Ipv4Endpoint peer {
        .address = {192, 0, 2, 43},
        .port = 16'003,
    };
    SocketOptions options;
    REQUIRE_EQ(options.set(SocketOption::maximum_payload_size, 4), Error::none);
    std::uint64_t now = 100'000;
    ConnectionRuntime runtime {{
        .channel = channel,
        .peer = peer,
        .peer_socket_id = 430,
        .initial_sequence = SequenceNumber {700},
        .flow_window_packets = 256,
        .options = options,
        .negotiated_options =
            {
                .periodic_nak = true,
                .retransmit_flag = true,
            },
        .origin = ConnectionRuntime::Clock::now(),
        .now_function = injected_now,
        .now_context = &now,
    }};

    const std::array<std::byte, 4> input {
        std::byte {'t'}, std::byte {'a'}, std::byte {'i'}, std::byte {'l'}};
    REQUIRE_EQ(runtime.queue_message(input, 0, true, false, -1).status,
        MessageIoStatus::success);
    (void)runtime.poll();
    const auto original_datagrams = take_datagrams(output);
    REQUIRE_EQ(original_datagrams.size(), 1U);
    const auto original = decode_packet(original_datagrams[0]);
    REQUIRE(original);
    REQUIRE_EQ(original.packet.data.sequence, SequenceNumber {700});
    REQUIRE(!original.packet.data.retransmitted);

    now += 329'999;
    (void)runtime.poll();
    REQUIRE_EQ(take_datagrams(output).size(), 0U);
    ++now;
    (void)runtime.poll();
    const auto timeout_datagrams = take_datagrams(output);
    REQUIRE_EQ(timeout_datagrams.size(), 1U);
    const auto retransmission = decode_packet(timeout_datagrams[0]);
    REQUIRE(retransmission);
    REQUIRE_EQ(retransmission.packet.data.sequence, SequenceNumber {700});
    REQUIRE(retransmission.packet.data.retransmitted);
}

TEST(compat_runtime_row_fec_reconstructs_a_dropped_source_packet)
{
    const auto caller_channel =
        std::make_shared<DatagramChannel>();
    const auto listener_channel =
        std::make_shared<DatagramChannel>();
    CapturedDatagrams caller_output;
    CapturedDatagrams listener_output;
    caller_channel->set_send_hook_for_testing(
        capture_datagram, &caller_output);
    listener_channel->set_send_hook_for_testing(
        capture_datagram, &listener_output);
    const Ipv4Endpoint caller_endpoint{
        .address = {192, 0, 2, 31},
        .port = 14'101,
    };
    const Ipv4Endpoint listener_endpoint{
        .address = {192, 0, 2, 32},
        .port = 14'102,
    };

    SocketOptions options;
    REQUIRE_EQ(options.set_packet_filter(
                   "fec,cols:2,rows:1,arq:always"),
        Error::none);
    std::uint64_t caller_now = 100'000;
    std::uint64_t listener_now = 100'000;
    const auto origin = ConnectionRuntime::Clock::now();
    ConnectionRuntime caller{{
        .channel = caller_channel,
        .peer = listener_endpoint,
        .peer_socket_id = 320,
        .initial_sequence = SequenceNumber{500},
        .flow_window_packets = 256,
        .options = options,
        .origin = origin,
        .now_function = injected_now,
        .now_context = &caller_now,
    }};
    ConnectionRuntime listener{{
        .channel = listener_channel,
        .peer = caller_endpoint,
        .peer_socket_id = 310,
        .initial_sequence = SequenceNumber{500},
        .flow_window_packets = 256,
        .options = options,
        .origin = origin,
        .now_function = injected_now,
        .now_context = &listener_now,
    }};

    const std::array first{std::byte{'A'}};
    const std::array second{std::byte{'B'}};
    REQUIRE_EQ(caller.queue_message(
                   first, 0, false, false, -1).status,
        MessageIoStatus::success);
    REQUIRE_EQ(caller.queue_message(
                   second, 0, false, false, -1).status,
        MessageIoStatus::success);

    (void)caller.poll();
    caller_now += 10U;
    (void)caller.poll();
    caller_now += 10U;
    (void)caller.poll();
    const auto outgoing =
        take_datagrams(caller_output);
    REQUIRE_EQ(outgoing.size(), 3U);

    std::size_t source_packets = 0;
    std::size_t filter_packets = 0;
    // Deliver parity before the surviving source packet. This exercises the
    // receive-batch path where that source completes reconstruction and the
    // transient gap must not escape as an unnecessary NAK.
    for (const auto& datagram : outgoing) {
        const auto decoded = decode_packet(datagram);
        REQUIRE(decoded);
        if (decoded.packet.data.message_number == 0U) {
            ++filter_packets;
            listener.process_packet(
                decoded.packet, caller_endpoint);
        } else {
            ++source_packets;
        }
    }
    for (const auto& datagram : outgoing) {
        const auto decoded = decode_packet(datagram);
        REQUIRE(decoded);
        if (decoded.packet.data.message_number != 0U
            && decoded.packet.data.sequence
                != SequenceNumber{500}) {
            listener.process_packet(
                decoded.packet, caller_endpoint);
        }
    }
    REQUIRE_EQ(source_packets, 2U);
    REQUIRE_EQ(filter_packets, 1U);

    std::array<std::byte, 8> received_bytes{};
    const auto recovered = listener.receive_message(
        received_bytes, false, -1);
    REQUIRE_EQ(recovered.status,
        MessageIoStatus::success);
    REQUIRE_EQ(recovered.first_sequence,
        SequenceNumber{500});
    REQUIRE_EQ(recovered.bytes, 1U);
    REQUIRE_EQ(received_bytes[0], std::byte{'A'});
    const auto following = listener.receive_message(
        received_bytes, false, -1);
    REQUIRE_EQ(following.status,
        MessageIoStatus::success);
    REQUIRE_EQ(following.first_sequence,
        SequenceNumber{501});
    REQUIRE_EQ(following.bytes, 1U);
    REQUIRE_EQ(received_bytes[0], std::byte{'B'});

    const auto sender_statistics =
        caller.statistics(false, true);
    REQUIRE_EQ(sender_statistics.total.sent.packets,
        3U);
    REQUIRE_EQ(
        sender_statistics.total.sent_unique.packets,
        2U);
    REQUIRE_EQ(
        sender_statistics.total.sender_filter_extra,
        1U);
    const auto receiver_statistics =
        listener.statistics(false, true);
    REQUIRE_EQ(
        receiver_statistics.total.received.packets,
        2U);
    REQUIRE_EQ(
        receiver_statistics.total.received_unique.packets,
        2U);
    REQUIRE_EQ(
        receiver_statistics.total.receiver_filter_extra,
        1U);
    REQUIRE_EQ(
        receiver_statistics.total.receiver_filter_supply,
        1U);
    for (const auto& datagram :
         take_datagrams(listener_output)) {
        const auto decoded = decode_packet(datagram);
        REQUIRE(decoded);
        REQUIRE(decoded.packet.kind
            != PacketKind::control
            || decoded.packet.control.type
                != ControlType::
                    negative_acknowledgement);
    }
}

TEST(compat_runtime_onreq_fec_reports_only_expired_row_losses)
{
    const auto channel =
        std::make_shared<DatagramChannel>();
    CapturedDatagrams output;
    channel->set_send_hook_for_testing(
        capture_datagram, &output);
    const Ipv4Endpoint peer{
        .address = {192, 0, 2, 36},
        .port = 14'106,
    };
    SocketOptions options;
    REQUIRE_EQ(options.set_packet_filter(
                   "fec,cols:4,rows:1,arq:onreq"),
        Error::none);
    std::uint64_t now = 300'000;
    ConnectionRuntime runtime{{
        .channel = channel,
        .peer = peer,
        .peer_socket_id = 360,
        .initial_sequence = SequenceNumber{100},
        .flow_window_packets = 256,
        .options = options,
        .negotiated_options = {
            .periodic_nak = true,
            .retransmit_flag = true,
        },
        .origin = ConnectionRuntime::Clock::now(),
        .now_function = injected_now,
        .now_context = &now,
    }};
    const std::array payload{std::byte{'x'}};
    const auto source = [&](std::uint32_t sequence) {
        return PacketView{
            .kind = PacketKind::data,
            .data = {
                .sequence = SequenceNumber{sequence},
                .message_number = sequence + 1U,
                .boundary = MessageBoundary::solo,
                .destination_socket_id = 360,
            },
            .payload = payload,
        };
    };

    for (const std::uint32_t sequence :
         {102U, 103U, 104U, 105U}) {
        runtime.process_packet(source(sequence), peer);
        ++now;
    }
    for (const auto& datagram :
         take_datagrams(output)) {
        const auto decoded = decode_packet(datagram);
        REQUIRE(decoded);
        REQUIRE(decoded.packet.kind
                != PacketKind::control
            || decoded.packet.control.type
                != ControlType::
                    negative_acknowledgement);
    }

    runtime.process_packet(source(106U), peer);
    std::size_t negative_acknowledgements = 0;
    for (const auto& datagram :
         take_datagrams(output)) {
        const auto decoded = decode_packet(datagram);
        REQUIRE(decoded);
        if (decoded.packet.kind != PacketKind::control
            || decoded.packet.control.type
                != ControlType::
                    negative_acknowledgement) {
            continue;
        }
        ++negative_acknowledgements;
        const auto loss =
            decode_loss_range(decoded.packet.payload);
        REQUIRE(loss);
        REQUIRE_EQ(
            loss.range.first, SequenceNumber{100});
        REQUIRE_EQ(
            loss.range.last, SequenceNumber{101});
        REQUIRE_EQ(
            loss.bytes_consumed,
            decoded.packet.payload.size());
    }
    REQUIRE_EQ(negative_acknowledgements, 1U);

    const auto statistics =
        runtime.statistics(false, true);
    REQUIRE_EQ(
        statistics.total.receiver_filter_loss, 2U);
}

TEST(compat_runtime_column_fec_reconstructs_a_dropped_source_packet)
{
    const auto caller_channel =
        std::make_shared<DatagramChannel>();
    const auto listener_channel =
        std::make_shared<DatagramChannel>();
    CapturedDatagrams caller_output;
    CapturedDatagrams listener_output;
    caller_channel->set_send_hook_for_testing(
        capture_datagram, &caller_output);
    listener_channel->set_send_hook_for_testing(
        capture_datagram, &listener_output);
    const Ipv4Endpoint caller_endpoint{
        .address = {192, 0, 2, 37},
        .port = 14'107,
    };
    const Ipv4Endpoint listener_endpoint{
        .address = {192, 0, 2, 38},
        .port = 14'108,
    };

    SocketOptions options;
    REQUIRE_EQ(options.set(
                   SocketOption::maximum_payload_size,
                   4),
        Error::none);
    REQUIRE_EQ(options.set_packet_filter(
                   "fec,cols:2,rows:-2,"
                   "layout:even,arq:onreq"),
        Error::none);
    std::uint64_t caller_now = 400'000;
    std::uint64_t listener_now = 400'000;
    const auto origin = ConnectionRuntime::Clock::now();
    ConnectionRuntime caller{{
        .channel = caller_channel,
        .peer = listener_endpoint,
        .peer_socket_id = 380,
        .initial_sequence = SequenceNumber{500},
        .flow_window_packets = 256,
        .options = options,
        .negotiated_options = {
            .periodic_nak = true,
            .retransmit_flag = true,
        },
        .origin = origin,
        .now_function = injected_now,
        .now_context = &caller_now,
    }};
    ConnectionRuntime listener{{
        .channel = listener_channel,
        .peer = caller_endpoint,
        .peer_socket_id = 370,
        .initial_sequence = SequenceNumber{500},
        .flow_window_packets = 256,
        .options = options,
        .negotiated_options = {
            .periodic_nak = true,
            .retransmit_flag = true,
        },
        .origin = origin,
        .now_function = injected_now,
        .now_context = &listener_now,
    }};

    for (const std::byte value :
         {std::byte{'A'}, std::byte{'B'},
             std::byte{'C'}, std::byte{'D'}}) {
        const std::array payload{value};
        REQUIRE_EQ(caller.queue_message(
                       payload, 0, false, false, -1)
                       .status,
            MessageIoStatus::success);
    }
    for (std::size_t attempt = 0;
         attempt < 20U; ++attempt) {
        (void)caller.poll();
        caller_now += 10U;
    }
    const auto outgoing =
        take_datagrams(caller_output);
    REQUIRE_EQ(outgoing.size(), 6U);

    std::size_t source_packets = 0;
    std::size_t filter_packets = 0;
    for (const auto& datagram : outgoing) {
        const auto decoded = decode_packet(datagram);
        REQUIRE(decoded);
        if (decoded.packet.data.message_number == 0U) {
            ++filter_packets;
        } else {
            ++source_packets;
            if (decoded.packet.data.sequence
                == SequenceNumber{500}) {
                continue;
            }
        }
        listener.process_packet(
            decoded.packet, caller_endpoint);
        ++listener_now;
    }
    REQUIRE_EQ(source_packets, 4U);
    REQUIRE_EQ(filter_packets, 2U);

    std::array<std::byte, 4> received_bytes{};
    for (std::uint32_t index = 0;
         index < 4U; ++index) {
        const auto received =
            listener.receive_message(
                received_bytes, false, -1);
        REQUIRE_EQ(received.status,
            MessageIoStatus::success);
        REQUIRE_EQ(received.first_sequence,
            SequenceNumber{500U + index});
        REQUIRE_EQ(received.bytes, 1U);
        REQUIRE_EQ(
            received_bytes[0],
            static_cast<std::byte>(
                static_cast<unsigned char>('A')
                + index));
    }

    const auto sender_statistics =
        caller.statistics(false, true);
    REQUIRE_EQ(
        sender_statistics.total.sent.packets, 6U);
    REQUIRE_EQ(
        sender_statistics.total.sent_unique.packets,
        4U);
    REQUIRE_EQ(
        sender_statistics.total.sender_filter_extra,
        2U);
    const auto receiver_statistics =
        listener.statistics(false, true);
    REQUIRE_EQ(
        receiver_statistics
            .total.receiver_filter_extra,
        2U);
    REQUIRE_EQ(
        receiver_statistics
            .total.receiver_filter_supply,
        1U);
    REQUIRE_EQ(
        receiver_statistics
            .total.receiver_filter_loss,
        0U);
    for (const auto& datagram :
         take_datagrams(listener_output)) {
        const auto decoded = decode_packet(datagram);
        REQUIRE(decoded);
        REQUIRE(decoded.packet.kind
                != PacketKind::control
            || decoded.packet.control.type
                != ControlType::
                    negative_acknowledgement);
    }
}

TEST(compat_runtime_column_fec_onreq_reports_expired_groups)
{
    const auto channel =
        std::make_shared<DatagramChannel>();
    CapturedDatagrams output;
    channel->set_send_hook_for_testing(
        capture_datagram, &output);
    const Ipv4Endpoint peer{
        .address = {192, 0, 2, 39},
        .port = 14'109,
    };
    SocketOptions options;
    REQUIRE_EQ(options.set_packet_filter(
                   "fec,cols:3,rows:-2,"
                   "layout:even,arq:onreq"),
        Error::none);
    std::uint64_t now = 500'000;
    ConnectionRuntime runtime{{
        .channel = channel,
        .peer = peer,
        .peer_socket_id = 390,
        .initial_sequence = SequenceNumber{600},
        .flow_window_packets = 256,
        .options = options,
        .negotiated_options = {
            .periodic_nak = true,
            .retransmit_flag = true,
        },
        .origin = ConnectionRuntime::Clock::now(),
        .now_function = injected_now,
        .now_context = &now,
    }};
    const std::array payload{std::byte{'x'}};
    const auto source = [&](std::uint32_t sequence) {
        return PacketView{
            .kind = PacketKind::data,
            .data = {
                .sequence = SequenceNumber{sequence},
                .message_number = sequence + 1U,
                .boundary = MessageBoundary::solo,
                .destination_socket_id = 390,
            },
            .payload = payload,
        };
    };

    for (const std::uint32_t sequence :
         {601U, 603U, 604U}) {
        runtime.process_packet(source(sequence), peer);
        ++now;
    }
    for (const auto& datagram :
         take_datagrams(output)) {
        const auto decoded = decode_packet(datagram);
        REQUIRE(decoded);
        REQUIRE(decoded.packet.kind
                != PacketKind::control
            || decoded.packet.control.type
                != ControlType::
                    negative_acknowledgement);
    }

    runtime.process_packet(source(606U), peer);
    std::vector<SequenceNumber> losses;
    std::size_t negative_acknowledgements = 0;
    for (const auto& datagram :
         take_datagrams(output)) {
        const auto decoded = decode_packet(datagram);
        REQUIRE(decoded);
        if (decoded.packet.kind != PacketKind::control
            || decoded.packet.control.type
                != ControlType::
                    negative_acknowledgement) {
            continue;
        }
        ++negative_acknowledgements;
        auto remaining = decoded.packet.payload;
        while (!remaining.empty()) {
            const auto loss =
                decode_loss_range(remaining);
            REQUIRE(loss);
            REQUIRE_EQ(
                loss.range.first, loss.range.last);
            losses.push_back(loss.range.first);
            remaining = remaining.subspan(
                loss.bytes_consumed);
        }
    }
    REQUIRE_EQ(negative_acknowledgements, 1U);
    const std::vector<SequenceNumber> expected{
        SequenceNumber{600},
        SequenceNumber{602},
        SequenceNumber{605},
    };
    REQUIRE_EQ(losses, expected);
    const auto statistics =
        runtime.statistics(false, true);
    REQUIRE_EQ(
        statistics.total.receiver_filter_loss, 3U);
}

TEST(compat_runtime_matrix_fec_recovers_a_recursive_loss_chain)
{
    const auto caller_channel =
        std::make_shared<DatagramChannel>();
    const auto listener_channel =
        std::make_shared<DatagramChannel>();
    CapturedDatagrams caller_output;
    CapturedDatagrams listener_output;
    caller_channel->set_send_hook_for_testing(
        capture_datagram, &caller_output);
    listener_channel->set_send_hook_for_testing(
        capture_datagram, &listener_output);
    const Ipv4Endpoint caller_endpoint{
        .address = {192, 0, 2, 40},
        .port = 14'110,
    };
    const Ipv4Endpoint listener_endpoint{
        .address = {192, 0, 2, 41},
        .port = 14'111,
    };

    SocketOptions options;
    REQUIRE_EQ(options.set(
                   SocketOption::maximum_payload_size,
                   4),
        Error::none);
    REQUIRE_EQ(options.set_packet_filter(
                   "fec,cols:3,rows:3,"
                   "layout:even,arq:onreq"),
        Error::none);
    std::uint64_t caller_now = 600'000;
    std::uint64_t listener_now = 600'000;
    const auto origin = ConnectionRuntime::Clock::now();
    ConnectionRuntime caller{{
        .channel = caller_channel,
        .peer = listener_endpoint,
        .peer_socket_id = 410,
        .initial_sequence = SequenceNumber{700},
        .flow_window_packets = 256,
        .options = options,
        .negotiated_options = {
            .periodic_nak = true,
            .retransmit_flag = true,
        },
        .origin = origin,
        .now_function = injected_now,
        .now_context = &caller_now,
    }};
    ConnectionRuntime listener{{
        .channel = listener_channel,
        .peer = caller_endpoint,
        .peer_socket_id = 400,
        .initial_sequence = SequenceNumber{700},
        .flow_window_packets = 256,
        .options = options,
        .negotiated_options = {
            .periodic_nak = true,
            .retransmit_flag = true,
        },
        .origin = origin,
        .now_function = injected_now,
        .now_context = &listener_now,
    }};

    for (std::uint32_t index = 0;
         index < 9U; ++index) {
        const std::array payload{
            static_cast<std::byte>(
                static_cast<unsigned char>('A')
                + index)};
        REQUIRE_EQ(caller.queue_message(
                       payload, 0, false, false, -1)
                       .status,
            MessageIoStatus::success);
    }
    for (std::size_t attempt = 0;
         attempt < 50U; ++attempt) {
        (void)caller.poll();
        caller_now += 10U;
    }
    const auto outgoing =
        take_datagrams(caller_output);
    REQUIRE_EQ(outgoing.size(), 15U);

    std::size_t source_packets = 0;
    std::size_t filter_packets = 0;
    for (const auto& datagram : outgoing) {
        const auto decoded =
            decode_packet(datagram);
        REQUIRE(decoded);
        if (decoded.packet.data.message_number == 0U) {
            ++filter_packets;
        } else {
            ++source_packets;
            const std::uint32_t index =
                (decoded.packet.data.sequence.value()
                    - 700U)
                & SequenceNumber::mask;
            if (index == 0U || index == 1U
                || index == 3U || index == 5U
                || index == 8U) {
                continue;
            }
        }
        listener.process_packet(
            decoded.packet, caller_endpoint);
        ++listener_now;
    }
    REQUIRE_EQ(source_packets, 9U);
    REQUIRE_EQ(filter_packets, 6U);

    std::array<std::byte, 4> received_bytes{};
    for (std::uint32_t index = 0;
         index < 9U; ++index) {
        const auto received =
            listener.receive_message(
                received_bytes, false, -1);
        REQUIRE_EQ(received.status,
            MessageIoStatus::success);
        REQUIRE_EQ(received.first_sequence,
            SequenceNumber{700U + index});
        REQUIRE_EQ(received.bytes, 1U);
        REQUIRE_EQ(
            received_bytes[0],
            static_cast<std::byte>(
                static_cast<unsigned char>('A')
                + index));
    }

    const auto sender_statistics =
        caller.statistics(false, true);
    REQUIRE_EQ(
        sender_statistics.total.sent.packets, 15U);
    REQUIRE_EQ(
        sender_statistics.total.sent_unique.packets,
        9U);
    REQUIRE_EQ(
        sender_statistics.total.sender_filter_extra,
        6U);
    const auto receiver_statistics =
        listener.statistics(false, true);
    REQUIRE_EQ(
        receiver_statistics
            .total.receiver_filter_extra,
        6U);
    REQUIRE_EQ(
        receiver_statistics
            .total.receiver_filter_supply,
        5U);
    REQUIRE_EQ(
        receiver_statistics
            .total.receiver_filter_loss,
        0U);
    for (const auto& datagram :
         take_datagrams(listener_output)) {
        const auto decoded =
            decode_packet(datagram);
        REQUIRE(decoded);
        REQUIRE(decoded.packet.kind
                != PacketKind::control
            || decoded.packet.control.type
                != ControlType::
                    negative_acknowledgement);
    }
}

TEST(compat_runtime_row_fec_reconstructs_encrypted_wire_payload)
{
    const auto caller_channel =
        std::make_shared<DatagramChannel>();
    const auto listener_channel =
        std::make_shared<DatagramChannel>();
    CapturedDatagrams caller_output;
    CapturedDatagrams listener_output;
    caller_channel->set_send_hook_for_testing(
        capture_datagram, &caller_output);
    listener_channel->set_send_hook_for_testing(
        capture_datagram, &listener_output);
    const Ipv4Endpoint caller_endpoint{
        .address = {192, 0, 2, 33},
        .port = 14'103,
    };
    const Ipv4Endpoint listener_endpoint{
        .address = {192, 0, 2, 34},
        .port = 14'104,
    };

    const CryptoConfiguration crypto_configuration{
        .passphrase = "fec encrypted fixture",
        .key_length = 16,
        .refresh_rate_packets = 100,
        .preannouncement_packets = 10,
    };
    auto caller_crypto =
        std::make_shared<CryptoSession>(
            crypto_configuration);
    auto listener_crypto =
        std::make_shared<CryptoSession>(
            crypto_configuration);
    REQUIRE_EQ(caller_crypto->start_initiator(),
        Error::none);
    REQUIRE_EQ(listener_crypto->accept_key_material(
                   caller_crypto->pending_key_material(),
                   true),
        Error::none);
    REQUIRE_EQ(caller_crypto->acknowledge_key_material(
                   listener_crypto->key_material_response(),
                   true),
        Error::none);
    confirm_directional_test_keys(*caller_crypto, *listener_crypto);

    SocketOptions options;
    REQUIRE_EQ(options.set(
                   SocketOption::maximum_payload_size, 16),
        Error::none);
    REQUIRE_EQ(options.set_packet_filter(
                   "fec,cols:2,rows:1,arq:always"),
        Error::none);
    std::uint64_t caller_now = 200'000;
    const auto origin = ConnectionRuntime::Clock::now();
    ConnectionRuntime caller{{
        .channel = caller_channel,
        .peer = listener_endpoint,
        .peer_socket_id = 340,
        .initial_sequence = SequenceNumber{700},
        .flow_window_packets = 256,
        .options = options,
        .origin = origin,
        .crypto = caller_crypto,
        .now_function = injected_now,
        .now_context = &caller_now,
    }};
    ConnectionRuntime listener{{
        .channel = listener_channel,
        .peer = caller_endpoint,
        .peer_socket_id = 330,
        .initial_sequence = SequenceNumber{700},
        .flow_window_packets = 256,
        .options = options,
        .origin = origin,
        .crypto = listener_crypto,
    }};

    constexpr std::string_view first = "secret-A";
    constexpr std::string_view second = "secret-B";
    REQUIRE_EQ(caller.queue_message(
                   std::as_bytes(std::span{
                       first.data(), first.size()}),
                   0, false, false, -1)
                   .status,
        MessageIoStatus::success);
    REQUIRE_EQ(caller.queue_message(
                   std::as_bytes(std::span{
                       second.data(), second.size()}),
                   0, false, false, -1)
                   .status,
        MessageIoStatus::success);
    (void)caller.poll();
    caller_now += 10U;
    (void)caller.poll();
    caller_now += 10U;
    (void)caller.poll();

    const auto outgoing =
        take_datagrams(caller_output);
    REQUIRE_EQ(outgoing.size(), 3U);
    for (const auto& datagram : outgoing) {
        const auto decoded = decode_packet(datagram);
        REQUIRE(decoded);
        REQUIRE_EQ(decoded.packet.data.encryption_key,
            EncryptionKey::even);
        if (decoded.packet.data.message_number == 0U
            || decoded.packet.data.sequence
                != SequenceNumber{700}) {
            listener.process_packet(
                decoded.packet, caller_endpoint);
        }
    }

    std::array<std::byte, 32> received_bytes{};
    const auto recovered = listener.receive_message(
        received_bytes, false, -1);
    REQUIRE_EQ(recovered.status,
        MessageIoStatus::success);
    REQUIRE_EQ(recovered.bytes, first.size());
    REQUIRE(std::equal(
        std::as_bytes(std::span{
            first.data(), first.size()}).begin(),
        std::as_bytes(std::span{
            first.data(), first.size()}).end(),
        received_bytes.begin()));
    const auto following = listener.receive_message(
        received_bytes, false, -1);
    REQUIRE_EQ(following.status,
        MessageIoStatus::success);
    REQUIRE_EQ(following.bytes, second.size());
    REQUIRE(std::equal(
        std::as_bytes(std::span{
            second.data(), second.size()}).begin(),
        std::as_bytes(std::span{
            second.data(), second.size()}).end(),
        received_bytes.begin()));
    REQUIRE(!listener.broken());
}

TEST(compat_runtime_gcm_row_fec_authenticates_reconstructed_wire_payload)
{
    const auto sender_channel = std::make_shared<DatagramChannel>();
    const auto receiver_channel = std::make_shared<DatagramChannel>();
    CapturedDatagrams sender_output;
    CapturedDatagrams receiver_output;
    sender_channel->set_send_hook_for_testing(capture_datagram, &sender_output);
    receiver_channel->set_send_hook_for_testing(
        capture_datagram, &receiver_output);
    const Ipv4Endpoint sender_endpoint {
        .address = {192, 0, 2, 73},
        .port = 14'503,
    };
    const Ipv4Endpoint receiver_endpoint {
        .address = {192, 0, 2, 74},
        .port = 14'504,
    };
    const CryptoConfiguration crypto_configuration {
        .passphrase = "authenticated row fec fixture",
        .mode = CryptoMode::aes_gcm,
        .enable_aes_gcm = true,
        .key_length = 32,
        .refresh_rate_packets = 64,
        .preannouncement_packets = 8,
    };
    auto sender_crypto = std::make_shared<CryptoSession>(crypto_configuration);
    auto receiver_crypto =
        std::make_shared<CryptoSession>(crypto_configuration);
    REQUIRE_EQ(sender_crypto->start_initiator(), Error::none);
    REQUIRE_EQ(receiver_crypto->accept_key_material(
                   sender_crypto->pending_key_material(), true),
        Error::none);
    REQUIRE_EQ(sender_crypto->acknowledge_key_material(
                   receiver_crypto->key_material_response(), true),
        Error::none);
    confirm_directional_test_keys(*sender_crypto, *receiver_crypto);

    SocketOptions options;
    REQUIRE_EQ(
        options.set(SocketOption::maximum_payload_size, 16), Error::none);
    REQUIRE_EQ(
        options.set_packet_filter("fec,cols:2,rows:1,arq:always"), Error::none);
    const auto origin = ConnectionRuntime::Clock::now();
    std::uint64_t sender_now = 1'600'000;
    std::uint64_t receiver_now = 1'600'000;
    ConnectionRuntime sender {{
        .channel = sender_channel,
        .peer = receiver_endpoint,
        .peer_socket_id = 740,
        .initial_sequence = SequenceNumber {1'000},
        .flow_window_packets = 256,
        .options = options,
        .origin = origin,
        .crypto = sender_crypto,
        .now_function = injected_now,
        .now_context = &sender_now,
    }};
    ConnectionRuntime receiver {{
        .channel = receiver_channel,
        .peer = sender_endpoint,
        .peer_socket_id = 730,
        .initial_sequence = SequenceNumber {1'000},
        .flow_window_packets = 256,
        .options = options,
        .origin = origin,
        .crypto = receiver_crypto,
        .now_function = injected_now,
        .now_context = &receiver_now,
    }};

    const auto send_row = [&](std::byte first, std::byte second) {
        REQUIRE_EQ(
            sender.queue_message(std::span {&first, 1U}, 0, false, false, -1)
                .status,
            MessageIoStatus::success);
        REQUIRE_EQ(
            sender.queue_message(std::span {&second, 1U}, 0, false, false, -1)
                .status,
            MessageIoStatus::success);
        for (std::size_t attempt = 0; attempt < 12U; ++attempt) {
            (void)sender.poll();
            sender_now += 10U;
        }
        REQUIRE(!sender.broken());
        return take_datagrams(sender_output);
    };

    auto first_row = send_row(std::byte {'A'}, std::byte {'B'});
    REQUIRE(first_row.size() >= 3U);
    std::vector<std::byte> first_source;
    std::vector<std::byte> second_source;
    std::vector<std::byte> parity;
    for (const auto& datagram : first_row) {
        const auto decoded = decode_packet(datagram);
        REQUIRE(decoded);
        if (decoded.packet.kind != PacketKind::data) {
            receiver.process_packet(decoded.packet, sender_endpoint);
            continue;
        }
        if (decoded.packet.data.message_number == 0U) {
            parity = datagram;
        } else if (decoded.packet.data.sequence == SequenceNumber {1'000}) {
            first_source = datagram;
        } else {
            second_source = datagram;
        }
    }
    REQUIRE(!first_source.empty());
    REQUIRE(!second_source.empty());
    REQUIRE(!parity.empty());
    const auto first_packet = decode_packet(first_source);
    const auto second_packet = decode_packet(second_source);
    const auto parity_packet = decode_packet(parity);
    REQUIRE(first_packet);
    REQUIRE(second_packet);
    REQUIRE(parity_packet);
    const auto parity_fields =
        decode_fec_control_payload(parity_packet.packet.payload);
    REQUIRE(parity_fields);
    REQUIRE_EQ(parity_fields.payload_recovery.size(),
        16U + srt_gcm_authentication_tag_size);
    for (std::size_t index = 0; index < second_packet.packet.payload.size();
        ++index) {
        REQUIRE_EQ(parity_fields.payload_recovery[index],
            first_packet.packet.payload[index]
                ^ second_packet.packet.payload[index]);
    }

    receiver.process_packet(first_packet.packet, sender_endpoint);
    auto corrupted_parity = parity;
    corrupted_parity[packet_header_size + fec_filter_header_size + 10U] ^=
        std::byte {0x01};
    const auto corrupted_packet = decode_packet(corrupted_parity);
    REQUIRE(corrupted_packet);
    receiver.process_packet(corrupted_packet.packet, sender_endpoint);

    std::array<std::byte, 16> received {};
    const auto first = receiver.receive_message(received, false, -1);
    REQUIRE_EQ(first.status, MessageIoStatus::success);
    REQUIRE_EQ(first.message_number, 1U);
    REQUIRE_EQ(received[0], std::byte {'A'});
    REQUIRE_EQ(receiver.receive_message(received, false, -1).status,
        MessageIoStatus::would_block);
    REQUIRE_EQ(
        receiver.statistics(false, true).total.receiver_undecryptable.packets,
        1U);

    // An authentic late source remains recoverable after a reconstructed tag
    // failure; no forged plaintext or terminal connection state escapes.
    receiver.process_packet(second_packet.packet, sender_endpoint);
    const auto second = receiver.receive_message(received, false, -1);
    REQUIRE_EQ(second.status, MessageIoStatus::success);
    REQUIRE_EQ(second.message_number, 2U);
    REQUIRE_EQ(received[0], std::byte {'B'});
    REQUIRE(!receiver.broken());

    // Recover a later message number rather than the upstream decoder's
    // constant one. Its exact header is required for GCM authentication.
    const auto second_row = send_row(std::byte {'C'}, std::byte {'D'});
    REQUIRE(second_row.size() >= 3U);
    for (const auto& datagram : second_row) {
        const auto decoded = decode_packet(datagram);
        REQUIRE(decoded);
        if (decoded.packet.kind != PacketKind::data) {
            receiver.process_packet(decoded.packet, sender_endpoint);
            continue;
        }
        if (decoded.packet.data.message_number == 0U
            || decoded.packet.data.sequence == SequenceNumber {1'002}) {
            receiver.process_packet(decoded.packet, sender_endpoint);
        }
    }
    const auto third = receiver.receive_message(received, false, -1);
    REQUIRE_EQ(third.status, MessageIoStatus::success);
    REQUIRE_EQ(third.message_number, 3U);
    REQUIRE_EQ(received[0], std::byte {'C'});
    const auto fourth = receiver.receive_message(received, false, -1);
    REQUIRE_EQ(fourth.status, MessageIoStatus::success);
    REQUIRE_EQ(fourth.message_number, 4U);
    REQUIRE_EQ(received[0], std::byte {'D'});
    REQUIRE_EQ(
        receiver.statistics(false, true).total.receiver_filter_supply, 1U);
}

TEST(compat_runtime_gcm_column_fec_reconstructs_before_authentication)
{
    constexpr std::array dropped {0U};
    exercise_gcm_fec_geometry(
        "fec,cols:2,rows:-2,layout:even,arq:onreq", 4U, dropped, 2U, 1U);
}

TEST(compat_runtime_gcm_matrix_fec_authenticates_recursive_recovery)
{
    constexpr std::array dropped {0U, 1U, 3U, 5U, 8U};
    exercise_gcm_fec_geometry(
        "fec,cols:3,rows:3,layout:even,arq:onreq", 9U, dropped, 6U, 5U);
}

TEST(compat_runtime_consumes_encrypted_fec_control_before_decryption)
{
    const auto channel =
        std::make_shared<DatagramChannel>();
    CapturedDatagrams output;
    channel->set_send_hook_for_testing(
        capture_datagram, &output);
    const Ipv4Endpoint peer{
        .address = {192, 0, 2, 35},
        .port = 14'105,
    };
    const CryptoConfiguration crypto_configuration{
        .passphrase = "control is recovery data",
        .key_length = 16,
    };
    auto crypto = std::make_shared<CryptoSession>(
        crypto_configuration);
    REQUIRE_EQ(crypto->start_initiator(), Error::none);

    SocketOptions options;
    REQUIRE_EQ(options.set(
                   SocketOption::maximum_payload_size,
                   4),
        Error::none);
    REQUIRE_EQ(options.set_packet_filter(
                   "fec,cols:2,rows:2,arq:always"),
        Error::none);
    ConnectionRuntime runtime{{
        .channel = channel,
        .peer = peer,
        .peer_socket_id = 350,
        .initial_sequence = SequenceNumber{800},
        .flow_window_packets = 256,
        .options = options,
        .origin = ConnectionRuntime::Clock::now(),
        .crypto = crypto,
    }};

    std::array<std::byte, 8> payload{};
    REQUIRE_EQ(encode_fec_control_header({
                   .group_index = -1,
               },
                   payload),
        Error::none);
    const PacketView control{
        .kind = PacketKind::data,
        .data = {
            .sequence = SequenceNumber{801},
            .message_number = 0,
            .boundary = MessageBoundary::solo,
            .encryption_key = EncryptionKey::even,
            .destination_socket_id = 350,
        },
        .payload = payload,
    };
    runtime.process_packet(control, peer);
    REQUIRE(!runtime.broken());
    const auto statistics =
        runtime.statistics(false, true);
    REQUIRE_EQ(statistics.total.received.packets, 1U);
    REQUIRE_EQ(
        statistics.total.receiver_filter_extra, 1U);
}

TEST(compat_runtime_encrypts_payloads_and_exchanges_rotation_keys)
{
    const auto caller_channel = std::make_shared<DatagramChannel>();
    const auto listener_channel = std::make_shared<DatagramChannel>();
    CapturedDatagrams caller_output;
    CapturedDatagrams listener_output;
    caller_channel->set_send_hook_for_testing(
        capture_datagram, &caller_output);
    listener_channel->set_send_hook_for_testing(
        capture_datagram, &listener_output);
    const Ipv4Endpoint caller_endpoint{
        .address = {192, 0, 2, 21},
        .port = 14'001,
    };
    const Ipv4Endpoint listener_endpoint{
        .address = {192, 0, 2, 22},
        .port = 14'002,
    };

    const CryptoConfiguration crypto_configuration{
        .passphrase = "correct horse battery",
        .key_length = 16,
        .refresh_rate_packets = 3,
        .preannouncement_packets = 1,
    };
    auto caller_crypto =
        std::make_shared<CryptoSession>(crypto_configuration);
    auto listener_crypto =
        std::make_shared<CryptoSession>(crypto_configuration);
    REQUIRE_EQ(caller_crypto->start_initiator(), Error::none);
    const std::vector<std::byte> initial_key_material(
        caller_crypto->pending_key_material().begin(),
        caller_crypto->pending_key_material().end());
    REQUIRE_EQ(listener_crypto->accept_key_material(
                   initial_key_material, true),
        Error::none);
    REQUIRE_EQ(caller_crypto->acknowledge_key_material(
                   listener_crypto->key_material_response(), true),
        Error::none);
    confirm_directional_test_keys(*caller_crypto, *listener_crypto);

    SocketOptions options;
    REQUIRE_EQ(options.set(
                   SocketOption::send_buffer_packets, 1),
        Error::none);
    const auto origin = ConnectionRuntime::Clock::now();
    std::uint64_t caller_now = 1'000'000;
    std::uint64_t listener_now = 1'000'000;
    ConnectionRuntime caller{{
        .channel = caller_channel,
        .peer = listener_endpoint,
        .peer_socket_id = 220,
        .initial_sequence =
            SequenceNumber{SequenceNumber::mask - 2U},
        .flow_window_packets = 256,
        .options = options,
        .origin = origin,
        .crypto = caller_crypto,
        .now_function = injected_now,
        .now_context = &caller_now,
    }};
    ConnectionRuntime listener{{
        .channel = listener_channel,
        .peer = caller_endpoint,
        .peer_socket_id = 110,
        .initial_sequence =
            SequenceNumber{SequenceNumber::mask - 2U},
        .flow_window_packets = 256,
        .options = options,
        .origin = origin,
        .crypto = listener_crypto,
        .now_function = injected_now,
        .now_context = &listener_now,
    }};

    std::vector<std::byte> forged_request = initial_key_material;
    forged_request.back() ^= std::byte{0x01};
    PacketView forged_request_packet;
    forged_request_packet.kind = PacketKind::control;
    forged_request_packet.control.type = ControlType::user_defined;
    forged_request_packet.control.subtype =
        key_material_request_subtype;
    forged_request_packet.payload = forged_request;
    listener.process_packet(
        forged_request_packet, caller_endpoint);
    REQUIRE(!listener.broken());
    REQUIRE_EQ(listener_crypto->receiver_state(),
        CryptoState::secured);

    const std::array<std::byte, 4> forged_response{
        std::byte{0}, std::byte{0}, std::byte{0},
        static_cast<std::byte>(CryptoState::bad_secret),
    };
    PacketView forged_response_packet;
    forged_response_packet.kind = PacketKind::control;
    forged_response_packet.control.type = ControlType::user_defined;
    forged_response_packet.control.subtype =
        key_material_response_subtype;
    forged_response_packet.payload = forged_response;
    caller.process_packet(
        forged_response_packet, listener_endpoint);
    REQUIRE(!caller.broken());
    REQUIRE_EQ(caller_crypto->sender_state(),
        CryptoState::secured);

    bool saw_request = false;
    bool saw_response = false;
    SequenceNumber expected_sequence{
        SequenceNumber::mask - 2U};
    const auto exchange_one = [&](std::string_view message,
                                  EncryptionKey expected_key) {
        caller_now += 1'000'000;
        const auto bytes = std::as_bytes(std::span{
            message.data(), message.size()});
        REQUIRE_EQ(caller.queue_message(
                       bytes, 0, true, false, -1).status,
            MessageIoStatus::success);
        (void)caller.poll();
        auto outgoing = take_datagrams(caller_output);
        bool saw_data = false;
        for (const auto& datagram : outgoing) {
            const auto decoded = decode_packet(datagram);
            REQUIRE(decoded);
            if (decoded.packet.kind == PacketKind::data) {
                saw_data = true;
                REQUIRE_EQ(decoded.packet.data.sequence,
                    expected_sequence);
                REQUIRE_EQ(decoded.packet.data.encryption_key,
                    expected_key);
                REQUIRE(!std::equal(bytes.begin(), bytes.end(),
                    decoded.packet.payload.begin()));
            } else if (decoded.packet.control.type
                    == ControlType::user_defined
                && decoded.packet.control.subtype
                    == key_material_request_subtype) {
                saw_request = true;
                const auto key_material =
                    decode_key_material(decoded.packet.payload);
                REQUIRE(key_material);
                REQUIRE_EQ(key_material.key_material.keys,
                    EncryptionKey::reserved);
            }
            listener.process_packet(
                decoded.packet, caller_endpoint);
        }
        REQUIRE(saw_data);
        expected_sequence = expected_sequence.next();
        listener_now += 10'000;
        (void)listener.poll();
        for (const auto& datagram :
            take_datagrams(listener_output)) {
            const auto decoded = decode_packet(datagram);
            REQUIRE(decoded);
            if (decoded.packet.kind == PacketKind::control
                && decoded.packet.control.type
                    == ControlType::user_defined
                && decoded.packet.control.subtype
                    == key_material_response_subtype) {
                saw_response = true;
            }
            caller.process_packet(
                decoded.packet, listener_endpoint);
        }
        deliver(caller_output, listener, caller_endpoint);
    };

    exchange_one("encrypted one", EncryptionKey::even);
    exchange_one("encrypted two", EncryptionKey::even);
    REQUIRE(saw_request);
    REQUIRE(saw_response);

    exchange_one("encrypted three", EncryptionKey::even);
    REQUIRE_EQ(caller_crypto->active_sender_key(),
        EncryptionKey::odd);
    exchange_one("encrypted four", EncryptionKey::odd);
    REQUIRE(!caller.broken());
    REQUIRE(!listener.broken());

    std::array<std::byte, 100> received{};
    for (std::size_t index = 0; index < 4U; ++index) {
        REQUIRE_EQ(listener.receive_message(
                       received, false, -1).status,
            MessageIoStatus::success);
    }

    caller_now += 1'000'000;
    constexpr std::string_view fifth_message = "encrypted five";
    REQUIRE_EQ(caller.queue_message(
                   std::as_bytes(std::span{
                       fifth_message.data(), fifth_message.size()}),
                   0, true, false, -1).status,
        MessageIoStatus::success);
    (void)caller.poll();
    auto fifth_output = take_datagrams(caller_output);
    bool dropped_initial_request = false;
    for (const auto& datagram : fifth_output) {
        const auto decoded = decode_packet(datagram);
        REQUIRE(decoded);
        if (decoded.packet.kind == PacketKind::control
            && decoded.packet.control.type
                == ControlType::user_defined
            && decoded.packet.control.subtype
                == key_material_request_subtype) {
            dropped_initial_request = true;
            continue;
        }
        if (decoded.packet.kind == PacketKind::data) {
            REQUIRE_EQ(decoded.packet.data.sequence,
                expected_sequence);
            expected_sequence = expected_sequence.next();
        }
        listener.process_packet(decoded.packet, caller_endpoint);
    }
    REQUIRE(dropped_initial_request);
    listener_now += 10'000;
    (void)listener.poll();
    deliver(listener_output, caller, listener_endpoint);

    constexpr std::string_view sixth_message = "encrypted six";
    REQUIRE_EQ(caller.queue_message(
                   std::as_bytes(std::span{
                       sixth_message.data(), sixth_message.size()}),
                   0, true, false, -1).status,
        MessageIoStatus::success);

    caller_now -= 100'000;
    (void)caller.poll();
    auto retry_output = take_datagrams(caller_output);
    bool saw_retry = false;
    for (const auto& datagram : retry_output) {
        const auto decoded = decode_packet(datagram);
        REQUIRE(decoded);
        REQUIRE(decoded.packet.kind != PacketKind::data);
        if (decoded.packet.kind == PacketKind::control
            && decoded.packet.control.type
                == ControlType::user_defined
            && decoded.packet.control.subtype
                == key_material_request_subtype) {
            saw_retry = true;
            listener.process_packet(decoded.packet, caller_endpoint);
        }
    }
    REQUIRE(saw_retry);
    const auto dropped_responses =
        take_datagrams(listener_output);
    REQUIRE(std::any_of(
        dropped_responses.begin(), dropped_responses.end(),
        [](const auto& datagram) {
            const auto decoded = decode_packet(datagram);
            return decoded
                && decoded.packet.kind == PacketKind::control
                && decoded.packet.control.type
                    == ControlType::user_defined
                && decoded.packet.control.subtype
                    == key_material_response_subtype;
        }));

    caller_now += 200'000;
    (void)caller.poll();
    retry_output = take_datagrams(caller_output);
    saw_retry = false;
    for (const auto& datagram : retry_output) {
        const auto decoded = decode_packet(datagram);
        REQUIRE(decoded);
        REQUIRE(decoded.packet.kind != PacketKind::data);
        if (decoded.packet.kind == PacketKind::control
            && decoded.packet.control.type
                == ControlType::user_defined
            && decoded.packet.control.subtype
                == key_material_request_subtype) {
            saw_retry = true;
            listener.process_packet(decoded.packet, caller_endpoint);
            listener.process_packet(decoded.packet, caller_endpoint);
        }
    }
    REQUIRE(saw_retry);

    auto duplicate_responses = take_datagrams(listener_output);
    std::size_t response_count = 0;
    for (const auto& datagram : duplicate_responses) {
        const auto decoded = decode_packet(datagram);
        REQUIRE(decoded);
        if (decoded.packet.kind == PacketKind::control
            && decoded.packet.control.type
                == ControlType::user_defined
            && decoded.packet.control.subtype
                == key_material_response_subtype) {
            ++response_count;
            caller.process_packet(decoded.packet, listener_endpoint);
            caller.process_packet(decoded.packet, listener_endpoint);
        }
    }
    REQUIRE_EQ(response_count, 2U);
    REQUIRE(!caller.broken());
    REQUIRE(!listener.broken());

    caller_now += 1'000'000;
    (void)caller.poll();
    auto resumed_output = take_datagrams(caller_output);
    bool resumed_data = false;
    for (const auto& datagram : resumed_output) {
        const auto decoded = decode_packet(datagram);
        REQUIRE(decoded);
        if (decoded.packet.kind == PacketKind::data) {
            resumed_data = true;
            REQUIRE_EQ(decoded.packet.data.sequence,
                expected_sequence);
            REQUIRE_EQ(decoded.packet.data.encryption_key,
                EncryptionKey::odd);
        }
        listener.process_packet(decoded.packet, caller_endpoint);
    }
    REQUIRE(resumed_data);
    expected_sequence = expected_sequence.next();
    REQUIRE_EQ(expected_sequence, SequenceNumber{3});
    REQUIRE_EQ(caller_crypto->active_sender_key(),
        EncryptionKey::even);
    deliver(listener_output, caller, listener_endpoint);

    for (std::size_t index = 0; index < 2U; ++index) {
        REQUIRE_EQ(listener.receive_message(
                       received, false, -1).status,
            MessageIoStatus::success);
    }
}

TEST(compat_runtime_gcm_caller_listener_authenticates_live_data_and_faults)
{
    const auto caller_channel = std::make_shared<DatagramChannel>();
    const auto listener_channel = std::make_shared<DatagramChannel>();
    CapturedDatagrams caller_output;
    CapturedDatagrams listener_output;
    caller_channel->set_send_hook_for_testing(capture_datagram, &caller_output);
    listener_channel->set_send_hook_for_testing(
        capture_datagram, &listener_output);
    const Ipv4Endpoint caller_endpoint {
        .address = {192, 0, 2, 51},
        .port = 14'301,
    };
    const Ipv4Endpoint listener_endpoint {
        .address = {192, 0, 2, 52},
        .port = 14'302,
    };

    const CryptoConfiguration crypto_configuration {
        .passphrase = "authenticated caller listener fixture",
        .mode = CryptoMode::aes_gcm,
        .enable_aes_gcm = true,
        .key_length = 32,
        .refresh_rate_packets = 64,
        .preannouncement_packets = 8,
    };
    auto caller_crypto = std::make_shared<CryptoSession>(crypto_configuration);
    auto listener_crypto =
        std::make_shared<CryptoSession>(crypto_configuration);
    REQUIRE_EQ(caller_crypto->start_initiator(), Error::none);
    REQUIRE_EQ(listener_crypto->accept_key_material(
                   caller_crypto->pending_key_material(), true),
        Error::none);
    REQUIRE_EQ(caller_crypto->acknowledge_key_material(
                   listener_crypto->key_material_response(), true),
        Error::none);
    confirm_directional_test_keys(*caller_crypto, *listener_crypto);
    REQUIRE(caller_crypto->authenticated_data_enabled());
    REQUIRE(listener_crypto->authenticated_data_enabled());

    SocketOptions options;
    REQUIRE_EQ(
        options.set(SocketOption::maximum_payload_size, 64), Error::none);
    REQUIRE_EQ(options.set(SocketOption::send_buffer_packets, 8), Error::none);
    REQUIRE_EQ(
        options.set(SocketOption::receive_buffer_packets, 8), Error::none);
    const auto origin = ConnectionRuntime::Clock::now();
    std::uint64_t caller_now = 1'000'000;
    std::uint64_t listener_now = 1'000'000;
    ConnectionRuntime caller {{
        .channel = caller_channel,
        .peer = listener_endpoint,
        .peer_socket_id = 520,
        .initial_sequence = SequenceNumber {500},
        .flow_window_packets = 256,
        .options = options,
        .origin = origin,
        .crypto = caller_crypto,
        .now_function = injected_now,
        .now_context = &caller_now,
    }};
    ConnectionRuntime listener {{
        .channel = listener_channel,
        .peer = caller_endpoint,
        .peer_socket_id = 510,
        .initial_sequence = SequenceNumber {500},
        .flow_window_packets = 256,
        .options = options,
        .origin = origin,
        .crypto = listener_crypto,
        .now_function = injected_now,
        .now_context = &listener_now,
    }};

    constexpr std::string_view first_message = "authenticated one";
    const auto first_bytes =
        std::as_bytes(std::span {first_message.data(), first_message.size()});
    REQUIRE_EQ(caller.queue_message(first_bytes, 0, true, false, -1).status,
        MessageIoStatus::success);
    (void)caller.poll();
    auto first_output = take_datagrams(caller_output);
    std::vector<std::byte> first_datagram;
    for (const auto& datagram : first_output) {
        const auto decoded = decode_packet(datagram);
        REQUIRE(decoded);
        if (decoded.packet.kind == PacketKind::data) {
            REQUIRE(first_datagram.empty());
            first_datagram = datagram;
        } else {
            listener.process_packet(decoded.packet, caller_endpoint);
        }
    }
    REQUIRE(!first_datagram.empty());
    const auto first_packet = decode_packet(first_datagram);
    REQUIRE(first_packet);
    REQUIRE_EQ(first_packet.packet.kind, PacketKind::data);
    REQUIRE_EQ(first_packet.packet.payload.size(),
        first_bytes.size() + srt_gcm_authentication_tag_size);
    REQUIRE(!std::equal(first_bytes.begin(), first_bytes.end(),
        first_packet.packet.payload.begin()));

    auto truncated = first_datagram;
    truncated.resize(packet_header_size + srt_gcm_authentication_tag_size - 1U);
    const auto truncated_packet = decode_packet(truncated);
    REQUIRE(truncated_packet);
    listener.process_packet(truncated_packet.packet, caller_endpoint);
    REQUIRE(!listener.broken());

    auto forged = first_datagram;
    forged.back() ^= std::byte {0x01};
    const auto forged_packet = decode_packet(forged);
    REQUIRE(forged_packet);
    listener.process_packet(forged_packet.packet, caller_endpoint);
    REQUIRE(!listener.broken());

    std::array<std::byte, 128> received {};
    REQUIRE_EQ(listener.receive_message(received, false, -1).status,
        MessageIoStatus::would_block);
    const auto failed_statistics = listener.statistics(false, true);
    REQUIRE_EQ(failed_statistics.total.receiver_undecryptable.packets, 2U);
    REQUIRE_EQ(failed_statistics.total.receiver_dropped.packets, 2U);

    listener.process_packet(first_packet.packet, caller_endpoint);
    listener.process_packet(first_packet.packet, caller_endpoint);
    const auto received_first = listener.receive_message(received, false, -1);
    REQUIRE_EQ(received_first.status, MessageIoStatus::success);
    REQUIRE_EQ(received_first.bytes, first_bytes.size());
    REQUIRE(
        std::equal(first_bytes.begin(), first_bytes.end(), received.begin()));
    REQUIRE_EQ(listener.receive_message(received, false, -1).status,
        MessageIoStatus::would_block);

    listener_now += 10'000;
    (void)listener.poll();
    deliver(listener_output, caller, listener_endpoint);
    caller_now += 10'000;
    (void)caller.poll();
    deliver(caller_output, listener, caller_endpoint);

    constexpr std::string_view lost_message = "temporarily lost";
    constexpr std::string_view reordered_message = "arrived second";
    const auto lost_bytes =
        std::as_bytes(std::span {lost_message.data(), lost_message.size()});
    const auto reordered_bytes = std::as_bytes(
        std::span {reordered_message.data(), reordered_message.size()});
    REQUIRE_EQ(caller.queue_message(lost_bytes, 0, true, false, -1).status,
        MessageIoStatus::success);
    caller_now += 1'000'000;
    (void)caller.poll();
    auto lost_output = take_datagrams(caller_output);
    std::vector<std::byte> lost_datagram;
    for (const auto& datagram : lost_output) {
        const auto decoded = decode_packet(datagram);
        REQUIRE(decoded);
        if (decoded.packet.kind == PacketKind::data) {
            REQUIRE(lost_datagram.empty());
            lost_datagram = datagram;
        } else {
            listener.process_packet(decoded.packet, caller_endpoint);
        }
    }
    REQUIRE(!lost_datagram.empty());
    REQUIRE_EQ(caller.queue_message(reordered_bytes, 0, true, false, -1).status,
        MessageIoStatus::success);
    caller_now += 10'000;
    (void)caller.poll();
    auto reordered_output = take_datagrams(caller_output);
    std::vector<std::byte> reordered_datagram;
    for (const auto& datagram : reordered_output) {
        const auto decoded = decode_packet(datagram);
        REQUIRE(decoded);
        if (decoded.packet.kind == PacketKind::data) {
            REQUIRE(reordered_datagram.empty());
            reordered_datagram = datagram;
        } else {
            listener.process_packet(decoded.packet, caller_endpoint);
        }
    }
    REQUIRE(!reordered_datagram.empty());
    const auto lost_packet = decode_packet(lost_datagram);
    const auto reordered_packet = decode_packet(reordered_datagram);
    REQUIRE(lost_packet);
    REQUIRE(reordered_packet);
    REQUIRE_EQ(lost_packet.packet.data.sequence, SequenceNumber {501});
    REQUIRE_EQ(reordered_packet.packet.data.sequence, SequenceNumber {502});

    listener.process_packet(reordered_packet.packet, caller_endpoint);
    REQUIRE_EQ(listener.receive_message(received, false, -1).status,
        MessageIoStatus::would_block);
    listener.process_packet(lost_packet.packet, caller_endpoint);
    listener.process_packet(reordered_packet.packet, caller_endpoint);
    listener_now = caller_now + 1'000'000;
    const auto received_lost = listener.receive_message(received, false, -1);
    REQUIRE_EQ(received_lost.status, MessageIoStatus::success);
    REQUIRE_EQ(received_lost.bytes, lost_bytes.size());
    REQUIRE(std::equal(lost_bytes.begin(), lost_bytes.end(), received.begin()));
    const auto received_reordered =
        listener.receive_message(received, false, -1);
    REQUIRE_EQ(received_reordered.status, MessageIoStatus::success);
    REQUIRE_EQ(received_reordered.bytes, reordered_bytes.size());
    REQUIRE(std::equal(
        reordered_bytes.begin(), reordered_bytes.end(), received.begin()));

    constexpr std::string_view reply = "listener authenticated reply";
    const auto reply_bytes =
        std::as_bytes(std::span {reply.data(), reply.size()});
    REQUIRE_EQ(listener.queue_message(reply_bytes, 0, true, false, -1).status,
        MessageIoStatus::success);
    listener_now += 1'000'000;
    (void)listener.poll();
    auto reply_output = take_datagrams(listener_output);
    std::vector<std::byte> reply_datagram;
    for (const auto& datagram : reply_output) {
        const auto decoded = decode_packet(datagram);
        REQUIRE(decoded);
        if (decoded.packet.kind == PacketKind::data) {
            REQUIRE(reply_datagram.empty());
            reply_datagram = datagram;
        } else {
            caller.process_packet(decoded.packet, listener_endpoint);
        }
    }
    REQUIRE(!reply_datagram.empty());
    const auto reply_packet = decode_packet(reply_datagram);
    REQUIRE(reply_packet);
    REQUIRE_EQ(reply_packet.packet.payload.size(),
        reply_bytes.size() + srt_gcm_authentication_tag_size);
    caller.process_packet(reply_packet.packet, listener_endpoint);
    const auto received_reply = caller.receive_message(received, false, -1);
    REQUIRE_EQ(received_reply.status, MessageIoStatus::success);
    REQUIRE_EQ(received_reply.bytes, reply_bytes.size());
    REQUIRE(
        std::equal(reply_bytes.begin(), reply_bytes.end(), received.begin()));
    REQUIRE(!caller.broken());
    REQUIRE(!listener.broken());
}

TEST(compat_runtime_gcm_ipv6_reserves_the_tag_at_the_mss_boundary)
{
    const auto sender_channel = std::make_shared<DatagramChannel>();
    const auto receiver_channel = std::make_shared<DatagramChannel>();
    CapturedDatagrams sender_output;
    sender_channel->set_send_hook_for_testing(capture_datagram, &sender_output);
    const auto sender_endpoint = IpEndpoint::ipv6_loopback(14'311);
    const auto receiver_endpoint = IpEndpoint::ipv6_loopback(14'312);
    const CryptoConfiguration crypto_configuration {
        .passphrase = "authenticated ipv6 boundary fixture",
        .mode = CryptoMode::aes_gcm,
        .enable_aes_gcm = true,
        .key_length = 16,
        .refresh_rate_packets = 64,
        .preannouncement_packets = 8,
    };
    auto sender_crypto = std::make_shared<CryptoSession>(crypto_configuration);
    auto receiver_crypto =
        std::make_shared<CryptoSession>(crypto_configuration);
    REQUIRE_EQ(sender_crypto->start_initiator(), Error::none);
    REQUIRE_EQ(receiver_crypto->accept_key_material(
                   sender_crypto->pending_key_material(), true),
        Error::none);
    REQUIRE_EQ(sender_crypto->acknowledge_key_material(
                   receiver_crypto->key_material_response(), true),
        Error::none);
    confirm_directional_test_keys(*sender_crypto, *receiver_crypto);

    SocketOptions options;
    REQUIRE_EQ(
        options.set(SocketOption::maximum_segment_size, 1'280), Error::none);
    REQUIRE_EQ(
        options.set(SocketOption::maximum_payload_size, 1'216), Error::none);
    REQUIRE_EQ(options.set(SocketOption::send_buffer_packets, 8), Error::none);
    REQUIRE_EQ(
        options.set(SocketOption::receive_buffer_packets, 8), Error::none);
    const auto origin = ConnectionRuntime::Clock::now();
    std::uint64_t sender_now = 1'000'000;
    std::uint64_t receiver_now = 1'000'000;
    ConnectionRuntime sender {{
        .channel = sender_channel,
        .peer = receiver_endpoint,
        .peer_socket_id = 532,
        .initial_sequence = SequenceNumber {530},
        .flow_window_packets = 256,
        .options = options,
        .origin = origin,
        .crypto = sender_crypto,
        .now_function = injected_now,
        .now_context = &sender_now,
    }};
    ConnectionRuntime receiver {{
        .channel = receiver_channel,
        .peer = sender_endpoint,
        .peer_socket_id = 531,
        .initial_sequence = SequenceNumber {530},
        .flow_window_packets = 256,
        .options = options,
        .origin = origin,
        .crypto = receiver_crypto,
        .now_function = injected_now,
        .now_context = &receiver_now,
    }};

    std::array<std::byte, 1'200> plaintext {};
    for (std::size_t index = 0; index < plaintext.size(); ++index) {
        plaintext[index] = static_cast<std::byte>(index % 251U);
    }
    REQUIRE_EQ(sender.queue_message(plaintext, 0, true, false, -1).status,
        MessageIoStatus::success);
    std::vector<std::byte> protected_datagram;
    for (std::size_t attempt = 0; attempt < 4U && protected_datagram.empty();
        ++attempt) {
        (void)sender.poll();
        for (const auto& datagram : take_datagrams(sender_output)) {
            const auto decoded = decode_packet(datagram);
            REQUIRE(decoded);
            if (decoded.packet.kind == PacketKind::data) {
                REQUIRE(protected_datagram.empty());
                protected_datagram = datagram;
            }
        }
        sender_now += 10'000;
    }
    REQUIRE(!protected_datagram.empty());
    REQUIRE_EQ(protected_datagram.size(),
        packet_header_size + plaintext.size()
            + srt_gcm_authentication_tag_size);
    const auto packet = decode_packet(protected_datagram);
    REQUIRE(packet);
    REQUIRE_EQ(packet.packet.payload.size(),
        plaintext.size() + srt_gcm_authentication_tag_size);
    receiver.process_packet(packet.packet, sender_endpoint);

    std::array<std::byte, 1'200> received {};
    const auto result = receiver.receive_message(received, false, -1);
    REQUIRE_EQ(result.status, MessageIoStatus::success);
    REQUIRE_EQ(result.bytes, plaintext.size());
    REQUIRE_EQ(received, plaintext);
    const auto sender_statistics = sender.statistics(false, true);
    const auto receiver_statistics = receiver.statistics(false, true);
    REQUIRE_EQ(sender_statistics.total.sent.payload_bytes, plaintext.size());
    REQUIRE_EQ(
        receiver_statistics.total.received.payload_bytes, plaintext.size());
}

TEST(compat_runtime_sends_each_new_rotation_request_without_retry_delay)
{
    const auto channel = std::make_shared<DatagramChannel>();
    CapturedDatagrams output;
    channel->set_send_hook_for_testing(capture_datagram, &output);
    const Ipv4Endpoint peer {
        .address = {192, 0, 2, 23},
        .port = 14'003,
    };
    const CryptoConfiguration crypto_configuration {
        .passphrase = "independent rotation retry windows",
        .key_length = 16,
        .refresh_rate_packets = 3,
        .preannouncement_packets = 1,
    };
    auto sender_crypto = std::make_shared<CryptoSession>(crypto_configuration);
    auto receiver_crypto =
        std::make_shared<CryptoSession>(crypto_configuration);
    REQUIRE_EQ(sender_crypto->start_initiator(), Error::none);
    REQUIRE_EQ(receiver_crypto->accept_key_material(
                   sender_crypto->pending_key_material(), false),
        Error::none);
    REQUIRE_EQ(sender_crypto->acknowledge_key_material(
                   receiver_crypto->key_material_response(), false),
        Error::none);

    SocketOptions options;
    REQUIRE_EQ(options.set(SocketOption::send_buffer_packets, 8), Error::none);
    std::uint64_t now = 1'000'000;
    ConnectionRuntime sender {{
        .channel = channel,
        .peer = peer,
        .peer_socket_id = 230,
        .initial_sequence = SequenceNumber {500},
        .flow_window_packets = 256,
        .options = options,
        .origin = ConnectionRuntime::Clock::now(),
        .crypto = sender_crypto,
        .now_function = injected_now,
        .now_context = &now,
    }};

    std::size_t request_count = 0;
    const auto send_one = [&](unsigned char value) {
        now += 10'000;
        const std::array payload {std::byte {value}};
        REQUIRE_EQ(sender.queue_message(payload, 0, true, false, -1).status,
            MessageIoStatus::success);
        (void)sender.poll();
        bool saw_data = false;
        for (const auto& datagram : take_datagrams(output)) {
            const auto decoded = decode_packet(datagram);
            REQUIRE(decoded);
            if (decoded.packet.kind == PacketKind::data) {
                saw_data = true;
                continue;
            }
            if (decoded.packet.control.type != ControlType::user_defined
                || decoded.packet.control.subtype
                    != key_material_request_subtype) {
                continue;
            }
            ++request_count;
            REQUIRE_EQ(receiver_crypto->accept_key_material(
                           decoded.packet.payload, false),
                Error::none);
            const PacketView response {
                .kind = PacketKind::control,
                .control =
                    {
                        .type = ControlType::user_defined,
                        .subtype = key_material_response_subtype,
                        .destination_socket_id = 230,
                    },
                .payload = receiver_crypto->key_material_response(),
            };
            sender.process_packet(response, peer);
        }
        REQUIRE(saw_data);
    };

    send_one(1U);
    send_one(2U);
    REQUIRE_EQ(request_count, 1U);
    send_one(3U);
    REQUIRE_EQ(sender_crypto->active_sender_key(), EncryptionKey::odd);
    send_one(4U);
    send_one(5U);
    // Only 30 ms separate the two independent KMREQ messages. The 100 ms
    // interval applies to retransmitting one unacknowledged request, not to
    // the first request of the next rotation.
    REQUIRE_EQ(request_count, 2U);
    REQUIRE(!sender.broken());
}

TEST(compat_runtime_establishes_directional_crypto_with_runtime_key_exchange)
{
    const auto sender_channel = std::make_shared<DatagramChannel>();
    const auto receiver_channel = std::make_shared<DatagramChannel>();
    CapturedDatagrams sender_output;
    CapturedDatagrams receiver_output;
    sender_channel->set_send_hook_for_testing(
        capture_datagram, &sender_output);
    receiver_channel->set_send_hook_for_testing(
        capture_datagram, &receiver_output);
    const Ipv4Endpoint sender_endpoint{
        .address = {192, 0, 2, 41},
        .port = 14'201,
    };
    const Ipv4Endpoint receiver_endpoint{
        .address = {192, 0, 2, 42},
        .port = 14'202,
    };

    constexpr std::string_view passphrase = "directional crypto fixture";
    const CryptoConfiguration sender_crypto_configuration{
        .passphrase = passphrase,
        .key_length = 24,
        .refresh_rate_packets = 100,
        .preannouncement_packets = 10,
    };
    auto sender_crypto = std::make_shared<CryptoSession>(
        sender_crypto_configuration);
    auto receiver_crypto = std::make_shared<CryptoSession>(
        CryptoConfiguration{
            .passphrase = passphrase,
            .key_length = 16,
            .refresh_rate_packets = 100,
            .preannouncement_packets = 10,
        });
    REQUIRE_EQ(sender_crypto->start_initiator(), Error::none);
    REQUIRE_EQ(receiver_crypto->sender_state(),
        CryptoState::unsecured);

    SocketOptions sender_options;
    REQUIRE_EQ(sender_options.set_passphrase(passphrase), Error::none);
    REQUIRE_EQ(sender_options.set(
                   SocketOption::encryption_key_length, 24),
        Error::none);
    SocketOptions receiver_options;
    REQUIRE_EQ(receiver_options.set_passphrase(passphrase), Error::none);
    const auto origin = ConnectionRuntime::Clock::now();
    std::uint64_t sender_now = 1'000'000;
    std::uint64_t receiver_now = 1'000'000;
    ConnectionRuntime sender{{
        .channel = sender_channel,
        .peer = receiver_endpoint,
        .peer_socket_id = 420,
        .initial_sequence = SequenceNumber{800},
        .flow_window_packets = 256,
        .options = sender_options,
        .origin = origin,
        .crypto = sender_crypto,
        .now_function = injected_now,
        .now_context = &sender_now,
    }};
    ConnectionRuntime receiver{{
        .channel = receiver_channel,
        .peer = sender_endpoint,
        .peer_socket_id = 410,
        .initial_sequence = SequenceNumber{800},
        .flow_window_packets = 256,
        .options = receiver_options,
        .origin = origin,
        .crypto = receiver_crypto,
        .now_function = injected_now,
        .now_context = &receiver_now,
    }};

    constexpr std::string_view message = "encrypted after runtime kmrsp";
    REQUIRE_EQ(sender.queue_message(
                   std::as_bytes(std::span{
                       message.data(), message.size()}),
                   0, true, false, -1).status,
        MessageIoStatus::success);
    (void)sender.poll();
    auto requests = take_datagrams(sender_output);
    REQUIRE_EQ(requests.size(), 1U);
    const auto request = decode_packet(requests.front());
    REQUIRE(request);
    REQUIRE_EQ(request.packet.kind, PacketKind::control);
    REQUIRE_EQ(request.packet.control.type,
        ControlType::user_defined);
    REQUIRE_EQ(request.packet.control.subtype,
        key_material_request_subtype);
    REQUIRE_EQ(request.packet.control.destination_socket_id, 420U);
    REQUIRE_EQ(decode_key_material(request.packet.payload)
                   .key_material.key_length,
        24U);
    receiver.process_packet(request.packet, sender_endpoint);
    REQUIRE_EQ(receiver_crypto->receiver_state(),
        CryptoState::secured);
    REQUIRE_EQ(receiver.crypto_key_length(), 24U);
    REQUIRE_EQ(receiver_crypto->sender_state(),
        CryptoState::unsecured);
    (void)receiver.poll();
    REQUIRE(!receiver.broken());

    auto responses = take_datagrams(receiver_output);
    REQUIRE_EQ(responses.size(), 1U);
    const auto response = decode_packet(responses.front());
    REQUIRE(response);
    REQUIRE_EQ(response.packet.control.subtype,
        key_material_response_subtype);
    REQUIRE(std::equal(
        response.packet.payload.begin(),
        response.packet.payload.end(),
        request.packet.payload.begin()));
    sender.process_packet(response.packet, receiver_endpoint);
    REQUIRE_EQ(sender_crypto->sender_state(), CryptoState::secured);

    sender_now += 1'000;
    (void)sender.poll();
    auto encrypted = take_datagrams(sender_output);
    REQUIRE_EQ(encrypted.size(), 1U);
    const auto data = decode_packet(encrypted.front());
    REQUIRE(data);
    REQUIRE_EQ(data.packet.kind, PacketKind::data);
    REQUIRE_EQ(data.packet.data.encryption_key, EncryptionKey::even);
    REQUIRE(!std::equal(
        data.packet.payload.begin(), data.packet.payload.end(),
        std::as_bytes(std::span{message.data(), message.size()}).begin()));
    receiver.process_packet(data.packet, sender_endpoint);

    std::array<std::byte, 64> clear{};
    const auto received = receiver.receive_message(
        clear, false, -1);
    REQUIRE_EQ(received.status, MessageIoStatus::success);
    REQUIRE_EQ(received.bytes, message.size());
    REQUIRE(std::equal(
        clear.begin(), clear.begin() + received.bytes,
        std::as_bytes(std::span{message.data(), message.size()}).begin()));
    REQUIRE(!sender.broken());
    REQUIRE(!receiver.broken());
}

TEST(compat_runtime_confirms_independent_bidirectional_keys_before_data)
{
    for (const auto mode : {CryptoMode::aes_ctr, CryptoMode::aes_gcm}) {
        auto first_channel = std::make_shared<DatagramChannel>();
        auto second_channel = std::make_shared<DatagramChannel>();
        CapturedDatagrams first_output, second_output;
        first_channel->set_send_hook_for_testing(
            capture_datagram, &first_output);
        second_channel->set_send_hook_for_testing(
            capture_datagram, &second_output);
        const Ipv4Endpoint first_endpoint {{192, 0, 2, 80}, 14800};
        const Ipv4Endpoint second_endpoint {{192, 0, 2, 81}, 14801};
        const CryptoConfiguration configuration {
            .passphrase = "directional runtime fixture",
            .mode = mode,
            .enable_aes_gcm = true,
            .key_length = 32,
        };
        auto first_crypto = std::make_shared<CryptoSession>(configuration);
        auto receiver_configuration = configuration;
        receiver_configuration.key_length = 16;
        auto second_crypto =
            std::make_shared<CryptoSession>(receiver_configuration);
        REQUIRE_EQ(first_crypto->start_initiator(), Error::none);
        REQUIRE_EQ(second_crypto->accept_key_material(
                       first_crypto->pending_key_material(), true),
            Error::none);
        REQUIRE_EQ(first_crypto->acknowledge_key_material(
                       second_crypto->key_material_response(), true),
            Error::none);
        // The responder's new sender key follows the negotiated length,
        // not its local default. No test helper confirms these runtime keys.
        REQUIRE_EQ(second_crypto->key_length(), 32U);
        SocketOptions options;
        REQUIRE_EQ(
            options.set(SocketOption::maximum_payload_size, 64), Error::none);
        std::uint64_t now = 1'000'000;
        const auto origin = ConnectionRuntime::Clock::now();
        ConnectionRuntime first {{
            .channel = first_channel,
            .peer = second_endpoint,
            .peer_socket_id = 81,
            .initial_sequence = SequenceNumber {700},
            .flow_window_packets = 256,
            .options = options,
            .origin = origin,
            .crypto = first_crypto,
            .now_function = injected_now,
            .now_context = &now,
        }};
        ConnectionRuntime second {{
            .channel = second_channel,
            .peer = first_endpoint,
            .peer_socket_id = 80,
            .initial_sequence = SequenceNumber {700},
            .flow_window_packets = 256,
            .options = options,
            .origin = origin,
            .crypto = second_crypto,
            .now_function = injected_now,
            .now_context = &now,
        }};
        const std::array<std::byte, 16> clear {std::byte {0x42}};
        REQUIRE_EQ(first.queue_message(clear, 0, false, false, -1).status,
            MessageIoStatus::success);
        REQUIRE_EQ(second.queue_message(clear, 0, false, false, -1).status,
            MessageIoStatus::success);
        (void)first.poll();
        (void)second.poll();
        for (auto* captured : {&first_output, &second_output}) {
            const auto requests = take_datagrams(*captured);
            REQUIRE_EQ(requests.size(), 1U);
            const auto packet = decode_packet(requests.front());
            REQUIRE(packet);
            REQUIRE_EQ(packet.packet.kind, PacketKind::control);
            REQUIRE_EQ(
                packet.packet.control.subtype, key_material_request_subtype);
            // Drop the first request: queued DATA must remain blocked.
        }
        REQUIRE(!first_crypto->ready_to_send_data());
        REQUIRE(!second_crypto->ready_to_send_data());
        now += 100'001;
        (void)first.poll();
        (void)second.poll();
        deliver(first_output, second, first_endpoint);
        deliver(second_output, first, second_endpoint);
        deliver(first_output, second, first_endpoint);
        REQUIRE(first_crypto->ready_to_send_data());
        REQUIRE(second_crypto->ready_to_send_data());
        now += 1'000;
        (void)first.poll();
        (void)second.poll();
        std::vector<std::byte> first_ciphertext, second_ciphertext;
        const auto forward =
            [&](CapturedDatagrams& captured, ConnectionRuntime& destination,
                Ipv4Endpoint source, std::vector<std::byte>& ciphertext) {
                for (const auto& datagram : take_datagrams(captured)) {
                    const auto packet = decode_packet(datagram);
                    REQUIRE(packet);
                    if (packet.packet.kind == PacketKind::data) {
                        REQUIRE(ciphertext.empty());
                        REQUIRE_EQ(
                            packet.packet.data.sequence, SequenceNumber {700});
                        REQUIRE(packet.packet.payload.size() >= clear.size());
                        ciphertext.assign(packet.packet.payload.begin(),
                            packet.packet.payload.begin() + clear.size());
                    }
                    destination.process_packet(packet.packet, source);
                }
            };
        forward(first_output, second, first_endpoint, first_ciphertext);
        forward(second_output, first, second_endpoint, second_ciphertext);
        REQUIRE_EQ(first_ciphertext.size(), clear.size());
        REQUIRE_EQ(second_ciphertext.size(), clear.size());
        REQUIRE(first_ciphertext != second_ciphertext);
        for (auto* runtime : {&first, &second}) {
            std::array<std::byte, 64> received {};
            const auto result = runtime->receive_message(received, false, -1);
            REQUIRE_EQ(result.status, MessageIoStatus::success);
            REQUIRE_EQ(result.bytes, clear.size());
            REQUIRE(std::equal(clear.begin(), clear.end(), received.begin()));
            REQUIRE(!runtime->broken());
        }
    }
}

TEST(compat_runtime_answers_optional_key_request_without_a_secret)
{
    const auto channel = std::make_shared<DatagramChannel>();
    CapturedDatagrams output;
    channel->set_send_hook_for_testing(capture_datagram, &output);
    const Ipv4Endpoint peer{
        .address = {192, 0, 2, 43},
        .port = 14'203,
    };
    SocketOptions options;
    REQUIRE_EQ(options.set(SocketOption::enforced_encryption, 0),
        Error::none);
    std::uint64_t now = 1'000'000;
    ConnectionRuntime receiver{{
        .channel = channel,
        .peer = peer,
        .peer_socket_id = 430,
        .initial_sequence = SequenceNumber{900},
        .flow_window_packets = 256,
        .options = options,
        .origin = ConnectionRuntime::Clock::now(),
        .now_function = injected_now,
        .now_context = &now,
    }};

    CryptoSession sender {{
        .passphrase = "optional crypto sender",
        .key_length = 16,
    }};
    REQUIRE_EQ(sender.start_initiator(), Error::none);
    const PacketView request{
        .kind = PacketKind::control,
        .control = {
            .type = ControlType::user_defined,
            .subtype = key_material_request_subtype,
            .destination_socket_id = 430,
        },
        .payload = sender.pending_key_material(),
    };
    receiver.process_packet(request, peer);
    REQUIRE(!receiver.broken());

    auto responses = take_datagrams(output);
    REQUIRE_EQ(responses.size(), 1U);
    const auto response = decode_packet(responses.front());
    REQUIRE(response);
    REQUIRE_EQ(response.packet.control.subtype,
        key_material_response_subtype);
    REQUIRE_EQ(response.packet.payload.size(), 4U);
    REQUIRE_EQ(response.packet.payload[3],
        static_cast<std::byte>(CryptoState::no_secret));
    REQUIRE_EQ(sender.acknowledge_key_material(
                   response.packet.payload, false),
        Error::cryptographic_failure);
    REQUIRE_EQ(sender.sender_state(), CryptoState::no_secret);
}

TEST(compat_runtime_optional_sender_resumes_clear_after_no_secret)
{
    const auto channel = std::make_shared<DatagramChannel>();
    CapturedDatagrams output;
    channel->set_send_hook_for_testing(capture_datagram, &output);
    const Ipv4Endpoint peer{
        .address = {192, 0, 2, 44},
        .port = 14'204,
    };
    constexpr std::string_view passphrase = "optional crypto secret";
    SocketOptions options;
    REQUIRE_EQ(options.set_passphrase(passphrase), Error::none);
    REQUIRE_EQ(options.set(SocketOption::enforced_encryption, 0),
        Error::none);
    auto crypto = std::make_shared<CryptoSession>(
        CryptoConfiguration{.passphrase = passphrase});
    REQUIRE_EQ(crypto->start_initiator(), Error::none);
    std::uint64_t now = 1'000'000;
    ConnectionRuntime sender{{
        .channel = channel,
        .peer = peer,
        .peer_socket_id = 440,
        .initial_sequence = SequenceNumber{950},
        .flow_window_packets = 256,
        .options = options,
        .origin = ConnectionRuntime::Clock::now(),
        .crypto = crypto,
        .now_function = injected_now,
        .now_context = &now,
    }};

    constexpr std::array<std::byte, 5> clear{
        std::byte{'c'}, std::byte{'l'}, std::byte{'e'},
        std::byte{'a'}, std::byte{'r'},
    };
    REQUIRE_EQ(sender.queue_message(
                   clear, 0, true, false, -1).status,
        MessageIoStatus::success);
    (void)sender.poll();
    auto request = take_datagrams(output);
    REQUIRE_EQ(request.size(), 1U);
    const auto decoded_request = decode_packet(request.front());
    REQUIRE(decoded_request);
    REQUIRE_EQ(decoded_request.packet.control.subtype,
        key_material_request_subtype);

    const std::array<std::byte, 4> no_secret{
        std::byte{0}, std::byte{0}, std::byte{0},
        static_cast<std::byte>(CryptoState::no_secret),
    };
    const PacketView response{
        .kind = PacketKind::control,
        .control = {
            .type = ControlType::user_defined,
            .subtype = key_material_response_subtype,
            .destination_socket_id = 440,
        },
        .payload = no_secret,
    };
    sender.process_packet(response, peer);
    REQUIRE(!sender.broken());
    REQUIRE_EQ(crypto->sender_state(), CryptoState::no_secret);
    REQUIRE_EQ(sender.sender_crypto_state(), CryptoState::unsecured);

    now += 1'000;
    (void)sender.poll();
    auto data = take_datagrams(output);
    REQUIRE_EQ(data.size(), 1U);
    const auto decoded_data = decode_packet(data.front());
    REQUIRE(decoded_data);
    REQUIRE_EQ(decoded_data.packet.kind, PacketKind::data);
    REQUIRE_EQ(decoded_data.packet.data.encryption_key,
        EncryptionKey::none);
    REQUIRE(std::equal(
        decoded_data.packet.payload.begin(),
        decoded_data.packet.payload.end(), clear.begin()));
}

TEST(compat_runtime_retransmits_original_ciphertext_across_key_rotation)
{
    const auto channel = std::make_shared<DatagramChannel>();
    CapturedDatagrams output;
    channel->set_send_hook_for_testing(
        capture_datagram, &output);
    const Ipv4Endpoint peer{
        .address = {192, 0, 2, 37},
        .port = 14'107,
    };

    const CryptoConfiguration crypto_configuration{
        .passphrase = "preserve retransmit wire payload",
        .key_length = 32,
        .refresh_rate_packets = 3,
        .preannouncement_packets = 1,
    };
    auto sender_crypto =
        std::make_shared<CryptoSession>(
            crypto_configuration);
    auto receiver_crypto =
        std::make_shared<CryptoSession>(
            crypto_configuration);
    REQUIRE_EQ(sender_crypto->start_initiator(),
        Error::none);
    REQUIRE_EQ(receiver_crypto->accept_key_material(
                   sender_crypto->pending_key_material(),
                   true),
        Error::none);
    REQUIRE_EQ(sender_crypto->acknowledge_key_material(
                   receiver_crypto->key_material_response(),
                   true),
        Error::none);
    confirm_directional_test_keys(*sender_crypto, *receiver_crypto);

    SocketOptions options;
    REQUIRE_EQ(options.set(
                   SocketOption::maximum_payload_size, 16),
        Error::none);
    REQUIRE_EQ(options.set(
                   SocketOption::send_buffer_packets, 8),
        Error::none);
    std::uint64_t now = 1'000'000;
    ConnectionRuntime sender{{
        .channel = channel,
        .peer = peer,
        .peer_socket_id = 370,
        .initial_sequence = SequenceNumber{100},
        .flow_window_packets = 256,
        .options = options,
        .origin = ConnectionRuntime::Clock::now(),
        .crypto = sender_crypto,
        .now_function = injected_now,
        .now_context = &now,
    }};

    std::array<std::vector<std::byte>, 4> originals;
    std::array<EncryptionKey, 4> original_keys{};
    for (std::size_t index = 0; index < originals.size(); ++index) {
        if (index == 2U) {
            REQUIRE_EQ(sender_crypto->prepare_rotation(),
                Error::none);
            REQUIRE_EQ(receiver_crypto->accept_key_material(
                           sender_crypto->pending_key_material(),
                           false),
                Error::none);
            REQUIRE_EQ(sender_crypto->acknowledge_key_material(
                           receiver_crypto->key_material_response(),
                           false),
                Error::none);
        }

        const std::array<std::byte, 4> clear{
            std::byte{static_cast<unsigned char>(index + 1U)},
            std::byte{0x22},
            std::byte{0x33},
            std::byte{0x44},
        };
        REQUIRE_EQ(sender.queue_message(
                       clear, 0, true, false, -1).status,
            MessageIoStatus::success);
        now += 10U;
        (void)sender.poll();

        bool saw_data = false;
        for (const auto& datagram : take_datagrams(output)) {
            const auto decoded = decode_packet(datagram);
            REQUIRE(decoded);
            if (decoded.packet.kind != PacketKind::data) {
                continue;
            }
            REQUIRE(!decoded.packet.data.retransmitted);
            REQUIRE_EQ(decoded.packet.data.sequence,
                SequenceNumber{
                    static_cast<std::uint32_t>(100U + index)});
            originals[index] = datagram;
            original_keys[index] =
                decoded.packet.data.encryption_key;
            saw_data = true;
        }
        REQUIRE(saw_data);
    }
    REQUIRE_EQ(original_keys[0], EncryptionKey::even);
    REQUIRE_EQ(original_keys[1], EncryptionKey::even);
    REQUIRE_EQ(original_keys[2], EncryptionKey::even);
    REQUIRE_EQ(original_keys[3], EncryptionKey::odd);
    REQUIRE_EQ(sender_crypto->active_sender_key(),
        EncryptionKey::odd);
    REQUIRE_EQ(sender_crypto->packets_on_active_key(), 1U);

    std::array<std::byte, 8> loss_payload{};
    const std::array<SequenceRange, 1> losses{{
        {
            .first = SequenceNumber{101},
            .last = SequenceNumber{103},
        },
    }};
    const auto encoded_loss =
        encode_loss_ranges(losses, loss_payload);
    REQUIRE(encoded_loss);
    const PacketView nak{
        .kind = PacketKind::control,
        .control = {
            .type = ControlType::negative_acknowledgement,
            .destination_socket_id = 370,
        },
        .payload = std::span{loss_payload}.first(
            encoded_loss.bytes_written),
    };
    sender.process_packet(nak, peer);

    std::array<std::vector<std::byte>, 3> retransmissions;
    for (std::size_t attempt = 0; attempt < 4U; ++attempt) {
        now += 10U;
        (void)sender.poll();
        for (const auto& datagram : take_datagrams(output)) {
            const auto decoded = decode_packet(datagram);
            REQUIRE(decoded);
            if (decoded.packet.kind != PacketKind::data) {
                continue;
            }
            REQUIRE(decoded.packet.data.retransmitted);
            const auto distance =
                decoded.packet.data.sequence.distance_from(
                    SequenceNumber{101});
            REQUIRE(distance >= 0);
            REQUIRE(static_cast<std::size_t>(distance)
                < retransmissions.size());
            retransmissions[
                static_cast<std::size_t>(distance)] = datagram;
        }
    }

    for (std::size_t index = 0; index < retransmissions.size(); ++index) {
        REQUIRE(!retransmissions[index].empty());
        const auto original =
            decode_packet(originals[index + 1U]);
        const auto retransmission =
            decode_packet(retransmissions[index]);
        REQUIRE(original);
        REQUIRE(retransmission);
        REQUIRE_EQ(
            retransmission.packet.data.encryption_key,
            original.packet.data.encryption_key);
        REQUIRE_EQ(retransmission.packet.payload.size(),
            original.packet.payload.size());
        REQUIRE(std::equal(
            retransmission.packet.payload.begin(),
            retransmission.packet.payload.end(),
            original.packet.payload.begin()));
    }
    REQUIRE_EQ(sender_crypto->active_sender_key(),
        EncryptionKey::odd);
    REQUIRE_EQ(sender_crypto->packets_on_active_key(), 1U);
    const auto statistics = sender.statistics(false, true);
    REQUIRE_EQ(
        statistics.total.sent_retransmitted.packets, 3U);
}

TEST(compat_runtime_gcm_retransmits_immutable_tag_across_key_rotation)
{
    const auto sender_channel = std::make_shared<DatagramChannel>();
    const auto receiver_channel = std::make_shared<DatagramChannel>();
    CapturedDatagrams sender_output;
    CapturedDatagrams receiver_output;
    sender_channel->set_send_hook_for_testing(capture_datagram, &sender_output);
    receiver_channel->set_send_hook_for_testing(
        capture_datagram, &receiver_output);
    const Ipv4Endpoint sender_endpoint {
        .address = {192, 0, 2, 61},
        .port = 14'401,
    };
    const Ipv4Endpoint receiver_endpoint {
        .address = {192, 0, 2, 62},
        .port = 14'402,
    };

    const CryptoConfiguration crypto_configuration {
        .passphrase = "authenticated immutable retransmission",
        .mode = CryptoMode::aes_gcm,
        .enable_aes_gcm = true,
        .key_length = 32,
        .refresh_rate_packets = 3,
        .preannouncement_packets = 1,
    };
    auto sender_crypto = std::make_shared<CryptoSession>(crypto_configuration);
    auto receiver_crypto =
        std::make_shared<CryptoSession>(crypto_configuration);
    REQUIRE_EQ(sender_crypto->start_initiator(), Error::none);
    REQUIRE_EQ(receiver_crypto->accept_key_material(
                   sender_crypto->pending_key_material(), true),
        Error::none);
    REQUIRE_EQ(sender_crypto->acknowledge_key_material(
                   receiver_crypto->key_material_response(), true),
        Error::none);
    confirm_directional_test_keys(*sender_crypto, *receiver_crypto);

    SocketOptions options;
    REQUIRE_EQ(
        options.set(SocketOption::maximum_payload_size, 32), Error::none);
    REQUIRE_EQ(options.set(SocketOption::send_buffer_packets, 8), Error::none);
    std::uint64_t sender_now = 1'000'000;
    std::uint64_t receiver_now = 1'000'000;
    const auto origin = ConnectionRuntime::Clock::now();
    ConnectionRuntime sender {{
        .channel = sender_channel,
        .peer = receiver_endpoint,
        .peer_socket_id = 620,
        .initial_sequence = SequenceNumber {100},
        .flow_window_packets = 256,
        .options = options,
        .origin = origin,
        .crypto = sender_crypto,
        .now_function = injected_now,
        .now_context = &sender_now,
    }};
    ConnectionRuntime receiver {{
        .channel = receiver_channel,
        .peer = sender_endpoint,
        .peer_socket_id = 610,
        .initial_sequence = SequenceNumber {100},
        .flow_window_packets = 256,
        .options = options,
        .origin = origin,
        .crypto = receiver_crypto,
        .now_function = injected_now,
        .now_context = &receiver_now,
    }};

    const std::array<std::byte, 4> lost_clear {
        std::byte {0x11},
        std::byte {0x22},
        std::byte {0x33},
        std::byte {0x44},
    };
    REQUIRE_EQ(sender.queue_message(lost_clear, 0, true, false, -1).status,
        MessageIoStatus::success);
    (void)sender.poll();
    std::vector<std::byte> original;
    for (const auto& datagram : take_datagrams(sender_output)) {
        const auto decoded = decode_packet(datagram);
        REQUIRE(decoded);
        if (decoded.packet.kind == PacketKind::data) {
            REQUIRE(original.empty());
            original = datagram;
        }
    }
    REQUIRE(!original.empty());
    const auto original_packet = decode_packet(original);
    REQUIRE(original_packet);
    REQUIRE_EQ(original_packet.packet.data.sequence, SequenceNumber {100});
    REQUIRE_EQ(original_packet.packet.data.encryption_key, EncryptionKey::even);
    REQUIRE_EQ(original_packet.packet.payload.size(),
        lost_clear.size() + srt_gcm_authentication_tag_size);

    const std::array<std::byte, 1> preannouncement_clear {std::byte {0x45}};
    REQUIRE_EQ(
        sender.queue_message(preannouncement_clear, 0, true, false, -1).status,
        MessageIoStatus::success);
    sender_now += 10'000;
    (void)sender.poll();
    bool saw_preannouncement_data = false;
    for (const auto& datagram : take_datagrams(sender_output)) {
        const auto decoded = decode_packet(datagram);
        REQUIRE(decoded);
        if (decoded.packet.kind == PacketKind::data) {
            saw_preannouncement_data = true;
            REQUIRE_EQ(decoded.packet.data.sequence, SequenceNumber {101});
            REQUIRE_EQ(decoded.packet.data.encryption_key, EncryptionKey::even);
        }
    }
    REQUIRE(saw_preannouncement_data);

    REQUIRE_EQ(sender_crypto->prepare_rotation(), Error::none);
    REQUIRE(!sender_crypto->pending_key_material().empty());
    REQUIRE_EQ(receiver_crypto->accept_key_material(
                   sender_crypto->pending_key_material(), false),
        Error::none);
    REQUIRE_EQ(sender_crypto->acknowledge_key_material(
                   receiver_crypto->key_material_response(), false),
        Error::none);

    const std::array<std::byte, 1> boundary_clear {std::byte {0x55}};
    REQUIRE_EQ(sender.queue_message(boundary_clear, 0, true, false, -1).status,
        MessageIoStatus::success);
    sender_now += 10'000;
    (void)sender.poll();
    for (const auto& datagram : take_datagrams(sender_output)) {
        const auto decoded = decode_packet(datagram);
        REQUIRE(decoded);
        if (decoded.packet.kind == PacketKind::data) {
            REQUIRE_EQ(decoded.packet.data.sequence, SequenceNumber {102});
            REQUIRE_EQ(decoded.packet.data.encryption_key, EncryptionKey::even);
        }
    }
    REQUIRE_EQ(sender_crypto->active_sender_key(), EncryptionKey::odd);

    const std::array<std::byte, 1> rotated_clear {std::byte {0x66}};
    REQUIRE_EQ(sender.queue_message(rotated_clear, 0, true, false, -1).status,
        MessageIoStatus::success);
    sender_now += 10'000;
    (void)sender.poll();
    bool saw_rotated_data = false;
    for (const auto& datagram : take_datagrams(sender_output)) {
        const auto decoded = decode_packet(datagram);
        REQUIRE(decoded);
        if (decoded.packet.kind == PacketKind::data) {
            saw_rotated_data = true;
            REQUIRE_EQ(decoded.packet.data.sequence, SequenceNumber {103});
            REQUIRE_EQ(decoded.packet.data.encryption_key, EncryptionKey::odd);
        }
    }
    REQUIRE(saw_rotated_data);

    std::array<std::byte, 4> loss_payload {};
    const std::array<SequenceRange, 1> losses {{
        {
            .first = SequenceNumber {100},
            .last = SequenceNumber {100},
        },
    }};
    const auto encoded_loss = encode_loss_ranges(losses, loss_payload);
    REQUIRE(encoded_loss);
    const PacketView nak {
        .kind = PacketKind::control,
        .control =
            {
                .type = ControlType::negative_acknowledgement,
                .destination_socket_id = 610,
            },
        .payload = std::span {loss_payload}.first(encoded_loss.bytes_written),
    };
    sender.process_packet(nak, receiver_endpoint);

    std::vector<std::byte> retransmission;
    for (std::size_t attempt = 0; attempt < 4U && retransmission.empty();
        ++attempt) {
        sender_now += 10'000;
        (void)sender.poll();
        for (const auto& datagram : take_datagrams(sender_output)) {
            const auto decoded = decode_packet(datagram);
            REQUIRE(decoded);
            if (decoded.packet.kind == PacketKind::data) {
                REQUIRE(retransmission.empty());
                retransmission = datagram;
            }
        }
    }
    REQUIRE(!retransmission.empty());
    const auto retransmitted_packet = decode_packet(retransmission);
    REQUIRE(retransmitted_packet);
    REQUIRE(retransmitted_packet.packet.data.retransmitted);
    REQUIRE_EQ(retransmitted_packet.packet.data.sequence, SequenceNumber {100});
    REQUIRE_EQ(retransmitted_packet.packet.data.encryption_key,
        original_packet.packet.data.encryption_key);
    REQUIRE_EQ(retransmitted_packet.packet.payload.size(),
        original_packet.packet.payload.size());
    REQUIRE(std::equal(retransmitted_packet.packet.payload.begin(),
        retransmitted_packet.packet.payload.end(),
        original_packet.packet.payload.begin()));

    receiver.process_packet(retransmitted_packet.packet, sender_endpoint);
    receiver_now += 1'000'000;
    std::array<std::byte, 16> received {};
    const auto received_message = receiver.receive_message(received, false, -1);
    REQUIRE_EQ(received_message.status, MessageIoStatus::success);
    REQUIRE_EQ(received_message.bytes, lost_clear.size());
    REQUIRE(std::equal(lost_clear.begin(), lost_clear.end(), received.begin()));
    const auto statistics = sender.statistics(false, true);
    REQUIRE_EQ(statistics.total.sent_retransmitted.packets, 1U);
    REQUIRE_EQ(
        statistics.total.sent_retransmitted.payload_bytes, lost_clear.size());
    REQUIRE(!sender.broken());
    REQUIRE(!receiver.broken());
}

TEST(compat_runtime_sends_one_shutdown_and_reports_peer_close)
{
    const auto caller_channel = std::make_shared<DatagramChannel>();
    const auto listener_channel = std::make_shared<DatagramChannel>();
    CapturedDatagrams caller_output;
    CapturedDatagrams listener_output;
    caller_channel->set_send_hook_for_testing(
        capture_datagram, &caller_output);
    listener_channel->set_send_hook_for_testing(
        capture_datagram, &listener_output);

    const Ipv4Endpoint caller_endpoint{
        .address = {192, 0, 2, 11},
        .port = 11'001,
    };
    const Ipv4Endpoint listener_endpoint{
        .address = {192, 0, 2, 12},
        .port = 11'002,
    };
    SocketOptions options;
    const auto origin = ConnectionRuntime::Clock::now();
    ConnectionRuntime caller{{
        .channel = caller_channel,
        .peer = listener_endpoint,
        .peer_socket_id = 200,
        .initial_sequence = SequenceNumber{2'000},
        .options = options,
        .origin = origin,
    }};
    ConnectionRuntime listener{{
        .channel = listener_channel,
        .peer = caller_endpoint,
        .peer_socket_id = 100,
        .initial_sequence = SequenceNumber{2'000},
        .options = options,
        .origin = origin,
    }};

    caller.close();
    const auto shutdown_datagrams = take_datagrams(caller_output);
    REQUIRE_EQ(shutdown_datagrams.size(), 1U);
    const auto shutdown = decode_packet(shutdown_datagrams[0]);
    REQUIRE(shutdown);
    REQUIRE_EQ(shutdown.packet.kind, PacketKind::control);
    REQUIRE_EQ(shutdown.packet.control.type, ControlType::shutdown);
    REQUIRE_EQ(shutdown.packet.control.destination_socket_id, 200U);

    listener.process_packet(shutdown.packet, caller_endpoint);
    REQUIRE(listener.peer_closed());
    REQUIRE(listener.broken());
    std::array<std::byte, 1'500> destination{};
    REQUIRE_EQ(listener.receive_message(
                   destination, false, -1).status,
        MessageIoStatus::peer_closed);
    REQUIRE_EQ(listener.queue_message(
                   std::span<const std::byte>{destination}.first(1),
                   0, true, false, -1).status,
        MessageIoStatus::peer_closed);

    caller.close();
    REQUIRE(take_datagrams(caller_output).empty());
}

TEST(compat_runtime_drains_tsbpd_message_after_peer_shutdown)
{
    const auto channel = std::make_shared<DatagramChannel>();
    CapturedDatagrams output;
    channel->set_send_hook_for_testing(
        capture_datagram, &output);
    const Ipv4Endpoint peer{
        .address = {192, 0, 2, 13},
        .port = 11'003,
    };
    std::uint64_t now = 2'000;
    ConnectionRuntime runtime{{
        .channel = channel,
        .peer = peer,
        .peer_socket_id = 300,
        .initial_sequence = SequenceNumber{3'000},
        .negotiated_options = {
            .receive_tsbpd = true,
            .receive_delay_milliseconds = 120,
        },
        .origin = ConnectionRuntime::Clock::now(),
        .handshake_arrival_microseconds = 1'000,
        .peer_handshake_timestamp = PacketTimestamp{0},
        .now_function = injected_now,
        .now_context = &now,
    }};

    const std::array<std::byte, 3> payload{
        std::byte{'s'}, std::byte{'r'}, std::byte{'t'}};
    PacketView data;
    data.kind = PacketKind::data;
    data.data.sequence = SequenceNumber{3'000};
    data.data.message_number = 1;
    data.data.boundary = MessageBoundary::solo;
    data.data.timestamp = PacketTimestamp{50};
    data.payload = payload;
    runtime.process_packet(data, peer);

    PacketView shutdown;
    shutdown.kind = PacketKind::control;
    shutdown.control.type = ControlType::shutdown;
    shutdown.control.destination_socket_id = 300;
    const std::array<std::byte, 4> shutdown_padding {};
    shutdown.payload = shutdown_padding;
    runtime.process_packet(shutdown, peer);

    REQUIRE(runtime.peer_closed());
    REQUIRE(runtime.broken());
    REQUIRE(!runtime.readable());
    std::array<std::byte, 3> received{};
    REQUIRE_EQ(runtime.receive_message(
                   received, false, -1).status,
        MessageIoStatus::would_block);

    now = 121'049;
    REQUIRE(!runtime.readable());
    REQUIRE_EQ(runtime.receive_message(
                   received, false, -1).status,
        MessageIoStatus::would_block);

    now = 121'050;
    REQUIRE(runtime.readable());
    const auto delivered =
        runtime.receive_message(received, false, -1);
    REQUIRE_EQ(delivered.status, MessageIoStatus::success);
    REQUIRE_EQ(delivered.bytes, payload.size());
    REQUIRE(std::equal(
        payload.begin(), payload.end(), received.begin()));

    REQUIRE(runtime.readable());
    REQUIRE_EQ(runtime.receive_message(
                   received, false, -1).status,
        MessageIoStatus::peer_closed);
}

TEST(compat_runtime_exposes_the_exact_next_tsbpd_deadline)
{
    const auto channel =
        std::make_shared<DatagramChannel>();
    CapturedDatagrams output;
    channel->set_send_hook_for_testing(
        capture_datagram, &output);
    const Ipv4Endpoint peer{
        .address = {192, 0, 2, 14},
        .port = 11'004,
    };
    const auto origin = ConnectionRuntime::Clock::now();
    ConnectionRuntime runtime {{
        .channel = channel,
        .peer = peer,
        .peer_socket_id = 301,
        .initial_sequence = SequenceNumber {3'100},
        .negotiated_options =
            {
                .receive_tsbpd = true,
                .receive_delay_milliseconds = 500,
            },
        .origin = origin,
        .handshake_arrival_microseconds = 0,
        .peer_handshake_timestamp = PacketTimestamp {0},
    }};

    const std::array<std::byte, 1> payload{
        std::byte{'t'}};
    PacketView data;
    data.kind = PacketKind::data;
    data.data.sequence = SequenceNumber{3'100};
    data.data.message_number = 1;
    data.data.boundary = MessageBoundary::solo;
    const auto packet_time =
        std::chrono::duration_cast<std::chrono::microseconds>(
            ConnectionRuntime::Clock::now() - origin)
            .count();
    REQUIRE(packet_time >= 0);
    data.data.timestamp =
        PacketTimestamp {static_cast<std::uint32_t>(packet_time)};
    data.payload = payload;
    runtime.process_packet(data, peer);

    const auto deadline = runtime.next_readable_deadline();
    REQUIRE(deadline.has_value());
    const auto expected =
        origin + std::chrono::microseconds {packet_time + 500'000};
    REQUIRE_EQ(*deadline, expected);
    REQUIRE(!runtime.readable());
}

TEST(compat_runtime_only_signals_an_effective_receive_discard)
{
    const auto channel = std::make_shared<DatagramChannel>();
    const Ipv4Endpoint peer {
        .address = {192, 0, 2, 15},
        .port = 11'005,
    };
    constexpr SequenceNumber initial_sequence {3'200};
    ConnectionRuntime runtime {{
        .channel = channel,
        .peer = peer,
        .peer_socket_id = 302,
        .initial_sequence = initial_sequence,
        .origin = ConnectionRuntime::Clock::now(),
    }};

    const std::uint64_t initial_generation = ReadinessSignal::generation();
    REQUIRE(runtime.discard_received_before(initial_sequence));
    REQUIRE_EQ(ReadinessSignal::generation(), initial_generation);

    REQUIRE(runtime.discard_received_before(initial_sequence.next()));
    REQUIRE(ReadinessSignal::generation() != initial_generation);
}

TEST(compat_runtime_receiver_tlpktdrop_sends_a_cumulative_ack)
{
    const auto channel =
        std::make_shared<DatagramChannel>();
    CapturedDatagrams output;
    channel->set_send_hook_for_testing(
        capture_datagram, &output);
    const Ipv4Endpoint peer{
        .address = {192, 0, 2, 15},
        .port = 11'005,
    };
    std::uint64_t now = 1'010;
    ConnectionRuntime runtime{{
        .channel = channel,
        .peer = peer,
        .peer_socket_id = 302,
        .initial_sequence = SequenceNumber{3'200},
        .negotiated_options = {
            .receive_tsbpd = true,
            .too_late_packet_drop = true,
            .periodic_nak = true,
            .retransmit_flag = true,
            .receive_delay_milliseconds = 120,
        },
        .origin = ConnectionRuntime::Clock::now(),
        .handshake_arrival_microseconds = 1'000,
        .peer_handshake_timestamp = PacketTimestamp{0},
        .now_function = injected_now,
        .now_context = &now,
    }};

    const std::array<std::byte, 1> payload{
        std::byte{'p'}};
    PacketView data;
    data.kind = PacketKind::data;
    data.data.sequence = SequenceNumber{3'201};
    data.data.message_number = 2;
    data.data.boundary = MessageBoundary::solo;
    data.data.timestamp = PacketTimestamp{50};
    data.payload = payload;
    runtime.process_packet(data, peer);
    (void)take_datagrams(output);

    now = 121'049;
    REQUIRE(!runtime.readable());
    now = 121'050;
    REQUIRE(runtime.readable());

    const auto datagrams = take_datagrams(output);
    REQUIRE_EQ(datagrams.size(), 1U);
    const auto decoded = decode_packet(datagrams[0]);
    REQUIRE(decoded);
    REQUIRE_EQ(decoded.packet.kind, PacketKind::control);
    REQUIRE_EQ(decoded.packet.control.type,
        ControlType::acknowledgement);
    const auto acknowledgement =
        decode_acknowledgement(decoded.packet);
    REQUIRE(acknowledgement);
    REQUIRE_EQ(acknowledgement.acknowledgement.next_sequence,
        SequenceNumber{3'202});

    std::array<std::byte, 1> received{};
    REQUIRE_EQ(runtime.receive_message(
                   received, false, -1).status,
        MessageIoStatus::success);
    REQUIRE_EQ(received, payload);
    const auto statistics = runtime.statistics(false, true);
    REQUIRE_EQ(statistics.total.receiver_dropped.packets, 1U);
}

TEST(compat_runtime_peer_error_wakes_a_blocked_sender_once)
{
    const auto channel =
        std::make_shared<DatagramChannel>();
    CapturedDatagrams output;
    channel->set_send_hook_for_testing(
        capture_datagram, &output);
    const Ipv4Endpoint peer{
        .address = {192, 0, 2, 21},
        .port = 12'001,
    };
    SocketOptions options;
    REQUIRE_EQ(options.set(
                   SocketOption::transmission_type,
                   static_cast<std::int64_t>(
                       TransmissionType::file)),
        Error::none);
    REQUIRE_EQ(options.set(
                   SocketOption::maximum_payload_size, 2),
        Error::none);
    REQUIRE_EQ(options.set(
                   SocketOption::send_buffer_packets, 1),
        Error::none);
    ConnectionRuntime runtime{{
        .channel = channel,
        .peer = peer,
        .peer_socket_id = 210,
        .initial_sequence = SequenceNumber{100},
        .flow_window_packets = 256,
        .options = options,
        .origin = ConnectionRuntime::Clock::now(),
    }};

    const std::array<std::byte, 2> bytes{
        std::byte{'e'}, std::byte{'r'}};
    REQUIRE_EQ(runtime.queue_stream(
                   bytes, false, -1).status,
        MessageIoStatus::success);
    auto blocked = std::async(
        std::launch::async, [&runtime, &bytes] {
            return runtime.queue_stream(
                bytes, true, -1);
        });

    const std::array<std::byte, sizeof(std::uint32_t)> padding{};
    PacketView peer_error;
    peer_error.kind = PacketKind::control;
    peer_error.control.type = ControlType::peer_error;
    peer_error.control.type_specific = SRT_EFILE;
    peer_error.payload = padding;
    runtime.process_packet(peer_error, peer);

    REQUIRE_EQ(blocked.wait_for(
                   std::chrono::milliseconds{250}),
        std::future_status::ready);
    REQUIRE_EQ(blocked.get().status,
        MessageIoStatus::peer_error);
    REQUIRE_EQ(runtime.queue_stream(
                   bytes, false, -1).status,
        MessageIoStatus::would_block);
}

TEST(compat_runtime_peer_idle_timeout_uses_last_valid_packet)
{
    const auto channel = std::make_shared<DatagramChannel>();
    CapturedDatagrams output;
    channel->set_send_hook_for_testing(capture_datagram, &output);
    const Ipv4Endpoint peer{
        .address = {198, 51, 100, 40},
        .port = 12'000,
    };
    std::uint64_t now = 0;
    ConnectionRuntime runtime{{
        .channel = channel,
        .peer = peer,
        .peer_socket_id = 900,
        .initial_sequence = SequenceNumber{3'000},
        .origin = ConnectionRuntime::Clock::now(),
        .peer_idle_timeout_milliseconds = 5,
        .now_function = injected_now,
        .now_context = &now,
    }};

    now = 4'000;
    PacketView keepalive;
    keepalive.kind = PacketKind::control;
    keepalive.control.type = ControlType::keepalive;
    keepalive.control.destination_socket_id = 1;
    const std::array<std::byte, 4> keepalive_padding {};
    keepalive.payload = keepalive_padding;
    runtime.process_packet(keepalive, peer);

    now = 9'000;
    (void)runtime.poll();
    REQUIRE(!runtime.broken());
    now = 9'001;
    (void)runtime.poll();
    REQUIRE(runtime.broken());
    REQUIRE(!runtime.writable());
}

TEST(compat_runtime_rejects_malformed_peer_error_without_refreshing_idle_time)
{
    const auto channel = std::make_shared<DatagramChannel>();
    CapturedDatagrams output;
    channel->set_send_hook_for_testing(capture_datagram, &output);
    const Ipv4Endpoint peer{
        .address = {198, 51, 100, 41},
        .port = 12'001,
    };
    SocketOptions options;
    REQUIRE_EQ(options.set(
                   SocketOption::transmission_type,
                   static_cast<std::int64_t>(TransmissionType::file)),
        Error::none);
    REQUIRE_EQ(options.set(SocketOption::maximum_payload_size, 2),
        Error::none);
    REQUIRE_EQ(options.set(SocketOption::send_buffer_packets, 1),
        Error::none);
    std::uint64_t now = 0;
    ConnectionRuntime runtime{{
        .channel = channel,
        .peer = peer,
        .peer_socket_id = 901,
        .initial_sequence = SequenceNumber{3'100},
        .options = options,
        .origin = ConnectionRuntime::Clock::now(),
        .peer_idle_timeout_milliseconds = 5,
        .now_function = injected_now,
        .now_context = &now,
    }};

    const std::array<std::byte, 2> bytes{
        std::byte{'e'}, std::byte{'r'}};
    REQUIRE_EQ(runtime.queue_stream(bytes, false, -1).status,
        MessageIoStatus::success);

    now = 4'000;
    const std::array<std::byte, 1> malformed_padding{
        std::byte{0}};
    PacketView peer_error;
    peer_error.kind = PacketKind::control;
    peer_error.control.type = ControlType::peer_error;
    peer_error.control.type_specific = SRT_EFILE;
    peer_error.payload = malformed_padding;
    runtime.process_packet(peer_error, peer);

    REQUIRE_EQ(runtime.queue_stream(bytes, false, -1).status,
        MessageIoStatus::would_block);
    now = 5'001;
    (void)runtime.poll();
    REQUIRE(runtime.broken());
    REQUIRE(!runtime.writable());
}

TEST(compat_runtime_rejects_malformed_controls_without_refreshing_liveness)
{
    const auto channel = std::make_shared<DatagramChannel>();
    const Ipv4Endpoint peer {
        .address = {198, 51, 100, 42},
        .port = 12'002,
    };
    std::uint64_t now = 0;
    ConnectionRuntime runtime {{
        .channel = channel,
        .peer = peer,
        .peer_socket_id = 902,
        .initial_sequence = SequenceNumber {3'200},
        .origin = ConnectionRuntime::Clock::now(),
        .peer_idle_timeout_milliseconds = 5,
        .now_function = injected_now,
        .now_context = &now,
    }};
    const std::array<std::byte, 4> malformed_padding {std::byte {1}};

    for (const ControlType type : {ControlType::acknowledgement_of_ack,
             ControlType::keepalive, ControlType::congestion_warning,
             ControlType::shutdown, ControlType::peer_error}) {
        for (const std::size_t payload_size : {0U, 1U, 2U, 3U, 4U}) {
            PacketView packet;
            packet.kind = PacketKind::control;
            packet.control.type = type;
            packet.control.type_specific =
                type == ControlType::acknowledgement_of_ack ? 1U : 0U;
            packet.payload = std::span {malformed_padding}.first(payload_size);
            now = 4'000;
            runtime.process_packet(packet, peer);
            REQUIRE_EQ(
                runtime.response_health().last_response_microseconds, 0U);
        }
    }

    now = 5'001;
    (void)runtime.poll();
    REQUIRE(runtime.broken());
}

TEST(compat_runtime_rejects_malformed_key_controls_without_state_or_liveness)
{
    const auto channel = std::make_shared<DatagramChannel>();
    CapturedDatagrams output;
    channel->set_send_hook_for_testing(capture_datagram, &output);
    const Ipv4Endpoint peer {
        .address = {198, 51, 100, 43},
        .port = 12'003,
    };
    std::uint64_t now = 0;
    ConnectionRuntime runtime {{
        .channel = channel,
        .peer = peer,
        .peer_socket_id = 903,
        .initial_sequence = SequenceNumber {3'300},
        .origin = ConnectionRuntime::Clock::now(),
        .peer_idle_timeout_milliseconds = 5,
        .now_function = injected_now,
        .now_context = &now,
    }};
    const std::array<std::byte, 8> malformed_request {};
    const std::array<std::byte, 4> invalid_state {std::byte {0}, std::byte {0},
        std::byte {0}, static_cast<std::byte>(SRT_KM_S_E_SIZE)};
    const std::vector<std::byte> oversized_request(4'096, std::byte {0});

    for (const std::span<const std::byte> payload :
        {std::span<const std::byte> {}, std::span {malformed_request}.first(1U),
            std::span {malformed_request}.first(2U),
            std::span {malformed_request}.first(3U),
            std::span {malformed_request}.first(4U),
            std::span<const std::byte> {oversized_request}}) {
        PacketView request;
        request.kind = PacketKind::control;
        request.control.type = ControlType::user_defined;
        request.control.subtype = key_material_request_subtype;
        request.payload = payload;
        now = 4'000;
        runtime.process_packet(request, peer);
        REQUIRE_EQ(runtime.response_health().last_response_microseconds, 0U);
        REQUIRE(take_datagrams(output).empty());
        REQUIRE(!runtime.broken());
    }

    for (std::uint8_t state = SRT_KM_S_E_SIZE; state <= SRT_KM_S_E_SIZE + 1;
        ++state) {
        auto response_state = invalid_state;
        response_state.back() = static_cast<std::byte>(state);
        PacketView response;
        response.kind = PacketKind::control;
        response.control.type = ControlType::user_defined;
        response.control.subtype = key_material_response_subtype;
        response.payload = response_state;
        runtime.process_packet(response, peer);
        REQUIRE_EQ(runtime.response_health().last_response_microseconds, 0U);
        REQUIRE(!runtime.broken());
    }

    now = 5'001;
    (void)runtime.poll();
    REQUIRE(runtime.broken());
}

TEST(compat_runtime_replays_the_final_listener_handshake)
{
    const auto channel = std::make_shared<DatagramChannel>();
    CapturedDatagrams output;
    channel->set_send_hook_for_testing(capture_datagram, &output);
    const Ipv4Endpoint peer{
        .address = {203, 0, 113, 50},
        .port = 13'000,
    };

    HandshakeAction response;
    response.kind = HandshakeActionKind::send;
    response.packet.version = handshake_version_5;
    response.packet.request = HandshakeRequest::conclusion;
    response.packet.socket_id = 700;
    response.packet.syn_cookie = 0x1234'5678U;
    response.packet.extension_field = 1;
    response.has_handshake_extension = true;
    response.extension_type =
        HandshakeExtensionType::handshake_response;

    ConnectionRuntime runtime{{
        .channel = channel,
        .peer = peer,
        .peer_socket_id = 600,
        .initial_sequence = SequenceNumber{4'000},
        .origin = ConnectionRuntime::Clock::now(),
        .handshake_replay_response = response,
        .handshake_replay_enabled = true,
    }};

    HandshakeMessage duplicate;
    duplicate.packet.version = handshake_version_5;
    duplicate.packet.request = HandshakeRequest::conclusion;
    duplicate.packet.socket_id = 600;
    duplicate.packet.syn_cookie = 0x1234'5678U;
    duplicate.has_handshake_extension = true;
    duplicate.extension_type =
        HandshakeExtensionType::handshake_request;
    REQUIRE(runtime.process_handshake(duplicate, peer));

    const auto responses = take_datagrams(output);
    REQUIRE_EQ(responses.size(), 1U);
    const auto decoded = decode_handshake_datagram(responses[0]);
    REQUIRE(decoded);
    REQUIRE_EQ(decoded.control.destination_socket_id, 600U);
    REQUIRE_EQ(decoded.message.packet.socket_id, 700U);
    REQUIRE_EQ(decoded.message.packet.request,
        HandshakeRequest::conclusion);
    REQUIRE_EQ(decoded.message.packet.syn_cookie, 0x1234'5678U);
    REQUIRE(decoded.message.has_handshake_extension);
    REQUIRE_EQ(decoded.message.extension_type,
        HandshakeExtensionType::handshake_response);

    duplicate.packet.syn_cookie = 1;
    REQUIRE(!runtime.process_handshake(duplicate, peer));
    REQUIRE(take_datagrams(output).empty());
}

TEST(compat_channel_reroutes_a_queued_duplicate_listener_conclusion)
{
    const auto channel = std::make_shared<DatagramChannel>();
    CapturedDatagrams output;
    channel->set_send_hook_for_testing(capture_datagram, &output);
    const Ipv4Endpoint peer {
        .address = {203, 0, 113, 52},
        .port = 13'001,
    };

    HandshakeAction response;
    response.kind = HandshakeActionKind::send;
    response.packet.version = handshake_version_5;
    response.packet.request = HandshakeRequest::conclusion;
    response.packet.socket_id = 701;
    response.packet.syn_cookie = 0x2345'6789U;
    response.packet.extension_field = 1;
    response.has_handshake_extension = true;
    response.extension_type = HandshakeExtensionType::handshake_response;

    const auto runtime =
        std::make_shared<ConnectionRuntime>(ConnectionRuntime::Configuration {
            .channel = channel,
            .peer = peer,
            .peer_socket_id = 601,
            .initial_sequence = SequenceNumber {4'001},
            .origin = ConnectionRuntime::Clock::now(),
            .handshake_replay_response = response,
            .handshake_replay_enabled = true,
        });
    REQUIRE(channel->register_connection(701, runtime));

    HandshakeEnvelope duplicate {
        .message =
            {
                .packet =
                    {
                        .version = handshake_version_5,
                        .request = HandshakeRequest::conclusion,
                        .socket_id = 601,
                        .syn_cookie = 0x2345'6789U,
                    },
                .has_handshake_extension = true,
                .extension_type = HandshakeExtensionType::handshake_request,
            },
        .peer = peer,
    };
    REQUIRE(channel->replay_established_handshake(duplicate));

    const auto responses = take_datagrams(output);
    REQUIRE_EQ(responses.size(), 1U);
    const auto decoded = decode_handshake_datagram(responses[0]);
    REQUIRE(decoded);
    REQUIRE_EQ(decoded.control.destination_socket_id, 601U);
    REQUIRE_EQ(decoded.message.packet.socket_id, 701U);
    REQUIRE_EQ(decoded.message.packet.request, HandshakeRequest::conclusion);
    REQUIRE_EQ(decoded.message.packet.syn_cookie, 0x2345'6789U);

    duplicate.message.packet.syn_cookie = 1U;
    REQUIRE(!channel->replay_established_handshake(duplicate));
    REQUIRE(take_datagrams(output).empty());
    channel->unregister_connection(701);
}
