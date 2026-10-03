#include "test.hpp"
#include "compat/socket_registry.hpp"
#include "srt.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

namespace {
struct Runtime {
    Runtime()
    {
        REQUIRE_EQ(srt_startup(), 0);
    }
    ~Runtime()
    {
        srt_cleanup();
    }
};
struct Socket {
    SRTSOCKET handle = srt_create_socket();
    Socket()
    {
        REQUIRE(handle != SRT_INVALID_SOCK);
    }
    ~Socket()
    {
        srt_close(handle);
    }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
};
template <class T> int set(SRTSOCKET socket, SRT_SOCKOPT option, T value)
{
    return srt_setsockflag(socket, option, &value, sizeof(value));
}
std::int32_t integer(SRTSOCKET socket, SRT_SOCKOPT option)
{
    std::int32_t value = -1;
    int size = sizeof(value);
    REQUIRE_EQ(srt_getsockflag(socket, option, &value, &size), 0);
    return value;
}
bool boolean(SRTSOCKET socket, SRT_SOCKOPT option)
{
    bool value = false;
    int size = sizeof(value);
    REQUIRE_EQ(srt_getsockflag(socket, option, &value, &size), 0);
    return value;
}
auto bundle(SRTSOCKET socket)
{
    return std::array<std::int32_t, 10> {integer(socket, SRTO_TRANSTYPE),
        boolean(socket, SRTO_MESSAGEAPI), boolean(socket, SRTO_TSBPDMODE),
        boolean(socket, SRTO_TLPKTDROP), boolean(socket, SRTO_NAKREPORT),
        integer(socket, SRTO_RETRANSMITALGO), integer(socket, SRTO_RCVLATENCY),
        integer(socket, SRTO_PAYLOADSIZE), integer(socket, SRTO_MSS),
        integer(socket, SRTO_RCVBUF)};
}
void provisional(SRTSOCKET socket, bool active = true,
    SRT_GROUP_TYPE group = SRT_GTYPE_UNDEFINED,
    SRT_SOCKSTATUS state = SRTS_CONNECTING)
{
    const auto record =
        robotweax::srt::compat::SocketRegistry::instance().find(socket);
    REQUIRE(record != nullptr);
    std::lock_guard lock(record->mutex);
    record->state = state;
    record->listen_callback_active = active;
    record->incoming_group_type = group;
}
struct Admission {
    std::atomic<SRT_TRANSTYPE> profile {SRTT_LIVE};
    std::atomic_int action {
        0}; // 0 accept, 1 reject, 2 close in callback, 3 gated close
    std::atomic_int calls {0};
    std::atomic_bool selected {false};
    std::atomic_bool immutable_udp {false};
    std::atomic_bool closed_setter_rejected {false};
    std::atomic<SRTSOCKET> child {SRT_INVALID_SOCK};
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool release = false;
};
int select_profile(
    void* opaque, SRTSOCKET child, int, const sockaddr*, const char*) noexcept
{
    auto& admission = *static_cast<Admission*>(opaque);
    ++admission.calls;
    if (admission.action == 3) {
        std::lock_guard lock(admission.mutex);
        // A retransmitted conclusion may start a fresh admission after the
        // first provisional socket is closed. Reject these retry children;
        // keep the first child and its close observation for this test.
        if (admission.entered)
            return -1;
    }
    admission.child = child;
    admission.immutable_udp =
        set(child, SRTO_MSS, std::int32_t {1400}) == SRT_ERROR;
    admission.selected =
        set(child, SRTO_TRANSTYPE, admission.profile.load()) == 0;
    if (!admission.selected)
        return -1;
    if (admission.action == 3) {
        std::unique_lock lock(admission.mutex);
        admission.entered = true;
        admission.changed.notify_all();
        if (!admission.changed.wait_for(lock, std::chrono::seconds {5}, [&] {
                return admission.release;
            }))
            return -1;
        admission.closed_setter_rejected =
            set(child, SRTO_TRANSTYPE, SRTT_LIVE) == SRT_ERROR;
    }
    if (admission.action == 2) {
        srt_close(child);
        admission.closed_setter_rejected =
            set(child, SRTO_TRANSTYPE, SRTT_LIVE) == SRT_ERROR;
    }
    return admission.action == 1 ? -1 : 0;
}
struct Listener {
    Socket socket;
    sockaddr_in address {};
    explicit Listener(Admission& admission)
    {
        REQUIRE_EQ(set(socket.handle, SRTO_CONNTIMEO, std::int32_t {1000}), 0);
        REQUIRE_EQ(set(socket.handle, SRTO_RCVTIMEO, std::int32_t {2000}), 0);
        REQUIRE_EQ(set(socket.handle, SRTO_SNDBUF, std::int32_t {65536}), 0);
        REQUIRE_EQ(set(socket.handle, SRTO_RCVBUF, std::int32_t {65536}), 0);
        REQUIRE_EQ(
            srt_listen_callback(socket.handle, select_profile, &admission), 0);
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        REQUIRE_EQ(
            srt_bind(socket.handle, reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)),
            0);
        REQUIRE_EQ(srt_listen(socket.handle, 4), 0);
        int size = sizeof(address);
        REQUIRE_EQ(srt_getsockname(socket.handle,
                       reinterpret_cast<sockaddr*>(&address), &size),
            0);
    }
};
void prepare(Socket& caller, SRT_TRANSTYPE profile)
{
    REQUIRE_EQ(set(caller.handle, SRTO_TRANSTYPE, profile), 0);
    REQUIRE_EQ(set(caller.handle, SRTO_CONNTIMEO, std::int32_t {1000}), 0);
    REQUIRE_EQ(set(caller.handle, SRTO_SNDBUF, std::int32_t {65536}), 0);
    REQUIRE_EQ(set(caller.handle, SRTO_RCVBUF, std::int32_t {65536}), 0);
}
int connect(Socket& caller, const Listener& listener, SRT_TRANSTYPE profile)
{
    prepare(caller, profile);
    return srt_connect(caller.handle,
        reinterpret_cast<const sockaddr*>(&listener.address),
        sizeof(listener.address));
}
void empty_accept_queue(const Listener& listener)
{
    REQUIRE_EQ(set(listener.socket.handle, SRTO_RCVSYN, false), 0);
    REQUIRE_EQ(
        srt_accept(listener.socket.handle, nullptr, nullptr), SRT_INVALID_SOCK);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EASYNCRCV);
    REQUIRE_EQ(set(listener.socket.handle, SRTO_RCVSYN, true), 0);
}
}

TEST(srt_compat_profile_admission_is_scoped_and_invalid_updates_are_atomic)
{
    Runtime runtime;
    Socket socket;
    provisional(socket.handle);
    for (const auto profile :
        {SRTT_LIVE, SRTT_FILE, SRTT_SENSOR, SRTT_CONTROL}) {
        REQUIRE_EQ(set(socket.handle, SRTO_TRANSTYPE, profile), 0);
        const auto before = bundle(socket.handle);
        REQUIRE_EQ(before[0], profile);
        REQUIRE_EQ(
            set(socket.handle, SRTO_TRANSTYPE, std::int32_t {999}), SRT_ERROR);
        REQUIRE_EQ(bundle(socket.handle), before);
        REQUIRE_EQ(srt_setsockflag(socket.handle, SRTO_TRANSTYPE, nullptr, 4),
            SRT_ERROR);
        REQUIRE_EQ(bundle(socket.handle), before);
        REQUIRE_EQ(srt_setsockflag(socket.handle, SRTO_TRANSTYPE, &profile, 1),
            SRT_ERROR);
        REQUIRE_EQ(bundle(socket.handle), before);
        REQUIRE_EQ(
            set(socket.handle, SRTO_MSS, std::int32_t {1400}), SRT_ERROR);
        REQUIRE_EQ(bundle(socket.handle), before);
    }
    for (const auto state :
        {SRTS_OPENED, SRTS_LISTENING, SRTS_CONNECTED, SRTS_CLOSING}) {
        provisional(socket.handle, true, SRT_GTYPE_UNDEFINED, state);
        REQUIRE_EQ(set(socket.handle, SRTO_TRANSTYPE, SRTT_FILE), SRT_ERROR);
    }
    provisional(socket.handle, false);
    REQUIRE_EQ(set(socket.handle, SRTO_TRANSTYPE, SRTT_FILE), SRT_ERROR);
    for (const auto group : {SRT_GTYPE_BROADCAST, SRT_GTYPE_BACKUP}) {
        provisional(socket.handle, true, group);
        REQUIRE_EQ(set(socket.handle, SRTO_TRANSTYPE, SRTT_FILE), SRT_ERROR);
    }
    provisional(socket.handle, false);
}

TEST(srt_compat_profile_admission_routes_all_profiles_without_mutating_listener)
{
    Runtime runtime;
    Admission admission;
    Listener listener(admission);
    const auto listener_bundle = bundle(listener.socket.handle);
    for (const auto profile :
        {SRTT_FILE, SRTT_SENSOR, SRTT_CONTROL, SRTT_LIVE}) {
        admission.profile = profile;
        Socket caller;
        REQUIRE_EQ(connect(caller, listener, profile), 0);
        const auto child = srt_accept(listener.socket.handle, nullptr, nullptr);
        REQUIRE(child != SRT_INVALID_SOCK);
        const auto child_profile = integer(child, SRTO_TRANSTYPE);
        const auto setter = set(child, SRTO_TRANSTYPE, SRTT_FILE);
        REQUIRE_EQ(srt_close(child), 0);
        REQUIRE_EQ(child_profile, profile);
        REQUIRE_EQ(setter, SRT_ERROR);
        REQUIRE(admission.selected && admission.immutable_udp);
        REQUIRE_EQ(bundle(listener.socket.handle), listener_bundle);
    }
    REQUIRE_EQ(admission.calls, 4);
}

TEST(srt_compat_profile_admission_rejection_close_and_mismatch_allow_recovery)
{
    Runtime runtime;
    Admission admission;
    Listener listener(admission);
    for (const int action : {1, 2, 0}) {
        admission.profile = action == 0 ? SRTT_CONTROL : SRTT_FILE;
        admission.action = action;
        Socket caller;
        // File versus Control deliberately disagrees on congestion/profile.
        REQUIRE_EQ(connect(caller, listener, SRTT_FILE), SRT_ERROR);
        empty_accept_queue(listener);
        REQUIRE(admission.selected);
        if (action == 2)
            REQUIRE(admission.closed_setter_rejected);
    }
    admission.profile = SRTT_FILE;
    admission.action = 0;
    Socket caller;
    REQUIRE_EQ(connect(caller, listener, SRTT_FILE), 0);
    const auto child = srt_accept(listener.socket.handle, nullptr, nullptr);
    REQUIRE(child != SRT_INVALID_SOCK);
    REQUIRE_EQ(srt_close(child), 0);
    // Closing the child cannot send a final rejection response; the caller
    // may retry its conclusion before its deadline. Each retry is admitted
    // separately, so do not equate callback count with logical caller count.
    REQUIRE(admission.calls.load() >= 4);
    REQUIRE_EQ(integer(listener.socket.handle, SRTO_TRANSTYPE), SRTT_LIVE);
}

TEST(srt_compat_profile_admission_external_close_during_callback_is_terminal)
{
    Runtime runtime;
    Admission admission;
    admission.profile = SRTT_FILE;
    admission.action = 3;
    Listener listener(admission);
    Socket caller;
    prepare(caller, SRTT_FILE);
    int result = 0;
    std::jthread worker([&] {
        result = srt_connect(caller.handle,
            reinterpret_cast<const sockaddr*>(&listener.address),
            sizeof(listener.address));
    });
    bool entered = false;
    {
        std::unique_lock lock(admission.mutex);
        entered =
            admission.changed.wait_for(lock, std::chrono::seconds {5}, [&] {
                return admission.entered;
            });
    }
    const int closed = entered ? srt_close(admission.child) : SRT_ERROR;
    {
        std::lock_guard lock(admission.mutex);
        admission.release = true;
    }
    admission.changed.notify_all();
    worker.join();
    REQUIRE(entered);
    REQUIRE_EQ(closed, 0);
    REQUIRE_EQ(result, SRT_ERROR);
    REQUIRE(admission.closed_setter_rejected);
    empty_accept_queue(listener);
    admission.action = 0;
    Socket recovered;
    REQUIRE_EQ(connect(recovered, listener, SRTT_FILE), 0);
    const auto child = srt_accept(listener.socket.handle, nullptr, nullptr);
    REQUIRE(child != SRT_INVALID_SOCK);
    REQUIRE_EQ(srt_close(child), 0);
}

#ifdef ENABLE_AEAD_API_PREVIEW
TEST(srt_compat_profile_admission_preserves_gcm_constraints_atomically)
{
    Runtime runtime;
    Socket socket;
    REQUIRE_EQ(set(socket.handle, SRTO_CRYPTOMODE, std::int32_t {2}), 0);
    provisional(socket.handle);
    const auto before = bundle(socket.handle);
    for (const auto profile : {SRTT_SENSOR, SRTT_CONTROL}) {
        REQUIRE_EQ(set(socket.handle, SRTO_TRANSTYPE, profile), SRT_ERROR);
        REQUIRE_EQ(bundle(socket.handle), before);
        REQUIRE_EQ(integer(socket.handle, SRTO_CRYPTOMODE), 2);
    }
    REQUIRE_EQ(set(socket.handle, SRTO_TRANSTYPE, SRTT_FILE), 0);
    REQUIRE_EQ(integer(socket.handle, SRTO_TRANSTYPE), SRTT_FILE);
    REQUIRE_EQ(integer(socket.handle, SRTO_CRYPTOMODE), 2);
    provisional(socket.handle, false);
}
#endif
