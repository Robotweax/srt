# Public SRT API compatibility

Robotweax SRT 0.2.3 provides a compatible public C boundary for the selected
Haivision SRT v1.5.7 API profile. It is an independent implementation, not a
source fork and not a claim of complete compatibility with every historical or
future `libsrt` behavior.

The profile is derived from the official v1.5.7 tag at immutable commit
`899348d8318eb9a3c5a5b6ec43c4a1114288773a`.

The machine-readable authority is
[`compat/srt-1.5.7-api.json`](../compat/srt-1.5.7-api.json). It records every
selected function, public type, socket option, constant group, ABI check, and
its exact implemented or partial scope. When this overview and the manifest
differ, the manifest is authoritative.

## Version axes

These values are intentionally independent:

| Axis | Robotweax SRT 0.2.3 |
| --- | --- |
| Project release | `0.2.3` |
| Shared-library ABI line | `0.2` |
| Default compatible public API | Haivision SRT `1.5.7` |
| `srt_getversion()` | `1.5.7` |
| Supported production handshake | HSv5 |
| Default payload cipher | AES-CTR |
| Optional authenticated extension | Robotweax AES-GCM profile |

The optional AES-GCM surface is compiled with the historical flag
`ENABLE_AEAD_API_PREVIEW=ON`. It is a released, default-off extension and does
not change the default v1.5.7 API claim. The upstream-preview
`SRTO_CRYPTOMODE` and `SRT_REJ_CRYPTO` declarations therefore remain guarded;
`SRT_KM_S_BADCRYPTOMODE` and the `SRT_KM_S_E_SIZE = 6` sentinel are part of
the unconditional v1.5.7 key-management enum.

## Unreleased caller replay correction

After an HSv5 caller advances to CONCLUSION, a delayed INDUCTION response is
ignored before version/crypto policy, routing metadata, or protocol state can
change. This includes stale responses with different cookie, socket-ID,
version, or key-length fields: none can restart negotiation or authenticate a
connection. The existing CONCLUSION validation, retry budget and overall
deadline still apply. The listener and rendezvous contracts are unchanged.

Regression coverage includes deterministic state/route/retry checks and
encrypted blocking and asynchronous callers receiving an injected stale
INDUCTION immediately before the valid final response. This correction does
not change the public C ABI or claim immunity to network overload.

## Headers, library, and package

The installed public layouts support both:

```c
#include <srt/srt.h>
```

and the compatibility forwarding header:

```c
#include <srt.h>
```

The CMake target is `RobotweaxSRT::srt`; POSIX installations also provide
`robotweax-srt.pc`. Static and shared artifacts use the conventional `srt`
library name. The 0.2.3 package exports an exact, versioned 80-symbol C ABI.
The installed `<robotweax_srt.h>` header contributes the six native packet and
option symbols in that ABI. Source-tree headers below `include/robotweax/srt/`
are private C++ implementation interfaces and are not installed.

See [Building and installing](building.md) and [Integration guide](integration.md)
for consumption examples.

## Supported API areas

The selected profile includes the following public areas:

- process lifecycle, version, time, error, and rejection helpers;
- IPv4 and IPv6 bind, listen, accept, caller connect, source-bound connect,
  and same-family Rendezvous;
- blocking and nonblocking connection operation, epoll readiness, connect and
  listener callbacks;
- Live/Message and File/Stream send and receive calls, message control, file
  helpers, EOF, peer-error, and linger behavior;
- socket option get/set, local and peer names, socket state, and Stream ID;
- AES-CTR encryption and acknowledged key rotation;
- default-off AES-GCM extension configuration when both library and consumer
  use the extension header contract;
- packet filters and bounded Row, Column, and Matrix FEC;
- Broadcast and weighted Main/Backup Connection Groups;
- statistics, send-buffer inspection, and logging control;
- shared/acquired UDP binding and deterministic ownership transfer;
- relocatable CMake and pkg-config packages.

The manifest currently marks all selected public types and exported data as
implemented. Functions or options marked `partial` exist with the documented
scope but do not claim every platform, mode, legacy behavior, or edge case of
the upstream API.

## Important partial surfaces

Applications must check the manifest before assuming behavior beyond these
released boundaries:

| Surface | Released boundary |
| --- | --- |
| Connection establishment | IPv4/IPv6 HSv5 Caller/Listener and bound same-family Rendezvous; blocking and nonblocking operation |
| `srt_accept` / `srt_listen` | Individual sockets plus the documented Broadcast/Backup group admission paths |
| Message I/O | One complete message per Live call, bounded by negotiated payload size; blocking, timeout, and async error contracts apply |
| Stream I/O | Partial writes and reads, cross-packet reads, zero-byte EOF after drain, FileCC recovery, and file helpers |
| `srt_epoll_wait` / `srt_epoll_uwait` | Documented SRT readiness; do not assume undocumented native descriptor combinations |
| Statistics | Stable v1.5.7 layouts and documented Robotweax counters; only measured fields should be used for decisions |
| Groups | Broadcast and weighted Main/Backup; no general-purpose striping or arbitrary multipath policy |
| `SRTO_MSS` | 76–1500 bytes; jumbo datagrams above the current fixed storage ceiling are rejected |
| `SRTO_CONGESTION` | Implemented LiveCC/FileCC selection; custom congestion modules are not supported |
| `SRTO_PACKETFILTER` | Built-in bounded `fec` grammar and documented geometries only |

The disabled-by-default `SRTO_MAXREXMITBW` declaration is outside the selected
v1.5.7 default profile. `SRTO_CRYPTOMODE` is present only in an extension build.
`SYSSOCKET_INVALID` follows the platform's public `SYSSOCKET` representation.

## Lifecycle and concurrency

`srt_startup`, implicit startup through socket or epoll creation, and the final
`srt_cleanup` transition form one serialized process-wide runtime generation.
Balanced callers may use independent startup/use/cleanup scopes concurrently.

The final cleanup:

- prevents late registry insertion into a generation being destroyed;
- wakes blocked I/O and epoll waiters;
- interrupts connection workers;
- closes listeners and queued accepted sockets;
- stops encryption rotation;
- cancels pending linger work; and
- performs ordered teardown before global dependencies disappear.

`srt_close` is idempotent at the public boundary and continues to honor the
configured linger contract during normal operation. The terminal process
cleanup bypasses per-socket linger so shutdown time does not grow by one
timeout per connection.

After shutdown completes, the registry releases the full socket or group
record. Operations that already acquired shared ownership may finish safely;
asynchronous linger retains the closing socket until drain or timeout.
Configured passphrases are erased after setup cancellation, without waiting
for final process cleanup or asynchronous linger completion.

`srt_getsockstate` reports `SRTS_CLOSED` for the most recent 4096 completed
closes in each of the socket and group registries. Older closed handles report
`SRTS_NONEXIST`; existing epoll subscriptions still report their terminal
error event. Applications must remove unused subscriptions themselves.
Numeric socket/group IDs are never recycled during the loaded runtime's
lifetime, including across cleanup/startup cycles. Exhausting the positive
30-bit ID space makes creation fail rather than alias a stale handle.
This bounded status history is not a promise to retain closed socket options
or statistics, nor a claim of identical reclamation timing to Haivision.

Callbacks may run concurrently with application work. Applications must not
hold a lock needed by a callback while waiting for a socket operation to
complete. The callback-specific contracts are described in
[Integration guide](integration.md).

## POSIX fork policy

Forking before any stateful Robotweax API initializes the runtime is supported;
the child may initialize its own runtime normally.

After runtime initialization, inherited sockets, epoll IDs, registries, and
worker state are not usable in the child. Stateful API calls fail immediately
with `SRT_EINVOP`, `srt_getsockstate` reports `SRTS_NONEXIST`, and
`srt_getrejectreason` reports `SRT_REJ_UNKNOWN`. The parent remains usable.

Stateless helpers such as `srt_getversion`, `srt_time_now`, error-string
conversion, message-control initialization, and last-error inspection remain
available. An affected child must call `exec` or `_exit`; Robotweax does not
attempt to repair thread and mutex state copied from a multithreaded parent.

## Addressing and UDP ownership

The released boundary supports:

- direct IPv4 and IPv6 binding, port autoselection, and scope IDs;
- `SRTO_IPV6ONLY` and explicit dual-stack wildcard behavior;
- compatible shared local endpoints with destination-socket routing;
- `srt_bind_acquire` ownership of a validated, already-bound, unconnected UDP
  socket; and
- release of the acquired native descriptor with the final SRT owner.

Rendezvous peers must use matching address families. Oversized UDP datagrams
are consumed and discarded atomically; a truncated prefix is never parsed as
a complete SRT packet.

## Error contract

Include `<srt/access_control.h>` for the `SRT_REJX_*` application rejection
constants. For example, a listener callback can use
`srt_setrejectreason(socket, SRT_REJX_OVERLOAD)` before rejecting a connection.
These constants describe application policy; they do not provide an
authentication or access-control engine. The header is standalone and is
installed with Robotweax's other public headers, so no Haivision header is
required. Legacy UDT headers are not part of this compatibility surface.

Public calls return the compatible success or error sentinel and update a
thread-local last-error record. Unsupported combinations fail explicitly.
Connection rejection reasons, asynchronous completion errors, peer errors,
authentication failures, EOF, and local validation failures remain distinct.

Do not write application logic that depends on an undocumented error from a
different SRT implementation. Use Robotweax's public headers, this guide, and
the manifest as the contract.

## Compatibility limits

Robotweax SRT 0.2.3 is not yet a link-compatible replacement for arbitrary
`libsrt` applications. In particular:

- HSv4 positive connection establishment is not supported in 0.2;
- APIs introduced outside the selected v1.5.7 surface are excluded unless a
  Robotweax extension explicitly documents them;
- platform-, topology-, or mode-specific behavior not named in the manifest
  remains unclaimed;
- long-duration, dedicated-host, and physical-network validation remains an
  application deployment responsibility; and
- private implementation details, scheduler topology, and object ownership are
  not ABI contracts.

See [Compatibility status](compatibility.md), [Known limitations](limitations.md),
and [Migration from 0.1](migration-0.2.md) before replacing an existing SRT
library.
