# Release process for Robotweax SRT 0.2.9

Status: **completed and published on 2026-10-13**.
Release: [v0.2.9](https://github.com/Robotweax/srt/releases/tag/v0.2.9), published
at 09:46:16 UTC. Protected annotated tag object
`2e1055a4733c62adfe4de7369d2fb0fd17c8f450` selects
`a87779d9c3d831583d32b2f61cae5b81edb05537`. This published tag must not be
moved or recreated. The annotation is not a GPG signature; native Windows
Authenticode acceptance is separate.
Previous published release: [v0.2.8](https://github.com/Robotweax/srt/releases/tag/v0.2.8).
Development baseline: `6ec3cbdb5e3151e5ca3231965613e6f2a96ac86f`.
Historical [0.2.8 acceptance](release-process-0.2.8.md) remains unchanged.

Preparation merge: `ce168060c241097b8af5b0cd9e8898027fb0ff09`, with the
complete candidate tree and frozen product identity verified.
[Current package record](https://github.com/Robotweax/srt/blob/main/packaging/qualification-0.2.9.md)
lists source/media/package/resource evidence and separate bottle status.

## Completed final acceptance

- [Final tag source and complete media CI](https://github.com/Robotweax/srt/actions/runs/38365080090):
  52 successful jobs including Required CI; one optional Live-timing diagnostic skipped.
- [Tag package managers](https://github.com/Robotweax/srt/actions/runs/38365080086):
  Homebrew source installation/test/audit and all six vcpkg profiles passed.
  [Linux packages](https://github.com/Robotweax/srt/actions/runs/38365080088):
  Ubuntu 24.04 and Fedora 44 passed build/consumer/coexistence/removal checks.
- [Final SDK](https://github.com/Robotweax/srt/actions/runs/38365080105):
  twelve variants, both installers, actual signed 0.2.8 upgrade/repair/rejection
  and coexistence, protected independent rebuild, signing by Robotweax GmbH
  with trusted timestamps and exact signed-executable tests passed. Rebuild
  job `115195540761`, signing `115222550174`, draft `115227012054`.
- Signed artifact `11801367380` has ZIP SHA-256
  `f4b0b54bda04ac1d71a29df93bf9c5b9ed966b540874d7bdc0f7b172ec6be05a`.
  Actual draft and public downloads of both installers and `SHA256SUMS` match
  this artifact byte for byte. Downloaded `tag-provenance.json` equals the
  independently rechecked annotated object and commit above. The published
  release has exactly those four assets and is the latest stable release.
- [Native ARM64 bottle qualification](https://github.com/Robotweax/homebrew-tap/actions/runs/38355338385)
  passed separately. Downloaded bottle SHA-256
  `eb36adf2e6440c3113385ef04e259840c4cc0ed0bba4695d338576d2245ed47c`
  matches its qualified artifact, canonical formula, version and 84 exports.
  [Tap #8](https://github.com/Robotweax/homebrew-tap/pull/8) remains with the
  tap maintainers for review/publication; no 0.2.9 bottle is advertised as published.

The original unpublished tag attempt failed the installed-documentation check:
a relative link targeted noninstalled packaging documentation. One-line
[PR #315](https://github.com/Robotweax/srt/pull/315) fixed that link; static
and shared installed checks passed, with frozen product/ABI/recipes preserved.
Explicit maintainer approval allowed an exception only for `refs/tags/v0.2.9`.
An exact lease on old object `b021060de696ad92b6eeb30b41e74098feb91db2`
guarded correction to the fixed commit. The complete transaction ran
07:48:43–07:48:53 UTC; original active update/delete protections for `v*`
were immediately restored without exclusions or bypass actors. Original
failed source run `38357547646` and cancelled SDK run `38357547699` remain
excluded from acceptance, with their evidence and original tag object retained.

The procedure below records the completed release. Tag creation/publication
steps are historical for this version. Selected resource and Group evidence
retain their source/workload limits; passing final CI does not close #112/#190
or qualify skipped optional diagnostics. Historical 0.2.8 records are unchanged.

## Release contract and scope

Release the existing Group recovery, metadata/path-label, Standby RTT,
listener/rejection and same-backend Windows SDK improvements. No new
accept-callback metadata, mbedTLS backend, configurable hold-down, reconnect
or RTT-selection policy belongs to this candidate.

Project version **0.2.9**, C ABI line **0.2**, compatible SRT API **1.5.7**.
Default crypto and optional feature activation remain unchanged. The new
versioned C observations are additive; preserve all prior ABI snapshots.
The [release notes](release-notes-0.2.9.md) describe published behavior and
retained limitations.

## Source freeze and package pin

Frozen product source: `80873657469344becb9c35f5306d0050ec5a566d`.
Downloaded GitHub archive SHA-256:
`16753663e52f77e9795b462927147b1d47ac48719d7a4d7fb10cd1db28f80c1b`.
All 608 regular files and executable bits match Git.
[Package acceptance record](https://github.com/Robotweax/srt/blob/f7b33bc2a9deaf69ecf4a0711608c7235ed781ae/packaging/qualification-0.2.9.md)
preserves the original frozen-source/recipe preparation record. Completed
final acceptance is recorded above.

Freeze the product/version commit before recipe qualification. Download that
exact public GitHub commit archive, compare every regular file and executable
bit with Git, and record SHA-256/SHA-512 of the downloaded bytes. Package
recipes must select those immutable bytes rather than moving main or an
uncreated release tag.

Record the source and recipe commits in the new public package qualification
record. Later recipe/evidence commits must preserve `src`, `include`, `cmake`,
`compat` and `CMakeLists.txt` relative to the frozen product. Any product change
invalidates that pin and requires new archive/package qualification. Pending
fields are not accepted evidence.

## Candidate acceptance gates

1. Exact-head source CI and Required CI: configured platform Debug/Release,
   static/shared installed consumers, exact C exports, sanitizers, fuzz smoke
   and pinned reference interoperability. Retain original failures and resolve
   applicable timing/lifecycle failures without weakening guards.
2. Explicit `release-integrations.yml` on the final candidate branch: full
   FFmpeg, GStreamer, VLC and OBS headless/desktop profiles. Skipped routine
   PR/main integrations do not substitute for this run.
3. Explicit `windows-bcrypt.yml` if not automatically triggered. Native ARM64
   and performance jobs are separately selected diagnostics, not inferred from
   cross-compilation or short functional profiles.
4. Candidate Live/File recovery, final File packet, exact payload integrity,
   reconnect/key rotation and supported Group regressions. Preserve the known
   Live final-loss boundary; do not apply it to File or a stricter predefined
   Group qualification contract.
5. Version-scoped Group extension checks: default-off compatibility with the
   pinned reference, bilateral enabled own-provider behavior, rejected
   unsupported/one-sided setups, lifecycle/capacity and truthful observation
   semantics. Historical fixed Lab profiles retain their exact source/workload
   scope; they do not establish a general final-candidate/WAN claim.
6. Source-matched CPU/RSS/throughput and longer-running acceptance for selected
   workloads; record actual host/build/peer pins and limits. Previous-version
   performance observations do not qualify this candidate.
7. Review the available Security Cloud scan/findings against the frozen
   product and preserve explicitly unresolved security-profile limitations.
   Running, unavailable or superseded scans are not passing results. Do not
   close #112/#190 because unrelated Group or package gates pass.

Associate every accepted run with an immutable SHA. Skipped, cancelled,
superseded or failed attempts are not accepted runs. Keep their evidence.

## Package and installer qualification

Update Homebrew URL/version/SHA-256, vcpkg version/URL/filename/SHA-512,
Debian changelog, Fedora version/source/extraction, Linux scripts/workflow
source pin and exact installed-consumer version together. Preserve previous
qualification records.

Require Homebrew source install/test/audit, all six configured vcpkg profiles,
Ubuntu 24.04 and Fedora 44 build/consumer/license/coexistence/removal checks.
Recipe CI builds the frozen archive, not the moving source checkout.

The Windows SDK must build all twelve variants, test both backend installers,
real earlier-release upgrade and same-version repair, rejection and coexistence,
and the independent reconstruction recipe. Its protected signing job still
requires the existing environment approval. Sign only independently rebuilt
bytes from the approved source; retest the exact signed pair and verify
publisher, trusted timestamp and post-signing checksums.

## Merge, tag and publication

After candidate review and required gates, merge preparation normally using
the expected-head guard. Reconfirm final main CI, product-tree identity,
version axes and recipe pins. Record the accepted final commit.

Only then create a new protected annotated `v0.2.9`. Never move or recreate a
published tag. Require final tag source/package/SDK acceptance and preserve the
release as a draft until all final gates pass. Compare downloaded draft assets
byte for byte with the qualified signed artifacts and record tag provenance
before publication.

The separate Homebrew tap/bottle update needs its own native qualification and
download/hash verification. Do not advertise an unqualified bottle. No new
PPA/COPR, registry or third-party application distribution is implied.

The completed record above and current package record supply the actual
source/recipe/tag commits, successful run URLs, downloaded-byte digests, dates
and retained limitations. Preparation alone is not release approval or publication.
