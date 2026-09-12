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
            record->system_sockets.clear();
        }
        ReadinessSignal::notify();
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
            entry.second->system_sockets.clear();
        }
        ReadinessSignal::notify();
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

void collect_user_events(PollRecord& record)
{
    auto& ready = record.user_ready_scratch;
    ready.clear();
    record.next_user_wakeup.reset();
    ready.reserve(record.user_sockets.size());
    for (auto& entry : record.user_sockets) {
        Subscription& subscription = entry.second;
        const SubjectReadiness readiness =
            subject_readiness(
                entry.first, record.group_member_scratch);
        const int watched = subscription.events
            & (SRT_EPOLL_IN | SRT_EPOLL_OUT | SRT_EPOLL_ERR);
        // An already registered handle remains terminal even after its
        // bounded CLOSED status history expires. Numeric IDs are not reused.
        const int current =
            (readiness.exists ? readiness.events : SRT_EPOLL_ERR) & watched;
        if ((watched & SRT_EPOLL_IN) != 0
            && (current & SRT_EPOLL_IN) == 0
            && readiness.read_wakeup.has_value()
            && (!record.next_user_wakeup.has_value()
                || *readiness.read_wakeup
                    < *record.next_user_wakeup)) {
            record.next_user_wakeup =
                readiness.read_wakeup;
        }
        const bool edge_triggered =
            (subscription.events & SRT_EPOLL_ET) != 0;
        int report = edge_triggered
            ? current & ~subscription.edge_seen
            : current;
        if (edge_triggered) {
            subscription.edge_seen &= current;
        }
        if ((subscription.events & SRT_EPOLL_UPDATE) != 0
            && readiness.update_version
                != subscription.update_seen) {
            subscription.update_pending =
                readiness.update_version;
            report |= SRT_EPOLL_UPDATE;
        }
        if (report != 0) {
            ready.emplace_back(entry.first, report);
        }
    }
    std::sort(ready.begin(), ready.end(),
        [](const SRT_EPOLL_EVENT& left,
            const SRT_EPOLL_EVENT& right) {
            return left.fd < right.fd;
        });
}

void consume_edge_events(
    PollRecord& record,
    const std::vector<SRT_EPOLL_EVENT>& events,
    std::size_t count, bool consume_updates) noexcept
{
    const std::size_t bounded = std::min(count, events.size());
    for (std::size_t index = 0; index < bounded; ++index) {
        const auto found =
            record.user_sockets.find(events[index].fd);
        if (found == record.user_sockets.end()) {
            continue;
        }
        if ((found->second.events & SRT_EPOLL_ET) != 0) {
            found->second.edge_seen |= events[index].events
                & (SRT_EPOLL_IN | SRT_EPOLL_OUT | SRT_EPOLL_ERR);
        }
        if (consume_updates
            && (events[index].events & SRT_EPOLL_UPDATE) != 0) {
            found->second.update_seen =
                found->second.update_pending;
        }
    }
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
    }
    ReadinessSignal::notify();
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
        if (requested == 0) {
            record->user_sockets.erase(socket);
        } else {
            record->user_sockets[socket] = {
                .events = requested,
                .update_seen = current.update_version,
                .update_pending = current.update_version};
        }
    } catch (...) {
        return fail(SRT_ENOBUF);
    }
    ReadinessSignal::notify();
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
    ReadinessSignal::notify();
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
        record->user_sockets.erase(socket);
    }
    ReadinessSignal::notify();
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
    ReadinessSignal::notify();
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
        if (requested == 0) {
            record->user_sockets.erase(socket);
        } else {
            record->user_sockets[socket] = {
                .events = requested,
                .update_seen = current.update_version,
                .update_pending = current.update_version};
        }
    } catch (const std::bad_alloc&) {
        return fail(SRT_ENOBUF);
    } catch (...) {
        return fail(SRT_ESYSOBJ);
    }
    ReadinessSignal::notify();
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
    ReadinessSignal::notify();
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
        const std::uint64_t generation =
            ReadinessSignal::generation();
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
                return fail(SRT_ENOBUF);
            } catch (...) {
                return fail(SRT_ESYSOBJ);
            }
            const auto& user_ready =
                record->user_ready_scratch;
            const auto& system_ready =
                record->system_ready_scratch;
            consume_edge_events(
                *record, user_ready, user_ready.size(), false);
            has_system_sockets = !record->system_sockets.empty();
            user_wakeup = record->next_user_wakeup;
            int total = 0;
            for (const auto& event : user_ready) {
                if ((event.events & (SRT_EPOLL_IN | SRT_EPOLL_ERR)) != 0
                    && read_count != nullptr
                    && *read_count < read_capacity) {
                    readfds[*read_count] = event.fd;
                    ++*read_count;
                    ++total;
                }
                if ((event.events & (SRT_EPOLL_OUT | SRT_EPOLL_ERR)) != 0
                    && write_count != nullptr
                    && *write_count < write_capacity) {
                    writefds[*write_count] = event.fd;
                    ++*write_count;
                    ++total;
                }
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
        ReadinessSignal::wait_until(generation, wake_deadline);
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
        const std::uint64_t generation =
            ReadinessSignal::generation();
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
                collect_user_events(*record);
            } catch (const std::bad_alloc&) {
                return fail(SRT_ENOBUF);
            } catch (...) {
                return fail(SRT_ESYSOBJ);
            }
            const auto& ready = record->user_ready_scratch;
            user_wakeup = record->next_user_wakeup;
            if (!ready.empty() && event_count != 0) {
                const int written = std::min<int>(
                    event_count, static_cast<int>(ready.size()));
                std::copy_n(ready.begin(), written, events);
                consume_edge_events(*record, ready,
                    static_cast<std::size_t>(written), true);
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
        ReadinessSignal::wait_until(
            generation, wake_deadline);
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
    ReadinessSignal::notify();
    return previous;
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
