# Experimental Android and iOS builds

Mobile support is under development. These configuration recipes are not evidence
of a successful mobile build, device interoperability or a supported minimum OS.
The desktop build matrix remains the qualified platform set.

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
