#include "test.hpp"

#include "compat/connection_datagram_inbox.hpp"
#include "compat/transport_runtime.hpp"
#include "robotweax/srt/codec.hpp"

#include <array>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <future>
#include <limits>
#include <memory>
#include <mutex>

using namespace robotweax::srt;
using namespace robotweax::srt::compat;

namespace {
using InboxStatus = ConnectionDatagramInbox::Status;
constexpr Ipv4Endpoint inbox_peer {.address = {192, 0, 2, 93}, .port = 14903};

struct InboxGate {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool open = false;

    static void run(void* pointer) noexcept
    {
        auto& gate = *static_cast<InboxGate*>(pointer);
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
struct InboxGateRelease {
    std::shared_ptr<InboxGate> gate;
    ~InboxGateRelease()
    {
        gate->release();
    }
};

std::shared_ptr<RuntimeScheduler> inbox_scheduler(std::size_t services = 2)
{
    auto scheduler = std::make_shared<RuntimeScheduler>(
        RuntimeScheduler::Configuration {.shard_count = 1,
            .queue_capacity_per_shard = 1,
            .timer_capacity_per_shard = 1,
            .service_capacity_per_shard = services});
    REQUIRE(scheduler->start());
    return scheduler;
}
std::shared_ptr<ConnectionWorkBinding> inbox_binding(
    const std::shared_ptr<RuntimeScheduler>& scheduler)
{
    auto binding = ConnectionWorkBinding::create(
        scheduler, 0, [](void*, ConnectionWorkHints) noexcept { }, nullptr);
    REQUIRE(binding != nullptr);
    return binding;
}
struct InboxWire {
    std::array<std::byte, 1500> bytes {};
    std::size_t size = 0;
    std::span<const std::byte> view() const
    {
        return std::span {bytes}.first(size);
    }
};
InboxWire inbox_wire(std::byte tag, bool control = false)
{
    MutablePacketView packet;
    packet.kind = control ? PacketKind::control : PacketKind::data;
    packet.control.type = ControlType::keepalive;
    packet.control.destination_socket_id = 700;
    packet.data.sequence = SequenceNumber {1000};
    packet.data.destination_socket_id = 700;
    packet.data.boundary = MessageBoundary::solo;
    const std::array payload {tag};
    if (!control) {
        packet.payload = payload;
    }
    InboxWire wire;
    const auto encoded = encode_packet(packet, wire.bytes);
    REQUIRE(encoded);
    wire.size = encoded.bytes_written;
    return wire;
}
std::size_t inbox_bytes(std::size_t capacity)
{
    const auto bytes = ConnectionDatagramInbox::storage_bytes(capacity);
    REQUIRE(bytes.has_value());
    return *bytes;
}
std::shared_ptr<ConnectionDatagramInbox> make_inbox(
    const std::shared_ptr<DatagramStorageBudget>& budget,
    const std::shared_ptr<ConnectionWorkBinding>& binding,
    std::size_t capacity = 4, std::size_t reserve = 1)
{
    auto inbox = ConnectionDatagramInbox::create(budget, binding, inbox_peer,
        {.capacity = capacity, .control_reserve = reserve});
    REQUIRE(inbox != nullptr);
    return inbox;
}

struct InboxConsumer {
    std::mutex mutex;
    std::condition_variable changed;
    std::shared_ptr<ConnectionDatagramInbox> inbox;
    std::shared_ptr<InboxGate> first_gate;
    std::array<std::byte, 16> received {};
    std::size_t count = 0;
    std::size_t turns = 0;
    bool valid = true;

    static void run(void* pointer, ConnectionWorkHints hints) noexcept
    {
        auto& self = *static_cast<InboxConsumer*>(pointer);
        std::shared_ptr<ConnectionDatagramInbox> inbox;
        bool first;
        {
            std::lock_guard lock(self.mutex);
            inbox = self.inbox;
            first = self.turns++ == 0;
            self.valid &= hints.datagrams;
        }
        if (inbox != nullptr) {
            DatagramEnvelope envelope;
            if (inbox->pop(inbox->token(), envelope, 100)
                == InboxStatus::accepted) {
                const auto packet = decode_packet(
                    std::span {envelope.bytes}.first(envelope.size));
                std::lock_guard lock(self.mutex);
                self.valid &= static_cast<bool>(packet)
                    && self.count < self.received.size();
                if (packet && self.count < self.received.size()) {
                    self.received[self.count++] =
                        packet.packet.kind == PacketKind::control
                        ? std::byte {0}
                        : packet.packet.payload.front();
                }
                self.changed.notify_all();
            }
            if (first && self.first_gate != nullptr) {
                InboxGate::run(self.first_gate.get());
            }
            (void)inbox->rearm(inbox->token());
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
} // namespace

TEST(connection_datagram_inbox_storage_budget_counts_empty_and_closed_inboxes)
{
    auto scheduler = inbox_scheduler();
    auto binding = inbox_binding(scheduler);
    auto budget = std::make_shared<DatagramStorageBudget>(inbox_bytes(4));
    auto inbox = make_inbox(budget, binding);
    REQUIRE_EQ(budget->reserved_bytes(), inbox_bytes(4));
    REQUIRE(ConnectionDatagramInbox::create(budget, binding, inbox_peer,
                {.capacity = 1, .control_reserve = 0})
        == nullptr);
    auto captured = inbox;
    inbox->close();
    inbox.reset();
    REQUIRE_EQ(budget->reserved_bytes(), inbox_bytes(4));
    captured.reset();
    REQUIRE_EQ(budget->reserved_bytes(), 0U);
    auto replacement = make_inbox(budget, binding);
    scheduler->stop();
}

TEST(connection_datagram_inbox_validates_storage_bounds_before_reservation)
{
    REQUIRE(!ConnectionDatagramInbox::storage_bytes(0).has_value());
    REQUIRE(!ConnectionDatagramInbox::storage_bytes(
        std::numeric_limits<std::size_t>::max())
            .has_value());
    auto scheduler = inbox_scheduler();
    auto binding = inbox_binding(scheduler);
    auto budget = std::make_shared<DatagramStorageBudget>(inbox_bytes(4));
    REQUIRE(ConnectionDatagramInbox::create(budget, binding, inbox_peer,
                {.capacity = 4, .control_reserve = 5})
        == nullptr);
    REQUIRE(ConnectionDatagramInbox::create(budget, nullptr, inbox_peer,
                {.capacity = 4, .control_reserve = 1})
        == nullptr);
    REQUIRE_EQ(budget->reserved_bytes(), 0U);
    scheduler->stop();
}

TEST(connection_datagram_inbox_control_reserve_preserves_fifo_and_owned_bytes)
{
    auto scheduler = inbox_scheduler();
    auto binding = inbox_binding(scheduler);
    auto budget = std::make_shared<DatagramStorageBudget>(inbox_bytes(4));
    auto inbox = make_inbox(budget, binding);
    const auto token = inbox->token();
    auto data = inbox_wire(std::byte {1});
    REQUIRE_EQ(inbox->publish(token, data.view(), inbox_peer, 10),
        InboxStatus::accepted);
    data.bytes.fill(std::byte {0xff});
    REQUIRE_EQ(
        inbox->publish(token, inbox_wire(std::byte {2}).view(), inbox_peer, 11),
        InboxStatus::accepted);
    REQUIRE_EQ(
        inbox->publish(token, inbox_wire(std::byte {3}).view(), inbox_peer, 12),
        InboxStatus::accepted);
    REQUIRE_EQ(
        inbox->publish(token, inbox_wire(std::byte {4}).view(), inbox_peer, 13),
        InboxStatus::full);
    REQUIRE_EQ(
        inbox->publish(token, inbox_wire({}, true).view(), inbox_peer, 14),
        InboxStatus::accepted);
    REQUIRE_EQ(
        inbox->publish(token, inbox_wire({}, true).view(), inbox_peer, 15),
        InboxStatus::full);
    DatagramEnvelope envelope;
    for (const auto tag :
        {std::byte {1}, std::byte {2}, std::byte {3}, std::byte {0}}) {
        REQUIRE_EQ(inbox->pop(token, envelope, 30), InboxStatus::accepted);
        const auto packet =
            decode_packet(std::span {envelope.bytes}.first(envelope.size));
        REQUIRE(packet);
        REQUIRE_EQ(packet.packet.kind == PacketKind::control
                ? std::byte {0}
                : packet.packet.payload.front(),
            tag);
        REQUIRE_EQ(envelope.peer, IpEndpoint {inbox_peer});
    }
    REQUIRE_EQ(inbox->pop(token, envelope, 30), InboxStatus::empty);
    const auto snapshot = inbox->snapshot();
    REQUIRE_EQ(snapshot.highwater, 4U);
    REQUIRE_EQ(snapshot.data_rejections, 1U);
    REQUIRE_EQ(snapshot.control_rejections, 1U);
    REQUIRE_EQ(snapshot.maximum_queue_delay, 20U);
    REQUIRE_EQ(snapshot.data_queued, 0U);
    // Exercise ring wrap and release of DATA admission after consumption.
    for (std::size_t index = 0; index < 12; ++index) {
        REQUIRE_EQ(inbox->publish(
                       token, inbox_wire(std::byte {5}).view(), inbox_peer, 50),
            InboxStatus::accepted);
        REQUIRE_EQ(inbox->pop(token, envelope, 49), InboxStatus::accepted);
    }
    REQUIRE_EQ(inbox->snapshot().maximum_queue_delay, 20U);
    scheduler->stop();
}

TEST(connection_datagram_inbox_rejects_stale_generations_and_invalid_peers)
{
    auto scheduler = inbox_scheduler();
    auto binding = inbox_binding(scheduler);
    auto budget = std::make_shared<DatagramStorageBudget>(inbox_bytes(8));
    auto first = make_inbox(budget, binding);
    auto second = make_inbox(budget, binding);
    const auto wire = inbox_wire(std::byte {1});
    REQUIRE_EQ(second->publish(first->token(), wire.view(), inbox_peer, 1),
        InboxStatus::stale);
    auto token = second->token();
    ++token.service.generation;
    REQUIRE_EQ(
        second->publish(token, wire.view(), inbox_peer, 1), InboxStatus::stale);
    token = second->token();
    ++token.service.scope;
    REQUIRE_EQ(
        second->publish(token, wire.view(), inbox_peer, 1), InboxStatus::stale);
    DatagramEnvelope envelope;
    REQUIRE_EQ(second->pop(first->token(), envelope, 1), InboxStatus::stale);
    REQUIRE_EQ(second->rearm(first->token()), InboxStatus::stale);
    const Ipv4Endpoint other {.address = {192, 0, 2, 94}, .port = 14903};
    REQUIRE_EQ(second->publish(second->token(), wire.view(), other, 1),
        InboxStatus::invalid);
    const std::array malformed {std::byte {1}};
    REQUIRE_EQ(second->publish(second->token(), malformed, inbox_peer, 1),
        InboxStatus::invalid);
    const std::array<std::byte, 1501> oversized {};
    REQUIRE_EQ(second->publish(second->token(), oversized, inbox_peer, 1),
        InboxStatus::invalid);
    first->close();
    REQUIRE_EQ(first->publish(first->token(), wire.view(), inbox_peer, 1),
        InboxStatus::closed);
    REQUIRE_EQ(second->snapshot().queued, 0U);
    scheduler->stop();
}

TEST(connection_datagram_inbox_failed_wake_is_terminal_and_discards_queue)
{
    auto scheduler = inbox_scheduler();
    auto binding = inbox_binding(scheduler);
    auto budget = std::make_shared<DatagramStorageBudget>(inbox_bytes(4));
    auto inbox = make_inbox(budget, binding);
    REQUIRE_EQ(inbox->publish(inbox->token(), inbox_wire(std::byte {1}).view(),
                   inbox_peer, 1),
        InboxStatus::accepted);
    binding->retire();
    REQUIRE_EQ(inbox->publish(inbox->token(), inbox_wire(std::byte {2}).view(),
                   inbox_peer, 2),
        InboxStatus::wake_failed);
    REQUIRE(inbox->snapshot().closed);
    REQUIRE_EQ(inbox->snapshot().queued, 0U);
    REQUIRE_EQ(inbox->snapshot().wake_failures, 1U);
    DatagramEnvelope envelope;
    REQUIRE_EQ(inbox->pop(inbox->token(), envelope, 3), InboxStatus::closed);
    REQUIRE_EQ(inbox->rearm(inbox->token()), InboxStatus::closed);
    REQUIRE_EQ(budget->reserved_bytes(), inbox_bytes(4));
    scheduler->stop();
}

TEST(connection_datagram_inbox_reserved_wake_survives_full_scheduler_pools)
{
    auto scheduler = inbox_scheduler(1);
    auto gate = std::make_shared<InboxGate>();
    auto consumer = std::make_shared<InboxConsumer>();
    auto binding = ConnectionWorkBinding::create(
        scheduler, 0, InboxConsumer::run, consumer);
    REQUIRE(binding != nullptr);
    auto budget = std::make_shared<DatagramStorageBudget>(inbox_bytes(4));
    consumer->inbox = make_inbox(budget, binding);
    InboxGateRelease release {gate};
    REQUIRE_EQ(
        scheduler->submit(0, {.function = InboxGate::run, .context = gate}),
        RuntimeScheduler::SubmitStatus::accepted);
    gate->wait();
    REQUIRE_EQ(scheduler->submit(0, {.function = [](void*) noexcept { }}),
        RuntimeScheduler::SubmitStatus::accepted);
    REQUIRE_EQ(scheduler->submit(0, {.function = [](void*) noexcept { }}),
        RuntimeScheduler::SubmitStatus::full);
    REQUIRE_EQ(
        scheduler
            ->schedule_at(0,
                std::chrono::steady_clock::now() + std::chrono::hours {1},
                {.function = [](void*) noexcept { }})
            .status,
        RuntimeScheduler::SubmitStatus::accepted);
    for (std::byte tag : {std::byte {1}, std::byte {2}, std::byte {3}}) {
        REQUIRE_EQ(consumer->inbox->publish(consumer->inbox->token(),
                       inbox_wire(tag).view(), inbox_peer, 1),
            InboxStatus::accepted);
    }
    REQUIRE_EQ(consumer->inbox->publish(consumer->inbox->token(),
                   inbox_wire({}, true).view(), inbox_peer, 1),
        InboxStatus::accepted);
    REQUIRE_EQ(scheduler->snapshot().services_pending, 1U);
    gate->release();
    consumer->wait(4);
    binding->retire();
    scheduler->stop();
    REQUIRE(consumer->valid);
    REQUIRE_EQ(consumer->received[0], std::byte {1});
    REQUIRE_EQ(consumer->received[1], std::byte {2});
    REQUIRE_EQ(consumer->received[2], std::byte {3});
    REQUIRE_EQ(consumer->received[3], std::byte {0});
    REQUIRE_EQ(consumer->turns, 4U);
}

TEST(connection_datagram_inbox_publication_during_consumer_turn_is_not_lost)
{
    auto scheduler = inbox_scheduler(1);
    auto gate = std::make_shared<InboxGate>();
    auto consumer = std::make_shared<InboxConsumer>();
    consumer->first_gate = gate;
    auto binding = ConnectionWorkBinding::create(
        scheduler, 0, InboxConsumer::run, consumer);
    REQUIRE(binding != nullptr);
    auto budget = std::make_shared<DatagramStorageBudget>(inbox_bytes(4));
    consumer->inbox = make_inbox(budget, binding);
    InboxGateRelease release {gate};
    REQUIRE_EQ(consumer->inbox->publish(consumer->inbox->token(),
                   inbox_wire(std::byte {1}).view(), inbox_peer, 1),
        InboxStatus::accepted);
    gate->wait();
    REQUIRE_EQ(consumer->inbox->publish(consumer->inbox->token(),
                   inbox_wire(std::byte {2}).view(), inbox_peer, 2),
        InboxStatus::accepted);
    gate->release();
    consumer->wait(2);
    binding->retire();
    scheduler->stop();
    REQUIRE(consumer->valid);
    REQUIRE_EQ(consumer->received[0], std::byte {1});
    REQUIRE_EQ(consumer->received[1], std::byte {2});
}

TEST(connection_datagram_inbox_close_discards_pending_work_without_rearming)
{
    auto scheduler = inbox_scheduler(1);
    auto gate = std::make_shared<InboxGate>();
    auto binding = inbox_binding(scheduler);
    auto budget = std::make_shared<DatagramStorageBudget>(inbox_bytes(4));
    auto inbox = make_inbox(budget, binding);
    InboxGateRelease release {gate};
    REQUIRE_EQ(
        scheduler->submit(0, {.function = InboxGate::run, .context = gate}),
        RuntimeScheduler::SubmitStatus::accepted);
    gate->wait();
    REQUIRE_EQ(inbox->publish(inbox->token(), inbox_wire(std::byte {1}).view(),
                   inbox_peer, 1),
        InboxStatus::accepted);
    auto close = std::async(std::launch::async, [inbox] {
        inbox->close();
    });
    REQUIRE_EQ(
        close.wait_for(std::chrono::seconds {2}), std::future_status::ready);
    close.get();
    DatagramEnvelope envelope;
    REQUIRE_EQ(inbox->pop(inbox->token(), envelope, 2), InboxStatus::closed);
    REQUIRE_EQ(inbox->rearm(inbox->token()), InboxStatus::closed);
    REQUIRE_EQ(inbox->snapshot().queued, 0U);
    gate->release();
    scheduler->stop();
}

TEST(connection_datagram_inbox_concurrent_creation_never_overcommits_storage)
{
    auto scheduler = inbox_scheduler();
    auto binding = inbox_binding(scheduler);
    auto budget = std::make_shared<DatagramStorageBudget>(inbox_bytes(4));
    std::array<std::future<std::shared_ptr<ConnectionDatagramInbox>>, 8>
        creates;
    for (auto& create : creates) {
        create = std::async(std::launch::async, [=] {
            return ConnectionDatagramInbox::create(budget, binding, inbox_peer,
                {.capacity = 4, .control_reserve = 1});
        });
    }
    std::array<std::shared_ptr<ConnectionDatagramInbox>, 8> inboxes;
    std::size_t accepted = 0;
    for (std::size_t index = 0; index < creates.size(); ++index) {
        inboxes[index] = creates[index].get();
        accepted += inboxes[index] != nullptr ? 1U : 0U;
    }
    REQUIRE_EQ(accepted, 1U);
    REQUIRE_EQ(budget->reserved_bytes(), inbox_bytes(4));
    inboxes.fill(nullptr);
    REQUIRE_EQ(budget->reserved_bytes(), 0U);
    scheduler->stop();
}

TEST(connection_datagram_inbox_rearm_and_stopped_scheduler_fail_closed)
{
    auto scheduler = inbox_scheduler();
    auto binding = inbox_binding(scheduler);
    auto budget = std::make_shared<DatagramStorageBudget>(inbox_bytes(8));
    auto inbox = make_inbox(budget, binding);
    REQUIRE_EQ(inbox->publish(inbox->token(), inbox_wire(std::byte {1}).view(),
                   inbox_peer, 1),
        InboxStatus::accepted);
    binding->retire();
    REQUIRE_EQ(inbox->rearm(inbox->token()), InboxStatus::wake_failed);
    REQUIRE_EQ(inbox->snapshot().queued, 0U);
    auto next_binding = inbox_binding(scheduler);
    auto next = make_inbox(budget, next_binding);
    auto stale = next->token();
    stale.service = binding->token();
    REQUIRE_EQ(
        next->publish(stale, inbox_wire(std::byte {2}).view(), inbox_peer, 2),
        InboxStatus::stale);
    scheduler->stop();
    REQUIRE_EQ(next->publish(next->token(), inbox_wire(std::byte {2}).view(),
                   inbox_peer, 2),
        InboxStatus::wake_failed);
    REQUIRE(next->snapshot().closed);
}

TEST(connection_inbox_process_budget_bounds_independent_channel_budgets)
{
    const auto bytes = *ConnectionDatagramInbox::storage_bytes(4);
    auto scheduler = inbox_scheduler();
    auto first_binding = inbox_binding(scheduler);
    auto second_binding = inbox_binding(scheduler);
    auto process = std::make_shared<DatagramStorageBudget>(bytes);
    auto first_budget = std::make_shared<DatagramStorageBudget>(bytes);
    auto second_budget = std::make_shared<DatagramStorageBudget>(bytes);
    auto first = ConnectionDatagramInbox::create(first_budget, first_binding,
        inbox_peer, {.capacity = 4, .control_reserve = 1}, process);
    REQUIRE(first != nullptr);
    REQUIRE_EQ(first_budget->reserved_bytes(), bytes);
    REQUIRE_EQ(process->reserved_bytes(), bytes);
    auto second = ConnectionDatagramInbox::create(second_budget, second_binding,
        inbox_peer, {.capacity = 4, .control_reserve = 1}, process);
    REQUIRE(second == nullptr);
    REQUIRE_EQ(second_budget->reserved_bytes(), 0U);
    first->close();
    REQUIRE_EQ(process->reserved_bytes(), bytes);
    first.reset();
    REQUIRE_EQ(first_budget->reserved_bytes(), 0U);
    REQUIRE_EQ(process->reserved_bytes(), 0U);
    second = ConnectionDatagramInbox::create(second_budget, second_binding,
        inbox_peer, {.capacity = 4, .control_reserve = 1}, process);
    REQUIRE(second != nullptr);
    second.reset();
    REQUIRE_EQ(second_budget->reserved_bytes(), 0U);
    REQUIRE_EQ(process->reserved_bytes(), 0U);
    scheduler->stop();
}

TEST(
    connection_inbox_process_budget_channel_rejection_and_invalid_config_leave_no_charge)
{
    const auto bytes = *ConnectionDatagramInbox::storage_bytes(4);
    auto scheduler = inbox_scheduler();
    auto binding = inbox_binding(scheduler);
    auto process = std::make_shared<DatagramStorageBudget>(bytes);
    auto small = std::make_shared<DatagramStorageBudget>(bytes - 1);
    REQUIRE(ConnectionDatagramInbox::create(small, binding, inbox_peer,
                {.capacity = 4, .control_reserve = 1}, process)
        == nullptr);
    REQUIRE_EQ(small->reserved_bytes(), 0U);
    REQUIRE_EQ(process->reserved_bytes(), 0U);
    auto budget = std::make_shared<DatagramStorageBudget>(bytes);
    REQUIRE(ConnectionDatagramInbox::create(budget, binding, inbox_peer,
                {.capacity = 4, .control_reserve = 5}, process)
        == nullptr);
    REQUIRE(ConnectionDatagramInbox::create(budget, nullptr, inbox_peer,
                {.capacity = 4, .control_reserve = 1}, process)
        == nullptr);
    REQUIRE_EQ(budget->reserved_bytes(), 0U);
    REQUIRE_EQ(process->reserved_bytes(), 0U);
    scheduler->stop();
}

TEST(connection_inbox_identical_channel_and_process_budget_is_charged_once)
{
    const auto bytes = *ConnectionDatagramInbox::storage_bytes(4);
    auto scheduler = inbox_scheduler();
    auto binding = inbox_binding(scheduler);
    auto budget = std::make_shared<DatagramStorageBudget>(bytes);
    auto inbox = ConnectionDatagramInbox::create(budget, binding, inbox_peer,
        {.capacity = 4, .control_reserve = 1}, budget);
    REQUIRE(inbox != nullptr);
    REQUIRE_EQ(budget->reserved_bytes(), bytes);
    inbox.reset();
    REQUIRE_EQ(budget->reserved_bytes(), 0U);
    scheduler->stop();
}

TEST(connection_inbox_concurrent_channel_admission_respects_one_process_ring)
{
    const auto bytes = *ConnectionDatagramInbox::storage_bytes(4);
    auto scheduler = inbox_scheduler();
    auto first_binding = inbox_binding(scheduler);
    auto second_binding = inbox_binding(scheduler);
    auto process = std::make_shared<DatagramStorageBudget>(bytes);
    auto first_budget = std::make_shared<DatagramStorageBudget>(bytes);
    auto second_budget = std::make_shared<DatagramStorageBudget>(bytes);
    std::barrier start(3);
    auto create = [&](const auto& budget, const auto& binding) {
        start.arrive_and_wait();
        return ConnectionDatagramInbox::create(budget, binding, inbox_peer,
            {.capacity = 4, .control_reserve = 1}, process);
    };
    auto first_result = std::async(std::launch::async, [&] {
        return create(first_budget, first_binding);
    });
    auto second_result = std::async(std::launch::async, [&] {
        return create(second_budget, second_binding);
    });
    start.arrive_and_wait();
    auto first = first_result.get();
    auto second = second_result.get();
    REQUIRE((first != nullptr) != (second != nullptr));
    REQUIRE_EQ(first_budget->reserved_bytes() + second_budget->reserved_bytes(),
        bytes);
    REQUIRE_EQ(process->reserved_bytes(), bytes);
    first.reset();
    second.reset();
    REQUIRE_EQ(first_budget->reserved_bytes(), 0U);
    REQUIRE_EQ(second_budget->reserved_bytes(), 0U);
    REQUIRE_EQ(process->reserved_bytes(), 0U);
    scheduler->stop();
}
