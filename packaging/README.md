# Package manager recipes

## Published 0.2.8

These recipes select the immutable Robotweax SRT 0.2.8 product source
`8126e166ecd3d33987b3748e799ef40e5192d068`, not the current checkout. All 587 regular
files and executable bits were compared with Git before pinning the downloaded
GitHub archive. Archive SHA-256:
`f3ad362970d8b83337bd8344f796337a56ff52347c2a9e2ff18c41fd2b71422d`.

The [0.2.8 release](https://github.com/Robotweax/srt/releases/tag/v0.2.8) and separately qualified native Homebrew bottle are published. See the [qualification record](qualification-0.2.8.md) and [release process](../docs/release-process-0.2.8.md). [Historical 0.2.7 evidence](qualification-0.2.7.md) is retained. No vcpkg registry, PPA or COPR is published.

## Package contract

- Use the distinct package name `robotweax-srt` and namespaced install layout.
  Do not overwrite Haivision headers, libraries, `srt.pc` or package entries.
- Export `RobotweaxSRT::srt` and `robotweax-srt.pc`. Public headers are below
  `include/robotweax-srt`; consumers obtain include paths from package metadata.
- Use OpenSSL from the package manager. BCrypt and AES-GCM preview are outside
  the initial recipes. Do not bundle third-party application binaries.
- Preserve project version 0.2.8, ABI line 0.2 and compatible API 1.5.7 as separate
  version axes. Download only the exact source revision with verified hashes.
- Installing this package does not redirect installed FFmpeg, GStreamer, VLC or
  OBS applications. Rebuild consumers explicitly with the selected provider;
  filesystem coexistence does not guarantee two SRT providers in one process.
- Keep source recipes in this repository. Distribution repositories may carry
  reviewed copies of these recipes, version indexes and build artifacts; they
  must not carry an independently modified protocol implementation.

## Linux package prototypes

The [Ubuntu and Fedora recipes](linux/README.md) build DEB and RPM packages
from the pinned 0.2.8 source archive. Separate runtime and development
packages retain Robotweax-specific filenames and metadata. The Linux
packages workflow builds and installs them on Ubuntu 24.04 and Fedora 44,
executes installed consumers, checks coexistence with Haivision, and tests
removal. No PPA or COPR has been published.

## Homebrew recipe

`homebrew/robotweax-srt.rb` is the canonical shared-library recipe using Homebrew
OpenSSL. Install the reviewed copy from the public tap:

```sh
brew install robotweax/tap/robotweax-srt
```

Homebrew may ask you to trust this external formula. The published 0.2.8 bottle is
qualified for Apple Silicon macOS 15 (`arm64_sequoia`). Its downloaded bytes
match the native CI artifact and the published formula's SHA-256.
To evaluate a proposed recipe change in
a development tap, copy the formula to its `Formula/` directory, then run:

```sh
brew install --build-from-source YOUR_USER/YOUR_TAP/robotweax-srt
brew install --only-dependencies --include-test YOUR_USER/YOUR_TAP/robotweax-srt
brew test YOUR_USER/YOUR_TAP/robotweax-srt
brew audit --strict YOUR_USER/YOUR_TAP/robotweax-srt
```

The formula includes a C consumer using Robotweax-specific symbols and checks
that generic Haivision paths were not installed. Native Homebrew test-bot
qualified the 0.2.8 bottle, including installation from the created bottle.
Other bottle platforms require their own native checks. Historical source
installation on macOS 26 does not qualify a new 0.2.8 bottle for that platform.

## vcpkg overlay

The initial overlay targets Windows MSVC, Linux and macOS with OpenSSL. The
supports expression is a build scope, not a completed platform qualification.
From this repository, using an installed vcpkg:

```sh
vcpkg install robotweax-srt --overlay-ports=packaging/vcpkg/ports --triplet=arm64-osx
```

Select the triplet for your platform. For manifest consumers, configure the
same overlay directory and add `robotweax-srt` to dependencies. Link through
`find_package(RobotweaxSRT CONFIG REQUIRED)` and `RobotweaxSRT::srt`. Static C
consumers need the C++ linker. Windows consumers must also match the triplet's
MSVC runtime: `x64-windows-static` uses `/MT` (`/MTd` for Debug), whereas
`x64-windows` uses `/MD` (`/MDd` for Debug). For the static triplet, set
`CMAKE_MSVC_RUNTIME_LIBRARY` to `MultiThreaded$<$<CONFIG:Debug>:Debug>` before
creating consumer targets; selecting a triplet alone does not configure the
consumer's runtime. This overlay intentionally does not shadow the
existing `libsrt` port or rewrite other ports' dependencies.

## Acceptance before publishing a package

Test the actual manager installation in a clean environment, both alone and
alongside its Haivision package. Compile and run installed C/C++ consumers;
check provider identity, pkg-config/CMake paths, license and runtime dependency
metadata. Test relocation where required, update and removal without damaging
the other provider. Static and dynamic vcpkg profiles need separate checks.
Native execution evidence must be distinguished from cross-compilation.

Package recipe changes need package-focused CI. Full architecture, coexistence
and upgrade matrices belong to release qualification; routine packaging edits
do not require all OBS desktop builds.

## Automated qualification

`Package managers` runs when recipes, the consumer fixtures or its workflow
change, on matching main pushes, on version tags and on manual dispatch. It
builds the pinned release archive, **not the current protocol source checkout**.
The normal source CI remains responsible for changes to the protocol itself.

Homebrew runs a source installation, formula test and strict audit on macOS,
then C/C++ consumers first without and then with Haivision installed. The vcpkg matrix uses a pinned
vcpkg revision on Windows x64, Linux x64 and macOS arm64, with static and dynamic
triplets. Each triplet executes Release and Debug consumers first standalone,
then alongside Haivision, then from a moved installation prefix with the original
path absent. Both providers execute in separate processes; removing Robotweax is
followed by running the Haivision consumer again. Consumer sources are shared
with the existing installed-package tests and must remain compatible with the
pinned recipe version, or the recipe and fixtures must move together.

Passing this workflow does not qualify an upgrade from an older package, binary
bottles, all OS versions, or official distribution admission. See
[0.2.5 qualification evidence](qualification-0.2.5.md) for historical results;
these results do not qualify the [0.2.8 release](qualification-0.2.8.md).

The vcpkg qualification can also run locally using a bootstrapped checkout of
the pinned vcpkg revision (a new work directory is required):

```sh
TRIPLET=arm64-osx python3 packaging/tests/qualify_vcpkg.py \
  --vcpkg-root /path/to/vcpkg --work-root /tmp/srt-qualification
```

Set `MSVC_RUNTIME` as described above when using `x64-windows-static`. The
script isolates installed packages below its work directory. Relocation covers
the complete vcpkg installation including dependencies; it does not claim that
an individual library can be copied without its dependencies, or that Homebrew
bottles are relocatable.
