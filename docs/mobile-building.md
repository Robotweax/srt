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
These were local cross-build checks, not mobile CI. Subsequent runtime smoke
checks are recorded below. Signing, app packaging, JNI/Swift wrappers,
device networking, timing and background behavior remain unqualified. Android NDK
emitted CMake deprecation warnings; OpenSSL's Android API define emitted a macro
redefinition warning. Neither was a SRT compiler error.

## Initial emulator runtime checks (2026-09-09)

Source baseline: mobile branch commit `739ce62`. Android was tested first and
shut down before iOS was started, to avoid concurrent emulator memory pressure.

| Environment | Public C consumer | Encrypted message demo |
| --- | --- | --- |
| Android 15/API 35 AOSP ARM64, emulator 37.1.11 | Exit 0 | 4/4 two-process loopback cases passed |
| iOS 26.5, iPhone 17 Pro simulator | Exit 0 | 4/4 simulator caller to macOS listener cases passed |

Each four-case set used AES-256-CTR and AES-256-GCM, each in blocking and
epoll-driven nonblocking mode. `examples/srt_message_demo.cpp` verified an exact
synthetic payload echo, called `srt_bistats`, exchanged completion messages and
exited successfully on both ends. Each case sent one short application message;
these are smoke checks, not throughput, key-rotation or endurance qualification.

Android execution used `adb shell` with the matching `libc++_shared.so` in
`LD_LIBRARY_PATH`; iOS used `simctl spawn` on the simulator-built executable.
These are native process tests, not installed APK/iOS-app tests, and do not validate
application sandbox permissions or background execution.

The attempted Android-to-macOS cases did **not** pass: `sendto(10.0.2.2)` returned
`ENETUNREACH`. A separate shell `ip route get 10.0.2.2` and ping also reported
unreachable, while socket creation, options and bind succeeded. This identifies a
routing problem in the test environment, not a demonstrated SRT handshake defect.
The original failures remain part of the test record; loopback alone is not
desktop interoperability evidence. After emulator restart and network initialization,
the separate host tests passed as described below.

### Android routing recovery and network readiness

On the subsequent 2026-09-09 run, restarting the same AVD and allowing Android's
network services to initialize restored routing without root, manual routes,
firewall changes or SRT modifications. The initial boot observation had only the
dummy network and no connectivity service; the later observation had a default
Wi-Fi network and a policy route to `10.0.2.2` through `wlan0` (table 1016).
Host ping then passed, followed by **4/4 Android caller to macOS listener** cases:
AES-256-CTR/GCM, each blocking/nonblocking, with exact echo, statistics and both
processes exiting 0. These extend, rather than replace, the original loopback tests.

The exact reason the previous run remained without a usable route is not proven.
Recovery after restart does not establish a permanent emulator fix. Treat ADB
availability and `sys.boot_completed=1` as insufficient network readiness signals.
Before launching a network case, check the actual target route with a bounded wait:

```sh
ADB="$HOME/Library/Android/sdk/platform-tools/adb"
SERIAL=emulator-5554
ready=false
for attempt in $(seq 1 30); do
  if "$ADB" -s "$SERIAL" shell ip route get 10.0.2.2; then
    ready=true
    break
  fi
  sleep 2
done
if [ "$ready" != true ]; then
  echo "Android host route unavailable; do not start SRT tests" >&2
  exit 1
fi
```

This is a routing precondition, not proof that a UDP listener is reachable. Keep
the actual encrypted transfer as the end-to-end gate. On failure retain `ip rule`,
`ip route show table all` and network-service diagnostics; do not disable Android
security protections or retry SRT tests until a failed run disappears.

To repeat a case, run the message demo as `listener --port PORT` and as
`caller --host HOST --port PORT --message TEXT`, adding identical
`--crypto ctr` or `--crypto gcm`, `--passphrase-env SRT_TEST_KEY` and
`--timeout-ms 15000` on both sides. Add `--nonblocking` to both for the asynchronous
case. Use only a synthetic test key. For iOS `simctl spawn`, propagate it via
`SIMCTL_CHILD_SRT_TEST_KEY`; for Android, set it in the remote process environment.
For Android loopback both processes run inside the emulator with host `127.0.0.1`;
for the iOS test only the caller runs in the simulator, against the macOS listener.
Apply an outer process timeout as the blocking listener may otherwise wait for a
peer indefinitely. Preserve failed attempts and do not infer performance from RTT
values produced by this one-message exchange.

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

An experimental [UIKit encrypted echo app](../examples/ios/README.md) is available
for actual Simulator app-process testing. It is separate from the native CLI smoke
tests and does not constitute physical-device qualification.

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
