# Fixed A/B continuation mechanism diagnosis

Lab [#33](https://github.com/Robotweax/srt_network_lab/pull/33) completed a valid
24-case plain comparison without a 5% gain. Product
[#38](https://github.com/Robotweax/srt/pull/38) stays unqualified. This experiment
measures whether its one-shot continuation actually avoids work. It changes
neither product source pin nor pacing, the five-microsecond limit or one-shot
policy. It never nominates a product candidate.

## Sources and instrumentation

- Baseline A: `8e1bdebed836cb7b732db852f51ef6a7b212e925`.
- Candidate B: `9207a1be4c3156eb2866bc00dbbd883ec38c1431`.
- Haivision 1.5.7: `899348d8318eb9a3c5a5b6ec43c4a1114288773a`.
- Unchanged public C++ peer SHA-256:
  `faf487b34f5661e4dc602130628ed216b8054a2ab364abcc275bdda1d826d2ae`.
- The handoff pins one clean common harness for all four fresh builds.

The harness leaves the normal product `src` and `include` trees unchanged.
Only fresh committed-source exports
receive the diagnostic overlay. Full original/patched file hashes, unique
anchors, collector/script identity and every unpatched exported file are checked.
A source bundle must include both product objects as well as the harness branch.

Both variants count channel calls/full polls, receive attempts/outcomes,
connection polls, DATA originals/retransmissions and a DATA-per-poll histogram.
B also counts successful skips, full polls immediately following a consumed
hint, calls without a hint, route eligibility, arm attempts/outcomes and take
outcomes. Counts reconcile per worker and against both public peer completions.

Each of the two scheduler shards registers its PID/TID and writes a completion
record at thread exit, including idle workers. Missing, duplicate, overflowed,
unfinished or unexpected worker files fail. Counters cover worker lifetime,
including setup/shutdown; missing data is never zero. Failed cases retain all
partial files and raw peer wait statuses.

Counter increments use fixed thread-local storage without synchronization,
allocation, I/O or clock queries. Registration and completion perform bounded
I/O once per worker. The diagnostic copy of B's helper has one additional bool
that records consumption solely to count the following full poll. The native
test compares original/patched behavior and helper size; public headers do not
change. No collector state is shared across workers.

Take reasons are mutually exclusive in original predicate order: no pending
hint, changed epoch, clock rollback, expiry, success. Expiry is attributed to
the cap when the stored deadline equals `since + 5us` (including a tie), otherwise
to the earlier pacing deadline. Generation takes priority if several reasons
apply. Arm outcomes are due deadline, overflow guard or success; channel-level
rejections distinguish missing hint and changed epoch. Route eligibility uses
priority: no route, multiple routes, setup, listener, isolated. Missing hint can
also mean no pacing hint from the connection; it is not proof of a route problem.

Age buckets `<1us`, `[1,2)us`, `[2,5)us`, `>=5us` and rollback reuse the existing
`take(now, epoch)` argument and stored origin. No additional clock read is added.
A consumed hint followed directly by channel shutdown may have no following
full poll; therefore the observed following-full count can be below skip count.

Instrumentation changes execution cost and can change hit rates in this narrow
time window. All counts/hit rates describe the instrumented builds; plain
controls do not prove identical uninstrumented hit frequency. These counters
identify mechanism activity, not nanoseconds saved or a causal syscall cost.

UDP arrival has no separate wake signal. B can defer receive/control work through
one extra ready-queue continuation. Five microseconds bounds skip eligibility,
not elapsed wall time under preemption. No control/media latency qualification.

## Preparation

Use only the original dedicated Ubuntu ARM64 VM, hostname
`lima-srt-network-lab-runtime`, four vCPU and configured 6 GiB. Record actual
memory, load, processes, host/VM identity, timer slack and security sysctls;
leave them unchanged. No concurrent work or profilers. Existing `sudo -n` only.

`SRT` is the clean pinned common harness checkout, `REFERENCE` the clean pinned
Haivision checkout and `WORK` a new empty directory. Clone the supplied bundle
using the handoff's explicit harness branch; verify hashes and both product
objects before testing/building. Preserve preparation failures and stop.

```sh
python3 -m unittest discover -s "$SRT/interop/tests" \
  -p 'test_continuation_diagnostics*.py' -v || exit 1
python3 -m unittest discover -s "$SRT/interop/tests" \
  -p 'test_scalability_scorecard.py' -v || exit 1
git -C "$SRT" worktree add --detach "$WORK/baseline-source" \
  8e1bdebed836cb7b732db852f51ef6a7b212e925 || exit 1
git -C "$SRT" worktree add --detach "$WORK/candidate-source" \
  9207a1be4c3156eb2866bc00dbbd883ec38c1431 || exit 1
for variant in baseline candidate; do
  python3 "$SRT/benchmarks/prepare_throughput.py" \
    --robotweax-source "$WORK/$variant-source" --reference-source "$REFERENCE" \
    --output-directory "$WORK/$variant-plain" --linkage static --jobs 2 || exit 1
  python3 "$SRT/benchmarks/prepare_throughput.py" \
    --robotweax-source "$WORK/$variant-source" --reference-source "$REFERENCE" \
    --output-directory "$WORK/$variant-counters" --linkage static --jobs 2 \
    --continuation-counters || exit 1
done
```

Four fresh static Release/OpenSSL build pairs, identical toolchain/flags:
libraries `-O3 -DNDEBUG` plus `-g`; peer `-O2 -g`. All builds finish before any
measurement. Keep commands, original statuses, logs, caches, manifests, all eight
library/eight peer hashes, normal and instrumented source exports. The runner
rechecks all 16 binaries and complete diagnostic exports. No overlay build may
produce plain/perf evidence; plain cases clear inherited counter environments.

## Exactly 32 serial cases

| Block | Variant/build | Cases |
| --- | --- | --- |
| 0 | A plain | H→H, 2×R→R, 2×R→H, H→H |
| 1 | A counters | R→R, R→H |
| 2 | B plain | H→H, 2×R→R, 2×R→H, H→H |
| 3 | B counters | R→R, R→H |
| 4 | B counters | R→R, R→H |
| 5 | B plain | H→H, 2×R→R, 2×R→H, H→H |
| 6 | A counters | R→R, R→H |
| 7 | A plain | H→H, 2×R→R, 2×R→H, H→H |

Both separate cohorts are A/B/B/A. **24 plain cases** (16 measurements,
eight H controls), **eight diagnostic cases** (two per variant/profile,
12 Robotweax endpoints / 24 registered scheduler workers). Each block starts
with 30 seconds cooldown. No warmups, replacements, retries or extra cases.
This new experiment does not replace the completed plain evidence from Lab #33.

Every case: one stream, 128 MiB requested, rounded to 101990 × 1316 = 134218840
actual bytes; IPv4 loopback, Live/Message, TSBPD 120 ms, TLPKTDROP false,
Pending 8192, FC 16384, SRT buffers 32 MiB, UDP buffers 8 MiB,
MAXBW 1250000000 bytes/s, target 0, timeout 45 seconds, shutdown 500 ms.

Run the existing outer operator once; both UDP maxima temporarily 33554432,
then independent restoration of their original values. No other sysctl change.

```sh
python3 "$SRT/benchmarks/run_capacity_operator.py" \
  --output-directory "$WORK/operator" -- \
  python3 "$SRT/benchmarks/run_continuation_diagnostics.py" \
    --baseline-plain "$WORK/baseline-plain/build-manifest.json" \
    --baseline-counters "$WORK/baseline-counters/build-manifest.json" \
    --candidate-plain "$WORK/candidate-plain/build-manifest.json" \
    --candidate-counters "$WORK/candidate-counters/build-manifest.json" \
    --output-directory "$WORK/continuation-results"
operator_status=$?
printf '%s\n' "$operator_status" > "$WORK/operator.exit-code.txt"
```

Ordinary case/report failures do not skip later blocks. Signals stop the plan
and clean the owned case group; retain 128+signal, original driver/block codes
and secondary cleanup errors. Never replace a partial counter file with zeros.

## Evidence and interpretation

All 32 cases require complete deterministic payload, exact options, public
packet counts, endpoint PID/CPU/context switches, actual raw peer wait codes,
zero retransmissions and explicit zero UDP error deltas (`InErrors`,
`RcvbufErrors`, `SndbufErrors`, `InCsumErrors`). Eight H controls require
max/min ≤1.15. Counter cases additionally require all worker records and all
counter/public-packet/lifecycle reconciliations. The runner rereads raw worker
files and compares the result with the original case report.

`continuation-diagnostics-report.json` separates `plain_comparison` from
`diagnostic_profiles`. Plain medians use all four measurements per variant and
profile; diagnostic medians use both counter samples per variant/profile and
endpoint role. CPU adds sender+receiver per case before its median and normalizes
actual bytes. Instrumented rates never enter plain medians. Preserve all samples,
worker files, age buckets, skip/take fractions and per-original counter ratios.
Zero denominators yield null. No significance or causal single-syscall claim.

Exit 0 means complete valid mechanism evidence and stable plain controls;
exit 1 means failed/incomplete evidence; exit 2 means invalid plain comparison;
signal exits retain 128+signal. Every failure has a reason.
`candidate_nomination_performed=false`, `followup_candidates=[]` and
`long_transfer_qualification_performed=false` always remain explicit.
No gain, hit-rate threshold or larger time budget automatically authorizes
another optimization, long/scaling test or merge.

The German Lab report should answer: how often was the hint armed/taken;
which rejection reasons dominated; how many full polls/receive attempts per
original remained; did they fall in both instrumented sender profiles? Explain
observer effects and limits. If few skips occur, avoid attributing unchanged
CPU to the cost of skipped work. If skips occur without useful total effect,
these counters alone cannot isolate invalidation or synchronization cost.

Deliver all raw cases/requests, all 128 peer stdout/stderr files,
32 raw wait sidecars, 24 worker JSONL files, eight block reports, complete
build/source/operator evidence, independent arithmetic, original statuses,
SHA-256/size manifests and a verified archive. Do not publish binaries, Haivision
source/build trees or full build directories. Confirm after cleanup: all 16
binary hashes and sources unchanged, both UDP values restored, timer/security
settings unchanged, no remaining cases/peers/profilers. Retain every failure.
