# Aggregate poll/wake diagnosis after Lab PR #27

This stage measures how much receive, polling, notification and pacing work
Robotweax performs per original DATA packet. It prepares the next isolated
optimization; it contains no production transport change or performance claim.

The [Lab PR #27 report](https://github.com/Robotweax/srt_network_lab/pull/27)
at `ae5d058a33e38f56e0cc8820c00ef32ec4cd9236` retained 16 complete transfers
with zero retransmissions and recorded UDP errors. Its partial stacks do not
establish repeated send-buffer scanning as the dominant cost. Receive syscalls,
futex/wakeup and clock paths warrant counting before changing scheduling.
Worker sample share is not scheduler overhead or proof of CPU saturation.
The original failed capture/operator status remains part of that evidence;
re-reading existing perf files did not turn it into a clean initial run.

## Fixed sources and scope

| Component | Required identity |
|---|---|
| Robotweax library, both builds | `8e1bdebed836cb7b732db852f51ef6a7b212e925` |
| Haivision 1.5.7, both builds | `899348d8318eb9a3c5a5b6ec43c4a1114288773a` |
| Harness | One clean committed checkout of `codex/poll-wake-diagnostics`; record its full SHA separately |
| Shared public-API peer SHA-256 | `faf487b34f5661e4dc602130628ed216b8054a2ab364abcc275bdda1d826d2ae` |

The counter builder exports the exact Robotweax library commit and patches
five implementation files in that export only. Every source anchor must occur
exactly once. It records original/patched hashes and the overlay/collector
identities; the runner rejects mismatched counter identities. Public headers,
class layouts, the public-API peer and ordinary builds are unchanged. Counter
builds are rejected by plain/perf/transport captures and the existing ABBA
runners. A counter build with its output environment disabled is still an
instrumented build and is not a plain baseline.

The collector uses fixed thread-local integer storage. An increment adds no
clock read, allocation, lock or output operation. Each instrumented thread
registers PID/TID once, opens one exclusive file and flushes a small registration
record; aggregate counters are written at thread exit. Registration has a
one-time cost, and counter increments still perturb execution. Each file needs
both registration and completion records. Missing endpoint output, missing
registered-thread completion, overflow, inconsistent counts and collector
output errors invalidate the evidence. This proves coverage of registered
instrumented threads, not every thread in the process.

Counts cover thread lifetimes, including connection setup and shutdown.
Process CPU from the peer ends at its completion event; the intervals differ.
Normalize counters by completed original packets, retain raw totals and per-TID
rows, and do not interpret their ratio to CPU as an exact per-call duration.
Haivision internals have no added counters. Its reference-only capture reports
`reference_only: true` and empty endpoints, never fabricated zero activity.

## Lab operator: build first, then 16 serial transfers

Use the original dedicated `srt-network-lab-runtime` Ubuntu ARM64 VM with
4 vCPU and 6 GiB. Verify VM identity and absence of competing work. Local macOS
smoke tests and hosted CI are separate checks. No builds, perf recording,
packet capture, parameter sweeps or concurrent scaling tests during this run.

Set `SRT` to the clean pinned harness checkout, `REFERENCE` to the clean exact
Haivision checkout, and `WORK` to a fresh work directory. Record the harness SHA
and toolchain before building both pairs. Do not use the harness tip as the
library revision:

```sh
HARNESS=$(git -C "$SRT" rev-parse HEAD)
printf '%s\n' "harness=$HARNESS"
git -C "$SRT" worktree add --detach "$WORK/library-source" \
  8e1bdebed836cb7b732db852f51ef6a7b212e925 || exit 1
python3 "$SRT/benchmarks/prepare_throughput.py" \
  --robotweax-source "$WORK/library-source" --reference-source "$REFERENCE" \
  --output-directory "$WORK/plain-build" --linkage static --jobs 2 || exit 1
python3 "$SRT/benchmarks/prepare_throughput.py" \
  --robotweax-source "$WORK/library-source" --reference-source "$REFERENCE" \
  --output-directory "$WORK/counter-build" --linkage static --jobs 2 \
  --poll-counters || exit 1
```

Both builds retain Release/OpenSSL/static, normal Release `-O3 -DNDEBUG` plus
`-g`, and common peer `-O2 -g`. Verify both manifests, pins, peer hash and build
flags. Do not reuse the previous perf build or alter optimizations. Keep all
configure/build/loader/compiler/OpenSSL evidence and binaries locally.

Use the existing [temporary UDP-limit and restoration procedure](throughput-profiling.md#2-check-udp-limits-do-not-silently-tune-the-host)
in the same operator shell. Record the prior values, restore them on failure
and interruption, and verify restoration. The runner does not change sysctls.
No perf privileges or kernel security changes are needed for counters.

Each transfer uses one plain IPv4 loopback connection, Live/Message API,
1316-byte payload, at least 1 GiB rounded to **815914 messages / 1073742824
bytes**, pending 8192, FC 16384, 32-MiB requested SRT buffers, 8-MiB requested
UDP buffers, TSBPD 120 ms, TLPKTDROP off, MAXBW 1,250,000,000 bytes/s,
`target-bps=0`, timeout 45 s and shutdown grace 500 ms. These settings match
the prior single-stream stage and are not production Live recommendations.

The fixed sequence is six plain-before transfers, four counter transfers,
six plain-after transfers. Each plain block contains one measurement in each
direction plus automatic Haivision self-controls before and after; warmups=0.
The counter block contains exactly one transfer per direction and no extra
controls or warmups. Cooldown is 30 seconds before each block. There are no
automatic retries or replacement transfers, including after a failure.

The following is the transfer portion of the operator shell, after installing
its cleanup/restoration trap. It preserves all three statuses and continues
the remaining planned blocks after a failed block; do not wrap it in a retry:

```sh
run_poll_block() {
  python3 "$SRT/benchmarks/throughput_diagnostics.py" \
    --build-manifest "$1" --output-directory "$2" --capture "$3" \
    --bytes-per-connection 1073741824 --repetitions 1 --warmups 0 \
    --cooldown-seconds 30 \
    --profile robotweax-self --profile haivision-self \
    --profile robotweax-to-haivision --profile haivision-to-robotweax
}
plain_before_status=0
counter_status=0
plain_after_status=0
run_poll_block "$WORK/plain-build/build-manifest.json" "$WORK/plain-before" none \
  >"$WORK/plain-before.log" 2>&1 || plain_before_status=$?
run_poll_block "$WORK/counter-build/build-manifest.json" "$WORK/counters" counters \
  >"$WORK/counters.log" 2>&1 || counter_status=$?
run_poll_block "$WORK/plain-build/build-manifest.json" "$WORK/plain-after" none \
  >"$WORK/plain-after.log" 2>&1 || plain_after_status=$?
printf 'plain-before=%s\ncounters=%s\nplain-after=%s\n' \
  "$plain_before_status" "$counter_status" "$plain_after_status" \
  >"$WORK/block-status.txt"
```

The enclosing operator must preserve an unsuccessful overall exit if any
block failed, while still collecting artifacts and performing cleanup. An
interrupted run is incomplete. Preserve every attempt and original status;
do not overwrite a failure after repairing an analysis step.

## Required result and decision

For all 16 transfers retain payload validation, both peer exit codes, original
and retransmitted packet counts, throughput, CPU user/system for both peers,
context switches and all four VM-global UDP error deltas (`InErrors`,
`RcvbufErrors`, `SndbufErrors`, `InCsumErrors`). Missing data is not zero.
Acceptance requires complete payloads, reconciled packet statistics and zero
retransmissions/recorded UDP errors for every transfer, including controls.
The counter runner automatically enforces this loss-free contract for its
four captures in `counter_measurement_contract_pass`; the Lab archive
validator must also apply it to all twelve plain transfers. A macOS smoke test
can validate counter output but cannot pass the Linux UDP-evidence contract.

For each Robotweax endpoint provide totals and counts per original packet,
with actual PID/TID rows, for these groups:

| Group | Meaning and limits |
|---|---|
| `rx_*` | Receive attempts, successful datagrams, EAGAIN, oversized datagrams, other errors, decoded DATA/ACK; decoded means before dispatch, not accepted payload |
| `channel_*` polls | Immediate continuation after I/O/send work, immediate sub-ms pacer continuation, delayed continuation |
| `connection_polls`, `batch_*`, `poll_packets` | Connection polls and histogram of successful DATA sends per poll (0, 1, 2–4, 5–16, 17–64, >64); includes retransmissions, excludes FEC/control |
| `tx_originals`, `tx_retransmissions` | Successful DATA sends, reconciled with sender completion statistics |
| `poll_*_block`, `selection_*`, `pacer_*` | Branch/query counts for flow, crypto, time and empty queue; not elapsed blocked time or necessarily mutually exclusive across calls |
| `channel_notify_*`, `scheduler_*` | Notify requests/coalescing/pending continuation, submissions, notify calls, dispatched tasks, condition-variable calls/returns |
| `readiness_*` | Notify requests, no-waiter shortcut, notify-all calls, wait calls/returns |
| `runtime_now_calls`, `*_clock_reads` | Existing runtime time-access calls and selected existing clock reads in elapsed-time and scheduler deadline checks; not a process-wide clock census |

Notify calls are not actual kernel wakeups. A wait return can be spurious.
Thread CPU samples, these event counts and wait durations are different
measurements. Do not infer lock contention time or off-CPU time from counts.
Compare instrumented rates separately with surrounding plain blocks to expose
perturbation; do not pool them or claim an optimization gain. Report control
variation and individual values; this one-pass stage is not a statistical
throughput qualification.

Use the result to choose exactly one subsequent experiment. Many empty polls
with repeated EAGAIN or notifications may justify a narrowly scoped scheduling
or coalescing change. Small successful batches need interpretation alongside
flow/time/empty-queue counts. Large flow/pacer-block counts call for a separate
window/timing investigation. Counts identify frequency, not cost saved: retain
the CPU profile as context and validate any change using plain ABBA afterwards.
Do not remove ACK handling, locks or pacing without their correctness proof.
The ten-stream receive-pressure and Haivision failure investigation from Lab
PR #26 remains separate.

Deliver a new Lab result directory/PR containing raw JSON/JSONL/stdout/stderr,
manifests, statuses, host/build evidence, SHA-256 archive manifest, validator
and cleanup proof. Do not publish Haivision binaries or whole build trees.
Keep earlier archives unchanged. Keep this diagnostic branch unmerged until
the results and the next isolated change have been reviewed.
