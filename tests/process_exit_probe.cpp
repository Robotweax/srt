#include "compat/runtime_scheduler_service.hpp"
#include "compat/runtime_work_executor_service.hpp"
#include "compat/socket_registry.hpp"
#include "robotweax/srt/crypto.hpp"
#include "srt/srt.h"

#include <chrono>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>

namespace {
std::weak_ptr<robotweax::srt::compat::SocketRecord> exit_record;
std::weak_ptr<robotweax::srt::compat::RuntimeScheduler> exit_scheduler;
std::weak_ptr<robotweax::srt::compat::RuntimeWorkExecutor> exit_executor;
std::atomic_bool worker_finished = false;

void verify_owner_teardown()
{
    if (!worker_finished.load(std::memory_order_acquire)
        || !exit_record.expired() || !exit_scheduler.expired()
        || !exit_executor.expired()) {
        std::fputs(
            "owner static teardown did not release runtime state\n", stderr);
        std::_Exit(18);
    }
}
} // namespace

int main()
{
    // Registered before runtime construction: the callback runs after its
    // static destructors and verifies that owner teardown was not skipped.
    if (std::atexit(verify_owner_teardown) != 0) {
        return 19;
    }
    if (srt_startup() != 0) {
        return 10;
    }

    const SRTSOCKET socket = srt_create_socket();
    if (socket == SRT_INVALID_SOCK) {
        return 11;
    }
    const auto record =
        robotweax::srt::compat::SocketRegistry::instance().find(socket);
    if (record == nullptr) {
        return 12;
    }
    const auto scheduler = robotweax::srt::compat::acquire_runtime_scheduler();
    if (scheduler == nullptr || !scheduler->snapshot().accepting) {
        return 16;
    }
    const auto work_executor =
        robotweax::srt::compat::acquire_runtime_work_executor();
    if (work_executor == nullptr || !work_executor->snapshot().accepting) {
        return 17;
    }
    exit_record = record;
    exit_scheduler = scheduler;
    exit_executor = work_executor;

    auto crypto = std::make_shared<robotweax::srt::CryptoSession>(
        robotweax::srt::CryptoConfiguration{
            .passphrase = "process-exit-secret",
            .key_length = 16,
            .refresh_rate_packets = 32,
            .preannouncement_packets = 8,
        });
    if (crypto->start_initiator() != robotweax::srt::Error::none) {
        return 13;
    }

    {
        std::lock_guard lock(record->mutex);
        record->crypto = std::move(crypto);
        record->connect_worker = std::thread([] {
            std::this_thread::sleep_for(
                std::chrono::milliseconds{50});
            worker_finished.store(true, std::memory_order_release);
        });
    }

    const int poll = srt_epoll_create();
    if (poll < 0) {
        return 14;
    }
    const int events = SRT_EPOLL_IN | SRT_EPOLL_ERR;
    if (srt_epoll_add_usock(poll, socket, &events) != 0) {
        return 15;
    }

    // Intentionally return without closing the socket or poll and without
    // calling srt_cleanup(). Static runtime teardown must join the worker and
    // release process-wide compatibility state without std::terminate.
    return 0;
}
