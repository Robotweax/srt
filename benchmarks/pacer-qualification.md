# Pacer timer-wait qualification

PR #99 replaces sub-millisecond resubmission with scheduler timers and allows
up to 1 ms of pacing schedule catch-up. Its review branch includes the NAK
recovery fix from PR #97. No release decision should rely on CPU alone.

## Hosted same-runner comparison

Dispatch `timing-diagnostics.yml` with investigation `pacer-ab` on the candidate
branch. The baseline is pinned to main commit
`4cc827956ac719a83ec98b5ad3e07f8f04616ddf`; the candidate is the dispatched commit.
The workflow runs independently on Linux, macOS and Windows. Each runner builds
both revisions, then measures them serially in A/B/B/A order.

- Live timing: 3,000 binary messages at 20 and 100 Mbit/s, cleartext,
  without faults and with `loss-delay-reorder`.
- Scaling: Robotweax-to-Robotweax, 1 and 10 simultaneous connections,
  8 MiB per connection, 20 and 100 Mbit/s total offered application rate shared
  across all connections (the existing scalability peer uses an aggregate pacer).
- Integrity failures remain failures; there is no retry-to-pass behavior.
- Raw timing scorecards and scalability resource reports are retained as artifacts.
- Missing CPU accounting is unavailable, not zero. The POSIX resource delta
  combines both peers; `peer_process_resources` contains separate sender/receiver
  CPU observations when supported, covering startup through completion.
- These short, shared-runner measurements detect gross regressions. They do not
  substantiate a universal 61% sender-CPU reduction, sustained capacity, or
  an exact configured send rate on every OS. No performance threshold is silently
  inferred from a successful functional run.

Initial comparison: https://github.com/Robotweax/srt/actions/runs/36451885425

## Network Lab handoff

Compare the pinned baseline above against the final PR #99 commit on the same
hardware, compiler, Release flags, crypto backend, MTU and buffer configuration.
Record both exact SHAs. Add v0.2.6 as a historical reference if useful, but do not
substitute it for the current-main control. Run warm-ups and at least three
measured ABBA blocks; preserve failed and outlying runs.

Required cases, cleartext initially:

| Offered rate per stream | RTT | Loss | Concurrent streams |
| --- | --- | --- | --- |
| 20 Mbit/s | 10 ms | 0% and 2% | 1 and 10 |
| 100 Mbit/s | 25 ms | 0% and 1% | 1 and 10 |

Use both Robotweax and pinned Haivision 1.5.7 receivers. Keep sender and receiver
placement constant, isolate benchmark CPU cores where possible, and document
whether competing streams share a scheduler shard. Qualify Windows timer wake-up
behavior as well as Linux; do not infer Windows throughput from Linux results.

Record sender and receiver CPU separately (user/system), empty UDP receive calls,
scheduler wake-ups, useful throughput, delivered bytes/hash, NAKs, retransmissions,
sender/receiver drops, p99/p99.9 egress error, and maximum burst depth. Repeat the
existing encrypted and File-mode recovery gates because they share the pacer.

Decide from matched distributions, not the best run: no payload failures or new
drops, no material throughput/timing regression, bounded catch-up, and a repeatable
CPU reduction in the backlogged/lossy scenarios. Investigate wake-up delays above
1 ms explicitly, since those restart this pacer's ideal schedule. The empirical
limits for the deployment should be agreed before interpreting the measurements.

## Initial hosted results (2026-09-28)

Candidate `89b69468f84230beb403a62a10902d24018b06ba`, baseline as pinned above.
The ordinary PR CI passed, including platform builds, sanitizers, encrypted
interoperability and File-mode recovery. The local Release/AEAD suite passed
744/744 tests. The additional ABBA run did **not** qualify the change for merge:

| Runner | Cases passed | Failures |
| --- | --- | --- |
| Ubuntu 24.04 | 32/32 | None |
| macOS | 31/32 | First candidate, 100 Mbit/s with faults: 2,999/3,000 captured messages |
| Windows | 29/32 | Both baselines and first candidate, 100 Mbit/s with faults |

All scaling cases passed. Linux mean sender CPU (two observations per variant)
changed by approximately -4%, -1%, -12%, -2% for 20 Mbit/s / one connection,
20 Mbit/s / ten connections, 100 Mbit/s / one connection, and 100 Mbit/s / ten
connections respectively. Useful throughput was similar. These short self-peer
scaling cases do not reproduce the author's backlogged/lossy CPU experiment and
neither confirm nor refute its claimed 61% reduction.

The macOS failing transfer ended with `timed receive 2999 failed: Connection was
broken`; the sender reported all 3,000 unique packets sent. The deliberately
lost packet was retransmitted after 100,321 us and cumulatively acknowledged
after 117,301 us, close to the configured 120 ms latency. This does not identify
which application message was absent, or prove its cause. Both baseline samples
and the second candidate sample passed; two samples cannot establish causality.

On Windows, the failing baseline/candidate/baseline UDP capture counts were
2,334 / 2,526 / 2,555, while the SRT receiver reached receive indices
2,680 / 2,825 / 2,664 before failing. Thus the diagnostic UDP output/capture path
also lost messages; attributing all missing captured messages to SRT would be
incorrect. Both main and candidate exhibit this failure. Windows CPU accounting
was unavailable in these artifacts and must not be interpreted as zero.

Before a merge recommendation:

1. Repeat the failing macOS and Windows scenario as a predeclared, longer matched
   A/B series, retaining every result. Do not retry until a green sample appears.
2. Instrument message identities at SRT receive and UDP capture separately, along
   with receiver drop statistics on failure, to localize missing messages. Keep
   source pacing, relay delay, retransmission timing and shutdown evidence.
3. Determine whether the 120 ms deadline is exceeded by protocol scheduling,
   relay/host scheduling, or delivery after SRT reception. Change latency or load
   only as explicitly separate diagnostic cases, not to erase the original gate.
4. Complete the dedicated Network Lab matrix above before claiming a repeatable
   CPU improvement or portable rate/timing behavior.

Raw evidence remains in run 36451885425 artifacts named
`pacer-abba-<runner>-36451885425`. Preserve those artifacts before GitHub retention
expires. No failure was retried or reclassified as a pass.
