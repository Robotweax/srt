# Package qualification: 0.2.5

Status: development recipes; no public package channel has been published.
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
  executing Debug consumers remains an additional qualification step.
- The recipe's immutable source archive also passed the existing shared
  `abi_baseline` and `package_consumer` source-build tests (2/2).

Homebrew updated CMake to 4.4.3, OpenSSL to 3.6.4 and pkgconf to 3.0.7 while
preparing the tests. Its automatic removal initially removed existing unused
Python/ICU packages when the test formula was uninstalled. Python 3.13.15 and python-packaging 26.3 were restored, and ICU 75.1 was
restored as bottle revision 75.1_1; CI now sets `HOMEBREW_NO_AUTOREMOVE=1` as well as
`HOMEBREW_NO_INSTALL_CLEANUP=1`. These are host-management details, not package
runtime requirements.

## Release gates still open

- Hosted Windows/Linux results must be inspected; a matrix
  in a workflow is not evidence that a platform passed.
- Test on clean machines without Haivision as well as the coexistence case.
- Qualify bottles/relocation, upgrades and rollback when introducing a public
  tap or registry. There is no previous Robotweax package to upgrade here.
- Confirm supported OS versions and architectures before advertising support.
- Ubuntu/PPA and Fedora/COPR packaging remain the following workstream.

Installing Robotweax does not switch existing applications from Haivision.
The coexistence test uses separate processes, never both providers in one.
