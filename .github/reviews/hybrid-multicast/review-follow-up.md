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
| BL-4 | Instrumented: the version probe times each run against a 60 s bound, repeats a slow cold start once and records the durations; see below | Collect `results.json` from hosted Windows runs; a run that does not exit within the bound is a hang and fails |
| BL-5 | Native wire bound retained; forced waits checked with injected clock and zero-wait counterexample | Native Windows after-fix CI pending; source fix does not qualify Windows hybrid transport |
| BL-6 | Partial: scope/labels corrected; matching-cache failure flags recovered | Enforce build/source identity, reject stale binary, test CLI record production; obtain missing platform evidence |
| L-1 | Fixed in source: an ACK or NAK ahead of the fanout is honored for the submitted part; see below | Hosted builds; no network-level throughput evidence above the existing soak rate |
| L-2 | Fixed in source: completion-busy detachment needs the attempt budget and the busy grace; see below | Hosted builds; no production-default contention run on a real network |
| L-3 | Fixed in source: such a send fails with `SRT_ECONNLOST` and consumes nothing; see below | Hosted default and enabled builds |
| L-4 | Measured and changed in source: 100 µs busy gate, blocked caller sleeps instead of polling; see below | Re-measure on Linux; the `full` wait still uses the global readiness signal |
| L-5 | Fixed in source: an expired message fails the send instead of detaching the member; see below | Hosted builds |
| L-6 | Resolved as designed: the dropped admission's key announcement is real, so the refusal is required; pinned by a test and documented in the field comment; see below | None |
| L-7 | Fixed in source: each wire submission of an overlap original is recorded once in either completion order; see below | Hosted builds |
| L-8 | Measured: the per-poll send buffer probes are constant-time (2 to 13 ns a pair, no growth to 8192 packets); the coordinated fanout scan is bounded and measured with the sender benchmark; see below | Re-measure if the probes change |
| T-1 | Fixed in source: the candidate check is compiled only with the hybrid control option; see below | Hosted default and enabled builds; no behavior difference was observable before the fix |
| T-2 | Fixed in source: activation runs only for a coordinated listener; see below | No dedicated regression; existing bonding, dispatch and coordinated suites |
| T-3 | Fixed in source and documented: a source sharing the control endpoint (or its port next to a wildcard control bind) is `SRT_EINVPARAM` at listen; re-listen on a retained port documented; see below | Hosted builds |
| T-4 | Fixed in source: the transmitter's native socket setup runs from a snapshot outside the record mutex, with re-validation before the commit; see below | Hosted builds; no deterministic lock-scope regression |
| T-5 | Documented and pinned: a coordinated sender group reads 0, accepts 0 and rejects other values with `SRT_EINVOP`; see below | None |
| T-6 | Fixed in source: datagrams rejected before decoding count as filtered; counters documented precisely; see below | Hosted builds |
| T-7 | Fixed in source: a superseded multicast route is retired on the channel that published it; see below | Hosted builds |
| T-8 | Fixed in source and documented: frozen network options report `SRT_EBOUNDSOCK`/`SRT_ECONNSOCK`; source readback states documented; see below | Hosted builds |
| H-1 | Fixed in source: the join budget is armed by the first pump after publication; see below | Network-level evidence with a slow fallback confirmation |
| H-2 | Fixed in source: recovery and return compare local multicast receipt with the transit-settled report; native lag test added; see below | Hosted builds; WAN evidence still absent |
| H-3 | Fixed in source: provisioning requires lifetime >= query spacing + round and millisecond floors; see below | Hosted hybrid builds |
| H-4 | Fixed in source: hybrid progress arrivals and startup deadlines use the service clock; see below | Hosted hybrid builds; no injected-clock path exists through `attach_native_control` |
| H-5 | Documented: closed control owner is terminal per connection, fallback allowance spent on transient refusals, manual backstop required; see below | None |
| H-6 | Fixed in source: the join worker starts before the receiver and membership locks are taken; see below | Hosted hybrid builds; no deterministic lock-scope regression, see below |
| S-1 | Documented and pinned: the first decodable OFFER with matching IDs binds the single attempt; see below | Protocol retry change only after a separate review |
| S-2 | Documented: the ordinary proof covers key material only, the announcement steers the join first, the hybrid exchange fails closed later; see below | Transcript change only after a separate compatibility review |
| S-3 | Resolved by decision: the documented availability limit stands (unauthenticated inputs can occupy the 16 slots until the deadline); no reserved slot or enqueue-time MAC check; see below | None |
| S-4 | Fixed: `Admission` storage bound asserted, version digit asserted, credential-strength note added; see below | None |

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

**KM-1 is fixed on `main`.** Pull request 388 (merge `a6459de`,
`fix/km1-receive-key-generation`) changed the receive-side key selection in
`src/crypto.cpp` so that old-generation DATA above the receive frontier at KM
replacement is decrypted with the retained key, with regressions in
`tests/test_crypto.cpp`, documentation in `docs/encryption.md` and
`docs/limitations.md`, and a CHANGELOG entry. This branch carries that fix
through the merge of `main` at `f2c7757`. The diagnosis below is kept as the
record of the defect as it was found; its statements about an open defect and
an unchanged receive implementation describe the state before pull request
388.

**KM-1 as found:** old-generation DATA above the receive frontier at KM
replacement is routed to the replacement key. A standalone public-API diagnostic
fails CTR/GCM without clocks/network in five configurations; four controls per
build match and the two lagging-frontier round trips fail. Exact opaque key-ID
traces bind wrong selection to the reported failing DATA position in all four
native v2 captures. CTR can silently publish wrong plaintext; GCM rejects.
The affected receive implementation was unchanged from Robotweax v0.3.2 at the
time of the diagnosis. This is not a GCC-only or sanitizer-only issue; the
production fix is the one named above.

[Canonical diagnosis and limits](README.md#gcc-datakm-diagnosis-receive-generation-misrouting)
and [record](records/review-gcc-km.json) preserve every diagnostic failure.
All original 32 finding states remain; this additional blocker does not close
L-3 or explain distinct freshness failures. The CTR generation/lifetime binding
and the bounded authenticated GCM selection have their production regressions
in `tests/test_crypto.cpp` since pull request 388.
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

## Canonical DATA path: L-6

The finding: `try_reserve` raises `prepared_key_generation` and runs
`CryptoSession::prepare_data_sequence` before the guard is known to commit;
both survive a dropped guard and can make `try_recover_prefix` refuse with
`invalid` for a prefix that never advanced.

Reading of the code: `prepare_data_sequence` installs the generation of the
admitted sequence and, inside the pre-announcement window, installs the next
generation and builds the key-material announcement. The member's unicast
control path sends that announcement on its next poll whether or not the
frame commits, and the receiver's slot of the generation two steps back is
replaced when it arrives. `try_recover_prefix` refuses exactly when the
prepared generation is two or more ahead of the oldest unacknowledged
history, which is the case where the receiver no longer holds that history's
key. The recorded value is therefore a faithful record of a real side effect,
as the field's comment already said, and the refusal protects against
repairing a prefix the receiver cannot decrypt. A dropped guard brings forward
what the retry of the same send does on commit; nothing is wedged, since the
next admission is accepted. The alternative, deferring the announcement from
reservation to commit, would split `prepare_data_sequence` across the
reservation, the commit (which must not fail) and the member's poll; the owner
decided to keep the conservative refusal.

Pinned by
`compat_group_coordinator_dropped_admission_keeps_its_key_announcement_and_refuses_stale_prefix`:
two members hold four unacknowledged originals, positions 3 to 6 (generations
0 and 1, within their flow window); the reservation for position 7, the
pre-announcement window (prepares generation 1, announces generation 2), is
dropped on member A without staging; A's prefix
recovery is `invalid`, B's is `ready`; A's control path emits the
announcement; the next send is accepted and nobody is detached. The field
comment states the reasoning. The admission API admits only the member's next
sequence, so a far-ahead reservation cannot create the situation; it needs a
member that is two generations behind in acknowledgements.

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

## Intermittent hosted failures

Four hosted failures passed on rerun during this review's follow-up; none
reproduced locally. State of each:

- Windows `srt-live-transmit -version` timeout: see BL-4 above (probe
  instrumented).
- Sanitized `hybrid_network_multi_receiver_control_loss_...`: the test's
  ordering was corrected on 2026-11-09 (`isolate_first()` waits for the first
  auxiliary drops before provoking freshness loss); no recurrence since.
- macOS ACP1 group rendezvous (run 42646571776 on `main`, first attempt):
  `interop/run_mbedtls_peers.py --acp1 --rendezvous --group backup` ended
  with `rdv-backup-caller message 256: Connection setup failure` and both
  peers exiting 1, after the twelve bidirectional scenarios had passed; the
  peers use a 6-second connect timeout on two loopback rendezvous paths. One
  occurrence, the second attempt passed, no local reproduction attempted (the
  scenario needs the mbedtls peer build). Recorded as a timing failure on a
  loaded runner until it recurs.
- macOS `srt_compat_listener_handoff_survives_full_duplicate_inbox` (hybrid
  control Release job, run 42696517020, first attempt): "expected one
  listener callback; observed 2". Reading of the mechanism: the listener actor
  holds its inbox while an admission is pending, the admission extracts every
  queued envelope of the same peer and socket ID into the setup inbox after
  the callback, and a conclusion admitted later is replayed by the established
  runtime. A second callback therefore needs a duplicate that reached the
  listener inbox after the extraction and was admitted before the runtime
  could replay it, which happens either in the dispatch window between the
  route lookup and the inbox push, or when the first setup failed under load
  and the caller's retransmit was then admitted. Sixty runs under four busy
  cores on a local Mac did not reproduce it. The test now records, for every
  callback, the admitted handle and its time and reports their socket states
  on a repeat, so the next occurrence tells the two cases apart.

## Windows version probe (BL-4)

What the record shows: the external `srt-live-transmit` qualification on hosted
Windows failed on 2026-11-09 on pull request 390 (`main` plus a Linux-only
change, run 42640904512, first attempt) with
`subprocess.TimeoutExpired` for `srt-live-transmit-robotweax.exe -version`
after five seconds; the job itself took 2 min 41 s, and the second attempt
passed. The same failure had been seen on this branch. It is therefore not a
hybrid multicast defect. The upstream application calls `srt_startup()` before
parsing options and `srt_cleanup()` from a local destructor on every return,
so the probe covers process start (loading the freshly built executable and
the SDK and OpenSSL libraries), `WSAStartup`, construction of the dormant
runtime services (no worker thread exists before the first socket), the print,
`runtime_cleanup` and `WSACleanup`. A five-second cutoff cannot tell a slow
cold start on a busy runner from a hang in that path, and the killed process
left no evidence.

Change: the harness times each version run against a 60-second bound, repeats
once when the cold start exceeded five seconds, and records the durations and
a `slow_cold_start` flag in `version.log` and `results.json`, which the
workflow already uploads. A run that does not exit within the bound still
fails the job, now as a hang with the preceding durations as evidence. No
library change; the lifecycle probes (`process_exit`, `late_cleanup`,
`global_lifecycle`) already cover startup and cleanup without sockets on the
Windows matrix. The next hosted failure, or the next slow cold start, yields
the numbers the native diagnosis needs.

## Multicast dispatch contracts: T-5, T-6, T-7, T-8

T-5. The coordinated sender disables shared multicast repair aggregation.
The group setter already rejected a nonzero
`SRTO_ROBOTWEAX_MCAST_REXMIT_THRESHOLD` with `SRT_EINVOP` when a coordinator
exists, and the coordinator is created together with the group record at
listen, so no path ever observed a nonzero value. The documentation said only
"keep it 0"; it now states the contract (reads 0, 0 accepted, other values
`SRT_EINVOP`) and
`multicast_dispatch_coordinated_plaintext_public_stream_and_lifecycle` pins
the error code, the accepted zero and the readback. No behavior changed. The
`socket-options.md` sentence for this item was committed together with T-8.

T-6. A receiver's `mcast_pkt_received` counted every routed datagram, but
`mcast_pkt_filtered` only unmatched destinations and source rejections.
Drops before decoding for other reasons (stale or replaced route identity,
non-DATA kind, oversize payload, arrival after close) were received and not
filtered. `ConnectionRuntime::rejected_multicast_packets()` counts them and
the dispatch receiver folds it into `mcast_pkt_filtered`; the documentation
defines both counters and their difference. Regression: the shared-loss
scenarios send a decodable keepalive carrying the dispatch ID over the
multicast channel and expect it filtered; they fail before the change at the
filtered count.

T-7. A registration on another channel superseded the member's multicast
identity but left the old route (and its runtime reference) in the former
channel until unregister or destruction. Bindings now record their publishing
channel; a replacement retires the stale route there after the registering
channel's lock is released, so two channels are never locked together.
Regression:
`compat_multicast_binding_replacement_retires_the_route_on_the_former_channel`
fails before the change. Unshared channels (unit-test stack objects) carry no
publisher and behave as before.

T-8. In the independent-source profile a change of
`SRTO_ROBOTWEAX_MCAST_GROUP`, `_IFACE` or `_TTL` on a bound socket failed
with `SRT_EINVPARAM`; it now reports `SRT_EBOUNDSOCK`, and `SRT_ECONNSOCK`
once listening, like every other pre-bind option. The source readback is
documented: `SRT_EINVPARAM` while unset, the configured value until listen,
the bound endpoint after listen, `SRT_EINVSOCK` after close. Regression:
`multicast_dispatch_independent_network_options_freeze_with_socket_state_errors`
fails before the change at the bound-socket expectation.

## Independent source socket: T-3, T-4

T-3. The documentation said the control endpoint "can differ from the
multicast source in address, port or both", which implied equality was
allowed, while an equal endpoint failed `srt_listen` with `SRT_ESOCKFAIL`
from the kernel's bind. Supporting equality is not an option: with
`SO_REUSEADDR` on both sockets the kernel would hand unicast control
datagrams to the send-only source socket, and the same holds for the control
socket's port next to a wildcard control bind. `prepare_multicast_transmitter`
now rejects both shapes with `SRT_EINVPARAM` before any syscall; the same
port on a different specific address stays allowed, and the independent
transmitter still does not inherit `SRTO_REUSEADDR`. The documentation
states the rule and that the source port stays bound while a dispatch group
retains the channel, so a re-listen on the same fixed port fails with
`SRT_ESOCKFAIL` until the group releases it. Regression:
`multicast_dispatch_independent_source_cannot_share_the_control_endpoint`
(same endpoint, and wildcard control on the source port) fails before the
change at the error code.

T-4. `listen_socket` ran the transmitter's `socket`/`setsockopt`/`bind`/
`getsockname` under the record mutex. It now validates and snapshots the
options and control endpoint under the mutex, runs the native setup from the
snapshot without it, then re-validates state and profile (including the post
option `SRTO_MAXBW` for the coordinated profile) before committing; a state
change in between (close, another listen, connect) fails with the state's own
error and drops the new socket. The multicast and source options are frozen
once bound, so the snapshot cannot go stale for them. There is no
deterministic regression for the lock scope; the dispatch and option suites
cover the restructured path.

## Experimental hybrid control: H-3, H-4, H-5, H-6

H-3. `ReceiverBridge::valid_configuration` bounded every interval on its own
but not their relation, so a host could choose a confirmation lifetime shorter
than one query spacing plus one round and get periodic `fresh=false` holes in
which proof, overlap, return, membership `uc_ready` and automatic candidates
all refuse; it also accepted microsecond query spacing that floods the
source's bounded record gate. Startup provisioning (`valid_options`) now
requires `confirmation_lifetime_us >= query_interval_us + round_timeout_us`
and query and retry spacing of at least one millisecond; the component's own
validation is unchanged so unit tests can still exercise expiry with short
lifetimes. Regression: three new invalid cases in
`hybrid_setup_provisioning_requires_explicit_policy_callbacks_and_bounded_configuration`,
failing before the change at the first of them. The dispatch fixture's
lifetime moved from 80 ms to 100 ms to satisfy the relation (query 50 ms,
round 40 ms).

H-4. DATA arrival for hybrid progress was stamped with `Clock::now()` while
every controller compares against `NativeService::now()`; the two agree in
production and differ under an injected test clock. The runtime now stamps
arrivals with the attached service's clock and applies the channel's ingress
stamp only when that clock is the monotonic one. `StartupSession` compares its
deadlines with `native_->now()`; its `start()` keeps `monotonic_now()` for the
caller-supplied deadline, which is in that domain by construction because
`attach_native_control` injects no clock. For the same reason no end-to-end
injected-clock regression exists through the public setup path; the controller
suites with injected clocks and the dispatch suites cover the change.

H-5. Documentation only, as the review asked: a closed control owner (after a
conflicting or defective authenticated reply) is terminal for the connection
and needs a host backstop; the automatic fallback allowance is spent on
transient refusals and the same revision is never retried.

H-6. `MembershipRecovery::pump()` held the receiver's mutex across
`register_ingress_route` and `channel->start()` (thread creation), and
`ReceiverBridge::work()` only tries that lock, so a slow join skipped the
receiver's query cycles. The worker now starts before any controller lock is
taken (a running channel without a route drops datagrams unrouted); the
route is registered under the locks as before, and a worker start failure
still closes the recovery. There is no deterministic regression for the lock
scope: the slow call is thread creation inside the library with no injection
point; the hybrid suites (117 gates) pass with the change.

## Experimental hybrid control: S-1, S-2, S-3, S-4

S-1. The OFFER carries only the Caller's nonce; the Listener cannot verify a
proof before it has issued its own nonce. It therefore binds the first
decodable OFFER that names its socket IDs and credential ID, resolves the
credential once and answers it; a later genuine OFFER is ignored and the
attempt fails explicitly at the proof. The connection makes one attempt
(`hybrid_attempted_`). Documented; pinned by
`hybrid_exchange_first_matching_offer_binds_the_single_attempt`. A protocol
retry would change the exchange and is left to a separate review.

S-2. Documentation only: the ordinary session-authentication proof covers key
material, not the `0x7f14`/`0x7f15` records; the announced source steers the
Caller's join and DATA filter first; the hybrid exchange binds the source into
its key derivation and fails closed after the join. Including the CONCLUSION
payload in the authenticated material is a transcript change for a separate
compatibility review.

S-3. Documentation states the availability limit precisely: inputs are
authenticated when processed, so unauthenticated datagrams that pass the
subtype filter can occupy the 16 input slots until the attempt's deadline;
the attempt then fails explicitly. A reserved exchange slot or an enqueue-time
MAC check would remove the limit; the owner decided to keep the documented
limit for the experimental component.

S-4. `static_assert(sizeof(Admission) < 8 * 1024)` pins the documented
bound next to the existing exchange, service and bridge assertions; the
session version constants are asserted below 10 because the domain salt
spells the version as one ASCII digit; the documentation notes that a captured
SELECTION record is an offline guessing oracle against a low-entropy pairwise
credential.

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

## Hybrid control on hosted Windows

The Release job of `experimental-hybrid-control.yml` runs on `windows-latest`
since `c93c937` (MSVC, the image's OpenSSL). The configuration compiles and
links there. The first hosted run (42774111162
on `51b4946`) ran every gate; both attempts ended with 84 of 93 tests passed.
All failures are gates labelled both `hybrid` and `network`; eight of those
17 gates passed in each attempt, and the failing nine were not the same nine
(`robotweax_srt_hybrid_key_multi_receiver_tests` failed only in the first
attempt, `robotweax_srt_multicast_source_error_tests` only in the second).

Measured in the failing load gate: the sender reached 85 and 89 packets per
second against a target of 100 in the first attempt, and 79 against 100 and 96
against 250 with 23 and 91 pacer resets in the second, where the gate then
failed its own offered-rate check (`load offered-rate row=1 actual_pps=96
target_pps=250`). The other failures are deadline or freshness expectations of
the same cases (for example `next < 96`, `expected state.result ==
confirmed`, `request_overlap() == accepted`, one late DATA drop per soak
profile).

Decision, applied in the workflow: Windows is treated like macOS. It compiles
the whole configuration and runs every gate except those labelled both
`hybrid` and `network`; Linux remains the hosted evidence for the excluded
gates. No test, deadline or assertion changed.

Not established: why the Windows sender falls behind. Neither the library nor
the tests request a finer Windows timer resolution, so sleeps and timed waits
of a few milliseconds may be rounded up to the scheduler tick; that would
explain the rate and the short-deadline failures, but it is a hypothesis
without a measurement. If it holds, it also bounds how tightly the
experimental hybrid control timers can be configured on Windows outside the
tests. The hybrid transport is therefore not qualified on Windows by hosted
evidence; BL-5's remaining column already says so.

Same push, unrelated job: `Mbed TLS 3.6 (macos-latest)` in CI run 42774111257
failed its first attempt in `interop/run_mbedtls_peers.py` (`crypto_session_peer
caller ... 24 ctr` timed out after 40 seconds, after the preceding pairs had
passed) and passed on the second attempt. Recorded with the intermittent
hosted failures above; one occurrence.

Head `6ce334c` (workflow and notes only): hybrid control passed on Linux,
macOS and Windows. CI run 42779265494 failed its first attempt in two jobs and
passed on the second attempt:

- `windows-latest Debug`:
  `multicast_dispatch_coordinated_aggregate_budget_bounds_correlated_prefix_repairs`,
  `tests/test_multicast_dispatch.cpp:4894`, a forced policy transition was not
  `applied`.
- `AES-GCM platform/ABI (macos-latest)`:
  `srt_compat_session_auth_admission_rejects_mismatch_and_failed_children_do_not_leak`,
  `tests/test_session_authentication_admission.cpp:260`, the recovery connect
  with a one-second timeout did not return 0.

Both passed 15 and 10 consecutive local runs on a Mac at the same source. One
occurrence each, no cause established; they join the intermittent hosted
failures above.

## Takeover review, 2026-11-10

A second review of `4af9c86..6c7b934` found defects in two fixes and gaps in
tests. Changes since `6c7b934`:

- H-2. The detector dropped the oldest of its sixteen report marks on every
  new report. With reports arriving faster than one sixteenth of
  `maximum_transit_us` no mark ever settled, `settled_multicast_next` stayed
  zero and return was impossible without any lag. Marks are now at least one
  fifteenth of the transit bound apart; a report inside that spacing raises
  the newest mark and keeps its earlier receipt time, so the comparison asks
  for the DATA up to one fifteenth of the bound sooner, never later. Cadences
  that fitted the history are unchanged. Test:
  `hybrid_detector_return_survives_a_transit_bound_longer_than_the_report_history`.
- H-2. The report that carried a new policy, or ended a sampling gap, lost its
  mark to the evidence reset. The mark is recorded after the resets. Test:
  `hybrid_detector_report_that_carries_a_new_policy_keeps_its_mark`.
- T-4. The listen commit re-checked only the socket state and `SRTO_MAXBW`.
  Message API, TSBPD, congestion control, packet filter and session
  authentication stay settable on a bound socket until it listens, so a
  racing setter produced a listening sender outside its profile. The profile
  rules now run again on the live options at the commit. The statement above
  that the snapshot cannot go stale holds for the source, independent and
  coordinated options only. Test:
  `multicast_dispatch_independent_listen_commit_rejects_a_profile_change_during_setup`,
  a race over 200 listens that failed in three of three runs before the fix.
- L-3. The lost-member fact was local to one send. A send that detached a
  connected member and returned busy forgot it, and the retry advanced the
  sequence unused. The coordinator keeps the fact until a send settles the
  sequence. Test:
  `compat_group_coordinator_busy_retry_after_losing_a_connected_member_reports_the_loss`.
  Open for the owner: the fact also survives an `expired` or `full` return;
  a group that empties without a retry reports one `SRT_ECONNLOST` on its
  next send; members detached on the completion path still lead to the
  silent empty advance.
- L-1. Five checks that only required success from the ACK after a
  submission proved nothing once an early ACK succeeds too. They now require
  that the history was retired.
- T-6. `mcast_pkt_filtered` also counts foreign datagrams on the member's
  unicast socket and can exceed `mcast_pkt_received`. Documentation corrected,
  no code change.
- New tests without a source change: a loss range that crosses into
  unsubmitted group originals is shortened; a coordinator detachment raises
  `SRT_EPOLL_UPDATE` on the group exactly once.
- L-5 was checked and is not a defect: `srt_group_data` and group epoll
  refresh coordinator detachments on every call.

Not changed, still open from that review: T-7 (the former route is retired
by socket ID without comparing the binding), H-3 (the documented lifetime
bound may be too strong for long round trips, not verified), H-4 (one
control-record arrival stamp bypasses the service clock, test-only effect),
H-1 (`snapshot()` can show `expired` before the first pump), T-8 (only the
`OPENED` state was aligned), and the owner questions on the 60-second version
probe (`d3915e8`) and the six logs rewritten by `4dcaf04`.

### Windows Debug: coordinated CTR rotation test

`multicast_dispatch_coordinated_ctr_public_rotations_and_empty_rejoin` failed
on `windows-latest Debug` on two consecutive heads and passed 100 local macOS
runs, 60 of them under full CPU load.

- `6c7b934`, run 42784963117, job 129723886822: a message of the expected
  length with the wrong content.
- `b5ddd1c`, run 42810868063, job 129811738331, with the first diagnostic:
  reading message 64 of 80 timed out (`SRT_ETIMEOUT`) after 64 deliveries.
  64 is the second key switch at refresh rate 32.

Cause not established. Reading of the code, not measured: an undecryptable
DATA packet is counted on the member and returns before the session sees it,
so it creates no loss and no NAK; the key switch is position-based and not
gated on the receiver's key response; a member that stays busy for 50 ms is
detached by the coordinator and gets no further key announcements. Either a
late key announcement or a detachment before position 56 fits the second
failure. The test now prints, on a mismatch, each receiving member's unique
and undecryptable counts and the sender coordinator's active and detached
members; the next hosted failure decides between the two. The group row of
`srt_bstats` carries deliveries only, so its loss and undecrypted fields are
zero on a group handle.

Local reproduction on macOS, source change not committed: with
`admission_busy_grace` forced from 50 ms to zero, so that the first busy
admission detaches the member, the test failed in four of four runs with the
hosted signatures. Three runs timed out on message 64 after 64 deliveries with
16 undecryptable packets on the member and one or two detached members,
reason `admission_busy`; one run delivered a later message in place of the
expected one. With the grace at 150 µs and at 1 µs the test still passed on
the Mac, where a busy admission clears within one 100 µs retry. So a member
that stays busy for the full grace on hosted Windows explains both failures;
that it does is not yet observed there. The report prints the detachment
reason for the next hosted failure.

This is the documented contract working against a healthy receiver: the
detached member stays `SRTS_CONNECTED`, gets no further key announcements and
no signal, and cannot decrypt after the next key switch. Whether a busy
sender-side admission should cut off a receiver this way, and whether the
receiver should be told, is an owner decision.

`platform_timing_report_measures_short_sleeps_and_timed_waits` and a step in
the hybrid control workflow now print sleep and timed-wait granularity on
every runner. Local Mac: a 1 ms sleep takes about 1.3 ms, a 100 µs sleep
about 130 µs.

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
