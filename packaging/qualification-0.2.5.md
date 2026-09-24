# Package qualification: 0.2.5

Status: Homebrew tap published for Robotweax SRT 0.2.5 on 2026-09-24;
vcpkg remains an overlay and no registry, PPA or COPR is published.
Source: `492a7d61390cbec86e44e177ec034f0f5d9a5cc3` (release 0.2.5).

## Local evidence, 2026-09-24

Native Apple Silicon, AppleClang 21, macOS; no cross-compilation:

- Homebrew 7.0.6: source installation alongside the Homebrew Haivision `srt`
  bottle 1.5.7 succeeded. `brew test` and `brew audit --strict` passed.
- The installed C native API, C++ API and separate Haivision consumers all
  compiled and executed (3/3 CTest cases). Removing Robotweax left the
  Haivision executable working. The temporary validation tap was removed.
- vcpkg revision `dc1232a6e05dcc49703091e83743e3b4df9b9b7c`:
  `robotweax-srt` and `libsrt` 1.5.6 installed together for both `arm64-osx`
  and `arm64-osx-dynamic`. vcpkg post-build validation and all three Release
  installed-consumer tests passed per triplet (6/6). Removing Robotweax left
  both Haivision consumers working (2/2). Debug packages built successfully;
  Debug consumer execution was added in the follow-up qualification below.
- The recipe's immutable source archive also passed the existing shared
  `abi_baseline` and `package_consumer` source-build tests (2/2).

Homebrew updated CMake to 4.4.3, OpenSSL to 3.6.4 and pkgconf to 3.0.7 while
preparing the tests. Its automatic removal initially removed existing unused
Python/ICU packages when the test formula was uninstalled. Python 3.13.15 and python-packaging 26.3 were restored, and ICU 75.1 was
restored as bottle revision 75.1_1; CI now sets `HOMEBREW_NO_AUTOREMOVE=1` as well as
`HOMEBREW_NO_INSTALL_CLEANUP=1`. These are host-management details, not package
runtime requirements.

## Release gates still open

- Qualify Intel macOS, Linux Homebrew and newer macOS bottles before advertising
  them. Only the Apple Silicon macOS 15 bottle is published today.
- Qualify upgrades and rollback for future package versions. There is no
  previous Robotweax package to upgrade from here.
- Confirm supported OS versions and architectures before advertising support.
- Ubuntu/PPA and Fedora/COPR packaging remain the following workstream.

Installing Robotweax does not switch existing applications from Haivision.
The coexistence test uses separate processes, never both providers in one.

## First hosted run

[Run 35987215907](https://github.com/Robotweax/srt/actions/runs/35987215907)
passed Homebrew, both Linux triplets, both macOS triplets and dynamic Windows.
Static Windows built and installed both packages but failed to link the C++
consumer: the package used `/MT`, while the consumer defaulted to `/MD`
(`LNK2038 RuntimeLibrary` mismatch). The workflow now explicitly configures
the static triplet's consumer runtime, including the Debug generator expression.
[Run 35988239811](https://github.com/Robotweax/srt/actions/runs/35988239811)
passed all seven package jobs at `1300db0ab1638a946a7cdea73e275556a086c8af`,
including static Windows. The regular CI and required gate also passed in
[run 35988239725](https://github.com/Robotweax/srt/actions/runs/35988239725).
PR #64 merged as `6c79ef364534b636484cf46df88a0fa5e86c6e68`.

## Expanded qualification after PR #64

The follow-up workflow runs fresh, isolated vcpkg installations in this order:
standalone Robotweax, coexistence with Haivision, relocation of the complete
installed prefix (the original path is absent), and removal of Robotweax.
C and C++ consumers execute in both Release and Debug; Haivision Debug uses its
Debug library rather than mixing configurations. Logs are retained as CI artifacts.

Native macOS arm64 static and dynamic qualification each passed all 18 test
executions (4 standalone, 6 coexistence, 6 relocated, 2 after removal; 36 total).

[Hosted package run 35990225375](https://github.com/Robotweax/srt/actions/runs/35990225375)
passed all seven jobs at `995573520c86cc7c2b74678beec3fa0327dcffe6`:
Homebrew on macOS, static and dynamic vcpkg on Linux x64, Windows x64, and
macOS arm64. The [regular CI and required gate](https://github.com/Robotweax/srt/actions/runs/35990225360)
also passed. The static Windows log confirms Debug, relocated-prefix, and
post-removal consumers executed. PR #65 merged as
`3a339236f13664c7565a6dbea28f8c2bd9fabae2`. Homebrew ran standalone
consumers before installing Haivision. No additional Homebrew test was performed
on the developer machine for this follow-up.

This validates the complete vcpkg prefix being moved with its dependencies;
it does not qualify moving an individual library or a Homebrew bottle. Official
official package submission, a vcpkg registry, and Ubuntu/Fedora packaging
remain separate milestones.

## Public Homebrew tap

The [Robotweax Homebrew tap](https://github.com/Robotweax/homebrew-tap) was
published on 2026-09-24 using the generated `brew pr-pull` workflow with the
reviewed head `7b89e8add61e02f4ad1ab43ef2a18d22959c18aa`. The
[macOS 15 arm64 test-bot run](https://github.com/Robotweax/homebrew-tap/actions/runs/35993079194)
passed syntax, source build, bottle creation, bottle installation and formula
tests. The published `arm64_sequoia` asset in
[release robotweax-srt-0.2.5](https://github.com/Robotweax/homebrew-tap/releases/tag/robotweax-srt-0.2.5)
has SHA-256 `66b6cb080c0c633495572fb306dcec0136a8ba6783eef00f13e06582e8930d1b`,
matching the CI artifact and the formula's bottle block. A direct installation
and formula test from the public tap also passed on Apple Silicon macOS 26;
that host used the source fallback. Public installation:

```sh
brew install robotweax/tap/robotweax-srt
```

This tap does not replace Haivision's formula or redirect applications already
linked against it. The test installation on the development Mac was removed.
