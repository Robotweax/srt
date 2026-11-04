#include "test.hpp"

#include "compat/socket_registry.hpp"
#include "compat/transport_runtime.hpp"
#include "robotweax/srt/codec.hpp"
#include "srt/srt.h"

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {
using namespace robotweax::srt;
using namespace robotweax::srt::compat;
using Clock = std::chrono::steady_clock;

struct Sockets {
    std::vector<SRTSOCKET> values;
    ~Sockets()
    {
        for (auto socket : values) {
            (void)srt_close(socket);
        }
    }
    SRTSOCKET keep(SRTSOCKET socket)
    {
        REQUIRE(socket != SRT_INVALID_SOCK);
        values.push_back(socket);
        return socket;
    }
};

// Only loss injection is synthetic: surviving data and every control packet
// use the actual UDP socket, runtime scheduler, FEC encoder and receive API.
struct TailLoss {
    std::shared_ptr<DatagramChannel> channel;
    int drop_from = 0;
    std::atomic<int> originals {0};
    std::atomic<int> dropped {0};
    std::atomic<int> repairs {0};
    std::atomic<std::int64_t> first_original {0};
    std::atomic<std::int64_t> first_repair {0};
    std::array<std::atomic<std::uint32_t>, 3> sequences {};
    std::array<std::atomic<std::int64_t>, 3> repaired_at {};

    ~TailLoss()
    {
        if (channel) {
            channel->set_send_hook_for_testing(nullptr, nullptr);
        }
    }
};

UdpIoResult lose_tail(
    std::span<const std::byte> bytes, IpEndpoint peer, void* context) noexcept
{
    auto& fault = *static_cast<TailLoss*>(context);
    const auto packet = decode_packet(bytes);
    if (packet && packet.packet.kind == PacketKind::data) {
        const auto now = std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now().time_since_epoch())
                             .count();
        std::int64_t unset = 0;
        if (packet.packet.data.retransmitted) {
            ++fault.repairs;
            (void)fault.first_repair.compare_exchange_strong(unset, now);
            for (std::size_t index = 0; index < fault.sequences.size();
                ++index) {
                if (fault.sequences[index].load()
                    == packet.packet.data.sequence.value()) {
                    std::int64_t unseen = 0;
                    (void)fault.repaired_at[index].compare_exchange_strong(
                        unseen, now);
                }
            }
        } else {
            (void)fault.first_original.compare_exchange_strong(unset, now);
            const auto index = fault.originals.fetch_add(1);
            if (index >= 0 && index < 3) {
                fault.sequences[static_cast<std::size_t>(index)] =
                    packet.packet.data.sequence.value();
            }
            if (index >= fault.drop_from) {
                ++fault.dropped;
                return {.bytes_transferred = bytes.size()};
            }
        }
    }
    return fault.channel->socket.send_to(bytes, peer);
}

UdpIoResult lose_feedback(
    std::span<const std::byte> bytes, IpEndpoint peer, void* context) noexcept
{
    auto& fault = *static_cast<TailLoss*>(context);
    const auto packet = decode_packet(bytes);
    if (packet && packet.packet.kind == PacketKind::control
        && packet.packet.control.type == ControlType::acknowledgement) {
        ++fault.dropped;
        return {.bytes_transferred = bytes.size()};
    }
    return fault.channel->socket.send_to(bytes, peer);
}

std::shared_ptr<SocketRecord> record_for(SRTSOCKET socket)
{
    auto record = SocketRegistry::instance().find(socket);
    REQUIRE(record != nullptr);
    return record;
}

void run_tail_loss(int crypto, int lost, bool warmup, bool probe,
    std::int32_t latency, bool timely)
{
    Sockets sockets;
    const auto listener = sockets.keep(srt_create_socket());
    const auto caller = sockets.keep(srt_create_socket());
    constexpr std::int32_t timeout = 1'500;
    constexpr char filter[] = "fec,cols:8,rows:8,layout:even,arq:always";
    constexpr char secret[] = "arq-transport-test-secret";
    for (const auto socket : {listener, caller}) {
        REQUIRE_EQ(
            srt_setsockflag(socket, SRTO_RCVLATENCY, &latency, sizeof(latency)),
            0);
        REQUIRE_EQ(srt_setsockflag(
                       socket, SRTO_PEERLATENCY, &latency, sizeof(latency)),
            0);
        REQUIRE_EQ(
            srt_setsockflag(socket, SRTO_RCVTIMEO, &timeout, sizeof(timeout)),
            0);
        REQUIRE_EQ(
            srt_setsockflag(socket, SRTO_CONNTIMEO, &timeout, sizeof(timeout)),
            0);
        REQUIRE_EQ(srt_setsockflag(
                       socket, SRTO_PACKETFILTER, filter, sizeof(filter) - 1),
            0);
        if (crypto != 0) {
            REQUIRE_EQ(srt_setsockflag(
                           socket, SRTO_PASSPHRASE, secret, sizeof(secret) - 1),
                0);
#ifdef ENABLE_AEAD_API_PREVIEW
            REQUIRE_EQ(srt_setsockflag(
                           socket, SRTO_CRYPTOMODE, &crypto, sizeof(crypto)),
                0);
#endif
        }
    }
    const int policy = ROBOTWEAX_SRT_FECPOLICY_ARQFIRST;
    REQUIRE_EQ(srt_setsockflag(
                   caller, SRTO_ROBOTWEAX_FECPOLICY, &policy, sizeof(policy)),
        0);
    REQUIRE_EQ(srt_setsockflag(
                   caller, SRTO_ROBOTWEAX_TAILPROBE, &probe, sizeof(probe)),
        0);
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE_EQ(srt_bind(listener, reinterpret_cast<const sockaddr*>(&address),
                   sizeof(address)),
        0);
    REQUIRE_EQ(srt_listen(listener, 1), 0);
    int size = sizeof(address);
    REQUIRE_EQ(
        srt_getsockname(listener, reinterpret_cast<sockaddr*>(&address), &size),
        0);
    REQUIRE_EQ(
        srt_connect(caller, reinterpret_cast<const sockaddr*>(&address), size),
        0);
    const auto receiver = sockets.keep(srt_accept(listener, nullptr, nullptr));
    const auto sender_record = record_for(caller);
    const auto receiver_record = record_for(receiver);
    std::shared_ptr<ConnectionRuntime> sender_runtime;
    std::shared_ptr<ConnectionRuntime> receiver_runtime;
    TailLoss fault;
    TailLoss feedback;
    {
        std::lock_guard lock(sender_record->mutex);
        sender_runtime = sender_record->runtime;
        fault.channel = sender_record->channel;
    }
    {
        std::lock_guard lock(receiver_record->mutex);
        receiver_runtime = receiver_record->runtime;
        feedback.channel = receiver_record->channel;
    }
    REQUIRE(sender_runtime && receiver_runtime && fault.channel);
    if (!warmup) {
        REQUIRE_EQ(
            sender_runtime->rtt_data().flags & ROBOTWEAX_SRT_RTT_OBSERVED, 0U);
        feedback.channel->set_send_hook_for_testing(lose_feedback, &feedback);
    }
    std::array<char, 1'500> received {};
    if (warmup) {
        // Complete one matrix and its ACK exchange before the partial tail.
        for (int index = 0; index < 64; ++index) {
            REQUIRE_EQ(srt_send(caller, "warm", 4), 4);
            std::this_thread::sleep_for(std::chrono::milliseconds {1});
        }
        for (int index = 0; index < 64; ++index) {
            REQUIRE_EQ(
                srt_recvmsg(receiver, received.data(), received.size()), 4);
        }
        REQUIRE(sender_runtime->statistics(false, false).total.parity_withheld
            > 0U);
    }
    const auto before =
        sender_runtime->statistics(false, false).total.tail_probes_sent;
    fault.drop_from = 3 - lost;
    fault.channel->set_send_hook_for_testing(lose_tail, &fault);
    for (int index = 0; index < 3; ++index) {
        const std::array<char, 2> payload {'t', static_cast<char>('0' + index)};
        REQUIRE_EQ(srt_send(caller, payload.data(), payload.size()), 2);
    }
    if (timely) {
        for (int index = 0; index < 3; ++index) {
            REQUIRE_EQ(
                srt_recvmsg(receiver, received.data(), received.size()), 2);
            REQUIRE_EQ(received[0], 't');
            REQUIRE_EQ(received[1], static_cast<char>('0' + index));
        }
        const auto stats = receiver_runtime->statistics(false, false);
        REQUIRE_EQ(stats.total.receiver_dropped.packets, 0U);
        // The belated counter also counts duplicate repairs below the ACK
        // frontier. Check exact source delivery and each missing packet's
        // repair deadline instead of confusing duplicates with late loss.
        REQUIRE_EQ(stats.total.received_unique.packets, warmup ? 67U : 3U);
        REQUIRE_EQ(fault.dropped.load(), lost);
        if (lost != 0) {
            REQUIRE(fault.repairs.load() > 0);
            REQUIRE(fault.first_repair.load() - fault.first_original.load()
                < static_cast<std::int64_t>(latency) * 1'000);
            for (int index = fault.drop_from; index < 3; ++index) {
                const auto repaired =
                    fault.repaired_at[static_cast<std::size_t>(index)].load();
                REQUIRE(repaired >= fault.first_original.load());
                REQUIRE(repaired - fault.first_original.load()
                    < static_cast<std::int64_t>(latency) * 1'000);
            }
        }
    } else {
        // With ACK feedback lost, an incomplete first FEC matrix cannot
        // reveal its missing tail. No RTT estimate means no armed probe,
        // even when the configured latency would otherwise allow ARQ.
        REQUIRE_EQ(srt_recvmsg(receiver, received.data(), received.size()), 2);
        REQUIRE_EQ(received[1], '0');
        constexpr std::int32_t tail_timeout = 200;
        REQUIRE_EQ(srt_setsockflag(receiver, SRTO_RCVTIMEO, &tail_timeout,
                       sizeof(tail_timeout)),
            0);
        REQUIRE_EQ(
            srt_recvmsg(receiver, received.data(), received.size()), SRT_ERROR);
        REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ETIMEOUT);
        REQUIRE(feedback.dropped.load() > 0);
        REQUIRE_EQ(
            sender_runtime->rtt_data().flags & ROBOTWEAX_SRT_RTT_OBSERVED, 0U);
        REQUIRE_EQ(
            sender_runtime->statistics(false, false).total.tail_probes_sent,
            before);
        REQUIRE_EQ(fault.dropped.load(), lost);
    }
    const auto fired =
        sender_runtime->statistics(false, false).total.tail_probes_sent
        - before;
    if (timely && lost != 0 && probe) {
        REQUIRE(fired >= 1U);
    }
    if (!probe) {
        REQUIRE_EQ(fired, 0U);
    }
    REQUIRE(fired <= 3U);
}
} // namespace

TEST(arq_transport_recovers_tail_with_measured_rtt)
{
    for (const int crypto : {0, 1
#ifdef ENABLE_AEAD_API_PREVIEW
             ,
             2
#endif
         }) {
        for (const int lost : {1, 2}) {
            run_tail_loss(crypto, lost, true, true, 400, true);
        }
    }
}

TEST(arq_transport_missing_rtt_feedback_keeps_probe_disarmed)
{
    run_tail_loss(0, 2, false, true, 400, false);
    run_tail_loss(0, 2, false, true, 60, false);
}

TEST(arq_transport_probe_stays_off_unless_enabled)
{
    run_tail_loss(0, 0, true, false, 400, true);
}
