#include "compat/connect_callback_executor.hpp"
#include "srt/srt.h"
#include "test.hpp"

#include <atomic>
#include <chrono>
#include <thread>
#if defined(_WIN32)
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

namespace {
struct CallbackClose {
    sockaddr_in peer {};
    std::atomic_int child_callbacks = 0;
    std::atomic_int error = 0;
    std::atomic_bool finished = false;
};
void child_callback(void* opaque, SRTSOCKET, int, const sockaddr*, int)
{
    ++static_cast<CallbackClose*>(opaque)->child_callbacks;
}
void parent_callback(
    void* opaque, SRTSOCKET self, int error, const sockaddr*, int)
{
    auto& state = *static_cast<CallbackClose*>(opaque);
    const auto child = srt_create_socket();
    const bool asynchronous = false;
    if (error != 0 || child == SRT_INVALID_SOCK
        || srt_setsockflag(
               child, SRTO_RCVSYN, &asynchronous, sizeof(asynchronous))
            != 0
        || srt_connect_callback(child, child_callback, opaque) != 0
        || srt_connect(child, reinterpret_cast<const sockaddr*>(&state.peer),
               sizeof(state.peer))
            != 0) {
        state.error = 1;
    }
    // Closing a pending child must deliver its cancellation callback on
    // another worker before returning, even though this callback is active.
    if (child != SRT_INVALID_SOCK && srt_close(child) != 0)
        state.error = 2;
    if (state.child_callbacks.load() != 1)
        state.error = 3;
    if (srt_close(self) != 0)
        state.error = 4;
    state.finished = true;
}
bool wait_finished(const std::atomic_bool& flag)
{
    const auto end =
        std::chrono::steady_clock::now() + std::chrono::seconds {5};
    while (!flag.load() && std::chrono::steady_clock::now() < end)
        std::this_thread::yield();
    return flag.load();
}
} // namespace

TEST(connect_callback_lifecycle_cross_socket_close_and_self_close)
{
    REQUIRE_EQ(srt_startup(), 0);
    CallbackClose observation;
    observation.peer.sin_family = AF_INET;
    observation.peer.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const auto listener = srt_create_socket();
    REQUIRE(listener != SRT_INVALID_SOCK);
    REQUIRE_EQ(
        srt_bind(listener, reinterpret_cast<const sockaddr*>(&observation.peer),
            sizeof(observation.peer)),
        0);
    REQUIRE_EQ(srt_listen(listener, 8), 0);
    int size = sizeof(observation.peer);
    REQUIRE_EQ(srt_getsockname(listener,
                   reinterpret_cast<sockaddr*>(&observation.peer), &size),
        0);
    const auto caller = srt_create_socket();
    const bool asynchronous = false;
    REQUIRE_EQ(srt_setsockflag(
                   caller, SRTO_RCVSYN, &asynchronous, sizeof(asynchronous)),
        0);
    REQUIRE_EQ(srt_connect_callback(caller, parent_callback, &observation), 0);
    REQUIRE_EQ(srt_connect(caller,
                   reinterpret_cast<const sockaddr*>(&observation.peer),
                   sizeof(observation.peer)),
        0);
    REQUIRE(wait_finished(observation.finished));
    REQUIRE_EQ(observation.error.load(), 0);
    REQUIRE_EQ(srt_getsockstate(caller), SRTS_CLOSED);
    REQUIRE_EQ(srt_close(listener), 0);
    REQUIRE_EQ(srt_cleanup(), 0);
    // A final cleanup must permit a fresh executor generation.
    REQUIRE_EQ(srt_startup(), 0);
    const auto fresh =
        robotweax::srt::compat::acquire_connect_callback_executor();
    REQUIRE(!fresh->snapshot().stopping);
    REQUIRE_EQ(fresh->snapshot().created, 0U);
    REQUIRE_EQ(srt_cleanup(), 0);
}

namespace {
struct ChurnCallback {
    std::atomic_uint calls = 0;
    std::atomic_bool valid = true;
};
void observe_churn(
    void* opaque, SRTSOCKET socket, int error, const sockaddr*, int)
{
    auto& observation = *static_cast<ChurnCallback*>(opaque);
    if (error != 0 || srt_getsockstate(socket) != SRTS_CONNECTED)
        observation.valid = false;
    ++observation.calls;
}
} // namespace

TEST(connect_callback_lifecycle_reuses_worker_across_new_sockets)
{
    REQUIRE_EQ(srt_startup(), 0);
    auto executor = robotweax::srt::compat::acquire_connect_callback_executor();
    REQUIRE_EQ(executor->snapshot().created, 0U);
    sockaddr_in peer {};
    peer.sin_family = AF_INET;
    peer.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const auto listener = srt_create_socket();
    REQUIRE_EQ(srt_bind(listener, reinterpret_cast<const sockaddr*>(&peer),
                   sizeof(peer)),
        0);
    REQUIRE_EQ(srt_listen(listener, 1), 0);
    int size = sizeof(peer);
    REQUIRE_EQ(
        srt_getsockname(listener, reinterpret_cast<sockaddr*>(&peer), &size),
        0);
    ChurnCallback observation;
    for (unsigned i = 0; i < 32; ++i) {
        const auto caller = srt_create_socket();
        const bool asynchronous = false;
        REQUIRE_EQ(srt_setsockflag(caller, SRTO_RCVSYN, &asynchronous,
                       sizeof(asynchronous)),
            0);
        REQUIRE_EQ(
            srt_connect_callback(caller, observe_churn, &observation), 0);
        REQUIRE_EQ(srt_connect(caller, reinterpret_cast<const sockaddr*>(&peer),
                       sizeof(peer)),
            0);
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds {5};
        while (executor->snapshot().completed != i + 1
            && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        REQUIRE_EQ(executor->snapshot().completed, i + 1);
        REQUIRE_EQ(observation.calls.load(), i + 1);
        REQUIRE(observation.valid.load());
        const auto accepted = srt_accept(listener, nullptr, nullptr);
        REQUIRE(accepted != SRT_INVALID_SOCK);
        REQUIRE_EQ(srt_close(caller), 0);
        REQUIRE_EQ(srt_close(accepted), 0);
    }
    REQUIRE_EQ(executor->snapshot().created, 1U);
    REQUIRE_EQ(executor->snapshot().reused, 31U);
    REQUIRE_EQ(srt_close(listener), 0);
    REQUIRE_EQ(srt_cleanup(), 0);
    REQUIRE(executor->snapshot().stopping);
    REQUIRE_EQ(executor->snapshot().workers, 0U);
}

namespace {
struct TlsCleanupObservation {
    std::atomic_bool entered = false;
    std::atomic_bool finished = false;
    std::atomic_bool valid = false;
};
struct CallbackThreadLocal {
    std::shared_ptr<TlsCleanupObservation> observation;
    ~CallbackThreadLocal()
    {
        if (!observation)
            return;
        observation->entered = true;
        const bool started = srt_startup() == 0;
        const auto socket = srt_create_socket();
        const bool closed = srt_close(socket) == 0;
        const bool cleaned = srt_cleanup() == 0;
        observation->valid =
            started && socket != SRT_INVALID_SOCK && closed && cleaned;
        observation->finished = true;
    }
};
void initialize_callback_thread_local(void* context, int) noexcept
{
    thread_local CallbackThreadLocal local;
    local.observation =
        *static_cast<std::shared_ptr<TlsCleanupObservation>*>(context);
}
} // namespace

TEST(connect_callback_lifecycle_thread_local_destructor_can_restart_runtime)
{
    REQUIRE_EQ(srt_startup(), 0);
    const auto executor =
        robotweax::srt::compat::acquire_connect_callback_executor();
    auto observation = std::make_shared<TlsCleanupObservation>();
    auto context =
        std::make_shared<std::shared_ptr<TlsCleanupObservation>>(observation);
    REQUIRE(executor->submit({initialize_callback_thread_local, context, 0}));
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds {3};
    while (executor->snapshot().completed != 1
        && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    REQUIRE_EQ(executor->snapshot().completed, 1U);
    REQUIRE_EQ(executor->snapshot().idle, 1U);
    REQUIRE(!observation->entered.load());
    // Final cleanup must not join this idle worker while holding the
    // runtime-generation mutex needed by application TLS destructors.
    REQUIRE_EQ(srt_cleanup(), 0);
    REQUIRE(observation->finished.load());
    REQUIRE(observation->valid.load());
    REQUIRE_EQ(executor->snapshot().workers, 0U);
}
