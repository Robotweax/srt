#include "test.hpp"

#include "compat/connection_work_binding.hpp"
#include "compat/transport_runtime.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <thread>

using namespace robotweax::srt;
using namespace robotweax::srt::compat;

namespace {

struct WorkGate {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool open = false;

    static void block(void* pointer) noexcept
    {
        auto& gate = *static_cast<WorkGate*>(pointer);
        std::unique_lock lock(gate.mutex);
        gate.entered = true;
        gate.changed.notify_all();
        gate.changed.wait(lock, [&] {
            return gate.open;
        });
    }

    void wait()
    {
        std::unique_lock lock(mutex);
        REQUIRE(changed.wait_for(lock, std::chrono::seconds {2}, [&] {
            return entered;
        }));
    }

    void release() noexcept
    {
        std::lock_guard lock(mutex);
        open = true;
        changed.notify_all();
    }
};

struct WorkGateRelease {
    std::shared_ptr<WorkGate> gate;
    ~WorkGateRelease()
    {
        gate->release();
    }
};

struct WorkProbe {
    std::mutex mutex;
    std::condition_variable changed;
    std::array<ConnectionWorkHints, 4> turns {};
    std::array<std::thread::id, 4> threads {};
    std::size_t count = 0;
    std::shared_ptr<WorkGate> first_turn_gate;
    std::weak_ptr<ConnectionRuntime> runtime;
    bool observed_runtime = false;

    static void run(void* pointer, ConnectionWorkHints hints) noexcept
    {
        auto& probe = *static_cast<WorkProbe*>(pointer);
        const auto runtime = probe.runtime.lock();
        // Take the real runtime lock from the service, without retaining any
        // caller memory or extending the runtime's lifetime from its context.
        const bool observed = runtime != nullptr && !runtime->terminal();
        bool first;
        {
            std::lock_guard lock(probe.mutex);
            first = probe.count == 0U;
            if (probe.count < probe.turns.size()) {
                probe.turns[probe.count] = hints;
                probe.threads[probe.count] = std::this_thread::get_id();
            }
            ++probe.count;
            probe.observed_runtime |= observed;
            probe.changed.notify_all();
        }
        if (first && probe.first_turn_gate != nullptr) {
            WorkGate::block(probe.first_turn_gate.get());
        }
    }

    void wait(std::size_t expected)
    {
        std::unique_lock lock(mutex);
        REQUIRE(changed.wait_for(lock, std::chrono::seconds {2}, [&] {
            return count >= expected;
        }));
    }
};

std::shared_ptr<RuntimeScheduler> work_scheduler(std::size_t shards = 1)
{
    auto scheduler = std::make_shared<RuntimeScheduler>(
        RuntimeScheduler::Configuration {.shard_count = shards,
            .queue_capacity_per_shard = 1,
            .timer_capacity_per_shard = 1,
            .service_capacity_per_shard = 1});
    REQUIRE(scheduler->start());
    return scheduler;
}

void no_work(void*) noexcept { }

} // namespace

TEST(connection_work_binding_coalesces_reasons_despite_pool_pressure)
{
    auto scheduler = work_scheduler(2);
    auto gate = std::make_shared<WorkGate>();
    auto probe = std::make_shared<WorkProbe>();
    auto binding =
        ConnectionWorkBinding::create(scheduler, 1, WorkProbe::run, probe);
    REQUIRE(binding != nullptr);
    REQUIRE_EQ(binding->token().shard, scheduler->shard_for(1));
    const auto before = binding->observe_work();
    REQUIRE(binding->work_is_current(before));
    std::array<std::future<RuntimeScheduler::SubmitStatus>, 16> callers;
    WorkGateRelease release {gate};
    REQUIRE_EQ(
        scheduler->submit(1, {.function = WorkGate::block, .context = gate}),
        RuntimeScheduler::SubmitStatus::accepted);
    gate->wait();
    REQUIRE_EQ(scheduler->submit(1, {.function = no_work}),
        RuntimeScheduler::SubmitStatus::accepted);
    REQUIRE_EQ(scheduler->submit(1, {.function = no_work}),
        RuntimeScheduler::SubmitStatus::full);
    REQUIRE_EQ(
        scheduler
            ->schedule_at(1,
                std::chrono::steady_clock::now() + std::chrono::hours {1},
                {.function = no_work})
            .status,
        RuntimeScheduler::SubmitStatus::accepted);
    REQUIRE(ConnectionWorkBinding::create(scheduler, 1, WorkProbe::run, probe)
        == nullptr);
    for (std::size_t index = 0; index < callers.size(); ++index) {
        callers[index] = std::async(std::launch::async, [binding, index] {
            return binding->notify({.send = index % 2U == 0U,
                .receive_release = index % 2U != 0U});
        });
    }
    for (auto& caller : callers) {
        REQUIRE_EQ(caller.wait_for(std::chrono::seconds {2}),
            std::future_status::ready);
        REQUIRE_EQ(caller.get(), RuntimeScheduler::SubmitStatus::accepted);
    }
    REQUIRE_EQ(scheduler->snapshot().services_pending, 1U);
    const auto after = binding->observe_work();
    REQUIRE_EQ(after.epoch, before.epoch + callers.size());
    REQUIRE(!binding->work_is_current(before));
    REQUIRE(binding->work_is_current(after));
    gate->release();
    probe->wait(1);
    binding->retire();
    scheduler->stop();
    REQUIRE_EQ(probe->count, 1U);
    REQUIRE(probe->turns[0].send);
    REQUIRE(probe->turns[0].receive_release);
    REQUIRE(probe->threads[0] != std::this_thread::get_id());
}

TEST(connection_work_binding_publication_during_dispatch_gets_another_turn)
{
    auto scheduler = work_scheduler();
    auto probe = std::make_shared<WorkProbe>();
    auto gate = std::make_shared<WorkGate>();
    probe->first_turn_gate = gate;
    auto binding =
        ConnectionWorkBinding::create(scheduler, 0, WorkProbe::run, probe);
    REQUIRE(binding != nullptr);
    WorkGateRelease release {gate};
    REQUIRE_EQ(binding->notify({.send = true}),
        RuntimeScheduler::SubmitStatus::accepted);
    gate->wait();
    const auto during = binding->observe_work();
    REQUIRE_EQ(binding->notify({.receive_release = true}),
        RuntimeScheduler::SubmitStatus::accepted);
    REQUIRE(!binding->work_is_current(during));
    REQUIRE_EQ(binding->notify({.receive_release = true}),
        RuntimeScheduler::SubmitStatus::accepted);
    gate->release();
    probe->wait(2);
    binding->retire();
    scheduler->stop();
    REQUIRE_EQ(probe->count, 2U);
    REQUIRE(probe->turns[0].send && !probe->turns[0].receive_release);
    REQUIRE(!probe->turns[1].send && probe->turns[1].receive_release);
}

TEST(connection_work_binding_retirement_rejects_old_generation)
{
    auto scheduler = work_scheduler();
    auto gate = std::make_shared<WorkGate>();
    auto old_probe = std::make_shared<WorkProbe>();
    auto old =
        ConnectionWorkBinding::create(scheduler, 0, WorkProbe::run, old_probe);
    REQUIRE(old != nullptr);
    WorkGateRelease release {gate};
    REQUIRE_EQ(
        scheduler->submit(0, {.function = WorkGate::block, .context = gate}),
        RuntimeScheduler::SubmitStatus::accepted);
    gate->wait();
    REQUIRE_EQ(
        old->notify({.send = true}), RuntimeScheduler::SubmitStatus::accepted);
    const auto old_observation = old->observe_work();
    old->retire();
    REQUIRE(!old->work_is_current(old_observation));
    REQUIRE_EQ(old->observe_work().epoch, 0U);
    old->retire();
    REQUIRE(old->quiescent());
    auto probe = std::make_shared<WorkProbe>();
    auto binding =
        ConnectionWorkBinding::create(scheduler, 0, WorkProbe::run, probe);
    REQUIRE(binding != nullptr);
    REQUIRE_EQ(binding->token().slot, old->token().slot);
    REQUIRE(binding->token().generation != old->token().generation);
    REQUIRE_EQ(old->notify({.receive_release = true}),
        RuntimeScheduler::SubmitStatus::invalid);
    REQUIRE_EQ(scheduler->notify_service(old->token()),
        RuntimeScheduler::SubmitStatus::invalid);
    REQUIRE_EQ(binding->notify({.receive_release = true}),
        RuntimeScheduler::SubmitStatus::accepted);
    REQUIRE_EQ(binding->observe_work().epoch, old_observation.epoch);
    REQUIRE(!binding->work_is_current(old_observation));
    gate->release();
    probe->wait(1);
    binding->retire();
    scheduler->stop();
    REQUIRE_EQ(old_probe->count, 0U);
    REQUIRE(!probe->turns[0].send && probe->turns[0].receive_release);
}

TEST(connection_work_binding_epoch_exhaustion_never_revives_old_observations)
{
    const auto maximum = (std::numeric_limits<std::uint64_t>::max)();
    REQUIRE_EQ(next_connection_work_epoch(maximum - 1U), maximum);
    REQUIRE_EQ(next_connection_work_epoch(maximum), 0U);
    REQUIRE_EQ(next_connection_work_epoch(0U), 0U);
}

TEST(connection_work_binding_observations_do_not_alias_foreign_scheduler_scope)
{
    auto first_scheduler = work_scheduler();
    auto second_scheduler = work_scheduler();
    auto probe = std::make_shared<WorkProbe>();
    auto first = ConnectionWorkBinding::create(
        first_scheduler, 0, WorkProbe::run, probe);
    auto second = ConnectionWorkBinding::create(
        second_scheduler, 0, WorkProbe::run, probe);
    REQUIRE(first != nullptr && second != nullptr);
    const auto observation = first->observe_work();
    REQUIRE_EQ(observation.epoch, second->observe_work().epoch);
    REQUIRE(first->work_is_current(observation));
    REQUIRE(!second->work_is_current(observation));
    REQUIRE(!first->work_is_current({}));
    first->retire();
    second->retire();
    first_scheduler->stop();
    second_scheduler->stop();
}

TEST(connection_work_binding_retirement_does_not_wait_for_active_callback)
{
    auto scheduler = work_scheduler();
    auto gate = std::make_shared<WorkGate>();
    auto probe = std::make_shared<WorkProbe>();
    probe->first_turn_gate = gate;
    auto binding =
        ConnectionWorkBinding::create(scheduler, 0, WorkProbe::run, probe);
    REQUIRE(binding != nullptr);
    WorkGateRelease release {gate};
    REQUIRE_EQ(binding->notify({.send = true}),
        RuntimeScheduler::SubmitStatus::accepted);
    gate->wait();
    binding->retire();
    REQUIRE(!binding->quiescent());
    REQUIRE(ConnectionWorkBinding::create(scheduler, 0, WorkProbe::run, probe)
        == nullptr);
    REQUIRE_EQ(binding->notify({.send = true}),
        RuntimeScheduler::SubmitStatus::invalid);
    gate->release();
    scheduler->stop();
    REQUIRE(binding->quiescent());
    REQUIRE_EQ(probe->count, 1U);
}

TEST(connection_work_binding_does_not_pin_scheduler_and_fails_closed)
{
    auto scheduler = work_scheduler();
    auto probe = std::make_shared<WorkProbe>();
    REQUIRE(ConnectionWorkBinding::create(nullptr, 0, WorkProbe::run, probe)
        == nullptr);
    REQUIRE(
        ConnectionWorkBinding::create(scheduler, 0, nullptr, probe) == nullptr);
    auto binding =
        ConnectionWorkBinding::create(scheduler, 0, WorkProbe::run, probe);
    REQUIRE(binding != nullptr);
    REQUIRE_EQ(binding->notify({}), RuntimeScheduler::SubmitStatus::invalid);
    scheduler->stop();
    REQUIRE_EQ(binding->notify({.send = true}),
        RuntimeScheduler::SubmitStatus::stopped);
    scheduler.reset();
    REQUIRE_EQ(binding->notify({.receive_release = true}),
        RuntimeScheduler::SubmitStatus::stopped);
    binding->retire();
    REQUIRE(binding->quiescent());
}

TEST(connection_work_binding_runtime_commits_then_hints_and_close_retires)
{
    auto scheduler = work_scheduler();
    auto gate = std::make_shared<WorkGate>();
    auto probe = std::make_shared<WorkProbe>();
    auto binding =
        ConnectionWorkBinding::create(scheduler, 0, WorkProbe::run, probe);
    REQUIRE(binding != nullptr);
    SocketOptions options;
    REQUIRE_EQ(
        options.set(SocketOption::maximum_payload_size, 800), Error::none);
    REQUIRE_EQ(options.set(SocketOption::tsbpd_mode, 0), Error::none);
    const Ipv4Endpoint peer {.address = {192, 0, 2, 90}, .port = 14900};
    auto channel = std::make_shared<DatagramChannel>();
    channel->set_send_hook_for_testing(
        [](std::span<const std::byte> bytes, Ipv4Endpoint, void*) noexcept {
            return UdpIoResult {.bytes_transferred = bytes.size()};
        },
        nullptr);
    auto runtime =
        std::make_shared<ConnectionRuntime>(ConnectionRuntime::Configuration {
            .channel = channel,
            .peer = peer,
            .peer_socket_id = 90,
            .initial_sequence = SequenceNumber {1000},
            .options = options,
            .origin = ConnectionRuntime::Clock::now(),
            .work_binding = binding,
        });
    probe->runtime = runtime;
    WorkGateRelease release {gate};
    REQUIRE_EQ(
        scheduler->submit(0, {.function = WorkGate::block, .context = gate}),
        RuntimeScheduler::SubmitStatus::accepted);
    gate->wait();
    const std::array<std::byte, 800> input {};
    REQUIRE_EQ(runtime->queue_message(input, 0, true, false, 0).status,
        MessageIoStatus::success);
    PacketView packet;
    packet.kind = PacketKind::data;
    packet.data.sequence = SequenceNumber {1000};
    packet.data.message_number = 7;
    packet.data.in_order = true;
    packet.data.boundary = MessageBoundary::solo;
    packet.payload = input;
    runtime->process_packet(packet, peer);
    std::array<std::byte, 800> output;
    REQUIRE_EQ(runtime->receive_message(output, false, 0).status,
        MessageIoStatus::success);
    REQUIRE_EQ(output, input);
    REQUIRE_EQ(scheduler->snapshot().services_pending, 1U);
    gate->release();
    probe->wait(1);
    runtime->close();
    scheduler->stop();
    REQUIRE(probe->observed_runtime);
    REQUIRE(probe->turns[0].send && probe->turns[0].receive_release);
    REQUIRE(binding->quiescent());
    REQUIRE_EQ(runtime->queue_message(input, 0, true, false, 0).status,
        MessageIoStatus::local_closed);
    REQUIRE_EQ(binding->notify({.send = true}),
        RuntimeScheduler::SubmitStatus::invalid);
}

TEST(connection_work_binding_runtime_destruction_cancels_pending_hint)
{
    auto scheduler = work_scheduler();
    auto gate = std::make_shared<WorkGate>();
    auto probe = std::make_shared<WorkProbe>();
    auto binding =
        ConnectionWorkBinding::create(scheduler, 0, WorkProbe::run, probe);
    REQUIRE(binding != nullptr);
    auto runtime =
        std::make_shared<ConnectionRuntime>(ConnectionRuntime::Configuration {
            .origin = ConnectionRuntime::Clock::now(),
            .work_binding = binding,
        });
    WorkGateRelease release {gate};
    REQUIRE_EQ(
        scheduler->submit(0, {.function = WorkGate::block, .context = gate}),
        RuntimeScheduler::SubmitStatus::accepted);
    gate->wait();
    const std::array<std::byte, 8> input {};
    REQUIRE_EQ(runtime->queue_message(input, 0, true, false, 0).status,
        MessageIoStatus::success);
    REQUIRE_EQ(scheduler->snapshot().services_pending, 1U);
    runtime.reset();
    REQUIRE(binding->quiescent());
    REQUIRE_EQ(binding->notify({.send = true}),
        RuntimeScheduler::SubmitStatus::invalid);
    gate->release();
    scheduler->stop();
    REQUIRE_EQ(probe->count, 0U);
}

TEST(connection_work_binding_rejected_hint_does_not_undo_application_commit)
{
    auto scheduler = work_scheduler();
    auto probe = std::make_shared<WorkProbe>();
    auto binding =
        ConnectionWorkBinding::create(scheduler, 0, WorkProbe::run, probe);
    REQUIRE(binding != nullptr);
    SocketOptions options;
    REQUIRE_EQ(
        options.set(SocketOption::maximum_payload_size, 800), Error::none);
    REQUIRE_EQ(options.set(SocketOption::send_buffer_packets, 3), Error::none);
    auto runtime =
        std::make_shared<ConnectionRuntime>(ConnectionRuntime::Configuration {
            .options = options,
            .origin = ConnectionRuntime::Clock::now(),
            .work_binding = binding,
        });
    binding->retire();
    const std::array<std::byte, 800> input {};
    REQUIRE_EQ(runtime->queue_message(input, 0, true, false, 0).status,
        MessageIoStatus::success);
    scheduler->stop();
    REQUIRE_EQ(runtime->queue_message(input, 0, true, false, 0).status,
        MessageIoStatus::success);
    REQUIRE_EQ(runtime->queue_message(input, 0, true, false, 0).status,
        MessageIoStatus::success);
    REQUIRE_EQ(runtime->queue_message(input, 0, true, false, 0).status,
        MessageIoStatus::would_block);
    REQUIRE_EQ(runtime->buffer_packet_counts().unacknowledged_send, 3U);
    REQUIRE_EQ(probe->count, 0U);
}

TEST(connection_work_binding_runtime_close_discards_queued_hint)
{
    auto scheduler = work_scheduler();
    auto gate = std::make_shared<WorkGate>();
    auto probe = std::make_shared<WorkProbe>();
    auto binding =
        ConnectionWorkBinding::create(scheduler, 0, WorkProbe::run, probe);
    REQUIRE(binding != nullptr);
    auto runtime =
        std::make_shared<ConnectionRuntime>(ConnectionRuntime::Configuration {
            .origin = ConnectionRuntime::Clock::now(),
            .work_binding = binding,
        });
    WorkGateRelease release {gate};
    REQUIRE_EQ(
        scheduler->submit(0, {.function = WorkGate::block, .context = gate}),
        RuntimeScheduler::SubmitStatus::accepted);
    gate->wait();
    const std::array<std::byte, 8> input {};
    REQUIRE_EQ(runtime->queue_message(input, 0, true, false, 0).status,
        MessageIoStatus::success);
    runtime->close();
    REQUIRE(binding->quiescent());
    gate->release();
    scheduler->stop();
    REQUIRE_EQ(probe->count, 0U);
    REQUIRE_EQ(runtime->queue_message(input, 0, true, false, 0).status,
        MessageIoStatus::local_closed);
}

TEST(connection_work_binding_drain_deadline_preserves_running_callback)
{
    auto scheduler = work_scheduler();
    auto probe = std::make_shared<WorkProbe>();
    auto gate = std::make_shared<WorkGate>();
    probe->first_turn_gate = gate;
    auto binding =
        ConnectionWorkBinding::create(scheduler, 0, WorkProbe::run, probe);
    REQUIRE(binding != nullptr);
    std::future<ConnectionWorkBinding::DrainStatus> drain;
    WorkGateRelease release {gate};
    REQUIRE_EQ(binding->wait_quiescent(std::chrono::steady_clock::now()),
        ConnectionWorkBinding::DrainStatus::not_retired);
    REQUIRE_EQ(binding->notify({.datagrams = true}),
        RuntimeScheduler::SubmitStatus::accepted);
    gate->wait();
    binding->retire();
    REQUIRE_EQ(binding->wait_quiescent(std::chrono::steady_clock::now()),
        ConnectionWorkBinding::DrainStatus::timeout);
    REQUIRE(!binding->quiescent());
    drain = std::async(std::launch::async, [&] {
        return binding->wait_quiescent(
            std::chrono::steady_clock::now() + std::chrono::seconds {2});
    });
    REQUIRE_EQ(drain.wait_for(std::chrono::milliseconds {0}),
        std::future_status::timeout);
    gate->release();
    REQUIRE_EQ(drain.get(), ConnectionWorkBinding::DrainStatus::quiescent);
    REQUIRE_EQ(
        binding->wait_quiescent(std::chrono::steady_clock::time_point {}),
        ConnectionWorkBinding::DrainStatus::quiescent);
    scheduler->stop();
    REQUIRE(binding->quiescent());
}

TEST(connection_work_binding_drain_rejects_wait_on_any_affinity_worker)
{
    auto scheduler = work_scheduler();
    auto binding = ConnectionWorkBinding::create(
        scheduler, 0, [](void*, ConnectionWorkHints) noexcept { }, nullptr);
    REQUIRE(binding != nullptr);
    binding->retire();
    auto answer =
        std::make_shared<std::promise<ConnectionWorkBinding::DrainStatus>>();
    auto result = answer->get_future();
    struct Check {
        std::shared_ptr<ConnectionWorkBinding> binding;
        std::shared_ptr<std::promise<ConnectionWorkBinding::DrainStatus>>
            answer;
    };
    auto check = std::make_shared<Check>(Check {binding, answer});
    REQUIRE_EQ(scheduler->submit(0,
                   {.function =
                           [](void* pointer) noexcept {
                               auto& self = *static_cast<Check*>(pointer);
                               self.answer->set_value(
                                   self.binding->wait_quiescent(
                                       std::chrono::steady_clock::now()
                                       + std::chrono::seconds {10}));
                           },
                       .context = check}),
        RuntimeScheduler::SubmitStatus::accepted);
    REQUIRE_EQ(
        result.wait_for(std::chrono::seconds {2}), std::future_status::ready);
    REQUIRE_EQ(result.get(), ConnectionWorkBinding::DrainStatus::worker_thread);
    scheduler->stop();
}

namespace {
struct DrainScheduler : RuntimeScheduler {
    std::shared_ptr<WorkGate> destructor_gate;
    explicit DrainScheduler(std::shared_ptr<WorkGate> gate)
        : RuntimeScheduler(Configuration {.shard_count = 1,
              .queue_capacity_per_shard = 1,
              .timer_capacity_per_shard = 1,
              .service_capacity_per_shard = 1})
        , destructor_gate(std::move(gate))
    {
    }
    ~DrainScheduler()
    {
        WorkGate::block(destructor_gate.get());
    }
};
}

TEST(connection_work_binding_drain_expired_scheduler_is_not_callback_completion)
{
    auto destructor_gate = std::make_shared<WorkGate>();
    auto callback_gate = std::make_shared<WorkGate>();
    std::shared_ptr<RuntimeScheduler> scheduler =
        std::make_shared<DrainScheduler>(destructor_gate);
    REQUIRE(scheduler->start());
    std::weak_ptr<RuntimeScheduler> old_scheduler = scheduler;
    auto probe = std::make_shared<WorkProbe>();
    probe->first_turn_gate = callback_gate;
    auto binding =
        ConnectionWorkBinding::create(scheduler, 0, WorkProbe::run, probe);
    REQUIRE(binding != nullptr);
    std::future<void> destruction;
    WorkGateRelease release_destructor {destructor_gate};
    WorkGateRelease release_callback {callback_gate};
    REQUIRE_EQ(binding->notify({.datagrams = true}),
        RuntimeScheduler::SubmitStatus::accepted);
    callback_gate->wait();
    destruction = std::async(
        std::launch::async, [owned = std::move(scheduler)]() mutable {
            owned.reset();
        });
    destructor_gate->wait();
    REQUIRE(old_scheduler.expired());
    binding->retire();
    REQUIRE(!binding->quiescent());
    REQUIRE_EQ(binding->wait_quiescent(std::chrono::steady_clock::now()),
        ConnectionWorkBinding::DrainStatus::timeout);
    callback_gate->release();
    REQUIRE_EQ(binding->wait_quiescent(
                   std::chrono::steady_clock::now() + std::chrono::seconds {2}),
        ConnectionWorkBinding::DrainStatus::quiescent);
    REQUIRE(binding->quiescent());
    destructor_gate->release();
    destruction.get();
}

TEST(connection_work_binding_completion_request_racing_hook_gets_another_pass)
{
    auto scheduler = work_scheduler();
    struct Probe {
        std::atomic<unsigned> calls {0};
        std::shared_ptr<WorkGate> gate = std::make_shared<WorkGate>();
        static void complete(void* pointer) noexcept
        {
            auto& self = *static_cast<Probe*>(pointer);
            if (self.calls.fetch_add(1) == 0) {
                WorkGate::block(self.gate.get());
            }
        }
    };
    auto probe = std::make_shared<Probe>();
    WorkGateRelease release {probe->gate};
    auto binding = ConnectionWorkBinding::create(
        scheduler, 0, [](void*, ConnectionWorkHints) noexcept { }, probe,
        Probe::complete);
    REQUIRE(binding != nullptr);
    REQUIRE_EQ(binding->notify({.datagrams = true}),
        RuntimeScheduler::SubmitStatus::accepted);
    probe->gate->wait();
    binding->retire(true);
    REQUIRE_EQ(binding->wait_quiescent(std::chrono::steady_clock::now()),
        ConnectionWorkBinding::DrainStatus::timeout);
    probe->gate->release();
    REQUIRE_EQ(binding->wait_quiescent(
                   std::chrono::steady_clock::now() + std::chrono::seconds {2}),
        ConnectionWorkBinding::DrainStatus::quiescent);
    REQUIRE_EQ(probe->calls.load(), 2U);
    scheduler->stop();
}

TEST(connection_work_binding_quiet_retirement_includes_completion_in_barrier)
{
    auto scheduler = work_scheduler();
    auto gate = std::make_shared<WorkGate>();
    auto binding = ConnectionWorkBinding::create(
        scheduler, 0, [](void*, ConnectionWorkHints) noexcept { }, gate,
        WorkGate::block);
    REQUIRE(binding != nullptr);
    std::future<void> retirement;
    WorkGateRelease release {gate};
    retirement = std::async(std::launch::async, [&] {
        binding->retire(true);
    });
    gate->wait();
    REQUIRE(!binding->quiescent());
    REQUIRE_EQ(binding->wait_quiescent(std::chrono::steady_clock::now()),
        ConnectionWorkBinding::DrainStatus::timeout);
    gate->release();
    retirement.get();
    REQUIRE_EQ(binding->wait_quiescent(std::chrono::steady_clock::now()),
        ConnectionWorkBinding::DrainStatus::quiescent);
    scheduler->stop();
}
