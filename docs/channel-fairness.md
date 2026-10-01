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
slice does not restart an earlier wait. On Linux and macOS, every future
deadline, including a sub-millisecond pacing slot, is a scheduler timer wait;
only a deadline that has already passed resumes the channel at once. On those
platforms the pacer keeps a bounded schedule credit (1 ms): a send that is late
by at most the credit, for example
after a late timer wake-up, keeps the ideal schedule and may catch up the
missed slots back to back, so timer latency does not lower the configured
rate. After a longer pause the schedule restarts at the send time, and the
first packet of a connection starts a fresh schedule, so a pause is not
followed by a burst. If a host's timer wake-ups repeatedly arrive later than
the credit, the channel falls back to resubmitting sub-millisecond deadlines.
While that fallback is active, it permits one real sub-millisecond timer wait
about every 100 ms. These probes measure current wake-up accuracy even if the
sender remains continuously busy; 16 punctual wakes return the channel to
normal timer waits. An active probe is preserved across new send notifications.
The measurement uses the time the idle scheduler wait returned, rather than
the later channel callback entry. Timers already due at the same wake share
that observation; intervening callbacks and channel locks do not inflate it.
If a deadline becomes due while the shard executes other work, the channel
clears the fallback and uses timer waits again. Immediate resubmissions would
add work to that busy shard. An idle-wait observation still includes operating
system scheduling delay and is not a hardware timer-resolution measurement.
Each probe can delay one pacing continuation on a coarse host; the pacer catches
up within its 1 ms credit and restarts its schedule after a longer delay.
Windows and other platforms retain immediate continuations for deadlines under
1 ms and start the next pacing interval at the actual send time. Protocol timer
rules are unchanged. The
fairness change adds no
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

See [K43 host qualification](k43-host-qualification.md) for native macOS
64/256-connection CPU/transport evidence and the extended scheduler timer probe.
