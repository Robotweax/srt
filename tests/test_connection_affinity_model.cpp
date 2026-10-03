#include "test.hpp"
#include "compat/connection_affinity_model.hpp"

#include <array>
#include <atomic>
#include <barrier>
#include <thread>

namespace robotweax::srt::test {
extern std::atomic<std::size_t> affinity_allocations;
}

namespace {
using Model = robotweax::srt::compat::model::ConnectionAffinity;
using Credits = robotweax::srt::compat::model::ChannelCredits;
using Kind = Model::Kind;
using Admission = Model::Admission;
using Phase = Model::Phase;
using CommandState = Model::CommandState;
constexpr std::array payload {
    std::byte {0x53}, std::byte {0x52}, std::byte {0x54}};

template <class T> T present(std::optional<T> value)
{
    REQUIRE(value.has_value());
    return *value;
}
Model::Key connection(Model& model, std::uint32_t wire = 1, bool setup = false)
{
    return present(model.admit(1, wire, setup));
}
Model::Turn turn(Model& model, Model::Key key)
{
    auto result = present(model.begin_turn(model.snapshot(key).shard));
    REQUIRE_EQ(result.key, key);
    return result;
}
void drain(Model& model, Model::Key key, std::uint64_t now = 100)
{
    auto token = present(model.begin_drain(key));
    REQUIRE(!model.begin_drain(key));
    REQUIRE(model.finish_drain(token, now));
    REQUIRE(!model.finish_drain(token, now));
}
void consume(Model& model, Model::Turn owner, std::uint64_t now)
{
    while (auto event = model.next(owner, now)) {
        if (event->kind == Kind::command)
            REQUIRE(model.commit(owner, event->command, now, 42));
        if (event->kind == Kind::timer)
            REQUIRE(model.commit_timer(owner, event->timer, now));
    }
    REQUIRE(model.finish_turn(owner, now));
}
} // namespace

TEST(affinity_owned_prefix_survives_reuse_and_ring_wrap)
{
    Model model({});
    auto key = connection(model);
    std::array<std::byte, Model::maximum_bytes> scratch;
    scratch.fill(std::byte {0xAA});
    REQUIRE_EQ(model.publish(key, Kind::data, scratch, 1), Admission::accepted);
    scratch.fill(std::byte {0xBB});
    auto owner = turn(model, key);
    const auto retained = present(model.next(owner, 2));
    REQUIRE_EQ(retained.size, scratch.size());
    for (auto byte : retained.bytes)
        REQUIRE_EQ(byte, std::byte {0xAA});
    REQUIRE(model.finish_turn(owner, 2));
    for (std::size_t i = 0; i < 130; ++i) {
        REQUIRE_EQ(model.publish(key, Kind::data, payload, i + 3),
            Admission::accepted);
        owner = turn(model, key);
        const auto event = present(model.next(owner, i + 4));
        REQUIRE_EQ(event.size, payload.size());
        for (std::size_t j = 0; j < payload.size(); ++j)
            REQUIRE_EQ(event.bytes[j], payload[j]);
        REQUIRE(model.finish_turn(owner, i + 4));
    }
    for (auto byte : retained.bytes)
        REQUIRE_EQ(byte, std::byte {0xAA});
    std::array<std::byte, Model::maximum_bytes + 1> oversized {};
    REQUIRE_EQ(
        model.publish(key, Kind::data, oversized, 200), Admission::invalid);
    REQUIRE_EQ(model.publish(key, Kind::data, {}, 200), Admission::invalid);
    REQUIRE_EQ(
        model.publish(key, Kind::command, payload, 200), Admission::invalid);
    REQUIRE_EQ(model.snapshot(key).datagrams, 0U);
}

TEST(affinity_data_reserve_and_control_overflow_are_connection_local)
{
    Model model({});
    auto key = connection(model);
    auto other = connection(model, 2);
    for (std::size_t i = 0; i < Model::data_capacity; ++i)
        REQUIRE_EQ(
            model.publish(key, Kind::data, payload, 1), Admission::accepted);
    REQUIRE_EQ(model.publish(key, Kind::data, payload, 2), Admission::full);
    REQUIRE_EQ(model.snapshot(key).phase, Phase::ready);
    REQUIRE_EQ(model.snapshot(key).data_rejections, 1U);
    for (std::size_t i = Model::data_capacity; i < Model::datagram_capacity;
        ++i)
        REQUIRE_EQ(
            model.publish(key, Kind::control, payload, 2), Admission::accepted);
    REQUIRE_EQ(model.snapshot(key).inbox_highwater, Model::datagram_capacity);
    REQUIRE_EQ(model.publish(key, Kind::control, payload, 3), Admission::full);
    REQUIRE_EQ(model.snapshot(key).phase, Phase::closing);
    REQUIRE_EQ(model.snapshot(key).control_rejections, 1U);
    REQUIRE_EQ(model.snapshot(key).datagrams, 0U);
    REQUIRE_EQ(model.publish(key, Kind::data, payload, 3), Admission::closed);
    REQUIRE_EQ(
        model.publish(other, Kind::control, payload, 3), Admission::accepted);
    consume(model, turn(model, other), 4);
    drain(model, key);
}

TEST(affinity_pool_and_wake_reservations_fail_closed_at_configuration)
{
    const auto bytes = Model::storage_bytes();
    REQUIRE(bytes <= 16U * 1024U * 1024U);
    Model too_small({2, bytes - 1, 2});
    REQUIRE(!too_small.valid());
    REQUIRE(!too_small.admit(1, 1));
    Model no_reserve({2, bytes, 1});
    REQUIRE(!no_reserve.valid());
    Model no_shards({0, bytes, 0});
    REQUIRE(!no_shards.valid());
    Model too_many_shards({65, bytes, 65});
    REQUIRE(!too_many_shards.valid());
    Model model({64, bytes, 64});
    REQUIRE(model.valid());
    REQUIRE(!model.admit(0, 1));
    REQUIRE(!model.admit(1, 0));
    for (std::size_t i = 0; i < Model::maximum_connections; ++i) {
        auto key = connection(model, static_cast<std::uint32_t>(i + 1));
        REQUIRE_EQ(model.snapshot(key).shard, i);
        REQUIRE_EQ(
            model.publish(key, Kind::control, payload, 1), Admission::accepted);
        REQUIRE_EQ(model.wake_snapshot(i).ready, 1U);
    }
    REQUIRE(!model.admit(1, 100));
    REQUIRE(!model.admit(1, 1));
}

TEST(affinity_full_doorbell_and_publish_at_idle_boundaries_keep_work_reachable)
{
    Model model({1, 16U * 1024U * 1024U, 1});
    auto key = connection(model);
    REQUIRE_EQ(model.publish(key, Kind::data, payload, 1), Admission::accepted);
    REQUIRE(model.notify(key, Kind::send, 1));
    REQUIRE(model.notify(key, Kind::send, 2));
    REQUIRE_EQ(model.wake_snapshot(0).ready, 1U);
    model.observe_wake(0, Model::WakeResult::full, 2);
    REQUIRE(model.wake_snapshot(0).fallback);
    REQUIRE_EQ(model.wake_snapshot(0).full_rejections, 1U);
    auto owner = turn(model, key);
    REQUIRE(!model.begin_turn(0));
    REQUIRE_EQ(present(model.next(owner, 3)).kind, Kind::data);
    REQUIRE_EQ(present(model.next(owner, 3)).kind, Kind::send);
    REQUIRE(!model.next(owner, 3));
    // Producer enters after the owner's empty scan but before its idle transition.
    REQUIRE_EQ(
        model.publish(key, Kind::control, payload, 4), Admission::accepted);
    REQUIRE_EQ(model.wake_snapshot(0).ready, 0U);
    REQUIRE(model.finish_turn(owner, 4));
    REQUIRE_EQ(model.wake_snapshot(0).ready, 1U);
    model.observe_wake(0, Model::WakeResult::accepted, 4);
    REQUIRE(model.wake_snapshot(0).doorbell);
    consume(model, turn(model, key), 5);
    REQUIRE(!model.wake_snapshot(0).fallback);
    // Producer enters after idle: reserve is armed directly by publication.
    REQUIRE_EQ(model.publish(key, Kind::data, payload, 6), Admission::accepted);
    REQUIRE(model.wake_snapshot(0).fallback);
    consume(model, turn(model, key), 7);
}

TEST(affinity_stopped_shard_closes_its_connections_without_stranding_others)
{
    Model model({});
    auto first = connection(model);
    auto second = connection(model, 2);
    auto third = connection(model, 3);
    REQUIRE_EQ(
        model.publish(first, Kind::control, payload, 1), Admission::accepted);
    auto owner = turn(model, first);
    REQUIRE_EQ(
        model.publish(third, Kind::control, payload, 1), Admission::accepted);
    model.observe_wake(0, Model::WakeResult::stopped, 2);
    REQUIRE_EQ(model.snapshot(first).phase, Phase::closing);
    REQUIRE_EQ(model.snapshot(third).phase, Phase::closing);
    REQUIRE(!model.next(owner, 3));
    REQUIRE(!model.begin_drain(first));
    REQUIRE(model.finish_turn(owner, 4));
    REQUIRE(!model.begin_turn(0));
    REQUIRE_EQ(
        model.publish(second, Kind::control, payload, 4), Admission::accepted);
    consume(model, turn(model, second), 5);
    drain(model, first);
    drain(model, third);
}

TEST(affinity_tickets_order_datagrams_commands_and_deduplicated_markers)
{
    Model model({});
    auto key = connection(model);
    REQUIRE_EQ(
        model.publish(key, Kind::control, payload, 1), Admission::accepted);
    auto command = present(model.publish_command(key, payload, 2));
    REQUIRE(model.notify(key, Kind::send, 3));
    REQUIRE(model.notify(key, Kind::send, 100));
    REQUIRE_EQ(model.publish(key, Kind::data, payload, 4), Admission::accepted);
    auto timer = present(model.arm_timer(key, 5));
    REQUIRE(model.publish_timer(timer, 5));
    REQUIRE(model.publish_timer(timer, 6));
    REQUIRE(model.notify(key, Kind::receive_release, 6));
    auto owner = turn(model, key);
    constexpr std::array order {Kind::control, Kind::command, Kind::send,
        Kind::data, Kind::timer, Kind::receive_release};
    for (std::size_t i = 0; i < order.size(); ++i) {
        auto event = present(model.next(owner, 10));
        REQUIRE_EQ(event.kind, order[i]);
        REQUIRE_EQ(event.ticket, i + 1);
        if (event.kind == Kind::command)
            REQUIRE(model.commit(owner, command, 10, 7));
        if (event.kind == Kind::timer)
            REQUIRE(model.commit_timer(owner, timer, 10));
        if (event.kind == Kind::send)
            REQUIRE_EQ(event.published_at, 3U);
    }
    REQUIRE(!model.next(owner, 10));
    REQUIRE(model.finish_turn(owner, 10));
    REQUIRE_EQ(present(model.take_result(command)), 7);
    REQUIRE_EQ(model.snapshot(key).max_queue_delay, 9U);
}

TEST(affinity_commands_own_input_cancel_before_commit_and_return_once)
{
    Model model({});
    auto key = connection(model);
    auto scratch = payload;
    auto command = present(model.publish_command(key, scratch, 1));
    scratch.fill(std::byte {0});
    auto owner = turn(model, key);
    auto event = present(model.next(owner, 2));
    REQUIRE_EQ(event.command, command);
    for (std::size_t i = 0; i < payload.size(); ++i)
        REQUIRE_EQ(event.bytes[i], payload[i]);
    REQUIRE(!model.finish_turn(owner, 2));
    const auto previous_owner = owner;
    auto invalid_owner = owner;
    ++invalid_owner.serial;
    REQUIRE(!model.commit(invalid_owner, command, 2, 5));
    REQUIRE(!model.wait(invalid_owner, command));
    REQUIRE(model.cancel(command));
    REQUIRE(!model.commit(owner, command, 2, 5));
    REQUIRE(!model.cancel(command));
    REQUIRE_EQ(present(model.take_result(command)), -1);
    REQUIRE(!model.take_result(command));
    REQUIRE(model.finish_turn(owner, 2));
    auto replacement = present(model.publish_command(key, payload, 3));
    REQUIRE_EQ(replacement.slot, command.slot);
    REQUIRE(replacement.serial != command.serial);
    REQUIRE(!model.cancel(command));
    owner = turn(model, key);
    REQUIRE(model.next(owner, 4));
    REQUIRE(!model.commit(previous_owner, replacement, 4, 9));
    REQUIRE(model.commit(owner, replacement, 4, 9));
    REQUIRE(!model.cancel(replacement));
    REQUIRE(!model.commit(owner, replacement, 4, 10));
    REQUIRE(model.finish_turn(owner, 4));
    REQUIRE_EQ(present(model.take_result(replacement)), 9);
    REQUIRE(!model.take_result(replacement));
    auto queued = present(model.publish_command(key, payload, 5));
    REQUIRE(model.cancel(queued));
    owner = turn(model, key);
    REQUIRE(!model.next(owner, 6));
    REQUIRE(model.finish_turn(owner, 6));
    REQUIRE_EQ(present(model.take_result(queued)), -1);
}

TEST(affinity_command_pool_deadlines_and_wait_resume_are_bounded)
{
    Model model({1, 16U * 1024U * 1024U, 1});
    auto key = connection(model);
    auto other = connection(model, 2);
    std::array<Model::Command, Model::command_capacity> commands;
    for (auto& command : commands)
        command = present(model.publish_command(key, payload, 1, 10));
    REQUIRE(!model.publish_command(key, payload, 1));
    REQUIRE_EQ(model.snapshot(key).command_highwater, commands.size());
    auto owner = turn(model, key);
    auto event = present(model.next(owner, 2));
    REQUIRE(model.wait(owner, event.command));
    for (std::size_t i = 1; i < commands.size(); ++i)
        REQUIRE(model.cancel(commands[i]));
    REQUIRE_EQ(
        model.publish(other, Kind::data, payload, 3), Admission::accepted);
    REQUIRE(model.finish_turn(owner, 3));
    consume(model, turn(model, other), 4);
    REQUIRE_EQ(
        model.publish(key, Kind::control, payload, 4), Admission::accepted);
    REQUIRE(model.resume(commands[0], 5));
    owner = turn(model, key);
    REQUIRE_EQ(present(model.next(owner, 6)).kind, Kind::control);
    event = present(model.next(owner, 6));
    REQUIRE_EQ(event.command, commands[0]);
    REQUIRE(!model.commit(owner, commands[0], 10, 42));
    REQUIRE(model.finish_turn(owner, 10));
    for (auto command : commands)
        REQUIRE_EQ(present(model.take_result(command)), -1);
    REQUIRE(!model.publish_command(key, payload, 10, 10));
    auto expired = present(model.publish_command(key, payload, 11, 12));
    owner = turn(model, key);
    REQUIRE(!model.next(owner, 12));
    REQUIRE(model.finish_turn(owner, 12));
    REQUIRE_EQ(present(model.take_result(expired)), -1);
    auto waiting = present(model.publish_command(key, payload, 13, 15));
    owner = turn(model, key);
    REQUIRE(model.next(owner, 14));
    REQUIRE(model.wait(owner, waiting));
    REQUIRE(model.finish_turn(owner, 14));
    REQUIRE(!model.resume(waiting, 15));
    REQUIRE_EQ(present(model.take_result(waiting)), -1);
}

TEST(affinity_timer_deadlines_cancel_and_rearm_invalidate_extracted_events)
{
    Model model({});
    auto key = connection(model);
    auto timer = present(model.arm_timer(key, 10));
    REQUIRE(!model.publish_timer(timer, 9));
    REQUIRE(!model.begin_turn(0));
    REQUIRE(model.publish_timer(timer, 10));
    auto owner = turn(model, key);
    auto event = present(model.next(owner, 20));
    REQUIRE_EQ(event.kind, Kind::timer);
    REQUIRE(!model.finish_turn(owner, 20));
    auto invalid_owner = owner;
    ++invalid_owner.serial;
    REQUIRE(!model.commit_timer(invalid_owner, event.timer, 20));
    REQUIRE(model.cancel_timer(timer));
    REQUIRE(!model.commit_timer(owner, event.timer, 20));
    REQUIRE(model.finish_turn(owner, 20));
    auto replacement = present(model.arm_timer(key, 30));
    REQUIRE(!model.publish_timer(timer, 40));
    REQUIRE(!model.cancel_timer(timer));
    REQUIRE(model.publish_timer(replacement, 40));
    owner = turn(model, key);
    event = present(model.next(owner, 40));
    auto latest = present(model.arm_timer(key, 50));
    REQUIRE(!model.commit_timer(owner, event.timer, 40));
    REQUIRE(model.finish_turn(owner, 40));
    REQUIRE(model.publish_timer(latest, 50));
    consume(model, turn(model, key), 60);
    REQUIRE(!model.publish_timer(latest, 60));
    REQUIRE_EQ(model.snapshot(key).max_queue_delay, 10U);
}

TEST(
    affinity_all_connections_progress_with_one_owner_per_shard_and_bounded_turns)
{
    Model model({});
    std::array<Model::Key, Model::maximum_connections> keys;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        keys[i] = connection(model, static_cast<std::uint32_t>(i + 1));
        for (std::size_t n = 0; n < Model::data_capacity; ++n)
            REQUIRE_EQ(model.publish(keys[i], Kind::data, payload, 1),
                Admission::accepted);
    }
    // Every ready connection gets a turn before a hot connection's next turn.
    for (std::size_t round = 0; round < 3; ++round) {
        for (std::size_t i = 0; i < keys.size(); i += 2) {
            auto left = turn(model, keys[i]);
            auto right = turn(model, keys[i + 1]);
            REQUIRE(!model.begin_turn(0));
            REQUIRE(!model.begin_turn(1));
            for (std::size_t n = 0; n < Model::turn_budget; ++n) {
                REQUIRE(model.next(left, 2));
                REQUIRE(model.next(right, 2));
            }
            REQUIRE(!model.next(left, 2));
            REQUIRE(!model.next(right, 2));
            REQUIRE(model.finish_turn(left, 2));
            REQUIRE(model.finish_turn(right, 2));
        }
    }
    for (auto key : keys) {
        REQUIRE_EQ(model.snapshot(key).datagrams, 0U);
        REQUIRE_EQ(model.snapshot(key).turns, 3U);
    }
    REQUIRE(!model.begin_turn(0));
    REQUIRE(!model.begin_turn(1));
}

TEST(affinity_channel_leases_count_failed_attempts_and_do_not_refill_per_turn)
{
    Credits credits;
    std::array<Credits::Lease, 16> leases;
    for (std::size_t i = 0; i < leases.size(); ++i)
        leases[i] = present(credits.grant(i + 1));
    REQUIRE_EQ(credits.snapshot().available_attempts, 0U);
    REQUIRE_EQ(credits.snapshot().available_bytes, 0U);
    REQUIRE(!credits.grant(17));
    REQUIRE(!credits.grant(1));
    REQUIRE(!credits.next_round());
    for (auto lease : leases) {
        REQUIRE(!credits.attempt(lease, 1501));
        REQUIRE(!credits.attempt(lease, 0));
        // A credit is consumed before transport submission, also on EAGAIN.
        for (std::size_t n = 0; n < 4; ++n)
            REQUIRE(credits.attempt(lease, 1500));
        REQUIRE(!credits.attempt(lease, 1));
    }
    REQUIRE_EQ(credits.snapshot().attempts, Credits::attempts_per_round);
    REQUIRE_EQ(credits.snapshot().bytes, Credits::bytes_per_round);
    for (std::size_t i = 0; i + 1 < leases.size(); ++i)
        REQUIRE(credits.release(leases[i]));
    REQUIRE(!credits.next_round());
    REQUIRE(credits.release(leases.back()));
    REQUIRE(!credits.grant(99));
    REQUIRE(credits.next_round());
    REQUIRE(!credits.attempt(leases[0], 1));
    REQUIRE(!credits.release(leases[0]));
    auto lease = present(credits.grant(1));
    REQUIRE(credits.attempt(lease, 100));
    REQUIRE(credits.release(lease));
    REQUIRE_EQ(credits.snapshot().available_attempts, 63U);
    REQUIRE_EQ(credits.snapshot().available_bytes, 95900U);
    auto reused = present(credits.grant(1));
    REQUIRE(!credits.release(lease));
    credits.stop();
    REQUIRE(!credits.attempt(reused, 1));
    REQUIRE(!credits.release(reused));
    REQUIRE(!credits.grant(2));
    REQUIRE(!credits.next_round());
}

TEST(affinity_setup_seal_orders_old_prefix_before_new_publications)
{
    Model model({});
    auto key = connection(model, 1, true);
    REQUIRE(!model.finish_promotion(key));
    REQUIRE_EQ(
        model.publish(key, Kind::control, payload, 1), Admission::invalid);
    REQUIRE_EQ(model.publish(key, Kind::control, payload, 1, true),
        Admission::accepted);
    REQUIRE_EQ(
        model.publish(key, Kind::data, payload, 2, true), Admission::accepted);
    REQUIRE(!model.begin_turn(0));
    REQUIRE(model.seal_setup(key));
    REQUIRE(!model.seal_setup(key));
    REQUIRE_EQ(
        model.publish(key, Kind::control, payload, 3, true), Admission::stale);
    REQUIRE_EQ(
        model.publish(key, Kind::control, payload, 3), Admission::accepted);
    REQUIRE(!model.begin_turn(0));
    REQUIRE(model.finish_promotion(key));
    REQUIRE(!model.finish_promotion(key));
    auto owner = turn(model, key);
    for (std::uint64_t i = 1; i <= 3; ++i)
        REQUIRE_EQ(present(model.next(owner, 4)).ticket, i);
    REQUIRE(model.finish_turn(owner, 4));
    auto overflow = connection(model, 2, true);
    for (std::size_t i = 0; i < Model::datagram_capacity; ++i)
        REQUIRE_EQ(model.publish(overflow, Kind::control, payload, 1, true),
            Admission::accepted);
    REQUIRE_EQ(model.publish(overflow, Kind::control, payload, 2, true),
        Admission::full);
    REQUIRE(!model.seal_setup(overflow));
    REQUIRE(!model.finish_promotion(overflow));
    drain(model, overflow);
}

TEST(affinity_close_bypasses_full_pools_and_waits_for_owner_and_unique_drain)
{
    Model model({});
    auto key = connection(model);
    std::array<Model::Command, Model::command_capacity> commands;
    for (auto& command : commands)
        command = present(model.publish_command(key, payload, 1));
    auto owner = turn(model, key);
    REQUIRE(model.next(owner, 2));
    REQUIRE(model.commit(owner, commands[0], 2, 77));
    REQUIRE(model.next(owner, 2));
    for (std::size_t i = 0; i < Model::datagram_capacity; ++i)
        REQUIRE_EQ(
            model.publish(key, Kind::control, payload, 2), Admission::accepted);
    auto timer = present(model.arm_timer(key, 2));
    REQUIRE(model.publish_timer(timer, 2));
    REQUIRE(model.close(key, 10));
    REQUIRE(!model.close(key, 11));
    REQUIRE(!model.begin_drain(key));
    REQUIRE(!model.next(owner, 11));
    REQUIRE(!model.commit(owner, commands[1], 11, 88));
    REQUIRE(!model.commit_timer(owner, timer, 11));
    REQUIRE(!model.notify(key, Kind::send, 11));
    REQUIRE(!model.publish_command(key, payload, 11));
    REQUIRE(model.finish_turn(owner, 12));
    REQUIRE_EQ(model.snapshot(key).close_quiescence_delay, 2U);
    drain(model, key, 15);
    REQUIRE_EQ(model.snapshot(key).close_completion_delay, 3U);
    REQUIRE(!model.release(key));
    REQUIRE_EQ(present(model.take_result(commands[0])), 77);
    for (std::size_t i = 1; i < commands.size(); ++i)
        REQUIRE_EQ(present(model.take_result(commands[i])), -1);
    REQUIRE(model.release(key));
}

TEST(affinity_route_reuse_and_runtime_restart_reject_all_old_tokens)
{
    Model model({1, 16U * 1024U * 1024U, 1});
    auto old = connection(model);
    auto command = present(model.publish_command(old, payload, 1));
    auto timer = present(model.arm_timer(old, 1));
    REQUIRE(model.publish_timer(timer, 1));
    auto owner = turn(model, old);
    REQUIRE(model.next(owner, 2));
    REQUIRE_EQ(present(model.next(owner, 2)).kind, Kind::timer);
    model.stop(3);
    REQUIRE(!model.restart());
    REQUIRE(!model.admit(1, 1));
    REQUIRE(!model.begin_drain(old));
    REQUIRE(model.finish_turn(owner, 4));
    auto drain_token = present(model.begin_drain(old));
    REQUIRE(!model.restart());
    REQUIRE(model.finish_drain(drain_token, 5));
    REQUIRE(!model.restart());
    REQUIRE_EQ(present(model.take_result(command)), -1);
    REQUIRE(model.restart());
    auto fresh = connection(model);
    REQUIRE_EQ(fresh.slot, old.slot);
    REQUIRE(fresh.runtime != old.runtime);
    REQUIRE(fresh.route != old.route);
    REQUIRE_EQ(model.publish(old, Kind::data, payload, 6), Admission::stale);
    REQUIRE(!model.cancel(command));
    REQUIRE(!model.commit(owner, command, 6, 1));
    REQUIRE(!model.publish_timer(timer, 6));
    REQUIRE(!model.commit_timer(owner, timer, 6));
    REQUIRE(!model.finish_turn(owner, 6));
    REQUIRE(!model.finish_drain(drain_token, 6));
    REQUIRE(!model.close(old, 6));
    REQUIRE(model.close(fresh, 7));
    drain(model, fresh);
    REQUIRE(model.release(fresh));
    auto same_runtime = connection(model);
    REQUIRE_EQ(same_runtime.runtime, fresh.runtime);
    REQUIRE(same_runtime.route != fresh.route);
    REQUIRE_EQ(model.publish(fresh, Kind::data, payload, 8), Admission::stale);
    REQUIRE_EQ(model.publish(same_runtime, Kind::data, payload, 8),
        Admission::accepted);
}

TEST(affinity_readiness_epochs_record_drained_and_terminal_transitions)
{
    Model model({});
    auto key = connection(model);
    const auto initial = model.snapshot(key).readiness;
    REQUIRE(!initial.work);
    REQUIRE(!initial.terminal);
    REQUIRE_EQ(model.publish(key, Kind::data, payload, 1), Admission::accepted);
    const auto high = model.snapshot(key).readiness;
    REQUIRE(high.work);
    REQUIRE(high.change_epoch > initial.change_epoch);
    auto owner = turn(model, key);
    REQUIRE(model.next(owner, 2));
    const auto low = model.snapshot(key).readiness;
    REQUIRE(!low.work);
    REQUIRE_EQ(low.low_epoch, high.low_epoch + 1);
    REQUIRE_EQ(
        model.publish(key, Kind::control, payload, 3), Admission::accepted);
    const auto high_again = model.snapshot(key).readiness;
    REQUIRE(high_again.work);
    REQUIRE_EQ(high_again.low_epoch, low.low_epoch);
    REQUIRE(high_again.change_epoch > low.change_epoch);
    REQUIRE(model.close(key, 4));
    const auto terminal = model.snapshot(key).readiness;
    REQUIRE(terminal.terminal);
    REQUIRE(!terminal.work);
    REQUIRE_EQ(terminal.low_epoch, low.low_epoch + 1);
    REQUIRE(terminal.change_epoch > high_again.change_epoch);
    REQUIRE(model.finish_turn(owner, 5));
    drain(model, key);
}

TEST(affinity_concurrent_producers_preserve_owned_bytes_and_one_ready_entry)
{
    Model model({});
    auto key = connection(model);
    std::barrier rendezvous(3);
    std::array<Admission, 2> admitted {};
    auto produce = [&](std::size_t index) {
        std::array<std::byte, 1500> scratch;
        scratch.fill(static_cast<std::byte>(index + 1));
        rendezvous.arrive_and_wait();
        admitted[index] = model.publish(key, Kind::control, scratch, 1);
        scratch.fill(std::byte {0});
        rendezvous.arrive_and_wait();
    };
    std::jthread first(produce, 0);
    std::jthread second(produce, 1);
    rendezvous.arrive_and_wait();
    rendezvous.arrive_and_wait();
    first.join();
    second.join();
    REQUIRE_EQ(admitted[0], Admission::accepted);
    REQUIRE_EQ(admitted[1], Admission::accepted);
    REQUIRE_EQ(model.wake_snapshot(0).ready, 1U);
    auto owner = turn(model, key);
    std::array<bool, 2> seen {};
    for (std::uint64_t ticket = 1; ticket <= 2; ++ticket) {
        auto event = present(model.next(owner, 2));
        REQUIRE_EQ(event.ticket, ticket);
        REQUIRE_EQ(event.size, 1500U);
        REQUIRE(
            event.bytes[0] == std::byte {1} || event.bytes[0] == std::byte {2});
        const auto index = std::to_integer<std::size_t>(event.bytes[0]) - 1;
        REQUIRE(!seen[index]);
        seen[index] = true;
        for (auto byte : event.bytes)
            REQUIRE_EQ(byte, event.bytes[0]);
    }
    REQUIRE(model.finish_turn(owner, 2));
}

TEST(affinity_full_pool_operations_do_not_allocate_after_setup)
{
    auto before = robotweax::srt::test::affinity_allocations.load();
    Model model({});
    REQUIRE_EQ(robotweax::srt::test::affinity_allocations.load(), before + 1);
    before = robotweax::srt::test::affinity_allocations.load();
    std::array<Model::Key, Model::maximum_connections> keys;
    std::array<std::array<Model::Command, Model::command_capacity>,
        Model::maximum_connections>
        commands;
    std::array<Model::Timer, Model::maximum_connections> timers;
    Credits credits;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        keys[i] = connection(model, static_cast<std::uint32_t>(i + 1));
        for (auto& command : commands[i])
            command = present(model.publish_command(keys[i], payload, 1));
        for (std::size_t n = 0; n < Model::datagram_capacity; ++n)
            REQUIRE_EQ(model.publish(keys[i], Kind::control, payload, 1),
                Admission::accepted);
        REQUIRE(model.notify(keys[i], Kind::send, 1));
        REQUIRE(model.notify(keys[i], Kind::receive_release, 1));
        timers[i] = present(model.arm_timer(keys[i], 2));
        REQUIRE(model.publish_timer(timers[i], 2));
    }
    REQUIRE(!model.admit(1, 100));
    // Visit every slot with payload, command, timer and completion storage live.
    for (auto key : keys)
        consume(model, turn(model, key), 3);
    model.stop(4);
    for (std::size_t i = 0; i < keys.size(); ++i) {
        drain(model, keys[i], 5);
        for (auto command : commands[i])
            REQUIRE(model.take_result(command));
    }
    REQUIRE(model.restart());
    auto key = connection(model);
    auto lease = present(credits.grant(key.route));
    REQUIRE(credits.attempt(lease, 1500));
    REQUIRE(credits.release(lease));
    REQUIRE(credits.next_round());
    REQUIRE_EQ(robotweax::srt::test::affinity_allocations.load(), before);
}
