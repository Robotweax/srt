#include "test.hpp"

#include "compat/group_multicast_emitter.hpp"
#include "compat/group_packet_encoder.hpp"
#include "compat/transport_runtime.hpp"
#include "robotweax/srt/codec.hpp"
#include "robotweax/srt/connection_group.hpp"
#include "robotweax/srt/send_buffer.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <vector>

using namespace robotweax::srt;
using namespace robotweax::srt::compat;

namespace {
using Emitter = GroupMulticastEmitter;
using Encoder = GroupPacketEncoder;
using Admission = Emitter::Admission;
using namespace std::chrono_literals;
constexpr std::uint32_t group_id = group_handle_mask | 63U;
constexpr std::uint64_t generation = 31;
constexpr SequenceNumber initial {SequenceNumber::mask};
constexpr std::array<std::byte, 3> plaintext {
    std::byte {1}, std::byte {2}, std::byte {3}};
DataHeader header(SequenceNumber sequence = initial)
{
    return {.sequence = sequence,
        .message_number = 11,
        .boundary = MessageBoundary::solo,
        .in_order = true,
        .timestamp = PacketTimestamp {0xffff'ffffU},
        .destination_socket_id = group_id};
}
std::vector<std::byte> encoded(DataHeader h = header())
{
    std::array<std::byte, 64> bytes {};
    const auto result = encode_packet(
        {.kind = PacketKind::data, .data = h, .payload = plaintext}, bytes);
    REQUIRE(result);
    return {bytes.begin(), bytes.begin() + result.bytes_written};
}
// A wire ACK that runs ahead of the group's submission report is not an
// error, and it retires nothing: an unsubmitted original is never authorized.
void require_ack_retires_nothing(SendBuffer& history, SequenceNumber next)
{
    const auto first = history.first_sequence();
    const auto size = history.size();
    REQUIRE_EQ(history.acknowledge_submitted_before(next), Error::none);
    REQUIRE_EQ(history.first_sequence(), first);
    REQUIRE_EQ(history.size(), size);
}
// The counterpart: once the submission is reported, the same ACK retires the
// history up to it. Success alone does not show that, an early ACK succeeds
// too.
void require_ack_retires_through(SendBuffer& history, SequenceNumber next)
{
    const auto size = history.size();
    REQUIRE_EQ(history.acknowledge_submitted_before(next), Error::none);
    REQUIRE_EQ(history.first_sequence(), next);
    REQUIRE(history.size() < size);
}
void submitted(SendBuffer& history, SequenceNumber sequence, std::uint64_t now)
{
    if (const auto token = history.group_original_submission(sequence))
        history.note_group_original_submitted(*token, now);
}
struct Capture {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::vector<std::byte>> bytes;
    bool all_committed = true;
    std::atomic<bool>* committed = nullptr;
    unsigned failures = 0;
};
UdpIoResult capture(
    std::span<const std::byte> bytes, IpEndpoint, void* context) noexcept
{
    auto& result = *static_cast<Capture*>(context);
    std::lock_guard lock(result.mutex);
    if (result.committed && !result.committed->load())
        result.all_committed = false;
    result.bytes.emplace_back(bytes.begin(), bytes.end());
    result.changed.notify_all();
    if (result.failures != 0U) {
        --result.failures;
        return {.error = Error::would_block};
    }
    return {.bytes_transferred = bytes.size()};
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
            .initial_sequence = initial,
            .capacity = 2,
            .wire_bytes_per_second = 1'000'000,
            .clock =
                [](void* p) noexcept {
                    return *static_cast<Emitter::TimePoint*>(p);
                },
            .clock_context = &now};
        emitter = Emitter::create_for_testing(config);
        REQUIRE(emitter);
    }
};
bool count_commit(void* context) noexcept
{
    ++*static_cast<unsigned*>(context);
    return true;
}
bool reject_commit(void*) noexcept
{
    return false;
}
std::shared_ptr<RuntimeScheduler> scheduler()
{
    auto result = std::make_shared<RuntimeScheduler>(
        RuntimeScheduler::Configuration {1, 2, 2, 1});
    REQUIRE(result->start());
    return result;
}
std::shared_ptr<SharedTransmitKeySchedule> keys(CryptoMode mode)
{
    auto result = std::make_shared<SharedTransmitKeySchedule>(
        SharedTransmitKeySchedule::Configuration {.mode = mode,
            .refresh_rate_packets = 4,
            .preannouncement_packets = 1,
            .origin = initial});
    REQUIRE_EQ(result->initialize(), Error::none);
    return result;
}
} // namespace

TEST(
    compat_group_admission_reservation_cancel_move_and_validation_preserve_capacity)
{
    Fixture f;
    unsigned commits = 0;
    {
        auto reserved = f.emitter->reserve(generation, initial, f.now + 1s);
        REQUIRE_EQ(reserved.status(), Admission::accepted);
        auto moved = std::move(reserved);
        REQUIRE_EQ(reserved.status(), Admission::cancelled);
        REQUIRE_EQ(reserved.publish(encoded(), {}, count_commit, &commits),
            Admission::cancelled);
        // Moved transaction is cancelled by destruction, without publication.
        REQUIRE_EQ(moved.status(), Admission::accepted);
    }
    REQUIRE_EQ(f.emitter->snapshot().queued, 0U);
    REQUIRE_EQ(f.emitter->snapshot().accepted_next_sequence, initial);
    auto reserved = f.emitter->reserve(generation, initial, f.now + 1s);
    auto invalid = encoded();
    invalid[15] ^= std::byte {1};
    REQUIRE_EQ(reserved.publish(invalid, {}, count_commit, &commits),
        Admission::invalid_packet);
    REQUIRE_EQ(commits, 0U);
    auto wrong = f.emitter->reserve(generation, initial, f.now + 1s);
    REQUIRE_EQ(wrong.publish(
                   encoded(header(initial.next())), {}, count_commit, &commits),
        Admission::wrong_sequence);
    auto retry = f.emitter->reserve(generation, initial, f.now + 1s);
    REQUIRE_EQ(retry.publish(encoded(), {}, count_commit, &commits),
        Admission::accepted);
    REQUIRE_EQ(retry.publish(encoded(), {}, count_commit, &commits),
        Admission::cancelled);
    REQUIRE_EQ(commits, 1U);
}

TEST(
    compat_group_admission_rejected_commit_cannot_publish_or_disturb_accepted_work)
{
    Fixture f;
    REQUIRE_EQ(f.emitter->enqueue(generation, encoded(), f.now + 1s),
        Admission::accepted);
    auto rejected = f.emitter->reserve(generation, initial.next(), f.now + 1s);
    REQUIRE_EQ(
        rejected.publish(encoded(header(initial.next())), {}, reject_commit),
        Admission::cancelled);
    REQUIRE_EQ(f.emitter->snapshot().queued, 1U);
    REQUIRE_EQ(f.emitter->snapshot().accepted_packets, 1U);
    REQUIRE_EQ(f.emitter->snapshot().accepted_next_sequence, initial.next());
    REQUIRE_EQ(f.emitter->snapshot().failure, Emitter::Failure::none);
    f.emitter->pump_for_testing();
    REQUIRE_EQ(f.output.bytes.size(), 1U);
    REQUIRE_EQ(f.output.bytes[0], encoded());
}

TEST(
    compat_group_admission_capacity_generation_sequence_and_deadline_fail_before_preparation)
{
    Fixture f;
    REQUIRE_EQ(f.emitter->reserve(generation + 1, initial, f.now + 1s).status(),
        Admission::wrong_generation);
    REQUIRE_EQ(
        f.emitter->reserve(generation, initial.next(), f.now + 1s).status(),
        Admission::wrong_sequence);
    REQUIRE_EQ(f.emitter->reserve(generation, initial, f.now).status(),
        Admission::expired);
    for (unsigned i = 0; i < 2; ++i)
        REQUIRE_EQ(f.emitter->enqueue(generation,
                       encoded(header(initial.advanced(i))), f.now + 1s),
            Admission::accepted);
    auto encoder = Encoder::create({.group_id = group_id,
        .stream_generation = generation,
        .initial_sequence = initial.advanced(2),
        .key_schedule = keys(CryptoMode::aes_gcm)});
    REQUIRE(encoder);
    auto full = f.emitter->reserve(generation, initial.advanced(2), f.now + 1s);
    REQUIRE_EQ(full.status(), Admission::full);
    REQUIRE(!encoder->snapshot().prepared);
    REQUIRE_EQ(encoder->snapshot().live_packets, 0U);
}

TEST(
    compat_group_admission_reserved_deadline_expiring_during_preparation_is_accounted_after_commit)
{
    Fixture f;
    unsigned commits = 0;
    auto reserved = f.emitter->reserve(generation, initial, f.now + 1ms);
    REQUIRE_EQ(reserved.status(), Admission::accepted);
    f.now += 2ms;
    REQUIRE_EQ(reserved.publish(encoded(), {}, count_commit, &commits),
        Admission::accepted);
    REQUIRE_EQ(commits, 1U);
    f.emitter->pump_for_testing();
    REQUIRE_EQ(f.emitter->snapshot().accepted_packets, 1U);
    REQUIRE_EQ(f.emitter->snapshot().expired_packets, 1U);
    REQUIRE_EQ(f.emitter->snapshot().submitted_packets, 0U);
    REQUIRE(f.output.bytes.empty());
}

TEST(compat_group_admission_scheduler_stop_cannot_run_coordinator_commit)
{
    Fixture f;
    auto pool = scheduler();
    auto emitter = Emitter::create(f.config, pool);
    REQUIRE(emitter);
    unsigned commits = 0;
    auto reservation = emitter->reserve(generation, initial, f.now + 1s);
    REQUIRE_EQ(reservation.status(), Admission::accepted);
    pool->stop();
    REQUIRE_EQ(reservation.publish(encoded(), {}, count_commit, &commits),
        Admission::failed);
    REQUIRE_EQ(commits, 0U);
    REQUIRE_EQ(emitter->snapshot().accepted_packets, 0U);
    REQUIRE_EQ(emitter->snapshot().accepted_next_sequence, initial);
    REQUIRE_EQ(emitter->snapshot().failure, Emitter::Failure::scheduler);
}

TEST(
    compat_group_admission_live_scheduler_cannot_emit_before_coordinator_commit)
{
    Fixture f;
    std::atomic<bool> committed {false};
    auto pool = scheduler();
    auto c = f.config;
    c.clock = nullptr;
    c.clock_context = nullptr;
    auto emitter = Emitter::create(c, pool);
    REQUIRE(emitter);
    f.output.committed = &committed;
    auto reservation =
        emitter->reserve(generation, initial, Emitter::Clock::now() + 2s);
    REQUIRE_EQ(reservation.status(), Admission::accepted);
    REQUIRE_EQ(reservation.publish(
                   encoded(), {},
                   [](void* p) noexcept {
                       static_cast<std::atomic<bool>*>(p)->store(true);
                       return true;
                   },
                   &committed),
        Admission::accepted);
    {
        std::unique_lock lock(f.output.mutex);
        REQUIRE(f.output.changed.wait_for(lock, 2s, [&] {
            return !f.output.bytes.empty();
        }));
        REQUIRE(f.output.all_committed);
    }
    emitter->close();
    pool->stop();
}

TEST(compat_group_admission_reservation_keeps_owner_alive_until_unlock)
{
    Fixture f;
    std::weak_ptr<Emitter> weak = f.emitter;
    {
        auto reservation = f.emitter->reserve(generation, initial, f.now + 1s);
        f.emitter.reset();
        REQUIRE(!weak.expired());
        REQUIRE_EQ(reservation.status(), Admission::accepted);
    }
    REQUIRE(weak.expired());
}

TEST(compat_group_admission_history_does_not_emit_or_authorize_unsubmitted_data)
{
    SendBuffer history {initial, 2};
    REQUIRE_EQ(history.preflight_group_original(
                   initial, plaintext.size(), CryptoMode::automatic),
        Error::none);
    REQUIRE_EQ(history.import_group_original(header(), plaintext,
                   plaintext.size(), CryptoMode::automatic, 100),
        Error::none);
    REQUIRE_EQ(history.size(), 1U);
    REQUIRE_EQ(history.buffered_payload_bytes(), plaintext.size());
    REQUIRE(!history.peek_new_packet());
    REQUIRE(!history.next_packet());
    require_ack_retires_nothing(history, initial.next());
    REQUIRE_EQ(history.request_retransmission({initial, initial}),
        Error::invalid_control_payload);
    REQUIRE(!history.request_retransmission_of_last_sent());
    REQUIRE_EQ(history.request_retransmission_of_all_sent(), 0U);
    REQUIRE(!history.drop_messages_older_than(1'000'000));
    // Member completions cannot stand in for group transport submission.
    history.note_packet_submitted(initial);
    require_ack_retires_nothing(history, initial.next());
    submitted(history, initial.next(), 110); // stale/wrong packet
    REQUIRE_EQ(history.request_retransmission({initial, initial}),
        Error::invalid_control_payload);
    submitted(history, initial, 120);
    submitted(history, initial, 900); // duplicate cannot reset age
    REQUIRE_EQ(history.request_retransmission({initial, initial}), Error::none);
    auto repeat = history.next_packet(true);
    REQUIRE(repeat);
    REQUIRE(repeat->header.retransmitted);
    REQUIRE_EQ(repeat->header.destination_socket_id, group_id);
    REQUIRE(std::equal(repeat->payload.begin(), repeat->payload.end(),
        plaintext.begin(), plaintext.end()));
    REQUIRE(history.drop_messages_older_than(120));
}

TEST(
    compat_group_admission_history_preflight_and_invalid_import_are_transactional)
{
    SendBuffer history {initial, 1, 3};
    REQUIRE_EQ(history.preflight_group_original(
                   initial.next(), 3, CryptoMode::automatic),
        Error::invalid_state);
    REQUIRE_EQ(
        history.preflight_group_original(initial, 4, CryptoMode::automatic),
        Error::invalid_state);
    REQUIRE_EQ(
        history.preflight_group_original(initial, 0, CryptoMode::automatic),
        Error::invalid_state);
    REQUIRE_EQ(history.preflight_group_original(
                   initial, 3, static_cast<CryptoMode>(99)),
        Error::invalid_state);
    for (unsigned mutation = 0; mutation < 8; ++mutation) {
        auto h = header();
        switch (mutation) {
        case 0:
            h.message_number = 0;
            break;
        case 1:
            h.message_number = 0x0400'0000U;
            break;
        case 2:
            h.boundary = static_cast<MessageBoundary>(4);
            break;
        case 3:
            h.retransmitted = true;
            break;
        case 4:
            h.destination_socket_id = 1;
            break;
        case 5:
            h.encryption_key = EncryptionKey::even;
            break;
        case 6:
            h.sequence = initial.next();
            break;
        case 7:
            h.encryption_key = EncryptionKey::reserved;
            break;
        }
        REQUIRE_EQ(history.import_group_original(
                       h, plaintext, 3, CryptoMode::automatic),
            Error::invalid_state);
        REQUIRE_EQ(history.size(), 0U);
        REQUIRE_EQ(history.next_sequence(), initial);
    }
    REQUIRE_EQ(history.import_group_original(
                   header(), plaintext, 2, CryptoMode::automatic),
        Error::invalid_payload_size);
    REQUIRE_EQ(history.import_group_original(
                   header(), plaintext, 3, CryptoMode::automatic),
        Error::none);
    REQUIRE_EQ(history.preflight_group_original(
                   initial.next(), 3, CryptoMode::automatic),
        Error::buffer_too_small);
    REQUIRE_EQ(history.import_group_original(
                   header(initial.next()), plaintext, 3, CryptoMode::automatic),
        Error::buffer_too_small);
    REQUIRE_EQ(history.next_sequence(), initial.next());
}

TEST(compat_group_admission_history_mixed_unsent_originals_are_rejected)
{
    SendBuffer history {initial, 3};
    REQUIRE_EQ(history.enqueue_message(plaintext, 1, PacketTimestamp {}, 17),
        Error::none);
    REQUIRE_EQ(history.preflight_group_original(
                   initial.next(), 3, CryptoMode::automatic),
        Error::invalid_state);
    REQUIRE_EQ(history.import_group_original(
                   header(initial.next()), plaintext, 3, CryptoMode::automatic),
        Error::invalid_state);
    REQUIRE(history.next_packet());
    REQUIRE_EQ(history.import_group_original(
                   header(initial.next()), plaintext, 3, CryptoMode::automatic),
        Error::none);
    REQUIRE(!history.next_packet());
    REQUIRE_EQ(history.request_retransmission({initial, initial.next()}),
        Error::invalid_control_payload);
    REQUIRE(!history.has_pending_retransmission());
    REQUIRE_EQ(history.request_retransmission({initial, initial}), Error::none);
    submitted(history, initial.next(), 100);
    REQUIRE_EQ(
        history.request_retransmission({initial, initial.next()}), Error::none);
}

TEST(
    compat_group_admission_history_copy_expiry_reuse_and_wire_wrap_preserve_submission_gates)
{
    SendBuffer original {initial, 2};
    REQUIRE_EQ(original.import_group_original(
                   header(), plaintext, 3, CryptoMode::automatic, 10, 100),
        Error::none);
    auto copied = original;
    SendBuffer assigned {SequenceNumber {}, 1};
    assigned = original;
    SendBuffer moved = std::move(copied);
    for (auto* h : {&original, &assigned, &moved}) {
        require_ack_retires_nothing(*h, initial.next());
        REQUIRE(!h->drop_expired_message(100));
        REQUIRE_EQ(h->drop_expired_message(101).packets, 1U);
        submitted(*h, initial, 102); // expired packet is not revived
        REQUIRE_EQ(
            h->acknowledge_submitted_before(initial.next()), Error::none);
        REQUIRE_EQ(h->import_group_original(header(initial.next()), plaintext,
                       3, CryptoMode::automatic),
            Error::none);
        REQUIRE_EQ(h->request_retransmission({initial.next(), initial.next()}),
            Error::invalid_control_payload);
        submitted(*h, initial.next(), 110);
        REQUIRE_EQ(h->request_retransmission({initial.next(), initial.next()}),
            Error::none);
        REQUIRE_EQ(
            h->acknowledge_submitted_before(initial.advanced(2)), Error::none);
        REQUIRE_EQ(h->size(), 0U);
        REQUIRE_EQ(h->packets_in_flight(), 0U);
    }
}

namespace {
// Externally serialized histories mirror the locks a runtime coordinator must
// hold. There is no registry, member notification or socket I/O in this commit.
struct Publish {
    Encoder* encoder;
    std::shared_ptr<const Encoder::Packet> packet;
    std::array<SendBuffer*, 2> histories;
    CryptoMode mode;
    bool imports_ok = true;
    static bool commit(void* p) noexcept
    {
        auto& self = *static_cast<Publish*>(p);
        const auto decoded = decode_packet(self.packet->bytes());
        for (auto* history : self.histories)
            if (history->preflight_group_original(
                    decoded.packet.data.sequence, plaintext.size(), self.mode)
                != Error::none)
                return false;
        if (!self.encoder->commit(self.packet))
            return false;
        for (auto* history : self.histories)
            self.imports_ok &=
                history->import_group_original(decoded.packet.data,
                    decoded.packet.payload, plaintext.size(), self.mode, 10)
                == Error::none;
        return true;
    }
};
} // namespace

TEST(
    compat_group_admission_real_crypto_history_and_emitter_publish_one_canonical_original)
{
    for (const auto mode : {CryptoMode::aes_ctr, CryptoMode::aes_gcm}) {
        Fixture f;
        auto encoder = Encoder::create({.group_id = group_id,
            .stream_generation = generation,
            .initial_sequence = initial,
            .key_schedule = keys(mode)});
        REQUIRE(encoder);
        SendBuffer first {initial, 1};
        SendBuffer second {initial, 1};
        auto reservation = f.emitter->reserve(generation, initial, f.now + 1s);
        REQUIRE_EQ(reservation.status(), Admission::accepted);
        for (auto* history : {&first, &second})
            REQUIRE_EQ(history->preflight_group_original(
                           initial, plaintext.size(), mode),
                Error::none);
        auto packet = encoder->prepare(header(), plaintext);
        REQUIRE(packet);
        Publish publish {encoder.get(), packet.packet, {&first, &second}, mode};
        REQUIRE_EQ(
            reservation.publish(packet.packet->bytes(),
                packet.packet->generation_owner(), Publish::commit, &publish),
            Admission::accepted);
        REQUIRE(publish.imports_ok);
        REQUIRE_EQ(encoder->snapshot().committed_packets, 1U);
        REQUIRE_EQ(f.emitter->snapshot().accepted_packets, 1U);
        for (auto* history : {&first, &second}) {
            REQUIRE(!history->peek_new_packet());
            REQUIRE_EQ(history->request_retransmission({initial, initial}),
                Error::invalid_control_payload);
        }
        f.output.failures = 1;
        f.emitter->pump_for_testing();
        REQUIRE_EQ(f.emitter->snapshot().submitted_packets, 0U);
        require_ack_retires_nothing(first, initial.next());
        f.now += 1ms;
        f.emitter->pump_for_testing();
        REQUIRE_EQ(f.emitter->snapshot().submitted_packets, 1U);
        for (auto* history : {&first, &second}) {
            submitted(*history, initial, 100);
            REQUIRE_EQ(history->request_retransmission({initial, initial}),
                Error::none);
            const auto repeat = history->next_packet(true);
            REQUIRE(repeat);
            REQUIRE(repeat->header.retransmitted);
            auto identity = repeat->header;
            identity.retransmitted = false;
            std::array<std::byte, Emitter::maximum_datagram_size> bytes {};
            const auto encoded_repeat =
                encode_packet({.kind = PacketKind::data,
                                  .data = identity,
                                  .payload = repeat->payload},
                    bytes);
            REQUIRE(encoded_repeat);
            REQUIRE(std::equal(bytes.begin(),
                bytes.begin() + encoded_repeat.bytes_written,
                packet.packet->bytes().begin(), packet.packet->bytes().end()));
        }
        REQUIRE_EQ(f.output.bytes[0], f.output.bytes[1]);
    }
}

TEST(
    compat_group_admission_full_history_cancels_before_sealing_and_allows_different_retry_payload)
{
    Fixture f;
    const SequenceNumber previous {initial.value() - 1U};
    SendBuffer full {previous, 1};
    REQUIRE_EQ(full.import_group_original(
                   header(previous), plaintext, 3, CryptoMode::automatic),
        Error::none);
    auto encoder = Encoder::create({.group_id = group_id,
        .stream_generation = generation,
        .initial_sequence = initial});
    REQUIRE(encoder);
    {
        auto reservation = f.emitter->reserve(generation, initial, f.now + 1s);
        REQUIRE_EQ(reservation.status(), Admission::accepted);
        REQUIRE_EQ(
            full.preflight_group_original(initial, 3, CryptoMode::automatic),
            Error::buffer_too_small);
        // Return would-block before calling prepare, then release reservation.
    }
    REQUIRE(!encoder->snapshot().prepared);
    REQUIRE_EQ(f.emitter->snapshot().accepted_packets, 0U);
    const std::array<std::byte, 1> changed {std::byte {9}};
    auto retry = encoder->prepare(header(), changed);
    REQUIRE(retry);
    REQUIRE_EQ(retry.packet->bytes().back(), changed[0]);
}

TEST(
    compat_group_admission_close_waits_for_reservation_and_cancellation_leaves_no_emission)
{
    Fixture f;
    std::future<void> closing;
    {
        auto reservation = f.emitter->reserve(generation, initial, f.now + 1s);
        REQUIRE_EQ(reservation.status(), Admission::accepted);
        std::promise<void> requested;
        auto started = requested.get_future();
        closing = std::async(std::launch::async,
            [source = f.emitter, requested = std::move(requested)]() mutable {
                requested.set_value();
                source->close();
            });
        REQUIRE_EQ(started.wait_for(2s), std::future_status::ready);
        REQUIRE_EQ(closing.wait_for(10ms), std::future_status::timeout);
    }
    REQUIRE_EQ(closing.wait_for(2s), std::future_status::ready);
    closing.get();
    REQUIRE(f.emitter->snapshot().closed);
    REQUIRE_EQ(f.emitter->snapshot().accepted_packets, 0U);
    REQUIRE_EQ(f.emitter->reserve(generation, initial, f.now + 1s).status(),
        Admission::closed);
    f.emitter->pump_for_testing();
    REQUIRE(f.output.bytes.empty());
}

TEST(
    compat_group_admission_publish_rejection_retires_generation_owners_outside_lock)
{
    struct Owner {
        std::weak_ptr<Emitter> emitter;
        std::shared_ptr<std::atomic<unsigned>> count;
        ~Owner()
        {
            if (auto source = emitter.lock())
                (void)source->snapshot();
            ++*count;
        }
    };
    Fixture f;
    auto count = std::make_shared<std::atomic<unsigned>>(0);
    auto owner = std::make_shared<Owner>();
    owner->emitter = f.emitter;
    owner->count = count;
    auto reservation = f.emitter->reserve(generation, initial, f.now + 1s);
    REQUIRE_EQ(reservation.publish(encoded(), std::move(owner), reject_commit),
        Admission::cancelled);
    REQUIRE_EQ(count->load(), 1U);
    REQUIRE_EQ(f.emitter->snapshot().queued, 0U);
}

TEST(
    compat_group_admission_old_submission_position_cannot_authorize_reused_wire_sequence)
{
    SendBuffer history {initial, 1};
    REQUIRE_EQ(history.import_group_original(
                   header(), plaintext, 3, CryptoMode::automatic),
        Error::none);
    const auto old = history.group_original_submission(initial);
    REQUIRE(old);
    history.note_group_original_submitted(*old, 100);
    REQUIRE_EQ(
        history.acknowledge_submitted_before(initial.next()), Error::none);
    REQUIRE(
        history.synchronize_empty(SequenceNumber {SequenceNumber::half_range}));
    REQUIRE(history.synchronize_empty(initial));
    REQUIRE_EQ(history.import_group_original(
                   header(), plaintext, 3, CryptoMode::automatic),
        Error::none);
    const auto fresh = history.group_original_submission(initial);
    REQUIRE(fresh);
    REQUIRE_EQ(fresh->sequence_position,
        static_cast<std::uint64_t>(SequenceNumber::modulus));
    history.note_group_original_submitted(*old, 200);
    require_ack_retires_nothing(history, initial.next());
    REQUIRE_EQ(history.request_retransmission({initial, initial}),
        Error::invalid_control_payload);
    history.note_group_original_submitted(*fresh, 300);
    require_ack_retires_through(history, initial.next());
}

TEST(compat_group_admission_history_enforces_complete_protected_wire_ceiling)
{
    constexpr auto clear_size =
        maximum_data_payload_size - srt_gcm_authentication_tag_size;
    std::array<std::byte, maximum_data_payload_size> body {};
    std::fill(body.begin(), body.end(), std::byte {0x5a});
    auto identity = header();
    identity.encryption_key = EncryptionKey::odd;
    SendBuffer history {initial, 1};
    REQUIRE_EQ(history.preflight_group_original(
                   initial, clear_size + 1, CryptoMode::aes_gcm),
        Error::invalid_state);
    REQUIRE_EQ(history.import_group_original(identity,
                   std::span {body}.first(body.size() - 1), clear_size,
                   CryptoMode::aes_gcm),
        Error::invalid_payload_size);
    REQUIRE_EQ(history.import_group_original(
                   identity, body, clear_size, CryptoMode::aes_ctr),
        Error::invalid_payload_size);
    REQUIRE_EQ(history.size(), 0U);
    REQUIRE_EQ(history.import_group_original(
                   identity, body, clear_size, CryptoMode::aes_gcm),
        Error::none);
    REQUIRE_EQ(history.buffered_payload_bytes(), clear_size);
    submitted(history, initial, 1);
    REQUIRE_EQ(history.request_retransmission({initial, initial}), Error::none);
    const auto repeat = history.next_packet(true);
    REQUIRE(repeat);
    REQUIRE_EQ(repeat->payload.size(), maximum_data_payload_size);
    REQUIRE(std::equal(repeat->payload.begin(), repeat->payload.end(),
        body.begin(), body.end()));
    REQUIRE_EQ(repeat->header.encryption_key, EncryptionKey::odd);
    REQUIRE_EQ(repeat->header.destination_socket_id, group_id);
    REQUIRE_EQ(repeat->header.timestamp, identity.timestamp);
}
