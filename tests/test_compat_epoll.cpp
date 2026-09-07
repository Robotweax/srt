#include "test.hpp"

#include "robotweax/srt/packet.hpp"
#include "compat/group_registry.hpp"
#include "compat/readiness.hpp"
#include "compat/socket_registry.hpp"
#include "srt/srt.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <future>
#include <memory>
#include <span>

#if !defined(_WIN32)
#  include <unistd.h>
#endif

using namespace robotweax::srt;
using namespace robotweax::srt::compat;

namespace {

UdpIoResult accept_datagram(
    std::span<const std::byte> bytes,
    Ipv4Endpoint,
    void*) noexcept
{
    return {.bytes_transferred = bytes.size()};
}

std::shared_ptr<ConnectionRuntime> attach_test_runtime(
    SRTSOCKET socket,
    Ipv4Endpoint peer,
    SequenceNumber initial_sequence,
    NegotiatedLiveOptions negotiated_options = {})
{
    const auto record = SocketRegistry::instance().find(socket);
    REQUIRE(record != nullptr);
    auto channel = std::make_shared<DatagramChannel>();
    channel->set_send_hook_for_testing(accept_datagram, nullptr);
    auto runtime = std::make_shared<ConnectionRuntime>(
        ConnectionRuntime::Configuration{
            .channel = channel,
            .peer = peer,
            .peer_socket_id = 700,
            .initial_sequence = initial_sequence,
            .flow_window_packets = 256,
            .options = record->native_options,
            .negotiated_options = negotiated_options,
            .origin = ConnectionRuntime::Clock::now(),
        });
    {
        std::lock_guard lock(record->mutex);
        record->channel = std::move(channel);
        record->runtime = runtime;
        record->peer_endpoint = peer;
        record->has_peer_endpoint = true;
        record->peer_protocol_socket_id = 700;
        record->connection_initial_sequence =
            initial_sequence.value();
        record->state = SRTS_CONNECTED;
    }
    ReadinessSignal::notify();
    return runtime;
}

PacketView single_packet(
    SequenceNumber sequence,
    std::uint32_t message_number,
    std::span<const std::byte> payload)
{
    PacketView packet;
    packet.kind = PacketKind::data;
    packet.data.sequence = sequence;
    packet.data.boundary = MessageBoundary::solo;
    packet.data.in_order = true;
    packet.data.message_number = message_number;
    packet.data.timestamp = PacketTimestamp{
        static_cast<std::uint32_t>(message_number)};
    packet.data.destination_socket_id = 1;
    packet.payload = payload;
    return packet;
}

} // namespace

TEST(compat_epoll_lifecycle_flags_and_connection_events_match_contract)
{
    const int eid = srt_epoll_create();
    REQUIRE(eid >= 0);

    std::array<SRT_EPOLL_EVENT, 2> events{};
    REQUIRE_EQ(srt_epoll_uwait(
                   eid, events.data(),
                   static_cast<int>(events.size()), 0),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EPOLLEMPTY);

    REQUIRE_EQ(srt_epoll_set(eid, SRT_EPOLL_ENABLE_EMPTY), 0);
    REQUIRE_EQ(srt_epoll_set(eid, -1), SRT_EPOLL_ENABLE_EMPTY);
    REQUIRE_EQ(srt_epoll_uwait(
                   eid, events.data(),
                   static_cast<int>(events.size()), 0),
        0);
    REQUIRE_EQ(srt_epoll_set(eid, 0), SRT_EPOLL_ENABLE_EMPTY);

    const SRTSOCKET socket = srt_create_socket();
    REQUIRE(socket != SRT_INVALID_SOCK);
    const int watched = SRT_EPOLL_OUT | SRT_EPOLL_ERR;
    REQUIRE_EQ(srt_epoll_add_usock(eid, socket, &watched), 0);
    REQUIRE_EQ(srt_epoll_uwait(
                   eid, events.data(),
                   static_cast<int>(events.size()), 0),
        0);

    const auto record = SocketRegistry::instance().find(socket);
    REQUIRE(record != nullptr);
    {
        std::lock_guard lock(record->mutex);
        record->state = SRTS_CONNECTING;
        record->connect_error = SRT_ENOSERVER;
    }
    ReadinessSignal::notify();
    REQUIRE_EQ(srt_epoll_uwait(
                   eid, events.data(),
                   static_cast<int>(events.size()), 0),
        1);
    REQUIRE_EQ(events[0].fd, socket);
    REQUIRE_EQ(events[0].events, SRT_EPOLL_ERR);

    SRTSOCKET read_ready = SRT_INVALID_SOCK;
    int read_count = 1;
    SRTSOCKET write_ready = SRT_INVALID_SOCK;
    int write_count = 1;
    REQUIRE_EQ(srt_epoll_wait(eid,
                   &read_ready, &read_count,
                   &write_ready, &write_count, 0,
                   nullptr, nullptr, nullptr, nullptr),
        2);
    REQUIRE_EQ(read_count, 1);
    REQUIRE_EQ(write_count, 1);
    REQUIRE_EQ(read_ready, socket);
    REQUIRE_EQ(write_ready, socket);

    REQUIRE_EQ(srt_epoll_clear_usocks(eid), 0);
    REQUIRE_EQ(srt_epoll_remove_usock(eid, socket), 0);
    REQUIRE_EQ(srt_epoll_release(eid), 0);
    REQUIRE_EQ(srt_epoll_release(eid), SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPOLLID);
    REQUIRE_EQ(srt_close(socket), 0);
    REQUIRE_EQ(srt_cleanup(), 0);
}

TEST(compat_epoll_reports_level_and_edge_triggered_message_readiness)
{
    const SRTSOCKET socket = srt_create_socket();
    REQUIRE(socket != SRT_INVALID_SOCK);
    const Ipv4Endpoint peer{
        .address = {192, 0, 2, 10},
        .port = 9'000,
    };
    const SequenceNumber initial{4'000};
    const auto runtime =
        attach_test_runtime(socket, peer, initial);

    const int eid = srt_epoll_create();
    REQUIRE(eid >= 0);
    int watched = SRT_EPOLL_OUT;
    REQUIRE_EQ(srt_epoll_add_usock(eid, socket, &watched), 0);

    std::array<SRT_EPOLL_EVENT, 2> events{};
    REQUIRE_EQ(srt_epoll_uwait(
                   eid, events.data(),
                   static_cast<int>(events.size()), 0),
        1);
    REQUIRE_EQ(events[0].events, SRT_EPOLL_OUT);
    REQUIRE_EQ(srt_epoll_uwait(
                   eid, events.data(),
                   static_cast<int>(events.size()), 0),
        1);

    watched = SRT_EPOLL_IN | SRT_EPOLL_ET;
    REQUIRE_EQ(srt_epoll_update_usock(eid, socket, &watched), 0);
    REQUIRE_EQ(srt_epoll_uwait(
                   eid, events.data(),
                   static_cast<int>(events.size()), 0),
        0);

    const std::array<std::byte, 4> first_payload{
        std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    const PacketView first =
        single_packet(initial, 1, first_payload);
    runtime->process_packet(first, peer);
    REQUIRE_EQ(srt_epoll_uwait(
                   eid, events.data(),
                   static_cast<int>(events.size()), 0),
        1);
    REQUIRE_EQ(events[0].events, SRT_EPOLL_IN);
    REQUIRE_EQ(srt_epoll_uwait(
                   eid, events.data(),
                   static_cast<int>(events.size()), 0),
        0);

    std::array<std::byte, 32> received{};
    REQUIRE_EQ(runtime->receive_message(
                   received, false, -1).status,
        MessageIoStatus::success);
    REQUIRE_EQ(srt_epoll_uwait(
                   eid, events.data(),
                   static_cast<int>(events.size()), 0),
        0);

    const std::array<std::byte, 2> second_payload{
        std::byte{5}, std::byte{6}};
    const PacketView second =
        single_packet(initial.next(), 2, second_payload);
    runtime->process_packet(second, peer);
    REQUIRE_EQ(srt_epoll_uwait(
                   eid, events.data(),
                   static_cast<int>(events.size()), 0),
        1);
    REQUIRE_EQ(events[0].events, SRT_EPOLL_IN);

    REQUIRE_EQ(srt_epoll_release(eid), 0);
    REQUIRE_EQ(srt_close(socket), 0);
    REQUIRE_EQ(srt_cleanup(), 0);
}

TEST(compat_readiness_options_share_epoll_and_buffer_state)
{
    const auto read_option = [](SRTSOCKET socket, SRT_SOCKOPT option) {
        std::int32_t value = -1;
        int size = static_cast<int>(sizeof(value));
        REQUIRE_EQ(srt_getsockflag(socket, option, &value, &size), 0);
        REQUIRE_EQ(size, static_cast<int>(sizeof(value)));
        return value;
    };

    const SRTSOCKET socket = srt_create_socket();
    REQUIRE(socket != SRT_INVALID_SOCK);
    REQUIRE_EQ(read_option(socket, SRTO_EVENT), 0);
    REQUIRE_EQ(read_option(socket, SRTO_SNDDATA), 0);
    REQUIRE_EQ(read_option(socket, SRTO_RCVDATA), 0);

    const Ipv4Endpoint peer{
        .address = {192, 0, 2, 11},
        .port = 9'001,
    };
    const SequenceNumber initial{5'000};
    const auto runtime = attach_test_runtime(socket, peer, initial);

    const int eid = srt_epoll_create();
    REQUIRE(eid >= 0);
    int watched = SRT_EPOLL_IN | SRT_EPOLL_OUT | SRT_EPOLL_ERR;
    REQUIRE_EQ(srt_epoll_add_usock(eid, socket, &watched), 0);

    std::array<SRT_EPOLL_EVENT, 1> events{};
    REQUIRE_EQ(srt_epoll_uwait(eid, events.data(), 1, 0), 1);
    REQUIRE_EQ(events[0].events, SRT_EPOLL_OUT);
    REQUIRE_EQ(read_option(socket, SRTO_EVENT), SRT_EPOLL_OUT);

    const std::array<std::byte, 4> inbound{
        std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    runtime->process_packet(single_packet(initial, 1, inbound), peer);
    REQUIRE_EQ(read_option(socket, SRTO_RCVDATA), 1);
    REQUIRE_EQ(read_option(socket, SRTO_EVENT),
        SRT_EPOLL_IN | SRT_EPOLL_OUT);
    REQUIRE_EQ(srt_epoll_uwait(eid, events.data(), 1, 0), 1);
    REQUIRE_EQ(events[0].events, SRT_EPOLL_IN | SRT_EPOLL_OUT);

    const std::array<std::byte, 3> outbound{
        std::byte{5}, std::byte{6}, std::byte{7}};
    const auto queued = runtime->queue_message(
        outbound, 0, true, false, -1);
    REQUIRE_EQ(queued.status, MessageIoStatus::success);
    REQUIRE_EQ(read_option(socket, SRTO_SNDDATA), 1);

    std::array<std::byte, 16> received{};
    REQUIRE_EQ(runtime->receive_message(received, false, -1).status,
        MessageIoStatus::success);
    REQUIRE_EQ(read_option(socket, SRTO_RCVDATA), 0);
    REQUIRE_EQ(read_option(socket, SRTO_EVENT), SRT_EPOLL_OUT);

    std::int32_t replacement = 0;
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_EVENT,
                   &replacement, static_cast<int>(sizeof(replacement))),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVOP);
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_SNDDATA,
                   &replacement, static_cast<int>(sizeof(replacement))),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVOP);
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_RCVDATA,
                   &replacement, static_cast<int>(sizeof(replacement))),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVOP);

    const SRTSOCKET group = srt_create_group(SRT_GTYPE_BROADCAST);
    REQUIRE(group != SRT_INVALID_SOCK);
    int size = static_cast<int>(sizeof(replacement));
    REQUIRE_EQ(srt_getsockflag(
                   group, SRTO_EVENT, &replacement, &size),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVOP);
    size = static_cast<int>(sizeof(replacement));
    REQUIRE_EQ(srt_getsockflag(
                   group, SRTO_SNDDATA, &replacement, &size),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVOP);
    size = static_cast<int>(sizeof(replacement));
    REQUIRE_EQ(srt_getsockflag(
                   group, SRTO_RCVDATA, &replacement, &size),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVOP);

    const SRTSOCKET delayed = srt_create_socket();
    REQUIRE(delayed != SRT_INVALID_SOCK);
    const auto delayed_runtime = attach_test_runtime(
        delayed, peer, SequenceNumber{6'000},
        NegotiatedLiveOptions{
            .receive_tsbpd = true,
            .receive_delay_milliseconds = 1'000,
        });
    delayed_runtime->process_packet(
        single_packet(SequenceNumber{6'000}, 2, inbound), peer);
    REQUIRE_EQ(read_option(delayed, SRTO_RCVDATA), 1);
    REQUIRE_EQ(read_option(delayed, SRTO_EVENT), SRT_EPOLL_OUT);

    const SRTSOCKET file = srt_create_socket();
    REQUIRE(file != SRT_INVALID_SOCK);
    const std::int32_t file_type = SRTT_FILE;
    REQUIRE_EQ(srt_setsockflag(file, SRTO_TRANSTYPE,
                   &file_type, static_cast<int>(sizeof(file_type))),
        0);
    const linger no_linger{};
    REQUIRE_EQ(srt_setsockflag(file, SRTO_LINGER,
                   &no_linger, static_cast<int>(sizeof(no_linger))),
        0);
    const auto file_runtime = attach_test_runtime(
        file, peer, SequenceNumber{7'000});
    std::array<std::byte, 2'000> file_payload{};
    const auto file_queued = file_runtime->queue_stream(
        file_payload, false, -1);
    REQUIRE_EQ(file_queued.status, MessageIoStatus::success);
    REQUIRE_EQ(file_queued.bytes, file_payload.size());
    REQUIRE_EQ(read_option(file, SRTO_SNDDATA), 2);
    file_runtime->process_packet(
        single_packet(SequenceNumber{7'000}, 3, inbound), peer);
    REQUIRE_EQ(read_option(file, SRTO_RCVDATA), 1);
    REQUIRE_EQ(read_option(file, SRTO_EVENT),
        SRT_EPOLL_IN | SRT_EPOLL_OUT);

    runtime->mark_broken(0);
    REQUIRE_EQ(read_option(socket, SRTO_EVENT),
        SRT_EPOLL_IN | SRT_EPOLL_OUT | SRT_EPOLL_ERR);

    REQUIRE_EQ(srt_close(group), 0);
    REQUIRE_EQ(srt_close(delayed), 0);
    REQUIRE_EQ(srt_close(file), 0);
    REQUIRE_EQ(srt_epoll_release(eid), 0);
    REQUIRE_EQ(srt_close(socket), 0);
    REQUIRE_EQ(srt_cleanup(), 0);
}

TEST(compat_epoll_wakes_at_the_next_tsbpd_delivery_deadline)
{
    const SRTSOCKET socket = srt_create_socket();
    REQUIRE(socket != SRT_INVALID_SOCK);
    const auto record =
        SocketRegistry::instance().find(socket);
    REQUIRE(record != nullptr);

    const Ipv4Endpoint peer{
        .address = {192, 0, 2, 12},
        .port = 9'002,
    };
    const SequenceNumber initial{6'000};
    const auto channel =
        std::make_shared<DatagramChannel>();
    channel->set_send_hook_for_testing(
        accept_datagram, nullptr);
    const auto origin =
        ConnectionRuntime::Clock::now();
    const auto runtime =
        std::make_shared<ConnectionRuntime>(ConnectionRuntime::Configuration {
            .channel = channel,
            .peer = peer,
            .peer_socket_id = 701,
            .initial_sequence = initial,
            .flow_window_packets = 256,
            .options = record->native_options,
            .negotiated_options =
                {
                    .receive_tsbpd = true,
                    .receive_delay_milliseconds = 100,
                },
            .origin = origin,
            .handshake_arrival_microseconds = 0,
            .peer_handshake_timestamp = PacketTimestamp {0},
        });
    {
        std::lock_guard lock(record->mutex);
        record->channel = channel;
        record->runtime = runtime;
        record->peer_endpoint = peer;
        record->has_peer_endpoint = true;
        record->peer_protocol_socket_id = 701;
        record->connection_initial_sequence =
            initial.value();
        record->state = SRTS_CONNECTED;
    }

    const std::array<std::byte, 1> payload{
        std::byte{'t'}};
    auto packet = single_packet(initial, 1, payload);
    const auto packet_time =
        std::chrono::duration_cast<std::chrono::microseconds>(
            ConnectionRuntime::Clock::now() - origin)
            .count();
    REQUIRE(packet_time >= 0);
    packet.data.timestamp =
        PacketTimestamp {static_cast<std::uint32_t>(packet_time)};
    runtime->process_packet(packet, peer);
    const auto expected_deadline = runtime->next_readable_deadline();
    REQUIRE(expected_deadline.has_value());

    const int eid = srt_epoll_create();
    REQUIRE(eid >= 0);
    const int watched = SRT_EPOLL_IN;
    REQUIRE_EQ(srt_epoll_add_usock(
                   eid, socket, &watched),
        0);
    std::array<SRT_EPOLL_EVENT, 1> events{};
    REQUIRE_EQ(srt_epoll_uwait(
                   eid, events.data(), 1, 500),
        1);
    const auto wait_completed = ConnectionRuntime::Clock::now();
    REQUIRE(wait_completed >= *expected_deadline);
    REQUIRE(
        wait_completed - *expected_deadline < std::chrono::milliseconds {250});
    REQUIRE_EQ(events[0].fd, socket);
    REQUIRE_EQ(events[0].events, SRT_EPOLL_IN);

    REQUIRE_EQ(srt_epoll_release(eid), 0);
    REQUIRE_EQ(srt_close(socket), 0);
    REQUIRE_EQ(srt_cleanup(), 0);
}

TEST(compat_epoll_reports_group_updates_once_and_only_through_uwait)
{
    const SRTSOCKET group = srt_create_group(SRT_GTYPE_BROADCAST);
    const SRTSOCKET first = srt_create_socket();
    const SRTSOCKET second = srt_create_socket();
    const SRTSOCKET third = srt_create_socket();
    REQUIRE(group != SRT_INVALID_SOCK);
    REQUIRE(first != SRT_INVALID_SOCK);
    REQUIRE(second != SRT_INVALID_SOCK);
    REQUIRE(third != SRT_INVALID_SOCK);

    sockaddr_storage peer{};
    peer.ss_family = AF_INET;
    std::uint64_t group_generation = 0;
    std::array<std::uint64_t, 3> member_generations{};
    const std::array<SRTSOCKET, 3> members{first, second, third};
    for (std::size_t index = 0; index < members.size(); ++index) {
        REQUIRE(GroupRegistry::instance().add_member(
            group, members[index], peer, 0U,
            static_cast<int>(index), group_generation,
            member_generations[index]));
        GroupRegistry::instance().update_member(
            group, group_generation, members[index],
            member_generations[index], SRTS_CONNECTED,
            SRT_SUCCESS);
    }
    GroupRegistry::instance().mark_opened(group, group_generation);

    const int eid = srt_epoll_create();
    REQUIRE(eid >= 0);
    const int watched = SRT_EPOLL_UPDATE;
    REQUIRE_EQ(srt_epoll_add_usock(eid, group, &watched), 0);
    std::array<SRT_EPOLL_EVENT, 2> events{};
    REQUIRE_EQ(srt_epoll_uwait(
                   eid, events.data(),
                   static_cast<int>(events.size()), 0),
        0);

    GroupRegistry::instance().update_member(
        group, group_generation, first, member_generations[0],
        SRTS_BROKEN, SRT_ECONNLOST, true);
    REQUIRE_EQ(srt_epoll_uwait(
                   eid, events.data(),
                   static_cast<int>(events.size()), 0),
        1);
    REQUIRE_EQ(events[0].fd, group);
    REQUIRE_EQ(events[0].events, SRT_EPOLL_UPDATE);
    REQUIRE_EQ(srt_epoll_uwait(
                   eid, events.data(),
                   static_cast<int>(events.size()), 0),
        0);

    GroupRegistry::instance().update_member(
        group, group_generation, second, member_generations[1],
        SRTS_BROKEN, SRT_ECONNLOST, true);
    SRTSOCKET read_ready = SRT_INVALID_SOCK;
    int read_count = 1;
    SRTSOCKET write_ready = SRT_INVALID_SOCK;
    int write_count = 1;
    REQUIRE_EQ(srt_epoll_wait(eid,
                   &read_ready, &read_count,
                   &write_ready, &write_count, 0,
                   nullptr, nullptr, nullptr, nullptr),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ETIMEOUT);
    REQUIRE_EQ(read_count, 0);
    REQUIRE_EQ(write_count, 0);
    REQUIRE_EQ(srt_epoll_uwait(
                   eid, events.data(),
                   static_cast<int>(events.size()), 0),
        1);
    REQUIRE_EQ(events[0].events, SRT_EPOLL_UPDATE);

    GroupRegistry::instance().update_member(
        group, group_generation, third, member_generations[2],
        SRTS_BROKEN, SRT_ECONNLOST, true);
    REQUIRE_EQ(srt_epoll_uwait(
                   eid, events.data(),
                   static_cast<int>(events.size()), 0),
        0);

    REQUIRE_EQ(srt_epoll_release(eid), 0);
    REQUIRE_EQ(srt_close(group), 0);
    REQUIRE_EQ(srt_cleanup(), 0);
}

TEST(compat_group_epoll_observes_runtime_break_without_state_polling)
{
    const SRTSOCKET group = srt_create_group(SRT_GTYPE_BACKUP);
    const SRTSOCKET first = srt_create_socket();
    const SRTSOCKET second = srt_create_socket();
    REQUIRE(group != SRT_INVALID_SOCK);
    REQUIRE(first != SRT_INVALID_SOCK);
    REQUIRE(second != SRT_INVALID_SOCK);
    const Ipv4Endpoint peer{
        .address = {203, 0, 113, 40},
        .port = 10'040,
    };
    const auto first_runtime =
        attach_test_runtime(first, peer, SequenceNumber{10'000});
    (void)attach_test_runtime(second, peer, SequenceNumber{10'000});

    sockaddr_storage peer_address{};
    peer_address.ss_family = AF_INET;
    std::uint64_t group_generation = 0;
    std::array<std::uint64_t, 2> member_generations{};
    const std::array<SRTSOCKET, 2> members{first, second};
    for (std::size_t index = 0; index < members.size(); ++index) {
        REQUIRE(GroupRegistry::instance().add_member(
            group, members[index], peer_address, 0U,
            static_cast<int>(index), group_generation,
            member_generations[index]));
        GroupRegistry::instance().update_member(
            group, group_generation, members[index],
            member_generations[index], SRTS_CONNECTED,
            SRT_SUCCESS);
        const auto socket =
            SocketRegistry::instance().find(members[index]);
        REQUIRE(socket != nullptr);
        std::lock_guard lock(socket->mutex);
        socket->group_id = group;
        socket->group_generation = group_generation;
        socket->member_generation = member_generations[index];
    }
    GroupRegistry::instance().mark_opened(group, group_generation);

    const int eid = srt_epoll_create();
    REQUIRE(eid >= 0);
    const int watched = SRT_EPOLL_UPDATE;
    REQUIRE_EQ(srt_epoll_add_usock(eid, group, &watched), 0);

    PacketView shutdown;
    shutdown.kind = PacketKind::control;
    shutdown.control.type = ControlType::shutdown;
    shutdown.control.destination_socket_id =
        static_cast<std::uint32_t>(first);
    const std::array<std::byte, 4> shutdown_padding {};
    shutdown.payload = shutdown_padding;
    first_runtime->process_packet(shutdown, peer);

    std::array<SRT_EPOLL_EVENT, 1> events{};
    REQUIRE_EQ(srt_epoll_uwait(
                   eid, events.data(),
                   static_cast<int>(events.size()), 0),
        1);
    REQUIRE_EQ(events[0].fd, group);
    REQUIRE_EQ(events[0].events, SRT_EPOLL_UPDATE);
    REQUIRE_EQ(srt_getsockstate(first), SRTS_BROKEN);

    REQUIRE_EQ(srt_epoll_release(eid), 0);
    REQUIRE_EQ(srt_close(group), 0);
    REQUIRE_EQ(srt_cleanup(), 0);
}

TEST(compat_live_message_receive_enforces_the_maximum_payload_buffer)
{
    const SRTSOCKET socket = srt_create_socket();
    REQUIRE(socket != SRT_INVALID_SOCK);
    const Ipv4Endpoint peer{
        .address = {192, 0, 2, 11},
        .port = 9'001,
    };
    const SequenceNumber initial{5'000};
    const auto runtime =
        attach_test_runtime(socket, peer, initial);
    const std::array payload{
        std::byte{'s'}, std::byte{'m'}, std::byte{'a'},
        std::byte{'l'}, std::byte{'l'}};

    runtime->process_packet(
        single_packet(initial, 1, payload), peer);
    std::array<char, SRT_LIVE_DEF_PLSIZE> received {};
    REQUIRE_EQ(srt_recvmsg(socket, received.data(),
                   static_cast<int>(received.size())),
        static_cast<int>(payload.size()));
    REQUIRE(std::memcmp(received.data(), payload.data(),
                payload.size())
        == 0);

    runtime->process_packet(
        single_packet(initial.next(), 2, payload), peer);
    std::array<char, 64> too_small {};
    REQUIRE_EQ(srt_recvmsg(socket, too_small.data(),
                   static_cast<int>(too_small.size())),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVALMSGAPI);

    received.fill(0);
    REQUIRE_EQ(srt_recvmsg(socket, received.data(),
                   static_cast<int>(received.size())),
        static_cast<int>(payload.size()));
    REQUIRE(std::memcmp(received.data(), payload.data(),
                payload.size())
        == 0);

    REQUIRE_EQ(srt_close(socket), 0);
    REQUIRE_EQ(srt_cleanup(), 0);
}

TEST(compat_epoll_wait_wakes_on_runtime_events_and_release)
{
    const SRTSOCKET socket = srt_create_socket();
    REQUIRE(socket != SRT_INVALID_SOCK);
    const Ipv4Endpoint peer{
        .address = {198, 51, 100, 20},
        .port = 10'000,
    };
    const SequenceNumber initial{8'000};
    const auto runtime =
        attach_test_runtime(socket, peer, initial);

    const int eid = srt_epoll_create();
    REQUIRE(eid >= 0);
    const int watched = SRT_EPOLL_IN;
    REQUIRE_EQ(srt_epoll_add_usock(eid, socket, &watched), 0);

    std::promise<void> waiting;
    auto entered = waiting.get_future();
    auto result = std::async(std::launch::async,
        [eid, &waiting] {
            std::array<SRT_EPOLL_EVENT, 1> events{};
            waiting.set_value();
            const int count = srt_epoll_uwait(
                eid, events.data(),
                static_cast<int>(events.size()), 1'000);
            return std::pair{count, events[0]};
        });
    entered.wait();

    const std::array<std::byte, 1> payload{std::byte{9}};
    const PacketView packet =
        single_packet(initial, 1, payload);
    runtime->process_packet(packet, peer);
    const auto wake_result = result.get();
    REQUIRE_EQ(wake_result.first, 1);
    REQUIRE_EQ(wake_result.second.fd, socket);
    REQUIRE_EQ(wake_result.second.events, SRT_EPOLL_IN);

    REQUIRE_EQ(srt_epoll_release(eid), 0);

    const int empty_eid = srt_epoll_create();
    REQUIRE(empty_eid >= 0);
    REQUIRE_EQ(srt_epoll_set(
                   empty_eid, SRT_EPOLL_ENABLE_EMPTY),
        0);
    std::promise<void> release_waiting;
    auto release_entered = release_waiting.get_future();
    auto release_result = std::async(std::launch::async,
        [empty_eid, &release_waiting] {
            std::array<SRT_EPOLL_EVENT, 1> events{};
            release_waiting.set_value();
            const int value = srt_epoll_uwait(
                empty_eid, events.data(),
                static_cast<int>(events.size()), -1);
            return std::pair{
                value, srt_getlasterror(nullptr)};
        });
    release_entered.wait();
    REQUIRE_EQ(srt_epoll_release(empty_eid), 0);
    const auto released = release_result.get();
    REQUIRE_EQ(released.first, SRT_ERROR);
    REQUIRE_EQ(released.second, SRT_EINVPOLLID);

    REQUIRE_EQ(srt_close(socket), 0);
    REQUIRE_EQ(srt_cleanup(), 0);
}

TEST(compat_readiness_signal_wakes_all_registered_waiters)
{
    const std::uint64_t observed = ReadinessSignal::generation();
    const auto deadline =
        ReadinessSignal::Clock::now() + std::chrono::seconds {2};

    std::promise<void> first_entering;
    std::promise<void> second_entering;
    auto first_entered = first_entering.get_future();
    auto second_entered = second_entering.get_future();
    auto first =
        std::async(std::launch::async, [observed, deadline, &first_entering] {
            first_entering.set_value();
            ReadinessSignal::wait_until(observed, deadline);
        });
    auto second =
        std::async(std::launch::async, [observed, deadline, &second_entering] {
            second_entering.set_value();
            ReadinessSignal::wait_until(observed, deadline);
        });
    first_entered.wait();
    second_entered.wait();

    ReadinessSignal::notify();
    REQUIRE_EQ(
        first.wait_for(std::chrono::seconds {1}), std::future_status::ready);
    REQUIRE_EQ(
        second.wait_for(std::chrono::seconds {1}), std::future_status::ready);
    first.get();
    second.get();
}

TEST(compat_epoll_reports_peer_shutdown_as_a_broken_connection)
{
    const SRTSOCKET socket = srt_create_socket();
    REQUIRE(socket != SRT_INVALID_SOCK);
    const Ipv4Endpoint peer{
        .address = {203, 0, 113, 25},
        .port = 10'025,
    };
    const auto runtime =
        attach_test_runtime(socket, peer, SequenceNumber{9'000});

    const int eid = srt_epoll_create();
    REQUIRE(eid >= 0);
    const int watched =
        SRT_EPOLL_IN | SRT_EPOLL_OUT | SRT_EPOLL_ERR;
    REQUIRE_EQ(srt_epoll_add_usock(eid, socket, &watched), 0);

    PacketView shutdown;
    shutdown.kind = PacketKind::control;
    shutdown.control.type = ControlType::shutdown;
    shutdown.control.destination_socket_id =
        static_cast<std::uint32_t>(socket);
    const std::array<std::byte, 4> shutdown_padding {};
    shutdown.payload = shutdown_padding;
    runtime->process_packet(shutdown, peer);

    REQUIRE_EQ(srt_getsockstate(socket), SRTS_BROKEN);
    std::array<SRT_EPOLL_EVENT, 1> events{};
    REQUIRE_EQ(srt_epoll_uwait(
                   eid, events.data(),
                   static_cast<int>(events.size()), 0),
        1);
    REQUIRE_EQ(events[0].fd, socket);
    REQUIRE_EQ(events[0].events, watched);

    std::array<char, 1'500> buffer{};
    REQUIRE_EQ(srt_recv(socket, buffer.data(),
                   static_cast<int>(buffer.size())),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ECONNLOST);

    REQUIRE_EQ(srt_epoll_release(eid), 0);
    REQUIRE_EQ(srt_close(socket), 0);
    REQUIRE_EQ(srt_cleanup(), 0);
}

TEST(compat_epoll_drains_tsbpd_data_before_peer_shutdown_error)
{
    const SRTSOCKET socket = srt_create_socket();
    REQUIRE(socket != SRT_INVALID_SOCK);
    const Ipv4Endpoint peer {
        .address = {203, 0, 113, 26},
        .port = 10'026,
    };
    const SequenceNumber initial {9'100};
    const auto runtime = attach_test_runtime(socket, peer, initial,
        NegotiatedLiveOptions {
            .receive_tsbpd = true,
            .receive_delay_milliseconds = 100,
        });

    const std::array<std::byte, 4> payload {
        std::byte {'t'}, std::byte {'a'}, std::byte {'i'}, std::byte {'l'}};
    runtime->process_packet(single_packet(initial, 1, payload), peer);

    PacketView shutdown;
    shutdown.kind = PacketKind::control;
    shutdown.control.type = ControlType::shutdown;
    shutdown.control.destination_socket_id = static_cast<std::uint32_t>(socket);
    const std::array<std::byte, 4> shutdown_padding {};
    shutdown.payload = shutdown_padding;
    runtime->process_packet(shutdown, peer);
    // SHUTDOWN may arrive after socket_readiness captures CONNECTED but before
    // it inspects the runtime. That stale snapshot must still drain TSBPD data.
    const auto stale_pending = connection_readiness(*runtime, false);
    REQUIRE_EQ(stale_pending.events & SRT_EPOLL_ERR, 0);
    REQUIRE(stale_pending.read_wakeup.has_value()
        || (stale_pending.events & SRT_EPOLL_IN) != 0);
    REQUIRE_EQ(srt_getsockstate(socket), SRTS_BROKEN);

    const int eid = srt_epoll_create();
    REQUIRE(eid >= 0);
    const int watched = SRT_EPOLL_IN | SRT_EPOLL_OUT | SRT_EPOLL_ERR;
    REQUIRE_EQ(srt_epoll_add_usock(eid, socket, &watched), 0);

    std::array<SRT_EPOLL_EVENT, 1> events {};
    REQUIRE_EQ(srt_epoll_uwait(eid, events.data(), 1, 0), 0);
    REQUIRE_EQ(srt_epoll_uwait(eid, events.data(), 1, 500), 1);
    REQUIRE_EQ(events[0].fd, socket);
    REQUIRE_EQ(events[0].events, SRT_EPOLL_IN);

    const auto stale_ready = connection_readiness(*runtime, false);
    REQUIRE_EQ(stale_ready.events, SRT_EPOLL_IN);

    SRTSOCKET read_ready = SRT_INVALID_SOCK;
    int read_count = 1;
    SRTSOCKET write_ready = SRT_INVALID_SOCK;
    int write_count = 1;
    REQUIRE_EQ(srt_epoll_wait(eid, &read_ready, &read_count, &write_ready,
                   &write_count, 0, nullptr, nullptr, nullptr, nullptr),
        1);
    REQUIRE_EQ(read_count, 1);
    REQUIRE_EQ(write_count, 0);
    REQUIRE_EQ(read_ready, socket);

    std::array<char, SRT_LIVE_DEF_PLSIZE> received {};
    REQUIRE_EQ(
        srt_recvmsg(socket, received.data(), static_cast<int>(received.size())),
        static_cast<int>(payload.size()));
    REQUIRE(std::equal(payload.begin(), payload.end(),
        reinterpret_cast<const std::byte*>(received.data())));

    REQUIRE_EQ(srt_epoll_uwait(eid, events.data(), 1, 0), 1);
    REQUIRE_EQ(events[0].events, watched);
    REQUIRE_EQ(connection_readiness(*runtime, false).events, watched);
    REQUIRE_EQ(
        srt_recvmsg(socket, received.data(), static_cast<int>(received.size())),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ECONNLOST);

    REQUIRE_EQ(srt_epoll_release(eid), 0);
    REQUIRE_EQ(srt_close(socket), 0);
    REQUIRE_EQ(srt_cleanup(), 0);
}

#if !defined(_WIN32)
TEST(compat_epoll_wait_integrates_system_socket_readiness)
{
    std::array<int, 2> pipe_fds{};
    REQUIRE_EQ(pipe(pipe_fds.data()), 0);

    const int eid = srt_epoll_create();
    REQUIRE(eid >= 0);
    const int watched = SRT_EPOLL_IN | SRT_EPOLL_ERR;
    REQUIRE_EQ(srt_epoll_add_ssock(eid, -1, &watched),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ECONNSETUP);
    REQUIRE_EQ(
        srt_epoll_add_ssock(eid, pipe_fds[0], &watched), 0);

    std::array<SYSSOCKET, 1> readable{};
    int readable_count = static_cast<int>(readable.size());
    REQUIRE_EQ(srt_epoll_wait(eid,
                   nullptr, nullptr, nullptr, nullptr, 0,
                   readable.data(), &readable_count,
                   nullptr, nullptr),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ETIMEOUT);
    REQUIRE_EQ(readable_count, 0);

    const char byte = 'x';
    REQUIRE_EQ(write(pipe_fds[1], &byte, 1), 1);
    readable_count = static_cast<int>(readable.size());
    REQUIRE_EQ(srt_epoll_wait(eid,
                   nullptr, nullptr, nullptr, nullptr, 100,
                   readable.data(), &readable_count,
                   nullptr, nullptr),
        1);
    REQUIRE_EQ(readable_count, 1);
    REQUIRE_EQ(readable[0], pipe_fds[0]);

    std::array<SRT_EPOLL_EVENT, 1> events{};
    REQUIRE_EQ(srt_epoll_uwait(
                   eid, events.data(),
                   static_cast<int>(events.size()), 0),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);

    REQUIRE_EQ(srt_epoll_release(eid), 0);
    REQUIRE_EQ(close(pipe_fds[0]), 0);
    REQUIRE_EQ(close(pipe_fds[1]), 0);
    REQUIRE_EQ(srt_cleanup(), 0);
}
#endif
