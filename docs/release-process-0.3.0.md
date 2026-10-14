# Release process for Robotweax SRT 0.3.0

Status: **startup fallback source correction; renewed qualification pending; signing/publication on hold**.
Previous release: [v0.2.9](https://github.com/Robotweax/srt/releases/tag/v0.2.9).
Preparation baseline: `9899fd7407cd82385de6f5238fe144647f299066`, including the
merged SSH interoperability cleanup correction.
No v0.3.0 tag or public release has been created by this preparation.

## Scope and version contract

Release the integrated Mbed TLS provider, peer admission metadata, optional
extension policies, group admission/statistics and lifecycle/resource fixes.
The [candidate notes](release-notes-0.3.0.md) define migration and retained limits.
Project version **0.3.0**, shared-library ABI line **0.3**, compatible SRT API
**1.5.7**. Existing C exports remain available, but consumers must rebuild when
moving from ABI line 0.2. Historical ABI and release records remain unchanged.

OpenSSL and compatible AES-CTR remain defaults. Preserve existing GCM/session
opt-ins. ACP1 remains an unreviewed, default-off prototype, requiring independent
cryptographic review and a separate supported-profile decision. Candidate tests
do not close #112/#190 or qualify it as a supported security profile.

## Freeze and immutable package inputs

Commit product/version changes first. Download the exact public GitHub commit
archive, compare every tracked regular file and executable bit with Git, and
record archive SHA-256/SHA-512 and source revision in the
[0.3.0 package record](https://github.com/Robotweax/srt/blob/main/packaging/qualification-0.3.0.md).
Recipe/evidence commits must preserve `src`, `include`, `cmake`, `compat` and
`CMakeLists.txt` relative to that frozen product. Any product change requires a
new archive pin and repeated qualification. Pending fields are not evidence.

Update Homebrew/vcpkg pins, Debian/Fedora recipes, Linux qualification scripts,
installed-consumer versions and latest signed SDK upgrade fixtures together.
Retain all previous qualification records and published tag identities.

The post-diagnostic File/Control capability correction changes the frozen
product. The prior CI results below apply to the earlier source only. The corrected archive is pinned to
`bc7e39cc942bbd129ff55b1c014edbe563be266d`; repeat affected qualification
before accepting corrected source.
Startup pacing and the failed resource gate remain unresolved.

## Previous candidate acceptance gates

| Gate | Current state | Acceptance required |
| --- | --- | --- |
| Source CI | Passed at `a09b2cb30` | Exact-head full CI and Required CI, platform Debug/Release, static/shared consumers, exports, sanitizers, fuzz and reference interoperability |
| Complete media integrations | Passed at `a09b2cb30` | Explicit release-integrations run including all OBS platform/desktop jobs, FFmpeg, GStreamer and VLC |
| Crypto providers | Routine profiles passed; optional native/performance diagnostics unselected | Mbed TLS profiles, default OpenSSL and explicit BCrypt qualification; report native/performance diagnostics separately |
| Experimental ACP1 | Linux/macOS test matrices passed; still unreviewed | Exact-source enabled mode partitions and cross-provider cases; no supported-security claim |
| Packages | All selected jobs passed at `a09b2cb30` | Homebrew source install/test/audit, six vcpkg profiles, Ubuntu 24.04 and Fedora 44 consumer/coexistence/removal checks |
| Windows SDK | Unsigned candidate and upgrade/rebuild qualification passed | Twelve variants, both unsigned installers, signed 0.2.9 upgrade, repair/rejection/coexistence and independent reconstruction |
| Resource/longer-running profiles | Failed; both encrypted soaks passed | Source-matched host/build/workload pins, payload integrity and stated CPU/RSS/throughput limits |
| Security review | Available finding and frozen-source delta reviewed; no new full scan | Available scan findings reviewed against the frozen product; preserve unresolved profile limitations |
| Protected signing/publication | Not started | Approved independent rebuild, publisher/timestamp checks and exact signed-byte retests |

Retain failed, cancelled and superseded runs. Accept only completed passing
results tied to immutable commits. Skips do not replace required profiles.

All seven candidate workflows passed at `a09b2cb30`; the
[package record](https://github.com/Robotweax/srt/blob/main/packaging/qualification-0.3.0.md)
links exact runs and distinguishes routine, optional and protected stages.
Those runs tested the previous frozen product `1e7d7824a`.

The previous Security Cloud scan at `3119aa208` reported a low-severity remote
interop cleanup finding; its source correction and regressions are included in
the preparation baseline. That review predates this version freeze and is not
a new passing scan of the final candidate.

## Resource gate remains open

The original resource cohort passed 38/40 cases. Published 0.2.9 and candidate
0.3.0 each missed one final Live payload under loss; those cases remain failed.
Both 300-second encrypted soaks passed exact-byte checks. A separately declared
ten-case File/Stream cohort also had deadline failures on both versions at the
fixed 60-second limit. Neither cohort is accepted as a whole. No failed case was
repeated, replaced or marked passed, and incomplete pairs do not qualify the
resource thresholds. The [package record](https://github.com/Robotweax/srt/blob/main/packaging/qualification-0.3.0.md)
and compact resource evidence preserve the results. The separate diagnostic
follow-up confirms a File-mode reorder-capability initialization defect on both
versions and identifies sensitive FileCC startup pacing. The capability
correction is included in the corrected archive. Startup pacing remains open
after source review; run a prospectively defined qualification against corrected
source before advancing release acceptance.

## Signing, tag and publication sequence

After candidate review and gates, merge preparation with an expected-head guard.
Reconfirm main CI, frozen product identity, version axes and recipe inputs.
Manual Windows signing runs only from main through the existing protected
environment, with independent reconstruction before signing. The environment's
approval remains required; preparation does not bypass it.

Only after acceptance create a new protected annotated `v0.3.0`. Never move or
recreate a published tag. Run final tag source/package/SDK gates, keep the release
as a draft, verify tag provenance and compare downloaded draft assets byte for
byte with qualified signed artifacts before publication.

Separate Homebrew tap/bottle qualification requires its own native build and
download/hash verification. No unqualified bottle, PPA, COPR or third-party app
distribution is implied. Starting this process is not final release acceptance.
