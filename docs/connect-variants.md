# Connect variants

Robotweax SRT implements the two Haivision SRT v1.5.7 connection entry points
that extend `srt_connect` without introducing a second connection state
machine.

## Source-bound connect

`srt_connect_bind(socket, source, target, size)` validates both socket
addresses and their shared IPv4 or IPv6 family before changing socket state.
For an individual socket it then performs the compatible bind-then-connect
sequence. A zero source port asks the operating system to select the port;
the source address, scope, native options, shared-binding rules, and resulting
`srt_getsockname` observation use the normal binding implementation.

For a Broadcast or Backup group the same call prepares one endpoint and uses
the regular managed-member connection path. The return value is therefore `0`
for an individual socket and the created member socket for a group, matching
the v1.5.7 contract. Family mismatches and malformed addresses fail before a
socket is bound or a group member is allocated.

## Developer forced-ISN connect

`srt_connect_debug(socket, target, size, forced_isn)` follows the normal
connection path, including blocking/nonblocking operation, callbacks, epoll,
encryption, packet filters, congestion control, and Rendezvous. `-1` retains
the socket's normal generated initial sequence number. A nonnegative value is
the exact 31-bit ISN advertised by the handshake and used by the transport.

The function is intended for deterministic tests and unusual experiments, not
as an application-level sequencing control. Values below `-1` are rejected.
When the handle is a group, the parameter is ignored because the group owns one
common ISN for all members.

The forced value enters only the compatibility handshake configuration. It is
not a mutable global, a socket option, or a test hook in the native protocol
core.

## Completion callback workers

Outgoing asynchronous caller and rendezvous completion callbacks share a bounded
pool of at most 64 workers per runtime generation. A connection with a callback
reserves its worker before the asynchronous handshake is admitted. If all slots
are occupied by pending handshakes or running callbacks, `srt_connect` returns
`SRT_ERROR` with `SRT_ENOBUF`; no completion callback is promised for that rejected
start. Close the failed socket and retry later with a new socket.

Accepted callbacks never queue behind other callbacks and never fall back to
running on a protocol shard or an extra unbounded thread. This preserves progress
when a callback closes another admitted socket and waits for its callback. A
callback that starts additional connections must handle admission failure.
Connections without completion callbacks do not reserve a worker.

Workers are retained until final cleanup, including idle workers, so application
thread-local destruction cannot bypass the thread bound. Do not rely on a distinct
thread identity for each invocation. The SRT thread-local last-error record is
cleared before reuse. An external close still waits for its callback; self-close
and final cleanup from a callback remain supported. Old runtime services are
retired before workers are joined outside the lifecycle lock, so application
thread-local destructors can enter a fresh startup/cleanup scope.
