# Package qualification: 0.2.6 candidate

Status: **corrected source package and bottle qualification passed; final tag/SDK acceptance and publication pending**.
Source: `7ecb60ea8b4faca01ed86237b0cc9dc906350f6e`.
Archive SHA-256: `b2920453b222879e1c7181a7c6b895c9440d104987d01cf9699eb4126b3b2476`.
Archive SHA-512: `73b91e3110a09b16f2903b6dbb4ab9068783044660312809d13e0406b62bb7df1eedbd9c8d671fdf64a58ca539e39a807020585ab4684fddbd55635c2140b35c`.

All 524 regular files in the downloaded archive were compared byte-for-byte
with Git. This source adds [PR #77](https://github.com/Robotweax/srt/pull/77):
retire the native readiness poll before closing its UDP socket, allowing an
immediate listener restart. The preceding full tag CI on `2873c66` failed the
GStreamer stop/start test; earlier green package checks and signed draft SDKs
therefore cannot be accepted for publication. Original qualification is retained
below as historical evidence, not relabeled as acceptance of this correction.

[Fix CI](https://github.com/Robotweax/srt/actions/runs/36249699089) passed;
PR #77 was merged as `4247cb82fc22e76ec55793da1d7cc5ad461b52d7`.
The focused Linux negative control fails with the old implementation; the fix
passes five readiness cases on Linux and macOS, seven macOS idle-readiness and
seven receive-slice cases, and 600 Linux GStreamer start/stop cycles. These are
lifecycle checks, not package qualification or throughput claims.

Recipes select the immutable archive, not the moving workflow checkout. If
implementation code changes again, update every recipe/hash and repeat affected
gates. Tag and Windows SDK acceptance, including replacement of affected draft
assets, remain separate release steps.

## Required checks

- Homebrew source install, formula test/audit, standalone/coexistence/removal.
- vcpkg static and dynamic profiles on Windows x64, Linux x64 and macOS arm64;
  Debug/Release consumers, coexistence, relocation and removal.
- Ubuntu 24.04 DEB and Fedora 44 RPM builds, source archive/license checks,
  installed consumers, coexistence and removal.
- Separately published Homebrew tap update and native bottle qualification
  before advertising 0.2.6 bottle availability.

Record CI run links, exact tested commits and failures before changing this
status. Previous [0.2.5 evidence](qualification-0.2.5.md) is historical and does
not establish acceptance of this candidate. Upgrade paths, additional bottle
platforms, PPA/COPR publication and distribution admission remain outside the
existing clean-install package guarantees.

## Corrected source qualification, 2026-09-26

Recipe commit: `f17d7eb2686e93223383a7a72a80ac35931157db`,
[PR #78](https://github.com/Robotweax/srt/pull/78). Later base synchronization and
this evidence update preserve the runtime, archive and executable recipes.

- [Package managers 36250172158](https://github.com/Robotweax/srt/actions/runs/36250172158)
  passed all seven jobs: Homebrew source install/test/audit and Windows x64,
  Linux x64 and macOS arm64 vcpkg static/dynamic profiles, including their
  configured standalone/coexistence/relocation/removal consumers.
- [Linux packages 36250172274](https://github.com/Robotweax/srt/actions/runs/36250172274)
  passed Ubuntu 24.04 DEB and Fedora 44 RPM qualification.
- [Recipe CI 36250172249](https://github.com/Robotweax/srt/actions/runs/36250172249)
  passed its selected checks. The unchanged runtime is qualified by the
  separate fix CI above, not by skipped recipe-only workflow jobs.
- [Homebrew bottle 36250101792](https://github.com/Robotweax/homebrew-tap/actions/runs/36250101792)
  passed on tap head `967429d4564af30c340fd447f43cc28d9df32ee6`.
  Tested merge ref `beeb22ca6b7031045ad4d2f2a6961b56b42aaab6` contains that head.
  The embedded formula is byte-identical to the canonical recipe; the actual
  arm64_sequoia bottle SHA-256 matches its metadata:
  `091292d6025bb07a658df8ef970f877f3d366135f383a504ae870a6ae3286650`.

The failed full tag CI on `2873c66` remains original failure evidence. Its signed
Windows SDKs remain draft artifacts containing the affected implementation;
they must be replaced by newly built and verified installers before publication.
A new protected-tag decision and final tag/SDK acceptance remain outstanding.

## Earlier source qualification, superseded by the runtime correction

Source: `daac593ffcb9bbddd25126a2bd97ddb607792fc2`.
Archive SHA-256: `852a9d4ca9d9a73c7a87be78252c1d641a0c4ffba5b6062b253249d8595cded4`.
Archive SHA-512: `ac240dea537f328c2795bb6bfc2a29d56ea80d8e44d7c44c6f3857840234b7824d7590baa75c735d1cb7266b4e861062278131a11b7a5fcdaac8268db4993ca6`.
The following successful checks qualify only that earlier source; they do not
accept the corrected candidate above.

Recipe commit: `fa009e3818a79852b27c87f947627613d5f795be`, merged by
[PR #74](https://github.com/Robotweax/srt/pull/74) as `d1c63c9299a2d75e2259584c8a478da93d7c28ca`.
This selects the source archive above, not the moving workflow checkout.

- [Package managers 36246468496](https://github.com/Robotweax/srt/actions/runs/36246468496)
  passed Homebrew source installation/test/audit and all six vcpkg profiles:
  Windows x64, Linux x64 and macOS arm64, each static and dynamic, including
  Release/Debug consumers, coexistence, relocation and removal.
- [Linux packages 36246468489](https://github.com/Robotweax/srt/actions/runs/36246468489)
  passed Ubuntu 24.04 DEB and Fedora 44 RPM qualification, including installed
  consumers, source archive/license checks, coexistence and removal.
- [Recipe CI 36246468475](https://github.com/Robotweax/srt/actions/runs/36246468475)
  passed its selected checks and required gate. Skipped core/integration jobs
  are not reported as newly passed; the unchanged implementation is qualified
  by the source CI linked above.
- [Homebrew bottle 36246479280](https://github.com/Robotweax/homebrew-tap/actions/runs/36246479280)
  passed build/install checks on tap head `174768f955e4ed85aa254d6c483e5a4de761854d`.
  The tested merge ref contains that head, and the embedded formula matches
  the canonical recipe byte-for-byte. The downloaded arm64_sequoia bottle
  SHA-256 matches its metadata:
  `aaf056f4b73bc7de91528a1ad32ad136cea80cb46c1e8445dff460b4410268fc`.

Earlier successful builds for source `3050534` and bottle head `cdb65e2` remain
historical evidence. They were superseded because PR #73 structurally changed
production source to make receive-slice tests deterministic. No previous tag,
release asset or published bottle was overwritten. Tag/Windows SDK acceptance
and the tap publication remain separate final steps. The clean-install checks
do not establish an upgrade or rollback guarantee.
