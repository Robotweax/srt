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

Outgoing asynchronous caller and rendezvous completion callbacks can reuse a
process-wide idle worker. At most four idle workers are retained. If all workers
are occupied, another callback gets a new worker; callbacks are not queued behind
occupied workers. This preserves progress when a callback closes another socket
and waits for that socket's completion callback. Slow application callbacks can
therefore still increase the number of active threads; they must return promptly.

Do not rely on a distinct thread identity for each invocation. The SRT
thread-local last-error record is cleared before a reused worker invokes a
callback. An external close still waits for its socket's callback to return;
self-close and final cleanup from a callback remain supported. Final cleanup
retires the worker cache, and a later startup obtains a fresh generation.
The old runtime services are retired before cached workers are joined outside
the lifecycle lock. Application thread-local destructors can therefore enter a
new startup/cleanup scope without waiting on the thread that is joining them.

Worker reuse does not change the existing resource-exhaustion fallback: if both
cache submission and the ordinary callback-thread creation fail, the completion
callback can still run inline. Applications must not depend on an exact thread
identity or use indefinitely blocking callbacks.
