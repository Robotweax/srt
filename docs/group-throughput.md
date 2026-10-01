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
