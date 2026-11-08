<!-- SPDX-License-Identifier: MIT -->
# PR #387 review follow-up

Source: [complete technical review](https://github.com/Robotweax/srt/pull/387#issuecomment-6722547185), reviewed head
`4af9c86cc86bd915fb67bda22463bbd0ec068a05`, 8 November 2026. The reviewer reports
AI-assisted area reviews, manual verification, local builds and CI inspection.
Findings are input for verification, not permission to weaken assertions or
change protocol/security policy. Positive observations do not constitute final
human security or merge sign-off.

## First batch: build blockers and evidence correction

Code commit: `e710d4e6f9ab027483e399f222c8cef7837c8d5d`.

- BL-1 reproduced locally before the fix in a fresh repository-default build:
  unguarded `SRTO_CRYPTOMODE`. The guarded source builds; true default passes
  84/84 gates with AEAD/HC/ACP1 OFF and ordinary test/tool/benchmark defaults.
- BL-2 reproduced from the old Win32 MSVC C4244/C2220 log. The explicit cast is
  after a minimum bounded by `config.maximum_unicast_members` (`size_t`), so no
  value is truncated even on Win32. Budget/coordinator tests pass. Hosted CI at `18b4595` now passes native Win32/x64 builds and selected
  provider tests, plus shared x64 Debug/Release jobs; see
  [Windows evidence](records/review-harness-windows-ci.json). ARM64 is cross-build
  evidence only. This is not full native Windows hybrid qualification.
- Enabled Release and sanitizer builds each pass 9 budget + 7 wire-budget + 23
  coordinator + 6 investigation cases. The original GCM fixture still runs
  under AEAD ON. No transport deadline, warning policy or test assertion changed.
- The former "default" packet label was wrong: HC/ACP1 were OFF, AEAD was ON.
  It is now called hybrid-off/AEAD-on. Historical raw filenames are retained.
- Build/source attestation is still missing from the soak driver. Historical
  records stamp checkout HEAD and whatever binary is supplied; hashes identify
  artifacts, not an enforced commit relationship. Original records are not
  edited to imply stronger provenance. Failed-build configuration sidecars use
  matching cache digests; the diagnostic lacking source identity stays unbound.

## All review findings

"Open" means not fixed or not yet independently reproduced in this follow-up.
Only the evidence above is claimed for the first batch; no blanket resolution.

| ID | Current state | Next verification / acceptance |
| --- | --- | --- |
| BL-1 | Fixed in source; local true-default build and 84 gates pass | Inspect fresh hosted default-build jobs |
| BL-2 | Fixed; hosted Win32/x64 builds and selected provider tests pass at 18b4595 | Broader Windows hybrid failures remain BL-3/BL-4/BL-5; no local Windows claim |
| BL-3 | Partitioning implemented at 2f31b96; local results below; hosted and residual budget checks pending | Keep all cases/deadlines; inspect current CI and remaining GCC CPU-oracle/rotation costs |
| BL-4 | Open: Windows `srt-live-transmit -version` hang | Native debugger/thread evidence and same-base comparison |
| BL-5 | Native wire bound retained; forced waits checked with injected clock and zero-wait counterexample | Native Windows after-fix CI pending; source fix does not qualify Windows hybrid transport |
| BL-6 | Partial: scope/labels corrected; matching-cache failure flags recovered | Enforce build/source identity, reject stale binary, test CLI record production; obtain missing platform evidence |
| L-1 | Open: ACK/NAK versus pending completion fanout | Reproduce genuine early control versus forged/unsubmitted control; preserve actual-submission authorization |
| L-2 | Open: completion-busy attempt count versus send rate | Production-default send-driven contention regression and time-bounded policy |
| L-3 | Open: detach-last-member during one send | Assert no silent successful un-emitted sequence on mid-admission detachment |
| L-4 | Open: one member admission contention/backpressure | Quantify retry/liveness with healthy-peer traffic and bounded work |
| L-5 | Open: per-message TTL causing permanent detachment | Deterministic expiry rollback/member-state test |
| L-6 | Open: prepared crypto generation advances on aborted admission | Cancelled-guard/prefix-recovery reproduction |
| L-7 | Open: overlap completion order/statistics | MC-first/UC-first physical-submission counter regression |
| L-8 | Open: bounded but repeated hot-path scans | Measure work and prove appropriate bounds before optimization |
| T-1 | Open: candidate interception in default build | Verify intended ordinary-control behavior and default-off isolation; regression before gating |
| T-2 | Open: coordinated activation on ordinary group setup | Gate/select activation without changing close-race error semantics |
| T-3 | Open: equal endpoints/re-listen and documented contract | Reproduce native ownership/bind behavior; explicitly support or reject/document |
| T-4 | Open: native source syscalls under record lock | Check rollback/close races before moving work outside lock |
| T-5 | Open: nonzero repair threshold at coordinated listen | Consistent rejection or explicit ignored-option contract, with tests |
| T-6 | Open: multicast stats before provenance admission | Define routed/admitted/filtered counters and test each drop |
| T-7 | Open: stale route retains runtime until unregister | Rebind/unregister lifetime and counter regression |
| T-8 | Open: source readback and late-option error contracts | Tests and precise documentation |
| H-1 | Open: initial join deadline precedes publication gate | Inject delayed fallback/publication and prove join is re-armed or explicitly fails |
| H-2 | Open: live-stream automatic return/high-water/lifetime budget | Native continuous-stream automatic fallback/return test with delayed reports |
| H-3 | Open: report/query freshness configuration | Define valid interval/round/lifetime bounds and rejection tests |
| H-4 | Open: progress/setup clock domains | End-to-end injected-clock regression without wall-clock mixing |
| H-5 | Open: terminal response policy and one-shot fallback | Validate/document manual backstop and transient-refusal semantics |
| H-6 | Open: receiver lock across join/thread start | Delayed join plus query-service/freshness contention regression |
| S-1 | Open: first valid foreign-nonce OFFER consumes attempt | Test precise availability contract before considering any protocol retry change |
| S-2 | Open: source announcement authentication timing | Document trust boundary; cryptographic transcript changes need separate compatibility review |
| S-3 | Open: pre-auth queue starvation/overflow | Bounded adversarial queue model and explicitly chosen availability contract |
| S-4 | Open: storage assertions/version/credential-strength documentation | Add meaningful static bounds and precise supported-version/entropy limits |

## Harness follow-up

Code: `64591511d588416f55123a05bd598c3ef510c412`. [Commands, identities, before/after controls and
native results](records/review-harness.json) are separate from historical
qualification. The invalid-duration exception happens before fault-worker
creation: teardown cannot be its sole cause. A packet-pool-only change on the
same clean baseline/flags restores normal GCC Og/O1 exception reporting.
No noexcept declaration or sanitizer instrumentation was removed. This is a
storage-layout-dependent observation; compiler-defect and historical DATA-loss
causality remain unproved.

Fault cleanup is registered before configuration can fail and runs before the
first owned socket closes. Key and normal callbacks are published after worker
creation succeeds. Four new regressions force failure before construction,
during partial construction, on the producer with active readers/workers, and
inside a reader; cleanup audits check joined workers and a still-open listener.
The new serial harness gate has its own 30-second budget. Multicast dispatch
also runs serial because it shares the fixed loopback group/port. Soak/setup
waits are labelled; DATA diagnostics include membership epoch and control state.
The original 200 ms proof/playout, fault model and recovery limits are retained.

Repository-default CTest passes 84/84 and Python 854/854. Native validation is
narrower than full requalification; inspect successes and preserved failures
in the record. Hosted build-fix evidence resolves the Win32 compilation blocker,
not the full Windows hybrid profile. GCC O0 and outstanding canonical DATA,
suite timing, automation and source/binary attestation findings remain open.

## Additional confirmed receive-key finding

**KM-1 remains open:** old-generation DATA above the receive frontier at KM
replacement is routed to the replacement key. A standalone public-API diagnostic
fails CTR/GCM without clocks/network in five configurations; four controls per
build match and the two lagging-frontier round trips fail. Exact opaque key-ID
traces bind wrong selection to the reported failing DATA position in all four
native v2 captures. CTR can silently publish wrong plaintext; GCM rejects.
The affected receive implementation is unchanged from Robotweax v0.3.2. This is
not a GCC-only or sanitizer-only issue, and no production fix is claimed.

[Canonical diagnosis and limits](README.md#gcc-datakm-diagnosis-receive-generation-misrouting)
and [record](records/review-gcc-km.json) preserve every diagnostic failure.
All original 32 finding states remain; this additional blocker does not close
L-3 or explain distinct freshness failures. Safe CTR generation/lifetime binding
and bounded authenticated GCM selection need dedicated production regressions.
No payload-format guessing, widened deadline or sanitizer exemption is accepted.

## Deterministic listener-close follow-up

`4d59666ebaf54df57b514d1069a5172df440af51` adds four deterministic real-actor close cases:
idle/admission pending, with/without service pressure, each behind overdue timers.
All four fail before the fix and pass afterward; existing timer/FIFO/service
ordering and lifecycle checks remain. Ordinary timer/FIFO dispatch now alternates
so queued close work progresses. Tests use a callback-count checkpoint and finite
pressure, without network timing or sleeps. The bound assumes callbacks return.

[Current result tables](README.md#deterministic-listener-close-regression-and-scheduler-fix)
and [record](records/review-listener-close.json) include eight 78-case focused
builds, ASan/UBSan/TSan, fresh full macOS 88/114-gate runs and the native probes.
The former Og key case now reports a regular freshness failure; DATA/KM and wider
GCC qualification remain open. This confirms a starvation mechanism, not sole
causality for every prior native hang. The earlier debugger observations below
remain historical. Continue packet/KM/member diagnostics and capture any other
blocked close state before changing further lifecycle code.

## CI partition and quota follow-up

C++/initial partition commit: `2f31b966378a81c74b375dffcd325a401d6137bd`.
Final key-case partition commit: `b2b2a547606684ef7660117371614c03fc3e85eb`.
The [canonical result tables and scopes](README.md#ci-partition-and-quota-follow-up)
and [complete record](records/review-partitions.json) distinguish full 88/111-gate
runs from final four-key-gate checks on byte-identical binaries. Final 114/115
inventories are not represented as new full executions. Every source case has
one owner; optional retransmission CI includes cases moved from the aggregate.

All new process budgets remain 30 seconds. Quota checks retain the physical wire
envelope and force waits with an injected clock; a coarse-clock counterexample
checks valid zero-wait pacing. Downloaded Windows/macOS artifacts reproduce the
old assertion. Windows also lacks a second assigned IPv4 address, so complete
native qualification still requires environment evidence and after-fix CI.

GCC freshness/DATA/KM failures remain. Before the listener-close fix, one
isolated Og key case timed out;
a debugger snapshot locates close completion waiting in `ListenerHandshakeActor::stop`
with fault workers already stopped. A second probe exits normally with a freshness
failure. These were pre-fix observations; the deterministic close follow-up above now
reproduces and corrects timer/FIFO starvation. Other native causes remain open. GCC ARM64 CPU
oracle costs and the incomplete timing probe are separate diagnostic evidence.
The Default console proves 88 gates; its lost auxiliary detail is stated explicitly.

## Remaining harness and evidence work

- Use normal failure reports and membership/control diagnostics to investigate
  remaining GCC DATA/proof failures. Add packet arrival/repair/playout timing
  before attributing a missing canonical sequence to lateness, L-3 or a race.
- A changed-latency scratch experiment may help diagnose a deadline; it cannot
  count as passing the original 200 ms qualification envelope.
- Fixed-port dispatch/hybrid gates are serial and split by case ownership.
  Inspect hosted/residual suite budgets and rotation, add missing native
  automatic-controller/transition tests, and enforce the validator's binary binding. Preserve original failed logs and genuine repository-default scope.

The PR remains draft. No finding is marked resolved in GitHub, no review comment
is altered, and no merge/release/default enablement is authorized by this batch.
