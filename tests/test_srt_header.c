#include <srt/srt.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

_Static_assert(sizeof(SRTSOCKET) == sizeof(int32_t), "SRTSOCKET must be 32-bit");
_Static_assert(SRTS_INIT == 1, "socket-state values are ABI");
_Static_assert(SRTO_REUSEADDR == 15, "reuse option value is ABI");
_Static_assert(SRTO_MAXBW == 16, "option values are ABI");
_Static_assert(MJ_UNKNOWN == -1, "major error values are ABI");
_Static_assert(MJ_SETUP == 1, "major error values are ABI");
_Static_assert(MJ_PEERERROR == 7, "major error values are ABI");
_Static_assert(MN_NONE == 0, "minor error values are ABI");
_Static_assert(MN_INVALMSGAPI == 9, "minor error values are ABI");
_Static_assert(MN_BUSYPORT == 15, "minor error values are ABI");
_Static_assert(MN_CONGESTION == 4, "minor error aliases are ABI");
_Static_assert(SRT_EINVSOCK == 5004, "error values are ABI");
_Static_assert(SRT_KM_S_BADCRYPTOMODE == 5, "key-state values are ABI");
_Static_assert(SRT_KM_S_E_SIZE == 6, "key-state enum bound is ABI");
_Static_assert(offsetof(SRT_MSGCTRL, srctime) == 16, "message control layout");
_Static_assert(SRT_EPOLL_OUT == 0x4, "epoll values are ABI");
_Static_assert(SRT_REJ_TIMEOUT == 16, "rejection values are ABI");
_Static_assert(SRT_REJC_PREDEFINED == 1000, "rejection categories are ABI");
_Static_assert(LOG_CRIT == 2, "logging levels use syslog values");
_Static_assert(LOG_DEBUG == 7, "logging levels use syslog values");
_Static_assert(SRT_LOGF_DISABLE_EOL == 8, "logging flags are ABI");
_Static_assert(SRT_LOGFA_API_CTRL == 11, "logging areas are ABI");
_Static_assert(SRT_LOGFA_EPOLL_API == 46, "logging areas are ABI");
_Static_assert(SRT_LOGFA_LASTNONE == 63, "logging area bound is ABI");
_Static_assert(sizeof(SRT_EPOLL_EVENT) == 8, "epoll event layout");
_Static_assert(
    offsetof(SRT_EPOLL_EVENT, events) == 4, "epoll event member layout");
_Static_assert(sizeof(SRT_TRACEBSTATS) == 496, "statistics ABI size");
_Static_assert(
    offsetof(SRT_TRACEBSTATS, byteSentTotal) == 80,
    "statistics total-byte offset");
_Static_assert(
    offsetof(SRT_TRACEBSTATS, usPktSndPeriod) == 304,
    "statistics instantaneous offset");
_Static_assert(
    offsetof(SRT_TRACEBSTATS, pktSentUniqueTotal) == 432,
    "statistics unique-packet offset");
_Static_assert(SRT_GTYPE_UNDEFINED == 0, "group type values are ABI");
_Static_assert(SRT_GTYPE_BROADCAST == 1, "group type values are ABI");
_Static_assert(SRT_GTYPE_BACKUP == 2, "group type values are ABI");
_Static_assert(SRT_GTYPE_E_END == 3, "group type values are ABI");
_Static_assert(SRT_GST_PENDING == 0, "member status values are ABI");
_Static_assert(SRT_GST_BROKEN == 3, "member status values are ABI");
_Static_assert(sizeof(SRT_SOCKGROUPDATA) == 160,
    "group data ABI size");
_Static_assert(offsetof(SRT_SOCKGROUPDATA, peeraddr) == 8,
    "group data peer offset");
_Static_assert(offsetof(SRT_SOCKGROUPDATA, weight) == 140,
    "group data weight offset");
_Static_assert(offsetof(SRT_SOCKGROUPDATA, token) == 152,
    "group data token offset");
_Static_assert(sizeof(SRT_SOCKGROUPCONFIG) == 288,
    "group endpoint ABI size");
_Static_assert(offsetof(SRT_SOCKGROUPCONFIG, srcaddr) == 8,
    "group endpoint source offset");
_Static_assert(offsetof(SRT_SOCKGROUPCONFIG, peeraddr) == 136,
    "group endpoint peer offset");
_Static_assert(offsetof(SRT_SOCKGROUPCONFIG, config) == 272,
    "group endpoint config offset");
_Static_assert(offsetof(SRT_SOCKGROUPCONFIG, token) == 284,
    "group endpoint token offset");

static int accept_listener_connection(
    void* opaque,
    SRTSOCKET socket,
    int handshake_version,
    const struct sockaddr* peer_address,
    const char* stream_id)
{
    (void)opaque;
    (void)socket;
    (void)handshake_version;
    (void)peer_address;
    (void)stream_id;
    return 0;
}

static void observe_connect_result(
    void* opaque,
    SRTSOCKET socket,
    int error_code,
    const struct sockaddr* peer_address,
    int token)
{
    (void)opaque;
    (void)socket;
    (void)error_code;
    (void)peer_address;
    (void)token;
}

static void observe_log(
    void* opaque,
    int level,
    const char* file,
    int line,
    const char* area,
    const char* message)
{
    (void)opaque;
    (void)level;
    (void)file;
    (void)line;
    (void)area;
    (void)message;
}

int main(void)
{
#if defined(_WIN32)
    if (SYSSOCKET_INVALID != INVALID_SOCKET) {
#else
    if (SYSSOCKET_INVALID != -1) {
#endif
        return 1;
    }
    enum CodeMajor major_error = MJ_SUCCESS;
    enum CodeMinor minor_error = MN_NONE;
    srt_listen_callback_fn* callback = accept_listener_connection;
    srt_connect_callback_fn* connect_callback = observe_connect_result;
    SRT_LOG_HANDLER_FN* log_handler = observe_log;
    int (*connect_debug_fn)(SRTSOCKET, const struct sockaddr*, int, int) =
        srt_connect_debug;
    int (*connect_bind_fn)(SRTSOCKET, const struct sockaddr*,
        const struct sockaddr*, int) = srt_connect_bind;
    (void)&srt_listen;
    (void)major_error;
    (void)minor_error;
    (void)&srt_listen_callback;
    (void)callback;
    (void)&srt_connect_callback;
    (void)connect_callback;
    (void)&srt_connect_debug;
    (void)&srt_connect_bind;
    (void)connect_debug_fn;
    (void)connect_bind_fn;
    (void)log_handler;
    (void)&srt_bind_acquire;
    (void)&srt_bind_peerof;
    (void)&srt_accept;
    (void)&srt_accept_bond;
    (void)&srt_connect;
    (void)&srt_rendezvous;
    (void)&srt_getpeername;
    (void)&srt_send;
    (void)&srt_sendmsg;
    (void)&srt_sendmsg2;
    (void)&srt_recv;
    (void)&srt_recvmsg;
    (void)&srt_recvmsg2;
    (void)&srt_sendfile;
    (void)&srt_recvfile;
    (void)&srt_bstats;
    (void)&srt_bistats;
    (void)&srt_getsndbuffer;
    (void)&srt_epoll_create;
    (void)&srt_epoll_clear_usocks;
    (void)&srt_epoll_add_usock;
    (void)&srt_epoll_add_ssock;
    (void)&srt_epoll_remove_usock;
    (void)&srt_epoll_remove_ssock;
    (void)&srt_epoll_update_usock;
    (void)&srt_epoll_update_ssock;
    (void)&srt_epoll_wait;
    (void)&srt_epoll_uwait;
    (void)&srt_epoll_set;
    (void)&srt_epoll_release;
    (void)&srt_getrejectreason;
    (void)&srt_setrejectreason;
    (void)&srt_rejectreason_str;
    (void)&srt_rejectreason_msg;
    (void)&srt_create_group;
    (void)&srt_groupof;
    (void)&srt_group_data;
    (void)&srt_create_config;
    (void)&srt_delete_config;
    (void)&srt_config_add;
    (void)&srt_prepare_endpoint;
    (void)&srt_connect_group;
    (void)&srt_connection_time;
    (void)&srt_setloglevel;
    (void)&srt_addlogfa;
    (void)&srt_dellogfa;
    (void)&srt_resetlogfa;
    (void)&srt_setloghandler;
    (void)&srt_setlogflags;

    if (srt_startup() != 0)
        return 1;

    SRTSOCKET socket = srt_create_socket();
    if (socket == SRT_INVALID_SOCK || srt_getsockstate(socket) != SRTS_INIT)
        return 2;

    SRTSOCKET group = srt_create_group(SRT_GTYPE_BROADCAST);
    if (group == SRT_INVALID_SOCK
        || (group & SRTGROUP_MASK) == 0
        || srt_getsockstate(group) != SRTS_BROKEN)
        return 15;
    if (srt_close(group) != 0 || srt_getsockstate(group) != SRTS_CLOSED)
        return 16;

    int64_t maximum_bandwidth = 250000;
    if (srt_setsockopt(socket, 0, SRTO_MAXBW,
            &maximum_bandwidth, (int)sizeof(maximum_bandwidth)) != 0)
        return 3;

    maximum_bandwidth = 0;
    int option_size = (int)sizeof(maximum_bandwidth);
    if (srt_getsockopt(socket, 0, SRTO_MAXBW,
            &maximum_bandwidth, &option_size) != 0)
        return 4;
    if (maximum_bandwidth != 250000 || option_size != (int)sizeof(int64_t))
        return 5;

    SRT_MSGCTRL control;
    srt_msgctrl_init(&control);
    if (control.msgttl != SRT_MSGTTL_INF || control.pktseq != SRT_SEQNO_NONE)
        return 6;
    if (srt_getrejectreason(socket) != SRT_REJ_UNKNOWN)
        return 12;
    if (srt_setrejectreason(socket, 1404) != 0
        || srt_getrejectreason(socket) != 1404)
        return 13;
    if (strcmp(srt_rejectreason_str(SRT_REJ_TIMEOUT),
            "Connection timeout") != 0)
        return 14;

    {
        struct sockaddr_in local = {0};
        int local_size = (int)sizeof(local);
        if (srt_getsockname(socket, (struct sockaddr*)&local, &local_size)
            != SRT_ERROR)
            return 7;
        if (srt_getlasterror(NULL) != SRT_ENOCONN)
            return 8;
    }

    if (srt_close(socket) != 0 || srt_getsockstate(socket) != SRTS_CLOSED)
        return 9;
    if (strcmp(srt_strerror(SRT_EINVSOCK, 0),
            "Operation not supported: Invalid socket ID") != 0)
        return 10;

    return srt_cleanup() == 0 ? 0 : 11;
}
