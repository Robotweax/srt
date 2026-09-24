# Package manager recipes

These recipes package the immutable Robotweax SRT 0.2.5 source commit
`492a7d61390cbec86e44e177ec034f0f5d9a5cc3`. The reviewed Homebrew formula
is published in [Robotweax/homebrew-tap](https://github.com/Robotweax/homebrew-tap),
with an Apple Silicon macOS 15 bottle. The vcpkg overlay is available below; no
vcpkg registry, PPA or COPR is published yet.

## Package contract

- Use the distinct package name `robotweax-srt` and namespaced install layout.
  Do not overwrite Haivision headers, libraries, `srt.pc` or package entries.
- Export `RobotweaxSRT::srt` and `robotweax-srt.pc`. Public headers are below
  `include/robotweax-srt`; consumers obtain include paths from package metadata.
- Use OpenSSL from the package manager. BCrypt and AES-GCM preview are outside
  the initial recipes. Do not bundle third-party application binaries.
- Preserve project version 0.2.5, ABI line 0.2 and compatible API 1.5.7 as separate
  version axes. Download only the exact source revision with verified hashes.
- Installing this package does not redirect installed FFmpeg, GStreamer, VLC or
  OBS applications. Rebuild consumers explicitly with the selected provider;
  filesystem coexistence does not guarantee two SRT providers in one process.
- Keep source recipes in this repository. Distribution repositories may carry
  reviewed copies of these recipes, version indexes and build artifacts; they
  must not carry an independently modified protocol implementation.

## Homebrew recipe

`homebrew/robotweax-srt.rb` is the canonical shared-library recipe using Homebrew
OpenSSL. Install the reviewed copy from the public tap:

```sh
brew install robotweax/tap/robotweax-srt
```

Homebrew may ask you to trust this external formula. The published bottle is
qualified for Apple Silicon macOS 15. A source installation from the public tap
also passed on Apple Silicon macOS 26. To evaluate a proposed recipe change in
a development tap, copy the formula to its `Formula/` directory, then run:

```sh
brew install --build-from-source YOUR_USER/YOUR_TAP/robotweax-srt
brew install --only-dependencies --include-test YOUR_USER/YOUR_TAP/robotweax-srt
brew test YOUR_USER/YOUR_TAP/robotweax-srt
brew audit --strict YOUR_USER/YOUR_TAP/robotweax-srt
```

The formula includes a C consumer using Robotweax-specific symbols and checks
that generic Haivision paths were not installed. The macOS 15 bottle passed Homebrew test-bot, including installation from the
created bottle; its published SHA-256 matches the reviewed CI artifact. Other
Homebrew bottle platforms require their own native checks.

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
[qualification evidence](qualification-0.2.5.md) for observed results and gaps.

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
