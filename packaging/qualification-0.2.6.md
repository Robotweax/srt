# Package qualification: 0.2.6 candidate

Status: **qualification pending; no 0.2.6 package publication claimed**.
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
