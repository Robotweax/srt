#include "compat/group_receive_retention.hpp"
#include "test.hpp"

#include "compat/group_config.hpp"
#include "compat/group_replay_buffer.hpp"
#include "compat/epoll.hpp"
#include "compat/group_registry.hpp"
#include "compat/message_io.hpp"
#include "compat/socket_registry.hpp"
#include "compat/transport_runtime.hpp"
#include "robotweax/srt/codec.hpp"
#include "robotweax/srt/control.hpp"
#include "srt/srt.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <future>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace {

using robotweax::srt::compat::GroupRegistry;
using robotweax::srt::compat::GroupReplayBuffer;
using robotweax::srt::compat::SocketRegistry;
using robotweax::srt::compat::ConnectionRuntime;
using robotweax::srt::IpEndpoint;
using robotweax::srt::SequenceNumber;

struct TestClock {
    std::uint64_t now_microseconds = 0;
    std::shared_ptr<robotweax::srt::compat::DatagramChannel> channel;
};

struct CapturedGroupDatagrams {
    std::mutex mutex;
    std::vector<std::vector<std::byte>> values;
};

std::uint64_t read_test_clock(void* context) noexcept
{
    return static_cast<TestClock*>(context)->now_microseconds;
}

robotweax::srt::UdpIoResult accept_test_datagram(
    std::span<const std::byte> bytes,
    IpEndpoint,
    void*) noexcept
{
    return {.bytes_transferred = bytes.size()};
}

robotweax::srt::UdpIoResult capture_group_datagram(
    std::span<const std::byte> bytes,
    IpEndpoint,
    void* context) noexcept
{
    auto& captured = *static_cast<CapturedGroupDatagrams*>(context);
    try {
        std::lock_guard lock(captured.mutex);
        captured.values.emplace_back(bytes.begin(), bytes.end());
        return {.bytes_transferred = bytes.size()};
    } catch (...) {
        return {.error = robotweax::srt::Error::io_error};
    }
}

std::vector<std::vector<std::byte>> take_group_datagrams(
    CapturedGroupDatagrams& captured)
{
    std::lock_guard lock(captured.mutex);
    std::vector<std::vector<std::byte>> values;
    values.swap(captured.values);
    return values;
}

void deliver_lite_ack(
    const std::shared_ptr<ConnectionRuntime>& runtime,
    SequenceNumber next_sequence)
{
    std::array<std::byte, 4> payload{};
    const auto encoded = robotweax::srt::encode_acknowledgement_payload(
        {
            .kind = robotweax::srt::AcknowledgementKind::lite,
            .next_sequence = next_sequence,
        },
        payload);
    REQUIRE(encoded);
    robotweax::srt::PacketView packet;
    packet.kind = robotweax::srt::PacketKind::control;
    packet.control.type =
        robotweax::srt::ControlType::acknowledgement;
    packet.payload = std::span{payload}.first(encoded.bytes_written);
    runtime->process_packet(packet, IpEndpoint::loopback(9'000));
}

std::shared_ptr<ConnectionRuntime> attach_group_runtime(SRTSOCKET group,
    SRTSOCKET socket, std::uint32_t initial_sequence, std::uint16_t weight = 1,
    TestClock* clock = nullptr, std::size_t send_capacity_packets = 0U,
    bool receive_tsbpd = false, std::uint16_t receive_delay_milliseconds = 0U,
    ConnectionRuntime::Clock::time_point origin =
        ConnectionRuntime::Clock::now(),
    const std::shared_ptr<robotweax::srt::compat::GroupRecord>& timing_group =
        {},
    robotweax::srt::PacketTimestamp handshake_timestamp = {},
    ConnectionRuntime::ReceivePopHook receive_pop_hook = nullptr,
    void* receive_pop_context = nullptr,
    bool receive_too_late_packet_drop = false,
    std::size_t receive_capacity_packets = 0U)
{
    robotweax::srt::SocketOptions options;
    REQUIRE_EQ(options.set(
                   robotweax::srt::SocketOption::tsbpd_mode,
                   receive_tsbpd ? 1 : 0),
        robotweax::srt::Error::none);
    if (send_capacity_packets != 0U) {
        REQUIRE_EQ(options.set(
                       robotweax::srt::SocketOption::send_buffer_packets,
                       static_cast<std::int64_t>(send_capacity_packets)),
            robotweax::srt::Error::none);
    }
    if (receive_capacity_packets != 0U) {
        REQUIRE_EQ(
            options.set(robotweax::srt::SocketOption::receive_buffer_packets,
                static_cast<std::int64_t>(receive_capacity_packets)),
            robotweax::srt::Error::none);
    }
    const IpEndpoint peer = IpEndpoint::loopback(9'000);
    auto runtime =
        std::make_shared<ConnectionRuntime>(ConnectionRuntime::Configuration {
            .channel = clock == nullptr
                ? std::weak_ptr<robotweax::srt::compat::DatagramChannel> {}
                : std::weak_ptr {clock->channel},
            .group = timing_group,
            .peer = peer,
            .peer_socket_id = 77,
            .initial_sequence = SequenceNumber {initial_sequence},
            .peer_initial_sequence = SequenceNumber {initial_sequence},
            .has_distinct_peer_initial_sequence = true,
            .options = options,
            .negotiated_options =
                {
                    .receive_tsbpd = receive_tsbpd,
                    .too_late_packet_drop = receive_too_late_packet_drop,
                    .receive_delay_milliseconds = receive_delay_milliseconds,
                },
            .origin = origin,
            .handshake_arrival_microseconds =
                clock == nullptr ? 0 : clock->now_microseconds,
            .peer_handshake_timestamp = handshake_timestamp,
            .now_function = clock == nullptr ? nullptr : read_test_clock,
            .now_context = clock,
            .receive_pop_hook_for_testing = receive_pop_hook,
            .receive_pop_context_for_testing = receive_pop_context,
        });
    sockaddr_storage peer_storage{};
    auto& peer4 = reinterpret_cast<sockaddr_in&>(peer_storage);
    peer4.sin_family = AF_INET;
    peer4.sin_port = htons(peer.port);
    peer4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    std::uint64_t group_generation = 0;
    std::uint64_t member_generation = 0;
    REQUIRE(GroupRegistry::instance().add_member(
        group, socket, peer_storage, weight, socket,
        group_generation, member_generation));
    const auto record = SocketRegistry::instance().find(socket);
    REQUIRE(record != nullptr);
    const auto group_record = GroupRegistry::instance().find(group);
    REQUIRE(group_record != nullptr);
    SRT_GROUP_TYPE group_type = SRT_GTYPE_UNDEFINED;
    {
        std::lock_guard lock(group_record->mutex);
        group_type = group_record->type;
    }
    {
        std::lock_guard lock(record->mutex);
        record->state = SRTS_CONNECTED;
        record->runtime = runtime;
        record->group_id = group;
        record->group_generation = group_generation;
        record->member_generation = member_generation;
        record->group_type = group_type;
        record->group_weight = weight;
    }
    GroupRegistry::instance().update_member(
        group, group_generation, socket, member_generation,
        SRTS_CONNECTED, SRT_SUCCESS);
    GroupRegistry::instance().mark_opened(
        group, group_generation);
    return runtime;
}

void observe_group_connect(
    void*, SRTSOCKET, int, const sockaddr*, int)
{
}

sockaddr_in ipv4_address(std::uint16_t port)
{
    sockaddr_in result{};
    result.sin_family = AF_INET;
    result.sin_port = htons(port);
    result.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return result;
}

sockaddr_in6 ipv6_address(std::uint16_t port)
{
    sockaddr_in6 result{};
    result.sin6_family = AF_INET6;
    result.sin6_port = htons(port);
    result.sin6_addr = in6addr_loopback;
    return result;
}

} // namespace

TEST(compat_group_registry_allocates_distinct_masked_tombstoned_handles)
{
    const SRTSOCKET broadcast = srt_create_group(SRT_GTYPE_BROADCAST);
    const SRTSOCKET backup = srt_create_group(SRT_GTYPE_BACKUP);
    REQUIRE(broadcast != SRT_INVALID_SOCK);
    REQUIRE(backup != SRT_INVALID_SOCK);
    REQUIRE(broadcast != backup);
    REQUIRE((broadcast & SRTGROUP_MASK) != 0);
    REQUIRE((backup & SRTGROUP_MASK) != 0);
    REQUIRE_EQ(srt_getsockstate(broadcast), SRTS_BROKEN);
    REQUIRE_EQ(srt_getsockstate(backup), SRTS_BROKEN);

    REQUIRE_EQ(srt_close(broadcast), 0);
    REQUIRE_EQ(srt_getsockstate(broadcast), SRTS_CLOSED);
    REQUIRE_EQ(srt_close(broadcast), 0);

    const SRTSOCKET replacement =
        srt_create_group(SRT_GTYPE_BROADCAST);
    REQUIRE(replacement != SRT_INVALID_SOCK);
    REQUIRE(replacement != broadcast);
    REQUIRE_EQ(srt_getsockstate(broadcast), SRTS_CLOSED);
    REQUIRE_EQ(srt_close(backup), 0);
    REQUIRE_EQ(srt_close(replacement), 0);
}

TEST(compat_group_bstats_requires_connection_and_uses_its_start_time)
{
    const SRTSOCKET group = srt_create_group(SRT_GTYPE_BROADCAST);
    REQUIRE(group != SRT_INVALID_SOCK);
    SRT_TRACEBSTATS statistics {};
    REQUIRE_EQ(srt_bstats(group, &statistics, 0), SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ENOCONN);
    REQUIRE_EQ(srt_bistats(group, &statistics, 0, 1), SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ENOCONN);

    const auto record = GroupRegistry::instance().find(group);
    REQUIRE(record != nullptr);
    std::uint32_t initial_sequence = 0;
    {
        std::lock_guard lock(record->mutex);
        record->timestamp_origin =
            std::chrono::steady_clock::now() - std::chrono::seconds {5};
        initial_sequence = record->initial_sequence;
    }
    const SRTSOCKET member = srt_create_socket();
    REQUIRE(member != SRT_INVALID_SOCK);
    (void)attach_group_runtime(group, member, initial_sequence);
    REQUIRE_EQ(srt_bstats(group, &statistics, 0), 0);
    REQUIRE(statistics.msTimeStamp >= 5'000);
    REQUIRE_EQ(srt_close(group), 0);
    REQUIRE_EQ(srt_bstats(group, &statistics, 0), SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVSOCK);
}

TEST(
    compat_group_peer_version_uses_the_first_member_and_group_type_is_socket_only)
{
    const SRTSOCKET group = srt_create_group(SRT_GTYPE_BROADCAST);
    const SRTSOCKET socket = srt_create_socket();
    REQUIRE(group != SRT_INVALID_SOCK);
    REQUIRE(socket != SRT_INVALID_SOCK);

    std::int32_t value = -1;
    int value_size = static_cast<int>(sizeof(value));
    REQUIRE_EQ(
        srt_getsockflag(group, SRTO_PEERVERSION, &value, &value_size), 0);
    REQUIRE_EQ(value, 0);

    value_size = static_cast<int>(sizeof(value));
    REQUIRE_EQ(
        srt_getsockflag(group, SRTO_GROUPTYPE, &value, &value_size), SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVOP);
    REQUIRE_EQ(srt_setsockflag(group, SRTO_PEERVERSION,
                   &value, static_cast<int>(sizeof(value))),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVOP);
    REQUIRE_EQ(srt_setsockflag(group, SRTO_GROUPTYPE,
                   &value, static_cast<int>(sizeof(value))),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVOP);

    const auto group_record = GroupRegistry::instance().find(group);
    const auto socket_record = SocketRegistry::instance().find(socket);
    REQUIRE(group_record != nullptr);
    REQUIRE(socket_record != nullptr);
    std::uint64_t group_generation = 0;
    {
        std::lock_guard lock(group_record->mutex);
        group_generation = group_record->generation;
    }
    sockaddr_storage peer{};
    auto& peer4 = reinterpret_cast<sockaddr_in&>(peer);
    peer4.sin_family = AF_INET;
    peer4.sin_port = htons(9'000);
    peer4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    std::uint64_t observed_group_generation = 0;
    std::uint64_t member_generation = 0;
    REQUIRE(GroupRegistry::instance().add_member(
        group, socket, peer, 1U, 1,
        observed_group_generation, member_generation));
    REQUIRE_EQ(observed_group_generation, group_generation);
    {
        std::lock_guard lock(socket_record->mutex);
        socket_record->peer_srt_version = 0x0001'0505U;
    }

    value = 0;
    value_size = static_cast<int>(sizeof(value));
    REQUIRE_EQ(srt_getsockflag(
                   group, SRTO_PEERVERSION, &value, &value_size),
        0);
    REQUIRE_EQ(value, 0x0001'0505);

    REQUIRE_EQ(srt_close(group), 0);
    REQUIRE_EQ(srt_close(socket), 0);
}

TEST(compat_group_registry_supports_size_queries_and_generation_checked_membership)
{
    const SRTSOCKET group = srt_create_group(SRT_GTYPE_BROADCAST);
    const SRTSOCKET socket = srt_create_socket();
    REQUIRE(group != SRT_INVALID_SOCK);
    REQUIRE(socket != SRT_INVALID_SOCK);

    std::size_t size = 41;
    REQUIRE_EQ(srt_group_data(group, nullptr, &size), 0);
    REQUIRE_EQ(size, 0U);

    const auto group_record = GroupRegistry::instance().find(group);
    const auto socket_record = SocketRegistry::instance().find(socket);
    REQUIRE(group_record != nullptr);
    REQUIRE(socket_record != nullptr);
    std::uint64_t generation = 0;
    {
        std::lock_guard lock(group_record->mutex);
        generation = group_record->generation;
        robotweax::srt::compat::GroupMemberSnapshot member;
        member.generation = 7;
        member.public_data.id = socket;
        member.public_data.sockstate = SRTS_CONNECTED;
        member.public_data.memberstate = SRT_GST_RUNNING;
        member.public_data.weight = 19;
        member.public_data.result = SRT_SUCCESS;
        member.public_data.token = 73;
        group_record->members.push_back(member);
        ++group_record->snapshot_version;
    }
    {
        std::lock_guard lock(socket_record->mutex);
        socket_record->group_id = group;
        socket_record->group_generation = generation;
        socket_record->member_generation = 7;
    }

    REQUIRE_EQ(srt_groupof(socket), group);
    REQUIRE_EQ(srt_getsockstate(group), SRTS_CONNECTED);
    REQUIRE_EQ(srt_group_data(group, nullptr, &size), 0);
    REQUIRE_EQ(size, 1U);
    size = 0;
    std::array<SRT_SOCKGROUPDATA, 1> data{};
    REQUIRE_EQ(srt_group_data(group, data.data(), &size), SRT_ERROR);
    REQUIRE_EQ(size, 1U);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ELARGEMSG);
    size = data.size();
    REQUIRE_EQ(srt_group_data(group, data.data(), &size), 1);
    REQUIRE_EQ(size, 1U);
    REQUIRE_EQ(data[0].id, socket);
    REQUIRE_EQ(data[0].weight, 19);
    REQUIRE_EQ(data[0].memberstate, SRT_GST_RUNNING);
    REQUIRE_EQ(data[0].token, 73);

    REQUIRE_EQ(srt_close(group), 0);
    REQUIRE_EQ(srt_groupof(socket), SRT_INVALID_SOCK);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);
    REQUIRE_EQ(srt_close(socket), 0);
}

TEST(compat_group_registry_coordinates_member_lifecycle_without_owning_cycles)
{
    const SRTSOCKET group = srt_create_group(SRT_GTYPE_BACKUP);
    const SRTSOCKET socket = srt_create_socket();
    REQUIRE(group != SRT_INVALID_SOCK);
    REQUIRE(socket != SRT_INVALID_SOCK);
    const auto peer = ipv4_address(9'011);
    sockaddr_storage peer_storage{};
    std::memcpy(&peer_storage, &peer, sizeof(peer));

    GroupRegistry::ConnectDescription description;
    REQUIRE(GroupRegistry::instance().describe_connect(
        group, description));
    REQUIRE(description.block_until_connected);
    REQUIRE_EQ(description.type, SRT_GTYPE_BACKUP);

    std::uint64_t group_generation = 0;
    std::uint64_t member_generation = 0;
    REQUIRE(GroupRegistry::instance().add_member(
        group, socket, peer_storage, 13U, 71,
        group_generation, member_generation));
    REQUIRE(member_generation != 0U);
    GroupRegistry::instance().mark_opened(
        group, group_generation);
    REQUIRE(GroupRegistry::instance().describe_connect(
        group, description));
    REQUIRE(!description.block_until_connected);

    const auto socket_record = SocketRegistry::instance().find(socket);
    REQUIRE(socket_record != nullptr);
    {
        std::lock_guard lock(socket_record->mutex);
        socket_record->group_id = group;
        socket_record->group_generation = group_generation;
        socket_record->member_generation = member_generation;
    }
    GroupRegistry::instance().update_member(
        group, group_generation, socket, member_generation,
        SRTS_CONNECTED, SRT_SUCCESS);
    std::array<SRT_SOCKGROUPDATA, 1> data{};
    std::size_t size = data.size();
    REQUIRE_EQ(srt_group_data(group, data.data(), &size), 1);
    REQUIRE_EQ(data[0].sockstate, SRTS_CONNECTED);
    REQUIRE_EQ(data[0].memberstate, SRT_GST_IDLE);
    REQUIRE_EQ(data[0].weight, 13U);
    REQUIRE_EQ(data[0].token, 71);

    REQUIRE(GroupRegistry::instance().set_peer_group(
        group, group_generation, SRTGROUP_MASK | 99));
    REQUIRE(!GroupRegistry::instance().set_peer_group(
        group, group_generation, SRTGROUP_MASK | 100));
    REQUIRE_EQ(srt_close(group), 0);
    REQUIRE_EQ(srt_getsockstate(socket), SRTS_CLOSED);
}

TEST(compat_group_connect_callback_snapshots_current_registration_for_future_members)
{
    int opaque = 17;
    const SRTSOCKET group = srt_create_group(SRT_GTYPE_BROADCAST);
    REQUIRE(group != SRT_INVALID_SOCK);

    REQUIRE_EQ(srt_connect_callback(
                   group, observe_group_connect, &opaque),
        0);
    GroupRegistry::ConnectDescription description;
    REQUIRE(GroupRegistry::instance().describe_connect(
        group, description));
    REQUIRE_EQ(description.connect_callback, observe_group_connect);
    REQUIRE_EQ(description.connect_callback_opaque, &opaque);

    REQUIRE_EQ(srt_connect_callback(group, nullptr, &opaque), 0);
    REQUIRE(GroupRegistry::instance().describe_connect(
        group, description));
    REQUIRE_EQ(description.connect_callback, nullptr);
    REQUIRE_EQ(description.connect_callback_opaque, nullptr);

    REQUIRE_EQ(srt_connect_callback(
                   group, observe_group_connect, &opaque),
        0);
    GroupRegistry::instance().mark_opened(
        group, description.generation);
    REQUIRE_EQ(srt_connect_callback(group, nullptr, nullptr), 0);
    REQUIRE(GroupRegistry::instance().describe_connect(
        group, description));
    REQUIRE(!description.block_until_connected);
    REQUIRE_EQ(description.connect_callback, nullptr);
    REQUIRE_EQ(description.connect_callback_opaque, nullptr);

    REQUIRE_EQ(srt_close(group), 0);
    REQUIRE_EQ(srt_connect_callback(
                   group, observe_group_connect, &opaque),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVSOCK);
    REQUIRE_EQ(srt_connect_callback(
                   SRTGROUP_MASK | 999,
                   observe_group_connect, &opaque),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVSOCK);
}

TEST(compat_group_network_options_are_preconnect_member_defaults)
{
    const SRTSOCKET group = srt_create_group(SRT_GTYPE_BROADCAST);
    REQUIRE(group != SRT_INVALID_SOCK);

    std::int32_t value = 0;
    int size = static_cast<int>(sizeof(value));
    REQUIRE_EQ(srt_getsockflag(group, SRTO_IPTTL, &value, &size), 0);
    REQUIRE_EQ(value, 64);
    size = static_cast<int>(sizeof(value));
    REQUIRE_EQ(srt_getsockflag(group, SRTO_IPTOS, &value, &size), 0);
    REQUIRE_EQ(value, 0xB8);

    value = 37;
    REQUIRE_EQ(srt_setsockflag(group, SRTO_IPTTL, &value,
                   static_cast<int>(sizeof(value))),
        0);
    value = 0x84;
    REQUIRE_EQ(srt_setsockflag(group, SRTO_IPTOS, &value,
                   static_cast<int>(sizeof(value))),
        0);
    GroupRegistry::ConnectDescription description;
    REQUIRE(GroupRegistry::instance().describe_connect(group, description));
    REQUIRE_EQ(description.ip_time_to_live, 37);
    REQUIRE_EQ(description.ip_type_of_service, 0x84);
    REQUIRE(description.ip_type_of_service_explicit);

    value = 0;
    REQUIRE_EQ(srt_setsockflag(group, SRTO_IPTTL, &value,
                   static_cast<int>(sizeof(value))),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);
    value = 256;
    REQUIRE_EQ(srt_setsockflag(group, SRTO_IPTOS, &value,
                   static_cast<int>(sizeof(value))),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);

    char device[16]{};
    size = static_cast<int>(sizeof(device));
    REQUIRE_EQ(srt_getsockflag(
                   group, SRTO_BINDTODEVICE, device, &size),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVOP);
    REQUIRE_EQ(srt_setsockflag(
                   group, SRTO_BINDTODEVICE, "lo", 2),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVOP);

    GroupRegistry::instance().mark_opened(
        group, description.generation);
    value = 38;
    REQUIRE_EQ(srt_setsockflag(group, SRTO_IPTTL, &value,
                   static_cast<int>(sizeof(value))),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ECONNSOCK);
    REQUIRE_EQ(srt_close(group), 0);
}

TEST(compat_group_security_and_idle_options_are_member_defaults)
{
    const SRTSOCKET group = srt_create_group(SRT_GTYPE_BACKUP);
    REQUIRE(group != SRT_INVALID_SOCK);

    constexpr char passphrase[] = "0123456789abcdef";
    std::int32_t key_length = 32;
    std::int32_t refresh_rate = 64;
    std::int32_t preannouncement = 20;
    std::int32_t peer_idle_timeout = 900;
    bool enforced = true;
    REQUIRE_EQ(
        srt_setsockflag(group, SRTO_PASSPHRASE, passphrase, -1), SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);
    REQUIRE_EQ(srt_setsockflag(group, SRTO_PASSPHRASE, passphrase,
                   static_cast<int>(sizeof(passphrase) - 1U)),
        0);
    REQUIRE_EQ(srt_setsockflag(group, SRTO_PBKEYLEN, &key_length,
                   static_cast<int>(sizeof(key_length))),
        0);
    REQUIRE_EQ(srt_setsockflag(group, SRTO_KMREFRESHRATE, &refresh_rate,
                   static_cast<int>(sizeof(refresh_rate))),
        0);
    REQUIRE_EQ(srt_setsockflag(group, SRTO_KMPREANNOUNCE, &preannouncement,
                   static_cast<int>(sizeof(preannouncement))),
        0);
    REQUIRE_EQ(srt_setsockflag(group, SRTO_ENFORCEDENCRYPTION, &enforced,
                   static_cast<int>(sizeof(enforced))),
        0);
    REQUIRE_EQ(srt_setsockflag(group, SRTO_PEERIDLETIMEO, &peer_idle_timeout,
                   static_cast<int>(sizeof(peer_idle_timeout))),
        0);

    std::int32_t integer_value = 0;
    int size = static_cast<int>(sizeof(integer_value));
    REQUIRE_EQ(srt_getsockflag(group, SRTO_PBKEYLEN, &integer_value, &size), 0);
    REQUIRE_EQ(integer_value, key_length);
    size = static_cast<int>(sizeof(integer_value));
    REQUIRE_EQ(
        srt_getsockflag(group, SRTO_PEERIDLETIMEO, &integer_value, &size), 0);
    REQUIRE_EQ(integer_value, peer_idle_timeout);
    bool boolean_value = false;
    size = static_cast<int>(sizeof(boolean_value));
    REQUIRE_EQ(
        srt_getsockflag(group, SRTO_ENFORCEDENCRYPTION, &boolean_value, &size),
        0);
    REQUIRE(boolean_value);
    char secret_output[sizeof(passphrase)] {};
    size = static_cast<int>(sizeof(secret_output));
    REQUIRE_EQ(srt_getsockflag(group, SRTO_PASSPHRASE, secret_output, &size),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVOP);

    GroupRegistry::ConnectDescription description;
    REQUIRE(GroupRegistry::instance().describe_connect(group, description));
    REQUIRE_EQ(description.peer_idle_timeout_milliseconds, peer_idle_timeout);
    REQUIRE_EQ(description.member_native_options.passphrase(),
        (std::string_view {passphrase, sizeof(passphrase) - 1U}));
    REQUIRE_EQ(
        description.member_native_options.configured_encryption_key_length(),
        static_cast<std::size_t>(key_length));
    REQUIRE(description.member_native_options.enforced_encryption());
    REQUIRE_EQ(description.member_native_options
                   .get(robotweax::srt::SocketOption::key_refresh_rate_packets)
                   .value,
        refresh_rate);
    REQUIRE_EQ(
        description.member_native_options
            .get(robotweax::srt::SocketOption::key_preannouncement_packets)
            .value,
        preannouncement);

    GroupRegistry::instance().mark_opened(group, description.generation);
    key_length = 16;
    REQUIRE_EQ(srt_setsockflag(group, SRTO_PBKEYLEN, &key_length,
                   static_cast<int>(sizeof(key_length))),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ECONNSOCK);
    REQUIRE_EQ(srt_setsockflag(group, SRTO_PEERIDLETIMEO, &peer_idle_timeout,
                   static_cast<int>(sizeof(peer_idle_timeout))),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ECONNSOCK);
    REQUIRE_EQ(srt_close(group), 0);
}

#ifdef ENABLE_AEAD_API_PREVIEW
TEST(compat_group_crypto_mode_is_a_preconnect_member_default)
{
    const SRTSOCKET group = srt_create_group(SRT_GTYPE_BACKUP);
    REQUIRE(group != SRT_INVALID_SOCK);

    const std::int32_t gcm = 2;
    REQUIRE_EQ(srt_setsockflag(
                   group, SRTO_CRYPTOMODE, &gcm, static_cast<int>(sizeof(gcm))),
        0);

    std::int32_t observed = -1;
    int size = static_cast<int>(sizeof(observed));
    REQUIRE_EQ(srt_getsockflag(group, SRTO_CRYPTOMODE, &observed, &size), 0);
    REQUIRE_EQ(observed, gcm);

    GroupRegistry::ConnectDescription description;
    REQUIRE(GroupRegistry::instance().describe_connect(group, description));
    REQUIRE_EQ(description.member_native_options
                   .get(robotweax::srt::SocketOption::crypto_mode)
                   .value,
        gcm);

    GroupRegistry::instance().mark_opened(group, description.generation);
    const std::int32_t ctr = 1;
    REQUIRE_EQ(srt_setsockflag(
                   group, SRTO_CRYPTOMODE, &ctr, static_cast<int>(sizeof(ctr))),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ECONNSOCK);
    REQUIRE_EQ(srt_close(group), 0);
}
#endif

TEST(compat_group_listener_option_is_boolean_and_pre_connection)
{
    const SRTSOCKET listener = srt_create_socket();
    REQUIRE(listener != SRT_INVALID_SOCK);
    bool enabled = true;
    int size = static_cast<int>(sizeof(enabled));
    REQUIRE_EQ(srt_getsockflag(
                   listener, SRTO_GROUPCONNECT, &enabled, &size),
        0);
    REQUIRE(!enabled);
    REQUIRE_EQ(size, static_cast<int>(sizeof(enabled)));

    enabled = true;
    REQUIRE_EQ(srt_setsockflag(
                   listener, SRTO_GROUPCONNECT,
                   &enabled, static_cast<int>(sizeof(enabled))),
        0);
    enabled = false;
    size = static_cast<int>(sizeof(enabled));
    REQUIRE_EQ(srt_getsockflag(
                   listener, SRTO_GROUPCONNECT, &enabled, &size),
        0);
    REQUIRE(enabled);

    const int compatible_disabled = 0;
    REQUIRE_EQ(srt_setsockflag(
                   listener, SRTO_GROUPCONNECT,
                   &compatible_disabled,
                   static_cast<int>(sizeof(compatible_disabled))),
        0);
    enabled = true;
    size = static_cast<int>(sizeof(enabled));
    REQUIRE_EQ(srt_getsockflag(
                   listener, SRTO_GROUPCONNECT, &enabled, &size),
        0);
    REQUIRE(!enabled);
    const int invalid_boolean = 2;
    REQUIRE_EQ(srt_setsockflag(
                   listener, SRTO_GROUPCONNECT,
                   &invalid_boolean,
                   static_cast<int>(sizeof(invalid_boolean))),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);

    enabled = true;
    REQUIRE_EQ(srt_setsockflag(
                   listener, SRTO_GROUPCONNECT,
                   &enabled, static_cast<int>(sizeof(enabled))),
        0);
    const auto record = SocketRegistry::instance().find(listener);
    REQUIRE(record != nullptr);
    {
        std::lock_guard lock(record->mutex);
        record->state = SRTS_LISTENING;
    }
    enabled = false;
    REQUIRE_EQ(srt_setsockflag(
                   listener, SRTO_GROUPCONNECT,
                   &enabled, static_cast<int>(sizeof(enabled))),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ECONNSOCK);
    {
        std::lock_guard lock(record->mutex);
        record->state = SRTS_INIT;
    }
    REQUIRE_EQ(srt_close(listener), 0);
}

TEST(compat_group_snapshot_filters_pending_and_stale_identities)
{
    for (const auto type : {SRT_GTYPE_BACKUP, SRT_GTYPE_BROADCAST}) {
        for (const std::size_t count : {16U, 17U, 32U}) {
            const auto group = srt_create_group(type);
            const auto record = GroupRegistry::instance().find(group);
            REQUIRE(record != nullptr);
            std::vector<SRTSOCKET> sockets;
            std::vector<std::shared_ptr<ConnectionRuntime>> runtimes;
            const auto initial = record->initial_sequence;
            for (std::size_t index = 0; index < count; ++index) {
                const auto socket = srt_create_socket();
                REQUIRE(socket != SRT_INVALID_SOCK);
                sockets.push_back(socket);
                runtimes.push_back(
                    attach_group_runtime(group, socket, initial));
                if (index == 0U || index == count / 2U || index == count - 2U) {
                    const auto member = SocketRegistry::instance().find(socket);
                    std::lock_guard lock(member->mutex);
                    if (index == count - 2U) {
                        ++member->member_generation;
                    } else {
                        member->state = SRTS_CONNECTING;
                    }
                }
            }
            const std::array payload {std::byte {'s'}};
            robotweax::srt::PacketView packet;
            packet.kind = robotweax::srt::PacketKind::data;
            packet.data.sequence = SequenceNumber {initial};
            packet.data.message_number = 1;
            packet.data.boundary = robotweax::srt::MessageBoundary::solo;
            packet.data.in_order = true;
            packet.payload = payload;
            // Only the final valid identity may contribute this message.
            runtimes[count - 2U]->process_packet(
                packet, IpEndpoint::loopback(9'000));
            REQUIRE(!robotweax::srt::compat::group_receive_readiness(record)
                    .message_ready);
            runtimes.back()->process_packet(
                packet, IpEndpoint::loopback(9'000));
            REQUIRE(robotweax::srt::compat::group_receive_readiness(record)
                    .message_ready);
            std::array<char, 1> received {};
            REQUIRE_EQ(srt_recvmsg(group, received.data(), received.size()), 1);
            REQUIRE_EQ(received[0], 's');
            REQUIRE_EQ(srt_close(group), 0);
            for (const auto socket : sockets) {
                if (SocketRegistry::instance().find(socket) != nullptr) {
                    REQUIRE_EQ(srt_close(socket), 0);
                }
            }
        }
    }
}

TEST(compat_group_pop_refreshes_a_newly_connected_member_without_a_false_edge)
{
    struct PendingMember {
        std::shared_ptr<robotweax::srt::compat::SocketRecord> socket;
        SRTSOCKET group = SRT_INVALID_SOCK;
        SRTSOCKET id = SRT_INVALID_SOCK;
        std::uint64_t group_generation = 0;
        std::uint64_t member_generation = 0;
    };
    const auto connect_pending = [](void* context) noexcept {
        auto& member = *static_cast<PendingMember*>(context);
        {
            std::lock_guard lock(member.socket->mutex);
            member.socket->state = SRTS_CONNECTED;
        }
        GroupRegistry::instance().update_member(member.group,
            member.group_generation, member.id, member.member_generation,
            SRTS_CONNECTED, SRT_SUCCESS);
    };
    for (const auto type : {SRT_GTYPE_BACKUP, SRT_GTYPE_BROADCAST}) {
        const auto group = srt_create_group(type);
        const auto record = GroupRegistry::instance().find(group);
        REQUIRE(record != nullptr);
        const auto first_socket = srt_create_socket();
        PendingMember pending;
        const auto first = attach_group_runtime(group, first_socket,
            record->initial_sequence, 1, nullptr, 0, false, 0,
            ConnectionRuntime::Clock::now(), {}, {}, connect_pending, &pending);
        const auto next_socket = srt_create_socket();
        const auto next =
            attach_group_runtime(group, next_socket, record->initial_sequence);
        pending.socket = SocketRegistry::instance().find(next_socket);
        pending.group = group;
        pending.id = next_socket;
        {
            std::lock_guard lock(pending.socket->mutex);
            pending.group_generation = pending.socket->group_generation;
            pending.member_generation = pending.socket->member_generation;
            pending.socket->state = SRTS_CONNECTING;
        }
        GroupRegistry::instance().update_member(group, pending.group_generation,
            next_socket, pending.member_generation, SRTS_CONNECTING,
            SRT_SUCCESS);
        const std::array payload {std::byte {'p'}};
        robotweax::srt::PacketView packet;
        packet.kind = robotweax::srt::PacketKind::data;
        packet.data.sequence = SequenceNumber {record->initial_sequence};
        packet.data.message_number = 1;
        packet.data.boundary = robotweax::srt::MessageBoundary::solo;
        packet.data.in_order = true;
        packet.payload = payload;
        first->process_packet(packet, IpEndpoint::loopback(9'000));
        packet.data.sequence = packet.data.sequence.next();
        packet.data.message_number = 2;
        next->process_packet(packet, IpEndpoint::loopback(9'000));
        const int poll = srt_epoll_create();
        const int watched = SRT_EPOLL_IN | SRT_EPOLL_ET;
        REQUIRE_EQ(srt_epoll_add_usock(poll, group, &watched), 0);
        SRT_EPOLL_EVENT event {};
        REQUIRE_EQ(srt_epoll_uwait(poll, &event, 1, 0), 1);
        std::array<char, 1> received {};
        REQUIRE_EQ(srt_recvmsg(group, received.data(), received.size()), 1);
        // Continuous logical IN must not manufacture a new ET event when
        // the next message becomes reachable during the previous pop.
        REQUIRE_EQ(srt_epoll_uwait(poll, &event, 1, 0), 0);
        REQUIRE_EQ(srt_recvmsg(group, received.data(), received.size()), 1);
        REQUIRE_EQ(received[0], 'p');
        REQUIRE_EQ(srt_epoll_release(poll), 0);
        REQUIRE_EQ(srt_close(group), 0);
    }
}

TEST(compat_group_receive_snapshots_preserve_members_across_inline_boundary)
{
    for (const std::size_t count : {2U, 8U, 16U, 17U, 32U}) {
        const auto group = srt_create_group(SRT_GTYPE_BROADCAST);
        REQUIRE(group != SRT_INVALID_SOCK);
        const auto record = GroupRegistry::instance().find(group);
        REQUIRE(record != nullptr);
        constexpr std::uint32_t initial = 500;
        {
            std::lock_guard lock(record->mutex);
            record->next_receive_sequence = initial;
        }
        const bool asynchronous = false;
        REQUIRE_EQ(srt_setsockflag(group, SRTO_RCVSYN, &asynchronous,
                       static_cast<int>(sizeof(asynchronous))),
            0);
        std::vector<SRTSOCKET> sockets;
        std::vector<std::shared_ptr<ConnectionRuntime>> runtimes;
        for (std::size_t index = 0; index < count; ++index) {
            const auto socket = srt_create_socket();
            REQUIRE(socket != SRT_INVALID_SOCK);
            sockets.push_back(socket);
            runtimes.push_back(attach_group_runtime(group, socket, initial, 1,
                nullptr, 0, false, 0, ConnectionRuntime::Clock::now(), record));
        }
        REQUIRE(!robotweax::srt::compat::group_receive_readiness(record)
                .message_ready);
        const int eid = srt_epoll_create();
        REQUIRE(eid >= 0);
        const int watched = SRT_EPOLL_IN | SRT_EPOLL_ERR | SRT_EPOLL_ET;
        REQUIRE_EQ(srt_epoll_add_usock(eid, group, &watched), 0);
        SRT_EPOLL_EVENT event {};
        REQUIRE_EQ(srt_epoll_uwait(eid, &event, 1, 0), 0);
        const std::array<std::byte, 4> payload {
            std::byte {1}, std::byte {2}, std::byte {3}, std::byte {4}};
        robotweax::srt::PacketView packet;
        packet.kind = robotweax::srt::PacketKind::data;
        packet.data.sequence = SequenceNumber {initial};
        packet.data.message_number = 1;
        packet.data.boundary = robotweax::srt::MessageBoundary::solo;
        packet.data.in_order = true;
        packet.payload = payload;
        // A ready member beyond the inline boundary must remain selectable.
        runtimes.back()->process_packet(packet, IpEndpoint::loopback(9'000));
        REQUIRE(robotweax::srt::compat::group_receive_readiness(record)
                .message_ready);
        REQUIRE_EQ(srt_epoll_uwait(eid, &event, 1, 0), 1);
        REQUIRE_EQ(event.fd, group);
        REQUIRE_EQ(event.events, SRT_EPOLL_IN);
        REQUIRE_EQ(srt_epoll_uwait(eid, &event, 1, 0), 0);
        std::array<char, 4> received {};
        REQUIRE_EQ(srt_recvmsg2(group, received.data(),
                       static_cast<int>(received.size()), nullptr),
            4);
        REQUIRE_EQ(
            std::memcmp(received.data(), payload.data(), payload.size()), 0);
        REQUIRE(!robotweax::srt::compat::group_receive_readiness(record)
                .message_ready);
        REQUIRE_EQ(srt_epoll_uwait(eid, &event, 1, 0), 0);
        REQUIRE_EQ(srt_close(sockets.back()), 0);
        packet.data.sequence = packet.data.sequence.next();
        packet.data.message_number = 2;
        runtimes.front()->process_packet(packet, IpEndpoint::loopback(9'000));
        // In particular 17 -> 16 members changes storage without retaining a
        // pointer into the previous snapshot or losing the new cursor.
        REQUIRE(robotweax::srt::compat::group_receive_readiness(record)
                .message_ready);
        REQUIRE_EQ(srt_epoll_uwait(eid, &event, 1, 0), 1);
        REQUIRE_EQ(event.fd, group);
        REQUIRE_EQ(event.events, SRT_EPOLL_IN);
        REQUIRE_EQ(srt_epoll_uwait(eid, &event, 1, 0), 0);
        REQUIRE_EQ(srt_recvmsg2(group, received.data(),
                       static_cast<int>(received.size()), nullptr),
            4);
        REQUIRE_EQ(
            std::memcmp(received.data(), payload.data(), payload.size()), 0);
        REQUIRE_EQ(srt_epoll_uwait(eid, &event, 1, 0), 0);
        // Terminal publication must work without a separate getsockstate
        // query, and the next refresh must see newly pending membership.
        for (std::size_t index = 0; index + 1 < count; ++index) {
            runtimes[index]->mark_broken(9);
        }
        REQUIRE_EQ(srt_epoll_uwait(eid, &event, 1, 0), 1);
        REQUIRE_EQ(event.events, SRT_EPOLL_IN | SRT_EPOLL_ERR);
        REQUIRE_EQ(srt_epoll_uwait(eid, &event, 1, 0), 0);
        const auto pending = srt_create_socket();
        REQUIRE(pending != SRT_INVALID_SOCK);
        sockaddr_storage address {};
        std::uint64_t group_generation = 0;
        std::uint64_t member_generation = 0;
        REQUIRE(GroupRegistry::instance().add_member(group, pending, address, 1,
            pending, group_generation, member_generation));
        const auto pending_record = SocketRegistry::instance().find(pending);
        REQUIRE(pending_record != nullptr);
        {
            std::lock_guard lock(pending_record->mutex);
            pending_record->group_id = group;
            pending_record->group_generation = group_generation;
            pending_record->member_generation = member_generation;
        }
        REQUIRE_EQ(srt_epoll_uwait(eid, &event, 1, 0), 0);
        REQUIRE_EQ(srt_epoll_release(eid), 0);
        REQUIRE_EQ(srt_close(group), 0);
    }
}

TEST(compat_group_registry_scopes_mirrors_to_the_listener)
{
    const SRTSOCKET first_listener = srt_create_socket();
    const SRTSOCKET second_listener = srt_create_socket();
    REQUIRE(first_listener != SRT_INVALID_SOCK);
    REQUIRE(second_listener != SRT_INVALID_SOCK);
    const SRTSOCKET peer_group = SRTGROUP_MASK | 77;

    GroupRegistry::MirrorDescription first;
    REQUIRE(GroupRegistry::instance().prepare_mirror(
        first_listener, peer_group, SRT_GTYPE_BROADCAST,
        91U, first));
    REQUIRE(first.created);
    REQUIRE((first.group & SRTGROUP_MASK) != 0);

    GroupRegistry::MirrorDescription repeated;
    REQUIRE(GroupRegistry::instance().prepare_mirror(
        first_listener, peer_group, SRT_GTYPE_BROADCAST,
        91U, repeated));
    REQUIRE(!repeated.created);
    REQUIRE_EQ(repeated.group, first.group);
    REQUIRE_EQ(repeated.generation, first.generation);

    GroupRegistry::MirrorDescription rejected;
    REQUIRE(!GroupRegistry::instance().prepare_mirror(
        first_listener, peer_group, SRT_GTYPE_BACKUP,
        91U, rejected));
    REQUIRE(GroupRegistry::instance().prepare_mirror(
        first_listener, peer_group, SRT_GTYPE_BROADCAST, 92U, rejected));
    REQUIRE_EQ(rejected.group, first.group);
    REQUIRE(!rejected.created);
    REQUIRE(!GroupRegistry::instance().prepare_mirror(
        first_listener, SRT_INVALID_SOCK, SRT_GTYPE_BROADCAST, 92U, rejected));

    GroupRegistry::MirrorDescription independent;
    REQUIRE(GroupRegistry::instance().prepare_mirror(
        second_listener, peer_group, SRT_GTYPE_BROADCAST,
        91U, independent));
    REQUIRE(independent.created);
    REQUIRE(independent.group != first.group);

    GroupRegistry::instance().release_empty_mirror(
        independent.group, independent.generation);
    REQUIRE(GroupRegistry::instance().find(independent.group) == nullptr);
    REQUIRE_EQ(srt_close(first.group), 0);
    REQUIRE_EQ(srt_close(first_listener), 0);
    REQUIRE_EQ(srt_close(second_listener), 0);
}

TEST(compat_group_registry_accepts_late_member_sequences_across_rollover)
{
    for (const auto type : {SRT_GTYPE_BACKUP, SRT_GTYPE_BROADCAST}) {
        const SRTSOCKET listener = srt_create_socket();
        REQUIRE(listener != SRT_INVALID_SOCK);
        const SRTSOCKET peer_group = SRTGROUP_MASK | 88;
        const SequenceNumber initial {SequenceNumber::mask - 1U};
        GroupRegistry::MirrorDescription mirror;
        REQUIRE(GroupRegistry::instance().prepare_mirror(
            listener, peer_group, type, initial.value(), mirror));
        REQUIRE(mirror.created);
        const auto record = GroupRegistry::instance().find(mirror.group);
        REQUIRE(record != nullptr);

        // The sender has wrapped while the application still has unread
        // messages from the old sequence range.
        GroupRegistry::MirrorDescription joined;
        REQUIRE(GroupRegistry::instance().prepare_mirror(
            listener, peer_group, type, initial.advanced(3).value(), joined));
        REQUIRE_EQ(joined.group, mirror.group);
        REQUIRE(!joined.created);

        // A handshake captured before the application advanced may arrive
        // after the mirror has consumed beyond that member's initial packet.
        {
            std::lock_guard lock(record->mutex);
            record->next_receive_sequence = 2U;
        }
        REQUIRE(GroupRegistry::instance().prepare_mirror(
            listener, peer_group, type, SequenceNumber::mask, joined));
        REQUIRE_EQ(joined.group, mirror.group);
        constexpr std::uint32_t excessive_lag = SequenceNumber::half_range / 2U;
        const SequenceNumber stale {2U - excessive_lag};
        REQUIRE(!GroupRegistry::instance().prepare_mirror(
            listener, peer_group, type, stale.value(), joined));
        REQUIRE(!GroupRegistry::instance().prepare_mirror(
            listener, peer_group, type, SequenceNumber::mask + 1U, joined));

        REQUIRE_EQ(srt_close(mirror.group), 0);
        REQUIRE_EQ(srt_close(listener), 0);
    }
}

TEST(compat_group_registry_mirror_inherits_listener_io_policy)
{
    const SRTSOCKET listener = srt_create_socket();
    REQUIRE(listener != SRT_INVALID_SOCK);

    const bool asynchronous = false;
    constexpr std::int32_t send_timeout_milliseconds = 731;
    constexpr std::int32_t receive_timeout_milliseconds = 947;
    REQUIRE_EQ(srt_setsockflag(listener, SRTO_SNDSYN, &asynchronous,
                   static_cast<int>(sizeof(asynchronous))),
        0);
    REQUIRE_EQ(srt_setsockflag(listener, SRTO_RCVSYN, &asynchronous,
                   static_cast<int>(sizeof(asynchronous))),
        0);
    REQUIRE_EQ(
        srt_setsockflag(listener, SRTO_SNDTIMEO, &send_timeout_milliseconds,
            static_cast<int>(sizeof(send_timeout_milliseconds))),
        0);
    REQUIRE_EQ(
        srt_setsockflag(listener, SRTO_RCVTIMEO, &receive_timeout_milliseconds,
            static_cast<int>(sizeof(receive_timeout_milliseconds))),
        0);

    GroupRegistry::MirrorDescription mirror;
    REQUIRE(GroupRegistry::instance().prepare_mirror(
        listener, SRTGROUP_MASK | 79, SRT_GTYPE_BACKUP, 94U, mirror));
    REQUIRE(mirror.created);

    bool boolean_value = true;
    int size = static_cast<int>(sizeof(boolean_value));
    REQUIRE_EQ(
        srt_getsockflag(mirror.group, SRTO_SNDSYN, &boolean_value, &size), 0);
    REQUIRE(!boolean_value);
    boolean_value = true;
    size = static_cast<int>(sizeof(boolean_value));
    REQUIRE_EQ(
        srt_getsockflag(mirror.group, SRTO_RCVSYN, &boolean_value, &size), 0);
    REQUIRE(!boolean_value);

    std::int32_t timeout = 0;
    size = static_cast<int>(sizeof(timeout));
    REQUIRE_EQ(
        srt_getsockflag(mirror.group, SRTO_SNDTIMEO, &timeout, &size), 0);
    REQUIRE_EQ(timeout, send_timeout_milliseconds);
    timeout = 0;
    size = static_cast<int>(sizeof(timeout));
    REQUIRE_EQ(
        srt_getsockflag(mirror.group, SRTO_RCVTIMEO, &timeout, &size), 0);
    REQUIRE_EQ(timeout, receive_timeout_milliseconds);

    GroupRegistry::instance().release_empty_mirror(
        mirror.group, mirror.generation);
    REQUIRE_EQ(srt_close(listener), 0);
}

TEST(compat_group_registry_scopes_mirrors_to_an_explicit_listener_bond)
{
    const SRTSOCKET first_listener = srt_create_socket();
    const SRTSOCKET second_listener = srt_create_socket();
    const SRTSOCKET independent_listener = srt_create_socket();
    REQUIRE(first_listener != SRT_INVALID_SOCK);
    REQUIRE(second_listener != SRT_INVALID_SOCK);
    REQUIRE(independent_listener != SRT_INVALID_SOCK);

    const auto first_record =
        SocketRegistry::instance().find(first_listener);
    const auto second_record =
        SocketRegistry::instance().find(second_listener);
    const auto independent_record =
        SocketRegistry::instance().find(independent_listener);
    REQUIRE(first_record != nullptr);
    REQUIRE(second_record != nullptr);
    REQUIRE(independent_record != nullptr);
    {
        std::lock_guard lock(first_record->mutex);
        first_record->accept_bond_scope = 41U;
    }
    {
        std::lock_guard lock(second_record->mutex);
        second_record->accept_bond_scope = 41U;
    }
    {
        std::lock_guard lock(independent_record->mutex);
        independent_record->accept_bond_scope = 42U;
    }

    const SRTSOCKET peer_group = SRTGROUP_MASK | 78;
    GroupRegistry::MirrorDescription first;
    REQUIRE(GroupRegistry::instance().prepare_mirror(
        first_listener, peer_group, SRT_GTYPE_BACKUP,
        93U, first));
    REQUIRE(first.created);

    GroupRegistry::MirrorDescription bonded;
    REQUIRE(GroupRegistry::instance().prepare_mirror(
        second_listener, peer_group, SRT_GTYPE_BACKUP,
        93U, bonded));
    REQUIRE(!bonded.created);
    REQUIRE_EQ(bonded.group, first.group);
    REQUIRE_EQ(bonded.generation, first.generation);

    GroupRegistry::MirrorDescription independent;
    REQUIRE(GroupRegistry::instance().prepare_mirror(
        independent_listener, peer_group, SRT_GTYPE_BACKUP,
        93U, independent));
    REQUIRE(independent.created);
    REQUIRE(independent.group != first.group);

    GroupRegistry::instance().release_empty_mirror(
        independent.group, independent.generation);
    REQUIRE_EQ(srt_close(first.group), 0);
    REQUIRE_EQ(srt_close(first_listener), 0);
    REQUIRE_EQ(srt_close(second_listener), 0);
    REQUIRE_EQ(srt_close(independent_listener), 0);
}

TEST(compat_group_registry_marks_only_the_first_mirror_member_for_accept)
{
    const SRTSOCKET listener = srt_create_socket();
    const SRTSOCKET first_socket = srt_create_socket();
    const SRTSOCKET second_socket = srt_create_socket();
    REQUIRE(listener != SRT_INVALID_SOCK);
    REQUIRE(first_socket != SRT_INVALID_SOCK);
    REQUIRE(second_socket != SRT_INVALID_SOCK);

    GroupRegistry::MirrorDescription mirror;
    REQUIRE(GroupRegistry::instance().prepare_mirror(
        listener, SRTGROUP_MASK | 81, SRT_GTYPE_BACKUP,
        101U, mirror));
    sockaddr_storage peer{};
    const auto address = ipv4_address(9'111);
    std::memcpy(&peer, &address, sizeof(address));

    std::uint64_t group_generation = 0;
    std::uint64_t first_generation = 0;
    bool first_member = false;
    REQUIRE(GroupRegistry::instance().add_member(
        mirror.group, first_socket, peer, 9U, -1,
        group_generation, first_generation, &first_member));
    REQUIRE(first_member);
    REQUIRE_EQ(group_generation, mirror.generation);

    std::uint64_t second_generation = 0;
    first_member = true;
    REQUIRE(GroupRegistry::instance().add_member(
        mirror.group, second_socket, peer, 5U, -1,
        group_generation, second_generation, &first_member));
    REQUIRE(!first_member);
    REQUIRE(second_generation != first_generation);
    GroupRegistry::instance().mark_opened(
        mirror.group, mirror.generation);

    std::array<SRT_SOCKGROUPDATA, 2> data{};
    std::size_t size = data.size();
    REQUIRE_EQ(srt_group_data(mirror.group, data.data(), &size), 2);
    REQUIRE_EQ(size, 2U);
    REQUIRE_EQ(data[0].id, first_socket);
    REQUIRE_EQ(data[1].id, second_socket);
    REQUIRE_EQ(data[0].token, -1);
    REQUIRE_EQ(data[1].token, -1);

    GroupRegistry::instance().remove_member(
        mirror.group, mirror.generation,
        first_socket, first_generation);
    GroupRegistry::instance().remove_member(
        mirror.group, mirror.generation,
        second_socket, second_generation);
    const SRTSOCKET replacement_socket = srt_create_socket();
    REQUIRE(replacement_socket != SRT_INVALID_SOCK);
    std::uint64_t replacement_generation = 0;
    first_member = true;
    REQUIRE(GroupRegistry::instance().add_member(
        mirror.group, replacement_socket, peer, 3U, -1,
        group_generation, replacement_generation, &first_member));
    REQUIRE(!first_member);

    REQUIRE_EQ(srt_close(mirror.group), 0);
    REQUIRE_EQ(srt_getsockstate(replacement_socket), SRTS_CLOSED);
    REQUIRE_EQ(srt_close(first_socket), 0);
    REQUIRE_EQ(srt_close(second_socket), 0);
    REQUIRE_EQ(srt_close(listener), 0);
}

TEST(compat_group_connect_rejects_invalid_endpoint_metadata_before_spawning)
{
    const SRTSOCKET group = srt_create_group(SRT_GTYPE_BROADCAST);
    REQUIRE(group != SRT_INVALID_SOCK);
    const auto destination = ipv4_address(9'012);
    auto endpoint = srt_prepare_endpoint(nullptr,
        reinterpret_cast<const sockaddr*>(&destination),
        static_cast<int>(sizeof(destination)));
    endpoint.weight = 32'768U;
    REQUIRE_EQ(srt_connect_group(group, &endpoint, 1), SRT_ERROR);
    REQUIRE_EQ(endpoint.errorcode, SRT_EINVPARAM);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);

    endpoint = srt_prepare_endpoint(nullptr,
        reinterpret_cast<const sockaddr*>(&destination),
        static_cast<int>(sizeof(destination)));
    endpoint.srcaddr.ss_family = AF_INET6;
    REQUIRE_EQ(srt_connect_group(group, &endpoint, 1), SRT_ERROR);
    REQUIRE_EQ(endpoint.errorcode, SRT_EINVPARAM);
    REQUIRE_EQ(srt_close(group), 0);
}

TEST(compat_group_handshakes_keep_one_origin_without_reusing_timeout_budget)
{
    struct Cleanup {
        SRTSOCKET listener = SRT_INVALID_SOCK;
        SRTSOCKET group = SRT_INVALID_SOCK;
        SRTSOCKET mirror = SRT_INVALID_SOCK;
        ~Cleanup()
        {
            if (group != SRT_INVALID_SOCK)
                (void)srt_close(group);
            if (mirror != SRT_INVALID_SOCK)
                (void)srt_close(mirror);
            if (listener != SRT_INVALID_SOCK)
                (void)srt_close(listener);
        }
    } cleanup;
    const auto listener = srt_create_socket();
    const auto group = srt_create_group(SRT_GTYPE_BROADCAST);
    cleanup.listener = listener;
    cleanup.group = group;
    REQUIRE(listener != SRT_INVALID_SOCK);
    REQUIRE(group != SRT_INVALID_SOCK);
    const int enabled = 1;
    REQUIRE_EQ(
        srt_setsockflag(listener, SRTO_GROUPCONNECT, &enabled, sizeof(enabled)),
        0);
    auto address = ipv4_address(0);
    REQUIRE_EQ(srt_bind(listener, reinterpret_cast<const sockaddr*>(&address),
                   sizeof(address)),
        0);
    REQUIRE_EQ(srt_listen(listener, 4), 0);
    int address_size = sizeof(address);
    REQUIRE_EQ(srt_getsockname(listener, reinterpret_cast<sockaddr*>(&address),
                   &address_size),
        0);
    const auto record = GroupRegistry::instance().find(group);
    REQUIRE(record != nullptr);
    const auto origin =
        ConnectionRuntime::Clock::now() - std::chrono::seconds {10};
    {
        std::lock_guard lock(record->mutex);
        record->timestamp_origin = origin;
    }
    const auto expected_origin =
        std::chrono::duration_cast<std::chrono::microseconds>(
            origin.time_since_epoch())
            .count();
    const auto wait_runtime = [](SRTSOCKET socket) {
        const auto deadline =
            ConnectionRuntime::Clock::now() + std::chrono::seconds {2};
        std::shared_ptr<ConnectionRuntime> runtime;
        do {
            const auto member = SocketRegistry::instance().find(socket);
            if (!member)
                break;
            {
                std::lock_guard lock(member->mutex);
                runtime = member->runtime;
            }
            if (runtime)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds {1});
        } while (ConnectionRuntime::Clock::now() < deadline);
        return runtime;
    };
    for (int index = 0; index < 2; ++index) {
        auto endpoint = srt_prepare_endpoint(nullptr,
            reinterpret_cast<const sockaddr*>(&address), sizeof(address));
        REQUIRE(srt_connect_group(group, &endpoint, 1) != SRT_ERROR);
        const auto runtime = wait_runtime(endpoint.id);
        REQUIRE(runtime != nullptr);
        REQUIRE_EQ(runtime->timestamp_origin_microseconds(), expected_origin);
    }
    const auto mirror = srt_accept(listener, nullptr, nullptr);
    cleanup.mirror = mirror;
    REQUIRE(mirror != SRT_INVALID_SOCK);
    const auto mirror_record = GroupRegistry::instance().find(mirror);
    REQUIRE(mirror_record != nullptr);
    std::vector<robotweax::srt::compat::GroupMemberSnapshot> members;
    {
        std::lock_guard lock(mirror_record->mutex);
        members = mirror_record->members;
    }
    REQUIRE_EQ(members.size(), 2U);
    std::int64_t receiver_origin = 0;
    for (const auto& entry : members) {
        const auto member =
            SocketRegistry::instance().find(entry.public_data.id);
        REQUIRE(member != nullptr);
        std::lock_guard lock(member->mutex);
        REQUIRE(member->runtime != nullptr);
        if (receiver_origin == 0) {
            receiver_origin = member->runtime->timestamp_origin_microseconds();
        }
        REQUIRE_EQ(
            member->runtime->timestamp_origin_microseconds(), receiver_origin);
    }
    constexpr char payload[] = "shared group origin";
    REQUIRE_EQ(
        srt_sendmsg(group, payload, sizeof(payload), -1, 1), sizeof(payload));
    std::array<char, 64> received {};
    REQUIRE_EQ(
        srt_recvmsg(mirror, received.data(), received.size()), sizeof(payload));
    REQUIRE_EQ(std::memcmp(received.data(), payload, sizeof(payload)), 0);
    // Group handles report the message once, not once per member (2 here).
    SRT_TRACEBSTATS sender_statistics {};
    REQUIRE_EQ(srt_bstats(group, &sender_statistics, 0), 0);
    REQUIRE_EQ(sender_statistics.pktSentUniqueTotal, 1);
    REQUIRE_EQ(sender_statistics.byteSentUniqueTotal, sizeof(payload) + 44U);
    REQUIRE_EQ(sender_statistics.pktRecvUniqueTotal, 0);
    SRT_TRACEBSTATS receiver_statistics {};
    REQUIRE_EQ(srt_bstats(mirror, &receiver_statistics, 0), 0);
    REQUIRE_EQ(receiver_statistics.pktRecvUniqueTotal, 1);
    REQUIRE_EQ(receiver_statistics.pktSentUniqueTotal, 0);
    REQUIRE_EQ(srt_close(group), 0);
    cleanup.group = SRT_INVALID_SOCK;
    REQUIRE_EQ(srt_close(mirror), 0);
    cleanup.mirror = SRT_INVALID_SOCK;
    REQUIRE_EQ(srt_close(listener), 0);
    cleanup.listener = SRT_INVALID_SOCK;
}

TEST(compat_group_connect_joins_with_unread_messages_across_rollover)
{
    for (const auto type : {SRT_GTYPE_BACKUP, SRT_GTYPE_BROADCAST}) {
        struct Cleanup {
            SRTSOCKET listener = SRT_INVALID_SOCK;
            SRTSOCKET sender = SRT_INVALID_SOCK;
            SRTSOCKET mirror = SRT_INVALID_SOCK;
            ~Cleanup()
            {
                if (sender != SRT_INVALID_SOCK)
                    (void)srt_close(sender);
                if (mirror != SRT_INVALID_SOCK)
                    (void)srt_close(mirror);
                if (listener != SRT_INVALID_SOCK)
                    (void)srt_close(listener);
            }
        } cleanup;
        struct HandshakeGate {
            std::atomic<int> calls {0};
            std::atomic<bool> delayed {false};
            std::promise<void> entered;
            std::promise<void> release;
            std::shared_future<void> proceed = release.get_future().share();
        } gate;
        cleanup.listener = srt_create_socket();
        cleanup.sender = srt_create_group(type);
        REQUIRE(cleanup.listener != SRT_INVALID_SOCK);
        REQUIRE(cleanup.sender != SRT_INVALID_SOCK);
        const auto sender_record =
            GroupRegistry::instance().find(cleanup.sender);
        REQUIRE(sender_record != nullptr);
        const SequenceNumber initial {SequenceNumber::mask - 1U};
        {
            std::lock_guard lock(sender_record->mutex);
            sender_record->initial_sequence = initial.value();
            sender_record->next_send_sequence = initial.value();
            sender_record->replay_acknowledged_sequence = initial.value();
            sender_record->next_receive_sequence = initial.value();
        }
        const int enabled = 1;
        REQUIRE_EQ(srt_setsockflag(cleanup.listener, SRTO_GROUPCONNECT,
                       &enabled, sizeof(enabled)),
            0);
        REQUIRE_EQ(
            srt_listen_callback(
                cleanup.listener,
                [](void* opaque, SRTSOCKET, int, const sockaddr*, const char*) {
                    auto& gate = *static_cast<HandshakeGate*>(opaque);
                    if (++gate.calls >= 3 && !gate.delayed.exchange(true)) {
                        gate.entered.set_value();
                        return gate.proceed.wait_for(std::chrono::seconds {5})
                                == std::future_status::ready
                            ? 0
                            : -1;
                    }
                    return 0;
                },
                &gate),
            0);
        const std::int32_t receive_timeout = 2'000;
        REQUIRE_EQ(srt_setsockflag(cleanup.listener, SRTO_RCVTIMEO,
                       &receive_timeout, sizeof(receive_timeout)),
            0);
        REQUIRE_EQ(srt_setsockflag(cleanup.sender, SRTO_RCVTIMEO,
                       &receive_timeout, sizeof(receive_timeout)),
            0);
        auto address = ipv4_address(0);
        REQUIRE_EQ(
            srt_bind(cleanup.listener,
                reinterpret_cast<const sockaddr*>(&address), sizeof(address)),
            0);
        REQUIRE_EQ(srt_listen(cleanup.listener, 4), 0);
        int address_size = sizeof(address);
        REQUIRE_EQ(srt_getsockname(cleanup.listener,
                       reinterpret_cast<sockaddr*>(&address), &address_size),
            0);
        auto first_endpoint = srt_prepare_endpoint(nullptr,
            reinterpret_cast<const sockaddr*>(&address), sizeof(address));
        REQUIRE(
            srt_connect_group(cleanup.sender, &first_endpoint, 1) != SRT_ERROR);
        cleanup.mirror = srt_accept(cleanup.listener, nullptr, nullptr);
        REQUIRE(cleanup.mirror != SRT_INVALID_SOCK);
        const auto mirror_record =
            GroupRegistry::instance().find(cleanup.mirror);
        REQUIRE(mirror_record != nullptr);
        // The socket and group can publish CONNECTED before the transport
        // runtime is attached; group sends require all three to be ready.
        const auto wait_for_connected_member = [&](SRTSOCKET member) {
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds {3};
            for (;;) {
                const auto state = srt_getsockstate(member);
                bool published = false;
                std::uint64_t group_generation = 0;
                std::uint64_t member_generation = 0;
                {
                    std::lock_guard lock(sender_record->mutex);
                    group_generation = sender_record->generation;
                    const auto entry =
                        std::find_if(sender_record->members.begin(),
                            sender_record->members.end(),
                            [member](const auto& entry) {
                                return entry.public_data.id == member
                                    && entry.public_data.sockstate
                                    == SRTS_CONNECTED;
                            });
                    published = entry != sender_record->members.end();
                    if (published)
                        member_generation = entry->generation;
                }
                bool transport_ready = false;
                if (published) {
                    const auto socket = SocketRegistry::instance().find(member);
                    if (socket != nullptr) {
                        std::lock_guard lock(socket->mutex);
                        transport_ready = socket->state == SRTS_CONNECTED
                            && socket->runtime != nullptr
                            && socket->group_id == cleanup.sender
                            && socket->group_generation == group_generation
                            && socket->member_generation == member_generation;
                    }
                }
                if (state == SRTS_CONNECTED && transport_ready)
                    return true;
                if (std::chrono::steady_clock::now() >= deadline
                    || (state != SRTS_CONNECTING && state != SRTS_CONNECTED)) {
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds {1});
            }
        };
        REQUIRE(wait_for_connected_member(first_endpoint.id));

        const std::array<std::array<char, 2>, 2> before_join {
            {{'a', '0'}, {'b', '1'}}};
        for (const auto& payload : before_join) {
            REQUIRE_EQ(srt_sendmsg(cleanup.sender, payload.data(),
                           payload.size(), -1, 1),
                static_cast<int>(payload.size()));
        }
        {
            std::lock_guard sender_lock(sender_record->mutex);
            std::lock_guard mirror_lock(mirror_record->mutex);
            REQUIRE_EQ(sender_record->next_send_sequence, 0U);
            REQUIRE_EQ(mirror_record->next_receive_sequence, initial.value());
        }

        auto second_endpoint = srt_prepare_endpoint(nullptr,
            reinterpret_cast<const sockaddr*>(&address), sizeof(address));
        REQUIRE(srt_connect_group(cleanup.sender, &second_endpoint, 1)
            != SRT_ERROR);
        REQUIRE(wait_for_connected_member(second_endpoint.id));
        std::vector<robotweax::srt::compat::GroupMemberSnapshot> members;
        {
            std::lock_guard lock(mirror_record->mutex);
            members = mirror_record->members;
        }
        REQUIRE_EQ(members.size(), 2U);
        std::int64_t receiver_origin = 0;
        for (const auto& entry : members) {
            const auto socket =
                SocketRegistry::instance().find(entry.public_data.id);
            REQUIRE(socket != nullptr);
            std::lock_guard lock(socket->mutex);
            REQUIRE(socket->runtime != nullptr);
            const auto origin =
                socket->runtime->timestamp_origin_microseconds();
            if (receiver_origin == 0) {
                receiver_origin = origin;
            }
            REQUIRE_EQ(origin, receiver_origin);
        }

        std::array<char, 8> received {};
        SRT_MSGCTRL control = srt_msgctrl_default;
        for (std::size_t index = 0; index < before_join.size(); ++index) {
            REQUIRE_EQ(srt_recvmsg2(cleanup.mirror, received.data(),
                           received.size(), &control),
                2);
            REQUIRE_EQ(control.pktseq,
                static_cast<std::int32_t>(
                    initial.advanced(static_cast<std::uint32_t>(index))
                        .value()));
            REQUIRE(std::equal(before_join[index].begin(),
                before_join[index].end(), received.begin()));
        }
        const std::array<char, 2> after_join {'c', '2'};
        REQUIRE_EQ(srt_sendmsg(cleanup.sender, after_join.data(),
                       after_join.size(), -1, 1),
            2);
        REQUIRE_EQ(srt_recvmsg2(cleanup.mirror, received.data(),
                       received.size(), &control),
            2);
        REQUIRE_EQ(control.pktseq, 0);
        REQUIRE(
            std::equal(after_join.begin(), after_join.end(), received.begin()));

        // The reverse send cursor initially precedes the later member's
        // wire ISN. Sending must preserve both paths, then advance the reverse
        // direction beyond the forward cursor before the next admission.
        const auto reverse_send = [&](char suffix) {
            const std::array<char, 2> payload {'r', suffix};
            REQUIRE_EQ(srt_sendmsg(cleanup.mirror, payload.data(),
                           payload.size(), -1, 1),
                2);
            REQUIRE_EQ(srt_recvmsg2(cleanup.sender, received.data(),
                           received.size(), &control),
                2);
            REQUIRE(
                std::equal(payload.begin(), payload.end(), received.begin()));
            std::vector<robotweax::srt::compat::GroupMemberSnapshot>
                reverse_members;
            {
                std::lock_guard lock(mirror_record->mutex);
                reverse_members = mirror_record->members;
            }
            for (const auto& member : reverse_members) {
                REQUIRE_EQ(
                    srt_getsockstate(member.public_data.id), SRTS_CONNECTED);
            }
        };
        for (char suffix = '0'; suffix != '6'; ++suffix) {
            reverse_send(suffix);
        }

        // Hold a third handshake after it captures the sender's sequence.
        // While it waits, the earlier paths advance the receive cursor.
        auto entered = gate.entered.get_future();
        auto delayed_join =
            std::async(std::launch::async, [sender = cleanup.sender, address] {
                auto endpoint = srt_prepare_endpoint(nullptr,
                    reinterpret_cast<const sockaddr*>(&address),
                    sizeof(address));
                const int result = srt_connect_group(sender, &endpoint, 1);
                return std::pair {result, endpoint.id};
            });
        const bool reached_callback = entered.wait_for(std::chrono::seconds {3})
            == std::future_status::ready;
        const std::array<char, 2> during_handshake {'d', '3'};
        int send_result = SRT_ERROR;
        int receive_result = SRT_ERROR;
        int received_sequence = SRT_SEQNO_NONE;
        if (reached_callback) {
            send_result = srt_sendmsg(cleanup.sender, during_handshake.data(),
                during_handshake.size(), -1, 1);
            if (send_result == 2) {
                receive_result = srt_recvmsg2(
                    cleanup.mirror, received.data(), received.size(), &control);
                received_sequence = control.pktseq;
            }
        }
        gate.release.set_value();
        const auto [join_result, third_socket] = delayed_join.get();
        REQUIRE(reached_callback);
        REQUIRE_EQ(send_result, 2);
        REQUIRE_EQ(receive_result, 2);
        REQUIRE_EQ(received_sequence, 1);
        REQUIRE(std::equal(during_handshake.begin(), during_handshake.end(),
            received.begin()));
        REQUIRE(join_result != SRT_ERROR);
        REQUIRE(wait_for_connected_member(third_socket));
        const auto third_record = SocketRegistry::instance().find(third_socket);
        REQUIRE(third_record != nullptr);
        {
            std::lock_guard lock(third_record->mutex);
            REQUIRE_EQ(third_record->connection_initial_sequence, 1U);
        }
        {
            std::lock_guard lock(mirror_record->mutex);
            members = mirror_record->members;
            REQUIRE_EQ(mirror_record->next_receive_sequence, 2U);
        }
        REQUIRE_EQ(members.size(), 3U);
        const auto joined_socket =
            SocketRegistry::instance().find(members.back().public_data.id);
        REQUIRE(joined_socket != nullptr);
        {
            std::lock_guard lock(joined_socket->mutex);
            REQUIRE(joined_socket->runtime != nullptr);
            REQUIRE_EQ(joined_socket->runtime->timestamp_origin_microseconds(),
                receiver_origin);
        }
        reverse_send('6');
        const std::array<char, 2> after_delayed_join {'e', '4'};
        REQUIRE_EQ(srt_sendmsg(cleanup.sender, after_delayed_join.data(),
                       after_delayed_join.size(), -1, 1),
            2);
        REQUIRE_EQ(srt_recvmsg2(cleanup.mirror, received.data(),
                       received.size(), &control),
            2);
        REQUIRE_EQ(control.pktseq, 2);
        REQUIRE(std::equal(after_delayed_join.begin(), after_delayed_join.end(),
            received.begin()));
    }
}

TEST(compat_group_registry_serializes_parallel_handle_allocation)
{
    constexpr std::size_t thread_count = 8;
    constexpr std::size_t groups_per_thread = 32;
    std::array<std::thread, thread_count> threads;
    std::mutex results_mutex;
    std::vector<SRTSOCKET> results;
    results.reserve(thread_count * groups_per_thread);

    for (auto& thread : threads) {
        thread = std::thread([&] {
            std::array<SRTSOCKET, groups_per_thread> local{};
            for (auto& group : local) {
                group = srt_create_group(SRT_GTYPE_BACKUP);
            }
            std::lock_guard lock(results_mutex);
            results.insert(results.end(), local.begin(), local.end());
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    REQUIRE_EQ(results.size(), thread_count * groups_per_thread);
    REQUIRE(std::all_of(results.begin(), results.end(), [](SRTSOCKET group) {
        return group != SRT_INVALID_SOCK && (group & SRTGROUP_MASK) != 0;
    }));
    std::sort(results.begin(), results.end());
    REQUIRE(std::adjacent_find(results.begin(), results.end()) == results.end());
    for (const SRTSOCKET group : results) {
        REQUIRE_EQ(srt_close(group), 0);
    }
}

TEST(compat_broadcast_group_fans_out_one_logical_message_and_reports_members)
{
    const SRTSOCKET group = srt_create_group(SRT_GTYPE_BROADCAST);
    const SRTSOCKET first = srt_create_socket();
    const SRTSOCKET second = srt_create_socket();
    REQUIRE(group != SRT_INVALID_SOCK);
    REQUIRE(first != SRT_INVALID_SOCK);
    REQUIRE(second != SRT_INVALID_SOCK);
    const auto group_record = GroupRegistry::instance().find(group);
    REQUIRE(group_record != nullptr);
    std::uint32_t initial_sequence = 0;
    {
        std::lock_guard lock(group_record->mutex);
        initial_sequence = group_record->initial_sequence;
    }
    const auto first_runtime = attach_group_runtime(
        group, first, initial_sequence, 3);
    const auto second_runtime = attach_group_runtime(
        group, second, initial_sequence, 7);

    constexpr char payload[] = "broadcast";
    std::array<SRT_SOCKGROUPDATA, 2> data{};
    SRT_MSGCTRL control = srt_msgctrl_default;
    control.grpdata = data.data();
    control.grpdata_size = data.size();
    REQUIRE_EQ(srt_sendmsg2(group, payload,
                   static_cast<int>(sizeof(payload)), &control),
        static_cast<int>(sizeof(payload)));
    REQUIRE_EQ(control.pktseq,
        static_cast<std::int32_t>(initial_sequence));
    REQUIRE_EQ(control.msgno, 1);
    REQUIRE(control.srctime > 0);
    REQUIRE(control.grpdata == data.data());
    REQUIRE_EQ(control.grpdata_size, data.size());
    REQUIRE(std::all_of(data.begin(), data.end(), [](const auto& member) {
        return member.memberstate == SRT_GST_RUNNING
            && member.result == static_cast<int>(sizeof(payload));
    }));

    control = srt_msgctrl_default;
    REQUIRE_EQ(srt_sendmsg2(group, payload,
                   static_cast<int>(sizeof(payload)), &control),
        static_cast<int>(sizeof(payload)));
    REQUIRE(control.grpdata == nullptr);
    REQUIRE_EQ(control.grpdata_size, data.size());

    std::array<SRT_SOCKGROUPDATA, 1> insufficient{};
    control = srt_msgctrl_default;
    control.grpdata = insufficient.data();
    control.grpdata_size = insufficient.size();
    REQUIRE_EQ(srt_sendmsg2(group, payload,
                   static_cast<int>(sizeof(payload)), &control),
        static_cast<int>(sizeof(payload)));
    REQUIRE(control.grpdata == nullptr);
    REQUIRE_EQ(control.grpdata_size, data.size());

    const auto duplicate_identity = first_runtime->queue_group_message(
        std::as_bytes(std::span {payload, sizeof(payload)}),
        SequenceNumber {initial_sequence}, 1, 0, true);
    REQUIRE_EQ(duplicate_identity.status,
        robotweax::srt::compat::MessageIoStatus::invalid_state);
    (void)second_runtime;
    REQUIRE_EQ(srt_close(group), 0);
}

TEST(compat_broadcast_group_receives_once_and_discards_path_duplicates)
{
    const SRTSOCKET group = srt_create_group(SRT_GTYPE_BROADCAST);
    const SRTSOCKET first = srt_create_socket();
    const SRTSOCKET second = srt_create_socket();
    const auto group_record = GroupRegistry::instance().find(group);
    REQUIRE(group_record != nullptr);
    std::uint32_t initial_sequence = 0;
    {
        std::lock_guard lock(group_record->mutex);
        initial_sequence = group_record->initial_sequence;
    }
    const auto first_runtime = attach_group_runtime(
        group, first, initial_sequence);
    const auto second_runtime = attach_group_runtime(
        group, second, initial_sequence);

    constexpr std::array<std::byte, 4> payload{
        std::byte{'d'}, std::byte{'a'}, std::byte{'t'}, std::byte{'a'}};
    robotweax::srt::PacketView packet;
    packet.kind = robotweax::srt::PacketKind::data;
    packet.data.sequence = SequenceNumber{initial_sequence};
    packet.data.message_number = 1;
    packet.data.boundary = robotweax::srt::MessageBoundary::solo;
    packet.data.in_order = true;
    packet.payload = payload;
    const IpEndpoint peer = IpEndpoint::loopback(9'000);
    first_runtime->process_packet(packet, peer);
    second_runtime->process_packet(packet, peer);

    std::array<char, 32> received{};
    SRT_MSGCTRL control = srt_msgctrl_default;
    REQUIRE_EQ(srt_recvmsg2(group, received.data(),
                   static_cast<int>(received.size()), &control),
        static_cast<int>(payload.size()));
    REQUIRE(std::equal(payload.begin(), payload.end(),
        reinterpret_cast<const std::byte*>(received.data())));
    REQUIRE_EQ(control.pktseq,
        static_cast<std::int32_t>(initial_sequence));
    REQUIRE_EQ(control.msgno, 1);

    const bool asynchronous = false;
    REQUIRE_EQ(srt_setsockflag(group, SRTO_RCVSYN,
                   &asynchronous,
                   static_cast<int>(sizeof(asynchronous))),
        0);
    REQUIRE_EQ(srt_recv(group, received.data(),
                   static_cast<int>(received.size())),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EASYNCRCV);
    REQUIRE_EQ(srt_close(group), 0);
}

TEST(compat_group_receive_withholds_a_message_beyond_the_logical_prefix)
{
    constexpr std::array<SRT_GROUP_TYPE, 2> group_types {
        SRT_GTYPE_BROADCAST, SRT_GTYPE_BACKUP};
    for (const SRT_GROUP_TYPE group_type : group_types) {
        const SRTSOCKET group = srt_create_group(group_type);
        const SRTSOCKET member = srt_create_socket();
        REQUIRE(group != SRT_INVALID_SOCK);
        REQUIRE(member != SRT_INVALID_SOCK);
        const auto group_record = GroupRegistry::instance().find(group);
        REQUIRE(group_record != nullptr);
        std::uint32_t initial_sequence = 0;
        {
            std::lock_guard lock(group_record->mutex);
            initial_sequence = group_record->initial_sequence;
        }
        TestClock clock {
            .channel =
                std::make_shared<robotweax::srt::compat::DatagramChannel>(),
        };
        clock.channel->set_send_hook_for_testing(accept_test_datagram, nullptr);
        const auto runtime =
            attach_group_runtime(group, member, initial_sequence, 1, &clock);
        const IpEndpoint peer = IpEndpoint::loopback(9'000);
        const auto inject = [&](SequenceNumber sequence,
                                std::uint32_t message_number,
                                std::span<const std::byte> payload) {
            robotweax::srt::PacketView packet;
            packet.kind = robotweax::srt::PacketKind::data;
            packet.data.sequence = sequence;
            packet.data.message_number = message_number;
            packet.data.boundary = robotweax::srt::MessageBoundary::solo;
            packet.data.in_order = true;
            packet.payload = payload;
            runtime->process_packet(packet, peer);
        };

        constexpr std::array<std::byte, 5> expected_payload {std::byte {'f'},
            std::byte {'i'}, std::byte {'r'}, std::byte {'s'}, std::byte {'t'}};
        constexpr std::array<std::byte, 6> future_payload {std::byte {'s'},
            std::byte {'e'}, std::byte {'c'}, std::byte {'o'}, std::byte {'n'},
            std::byte {'d'}};
        const SequenceNumber expected {initial_sequence};
        const SequenceNumber future = expected.next();
        inject(future, 2, future_payload);

        const bool asynchronous = false;
        REQUIRE_EQ(srt_setsockflag(group, SRTO_RCVSYN, &asynchronous,
                       static_cast<int>(sizeof(asynchronous))),
            0);
        std::array<char, 32> received {};
        SRT_MSGCTRL control = srt_msgctrl_default;
        REQUIRE_EQ(srt_recvmsg2(group, received.data(),
                       static_cast<int>(received.size()), &control),
            SRT_ERROR);
        REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EASYNCRCV);

        inject(expected, 1, expected_payload);
        REQUIRE_EQ(srt_recvmsg2(group, received.data(),
                       static_cast<int>(received.size()), &control),
            static_cast<int>(expected_payload.size()));
        REQUIRE_EQ(control.pktseq, static_cast<std::int32_t>(expected.value()));
        REQUIRE(std::equal(expected_payload.begin(), expected_payload.end(),
            reinterpret_cast<const std::byte*>(received.data())));
        REQUIRE_EQ(srt_recvmsg2(group, received.data(),
                       static_cast<int>(received.size()), &control),
            static_cast<int>(future_payload.size()));
        REQUIRE_EQ(control.pktseq, static_cast<std::int32_t>(future.value()));
        REQUIRE(std::equal(future_payload.begin(), future_payload.end(),
            reinterpret_cast<const std::byte*>(received.data())));
        REQUIRE_EQ(srt_close(group), 0);
    }
}

TEST(compat_group_epoll_wakes_on_member_data_without_process_wide_rescans)
{
    const auto group = srt_create_group(SRT_GTYPE_BROADCAST);
    const auto member = srt_create_socket();
    REQUIRE(group != SRT_INVALID_SOCK);
    REQUIRE(member != SRT_INVALID_SOCK);
    const auto record = GroupRegistry::instance().find(group);
    REQUIRE(record != nullptr);
    const auto sequence = record->initial_sequence;
    TestClock clock {.now_microseconds = 0,
        .channel = std::make_shared<robotweax::srt::compat::DatagramChannel>()};
    clock.channel->set_send_hook_for_testing(accept_test_datagram, nullptr);
    const auto runtime = attach_group_runtime(group, member, sequence, 1,
        &clock, 0, true, 20, ConnectionRuntime::Clock::now(), record);

    // One poller watches the group, another watches unrelated sockets only.
    const int poll = srt_epoll_create();
    const int other = srt_epoll_create();
    REQUIRE(poll >= 0);
    REQUIRE(other >= 0);
    const int input = SRT_EPOLL_IN;
    REQUIRE_EQ(srt_epoll_add_usock(poll, group, &input), 0);
    std::vector<SRTSOCKET> unrelated;
    for (unsigned index = 0; index < 16; ++index) {
        const auto socket = srt_create_socket();
        REQUIRE(socket != SRT_INVALID_SOCK);
        unrelated.push_back(socket);
        REQUIRE_EQ(srt_epoll_add_usock(other, socket, &input), 0);
    }
    SRT_EPOLL_EVENT event {};
    REQUIRE_EQ(srt_epoll_uwait(poll, &event, 1, 0), 0);
    REQUIRE_EQ(srt_epoll_uwait(other, &event, 1, 0), 0);
    const auto other_before =
        robotweax::srt::compat::epoll_readiness_queries_for_testing(other);

    const std::array<std::byte, 1> payload {std::byte {'g'}};
    robotweax::srt::PacketView packet;
    packet.kind = robotweax::srt::PacketKind::data;
    packet.data.sequence = SequenceNumber {sequence};
    packet.data.message_number = 1;
    packet.data.boundary = robotweax::srt::MessageBoundary::solo;
    packet.data.in_order = true;
    packet.data.timestamp = robotweax::srt::PacketTimestamp {0};
    packet.payload = payload;
    clock.now_microseconds = 10'000;
    runtime->process_packet(packet, IpEndpoint::loopback(9'000));
    clock.now_microseconds = 40'000;
    (void)runtime->poll();

    // The group poller sees the member's data through the group source ...
    REQUIRE_EQ(srt_epoll_uwait(poll, &event, 1, 0), 1);
    REQUIRE_EQ(event.fd, group);
    REQUIRE((event.events & SRT_EPOLL_IN) != 0);
    // ... while the unrelated poller was not asked to re-query anything.
    REQUIRE_EQ(srt_epoll_uwait(other, &event, 1, 0), 0);
    REQUIRE_EQ(
        robotweax::srt::compat::epoll_readiness_queries_for_testing(other),
        other_before);

    for (const auto socket : unrelated) {
        REQUIRE_EQ(srt_close(socket), 0);
    }
    REQUIRE_EQ(srt_epoll_release(poll), 0);
    REQUIRE_EQ(srt_epoll_release(other), 0);
    REQUIRE_EQ(srt_close(group), 0);
}

TEST(compat_group_receive_skips_a_gap_every_member_has_dropped)
{
    // Receiver TLPKTDROP on the only carrying member moves its receive
    // floor past the expected group sequence. The group must skip the gap
    // instead of waiting for a message that can no longer arrive.
    for (const auto type : {SRT_GTYPE_BACKUP, SRT_GTYPE_BROADCAST}) {
        const auto group = srt_create_group(type);
        const auto member = srt_create_socket();
        REQUIRE(group != SRT_INVALID_SOCK);
        REQUIRE(member != SRT_INVALID_SOCK);
        const auto record = GroupRegistry::instance().find(group);
        REQUIRE(record != nullptr);
        const auto sequence = record->initial_sequence;
        const auto origin = ConnectionRuntime::Clock::now();
        TestClock clock {.now_microseconds = 0,
            .channel =
                std::make_shared<robotweax::srt::compat::DatagramChannel>()};
        clock.channel->set_send_hook_for_testing(accept_test_datagram, nullptr);
        constexpr std::uint16_t latency_milliseconds = 300;
        const auto runtime = attach_group_runtime(group, member, sequence, 1,
            &clock, 0, true, latency_milliseconds, origin, record, {}, nullptr,
            nullptr, true);
        const auto inject = [&](std::uint32_t seq, std::uint32_t timestamp,
                                std::byte value) {
            const std::array<std::byte, 1> payload {value};
            robotweax::srt::PacketView packet;
            packet.kind = robotweax::srt::PacketKind::data;
            packet.data.sequence = SequenceNumber {seq};
            packet.data.message_number = seq - sequence + 1U;
            packet.data.boundary = robotweax::srt::MessageBoundary::solo;
            packet.data.in_order = true;
            packet.data.timestamp = robotweax::srt::PacketTimestamp {timestamp};
            packet.payload = payload;
            runtime->process_packet(packet, IpEndpoint::loopback(9'000));
        };
        const bool synchronous = false;
        REQUIRE_EQ(srt_setsockflag(group, SRTO_RCVSYN, &synchronous,
                       static_cast<int>(sizeof(synchronous))),
            0);

        // The first group message is lost; the two following ones arrive.
        const SequenceNumber second = SequenceNumber {sequence}.next();
        const SequenceNumber third = second.next();
        clock.now_microseconds = 10'000;
        inject(second.value(), 10'000U, std::byte {'b'});
        inject(third.value(), 11'000U, std::byte {'c'});
        const int poll = srt_epoll_create();
        REQUIRE(poll >= 0);
        const int watched = SRT_EPOLL_IN;
        REQUIRE_EQ(srt_epoll_add_usock(poll, group, &watched), 0);
        SRT_EPOLL_EVENT event {};
        REQUIRE_EQ(srt_epoll_uwait(poll, &event, 1, 0), 0);

        std::array<char, 8> buffer {};
        SRT_MSGCTRL control = srt_msgctrl_default;
        // Before the delivery deadline the gap can still be recovered.
        REQUIRE_EQ(srt_recvmsg2(group, buffer.data(), buffer.size(), &control),
            SRT_ERROR);
        REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EASYNCRCV);

        // Past the deadline the member drops the gap. The group must deliver
        // the surviving messages in order rather than block forever.
        clock.now_microseconds = 10'000U
            + static_cast<std::uint64_t>(latency_milliseconds) * 1'000U
            + 50'000U;
        REQUIRE_EQ(srt_epoll_update_usock(poll, group, &watched), 0);
        REQUIRE_EQ(srt_epoll_uwait(poll, &event, 1, 0), 1);
        REQUIRE_EQ(event.events, SRT_EPOLL_IN);
        REQUIRE_EQ(
            srt_recvmsg2(group, buffer.data(), buffer.size(), &control), 1);
        REQUIRE_EQ(buffer[0], 'b');
        REQUIRE_EQ(control.pktseq, static_cast<std::int32_t>(second.value()));
        REQUIRE_EQ(srt_epoll_uwait(poll, &event, 1, 0), 1);
        REQUIRE_EQ(
            srt_recvmsg2(group, buffer.data(), buffer.size(), &control), 1);
        REQUIRE_EQ(buffer[0], 'c');
        REQUIRE_EQ(control.pktseq, static_cast<std::int32_t>(third.value()));
        REQUIRE_EQ(srt_recvmsg2(group, buffer.data(), buffer.size(), &control),
            SRT_ERROR);
        REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EASYNCRCV);
        REQUIRE_EQ(srt_epoll_uwait(poll, &event, 1, 0), 0);

        // The group handle reports its own receive and drop counters.
        SRT_TRACEBSTATS statistics {};
        REQUIRE_EQ(srt_bstats(group, &statistics, 1), 0);
        REQUIRE_EQ(statistics.pktRecvUniqueTotal, 2);
        REQUIRE_EQ(statistics.pktRecvUnique, 2);
        REQUIRE_EQ(statistics.byteRecvUniqueTotal, 2U * (1U + 44U));
        REQUIRE_EQ(statistics.pktRcvDropTotal, 1);
        REQUIRE_EQ(statistics.pktRcvDrop, 1);
        REQUIRE_EQ(statistics.byteRcvDropTotal,
            static_cast<std::uint64_t>(SRT_LIVE_DEF_PLSIZE + 44));
        REQUIRE_EQ(statistics.pktSentUniqueTotal, 0);
        REQUIRE_EQ(statistics.pktSentTotal, 0);
        // The interval counters were cleared by the previous call.
        REQUIRE_EQ(srt_bstats(group, &statistics, 0), 0);
        REQUIRE_EQ(statistics.pktRecvUniqueTotal, 2);
        REQUIRE_EQ(statistics.pktRecvUnique, 0);
        REQUIRE_EQ(statistics.pktRcvDrop, 0);
        REQUIRE_EQ(srt_epoll_release(poll), 0);
        REQUIRE_EQ(srt_close(group), 0);
        REQUIRE_EQ(srt_bstats(group, &statistics, 0), SRT_ERROR);
        REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVSOCK);
    }
}

TEST(compat_group_receive_preserves_terminal_member_pending_message)
{
    // SHUTDOWN does not discard a complete message waiting for TSBPD. A
    // replacement member that starts at the next sequence must not cause the
    // group to skip that still-buffered message.
    for (const auto type : {SRT_GTYPE_BACKUP, SRT_GTYPE_BROADCAST}) {
        const SRTSOCKET group = srt_create_group(type);
        const SRTSOCKET first_socket = srt_create_socket();
        const SRTSOCKET later_socket = srt_create_socket();
        REQUIRE(group != SRT_INVALID_SOCK);
        REQUIRE(first_socket != SRT_INVALID_SOCK);
        REQUIRE(later_socket != SRT_INVALID_SOCK);
        const auto record = GroupRegistry::instance().find(group);
        REQUIRE(record != nullptr);
        const SequenceNumber first_sequence {record->initial_sequence};
        const auto origin = ConnectionRuntime::Clock::now();
        TestClock clock {.now_microseconds = 0,
            .channel =
                std::make_shared<robotweax::srt::compat::DatagramChannel>()};
        clock.channel->set_send_hook_for_testing(accept_test_datagram, nullptr);
        constexpr std::uint16_t latency_milliseconds = 100;
        const auto first =
            attach_group_runtime(group, first_socket, first_sequence.value(), 1,
                &clock, 0, true, latency_milliseconds, origin, record);
        const auto inject =
            [&](const std::shared_ptr<ConnectionRuntime>& runtime,
                SequenceNumber sequence, std::uint32_t message_number,
                std::uint32_t timestamp, std::byte value) {
                const std::array<std::byte, 1> payload {value};
                robotweax::srt::PacketView packet;
                packet.kind = robotweax::srt::PacketKind::data;
                packet.data.sequence = sequence;
                packet.data.message_number = message_number;
                packet.data.boundary = robotweax::srt::MessageBoundary::solo;
                packet.data.in_order = true;
                packet.data.timestamp =
                    robotweax::srt::PacketTimestamp {timestamp};
                packet.payload = payload;
                runtime->process_packet(packet, IpEndpoint::loopback(9'000));
            };
        inject(first, first_sequence, 1, 0, std::byte {'a'});
        robotweax::srt::PacketView shutdown;
        shutdown.kind = robotweax::srt::PacketKind::control;
        shutdown.control.type = robotweax::srt::ControlType::shutdown;
        shutdown.control.destination_socket_id = 77;
        const std::array<std::byte, 4> shutdown_padding {};
        shutdown.payload = shutdown_padding;
        first->process_packet(shutdown, IpEndpoint::loopback(9'000));
        REQUIRE_EQ(srt_getsockstate(first_socket), SRTS_BROKEN);

        const SequenceNumber second_sequence = first_sequence.next();
        const auto later =
            attach_group_runtime(group, later_socket, second_sequence.value(),
                1, &clock, 0, true, latency_milliseconds, origin, record);
        inject(later, second_sequence, 2, 1'000, std::byte {'b'});
        const bool asynchronous = false;
        REQUIRE_EQ(srt_setsockflag(group, SRTO_RCVSYN, &asynchronous,
                       static_cast<int>(sizeof(asynchronous))),
            0);
        std::array<char, 8> buffer {};
        SRT_MSGCTRL control = srt_msgctrl_default;
        REQUIRE_EQ(srt_recvmsg2(group, buffer.data(), buffer.size(), &control),
            SRT_ERROR);
        REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EASYNCRCV);

        clock.now_microseconds = 100'000;
        REQUIRE_EQ(
            srt_recvmsg2(group, buffer.data(), buffer.size(), &control), 1);
        REQUIRE_EQ(buffer[0], 'a');
        REQUIRE_EQ(
            control.pktseq, static_cast<std::int32_t>(first_sequence.value()));
        clock.now_microseconds = 101'000;
        REQUIRE_EQ(
            srt_recvmsg2(group, buffer.data(), buffer.size(), &control), 1);
        REQUIRE_EQ(buffer[0], 'b');
        REQUIRE_EQ(
            control.pktseq, static_cast<std::int32_t>(second_sequence.value()));
        REQUIRE_EQ(srt_close(group), 0);
    }
}

TEST(compat_group_shared_tsbpd_clock_gates_member_switch_and_late_replay)
{
    for (const auto type : {SRT_GTYPE_BACKUP, SRT_GTYPE_BROADCAST}) {
        for (const bool rollover : {false, true}) {
            const auto group = srt_create_group(type);
            const auto primary = srt_create_socket();
            const auto backup = srt_create_socket();
            const auto record = GroupRegistry::instance().find(group);
            REQUIRE(record != nullptr);
            const auto sequence = record->initial_sequence;
            const auto origin = ConnectionRuntime::Clock::now();
            TestClock primary_clock {.now_microseconds = rollover ? 0U : 1'797U,
                .channel = std::make_shared<
                    robotweax::srt::compat::DatagramChannel>()};
            TestClock backup_clock {.now_microseconds = 0,
                .channel = std::make_shared<
                    robotweax::srt::compat::DatagramChannel>()};
            primary_clock.channel->set_send_hook_for_testing(
                accept_test_datagram, nullptr);
            backup_clock.channel->set_send_hook_for_testing(
                accept_test_datagram, nullptr);
            auto primary_runtime = attach_group_runtime(group, primary,
                sequence, 1, &primary_clock, 0, true, 300, origin, record,
                robotweax::srt::PacketTimestamp {rollover ? 0xffff'0000U : 0U});
            const auto inject = [](const auto& runtime, std::uint32_t seq,
                                    std::uint32_t timestamp) {
                constexpr std::array<std::byte, 1> payload {std::byte {'x'}};
                robotweax::srt::PacketView packet;
                packet.kind = robotweax::srt::PacketKind::data;
                packet.data.sequence = SequenceNumber {seq};
                packet.data.message_number = 1;
                packet.data.boundary = robotweax::srt::MessageBoundary::solo;
                packet.data.in_order = true;
                packet.data.timestamp =
                    robotweax::srt::PacketTimestamp {timestamp};
                packet.payload = payload;
                runtime->process_packet(packet, IpEndpoint::loopback(9'000));
            };
            const bool synchronous = false;
            REQUIRE_EQ(srt_setsockflag(group, SRTO_RCVSYN, &synchronous,
                           static_cast<int>(sizeof(synchronous))),
                0);
            const std::uint64_t first_deadline = rollover ? 365'520U : 311'797U;
            primary_clock.now_microseconds = first_deadline;
            inject(
                primary_runtime, sequence, rollover ? 0xffff'fff0U : 10'000U);
            std::array<char, 8> buffer {};
            SRT_MSGCTRL first = srt_msgctrl_default;
            REQUIRE_EQ(
                srt_recvmsg2(group, buffer.data(), buffer.size(), &first), 1);
            REQUIRE_EQ(first.srctime,
                primary_runtime->timestamp_origin_microseconds()
                    + static_cast<std::int64_t>(first_deadline));

            // A new member's independent handshake estimate must not replace
            // the established clock, even after the first member disappears.
            auto backup_runtime = attach_group_runtime(group, backup, sequence,
                1, &backup_clock, 0, true, 300, origin, record,
                robotweax::srt::PacketTimestamp {rollover ? 0x10U : 0U});
            REQUIRE_EQ(srt_close(primary), 0);
            primary_runtime.reset();
            const auto step = rollover ? 32U : 1'277U;
            backup_clock.now_microseconds = first_deadline + step - 1;
            inject(backup_runtime, sequence, rollover ? 0xffff'fff0U : 10'000U);
            inject(backup_runtime, SequenceNumber {sequence}.next().value(),
                rollover ? 0x10U : 11'277U);
            SRT_MSGCTRL second = srt_msgctrl_default;
            REQUIRE_EQ(
                srt_recvmsg2(group, buffer.data(), buffer.size(), &second),
                SRT_ERROR);
            REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EASYNCRCV);
            REQUIRE(!backup_runtime->next_readable_message_sequence());
            backup_clock.now_microseconds += 1;
            REQUIRE_EQ(
                srt_recvmsg2(group, buffer.data(), buffer.size(), &second), 1);
            REQUIRE_EQ(second.srctime - first.srctime, step);
            REQUIRE_EQ(second.pktseq,
                static_cast<std::int32_t>(
                    SequenceNumber {sequence}.next().value()));
            REQUIRE_EQ(buffer[0], 'x');
            REQUIRE_EQ(srt_close(group), 0);
        }
    }
}

TEST(compat_group_shared_tsbpd_clock_never_shortens_negotiated_latency)
{
    const auto group = srt_create_group(SRT_GTYPE_BACKUP);
    const auto record = GroupRegistry::instance().find(group);
    REQUIRE(record != nullptr);
    const auto sequence = record->initial_sequence;
    const auto origin = ConnectionRuntime::Clock::now();
    TestClock clock {.now_microseconds = 0,
        .channel = std::make_shared<robotweax::srt::compat::DatagramChannel>()};
    clock.channel->set_send_hook_for_testing(accept_test_datagram, nullptr);
    const auto first = attach_group_runtime(group, srt_create_socket(),
        sequence, 1, &clock, 0, true, 200, origin, record);
    (void)attach_group_runtime(group, srt_create_socket(), sequence, 1, &clock,
        0, true, 300, origin, record);
    (void)attach_group_runtime(group, srt_create_socket(), sequence, 1, &clock,
        0, true, 100, origin, record);
    constexpr std::array<std::byte, 1> payload {std::byte {'x'}};
    robotweax::srt::PacketView packet;
    packet.kind = robotweax::srt::PacketKind::data;
    packet.data.sequence = SequenceNumber {sequence};
    packet.data.message_number = 1;
    packet.data.boundary = robotweax::srt::MessageBoundary::solo;
    packet.data.in_order = true;
    packet.data.timestamp = robotweax::srt::PacketTimestamp {10'000};
    packet.payload = payload;
    first->process_packet(packet, IpEndpoint::loopback(9'000));
    std::array<std::byte, 8> buffer {};
    clock.now_microseconds = 309'999;
    REQUIRE_EQ(first->receive_message(buffer, false, -1).status,
        robotweax::srt::compat::MessageIoStatus::would_block);
    clock.now_microseconds = 310'000;
    const auto received = first->receive_message(buffer, false, -1);
    REQUIRE_EQ(
        received.status, robotweax::srt::compat::MessageIoStatus::success);
    REQUIRE_EQ(received.source_time_microseconds,
        first->timestamp_origin_microseconds() + 310'000);
    REQUIRE_EQ(srt_close(group), 0);
}

TEST(compat_group_receive_preserves_deadline_when_member_joins_after_pop)
{
    for (const auto type : {SRT_GTYPE_BACKUP, SRT_GTYPE_BROADCAST}) {
        const auto group = srt_create_group(type);
        const auto record = GroupRegistry::instance().find(group);
        REQUIRE(record != nullptr);
        const bool asynchronous = false;
        REQUIRE_EQ(srt_setsockopt(group, 0, SRTO_RCVSYN, &asynchronous,
                       sizeof(asynchronous)),
            0);
        const auto sequence = record->initial_sequence;
        const auto origin = ConnectionRuntime::Clock::now();
        TestClock clock {.now_microseconds = 0,
            .channel =
                std::make_shared<robotweax::srt::compat::DatagramChannel>()};
        clock.channel->set_send_hook_for_testing(accept_test_datagram, nullptr);
        struct PopBarrier {
            std::promise<void> popped;
            std::promise<void> joined;
            std::shared_future<void> join_done = joined.get_future().share();
            bool fired = false;
            bool timed_out = false;
        } barrier;
        auto pop_ready = barrier.popped.get_future();
        const auto after_pop = [](void* context) noexcept {
            auto& barrier = *static_cast<PopBarrier*>(context);
            if (barrier.fired) {
                return;
            }
            barrier.fired = true;
            barrier.popped.set_value();
            barrier.timed_out =
                barrier.join_done.wait_for(std::chrono::seconds {5})
                != std::future_status::ready;
        };
        const auto primary =
            attach_group_runtime(group, srt_create_socket(), sequence, 1,
                &clock, 0, true, 120, origin, record, {}, after_pop, &barrier);
        constexpr std::array<std::byte, 1> payload {std::byte {'x'}};
        for (std::uint32_t index = 0; index < 2; ++index) {
            robotweax::srt::PacketView packet;
            packet.kind = robotweax::srt::PacketKind::data;
            packet.data.sequence = SequenceNumber {sequence}.advanced(index);
            packet.data.message_number = index + 1;
            packet.data.boundary = robotweax::srt::MessageBoundary::solo;
            packet.data.in_order = true;
            packet.data.timestamp =
                robotweax::srt::PacketTimestamp {10'000 + index * 1'000};
            packet.payload = payload;
            primary->process_packet(packet, IpEndpoint::loopback(9'000));
        }
        clock.now_microseconds = 130'000;
        TestClock late_clock {
            .now_microseconds = 130'000, .channel = clock.channel};
        auto join = std::async(std::launch::async, [&] {
            if (pop_ready.wait_for(std::chrono::seconds {5})
                != std::future_status::ready) {
                return false;
            }
            (void)attach_group_runtime(group, srt_create_socket(), sequence, 1,
                &late_clock, 0, true, 300, origin, record,
                robotweax::srt::PacketTimestamp {130'000});
            barrier.joined.set_value();
            return true;
        });
        std::array<char, 8> buffer {};
        SRT_MSGCTRL first = srt_msgctrl_default;
        REQUIRE_EQ(
            srt_recvmsg2(group, buffer.data(), buffer.size(), &first), 1);
        REQUIRE(join.get());
        REQUIRE(barrier.fired);
        REQUIRE(!barrier.timed_out);
        REQUIRE_EQ(
            first.srctime, primary->timestamp_origin_microseconds() + 130'000);
        REQUIRE_EQ(buffer[0], 'x');
        // A subsequent pop observes the new member's larger delay.
        clock.now_microseconds = 310'999;
        SRT_MSGCTRL second = srt_msgctrl_default;
        REQUIRE_EQ(srt_recvmsg2(group, buffer.data(), buffer.size(), &second),
            SRT_ERROR);
        REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EASYNCRCV);
        clock.now_microseconds = 311'000;
        REQUIRE_EQ(
            srt_recvmsg2(group, buffer.data(), buffer.size(), &second), 1);
        REQUIRE_EQ(
            second.srctime, primary->timestamp_origin_microseconds() + 311'000);
        REQUIRE_EQ(srt_close(group), 0);
    }
}

TEST(compat_broadcast_group_receive_wakes_at_the_tsbpd_deadline)
{
    const SRTSOCKET group = srt_create_group(SRT_GTYPE_BROADCAST);
    const SRTSOCKET member = srt_create_socket();
    REQUIRE(group != SRT_INVALID_SOCK);
    REQUIRE(member != SRT_INVALID_SOCK);
    const auto group_record = GroupRegistry::instance().find(group);
    REQUIRE(group_record != nullptr);
    std::uint32_t initial_sequence = 0;
    {
        std::lock_guard lock(group_record->mutex);
        initial_sequence = group_record->initial_sequence;
    }
    constexpr std::uint16_t receive_delay_milliseconds = 100;
    const auto origin = ConnectionRuntime::Clock::now();
    const auto runtime = attach_group_runtime(group, member, initial_sequence,
        1, nullptr, 0U, true, receive_delay_milliseconds, origin);

    const int receive_timeout_milliseconds = 500;
    REQUIRE_EQ(srt_setsockflag(group, SRTO_RCVTIMEO,
                   &receive_timeout_milliseconds,
                   static_cast<int>(sizeof(receive_timeout_milliseconds))),
        0);

    constexpr std::array<std::byte, 4> payload{
        std::byte{'t'}, std::byte{'i'}, std::byte{'m'}, std::byte{'e'}};
    robotweax::srt::PacketView packet;
    packet.kind = robotweax::srt::PacketKind::data;
    packet.data.sequence = SequenceNumber {initial_sequence};
    packet.data.message_number = 1;
    packet.data.boundary = robotweax::srt::MessageBoundary::solo;
    packet.data.in_order = true;
    const auto packet_time =
        std::chrono::duration_cast<std::chrono::microseconds>(
            ConnectionRuntime::Clock::now() - origin)
            .count();
    REQUIRE(packet_time >= 0);
    packet.data.timestamp = robotweax::srt::PacketTimestamp {
        static_cast<std::uint32_t>(packet_time)};
    packet.payload = payload;
    runtime->process_packet(packet, IpEndpoint::loopback(9'000));
    const auto expected_deadline = origin
        + std::chrono::microseconds {packet_time
            + static_cast<std::int64_t>(receive_delay_milliseconds) * 1'000};

    std::array<char, 32> received{};
    REQUIRE_EQ(srt_recvmsg2(group, received.data(),
                   static_cast<int>(received.size()), nullptr),
        static_cast<int>(payload.size()));
    const auto completed = ConnectionRuntime::Clock::now();
    REQUIRE(completed >= expected_deadline);
    REQUIRE(completed - expected_deadline < std::chrono::milliseconds {250});
    REQUIRE(std::equal(payload.begin(), payload.end(),
        reinterpret_cast<const std::byte*>(received.data())));
    REQUIRE_EQ(srt_close(group), 0);
}

TEST(compat_group_receive_obeys_nonblocking_and_timeout_contract)
{
    constexpr std::array<SRT_GROUP_TYPE, 2> group_types {
        SRT_GTYPE_BROADCAST, SRT_GTYPE_BACKUP};
    for (const SRT_GROUP_TYPE group_type : group_types) {
        const SRTSOCKET group = srt_create_group(group_type);
        const SRTSOCKET member = srt_create_socket();
        REQUIRE(group != SRT_INVALID_SOCK);
        REQUIRE(member != SRT_INVALID_SOCK);
        const auto group_record = GroupRegistry::instance().find(group);
        REQUIRE(group_record != nullptr);
        std::uint32_t initial_sequence = 0;
        {
            std::lock_guard lock(group_record->mutex);
            initial_sequence = group_record->initial_sequence;
        }
        (void)attach_group_runtime(group, member, initial_sequence);

        std::array<char, 1'500> received {};
        const bool asynchronous = false;
        REQUIRE_EQ(srt_setsockflag(group, SRTO_RCVSYN, &asynchronous,
                       static_cast<int>(sizeof(asynchronous))),
            0);
        const auto nonblocking_started = ConnectionRuntime::Clock::now();
        REQUIRE_EQ(srt_recvmsg(group, received.data(),
                       static_cast<int>(received.size())),
            SRT_ERROR);
        const auto nonblocking_elapsed =
            ConnectionRuntime::Clock::now() - nonblocking_started;
        REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EASYNCRCV);
        REQUIRE(nonblocking_elapsed < std::chrono::milliseconds {100});

        const bool synchronous = true;
        constexpr std::int32_t timeout_milliseconds = 30;
        REQUIRE_EQ(srt_setsockflag(group, SRTO_RCVSYN, &synchronous,
                       static_cast<int>(sizeof(synchronous))),
            0);
        REQUIRE_EQ(srt_setsockflag(group, SRTO_RCVTIMEO, &timeout_milliseconds,
                       static_cast<int>(sizeof(timeout_milliseconds))),
            0);
        const auto timeout_started = ConnectionRuntime::Clock::now();
        REQUIRE_EQ(srt_recvmsg(group, received.data(),
                       static_cast<int>(received.size())),
            SRT_ERROR);
        const auto timeout_elapsed =
            ConnectionRuntime::Clock::now() - timeout_started;
        REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ETIMEOUT);
        REQUIRE(timeout_elapsed >= std::chrono::milliseconds {20});
        REQUIRE(timeout_elapsed < std::chrono::milliseconds {500});

        REQUIRE_EQ(srt_close(group), 0);
    }
}

TEST(compat_group_receive_drains_a_terminal_member_after_tsbpd)
{
    constexpr std::uint16_t receive_delay_milliseconds = 100;
    constexpr std::array<std::byte, 7> payload {std::byte {'g'},
        std::byte {'r'}, std::byte {'o'}, std::byte {'u'}, std::byte {'p'},
        std::byte {'!'}, std::byte {'!'}};
    constexpr std::array<SRT_GROUP_TYPE, 2> group_types {
        SRT_GTYPE_BROADCAST, SRT_GTYPE_BACKUP};
    for (const SRT_GROUP_TYPE group_type : group_types) {
        const SRTSOCKET group = srt_create_group(group_type);
        const SRTSOCKET member = srt_create_socket();
        REQUIRE(group != SRT_INVALID_SOCK);
        REQUIRE(member != SRT_INVALID_SOCK);
        const auto group_record = GroupRegistry::instance().find(group);
        REQUIRE(group_record != nullptr);
        std::uint32_t initial_sequence = 0;
        {
            std::lock_guard lock(group_record->mutex);
            initial_sequence = group_record->initial_sequence;
        }

        TestClock clock {
            .now_microseconds = 0,
            .channel =
                std::make_shared<robotweax::srt::compat::DatagramChannel>(),
        };
        clock.channel->set_send_hook_for_testing(accept_test_datagram, nullptr);
        const auto runtime = attach_group_runtime(group, member,
            initial_sequence, 1, &clock, 0U, true, receive_delay_milliseconds);

        const bool nonblocking = false;
        REQUIRE_EQ(srt_setsockflag(group, SRTO_RCVSYN, &nonblocking,
                       static_cast<int>(sizeof(nonblocking))),
            0);
        robotweax::srt::PacketView data;
        data.kind = robotweax::srt::PacketKind::data;
        data.data.sequence = SequenceNumber {initial_sequence};
        data.data.message_number = 1;
        data.data.boundary = robotweax::srt::MessageBoundary::solo;
        data.data.in_order = true;
        data.data.timestamp = robotweax::srt::PacketTimestamp {0};
        data.payload = payload;
        runtime->process_packet(data, IpEndpoint::loopback(9'000));

        robotweax::srt::PacketView shutdown;
        shutdown.kind = robotweax::srt::PacketKind::control;
        shutdown.control.type = robotweax::srt::ControlType::shutdown;
        shutdown.control.destination_socket_id = 77;
        const std::array<std::byte, 4> shutdown_padding {};
        shutdown.payload = shutdown_padding;
        runtime->process_packet(shutdown, IpEndpoint::loopback(9'000));
        REQUIRE_EQ(srt_getsockstate(member), SRTS_BROKEN);

        std::array<char, payload.size()> received {};
        REQUIRE_EQ(srt_recvmsg(group, received.data(),
                       static_cast<int>(received.size())),
            SRT_ERROR);
        REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EASYNCRCV);

        clock.now_microseconds =
            static_cast<std::uint64_t>(receive_delay_milliseconds) * 1'000U;
        std::array<char, 3> short_buffer {};
        REQUIRE_EQ(srt_recvmsg(group, short_buffer.data(),
                       static_cast<int>(short_buffer.size())),
            SRT_ERROR);
        REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ELARGEMSG);
        REQUIRE_EQ(srt_recvmsg(group, received.data(),
                       static_cast<int>(received.size())),
            static_cast<int>(payload.size()));
        REQUIRE(std::equal(payload.begin(), payload.end(),
            reinterpret_cast<const std::byte*>(received.data())));
        REQUIRE_EQ(srt_recvmsg(group, received.data(),
                       static_cast<int>(received.size())),
            SRT_ERROR);
        REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ECONNLOST);

        REQUIRE_EQ(srt_close(group), 0);
    }
}

TEST(compat_broadcast_group_keeps_sending_after_one_member_path_fails)
{
    const SRTSOCKET group = srt_create_group(SRT_GTYPE_BROADCAST);
    const SRTSOCKET healthy = srt_create_socket();
    const SRTSOCKET failed = srt_create_socket();
    const auto group_record = GroupRegistry::instance().find(group);
    REQUIRE(group_record != nullptr);
    std::uint32_t initial_sequence = 0;
    {
        std::lock_guard lock(group_record->mutex);
        initial_sequence = group_record->initial_sequence;
    }
    const auto healthy_runtime = attach_group_runtime(
        group, healthy, initial_sequence);
    const auto failed_runtime = attach_group_runtime(
        group, failed, initial_sequence);
    failed_runtime->close();

    constexpr char payload[] = "surviving path";
    std::array<SRT_SOCKGROUPDATA, 2> data{};
    SRT_MSGCTRL control = srt_msgctrl_default;
    control.grpdata = data.data();
    control.grpdata_size = data.size();
    REQUIRE_EQ(srt_sendmsg2(group, payload,
                   static_cast<int>(sizeof(payload)), &control),
        static_cast<int>(sizeof(payload)));
    REQUIRE_EQ(control.grpdata_size, 1U);
    const auto healthy_data = std::find_if(
        data.begin(), data.end(), [healthy](const auto& member) {
            return member.id == healthy;
        });
    const auto failed_data = std::find_if(
        data.begin(), data.end(), [failed](const auto& member) {
            return member.id == failed;
        });
    REQUIRE(healthy_data != data.end());
    REQUIRE(failed_data == data.end());
    REQUIRE_EQ(healthy_data->memberstate, SRT_GST_RUNNING);
    REQUIRE_EQ(healthy_data->result,
        static_cast<int>(sizeof(payload)));
    REQUIRE(srt_getsockstate(failed) == SRTS_CLOSING
        || srt_getsockstate(failed) == SRTS_CLOSED);

    const auto duplicate_identity = healthy_runtime->queue_group_message(
        std::as_bytes(std::span {payload, sizeof(payload)}),
        SequenceNumber {initial_sequence}, 1, 0, true);
    REQUIRE_EQ(duplicate_identity.status,
        robotweax::srt::compat::MessageIoStatus::invalid_state);
    REQUIRE_EQ(srt_close(group), 0);
}

TEST(compat_broadcast_group_recovers_a_backpressured_member)
{
    TestClock clock;
    clock.channel = std::make_shared<robotweax::srt::compat::DatagramChannel>();
    clock.channel->set_send_hook_for_testing(accept_test_datagram, nullptr);
    const SRTSOCKET group = srt_create_group(SRT_GTYPE_BROADCAST);
    const SRTSOCKET healthy = srt_create_socket();
    const SRTSOCKET slow = srt_create_socket();
    REQUIRE(group != SRT_INVALID_SOCK);
    REQUIRE(healthy != SRT_INVALID_SOCK);
    REQUIRE(slow != SRT_INVALID_SOCK);
    const auto group_record = GroupRegistry::instance().find(group);
    REQUIRE(group_record != nullptr);
    std::uint32_t initial_sequence = 0;
    {
        std::lock_guard lock(group_record->mutex);
        initial_sequence = group_record->initial_sequence;
    }
    const auto healthy_runtime =
        attach_group_runtime(group, healthy, initial_sequence);
    const auto slow_runtime =
        attach_group_runtime(group, slow, initial_sequence, 1, &clock, 1U);

    constexpr char first[] = "first";
    constexpr char second[] = "second";
    constexpr char third[] = "third";
    constexpr char recovered[] = "recovered";
    REQUIRE_EQ(srt_send(group, first, static_cast<int>(sizeof(first))),
        static_cast<int>(sizeof(first)));
    REQUIRE_EQ(slow_runtime->sender_buffer_status().packets, 1U);

    const auto send_with_members = [&](const char* payload, int size) {
        std::array<SRT_SOCKGROUPDATA, 2> data {};
        SRT_MSGCTRL control = srt_msgctrl_default;
        control.grpdata = data.data();
        control.grpdata_size = data.size();
        REQUIRE_EQ(srt_sendmsg2(group, payload, size, &control), size);
        REQUIRE_EQ(control.grpdata_size, data.size());
        const auto slow_data =
            std::find_if(data.begin(), data.end(), [slow](const auto& member) {
                return member.id == slow;
            });
        REQUIRE(slow_data != data.end());
        return *slow_data;
    };
    const auto blocked = send_with_members(second, sizeof(second));
    REQUIRE_EQ(blocked.memberstate, SRT_GST_IDLE);
    REQUIRE_EQ(blocked.result, SRT_EASYNCSND);
    REQUIRE_EQ(blocked.sockstate, SRTS_CONNECTED);
    REQUIRE_EQ(srt_getsockstate(slow), SRTS_CONNECTED);
    REQUIRE_EQ(healthy_runtime->sender_buffer_status().packets, 2U);

    const auto still_blocked = send_with_members(third, sizeof(third));
    REQUIRE_EQ(still_blocked.memberstate, SRT_GST_IDLE);
    REQUIRE_EQ(still_blocked.result, SRT_EASYNCSND);
    REQUIRE_EQ(srt_getsockstate(slow), SRTS_CONNECTED);

    (void)slow_runtime->poll();
    deliver_lite_ack(slow_runtime, SequenceNumber {initial_sequence}.next());
    REQUIRE_EQ(slow_runtime->sender_buffer_status().packets, 0U);
    const auto resumed = send_with_members(recovered, sizeof(recovered));
    REQUIRE_EQ(resumed.memberstate, SRT_GST_RUNNING);
    REQUIRE_EQ(resumed.result, static_cast<int>(sizeof(recovered)));
    REQUIRE_EQ(srt_getsockstate(slow), SRTS_CONNECTED);
    REQUIRE_EQ(slow_runtime->response_health().next_send_sequence,
        SequenceNumber {initial_sequence}.advanced(4U));
    REQUIRE_EQ(srt_close(group), 0);
}

TEST(compat_broadcast_group_replaces_a_peer_error_member_with_a_new_socket)
{
    const SRTSOCKET group = srt_create_group(SRT_GTYPE_BROADCAST);
    const SRTSOCKET healthy = srt_create_socket();
    const SRTSOCKET failed = srt_create_socket();
    REQUIRE(group != SRT_INVALID_SOCK);
    REQUIRE(healthy != SRT_INVALID_SOCK);
    REQUIRE(failed != SRT_INVALID_SOCK);
    const auto group_record = GroupRegistry::instance().find(group);
    REQUIRE(group_record != nullptr);
    std::uint32_t initial_sequence = 0;
    {
        std::lock_guard lock(group_record->mutex);
        initial_sequence = group_record->initial_sequence;
    }
    (void)attach_group_runtime(group, healthy, initial_sequence);
    const auto failed_runtime = attach_group_runtime(
        group, failed, initial_sequence);
    const int poll = srt_epoll_create();
    REQUIRE(poll >= 0);
    const int watched = SRT_EPOLL_UPDATE;
    REQUIRE_EQ(srt_epoll_add_usock(poll, group, &watched), 0);

    const std::array<std::byte, sizeof(std::uint32_t)> padding{};
    robotweax::srt::PacketView peer_error;
    peer_error.kind = robotweax::srt::PacketKind::control;
    peer_error.control.type = robotweax::srt::ControlType::peer_error;
    peer_error.control.type_specific = SRT_EFILE;
    peer_error.payload = padding;
    failed_runtime->process_packet(
        peer_error, IpEndpoint::loopback(9'000));

    constexpr char first_payload[] = "surviving path";
    REQUIRE_EQ(srt_send(group, first_payload,
                   static_cast<int>(sizeof(first_payload))),
        static_cast<int>(sizeof(first_payload)));
    REQUIRE_EQ(srt_getsockstate(group), SRTS_CONNECTED);
    REQUIRE(srt_getsockstate(failed) == SRTS_CLOSING
        || srt_getsockstate(failed) == SRTS_CLOSED);
    REQUIRE_EQ(srt_getsockstate(healthy), SRTS_CONNECTED);
    std::array<SRT_EPOLL_EVENT, 1> events{};
    REQUIRE_EQ(srt_epoll_uwait(
                   poll, events.data(),
                   static_cast<int>(events.size()), 0),
        1);
    REQUIRE_EQ(events[0].fd, group);
    REQUIRE_EQ(events[0].events, SRT_EPOLL_UPDATE);
    REQUIRE_EQ(srt_epoll_uwait(
                   poll, events.data(),
                   static_cast<int>(events.size()), 0),
        0);

    const SRTSOCKET replacement = srt_create_socket();
    REQUIRE(replacement != SRT_INVALID_SOCK);
    REQUIRE(replacement != failed);
    (void)attach_group_runtime(group, replacement, initial_sequence);

    constexpr char second_payload[] = "replacement path";
    std::array<SRT_SOCKGROUPDATA, 3> data{};
    SRT_MSGCTRL control = srt_msgctrl_default;
    control.grpdata = data.data();
    control.grpdata_size = data.size();
    REQUIRE_EQ(srt_sendmsg2(group, second_payload,
                   static_cast<int>(sizeof(second_payload)), &control),
        static_cast<int>(sizeof(second_payload)));
    REQUIRE_EQ(srt_getsockstate(group), SRTS_CONNECTED);
    const auto replacement_data = std::find_if(
        data.begin(), data.end(), [replacement](const auto& member) {
            return member.id == replacement;
        });
    REQUIRE(replacement_data != data.end());
    REQUIRE_EQ(replacement_data->memberstate, SRT_GST_RUNNING);
    REQUIRE_EQ(replacement_data->result,
        static_cast<int>(sizeof(second_payload)));
    REQUIRE_EQ(srt_close(replacement), 0);
    REQUIRE_EQ(srt_getsockstate(group), SRTS_CONNECTED);
    REQUIRE_EQ(srt_epoll_uwait(
                   poll, events.data(),
                   static_cast<int>(events.size()), 0),
        0);
    REQUIRE_EQ(srt_epoll_release(poll), 0);
    REQUIRE_EQ(srt_close(group), 0);
}

TEST(compat_broadcast_group_rebases_a_late_sender_member)
{
    const SRTSOCKET group = srt_create_group(SRT_GTYPE_BROADCAST);
    const SRTSOCKET first = srt_create_socket();
    const SRTSOCKET late = srt_create_socket();
    const auto group_record = GroupRegistry::instance().find(group);
    REQUIRE(group_record != nullptr);
    std::uint32_t initial_sequence = 0;
    {
        std::lock_guard lock(group_record->mutex);
        initial_sequence = group_record->initial_sequence;
    }
    (void)attach_group_runtime(group, first, initial_sequence);
    constexpr char prefix[] = "prefix";
    REQUIRE_EQ(srt_send(group, prefix, static_cast<int>(sizeof(prefix))),
        static_cast<int>(sizeof(prefix)));

    (void)attach_group_runtime(group, late, initial_sequence);
    constexpr char suffix[] = "late member";
    std::array<SRT_SOCKGROUPDATA, 2> data{};
    SRT_MSGCTRL control = srt_msgctrl_default;
    control.grpdata = data.data();
    control.grpdata_size = data.size();
    REQUIRE_EQ(srt_sendmsg2(group, suffix,
                   static_cast<int>(sizeof(suffix)), &control),
        static_cast<int>(sizeof(suffix)));
    REQUIRE_EQ(control.pktseq,
        static_cast<std::int32_t>(
            SequenceNumber{initial_sequence}.advanced(1U).value()));
    REQUIRE_EQ(control.msgno, 2);
    REQUIRE(std::all_of(data.begin(), data.end(), [](const auto& member) {
        return member.memberstate == SRT_GST_RUNNING
            && member.result == static_cast<int>(sizeof(suffix));
    }));
    REQUIRE_EQ(srt_close(group), 0);
}

TEST(compat_broadcast_group_maps_one_source_time_to_each_member_timebase)
{
    // Low-level queue conversion coverage using deliberately independent
    // fixture runtimes. Public group handshakes now share one wire origin.
    TestClock clock;
    clock.channel = std::make_shared<
        robotweax::srt::compat::DatagramChannel>();
    CapturedGroupDatagrams captured;
    clock.channel->set_send_hook_for_testing(
        capture_group_datagram, &captured);
    const SRTSOCKET group = srt_create_group(SRT_GTYPE_BROADCAST);
    const SRTSOCKET first = srt_create_socket();
    const SRTSOCKET late = srt_create_socket();
    const auto group_record = GroupRegistry::instance().find(group);
    REQUIRE(group_record != nullptr);
    std::uint32_t initial_sequence = 0;
    {
        std::lock_guard lock(group_record->mutex);
        initial_sequence = group_record->initial_sequence;
    }

    const auto first_origin =
        ConnectionRuntime::Clock::now() - std::chrono::seconds{2};
    const auto first_runtime = attach_group_runtime(
        group, first, initial_sequence, 1, &clock, 0U, false, 0U,
        first_origin);
    const std::int64_t group_origin =
        first_runtime->timestamp_origin_microseconds();

    constexpr char prefix[] = "common timebase prefix";
    SRT_MSGCTRL prefix_control = srt_msgctrl_default;
    prefix_control.srctime = group_origin + 100'000;
    REQUIRE_EQ(srt_sendmsg2(group, prefix,
                   static_cast<int>(sizeof(prefix)), &prefix_control),
        static_cast<int>(sizeof(prefix)));
    REQUIRE_EQ(prefix_control.srctime, group_origin + 100'000);
    (void)first_runtime->poll();
    const auto prefix_datagrams = take_group_datagrams(captured);
    const auto prefix_packet = std::find_if(
        prefix_datagrams.begin(), prefix_datagrams.end(),
        [initial_sequence](const auto& datagram) {
            const auto decoded = robotweax::srt::decode_packet(datagram);
            return decoded
                && decoded.packet.kind == robotweax::srt::PacketKind::data
                && decoded.packet.data.sequence
                    == SequenceNumber{initial_sequence};
        });
    REQUIRE(prefix_packet != prefix_datagrams.end());
    REQUIRE_EQ(robotweax::srt::decode_packet(*prefix_packet)
                   .packet.data.timestamp,
        robotweax::srt::PacketTimestamp{100'000U});
    deliver_lite_ack(first_runtime,
        SequenceNumber{initial_sequence}.next());

    clock.now_microseconds = 400'000;
    const auto late_runtime = attach_group_runtime(
        group, late, initial_sequence, 1, &clock, 0U, false, 0U,
        first_origin + std::chrono::microseconds{400'000});
    REQUIRE_EQ(late_runtime->timestamp_origin_microseconds(),
        group_origin + 400'000);

    constexpr char suffix[] = "common timebase suffix";
    SRT_MSGCTRL suffix_control = srt_msgctrl_default;
    suffix_control.srctime = group_origin + 500'000;
    REQUIRE_EQ(srt_sendmsg2(group, suffix,
                   static_cast<int>(sizeof(suffix)), &suffix_control),
        static_cast<int>(sizeof(suffix)));
    REQUIRE_EQ(suffix_control.srctime, group_origin + 500'000);
    (void)first_runtime->poll();
    (void)late_runtime->poll();

    std::vector<robotweax::srt::PacketTimestamp> suffix_timestamps;
    for (const auto& datagram : take_group_datagrams(captured)) {
        const auto decoded = robotweax::srt::decode_packet(datagram);
        if (decoded
            && decoded.packet.kind == robotweax::srt::PacketKind::data
            && decoded.packet.data.sequence
                == SequenceNumber{initial_sequence}.next()) {
            suffix_timestamps.push_back(
                decoded.packet.data.timestamp);
        }
    }
    REQUIRE_EQ(suffix_timestamps.size(), 2U);
    std::ranges::sort(suffix_timestamps, [](const auto left, const auto right) {
        return left.value() < right.value();
    });
    REQUIRE_EQ(
        suffix_timestamps[0], robotweax::srt::PacketTimestamp {100'000U});
    REQUIRE_EQ(
        suffix_timestamps[1], robotweax::srt::PacketTimestamp {500'000U});
    REQUIRE_EQ(srt_close(group), 0);
}

TEST(compat_broadcast_group_rebases_and_deduplicates_a_late_receiver_member)
{
    const SRTSOCKET group = srt_create_group(SRT_GTYPE_BROADCAST);
    const SRTSOCKET first = srt_create_socket();
    const SRTSOCKET late = srt_create_socket();
    const auto group_record = GroupRegistry::instance().find(group);
    REQUIRE(group_record != nullptr);
    std::uint32_t initial_sequence = 0;
    {
        std::lock_guard lock(group_record->mutex);
        initial_sequence = group_record->initial_sequence;
    }
    const auto first_runtime = attach_group_runtime(
        group, first, initial_sequence);
    const IpEndpoint peer = IpEndpoint::loopback(9'000);
    const auto inject = [&](const std::shared_ptr<ConnectionRuntime>& runtime,
                            SequenceNumber sequence,
                            std::uint32_t message,
                            std::span<const std::byte> payload) {
        robotweax::srt::PacketView packet;
        packet.kind = robotweax::srt::PacketKind::data;
        packet.data.sequence = sequence;
        packet.data.message_number = message;
        packet.data.boundary = robotweax::srt::MessageBoundary::solo;
        packet.data.in_order = true;
        packet.payload = payload;
        runtime->process_packet(packet, peer);
    };
    constexpr std::array<std::byte, 3> prefix{
        std::byte{'o'}, std::byte{'n'}, std::byte{'e'}};
    inject(first_runtime, SequenceNumber{initial_sequence}, 1, prefix);
    std::array<char, 32> received{};
    REQUIRE_EQ(srt_recv(group, received.data(),
                   static_cast<int>(received.size())),
        static_cast<int>(prefix.size()));

    const auto late_runtime = attach_group_runtime(
        group, late, initial_sequence);
    constexpr std::array<std::byte, 3> suffix{
        std::byte{'t'}, std::byte{'w'}, std::byte{'o'}};
    const SequenceNumber next =
        SequenceNumber{initial_sequence}.advanced(1U);
    inject(first_runtime, next, 2, suffix);
    inject(late_runtime, next, 2, suffix);
    SRT_MSGCTRL control = srt_msgctrl_default;
    REQUIRE_EQ(srt_recvmsg2(group, received.data(),
                   static_cast<int>(received.size()), &control),
        static_cast<int>(suffix.size()));
    REQUIRE_EQ(control.pktseq, static_cast<std::int32_t>(next.value()));
    REQUIRE_EQ(control.msgno, 2);

    const bool asynchronous = false;
    REQUIRE_EQ(srt_setsockflag(group, SRTO_RCVSYN,
                   &asynchronous,
                   static_cast<int>(sizeof(asynchronous))),
        0);
    REQUIRE_EQ(srt_recv(group, received.data(),
                   static_cast<int>(received.size())),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EASYNCRCV);
    REQUIRE_EQ(srt_close(group), 0);
}

TEST(compat_group_passes_stream_id_latency_and_maxbw_to_members)
{
    // libsrt lets these options be set on a group and passes them to members.
    // They were previously rejected with SRT_EINVPARAM, so a caller-side group
    // could not carry a Stream ID, latency or a rate limit.
    const SRTSOCKET group = srt_create_group(SRT_GTYPE_BROADCAST);
    REQUIRE(group != SRT_INVALID_SOCK);

    constexpr char stream_id[] = "#!::r=live/camera-3";
    REQUIRE_EQ(srt_setsockflag(group, SRTO_STREAMID, stream_id,
                   static_cast<int>(sizeof(stream_id) - 1U)),
        0);
    const std::int32_t receive_latency = 800;
    REQUIRE_EQ(srt_setsockflag(group, SRTO_RCVLATENCY, &receive_latency,
                   static_cast<int>(sizeof(receive_latency))),
        0);
    const std::int32_t peer_latency = 450;
    REQUIRE_EQ(srt_setsockflag(group, SRTO_PEERLATENCY, &peer_latency,
                   static_cast<int>(sizeof(peer_latency))),
        0);
    const std::int64_t maximum_bandwidth = 12'500'000;
    REQUIRE_EQ(srt_setsockflag(group, SRTO_MAXBW, &maximum_bandwidth,
                   static_cast<int>(sizeof(maximum_bandwidth))),
        0);

    // An out-of-range latency is still rejected.
    const std::int32_t invalid_latency = -1;
    REQUIRE_EQ(srt_setsockflag(group, SRTO_RCVLATENCY, &invalid_latency,
                   static_cast<int>(sizeof(invalid_latency))),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);

    GroupRegistry::ConnectDescription description;
    REQUIRE(GroupRegistry::instance().describe_connect(group, description));
    REQUIRE_EQ(description.member_public_options.stream_id.view(),
        (std::string {stream_id, sizeof(stream_id) - 1U}));
    REQUIRE_EQ(description.member_public_options.receiver_latency_milliseconds,
        receive_latency);
    REQUIRE_EQ(description.member_public_options.peer_latency_milliseconds,
        peer_latency);
    REQUIRE_EQ(
        description.member_public_options.maximum_bandwidth_bytes_per_second,
        maximum_bandwidth);
    using robotweax::srt::SocketOption;
    const auto& native = description.member_native_options;
    REQUIRE_EQ(native.get(SocketOption::receiver_latency_milliseconds).value,
        receive_latency);
    REQUIRE_EQ(native.get(SocketOption::peer_latency_milliseconds).value,
        peer_latency);
    REQUIRE_EQ(
        native.get(SocketOption::maximum_bandwidth_bytes_per_second).value,
        maximum_bandwidth);

    // SRTO_LATENCY sets both receiver and peer latency.
    const std::int32_t symmetric_latency = 300;
    REQUIRE_EQ(srt_setsockflag(group, SRTO_LATENCY, &symmetric_latency,
                   static_cast<int>(sizeof(symmetric_latency))),
        0);
    GroupRegistry::ConnectDescription after_latency;
    REQUIRE(GroupRegistry::instance().describe_connect(group, after_latency));
    REQUIRE_EQ(
        after_latency.member_public_options.receiver_latency_milliseconds,
        symmetric_latency);
    REQUIRE_EQ(after_latency.member_public_options.peer_latency_milliseconds,
        symmetric_latency);

    // The options are pre-connect: rejected once the group has opened.
    GroupRegistry::instance().mark_opened(group, after_latency.generation);
    REQUIRE_EQ(srt_setsockflag(group, SRTO_STREAMID, stream_id,
                   static_cast<int>(sizeof(stream_id) - 1U)),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ECONNSOCK);
    REQUIRE_EQ(srt_close(group), 0);
}

TEST(compat_group_io_options_are_owned_by_the_group)
{
    const SRTSOCKET group = srt_create_group(SRT_GTYPE_BROADCAST);
    REQUIRE(group != SRT_INVALID_SOCK);

    const int asynchronous = 0;
    REQUIRE_EQ(srt_setsockflag(group, SRTO_SNDSYN,
                   &asynchronous,
                   static_cast<int>(sizeof(asynchronous))),
        0);
    REQUIRE_EQ(srt_setsockflag(group, SRTO_RCVSYN,
                   &asynchronous,
                   static_cast<int>(sizeof(asynchronous))),
        0);
    const int invalid_boolean = 2;
    REQUIRE_EQ(srt_setsockflag(group, SRTO_SNDSYN,
                   &invalid_boolean,
                   static_cast<int>(sizeof(invalid_boolean))),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);
    constexpr std::int32_t timeout = 47;
    REQUIRE_EQ(srt_setsockflag(group, SRTO_SNDTIMEO,
                   &timeout, static_cast<int>(sizeof(timeout))),
        0);
    REQUIRE_EQ(srt_setsockflag(group, SRTO_RCVTIMEO,
                   &timeout, static_cast<int>(sizeof(timeout))),
        0);

    bool boolean_value = true;
    int size = sizeof(boolean_value);
    REQUIRE_EQ(srt_getsockflag(group, SRTO_SNDSYN,
                   &boolean_value, &size),
        0);
    REQUIRE(!boolean_value);
    REQUIRE_EQ(size, static_cast<int>(sizeof(boolean_value)));

    std::int32_t integer_value = 0;
    size = sizeof(integer_value);
    REQUIRE_EQ(srt_getsockflag(group, SRTO_RCVTIMEO,
                   &integer_value, &size),
        0);
    REQUIRE_EQ(integer_value, timeout);
    REQUIRE_EQ(srt_close(group), 0);
}

TEST(compat_group_derives_version_drift_and_minimum_input_options)
{
    const SRTSOCKET group = srt_create_group(SRT_GTYPE_BROADCAST);
    REQUIRE(group != SRT_INVALID_SOCK);

    bool drift = false;
    int size = static_cast<int>(sizeof(drift));
    REQUIRE_EQ(srt_getsockflag(
                   group, SRTO_DRIFTTRACER, &drift, &size),
        0);
    REQUIRE(drift);
    drift = false;
    REQUIRE_EQ(srt_setsockflag(
                   group, SRTO_DRIFTTRACER, &drift,
                   static_cast<int>(sizeof(drift))),
        0);

    std::int64_t minimum_input = 350'000;
    REQUIRE_EQ(srt_setsockflag(
                   group, SRTO_MININPUTBW, &minimum_input,
                   static_cast<int>(sizeof(minimum_input))),
        0);
    std::int32_t minimum_version = 0x0001'0500;
    REQUIRE_EQ(srt_setsockflag(
                   group, SRTO_MINVERSION, &minimum_version,
                   static_cast<int>(sizeof(minimum_version))),
        0);

    GroupRegistry::ConnectDescription description;
    REQUIRE(GroupRegistry::instance().describe_connect(group, description));
    REQUIRE(!description.drift_tracer);
    REQUIRE_EQ(description.minimum_input_bandwidth_bytes_per_second,
        350'000);
    REQUIRE_EQ(description.minimum_peer_srt_version, 0x0001'0500);

    minimum_input = 0;
    size = static_cast<int>(sizeof(minimum_input));
    REQUIRE_EQ(srt_getsockflag(
                   group, SRTO_MININPUTBW, &minimum_input, &size),
        0);
    REQUIRE_EQ(minimum_input, 350'000);
    minimum_version = 0;
    size = static_cast<int>(sizeof(minimum_version));
    REQUIRE_EQ(srt_getsockflag(
                   group, SRTO_MINVERSION, &minimum_version, &size),
        0);
    REQUIRE_EQ(minimum_version, 0x0001'0500);
    GroupRegistry::instance().mark_opened(
        group, description.generation);
    minimum_version = 0x0001'0400;
    REQUIRE_EQ(srt_setsockflag(
                   group, SRTO_MINVERSION, &minimum_version,
                   static_cast<int>(sizeof(minimum_version))),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ECONNSOCK);
    drift = true;
    REQUIRE_EQ(srt_setsockflag(
                   group, SRTO_DRIFTTRACER, &drift,
                   static_cast<int>(sizeof(drift))),
        0);
    minimum_input = 360'000;
    REQUIRE_EQ(srt_setsockflag(
                   group, SRTO_MININPUTBW, &minimum_input,
                   static_cast<int>(sizeof(minimum_input))),
        0);
    REQUIRE_EQ(srt_close(group), 0);
}

TEST(compat_backup_group_owns_the_minimum_stability_timeout)
{
    const SRTSOCKET group = srt_create_group(SRT_GTYPE_BACKUP);
    const SRTSOCKET broadcast = srt_create_group(SRT_GTYPE_BROADCAST);
    REQUIRE(group != SRT_INVALID_SOCK);
    REQUIRE(broadcast != SRT_INVALID_SOCK);

    std::int32_t value = 0;
    int size = sizeof(value);
    REQUIRE_EQ(srt_getsockflag(group, SRTO_GROUPMINSTABLETIMEO,
                   &value, &size),
        0);
    REQUIRE_EQ(value, 60);
    REQUIRE_EQ(size, static_cast<int>(sizeof(value)));

    value = 175;
    REQUIRE_EQ(srt_setsockflag(group, SRTO_GROUPMINSTABLETIMEO,
                   &value, static_cast<int>(sizeof(value))),
        0);
    value = 0;
    size = sizeof(value);
    REQUIRE_EQ(srt_getsockflag(group, SRTO_GROUPMINSTABLETIMEO,
                   &value, &size),
        0);
    REQUIRE_EQ(value, 175);

    value = 59;
    REQUIRE_EQ(srt_setsockflag(group, SRTO_GROUPMINSTABLETIMEO,
                   &value, static_cast<int>(sizeof(value))),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);
    value = 5'001;
    REQUIRE_EQ(srt_setsockflag(group, SRTO_GROUPMINSTABLETIMEO,
                   &value, static_cast<int>(sizeof(value))),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);
    value = 60;
    REQUIRE_EQ(srt_setsockflag(broadcast, SRTO_GROUPMINSTABLETIMEO,
                   &value, static_cast<int>(sizeof(value))),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);

    const SRTSOCKET socket = srt_create_socket();
    REQUIRE(socket != SRT_INVALID_SOCK);
    size = static_cast<int>(sizeof(value));
    REQUIRE_EQ(srt_getsockflag(socket, SRTO_GROUPMINSTABLETIMEO, &value, &size),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVOP);
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_GROUPMINSTABLETIMEO, &value,
                   static_cast<int>(sizeof(value))),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVOP);
    REQUIRE_EQ(srt_close(socket), 0);
    REQUIRE_EQ(srt_close(group), 0);
    REQUIRE_EQ(srt_close(broadcast), 0);
}

TEST(compat_backup_group_sends_only_over_the_highest_weight_member)
{
    const SRTSOCKET group = srt_create_group(SRT_GTYPE_BACKUP);
    const SRTSOCKET lower = srt_create_socket();
    const SRTSOCKET higher = srt_create_socket();
    REQUIRE(group != SRT_INVALID_SOCK);
    const auto group_record = GroupRegistry::instance().find(group);
    REQUIRE(group_record != nullptr);
    std::uint32_t initial_sequence = 0;
    {
        std::lock_guard lock(group_record->mutex);
        initial_sequence = group_record->initial_sequence;
    }
    const auto lower_runtime = attach_group_runtime(
        group, lower, initial_sequence, 3);
    const auto higher_runtime = attach_group_runtime(
        group, higher, initial_sequence, 9);

    constexpr char payload[] = "main path";
    std::array<SRT_SOCKGROUPDATA, 2> data{};
    SRT_MSGCTRL control = srt_msgctrl_default;
    control.grpdata = data.data();
    control.grpdata_size = data.size();
    REQUIRE_EQ(srt_sendmsg2(group, payload,
                   static_cast<int>(sizeof(payload)), &control),
        static_cast<int>(sizeof(payload)));
    REQUIRE_EQ(control.pktseq,
        static_cast<std::int32_t>(initial_sequence));
    REQUIRE_EQ(control.msgno, 1);
    REQUIRE_EQ(lower_runtime->sender_buffer_status().packets, 0U);
    REQUIRE_EQ(higher_runtime->sender_buffer_status().packets, 1U);
    const auto lower_data = std::find_if(
        data.begin(), data.end(), [lower](const auto& member) {
            return member.id == lower;
        });
    const auto higher_data = std::find_if(
        data.begin(), data.end(), [higher](const auto& member) {
            return member.id == higher;
        });
    REQUIRE(lower_data != data.end());
    REQUIRE(higher_data != data.end());
    REQUIRE_EQ(lower_data->memberstate, SRT_GST_IDLE);
    REQUIRE_EQ(lower_data->result, SRT_SUCCESS);
    REQUIRE_EQ(higher_data->memberstate, SRT_GST_RUNNING);
    REQUIRE_EQ(higher_data->result,
        static_cast<int>(sizeof(payload)));
    REQUIRE_EQ(srt_close(group), 0);
}

TEST(compat_backup_group_breaks_equal_weight_ties_by_socket_id)
{
    const SRTSOCKET group = srt_create_group(SRT_GTYPE_BACKUP);
    const SRTSOCKET first_id = srt_create_socket();
    const SRTSOCKET second_id = srt_create_socket();
    REQUIRE(first_id != second_id);
    // Handles are random; order them so the tie break is observable.
    const SRTSOCKET lower_id = std::min(first_id, second_id);
    const SRTSOCKET higher_id = std::max(first_id, second_id);
    const auto group_record = GroupRegistry::instance().find(group);
    REQUIRE(group_record != nullptr);
    std::uint32_t initial_sequence = 0;
    {
        std::lock_guard lock(group_record->mutex);
        initial_sequence = group_record->initial_sequence;
    }
    const auto lower_runtime = attach_group_runtime(
        group, lower_id, initial_sequence, 8);
    const auto higher_runtime = attach_group_runtime(
        group, higher_id, initial_sequence, 8);

    constexpr char payload[] = "stable tie";
    REQUIRE_EQ(srt_send(group, payload, static_cast<int>(sizeof(payload))),
        static_cast<int>(sizeof(payload)));
    REQUIRE_EQ(lower_runtime->sender_buffer_status().packets, 1U);
    REQUIRE_EQ(higher_runtime->sender_buffer_status().packets, 0U);
    REQUIRE_EQ(srt_close(group), 0);
}

TEST(compat_backup_group_fails_over_to_the_next_weighted_member)
{
    const SRTSOCKET group = srt_create_group(SRT_GTYPE_BACKUP);
    const SRTSOCKET primary = srt_create_socket();
    const SRTSOCKET backup = srt_create_socket();
    const auto group_record = GroupRegistry::instance().find(group);
    REQUIRE(group_record != nullptr);
    std::uint32_t initial_sequence = 0;
    {
        std::lock_guard lock(group_record->mutex);
        initial_sequence = group_record->initial_sequence;
    }
    const auto primary_runtime = attach_group_runtime(
        group, primary, initial_sequence, 20);
    const auto backup_runtime = attach_group_runtime(
        group, backup, initial_sequence, 10);

    constexpr char prefix[] = "unacknowledged prefix";
    REQUIRE_EQ(srt_send(group, prefix, static_cast<int>(sizeof(prefix))),
        static_cast<int>(sizeof(prefix)));
    primary_runtime->close();

    constexpr char payload[] = "failover";
    std::array<SRT_SOCKGROUPDATA, 2> data{};
    SRT_MSGCTRL control = srt_msgctrl_default;
    control.grpdata = data.data();
    control.grpdata_size = data.size();
    REQUIRE_EQ(srt_sendmsg2(group, payload,
                   static_cast<int>(sizeof(payload)), &control),
        static_cast<int>(sizeof(payload)));
    REQUIRE_EQ(control.pktseq,
        static_cast<std::int32_t>(
            SequenceNumber{initial_sequence}.next().value()));
    REQUIRE_EQ(control.msgno, 2);
    REQUIRE_EQ(backup_runtime->sender_buffer_status().packets, 2U);
    const auto primary_data = std::find_if(
        data.begin(), data.end(), [primary](const auto& member) {
            return member.id == primary;
        });
    const auto backup_data = std::find_if(
        data.begin(), data.end(), [backup](const auto& member) {
            return member.id == backup;
        });
    REQUIRE_EQ(control.grpdata_size, 1U);
    REQUIRE(primary_data == data.end());
    REQUIRE(backup_data != data.end());
    REQUIRE(srt_getsockstate(primary) == SRTS_CLOSING
        || srt_getsockstate(primary) == SRTS_CLOSED);
    REQUIRE_EQ(backup_data->memberstate, SRT_GST_RUNNING);
    REQUIRE_EQ(backup_data->result,
        static_cast<int>(sizeof(payload)));
    REQUIRE_EQ(srt_close(group), 0);
    REQUIRE(!group_record->replay_history.storage_allocated());
}

TEST(compat_backup_group_advances_retired_standby_prefix_with_dropreq)
{
    TestClock clock;
    clock.channel = std::make_shared<
        robotweax::srt::compat::DatagramChannel>();
    CapturedGroupDatagrams captured;
    clock.channel->set_send_hook_for_testing(capture_group_datagram, &captured);
    const SRTSOCKET group = srt_create_group(SRT_GTYPE_BACKUP);
    const SRTSOCKET primary = srt_create_socket();
    const SRTSOCKET backup = srt_create_socket();
    const auto group_record = GroupRegistry::instance().find(group);
    REQUIRE(group_record != nullptr);
    std::uint32_t initial_sequence = 0;
    {
        std::lock_guard lock(group_record->mutex);
        initial_sequence = group_record->initial_sequence;
    }
    const auto primary_runtime = attach_group_runtime(
        group, primary, initial_sequence, 20, &clock);
    const auto backup_runtime = attach_group_runtime(
        group, backup, initial_sequence, 10, &clock);

    constexpr char acknowledged_prefix[] = "acknowledged prefix";
    REQUIRE_EQ(srt_send(group, acknowledged_prefix,
                   static_cast<int>(sizeof(acknowledged_prefix))),
        static_cast<int>(sizeof(acknowledged_prefix)));
    deliver_lite_ack(primary_runtime,
        SequenceNumber{initial_sequence}.next());
    primary_runtime->close();
    (void)take_group_datagrams(captured);

    constexpr char failover[] = "only outstanding data";
    REQUIRE_EQ(srt_send(group, failover,
                   static_cast<int>(sizeof(failover))),
        static_cast<int>(sizeof(failover)));
    REQUIRE_EQ(backup_runtime->sender_buffer_status().packets, 1U);
    REQUIRE_EQ(backup_runtime->response_health().next_send_sequence,
        SequenceNumber{initial_sequence}.advanced(2U));

    (void)backup_runtime->poll();
    const auto datagrams = take_group_datagrams(captured);
    REQUIRE(datagrams.size() >= 2U);
    const auto drop = robotweax::srt::decode_packet(datagrams[0]);
    REQUIRE(drop);
    REQUIRE_EQ(drop.packet.kind, robotweax::srt::PacketKind::control);
    REQUIRE_EQ(
        drop.packet.control.type, robotweax::srt::ControlType::drop_request);
    const auto skipped = robotweax::srt::decode_drop_request(drop.packet);
    REQUIRE(skipped);
    REQUIRE_EQ(skipped.request.message_number, 0U);
    REQUIRE_EQ(
        skipped.request.sequences.first, SequenceNumber {initial_sequence});
    REQUIRE_EQ(
        skipped.request.sequences.last, SequenceNumber {initial_sequence});
    const auto data = robotweax::srt::decode_packet(datagrams[1]);
    REQUIRE(data);
    REQUIRE_EQ(data.packet.kind, robotweax::srt::PacketKind::data);
    REQUIRE_EQ(
        data.packet.data.sequence, SequenceNumber {initial_sequence}.next());
    REQUIRE_EQ(srt_close(group), 0);
}

TEST(compat_backup_group_fails_closed_when_required_history_was_evicted)
{
    const SRTSOCKET group = srt_create_group(SRT_GTYPE_BACKUP);
    const SRTSOCKET primary = srt_create_socket();
    const SRTSOCKET backup = srt_create_socket();
    const auto group_record = GroupRegistry::instance().find(group);
    REQUIRE(group_record != nullptr);
    std::uint32_t initial_sequence = 0;
    {
        std::lock_guard lock(group_record->mutex);
        initial_sequence = group_record->initial_sequence;
    }
    {
        std::lock_guard lock(group_record->send_mutex);
        group_record->replay_history = GroupReplayBuffer(64U, 1U);
    }
    const auto primary_runtime = attach_group_runtime(
        group, primary, initial_sequence, 20);
    const auto backup_runtime = attach_group_runtime(
        group, backup, initial_sequence, 10);

    constexpr char first[] = "first";
    constexpr char second[] = "second";
    REQUIRE_EQ(srt_send(group, first, static_cast<int>(sizeof(first))),
        static_cast<int>(sizeof(first)));
    REQUIRE_EQ(srt_send(group, second, static_cast<int>(sizeof(second))),
        static_cast<int>(sizeof(second)));
    primary_runtime->close();

    constexpr char unsafe_suffix[] = "must not skip the gap";
    REQUIRE_EQ(srt_send(group, unsafe_suffix,
                   static_cast<int>(sizeof(unsafe_suffix))),
        SRT_ERROR);
    REQUIRE_EQ(backup_runtime->sender_buffer_status().packets, 0U);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ESCLOSED);
    REQUIRE_EQ(srt_close(group), 0);
}

TEST(compat_group_close_unblocks_a_send_waiting_for_member_capacity)
{
    // A blocking group send parks in its readiness wait while every member
    // reports would_block. srt_close(group) from another thread must
    // return promptly and wake that sender with an error instead of
    // waiting behind the send coordinator lock forever.
    for (const auto type : {SRT_GTYPE_BROADCAST, SRT_GTYPE_BACKUP}) {
        TestClock clock;
        clock.channel =
            std::make_shared<robotweax::srt::compat::DatagramChannel>();
        clock.channel->set_send_hook_for_testing(accept_test_datagram, nullptr);
        const SRTSOCKET group = srt_create_group(type);
        const SRTSOCKET member = srt_create_socket();
        const auto group_record = GroupRegistry::instance().find(group);
        REQUIRE(group_record != nullptr);
        std::uint32_t initial_sequence = 0;
        {
            std::lock_guard lock(group_record->mutex);
            initial_sequence = group_record->initial_sequence;
        }
        const auto runtime = attach_group_runtime(
            group, member, initial_sequence, 10, &clock, 1U);

        constexpr char first[] = "fills the only slot";
        REQUIRE_EQ(srt_send(group, first, static_cast<int>(sizeof(first))),
            static_cast<int>(sizeof(first)));

        std::atomic<int> send_result {0};
        std::atomic<int> send_error {SRT_SUCCESS};
        std::atomic<bool> send_started {false};
        std::thread sender([&] {
            constexpr char second[] = "waits for capacity";
            send_started.store(true);
            send_result.store(
                srt_send(group, second, static_cast<int>(sizeof(second))));
            send_error.store(srt_getlasterror(nullptr));
        });
        while (!send_started.load()) {
            std::this_thread::yield();
        }
        // Give the sender time to reach its wait; the send buffer holds one
        // packet and no ACK ever arrives.
        std::this_thread::sleep_for(std::chrono::milliseconds {50});
        REQUIRE_EQ(runtime->sender_buffer_status().packets, 1U);

        const auto close_started = std::chrono::steady_clock::now();
        REQUIRE_EQ(srt_close(group), 0);
        const auto close_elapsed =
            std::chrono::steady_clock::now() - close_started;
        REQUIRE(close_elapsed < std::chrono::seconds {5});
        sender.join();
        REQUIRE_EQ(send_result.load(), SRT_ERROR);
        REQUIRE(send_error.load() == SRT_ESCLOSED
            || send_error.load() == SRT_ENOCONN);
    }
}

TEST(compat_backup_group_resumes_replay_at_member_buffer_capacity)
{
    TestClock clock;
    clock.channel = std::make_shared<
        robotweax::srt::compat::DatagramChannel>();
    clock.channel->set_send_hook_for_testing(
        accept_test_datagram, nullptr);
    const SRTSOCKET group = srt_create_group(SRT_GTYPE_BACKUP);
    const SRTSOCKET primary = srt_create_socket();
    const SRTSOCKET backup = srt_create_socket();
    const auto group_record = GroupRegistry::instance().find(group);
    REQUIRE(group_record != nullptr);
    std::uint32_t initial_sequence = 0;
    {
        std::lock_guard lock(group_record->mutex);
        initial_sequence = group_record->initial_sequence;
    }
    const auto primary_runtime = attach_group_runtime(
        group, primary, initial_sequence, 20, &clock);
    const auto backup_runtime = attach_group_runtime(
        group, backup, initial_sequence, 10, &clock, 1U);

    constexpr char first[] = "first outstanding";
    constexpr char second[] = "second outstanding";
    REQUIRE_EQ(srt_send(group, first, static_cast<int>(sizeof(first))),
        static_cast<int>(sizeof(first)));
    REQUIRE_EQ(srt_send(group, second, static_cast<int>(sizeof(second))),
        static_cast<int>(sizeof(second)));
    primary_runtime->close();
    const bool asynchronous = false;
    REQUIRE_EQ(srt_setsockflag(group, SRTO_SNDSYN,
                   &asynchronous,
                   static_cast<int>(sizeof(asynchronous))),
        0);

    constexpr char current[] = "current message";
    REQUIRE_EQ(srt_send(group, current, static_cast<int>(sizeof(current))),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EASYNCSND);
    REQUIRE_EQ(backup_runtime->sender_buffer_status().packets, 1U);
    (void)backup_runtime->poll();
    deliver_lite_ack(backup_runtime,
        SequenceNumber{initial_sequence}.next());

    REQUIRE_EQ(srt_send(group, current, static_cast<int>(sizeof(current))),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EASYNCSND);
    REQUIRE_EQ(backup_runtime->sender_buffer_status().packets, 1U);
    (void)backup_runtime->poll();
    deliver_lite_ack(backup_runtime,
        SequenceNumber{initial_sequence}.advanced(2U));

    REQUIRE_EQ(srt_send(group, current, static_cast<int>(sizeof(current))),
        static_cast<int>(sizeof(current)));
    REQUIRE_EQ(backup_runtime->sender_buffer_status().packets, 1U);
    REQUIRE_EQ(backup_runtime->response_health().next_send_sequence,
        SequenceNumber{initial_sequence}.advanced(3U));
    REQUIRE_EQ(srt_close(group), 0);
}

TEST(compat_backup_group_replays_across_sequence_rollover)
{
    TestClock clock;
    clock.channel = std::make_shared<
        robotweax::srt::compat::DatagramChannel>();
    CapturedGroupDatagrams captured;
    clock.channel->set_send_hook_for_testing(
        capture_group_datagram, &captured);
    const SRTSOCKET group = srt_create_group(SRT_GTYPE_BACKUP);
    const SRTSOCKET primary = srt_create_socket();
    const SRTSOCKET backup = srt_create_socket();
    const auto group_record = GroupRegistry::instance().find(group);
    REQUIRE(group_record != nullptr);
    constexpr std::uint32_t initial_sequence = SequenceNumber::mask;
    {
        std::lock_guard lock(group_record->mutex);
        group_record->initial_sequence = initial_sequence;
        group_record->next_send_sequence = initial_sequence;
        group_record->replay_acknowledged_sequence = initial_sequence;
        group_record->next_receive_sequence = initial_sequence;
    }
    const auto primary_origin = ConnectionRuntime::Clock::now();
    const auto primary_runtime = attach_group_runtime(group, primary,
        initial_sequence, 20, &clock, 0U, false, 0U, primary_origin);
    const auto backup_runtime =
        attach_group_runtime(group, backup, initial_sequence, 10, &clock, 0U,
            false, 0U, primary_origin + std::chrono::microseconds {400'000});
    const std::int64_t group_origin =
        primary_runtime->timestamp_origin_microseconds();

    constexpr char before_wrap[] = "before wrap";
    constexpr char after_wrap[] = "after wrap";
    SRT_MSGCTRL before_control = srt_msgctrl_default;
    before_control.srctime = group_origin + 0xffff'fff0LL;
    REQUIRE_EQ(srt_sendmsg2(group, before_wrap,
                   static_cast<int>(sizeof(before_wrap)),
                   &before_control),
        static_cast<int>(sizeof(before_wrap)));
    SRT_MSGCTRL after_control = srt_msgctrl_default;
    after_control.srctime = group_origin + 0x1'0000'0010LL;
    REQUIRE_EQ(srt_sendmsg2(group, after_wrap,
                   static_cast<int>(sizeof(after_wrap)),
                   &after_control),
        static_cast<int>(sizeof(after_wrap)));
    primary_runtime->close();
    (void)take_group_datagrams(captured);

    constexpr char failover[] = "post-wrap failover";
    SRT_MSGCTRL control = srt_msgctrl_default;
    control.srctime = group_origin + 0x1'0000'0020LL;
    REQUIRE_EQ(srt_sendmsg2(group, failover,
                   static_cast<int>(sizeof(failover)), &control),
        static_cast<int>(sizeof(failover)));
    REQUIRE_EQ(control.pktseq, 1);
    REQUIRE_EQ(control.msgno, 3);
    REQUIRE_EQ(backup_runtime->sender_buffer_status().packets, 3U);
    REQUIRE_EQ(backup_runtime->response_health().next_send_sequence,
        SequenceNumber{2U});

    const auto backup_origin = backup_runtime->timestamp_origin_microseconds();
    const std::array expected {
        std::pair {SequenceNumber {initial_sequence},
            robotweax::srt::PacketTimestamp {static_cast<std::uint32_t>(
                before_control.srctime - backup_origin)}},
        std::pair {SequenceNumber {0U},
            robotweax::srt::PacketTimestamp {static_cast<std::uint32_t>(
                after_control.srctime - backup_origin)}},
        std::pair {SequenceNumber {1U},
            robotweax::srt::PacketTimestamp {
                static_cast<std::uint32_t>(control.srctime - backup_origin)}},
    };
    for (const auto& [sequence, timestamp] : expected) {
        clock.now_microseconds += 100'000;
        (void)backup_runtime->poll();
        const auto datagrams = take_group_datagrams(captured);
        const auto packet = std::find_if(
            datagrams.begin(), datagrams.end(),
            [sequence](const auto& datagram) {
                const auto decoded =
                    robotweax::srt::decode_packet(datagram);
                return decoded
                    && decoded.packet.kind
                        == robotweax::srt::PacketKind::data
                    && decoded.packet.data.sequence == sequence;
            });
        REQUIRE(packet != datagrams.end());
        REQUIRE_EQ(robotweax::srt::decode_packet(*packet)
                       .packet.data.timestamp,
            timestamp);
        deliver_lite_ack(backup_runtime, sequence.next());
    }
    REQUIRE_EQ(srt_close(group), 0);
}

TEST(compat_backup_group_ack_qualifies_a_silent_active_replacement)
{
    TestClock clock;
    clock.channel = std::make_shared<robotweax::srt::compat::DatagramChannel>();
    clock.channel->set_send_hook_for_testing(accept_test_datagram, nullptr);
    const SRTSOCKET group = srt_create_group(SRT_GTYPE_BACKUP);
    const SRTSOCKET original = srt_create_socket();
    const SRTSOCKET preferred = srt_create_socket();
    const auto group_record = GroupRegistry::instance().find(group);
    REQUIRE(group_record != nullptr);
    std::uint32_t initial_sequence = 0;
    {
        std::lock_guard lock(group_record->mutex);
        initial_sequence = group_record->initial_sequence;
    }
    const auto original_runtime = attach_group_runtime(
        group, original, initial_sequence, 15, &clock);
    const auto preferred_runtime = attach_group_runtime(
        group, preferred, initial_sequence, 5, &clock);
    constexpr char prefix[] = "prefix";
    REQUIRE_EQ(srt_send(group, prefix, static_cast<int>(sizeof(prefix))),
        static_cast<int>(sizeof(prefix)));
    REQUIRE_EQ(original_runtime->sender_buffer_status().packets, 1U);
    REQUIRE_EQ(preferred_runtime->sender_buffer_status().packets, 0U);

    // The initial RTT estimator is 100 ms with 50 ms variation, making the
    // dynamic threshold max(60 ms, 2*SRTT + 4*RTTVar) = 400 ms.
    clock.now_microseconds = 400'001;
    constexpr char failover[] = "qualified replacement";
    REQUIRE_EQ(srt_send(group, failover, static_cast<int>(sizeof(failover))),
        static_cast<int>(sizeof(failover)));
    REQUIRE_EQ(original_runtime->sender_buffer_status().packets, 2U);
    REQUIRE_EQ(preferred_runtime->sender_buffer_status().packets, 2U);
    {
        std::lock_guard lock(group_record->mutex);
        REQUIRE_EQ(group_record->active_send_member, original);
        REQUIRE_EQ(group_record->probe_send_member, preferred);
    }

    // Local buffer acceptance alone cannot promote the replacement. It must
    // acknowledge the probe and remain responsive for the complete dynamic
    // stability interval.
    deliver_lite_ack(
        preferred_runtime, SequenceNumber {initial_sequence}.advanced(2U));
    // At exactly one complete 400 ms stability interval the qualifying ACK
    // is still within that same response window.
    clock.now_microseconds = 800'001;
    constexpr char promoted[] = "acknowledged replacement";
    REQUIRE_EQ(srt_send(group, promoted, static_cast<int>(sizeof(promoted))),
        static_cast<int>(sizeof(promoted)));
    REQUIRE_EQ(original_runtime->sender_buffer_status().packets, 2U);
    REQUIRE_EQ(preferred_runtime->sender_buffer_status().packets, 1U);
    {
        std::lock_guard lock(group_record->mutex);
        REQUIRE_EQ(group_record->active_send_member, preferred);
        REQUIRE_EQ(group_record->probe_send_member, SRT_INVALID_SOCK);
    }
    REQUIRE_EQ(srt_close(group), 0);
}

TEST(compat_backup_group_qualifies_a_better_path_in_parallel_before_promotion)
{
    TestClock clock;
    clock.channel = std::make_shared<
        robotweax::srt::compat::DatagramChannel>();
    clock.channel->set_send_hook_for_testing(
        accept_test_datagram, nullptr);
    const SRTSOCKET group = srt_create_group(SRT_GTYPE_BACKUP);
    const SRTSOCKET original = srt_create_socket();
    const SRTSOCKET preferred = srt_create_socket();
    const auto group_record = GroupRegistry::instance().find(group);
    REQUIRE(group_record != nullptr);
    std::uint32_t initial_sequence = 0;
    {
        std::lock_guard lock(group_record->mutex);
        initial_sequence = group_record->initial_sequence;
    }
    const auto original_runtime = attach_group_runtime(
        group, original, initial_sequence, 5, &clock);
    constexpr char prefix[] = "prefix";
    REQUIRE_EQ(srt_send(group, prefix, static_cast<int>(sizeof(prefix))),
        static_cast<int>(sizeof(prefix)));

    clock.now_microseconds = 10'000;
    const auto preferred_runtime = attach_group_runtime(
        group, preferred, initial_sequence, 15, &clock);
    constexpr char probe[] = "parallel probe";
    std::array<SRT_SOCKGROUPDATA, 2> probe_data{};
    SRT_MSGCTRL probe_control = srt_msgctrl_default;
    probe_control.grpdata = probe_data.data();
    probe_control.grpdata_size = probe_data.size();
    REQUIRE_EQ(srt_sendmsg2(group, probe,
                   static_cast<int>(sizeof(probe)), &probe_control),
        static_cast<int>(sizeof(probe)));
    REQUIRE_EQ(original_runtime->sender_buffer_status().packets, 2U);
    REQUIRE_EQ(preferred_runtime->sender_buffer_status().packets, 2U);
    const auto original_data = std::find_if(
        probe_data.begin(), probe_data.end(), [original](const auto& member) {
            return member.id == original;
        });
    const auto preferred_data = std::find_if(
        probe_data.begin(), probe_data.end(), [preferred](const auto& member) {
            return member.id == preferred;
        });
    REQUIRE(original_data != probe_data.end());
    REQUIRE(preferred_data != probe_data.end());
    REQUIRE_EQ(original_data->memberstate, SRT_GST_RUNNING);
    REQUIRE_EQ(preferred_data->memberstate, SRT_GST_RUNNING);

    // Peer activity alone is not sufficient for promotion. Once the dynamic
    // 400 ms stability interval has elapsed, the candidate remains a probe
    // until it has cumulatively acknowledged group data.
    clock.now_microseconds = 300'000;
    robotweax::srt::PacketView keepalive;
    keepalive.kind = robotweax::srt::PacketKind::control;
    keepalive.control.type = robotweax::srt::ControlType::keepalive;
    const std::array<std::byte, 4> keepalive_padding {};
    keepalive.payload = keepalive_padding;
    original_runtime->process_packet(
        keepalive, IpEndpoint::loopback(9'000));
    preferred_runtime->process_packet(
        keepalive, IpEndpoint::loopback(9'000));
    clock.now_microseconds = 410'001;
    constexpr char awaiting_ack[] = "awaiting acknowledgement";
    REQUIRE_EQ(srt_send(group, awaiting_ack,
                   static_cast<int>(sizeof(awaiting_ack))),
        static_cast<int>(sizeof(awaiting_ack)));
    REQUIRE_EQ(original_runtime->sender_buffer_status().packets, 3U);
    REQUIRE_EQ(preferred_runtime->sender_buffer_status().packets, 3U);

    // An ACK on only the active path neither retires the common replay history
    // nor qualifies the candidate.
    clock.now_microseconds = 420'000;
    const SequenceNumber active_acknowledged =
        SequenceNumber{initial_sequence}.advanced(3U);
    deliver_lite_ack(original_runtime, active_acknowledged);
    clock.now_microseconds = 420'001;
    constexpr char one_sided_ack[] = "one-sided acknowledgement";
    REQUIRE_EQ(srt_send(group, one_sided_ack,
                   static_cast<int>(sizeof(one_sided_ack))),
        static_cast<int>(sizeof(one_sided_ack)));
    REQUIRE_EQ(original_runtime->sender_buffer_status().packets, 1U);
    REQUIRE_EQ(preferred_runtime->sender_buffer_status().packets, 4U);
    {
        std::lock_guard lock(group_record->mutex);
        REQUIRE_EQ(group_record->replay_acknowledged_sequence,
            initial_sequence);
    }

    // Once both paths acknowledge the complete probe range, the next
    // scheduling decision can retire the common history and promote the
    // higher-weight path without replaying confirmed data.
    clock.now_microseconds = 430'000;
    const SequenceNumber commonly_acknowledged =
        SequenceNumber{initial_sequence}.advanced(4U);
    deliver_lite_ack(original_runtime, commonly_acknowledged);
    deliver_lite_ack(preferred_runtime, commonly_acknowledged);
    REQUIRE_EQ(original_runtime->sender_buffer_status().packets, 0U);
    REQUIRE_EQ(preferred_runtime->sender_buffer_status().packets, 0U);

    clock.now_microseconds = 430'001;
    constexpr char promoted[] = "promoted path";
    REQUIRE_EQ(srt_send(group, promoted,
                   static_cast<int>(sizeof(promoted))),
        static_cast<int>(sizeof(promoted)));
    REQUIRE_EQ(original_runtime->sender_buffer_status().packets, 0U);
    REQUIRE_EQ(preferred_runtime->sender_buffer_status().packets, 1U);
    REQUIRE_EQ(srt_close(group), 0);
}

TEST(compat_backup_group_receives_from_the_running_path)
{
    const SRTSOCKET group = srt_create_group(SRT_GTYPE_BACKUP);
    const SRTSOCKET active = srt_create_socket();
    const SRTSOCKET standby = srt_create_socket();
    const auto group_record = GroupRegistry::instance().find(group);
    REQUIRE(group_record != nullptr);
    std::uint32_t initial_sequence = 0;
    {
        std::lock_guard lock(group_record->mutex);
        initial_sequence = group_record->initial_sequence;
    }
    const auto active_runtime = attach_group_runtime(
        group, active, initial_sequence, 10);
    (void)attach_group_runtime(
        group, standby, initial_sequence, 5);

    constexpr std::array<std::byte, 6> payload{
        std::byte{'a'}, std::byte{'c'}, std::byte{'t'},
        std::byte{'i'}, std::byte{'v'}, std::byte{'e'}};
    robotweax::srt::PacketView packet;
    packet.kind = robotweax::srt::PacketKind::data;
    packet.data.sequence = SequenceNumber{initial_sequence};
    packet.data.message_number = 1;
    packet.data.boundary = robotweax::srt::MessageBoundary::solo;
    packet.data.in_order = true;
    packet.payload = payload;
    active_runtime->process_packet(
        packet, IpEndpoint::loopback(9'000));

    std::array<char, 32> received{};
    std::array<SRT_SOCKGROUPDATA, 2> data{};
    SRT_MSGCTRL control = srt_msgctrl_default;
    control.grpdata = data.data();
    control.grpdata_size = data.size();
    REQUIRE_EQ(srt_recvmsg2(group, received.data(),
                   static_cast<int>(received.size()), &control),
        static_cast<int>(payload.size()));
    REQUIRE(std::equal(payload.begin(), payload.end(),
        reinterpret_cast<const std::byte*>(received.data())));
    const auto active_data = std::find_if(
        data.begin(), data.end(), [active](const auto& member) {
            return member.id == active;
        });
    const auto standby_data = std::find_if(
        data.begin(), data.end(), [standby](const auto& member) {
            return member.id == standby;
        });
    REQUIRE(active_data != data.end());
    REQUIRE(standby_data != data.end());
    REQUIRE_EQ(active_data->memberstate, SRT_GST_RUNNING);
    REQUIRE_EQ(active_data->result,
        static_cast<int>(payload.size()));
    REQUIRE_EQ(standby_data->memberstate, SRT_GST_IDLE);
    REQUIRE_EQ(standby_data->result, SRT_SUCCESS);
    REQUIRE_EQ(srt_close(group), 0);
}

TEST(compat_group_config_accepts_supported_member_options)
{
    std::vector<SRT_SOCKOPT> allowed = {
        SRTO_BINDTODEVICE,
        SRTO_CONNTIMEO,
        SRTO_DRIFTTRACER,
        SRTO_IPTOS,
        SRTO_IPTTL,
        SRTO_KMREFRESHRATE,
        SRTO_KMPREANNOUNCE,
        SRTO_LOSSMAXTTL,
        SRTO_NAKREPORT,
        SRTO_PACKETFILTER,
        SRTO_PAYLOADSIZE,
        SRTO_PEERIDLETIMEO,
        SRTO_RCVBUF,
        SRTO_SNDBUF,
        SRTO_SNDDROPDELAY,
        SRTO_UDP_RCVBUF,
        SRTO_UDP_SNDBUF,
    };
#ifdef ENABLE_AEAD_API_PREVIEW
    allowed.push_back(SRTO_CRYPTOMODE);
#endif
    auto* config = srt_create_config();
    REQUIRE(config != nullptr);
    for (std::size_t index = 0; index < allowed.size(); ++index) {
        const std::int32_t value = static_cast<std::int32_t>(index + 100);
        REQUIRE_EQ(srt_config_add(config, allowed[index], &value,
                       static_cast<int>(sizeof(value))),
            0);
    }
    // Stability currently belongs to the Backup coordinator. Reject an
    // unsupported link override here, before it can fail a later connect.
    const std::int32_t stability = 175;
    REQUIRE_EQ(srt_config_add(config, SRTO_GROUPMINSTABLETIMEO, &stability,
                   static_cast<int>(sizeof(stability))),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);
    std::int32_t rejected = 1;
    REQUIRE_EQ(srt_config_add(config, SRTO_RENDEZVOUS, &rejected,
                   static_cast<int>(sizeof(rejected))),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);
    REQUIRE_EQ(srt_config_add(config, SRTO_SNDBUF, nullptr,
                   static_cast<int>(sizeof(rejected))),
        SRT_ERROR);

    const auto snapshot = config->storage.snapshot();
    REQUIRE_EQ(snapshot.size, allowed.size());
    for (std::size_t index = 0; index < snapshot.size; ++index) {
        REQUIRE_EQ(snapshot.options[index].option, allowed[index]);
        std::int32_t copied = 0;
        std::memcpy(&copied, snapshot.options[index].bytes.data(),
            sizeof(copied));
        REQUIRE_EQ(copied, static_cast<std::int32_t>(index + 100));
    }
    srt_delete_config(config);
    srt_delete_config(nullptr);
}

TEST(compat_group_config_adds_endpoint_local_security_options)
{
    auto* config = srt_create_config();
    REQUIRE(config != nullptr);
    constexpr char passphrase[] = "0123456789abcdef";
    const std::int32_t key_length = 32;
    const std::int32_t refresh_rate = 64;
    const std::int32_t preannouncement = 20;
    const bool enforced = true;
    REQUIRE_EQ(srt_config_add(config, SRTO_PASSPHRASE, passphrase,
                   static_cast<int>(sizeof(passphrase) - 1U)),
        0);
    REQUIRE_EQ(srt_config_add(config, SRTO_PBKEYLEN, &key_length,
                   static_cast<int>(sizeof(key_length))),
        0);
    REQUIRE_EQ(srt_config_add(config, SRTO_KMREFRESHRATE, &refresh_rate,
                   static_cast<int>(sizeof(refresh_rate))),
        0);
    REQUIRE_EQ(srt_config_add(config, SRTO_KMPREANNOUNCE, &preannouncement,
                   static_cast<int>(sizeof(preannouncement))),
        0);
    REQUIRE_EQ(srt_config_add(config, SRTO_ENFORCEDENCRYPTION, &enforced,
                   static_cast<int>(sizeof(enforced))),
        0);

    const auto snapshot = config->storage.snapshot();
    REQUIRE_EQ(snapshot.size, 5U);
    REQUIRE_EQ(snapshot.options[0].option, SRTO_PASSPHRASE);
    REQUIRE_EQ(snapshot.options[0].size, sizeof(passphrase) - 1U);
    REQUIRE(std::equal(passphrase, passphrase + sizeof(passphrase) - 1U,
        reinterpret_cast<const char*>(snapshot.options[0].bytes.data())));
    REQUIRE_EQ(snapshot.options[1].option, SRTO_PBKEYLEN);
    REQUIRE_EQ(snapshot.options[4].option, SRTO_ENFORCEDENCRYPTION);
    srt_delete_config(config);
}

#ifdef ENABLE_AEAD_API_PREVIEW
TEST(compat_group_config_adds_endpoint_local_crypto_mode)
{
    auto* config = srt_create_config();
    REQUIRE(config != nullptr);
    const std::int32_t gcm = 2;
    REQUIRE_EQ(srt_config_add(config, SRTO_CRYPTOMODE, &gcm,
                   static_cast<int>(sizeof(gcm))),
        0);

    const auto snapshot = config->storage.snapshot();
    REQUIRE_EQ(snapshot.size, 1U);
    REQUIRE_EQ(snapshot.options[0].option, SRTO_CRYPTOMODE);
    std::int32_t copied = 0;
    std::memcpy(&copied, snapshot.options[0].bytes.data(), sizeof(copied));
    REQUIRE_EQ(copied, gcm);
    srt_delete_config(config);
}
#endif

TEST(compat_group_config_is_bounded_and_serializes_parallel_additions)
{
    auto* config = srt_create_config();
    REQUIRE(config != nullptr);
    constexpr std::size_t thread_count = 8;
    std::array<std::thread, thread_count> threads;
    std::array<int, thread_count> results{};
    for (std::size_t index = 0; index < thread_count; ++index) {
        threads[index] = std::thread([&, index] {
            const std::int32_t value = static_cast<std::int32_t>(index);
            results[index] = srt_config_add(config, SRTO_SNDBUF, &value,
                static_cast<int>(sizeof(value)));
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    REQUIRE(std::all_of(results.begin(), results.end(), [](int result) {
        return result == 0;
    }));
    REQUIRE_EQ(config->storage.snapshot().size, thread_count);

    for (std::size_t index = thread_count;
         index < robotweax::srt::compat::maximum_group_config_options;
         ++index) {
        const std::int32_t value = static_cast<std::int32_t>(index);
        REQUIRE_EQ(srt_config_add(config, SRTO_SNDBUF, &value,
                       static_cast<int>(sizeof(value))),
            0);
    }
    const std::int32_t overflow = 33;
    REQUIRE_EQ(srt_config_add(config, SRTO_SNDBUF, &overflow,
                   static_cast<int>(sizeof(overflow))),
        SRT_ERROR);

    std::array<std::byte,
        robotweax::srt::compat::maximum_group_option_bytes + 1> oversized{};
    REQUIRE_EQ(srt_config_add(config, SRTO_BINDTODEVICE,
                   oversized.data(), static_cast<int>(oversized.size())),
        SRT_ERROR);
    srt_delete_config(config);
}

TEST(compat_prepare_endpoint_handles_ipv4_ipv6_and_rejects_unsafe_inputs)
{
    const auto destination4 = ipv4_address(9'001);
    auto endpoint = srt_prepare_endpoint(nullptr,
        reinterpret_cast<const sockaddr*>(&destination4),
        static_cast<int>(sizeof(destination4)));
    REQUIRE_EQ(endpoint.id, SRT_INVALID_SOCK);
    REQUIRE_EQ(endpoint.errorcode, SRT_SUCCESS);
    REQUIRE_EQ(endpoint.token, -1);
    REQUIRE_EQ(endpoint.srcaddr.ss_family, AF_INET);
    REQUIRE_EQ(endpoint.peeraddr.ss_family, AF_INET);
    const auto* copied4 =
        reinterpret_cast<const sockaddr_in*>(&endpoint.peeraddr);
    REQUIRE_EQ(copied4->sin_port, destination4.sin_port);

    const auto source6 = ipv6_address(0);
    const auto destination6 = ipv6_address(9'002);
    endpoint = srt_prepare_endpoint(
        reinterpret_cast<const sockaddr*>(&source6),
        reinterpret_cast<const sockaddr*>(&destination6),
        static_cast<int>(sizeof(destination6)));
    REQUIRE_EQ(endpoint.errorcode, SRT_SUCCESS);
    REQUIRE_EQ(endpoint.srcaddr.ss_family, AF_INET6);
    REQUIRE_EQ(endpoint.peeraddr.ss_family, AF_INET6);

    endpoint = srt_prepare_endpoint(nullptr, nullptr, 0);
    REQUIRE_EQ(endpoint.errorcode, SRT_EINVPARAM);
    endpoint = srt_prepare_endpoint(
        reinterpret_cast<const sockaddr*>(&source6),
        reinterpret_cast<const sockaddr*>(&destination4),
        static_cast<int>(sizeof(source6)));
    REQUIRE_EQ(endpoint.errorcode, SRT_EINVPARAM);
    endpoint = srt_prepare_endpoint(nullptr,
        reinterpret_cast<const sockaddr*>(&destination6),
        static_cast<int>(sizeof(sockaddr_in)));
    REQUIRE_EQ(endpoint.errorcode, SRT_EINVPARAM);
}

TEST(compat_group_member_options_round_trip_and_reach_live_members)
{
    struct Cleanup {
        SRTSOCKET listener = SRT_INVALID_SOCK;
        SRTSOCKET group = SRT_INVALID_SOCK;
        SRTSOCKET mirror = SRT_INVALID_SOCK;
        SRT_SOCKOPT_CONFIG* config = nullptr;
        ~Cleanup()
        {
            if (config != nullptr)
                srt_delete_config(config);
            if (group != SRT_INVALID_SOCK)
                (void)srt_close(group);
            if (mirror != SRT_INVALID_SOCK)
                (void)srt_close(mirror);
            if (listener != SRT_INVALID_SOCK)
                (void)srt_close(listener);
        }
    };
    struct CallbackState {
        std::mutex mutex;
        int matched = 0;
    } callback_state;
    Cleanup cleanup;
    constexpr char stream[] = "#!::r=group/options";
    cleanup.listener = srt_create_socket();
    cleanup.group = srt_create_group(SRT_GTYPE_BROADCAST);
    REQUIRE(cleanup.listener != SRT_INVALID_SOCK);
    REQUIRE(cleanup.group != SRT_INVALID_SOCK);
    const int enabled = 1;
    REQUIRE_EQ(srt_setsockflag(cleanup.listener, SRTO_GROUPCONNECT, &enabled,
                   sizeof(enabled)),
        0);
    const int timeout = 2'000;
    REQUIRE_EQ(srt_setsockflag(
                   cleanup.listener, SRTO_RCVTIMEO, &timeout, sizeof(timeout)),
        0);
    constexpr char listener_filter[] = "fec";
    REQUIRE_EQ(srt_setsockflag(cleanup.listener, SRTO_PACKETFILTER,
                   listener_filter, sizeof(listener_filter) - 1),
        0);
    REQUIRE_EQ(
        srt_listen_callback(
            cleanup.listener,
            [](void* context, SRTSOCKET, int, const sockaddr*, const char* id) {
                auto& state = *static_cast<CallbackState*>(context);
                std::lock_guard lock(state.mutex);
                if (id == nullptr
                    || std::strcmp(id, "#!::r=group/options") != 0)
                    return -1;
                ++state.matched;
                return 0;
            },
            &callback_state),
        0);
    auto address = ipv4_address(0);
    REQUIRE_EQ(
        srt_bind(cleanup.listener, reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)),
        0);
    REQUIRE_EQ(srt_listen(cleanup.listener, 4), 0);
    int address_size = sizeof(address);
    REQUIRE_EQ(srt_getsockname(cleanup.listener,
                   reinterpret_cast<sockaddr*>(&address), &address_size),
        0);
    const std::int32_t symmetric = 300;
    REQUIRE_EQ(srt_setsockflag(
                   cleanup.group, SRTO_LATENCY, &symmetric, sizeof(symmetric)),
        0);
    const std::int32_t receive = 800;
    const std::int32_t peer = 450;
    std::int64_t bandwidth = 12'500'000;
    REQUIRE_EQ(srt_setsockflag(
                   cleanup.group, SRTO_RCVLATENCY, &receive, sizeof(receive)),
        0);
    REQUIRE_EQ(
        srt_setsockflag(cleanup.group, SRTO_PEERLATENCY, &peer, sizeof(peer)),
        0);
    REQUIRE_EQ(srt_setsockflag(
                   cleanup.group, SRTO_MAXBW, &bandwidth, sizeof(bandwidth)),
        0);
    REQUIRE_EQ(srt_setsockflag(
                   cleanup.group, SRTO_STREAMID, stream, sizeof(stream) - 1),
        0);
    constexpr char group_filter[] = "fec,cols:3,rows:1";
    constexpr char override_filter[] = "fec,cols:4,rows:1";
    const std::int32_t group_payload = 1'000;
    const std::int32_t override_payload = 900;
    const std::int32_t group_connect_timeout = 2'500;
    const std::int32_t override_connect_timeout = 4'000;
    REQUIRE_EQ(srt_setsockflag(cleanup.group, SRTO_PACKETFILTER, group_filter,
                   sizeof(group_filter) - 1),
        0);
    REQUIRE_EQ(srt_setsockflag(cleanup.group, SRTO_PAYLOADSIZE, &group_payload,
                   sizeof(group_payload)),
        0);
    REQUIRE_EQ(srt_setsockflag(cleanup.group, SRTO_CONNTIMEO,
                   &group_connect_timeout, sizeof(group_connect_timeout)),
        0);
    const std::int64_t input_bandwidth = 4'000'000;
    REQUIRE_EQ(srt_setsockflag(cleanup.group, SRTO_INPUTBW, &input_bandwidth,
                   sizeof(input_bandwidth)),
        0);
    const std::array inherited {
        std::pair {SRTO_OHEADBW, 40},
        std::pair {SRTO_LOSSMAXTTL, 7},
        std::pair {SRTO_FC, 1'024},
        std::pair {SRTO_UDP_SNDBUF, 131'072},
        std::pair {SRTO_UDP_RCVBUF, 262'144},
    };
    for (const auto [option, configured] : inherited) {
        REQUIRE_EQ(srt_setsockflag(
                       cleanup.group, option, &configured, sizeof(configured)),
            0);
    }
    const auto check = [&](SRTSOCKET socket, std::int32_t expected_payload,
                           std::int32_t expected_timeout,
                           int expected_columns) {
        for (const auto option :
            {SRTO_LATENCY, SRTO_RCVLATENCY, SRTO_PEERLATENCY}) {
            std::int32_t actual = 0;
            int size = sizeof(actual);
            REQUIRE_EQ(srt_getsockflag(socket, option, &actual, &size), 0);
            REQUIRE_EQ(actual, option == SRTO_PEERLATENCY ? peer : receive);
            REQUIRE_EQ(size, sizeof(actual));
        }
        for (const auto [option, expected] : inherited) {
            std::int32_t actual = 0;
            int size = sizeof(actual);
            REQUIRE_EQ(srt_getsockflag(socket, option, &actual, &size), 0);
            REQUIRE_EQ(actual, expected);
        }
        std::int64_t actual = 0;
        int size = sizeof(actual);
        REQUIRE_EQ(srt_getsockflag(socket, SRTO_INPUTBW, &actual, &size), 0);
        REQUIRE_EQ(actual, input_bandwidth);
        size = sizeof(actual);
        REQUIRE_EQ(srt_getsockflag(socket, SRTO_MAXBW, &actual, &size), 0);
        REQUIRE_EQ(actual, bandwidth);
        REQUIRE_EQ(size, sizeof(actual));
        std::array<char, sizeof(stream)> text {};
        size = text.size();
        REQUIRE_EQ(
            srt_getsockflag(socket, SRTO_STREAMID, text.data(), &size), 0);
        REQUIRE_EQ(size, sizeof(stream) - 1);
        REQUIRE_EQ(std::memcmp(text.data(), stream, sizeof(stream)), 0);
        for (const auto [option, expected] :
            {std::pair {SRTO_PAYLOADSIZE, expected_payload},
                std::pair {SRTO_CONNTIMEO, expected_timeout}}) {
            std::int32_t integer = -1;
            size = sizeof(integer);
            REQUIRE_EQ(srt_getsockflag(socket, option, &integer, &size), 0);
            REQUIRE_EQ(integer, expected);
            REQUIRE_EQ(size, sizeof(integer));
        }
        std::array<char, 513> filter {};
        size = filter.size();
        REQUIRE_EQ(
            srt_getsockflag(socket, SRTO_PACKETFILTER, filter.data(), &size),
            0);
        REQUIRE_EQ(size, static_cast<int>(std::strlen(filter.data())));
        REQUIRE(std::strstr(
                    filter.data(), expected_columns == 3 ? "cols:3" : "cols:4")
            != nullptr);
    };
    check(cleanup.group, group_payload, group_connect_timeout, 3);
    cleanup.config = srt_create_config();
    REQUIRE(cleanup.config != nullptr);
    REQUIRE_EQ(srt_config_add(cleanup.config, SRTO_PACKETFILTER,
                   override_filter, sizeof(override_filter) - 1),
        0);
    REQUIRE_EQ(srt_config_add(cleanup.config, SRTO_PAYLOADSIZE,
                   &override_payload, sizeof(override_payload)),
        0);
    REQUIRE_EQ(srt_config_add(cleanup.config, SRTO_CONNTIMEO,
                   &override_connect_timeout, sizeof(override_connect_timeout)),
        0);
    for (int index = 0; index < 2; ++index) {
        auto endpoint = srt_prepare_endpoint(nullptr,
            reinterpret_cast<const sockaddr*>(&address), sizeof(address));
        if (index == 1) {
            endpoint.config = cleanup.config;
        }
        REQUIRE(srt_connect_group(cleanup.group, &endpoint, 1) != SRT_ERROR);
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds {2};
        while (srt_getsockstate(endpoint.id) == SRTS_CONNECTING
            && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds {1});
        }
        REQUIRE_EQ(srt_getsockstate(endpoint.id), SRTS_CONNECTED);
        check(endpoint.id, index == 0 ? group_payload : override_payload,
            index == 0 ? group_connect_timeout : override_connect_timeout,
            index == 0 ? 3 : 4);
        if (index == 0) {
            bandwidth = 10'000'000;
            REQUIRE_EQ(srt_setsockflag(cleanup.group, SRTO_MAXBW, &bandwidth,
                           sizeof(bandwidth)),
                0);
        }
        const auto member = SocketRegistry::instance().find(endpoint.id);
        REQUIRE(member != nullptr);
        std::lock_guard lock(member->mutex);
        using robotweax::srt::SocketOption;
        REQUIRE_EQ(member->native_options
                       .get(SocketOption::receiver_latency_milliseconds)
                       .value,
            receive);
        REQUIRE_EQ(
            member->native_options.get(SocketOption::peer_latency_milliseconds)
                .value,
            peer);
        REQUIRE_EQ(member->native_options
                       .get(SocketOption::maximum_bandwidth_bytes_per_second)
                       .value,
            bandwidth);
        REQUIRE_EQ(member->native_options.packet_filter_configuration().columns,
            index == 0 ? 3U : 4U);
    }
    cleanup.mirror = srt_accept(cleanup.listener, nullptr, nullptr);
    REQUIRE(cleanup.mirror != SRT_INVALID_SOCK);
    check(cleanup.group, group_payload, group_connect_timeout, 3);
    const bool synchronous = true;
    REQUIRE_EQ(srt_setsockflag(cleanup.group, SRTO_SNDSYN, &synchronous,
                   sizeof(synchronous)),
        0);
    const auto caller_record = GroupRegistry::instance().find(cleanup.group);
    SRTSOCKET caller_member = SRT_INVALID_SOCK;
    {
        std::lock_guard caller_lock(caller_record->mutex);
        caller_member = caller_record->members.front().public_data.id;
    }
    bool member_synchronous = true;
    int member_option_size = sizeof(member_synchronous);
    REQUIRE_EQ(srt_getsockflag(caller_member, SRTO_SNDSYN, &member_synchronous,
                   &member_option_size),
        0);
    REQUIRE(!member_synchronous);
    const auto mirror_record = GroupRegistry::instance().find(cleanup.mirror);
    REQUIRE(mirror_record != nullptr);
    SRTSOCKET accepted_member = SRT_INVALID_SOCK;
    {
        std::lock_guard mirror_lock(mirror_record->mutex);
        REQUIRE(!mirror_record->members.empty());
        accepted_member = mirror_record->members.front().public_data.id;
    }
    for (const auto option :
        {SRTO_STREAMID, SRTO_RCVLATENCY, SRTO_PEERLATENCY, SRTO_PEERIDLETIMEO,
            SRTO_CONNTIMEO, SRTO_PACKETFILTER, SRTO_PAYLOADSIZE}) {
        std::array<std::byte, 1'024> effective {}, group_value {};
        int effective_size = effective.size();
        int group_size = group_value.size();
        REQUIRE_EQ(srt_getsockflag(accepted_member, option, effective.data(),
                       &effective_size),
            0);
        REQUIRE_EQ(srt_getsockflag(
                       cleanup.mirror, option, group_value.data(), &group_size),
            0);
        REQUIRE_EQ(group_size, effective_size);
        REQUIRE_EQ(group_value, effective);
    }
    std::lock_guard lock(callback_state.mutex);
    REQUIRE_EQ(callback_state.matched, 2);
}

TEST(compat_group_filter_payload_and_timeout_validate_values_and_stage)
{
    const SRTSOCKET group = srt_create_group(SRT_GTYPE_BROADCAST);
    REQUIRE(group != SRT_INVALID_SOCK);
    const auto integer_option = [group](SRT_SOCKOPT option) {
        std::int32_t value = -1;
        int size = sizeof(value);
        REQUIRE_EQ(srt_getsockflag(group, option, &value, &size), 0);
        REQUIRE_EQ(size, sizeof(value));
        return value;
    };
    REQUIRE_EQ(integer_option(SRTO_CONNTIMEO), 3'000);
    REQUIRE_EQ(integer_option(SRTO_PAYLOADSIZE), SRT_LIVE_DEF_PLSIZE);
    char filter[64] {};
    int filter_size = sizeof(filter);
    REQUIRE_EQ(
        srt_getsockflag(group, SRTO_PACKETFILTER, filter, &filter_size), 0);
    REQUIRE_EQ(filter_size, 0);
    REQUIRE_EQ(filter[0], '\0');

    constexpr char valid_filter[] = "fec,cols:3,rows:1";
    constexpr char invalid_filter[] = "unknown";
    REQUIRE_EQ(srt_setsockflag(group, SRTO_PACKETFILTER, invalid_filter,
                   sizeof(invalid_filter) - 1),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);
    REQUIRE_EQ(srt_setsockflag(group, SRTO_PACKETFILTER, valid_filter,
                   sizeof(valid_filter) - 1),
        0);
    const std::int32_t too_large = SRT_LIVE_MAX_PLSIZE;
    REQUIRE_EQ(
        srt_setsockflag(group, SRTO_PAYLOADSIZE, &too_large, sizeof(too_large)),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);
    REQUIRE_EQ(integer_option(SRTO_PAYLOADSIZE), SRT_LIVE_DEF_PLSIZE);
    const std::int32_t payload = 1'000;
    REQUIRE_EQ(
        srt_setsockflag(group, SRTO_PAYLOADSIZE, &payload, sizeof(payload)), 0);
    const std::int32_t default_payload = 0;
    REQUIRE_EQ(srt_setsockflag(group, SRTO_PAYLOADSIZE, &default_payload,
                   sizeof(default_payload)),
        0);
    REQUIRE_EQ(integer_option(SRTO_PAYLOADSIZE), SRT_LIVE_DEF_PLSIZE);
    REQUIRE_EQ(
        srt_setsockflag(group, SRTO_PAYLOADSIZE, &payload, sizeof(payload)), 0);
    const std::int32_t invalid_timeout = -1;
    REQUIRE_EQ(srt_setsockflag(group, SRTO_CONNTIMEO, &invalid_timeout,
                   sizeof(invalid_timeout)),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);
    const std::int32_t timeout = 4'000;
    REQUIRE_EQ(
        srt_setsockflag(group, SRTO_CONNTIMEO, &timeout, sizeof(timeout)), 0);
    REQUIRE_EQ(integer_option(SRTO_PAYLOADSIZE), payload);
    REQUIRE_EQ(integer_option(SRTO_CONNTIMEO), timeout);
    filter_size = sizeof(valid_filter) - 1;
    REQUIRE_EQ(srt_getsockflag(group, SRTO_PACKETFILTER, filter, &filter_size),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);
    filter_size = sizeof(filter);
    REQUIRE_EQ(
        srt_getsockflag(group, SRTO_PACKETFILTER, filter, &filter_size), 0);
    REQUIRE_EQ(filter_size, sizeof(valid_filter) - 1);
    REQUIRE_EQ(std::strcmp(filter, valid_filter), 0);

    GroupRegistry::ConnectDescription description;
    REQUIRE(GroupRegistry::instance().describe_connect(group, description));
    GroupRegistry::instance().mark_opened(group, description.generation);
    REQUIRE_EQ(srt_setsockflag(group, SRTO_PACKETFILTER, valid_filter,
                   sizeof(valid_filter) - 1),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ECONNSOCK);
    REQUIRE_EQ(
        srt_setsockflag(group, SRTO_PAYLOADSIZE, &payload, sizeof(payload)),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ECONNSOCK);
    REQUIRE_EQ(
        srt_setsockflag(group, SRTO_CONNTIMEO, &timeout, sizeof(timeout)),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ECONNSOCK);
    REQUIRE_EQ(srt_close(group), 0);
}

TEST(compat_group_member_option_getters_validate_buffer_sizes)
{
    const auto group = srt_create_group(SRT_GTYPE_BACKUP);
    REQUIRE(group != SRT_INVALID_SOCK);
    struct Cleanup {
        SRTSOCKET group;
        ~Cleanup()
        {
            (void)srt_close(group);
        }
    } cleanup {group};
    std::array<char, 513> text {};
    text.fill('x');
    REQUIRE_EQ(srt_setsockflag(group, SRTO_STREAMID, text.data(), 512), 0);
    REQUIRE_EQ(
        srt_setsockflag(group, SRTO_STREAMID, text.data(), 513), SRT_ERROR);
    int size = 512;
    REQUIRE_EQ(
        srt_getsockflag(group, SRTO_STREAMID, text.data(), &size), SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);
    size = 513;
    REQUIRE_EQ(srt_getsockflag(group, SRTO_STREAMID, text.data(), &size), 0);
    REQUIRE_EQ(size, 512);
    REQUIRE_EQ(text[511], 'x');
    REQUIRE_EQ(text[512], '\0');
    REQUIRE_EQ(srt_setsockflag(group, SRTO_STREAMID, "", 0), 0);
    size = text.size();
    REQUIRE_EQ(srt_getsockflag(group, SRTO_STREAMID, text.data(), &size), 0);
    REQUIRE_EQ(size, 0);
    REQUIRE_EQ(text[0], '\0');
    for (const auto option :
        {SRTO_LATENCY, SRTO_RCVLATENCY, SRTO_PEERLATENCY, SRTO_MAXBW}) {
        std::int64_t value = 0;
        size = 1;
        REQUIRE_EQ(srt_getsockflag(group, option, &value, &size), SRT_ERROR);
        REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);
    }
}

TEST(compat_group_epoll_rearms_out_after_member_fill_and_ack_between_waits)
{
    for (const auto type : {SRT_GTYPE_BROADCAST, SRT_GTYPE_BACKUP}) {
        const SRTSOCKET group = srt_create_group(type);
        const SRTSOCKET member = srt_create_socket();
        const auto record = GroupRegistry::instance().find(group);
        REQUIRE(record != nullptr);
        TestClock clock {
            .channel =
                std::make_shared<robotweax::srt::compat::DatagramChannel>()};
        clock.channel->set_send_hook_for_testing(accept_test_datagram, nullptr);
        const auto runtime =
            attach_group_runtime(group, member, record->initial_sequence, 1,
                &clock, 1, false, 0, ConnectionRuntime::Clock::now(), record);
        const int first = srt_epoll_create();
        const int second = srt_epoll_create();
        struct Cleanup {
            SRTSOCKET group;
            int first, second;
            ~Cleanup()
            {
                (void)srt_epoll_release(first);
                (void)srt_epoll_release(second);
                (void)srt_close(group);
            }
        } cleanup {group, first, second};
        const int watched = SRT_EPOLL_OUT | SRT_EPOLL_ET;
        for (const auto eid : {first, second}) {
            REQUIRE_EQ(srt_epoll_add_usock(eid, group, &watched), 0);
            SRT_EPOLL_EVENT event {};
            REQUIRE_EQ(srt_epoll_uwait(eid, &event, 1, 0), 1);
        }
        const std::array payload {std::byte {'p'}};
        for (std::uint32_t iteration = 0; iteration < 3; ++iteration) {
            REQUIRE_EQ(
                runtime
                    ->queue_group_message(payload,
                        SequenceNumber {record->initial_sequence}.advanced(
                            iteration),
                        iteration + 1U, 0, true, -1)
                    .status,
                robotweax::srt::compat::MessageIoStatus::success);
            clock.now_microseconds += 1'000;
            (void)runtime->poll();
            deliver_lite_ack(runtime,
                SequenceNumber {record->initial_sequence}.advanced(
                    iteration + 1U));
            REQUIRE_EQ(runtime->sender_buffer_status().packets, 0U);
            for (const auto eid : {first, second}) {
                SRT_EPOLL_EVENT event {};
                REQUIRE_EQ(srt_epoll_uwait(eid, &event, 1, 0), 1);
                REQUIRE_EQ(event.events, SRT_EPOLL_OUT);
                REQUIRE_EQ(srt_epoll_uwait(eid, &event, 1, 0), 0);
            }
        }
    }
}

TEST(compat_group_epoll_rearms_err_after_replacement_between_waits)
{
    for (const auto type : {SRT_GTYPE_BROADCAST, SRT_GTYPE_BACKUP}) {
        const SRTSOCKET group = srt_create_group(type);
        const auto record = GroupRegistry::instance().find(group);
        REQUIRE(record != nullptr);
        const int eid = srt_epoll_create();
        struct Cleanup {
            SRTSOCKET group;
            int eid;
            ~Cleanup()
            {
                (void)srt_epoll_release(eid);
                (void)srt_close(group);
            }
        } cleanup {group, eid};
        const int watched = SRT_EPOLL_ERR | SRT_EPOLL_ET;
        REQUIRE_EQ(srt_epoll_add_usock(eid, group, &watched), 0);
        SRT_EPOLL_EVENT event {};
        REQUIRE_EQ(srt_epoll_uwait(eid, &event, 1, 0), 0);
        for (int iteration = 0; iteration < 3; ++iteration) {
            const SRTSOCKET member = srt_create_socket();
            REQUIRE(member != SRT_INVALID_SOCK);
            const auto runtime =
                attach_group_runtime(group, member, record->initial_sequence);
            // Do not sample the intervening healthy level: its low epoch must
            // survive a complete replacement/break cycle between waits.
            runtime->mark_broken(9);
            REQUIRE_EQ(srt_epoll_uwait(eid, &event, 1, 0), 1);
            REQUIRE_EQ(event.events, SRT_EPOLL_ERR);
            REQUIRE_EQ(srt_epoll_uwait(eid, &event, 1, 0), 0);
            REQUIRE_EQ(srt_close(member), 0);
        }
    }
}

TEST(compat_group_epoll_rearms_in_after_drain_and_refill)
{
    for (const auto type : {SRT_GTYPE_BACKUP, SRT_GTYPE_BROADCAST}) {
        const SRTSOCKET group = srt_create_group(type);
        const SRTSOCKET member = srt_create_socket();
        REQUIRE(group != SRT_INVALID_SOCK);
        REQUIRE(member != SRT_INVALID_SOCK);
        const auto record = GroupRegistry::instance().find(group);
        REQUIRE(record != nullptr);
        const SequenceNumber initial {record->initial_sequence};
        TestClock clock {
            .channel =
                std::make_shared<robotweax::srt::compat::DatagramChannel>()};
        clock.channel->set_send_hook_for_testing(accept_test_datagram, nullptr);
        const auto runtime =
            attach_group_runtime(group, member, initial.value(), 1, &clock, 0,
                true, 0, ConnectionRuntime::Clock::now(), record);
        const bool asynchronous = false;
        REQUIRE_EQ(srt_setsockflag(group, SRTO_RCVSYN, &asynchronous,
                       static_cast<int>(sizeof(asynchronous))),
            0);

        const int eid = srt_epoll_create();
        REQUIRE(eid >= 0);
        const int watched = SRT_EPOLL_IN | SRT_EPOLL_ET;
        REQUIRE_EQ(srt_epoll_add_usock(eid, group, &watched), 0);
        SRT_EPOLL_EVENT event {};
        REQUIRE_EQ(srt_epoll_uwait(eid, &event, 1, 0), 0);
        const auto inject = [&](SequenceNumber sequence,
                                std::uint32_t message_number) {
            const std::array<std::byte, 1> payload {std::byte {'x'}};
            robotweax::srt::PacketView packet;
            packet.kind = robotweax::srt::PacketKind::data;
            packet.data.sequence = sequence;
            packet.data.message_number = message_number;
            packet.data.boundary = robotweax::srt::MessageBoundary::solo;
            packet.data.in_order = true;
            packet.data.timestamp = robotweax::srt::PacketTimestamp {0};
            packet.payload = payload;
            runtime->process_packet(packet, IpEndpoint::loopback(9'000));
        };
        inject(initial, 1);
        REQUIRE_EQ(srt_epoll_uwait(eid, &event, 1, 0), 1);
        REQUIRE_EQ(event.events, SRT_EPOLL_IN);
        std::array<char, 8> buffer {};
        REQUIRE_EQ(srt_recvmsg(group, buffer.data(), buffer.size()), 1);
        inject(initial.next(), 2);
        REQUIRE_EQ(srt_epoll_uwait(eid, &event, 1, 0), 1);
        REQUIRE_EQ(event.events, SRT_EPOLL_IN);
        REQUIRE_EQ(srt_epoll_release(eid), 0);
        REQUIRE_EQ(srt_close(group), 0);
    }
}

TEST(compat_group_epoll_waits_for_logical_receive_prefix)
{
    for (const auto type : {SRT_GTYPE_BACKUP, SRT_GTYPE_BROADCAST}) {
        const SRTSOCKET group = srt_create_group(type);
        const SRTSOCKET first_socket = srt_create_socket();
        const SRTSOCKET later_socket = srt_create_socket();
        REQUIRE(group != SRT_INVALID_SOCK);
        REQUIRE(first_socket != SRT_INVALID_SOCK);
        REQUIRE(later_socket != SRT_INVALID_SOCK);
        const auto record = GroupRegistry::instance().find(group);
        REQUIRE(record != nullptr);
        const SequenceNumber first_sequence {record->initial_sequence};
        TestClock clock {
            .channel =
                std::make_shared<robotweax::srt::compat::DatagramChannel>()};
        clock.channel->set_send_hook_for_testing(accept_test_datagram, nullptr);
        const auto origin = ConnectionRuntime::Clock::now();
        const auto first = attach_group_runtime(group, first_socket,
            first_sequence.value(), 1, &clock, 0, false, 0, origin, record);
        const auto later = attach_group_runtime(group, later_socket,
            first_sequence.advanced(2).value(), 1, &clock, 0, false, 0, origin,
            record);
        const bool asynchronous = false;
        REQUIRE_EQ(srt_setsockflag(group, SRTO_RCVSYN, &asynchronous,
                       static_cast<int>(sizeof(asynchronous))),
            0);
        const int poll = srt_epoll_create();
        REQUIRE(poll >= 0);
        const int watched = SRT_EPOLL_IN;
        REQUIRE_EQ(srt_epoll_add_usock(poll, group, &watched), 0);
        SRT_EPOLL_EVENT event {};
        REQUIRE_EQ(srt_epoll_uwait(poll, &event, 1, 0), 0);

        const auto inject =
            [&](const std::shared_ptr<ConnectionRuntime>& runtime,
                SequenceNumber sequence, std::uint32_t message,
                robotweax::srt::MessageBoundary boundary, std::byte value) {
                const std::array<std::byte, 1> payload {value};
                robotweax::srt::PacketView packet;
                packet.kind = robotweax::srt::PacketKind::data;
                packet.data.sequence = sequence;
                packet.data.message_number = message;
                packet.data.boundary = boundary;
                packet.data.in_order = true;
                packet.payload = payload;
                runtime->process_packet(packet, IpEndpoint::loopback(9'000));
            };
        inject(first, first_sequence, 1, robotweax::srt::MessageBoundary::first,
            std::byte {'a'});
        inject(later, first_sequence.advanced(2), 2,
            robotweax::srt::MessageBoundary::solo, std::byte {'b'});
        std::array<char, 8> buffer {};
        REQUIRE_EQ(srt_epoll_uwait(poll, &event, 1, 0), 0);
        REQUIRE_EQ(srt_recvmsg(group, buffer.data(), buffer.size()), SRT_ERROR);
        REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EASYNCRCV);

        inject(first, first_sequence.next(), 1,
            robotweax::srt::MessageBoundary::last, std::byte {'A'});
        REQUIRE_EQ(srt_epoll_uwait(poll, &event, 1, 0), 1);
        REQUIRE_EQ(event.events, SRT_EPOLL_IN);
        REQUIRE_EQ(srt_recvmsg(group, buffer.data(), buffer.size()), 2);
        REQUIRE_EQ(buffer[0], 'a');
        REQUIRE_EQ(buffer[1], 'A');
        REQUIRE_EQ(srt_epoll_uwait(poll, &event, 1, 0), 1);
        REQUIRE_EQ(srt_recvmsg(group, buffer.data(), buffer.size()), 1);
        REQUIRE_EQ(buffer[0], 'b');
        REQUIRE_EQ(srt_epoll_uwait(poll, &event, 1, 0), 0);
        REQUIRE_EQ(srt_epoll_release(poll), 0);
        REQUIRE_EQ(srt_close(group), 0);
    }
}

TEST(compat_group_receive_skips_expired_gap_with_unused_standby)
{
    // Receiver TLPKTDROP on the only carrying member moves its receive
    // floor past the expected group sequence. The group must skip the gap
    // instead of waiting for a message that can no longer arrive.
    for (const auto type : {SRT_GTYPE_BACKUP, SRT_GTYPE_BROADCAST}) {
        const auto group = srt_create_group(type);
        const auto member = srt_create_socket();
        REQUIRE(group != SRT_INVALID_SOCK);
        REQUIRE(member != SRT_INVALID_SOCK);
        const auto record = GroupRegistry::instance().find(group);
        REQUIRE(record != nullptr);
        const auto sequence = record->initial_sequence;
        const auto origin = ConnectionRuntime::Clock::now();
        TestClock clock {.now_microseconds = 0,
            .channel =
                std::make_shared<robotweax::srt::compat::DatagramChannel>()};
        clock.channel->set_send_hook_for_testing(accept_test_datagram, nullptr);
        constexpr std::uint16_t latency_milliseconds = 300;
        const auto runtime = attach_group_runtime(group, member, sequence, 1,
            &clock, 0, true, latency_milliseconds, origin, record, {}, nullptr,
            nullptr, true);
        const auto standby = srt_create_socket();
        REQUIRE(standby != SRT_INVALID_SOCK);
        (void)attach_group_runtime(group, standby, sequence, 2, &clock, 0, true,
            latency_milliseconds, origin, record, {}, nullptr, nullptr, true);
        const auto inject = [&](std::uint32_t seq, std::uint32_t timestamp,
                                std::byte value) {
            const std::array<std::byte, 1> payload {value};
            robotweax::srt::PacketView packet;
            packet.kind = robotweax::srt::PacketKind::data;
            packet.data.sequence = SequenceNumber {seq};
            packet.data.message_number = seq - sequence + 1U;
            packet.data.boundary = robotweax::srt::MessageBoundary::solo;
            packet.data.in_order = true;
            packet.data.timestamp = robotweax::srt::PacketTimestamp {timestamp};
            packet.payload = payload;
            runtime->process_packet(packet, IpEndpoint::loopback(9'000));
        };
        const bool synchronous = false;
        REQUIRE_EQ(srt_setsockflag(group, SRTO_RCVSYN, &synchronous,
                       static_cast<int>(sizeof(synchronous))),
            0);

        // The first group message is lost; the two following ones arrive.
        const SequenceNumber second = SequenceNumber {sequence}.next();
        const SequenceNumber third = second.next();
        clock.now_microseconds = 10'000;
        inject(second.value(), 10'000U, std::byte {'b'});
        inject(third.value(), 11'000U, std::byte {'c'});
        const int poll = srt_epoll_create();
        REQUIRE(poll >= 0);
        const int watched = SRT_EPOLL_IN;
        REQUIRE_EQ(srt_epoll_add_usock(poll, group, &watched), 0);
        SRT_EPOLL_EVENT event {};
        REQUIRE_EQ(srt_epoll_uwait(poll, &event, 1, 0), 0);

        std::array<char, 8> buffer {};
        SRT_MSGCTRL control = srt_msgctrl_default;
        // Before the delivery deadline the gap can still be recovered.
        REQUIRE_EQ(srt_recvmsg2(group, buffer.data(), buffer.size(), &control),
            SRT_ERROR);
        REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EASYNCRCV);

        // Past the deadline the member drops the gap. The group must deliver
        // the surviving messages in order rather than block forever.
        clock.now_microseconds = 10'000U
            + static_cast<std::uint64_t>(latency_milliseconds) * 1'000U
            + 50'000U;
        REQUIRE_EQ(srt_epoll_update_usock(poll, group, &watched), 0);
        REQUIRE_EQ(srt_epoll_uwait(poll, &event, 1, 0), 1);
        REQUIRE_EQ(event.events, SRT_EPOLL_IN);
        REQUIRE_EQ(
            srt_recvmsg2(group, buffer.data(), buffer.size(), &control), 1);
        REQUIRE_EQ(buffer[0], 'b');
        REQUIRE_EQ(control.pktseq, static_cast<std::int32_t>(second.value()));
        REQUIRE_EQ(srt_epoll_uwait(poll, &event, 1, 0), 1);
        REQUIRE_EQ(
            srt_recvmsg2(group, buffer.data(), buffer.size(), &control), 1);
        REQUIRE_EQ(buffer[0], 'c');
        REQUIRE_EQ(control.pktseq, static_cast<std::int32_t>(third.value()));
        REQUIRE_EQ(srt_recvmsg2(group, buffer.data(), buffer.size(), &control),
            SRT_ERROR);
        REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EASYNCRCV);
        REQUIRE_EQ(srt_epoll_uwait(poll, &event, 1, 0), 0);

        // The group handle reports its own receive and drop counters.
        SRT_TRACEBSTATS statistics {};
        REQUIRE_EQ(srt_bstats(group, &statistics, 1), 0);
        REQUIRE_EQ(statistics.pktRecvUniqueTotal, 2);
        REQUIRE_EQ(statistics.pktRecvUnique, 2);
        REQUIRE_EQ(statistics.byteRecvUniqueTotal, 2U * (1U + 44U));
        REQUIRE_EQ(statistics.pktRcvDropTotal, 1);
        REQUIRE_EQ(statistics.pktRcvDrop, 1);
        REQUIRE_EQ(statistics.byteRcvDropTotal,
            static_cast<std::uint64_t>(SRT_LIVE_DEF_PLSIZE + 44));
        REQUIRE_EQ(statistics.pktSentUniqueTotal, 0);
        REQUIRE_EQ(statistics.pktSentTotal, 0);
        // The interval counters were cleared by the previous call.
        REQUIRE_EQ(srt_bstats(group, &statistics, 0), 0);
        REQUIRE_EQ(statistics.pktRecvUniqueTotal, 2);
        REQUIRE_EQ(statistics.pktRecvUnique, 0);
        REQUIRE_EQ(statistics.pktRcvDrop, 0);
        REQUIRE_EQ(srt_epoll_release(poll), 0);
        REQUIRE_EQ(srt_close(group), 0);
        REQUIRE_EQ(srt_bstats(group, &statistics, 0), SRT_ERROR);
        REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVSOCK);
    }
}

TEST(compat_group_receive_observes_terminal_without_state_getter)
{
    constexpr std::uint16_t receive_delay_milliseconds = 100;
    constexpr std::array<std::byte, 7> payload {std::byte {'g'},
        std::byte {'r'}, std::byte {'o'}, std::byte {'u'}, std::byte {'p'},
        std::byte {'!'}, std::byte {'!'}};
    constexpr std::array<SRT_GROUP_TYPE, 2> group_types {
        SRT_GTYPE_BROADCAST, SRT_GTYPE_BACKUP};
    for (const SRT_GROUP_TYPE group_type : group_types) {
        const SRTSOCKET group = srt_create_group(group_type);
        const SRTSOCKET member = srt_create_socket();
        REQUIRE(group != SRT_INVALID_SOCK);
        REQUIRE(member != SRT_INVALID_SOCK);
        const auto group_record = GroupRegistry::instance().find(group);
        REQUIRE(group_record != nullptr);
        std::uint32_t initial_sequence = 0;
        {
            std::lock_guard lock(group_record->mutex);
            initial_sequence = group_record->initial_sequence;
        }

        TestClock clock {
            .now_microseconds = 0,
            .channel =
                std::make_shared<robotweax::srt::compat::DatagramChannel>(),
        };
        clock.channel->set_send_hook_for_testing(accept_test_datagram, nullptr);
        const auto runtime = attach_group_runtime(group, member,
            initial_sequence, 1, &clock, 0U, true, receive_delay_milliseconds);

        const bool nonblocking = false;
        REQUIRE_EQ(srt_setsockflag(group, SRTO_RCVSYN, &nonblocking,
                       static_cast<int>(sizeof(nonblocking))),
            0);
        robotweax::srt::PacketView data;
        data.kind = robotweax::srt::PacketKind::data;
        data.data.sequence = SequenceNumber {initial_sequence};
        data.data.message_number = 1;
        data.data.boundary = robotweax::srt::MessageBoundary::solo;
        data.data.in_order = true;
        data.data.timestamp = robotweax::srt::PacketTimestamp {0};
        data.payload = payload;
        runtime->process_packet(data, IpEndpoint::loopback(9'000));

        robotweax::srt::PacketView shutdown;
        shutdown.kind = robotweax::srt::PacketKind::control;
        shutdown.control.type = robotweax::srt::ControlType::shutdown;
        shutdown.control.destination_socket_id = 77;
        const std::array<std::byte, 4> shutdown_padding {};
        shutdown.payload = shutdown_padding;
        runtime->process_packet(shutdown, IpEndpoint::loopback(9'000));

        std::array<char, payload.size()> received {};
        REQUIRE_EQ(srt_recvmsg(group, received.data(),
                       static_cast<int>(received.size())),
            SRT_ERROR);
        REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EASYNCRCV);

        clock.now_microseconds =
            static_cast<std::uint64_t>(receive_delay_milliseconds) * 1'000U;
        std::array<char, 3> short_buffer {};
        REQUIRE_EQ(srt_recvmsg(group, short_buffer.data(),
                       static_cast<int>(short_buffer.size())),
            SRT_ERROR);
        REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ELARGEMSG);
        std::array<SRT_SOCKGROUPDATA, 1> group_data {};
        SRT_MSGCTRL control = srt_msgctrl_default;
        control.grpdata = group_data.data();
        control.grpdata_size = group_data.size();
        REQUIRE_EQ(srt_recvmsg2(group, received.data(),
                       static_cast<int>(received.size()), &control),
            static_cast<int>(payload.size()));
        REQUIRE(std::equal(payload.begin(), payload.end(),
            reinterpret_cast<const std::byte*>(received.data())));
        REQUIRE_EQ(control.grpdata_size, 1U);
        REQUIRE_EQ(group_data[0].memberstate, SRT_GST_BROKEN);
        REQUIRE_EQ(group_data[0].sockstate, SRTS_BROKEN);
        SRT_TRACEBSTATS member_statistics {};
        REQUIRE_EQ(srt_bstats(member, &member_statistics, 0), 0);
        REQUIRE_EQ(member_statistics.pktRecvUniqueTotal, 1);
        REQUIRE_EQ(srt_recvmsg(group, received.data(),
                       static_cast<int>(received.size())),
            SRT_ERROR);
        REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ECONNLOST);

        REQUIRE_EQ(srt_close(group), 0);
        REQUIRE_EQ(srt_bstats(member, &member_statistics, 0), SRT_ERROR);
        REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVSOCK);
    }
}

TEST(compat_group_epoll_delays_terminal_error_until_buffered_delivery)
{
    for (const auto type : {SRT_GTYPE_BACKUP, SRT_GTYPE_BROADCAST}) {
        for (const int watched :
            {SRT_EPOLL_IN | SRT_EPOLL_ERR, SRT_EPOLL_OUT | SRT_EPOLL_ERR}) {
            const SRTSOCKET group = srt_create_group(type);
            const SRTSOCKET member = srt_create_socket();
            REQUIRE(group != SRT_INVALID_SOCK);
            REQUIRE(member != SRT_INVALID_SOCK);
            const auto record = GroupRegistry::instance().find(group);
            REQUIRE(record != nullptr);
            TestClock clock {.channel = std::make_shared<
                                 robotweax::srt::compat::DatagramChannel>()};
            clock.channel->set_send_hook_for_testing(
                accept_test_datagram, nullptr);
            constexpr std::uint16_t delay_milliseconds = 100;
            const auto runtime = attach_group_runtime(group, member,
                record->initial_sequence, 1, &clock, 0, true,
                delay_milliseconds, ConnectionRuntime::Clock::now(), record);
            const bool asynchronous = false;
            REQUIRE_EQ(srt_setsockflag(group, SRTO_RCVSYN, &asynchronous,
                           static_cast<int>(sizeof(asynchronous))),
                0);
            const std::array<std::byte, 1> payload {std::byte {'x'}};
            robotweax::srt::PacketView data;
            data.kind = robotweax::srt::PacketKind::data;
            data.data.sequence = SequenceNumber {record->initial_sequence};
            data.data.message_number = 1;
            data.data.boundary = robotweax::srt::MessageBoundary::solo;
            data.data.in_order = true;
            data.payload = payload;
            runtime->process_packet(data, IpEndpoint::loopback(9'000));
            robotweax::srt::PacketView shutdown;
            shutdown.kind = robotweax::srt::PacketKind::control;
            shutdown.control.type = robotweax::srt::ControlType::shutdown;
            shutdown.control.destination_socket_id = 77;
            const std::array<std::byte, 4> shutdown_padding {};
            shutdown.payload = shutdown_padding;
            runtime->process_packet(shutdown, IpEndpoint::loopback(9'000));

            const int poll = srt_epoll_create();
            REQUIRE(poll >= 0);
            REQUIRE_EQ(srt_epoll_add_usock(poll, group, &watched), 0);
            SRT_EPOLL_EVENT event {};
            REQUIRE_EQ(srt_epoll_uwait(poll, &event, 1, 0), 0);
            std::array<char, 8> buffer {};
            REQUIRE_EQ(
                srt_recvmsg(group, buffer.data(), buffer.size()), SRT_ERROR);
            REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EASYNCRCV);

            clock.now_microseconds =
                static_cast<std::uint64_t>(delay_milliseconds) * 1'000U;
            REQUIRE_EQ(srt_epoll_update_usock(poll, group, &watched), 0);
            if ((watched & SRT_EPOLL_IN) != 0) {
                REQUIRE_EQ(srt_epoll_uwait(poll, &event, 1, 0), 1);
                REQUIRE_EQ(event.events, SRT_EPOLL_IN);
            } else {
                REQUIRE_EQ(srt_epoll_uwait(poll, &event, 1, 0), 0);
            }
            REQUIRE_EQ(srt_recvmsg(group, buffer.data(), buffer.size()), 1);
            REQUIRE_EQ(buffer[0], 'x');
            REQUIRE_EQ(srt_epoll_uwait(poll, &event, 1, 0), 1);
            REQUIRE_EQ(event.events, watched);
            REQUIRE_EQ(
                srt_recvmsg(group, buffer.data(), buffer.size()), SRT_ERROR);
            REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ECONNLOST);
            REQUIRE_EQ(srt_epoll_release(poll), 0);
            REQUIRE_EQ(srt_close(group), 0);
        }
    }
}

TEST(compat_group_epoll_and_receive_wake_when_group_closes)
{
    for (const auto type : {SRT_GTYPE_BACKUP, SRT_GTYPE_BROADCAST}) {
        const SRTSOCKET group = srt_create_group(type);
        const SRTSOCKET member = srt_create_socket();
        REQUIRE(group != SRT_INVALID_SOCK);
        REQUIRE(member != SRT_INVALID_SOCK);
        const auto record = GroupRegistry::instance().find(group);
        REQUIRE(record != nullptr);
        (void)attach_group_runtime(group, member, record->initial_sequence, 1);
        const int poll = srt_epoll_create();
        REQUIRE(poll >= 0);
        const int watched = SRT_EPOLL_IN | SRT_EPOLL_ERR;
        REQUIRE_EQ(srt_epoll_add_usock(poll, group, &watched), 0);

        auto receive = std::async(std::launch::async, [group] {
            std::array<char, 8> buffer {};
            const int result = srt_recvmsg(group, buffer.data(), buffer.size());
            return std::pair {result, srt_getlasterror(nullptr)};
        });
        auto wait = std::async(std::launch::async, [poll] {
            SRT_EPOLL_EVENT event {};
            const int result = srt_epoll_uwait(poll, &event, 1, 1'000);
            return std::pair {result, event.events};
        });
        std::this_thread::sleep_for(std::chrono::milliseconds {10});
        REQUIRE_EQ(srt_close(group), 0);
        REQUIRE(receive.wait_for(std::chrono::seconds {1})
            == std::future_status::ready);
        const auto [received, receive_error] = receive.get();
        REQUIRE_EQ(received, SRT_ERROR);
        REQUIRE_EQ(receive_error, SRT_ESCLOSED);
        REQUIRE(wait.wait_for(std::chrono::seconds {1})
            == std::future_status::ready);
        const auto [events, event_mask] = wait.get();
        REQUIRE_EQ(events, 1);
        REQUIRE((event_mask & SRT_EPOLL_ERR) != 0);
        REQUIRE_EQ(srt_epoll_release(poll), 0);
    }
}

TEST(
    compat_broadcast_group_retires_hard_failures_while_other_members_are_congested)
{
    for (const bool blocking : {false, true}) {
        TestClock clock;
        clock.channel =
            std::make_shared<robotweax::srt::compat::DatagramChannel>();
        clock.channel->set_send_hook_for_testing(accept_test_datagram, nullptr);
        const SRTSOCKET group = srt_create_group(SRT_GTYPE_BROADCAST);
        const SRTSOCKET failed = srt_create_socket();
        const SRTSOCKET slow = srt_create_socket();
        const auto record = GroupRegistry::instance().find(group);
        REQUIRE(record != nullptr);
        std::uint32_t initial = 0;
        {
            std::lock_guard lock(record->mutex);
            initial = record->initial_sequence;
        }
        const auto failed_runtime =
            attach_group_runtime(group, failed, initial);
        const auto slow_runtime =
            attach_group_runtime(group, slow, initial, 1, &clock, 1U);
        const bool synchronous = blocking;
        const std::int32_t timeout = 20;
        REQUIRE_EQ(srt_setsockflag(
                       group, SRTO_SNDSYN, &synchronous, sizeof(synchronous)),
            0);
        REQUIRE_EQ(
            srt_setsockflag(group, SRTO_SNDTIMEO, &timeout, sizeof(timeout)),
            0);
        constexpr char payload[] = "capacity";
        REQUIRE_EQ(srt_send(group, payload, sizeof(payload)),
            static_cast<int>(sizeof(payload)));
        failed_runtime->mark_broken(0);
        REQUIRE_EQ(srt_send(group, payload, sizeof(payload)), SRT_ERROR);
        REQUIRE_EQ(
            srt_getlasterror(nullptr), blocking ? SRT_ETIMEOUT : SRT_EASYNCSND);
        const auto state = srt_getsockstate(failed);
        REQUIRE(state == SRTS_CLOSING || state == SRTS_CLOSED);
        REQUIRE_EQ(srt_getsockstate(slow), SRTS_CONNECTED);
        (void)slow_runtime->poll();
        deliver_lite_ack(slow_runtime, SequenceNumber {initial}.next());
        REQUIRE_EQ(srt_send(group, payload, sizeof(payload)),
            static_cast<int>(sizeof(payload)));
        REQUIRE_EQ(srt_close(group), 0);
    }
}

TEST(compat_group_connect_description_honors_receive_synchronous_before_open)
{
    for (const bool synchronous : {false, true}) {
        const SRTSOCKET group = srt_create_group(SRT_GTYPE_BROADCAST);
        REQUIRE_EQ(srt_setsockflag(
                       group, SRTO_RCVSYN, &synchronous, sizeof(synchronous)),
            0);
        GroupRegistry::ConnectDescription description;
        REQUIRE(GroupRegistry::instance().describe_connect(group, description));
        REQUIRE_EQ(description.block_until_connected, synchronous);
        REQUIRE_EQ(srt_close(group), 0);
    }
}

TEST(
    compat_group_connect_getter_initializes_the_complete_integer_representation)
{
    const SRTSOCKET listener = srt_create_socket();
    REQUIRE(listener != SRT_INVALID_SOCK);
    for (const bool enabled : {false, true}) {
        REQUIRE_EQ(srt_setsockflag(
                       listener, SRTO_GROUPCONNECT, &enabled, sizeof(enabled)),
            0);
        std::array<std::int32_t, 2> result {-1, 0x12345678};
        int size = sizeof(result);
        REQUIRE_EQ(
            srt_getsockflag(listener, SRTO_GROUPCONNECT, result.data(), &size),
            0);
        REQUIRE_EQ(size, static_cast<int>(sizeof(std::int32_t)));
        REQUIRE_EQ(result[0], enabled ? 1 : 0);
        REQUIRE_EQ(result[1], 0x12345678);
        bool compact = !enabled;
        size = sizeof(compact);
        REQUIRE_EQ(
            srt_getsockflag(listener, SRTO_GROUPCONNECT, &compact, &size), 0);
        REQUIRE_EQ(compact, enabled);
        REQUIRE_EQ(size, static_cast<int>(sizeof(bool)));
    }
    REQUIRE_EQ(srt_close(listener), 0);
}

TEST(compat_group_state_and_data_refresh_terminal_members)
{
    for (const auto type : {SRT_GTYPE_BACKUP, SRT_GTYPE_BROADCAST}) {
        for (const bool read_data_first : {false, true}) {
            const auto group = srt_create_group(type);
            const auto socket = srt_create_socket();
            const auto record = GroupRegistry::instance().find(group);
            REQUIRE(record != nullptr);
            const auto runtime =
                attach_group_runtime(group, socket, record->initial_sequence);
            robotweax::srt::PacketView shutdown;
            shutdown.kind = robotweax::srt::PacketKind::control;
            shutdown.control.type = robotweax::srt::ControlType::shutdown;
            const std::array<std::byte, 4> padding {};
            shutdown.payload = padding;
            runtime->process_packet(shutdown, IpEndpoint::loopback(9'000));
            std::array<SRT_SOCKGROUPDATA, 1> data {};
            std::size_t size = data.size();
            if (!read_data_first) {
                REQUIRE_EQ(srt_getsockstate(group), SRTS_BROKEN);
            }
            REQUIRE_EQ(srt_group_data(group, data.data(), &size), 1);
            REQUIRE_EQ(data[0].sockstate, SRTS_BROKEN);
            REQUIRE_EQ(data[0].memberstate, SRT_GST_BROKEN);
            REQUIRE_EQ(srt_getsockstate(group), SRTS_BROKEN);
            REQUIRE_EQ(srt_close(group), 0);
        }
    }
}

TEST(
    compat_group_status_refresh_preserves_terminal_publication_at_inline_boundary)
{
    for (const auto type : {SRT_GTYPE_BACKUP, SRT_GTYPE_BROADCAST}) {
        for (const unsigned count : {16U, 17U}) {
            for (const unsigned first_query : {0U, 1U, 2U}) {
                const auto group = srt_create_group(type);
                const auto record = GroupRegistry::instance().find(group);
                REQUIRE(record != nullptr);
                std::vector<std::shared_ptr<ConnectionRuntime>> runtimes;
                for (unsigned index = 0; index < count; ++index) {
                    const auto socket = srt_create_socket();
                    runtimes.push_back(attach_group_runtime(group, socket,
                        record->initial_sequence, 1, nullptr, 8));
                }
                robotweax::srt::PacketView shutdown;
                shutdown.kind = robotweax::srt::PacketKind::control;
                shutdown.control.type = robotweax::srt::ControlType::shutdown;
                const std::array<std::byte, 4> padding {};
                shutdown.payload = padding;
                for (const auto& runtime : runtimes) {
                    runtime->process_packet(
                        shutdown, IpEndpoint::loopback(9'000));
                }
                std::array<SRT_SOCKGROUPDATA, 17> data {};
                if (first_query == 0U) {
                    REQUIRE_EQ(srt_getsockstate(group), SRTS_BROKEN);
                } else if (first_query == 1U) {
                    std::size_t size = data.size();
                    REQUIRE_EQ(srt_group_data(group, data.data(), &size),
                        static_cast<int>(count));
                    REQUIRE_EQ(size, count);
                } else {
                    int state = 0;
                    int size = sizeof(state);
                    REQUIRE_EQ(
                        srt_getsockflag(group, SRTO_STATE, &state, &size), 0);
                    REQUIRE_EQ(state, SRTS_BROKEN);
                    REQUIRE_EQ(size, static_cast<int>(sizeof(state)));
                }
                std::size_t size = data.size();
                REQUIRE_EQ(srt_group_data(group, data.data(), &size),
                    static_cast<int>(count));
                for (unsigned index = 0; index < count; ++index) {
                    REQUIRE_EQ(data[index].sockstate, SRTS_BROKEN);
                    REQUIRE_EQ(data[index].memberstate, SRT_GST_BROKEN);
                }
                REQUIRE_EQ(srt_close(group), 0);
            }
        }
    }
}

TEST(compat_group_close_detaches_members_across_replacement_cycles)
{
    for (const auto type : {SRT_GTYPE_BACKUP, SRT_GTYPE_BROADCAST}) {
        const auto group = srt_create_group(type);
        const auto healthy = srt_create_socket();
        const auto record = GroupRegistry::instance().find(group);
        REQUIRE(record != nullptr);
        (void)attach_group_runtime(group, healthy, record->initial_sequence);
        for (int cycle = 0; cycle < 32; ++cycle) {
            const auto replacement = srt_create_socket();
            (void)attach_group_runtime(
                group, replacement, record->initial_sequence);
            std::size_t size = 0;
            REQUIRE_EQ(srt_group_data(group, nullptr, &size), 0);
            REQUIRE_EQ(size, 2U);
            REQUIRE_EQ(srt_close(replacement), 0);
            REQUIRE_EQ(srt_group_data(group, nullptr, &size), 0);
            REQUIRE_EQ(size, 1U);
            REQUIRE_EQ(srt_getsockstate(group), SRTS_CONNECTED);
        }
        REQUIRE_EQ(srt_close(healthy), 0);
        REQUIRE_EQ(srt_getsockstate(group), SRTS_BROKEN);
        REQUIRE_EQ(srt_close(group), 0);
    }
}

TEST(compat_group_state_ignores_terminal_members_before_pending_replacement)
{
    const auto group = srt_create_group(SRT_GTYPE_BACKUP);
    const auto record = GroupRegistry::instance().find(group);
    REQUIRE(record != nullptr);
    for (const auto state :
        {SRTS_BROKEN, SRTS_CLOSING, SRTS_CLOSED, SRTS_NONEXIST}) {
        {
            std::lock_guard lock(record->mutex);
            record->members.clear();
            robotweax::srt::compat::GroupMemberSnapshot terminal;
            terminal.public_data.sockstate = state;
            record->members.push_back(terminal);
            robotweax::srt::compat::GroupMemberSnapshot pending;
            pending.public_data.sockstate = SRTS_CONNECTING;
            record->members.push_back(pending);
        }
        REQUIRE_EQ(srt_getsockstate(group), SRTS_CONNECTING);
    }
    REQUIRE_EQ(srt_close(group), 0);
}

TEST(compat_group_terminal_gap_keeps_future_messages_before_replacement_floor)
{
    for (const auto type : {SRT_GTYPE_BACKUP, SRT_GTYPE_BROADCAST}) {
        for (const bool rollover : {false, true}) {
            const auto group = srt_create_group(type);
            const auto terminal = srt_create_socket();
            const auto replacement = srt_create_socket();
            const auto record = GroupRegistry::instance().find(group);
            REQUIRE(record != nullptr);
            const SequenceNumber initial {rollover ? SequenceNumber::mask - 1U
                                                   : record->initial_sequence};
            {
                std::lock_guard lock(record->mutex);
                record->initial_sequence = initial.value();
                record->next_send_sequence = initial.value();
                record->next_receive_sequence = initial.value();
            }
            TestClock clock {.now_microseconds = 0,
                .channel = std::make_shared<
                    robotweax::srt::compat::DatagramChannel>()};
            clock.channel->set_send_hook_for_testing(
                accept_test_datagram, nullptr);
            const auto origin = ConnectionRuntime::Clock::now();
            const auto runtime = attach_group_runtime(group, terminal,
                initial.value(), 1, &clock, 0, true, 100, origin, record, {},
                nullptr, nullptr, true);
            (void)attach_group_runtime(group, replacement,
                initial.advanced(3).value(), 1, &clock, 0, true, 100, origin,
                record, {}, nullptr, nullptr, true);
            const bool synchronous = false;
            REQUIRE_EQ(srt_setsockflag(group, SRTO_RCVSYN, &synchronous,
                           sizeof(synchronous)),
                0);
            for (std::uint32_t offset = 1; offset <= 2; ++offset) {
                const std::array<std::byte, 1> payload {
                    offset == 1 ? std::byte {'b'} : std::byte {'c'}};
                robotweax::srt::PacketView packet;
                packet.kind = robotweax::srt::PacketKind::data;
                packet.data.sequence = initial.advanced(offset);
                packet.data.message_number = offset + 1;
                packet.data.boundary = robotweax::srt::MessageBoundary::solo;
                packet.data.in_order = true;
                packet.data.timestamp =
                    robotweax::srt::PacketTimestamp {offset * 1'000U};
                packet.payload = payload;
                runtime->process_packet(packet, IpEndpoint::loopback(9'000));
            }
            robotweax::srt::PacketView shutdown;
            shutdown.kind = robotweax::srt::PacketKind::control;
            shutdown.control.type = robotweax::srt::ControlType::shutdown;
            const std::array<std::byte, 4> padding {};
            shutdown.payload = padding;
            runtime->process_packet(shutdown, IpEndpoint::loopback(9'000));
            const auto poll = srt_epoll_create();
            const int watched = SRT_EPOLL_IN;
            REQUIRE_EQ(srt_epoll_add_usock(poll, group, &watched), 0);
            SRT_EPOLL_EVENT event {};
            REQUIRE_EQ(srt_epoll_uwait(poll, &event, 1, 0), 0);
            std::array<char, 8> buffer {};
            SRT_MSGCTRL control = srt_msgctrl_default;
            REQUIRE_EQ(
                srt_recvmsg2(group, buffer.data(), buffer.size(), &control),
                SRT_ERROR);
            REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EASYNCRCV);
            REQUIRE(runtime->has_buffered_receive_data());
            REQUIRE_EQ(runtime->receive_floor_sequence(), initial);
            for (std::uint32_t offset = 1; offset <= 2; ++offset) {
                clock.now_microseconds = 100'000U + offset * 1'000U;
                REQUIRE_EQ(srt_epoll_update_usock(poll, group, &watched), 0);
                REQUIRE_EQ(srt_epoll_uwait(poll, &event, 1, 0), 1);
                REQUIRE_EQ(event.events, SRT_EPOLL_IN);
                REQUIRE_EQ(
                    srt_recvmsg2(group, buffer.data(), buffer.size(), &control),
                    1);
                REQUIRE_EQ(buffer[0], offset == 1 ? 'b' : 'c');
                REQUIRE_EQ(control.pktseq,
                    static_cast<std::int32_t>(
                        initial.advanced(offset).value()));
            }
            REQUIRE_EQ(srt_epoll_release(poll), 0);
            REQUIRE_EQ(srt_close(group), 0);
        }
    }
}

TEST(compat_group_option_parity_uses_socket_validation_and_normalization)
{
    const auto check = [](SRT_SOCKOPT option, const auto& configured) {
        const SRTSOCKET group = srt_create_group(SRT_GTYPE_BROADCAST);
        const SRTSOCKET socket = srt_create_socket();
        struct Cleanup {
            SRTSOCKET group, socket;
            ~Cleanup()
            {
                (void)srt_close(group);
                (void)srt_close(socket);
            }
        } cleanup {group, socket};
        REQUIRE(group != SRT_INVALID_SOCK);
        REQUIRE(socket != SRT_INVALID_SOCK);
        REQUIRE_EQ(
            srt_setsockflag(socket, option, &configured, sizeof(configured)),
            0);
        REQUIRE_EQ(
            srt_setsockflag(group, option, &configured, sizeof(configured)), 0);
        std::array<std::byte, 1'024> expected {}, actual {};
        int expected_size = expected.size();
        int actual_size = actual.size();
        REQUIRE_EQ(
            srt_getsockflag(socket, option, expected.data(), &expected_size),
            0);
        REQUIRE_EQ(
            srt_getsockflag(group, option, actual.data(), &actual_size), 0);
        REQUIRE_EQ(actual_size, expected_size);
        REQUIRE_EQ(actual, expected);
        REQUIRE_EQ(srt_setsockflag(group, option, &configured, 0), SRT_ERROR);
        actual_size = actual.size();
        REQUIRE_EQ(
            srt_getsockflag(group, option, actual.data(), &actual_size), 0);
        REQUIRE_EQ(actual, expected);
    };
    for (const auto [option, value] : {std::pair {SRTO_SNDDROPDELAY, -1},
             {SRTO_LOSSMAXTTL, 7}, {SRTO_RETRANSMITALGO, 0}, {SRTO_OHEADBW, 40},
             {SRTO_UDP_SNDBUF, 131'072}, {SRTO_UDP_RCVBUF, 262'144},
             {SRTO_MSS, 1'400}, {SRTO_FC, 512}, {SRTO_SNDBUF, 100'000},
             {SRTO_RCVBUF, 100'000}, {SRTO_IPV6ONLY, 1}}) {
        check(option, value);
    }
    check(SRTO_INPUTBW, std::int64_t {125'000});
    for (const auto option : {SRTO_REUSEADDR, SRTO_MESSAGEAPI, SRTO_NAKREPORT,
             SRTO_TLPKTDROP, SRTO_SENDER}) {
        check(option, false);
    }
    check(SRTO_TRANSTYPE, SRTT_LIVE);
    check(SRTO_TSBPDMODE, true);
    check(SRTO_LINGER, linger {1, 2});
}

TEST(compat_group_option_parity_updates_all_members_and_future_defaults)
{
    const SRTSOCKET group = srt_create_group(SRT_GTYPE_BROADCAST);
    const SRTSOCKET first = srt_create_socket();
    const SRTSOCKET second = srt_create_socket();
    struct Cleanup {
        SRTSOCKET group;
        ~Cleanup()
        {
            (void)srt_close(group);
        }
    } cleanup {group};
    REQUIRE(group != SRT_INVALID_SOCK);
    REQUIRE(first != SRT_INVALID_SOCK);
    REQUIRE(second != SRT_INVALID_SOCK);
    attach_group_runtime(group, first, 100U);
    attach_group_runtime(group, second, 100U);
    const auto group_record = GroupRegistry::instance().find(group);
    REQUIRE(group_record != nullptr);
    GroupRegistry::instance().mark_opened(group, group_record->generation);
    const std::int64_t bandwidth = 2'000'000;
    const std::int64_t input = 1'000'000;
    const std::int32_t overhead = 40;
    REQUIRE_EQ(
        srt_setsockflag(group, SRTO_MAXBW, &bandwidth, sizeof(bandwidth)), 0);
    REQUIRE_EQ(srt_setsockflag(group, SRTO_INPUTBW, &input, sizeof(input)), 0);
    REQUIRE_EQ(
        srt_setsockflag(group, SRTO_OHEADBW, &overhead, sizeof(overhead)), 0);
    for (const auto member : {group, first, second}) {
        std::int64_t actual = 0;
        int size = sizeof(actual);
        REQUIRE_EQ(srt_getsockflag(member, SRTO_MAXBW, &actual, &size), 0);
        REQUIRE_EQ(actual, bandwidth);
        size = sizeof(actual);
        REQUIRE_EQ(srt_getsockflag(member, SRTO_INPUTBW, &actual, &size), 0);
        REQUIRE_EQ(actual, input);
        std::int32_t actual_overhead = 0;
        size = sizeof(actual_overhead);
        REQUIRE_EQ(
            srt_getsockflag(member, SRTO_OHEADBW, &actual_overhead, &size), 0);
        REQUIRE_EQ(actual_overhead, overhead);
    }
    GroupRegistry::ConnectDescription description;
    REQUIRE(GroupRegistry::instance().describe_connect(group, description));
    REQUIRE_EQ(description.member_native_options
                   .get(robotweax::srt::SocketOption::
                           maximum_bandwidth_bytes_per_second)
                   .value,
        bandwidth);
    const std::int64_t invalid = -2;
    REQUIRE_EQ(srt_setsockflag(group, SRTO_MAXBW, &invalid, sizeof(invalid)),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);
    const bool disabled = false;
    REQUIRE_EQ(
        srt_setsockflag(group, SRTO_TSBPDMODE, &disabled, sizeof(disabled)),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ECONNSOCK);
    std::int32_t state = 0;
    int size = sizeof(state);
    REQUIRE_EQ(srt_getsockflag(group, SRTO_STATE, &state, &size), 0);
    REQUIRE_EQ(state, SRTS_CONNECTED);
    size = sizeof(state);
    REQUIRE_EQ(srt_getsockflag(group, SRTO_KMSTATE, &state, &size), 0);
    REQUIRE_EQ(state, SRT_KM_S_UNSECURED);
}

TEST(compat_group_option_parity_mirror_uses_listener_then_member_values)
{
    const SRTSOCKET listener = srt_create_socket();
    const SRTSOCKET caller = srt_create_group(SRT_GTYPE_BROADCAST);
    struct Cleanup {
        SRTSOCKET listener, caller, mirror = SRT_INVALID_SOCK;
        ~Cleanup()
        {
            (void)srt_close(mirror);
            (void)srt_close(caller);
            (void)srt_close(listener);
        }
    } cleanup {listener, caller};
    const std::int32_t latency = 500;
    const std::int32_t idle = 9'000;
    REQUIRE_EQ(
        srt_setsockflag(listener, SRTO_RCVLATENCY, &latency, sizeof(latency)),
        0);
    REQUIRE_EQ(
        srt_setsockflag(listener, SRTO_PEERIDLETIMEO, &idle, sizeof(idle)), 0);
    GroupRegistry::MirrorDescription description;
    REQUIRE(GroupRegistry::instance().prepare_mirror(
        listener, caller, SRT_GTYPE_BROADCAST, 100U, description));
    cleanup.mirror = description.group;
    for (const auto [option, expected] :
        {std::pair {SRTO_RCVLATENCY, latency}, {SRTO_PEERIDLETIMEO, idle}}) {
        std::int32_t actual = 0;
        int size = sizeof(actual);
        REQUIRE_EQ(srt_getsockflag(cleanup.mirror, option, &actual, &size), 0);
        REQUIRE_EQ(actual, expected);
    }
    const SRTSOCKET member = srt_create_socket();
    REQUIRE(member != SRT_INVALID_SOCK);
    constexpr char stream[] = "#!::r=accepted/group";
    REQUIRE_EQ(
        srt_setsockflag(member, SRTO_STREAMID, stream, sizeof(stream) - 1), 0);
    const std::int32_t negotiated = 700;
    REQUIRE_EQ(srt_setsockflag(
                   member, SRTO_RCVLATENCY, &negotiated, sizeof(negotiated)),
        0);
    attach_group_runtime(cleanup.mirror, member, 100U);
    {
        const auto member_record = SocketRegistry::instance().find(member);
        std::lock_guard lock(member_record->mutex);
        member_record->negotiated_live_options.receive_delay_milliseconds =
            negotiated;
    }
    std::array<char, sizeof(stream)> actual {};
    int size = actual.size();
    REQUIRE_EQ(
        srt_getsockflag(cleanup.mirror, SRTO_STREAMID, actual.data(), &size),
        0);
    REQUIRE_EQ(std::memcmp(actual.data(), stream, sizeof(stream)), 0);
    std::int32_t actual_latency = 0;
    size = sizeof(actual_latency);
    REQUIRE_EQ(srt_getsockflag(
                   cleanup.mirror, SRTO_RCVLATENCY, &actual_latency, &size),
        0);
    REQUIRE_EQ(actual_latency, negotiated);
    REQUIRE_EQ(srt_close(member), 0);
    size = sizeof(actual_latency);
    REQUIRE_EQ(srt_getsockflag(
                   cleanup.mirror, SRTO_RCVLATENCY, &actual_latency, &size),
        0);
    REQUIRE_EQ(actual_latency, latency);
}

TEST(compat_group_member_identity_updates_remain_exact_after_compaction)
{
    for (const auto type : {SRT_GTYPE_BROADCAST, SRT_GTYPE_BACKUP}) {
        const auto group = srt_create_group(type);
        const auto record = GroupRegistry::instance().find(group);
        REQUIRE(record != nullptr);
        std::array<SRTSOCKET, 34> sockets {};
        std::array<std::uint64_t, 34> generations {};
        std::uint64_t group_generation = 0;
        sockaddr_storage peer {};
        const auto address = ipv4_address(9'123);
        std::memcpy(&peer, &address, sizeof(address));
        const auto add = [&](unsigned index) {
            sockets[index] = srt_create_socket();
            REQUIRE(sockets[index] != SRT_INVALID_SOCK);
            REQUIRE(GroupRegistry::instance().add_member(group, sockets[index],
                peer, 1, static_cast<int>(index), group_generation,
                generations[index]));
            GroupRegistry::instance().update_member(group, group_generation,
                sockets[index], generations[index], SRTS_CONNECTED,
                SRT_SUCCESS);
        };
        for (unsigned i = 0; i < 33; ++i) {
            add(i);
        }
        std::uint64_t version = 0;
        {
            std::lock_guard lock(record->mutex);
            REQUIRE(record->member_generations_ordered);
            version = record->snapshot_version;
        }
        GroupRegistry::instance().note_io_result(group, group_generation,
            sockets[9], generations[10], SRT_GST_RUNNING, SRT_ERROR);
        GroupRegistry::instance().update_member(group, group_generation + 1U,
            sockets[10], generations[10], SRTS_BROKEN, SRT_ERROR, true);
        {
            std::lock_guard lock(record->mutex);
            REQUIRE_EQ(record->snapshot_version, version);
        }
        GroupRegistry::instance().remove_member(
            group, group_generation, sockets[0], generations[0]);
        add(33);
        GroupRegistry::instance().note_io_result(group, group_generation,
            sockets[32], generations[32], SRT_GST_RUNNING, SRT_ERROR);
        GroupRegistry::instance().update_member(group, group_generation,
            sockets[1], generations[1], SRTS_BROKEN, SRT_ERROR, true);
        {
            std::lock_guard lock(record->mutex);
            REQUIRE(record->member_generations_ordered);
            REQUIRE_EQ(record->members.size(), 33U);
            REQUIRE_EQ(record->members.front().public_data.id, sockets[1]);
            REQUIRE_EQ(
                record->members.front().public_data.sockstate, SRTS_BROKEN);
            REQUIRE_EQ(record->members[31].public_data.id, sockets[32]);
            REQUIRE_EQ(
                record->members[31].public_data.memberstate, SRT_GST_RUNNING);
            REQUIRE_EQ(record->members[31].public_data.result, SRT_ERROR);
            REQUIRE_EQ(record->members.back().public_data.id, sockets[33]);
            REQUIRE_EQ(
                record->members.back().public_data.sockstate, SRTS_CONNECTED);
            REQUIRE_EQ(record->update_version, 1U);
            version = record->snapshot_version;
        }
        GroupRegistry::instance().note_io_result(group, group_generation,
            sockets[0], generations[0], SRT_GST_RUNNING, SRT_ERROR);
        {
            std::lock_guard lock(record->mutex);
            REQUIRE_EQ(record->snapshot_version, version);
        }
        REQUIRE_EQ(srt_close(group), 0);
        REQUIRE_EQ(srt_close(sockets[0]), 0);
    }
}

TEST(compat_group_member_identity_updates_preserve_generation_wrap_and_reuse)
{
    for (const auto type : {SRT_GTYPE_BROADCAST, SRT_GTYPE_BACKUP}) {
        const auto group = srt_create_group(type);
        const auto record = GroupRegistry::instance().find(group);
        REQUIRE(record != nullptr);
        std::array<SRTSOCKET, 5> sockets {};
        std::array<std::uint64_t, 5> generations {};
        std::uint64_t group_generation = 0;
        sockaddr_storage peer {};
        const auto address = ipv4_address(9'124);
        std::memcpy(&peer, &address, sizeof(address));
        const auto add = [&](unsigned index) {
            sockets[index] = srt_create_socket();
            REQUIRE(sockets[index] != SRT_INVALID_SOCK);
            REQUIRE(GroupRegistry::instance().add_member(group, sockets[index],
                peer, 1, static_cast<int>(index), group_generation,
                generations[index]));
            GroupRegistry::instance().update_member(group, group_generation,
                sockets[index], generations[index], SRTS_CONNECTED,
                SRT_SUCCESS);
        };
        add(0);
        {
            std::lock_guard lock(record->mutex);
            record->next_member_generation =
                std::numeric_limits<std::uint64_t>::max() - 1U;
        }
        for (unsigned i = 1; i < 4; ++i) {
            add(i);
        }
        REQUIRE_EQ(generations[0], generations[3]);
        GroupRegistry::instance().note_io_result(group, group_generation,
            sockets[3], generations[3], SRT_GST_RUNNING, SRT_ERROR);
        {
            std::lock_guard lock(record->mutex);
            REQUIRE(!record->member_generations_ordered);
            REQUIRE_EQ(
                record->members[0].public_data.memberstate, SRT_GST_IDLE);
            REQUIRE_EQ(
                record->members[3].public_data.memberstate, SRT_GST_RUNNING);
        }
        GroupRegistry::instance().update_member(group, group_generation,
            sockets[3], generations[3], SRTS_BROKEN, SRT_ERROR, true);
        GroupRegistry::instance().remove_member(
            group, group_generation, sockets[0], generations[0]);
        GroupRegistry::instance().note_io_result(group, group_generation,
            sockets[3], generations[3], SRT_GST_BROKEN, SRT_SUCCESS);
        {
            std::lock_guard lock(record->mutex);
            REQUIRE_EQ(record->members.front().public_data.id, sockets[1]);
            REQUIRE_EQ(record->members.back().public_data.id, sockets[3]);
            REQUIRE_EQ(
                record->members.back().public_data.sockstate, SRTS_BROKEN);
            REQUIRE_EQ(record->members.back().public_data.result, SRT_SUCCESS);
        }
        for (unsigned i = 1; i < 4; ++i) {
            GroupRegistry::instance().remove_member(
                group, group_generation, sockets[i], generations[i]);
        }
        add(4);
        GroupRegistry::instance().note_io_result(group, group_generation,
            sockets[4], generations[4], SRT_GST_RUNNING, SRT_ERROR);
        {
            std::lock_guard lock(record->mutex);
            REQUIRE(record->member_generations_ordered);
            REQUIRE_EQ(record->members.size(), 1U);
            REQUIRE_EQ(record->members.front().public_data.id, sockets[4]);
            REQUIRE_EQ(record->members.front().public_data.memberstate,
                SRT_GST_RUNNING);
        }
        REQUIRE_EQ(srt_close(group), 0);
        for (unsigned i = 0; i < 4; ++i) {
            REQUIRE_EQ(srt_close(sockets[i]), 0);
        }
    }
}

TEST(compat_group_send_result_buffers_keep_large_member_outcomes_exact)
{
    for (const auto type : {SRT_GTYPE_BROADCAST, SRT_GTYPE_BACKUP}) {
        for (const unsigned count : {1U, 16U, 17U, 64U}) {
            for (const bool congested_prefix : {false, true}) {
                const auto group = srt_create_group(type);
                const auto record = GroupRegistry::instance().find(group);
                REQUIRE(record != nullptr);
                const auto initial = record->initial_sequence;
                const auto target = SequenceNumber {initial}.advanced(
                    congested_prefix ? 1U : 0U);
                record->next_send_sequence = target.value();
                record->next_send_message = congested_prefix ? 2U : 1U;
                const bool nonblocking = false;
                REQUIRE_EQ(srt_setsockflag(group, SRTO_SNDSYN, &nonblocking,
                               sizeof(nonblocking)),
                    0);
                std::array<SRTSOCKET, 64> sockets {};
                const std::array<std::byte, 1> older {std::byte {'o'}};
                for (unsigned index = 0; index < count; ++index) {
                    sockets[index] = srt_create_socket();
                    REQUIRE(sockets[index] != SRT_INVALID_SOCK);
                    const auto runtime = attach_group_runtime(group,
                        sockets[index],
                        congested_prefix && index + 1U < count ? initial
                                                               : target.value(),
                        static_cast<std::uint16_t>(count - index), nullptr, 1U);
                    if (congested_prefix && index + 1U < count) {
                        REQUIRE_EQ(
                            runtime
                                ->queue_group_message(older,
                                    SequenceNumber {initial}, 1U, 0, false, -1)
                                .status,
                            robotweax::srt::compat::MessageIoStatus::success);
                    }
                }
                std::array<SRT_SOCKGROUPDATA, 64> data {};
                SRT_MSGCTRL control = srt_msgctrl_default;
                control.grpdata = data.data();
                control.grpdata_size = data.size();
                const char payload = 'n';
                REQUIRE_EQ(srt_sendmsg2(group, &payload, 1, &control), 1);
                REQUIRE_EQ(control.grpdata_size, count);
                REQUIRE_EQ(
                    static_cast<std::uint32_t>(control.pktseq), target.value());
                REQUIRE_EQ(record->next_send_sequence, target.next().value());
                for (unsigned index = 0; index < count; ++index) {
                    REQUIRE_EQ(data[index].id, sockets[index]);
                    REQUIRE_EQ(data[index].sockstate, SRTS_CONNECTED);
                    const bool blocked = congested_prefix && index + 1U < count;
                    const bool successful = type == SRT_GTYPE_BROADCAST
                        ? !blocked
                        : index == (congested_prefix ? count - 1U : 0U);
                    REQUIRE_EQ(data[index].memberstate,
                        successful ? SRT_GST_RUNNING : SRT_GST_IDLE);
                    REQUIRE_EQ(data[index].result,
                        successful ? 1
                                   : (blocked ? SRT_EASYNCSND : SRT_SUCCESS));
                }
                REQUIRE_EQ(srt_close(group), 0);
            }
        }
    }
}

TEST(compat_groups_reject_file_and_disabled_tsbpd_transactionally)
{
    for (const auto type : {SRT_GTYPE_BROADCAST, SRT_GTYPE_BACKUP}) {
        const auto group = srt_create_group(type);
        REQUIRE(group != SRT_INVALID_SOCK);
        const auto file = SRTT_FILE;
        const bool disabled = false;
        REQUIRE_EQ(srt_setsockflag(group, SRTO_TRANSTYPE, &file, sizeof(file)),
            SRT_ERROR);
        REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);
        REQUIRE_EQ(
            srt_setsockflag(group, SRTO_TSBPDMODE, &disabled, sizeof(disabled)),
            SRT_ERROR);
        REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);
        SRT_TRANSTYPE actual = SRTT_FILE;
        int size = sizeof(actual);
        REQUIRE_EQ(srt_getsockflag(group, SRTO_TRANSTYPE, &actual, &size), 0);
        REQUIRE_EQ(actual, SRTT_LIVE);
        bool tsbpd = false;
        size = sizeof(tsbpd);
        REQUIRE_EQ(srt_getsockflag(group, SRTO_TSBPDMODE, &tsbpd, &size), 0);
        REQUIRE(tsbpd);
        REQUIRE_EQ(srt_close(group), 0);
    }
}

TEST(compat_group_closed_member_retains_due_and_future_prefix)
{
    for (const auto type : {SRT_GTYPE_BACKUP, SRT_GTYPE_BROADCAST}) {
        for (const bool rollover : {false, true}) {
            for (const bool tlpktdrop : {false, true}) {
                const auto group = srt_create_group(type);
                const auto old_socket = srt_create_socket();
                const auto replacement = srt_create_socket();
                const auto record = GroupRegistry::instance().find(group);
                REQUIRE(record != nullptr);
                const SequenceNumber initial {
                    rollover ? SequenceNumber::mask : record->initial_sequence};
                {
                    std::lock_guard lock(record->mutex);
                    record->initial_sequence = initial.value();
                    record->next_receive_sequence = initial.value();
                }
                TestClock clock {
                    .channel = std::make_shared<
                        robotweax::srt::compat::DatagramChannel>()};
                clock.channel->set_send_hook_for_testing(
                    accept_test_datagram, nullptr);
                struct Cleanup {
                    SRTSOCKET group;
                    std::array<SRTSOCKET, 2> polls {
                        SRT_INVALID_SOCK, SRT_INVALID_SOCK};
                    ~Cleanup()
                    {
                        for (const auto poll : polls) {
                            if (poll != SRT_INVALID_SOCK) {
                                (void)srt_epoll_release(poll);
                            }
                        }
                        (void)srt_close(group);
                    }
                } cleanup {group};
                const auto origin = ConnectionRuntime::Clock::now();
                auto old = attach_group_runtime(group, old_socket,
                    initial.value(), 1, &clock, 0, true, 100, origin, record,
                    {}, nullptr, nullptr, tlpktdrop);
                const auto inject = [&](const auto& runtime,
                                        std::uint32_t offset) {
                    const std::array<std::byte, 1> payload {
                        static_cast<std::byte>('a' + offset)};
                    robotweax::srt::PacketView packet;
                    packet.kind = robotweax::srt::PacketKind::data;
                    packet.data.sequence = initial.advanced(offset);
                    packet.data.message_number = offset + 1;
                    packet.data.boundary =
                        robotweax::srt::MessageBoundary::solo;
                    packet.data.in_order = true;
                    packet.data.timestamp =
                        robotweax::srt::PacketTimestamp {offset * 1'000U};
                    packet.payload = payload;
                    runtime->process_packet(
                        packet, IpEndpoint::loopback(9'000));
                };
                inject(old, 0);
                inject(old, 1);
                const auto later = attach_group_runtime(group, replacement,
                    initial.advanced(2).value(), 1, &clock, 0, true, 100,
                    origin, record, {}, nullptr, nullptr, tlpktdrop);
                inject(later, 2);
                const auto old_record =
                    SocketRegistry::instance().find(old_socket);
                REQUIRE(old_record != nullptr);
                GroupRegistry::instance().retain_member_receive(group,
                    record->generation + 1, old_socket,
                    old_record->member_generation, old);
                GroupRegistry::instance().retain_member_receive(group,
                    record->generation, old_socket,
                    old_record->member_generation + 1, old);
                REQUIRE(record->retained_receive == nullptr);
                const bool synchronous = false;
                REQUIRE_EQ(srt_setsockflag(group, SRTO_RCVSYN, &synchronous,
                               sizeof(synchronous)),
                    0);
                for (auto& poll : cleanup.polls) {
                    poll = srt_epoll_create();
                    const int flags =
                        SRT_EPOLL_IN | SRT_EPOLL_ERR | SRT_EPOLL_ET;
                    REQUIRE_EQ(srt_epoll_add_usock(poll, group, &flags), 0);
                }
                std::weak_ptr<ConnectionRuntime> old_owner = old;
                REQUIRE_EQ(srt_close(old_socket), 0);
                old.reset();
                REQUIRE(old_owner.expired());
                std::size_t count = 0;
                REQUIRE_EQ(srt_group_data(group, nullptr, &count), 0);
                REQUIRE_EQ(count, 1U);
                std::array<char, 8> buffer {};
                SRT_MSGCTRL control = srt_msgctrl_default;
                REQUIRE_EQ(
                    srt_recvmsg2(group, buffer.data(), buffer.size(), &control),
                    SRT_ERROR);
                REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EASYNCRCV);
                REQUIRE_EQ(record->next_receive_sequence, initial.value());
                for (const auto poll : cleanup.polls) {
                    SRT_EPOLL_EVENT event {};
                    REQUIRE_EQ(srt_epoll_uwait(poll, &event, 1, 0), 0);
                }
                for (std::uint32_t offset = 0; offset < 3; ++offset) {
                    clock.now_microseconds = 100'000U + offset * 1'000U;
                    for (const auto poll : cleanup.polls) {
                        const int flags =
                            SRT_EPOLL_IN | SRT_EPOLL_ERR | SRT_EPOLL_ET;
                        REQUIRE_EQ(
                            srt_epoll_update_usock(poll, group, &flags), 0);
                        SRT_EPOLL_EVENT event {};
                        REQUIRE_EQ(srt_epoll_uwait(poll, &event, 1, 0), 1);
                        REQUIRE_EQ(event.events, SRT_EPOLL_IN);
                    }
                    REQUIRE_EQ(srt_recvmsg2(group, buffer.data(), buffer.size(),
                                   &control),
                        1);
                    REQUIRE_EQ(buffer[0], static_cast<char>('a' + offset));
                    REQUIRE_EQ(control.pktseq,
                        static_cast<std::int32_t>(
                            initial.advanced(offset).value()));
                    REQUIRE_EQ(
                        control.msgno, static_cast<std::int32_t>(offset + 1));
                }
            }
        }
    }
}

TEST(compat_group_receive_racing_member_close_preserves_exact_prefix)
{
    for (unsigned iteration = 0; iteration < 16; ++iteration) {
        const auto group = srt_create_group(SRT_GTYPE_BROADCAST);
        const auto old_socket = srt_create_socket();
        const auto record = GroupRegistry::instance().find(group);
        REQUIRE(record != nullptr);
        const SequenceNumber first {record->initial_sequence};
        TestClock clock {
            .channel =
                std::make_shared<robotweax::srt::compat::DatagramChannel>()};
        clock.channel->set_send_hook_for_testing(accept_test_datagram, nullptr);
        struct Cleanup {
            SRTSOCKET group;
            ~Cleanup()
            {
                (void)srt_close(group);
            }
        } cleanup {group};
        const auto origin = ConnectionRuntime::Clock::now();
        auto old = attach_group_runtime(group, old_socket, first.value(), 1,
            &clock, 0, true, 100, origin, record, {}, nullptr, nullptr, false);
        const auto later = attach_group_runtime(group, srt_create_socket(),
            first.advanced(2).value(), 1, &clock, 0, true, 100, origin, record,
            {}, nullptr, nullptr, false);
        for (std::uint32_t offset = 0; offset < 3; ++offset) {
            const std::array<std::byte, 1> payload {
                static_cast<std::byte>('a' + offset)};
            robotweax::srt::PacketView packet;
            packet.kind = robotweax::srt::PacketKind::data;
            packet.data.sequence = first.advanced(offset);
            packet.data.message_number = offset + 1;
            packet.data.boundary = robotweax::srt::MessageBoundary::solo;
            packet.data.in_order = true;
            packet.payload = payload;
            (offset < 2 ? old : later)
                ->process_packet(packet, IpEndpoint::loopback(9'000));
        }
        clock.now_microseconds = 200'000;
        const int timeout = 1'000;
        REQUIRE_EQ(
            srt_setsockflag(group, SRTO_RCVTIMEO, &timeout, sizeof(timeout)),
            0);
        std::jthread close_member([&] {
            (void)srt_close(old_socket);
        });
        for (unsigned offset = 0; offset < 3; ++offset) {
            std::array<char, 1> output {};
            REQUIRE_EQ(srt_recvmsg(group, output.data(), output.size()), 1);
            REQUIRE_EQ(output[0], static_cast<char>('a' + offset));
        }
        close_member.join();
        old.reset();
        REQUIRE_EQ(record->next_receive_sequence, first.advanced(3).value());
    }
}

TEST(compat_group_closed_fragment_keeps_shared_clock_and_small_buffer_retry)
{
    const auto group = srt_create_group(SRT_GTYPE_BACKUP);
    const auto socket = srt_create_socket();
    const auto record = GroupRegistry::instance().find(group);
    REQUIRE(record != nullptr);
    const SequenceNumber first {record->initial_sequence};
    TestClock clock {
        .channel = std::make_shared<robotweax::srt::compat::DatagramChannel>()};
    clock.channel->set_send_hook_for_testing(accept_test_datagram, nullptr);
    struct Cleanup {
        SRTSOCKET group;
        ~Cleanup()
        {
            (void)srt_close(group);
        }
    } cleanup {group};
    const auto origin = ConnectionRuntime::Clock::now();
    auto runtime = attach_group_runtime(
        group, socket, first.value(), 1, &clock, 0, true, 100, origin, record);
    const auto epoch = runtime->timestamp_origin_microseconds();
    const std::array<std::byte, 2> payload {std::byte {'a'}, std::byte {'b'}};
    for (std::uint32_t offset = 0; offset < 2; ++offset) {
        robotweax::srt::PacketView packet;
        packet.kind = robotweax::srt::PacketKind::data;
        packet.data.sequence = first.advanced(offset);
        packet.data.message_number = 7;
        packet.data.boundary = offset == 0
            ? robotweax::srt::MessageBoundary::first
            : robotweax::srt::MessageBoundary::last;
        packet.data.in_order = true;
        packet.data.timestamp = robotweax::srt::PacketTimestamp {10'000};
        packet.payload = payload;
        runtime->process_packet(packet, IpEndpoint::loopback(9'000));
    }
    REQUIRE_EQ(srt_close(socket), 0);
    runtime.reset();
    // A member added after retirement raises the shared release deadline.
    (void)attach_group_runtime(group, srt_create_socket(),
        first.advanced(2).value(), 1, &clock, 0, true, 300, origin, record);
    const bool synchronous = false;
    REQUIRE_EQ(
        srt_setsockflag(group, SRTO_RCVSYN, &synchronous, sizeof(synchronous)),
        0);
    std::array<char, 4> output {};
    SRT_MSGCTRL control = srt_msgctrl_default;
    clock.now_microseconds = 309'999;
    REQUIRE_EQ(
        srt_recvmsg2(group, output.data(), output.size(), &control), SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EASYNCRCV);
    clock.now_microseconds = 310'000;
    REQUIRE_EQ(srt_recvmsg2(group, output.data(), 3, &control), SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ELARGEMSG);
    REQUIRE_EQ(record->next_receive_sequence, first.value());
    REQUIRE_EQ(srt_recvmsg2(group, output.data(), output.size(), &control), 4);
    REQUIRE_EQ(output, (std::array<char, 4> {'a', 'b', 'a', 'b'}));
    REQUIRE_EQ(control.msgno, 7);
    REQUIRE_EQ(control.pktseq, static_cast<std::int32_t>(first.value()));
    REQUIRE_EQ(control.srctime, epoch + 310'000);
    REQUIRE_EQ(record->next_receive_sequence, first.advanced(2).value());
}

namespace {

void verify_closed_member_bounded_retention(bool oversized)
{
    using robotweax::srt::compat::GroupReceiveRetention;
    const auto bound =
        static_cast<std::uint32_t>(GroupReceiveRetention::maximum_packets);
    {
        const auto group = srt_create_group(SRT_GTYPE_BACKUP);
        const auto socket = srt_create_socket();
        struct Cleanup {
            SRTSOCKET group;
            ~Cleanup()
            {
                (void)srt_close(group);
            }
        };
        const auto record = GroupRegistry::instance().find(group);
        REQUIRE(record != nullptr);
        const SequenceNumber first {record->initial_sequence};
        TestClock clock {
            .channel =
                std::make_shared<robotweax::srt::compat::DatagramChannel>()};
        // Closing members can still consult the injected clock.
        Cleanup cleanup {group};
        clock.channel->set_send_hook_for_testing(accept_test_datagram, nullptr);
        const auto origin = ConnectionRuntime::Clock::now();
        auto runtime = attach_group_runtime(group, socket, first.value(), 1,
            &clock, 0, false, 0, origin, record, {}, nullptr, nullptr, false,
            bound + 2U);
        const std::array payload {std::byte {'x'}};
        const std::array tail {std::byte {'y'}};
        const auto prefix_packets = oversized ? bound + 1U : bound;
        for (std::uint32_t offset = 0; offset <= bound + 1U; ++offset) {
            robotweax::srt::PacketView packet;
            packet.kind = robotweax::srt::PacketKind::data;
            packet.data.sequence = first.advanced(offset);
            packet.data.message_number =
                offset < prefix_packets ? 1U : offset + 1U;
            packet.data.boundary = offset < prefix_packets
                ? (offset == 0U ? robotweax::srt::MessageBoundary::first
                          : offset + 1U == prefix_packets
                          ? robotweax::srt::MessageBoundary::last
                          : robotweax::srt::MessageBoundary::subsequent)
                : robotweax::srt::MessageBoundary::solo;
            packet.data.in_order = true;
            packet.payload = offset == bound + 1U ? tail : payload;
            runtime->process_packet(packet, IpEndpoint::loopback(9'000));
        }
        const auto later = attach_group_runtime(group, srt_create_socket(),
            first.advanced(bound + 2U).value(), 1, &clock, 0, false, 0, origin,
            record);
        REQUIRE_EQ(srt_close(socket), 0);
        runtime.reset();
        const bool synchronous = false;
        REQUIRE_EQ(srt_setsockflag(
                       group, SRTO_RCVSYN, &synchronous, sizeof(synchronous)),
            0);
        REQUIRE_EQ(srt_getsockstate(group), SRTS_CONNECTED);
        std::array<char, GroupReceiveRetention::maximum_packets> output {};
        if (oversized) {
            // One unretainable fragmented message must not erase its tail.
            REQUIRE_EQ(srt_recvmsg(group, output.data(), output.size()), 1);
            REQUIRE_EQ(output[0], 'y');
        } else {
            // A single fragmented message fills the packet budget. Deliver
            // it once instead of repeatedly scanning 8,192 solo messages.
            REQUIRE_EQ(srt_recvmsg(group, output.data(), output.size()),
                static_cast<int>(bound));
            REQUIRE(std::all_of(output.begin(), output.end(), [](char value) {
                return value == 'x';
            }));
        }
        REQUIRE_EQ(record->next_receive_sequence,
            first.advanced(oversized ? bound + 2U : bound).value());
        SRT_TRACEBSTATS statistics {};
        REQUIRE_EQ(srt_bistats(group, &statistics, 0, 1), 0);
        REQUIRE_EQ(statistics.pktRcvDropTotal, oversized ? bound + 1U : 0U);
        robotweax::srt::PacketView next;
        next.kind = robotweax::srt::PacketKind::data;
        next.data.sequence = first.advanced(bound + 2U);
        next.data.message_number = bound + 3U;
        next.data.boundary = robotweax::srt::MessageBoundary::solo;
        const std::array next_payload {std::byte {'z'}};
        next.payload = next_payload;
        later->process_packet(next, IpEndpoint::loopback(9'000));
        REQUIRE_EQ(srt_recvmsg(group, output.data(), output.size()), 1);
        REQUIRE_EQ(output[0], 'z');
        REQUIRE_EQ(
            record->next_receive_sequence, first.advanced(bound + 3U).value());
        REQUIRE_EQ(srt_recvmsg(group, output.data(), output.size()), SRT_ERROR);
        REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EASYNCRCV);
        REQUIRE_EQ(srt_bistats(group, &statistics, 0, 1), 0);
        REQUIRE_EQ(statistics.pktRcvDropTotal, oversized ? bound + 1U : 2U);
    }
}

} // namespace

TEST(compat_group_closed_member_retains_bounded_copy_prefix)
{
    verify_closed_member_bounded_retention(false);
}

TEST(compat_group_closed_member_retains_bounded_copy_after_oversized_message)
{
    verify_closed_member_bounded_retention(true);
}

TEST(compat_group_retention_bounds_drop_oldest_and_deduplicate)
{
    using robotweax::srt::compat::GroupReceiveRetention;
    using robotweax::srt::compat::MessageIoStatus;
    using robotweax::srt::compat::RetainedGroupReceiveBatch;
    const auto batch = [](SequenceNumber first, std::size_t bytes = 1U,
                           std::uint32_t packets = 1U,
                           std::byte fill = std::byte {'x'}) {
        RetainedGroupReceiveBatch result;
        result.origin = ConnectionRuntime::Clock::now();
        robotweax::srt::BufferedMessageCopy message {
            .first_sequence = first,
            .next_sequence = first.advanced(packets),
            .message_number = 1,
        };
        message.payload.resize(bytes, fill);
        result.copies.messages.push_back(std::move(message));
        return result;
    };
    const SequenceNumber first {SequenceNumber::mask - 1U};
    std::array<std::byte, 1> output {};
    {
        // Duplicate ranges consume no additional storage.
        GroupReceiveRetention retained;
        for (std::uint32_t index = 0; index < 32; ++index) {
            retained.retain(batch(first), first);
        }
        REQUIRE_EQ(retained.receive_message(output, first).status,
            MessageIoStatus::success);
        REQUIRE_EQ(output[0], std::byte {'x'});
        REQUIRE_EQ(retained.receive_message(output, first.next()).status,
            MessageIoStatus::would_block);
    }
    for (const bool packet_limit : {false, true}) {
        // A message that can never fit is dropped on its own; the storage
        // stays usable for later prefixes and nothing becomes terminal.
        GroupReceiveRetention retained;
        retained.retain(
            batch(first,
                packet_limit ? 1U : GroupReceiveRetention::maximum_bytes + 1U,
                packet_limit ? GroupReceiveRetention::maximum_packets + 1U
                             : 1U),
            first);
        REQUIRE_EQ(retained.receive_message(output, first).status,
            MessageIoStatus::would_block);
        retained.retain(batch(first.next(), 1U, 1U, std::byte {'y'}), first);
        REQUIRE_EQ(retained.receive_message(output, first.next()).status,
            MessageIoStatus::success);
        REQUIRE_EQ(output[0], std::byte {'y'});
    }
    {
        // The batch bound evicts the oldest retained batch for the newest.
        GroupReceiveRetention retained;
        for (std::uint32_t index = 0;
            index < GroupReceiveRetention::maximum_batches; ++index) {
            retained.retain(batch(first.advanced(index)), first);
        }
        retained.retain(
            batch(first.advanced(GroupReceiveRetention::maximum_batches)),
            first);
        REQUIRE_EQ(retained.receive_message(output, first).status,
            MessageIoStatus::would_block);
        REQUIRE_EQ(retained.receive_message(output, first.next()).status,
            MessageIoStatus::success);
        REQUIRE_EQ(
            retained
                .receive_message(output,
                    first.advanced(GroupReceiveRetention::maximum_batches))
                .status,
            MessageIoStatus::success);
    }
    {
        // The packet bound evicts oldest batches until the newest fits.
        GroupReceiveRetention retained;
        retained.retain(
            batch(first, 1U, GroupReceiveRetention::maximum_packets), first);
        REQUIRE_EQ(retained.receive_message(output, first).status,
            MessageIoStatus::success);
        retained.retain(
            batch(first, 1U, GroupReceiveRetention::maximum_packets), first);
        retained.retain(
            batch(first.advanced(GroupReceiveRetention::maximum_packets), 1U,
                1U, std::byte {'z'}),
            first);
        REQUIRE_EQ(retained.receive_message(output, first).status,
            MessageIoStatus::would_block);
        REQUIRE_EQ(
            retained
                .receive_message(output,
                    first.advanced(GroupReceiveRetention::maximum_packets))
                .status,
            MessageIoStatus::success);
        REQUIRE_EQ(output[0], std::byte {'z'});
    }
}

TEST(compat_group_retained_prefix_expiry_drops_data_and_keeps_group_usable)
{
    using robotweax::srt::compat::GroupReceiveRetention;
    for (const bool expire : {false, true}) {
        const auto group = srt_create_group(SRT_GTYPE_BACKUP);
        const auto socket = srt_create_socket();
        const auto replacement = srt_create_socket();
        const auto record = GroupRegistry::instance().find(group);
        REQUIRE(record != nullptr);
        const SequenceNumber initial {record->initial_sequence};
        TestClock clock {
            .channel =
                std::make_shared<robotweax::srt::compat::DatagramChannel>()};
        clock.channel->set_send_hook_for_testing(accept_test_datagram, nullptr);
        struct Cleanup {
            SRTSOCKET group;
            ~Cleanup()
            {
                (void)srt_close(group);
            }
        } cleanup {group};
        const auto origin = ConnectionRuntime::Clock::now();
        auto runtime = attach_group_runtime(group, socket, initial.value(), 1,
            &clock, 0, true, 100, origin, record);
        const auto inject = [&](const auto& target, std::uint32_t offset,
                                std::byte value) {
            const std::array<std::byte, 1> payload {value};
            robotweax::srt::PacketView packet;
            packet.kind = robotweax::srt::PacketKind::data;
            packet.data.sequence = initial.advanced(offset);
            packet.data.message_number = offset + 1;
            packet.data.boundary = robotweax::srt::MessageBoundary::solo;
            packet.data.timestamp =
                robotweax::srt::PacketTimestamp {offset * 1'000U};
            packet.payload = payload;
            target->process_packet(packet, IpEndpoint::loopback(9'000));
        };
        inject(runtime, 0, std::byte {'x'});
        // A healthy replacement already carries the next message.
        const auto later = attach_group_runtime(group, replacement,
            initial.next().value(), 1, &clock, 8, true, 100, origin, record);
        inject(later, 1, std::byte {'y'});
        REQUIRE_EQ(srt_close(socket), 0);
        runtime.reset();
        const auto retained = record->retained_receive;
        REQUIRE(retained != nullptr);
        const bool synchronous = false;
        REQUIRE_EQ(srt_setsockflag(
                       group, SRTO_RCVSYN, &synchronous, sizeof(synchronous)),
            0);
        const auto poll = srt_epoll_create();
        const int flags = SRT_EPOLL_IN | SRT_EPOLL_ERR;
        REQUIRE_EQ(srt_epoll_add_usock(poll, group, &flags), 0);
        std::array<std::byte, 1> output {};
        if (expire) {
            clock.now_microseconds =
                GroupReceiveRetention::maximum_age_microseconds - 1U;
            // Still retained: the closed member's message is delivered.
            REQUIRE_EQ(srt_getsockstate(group), SRTS_CONNECTED);
            clock.now_microseconds += 1U;
            // Expired: the retained prefix is receiver-side loss, the group
            // stays usable and skips to the replacement's data.
            REQUIRE_EQ(srt_getsockstate(group), SRTS_CONNECTED);
            SRT_EPOLL_EVENT event {};
            REQUIRE_EQ(srt_epoll_uwait(poll, &event, 1, 0), 1);
            REQUIRE((event.events & SRT_EPOLL_ERR) == 0);
            REQUIRE((event.events & SRT_EPOLL_IN) != 0);
            REQUIRE_EQ(
                srt_recvmsg(group, reinterpret_cast<char*>(output.data()),
                    output.size()),
                1);
            REQUIRE_EQ(output[0], std::byte {'y'});
            REQUIRE_EQ(
                record->next_receive_sequence, initial.advanced(2).value());
            SRT_TRACEBSTATS statistics {};
            REQUIRE_EQ(srt_bistats(group, &statistics, 0, 1), 0);
            REQUIRE_EQ(statistics.pktRcvDropTotal, 1);
            // The send path is unaffected by receive-side retention loss.
            if (srt_sendmsg(group, "x", 1, -1, 1) != 1) {
                REQUIRE(srt_getlasterror(nullptr) != SRT_ECONNLOST);
            }
        } else {
            clock.now_microseconds = 200'000;
            REQUIRE_EQ(
                srt_recvmsg(group, reinterpret_cast<char*>(output.data()),
                    output.size()),
                1);
            REQUIRE_EQ(output[0], std::byte {'x'});
            REQUIRE_EQ(
                srt_recvmsg(group, reinterpret_cast<char*>(output.data()),
                    output.size()),
                1);
            REQUIRE_EQ(output[0], std::byte {'y'});
        }
        REQUIRE_EQ(srt_close(group), 0);
        REQUIRE_EQ(retained->receive_message(output, initial).status,
            robotweax::srt::compat::MessageIoStatus::local_closed);
        REQUIRE_EQ(srt_epoll_release(poll), 0);
    }
}

#ifdef ENABLE_MAXREXMITBW
TEST(maxrexmitbw_group_mirror_retains_listener_and_updated_member_template)
{
    REQUIRE_EQ(srt_startup(), 0);
    const auto listener = srt_create_socket();
    std::int64_t limit = 1700;
    REQUIRE_EQ(
        srt_setsockflag(listener, SRTO_MAXREXMITBW, &limit, sizeof(limit)), 0);
    GroupRegistry::MirrorDescription first;
    REQUIRE(GroupRegistry::instance().prepare_mirror(
        listener, SRTGROUP_MASK | 178, SRT_GTYPE_BROADCAST, 700, first));
    REQUIRE_EQ(first.maximum_retransmission_bandwidth_bytes_per_second, limit);
    std::int64_t observed = 0;
    int size = sizeof(observed);
    REQUIRE_EQ(
        srt_getsockflag(first.group, SRTO_MAXREXMITBW, &observed, &size), 0);
    REQUIRE_EQ(observed, limit);
    limit = 0;
    REQUIRE_EQ(
        srt_setsockflag(first.group, SRTO_MAXREXMITBW, &limit, sizeof(limit)),
        0);
    GroupRegistry::MirrorDescription later;
    REQUIRE(GroupRegistry::instance().prepare_mirror(
        listener, SRTGROUP_MASK | 178, SRT_GTYPE_BROADCAST, 701, later));
    REQUIRE_EQ(later.maximum_retransmission_bandwidth_bytes_per_second, 0);
    auto* config = srt_create_config();
    REQUIRE(config != nullptr);
    REQUIRE_EQ(
        srt_config_add(config, SRTO_MAXREXMITBW, &limit, sizeof(limit)), 0);
    REQUIRE_EQ(config->storage.snapshot().options[0].size, sizeof(limit));
    srt_delete_config(config);
    REQUIRE_EQ(srt_close(first.group), 0);
    REQUIRE_EQ(srt_close(listener), 0);
    REQUIRE_EQ(srt_cleanup(), 0);
}
#endif

TEST(compat_group_receive_control_reads_only_metadata_buffer_inputs)
{
    for (const auto type : {SRT_GTYPE_BROADCAST, SRT_GTYPE_BACKUP}) {
        for (const std::size_t capacity : {0U, 1U, 2U}) {
            const auto group = srt_create_group(type);
            REQUIRE(group != SRT_INVALID_SOCK);
            struct Cleanup {
                SRTSOCKET group;
                ~Cleanup()
                {
                    (void)srt_close(group);
                }
            } cleanup {group};
            const auto record = GroupRegistry::instance().find(group);
            REQUIRE(record != nullptr);
            const auto socket = srt_create_socket();
            const auto runtime =
                attach_group_runtime(group, socket, record->initial_sequence);
            constexpr std::array<std::byte, 1> payload {std::byte {'g'}};
            robotweax::srt::PacketView packet;
            packet.kind = robotweax::srt::PacketKind::data;
            packet.data.sequence = SequenceNumber {record->initial_sequence};
            packet.data.message_number = 1;
            packet.data.boundary = robotweax::srt::MessageBoundary::solo;
            packet.payload = payload;
            runtime->process_packet(packet, IpEndpoint::loopback(9'000));
            std::array<SRT_SOCKGROUPDATA, 2> metadata {};
            SRT_MSGCTRL control;
            std::memset(&control, 0xa5, sizeof(control));
            control.grpdata = metadata.data();
            control.grpdata_size = capacity;
            std::array<char, 8> output {};
            REQUIRE_EQ(
                srt_sendmsg2(group, output.data(), 1, &control), SRT_ERROR);
            REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVALMSGAPI);
            REQUIRE_EQ(
                srt_recvmsg2(group, output.data(), output.size(), &control), 1);
            REQUIRE_EQ(output[0], 'g');
            REQUIRE_EQ(control.msgno, 1);
            REQUIRE_EQ(control.pktseq,
                static_cast<std::int32_t>(record->initial_sequence));
            REQUIRE_EQ(control.grpdata_size, 1U);
            if (capacity == 0U) {
                REQUIRE(control.grpdata == nullptr);
            } else {
                REQUIRE(control.grpdata == metadata.data());
                REQUIRE_EQ(metadata[0].id, socket);
            }
        }
    }
}
