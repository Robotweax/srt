/* SPDX-License-Identifier: MIT */
#include "test.hpp"

#include "compat/connect_callback_executor.hpp"
#include "compat/runtime_scheduler.hpp"
#include "compat/runtime_work_executor.hpp"
#include "compat/socket_readiness.hpp"
#include "compat/thread_name.hpp"
#include "robotweax/srt/udp.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

using namespace robotweax::srt;
using namespace robotweax::srt::compat;

#if defined(__linux__) || defined(__APPLE__)
namespace {
struct NameRecord {
    std::mutex mutex;
    std::condition_variable changed;
    std::array<char, 64> name {};
    int error = -1;
    bool done = false;
};

void record_name(void* context) noexcept
{
    auto& record = *static_cast<NameRecord*>(context);
    std::lock_guard lock(record.mutex);
    record.error = pthread_getname_np(
        pthread_self(), record.name.data(), record.name.size());
    record.done = true;
    record.changed.notify_all();
}

void expect_name(
    const std::shared_ptr<NameRecord>& record, const std::string& expected)
{
    std::unique_lock lock(record->mutex);
    REQUIRE(record->changed.wait_for(lock, std::chrono::seconds {2}, [&] {
        return record->done;
    }));
    REQUIRE_EQ(record->error, 0);
    REQUIRE_EQ(std::string(record->name.data()), expected);
    REQUIRE(expected.size() <= 15U);
}
} // namespace

TEST(thread_names_scheduler_jobs_and_timers_expose_eight_distinct_shards)
{
    // Read names inside actual dispatched work, without racing thread startup.
    RuntimeScheduler scheduler({8, 1, 1});
    REQUIRE(scheduler.start());
    std::array<std::shared_ptr<NameRecord>, 8> jobs;
    std::array<std::shared_ptr<NameRecord>, 8> timers;
    for (std::size_t index = 0; index < jobs.size(); ++index) {
        jobs[index] = std::make_shared<NameRecord>();
        timers[index] = std::make_shared<NameRecord>();
        REQUIRE_EQ(scheduler.submit(index, {record_name, jobs[index]}),
            RuntimeScheduler::SubmitStatus::accepted);
        REQUIRE_EQ(scheduler
                       .schedule_at(index, std::chrono::steady_clock::now(),
                           {record_name, timers[index]})
                       .status,
            RuntimeScheduler::SubmitStatus::accepted);
    }
    for (std::size_t index = 0; index < jobs.size(); ++index) {
        const auto expected = "rwx-shard-" + std::to_string(index);
        expect_name(jobs[index], expected);
        expect_name(timers[index], expected);
    }
}

TEST(thread_names_work_callback_and_readiness_dispatch_have_role_names)
{
    const auto work = std::make_shared<NameRecord>();
    RuntimeWorkExecutor executor({1, 1});
    REQUIRE(executor.start());
    REQUIRE_EQ(executor.submit({record_name, work}),
        RuntimeWorkExecutor::SubmitStatus::accepted);
    expect_name(work, "rwx-worker-0");

    const auto callback = std::make_shared<NameRecord>();
    ConnectCallbackExecutor callbacks(1);
    REQUIRE(callbacks.submit({[](void* context, int) noexcept {
                                  record_name(context);
                              },
        callback, 0}));
    expect_name(callback, "rwx-callback");

    UdpSocket target;
    UdpSocket source;
    REQUIRE_EQ(target.bind(IpEndpoint::loopback()), Error::none);
    const auto address = target.local_endpoint();
    REQUIRE(address);
    SocketReadiness readiness(1);
    REQUIRE(readiness.start());
    const auto ready = std::make_shared<NameRecord>();
    const auto token =
        readiness.watch(target.native_handle(), {record_name, ready});
    REQUIRE(token.valid());
    REQUIRE(readiness.arm(token));
    REQUIRE(source.send_to(std::array {std::byte {1}}, address.endpoint));
    expect_name(ready, "rwx-readiness");
}

TEST(thread_names_fit_native_limit_preserve_errno_and_leave_caller_unchanged)
{
    NameRecord caller_before;
    record_name(&caller_before);
    auto record = std::make_shared<NameRecord>();
    int preserved_errno = 0;
    // Rename only a disposable library-style worker, never the test runner.
    std::thread worker([&] {
        errno = EDOM;
        name_current_thread("123456789012345");
        preserved_errno = errno;
        record_name(record.get());
    });
    worker.join();
    REQUIRE_EQ(preserved_errno, EDOM);
    expect_name(record, "123456789012345");

    auto indexed = std::make_shared<NameRecord>();
    std::thread maximum_shard([&] {
        name_current_thread("rwx-shard-", 63);
        // An unrepresentable suffix must not truncate into a different index.
        name_current_thread(
            "rwx-shard-", std::numeric_limits<std::size_t>::max());
        record_name(indexed.get());
    });
    maximum_shard.join();
    expect_name(indexed, "rwx-shard-63");
    NameRecord caller_after;
    record_name(&caller_after);
    REQUIRE_EQ(caller_before.error, 0);
    REQUIRE_EQ(caller_after.error, 0);
    REQUIRE_EQ(caller_before.name, caller_after.name);
}
#else
TEST(thread_names_are_optional_on_other_platforms)
{
    SKIP_UNLESS(false, "native thread naming is enabled on Linux/macOS only");
}
#endif
