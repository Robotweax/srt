/* SPDX-License-Identifier: MIT */

#ifndef ROBOTWEAX_SRT_COMPAT_SRT_H
#define ROBOTWEAX_SRT_COMPAT_SRT_H

#include "srt/logging_api.h"
#include "srt/version.h"

#include <stddef.h>
#include <stdint.h>

#if !defined(__cplusplus)
#  include <stdbool.h>
#endif

#if defined(_WIN32)
#  if !defined(NOMINMAX)
#    define NOMINMAX
#  endif
#  if !defined(WIN32_LEAN_AND_MEAN)
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#endif

#define SRT_VERSION_FEAT_HSv5 0x010300

#ifndef SRT_API
#  if defined(_WIN32)
#    if defined(SRT_DYNAMIC)
#      if defined(SRT_EXPORTS)
#        define SRT_API __declspec(dllexport)
#      else
#        define SRT_API __declspec(dllimport)
#      endif
#    else
#      define SRT_API
#    endif
#  elif defined(__GNUC__) || defined(__clang__)
#    define SRT_API __attribute__((visibility("default")))
#  else
#    define SRT_API
#  endif
#endif

#if defined(__cplusplus)
extern "C" {
#endif

typedef int32_t SRTSOCKET;

#if defined(_WIN32)
typedef SOCKET SYSSOCKET;
static const SYSSOCKET SYSSOCKET_INVALID = INVALID_SOCKET;
#else
typedef int SYSSOCKET;
static const int SYSSOCKET_INVALID = -1;
#endif
typedef SYSSOCKET UDPSOCKET;

static const int32_t SRTGROUP_MASK = (1 << 30);
static const SRTSOCKET SRT_INVALID_SOCK = -1;
static const int SRT_ERROR = -1;

typedef enum SRT_SOCKSTATUS {
    SRTS_INIT = 1,
    SRTS_OPENED,
    SRTS_LISTENING,
    SRTS_CONNECTING,
    SRTS_CONNECTED,
    SRTS_BROKEN,
    SRTS_CLOSING,
    SRTS_CLOSED,
    SRTS_NONEXIST
} SRT_SOCKSTATUS;

/*
 * Numeric values are part of the public ABI.  Options whose protocol feature
 * is not implemented remain declared so callers can detect a defined
 * SRT_EINVOP result instead of relying on private Robotweax identifiers.
 */
typedef enum SRT_SOCKOPT {
    SRTO_MSS = 0,
    SRTO_SNDSYN = 1,
    SRTO_RCVSYN = 2,
    SRTO_ISN = 3,
    SRTO_FC = 4,
    SRTO_SNDBUF = 5,
    SRTO_RCVBUF = 6,
    SRTO_LINGER = 7,
    SRTO_UDP_SNDBUF = 8,
    SRTO_UDP_RCVBUF = 9,
    SRTO_RENDEZVOUS = 12,
    SRTO_SNDTIMEO = 13,
    SRTO_RCVTIMEO = 14,
    SRTO_REUSEADDR = 15,
    SRTO_MAXBW = 16,
    SRTO_STATE = 17,
    SRTO_EVENT = 18,
    SRTO_SNDDATA = 19,
    SRTO_RCVDATA = 20,
    SRTO_SENDER = 21,
    SRTO_TSBPDMODE = 22,
    SRTO_LATENCY = 23,
    SRTO_INPUTBW = 24,
    SRTO_OHEADBW = 25,
    SRTO_PASSPHRASE = 26,
    SRTO_PBKEYLEN = 27,
    SRTO_KMSTATE = 28,
    SRTO_IPTTL = 29,
    SRTO_IPTOS = 30,
    SRTO_TLPKTDROP = 31,
    SRTO_SNDDROPDELAY = 32,
    SRTO_NAKREPORT = 33,
    SRTO_VERSION = 34,
    SRTO_PEERVERSION = 35,
    SRTO_CONNTIMEO = 36,
    SRTO_DRIFTTRACER = 37,
    SRTO_MININPUTBW = 38,
    SRTO_SNDKMSTATE = 40,
    SRTO_RCVKMSTATE = 41,
    SRTO_LOSSMAXTTL = 42,
    SRTO_RCVLATENCY = 43,
    SRTO_PEERLATENCY = 44,
    SRTO_MINVERSION = 45,
    SRTO_STREAMID = 46,
    SRTO_CONGESTION = 47,
    SRTO_MESSAGEAPI = 48,
    SRTO_PAYLOADSIZE = 49,
    SRTO_TRANSTYPE = 50,
    SRTO_KMREFRESHRATE = 51,
    SRTO_KMPREANNOUNCE = 52,
    SRTO_ENFORCEDENCRYPTION = 53,
    SRTO_IPV6ONLY = 54,
    SRTO_PEERIDLETIMEO = 55,
    SRTO_BINDTODEVICE = 56,
    SRTO_GROUPCONNECT = 57,
    SRTO_GROUPMINSTABLETIMEO = 58,
    SRTO_GROUPTYPE = 59,
    SRTO_PACKETFILTER = 60,
    SRTO_RETRANSMITALGO = 61,
    /** Local library release as int32_t; read-only, not a peer/wire version.
     * Requires a valid socket or group. Unsupported by older implementations.
     * This extension does not change SRTO_E_SIZE or compatibility options.
     */
    SRTO_ROBOTWEAX_VERSION = 0x01000001,
#ifdef ENABLE_AEAD_API_PREVIEW
    SRTO_CRYPTOMODE = 62,
    SRTO_E_SIZE = 63
#else
    SRTO_E_SIZE = 62
#endif
} SRT_SOCKOPT;

typedef enum SRT_TRANSTYPE {
    SRTT_LIVE = 0,
    SRTT_FILE = 1,
    SRTT_INVALID = 2
} SRT_TRANSTYPE;

typedef enum SRT_KM_STATE {
    SRT_KM_S_UNSECURED = 0,
    SRT_KM_S_SECURING = 1,
    SRT_KM_S_SECURED = 2,
    SRT_KM_S_NOSECRET = 3,
    SRT_KM_S_BADSECRET = 4,
    SRT_KM_S_BADCRYPTOMODE = 5,
    SRT_KM_S_E_SIZE = 6
} SRT_KM_STATE;

typedef enum SRT_EPOLL_OPT {
    SRT_EPOLL_OPT_NONE = 0x0,
    SRT_EPOLL_IN = 0x1,
    SRT_EPOLL_OUT = 0x4,
    SRT_EPOLL_ERR = 0x8,
    SRT_EPOLL_CONNECT = SRT_EPOLL_OUT,
    SRT_EPOLL_ACCEPT = SRT_EPOLL_IN,
    SRT_EPOLL_UPDATE = 0x10,
    SRT_EPOLL_ET = (-2147483647 - 1)
} SRT_EPOLL_OPT;

typedef int32_t SRT_EPOLL_T;

#define SRT_EPOLL_EVENTTYPES \
    (SRT_EPOLL_IN | SRT_EPOLL_OUT | SRT_EPOLL_UPDATE | SRT_EPOLL_ERR)
#define SRT_EPOLL_ETONLY SRT_EPOLL_UPDATE

typedef enum SRT_EPOLL_FLAGS {
    SRT_EPOLL_ENABLE_EMPTY = 1,
    SRT_EPOLL_ENABLE_OUTPUTCHECK = 2
} SRT_EPOLL_FLAGS;

typedef struct SRT_EPOLL_EVENT_STR {
    SRTSOCKET fd;
    int events;
#if defined(__cplusplus)
    SRT_EPOLL_EVENT_STR(SRTSOCKET socket, int event_flags)
        : fd(socket), events(event_flags) {}
    SRT_EPOLL_EVENT_STR() : fd(-1), events(0) {}
#endif
} SRT_EPOLL_EVENT;

static const int SRT_LIVE_DEF_PLSIZE = 1316;
static const int SRT_LIVE_MAX_PLSIZE = 1456;
static const int SRT_LIVE_DEF_LATENCY_MS = 120;

/*
 * Field order and native alignment are part of the libsrt 1.5.x ABI.
 * Append-only compatibility mirrors Haivision's CBytePerfMon layout.
 */
struct CBytePerfMon {
    int64_t msTimeStamp;
    int64_t pktSentTotal;
    int64_t pktRecvTotal;
    int pktSndLossTotal;
    int pktRcvLossTotal;
    int pktRetransTotal;
    int pktSentACKTotal;
    int pktRecvACKTotal;
    int pktSentNAKTotal;
    int pktRecvNAKTotal;
    int64_t usSndDurationTotal;
    int pktSndDropTotal;
    int pktRcvDropTotal;
    int pktRcvUndecryptTotal;
    uint64_t byteSentTotal;
    uint64_t byteRecvTotal;
    uint64_t byteRcvLossTotal;
    uint64_t byteRetransTotal;
    uint64_t byteSndDropTotal;
    uint64_t byteRcvDropTotal;
    uint64_t byteRcvUndecryptTotal;

    int64_t pktSent;
    int64_t pktRecv;
    int pktSndLoss;
    int pktRcvLoss;
    int pktRetrans;
    int pktRcvRetrans;
    int pktSentACK;
    int pktRecvACK;
    int pktSentNAK;
    int pktRecvNAK;
    double mbpsSendRate;
    double mbpsRecvRate;
    int64_t usSndDuration;
    int pktReorderDistance;
    double pktRcvAvgBelatedTime;
    int64_t pktRcvBelated;
    int pktSndDrop;
    int pktRcvDrop;
    int pktRcvUndecrypt;
    uint64_t byteSent;
    uint64_t byteRecv;
    uint64_t byteRcvLoss;
    uint64_t byteRetrans;
    uint64_t byteSndDrop;
    uint64_t byteRcvDrop;
    uint64_t byteRcvUndecrypt;

    double usPktSndPeriod;
    int pktFlowWindow;
    int pktCongestionWindow;
    int pktFlightSize;
    double msRTT;
    double mbpsBandwidth;
    int byteAvailSndBuf;
    int byteAvailRcvBuf;
    double mbpsMaxBW;
    int byteMSS;

    int pktSndBuf;
    int byteSndBuf;
    int msSndBuf;
    int msSndTsbPdDelay;
    int pktRcvBuf;
    int byteRcvBuf;
    int msRcvBuf;
    int msRcvTsbPdDelay;

    int pktSndFilterExtraTotal;
    int pktRcvFilterExtraTotal;
    int pktRcvFilterSupplyTotal;
    int pktRcvFilterLossTotal;
    int pktSndFilterExtra;
    int pktRcvFilterExtra;
    int pktRcvFilterSupply;
    int pktRcvFilterLoss;
    int pktReorderTolerance;

    int64_t pktSentUniqueTotal;
    int64_t pktRecvUniqueTotal;
    uint64_t byteSentUniqueTotal;
    uint64_t byteRecvUniqueTotal;
    int64_t pktSentUnique;
    int64_t pktRecvUnique;
    uint64_t byteSentUnique;
    uint64_t byteRecvUnique;
};

typedef struct CBytePerfMon SRT_TRACEBSTATS;

enum CodeMajor {
    MJ_UNKNOWN = -1,
    MJ_SUCCESS = 0,
    MJ_SETUP = 1,
    MJ_CONNECTION = 2,
    MJ_SYSTEMRES = 3,
    MJ_FILESYSTEM = 4,
    MJ_NOTSUP = 5,
    MJ_AGAIN = 6,
    MJ_PEERERROR = 7
};

enum CodeMinor {
    MN_NONE = 0,
    MN_TIMEOUT = 1,
    MN_REJECTED = 2,
    MN_NORES = 3,
    MN_SECURITY = 4,
    MN_CLOSED = 5,
    MN_CONNLOST = 1,
    MN_NOCONN = 2,
    MN_THREAD = 1,
    MN_MEMORY = 2,
    MN_OBJECT = 3,
    MN_SEEKGFAIL = 1,
    MN_READFAIL = 2,
    MN_SEEKPFAIL = 3,
    MN_WRITEFAIL = 4,
    MN_ISBOUND = 1,
    MN_ISCONNECTED = 2,
    MN_INVAL = 3,
    MN_SIDINVAL = 4,
    MN_ISUNBOUND = 5,
    MN_NOLISTEN = 6,
    MN_ISRENDEZVOUS = 7,
    MN_ISRENDUNBOUND = 8,
    MN_INVALMSGAPI = 9,
    MN_INVALBUFFERAPI = 10,
    MN_BUSY = 11,
    MN_XSIZE = 12,
    MN_EIDINVAL = 13,
    MN_EEMPTY = 14,
    MN_BUSYPORT = 15,
    MN_WRAVAIL = 1,
    MN_RDAVAIL = 2,
    MN_XMTIMEOUT = 3,
    MN_CONGESTION = 4
};

typedef enum SRT_ERRNO {
    SRT_EUNKNOWN = -1,
    SRT_SUCCESS = 0,

    SRT_ECONNSETUP = 1000,
    SRT_ENOSERVER = 1001,
    SRT_ECONNREJ = 1002,
    SRT_ESOCKFAIL = 1003,
    SRT_ESECFAIL = 1004,
    SRT_ESCLOSED = 1005,

    SRT_ECONNFAIL = 2000,
    SRT_ECONNLOST = 2001,
    SRT_ENOCONN = 2002,

    SRT_ERESOURCE = 3000,
    SRT_ETHREAD = 3001,
    SRT_ENOBUF = 3002,
    SRT_ESYSOBJ = 3003,

    SRT_EFILE = 4000,
    SRT_EINVRDOFF = 4001,
    SRT_ERDPERM = 4002,
    SRT_EINVWROFF = 4003,
    SRT_EWRPERM = 4004,

    SRT_EINVOP = 5000,
    SRT_EBOUNDSOCK = 5001,
    SRT_ECONNSOCK = 5002,
    SRT_EINVPARAM = 5003,
    SRT_EINVSOCK = 5004,
    SRT_EUNBOUNDSOCK = 5005,
    SRT_ENOLISTEN = 5006,
    SRT_ERDVNOSERV = 5007,
    SRT_ERDVUNBOUND = 5008,
    SRT_EINVALMSGAPI = 5009,
    SRT_EINVALBUFFERAPI = 5010,
    SRT_EDUPLISTEN = 5011,
    SRT_ELARGEMSG = 5012,
    SRT_EINVPOLLID = 5013,
    SRT_EPOLLEMPTY = 5014,
    SRT_EBINDCONFLICT = 5015,

    SRT_EASYNCFAIL = 6000,
    SRT_EASYNCSND = 6001,
    SRT_EASYNCRCV = 6002,
    SRT_ETIMEOUT = 6003,
    SRT_ECONGEST = 6004,

    SRT_EPEERERR = 7000
} SRT_ERRNO;

typedef enum SRT_REJECT_REASON {
    SRT_REJ_UNKNOWN = 0,
    SRT_REJ_SYSTEM,
    SRT_REJ_PEER,
    SRT_REJ_RESOURCE,
    SRT_REJ_ROGUE,
    SRT_REJ_BACKLOG,
    SRT_REJ_IPE,
    SRT_REJ_CLOSE,
    SRT_REJ_VERSION,
    SRT_REJ_RDVCOOKIE,
    SRT_REJ_BADSECRET,
    SRT_REJ_UNSECURE,
    SRT_REJ_MESSAGEAPI,
    SRT_REJ_CONGESTION,
    SRT_REJ_FILTER,
    SRT_REJ_GROUP,
    SRT_REJ_TIMEOUT,
#ifdef ENABLE_AEAD_API_PREVIEW
    SRT_REJ_CRYPTO,
#endif
    SRT_REJ_E_SIZE
} SRT_REJECT_REASON;

#define SRT_REJ__SIZE SRT_REJ_E_SIZE
#define SRT_REJC_VALUE(code) (1000 * ((code) / 1000))
#define SRT_REJC_INTERNAL 0
#define SRT_REJC_PREDEFINED 1000
#define SRT_REJC_USERDEFINED 2000

/* Logger functional-area values are part of the v1.5.7 source API. */
#define SRT_LOGFA_GENERAL 0
#define SRT_LOGFA_SOCKMGMT 1
#define SRT_LOGFA_CONN 2
#define SRT_LOGFA_XTIMER 3
#define SRT_LOGFA_TSBPD 4
#define SRT_LOGFA_RSRC 5
#define SRT_LOGFA_HAICRYPT 6
#define SRT_LOGFA_CONGEST 7
#define SRT_LOGFA_PFILTER 8
#define SRT_LOGFA_APPLOG 10
#define SRT_LOGFA_API_CTRL 11
#define SRT_LOGFA_QUE_CTRL 13
#define SRT_LOGFA_EPOLL_UPD 16
#define SRT_LOGFA_API_RECV 21
#define SRT_LOGFA_BUF_RECV 22
#define SRT_LOGFA_QUE_RECV 23
#define SRT_LOGFA_CHN_RECV 24
#define SRT_LOGFA_GRP_RECV 25
#define SRT_LOGFA_API_SEND 31
#define SRT_LOGFA_BUF_SEND 32
#define SRT_LOGFA_QUE_SEND 33
#define SRT_LOGFA_CHN_SEND 34
#define SRT_LOGFA_GRP_SEND 35
#define SRT_LOGFA_INTERNAL 41
#define SRT_LOGFA_QUE_MGMT 43
#define SRT_LOGFA_CHN_MGMT 44
#define SRT_LOGFA_GRP_MGMT 45
#define SRT_LOGFA_EPOLL_API 46
#define SRT_LOGFA_LASTNONE 63

/*
 * Socket-group declarations mirror the public Haivision SRT v1.5.7 ABI.
 * The declarations and layouts remain exactly compatible while Robotweax
 * implements the control and data planes in independently testable slices.
 */
typedef enum SRT_GROUP_TYPE {
    SRT_GTYPE_UNDEFINED = 0,
    SRT_GTYPE_BROADCAST,
    SRT_GTYPE_BACKUP,
    SRT_GTYPE_E_END
} SRT_GROUP_TYPE;

static const uint32_t SRT_GFLAG_SYNCONMSG = 1;

typedef enum SRT_MemberStatus {
    SRT_GST_PENDING = 0,
    SRT_GST_IDLE,
    SRT_GST_RUNNING,
    SRT_GST_BROKEN
} SRT_MEMBERSTATUS;

typedef struct SRT_SocketGroupData_ {
    SRTSOCKET id;
    struct sockaddr_storage peeraddr;
    SRT_SOCKSTATUS sockstate;
    uint16_t weight;
    SRT_MEMBERSTATUS memberstate;
    int result;
    int token;
} SRT_SOCKGROUPDATA;

typedef struct SRT_SocketOptionObject SRT_SOCKOPT_CONFIG;

typedef struct SRT_GroupMemberConfig_ {
    SRTSOCKET id;
    struct sockaddr_storage srcaddr;
    struct sockaddr_storage peeraddr;
    uint16_t weight;
    SRT_SOCKOPT_CONFIG* config;
    int errorcode;
    int token;
} SRT_SOCKGROUPCONFIG;

typedef struct SRT_MsgCtrl_ {
    int flags;
    int msgttl;
    int inorder;
    int boundary;
    int64_t srctime;
    int32_t pktseq;
    int32_t msgno;
    SRT_SOCKGROUPDATA* grpdata;
    size_t grpdata_size;
} SRT_MSGCTRL;

static const int32_t SRT_SEQNO_NONE = -1;
static const int32_t SRT_MSGNO_NONE = -1;
static const int32_t SRT_MSGNO_CONTROL = 0;
static const int SRT_MSGTTL_INF = -1;

#define SRT_DEFAULT_SENDFILE_BLOCK 364000
#define SRT_DEFAULT_RECVFILE_BLOCK 7280000

#define SRT_SYNC_CLOCK_STDCXX_STEADY 0
#define SRT_SYNC_CLOCK_GETTIME_MONOTONIC 1
#define SRT_SYNC_CLOCK_WINQPC 2
#define SRT_SYNC_CLOCK_MACH_ABSTIME 3
#define SRT_SYNC_CLOCK_POSIX_GETTIMEOFDAY 4
#define SRT_SYNC_CLOCK_AMD64_RDTSC 5
#define SRT_SYNC_CLOCK_IA32_RDTSC 6
#define SRT_SYNC_CLOCK_IA64_ITC 7

/**
 * @file
 * @brief Robotweax implementation of the selected SRT 1.5.7 C API.
 *
 * Unless documented otherwise, a failing function returns `SRT_ERROR` (or
 * `SRT_INVALID_SOCK` for handle-producing functions) and records a
 * thread-local error retrievable with srt_getlasterror(). A successful call
 * does not clear a previously recorded error. Stateful calls are safe from
 * independent application threads, but callbacks may run concurrently and
 * application-owned buffers must not be accessed concurrently by another
 * thread. Pointer arguments are borrowed only for the duration of a call
 * unless a callback-registration contract says otherwise.
 */

/** @name Process lifecycle
 * @{ */

/**
 * @brief Acquires one process-wide SRT runtime reference.
 * @return 0 on success, or `SRT_ERROR` on failure.
 * @note Balance every successful call with srt_cleanup(). The first call may
 * initialize worker state. After a multithreaded process forks, the child must
 * call `exec` or `_exit` before using stateful SRT APIs.
 */
SRT_API int srt_startup(void);

/**
 * @brief Releases one process-wide SRT runtime reference.
 * @return 0 on success, or `SRT_ERROR` on failure.
 * @note Close sockets, groups, and epoll instances before the final cleanup.
 * The final call wakes blocked operations and performs ordered teardown.
 */
SRT_API int srt_cleanup(void);

/** @} */

/** @name Socket creation, binding, and connection
 * @{ */

/**
 * @brief Creates an SRT socket using the source-compatible socket signature.
 * @param af Requested address family.
 * @param type Requested socket type.
 * @param protocol Requested protocol value.
 * @return A new socket handle, or `SRT_INVALID_SOCK` on failure.
 * @note The caller owns the handle and closes it with srt_close().
 */
SRT_API SRTSOCKET srt_socket(int af, int type, int protocol);

/**
 * @brief Creates an SRT socket with default settings.
 * @return A caller-owned socket handle, or `SRT_INVALID_SOCK` on failure.
 */
SRT_API SRTSOCKET srt_create_socket(void);

/**
 * @brief Binds an unbound socket to an IPv4 or IPv6 local address.
 * @param socket Socket in the initial state.
 * @param name Address borrowed for this call.
 * @param name_size Size of the concrete address structure in bytes.
 * @return 0 on success, or `SRT_ERROR` on failure.
 */
SRT_API int srt_bind(
    SRTSOCKET socket, const struct sockaddr* name, int name_size);

/**
 * @brief Adopts an already-bound, unconnected native UDP socket.
 * @param socket SRT socket in the initial state.
 * @param native_socket Valid native UDP descriptor/socket.
 * @return 0 on success, or `SRT_ERROR` on failure.
 * @warning Ownership transfers to SRT only on success. The final SRT owner
 * closes the native socket; on failure the caller remains responsible for it.
 */
SRT_API int srt_bind_acquire(
    SRTSOCKET socket, UDPSOCKET native_socket);

/** @brief Source-compatible alias for srt_bind_acquire(). */
static inline int srt_bind_peerof(
    SRTSOCKET socket, UDPSOCKET native_socket)
{
    return srt_bind_acquire(socket, native_socket);
}

/**
 * @brief Starts accepting connections on a bound socket.
 * @param socket Bound socket.
 * @param backlog Maximum queued accepted connections.
 * @return 0 on success, or `SRT_ERROR` on failure.
 */
SRT_API int srt_listen(SRTSOCKET socket, int backlog);

/**
 * @brief Accepts one connection. `SRTO_RCVSYN` selects blocking or polling
 * behavior; this call does not apply `SRTO_RCVTIMEO`.
 * @param socket Listening socket.
 * @param[out] name Optional peer-address buffer.
 * @param[in,out] name_size Required when `name` is non-null; supplies capacity
 * and receives the concrete address size in bytes.
 * @return A caller-owned connected socket, or `SRT_INVALID_SOCK` on failure.
 */
SRT_API SRTSOCKET srt_accept(
    SRTSOCKET socket, struct sockaddr* name, int* name_size);

/**
 * @brief Accepts from an explicit bond domain of listeners.
 * @param[in] listeners Array borrowed for this call.
 * @param listener_count Number of elements in `listeners`.
 * @param timeout_milliseconds `-1` waits indefinitely, `0` polls, and a
 * positive value bounds the wait.
 * @return A caller-owned socket or mirror-group handle, or
 * `SRT_INVALID_SOCK` on failure/timeout.
 */
SRT_API SRTSOCKET srt_accept_bond(
    const SRTSOCKET listeners[], int listener_count,
    int64_t timeout_milliseconds);

/**
 * @brief Listener admission callback.
 *
 * The callback runs on a connection worker and may run concurrently with
 * application code. `peer_address` and `stream_id` are borrowed only during
 * the callback. `socket` is provisional and may be inspected or configured
 * only through connection-stage APIs. Return 0 to admit and nonzero to reject.
 * The callback must return promptly and must not throw through the C ABI.
 */
typedef int srt_listen_callback_fn(
    void* opaque,
    SRTSOCKET socket,
    int handshake_version,
    const struct sockaddr* peer_address,
    const char* stream_id);

/**
 * @brief Registers or clears the admission callback on an unconnected socket.
 * @param listener Socket before listen/connect; this registration is inherited
 * by its listener role.
 * @param callback Callback, or null to clear it.
 * @param opaque Application context passed unchanged to `callback`.
 * @return 0 on success, or `SRT_ERROR` on failure.
 * @warning The application must keep `opaque` and callback code alive until
 * the registration is cleared or the listener is closed.
 */
SRT_API int srt_listen_callback(
    SRTSOCKET listener,
    srt_listen_callback_fn* callback,
    void* opaque);

/**
 * @brief Outgoing connection-completion callback.
 *
 * The callback runs on a connection worker, possibly concurrently with the
 * registering thread. `peer_address` is borrowed only for the invocation.
 * `error_code` is `SRT_SUCCESS` on success; `token` identifies a group
 * endpoint or is `-1` for an ordinary socket. Return promptly and do not throw
 * through the C ABI.
 */
typedef void srt_connect_callback_fn(
    void* opaque,
    SRTSOCKET socket,
    int error_code,
    const struct sockaddr* peer_address,
    int token);

/**
 * @brief Registers or clears an outgoing connection callback.
 * @param caller Unconnected socket or group handle.
 * @param callback Callback, or null to clear it.
 * @param opaque Application context passed unchanged to `callback`.
 * @return 0 on success, or `SRT_ERROR` on failure.
 * @warning Keep the callback and `opaque` alive until cleared, the socket or
 * group is closed, or all member connection attempts that captured it finish.
 */
SRT_API int srt_connect_callback(
    SRTSOCKET caller,
    srt_connect_callback_fn* callback,
    void* opaque);

/**
 * @brief Connects a socket, or adds one endpoint to a group.
 * @param socket Unconnected socket or group handle.
 * @param name Remote IPv4/IPv6 address borrowed for this call.
 * @param name_size Concrete address size in bytes.
 * @return For a socket, 0 on success; for a group, the created member socket
 * ID; `SRT_ERROR` on failure.
 * @note Individual-socket blocking follows `SRTO_RCVSYN` and
 * `SRTO_CONNTIMEO`. A group's first member blocks until the group opens;
 * later member additions return asynchronously.
 */
SRT_API int srt_connect(
    SRTSOCKET socket, const struct sockaddr* name, int name_size);

/**
 * @brief Connects using a caller-selected 31-bit initial sequence number.
 * @param forced_initial_sequence `-1` selects the normal generated value;
 * otherwise the exact value must be in the 31-bit sequence domain.
 * @return The same result as srt_connect().
 * @warning Developer/test hook for deterministic experiments; production
 * integrations should normally call srt_connect().
 */
SRT_API int srt_connect_debug(
    SRTSOCKET socket, const struct sockaddr* name, int name_size,
    int forced_initial_sequence);

/**
 * @brief Binds a source address, then connects to a target.
 * @param source Required local address.
 * @param target Required remote address of the same family.
 * @param name_size Concrete address size used for both addresses.
 * @return The same socket/group result convention as srt_connect().
 * @note For an individual socket, if connect fails after bind succeeds, the
 * socket remains bound and open. Close and recreate it for a clean retry.
 */
SRT_API int srt_connect_bind(
    SRTSOCKET socket, const struct sockaddr* source,
    const struct sockaddr* target, int name_size);

/**
 * @brief Configures, binds, and connects one Rendezvous socket.
 * @param local_name Local IPv4/IPv6 address.
 * @param local_name_size Local address size in bytes.
 * @param remote_name Remote address of the same family.
 * @param remote_name_size Remote address size in bytes.
 * @return 0 on success, or `SRT_ERROR` on failure.
 */
SRT_API int srt_rendezvous(
    SRTSOCKET socket,
    const struct sockaddr* local_name,
    int local_name_size,
    const struct sockaddr* remote_name,
    int remote_name_size);

/**
 * @brief Closes a socket or group handle.
 * @return 0 while the stateful runtime is available, or `SRT_ERROR` after an
 * unsupported post-fork transition. Within an active runtime, close is
 * idempotent even for stale or unknown handles.
 * @note Enabled linger may block, or may complete asynchronously when
 * `SRTO_SNDSYN` is false. Do not use the handle after this call.
 */
SRT_API int srt_close(SRTSOCKET socket);

/**
 * @brief Retrieves the effective local address.
 * @param[out] name Destination address storage.
 * @param[in,out] name_size Supplies capacity and receives bytes written.
 */
SRT_API int srt_getsockname(
    SRTSOCKET socket, struct sockaddr* name, int* name_size);

/**
 * @brief Retrieves the connected peer address.
 * @param[out] name Destination address storage.
 * @param[in,out] name_size Supplies capacity and receives bytes written.
 */
SRT_API int srt_getpeername(
    SRTSOCKET socket, struct sockaddr* name, int* name_size);

/** @brief Returns the current socket/group state, or `SRTS_NONEXIST`. */
SRT_API SRT_SOCKSTATUS srt_getsockstate(SRTSOCKET socket);

/** @} */

/** @name Readiness polling
 * @{ */

/** @brief Creates a caller-owned epoll instance, or returns `SRT_ERROR`. */
SRT_API int srt_epoll_create(void);

/** @brief Removes every SRT socket from an epoll instance. */
SRT_API int srt_epoll_clear_usocks(int eid);

/**
 * @brief Adds an SRT socket with the supplied event mask.
 * @param events Borrowed event mask, or null for the compatible default mask.
 */
SRT_API int srt_epoll_add_usock(
    int eid, SRTSOCKET socket, const int* events);

/** @brief Adds a native system socket with the supplied/default event mask. */
SRT_API int srt_epoll_add_ssock(
    int eid, SYSSOCKET socket, const int* events);

/** @brief Removes an SRT socket from an epoll instance. */
SRT_API int srt_epoll_remove_usock(int eid, SRTSOCKET socket);

/** @brief Removes a native system socket from an epoll instance. */
SRT_API int srt_epoll_remove_ssock(int eid, SYSSOCKET socket);

/** @brief Replaces an SRT socket's subscribed event mask. */
SRT_API int srt_epoll_update_usock(
    int eid, SRTSOCKET socket, const int* events);

/** @brief Replaces a native system socket's subscribed event mask. */
SRT_API int srt_epoll_update_ssock(
    int eid, SYSSOCKET socket, const int* events);

/**
 * @brief Waits for separate read/write readiness sets.
 *
 * Every non-null descriptor array must have a corresponding non-null count.
 * On input each count is the array capacity; on output it is the number of
 * handles written. `timeout_milliseconds` is `-1` for an indefinite wait,
 * `0` for a poll, or a positive deadline. Returns the total ready count or
 * `SRT_ERROR`. This form does not consume `SRT_EPOLL_UPDATE`.
 */
SRT_API int srt_epoll_wait(
    int eid,
    SRTSOCKET* readfds, int* rnum,
    SRTSOCKET* writefds, int* wnum,
    int64_t timeout_milliseconds,
    SYSSOCKET* system_readfds, int* system_read_count,
    SYSSOCKET* system_writefds, int* system_write_count);

/**
 * @brief Waits for a unified SRT event array, including group UPDATE events.
 * @param[out] events Array of `event_count` entries.
 * @param event_count Output capacity; zero is allowed unless
 * `SRT_EPOLL_ENABLE_OUTPUTCHECK` is enabled.
 * @param timeout_milliseconds `-1` waits indefinitely, `0` polls.
 * @return Number of events written, 0 on timeout, or `SRT_ERROR`.
 */
SRT_API int srt_epoll_uwait(
    int eid, SRT_EPOLL_EVENT* events, int event_count,
    int64_t timeout_milliseconds);

/**
 * @brief Queries or updates epoll-instance flags.
 * @param flags `-1` queries without changing flags, 0 clears all flags, and
 * any other value adds the supplied flags.
 * @return Previous flags, or `SRT_ERROR`.
 */
SRT_API int32_t srt_epoll_set(int eid, int32_t flags);

/** @brief Releases an epoll instance and wakes its waiters. */
SRT_API int srt_epoll_release(int eid);

/** @} */

/** @name Logging
 * @{ */

/** @brief Sets the maximum emitted syslog-compatible severity. */
SRT_API void srt_setloglevel(int level);

/** @brief Enables one `SRT_LOGFA_*` functional area. */
SRT_API void srt_addlogfa(int functional_area);

/** @brief Disables one `SRT_LOGFA_*` functional area. */
SRT_API void srt_dellogfa(int functional_area);

/**
 * @brief Replaces the enabled functional-area set.
 * @param functional_areas Borrowed array; null selects an empty set.
 * @param functional_area_count Number of entries, bounded by the public area
 * domain.
 */
SRT_API void srt_resetlogfa(
    const int* functional_areas, size_t functional_area_count);

/**
 * @brief Replaces the process-wide log handler.
 * @param opaque Application context kept until the next replacement.
 * @param handler Handler, or null to restore stderr logging.
 * @warning The handler may run on any SRT/application logging thread. Keep
 * `opaque` alive while registered; see SRT_LOG_HANDLER_FN for string lifetime
 * and reentrancy rules.
 */
SRT_API void srt_setloghandler(
    void* opaque, SRT_LOG_HANDLER_FN* handler);

/** @brief Sets the process-wide bitwise `SRT_LOGF_*` formatting flags. */
SRT_API void srt_setlogflags(int flags);

/** @} */

/** @name Data and file I/O
 * @{ */

/**
 * @brief Sends Message-mode data or Stream-mode bytes.
 * @return Positive bytes accepted, or `SRT_ERROR`. Short positive Stream
 * writes are valid. Blocking follows `SRTO_SNDSYN`/`SRTO_SNDTIMEO`.
 * @note `buffer` is copied before return and is not retained.
 */
SRT_API int srt_send(SRTSOCKET socket, const char* buffer, int length);

/** @brief Sends with message TTL in milliseconds and in-order policy. */
SRT_API int srt_sendmsg(
    SRTSOCKET socket, const char* buffer, int length,
    int ttl, int inorder);

/**
 * @brief Sends using optional message/group metadata.
 * @param[in,out] control Optional initialized control. Input fields select
 * source time (microseconds in the srt_time_now() domain), TTL, and ordering;
 * `msgno` receives the assigned message number. Group sends also update
 * `srctime` and `pktseq` and may populate member results through `grpdata`.
 * @return Positive bytes accepted, or `SRT_ERROR`.
 */
SRT_API int srt_sendmsg2(
    SRTSOCKET socket, const char* buffer, int length,
    SRT_MSGCTRL* control);

/**
 * @brief Receives Message-mode data or Stream-mode bytes.
 * @return Positive bytes written, 0 for drained Stream EOF, or `SRT_ERROR`.
 * Short positive Stream reads are valid. Blocking follows
 * `SRTO_RCVSYN`/`SRTO_RCVTIMEO`.
 */
SRT_API int srt_recv(SRTSOCKET socket, char* buffer, int length);

/** @brief Equivalent to srt_recv() for the selected compatibility profile. */
SRT_API int srt_recvmsg(SRTSOCKET socket, char* buffer, int length);

/**
 * @brief Receives data and optional source-time/sequence/group metadata.
 * @param[in,out] control Optional initialized result. If `grpdata` is supplied,
 * `grpdata_size` is its input capacity and receives the snapshot size.
 * @return The same result convention as srt_recv().
 */
SRT_API int srt_recvmsg2(
    SRTSOCKET socket, char* buffer, int length,
    SRT_MSGCTRL* control);

/**
 * @brief Sends file bytes through Stream mode.
 * @param path NUL-terminated path borrowed for this call.
 * @param[in,out] offset File offset advanced only by transferred bytes.
 * @param size Bytes to send, or `-1` through end of file.
 * @param block Reusable application-buffer size in bytes.
 * @return Bytes transferred, or `SRT_ERROR`.
 * @warning Always blocking, independently of `SRTO_SNDSYN`.
 */
SRT_API int64_t srt_sendfile(
    SRTSOCKET socket, const char* path, int64_t* offset,
    int64_t size, int block);

/**
 * @brief Receives file bytes through Stream mode.
 * @param path NUL-terminated destination path borrowed for this call.
 * @param[in,out] offset File offset advanced only by transferred bytes.
 * @param size Maximum bytes to receive.
 * @param block Reusable application-buffer size in bytes.
 * @return Bytes transferred, or `SRT_ERROR`.
 * @warning Always blocking, independently of `SRTO_RCVSYN`.
 */
SRT_API int64_t srt_recvfile(
    SRTSOCKET socket, const char* path, int64_t* offset,
    int64_t size, int block);

/** @} */

/** @name Statistics and socket options
 * @{ */

/**
 * @brief Reads a coherent statistics snapshot.
 * @param[out] statistics Caller-owned compatible statistics structure.
 * @param clear Nonzero clears interval counters after the snapshot; lifetime
 * totals remain intact.
 */
SRT_API int srt_bstats(
    SRTSOCKET socket, SRT_TRACEBSTATS* statistics, int clear);

/**
 * @brief Reads moving-average or instantaneous statistics.
 * @param instantaneous Nonzero selects instantaneous buffer measurements.
 */
SRT_API int srt_bistats(
    SRTSOCKET socket, SRT_TRACEBSTATS* statistics,
    int clear, int instantaneous);

/**
 * @brief Reads current sender-buffer occupancy.
 * @param[out] blocks Optional queued packet count.
 * @param[out] bytes Optional queued payload-byte count.
 * @return Buffered timestamp span in milliseconds, or `SRT_ERROR`.
 */
SRT_API int srt_getsndbuffer(
    SRTSOCKET socket, size_t* blocks, size_t* bytes);

/**
 * @brief Gets an option using the source-compatible level signature.
 * @param[out] value Caller-owned output storage.
 * @param[in,out] value_size Supplies capacity and receives required/actual
 * bytes according to the option contract.
 */
SRT_API int srt_getsockopt(
    SRTSOCKET socket, int level, SRT_SOCKOPT option, void* value, int* value_size);

/** @brief Sets an option; input storage is copied and never retained. */
SRT_API int srt_setsockopt(
    SRTSOCKET socket, int level, SRT_SOCKOPT option,
    const void* value, int value_size);

/** @brief Gets an option without the compatibility-only level parameter. */
SRT_API int srt_getsockflag(
    SRTSOCKET socket, SRT_SOCKOPT option, void* value, int* value_size);

/** @brief Sets an option without the compatibility-only level parameter. */
SRT_API int srt_setsockflag(
    SRTSOCKET socket, SRT_SOCKOPT option, const void* value, int value_size);

/** @} */

/** @name Message control, errors, and clocks
 * @{ */

/** @brief Initializes `control` to srt_msgctrl_default; null is allowed. */
SRT_API void srt_msgctrl_init(SRT_MSGCTRL* control);

/** @brief Immutable default initializer for `SRT_MSGCTRL`. */
SRT_API extern const SRT_MSGCTRL srt_msgctrl_default;

/**
 * @brief Returns the current thread's last SRT error text.
 * @return Library-owned static storage; do not modify or free it.
 */
SRT_API const char* srt_getlasterror_str(void);

/**
 * @brief Returns the current thread's last SRT error code.
 * @param[out] system_error Optional underlying platform error code.
 */
SRT_API int srt_getlasterror(int* system_error);

/**
 * @brief Converts an SRT error code to stable library-owned text.
 * @param system_error Reserved for source compatibility.
 */
SRT_API const char* srt_strerror(int code, int system_error);

/** @brief Clears the calling thread's last-error record. */
SRT_API void srt_clearlasterror(void);

/** @brief Returns a socket's peer/application rejection reason. */
SRT_API int srt_getrejectreason(SRTSOCKET socket);

/**
 * @brief Sets an application-defined rejection reason on a provisional socket.
 * @param value At least `SRT_REJC_PREDEFINED`; application-defined reasons
 * normally use the `SRT_REJC_USERDEFINED` range.
 */
SRT_API int srt_setrejectreason(SRTSOCKET socket, int value);

/** @brief Library-owned predefined rejection-reason strings. */
SRT_API extern const char* const srt_rejectreason_msg[];

/** @brief Returns library-owned text for a rejection-reason identifier. */
SRT_API const char* srt_rejectreason_str(int id);

/** @brief Returns the packed `0x00XXYYZZ` library version. */
SRT_API uint32_t srt_getversion(void);

/** @brief Returns process-local monotonic time in microseconds. */
SRT_API int64_t srt_time_now(void);

/**
 * @brief Returns a socket's open instant in the srt_time_now() microsecond
 * domain, or `-1` for invalid, group, or closed handles.
 */
SRT_API int64_t srt_connection_time(SRTSOCKET socket);

/** @brief Returns the `SRT_SYNC_CLOCK_*` identifier used by srt_time_now(). */
SRT_API int srt_clock_type(void);

/** @} */

/** @name Connection groups
 * @{ */

/**
 * @brief Creates a Broadcast or Backup connection group.
 * @return Caller-owned group handle, or `SRT_INVALID_SOCK` on failure.
 */
SRT_API SRTSOCKET srt_create_group(SRT_GROUP_TYPE type);

/** @brief Returns a socket's current owning group, or `SRT_INVALID_SOCK`. */
SRT_API SRTSOCKET srt_groupof(SRTSOCKET socket);

/**
 * @brief Copies one coherent group-member snapshot.
 * @param[out] output Optional member array; null performs a size query.
 * @param[in,out] inout_length Supplies element capacity and receives required
 * or written element count.
 * @return Member count copied, 0 for a successful size query, or `SRT_ERROR`.
 */
SRT_API int srt_group_data(
    SRTSOCKET group, SRT_SOCKGROUPDATA* output, size_t* inout_length);

/** @brief Allocates an empty bounded endpoint-option snapshot. */
SRT_API SRT_SOCKOPT_CONFIG* srt_create_config(void);

/** @brief Releases a configuration; null is allowed. */
SRT_API void srt_delete_config(SRT_SOCKOPT_CONFIG* config);

/**
 * @brief Copies one option into an endpoint configuration.
 * @note A configuration holds at most 32 entries and 256 bytes per value.
 */
SRT_API int srt_config_add(
    SRT_SOCKOPT_CONFIG* config, SRT_SOCKOPT option,
    const void* contents, int length);

/**
 * @brief Validates and copies source/destination addresses into an endpoint.
 * @param source Optional same-family local address; null selects wildcard.
 * @param destination Required remote IPv4/IPv6 address.
 * @param name_length Concrete address size in bytes.
 * @return Value object whose `errorcode` is `SRT_SUCCESS` when valid.
 * @note The returned `config` pointer is null; callers may attach a
 * caller-owned configuration that remains valid through srt_connect_group().
 */
SRT_API SRT_SOCKGROUPCONFIG srt_prepare_endpoint(
    const struct sockaddr* source,
    const struct sockaddr* destination,
    int name_length);

/**
 * @brief Starts one or more configured group-member connections.
 * @param group Broadcast or Backup group.
 * @param[in,out] endpoints Array updated with member IDs and per-endpoint
 * error codes. Address and option values are copied before return.
 * @param endpoint_count Number of endpoint records.
 * @return One started/connected member socket, or `SRT_ERROR` when no endpoint
 * can be started or completed.
 * @note Each non-null endpoint `config` remains owned by the caller.
 */
SRT_API int srt_connect_group(
    SRTSOCKET group, SRT_SOCKGROUPCONFIG endpoints[], int endpoint_count);

/** @} */

#if defined(__cplusplus)
}
#endif

#endif
