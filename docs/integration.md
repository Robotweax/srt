# Integration guide

This guide describes the stable integration surface of Robotweax SRT 0.2.3.
It assumes the library has been installed or provided through a package built
as described in [Building and installing](building.md).

## Select the public surface

For the SRT socket API, include:

```c
#include <srt/srt.h>
```

The flat `<srt.h>` forwarding header is also installed. Prefer `<srt/srt.h>` in
new code so the dependency is unambiguous.

Link through the installed target:

```cmake
find_package(RobotweaxSRT 0.2 CONFIG REQUIRED)
target_link_libraries(my_application PRIVATE RobotweaxSRT::srt)
```

Headers under `include/robotweax/srt/` are private native C++ implementation
interfaces. They remain available in the source tree for project development,
but are not installed and carry no source- or binary-compatibility promise for
applications. The small installed `<robotweax_srt.h>` C interface is a stable
native packet/options boundary, not a replacement for the socket API.

## Version checks

These values describe different things:

- Robotweax release: 0.2.3;
- shared-library ABI: 0.2;
- default compatible API and `srt_getversion()`: 1.5.7;
- connection establishment: HSv5.

Use package metadata to select Robotweax releases. Use `srt_getversion()` only
when application logic needs the compatible SRT API value.

## Runtime lifecycle

Call `srt_startup()` before the first SRT socket is created. Close all sockets,
epoll instances, and group handles before the final `srt_cleanup()`.

```c
if (srt_startup() == SRT_ERROR) {
    fprintf(stderr, "SRT startup failed: %s\n", srt_getlasterror_str());
    return 1;
}

SRTSOCKET socket = srt_create_socket();
if (socket == SRT_INVALID_SOCK) {
    fprintf(stderr, "socket creation failed: %s\n",
            srt_getlasterror_str());
    (void)srt_cleanup();
    return 1;
}

/* Configure and use the socket. */

(void)srt_close(socket);
(void)srt_cleanup();
```

Wrap this lifecycle in one process-level owner. Avoid repeated global cleanup
while other threads still create or use SRT handles.

## Configure before connecting

Many options are connection-stage or pre-bind settings. Apply them before
`srt_bind`, `srt_listen`, `srt_connect`, or `srt_rendezvous`. Examples include:

- `SRTO_TRANSTYPE` and `SRTO_MESSAGEAPI`;
- latency and TSBPD configuration;
- `SRTO_MSS`, UDP buffers, TTL, and traffic class;
- Stream ID and minimum peer version;
- passphrase, key length, key rotation, and crypto mode;
- packet-filter configuration;
- Rendezvous or connection-group settings.

Check every setter result. Unsupported options return `SRT_ERROR` with a
defined last-error value; Robotweax does not silently approximate an
unsupported setting. Consult [Socket options](socket-options.md) for types,
sizes, valid ranges, and mutability.

## Caller and Listener

The normal Listener sequence is:

```c
SRTSOCKET listener = srt_create_socket();
/* Set Listener options. */

if (srt_bind(listener, local_address, local_address_size) == SRT_ERROR ||
    srt_listen(listener, backlog) == SRT_ERROR) {
    /* Handle and close the Listener. */
}

int peer_size = sizeof(peer_storage);
SRTSOCKET connection = srt_accept(
    listener, (struct sockaddr*)&peer_storage, &peer_size);
```

Transfer data on the accepted `connection`, not on the listening socket.
Accepted sockets share the Listener's UDP channel and inherit applicable
connection settings.

The normal Caller sequence is:

```c
SRTSOCKET caller = srt_create_socket();
/* Set Caller options. */

if (srt_connect(caller, remote_address, remote_address_size) == SRT_ERROR) {
    /* Inspect srt_getlasterror() or srt_getlasterror_str(). */
}
```

The Caller may bind explicitly before connecting or use `srt_connect_bind()`
to select a source address. See [Connect variants](connect-variants.md).

## Rendezvous

In Rendezvous mode both endpoints bind and participate in simultaneous-open
connection establishment. Configure both peers consistently, including
address family, transport mode, encryption, and packet-filter settings.

Use either:

- `SRTO_RENDEZVOUS` followed by bind/connect; or
- `srt_rendezvous()` with local and remote addresses.

Robotweax SRT 0.2 establishes supported connections with HSv5. It does not
automatically downgrade to positive HSv4. Read [Rendezvous](rendezvous.md) for
startup ordering, role resolution, and failure behavior.

## Message and Stream I/O

Live/Message mode preserves application message boundaries. Use `srt_sendmsg2`
and `srt_recvmsg2` when you need `SRT_MSGCTRL` sequence, source-time, TTL, or
group metadata. Initialize controls with `srt_msgctrl_init()`.

File/Stream mode is an ordered byte stream. Reads and writes may be partial,
and a zero-byte receive after buffered data is drained represents stream EOF.
Loop until the requested byte count has been processed. The `srt_sendfile()`
and `srt_recvfile()` helpers provide blocking path/offset adapters.

Do not mix Message and Stream assumptions. In particular, a small Message
receive buffer must not be treated like a partial Stream read. See
[File/Stream mode](file-mode.md).

## Blocking, nonblocking, and readiness

`SRTO_SNDSYN` and `SRTO_RCVSYN` select synchronous behavior. Send and receive
timeouts bound blocking calls. In nonblocking mode, retryable results are
reported through the SRT last-error API.

For event-driven applications, use the complete `srt_epoll_*` family. Register
the desired event mask, drain edge-triggered readiness until the operation is
retryable, and release the epoll handle with `srt_epoll_release()`.

Connection callbacks run asynchronously and must return quickly. Do not block
them on application work. Listener callbacks may inspect the provisional
socket and set allowed connection-stage options before admission.

## Encryption

### Default AES-CTR

Set the passphrase and key length before connecting. Configure the same policy
on both peers and decide whether encryption mismatches must reject the
connection. Keep credentials out of logs and command lines.

AES-CTR provides confidentiality but does not authenticate the payload. If the
application requires authenticated payloads, evaluate the AES-GCM extension.

### Opt-in AES-GCM

Robotweax SRT 0.2.0 releases a version-pinned AES-GCM extension selected with
the historical `ENABLE_AEAD_API_PREVIEW=ON` build flag. In that build,
`SRTO_CRYPTOMODE` is available as a pre-connection option. Use a matching
GCM-enabled package throughout the dependency graph.

The installed CMake target propagates the compile definition. Do not manually
define it for a consumer linked to a default AES-CTR library, and do not mix
default and GCM-enabled headers or binaries. Review [Encryption and key
rotation](encryption.md) for supported modes and combinations.

## Packet-filter FEC

Configure `SRTO_PACKETFILTER` before connection establishment. Robotweax
supports the built-in Row, Column, and Matrix FEC profiles within their
documented transport combinations. FEC consumes payload budget, so derive
message sizes from the negotiated MSS and active security mode. See
[Packet-filter FEC](packet-filter.md).

## Connection groups

The compatible group API supports Broadcast and Backup policies for documented
Live Caller/Listener use cases. Group handles and socket handles share the
public `SRTSOCKET` namespace but have different option ownership and lifecycle
rules. Query member state and per-member results rather than treating the group
as one network path.

Applications own topology, path selection inputs, replacement policy, and
failure escalation. See [Connection groups](connection-groups.md) and
[Known limitations](limitations.md).

## Errors and rejection

Most public functions return `SRT_ERROR` or `SRT_INVALID_SOCK` on failure. Read
the thread-local error immediately:

```c
int system_error = 0;
int code = srt_getlasterror(&system_error);
const char* text = srt_strerror(code, system_error);
```

Connection rejection has a separate reason API. Preserve both the SRT error
and peer rejection reason in diagnostics, while excluding passphrases and key
material.

## Shutdown and reconnection

Close connected sockets with `srt_close()`. If linger is enabled, account for
its synchronous or deferred close semantics. Stream receivers should drain
buffered data before interpreting zero-byte EOF.

The application owns reconnect timing, endpoint discovery, credential
rotation policy, and restoration of group topology. Recreate and configure a
new socket rather than assuming a broken connection can be reset in place.

## Deployment checklist

1. Link through `RobotweaxSRT::srt` or the installed pkg-config file.
2. Use `<srt/srt.h>` and avoid implementation C++ symbols in the ABI boundary.
3. Verify the 0.2 package and matching static/shared runtime architecture.
4. Apply every connection-stage option before bind/connect/listen.
5. Make Message versus Stream semantics explicit.
6. Keep the default AES-CTR and opt-in AES-GCM package variants separate.
7. Test the actual peer versions, modes, address families, and fault patterns.
8. Capture SRT errors, rejection reasons, and statistics without secrets.
9. Validate shutdown, reconnect, and deployment packaging on every platform.
10. Review [Known limitations](limitations.md) before production release.
