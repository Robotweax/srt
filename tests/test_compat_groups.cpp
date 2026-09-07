#include "test.hpp"

#include "compat/group_config.hpp"
#include "compat/group_replay_buffer.hpp"
#include "compat/group_registry.hpp"
#include "compat/socket_registry.hpp"
#include "compat/transport_runtime.hpp"
#include "robotweax/srt/codec.hpp"
#include "robotweax/srt/control.hpp"
#include "srt/srt.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <future>
#include <mutex>
#include <thread>
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
    void* receive_pop_context = nullptr)
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

TEST(compat_group_peer_version_uses_the_first_member_and_group_type_is_socket_only)
{
    const SRTSOCKET group = srt_create_group(SRT_GTYPE_BROADCAST);
    const SRTSOCKET socket = srt_create_socket();
    REQUIRE(group != SRT_INVALID_SOCK);
    REQUIRE(socket != SRT_INVALID_SOCK);

    std::int32_t value = -1;
    int value_size = static_cast<int>(sizeof(value));
    REQUIRE_EQ(srt_getsockflag(
                   group, SRTO_PEERVERSION, &value, &value_size),
        0);
    REQUIRE_EQ(value, 0);

    value_size = static_cast<int>(sizeof(value));
    REQUIRE_EQ(srt_getsockflag(
                   group, SRTO_GROUPTYPE, &value, &value_size),
        SRT_ERROR);
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
    REQUIRE(!GroupRegistry::instance().prepare_mirror(
        first_listener, peer_group, SRT_GTYPE_BROADCAST,
        92U, rejected));

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
    REQUIRE_EQ(srt_close(group), 0);
    cleanup.group = SRT_INVALID_SOCK;
    REQUIRE_EQ(srt_close(mirror), 0);
    cleanup.mirror = SRT_INVALID_SOCK;
    REQUIRE_EQ(srt_close(listener), 0);
    cleanup.listener = SRT_INVALID_SOCK;
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
    REQUIRE_EQ(control.grpdata_size, data.size());
    const auto healthy_data = std::find_if(
        data.begin(), data.end(), [healthy](const auto& member) {
            return member.id == healthy;
        });
    const auto failed_data = std::find_if(
        data.begin(), data.end(), [failed](const auto& member) {
            return member.id == failed;
        });
    REQUIRE(healthy_data != data.end());
    REQUIRE(failed_data != data.end());
    REQUIRE_EQ(healthy_data->memberstate, SRT_GST_RUNNING);
    REQUIRE_EQ(healthy_data->result,
        static_cast<int>(sizeof(payload)));
    REQUIRE_EQ(failed_data->memberstate, SRT_GST_BROKEN);
    REQUIRE(failed_data->sockstate == SRTS_CLOSING
        || failed_data->sockstate == SRTS_CLOSED);

    const auto duplicate_identity = healthy_runtime->queue_group_message(
        std::as_bytes(std::span {payload, sizeof(payload)}),
        SequenceNumber {initial_sequence}, 1, 0, true);
    REQUIRE_EQ(duplicate_identity.status,
        robotweax::srt::compat::MessageIoStatus::invalid_state);
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
    const SRTSOCKET lower_id = srt_create_socket();
    const SRTSOCKET higher_id = srt_create_socket();
    REQUIRE(lower_id < higher_id);
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
    REQUIRE(primary_data != data.end());
    REQUIRE(backup_data != data.end());
    REQUIRE_EQ(primary_data->memberstate, SRT_GST_BROKEN);
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

TEST(compat_group_config_uses_the_complete_v1_5_5_member_option_matrix)
{
    std::vector<SRT_SOCKOPT> allowed = {
        SRTO_BINDTODEVICE,
        SRTO_CONNTIMEO,
        SRTO_DRIFTTRACER,
        SRTO_GROUPMINSTABLETIMEO,
        SRTO_IPTOS,
        SRTO_IPTTL,
        SRTO_KMREFRESHRATE,
        SRTO_KMPREANNOUNCE,
        SRTO_LOSSMAXTTL,
        SRTO_NAKREPORT,
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
