# Release process for Robotweax SRT 0.2.9

Status: **preparation in progress; no tag or publication accepted**.
Previous published release: [v0.2.8](https://github.com/Robotweax/srt/releases/tag/v0.2.8).
Development baseline: `6ec3cbdb5e3151e5ca3231965613e6f2a96ac86f`.
Historical [0.2.8 acceptance](release-process-0.2.8.md) remains unchanged.

## Release contract and scope

Release the existing Group recovery, metadata/path-label, Standby RTT,
listener/rejection and same-backend Windows SDK improvements. No new
accept-callback metadata, mbedTLS backend, configurable hold-down, reconnect
or RTT-selection policy belongs to this candidate.

Project version **0.2.9**, C ABI line **0.2**, compatible SRT API **1.5.7**.
Default crypto and optional feature activation remain unchanged. The new
versioned C observations are additive; preserve all prior ABI snapshots.
The [candidate release notes](release-notes-0.2.9.md) distinguish implemented
behavior from pending release acceptance and retained limitations.

## Source freeze and package pin

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

Complete this record with actual source/recipe/tag commits, successful run URLs,
downloaded-byte digests, dates and retained limitations. Preparation alone is
not release approval or publication.
