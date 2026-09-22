# OBS Linux desktop qualification

This profile builds the real OBS Studio Qt frontend with Robotweax SRT in
both its native MPEG-TS output and FFmpeg-backed media source. It extends
the [headless integration](obs-integration.md); it does not replace that
profile or qualify a general-purpose OBS distribution.

## Build boundary

Use the same immutable OBS, FFmpeg, GStreamer and reference-SRT revisions
listed in the integration guide. The desktop profile selects `obs-ffmpeg`,
`obs-x264`, `rtmp-services` (including its custom SRT service), and
`obs-transitions`. It enables the Qt frontend with the narrowly scoped,
version-locked MPEG-TS lifecycle correction described below. It does not
change the SRT protocol implementation. Service-list updates and the What's New dialog
are disabled for this isolated build.
The source revisions and build profile are pinned; distribution dependency
updates mean this is not a promise of bit-identical binaries.

### Desktop MPEG-TS lifecycle correction

The desktop recipe applies `tests/obs/prepare_desktop_lifecycle.py` to the
pinned OBS 32.2.2 MPEG-TS module. This independently authored correction
releases the previous FFmpeg state before reinitializing it, and releases
partially initialized state before reporting a failed start. Without this
cleanup, reconnect can discard the previous error-string allocation and
FFmpeg state. Existing serialized start/writer joins remain unchanged.
The correction validates the original file's SHA-256, is idempotent, and
rejects unknown or partially modified source. The headless recipe remains
unpatched; do not reuse a desktop checkout for it.

The lifecycle analysis used OBS revision
`ba2f32bdf791005443988a4955e963663e16b1ed`,
`plugins/obs-ffmpeg/obs-ffmpeg-mpegts.c`, its `obs-ffmpeg-srt.h` companion,
`libobs/obs-output.c` and `libobs/util/bmem.c`. Runtime allocation tracing
on the synthetic reconnect fixture identified a retained 61-byte error
string in the MPEG-TS logging path per tested interruption. This is an
OBS-version-scoped lifecycle finding, not an SRT wire-compatibility rule.
The script contains no copied upstream implementation. The modified OBS
build retains OBS's upstream license; this does not relicense its source.

The supported test setup is Ubuntu 24.04, X11/Xvfb, Mesa software rendering,
IPv4 loopback, H.264/AAC MPEG-TS, and cleartext or AES-CTR. The restricted
FFmpeg profile is not a general media player. Browser sources, screen/device
capture, desktop/microphone capture, hardware encoders, Wayland, Windows and
macOS are outside this desktop profile. Do not replace libraries in a system
OBS installation or mix a distribution FFmpeg with this build.

## Reproduce the build

Install the base dependencies from the integration guide, plus:

```sh
sudo apt-get install qt6-base-dev qt6-base-private-dev libqt6svg6-dev \
  libcurl4-openssl-dev nlohmann-json3-dev libxkbcommon-dev xdotool
```

First run `tests/obs/build_and_test.sh` as documented there. This produces
the isolated Robotweax, FFmpeg, GStreamer and reference-provider prefixes.
Keep these installed prefixes in place: runtime library paths are absolute.

Use a **second, clean OBS checkout** at the pinned revision. A checkout
already prepared for the headless profile is deliberately rejected rather
than silently repurposed. No OBS submodules are needed for this selection.

```sh
JOBS=4 tests/obs/build_desktop.sh \
  /path/to/clean-obs-desktop-source \
  /path/to/headless-work/robotweax-srt \
  /path/to/headless-work/ffmpeg \
  /path/to/new-desktop-work
```

The last directory must not exist. The helper configures, builds and installs
the frontend to `new-desktop-work/obs`; it records the build log under
`new-desktop-work/evidence`. It does not install a desktop package globally.
The equivalent lower-level configure command is:

```sh
tests/obs/configure.sh /path/to/clean-obs-desktop-source \
  /path/to/new-build /path/to/robotweax-prefix /path/to/ffmpeg-prefix \
  /path/to/obs-install desktop
```

Omitting the final argument retains the existing headless profile.

## Automated desktop smoke test

```sh
xvfb-run -a ./tools/python tests/obs/run_desktop_smoke.py \
  --obs-prefix /path/to/new-desktop-work/obs \
  --srt-prefix /path/to/headless-work/robotweax-srt \
  --ffmpeg-prefix /path/to/headless-work/ffmpeg \
  --gst-prefix /path/to/headless-work/gstreamer \
  --reference-srt-prefix /path/to/headless-work/reference-srt \
  --artifacts /path/to/new-desktop-evidence
```

The evidence directory must not exist. Each case creates a private HOME and
XDG configuration, cache, data and runtime directories. It never imports a
user's scenes, stream keys, plugins or OBS preferences. Test passphrases are
public synthetic fixtures, not credentials.

The test opens the real Qt application and operates its start/stop streaming
hotkeys. It checks native SRT output against an isolated Haivision-backed
GStreamer receiver, cleartext and encrypted. An additional encrypted duplex
case receives through the GUI media source and forwards through the native
output in the same OBS process. The visible application window must exist.
All cases require decoded
moving video and audible audio, the expected installed library mappings with no
second SRT provider, frontend stop acknowledgement and bounded normal shutdown.
OBS's reported allocation count is compared with an idle frontend run and
must not increase. The pinned frontend can report one allocation even without
streaming; this comparison does not certify that OBS or Qt is leak-free.
The separate headless gate continues to verify the native and FFmpeg symbol
providers and its full existing interoperability matrix.

Logs and decoded frame hashes are diagnostic evidence. The default invocation
does not exercise reconnect or long durations; use the additional options
below. Neither mode establishes A/V synchronization, adverse-network
performance or every desktop workflow.

## Interactive acceptance procedure

### Optional automated reconnect and soak run

Add `--reconnect-cycles 3 --soak-seconds 3600` to the desktop smoke command
above, with a fresh evidence directory. CI selects three reconnects and a
30-second soak; this is a regression check, not a long-duration qualification.
The additional checks use the encrypted native output and a looping local
synthetic source. They terminate the receiver, leave it absent for eight
seconds, and restart it on the same port. OBS automatic reconnect is enabled;
no additional start hotkey is sent. Each new receiver must decode moving
video and non-silent audio within 15 seconds of restarting.

The soak starts after recovery and repeatedly decodes fresh byte ranges, so
old successful media cannot hide a later stall. Bounded two-MiB snapshots
avoid loading an hour-long capture into memory. The full transport capture
remains on disk; allow at least one GiB of free space per hour. The
`desktop-soak-memory.jsonl` evidence records elapsed time, OBS resident memory
and received bytes. Memory is recorded for analysis, not judged against an
arbitrary universal growth threshold. Existing provider and normal-shutdown
allocation checks still apply. This does not measure A/V synchronization,
qualify network-source reconnect, or inject packet loss/jitter.
For a soak, the harness explicitly sets `ROBOTWEAX_TEST_PEER_TIMEOUT_SECONDS`
to the requested duration plus 60 seconds. The reference test peer retains
its 20-second default otherwise and rejects values outside 1..86500 seconds.

Shorter durations are useful harness checks, but must not be reported as a
60-minute qualification. A failed recovery fails the run; there is no manual
restart fallback.

### Manual desktop checks

Run on a Linux desktop with a display, using a new dedicated state directory:

```sh
desktop_state="$PWD/robotweax-obs-desktop-state"
mkdir "$desktop_state"
mkdir "$desktop_state/home" "$desktop_state/config" "$desktop_state/cache" \
  "$desktop_state/data" "$desktop_state/runtime"
chmod 700 "$desktop_state/runtime"
env -u LD_PRELOAD -u LD_LIBRARY_PATH -u QT_PLUGIN_PATH -u OBS_PLUGINS_PATH \
  HOME="$desktop_state/home" XDG_CONFIG_HOME="$desktop_state/config" \
  XDG_CACHE_HOME="$desktop_state/cache" XDG_DATA_HOME="$desktop_state/data" \
  XDG_RUNTIME_DIR="$desktop_state/runtime" QT_QPA_PLATFORM=xcb LIBGL_ALWAYS_SOFTWARE=1 \
  /path/to/new-desktop-work/obs/bin/obs --disable-updater
```

Stop if directory creation fails; choose a new directory instead of deleting
existing data. Use the isolated prefixes without inherited `LD_PRELOAD`,
`LD_LIBRARY_PATH`, `QT_PLUGIN_PATH` or custom OBS plugin-path overrides.

1. Add a **Media Source** containing the generated H.264/AAC `fixture.ts`.
   Enable looping. Confirm a moving preview and audio-meter activity.
2. In **Settings → Stream**, select the custom service and use an isolated
   test receiver's `srt://127.0.0.1:PORT?mode=caller` URL. Select software x264
   and AAC output. Start and stop streaming using the visible controls.
   Verify moving video and audible audio at the receiver after each start.
3. Repeat with AES-CTR and matching test passphrases. Verify that a wrong
   passphrase produces no decoded media; restore the correct configuration
   and confirm recovery. Do not publish real keys or unredacted profiles.
4. Disable **Local File** on a second Media Source and use an SRT sender URL
   with MPEG-TS input. Confirm preview/audio and forward that scene to a
   separate receiver, including one encrypted duplex case.
5. Enable OBS automatic reconnect, restart the receiver and record whether
   the GUI returns to streaming without a manual start. Repeat three times.
   A successful explicit restart is not evidence of automatic reconnect.
6. Run for at least 60 minutes at a recorded resolution, frame rate and
   bitrate. Record reconnects, decoder errors, dropped frames, memory growth
   and observed A/V drift. Exercise controlled loss/jitter only on an
   isolated test network, never by changing the host's general interfaces.
7. Exit while streaming and while awaiting a peer. Confirm process shutdown
   within 15 seconds and that the same ports can be reused by a fresh run.

For each case record the Robotweax/OBS/FFmpeg revisions, OS, graphics backend,
roles, encryption, media parameters, duration, expected result and actual
result. Mark unperformed cases **not tested**, not passed. Retain receiver
evidence and sanitized logs. Fail acceptance on crashes, hangs, provider
mixing, unintended media delivery with a wrong key, or a failed claimed
recovery. Agree quantitative loss/jitter and A/V drift thresholds before
making performance claims; the short smoke test supplies no such guarantee.

OBS and its dependencies retain their upstream licenses. This repository
provides build recipes and tests, not a relicensed OBS binary distribution.
Desktop build/configuration behavior was checked against the pinned OBS
revision's `frontend/CMakeLists.txt`, `frontend/cmake/ui-qt.cmake`,
`frontend/cmake/os-linux.cmake`, `frontend/cmake/feature-plugin-manager.cmake`,
`frontend/obs-main.cpp`, `frontend/OBSApp.cpp`, the frontend
`widgets/OBSBasic_{Hotkeys,Service,SceneCollections,Streaming}.cpp` files,
`frontend/utility/CrashHandler.cpp`, `libobs/obs-scene.c`, and
`plugins/rtmp-services/{CMakeLists.txt,rtmp-custom.c}`. Configuration
fixtures and build selection are independently authored; no upstream
implementation is copied into Robotweax.
The same pinned revision's `frontend/utility/SimpleOutput.cpp` and
`frontend/utility/AdvancedOutput.cpp` were inspected to confirm the public
profile keys `Output/Reconnect`, `Output/RetryDelay` and `Output/MaxRetries`.
