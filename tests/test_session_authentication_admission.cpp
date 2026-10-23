/* SPDX-License-Identifier: MIT */
#include "test.hpp"
#include "compat/socket_registry.hpp"
#include "srt/srt.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <string_view>

using namespace robotweax::srt::compat;

namespace {
constexpr std::string_view shared_secret = "shared-port-session-auth-fixture";
struct Runtime {
    Runtime()
    {
        REQUIRE_EQ(srt_startup(), 0);
    }
    ~Runtime()
    {
        (void)srt_cleanup();
    }
};
struct Socket {
    SRTSOCKET id = srt_create_socket();
    Socket()
    {
        REQUIRE(id != SRT_INVALID_SOCK);
    }
    ~Socket()
    {
        (void)srt_close(id);
    }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
};
template <class T> int set(SRTSOCKET socket, SRT_SOCKOPT option, T value)
{
    return srt_setsockflag(socket, option, &value, sizeof(value));
}
bool flag(SRTSOCKET socket, SRT_SOCKOPT option)
{
    bool value = false;
    int size = sizeof(value);
    REQUIRE_EQ(srt_getsockflag(socket, option, &value, &size), 0);
    return value;
}
bool auth(SRTSOCKET socket)
{
    return flag(socket, SRTO_ROBOTWEAX_SESSIONAUTH);
}
void configure(SRTSOCKET socket, bool authenticated, int mode,
    std::string_view secret = shared_secret)
{
    for (auto option : {SRTO_CONNTIMEO, SRTO_RCVTIMEO, SRTO_SNDTIMEO})
        REQUIRE_EQ(set(socket, option, std::int32_t {1000}), 0);
    for (auto option : {SRTO_SNDBUF, SRTO_RCVBUF})
        REQUIRE_EQ(set(socket, option, std::int32_t {65536}), 0);
    REQUIRE_EQ(set(socket, SRTO_LATENCY, std::int32_t {0}), 0);
    REQUIRE_EQ(set(socket, SRTO_KMPREANNOUNCE, std::int32_t {1}), 0);
    REQUIRE_EQ(set(socket, SRTO_KMREFRESHRATE, std::int32_t {8}), 0);
    REQUIRE_EQ(set(socket, SRTO_ROBOTWEAX_SESSIONAUTH, authenticated), 0);
    REQUIRE_EQ(srt_setsockflag(socket, SRTO_PASSPHRASE, secret.data(),
                   static_cast<int>(secret.size())),
        0);
#ifdef ENABLE_AEAD_API_PREVIEW
    REQUIRE_EQ(set(socket, SRTO_CRYPTOMODE, std::int32_t {mode}), 0);
#else
    REQUIRE_EQ(mode, 1);
#endif
}
struct Admission {
    // Rejection, closure and a final missing-secret configuration must not leak
    // their choices into the listener or its next provisional child.
    std::atomic_int action {0}; // 0 accept, 1 reject, 2 close, 3 clear secret
    std::atomic_bool selected {false};
    std::atomic_bool supply_secret {false};
    std::atomic_bool closed_setter_rejected {false};
    std::atomic_int calls {0};
};
int select(void* opaque, SRTSOCKET child, int, const sockaddr*,
    const char* id) noexcept
{
    auto& admission = *static_cast<Admission*>(opaque);
    ++admission.calls;
    const std::string_view stream = id == nullptr ? "" : id;
    const bool secure = stream.starts_with("secure-");
    const auto profile = stream.ends_with("control") ? SRTT_CONTROL
        : stream.ends_with("sensor")                 ? SRTT_SENSOR
        : stream.ends_with("file")                   ? SRTT_FILE
                                                     : SRTT_LIVE;
    bool ok = set(child, SRTO_TRANSTYPE, profile) == 0;
    ok = set(child, SRTO_LATENCY, std::int32_t {0}) == 0 && ok;
    // Enforced encryption must precede enabling SESSIONAUTH. Disable
    // SESSIONAUTH first if an ordinary child needs optional encryption.
    if (secure)
        ok = set(child, SRTO_ENFORCEDENCRYPTION, true) == 0 && ok;
    ok = set(child, SRTO_ROBOTWEAX_SESSIONAUTH, secure) == 0 && ok;
    if (stream == "clear-file") {
        ok = set(child, SRTO_ENFORCEDENCRYPTION, false) == 0 && ok;
        ok = srt_setsockflag(child, SRTO_PASSPHRASE, nullptr, 0) == 0 && ok;
    }
    if (admission.supply_secret)
        ok = srt_setsockflag(child, SRTO_PASSPHRASE, shared_secret.data(),
                 static_cast<int>(shared_secret.size()))
                == 0
            && ok;
    if (admission.action == 3)
        ok = srt_setsockflag(child, SRTO_PASSPHRASE, nullptr, 0) == 0 && ok;
    admission.selected = ok;
    if (admission.action == 2) {
        (void)srt_close(child);
        admission.closed_setter_rejected =
            set(child, SRTO_ROBOTWEAX_SESSIONAUTH, false) == SRT_ERROR;
    }
    return !ok || admission.action == 1 ? -1 : 0;
}
struct Listener {
    Socket socket;
    sockaddr_in address {};
    Listener(Admission& admission, bool authenticated, int mode,
        std::string_view secret = shared_secret, bool enforced = true)
    {
        configure(socket.id, authenticated, mode, secret);
        REQUIRE_EQ(set(socket.id, SRTO_ENFORCEDENCRYPTION, enforced), 0);
        REQUIRE_EQ(srt_listen_callback(socket.id, select, &admission), 0);
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        REQUIRE_EQ(
            srt_bind(socket.id, reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)),
            0);
        REQUIRE_EQ(srt_listen(socket.id, 8), 0);
        int size = sizeof(address);
        REQUIRE_EQ(srt_getsockname(
                       socket.id, reinterpret_cast<sockaddr*>(&address), &size),
            0);
    }
};
int connect(Socket& caller, const Listener& listener, std::string_view stream,
    bool authenticated, int mode, std::string_view secret = shared_secret)
{
    const auto profile = stream.ends_with("control") ? SRTT_CONTROL
        : stream.ends_with("sensor")                 ? SRTT_SENSOR
        : stream.ends_with("file")                   ? SRTT_FILE
                                                     : SRTT_LIVE;
    REQUIRE_EQ(set(caller.id, SRTO_TRANSTYPE, profile), 0);
    configure(caller.id, authenticated, mode, secret);
    REQUIRE_EQ(srt_setsockflag(caller.id, SRTO_STREAMID, stream.data(),
                   static_cast<int>(stream.size())),
        0);
    return srt_connect(caller.id,
        reinterpret_cast<const sockaddr*>(&listener.address),
        sizeof(listener.address));
}
void expect_runtime_auth(SRTSOCKET socket, bool authenticated)
{
    REQUIRE_EQ(auth(socket), authenticated);
    const auto record = SocketRegistry::instance().find(socket);
    REQUIRE(record);
    std::lock_guard lock(record->mutex);
    REQUIRE(record->runtime);
    REQUIRE(record->crypto);
    const auto session = record->crypto->session_authentication();
    REQUIRE_EQ(session != nullptr, authenticated);
    if (session != nullptr)
        REQUIRE(session->ready());
}
void exchange(SRTSOCKET caller, SRTSOCKET child)
{
    for (int index = 0; index < 24; ++index) {
        const std::array<char, 3> payload {static_cast<char>(index), '\0', 'x'};
        for (bool reverse : {false, true}) {
            const auto sender = reverse ? child : caller;
            const auto receiver = reverse ? caller : child;
            REQUIRE_EQ(srt_send(sender, payload.data(), payload.size()),
                static_cast<int>(payload.size()));
            std::array<char, 1500> received {};
            REQUIRE_EQ(srt_recv(receiver, received.data(), received.size()),
                static_cast<int>(payload.size()));
            REQUIRE(
                std::equal(payload.begin(), payload.end(), received.begin()));
        }
    }
}
void empty_accept_queue(const Listener& listener)
{
    REQUIRE_EQ(set(listener.socket.id, SRTO_RCVSYN, false), 0);
    REQUIRE_EQ(
        srt_accept(listener.socket.id, nullptr, nullptr), SRT_INVALID_SOCK);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EASYNCRCV);
    REQUIRE_EQ(set(listener.socket.id, SRTO_RCVSYN, true), 0);
}
} // namespace

TEST(srt_compat_session_auth_admission_mixes_profiles_and_rotates_on_one_port)
{
    Runtime runtime;
    for (int mode : {1, 2}) {
#ifndef ENABLE_AEAD_API_PREVIEW
        if (mode == 2)
            continue;
#endif
        for (bool inherited : {false, true}) {
            Admission admission;
            Listener listener(admission, inherited, mode);
            const std::array<std::string_view, 4> streams {"ordinary-live",
                "secure-control", "secure-sensor", "ordinary-file"};
            std::array<Socket, 4> callers;
            std::array<SRTSOCKET, 4> children {};
            for (std::size_t index = 0; index < streams.size(); ++index) {
                const bool secure = streams[index].starts_with("secure-");
                REQUIRE_EQ(connect(callers[index], listener, streams[index],
                               secure, mode),
                    0);
                children[index] =
                    srt_accept(listener.socket.id, nullptr, nullptr);
                REQUIRE(children[index] != SRT_INVALID_SOCK);
                REQUIRE(admission.selected);
                REQUIRE_EQ(auth(listener.socket.id), inherited);
                for (auto socket : {callers[index].id, children[index]})
                    expect_runtime_auth(socket, secure);
                REQUIRE_EQ(
                    set(children[index], SRTO_ROBOTWEAX_SESSIONAUTH, !secure),
                    SRT_ERROR);
                REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ECONNSOCK);
            }
            for (std::size_t index = 0; index < children.size(); ++index) {
                exchange(callers[index].id, children[index]);
                REQUIRE_EQ(srt_close(children[index]), 0);
            }
            REQUIRE_EQ(auth(listener.socket.id), inherited);
        }
    }
}

TEST(
    srt_compat_session_auth_admission_rejects_mismatch_and_failed_children_do_not_leak)
{
    Runtime runtime;
    Admission admission;
    Listener listener(admission, false, 1);
    for (int variant = 0; variant < 6; ++variant) {
        admission.action = variant >= 3 ? variant - 2 : 0;
        Socket caller;
        const auto stream = variant == 1 ? "ordinary-live" : "secure-control";
        const auto secret =
            variant == 2 ? "incorrect-session-auth-fixture" : shared_secret;
        REQUIRE_EQ(connect(caller, listener, stream, variant != 0, 1, secret),
            SRT_ERROR);
        REQUIRE(admission.selected);
        if (variant == 4)
            REQUIRE(admission.closed_setter_rejected);
        empty_accept_queue(listener);
        REQUIRE(!auth(listener.socket.id));
        admission.action = 0;
        Socket recovered;
        REQUIRE_EQ(connect(recovered, listener, "secure-control", true, 1), 0);
        const auto child = srt_accept(listener.socket.id, nullptr, nullptr);
        REQUIRE(child != SRT_INVALID_SOCK);
        expect_runtime_auth(child, true);
        exchange(recovered.id, child);
        REQUIRE_EQ(srt_close(child), 0);
    }
}

TEST(
    srt_compat_session_auth_admission_can_select_clear_child_without_mutating_listener)
{
    Runtime runtime;
    Admission admission;
    Listener listener(admission, true, 1);
    Socket caller;
    REQUIRE_EQ(connect(caller, listener, "clear-file", false, 1, ""), 0);
    const auto child = srt_accept(listener.socket.id, nullptr, nullptr);
    REQUIRE(child != SRT_INVALID_SOCK);
    REQUIRE(!auth(child));
    REQUIRE(!flag(child, SRTO_ENFORCEDENCRYPTION));
    REQUIRE(flag(listener.socket.id, SRTO_ENFORCEDENCRYPTION));
    exchange(caller.id, child);
    REQUIRE_EQ(srt_close(child), 0);
    REQUIRE(auth(listener.socket.id));
    Socket secure;
    REQUIRE_EQ(connect(secure, listener, "secure-sensor", true, 1), 0);
    const auto protected_child =
        srt_accept(listener.socket.id, nullptr, nullptr);
    REQUIRE(protected_child != SRT_INVALID_SOCK);
    expect_runtime_auth(protected_child, true);
    REQUIRE_EQ(srt_close(protected_child), 0);
}

TEST(srt_compat_session_auth_admission_options_remain_scoped_and_atomic)
{
    Runtime runtime;
    Socket socket;
    const auto record = SocketRegistry::instance().find(socket.id);
    REQUIRE(record);
    {
        std::lock_guard lock(record->mutex);
        record->state = SRTS_CONNECTING;
    }
    for (auto option : {SRTO_ROBOTWEAX_SESSIONAUTH, SRTO_ENFORCEDENCRYPTION}) {
        REQUIRE_EQ(set(socket.id, option, true), SRT_ERROR);
        REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ECONNSOCK);
    }
    {
        std::lock_guard lock(record->mutex);
        record->listen_callback_active = true;
    }
    REQUIRE_EQ(set(socket.id, SRTO_ROBOTWEAX_SESSIONAUTH, true), 0);
    REQUIRE_EQ(set(socket.id, SRTO_ENFORCEDENCRYPTION, false), SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);
    REQUIRE(auth(socket.id));
    const unsigned char invalid = 2;
    REQUIRE_EQ(srt_setsockflag(socket.id, SRTO_ROBOTWEAX_SESSIONAUTH, &invalid,
                   sizeof(invalid)),
        SRT_ERROR);
    REQUIRE_EQ(srt_setsockflag(socket.id, SRTO_ROBOTWEAX_SESSIONAUTH, nullptr,
                   sizeof(bool)),
        SRT_ERROR);
    REQUIRE(auth(socket.id));
    REQUIRE_EQ(set(socket.id, SRTO_ROBOTWEAX_SESSIONAUTH, false), 0);
    REQUIRE_EQ(set(socket.id, SRTO_ENFORCEDENCRYPTION, false), 0);
    REQUIRE_EQ(set(socket.id, SRTO_ROBOTWEAX_SESSIONAUTH, true), SRT_ERROR);
    REQUIRE(!auth(socket.id));
    {
        std::lock_guard lock(record->mutex);
        record->listen_callback_active = false;
    }
}

TEST(srt_compat_session_auth_admission_can_enable_encryption_after_callback)
{
    Runtime runtime;
    Admission admission;
    admission.supply_secret = true;
    Listener listener(admission, false, 1, "", false);
    Socket caller;
    REQUIRE_EQ(connect(caller, listener, "secure-control", true, 1), 0);
    const auto child = srt_accept(listener.socket.id, nullptr, nullptr);
    REQUIRE(child != SRT_INVALID_SOCK);
    expect_runtime_auth(child, true);
    REQUIRE(flag(child, SRTO_ENFORCEDENCRYPTION));
    exchange(caller.id, child);
    REQUIRE_EQ(srt_close(child), 0);
    REQUIRE(!auth(listener.socket.id));
    REQUIRE(!flag(listener.socket.id, SRTO_ENFORCEDENCRYPTION));
    admission.supply_secret = false;
    Socket clear;
    REQUIRE_EQ(connect(clear, listener, "clear-file", false, 1, ""), 0);
    const auto clear_child = srt_accept(listener.socket.id, nullptr, nullptr);
    REQUIRE(clear_child != SRT_INVALID_SOCK);
    exchange(clear.id, clear_child);
    REQUIRE_EQ(srt_close(clear_child), 0);
}
