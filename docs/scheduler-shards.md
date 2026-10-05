# Scheduler shards and staged scaling

## Process configuration

Set `ROBOTWEAX_SRT_SCHEDULER_SHARDS` before the first runtime scheduler
acquisition, normally triggered by network activity. An unset value preserves
the default of **2** workers. The setting accepts decimal integers from **1 to
64**, including leading zeros; whitespace, signs, trailing text, empty values
and out-of-range values are invalid. Invalid configuration returns no scheduler
and fails the operation that needs to start it; it never silently selects a
different worker count. `srt_startup()` alone need not acquire the scheduler.
On Windows, removing the variable is the way to select the default.

```sh
ROBOTWEAX_SRT_SCHEDULER_SHARDS=4 ./your-srt-application
```

The setting belongs to the process, not a listener or connection. It introduces
no public socket option, C ABI or wire change. Once a scheduler generation has
started, it keeps its configuration even if the environment changes. Only final
`srt_cleanup()` after all startup references are released stops that generation;
a subsequent acquisition reads the setting again. Configure the environment
before application threads start. Concurrent environment mutation is unsupported.
An invalid setting does not cache a failed generation, so a corrected setting
can be used by a subsequent acquisition.

Each worker preallocates 4,096 task slots and 4,096 timer slots. Total scheduler
storage and worker stacks grow with the requested count. The default remains
conservative; CPU count does not automatically choose workers or account for
container quotas. Connection count alone is not a reason to raise the setting.

## What scales today

Each UDP channel receives a fixed affinity, assigned in channel creation order
modulo the shard count. All receive dispatch and connection polling for one
channel remain on its worker. Multiple independently busy UDP sockets can use
more workers, but one busy shared listener still performs protocol work on one
worker. Uneven channel traffic can leave shards unevenly loaded. The channel's
send mutex also serializes its UDP submissions.

Application `srt_sendmsg2` enqueues under the connection mutex. Encryption and
transport sends occur later in runtime work. The channel-wide send mutex is a
separate lock around UDP submission, not the application's send-buffer lock.

There is no portable per-process Gbit/s ceiling established by this change.
The supplied server observations at 696 Mbit/s and 44% hottest-thread CPU imply
an approximate 1.5 Gbit/s linear extrapolation on that host. They do not establish
a measured saturation point or prove that encryption dominates its hot thread.

## Reproducible first-stage comparison

Use separate matching Release builds for the baseline and candidate. Record
source revisions, compiler, crypto backend, host CPU, CPU quota/affinity and UDP
limits. Do not mix private headers or archives across revisions. The baseline
before this configuration always uses two shards, regardless of the variable.

Build the public API peer as described in [Performance](performance.md). The
candidate scorecard runner can launch either revision's matching peer:

```sh
./tools/python benchmarks/scalability_scorecard.py \
  --topology many-socket --profile robotweax-self \
  --robotweax-peer /absolute/baseline-build/robotweax_srt_scalability_peer \
  --robotweax-revision BASELINE_COMMIT --scheduler-shards 2 \
  --connections 1,8,64 --message-sizes 1316 \
  --warmup 1 --iterations 3 --output baseline.json

for shards in 1 2 4 8; do
  ./tools/python benchmarks/scalability_scorecard.py \
    --topology many-socket --profile robotweax-self \
    --robotweax-peer /absolute/candidate-build/robotweax_srt_scalability_peer \
    --robotweax-revision CANDIDATE_COMMIT --scheduler-shards "$shards" \
    --connections 1,8,64 --message-sizes 1316 \
    --warmup 1 --iterations 3 --output "candidate-$shards.json"
done
```

The runner explicitly sets the requested value only for Robotweax peers and
removes it for reference peers, so ambient environment settings cannot change
the comparison. JSON records the **requested** count; an older library may
ignore it. This scorecard uses unencrypted integrity-checked transfers, not an
encrypted relay workload. The caller owns multiple UDP channels; the accepting
listener shares one channel. It can demonstrate functional many-channel scaling
and preserve a single-listener control, but does not establish relay fan-out
capacity. Completion percentiles are not per-packet delivery latency.

Repeat comparisons serially in alternating baseline/candidate order on a quiet
host. Retain every report, including failed runs; do not select only fast runs.
Local loopback results are functional evidence, not production capacity claims.

## Linux relay qualification pending

The external server/probe workload must be archived with source revisions and
exact build commands before using the original report as an acceptance test.
A Linux target and the external probe are not included in this repository.
Prepare that workload separately; do not substitute the scorecard above for it.

Run both shared-listener and multiple-listener topologies with 1/2/4/8 shards,
64 subscribers, 1,316-byte messages, clear traffic and AES-256. Capture idle,
steady state and overload separately. Use the same offered traffic and settings
for baseline and candidate; record actual delivered payload goodput, per-thread
user/system CPU, CPU seconds per delivered Gbit, RSS, thread count, packet
latency p50/p99/p99.9, retransmits, subscriber receive loss, relay-queue drops and
kernel UDP error deltas. CPU profiles must distinguish crypto, syscalls, polling,
copying and lock waits. Linux `perf`/per-thread process sampling are observers,
not grounds for changing host limits silently.

64 subscribers at 4,000 packets/s yield only **2.695 Gbit/s payload**. Use at least
4,453 packets/s per subscriber to offer 3 Gbit/s, and enough additional headroom
to measure a delivered 3 Gbit/s target. Count payload bytes separately from wire
bytes. Duration, warmup and repetitions must be fixed before running. A lower
hottest-thread CPU is diagnostic; delivered goodput and CPU cost are the primary
outcomes. Do not require connection sharding to improve single-listener traffic
before it exists.

Also inject loss, reordering, local UDP backpressure and concurrent closes.
Interop, encrypted rotation, AEAD, group, FEC, file, lifecycle and TSan runs must
remain clean. Deterministic wake-up/order tests supplement race detection.

## Packet-cost experiments (D)

Profile first, then compare each change independently with the default shard
count: syscall batching, clock sampling and removal of redundant initialization.
A receive slice currently allocates one zeroed 1,500-byte buffer per slice, not
per received datagram. DATA encoding creates a zeroed buffer per send. Check
optimized assembly before assuming either clear remains a runtime cost.

For `recvmmsg`, preserve truncation, transient ICMP errors, peer routing and the
64-datagram slice budget. For `sendmmsg`, commit only the successfully submitted
prefix; preserve the remaining datagrams, pacing and retry accounting. Batching
must not release future pacing slots early. Reusing one clock snapshot must not
shift deadlines after a long slice. Removing buffer initialization requires proof
that the exact submitted prefix is written in clear, CTR, GCM, FEC and retry
paths. No numerical improvement is claimed until an isolated comparison passes.

## Connection-affinity investigation (B)

This is a design gate, not a production feature in the configuration change.
Prefer one owner shard per established connection. The UDP channel continues
receiving and resolving routes, then forwards owned packet bytes to that owner.
The owner processes DATA and controls, pacing, crypto, retransmission, KM and
connection timers in serial order. Avoid direct receive-side mutation on a second
shard merely because a connection mutex exists.

Before a limited transport prototype, specify:

- Bounded per-connection inboxes and a bounded shared memory budget, including
  control admission and behavior under full queues. Pending work must be
  coalesced; queue exhaustion must not silently lose the only wake-up.
- Ownership of copied datagram bytes, route generation checks, connection close,
  timer cancellation, callbacks and scheduler cleanup. Old queued work cannot
  reach a reused socket ID or resurrect a closed connection.
- Ordering of ACK/NAK, DATA, KM and shutdown relative to sends and application
  operations. Affinity alone only orders scheduler execution, not competing
  submissions or current synchronous application mutations.
- Per-connection fairness plus a channel-wide send/byte budget. Moving the old
  64-attempt channel budget to every connection would multiply burst size.
- The remaining channel send lock and kernel socket serialization. Measure
  contention before weakening the send-hook/socket lifetime protection.
- A lock/ownership map for route lookup, inbox publication, connection state,
  send submission and readiness. Current route polling releases the route mutex
  before protocol work; preserve that property.

The first transport spike must stay off by default and compare one channel with
1/8/64 connections against the baseline. Instrument inbox high-water marks,
rejections, worker distribution, queue delay and close completion. Test paused
workers, reordered controls, queue exhaustion, key rotation and close/reopen
before running the Linux relay matrix. Production integration depends on these
results. Crypto-only offload (C) is an alternative only if profiles justify it;
it still needs pinned key/IV lifetime, bounded queues and ordered completion.


## Persistent service notification publication

A producer sets a service slot's pending bit and calls `notify_one` while
holding that shard's mutex. A later producer observing the bit under the same
mutex may coalesce its notification: the earlier notification has already been
published before the mutex becomes available. Merely setting pending and then
notifying after unlocking would leave a paused publisher gap.

The shard worker can also set pending when a service deadline becomes due.
That worker is already active and checks pending before returning to its wait,
so this origin needs no additional producer notification. Clearing pending for
execution, release and stop uses the same shard mutex. A producer arriving after
execution clears pending publishes a fresh notification even while the callback
is running. Coalescing remains per slot; another slot on the same shard still
publishes its own first notification. Token scope and generation validation
precedes both paths.

`service_wakes` continues to count accepted calls and `service_coalesced` counts
calls finding pending work. These counters do not count kernel wakeups. Ordinary
queue notifications, service timer registration and shutdown notifications retain
their existing behavior. No ingress allowance, poll window or deadline changes.
