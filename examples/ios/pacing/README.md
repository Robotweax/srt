# iOS pacing measurement app

This experimental foreground app compares legacy mobile pacing with the
**default-off** timer-wait candidate for issue #353. It reuses
`benchmarks/scalability_peer.cpp` and the public SRT API, with one backlogged
caller, cleartext synthetic messages and a desktop receiver. It is a measurement
harness, not a streaming application or mobile support commitment.

The candidate keeps the existing 1 ms pacing credit and waits for future pacing
deadlines even after coarse timer wakes. Due work and bounded route sweeps still
continue immediately. Unlike the desktop policy, it never restores pacing spin
because timers are coarse. Coarse mobile timers can therefore reduce throughput;
that tradeoff must be measured before enabling the policy by default.

## Build both variants

Use full Xcode and matching iOS ARM64 OpenSSL as described in
[mobile-building](../../../docs/mobile-building.md). Use the same deployment
target for OpenSSL and this app. From the repository root, substitute your own
paths and Apple development team ID:

```sh
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
TEAM_ID=YOUR_TEAM_ID
CRYPTO=/absolute/path/to/openssl-ios-arm64

cmake -S examples/ios/pacing -B build-pacing-baseline -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 \
  -DOPENSSL_INCLUDE_DIR="$CRYPTO/include" \
  -DOPENSSL_CRYPTO_LIBRARY="$CRYPTO/lib/libcrypto.a" \
  -DROBOTWEAX_SRT_ENABLE_EXPERIMENTAL_MOBILE_PACING=OFF \
  -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGN_STYLE=Automatic \
  -DCMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM="$TEAM_ID"
cmake --build build-pacing-baseline --config Release --parallel 3 -- \
  -allowProvisioningUpdates
```

Repeat with a fresh directory `build-pacing-candidate` and the experimental
option `ON`. Do not reuse a desktop, simulator or differently configured cache.
The bundle IDs are `ai.robotweax.srt.pacing.baseline` and
`ai.robotweax.srt.pacing.candidate`, so both apps can coexist. Each build produces
`Release-iphoneos/SRTMobilePacing.app`. For compile/link checks without a signing
identity, replace the two signing arguments with
`-DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED=NO`; that app cannot be installed.

Install using Xcode or `xcrun devicectl device install app --device "$DEVICE" APP`.
The phone must be paired, trusted, unlocked and in Developer Mode. A Simulator
build can exercise the harness but does not qualify phone timing or CPU.

## Desktop receiver and one trial

Build `robotweax_srt_scalability_peer` on the desktop with
`ROBOTWEAX_SRT_BUILD_BENCHMARKS=ON`. Use the same source revision for both app
variants and the desktop peer. Connect the phone and desktop to the same local
network and allow the app's Local Network permission when iOS asks.

For a requested duration `seconds`, rate `bps` and payload size `bytes`, use
`messages = floor(bps * seconds / (bytes * 8))`. Start the receiver first. For
example, 30 seconds at 2 Mbit/s with 188-byte messages uses 39893 messages:

```sh
robotweax_srt_scalability_peer listener --host 0.0.0.0 --port 24583 \
  --connections 1 --messages 39893 --message-size 188 \
  --latency-ms 120 --timeout-ms 95000 > receiver.jsonl 2> receiver.stderr
```

Apply an outer process timeout: the listener can wait indefinitely for admission.
Enter the desktop's numeric IPv4 address and the same profile in the app, then
start the trial. Keep the app foreground. It prevents idle screen sleep during a
trial and marks a backgrounded trial invalid. A transfer failure has a bounded
network timeout; the app does not implement background streaming or a cancel UI.

For reproducible launches, these environment variables replace the UI settings:

```sh
xcrun devicectl device process launch --device "$DEVICE" --terminate-existing \
  --environment-variables '{"SRT_PACING_HOST":"192.0.2.1","SRT_PACING_PORT":"24583","SRT_PACING_BPS":"2000000","SRT_PACING_BYTES":"188","SRT_PACING_SECONDS":"30","SRT_PACING_AUTORUN":"1"}' \
  ai.robotweax.srt.pacing.candidate
```

Replace the example address with the desktop address. Rates are 2000000, 5000000
or 8000000; payloads are 188 or 1316 bytes; durations are 10 or 30 seconds.
Do not terminate an active trial to start the next one.

The app writes UUID-named JSON resource reports and peer JSONL logs in its
Documents directory, plus a convenience `latest.json`. Copy the directory after
a trial, preserving all attempts:

```sh
xcrun devicectl device copy from --device "$DEVICE" \
  --domain-type appDataContainer \
  --domain-identifier ai.robotweax.srt.pacing.candidate \
  --source Documents --destination candidate-documents
```

Pair each resource report with its referenced sender log and that trial's desktop
receiver log. Sender completion alone is insufficient: both peers must exit 0,
with exactly one `complete` event each, `integrity: true`, the requested message
count and `bytes == messages * payload_bytes`. Keep stderr and failed attempts.

## Physical-device comparison protocol

Before collecting results, record source revision and binary hashes, Xcode/SDK,
OpenSSL revision, phone model and OS, network, Low Power Mode, charging state,
screen brightness and thermal state. Keep these conditions fixed across variants.
No device identifier or personal network address belongs in a public result.

1. Grant Local Network permission and run an unscored 10-second warm-up for each
   variant. Confirm both peer logs and resource counters are usable.
2. For each of 2, 5 and 8 Mbit/s, test both 188-byte and 1316-byte payloads for
   30 seconds. Use baseline, candidate, candidate, baseline order for each profile.
3. Start each scored run at nominal thermal state, keep the app foreground and
   allow cooldown between runs. Restart the appropriate receiver for each run.
   Preserve invalid attempts and their reasons; repeat a whole profile block if
   environmental conditions changed.
4. Reject incomplete/backgrounded runs, any payload mismatch, unavailable CPU or
   wake-up counters, or a non-nominal thermal state before or after a scored run.
5. Calculate useful payload throughput using `bytes * 8 / (elapsed_us / 1e6)`
   from the receiver's `complete` event. Require at least 95% of requested rate
   for every scored run. Report sender/receiver retransmission and loss counters;
   unexplained changes require investigation before a performance conclusion.
6. Compare CPU seconds per useful Gbit and wake-ups per second, showing each run
   and per-profile medians. For this initial experiment, require at least 10%
   median CPU reduction for each 188-byte profile, and no greater than 10% median
   CPU regression for any 1316-byte control profile before recommending enablement.
   These are proposed qualification gates, not measured outcomes.

188-byte messages exercise sub-millisecond pacing at these rates; 1316-byte
messages are the control (their interval exceeds 1 ms even at 8 Mbit/s). The app
adjusts `SRTO_MAXBW` for the 16-byte SRT data header and keeps the library
backlogged; it does not add application rate sleeps. The app explicitly uses a
4 MiB SRT send buffer and a 4096-packet application window. At MSS 1500 the send
queue has 2849 slots, so it fills before the application window: backpressure
uses SRT epoll rather than the benchmark's 100-microsecond window polling loop.
The transport flow window remains 1024 packets. Queue storage remains bounded.
The original 64-packet harness runs are retained as exploratory results; they do
not qualify transport wake-ups or saturation throughput. Use identical corrected
harness settings for both variants and restart the full ABBA matrix.

CPU is the whole app process's `getrusage` delta around the peer run, including
startup and shutdown. Wake-up deltas come from Mach `TASK_POWER_INFO` interrupt,
platform-idle and timer-bin counters. These categories can overlap; do not sum
them into a unique total, equate them with scheduler dispatches, or infer battery
energy from them. Unsupported counters are null, not zero. More timer wakes with
less CPU may be a valid tradeoff; present both rather than claiming every counter
must fall. Thermal observations are not an energy measurement.

This first matrix qualifies only one foreground, cleartext, single-connection
iPhone workload. Android physical-device qualification, encrypted performance,
background behavior, network migration and long-running battery measurements
remain separate work. Keep the mobile option off by default until evidence
supports a platform-specific decision.


## Initial physical iPhone results — 2026-10-23

The timer candidate reduced CPU per delivered bit and met its scoped throughput
and CPU gates on one iPhone. It substantially increased wake-ups for 188-byte
messages. This is **not a full mobile qualification pass or a battery-life
result**: the original protocol's baseline throughput gates failed for the
5/8 Mbit/s controls, and one baseline connection failure remains unexplained.
Keep `ROBOTWEAX_SRT_ENABLE_EXPERIMENTAL_MOBILE_PACING=OFF` by default.

Tested source: `fcead32652951dccdb550863ac527bb76c80c819`. Both installed app
variants were Release builds from that source, differing in the mobile pacing
option, with the corrected 4096-packet application window and 4 MiB SRT send
buffer. Toolchain: Xcode 26.6 (17F113), iOS SDK 26.5, ARM64 deployment target 17.0;
OpenSSL 3.6.3 at `aae016bfd52fcad2bc9657c2c782cfdf73b1ed5f`.
The device was an iPhone 14 Pro (`iPhone15,2`) on iOS 27.0.1, foreground on the
same IPv4 Wi-Fi, USB connected to the desktop. Every completed scored run
reported nominal thermal state before/after and Low Power Mode off. This setup
did not measure energy or control ambient radio traffic.

The table contains per-profile medians of two runs per variant from six complete
ABBA blocks (24 successful transfers, requested duration 30 seconds each).
Arrows mean **legacy baseline → timer candidate**. CPU seconds per useful Gbit
normalizes for delivered payload; CPU percentage alone is misleading when the
legacy variant misses the requested rate.

| Target Mbit/s | Payload bytes | Delivered Mbit/s | CPU seconds / useful Gbit | CPU reduction | Interrupt wake-ups / second |
| --- | --- | --- | --- | --- | --- |
| 2 | 188 | 1.995 → 1.989 | 499.15 → 88.71 | 82.2% | 69 → 1370 |
| 2 | 1316 | 1.978 → 1.994 | 35.86 → 28.69 | 20.0% | 764 → 814 |
| 5 | 188 | 4.941 → 4.968 | 200.20 → 63.52 | 68.3% | 35 → 3210 |
| 5 | 1316 | 3.962 → 4.971 | 17.02 → 15.80 | 7.2% | 536 → 631 |
| 8 | 188 | 7.960 → 7.995 | 125.48 → 13.64 | 89.1% | 27 → 5275 |
| 8 | 1316 | 6.218 → 7.954 | 17.09 → 14.15 | 17.2% | 692 → 853 |

All 12 candidate runs delivered at least 99.3% of their requested payload rate,
with exact sender/receiver counts and payload integrity. The three 188-byte
profiles met the predeclared 10% median CPU reduction gate; all 1316-byte controls
met the no-more-than-10% CPU regression gate. Four baseline control runs missed
the 95% throughput gate. Therefore the protocol's requirement that *every scored
run* meet that rate gate did not pass, even though every candidate run did.

Wake-ups are not energy measurements. At 188 bytes the candidate replaces a
nearly continuously busy transport worker with roughly one timer wake per
packet, increasing interrupt wake-up rates to about 1370/3210/5275 per second at
2/5/8 Mbit/s. For the 5/8 Mbit/s controls, wake-ups per second increase while
total interrupt wake-ups per fixed payload decrease by about 6%/3%, because the
candidate finishes sooner. Timer-bin and platform-idle counts are retained in
the private per-run reports; the categories overlap and must not be summed.
Apple's [timer energy guidance](https://developer.apple.com/library/archive/documentation/Performance/Conceptual/EnergyGuide-iOS/MinimizeTimerUse.html)
recommends minimizing wake-ups and batching timers where timing permits. These
results support investigating bounded timer coalescing and measuring energy;
they do not establish a battery benefit from CPU savings alone.

### Failures and limits retained in the result

There were 28 scored attempts with the corrected harness: 27 complete transfers
and one failed baseline attempt. In the first 5 Mbit/s/1316-byte block, the last
baseline connection broke after 19.36 seconds; sender and receiver exited 5 with
incomplete delivery. The app remained foreground at nominal thermal state, and
the operator reported no network changes. The desktop address was unchanged;
checking the Mac power log did not identify a sleep/wake event in that window.
The cause is unknown. A separate instrumented baseline repeat succeeded; its
observer overhead excludes it from performance results. The entire affected
ABBA block was then repeated with complete data delivery. The original failed attempt and the
three successful runs in its incomplete block remain in the evidence and are
not silently replaced by a qualification pass.

Recovered loss/retransmission counts varied across the Wi-Fi runs. For the
188-byte profiles, baseline retransmission pairs were 2/2, 169/0 and 0/1123;
candidate pairs were 0/0, 0/0 and 2/0. For the complete 1316-byte blocks they
were baseline 0/0, 0/0 and 0/15, versus candidate 1/0, 0/0 and 0/0.
Every complete transfer retained exact application data. At 5/8 Mbit/s, comparing
the zero-loss baseline and candidate runs still shows the large small-packet CPU
benefit. This does not establish a loss-rate improvement or explain the isolated
connection break; stability investigation remains required.

The original 64-packet harness runs are separate exploratory evidence. They
exposed application-window polling and saturation problems and do not qualify
transport wake-ups or throughput. Neither those runs nor the instrumented repeat
are included in the table. The source revision, app binary hashes, every attempt,
resource JSON, sender/receiver logs, profile medians and comparison scripts are
retained in private qualification evidence; device identifiers and personal
network details are not published.

Before default enablement: investigate the baseline connection failure, assess
timer coalescing with bounded burst/latency behavior, and perform longer device
energy/resource checks. Android physical devices, encrypted performance,
background execution, connection groups and network migration remain unqualified.
