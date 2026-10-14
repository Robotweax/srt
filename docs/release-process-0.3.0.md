# Release process for Robotweax SRT 0.3.0

Status: **started; candidate preparation and qualification pending**.
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

## Candidate acceptance gates

| Gate | Initial state | Acceptance required |
| --- | --- | --- |
| Source CI | Pending | Exact-head full CI and Required CI, platform Debug/Release, static/shared consumers, exports, sanitizers, fuzz and reference interoperability |
| Complete media integrations | Pending | Explicit release-integrations run including all OBS platform/desktop jobs, FFmpeg, GStreamer and VLC |
| Crypto providers | Pending | Mbed TLS profiles, default OpenSSL and explicit BCrypt qualification; report native/performance diagnostics separately |
| Experimental ACP1 | Pending | Exact-source enabled mode partitions and cross-provider cases; no supported-security claim |
| Packages | Pending | Homebrew source install/test/audit, six vcpkg profiles, Ubuntu 24.04 and Fedora 44 consumer/coexistence/removal checks |
| Windows SDK | Pending | Twelve variants, both unsigned installers, signed 0.2.9 upgrade, repair/rejection/coexistence and independent reconstruction |
| Resource/longer-running profiles | Pending | Source-matched host/build/workload pins, payload integrity and stated CPU/RSS/throughput limits |
| Security review | Pending | Available scan findings reviewed against the frozen product; preserve unresolved profile limitations |
| Protected signing/publication | Not started | Approved independent rebuild, publisher/timestamp checks and exact signed-byte retests |

Retain failed, cancelled and superseded runs. Accept only completed passing
results tied to immutable commits. Skips do not replace required profiles.

The previous Security Cloud scan at `3119aa208` reported a low-severity remote
interop cleanup finding; its source correction and regressions are included in
the preparation baseline. That review predates this version freeze and is not
a new passing scan of the final candidate.

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
