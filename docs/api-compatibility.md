# Public SRT API compatibility

Robotweax SRT 0.2.7 provides a compatible public C boundary for the selected
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

| Axis | Robotweax SRT 0.2.7 |
| --- | --- |
| Project release | `0.2.7` |
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

## Caller replay correction

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
`robotweax-srt.pc`. Static and shared artifacts use the coinstallable
`robotweax-srt` library name by default. The explicit
`ROBOTWEAX_SRT_INSTALL_LAYOUT=legacy` build option selects the conventional
`srt` library name and include paths. The released 0.2.7/0.2.8 packages export an exact, versioned 80-symbol C ABI.
The development branch adds optional group-path, RTT-history and Backup-sender
observation entry points (83 symbols),
with the existing structures and entry points unchanged. See
[optional group path identifiers](group-path-identifiers.md) for its fixed v1
layout and provider-availability rules.
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

The default-off `SRTO_MAXREXMITBW` extension is outside the selected v1.5.7
default profile. Development builds can enable `ENABLE_MAXREXMITBW`; see its
[option contract](socket-options.md#optional-retransmission-bandwidth-limit).
The published 0.2.7 binaries do not contain this extension. `SRTO_CRYPTOMODE` is present only in an extension build.
`SYSSOCKET_INVALID` follows the platform's public `SYSSOCKET` representation.

## Lifecycle and concurrency

`srt_startup`, implicit startup through socket or epoll creation, and the final
`srt_cleanup` transition form one serialized process-wide runtime generation.
Balanced callers may use independent startup/use/cleanup scopes concurrently.
A startup from a callback or cleanup worker during final cleanup fails with
`SRT_EINVOP` to avoid waiting on its own teardown. Other threads wait for that
cleanup to finish before starting a new generation.

The final cleanup:

- prevents late registry insertion into a generation being destroyed;
- wakes blocked I/O and epoll waiters;
- interrupts connection workers;
- closes listeners and queued accepted sockets;
- stops encryption rotation;
- cancels pending linger work; and
- performs ordered teardown before global dependencies disappear.

Unlike the reference's `SRT_EINVSOCK` for an unknown handle, this implementation
also treats closing an unknown handle as a successful no-op.

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
IDs are not sequential: each registry walks a keyed pseudo-random permutation
of that space, with keys drawn once per loaded runtime from the cryptographic
provider. Because socket IDs appear on the wire, one ID reveals nothing about
the IDs of other sockets. If the random source fails, creation fails rather
than fall back to a predictable ID.
This bounded status history is not a promise to retain closed socket options
or statistics, nor a claim of identical reclamation timing to Haivision.

Callbacks may run concurrently with application work. Applications must not
hold a lock needed by a callback while waiting for a socket operation to
complete. The callback-specific contracts are described in
[Integration guide](integration.md).

Internal cleanup reached from an affinity worker hands its final connection
drain to the existing process work executor. The runtime becomes locally
closed before submission, and accepted cleanup owns the runtime and UDP channel
until completion. The executor retains its four-worker, 1,024-entry bound;
it is prepared by asynchronous connect callers and listener startup. Cleanup
does not create a new executor generation. A full queue, unavailable/stopped
executor, or allocation failure keeps the bounded synchronous fallback, which
can occupy the calling shard for its retry budget. This is a resource-failure
exception, not an unconditional nonblocking-close guarantee.

The final cumulative ACK still precedes SHUTDOWN, with at most one ACK interval
(10 ms of real retry time once the drain runs) under UDP backpressure. Queue
wait time is separate from that retry budget. Public close/linger calls from
application threads retain their existing synchronous/asynchronous contracts.

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

Static compatibility state records its constructing process. If inherited
destructors run in a child, Robotweax leaves that state intact until the OS
reclaims the child's address space and descriptors. This prevents its own
teardown from taking inherited locks, joining absent parent workers, or
removing registrations from a shared kernel readiness queue. The owning
process keeps normal reverse-order teardown, including when `srt_cleanup`
is omitted. This defensive guard does not make general `exit` or C++ stack
unwinding after a multithreaded fork safe; the child policy above still applies.

## Addressing and UDP ownership

The released boundary supports:

- direct IPv4 and IPv6 binding, port autoselection, and scope IDs;
- `SRTO_IPV6ONLY` and explicit dual-stack wildcard behavior;
- compatible shared local endpoints with destination-socket routing;
- `srt_bind_acquire` ownership of a validated, already-bound, unconnected UDP
  socket; and
- release of the acquired native descriptor with the final SRT owner.

`srt_getsockname` reports the binding before connection admission and on
listeners. After successful caller, accepted, or rendezvous setup, a wildcard
binding reports a connection-local outgoing-route address snapshot when the OS
can resolve one. The original SRT port is retained, including IPv6 scope and
IPv4-mapped IPv6 representation. Explicit non-wildcard bindings keep their
configured address. Broken connections retain their last successful snapshot;
closed handles fail with `SRT_EINVSOCK`.

The snapshot is resolved once at connection admission with the socket's address
family, IPv6-only mode, traffic class and configured device. Resolution uses one
short-lived UDP socket without sending data or connecting the shared transport
channel. If resolution fails, `srt_getsockname` retains the bound address and the
connection continues normally. Shared sockets retain independent snapshots and
continue using the same original binding. Retain the preconnection binding
address when adding another socket to a shared wildcard endpoint; the connected
route snapshot is not a replacement for that binding identity.
A snapshot describes the outgoing
route at setup, not a per-packet source, incoming destination, NAT-visible address,
or persistent interface identity; it does not update after a route change.
Applications that need to select an interface should explicitly bind it or use
`SRTO_BINDTODEVICE` where supported.

This behavior matches the concrete connection-address readback observed through
an independently authored public-API probe against Haivision SRT 1.5.7 on IPv4,
IPv6 and dual-stack loopback, including shared and acquired sockets. The
[reference API documentation](https://github.com/Haivision/srt/blob/v1.5.7/docs/API/API-functions.md#srt_getsockname)
describes the bound-address contract, so the connection-route snapshot is a
version-scoped compatibility behavior rather than a wire requirement.

On Linux, `srt_bind_acquire` also accepts distinct native UDP sockets with
`SO_REUSEPORT` enabled on the same exact local endpoint, IPv6-only mode, and
configured device. Each adopted socket retains its own datagram channel;
`SRTO_REUSEADDR` and native `SO_REUSEADDR` alone do not permit this independent
channel configuration. Multi-member native groups support listeners and their
accepted connections only: caller and rendezvous `srt_connect` attempts fail
immediately with `SRT_EINVOP`, including asynchronous connect. This restriction
remains on a surviving channel after another member closes. A single adopted
reuseport socket retains caller support; once it begins outgoing connection
setup, adopting another socket at that endpoint fails with `SRT_EBINDCONFLICT`.
A descriptor alias of an already adopted native socket
is rejected rather than assigned a second independent channel. Overlapping
wildcard or dual-stack bindings, and mixing
ordinary `srt_bind` sockets with the native reuseport group, still fail with
`SRT_EBINDCONFLICT`. Other platforms retain the single adopted-channel boundary.

The application must bind every native group member before any listener starts
accepting traffic, then keep group membership fixed until all connections have
closed. Adding or removing a member can redirect established flows to a channel
that does not own their connection state. Linux requires the same effective UID
for sockets sharing a reuseport endpoint; isolate a service's UID from other
processes that could join its group. Each channel has its own kernel buffers and
runtime work, so increasing group size increases resource use.

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

A nonblocking connection stays `SRTS_CONNECTING` while setup is in progress. A
terminal timeout or peer rejection changes it to `SRTS_BROKEN` before the
completion callback runs. Epoll then reports the requested
`SRT_EPOLL_IN`, `SRT_EPOLL_OUT`, and `SRT_EPOLL_ERR` interests. The rejection reason
and callback error remain available. Close the failed socket and create a new
one for another attempt. Blocking failures use the same terminal state.

Nonblocking caller setup publishes a peer rejection no earlier than 10 ms from
connect invocation, including a negative key-material response or peer
negotiation policy failure. Until that completion deadline, the socket remains
`SRTS_CONNECTING`. At completion, the terminal state and rejection reason are
published before epoll notification and the completion callback. A rejection
received after 10 ms completes immediately. Success, local validation/resource
errors, cancellation, ordinary handshake timeout, blocking caller setup, and
Rendezvous retain their existing timing. Close cancels deferred completion and
preserves the callback-before-external-close-return contract.

This default compatibility pacing follows black-box observations of Haivision
SRT 1.5.7 at revision `899348d8318eb9a3c5a5b6ec43c4a1114288773a`, where a
nonblocking rejection woke epoll after approximately 10 ms. It limits a single
serial loop of peer-rejected nonblocking connects to roughly 100 attempts per
second; it is not an aggregate admission limit. The runtime retains one pending result and replaces
the handshake deadlines with one bounded scheduler timer. A completion-timer
admission failure is reported as a resource failure. `SRTO_CONNTIMEO` governs the
handshake timeout, not an application reconnect schedule.

Apply a bounded reconnect policy independently of epoll and callback timing. For
transient failures, use increasing delays with jitter, a maximum delay, and an
attempt or elapsed-time limit. Treat `SRT_REJ_BADSECRET`, `SRT_REJ_UNSECURE`, and
`SRT_REJ_MESSAGEAPI` as configuration failures: suspend automatic retries until
the relevant configuration changes. Listener warning suppression limits log
output, not connection attempts or rejection packets. See
[rejection timing diagnostics](testing.md#rejection-timing-diagnostics) for a
checked loopback probe.

Do not write application logic that depends on an undocumented error from a
different SRT implementation. Use Robotweax's public headers, this guide, and
the manifest as the contract.

## Compatibility limits

Robotweax SRT 0.2.7 is not yet a link-compatible replacement for arbitrary
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

See [Optional session authentication](session-authentication.md) for the default-off
`SRTO_ROBOTWEAX_SESSIONAUTH` option and its private wire contract.

## Optional RTT observation metadata

`robotweax_srt_rtt_data_v1` is an additive Robotweax extension, not part of
`SRT_TRACEBSTATS` or the common SRT API. Discover or explicitly inject this
symbol from the selected library after qualifying its exact build. Old Robotweax
and Haivision providers remain supported without it; absence means metadata is
unavailable, never that a numeric estimate is valid or invalid.

Initialize `ROBOTWEAX_SRT_RTTDATA_V1.struct_size` and `abi_version = 1`, and pass
the actual writable byte capacity. V1 is 48 bytes; larger buffers are accepted,
but only V1 is written. Invalid size/version/null output preserves the buffer.
The getter follows socket statistics lifecycle rules, including retained BROKEN
members. Group handles have no per-path RTT and are rejected. Closing/closed
sockets fail; a new connection starts a new observation history.

The snapshot reports the current numeric estimate and variation in microseconds,
an accepted-observation flag, last accepted source, and separate local ACKACK and
peer-estimate ages. `UINT64_MAX` is unavailable; zero is a real observation at the
snapshot instant. Local observations require a valid tracked ACKACK with positive
elapsed time. Peer estimates are accepted Full/Small ACK values under the existing
estimator rules; their age measures local acceptance, not the peer's unknown
measurement age. Both histories may remain after smoothing or replacement; they
do not prove the contribution of either source to the current estimate.

The initial 100 ms / 50 ms pair remains unobserved until the estimator accepts an
observation. A later accepted estimate can equal that pair; numeric equality alone
never establishes validity. An observation flag means history exists, not freshness
or accuracy. Keepalive, statistics reads and interval clearing do not refresh or
reset metadata. Reading a retained BROKEN member shows its increasing age until
close. No estimator, congestion controller, Backup qualification rule or wire
exchange changes, and no measurement traffic is generated by the getter.

## Optional Backup sender observation

`robotweax_srt_group_send_state_v1` returns an 80-byte, caller-owned V1 snapshot
for a local `SRT_GTYPE_BACKUP` group. Initialize `struct_size`, `abi_version=1`
and supply the buffer size. Invalid size/version/null output fails with
`SRT_EINVPARAM`; invalid, closed or plain socket handles with `SRT_EINVSOCK`;
other group types with `SRT_EINVOP`. Failure leaves the caller buffer unchanged;
a larger buffer's tail remains untouched. A newly created Backup group is
readable and explicitly unselected.

The coherent snapshot identifies group generation, selected member socket and
connection generation, pending probe socket and generation, and local monotonic
ages in microseconds. Absent ages are `UINT64_MAX`, absent sockets
`SRT_INVALID_SOCK`, absent member generations zero. Ages measure observation
bookkeeping, not peer responsiveness or RTT freshness. Selection is local
sender coordinator authority, not a health guarantee or a remote/global
receiver election. Concurrent removal clears the matching member generation;
a reused socket has a distinct connection generation.

`authority_revision` starts at zero and advances modulo 2^64 for each actual
selection, replacement or clear, including initial selection. Probe start and
cancellation do not advance it. Compare revisions only within one group
generation. A revision gap reveals unobserved changes even if the selected
member returned to its earlier identity; it does not reconstruct intermediate
identities, reasons or exact transition times. New groups start a new history.
Reads and statistics clears do not mutate or reset this observation.

The getter takes only the short group metadata lock. It sends no traffic,
performs no runtime I/O and changes no priority, qualification, timer, replay
or wire rule. Existing public structures remain unchanged. Providers without
this optional symbol must expose observation unavailable; integrations qualify
and explicitly bind the getter rather than infer it from a provider name.
