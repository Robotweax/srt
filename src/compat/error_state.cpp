#include "compat/error_state.hpp"

namespace robotweax::srt::compat {
namespace {

thread_local ErrorState current_error;

} // namespace

void set_last_error(SRT_ERRNO code, int system_error) noexcept
{
    current_error = {.code = code, .system_error = system_error};
}

void clear_last_error() noexcept
{
    current_error = {};
}

ErrorState last_error() noexcept
{
    return current_error;
}

const char* error_text(int code) noexcept
{
    switch (code) {
    case SRT_SUCCESS: return "Success";
    case SRT_ECONNSETUP: return "Connection setup failure";
    case SRT_ENOSERVER: return "Connection setup failure: connection timed out";
    case SRT_ECONNREJ: return "Connection setup failure: connection rejected";
    case SRT_ESOCKFAIL:
        return "Connection setup failure: unable to create/configure SRT socket";
    case SRT_ESECFAIL:
        return "Connection setup failure: aborted for security reasons";
    case SRT_ESCLOSED:
        return "Connection setup failure: socket closed during operation";
    case SRT_ECONNFAIL: return "";
    case SRT_ECONNLOST: return "Connection was broken";
    case SRT_ENOCONN: return "Connection does not exist";
    case SRT_ERESOURCE: return "System resource failure";
    case SRT_ETHREAD:
        return "System resource failure: unable to create new threads";
    case SRT_ENOBUF:
        return "System resource failure: unable to allocate buffers";
    case SRT_ESYSOBJ:
        return "System resource failure: unable to allocate a system object";
    case SRT_EFILE: return "File system failure";
    case SRT_EINVRDOFF: return "File system failure: cannot seek read position";
    case SRT_ERDPERM: return "File system failure: failure in read";
    case SRT_EINVWROFF: return "File system failure: cannot seek write position";
    case SRT_EWRPERM: return "File system failure: failure in write";
    case SRT_EINVOP: return "Operation not supported";
    case SRT_EBOUNDSOCK:
        return "Operation not supported: Cannot do this operation on a BOUND socket";
    case SRT_ECONNSOCK:
        return "Operation not supported: Cannot do this operation on a CONNECTED or LISTENING socket";
    case SRT_EINVPARAM: return "Operation not supported: Bad parameters";
    case SRT_EINVSOCK: return "Operation not supported: Invalid socket ID";
    case SRT_EUNBOUNDSOCK:
        return "Operation not supported: Cannot do this operation on an UNBOUND socket";
    case SRT_ENOLISTEN:
        return "Operation not supported: Socket is not in listening state";
    case SRT_ERDVNOSERV:
        return "Operation not supported: Listen/accept is not supported in rendezvous connection setup";
    case SRT_ERDVUNBOUND:
        return "Operation not supported: Cannot call connect on UNBOUND socket in rendezvous connection setup";
    case SRT_EINVALMSGAPI:
        return "Operation not supported: Incorrect use of Message API (sendmsg/recvmsg)";
    case SRT_EINVALBUFFERAPI:
        return "Operation not supported: Incorrect use of Buffer API (send/recv) or File API (sendfile/recvfile)";
    case SRT_EDUPLISTEN:
        return "Operation not supported: Another socket is already listening on the same port";
    case SRT_ELARGEMSG:
        return "Operation not supported: Message too large to send (exceeds send buffer size)";
    case SRT_EINVPOLLID:
        return "Operation not supported: Invalid epoll ID";
    case SRT_EPOLLEMPTY:
        return "Operation not supported: All sockets removed from epoll, waiting would deadlock";
    case SRT_EBINDCONFLICT:
        return "Operation not supported: Another socket is bound to that port and is not reusable for requested settings";
    case SRT_EASYNCFAIL: return "Non-blocking call failure";
    case SRT_EASYNCSND:
        return "Non-blocking call failure: no buffer available for sending";
    case SRT_EASYNCRCV:
        return "Non-blocking call failure: no data available for reading";
    case SRT_ETIMEOUT:
        return "Non-blocking call failure: transmission timed out";
    case SRT_ECONGEST:
        return "Non-blocking call failure: early congestion notification";
    case SRT_EPEERERR: return "The peer side has signaled an error";
    default: return "UNDEFINED ERROR";
    }
}

} // namespace robotweax::srt::compat
