# Experimental reorder-budget evaluation

This branch is for comparative testing only. **Do not deploy it as a release.**
It is intentionally not merged into `main` and does not change the release version.

Branch: `codex/reorder-budget-evaluation`.
Pinned control: `8e1bdebed836cb7b732db852f51ef6a7b212e925` (main integrated in this update).
Record the candidate commit with `git rev-parse HEAD` for every result.

The branch now includes main's changes through the throughput fast path and
Lite-ACK window-credit correction (PR #32). Its experimental reorder behavior
remains separate from main. The original control was
`ac9c14d7815bfd6f76f8b60308c5465095625eb7`; preserve historical results with that
pin, but use the updated control for new matched comparisons. Do not pool the
old and new cohorts or treat this synchronization as performance qualification.

## Initial local verification

On 2026-09-08, the macOS Release build passed all 595 cases in
`robotweax_srt_tests`, including the new reorder-budget cases. This is a local
core-suite result, not Linux/Windows, full interoperability, or A/B performance
qualification. Those remain outstanding for this combined candidate.

## What is enabled

- Learned reorder tolerance decays after 2,000 ordered packets instead of 50.
- Each fresh gap has a maximum additional wait of 2 ms, independent of further DATA.
- The earliest deadline is cached and passed separately to the runtime timer,
  without using the pacing sub-millisecond immediate-work loop.
- Live TSBPD/TLPKTDROP connections enable a provisional recovery reserve:
  smoothed RTT + 4 × RTT variation + 2 ms. This is an estimate, **not a bound**.
- Existing deadlines can only shorten when the first complete message's drop
  deadline or reserve changes. Unknown or insufficient budget causes immediate
  report eligibility. The actual scheduler and network can still execute late.
- Packet-count expiry and periodic NAKs remain independent.

The explicit internal reserve override exists for deterministic tests only.
Normal connections use the estimate above. Startup RTT/variation values reserve
302 ms, so short-latency connections initially cannot add reorder waiting.
There is no new public C API or socket option. Non-TSBPD/File traffic does not
use this Live budget rule, and needs separate qualification.

**`SRTO_LOSSMAXTTL=0` remains the default.** It does not learn a positive waiting
tolerance. To exercise reorder waiting, the receiving application must set a
positive value using the existing API, identically on both builds, for example:

```cpp
int maximum_reorder = 5;
if (srt_setsockflag(socket, SRTO_LOSSMAXTTL,
                   &maximum_reorder, sizeof(maximum_reorder)) == SRT_ERROR) {
    // Report the error and abort this test configuration.
}
```

For listeners, set the option before accepting connections. Verify the effective
option on the actual connected socket. The UDP bridge CLI currently does not
expose this option; merely switching its branch does not select a positive value.

## Build isolated controls

From a clean clone, fetch the branch and create separate worktrees:

```sh
git fetch origin main codex/reorder-budget-evaluation
git worktree add --detach ../srt-control 8e1bdebed836cb7b732db852f51ef6a7b212e925
git worktree add --detach ../srt-candidate origin/codex/reorder-budget-evaluation
cmake -S ../srt-control -B ../srt-control-build -DCMAKE_BUILD_TYPE=Release
cmake --build ../srt-control-build --config Release --parallel 2
cmake -S ../srt-candidate -B ../srt-candidate-build -DCMAKE_BUILD_TYPE=Release
cmake --build ../srt-candidate-build --config Release --parallel 2
ctest --test-dir ../srt-control-build -C Release --output-on-failure
ctest --test-dir ../srt-candidate-build -C Release --output-on-failure
```

Use the same compiler, OpenSSL, build flags, application and reference peer.
Never reuse a CMake cache across worktrees. For shared builds, verify the loaded
library path: both builds retain the same version and public C ABI identifiers.
Do not install one over the other system-wide. Follow the normal platform build
prerequisites in the repository documentation.

## Fixed qualification plan

### Stage 1: correctness before performance

Run the enabled budget path, not only a test-injected reserve or disabled path.
Cover deadline shortening, fragmented messages, recovery before expiry, expired
and unknown budget, initial RTT values, RTT changes, timestamp wrap/drift,
option changes, split loss ranges and default maximum 0. Preserve every failure.
Exercise shutdown and socket lifetime with sanitizers. Do not claim complete
qualification from the focused tests alone.

### Stage 2: matched A/B profiles

Four configurations separate opt-in effects from default behavior:

| Build | LOSS MAX TTL | Purpose |
| --- | ---: | --- |
| pinned main | 0 | public default control |
| candidate | 0 | default-path regression control |
| pinned main | 5 | matched positive-tolerance control |
| candidate | 5 | experimental waiting and budget path |

Use 20 serial pairs per profile, alternate A/B order, fixed fault seeds, no
parallel builds or load tests. Include no faults, rare depth-1 reorders,
reorder plus real loss, burst loss, visible gap followed by silence, fragments,
and concurrent connections. Start with 20/120 ms latency and 1/10 ms injected
RTT; clearly label combinations with insufficient recovery budget. Add higher
latency/RTT only as a separately recorded expansion. Never overwrite a failed
run or rerun until green. Repeat a complete cohort if its environment is invalid.

Measure payload integrity, delivered counts, final drops, unnecessary NAKs and
wire retransmissions, recovery median/p95/p99/max, CPU, scheduler task counts
and effective latency. Keep per-run data; do not average quantiles together and
call that a pooled packet percentile. The missing final packet with no following
packet is a separate tail-RTO case, not the visible-gap silence profile.

### Stage 3: platform checks

Linux and Windows via targeted CI; macOS locally. Keep this branch unmerged.
Do not launch the complete expensive interop matrix repeatedly for unchanged
test documentation. Core code changes still require the applicable full gates
before any eventual merge; do not weaken classification or required checks.

## Provisional acceptance and stop criteria

- Zero payload corruption, crashes, sanitizer/lifetime failures or unbounded waits.
- No additional final drops in matched cases where the control succeeds.
  Any such event stops promotion and remains an open failure.
- Maximum-0 deterministic outputs must remain equivalent; real maximum-0 runs
  must meet the same functional gates. Timing noise is recorded, not suppressed.
- Rare-reorder profiles should reduce unnecessary NAKs by at least 50% at the
  same positive tolerance; otherwise the added complexity is not justified.
- Recovery p99 must not increase by more than 2 ms in matched cohorts. The
  no-additional-drop gate takes precedence, particularly at 20 ms latency.
- Median CPU increase must be at most 10% in matched cohorts. Overlapping/noisy
  measurements are inconclusive, not a pass. Multi-connection scheduler results
  must show no runaway immediate-work loop.
- Thresholds are evaluation targets, not protocol guarantees. Do not relax them
  after observing a failure; any revised plan is a separately identified cohort.

If these gates cannot be met, retain main's current behavior and archive the
candidate as an evaluated experiment rather than merging it.

## Known limitations and reporting

### Main synchronization verification (2026-09-12)

After integrating main at `8e1bdebed836cb7b732db852f51ef6a7b212e925`, a clean
macOS Release build with AppleClang 21, OpenSSL 3.6.3 and Python 3.13 passed all
54 registered CTest tests and all 621 Python harness tests. The documentation
validator and changed-line C++ format check also passed. The experimental
reorder rules were not changed by this synchronization. These local checks do
not replace the matched transport evaluation or Linux/Windows qualification.

A previous fixed-deadline experiment lost one packet at 20 ms configured latency
with about 37.7 ms observed recovery. It remains unresolved. Later successful
runs do not erase that signal, and the enabled automatic reserve in this branch
is a new combination requiring its own evidence.

Budget reevaluation scans loss/message state; its enabled-path cost is not
qualified by earlier cached-expiry microbenchmarks. RTT estimates do not bound
OS scheduling or sender delays. Independent crypto review and full release
qualification are outside this experiment.

Report: candidate/control SHA, OS/CPU/compiler/OpenSSL, commands and loaded
library paths, effective options, fault seed, repeat order, raw results, failures
and uncertainty. Do not redistribute Haivision binaries with results.
