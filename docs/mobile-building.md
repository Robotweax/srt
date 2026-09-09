# Experimental Android and iOS builds

Mobile support is under development. Initial compile/link checks pass as recorded
below; device interoperability and a supported minimum OS are not yet established.
The desktop build matrix remains the qualified platform set.

## Initial compile/link evidence (2026-09-09)

All three targets built the Release static SRT library with AES-GCM enabled and
linked `tests/package_consumer/c_consumer.c` (compiled as C11, linked with the target
C++ driver) against SRT and target OpenSSL 3.6.3. The source is the mobile branch
with explicit `std::int64_t` template selection in the two timeout `std::min`
calls in `compat/connection.cpp` and `compat/epoll.cpp`. Android libc++ exposes
different `long` / `long long` aliases here; implicit template deduction failed.
The bound calculation and public API are unchanged.

| Target | Toolchain / target setting | Result |
| --- | --- | --- |
| Android ARM64 | NDK r27d (27.3.13750724), Clang 18.0.4, API 28 | Static library + AArch64 ELF consumer link passed |
| iOS ARM64 device | Xcode 26.6, AppleClang 21.0.0, SDK 26.5, deployment 15.0 | Static library + Mach-O consumer link passed |
| iOS ARM64 simulator | Same Xcode, simulator SDK 26.5, deployment 15.0 | Static library + simulator consumer link passed |

Host: macOS ARM64; CMake 4.4.2, Ninja. OpenSSL revision and recipes appear below.
These were local cross-build checks, not mobile CI or runtime tests. Consumers
were not executed on devices/simulators. Signing, app packaging, JNI/Swift wrappers,
network transfers, timing and background behavior remain unqualified. Android NDK
emitted CMake deprecation warnings; OpenSSL's Android API define emitted a macro
redefinition warning. Neither was a SRT compiler error.

## Prerequisites

- CMake 3.20+, Ninja, Python 3.10+, and the exact target toolchain.
- Android: an explicitly selected Android NDK with its CMake toolchain.
- iOS: full Xcode with device/simulator SDKs; Command Line Tools alone are insufficient.
- OpenSSL 3 Crypto built separately for each target, installed as
  `include/openssl/...` and `lib/libcrypto.a`. Android static Crypto must be PIC.
  Device ARM64, simulator ARM64 and macOS ARM64 libraries are not interchangeable.

Use the same deployment target in OpenSSL and SRT builds. Record NDK/Xcode,
compiler, SDK, OpenSSL revision/configuration and SRT commit with each result.
The helper supplies explicit Crypto/include paths, but does not prove the archive
has the correct platform or deployment target. A final application link and device
test are mandatory; creating a static archive alone cannot prove dependency linkage.

Select Xcode for this shell without changing the machine-wide developer directory:

```sh
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
xcrun --sdk iphoneos --show-sdk-path
xcrun --sdk iphonesimulator --show-sdk-path
```

Keep the Android NDK in a permanent installation directory rather than relying on
a mounted installer volume. Pass that exact directory to `--ndk`.

### Building target OpenSSL

The initial experiment pins OpenSSL 3.6.3 at commit
`aae016bfd52fcad2bc9657c2c782cfdf73b1ed5f`. This is a reproduction pin, not a
recommendation to freeze security updates indefinitely. Use separate empty build
directories and installation prefixes for each target. With `OPENSSL_SOURCE`
pointing to that checkout and `PREFIX` to the target installation, run one of:

```sh
# iOS device, from its empty OpenSSL build directory:
perl "$OPENSSL_SOURCE/Configure" ios64-xcrun no-shared no-tests \
  --prefix="$PREFIX" --libdir=lib -miphoneos-version-min=15.0

# iOS ARM64 simulator, from a different empty directory:
perl "$OPENSSL_SOURCE/Configure" iossimulator-arm64-xcrun no-shared no-tests \
  --prefix="$PREFIX" --libdir=lib -mios-simulator-version-min=15.0

# Android ARM64 on a macOS build host, from another empty directory:
export ANDROID_NDK_ROOT=/path/to/android-ndk
export PATH="$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/darwin-x86_64/bin:$PATH"
perl "$OPENSSL_SOURCE/Configure" android-arm64 no-shared no-tests \
  --prefix="$PREFIX" --libdir=lib -D__ANDROID_API__=28 -fPIC
```

After the selected configuration, run `make -j2 build_libs` and
`make install_dev`. These commands build libraries; they do not execute OpenSSL's
test suite or qualify cryptographic behavior on a device.

## Configure and build

From the repository root, replace the placeholder paths below. Deployment versions
are initial experiment examples, not minimum-version support commitments.

```sh
./tools/python tools/mobile_configure.py android-arm64 \
  --ndk /path/to/android-ndk \
  --openssl-root /path/to/openssl-android-arm64 \
  --deployment-target 28 --build-dir build-mobile-android
cmake --build build-mobile-android --parallel 2 --target robotweax_srt

./tools/python tools/mobile_configure.py ios-arm64 \
  --openssl-root /path/to/openssl-ios-arm64 \
  --deployment-target 15.0 --build-dir build-mobile-ios
cmake --build build-mobile-ios --parallel 2 --target robotweax_srt

./tools/python tools/mobile_configure.py ios-simulator-arm64 \
  --openssl-root /path/to/openssl-ios-simulator-arm64 \
  --deployment-target 15.0 --build-dir build-mobile-ios-simulator
cmake --build build-mobile-ios-simulator --parallel 2 --target robotweax_srt
```

Add `--plan` to print the command without running CMake or checking dependencies.
The helper never installs SDKs, deletes caches, changes Xcode selection or accepts
licenses. Normal configuration requires an empty build directory and explicit
target dependencies. It builds Release/PIC/static with AES-GCM enabled, and disables
host tests/tools/examples. Existing desktop defaults are unchanged.

Android consumers must package the matching shared C++ runtime and link Crypto
and all transitive dependencies. iOS consumers must link matching target libraries;
this step does not yet create an XCFramework or Swift package.

## Qualification still required

1. Compile all three target variants and link a consumer calling the public C API.
2. Verify startup/cleanup, encrypted caller/listener send/receive against a desktop
   peer, exact payload integrity, statistics and timeout/cancellation behavior.
3. Test real devices: app pause/resume, screen lock, network loss/reconnect and IPv6.
4. Measure timing, CPU, power and memory; simulator results do not qualify devices.
5. Add narrowly selected compile/link CI and app packaging only after the builds
   are reproducible. Do not enable expensive mobile jobs for unrelated commits.

Multicast permissions, local-network privacy and background execution are app-level
integration work, not supplied by a static SRT library. No network migration or
unrestricted background streaming is promised.

Toolchain references: [Android NDK CMake](https://developer.android.com/ndk/guides/cmake)
and [CMake cross compilation](https://cmake.org/cmake/help/latest/manual/cmake-toolchains.7.html).
