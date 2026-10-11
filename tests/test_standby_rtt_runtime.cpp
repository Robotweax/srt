#include "test.hpp"
#include "compat/group_registry.hpp"
#include "compat/socket_registry.hpp"
#include "compat/transport_runtime.hpp"
#include "compat/random_identity.hpp"
#include "srt/srt.h"
#include <array>
#include <cstring>
#include <memory>
#include <vector>
#include <future>
#include <thread>

using namespace robotweax::srt;
using namespace robotweax::srt::compat;
namespace {
std::uint64_t clock_value(void* context) noexcept
{
    return *static_cast<std::uint64_t*>(context);
}
StandbyRttIdentity identity()
{
    StandbyRttIdentity value;
    value.caller.fill(std::byte {1});
    value.listener.fill(std::byte {2});
    return value;
}
struct Fixture {
    std::uint64_t now = 1'000;
    std::uint64_t send_delay = 0;
    Error send_error = Error::none;
    std::vector<StandbyRttMessage> sent;
    std::shared_ptr<StandbyRttService> service;
    std::shared_ptr<DatagramChannel> channel =
        std::make_shared<DatagramChannel>();
    std::shared_ptr<GroupRecord> group = std::make_shared<GroupRecord>();
    std::shared_ptr<ConnectionRuntime> runtime;
    IpEndpoint peer = IpEndpoint::loopback(9022);
    Fixture(std::shared_ptr<StandbyRttService> shared = {}, bool caller = true)
    {
        service = shared
            ? shared
            : std::make_shared<StandbyRttService>(clock_value, &now);
        group->type = SRT_GTYPE_BACKUP;
        channel->set_send_hook_for_testing(
            [](std::span<const std::byte> bytes, IpEndpoint,
                void* context) noexcept {
                auto& f = *static_cast<Fixture*>(context);
                const auto decoded = decode_standby_rtt_datagram(bytes);
                if (decoded) {
                    f.sent.push_back(decoded.message);
                    f.now += f.send_delay;
                    if (f.send_error != Error::none)
                        return UdpIoResult {.error = f.send_error};
                }
                return UdpIoResult {.bytes_transferred = bytes.size()};
            },
            this);
        runtime = std::make_shared<ConnectionRuntime>(
            ConnectionRuntime::Configuration {.channel = channel,
                .group = group,
                .peer = peer,
                .local_socket_id = 66,
                .peer_socket_id = 77,
                .initial_sequence = SequenceNumber {900},
                .standby_rtt_identity = identity(),
                .standby_rtt_caller = caller,
                .standby_rtt_service_for_testing = service,
                .origin = ConnectionRuntime::Clock::now(),
                .now_function = clock_value,
                .now_context = &now});
    }
    void deliver(StandbyRttMessage message, std::uint32_t destination = 66,
        std::optional<IpEndpoint> endpoint = std::nullopt)
    {
        std::array<std::byte, 64> bytes {};
        REQUIRE(encode_standby_rtt_datagram(
            message, PacketTimestamp {999}, destination, bytes));
        const auto decoded = decode_packet(bytes);
        REQUIRE(decoded);
        runtime->process_packet(decoded.packet, endpoint.value_or(peer));
    }
    StandbyRttMessage echo() const
    {
        REQUIRE(!sent.empty());
        auto message = sent.front();
        message.kind = StandbyRttKind::echo;
        return message;
    }
    StandbyRttMessage request() const
    {
        return {.identity = identity(),
            .probe_id = 100,
            .kind = StandbyRttKind::request,
            .caller_initiated = false};
    }
};
}

TEST(compat_runtime_standby_native_acceptance_and_separate_snapshot)
{
    Fixture f;
    const auto standard = f.runtime->rtt_data();
    const auto health = f.runtime->response_health();
    f.send_delay = 1'000;
    (void)f.runtime->poll();
    REQUIRE_EQ(f.sent.size(), 1U);
    REQUIRE_EQ(f.now, 2'000U);
    f.now = 5'000;
    f.deliver(f.echo());
    auto data = f.runtime->standby_rtt_data();
    REQUIRE_EQ(data.flags,
        ROBOTWEAX_SRT_STANDBY_RTT_NEGOTIATED
            | ROBOTWEAX_SRT_STANDBY_RTT_ADMITTED
            | ROBOTWEAX_SRT_STANDBY_RTT_OBSERVED
            | ROBOTWEAX_SRT_STANDBY_RTT_FRESH);
    REQUIRE_EQ(data.rtt_microseconds, 3'000U);
    REQUIRE_EQ(data.age_microseconds, 0U);
    REQUIRE_EQ(data.samples, 1U);
    f.deliver(f.echo());
    REQUIRE_EQ(f.runtime->standby_rtt_data().samples, 1U);
    const auto after = f.runtime->rtt_data();
    REQUIRE_EQ(std::memcmp(&standard, &after, sizeof(after)), 0);
    const auto response = f.runtime->response_health();
    REQUIRE_EQ(
        response.last_response_microseconds, health.last_response_microseconds);
    REQUIRE_EQ(response.next_send_sequence, health.next_send_sequence);
    REQUIRE_EQ(f.group->send_authority_revision, 0U);
    f.now += 3'000'001;
    data = f.runtime->standby_rtt_data();
    REQUIRE((data.flags & ROBOTWEAX_SRT_STANDBY_RTT_FRESH) == 0);
    REQUIRE_EQ(data.rtt_microseconds, 3'000U);
}
TEST(compat_runtime_standby_endpoint_socket_nonce_direction_and_shape_admission)
{
    Fixture f;
    (void)f.runtime->poll();
    f.now = 2'000;
    auto echo = f.echo();
    f.deliver(echo, 66, IpEndpoint::loopback(9023));
    f.deliver(echo, 67);
    auto bad = echo;
    bad.identity.listener[0] = std::byte {3};
    f.deliver(bad);
    bad = echo;
    bad.caller_initiated = false;
    f.deliver(bad);
    bad = echo;
    ++bad.probe_id;
    f.deliver(bad);
    for (std::size_t length = 0; length != 49; ++length) {
        std::array<std::byte, 49> payload {};
        f.runtime->process_packet(
            {.kind = PacketKind::control,
                .control = {.type = ControlType::user_defined,
                    .subtype = standby_rtt_subtype,
                    .destination_socket_id = 66},
                .payload = std::span {payload}.first(length)},
            f.peer);
    }
    REQUIRE_EQ(f.runtime->standby_rtt_data().samples, 0U);
    f.deliver(echo);
    REQUIRE_EQ(f.runtime->standby_rtt_data().samples, 1U);
}
TEST(compat_runtime_standby_backpressure_skips_without_queue_or_retry)
{
    Fixture f;
    f.send_error = Error::would_block;
    (void)f.runtime->poll();
    REQUIRE_EQ(f.sent.size(), 1U);
    REQUIRE(!f.runtime->broken());
    REQUIRE_EQ(f.runtime->standby_rtt_data().outcome,
        ROBOTWEAX_SRT_STANDBY_RTT_SEND_SKIPPED);
    f.now += 10'000;
    f.send_error = Error::none;
    (void)f.runtime->poll();
    REQUIRE_EQ(f.sent.size(), 1U);
    f.deliver(f.echo());
    REQUIRE_EQ(f.runtime->standby_rtt_data().samples, 0U);
    std::size_t budget = 0;
    (void)f.runtime->poll(budget);
    REQUIRE_EQ(f.sent.size(), 1U);
}
TEST(
    compat_runtime_standby_pauses_for_sender_probe_data_and_metadata_contention)
{
    Fixture f;
    f.group->active_send_member = 66;
    (void)f.runtime->poll();
    REQUIRE(f.sent.empty());
    f.group->active_send_member = SRT_INVALID_SOCK;
    f.group->probe_send_member = 66;
    (void)f.runtime->poll();
    REQUIRE(f.sent.empty());
    f.group->probe_send_member = SRT_INVALID_SOCK;
    {
        std::promise<void> locked, release;
        auto proceed = release.get_future();
        auto worker = std::async(std::launch::async, [&] {
            std::lock_guard lock(f.group->mutex);
            locked.set_value();
            proceed.wait();
        });
        locked.get_future().wait();
        (void)f.runtime->poll();
        const bool suppressed = f.sent.empty();
        release.set_value();
        worker.get();
        REQUIRE(suppressed);
    }
    (void)f.runtime->poll();
    REQUIRE_EQ(f.sent.size(), 1U);
    f.now += 1'000;
    f.group->active_send_member = 66;
    f.deliver(f.echo());
    REQUIRE_EQ(f.runtime->standby_rtt_data().samples, 0U);
    const std::array<std::byte, 1> payload {std::byte {42}};
    REQUIRE_EQ(f.runtime->queue_message(payload, 0, true, false, -1).status,
        MessageIoStatus::success);
    f.group->active_send_member = SRT_INVALID_SOCK;
    f.now += 1'100'000;
    (void)f.runtime->poll();
    REQUIRE_EQ(f.sent.size(), 1U);
}
TEST(compat_runtime_standby_control_only_echoes_cannot_extend_idle_or_authority)
{
    Fixture f;
    const auto standard = f.runtime->rtt_data();
    for (std::uint64_t step = 1; step <= 4; ++step) {
        f.now = step * 1'000'000;
        f.deliver(f.request());
        REQUIRE_EQ(f.sent.size(), static_cast<std::size_t>(step));
        for (int duplicate = 0; duplicate != 100; ++duplicate)
            f.deliver(f.request());
        REQUIRE_EQ(f.sent.size(), static_cast<std::size_t>(step));
        REQUIRE_EQ(f.runtime->response_health().last_response_microseconds, 0U);
    }
    const auto after = f.runtime->rtt_data();
    REQUIRE_EQ(std::memcmp(&standard, &after, sizeof(after)), 0);
    REQUIRE_EQ(f.group->send_authority_revision, 0U);
    f.now = 5'000'001;
    (void)f.runtime->poll();
    REQUIRE(f.runtime->broken());
    REQUIRE((f.runtime->standby_rtt_data().flags
                & ROBOTWEAX_SRT_STANDBY_RTT_TERMINAL)
        != 0);
    f.deliver(f.request());
    REQUIRE_EQ(f.sent.size(), 4U);
}
TEST(compat_runtime_standby_loss_idle_deadline_and_terminal_history)
{
    Fixture f;
    (void)f.runtime->poll();
    f.now = 2'001'000;
    (void)f.runtime->poll();
    // Expired loss may start the next bounded attempt; never fabricates a sample.
    REQUIRE_EQ(f.runtime->standby_rtt_data().samples, 0U);
    REQUIRE_EQ(f.sent.size(), 2U);
    f.now += 1'000;
    auto echo = f.sent.back();
    echo.kind = StandbyRttKind::echo;
    f.deliver(echo);
    REQUIRE_EQ(f.runtime->standby_rtt_data().samples, 1U);
    f.runtime->mark_broken(0);
    const auto sample = f.runtime->standby_rtt_data();
    f.now += 3'000'001;
    const auto stale = f.runtime->standby_rtt_data();
    REQUIRE_EQ(stale.samples, sample.samples);
    REQUIRE_EQ(stale.age_microseconds, sample.age_microseconds + 3'000'001);
    f.deliver(f.request());
    REQUIRE_EQ(f.sent.size(), 2U);
    Fixture boundary;
    boundary.now = 4'999'500;
    (void)boundary.runtime->poll();
    REQUIRE_EQ(boundary.sent.size(), 1U);
    boundary.now = 5'000'000;
    boundary.deliver(boundary.echo());
    REQUIRE_EQ(boundary.runtime->standby_rtt_data().samples, 0U);
}
TEST(
    compat_runtime_standby_process_capacity_retained_terminal_and_generation_reuse)
{
    Fixture f;
    std::vector<StandbyRttService::Lease> leases;
    for (int i = 0; i != 255; ++i) {
        const auto lease = f.service->admit();
        REQUIRE(lease);
        leases.push_back(*lease);
    }
    REQUIRE(!f.service->admit());
    Fixture overflow(f.service);
    REQUIRE_EQ(overflow.runtime->standby_rtt_data().flags,
        ROBOTWEAX_SRT_STANDBY_RTT_NEGOTIATED);
    REQUIRE_EQ(overflow.runtime->standby_rtt_data().outcome,
        ROBOTWEAX_SRT_STANDBY_RTT_CAPACITY);
    (void)overflow.runtime->poll();
    REQUIRE(overflow.sent.empty());
    f.runtime->mark_broken(0);
    REQUIRE(!f.service
            ->admit()); // retained history remains inside the capacity bound
    f.runtime.reset();
    const auto fresh = f.service->admit();
    REQUIRE(fresh);
    const auto old = leases.back();
    f.service->release(old);
    leases.pop_back();
    const auto replacement = f.service->admit();
    REQUIRE(replacement);
    REQUIRE(old.generation != replacement->generation);
    f.service->release(old);
    REQUIRE(!f.service->admit());
    for (auto lease : leases)
        f.service->release(lease);
    f.service->clear();
    REQUIRE(!f.service->echo(old));
    REQUIRE(!f.service->request(old, true));
}
TEST(
    compat_runtime_standby_service_round_robin_delayed_claim_and_cleanup_budget)
{
    std::uint64_t now = 1'000;
    StandbyRttService service(clock_value, &now);
    std::array<StandbyRttService::Lease, 256> leases;
    for (auto& lease : leases) {
        const auto allocated = service.admit();
        REQUIRE(allocated);
        lease = *allocated;
    }
    // Populate due eligibility while the token is occupied.
    REQUIRE(service.request(leases.front(), true));
    for (std::size_t i = 1; i < leases.size(); ++i)
        REQUIRE(!service.request(leases[i], true));
    for (std::size_t i = 1; i < leases.size(); ++i) {
        now += 31'250;
        // A different owner reserves the next fair lease, without sending it.
        REQUIRE(!service.request(leases.front(), true));
        REQUIRE(service.request(leases[i], true));
        REQUIRE(!service.request(leases[i], true));
    }
    now += 31'250;
    REQUIRE(!service.request(leases.back(), true)); // reserves slot0
    now += 100'000;
    REQUIRE(service.request(leases.front(), true));
    REQUIRE(!service.request(
        leases[1], true)); // delayed claim must not create a burst
    REQUIRE(service.echo(leases.front()));
    service.clear();
    const auto fresh = service.admit();
    REQUIRE(fresh);
    REQUIRE(!service.request(*fresh, true));
    REQUIRE(!service.echo(*fresh));
}
TEST(compat_runtime_standby_listener_direction_and_echo_global_budget)
{
    Fixture listener({}, false);
    (void)listener.runtime->poll();
    REQUIRE_EQ(listener.sent.size(), 1U);
    REQUIRE(!listener.sent.front().caller_initiated);
    listener.now += 1'000;
    listener.deliver(listener.echo());
    REQUIRE_EQ(listener.runtime->standby_rtt_data().samples, 1U);
    Fixture caller(listener.service);
    auto request = caller.request();
    caller.deliver(request);
    REQUIRE_EQ(caller.sent.size(), 1U);
    request.caller_initiated = true;
    listener.deliver(request);
    REQUIRE_EQ(listener.sent.size(),
        1U); // independent member bound, shared echo budget
}
TEST(compat_runtime_standby_public_snapshot_validation_disabled_and_lifecycle)
{
    REQUIRE_EQ(sizeof(ROBOTWEAX_SRT_STANDBY_RTTDATA_V1), 48U);
    REQUIRE_EQ(sizeof(ROBOTWEAX_SRT_RTTDATA_V1), 48U);
    REQUIRE_EQ(srt_startup(), 0);
    const auto socket = srt_create_socket();
    REQUIRE(socket != SRT_INVALID_SOCK);
    const auto record = SocketRegistry::instance().find(socket);
    REQUIRE(record);
    ROBOTWEAX_SRT_STANDBY_RTTDATA_V1 data {};
    data.struct_size = sizeof(data);
    data.abi_version = 1;
    data.flags = 0xdead;
    const auto before = data;
    REQUIRE_EQ(
        robotweax_srt_standby_rtt_data_v1(socket, &data, sizeof(data) - 1),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);
    REQUIRE_EQ(std::memcmp(&data, &before, sizeof(data)), 0);
    REQUIRE_EQ(robotweax_srt_standby_rtt_data_v1(socket, &data, sizeof(data)),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ENOCONN);
    auto disabled =
        std::make_shared<ConnectionRuntime>(ConnectionRuntime::Configuration {
            .origin = ConnectionRuntime::Clock::now()});
    {
        std::lock_guard lock(record->mutex);
        record->runtime = disabled;
        record->state = SRTS_CONNECTED;
    }
    REQUIRE_EQ(
        robotweax_srt_standby_rtt_data_v1(socket, &data, sizeof(data)), 0);
    REQUIRE_EQ(data.flags, 0U);
    REQUIRE_EQ(data.samples, 0U);
    REQUIRE_EQ(data.age_microseconds, UINT64_MAX);
    data = before;
    for (auto bad_size : {0U, 47U, 49U}) {
        data.struct_size = bad_size;
        const auto invalid = data;
        REQUIRE_EQ(
            robotweax_srt_standby_rtt_data_v1(socket, &data, sizeof(data)),
            SRT_ERROR);
        REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);
        REQUIRE_EQ(std::memcmp(&data, &invalid, sizeof(data)), 0);
    }
    data = before;
    data.abi_version = 2;
    REQUIRE_EQ(robotweax_srt_standby_rtt_data_v1(socket, &data, sizeof(data)),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);
    REQUIRE_EQ(robotweax_srt_standby_rtt_data_v1(socket, nullptr, sizeof(data)),
        SRT_ERROR);
    data = before;
    const auto group = srt_create_group(SRT_GTYPE_BACKUP);
    REQUIRE_EQ(robotweax_srt_standby_rtt_data_v1(group, &data, sizeof(data)),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVSOCK);
    REQUIRE_EQ(srt_close(group), 0);
    Fixture f;
    {
        std::lock_guard lock(record->mutex);
        record->runtime = f.runtime;
        record->state = SRTS_CONNECTED;
    }
    struct Larger {
        ROBOTWEAX_SRT_STANDBY_RTTDATA_V1 data;
        std::uint64_t tail;
    } larger {before, 0x12345678};
    larger.data.struct_size = sizeof(larger);
    REQUIRE_EQ(
        robotweax_srt_standby_rtt_data_v1(socket, &larger.data, sizeof(larger)),
        0);
    REQUIRE_EQ(larger.tail, 0x12345678U);
    REQUIRE_EQ(larger.data.samples, 0U);
    REQUIRE_EQ(larger.data.age_microseconds, UINT64_MAX);
    f.runtime->mark_broken(0);
    {
        std::lock_guard lock(record->mutex);
        record->state = SRTS_BROKEN;
    }
    REQUIRE_EQ(
        robotweax_srt_standby_rtt_data_v1(socket, &data, sizeof(data)), 0);
    REQUIRE((data.flags & ROBOTWEAX_SRT_STANDBY_RTT_TERMINAL) != 0);
    REQUIRE_EQ(srt_close(socket), 0);
    REQUIRE_EQ(robotweax_srt_standby_rtt_data_v1(socket, &data, sizeof(data)),
        SRT_ERROR);
    REQUIRE_EQ(srt_cleanup(), 0);
}

namespace {
struct PublicSockets {
    SRTSOCKET listener = SRT_INVALID_SOCK, caller = SRT_INVALID_SOCK,
              receiver = SRT_INVALID_SOCK;
    ~PublicSockets()
    {
        if (caller != SRT_INVALID_SOCK)
            (void)srt_close(caller);
        if (receiver != SRT_INVALID_SOCK)
            (void)srt_close(receiver);
        if (listener != SRT_INVALID_SOCK)
            (void)srt_close(listener);
        (void)srt_cleanup();
    }
};
void enable_option(SRTSOCKET socket, SRT_SOCKOPT option, bool value = true)
{
    REQUIRE_EQ(srt_setsockflag(socket, option, &value, sizeof(value)), 0);
}
sockaddr_in listen_on_loopback(PublicSockets& sockets)
{
    sockaddr_in endpoint {};
    endpoint.sin_family = AF_INET;
    endpoint.sin_addr.s_addr = htonl(0x7f000001);
    REQUIRE_EQ(srt_bind(sockets.listener,
                   reinterpret_cast<sockaddr*>(&endpoint), sizeof(endpoint)),
        0);
    REQUIRE_EQ(srt_listen(sockets.listener, 8), 0);
    int size = sizeof(endpoint);
    REQUIRE_EQ(srt_getsockname(sockets.listener,
                   reinterpret_cast<sockaddr*>(&endpoint), &size),
        0);
    return endpoint;
}
}
TEST(
    standby_rtt_native_runtime_bilateral_plain_encrypted_and_session_authenticated_groups)
{
    for (int profile = 0; profile != 4; ++profile) {
#ifndef ENABLE_AEAD_API_PREVIEW
        if (profile == 3)
            continue;
#endif
        REQUIRE_EQ(srt_startup(), 0);
        PublicSockets sockets;
        sockets.listener = srt_create_socket();
        sockets.caller = srt_create_group(SRT_GTYPE_BACKUP);
        enable_option(sockets.listener, SRTO_GROUPCONNECT);
        for (auto socket : {sockets.caller, sockets.listener}) {
            enable_option(socket, SRTO_ROBOTWEAX_STANDBYRTT_REQUIRED);
            const int timeout = 2'000;
            REQUIRE_EQ(srt_setsockflag(
                           socket, SRTO_RCVTIMEO, &timeout, sizeof(timeout)),
                0);
            if (profile != 0) {
                constexpr char passphrase[] = "standby-rtt-test";
                REQUIRE_EQ(srt_setsockflag(socket, SRTO_PASSPHRASE, passphrase,
                               sizeof(passphrase) - 1),
                    0);
                if (profile == 2)
                    enable_option(socket, SRTO_ROBOTWEAX_SESSIONAUTH);
#ifdef ENABLE_AEAD_API_PREVIEW
                if (profile == 3) {
                    const int mode = 2;
                    REQUIRE_EQ(srt_setsockflag(socket, SRTO_CRYPTOMODE, &mode,
                                   sizeof(mode)),
                        0);
                }
#endif
            }
        }
        auto target = listen_on_loopback(sockets);
        std::array<SRT_SOCKGROUPCONFIG, 2> endpoints;
        for (auto& endpoint : endpoints)
            endpoint = srt_prepare_endpoint(
                nullptr, reinterpret_cast<sockaddr*>(&target), sizeof(target));
        REQUIRE(srt_connect_group(
                    sockets.caller, endpoints.data(), endpoints.size())
            != SRT_INVALID_SOCK);
        sockets.receiver = srt_accept(sockets.listener, nullptr, nullptr);
        REQUIRE(sockets.receiver != SRT_INVALID_SOCK);
        std::array<SRT_SOCKGROUPDATA, 2> caller {}, listener {};
        // Four members share a burst-one echo budget. Allow three 2s probe
        // attempts plus scheduling slack when admission drops a healthy echo.
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds {7};
        bool measured = false;
        do {
            std::size_t local_count = caller.size(),
                        remote_count = listener.size();
            REQUIRE(srt_group_data(sockets.caller, caller.data(), &local_count)
                >= 0);
            REQUIRE(
                srt_group_data(sockets.receiver, listener.data(), &remote_count)
                >= 0);
            measured = local_count == 2 && remote_count == 2;
            if (measured)
                for (const auto* members : {&caller, &listener})
                    for (const auto& member : *members) {
                        if (member.sockstate != SRTS_CONNECTED) {
                            measured = false;
                            continue;
                        }
                        ROBOTWEAX_SRT_STANDBY_RTTDATA_V1 data {};
                        data.struct_size = sizeof(data);
                        data.abi_version = 1;
                        const auto result = robotweax_srt_standby_rtt_data_v1(
                            member.id, &data, sizeof(data));
                        if (result == SRT_ERROR) {
                            REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ENOCONN);
                            measured = false;
                            continue;
                        }
                        measured &= data.samples != 0
                            && data.rtt_microseconds != 0
                            && data.rtt_microseconds < 2'000'000
                            && data.age_microseconds != UINT64_MAX;
                        REQUIRE_EQ(
                            (data.flags & ROBOTWEAX_SRT_STANDBY_RTT_FRESH) != 0,
                            data.samples != 0
                                && data.age_microseconds <= 3'000'000);
                    }
            if (!measured)
                std::this_thread::sleep_for(std::chrono::milliseconds {1});
        } while (!measured && std::chrono::steady_clock::now() < deadline);
        REQUIRE(measured);
        const bool disable = false;
        REQUIRE_EQ(
            srt_setsockflag(sockets.caller, SRTO_ROBOTWEAX_STANDBYRTT_REQUIRED,
                &disable, sizeof(disable)),
            SRT_ERROR);
        REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ECONNSOCK);
        for (const auto& member : caller) {
            ROBOTWEAX_SRT_RTTDATA_V1 standard {};
            standard.struct_size = sizeof(standard);
            standard.abi_version = 1;
            REQUIRE_EQ(robotweax_srt_rtt_data_v1(
                           member.id, &standard, sizeof(standard)),
                0);
            REQUIRE_EQ(standard.flags, 0U);
        }
        ROBOTWEAX_SRT_GROUP_SEND_STATE_V1 authority {};
        authority.struct_size = sizeof(authority);
        authority.abi_version = 1;
        REQUIRE_EQ(robotweax_srt_group_send_state_v1(
                       sockets.caller, &authority, sizeof(authority)),
            0);
        REQUIRE_EQ(authority.active_member, SRT_INVALID_SOCK);
        constexpr char message[] = "standby-data";
        REQUIRE_EQ(srt_sendmsg(sockets.caller, message, sizeof(message), -1, 1),
            static_cast<int>(sizeof(message)));
        std::array<char, 64> received {};
        REQUIRE_EQ(
            srt_recvmsg(sockets.receiver, received.data(), received.size()),
            static_cast<int>(sizeof(message)));
        REQUIRE_EQ(std::memcmp(received.data(), message, sizeof(message)), 0);
    }
}
TEST(standby_rtt_native_runtime_one_sided_opt_in_rejects_without_downgrade)
{
    for (bool listener_required : {false, true}) {
        REQUIRE_EQ(srt_startup(), 0);
        PublicSockets sockets;
        sockets.listener = srt_create_socket();
        sockets.caller = srt_create_group(SRT_GTYPE_BACKUP);
        enable_option(sockets.listener, SRTO_GROUPCONNECT);
        enable_option(listener_required ? sockets.listener : sockets.caller,
            SRTO_ROBOTWEAX_STANDBYRTT_REQUIRED);
        auto target = listen_on_loopback(sockets);
        auto endpoint = srt_prepare_endpoint(
            nullptr, reinterpret_cast<sockaddr*>(&target), sizeof(target));
        REQUIRE_EQ(
            srt_connect_group(sockets.caller, &endpoint, 1), SRT_INVALID_SOCK);
        REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ECONNSETUP);
        REQUIRE_EQ(srt_getrejectreason(endpoint.id), SRT_REJ_GROUP);
    }
}
TEST(standby_rtt_public_option_profiles_and_preconnect_boundary)
{
    REQUIRE_EQ(srt_startup(), 0);
    PublicSockets sockets;
    sockets.listener = srt_create_socket();
    sockets.caller = srt_create_group(SRT_GTYPE_BROADCAST);
    bool value = false;
    int size = sizeof(value);
    REQUIRE_EQ(srt_getsockflag(sockets.listener,
                   SRTO_ROBOTWEAX_STANDBYRTT_REQUIRED, &value, &size),
        0);
    REQUIRE(!value);
    value = true;
    REQUIRE_EQ(srt_setsockflag(sockets.caller,
                   SRTO_ROBOTWEAX_STANDBYRTT_REQUIRED, &value, sizeof(value)),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVOP);
    enable_option(sockets.listener, SRTO_ROBOTWEAX_STANDBYRTT_REQUIRED);
    value = false;
    for (auto option : {SRTO_TSBPDMODE, SRTO_MESSAGEAPI})
        REQUIRE_EQ(
            srt_setsockflag(sockets.listener, option, &value, sizeof(value)),
            SRT_ERROR);
    value = true;
    REQUIRE_EQ(srt_setsockflag(
                   sockets.listener, SRTO_RENDEZVOUS, &value, sizeof(value)),
        SRT_ERROR);
    const int file = SRTT_FILE;
    REQUIRE_EQ(
        srt_setsockflag(sockets.listener, SRTO_TRANSTYPE, &file, sizeof(file)),
        SRT_ERROR);
    enable_option(sockets.listener, SRTO_ROBOTWEAX_STANDBYRTT_REQUIRED, false);
    REQUIRE_EQ(
        srt_setsockflag(sockets.listener, SRTO_TRANSTYPE, &file, sizeof(file)),
        0);
    REQUIRE_EQ(srt_setsockflag(sockets.listener,
                   SRTO_ROBOTWEAX_STANDBYRTT_REQUIRED, &value, sizeof(value)),
        SRT_ERROR);
    REQUIRE(prepare_random_identity());
    const auto first = random_standby_rtt_nonce(),
               second = random_standby_rtt_nonce();
    REQUIRE(first && second);
    REQUIRE(valid_standby_rtt_nonce(*first));
    REQUIRE(*first != *second);
}
TEST(compat_runtime_standby_hard_native_error_and_explicit_close_cancel)
{
    Fixture broken;
    broken.send_error = Error::io_error;
    (void)broken.runtime->poll();
    REQUIRE(broken.runtime->broken());
    broken.deliver(broken.request());
    REQUIRE_EQ(broken.sent.size(), 1U);
    Fixture closed;
    (void)closed.runtime->poll();
    closed.runtime->close();
    closed.deliver(closed.request());
    closed.deliver(closed.echo());
    (void)closed.runtime->poll();
    REQUIRE_EQ(closed.sent.size(), 1U);
    REQUIRE_EQ(closed.runtime->standby_rtt_data().samples, 0U);
}
TEST(
    compat_runtime_standby_received_data_pauses_and_idle_responder_has_deadline)
{
    Fixture f;
    const std::array<std::byte, 1> payload {std::byte {42}};
    f.runtime->process_packet({.kind = PacketKind::data,
                                  .data = {.sequence = SequenceNumber {900},
                                      .message_number = 1,
                                      .boundary = MessageBoundary::solo,
                                      .in_order = true,
                                      .destination_socket_id = 66},
                                  .payload = payload},
        f.peer);
    REQUIRE(f.runtime->has_received_data());
    (void)f.runtime->poll();
    REQUIRE(f.sent.empty());
    f.now += 999'999;
    (void)f.runtime->poll();
    REQUIRE(f.sent.empty());
    ++f.now;
    (void)f.runtime->poll();
    REQUIRE_EQ(f.sent.size(), 1U);
    Fixture expired;
    expired.now = 5'000'000;
    expired.deliver(expired.request());
    REQUIRE(expired.sent.empty());
}
TEST(
    compat_runtime_standby_established_listener_replays_offer_with_original_pair)
{
    auto channel = std::make_shared<DatagramChannel>();
    std::vector<std::byte> response;
    channel->set_send_hook_for_testing(
        [](std::span<const std::byte> bytes, IpEndpoint,
            void* context) noexcept {
            static_cast<std::vector<std::byte>*>(context)->assign(
                bytes.begin(), bytes.end());
            return UdpIoResult {.bytes_transferred = bytes.size()};
        },
        &response);
    std::uint64_t now = 1'000;
    const auto peer = IpEndpoint::loopback(9022);
    HandshakeAction replay;
    replay.kind = HandshakeActionKind::send;
    replay.packet = {.version = 5,
        .extension_field = 5,
        .initial_sequence = SequenceNumber {900},
        .maximum_transmission_unit = 1500,
        .flow_window = 256,
        .request = HandshakeRequest::conclusion,
        .socket_id = 66,
        .syn_cookie = 100};
    replay.has_handshake_extension = true;
    replay.extension_type = HandshakeExtensionType::handshake_response;
    replay.has_standby_rtt = true;
    replay.standby_rtt = identity();
    ConnectionRuntime runtime({.channel = channel,
        .peer = peer,
        .peer_socket_id = 77,
        .initial_sequence = SequenceNumber {900},
        .origin = ConnectionRuntime::Clock::now(),
        .handshake_replay_response = replay,
        .handshake_replay_enabled = true,
        .now_function = clock_value,
        .now_context = &now});
    HandshakeMessage offer;
    offer.packet.request = HandshakeRequest::conclusion;
    offer.packet.socket_id = 77;
    offer.packet.syn_cookie = 100;
    offer.has_standby_rtt = true;
    offer.standby_rtt.caller = identity().caller;
    REQUIRE(runtime.process_handshake(offer, peer));
    const auto decoded = decode_handshake_datagram(response);
    REQUIRE(decoded);
    REQUIRE_EQ(decoded.message.standby_rtt, identity());
    auto changed = offer;
    changed.standby_rtt.caller[0] = std::byte {3};
    response.clear();
    REQUIRE(!runtime.process_handshake(changed, peer));
    REQUIRE(response.empty());
    changed = offer;
    changed.standby_rtt = identity();
    REQUIRE(!runtime.process_handshake(changed, peer));
}
TEST(compat_runtime_standby_native_send_lock_contention_skips_without_wait)
{
    Fixture f;
    std::promise<void> locked, release;
    auto proceed = release.get_future();
    struct Context {
        std::promise<void>* locked;
        std::future<void>* release;
    } context {&locked, &proceed};
    f.channel->set_send_hook_for_testing(
        [](std::span<const std::byte> bytes, IpEndpoint,
            void* opaque) noexcept {
            auto& value = *static_cast<Context*>(opaque);
            value.locked->set_value();
            value.release->wait();
            return UdpIoResult {.bytes_transferred = bytes.size()};
        },
        &context);
    const std::array<std::byte, 1> marker {};
    auto writer = std::async(std::launch::async, [&] {
        return f.channel->send_datagram(marker, f.peer);
    });
    locked.get_future().wait();
    (void)f.runtime->poll();
    const auto outcome = f.runtime->standby_rtt_data().outcome;
    release.set_value();
    REQUIRE(writer.get());
    REQUIRE_EQ(outcome, ROBOTWEAX_SRT_STANDBY_RTT_SEND_SKIPPED);
    REQUIRE(f.sent.empty());
    f.channel->set_send_hook_for_testing(nullptr, nullptr);
}
