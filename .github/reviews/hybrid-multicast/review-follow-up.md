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
  value is truncated even on Win32. Budget/coordinator tests pass. **Native
  Win32/MSVC after-fix validation is pending hosted CI, not a local pass.**
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
| BL-2 | Bounded cast applied; local tests pass; native Win32 pending | MSVC `/WX` Win32 and x64 builds/execution |
| BL-3 | Open: instrumented aggregate/native gate durations | Partition growing suites or justify outer suite budgets without widening protocol deadlines |
| BL-4 | Open: Windows `srt-live-transmit -version` hang | Native debugger/thread evidence and same-base comparison |
| BL-5 | Open: Windows quota-wait assertion depends on host pacing | Guarantee saturation by construction or isolate rate invariant from injected wait coverage |
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

## Harness and evidence work next

- Reproduce failure-to-`terminate`; identify the actual exception/noexcept path,
  rather than removing noexcept declarations speculatively.
- Stop/join fault injection workers and clear their hooks before closing their
  underlying sockets. Verify forced-failure cleanup under GCC `-Og`/`-O1`.
- Label each soak wait. Capture receiver drop deadlines and group membership
  state so a late repair can be separated from L-3 or a race.
- A changed-latency scratch experiment may help diagnose a deadline; it cannot
  count as passing the original 200 ms qualification envelope.
- Fix fixed-port test serialization, aggregate coverage/time budgets, missing
  native automatic-controller/transition tests and the validator's binary
  binding. Preserve original failed logs and genuine repository-default scope.

The PR remains draft. No finding is marked resolved in GitHub, no review comment
is altered, and no merge/release/default enablement is authorized by this batch.
