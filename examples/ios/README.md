# Experimental iOS encrypted echo app

This UIKit/Objective-C++ example uses the public SRT C API. It is a one-shot
IPv4 Caller, not a streaming bridge or production mobile SDK. Enter the desktop
peer address, port and a test passphrase, choose AES-CTR or AES-GCM, then connect.
It verifies an exact echo, shows a non-destructive statistics snapshot and closes.
Disconnect/backgrounding requests cancellation; an in-flight call can take up to
the configured three-second timeout. Socket ownership stays on one worker.
There is no automatic reconnect or background streaming.

## Build for Apple Silicon Simulator

First build simulator SRT and OpenSSL as described in
[mobile-building](../../docs/mobile-building.md). Use absolute paths below.

```sh
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
cmake -S examples/ios -B build-ios-app -G Ninja \
  -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphonesimulator \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
  -DSRT_INCLUDE_DIR="$PWD/include" \
  -DSRT_ARCHIVE=/absolute/path/to/simulator/librobotweax-srt.a \
  -DCRYPTO_ARCHIVE=/absolute/path/to/simulator/libcrypto.a
cmake --build build-ios-app
xcrun simctl install booted build-ios-app/SRTMobileDemo.app
xcrun simctl launch booted ai.robotweax.srt.mobile-demo
```

Start the desktop `robotweax_srt_message_demo` built with AES-GCM enabled:

```sh
export SRT_TEST_KEY=synthetic-mobile-test-key
./build-examples/robotweax_srt_message_demo listener \
  --bind 127.0.0.1 --port 24580 --crypto gcm \
  --passphrase-env SRT_TEST_KEY --timeout-ms 15000
```

Use the same key and crypto mode in the app. Restart the listener for each test.
For a physical device, use a reachable LAN address and listener binding, a signed
device build, and grant local-network access. Physical devices are not yet tested.
No passphrase is stored by the app; use synthetic credentials only for this demo.

## Automation and evidence

Simulator launch environment variables `SIMCTL_CHILD_SRT_DEMO_HOST`,
`SIMCTL_CHILD_SRT_DEMO_PORT`, `SIMCTL_CHILD_SRT_DEMO_KEY`,
`SIMCTL_CHILD_SRT_DEMO_CRYPTO` (`ctr` or `gcm`) populate the form.
`SIMCTL_CHILD_SRT_DEMO_AUTORUN=1` starts the same connection path automatically.
The final non-secret result is written to the app's `Documents/result.txt`.
Remove any previous result before an automated run to avoid reading stale output.

On 2026-09-09, an installed app on iPhone 17 Pro Simulator (iOS 26.5,
Xcode 26.6, arm64) passed both CTR and GCM exact echo exchanges against the
macOS desktop peer, including `srt_bistats(socket, stats, 0, 1)` and orderly
completion. Both desktop peers exited successfully. Simulator results do not
establish device timing, power use, LAN privacy behavior or background reliability.
