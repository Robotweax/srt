# One early paced-poll continuation: fixed ABBA experiment

Lab [#32](https://github.com/Robotweax/srt_network_lab/pull/32) rejected the
bounded-send variants even with the original dispatcher. This candidate starts
again from A and changes only the cost of one early ready-queue continuation.
It is a hypothesis, not a demonstrated performance improvement.

## Product contract

Baseline A: `8e1bdebed836cb7b732db852f51ef6a7b212e925`.
Candidate: `a56b2c5642f8a04ae75cb35650c8120c278f19cf`.
Haivision 1.5.7: `899348d8318eb9a3c5a5b6ec43c4a1114288773a`.
The handoff pins a separate clean common harness commit for both builds.
The common public peer remains SHA-256
`faf487b34f5661e4dc602130628ed216b8054a2ab364abcc275bdda1d826d2ae`.

After a full poll with no received datagram and no immediately runnable send,
a single-route channel may cache the exact future pacing deadline. Active
listener inboxes, setup routes and multiple established routes disable arming.
Injected protocol clocks disable the hint because they have no steady-clock
mapping. The existing relative poll result still drives A's scheduling policy.

One following ready-queue continuation may omit receive and connection polling
while that deadline remains future, for at most **5 microseconds after the full
poll finishes**. It sends nothing, waits for nothing and cannot renew itself.
The next continuation always runs a full poll. Expiry, overdue deadlines,
clock rollback or an invalidation epoch mismatch also force a full poll.
Each continuation returns through the same affinity ready queue as A.

Local send notifications, route/listener/setup changes, received protocol
state changes, application receive-buffer release, dynamic options, close and
broken-state transitions invalidate cached work. Invalidation is atomic and
adds no route/lifecycle lock acquisition under a connection lock. The hint is
owned by the serialized channel worker, contains no route pointer and survives
neither a changed epoch nor reuse. Stop/restart changes the epoch.

UDP arrival has no separate wake signal in this architecture. A packet arriving
after the previous receive check can therefore wait through the one cheap
continuation. The added polling deferral is capped as above and by one extra
ready-queue turn; scheduler delay/preemption is not a hard wall-clock guarantee.
The following full poll services receive, ACK/NAK, retransmission, timers,
crypto/FEC, readiness and every route normally. This bounded change in control
polling must remain visible in the experiment's interpretation.

There is no new timer scheduling, spin loop, send budget, burst/catch-up credit,
pacing-rate change, public option or public API/ABI change. The scheduler and
its scheduling methods remain A. Existing no-early-send pacing remains the
final authority on every packet.

## Prepare before measuring

Use only the original dedicated `srt-network-lab-runtime` Ubuntu ARM64 VM,
hostname `lima-srt-network-lab-runtime`, four vCPU and configured 6 GiB. Record
actual memory, load, processes, VM/host identity, timer slack and security
sysctls. Keep them unchanged. No concurrent work, profilers or overlays.

`SRT` is the clean common harness checkout, `REFERENCE` the clean H checkout,
`WORK` a new empty directory. Clone a supplied bundle with explicit branch
`codex/pacer-continuation-fast-path`. Verify the handoff's full harness and
bundle hashes, both product objects and the unchanged peer before any build.

```sh
for pin in \
  8e1bdebed836cb7b732db852f51ef6a7b212e925 \
  a56b2c5642f8a04ae75cb35650c8120c278f19cf; do
  git -C "$SRT" cat-file -e "$pin^{commit}" || exit 1
done
python3 -m unittest discover -s "$SRT/interop/tests" \
  -p 'test_pacer_continuation_ab.py' -v || exit 1
python3 -m unittest discover -s "$SRT/interop/tests" \
  -p 'test_scalability_scorecard.py' -v || exit 1
git -C "$SRT" worktree add --detach "$WORK/baseline-source" \
  8e1bdebed836cb7b732db852f51ef6a7b212e925 || exit 1
git -C "$SRT" worktree add --detach "$WORK/candidate-source" \
  a56b2c5642f8a04ae75cb35650c8120c278f19cf || exit 1
for variant in baseline candidate; do
  python3 "$SRT/benchmarks/prepare_throughput.py" \
    --robotweax-source "$WORK/$variant-source" --reference-source "$REFERENCE" \
    --output-directory "$WORK/$variant-plain" --linkage static --jobs 2 || exit 1
done
```

Nine ABBA/analysis/operator tests include six operator scenarios: success,
runner failure, SIGINT, SIGTERM, SIGHUP and failed first UDP restoration with
independent second restoration. Scorecard tests prove real recorded peer wait
statuses on success and failure. Synthetic tests do not transfer SRT payload
or change real sysctls. Preserve and stop on preparation failure.

Build two fresh static Release/OpenSSL pairs using the common harness:
`-O3 -DNDEBUG` and `-g` for libraries, `-O2 -g` for the unchanged peer.
All builds finish before measurement. Preserve manifests, commands, original
statuses, compiler/OpenSSL logs, caches and all four library/four peer hashes.

## Exactly 24 serial cases

Order **A/B/B/A**, where B means this new candidate, not the historical deadline
variant. Each block: 30-second cooldown, H→H control, two R→R measurements,
two R→H measurements, H→H control. No warmups. **16 measurements + 8 controls**,
four measurements per variant/profile. No replacement cases or retries.

Every case: one stream, 128 MiB requested, rounded to 101990 messages × 1316
bytes = 134218840 actual bytes. IPv4 loopback, Live/Message, TSBPD 120 ms,
TLPKTDROP off, Pending 8192, FC 16384, SRT buffers 32 MiB, UDP buffers 8 MiB,
MAXBW 1250000000 bytes/s, target 0, timeout 45 s, shutdown 500 ms.

Run the existing outer operator once. It temporarily sets both UDP maxima to
33554432 and independently restores the previous values. Existing `sudo -n`
only; no new privileges or security changes.

```sh
python3 "$SRT/benchmarks/run_capacity_operator.py" \
  --output-directory "$WORK/operator" -- \
  python3 "$SRT/benchmarks/run_pacer_continuation_ab.py" \
    --baseline-plain "$WORK/baseline-plain/build-manifest.json" \
    --candidate-plain "$WORK/candidate-plain/build-manifest.json" \
    --output-directory "$WORK/pacer-results"
operator_status=$?
printf '%s\n' "$operator_status" > "$WORK/operator.exit-code.txt"
```

All four blocks run despite ordinary case/report failures or poor performance.
Signals stop the plan; the runner owns case-group cleanup. Preserve observed
128+signal status, secondary cleanup errors and original driver/block codes.

## Evidence and gate

Every case, including controls: complete deterministic payload, exact options,
raw peer PIDs/wait returncodes, both endpoint CPU/context switches, complete
public packet counters, zero retransmissions and explicit zero UDP `InErrors`,
`RcvbufErrors`, `SndbufErrors`, `InCsumErrors` deltas. Missing evidence fails.
`peer-exit-status.json` stores actual wait returncodes after cleanup, including
failure cases; a missing/unspawned peer is absent, not zero. Successful cases
also serialize `peer_exit_codes` and carry the raw sidecar into `case.json`.

All eight H controls require max/min ≤1.15. Medians use all four measurements
per variant/profile; CPU and context switches normalize actual bytes. Add
endpoint CPU per case before the median. Report every sample and resource
ratio; no significance or single-syscall causal claim.

Candidate versus A must meet **both profiles**: throughput ≥0.95, sender CPU/GiB
≤1.05, total CPU/GiB ≤1.05. At least one profile must improve throughput ≥5% or
reduce total CPU/GiB ≥5%. Reasons are present even on incomplete evidence.

Exit **0** means a valid completed comparison, possibly with no eligible
candidate; **1** means failed/incomplete case evidence; **2** means invalid H
control stability. Check `analysis.candidate_gate` and `followup_candidates`
separately from `complete`. The runner never performs long-transfer/scaling
qualification. No 1-GiB or ten-stream cases, parameter sweep or merge belongs
to these 24 attempts. A successful candidate needs those later stages.

## Deliverables

New German Lab report/PR, all raw requests/cases/peer output/exit-status files,
four block reports, `pacer-fast-path-report.json`, independent arithmetic and
gate reasons, two complete build evidence sets, source/harness/peer exports,
operator and independent postflight evidence, preserved preparation errors and
SHA-256/size manifests. No binaries, Haivision artifacts or full build trees.
Verify both UDP values restored, security/timer slack unchanged, clean sources,
all eight binary hashes unchanged and no case/peer/profiler processes remaining.
