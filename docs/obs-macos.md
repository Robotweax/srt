# OBS macOS integration (Apple Silicon)

This is an isolated source-build and qualification profile for the production
OBS 32.2.2 `obs-ffmpeg` and `obs-x264` modules on Apple Silicon. It exercises
the native `ffmpeg_mpegts_muxer` SRT output and the `ffmpeg_source` SRT media
input. Both paths must bind the same namespaced Robotweax SRT dylib in one
OBS process. A separate process linked to the pinned Haivision SRT library
acts as the independent reference peer. This is **not** a signed OBS app,
installer, portable ZIP, or a release artifact.

For the separate Qt application and automatic reconnect qualification, see
[OBS macOS desktop](obs-macos-desktop.md).

## Reproduce the qualification

Use macOS arm64 with full Xcode and macOS SDK 26.5 or newer, CMake 3.28+, Ninja,
pkg-config, and Homebrew `openssl@3`. OBS pins its own prebuilt build
dependencies; the script fetches the exact archive after verifying its
SHA-256. It never loads that archive's `libsrt.dylib` into the OBS peer.

Check out these immutable sources into separate directories:

| Component | Revision |
| --- | --- |
| OBS Studio 32.2.2 | `ba2f32bdf791005443988a4955e963663e16b1ed` |
| FFmpeg | `3acec0a1af2dda0a0838689b8b8649e7deb080a0` |

From the Robotweax repository, run:

```sh
JOBS=3 tests/obs/build_macos.sh \
  /path/to/obs-source /path/to/ffmpeg-source /absolute/fresh-work-directory
```

The OBS checkout must be dedicated to this test; a hash-guarded local plugin
selection is the only OBS source adjustment. The script builds a shared
namespaced Robotweax library, an FFmpeg variant linked against it, and the
production OBS modules. The CMake cache is checked so that OBS cannot silently
select the OBS dependency archive's FFmpeg or SRT. At runtime, the test also
checks loaded image paths and resolves both SRT call sites to Robotweax.

`evidence/results.txt` records the passing test groups, and `evidence/*.log`
and `evidence/*.frames` retain build, peer, and decoded-frame evidence. The
required CI job uses an Apple Silicon macOS runner and uploads only text
diagnostics. It does not assemble or distribute a binary package.

The scope is IPv4 loopback, AES-CTR with a 128-bit key, MPEG-TS, moving video,
non-silent audio, OpenGL rendering, and clean shutdown. Only the required OBS
module/renderer targets are built; the unrelated Metal renderer is not part of
this profile. It does not qualify the OBS Qt frontend,
physical-network reliability, hardware encoders, Rendezvous, IPv6, AES-GCM,
performance, quantitative A/V sync, or a distributable macOS application.
