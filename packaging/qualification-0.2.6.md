# Package qualification: 0.2.6 candidate

Status: **source package and Apple Silicon bottle qualification passed; publication pending**.
Source: `daac593ffcb9bbddd25126a2bd97ddb607792fc2`.
Archive SHA-256: `852a9d4ca9d9a73c7a87be78252c1d641a0c4ffba5b6062b253249d8595cded4`.
Archive SHA-512: `ac240dea537f328c2795bb6bfc2a29d56ea80d8e44d7c44c6f3857840234b7824d7590baa75c735d1cb7266b4e861062278131a11b7a5fcdaac8268db4993ca6`.

The source commit contains the complete 0.2.6 runtime/storage changes and the
receive-slice CI corrections from PR #73. All 524 regular files in the downloaded
archive were compared byte-for-byte with Git.
[Main CI 36245587102](https://github.com/Robotweax/srt/actions/runs/36245587102)
passed on this exact source revision. Earlier package results for `3050534`
remain historical evidence; package and bottle acceptance must use this archive.
Later
recipe-only commits select that immutable archive; package workflows do not
build the moving branch. If implementation code changes during qualification,
update every recipe and hash to the new source commit and repeat affected gates.

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

## Final source qualification, 2026-09-26

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
