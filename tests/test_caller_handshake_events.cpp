#include "test.hpp"

#include "compat/caller_handshake_events.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>

using namespace robotweax::srt;
using namespace robotweax::srt::compat;

namespace {

struct SendTrace {
    std::mutex mutex;
    std::array<HandshakeAction, 8> actions {};
    std::array<std::uint32_t, 8> destinations {};
    std::array<std::thread::id, 8> threads {};
    std::size_t size = 0;
};

[[nodiscard]] CallerHandshakeSendResult record_send(void* context,
    const HandshakeAction& action, std::uint32_t destination_socket_id) noexcept
{
    auto& trace = *static_cast<SendTrace*>(context);
    std::lock_guard lock(trace.mutex);
    if (trace.size == trace.actions.size()) {
        return {.status = CallerHandshakeSendStatus::failed};
    }
    trace.actions[trace.size] = action;
    trace.destinations[trace.size] = destination_socket_id;
    trace.threads[trace.size] = std::this_thread::get_id();
    ++trace.size;
    return {};
}

} // namespace

TEST(caller_handshake_event_driver_requires_explicit_ordered_events)
{
    SendTrace trace;
    CallerHandshakeEventDriver driver {{
                                           .role = ConnectionRole::caller,
                                           .local_socket_id = 100U,
                                           .timeout_milliseconds = 37U,
                                           .maximum_retries = 1U,
                                       },
        record_send, &trace};

    REQUIRE_EQ(driver
                   .dispatch({
                       .kind = CallerHandshakeEventKind::retry_timer,
                   })
                   .outcome,
        CallerHandshakeEventOutcome::invalid);

    const CallerHandshakeEventResult started = driver.dispatch({
        .kind = CallerHandshakeEventKind::start,
    });
    REQUIRE_EQ(started.outcome, CallerHandshakeEventOutcome::running);
    REQUIRE(started.arm_retry_timer);
    REQUIRE_EQ(started.retry_timeout_milliseconds, 37U);
    REQUIRE_EQ(trace.size, 1U);

    const CallerHandshakeEventResult key_material = driver.dispatch({
        .kind = CallerHandshakeEventKind::key_material,
    });
    REQUIRE_EQ(key_material.outcome, CallerHandshakeEventOutcome::running);
    REQUIRE_EQ(key_material.error, Error::none);

    const CallerHandshakeEventResult retry = driver.dispatch({
        .kind = CallerHandshakeEventKind::retry_timer,
    });
    REQUIRE_EQ(retry.outcome, CallerHandshakeEventOutcome::running);
    REQUIRE(retry.arm_retry_timer);
    REQUIRE_EQ(trace.size, 2U);

    const CallerHandshakeEventResult timed_out = driver.dispatch({
        .kind = CallerHandshakeEventKind::overall_timeout,
    });
    REQUIRE_EQ(timed_out.outcome, CallerHandshakeEventOutcome::timed_out);
    REQUIRE_EQ(driver
                   .dispatch({
                       .kind = CallerHandshakeEventKind::close,
                   })
                   .outcome,
        CallerHandshakeEventOutcome::invalid);
}

TEST(caller_handshake_retry_exhaustion_is_a_timeout)
{
    SendTrace trace;
    CallerHandshakeEventDriver driver {{
                                           .role = ConnectionRole::caller,
                                           .local_socket_id = 100U,
                                           .maximum_retries = 0U,
                                       },
        record_send, &trace};

    REQUIRE_EQ(driver
                   .dispatch({
                       .kind = CallerHandshakeEventKind::start,
                   })
                   .outcome,
        CallerHandshakeEventOutcome::running);
    REQUIRE_EQ(driver
                   .dispatch({
                       .kind = CallerHandshakeEventKind::retry_timer,
                   })
                   .outcome,
        CallerHandshakeEventOutcome::timed_out);
}

TEST(caller_handshake_invalid_peer_progression_is_a_setup_failure)
{
    SendTrace trace;
    CallerHandshakeEventDriver driver {{
                                           .role = ConnectionRole::caller,
                                           .local_socket_id = 100U,
                                       },
        record_send, &trace};

    REQUIRE_EQ(driver
                   .dispatch({
                       .kind = CallerHandshakeEventKind::start,
                   })
                   .outcome,
        CallerHandshakeEventOutcome::running);
    REQUIRE_EQ(driver
                   .dispatch({
                       .kind = CallerHandshakeEventKind::inbox_ready,
                       .message = {},
                   })
                   .outcome,
        CallerHandshakeEventOutcome::failed);
}
