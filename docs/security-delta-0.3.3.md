# 0.3.3 security-sensitive delta disposition

This is a focused source-delta assessment for release preparation on 11 November
2026, not a new independent whole-repository security scan or protocol
certification. The [2026-11-04 review](security-review-2026-11-04.md) retains its
original scope. Runtime changes inspected here are the delta from `v0.3.2` to
`532fd6fe7648546c50a5f49b50f791718ec08fb9`; qualification-tool corrections follow
in `4d1185767`. Final-source execution evidence belongs in the release's separate
package qualification record.

## Reviewed boundaries

- Receive-key installation and generation selection in `src/crypto.cpp`: retained
  generation storage is bounded, routing uses accepted positions and lifetime
  anchors, and ambiguous CTR routing remains a documented policy rather than an
  integrity claim. GCM tries only eligible retained generations after an
  authentication failure, stops on provider errors, uses bounded scratch output,
  and publishes plaintext/advances accepted state only after authentication.
  Failed opens preserve overlapping encrypted inputs and erase scratch output.
- Hybrid admission and controls in `src/compat/hybrid`: credentials bind caller
  and listener groups, Stream ID, generation, expiry, exact conclusions and
  multicast source context. The control session checks destination, authenticated
  envelope, direction, bounded serial window and monotonic observation time
  before accepting state. Expired/revoked owners close; their traffic does not
  silently downgrade to the ordinary profile. The native pre-authentication queue
  remains bounded and is not a denial-of-service availability guarantee.
- Default-path isolation: hybrid classification and activation remain behind
  explicit build/profile selection. New independent/coordinated public options
  are opt-in. Existing shared-key multicast does not gain sender identity or
  cryptographic departure revocation through these changes.
- Listener/profile and route lifecycle changes: listen revalidates the prepared
  transmitter profile; superseded route retirement checks its binding; overdue
  timers no longer indefinitely defer listener-close work. Shared repair credit
  is charged at UDP acceptance and bounded blocked scans preserve progress.
- Sample credentials and fallback: receiver secrets are pairwise, selected by
  authenticated Stream ID, and omitted from diagnostic output. The sample's
  local discard authorizes its catch-up cursor; remote high-water reports do not.
  Discontinuities, limited controller budgets and reconnect responsibility remain
  explicit application contracts.

No additional reportable runtime security defect was established by this focused
assessment. This statement is limited to inspected boundaries and regression
evidence; it does not extend the supported security profile of experimental ACP1
or hybrid control.

## Qualification defect and correction

The hybrid soak driver previously read Git identity from its own checkout but
built an arbitrary supplied directory. A temporary unrelated CMake project with
a `robotweax_srt_tests` target reproduced `binary_bound_to_commit=true` for the
wrong source. The correction validates the canonical configured source root
before and after building, checks that HEAD/status did not change, and rejects
foreign or ambiguous source identities even when dirty diagnostic runs are
allowed. Such dirty runs remain explicitly unbound. Focused tests cover these
rejections and the successful clean-checkout path. This fixes a qualification
provenance claim; it is not remote transport exploitation evidence.

## Historical review reconciliation and retained limits

The historical PR #387 review packet remains an audit trail, including its old
pending/draft labels, failures and owner decisions. Current acceptance uses the
source and exact-source gates, not those old labels:

- BL-1/BL-2 default/Win32 build failures and BL-3 bounded partitioning have later
  source/CI corrections. Qualification must preserve exact case ownership and
  protocol deadlines. BL-6's source-binding claim is strengthened by the fix
  above. Do not rewrite historical soak records as newly bound evidence.
- The Windows one-millisecond period remains default ON for 0.3.3; the OFF
  profile must compile and execute its hosted timer/lifecycle checks. The earlier
  temporary 60-second application-version probe remains a recorded exception,
  not a demonstrated explanation of the original slow start.
- Source-failure recovery retains established control sessions for its documented
  `source_not_submitted` exception. Other published detachment breaks the session;
  the one-shot SHUTDOWN does not establish prompt remote notification if that
  datagram is lost while multicast continues. No stronger reconnect claim is made.
- Preserve the accepted first-OFFER binding, bounded unauthenticated input queue,
  terminal-control/manual-backstop policy and documented timing assumptions.
  Hosted macOS excludes native gates labelled both hybrid and network; local
  macOS evidence is recorded separately. Neither constitutes WAN qualification.
- Long-horizon compatible-mode KMREQ replay remains tracked by
  [issue #112](https://github.com/Robotweax/srt/issues/112). Real-network/IGMP,
  fan-out and sustained resource claims remain tracked by
  [issue #369](https://github.com/Robotweax/srt/issues/369). This release does not
  close either issue or broaden those support claims.

## Required execution evidence

Run the frozen product's default and hybrid builds, crypto generation/overlap
regressions, admission/control replay tests, provider matrices, sanitizer/fuzz
lanes and non-skipping multicast qualification. Repeat installed-package and
downstream validation with pinned companion identities. The previously green
PR #394 source and local 183-test run are correction evidence only; they do not
qualify versioned 0.3.3 artifacts. See the [release contract](release-process-0.3.3.md).
