# Release process for Robotweax SRT 0.2.7

Status: **completed and published on 2026-10-02**.
Release: [v0.2.7](https://github.com/Robotweax/srt/releases/tag/v0.2.7).
The protected annotated tag selects
`1f9cc7c045f84ba188a08677bfc57ce7424e5aff`; it must not be moved or recreated.
The following procedure records the completed release workflow. Commands that
create refs or publish artifacts are historical instructions, not steps to rerun
for this already published version.

## Completed acceptance record

- Product source `f254dd2c0fe084f7965238756b1d3b96665d592b` was frozen and its
  downloaded archive verified against all 547 regular files and executable bits.
- Version/recipe [PR #175](https://github.com/Robotweax/srt/pull/175) and formula
  test [PR #176](https://github.com/Robotweax/srt/pull/176) were normally merged
  with expected-head guards after their selected checks passed.
- [Candidate full ecosystem qualification](https://github.com/Robotweax/srt/actions/runs/36933471258),
  [final main CI](https://github.com/Robotweax/srt/actions/runs/36971215255),
  [final tag CI](https://github.com/Robotweax/srt/actions/runs/36971349853),
  [tag packages](https://github.com/Robotweax/srt/actions/runs/36971349877), and
  [tag Linux packages](https://github.com/Robotweax/srt/actions/runs/36971349861) passed.
- Existing `windows-release-signing` approval preceded signing. The
  [final SDK workflow](https://github.com/Robotweax/srt/actions/runs/36971349763)
  verified trusted Robotweax GmbH signatures/timestamps and retested the exact
  signed installers on Windows. All three downloaded draft assets matched the
  qualified signed artifact bytes and post-signing manifest before publication.
- The separate [tap PR #6](https://github.com/Robotweax/homebrew-tap/pull/6) was
  applied and closed by the existing `brew pr-pull` workflow, which cherry-picks
  formula changes and adds a bottle commit rather than creating a PR merge commit.
  [Native qualification](https://github.com/Robotweax/homebrew-tap/actions/runs/36970375530)
  and [publication](https://github.com/Robotweax/homebrew-tap/actions/runs/36972971654)
  passed. The published arm64_sequoia bottle matches the qualified artifact,
  its formula hash and canonical recipe. Other bottle platforms are unqualified.

See [release notes](release-notes-0.2.7.md) and the
[package qualification record](https://github.com/Robotweax/srt/blob/main/packaging/qualification-0.2.7.md) for exact
hashes, asset links, failure-evidence exclusions and remaining limits.

## Source freeze and qualification

1. Review the version changes and [release notes](release-notes-0.2.7.md).
   Project version is 0.2.7, C ABI line stays 0.2, SRT API stays 1.5.7. Preserve
   the default crypto policy and the explicitly unresolved security limits.
2. Commit the product/version change with matching DCO sign-off. Publish its
   branch for review and record its full immutable source commit. This commit
   becomes the package source; later packaging/evidence commits must not
   change `src`, `include`, `cmake`, `compat` or `CMakeLists.txt` relative to it.
3. Require exact-head PR CI and the Required CI gate, including platform
   Debug/Release, static/shared consumers, export checks, sanitizers, fuzz
   smoke and selected reference interoperability. Run the experimental
   BCrypt workflow for the final candidate where not triggered automatically.
4. Run the explicit `release-integrations.yml` workflow on the candidate ref.
   Require the complete FFmpeg, GStreamer, VLC and configured OBS
   headless/desktop profiles. Record skipped optional profiles as unmeasured.
   A routine green PR/main gate may skip integrations; it is not a substitute.

```sh
gh workflow run release-integrations.yml --ref dev/release-0.2.7
gh workflow run windows-bcrypt.yml --ref dev/release-0.2.7
```

Run commands only once the corresponding ref exists on origin. Associate every
run with its exact head SHA, not merely a branch name. Native ARM64/serial
performance diagnostics are separate optional qualifications, not inferred
from a Windows cross-build.

## Immutable package source

1. Download the full source-commit archive from GitHub. Compare its complete
   regular-file inventory and bytes with Git and verify project/header version
   0.2.7. Compute SHA-256 and SHA-512 from those exact downloaded bytes; a local
   `git archive` is not evidence of the GitHub download hashes.
2. In a separate metadata commit, repin Homebrew URL/version/SHA-256, vcpkg
   URL/filename/SHA-512/version, Debian changelog, Fedora version/source/extract
   directory/changelog, both Linux qualification scripts and the Linux
   workflow URL/hash/label, and the exact package-consumer version in
   `packaging/tests/CMakeLists.txt`. Update package documentation and create a new
   qualification record; retain historical 0.2.6 records.
3. Require `package-managers.yml`: Homebrew install/test/audit plus six vcpkg
   static/dynamic profiles on Linux, Windows and macOS. Require
   `linux-packages.yml`: Ubuntu 24.04 and Fedora 44 builds, archive/license
   checks, installed consumers, coexistence and removal.
4. Record the exact package-source SHA, archive hashes, recipe commit and
   successful run links. Any product change requires a new source pin and
   repeat qualification; metadata-only changes still require affected checks.

Those workflows select immutable recipe archives, not the moving checkout.
For 0.2.7, the historical 0.2.6 pins were replaced before qualification.
The private release record retains the archive verifier, concrete pin inventory
and original run/artifact evidence.

## Review and merge

Merge the preparation PR normally with the expected-head guard after required
checks pass. Do not bypass branch protection. Reconfirm the final main commit
and its CI, version axes, package pins, DCO and product-tree equality against
the archived package source. Record the release candidate SHA explicitly.

## Protected tag and Windows SDK

After the candidate and package gates are accepted, create the protected tag
`v0.2.7` at that exact final main SHA. Check that the tag does not already exist
and use the repository's existing protection/release permissions. Do not move
an existing release tag to repair a failing candidate.

```sh
git tag -a v0.2.7 RELEASE_COMMIT -m "Robotweax SRT 0.2.7"
git push origin refs/tags/v0.2.7
```

Replace `RELEASE_COMMIT` with the verified full commit, not a moving ref.
The tag triggers full CI and `windows-sdk.yml`. The SDK workflow requires
`windows-release-signing` environment approval, builds OpenSSL/BCrypt
Win32/x64/ARM64 Debug/Release variants, tests installers and coexistence,
signs the pair, rechecks signatures after installing, and uploads a draft only.

Require both tag CI and the complete SDK workflow to succeed. Expected assets:

- `robotweax-srt-0.2.7-windows-sdk-openssl.exe`
- `robotweax-srt-0.2.7-windows-sdk-bcrypt.exe`
- `SHA256SUMS`

Download the three draft assets; compare them byte-for-byte with the qualified
signed workflow artifact, verify trusted signatures/publisher/timestamps on
Windows, and confirm the post-signing manifest. Keep the release as a draft
while any gate fails; retain original failure evidence. A superseding candidate
requires a separate tag/version decision rather than rewriting published tags.

## Publication and package distribution

Replace preparation statements with actual source/tag SHAs, qualification run
links, release date and signed checksums. Apply the verified release notes to
the draft and publish it only after acceptance of the final assets.

Update the separate Homebrew tap from the canonical qualified formula. Build
and qualify the native bottle, verify its downloaded hash against metadata,
and only then advertise bottle availability. Source release, SDK publication
and tap/bottle publication are distinct outcomes. Do not promise PPA/COPR,
vcpkg registry availability, upgrade/rollback coverage or third-party app
binaries from these existing qualification workflows.

Complete the release record with exact commits, artifact hashes, run URLs and
known limits. [Issue #112](https://github.com/Robotweax/srt/issues/112) remains
open until complete long-horizon replay protection is implemented and qualified;
a successful 0.2.7 release does not close it.
