# CR-12: sub-millisecond pacing qualification

This branch reapplies the two signed commits from #103 to `origin/main` at
`f83165b`. The timer-wait and pacing-credit changes are enabled on Linux and
macOS. Windows and other platforms retain the previous immediate continuation
for sub-millisecond deadlines and the previous pacing schedule. The branch adds
`robotweax_srt_timer_wake_probe` so timer behavior can be measured through the
actual runtime scheduler on each target OS. The probe reports 1,000 serial
timer wakes after 20 warmups at 200, 500, and 1,000 us. Its lateness is
measured from the requested deadline to callback entry.

## macOS local evidence

Apple Silicon, macOS 25.6.0, AppleClang 21.0.0; Release builds, same host and
source tree. `scalability_scorecard.py` ran in A/B/B/A order with 4 MiB per
connection, one iteration per run, no warmup, `many-socket` topology, and
`robotweax-self` profile. Values below are medians of the two runs per variant.
The CPU metric includes both endpoint processes and connection setup.

| Connections | Message bytes | `main` Mbit/s | Candidate Mbit/s | `main` CPU-s/Gbit | Candidate CPU-s/Gbit |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 188 | 11.783 | 11.777 | 10.751 | 11.158 |
| 1 | 1316 | 73.333 | 73.140 | 2.894 | 2.693 |
| 8 | 188 | 93.200 | 93.500 | 8.181 | 7.982 |
| 8 | 1316 | 558.693 | 551.839 | 1.571 | 1.569 |

The 1 x 1316 case used about 7% less CPU per useful Gbit. The 1 x 188 case
used about 4% more. Two samples per variant do not establish whether either
small difference is stable. All four transfers completed with verified payload
integrity and zero retransmissions. Throughput differed by at most 1.3%.

Native timer probe result on this host:

| Deadline | Lateness p50 | Lateness p99 | Lateness maximum | Wakes later than 1 ms credit |
| ---: | ---: | ---: | ---: | ---: |
| 200 us | 109 us | 187 us | 319 us | 0/1000 |
| 500 us | 258 us | 366 us | 727 us | 0/1000 |
| 1000 us | 515 us | 598 us | 1642 us | 1/1000 |

At 100 Mbit/s, 160 TS-1316 messages and 120 ms receiver latency, both `main`
and candidate passed the live timing check with no faults. Mapped receiver
latency was 120339/120267 us (`main`/candidate), with no early UDP egress or
payload integrity failures. The measured maximum burst depth was 71/77; this
single run is insufficient to characterize burst behavior under load.

At 5 Mbit/s, the `loss-delay-reorder` timing profile passed for both variants
with 160 TS-1316 messages. It observed all three injected faults, a loss
report, a retransmitted packet with the retransmission flag, complete payload
integrity, and no early UDP egress. Candidate mapped receiver latency was
120494 us; `main` was 120470 us.

At 100 Mbit/s, the same profile passed on both variants when the delay fault
was shortened to 1 ms with `--fault-delay-ms 1`. Both observed the injected
delay, drop, and reorder, a loss report and retransmission, and complete
payload integrity. Mapped receiver latency was 120493/120450 us
(`main`/candidate); p99 egress deadline error was 9991/10059 us. The measured
maximum burst depth was 76/80; these are single-run observations.

With the default 30 ms delay, this profile fails at 15.04 Mbit/s on both
variants and at 100 Mbit/s on the candidate because the planned drop falls
inside the active delay (`rendezvous fault overlaps an active delay`). The
new option permits a non-overlapping high-rate test without changing the
default fault profiles.

## Reproduction

Build both revisions in Release mode with benchmarks and tools enabled. Run
`benchmarks/scalability_scorecard.py` with `--topology many-socket`,
`--profile robotweax-self`, `--connections 1,8`, `--message-sizes 188,1316`,
`--bytes-per-connection 4194304`, `--iterations 1`, and `--warmup 0`, using
each revision's own `robotweax_srt_scalability_peer`. Repeat in A/B/B/A order.
Run `robotweax_srt_timer_wake_probe` on the candidate. For latency and loss,
run `interop/run_live_timing_interop.py` with each revision's timing and sender
peers, `--profile ts-1316 --messages 160 --latency-ms 120`, and the bitrates
above; add `--fault-profile loss-delay-reorder` for the 5 Mbit/s recovery test
and `--fault-delay-ms 1` for the 100 Mbit/s recovery test.

## Remaining qualification before merge

- Repeat the native timer probe and same-host CPU/throughput comparisons on
  Linux, especially at 20, 50, and 100 Mbit/s. Hosted runners with different
  CPU models do not form a valid A/B comparison.
- Confirm the legacy sub-millisecond scheduling and pacing tests on native
  Windows CI; no Windows performance gain is claimed by this branch.
- Repeat macOS runs under controlled background load to resolve the small
  188-byte CPU change and measure burst depth distribution.
- Repeat high-rate loss and retransmission with non-overlapping fault
  injections, then compare latency and pacing distributions on Linux and macOS.

PF1/#105 covers a separate scheduler/readiness change and needs its own quiet,
busy, shared-port, 64-socket, and 256-socket evidence.
