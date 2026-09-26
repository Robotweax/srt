# Shared-channel scheduling fairness

A shared UDP listener can own many established connections on one scheduler
shard. Its connection-poll phase now yields after either 64 connection polls or
64 UDP send attempts, whichever is reached first. Previously it visited every
connection and each connection could submit its own batch, so a busy listener's
turn grew with its connection count.

## Work accounting and continuation

The send budget is shared by connection polls in the channel slice. It counts
attempts, including `would_block`, across DATA, retransmissions, FEC, queued
retries and timer-generated controls. A connection cannot spend a new budget
merely because it has switched from queued controls to new DATA.

Exhausting a scheduler budget is different from UDP backpressure. Remaining work
is resumed by an immediate scheduler continuation. It does not acquire the 1 ms
UDP-retry delay. A control action already prepared at the budget boundary uses
the existing immutable pending-datagram queue. New DATA/FEC preparation stops
when the budget is exhausted.

The receive phase retains its separate 64-datagram budget. Immediate responses
while dispatching received packets, initial handshake setup, and application-
initiated sends such as close are outside the connection-poll send budget.
These are operation bounds, not a hard wall-clock deadline or a complete
channel-wide syscall cap.

A receive slice requests an immediate continuation when it consumes all 64
receive slots, because unread UDP datagrams may remain. If `would_block` ends
the slice first, the channel preserves the connection-poll result instead of
forcing another empty receive and route sweep just because some DATA or control
was received. Malformed and truncated datagrams still consume the receive budget.
Pending sends, unfinished connection rounds and due protocol work retain their
immediate continuations. Later arrivals use the existing readiness notification
or bounded timer fallback; no pacing or protocol deadline is extended.

## Channel placement

Default scheduler placement is assigned when a channel starts, under its
lifecycle lock. Repeated starts of an already running channel preserve its
owner and do not advance placement for the next channel. Setup and established
runtime installation may both start the same channel. Independent channels
therefore retain round-robin placement across the fixed scheduler shards;
connections sharing one channel still have a single protocol owner. Explicit
scheduler affinities remain unchanged.

## Route ownership and timers

Routes form a circular poll order within the existing hash table. Registration
appends behind waiting routes, and removal unlinks the route and advances the
cursor when necessary. Hash-table rehash preserves node addresses; no hash-table
iterator is retained across mutations and no new list allocation is required.
Lookup remains through the hash table.

The channel takes a shared runtime reference and advances the cursor under the
routing mutex, then releases that mutex before polling the runtime. Concurrent
unregistration cannot destroy an in-progress runtime. Runtime state remains
protected by its existing mutex; the scheduler still has one owner per channel.
The normal socket-close path closes the runtime before unregistering it.

An unfinished round resumes at the saved cursor. A round visits the connection
count captured at its start; membership changes can cause a surviving route to
be revisited or a new route to wait until the next round. Work per slice stays
bounded. Once a round of idle connections finishes, the channel returns to its
idle wait rather than continuously scheduling full sweeps.

Pacing, UDP-retry and idle waits are accumulated as absolute scheduler-clock
deadlines across slices. Completion subtracts the time already spent, so a later
slice does not restart an earlier wait. Existing sub-millisecond cooperative
pacing and protocol timer rules remain in effect. The fairness change adds no
worker thread, wire format, public C-ABI field, negotiated feature or buffer
setting. The subsequent [idle-readiness optimization](idle-readiness.md) adds one
shared readiness watcher and lets eligible quiet channels wait for events.

## Focused validation

```sh
build/robotweax_srt_tests compat_channel_receive_slice
build/robotweax_srt_tests fairness
build/robotweax_srt_tests compat_runtime
build/robotweax_srt_tests compat_channel
build/robotweax_srt_tests compat_dispatcher
build/robotweax_srt_tests compat_setup_route_promotion
build/robotweax_srt_tests compat_listener_setup_route
```

The fairness cases cover busy routes sharing a send budget, bounded idle rounds,
deadlines across continuations, cursor erasure and hash-table growth, routing
changes from a send hook, a blocked route alongside a healthy route, and queued
controls resuming without a synthetic backpressure delay. Run the fairness
cases with address/undefined-behavior sanitizers when changing cursor ownership.

A native diagnostic can compare maximum channel-slice duration and total CPU
for the same due packets before and after this change. Such a diagnostic omits
real UDP, scheduler dispatch and peer ACKs: its slice duration bounds a source
of worker occupancy, but is neither measured application latency nor network
throughput. End-to-end multi-connection, platform and reference-interoperability
qualification is a separate final gate.
