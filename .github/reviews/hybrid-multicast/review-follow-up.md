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
| BL-3 | Partitioning implemented at 2f31b96; the aggregate unit gate is split once more, see below; hosted budgets inspected on 9 November | Keep all cases/deadlines; watch the remaining GCC CPU-oracle/rotation costs |
| BL-4 | Open: Windows `srt-live-transmit -version` hang | Native debugger/thread evidence and same-base comparison |
| BL-5 | Native wire bound retained; forced waits checked with injected clock and zero-wait counterexample | Native Windows after-fix CI pending; source fix does not qualify Windows hybrid transport |
| BL-6 | Partial: scope/labels corrected; matching-cache failure flags recovered | Enforce build/source identity, reject stale binary, test CLI record production; obtain missing platform evidence |
| L-1 | Fixed in source: an ACK or NAK ahead of the fanout is honored for the submitted part; see below | Hosted builds; no network-level throughput evidence above the existing soak rate |
| L-2 | Fixed in source: completion-busy detachment needs the attempt budget and the busy grace; see below | Hosted builds; no production-default contention run on a real network |
| L-3 | Fixed in source: such a send fails with `SRT_ECONNLOST` and consumes nothing; see below | Hosted default and enabled builds |
| L-4 | Measured and changed in source: 100 µs busy gate, blocked caller sleeps instead of polling; see below | Re-measure on Linux; the `full` wait still uses the global readiness signal |
| L-5 | Fixed in source: an expired message fails the send instead of detaching the member; see below | Hosted builds |
| L-6 | Open: prepared crypto generation advances on aborted admission | Cancelled-guard/prefix-recovery reproduction |
| L-7 | Fixed in source: each wire submission of an overlap original is recorded once in either completion order; see below | Hosted builds |
| L-8 | Measured: the per-poll send buffer probes are constant-time (2 to 13 ns a pair, no growth to 8192 packets); the coordinated fanout scan is bounded and measured with the sender benchmark; see below | Re-measure if the probes change |
| T-1 | Fixed in source: the candidate check is compiled only with the hybrid control option; see below | Hosted default and enabled builds; no behavior difference was observable before the fix |
| T-2 | Fixed in source: activation runs only for a coordinated listener; see below | No dedicated regression; existing bonding, dispatch and coordinated suites |
| T-3 | Open: equal endpoints/re-listen and documented contract | Reproduce native ownership/bind behavior; explicitly support or reject/document |
| T-4 | Open: native source syscalls under record lock | Check rollback/close races before moving work outside lock |
| T-5 | Open: nonzero repair threshold at coordinated listen | Consistent rejection or explicit ignored-option contract, with tests |
| T-6 | Open: multicast stats before provenance admission | Define routed/admitted/filtered counters and test each drop |
| T-7 | Open: stale route retains runtime until unregister | Rebind/unregister lifetime and counter regression |
| T-8 | Open: source readback and late-option error contracts | Tests and precise documentation |
| H-1 | Fixed in source: the join budget is armed by the first pump after publication; see below | Network-level evidence with a slow fallback confirmation |
| H-2 | Fixed in source: recovery and return compare local multicast receipt with the transit-settled report; native lag test added; see below | Hosted builds; WAN evidence still absent |
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

## Hosted CI follow-up at `d8db052`

Run 42507586967 left four red jobs. Linux Release was killed by a runner
shutdown during the build (exit 143, no compiler or test error); it passed on
the two preceding pushes. Address/undefined sanitizers timed out in
`robotweax_srt_rotation_tests` at 30.05 s; the same partition took 27.10 s on
`main` at `e45d0ed` and 7.6 to 30.0 s across this branch's runs, so it is a
pre-existing near-budget partition on a noisy runner, not a hybrid regression.
Both Windows multicast jobs failed in `tools/qualify_multicast.py`, which runs
every `multicast_dispatch` case and treated any skip as fatal: the new
`multicast_dispatch_independent_source_ip_distributes_and_rejoins` case skips on
hosted Windows runners ("no second assigned IPv4 address available"), while all
48 other cases, including every REQUIRED case, passed on both backends.

Changes: the qualification driver tolerates exactly that case with exactly that
reason, never a REQUIRED case, and records it under `environment_skips` in the
evidence; the real Windows and macOS transcripts were replayed through the new
validator (48 passed + 1 recorded skip; 77 passed, no skip). The rotation
matrix is split into CTR, GCM, transition and blackout partitions, each with
the unchanged 30-second budget; the loss-horizon matrix is divided by cipher in
source, and the Python registry check confirms every case keeps one owner.
No deadline, assertion or production code changed. BL-4 (`srt-live-transmit
-version` on Windows x64) has passed on every push since `18b4595`; the
scheduler starvation fix at `4d59666` is the likely cause, but this is not yet
attributed, so BL-4 stays open.

## Default-path isolation (T-1, T-2)

**T-1.** `ConnectionRuntime::process_packet` called `hybrid::native_candidate`
for every packet in every build and returned early on a match; only the
enqueue inside it was guarded. The whole check is now compiled only with
`ROBOTWEAX_SRT_EXPERIMENTAL_HYBRID_CONTROL`. A default build no longer
classifies subtypes `0x7f16`/`0x7f17` or decodes the inner packet of a
`0x7f05` envelope; those packets take the ordinary control path.

A behavioral regression could not be made to fail before the fix. On the
ordinary path the session answers an unimplemented user-defined subtype with
`Error::unsupported`, which also refreshes no peer activity and counts
nothing. Dropped early or ignored late, the externally visible result in a
default build was the same. The finding is therefore unconditional code and
pre-authentication parsing in every build, and a contradicted "no socket
activation" statement, not a changed outcome for existing peers. Two tests:

- `interop/tests/test_hybrid_default_isolation.py` scans `src/` outside
  `src/compat/hybrid/` and fails on any call into the `hybrid` namespace that
  a default build would compile. It reports the old call at
  `transport_runtime.cpp:4480` and passes on the fixed source. Type names are
  allowed. It is a source-structure check, not a behavioral one.
- `compat_runtime_default_build_gives_hybrid_control_subtypes_no_effect`
  (default builds only) requires that neither hybrid subtype nor the envelope
  form keeps an idle connection alive, with a keepalive as positive control.
  It passes before and after the fix and guards the visible claim.

Builds with the option are unchanged: a candidate is still consumed, and
dropped when no service is attached.

**T-2.** Listener admission called `GroupRegistry::activate_coordinated_member`
for every admitted group member, taking the accepted socket's `option_mutex`
and mapping a concurrent group close to `SRT_ECONNSETUP`. The call is now made
only when the listener selected `SRTO_ROBOTWEAX_MCAST_COORDINATED`, recorded
on the admission from the accepted socket's options, which the option setter
keeps equal to the listener's (fixed before listen, refused in the callback).
Bonding and uncoordinated dispatch admissions are back on the path they have
on `main`. The close race is not deterministic, so there is no dedicated
regression; the existing suites cover both directions.

## Canonical DATA path: L-3

A coordinated send that began with eligible members and detached every one of
them during admission returned `advanced_without_receivers`: the emitter,
encoder and cursor advanced, no datagram left, and `srt_send*` reported
success. A member detached for a full history, a stalled admission, an expired
message or a crypto error keeps its unicast session, can still listen to the
multicast group, and then expects a sequence the next original no longer
carries.

Decision by the maintainer on 9 November 2026: such a send fails and consumes
nothing. The coordinator returns the new `Status::receivers_lost` without the
unused advance; `srt_send*` publishes the detachments to the group data and
fails with `SRT_ECONNLOST`. The group stays connected. The following send
finds no eligible members and takes the documented empty advance, unchanged.
No packet format or other socket type is involved.

Scope, corrected during implementation. Failing every send that ends without
a member was too broad: when receivers close, the coordinator learns it during
the next send, and three existing public tests
(`multicast_dispatch_coordinated_*_public_*`) rightly expect that send to
succeed, as documented for a group whose receivers have left. The loss is
therefore reported only when at least one member detached in the call is still
connected, that is for every detachment reason except `closed` and `stale`.
Members that had already left keep the empty advance.

Regressions, each failing before the change:

- `compat_group_coordinator_send_losing_its_last_connected_members_fails_and_consumes_nothing`
  (plain, CTR, GCM): a history-full member next to a closed one; status,
  unchanged sequence, message number and key frontier, no further output, then
  the empty advance.
- the persistent rows of
  `compat_group_coordinator_busy_grace_retries_before_sealing_and_bounds_stalled_admission`
  now expect the loss and an unchanged sequence.
- `multicast_dispatch_coordinated_send_losing_its_only_receiver_reports_connection_lost`
  through the public API: a receiver that stops acknowledging fills its repair
  history; the send that detaches it returns `SRT_ECONNLOST`, the sequence is
  unchanged, the group data shows `SRT_GST_BROKEN`, and the next send succeeds.

Controls that pass before and after:
`compat_group_coordinator_all_unavailable_members_advance_without_emission`
(closed member, unchanged) and
`compat_group_coordinator_send_losing_one_of_two_members_is_accepted`.
L-5 (a per-message expiry detaching a member permanently) is separate and
still open; with this change such an expiry on the last member now also fails
the send instead of reporting success.

## Canonical DATA path: L-1 (acknowledgements)

`SendBuffer::acknowledge_submitted_before` rejected a whole cumulative ACK
when its prefix contained an occupied slot not yet marked submitted. For a
canonical group original that mark is set only when the coordinator applies
the emitter outcome to the member, after the UDP send. A genuine ACK can
arrive in between. It was then dropped with `invalid_control_payload`: nothing
was retired, no peer activity was recorded, and with every ACK ahead of the
newest fanout the member was eventually detached as `history_full`.

Such an ACK now retires the submitted prefix and stops at the first
unsubmitted group original. A later cumulative ACK retires the rest once it is
submitted. The session treats the ACK as covering exactly the retired prefix
for progress, the acknowledged cursor, timers and tracing.

Unchanged, and still asserted:

- an unsubmitted original is never retired by a wire ACK, including one that
  was never sent, expired, failed or belongs to a stale submission token;
- an ACK covering an ordinary packet that was never selected for sending is
  invalid as a whole
  (`send_buffer_ack_requires_submission_and_preserves_prefix_on_rejection`
  passes unmodified), so ordinary sessions behave as before.

What does change for such an ACK is the result code: `Error::none` instead of
`invalid_control_payload`, so its other fields are processed and the peer
counts as active. Nine assertions in `test_send_buffer.cpp`,
`test_group_admission.cpp` and `test_group_emission_outcomes.cpp` that
expressed "not authorized" through the old result code now call
`require_ack_retires_nothing`, which requires success with the first sequence
and size unchanged. One premature ACK was removed from
`compat_group_policy_explicit_prefix_recovery_classifies_unsent_and_submitted_work`;
it would now retire the submitted original whose repair that test goes on to
inspect.

Regressions, each failing before the change:

- `compat_group_coordinator_ack_ahead_of_fanout_retires_the_submitted_prefix`
- `compat_group_coordinator_acks_that_always_precede_fanout_keep_a_member_attached`
  (24 sends with every ACK ahead of the newest fanout; previously nothing was
  retired and the history filled)

No test shows the throughput of the ACK path on a real network at rates above
the existing soak.

## Canonical DATA path: L-1 (loss reports)

The same window exists for a NAK. `validate_retransmission_range` fails a
range that touches an unsubmitted group original, and the session validates a
whole report before changing any state, so one such range discarded every
range in the packet. A receiver can lose an original and report it before the
group tells the member that the original was submitted.

The session now asks the send buffer for the oldest unsubmitted group
original (`SendBuffer::first_unsubmitted_group_original`) and honors each
reported range only below it: a range that starts at or after it is dropped
from the report, a range that crosses it is shortened. The rest of the report
is validated and serviced as before. The receiver's next periodic report
names the sequences that were left out. An unsubmitted original is still
never repaired, and the buffer-level checks are unchanged; ordinary buffers
have no such original, so their reports are handled exactly as before.

The cut is at the oldest unsubmitted original rather than around each one.
Submissions are applied in order, so the unsubmitted originals are normally
the newest; if one is skipped by an expiry or failure, later submitted
originals wait for the next report too.

Regression, failing before the change:
`compat_group_coordinator_nak_ahead_of_fanout_repairs_the_submitted_sequences`
(a report naming a submitted and an unsubmitted original repairs the first;
the unsubmitted one alone repairs nothing; after its submission a new report
repairs it).

Not done: the left-out sequences are not remembered for an immediate repair
once they are submitted; that repair waits for the receiver's next report.

## Canonical DATA path: L-2

Completion fanout detached a member after `maximum_busy_attempts` (8 in
production) consecutive busy results. `drive()` runs on every accepted send as
well as on the 1 ms poll, and a member's runtime holds its mutex across a
whole poll including UDP I/O. Eight sends in a row while one member was inside
a single poll therefore detached a healthy member as `completion_busy`. The
count measured the send rate, not how long the member had been busy.

Detachment now also needs time. Each pending completion records its first
busy attempt on the admission clock; it is abandoned only when the attempt
budget is used up and, if a busy grace is configured, the member has stayed
busy for that grace. The grace is the existing `admission_busy_grace`, 50 ms
in production, so admission and completion use the same bound. With no grace
configured the behavior is the previous attempt count, which the existing
unit fixture relies on. A completion that waits keeps its frame occupied for
at most the grace.

Regression, failing before the change:
`compat_group_coordinator_busy_completion_uses_the_grace_not_only_the_attempt_count`
(16 fanout attempts at one instant and one more 9 ms later leave the member
attached; released, its completion is applied and its ACK retires the
original; kept busy until 10 ms, it is detached and nothing is authorized).
`compat_group_coordinator_busy_completion_budget_releases_frame_and_records_detachment`
passes unmodified.

Not shown: a production-default contention run on a real network.

## Aggregate unit gate split (BL-3, residual)

On 9 November the aggregate gate `robotweax_srt_tests` took 20.1 s and 24.7 s
on two Windows Debug runs and then hit its 30 s budget on a third after 918 of
about 1616 cases, on a runner that was running at roughly half the usual
speed. The same gate passed in 19.8 s on the rerun. The 249 canonical group
unit cases (`compat_group_*`) were a third of its time, so they now run as
`robotweax_srt_group_unit_tests` with its own 30 s budget, and the aggregate
excludes `compat_group_`. Both gates together own exactly the 1164 cases the
aggregate owned before, with no duplicate; the partition check in
`interop/tests/test_ci_build_scope.py` and the gate list of the optional
retransmission-limit job include the new gate. No budget was raised.

Hosted durations on the first run with the split (`ca37f70`): Windows Debug
13.84 s aggregate and 11.47 s group gate, Ubuntu Debug 7.06 s and 3.55 s,
macOS Debug 9.65 s and 4.90 s, each against the unchanged 30 s budget.

## Hot-path cost measured (L-8)

`benchmarks/send_buffer_probe_benchmark.cpp` times the two probes that every
session evaluates per poll, `peek_new_packet()` behind
`has_pending_send_work()` and `new_packet_flow_count()`, for windows of 64,
1024 and 8192 packets in all-sent, half-sent and empty states. On the
development Mac, Release, one pair of probes costs 2 to 13 ns and does not grow
with the window: the scan starts at the unsent cursor and stops at the first
candidate, and everything below the cursor has been sent. The O(n) worry in the
review holds only for a run of dropped tombstones at the cursor. The fanout
scan of `drive()` is bounded by frames times members and is exercised by the
coordinated sender benchmark described under L-4.

## Send contention measured and the busy gate shortened (L-4)

`benchmarks/coordinated_sender_benchmark.cpp` runs a coordinated listener
against N loopback receivers in one process, each draining in its own thread,
and sends 3000 blocking messages of 1316 bytes at `SRTO_MAXBW` 50 MB/s. It
reads three new counters from the coordinator snapshot: `busy_sends` (a member
admission was busy and the retry gate was armed), `gated_sends` (a send was
refused while the gate was still closed) and `full_sends` (queue or source
full). Development Mac, Release, no detachments, every message received:

| receivers | sends/s before | sends/s after | busy | gated before | gated after | full after |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 23256 | 22706 | 63 / 75 | 168814 | 0 | 56670 |
| 4 | 11482 | 14175 | 183 / 229 | 103467 | 1 | 22720 |
| 16 | 1999 | 2136 | 263 / 379 | 14838 | 0 | 12850 |
| 32 | 668 | 680 | 455 / 521 | 12780 | 0 | 5560 |

Reading before the change: busy admissions were rare, 63 to 455 per 3000
sends, but each closed the gate for a full `poll_interval` of 1 ms, which was
half to two thirds of the wall time with 1 to 4 receivers. The blocked caller
waited on the global readiness signal, which changes with every receiver's
traffic, so it spun through tens of thousands of refused calls per second.
The 1/poll-interval ceiling predicted in the review was not reached; with 16
and 32 receivers the per-send work for every member and the in-process
receivers dominate.

Change: a separate `admission_retry_interval` (default 100 µs, at most
`poll_interval`) arms the gate; `Snapshot::retry_wait` exposes the remaining
closed time; the blocking send path sleeps for it when refused with `busy`.
Non-blocking sends are unchanged. Effect: the gated spin is gone, throughput
+23 percent with 4 receivers, +7 percent with 16, within noise at 1 and 32.
Test: `compat_group_coordinator_busy_admission_gate_lasts_the_retry_interval`.

Still open, new observation: a caller refused with `full` also waits on the
global signal and spins while the queue drains. That is the library's
pre-existing blocking-send mechanism, visible here only at artificial rates
above the fanout; it is not specific to the coordinator and is not changed.

## Staged startup: join deadline armed at publication (H-1)

`MembershipRecovery::create` stamped the join deadline at construction, and
`StartupSession` constructs it on the first active poll but pumps it only once
the receiver is published, which waits for a fresh, confirmed unicast
fallback. With the network-timing configuration (membership timeout 1 s,
fallback timeout 600 ms) a slow build could publish a CONNECTED receiver whose
membership expired on its first pump and never retried, since `expired` is
absorbing and `publish_ready` does not consult membership.

The budget is now armed by the first pump: deadline = min(now + timeout,
grant expiry), with the first attempt due at once. Creation still validates
the grant and the receive history as before. After arming nothing changes:
provider latency cannot refresh the budget, and the deadline is fixed.

Tests, in `tests/test_hybrid_native.cpp`:

- `hybrid_membership_deadline_starts_at_the_first_pump_not_at_creation`
  (fails before the change): more than a whole timeout passes between
  creation and the first pump; the first pump still makes an attempt, the
  deadline equals that pump's time plus the timeout, and reaching it expires
  the budget without a further attempt.
- `hybrid_membership_deadline_and_provider_latency_cannot_refresh_the_budget`
  now arms the budget with a first attempt before testing that neither a
  clock at the deadline nor provider latency up to it refreshes the budget.

Not shown: a network-level run with a fallback confirmation slower than the
membership timeout.

## Automatic return against a lagging multicast path (H-2)

The detector's `recovered` predicate required the local multicast high-water
to reach the newest report's high-water, and the return admission in
`AutomaticReturn` repeated that comparison. A progress report describes DATA
emitted before it; while the source produces, the newest report leads the
DATA still in flight whenever the multicast path is slower than the unicast
control path. Measured on loopback with a fault link delaying only multicast
DATA, source producing every 12 ms, reports queried every 50 ms, grant 2 s:
no lag and 20 ms lag returned in about 230 to 260 ms; 40 ms lag never produced
a candidate within the grant.

Change. `PathDetector` keeps the receipt time and multicast high-water of the
last sixteen distinct reports and exposes `Assessment::settled_multicast_next`,
the high-water of the newest report received at least `maximum_transit_us`
ago. `recovered` and the return admission compare local multicast receipt with
that settled value. DATA the source emitted before such a report has had its
transit bound to arrive, so the comparison tolerates a lag up to the settled
report's age at evaluation, never less than the transit bound and, with
query-driven reports, about one query interval. The coverage proof of the
return window is unchanged and still checks every position. Beyond the
tolerated lag there is no candidate; a lag within it whose proof window is
not received in time ends one operation `unknown`, and a further operation
needs a higher policy revision, so the lifetime budget cannot be drained by
one persistent lag.

Evidence. `hybrid_return_lag_network_automatic_return_honors_the_configured_transit_bound`
(own gate `robotweax_srt_hybrid_return_lag_tests`, 30 s, serial): transit
5 ms with no lag returns in about 230 ms; transit 5 ms with 120 ms lag never
returns, spends at most one operation and stays unicast; transit 40 ms with
30 ms lag returns in about 265 ms. The last row fails before the change. The
existing detector, return and automatic unit suites pass unchanged (15, 16, 15
and 15 cases). Earlier native tests that already drove the controller over
loopback (`multicast_dispatch_native_automatic_return_lost_commits_plain_ctr_gcm`,
`..._startup_join_retry_fallback_and_return_plain_ctr_gcm`) still pass; they
have equal delays on both paths.

Not shown: a WAN or cross-host run, and the behaviour when the query interval
is much longer than the real lag, where the tolerated lag grows with it.

## Canonical DATA path: L-5

A member admission that found the message's own deadline already passed
(`RuntimeAdmission::Status::expired`) detached that member permanently as
`Detachment::expired`. The deadline is a property of the message, set by the
application per send, not a property of the member. The emitter's own check
of the same deadline fails the send with `Status::expired` and detaches
nobody; the member path now does the same: it returns `Status::expired`, the
enclosing scope releases the reservations made so far, every member stays
attached, and nothing is consumed. `srt_send*` maps that to `SRT_ETIMEOUT`,
as documented.

Regression, failing before the change:
`compat_group_coordinator_expired_message_at_member_admission_fails_the_send_without_detaching`
(one member's clock 5 s ahead, so a deadline the emitter accepts has passed
for that member: expired, no detachment, sequence and message number
unchanged, and the next send accepted by both members).

## Canonical DATA path: L-7

An overlap original reaches a member twice: over the group's multicast path
and as the member's own unicast copy. `SendBuffer` recorded the first of the
two completion reports as "submitted" and refused the second as a repeat, so
the outcome depended on which path completed first. Multicast first: the
later unicast completion returned false and skipped the session's
`note_data_packet_sent` (retransmission timer, tail-probe idle reference).
Unicast first: the later multicast report was counted as
`duplicate_completions` instead of `applied_completions`, and the sent note
was skipped as well. Counters stayed consistent; only the order decided what
was recorded.

Each delivery path now reports its own wire submission once. The multicast
report consumes the slot's pending multicast expectation
(`multicast_pending`, formerly `multicast_eligible`), the unicast completion
consumes its selection, and whichever arrives first authorizes the original
and starts its sender-drop clock; the other path's first report returns true
without touching either, and a repeated report of one path still returns
false. The coordinator therefore counts an overlap original as one applied
completion in both orders, and the member session records both wire
submissions.

Regression, failing before the change:
`send_buffer_overlap_reports_each_wire_submission_once_in_either_order`
(both orders: both first reports true, both repeats false, one packet in
flight, no pending unicast copy, cumulative ACK retires the original).
Updated expectations:
`send_buffer_overlap_counts_flight_once_and_preserves_pending_copy_through_multicast`
(the unicast completion after a multicast report is now a true first report
in the history and in its copy) and
`compat_group_policy_modes_persist_and_preserve_canonical_wire_identity`
(the multicast report after a completed unicast copy is applied, not a
duplicate).

## Source layout change

The 31 `hybrid_*` source and header files moved from `src/compat/` to
`src/compat/hybrid/` with unchanged file names. The commit contains only the
moves, the matching `#include "compat/hybrid/hybrid_*.hpp"` lines and the 15
source entries in `CMakeLists.txt`; no declaration, definition or test changed.
The directory is a reading aid, not a module or build boundary: these sources
are compiled in every build, and the canonical DATA and sending work remains in
`src/compat/group_*` and in existing files. Test files stay in `tests/`.

[README.md](README.md), the records, logs, patches and `SHA256SUMS` in this
directory are not edited. Where they name `src/compat/hybrid_*`, read
`src/compat/hybrid/hybrid_*` for any revision after this change.

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
