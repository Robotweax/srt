# Same-host UDP group throughput diagnostic

`benchmarks/group_throughput.py` builds the same public-API peer source against
two clean Robotweax revisions and runs serial before/after measurements for
Broadcast and Backup. This fills the group-workload gap in the individual-socket
[throughput scorecard](throughput-profiling.md). It is a fixed-workload diagnostic,
not a general network capacity or cross-platform certification.

## Reproduce

Use clean worktrees for both revisions, a POSIX/macOS host with CMake, a C++17
compiler, OpenSSL 3 and `pkg-config`, and a quiet host. No dependencies are
installed, reference repository downloaded, or kernel settings changed.

```sh
python3 benchmarks/group_throughput.py \
  --before-source /absolute/path/to/before \
  --after-source /absolute/path/to/after \
  --members 2,8,16,32,64 --sizes 1316 \
  --messages 10000 --iterations 3 \
  --output-directory /absolute/path/to/new-results
```

The output directory must not exist. Builds use fresh Release static libraries,
the matching revision's public headers, `-O2` peer compilation and the same
OpenSSL dependency. `build-manifest.json` records source commits, commands,
platform, harness/library/program SHA-256 identities, compiler and dependency
logs; CMake caches and loader logs accompany it. Builds do not share a cache.
To repeat measurements without rebuilding, pass `--prepared-directory` pointing
to those artifacts and a different new `--output-directory`. Their platform,
harness, program and library hashes are checked before reuse.

`report.json` is saved after every run, including failed runs. Each peer's stdout
and stderr are retained. A failed transfer makes the command exit unsuccessfully
and prevents its successful repeats from qualifying the whole case. Treat
incomplete reports, any unqualified run, host load or short measurement spans as
limitations, not speed evidence. Keep build artifacts local; publish reports and
summaries rather than binaries.

## Workload and correctness

Each run creates separate sender and receiver processes, one application thread
on each side, and real UDP sockets on `127.0.0.1`. The listener reports its actual
OS-assigned port. All requested group members must remain connected for 120 ms
before the controller releases either peer. Backup connects its preferred member
first, then its lower-weight standbys; it does not simulate failover. Broadcast
uses one path per member to the same mirrored receiving group.

A 256-message warmup is received and every member's sending buffer drained before
statistics are baselined on both sides. A final start barrier prevents measured
DATA from racing those baseline snapshots. Messages use 120 ms TSBPD latency, disabled late-packet
drop and no encryption. The sender polls member send buffers between batches of
at most 32 messages, keeping at most 64 pending packets on any member. This bounds
host bursts and preserves healthy Broadcast copies. The polling is public API
application overhead and scales with member count; the results include it.

Every message contains its expected sequence, a same-host monotonic microsecond
source timestamp and deterministic bytes at every remaining position. The receiver
checks length, order, each deterministic byte and timestamp plausibility. Both
peers independently SHA-256 the entire measured stream, including timestamps.
The controller requires identical digests, counts and byte totals. Member loss or
replacement, transport drops, missing Broadcast copies, timeouts and abnormal
peer exits invalidate the run. Both peers remain alive until both results have
been collected. There is no silent data-loss or lower-member-count success.

## Read the metrics

- `useful_mbps` counts each delivered application byte once, divided by the longer
  peer measurement duration. Receiver time includes initial TSBPD delay; sender
  time includes final acknowledgements and sending-buffer drain.
- `delivery_span_mbps` uses bytes after the first delivered message divided by
  the interval between first and last delivery. It excludes initial TSBPD delay,
  but does not establish a sustained maximum rate. Inspect the span and increase
  workload length when it is too short for the intended comparison.
- Receiver latency percentiles measure same-host source-to-delivery time, including
  TSBPD. Sender percentiles measure the send call, excluding payload construction.
- `cpu_seconds` sums process user/system CPU over the measured phases, including
  library workers, verification, SHA-256, window queries and final sending drain.
  Setup, warmup, summary generation and teardown are excluded.
- Per-member transport counters distinguish sent/received bytes and packets,
  unique sent packets, retransmissions and drops. Transport bytes follow the
  public stats convention, including SRT/UDP overhead; they are not application
  goodput or Ethernet capture totals. Broadcast requires exactly one unique sent
  packet per measured message on every member. Backup may have idle members.

Before/after ordering alternates by repeat. Summaries retain each variant's
median and full observed range, with every raw run available. No speed threshold
is used in CI. The opt-in CMake benchmark build adds a real UDP integrity smoke
for 1, 16, 17 and 64 members in both modes; this is not a capacity test.

Any follow-up production optimization requires profiling to separate transport,
application polling and payload verification costs. An isolated allocation or
metadata improvement is not automatically an end-to-end throughput improvement.

## macOS qualification, 2026-10-01

On Apple M1 Ultra (20 logical CPUs, 64 GiB RAM), 60 serial runs covered both modes,
2/8/16/32/64 members, 10,000 messages of 1,316 bytes per run and three repeats per
variant. Both peers used the same Release toolchain and OpenSSL 3.6.3. Baseline
`89cf10dcf0ade0481ddc132c1849cb9779e6ddad` precedes PR #166; candidate diagnostic
commit `86a9b97` has the production sources of main
`fe0daf65be0fc0118d40bfdfef03b584543b7cf6`. The only production diff is #166's
send-result bookkeeping change. These measurements do not attribute cumulative
gains to earlier PF20 changes.

All 60 runs passed exact payload/SHA-256 and membership/copy checks. There were
no reported transport drops or DATA retransmissions. Each Broadcast sender sent
exactly the member count times 10,000 unique DATA packets; Backup sent 10,000
unique packets across its members. Measured receiver durations ranged from
0.525 to 8.733 seconds. The application window and verification overhead remain
part of this workload; no other builds or tests ran concurrently with this series.

The following values are medians of three runs. CPU is sender plus receiver
process CPU seconds per fixed workload; it is not a CPU utilization percentage.

| Mode | Members | Before useful Mbit/s | After useful Mbit/s | After CPU seconds | After delivery p99 ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| Broadcast | 2 | 119.87 | 119.45 | 0.368 | 122.41 |
| Broadcast | 8 | 89.10 | 88.49 | 1.275 | 122.62 |
| Broadcast | 16 | 66.70 | 67.21 | 3.636 | 122.08 |
| Broadcast | 32 | 24.94 | 25.00 | 13.486 | 124.63 |
| Broadcast | 64 | 12.26 | 12.11 | 28.063 | 129.28 |
| Backup | 2 | 194.36 | 194.00 | 0.212 | 121.78 |
| Backup | 8 | 188.70 | 193.75 | 0.253 | 121.73 |
| Backup | 16 | 193.30 | 193.60 | 0.316 | 121.68 |
| Backup | 32 | 186.19 | 191.00 | 0.455 | 121.68 |
| Backup | 64 | 186.89 | 183.98 | 0.766 | 122.35 |

Median throughput changes range from -1.6% to +2.7% across these profiles, with
mixed signs and overlapping observed ranges in most cases. This provides no
consistent end-to-end throughput gain attributable to #166. It preserves the
previously demonstrated allocation benefit without promoting it to a bandwidth
claim. Large Broadcast groups consume substantially more CPU and useful
throughput falls as copies increase; Backup's CPU also rises with idle membership.
Profiling must separate required copy work, transport scheduling/receive scans and
this application's member-buffer polling before choosing another optimization.

This is macOS loopback qualification for a healthy, unencrypted fixed workload.
Linux/Windows capacity, real independent paths, encrypted throughput, outages,
long sustained transfers and alternative application windows remain open.
An exploratory run's startup failures came from the diagnostic's two-step member
size/fetch race; the final peer reserves its supported maximum before reading
joining members. Those failed runs are retained separately and excluded from the
qualified series. A further start barrier places both stats baselines before the
first measured DATA packet.
