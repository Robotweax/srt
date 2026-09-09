# Building and installing

Robotweax SRT uses CMake and builds as a C++20 library. This guide covers
development builds, lean installation builds, static and shared linkage, and
package consumption.

## Installation layouts

The default `ROBOTWEAX_SRT_INSTALL_LAYOUT=namespaced` installs public headers
under `include/robotweax-srt` and names the library `robotweax-srt`. CMake's
`RobotweaxSRT::srt` and `robotweax-srt.pc` supply the correct include root and
library. Application includes such as `<srt/srt.h>` remain unchanged.

For old integrations that require the original paths, explicitly configure
`-DROBOTWEAX_SRT_INSTALL_LAYOUT=legacy`: headers use `include`, and the library
is named `srt`. Use a separate prefix if Haivision is installed. Invalid layout
values are rejected. Both layouts support static and shared builds.

The optional `ROBOTWEAX_SRT_INSTALL_LIBSRT_PKGCONFIG_COMPAT=ON` still provides
`srt.pc`, referencing the selected library name. It conflicts with Haivision's
`srt.pc` and is therefore OFF by default even with the namespaced layout.
No `libsrt` alias or symlink is created in the default layout.

Migration: rebuild applications using the package metadata. Update hard-coded
`-lsrt` to `-lrobotweax-srt` and include paths to the new root, or select legacy.
Changing layout does not remove files from an earlier installation: use a clean
prefix or remove only files owned by that prior installation. Existing binaries
requiring the old library name must retain that library or be rebuilt.

This permits separate applications to use separate providers on one system.
It does not guarantee both providers can safely coexist in one process: their
public `srt_*` symbols remain identical. No symbol renaming is introduced.

## Requirements

| Dependency | Requirement |
| --- | --- |
| CMake | 3.20 or newer |
| C++ compiler | C++20 support |
| OpenSSL | 3.0 or newer, Crypto component |
| Threads | Platform thread library |
| Python | 3.10 or newer when tests are enabled |

Linux, macOS, and Windows are covered by the project build matrix. Install the
OpenSSL development package for your platform and ensure its architecture
matches the compiler and generator.

## Important CMake options

For unqualified mobile cross-build experiments, see
[Experimental Android and iOS builds](mobile-building.md).

| Option | Default | Purpose |
| --- | --- | --- |
| `BUILD_SHARED_LIBS` | `OFF` | Build a shared rather than static library |
| `ROBOTWEAX_SRT_BUILD_TESTS` | `ON` | Build and register tests |
| `ROBOTWEAX_SRT_BUILD_BENCHMARKS` | Static: `ON`; shared: `OFF` | Build native microbenchmarks |
| `ROBOTWEAX_SRT_BUILD_FUZZERS` | `OFF` | Build libFuzzer targets |
| `ROBOTWEAX_SRT_BUILD_TOOLS` | Static: `ON`; shared: `OFF` | Build protocol development tools |
| `ROBOTWEAX_SRT_BUILD_EXAMPLES` | `OFF` | Build public API demonstration applications |
| `ROBOTWEAX_SRT_WARNINGS_AS_ERRORS` | `ON` | Treat library warnings as errors |
| `ROBOTWEAX_SRT_INSTALL_LIBSRT_PKGCONFIG_COMPAT` | `OFF` | Install opt-in `srt.pc` metadata for shared-library consumers such as FFmpeg |
| `ENABLE_AEAD_API_PREVIEW` | `OFF` | Enable the released Robotweax 0.2 AES-GCM extension |

Native benchmarks and protocol tools use source-level C++ implementation
interfaces. A shared-library build therefore requires those two options to be
off. Build them in a separate static development tree when needed.

Public examples are independently opt-in, use only the installed C API, and
can link to static or shared builds. They are not installed by default. See
the [examples guide](../examples/README.md).

## Development build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Use `Release` for representative timing or throughput measurements. Debug and
sanitizer builds intentionally change scheduling and performance.

## Lean static build

```sh
cmake -S . -B build-static \
  -DCMAKE_BUILD_TYPE=Release \
  -DROBOTWEAX_SRT_BUILD_TESTS=OFF \
  -DROBOTWEAX_SRT_BUILD_BENCHMARKS=OFF \
  -DROBOTWEAX_SRT_BUILD_TOOLS=OFF
cmake --build build-static --parallel
```

Static consumers must resolve Robotweax SRT's transitive platform and OpenSSL
requirements. Prefer the exported CMake package or `pkg-config --static`
instead of assembling linker flags manually. A static C application also
needs the C++ runtime: compile C sources with the C compiler, then link the
objects with the matching C++ compiler driver as shown below.

## Shared-library build

```sh
cmake -S . -B build-shared \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=ON \
  -DROBOTWEAX_SRT_BUILD_TESTS=ON \
  -DROBOTWEAX_SRT_BUILD_BENCHMARKS=OFF \
  -DROBOTWEAX_SRT_BUILD_TOOLS=OFF
cmake --build build-shared --parallel
ctest --test-dir build-shared --output-on-failure
```

Robotweax SRT 0.2.0 uses shared-library ABI line 0.2. The shared artifact
exports the supported public C ABI; source-tree C++ implementation symbols are
not a stable binary interface.

## AES-GCM build

AES-CTR is the default encryption profile. Enable the released AES-GCM
extension in a separate tree:

```sh
cmake -S . -B build-gcm \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_AEAD_API_PREVIEW=ON
cmake --build build-gcm --parallel
ctest --test-dir build-gcm --output-on-failure
```

`ENABLE_AEAD_API_PREVIEW` is the historical name of the opt-in build flag. In
0.2.0 it selects the released, version-pinned extension surface. The installed
CMake target propagates the compile definition, and the generated pkg-config
metadata publishes it in `Cflags`.

Use matching headers, compile definitions, and libraries across the entire
consumer graph. A default consumer must not load or link against a GCM-enabled
binary as though the two interfaces were interchangeable.

## Install

```sh
cmake --install build-static --prefix "$PWD/stage"
```

The installation contains:

- the `srt` library artifact;
- public headers under the selected include directory;
- `RobotweaxSRTConfig.cmake` and `RobotweaxSRTTargets.cmake`;
- `robotweax-srt.pc`;
- opt-in `srt.pc` compatibility metadata when explicitly enabled;
- release documentation and the ABI manifest.

Keep `LICENSE` (including the copyright and full MIT terms) and
`THIRD_PARTY.md` with redistributed packages. The downloadable CI bundle
`robotweax-interop-tools` also includes both files at the archive root. It
contains selected Robotweax test programs, not a supported SDK installation;
Haivision reference libraries and programs are excluded.

Use `DESTDIR` with the normal CMake install command when packaging for a system
root. Avoid moving only the library without its matching headers and package
metadata.

## Consume with CMake

```cmake
cmake_minimum_required(VERSION 3.20)
project(example LANGUAGES C CXX)

find_package(RobotweaxSRT 0.2 CONFIG REQUIRED)

add_executable(example main.cpp)
target_link_libraries(example PRIVATE RobotweaxSRT::srt)
```

Configure the consumer with a custom prefix:

```sh
cmake -S consumer -B consumer-build \
  -DCMAKE_PREFIX_PATH="$PWD/stage"
cmake --build consumer-build --parallel
```

The complete, CI-tested version of this pattern is the
[installed package consumer](../examples/installed-consumer/README.md). It is
also shipped below the installed documentation directory so an integrator can
build it without access to the Robotweax SRT source tree.

The package version policy is same-minor compatibility. Before 1.0, do not
assume a consumer built for one minor ABI line can load another.

## Consume with pkg-config

On POSIX systems, select the installed package's actual pkg-config directory.
For a **shared-library installation**, a C application can compile and link
with the C compiler:

```sh
export PKG_CONFIG_PATH="$PWD/stage/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
cc -std=c11 app.c $(pkg-config --cflags --libs robotweax-srt) -o app
```

For a **static-library installation**, compile the source as C and use the
C++ compiler driver for the final link:

```sh
cc -std=c11 $(pkg-config --cflags robotweax-srt) -c app.c -o app.o
c++ app.o $(pkg-config --static --libs robotweax-srt) -o app
```

`--static` supplies the package's private OpenSSL and platform dependencies;
it does not select a C++ runtime or force the linker to prefer `librobotweax-srt.a` over
a shared library in the same search directory. Use a static-only installation
for this recipe. The C and C++ compiler drivers must use a compatible toolchain
and target architecture. Do not compile the `.c` source as C++ merely to obtain
the runtime; link its C object with `c++` instead. A CMake C-only target can
likewise set `LINKER_LANGUAGE CXX` in a project enabling both `C` and `CXX`.

Both recipes require the installed library's runtime dependencies to be
discoverable when executing `app`; shared installations also require the
Robotweax library itself on the platform's loader search path.

Installation library directories can be `lib`, `lib64`, or a platform-specific
value. Derive `PKG_CONFIG_PATH` from the actual install tree rather than
assuming `lib`.

## Multi-config generators

Visual Studio, Xcode, and Ninja Multi-Config select their configuration at
build and test time:

```sh
cmake -S . -B build
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release --prefix "$PWD/stage"
```

Do not rely on `CMAKE_BUILD_TYPE` with a multi-config generator.

## OpenSSL discovery

If CMake finds the wrong OpenSSL installation, start from a fresh build tree
and provide the normal CMake discovery hints for your environment, such as
`OPENSSL_ROOT_DIR` or `CMAKE_PREFIX_PATH`. Mixing headers and libraries from
different OpenSSL installations can produce configure, link, or runtime
failures.

## Avoid stale build caches

CMake caches compilers, dependency locations, feature flags, and linkage mode.
Do not reuse one directory after changing any of these dimensions:

- generator or compiler toolchain;
- target architecture;
- static versus shared linkage;
- default AES-CTR versus opt-in AES-GCM interface;
- OpenSSL installation;
- source checkout with incompatible generated state.

Create descriptive directories such as `build-debug`, `build-shared`, and
`build-gcm`. If a cached tree is suspect, remove that build directory and
configure a new one; never delete source files to repair a CMake cache.

## Troubleshooting checklist

1. Confirm the compiler supports C++20.
2. Confirm CMake reports OpenSSL 3.x for the intended architecture.
3. Confirm Python 3.10 or newer is selected when tests are on.
4. Check that shared builds disable benchmarks and protocol tools.
5. Check that the consumer and library agree on the AES-GCM build flag.
6. Reconfigure in a clean build directory after changing toolchains or flags.
7. Run package-consumer tests before shipping an installation.

See [Testing](testing.md) and [Integration](integration.md) for the next steps.
See [FFmpeg integration](ffmpeg-integration.md) for the isolated shared-library
profile required by FFmpeg's conventional libsrt discovery.
