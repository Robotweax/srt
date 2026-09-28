# Idle channel readiness

A channel with no runnable transport work can wait for UDP readability and its
next protocol deadline instead of scheduling a task every 2 ms. Empty listener
and setup channels need no protocol timer. Established quiescent connections
retain full/lite ACK, keepalive and peer-idle deadlines. DATA, ACK and handshake
wire formats, negotiated capabilities and the public SRT C ABI are unchanged.

## Eligibility

Long waits apply only after a complete bounded channel-poll round confirms that
every connection is safe to wait. A connection is safe to wait whenever it has
nothing to send right now: every remaining duty is then either driven by an
inbound datagram, which the readiness watcher reports, or by a deadline the
connection states exactly — the ACK/NAK/keepalive cadence, the TSBPD delivery
time while the channel is not yet readable, receiver too-late and peer drop
deadlines, the retransmission timeout, the sender too-late drop and message
TTL, the KMREQ retry and the peer-idle timeout. Buffered but undeliverable
data, unacknowledged packets in flight and outstanding key material therefore
no longer keep a channel on the short polling path. Pacing, retransmission,
FEC, UDP-backpressure retries and the 64-item fairness budgets keep their
existing behavior.

Two rules keep this from costing more than the fixed cadence did. A socket
whose receive slice collects more than one datagram is busy: input arrives
faster than the polling cadence, and collecting it in timed batches is cheaper
than a wake-up per datagram, so such a channel keeps polling until a slice
finds at most one datagram. And timer duties keep the polling cadence as their
granularity: a parked channel wakes for the earliest deadline among its
connections, but never sooner than the cadence, so many connections with
staggered 10 ms timers on one shared socket do not wake it for each of them.
Pacing deadlines stay exact. A readable channel does not wake for the delivery
time of every further message; blocking readers, epoll waiters and group
receives time those deliveries themselves.

Connection registration and setup promotion wake parked channels. New send work
cancels a later timer or records a continuation while a task is active.
Application reads also notify the channel after releasing the runtime mutex,
so a newly available receive window is promptly advertised. Reads reuse an
already scheduled poll due within the active 2 ms interval. They do not restart
that deadline. A read racing with an active channel poll retains a follow-up
within that interval instead of forcing an immediate task per packet. Parked
channels and later protocol timers are still woken; outbound work remains
immediate. Peer-idle expiration
keeps its strict greater-than comparison, including the final microsecond.

## Shared readiness watcher

Each runtime scheduler lazily owns one bounded readiness watcher thread. On
Linux it waits on an `epoll` queue, on 64-bit Apple and BSD hosts on a
`kqueue`, and elsewhere (including 32-bit BSD and Windows) on `poll` or
`WSAPoll`. The normal
process-wide scheduler shares that thread across its channels. There is no
per-connection thread. Protocol processing remains on the existing scheduler
shards.

Each registration has a generation token and produces one callback per arm.
Level-triggered rearming also sees packets that arrived between the final
receive attempt and the arm operation. Callbacks retain a weak channel context;
cancellation rejects stale slot generations, and channel teardown cancels the
registration before closing its borrowed UDP descriptor. Already claimed
callbacks may finish; they cannot resurrect a destroyed channel. Callbacks run
outside the watcher's registry lock.

With `epoll` and `kqueue` the kernel holds the interest set. Arming is one
one-shot control call (`EPOLLONESHOT`, `EV_DISPATCH`) from the arming thread;
it neither wakes the watcher nor rebuilds a descriptor set, and a wait already
in progress observes the new interest. Delivery disables the interest until the
next arm, so each arm still yields at most one callback. Cancellation removes
the interest under the registry lock before returning, so the caller may close
the native socket at once; an event already fetched for a reused slot is
discarded by its generation.

With `poll`, cancellation also waits for an in-progress native poll snapshot to
retire before the caller closes the socket. A concurrent `poll` can retain the
native socket after `close` returns and briefly keep its UDP port bound. Merely
marking the registration inactive permits a rapid listener restart to fail with
`EADDRINUSE`. Cancellation wakes the poll and waits for its recorded generation
to complete; the registration slot is not reused until then. Callbacks run after
that completion is recorded, so a callback can cancel its own registration.
Concurrent cancellation reclaims the slot only once. This changes teardown
synchronization, not packet processing, socket reuse policy or wire behavior.

The watcher uses one additional loopback UDP socket to interrupt its native
wait (on every arm and cancel with `poll`, only for stop with the kernel
queues). A shared 100 ms watchdog bounds its stop response if a wake send
fails. That is up to ten idle native waits per second for the watcher, rather
than a 2 ms task per channel. The `poll` fallback still scans bounded
registration storage per wait; it is not an O(1) kernel event queue.

Registration storage is preallocated to the scheduler's aggregate task-queue
capacity (8,192 entries with the current production defaults). If watcher
creation, registration or arming fails, the channel uses its existing timer
polling. The watcher capacity is therefore an optimization limit, not a new
connection-admission limit. Fatal native-wait failure wakes armed channels so
they can fall back. Scheduler shutdown stops the watcher before retiring its
workers.

## Focused validation and measurement

Canceling a scheduler timer removes it and releases its context immediately,
without separately signaling the worker. Removing an entry cannot advance the
earliest remaining deadline. A worker already waiting for the removed deadline
can wake early at that deadline; new queue entries, new timers and shutdown
retain their own notifications. This avoids a premature wake in the frequent
cancel-then-submit path while keeping send work immediate. No UDP receive gate,
protocol deadline, timer capacity or affinity changes are involved. Focused
scheduler cases cover cancellation of the last/head timer, slot reuse, new work,
remaining deadlines and shutdown. CPU benefit requires separate measurement.

The native test filters `socket_readiness`, `compat_idle_readiness`,
`control_timer`, `compat_runtime`, `compat_dispatcher` and
`compat_channel_fairness` cover the affected paths. Additional targeted scheduler,
setup-route and active-cleanup cases cover ownership transitions. New cases
exercise IPv4/IPv6 readiness, one-shot rearming, stale tokens, callback teardown,
empty parking, incoming packets, new sends, registration, keepalive and timeout
deadlines, reopened receive windows and capacity fallback.
The native port-rebind regression covers immediate close/rebind after watch
cancellation for IPv4 and IPv6. A separate concurrent-cancellation case covers
slot reuse. GStreamer's repeated listener start/stop probe exercises the original
failure through the public C API; these are lifecycle checks, not throughput
benchmarks.

A short local Release diagnostic compares the same binary with its existing
2 ms test polling override versus readiness enabled, using 64 bound channels,
with and without an idle runtime per channel. It records completed scheduler
tasks and process CPU time over approximately 250 ms after settling, with four
trials per mode in alternating order. Keepalive is not due during that interval.
This isolates short idle intervals; it does not measure sustained traffic,
end-to-end latency, recovery throughput or long-term power consumption.

The initial native and ASan/UBSan validation used macOS ARM64. Linux/Windows,
installation, third-party integrations and final Haivision SRT 1.5.7 comparisons
remain part of final qualification. No interop certification is inferred from
local tests. macOS LeakSanitizer is unavailable in this toolchain; ASan/UBSan
results do not claim leak-detection coverage.
