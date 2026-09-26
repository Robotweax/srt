# Incremental SRT epoll readiness

User-socket epoll subscriptions cache their latest readiness snapshot. Normal
established-runtime changes notify only observers subscribed to that runtime.
Repeated notifications for the same subscription coalesce until the next query.
An unchanged empty `srt_epoll_uwait` no longer scans every subscribed socket.

## Ownership and collection

Each runtime owns a small readiness source independent of its packet buffers.
A subscription retains that source, not the runtime or socket record. Closing a
socket can therefore release transport storage while its epoll registration
remains present and terminal. Source lists and per-poll dirty lists are
intrusive; notification does not allocate queue nodes. There is at most one
dirty-list entry and one delivery-deadline heap entry per subscription.

The poll record owns subscription/watch storage and reserves scratch capacity
on registration, growing geometrically. Collection takes the dirty batch under
the observer lock, releases that lock, and only then queries socket/runtime
state. A source is attached before the final readiness query to cover changes
between lookup and subscription. Notifications during collection remain pending
for the next pass. A failed collection invalidates the cache for a complete
retry. Poll mutation and collection retain their existing record mutex.

The lock order for notification is source or observer-registry lock, followed
by the observer lock. Notification never takes a poll-record, socket or runtime
lock. Unregistration unlinks the source before destroying its watch; queued
notifications are removed under the observer lock. Release and final cleanup
wake the affected poll's waiters. Legacy non-epoll waiters retain the global
generation/condition-variable signal.

## Events, deadlines and fairness

Ready subscriptions form a circular list. Delivered level-triggered events move
to its tail, so small output arrays can make progress across all ready sockets.
Results are no longer numerically sorted by handle. `srt_epoll_uwait` copies only
its bounded output prefix and consumes only returned edge/UPDATE events.

The legacy `srt_epoll_wait` path retains its existing behavior: all collected
edge bits are consumed even if its fd arrays truncate output; UPDATE is consumed
only through `uwait`. Its existing read/write counting, output validation and
timeout return conventions are unchanged. The legacy path still visits the
ready set to distribute events across separate read/write outputs.

An indexed min-heap retains pending TSBPD read deadlines. A due subscription is
queried again without requiring a packet arrival or a global notification.
Removal and mask replacement unlink both ready-list and heap membership.
Level state, peer shutdown with buffered data, and consumed one-shot peer errors
are re-evaluated after the relevant runtime notification.

## Conservative paths and remaining limits

Global connection/setup/close and group-lifecycle notifications invalidate all
poll caches. Pollers watching groups also invalidate on runtime changes until
membership-specific subscriptions are implemented. Runtimes sharing a group
TSBPD clock preserve global invalidation because drift can move another member's
deadline. These paths keep their complete readiness evaluation. Local epoll
add/update/remove/flags/release operations wake their own observer.

System/native descriptors retain their existing native polling and 10 ms idle
check interval. This change does not replace that backend or the phase-4 UDP
readiness watcher. No public SRT C ABI, wire format or negotiated feature changes.
No new thread or independent event-queue capacity limit is introduced.

For ordinary sockets, an unchanged empty query is constant work after initial
collection. A sparse update costs work for changed subscriptions, due deadlines
and returned events; heap maintenance is logarithmic in pending deadlines.
Initial/global invalidation still costs a full subscription scan, group queries
can traverse members, and runtime notification costs scale with the number of
pollers watching that source. Storage scales with registered subscriptions and
scratch high-water capacity.

## Focused evidence

The `compat_epoll` filter includes sparse-query accounting, notification
coalescing/isolation, bounded-output fairness, parallel subscription churn,
multiple TSBPD deadlines/removal and peer-error consumption regressions.
Existing group epoll, shared-clock, runtime, release/cleanup and process teardown
cases cover the affected notification and ownership paths. Native Debug and
ASan/UBSan runs on macOS ARM64 passed; LeakSanitizer is unavailable there.

A native diagnostic compares the phase-4 library with this implementation using
matching private headers for each. It measures 64/256/1,024/4,096 registrations,
one established runtime and otherwise unconnected sockets. An idle case returns
no events; a sparse case injects one DATA packet, queries readiness and reads
back its payload. Both variants verify event identity and payload/message
checksums. Four serial A-B-B-A runs retain 101 samples per configuration.

These measurements isolate epoll lookup and a synthetic packet/read cycle.
They exclude real transport scheduling, network I/O, encryption, FEC and traffic
on thousands of established connections. They do not establish network goodput
or end-to-end delivery latency. Installation, external integrations, the full
suite, other platforms and final Haivision SRT 1.5.7 qualification remain separate
final checks.
