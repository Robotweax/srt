# Throughput profiling (diagnostic branch)

This diagnostic branch now includes an **experimental empty-receive-buffer
fast path**, not a performance qualification or a released throughput claim.
Only an empty message search returns earlier; public API, locking and transport
configuration remain unchanged. Use a dedicated Linux test VM to reproduce a Linux
capacity observation; native macOS and hosted x86-64 CI are separate platform
controls, not substitutes for that environment.

## Measurement contract

The first stage deliberately isolates **cleartext, one connection, IPv4
loopback**, Robotweax → Robotweax and Robotweax → Haivision. The same public-API
`benchmarks/scalability_peer.cpp` is compiled against both libraries. Defaults
of the older scalability scorecard remain unchanged; capacity settings are
explicit additional CLI arguments, never ambient `BENCH_*` variables.

| Setting | Capacity default |
|---|---|
| Payload / transfer | 1,316 bytes / at least 128 MiB, rounded up to whole messages |
| Mode | Live / Message API; TSBPD 120 ms; TLPKTDROP off |
| Cipher | None: isolate transport before CTR/GCM follow-up |
| MAXBW | Explicit 1,250,000,000 bytes/s (10 Gbit/s ceiling) |
| SRT send / receive buffer | 32 MiB each |
| Flow window / application pending limit | 16,384 / 8,192 packets |
| Requested UDP buffers | 8 MiB each |
| Sender workset | Up to 64 messages/socket, 128/round, then yield |
| Application pacing | Unpaced by default; optional `--target-bps` |
| Timeout / shutdown grace | 45 s / 500 ms |

These are **capacity diagnostics**, not a recommended production Live
configuration. TLPKTDROP is off to retain late payload; this cannot establish
Live deadline compliance. MAXBW=-1 in other scorecards must not be assumed to
mean unlimited capacity on every implementation.

Uninstrumented blocks contain one Haivision self-control before and after,
one excluded warmup per selected direction, and five serial measurements per
direction. Every failed attempt is kept; there are no automatic retries.
Profile blocks contain one separately instrumented attempt per selected
direction by default. Their rates are never pooled with baseline summaries.

## 1. Prepare clean identified builds

Requirements: Linux or macOS, Git, CMake >=3.20, a C++20-capable compiler,
Python >=3.10, pkg-config, and OpenSSL >=3 with development headers. On Ubuntu
the core packages are `build-essential cmake git pkg-config libssl-dev`.
For capture also install the kernel/distribution's `perf` tools, commonly
`linux-tools-common` and `linux-tools-$(uname -r)` or `linux-tools-generic`.
Check `perf --version`; a hosted/custom kernel may need the actual executable
under `/usr/lib/linux-tools*/` rather than a mismatched `/usr/bin/perf` wrapper.

From a **clean checkout** of this diagnostic branch:

```sh
SRT="$PWD"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/srt-throughput.XXXXXX")"
git clone --depth 1 --branch v1.5.7 https://github.com/Haivision/srt.git "$WORK/reference"
python3 benchmarks/prepare_throughput.py \
  --robotweax-source "$SRT" --reference-source "$WORK/reference" \
  --output-directory "$WORK/build-static" --jobs 2
```

The builder verifies the exact reference commit
`899348d8318eb9a3c5a5b6ec43c4a1114288773a`, rejects dirty library sources, uses a
new output directory, and records both Git revisions, helper identity, all
build commands, compiler/OpenSSL information, CMake caches, executable/library
SHA-256 hashes and loader dependencies. No existing build cache is reused.

Libraries use **Release with `-g`**, preserving CMake Release optimization.
Peers use identical `-std=c++17 -O2 -g` flags, matching the original lab's
peer optimization level. No sanitizers, Tracy instrumentation, extra frame
pointers or changed optimization flags are added. CPU recording uses DWARF
unwinding. Keep unstripped binaries and exact symbols locally for analysis.
`--allow-dirty-robotweax` is only for development smoke tests and is prominently
recorded; it is not suitable for release comparisons.

Static linking is the default because the September 11 capacity report used
static archives. Use `--linkage shared` with a **different fresh output
directory** for a separate linkage control. Both libraries still use OpenSSL.
Keep the same linkage for every before/after comparison.

To reproduce the original Robotweax library revision rather than current main:

```sh
git worktree add --detach "$WORK/robotweax-original" 7b6467ac8bc262ab5f35ac2143b39597cf2cc70e
python3 benchmarks/prepare_throughput.py \
  --robotweax-source "$WORK/robotweax-original" \
  --reference-source "$WORK/reference" \
  --output-directory "$WORK/build-original" --jobs 2
```

The helper is always taken from the diagnostic branch, not the old library
checkout. Record this distinction: it reproduces the settings and integrity
algorithm, not the byte-identical historical helper. Extra end-of-run resource
and option observations are outside the per-packet hot loop.

## 2. Check UDP limits, do not silently tune the host

```sh
sysctl net.core.rmem_max net.core.wmem_max kernel.perf_event_paranoid kernel.kptr_restrict
```

The runner refuses a Linux capacity run if the configured UDP request exceeds
either kernel maximum. It never calls sudo or changes sysctls. API readbacks
are retained, but **are not proof of actual OS buffer allocation**. UDP error
deltas from `/proc/net/snmp` are VM-global and can include unrelated traffic.

Only if this is a dedicated test VM and you approve the temporary change:

```sh
old_rmem="$(sysctl -n net.core.rmem_max)"
old_wmem="$(sysctl -n net.core.wmem_max)"
trap 'sudo sysctl -w net.core.rmem_max="$old_rmem" net.core.wmem_max="$old_wmem"' EXIT
sudo sysctl -w net.core.rmem_max=33554432 net.core.wmem_max=33554432
```

Run the measurements and then restore/verify both values in the same shell.
Never change production-host network settings for a benchmark. The explicit
`--allow-small-udp-buffers` override permits a labelled small-buffer experiment,
not a successful large-buffer capacity qualification.

## 3. Uninstrumented serial baseline

After builds have finished, no concurrent load tests or builds:

```sh
python3 benchmarks/throughput_diagnostics.py \
  --build-manifest "$WORK/build-static/build-manifest.json" \
  --output-directory "$WORK/baseline-01" --cooldown-seconds 30
```

The default block is 14 short transfers (two controls, two warmups, ten
measurements), not a broad cipher/fault/platform matrix. Optional independent
experiments use a new output directory each time:

- `--profile robotweax-to-haivision` isolates the Robotweax sender.
- `--profile haivision-to-robotweax` controls the Robotweax receiver.
- `--target-bps 250000000` changes only offered application rate.
- `--pending-packets 512` changes only the application window.
- `--bytes-per-connection 536870912` extends a too-short capture/transfer.

The runner checks recorded executable/library hashes before starting. It does
not overwrite result directories. A throughput summary contains median, p95,
mean and explicit successful/failed counts. These percentiles describe repeated
transfer rates, **not packet latency**, and are conditional on complete
transfers. Throughput uses the existing scorecard interval from before caller
launch until both completion events are observed, including setup and drain
but excluding the subsequent shutdown tail. Endpoint completion durations are
also retained. Do not compare these with differently defined payload-only rates.

## 4. Separate CPU profile

```sh
python3 benchmarks/throughput_diagnostics.py \
  --build-manifest "$WORK/build-static/build-manifest.json" \
  --output-directory "$WORK/cpu-01" --capture cpu --cooldown-seconds 30
```

This uses `perf record -e cpu-clock -F 199 --call-graph dwarf,8192` around one
driver plus its descendant peers per case. It does not require a virtualized
hardware cycle counter. Event access and stack unwinding still depend on the
guest kernel and permissions. A permission failure, absent perf data, rendering
failure, or zero recorded samples for either endpoint is **not** an unprofiled
success. If permissions block capture, review the kernel policy with the test
host administrator; on a dedicated VM you may explicitly run the command with
sudo. Do not disable host security policy globally merely to make the test green.

Artifacts include `perf-report.txt`, `perf-stacks.txt`, `sample-coverage.json`,
and local `perf.data`. The report contains peer PIDs and role/provider mapping.
Filter by these PIDs: the Python observer is included in the capture, and setup,
steady transfer and drain are all present. Inspect endpoint samples and stack
quality before attributing costs; nonzero samples alone do not guarantee useful
unwinding. DWARF stack dumps are bounded at 8 KiB and may truncate deep stacks.
Increase trace length before raising sample frequency indiscriminately.

Read CPU-heavy paths first: packet selection, queueing, readiness notifications,
runtime scheduling, serialization, `sendto`, receiver dispatch, ACK processing,
and the application's generator/validation loop. User/system CPU and voluntary/
involuntary context switches are also reported **separately by endpoint process**
through its completion event; they include setup, not just payload handling.

## 5. Separate scheduler trace

```sh
python3 benchmarks/throughput_diagnostics.py \
  --build-manifest "$WORK/build-static/build-manifest.json" \
  --output-directory "$WORK/scheduler-01" \
  --capture scheduler --allow-system-wide --cooldown-seconds 30
```

`perf sched record` uses **system-wide scheduler tracepoints**. It normally
needs elevated permissions and must only run on a dedicated machine without
sensitive workloads. The script requires explicit consent. The rendered
`perf-scheduler.txt` filters the endpoint process IDs; retain the local raw trace
privately. Check event availability and PID/TID coverage. Some kernels/VMs or
CI runners do not permit these events; that is an explicitly reported capture
failure, not evidence that there are no waits.

**Known qualification limitation:** on Linux 6.8/perf 6.8.12 ARM64, native
timehist summaries mentioned both PIDs but omitted seconds of worker runtime.
The earlier PID-presence acceptance rule was a false positive. Scheduler
captures now always remain `valid=false` pending independent worker-thread
coverage and runtime-plausibility review, even if recording and rendering
succeed. Logs, raw data and the transfer verdict are retained separately;
the diagnostic command exits nonzero. This is a conservative guard, **not a
repair of the native renderer**. A validated thread-family/runtime coverage
checker is still needed before restoring automatic scheduler acceptance.
Do not infer absence of contention from an incomplete summary. The first
empty-receive-buffer A/B experiment uses plain blocks and separate CPU
captures; it does not require a qualified scheduler summary.

Scheduler traces show running, waiting and scheduling delay. They do not by
themselves prove which C++ mutex caused a wait. If the critical path remains
ambiguous, the next stage is narrowly scoped off-CPU/lock tracing or optional
Tracy zones. **Tracy is not yet integrated, and no per-packet transport counters
or new telemetry API have been added.** Packet-per-poll and pacing-reason
instrumentation should be selected after these initial profiles.

Captures have a 128-MiB per-file limit and bounded process-group timeouts.
Reaching a limit is a failed/partial capture. Build logs and transfer failures
are retained. Perform the next plain control block without the profiler.

## 6. macOS smoke and Instruments

The same builder/runner can validate both peers locally. Example short smoke,
using a new directory and an explicit conservative macOS UDP request:

```sh
python3 benchmarks/throughput_diagnostics.py \
  --build-manifest "$WORK/build-static/build-manifest.json" \
  --output-directory "$WORK/mac-smoke" --repetitions 1 --warmups 0 \
  --bytes-per-connection 2097152 --udp-buffer 262144 --cooldown-seconds 0
```

This is a functional check, not Linux capacity evidence. Use Xcode Instruments
Time Profiler/System Trace for separate native macOS investigation. The Linux
`--capture` modes intentionally refuse to run on macOS instead of pretending an
unprofiled run is equivalent. Keep those results in a different evidence set.

## 7. Manual GitHub Actions only

The existing **Timing diagnostics** workflow accepts three additional choices
on this branch. These jobs run only for explicit `workflow_dispatch`, not on
push, pull request or schedule. The existing workflow is already registered on
main, so target the diagnostic branch explicitly:

```sh
gh workflow run timing-diagnostics.yml --repo Robotweax/srt \
  --ref codex/throughput-send-path -f investigation=throughput-baseline
# After reviewing the baseline, separately:
gh workflow run timing-diagnostics.yml --repo Robotweax/srt \
  --ref codex/throughput-send-path -f investigation=throughput-cpu
# Only if scheduler evidence is needed:
gh workflow run timing-diagnostics.yml --repo Robotweax/srt \
  --ref codex/throughput-send-path -f investigation=throughput-scheduler
```

Each job has a 15-minute budget, clean static Release builds, fixed reference,
two build workers, and a 30-second cooldown. CI is Ubuntu x86-64, **not** the
original Ubuntu ARM64 VM; record that platform distinction. The ephemeral
runner temporarily increases UDP maxima and restores them in an EXIT trap.
Capture commands run with explicit sudo, without altering perf security
sysctls. Baselines run as the normal runner user; profile rates are not baseline
results. Jobs can fail if hosted-kernel tracing restrictions prevent capture.

Only JSON/text/log evidence is uploaded (14-day retention), including failed
attempts. **No Haivision libraries, executables, perf build-ID archives or raw
system-wide scheduler traces are uploaded.** Raw profiles stay on the runner;
CPU stacks/reports are symbolized there before teardown. Payloads are synthetic.
The workflow never downloads code from the private Network Lab repository.

## Review and next change

Required before optimizing: repeatable plain baseline, intact payloads, both
endpoint profiles with useful symbols, explained UDP errors, and a dominant
cost or wait tied to the sending critical path. CPU percentages are not proof
of causality. Validate an optimization with identical before/after blocks on
the same host, and retain all incomplete transfers.

The first profiles selected the empty receive-side message search in an active
sender, not `SendBuffer::next_packet()`. This branch therefore adds only an
`occupied_ == 0` return to `ReceiveBuffer::first_complete_message()`. Occupied
buffers follow the original search; no send-only shortcut disables receiving.
The send-buffer cursor and pacing/notification changes are separate work with explicit
burst, fairness, lifetime, loss-recovery and timing regression tests. Neither
more permissive timeouts nor disabled integrity checks count as optimizations.

No hard throughput gate is introduced on noisy hosted runners. The immediate
milestone is to explain the sender plateau; parity and leadership claims need
matched cross-provider, multi-connection and physical-network evidence.

## Empty-receive-buffer A/B handoff

Use the same dedicated Ubuntu ARM64 VM as the original profiles. Native macOS
tests and hosted x86-64 CI cannot establish the before/after result for that VM.
The two library revisions are:

- Baseline: `a5c428b725b5373651041b0e30678406b2d5c38c`.
- Candidate: `40387f2dc70b7701b266be1c91f048f2d4fa3b61` (five implementation lines plus regression tests).
- Haivision: `899348d8318eb9a3c5a5b6ec43c4a1114288773a` in both builds.

From a clean checkout of the current diagnostic branch, prepare both source
worktrees and builds before any measurement. Use this same checkout's harness
for both; it records its own identity separately from the library source.
`scalability_peer.cpp` has not changed between baseline and candidate.

```sh
SRT="$PWD"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/srt-empty-receive-ab.XXXXXX")"
git worktree add --detach "$WORK/baseline-source" a5c428b725b5373651041b0e30678406b2d5c38c
git worktree add --detach "$WORK/candidate-source" 40387f2dc70b7701b266be1c91f048f2d4fa3b61
git clone --depth 1 --branch v1.5.7 https://github.com/Haivision/srt.git "$WORK/reference"
for version in baseline candidate; do
  python3 "$SRT/benchmarks/prepare_throughput.py" \
    --robotweax-source "$WORK/$version-source" \
    --reference-source "$WORK/reference" \
    --output-directory "$WORK/$version-build" --jobs 2
done
```

Apply only the approved temporary UDP maxima from section 2, keeping the
restore trap in the same shell. Stop other builds/load tests. Run four serial
plain blocks in **baseline → candidate → candidate → baseline** order to expose
host drift. Each block retains the unchanged 14-case contract: two reference
controls, two warmups and ten measurements. Do not increase parallelism or
change pending limits, payload, buffers, MAXBW, timeouts or loss policy.

```sh
index=0
for version in baseline candidate candidate baseline; do
  python3 "$SRT/benchmarks/throughput_diagnostics.py" \
    --build-manifest "$WORK/$version-build/build-manifest.json" \
    --output-directory "$WORK/plain-$index-$version" --cooldown-seconds 30
  status=$?
  printf '%s\n' "block=$index version=$version exit=$status"
  index=$((index + 1))
done
```

Every output directory is new. Keep nonzero statuses and failed attempts;
never rerun them away. A shell using `set -e` may stop at the first failure:
retain it and investigate before continuing rather than disabling a gate.
Compare median, mean and p95 rates per direction and per block, as well as
complete counts, CPU time, retransmissions, UDP error deltas and reference
controls. Do not pool captures/warmups/controls or hide incomplete transfers.

Then run `--capture cpu` separately for each build, with fresh output
directories and the same parameters. Review symbols, endpoint sample coverage
and the absolute CPU cost as well as the relative share of the receive search.
If the optimized transfer is too short for useful samples, retain that result;
a separately labelled longer-duration experiment must use the same larger
transfer on **both** revisions. Never lengthen only the candidate capture.

Acceptance requires correct payloads, no regression of the existing contracts,
repeatable throughput gain beyond host drift, and reduced work in the measured
receive scan. This experiment does not establish WAN/media-deadline behavior,
multi-connection scaling, encrypted throughput or superiority to Haivision.
No Linux before/after improvement is claimed until those results are reviewed.
