# Idle channel readiness

A channel with no runnable transport work can wait for UDP readability and its
next protocol deadline instead of scheduling a task every 2 ms. Empty listener
and setup channels need no protocol timer. Established quiescent connections
retain full/lite ACK, keepalive and peer-idle deadlines. DATA, ACK and handshake
wire formats, negotiated capabilities and the public SRT C ABI are unchanged.

## Conservative eligibility

Long waits apply only after a complete bounded channel-poll round confirms that
every connection is safe to wait. Send buffers, occupied receive buffers,
receive losses, pending peer drops, delivery deadlines and outstanding key
material retain the existing short polling path. Pacing, retransmission, FEC,
UDP-backpressure retries and the 64-item fairness budgets keep their existing
behavior. This change primarily benefits quiet channels; a busy connection on a
shared listener still keeps that channel on the active scheduling path.

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

Each runtime scheduler lazily owns one bounded readiness watcher thread using
`poll` on POSIX or `WSAPoll` on Windows. The normal process-wide scheduler shares
that thread across its channels. There is no per-connection thread. Protocol
processing remains on the existing scheduler shards.

Each registration has a generation token and produces one callback per arm.
Level-triggered rearming also sees packets that arrived between the final
receive attempt and the arm operation. Callbacks retain a weak channel context;
cancellation rejects stale slot generations, and channel teardown cancels the
registration before closing its borrowed UDP descriptor. Already claimed
callbacks may finish; they cannot resurrect a destroyed channel. Callbacks run
outside the watcher's registry lock.

The watcher uses one additional loopback UDP socket to interrupt its native
wait. A shared 100 ms watchdog bounds its stop response if a wake send fails.
That is up to ten idle native waits per second for the watcher, rather than a
2 ms task per channel. Descriptor collection still scans bounded registration
storage; this portable implementation is not an O(1) kernel event queue.

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
