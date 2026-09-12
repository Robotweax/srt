# Bounded send-work budget experiment

Lab [PR #30](https://github.com/Robotweax/srt_network_lab/pull/30) completed its
40 short transfers, but its absolute-deadline candidate remained about 23%
slower than baseline. Recomputing the instrumented histograms shows that
86–96% of the mean A/B difference in positive send lateness was already present
at callback start. This motivates testing less repeated dispatch/poll work
between closely spaced DATA packets. It does not establish a kernel or socket
syscall bottleneck, and the 16-MiB test did not qualify long transfers.

## Candidate and exact pins

| Role | Revision |
|---|---|
| A: baseline | `8e1bdebed836cb7b732db852f51ef6a7b212e925` |
| C: bounded send-work budget | `36477473ad3f68e94b90573797fb5a45c04d7872` |
| Haivision 1.5.7 | `899348d8318eb9a3c5a5b6ec43c4a1114288773a` |
| Common harness | One clean committed checkout containing this procedure; record its full SHA separately |

C is a new product candidate. The failed B pin `11814d3` is historical evidence,
not an input to this qualification. The common public-API peer remains unchanged:
SHA-256 `faf487b34f5661e4dc602130628ed216b8054a2ab364abcc275bdda1d826d2ae`.

The candidate shares one stack-allocated `SendWorkBudget` across all connections
visited in a channel callback:

- At most **64 DATA/FEC packets**, including retransmitted DATA.
- **50 microseconds** from admission of the first send attempt. Receive/control
  housekeeping precedes admission; later send preparation counts toward it.
- Bridge a Pacer gap only when it is **at most 5 microseconds**, and its deadline
  falls strictly inside the remaining total budget. Re-read the Pacer before
  selecting another packet; never send early or accumulate catch-up credit.
- End admission at either bound, rotate the starting route on the next callback,
  and retain absolute deadlines for deferred work. Ready/timed work alternates
  when both are runnable on one scheduler shard.

These are cooperative admission limits. An admitted syscall, lock acquisition
or descheduled thread can complete after 50 microseconds. Control housekeeping
retains its existing limits. This is not a hard callback deadline or a CPU
percentage cap. Bridges use clock reads without a yielding syscall; CPU cost is
part of the qualification below. There is no new public option, pacing-rate
rule, public layout, per-packet allocation, timer-slack change or instrumentation.
Protocol clocks injected by deterministic tests do not spin against wall time.

## Preflight and two fresh builds

Use the original dedicated Ubuntu ARM64 VM `srt-network-lab-runtime`, hostname
`lima-srt-network-lab-runtime`, 4 vCPU and configured 6 GiB. Record VM identity,
actual memory, toolchain, CPU configuration, host load and competing processes.
Use static Release/OpenSSL, library `-O3 -DNDEBUG` plus `-g`, common peer `-O2 -g`.
No competing work, cached builds, profiler, sanitizer or diagnostic overlay.

With `SRT` pointing to the clean pinned harness, `REFERENCE` to the exact clean
Haivision checkout and `WORK` to a new directory, first verify all Git objects:

```sh
git -C "$SRT" status --porcelain
git -C "$SRT" rev-parse HEAD
git -C "$SRT" cat-file -e 8e1bdebed836cb7b732db852f51ef6a7b212e925^{commit}
git -C "$SRT" cat-file -e 36477473ad3f68e94b90573797fb5a45c04d7872^{commit}
python3 -m unittest discover -s "$SRT/interop/tests" -p 'test_bounded_send*.py' -v
```

Stop and retain original output/status if preparation fails. The eight synthetic
tests cover the full plan, rejection/skip paths, aggregate-byte accounting,
missing evidence, and five outer-operator scenarios. Ready JSON is written,
closed and atomically renamed before sending a test signal. These tests do not
perform SRT transfers or change real sysctls.

Build both variants completely before measuring:

```sh
git -C "$SRT" worktree add --detach "$WORK/baseline-source" \
  8e1bdebed836cb7b732db852f51ef6a7b212e925 || exit 1
git -C "$SRT" worktree add --detach "$WORK/candidate-source" \
  36477473ad3f68e94b90573797fb5a45c04d7872 || exit 1
for variant in baseline candidate; do
  python3 "$SRT/benchmarks/prepare_throughput.py" \
    --robotweax-source "$WORK/$variant-source" --reference-source "$REFERENCE" \
    --output-directory "$WORK/$variant-plain" --linkage static --jobs 2 || exit 1
done
```

Preserve every source/harness/build identity, command, compiler/OpenSSL log,
CMake cache, binary hash and original exit code. The runner rejects wrong pins,
dirty checkouts, differing toolchains, changed binaries or instrumented builds.

## Three conditional stages: at most 104 transfers

Each stage uses serial **A/B/B/A blocks** (the runner's `candidate` means C),
profiles R→R then R→H, with an H→H control before and after each block. A warmup,
when specified, precedes the measurements of each profile in each block.
Cooldown is 30 seconds before every block.

| Stage | Streams | Requested bytes per stream | Warmups/profile/block | Measurements/profile/block | Transfers |
|---|---:|---:|---:|---:|---:|
| 00-window-128m | 1 | 128 MiB | 0 | 2 | 24 |
| 01-long-1g | 1 | 1 GiB | 1 | 3 | 40 |
| 02-ten-streams | 10 | 128 MiB | 1 | 3 | 40 |

At maximum: **64 measurements + 24 H controls + 16 warmups = 104 transfers**.
A ten-stream case transfers 1.25 GiB requested in aggregate. The unchanged peer
rounds each stream to complete 1316-byte messages. Public packet counts and CPU
normalization use the actual payload and all streams, not one stream's bytes.

All cases use IPv4 loopback, Live/Message, TSBPD 120 ms, TLPKTDROP off, Pending
8192, FC 16384, requested SRT buffers 32 MiB and UDP buffers 8 MiB, MAXBW
1250000000 bytes/s, target 0, timeout 45 s and shutdown 500 ms. Both peers use
the stage's stream count, including H controls. No adaptive parameter changes.

## Gates fixed before measurement

Within an entered stage, ordinary failures do not omit the remaining ABBA
blocks. At stage end, all of the following are required to advance:

1. Every case, including controls/warmups, proves complete deterministic payload,
   exact options, both peer PIDs, U/S CPU, voluntary/involuntary context switches,
   complete public packet counters and zero retransmissions. UDP `InErrors`,
   `RcvbufErrors`, `SndbufErrors`, `InCsumErrors` must explicitly have zero deltas.
   Ten-stream cases additionally require valid ordered completion distributions
   for both peers and all ten verified connections.
2. Across the eight H controls, maximum/minimum throughput is at most **1.15**.
3. For each R-sender profile, compare medians of the two A versus two C blocks'
   measurements: C/A throughput at least **0.95**, sender CPU/actual GiB at most
   **1.05**, and combined sender+receiver CPU/actual GiB at most **1.05**.
4. At least one R-sender profile improves throughput by **5%** or combined CPU
   per actual GiB by **5%**. An unchanged result is not a qualification gain.

These thresholds are experiment decisions, not statistical significance claims.
Keep each block/case and both endpoint CPU values visible. The runner records
all comparisons. A failed gate explicitly skips later stages; no retries,
replacement cases, adjusted thresholds or selective deletion. This prevents a
failed short/long experiment from silently proceeding into ten-stream claims.

Exit 0 means all three stages qualified under these gates; exit 1 means a
measurement/evidence failure; exit 2 means a complete stage failed its control
or performance gate. Original block exit codes remain recorded independently.
SIGINT/SIGTERM/SIGHUP stops the plan and preserves its interruption code. A
report can have `complete: true` with `qualified: false` when it correctly
finishes a conditional plan by skipping later stages.

## Operator and one execution

`run_capacity_operator.py` reuses the signal-forwarding/restoration operator
validated by Lab #29 and SRT #35. It records prior UDP maxima, temporarily sets
both to 33554432, independently restores/verifies both original values, checks
unchanged security sysctls, and preserves a nonzero runner result even if cleanup
fails. Use existing `sudo -n`; this stage does not configure new privileges.
After preflight, invoke exactly once:

```sh
python3 "$SRT/benchmarks/run_capacity_operator.py" \
  --output-directory "$WORK/operator" -- \
  python3 "$SRT/benchmarks/run_bounded_send_ab.py" \
    --baseline-plain "$WORK/baseline-plain/build-manifest.json" \
    --candidate-plain "$WORK/candidate-plain/build-manifest.json" \
    --output-directory "$WORK/budget-results"
operator_status=$?
printf '%s\n' "$operator_status" > "$WORK/operator.exit-code.txt"
```

The runner itself never changes sysctls. Independently verify restored UDP
values, unchanged security settings, no surviving case/peer/profiler processes,
and unchanged source/binary identities after cleanup.

Deliver a new Lab directory/PR with the complete conditional plan, raw
requests/cases/reports and peer stdout/stderr, original statuses, both build
manifests, host/source/operator identities, `qualification-report.json`, skipped
stage reasons, SHA-256 archive manifest, independently reproducible gate
calculations and verified cleanup. Missing values are not zeros. No Haivision
binaries or full build trees. Keep earlier archives unchanged.

The development macOS smoke completed 128 MiB, 1 GiB and ten streams. Its initial
ten-stream run had five retransmissions; a subsequent separate local ABBA
comparison completed without retransmissions for either variant. Both records
are retained. This does not explain away the initial observation or qualify
Linux performance. Any retransmission in this controlled laboratory plan fails
its gate.
