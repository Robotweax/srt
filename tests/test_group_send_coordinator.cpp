#include "test.hpp"
#include "compat/group_send_coordinator.hpp"
#include "robotweax/srt/control.hpp"

#include <atomic>
#include <condition_variable>
#include <future>
#include <thread>

using namespace robotweax::srt;
using namespace robotweax::srt::compat;
using namespace std::chrono_literals;

namespace robotweax::srt::compat {
struct GroupSendCoordinatorTestAccess {
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
    static UdpIoResult send(
        std::span<const std::byte> bytes, IpEndpoint, void* p) noexcept
    {
        auto& self = *static_cast<Output*>(p);
        std::lock_guard lock(self.mutex);
        self.packets.emplace_back(bytes.begin(), bytes.end());
        self.changed.notify_all();
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
        std::size_t capacity = 4, SequenceNumber start = initial)
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
            .maximum_busy_attempts = 2};
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
    explicit Receiver(Fixture& f, std::size_t capacity = 4, bool add = true,
        SequenceNumber start = initial, bool realtime = false)
    {
        channel->set_send_hook_for_testing(Output::send, &output);
        SocketOptions options;
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
    void ack(SequenceNumber next)
    {
        std::array<std::byte, 64> bytes {};
        const auto encoded = encode_acknowledgement_payload(
            {.kind = AcknowledgementKind::lite, .next_sequence = next}, bytes);
        REQUIRE(encoded);
        runtime->process_packet(
            {.kind = PacketKind::control,
                .control = {.type = ControlType::acknowledgement,
                    .destination_socket_id = 107},
                .payload = std::span {bytes}.first(encoded.bytes_written)},
            peer);
    }
    std::size_t buffered()
    {
        return runtime->buffer_packet_counts().unacknowledged_send;
    }
    void exchange_keys()
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
            REQUIRE(p.packet.kind != PacketKind::data);
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
            persistent ? Status::advanced_without_receivers : Status::accepted);
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
