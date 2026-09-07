#include "srt/srt.h"

#include "compat/connection.hpp"
#include "compat/epoll.hpp"
#include "compat/error_state.hpp"
#include "compat/file_io.hpp"
#include "compat/group_registry.hpp"
#include "compat/logging.hpp"
#include "compat/message_io.hpp"
#include "compat/platform_networking.hpp"
#include "compat/process_state.hpp"
#include "compat/socket_io.hpp"
#include "compat/socket_registry.hpp"
#include "compat/statistics.hpp"

#include <algorithm>
#include <chrono>
#include <limits>

namespace {

[[nodiscard]] bool stateful_api_available() noexcept
{
    if (!robotweax::srt::compat::stateful_process_available()) {
        robotweax::srt::compat::set_last_error(SRT_EINVOP);
        return false;
    }
    return true;
}

[[nodiscard]] SRTSOCKET create_socket() noexcept
{
    if (!stateful_api_available()) {
        return SRT_INVALID_SOCK;
    }
    const int system_error =
        robotweax::srt::compat::initialize_platform_networking();
    if (system_error != 0) {
        robotweax::srt::compat::set_last_error(SRT_ECONNSETUP, system_error);
        return SRT_INVALID_SOCK;
    }
    const SRTSOCKET socket = robotweax::srt::compat::runtime_create_socket();
    if (socket == SRT_INVALID_SOCK) {
        robotweax::srt::compat::set_last_error(SRT_ENOBUF);
    }
    return socket;
}

} // namespace

extern "C" {

const SRT_MSGCTRL srt_msgctrl_default = {
    0,
    SRT_MSGTTL_INF,
    0,
    0,
    0,
    SRT_SEQNO_NONE,
    SRT_MSGNO_NONE,
    nullptr,
    0,
};

const char* const srt_rejectreason_msg[] = {
    "Unknown or erroneous",
    "Error in system calls",
    "Peer rejected connection",
    "Resource allocation failure",
    "Rogue peer or incorrect parameters",
    "Listener's backlog exceeded",
    "Internal Program Error",
    "Socket is being closed",
    "Peer version too old",
    "Rendezvous-mode cookie collision",
    "Incorrect passphrase",
    "Password required or unexpected",
    "MessageAPI/StreamAPI collision",
    "Congestion controller type collision",
    "Packet Filter settings error",
    "Group settings collision",
    "Connection timeout",
#ifdef ENABLE_AEAD_API_PREVIEW
    "Conflicting cryptographic modes",
#endif
};

int srt_startup(void)
{
    if (!stateful_api_available()) {
        return SRT_ERROR;
    }
    const int system_error =
        robotweax::srt::compat::initialize_platform_networking();
    if (system_error != 0) {
        robotweax::srt::compat::set_last_error(SRT_ECONNSETUP, system_error);
        return SRT_ERROR;
    }
    robotweax::srt::compat::runtime_start();
    ROBOTWEAX_SRT_COMPAT_LOG(LOG_NOTICE, SRT_LOGFA_API_CTRL,
        ".N", "SRT.ac", "startup completed");
    return 0;
}

int srt_cleanup(void)
{
    if (!stateful_api_available()) {
        return SRT_ERROR;
    }
    ROBOTWEAX_SRT_COMPAT_LOG(LOG_NOTICE, SRT_LOGFA_API_CTRL,
        ".N", "SRT.ac", "cleanup requested");
    robotweax::srt::compat::runtime_cleanup();
    return 0;
}

SRTSOCKET srt_socket(int, int, int)
{
    const SRTSOCKET socket = create_socket();
    if (socket != SRT_INVALID_SOCK) {
        ROBOTWEAX_SRT_COMPAT_LOG(LOG_NOTICE, SRT_LOGFA_SOCKMGMT,
            ".N", "SRT.sm", "socket created");
    }
    return socket;
}

SRTSOCKET srt_create_socket(void)
{
    const SRTSOCKET socket = create_socket();
    if (socket != SRT_INVALID_SOCK) {
        ROBOTWEAX_SRT_COMPAT_LOG(LOG_NOTICE, SRT_LOGFA_SOCKMGMT,
            ".N", "SRT.sm", "socket created");
    }
    return socket;
}

int srt_bind(SRTSOCKET socket, const sockaddr* name, int name_size)
{
    if (!stateful_api_available()) {
        return SRT_ERROR;
    }
    const auto record =
        robotweax::srt::compat::SocketRegistry::instance().find(socket);
    return robotweax::srt::compat::bind_socket(
        record.get(), name, name_size);
}

int srt_bind_acquire(
    SRTSOCKET socket, UDPSOCKET native_socket)
{
    if (!stateful_api_available()) {
        return SRT_ERROR;
    }
    const auto record =
        robotweax::srt::compat::SocketRegistry::instance().find(socket);
    return robotweax::srt::compat::bind_acquired_socket(
        record.get(), native_socket);
}

int srt_listen(SRTSOCKET socket, int backlog)
{
    if (!stateful_api_available()) {
        return SRT_ERROR;
    }
    const auto record =
        robotweax::srt::compat::SocketRegistry::instance().find(socket);
    // Keep argument validation precedence and the recent-close diagnostic
    // without retaining a complete SocketRecord solely for this distinction.
    if (backlog > 0 && record == nullptr
        && robotweax::srt::compat::SocketRegistry::instance().state(socket)
            == SRTS_CLOSED) {
        robotweax::srt::compat::set_last_error(SRT_ESCLOSED);
        return SRT_ERROR;
    }
    return robotweax::srt::compat::listen_socket(record, backlog);
}

SRTSOCKET srt_accept(
    SRTSOCKET socket, sockaddr* name, int* name_size)
{
    if (!stateful_api_available()) {
        return SRT_INVALID_SOCK;
    }
    const auto record =
        robotweax::srt::compat::SocketRegistry::instance().find(socket);
    return robotweax::srt::compat::accept_socket(
        record, name, name_size);
}

int srt_listen_callback(
    SRTSOCKET listener,
    srt_listen_callback_fn* callback,
    void* opaque)
{
    if (!stateful_api_available()) {
        return SRT_ERROR;
    }
    const auto record =
        robotweax::srt::compat::SocketRegistry::instance().find(listener);
    return robotweax::srt::compat::set_listen_callback(
        record, callback, opaque);
}

int srt_connect_callback(
    SRTSOCKET caller,
    srt_connect_callback_fn* callback,
    void* opaque)
{
    if (!stateful_api_available()) {
        return SRT_ERROR;
    }
    if (robotweax::srt::compat::is_group_handle(caller)) {
        return robotweax::srt::compat::GroupRegistry::instance()
            .set_connect_callback(caller, callback, opaque);
    }
    const auto record =
        robotweax::srt::compat::SocketRegistry::instance().find(caller);
    return robotweax::srt::compat::set_connect_callback(
        record, callback, opaque);
}

int srt_connect(
    SRTSOCKET socket, const sockaddr* name, int name_size)
{
    if (!stateful_api_available()) {
        return SRT_ERROR;
    }
    if (robotweax::srt::compat::is_group_handle(socket)) {
        SRT_SOCKGROUPCONFIG endpoint =
            srt_prepare_endpoint(nullptr, name, name_size);
        if (endpoint.errorcode != SRT_SUCCESS) {
            robotweax::srt::compat::set_last_error(
                SRT_EINVPARAM);
            return SRT_ERROR;
        }
        return srt_connect_group(socket, &endpoint, 1);
    }
    const auto record =
        robotweax::srt::compat::SocketRegistry::instance().find(socket);
    return robotweax::srt::compat::connect_socket(
        record, name, name_size);
}

int srt_connect_debug(
    SRTSOCKET socket, const sockaddr* name, int name_size,
    int forced_initial_sequence)
{
    if (!stateful_api_available()) {
        return SRT_ERROR;
    }
    if (robotweax::srt::compat::is_group_handle(socket)) {
        // v1.5.7 groups always manage their shared ISN themselves.
        return srt_connect(socket, name, name_size);
    }
    const auto record =
        robotweax::srt::compat::SocketRegistry::instance().find(socket);
    return robotweax::srt::compat::connect_socket(
        record, name, name_size, forced_initial_sequence);
}

int srt_connect_bind(
    SRTSOCKET socket,
    const sockaddr* source,
    const sockaddr* target,
    int name_size)
{
    if (!stateful_api_available()) {
        return SRT_ERROR;
    }
    if (source == nullptr) {
        robotweax::srt::compat::set_last_error(
            SRT_EINVPARAM);
        return SRT_ERROR;
    }
    if (robotweax::srt::compat::is_group_handle(socket)) {
        SRT_SOCKGROUPCONFIG endpoint =
            srt_prepare_endpoint(source, target, name_size);
        if (endpoint.errorcode != SRT_SUCCESS) {
            robotweax::srt::compat::set_last_error(
                SRT_EINVPARAM);
            return SRT_ERROR;
        }
        return srt_connect_group(socket, &endpoint, 1);
    }
    const auto record =
        robotweax::srt::compat::SocketRegistry::instance().find(socket);
    return robotweax::srt::compat::connect_bound_socket(
        record, source, target, name_size);
}

int srt_rendezvous(
    SRTSOCKET socket,
    const sockaddr* local_name,
    int local_name_size,
    const sockaddr* remote_name,
    int remote_name_size)
{
    if (!stateful_api_available()) {
        return SRT_ERROR;
    }
    const bool enabled = true;
    if (srt_setsockopt(socket, 0, SRTO_RENDEZVOUS,
            &enabled, static_cast<int>(sizeof(enabled)))
        == SRT_ERROR) {
        return SRT_ERROR;
    }
    if (srt_bind(
            socket, local_name, local_name_size)
        == SRT_ERROR) {
        return SRT_ERROR;
    }
    return srt_connect(
        socket, remote_name, remote_name_size);
}

int srt_close(SRTSOCKET socket)
{
    if (!stateful_api_available()) {
        return SRT_ERROR;
    }
    /*
     * v1.5.7 treats close as idempotent, including unknown handles.  Closed
     * status is retained in bounded tombstone histories. Numeric handles
     * are not recycled, so stale IDs cannot alias a newly created socket.
     */
    if (robotweax::srt::compat::is_group_handle(socket)) {
        robotweax::srt::compat::GroupRegistry::instance().close(socket);
    } else {
        robotweax::srt::compat::SocketRegistry::instance().close(socket);
    }
    ROBOTWEAX_SRT_COMPAT_LOG(LOG_NOTICE, SRT_LOGFA_SOCKMGMT,
        ".N", "SRT.sm", "socket closed");
    return 0;
}

int srt_getsockname(SRTSOCKET socket, sockaddr* name, int* name_size)
{
    if (!stateful_api_available()) {
        return SRT_ERROR;
    }
    const auto record =
        robotweax::srt::compat::SocketRegistry::instance().find(socket);
    if (record == nullptr) {
        robotweax::srt::compat::set_last_error(SRT_EINVSOCK);
        return SRT_ERROR;
    }
    return robotweax::srt::compat::get_socket_name(
        *record, name, name_size);
}

int srt_getpeername(SRTSOCKET socket, sockaddr* name, int* name_size)
{
    if (!stateful_api_available()) {
        return SRT_ERROR;
    }
    const auto record =
        robotweax::srt::compat::SocketRegistry::instance().find(socket);
    if (record == nullptr) {
        robotweax::srt::compat::set_last_error(SRT_EINVSOCK);
        return SRT_ERROR;
    }
    return robotweax::srt::compat::get_peer_name(
        *record, name, name_size);
}

SRT_SOCKSTATUS srt_getsockstate(SRTSOCKET socket)
{
    if (!stateful_api_available()) {
        return SRTS_NONEXIST;
    }
    if (robotweax::srt::compat::is_group_handle(socket)) {
        return robotweax::srt::compat::GroupRegistry::instance().state(socket);
    }
    return robotweax::srt::compat::SocketRegistry::instance().state(socket);
}

int srt_epoll_create(void)
{
    if (!stateful_api_available()) {
        return SRT_ERROR;
    }
    return robotweax::srt::compat::runtime_create_epoll();
}

int srt_epoll_clear_usocks(int eid)
{
    if (!stateful_api_available()) {
        return SRT_ERROR;
    }
    return robotweax::srt::compat::epoll_clear_usocks(eid);
}

int srt_epoll_add_usock(
    int eid, SRTSOCKET socket, const int* events)
{
    if (!stateful_api_available()) {
        return SRT_ERROR;
    }
    return robotweax::srt::compat::epoll_add_usock(eid, socket, events);
}

int srt_epoll_add_ssock(
    int eid, SYSSOCKET socket, const int* events)
{
    if (!stateful_api_available()) {
        return SRT_ERROR;
    }
    return robotweax::srt::compat::epoll_add_ssock(eid, socket, events);
}

int srt_epoll_remove_usock(int eid, SRTSOCKET socket)
{
    if (!stateful_api_available()) {
        return SRT_ERROR;
    }
    return robotweax::srt::compat::epoll_remove_usock(eid, socket);
}

int srt_epoll_remove_ssock(int eid, SYSSOCKET socket)
{
    if (!stateful_api_available()) {
        return SRT_ERROR;
    }
    return robotweax::srt::compat::epoll_remove_ssock(eid, socket);
}

int srt_epoll_update_usock(
    int eid, SRTSOCKET socket, const int* events)
{
    if (!stateful_api_available()) {
        return SRT_ERROR;
    }
    return robotweax::srt::compat::epoll_update_usock(
        eid, socket, events);
}

int srt_epoll_update_ssock(
    int eid, SYSSOCKET socket, const int* events)
{
    if (!stateful_api_available()) {
        return SRT_ERROR;
    }
    return robotweax::srt::compat::epoll_update_ssock(
        eid, socket, events);
}

int srt_epoll_wait(
    int eid,
    SRTSOCKET* readfds, int* read_count,
    SRTSOCKET* writefds, int* write_count,
    int64_t timeout_milliseconds,
    SYSSOCKET* system_readfds, int* system_read_count,
    SYSSOCKET* system_writefds, int* system_write_count)
{
    if (!stateful_api_available()) {
        return SRT_ERROR;
    }
    return robotweax::srt::compat::epoll_wait(eid,
        readfds, read_count, writefds, write_count,
        timeout_milliseconds,
        system_readfds, system_read_count,
        system_writefds, system_write_count);
}

int srt_epoll_uwait(
    int eid, SRT_EPOLL_EVENT* events, int event_count,
    int64_t timeout_milliseconds)
{
    if (!stateful_api_available()) {
        return SRT_ERROR;
    }
    return robotweax::srt::compat::epoll_uwait(
        eid, events, event_count, timeout_milliseconds);
}

int32_t srt_epoll_set(int eid, int32_t flags)
{
    if (!stateful_api_available()) {
        return SRT_ERROR;
    }
    return robotweax::srt::compat::epoll_set(eid, flags);
}

int srt_epoll_release(int eid)
{
    if (!stateful_api_available()) {
        return SRT_ERROR;
    }
    return robotweax::srt::compat::epoll_release(eid);
}

void srt_setloglevel(int level)
{
    robotweax::srt::compat::set_log_level(level);
}

void srt_addlogfa(int functional_area)
{
    robotweax::srt::compat::add_log_functional_area(
        functional_area);
}

void srt_dellogfa(int functional_area)
{
    robotweax::srt::compat::delete_log_functional_area(
        functional_area);
}

void srt_resetlogfa(
    const int* functional_areas, size_t functional_area_count)
{
    robotweax::srt::compat::reset_log_functional_areas(
        functional_areas, functional_area_count);
}

void srt_setloghandler(
    void* opaque, SRT_LOG_HANDLER_FN* handler)
{
    robotweax::srt::compat::set_log_handler(opaque, handler);
}

void srt_setlogflags(int flags)
{
    robotweax::srt::compat::set_log_flags(flags);
}

int srt_send(SRTSOCKET socket, const char* buffer, int length)
{
    return srt_sendmsg2(socket, buffer, length, nullptr);
}

int srt_sendmsg(
    SRTSOCKET socket, const char* buffer, int length,
    int ttl, int inorder)
{
    SRT_MSGCTRL control = srt_msgctrl_default;
    control.msgttl = ttl;
    control.inorder = inorder;
    return srt_sendmsg2(socket, buffer, length, &control);
}

int srt_sendmsg2(
    SRTSOCKET socket, const char* buffer, int length,
    SRT_MSGCTRL* control)
{
    if (!stateful_api_available()) {
        return SRT_ERROR;
    }
    if (robotweax::srt::compat::is_group_handle(socket)) {
        const auto group = robotweax::srt::compat::GroupRegistry::instance()
            .find(socket);
        return robotweax::srt::compat::send_group_message(
            group, buffer, length, control);
    }
    const auto record = robotweax::srt::compat::SocketRegistry::instance()
        .find(socket);
    return robotweax::srt::compat::send_message(
        record.get(), buffer, length, control);
}

int srt_recv(SRTSOCKET socket, char* buffer, int length)
{
    return srt_recvmsg2(socket, buffer, length, nullptr);
}

int srt_recvmsg(SRTSOCKET socket, char* buffer, int length)
{
    return srt_recvmsg2(socket, buffer, length, nullptr);
}

int srt_recvmsg2(
    SRTSOCKET socket, char* buffer, int length,
    SRT_MSGCTRL* control)
{
    if (!stateful_api_available()) {
        return SRT_ERROR;
    }
    if (robotweax::srt::compat::is_group_handle(socket)) {
        const auto group = robotweax::srt::compat::GroupRegistry::instance()
            .find(socket);
        return robotweax::srt::compat::receive_group_message(
            group, buffer, length, control);
    }
    const auto record = robotweax::srt::compat::SocketRegistry::instance()
        .find(socket);
    return robotweax::srt::compat::receive_message(
        record.get(), buffer, length, control);
}

int64_t srt_sendfile(
    SRTSOCKET socket, const char* path, int64_t* offset,
    int64_t size, int block)
{
    if (!stateful_api_available()) {
        return SRT_ERROR;
    }
    const auto record =
        robotweax::srt::compat::SocketRegistry::instance().find(socket);
    return robotweax::srt::compat::send_file(
        record.get(), path, offset, size, block);
}

int64_t srt_recvfile(
    SRTSOCKET socket, const char* path, int64_t* offset,
    int64_t size, int block)
{
    if (!stateful_api_available()) {
        return SRT_ERROR;
    }
    const auto record =
        robotweax::srt::compat::SocketRegistry::instance().find(socket);
    return robotweax::srt::compat::receive_file(
        record.get(), path, offset, size, block);
}

int srt_bstats(
    SRTSOCKET socket, SRT_TRACEBSTATS* statistics, int clear)
{
    return srt_bistats(socket, statistics, clear, 0);
}

int srt_bistats(
    SRTSOCKET socket, SRT_TRACEBSTATS* statistics,
    int clear, int instantaneous)
{
    if (!stateful_api_available()) {
        return SRT_ERROR;
    }
    if (statistics == nullptr) {
        robotweax::srt::compat::set_last_error(SRT_EINVPARAM);
        return SRT_ERROR;
    }
    const auto record =
        robotweax::srt::compat::SocketRegistry::instance().find(socket);
    if (record == nullptr) {
        robotweax::srt::compat::set_last_error(SRT_EINVSOCK);
        return SRT_ERROR;
    }

    std::shared_ptr<robotweax::srt::compat::ConnectionRuntime> runtime;
    {
        std::lock_guard lock(record->mutex);
        if (record->state == SRTS_CLOSED) {
            robotweax::srt::compat::set_last_error(SRT_EINVSOCK);
            return SRT_ERROR;
        }
        if (record->state == SRTS_BROKEN
            || record->state == SRTS_CLOSING) {
            robotweax::srt::compat::set_last_error(SRT_ECONNLOST);
            return SRT_ERROR;
        }
        if (record->state != SRTS_CONNECTED
            || record->runtime == nullptr) {
            robotweax::srt::compat::set_last_error(SRT_ENOCONN);
            return SRT_ERROR;
        }
        runtime = record->runtime;
    }

    robotweax::srt::compat::populate_trace_statistics(
        runtime->statistics(clear != 0, instantaneous != 0),
        *statistics);
    return 0;
}

int srt_getsndbuffer(
    SRTSOCKET socket, size_t* blocks, size_t* bytes)
{
    if (!stateful_api_available()) {
        return SRT_ERROR;
    }
    const auto record =
        robotweax::srt::compat::SocketRegistry::instance().find(socket);
    if (record == nullptr) {
        robotweax::srt::compat::set_last_error(SRT_EINVSOCK);
        return SRT_ERROR;
    }

    std::shared_ptr<robotweax::srt::compat::ConnectionRuntime> runtime;
    {
        std::lock_guard lock(record->mutex);
        if (record->state == SRTS_CLOSED) {
            robotweax::srt::compat::set_last_error(SRT_EINVSOCK);
            return SRT_ERROR;
        }
        runtime = record->runtime;
    }
    if (runtime == nullptr) {
        robotweax::srt::compat::set_last_error(SRT_ENOCONN);
        return SRT_ERROR;
    }

    const auto status = runtime->sender_buffer_status();
    if (blocks != nullptr) {
        *blocks = status.packets;
    }
    if (bytes != nullptr) {
        *bytes = status.bytes;
    }
    return static_cast<int>(std::min<std::uint64_t>(
        status.milliseconds,
        static_cast<std::uint64_t>(
            std::numeric_limits<int>::max())));
}

int srt_getsockopt(
    SRTSOCKET socket, int, SRT_SOCKOPT option, void* value, int* value_size)
{
    return srt_getsockflag(socket, option, value, value_size);
}

int srt_setsockopt(
    SRTSOCKET socket, int, SRT_SOCKOPT option,
    const void* value, int value_size)
{
    return srt_setsockflag(socket, option, value, value_size);
}

int srt_getsockflag(
    SRTSOCKET socket, SRT_SOCKOPT option, void* value, int* value_size)
{
    if (!stateful_api_available()) {
        return SRT_ERROR;
    }
    if (robotweax::srt::compat::is_group_handle(socket)) {
        return robotweax::srt::compat::GroupRegistry::instance()
            .get_io_option(socket, option, value, value_size);
    }
    const auto record =
        robotweax::srt::compat::SocketRegistry::instance().find(socket);
    if (record == nullptr) {
        robotweax::srt::compat::set_last_error(SRT_EINVSOCK);
        return SRT_ERROR;
    }
    return robotweax::srt::compat::get_socket_option(
        socket, *record, option, value, value_size);
}

int srt_setsockflag(
    SRTSOCKET socket, SRT_SOCKOPT option,
    const void* value, int value_size)
{
    if (!stateful_api_available()) {
        return SRT_ERROR;
    }
    if (robotweax::srt::compat::is_group_handle(socket)) {
        return robotweax::srt::compat::GroupRegistry::instance()
            .set_io_option(socket, option, value, value_size);
    }
    const auto record =
        robotweax::srt::compat::SocketRegistry::instance().find(socket);
    if (record == nullptr) {
        robotweax::srt::compat::set_last_error(SRT_EINVSOCK);
        return SRT_ERROR;
    }
    const int result = robotweax::srt::compat::set_socket_option(
        *record, option, value, value_size);
    if (result == 0) {
        std::shared_ptr<robotweax::srt::compat::ConnectionRuntime> runtime;
        robotweax::srt::SocketOptions options;
        {
            std::lock_guard lock(record->mutex);
            runtime = record->runtime;
            options = record->native_options;
        }
        if (runtime != nullptr) {
            runtime->apply_options(options);
        }
    }
    return result;
}

void srt_msgctrl_init(SRT_MSGCTRL* control)
{
    if (control != nullptr) {
        *control = srt_msgctrl_default;
    }
}

const char* srt_getlasterror_str(void)
{
    return robotweax::srt::compat::error_text(
        robotweax::srt::compat::last_error().code);
}

int srt_getlasterror(int* system_error)
{
    const auto error = robotweax::srt::compat::last_error();
    if (system_error != nullptr) {
        *system_error = error.system_error;
    }
    return error.code;
}

const char* srt_strerror(int code, int)
{
    return robotweax::srt::compat::error_text(code);
}

void srt_clearlasterror(void)
{
    robotweax::srt::compat::clear_last_error();
}

int srt_getrejectreason(SRTSOCKET socket)
{
    if (!stateful_api_available()) {
        return SRT_REJ_UNKNOWN;
    }
    const auto record =
        robotweax::srt::compat::SocketRegistry::instance().find(socket);
    if (record == nullptr) {
        return SRT_REJ_UNKNOWN;
    }
    std::lock_guard lock(record->mutex);
    return record->rejection_reason;
}

int srt_setrejectreason(SRTSOCKET socket, int value)
{
    if (!stateful_api_available()) {
        return SRT_ERROR;
    }
    const auto record =
        robotweax::srt::compat::SocketRegistry::instance().find(socket);
    if (record == nullptr) {
        robotweax::srt::compat::set_last_error(SRT_EINVSOCK);
        return SRT_ERROR;
    }
    if (value < SRT_REJC_PREDEFINED) {
        robotweax::srt::compat::set_last_error(SRT_EINVPARAM);
        return SRT_ERROR;
    }
    std::lock_guard lock(record->mutex);
    record->rejection_reason = value;
    return 0;
}

const char* srt_rejectreason_str(int id)
{
    if (id >= SRT_REJC_PREDEFINED) {
        return "Application-defined rejection reason";
    }
    if (id < 0 || id >= SRT_REJ_E_SIZE) {
        return srt_rejectreason_msg[SRT_REJ_UNKNOWN];
    }
    return srt_rejectreason_msg[id];
}

uint32_t srt_getversion(void)
{
    return SRT_VERSION_VALUE;
}

int64_t srt_time_now(void)
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::microseconds>(now).count();
}

int64_t srt_connection_time(SRTSOCKET socket)
{
    if (!stateful_api_available()
        || robotweax::srt::compat::is_group_handle(socket)) {
        robotweax::srt::compat::set_last_error(SRT_EINVSOCK);
        return -1;
    }
    const auto record =
        robotweax::srt::compat::SocketRegistry::instance().find(socket);
    if (record == nullptr) {
        robotweax::srt::compat::set_last_error(SRT_EINVSOCK);
        return -1;
    }
    std::lock_guard lock(record->mutex);
    if (record->state == SRTS_CLOSED) {
        robotweax::srt::compat::set_last_error(SRT_EINVSOCK);
        return -1;
    }
    return record->open_time_microseconds;
}

int srt_clock_type(void)
{
    return SRT_SYNC_CLOCK_STDCXX_STEADY;
}

} // extern "C"
