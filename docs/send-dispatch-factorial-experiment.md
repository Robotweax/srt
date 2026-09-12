# Send budget and dispatch policy: fixed four-variant experiment

Lab [#31](https://github.com/Robotweax/srt_network_lab/pull/31) rejected C at
128 MiB: throughput fell 10.60% for R→R and 19.77% for R→H. Fewer voluntary
context switches did not yield better throughput. C combines a send budget
with the absolute-deadline dispatch policy. This experiment separates those
two change sets, including their interaction. It does not tune the bounds.

## Exact product variants

| Variant | Dispatch policy | Send-work budget | Product revision |
|---|---|---|---|
| A | Baseline | Previous behavior | `8e1bdebed836cb7b732db852f51ef6a7b212e925` |
| B | Absolute deadlines and changed ready/timer selection | Previous behavior | `11814d3a1ebc217dbe95613b679960ac3152ff59` |
| C | Absolute deadlines and changed ready/timer selection | Bounded | `36477473ad3f68e94b90573797fb5a45c04d7872` |
| D | Baseline | Bounded, identical to C | `52d48ee9439d190b920692bd2981729365416b02` |

Use Haivision 1.5.7 `899348d8318eb9a3c5a5b6ec43c4a1114288773a` throughout.
Record one separate, clean common harness SHA for all four builds and the
runner. The public peer remains unchanged, SHA-256
`faf487b34f5661e4dc602130628ed216b8054a2ab364abcc275bdda1d826d2ae`.

D is A plus the shared send-work budget and rotating first route from C. Its
scheduler source and channel scheduling methods remain A. Sub-millisecond
remaining pacing delays still enter A's ready queue. The budget header, send
admission/bridge logic and rotation are identical to C, with the original
relative-delay result retained at the poll boundary.

The shared channel budget admits at most 64 DATA/FEC packets (including
retransmitted DATA), 50 microseconds from the first eligible send attempt,
and a clock-read bridge only for gaps at most 5 microseconds strictly inside
the total budget. These are cooperative admission limits; an admitted syscall,
lock wait or preempted thread can overrun the duration. Existing flow/crypto
blocking, no-early-send/no-catch-up pacing and control housekeeping remain.
There is no public option, per-packet allocation, instrumentation or changed
pacing rate. See the [budget procedure](bounded-send-budget-experiment.md)
for the historical C experiment and its limitations.

## Preflight before any build

Use the original dedicated Ubuntu ARM64 VM `srt-network-lab-runtime`, hostname
`lima-srt-network-lab-runtime`, four vCPU and configured 6 GiB. Record actual
memory, host/VM identity, load, competing processes and toolchain. Keep the
CPU configuration, timer slack and security sysctls unchanged. No other work,
profilers, sanitizers, diagnostic overlays or cached builds.

`SRT` is the clean common harness checkout, `REFERENCE` the clean exact H
checkout, and `WORK` a new existing empty work directory. If supplied a source
bundle, explicitly clone its `codex/send-budget-dispatch-factorial` branch.
Do not rely on a bundle containing only a remote-tracking ref to select HEAD.
The prepared handoff provides the full harness SHA and verified bundle hash.

Prove every object exists before preparing worktrees or builds:

```sh
git -C "$SRT" rev-parse HEAD
git -C "$SRT" status --porcelain
for pin in \
  8e1bdebed836cb7b732db852f51ef6a7b212e925 \
  11814d3a1ebc217dbe95613b679960ac3152ff59 \
  36477473ad3f68e94b90573797fb5a45c04d7872 \
  52d48ee9439d190b920692bd2981729365416b02; do
  git -C "$SRT" cat-file -e "$pin^{commit}" || exit 1
done
git -C "$REFERENCE" rev-parse HEAD
git -C "$REFERENCE" status --porcelain
sha256sum "$SRT/benchmarks/scalability_peer.cpp"
python3 -m unittest discover -s "$SRT/interop/tests" \
  -p 'test_send_dispatch_factorial.py' -v
```

Expect twelve synthetic tests. Their operator coverage includes success,
runner failure, SIGINT, SIGTERM, SIGHUP and independent UDP restoration when
the first restoration fails. Ready JSON is atomically published. These tests
do not transfer SRT payload or modify real sysctls. Keep original logs/status
if preparation fails; stop before measuring and preserve the failed directory.

Build all four variants completely before the first measurement:

```sh
git -C "$SRT" worktree add --detach "$WORK/a-source" \
  8e1bdebed836cb7b732db852f51ef6a7b212e925 || exit 1
git -C "$SRT" worktree add --detach "$WORK/b-source" \
  11814d3a1ebc217dbe95613b679960ac3152ff59 || exit 1
git -C "$SRT" worktree add --detach "$WORK/c-source" \
  36477473ad3f68e94b90573797fb5a45c04d7872 || exit 1
git -C "$SRT" worktree add --detach "$WORK/d-source" \
  52d48ee9439d190b920692bd2981729365416b02 || exit 1
for variant in a b c d; do
  python3 "$SRT/benchmarks/prepare_throughput.py" \
    --robotweax-source "$WORK/$variant-source" --reference-source "$REFERENCE" \
    --output-directory "$WORK/$variant-plain" --linkage static --jobs 2 || exit 1
done
```

Preserve all four build manifests, original commands/statuses, compiler,
OpenSSL and build logs, CMake caches, source/harness identities and hashes of
all eight libraries and eight peers. The common checks require identical
Release/OpenSSL configurations across every variant: library `-O3 -DNDEBUG`
plus `-g`, peer `-O2 -g`. There is no new download/build inside the runner.

## Exactly 48 serial cases

Fixed block order: **A/B/C/D/D/C/B/A**. Before each block: 30-second cooldown.
Each block contains H→H control, two R→R measurements, two R→H measurements,
H→H control. No warmups. Eight blocks × six cases = **32 measurements and
16 controls**. Each variant/profile has four measurements across two blocks.

All cases: one stream, 128 MiB requested, rounded to 101990 complete 1316-byte
messages (134218840 actual bytes), IPv4 loopback, Live/Message, TSBPD 120 ms,
TLPKTDROP off, Pending 8192, FC 16384, SRT buffers 32 MiB, UDP buffers 8 MiB,
MAXBW 1250000000 bytes/s, target 0, timeout 45 s, shutdown 500 ms.

Complete all eight blocks despite ordinary transfer/report failures or poor
performance. Preserve original block and case exit codes. No replacement
cases, retries, parameter changes or selective deletion. A signal stops the
whole plan and the owning runner cleans/reaps its case process group.

Use the existing outer operator once; it temporarily sets both UDP maxima to
33554432, records and independently restores their prior values, and preserves
the runner's nonzero status even if restoration also fails. Existing `sudo -n`
only; do not configure new privileges.

```sh
python3 "$SRT/benchmarks/run_capacity_operator.py" \
  --output-directory "$WORK/operator" -- \
  python3 "$SRT/benchmarks/run_send_dispatch_factorial.py" \
    --a-plain "$WORK/a-plain/build-manifest.json" \
    --b-plain "$WORK/b-plain/build-manifest.json" \
    --c-plain "$WORK/c-plain/build-manifest.json" \
    --d-plain "$WORK/d-plain/build-manifest.json" \
    --output-directory "$WORK/factorial-results"
operator_status=$?
printf '%s\n' "$operator_status" > "$WORK/operator.exit-code.txt"
```

## Valid comparison and subsequent candidate decisions

Every case, including controls, must prove deterministic complete payload,
exact options, both peer PIDs, U/S CPU, voluntary/involuntary context switches,
complete public packet counters, zero retransmissions and explicit zero UDP
`InErrors`, `RcvbufErrors`, `SndbufErrors`, `InCsumErrors` deltas. Missing values
are not zeros. The max/min throughput across all 16 H controls must be ≤1.15.

The runner reports medians of the four measurements for each variant/profile.
CPU and context switches are normalized by actual bytes. Combined CPU is
added per case before taking the median. It reports **D/A, C/B, B/A, C/D**
and the ratio-of-ratios **(C/B)/(D/A)**. Zero denominators in descriptive resource
ratios yield `null`; core throughput/CPU metrics must be positive. These are
descriptive component contrasts, not statistical significance claims or a
causal attribution to one syscall. Both factors include the full change sets
listed above, including route rotation and ready/timer selection respectively.

Only a valid comparison can nominate B, C or D for a later long-transfer test.
For each variant versus A, in **both** R-sender profiles require throughput
ratio ≥0.95, sender CPU/GiB ratio ≤1.05 and total CPU/GiB ratio ≤1.05. At least
one profile must improve throughput by ≥5% or reduce total CPU/GiB by ≥5%.
The runner records every variant's reasons and `followup_candidates`; it
does not select only the best-looking sample or initiate additional stages.

Exit **0** means the 48-case comparison is complete and valid, **even when no
candidate improves**. Exit **1** means failed/incomplete measurement evidence;
exit **2** means invalid H control stability. Original block codes remain
separate. Signal codes are 128+signal. `complete` means the planned attempts
finished; check `analysis.comparison_valid` and `analysis.followup_candidates`
separately. This differs intentionally from the previous candidate-qualification
runner. `long_transfer_qualification_performed` always remains false.

There are no 1-GiB or ten-stream cases in this plan and no merge approval from
exit 0. A nominated candidate still needs its own fixed long-transfer/scaling
qualification. Lab #31's skipped stages and the earlier local five
retransmissions remain unchanged historical evidence.

## Deliverables

New Lab directory/PR and German report, with all 48 raw requests/cases/reports,
peer stdout/stderr, all original statuses, `factorial-report.json`, independent
recalculation of component contrasts and every candidate gate, four complete
build manifests/logs/caches, source/harness/peer identities, operator evidence,
preserved preparation errors and SHA-256/size manifests. Export the relevant
A/B/C/D product files and actual harness files for independent verification.
No binaries, Haivision artifacts or full build trees.

Independently verify both restored UDP values, unchanged security sysctls,
clean source identities, all 16 binary hashes unchanged and no remaining
case/peer/profiler processes. Keep previous Lab archives unchanged.
