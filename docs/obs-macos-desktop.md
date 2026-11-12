# OBS macOS desktop qualification

This profile builds the OBS Studio 32.2.2 Qt application on Apple Silicon and
exercises its real frontend with Robotweax SRT. It extends the
[macOS module qualification](obs-macos.md); the same pinned OBS and FFmpeg
revisions, Robotweax library, and independent Haivision SRT reference peer
apply. The result is a local test build, not a signed or distributed OBS app.

## Build and run

Use the Xcode, CMake, Ninja, pkg-config and Homebrew `openssl@3` prerequisites
in the macOS integration guide. Check out the pinned OBS and FFmpeg revisions
into separate directories. The OBS checkout must be dedicated and otherwise
unmodified; the desktop source preparation uses a hash-guarded plugin selection
and the pinned MPEG-TS lifecycle cleanup described in the
[Linux desktop guide](obs-desktop.md). Use a fresh work directory:

```sh
JOBS=3 tests/obs/build_macos.sh \
  /path/to/obs-desktop-source /path/to/ffmpeg-source \
  /absolute/fresh-work-directory desktop
```

The default three-argument form remains the module-only profile. The desktop
form builds `OBS.app` in the Xcode build tree, verifies its embedded Robotweax
and FFmpeg libraries, runs the existing macOS module test, then runs the Qt
application test. It does not install into `/Applications`, sign for
distribution, archive, package, or upload the app. The CI job uploads text
logs, frame hashes, CMake cache and window/provider observations only.

The desktop form builds OBS 32.2.2 against the Qt archive of obs-deps
`2026-10-23` instead of the `2026-07-15` archive that OBS 32.2.2 pins. Both
contain Qt 6.11.1; the later one, pinned by OBS 33.0.1, backports the fix for
QTBUG-147602. Without it Qt releases the color space of an ICC-tagged image
before Core Graphics reads it, and OBS stops with `EXC_BREAKPOINT` on its first
wait cursor on macOS 26.7 and later. The build verifies the archive hash and
fails if CMake resolves any Qt package from another prefix. The OBS source, its
other prebuilt dependencies, FFmpeg and Robotweax SRT are unchanged, and the
module-only profile does not use Qt.

The desktop test creates a fresh private macOS Application Support directory
for every launch. It sets both `HOME` and `CFFIXED_USER_HOME`; macOS Foundation
does not necessarily honor `HOME` alone for Application Support. It never reads
or changes a real OBS profile. The test uses public synthetic media and a
synthetic passphrase.

The restricted FFmpeg build includes PNG decoding and image demuxing for OBS's
preview assets. Before launching the desktop, the fixture decodes the bundled
`overflow.png` into one hashed video frame. Missing desktop codec support fails
this preflight instead of repeatedly exercising an image-load failure path.

## Acceptance covered by CI

The test requires an on-screen OBS main window and a normal idle shutdown.
It starts streaming through the Qt frontend's `--startstreaming` option and
requires moving decoded video and audible audio at an independent encrypted
SRT receiver. After the receiver is interrupted for eight seconds, it requires
fresh decoded media from a new receiver without restarting OBS. In a second
launch, an independent encrypted SRT sender feeds OBS's FFmpeg media source;
OBS forwards the result through a separate encrypted native SRT output, whose
video and audio must decode. The process must shut down normally with its
allocation count no higher than the idle baseline. Build cache, app contents,
and running mappings must identify Robotweax SRT and the selected FFmpeg,
without a competing SRT provider.

The dedicated macOS CI job enables kernel core dumps in a runner-temporary
directory and verifies core generation and LLDB symbolization with a small
crash probe before building OBS. A failed endpoint's recorded PID selects its
core for LLDB's all-thread backtrace, registers, loaded images and fault-frame
disassembly. Only text diagnostics are uploaded; binary cores remain on the
temporary runner. Missing cores or debugger failures are reported explicitly.
This captures a reconnect crash without attaching a debugger to the running
qualification or changing its media deadlines.

The isolated desktop build also has opt-in allocation tracing. At the existing
shutdown allocation-count check it records live libobs allocation sizes, bounded
contents and allocation stacks. The runner resolves those stacks with `atos`
before uploading text evidence. The allocation limits remain unchanged.

This profile uses IPv4 loopback, AES-CTR with a 128-bit key, synthetic MPEG-TS,
software encoding and the hosted macOS GUI session. It does not qualify capture
devices, hardware encoders, adverse networks, AES-GCM, IPv6, quantitative A/V
sync, long-duration operation, signing, notarization, or redistribution. A
physical Mac acceptance run should follow before claiming those device or
deployment properties.
