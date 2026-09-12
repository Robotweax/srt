# Absolute pacer-deadline experiment

**Result:** [Lab PR #29](https://github.com/Robotweax/srt_network_lab/blob/9d544ef77deadf4638fa85899e181a73c20c45f8/results/2026-09-12-pacer-deadline-abba/report-de.md)
attempted all 72 transfers. All 16 candidate Robotweax-sender cases timed out;
56 transfers completed. This candidate failed the performance criterion and
should not be merged in this form. The text below preserves the original
experiment contract. The next stage is a separate, smaller
[deadline/dispatch/send diagnosis](pacer-deadline-diagnostics.md).

This candidate tests whether scheduling the next channel poll at its actual
pacer deadline reduces sender CPU without losing throughput. It is an
experiment, not a measured performance improvement or a production release.

[Lab PR #28](https://github.com/Robotweax/srt_network_lab/blob/cda122a9a4b2414bec9b3ff623eec1038f91c837/results/2026-09-12-poll-wake-counters/report-de.md)
recorded 3.41–3.66 sender polls per original DATA packet, about 74% of them
without a DATA send. Over 98% of channel continuations took the immediate
sub-millisecond pacer path. More than 99.5% of sender receive attempts ended in
EAGAIN. Application notifications were already over 99.96% coalesced. These
counts motivate a scheduling change but do not measure its potential CPU gain.

## Candidate behavior and limits

The connection returns an absolute `steady_clock` deadline tied to its origin.
The channel retains the earliest connection deadline, capped by the existing
idle interval for receive/control work, and submits that exact deadline to the
scheduler. A short positive remainder no longer requests immediate polling or
gets rounded to milliseconds. Processing time is not added to a previously
computed relative delay.

Due timers and ready work alternate when both are runnable on a scheduler
shard. This preserves progress for other channels even when a hot sender's
timer is already due by its next dispatch. Timer order and ready FIFO order
remain intact within their respective queues. Existing notification, timer
cancellation, active-poll follow-up and stop/context-lifetime handling remain
in place; failure to schedule still breaks affected connections explicitly.

Public headers and class layouts, the shared public-API peer, pacing rate,
catch-up policy, flow window, buffer sizes and batch limits are unchanged.
Only private runtime deadline representation and dispatch selection change.
The diagnostic counter overlay from SRT PR #33 is separate and still tied to
its original source; do not apply it to the new candidate without reviewing
and pinning new anchors.

The scheduler can wake after a deadline. The unchanged pacer computes its
next deadline from `max(previous_deadline, actual_now) + interval`, so lateness
does not create compensating burst credit. Lower CPU with substantially lower
throughput would fail the purpose of this experiment. No busy-spin threshold,
timer-resolution tuning, batching change or rate change is combined with it.

## Fixed library and harness identities

| Role | Exact revision |
|---|---|
| A: baseline Robotweax library | `8e1bdebed836cb7b732db852f51ef6a7b212e925` |
| B: absolute-deadline Robotweax library | `11814d3a1ebc217dbe95613b679960ac3152ff59` |
| Haivision 1.5.7, both variants | `899348d8318eb9a3c5a5b6ec43c4a1114288773a` |
| Harness | One clean committed checkout of `codex/pacer-deadline` containing this document; record its full SHA separately |

The common peer source SHA-256 remains
`faf487b34f5661e4dc602130628ed216b8054a2ab364abcc275bdda1d826d2ae`.
The harness commit follows the candidate commit and must not replace the
candidate library pin in manifests. `run_pacer_deadline_ab.py` accepts only
this pair. The earlier `run_plain_capacity_ab.py` retains its original pair
and defaults; common validators also accept the new runner's explicit
contract without changing the old experiment.

## Lab handoff: build both variants before measuring

Use the original dedicated `srt-network-lab-runtime` Ubuntu ARM64 VM with
4 vCPU and 6 GiB. The runner checks Linux ARM64, the established hostname
`lima-srt-network-lab-runtime` and four CPUs. The operator must also verify
memory/VM identity and absence of competing work. Hosted CI and local macOS
checks are separate evidence, not substitutes for the VM.

Set `SRT` to the clean committed harness checkout, `REFERENCE` to the clean
exact-pinned Haivision checkout and `WORK` to a new work directory:

```sh
HARNESS=$(git -C "$SRT" rev-parse HEAD)
printf '%s\n' "harness=$HARNESS"
git -C "$SRT" worktree add --detach "$WORK/baseline-source" \
  8e1bdebed836cb7b732db852f51ef6a7b212e925 || exit 1
git -C "$SRT" worktree add --detach "$WORK/candidate-source" \
  11814d3a1ebc217dbe95613b679960ac3152ff59 || exit 1
for variant in baseline candidate; do
  python3 "$SRT/benchmarks/prepare_throughput.py" \
    --robotweax-source "$WORK/$variant-source" --reference-source "$REFERENCE" \
    --output-directory "$WORK/$variant-plain" --linkage static --jobs 2 || exit 1
done
```

Both library pairs use Release/OpenSSL/static, normal Release `-O3 -DNDEBUG`
with `-g`; both peers use the common `-O2 -g` flags. No overlay, sanitizers,
new profiling flags or cached build reuse. Preserve compiler/OpenSSL/CMake,
cache, loader and binary-hash evidence. Stop on a build error.

Before running, review/test the operator interruption and restoration path.
Use the [temporary UDP-limit procedure](throughput-profiling.md#2-check-udp-limits-do-not-silently-tune-the-host)
with recorded prior values, independent restoration attempts and verification
in the enclosing operator's cleanup path. The new runner itself never changes
sysctls. No perf recording, concurrent builds or other load tests.

```sh
python3 "$SRT/benchmarks/run_pacer_deadline_ab.py" \
  --baseline-plain "$WORK/baseline-plain/build-manifest.json" \
  --candidate-plain "$WORK/candidate-plain/build-manifest.json" \
  --output-directory "$WORK/pacer-abba-results" \
  >"$WORK/pacer-abba.log" 2>&1
status=$?
printf '%s\n' "$status" >"$WORK/pacer-abba.exit-code.txt"
```

This is the measurement command, not a replacement for the enclosing
operator's finally/trap and archive collection. Preserve its original status
through cleanup. The operator must not use the flawed original interruption
wrapper from Lab PR #28 unchanged.

## Exact plan and failure handling

Four blocks, **A → B → B → A**, with 30 seconds' cooldown before each block.
Each block contains one Haivision self-control, then one excluded warmup and
three measurements for each direction in this order: Robotweax self,
Haivision self, Robotweax → Haivision, Haivision → Robotweax; one Haivision
self-control ends the block.

Total: **72 transfers = 48 measurements + 16 warmups + 8 controls**. There are
six measured samples per direction/variant across two blocks. Keep blocks and
individual values visible; three-point p95 values are descriptive only.

Every transfer is at least 1 GiB rounded to 815914 messages / 1073742824
payload bytes, 1316-byte payload, one plain IPv4-loopback connection,
Live/Message, TSBPD 120 ms, TLPKTDROP off, pending 8192, FC 16384,
32-MiB requested SRT buffers, 8-MiB requested UDP buffers, MAXBW
1,250,000,000 bytes/s, application target 0, timeout 45 s and shutdown 500 ms.
Keep this contract even if the candidate is slower or times out.

No retries, replacement runs or selective deletion. A nonzero block status
remains in the report and the remaining planned blocks still run. Interruption
stops the plan and leaves an incomplete report. SIGINT/SIGTERM/SIGHUP are
handled in the runner, which invokes the diagnostic driver in the same process;
the driver owns and cleans up its tracked case session and peer descendants.
Repeated handled signals are ignored during cleanup. Two synthetic tests
exercise real SIGINT/SIGTERM with a case and peer that ignore those signals;
they do not perform SRT transfers. Enclosing operator cleanup still needs to
restore and verify UDP maxima after the runner exits.

## Acceptance and result delivery

`ab-report.json` requires all planned payloads, exact pins/options, complete
packet statistics, zero retransmissions and recorded zero `InErrors`,
`RcvbufErrors`, `SndbufErrors` and `InCsumErrors` deltas, including warmups and
controls. Missing values are not zero. Both endpoint PIDs, CPU user/system and
voluntary/involuntary context switches must be present. CPU summaries include
both processes in seconds and seconds per actual payload GiB. Process CPU
includes setup through completion, not the entire shutdown interval.

Completed measurements with retransmissions remain visible in summaries while
failing the contract. Warmups and controls are excluded from those summaries.
Review CPU/GiB and throughput by direction/block together with all Haivision
controls; R→H varied by 4.13% between the prior plain brackets despite only
0.56% control spread, so small changes need careful interpretation.

Success needs repeatable sender-CPU improvement with preserved or better
throughput and complete transport correctness. Exit zero is not automatic
performance/release approval. A regression is useful evidence and remains
archived; do not silently switch to a different waiting or batching policy.

Deliver a new Lab result directory/PR with raw requests/cases/reports,
stdout/stderr, every original status, exact source/build/host identities,
SHA-256 manifest, independently reproducible validator and verified cleanup.
No Haivision binaries or whole build directories. Keep earlier evidence and
the ten-stream PR #26 investigation separate. Keep this candidate unmerged
pending lab review and a separate merge decision.
