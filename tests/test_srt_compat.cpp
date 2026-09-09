#include "test.hpp"

#include "robotweax/srt/codec.hpp"
#include "robotweax/srt/handshake_datagram.hpp"
#include "robotweax/srt/udp.hpp"
#include "compat/caller_handshake_steps.hpp"
#include "compat/connection.hpp"
#include "compat/runtime_work_executor_service.hpp"
#include "compat/socket_io.hpp"
#include "compat/socket_registry.hpp"
#include "srt.h"
#include "srt/access_control.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>

#if !defined(_WIN32)
#  include <fcntl.h>
#  include <unistd.h>
#endif

namespace {

#if defined(_WIN32)
using NativeSocketLength = int;
#else
using NativeSocketLength = socklen_t;
#endif

struct FinalHandshakeGate {
    std::shared_ptr<robotweax::srt::compat::DatagramChannel> channel;
    std::atomic_bool response_observed = false;
    std::atomic_bool release_response = false;
};

struct InductionReplay {
    ~InductionReplay()
    {
        if (channel != nullptr) {
            channel->set_send_hook_for_testing(nullptr, nullptr);
        }
    }

    std::shared_ptr<robotweax::srt::compat::DatagramChannel> channel;
    std::array<std::byte, 1500> induction {};
    std::size_t size = 0;
    unsigned variant = 0;
    bool injected = false;
};

robotweax::srt::UdpIoResult replay_induction_before_conclusion(
    std::span<const std::byte> bytes, robotweax::srt::IpEndpoint peer,
    void* context) noexcept
{
    using namespace robotweax::srt;
    auto& replay = *static_cast<InductionReplay*>(context);
    const auto decoded = decode_handshake_datagram(bytes);
    if (decoded
        && decoded.message.packet.request == HandshakeRequest::induction) {
        HandshakeAction stale;
        stale.kind = HandshakeActionKind::send;
        stale.packet = decoded.message.packet;
        if (replay.variant == 1) {
            stale.packet.encryption_field = 4U;
        }
        if (replay.variant == 2) {
            stale.packet.version = handshake_version_4;
        }
        const auto encoded =
            encode_handshake_datagram(stale, decoded.control.timestamp,
                decoded.control.destination_socket_id, replay.induction);
        if (encoded) {
            replay.size = encoded.bytes_written;
        }
    }
    if (decoded
        && decoded.message.packet.request == HandshakeRequest::conclusion
        && replay.size != 0 && !replay.injected) {
        const auto sent = replay.channel->socket.send_to(
            std::span {replay.induction}.first(replay.size), peer);
        if (!sent) {
            return sent;
        }
        replay.injected = true;
    }
    return replay.channel->socket.send_to(bytes, peer);
}

struct ListenCallbackObservation {
    int calls = 0;
    SRTSOCKET socket = SRT_INVALID_SOCK;
    int handshake_version = 0;
    int peer_family = AF_UNSPEC;
    std::string argument_stream_id;
    std::string socket_stream_id;
    std::int32_t peer_version = 0;
    SRT_GROUP_TYPE incoming_group_type = SRT_GTYPE_E_END;
    bool peer_metadata_observed = false;
    bool select_sender = false;
    bool sender_update_succeeded = false;
    bool option_update_succeeded = false;
};

struct RejectCallbackObservation {
    int calls = 0;
    SRTSOCKET socket = SRT_INVALID_SOCK;
    std::string stream_id;
    bool reason_update_succeeded = false;
};

struct ConnectCallbackObservation {
    std::atomic_int calls = 0;
    std::atomic<SRTSOCKET> socket{SRT_INVALID_SOCK};
    std::atomic_int error_code = SRT_SUCCESS;
    std::atomic_int peer_family = AF_UNSPEC;
    std::atomic_int peer_port = 0;
    std::atomic_int token = 0;
    std::atomic_int socket_state = SRTS_NONEXIST;
    std::atomic_bool peer_name_available = false;
    std::atomic_bool finished = false;
    std::mutex callback_mutex;
    std::condition_variable callback_changed;
    bool block_callback = false;
    bool callback_entered = false;
    bool release_callback = false;
    bool throw_after_callback = false;
};

struct CallbackCleanupObservation {
    std::atomic_int result = SRT_ERROR;
    std::atomic_bool finished = false;
};

struct UnsupportedHsv4PeerObservation {
    bool induction_received = false;
    bool response_sent = false;
};

void answer_with_unsupported_hsv4_induction(robotweax::srt::UdpSocket& peer,
    UnsupportedHsv4PeerObservation& observation) noexcept
{
    std::array<std::byte, 1'500> datagram {};
    const auto ready = peer.wait_readable(2'000);
    if (!ready || !ready.ready) {
        return;
    }
    const auto received = peer.receive_from(datagram);
    if (!received) {
        return;
    }
    const auto induction = robotweax::srt::decode_handshake_datagram(
        std::span {datagram}.first(received.bytes_transferred));
    if (!induction
        || induction.message.packet.version
            != robotweax::srt::handshake_version_4
        || induction.message.packet.request
            != robotweax::srt::HandshakeRequest::induction
        || induction.message.packet.socket_id == 0U) {
        return;
    }
    observation.induction_received = true;

    robotweax::srt::HandshakeAction response;
    response.kind = robotweax::srt::HandshakeActionKind::send;
    response.packet = induction.message.packet;
    response.packet.version = robotweax::srt::handshake_version_4;
    response.packet.encryption_field = 0U;
    response.packet.extension_field = 2U; // UDT_DGRAM
    response.packet.socket_id = 0x7654U;
    response.packet.syn_cookie = 0x1020'3040U;
    const auto encoded = robotweax::srt::encode_handshake_datagram(response,
        robotweax::srt::PacketTimestamp {101U},
        induction.message.packet.socket_id, datagram);
    observation.response_sent = encoded
        && static_cast<bool>(peer.send_to(
            std::span {datagram}.first(encoded.bytes_written), received.peer));
}

struct ScopedSrtRuntime {
    int startup_result = srt_startup();

    ScopedSrtRuntime() = default;

    ~ScopedSrtRuntime()
    {
        if (startup_result == 0) {
            (void)srt_cleanup();
        }
    }

    ScopedSrtRuntime(const ScopedSrtRuntime&) = delete;
    ScopedSrtRuntime& operator=(const ScopedSrtRuntime&) = delete;
};

void observe_connect_result(
    void* opaque,
    SRTSOCKET socket,
    int error_code,
    const sockaddr* peer_address,
    int token)
{
    auto& observation =
        *static_cast<ConnectCallbackObservation*>(opaque);
    observation.socket.store(socket);
    observation.error_code.store(error_code);
    observation.peer_family.store(peer_address != nullptr
            ? peer_address->sa_family
            : AF_UNSPEC);
    if (peer_address != nullptr
        && peer_address->sa_family == AF_INET) {
        observation.peer_port.store(ntohs(
            reinterpret_cast<const sockaddr_in*>(peer_address)->sin_port));
    } else if (peer_address != nullptr
        && peer_address->sa_family == AF_INET6) {
        observation.peer_port.store(ntohs(
            reinterpret_cast<const sockaddr_in6*>(peer_address)->sin6_port));
    }
    observation.token.store(token);
    observation.socket_state.store(srt_getsockstate(socket));

    sockaddr_storage connected_peer{};
    int connected_peer_size = static_cast<int>(sizeof(connected_peer));
    observation.peer_name_available.store(
        srt_getpeername(socket,
            reinterpret_cast<sockaddr*>(&connected_peer),
            &connected_peer_size)
        == 0);
    {
        std::unique_lock lock(observation.callback_mutex);
        observation.callback_entered = true;
        observation.callback_changed.notify_all();
        if (observation.block_callback) {
            observation.callback_changed.wait(lock, [&observation] {
                return observation.release_callback;
            });
        }
    }
    observation.calls.fetch_add(1);
    observation.finished.store(true, std::memory_order_release);
    if (observation.throw_after_callback) {
        throw std::runtime_error{"connect callback test"};
    }
}

bool wait_for_connect_callback(
    const ConnectCallbackObservation& observation,
    std::chrono::milliseconds timeout) noexcept
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!observation.finished.load(std::memory_order_acquire)
        && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return observation.finished.load(std::memory_order_acquire);
}

bool wait_for_connect_cleanup(
    const std::shared_ptr<robotweax::srt::compat::SocketRecord>& socket,
    std::chrono::milliseconds timeout) noexcept
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard lock(socket->mutex);
            if (!socket->connect_worker.joinable()
                && socket->connect_handshake_operation == nullptr) {
                return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds {1});
    }
    return false;
}

void cleanup_from_connect_callback(
    void* opaque, SRTSOCKET, int, const sockaddr*, int)
{
    auto& observation = *static_cast<CallbackCleanupObservation*>(opaque);
    observation.result.store(srt_cleanup());
    observation.finished.store(true, std::memory_order_release);
}

int observe_listener_connection(
    void* opaque,
    SRTSOCKET socket,
    int handshake_version,
    const sockaddr* peer_address,
    const char* stream_id)
{
    auto& observation =
        *static_cast<ListenCallbackObservation*>(opaque);
    ++observation.calls;
    observation.socket = socket;
    observation.handshake_version = handshake_version;
    observation.peer_family = peer_address != nullptr
        ? peer_address->sa_family
        : AF_UNSPEC;
    observation.argument_stream_id =
        stream_id != nullptr ? stream_id : "";

    std::array<char,
        robotweax::srt::maximum_stream_id_size + 1U>
        observed_stream_id{};
    int observed_stream_id_size =
        static_cast<int>(observed_stream_id.size());
    if (srt_getsockflag(socket, SRTO_STREAMID,
            observed_stream_id.data(),
            &observed_stream_id_size)
        == SRT_ERROR) {
        return SRT_ERROR;
    }
    observation.socket_stream_id = observed_stream_id.data();

    int peer_version_size =
        static_cast<int>(sizeof(observation.peer_version));
    int group_type_size =
        static_cast<int>(sizeof(observation.incoming_group_type));
    observation.peer_metadata_observed =
        srt_getsockflag(socket, SRTO_PEERVERSION,
            &observation.peer_version, &peer_version_size) == 0
        && srt_getsockflag(socket, SRTO_GROUPTYPE,
            &observation.incoming_group_type, &group_type_size) == 0;
    if (!observation.peer_metadata_observed) {
        return SRT_ERROR;
    }

    constexpr std::int32_t callback_connection_timeout = 2'347;
    const bool timeout_update_succeeded =
        srt_setsockflag(socket, SRTO_CONNTIMEO,
            &callback_connection_timeout,
            static_cast<int>(sizeof(callback_connection_timeout)))
        == 0;
    observation.sender_update_succeeded = !observation.select_sender;
    if (observation.select_sender) {
        constexpr bool sender = true;
        observation.sender_update_succeeded =
            srt_setsockflag(socket, SRTO_SENDER,
                &sender, static_cast<int>(sizeof(sender)))
            == 0;
    }
    observation.option_update_succeeded =
        timeout_update_succeeded && observation.sender_update_succeeded;
    return observation.option_update_succeeded ? 0 : SRT_ERROR;
}

int reject_listener_connection(
    void* opaque,
    SRTSOCKET socket,
    int,
    const sockaddr*,
    const char* stream_id)
{
    auto& observation =
        *static_cast<RejectCallbackObservation*>(opaque);
    ++observation.calls;
    observation.socket = socket;
    observation.stream_id = stream_id != nullptr ? stream_id : "";
    observation.reason_update_succeeded =
        srt_setrejectreason(socket, SRT_REJX_OVERLOAD) == 0;
    if (!observation.reason_update_succeeded) {
        return SRT_ERROR;
    }
    throw std::runtime_error{"listener callback rejection"};
}

robotweax::srt::UdpIoResult gate_final_handshake_response(
    std::span<const std::byte> bytes,
    robotweax::srt::Ipv4Endpoint peer,
    void* context) noexcept
{
    auto& gate = *static_cast<FinalHandshakeGate*>(context);
    const auto sent = gate.channel->socket.send_to(bytes, peer);
    if (!sent) {
        return sent;
    }
    const auto decoded =
        robotweax::srt::decode_handshake_datagram(bytes);
    const bool final_response = decoded
        && decoded.message.packet.request
            == robotweax::srt::HandshakeRequest::conclusion
        && decoded.message.has_handshake_extension
        && decoded.message.extension_type
            == robotweax::srt::HandshakeExtensionType::
                handshake_response;
    if (!final_response
        || gate.response_observed.exchange(true)) {
        return sent;
    }

    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds{2};
    while (!gate.release_response.load()
        && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds{1});
    }
    if (gate.release_response.load()) {
        // Keep the accepting thread inside the final send long enough for
        // the channel worker to dispatch the caller's first data packet.
        std::this_thread::sleep_for(
            std::chrono::milliseconds{25});
    }
    return sent;
}

void close_udp_socket(UDPSOCKET socket) noexcept
{
#if defined(_WIN32)
    (void)closesocket(socket);
#else
    (void)::close(socket);
#endif
}

UDPSOCKET create_udp_socket(int family = AF_INET) noexcept
{
#if defined(_WIN32)
    static const bool winsock_initialized = []() noexcept {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    if (!winsock_initialized) {
        return INVALID_SOCKET;
    }
#endif
    return ::socket(family, SOCK_DGRAM, IPPROTO_UDP);
}

bool udp_socket_is_closed(UDPSOCKET socket) noexcept
{
#if defined(_WIN32)
    int type = 0;
    int size = static_cast<int>(sizeof(type));
    return getsockopt(socket, SOL_SOCKET, SO_TYPE,
               reinterpret_cast<char*>(&type), &size)
            == SOCKET_ERROR
        && WSAGetLastError() == WSAENOTSOCK;
#else
    errno = 0;
    return fcntl(socket, F_GETFD) == -1
        && errno == EBADF;
#endif
}

} // namespace

static_assert(sizeof(SRTSOCKET) == sizeof(std::int32_t));
static_assert(SRT_INVALID_SOCK == -1);
#if defined(_WIN32)
static_assert(SYSSOCKET_INVALID == INVALID_SOCKET);
#else
static_assert(SYSSOCKET_INVALID == -1);
#endif
static_assert(SRT_VERSION_FEAT_HSv5 == 0x010300);
static_assert(SRT_VERSION_VALUE == 0x010507);
static_assert(SRTO_ISN == 3);
static_assert(SRTO_REUSEADDR == 15);
static_assert(SRTO_IPV6ONLY == 54);
static_assert(SRTO_RETRANSMITALGO == 61);
static_assert(SRTO_PACKETFILTER == 60);
static_assert(SRTO_STREAMID == 46);
static_assert(SRTS_NONEXIST == 9);
static_assert(MJ_UNKNOWN == -1);
static_assert(MJ_SETUP == 1);
static_assert(MJ_PEERERROR == 7);
static_assert(sizeof(CodeMajor) == sizeof(int));
static_assert(MN_NONE == 0);
static_assert(MN_INVALMSGAPI == 9);
static_assert(MN_BUSYPORT == 15);
static_assert(MN_CONGESTION == 4);
static_assert(sizeof(CodeMinor) == sizeof(int));
static_assert(offsetof(SRT_MSGCTRL, srctime) == 16);
static_assert(offsetof(SRT_MSGCTRL, pktseq) == 24);
static_assert(offsetof(SRT_MSGCTRL, msgno) == 28);
static_assert(SRT_EPOLL_IN == 0x1);
static_assert(SRT_EPOLL_OUT == 0x4);
static_assert(SRT_EPOLL_ERR == 0x8);
static_assert(SRT_REJ_TIMEOUT == 16);
static_assert(SRT_KM_S_BADCRYPTOMODE == 5);
static_assert(SRT_KM_S_E_SIZE == 6);
#ifdef ENABLE_AEAD_API_PREVIEW
static_assert(SRTO_CRYPTOMODE == 62);
static_assert(SRTO_E_SIZE == 63);
static_assert(SRT_REJ_CRYPTO == 17);
static_assert(SRT_REJ_E_SIZE == 18);
#else
static_assert(SRT_REJ_E_SIZE == 17);
#endif
static_assert(SRT_REJC_PREDEFINED == 1'000);
static_assert(static_cast<std::uint32_t>(SRT_EPOLL_ET)
    == 0x8000'0000U);
static_assert(sizeof(SRT_EPOLL_EVENT) == 8);
static_assert(offsetof(SRT_EPOLL_EVENT, events) == 4);
static_assert(sizeof(SRT_TRACEBSTATS) == 496);
static_assert(offsetof(SRT_TRACEBSTATS, byteSentTotal) == 80);
static_assert(offsetof(SRT_TRACEBSTATS, usPktSndPeriod) == 304);
static_assert(
    offsetof(SRT_TRACEBSTATS, pktSentUniqueTotal) == 432);
static_assert(SRT_GTYPE_UNDEFINED == 0);
static_assert(SRT_GTYPE_BROADCAST == 1);
static_assert(SRT_GTYPE_BACKUP == 2);
static_assert(SRT_GTYPE_E_END == 3);
static_assert(SRT_GST_PENDING == 0);
static_assert(SRT_GST_IDLE == 1);
static_assert(SRT_GST_RUNNING == 2);
static_assert(SRT_GST_BROKEN == 3);
static_assert(sizeof(SRT_SOCKGROUPDATA) == 160);
static_assert(offsetof(SRT_SOCKGROUPDATA, peeraddr) == 8);
static_assert(offsetof(SRT_SOCKGROUPDATA, weight) == 140);
static_assert(offsetof(SRT_SOCKGROUPDATA, token) == 152);
// The config pointer changes both padding and trailing offsets on Win32.
// Keep exact ABI checks for both pointer widths; do not alter the public layout.
static_assert(sizeof(void*) == 4 || sizeof(void*) == 8);
static_assert(sizeof(SRT_SOCKGROUPCONFIG) == (sizeof(void*) == 8 ? 288 : 280));
static_assert(offsetof(SRT_SOCKGROUPCONFIG, srcaddr) == 8);
static_assert(offsetof(SRT_SOCKGROUPCONFIG, peeraddr) == 136);
static_assert(offsetof(SRT_SOCKGROUPCONFIG, weight) == 264);
static_assert(offsetof(SRT_SOCKGROUPCONFIG, config)
    == (sizeof(void*) == 8 ? 272 : 268));
static_assert(offsetof(SRT_SOCKGROUPCONFIG, token)
    == (sizeof(void*) == 8 ? 284 : 276));

TEST(compat_group_accept_bond_and_connect_validation_are_explicit)
{
    REQUIRE_EQ(srt_create_group(SRT_GTYPE_UNDEFINED),
        SRT_INVALID_SOCK);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);

    REQUIRE_EQ(srt_accept_bond(nullptr, 0, 0),
        SRT_INVALID_SOCK);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);
    REQUIRE_EQ(srt_connect_group(17, nullptr, 0),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);
}

TEST(srt_accept_bond_validates_listeners_and_reports_timeout)
{
    ScopedSrtRuntime runtime;
    REQUIRE_EQ(runtime.startup_result, 0);

    const SRTSOCKET unused = 0;
    REQUIRE_EQ(srt_accept_bond(&unused, 0, 0),
        SRT_INVALID_SOCK);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);

    const SRTSOCKET invalid = 987'654'321;
    REQUIRE_EQ(srt_accept_bond(&invalid, 1, 0),
        SRT_INVALID_SOCK);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVSOCK);

    const SRTSOCKET socket = srt_create_socket();
    REQUIRE(socket != SRT_INVALID_SOCK);
    REQUIRE_EQ(srt_accept_bond(&socket, 1, 0),
        SRT_INVALID_SOCK);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ENOLISTEN);
    REQUIRE_EQ(srt_accept_bond(&socket, 1, -2),
        SRT_INVALID_SOCK);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);

    sockaddr_in bind_address{};
    bind_address.sin_family = AF_INET;
    bind_address.sin_port = 0;
    bind_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (srt_bind(socket,
            reinterpret_cast<const sockaddr*>(&bind_address),
            static_cast<int>(sizeof(bind_address))) == SRT_ERROR) {
        REQUIRE_EQ(srt_close(socket), 0);
        return;
    }
    REQUIRE_EQ(srt_listen(socket, 1), 0);
    REQUIRE_EQ(srt_accept_bond(&socket, 1, 0),
        SRT_INVALID_SOCK);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ETIMEOUT);
    REQUIRE_EQ(srt_close(socket), 0);
    REQUIRE_EQ(srt_accept_bond(&socket, 1, 0),
        SRT_INVALID_SOCK);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVSOCK);

    const SRTSOCKET group =
        srt_create_group(SRT_GTYPE_BROADCAST);
    REQUIRE(group != SRT_INVALID_SOCK);
    REQUIRE_EQ(srt_accept_bond(&group, 1, 0),
        SRT_INVALID_SOCK);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVSOCK);
    REQUIRE_EQ(srt_close(group), 0);
}

TEST(srt_accept_bond_accepts_the_first_ready_of_multiple_listeners)
{
    ScopedSrtRuntime runtime;
    REQUIRE_EQ(runtime.startup_result, 0);

    constexpr std::int32_t connection_timeout_milliseconds = 2'000;
    sockaddr_in bind_address{};
    bind_address.sin_family = AF_INET;
    bind_address.sin_port = 0;
    bind_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    const SRTSOCKET first = srt_create_socket();
    const SRTSOCKET second = srt_create_socket();
    REQUIRE(first != SRT_INVALID_SOCK);
    REQUIRE(second != SRT_INVALID_SOCK);
    if (srt_bind(first,
            reinterpret_cast<const sockaddr*>(&bind_address),
            static_cast<int>(sizeof(bind_address))) == SRT_ERROR
        || srt_bind(second,
            reinterpret_cast<const sockaddr*>(&bind_address),
            static_cast<int>(sizeof(bind_address))) == SRT_ERROR) {
        REQUIRE_EQ(srt_close(first), 0);
        REQUIRE_EQ(srt_close(second), 0);
        return;
    }
    REQUIRE_EQ(srt_listen(first, 1), 0);
    REQUIRE_EQ(srt_listen(second, 1), 0);

    sockaddr_in second_name{};
    int second_name_size = static_cast<int>(sizeof(second_name));
    REQUIRE_EQ(srt_getsockname(second,
                   reinterpret_cast<sockaddr*>(&second_name),
                   &second_name_size),
        0);

    const std::array<SRTSOCKET, 2> listeners{second, first};
    std::atomic<SRTSOCKET> accepted{SRT_INVALID_SOCK};
    std::atomic<int> accept_error{SRT_SUCCESS};
    std::thread accept_thread([&] {
        accepted.store(srt_accept_bond(
            listeners.data(), static_cast<int>(listeners.size()),
            connection_timeout_milliseconds));
        accept_error.store(srt_getlasterror(nullptr));
    });

    const SRTSOCKET caller = srt_create_socket();
    REQUIRE(caller != SRT_INVALID_SOCK);
    REQUIRE_EQ(srt_setsockflag(caller, SRTO_CONNTIMEO,
                   &connection_timeout_milliseconds,
                   static_cast<int>(
                       sizeof(connection_timeout_milliseconds))),
        0);
    const int connect_result = srt_connect(caller,
        reinterpret_cast<const sockaddr*>(&second_name),
        static_cast<int>(sizeof(second_name)));
    if (connect_result == SRT_ERROR) {
        (void)srt_close(first);
        (void)srt_close(second);
    }
    accept_thread.join();

    REQUIRE_EQ(connect_result, 0);
    REQUIRE(accepted.load() != SRT_INVALID_SOCK);
    REQUIRE_EQ(accept_error.load(), SRT_SUCCESS);

    sockaddr_in accepted_local{};
    int accepted_local_size = static_cast<int>(sizeof(accepted_local));
    REQUIRE_EQ(srt_getsockname(accepted.load(),
                   reinterpret_cast<sockaddr*>(&accepted_local),
                   &accepted_local_size),
        0);
    REQUIRE_EQ(accepted_local.sin_port, second_name.sin_port);

    REQUIRE_EQ(srt_close(caller), 0);
    REQUIRE_EQ(srt_close(accepted.load()), 0);
    REQUIRE_EQ(srt_close(first), 0);
    REQUIRE_EQ(srt_close(second), 0);
}

TEST(srt_accept_bond_wakes_when_a_listener_closes)
{
    ScopedSrtRuntime runtime;
    REQUIRE_EQ(runtime.startup_result, 0);

    sockaddr_in bind_address{};
    bind_address.sin_family = AF_INET;
    bind_address.sin_port = 0;
    bind_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    const SRTSOCKET listener = srt_create_socket();
    REQUIRE(listener != SRT_INVALID_SOCK);
    if (srt_bind(listener,
            reinterpret_cast<const sockaddr*>(&bind_address),
            static_cast<int>(sizeof(bind_address))) == SRT_ERROR) {
        REQUIRE_EQ(srt_close(listener), 0);
        return;
    }
    REQUIRE_EQ(srt_listen(listener, 1), 0);

    std::atomic<SRTSOCKET> accepted{SRT_INVALID_SOCK};
    std::atomic<int> accept_error{SRT_SUCCESS};
    std::thread accept_thread([&] {
        accepted.store(srt_accept_bond(&listener, 1, 500));
        accept_error.store(srt_getlasterror(nullptr));
    });
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
    REQUIRE_EQ(srt_close(listener), 0);
    accept_thread.join();

    REQUIRE_EQ(accepted.load(), SRT_INVALID_SOCK);
    REQUIRE_EQ(accept_error.load(), SRT_EINVSOCK);
}

TEST(compat_caller_routes_conclusion_for_standard_and_haivision_induction)
{
    constexpr std::uint32_t local_socket_id = 17U;
    REQUIRE_EQ(
        robotweax::srt::compat::caller_conclusion_destination(
            local_socket_id, 91U),
        91U);
    REQUIRE_EQ(
        robotweax::srt::compat::caller_conclusion_destination(
            local_socket_id, local_socket_id),
        0U);
    REQUIRE_EQ(
        robotweax::srt::compat::caller_conclusion_destination(
            local_socket_id, 0U),
        0U);
}

TEST(srt_compat_lifecycle_matches_the_v1_5_5_contract)
{
    srt_clearlasterror();
    REQUIRE_EQ(srt_startup(), 0);
    REQUIRE_EQ(srt_startup(), 0);

    const SRTSOCKET first = srt_create_socket();
    const SRTSOCKET second = srt_socket(0, 0, 0);
    REQUIRE(first != SRT_INVALID_SOCK);
    REQUIRE(second != SRT_INVALID_SOCK);
    REQUIRE(first != second);
    REQUIRE_EQ(srt_getsockstate(first), SRTS_INIT);
    REQUIRE_EQ(srt_getsockstate(123'456'789), SRTS_NONEXIST);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_SUCCESS);

    REQUIRE_EQ(srt_close(first), 0);
    REQUIRE_EQ(srt_getsockstate(first), SRTS_CLOSED);
    REQUIRE_EQ(srt_close(first), 0);
    REQUIRE_EQ(srt_close(123'456'789), 0);

    REQUIRE_EQ(srt_cleanup(), 0);
    REQUIRE_EQ(srt_getsockstate(second), SRTS_INIT);
    REQUIRE_EQ(srt_cleanup(), 0);
    REQUIRE_EQ(srt_getsockstate(second), SRTS_NONEXIST);

    const SRTSOCKET after_cleanup = srt_create_socket();
    REQUIRE(after_cleanup != first);
    REQUIRE(after_cleanup != second);
    REQUIRE_EQ(srt_getsockstate(first), SRTS_NONEXIST);
    REQUIRE_EQ(srt_close(after_cleanup), 0);
    REQUIRE_EQ(srt_cleanup(), 0);
}

TEST(srt_compat_listener_callback_registration_validates_socket_state)
{
    ListenCallbackObservation observation;
    const SRTSOCKET socket = srt_create_socket();
    REQUIRE(socket != SRT_INVALID_SOCK);

    REQUIRE_EQ(srt_listen_callback(socket,
                   observe_listener_connection, &observation),
        0);
    REQUIRE_EQ(srt_listen_callback(socket, nullptr, nullptr), 0);
    REQUIRE_EQ(srt_listen_callback(
                   123'456'789, observe_listener_connection, &observation),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVSOCK);

    REQUIRE_EQ(srt_close(socket), 0);
    REQUIRE_EQ(srt_listen_callback(
                   socket, observe_listener_connection, &observation),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVSOCK);
    REQUIRE_EQ(srt_cleanup(), 0);
}

TEST(srt_compat_connect_callback_registration_validates_socket_state)
{
    ConnectCallbackObservation observation;
    const SRTSOCKET socket = srt_create_socket();
    REQUIRE(socket != SRT_INVALID_SOCK);

    REQUIRE_EQ(srt_connect_callback(socket,
                   observe_connect_result, &observation),
        0);
    REQUIRE_EQ(srt_connect_callback(socket, nullptr, nullptr), 0);
    REQUIRE_EQ(srt_connect_callback(
                   123'456'789, observe_connect_result, &observation),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVSOCK);

    const auto record =
        robotweax::srt::compat::SocketRegistry::instance().find(socket);
    REQUIRE(record != nullptr);
    {
        std::lock_guard lock(record->mutex);
        record->state = SRTS_CONNECTED;
    }
    REQUIRE_EQ(srt_connect_callback(socket,
                   observe_connect_result, &observation),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ECONNSOCK);

    REQUIRE_EQ(srt_close(socket), 0);
    REQUIRE_EQ(srt_connect_callback(socket,
                   observe_connect_result, &observation),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVSOCK);
    REQUIRE_EQ(srt_cleanup(), 0);
}

TEST(srt_compat_peer_metadata_options_are_read_only_and_contextual)
{
    ScopedSrtRuntime runtime;
    REQUIRE_EQ(runtime.startup_result, 0);

    const SRTSOCKET socket = srt_create_socket();
    REQUIRE(socket != SRT_INVALID_SOCK);
    std::int32_t value = -1;
    int value_size = static_cast<int>(sizeof(value));
    REQUIRE_EQ(srt_getsockflag(
                   socket, SRTO_PEERVERSION, &value, &value_size),
        0);
    REQUIRE_EQ(value, 0);

    value = -1;
    value_size = static_cast<int>(sizeof(value));
    REQUIRE_EQ(srt_getsockflag(
                   socket, SRTO_GROUPTYPE, &value, &value_size),
        0);
    REQUIRE_EQ(value, static_cast<std::int32_t>(SRT_GTYPE_UNDEFINED));

    REQUIRE_EQ(srt_setsockflag(socket, SRTO_PEERVERSION,
                   &value, static_cast<int>(sizeof(value))),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVOP);
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_GROUPTYPE,
                   &value, static_cast<int>(sizeof(value))),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVOP);

    const auto record =
        robotweax::srt::compat::SocketRegistry::instance().find(socket);
    REQUIRE(record != nullptr);
    {
        std::lock_guard lock(record->mutex);
        record->listen_callback_active = true;
        record->incoming_group_type = SRT_GTYPE_BACKUP;
    }
    value_size = static_cast<int>(sizeof(value));
    REQUIRE_EQ(srt_getsockflag(
                   socket, SRTO_PEERVERSION, &value, &value_size),
        0);
    REQUIRE_EQ(value, 0);
    value_size = static_cast<int>(sizeof(value));
    REQUIRE_EQ(srt_getsockflag(
                   socket, SRTO_GROUPTYPE, &value, &value_size),
        0);
    REQUIRE_EQ(value, static_cast<std::int32_t>(SRT_GTYPE_BACKUP));

    {
        std::lock_guard lock(record->mutex);
        record->listen_callback_active = false;
        record->peer_srt_version = 0x0001'0402U;
    }
    value_size = static_cast<int>(sizeof(value));
    REQUIRE_EQ(srt_getsockflag(
                   socket, SRTO_PEERVERSION, &value, &value_size),
        0);
    REQUIRE_EQ(value, 0x0001'0402);
    value_size = static_cast<int>(sizeof(value));
    REQUIRE_EQ(srt_getsockflag(
                   socket, SRTO_GROUPTYPE, &value, &value_size),
        0);
    REQUIRE_EQ(value, static_cast<std::int32_t>(SRT_GTYPE_UNDEFINED));

    value_size = static_cast<int>(sizeof(value) - 1U);
    REQUIRE_EQ(srt_getsockflag(
                   socket, SRTO_PEERVERSION, &value, &value_size),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);
    REQUIRE_EQ(srt_close(socket), 0);
}

TEST(srt_compat_exposes_the_initial_sequence_number_as_read_only)
{
    const SRTSOCKET socket = srt_create_socket();
    REQUIRE(socket != SRT_INVALID_SOCK);

    std::int32_t initial_sequence = -1;
    int size = static_cast<int>(sizeof(initial_sequence));
    REQUIRE_EQ(
        srt_getsockflag(
            socket, SRTO_ISN, &initial_sequence, &size),
        0);
    REQUIRE_EQ(size, static_cast<int>(sizeof(initial_sequence)));
    REQUIRE(initial_sequence >= 0);
    REQUIRE(initial_sequence <= 0x7fff'ffff);

    std::int32_t legacy_value = -1;
    size = static_cast<int>(sizeof(legacy_value));
    REQUIRE_EQ(
        srt_getsockopt(
            socket, 0, SRTO_ISN, &legacy_value, &size),
        0);
    REQUIRE_EQ(legacy_value, initial_sequence);

    size = static_cast<int>(sizeof(initial_sequence) - 1U);
    REQUIRE_EQ(
        srt_getsockflag(
            socket, SRTO_ISN, &initial_sequence, &size),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);

    const std::int32_t replacement = 123;
    REQUIRE_EQ(
        srt_setsockflag(
            socket, SRTO_ISN, &replacement,
            static_cast<int>(sizeof(replacement))),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVOP);

    size = static_cast<int>(sizeof(initial_sequence));
    REQUIRE_EQ(
        srt_getsockflag(
            socket, SRTO_ISN, &initial_sequence, &size),
        0);
    REQUIRE_EQ(initial_sequence, legacy_value);

    REQUIRE_EQ(srt_close(socket), 0);
    REQUIRE_EQ(srt_cleanup(), 0);
}

TEST(srt_compat_exposes_reference_defaults_and_validated_live_options)
{
    const SRTSOCKET socket = srt_create_socket();
    REQUIRE(socket != SRT_INVALID_SOCK);

    bool boolean_value = false;
    int size = static_cast<int>(sizeof(boolean_value));
    REQUIRE_EQ(
        srt_getsockflag(socket, SRTO_SNDSYN, &boolean_value, &size), 0);
    REQUIRE(boolean_value);
    REQUIRE_EQ(size, static_cast<int>(sizeof(bool)));

    std::int32_t integer_value = 0;
    size = static_cast<int>(sizeof(integer_value));
    REQUIRE_EQ(srt_getsockflag(socket, SRTO_FC, &integer_value, &size), 0);
    REQUIRE_EQ(integer_value, 25'600);

    size = static_cast<int>(sizeof(integer_value));
    REQUIRE_EQ(srt_getsockflag(
                   socket, SRTO_MSS,
                   &integer_value, &size),
        0);
    REQUIRE_EQ(integer_value, 1'500);

    size = static_cast<int>(sizeof(integer_value));
    REQUIRE_EQ(srt_getsockflag(
                   socket, SRTO_SNDBUF,
                   &integer_value, &size),
        0);
    REQUIRE_EQ(integer_value, 12'058'624);
    size = static_cast<int>(sizeof(integer_value));
    REQUIRE_EQ(srt_getsockflag(
                   socket, SRTO_RCVBUF,
                   &integer_value, &size),
        0);
    REQUIRE_EQ(integer_value, 12'058'624);
    size = static_cast<int>(sizeof(integer_value));
    REQUIRE_EQ(srt_getsockflag(
                   socket, SRTO_UDP_SNDBUF,
                   &integer_value, &size),
        0);
    REQUIRE_EQ(integer_value, 65'536);
    size = static_cast<int>(sizeof(integer_value));
    REQUIRE_EQ(srt_getsockflag(
                   socket, SRTO_UDP_RCVBUF,
                   &integer_value, &size),
        0);
    REQUIRE_EQ(integer_value, 12'288'000);

    integer_value = 131'072;
    REQUIRE_EQ(srt_setsockflag(
                   socket, SRTO_UDP_SNDBUF,
                   &integer_value,
                   static_cast<int>(sizeof(integer_value))),
        0);
    integer_value = 1'048'576;
    REQUIRE_EQ(srt_setsockflag(
                   socket, SRTO_UDP_RCVBUF,
                   &integer_value,
                   static_cast<int>(sizeof(integer_value))),
        0);
    integer_value = 0;
    size = static_cast<int>(sizeof(integer_value));
    REQUIRE_EQ(srt_getsockflag(
                   socket, SRTO_UDP_SNDBUF,
                   &integer_value, &size),
        0);
    REQUIRE_EQ(integer_value, 131'072);
    integer_value = 1'499;
    REQUIRE_EQ(srt_setsockflag(
                   socket, SRTO_UDP_RCVBUF,
                   &integer_value,
                   static_cast<int>(sizeof(integer_value))),
        0);
    integer_value = 0;
    size = static_cast<int>(sizeof(integer_value));
    REQUIRE_EQ(srt_getsockflag(
                   socket, SRTO_UDP_RCVBUF,
                   &integer_value, &size),
        0);
    REQUIRE_EQ(integer_value, 1'500);

    std::int64_t wide_value = 0;
    size = static_cast<int>(sizeof(wide_value));
    REQUIRE_EQ(srt_getsockflag(socket, SRTO_MAXBW, &wide_value, &size), 0);
    REQUIRE_EQ(wide_value, -1);

    wide_value = -1;
    size = static_cast<int>(sizeof(wide_value));
    REQUIRE_EQ(srt_getsockflag(
                   socket, SRTO_MININPUTBW, &wide_value, &size),
        0);
    REQUIRE_EQ(wide_value, 0);
    wide_value = 250'000;
    REQUIRE_EQ(srt_setsockflag(
                   socket, SRTO_MININPUTBW, &wide_value,
                   static_cast<int>(sizeof(wide_value))),
        0);
    wide_value = -1;
    REQUIRE_EQ(srt_setsockflag(
                   socket, SRTO_MININPUTBW, &wide_value,
                   static_cast<int>(sizeof(wide_value))),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);

    boolean_value = false;
    size = static_cast<int>(sizeof(boolean_value));
    REQUIRE_EQ(srt_getsockflag(
                   socket, SRTO_DRIFTTRACER, &boolean_value, &size),
        0);
    REQUIRE(boolean_value);
    boolean_value = false;
    REQUIRE_EQ(srt_setsockflag(
                   socket, SRTO_DRIFTTRACER, &boolean_value,
                   static_cast<int>(sizeof(boolean_value))),
        0);

    integer_value = 0;
    size = static_cast<int>(sizeof(integer_value));
    REQUIRE_EQ(srt_getsockflag(
                   socket, SRTO_MINVERSION, &integer_value, &size),
        0);
    REQUIRE_EQ(integer_value, 0x0001'0000);
    integer_value = 0x0001'0500;
    REQUIRE_EQ(srt_setsockflag(
                   socket, SRTO_MINVERSION, &integer_value,
                   static_cast<int>(sizeof(integer_value))),
        0);
    integer_value = -1;
    REQUIRE_EQ(srt_setsockflag(
                   socket, SRTO_MINVERSION, &integer_value,
                   static_cast<int>(sizeof(integer_value))),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);

    linger linger_value{};
    size = static_cast<int>(sizeof(linger_value));
    REQUIRE_EQ(srt_getsockflag(
                   socket, SRTO_LINGER,
                   &linger_value, &size),
        0);
    REQUIRE_EQ(linger_value.l_onoff, 0);
    REQUIRE_EQ(linger_value.l_linger, 0);

    integer_value = 0;
    size = static_cast<int>(sizeof(integer_value));
    REQUIRE_EQ(
        srt_getsockflag(socket, SRTO_PAYLOADSIZE, &integer_value, &size), 0);
    REQUIRE_EQ(integer_value, SRT_LIVE_DEF_PLSIZE);

    integer_value = 0;
    size = static_cast<int>(sizeof(integer_value));
    REQUIRE_EQ(
        srt_getsockflag(socket, SRTO_CONNTIMEO, &integer_value, &size), 0);
    REQUIRE_EQ(integer_value, 3'000);

    integer_value = 0;
    size = static_cast<int>(sizeof(integer_value));
    REQUIRE_EQ(srt_getsockflag(
                   socket, SRTO_PEERIDLETIMEO,
                   &integer_value, &size),
        0);
    REQUIRE_EQ(integer_value, 5'000);
    integer_value = -1;
    size = static_cast<int>(sizeof(integer_value));
    REQUIRE_EQ(srt_getsockflag(
                   socket, SRTO_LOSSMAXTTL,
                   &integer_value, &size),
        0);
    REQUIRE_EQ(integer_value, 0);
    REQUIRE_EQ(size, static_cast<int>(sizeof(integer_value)));
    integer_value = 5;
    REQUIRE_EQ(srt_setsockflag(
                   socket, SRTO_LOSSMAXTTL,
                   &integer_value,
                   static_cast<int>(sizeof(integer_value))),
        0);
    integer_value = 0;
    size = static_cast<int>(sizeof(integer_value));
    REQUIRE_EQ(srt_getsockflag(
                   socket, SRTO_LOSSMAXTTL,
                   &integer_value, &size),
        0);
    REQUIRE_EQ(integer_value, 5);
    integer_value = -1;
    REQUIRE_EQ(srt_setsockflag(
                   socket, SRTO_LOSSMAXTTL,
                   &integer_value,
                   static_cast<int>(sizeof(integer_value))),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);
    integer_value = 7'500;
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_PEERIDLETIMEO,
                   &integer_value,
                   static_cast<int>(sizeof(integer_value))),
        0);
    integer_value = -1;
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_PEERIDLETIMEO,
                   &integer_value,
                   static_cast<int>(sizeof(integer_value))),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);

    integer_value = 0;
    size = static_cast<int>(sizeof(integer_value));
    REQUIRE_EQ(
        srt_getsockflag(socket, SRTO_SNDTIMEO, &integer_value, &size), 0);
    REQUIRE_EQ(integer_value, -1);
    size = static_cast<int>(sizeof(integer_value));
    REQUIRE_EQ(
        srt_getsockflag(socket, SRTO_RCVTIMEO, &integer_value, &size), 0);
    REQUIRE_EQ(integer_value, -1);
    integer_value = 250;
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_SNDTIMEO,
                   &integer_value, static_cast<int>(sizeof(integer_value))),
        0);
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_RCVTIMEO,
                   &integer_value, static_cast<int>(sizeof(integer_value))),
        0);

    integer_value = 1'250;
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_CONNTIMEO,
                   &integer_value, static_cast<int>(sizeof(integer_value))),
        0);
    integer_value = -1;
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_CONNTIMEO,
                   &integer_value, static_cast<int>(sizeof(integer_value))),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);

    wide_value = 400'000;
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_INPUTBW,
                   &wide_value, static_cast<int>(sizeof(wide_value))),
        0);
    wide_value = 500'000;
    REQUIRE_EQ(srt_setsockopt(socket, 0, SRTO_MAXBW,
                   &wide_value, static_cast<int>(sizeof(wide_value))),
        0);
    integer_value = 300;
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_LATENCY,
                   &integer_value, static_cast<int>(sizeof(integer_value))),
        0);

    integer_value = 0;
    size = static_cast<int>(sizeof(integer_value));
    REQUIRE_EQ(
        srt_getsockflag(socket, SRTO_RCVLATENCY, &integer_value, &size), 0);
    REQUIRE_EQ(integer_value, 300);
    integer_value = 0;
    size = static_cast<int>(sizeof(integer_value));
    REQUIRE_EQ(
        srt_getsockflag(socket, SRTO_PEERLATENCY, &integer_value, &size), 0);
    REQUIRE_EQ(integer_value, 300);

    integer_value = 101;
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_OHEADBW,
                   &integer_value, static_cast<int>(sizeof(integer_value))),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);
    REQUIRE(std::strcmp(srt_getlasterror_str(),
                "Operation not supported: Bad parameters")
        == 0);

    size = static_cast<int>(sizeof(integer_value));
    REQUIRE_EQ(
        srt_getsockflag(socket, SRTO_PBKEYLEN, &integer_value, &size),
        0);
    REQUIRE_EQ(integer_value, 0);

    constexpr char passphrase[] = "correct horse battery";
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_PASSPHRASE,
                   passphrase,
                   static_cast<int>(sizeof(passphrase) - 1U)),
        0);
    integer_value = 32;
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_PBKEYLEN,
                   &integer_value, static_cast<int>(sizeof(integer_value))),
        0);
    integer_value = 10'000;
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_KMREFRESHRATE,
                   &integer_value, static_cast<int>(sizeof(integer_value))),
        0);
    integer_value = 1'000;
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_KMPREANNOUNCE,
                   &integer_value, static_cast<int>(sizeof(integer_value))),
        0);
    boolean_value = false;
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_ENFORCEDENCRYPTION,
                   &boolean_value,
                   static_cast<int>(sizeof(boolean_value))),
        0);

    integer_value = 0;
    size = static_cast<int>(sizeof(integer_value));
    REQUIRE_EQ(srt_getsockflag(
                   socket, SRTO_PBKEYLEN, &integer_value, &size),
        0);
    REQUIRE_EQ(integer_value, 32);
    size = static_cast<int>(sizeof(integer_value));
    REQUIRE_EQ(srt_getsockflag(socket, SRTO_KMREFRESHRATE,
                   &integer_value, &size),
        0);
    REQUIRE_EQ(integer_value, 10'000);
    size = static_cast<int>(sizeof(integer_value));
    REQUIRE_EQ(srt_getsockflag(socket, SRTO_KMPREANNOUNCE,
                   &integer_value, &size),
        0);
    REQUIRE_EQ(integer_value, 1'000);
    integer_value = -1;
    size = static_cast<int>(sizeof(integer_value));
    REQUIRE_EQ(srt_getsockflag(socket, SRTO_KMSTATE,
                   &integer_value, &size),
        0);
    REQUIRE_EQ(integer_value,
        static_cast<int>(SRT_KM_S_UNSECURED));
    boolean_value = true;
    size = static_cast<int>(sizeof(boolean_value));
    REQUIRE_EQ(srt_getsockflag(socket, SRTO_ENFORCEDENCRYPTION,
                   &boolean_value, &size),
        0);
    REQUIRE(!boolean_value);

    const auto record =
        robotweax::srt::compat::SocketRegistry::instance().find(socket);
    REQUIRE(record != nullptr);
    {
        std::lock_guard lock(record->mutex);
        record->state = SRTS_CONNECTED;
    }
    integer_value = 0x0001'0400;
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_MINVERSION,
                   &integer_value,
                   static_cast<int>(sizeof(integer_value))),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ECONNSOCK);
    boolean_value = true;
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_DRIFTTRACER,
                   &boolean_value,
                   static_cast<int>(sizeof(boolean_value))),
        0);
    wide_value = 300'000;
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_MININPUTBW,
                   &wide_value,
                   static_cast<int>(sizeof(wide_value))),
        0);

    REQUIRE_EQ(srt_close(socket), 0);
    REQUIRE_EQ(srt_cleanup(), 0);
}

TEST(srt_compat_connection_time_uses_the_public_monotonic_epoch)
{
    ScopedSrtRuntime runtime;
    REQUIRE_EQ(runtime.startup_result, 0);
    const auto before = srt_time_now();
    const SRTSOCKET socket = srt_create_socket();
    REQUIRE(socket != SRT_INVALID_SOCK);
    const auto opened = srt_connection_time(socket);
    const auto after = srt_time_now();
    REQUIRE(opened >= before);
    REQUIRE(opened <= after);
    REQUIRE_EQ(srt_connection_time(socket), opened);

    const SRTSOCKET group = srt_create_group(SRT_GTYPE_BROADCAST);
    REQUIRE(group != SRT_INVALID_SOCK);
    REQUIRE_EQ(srt_connection_time(group), -1);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVSOCK);
    REQUIRE_EQ(srt_close(group), 0);

    REQUIRE_EQ(srt_close(socket), 0);
    REQUIRE_EQ(srt_connection_time(socket), -1);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVSOCK);
    REQUIRE_EQ(srt_connection_time(123'456'789), -1);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVSOCK);
}

TEST(srt_compat_mss_is_pre_bind_and_realigns_packet_buffers)
{
    const SRTSOCKET socket = srt_create_socket();
    REQUIRE(socket != SRT_INVALID_SOCK);

    std::int32_t value = 1'280;
    REQUIRE_EQ(srt_setsockflag(
                   socket, SRTO_MSS, &value,
                   static_cast<int>(sizeof(value))),
        0);
    int size = static_cast<int>(sizeof(value));
    value = 0;
    REQUIRE_EQ(srt_getsockflag(
                   socket, SRTO_MSS, &value, &size),
        0);
    REQUIRE_EQ(value, 1'280);

    size = static_cast<int>(sizeof(value));
    REQUIRE_EQ(srt_getsockflag(
                   socket, SRTO_SNDBUF, &value, &size),
        0);
    REQUIRE_EQ(value, 10'256'384);
    size = static_cast<int>(sizeof(value));
    REQUIRE_EQ(srt_getsockflag(
                   socket, SRTO_RCVBUF, &value, &size),
        0);
    REQUIRE_EQ(value, 10'256'384);

    for (const std::int32_t invalid : {75, 1'501}) {
        value = invalid;
        REQUIRE_EQ(srt_setsockflag(
                       socket, SRTO_MSS, &value,
                       static_cast<int>(sizeof(value))),
            SRT_ERROR);
        REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);
    }

    REQUIRE_EQ(srt_close(socket), 0);
}

TEST(srt_compat_srt_buffers_use_aligned_packet_capacity)
{
    constexpr std::int32_t buffer_unit_bytes = 1'472;
    const SRTSOCKET socket = srt_create_socket();
    REQUIRE(socket != SRT_INVALID_SOCK);

    std::int32_t value = 1'000'000;
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_SNDBUF,
                   &value, static_cast<int>(sizeof(value))),
        0);
    value = 0;
    int size = static_cast<int>(sizeof(value));
    REQUIRE_EQ(srt_getsockflag(socket, SRTO_SNDBUF,
                   &value, &size),
        0);
    REQUIRE_EQ(value, 679 * buffer_unit_bytes);

    value = 256;
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_FC,
                   &value, static_cast<int>(sizeof(value))),
        0);
    value = 1'000'000;
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_RCVBUF,
                   &value, static_cast<int>(sizeof(value))),
        0);
    value = 0;
    size = static_cast<int>(sizeof(value));
    REQUIRE_EQ(srt_getsockflag(socket, SRTO_RCVBUF,
                   &value, &size),
        0);
    REQUIRE_EQ(value, 256 * buffer_unit_bytes);

    const auto record =
        robotweax::srt::compat::SocketRegistry::instance().find(socket);
    REQUIRE(record != nullptr);
    {
        std::lock_guard lock(record->mutex);
        REQUIRE_EQ(record->native_options.send_buffer_packets(), 679U);
        REQUIRE_EQ(record->native_options.receive_buffer_packets(), 256U);
    }

    value = 1;
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_SNDBUF,
                   &value, static_cast<int>(sizeof(value))),
        0);
    value = 0;
    size = static_cast<int>(sizeof(value));
    REQUIRE_EQ(srt_getsockflag(socket, SRTO_SNDBUF,
                   &value, &size),
        0);
    REQUIRE_EQ(value, 32 * buffer_unit_bytes);

    value = 0;
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_RCVBUF,
                   &value, static_cast<int>(sizeof(value))),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);

    REQUIRE_EQ(srt_close(socket), 0);
    REQUIRE_EQ(srt_cleanup(), 0);
}

TEST(srt_compat_packet_filter_option_is_bounded_and_reduces_payload)
{
    const SRTSOCKET socket = srt_create_socket();
    REQUIRE(socket != SRT_INVALID_SOCK);

    std::array<char, 64> value{};
    int size = static_cast<int>(value.size());
    REQUIRE_EQ(srt_getsockflag(
            socket, SRTO_PACKETFILTER,
            value.data(), &size),
        0);
    REQUIRE_EQ(size, 0);
    REQUIRE_EQ(value[0], '\0');
    REQUIRE_EQ(srt_setsockflag(
            socket, SRTO_PACKETFILTER,
            nullptr, 0),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr),
        SRT_EINVPARAM);

    std::int32_t payload = SRT_LIVE_MAX_PLSIZE;
    REQUIRE_EQ(srt_setsockflag(
            socket, SRTO_PAYLOADSIZE,
            &payload,
            static_cast<int>(sizeof(payload))),
        0);

    constexpr char configuration[] =
        "fec,cols:10,arq:onreq";
    REQUIRE_EQ(srt_setsockflag(
            socket, SRTO_PACKETFILTER,
            configuration,
            static_cast<int>(
                sizeof(configuration) - 1U)),
        0);

    value.fill('\0');
    size = static_cast<int>(value.size());
    REQUIRE_EQ(srt_getsockflag(
            socket, SRTO_PACKETFILTER,
            value.data(), &size),
        0);
    REQUIRE_EQ(size, static_cast<int>(
        sizeof(configuration) - 1U));
    REQUIRE(std::strcmp(value.data(),
        configuration) == 0);

    payload = 0;
    size = static_cast<int>(sizeof(payload));
    REQUIRE_EQ(srt_getsockflag(
            socket, SRTO_PAYLOADSIZE,
            &payload, &size),
        0);
    REQUIRE_EQ(payload,
        static_cast<std::int32_t>(
            SRT_LIVE_MAX_PLSIZE - 4));

    constexpr char invalid[] = "fec,cols:1";
    REQUIRE_EQ(srt_setsockflag(
            socket, SRTO_PACKETFILTER,
            invalid,
            static_cast<int>(
                sizeof(invalid) - 1U)),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr),
        SRT_EINVPARAM);

    REQUIRE_EQ(srt_close(socket), 0);
}

TEST(srt_compat_stream_id_option_is_bounded_and_round_trips_exact_bytes)
{
    const SRTSOCKET socket = srt_create_socket();
    REQUIRE(socket != SRT_INVALID_SOCK);

    std::array<char,
        robotweax::srt::maximum_stream_id_size + 1U> value{};
    int size = static_cast<int>(value.size());
    REQUIRE_EQ(srt_getsockflag(
            socket, SRTO_STREAMID, value.data(), &size),
        0);
    REQUIRE_EQ(size, 0);
    REQUIRE_EQ(value[0], '\0');

    std::array<char,
        robotweax::srt::maximum_stream_id_size> maximum{};
    for (std::size_t index = 0; index < maximum.size(); ++index) {
        maximum[index] = static_cast<char>('a' + index % 26U);
    }
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_STREAMID,
                   maximum.data(), static_cast<int>(maximum.size())),
        0);
    size = static_cast<int>(value.size());
    REQUIRE_EQ(srt_getsockflag(
            socket, SRTO_STREAMID, value.data(), &size),
        0);
    REQUIRE_EQ(size, static_cast<int>(maximum.size()));
    REQUIRE(std::equal(maximum.begin(), maximum.end(), value.begin()));
    REQUIRE_EQ(value[maximum.size()], '\0');

    std::array<char,
        robotweax::srt::maximum_stream_id_size + 1U> too_long{};
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_STREAMID,
                   too_long.data(), static_cast<int>(too_long.size())),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);

    REQUIRE_EQ(srt_setsockflag(socket, SRTO_STREAMID, nullptr, 0), 0);
    size = static_cast<int>(value.size());
    REQUIRE_EQ(srt_getsockflag(
            socket, SRTO_STREAMID, value.data(), &size),
        0);
    REQUIRE_EQ(size, 0);
    REQUIRE_EQ(value[0], '\0');

    REQUIRE_EQ(srt_close(socket), 0);
}

TEST(srt_compat_rejection_diagnostics_match_the_public_contract)
{
    REQUIRE_EQ(srt_getrejectreason(987'654'321),
        SRT_REJ_UNKNOWN);
    REQUIRE(std::strcmp(
                srt_rejectreason_str(SRT_REJ_TIMEOUT),
                "Connection timeout")
        == 0);
    REQUIRE(std::strcmp(
                srt_rejectreason_str(-1),
                "Unknown or erroneous")
        == 0);
    REQUIRE(std::strcmp(
                srt_rejectreason_str(1'404),
                "Application-defined rejection reason")
        == 0);

    const SRTSOCKET socket = srt_create_socket();
    REQUIRE(socket != SRT_INVALID_SOCK);
    REQUIRE_EQ(srt_getrejectreason(socket), SRT_REJ_UNKNOWN);
    REQUIRE_EQ(srt_setrejectreason(socket, 999), SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);
    REQUIRE_EQ(srt_setrejectreason(socket, 1'404), 0);
    REQUIRE_EQ(srt_getrejectreason(socket), 1'404);
    REQUIRE_EQ(srt_setrejectreason(987'654'321, 1'404),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVSOCK);
    REQUIRE_EQ(srt_close(socket), 0);
    REQUIRE_EQ(srt_cleanup(), 0);
}

TEST(srt_compat_bind_and_local_name_follow_socket_state)
{
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = 0;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    REQUIRE_EQ(srt_bind(987'654'321,
                   reinterpret_cast<const sockaddr*>(&address),
                   static_cast<int>(sizeof(address))),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVSOCK);

    const SRTSOCKET socket = srt_create_socket();
    REQUIRE(socket != SRT_INVALID_SOCK);

    REQUIRE_EQ(srt_bind(socket, nullptr, 0), SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);

    sockaddr_in local{};
    int local_size = static_cast<int>(sizeof(local));
    REQUIRE_EQ(srt_getsockname(socket,
                   reinterpret_cast<sockaddr*>(&local), &local_size),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ENOCONN);

    sockaddr_in unsupported = address;
    unsupported.sin_family = AF_UNSPEC;
    REQUIRE_EQ(srt_bind(socket,
                   reinterpret_cast<const sockaddr*>(&unsupported),
                   static_cast<int>(sizeof(unsupported))),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);

    const int bind_result = srt_bind(socket,
        reinterpret_cast<const sockaddr*>(&address),
        static_cast<int>(sizeof(address)));
    if (bind_result == 0) {
        REQUIRE_EQ(srt_getsockstate(socket), SRTS_OPENED);
        local_size = static_cast<int>(sizeof(local));
        REQUIRE_EQ(srt_getsockname(socket,
                       reinterpret_cast<sockaddr*>(&local), &local_size),
            0);
        REQUIRE_EQ(local_size, static_cast<int>(sizeof(sockaddr_in)));
        REQUIRE_EQ(local.sin_family, AF_INET);
        REQUIRE(ntohs(local.sin_port) != 0);

        std::int32_t buffer_size = 1'000'000;
        REQUIRE_EQ(srt_setsockflag(socket, SRTO_SNDBUF,
                       &buffer_size, static_cast<int>(sizeof(buffer_size))),
            SRT_ERROR);
        REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EBOUNDSOCK);

        REQUIRE_EQ(srt_setsockflag(socket, SRTO_UDP_RCVBUF,
                       &buffer_size, static_cast<int>(sizeof(buffer_size))),
            SRT_ERROR);
        REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EBOUNDSOCK);

        REQUIRE_EQ(srt_bind(socket,
                       reinterpret_cast<const sockaddr*>(&address),
                       static_cast<int>(sizeof(address))),
            SRT_ERROR);
        REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVOP);
    } else {
        int system_error = 0;
        REQUIRE_EQ(srt_getlasterror(&system_error), SRT_ESOCKFAIL);
        REQUIRE(system_error != 0);
        REQUIRE_EQ(srt_getsockstate(socket), SRTS_INIT);
    }

    REQUIRE_EQ(srt_close(socket), 0);
    REQUIRE_EQ(srt_cleanup(), 0);
}

TEST(srt_compat_validates_ipv6only_and_dual_stack_bindings)
{
    REQUIRE_EQ(srt_startup(), 0);

    const SRTSOCKET unspecified = srt_create_socket();
    const SRTSOCKET dual_stack = srt_create_socket();
    const SRTSOCKET ipv4 = srt_create_socket();
    REQUIRE(unspecified != SRT_INVALID_SOCK);
    REQUIRE(dual_stack != SRT_INVALID_SOCK);
    REQUIRE(ipv4 != SRT_INVALID_SOCK);

    std::int32_t ipv6_only = 0;
    int ipv6_only_size = static_cast<int>(sizeof(ipv6_only));
    REQUIRE_EQ(srt_getsockflag(unspecified, SRTO_IPV6ONLY,
                   &ipv6_only, &ipv6_only_size),
        0);
    REQUIRE_EQ(ipv6_only, -1);
    REQUIRE_EQ(ipv6_only_size,
        static_cast<int>(sizeof(ipv6_only)));

    ipv6_only = -2;
    REQUIRE_EQ(srt_setsockflag(unspecified, SRTO_IPV6ONLY,
                   &ipv6_only,
                   static_cast<int>(sizeof(ipv6_only))),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);
    ipv6_only = 2;
    REQUIRE_EQ(srt_setsockflag(unspecified, SRTO_IPV6ONLY,
                   &ipv6_only,
                   static_cast<int>(sizeof(ipv6_only))),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);

    sockaddr_in6 wildcard{};
    wildcard.sin6_family = AF_INET6;
    wildcard.sin6_port = 0;
    wildcard.sin6_addr = in6addr_any;
    REQUIRE_EQ(srt_bind(unspecified,
                   reinterpret_cast<const sockaddr*>(&wildcard),
                   static_cast<int>(sizeof(wildcard))),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);

    ipv6_only = 0;
    REQUIRE_EQ(srt_setsockflag(dual_stack, SRTO_IPV6ONLY,
                   &ipv6_only,
                   static_cast<int>(sizeof(ipv6_only))),
        0);
    if (srt_bind(dual_stack,
            reinterpret_cast<const sockaddr*>(&wildcard),
            static_cast<int>(sizeof(wildcard))) == SRT_ERROR) {
        int system_error = 0;
        REQUIRE_EQ(
            srt_getlasterror(&system_error), SRT_ESOCKFAIL);
        REQUIRE(system_error != 0);
        REQUIRE_EQ(srt_close(ipv4), 0);
        REQUIRE_EQ(srt_close(dual_stack), 0);
        REQUIRE_EQ(srt_close(unspecified), 0);
        REQUIRE_EQ(srt_cleanup(), 0);
        return;
    }

    sockaddr_in6 dual_stack_name{};
    int dual_stack_name_size =
        static_cast<int>(sizeof(dual_stack_name));
    REQUIRE_EQ(srt_getsockname(dual_stack,
                   reinterpret_cast<sockaddr*>(
                       &dual_stack_name),
                   &dual_stack_name_size),
        0);
    REQUIRE(ntohs(dual_stack_name.sin6_port) != 0);

    sockaddr_in ipv4_name{};
    ipv4_name.sin_family = AF_INET;
    ipv4_name.sin_port = dual_stack_name.sin6_port;
    ipv4_name.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE_EQ(srt_bind(ipv4,
                   reinterpret_cast<const sockaddr*>(&ipv4_name),
                   static_cast<int>(sizeof(ipv4_name))),
        SRT_ERROR);
    REQUIRE_EQ(
        srt_getlasterror(nullptr), SRT_EBINDCONFLICT);

    ipv6_only = 1;
    REQUIRE_EQ(srt_setsockflag(dual_stack, SRTO_IPV6ONLY,
                   &ipv6_only,
                   static_cast<int>(sizeof(ipv6_only))),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EBOUNDSOCK);

    REQUIRE_EQ(srt_close(ipv4), 0);
    REQUIRE_EQ(srt_close(dual_stack), 0);
    REQUIRE_EQ(srt_close(unspecified), 0);
    REQUIRE_EQ(srt_cleanup(), 0);
}

TEST(srt_compat_applies_and_reports_prebind_network_options)
{
    REQUIRE_EQ(srt_startup(), 0);
    const SRTSOCKET socket = srt_create_socket();
    REQUIRE(socket != SRT_INVALID_SOCK);

    std::int32_t value = 0;
    int size = static_cast<int>(sizeof(value));
    REQUIRE_EQ(srt_getsockflag(socket, SRTO_IPTTL, &value, &size), 0);
    REQUIRE_EQ(value, 64);
    size = static_cast<int>(sizeof(value));
    REQUIRE_EQ(srt_getsockflag(socket, SRTO_IPTOS, &value, &size), 0);
    REQUIRE_EQ(value, 0xB8);

    value = 42;
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_IPTTL, &value,
                   static_cast<int>(sizeof(value))),
        0);
    value = 0x88;
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_IPTOS, &value,
                   static_cast<int>(sizeof(value))),
        0);
    value = 0;
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_IPTTL, &value,
                   static_cast<int>(sizeof(value))),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);
    value = 256;
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_IPTOS, &value,
                   static_cast<int>(sizeof(value))),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);

    char device[16]{};
    size = static_cast<int>(sizeof(device));
#if defined(__linux__)
    constexpr std::string_view loopback_device = "lo";
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_BINDTODEVICE,
                   loopback_device.data(),
                   static_cast<int>(loopback_device.size())),
        0);
    size = static_cast<int>(sizeof(device) - 1U);
    REQUIRE_EQ(srt_getsockflag(
                   socket, SRTO_BINDTODEVICE, device, &size),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);
    size = static_cast<int>(sizeof(device));
    REQUIRE_EQ(srt_getsockflag(
                   socket, SRTO_BINDTODEVICE, device, &size),
        0);
    REQUIRE_EQ(std::string_view(device, static_cast<std::size_t>(size)),
        loopback_device);
#else
    REQUIRE_EQ(srt_setsockflag(
                   socket, SRTO_BINDTODEVICE, "lo", 2),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVOP);
    REQUIRE_EQ(srt_getsockflag(
                   socket, SRTO_BINDTODEVICE, device, &size),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVOP);
#endif

#if defined(__linux__)
    // Do not require interface-binding capabilities from the test runner.
    const char empty_device = '\0';
    REQUIRE_EQ(srt_setsockflag(
                   socket, SRTO_BINDTODEVICE, &empty_device, 0),
        0);
#endif
    sockaddr_in requested{};
    requested.sin_family = AF_INET;
    requested.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (srt_bind(socket,
            reinterpret_cast<const sockaddr*>(&requested),
            static_cast<int>(sizeof(requested))) == 0) {
        value = 0;
        size = static_cast<int>(sizeof(value));
        REQUIRE_EQ(srt_getsockflag(socket, SRTO_IPTTL,
                       &value, &size),
            0);
        REQUIRE_EQ(value, 42);
        size = static_cast<int>(sizeof(value));
        REQUIRE_EQ(srt_getsockflag(socket, SRTO_IPTOS,
                       &value, &size),
            0);
        REQUIRE_EQ(value, 0x88);
        value = 43;
        REQUIRE_EQ(srt_setsockflag(socket, SRTO_IPTTL, &value,
                       static_cast<int>(sizeof(value))),
            SRT_ERROR);
        REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EBOUNDSOCK);
    }

    REQUIRE_EQ(srt_close(socket), 0);
    REQUIRE_EQ(srt_cleanup(), 0);
}

TEST(srt_compat_rejects_explicit_ipv6_tos_on_winsock)
{
#if defined(_WIN32)
    REQUIRE_EQ(srt_startup(), 0);
    const SRTSOCKET socket = srt_create_socket();
    REQUIRE(socket != SRT_INVALID_SOCK);

    std::int32_t tos = 0x88;
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_IPTOS, &tos,
                   static_cast<int>(sizeof(tos))),
        0);

    sockaddr_in6 requested{};
    requested.sin6_family = AF_INET6;
    requested.sin6_port = 0;
    requested.sin6_addr = in6addr_loopback;
    REQUIRE_EQ(srt_bind(socket,
                   reinterpret_cast<const sockaddr*>(&requested),
                   static_cast<int>(sizeof(requested))),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVOP);

    REQUIRE_EQ(srt_close(socket), 0);
    REQUIRE_EQ(srt_cleanup(), 0);
#else
    REQUIRE(true);
#endif
}

TEST(srt_compat_reuseaddr_shares_compatible_ipv4_bindings)
{
    sockaddr_in requested{};
    requested.sin_family = AF_INET;
    requested.sin_port = 0;
    requested.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    const SRTSOCKET first = srt_create_socket();
    const SRTSOCKET second = srt_create_socket();
    const SRTSOCKET disabled = srt_create_socket();
    const SRTSOCKET incompatible = srt_create_socket();
    REQUIRE(first != SRT_INVALID_SOCK);
    REQUIRE(second != SRT_INVALID_SOCK);
    REQUIRE(disabled != SRT_INVALID_SOCK);
    REQUIRE(incompatible != SRT_INVALID_SOCK);

    bool reuse = false;
    int reuse_size = static_cast<int>(sizeof(reuse));
    REQUIRE_EQ(srt_getsockflag(
                   first, SRTO_REUSEADDR,
                   &reuse, &reuse_size),
        0);
    REQUIRE(reuse);
    REQUIRE_EQ(reuse_size, static_cast<int>(sizeof(reuse)));
    const std::int32_t invalid_boolean_value = 2;
    REQUIRE_EQ(srt_setsockflag(
                   disabled, SRTO_REUSEADDR,
                   &invalid_boolean_value,
                   static_cast<int>(
                       sizeof(invalid_boolean_value))),
        SRT_ERROR);
    REQUIRE_EQ(
        srt_getlasterror(nullptr),
        SRT_EINVPARAM);

    const int first_bind = srt_bind(
        first,
        reinterpret_cast<const sockaddr*>(&requested),
        static_cast<int>(sizeof(requested)));
    if (first_bind == SRT_ERROR) {
        int system_error = 0;
        REQUIRE_EQ(
            srt_getlasterror(&system_error),
            SRT_ESOCKFAIL);
        REQUIRE(system_error != 0);
        REQUIRE_EQ(srt_close(incompatible), 0);
        REQUIRE_EQ(srt_close(disabled), 0);
        REQUIRE_EQ(srt_close(second), 0);
        REQUIRE_EQ(srt_close(first), 0);
        REQUIRE_EQ(srt_cleanup(), 0);
        return;
    }

    sockaddr_in local{};
    int local_size = static_cast<int>(sizeof(local));
    REQUIRE_EQ(srt_getsockname(
                   first,
                   reinterpret_cast<sockaddr*>(&local),
                   &local_size),
        0);
    REQUIRE(ntohs(local.sin_port) != 0);

    REQUIRE_EQ(srt_bind(
                   second,
                   reinterpret_cast<const sockaddr*>(&local),
                   static_cast<int>(sizeof(local))),
        0);
    const auto first_record =
        robotweax::srt::compat::SocketRegistry::instance()
            .find(first);
    const auto second_record =
        robotweax::srt::compat::SocketRegistry::instance()
            .find(second);
    REQUIRE(first_record != nullptr);
    REQUIRE(second_record != nullptr);
    REQUIRE(first_record->channel != nullptr);
    REQUIRE(first_record->channel
        == second_record->channel);

    reuse = false;
    REQUIRE_EQ(srt_setsockflag(
                   disabled, SRTO_REUSEADDR,
                   &reuse,
                   static_cast<int>(sizeof(reuse))),
        0);
    REQUIRE_EQ(srt_bind(
                   disabled,
                   reinterpret_cast<const sockaddr*>(&local),
                   static_cast<int>(sizeof(local))),
        SRT_ERROR);
    REQUIRE_EQ(
        srt_getlasterror(nullptr),
        SRT_EBINDCONFLICT);

    std::int32_t receive_buffer = 256'000;
    REQUIRE_EQ(srt_setsockflag(
                   incompatible, SRTO_UDP_RCVBUF,
                   &receive_buffer,
                   static_cast<int>(
                       sizeof(receive_buffer))),
        0);
    REQUIRE_EQ(srt_bind(
                   incompatible,
                   reinterpret_cast<const sockaddr*>(&local),
                   static_cast<int>(sizeof(local))),
        SRT_ERROR);
    REQUIRE_EQ(
        srt_getlasterror(nullptr),
        SRT_EBINDCONFLICT);

    reuse = false;
    REQUIRE_EQ(srt_setsockflag(
                   second, SRTO_REUSEADDR,
                   &reuse,
                   static_cast<int>(sizeof(reuse))),
        SRT_ERROR);
    REQUIRE_EQ(
        srt_getlasterror(nullptr),
        SRT_EBOUNDSOCK);

    REQUIRE_EQ(srt_listen(first, 1), 0);
    REQUIRE_EQ(srt_listen(second, 1), SRT_ERROR);
    REQUIRE_EQ(
        srt_getlasterror(nullptr),
        SRT_EDUPLISTEN);
    REQUIRE_EQ(srt_close(first), 0);

    local_size = static_cast<int>(sizeof(local));
    REQUIRE_EQ(srt_getsockname(
                   second,
                   reinterpret_cast<sockaddr*>(&local),
                   &local_size),
        0);
    REQUIRE_EQ(srt_listen(second, 1), 0);

    REQUIRE_EQ(srt_close(incompatible), 0);
    REQUIRE_EQ(srt_close(disabled), 0);
    REQUIRE_EQ(srt_close(second), 0);
    REQUIRE_EQ(srt_cleanup(), 0);
}

TEST(srt_compat_bind_acquire_owns_a_bound_unconnected_udp_socket)
{
    const UDPSOCKET native_socket =
        create_udp_socket();
#if defined(_WIN32)
    REQUIRE(native_socket != INVALID_SOCKET);
#else
    REQUIRE(native_socket >= 0);
#endif

    sockaddr_in requested{};
    requested.sin_family = AF_INET;
    requested.sin_port = 0;
    requested.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const int native_bind = ::bind(
        native_socket,
        reinterpret_cast<const sockaddr*>(&requested),
        static_cast<NativeSocketLength>(
            sizeof(requested)));
    if (native_bind != 0) {
        close_udp_socket(native_socket);
        return;
    }

    sockaddr_in expected{};
    NativeSocketLength expected_size =
        static_cast<NativeSocketLength>(
            sizeof(expected));
    REQUIRE_EQ(::getsockname(
                   native_socket,
                   reinterpret_cast<sockaddr*>(&expected),
                   &expected_size),
        0);
    REQUIRE(ntohs(expected.sin_port) != 0);

    const SRTSOCKET socket = srt_create_socket();
    REQUIRE(socket != SRT_INVALID_SOCK);
    REQUIRE_EQ(
        srt_bind_acquire(socket, native_socket),
        0);
    REQUIRE_EQ(srt_getsockstate(socket), SRTS_OPENED);

    sockaddr_in actual{};
    int actual_size = static_cast<int>(sizeof(actual));
    REQUIRE_EQ(srt_getsockname(
                   socket,
                   reinterpret_cast<sockaddr*>(&actual),
                   &actual_size),
        0);
    REQUIRE_EQ(actual.sin_family, AF_INET);
    REQUIRE_EQ(actual.sin_port, expected.sin_port);
    REQUIRE_EQ(
        actual.sin_addr.s_addr,
        expected.sin_addr.s_addr);

    REQUIRE_EQ(srt_close(socket), 0);
    REQUIRE(udp_socket_is_closed(native_socket));
    REQUIRE_EQ(srt_cleanup(), 0);
}

TEST(srt_compat_bind_acquire_owns_an_ipv6_udp_socket)
{
    const UDPSOCKET native_socket =
        create_udp_socket(AF_INET6);
#if defined(_WIN32)
    if (native_socket == INVALID_SOCKET) {
        return;
    }
#else
    if (native_socket < 0) {
        return;
    }
#endif

    sockaddr_in6 requested{};
    requested.sin6_family = AF_INET6;
    requested.sin6_port = 0;
    requested.sin6_addr = in6addr_loopback;
    if (::bind(native_socket,
            reinterpret_cast<const sockaddr*>(&requested),
            static_cast<NativeSocketLength>(
                sizeof(requested)))
        != 0) {
        close_udp_socket(native_socket);
        return;
    }

    sockaddr_in6 expected{};
    NativeSocketLength expected_size =
        static_cast<NativeSocketLength>(sizeof(expected));
    REQUIRE_EQ(::getsockname(native_socket,
                   reinterpret_cast<sockaddr*>(&expected),
                   &expected_size),
        0);
    REQUIRE(ntohs(expected.sin6_port) != 0U);

    const SRTSOCKET socket = srt_create_socket();
    REQUIRE(socket != SRT_INVALID_SOCK);
    REQUIRE_EQ(srt_bind_acquire(socket, native_socket), 0);
    REQUIRE_EQ(srt_getsockstate(socket), SRTS_OPENED);

    sockaddr_in6 actual{};
    int actual_size = static_cast<int>(sizeof(actual));
    REQUIRE_EQ(srt_getsockname(socket,
                   reinterpret_cast<sockaddr*>(&actual),
                   &actual_size),
        0);
    REQUIRE_EQ(actual_size,
        static_cast<int>(sizeof(sockaddr_in6)));
    REQUIRE_EQ(actual.sin6_family, AF_INET6);
    REQUIRE_EQ(actual.sin6_port, expected.sin6_port);
    REQUIRE_EQ(actual.sin6_scope_id, expected.sin6_scope_id);
    REQUIRE_EQ(std::memcmp(&actual.sin6_addr,
                   &expected.sin6_addr,
                   sizeof(actual.sin6_addr)),
        0);

    int native_ipv6_only = -1;
    NativeSocketLength native_ipv6_only_size =
        static_cast<NativeSocketLength>(
            sizeof(native_ipv6_only));
#if defined(_WIN32)
    REQUIRE_EQ(::getsockopt(native_socket, IPPROTO_IPV6,
                   IPV6_V6ONLY,
                   reinterpret_cast<char*>(&native_ipv6_only),
                   &native_ipv6_only_size),
        0);
#else
    REQUIRE_EQ(::getsockopt(native_socket, IPPROTO_IPV6,
                   IPV6_V6ONLY, &native_ipv6_only,
                   &native_ipv6_only_size),
        0);
#endif
    std::int32_t reported_ipv6_only = -1;
    int reported_ipv6_only_size =
        static_cast<int>(sizeof(reported_ipv6_only));
    REQUIRE_EQ(srt_getsockflag(socket, SRTO_IPV6ONLY,
                   &reported_ipv6_only,
                   &reported_ipv6_only_size),
        0);
    REQUIRE_EQ(reported_ipv6_only,
        native_ipv6_only != 0 ? 1 : 0);

    std::int32_t reported_tos = 0;
    int reported_tos_size = static_cast<int>(sizeof(reported_tos));
    REQUIRE_EQ(srt_getsockflag(socket, SRTO_IPTOS,
                   &reported_tos, &reported_tos_size),
        0);
    REQUIRE_EQ(reported_tos, 0xB8);

    REQUIRE_EQ(srt_close(socket), 0);
    REQUIRE(udp_socket_is_closed(native_socket));
    REQUIRE_EQ(srt_cleanup(), 0);
}

TEST(srt_compat_bind_acquire_rejects_unbound_and_connected_udp)
{
    const UDPSOCKET unbound =
        create_udp_socket();
#if defined(_WIN32)
    REQUIRE(unbound != INVALID_SOCKET);
#else
    REQUIRE(unbound >= 0);
#endif
    const SRTSOCKET unbound_srt = srt_create_socket();
    REQUIRE(unbound_srt != SRT_INVALID_SOCK);
    REQUIRE_EQ(
        srt_bind_peerof(unbound_srt, unbound),
        SRT_ERROR);
    REQUIRE_EQ(
        srt_getlasterror(nullptr),
        SRT_EINVPARAM);
    REQUIRE(!udp_socket_is_closed(unbound));
    close_udp_socket(unbound);
    REQUIRE_EQ(srt_close(unbound_srt), 0);

    const UDPSOCKET receiver =
        create_udp_socket();
    const UDPSOCKET connected =
        create_udp_socket();
#if defined(_WIN32)
    REQUIRE(receiver != INVALID_SOCKET);
    REQUIRE(connected != INVALID_SOCKET);
#else
    REQUIRE(receiver >= 0);
    REQUIRE(connected >= 0);
#endif

    sockaddr_in receiver_address{};
    receiver_address.sin_family = AF_INET;
    receiver_address.sin_port = 0;
    receiver_address.sin_addr.s_addr =
        htonl(INADDR_LOOPBACK);
    if (::bind(receiver,
            reinterpret_cast<const sockaddr*>(
                &receiver_address),
            static_cast<NativeSocketLength>(
                sizeof(receiver_address)))
        != 0) {
        close_udp_socket(connected);
        close_udp_socket(receiver);
        REQUIRE_EQ(srt_cleanup(), 0);
        return;
    }
    NativeSocketLength receiver_size =
        static_cast<NativeSocketLength>(
            sizeof(receiver_address));
    REQUIRE_EQ(::getsockname(
                   receiver,
                   reinterpret_cast<sockaddr*>(
                       &receiver_address),
                   &receiver_size),
        0);

    sockaddr_in connected_address{};
    connected_address.sin_family = AF_INET;
    connected_address.sin_port = 0;
    connected_address.sin_addr.s_addr =
        htonl(INADDR_LOOPBACK);
    REQUIRE_EQ(::bind(
                   connected,
                   reinterpret_cast<const sockaddr*>(
                       &connected_address),
                   static_cast<NativeSocketLength>(
                       sizeof(connected_address))),
        0);
    REQUIRE_EQ(::connect(
                   connected,
                   reinterpret_cast<const sockaddr*>(
                       &receiver_address),
                   static_cast<NativeSocketLength>(
                       sizeof(receiver_address))),
        0);

    const SRTSOCKET connected_srt =
        srt_create_socket();
    REQUIRE(connected_srt != SRT_INVALID_SOCK);
    REQUIRE_EQ(
        srt_bind_acquire(connected_srt, connected),
        SRT_ERROR);
    REQUIRE_EQ(
        srt_getlasterror(nullptr),
        SRT_EINVPARAM);
    REQUIRE(!udp_socket_is_closed(connected));

    close_udp_socket(connected);
    close_udp_socket(receiver);
    REQUIRE_EQ(srt_close(connected_srt), 0);
    REQUIRE_EQ(srt_cleanup(), 0);
}

TEST(srt_compat_shared_binding_routes_sequential_connections)
{
    constexpr std::int32_t timeout_milliseconds = 2'000;
    sockaddr_in any_loopback{};
    any_loopback.sin_family = AF_INET;
    any_loopback.sin_port = 0;
    any_loopback.sin_addr.s_addr =
        htonl(INADDR_LOOPBACK);

    const SRTSOCKET listener = srt_create_socket();
    const SRTSOCKET first = srt_create_socket();
    const SRTSOCKET second = srt_create_socket();
    REQUIRE(listener != SRT_INVALID_SOCK);
    REQUIRE(first != SRT_INVALID_SOCK);
    REQUIRE(second != SRT_INVALID_SOCK);
    for (const SRTSOCKET socket :
        {listener, first, second}) {
        REQUIRE_EQ(srt_setsockflag(
                       socket, SRTO_CONNTIMEO,
                       &timeout_milliseconds,
                       static_cast<int>(
                           sizeof(timeout_milliseconds))),
            0);
        REQUIRE_EQ(srt_setsockflag(
                       socket, SRTO_RCVTIMEO,
                       &timeout_milliseconds,
                       static_cast<int>(
                           sizeof(timeout_milliseconds))),
            0);
    }

    if (srt_bind(
            listener,
            reinterpret_cast<const sockaddr*>(
                &any_loopback),
            static_cast<int>(sizeof(any_loopback)))
        == SRT_ERROR) {
        REQUIRE_EQ(
            srt_getlasterror(nullptr),
            SRT_ESOCKFAIL);
        REQUIRE_EQ(srt_close(second), 0);
        REQUIRE_EQ(srt_close(first), 0);
        REQUIRE_EQ(srt_close(listener), 0);
        REQUIRE_EQ(srt_cleanup(), 0);
        return;
    }
    REQUIRE_EQ(srt_listen(listener, 4), 0);
    sockaddr_in listener_name{};
    int listener_name_size =
        static_cast<int>(sizeof(listener_name));
    REQUIRE_EQ(srt_getsockname(
                   listener,
                   reinterpret_cast<sockaddr*>(
                       &listener_name),
                   &listener_name_size),
        0);

    REQUIRE_EQ(srt_bind(
                   first,
                   reinterpret_cast<const sockaddr*>(
                       &any_loopback),
                   static_cast<int>(
                       sizeof(any_loopback))),
        0);
    sockaddr_in shared_name{};
    int shared_name_size =
        static_cast<int>(sizeof(shared_name));
    REQUIRE_EQ(srt_getsockname(
                   first,
                   reinterpret_cast<sockaddr*>(
                       &shared_name),
                   &shared_name_size),
        0);
    REQUIRE_EQ(srt_bind(
                   second,
                   reinterpret_cast<const sockaddr*>(
                       &shared_name),
                   static_cast<int>(
                       sizeof(shared_name))),
        0);

    std::array<std::atomic<SRTSOCKET>, 2>
        accepted{
            SRT_INVALID_SOCK,
            SRT_INVALID_SOCK,
        };
    std::thread accept_thread([&] {
        for (auto& socket : accepted) {
            socket.store(
                srt_accept(listener, nullptr, nullptr));
            if (socket.load() == SRT_INVALID_SOCK) {
                break;
            }
        }
    });

    const int first_connect = srt_connect(
        first,
        reinterpret_cast<const sockaddr*>(
            &listener_name),
        static_cast<int>(sizeof(listener_name)));
    const int second_connect =
        first_connect == 0
        ? srt_connect(
              second,
              reinterpret_cast<const sockaddr*>(
                  &listener_name),
              static_cast<int>(
                  sizeof(listener_name)))
        : SRT_ERROR;
    if (first_connect == SRT_ERROR
        || second_connect == SRT_ERROR) {
        (void)srt_close(listener);
    }
    accept_thread.join();

    REQUIRE_EQ(first_connect, 0);
    REQUIRE_EQ(second_connect, 0);
    REQUIRE(
        accepted[0].load() != SRT_INVALID_SOCK);
    REQUIRE(
        accepted[1].load() != SRT_INVALID_SOCK);

    constexpr char first_payload[] = "first";
    constexpr char second_payload[] = "second";
    REQUIRE_EQ(srt_send(
                   first, first_payload,
                   static_cast<int>(
                       sizeof(first_payload))),
        static_cast<int>(sizeof(first_payload)));
    REQUIRE_EQ(srt_send(
                   second, second_payload,
                   static_cast<int>(
                       sizeof(second_payload))),
        static_cast<int>(sizeof(second_payload)));

    std::array<char, SRT_LIVE_DEF_PLSIZE> received {};
    REQUIRE_EQ(srt_recv(
                   accepted[0].load(),
                   received.data(),
                   static_cast<int>(received.size())),
        static_cast<int>(sizeof(first_payload)));
    REQUIRE_EQ(std::memcmp(
                   received.data(),
                   first_payload,
                   sizeof(first_payload)),
        0);
    received.fill(0);
    REQUIRE_EQ(srt_recv(
                   accepted[1].load(),
                   received.data(),
                   static_cast<int>(received.size())),
        static_cast<int>(sizeof(second_payload)));
    REQUIRE_EQ(std::memcmp(
                   received.data(),
                   second_payload,
                   sizeof(second_payload)),
        0);

    REQUIRE_EQ(srt_close(first), 0);
    const int eid = srt_epoll_create();
    REQUIRE(eid >= 0);
    int watched_events = SRT_EPOLL_IN;
    REQUIRE_EQ(srt_epoll_add_usock(
                   eid, accepted[1].load(),
                   &watched_events),
        0);
    constexpr char survivor_payload[] = "survivor";
    REQUIRE_EQ(srt_send(
                   second, survivor_payload,
                   static_cast<int>(
                       sizeof(survivor_payload))),
        static_cast<int>(
            sizeof(survivor_payload)));
    SRTSOCKET readable = SRT_INVALID_SOCK;
    int readable_count = 1;
    int writable_count = 0;
    REQUIRE_EQ(srt_epoll_wait(
                   eid,
                   &readable, &readable_count,
                   nullptr, &writable_count,
                   timeout_milliseconds,
                   nullptr, nullptr,
                   nullptr, nullptr),
        1);
    REQUIRE_EQ(readable_count, 1);
    REQUIRE_EQ(readable, accepted[1].load());
    received.fill(0);
    REQUIRE_EQ(srt_recv(
                   accepted[1].load(),
                   received.data(),
                   static_cast<int>(received.size())),
        static_cast<int>(
            sizeof(survivor_payload)));
    REQUIRE_EQ(std::memcmp(
                   received.data(),
                   survivor_payload,
                   sizeof(survivor_payload)),
        0);

    REQUIRE_EQ(srt_epoll_release(eid), 0);
    REQUIRE_EQ(srt_close(second), 0);
    REQUIRE_EQ(srt_close(accepted[1].load()), 0);
    REQUIRE_EQ(srt_close(accepted[0].load()), 0);
    REQUIRE_EQ(srt_close(listener), 0);
    REQUIRE_EQ(srt_cleanup(), 0);
}

TEST(srt_compat_connection_api_validates_state_and_arguments)
{
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(9'999);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    REQUIRE_EQ(srt_listen(987'654'321, 0), SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);
    REQUIRE_EQ(srt_listen(987'654'321, 1), SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVSOCK);

    const SRTSOCKET socket = srt_create_socket();
    REQUIRE(socket != SRT_INVALID_SOCK);
    REQUIRE_EQ(srt_listen(socket, 1), SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EUNBOUNDSOCK);

    REQUIRE_EQ(srt_accept(socket, nullptr, nullptr), SRT_INVALID_SOCK);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ENOLISTEN);

    sockaddr_in peer{};
    int peer_size = static_cast<int>(sizeof(peer));
    REQUIRE_EQ(srt_getpeername(socket,
                   reinterpret_cast<sockaddr*>(&peer), &peer_size),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ENOCONN);

    REQUIRE_EQ(srt_connect(socket, nullptr, 0), SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);

    std::array<char, 1'500> message_buffer{};
    REQUIRE_EQ(srt_send(987'654'321,
                   message_buffer.data(), 1),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVSOCK);
    REQUIRE_EQ(srt_send(socket, message_buffer.data(), 1), SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ENOCONN);
    REQUIRE_EQ(srt_recv(socket, message_buffer.data(),
                   static_cast<int>(message_buffer.size())),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ENOCONN);

    sockaddr_in zero_port = address;
    zero_port.sin_port = 0;
    REQUIRE_EQ(srt_connect(socket,
                   reinterpret_cast<const sockaddr*>(&zero_port),
                   static_cast<int>(sizeof(zero_port))),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);

    const bool asynchronous = false;
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_RCVSYN,
                   &asynchronous, static_cast<int>(sizeof(asynchronous))),
        0);
    const int asynchronous_connect = srt_connect(socket,
        reinterpret_cast<const sockaddr*>(&address),
        static_cast<int>(sizeof(address)));
    if (asynchronous_connect == 0) {
        REQUIRE_EQ(srt_getsockstate(socket), SRTS_CONNECTING);
    } else {
        REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ESOCKFAIL);
        REQUIRE_EQ(srt_getsockstate(socket), SRTS_INIT);
    }

    REQUIRE_EQ(srt_close(socket), 0);
    REQUIRE_EQ(srt_listen(socket, 1), SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ESCLOSED);
    REQUIRE_EQ(srt_cleanup(), 0);
}

TEST(srt_compat_broken_socket_cannot_be_reconnected)
{
    const SRTSOCKET broken = srt_create_socket();
    const SRTSOCKET replacement = srt_create_socket();
    REQUIRE(broken != SRT_INVALID_SOCK);
    REQUIRE(replacement != SRT_INVALID_SOCK);
    const auto record =
        robotweax::srt::compat::SocketRegistry::instance().find(broken);
    REQUIRE(record != nullptr);
    {
        std::lock_guard lock(record->mutex);
        record->state = SRTS_BROKEN;
    }

    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_port = htons(9'999);
    peer.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE_EQ(srt_connect(broken,
                   reinterpret_cast<const sockaddr*>(&peer),
                   static_cast<int>(sizeof(peer))),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ESCLOSED);
    REQUIRE_EQ(srt_getsockstate(broken), SRTS_BROKEN);
    REQUIRE_EQ(srt_getsockstate(replacement), SRTS_INIT);

    REQUIRE_EQ(srt_close(replacement), 0);
    REQUIRE_EQ(srt_close(broken), 0);
    REQUIRE_EQ(srt_cleanup(), 0);
}

TEST(srt_compat_rendezvous_option_enforces_socket_lifecycle)
{
    const SRTSOCKET socket = srt_create_socket();
    REQUIRE(socket != SRT_INVALID_SOCK);

    const bool enabled = true;
    REQUIRE_EQ(srt_setsockflag(socket,
                   SRTO_RENDEZVOUS,
                   &enabled,
                   static_cast<int>(sizeof(enabled))),
        0);
    bool actual = false;
    int actual_size =
        static_cast<int>(sizeof(actual));
    REQUIRE_EQ(srt_getsockflag(socket,
                   SRTO_RENDEZVOUS,
                   &actual, &actual_size),
        0);
    REQUIRE(actual);

    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_port = htons(9'999);
    peer.sin_addr.s_addr =
        htonl(INADDR_LOOPBACK);
    REQUIRE_EQ(srt_connect(socket,
                   reinterpret_cast<const sockaddr*>(&peer),
                   static_cast<int>(sizeof(peer))),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr),
        SRT_ERDVUNBOUND);
    REQUIRE_EQ(srt_listen(socket, 1),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr),
        SRT_ERDVNOSERV);

    REQUIRE_EQ(srt_close(socket), 0);
    REQUIRE_EQ(srt_cleanup(), 0);
}

TEST(srt_compat_sender_option_is_pre_connection_and_round_trips)
{
    const SRTSOCKET socket = srt_create_socket();
    REQUIRE(socket != SRT_INVALID_SOCK);

    bool actual = true;
    int actual_size = static_cast<int>(sizeof(actual));
    REQUIRE_EQ(srt_getsockflag(socket, SRTO_SENDER,
                   &actual, &actual_size),
        0);
    REQUIRE(!actual);

    const bool enabled = true;
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_SENDER,
                   &enabled, static_cast<int>(sizeof(enabled))),
        0);
    actual = false;
    actual_size = static_cast<int>(sizeof(actual));
    REQUIRE_EQ(srt_getsockflag(socket, SRTO_SENDER,
                   &actual, &actual_size),
        0);
    REQUIRE(actual);

    const auto record =
        robotweax::srt::compat::SocketRegistry::instance().find(socket);
    REQUIRE(record != nullptr);
    {
        std::lock_guard lock(record->mutex);
        record->state = SRTS_CONNECTING;
    }
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_SENDER,
                   &enabled, static_cast<int>(sizeof(enabled))),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ECONNSOCK);

    REQUIRE_EQ(srt_close(socket), 0);
    REQUIRE_EQ(srt_cleanup(), 0);
}

TEST(srt_compat_tlpktdrop_and_boolean_options_accept_integer_abi)
{
    struct BooleanOptionCase {
        SRT_SOCKOPT option;
        int value;
    };
    constexpr std::array<BooleanOptionCase, 11> cases{{
        {SRTO_SNDSYN, 0},
        {SRTO_RCVSYN, 0},
        {SRTO_REUSEADDR, 0},
        {SRTO_SENDER, 1},
        {SRTO_TSBPDMODE, 0},
        {SRTO_TLPKTDROP, 0},
        {SRTO_NAKREPORT, 0},
        {SRTO_MESSAGEAPI, 0},
        {SRTO_RENDEZVOUS, 1},
        {SRTO_ENFORCEDENCRYPTION, 0},
        {SRTO_GROUPCONNECT, 1},
    }};

    for (const auto& test_case : cases) {
        const SRTSOCKET socket = srt_create_socket();
        REQUIRE(socket != SRT_INVALID_SOCK);
        REQUIRE_EQ(srt_setsockflag(socket, test_case.option,
                       &test_case.value,
                       static_cast<int>(sizeof(test_case.value))),
            0);

        bool actual = test_case.value == 0;
        int actual_size = static_cast<int>(sizeof(actual));
        REQUIRE_EQ(srt_getsockflag(socket, test_case.option,
                       &actual, &actual_size),
            0);
        REQUIRE_EQ(actual, test_case.value != 0);
        REQUIRE_EQ(actual_size, static_cast<int>(sizeof(actual)));

        const int invalid_boolean = 2;
        REQUIRE_EQ(srt_setsockflag(socket, test_case.option,
                       &invalid_boolean,
                       static_cast<int>(sizeof(invalid_boolean))),
            SRT_ERROR);
        REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);
        REQUIRE_EQ(srt_close(socket), 0);
    }
    REQUIRE_EQ(srt_cleanup(), 0);
}

#ifdef ENABLE_AEAD_API_PREVIEW
TEST(srt_compat_crypto_mode_preview_matches_upstream_option_contract)
{
    REQUIRE_EQ(srt_startup(), 0);
    const SRTSOCKET socket = srt_create_socket();
    REQUIRE(socket != SRT_INVALID_SOCK);

    std::int32_t mode = -1;
    int size = static_cast<int>(sizeof(mode));
    REQUIRE_EQ(srt_getsockflag(socket, SRTO_CRYPTOMODE, &mode, &size), 0);
    REQUIRE_EQ(mode, 0);
    REQUIRE_EQ(size, static_cast<int>(sizeof(mode)));

    mode = 3;
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_CRYPTOMODE, &mode,
                   static_cast<int>(sizeof(mode))),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);

    mode = 2;
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_CRYPTOMODE, &mode,
                   static_cast<int>(sizeof(mode))),
        0);
    const bool rendezvous = true;
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_RENDEZVOUS, &rendezvous,
                   static_cast<int>(sizeof(rendezvous))),
        0);
    bool tsbpd = false;
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_TSBPDMODE, &tsbpd,
                   static_cast<int>(sizeof(tsbpd))),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);

    mode = -1;
    size = static_cast<int>(sizeof(mode));
    REQUIRE_EQ(srt_getsockflag(socket, SRTO_CRYPTOMODE, &mode, &size), 0);
    REQUIRE_EQ(mode, 2);

    const SRTSOCKET no_tsbpd = srt_create_socket();
    REQUIRE(no_tsbpd != SRT_INVALID_SOCK);
    REQUIRE_EQ(srt_setsockflag(no_tsbpd, SRTO_TSBPDMODE, &tsbpd,
                   static_cast<int>(sizeof(tsbpd))),
        0);
    mode = 2;
    REQUIRE_EQ(srt_setsockflag(no_tsbpd, SRTO_CRYPTOMODE, &mode,
                   static_cast<int>(sizeof(mode))),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);

    const SRTSOCKET rendezvous_first = srt_create_socket();
    REQUIRE(rendezvous_first != SRT_INVALID_SOCK);
    REQUIRE_EQ(srt_setsockflag(rendezvous_first, SRTO_RENDEZVOUS, &rendezvous,
                   static_cast<int>(sizeof(rendezvous))),
        0);
    mode = 2;
    REQUIRE_EQ(srt_setsockflag(rendezvous_first, SRTO_CRYPTOMODE, &mode,
                   static_cast<int>(sizeof(mode))),
        0);

    const SRTSOCKET file_first = srt_create_socket();
    REQUIRE(file_first != SRT_INVALID_SOCK);
    const SRT_TRANSTYPE file_type = SRTT_FILE;
    REQUIRE_EQ(srt_setsockflag(file_first, SRTO_TRANSTYPE, &file_type,
                   static_cast<int>(sizeof(file_type))),
        0);
    REQUIRE_EQ(srt_setsockflag(file_first, SRTO_CRYPTOMODE, &mode,
                   static_cast<int>(sizeof(mode))),
        0);
    std::int32_t file_mode = -1;
    int file_mode_size = static_cast<int>(sizeof(file_mode));
    REQUIRE_EQ(srt_getsockflag(
                   file_first, SRTO_CRYPTOMODE, &file_mode, &file_mode_size),
        0);
    REQUIRE_EQ(file_mode, 2);

    const SRTSOCKET gcm_first = srt_create_socket();
    REQUIRE(gcm_first != SRT_INVALID_SOCK);
    REQUIRE_EQ(srt_setsockflag(gcm_first, SRTO_CRYPTOMODE, &mode,
                   static_cast<int>(sizeof(mode))),
        0);
    REQUIRE_EQ(srt_setsockflag(gcm_first, SRTO_TRANSTYPE, &file_type,
                   static_cast<int>(sizeof(file_type))),
        0);

    REQUIRE_EQ(srt_close(gcm_first), 0);
    REQUIRE_EQ(srt_close(file_first), 0);
    REQUIRE_EQ(srt_close(rendezvous_first), 0);
    REQUIRE_EQ(srt_close(no_tsbpd), 0);
    REQUIRE_EQ(srt_close(socket), 0);
    REQUIRE_EQ(srt_cleanup(), 0);
}
#endif

TEST(srt_compat_file_type_exposes_the_implemented_reference_option_bundle)
{
    const SRTSOCKET socket = srt_create_socket();
    REQUIRE(socket != SRT_INVALID_SOCK);
    const SRT_TRANSTYPE type = SRTT_FILE;
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_TRANSTYPE,
                   &type, static_cast<int>(sizeof(type))),
        0);

    SRT_TRANSTYPE actual_type = SRTT_INVALID;
    int size = static_cast<int>(sizeof(actual_type));
    REQUIRE_EQ(srt_getsockflag(socket, SRTO_TRANSTYPE,
                   &actual_type, &size),
        0);
    REQUIRE_EQ(actual_type, SRTT_FILE);

    bool enabled = true;
    size = static_cast<int>(sizeof(enabled));
    REQUIRE_EQ(srt_getsockflag(socket, SRTO_MESSAGEAPI,
                   &enabled, &size),
        0);
    REQUIRE(!enabled);
    size = static_cast<int>(sizeof(enabled));
    REQUIRE_EQ(srt_getsockflag(socket, SRTO_TSBPDMODE,
                   &enabled, &size),
        0);
    REQUIRE(!enabled);
    size = static_cast<int>(sizeof(enabled));
    REQUIRE_EQ(srt_getsockflag(socket, SRTO_TLPKTDROP,
                   &enabled, &size),
        0);
    REQUIRE(!enabled);
    size = static_cast<int>(sizeof(enabled));
    REQUIRE_EQ(srt_getsockflag(socket, SRTO_NAKREPORT,
                   &enabled, &size),
        0);
    REQUIRE(!enabled);

    std::int32_t value = 0;
    size = static_cast<int>(sizeof(value));
    REQUIRE_EQ(srt_getsockflag(socket, SRTO_PAYLOADSIZE,
                   &value, &size),
        0);
    REQUIRE_EQ(value, SRT_LIVE_MAX_PLSIZE);
    size = static_cast<int>(sizeof(value));
    REQUIRE_EQ(srt_getsockflag(socket, SRTO_RETRANSMITALGO,
                   &value, &size),
        0);
    REQUIRE_EQ(value, 0);
    size = static_cast<int>(sizeof(value));
    REQUIRE_EQ(srt_getsockflag(socket, SRTO_SNDDROPDELAY,
                   &value, &size),
        0);
    REQUIRE_EQ(value, -1);

    linger linger_value{};
    size = static_cast<int>(sizeof(linger_value));
    REQUIRE_EQ(srt_getsockflag(socket, SRTO_LINGER,
                   &linger_value, &size),
        0);
    REQUIRE_EQ(linger_value.l_onoff, 1);
    REQUIRE_EQ(linger_value.l_linger, 180);

    linger_value = {
        .l_onoff = 1,
        .l_linger = 7,
    };
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_LINGER,
                   &linger_value,
                   static_cast<int>(sizeof(linger_value))),
        0);
    linger_value = {};
    size = static_cast<int>(sizeof(linger_value));
    REQUIRE_EQ(srt_getsockflag(socket, SRTO_LINGER,
                   &linger_value, &size),
        0);
    REQUIRE_EQ(linger_value.l_onoff, 1);
    REQUIRE_EQ(linger_value.l_linger, 7);

#if !defined(_WIN32)
    linger_value.l_linger = -1;
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_LINGER,
                   &linger_value,
                   static_cast<int>(sizeof(linger_value))),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);
#endif

    const bool message_api = true;
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_MESSAGEAPI,
                   &message_api,
                   static_cast<int>(sizeof(message_api))),
        0);
    REQUIRE_EQ(srt_close(socket), 0);
}

TEST(srt_compat_congestion_option_selects_a_controller_without_a_mode_bundle)
{
    const SRTSOCKET socket = srt_create_socket();
    REQUIRE(socket != SRT_INVALID_SOCK);

    constexpr char file_controller[] = "file";
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_CONGESTION,
                   file_controller,
                   static_cast<int>(
                       sizeof(file_controller) - 1U)),
        0);
    const auto record =
        robotweax::srt::compat::SocketRegistry::instance()
            .find(socket);
    REQUIRE(record != nullptr);
    {
        std::lock_guard lock(record->mutex);
        REQUIRE_EQ(record->native_options
                .congestion_controller(),
            robotweax::srt::CongestionController::file);
        REQUIRE_EQ(record->native_options
                .transmission_type(),
            robotweax::srt::TransmissionType::live);
        REQUIRE(record->native_options.message_api());
        record->state = SRTS_OPENED;
    }

    constexpr char live_controller[] = "live";
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_CONGESTION,
                   live_controller,
                   static_cast<int>(
                       sizeof(live_controller) - 1U)),
        0);
    constexpr char unknown_controller[] = "cubic";
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_CONGESTION,
                   unknown_controller,
                   static_cast<int>(
                       sizeof(unknown_controller) - 1U)),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr),
        SRT_EINVPARAM);
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_CONGESTION,
                   live_controller,
                   static_cast<int>(
                       sizeof(live_controller))),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr),
        SRT_EINVPARAM);

    char controller[8]{};
    int controller_size =
        static_cast<int>(sizeof(controller));
    REQUIRE_EQ(srt_getsockflag(socket, SRTO_CONGESTION,
                   controller, &controller_size),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr),
        SRT_EINVOP);

    {
        std::lock_guard lock(record->mutex);
        record->state = SRTS_CONNECTING;
    }
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_CONGESTION,
                   live_controller,
                   static_cast<int>(
                       sizeof(live_controller) - 1U)),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr),
        SRT_ECONNSOCK);

    {
        std::lock_guard lock(record->mutex);
        record->state = SRTS_INIT;
    }
    REQUIRE_EQ(srt_close(socket), 0);
}

TEST(srt_compat_nonblocking_rendezvous_uses_an_affinity_actor)
{
    ScopedSrtRuntime runtime;
    REQUIRE_EQ(runtime.startup_result, 0);

    sockaddr_in bind_address {};
    bind_address.sin_family = AF_INET;
    bind_address.sin_port = 0;
    bind_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    const SRTSOCKET left = srt_create_socket();
    const SRTSOCKET right = srt_create_socket();
    REQUIRE(left != SRT_INVALID_SOCK);
    REQUIRE(right != SRT_INVALID_SOCK);
    const bool enabled = true;
    const bool asynchronous = false;
    constexpr std::int32_t connection_timeout = 500;
    constexpr std::int32_t receive_timeout = 1'000;
    ConnectCallbackObservation left_callback;
    ConnectCallbackObservation right_callback;
    for (const SRTSOCKET socket : {left, right}) {
        REQUIRE_EQ(srt_setsockflag(socket, SRTO_RENDEZVOUS, &enabled,
                       static_cast<int>(sizeof(enabled))),
            0);
        REQUIRE_EQ(srt_setsockflag(socket, SRTO_RCVSYN, &asynchronous,
                       static_cast<int>(sizeof(asynchronous))),
            0);
        REQUIRE_EQ(srt_setsockflag(socket, SRTO_CONNTIMEO, &connection_timeout,
                       static_cast<int>(sizeof(connection_timeout))),
            0);
        REQUIRE_EQ(srt_setsockflag(socket, SRTO_RCVTIMEO, &receive_timeout,
                       static_cast<int>(sizeof(receive_timeout))),
            0);
    }
    REQUIRE_EQ(
        srt_connect_callback(left, observe_connect_result, &left_callback), 0);
    REQUIRE_EQ(
        srt_connect_callback(right, observe_connect_result, &right_callback),
        0);

    if (srt_bind(left, reinterpret_cast<const sockaddr*>(&bind_address),
            static_cast<int>(sizeof(bind_address)))
        == SRT_ERROR) {
        REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ESOCKFAIL);
        REQUIRE_EQ(srt_close(left), 0);
        REQUIRE_EQ(srt_close(right), 0);
        return;
    }
    if (srt_bind(right, reinterpret_cast<const sockaddr*>(&bind_address),
            static_cast<int>(sizeof(bind_address)))
        == SRT_ERROR) {
        REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ESOCKFAIL);
        REQUIRE_EQ(srt_close(left), 0);
        REQUIRE_EQ(srt_close(right), 0);
        return;
    }

    sockaddr_in left_name {};
    sockaddr_in right_name {};
    int left_name_size = static_cast<int>(sizeof(left_name));
    int right_name_size = static_cast<int>(sizeof(right_name));
    REQUIRE_EQ(srt_getsockname(left, reinterpret_cast<sockaddr*>(&left_name),
                   &left_name_size),
        0);
    REQUIRE_EQ(srt_getsockname(right, reinterpret_cast<sockaddr*>(&right_name),
                   &right_name_size),
        0);

    REQUIRE_EQ(srt_connect(left, reinterpret_cast<const sockaddr*>(&right_name),
                   static_cast<int>(sizeof(right_name))),
        0);
    const auto left_record =
        robotweax::srt::compat::SocketRegistry::instance().find(left);
    REQUIRE(left_record != nullptr);
    {
        std::lock_guard lock(left_record->mutex);
        REQUIRE(!left_record->connect_worker.joinable());
        REQUIRE(left_record->connect_handshake_operation != nullptr);
    }
    REQUIRE_EQ(srt_getsockstate(left), SRTS_CONNECTING);

    REQUIRE_EQ(srt_connect(right, reinterpret_cast<const sockaddr*>(&left_name),
                   static_cast<int>(sizeof(left_name))),
        0);
    REQUIRE(wait_for_connect_callback(left_callback, std::chrono::seconds {8}));
    REQUIRE(
        wait_for_connect_callback(right_callback, std::chrono::seconds {8}));
    REQUIRE_EQ(left_callback.error_code.load(), SRT_SUCCESS);
    REQUIRE_EQ(right_callback.error_code.load(), SRT_SUCCESS);
    REQUIRE_EQ(srt_getsockstate(left), SRTS_CONNECTED);
    REQUIRE_EQ(srt_getsockstate(right), SRTS_CONNECTED);

    const auto right_record =
        robotweax::srt::compat::SocketRegistry::instance().find(right);
    REQUIRE(right_record != nullptr);
    REQUIRE(wait_for_connect_cleanup(left_record, std::chrono::seconds {5}));
    REQUIRE(wait_for_connect_cleanup(right_record, std::chrono::seconds {5}));
    {
        std::lock_guard left_lock(left_record->mutex);
        REQUIRE(!left_record->connect_worker.joinable());
        REQUIRE(left_record->connect_handshake_operation == nullptr);
    }
    {
        std::lock_guard right_lock(right_record->mutex);
        REQUIRE(!right_record->connect_worker.joinable());
        REQUIRE(right_record->connect_handshake_operation == nullptr);
    }

    // RCVSYN=false selected the asynchronous connect contract and also leaves
    // application receives nonblocking. Restore a bounded blocking receive so
    // this transport assertion waits for packet readiness instead of racing
    // the loopback dispatcher immediately after srt_send returns.
    REQUIRE_EQ(srt_setsockflag(right, SRTO_RCVSYN, &enabled,
                   static_cast<int>(sizeof(enabled))),
        0);
    constexpr char payload[] = "asynchronous rendezvous actor";
    REQUIRE_EQ(srt_send(left, payload, static_cast<int>(sizeof(payload))),
        static_cast<int>(sizeof(payload)));
    std::array<char, SRT_LIVE_DEF_PLSIZE> received {};
    REQUIRE_EQ(
        srt_recv(right, received.data(), static_cast<int>(received.size())),
        static_cast<int>(sizeof(payload)));
    REQUIRE_EQ(std::memcmp(received.data(), payload, sizeof(payload)), 0);

    REQUIRE_EQ(srt_close(left), 0);
    REQUIRE_EQ(srt_close(right), 0);
}

TEST(srt_compat_nonblocking_rendezvous_actor_closes_while_pending)
{
    ScopedSrtRuntime runtime;
    REQUIRE_EQ(runtime.startup_result, 0);

    const UDPSOCKET sink = create_udp_socket();
#if defined(_WIN32)
    if (sink == INVALID_SOCKET) {
#else
    if (sink < 0) {
#endif
        return;
    }
    sockaddr_in sink_address {};
    sink_address.sin_family = AF_INET;
    sink_address.sin_port = 0;
    sink_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(sink, reinterpret_cast<const sockaddr*>(&sink_address),
            static_cast<NativeSocketLength>(sizeof(sink_address)))
        != 0) {
        close_udp_socket(sink);
        return;
    }
    NativeSocketLength sink_address_size =
        static_cast<NativeSocketLength>(sizeof(sink_address));
    REQUIRE_EQ(::getsockname(sink, reinterpret_cast<sockaddr*>(&sink_address),
                   &sink_address_size),
        0);

    const SRTSOCKET socket = srt_create_socket();
    REQUIRE(socket != SRT_INVALID_SOCK);
    const bool enabled = true;
    const bool asynchronous = false;
    constexpr std::int32_t connection_timeout = 30'000;
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_RENDEZVOUS, &enabled,
                   static_cast<int>(sizeof(enabled))),
        0);
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_RCVSYN, &asynchronous,
                   static_cast<int>(sizeof(asynchronous))),
        0);
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_CONNTIMEO, &connection_timeout,
                   static_cast<int>(sizeof(connection_timeout))),
        0);
    ConnectCallbackObservation callback;
    REQUIRE_EQ(
        srt_connect_callback(socket, observe_connect_result, &callback), 0);

    sockaddr_in local_address {};
    local_address.sin_family = AF_INET;
    local_address.sin_port = 0;
    local_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (srt_bind(socket, reinterpret_cast<const sockaddr*>(&local_address),
            static_cast<int>(sizeof(local_address)))
        == SRT_ERROR) {
        REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ESOCKFAIL);
        REQUIRE_EQ(srt_close(socket), 0);
        close_udp_socket(sink);
        return;
    }
    REQUIRE_EQ(
        srt_connect(socket, reinterpret_cast<const sockaddr*>(&sink_address),
            static_cast<int>(sizeof(sink_address))),
        0);
    const auto record =
        robotweax::srt::compat::SocketRegistry::instance().find(socket);
    REQUIRE(record != nullptr);
    {
        std::lock_guard lock(record->mutex);
        REQUIRE(!record->connect_worker.joinable());
        REQUIRE(record->connect_handshake_operation != nullptr);
    }
    REQUIRE_EQ(srt_getsockstate(socket), SRTS_CONNECTING);

    REQUIRE_EQ(srt_close(socket), 0);
    REQUIRE(callback.finished.load(std::memory_order_acquire));
    REQUIRE_EQ(callback.calls.load(), 1);
    REQUIRE_EQ(callback.error_code.load(), SRT_ESCLOSED);
    close_udp_socket(sink);
}

TEST(srt_compat_encrypted_rendezvous_peers_exchange_a_message)
{
    sockaddr_in bind_address{};
    bind_address.sin_family = AF_INET;
    bind_address.sin_port = 0;
    bind_address.sin_addr.s_addr =
        htonl(INADDR_LOOPBACK);

    const SRTSOCKET left = srt_create_socket();
    const SRTSOCKET right = srt_create_socket();
    REQUIRE(left != SRT_INVALID_SOCK);
    REQUIRE(right != SRT_INVALID_SOCK);
    constexpr std::int32_t left_flow_window = 111;
    constexpr std::int32_t right_flow_window = 222;
    REQUIRE_EQ(srt_setsockflag(left, SRTO_FC, &left_flow_window,
                   static_cast<int>(sizeof(left_flow_window))),
        0);
    REQUIRE_EQ(srt_setsockflag(right, SRTO_FC, &right_flow_window,
                   static_cast<int>(sizeof(right_flow_window))),
        0);
    std::int32_t left_initial_sequence = -1;
    int left_initial_sequence_size =
        static_cast<int>(sizeof(left_initial_sequence));
    REQUIRE_EQ(srt_getsockflag(left, SRTO_ISN,
                   &left_initial_sequence,
                   &left_initial_sequence_size),
        0);
    std::int32_t right_initial_sequence = -1;
    int right_initial_sequence_size =
        static_cast<int>(sizeof(right_initial_sequence));
    REQUIRE_EQ(srt_getsockflag(right, SRTO_ISN,
                   &right_initial_sequence,
                   &right_initial_sequence_size),
        0);
    const bool enabled = true;
    REQUIRE_EQ(srt_setsockflag(left,
                   SRTO_RENDEZVOUS,
                   &enabled,
                   static_cast<int>(sizeof(enabled))),
        0);
    REQUIRE_EQ(srt_setsockflag(right,
                   SRTO_RENDEZVOUS,
                   &enabled,
                   static_cast<int>(sizeof(enabled))),
        0);
    constexpr char passphrase[] =
        "robotweax-rendezvous-integration";
    for (const SRTSOCKET socket : {left, right}) {
        REQUIRE_EQ(srt_setsockflag(socket,
                       SRTO_PASSPHRASE,
                       passphrase,
                       static_cast<int>(
                           sizeof(passphrase) - 1U)),
            0);
        const std::int32_t key_length = 32;
        REQUIRE_EQ(srt_setsockflag(socket,
                       SRTO_PBKEYLEN,
                       &key_length,
                       static_cast<int>(
                           sizeof(key_length))),
            0);
#ifdef ENABLE_AEAD_API_PREVIEW
        const std::int32_t crypto_mode = 2;
        REQUIRE_EQ(srt_setsockflag(socket, SRTO_CRYPTOMODE, &crypto_mode,
                       static_cast<int>(sizeof(crypto_mode))),
            0);
#endif
    }
    std::int32_t connection_timeout = 500;
    REQUIRE_EQ(srt_setsockflag(left,
                   SRTO_CONNTIMEO,
                   &connection_timeout,
                   static_cast<int>(
                       sizeof(connection_timeout))),
        0);
    REQUIRE_EQ(srt_setsockflag(right,
                   SRTO_CONNTIMEO,
                   &connection_timeout,
                   static_cast<int>(
                       sizeof(connection_timeout))),
        0);
    std::int32_t receive_timeout = 1'000;
    REQUIRE_EQ(srt_setsockflag(right,
                   SRTO_RCVTIMEO,
                   &receive_timeout,
                   static_cast<int>(
                       sizeof(receive_timeout))),
        0);

    if (srt_bind(left,
            reinterpret_cast<const sockaddr*>(
                &bind_address),
            static_cast<int>(sizeof(bind_address)))
        == SRT_ERROR) {
        REQUIRE_EQ(srt_getlasterror(nullptr),
            SRT_ESOCKFAIL);
        REQUIRE_EQ(srt_close(left), 0);
        REQUIRE_EQ(srt_close(right), 0);
        REQUIRE_EQ(srt_cleanup(), 0);
        return;
    }
    if (srt_bind(right,
            reinterpret_cast<const sockaddr*>(
                &bind_address),
            static_cast<int>(sizeof(bind_address)))
        == SRT_ERROR) {
        REQUIRE_EQ(srt_getlasterror(nullptr),
            SRT_ESOCKFAIL);
        REQUIRE_EQ(srt_close(left), 0);
        REQUIRE_EQ(srt_close(right), 0);
        REQUIRE_EQ(srt_cleanup(), 0);
        return;
    }

    sockaddr_in left_name{};
    sockaddr_in right_name{};
    int left_name_size =
        static_cast<int>(sizeof(left_name));
    int right_name_size =
        static_cast<int>(sizeof(right_name));
    REQUIRE_EQ(srt_getsockname(left,
                   reinterpret_cast<sockaddr*>(
                       &left_name),
                   &left_name_size),
        0);
    REQUIRE_EQ(srt_getsockname(right,
                   reinterpret_cast<sockaddr*>(
                       &right_name),
                   &right_name_size),
        0);

    std::atomic<int> left_result{SRT_ERROR};
    std::atomic<int> right_result{SRT_ERROR};
    std::thread left_thread([&] {
        left_result.store(srt_connect(left,
            reinterpret_cast<const sockaddr*>(
                &right_name),
            static_cast<int>(sizeof(right_name))));
    });
    std::thread right_thread([&] {
        right_result.store(srt_connect(right,
            reinterpret_cast<const sockaddr*>(
                &left_name),
            static_cast<int>(sizeof(left_name))));
    });
    left_thread.join();
    right_thread.join();

    REQUIRE_EQ(left_result.load(), 0);
    REQUIRE_EQ(right_result.load(), 0);
    REQUIRE_EQ(srt_getsockstate(left), SRTS_CONNECTED);
    REQUIRE_EQ(srt_getsockstate(right), SRTS_CONNECTED);
#ifdef ENABLE_AEAD_API_PREVIEW
    for (const SRTSOCKET socket : {left, right}) {
        std::int32_t crypto_mode = 0;
        int crypto_mode_size = static_cast<int>(sizeof(crypto_mode));
        REQUIRE_EQ(srt_getsockflag(socket, SRTO_CRYPTOMODE, &crypto_mode,
                       &crypto_mode_size),
            0);
        REQUIRE_EQ(crypto_mode, 2);
    }
#endif

    std::int32_t connected_flow_window = 0;
    int connected_flow_window_size =
        static_cast<int>(sizeof(connected_flow_window));
    REQUIRE_EQ(srt_getsockflag(left, SRTO_FC, &connected_flow_window,
                   &connected_flow_window_size),
        0);
    REQUIRE_EQ(connected_flow_window, left_flow_window);
    connected_flow_window_size =
        static_cast<int>(sizeof(connected_flow_window));
    REQUIRE_EQ(srt_getsockflag(right, SRTO_FC, &connected_flow_window,
                   &connected_flow_window_size),
        0);
    REQUIRE_EQ(connected_flow_window, right_flow_window);
    SRT_TRACEBSTATS left_statistics {};
    SRT_TRACEBSTATS right_statistics {};
    REQUIRE_EQ(srt_bstats(left, &left_statistics, 0), 0);
    REQUIRE_EQ(srt_bstats(right, &right_statistics, 0), 0);
    REQUIRE_EQ(left_statistics.pktFlowWindow, right_flow_window);
    REQUIRE_EQ(right_statistics.pktFlowWindow, left_flow_window);

    std::int32_t connected_left_initial_sequence = -1;
    left_initial_sequence_size =
        static_cast<int>(sizeof(connected_left_initial_sequence));
    REQUIRE_EQ(srt_getsockflag(left, SRTO_ISN,
                   &connected_left_initial_sequence,
                   &left_initial_sequence_size),
        0);
    REQUIRE_EQ(
        connected_left_initial_sequence, left_initial_sequence);
    std::int32_t connected_right_initial_sequence = -1;
    right_initial_sequence_size =
        static_cast<int>(sizeof(connected_right_initial_sequence));
    REQUIRE_EQ(srt_getsockflag(right, SRTO_ISN,
                   &connected_right_initial_sequence,
                   &right_initial_sequence_size),
        0);
    REQUIRE_EQ(
        connected_right_initial_sequence, right_initial_sequence);

    constexpr char payload[] =
        "rendezvous works";
    REQUIRE_EQ(srt_sendmsg(left, payload,
                   static_cast<int>(sizeof(payload)),
                   -1, 1),
        static_cast<int>(sizeof(payload)));
    std::array<char, SRT_LIVE_DEF_PLSIZE> received {};
    REQUIRE_EQ(srt_recvmsg(right,
                   received.data(),
                   static_cast<int>(received.size())),
        static_cast<int>(sizeof(payload)));
    REQUIRE_EQ(std::memcmp(received.data(),
                   payload, sizeof(payload)),
        0);

    REQUIRE_EQ(srt_close(left), 0);
    REQUIRE_EQ(srt_close(right), 0);
    REQUIRE_EQ(srt_cleanup(), 0);
}

TEST(srt_compat_ipv6_rendezvous_peers_exchange_a_message)
{
    sockaddr_in6 bind_address{};
    bind_address.sin6_family = AF_INET6;
    bind_address.sin6_port = 0;
    bind_address.sin6_addr = in6addr_loopback;

    const SRTSOCKET left = srt_create_socket();
    const SRTSOCKET right = srt_create_socket();
    REQUIRE(left != SRT_INVALID_SOCK);
    REQUIRE(right != SRT_INVALID_SOCK);

    const bool enabled = true;
    constexpr std::int32_t connection_timeout = 500;
    constexpr std::int32_t receive_timeout = 1'000;
    for (const SRTSOCKET socket : {left, right}) {
        REQUIRE_EQ(srt_setsockflag(socket, SRTO_RENDEZVOUS,
                       &enabled, static_cast<int>(sizeof(enabled))),
            0);
        REQUIRE_EQ(srt_setsockflag(socket, SRTO_CONNTIMEO,
                       &connection_timeout,
                       static_cast<int>(sizeof(connection_timeout))),
            0);
        REQUIRE_EQ(srt_setsockflag(socket, SRTO_RCVTIMEO,
                       &receive_timeout,
                       static_cast<int>(sizeof(receive_timeout))),
            0);
    }

    if (srt_bind(left,
            reinterpret_cast<const sockaddr*>(&bind_address),
            static_cast<int>(sizeof(bind_address))) == SRT_ERROR) {
        int system_error = 0;
        REQUIRE_EQ(srt_getlasterror(&system_error), SRT_ESOCKFAIL);
        REQUIRE(system_error != 0);
        REQUIRE_EQ(srt_close(left), 0);
        REQUIRE_EQ(srt_close(right), 0);
        REQUIRE_EQ(srt_cleanup(), 0);
        return;
    }
    REQUIRE_EQ(srt_bind(right,
                   reinterpret_cast<const sockaddr*>(&bind_address),
                   static_cast<int>(sizeof(bind_address))),
        0);

    sockaddr_in6 left_name{};
    sockaddr_in6 right_name{};
    int left_name_size = static_cast<int>(sizeof(left_name));
    int right_name_size = static_cast<int>(sizeof(right_name));
    REQUIRE_EQ(srt_getsockname(left,
                   reinterpret_cast<sockaddr*>(&left_name),
                   &left_name_size),
        0);
    REQUIRE_EQ(srt_getsockname(right,
                   reinterpret_cast<sockaddr*>(&right_name),
                   &right_name_size),
        0);
    REQUIRE_EQ(left_name.sin6_family, AF_INET6);
    REQUIRE_EQ(right_name.sin6_family, AF_INET6);

    std::atomic<int> left_result{SRT_ERROR};
    std::atomic<int> right_result{SRT_ERROR};
    std::thread left_thread([&] {
        left_result.store(srt_connect(left,
            reinterpret_cast<const sockaddr*>(&right_name),
            static_cast<int>(sizeof(right_name))));
    });
    std::thread right_thread([&] {
        right_result.store(srt_connect(right,
            reinterpret_cast<const sockaddr*>(&left_name),
            static_cast<int>(sizeof(left_name))));
    });
    left_thread.join();
    right_thread.join();

    REQUIRE_EQ(left_result.load(), 0);
    REQUIRE_EQ(right_result.load(), 0);
    REQUIRE_EQ(srt_getsockstate(left), SRTS_CONNECTED);
    REQUIRE_EQ(srt_getsockstate(right), SRTS_CONNECTED);

    constexpr char payload[] = "ipv6 rendezvous works";
    REQUIRE_EQ(srt_send(left, payload,
                   static_cast<int>(sizeof(payload))),
        static_cast<int>(sizeof(payload)));
    std::array<char, SRT_LIVE_DEF_PLSIZE> received {};
    REQUIRE_EQ(srt_recv(right, received.data(),
                   static_cast<int>(received.size())),
        static_cast<int>(sizeof(payload)));
    REQUIRE_EQ(std::memcmp(received.data(), payload,
                   sizeof(payload)),
        0);

    REQUIRE_EQ(srt_close(left), 0);
    REQUIRE_EQ(srt_close(right), 0);
    REQUIRE_EQ(srt_cleanup(), 0);
}

TEST(srt_compat_blocking_caller_and_listener_complete_an_ipv4_handshake)
{
    constexpr std::int32_t connection_timeout_milliseconds = 2'000;
    constexpr std::int32_t receive_timeout_milliseconds = 2'000;
    constexpr std::int32_t udp_receive_buffer_bytes = 1'048'576;
    constexpr std::int32_t listener_flow_window = 222;
    constexpr std::int32_t caller_flow_window = 111;

    sockaddr_in bind_address{};
    bind_address.sin_family = AF_INET;
    bind_address.sin_port = 0;
    bind_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    constexpr char caller_stream_id[] =
        "#!::r=live/robotweax,m=request";
    ListenCallbackObservation callback_observation;

    const SRTSOCKET listener = srt_create_socket();
    REQUIRE(listener != SRT_INVALID_SOCK);
    constexpr char listener_stream_id[] = "must-not-be-inherited";
    REQUIRE_EQ(srt_setsockflag(listener, SRTO_STREAMID,
                   listener_stream_id,
                   static_cast<int>(sizeof(listener_stream_id) - 1U)),
        0);
    REQUIRE_EQ(srt_setsockflag(listener, SRTO_CONNTIMEO,
                   &connection_timeout_milliseconds,
                   static_cast<int>(
                       sizeof(connection_timeout_milliseconds))),
        0);
    REQUIRE_EQ(srt_setsockflag(listener, SRTO_RCVTIMEO,
                   &receive_timeout_milliseconds,
                   static_cast<int>(
                       sizeof(receive_timeout_milliseconds))),
        0);
    REQUIRE_EQ(srt_setsockflag(listener, SRTO_UDP_RCVBUF,
                   &udp_receive_buffer_bytes,
                   static_cast<int>(sizeof(udp_receive_buffer_bytes))),
        0);
    REQUIRE_EQ(srt_setsockflag(listener, SRTO_FC, &listener_flow_window,
                   static_cast<int>(sizeof(listener_flow_window))),
        0);
    REQUIRE_EQ(srt_listen_callback(listener,
                   observe_listener_connection,
                   &callback_observation),
        0);
    if (srt_bind(listener,
            reinterpret_cast<const sockaddr*>(&bind_address),
            static_cast<int>(sizeof(bind_address))) == SRT_ERROR) {
        int system_error = 0;
        REQUIRE_EQ(srt_getlasterror(&system_error), SRT_ESOCKFAIL);
        REQUIRE(system_error != 0);
        REQUIRE_EQ(srt_close(listener), 0);
        REQUIRE_EQ(srt_cleanup(), 0);
        return;
    }

    REQUIRE_EQ(srt_listen(listener, 4), 0);
    REQUIRE_EQ(srt_getsockstate(listener), SRTS_LISTENING);
    REQUIRE_EQ(srt_listen_callback(listener, nullptr, nullptr),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ECONNSOCK);

    sockaddr_in listener_name{};
    int listener_name_size = static_cast<int>(sizeof(listener_name));
    REQUIRE_EQ(srt_getsockname(listener,
                   reinterpret_cast<sockaddr*>(&listener_name),
                   &listener_name_size),
        0);

    std::atomic<SRTSOCKET> accepted{SRT_INVALID_SOCK};
    std::atomic<int> accept_error{SRT_SUCCESS};
    sockaddr_in accepted_peer{};
    std::thread accept_thread([&] {
        int accepted_peer_size = static_cast<int>(sizeof(accepted_peer));
        accepted.store(srt_accept(listener,
            reinterpret_cast<sockaddr*>(&accepted_peer),
            &accepted_peer_size));
        accept_error.store(srt_getlasterror(nullptr));
    });

    const SRTSOCKET caller = srt_create_socket();
    REQUIRE(caller != SRT_INVALID_SOCK);
    REQUIRE_EQ(srt_setsockflag(caller, SRTO_STREAMID,
                   caller_stream_id,
                   static_cast<int>(sizeof(caller_stream_id) - 1U)),
        0);
    std::int32_t caller_initial_sequence = -1;
    int caller_initial_sequence_size =
        static_cast<int>(sizeof(caller_initial_sequence));
    REQUIRE_EQ(srt_getsockflag(caller, SRTO_ISN,
                   &caller_initial_sequence,
                   &caller_initial_sequence_size),
        0);
    REQUIRE_EQ(srt_setsockflag(caller, SRTO_CONNTIMEO,
                   &connection_timeout_milliseconds,
                   static_cast<int>(
                       sizeof(connection_timeout_milliseconds))),
        0);
    REQUIRE_EQ(srt_setsockflag(caller, SRTO_RCVTIMEO,
                   &receive_timeout_milliseconds,
                   static_cast<int>(
                       sizeof(receive_timeout_milliseconds))),
        0);
    REQUIRE_EQ(srt_setsockflag(caller, SRTO_FC, &caller_flow_window,
                   static_cast<int>(sizeof(caller_flow_window))),
        0);
    const int connect_result = srt_connect(caller,
        reinterpret_cast<const sockaddr*>(&listener_name),
        static_cast<int>(sizeof(listener_name)));
    if (connect_result == SRT_ERROR) {
        (void)srt_close(listener);
    }
    accept_thread.join();

    REQUIRE_EQ(connect_result, 0);
    REQUIRE(accepted.load() != SRT_INVALID_SOCK);
    REQUIRE_EQ(accept_error.load(), SRT_SUCCESS);
    REQUIRE_EQ(srt_getsockstate(caller), SRTS_CONNECTED);
    REQUIRE_EQ(srt_getsockstate(accepted.load()), SRTS_CONNECTED);
    std::int32_t connected_flow_window = 0;
    int connected_flow_window_size =
        static_cast<int>(sizeof(connected_flow_window));
    REQUIRE_EQ(srt_getsockflag(caller, SRTO_FC, &connected_flow_window,
                   &connected_flow_window_size),
        0);
    REQUIRE_EQ(connected_flow_window, caller_flow_window);
    connected_flow_window_size =
        static_cast<int>(sizeof(connected_flow_window));
    REQUIRE_EQ(srt_getsockflag(accepted.load(), SRTO_FC, &connected_flow_window,
                   &connected_flow_window_size),
        0);
    REQUIRE_EQ(connected_flow_window, listener_flow_window);
    SRT_TRACEBSTATS caller_statistics {};
    SRT_TRACEBSTATS listener_statistics {};
    REQUIRE_EQ(srt_bstats(caller, &caller_statistics, 0), 0);
    REQUIRE_EQ(srt_bstats(accepted.load(), &listener_statistics, 0), 0);
    REQUIRE_EQ(caller_statistics.pktFlowWindow, listener_flow_window);
    REQUIRE_EQ(listener_statistics.pktFlowWindow, caller_flow_window);
    REQUIRE_EQ(callback_observation.calls, 1);
    REQUIRE_EQ(callback_observation.socket, accepted.load());
    REQUIRE_EQ(callback_observation.handshake_version, 5);
    REQUIRE_EQ(callback_observation.peer_family, AF_INET);
    REQUIRE_EQ(callback_observation.argument_stream_id,
        std::string{caller_stream_id});
    REQUIRE_EQ(callback_observation.socket_stream_id,
        std::string{caller_stream_id});
    REQUIRE(callback_observation.peer_metadata_observed);
    REQUIRE_EQ(callback_observation.peer_version, 0);
    REQUIRE_EQ(callback_observation.incoming_group_type,
        SRT_GTYPE_UNDEFINED);
    REQUIRE(callback_observation.option_update_succeeded);

    for (const SRTSOCKET connected : {caller, accepted.load()}) {
        std::int32_t peer_version = 0;
        int peer_version_size = static_cast<int>(sizeof(peer_version));
        REQUIRE_EQ(srt_getsockflag(connected, SRTO_PEERVERSION,
                       &peer_version, &peer_version_size),
            0);
        REQUIRE_EQ(peer_version,
            static_cast<std::int32_t>(SRT_VERSION_VALUE));
        std::int32_t group_type = SRT_GTYPE_E_END;
        int group_type_size = static_cast<int>(sizeof(group_type));
        REQUIRE_EQ(srt_getsockflag(connected, SRTO_GROUPTYPE,
                       &group_type, &group_type_size),
            0);
        REQUIRE_EQ(group_type,
            static_cast<std::int32_t>(SRT_GTYPE_UNDEFINED));
    }

    std::int32_t callback_connection_timeout = 0;
    int callback_connection_timeout_size =
        static_cast<int>(sizeof(callback_connection_timeout));
    REQUIRE_EQ(srt_getsockflag(accepted.load(), SRTO_CONNTIMEO,
                   &callback_connection_timeout,
                   &callback_connection_timeout_size),
        0);
    REQUIRE_EQ(callback_connection_timeout, 2'347);

    std::array<char,
        robotweax::srt::maximum_stream_id_size + 1U>
        accepted_stream_id{};
    int accepted_stream_id_size =
        static_cast<int>(accepted_stream_id.size());
    REQUIRE_EQ(srt_getsockflag(accepted.load(), SRTO_STREAMID,
                   accepted_stream_id.data(),
                   &accepted_stream_id_size),
        0);
    REQUIRE_EQ(accepted_stream_id_size,
        static_cast<int>(sizeof(caller_stream_id) - 1U));
    REQUIRE(std::strcmp(accepted_stream_id.data(),
                caller_stream_id)
        == 0);

    std::int32_t accepted_initial_sequence = -1;
    int accepted_initial_sequence_size =
        static_cast<int>(sizeof(accepted_initial_sequence));
    REQUIRE_EQ(srt_getsockflag(accepted.load(), SRTO_ISN,
                   &accepted_initial_sequence,
                   &accepted_initial_sequence_size),
        0);
    REQUIRE_EQ(accepted_initial_sequence, caller_initial_sequence);

    std::int32_t inherited_udp_receive_buffer = 0;
    int inherited_udp_receive_buffer_size =
        static_cast<int>(sizeof(inherited_udp_receive_buffer));
    REQUIRE_EQ(srt_getsockflag(
                   accepted.load(), SRTO_UDP_RCVBUF,
                   &inherited_udp_receive_buffer,
                   &inherited_udp_receive_buffer_size),
        0);
    REQUIRE_EQ(inherited_udp_receive_buffer, udp_receive_buffer_bytes);

    sockaddr_in caller_local{};
    int caller_local_size = static_cast<int>(sizeof(caller_local));
    REQUIRE_EQ(srt_getsockname(caller,
                   reinterpret_cast<sockaddr*>(&caller_local),
                   &caller_local_size),
        0);
    REQUIRE_EQ(accepted_peer.sin_family, AF_INET);
    REQUIRE_EQ(accepted_peer.sin_port, caller_local.sin_port);

    sockaddr_in caller_peer{};
    int caller_peer_size = static_cast<int>(sizeof(caller_peer));
    REQUIRE_EQ(srt_getpeername(caller,
                   reinterpret_cast<sockaddr*>(&caller_peer),
                   &caller_peer_size),
        0);
    REQUIRE_EQ(caller_peer.sin_port, listener_name.sin_port);

    sockaddr_in accepted_local{};
    int accepted_local_size = static_cast<int>(sizeof(accepted_local));
    REQUIRE_EQ(srt_getsockname(accepted.load(),
                   reinterpret_cast<sockaddr*>(&accepted_local),
                   &accepted_local_size),
        0);
    REQUIRE_EQ(accepted_local.sin_port, listener_name.sin_port);

    constexpr char caller_payload[] = "robotweax caller payload";
    SRT_MSGCTRL caller_send_control = srt_msgctrl_default;
    REQUIRE_EQ(srt_sendmsg2(caller, caller_payload,
                   static_cast<int>(sizeof(caller_payload)),
                   &caller_send_control),
        static_cast<int>(sizeof(caller_payload)));
    REQUIRE(caller_send_control.msgno > 0);

    std::array<char, 1'500> received_payload{};
    SRT_MSGCTRL accepted_receive_control = srt_msgctrl_default;
    REQUIRE_EQ(srt_recvmsg2(accepted.load(), received_payload.data(),
                   static_cast<int>(received_payload.size()),
                   &accepted_receive_control),
        static_cast<int>(sizeof(caller_payload)));
    REQUIRE(std::memcmp(received_payload.data(), caller_payload,
                sizeof(caller_payload))
        == 0);
    REQUIRE(accepted_receive_control.msgno > 0);
    REQUIRE(accepted_receive_control.pktseq >= 0);
    REQUIRE(accepted_receive_control.srctime > 0);

    constexpr char listener_payload[] = "robotweax listener payload";
    REQUIRE_EQ(srt_send(accepted.load(), listener_payload,
                   static_cast<int>(sizeof(listener_payload))),
        static_cast<int>(sizeof(listener_payload)));
    received_payload.fill(0);
    REQUIRE_EQ(srt_recv(caller, received_payload.data(),
                   static_cast<int>(received_payload.size())),
        static_cast<int>(sizeof(listener_payload)));
    REQUIRE(std::memcmp(received_payload.data(), listener_payload,
                sizeof(listener_payload))
        == 0);

    const bool receive_asynchronously = false;
    REQUIRE_EQ(srt_setsockflag(accepted.load(), SRTO_RCVSYN,
                   &receive_asynchronously,
                   static_cast<int>(sizeof(receive_asynchronously))),
        0);
    REQUIRE_EQ(srt_recv(accepted.load(), received_payload.data(),
                   static_cast<int>(received_payload.size())),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EASYNCRCV);

    std::array<char, SRT_LIVE_DEF_PLSIZE + 1> oversized{};
    REQUIRE_EQ(srt_send(caller, oversized.data(),
                   static_cast<int>(oversized.size())),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVALMSGAPI);

    REQUIRE_EQ(srt_close(caller), 0);
    REQUIRE_EQ(srt_close(accepted.load()), 0);
    REQUIRE_EQ(srt_close(listener), 0);
    REQUIRE_EQ(srt_cleanup(), 0);
}

TEST(srt_compat_listener_discards_oversized_udp_before_valid_handshake)
{
    constexpr std::int32_t timeout_milliseconds = 2'000;
    sockaddr_in bind_address {};
    bind_address.sin_family = AF_INET;
    bind_address.sin_port = 0;
    bind_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    const SRTSOCKET listener = srt_create_socket();
    REQUIRE(listener != SRT_INVALID_SOCK);
    REQUIRE_EQ(srt_setsockflag(listener, SRTO_CONNTIMEO, &timeout_milliseconds,
                   static_cast<int>(sizeof(timeout_milliseconds))),
        0);
    if (srt_bind(listener, reinterpret_cast<const sockaddr*>(&bind_address),
            static_cast<int>(sizeof(bind_address)))
        == SRT_ERROR) {
        REQUIRE_EQ(srt_close(listener), 0);
        REQUIRE_EQ(srt_cleanup(), 0);
        return;
    }
    REQUIRE_EQ(srt_listen(listener, 1), 0);

    sockaddr_in listener_name {};
    int listener_name_size = static_cast<int>(sizeof(listener_name));
    REQUIRE_EQ(
        srt_getsockname(listener, reinterpret_cast<sockaddr*>(&listener_name),
            &listener_name_size),
        0);

    robotweax::srt::UdpSocket injector;
    REQUIRE(injector.valid());
    REQUIRE_EQ(injector.bind(robotweax::srt::IpEndpoint::loopback()),
        robotweax::srt::Error::none);
    std::array<std::byte, 1'501> oversized {};
    REQUIRE(injector.send_to(oversized,
        robotweax::srt::IpEndpoint::loopback(ntohs(listener_name.sin_port))));

    const SRTSOCKET caller = srt_create_socket();
    REQUIRE(caller != SRT_INVALID_SOCK);
    REQUIRE_EQ(srt_setsockflag(caller, SRTO_CONNTIMEO, &timeout_milliseconds,
                   static_cast<int>(sizeof(timeout_milliseconds))),
        0);

    std::atomic<SRTSOCKET> accepted {SRT_INVALID_SOCK};
    std::thread accept_thread([&] {
        accepted.store(srt_accept(listener, nullptr, nullptr));
    });
    const int connected =
        srt_connect(caller, reinterpret_cast<const sockaddr*>(&listener_name),
            static_cast<int>(sizeof(listener_name)));
    if (connected == SRT_ERROR) {
        (void)srt_close(listener);
    }
    accept_thread.join();

    REQUIRE_EQ(connected, 0);
    REQUIRE(accepted.load() != SRT_INVALID_SOCK);
    REQUIRE_EQ(srt_getsockstate(caller), SRTS_CONNECTED);
    REQUIRE_EQ(srt_getsockstate(accepted.load()), SRTS_CONNECTED);

    REQUIRE_EQ(srt_close(caller), 0);
    REQUIRE_EQ(srt_close(accepted.load()), 0);
    REQUIRE_EQ(srt_close(listener), 0);
    REQUIRE_EQ(srt_cleanup(), 0);
}

TEST(
    srt_compat_blocking_caller_rejects_hsv4_message_and_stream_without_downgrade)
{
    ScopedSrtRuntime runtime;
    REQUIRE_EQ(runtime.startup_result, 0);
    constexpr std::int32_t timeout_milliseconds = 2'000;

    for (const SRT_TRANSTYPE transmission_type : {SRTT_LIVE, SRTT_FILE}) {
        robotweax::srt::UdpSocket peer;
        if (!peer.valid()
            || peer.bind(robotweax::srt::IpEndpoint::loopback())
                != robotweax::srt::Error::none) {
            return;
        }
        const auto endpoint = peer.local_endpoint();
        REQUIRE(endpoint);
        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_port = htons(endpoint.endpoint.port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        const SRTSOCKET caller = srt_create_socket();
        REQUIRE(caller != SRT_INVALID_SOCK);
        REQUIRE_EQ(
            srt_setsockflag(caller, SRTO_CONNTIMEO, &timeout_milliseconds,
                static_cast<int>(sizeof(timeout_milliseconds))),
            0);
        REQUIRE_EQ(srt_setsockflag(caller, SRTO_TRANSTYPE, &transmission_type,
                       static_cast<int>(sizeof(transmission_type))),
            0);

        UnsupportedHsv4PeerObservation observation;
        std::thread peer_thread([&] {
            answer_with_unsupported_hsv4_induction(peer, observation);
        });
        const int result =
            srt_connect(caller, reinterpret_cast<const sockaddr*>(&address),
                static_cast<int>(sizeof(address)));
        const int error = srt_getlasterror(nullptr);
        peer_thread.join();

        REQUIRE(observation.induction_received);
        REQUIRE(observation.response_sent);
        REQUIRE_EQ(result, SRT_ERROR);
        REQUIRE_EQ(error, SRT_ECONNREJ);
        REQUIRE_EQ(srt_getrejectreason(caller), SRT_REJ_VERSION);
        REQUIRE_EQ(srt_getsockstate(caller), SRTS_BROKEN);
        REQUIRE_EQ(srt_close(caller), 0);
    }
}

TEST(srt_compat_async_caller_reports_hsv4_rejection_to_epoll_and_callback)
{
    ScopedSrtRuntime runtime;
    REQUIRE_EQ(runtime.startup_result, 0);
    robotweax::srt::UdpSocket peer;
    if (!peer.valid()
        || peer.bind(robotweax::srt::IpEndpoint::loopback())
            != robotweax::srt::Error::none) {
        return;
    }
    const auto endpoint = peer.local_endpoint();
    REQUIRE(endpoint);
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = htons(endpoint.endpoint.port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    const SRTSOCKET caller = srt_create_socket();
    REQUIRE(caller != SRT_INVALID_SOCK);
    constexpr bool asynchronous = false;
    constexpr std::int32_t timeout_milliseconds = 2'000;
    REQUIRE_EQ(srt_setsockflag(caller, SRTO_RCVSYN, &asynchronous,
                   static_cast<int>(sizeof(asynchronous))),
        0);
    REQUIRE_EQ(srt_setsockflag(caller, SRTO_CONNTIMEO, &timeout_milliseconds,
                   static_cast<int>(sizeof(timeout_milliseconds))),
        0);
    ConnectCallbackObservation callback;
    REQUIRE_EQ(
        srt_connect_callback(caller, observe_connect_result, &callback), 0);
    const int poll = srt_epoll_create();
    REQUIRE(poll >= 0);
    const int events = SRT_EPOLL_OUT | SRT_EPOLL_ERR;
    REQUIRE_EQ(srt_epoll_add_usock(poll, caller, &events), 0);

    UnsupportedHsv4PeerObservation observation;
    std::thread peer_thread([&] {
        answer_with_unsupported_hsv4_induction(peer, observation);
    });
    REQUIRE_EQ(srt_connect(caller, reinterpret_cast<const sockaddr*>(&address),
                   static_cast<int>(sizeof(address))),
        0);
    std::array<SRT_EPOLL_EVENT, 1> ready {};
    REQUIRE_EQ(srt_epoll_uwait(
                   poll, ready.data(), static_cast<int>(ready.size()), 5'000),
        1);
    peer_thread.join();

    REQUIRE(observation.induction_received);
    REQUIRE(observation.response_sent);
    REQUIRE_EQ(ready[0].fd, caller);
    REQUIRE_EQ(ready[0].events, SRT_EPOLL_ERR);
    REQUIRE(wait_for_connect_callback(callback, std::chrono::seconds {5}));
    REQUIRE_EQ(callback.calls.load(), 1);
    REQUIRE_EQ(callback.socket.load(), caller);
    REQUIRE_EQ(callback.error_code.load(), SRT_ECONNREJ);
    REQUIRE_EQ(callback.socket_state.load(), SRTS_CONNECTING);
    REQUIRE_EQ(srt_getsockstate(caller), SRTS_CONNECTING);
    REQUIRE_EQ(srt_getrejectreason(caller), SRT_REJ_VERSION);

    REQUIRE_EQ(srt_epoll_release(poll), 0);
    REQUIRE_EQ(srt_close(caller), 0);
}

TEST(
    srt_compat_listener_routes_ambiguous_udt_discovery_as_hsv5_without_allocation)
{
    sockaddr_in bind_address {};
    bind_address.sin_family = AF_INET;
    bind_address.sin_port = 0;
    bind_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    ListenCallbackObservation callback_observation;
    const SRTSOCKET listener = srt_create_socket();
    REQUIRE(listener != SRT_INVALID_SOCK);
    REQUIRE_EQ(srt_listen_callback(listener, observe_listener_connection,
                   &callback_observation),
        0);
    if (srt_bind(listener,
            reinterpret_cast<const sockaddr*>(&bind_address),
            static_cast<int>(sizeof(bind_address))) == SRT_ERROR) {
        REQUIRE_EQ(srt_close(listener), 0);
        REQUIRE_EQ(srt_cleanup(), 0);
        return;
    }
    REQUIRE_EQ(srt_listen(listener, 1), 0);

    sockaddr_in listener_name{};
    int listener_name_size = static_cast<int>(sizeof(listener_name));
    REQUIRE_EQ(srt_getsockname(listener,
                   reinterpret_cast<sockaddr*>(&listener_name),
                   &listener_name_size),
        0);
    robotweax::srt::IpEndpoint listener_endpoint;
    REQUIRE_EQ(robotweax::srt::compat::decode_ip_endpoint(
                   reinterpret_cast<const sockaddr*>(&listener_name),
                   listener_name_size, listener_endpoint, false),
        0);

    robotweax::srt::UdpSocket peer;
    if (!peer.valid()
        || peer.bind(robotweax::srt::IpEndpoint::loopback())
            != robotweax::srt::Error::none) {
        REQUIRE_EQ(srt_close(listener), 0);
        REQUIRE_EQ(srt_cleanup(), 0);
        return;
    }

    robotweax::srt::HandshakeAction induction;
    induction.kind = robotweax::srt::HandshakeActionKind::send;
    induction.packet.version = robotweax::srt::handshake_version_4;
    // UDT_DGRAM is the ambiguous discovery value shared by HSv4 and HSv5.
    induction.packet.extension_field = 2U;
    induction.packet.initial_sequence =
        robotweax::srt::SequenceNumber{123U};
    induction.packet.maximum_transmission_unit = 1'500U;
    // This is the exact discovery shape used by a deployed Haivision HSv5
    // caller. It is indistinguishable from a genuine legacy HSv4 induction.
    induction.packet.flow_window = 8'192U;
    induction.packet.request =
        robotweax::srt::HandshakeRequest::induction;
    induction.packet.socket_id = 0x1234U;
    std::array<std::byte, 1'500> datagram{};
    const auto encoded = robotweax::srt::encode_handshake_datagram(
        induction, robotweax::srt::PacketTimestamp{0U}, 0U, datagram);
    REQUIRE(encoded);
    REQUIRE(peer.send_to(
        std::span{datagram}.first(encoded.bytes_written),
        listener_endpoint));
    const auto ready = peer.wait_readable(2'000);
    REQUIRE(ready);
    REQUIRE(ready.ready);
    const auto received = peer.receive_from(datagram);
    REQUIRE(received);
    const auto response = robotweax::srt::decode_handshake_datagram(
        std::span{datagram}.first(received.bytes_transferred));
    REQUIRE(response);
    REQUIRE_EQ(response.message.packet.version,
        robotweax::srt::handshake_version_5);
    REQUIRE_EQ(response.message.packet.extension_field,
        robotweax::srt::handshake_srt_magic);
    REQUIRE_EQ(response.message.packet.socket_id,
        static_cast<std::uint32_t>(listener));
    REQUIRE(response.message.packet.syn_cookie != 0U);

    // Once the peer commits to genuine HSv4, reject it canonically without
    // admission, callback publication, or an accepted socket allocation.
    robotweax::srt::HandshakeAction conclusion = induction;
    conclusion.packet.request = robotweax::srt::HandshakeRequest::conclusion;
    conclusion.packet.syn_cookie = response.message.packet.syn_cookie;
    const auto encoded_conclusion = robotweax::srt::encode_handshake_datagram(
        conclusion, robotweax::srt::PacketTimestamp {1U},
        response.message.packet.socket_id, datagram);
    REQUIRE(encoded_conclusion);
    REQUIRE(peer.send_to(
        std::span {datagram}.first(encoded_conclusion.bytes_written),
        listener_endpoint));
    const auto rejection_ready = peer.wait_readable(2'000);
    REQUIRE(rejection_ready);
    REQUIRE(rejection_ready.ready);
    const auto rejection_datagram = peer.receive_from(datagram);
    REQUIRE(rejection_datagram);
    const auto rejection = robotweax::srt::decode_handshake_datagram(
        std::span {datagram}.first(rejection_datagram.bytes_transferred));
    REQUIRE(rejection);
    REQUIRE_EQ(
        rejection.message.packet.version, robotweax::srt::handshake_version_5);
    REQUIRE_EQ(static_cast<std::int32_t>(rejection.message.packet.request),
        SRT_REJC_PREDEFINED + SRT_REJ_VERSION);
    REQUIRE_EQ(rejection.message.packet.syn_cookie,
        response.message.packet.syn_cookie);
    REQUIRE_EQ(callback_observation.calls, 0);

    constexpr bool asynchronous = false;
    REQUIRE_EQ(srt_setsockflag(listener, SRTO_RCVSYN, &asynchronous,
                   static_cast<int>(sizeof(asynchronous))),
        0);
    REQUIRE_EQ(srt_accept(listener, nullptr, nullptr), SRT_INVALID_SOCK);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EASYNCRCV);
    const int poll = srt_epoll_create();
    REQUIRE(poll >= 0);
    const int events = SRT_EPOLL_IN | SRT_EPOLL_ERR;
    REQUIRE_EQ(srt_epoll_add_usock(poll, listener, &events), 0);
    std::array<SRT_EPOLL_EVENT, 1> ready_events {};
    REQUIRE_EQ(srt_epoll_uwait(poll, ready_events.data(),
                   static_cast<int>(ready_events.size()), 50),
        0);

    // UDT_STREAM is an unambiguous legacy discovery shape. It is dropped
    // without amplification and without allocating listener-side state.
    robotweax::srt::HandshakeAction stream_induction = induction;
    stream_induction.packet.extension_field = 1U; // UDT_STREAM
    ++stream_induction.packet.socket_id;
    const auto encoded_stream = robotweax::srt::encode_handshake_datagram(
        stream_induction, robotweax::srt::PacketTimestamp {2U}, 0U, datagram);
    REQUIRE(encoded_stream);
    REQUIRE(
        peer.send_to(std::span {datagram}.first(encoded_stream.bytes_written),
            listener_endpoint));
    const auto stream_ready = peer.wait_readable(100);
    REQUIRE(stream_ready);
    REQUIRE(!stream_ready.ready);
    REQUIRE_EQ(callback_observation.calls, 0);

    const auto listener_record =
        robotweax::srt::compat::SocketRegistry::instance().find(listener);
    REQUIRE(listener_record != nullptr);
    {
        std::lock_guard lock(listener_record->mutex);
        REQUIRE(std::any_of(
            listener_record->listener_cookie_secret.begin(),
            listener_record->listener_cookie_secret.end(),
            [](std::byte value) {
                return value != std::byte{0};
            }));
    }

    const SRTSOCKET next_socket = srt_create_socket();
    REQUIRE_EQ(next_socket, listener + 1);
    REQUIRE_EQ(srt_close(next_socket), 0);
    REQUIRE_EQ(srt_epoll_release(poll), 0);
    REQUIRE_EQ(srt_close(listener), 0);
    {
        std::lock_guard lock(listener_record->mutex);
        REQUIRE(std::none_of(
            listener_record->listener_cookie_secret.begin(),
            listener_record->listener_cookie_secret.end(),
            [](std::byte value) {
                return value != std::byte{0};
            }));
    }
    REQUIRE_EQ(srt_cleanup(), 0);
}

TEST(srt_compat_listener_callback_rejects_before_accept)
{
    constexpr std::int32_t timeout_milliseconds = 2'000;
    constexpr char stream_id[] = "#!::r=private,m=request";

    sockaddr_in bind_address{};
    bind_address.sin_family = AF_INET;
    bind_address.sin_port = 0;
    bind_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    RejectCallbackObservation callback_observation;
    const SRTSOCKET listener = srt_create_socket();
    REQUIRE(listener != SRT_INVALID_SOCK);
    REQUIRE_EQ(srt_setsockflag(listener, SRTO_CONNTIMEO,
                   &timeout_milliseconds,
                   static_cast<int>(sizeof(timeout_milliseconds))),
        0);
    REQUIRE_EQ(srt_listen_callback(listener,
                   reject_listener_connection,
                   &callback_observation),
        0);
    if (srt_bind(listener,
            reinterpret_cast<const sockaddr*>(&bind_address),
            static_cast<int>(sizeof(bind_address))) == SRT_ERROR) {
        int system_error = 0;
        REQUIRE_EQ(srt_getlasterror(&system_error), SRT_ESOCKFAIL);
        REQUIRE(system_error != 0);
        REQUIRE_EQ(srt_close(listener), 0);
        REQUIRE_EQ(srt_cleanup(), 0);
        return;
    }
    REQUIRE_EQ(srt_listen(listener, 1), 0);

    sockaddr_in listener_name{};
    int listener_name_size = static_cast<int>(sizeof(listener_name));
    REQUIRE_EQ(srt_getsockname(listener,
                   reinterpret_cast<sockaddr*>(&listener_name),
                   &listener_name_size),
        0);

    const SRTSOCKET caller = srt_create_socket();
    REQUIRE(caller != SRT_INVALID_SOCK);
    REQUIRE_EQ(srt_setsockflag(caller, SRTO_CONNTIMEO,
                   &timeout_milliseconds,
                   static_cast<int>(sizeof(timeout_milliseconds))),
        0);
    REQUIRE_EQ(srt_setsockflag(caller, SRTO_STREAMID,
                   stream_id,
                   static_cast<int>(sizeof(stream_id) - 1U)),
        0);
    REQUIRE_EQ(srt_connect(caller,
                   reinterpret_cast<const sockaddr*>(&listener_name),
                   static_cast<int>(sizeof(listener_name))),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ECONNREJ);
    REQUIRE_EQ(srt_getrejectreason(caller), SRT_REJX_OVERLOAD);
    REQUIRE_EQ(callback_observation.calls, 1);
    REQUIRE(callback_observation.socket != SRT_INVALID_SOCK);
    REQUIRE_EQ(callback_observation.stream_id,
        std::string{stream_id});
    REQUIRE(callback_observation.reason_update_succeeded);

    const bool receive_asynchronously = false;
    REQUIRE_EQ(srt_setsockflag(listener, SRTO_RCVSYN,
                   &receive_asynchronously,
                   static_cast<int>(sizeof(receive_asynchronously))),
        0);
    REQUIRE_EQ(srt_accept(listener, nullptr, nullptr),
        SRT_INVALID_SOCK);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EASYNCRCV);

    REQUIRE_EQ(srt_close(caller), 0);
    REQUIRE_EQ(srt_close(listener), 0);
    REQUIRE_EQ(srt_cleanup(), 0);
}

TEST(srt_compat_ipv6_caller_listener_names_and_payload_are_end_to_end)
{
    constexpr std::int32_t timeout_milliseconds = 2'000;
    constexpr std::int32_t listener_mss = 1'280;
    constexpr std::int32_t caller_mss = 1'400;

    sockaddr_in6 bind_address{};
    bind_address.sin6_family = AF_INET6;
    bind_address.sin6_port = 0;
    bind_address.sin6_addr = in6addr_loopback;

    const SRTSOCKET listener = srt_create_socket();
    REQUIRE(listener != SRT_INVALID_SOCK);
    REQUIRE_EQ(srt_setsockflag(listener, SRTO_CONNTIMEO,
                   &timeout_milliseconds,
                   static_cast<int>(sizeof(timeout_milliseconds))),
        0);
    REQUIRE_EQ(srt_setsockflag(listener, SRTO_RCVTIMEO,
                   &timeout_milliseconds,
                   static_cast<int>(sizeof(timeout_milliseconds))),
        0);
    REQUIRE_EQ(srt_setsockflag(listener, SRTO_MSS,
                   &listener_mss,
                   static_cast<int>(sizeof(listener_mss))),
        0);
#ifdef ENABLE_AEAD_API_PREVIEW
    constexpr char passphrase[] = "robotweax-ipv6-gcm-boundary";
    constexpr std::int32_t key_length = 16;
    constexpr std::int32_t crypto_mode = 2;
    REQUIRE_EQ(srt_setsockflag(listener, SRTO_PASSPHRASE, passphrase,
                   static_cast<int>(sizeof(passphrase) - 1U)),
        0);
    REQUIRE_EQ(srt_setsockflag(listener, SRTO_PBKEYLEN, &key_length,
                   static_cast<int>(sizeof(key_length))),
        0);
    REQUIRE_EQ(srt_setsockflag(listener, SRTO_CRYPTOMODE, &crypto_mode,
                   static_cast<int>(sizeof(crypto_mode))),
        0);
#endif
    if (srt_bind(listener,
            reinterpret_cast<const sockaddr*>(&bind_address),
            static_cast<int>(sizeof(bind_address))) == SRT_ERROR) {
        int system_error = 0;
        REQUIRE_EQ(srt_getlasterror(&system_error), SRT_ESOCKFAIL);
        REQUIRE(system_error != 0);
        REQUIRE_EQ(srt_close(listener), 0);
        REQUIRE_EQ(srt_cleanup(), 0);
        return;
    }

    sockaddr_in too_small{};
    int too_small_size = static_cast<int>(sizeof(too_small));
    REQUIRE_EQ(srt_getsockname(listener,
                   reinterpret_cast<sockaddr*>(&too_small),
                   &too_small_size),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);

    sockaddr_in6 listener_name{};
    int listener_name_size =
        static_cast<int>(sizeof(listener_name));
    REQUIRE_EQ(srt_getsockname(listener,
                   reinterpret_cast<sockaddr*>(&listener_name),
                   &listener_name_size),
        0);
    REQUIRE_EQ(listener_name_size,
        static_cast<int>(sizeof(sockaddr_in6)));
    REQUIRE_EQ(listener_name.sin6_family, AF_INET6);
    REQUIRE_EQ(std::memcmp(&listener_name.sin6_addr,
                   &in6addr_loopback,
                   sizeof(in6addr_loopback)),
        0);
    REQUIRE(ntohs(listener_name.sin6_port) != 0U);
    REQUIRE_EQ(srt_listen(listener, 4), 0);

    std::atomic<SRTSOCKET> accepted{SRT_INVALID_SOCK};
    std::atomic<int> accept_error{SRT_SUCCESS};
    sockaddr_in6 accepted_peer{};
    int accepted_peer_size =
        static_cast<int>(sizeof(accepted_peer));
    std::thread accept_thread([&] {
        accepted.store(srt_accept(listener,
            reinterpret_cast<sockaddr*>(&accepted_peer),
            &accepted_peer_size));
        accept_error.store(srt_getlasterror(nullptr));
    });

    const SRTSOCKET caller = srt_create_socket();
    REQUIRE(caller != SRT_INVALID_SOCK);
    REQUIRE_EQ(srt_setsockflag(caller, SRTO_CONNTIMEO,
                   &timeout_milliseconds,
                   static_cast<int>(sizeof(timeout_milliseconds))),
        0);
    REQUIRE_EQ(srt_setsockflag(caller, SRTO_RCVTIMEO,
                   &timeout_milliseconds,
                   static_cast<int>(sizeof(timeout_milliseconds))),
        0);
    REQUIRE_EQ(srt_setsockflag(caller, SRTO_MSS,
                   &caller_mss,
                   static_cast<int>(sizeof(caller_mss))),
        0);
#ifdef ENABLE_AEAD_API_PREVIEW
    REQUIRE_EQ(srt_setsockflag(caller, SRTO_PASSPHRASE, passphrase,
                   static_cast<int>(sizeof(passphrase) - 1U)),
        0);
    REQUIRE_EQ(srt_setsockflag(caller, SRTO_PBKEYLEN, &key_length,
                   static_cast<int>(sizeof(key_length))),
        0);
    REQUIRE_EQ(srt_setsockflag(caller, SRTO_CRYPTOMODE, &crypto_mode,
                   static_cast<int>(sizeof(crypto_mode))),
        0);
#endif
    const int connect_result = srt_connect(caller,
        reinterpret_cast<const sockaddr*>(&listener_name),
        static_cast<int>(sizeof(listener_name)));
    if (connect_result == SRT_ERROR) {
        (void)srt_close(listener);
    }
    accept_thread.join();

    REQUIRE_EQ(connect_result, 0);
    REQUIRE(accepted.load() != SRT_INVALID_SOCK);
    REQUIRE_EQ(accept_error.load(), SRT_SUCCESS);
    REQUIRE_EQ(accepted_peer_size,
        static_cast<int>(sizeof(sockaddr_in6)));
    REQUIRE_EQ(accepted_peer.sin6_family, AF_INET6);

    for (const SRTSOCKET connected :
         {caller, accepted.load()}) {
        std::int32_t negotiated_mss = 0;
        int negotiated_mss_size =
            static_cast<int>(sizeof(negotiated_mss));
        REQUIRE_EQ(srt_getsockflag(
                       connected, SRTO_MSS,
                       &negotiated_mss,
                       &negotiated_mss_size),
            0);
        REQUIRE_EQ(negotiated_mss, listener_mss);
    }

    for (const SRTSOCKET connected :
         {caller, accepted.load()}) {
        std::int32_t maximum_payload = 0;
        int maximum_payload_size =
            static_cast<int>(sizeof(maximum_payload));
        REQUIRE_EQ(srt_getsockflag(
                       connected, SRTO_PAYLOADSIZE,
                       &maximum_payload,
                       &maximum_payload_size),
            0);
#ifdef ENABLE_AEAD_API_PREVIEW
        REQUIRE_EQ(maximum_payload, 1'200);
        std::int32_t connected_crypto_mode = 0;
        int connected_crypto_mode_size =
            static_cast<int>(sizeof(connected_crypto_mode));
        REQUIRE_EQ(srt_getsockflag(connected, SRTO_CRYPTOMODE,
                       &connected_crypto_mode, &connected_crypto_mode_size),
            0);
        REQUIRE_EQ(connected_crypto_mode, crypto_mode);
#else
        REQUIRE_EQ(maximum_payload, 1'216);
#endif
    }

    std::int32_t caller_ipv6_only = -1;
    int caller_ipv6_only_size =
        static_cast<int>(sizeof(caller_ipv6_only));
    REQUIRE_EQ(srt_getsockflag(caller, SRTO_IPV6ONLY,
                   &caller_ipv6_only,
                   &caller_ipv6_only_size),
        0);
    REQUIRE(caller_ipv6_only == 0
        || caller_ipv6_only == 1);
    std::int32_t accepted_ipv6_only = -1;
    int accepted_ipv6_only_size =
        static_cast<int>(sizeof(accepted_ipv6_only));
    REQUIRE_EQ(srt_getsockflag(accepted.load(), SRTO_IPV6ONLY,
                   &accepted_ipv6_only,
                   &accepted_ipv6_only_size),
        0);
    REQUIRE(accepted_ipv6_only == 0
        || accepted_ipv6_only == 1);

    sockaddr_in6 caller_local{};
    int caller_local_size =
        static_cast<int>(sizeof(caller_local));
    REQUIRE_EQ(srt_getsockname(caller,
                   reinterpret_cast<sockaddr*>(&caller_local),
                   &caller_local_size),
        0);
    REQUIRE_EQ(caller_local.sin6_family, AF_INET6);
    REQUIRE_EQ(accepted_peer.sin6_port, caller_local.sin6_port);

    sockaddr_in6 caller_peer{};
    int caller_peer_size =
        static_cast<int>(sizeof(caller_peer));
    REQUIRE_EQ(srt_getpeername(caller,
                   reinterpret_cast<sockaddr*>(&caller_peer),
                   &caller_peer_size),
        0);
    REQUIRE_EQ(caller_peer.sin6_family, AF_INET6);
    REQUIRE_EQ(caller_peer.sin6_port, listener_name.sin6_port);

    std::array<char, SRT_LIVE_DEF_PLSIZE> received {};
#ifdef ENABLE_AEAD_API_PREVIEW
    std::array<char, 1'201> oversized_request {};
    REQUIRE_EQ(srt_send(caller, oversized_request.data(),
                   static_cast<int>(oversized_request.size())),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVALMSGAPI);
    std::array<char, 1'200> request {};
    for (std::size_t index = 0; index < request.size(); ++index) {
        request[index] = static_cast<char>(index % 251U);
    }
#else
    constexpr auto request = std::to_array("robotweax ipv6 request");
#endif
    REQUIRE_EQ(
        srt_send(caller, request.data(), static_cast<int>(request.size())),
        static_cast<int>(request.size()));
    REQUIRE_EQ(srt_recv(accepted.load(), received.data(),
                   static_cast<int>(received.size())),
        static_cast<int>(request.size()));
    REQUIRE_EQ(std::memcmp(received.data(), request.data(), request.size()), 0);

    constexpr char response[] = "robotweax ipv6 response";
    REQUIRE_EQ(srt_send(accepted.load(), response,
                   static_cast<int>(sizeof(response))),
        static_cast<int>(sizeof(response)));
    received.fill(0);
    REQUIRE_EQ(srt_recv(caller, received.data(),
                   static_cast<int>(received.size())),
        static_cast<int>(sizeof(response)));
    REQUIRE_EQ(std::memcmp(received.data(), response,
                   sizeof(response)),
        0);

    SRT_TRACEBSTATS caller_statistics{};
    SRT_TRACEBSTATS accepted_statistics{};
    REQUIRE_EQ(srt_bstats(
                   caller, &caller_statistics, 0),
        0);
    REQUIRE_EQ(srt_bstats(
                   accepted.load(), &accepted_statistics, 0),
        0);
    REQUIRE_EQ(caller_statistics.byteMSS, listener_mss);
    REQUIRE_EQ(accepted_statistics.byteMSS, listener_mss);
    REQUIRE_EQ(caller_statistics.byteSentTotal,
        request.size()
            + robotweax::srt::compat::ipv6_statistics_packet_header_bytes);
    REQUIRE_EQ(accepted_statistics.byteRecvTotal,
        request.size()
            + robotweax::srt::compat::ipv6_statistics_packet_header_bytes);

    REQUIRE_EQ(srt_close(caller), 0);
    REQUIRE_EQ(srt_close(accepted.load()), 0);
    REQUIRE_EQ(srt_close(listener), 0);
    REQUIRE_EQ(srt_cleanup(), 0);
}

TEST(srt_compat_dual_stack_ipv6_caller_connects_to_ipv4_listener)
{
    constexpr std::int32_t timeout_milliseconds = 2'000;

    sockaddr_in listener_bind{};
    listener_bind.sin_family = AF_INET;
    listener_bind.sin_port = 0;
    listener_bind.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    const SRTSOCKET listener = srt_create_socket();
    REQUIRE(listener != SRT_INVALID_SOCK);
    REQUIRE_EQ(srt_setsockflag(listener, SRTO_CONNTIMEO,
                   &timeout_milliseconds,
                   static_cast<int>(sizeof(timeout_milliseconds))),
        0);
    if (srt_bind(listener,
            reinterpret_cast<const sockaddr*>(&listener_bind),
            static_cast<int>(sizeof(listener_bind))) == SRT_ERROR) {
        int system_error = 0;
        REQUIRE_EQ(
            srt_getlasterror(&system_error), SRT_ESOCKFAIL);
        REQUIRE(system_error != 0);
        REQUIRE_EQ(srt_close(listener), 0);
        REQUIRE_EQ(srt_cleanup(), 0);
        return;
    }
    REQUIRE_EQ(srt_listen(listener, 1), 0);

    sockaddr_in listener_name{};
    int listener_name_size =
        static_cast<int>(sizeof(listener_name));
    REQUIRE_EQ(srt_getsockname(listener,
                   reinterpret_cast<sockaddr*>(&listener_name),
                   &listener_name_size),
        0);

    std::atomic<SRTSOCKET> accepted{SRT_INVALID_SOCK};
    std::thread accept_thread([&] {
        accepted.store(srt_accept(listener, nullptr, nullptr));
    });

    const SRTSOCKET caller = srt_create_socket();
    REQUIRE(caller != SRT_INVALID_SOCK);
    std::int32_t ipv6_only = 0;
    REQUIRE_EQ(srt_setsockflag(caller, SRTO_IPV6ONLY,
                   &ipv6_only,
                   static_cast<int>(sizeof(ipv6_only))),
        0);
    REQUIRE_EQ(srt_setsockflag(caller, SRTO_CONNTIMEO,
                   &timeout_milliseconds,
                   static_cast<int>(sizeof(timeout_milliseconds))),
        0);
    REQUIRE_EQ(srt_setsockflag(caller, SRTO_RCVTIMEO,
                   &timeout_milliseconds,
                   static_cast<int>(sizeof(timeout_milliseconds))),
        0);

    sockaddr_in6 caller_bind{};
    caller_bind.sin6_family = AF_INET6;
    caller_bind.sin6_port = 0;
    caller_bind.sin6_addr = in6addr_any;
    const int caller_bind_result = srt_bind(caller,
        reinterpret_cast<const sockaddr*>(&caller_bind),
        static_cast<int>(sizeof(caller_bind)));
    if (caller_bind_result == SRT_ERROR) {
        (void)srt_close(listener);
        accept_thread.join();
        int system_error = 0;
        REQUIRE_EQ(
            srt_getlasterror(&system_error), SRT_ESOCKFAIL);
        REQUIRE(system_error != 0);
        REQUIRE_EQ(srt_close(caller), 0);
        REQUIRE_EQ(srt_cleanup(), 0);
        return;
    }

    const int connect_result = srt_connect(caller,
        reinterpret_cast<const sockaddr*>(&listener_name),
        static_cast<int>(sizeof(listener_name)));
    if (connect_result == SRT_ERROR) {
        (void)srt_close(listener);
    }
    accept_thread.join();

    REQUIRE_EQ(connect_result, 0);
    REQUIRE(accepted.load() != SRT_INVALID_SOCK);

    sockaddr_in6 caller_peer{};
    int caller_peer_size = static_cast<int>(sizeof(caller_peer));
    REQUIRE_EQ(srt_getpeername(caller,
                   reinterpret_cast<sockaddr*>(&caller_peer),
                   &caller_peer_size),
        0);
    REQUIRE_EQ(caller_peer.sin6_family, AF_INET6);
    const auto* peer_bytes =
        caller_peer.sin6_addr.s6_addr;
    REQUIRE_EQ(peer_bytes[10], 0xffU);
    REQUIRE_EQ(peer_bytes[11], 0xffU);
    REQUIRE_EQ(peer_bytes[12], 127U);
    REQUIRE_EQ(peer_bytes[15], 1U);

    constexpr char payload[] = "dual-stack caller";
    REQUIRE_EQ(srt_send(caller, payload,
                   static_cast<int>(sizeof(payload))),
        static_cast<int>(sizeof(payload)));
    std::array<char, SRT_LIVE_DEF_PLSIZE> received {};
    REQUIRE_EQ(srt_recv(accepted.load(), received.data(),
                   static_cast<int>(received.size())),
        static_cast<int>(sizeof(payload)));
    REQUIRE_EQ(std::memcmp(received.data(), payload,
                   sizeof(payload)),
        0);

    SRT_TRACEBSTATS caller_statistics{};
    SRT_TRACEBSTATS accepted_statistics{};
    REQUIRE_EQ(srt_bstats(caller, &caller_statistics, 0), 0);
    REQUIRE_EQ(
        srt_bstats(accepted.load(), &accepted_statistics, 0),
        0);
    REQUIRE_EQ(caller_statistics.byteSentTotal,
        sizeof(payload)
            + robotweax::srt::compat::
                ipv4_statistics_packet_header_bytes);
    REQUIRE_EQ(accepted_statistics.byteRecvTotal,
        sizeof(payload)
            + robotweax::srt::compat::
                ipv4_statistics_packet_header_bytes);

    REQUIRE_EQ(srt_close(caller), 0);
    REQUIRE_EQ(srt_close(accepted.load()), 0);
    REQUIRE_EQ(srt_close(listener), 0);
    REQUIRE_EQ(srt_cleanup(), 0);
}

TEST(srt_compat_rejects_mixed_address_families_for_rendezvous)
{
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = 0;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    const SRTSOCKET socket = srt_create_socket();
    REQUIRE(socket != SRT_INVALID_SOCK);
    const bool enabled = true;
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_RENDEZVOUS,
                   &enabled, static_cast<int>(sizeof(enabled))),
        0);
    if (srt_bind(socket,
            reinterpret_cast<const sockaddr*>(&local),
            static_cast<int>(sizeof(local))) == SRT_ERROR) {
        int system_error = 0;
        REQUIRE_EQ(srt_getlasterror(&system_error), SRT_ESOCKFAIL);
        REQUIRE(system_error != 0);
        REQUIRE_EQ(srt_close(socket), 0);
        REQUIRE_EQ(srt_cleanup(), 0);
        return;
    }

    sockaddr_in6 peer{};
    peer.sin6_family = AF_INET6;
    peer.sin6_port = htons(9'999);
    peer.sin6_addr = in6addr_loopback;
    REQUIRE_EQ(srt_connect(socket,
                   reinterpret_cast<const sockaddr*>(&peer),
                   static_cast<int>(sizeof(peer))),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);

    REQUIRE_EQ(srt_close(socket), 0);
    REQUIRE_EQ(srt_cleanup(), 0);
}

TEST(srt_compat_encrypted_callers_ignore_stale_induction_before_crypto_policy)
{
    ScopedSrtRuntime runtime;
    REQUIRE_EQ(runtime.startup_result, 0);
    for (const bool blocking : {true, false}) {
        for (unsigned variant = 0; variant < 3; ++variant) {
            const auto listener = srt_create_socket();
            const auto caller = srt_create_socket();
            REQUIRE(listener != SRT_INVALID_SOCK && caller != SRT_INVALID_SOCK);
            constexpr int timeout = 2000;
            constexpr int key_length = 16;
            constexpr bool enforced = true;
            constexpr char secret[] = "synthetic-replay-test-secret";
            for (const auto socket : {listener, caller}) {
                REQUIRE_EQ(srt_setsockflag(socket, SRTO_CONNTIMEO, &timeout,
                               sizeof(timeout)),
                    0);
                REQUIRE_EQ(srt_setsockflag(socket, SRTO_RCVTIMEO, &timeout,
                               sizeof(timeout)),
                    0);
                REQUIRE_EQ(srt_setsockflag(socket, SRTO_PASSPHRASE, secret,
                               sizeof(secret) - 1),
                    0);
                REQUIRE_EQ(srt_setsockflag(socket, SRTO_PBKEYLEN, &key_length,
                               sizeof(key_length)),
                    0);
                REQUIRE_EQ(srt_setsockflag(socket, SRTO_ENFORCEDENCRYPTION,
                               &enforced, sizeof(enforced)),
                    0);
            }
            sockaddr_in address {};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            REQUIRE_EQ(
                srt_bind(listener, reinterpret_cast<const sockaddr*>(&address),
                    sizeof(address)),
                0);
            REQUIRE_EQ(srt_listen(listener, 4), 0);
            int address_size = sizeof(address);
            REQUIRE_EQ(
                srt_getsockname(listener, reinterpret_cast<sockaddr*>(&address),
                    &address_size),
                0);
            const auto record =
                robotweax::srt::compat::SocketRegistry::instance().find(
                    listener);
            InductionReplay replay;
            {
                std::lock_guard lock(record->mutex);
                replay.channel = record->channel;
            }
            replay.variant = variant;
            replay.channel->set_send_hook_for_testing(
                replay_induction_before_conclusion, &replay);
            REQUIRE_EQ(srt_setsockflag(
                           caller, SRTO_RCVSYN, &blocking, sizeof(blocking)),
                0);
            ConnectCallbackObservation callback;
            REQUIRE_EQ(
                srt_connect_callback(caller, observe_connect_result, &callback),
                0);
            SRTSOCKET accepted = SRT_INVALID_SOCK;
            std::jthread accept_thread([&] {
                accepted = srt_accept(listener, nullptr, nullptr);
            });
            const int connected = srt_connect(caller,
                reinterpret_cast<const sockaddr*>(&address), sizeof(address));
            const bool callback_done = blocking
                || wait_for_connect_callback(
                    callback, std::chrono::seconds {3});
            accept_thread.join();
            replay.channel->set_send_hook_for_testing(nullptr, nullptr);
            REQUIRE_EQ(connected, 0);
            REQUIRE(callback_done);
            if (!blocking) {
                REQUIRE_EQ(callback.error_code.load(), SRT_SUCCESS);
            }
            REQUIRE(replay.injected);
            REQUIRE_EQ(srt_getsockstate(caller), SRTS_CONNECTED);
            REQUIRE(accepted != SRT_INVALID_SOCK);
            constexpr char payload[] = "encrypted after replay";
            REQUIRE_EQ(
                srt_send(caller, payload, sizeof(payload)), sizeof(payload));
            std::array<char, SRT_LIVE_DEF_PLSIZE> received {};
            REQUIRE_EQ(srt_recv(accepted, received.data(), received.size()),
                sizeof(payload));
            REQUIRE_EQ(
                std::memcmp(received.data(), payload, sizeof(payload)), 0);
            REQUIRE_EQ(srt_close(caller), 0);
            REQUIRE_EQ(srt_close(accepted), 0);
            REQUIRE_EQ(srt_close(listener), 0);
        }
    }
}

TEST(srt_compat_listener_routes_data_before_sending_final_kmrsp)
{
    constexpr std::int32_t timeout_milliseconds = 2'000;
    constexpr std::int32_t key_length = 16;
    constexpr char passphrase[] = "handoff-race-secret";

    sockaddr_in bind_address{};
    bind_address.sin_family = AF_INET;
    bind_address.sin_port = 0;
    bind_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    const SRTSOCKET listener = srt_create_socket();
    REQUIRE(listener != SRT_INVALID_SOCK);
    REQUIRE_EQ(srt_setsockflag(listener, SRTO_CONNTIMEO,
                   &timeout_milliseconds,
                   static_cast<int>(sizeof(timeout_milliseconds))),
        0);
    REQUIRE_EQ(srt_setsockflag(listener, SRTO_RCVTIMEO,
                   &timeout_milliseconds,
                   static_cast<int>(sizeof(timeout_milliseconds))),
        0);
    REQUIRE_EQ(srt_setsockflag(listener, SRTO_PASSPHRASE,
                   passphrase,
                   static_cast<int>(sizeof(passphrase) - 1U)),
        0);
    REQUIRE_EQ(srt_setsockflag(listener, SRTO_PBKEYLEN,
                   &key_length,
                   static_cast<int>(sizeof(key_length))),
        0);
    if (srt_bind(listener,
            reinterpret_cast<const sockaddr*>(&bind_address),
            static_cast<int>(sizeof(bind_address))) == SRT_ERROR) {
        int system_error = 0;
        REQUIRE_EQ(srt_getlasterror(&system_error), SRT_ESOCKFAIL);
        REQUIRE(system_error != 0);
        REQUIRE_EQ(srt_close(listener), 0);
        REQUIRE_EQ(srt_cleanup(), 0);
        return;
    }
    REQUIRE_EQ(srt_listen(listener, 4), 0);

    sockaddr_in listener_name{};
    int listener_name_size = static_cast<int>(sizeof(listener_name));
    REQUIRE_EQ(srt_getsockname(listener,
                   reinterpret_cast<sockaddr*>(&listener_name),
                   &listener_name_size),
        0);

    const auto listener_record =
        robotweax::srt::compat::SocketRegistry::instance().find(listener);
    REQUIRE(listener_record != nullptr);
    std::shared_ptr<robotweax::srt::compat::DatagramChannel>
        listener_channel;
    {
        std::lock_guard lock(listener_record->mutex);
        listener_channel = listener_record->channel;
    }
    REQUIRE(listener_channel != nullptr);
    FinalHandshakeGate gate{.channel = listener_channel};
    listener_channel->set_send_hook_for_testing(
        gate_final_handshake_response, &gate);

    std::atomic<SRTSOCKET> accepted{SRT_INVALID_SOCK};
    std::atomic<int> accept_error{SRT_SUCCESS};
    std::thread accept_thread([&] {
        accepted.store(srt_accept(listener, nullptr, nullptr));
        accept_error.store(srt_getlasterror(nullptr));
    });

    const SRTSOCKET caller = srt_create_socket();
    REQUIRE(caller != SRT_INVALID_SOCK);
    REQUIRE_EQ(srt_setsockflag(caller, SRTO_CONNTIMEO,
                   &timeout_milliseconds,
                   static_cast<int>(sizeof(timeout_milliseconds))),
        0);
    REQUIRE_EQ(srt_setsockflag(caller, SRTO_PASSPHRASE,
                   passphrase,
                   static_cast<int>(sizeof(passphrase) - 1U)),
        0);
    REQUIRE_EQ(srt_setsockflag(caller, SRTO_PBKEYLEN,
                   &key_length,
                   static_cast<int>(sizeof(key_length))),
        0);

    const int connect_result = srt_connect(caller,
        reinterpret_cast<const sockaddr*>(&listener_name),
        static_cast<int>(sizeof(listener_name)));
    constexpr char payload[] = "first encrypted packet";
    const int send_result = connect_result == 0
        ? srt_send(caller, payload,
              static_cast<int>(sizeof(payload)))
        : SRT_ERROR;
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    gate.release_response.store(true);
    accept_thread.join();
    listener_channel->set_send_hook_for_testing(nullptr, nullptr);

    REQUIRE_EQ(connect_result, 0);
    REQUIRE(gate.response_observed.load());
    REQUIRE_EQ(send_result, static_cast<int>(sizeof(payload)));
    REQUIRE(accepted.load() != SRT_INVALID_SOCK);
    REQUIRE_EQ(accept_error.load(), SRT_SUCCESS);

    std::array<char, SRT_LIVE_DEF_PLSIZE> received {};
    REQUIRE_EQ(srt_recv(accepted.load(), received.data(),
                   static_cast<int>(received.size())),
        static_cast<int>(sizeof(payload)));
    REQUIRE_EQ(std::memcmp(
                   received.data(), payload, sizeof(payload)),
        0);

    REQUIRE_EQ(srt_close(caller), 0);
    REQUIRE_EQ(srt_close(accepted.load()), 0);
    REQUIRE_EQ(srt_close(listener), 0);
    REQUIRE_EQ(srt_cleanup(), 0);
}

TEST(srt_compat_epoll_drives_nonblocking_connect_and_accept)
{
    sockaddr_in bind_address{};
    bind_address.sin_family = AF_INET;
    bind_address.sin_port = 0;
    bind_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    const SRTSOCKET listener = srt_create_socket();
    REQUIRE(listener != SRT_INVALID_SOCK);
    if (srt_bind(listener,
            reinterpret_cast<const sockaddr*>(&bind_address),
            static_cast<int>(sizeof(bind_address))) == SRT_ERROR) {
        int system_error = 0;
        REQUIRE_EQ(srt_getlasterror(&system_error), SRT_ESOCKFAIL);
        REQUIRE(system_error != 0);
        REQUIRE_EQ(srt_close(listener), 0);
        REQUIRE_EQ(srt_cleanup(), 0);
        return;
    }
    REQUIRE_EQ(srt_listen(listener, 4), 0);

    const bool asynchronous = false;
    REQUIRE_EQ(srt_setsockflag(listener, SRTO_RCVSYN,
                   &asynchronous,
                   static_cast<int>(sizeof(asynchronous))),
        0);
    const int listener_poll = srt_epoll_create();
    REQUIRE(listener_poll >= 0);
    const int accept_events = SRT_EPOLL_IN | SRT_EPOLL_ERR;
    REQUIRE_EQ(srt_epoll_add_usock(
                   listener_poll, listener, &accept_events),
        0);

    sockaddr_in listener_name{};
    int listener_name_size = static_cast<int>(sizeof(listener_name));
    REQUIRE_EQ(srt_getsockname(listener,
                   reinterpret_cast<sockaddr*>(&listener_name),
                   &listener_name_size),
        0);

    const SRTSOCKET caller = srt_create_socket();
    REQUIRE(caller != SRT_INVALID_SOCK);
    REQUIRE_EQ(srt_setsockflag(caller, SRTO_RCVSYN,
                   &asynchronous,
                   static_cast<int>(sizeof(asynchronous))),
        0);
    REQUIRE_EQ(srt_setsockflag(caller, SRTO_SNDSYN,
                   &asynchronous,
                   static_cast<int>(sizeof(asynchronous))),
        0);
    const int caller_poll = srt_epoll_create();
    REQUIRE(caller_poll >= 0);
    const int connect_events = SRT_EPOLL_OUT | SRT_EPOLL_ERR;
    REQUIRE_EQ(srt_epoll_add_usock(
                   caller_poll, caller, &connect_events),
        0);

    REQUIRE_EQ(srt_connect(caller,
                   reinterpret_cast<const sockaddr*>(&listener_name),
                   static_cast<int>(sizeof(listener_name))),
        0);
    const SRT_SOCKSTATUS initial_caller_state =
        srt_getsockstate(caller);
    REQUIRE(initial_caller_state == SRTS_CONNECTING
        || initial_caller_state == SRTS_CONNECTED);

    std::array<SRT_EPOLL_EVENT, 2> ready{};
    REQUIRE_EQ(srt_epoll_uwait(caller_poll, ready.data(),
                   static_cast<int>(ready.size()), 5'000),
        1);
    REQUIRE_EQ(ready[0].fd, caller);
    REQUIRE_EQ(ready[0].events, SRT_EPOLL_OUT);
    REQUIRE_EQ(srt_getsockstate(caller), SRTS_CONNECTED);

    REQUIRE_EQ(srt_epoll_uwait(listener_poll, ready.data(),
                   static_cast<int>(ready.size()), 5'000),
        1);
    REQUIRE_EQ(ready[0].fd, listener);
    REQUIRE_EQ(ready[0].events, SRT_EPOLL_IN);

    const SRTSOCKET accepted =
        srt_accept(listener, nullptr, nullptr);
    REQUIRE(accepted != SRT_INVALID_SOCK);
    REQUIRE_EQ(srt_getsockstate(accepted), SRTS_CONNECTED);
    REQUIRE_EQ(srt_accept(listener, nullptr, nullptr),
        SRT_INVALID_SOCK);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EASYNCRCV);

    REQUIRE_EQ(srt_epoll_release(caller_poll), 0);
    REQUIRE_EQ(srt_epoll_release(listener_poll), 0);
    REQUIRE_EQ(srt_close(caller), 0);
    REQUIRE_EQ(srt_close(accepted), 0);
    REQUIRE_EQ(srt_close(listener), 0);
    REQUIRE_EQ(srt_cleanup(), 0);
}

TEST(srt_compat_connect_callback_reports_nonblocking_success_without_locks)
{
    constexpr std::int32_t timeout_milliseconds = 2'000;
    sockaddr_in bind_address{};
    bind_address.sin_family = AF_INET;
    bind_address.sin_port = 0;
    bind_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    const SRTSOCKET listener = srt_create_socket();
    REQUIRE(listener != SRT_INVALID_SOCK);
    if (srt_bind(listener,
            reinterpret_cast<const sockaddr*>(&bind_address),
            static_cast<int>(sizeof(bind_address))) == SRT_ERROR) {
        int system_error = 0;
        REQUIRE_EQ(srt_getlasterror(&system_error), SRT_ESOCKFAIL);
        REQUIRE(system_error != 0);
        REQUIRE_EQ(srt_close(listener), 0);
        REQUIRE_EQ(srt_cleanup(), 0);
        return;
    }
    REQUIRE_EQ(srt_listen(listener, 4), 0);

    sockaddr_in listener_name{};
    int listener_name_size = static_cast<int>(sizeof(listener_name));
    REQUIRE_EQ(srt_getsockname(listener,
                   reinterpret_cast<sockaddr*>(&listener_name),
                   &listener_name_size),
        0);

    const SRTSOCKET caller = srt_create_socket();
    REQUIRE(caller != SRT_INVALID_SOCK);
    const bool asynchronous = false;
    REQUIRE_EQ(srt_setsockflag(caller, SRTO_RCVSYN,
                   &asynchronous,
                   static_cast<int>(sizeof(asynchronous))),
        0);
    REQUIRE_EQ(srt_setsockflag(caller, SRTO_CONNTIMEO,
                   &timeout_milliseconds,
                   static_cast<int>(sizeof(timeout_milliseconds))),
        0);
    ConnectCallbackObservation observation;
    observation.throw_after_callback = true;
    REQUIRE_EQ(srt_connect_callback(caller,
                   observe_connect_result, &observation),
        0);
    REQUIRE_EQ(srt_connect(caller,
                   reinterpret_cast<const sockaddr*>(&listener_name),
                   static_cast<int>(sizeof(listener_name))),
        0);
    REQUIRE(wait_for_connect_callback(
        observation, std::chrono::seconds{5}));
    REQUIRE_EQ(observation.calls.load(), 1);
    REQUIRE_EQ(observation.socket.load(), caller);
    REQUIRE_EQ(observation.error_code.load(), SRT_SUCCESS);
    REQUIRE_EQ(observation.peer_family.load(), AF_INET);
    REQUIRE_EQ(observation.peer_port.load(), ntohs(listener_name.sin_port));
    REQUIRE_EQ(observation.token.load(), -1);
    REQUIRE_EQ(observation.socket_state.load(), SRTS_CONNECTED);
    REQUIRE(observation.peer_name_available.load());
    const auto caller_record =
        robotweax::srt::compat::SocketRegistry::instance().find(caller);
    REQUIRE(caller_record != nullptr);
    REQUIRE(wait_for_connect_cleanup(caller_record, std::chrono::seconds {5}));
    {
        std::lock_guard lock(caller_record->mutex);
        REQUIRE(!caller_record->connect_worker.joinable());
        REQUIRE(caller_record->connect_handshake_operation == nullptr);
    }

    const SRTSOCKET accepted = srt_accept(listener, nullptr, nullptr);
    REQUIRE(accepted != SRT_INVALID_SOCK);
    std::this_thread::sleep_for(std::chrono::milliseconds{25});
    REQUIRE_EQ(observation.calls.load(), 1);

    REQUIRE_EQ(srt_close(caller), 0);
    REQUIRE_EQ(srt_close(accepted), 0);
    REQUIRE_EQ(srt_close(listener), 0);
    REQUIRE_EQ(srt_cleanup(), 0);
}

TEST(srt_compat_external_close_waits_for_caller_actor_callback)
{
    sockaddr_in bind_address {};
    bind_address.sin_family = AF_INET;
    bind_address.sin_port = 0;
    bind_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    const SRTSOCKET listener = srt_create_socket();
    REQUIRE(listener != SRT_INVALID_SOCK);
    if (srt_bind(listener, reinterpret_cast<const sockaddr*>(&bind_address),
            static_cast<int>(sizeof(bind_address)))
        == SRT_ERROR) {
        int system_error = 0;
        REQUIRE_EQ(srt_getlasterror(&system_error), SRT_ESOCKFAIL);
        REQUIRE(system_error != 0);
        REQUIRE_EQ(srt_close(listener), 0);
        REQUIRE_EQ(srt_cleanup(), 0);
        return;
    }
    REQUIRE_EQ(srt_listen(listener, 1), 0);
    sockaddr_in listener_name {};
    int listener_name_size = static_cast<int>(sizeof(listener_name));
    REQUIRE_EQ(
        srt_getsockname(listener, reinterpret_cast<sockaddr*>(&listener_name),
            &listener_name_size),
        0);

    const SRTSOCKET caller = srt_create_socket();
    REQUIRE(caller != SRT_INVALID_SOCK);
    const bool asynchronous = false;
    REQUIRE_EQ(srt_setsockflag(caller, SRTO_RCVSYN, &asynchronous,
                   static_cast<int>(sizeof(asynchronous))),
        0);
    ConnectCallbackObservation observation;
    observation.block_callback = true;
    REQUIRE_EQ(
        srt_connect_callback(caller, observe_connect_result, &observation), 0);
    REQUIRE_EQ(
        srt_connect(caller, reinterpret_cast<const sockaddr*>(&listener_name),
            static_cast<int>(sizeof(listener_name))),
        0);
    {
        std::unique_lock lock(observation.callback_mutex);
        REQUIRE(observation.callback_changed.wait_for(
            lock, std::chrono::seconds {5}, [&observation] {
                return observation.callback_entered;
            }));
    }

    std::atomic_bool close_finished = false;
    std::atomic_int close_result = SRT_ERROR;
    std::thread closer([&] {
        close_result.store(srt_close(caller));
        close_finished.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds {25});
    const bool closed_before_release =
        close_finished.load(std::memory_order_acquire);
    {
        std::lock_guard lock(observation.callback_mutex);
        observation.release_callback = true;
    }
    observation.callback_changed.notify_all();
    closer.join();

    REQUIRE(!closed_before_release);
    REQUIRE_EQ(close_result.load(), 0);
    REQUIRE(close_finished.load(std::memory_order_acquire));
    REQUIRE_EQ(observation.calls.load(), 1);
    REQUIRE_EQ(observation.error_code.load(), SRT_SUCCESS);
    const SRTSOCKET accepted = srt_accept(listener, nullptr, nullptr);
    if (accepted != SRT_INVALID_SOCK) {
        REQUIRE_EQ(srt_close(accepted), 0);
    }
    REQUIRE_EQ(srt_close(listener), 0);
    REQUIRE_EQ(srt_cleanup(), 0);
}

TEST(srt_compat_caller_actor_callback_can_perform_final_cleanup)
{
    REQUIRE_EQ(srt_startup(), 0);
    sockaddr_in bind_address {};
    bind_address.sin_family = AF_INET;
    bind_address.sin_port = 0;
    bind_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    const SRTSOCKET listener = srt_create_socket();
    REQUIRE(listener != SRT_INVALID_SOCK);
    if (srt_bind(listener, reinterpret_cast<const sockaddr*>(&bind_address),
            static_cast<int>(sizeof(bind_address)))
        == SRT_ERROR) {
        int system_error = 0;
        REQUIRE_EQ(srt_getlasterror(&system_error), SRT_ESOCKFAIL);
        REQUIRE(system_error != 0);
        REQUIRE_EQ(srt_cleanup(), 0);
        return;
    }
    REQUIRE_EQ(srt_listen(listener, 1), 0);
    sockaddr_in listener_name {};
    int listener_name_size = static_cast<int>(sizeof(listener_name));
    REQUIRE_EQ(
        srt_getsockname(listener, reinterpret_cast<sockaddr*>(&listener_name),
            &listener_name_size),
        0);

    const SRTSOCKET caller = srt_create_socket();
    REQUIRE(caller != SRT_INVALID_SOCK);
    const bool asynchronous = false;
    REQUIRE_EQ(srt_setsockflag(caller, SRTO_RCVSYN, &asynchronous,
                   static_cast<int>(sizeof(asynchronous))),
        0);
    CallbackCleanupObservation observation;
    REQUIRE_EQ(srt_connect_callback(
                   caller, cleanup_from_connect_callback, &observation),
        0);
    REQUIRE_EQ(
        srt_connect(caller, reinterpret_cast<const sockaddr*>(&listener_name),
            static_cast<int>(sizeof(listener_name))),
        0);

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds {5};
    while (!observation.finished.load(std::memory_order_acquire)
        && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds {1});
    }
    REQUIRE(observation.finished.load(std::memory_order_acquire));
    REQUIRE_EQ(observation.result.load(), 0);
    REQUIRE_EQ(srt_getsockstate(caller), SRTS_NONEXIST);
    REQUIRE_EQ(srt_getsockstate(listener), SRTS_NONEXIST);
}

TEST(srt_compat_connect_callback_reports_timeout_and_close_cancellation)
{
    const UDPSOCKET sink = create_udp_socket();
#if defined(_WIN32)
    if (sink == INVALID_SOCKET) {
#else
    if (sink < 0) {
#endif
        return;
    }
    sockaddr_in sink_address{};
    sink_address.sin_family = AF_INET;
    sink_address.sin_port = 0;
    sink_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(sink,
            reinterpret_cast<const sockaddr*>(&sink_address),
            static_cast<NativeSocketLength>(sizeof(sink_address))) != 0) {
        close_udp_socket(sink);
        return;
    }
    NativeSocketLength sink_address_size =
        static_cast<NativeSocketLength>(sizeof(sink_address));
    REQUIRE_EQ(::getsockname(sink,
                   reinterpret_cast<sockaddr*>(&sink_address),
                   &sink_address_size),
        0);

    const bool asynchronous = false;
    const auto start_connect = [&](std::int32_t timeout,
                                   ConnectCallbackObservation& observation) {
        const SRTSOCKET socket = srt_create_socket();
        REQUIRE(socket != SRT_INVALID_SOCK);
        REQUIRE_EQ(srt_setsockflag(socket, SRTO_RCVSYN,
                       &asynchronous,
                       static_cast<int>(sizeof(asynchronous))),
            0);
        REQUIRE_EQ(srt_setsockflag(socket, SRTO_CONNTIMEO,
                       &timeout, static_cast<int>(sizeof(timeout))),
            0);
        REQUIRE_EQ(srt_connect_callback(socket,
                       observe_connect_result, &observation),
            0);
        if (srt_connect(socket,
                reinterpret_cast<const sockaddr*>(&sink_address),
                static_cast<int>(sizeof(sink_address))) == SRT_ERROR) {
            int system_error = 0;
            REQUIRE_EQ(srt_getlasterror(&system_error), SRT_ESOCKFAIL);
            REQUIRE(system_error != 0);
            REQUIRE_EQ(srt_close(socket), 0);
            return SRT_INVALID_SOCK;
        }
        return socket;
    };

    ConnectCallbackObservation timeout_observation;
    const SRTSOCKET timed_out =
        start_connect(350, timeout_observation);
    if (timed_out != SRT_INVALID_SOCK) {
        const auto record =
            robotweax::srt::compat::SocketRegistry::instance().find(timed_out);
        REQUIRE(record != nullptr);
        {
            std::lock_guard lock(record->mutex);
            REQUIRE(!record->connect_worker.joinable());
            REQUIRE(record->connect_handshake_operation != nullptr);
        }
        REQUIRE(wait_for_connect_callback(
            timeout_observation, std::chrono::seconds{3}));
        REQUIRE_EQ(timeout_observation.calls.load(), 1);
        REQUIRE_EQ(timeout_observation.socket.load(), timed_out);
        REQUIRE_EQ(timeout_observation.error_code.load(), SRT_ENOSERVER);
        REQUIRE_EQ(timeout_observation.peer_family.load(), AF_INET);
        REQUIRE_EQ(timeout_observation.peer_port.load(),
            ntohs(sink_address.sin_port));
        REQUIRE_EQ(timeout_observation.token.load(), -1);
        REQUIRE(!timeout_observation.peer_name_available.load());
        REQUIRE_EQ(srt_close(timed_out), 0);
    }

    ConnectCallbackObservation close_observation;
    const SRTSOCKET cancelled =
        start_connect(30'000, close_observation);
    if (cancelled != SRT_INVALID_SOCK) {
        REQUIRE_EQ(srt_close(cancelled), 0);
        REQUIRE(close_observation.finished.load(std::memory_order_acquire));
        REQUIRE_EQ(close_observation.calls.load(), 1);
        REQUIRE_EQ(close_observation.socket.load(), cancelled);
        REQUIRE_EQ(close_observation.error_code.load(), SRT_ESCLOSED);
        REQUIRE_EQ(close_observation.peer_family.load(), AF_INET);
        REQUIRE_EQ(close_observation.peer_port.load(),
            ntohs(sink_address.sin_port));
        REQUIRE_EQ(close_observation.token.load(), -1);
        REQUIRE(!close_observation.peer_name_available.load());
    }

    close_udp_socket(sink);
    REQUIRE_EQ(srt_cleanup(), 0);
}

TEST(srt_compat_last_error_is_thread_local)
{
    srt_clearlasterror();
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_SUCCESS);

    int worker_error = SRT_SUCCESS;
    std::thread worker([&worker_error] {
        int value = 0;
        int size = static_cast<int>(sizeof(value));
        REQUIRE_EQ(srt_getsockflag(
                       987'654'321, SRTO_FC, &value, &size),
            SRT_ERROR);
        worker_error = srt_getlasterror(nullptr);
    });
    worker.join();

    REQUIRE_EQ(worker_error, SRT_EINVSOCK);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_SUCCESS);
}

TEST(srt_compat_robotweax_version_identifies_local_implementation)
{
    static_assert(SRTO_ROBOTWEAX_VERSION == 0x01000001);
    const SRTSOCKET socket = srt_create_socket();
    const SRTSOCKET group = srt_create_group(SRT_GTYPE_BROADCAST);
    REQUIRE(socket != SRT_INVALID_SOCK);
    REQUIRE(group != SRT_INVALID_SOCK);
    for (const SRTSOCKET handle : {socket, group}) {
        std::int32_t value = -1;
        int size = sizeof(value);
        REQUIRE_EQ(
            srt_getsockflag(handle, SRTO_ROBOTWEAX_VERSION, &value, &size), 0);
        REQUIRE_EQ(value, ROBOTWEAX_SRT_VERSION_VALUE);
        REQUIRE_EQ(size, static_cast<int>(sizeof(value)));
        REQUIRE_EQ(srt_setsockflag(
                       handle, SRTO_ROBOTWEAX_VERSION, &value, sizeof(value)),
            SRT_ERROR);
        size = 1;
        value = -1;
        REQUIRE_EQ(
            srt_getsockflag(handle, SRTO_ROBOTWEAX_VERSION, &value, &size),
            SRT_ERROR);
        REQUIRE_EQ(value, -1);
        REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);
        size = sizeof(value);
        REQUIRE_EQ(
            srt_getsockflag(handle, SRTO_ROBOTWEAX_VERSION, nullptr, &size),
            SRT_ERROR);
        REQUIRE_EQ(
            srt_getsockflag(handle, SRTO_ROBOTWEAX_VERSION, &value, nullptr),
            SRT_ERROR);
    }
    std::int32_t value = 0;
    int size = sizeof(value);
    REQUIRE_EQ(srt_getsockflag(socket, SRTO_VERSION, &value, &size), 0);
    REQUIRE_EQ(value, SRT_VERSION_VALUE);
    REQUIRE_EQ(srt_getversion(), static_cast<std::uint32_t>(SRT_VERSION_VALUE));
    REQUIRE_EQ(srt_close(socket), 0);
    REQUIRE_EQ(srt_close(group), 0);
    REQUIRE_EQ(srt_getsockflag(socket, SRTO_ROBOTWEAX_VERSION, &value, &size),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVSOCK);
}

TEST(srt_compat_message_control_and_helpers_match_public_layout)
{
    SRT_MSGCTRL control{};
    control.flags = 99;
    srt_msgctrl_init(&control);
    REQUIRE_EQ(control.flags, 0);
    REQUIRE_EQ(control.msgttl, SRT_MSGTTL_INF);
    REQUIRE_EQ(control.inorder, 0);
    REQUIRE_EQ(control.boundary, 0);
    REQUIRE_EQ(control.srctime, 0);
    REQUIRE_EQ(control.pktseq, SRT_SEQNO_NONE);
    REQUIRE_EQ(control.msgno, SRT_MSGNO_NONE);
    REQUIRE(control.grpdata == nullptr);
    REQUIRE_EQ(control.grpdata_size, 0U);

    REQUIRE_EQ(srt_getversion(), static_cast<std::uint32_t>(SRT_VERSION_VALUE));
    REQUIRE(srt_time_now() > 0);
    REQUIRE_EQ(srt_clock_type(), SRT_SYNC_CLOCK_STDCXX_STEADY);
}

TEST(srt_compat_sendmsg_accepts_finite_and_other_negative_ttl_values)
{
    const SRTSOCKET socket = srt_create_socket();
    REQUIRE(socket != SRT_INVALID_SOCK);
    constexpr char payload[] = "ttl";

    SRT_MSGCTRL control = srt_msgctrl_default;
    control.msgttl = 25;
    REQUIRE_EQ(srt_sendmsg2(socket, payload,
                   static_cast<int>(sizeof(payload)), &control),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ENOCONN);

    REQUIRE_EQ(srt_sendmsg(socket, payload,
                   static_cast<int>(sizeof(payload)), -2, 0),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ENOCONN);
    REQUIRE_EQ(srt_close(socket), 0);
}

TEST(srt_compat_receive_drains_buffered_data_after_peer_shutdown)
{
    const auto attach_runtime = [](SRTSOCKET socket, bool stream_mode) {
        const auto channel =
            std::make_shared<robotweax::srt::compat::DatagramChannel>();
        channel->set_send_hook_for_testing(
            [](std::span<const std::byte> bytes, robotweax::srt::IpEndpoint,
                void*) noexcept {
                return robotweax::srt::UdpIoResult {
                    .bytes_transferred = bytes.size()};
            },
            nullptr);
        robotweax::srt::SocketOptions options;
        if (stream_mode) {
            REQUIRE_EQ(
                options.set(robotweax::srt::SocketOption::transmission_type,
                    static_cast<std::int64_t>(SRTT_FILE)),
                robotweax::srt::Error::none);
        }
        constexpr std::uint32_t initial_sequence = 40'000;
        auto runtime =
            std::make_shared<robotweax::srt::compat::ConnectionRuntime>(
                robotweax::srt::compat::ConnectionRuntime::Configuration {
                    .channel = channel,
                    .peer = robotweax::srt::IpEndpoint::loopback(9'000),
                    .peer_socket_id = 77,
                    .initial_sequence =
                        robotweax::srt::SequenceNumber {initial_sequence},
                    .peer_initial_sequence =
                        robotweax::srt::SequenceNumber {initial_sequence},
                    .has_distinct_peer_initial_sequence = true,
                    .options = options,
                    .origin =
                        robotweax::srt::compat::ConnectionRuntime::Clock::now(),
                });
        const auto record =
            robotweax::srt::compat::SocketRegistry::instance().find(socket);
        REQUIRE(record != nullptr);
        {
            std::lock_guard lock(record->mutex);
            record->state = SRTS_CONNECTED;
            record->channel = channel;
            record->runtime = runtime;
            record->public_options.message_api = !stream_mode;
            record->public_options.tsbpd_mode = !stream_mode;
            record->public_options.transmission_type =
                stream_mode ? SRTT_FILE : SRTT_LIVE;
        }
        return runtime;
    };

    const auto deliver_then_shutdown = [](const auto& runtime,
                                           std::span<const std::byte> payload) {
        robotweax::srt::PacketView data;
        data.kind = robotweax::srt::PacketKind::data;
        data.data.sequence = robotweax::srt::SequenceNumber {40'000};
        data.data.message_number = 1;
        data.data.boundary = robotweax::srt::MessageBoundary::solo;
        data.data.in_order = true;
        data.payload = payload;
        runtime->process_packet(
            data, robotweax::srt::IpEndpoint::loopback(9'000));

        robotweax::srt::PacketView shutdown;
        shutdown.kind = robotweax::srt::PacketKind::control;
        shutdown.control.type = robotweax::srt::ControlType::shutdown;
        shutdown.control.destination_socket_id = 77;
        const std::array<std::byte, 4> shutdown_padding {};
        shutdown.payload = shutdown_padding;
        runtime->process_packet(
            shutdown, robotweax::srt::IpEndpoint::loopback(9'000));
    };

    const SRTSOCKET message_socket = srt_create_socket();
    REQUIRE(message_socket != SRT_INVALID_SOCK);
    const auto message_runtime = attach_runtime(message_socket, false);
    constexpr std::array<std::byte, 7> message_payload {std::byte {'m'},
        std::byte {'e'}, std::byte {'s'}, std::byte {'s'}, std::byte {'a'},
        std::byte {'g'}, std::byte {'e'}};
    deliver_then_shutdown(message_runtime, message_payload);
    REQUIRE_EQ(srt_getsockstate(message_socket), SRTS_BROKEN);

    std::array<char, 3> short_message {};
    REQUIRE_EQ(srt_recvmsg(message_socket, short_message.data(),
                   static_cast<int>(short_message.size())),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVALMSGAPI);

    std::array<char, SRT_LIVE_DEF_PLSIZE> message {};
    REQUIRE_EQ(srt_recvmsg(message_socket, message.data(),
                   static_cast<int>(message.size())),
        static_cast<int>(message_payload.size()));
    REQUIRE(std::equal(message_payload.begin(), message_payload.end(),
        reinterpret_cast<const std::byte*>(message.data())));
    REQUIRE_EQ(srt_recvmsg(message_socket, message.data(),
                   static_cast<int>(message.size())),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ECONNLOST);
    REQUIRE_EQ(srt_close(message_socket), 0);

    const SRTSOCKET stream_socket = srt_create_socket();
    REQUIRE(stream_socket != SRT_INVALID_SOCK);
    const auto stream_runtime = attach_runtime(stream_socket, true);
    constexpr std::array<std::byte, 7> stream_payload {std::byte {'s'},
        std::byte {'t'}, std::byte {'r'}, std::byte {'e'}, std::byte {'a'},
        std::byte {'m'}, std::byte {'!'}};
    deliver_then_shutdown(stream_runtime, stream_payload);
    REQUIRE_EQ(srt_getsockstate(stream_socket), SRTS_BROKEN);

    std::array<char, 3> first_stream_read {};
    REQUIRE_EQ(srt_recv(stream_socket, first_stream_read.data(),
                   static_cast<int>(first_stream_read.size())),
        static_cast<int>(first_stream_read.size()));
    std::array<char, 8> second_stream_read {};
    REQUIRE_EQ(srt_recv(stream_socket, second_stream_read.data(),
                   static_cast<int>(second_stream_read.size())),
        4);
    REQUIRE(std::equal(stream_payload.begin(), stream_payload.begin() + 3,
        reinterpret_cast<const std::byte*>(first_stream_read.data())));
    REQUIRE(std::equal(stream_payload.begin() + 3, stream_payload.end(),
        reinterpret_cast<const std::byte*>(second_stream_read.data())));
    REQUIRE_EQ(srt_recv(stream_socket, second_stream_read.data(),
                   static_cast<int>(second_stream_read.size())),
        0);
    REQUIRE_EQ(srt_close(stream_socket), 0);
}
