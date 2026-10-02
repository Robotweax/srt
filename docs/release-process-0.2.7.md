# Release process for Robotweax SRT 0.2.7

Status: preparation only. Version and release-note changes are reviewable on
`dev/release-0.2.7`. No new tag, GitHub release, signed SDK or tap update has
been published. Release date is the actual publication date, not the date of
this plan.

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
Their existing 0.2.6 pins must be replaced before claiming 0.2.7 qualification.
The private preparation directory includes an archive verifier and a concrete
pin inventory for this step.

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
