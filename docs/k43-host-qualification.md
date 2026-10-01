# K43: many-connection host qualification

This qualification measures main at `2444b93` after the idle-wake attribution
change in #146. It uses Apple M1 Ultra (20 logical/physical cores, 64 GiB),
macOS 26.6.2, AppleClang 21.0.0, and a strict Release build with OpenSSL and
AEAD API preview enabled. The transport is unencrypted public-API loopback SRT.
It qualifies this host and workload; native Linux and Windows performance
qualification remains separate.

## Transport and CPU

`scalability_scorecard.py` used `many-socket`, `robotweax-self`, 1/8/64/256
connections, 188/1316-byte messages, 4 MiB per connection, three measured runs
after one warmup, 120 ms latency and 500 ms shutdown grace. Each side runs one
epoll application process. Caller sockets have independent UDP channels;
the listener shares one UDP channel across accepted connections. Background
processes were not profiled; the first series adds no artificial host load.

All eight short smoke cases and all 32 full runs completed with both peers
exiting successfully, verified deterministic payloads on every connection,
complete drain, and zero retransmitted packets. Medians of the three measured
runs are:

| Connections | Message bytes | Useful Mbit/s | CPU-s / useful Gbit |
| ---: | ---: | ---: | ---: |
| 1 | 188 | 11.9 | 11.726 |
| 8 | 188 | 92.7 | 7.997 |
| 64 | 188 | 276.2 | 9.424 |
| 256 | 188 | 279.3 | 9.961 |
| 1 | 1316 | 73.6 | 2.716 |
| 8 | 1316 | 558.8 | 1.497 |
| 64 | 1316 | 1565.0 | 1.557 |
| 256 | 1316 | 1509.2 | 1.800 |

CPU accounting includes both endpoint processes, establishment, drain and
cleanup. Throughput also includes caller establishment, so this is a complete
transfer score rather than an isolated steady-state rate. The byte target is
rounded up to a complete message. The 188-byte case reaches an aggregate
plateau around 64 connections; adding connections does not establish linear
throughput scaling. These are absolute measurements, not a before/after
performance claim for #146. macOS RSS/thread peaks are unavailable in this
scorecard's Linux `/proc` sampler.

A second series repeated the same 64/256 workload with four additional CPU-only
arithmetic-loop processes, using default OS placement and the same warmup and
three measured repetitions. All 16 transfers again passed integrity, drain and
exit checks with zero retransmissions. The scorecard's endpoint CPU accounting
excludes these sibling load generators.

| Connections | Message bytes | Useful Mbit/s, four loaders | CPU-s / useful Gbit |
| ---: | ---: | ---: | ---: |
| 64 | 188 | 245.7 | 10.557 |
| 256 | 188 | 235.7 | 11.527 |
| 64 | 1316 | 1416.1 | 1.730 |
| 256 | 1316 | 1390.6 | 1.972 |

This added load lowers throughput and raises endpoint CPU cost per useful bit.
The two series were sequential, without CPU affinity or thermal/frequency
control; they do not isolate every cause of the difference.

## Timer attribution

The extended `robotweax_srt_timer_wake_probe` schedules equal-deadline batches
on configurable scheduler shards and optionally performs CPU work in each
callback. Its original serial defaults and callback-entry `lateness_*` fields
are retained. Additional fields report:

- `idle_lateness_*`: deadline to the idle-wake timestamp supplied by the
  scheduler. Timers sharing one wake repeat that observation; these are
  callback-weighted samples, not independent OS wake-ups.
- `idle_to_callback_*`: supplied idle wake to callback entry, including earlier
  callbacks on that shard and dispatch overhead.
- `busy_lateness_*`: deadline to callback entry when no idle wake was supplied.
  An empty idle distribution is JSON `null`, rather than a punctuality claim.
- `callback_duration_*`: callback entry to completion of the requested CPU work,
  before batch-completion notification.

A matrix used two shards, fanout 1/8/64/256, 200 measured batches after 20
warmups, delays 200/500/1000 us, and callback work 0/50 us. In the 500-us,
50-us-work profile, p99 idle lateness was 272 us at fanout 64 and 280 us at
fanout 256. p99 idle-to-callback delay was 1558/6321 us. Long callback backlog
therefore remains distinct from the reported idle-wake delay. The native
matrix is exploratory; it sets no host-dependent pass threshold.

Separate, privately instrumented UDP runs measured 1 MiB per connection,
1/8/64/256 connections, both message sizes, and two repetitions. They recorded
actual idle wait returns once per wait, timer dequeue and callback timestamps,
channel lifecycle-lock observation, and Coarse-mode transitions. All 16 runs
passed integrity, exit and drain checks without retransmissions. No temporary
counters or runtime changes are committed, and their CPU values are excluded
from the CPU table above. Their extra clocks/atomics can affect dispatch delay.

At 64/256 connections, the Caller channels recorded no Coarse-mode entries in
either repetition for either message size. The 188-byte Caller's busy-deadline
to dequeue p99 was 885--929 us; idle-return p99 ranged from 776 to 6528 us.
These are separate distributions: busy deadlines do not provide an idle-wake
sample to the channel's monitor. Samples can include setup, drain and cleanup.
An idle wait-return timestamp includes OS scheduling and reacquiring the shard
mutex; it does not isolate hardware timer resolution.

Some Listener runs entered Coarse mode after genuine late idle observations.
For 64 x 188 bytes, the flag occupied 79--92% of the channel lifetime, but only
one pacing probe occurred per run. A latched flag does not by itself prove
continuous immediate resubmission or CPU spin: fallback applies to future
sub-millisecond work. Mode recovery on a genuinely coarse host, rate-controlled
traffic and readiness arrival during an active probe remain qualification
limits. The existing policy preserves bounded probes across send notifications;
this measurement does not claim that those notifications cancel them.

A second instrumented series repeated the 64/256 cases twice with four CPU
loaders. All eight runs again passed without retransmissions; Caller channels
still recorded zero Coarse-mode entries. Busy-deadline to dequeue p99 for the
188-byte Caller was 923--946 us. The 256 x 188-byte actual idle-return p99 fell
in the histogram's overflow bucket (at least 10000 us), so it has no exact
quantile value. These observations include sparse idle waits during setup and
cleanup; they are not per-packet latency distributions. Genuine late idle
observations can still latch a Listener's mode flag under added load.

## Reproduction

Build with `ROBOTWEAX_SRT_BUILD_BENCHMARKS=ON`, Release and strict warnings:

```sh
python3 benchmarks/scalability_scorecard.py \
  --topology many-socket --profile robotweax-self \
  --robotweax-peer build/robotweax_srt_scalability_peer \
  --robotweax-revision 2444b93 \
  --connections 1,8,64,256 --message-sizes 188,1316 \
  --bytes-per-connection 4194304 --iterations 3 --warmup 1 \
  --timeout-seconds 120 --output qualification.json
build/robotweax_srt_timer_wake_probe \
  --shards 2 --fanout 256 --rounds 200 --warmup 20 --callback-work-us 50
```

Repeat the timer command with fanout 1/8/64/256 and work 0/50. Fanout describes
synthetic scheduler timers; only the scorecard counts established connections.
The probe bounds shards to 64, fanout to 256, rounds to 10000, warmup to 1000,
callback work to 1000 us, and retained measured samples to one million. A failed
submission or batch timeout fails the command.

The private transport instrumentation is diagnostic evidence, not a supported
library API or a reproducible public counter interface. Repeating its detailed
Coarse/dispatch measurements requires separately reviewed instrumentation.
