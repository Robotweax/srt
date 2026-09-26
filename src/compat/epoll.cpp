#include "compat/epoll.hpp"

#include "compat/error_state.hpp"
#include "compat/group_registry.hpp"
#include "compat/readiness.hpp"
#include "compat/socket_registry.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#  include <winsock2.h>
#else
#  include <cerrno>
#  include <fcntl.h>
#  include <poll.h>
#endif

namespace robotweax::srt::compat {
namespace {

using Clock = ReadinessSignal::Clock;
constexpr int default_events =
    SRT_EPOLL_IN | SRT_EPOLL_OUT | SRT_EPOLL_ERR;
constexpr std::int64_t system_poll_interval_milliseconds = 10;

struct Subscription {
    int events = default_events;
    int edge_seen = 0;
    std::uint64_t update_seen = 0;
    std::uint64_t update_pending = 0;
    SRTSOCKET handle = SRT_INVALID_SOCK;
    SocketReadinessSnapshot cached;
    std::unique_ptr<ReadinessWatch> watch;
    Subscription* ready_previous = nullptr;
    Subscription* ready_next = nullptr;
    bool ready = false;
    int report = 0;
    std::optional<Clock::time_point> deadline;
    std::size_t heap_index = 0;
};

using SubjectReadiness = SocketReadinessSnapshot;

struct SystemEvent {
    SYSSOCKET socket{};
    int events = 0;
};

#if defined(_WIN32)
using SystemPollDescriptor = WSAPOLLFD;
#else
using SystemPollDescriptor = pollfd;
#endif

struct PollRecord {
    std::mutex mutex;
    // Declared before subscriptions: watches unlink before their observer dies.
    ReadinessObserver observer;
    std::vector<SRTSOCKET> changed_scratch;
    std::vector<Subscription*> deadlines;
    Subscription* ready_head = nullptr;
    std::uint64_t readiness_queries = 0;
    std::unordered_map<SRTSOCKET, Subscription> user_sockets;
    std::unordered_map<SYSSOCKET, int> system_sockets;
    std::vector<SRT_EPOLL_EVENT> user_ready_scratch;
    std::vector<std::pair<SRTSOCKET, SRT_SOCKSTATUS>>
        group_member_scratch;
    std::vector<SystemEvent> system_ready_scratch;
    std::vector<SystemPollDescriptor> system_poll_scratch;
    std::optional<Clock::time_point> next_user_wakeup;
    std::int32_t flags = 0;
    bool released = false;
};

class PollRegistry {
public:
    [[nodiscard]] static PollRegistry& instance() noexcept
    {
        static PollRegistry registry;
        return registry;
    }

    [[nodiscard]] int create() noexcept
    {
        try {
            auto record = std::make_shared<PollRecord>();
            std::lock_guard lock(mutex_);
            const int first_candidate = next_id_;
            do {
                const int candidate = next_id_;
                next_id_ = next_id_ == std::numeric_limits<int>::max()
                    ? 1 : next_id_ + 1;
                if (polls_.emplace(candidate, record).second) {
                    return candidate;
                }
            } while (next_id_ != first_candidate);
        } catch (...) {
            return SRT_ERROR;
        }
        return SRT_ERROR;
    }

    [[nodiscard]] std::shared_ptr<PollRecord> find(int eid) noexcept
    {
        std::lock_guard lock(mutex_);
        const auto found = polls_.find(eid);
        return found == polls_.end() ? nullptr : found->second;
    }

    [[nodiscard]] bool release(int eid) noexcept
    {
        std::shared_ptr<PollRecord> record;
        {
            std::lock_guard lock(mutex_);
            const auto found = polls_.find(eid);
            if (found == polls_.end()) {
                return false;
            }
            record = std::move(found->second);
            polls_.erase(found);
        }
        {
            std::lock_guard lock(record->mutex);
            record->released = true;
            record->user_sockets.clear();
            record->ready_head = nullptr;
            record->deadlines.clear();
            record->system_sockets.clear();
        }
        record->observer.wake();
        return true;
    }

    void clear() noexcept
    {
        std::unordered_map<int, std::shared_ptr<PollRecord>> records;
        {
            std::lock_guard lock(mutex_);
            records.swap(polls_);
        }
        for (const auto& entry : records) {
            std::lock_guard lock(entry.second->mutex);
            entry.second->released = true;
            entry.second->user_sockets.clear();
            entry.second->ready_head = nullptr;
            entry.second->deadlines.clear();
            entry.second->system_sockets.clear();
            entry.second->observer.wake();
        }
    }

private:
    std::mutex mutex_;
    std::unordered_map<int, std::shared_ptr<PollRecord>> polls_;
    int next_id_ = 1;
};

[[nodiscard]] int fail(SRT_ERRNO error, int system_error = 0) noexcept
{
    set_last_error(error, system_error);
    return SRT_ERROR;
}

[[nodiscard]] bool valid_event_mask(int events, bool system) noexcept
{
    const std::uint32_t value = static_cast<std::uint32_t>(events);
    const std::uint32_t allowed = static_cast<std::uint32_t>(
        SRT_EPOLL_EVENTTYPES)
        | static_cast<std::uint32_t>(SRT_EPOLL_ET);
    if ((value & ~allowed) != 0U) {
        return false;
    }
    if (system && (events & (SRT_EPOLL_UPDATE | SRT_EPOLL_ET)) != 0) {
        return false;
    }
    return (events & SRT_EPOLL_EVENTTYPES) != 0
        || (events & SRT_EPOLL_ET) == 0;
}

[[nodiscard]] int selected_events(const int* events) noexcept
{
    return events == nullptr ? default_events : *events;
}

[[nodiscard]] bool valid_system_socket(
    SYSSOCKET socket, int& system_error) noexcept
{
#if defined(_WIN32)
    int socket_type = 0;
    int size = static_cast<int>(sizeof(socket_type));
    const int result = getsockopt(socket, SOL_SOCKET, SO_TYPE,
        reinterpret_cast<char*>(&socket_type), &size);
    system_error = result == 0 ? 0 : WSAGetLastError();
    return result == 0;
#else
    errno = 0;
    const int result = fcntl(socket, F_GETFD);
    system_error = result >= 0 ? 0 : errno;
    return result >= 0;
#endif
}

[[nodiscard]] SubjectReadiness group_readiness(
    SRTSOCKET handle,
    std::vector<std::pair<SRTSOCKET, SRT_SOCKSTATUS>>& members)
{
    const auto group = GroupRegistry::instance().find(handle);
    if (group == nullptr) {
        const bool closed =
            GroupRegistry::instance().state(handle) == SRTS_CLOSED;
        SubjectReadiness readiness {};
        readiness.events = closed ? SRT_EPOLL_ERR : 0;
        readiness.exists = closed;
        return readiness;
    }

    SubjectReadiness readiness{};
    readiness.exists = true;
    bool opened = false;
    bool closed = false;
    members.clear();
    {
        std::lock_guard lock(group->mutex);
        members.reserve(group->members.size());
        for (const auto& member : group->members) {
            members.emplace_back(
                member.public_data.id,
                member.public_data.sockstate);
        }
    }
    for (const auto& member : members) {
        (void)SocketRegistry::instance().state(member.first);
    }
    {
        std::lock_guard lock(group->mutex);
        readiness.update_version = group->update_version;
        opened = group->opened;
        closed = group->closed;
        members.clear();
        for (const auto& member : group->members) {
            members.emplace_back(
                member.public_data.id,
                member.public_data.sockstate);
        }
    }

    if (closed) {
        readiness.events = SRT_EPOLL_ERR;
        return readiness;
    }

    bool connected = false;
    bool pending = false;
    for (const auto& member : members) {
        if (member.second == SRTS_CONNECTED) {
            connected = true;
            const auto member_readiness =
                socket_readiness(member.first);
            readiness.events |= member_readiness.events
                & (SRT_EPOLL_IN | SRT_EPOLL_OUT | SRT_EPOLL_ERR);
            if (member_readiness.read_wakeup.has_value()
                && (!readiness.read_wakeup.has_value()
                    || *member_readiness.read_wakeup
                        < *readiness.read_wakeup)) {
                readiness.read_wakeup =
                    member_readiness.read_wakeup;
            }
        } else if (member.second != SRTS_BROKEN
            && member.second != SRTS_CLOSING
            && member.second != SRTS_CLOSED
            && member.second != SRTS_NONEXIST) {
            pending = true;
        }
    }
    if (opened && !connected && !pending) {
        readiness.events =
            SRT_EPOLL_IN | SRT_EPOLL_OUT | SRT_EPOLL_ERR;
    }
    return readiness;
}

[[nodiscard]] SubjectReadiness subject_readiness(
    SRTSOCKET handle,
    std::vector<std::pair<SRTSOCKET, SRT_SOCKSTATUS>>& group_members)
{
    return is_group_handle(handle)
        ? group_readiness(handle, group_members)
        : socket_readiness(handle);
}

void remove_ready(PollRecord& record, Subscription& subscription) noexcept
{
    if (!subscription.ready)
        return;
    if (subscription.ready_next == &subscription)
        record.ready_head = nullptr;
    else {
        subscription.ready_previous->ready_next = subscription.ready_next;
        subscription.ready_next->ready_previous = subscription.ready_previous;
        if (record.ready_head == &subscription)
            record.ready_head = subscription.ready_next;
    }
    subscription.ready = false;
    subscription.ready_next = subscription.ready_previous = nullptr;
}
void append_ready(PollRecord& record, Subscription& subscription) noexcept
{
    if (subscription.ready)
        return;
    if (record.ready_head == nullptr) {
        record.ready_head = &subscription;
        subscription.ready_previous = subscription.ready_next = &subscription;
    } else {
        auto* tail = record.ready_head->ready_previous;
        subscription.ready_previous = tail;
        subscription.ready_next = record.ready_head;
        tail->ready_next = &subscription;
        record.ready_head->ready_previous = &subscription;
    }
    subscription.ready = true;
}
bool earlier(Subscription* left, Subscription* right) noexcept
{
    return *left->deadline < *right->deadline;
}
void heap_swap(PollRecord& record, std::size_t left, std::size_t right) noexcept
{
    std::swap(record.deadlines[left], record.deadlines[right]);
    record.deadlines[left]->heap_index = left;
    record.deadlines[right]->heap_index = right;
}
void repair_heap(PollRecord& record, std::size_t index) noexcept
{
    while (index != 0U
        && earlier(
            record.deadlines[index], record.deadlines[(index - 1) / 2])) {
        const auto parent = (index - 1) / 2;
        heap_swap(record, index, parent);
        index = parent;
    }
    for (;;) {
        const auto left = index * 2 + 1;
        if (left >= record.deadlines.size())
            break;
        auto child = left;
        if (left + 1 < record.deadlines.size()
            && earlier(record.deadlines[left + 1], record.deadlines[left]))
            child = left + 1;
        if (!earlier(record.deadlines[child], record.deadlines[index]))
            break;
        heap_swap(record, index, child);
        index = child;
    }
}
void remove_deadline(PollRecord& record, Subscription& subscription) noexcept
{
    if (!subscription.deadline.has_value())
        return;
    const auto index = subscription.heap_index;
    heap_swap(record, index, record.deadlines.size() - 1);
    record.deadlines.pop_back();
    subscription.deadline.reset();
    if (index < record.deadlines.size())
        repair_heap(record, index);
}
void update_report(PollRecord& record, Subscription& subscription) noexcept
{
    const auto& readiness = subscription.cached;
    const int watched =
        subscription.events & (SRT_EPOLL_IN | SRT_EPOLL_OUT | SRT_EPOLL_ERR);
    const int current =
        (readiness.exists ? readiness.events : SRT_EPOLL_ERR) & watched;
    const bool edge = (subscription.events & SRT_EPOLL_ET) != 0;
    subscription.report = edge ? current & ~subscription.edge_seen : current;
    if (edge)
        subscription.edge_seen &= current;
    if ((subscription.events & SRT_EPOLL_UPDATE) != 0
        && readiness.update_version != subscription.update_seen) {
        subscription.update_pending = readiness.update_version;
        subscription.report |= SRT_EPOLL_UPDATE;
    }
    if (subscription.report != 0)
        append_ready(record, subscription);
    else
        remove_ready(record, subscription);
}
void refresh_subscription(PollRecord& record, Subscription& subscription)
{
    ++record.readiness_queries;
    auto readiness =
        subject_readiness(subscription.handle, record.group_member_scratch);
    if (subscription.watch->bind(readiness.source)) {
        // Subscribe before the final query: a change between the initial query
        // and attachment must not leave a cached state without a notification.
        ++record.readiness_queries;
        readiness =
            subject_readiness(subscription.handle, record.group_member_scratch);
    }
    subscription.cached = std::move(readiness);
    update_report(record, subscription);
    remove_deadline(record, subscription);
    if ((subscription.events & SRT_EPOLL_IN) != 0
        && (subscription.cached.events & SRT_EPOLL_IN) == 0
        && subscription.cached.read_wakeup.has_value()) {
        subscription.deadline = subscription.cached.read_wakeup;
        subscription.heap_index = record.deadlines.size();
        record.deadlines.push_back(&subscription);
        repair_heap(record, subscription.heap_index);
    }
}
void erase_subscription(PollRecord& record, SRTSOCKET socket) noexcept
{
    const auto found = record.user_sockets.find(socket);
    if (found == record.user_sockets.end())
        return;
    remove_ready(record, found->second);
    remove_deadline(record, found->second);
    record.user_sockets.erase(found);
}
void set_subscription(
    PollRecord& record, SRTSOCKET socket, int events, std::uint64_t version)
{
    if (events == 0) {
        erase_subscription(record, socket);
        return;
    }
    const auto capacity = record.user_sockets.size() + 1;
    const auto reserve = [](auto& values, std::size_t needed) {
        if (values.capacity() < needed)
            values.reserve(std::max(needed, values.capacity() * 2));
    };
    reserve(record.changed_scratch, capacity * 2);
    reserve(record.user_ready_scratch, capacity);
    reserve(record.deadlines, capacity);
    Subscription replacement;
    replacement.events = events;
    replacement.update_seen = replacement.update_pending = version;
    replacement.handle = socket;
    replacement.watch = std::make_unique<ReadinessWatch>(
        record.observer, socket, is_group_handle(socket));
    auto [entry, inserted] = record.user_sockets.try_emplace(socket);
    (void)inserted;
    remove_ready(record, entry->second);
    remove_deadline(record, entry->second);
    entry->second = std::move(replacement);
}
void collect_user_events(PollRecord& record,
    std::size_t maximum = std::numeric_limits<std::size_t>::max())
{
    const bool all = record.observer.take_changes(record.changed_scratch);
    if (all) {
        for (auto& entry : record.user_sockets)
            refresh_subscription(record, entry.second);
    } else {
        const auto now = Clock::now();
        // Capture due entries before refreshing: an overdue timestamp returned
        // by a concurrent transition must not spin inside this collection.
        while (!record.deadlines.empty()
            && *record.deadlines.front()->deadline <= now) {
            auto* subscription = record.deadlines.front();
            record.changed_scratch.push_back(subscription->handle);
            remove_deadline(record, *subscription);
        }
        for (auto handle : record.changed_scratch) {
            const auto entry = record.user_sockets.find(handle);
            if (entry != record.user_sockets.end())
                refresh_subscription(record, entry->second);
        }
    }
    record.next_user_wakeup = record.deadlines.empty()
        ? std::nullopt
        : record.deadlines.front()->deadline;
    auto& ready = record.user_ready_scratch;
    ready.clear();
    if (auto* entry = record.ready_head; entry != nullptr && maximum != 0U) {
        do {
            ready.emplace_back(entry->handle, entry->report);
            entry = entry->ready_next;
        } while (entry != record.ready_head && ready.size() < maximum);
    }
}
void consume_event(PollRecord& record, const SRT_EPOLL_EVENT& event,
    int consumed, bool consume_updates) noexcept
{
    auto& subscription = record.user_sockets.find(event.fd)->second;
    if ((subscription.events & SRT_EPOLL_ET) != 0) {
        subscription.edge_seen |=
            consumed & (SRT_EPOLL_IN | SRT_EPOLL_OUT | SRT_EPOLL_ERR);
    }
    if (consume_updates && (consumed & SRT_EPOLL_UPDATE) != 0) {
        subscription.update_seen = subscription.update_pending;
    }
    // Delivered level readiness goes to the tail; output truncation cannot
    // indefinitely favor one handle. Undelivered edges remain pending.
    remove_ready(record, subscription);
    update_report(record, subscription);
}

void collect_system_events(PollRecord& record)
{
    auto& ready = record.system_ready_scratch;
    ready.clear();
    if (record.system_sockets.empty()) {
        return;
    }

    ready.reserve(record.system_sockets.size());
    auto& descriptors = record.system_poll_scratch;
    descriptors.clear();
    descriptors.reserve(record.system_sockets.size());
    for (const auto& entry : record.system_sockets) {
#if defined(_WIN32)
        WSAPOLLFD descriptor{};
#else
        pollfd descriptor{};
#endif
        descriptor.fd = entry.first;
        if ((entry.second & SRT_EPOLL_IN) != 0) {
            descriptor.events |= POLLIN;
        }
        if ((entry.second & SRT_EPOLL_OUT) != 0) {
            descriptor.events |= POLLOUT;
        }
        descriptors.push_back(descriptor);
    }

#if defined(_WIN32)
    const int result = WSAPoll(
        descriptors.data(), static_cast<ULONG>(descriptors.size()), 0);
#else
    const int result = ::poll(
        descriptors.data(),
        static_cast<nfds_t>(descriptors.size()), 0);
#endif
    if (result <= 0) {
        return;
    }
    for (const auto& descriptor : descriptors) {
        const auto subscription =
            record.system_sockets.find(descriptor.fd);
        if (subscription == record.system_sockets.end()) {
            continue;
        }
        int events = 0;
        if ((descriptor.revents & POLLIN) != 0
            && (subscription->second & SRT_EPOLL_IN) != 0) {
            events |= SRT_EPOLL_IN;
        }
        if ((descriptor.revents & POLLOUT) != 0
            && (subscription->second & SRT_EPOLL_OUT) != 0) {
            events |= SRT_EPOLL_OUT;
        }
        if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0
            && (subscription->second & SRT_EPOLL_ERR) != 0) {
            events |= SRT_EPOLL_ERR;
        }
        if (events != 0) {
            ready.push_back({descriptor.fd, events});
        }
    }
}

[[nodiscard]] bool valid_output(
    const void* values, const int* count) noexcept
{
    if (count == nullptr) {
        return values == nullptr;
    }
    return *count >= 0 && (*count == 0 || values != nullptr);
}

[[nodiscard]] Clock::time_point deadline_from(
    std::int64_t timeout_milliseconds) noexcept
{
    if (timeout_milliseconds < 0) {
        return Clock::time_point::max();
    }
    const auto maximum = std::chrono::duration_cast<
        std::chrono::milliseconds>(
        Clock::time_point::max() - Clock::now()).count();
    const auto bounded = std::min<std::int64_t>(timeout_milliseconds, maximum);
    return Clock::now() + std::chrono::milliseconds{bounded};
}

[[nodiscard]] std::shared_ptr<PollRecord> find_poll(int eid) noexcept
{
    return PollRegistry::instance().find(eid);
}

} // namespace

void epoll_initialize() noexcept
{
    // Poll cleanup and socket-registry teardown notify readiness waiters.
    // Construct the signal first so it is destroyed after the poll registry.
    (void)ReadinessSignal::generation();
    (void)PollRegistry::instance();
}

int epoll_create() noexcept
{
    epoll_initialize();
    const int eid = PollRegistry::instance().create();
    if (eid == SRT_ERROR) {
        return fail(SRT_ECONNSETUP);
    }
    return eid;
}

int epoll_clear_usocks(int eid) noexcept
{
    const auto record = find_poll(eid);
    if (record == nullptr) {
        return fail(SRT_EINVPOLLID);
    }
    {
        std::lock_guard lock(record->mutex);
        record->user_sockets.clear();
        record->ready_head = nullptr;
        record->deadlines.clear();
    }
    record->observer.wake();
    return 0;
}

int epoll_add_usock(
    int eid, SRTSOCKET socket, const int* events) noexcept
{
    const auto record = find_poll(eid);
    if (record == nullptr) {
        return fail(SRT_EINVPOLLID);
    }
    SubjectReadiness initial;
    std::vector<std::pair<SRTSOCKET, SRT_SOCKSTATUS>>
        group_members;
    try {
        initial = subject_readiness(socket, group_members);
    } catch (const std::bad_alloc&) {
        return fail(SRT_ENOBUF);
    } catch (...) {
        return fail(SRT_ESYSOBJ);
    }
    if (!initial.exists) {
        return fail(SRT_EINVSOCK);
    }
    const int requested = selected_events(events);
    if (!valid_event_mask(requested, false)) {
        return fail(SRT_EINVPARAM);
    }
    try {
        std::lock_guard lock(record->mutex);
        if (record->released) {
            return fail(SRT_EINVPOLLID);
        }
        const SubjectReadiness current =
            subject_readiness(socket, group_members);
        if (!current.exists) {
            return fail(SRT_EINVSOCK);
        }
        set_subscription(*record, socket, requested, current.update_version);
    } catch (...) {
        return fail(SRT_ENOBUF);
    }
    record->observer.wake();
    return 0;
}

int epoll_add_ssock(
    int eid, SYSSOCKET socket, const int* events) noexcept
{
    const auto record = find_poll(eid);
    if (record == nullptr) {
        return fail(SRT_EINVPOLLID);
    }
    const int requested = selected_events(events);
    if (!valid_event_mask(requested, true)) {
        return fail(SRT_EINVPARAM);
    }
    int system_error = 0;
    if (!valid_system_socket(socket, system_error)) {
        return fail(SRT_ECONNSETUP, system_error);
    }
    try {
        std::lock_guard lock(record->mutex);
        if (record->released) {
            return fail(SRT_EINVPOLLID);
        }
        record->system_sockets[socket] = requested;
    } catch (...) {
        return fail(SRT_ENOBUF);
    }
    record->observer.wake();
    return 0;
}

int epoll_remove_usock(int eid, SRTSOCKET socket) noexcept
{
    const auto record = find_poll(eid);
    if (record == nullptr) {
        return fail(SRT_EINVPOLLID);
    }
    {
        std::lock_guard lock(record->mutex);
        if (record->released) {
            return fail(SRT_EINVPOLLID);
        }
        erase_subscription(*record, socket);
    }
    record->observer.wake();
    return 0;
}

int epoll_remove_ssock(int eid, SYSSOCKET socket) noexcept
{
    const auto record = find_poll(eid);
    if (record == nullptr) {
        return fail(SRT_EINVPOLLID);
    }
    {
        std::lock_guard lock(record->mutex);
        if (record->released) {
            return fail(SRT_EINVPOLLID);
        }
        record->system_sockets.erase(socket);
    }
    record->observer.wake();
    return 0;
}

int epoll_update_usock(
    int eid, SRTSOCKET socket, const int* events) noexcept
{
    const auto record = find_poll(eid);
    if (record == nullptr) {
        return fail(SRT_EINVPOLLID);
    }
    const int requested = selected_events(events);
    if (!valid_event_mask(requested, false)) {
        return fail(SRT_EINVPARAM);
    }
    try {
        std::vector<std::pair<SRTSOCKET, SRT_SOCKSTATUS>>
            group_members;
        std::lock_guard lock(record->mutex);
        if (record->released) {
            return fail(SRT_EINVPOLLID);
        }
        const SubjectReadiness current =
            subject_readiness(socket, group_members);
        if (!current.exists) {
            return fail(SRT_EINVSOCK);
        }
        set_subscription(*record, socket, requested, current.update_version);
    } catch (const std::bad_alloc&) {
        return fail(SRT_ENOBUF);
    } catch (...) {
        return fail(SRT_ESYSOBJ);
    }
    record->observer.wake();
    return 0;
}

int epoll_update_ssock(
    int eid, SYSSOCKET socket, const int* events) noexcept
{
    const auto record = find_poll(eid);
    if (record == nullptr) {
        return fail(SRT_EINVPOLLID);
    }
    const int requested = selected_events(events);
    if (!valid_event_mask(requested, true)) {
        return fail(SRT_EINVPARAM);
    }
    int system_error = 0;
    if (!valid_system_socket(socket, system_error)) {
        return fail(SRT_ECONNSETUP, system_error);
    }
    {
        std::lock_guard lock(record->mutex);
        if (record->released) {
            return fail(SRT_EINVPOLLID);
        }
        const auto found = record->system_sockets.find(socket);
        if (found == record->system_sockets.end()) {
            return fail(SRT_EINVPARAM);
        }
        if (requested == 0) {
            record->system_sockets.erase(found);
        } else {
            found->second = requested;
        }
    }
    record->observer.wake();
    return 0;
}

int epoll_wait(
    int eid,
    SRTSOCKET* readfds, int* read_count,
    SRTSOCKET* writefds, int* write_count,
    std::int64_t timeout_milliseconds,
    SYSSOCKET* system_readfds, int* system_read_count,
    SYSSOCKET* system_writefds, int* system_write_count) noexcept
{
    if (timeout_milliseconds < -1
        || !valid_output(readfds, read_count)
        || !valid_output(writefds, write_count)
        || !valid_output(system_readfds, system_read_count)
        || !valid_output(system_writefds, system_write_count)
        || (timeout_milliseconds < 0
            && read_count == nullptr && write_count == nullptr
            && system_read_count == nullptr
            && system_write_count == nullptr)) {
        return fail(SRT_EINVPARAM);
    }
    const auto record = find_poll(eid);
    if (record == nullptr) {
        return fail(SRT_EINVPOLLID);
    }

    const int read_capacity = read_count == nullptr ? 0 : *read_count;
    const int write_capacity = write_count == nullptr ? 0 : *write_count;
    const int system_read_capacity =
        system_read_count == nullptr ? 0 : *system_read_count;
    const int system_write_capacity =
        system_write_count == nullptr ? 0 : *system_write_count;
    if (read_count != nullptr) {
        *read_count = 0;
    }
    if (write_count != nullptr) {
        *write_count = 0;
    }
    if (system_read_count != nullptr) {
        *system_read_count = 0;
    }
    if (system_write_count != nullptr) {
        *system_write_count = 0;
    }

    const Clock::time_point deadline =
        deadline_from(timeout_milliseconds);
    for (;;) {
        const std::uint64_t generation = record->observer.generation();
        bool has_system_sockets = false;
        std::optional<Clock::time_point> user_wakeup;
        {
            std::lock_guard lock(record->mutex);
            if (record->released) {
                return fail(SRT_EINVPOLLID);
            }
            const bool empty = record->user_sockets.empty()
                && record->system_sockets.empty();
            if (empty
                && (record->flags & SRT_EPOLL_ENABLE_EMPTY) == 0) {
                return fail(SRT_EPOLLEMPTY);
            }
            if ((record->flags & SRT_EPOLL_ENABLE_OUTPUTCHECK) != 0) {
                if ((!record->user_sockets.empty()
                        && (readfds == nullptr
                            || read_capacity == 0
                            || writefds == nullptr
                            || write_capacity == 0))
                    || (!record->system_sockets.empty()
                        && (system_readfds == nullptr
                            || system_read_capacity == 0
                            || system_writefds == nullptr
                            || system_write_capacity == 0))) {
                    return fail(SRT_EINVPARAM);
                }
            }

            try {
                collect_user_events(*record);
                collect_system_events(*record);
            } catch (const std::bad_alloc&) {
                record->observer.invalidate_all();
                return fail(SRT_ENOBUF);
            } catch (...) {
                record->observer.invalidate_all();
                return fail(SRT_ESYSOBJ);
            }
            const auto& user_ready =
                record->user_ready_scratch;
            const auto& system_ready =
                record->system_ready_scratch;
            has_system_sockets = !record->system_sockets.empty();
            user_wakeup = record->next_user_wakeup;
            int total = 0;
            for (const auto& event : user_ready) {
                if ((record->user_sockets.find(event.fd)->second.events
                        & SRT_EPOLL_ET)
                    != 0) {
                    consume_event(*record, event, event.events, false);
                }
            }
            for (const auto& event : user_ready) {
                int consumed = 0;
                if ((event.events & (SRT_EPOLL_IN | SRT_EPOLL_ERR)) != 0
                    && read_count != nullptr
                    && *read_count < read_capacity) {
                    consumed |= event.events & (SRT_EPOLL_IN | SRT_EPOLL_ERR);
                    readfds[*read_count] = event.fd;
                    ++*read_count;
                    ++total;
                }
                if ((event.events & (SRT_EPOLL_OUT | SRT_EPOLL_ERR)) != 0
                    && write_count != nullptr
                    && *write_count < write_capacity) {
                    consumed |= event.events & (SRT_EPOLL_OUT | SRT_EPOLL_ERR);
                    writefds[*write_count] = event.fd;
                    ++*write_count;
                    ++total;
                }
                if (consumed != 0)
                    consume_event(*record, event, consumed, false);
            }
            for (const auto& event : system_ready) {
                if ((event.events & (SRT_EPOLL_IN | SRT_EPOLL_ERR)) != 0
                    && system_read_count != nullptr
                    && *system_read_count < system_read_capacity) {
                    system_readfds[*system_read_count] = event.socket;
                    ++*system_read_count;
                    ++total;
                }
                if ((event.events & (SRT_EPOLL_OUT | SRT_EPOLL_ERR)) != 0
                    && system_write_count != nullptr
                    && *system_write_count < system_write_capacity) {
                    system_writefds[*system_write_count] = event.socket;
                    ++*system_write_count;
                    ++total;
                }
            }
            if (total != 0) {
                return total;
            }
        }

        const Clock::time_point now = Clock::now();
        if (timeout_milliseconds >= 0 && now >= deadline) {
            return fail(SRT_ETIMEOUT);
        }
        Clock::time_point wake_deadline = has_system_sockets
            ? std::min(deadline, now + std::chrono::milliseconds{
                system_poll_interval_milliseconds})
            : deadline;
        if (user_wakeup.has_value()) {
            wake_deadline = std::min(
                wake_deadline, *user_wakeup);
        }
        record->observer.wait_until(generation, wake_deadline);
    }
}

int epoll_uwait(
    int eid, SRT_EPOLL_EVENT* events, int event_count,
    std::int64_t timeout_milliseconds) noexcept
{
    if (timeout_milliseconds < -1 || event_count < 0
        || (event_count > 0 && events == nullptr)) {
        return fail(SRT_EINVPARAM);
    }
    const auto record = find_poll(eid);
    if (record == nullptr) {
        return fail(SRT_EINVPOLLID);
    }

    const Clock::time_point deadline =
        deadline_from(timeout_milliseconds);
    for (;;) {
        const std::uint64_t generation = record->observer.generation();
        std::optional<Clock::time_point> user_wakeup;
        {
            std::lock_guard lock(record->mutex);
            if (record->released) {
                return fail(SRT_EINVPOLLID);
            }
            if (!record->system_sockets.empty()) {
                return fail(SRT_EINVPARAM);
            }
            if (record->user_sockets.empty()
                && (record->flags & SRT_EPOLL_ENABLE_EMPTY) == 0) {
                return fail(SRT_EPOLLEMPTY);
            }
            if ((record->flags & SRT_EPOLL_ENABLE_OUTPUTCHECK) != 0
                && (events == nullptr || event_count == 0)) {
                return fail(SRT_EINVPARAM);
            }
            try {
                collect_user_events(
                    *record, static_cast<std::size_t>(event_count));
            } catch (const std::bad_alloc&) {
                record->observer.invalidate_all();
                return fail(SRT_ENOBUF);
            } catch (...) {
                record->observer.invalidate_all();
                return fail(SRT_ESYSOBJ);
            }
            const auto& ready = record->user_ready_scratch;
            user_wakeup = record->next_user_wakeup;
            if (!ready.empty() && event_count != 0) {
                const int written = std::min<int>(
                    event_count, static_cast<int>(ready.size()));
                std::copy_n(ready.begin(), written, events);
                for (std::size_t index = 0;
                    index < static_cast<std::size_t>(written); ++index) {
                    consume_event(
                        *record, ready[index], ready[index].events, true);
                }
                return written;
            }
        }

        const Clock::time_point now = Clock::now();
        if (timeout_milliseconds >= 0 && now >= deadline) {
            return 0;
        }
        const Clock::time_point wake_deadline =
            user_wakeup.has_value()
            ? std::min(deadline, *user_wakeup)
            : deadline;
        record->observer.wait_until(generation, wake_deadline);
    }
}

std::int32_t epoll_set(int eid, std::int32_t flags) noexcept
{
    const auto record = find_poll(eid);
    if (record == nullptr) {
        return fail(SRT_EINVPOLLID);
    }
    std::lock_guard lock(record->mutex);
    if (record->released) {
        return fail(SRT_EINVPOLLID);
    }
    const std::int32_t previous = record->flags;
    if (flags == -1) {
        return previous;
    }
    if (flags == 0) {
        record->flags = 0;
    } else {
        record->flags |= flags;
    }
    record->observer.wake();
    return previous;
}

std::uint64_t epoll_readiness_queries_for_testing(int eid) noexcept
{
    const auto record = find_poll(eid);
    if (record == nullptr)
        return 0;
    std::lock_guard lock(record->mutex);
    return record->readiness_queries;
}

int epoll_release(int eid) noexcept
{
    return PollRegistry::instance().release(eid)
        ? 0 : fail(SRT_EINVPOLLID);
}

void epoll_clear_all() noexcept
{
    PollRegistry::instance().clear();
}

} // namespace robotweax::srt::compat
