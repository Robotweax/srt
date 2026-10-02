# Package qualification 0.2.7

Status: **qualified and published on 2026-10-02**.

Release: [v0.2.7](https://github.com/Robotweax/srt/releases/tag/v0.2.7).
Tag commit: `1f9cc7c045f84ba188a08677bfc57ce7424e5aff`.

Product source: `f254dd2c0fe084f7965238756b1d3b96665d592b`.
Archive SHA-256: `7ba68805f66bca1da4e0d509d33aedab239a4de57ba9affb4db1481d4205c908`.
Archive SHA-512: `a22179bf28dbb4f681ddc44e22afc2ea0ca0e3e09d4652bb189fdb52574f75bb84f9c6a95e03549b41318043589c0bdb28c793241dff1de82b965446da952e0b`.

All 547 regular files and executable bits in the downloaded GitHub archive were
compared with the immutable Git source. These hashes describe those downloaded
bytes, not a local git archive. The product source remains separate from this
recipe/evidence commit. Changes to product files require a new source pin and
repeat qualification. No prior 0.2.6 result qualifies this candidate.

## Accepted qualification

- Homebrew source install, test/audit, standalone consumers, coexistence and removal.
- Six vcpkg profiles: Linux x64, Windows x64 and macOS arm64, static/dynamic;
  Debug/Release consumers, relocation, coexistence and removal.
- Ubuntu 24.04 and Fedora 44 binary/source package builds, archive and license
  checks, installed consumers, coexistence and removal.
- Exact-head source CI and the explicit complete release ecosystem suite.
- Separately qualified final main/tag CI and signed Windows SDK download bytes.
- Separately qualified Homebrew tap formula and native bottle before advertising availability.

Package workflows build the pinned archive, not the moving workflow checkout.
Clean install/coexistence/removal checks do not establish upgrade/rollback,
PPA/COPR publication, registry availability or all-platform bottle coverage.
[Historical 0.2.6 qualification](qualification-0.2.6.md) remains unchanged.

## Evidence

Recipe candidate `6edfa60b3f921f0d4cdf0760be87ac0a471a45ff` was reviewed in
[PR #175](https://github.com/Robotweax/srt/pull/175). The Homebrew test environment
correction was reviewed in [PR #176](https://github.com/Robotweax/srt/pull/176).
Product files at the final tag remain identical to the immutable package source.

- [Candidate source CI](https://github.com/Robotweax/srt/actions/runs/36933470618),
  [BCrypt](https://github.com/Robotweax/srt/actions/runs/36933470714), and
  [explicit complete ecosystem suite](https://github.com/Robotweax/srt/actions/runs/36933471258) passed.
- [Final main CI](https://github.com/Robotweax/srt/actions/runs/36971215255) and
  [final tag CI](https://github.com/Robotweax/srt/actions/runs/36971349853) passed.
  Tag CI executed the configured integrations. Optional live-timing and native
  ARM64/performance diagnostics were not newly measured.
- [Final tag package managers](https://github.com/Robotweax/srt/actions/runs/36971349877)
  passed Homebrew and all six vcpkg static/dynamic platform profiles.
- [Final tag Linux packages](https://github.com/Robotweax/srt/actions/runs/36971349861)
  passed Ubuntu 24.04 and Fedora 44 package/consumer/coexistence/removal checks.
- [Final signed SDK workflow](https://github.com/Robotweax/srt/actions/runs/36971349763)
  built twelve variants, passed unsigned installation/coexistence checks, then
  signed and retested the exact installer pair on Windows. Release downloads
  match the signed CI artifact byte for byte and their post-signing manifest.
- [Native bottle qualification](https://github.com/Robotweax/homebrew-tap/actions/runs/36970375530)
  and [tap publication](https://github.com/Robotweax/homebrew-tap/actions/runs/36972971654)
  passed. Tap main is `a2f448abafd7ee072b29b084e1fffba77c316a4c`; the
  published formula matches the canonical recipe apart from bottle metadata.

The [published bottle](https://github.com/Robotweax/homebrew-tap/releases/tag/robotweax-srt-0.2.7)
is qualified for Apple Silicon macOS 15 (`arm64_sequoia`). Its downloaded bytes
match the native CI artifact and SHA-256
`8f54dc38670715a6addc2292de1fda26cacdd7e1d618a2d8a571b8011b233202`.

The original tap run 36969456768 ignored a failed consumer test and produced no
bottle; it is not accepted. Run 36970125518 failed style validation. Both original
results are retained. The final formula selects keg-only OpenSSL pkg-config
metadata; the tap now fails CI on failed test steps or missing bottle artifacts.
Superseded/cancelled runs are not accepted qualification evidence.

See the [release notes](../docs/release-notes-0.2.7.md) for download links,
post-signing checksums and the unresolved security/performance boundaries.
