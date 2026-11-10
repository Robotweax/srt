#include "test.hpp"

#include "compat/group_runtime_admission.hpp"
#include "robotweax/srt/control.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <future>
#include <limits>
#include <vector>

using namespace robotweax::srt;
using namespace robotweax::srt::compat;
using namespace std::chrono_literals;

namespace robotweax::srt::compat {
struct GroupRuntimeAdmissionTestAccess {
    static void generation(ConnectionRuntime& runtime, std::uint64_t value)
    {
        std::lock_guard lock(runtime.mutex_);
        runtime.group_send_state_->generation = value;
    }
};
}
namespace {
using Admission = GroupRuntimeAdmission;
using Status = Admission::Status;
using Application = Admission::Application;
using Clock = ConnectionRuntime::Clock;
constexpr std::uint32_t group_id = group_handle_mask | 97U;
constexpr std::uint64_t stream_generation = 61;
constexpr SequenceNumber initial {1'000};
constexpr std::array<std::byte, 3> clear {
    std::byte {'a'}, std::byte {'b'}, std::byte {'c'}};
struct Sent {
    std::vector<std::byte> bytes;
    IpEndpoint destination;
};
struct Fixture {
    Clock::time_point origin = Clock::now();
    std::uint64_t now = 5'000;
    std::vector<Sent> sent;
    std::shared_ptr<DatagramChannel> channel =
        std::make_shared<DatagramChannel>();
    const IpEndpoint peer {.address = {192, 0, 2, 97}, .port = 14997};
    SocketOptions options;
    std::shared_ptr<SharedTransmitKeySchedule> schedule;
    std::shared_ptr<CryptoSession> crypto;
    std::unique_ptr<CryptoSession> receiver;
    std::shared_ptr<ConnectionRuntime> runtime;
    Admission::Binding binding;
    std::unique_ptr<GroupPacketEncoder> encoder;

    static UdpIoResult capture(
        std::span<const std::byte> bytes, IpEndpoint peer, void* p) noexcept
    {
        auto& self = *static_cast<Fixture*>(p);
        self.sent.push_back({{bytes.begin(), bytes.end()}, peer});
        return {.bytes_transferred = bytes.size()};
    }
    static std::uint64_t clock(void* p) noexcept
    {
        return *static_cast<std::uint64_t*>(p);
    }
    explicit Fixture(CryptoMode mode = CryptoMode::automatic,
        bool bind_now = true,
        std::shared_ptr<SharedTransmitKeySchedule> shared = {},
        Clock::time_point common_origin = {})
    {
        if (common_origin != Clock::time_point {})
            origin = common_origin;
        channel->set_send_hook_for_testing(capture, this);
        REQUIRE_EQ(
            options.set(SocketOption::send_buffer_packets, 4), Error::none);
        REQUIRE_EQ(
            options.set(SocketOption::receive_buffer_packets, 4), Error::none);
        if (mode != CryptoMode::automatic) {
            schedule = std::move(shared);
            if (!schedule) {
                schedule = std::make_shared<SharedTransmitKeySchedule>(
                    SharedTransmitKeySchedule::Configuration {.mode = mode,
                        .refresh_rate_packets = 4,
                        .preannouncement_packets = 1,
                        .origin = initial});
                REQUIRE_EQ(schedule->initialize(), Error::none);
            }
            CryptoConfiguration c {
                .passphrase = "runtime multicast guard fixture",
                .mode = mode,
                .enable_aes_gcm = true,
                .key_length = 16,
                .refresh_rate_packets = 4,
                .preannouncement_packets = 1};
            CryptoSession initiator(c);
            crypto = std::make_shared<CryptoSession>(c);
            receiver = std::make_unique<CryptoSession>(c);
            REQUIRE_EQ(initiator.start_initiator(), Error::none);
            REQUIRE_EQ(crypto->accept_key_material(
                           initiator.pending_key_material(), true),
                Error::none);
            REQUIRE_EQ(
                crypto->attach_shared_transmit_schedule(schedule), Error::none);
            REQUIRE_EQ(receiver->accept_key_material(
                           crypto->pending_key_material(), false),
                Error::none);
            REQUIRE_EQ(crypto->acknowledge_key_material(
                           receiver->key_material_response(), false),
                Error::none);
            REQUIRE(crypto->ready_to_send_data());
        }
        runtime = std::make_shared<ConnectionRuntime>(
            ConnectionRuntime::Configuration {.channel = channel,
                .peer = peer,
                .local_socket_id = 97,
                .peer_socket_id = 98,
                .initial_sequence = initial,
                .flow_window_packets = 1,
                .peer_flow_window_packets = 1,
                .options = options,
                .origin = origin,
                .crypto = crypto,
                .now_function = clock,
                .now_context = &now});
        if (bind_now) {
            binding = Admission::bind(runtime, configuration());
            REQUIRE(binding);
        }
        encoder = GroupPacketEncoder::create({.group_id = group_id,
            .stream_generation = stream_generation,
            .initial_sequence = initial,
            .key_schedule = schedule});
        REQUIRE(encoder);
    }
    Admission::Configuration configuration() const
    {
        return {.group_id = group_id,
            .stream_generation = stream_generation,
            .timestamp_origin = origin,
            .key_schedule = schedule};
    }
    DataHeader header(unsigned offset = 0) const
    {
        return {.sequence = initial.advanced(offset),
            .message_number = offset + 1,
            .boundary = MessageBoundary::solo,
            .in_order = true,
            .timestamp = PacketTimestamp {static_cast<std::uint32_t>(now)},
            .destination_socket_id = group_id};
    }
    std::pair<Admission::Ticket,
        std::shared_ptr<const GroupPacketEncoder::Packet>>
    admit(unsigned offset = 0)
    {
        auto guard =
            Admission::try_reserve(binding, header(offset), clear.size());
        REQUIRE_EQ(guard.status(), Status::ready);
        auto packet = encoder->prepare(header(offset), clear);
        REQUIRE(packet);
        REQUIRE(guard.stage(packet.packet));
        REQUIRE(encoder->commit(packet.packet));
        auto ticket = guard.commit();
        REQUIRE(ticket);
        REQUIRE(!guard.commit());
        guard.release();
        Admission::notify_work(binding);
        return {*ticket, packet.packet};
    }
    GroupMulticastEmitter::Outcome submitted(
        const Admission::Ticket& ticket) const
    {
        return {.group_id = group_id,
            .generation = stream_generation,
            .sequence = ticket.original.sequence,
            .disposition = GroupMulticastEmitter::Disposition::submitted,
            .completed_at = origin + std::chrono::microseconds {now}};
    }
    void ack(SequenceNumber next)
    {
        std::array<std::byte, 64> bytes {};
        const auto encoded = encode_acknowledgement_payload(
            {.kind = AcknowledgementKind::lite, .next_sequence = next}, bytes);
        REQUIRE(encoded);
        PacketView p {.kind = PacketKind::control,
            .control = {.type = ControlType::acknowledgement,
                .destination_socket_id = 97},
            .payload = std::span {bytes}.first(encoded.bytes_written)};
        runtime->process_packet(p, peer);
    }
    void nak(SequenceNumber sequence)
    {
        const SequenceRange range {sequence, sequence};
        std::array<std::byte, 16> bytes {};
        const auto encoded = encode_loss_ranges(std::span {&range, 1U}, bytes);
        REQUIRE(encoded);
        runtime->process_packet(
            {.kind = PacketKind::control,
                .control = {.type = ControlType::negative_acknowledgement,
                    .destination_socket_id = 97},
                .payload = std::span {bytes}.first(encoded.bytes_written)},
            peer);
    }
    std::vector<Sent> data() const
    {
        std::vector<Sent> result;
        for (const auto& datagram : sent) {
            const auto decoded = decode_packet(datagram.bytes);
            if (decoded && decoded.packet.kind == PacketKind::data)
                result.push_back(datagram);
        }
        return result;
    }
};
}

TEST(
    compat_group_runtime_binding_validates_origin_family_schedule_and_preserves_legacy_on_failure)
{
    Fixture f(CryptoMode::automatic, false);
    for (unsigned mutation = 0; mutation < 5; ++mutation) {
        auto c = f.configuration();
        switch (mutation) {
        case 0:
            c.group_id = 97;
            break;
        case 1:
            c.stream_generation = 0;
            break;
        case 2:
            c.timestamp_origin += 1ns;
            break;
        case 3:
            c.family = IpAddressFamily::ipv6;
            break;
        case 4:
            c.key_schedule = std::make_shared<SharedTransmitKeySchedule>(
                SharedTransmitKeySchedule::Configuration {});
            break;
        }
        REQUIRE(!Admission::bind(f.runtime, c));
    }
    REQUIRE(f.runtime->writable());
    REQUIRE_EQ(f.runtime->queue_message(clear, 0, true, false, 0).status,
        MessageIoStatus::success);
    REQUIRE(!Admission::bind(
        f.runtime, f.configuration())); // Never take over queued originals.
    Fixture encrypted(CryptoMode::aes_gcm, false);
    auto c = encrypted.configuration();
    c.key_schedule = std::make_shared<SharedTransmitKeySchedule>(
        SharedTransmitKeySchedule::Configuration {.mode = CryptoMode::aes_gcm});
    REQUIRE_EQ(c.key_schedule->initialize(), Error::none);
    REQUIRE(!Admission::bind(encrypted.runtime, c));
    REQUIRE(Admission::bind(encrypted.runtime, encrypted.configuration()));
}

TEST(
    compat_group_runtime_binding_excludes_ordinary_original_apis_and_legacy_owner_election)
{
    Fixture f;
    REQUIRE(!f.runtime->writable());
    REQUIRE((f.runtime->readiness_snapshot(false).events & SRT_EPOLL_OUT) == 0);
    REQUIRE_EQ(f.runtime->queue_message(clear, 0, true, false, 0).status,
        MessageIoStatus::invalid_state);
    REQUIRE_EQ(
        f.runtime->queue_group_message(clear, initial, 1, 0, true).status,
        MessageIoStatus::invalid_state);
    REQUIRE_EQ(f.runtime->queue_stream(clear, false, 0).status,
        MessageIoStatus::invalid_state);
    f.runtime->set_group_data_egress({});
    REQUIRE_EQ(
        Admission::try_reserve(f.binding, f.header(), clear.size()).status(),
        Status::stale);
    REQUIRE_EQ(f.runtime->group_data_egress().mode,
        ConnectionRuntime::GroupDataEgress::Mode::external_originals);
    REQUIRE_EQ(f.runtime->queue_message(clear, 0, true, false, 0).status,
        MessageIoStatus::invalid_state);
}

TEST(
    compat_group_runtime_guard_cancellation_is_transactional_and_stage_pins_exact_packet)
{
    Fixture f;
    {
        auto guard =
            Admission::try_reserve(f.binding, f.header(), clear.size());
        REQUIRE_EQ(guard.status(), Status::ready);
        REQUIRE(!guard.commit());
    }
    REQUIRE_EQ(f.runtime->buffer_packet_counts().unacknowledged_send, 0U);
    auto prepared = f.encoder->prepare(f.header(), clear);
    REQUIRE(prepared);
    {
        auto guard =
            Admission::try_reserve(f.binding, f.header(), clear.size());
        REQUIRE_EQ(guard.status(), Status::ready);
        REQUIRE(guard.stage(prepared.packet));
        REQUIRE(guard.stage(prepared.packet));
    }
    REQUIRE_EQ(f.runtime->response_health().next_send_sequence, initial);
    REQUIRE(f.encoder->snapshot().prepared);
    auto guard = Admission::try_reserve(f.binding, f.header(), clear.size());
    REQUIRE(guard.stage(prepared.packet));
    REQUIRE(f.encoder->commit(prepared.packet));
    REQUIRE(guard.commit());
    guard.release();
    REQUIRE_EQ(f.runtime->response_health().next_send_sequence, initial.next());
}

TEST(
    compat_group_runtime_guard_capacity_is_independent_of_peer_window_and_fails_before_key_preparation)
{
    Fixture f(CryptoMode::aes_gcm);
    for (unsigned i = 0; i < 4; ++i)
        (void)f.admit(i);
    REQUIRE_EQ(
        f.runtime->statistics(false, true).instantaneous.flow_window_packets,
        1U);
    REQUIRE_EQ(f.runtime->buffer_packet_counts().unacknowledged_send, 4U);
    const auto revision = f.crypto->key_material_revision();
    const auto frontier = f.schedule->frontier_position();
    REQUIRE_EQ(
        Admission::try_reserve(f.binding, f.header(4), clear.size()).status(),
        Status::full);
    REQUIRE_EQ(f.crypto->key_material_revision(), revision);
    REQUIRE_EQ(f.schedule->frontier_position(), frontier);
    REQUIRE(!f.encoder->snapshot().prepared);
    (void)f.runtime->poll();
    REQUIRE(f.data().empty());
}

TEST(
    compat_group_runtime_guard_rejects_wrong_shape_message_and_foreign_encoder_schedule)
{
    Fixture f(CryptoMode::aes_ctr);
    for (unsigned i = 0; i < 5; ++i) {
        auto h = f.header();
        switch (i) {
        case 0:
            h.destination_socket_id ^= 1;
            break;
        case 1:
            h.boundary = MessageBoundary::first;
            break;
        case 2:
            h.message_number = 0;
            break;
        case 3:
            h.sequence = initial.next();
            break;
        case 4:
            h.encryption_key = EncryptionKey::even;
            break;
        }
        REQUIRE_EQ(Admission::try_reserve(f.binding, h, clear.size()).status(),
            Status::invalid);
    }
    REQUIRE_EQ(f.schedule->frontier_position(), 0U);
    Fixture foreign(CryptoMode::aes_ctr);
    auto wrong = foreign.encoder->prepare(f.header(), clear);
    REQUIRE(wrong);
    auto guard = Admission::try_reserve(f.binding, f.header(), clear.size());
    REQUIRE_EQ(guard.status(), Status::ready);
    REQUIRE(!guard.stage(wrong.packet));
    auto correct = f.encoder->prepare(f.header(), clear);
    REQUIRE(correct);
    REQUIRE(guard.stage(correct.packet));
    guard.release();
    auto ticket = f.admit(0).first;
    auto bad_next = f.header(1);
    bad_next.message_number = 9;
    REQUIRE_EQ(
        Admission::try_reserve(f.binding, bad_next, clear.size()).status(),
        Status::invalid);
    (void)ticket;
}

TEST(
    compat_group_runtime_outcome_authorizes_only_matching_success_with_valid_runtime_time)
{
    Fixture f;
    auto [ticket, packet] = f.admit();
    auto report = f.submitted(ticket);
    auto wrong = report;
    wrong.generation++;
    REQUIRE_EQ(Admission::try_apply(ticket, wrong), Application::stale);
    wrong = report;
    wrong.sequence = initial.next();
    REQUIRE_EQ(Admission::try_apply(ticket, wrong), Application::stale);
    wrong = report;
    wrong.disposition = GroupMulticastEmitter::Disposition::expired;
    REQUIRE_EQ(Admission::try_apply(ticket, wrong), Application::not_submitted);
    wrong = report;
    wrong.completed_at = f.origin - 1ns;
    REQUIRE_EQ(Admission::try_apply(ticket, wrong), Application::invalid_time);
    wrong.completed_at = f.origin + std::chrono::microseconds {f.now + 1};
    REQUIRE_EQ(Admission::try_apply(ticket, wrong), Application::invalid_time);
    auto stale_position = ticket;
    stale_position.original.sequence_position++;
    REQUIRE_EQ(Admission::try_apply(stale_position, report),
        Application::duplicate_or_retired);
    f.ack(initial.next());
    REQUIRE_EQ(f.runtime->buffer_packet_counts().unacknowledged_send, 1U);
    REQUIRE_EQ(Admission::try_apply(ticket, report), Application::applied);
    REQUIRE_EQ(Admission::try_apply(ticket, report),
        Application::duplicate_or_retired);
    f.ack(initial.next());
    REQUIRE_EQ(f.runtime->buffer_packet_counts().unacknowledged_send, 0U);
    REQUIRE_EQ(Admission::try_apply(ticket, report),
        Application::duplicate_or_retired);
    REQUIRE_EQ(
        f.runtime->statistics(false, true).total.sent_unique.packets, 0U);
}

TEST(
    compat_group_runtime_busy_admission_and_outcome_can_retry_without_losing_ticket)
{
    Fixture f;
    auto [ticket, packet] = f.admit();
    auto held = Admission::try_reserve(f.binding, f.header(1), clear.size());
    REQUIRE_EQ(held.status(), Status::ready);
    auto check = std::async(std::launch::async, [&] {
        return std::pair {
            Admission::try_reserve(f.binding, f.header(1), clear.size())
                .status(),
            Admission::try_apply(ticket, f.submitted(ticket))};
    });
    REQUIRE_EQ(check.wait_for(2s), std::future_status::ready);
    auto result = check.get();
    REQUIRE_EQ(result.first, Status::busy);
    REQUIRE_EQ(result.second, Application::busy);
    held.release();
    REQUIRE_EQ(Admission::try_apply(ticket, f.submitted(ticket)),
        Application::applied);
}

TEST(
    compat_group_runtime_replacement_revoke_and_generation_exhaustion_fence_old_owners)
{
    Fixture f;
    auto first = f.binding;
    auto replacement = Admission::bind(f.runtime, f.configuration());
    REQUIRE(replacement);
    REQUIRE_EQ(Admission::try_reserve(first, f.header(), clear.size()).status(),
        Status::stale);
    Admission::revoke(first);
    {
        auto guard =
            Admission::try_reserve(replacement, f.header(), clear.size());
        REQUIRE_EQ(guard.status(), Status::ready);
    }
    GroupRuntimeAdmissionTestAccess::generation(
        *f.runtime, std::numeric_limits<std::uint64_t>::max() - 1);
    auto last = Admission::bind(f.runtime, f.configuration());
    REQUIRE(last);
    REQUIRE(!Admission::bind(f.runtime, f.configuration()));
    {
        auto guard = Admission::try_reserve(last, f.header(), clear.size());
        REQUIRE_EQ(guard.status(), Status::ready);
    }
    Admission::revoke(last);
    REQUIRE_EQ(Admission::try_reserve(last, f.header(), clear.size()).status(),
        Status::stale);
}

TEST(
    compat_group_runtime_wire_ack_nak_and_unicast_repair_preserve_canonical_ctr_gcm_bytes)
{
    for (const auto mode :
        {CryptoMode::automatic, CryptoMode::aes_ctr, CryptoMode::aes_gcm}) {
        Fixture f(mode);
        auto [ticket, packet] = f.admit();
        f.ack(initial.next());
        f.nak(initial);
        (void)f.runtime->poll();
        REQUIRE_EQ(f.runtime->buffer_packet_counts().unacknowledged_send, 1U);
        REQUIRE(f.data().empty());
        REQUIRE_EQ(Admission::try_apply(ticket, f.submitted(ticket)),
            Application::applied);
        f.nak(initial);
        for (unsigned turn = 0; turn < 20 && f.data().empty(); ++turn) {
            (void)f.runtime->poll();
            f.now += 1'000;
        }
        const auto data = f.data();
        REQUIRE_EQ(data.size(), 1U);
        REQUIRE_EQ(data[0].destination, f.peer);
        const auto decoded = decode_packet(data[0].bytes);
        REQUIRE(decoded);
        REQUIRE(decoded.packet.data.retransmitted);
        REQUIRE_EQ(decoded.packet.data.destination_socket_id, group_id);
        auto canonical = decoded.packet.data;
        canonical.retransmitted = false;
        std::array<std::byte, 1500> rebuilt {};
        const auto encoded =
            encode_packet({.kind = PacketKind::data,
                              .data = canonical,
                              .payload = decoded.packet.payload},
                rebuilt);
        REQUIRE(encoded);
        REQUIRE(
            std::equal(rebuilt.begin(), rebuilt.begin() + encoded.bytes_written,
                packet->bytes().begin(), packet->bytes().end()));
        std::array<std::byte, clear.size()> output {};
        if (mode == CryptoMode::aes_gcm) {
            REQUIRE_EQ(f.receiver->open(decoded.packet.data,
                           decoded.packet.payload.first(clear.size()),
                           decoded.packet.payload.last(
                               srt_gcm_authentication_tag_size),
                           output),
                Error::none);
        } else if (mode == CryptoMode::aes_ctr) {
            REQUIRE_EQ(f.receiver->decrypt(decoded.packet.data.encryption_key,
                           decoded.packet.data.sequence, decoded.packet.payload,
                           output),
                Error::none);
        } else {
            std::copy(decoded.packet.payload.begin(),
                decoded.packet.payload.end(), output.begin());
        }
        REQUIRE_EQ(output, clear);
        REQUIRE_EQ(
            f.runtime->statistics(false, true).total.sent_retransmitted.packets,
            1U);
        REQUIRE_EQ(
            f.runtime->statistics(false, true).total.sent_unique.packets, 0U);
        f.ack(initial.next());
        REQUIRE_EQ(f.runtime->buffer_packet_counts().unacknowledged_send, 0U);
    }
}

TEST(
    compat_group_runtime_close_failure_options_and_revoke_reject_captured_outcomes)
{
    for (unsigned mutation = 0; mutation < 5; ++mutation) {
        Fixture f;
        auto [ticket, packet] = f.admit();
        switch (mutation) {
        case 0:
            f.runtime->close();
            break;
        case 1:
            f.runtime->mark_broken(22);
            break;
        case 2:
            f.runtime->apply_options(f.options);
            break;
        case 3:
            Admission::revoke(f.binding);
            break;
        case 4: {
            const std::array<std::byte, 4> padding {};
            f.runtime->process_packet(
                {.kind = PacketKind::control,
                    .control = {.type = ControlType::shutdown,
                        .destination_socket_id = 97},
                    .payload = padding},
                f.peer);
            break;
        }
        }
        const auto before =
            f.runtime->statistics(false, true).total.sent_unique.packets;
        const auto activity =
            f.runtime->response_health().last_response_microseconds;
        REQUIRE_EQ(Admission::try_apply(ticket, f.submitted(ticket)),
            Application::stale);
        REQUIRE_EQ(f.runtime->statistics(false, true).total.sent_unique.packets,
            before);
        REQUIRE_EQ(
            f.runtime->response_health().last_response_microseconds, activity);
        REQUIRE(!Admission::bind(f.runtime, f.configuration()));
    }
}

TEST(
    compat_group_runtime_guard_pins_lifetime_and_serializes_close_without_socket_io)
{
    Fixture f;
    std::future<void> closing;
    auto guard = Admission::try_reserve(f.binding, f.header(), clear.size());
    REQUIRE_EQ(guard.status(), Status::ready);
    std::promise<void> requested;
    auto started = requested.get_future();
    closing = std::async(std::launch::async,
        [runtime = f.runtime, requested = std::move(requested)]() mutable {
            requested.set_value();
            runtime->close();
        });
    REQUIRE_EQ(started.wait_for(2s), std::future_status::ready);
    REQUIRE_EQ(closing.wait_for(10ms), std::future_status::timeout);
    REQUIRE(f.sent.empty());
    guard.release();
    REQUIRE_EQ(closing.wait_for(2s), std::future_status::ready);
    closing.get();
    REQUIRE_EQ(
        Admission::try_reserve(f.binding, f.header(), clear.size()).status(),
        Status::closed);

    Fixture retiring;
    auto pinned = Admission::try_reserve(
        retiring.binding, retiring.header(), clear.size());
    REQUIRE_EQ(pinned.status(), Status::ready);
    std::weak_ptr<ConnectionRuntime> weak = retiring.runtime;
    retiring.runtime.reset();
    REQUIRE(!weak.expired());
    pinned.release();
    REQUIRE(weak.expired());
}

TEST(
    compat_group_runtime_acked_rebind_rejects_old_ticket_even_for_same_stream_identity)
{
    Fixture f;
    auto [ticket, packet] = f.admit();
    REQUIRE_EQ(Admission::try_apply(ticket, f.submitted(ticket)),
        Application::applied);
    f.ack(initial.next());
    auto next = Admission::bind(f.runtime, f.configuration());
    REQUIRE(next);
    REQUIRE_EQ(
        Admission::try_apply(ticket, f.submitted(ticket)), Application::stale);
    f.binding = next;
    auto current = f.admit(1).first;
    REQUIRE_EQ(
        Admission::try_apply(ticket, f.submitted(ticket)), Application::stale);
    f.ack(initial.advanced(2));
    REQUIRE_EQ(f.runtime->buffer_packet_counts().unacknowledged_send, 1U);
    REQUIRE_EQ(Admission::try_apply(current, f.submitted(current)),
        Application::applied);
    f.ack(initial.advanced(2));
    REQUIRE_EQ(f.runtime->buffer_packet_counts().unacknowledged_send, 0U);
}

TEST(
    compat_group_runtime_expiry_and_message_wrap_keep_session_cursor_consistent)
{
    Fixture f;
    REQUIRE_EQ(Admission::try_reserve(
                   f.binding, f.header(), clear.size(), f.origin - 1ns)
                   .status(),
        Status::expired);
    REQUIRE_EQ(Admission::try_reserve(f.binding, f.header(), clear.size(),
                   f.origin + std::chrono::microseconds {f.now})
                   .status(),
        Status::expired);
    auto identity = f.header();
    identity.message_number = 0x03ff'ffffU;
    auto guard = Admission::try_reserve(f.binding, identity, clear.size(),
        f.origin + std::chrono::microseconds {f.now + 100});
    REQUIRE_EQ(guard.status(), Status::ready);
    auto packet = f.encoder->prepare(identity, clear);
    REQUIRE(packet);
    REQUIRE(guard.stage(packet.packet));
    REQUIRE(f.encoder->commit(packet.packet));
    auto ticket = guard.commit();
    REQUIRE(ticket);
    guard.release();
    auto next = f.header(1);
    next.message_number = 1;
    {
        auto reserved = Admission::try_reserve(f.binding, next, clear.size());
        REQUIRE_EQ(reserved.status(), Status::ready);
    }
    f.now += 101;
    (void)f.runtime->poll();
    REQUIRE_EQ(f.runtime->statistics(false, true)
                   .total.sender_message_ttl_dropped.packets,
        1U);
    REQUIRE_EQ(Admission::try_apply(*ticket, f.submitted(*ticket)),
        Application::duplicate_or_retired);
    f.ack(initial.next());
    REQUIRE_EQ(f.runtime->buffer_packet_counts().unacknowledged_send, 0U);
}

namespace {
void exchange_runtime_keys(
    Fixture& f, std::size_t& processed, unsigned& exchanges)
{
    (void)f.runtime->poll();
    // Runtime controls are captured through its actual unicast egress. Answer
    // its KMREQ via established-peer packet admission, not direct sender ACK.
    while (processed < f.sent.size()) {
        const auto bytes = f.sent[processed++].bytes;
        const auto decoded = decode_packet(bytes);
        REQUIRE(decoded);
        if (decoded.packet.kind != PacketKind::control
            || decoded.packet.control.type != ControlType::user_defined
            || decoded.packet.control.subtype != key_material_request_subtype)
            continue;
        REQUIRE_EQ(
            f.receiver->accept_key_material(decoded.packet.payload, false),
            Error::none);
        const std::vector<std::byte> response {
            f.receiver->key_material_response().begin(),
            f.receiver->key_material_response().end()};
        f.runtime->process_packet(
            {.kind = PacketKind::control,
                .control = {.type = ControlType::user_defined,
                    .subtype = key_material_response_subtype,
                    .destination_socket_id = 97},
                .payload = response},
            f.peer);
        ++exchanges;
    }
}
}

TEST(
    compat_group_runtime_shared_schedule_rotates_through_native_key_controls_without_member_originals)
{
    for (const auto mode : {CryptoMode::aes_ctr, CryptoMode::aes_gcm}) {
        Fixture f(mode);
        std::size_t processed = 0;
        unsigned exchanges = 0;
        for (unsigned i = 0; i < 16; ++i) {
            auto [ticket, packet] = f.admit(i);
            exchange_runtime_keys(f, processed, exchanges);
            REQUIRE(f.data().empty());
            const auto decoded = decode_packet(packet->bytes());
            REQUIRE(decoded);
            std::array<std::byte, clear.size()> output {};
            if (mode == CryptoMode::aes_gcm) {
                REQUIRE_EQ(f.receiver->open(decoded.packet.data,
                               decoded.packet.payload.first(clear.size()),
                               decoded.packet.payload.last(
                                   srt_gcm_authentication_tag_size),
                               output),
                    Error::none);
            } else {
                REQUIRE_EQ(
                    f.receiver->decrypt(decoded.packet.data.encryption_key,
                        decoded.packet.data.sequence, decoded.packet.payload,
                        output),
                    Error::none);
                f.receiver->note_accepted_receive_sequence(
                    decoded.packet.data.sequence);
            }
            REQUIRE_EQ(output, clear);
            REQUIRE_EQ(Admission::try_apply(ticket, f.submitted(ticket)),
                Application::applied);
            f.ack(initial.advanced(i + 1));
            REQUIRE_EQ(
                f.runtime->buffer_packet_counts().unacknowledged_send, 0U);
            f.now += 1'000;
        }
        REQUIRE(exchanges >= 3U);
        REQUIRE(!f.runtime->broken());
        REQUIRE_EQ(
            f.runtime->statistics(false, true).total.sent_unique.packets, 0U);
    }
}

namespace {
struct RuntimeFanout {
    std::array<std::optional<Admission::Ticket>, 2> tickets;
};
struct PublishRuntime {
    GroupPacketEncoder* encoder;
    std::shared_ptr<const GroupPacketEncoder::Packet> packet;
    std::array<Admission::Guard*, 2> guards;
    std::shared_ptr<RuntimeFanout> context;
    bool valid = true;
    static bool commit(void* p) noexcept
    {
        auto& self = *static_cast<PublishRuntime*>(p);
        if (!self.encoder->commit(self.packet))
            return false;
        for (std::size_t i = 0; i < self.guards.size(); ++i) {
            self.context->tickets[i] = self.guards[i]->commit();
            self.valid &= self.context->tickets[i].has_value();
        }
        return true;
    }
};
UdpIoResult capture_multicast(
    std::span<const std::byte> bytes, IpEndpoint destination, void* p) noexcept
{
    static_cast<std::vector<Sent>*>(p)->push_back(
        {{bytes.begin(), bytes.end()}, destination});
    return {.bytes_transferred = bytes.size()};
}
}

TEST(
    compat_group_runtime_emitter_transaction_survives_member_close_and_retries_busy_fanout)
{
    for (const auto mode : {CryptoMode::aes_ctr, CryptoMode::aes_gcm}) {
        for (unsigned scenario = 0; scenario < 3; ++scenario) {
            Fixture first(mode);
            Fixture second(mode, true, first.schedule, first.origin);
            std::vector<Sent> multicast;
            auto source_channel = std::make_shared<DatagramChannel>();
            source_channel->set_send_hook_for_testing(
                capture_multicast, &multicast);
            auto emitter = GroupMulticastEmitter::create_for_testing(
                {.channel = source_channel,
                    .destination = {.address = {239, 255, 133, 11},
                        .port = 6000},
                    .group_id = group_id,
                    .generation = stream_generation,
                    .initial_sequence = initial,
                    .wire_bytes_per_second = 1'000'000,
                    .clock =
                        [](void* p) noexcept {
                            const auto& f = *static_cast<Fixture*>(p);
                            return f.origin + std::chrono::microseconds {f.now};
                        },
                    .clock_context = &first});
            REQUIRE(emitter);
            // All fanout allocation, source/member capacity and lifetime
            // reservations precede canonical sealing. Commit does not notify
            // channels or enter registries while emitter/member locks are held.
            auto context = std::make_shared<RuntimeFanout>();
            auto reservation = emitter->reserve(
                stream_generation, initial, first.origin + 1s, true);
            REQUIRE_EQ(reservation.status(),
                GroupMulticastEmitter::Admission::accepted);
            auto one = Admission::try_reserve(
                first.binding, first.header(), clear.size());
            auto two = Admission::try_reserve(
                second.binding, first.header(), clear.size());
            REQUIRE_EQ(one.status(), Status::ready);
            REQUIRE_EQ(two.status(), Status::ready);
            auto packet = first.encoder->prepare(first.header(), clear);
            REQUIRE(packet);
            REQUIRE(one.stage(packet.packet));
            REQUIRE(two.stage(packet.packet));
            PublishRuntime publish {
                first.encoder.get(), packet.packet, {&one, &two}, context};
            REQUIRE_EQ(reservation.publish(packet.packet->bytes(),
                           packet.packet->generation_owner(),
                           PublishRuntime::commit, &publish, context),
                GroupMulticastEmitter::Admission::accepted);
            REQUIRE(publish.valid);
            one.release();
            two.release();
            Admission::notify_work(first.binding);
            Admission::notify_work(second.binding);
            (void)first.runtime->poll();
            (void)second.runtime->poll();
            REQUIRE(first.data().empty());
            REQUIRE(second.data().empty());
            if (scenario == 1)
                first.runtime->close(); // Source channel/actor are independent.
            emitter->pump_for_testing();
            REQUIRE_EQ(multicast.size(), 1U);
            REQUIRE(multicast[0].destination.is_multicast());
            REQUIRE(std::equal(multicast[0].bytes.begin(),
                multicast[0].bytes.end(), packet.packet->bytes().begin(),
                packet.packet->bytes().end()));
            std::array<GroupMulticastEmitter::Outcome, 1> outcomes;
            REQUIRE_EQ(emitter->take_outcomes(outcomes), 1U);
            const auto captured = std::static_pointer_cast<const RuntimeFanout>(
                outcomes[0].context);
            REQUIRE_EQ(Admission::try_apply(*captured->tickets[1], outcomes[0]),
                Application::applied);
            if (scenario == 2) {
                auto held = Admission::try_reserve(
                    first.binding, first.header(1), clear.size());
                REQUIRE_EQ(held.status(), Status::ready);
                auto attempt = std::async(std::launch::async, [&] {
                    return Admission::try_apply(
                        *captured->tickets[0], outcomes[0]);
                });
                REQUIRE_EQ(attempt.wait_for(2s), std::future_status::ready);
                REQUIRE_EQ(attempt.get(), Application::busy);
                held.release();
            }
            REQUIRE_EQ(Admission::try_apply(*captured->tickets[0], outcomes[0]),
                scenario == 1 ? Application::stale : Application::applied);
            second.ack(initial.next());
            REQUIRE_EQ(
                second.runtime->buffer_packet_counts().unacknowledged_send, 0U);
            if (scenario != 1) {
                first.ack(initial.next());
                REQUIRE_EQ(
                    first.runtime->buffer_packet_counts().unacknowledged_send,
                    0U);
            }
        }
    }
}

TEST(
    compat_group_runtime_binding_rejects_unsupported_message_and_filter_profiles)
{
    for (unsigned scenario = 0; scenario < 3; ++scenario) {
        Fixture f(CryptoMode::automatic, false);
        auto options = f.options;
        if (scenario == 0)
            REQUIRE_EQ(options.set(SocketOption::message_api, 0), Error::none);
        else if (scenario == 1)
            REQUIRE_EQ(options.set_packet_filter("fec,cols:2"), Error::none);
        else
            REQUIRE_EQ(options.set_congestion_controller("file"), Error::none);
        f.runtime->apply_options(options);
        REQUIRE(!Admission::bind(f.runtime, f.configuration()));
    }
}

TEST(
    compat_group_runtime_guard_rejects_old_crypto_generation_despite_identical_wrapped_header)
{
    Fixture f(CryptoMode::aes_gcm);
    auto old = f.encoder->prepare(f.header(), clear);
    REQUIRE(old);
    auto keys = f.schedule;
    constexpr auto span = SequenceNumber::half_range - 1U;
    REQUIRE_EQ(keys->advance_unused(initial, span), Error::none);
    REQUIRE_EQ(keys->advance_unused(initial.advanced(span), span), Error::none);
    REQUIRE_EQ(
        keys->advance_unused(initial.advanced(2U * span), 2), Error::none);
    REQUIRE_EQ(keys->frontier_position(),
        static_cast<std::uint64_t>(SequenceNumber::modulus));
    auto current_encoder = GroupPacketEncoder::create({.group_id = group_id,
        .stream_generation = stream_generation,
        .initial_sequence = initial,
        .key_schedule = keys});
    REQUIRE(current_encoder);
    auto guard = Admission::try_reserve(f.binding, f.header(), clear.size());
    REQUIRE_EQ(guard.status(), Status::ready);
    auto current = current_encoder->prepare(f.header(), clear);
    REQUIRE(current);
    REQUIRE(std::equal(old.packet->bytes().begin(),
        old.packet->bytes().begin() + packet_header_size,
        current.packet->bytes().begin(),
        current.packet->bytes().begin() + packet_header_size));
    REQUIRE(
        old.packet->crypto_generation() != current.packet->crypto_generation());
    REQUIRE(!guard.stage(old.packet));
    REQUIRE(guard.stage(current.packet));
}

TEST(compat_group_runtime_admission_cannot_overwrite_an_unsent_key_announcement)
{
    for (const auto mode : {CryptoMode::aes_ctr, CryptoMode::aes_gcm}) {
        Fixture f(mode);
        for (unsigned i = 0; i < 4; ++i) {
            auto ticket = f.admit(i).first;
            REQUIRE_EQ(Admission::try_apply(ticket, f.submitted(ticket)),
                Application::applied);
            f.ack(initial.advanced(i + 1));
            f.now += 1'000;
        }
        const auto revision = f.crypto->key_material_revision();
        const std::vector<std::byte> announcement {
            f.crypto->pending_key_material().begin(),
            f.crypto->pending_key_material().end()};
        REQUIRE(!announcement.empty());
        REQUIRE_EQ(Admission::try_reserve(f.binding, f.header(4), clear.size())
                       .status(),
            Status::busy);
        REQUIRE_EQ(f.crypto->key_material_revision(), revision);
        REQUIRE(std::equal(announcement.begin(), announcement.end(),
            f.crypto->pending_key_material().begin(),
            f.crypto->pending_key_material().end()));
        REQUIRE(!f.encoder->snapshot().prepared);
        (void)f.runtime->poll();
        bool queued = false;
        for (const auto& sent : f.sent) {
            const auto decoded = decode_packet(sent.bytes);
            if (decoded && decoded.packet.kind == PacketKind::control
                && decoded.packet.control.subtype
                    == key_material_request_subtype
                && decoded.packet.control.type == ControlType::user_defined) {
                queued = true;
                REQUIRE(std::equal(announcement.begin(), announcement.end(),
                    decoded.packet.payload.begin(),
                    decoded.packet.payload.end()));
            }
        }
        REQUIRE(queued);
        // No KMRSP has been supplied: dispatching the immutable KM request is
        // sufficient to protect it from overwrite without waiting for the peer.
        auto next =
            Admission::try_reserve(f.binding, f.header(4), clear.size());
        REQUIRE_EQ(next.status(), Status::ready);
    }
}
