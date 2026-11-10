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
using Admission = Emitter::Admission;
using Disposition = Emitter::Disposition;
using namespace std::chrono_literals;
constexpr std::uint32_t group_id = group_handle_mask | 81U;
constexpr std::uint64_t generation = 43;
constexpr std::array<std::byte, 3> plaintext {
    std::byte {1}, std::byte {2}, std::byte {3}};
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
DataHeader header(std::uint32_t sequence)
{
    return {.sequence = SequenceNumber {sequence},
        .message_number = 1U + sequence % 0x03ff'ffffU,
        .boundary = MessageBoundary::solo,
        .in_order = true,
        .timestamp = PacketTimestamp {1},
        .destination_socket_id = group_id};
}
std::vector<std::byte> packet(std::uint32_t sequence)
{
    std::array<std::byte, 64> bytes {};
    const auto result = encode_packet({.kind = PacketKind::data,
                                          .data = header(sequence),
                                          .payload = plaintext},
        bytes);
    REQUIRE(result);
    return {bytes.begin(), bytes.begin() + result.bytes_written};
}
struct Capture {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::vector<std::byte>> attempts;
    unsigned failures = 0;
    bool fatal = false;
    bool partial = false;
    Emitter::TimePoint* clock = nullptr;
    Emitter::Clock::duration tick {};
};
UdpIoResult capture(
    std::span<const std::byte> bytes, IpEndpoint, void* context) noexcept
{
    auto& c = *static_cast<Capture*>(context);
    std::lock_guard lock(c.mutex);
    c.attempts.emplace_back(bytes.begin(), bytes.end());
    c.changed.notify_all();
    if (c.clock)
        *c.clock += c.tick;
    if (c.fatal)
        return {.error = Error::io_error, .system_error = 22};
    if (c.partial)
        return {.bytes_transferred = bytes.size() - 1};
    if (c.failures != 0) {
        --c.failures;
        return {.error = Error::would_block, .system_error = 7};
    }
    return {.bytes_transferred = bytes.size()};
}
Emitter::TimePoint read_clock(void* p) noexcept
{
    return *static_cast<Emitter::TimePoint*>(p);
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
        std::shared_ptr<const void> context,
        Emitter::TimePoint deadline = Emitter::TimePoint {} + 10s)
    {
        return emitter->enqueue(
            generation, packet(sequence), deadline, {}, std::move(context));
    }
};
struct Owner {
    std::weak_ptr<Emitter> emitter;
    std::shared_ptr<std::atomic<unsigned>> retired;
    ~Owner()
    {
        if (auto source = emitter.lock())
            (void)source->snapshot();
        ++*retired;
    }
};
std::shared_ptr<Owner> owner(const std::shared_ptr<Emitter>& emitter,
    const std::shared_ptr<std::atomic<unsigned>>& retired)
{
    auto result = std::make_shared<Owner>();
    result->emitter = emitter;
    result->retired = retired;
    return result;
}
std::shared_ptr<RuntimeScheduler> scheduler()
{
    auto result = std::make_shared<RuntimeScheduler>(
        RuntimeScheduler::Configuration {1, 2, 2, 1});
    REQUIRE(result->start());
    return result;
}
} // namespace

TEST(
    compat_group_outcome_complete_udp_submission_keeps_exact_context_and_post_io_time)
{
    Fixture f;
    auto context = std::make_shared<int>(17);
    auto retired = std::make_shared<std::atomic<unsigned>>(0);
    auto crypto_owner = owner(f.emitter, retired);
    std::weak_ptr<Owner> weak_crypto = crypto_owner;
    f.output.clock = &f.now;
    f.output.tick = 13us;
    REQUIRE_EQ(f.emitter->enqueue(generation, packet(1), f.now + 1s,
                   std::move(crypto_owner), context),
        Admission::accepted);
    REQUIRE_EQ(f.emitter->snapshot().reported_queued, 1U);
    std::array<Emitter::Outcome, 1> reports;
    REQUIRE_EQ(f.emitter->take_outcomes(reports), 0U);
    f.emitter->pump_for_testing();
    REQUIRE(weak_crypto.expired());
    REQUIRE_EQ(retired->load(), 1U);
    REQUIRE_EQ(f.emitter->take_outcomes(reports), 1U);
    const auto& report = reports[0];
    REQUIRE_EQ(report.group_id, group_id);
    REQUIRE_EQ(report.generation, generation);
    REQUIRE_EQ(report.sequence, SequenceNumber {1});
    REQUIRE_EQ(report.disposition, Disposition::submitted);
    REQUIRE_EQ(report.failure, Emitter::Failure::none);
    REQUIRE_EQ(report.system_error, 0);
    REQUIRE_EQ(report.attempts, 1U);
    REQUIRE_EQ(report.datagram_size, packet_header_size + plaintext.size());
    REQUIRE_EQ(report.completed_at, Emitter::TimePoint {} + 13us);
    REQUIRE_EQ(report.context, context);
    REQUIRE_EQ(f.emitter->snapshot().reported_queued, 0U);
    REQUIRE_EQ(f.emitter->take_outcomes(reports), 0U);
    REQUIRE_EQ(
        reports[0].context, context); // Empty drain does not replace output.
}

TEST(compat_group_outcome_retry_has_one_terminal_report_and_does_not_hide_a_gap)
{
    Fixture f;
    f.config.maximum_attempts = 2;
    f.recreate();
    auto first = std::make_shared<int>(1);
    auto second = std::make_shared<int>(2);
    f.output.failures = 2;
    REQUIRE_EQ(f.enqueue(1, first), Admission::accepted);
    REQUIRE_EQ(f.enqueue(2, second), Admission::accepted);
    std::array<Emitter::Outcome, 2> reports;
    f.emitter->pump_for_testing();
    REQUIRE_EQ(f.emitter->take_outcomes(reports), 0U);
    f.now += 1ms;
    f.emitter->pump_for_testing();
    REQUIRE_EQ(f.emitter->snapshot().last_submitted_sequence,
        std::optional {SequenceNumber {2}});
    REQUIRE_EQ(f.emitter->take_outcomes(reports), 2U);
    REQUIRE_EQ(reports[0].sequence, SequenceNumber {1});
    REQUIRE_EQ(reports[0].disposition, Disposition::retry_exhausted);
    REQUIRE_EQ(reports[0].attempts, 2U);
    REQUIRE_EQ(reports[0].system_error, 7);
    REQUIRE_EQ(reports[0].context, first);
    REQUIRE_EQ(reports[1].sequence, SequenceNumber {2});
    REQUIRE_EQ(reports[1].disposition, Disposition::submitted);
    REQUIRE_EQ(reports[1].context, second);
    REQUIRE_EQ(f.emitter->take_outcomes(reports), 0U);
}

TEST(compat_group_outcome_expiry_and_submission_after_deadline_are_distinct)
{
    Fixture f;
    REQUIRE_EQ(f.enqueue(1, std::make_shared<int>(1), f.now + 1ms),
        Admission::accepted);
    f.now += 1ms;
    f.emitter->pump_for_testing();
    std::array<Emitter::Outcome, 1> reports;
    REQUIRE_EQ(f.emitter->take_outcomes(reports), 1U);
    REQUIRE_EQ(reports[0].disposition, Disposition::expired);
    REQUIRE_EQ(reports[0].attempts, 0U);
    REQUIRE_EQ(reports[0].completed_at, f.now);
    f.output.clock = &f.now;
    f.output.tick = 2ms;
    REQUIRE_EQ(f.enqueue(2, std::make_shared<int>(2), f.now + 1ms),
        Admission::accepted);
    f.emitter->pump_for_testing();
    REQUIRE_EQ(f.emitter->take_outcomes(reports), 1U);
    REQUIRE_EQ(reports[0].disposition, Disposition::submitted);
    REQUIRE_EQ(reports[0].completed_at, Emitter::TimePoint {} + 3ms);
    REQUIRE_EQ(reports[0].attempts, 1U);
}

TEST(
    compat_group_outcome_transport_and_partial_failure_report_every_accepted_original)
{
    for (const bool partial : {false, true}) {
        Fixture f;
        f.output.partial = partial;
        f.output.fatal = !partial;
        for (std::uint32_t i = 1; i <= 3; ++i)
            REQUIRE_EQ(f.enqueue(i, std::make_shared<int>(static_cast<int>(i))),
                Admission::accepted);
        f.emitter->pump_for_testing();
        std::array<Emitter::Outcome, 3> reports;
        REQUIRE_EQ(f.emitter->take_outcomes(reports), 3U);
        for (std::size_t i = 0; i < reports.size(); ++i) {
            REQUIRE_EQ(reports[i].sequence,
                SequenceNumber {static_cast<std::uint32_t>(i + 1)});
            REQUIRE_EQ(reports[i].disposition, Disposition::failed);
            REQUIRE_EQ(reports[i].failure,
                partial ? Emitter::Failure::partial_datagram
                        : Emitter::Failure::transport);
            REQUIRE_EQ(reports[i].attempts, i == 0 ? 1U : 0U);
            REQUIRE_EQ(reports[i].system_error, partial ? 0 : 22);
        }
        REQUIRE_EQ(f.emitter->snapshot().submitted_packets, 0U);
        f.emitter->close();
        REQUIRE_EQ(f.emitter->take_outcomes(reports), 0U);
    }
}

TEST(
    compat_group_outcome_close_preserves_completed_and_cancelled_reports_until_drain)
{
    Fixture f;
    for (std::uint32_t i = 1; i <= 3; ++i)
        REQUIRE_EQ(f.enqueue(i, std::make_shared<int>(static_cast<int>(i))),
            Admission::accepted);
    f.emitter->pump_for_testing(); // Only first packet passes the pacer.
    f.now += 10us;
    f.emitter->close();
    f.emitter->close();
    std::array<Emitter::Outcome, 3> reports;
    REQUIRE_EQ(f.emitter->take_outcomes(reports), 3U);
    REQUIRE_EQ(reports[0].disposition, Disposition::submitted);
    for (std::size_t i = 1; i < reports.size(); ++i) {
        REQUIRE_EQ(reports[i].disposition, Disposition::cancelled);
        REQUIRE_EQ(reports[i].attempts, 0U);
        REQUIRE_EQ(reports[i].completed_at, f.now);
    }
    REQUIRE(!f.emitter->snapshot().report_writable);
    REQUIRE_EQ(f.emitter->take_outcomes(reports), 0U);
}

TEST(
    compat_group_outcome_mailbox_capacity_is_reserved_before_sealing_and_wakes_after_drain)
{
    Fixture f;
    f.config.capacity = 1;
    f.recreate();
    ReadinessObserver observer;
    ReadinessWatch watch(observer, 81, false);
    REQUIRE(watch.bind(f.emitter->outcome_readiness_source()));
    std::vector<SRTSOCKET> handles;
    handles.reserve(1);
    (void)observer.take_changes(handles); // Consume initial watch invalidation.
    REQUIRE_EQ(f.enqueue(1, std::make_shared<int>(1)), Admission::accepted);
    REQUIRE(!f.emitter->snapshot().report_writable);
    const auto before = observer.generation();
    f.emitter->pump_for_testing();
    REQUIRE(observer.generation() > before);
    REQUIRE(!observer.take_changes(handles));
    REQUIRE_EQ(handles.size(), 1U);
    REQUIRE_EQ(handles[0], 81);
    REQUIRE(f.emitter->snapshot().writable);
    REQUIRE(!f.emitter->snapshot().report_writable);
    REQUIRE_EQ(
        f.emitter->reserve(generation, SequenceNumber {2}, f.now + 1s, true)
            .status(),
        Admission::full);
    auto encoder = GroupPacketEncoder::create({.group_id = group_id,
        .stream_generation = generation,
        .initial_sequence = SequenceNumber {2}});
    REQUIRE(encoder);
    REQUIRE(!encoder->snapshot().prepared);
    std::array<Emitter::Outcome, 1> reports;
    const auto before_drain = observer.generation();
    REQUIRE_EQ(f.emitter->take_outcomes(reports), 1U);
    REQUIRE(observer.generation() > before_drain);
    REQUIRE(f.emitter->snapshot().report_writable);
    REQUIRE_EQ(f.emitter->outcome_readiness_source()->low_epochs().in, 1U);
    auto reserved =
        f.emitter->reserve(generation, SequenceNumber {2}, f.now + 1s, true);
    REQUIRE_EQ(reserved.status(), Admission::accepted);
}

TEST(
    compat_group_outcome_rejected_publication_has_no_event_and_retires_context_outside_lock)
{
    Fixture f;
    auto retired = std::make_shared<std::atomic<unsigned>>(0);
    {
        auto reserved = f.emitter->reserve(
            generation, SequenceNumber {1}, f.now + 1s, true);
        REQUIRE_EQ(reserved.status(), Admission::accepted);
    }
    auto missing =
        f.emitter->reserve(generation, SequenceNumber {1}, f.now + 1s, true);
    REQUIRE_EQ(missing.publish(packet(1)), Admission::invalid_packet);
    auto unsolicited =
        f.emitter->reserve(generation, SequenceNumber {1}, f.now + 1s);
    REQUIRE_EQ(unsolicited.publish(
                   packet(1), {}, nullptr, nullptr, owner(f.emitter, retired)),
        Admission::invalid_packet);
    auto rejected =
        f.emitter->reserve(generation, SequenceNumber {1}, f.now + 1s, true);
    REQUIRE_EQ(rejected.publish(
                   packet(1), {},
                   [](void*) noexcept {
                       return false;
                   },
                   nullptr, owner(f.emitter, retired)),
        Admission::cancelled);
    REQUIRE_EQ(retired->load(), 2U);
    REQUIRE_EQ(f.emitter->snapshot().accepted_packets, 0U);
    REQUIRE_EQ(f.emitter->snapshot().reported_queued, 0U);
    REQUIRE(f.emitter->snapshot().report_writable);
    std::array<Emitter::Outcome, 1> reports;
    REQUIRE_EQ(f.emitter->take_outcomes(reports), 0U);
}

TEST(
    compat_group_outcome_drain_replaces_old_contexts_outside_lock_and_ring_reuses_capacity)
{
    Fixture f;
    f.config.capacity = 1;
    f.recreate();
    auto retired = std::make_shared<std::atomic<unsigned>>(0);
    std::array<Emitter::Outcome, 1> reports;
    for (std::uint32_t i = 1; i <= 5; ++i) {
        REQUIRE_EQ(
            f.enqueue(i, owner(f.emitter, retired)), Admission::accepted);
        f.emitter->pump_for_testing();
        REQUIRE_EQ(f.emitter->take_outcomes({}), 0U);
        REQUIRE_EQ(f.emitter->snapshot().pending_outcomes, 1U);
        REQUIRE_EQ(f.emitter->take_outcomes(reports), 1U);
        REQUIRE_EQ(reports[0].sequence, SequenceNumber {i});
        REQUIRE_EQ(retired->load(), i - 1);
        f.now += 1ms;
    }
    reports[0] = Emitter::Outcome {};
    REQUIRE_EQ(retired->load(), 5U);
}

TEST(
    compat_group_outcome_each_drain_has_fixed_budget_even_after_full_queue_close)
{
    Fixture f;
    f.config.capacity = Emitter::maximum_capacity;
    f.recreate();
    for (std::uint32_t i = 1; i <= Emitter::maximum_capacity; ++i)
        REQUIRE_EQ(f.enqueue(i, std::make_shared<int>(static_cast<int>(i))),
            Admission::accepted);
    f.emitter->close();
    REQUIRE_EQ(
        f.emitter->snapshot().pending_outcomes, Emitter::maximum_capacity);
    std::array<Emitter::Outcome, Emitter::maximum_capacity> reports;
    std::size_t total = 0;
    while (total < Emitter::maximum_capacity) {
        const auto count = f.emitter->take_outcomes(reports);
        REQUIRE_EQ(count, Emitter::maximum_batch);
        for (std::size_t i = 0; i < count; ++i) {
            REQUIRE_EQ(reports[i].sequence,
                SequenceNumber {static_cast<std::uint32_t>(total + i + 1)});
            REQUIRE_EQ(reports[i].disposition, Disposition::cancelled);
            REQUIRE_EQ(reports[i].attempts, 0U);
        }
        total += count;
    }
    REQUIRE_EQ(f.emitter->take_outcomes(reports), 0U);
}

TEST(compat_group_outcome_mailbox_does_not_change_unreported_enqueue_contract)
{
    Fixture f;
    f.config.capacity = 1;
    f.recreate();
    REQUIRE_EQ(f.enqueue(1, std::make_shared<int>(1)), Admission::accepted);
    f.emitter->pump_for_testing();
    REQUIRE(!f.emitter->snapshot().report_writable);
    REQUIRE_EQ(f.emitter->enqueue(generation, packet(2), f.now + 1s),
        Admission::accepted);
    f.now += 1ms;
    f.emitter->pump_for_testing();
    REQUIRE_EQ(f.emitter->snapshot().submitted_packets, 2U);
    REQUIRE_EQ(f.emitter->snapshot().pending_outcomes, 1U);
    std::array<Emitter::Outcome, 2> reports;
    REQUIRE_EQ(f.emitter->take_outcomes(reports), 1U);
    REQUIRE_EQ(reports[0].sequence, SequenceNumber {1});
    REQUIRE(f.emitter->snapshot().report_writable);
}

TEST(
    compat_group_outcome_scheduler_failure_reports_prior_work_but_never_unaccepted_tail)
{
    Fixture f;
    auto pool = scheduler();
    auto c = f.config;
    c.clock = nullptr;
    c.clock_context = nullptr;
    c.retry_delay = 1h;
    auto emitter = Emitter::create(c, pool);
    REQUIRE(emitter);
    f.output.failures = 1;
    auto accepted = std::make_shared<int>(1);
    REQUIRE_EQ(emitter->enqueue(generation, packet(1),
                   Emitter::Clock::now() + 24h, {}, accepted),
        Admission::accepted);
    {
        std::unique_lock lock(f.output.mutex);
        REQUIRE(f.output.changed.wait_for(lock, 2s, [&] {
            return !f.output.attempts.empty();
        }));
    }
    REQUIRE_EQ(emitter->snapshot().queued, 1U);
    pool->stop(); // Unexpected stop is intentionally injected before close.
    auto rejected = std::make_shared<int>(2);
    REQUIRE_EQ(emitter->enqueue(generation, packet(2),
                   Emitter::Clock::now() + 24h, {}, rejected),
        Admission::failed);
    std::array<Emitter::Outcome, 2> reports;
    REQUIRE_EQ(emitter->take_outcomes(reports), 1U);
    REQUIRE_EQ(reports[0].sequence, SequenceNumber {1});
    REQUIRE_EQ(reports[0].context, accepted);
    REQUIRE_EQ(reports[0].failure, Emitter::Failure::scheduler);
    REQUIRE_EQ(reports[0].disposition, Disposition::failed);
    REQUIRE_EQ(reports[0].attempts, 1U);
    REQUIRE_EQ(emitter->snapshot().accepted_packets, 1U);
    REQUIRE_EQ(emitter->snapshot().accepted_next_sequence, SequenceNumber {2});
    emitter->close();
    REQUIRE_EQ(emitter->take_outcomes(reports), 0U);
}

TEST(
    compat_group_outcome_sequence_wrap_and_regressed_clock_keep_distinct_reports)
{
    Fixture f;
    f.config.initial_sequence = SequenceNumber {SequenceNumber::mask};
    f.recreate();
    auto a = std::make_shared<int>(1);
    auto b = std::make_shared<int>(2);
    REQUIRE_EQ(f.enqueue(SequenceNumber::mask, a), Admission::accepted);
    REQUIRE_EQ(f.enqueue(0, b), Admission::accepted);
    f.now += 1ms;
    f.emitter->pump_for_testing();
    f.now -= 1ms;
    f.emitter->pump_for_testing();
    REQUIRE_EQ(f.emitter->snapshot().pending_outcomes, 1U);
    f.now += 2ms;
    f.emitter->pump_for_testing();
    std::array<Emitter::Outcome, 2> reports;
    REQUIRE_EQ(f.emitter->take_outcomes(reports), 2U);
    REQUIRE_EQ(reports[0].sequence, SequenceNumber {SequenceNumber::mask});
    REQUIRE_EQ(reports[1].sequence, SequenceNumber {0});
    REQUIRE_EQ(reports[0].context, a);
    REQUIRE_EQ(reports[1].context, b);
    REQUIRE(reports[1].completed_at >= reports[0].completed_at);
}

namespace {
struct History {
    SendBuffer buffer {SequenceNumber {1}, 4};
    std::shared_ptr<const int> binding = std::make_shared<const int>(1);
};
struct Fanout {
    struct Member {
        std::weak_ptr<History> target;
        std::shared_ptr<const int> binding;
        SendBuffer::GroupOriginalSubmission submission;
    };
    std::array<Member, 2> members;
};
struct Commit {
    GroupPacketEncoder* encoder;
    std::shared_ptr<const GroupPacketEncoder::Packet> packet;
    std::array<std::shared_ptr<History>, 2> histories;
    std::shared_ptr<Fanout> fanout;
    CryptoMode mode;
    bool valid = true;
    static bool run(void* p) noexcept
    {
        auto& self = *static_cast<Commit*>(p);
        if (!self.encoder->commit(self.packet))
            return false;
        const auto decoded = decode_packet(self.packet->bytes());
        for (std::size_t i = 0; i < self.histories.size(); ++i) {
            auto& h = *self.histories[i];
            self.valid &=
                h.buffer.import_group_original(decoded.packet.data,
                    decoded.packet.payload, plaintext.size(), self.mode, 0)
                == Error::none;
            const auto token = h.buffer.group_original_submission(
                decoded.packet.data.sequence);
            self.valid &= token.has_value();
            if (token)
                self.fanout->members[i] = {
                    self.histories[i], h.binding, *token};
        }
        return true;
    }
};
// Fixture coordinator only: histories are externally serialized, epoch zero
// matches its injectable clock, and each captured binding is an exact identity.
// Production runtime locking/time conversion is deliberately not emulated here.
std::size_t apply(
    const Emitter::Outcome& outcome, std::uint64_t active_generation)
{
    if (outcome.disposition != Disposition::submitted
        || outcome.group_id != group_id
        || outcome.generation != active_generation)
        return 0;
    const auto context =
        std::static_pointer_cast<const Fanout>(outcome.context);
    std::size_t matched = 0;
    for (const auto& member : context->members) {
        if (member.submission.sequence != outcome.sequence)
            continue;
        if (auto target = member.target.lock();
            target && target->binding == member.binding) {
            target->buffer.note_group_original_submitted(member.submission,
                static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        outcome.completed_at.time_since_epoch())
                        .count()));
            ++matched;
        }
    }
    return matched;
}
std::unique_ptr<GroupPacketEncoder> encoder(CryptoMode mode)
{
    auto keys = std::make_shared<SharedTransmitKeySchedule>(
        SharedTransmitKeySchedule::Configuration {.mode = mode,
            .refresh_rate_packets = 4,
            .preannouncement_packets = 1,
            .origin = SequenceNumber {1}});
    REQUIRE_EQ(keys->initialize(), Error::none);
    auto result = GroupPacketEncoder::create({.group_id = group_id,
        .stream_generation = generation,
        .initial_sequence = SequenceNumber {1},
        .key_schedule = keys});
    REQUIRE(result);
    return result;
}
void admit(Fixture& f, GroupPacketEncoder& encoder, std::uint32_t sequence,
    const std::array<std::shared_ptr<History>, 2>& histories, CryptoMode mode,
    Emitter::TimePoint deadline)
{
    auto reservation = f.emitter->reserve(
        generation, SequenceNumber {sequence}, deadline, true);
    REQUIRE_EQ(reservation.status(), Admission::accepted);
    for (const auto& h : histories)
        REQUIRE_EQ(h->buffer.preflight_group_original(
                       SequenceNumber {sequence}, plaintext.size(), mode),
            Error::none);
    // Allocate captured fanout storage before a nonce can be consumed.
    auto context = std::make_shared<Fanout>();
    auto prepared = encoder.prepare(header(sequence), plaintext);
    REQUIRE(prepared);
    Commit commit {&encoder, prepared.packet, histories, context, mode};
    REQUIRE_EQ(
        reservation.publish(prepared.packet->bytes(),
            prepared.packet->generation_owner(), Commit::run, &commit, context),
        Admission::accepted);
    REQUIRE(commit.valid);
}
} // namespace

TEST(
    compat_group_outcome_captured_ctr_gcm_history_fanout_never_authorizes_an_expired_gap)
{
    for (const auto mode : {CryptoMode::aes_ctr, CryptoMode::aes_gcm}) {
        Fixture f;
        auto source = encoder(mode);
        const std::array histories {
            std::make_shared<History>(), std::make_shared<History>()};
        admit(f, *source, 1, histories, mode, f.now + 1ms);
        admit(f, *source, 2, histories, mode, f.now + 10s);
        f.now += 1ms;
        f.emitter->pump_for_testing();
        std::array<Emitter::Outcome, 2> outcomes;
        REQUIRE_EQ(f.emitter->take_outcomes(outcomes), 2U);
        REQUIRE_EQ(apply(outcomes[0], generation), 0U);
        REQUIRE_EQ(apply(outcomes[1], generation), 2U);
        for (const auto& h : histories) {
            require_ack_retires_nothing(h->buffer, SequenceNumber {3});
            REQUIRE_EQ(h->buffer.request_retransmission(
                           {SequenceNumber {1}, SequenceNumber {1}}),
                Error::invalid_control_payload);
            REQUIRE_EQ(h->buffer.request_retransmission(
                           {SequenceNumber {2}, SequenceNumber {2}}),
                Error::none);
            const auto repeat = h->buffer.next_packet(true);
            REQUIRE(repeat);
            REQUIRE_EQ(repeat->header.sequence, SequenceNumber {2});
            REQUIRE_EQ(repeat->header.destination_socket_id, group_id);
            REQUIRE(repeat->header.encryption_key != EncryptionKey::none);
        }
    }
}

TEST(
    compat_group_outcome_fixture_fanout_rejects_rebound_members_and_retired_source_generation)
{
    for (const bool destroyed : {false, true}) {
        Fixture f;
        auto source = encoder(CryptoMode::aes_gcm);
        std::array histories {
            std::make_shared<History>(), std::make_shared<History>()};
        admit(f, *source, 1, histories, CryptoMode::aes_gcm, f.now + 1s);
        if (destroyed)
            histories[1].reset();
        else
            histories[1]->binding = std::make_shared<const int>(
                1); // Same value, different identity.
        f.emitter->pump_for_testing();
        std::array<Emitter::Outcome, 1> outcome;
        REQUIRE_EQ(f.emitter->take_outcomes(outcome), 1U);
        REQUIRE_EQ(apply(outcome[0], generation + 1), 0U);
        auto misrouted = outcome[0];
        misrouted.sequence = SequenceNumber {2};
        REQUIRE_EQ(apply(misrouted, generation), 0U);
        require_ack_retires_nothing(histories[0]->buffer, SequenceNumber {2});
        REQUIRE_EQ(apply(outcome[0], generation), 1U);
        require_ack_retires_through(histories[0]->buffer, SequenceNumber {2});
        if (!destroyed)
            require_ack_retires_nothing(
                histories[1]->buffer, SequenceNumber {2});
    }
}

TEST(
    compat_group_outcome_source_destruction_releases_abandoned_contexts_without_cycles)
{
    Fixture f;
    auto retired = std::make_shared<std::atomic<unsigned>>(0);
    REQUIRE_EQ(f.enqueue(1, owner(f.emitter, retired)), Admission::accepted);
    f.emitter->pump_for_testing();
    REQUIRE_EQ(f.enqueue(2, owner(f.emitter, retired)), Admission::accepted);
    f.emitter
        .reset(); // One completed and one queued report abandoned by owner.
    REQUIRE_EQ(retired->load(), 2U);
}

namespace {
struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool released = false;
};
void release(const std::shared_ptr<Gate>& gate)
{
    std::lock_guard lock(gate->mutex);
    gate->released = true;
    gate->changed.notify_all();
}
struct Release {
    std::shared_ptr<Gate> gate;
    ~Release()
    {
        release(gate);
    }
};
UdpIoResult blocked(
    std::span<const std::byte> bytes, IpEndpoint, void* context) noexcept
{
    auto& gate = *static_cast<Gate*>(context);
    std::unique_lock lock(gate.mutex);
    gate.entered = true;
    gate.changed.notify_all();
    gate.changed.wait(lock, [&] {
        return gate.released;
    });
    return {.bytes_transferred = bytes.size()};
}
struct Drained {
    std::size_t count = 0;
    std::array<Emitter::Outcome, 1> reports;
};
} // namespace

TEST(
    compat_group_outcome_live_worker_and_concurrent_drain_publish_exactly_once_after_io)
{
    Fixture f;
    auto gate = std::make_shared<Gate>();
    auto pool = scheduler();
    f.channel->set_send_hook_for_testing(blocked, gate.get());
    auto c = f.config;
    c.clock = nullptr;
    c.clock_context = nullptr;
    auto emitter = Emitter::create(c, pool);
    REQUIRE(emitter);
    std::future<Drained> draining;
    Release cleanup {
        gate}; // Releases I/O before a failed assertion joins the future.
    auto context = std::make_shared<int>(1);
    REQUIRE_EQ(emitter->enqueue(generation, packet(1),
                   Emitter::Clock::now() + 2s, {}, context),
        Admission::accepted);
    {
        std::unique_lock lock(gate->mutex);
        REQUIRE(gate->changed.wait_for(lock, 2s, [&] {
            return gate->entered;
        }));
    }
    std::promise<void> requested;
    auto started = requested.get_future();
    draining = std::async(std::launch::async,
        [emitter, requested = std::move(requested)]() mutable {
            Drained result;
            requested.set_value();
            result.count = emitter->take_outcomes(result.reports);
            return result;
        });
    REQUIRE_EQ(started.wait_for(2s), std::future_status::ready);
    REQUIRE_EQ(draining.wait_for(10ms), std::future_status::timeout);
    release(gate);
    REQUIRE_EQ(draining.wait_for(2s), std::future_status::ready);
    const auto result = draining.get();
    REQUIRE_EQ(result.count, 1U);
    REQUIRE_EQ(result.reports[0].disposition, Disposition::submitted);
    REQUIRE_EQ(result.reports[0].context, context);
    std::array<Emitter::Outcome, 1> remaining;
    emitter->close();
    REQUIRE_EQ(emitter->take_outcomes(remaining), 0U);
    pool->stop();
    f.channel->set_send_hook_for_testing(capture, &f.output);
}
