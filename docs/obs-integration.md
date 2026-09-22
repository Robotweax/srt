# OBS integration

Build OBS's production `obs-ffmpeg` and `obs-x264` modules with Robotweax SRT.
The native `ffmpeg_mpegts_muxer` output calls the SRT API directly. The
`ffmpeg_source` media source receives through FFmpeg's `libavformat`. Both
paths must use the same Robotweax shared library in one OBS process.
No new OBS transport plugin or URL scheme is required.

## Scope and pinned sources

The reproducible qualification profile is headless Linux with Xvfb and Mesa
software rendering, IPv4 loopback, Live/Message MPEG-TS, H.264 video and AAC
audio, cleartext and AES-CTR with 128-bit keys. It builds real libobs, media
source, native SRT output, and software encoders; the test driver uses the
public libobs API. It is not an OBS Studio GUI installer, a binary drop-in
replacement for an existing distribution, or Windows/macOS qualification.
For the separate Qt frontend build and user-facing acceptance procedure, see
[OBS Linux desktop qualification](obs-desktop.md).
For the narrower Windows x64 module and media-path profile, see
[OBS Windows integration](obs-windows.md).
The profile does not qualify RIST, hardware encoders, Rendezvous, IPv6,
AES-GCM, adverse-network performance, or quantitative A/V synchronization.

| Component | Immutable revision |
| --- | --- |
| OBS Studio 32.2.2 | `ba2f32bdf791005443988a4955e963663e16b1ed` |
| FFmpeg | `3acec0a1af2dda0a0838689b8b8649e7deb080a0` |
| GStreamer 1.28.7 reference peer | `070125524a8422e29d3b69a372ed4f62fd343ffa` |
| Haivision SRT 1.5.7 reference provider | `899348d8318eb9a3c5a5b6ec43c4a1114288773a` |

The reference provider is installed separately and runs only in peer
processes. Do not load Haivision SRT and Robotweax's compatible `srt_*`
symbols into the same OBS process. Rebuilding only OBS while retaining a
system FFmpeg that loads another SRT provider does not establish integration.

## Build and run the qualification

On Ubuntu 24.04:

```sh
sudo apt-get install build-essential cmake ninja-build git pkg-config \
  python3-venv libssl-dev libglib2.0-dev flex bison extra-cmake-modules \
  libsimde-dev uthash-dev libjansson-dev uuid-dev libx11-dev libx11-xcb-dev \
  libxcb1-dev libxcb-randr0-dev libxcb-xinput-dev libgl1-mesa-dev \
  libegl1-mesa-dev libdrm-dev libva-dev libpci-dev librist-dev libx264-dev \
  xvfb xauth libgl1-mesa-dri zlib1g-dev
python3 -m venv /path/to/obs-tools
/path/to/obs-tools/bin/pip install meson==1.9.1
export PATH="/path/to/obs-tools/bin:$PATH"
```

Clone the four upstream repositories into dedicated source directories and
check out the immutable revisions above. The OBS checkout must be writable
and otherwise unmodified. Submodules are not needed for this restricted
profile. From the current Robotweax repository:

```sh
JOBS=4 tests/obs/build_and_test.sh \
  /path/to/obs-source /path/to/gstreamer-source /path/to/ffmpeg-source \
  /path/to/haivision-srt-source "$PWD/build-obs-qualification"
```

Use a fresh work directory. This builds and installs namespaced shared
Robotweax with opt-in `srt.pc`, the OBS-specific shared FFmpeg profile, OBS,
and the separate reference peer. It then runs the actual media modules under
Xvfb. `evidence/results.txt` contains successful test groups; failures remain
nonzero even though the output is also logged. Build logs, peer logs and
decoded-frame hashes are retained for diagnosis. CI uploads only these
diagnostics, not OBS, FFmpeg, x264 or other third-party binaries.

For an existing isolated Robotweax installation, the individual build helpers
are:

```sh
tests/obs/configure_ffmpeg.sh /path/to/ffmpeg-source /path/to/ffmpeg-build \
  /path/to/robotweax-prefix /path/to/ffmpeg-prefix
make -C /path/to/ffmpeg-build REVISION=8.0.git-3acec0a -j4
make -C /path/to/ffmpeg-build REVISION=8.0.git-3acec0a install
tests/obs/configure.sh /path/to/obs-source /path/to/obs-build \
  /path/to/robotweax-prefix /path/to/ffmpeg-prefix /path/to/obs-prefix
cmake --build /path/to/obs-build --parallel 4
cmake --install /path/to/obs-build
```

The standard [FFmpeg smoke profile](ffmpeg-integration.md) is intentionally
smaller and is not sufficient for OBS. This profile installs shared avcodec,
avformat, avdevice, avfilter, avutil, swscale and swresample, along with the
software codecs and fixture filters used by the qualification.
PNG decoding and image demuxers are enabled for OBS desktop interface assets;
this does not make the profile a general-purpose FFmpeg build.

The `REVISION` make variable is FFmpeg's supported version-string input. For
this pinned snapshot it preserves the source `RELEASE` baseline (`8.0.git`)
and commit identity. OBS's version finder cannot parse a shallow checkout's
default `git-YYYY-MM-DD-HASH` string. This is not a claim that the snapshot is
a released FFmpeg version; component ABI versions remain unchanged.

## Build adaptations and provenance

The helper supplies OBS's `OBS_VERSION_OVERRIDE` as
`32.2.2-robotweax-qualification` after verifying the exact source revision.
This keeps version detection independent of Git tags, including in shallow
SHA-only checkouts, and identifies the restricted build profile. The canonical
OBS version remains `32.2.2`.

OBS's `FindLibsrt.cmake` searches for `libsrt.pc` and library names `srt` or
`libsrt`. Robotweax's opt-in metadata is `srt.pc` and its namespaced library
is `librobotweax-srt`. The helper therefore validates pkg-config provenance
and explicitly supplies `Libsrt_LIBRARY` and `Libsrt_INCLUDE_DIR`; setting
`PKG_CONFIG_PATH` alone is insufficient. Each FFmpeg component's metadata
must also resolve to the selected FFmpeg installation.

The pinned OBS build does not offer a switch for selecting just the two
required plugins. `prepare_source.py` replaces only `plugins/CMakeLists.txt`
in the dedicated checkout with an independently authored two-module build
selection. It checks the original file's SHA-256, accepts its own prepared
content on subsequent calls, and rejects unknown contents without overwriting
them. It changes no plugin implementation. Keep this checkout separate from
one used to build a complete OBS desktop package.

The qualification CMake hook supplies POSIX `<netdb.h>` and `<arpa/inet.h>`
includes explicitly for `obs-ffmpeg`: its SRT adapter uses the corresponding
declarations without including those headers itself. Robotweax's installed
header contract is not expanded to emulate incidental transitive includes.
The hook also restores isolated runtime library paths after OBS's target
helpers set their defaults. `ENABLE_NEW_MPEGTS_OUTPUT` stays enabled; its
existing build requires librist even though RIST is not exercised here.

These conclusions were derived from the pinned OBS revision's
`cmake/finders/FindLibsrt.cmake`, `cmake/finders/FindFFmpeg.cmake`,
`cmake/linux/helpers.cmake`,
`cmake/common/versionconfig.cmake`,
`plugins/CMakeLists.txt`, `plugins/obs-ffmpeg/CMakeLists.txt`,
`plugins/obs-ffmpeg/cmake/dependencies.cmake`, `obs-ffmpeg-srt.h`,
`obs-ffmpeg-mpegts.c`, and `obs-ffmpeg-source.c` within that plugin directory,
and `shared/media-playback/media-playback/media.c`.
No upstream implementation, tests, or internal transport design is copied
into Robotweax. The libobs test driver is independently authored against the
public OBS headers `libobs/obs.h`, `obs-source.h`, and `obs-service.h`.
The test driver's stop ordering also follows the public output lifecycle,
cross-checked against `libobs/obs-output.c` at that same revision: it does
not issue another forced stop after an output has become inactive.
FFmpeg version-string handling was checked against `Makefile` and `ffbuild/version.sh` at
the pinned FFmpeg revision.

## Runtime checks and support boundary

The smoke suite requires:

- SRT send and receive, each as Caller and Listener, cleartext and encrypted,
  against the separately linked reference provider;
- moving decoded 320x180 video and non-silent decoded audio, rather than only
  byte counts or connection success;
- simultaneous encrypted receive through libavformat and native SRT output
  in the same OBS process;
- explicit rejection of a wrong passphrase and denied Stream ID, followed by
  a valid caller on the same reference listener;
- repeated native output start/stop after the receiver disconnects, and
  bounded shutdown while an input or output waits for a connection;
- process mappings showing the selected OBS module, selected libavformat,
  exactly one namespaced Robotweax SRT library and no competing `libsrt`,
  plus `srt_startup` symbol-origin checks through both the native module and
  libavformat.

Observed video/audio confirms media delivery, not pixel-perfect re-encoding,
audio fidelity, frame-perfect A/V sync, or lossless delivery during abrupt
teardown. Native output restart is driven explicitly through libobs; this
does not establish all GUI reconnect policies. Consult the canonical
[compatibility boundary](compatibility.md) before widening the profile.

OBS and the FFmpeg/x264 profile have their own licensing and redistribution
requirements. This repository provides build recipes and independently
authored test code, not a relicensed OBS distribution.
