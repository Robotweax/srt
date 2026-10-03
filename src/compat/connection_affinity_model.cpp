#include "compat/connection_affinity_model.hpp"

#include <algorithm>
#include <limits>

namespace robotweax::srt::compat::model {
namespace {
constexpr auto maximum_counter = (std::numeric_limits<std::uint64_t>::max)();
}

struct ConnectionAffinity::State {
    struct CommandEntry {
        Event event;
        CommandState state = CommandState::stale;
        std::optional<std::uint64_t> deadline;
        std::int64_t result = 0;
    };
    struct Marker {
        Event event;
        bool pending = false;
    };
    struct Slot {
        Key key;
        std::uint32_t wire_id = 0;
        bool used = false;
        bool sealed = false;
        Snapshot view;
        std::array<Event, datagram_capacity> datagrams;
        std::size_t head = 0;
        std::array<CommandEntry, command_capacity> commands;
        std::array<Marker, 3> markers;
        std::uint64_t ticket = 0;
        std::uint64_t turn = 0;
        std::size_t consumed = 0;
        std::uint64_t timer = 0;
        std::uint64_t deadline = 0;
        bool timer_armed = false;
        bool timer_published = false;
        bool timer_issued = false;
        std::uint64_t close_time = 0;
        std::uint64_t quiescent_time = 0;
        bool close_started = false;
    };
    struct Shard {
        std::array<std::size_t, maximum_connections> ready {};
        std::size_t size = 0;
        std::size_t active = maximum_connections;
        bool enabled = true;
        WakeSnapshot wake;
    };
    // A single model lock makes each transition atomic. This is deliberately
    // not the lock topology or performance model of a transport integration.
    mutable std::mutex mutex;
    std::array<Slot, maximum_connections> slots;
    std::array<Shard, maximum_shards> shards;
    std::uint64_t runtime = 1;
    std::uint64_t route = 0;
    std::uint64_t admissions = 0;
    bool stopped = false;

    Slot* find(Key key) noexcept
    {
        if (key.slot >= slots.size())
            return nullptr;
        auto& slot = slots[key.slot];
        return slot.used && slot.key == key ? &slot : nullptr;
    }
    CommandEntry* find(Command command) noexcept
    {
        auto* slot = find(command.key);
        if (slot == nullptr || command.slot >= command_capacity)
            return nullptr;
        auto& entry = slot->commands[command.slot];
        return entry.state != CommandState::stale
                && entry.event.command == command
            ? &entry
            : nullptr;
    }
    bool accepting(const Slot& slot) const noexcept
    {
        return !stopped && shards[slot.view.shard].enabled
            && slot.view.phase != Phase::closing
            && slot.view.phase != Phase::closed;
    }
    bool pending(const Slot& slot) const noexcept
    {
        if (slot.view.datagrams != 0)
            return true;
        for (const auto& command : slot.commands)
            if (command.state == CommandState::queued)
                return true;
        for (const auto& marker : slot.markers)
            if (marker.pending)
                return true;
        return false;
    }
    void signal(Slot& slot) noexcept
    {
        auto& readiness = slot.view.readiness;
        const bool work = accepting(slot) && pending(slot);
        if (readiness.work && !work)
            ++readiness.low_epoch;
        readiness.work = work;
        readiness.terminal = !accepting(slot);
        ++readiness.change_epoch;
    }
    void ready(Slot& slot) noexcept
    {
        signal(slot);
        if (!pending(slot) || slot.view.phase != Phase::idle)
            return;
        auto& shard = shards[slot.view.shard];
        shard.ready[shard.size++] = slot.key.slot;
        shard.wake.ready = shard.size;
        // This is a pre-reserved model service lane, not a normal queue task.
        shard.wake.fallback = true;
        slot.view.phase = Phase::ready;
    }
    void remove_ready(Slot& slot) noexcept
    {
        auto& shard = shards[slot.view.shard];
        std::size_t kept = 0;
        for (std::size_t index = 0; index < shard.size; ++index)
            if (shard.ready[index] != slot.key.slot)
                shard.ready[kept++] = shard.ready[index];
        shard.size = kept;
        shard.wake.ready = kept;
        if (kept == 0) {
            shard.wake.doorbell = false;
            shard.wake.fallback = false;
        }
    }
    void close(Slot& slot, std::uint64_t now) noexcept
    {
        if (slot.close_started)
            return;
        slot.close_started = true;
        slot.close_time = now;
        slot.view.phase = Phase::closing;
        remove_ready(slot);
        slot.view.datagrams = slot.view.data = 0;
        slot.head = 0;
        for (auto& command : slot.commands)
            if (command.state == CommandState::queued
                || command.state == CommandState::issued
                || command.state == CommandState::waiting)
                command.state = CommandState::cancelled;
        for (auto& marker : slot.markers)
            marker.pending = false;
        slot.timer_armed = slot.timer_published = slot.timer_issued = false;
        if (!slot.view.owner_active)
            slot.quiescent_time = now;
        signal(slot);
    }
    std::optional<std::uint64_t> ticket(Slot& slot, std::uint64_t now) noexcept
    {
        if (slot.ticket == maximum_counter) {
            close(slot, now);
            return std::nullopt;
        }
        return ++slot.ticket;
    }
    bool owner(Turn turn) noexcept
    {
        const auto* slot = find(turn.key);
        return slot != nullptr && slot->view.owner_active
            && slot->turn == turn.serial;
    }
};

std::size_t ConnectionAffinity::storage_bytes() noexcept
{
    return sizeof(ConnectionAffinity) + sizeof(State) + sizeof(ChannelCredits);
}

ConnectionAffinity::ConnectionAffinity(Configuration configuration)
    : configuration_(configuration)
{
    if (configuration.shards == 0 || configuration.shards > maximum_shards
        || configuration.reserved_wake_slots < configuration.shards
        || configuration.memory_budget < storage_bytes())
        return;
    state_ = std::make_unique<State>();
}
ConnectionAffinity::~ConnectionAffinity() = default;
bool ConnectionAffinity::valid() const noexcept
{
    return state_ != nullptr;
}

std::optional<ConnectionAffinity::Key> ConnectionAffinity::admit(
    std::uint64_t channel, std::uint32_t wire_id, bool setup) noexcept
{
    if (!valid() || channel == 0 || wire_id == 0)
        return std::nullopt;
    std::lock_guard lock(state_->mutex);
    if (state_->stopped || state_->route == maximum_counter
        || state_->admissions == maximum_counter)
        return std::nullopt;
    for (const auto& slot : state_->slots)
        if (slot.used && slot.key.channel == channel && slot.wire_id == wire_id)
            return std::nullopt;
    const auto shard = state_->admissions % configuration_.shards;
    if (!state_->shards[shard].enabled)
        return std::nullopt;
    for (std::size_t index = 0; index < maximum_connections; ++index) {
        auto& slot = state_->slots[index];
        if (slot.used)
            continue;
        slot = State::Slot {};
        slot.used = true;
        slot.key = {state_->runtime, channel, ++state_->route, index};
        slot.wire_id = wire_id;
        slot.view.shard = shard;
        slot.view.phase = setup ? Phase::setup : Phase::idle;
        ++state_->admissions;
        return slot.key;
    }
    return std::nullopt;
}

ConnectionAffinity::Admission ConnectionAffinity::publish(Key key, Kind kind,
    std::span<const std::byte> bytes, std::uint64_t now,
    bool setup_publisher) noexcept
{
    if (!valid())
        return Admission::stale;
    std::lock_guard lock(state_->mutex);
    auto* slot = state_->find(key);
    if (slot == nullptr)
        return Admission::stale;
    if (!state_->accepting(*slot))
        return Admission::closed;
    if (bytes.empty() || bytes.size() > maximum_bytes
        || (kind != Kind::data && kind != Kind::control))
        return Admission::invalid;
    if (setup_publisher && (slot->sealed || slot->view.phase != Phase::setup))
        return Admission::stale;
    if (!setup_publisher && slot->view.phase == Phase::setup && !slot->sealed)
        return Admission::invalid;
    if (slot->view.datagrams == datagram_capacity
        || (kind == Kind::data && slot->view.data == data_capacity)) {
        if (kind == Kind::data)
            ++slot->view.data_rejections;
        else {
            ++slot->view.control_rejections;
            state_->close(*slot, now);
        }
        return Admission::full;
    }
    const auto ticket = state_->ticket(*slot, now);
    if (!ticket)
        return Admission::closed;
    auto& event = slot->datagrams[(slot->head + slot->view.datagrams)
        % datagram_capacity];
    event.kind = kind;
    event.ticket = *ticket;
    event.published_at = now;
    event.size = bytes.size();
    std::copy(bytes.begin(), bytes.end(), event.bytes.begin());
    ++slot->view.datagrams;
    if (kind == Kind::data)
        ++slot->view.data;
    slot->view.inbox_highwater =
        std::max(slot->view.inbox_highwater, slot->view.datagrams);
    state_->ready(*slot);
    return Admission::accepted;
}

bool ConnectionAffinity::seal_setup(Key key) noexcept
{
    if (!valid())
        return false;
    std::lock_guard lock(state_->mutex);
    auto* slot = state_->find(key);
    if (slot == nullptr || !state_->accepting(*slot)
        || slot->view.phase != Phase::setup || slot->sealed)
        return false;
    slot->sealed = true;
    return true;
}
bool ConnectionAffinity::finish_promotion(Key key) noexcept
{
    if (!valid())
        return false;
    std::lock_guard lock(state_->mutex);
    auto* slot = state_->find(key);
    if (slot == nullptr || !state_->accepting(*slot)
        || slot->view.phase != Phase::setup || !slot->sealed)
        return false;
    slot->view.phase = Phase::idle;
    state_->ready(*slot);
    return true;
}

std::optional<ConnectionAffinity::Command> ConnectionAffinity::publish_command(
    Key key, std::span<const std::byte> bytes, std::uint64_t now,
    std::optional<std::uint64_t> deadline) noexcept
{
    if (!valid() || bytes.size() > maximum_bytes)
        return std::nullopt;
    std::lock_guard lock(state_->mutex);
    auto* slot = state_->find(key);
    if (slot == nullptr || !state_->accepting(*slot)
        || slot->view.phase == Phase::setup || (deadline && now >= *deadline))
        return std::nullopt;
    for (std::size_t index = 0; index < command_capacity; ++index) {
        auto& entry = slot->commands[index];
        if (entry.state != CommandState::stale)
            continue;
        const auto ticket = state_->ticket(*slot, now);
        if (!ticket)
            return std::nullopt;
        entry = State::CommandEntry {};
        entry.event.kind = Kind::command;
        entry.event.ticket = *ticket;
        entry.event.published_at = now;
        entry.event.size = bytes.size();
        std::copy(bytes.begin(), bytes.end(), entry.event.bytes.begin());
        entry.event.command = {key, index, *ticket};
        entry.deadline = deadline;
        entry.state = CommandState::queued;
        ++slot->view.commands;
        slot->view.command_highwater =
            std::max(slot->view.commands, slot->view.command_highwater);
        state_->ready(*slot);
        return entry.event.command;
    }
    return std::nullopt;
}

bool ConnectionAffinity::cancel(Command command) noexcept
{
    if (!valid())
        return false;
    std::lock_guard lock(state_->mutex);
    auto* entry = state_->find(command);
    if (entry == nullptr || entry->state == CommandState::completed
        || entry->state == CommandState::cancelled)
        return false;
    entry->state = CommandState::cancelled;
    state_->signal(*state_->find(command.key));
    return true;
}
bool ConnectionAffinity::commit(
    Turn turn, Command command, std::uint64_t now, std::int64_t result) noexcept
{
    if (!valid())
        return false;
    std::lock_guard lock(state_->mutex);
    auto* entry = state_->find(command);
    auto* slot = state_->find(command.key);
    if (entry == nullptr || !state_->owner(turn) || turn.key != command.key
        || !state_->accepting(*slot) || entry->state != CommandState::issued)
        return false;
    if (entry->deadline && now >= *entry->deadline) {
        entry->state = CommandState::cancelled;
        state_->signal(*slot);
        return false;
    }
    entry->state = CommandState::completed;
    entry->result = result;
    state_->signal(*slot);
    return true;
}
bool ConnectionAffinity::wait(Turn turn, Command command) noexcept
{
    if (!valid())
        return false;
    std::lock_guard lock(state_->mutex);
    auto* entry = state_->find(command);
    if (entry == nullptr || !state_->owner(turn) || turn.key != command.key
        || entry->state != CommandState::issued)
        return false;
    entry->state = CommandState::waiting;
    return true;
}
bool ConnectionAffinity::resume(Command command, std::uint64_t now) noexcept
{
    if (!valid())
        return false;
    std::lock_guard lock(state_->mutex);
    auto* entry = state_->find(command);
    auto* slot = state_->find(command.key);
    if (entry == nullptr || !state_->accepting(*slot)
        || entry->state != CommandState::waiting)
        return false;
    if (entry->deadline && now >= *entry->deadline) {
        entry->state = CommandState::cancelled;
        state_->signal(*slot);
        return false;
    }
    const auto ticket = state_->ticket(*slot, now);
    if (!ticket)
        return false;
    entry->event.ticket = *ticket;
    entry->event.published_at = now;
    entry->state = CommandState::queued;
    state_->ready(*slot);
    return true;
}
ConnectionAffinity::CommandState ConnectionAffinity::command_state(
    Command command) const noexcept
{
    if (!valid())
        return CommandState::stale;
    std::lock_guard lock(state_->mutex);
    const auto* entry = state_->find(command);
    return entry == nullptr ? CommandState::stale : entry->state;
}
std::optional<std::int64_t> ConnectionAffinity::take_result(
    Command command) noexcept
{
    if (!valid())
        return std::nullopt;
    std::lock_guard lock(state_->mutex);
    auto* entry = state_->find(command);
    if (entry == nullptr
        || (entry->state != CommandState::completed
            && entry->state != CommandState::cancelled))
        return std::nullopt;
    const auto result =
        entry->state == CommandState::completed ? entry->result : -1;
    entry->state = CommandState::stale;
    --state_->find(command.key)->view.commands;
    return result;
}

bool ConnectionAffinity::notify(Key key, Kind kind, std::uint64_t now) noexcept
{
    if (!valid() || (kind != Kind::send && kind != Kind::receive_release))
        return false;
    std::lock_guard lock(state_->mutex);
    auto* slot = state_->find(key);
    if (slot == nullptr || !state_->accepting(*slot))
        return false;
    auto& marker = slot->markers[kind == Kind::send ? 0 : 1];
    if (!marker.pending) {
        const auto ticket = state_->ticket(*slot, now);
        if (!ticket)
            return false;
        marker.event.kind = kind;
        marker.event.ticket = *ticket;
        marker.event.published_at = now;
        marker.pending = true;
    }
    state_->ready(*slot);
    return true;
}
std::optional<ConnectionAffinity::Timer> ConnectionAffinity::arm_timer(
    Key key, std::uint64_t deadline) noexcept
{
    if (!valid())
        return std::nullopt;
    std::lock_guard lock(state_->mutex);
    auto* slot = state_->find(key);
    if (slot == nullptr || !state_->accepting(*slot)
        || slot->timer == maximum_counter)
        return std::nullopt;
    ++slot->timer;
    slot->deadline = deadline;
    slot->timer_armed = true;
    slot->timer_published = slot->timer_issued = false;
    slot->markers[2].pending = false;
    state_->signal(*slot);
    return Timer {key, slot->timer};
}
bool ConnectionAffinity::cancel_timer(Timer timer) noexcept
{
    if (!valid())
        return false;
    std::lock_guard lock(state_->mutex);
    auto* slot = state_->find(timer.key);
    if (slot == nullptr || timer.serial != slot->timer || !slot->timer_armed)
        return false;
    slot->timer_armed = slot->timer_published = slot->timer_issued = false;
    slot->markers[2].pending = false;
    state_->signal(*slot);
    return true;
}
bool ConnectionAffinity::publish_timer(Timer timer, std::uint64_t now) noexcept
{
    if (!valid())
        return false;
    std::lock_guard lock(state_->mutex);
    auto* slot = state_->find(timer.key);
    if (slot == nullptr || !state_->accepting(*slot)
        || timer.serial != slot->timer || !slot->timer_armed
        || now < slot->deadline)
        return false;
    if (slot->timer_published)
        return true;
    const auto ticket = state_->ticket(*slot, now);
    if (!ticket)
        return false;
    auto& marker = slot->markers[2];
    marker.event.kind = Kind::timer;
    marker.event.ticket = *ticket;
    marker.event.published_at = now;
    marker.event.timer = timer;
    marker.pending = slot->timer_published = true;
    state_->ready(*slot);
    return true;
}
bool ConnectionAffinity::commit_timer(
    Turn turn, Timer timer, std::uint64_t now) noexcept
{
    if (!valid())
        return false;
    std::lock_guard lock(state_->mutex);
    auto* slot = state_->find(timer.key);
    if (slot == nullptr || !state_->owner(turn) || turn.key != timer.key
        || !state_->accepting(*slot) || timer.serial != slot->timer
        || !slot->timer_armed || !slot->timer_issued || now < slot->deadline)
        return false;
    slot->timer_armed = slot->timer_published = slot->timer_issued = false;
    return true;
}

void ConnectionAffinity::observe_wake(
    std::size_t shard_index, WakeResult result, std::uint64_t now) noexcept
{
    if (!valid() || shard_index >= configuration_.shards)
        return;
    std::lock_guard lock(state_->mutex);
    auto& shard = state_->shards[shard_index];
    if (result == WakeResult::stopped) {
        shard.enabled = false;
        for (auto& slot : state_->slots)
            if (slot.used && slot.view.shard == shard_index)
                state_->close(slot, now);
    } else if (shard.size != 0) {
        shard.wake.doorbell = result == WakeResult::accepted;
        shard.wake.fallback = result == WakeResult::full;
        if (result == WakeResult::full)
            ++shard.wake.full_rejections;
    }
}
ConnectionAffinity::WakeSnapshot ConnectionAffinity::wake_snapshot(
    std::size_t shard) const noexcept
{
    if (!valid() || shard >= configuration_.shards)
        return {};
    std::lock_guard lock(state_->mutex);
    return state_->shards[shard].wake;
}
std::optional<ConnectionAffinity::Turn> ConnectionAffinity::begin_turn(
    std::size_t shard_index) noexcept
{
    if (!valid() || shard_index >= configuration_.shards)
        return std::nullopt;
    std::lock_guard lock(state_->mutex);
    auto& shard = state_->shards[shard_index];
    if (!shard.enabled || shard.size == 0
        || shard.active != maximum_connections)
        return std::nullopt;
    const auto index = shard.ready[0];
    for (std::size_t i = 1; i < shard.size; ++i)
        shard.ready[i - 1] = shard.ready[i];
    --shard.size;
    shard.wake.ready = shard.size;
    shard.wake.doorbell = false;
    shard.wake.fallback = shard.size != 0;
    auto& slot = state_->slots[index];
    if (slot.turn == maximum_counter) {
        state_->close(slot, 0);
        return std::nullopt;
    }
    shard.active = index;
    slot.view.owner_active = true;
    slot.view.phase = Phase::running;
    slot.consumed = 0;
    ++slot.view.turns;
    return Turn {slot.key, ++slot.turn};
}
std::optional<ConnectionAffinity::Event> ConnectionAffinity::next(
    Turn turn, std::uint64_t now) noexcept
{
    if (!valid())
        return std::nullopt;
    std::lock_guard lock(state_->mutex);
    if (!state_->owner(turn))
        return std::nullopt;
    auto& slot = *state_->find(turn.key);
    if (!state_->accepting(slot) || slot.consumed >= turn_budget)
        return std::nullopt;
    // Expire before selection; cancelled commands never enter owner effects.
    for (auto& command : slot.commands)
        if (command.state == CommandState::queued && command.deadline
            && now >= *command.deadline)
            command.state = CommandState::cancelled;
    Event* selected =
        slot.view.datagrams ? &slot.datagrams[slot.head] : nullptr;
    State::CommandEntry* selected_command = nullptr;
    State::Marker* selected_marker = nullptr;
    for (auto& command : slot.commands)
        if (command.state == CommandState::queued
            && (selected == nullptr
                || command.event.ticket < selected->ticket)) {
            selected = &command.event;
            selected_command = &command;
        }
    for (auto& marker : slot.markers)
        if (marker.pending
            && (selected == nullptr
                || marker.event.ticket < selected->ticket)) {
            selected = &marker.event;
            selected_command = nullptr;
            selected_marker = &marker;
        }
    if (selected == nullptr) {
        state_->signal(slot);
        return std::nullopt;
    }
    const Event event = *selected;
    if (selected_marker != nullptr) {
        selected_marker->pending = false;
        if (event.kind == Kind::timer)
            slot.timer_issued = true;
    } else if (selected_command != nullptr)
        selected_command->state = CommandState::issued;
    else {
        --slot.view.datagrams;
        if (event.kind == Kind::data)
            --slot.view.data;
        slot.head = (slot.head + 1) % datagram_capacity;
    }
    ++slot.consumed;
    if (now >= event.published_at)
        slot.view.max_queue_delay =
            std::max(slot.view.max_queue_delay, now - event.published_at);
    state_->signal(slot);
    return event;
}
bool ConnectionAffinity::finish_turn(Turn turn, std::uint64_t now) noexcept
{
    if (!valid())
        return false;
    std::lock_guard lock(state_->mutex);
    if (!state_->owner(turn))
        return false;
    auto& slot = *state_->find(turn.key);
    for (const auto& command : slot.commands)
        if (command.state == CommandState::issued)
            return false;
    if (slot.timer_issued)
        return false;
    slot.view.owner_active = false;
    state_->shards[slot.view.shard].active = maximum_connections;
    if (slot.view.phase == Phase::closing) {
        slot.quiescent_time = now;
        slot.view.close_quiescence_delay =
            now >= slot.close_time ? now - slot.close_time : 0;
    } else {
        slot.view.phase = Phase::idle;
        state_->ready(slot);
    }
    return true;
}

bool ConnectionAffinity::close(Key key, std::uint64_t now) noexcept
{
    if (!valid())
        return false;
    std::lock_guard lock(state_->mutex);
    auto* slot = state_->find(key);
    if (slot == nullptr || slot->close_started)
        return false;
    state_->close(*slot, now);
    return true;
}
std::optional<ConnectionAffinity::Drain> ConnectionAffinity::begin_drain(
    Key key) noexcept
{
    if (!valid())
        return std::nullopt;
    std::lock_guard lock(state_->mutex);
    auto* slot = state_->find(key);
    if (slot == nullptr || slot->view.phase != Phase::closing
        || slot->view.owner_active || slot->view.drain_active)
        return std::nullopt;
    slot->view.drain_active = true;
    return Drain {key, 1};
}
bool ConnectionAffinity::finish_drain(Drain drain, std::uint64_t now) noexcept
{
    if (!valid() || drain.serial != 1)
        return false;
    std::lock_guard lock(state_->mutex);
    auto* slot = state_->find(drain.key);
    if (slot == nullptr || !slot->view.drain_active
        || slot->view.phase != Phase::closing)
        return false;
    slot->view.drain_active = false;
    slot->view.phase = Phase::closed;
    slot->view.close_completion_delay =
        now >= slot->quiescent_time ? now - slot->quiescent_time : 0;
    state_->signal(*slot);
    return true;
}
bool ConnectionAffinity::release(Key key) noexcept
{
    if (!valid())
        return false;
    std::lock_guard lock(state_->mutex);
    auto* slot = state_->find(key);
    if (slot == nullptr || slot->view.phase != Phase::closed
        || slot->view.commands != 0)
        return false;
    slot->used = false;
    return true;
}
void ConnectionAffinity::stop(std::uint64_t now) noexcept
{
    if (!valid())
        return;
    std::lock_guard lock(state_->mutex);
    state_->stopped = true;
    for (auto& slot : state_->slots)
        if (slot.used)
            state_->close(slot, now);
}
bool ConnectionAffinity::restart() noexcept
{
    if (!valid())
        return false;
    std::lock_guard lock(state_->mutex);
    if (!state_->stopped || state_->runtime == maximum_counter)
        return false;
    for (const auto& slot : state_->slots)
        if (slot.used
            && (slot.view.phase != Phase::closed || slot.view.commands != 0))
            return false;
    for (auto& slot : state_->slots)
        slot.used = false;
    for (auto& shard : state_->shards)
        shard = State::Shard {};
    ++state_->runtime;
    state_->stopped = false;
    return true;
}
ConnectionAffinity::Snapshot ConnectionAffinity::snapshot(
    Key key) const noexcept
{
    if (!valid())
        return {};
    std::lock_guard lock(state_->mutex);
    const auto* slot = state_->find(key);
    return slot == nullptr ? Snapshot {} : slot->view;
}

std::optional<ChannelCredits::Lease> ChannelCredits::grant(
    std::uint64_t owner) noexcept
{
    std::lock_guard lock(mutex_);
    if (owner == 0 || stopped_ || serial_ == maximum_counter
        || snapshot_.available_attempts == 0)
        return std::nullopt;
    for (const auto& entry : entries_)
        if (entry.owner == owner)
            return std::nullopt;
    for (std::size_t index = 0; index < entries_.size(); ++index) {
        auto& entry = entries_[index];
        if (entry.owner != 0)
            continue;
        const auto attempts =
            std::min<std::size_t>(4, snapshot_.available_attempts);
        const auto bytes = attempts * ConnectionAffinity::maximum_bytes;
        if (bytes > snapshot_.available_bytes)
            return std::nullopt;
        entry = {owner, ++serial_, attempts, bytes};
        snapshot_.available_attempts -= attempts;
        snapshot_.available_bytes -= bytes;
        ++snapshot_.active;
        return Lease {generation_, serial_, index};
    }
    return std::nullopt;
}
bool ChannelCredits::attempt(Lease lease, std::size_t bytes) noexcept
{
    std::lock_guard lock(mutex_);
    if (stopped_ || lease.generation != generation_
        || lease.slot >= entries_.size() || bytes == 0
        || bytes > ConnectionAffinity::maximum_bytes)
        return false;
    auto& entry = entries_[lease.slot];
    if (entry.owner == 0 || entry.serial != lease.serial || entry.attempts == 0
        || entry.bytes < bytes)
        return false;
    --entry.attempts;
    entry.bytes -= bytes;
    ++snapshot_.attempts;
    snapshot_.bytes += bytes;
    return true;
}
bool ChannelCredits::release(Lease lease) noexcept
{
    std::lock_guard lock(mutex_);
    if (lease.generation != generation_ || lease.slot >= entries_.size())
        return false;
    auto& entry = entries_[lease.slot];
    if (entry.owner == 0 || entry.serial != lease.serial)
        return false;
    snapshot_.available_attempts += entry.attempts;
    snapshot_.available_bytes += entry.bytes;
    --snapshot_.active;
    entry = {};
    return true;
}
bool ChannelCredits::next_round() noexcept
{
    std::lock_guard lock(mutex_);
    if (stopped_ || snapshot_.active != 0 || generation_ == maximum_counter)
        return false;
    ++generation_;
    snapshot_ = Snapshot {};
    return true;
}
void ChannelCredits::stop() noexcept
{
    std::lock_guard lock(mutex_);
    stopped_ = true;
    for (auto& entry : entries_)
        entry = {};
    snapshot_.active = 0;
    if (generation_ != maximum_counter)
        ++generation_;
}
ChannelCredits::Snapshot ChannelCredits::snapshot() const noexcept
{
    std::lock_guard lock(mutex_);
    return snapshot_;
}

} // namespace robotweax::srt::compat::model
