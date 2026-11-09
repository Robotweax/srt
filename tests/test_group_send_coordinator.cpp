#include "test.hpp"
#include "compat/group_send_coordinator.hpp"
#include "compat/group_registry.hpp"
#include "robotweax/srt/control.hpp"
#include "compat/hybrid/hybrid_bridge.hpp"
#include "robotweax/srt/handshake_datagram.hpp"

#include <atomic>
#include <condition_variable>
#include <future>
#include <limits>
#include <thread>

using namespace robotweax::srt;
using namespace robotweax::srt::compat;
using namespace std::chrono_literals;

namespace robotweax::srt::compat {
#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_HYBRID_CONTROL
struct HybridBridgeTestAccess {
    static void install(
        ConnectionRuntime& r, const std::shared_ptr<hybrid::NativeService>& s)
    {
        std::lock_guard lock(r.mutex_);
        r.hybrid_service_ = s;
        r.hybrid_attempted_ = true;
    }
    static std::unique_lock<std::mutex> hold(ConnectionRuntime& r)
    {
        return std::unique_lock(r.mutex_);
    }
    static void stale_generation(ConnectionRuntime& r)
    {
        std::lock_guard lock(r.mutex_);
        ++r.group_send_state_->generation;
    }
};
#endif
struct GroupSendCoordinatorTestAccess {
    static GroupPacketEncoder::Snapshot encoder(GroupSendCoordinator& actor)
    {
        std::lock_guard lock(actor.publication_);
        return actor.encoder_->snapshot();
    }
    static void source_revision(
        GroupSendCoordinator& actor, std::uint64_t value)
    {
        std::lock_guard lock(actor.publication_);
        actor.state_.source_revision = value;
    }
    static void fail(GroupSendCoordinator& actor)
    {
        std::lock_guard lock(actor.publication_);
        actor.fail();
    }
    static std::shared_ptr<GroupUnicastBudget> budget(
        GroupSendCoordinator& actor)
    {
        std::lock_guard lock(actor.publication_);
        return actor.unicast_budget_;
    }
    static void revision(
        GroupSendCoordinator& actor, std::uint64_t id, std::uint64_t revision)
    {
        std::lock_guard lock(actor.publication_);
        for (auto& m : actor.members_)
            if (m.state.id == id)
                m.state.revision = revision;
    }
    static GroupRuntimeAdmission::Binding binding(
        GroupSendCoordinator& actor, std::uint64_t id)
    {
        std::lock_guard lock(actor.publication_);
        for (auto& m : actor.members_)
            if (m.state.id == id)
                return m.binding;
        return {};
    }
};
}
namespace {
using Actor = GroupSendCoordinator;
using Status = Actor::Status;
using Admission = GroupRuntimeAdmission;
using Clock = Actor::Clock;
constexpr std::uint32_t group_id = group_handle_mask | 107U;
constexpr std::uint64_t generation = 71;
constexpr SequenceNumber initial {1'000};
constexpr std::array<std::byte, 3> payload {
    std::byte {'a'}, std::byte {'b'}, std::byte {'c'}};
struct Output {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::vector<std::byte>> packets;
    Error error = Error::none;
    bool partial = false;
    unsigned data_failures = 0;
    static UdpIoResult send(
        std::span<const std::byte> bytes, IpEndpoint, void* p) noexcept
    {
        auto& self = *static_cast<Output*>(p);
        std::lock_guard lock(self.mutex);
        self.packets.emplace_back(bytes.begin(), bytes.end());
        self.changed.notify_all();
        if (self.data_failures
            && decode_packet(bytes).packet.kind == PacketKind::data) {
            --self.data_failures;
            return {.error = Error::would_block};
        }
        if (self.error != Error::none)
            return {.error = self.error};
        return {.bytes_transferred =
                    bytes.size() - static_cast<std::size_t>(self.partial)};
    }
    std::size_t size()
    {
        std::lock_guard lock(mutex);
        return packets.size();
    }
};
struct Fixture {
    Clock::time_point origin = Clock::now();
    Clock::time_point now = origin + 5ms;
    Output output;
    std::shared_ptr<DatagramChannel> channel =
        std::make_shared<DatagramChannel>();
    std::shared_ptr<SharedTransmitKeySchedule> schedule;
    Actor::Configuration config;
    std::shared_ptr<Actor> actor;
    explicit Fixture(CryptoMode mode = CryptoMode::automatic,
        std::size_t capacity = 4, SequenceNumber start = initial,
        std::size_t unicast_members = 4)
    {
        channel->set_send_hook_for_testing(Output::send, &output);
        if (mode != CryptoMode::automatic) {
            schedule = std::make_shared<SharedTransmitKeySchedule>(
                SharedTransmitKeySchedule::Configuration {.mode = mode,
                    .refresh_rate_packets = 4,
                    .preannouncement_packets = 1,
                    .origin = start});
            REQUIRE_EQ(schedule->initialize(), Error::none);
        }
        config = {
            .source = {.channel = channel,
                .destination = {.address = {239, 255, 133, 11}, .port = 6000},
                .group_id = group_id,
                .generation = generation,
                .initial_sequence = start,
                .capacity = capacity,
                .wire_bytes_per_second = 1'000'000,
                .clock =
                    [](void* p) noexcept {
                        return *static_cast<Clock::time_point*>(p);
                    },
                .clock_context = &now},
            .timestamp_origin = origin,
            .key_schedule = schedule,
            .member_capacity = 4,
            .maximum_busy_attempts = 2,
            .maximum_unicast_members = unicast_members};
        recreate();
    }
    void recreate()
    {
        actor = Actor::create_for_testing(config);
        REQUIRE(actor);
    }
    Status send(Clock::time_point deadline = Clock::time_point::max())
    {
        return actor->send(payload, PacketTimestamp {5'000}, deadline);
    }
    void pump()
    {
        actor->pump_for_testing();
    }
};
struct Receiver {
    Output output;
    std::uint64_t now = 5'000;
    const IpEndpoint peer {.address = {192, 0, 2, 107}, .port = 14907};
    std::shared_ptr<DatagramChannel> channel =
        std::make_shared<DatagramChannel>();
    std::shared_ptr<CryptoSession> crypto;
    std::unique_ptr<CryptoSession> decryptor;
    std::shared_ptr<ConnectionRuntime> runtime;
    std::uint64_t id = 0;
    std::uint32_t ack_serial = 1;
    std::vector<std::vector<std::byte>> data_attempts;
    std::size_t control_attempts = 0;
    explicit Receiver(Fixture& f, std::size_t capacity = 4, bool add = true,
        SequenceNumber start = initial, bool realtime = false,
        std::int64_t rate = -1)
    {
        channel->set_send_hook_for_testing(Output::send, &output);
        SocketOptions options;
        REQUIRE_EQ(
            options.set(SocketOption::maximum_bandwidth_bytes_per_second, rate),
            Error::none);
        REQUIRE_EQ(options.set(SocketOption::send_buffer_packets,
                       static_cast<std::int64_t>(capacity)),
            Error::none);
        REQUIRE_EQ(
            options.set(SocketOption::receive_buffer_packets, 4), Error::none);
        if (f.schedule) {
            CryptoConfiguration c {.passphrase = "coordinator rotation fixture",
                .mode = f.schedule->mode(),
                .enable_aes_gcm = true,
                .key_length = 16,
                .refresh_rate_packets = 4,
                .preannouncement_packets = 1};
            CryptoSession initiator(c);
            crypto = std::make_shared<CryptoSession>(c);
            decryptor = std::make_unique<CryptoSession>(c);
            REQUIRE_EQ(initiator.start_initiator(), Error::none);
            REQUIRE_EQ(crypto->accept_key_material(
                           initiator.pending_key_material(), true),
                Error::none);
            REQUIRE_EQ(crypto->attach_shared_transmit_schedule(f.schedule),
                Error::none);
            REQUIRE_EQ(decryptor->accept_key_material(
                           crypto->pending_key_material(), false),
                Error::none);
            REQUIRE_EQ(crypto->acknowledge_key_material(
                           decryptor->key_material_response(), false),
                Error::none);
        }
        runtime = std::make_shared<ConnectionRuntime>(ConnectionRuntime::Configuration {
            .channel = channel, .peer = peer, .local_socket_id = 107, .peer_socket_id = 108,
            .initial_sequence = start, .flow_window_packets = 1, .peer_flow_window_packets = 1,
            .options = options, .origin = f.origin, .crypto = crypto,
            .now_function = realtime ? nullptr : +[](void* p) noexcept { return *static_cast<std::uint64_t*>(p); },
            .now_context = &now});
        if (add) {
            const auto member = f.actor->add_member(runtime);
            REQUIRE(member);
            id = member->id;
        }
    }
    void ack(SequenceNumber next, bool full = false)
    {
        const auto serial = full ? ack_serial++ : 0U;
        std::array<std::byte, 64> bytes {};
        const auto encoded = encode_acknowledgement_payload(
            {.kind = full ? AcknowledgementKind::full
                          : AcknowledgementKind::lite,
                .acknowledgement_number = serial,
                .next_sequence = next,
                .available_receive_buffer_packets = 4},
            bytes);
        REQUIRE(encoded);
        runtime->process_packet(
            {.kind = PacketKind::control,
                .control = {.type = ControlType::acknowledgement,
                    .type_specific = serial,
                    .destination_socket_id = 107},
                .payload = std::span {bytes}.first(encoded.bytes_written)},
            peer);
    }
    void nak(std::initializer_list<SequenceRange> lost)
    {
        const std::vector<SequenceRange> ranges(lost);
        std::array<std::byte, 64> bytes {};
        const auto encoded = encode_loss_ranges(ranges, bytes);
        REQUIRE(encoded);
        runtime->process_packet(
            {.kind = PacketKind::control,
                .control = {.type = ControlType::negative_acknowledgement,
                    .destination_socket_id = 107},
                .payload = std::span {bytes}.first(encoded.bytes_written)},
            peer);
    }
    std::size_t buffered()
    {
        return runtime->buffer_packet_counts().unacknowledged_send;
    }
    void exchange_keys(bool allow_data = false)
    {
        (void)runtime->poll();
        std::vector<std::vector<std::byte>> packets;
        {
            std::lock_guard lock(output.mutex);
            packets.swap(output.packets);
        }
        for (auto& bytes : packets) {
            const auto p = decode_packet(bytes);
            REQUIRE(p);
            if (p.packet.kind == PacketKind::data) {
                REQUIRE(allow_data);
                data_attempts.push_back(std::move(bytes));
                continue;
            }
            ++control_attempts;
            if (!decryptor || p.packet.control.type != ControlType::user_defined
                || p.packet.control.subtype != key_material_request_subtype)
                continue;
            REQUIRE_EQ(decryptor->accept_key_material(p.packet.payload, false),
                Error::none);
            runtime->process_packet(
                {.kind = PacketKind::control,
                    .control = {.type = ControlType::user_defined,
                        .subtype = key_material_response_subtype,
                        .destination_socket_id = 107},
                    .payload = decryptor->key_material_response()},
                peer);
        }
    }
    Admission::Guard hold(Fixture& f)
    {
        const auto state = f.actor->snapshot();
        return Admission::try_reserve(
            GroupSendCoordinatorTestAccess::binding(*f.actor, id),
            {.sequence = state.next_sequence,
                .message_number = state.next_message,
                .boundary = MessageBoundary::solo,
                .in_order = true,
                .timestamp = PacketTimestamp {5'000},
                .destination_socket_id = group_id},
            payload.size());
    }
};
Actor::Detachment reason(Fixture& f, std::uint64_t id)
{
    std::array<Actor::Member, Actor::maximum_members> rows;
    const auto count = f.actor->members(rows);
    for (std::size_t i = 0; i < count; ++i)
        if (rows[i].id == id)
            return rows[i].detachment;
    throw std::runtime_error("missing member");
}
}

TEST(
    compat_group_coordinator_commits_one_canonical_original_before_wire_and_ack)
{
    Fixture f;
    Receiver a(f), b(f);
    REQUIRE_EQ(f.send(), Status::accepted);
    REQUIRE_EQ(a.buffered(), 1U);
    REQUIRE_EQ(b.buffered(), 1U);
    REQUIRE_EQ(f.output.size(), 0U);
    a.ack(initial.next());
    REQUIRE_EQ(a.buffered(), 1U);
    a.exchange_keys();
    b.exchange_keys();
    f.pump();
    REQUIRE_EQ(f.output.size(), 1U);
    REQUIRE_EQ(f.actor->snapshot().applied_completions, 2U);
    a.ack(initial.next());
    b.ack(initial.next());
    REQUIRE_EQ(a.buffered(), 0U);
    REQUIRE_EQ(b.buffered(), 0U);
    REQUIRE_EQ(
        a.runtime->statistics(false, true).total.sent_unique.packets, 0U);
}
// A cumulative ACK can cover an original whose UDP submission the coordinator
// has not reported to the member yet. It retires the submitted prefix and
// leaves the rest for a later ACK; it never retires an unsubmitted original.
TEST(compat_group_coordinator_ack_ahead_of_fanout_retires_the_submitted_prefix)
{
    Fixture f;
    Receiver a(f);
    REQUIRE_EQ(f.send(), Status::accepted);
    f.pump();
    REQUIRE_EQ(f.output.size(), 1U);
    f.now += 1ms;
    a.now += 1'000;
    REQUIRE_EQ(f.send(), Status::accepted);
    REQUIRE_EQ(a.buffered(), 2U);

    // The second original is accepted but its submission is not applied.
    a.ack(initial.advanced(2));
    REQUIRE_EQ(a.buffered(), 1U);
    a.ack(initial.advanced(2));
    REQUIRE_EQ(a.buffered(), 1U);

    f.pump();
    REQUIRE_EQ(f.output.size(), 2U);
    a.ack(initial.advanced(2));
    REQUIRE_EQ(a.buffered(), 0U);
    REQUIRE_EQ(reason(f, a.id), Actor::Detachment::none);
}

// A loss report can name an original whose UDP submission the coordinator has
// not reported to the member yet. The submitted sequences in the same report
// are repaired; the unsubmitted one is left for the receiver's next report
// and is never repaired before its submission.
TEST(
    compat_group_coordinator_nak_ahead_of_fanout_repairs_the_submitted_sequences)
{
    Fixture f(CryptoMode::automatic, 8);
    Receiver a(f, 8);
    for (unsigned i = 0; i < 2; ++i) {
        REQUIRE_EQ(f.send(), Status::accepted);
        f.pump();
        f.now += 1ms;
        a.now += 1'000;
    }
    REQUIRE_EQ(f.send(), Status::accepted); // Accepted, submission unapplied.
    const auto third = initial.advanced(2);
    const auto repair = [&](std::size_t index) {
        const auto decoded = decode_packet(a.data_attempts[index]);
        REQUIRE(decoded);
        REQUIRE(decoded.packet.data.retransmitted);
        return decoded.packet.data.sequence;
    };

    a.nak({{initial, initial}, {third, third}});
    f.now += 1ms;
    a.now += 1'000;
    a.exchange_keys(true);
    REQUIRE_EQ(a.data_attempts.size(), 1U);
    REQUIRE_EQ(repair(0), initial);

    // Alone, the unsubmitted sequence is accepted and repairs nothing.
    a.nak({{third, third}});
    f.now += 1ms;
    a.now += 1'000;
    a.exchange_keys(true);
    REQUIRE_EQ(a.data_attempts.size(), 1U);
    REQUIRE_EQ(reason(f, a.id), Actor::Detachment::none);

    f.pump();
    REQUIRE_EQ(f.output.size(), 3U);
    a.nak({{third, third}});
    f.now += 1ms;
    a.now += 1'000;
    a.exchange_keys(true);
    REQUIRE_EQ(a.data_attempts.size(), 2U);
    REQUIRE_EQ(repair(1), third);
}

// The consequence of rejecting such ACKs outright: when every ACK reaches the
// sender before the fanout of the newest original, nothing is ever retired
// and a healthy member is detached once its history is full.
TEST(
    compat_group_coordinator_acks_that_always_precede_fanout_keep_a_member_attached)
{
    Fixture f;
    Receiver a(f);
    for (std::uint32_t i = 0; i < 24; ++i) {
        REQUIRE_EQ(f.send(), Status::accepted);
        a.ack(initial.advanced(i + 1));
        f.pump();
        REQUIRE_EQ(reason(f, a.id), Actor::Detachment::none);
        REQUIRE(a.buffered() <= 2U);
        f.now += 1ms;
        a.now += 1'000;
    }
    REQUIRE_EQ(f.output.size(), 24U);
    a.ack(initial.advanced(24));
    REQUIRE_EQ(a.buffered(), 0U);
}

TEST(
    compat_group_coordinator_source_capacity_rejects_before_mutation_and_recovers_readiness)
{
    Fixture f(CryptoMode::automatic, 1);
    Receiver a(f);
    REQUIRE_EQ(f.send(), Status::accepted);
    REQUIRE(!f.actor->snapshot().writable);
    const auto state = f.actor->snapshot();
    REQUIRE_EQ(f.send(), Status::full);
    REQUIRE_EQ(f.actor->snapshot().next_sequence, state.next_sequence);
    REQUIRE_EQ(reason(f, a.id), Actor::Detachment::none);
    REQUIRE_EQ(a.buffered(), 1U);
    f.pump();
    REQUIRE(f.actor->snapshot().writable);
    REQUIRE_EQ(f.actor->snapshot().inflight_frames, 0U);
}
TEST(
    compat_group_coordinator_slow_history_is_detached_without_stalling_healthy_receiver)
{
    Fixture f;
    Receiver slow(f, 1), healthy(f);
    REQUIRE_EQ(f.send(), Status::accepted);
    f.pump();
    healthy.ack(initial.next());
    f.now += 1ms;
    slow.now += 1'000;
    healthy.now += 1'000;
    REQUIRE_EQ(f.send(), Status::accepted);
    f.pump();
    REQUIRE_EQ(reason(f, slow.id), Actor::Detachment::history_full);
    healthy.ack(initial.advanced(2));
    REQUIRE_EQ(healthy.buffered(), 0U);
    REQUIRE_EQ(slow.buffered(), 1U);
    REQUIRE_EQ(f.output.size(), 2U);
}
TEST(
    compat_group_coordinator_busy_admission_is_explicit_and_does_not_block_source)
{
    Fixture f;
    Receiver busy(f), healthy(f);
    auto held = busy.hold(f);
    REQUIRE_EQ(held.status(), Admission::Status::ready);
    auto sent = std::async(std::launch::async, [&] {
        return f.send();
    });
    REQUIRE_EQ(sent.wait_for(2s), std::future_status::ready);
    REQUIRE_EQ(sent.get(), Status::accepted);
    held.release();
    REQUIRE_EQ(reason(f, busy.id), Actor::Detachment::admission_busy);
    f.pump();
    healthy.ack(initial.next());
    REQUIRE_EQ(healthy.buffered(), 0U);
    REQUIRE_EQ(busy.buffered(), 0U);
}
TEST(
    compat_group_coordinator_busy_completion_is_retried_without_blocking_healthy_fanout)
{
    Fixture f;
    Receiver busy(f), healthy(f);
    REQUIRE_EQ(f.send(), Status::accepted);
    auto held = busy.hold(f);
    REQUIRE_EQ(held.status(), Admission::Status::ready);
    auto pumped = std::async(std::launch::async, [&] {
        f.pump();
    });
    REQUIRE_EQ(pumped.wait_for(2s), std::future_status::ready);
    pumped.get();
    REQUIRE_EQ(f.actor->snapshot().applied_completions, 1U);
    healthy.ack(initial.next());
    REQUIRE_EQ(healthy.buffered(), 0U);
    REQUIRE_EQ(f.actor->snapshot().inflight_frames, 1U);
    held.release();
    f.pump();
    busy.ack(initial.next());
    REQUIRE_EQ(busy.buffered(), 0U);
    REQUIRE_EQ(f.actor->snapshot().inflight_frames, 0U);
}
TEST(
    compat_group_coordinator_busy_completion_budget_releases_frame_and_records_detachment)
{
    Fixture f;
    Receiver busy(f), healthy(f);
    REQUIRE_EQ(f.send(), Status::accepted);
    auto held = busy.hold(f);
    REQUIRE_EQ(held.status(), Admission::Status::ready);
    auto pumped = std::async(std::launch::async, [&] {
        f.pump();
        f.pump();
    });
    REQUIRE_EQ(pumped.wait_for(2s), std::future_status::ready);
    pumped.get();
    held.release();
    REQUIRE_EQ(f.actor->snapshot().inflight_frames, 0U);
    REQUIRE_EQ(reason(f, busy.id), Actor::Detachment::completion_busy);
    healthy.ack(initial.next());
    f.now += 1ms;
    healthy.now += 1'000;
    REQUIRE_EQ(f.send(), Status::accepted);
    f.pump();
    healthy.ack(initial.advanced(2));
    REQUIRE_EQ(healthy.buffered(), 0U);
    busy.ack(initial.next());
    REQUIRE_EQ(busy.buffered(), 1U);
}
TEST(compat_group_coordinator_closed_member_cannot_cancel_accepted_multicast)
{
    Fixture f;
    Receiver a(f), b(f);
    REQUIRE_EQ(f.send(), Status::accepted);
    a.runtime->close();
    f.pump();
    REQUIRE_EQ(f.output.size(), 1U);
    REQUIRE_EQ(reason(f, a.id), Actor::Detachment::stale);
    b.ack(initial.next());
    REQUIRE_EQ(b.buffered(), 0U);
}
TEST(compat_group_coordinator_failed_or_expired_source_never_authorizes_history)
{
    for (unsigned scenario = 0; scenario < 4; ++scenario) {
        Fixture f;
        Receiver a(f);
        const auto deadline = f.now + 1ms;
        REQUIRE_EQ(f.send(deadline), Status::accepted);
        if (scenario == 0)
            f.now = deadline;
        if (scenario == 1)
            f.output.error = Error::io_error;
        if (scenario == 2)
            f.output.partial = true;
        if (scenario == 3)
            f.output.error = Error::would_block;
        for (unsigned i = 0; i < 9; ++i) {
            f.pump();
            f.now += 1ms;
            a.now += 1'000;
        }
        a.ack(initial.next());
        REQUIRE_EQ(a.buffered(), 1U);
        REQUIRE_EQ(f.actor->snapshot().applied_completions, 0U);
        REQUIRE_EQ(f.actor->snapshot().inflight_frames, 0U);
        REQUIRE_EQ(reason(f, a.id), Actor::Detachment::source_not_submitted);
        REQUIRE_EQ(f.actor->snapshot().failed, scenario == 1 || scenario == 2);
    }
}
TEST(
    compat_group_coordinator_empty_timeline_wraps_and_late_join_uses_current_cursor)
{
    for (auto mode :
        {CryptoMode::automatic, CryptoMode::aes_ctr, CryptoMode::aes_gcm}) {
        Fixture f(mode, 4, SequenceNumber {SequenceNumber::mask});
        f.actor->close();
        f.config.initial_message = 0x03ff'ffffU;
        f.recreate();
        REQUIRE_EQ(f.send(f.now), Status::expired);
        REQUIRE_EQ(f.send(), Status::advanced_without_receivers);
        REQUIRE_EQ(f.actor->snapshot().next_sequence, SequenceNumber {0});
        REQUIRE_EQ(f.actor->snapshot().next_message, 1U);
        for (unsigned i = 0; i < 8; ++i)
            REQUIRE_EQ(f.send(), Status::advanced_without_receivers);
        REQUIRE_EQ(f.output.size(), 0U);
        if (f.schedule)
            REQUIRE_EQ(f.schedule->frontier_position(), 9U);
        Receiver stale(f, 4, false, SequenceNumber {SequenceNumber::mask});
        REQUIRE(!f.actor->add_member(stale.runtime));
        REQUIRE(stale.runtime->writable());
        Receiver joined(f, 4, true, SequenceNumber {8});
        REQUIRE_EQ(f.send(), Status::accepted);
        f.pump();
        joined.ack(SequenceNumber {9});
        REQUIRE_EQ(joined.buffered(), 0U);
    }
}
TEST(
    compat_group_coordinator_invalid_input_and_expired_reservation_do_not_detach_members)
{
    Fixture f;
    Receiver a(f);
    REQUIRE_EQ(f.actor->send({}, PacketTimestamp {}), Status::invalid);
    std::array<std::byte, maximum_data_payload_size + 1> large {};
    REQUIRE_EQ(f.actor->send(large, PacketTimestamp {}), Status::invalid);
    REQUIRE_EQ(f.send(f.now), Status::expired);
    REQUIRE_EQ(f.actor->snapshot().next_sequence, initial);
    REQUIRE_EQ(reason(f, a.id), Actor::Detachment::none);
    REQUIRE_EQ(a.buffered(), 0U);
}
TEST(
    compat_group_coordinator_membership_capacity_removal_and_stale_results_are_bounded)
{
    Fixture f;
    Receiver a(f), b(f), c(f), d(f), extra(f, 4, false);
    REQUIRE(!f.actor->add_member(extra.runtime));
    REQUIRE(!f.actor->add_member(a.runtime));
    REQUIRE_EQ(f.send(), Status::accepted);
    f.actor->remove_member(a.id);
    // Replacement at the advertised cursor gets a fresh identity and cannot
    // inherit the removed member's captured completion.
    Receiver replacement(f, 4, true, initial.next());
    REQUIRE(replacement.id > d.id);
    f.pump();
    REQUIRE_EQ(replacement.buffered(), 0U);
    REQUIRE_EQ(reason(f, replacement.id), Actor::Detachment::none);
    REQUIRE_EQ(f.actor->snapshot().applied_completions, 3U);
    a.ack(initial.next());
    REQUIRE_EQ(a.buffered(), 1U);
}
TEST(compat_group_coordinator_close_cancels_pending_work_and_is_idempotent)
{
    Fixture f;
    Receiver a(f);
    REQUIRE_EQ(f.send(), Status::accepted);
    f.actor->close();
    f.actor->close();
    f.pump();
    REQUIRE_EQ(f.output.size(), 0U);
    REQUIRE(f.actor->snapshot().closed);
    REQUIRE(!f.actor->snapshot().writable);
    REQUIRE_EQ(f.actor->snapshot().inflight_frames, 0U);
    REQUIRE_EQ(f.send(), Status::closed);
    REQUIRE(!f.actor->add_member(a.runtime));
    REQUIRE(!a.runtime->broken());
    a.ack(initial.next());
    REQUIRE_EQ(a.buffered(), 1U);
}
TEST(
    compat_group_coordinator_ctr_gcm_rotation_preserves_canonical_bytes_and_unicast_km)
{
    for (auto mode : {CryptoMode::aes_ctr, CryptoMode::aes_gcm}) {
        Fixture f(mode);
        Receiver a(f), b(f);
        for (unsigned i = 0; i < 20; ++i) {
            REQUIRE_EQ(f.send(), Status::accepted);
            a.exchange_keys();
            b.exchange_keys();
            f.pump();
            std::vector<std::byte> bytes;
            {
                std::lock_guard lock(f.output.mutex);
                bytes = f.output.packets.back();
            }
            const auto decoded = decode_packet(bytes);
            REQUIRE(decoded);
            for (auto* receiver : {&a, &b}) {
                std::array<std::byte, payload.size()> clear {};
                if (mode == CryptoMode::aes_gcm)
                    REQUIRE_EQ(receiver->decryptor->open(decoded.packet.data,
                                   decoded.packet.payload.first(payload.size()),
                                   decoded.packet.payload.last(
                                       srt_gcm_authentication_tag_size),
                                   clear),
                        Error::none);
                else {
                    REQUIRE_EQ(receiver->decryptor->decrypt(
                                   decoded.packet.data.encryption_key,
                                   decoded.packet.data.sequence,
                                   decoded.packet.payload, clear),
                        Error::none);
                    receiver->decryptor->note_accepted_receive_sequence(
                        decoded.packet.data.sequence);
                }
                REQUIRE_EQ(clear, payload);
                receiver->ack(initial.advanced(i + 1));
                REQUIRE_EQ(receiver->buffered(), 0U);
                receiver->now += 1'000;
            }
            f.now += 1ms;
        }
        REQUIRE_EQ(f.actor->snapshot().applied_completions, 40U);
        REQUIRE_EQ(f.actor->snapshot().detached_members, 0U);
    }
}
TEST(
    compat_group_coordinator_unsent_key_announcement_detaches_only_affected_receiver)
{
    Fixture f(CryptoMode::aes_gcm, 8);
    Receiver stalled(f, 8), healthy(f, 8);
    for (unsigned i = 0; i < 4; ++i) {
        REQUIRE_EQ(f.send(), Status::accepted);
        healthy.exchange_keys();
        f.pump();
        healthy.ack(initial.advanced(i + 1));
        stalled.ack(initial.advanced(i + 1));
        f.now += 1ms;
        stalled.now += 1'000;
        healthy.now += 1'000;
    }
    REQUIRE_EQ(f.send(), Status::accepted);
    REQUIRE_EQ(reason(f, stalled.id), Actor::Detachment::admission_busy);
    REQUIRE_EQ(reason(f, healthy.id), Actor::Detachment::none);
    healthy.exchange_keys();
    f.pump();
    healthy.ack(initial.advanced(5));
    REQUIRE_EQ(healthy.buffered(), 0U);
}

TEST(
    compat_group_coordinator_key_horizon_stops_before_overwriting_unsent_originals)
{
    for (auto mode : {CryptoMode::aes_ctr, CryptoMode::aes_gcm}) {
        Fixture f(mode, 32);
        Receiver a(f, 32);
        for (unsigned i = 0; i < 7; ++i) {
            REQUIRE_EQ(f.send(), Status::accepted);
            a.exchange_keys();
        }
        const auto revision = a.crypto->key_material_revision();
        REQUIRE(!f.actor->snapshot().writable);
        REQUIRE_EQ(f.send(), Status::full);
        REQUIRE_EQ(a.crypto->key_material_revision(), revision);
        REQUIRE_EQ(a.buffered(), 7U);
        REQUIRE_EQ(f.actor->snapshot().next_sequence, initial.advanced(7));
        REQUIRE_EQ(reason(f, a.id), Actor::Detachment::none);
        // Deliver the backlog only after every accepted KM announcement has
        // already reached the receiver, reproducing a faster control path.
        for (unsigned i = 0; i < 7; ++i) {
            f.pump();
            std::vector<std::byte> bytes;
            {
                std::lock_guard lock(f.output.mutex);
                bytes = f.output.packets.at(i);
            }
            const auto p = decode_packet(bytes);
            REQUIRE(p);
            std::array<std::byte, payload.size()> clear {};
            if (mode == CryptoMode::aes_gcm)
                REQUIRE_EQ(
                    a.decryptor->open(p.packet.data,
                        p.packet.payload.first(payload.size()),
                        p.packet.payload.last(srt_gcm_authentication_tag_size),
                        clear),
                    Error::none);
            else {
                REQUIRE_EQ(a.decryptor->decrypt(p.packet.data.encryption_key,
                               p.packet.data.sequence, p.packet.payload, clear),
                    Error::none);
                a.decryptor->note_accepted_receive_sequence(
                    p.packet.data.sequence);
            }
            REQUIRE_EQ(clear, payload);
            a.ack(initial.advanced(i + 1));
            f.now += 1ms;
            a.now += 1'000;
        }
        REQUIRE(f.actor->snapshot().writable);
        REQUIRE_EQ(a.buffered(), 0U);
        REQUIRE_EQ(f.send(), Status::accepted);
        a.exchange_keys();
        f.pump();
        a.ack(initial.advanced(8));
        REQUIRE_EQ(a.buffered(), 0U);
    }
}

// Members whose runtimes are already closed have left the group; finding that
// out during a send is the same as finding the group empty.
TEST(compat_group_coordinator_all_unavailable_members_advance_without_emission)
{
    Fixture f;
    Receiver a(f);
    a.runtime->close();
    REQUIRE_EQ(f.send(), Status::advanced_without_receivers);
    REQUIRE_EQ(f.actor->snapshot().next_sequence, initial.next());
    REQUIRE_EQ(reason(f, a.id), Actor::Detachment::closed);
    f.pump();
    REQUIRE_EQ(f.output.size(), 0U);
}

// A send that detaches its last members while their sessions are still
// established reports the loss. It consumes no sequence, message number or
// key position, so the next emitted original still carries the number those
// multicast listeners expect. A closed member alongside does not change that.
TEST(
    compat_group_coordinator_send_losing_its_last_connected_members_fails_and_consumes_nothing)
{
    for (auto mode :
        {CryptoMode::automatic, CryptoMode::aes_ctr, CryptoMode::aes_gcm}) {
        Fixture f(mode);
        Receiver slow(f, 1), gone(f);
        slow.exchange_keys();
        gone.exchange_keys();
        REQUIRE_EQ(f.send(), Status::accepted);
        f.pump();
        REQUIRE_EQ(f.output.size(), 1U);
        gone.runtime->close();
        f.now += 1ms;
        slow.now += 1'000;
        const auto message = f.actor->snapshot().next_message;

        const auto key_frontier =
            f.schedule ? f.schedule->frontier_position() : 0U;

        // The unacknowledged original fills the slow member's history.
        REQUIRE_EQ(f.send(), Status::receivers_lost);
        REQUIRE_EQ(f.actor->snapshot().next_sequence, initial.next());
        REQUIRE_EQ(f.actor->snapshot().next_message, message);
        REQUIRE_EQ(f.actor->snapshot().active_members, 0U);
        REQUIRE_EQ(reason(f, slow.id), Actor::Detachment::history_full);
        REQUIRE_EQ(reason(f, gone.id), Actor::Detachment::closed);
        if (f.schedule)
            REQUIRE_EQ(f.schedule->frontier_position(), key_frontier);
        f.pump();
        REQUIRE_EQ(f.output.size(), 1U);
        REQUIRE(!f.actor->snapshot().failed);

        // The group is empty now, which is the documented unused advance.
        REQUIRE_EQ(f.send(), Status::advanced_without_receivers);
        REQUIRE_EQ(f.actor->snapshot().next_sequence, initial.advanced(2));
        if (f.schedule)
            REQUIRE_EQ(f.schedule->frontier_position(), 2U);
        f.pump();
        REQUIRE_EQ(f.output.size(), 1U);
        f.actor->close();
    }
}

// One member lost during a send does not fail it while another remains.
TEST(compat_group_coordinator_send_losing_one_of_two_members_is_accepted)
{
    Fixture f;
    Receiver slow(f, 1), healthy(f);
    REQUIRE_EQ(f.send(), Status::accepted);
    f.pump();
    healthy.ack(initial.next());
    f.now += 1ms;
    slow.now += 1'000;
    healthy.now += 1'000;
    REQUIRE_EQ(f.send(), Status::accepted);
    REQUIRE_EQ(f.actor->snapshot().next_sequence, initial.advanced(2));
    REQUIRE_EQ(reason(f, slow.id), Actor::Detachment::history_full);
    REQUIRE_EQ(reason(f, healthy.id), Actor::Detachment::none);
    f.pump();
    REQUIRE_EQ(f.output.size(), 2U);
}

TEST(compat_group_coordinator_retry_exhaustion_never_becomes_submission)
{
    Fixture f;
    f.actor->close();
    f.config.source.maximum_attempts = 2;
    f.recreate();
    Receiver a(f);
    f.output.error = Error::would_block;
    REQUIRE_EQ(f.send(f.now + 1s), Status::accepted);
    f.pump();
    f.now += 1ms;
    a.now += 1'000;
    f.pump();
    REQUIRE_EQ(f.output.size(), 2U);
    REQUIRE_EQ(f.actor->snapshot().inflight_frames, 0U);
    REQUIRE_EQ(f.actor->snapshot().applied_completions, 0U);
    REQUIRE_EQ(reason(f, a.id), Actor::Detachment::source_not_submitted);
    a.ack(initial.next());
    REQUIRE_EQ(a.buffered(), 1U);
}

TEST(
    compat_group_coordinator_small_fanout_turn_preserves_all_frames_until_consumed)
{
    Fixture f;
    f.actor->close();
    f.config.fanout_batch = 1;
    f.recreate();
    Receiver a(f), b(f), c(f), d(f);
    for (unsigned i = 0; i < 4; ++i)
        REQUIRE_EQ(f.send(), Status::accepted);
    REQUIRE_EQ(f.actor->snapshot().inflight_frames, 4U);
    REQUIRE_EQ(f.send(), Status::full);
    for (unsigned i = 0; i < 20; ++i) {
        f.pump();
        f.now += 1ms;
        a.now += 1'000;
        b.now += 1'000;
        c.now += 1'000;
        d.now += 1'000;
    }
    REQUIRE_EQ(f.actor->snapshot().applied_completions, 16U);
    REQUIRE_EQ(f.actor->snapshot().inflight_frames, 0U);
    for (auto* receiver : {&a, &b, &c, &d}) {
        receiver->ack(initial.advanced(4));
        REQUIRE_EQ(receiver->buffered(), 0U);
    }
}

namespace {
struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool released = false;
    static void run(void* p) noexcept
    {
        auto& self = *static_cast<Gate*>(p);
        std::unique_lock lock(self.mutex);
        self.entered = true;
        self.changed.notify_all();
        self.changed.wait(lock, [&] {
            return self.released;
        });
    }
    void release()
    {
        std::lock_guard lock(mutex);
        released = true;
        changed.notify_all();
    }
};
struct Release {
    std::shared_ptr<Gate> gate;
    ~Release()
    {
        gate->release();
    }
};
void noop(void*) noexcept { }
}
TEST(
    compat_group_coordinator_close_serializes_native_submission_and_rejects_later_sends)
{
    Fixture f;
    Receiver a(f);
    struct Blocked {
        std::shared_ptr<Gate> gate;
        Output& output;
        static UdpIoResult send(
            std::span<const std::byte> bytes, IpEndpoint peer, void* p) noexcept
        {
            auto& self = *static_cast<Blocked*>(p);
            Gate::run(self.gate.get());
            return Output::send(bytes, peer, &self.output);
        }
    } blocked {std::make_shared<Gate>(), f.output};
    f.channel->set_send_hook_for_testing(Blocked::send, &blocked);
    REQUIRE_EQ(f.send(), Status::accepted);
    REQUIRE_EQ(a.buffered(), 1U);
    std::future<void> pumping, closing;
    Release cleanup {blocked.gate};
    pumping = std::async(std::launch::async, [&] {
        f.pump();
    });
    {
        std::unique_lock lock(blocked.gate->mutex);
        REQUIRE(blocked.gate->changed.wait_for(lock, 2s, [&] {
            return blocked.gate->entered;
        }));
    }
    std::promise<void> requested;
    auto started = requested.get_future();
    closing = std::async(std::launch::async, [&] {
        requested.set_value();
        f.actor->close();
    });
    REQUIRE_EQ(started.wait_for(2s), std::future_status::ready);
    REQUIRE_EQ(closing.wait_for(10ms), std::future_status::timeout);
    blocked.gate->release();
    REQUIRE_EQ(closing.wait_for(2s), std::future_status::ready);
    closing.get();
    REQUIRE_EQ(pumping.wait_for(2s), std::future_status::ready);
    pumping.get();
    REQUIRE_EQ(f.send(), Status::closed);
    REQUIRE_EQ(f.output.size(), 1U);
    REQUIRE_EQ(f.actor->snapshot().applied_completions, 0U);
    a.ack(initial.next());
    REQUIRE_EQ(a.buffered(), 1U);
}

TEST(
    compat_group_coordinator_pending_frames_and_service_contexts_do_not_pin_actor_or_runtime)
{
    Fixture f;
    Receiver a(f);
    REQUIRE_EQ(f.send(), Status::accepted);
    std::weak_ptr<ConnectionRuntime> runtime = a.runtime;
    a.runtime.reset();
    REQUIRE(runtime.expired());
    std::weak_ptr<Actor> actor = f.actor;
    f.actor.reset();
    REQUIRE(actor.expired());
    REQUIRE_EQ(f.output.size(), 0U);
}

TEST(
    compat_group_coordinator_two_reserved_services_survive_queue_and_timer_saturation)
{
    Fixture f;
    f.actor->close();
    auto pool = std::make_shared<RuntimeScheduler>(
        RuntimeScheduler::Configuration {1, 1, 1, 2});
    REQUIRE(pool->start());
    f.config.source.clock = nullptr;
    f.actor = Actor::create(f.config, pool);
    REQUIRE(f.actor);
    Receiver a(f, 4, true, initial, true);
    auto gate = std::make_shared<Gate>();
    Release cleanup {gate};
    REQUIRE_EQ(pool->submit(0, {Gate::run, gate}),
        RuntimeScheduler::SubmitStatus::accepted);
    {
        std::unique_lock lock(gate->mutex);
        REQUIRE(gate->changed.wait_for(lock, 2s, [&] {
            return gate->entered;
        }));
    }
    REQUIRE_EQ(
        pool->submit(0, {noop, {}}), RuntimeScheduler::SubmitStatus::accepted);
    REQUIRE_EQ(
        pool->submit(0, {noop, {}}), RuntimeScheduler::SubmitStatus::full);
    const auto timer = pool->schedule_at(0, Clock::now() + 10s, {noop, {}});
    REQUIRE_EQ(timer.status, RuntimeScheduler::SubmitStatus::accepted);
    REQUIRE_EQ(pool->schedule_at(0, Clock::now() + 10s, {noop, {}}).status,
        RuntimeScheduler::SubmitStatus::full);
    REQUIRE_EQ(f.send(Clock::now() + 2s), Status::accepted);
    ReadinessObserver observer;
    ReadinessWatch watch(observer, 1, false);
    REQUIRE(watch.bind(f.actor->readiness_source()));
    gate->release();
    const auto deadline = Clock::now() + 2s;
    while (f.actor->snapshot().inflight_frames && Clock::now() < deadline) {
        const auto generation_before = observer.generation();
        if (f.actor->snapshot().inflight_frames)
            observer.wait_until(generation_before, deadline);
    }
    REQUIRE_EQ(f.actor->snapshot().applied_completions, 1U);
    a.ack(initial.next());
    REQUIRE_EQ(a.buffered(), 0U);
    f.actor->close();
    (void)pool->cancel_timer(timer.token);
    pool->stop();
    REQUIRE_EQ(pool->snapshot().services_reserved, 0U);
}
TEST(compat_group_coordinator_factory_rolls_back_partial_service_reservation)
{
    Fixture f;
    auto pool = std::make_shared<RuntimeScheduler>(
        RuntimeScheduler::Configuration {1, 1, 1, 1});
    REQUIRE(pool->start());
    REQUIRE(!Actor::create(f.config, pool));
    REQUIRE_EQ(pool->snapshot().services_reserved, 0U);
    for (unsigned i = 0; i < 6; ++i) {
        auto c = f.config;
        switch (i) {
        case 0:
            c.member_capacity = Actor::maximum_members + 1;
            break;
        case 1:
            c.fanout_batch = 0;
            break;
        case 2:
            c.maximum_busy_attempts = 0;
            break;
        case 3:
            c.initial_message = 0;
            break;
        case 4:
            c.source.wire_bytes_per_second = 0;
            break;
        case 5:
            c.source.generation = 0;
            break;
        }
        REQUIRE(!Actor::create_for_testing(c));
    }
    pool->stop();
}

TEST(
    compat_group_coordinator_busy_grace_retries_before_sealing_and_bounds_stalled_admission)
{
    for (unsigned scenario = 0; scenario < 3; ++scenario) {
        const bool persistent = scenario != 0;
        Fixture f;
        auto admission_time =
            scenario == 2 ? Clock::time_point::max() - 1ms : f.now;
        f.actor->close();
        f.config.admission_busy_grace = 10ms;
        f.config.admission_clock = +[](void* p) noexcept {
            return *static_cast<Clock::time_point*>(p);
        };
        f.config.admission_clock_context = &admission_time;
        f.recreate();
        Receiver a(f);
        auto held = a.hold(f);
        REQUIRE_EQ(held.status(), Admission::Status::ready);
        auto attempt = std::async(std::launch::async, [&] {
            return f.send();
        });
        REQUIRE_EQ(attempt.wait_for(2s), std::future_status::ready);
        REQUIRE_EQ(attempt.get(), Status::busy);
        REQUIRE_EQ(f.actor->snapshot().next_sequence, initial);
        REQUIRE(!f.actor->snapshot().writable);
        REQUIRE_EQ(f.output.size(), 0U);
        if (!persistent)
            held.release();
        f.now += persistent ? 11ms : 1ms;
        admission_time = scenario == 2 ? Clock::time_point::max() : f.now;
        f.pump();
        REQUIRE(f.actor->snapshot().writable);
        attempt = std::async(std::launch::async, [&] {
            return f.send();
        });
        REQUIRE_EQ(attempt.wait_for(2s), std::future_status::ready);
        REQUIRE_EQ(attempt.get(),
            persistent ? Status::receivers_lost : Status::accepted);
        REQUIRE_EQ(f.actor->snapshot().next_sequence,
            persistent ? initial : initial.next());
        held.release();
        REQUIRE_EQ(reason(f, a.id),
            persistent ? Actor::Detachment::admission_busy
                       : Actor::Detachment::none);
        REQUIRE_EQ(a.buffered(), persistent ? 0U : 1U);
        f.now += 1ms;
        a.now += 12'000;
        f.pump();
        REQUIRE_EQ(f.output.size(), persistent ? 0U : 1U);
        f.actor->close();
    }
}

namespace {
using Delivery = GroupOriginalDelivery;
using Transition = Actor::TransitionStatus;
void tick(Fixture& f, Receiver& a, Receiver* b = nullptr)
{
    f.now += 1ms;
    a.now += 1'000;
    if (b)
        b->now += 1'000;
}
void decrypt_original(
    Receiver& a, CryptoMode mode, const std::vector<std::byte>& bytes)
{
    const auto decoded = decode_packet(bytes);
    REQUIRE(decoded);
    REQUIRE_EQ(decoded.packet.data.destination_socket_id, group_id);
    std::array<std::byte, payload.size()> clear {};
    if (mode == CryptoMode::automatic)
        std::copy(decoded.packet.payload.begin(), decoded.packet.payload.end(),
            clear.begin());
    else if (mode == CryptoMode::aes_gcm)
        REQUIRE_EQ(
            a.decryptor->open(decoded.packet.data,
                decoded.packet.payload.first(payload.size()),
                decoded.packet.payload.last(srt_gcm_authentication_tag_size),
                clear),
            Error::none);
    else {
        REQUIRE_EQ(
            a.decryptor->decrypt(decoded.packet.data.encryption_key,
                decoded.packet.data.sequence, decoded.packet.payload, clear),
            Error::none);
        a.decryptor->note_accepted_receive_sequence(
            decoded.packet.data.sequence);
    }
    REQUIRE_EQ(clear, payload);
}
}

TEST(compat_group_policy_modes_persist_and_preserve_canonical_wire_identity)
{
    for (auto mode :
        {CryptoMode::automatic, CryptoMode::aes_ctr, CryptoMode::aes_gcm}) {
        Fixture f(mode, 8);
        Receiver a(f), b(f);
        auto policy = f.actor->set_delivery(a.id, 0, Delivery::unicast);
        REQUIRE_EQ(policy.status, Transition::applied);
        REQUIRE_EQ(policy.member.revision, 1U);
        REQUIRE_EQ(policy.member.effective_sequence, initial);
        REQUIRE_EQ(policy.member.effective_position, 0U);
        REQUIRE_EQ(f.send(), Status::accepted);
        f.pump();
        a.ack(initial.next(), true);
        REQUIRE_EQ(a.buffered(), 1U); // MC cannot credit uni-only.
        b.ack(initial.next(), true);
        REQUIRE_EQ(b.buffered(), 0U);
        a.exchange_keys(true);
        b.exchange_keys();
        REQUIRE_EQ(a.data_attempts.size(), 1U);
        REQUIRE_EQ(a.data_attempts.back(), f.output.packets.back());
        decrypt_original(a, mode, a.data_attempts.back());
        a.ack(initial.next(), true);
        REQUIRE_EQ(a.buffered(), 0U);
        tick(f, a, &b);
        policy = f.actor->set_delivery(a.id, 1, Delivery::overlap);
        REQUIRE_EQ(policy.status, Transition::applied);
        REQUIRE_EQ(policy.member.effective_sequence, initial.next());
        REQUIRE_EQ(f.send(), Status::accepted);
        a.exchange_keys(true);
        b.exchange_keys();
        f.pump();
        REQUIRE_EQ(a.data_attempts.size(), 2U);
        REQUIRE_EQ(a.data_attempts.back(), f.output.packets.back());
        decrypt_original(a, mode, a.data_attempts.back());
        REQUIRE_EQ(f.actor->snapshot().duplicate_completions, 1U);
        a.ack(initial.advanced(2), true);
        b.ack(initial.advanced(2), true);
        tick(f, a, &b);
        policy = f.actor->set_delivery(a.id, 2, Delivery::multicast);
        REQUIRE_EQ(policy.status, Transition::applied);
        REQUIRE_EQ(f.send(), Status::accepted);
        f.pump();
        a.exchange_keys(true);
        b.exchange_keys();
        REQUIRE_EQ(a.data_attempts.size(), 2U);
        REQUIRE_EQ(
            a.runtime->statistics(false, true).total.sent_unique.packets, 2U);
        REQUIRE_EQ(
            b.runtime->statistics(false, true).total.sent_unique.packets, 0U);
        a.ack(initial.advanced(3), true);
        b.ack(initial.advanced(3), true);
        REQUIRE_EQ(a.buffered(), 0U);
        REQUIRE_EQ(f.actor->snapshot().unicast_reservations, 0U);
    }
}

TEST(
    compat_group_policy_queued_retry_keeps_mode_and_budget_across_future_transition)
{
    Fixture f(CryptoMode::aes_gcm, 8);
    f.actor->close();
    f.config.maximum_unicast_members = 1;
    f.recreate();
    Receiver a(f), b(f);
    REQUIRE_EQ(f.actor->set_delivery(a.id, 0, Delivery::unicast).status,
        Transition::applied);
    REQUIRE_EQ(f.send(), Status::accepted);
    a.output.data_failures = 1;
    a.exchange_keys(true);
    REQUIRE_EQ(a.data_attempts.size(), 1U);
    const auto encoded = a.data_attempts[0];
    REQUIRE_EQ(
        a.runtime->statistics(false, true).total.sent_unique.packets, 0U);
    REQUIRE_EQ(f.actor->set_delivery(a.id, 1, Delivery::multicast).status,
        Transition::applied);
    REQUIRE_EQ(f.actor->set_delivery(b.id, 0, Delivery::unicast).status,
        Transition::full);
    f.pump();
    a.ack(initial.next(), true);
    REQUIRE_EQ(a.buffered(), 1U);
    b.ack(initial.next(), true);
    tick(f, a, &b);
    REQUIRE_EQ(f.send(), Status::accepted);
    f.pump();
    a.ack(initial.advanced(2), true);
    REQUIRE_EQ(a.buffered(), 2U);
    a.exchange_keys(true);
    REQUIRE_EQ(a.data_attempts.size(), 2U);
    REQUIRE_EQ(a.data_attempts[1], encoded);
    REQUIRE_EQ(a.data_attempts[1], f.output.packets[0]);
    a.ack(initial.advanced(2), true);
    b.ack(initial.advanced(2), true);
    REQUIRE_EQ(a.buffered(), 0U);
    f.pump();
    REQUIRE_EQ(f.actor->snapshot().unicast_reservations, 0U);
    REQUIRE_EQ(f.actor->set_delivery(b.id, 0, Delivery::unicast).status,
        Transition::applied);
}

TEST(
    compat_group_policy_explicit_prefix_recovery_classifies_unsent_and_submitted_work)
{
    for (auto mode :
        {CryptoMode::automatic, CryptoMode::aes_ctr, CryptoMode::aes_gcm}) {
        Fixture f(mode, 8);
        Receiver a(f), b(f);
        REQUIRE_EQ(f.send(), Status::accepted);
        f.pump();
        b.ack(initial.next(), true);
        tick(f, a, &b);
        REQUIRE_EQ(f.send(),
            Status::accepted); // This multicast original is still queued.
        auto result = f.actor->set_delivery(a.id, 0, Delivery::unicast, true);
        REQUIRE_EQ(result.status, Transition::applied);
        REQUIRE_EQ(result.member.effective_sequence, initial.advanced(2));
        REQUIRE_EQ(result.prefix.repairs, 1U);
        REQUIRE_EQ(result.prefix.originals, 1U);
        REQUIRE(result.prefix_scheduled);
        REQUIRE_EQ(a.buffered(), 2U);
        a.exchange_keys(true);
        REQUIRE_EQ(a.data_attempts.size(), 1U);
        auto first = decode_packet(a.data_attempts[0]);
        REQUIRE(first);
        REQUIRE(first.packet.data.retransmitted);
        decrypt_original(a, mode, a.data_attempts[0]);
        a.ack(initial.next(), true);
        tick(f, a, &b);
        a.exchange_keys(true);
        REQUIRE_EQ(a.data_attempts.size(), 2U);
        auto second = decode_packet(a.data_attempts[1]);
        REQUIRE(second);
        REQUIRE(!second.packet.data.retransmitted);
        decrypt_original(a, mode, a.data_attempts[1]);
        f.pump();
        REQUIRE_EQ(a.data_attempts[1], f.output.packets[1]);
        a.ack(initial.advanced(2), true);
        b.ack(initial.advanced(2), true);
        REQUIRE_EQ(a.buffered(), 0U);
        REQUIRE_EQ(
            a.runtime->statistics(false, true).total.sent_retransmitted.packets,
            1U);
        REQUIRE_EQ(
            a.runtime->statistics(false, true).total.sent_unique.packets, 1U);
        REQUIRE_EQ(
            f.actor->set_delivery(a.id, 0, Delivery::overlap, true).status,
            Transition::stale);
    }
}

TEST(compat_group_policy_revision_member_lifetime_and_budget_checks_are_atomic)
{
    Fixture f;
    f.actor->close();
    f.config.maximum_unicast_members = 1;
    f.recreate();
    Receiver a(f), b(f);
    REQUIRE_EQ(f.actor->set_delivery(a.id, 1, Delivery::unicast).status,
        Transition::stale);
    REQUIRE_EQ(f.actor->set_delivery(a.id, 0, Delivery::multicast).status,
        Transition::unchanged);
    REQUIRE_EQ(
        f.actor->set_delivery(a.id, 0, static_cast<Delivery>(255)).status,
        Transition::unavailable);
    auto held = a.hold(f);
    REQUIRE_EQ(held.status(), Admission::Status::ready);
    auto attempt = std::async(std::launch::async, [&] {
        return f.actor->set_delivery(a.id, 0, Delivery::unicast);
    });
    REQUIRE_EQ(attempt.wait_for(2s), std::future_status::ready);
    REQUIRE_EQ(attempt.get().status, Transition::busy);
    held.release();
    REQUIRE_EQ(f.actor->set_delivery(a.id, 0, Delivery::unicast).status,
        Transition::applied);
    REQUIRE_EQ(f.actor->set_delivery(b.id, 0, Delivery::overlap).status,
        Transition::full);
    REQUIRE_EQ(f.actor->set_delivery(a.id, 1, Delivery::multicast).status,
        Transition::applied);
    REQUIRE_EQ(f.actor->set_delivery(b.id, 0, Delivery::overlap).status,
        Transition::applied);
    GroupSendCoordinatorTestAccess::revision(
        *f.actor, a.id, std::numeric_limits<std::uint64_t>::max());
    REQUIRE_EQ(
        f.actor
            ->set_delivery(a.id, std::numeric_limits<std::uint64_t>::max(),
                Delivery::unicast)
            .status,
        Transition::exhausted);
    f.actor->remove_member(b.id);
    REQUIRE_EQ(f.actor->snapshot().unicast_reservations, 0U);
    REQUIRE_EQ(f.actor->set_delivery(b.id, 1, Delivery::unicast).status,
        Transition::stale);
    f.actor->close();
    REQUIRE_EQ(f.actor->set_delivery(a.id, 0, Delivery::unicast).status,
        Transition::closed);
}

TEST(
    compat_group_policy_unavailable_or_excessive_prefix_never_partially_replays)
{
    for (const bool too_large : {false, true}) {
        Fixture f(CryptoMode::automatic, 64);
        Receiver a(f, 300);
        if (too_large) {
            for (unsigned i = 0; i < 257; ++i) {
                REQUIRE_EQ(f.send(), Status::accepted);
                f.pump();
                tick(f, a);
            }
        } else {
            REQUIRE_EQ(f.send(f.now + 1ms), Status::accepted);
            f.pump();
            tick(f, a);
            tick(f, a);
            a.exchange_keys();
        }
        const auto policy =
            f.actor->set_delivery(a.id, 0, Delivery::unicast, true);
        REQUIRE_EQ(policy.status,
            too_large ? Transition::full : Transition::unavailable);
        REQUIRE_EQ(policy.member.revision, 0U);
        REQUIRE_EQ(policy.member.delivery, Delivery::multicast);
        REQUIRE_EQ(f.actor->snapshot().unicast_reservations, 0U);
        REQUIRE(a.data_attempts.empty());
    }
}

TEST(compat_group_policy_stalled_unicast_key_horizon_detaches_only_that_member)
{
    for (auto mode : {CryptoMode::aes_ctr, CryptoMode::aes_gcm}) {
        Fixture f(mode, 32);
        f.actor->close();
        f.config.maximum_unicast_members = 1;
        f.recreate();
        Receiver a(f, 32), b(f, 32);
        REQUIRE_EQ(f.actor->set_delivery(a.id, 0, Delivery::unicast).status,
            Transition::applied);
        for (unsigned i = 0; i < 7; ++i) {
            REQUIRE_EQ(f.send(), Status::accepted);
            a.exchange_keys(true);
            b.exchange_keys();
            f.pump();
            b.ack(initial.advanced(i + 1), true);
            tick(f, a, &b);
        }
        REQUIRE_EQ(
            a.data_attempts.size(), 1U); // One-packet unicast flow window.
        const auto revision = a.crypto->key_material_revision();
        REQUIRE_EQ(f.send(), Status::accepted);
        b.exchange_keys();
        f.pump();
        REQUIRE_EQ(reason(f, a.id), Actor::Detachment::history_full);
        REQUIRE_EQ(reason(f, b.id), Actor::Detachment::none);
        REQUIRE_EQ(a.crypto->key_material_revision(), revision);
        b.ack(initial.advanced(8), true);
        REQUIRE_EQ(b.buffered(), 0U);
        REQUIRE_EQ(f.actor->snapshot().unicast_reservations, 1U);
        REQUIRE_EQ(f.actor->set_delivery(b.id, 0, Delivery::unicast).status,
            Transition::full);
        a.runtime->close();
        f.pump();
        REQUIRE_EQ(f.actor->snapshot().unicast_reservations, 0U);
        REQUIRE_EQ(f.actor->set_delivery(b.id, 0, Delivery::unicast).status,
            Transition::applied);
    }
}

TEST(compat_group_policy_rotations_and_sequence_wrap_preserve_both_paths)
{
    for (auto mode : {CryptoMode::aes_ctr, CryptoMode::aes_gcm}) {
        const SequenceNumber start {SequenceNumber::mask - 2U};
        Fixture f(mode, 8, start);
        Receiver a(f, 4, true, start), b(f, 4, true, start);
        std::uint64_t revision = 0;
        std::size_t unicast = 0;
        for (unsigned i = 0; i < 20; ++i) {
            const auto delivery = static_cast<Delivery>(i % 3);
            const auto policy = f.actor->set_delivery(a.id, revision, delivery);
            REQUIRE(policy.status == Transition::applied
                || policy.status == Transition::unchanged);
            revision = policy.member.revision;
            REQUIRE_EQ(f.send(), Status::accepted);
            if (i % 2) {
                a.exchange_keys(true);
                b.exchange_keys();
                f.pump();
            } else {
                f.pump();
                a.exchange_keys(true);
                b.exchange_keys();
            }
            if (delivery != Delivery::multicast) {
                REQUIRE_EQ(a.data_attempts.size(), unicast + 1);
                ++unicast;
                REQUIRE_EQ(a.data_attempts.back(), f.output.packets.back());
            }
            decrypt_original(a, mode, f.output.packets.back());
            a.ack(start.advanced(i + 1), true);
            b.ack(start.advanced(i + 1), true);
            REQUIRE_EQ(a.buffered(), 0U);
            tick(f, a, &b);
        }
        REQUIRE_EQ(a.runtime->statistics(false, true).total.sent_unique.packets,
            unicast);
        REQUIRE_EQ(
            b.runtime->statistics(false, true).total.sent_unique.packets, 0U);
        REQUIRE_EQ(f.actor->snapshot().detached_members, 0U);
    }
}

TEST(
    compat_group_policy_source_failure_does_not_credit_or_cancel_committed_unicast)
{
    for (auto delivery :
        {Delivery::multicast, Delivery::unicast, Delivery::overlap}) {
        Fixture f;
        Receiver a(f);
        const auto policy = f.actor->set_delivery(a.id, 0, delivery);
        REQUIRE(policy.status == Transition::applied
            || policy.status == Transition::unchanged);
        REQUIRE_EQ(f.send(), Status::accepted);
        f.output.error = Error::io_error;
        f.pump();
        a.ack(initial.next(), true);
        REQUIRE_EQ(a.buffered(), 1U);
        a.exchange_keys(true);
        if (delivery == Delivery::multicast) {
            REQUIRE(a.data_attempts.empty());
            REQUIRE_EQ(
                reason(f, a.id), Actor::Detachment::source_not_submitted);
        } else {
            REQUIRE_EQ(a.data_attempts.size(), 1U);
            a.ack(initial.next(), true);
            REQUIRE_EQ(a.buffered(), 0U);
            REQUIRE_EQ(reason(f, a.id), Actor::Detachment::none);
        }
        REQUIRE(!a.runtime->broken());
        REQUIRE_EQ(f.send(), Status::failed);
    }
}

TEST(compat_group_policy_native_retry_is_fenced_by_binding_close_ttl_or_ack)
{
    for (unsigned cause = 0; cause < 5; ++cause) {
        Fixture f;
        Receiver a(f);
        const auto mode = cause == 4 ? Delivery::overlap : Delivery::unicast;
        REQUIRE_EQ(
            f.actor->set_delivery(a.id, 0, mode).status, Transition::applied);
        REQUIRE_EQ(f.send(cause == 3 ? f.now + 1ms : Clock::time_point::max()),
            Status::accepted);
        a.output.data_failures = 1;
        a.exchange_keys(true);
        REQUIRE_EQ(a.data_attempts.size(), 1U);
        if (cause == 0)
            Admission::revoke(
                GroupSendCoordinatorTestAccess::binding(*f.actor, a.id));
        if (cause == 1)
            a.runtime->close();
        if (cause == 2) {
            SocketOptions options;
            a.runtime->apply_options(options);
        }
        if (cause == 4) {
            f.pump();
            a.ack(initial.next(), true);
            REQUIRE_EQ(a.buffered(), 0U);
        }
        tick(f, a);
        tick(f, a);
        a.exchange_keys(true);
        f.pump();
        REQUIRE_EQ(a.data_attempts.size(), 1U);
        REQUIRE_EQ(
            a.runtime->statistics(false, true).total.sent_unique.packets, 0U);
    }
}

TEST(compat_group_policy_lite_ack_does_not_replenish_unicast_receive_credit)
{
    Fixture f;
    Receiver a(f);
    REQUIRE_EQ(f.actor->set_delivery(a.id, 0, Delivery::unicast).status,
        Transition::applied);
    REQUIRE_EQ(f.send(), Status::accepted);
    a.exchange_keys(true);
    f.pump();
    REQUIRE_EQ(a.data_attempts.size(), 1U);
    a.ack(initial.next()); // Lite ACK consumes the one advertised credit.
    tick(f, a);
    REQUIRE_EQ(f.send(), Status::accepted);
    a.exchange_keys(true);
    f.pump();
    REQUIRE_EQ(a.data_attempts.size(), 1U);
    a.ack(initial.next(), true);
    tick(f, a);
    a.exchange_keys(true);
    REQUIRE_EQ(a.data_attempts.size(), 2U);
    a.ack(initial.advanced(2), true);
    REQUIRE_EQ(a.buffered(), 0U);
}

TEST(
    compat_group_policy_late_member_key_horizon_uses_group_position_not_local_zero)
{
    for (auto mode : {CryptoMode::aes_ctr, CryptoMode::aes_gcm}) {
        Fixture f(mode, 32);
        for (unsigned i = 0; i < 9; ++i)
            REQUIRE_EQ(f.send(), Status::advanced_without_receivers);
        const auto start = initial.advanced(9);
        Receiver a(f, 32, true, start), b(f, 32, true, start);
        REQUIRE_EQ(f.actor->set_delivery(a.id, 0, Delivery::unicast).status,
            Transition::applied);
        for (unsigned i = 0; i < 6; ++i) {
            REQUIRE_EQ(f.send(), Status::accepted);
            a.exchange_keys(true);
            b.exchange_keys();
            f.pump();
            b.ack(start.advanced(i + 1), true);
            tick(f, a, &b);
        }
        REQUIRE_EQ(a.data_attempts.size(), 1U);
        REQUIRE_EQ(f.send(), Status::accepted);
        b.exchange_keys();
        f.pump();
        REQUIRE_EQ(reason(f, a.id), Actor::Detachment::history_full);
        REQUIRE_EQ(reason(f, b.id), Actor::Detachment::none);
        b.ack(start.advanced(7), true);
        REQUIRE_EQ(b.buffered(), 0U);
    }
}

TEST(
    compat_group_policy_unicast_uses_actual_datagram_pacing_without_double_gcm_tag)
{
    for (auto mode : {CryptoMode::automatic, CryptoMode::aes_gcm}) {
        Fixture f(mode, 8);
        const std::int64_t rate = packet_header_size + payload.size()
            + (mode == CryptoMode::aes_gcm ? srt_gcm_authentication_tag_size
                                           : 0);
        Receiver a(f, 4, true, initial, false, rate);
        REQUIRE_EQ(f.actor->set_delivery(a.id, 0, Delivery::unicast).status,
            Transition::applied);
        REQUIRE_EQ(f.send(), Status::accepted);
        a.exchange_keys(true);
        f.pump();
        REQUIRE_EQ(a.data_attempts.size(), 1U);
        a.ack(initial.next(), true);
        REQUIRE_EQ(f.send(), Status::accepted);
        a.now += 999'999;
        f.now += 999999us;
        a.exchange_keys(true);
        REQUIRE_EQ(a.data_attempts.size(), 1U);
        ++a.now;
        f.now += 1us;
        a.exchange_keys(true);
        f.pump();
        REQUIRE_EQ(a.data_attempts.size(), 2U);
        REQUIRE_EQ(
            a.data_attempts.back().size(), static_cast<std::size_t>(rate));
        REQUIRE_EQ(
            a.runtime->statistics(false, true).total.sent_unique.payload_bytes,
            2U * payload.size());
        a.ack(initial.advanced(2), true);
        REQUIRE_EQ(a.buffered(), 0U);
    }
}

TEST(
    compat_group_policy_replaced_prefix_key_generation_refuses_transition_without_replay)
{
    for (auto mode : {CryptoMode::aes_ctr, CryptoMode::aes_gcm}) {
        Fixture f(mode, 16);
        Receiver a(f, 16);
        for (unsigned i = 0; i < 8; ++i) {
            REQUIRE_EQ(f.send(), Status::accepted);
            a.exchange_keys();
            f.pump();
            tick(f, a);
        }
        for (const bool replay : {false, true}) {
            const auto result =
                f.actor->set_delivery(a.id, 0, Delivery::unicast, replay);
            REQUIRE_EQ(result.status, Transition::unavailable);
            REQUIRE_EQ(result.member.revision, 0U);
            REQUIRE_EQ(result.member.delivery, Delivery::multicast);
            REQUIRE_EQ(f.actor->snapshot().unicast_reservations, 0U);
            a.exchange_keys();
            REQUIRE(a.data_attempts.empty());
        }
        // Explicitly retiring the old prefix makes a future boundary eligible.
        a.ack(initial.advanced(8), true);
        REQUIRE_EQ(f.actor->set_delivery(a.id, 0, Delivery::unicast).status,
            Transition::applied);
    }
}

TEST(compat_group_policy_prefix_repairs_pin_keys_until_cumulative_retirement)
{
    for (auto mode : {CryptoMode::aes_ctr, CryptoMode::aes_gcm}) {
        for (const bool retire : {false, true}) {
            Fixture f(mode, 16);
            Receiver a(f, 16), b(f, 16);
            for (unsigned i = 0; i < 7; ++i) {
                REQUIRE_EQ(f.send(), Status::accepted);
                a.exchange_keys();
                b.exchange_keys();
                f.pump();
                b.ack(initial.advanced(i + 1), true);
                tick(f, a, &b);
            }
            const auto recovered =
                f.actor->set_delivery(a.id, 0, Delivery::multicast, true);
            REQUIRE_EQ(recovered.status, Transition::applied);
            REQUIRE_EQ(recovered.prefix.repairs, 7U);
            a.output.data_failures = 1;
            a.exchange_keys(true);
            REQUIRE_EQ(a.data_attempts.size(), 1U);
            REQUIRE(decode_packet(a.data_attempts.back())
                    .packet.data.retransmitted);
            const auto key_revision = a.crypto->key_material_revision();
            if (retire)
                a.ack(initial.advanced(7), true);
            REQUIRE_EQ(f.send(), Status::accepted);
            b.exchange_keys();
            f.pump();
            REQUIRE_EQ(reason(f, a.id),
                retire ? Actor::Detachment::none
                       : Actor::Detachment::history_full);
            REQUIRE_EQ(reason(f, b.id), Actor::Detachment::none);
            if (!retire)
                REQUIRE_EQ(a.crypto->key_material_revision(), key_revision);
        }
    }
}

namespace {
void configure_wire_budget(Fixture& f, Clock::time_point& clock,
    std::uint64_t rate, std::uint64_t source_rate = 1'000'000)
{
    f.actor->close();
    f.config.source.wire_bytes_per_second = source_rate;
    f.config.aggregate_unicast_wire_bytes_per_second = rate;
    f.config.unicast_budget_clock = [](void* p) noexcept {
        return *static_cast<Clock::time_point*>(p);
    };
    f.config.unicast_budget_clock_context = &clock;
    f.recreate();
}
void exhaust_wire_credit(Fixture& f)
{
    auto budget = GroupSendCoordinatorTestAccess::budget(*f.actor);
    const auto credit = budget->snapshot().available_bytes;
    REQUIRE(credit > 28U);
    auto permit = budget->acquire(credit - 28);
    REQUIRE(permit);
    permit.finish(true, false);
    REQUIRE_EQ(budget->snapshot().available_bytes, 0U);
}
}

TEST(
    compat_group_budget_rate_capacity_and_old_originals_fence_new_recipient_admission)
{
    Fixture f;
    auto clock = f.now;
    configure_wire_budget(f, clock, 1'999'999);
    Receiver a(f), b(f);
    REQUIRE_EQ(f.actor->snapshot().unicast_recipient_limit, 1U);
    REQUIRE_EQ(f.actor->set_delivery(a.id, 0, Delivery::unicast).status,
        Transition::applied);
    exhaust_wire_credit(f);
    REQUIRE_EQ(f.send(), Status::accepted);
    a.exchange_keys(true);
    REQUIRE(a.data_attempts.empty());
    REQUIRE_EQ(f.actor->set_delivery(a.id, 1, Delivery::multicast).status,
        Transition::applied);
    REQUIRE_EQ(f.actor->set_delivery(b.id, 0, Delivery::overlap).status,
        Transition::full);
    REQUIRE_EQ(f.actor->snapshot().unicast_reservations, 1U);
    clock += 1ms;
    a.exchange_keys(true);
    REQUIRE_EQ(a.data_attempts.size(), 1U);
    f.pump();
    REQUIRE_EQ(f.actor->snapshot().unicast_reservations, 0U);
    REQUIRE_EQ(f.actor->set_delivery(b.id, 0, Delivery::overlap).status,
        Transition::applied);
}

TEST(
    compat_group_budget_original_overlap_and_prefix_repair_share_protected_wire_rate)
{
    for (auto mode :
        {CryptoMode::automatic, CryptoMode::aes_ctr, CryptoMode::aes_gcm}) {
        Fixture f(mode, 8);
        auto clock = f.now;
        configure_wire_budget(f, clock, 3'000'000);
        Receiver a(f), b(f);
        REQUIRE_EQ(f.actor->set_delivery(a.id, 0, Delivery::unicast).status,
            Transition::applied);
        REQUIRE_EQ(f.actor->set_delivery(b.id, 0, Delivery::overlap).status,
            Transition::applied);
        exhaust_wire_credit(f);
        REQUIRE_EQ(f.send(), Status::accepted);
        f.pump();
        a.exchange_keys(true);
        b.exchange_keys(true);
        REQUIRE(a.data_attempts.empty());
        REQUIRE(b.data_attempts.empty());
        a.ack(initial.next(), true);
        REQUIRE_EQ(a.buffered(), 1U);
        const auto cost = 47U + (mode == CryptoMode::aes_gcm ? 16U : 0U);
        const auto step = std::chrono::microseconds {(cost + 2U) / 3U};
        clock += step;
        a.exchange_keys(true);
        REQUIRE_EQ(a.data_attempts.size(), 1U);
        const auto recovery =
            f.actor->set_delivery(a.id, 1, Delivery::unicast, true);
        REQUIRE_EQ(recovery.status, Transition::applied);
        REQUIRE_EQ(recovery.prefix.repairs, 1U);
        b.exchange_keys(true);
        REQUIRE(b.data_attempts.empty());
        clock += step;
        b.exchange_keys(true);
        REQUIRE_EQ(b.data_attempts.size(), 1U);
        clock += step;
        tick(f, a);
        a.exchange_keys(true);
        REQUIRE_EQ(a.data_attempts.size(), 2U);
        REQUIRE(
            decode_packet(a.data_attempts.back()).packet.data.retransmitted);
        decrypt_original(a, mode, a.data_attempts.front());
        decrypt_original(a, mode, a.data_attempts.back());
        const auto budget = f.actor->snapshot().unicast_budget;
        REQUIRE_EQ(budget.original_wire_bytes, 1500U + 2U * cost);
        REQUIRE_EQ(budget.repair_wire_bytes, cost);
        REQUIRE_EQ(budget.submitted_packets, 4U);
        REQUIRE(budget.waits > 0);
        REQUIRE_EQ(f.output.packets.size(), 1U);
    }
}

TEST(compat_group_budget_quota_wait_keeps_key_exchange_and_control_service_live)
{
    for (auto mode : {CryptoMode::aes_ctr, CryptoMode::aes_gcm}) {
        Fixture f(mode, 16);
        auto clock = f.now;
        configure_wire_budget(f, clock, 1'000'000);
        Receiver a(f, 16);
        REQUIRE_EQ(f.actor->set_delivery(a.id, 0, Delivery::unicast).status,
            Transition::applied);
        const auto initial_revision = a.crypto->key_material_revision();
        exhaust_wire_credit(f);
        for (unsigned i = 0; i < 4; ++i) {
            REQUIRE_EQ(f.send(), Status::accepted);
            a.exchange_keys(true);
            f.pump();
            tick(f, a);
        }
        REQUIRE(a.data_attempts.empty());
        REQUIRE(a.control_attempts > 0);
        REQUIRE(a.crypto->key_material_revision() > initial_revision);
        REQUIRE(a.crypto->ready_to_send_data());
        a.ack(initial.advanced(4), true);
        REQUIRE_EQ(a.buffered(), 4U);
        clock += 1s;
        a.exchange_keys(true);
        REQUIRE_EQ(a.data_attempts.size(), 1U);
        decrypt_original(a, mode, a.data_attempts.back());
        REQUIRE_EQ(f.actor->snapshot().unicast_budget.submitted_packets, 2U);
    }
}

TEST(
    compat_group_budget_native_retry_refunds_credit_and_moves_quota_wait_out_of_fifo)
{
    Fixture f(CryptoMode::aes_gcm, 8);
    auto clock = f.now;
    configure_wire_budget(f, clock, 2'000'000);
    Receiver a(f), b(f);
    REQUIRE_EQ(f.actor->set_delivery(a.id, 0, Delivery::unicast).status,
        Transition::applied);
    REQUIRE_EQ(f.actor->set_delivery(b.id, 0, Delivery::unicast).status,
        Transition::applied);
    REQUIRE_EQ(f.send(), Status::accepted);
    a.output.data_failures = 1;
    a.exchange_keys(true);
    REQUIRE_EQ(a.data_attempts.size(), 1U);
    const auto exact = a.data_attempts.back();
    REQUIRE_EQ(f.actor->snapshot().unicast_budget.submitted_packets, 0U);
    b.exchange_keys(true);
    REQUIRE_EQ(b.data_attempts.size(), 1U);
    exhaust_wire_credit(f);
    tick(f, a, &b);
    a.exchange_keys(true);
    REQUIRE_EQ(a.data_attempts.size(), 1U);
    const auto controls = a.control_attempts;
    a.now += 1'000'000;
    a.exchange_keys(true);
    REQUIRE(a.control_attempts > controls);
    REQUIRE_EQ(a.data_attempts.size(), 1U);
    clock += 1ms;
    a.exchange_keys(true);
    REQUIRE_EQ(a.data_attempts.size(), 2U);
    REQUIRE_EQ(a.data_attempts.back(), exact);
    const auto state = f.actor->snapshot().unicast_budget;
    REQUIRE_EQ(state.cancelled_attempts, 1U);
    REQUIRE_EQ(
        state.submitted_packets, 3U); // B, synthetic prior load, A retry.
}

TEST(compat_group_budget_held_original_is_fenced_by_ack_ttl_revoke_and_close)
{
    for (unsigned action = 0; action < 4; ++action) {
        Fixture f;
        auto clock = f.now;
        configure_wire_budget(f, clock, 1'000'000);
        Receiver a(f);
        REQUIRE_EQ(f.actor
                       ->set_delivery(a.id, 0,
                           action == 0 ? Delivery::overlap : Delivery::unicast)
                       .status,
            Transition::applied);
        exhaust_wire_credit(f);
        REQUIRE_EQ(f.send(action == 1 ? f.now + 1ms : Clock::time_point::max()),
            Status::accepted);
        f.pump();
        a.exchange_keys(true);
        REQUIRE(a.data_attempts.empty());
        std::future<bool> drained;
        if (action == 0) {
            a.ack(initial.next(), true);
            drained = std::async(std::launch::async, [&] {
                return a.runtime->wait_for_send_drain(2s);
            });
            REQUIRE_EQ(drained.wait_for(2ms), std::future_status::timeout);
        }
        if (action == 1) {
            tick(f, a);
            tick(f, a);
        }
        if (action == 2)
            Admission::revoke(
                GroupSendCoordinatorTestAccess::binding(*f.actor, a.id));
        if (action == 3)
            f.actor->close();
        clock += 1s;
        a.exchange_keys(true);
        REQUIRE(a.data_attempts.empty());
        if (action == 0) {
            REQUIRE_EQ(drained.wait_for(1s), std::future_status::ready);
            REQUIRE(drained.get());
        }
        REQUIRE_EQ(f.actor->snapshot().unicast_budget.submitted_packets, 1U);
    }
}

TEST(
    compat_group_budget_partial_native_success_is_charged_and_breaks_only_its_member)
{
    Fixture f;
    Receiver a(f), b(f);
    REQUIRE_EQ(f.actor->set_delivery(a.id, 0, Delivery::unicast).status,
        Transition::applied);
    a.output.partial = true;
    REQUIRE_EQ(f.send(), Status::accepted);
    a.exchange_keys(true);
    REQUIRE_EQ(a.data_attempts.size(), 1U);
    const auto budget = f.actor->snapshot().unicast_budget;
    REQUIRE_EQ(budget.original_wire_bytes, 47U);
    REQUIRE_EQ(budget.submitted_packets, 1U);
    REQUIRE_EQ(
        a.runtime->statistics(false, true).total.sent_unique.packets, 0U);
    REQUIRE_EQ(f.send(), Status::accepted);
    f.pump();
    REQUIRE_EQ(reason(f, a.id), Actor::Detachment::closed);
    REQUIRE_EQ(reason(f, b.id), Actor::Detachment::none);
}

TEST(
    compat_group_budget_concurrent_transitions_reserve_only_available_full_stream_capacity)
{
    Fixture f;
    auto clock = f.now;
    configure_wire_budget(f, clock, 2'000'000);
    Receiver a(f), b(f), c(f);
    auto one = std::async(std::launch::async, [&] {
        return f.actor->set_delivery(a.id, 0, Delivery::unicast);
    });
    auto two = std::async(std::launch::async, [&] {
        return f.actor->set_delivery(b.id, 0, Delivery::overlap);
    });
    auto three = std::async(std::launch::async, [&] {
        return f.actor->set_delivery(c.id, 0, Delivery::unicast);
    });
    std::size_t applied = 0, refused = 0;
    for (auto result : {one.get(), two.get(), three.get()}) {
        if (result.status == Transition::applied) {
            ++applied;
            REQUIRE_EQ(result.member.revision, 1U);
        }
        if (result.status == Transition::full) {
            ++refused;
            REQUIRE_EQ(result.member.revision, 0U);
        }
    }
    REQUIRE_EQ(applied, 2U);
    REQUIRE_EQ(refused, 1U);
    REQUIRE_EQ(f.actor->snapshot().unicast_reservations, 2U);
}

TEST(compat_group_budget_long_rate_wait_preserves_sender_ttl_deadline)
{
    Fixture f;
    auto clock = f.now;
    configure_wire_budget(f, clock, 1, 1);
    Receiver a(f);
    REQUIRE_EQ(f.actor->set_delivery(a.id, 0, Delivery::unicast).status,
        Transition::applied);
    exhaust_wire_credit(f);
    REQUIRE_EQ(f.send(f.now + 2ms), Status::accepted);
    const auto result = a.runtime->poll();
    REQUIRE(!result.immediate_work);
    REQUIRE(result.next_work_delay);
    REQUIRE(*result.next_work_delay <= 2001us);
    f.now += 2001us;
    a.now += 2001;
    a.exchange_keys(true);
    REQUIRE(a.data_attempts.empty());
    REQUIRE_EQ(a.buffered(), 0U);
    REQUIRE_EQ(f.actor->snapshot().unicast_budget.submitted_packets, 1U);
}

TEST(
    compat_group_budget_configuration_saturates_default_rate_and_refuses_unfunded_originals)
{
    Fixture f;
    auto clock = f.now;
    REQUIRE_EQ(
        f.actor->snapshot().unicast_budget.wire_bytes_per_second, 4'000'000U);
    f.actor->close();
    f.config.source.wire_bytes_per_second = UINT64_MAX;
    f.recreate();
    REQUIRE_EQ(
        f.actor->snapshot().unicast_budget.wire_bytes_per_second, UINT64_MAX);
    REQUIRE_EQ(f.actor->snapshot().unicast_recipient_limit, 1U);
    configure_wire_budget(f, clock, 999'999);
    Receiver a(f);
    REQUIRE_EQ(f.actor->snapshot().unicast_recipient_limit, 0U);
    REQUIRE_EQ(f.actor->set_delivery(a.id, 0, Delivery::unicast).status,
        Transition::full);
    REQUIRE_EQ(f.actor->set_delivery(a.id, 0, Delivery::overlap).status,
        Transition::full);
    REQUIRE_EQ(f.send(), Status::accepted);
    REQUIRE_EQ(f.actor->set_delivery(a.id, 0, Delivery::multicast, true).status,
        Transition::full);
    REQUIRE_EQ(f.actor->snapshot().unicast_reservations, 0U);
    f.pump();
    const auto repaired =
        f.actor->set_delivery(a.id, 0, Delivery::multicast, true);
    REQUIRE_EQ(repaired.status, Transition::applied);
    REQUIRE_EQ(repaired.prefix.repairs, 1U);
    a.exchange_keys(true);
    REQUIRE_EQ(a.data_attempts.size(), 1U);
    REQUIRE(decode_packet(a.data_attempts.back()).packet.data.retransmitted);
}

namespace {
Actor::SourceRecovery recover_source(Fixture& f,
    std::initializer_list<Actor::RecoveryMember> members,
    std::uint64_t revision = 0)
{
    return f.actor->recover_source_to_unicast(
        revision, {members.begin(), members.size()});
}
void fail_source(Fixture& f)
{
    f.output.error = Error::io_error;
    for (unsigned i = 0; i < 64 && f.actor->snapshot().inflight_frames; ++i)
        f.pump();
    REQUIRE_EQ(f.actor->snapshot().inflight_frames, 0U);
    REQUIRE(f.actor->snapshot().failed);
    REQUIRE_EQ(f.actor->snapshot().source_failure,
        GroupMulticastEmitter::Failure::transport);
}
}

TEST(
    compat_group_source_recovery_preserves_prefix_identity_wrap_and_rotating_future_unicast)
{
    for (auto mode :
        {CryptoMode::automatic, CryptoMode::aes_ctr, CryptoMode::aes_gcm}) {
        const SequenceNumber start {SequenceNumber::mask - 2U};
        Fixture f(mode, 8, start);
        f.actor->close();
        f.config.unicast_budget_clock = f.config.source.clock;
        f.config.unicast_budget_clock_context = &f.now;
        f.recreate();
        Receiver a(f, 16, true, start), b(f, 16, true, start);
        REQUIRE_EQ(f.actor->set_delivery(b.id, 0, Delivery::overlap).status,
            Transition::applied);
        for (unsigned i = 0; i < 3; ++i)
            REQUIRE_EQ(f.send(), Status::accepted);
        fail_source(f);
        REQUIRE_EQ(reason(f, a.id), Actor::Detachment::source_not_submitted);
        REQUIRE_EQ(reason(f, b.id), Actor::Detachment::none);
        a.ack(start.next(), true);
        REQUIRE_EQ(a.buffered(), 3U);
        const auto result = recover_source(f, {{a.id, 0}, {b.id, 1}});
        REQUIRE_EQ(result.status, Transition::applied);
        REQUIRE_EQ(result.revision, 1U);
        REQUIRE_EQ(result.members, 2U);
        REQUIRE_EQ(result.effective_sequence, start.advanced(3));
        REQUIRE_EQ(result.effective_position, 3U);
        REQUIRE_EQ(result.prefix.originals, 3U);
        REQUIRE_EQ(result.prefix.repairs, 0U);
        REQUIRE_EQ(
            GroupSendCoordinatorTestAccess::encoder(*f.actor).committed_packets,
            3U);
        REQUIRE(!f.actor->snapshot().failed);
        REQUIRE(f.actor->snapshot().writable);
        for (unsigned i = 0; i < 3; ++i) {
            tick(f, a, &b);
            a.exchange_keys(true);
            b.exchange_keys(true);
            REQUIRE(a.data_attempts.size() >= i + 1);
            REQUIRE(b.data_attempts.size() >= i + 1);
            REQUIRE_EQ(a.data_attempts[i], b.data_attempts[i]);
            REQUIRE(
                !decode_packet(a.data_attempts[i]).packet.data.retransmitted);
            decrypt_original(a, mode, a.data_attempts[i]);
            a.ack(start.advanced(i + 1), true);
            b.ack(start.advanced(i + 1), true);
        }
        REQUIRE_EQ(a.data_attempts.front(), f.output.packets.front());
        for (unsigned i = 3; i < 23; ++i) {
            tick(f, a, &b);
            REQUIRE_EQ(f.send(), Status::accepted);
            a.exchange_keys(true);
            b.exchange_keys(true);
            f.pump();
            REQUIRE_EQ(a.data_attempts.size(), i + 1U);
            REQUIRE_EQ(b.data_attempts.size(), i + 1U);
            REQUIRE_EQ(a.data_attempts.back(), b.data_attempts.back());
            decrypt_original(a, mode, a.data_attempts.back());
            a.ack(start.advanced(i + 1), true);
            b.ack(start.advanced(i + 1), true);
        }
        REQUIRE_EQ(
            f.output.size(), 1U); // No multicast syscall after retirement.
        REQUIRE_EQ(f.actor->snapshot().unicast_only_frames, 20U);
        REQUIRE_EQ(
            GroupSendCoordinatorTestAccess::encoder(*f.actor).committed_packets,
            23U);
        REQUIRE_EQ(f.actor->snapshot().detached_members, 0U);
        REQUIRE_EQ(a.buffered() + b.buffered(), 0U);
    }
}

TEST(
    compat_group_source_recovery_revision_request_and_member_checks_leave_failure_unchanged)
{
    Fixture f;
    Receiver a(f), b(f);
    REQUIRE_EQ(recover_source(f, {{a.id, 0}}).status, Transition::unavailable);
    REQUIRE_EQ(f.send(), Status::accepted);
    fail_source(f);
    REQUIRE_EQ(recover_source(f, {}).status, Transition::invalid);
    REQUIRE_EQ(
        recover_source(f, {{a.id, 0}, {a.id, 0}}).status, Transition::invalid);
    REQUIRE_EQ(recover_source(f, {{0, 0}}).status, Transition::stale);
    REQUIRE_EQ(recover_source(f, {{a.id, 1}}).status, Transition::stale);
    REQUIRE_EQ(recover_source(f, {{a.id, 0}}, 1).status, Transition::stale);
    REQUIRE(f.actor->snapshot().failed);
    REQUIRE_EQ(f.actor->snapshot().source_revision, 0U);
    a.exchange_keys(true);
    b.exchange_keys(true);
    REQUIRE(a.data_attempts.empty());
    REQUIRE(b.data_attempts.empty());
    REQUIRE_EQ(recover_source(f, {{a.id, 0}}).status, Transition::applied);
    REQUIRE_EQ(recover_source(f, {{a.id, 0}}).status, Transition::stale);
    REQUIRE_EQ(
        recover_source(f, {{b.id, 0}}, 1).status, Transition::unavailable);
    REQUIRE_EQ(f.actor->set_delivery(a.id, 1, Delivery::multicast).status,
        Transition::unavailable);
    REQUIRE_EQ(f.actor->set_delivery(a.id, 1, Delivery::overlap).status,
        Transition::unavailable);
    REQUIRE_EQ(f.actor->set_delivery(a.id, 1, Delivery::unicast).status,
        Transition::unchanged);
}

TEST(
    compat_group_source_recovery_busy_second_runtime_does_not_schedule_first_prefix)
{
    Fixture f;
    Receiver a(f), b(f);
    REQUIRE_EQ(f.send(), Status::accepted);
    fail_source(f);
    auto binding = GroupSendCoordinatorTestAccess::binding(*f.actor, b.id);
    std::promise<void> held, release;
    auto released = release.get_future();
    auto blocker = std::async(std::launch::async, [&] {
        auto guard = Admission::try_recover_prefix(binding, initial.next());
        REQUIRE_EQ(guard.result().status, Admission::Status::ready);
        held.set_value();
        released.wait();
    });
    held.get_future().wait();
    const auto result = recover_source(
        f, {{b.id, 0}, {a.id, 0}}); // Order cannot change lock order.
    release.set_value();
    blocker.get();
    REQUIRE_EQ(result.status, Transition::busy);
    REQUIRE(f.actor->snapshot().failed);
    REQUIRE_EQ(f.actor->snapshot().unicast_reservations, 0U);
    a.exchange_keys(true);
    REQUIRE(a.data_attempts.empty());
    REQUIRE_EQ(
        recover_source(f, {{a.id, 0}, {b.id, 0}}).status, Transition::applied);
}

TEST(
    compat_group_source_recovery_budget_rejection_is_atomic_and_nonselected_members_stay_detached)
{
    Fixture f;
    f.actor->close();
    f.config.maximum_unicast_members = 1;
    f.recreate();
    Receiver a(f), b(f);
    REQUIRE_EQ(f.send(), Status::accepted);
    fail_source(f);
    REQUIRE_EQ(
        recover_source(f, {{a.id, 0}, {b.id, 0}}).status, Transition::full);
    REQUIRE_EQ(f.actor->snapshot().unicast_reservations, 0U);
    a.exchange_keys(true);
    b.exchange_keys(true);
    REQUIRE(a.data_attempts.empty());
    REQUIRE(b.data_attempts.empty());
    REQUIRE_EQ(recover_source(f, {{a.id, 0}}).status, Transition::applied);
    tick(f, a);
    a.exchange_keys(true);
    a.ack(initial.next(), true);
    REQUIRE_EQ(f.send(), Status::accepted);
    tick(f, a);
    a.exchange_keys(true);
    f.pump();
    REQUIRE_EQ(a.data_attempts.size(), 2U);
    REQUIRE(b.data_attempts.empty());
    REQUIRE_EQ(reason(f, b.id), Actor::Detachment::source_not_submitted);
    REQUIRE_EQ(f.actor->snapshot().active_members, 1U);
    REQUIRE_EQ(f.actor->snapshot().detached_members, 1U);
}

TEST(compat_group_source_recovery_waits_for_every_old_outcome_and_fanout_ticket)
{
    Fixture f(CryptoMode::automatic, 8);
    f.actor->close();
    f.config.fanout_batch = 1;
    f.recreate();
    Receiver a(f), b(f);
    for (unsigned i = 0; i < 3; ++i)
        REQUIRE_EQ(f.send(), Status::accepted);
    f.output.error = Error::io_error;
    f.pump();
    REQUIRE(f.actor->snapshot().inflight_frames > 0);
    REQUIRE_EQ(recover_source(f, {{a.id, 0}}).status, Transition::busy);
    fail_source(f);
    REQUIRE_EQ(recover_source(f, {{a.id, 0}}).status, Transition::applied);
    for (unsigned i = 0; i < 12; ++i)
        f.pump(); // No old event can apply to reused frame slots.
    REQUIRE(!f.actor->snapshot().failed);
    REQUIRE_EQ(reason(f, a.id), Actor::Detachment::none);
}

TEST(
    compat_group_source_recovery_mixed_submitted_and_unsubmitted_prefix_uses_correct_R_flags)
{
    Fixture f(CryptoMode::aes_gcm, 8);
    Receiver a(f, 16);
    REQUIRE_EQ(f.send(), Status::accepted);
    f.pump();
    tick(f, a);
    REQUIRE_EQ(f.send(), Status::accepted);
    fail_source(f);
    const auto first = f.output.packets[0], second = f.output.packets[1];
    const auto recovered = recover_source(f, {{a.id, 0}});
    REQUIRE_EQ(recovered.status, Transition::applied);
    REQUIRE_EQ(recovered.prefix.repairs, 1U);
    REQUIRE_EQ(recovered.prefix.originals, 1U);
    for (unsigned i = 0; i < 3; ++i) {
        tick(f, a);
        a.exchange_keys(true);
        a.ack(initial.advanced(std::min(i + 1, 2U)), true);
    }
    REQUIRE_EQ(a.data_attempts.size(), 2U);
    for (const auto& bytes : a.data_attempts) {
        const auto decoded = decode_packet(bytes);
        REQUIRE(decoded);
        const bool old = decoded.packet.data.sequence == initial;
        REQUIRE_EQ(decoded.packet.data.retransmitted, old);
        auto normalized = decoded.packet.data;
        normalized.retransmitted = false;
        std::vector<std::byte> wire(bytes.size());
        REQUIRE(encode_packet({.kind = PacketKind::data,
                                  .data = normalized,
                                  .payload = decoded.packet.payload},
            wire));
        REQUIRE_EQ(wire, old ? first : second);
        decrypt_original(a, CryptoMode::aes_gcm, bytes);
    }
    REQUIRE_EQ(a.buffered(), 0U);
}

TEST(
    compat_group_source_recovery_unavailable_prefix_key_or_deadline_cannot_be_partially_replayed)
{
    for (unsigned variant = 0; variant < 3; ++variant) {
        Fixture f(
            variant == 2 ? CryptoMode::aes_gcm : CryptoMode::automatic, 16);
        Receiver a(f, 16);
        if (variant == 2) {
            for (unsigned i = 0; i < 8; ++i) {
                REQUIRE_EQ(f.send(), Status::accepted);
                a.exchange_keys();
                f.pump();
                tick(f, a);
            }
            REQUIRE_EQ(f.send(), Status::accepted);
        } else
            REQUIRE_EQ(f.send(f.now + 1ms), Status::accepted);
        fail_source(f);
        if (variant != 2) {
            tick(f, a);
            tick(f, a);
        }
        if (variant == 1)
            a.exchange_keys(); // Convert expired storage into a tombstone.
        REQUIRE_EQ(
            recover_source(f, {{a.id, 0}}).status, Transition::unavailable);
        REQUIRE(f.actor->snapshot().failed);
        REQUIRE_EQ(f.actor->snapshot().source_revision, 0U);
        REQUIRE_EQ(f.actor->snapshot().unicast_reservations, 0U);
        a.exchange_keys(true);
        REQUIRE(a.data_attempts.empty());
    }
}

TEST(
    compat_group_source_recovery_excessive_second_prefix_rolls_back_the_entire_batch)
{
    Fixture f(CryptoMode::automatic, 64);
    Receiver a(f, 300), b(f, 300);
    for (unsigned i = 0; i < 256; ++i) {
        REQUIRE_EQ(f.send(), Status::accepted);
        f.pump();
        a.ack(initial.advanced(i + 1), true);
        tick(f, a, &b);
    }
    REQUIRE_EQ(f.send(), Status::accepted);
    fail_source(f);
    REQUIRE_EQ(
        recover_source(f, {{a.id, 0}, {b.id, 0}}).status, Transition::full);
    a.exchange_keys(true);
    REQUIRE(a.data_attempts.empty());
    REQUIRE_EQ(a.buffered(), 1U);
    REQUIRE_EQ(f.actor->snapshot().unicast_reservations, 0U);
}

TEST(
    compat_group_source_recovery_internal_fault_or_retired_binding_is_not_a_resumable_source_error)
{
    for (unsigned variant = 0; variant < 4; ++variant) {
        Fixture f;
        Receiver a(f);
        REQUIRE_EQ(f.send(), Status::accepted);
        if (variant == 0)
            GroupSendCoordinatorTestAccess::fail(*f.actor);
        fail_source(f);
        if (variant == 1)
            Admission::revoke(
                GroupSendCoordinatorTestAccess::binding(*f.actor, a.id));
        if (variant == 2)
            a.runtime->close();
        if (variant == 3)
            a.runtime->apply_options(SocketOptions {});
        REQUIRE_EQ(
            recover_source(f, {{a.id, 0}}).status, Transition::unavailable);
        REQUIRE(f.actor->snapshot().failed);
        REQUIRE_EQ(f.actor->snapshot().source_revision, 0U);
    }
}

TEST(
    compat_group_source_recovery_last_revisions_are_usable_and_exhaustion_never_wraps)
{
    Fixture f;
    Receiver a(f);
    REQUIRE_EQ(f.send(), Status::accepted);
    fail_source(f);
    GroupSendCoordinatorTestAccess::source_revision(*f.actor, UINT64_MAX);
    REQUIRE_EQ(recover_source(f, {{a.id, 0}}, UINT64_MAX).status,
        Transition::exhausted);
    GroupSendCoordinatorTestAccess::source_revision(*f.actor, UINT64_MAX - 1);
    GroupSendCoordinatorTestAccess::revision(*f.actor, a.id, UINT64_MAX);
    REQUIRE_EQ(recover_source(f, {{a.id, UINT64_MAX}}, UINT64_MAX - 1).status,
        Transition::exhausted);
    GroupSendCoordinatorTestAccess::revision(*f.actor, a.id, UINT64_MAX - 1);
    REQUIRE_EQ(
        recover_source(f, {{a.id, UINT64_MAX - 1}}, UINT64_MAX - 1).status,
        Transition::applied);
    REQUIRE_EQ(f.actor->snapshot().source_revision, UINT64_MAX);
}

TEST(
    compat_group_source_recovery_new_work_is_bounded_and_empty_advance_never_rejoins_a_failed_source)
{
    Fixture f(CryptoMode::aes_gcm, 2);
    Receiver a(f, 16);
    REQUIRE_EQ(f.send(), Status::accepted);
    fail_source(f);
    REQUIRE_EQ(recover_source(f, {{a.id, 0}}).status, Transition::applied);
    tick(f, a);
    a.exchange_keys(true);
    a.ack(initial.next(), true);
    REQUIRE_EQ(f.send(), Status::accepted);
    REQUIRE_EQ(f.send(), Status::accepted);
    const auto next = f.actor->snapshot().next_sequence;
    REQUIRE_EQ(f.send(), Status::full);
    REQUIRE_EQ(f.actor->snapshot().next_sequence, next);
    f.pump();
    REQUIRE_EQ(f.actor->snapshot().inflight_frames, 0U);
    REQUIRE_EQ(f.send(), Status::accepted);
    f.pump();
    f.actor->remove_member(a.id);
    for (unsigned i = 0; i < 20; ++i)
        REQUIRE_EQ(f.send(), Status::advanced_without_receivers);
    Receiver late(f, 16, false, f.actor->snapshot().next_sequence);
    REQUIRE(!f.actor->add_member(late.runtime));
    REQUIRE_EQ(late.buffered(), 0U);
    REQUIRE_EQ(f.output.size(), 1U);
    REQUIRE_EQ(f.send(f.now), Status::expired);
    f.actor->close();
    REQUIRE_EQ(recover_source(f, {{a.id, 1}}, 1).status, Transition::closed);
}

TEST(
    compat_group_source_recovery_busy_admission_preserves_identity_until_all_survivors_are_ready)
{
    Fixture f;
    f.actor->close();
    f.config.admission_busy_grace = 10ms;
    f.config.admission_clock = f.config.source.clock;
    f.config.admission_clock_context = &f.now;
    f.recreate();
    Receiver a(f), b(f);
    REQUIRE_EQ(f.send(), Status::accepted);
    fail_source(f);
    REQUIRE_EQ(
        recover_source(f, {{a.id, 0}, {b.id, 0}}).status, Transition::applied);
    tick(f, a, &b);
    a.exchange_keys(true);
    b.exchange_keys(true);
    a.ack(initial.next(), true);
    b.ack(initial.next(), true);
    auto held = b.hold(f);
    REQUIRE_EQ(held.status(), Admission::Status::ready);
    const auto count =
        GroupSendCoordinatorTestAccess::encoder(*f.actor).committed_packets;
    REQUIRE_EQ(f.send(), Status::busy);
    REQUIRE_EQ(
        GroupSendCoordinatorTestAccess::encoder(*f.actor).committed_packets,
        count);
    held.release();
    tick(f, a, &b);
    REQUIRE_EQ(f.send(), Status::accepted);
    f.pump();
    REQUIRE_EQ(f.actor->snapshot().unicast_only_frames, 1U);
}

TEST(
    compat_group_source_recovery_member_status_refresh_rejects_a_stale_resume_observation)
{
    Fixture f;
    Receiver a(f);
    auto record = std::make_shared<GroupRecord>();
    record->dispatch = std::make_unique<DispatchGroupState>();
    record->dispatch->coordinator = f.actor;
    GroupMemberSnapshot member;
    member.coordinator_member = a.id;
    member.public_data.sockstate = SRTS_CONNECTED;
    member.public_data.memberstate = SRT_GST_RUNNING;
    record->members.push_back(member);
    REQUIRE_EQ(f.send(), Status::accepted);
    fail_source(f);
    GroupRegistry::refresh_coordinated_members(record);
    REQUIRE(record->members[0].coordinator_detached);
    REQUIRE_EQ(record->members[0].coordinator_status_revision, 1U);
    REQUIRE_EQ(recover_source(f, {{a.id, 0}}).status, Transition::applied);
    GroupRegistry::refresh_coordinated_members(record);
    REQUIRE(!record->members[0].coordinator_detached);
    REQUIRE_EQ(record->members[0].coordinator_status_revision, 2U);
    REQUIRE_EQ(record->members[0].public_data.memberstate, SRT_GST_RUNNING);
    // Model an already observed newer failure, followed by an older row read
    // before that failure. Refresh must not restore its stale recovery state.
    record->members[0].coordinator_status_revision = 3;
    record->members[0].coordinator_detached = true;
    record->members[0].public_data.memberstate = SRT_GST_BROKEN;
    record->members[0].public_data.result = SRT_ECONNLOST;
    const auto version = record->snapshot_version;
    GroupRegistry::refresh_coordinated_members(record);
    REQUIRE(record->members[0].coordinator_detached);
    REQUIRE_EQ(record->members[0].public_data.memberstate, SRT_GST_BROKEN);
    REQUIRE_EQ(record->snapshot_version, version);
}

TEST(
    compat_group_source_recovery_close_serializes_commit_and_fences_new_native_work)
{
    for (unsigned i = 0; i < 16; ++i) {
        Fixture f;
        Receiver a(f);
        REQUIRE_EQ(f.send(), Status::accepted);
        fail_source(f);
        auto recovered = std::async(std::launch::async, [&] {
            return recover_source(f, {{a.id, 0}});
        });
        auto closed = std::async(std::launch::async, [&] {
            f.actor->close();
        });
        const auto result = recovered.get();
        closed.get();
        REQUIRE(result.status == Transition::applied
            || result.status == Transition::closed);
        REQUIRE(f.actor->snapshot().closed);
        a.exchange_keys(true);
        REQUIRE(a.data_attempts.empty());
        REQUIRE_EQ(
            a.runtime->statistics(false, true).total.sent_unique.packets, 0U);
        REQUIRE_EQ(f.output.size(), 1U);
    }
}

TEST(
    compat_group_source_recovery_negative_outcome_retires_while_its_runtime_is_busy)
{
    Fixture f;
    Receiver a(f);
    REQUIRE_EQ(f.send(), Status::accepted);
    const auto binding =
        GroupSendCoordinatorTestAccess::binding(*f.actor, a.id);
    std::promise<void> held, release;
    auto released = release.get_future();
    Admission::Status held_status = Admission::Status::invalid;
    auto blocker = std::async(std::launch::async, [&] {
        auto guard = Admission::try_recover_prefix(binding, initial.next());
        held_status = guard.result().status;
        held.set_value();
        released.wait();
    });
    held.get_future().wait();
    f.output.error = Error::io_error;
    f.pump();
    const auto state = f.actor->snapshot();
    const auto detachment = reason(f, a.id);
    const auto recovery = recover_source(f, {{a.id, 0}});
    release.set_value();
    blocker.get();
    REQUIRE_EQ(held_status, Admission::Status::ready);
    REQUIRE_EQ(state.inflight_frames, 0U);
    REQUIRE_EQ(state.abandoned_completions, 1U);
    REQUIRE(state.failed);
    REQUIRE_EQ(detachment, Actor::Detachment::source_not_submitted);
    REQUIRE_EQ(recovery.status, Transition::busy);
    REQUIRE_EQ(recover_source(f, {{a.id, 0}}).status, Transition::applied);
}

#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_HYBRID_CONTROL
namespace {
namespace hb = robotweax::srt::compat::hybrid;
struct BridgePeer {
    hb::Authorization grant;
    std::uint64_t now = 100;
    bool alive = true;
    unsigned would_block = 0;
    std::weak_ptr<hb::NativeService> service;
    std::vector<std::vector<std::byte>> output;
    static bool resolve(void* p, const hb::CredentialRequest& r,
        hb::Authorization& g, std::span<std::byte, 32> secret) noexcept
    {
        auto& self = *static_cast<BridgePeer*>(p);
        g = {.credential_id = r.credential_id,
            .caller_group = r.caller_group,
            .listener_group = r.listener_group,
            .stream_id = r.stream_id,
            .generation = 7,
            .expires_at_us = self.now + 9900};
        self.grant = g;
        std::fill(secret.begin(), secret.end(), std::byte {0x22});
        return true;
    }
    static bool check(void* p, const hb::CredentialRequest&,
        const hb::Authorization&, hb::Authorization& g) noexcept
    {
        auto& self = *static_cast<BridgePeer*>(p);
        g = self.grant;
        return self.alive;
    }
    static bool enroll(void*, const hb::Context&, const hb::Authorization&,
        std::uint64_t) noexcept
    {
        return true;
    }
    static bool bound(void* p, const hb::NativeService*) noexcept
    {
        return static_cast<BridgePeer*>(p)->alive;
    }
    static std::uint64_t clock(void* p) noexcept
    {
        return static_cast<BridgePeer*>(p)->now;
    }
    static UdpIoResult send(void* p, const hb::NativeService*,
        std::span<const std::byte> bytes, std::uint64_t) noexcept
    {
        auto& self = *static_cast<BridgePeer*>(p);
        if (self.would_block) {
            --self.would_block;
            return {.error = Error::would_block};
        }
        self.output.emplace_back(bytes.begin(), bytes.end());
        return {.bytes_transferred = bytes.size()};
    }
};
struct BridgeFixture {
    Fixture f;
    Receiver receiver;
    std::shared_ptr<BridgePeer> cp = std::make_shared<BridgePeer>(),
                                lp = std::make_shared<BridgePeer>();
    std::shared_ptr<hb::NativeService> caller, listener;
    std::shared_ptr<hb::AuthorizationLease> lease;
    std::shared_ptr<hb::CoordinatorBridge> bridge;
    std::shared_ptr<const hb::Conclusions> origin;
    explicit BridgeFixture(std::uint32_t limit = 256, bool attach = true,
        std::size_t recipients = 4, hb::ReportingConfiguration reporting = {},
        std::optional<hb::ReturnPolicy> returning = std::nullopt)
        : f(CryptoMode::automatic, 4, initial, recipients)
        , receiver(f, 8)
    {
        REQUIRE_EQ(
            f.channel->socket.bind(IpEndpoint::loopback(0)), Error::none);
        const auto endpoint = f.channel->socket.local_endpoint();
        REQUIRE(endpoint);
        hb::ConclusionCapture capture;
        for (bool c : {true, false}) {
            HandshakeAction a;
            a.kind = HandshakeActionKind::send;
            a.packet.request = HandshakeRequest::conclusion;
            a.packet.socket_id = c ? 108 : 107;
            a.packet.initial_sequence = initial;
            a.packet.extension_field = handshake_extension_flag_handshake
                | handshake_extension_flag_config;
            a.has_handshake_extension = true;
            a.extension_type = c ? HandshakeExtensionType::handshake_request
                                 : HandshakeExtensionType::handshake_response;
            a.has_group_membership = true;
            a.group_membership = {
                .group_id = c ? (group_handle_mask | 2U) : group_id,
                .type = GroupType::multicast};
            a.has_multicast_source = true;
            if (c)
                a.multicast_source.kind = MulticastSourceKind::offer;
            else {
                a.has_multicast_dispatch = true;
                a.multicast_dispatch = {.group_id = group_id,
                    .multicast_group = f.config.source.destination,
                    .initial_sequence = initial.value(),
                    .time_to_live = 1};
                a.multicast_source = {.kind = MulticastSourceKind::announcement,
                    .group_id = group_id,
                    .endpoint = endpoint.endpoint};
            }
            std::array<std::byte, 1500> bytes {};
            const auto encoded =
                encode_handshake_datagram(a, PacketTimestamp {}, 0, bytes);
            REQUIRE(encoded);
            REQUIRE(capture.observe(
                std::span {bytes}.first(encoded.bytes_written)));
        }
        origin = capture.freeze(false, 107, 108);
        REQUIRE(origin);
        if (reporting.interval_us || returning)
            cp->now = lp->now = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    f.now.time_since_epoch())
                    .count());
        const auto identity =
            hb::CoordinatorBridge::identity(f.actor, receiver.runtime);
        REQUIRE(identity);
        const auto config = [&](bool c) {
            auto p = c ? cp : lp;
            hb::NativeService::Configuration x;
            x.caller = c;
            x.local_socket = c ? 108 : 107;
            x.peer_socket = c ? 107 : 108;
            x.conclusions = origin;
            x.exchange.dispatch = origin->dispatch();
            x.exchange.source = origin->source();
            x.exchange.connection_generation = c ? 1 : 2;
            x.exchange.established_peer = IpEndpoint::loopback(c ? 8000 : 8001);
            x.exchange.credential_id.fill(std::byte {0x11});
            x.exchange.policy.supported =
                returning ? hb::return_features : hb::policy_query_features;
            x.exchange.policy.prefix_limit = limit;
            if (!c) {
                x.exchange.stream = identity->stream;
                x.exchange.member = identity->member;
                x.exchange.initial_position = identity->initial_position;
            }
            x.callbacks.owner = p;
            x.callbacks.resolve = BridgePeer::resolve;
            x.callbacks.revalidate = BridgePeer::check;
            x.callbacks.enroll = BridgePeer::enroll;
            x.transport_owner = p;
            x.bound = BridgePeer::bound;
            x.transmit = BridgePeer::send;
            x.clock = BridgePeer::clock;
            x.clock_context = p.get();
            return x;
        };
        caller = hb::NativeService::create_for_testing(config(true));
        listener = hb::NativeService::create_for_testing(config(false));
        REQUIRE(caller);
        REQUIRE(listener);
        REQUIRE(receiver.runtime->publish_hybrid_conclusions(origin, false));
        HybridBridgeTestAccess::install(*receiver.runtime, listener);
        cp->service = caller;
        lp->service = listener;
        REQUIRE(listener->activate());
        REQUIRE(caller->activate());
        for (unsigned i = 0; i < 6; ++i)
            round();
        REQUIRE(caller->snapshot().active);
        REQUIRE(listener->snapshot().active);
        lease = std::make_shared<hb::AuthorizationLease>(lp->grant);
        if (attach) {
            bridge = hb::CoordinatorBridge::create(f.actor, listener, lease,
                {.reporting = reporting,
                    .returning = returning,
                    .command_timeout_us = 1000});
            REQUIRE(bridge);
        }
    }
    void deliver(const std::shared_ptr<BridgePeer>& p,
        const std::shared_ptr<hb::NativeService>& target)
    {
        auto packets = std::move(p->output);
        p->output = {};
        for (const auto& bytes : packets) {
            const auto packet = decode_packet(bytes);
            REQUIRE(packet);
            REQUIRE(target->enqueue(packet.packet, p->now));
        }
    }
    void round()
    {
        caller->pump_for_testing();
        deliver(cp, listener);
        listener->pump_for_testing();
        deliver(lp, caller);
    }
    hb::Message request(std::uint64_t serial = 1, std::uint64_t revision = 0,
        std::uint64_t position = 0)
    {
        return {.kind = hb::Kind::fallback_request,
            .stream = generation,
            .member = receiver.id,
            .serial = serial,
            .revision = revision,
            .next_position = position,
            .next_sequence =
                initial.advanced(static_cast<std::uint32_t>(position)).value(),
            .mode = hb::Mode::unicast};
    }
    void submit(hb::Message message)
    {
        REQUIRE_EQ(caller->send(message, cp->now + 1000),
            hb::NativeService::Admission::accepted);
        round();
    }
    hb::Message reply()
    {
        for (unsigned i = 0; i < 4; ++i) {
            bridge->pump();
            round();
        }
        hb::NativeService::Event e;
        while (caller->pop(e))
            if (e.kind == hb::NativeService::Event::Kind::received)
                return e.message;
        const auto bs = bridge->snapshot();
        const auto ns = listener->snapshot();
        throw std::runtime_error("missing bridge reply: closed="
            + std::to_string(bs.closed) + " denied=" + std::to_string(bs.denied)
            + " replied=" + std::to_string(bs.replies)
            + " native_closed=" + std::to_string(ns.closed)
            + " native_rejected=" + std::to_string(ns.rejected));
    }
    Actor::Member member()
    {
        std::array<Actor::Member, 4> m;
        const auto count = f.actor->members(m);
        for (std::size_t i = 0; i < count; ++i)
            if (m[i].id == receiver.id)
                return m[i];
        throw std::runtime_error("missing member");
    }
};
}
TEST(hybrid_bridge_enrolls_exact_native_member_and_commits_bounded_prefix_once)
{
    BridgeFixture f;
    REQUIRE(f.bridge->snapshot().enrolled);
    REQUIRE_EQ(f.f.send(), Status::accepted);
    f.f.pump();
    f.submit(f.request());
    const auto commit = f.reply();
    REQUIRE_EQ(commit.kind, hb::Kind::delivery_commit);
    REQUIRE_EQ(commit.revision, 1U);
    REQUIRE_EQ(commit.next_position, 1U);
    REQUIRE_EQ(f.member().delivery, GroupOriginalDelivery::unicast);
    REQUIRE_EQ(f.f.actor->snapshot().unicast_reservations, 1U);
    f.submit(f.request());
    const auto retry = f.reply();
    REQUIRE_EQ(retry, commit);
    REQUIRE_EQ(f.bridge->snapshot().applied, 1U);
    REQUIRE_EQ(f.bridge->snapshot().duplicates, 1U);
    REQUIRE_EQ(f.member().revision, 1U);
}
TEST(
    hybrid_bridge_rejects_stale_revision_and_future_or_retired_position_without_mutation)
{
    for (unsigned mode = 0; mode < 3; ++mode) {
        BridgeFixture f;
        if (mode == 2) {
            REQUIRE_EQ(f.f.send(), Status::accepted);
            f.f.pump();
            f.receiver.ack(initial.next());
        }
        auto m = f.request();
        if (mode == 0)
            m.revision = 1;
        if (mode == 1) {
            m.next_position = 1;
            m.next_sequence = initial.next().value();
        }
        f.submit(m);
        const auto r = f.reply();
        REQUIRE_EQ(r.kind, hb::Kind::delivery_status);
        REQUIRE_EQ(r.outcome,
            mode == 0 ? hb::Outcome::stale : hb::Outcome::history_unavailable);
        REQUIRE_EQ(f.member().revision, 0U);
        REQUIRE_EQ(f.member().delivery, GroupOriginalDelivery::multicast);
    }
}
TEST(hybrid_bridge_budget_and_negotiated_prefix_limit_refuse_atomically)
{
    for (bool prefix : {false, true}) {
        BridgeFixture f(prefix ? 1 : 256, true, prefix ? 4 : 0);
        if (prefix) {
            REQUIRE_EQ(f.f.send(), Status::accepted);
            f.f.pump();
            f.f.now += 1ms;
            f.receiver.now += 1000;
            REQUIRE_EQ(f.f.send(), Status::accepted);
            f.f.pump();
        }
        f.submit(f.request());
        const auto r = f.reply();
        REQUIRE_EQ(r.outcome,
            prefix ? hb::Outcome::history_unavailable : hb::Outcome::budget);
        REQUIRE_EQ(f.member().revision, 0U);
        REQUIRE_EQ(f.f.actor->snapshot().unicast_reservations, 0U);
    }
}
TEST(hybrid_bridge_revocation_and_native_close_clear_commands_before_commit)
{
    for (unsigned mode = 0; mode < 3; ++mode) {
        BridgeFixture f;
        f.submit(f.request());
        if (mode == 0)
            f.lease->revoke();
        else if (mode == 1)
            close_connection_runtime(f.receiver.runtime);
        else
            ++f.lp->grant.generation;
        f.bridge->pump();
        REQUIRE(f.bridge->snapshot().closed);
        REQUIRE_EQ(f.member().revision, 0U);
    }
}
TEST(
    hybrid_bridge_external_policy_change_source_error_and_departure_fence_cached_work)
{
    for (unsigned mode = 0; mode < 3; ++mode) {
        BridgeFixture f;
        f.submit(f.request());
        if (mode == 0)
            REQUIRE_EQ(f.f.actor
                           ->set_delivery(
                               f.receiver.id, 0, GroupOriginalDelivery::overlap)
                           .status,
                Actor::TransitionStatus::applied);
        else if (mode == 1) {
            REQUIRE_EQ(f.f.send(), Status::accepted);
            f.f.output.error = Error::io_error;
            f.f.pump();
        } else
            f.f.actor->remove_member(f.receiver.id);
        f.bridge->pump();
        if (mode == 1) {
            REQUIRE(!f.bridge->snapshot().closed);
            REQUIRE(f.bridge->snapshot().source_failed);
            REQUIRE_EQ(f.reply().outcome, hb::Outcome::denied);
        } else
            REQUIRE(f.bridge->snapshot().closed);
        REQUIRE_EQ(f.bridge->snapshot().applied, 0U);
    }
}
TEST(hybrid_bridge_queued_expiry_and_history_ttl_do_not_advance_policy)
{
    for (bool ttl : {false, true}) {
        BridgeFixture f;
        if (ttl) {
            REQUIRE_EQ(f.f.send(f.f.origin + 6ms), Status::accepted);
            f.f.pump();
            f.receiver.now = 7000;
        }
        f.submit(f.request());
        if (!ttl)
            f.cp->now = f.lp->now = 1100;
        f.bridge->pump();
        REQUIRE_EQ(f.member().revision, 0U);
        if (ttl)
            REQUIRE_EQ(f.reply().outcome, hb::Outcome::history_unavailable);
    }
}
TEST(
    hybrid_bridge_exclusive_queue_claim_and_current_runtime_binding_are_required)
{
    BridgeFixture f(256, false);
    hb::NativeService::Event
        e; // Consuming an authenticated input before attachment forbids handoff.
    f.submit(f.request());
    REQUIRE(f.listener->pop(e));
    REQUIRE(!hb::CoordinatorBridge::create(f.f.actor, f.listener, f.lease, {}));
    BridgeFixture clean;
    REQUIRE(!hb::CoordinatorBridge::create(
        clean.f.actor, clean.listener, clean.lease, {}));
    REQUIRE_EQ(
        clean.listener->send({}, 1000), hb::NativeService::Admission::invalid);
    REQUIRE(!clean.listener->pop(e));
}
TEST(hybrid_bridge_malformed_or_changed_authorization_cannot_enroll)
{
    for (unsigned mode = 0; mode < 3; ++mode) {
        BridgeFixture f(256, false);
        auto grant = f.lp->grant;
        if (mode == 0)
            ++grant.generation;
        else if (mode == 1)
            grant.stream_id.size = grant.stream_id.bytes.size() + 1;
        auto lease = std::make_shared<hb::AuthorizationLease>(grant);
        if (mode == 2)
            lease->revoke();
        REQUIRE(
            !hb::CoordinatorBridge::create(f.f.actor, f.listener, lease, {}));
        REQUIRE(!f.listener->closed());
        auto valid =
            hb::CoordinatorBridge::create(f.f.actor, f.listener, f.lease, {});
        REQUIRE(valid);
    }
}
TEST(
    hybrid_setup_deferred_sender_bridge_retains_one_owner_until_runtime_enrollment)
{
    BridgeFixture f(256, false);
    std::shared_ptr<hb::CoordinatorBridge> prepared;
    {
        auto held = HybridBridgeTestAccess::hold(*f.receiver.runtime);
        REQUIRE(
            !hb::CoordinatorBridge::create(f.f.actor, f.listener, f.lease, {}));
        prepared =
            hb::CoordinatorBridge::prepare(f.f.actor, f.listener, f.lease, {});
        REQUIRE(prepared);
        REQUIRE(!prepared->snapshot().enrolled);
        for (unsigned i = 0; i < 8; ++i)
            prepared->pump();
        REQUIRE(!prepared->snapshot().enrolled);
        REQUIRE_EQ(f.f.actor->snapshot().unicast_members, 0U);
    }
    prepared->pump();
    REQUIRE(prepared->snapshot().enrolled);
    REQUIRE(!prepared->snapshot().closed);
    REQUIRE_EQ(prepared->snapshot().applied, 0U);
    REQUIRE_EQ(f.f.actor->snapshot().unicast_members, 0U);
}
TEST(hybrid_bridge_runtime_contention_defers_without_mutation_and_resumes)
{
    for (bool exhaust : {false, true}) {
        BridgeFixture f;
        f.submit(f.request());
        std::promise<void> held, release;
        auto released = release.get_future();
        auto blocker = std::async(std::launch::async, [&] {
            auto guard = HybridBridgeTestAccess::hold(*f.receiver.runtime);
            held.set_value();
            released.wait();
        });
        held.get_future().wait();
        for (unsigned i = 0; i < (exhaust ? 8U : 1U); ++i)
            f.bridge->pump();
        const auto state = f.bridge->snapshot();
        release.set_value();
        blocker.get();
        REQUIRE_EQ(state.applied, 0U);
        if (exhaust) {
            REQUIRE_EQ(state.expired, 1U);
            f.submit(f.request(2));
        }
        REQUIRE_EQ(f.reply().kind, hb::Kind::delivery_commit);
        REQUIRE_EQ(f.member().revision, 1U);
    }
}
TEST(hybrid_bridge_cached_commit_is_withheld_after_an_external_policy_revision)
{
    for (bool generation : {false, true}) {
        BridgeFixture f;
        f.submit(f.request());
        REQUIRE_EQ(f.reply().revision, 1U);
        if (generation)
            HybridBridgeTestAccess::stale_generation(*f.receiver.runtime);
        else
            REQUIRE_EQ(f.f.actor
                           ->set_delivery(f.receiver.id, 1,
                               GroupOriginalDelivery::multicast)
                           .status,
                Actor::TransitionStatus::applied);
        f.submit(f.request());
        f.bridge->pump();
        REQUIRE(f.bridge->snapshot().closed);
        REQUIRE_EQ(f.member().revision, generation ? 1U : 2U);
        REQUIRE_EQ(f.member().delivery,
            generation ? GroupOriginalDelivery::unicast
                       : GroupOriginalDelivery::multicast);
    }
}
TEST(
    hybrid_bridge_ready_is_explicitly_unsupported_and_closed_budget_cannot_switch)
{
    for (bool budget : {false, true}) {
        BridgeFixture f;
        auto m = f.request();
        if (budget)
            GroupSendCoordinatorTestAccess::budget(*f.f.actor)->close();
        else {
            m.kind = hb::Kind::multicast_ready;
            m.mode = hb::Mode::multicast;
            m.interval_us = 100;
        }
        if (budget)
            f.submit(m);
        else {
            REQUIRE_EQ(f.caller->send(m, 1100, 100),
                hb::NativeService::Admission::accepted);
            f.round();
        }
        const auto reply = f.reply();
        REQUIRE_EQ(reply.outcome,
            budget ? hb::Outcome::budget : hb::Outcome::unsupported);
        REQUIRE_EQ(f.member().revision, 0U);
    }
}
TEST(hybrid_bridge_command_backpressure_retries_cached_reply_without_reapplying)
{
    BridgeFixture f;
    f.submit(f.request());
    f.lp->would_block = 1;
    const auto r = f.reply();
    REQUIRE_EQ(r.kind, hb::Kind::delivery_commit);
    REQUIRE_EQ(f.bridge->snapshot().applied, 1U);
    REQUIRE_EQ(f.bridge->snapshot().replies, 2U);
    REQUIRE_EQ(f.member().revision, 1U);
}
TEST(hybrid_bridge_status_queries_never_replay_prefix_or_reserve_unicast)
{
    BridgeFixture f;
    REQUIRE_EQ(f.f.send(), Status::accepted);
    f.f.pump();
    GroupSendCoordinatorTestAccess::budget(*f.f.actor)->close();
    auto query = f.request();
    query.kind = hb::Kind::delivery_query;
    query.mode = hb::Mode::multicast;
    query.revision = 99;
    f.submit(query);
    const auto reply = f.reply();
    REQUIRE_EQ(reply.kind, hb::Kind::delivery_status);
    REQUIRE_EQ(reply.outcome, hb::Outcome::ok);
    REQUIRE_EQ(reply.request_serial, query.serial);
    REQUIRE_EQ(reply.revision, 0U);
    REQUIRE_EQ(reply.next_position, 1U);
    REQUIRE_EQ(f.member().delivery, GroupOriginalDelivery::multicast);
    REQUIRE_EQ(f.f.actor->snapshot().unicast_reservations, 0U);
    REQUIRE_EQ(f.bridge->snapshot().applied, 0U);
}
TEST(
    hybrid_reporting_sender_requires_explicit_activity_and_reports_actual_udp_only)
{
    BridgeFixture f(256, true, 4, {100, 1000});
    f.bridge->pump();
    REQUIRE_EQ(f.bridge->snapshot().reports_queued, 0U);
    REQUIRE(
        f.f.actor->declare_source_activity(hb::Activity::producing, 1, 2000));
    const auto advance = [&] {
        f.cp->now += 100;
        f.lp->now += 100;
        f.f.now += 100us;
        f.receiver.now += 100;
        f.bridge->pump();
        for (unsigned i = 0; i < 3; ++i)
            f.round();
        f.bridge->pump();
    };
    const auto latest = [&] {
        hb::NativeService::Event e;
        std::optional<hb::Message> m;
        while (f.caller->pop(e))
            if (e.kind == hb::NativeService::Event::Kind::received
                && e.message.kind == hb::Kind::stream_progress)
                m = e.message;
        if (!m)
            throw std::runtime_error("missing progress: queued="
                + std::to_string(f.bridge->snapshot().reports_queued)
                + " skipped="
                + std::to_string(f.bridge->snapshot().reports_skipped)
                + " submitted="
                + std::to_string(f.bridge->snapshot().reports_submitted)
                + " native_rejected="
                + std::to_string(f.listener->snapshot().rejected)
                + " closed=" + std::to_string(f.bridge->snapshot().closed));
        return *m;
    };
    advance();
    auto m = latest();
    REQUIRE_EQ(m.produced_next, 1U);
    REQUIRE_EQ(m.next_position, 0U);
    REQUIRE_EQ(f.f.send(), Status::accepted);
    advance();
    m = latest();
    REQUIRE_EQ(m.next_position, 0U);
    REQUIRE_EQ(m.multicast_next, 0U);
    f.f.pump();
    advance();
    m = latest();
    REQUIRE_EQ(m.next_position, 1U);
    REQUIRE_EQ(m.multicast_next, 1U);
    REQUIRE_EQ(f.bridge->snapshot().applied, 0U);
    REQUIRE_EQ(f.member().revision, 0U);
    REQUIRE(f.f.actor->declare_source_activity(hb::Activity::paused, 1, 2000));
    advance();
    m = latest();
    REQUIRE_EQ(m.activity, hb::Activity::paused);
    REQUIRE_EQ(m.next_position, 1U);
    REQUIRE(f.f.actor->declare_source_activity(hb::Activity::ended, 1, 2000));
    advance();
    m = latest();
    REQUIRE_EQ(m.activity, hb::Activity::ended);
    f.receiver.ack(initial.next());
    REQUIRE_EQ(f.f.send(), Status::accepted);
    const auto queued = f.bridge->snapshot().reports_queued;
    advance();
    REQUIRE_EQ(f.bridge->snapshot().reports_queued, queued);
    REQUIRE(!f.f.actor->data_progress(f.receiver.id)->activity);
    REQUIRE(!f.bridge->snapshot().closed);
}
TEST(
    hybrid_reporting_sender_bounds_queue_pressure_eagain_and_expired_declarations)
{
    BridgeFixture f(256, true, 4, {100, 400});
    REQUIRE(f.f.actor->declare_source_activity(hb::Activity::paused, 0, 500));
    f.bridge->pump();
    REQUIRE_EQ(f.bridge->snapshot().reports_queued, 1U);
    for (unsigned i = 0; i < 50; ++i)
        f.bridge->pump();
    REQUIRE_EQ(f.bridge->snapshot().reports_queued, 1U);
    REQUIRE_EQ(f.listener->snapshot().queued, 1U);
    f.lp->would_block = 1;
    f.listener->pump_for_testing();
    f.bridge->pump();
    REQUIRE_EQ(f.bridge->snapshot().reports_submitted, 0U);
    f.cp->now += 100;
    f.lp->now += 100;
    f.f.now += 100us;
    f.bridge->pump();
    REQUIRE_EQ(f.bridge->snapshot().reports_queued, 2U);
    f.cp->now += 500;
    f.lp->now += 500;
    f.f.now += 500us;
    f.listener->pump_for_testing();
    f.bridge->pump();
    REQUIRE_EQ(f.bridge->snapshot().reports_submitted, 0U);
    REQUIRE_EQ(f.bridge->snapshot().reports_queued, 2U);
    REQUIRE_EQ(f.bridge->snapshot().applied, 0U);
    REQUIRE(!f.f.actor->data_progress(f.receiver.id)->activity);
    f.lease->revoke();
    REQUIRE(!f.bridge->peer_report(100));
    REQUIRE(f.bridge->snapshot().closed);
    BridgeFixture full(256, true, 4, {100, 400});
    REQUIRE(
        full.f.actor->declare_source_activity(hb::Activity::paused, 0, 1000));
    auto path = full.request();
    path.kind = hb::Kind::path_report;
    path.mode = hb::Mode::multicast;
    path.interval_us = 100;
    REQUIRE_EQ(full.caller->send(path, full.cp->now + 1000, full.cp->now),
        hb::NativeService::Admission::accepted);
    full.caller->pump_for_testing();
    REQUIRE(!full.cp->output.empty());
    const auto wire = decode_packet(full.cp->output.back());
    REQUIRE(wire);
    for (unsigned i = 0; i < hb::NativeService::capacity; ++i)
        REQUIRE(full.listener->enqueue(wire.packet, full.lp->now));
    full.bridge->pump();
    REQUIRE_EQ(full.bridge->snapshot().reports_queued, 0U);
    REQUIRE_EQ(full.bridge->snapshot().reports_skipped, 1U);
    for (unsigned i = 0; i < 50; ++i)
        full.bridge->pump();
    REQUIRE_EQ(full.bridge->snapshot().reports_skipped, 1U);
}
TEST(
    hybrid_reporting_sender_consumes_authenticated_path_reports_without_policy_mutation)
{
    BridgeFixture f;
    auto m = f.request();
    m.kind = hb::Kind::path_report;
    m.mode = hb::Mode::multicast;
    m.interval_us = 100;
    REQUIRE_EQ(f.caller->send(m, f.cp->now + 1000, f.cp->now),
        hb::NativeService::Admission::accepted);
    f.round();
    f.bridge->pump();
    auto p = f.bridge->peer_report(100);
    REQUIRE(p);
    REQUIRE(p->fresh);
    REQUIRE_EQ(p->message.kind, hb::Kind::path_report);
    REQUIRE_EQ(p->next_advanced_at_us, 0U);
    f.cp->now += 100;
    f.lp->now += 100;
    REQUIRE(!f.bridge->peer_report(100)->fresh);
    REQUIRE_EQ(f.bridge->snapshot().applied, 0U);
    auto fallback = f.request(2);
    f.submit(fallback);
    const auto reply = f.reply();
    REQUIRE_EQ(reply.kind, hb::Kind::delivery_commit);
    REQUIRE_EQ(f.bridge->snapshot().applied, 1U);
}
namespace {
hb::ReturnPolicy return_policy()
{
    return {1000, 8, 2, 10, 500};
}
void ready(BridgeFixture& f, std::uint64_t serial, std::uint64_t revision,
    hb::Mode mode, std::uint64_t begin = 0, std::uint64_t end = 0,
    std::uint32_t interval = 10)
{
    auto m = f.request(serial, revision, end);
    m.kind = hb::Kind::multicast_ready;
    m.mode = mode;
    m.multicast_next = begin;
    m.interval_us = interval;
    REQUIRE_EQ(f.caller->send(m, f.cp->now + 1000, f.cp->now),
        hb::NativeService::Admission::accepted);
    f.round();
}
void to_unicast(BridgeFixture& f)
{
    f.submit(f.request());
    REQUIRE_EQ(f.reply().mode, hb::Mode::unicast);
}
}
TEST(
    hybrid_return_sender_commits_overlap_then_multicast_without_replaying_or_releasing_queued_uc)
{
    auto p = return_policy();
    p.maximum_overlap_packets = 2;
    BridgeFixture f(256, true, 4, {}, p);
    to_unicast(f);
    ready(f, 2, 1, hb::Mode::overlap);
    auto reply = f.reply();
    REQUIRE_EQ(reply.mode, hb::Mode::overlap);
    REQUIRE_EQ(reply.revision, 2U);
    REQUIRE_EQ(reply.multicast_next, 0U);
    for (unsigned i = 0; i < 2; ++i)
        REQUIRE_EQ(f.f.send(), Status::accepted);
    f.f.pump();
    ready(f, 3, 2, hb::Mode::multicast, 0, 2);
    reply = f.reply();
    REQUIRE_EQ(reply.mode, hb::Mode::multicast);
    REQUIRE_EQ(reply.revision, 3U);
    REQUIRE_EQ(reply.multicast_next, 2U);
    REQUIRE_EQ(f.f.actor->snapshot().unicast_members, 0U);
    REQUIRE_EQ(f.f.actor->snapshot().unicast_reservations, 1U);
    REQUIRE_EQ(f.receiver.buffered(), 2U);
    REQUIRE_EQ(f.bridge->snapshot().applied, 3U);
}
TEST(
    hybrid_return_sender_expiry_reverts_to_uc_by_time_and_before_excess_admission)
{
    for (bool packets : {false, true}) {
        auto p = return_policy();
        p.maximum_overlap_packets = 2;
        BridgeFixture f(256, true, 4, {}, p);
        to_unicast(f);
        ready(f, 2, 1, hb::Mode::overlap);
        REQUIRE_EQ(f.reply().mode, hb::Mode::overlap);
        if (packets) {
            for (unsigned i = 0; i < 3; ++i)
                REQUIRE_EQ(f.f.send(), Status::accepted);
        } else {
            f.f.now += 1000us;
            f.cp->now += 1000;
            f.lp->now += 1000;
            f.f.pump();
        }
        REQUIRE_EQ(f.member().delivery, GroupOriginalDelivery::unicast);
        REQUIRE_EQ(f.member().revision, 3U);
        f.bridge->pump();
        REQUIRE(!f.bridge->snapshot().closed);
        REQUIRE_EQ(f.bridge->snapshot().overlap_expirations, 1U);
        REQUIRE_EQ(f.f.actor->snapshot().unicast_reservations, 1U);
    }
}
TEST(
    hybrid_return_sender_rejects_wrong_boundary_short_stale_or_out_of_horizon_coverage)
{
    for (unsigned wrong = 0; wrong < 4; ++wrong) {
        BridgeFixture f(256, true, 4, {}, return_policy());
        to_unicast(f);
        ready(f, 2, 1, hb::Mode::overlap);
        (void)f.reply();
        for (unsigned i = 0; i < 2; ++i)
            REQUIRE_EQ(f.f.send(), Status::accepted);
        f.f.pump();
        auto m = f.request(3, 2, wrong == 1 ? 1 : wrong == 2 ? 3 : 2);
        m.kind = hb::Kind::multicast_ready;
        m.mode = hb::Mode::multicast;
        m.multicast_next = wrong == 0 ? 1 : 0;
        m.interval_us = wrong == 3 ? 1 : 10;
        REQUIRE_EQ(f.caller->send(m, f.cp->now + 1000, f.cp->now),
            hb::NativeService::Admission::accepted);
        f.round();
        const auto denied = f.reply();
        REQUIRE_EQ(denied.outcome, hb::Outcome::denied);
        REQUIRE_EQ(f.member().delivery, GroupOriginalDelivery::overlap);
        REQUIRE_EQ(f.bridge->snapshot().applied, 2U);
    }
}
TEST(
    hybrid_return_sender_old_queued_proofs_and_unselected_policy_cannot_release_uc)
{
    BridgeFixture legacy;
    to_unicast(legacy);
    ready(legacy, 2, 1, hb::Mode::overlap);
    REQUIRE_EQ(legacy.reply().outcome, hb::Outcome::unsupported);
    REQUIRE_EQ(legacy.member().delivery, GroupOriginalDelivery::unicast);
    BridgeFixture f(256, true, 4, {}, return_policy());
    to_unicast(f);
    ready(f, 2, 1, hb::Mode::overlap);
    (void)f.reply();
    for (unsigned i = 0; i < 2; ++i)
        REQUIRE_EQ(f.f.send(), Status::accepted);
    f.f.pump();
    auto m = f.request(3, 2, 2);
    m.kind = hb::Kind::multicast_ready;
    m.mode = hb::Mode::multicast;
    m.multicast_next = 0;
    m.interval_us = 10;
    REQUIRE_EQ(f.caller->send(m, f.cp->now + 1000, f.cp->now),
        hb::NativeService::Admission::accepted);
    f.cp->now += 500;
    f.lp->now += 500;
    f.f.now += 500us;
    f.receiver.now += 500;
    f.round();
    f.bridge->pump();
    f.round();
    REQUIRE_EQ(f.member().delivery, GroupOriginalDelivery::overlap);
    REQUIRE_EQ(f.bridge->snapshot().applied, 2U);
    auto query = f.request(4);
    query.kind = hb::Kind::delivery_query;
    query.mode = hb::Mode::multicast;
    f.submit(query);
    REQUIRE_EQ(f.reply().mode, hb::Mode::overlap);
}
TEST(
    hybrid_return_sender_fresh_serial_retries_do_not_reset_overlap_boundary_or_budget)
{
    BridgeFixture f(256, true, 4, {}, return_policy());
    to_unicast(f);
    ready(f, 2, 1, hb::Mode::overlap);
    const auto first = f.reply();
    const auto deadline = f.bridge->snapshot().overlap_deadline_us;
    ready(f, 3, 1, hb::Mode::overlap);
    const auto retry = f.reply();
    REQUIRE_EQ(retry.outcome, hb::Outcome::stale);
    REQUIRE_EQ(retry.multicast_next, first.multicast_next);
    REQUIRE_EQ(f.bridge->snapshot().overlap_deadline_us, deadline);
    REQUIRE_EQ(f.bridge->snapshot().applied, 2U);
}
TEST(hybrid_progress_sender_admission_and_eagain_are_not_multicast_submissions)
{
    Fixture f;
    Receiver r(f);
    f.output.data_failures = 1;
    REQUIRE_EQ(f.send(), Status::accepted);
    auto s = f.actor->data_progress(r.id);
    REQUIRE(s);
    REQUIRE_EQ(s->admitted_next, 1U);
    REQUIRE(!s->multicast.observed);
    f.pump();
    REQUIRE(!f.actor->data_progress(r.id)->multicast.observed);
    f.now += 2ms;
    r.now += 2000;
    f.pump();
    s = f.actor->data_progress(r.id);
    REQUIRE(s);
    REQUIRE_EQ(s->multicast.next, 1U);
    REQUIRE_EQ(s->multicast.packets, 1U);
    REQUIRE(!s->unicast.observed);
    REQUIRE_EQ(s->multicast.last_advance_us,
        static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                f.now.time_since_epoch())
                .count()));
}
TEST(hybrid_progress_sender_unicast_records_only_full_native_udp_completion)
{
    Fixture f;
    Receiver r(f);
    REQUIRE_EQ(
        f.actor->set_delivery(r.id, 0, GroupOriginalDelivery::unicast).status,
        Actor::TransitionStatus::applied);
    REQUIRE_EQ(f.send(), Status::accepted);
    REQUIRE(!f.actor->data_progress(r.id)->unicast.observed);
    r.output.data_failures = 1;
    (void)r.runtime->poll();
    REQUIRE(!f.actor->data_progress(r.id)->unicast.observed);
    r.now += 2000;
    (void)r.runtime->poll();
    const auto s = f.actor->data_progress(r.id);
    REQUIRE(s);
    REQUIRE_EQ(s->unicast.next, 1U);
    REQUIRE_EQ(s->unicast.packets, 1U);
    REQUIRE(!s->multicast.observed);
}
TEST(hybrid_progress_sender_overlap_paths_remain_separate_high_waters)
{
    Fixture f;
    Receiver r(f);
    REQUIRE_EQ(
        f.actor->set_delivery(r.id, 0, GroupOriginalDelivery::overlap).status,
        Actor::TransitionStatus::applied);
    REQUIRE_EQ(f.send(), Status::accepted);
    f.pump();
    (void)r.runtime->poll();
    const auto s = f.actor->data_progress(r.id);
    REQUIRE(s);
    REQUIRE_EQ(s->multicast.next, 1U);
    REQUIRE_EQ(s->unicast.next, 1U);
    REQUIRE_EQ(s->multicast.packets, 1U);
    REQUIRE_EQ(s->unicast.packets, 1U);
}
TEST(hybrid_progress_sender_partial_and_failed_data_have_no_success_sample)
{
    for (bool partial : {false, true}) {
        Fixture f;
        Receiver r(f);
        f.output.partial = partial;
        if (!partial)
            f.output.error = Error::io_error;
        REQUIRE_EQ(f.send(), Status::accepted);
        REQUIRE(!f.actor->data_progress(r.id)->multicast.observed);
        f.pump();
        const auto failed = f.actor->data_progress(r.id);
        REQUIRE(failed);
        REQUIRE(failed->source_failed);
        REQUIRE(!failed->multicast.observed);
        REQUIRE(!failed->unicast.observed);
        REQUIRE(f.actor->snapshot().failed);
        Fixture u;
        Receiver receiver(u);
        REQUIRE_EQ(
            u.actor->set_delivery(
                       receiver.id, 0, GroupOriginalDelivery::unicast)
                .status,
            Actor::TransitionStatus::applied);
        REQUIRE_EQ(u.send(), Status::accepted);
        REQUIRE(!u.actor->data_progress(receiver.id)->unicast.observed);
        receiver.output.partial = partial;
        if (!partial)
            receiver.output.error = Error::io_error;
        (void)receiver.runtime->poll();
        REQUIRE(!u.actor->data_progress(receiver.id));
    }
}
TEST(
    hybrid_progress_sender_member_replacement_does_not_reuse_unicast_observations)
{
    Fixture f;
    Receiver r(f);
    REQUIRE_EQ(
        f.actor->set_delivery(r.id, 0, GroupOriginalDelivery::unicast).status,
        Actor::TransitionStatus::applied);
    REQUIRE_EQ(f.send(), Status::accepted);
    (void)r.runtime->poll();
    f.pump();
    REQUIRE(f.actor->data_progress(r.id)->unicast.observed);
    r.ack(initial.next());
    const auto old = r.id;
    f.actor->remove_member(old);
    REQUIRE(!f.actor->data_progress(old));
    const auto replacement = f.actor->add_member(r.runtime);
    REQUIRE(replacement);
    const auto s = f.actor->data_progress(replacement->id);
    REQUIRE(s);
    REQUIRE_EQ(s->member_origin, 1U);
    REQUIRE_EQ(s->unicast.next, 1U);
    REQUIRE(!s->unicast.observed);
    f.actor->close();
    REQUIRE(!f.actor->data_progress(replacement->id));
}
TEST(
    hybrid_progress_sender_late_join_maps_runtime_positions_after_unused_frames)
{
    Fixture f;
    REQUIRE_EQ(f.send(), Status::advanced_without_receivers);
    Receiver r(f, 8, true, initial.next());
    auto s = f.actor->data_progress(r.id);
    REQUIRE(s);
    REQUIRE_EQ(s->member_origin, 1U);
    REQUIRE(!s->multicast.observed);
    REQUIRE_EQ(
        f.actor->set_delivery(r.id, 0, GroupOriginalDelivery::unicast).status,
        Actor::TransitionStatus::applied);
    REQUIRE_EQ(f.send(), Status::accepted);
    (void)r.runtime->poll();
    s = f.actor->data_progress(r.id);
    REQUIRE(s);
    REQUIRE_EQ(s->unicast.next, 2U);
    REQUIRE_EQ(s->history_floor, 1U);
}
TEST(hybrid_progress_sender_prefix_repairs_are_not_unicast_originals)
{
    for (bool submitted : {false, true}) {
        Fixture f;
        Receiver r(f);
        REQUIRE_EQ(f.send(), Status::accepted);
        if (submitted)
            f.pump();
        REQUIRE_EQ(
            f.actor->set_delivery(r.id, 0, GroupOriginalDelivery::unicast, true)
                .status,
            Actor::TransitionStatus::applied);
        (void)r.runtime->poll();
        const auto s = f.actor->data_progress(r.id);
        REQUIRE(s);
        REQUIRE_EQ(s->unicast.observed, !submitted);
        if (!submitted)
            REQUIRE_EQ(s->unicast.next, 1U);
    }
}
#endif

#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_HYBRID_CONTROL
TEST(
    hybrid_source_error_bridge_reports_failed_multicast_and_answers_read_only_queries)
{
    for (bool partial : {false, true}) {
        BridgeFixture f(256, true, 4, {100, 1000});
        REQUIRE_EQ(f.f.send(), Status::accepted);
        f.f.output.partial = partial;
        if (!partial)
            f.f.output.error = Error::io_error;
        f.f.pump();
        REQUIRE(f.f.actor->snapshot().failed);
        f.cp->now += 100;
        f.lp->now += 100;
        f.f.now += 100us;
        f.bridge->pump();
        for (unsigned i = 0; i < 3; ++i)
            f.round();
        auto r = f.reply();
        REQUIRE_EQ(r.kind, hb::Kind::stream_progress);
        REQUIRE_EQ(r.activity, hb::Activity::source_failed);
        REQUIRE_EQ(r.produced_next, 1U);
        REQUIRE_EQ(r.next_position, 0U);
        REQUIRE_EQ(r.multicast_next, 0U);
        auto q = f.request();
        q.kind = hb::Kind::delivery_query;
        q.mode = hb::Mode::multicast;
        f.submit(q);
        r = f.reply();
        REQUIRE_EQ(r.kind, hb::Kind::delivery_status);
        REQUIRE_EQ(r.outcome, hb::Outcome::ok);
        REQUIRE_EQ(r.revision, 0U);
        REQUIRE_EQ(r.mode, hb::Mode::multicast);
        f.submit(f.request(2));
        r = f.reply();
        REQUIRE_EQ(r.outcome, hb::Outcome::denied);
        REQUIRE_EQ(f.member().revision, 0U);
        REQUIRE_EQ(f.bridge->snapshot().applied, 0U);
        REQUIRE_EQ(f.f.actor->snapshot().unicast_reservations, 0U);
        REQUIRE(!f.bridge->snapshot().closed);
    }
}
TEST(hybrid_source_error_bridge_adopts_only_exact_preapproved_unicast_recovery)
{
    BridgeFixture f;
    f.submit(f.request());
    REQUIRE_EQ(f.reply().outcome, hb::Outcome::ok);
    REQUIRE_EQ(f.member().revision, 1U);
    REQUIRE_EQ(f.f.send(), Status::accepted);
    f.f.output.error = Error::io_error;
    f.f.pump();
    REQUIRE_EQ(
        recover_source(f.f, {{f.receiver.id, 1}}).status, Transition::applied);
    auto q = f.request(2);
    q.kind = hb::Kind::delivery_query;
    q.mode = hb::Mode::multicast;
    f.submit(q);
    const auto r = f.reply();
    REQUIRE_EQ(r.revision, 2U);
    REQUIRE_EQ(r.mode, hb::Mode::unicast);
    REQUIRE_EQ(r.outcome, hb::Outcome::ok);
    REQUIRE_EQ(f.bridge->snapshot().source_recoveries, 1U);
    REQUIRE(f.bridge->snapshot().source_failed);
    REQUIRE(f.bridge->snapshot().unicast_only);
    REQUIRE(!f.bridge->snapshot().closed);
    const auto replies = f.bridge->snapshot().replies;
    f.submit(f.request()); // old cached commit cannot be rearmed after recovery
    for (unsigned i = 0; i < 4; ++i) {
        f.bridge->pump();
        f.round();
    }
    REQUIRE_EQ(f.bridge->snapshot().replies, replies);
    f.submit(f.request(3, 2));
    REQUIRE_EQ(f.reply().outcome, hb::Outcome::denied);
    REQUIRE_EQ(f.member().revision, 2U);
    REQUIRE_EQ(f.bridge->snapshot().applied, 1U);
    f.lease->revoke();
    f.bridge->pump();
    REQUIRE(f.bridge->snapshot().closed);
}
TEST(
    hybrid_source_error_bridge_refuses_recovery_of_members_without_existing_uc_approval)
{
    BridgeFixture f;
    REQUIRE_EQ(f.f.send(), Status::accepted);
    f.f.output.error = Error::io_error;
    f.f.pump();
    REQUIRE_EQ(
        recover_source(f.f, {{f.receiver.id, 0}}).status, Transition::applied);
    auto q = f.request(2);
    q.kind = hb::Kind::delivery_query;
    q.mode = hb::Mode::multicast;
    f.submit(q);
    f.bridge->pump();
    REQUIRE(f.bridge->snapshot().closed);
    REQUIRE_EQ(f.bridge->snapshot().source_recoveries, 0U);
    REQUIRE_EQ(f.member().delivery, GroupOriginalDelivery::unicast);
}
TEST(
    hybrid_source_error_bridge_internal_failure_and_missing_receipts_remain_terminal)
{
    for (unsigned mode = 0; mode < 4; ++mode) {
        BridgeFixture f;
        f.submit(f.request());
        (void)f.reply();
        if (mode == 0)
            GroupSendCoordinatorTestAccess::fail(*f.f.actor);
        else {
            REQUIRE_EQ(f.f.send(), Status::accepted);
            f.f.output.error = Error::io_error;
            f.f.pump();
            REQUIRE_EQ(recover_source(f.f, {{f.receiver.id, 1}}).status,
                Transition::applied);
            if (mode == 1)
                GroupSendCoordinatorTestAccess::source_revision(*f.f.actor, 2);
            if (mode == 2)
                GroupSendCoordinatorTestAccess::revision(
                    *f.f.actor, f.receiver.id, 3);
            if (mode == 3)
                f.lease->revoke();
        }
        auto q = f.request(2);
        q.kind = hb::Kind::delivery_query;
        q.mode = hb::Mode::multicast;
        if (mode != 3)
            f.submit(q);
        f.bridge->pump();
        REQUIRE(f.bridge->snapshot().closed);
        REQUIRE(!f.bridge->data_progress());
    }
}
#endif
