# Pacer deadline, dispatch and send diagnosis

Lab PR #29 rejected library candidate `11814d3`: every Robotweax-sender case
timed out after receiving only 11–12% of its planned messages. The precise
delay mechanism was not observed. This stage locates that delay; it does not
change pacing, batching, flow control, fairness or public API behavior.

The harness branch is based on baseline `8e1bdeb`. Its normal `src` and
`include` trees are unchanged. Build instrumentation is applied only to fresh
exports of the exact sources below. The failed candidate in SRT PR #34 and the
counter overlay in PR #33 remain separate.

## Exact identities and four builds

| Role | Revision |
|---|---|
| A: Robotweax baseline | `8e1bdebed836cb7b732db852f51ef6a7b212e925` |
| B: failed absolute-deadline candidate | `11814d3a1ebc217dbe95613b679960ac3152ff59` |
| Haivision 1.5.7 in all builds | `899348d8318eb9a3c5a5b6ec43c4a1114288773a` |
| Common harness | One clean committed checkout containing this document; record its full SHA separately |

The unchanged common public-API peer SHA-256 is
`faf487b34f5661e4dc602130628ed216b8054a2ab364abcc275bdda1d826d2ae`.
Both pins have reviewed whole-file SHA-256 identities and unique replacement
anchors in `pacer_deadline_diagnostics.py`. A wrong pin, changed source,
missing anchor, changed collector or mixed overlay fails validation. The
export records original/patched hashes for four private source files.

Use the original dedicated Ubuntu ARM64 VM `srt-network-lab-runtime`, hostname
`lima-srt-network-lab-runtime`, 4 vCPU and configured 6 GiB. Verify VM identity,
memory, toolchain and absence of competing work. Build all four pairs before
measuring, using the same clean harness. With `SRT`, `REFERENCE` and a new
`WORK` directory set to their intended paths:

```sh
HARNESS=$(git -C "$SRT" rev-parse HEAD)
git -C "$SRT" worktree add --detach "$WORK/baseline-source" \
  8e1bdebed836cb7b732db852f51ef6a7b212e925 || exit 1
git -C "$SRT" worktree add --detach "$WORK/candidate-source" \
  11814d3a1ebc217dbe95613b679960ac3152ff59 || exit 1
for variant in baseline candidate; do
  python3 "$SRT/benchmarks/prepare_throughput.py" \
    --robotweax-source "$WORK/$variant-source" --reference-source "$REFERENCE" \
    --output-directory "$WORK/$variant-plain" --linkage static --jobs 2 || exit 1
  python3 "$SRT/benchmarks/prepare_throughput.py" \
    --robotweax-source "$WORK/$variant-source" --reference-source "$REFERENCE" \
    --output-directory "$WORK/$variant-diagnostic" --linkage static --jobs 2 \
    --pacer-deadline-diagnostics || exit 1
done
```

Normal Release/OpenSSL/static builds, library `-O3 -DNDEBUG` plus `-g`, common
peer `-O2 -g`. No cached builds, sanitizer, perf capture, alternate timer
settings or extra overlay. Preserve source/host/build identities, flags,
binary hashes and original command statuses. Stop on a build error.

## Fixed 40-transfer plan

Each transfer requests 16 MiB: 12749 messages of 1316 bytes, or 16777684
actual payload bytes. One plain IPv4-loopback Live/Message connection,
TSBPD 120 ms, TLPKTDROP off, Pending 8192, FC 16384, requested SRT buffers
32 MiB and UDP buffers 8 MiB, MAXBW 1250000000 bytes/s, target 0, timeout
45 s and shutdown 500 ms. No changes if a case is slow or fails.

| Block | Library/build | Cases |
|---|---|---:|
| 00 | A plain | H self-control, 3 R→R, 3 R→H, H self-control |
| 01 | B plain | H self-control, 3 R→R, 3 R→H, H self-control |
| 02 | A diagnostic | 2 R→R, 2 R→H |
| 03 | B diagnostic | 2 R→R, 2 R→H |
| 04 | B plain | H self-control, 3 R→R, 3 R→H, H self-control |
| 05 | A plain | H self-control, 3 R→R, 3 R→H, H self-control |

**40 transfers = 24 plain measurements + 8 Haivision controls + 8 diagnostic
cases.** Zero warmups; 30 seconds' cooldown before each block. These short
plain brackets describe instrumentation disturbance and direction drift, not
a replacement for the failed 1-GiB capacity experiment or a production CPU
comparison. Keep each block/case visible; no combined plain/diagnostic rate
summary. At this transfer size, startup effects can be substantial.

## What the collector measures

Every scheduler worker registers PID, TID, shard index/count and, on Linux,
its current timer slack from a read-only query (`-1` means unavailable).
There is no timer-slack or timer-resolution adjustment. Worker-local counters
and fixed histograms measure:

- Accepted submission to actual callback start, separated into ready/timed
  tasks. Timer remaining/overdue time at submission covers dispatched tasks;
  cancelled tasks have no dispatch sample.
- Timed-wait requested duration, actual return duration and return lateness,
  plus early returns. `condition_variable` return includes mutex reacquisition;
  this does not isolate pure kernel sleep or count physical CPU wakeups.
- Pacer deadline relative to the task start, and relative to the successful
  DATA-send return, sampled for the same paced DATA packets. Initial packets
  without a prior Pacer deadline have a separate counter.
- Task start to DATA-send return, callback duration, DATA per callback and
  connection poll, receive attempts/EAGAIN, and Pacer/flow/crypto/empty-selection
  blocks. Send timestamps include `send_data` bookkeeping and do not represent
  hardware egress timestamps. Control/FEC packets are excluded from DATA counts.

Nanosecond histogram bounds are inclusive. Packet-count histograms use the
same numeric bounds with explicit packet units. Quantiles are reported as
bucket intervals, never invented exact percentiles. Read active sender shards
alongside the whole endpoint; marginal histograms are not a per-packet trace.

Extra clock reads, an additional read-only Pacer query and private `Task`
timestamp fields perturb timing and memory traffic. No public class layout
changes. The exporter modifies only diagnostics; measurements with this
overlay are always labeled instrumented even if output is incomplete.

## Partial output and failure handling

There is no per-packet output. Workers write registration, at most one
checkpoint per second after callbacks, and a final snapshot at orderly thread
exit. Checkpoints are capped at 128 per worker plus the final row. The
collector flags counter saturation or exhausted checkpoint budget; the reader
limits each worker file to 2 MiB. Checkpoint I/O runs outside callback locks,
but still delays the next callback and is part of observer overhead.

On timeout or forced process termination, earlier valid checkpoints are
retained. Registration without a final snapshot, truncated JSON, missing
workers, nonmonotonic counters/histograms or packet coverage mismatch fails
diagnostic completeness. Partial worker PIDs do not imply a known sender or
receiver role if the corresponding completion evidence is absent. Missing
CPU/packet statistics are not zero. No signal-unsafe collector I/O is added.

Ordinary failures retain their original status and remaining planned blocks
continue. SIGINT/SIGTERM/SIGHUP stop the plan; the runner invokes the existing
case driver in-process so its tracked case group and inherited peer processes
are cleaned. No retries, replacement transfers or selective deletion.

## Operator and execution

`run_pacer_deadline_operator.py` carries the reviewed outer operator from
[Lab PR #29](https://github.com/Robotweax/srt_network_lab/blob/9d544ef77deadf4638fa85899e181a73c20c45f8/results/2026-09-12-pacer-deadline-abba/report-de.md).
It records prior UDP maxima, temporarily sets both to 32 MiB, forwards signals
to the owning runner, then independently restores/verifies both prior values
and verifies unchanged security settings. A nonzero runner status is preserved
even when cleanup also fails. Run the five fake-sysctl operator scenarios and
the atomic readiness tests on the VM before any SRT transfer:

```sh
python3 -m unittest discover -s "$SRT/interop/tests" -p 'test_pacer_deadline*.py' -v
```

Those synthetic tests include success, runner failure, SIGINT/SIGTERM with
signal-ignoring case/peer, and failure of the first restore while the second
still runs. Ready JSON is written, closed and atomically renamed, including
the old 72-transfer harness test. Preserve any failed preparation; repair it
before measurement. Repeating a flaky test is not its repair.

After inspecting the operator on the actual VM, invoke exactly once:

```sh
python3 "$SRT/benchmarks/run_pacer_deadline_operator.py" \
  --output-directory "$WORK/operator" -- \
  python3 "$SRT/benchmarks/run_pacer_deadline_diagnostics.py" \
    --baseline-plain "$WORK/baseline-plain/build-manifest.json" \
    --candidate-plain "$WORK/candidate-plain/build-manifest.json" \
    --baseline-diagnostic "$WORK/baseline-diagnostic/build-manifest.json" \
    --candidate-diagnostic "$WORK/candidate-diagnostic/build-manifest.json" \
    --output-directory "$WORK/deadline-results"
operator_status=$?
printf '%s\n' "$operator_status" > "$WORK/operator.exit-code.txt"
```

Preserve all logs and statuses and independently check for remaining peers and
unchanged binaries/source checkouts after cleanup. The runner validates exact
pins, common harness, four build signatures and requested UDP capacity. It
never changes sysctls itself. The operator uses `sudo -n`; no password prompt
or unattended privilege setup is part of this stage.

## Result contract and interpretation

Every completed case, including controls and diagnostics, must prove full
deterministic payload, exact options, complete public packet statistics and
zero retransmissions. All 40 cases need explicit zero deltas for UDP
`InErrors`, `RcvbufErrors`, `SndbufErrors`, `InCsumErrors`. Both completed peer
PIDs, CPU U/S and voluntary/involuntary context switches are required. Keep
failed cases and partial metrics. An exit-zero diagnostic is not performance
approval; diagnose whether delay accumulates before callback start or during
poll/send processing, and inspect flow/empty/Pacer counters before attributing
it to the operating system.

Deliver a new Lab result directory/PR with raw requests/cases/reports, worker
JSONL, original stdout/stderr/status, exact source/overlay/build/host identities,
SHA-256 manifest, independently reproducible validation and verified cleanup.
Keep original archives unchanged; no Haivision binaries or whole build trees.
Do not implement a new pacing budget, busy-spin threshold or batching policy
inside this measurement. That requires a separate candidate and review.
