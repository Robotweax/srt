#include "test.hpp"

#include "compat/group_multicast_emitter.hpp"
#include "compat/transport_runtime.hpp"
#include "robotweax/srt/codec.hpp"
#include "robotweax/srt/connection_group.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <vector>

using namespace robotweax::srt;
using namespace robotweax::srt::compat;

namespace {
using Emitter = GroupMulticastEmitter;
using Admission = Emitter::Admission;
constexpr std::uint32_t group_id = group_handle_mask | 71U;
constexpr std::uint64_t generation = 19U;
using namespace std::chrono_literals;

std::vector<std::byte> packet(
    std::uint32_t sequence, std::byte value = std::byte {'a'})
{
    MutablePacketView view;
    view.kind = PacketKind::data;
    view.data.sequence = SequenceNumber {sequence};
    view.data.message_number = 1;
    view.data.boundary = MessageBoundary::solo;
    view.data.in_order = true;
    view.data.destination_socket_id = group_id;
    view.payload = std::span {&value, 1U};
    std::array<std::byte, 64> bytes {};
    const auto encoded = encode_packet(view, bytes);
    REQUIRE(encoded);
    return {bytes.begin(), bytes.begin() + encoded.bytes_written};
}

struct Capture {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::vector<std::byte>> attempts;
    std::size_t failures = 0;
    bool fatal = false;
    bool partial = false;
    Emitter::TimePoint* clock = nullptr;
    Emitter::Clock::duration tick {};
};
UdpIoResult capture(
    std::span<const std::byte> bytes, IpEndpoint, void* context) noexcept
{
    auto& output = *static_cast<Capture*>(context);
    std::lock_guard lock(output.mutex);
    output.attempts.emplace_back(bytes.begin(), bytes.end());
    output.changed.notify_all();
    if (output.clock)
        *output.clock += output.tick;
    if (output.fatal)
        return {.error = Error::io_error, .system_error = 22};
    if (output.partial)
        return {.bytes_transferred = bytes.size() - 1U};
    if (output.failures != 0U) {
        --output.failures;
        return {.error = Error::would_block};
    }
    return {.bytes_transferred = bytes.size()};
}
Emitter::TimePoint read_clock(void* context) noexcept
{
    return *static_cast<Emitter::TimePoint*>(context);
}
struct Fixture {
    Emitter::TimePoint now {};
    Capture output;
    std::shared_ptr<DatagramChannel> channel =
        std::make_shared<DatagramChannel>();
    Emitter::Configuration config;
    std::shared_ptr<Emitter> emitter;
    Fixture()
    {
        channel->set_send_hook_for_testing(capture, &output);
        config = {.channel = channel,
            .destination = {.address = {239, 255, 133, 11}, .port = 6000},
            .group_id = group_id,
            .generation = generation,
            .initial_sequence = SequenceNumber {1},
            .capacity = 4,
            .wire_bytes_per_second = 1'000'000,
            .clock = read_clock,
            .clock_context = &now};
        emitter = Emitter::create_for_testing(config);
        REQUIRE(emitter);
    }
    void recreate()
    {
        emitter = Emitter::create_for_testing(config);
        REQUIRE(emitter);
    }
    Admission enqueue(std::uint32_t sequence,
        Emitter::TimePoint deadline = Emitter::TimePoint {} + 10s)
    {
        return emitter->enqueue(generation, packet(sequence), deadline);
    }
};

struct Lease {
    std::shared_ptr<std::atomic<unsigned>> released;
    ~Lease()
    {
        ++*released;
    }
};

struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool released = false;
};
void wait_at_gate(void* context) noexcept
{
    auto& gate = *static_cast<Gate*>(context);
    std::unique_lock lock(gate.mutex);
    gate.entered = true;
    gate.changed.notify_all();
    gate.changed.wait(lock, [&] {
        return gate.released;
    });
}
void release_gate(const std::shared_ptr<Gate>& gate)
{
    std::lock_guard lock(gate->mutex);
    gate->released = true;
    gate->changed.notify_all();
}
struct GateRelease {
    std::shared_ptr<Gate> gate;
    ~GateRelease()
    {
        release_gate(gate);
    }
};
void entered(const std::shared_ptr<Gate>& gate)
{
    std::unique_lock lock(gate->mutex);
    REQUIRE(gate->changed.wait_for(lock, 2s, [&] {
        return gate->entered;
    }));
}
void noop(void*) noexcept { }

std::shared_ptr<RuntimeScheduler> scheduler(std::size_t services = 1)
{
    auto result = std::make_shared<RuntimeScheduler>(
        RuntimeScheduler::Configuration {1, 1, 1, services});
    REQUIRE(result->start());
    return result;
}
void await_attempts(Capture& output, std::size_t count)
{
    std::unique_lock lock(output.mutex);
    REQUIRE(output.changed.wait_for(lock, 2s, [&] {
        return output.attempts.size() >= count;
    }));
}
} // namespace

TEST(compat_group_emitter_rejects_invalid_configuration_and_unreserved_service)
{
    Fixture f;
    for (unsigned mutation = 0; mutation < 11; ++mutation) {
        auto c = f.config;
        switch (mutation) {
        case 0:
            c.capacity = 0;
            break;
        case 1:
            c.capacity = Emitter::maximum_capacity + 1;
            break;
        case 2:
            c.batch_size = 0;
            break;
        case 3:
            c.batch_size = Emitter::maximum_batch + 1;
            break;
        case 4:
            c.wire_bytes_per_second = 0;
            break;
        case 5:
            c.generation = 0;
            break;
        case 6:
            c.group_id = 71;
            break;
        case 7:
            c.destination = IpEndpoint::loopback(6000);
            break;
        case 8:
            c.maximum_attempts = 0;
            break;
        case 9:
            c.maximum_transmission_unit = 44;
            break;
        case 10:
            c.maximum_attempts = Emitter::maximum_retry_attempts + 1;
            break;
        }
        REQUIRE(!Emitter::create_for_testing(c));
    }
    REQUIRE(!Emitter::create(f.config, {}));
    auto pool = scheduler(0);
    REQUIRE(!Emitter::create(f.config, pool));
    REQUIRE_EQ(pool->snapshot().services_reserved, 0U);
    pool->stop();
}

TEST(
    compat_group_emitter_admission_is_bounded_transactional_and_generation_checked)
{
    Fixture f;
    f.config.capacity = 1;
    f.recreate();
    auto bytes = packet(1);
    REQUIRE_EQ(f.emitter->enqueue(generation + 1, bytes, f.now + 1s),
        Admission::wrong_generation);
    REQUIRE_EQ(f.emitter->enqueue(generation, packet(2), f.now + 1s),
        Admission::wrong_sequence);
    REQUIRE_EQ(f.enqueue(1, f.now), Admission::expired);
    REQUIRE_EQ(f.emitter->snapshot().accepted_packets, 0U);
    REQUIRE_EQ(f.enqueue(1), Admission::accepted);
    REQUIRE_EQ(f.enqueue(2), Admission::full);
    REQUIRE_EQ(
        f.emitter->snapshot().accepted_next_sequence, SequenceNumber {2});
    REQUIRE_EQ(f.emitter->snapshot().queued_bytes, bytes.size());
    REQUIRE(!f.emitter->snapshot().writable);
    REQUIRE_EQ(f.emitter->readiness_source()->low_epochs().out, 1U);
    f.emitter->pump_for_testing();
    REQUIRE(f.emitter->snapshot().writable);
    REQUIRE_EQ(f.enqueue(2), Admission::accepted);
}

TEST(
    compat_group_emitter_rejects_controls_wrong_destinations_repeats_and_oversized_input)
{
    Fixture f;
    const auto valid = packet(1);
    auto invalid = valid;
    invalid[0] |= std::byte {0x80};
    REQUIRE_EQ(f.emitter->enqueue(generation, invalid, f.now + 1s),
        Admission::invalid_packet);
    invalid = valid;
    invalid[15] ^= std::byte {1};
    REQUIRE_EQ(f.emitter->enqueue(generation, invalid, f.now + 1s),
        Admission::invalid_packet);
    MutablePacketView repeat;
    repeat.kind = PacketKind::data;
    repeat.data.sequence = SequenceNumber {1};
    repeat.data.message_number = 1;
    repeat.data.destination_socket_id = group_id;
    repeat.data.retransmitted = true;
    const std::array payload {std::byte {'r'}};
    repeat.payload = payload;
    std::array<std::byte, 64> bytes {};
    const auto encoded = encode_packet(repeat, bytes);
    REQUIRE(encoded);
    REQUIRE_EQ(f.emitter->enqueue(generation,
                   std::span {bytes}.first(encoded.bytes_written), f.now + 1s),
        Admission::invalid_packet);
    std::array<std::byte, Emitter::maximum_datagram_size + 1> large {};
    REQUIRE_EQ(f.emitter->enqueue(generation, large, f.now + 1s),
        Admission::invalid_packet);
    REQUIRE_EQ(
        f.emitter->snapshot().accepted_next_sequence, SequenceNumber {1});
}

TEST(
    compat_group_emitter_preserves_bytes_generation_ownership_and_sequence_wrap)
{
    Fixture f;
    f.config.initial_sequence = SequenceNumber {SequenceNumber::mask};
    f.recreate();
    auto released = std::make_shared<std::atomic<unsigned>>(0);
    auto lease = std::make_shared<Lease>(released);
    std::weak_ptr<Lease> weak = lease;
    auto original = packet(SequenceNumber::mask, std::byte {'k'});
    const auto expected = original;
    REQUIRE_EQ(f.emitter->enqueue(generation, original, f.now + 1s, lease),
        Admission::accepted);
    lease.reset();
    std::fill(original.begin(), original.end(), std::byte {0xff});
    REQUIRE(!weak.expired());
    REQUIRE_EQ(f.enqueue(0), Admission::accepted);
    f.emitter->pump_for_testing();
    REQUIRE(weak.expired());
    REQUIRE_EQ(released->load(), 1U);
    REQUIRE_EQ(f.output.attempts.front(), expected);
    REQUIRE_EQ(f.emitter->snapshot().submitted_packets, 1U);
    REQUIRE_EQ(
        f.emitter->snapshot().accepted_next_sequence, SequenceNumber {1});
    f.now += 45us; // 17-byte DATA + 28 IP/UDP bytes, at 1 MB/s.
    f.emitter->pump_for_testing();
    REQUIRE_EQ(f.emitter->snapshot().last_submitted_sequence,
        std::optional {SequenceNumber {0}});
}

TEST(compat_group_emitter_pacing_charges_wire_overhead_without_catch_up_bursts)
{
    Fixture f;
    REQUIRE_EQ(f.enqueue(1), Admission::accepted);
    REQUIRE_EQ(f.enqueue(2), Admission::accepted);
    REQUIRE_EQ(f.enqueue(3), Admission::accepted);
    f.emitter->pump_for_testing();
    REQUIRE_EQ(*f.emitter->snapshot().next_wakeup, f.now + 45us);
    f.now += 44us;
    f.emitter->pump_for_testing();
    REQUIRE_EQ(f.emitter->snapshot().submitted_packets, 1U);
    f.now += 1s;
    f.emitter->pump_for_testing();
    REQUIRE_EQ(f.emitter->snapshot().submitted_packets, 2U);
    REQUIRE_EQ(*f.emitter->snapshot().next_wakeup, f.now + 45us);
}

TEST(compat_group_emitter_expiry_preempts_the_pacing_deadline)
{
    Fixture f;
    f.config.wire_bytes_per_second = 450;
    f.recreate();
    REQUIRE_EQ(f.enqueue(1), Admission::accepted);
    REQUIRE_EQ(f.enqueue(2, f.now + 1ms), Admission::accepted);
    f.emitter->pump_for_testing();
    REQUIRE_EQ(*f.emitter->snapshot().next_wakeup, f.now + 1ms);
    f.now += 1ms;
    f.emitter->pump_for_testing();
    REQUIRE_EQ(f.emitter->snapshot().expired_packets, 1U);
    REQUIRE_EQ(f.emitter->snapshot().submitted_packets, 1U);
    REQUIRE_EQ(
        f.emitter->snapshot().accepted_next_sequence, SequenceNumber {3});
}

TEST(
    compat_group_emitter_retries_identical_bytes_and_keeps_emission_separate_from_acceptance)
{
    Fixture f;
    f.output.failures = 2;
    REQUIRE_EQ(f.enqueue(1), Admission::accepted);
    f.emitter->pump_for_testing();
    REQUIRE_EQ(f.emitter->snapshot().submitted_packets, 0U);
    REQUIRE(!f.emitter->snapshot().last_submitted_sequence.has_value());
    REQUIRE_EQ(f.emitter->snapshot().queued, 1U);
    f.emitter->pump_for_testing();
    REQUIRE_EQ(f.output.attempts.size(), 1U);
    f.now += 1ms;
    f.emitter->pump_for_testing();
    f.now += 1ms;
    f.emitter->pump_for_testing();
    REQUIRE_EQ(f.emitter->snapshot().submitted_packets, 1U);
    REQUIRE_EQ(f.output.attempts.size(), 3U);
    REQUIRE_EQ(f.output.attempts[0], f.output.attempts[1]);
    REQUIRE_EQ(f.output.attempts[1], f.output.attempts[2]);
}

TEST(compat_group_emitter_exhausted_retry_drops_only_that_original)
{
    Fixture f;
    f.config.maximum_attempts = 2;
    f.recreate();
    f.output.failures = 2;
    REQUIRE_EQ(f.enqueue(1), Admission::accepted);
    REQUIRE_EQ(f.enqueue(2), Admission::accepted);
    f.emitter->pump_for_testing();
    f.now += 1ms;
    f.emitter->pump_for_testing();
    REQUIRE_EQ(f.emitter->snapshot().retry_exhausted_packets, 1U);
    REQUIRE_EQ(f.emitter->snapshot().submitted_packets, 1U);
    REQUIRE_EQ(f.emitter->snapshot().last_submitted_sequence,
        std::optional {SequenceNumber {2}});
    REQUIRE_EQ(f.emitter->snapshot().failure, Emitter::Failure::none);
}

TEST(compat_group_emitter_transport_failure_cancels_accepted_queue)
{
    Fixture f;
    auto released = std::make_shared<std::atomic<unsigned>>(0);
    auto lease = std::make_shared<Lease>(released);
    REQUIRE_EQ(f.emitter->enqueue(generation, packet(1), f.now + 1s, lease),
        Admission::accepted);
    lease.reset();
    REQUIRE_EQ(f.enqueue(2), Admission::accepted);
    f.output.fatal = true;
    f.emitter->pump_for_testing();
    const auto state = f.emitter->snapshot();
    REQUIRE_EQ(state.failure, Emitter::Failure::transport);
    REQUIRE_EQ(state.system_error, 22);
    REQUIRE_EQ(state.cancelled_packets, 2U);
    REQUIRE_EQ(state.queued_bytes, 0U);
    REQUIRE_EQ(released->load(), 1U);
    REQUIRE_EQ(f.enqueue(3), Admission::failed);
    REQUIRE(!state.writable);
}

TEST(
    compat_group_emitter_partial_udp_submission_never_advances_emission_horizon)
{
    Fixture f;
    REQUIRE_EQ(f.enqueue(1), Admission::accepted);
    f.output.partial = true;
    f.emitter->pump_for_testing();
    REQUIRE_EQ(
        f.emitter->snapshot().failure, Emitter::Failure::partial_datagram);
    REQUIRE(!f.emitter->snapshot().last_submitted_sequence.has_value());
    REQUIRE_EQ(f.emitter->snapshot().submitted_packets, 0U);
}

TEST(compat_group_emitter_each_turn_has_a_fixed_work_budget)
{
    Fixture f;
    f.config.capacity = 8;
    f.config.batch_size = 3;
    f.config.wire_bytes_per_second = std::numeric_limits<std::uint64_t>::max();
    f.recreate();
    f.output.clock = &f.now;
    f.output.tick = 1ns;
    for (std::uint32_t i = 1; i <= 8; ++i)
        REQUIRE_EQ(f.enqueue(i), Admission::accepted);
    f.emitter->pump_for_testing();
    REQUIRE_EQ(f.emitter->snapshot().submitted_packets, 3U);
    REQUIRE_EQ(f.emitter->snapshot().queued, 5U);
    f.emitter->pump_for_testing();
    REQUIRE_EQ(f.emitter->snapshot().submitted_packets, 6U);
}

TEST(
    compat_group_emitter_clock_regression_and_deadline_overflow_do_not_create_credit)
{
    Fixture f;
    REQUIRE_EQ(f.enqueue(1), Admission::accepted);
    REQUIRE_EQ(f.enqueue(2), Admission::accepted);
    f.emitter->pump_for_testing();
    f.now -= 1s;
    f.emitter->pump_for_testing();
    REQUIRE_EQ(f.emitter->snapshot().submitted_packets, 1U);
    f.now = Emitter::TimePoint {} + 45us;
    f.emitter->pump_for_testing();
    REQUIRE_EQ(f.emitter->snapshot().submitted_packets, 2U);
    f.now = Emitter::TimePoint::max() - 1us;
    f.config.wire_bytes_per_second = 1;
    f.recreate();
    REQUIRE_EQ(f.enqueue(1, Emitter::TimePoint::max()), Admission::accepted);
    REQUIRE_EQ(f.enqueue(2, Emitter::TimePoint::max()), Admission::accepted);
    f.emitter->pump_for_testing();
    REQUIRE_EQ(*f.emitter->snapshot().next_wakeup, Emitter::TimePoint::max());
    f.now = Emitter::TimePoint::max();
    f.emitter->pump_for_testing();
    REQUIRE_EQ(f.emitter->snapshot().expired_packets, 1U);
}

TEST(compat_group_emitter_close_releases_leases_and_cancels_future_emission)
{
    Fixture f;
    auto released = std::make_shared<std::atomic<unsigned>>(0);
    auto lease = std::make_shared<Lease>(released);
    REQUIRE_EQ(f.emitter->enqueue(generation, packet(1), f.now + 1s, lease),
        Admission::accepted);
    lease.reset();
    f.emitter->close();
    f.emitter->close();
    f.emitter->pump_for_testing();
    REQUIRE_EQ(released->load(), 1U);
    REQUIRE_EQ(f.emitter->snapshot().cancelled_packets, 1U);
    REQUIRE(f.output.attempts.empty());
    REQUIRE_EQ(f.enqueue(2), Admission::closed);
}

TEST(
    compat_group_emitter_reserved_service_survives_ordinary_queue_and_timer_saturation)
{
    Fixture f;
    auto pool = scheduler();
    auto config = f.config;
    config.clock = nullptr;
    config.batch_size = 1;
    config.wire_bytes_per_second = 4500;
    auto emitter = Emitter::create(config, pool);
    REQUIRE(emitter);
    auto gate = std::make_shared<Gate>();
    GateRelease cleanup {gate};
    REQUIRE_EQ(pool->submit(0, {wait_at_gate, gate}),
        RuntimeScheduler::SubmitStatus::accepted);
    entered(gate);
    REQUIRE_EQ(
        pool->submit(0, {noop, {}}), RuntimeScheduler::SubmitStatus::accepted);
    const auto timer =
        pool->schedule_at(0, Emitter::Clock::now() + 10s, {noop, {}});
    REQUIRE_EQ(timer.status, RuntimeScheduler::SubmitStatus::accepted);
    REQUIRE_EQ(
        pool->submit(0, {noop, {}}), RuntimeScheduler::SubmitStatus::full);
    REQUIRE_EQ(
        emitter->enqueue(generation, packet(1), Emitter::Clock::now() + 2s),
        Admission::accepted);
    REQUIRE_EQ(
        emitter->enqueue(generation, packet(2), Emitter::Clock::now() + 2s),
        Admission::accepted);
    REQUIRE_EQ(emitter->snapshot().queued, 2U);
    release_gate(gate);
    await_attempts(f.output, 2);
    REQUIRE_EQ(emitter->snapshot().submitted_packets, 2U);
    emitter->close();
    (void)pool->cancel_timer(timer.token);
    pool->stop();
    // Submission is visible before the callback returns and the scheduler
    // increments its completion counter. Join workers before reading it.
    REQUIRE(pool->snapshot().service_runs >= 2U);
}

TEST(compat_group_emitter_service_reuse_cannot_execute_a_retired_owner)
{
    Fixture old;
    Fixture fresh;
    auto pool = scheduler();
    auto config = old.config;
    config.clock = nullptr;
    auto emitter = Emitter::create(config, pool);
    REQUIRE(emitter);
    REQUIRE(!Emitter::create(config, pool));
    auto gate = std::make_shared<Gate>();
    GateRelease cleanup {gate};
    REQUIRE_EQ(pool->submit(0, {wait_at_gate, gate}),
        RuntimeScheduler::SubmitStatus::accepted);
    entered(gate);
    REQUIRE_EQ(
        emitter->enqueue(generation, packet(1), Emitter::Clock::now() + 2s),
        Admission::accepted);
    emitter->close();
    REQUIRE_EQ(pool->snapshot().services_reserved, 0U);
    auto next_config = fresh.config;
    next_config.clock = nullptr;
    next_config.generation = generation + 1;
    auto next = Emitter::create(next_config, pool);
    REQUIRE(next);
    REQUIRE_EQ(next->enqueue(generation, packet(1), Emitter::Clock::now() + 2s),
        Admission::wrong_generation);
    REQUIRE_EQ(
        next->enqueue(generation + 1, packet(1), Emitter::Clock::now() + 2s),
        Admission::accepted);
    release_gate(gate);
    await_attempts(fresh.output, 1);
    next->close();
    pool->stop();
    REQUIRE(old.output.attempts.empty());
}

TEST(compat_group_emitter_scheduler_stop_rejects_uncommitted_work)
{
    Fixture f;
    auto pool = scheduler();
    auto emitter = Emitter::create(f.config, pool);
    REQUIRE(emitter);
    pool->stop();
    REQUIRE_EQ(
        emitter->enqueue(generation, packet(1), f.now + 1s), Admission::failed);
    REQUIRE_EQ(emitter->snapshot().accepted_packets, 0U);
    REQUIRE_EQ(emitter->snapshot().accepted_next_sequence, SequenceNumber {1});
    REQUIRE_EQ(emitter->snapshot().queued, 0U);
    REQUIRE_EQ(emitter->snapshot().failure, Emitter::Failure::scheduler);
}

TEST(compat_group_emitter_owner_retirement_is_outside_the_emitter_lock)
{
    struct Owner {
        std::weak_ptr<Emitter> emitter;
        std::shared_ptr<std::atomic<unsigned>> retired;
        ~Owner()
        {
            if (auto value = emitter.lock()) {
                const auto state = value->snapshot();
                if (state.queued == 0U)
                    ++*retired;
            }
        }
    };
    Fixture f;
    auto retired = std::make_shared<std::atomic<unsigned>>(0);
    auto lease = std::make_shared<Owner>(f.emitter, retired);
    REQUIRE_EQ(f.emitter->enqueue(generation, packet(1), f.now + 1s, lease),
        Admission::accepted);
    lease.reset();
    f.emitter->pump_for_testing();
    REQUIRE_EQ(retired->load(), 1U);
    lease = std::make_shared<Owner>(f.emitter, retired);
    REQUIRE_EQ(f.emitter->enqueue(generation, packet(2), f.now + 1s, lease),
        Admission::accepted);
    lease.reset();
    f.emitter->close();
    REQUIRE_EQ(retired->load(), 2U);
}

namespace {
UdpIoResult blocked_send(
    std::span<const std::byte> bytes, IpEndpoint, void* context) noexcept
{
    wait_at_gate(context);
    return {.bytes_transferred = bytes.size()};
}
} // namespace

TEST(compat_group_emitter_close_waits_for_admitted_io_and_rejects_late_work)
{
    Fixture f;
    auto pool = scheduler();
    auto gate = std::make_shared<Gate>();
    f.channel->set_send_hook_for_testing(blocked_send, gate.get());
    auto config = f.config;
    config.clock = nullptr;
    auto emitter = Emitter::create(config, pool);
    REQUIRE(emitter);
    std::future<void> closed;
    GateRelease cleanup {
        gate}; // Releases blocked I/O before future destruction.
    REQUIRE_EQ(
        emitter->enqueue(generation, packet(1), Emitter::Clock::now() + 2s),
        Admission::accepted);
    entered(gate);
    std::promise<void> requested;
    auto starting = requested.get_future();
    closed = std::async(std::launch::async, [&] {
        requested.set_value();
        emitter->close();
    });
    REQUIRE_EQ(starting.wait_for(2s), std::future_status::ready);
    REQUIRE_EQ(closed.wait_for(10ms), std::future_status::timeout);
    release_gate(gate);
    REQUIRE_EQ(closed.wait_for(2s), std::future_status::ready);
    closed.get();
    REQUIRE_EQ(emitter->snapshot().submitted_packets, 1U);
    REQUIRE_EQ(
        emitter->enqueue(generation, packet(2), Emitter::Clock::now() + 2s),
        Admission::closed);
    pool->stop();
    f.channel->set_send_hook_for_testing(capture, &f.output);
}

TEST(
    compat_group_emitter_protected_packets_require_generation_ownership_and_keep_exact_headers)
{
    for (const auto selector : {EncryptionKey::even, EncryptionKey::odd}) {
        Fixture f;
        std::array<std::byte, 32> opaque_ciphertext {};
        std::fill(opaque_ciphertext.begin(), opaque_ciphertext.end(),
            std::byte {0x5a});
        MutablePacketView view;
        view.kind = PacketKind::data;
        view.data.sequence = SequenceNumber {1};
        view.data.message_number = 17;
        view.data.boundary = MessageBoundary::solo;
        view.data.timestamp = PacketTimestamp {0x12345678};
        view.data.destination_socket_id = group_id;
        view.data.encryption_key = selector;
        view.payload = opaque_ciphertext;
        std::array<std::byte, 64> encoded {};
        const auto sealed = encode_packet(view, encoded);
        REQUIRE(sealed);
        // These are opaque protected-byte fixtures, not a crypto implementation.
        const auto bytes = std::span {encoded}.first(sealed.bytes_written);
        const std::vector<std::byte> expected {bytes.begin(), bytes.end()};
        REQUIRE_EQ(f.emitter->enqueue(generation, bytes, f.now + 1s),
            Admission::invalid_packet);
        auto retired = std::make_shared<std::atomic<unsigned>>(0);
        auto owner = std::make_shared<Lease>(retired);
        REQUIRE_EQ(f.emitter->enqueue(generation, bytes, f.now + 1s, owner),
            Admission::accepted);
        owner.reset();
        std::fill(encoded.begin(), encoded.end(), std::byte {});
        f.emitter->pump_for_testing();
        REQUIRE_EQ(f.output.attempts.front(), expected);
        REQUIRE_EQ(retired->load(), 1U);
    }
    Fixture f;
    auto invalid = packet(1);
    // The key selector occupies bits 27..28 of the message word.
    invalid[4] |= std::byte {0x18};
    REQUIRE_EQ(f.emitter->enqueue(
                   generation, invalid, f.now + 1s, std::make_shared<int>(1)),
        Admission::invalid_packet);
}

TEST(
    compat_group_emitter_unused_advance_is_atomic_with_callback_and_preserves_queued_work)
{
    Fixture f;
    f.config.capacity = 1;
    f.recreate();
    REQUIRE_EQ(f.enqueue(1), Admission::accepted);
    unsigned commits = 0;
    const auto commit = +[](void* p) noexcept {
        ++*static_cast<unsigned*>(p);
        return true;
    };
    const auto reject = +[](void*) noexcept {
        return false;
    };
    REQUIRE_EQ(f.emitter->advance_unused(
                   generation + 1, SequenceNumber {2}, commit, &commits),
        Admission::wrong_generation);
    REQUIRE_EQ(f.emitter->advance_unused(
                   generation, SequenceNumber {3}, commit, &commits),
        Admission::wrong_sequence);
    REQUIRE_EQ(f.emitter->advance_unused(
                   generation, SequenceNumber {2}, commit, &commits, f.now),
        Admission::expired);
    REQUIRE_EQ(f.emitter->advance_unused(
                   generation, SequenceNumber {2}, reject, nullptr),
        Admission::cancelled);
    REQUIRE_EQ(commits, 0U);
    REQUIRE_EQ(
        f.emitter->snapshot().accepted_next_sequence, SequenceNumber {2});
    // No new queue/mailbox slot is needed, even while the packet queue is full.
    REQUIRE_EQ(f.emitter->advance_unused(
                   generation, SequenceNumber {2}, commit, &commits),
        Admission::accepted);
    REQUIRE_EQ(commits, 1U);
    REQUIRE_EQ(
        f.emitter->snapshot().accepted_next_sequence, SequenceNumber {3});
    REQUIRE_EQ(f.emitter->snapshot().unused_packets, 1U);
    f.emitter->pump_for_testing();
    REQUIRE_EQ(f.output.attempts.size(), 1U);
    REQUIRE_EQ(f.output.attempts[0], packet(1));
    f.emitter->close();
    REQUIRE_EQ(f.emitter->advance_unused(
                   generation, SequenceNumber {3}, commit, &commits),
        Admission::closed);
    REQUIRE_EQ(commits, 1U);
}
