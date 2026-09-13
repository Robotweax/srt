# Worker-owned resubmission: fixed ABBA experiment

Lab [#34](https://github.com/Robotweax/srt_network_lab/pull/34) showed fewer
instrumented full polls but more channel turns, without qualifying plain gain.
The one-shot skip has been removed from the product. This candidate starts from
the original baseline A and changes only immediate scheduler resubmission.
It is a new hypothesis, not a demonstrated improvement.

## Product contract

Baseline A: `8e1bdebed836cb7b732db852f51ef6a7b212e925`.
Candidate B: `264d613543e953c63fd5ebe7d20394a8eb6557a2`.
Haivision 1.5.7: `899348d8318eb9a3c5a5b6ec43c4a1114288773a`.
The handoff pins a separate clean common harness for both fresh builds.
Public peer SHA-256 remains
`faf487b34f5661e4dc602130628ed216b8054a2ab364abcc275bdda1d826d2ae`.

When a channel callback needs immediate follow-up, the executing worker moves
its already owned task into the tail of its existing affinity queue. It does
not copy the shared context or notify its own condition variable. No task
executes inline and no second ready queue exists. The original shard mutex,
queue capacity, FIFO order, timer priority and completion accounting remain.
A task may transfer itself once per invocation, only on its own scheduler and
shard. Full/stopped rejections retain the current context until callback exit.
Accepted work drains under the existing stop policy. Normal external submit,
timed scheduling and channel wake/stop rules remain unchanged.

Every resumed channel runs the original full receive and connection poll.
No cached pacing hint, skipped poll, new send budget, spin, timer policy,
pacing rate, public option or API/ABI change. The existing no-early-send pacer
remains authoritative. Worker-local active-task bookkeeping and validation
have costs too; only the following plain comparison may demonstrate net gain.

Native tests cover ready- and timer-origin tasks, one transfer per invocation,
wrong scheduler/shard/thread, unique context ownership, queue-full rejection,
FIFO around resubmission, due-timer priority, and stop before/after acceptance.
Existing channel wake, shutdown, protocol and lifecycle tests still apply.

## Preparation

Use the original dedicated Ubuntu ARM64 VM, hostname
`lima-srt-network-lab-runtime`, four vCPU and configured 6 GiB. Record actual
memory, load, process list, host/VM identity, timer slack and security sysctls.
Keep them unchanged; no competing work or profilers. Existing `sudo -n` only.

`SRT` is the clean pinned common harness checkout, `REFERENCE` the clean pinned
Haivision checkout, `WORK` a new empty directory. Clone the supplied bundle with
explicit branch `codex/worker-owned-resubmit`; verify hashes and both product
objects. Preserve preparation failures and stop. Do not substitute current main
for either product pin.

```sh
python3 -m unittest discover -s "$SRT/interop/tests" \
  -p 'test_worker_resubmit_ab.py' -v || exit 1
python3 -m unittest discover -s "$SRT/interop/tests" \
  -p 'test_scalability_scorecard.py' -v || exit 1
git -C "$SRT" worktree add --detach "$WORK/baseline-source" \
  8e1bdebed836cb7b732db852f51ef6a7b212e925 || exit 1
git -C "$SRT" worktree add --detach "$WORK/candidate-source" \
  264d613543e953c63fd5ebe7d20394a8eb6557a2 || exit 1
for variant in baseline candidate; do
  python3 "$SRT/benchmarks/prepare_throughput.py" \
    --robotweax-source "$WORK/$variant-source" --reference-source "$REFERENCE" \
    --output-directory "$WORK/$variant-plain" --linkage static --jobs 2 || exit 1
done
```

Two fresh static Release/OpenSSL build pairs, identical toolchain/flags:
libraries `-O3 -DNDEBUG` plus `-g`; peer `-O2 -g`. Both pairs must finish before
measurement. Keep all build commands, original statuses, caches, manifests,
logs, source proofs and all four library/four peer hashes. No instrumentation,
including disabled-in-environment counter overlays, may enter plain evidence.

## Exactly 24 serial cases

A/B/B/A. Each block: 30-second cooldown, H→H control, two R→R measurements,
two R→H measurements, H→H control. No warmups. 16 measurements and eight H
controls; four measurements per variant/profile. No retries, replacements,
parameter sweep, long-transfer or ten-stream stage.

One stream per case, 128 MiB requested, 101990 × 1316 = 134218840 actual bytes;
IPv4 loopback, Live/Message, TSBPD 120 ms, TLPKTDROP false, Pending 8192,
FC 16384, SRT buffers 32 MiB, UDP buffers 8 MiB, MAXBW 1250000000 bytes/s,
target 0, timeout 45 seconds, shutdown 500 ms.

```sh
python3 "$SRT/benchmarks/run_capacity_operator.py" \
  --output-directory "$WORK/operator" -- \
  python3 "$SRT/benchmarks/run_worker_resubmit_ab.py" \
    --baseline-plain "$WORK/baseline-plain/build-manifest.json" \
    --candidate-plain "$WORK/candidate-plain/build-manifest.json" \
    --output-directory "$WORK/worker-resubmit-results"
operator_status=$?
printf '%s\n' "$operator_status" > "$WORK/operator.exit-code.txt"
```

The outer operator temporarily sets both UDP maxima to 33554432 and restores
their individual original values independently. No other sysctl change.
Ordinary case/report failures do not skip later blocks; signals stop the plan
and clean owned case processes. Keep original driver/block codes, 128+signal
and secondary cleanup errors. Missing peer evidence is never an inferred zero.

## Evidence and candidate gate

Every case: complete deterministic payload, exact options, public packet
counts, endpoint PID/CPU/context switches, actual raw peer wait codes, zero
retransmissions and explicit zero UDP error deltas (`InErrors`, `RcvbufErrors`,
`SndbufErrors`, `InCsumErrors`). All eight H controls require max/min ≤1.15.

`worker-resubmit-report.json` uses all four measurements per variant/profile.
Normalize CPU/context switches by actual GiB; sum endpoint CPU per case before
its median. Preserve all samples. No significance or causal per-operation cost.

Both profiles require B/A throughput ≥0.95, sender CPU/GiB ≤1.05 and total
CPU/GiB ≤1.05. At least one profile must gain ≥5% throughput or reduce total
CPU/GiB ≥5%. Exit 0 means valid evidence, possibly without a nominated candidate;
1 means failed/incomplete evidence; 2 means invalid H controls. Inspect reasons
and `followup_candidates` separately. `long_transfer_qualification_performed`
remains false. A nominated candidate still needs separate long/scaling and
latency qualification; this run does not authorize a merge.

## Delivery

German report, all raw requests/cases, 96 peer stdout/stderr files, 24 wait
sidecars with 48 actual statuses, four block reports, global report, independent
arithmetic without importing harness analyzers, complete build/source/operator/
environment evidence, original statuses, SHA-256/size manifests and a verified
archive. Keep every failure. Do not publish binaries, Haivision source/build
trees or full build directories. Verify after cleanup: all eight binary hashes
and source identities unchanged, both UDP values restored, timer/security
settings unchanged, no remaining case/peer/profiler processes.
