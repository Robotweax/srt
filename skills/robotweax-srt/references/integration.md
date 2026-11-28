# Release selection and integration

The examples in this skill use v0.3.5 at `fd9dac31675b12022579d28a748abb0839004cde`. Confirm the tag and artifact checksums/provenance, then inspect sources and evidence at that pin. For another release, resolve its tag to a commit and replace these references with its own. Re-evaluate build availability, option names, accepted ranges, combinations and evidence. A release recipe documents how to reproduce a result. Use the linked qualification for the exact scope and check its run revision and outcome before inheriting a verified status. The final-tag core CI and separately dispatched ecosystem qualification have different scopes. [v0.3.5 release and provenance][release] · [Integration guide][integration] · [API compatibility][api] · [Wire and application compatibility][compat] · [Examples][examples] · [Testing and interop][testing] · [Final-tag CI][ci] · [Integration workflows and pins][workflows] · [Performance evidence][performance]

## Native applications and migration

Use the installed C socket ABI from C or C++ via `<srt/srt.h>`. For v0.3.5, use `find_package(RobotweaxSRT 0.3.5 EXACT CONFIG REQUIRED)` and `RobotweaxSRT::srt` from the selected install. Point `RobotweaxSRT_DIR` at its config directory if multiple installs exist. Use the namespaced `robotweax-srt` pkg-config package when supported by the build system. Use the small installed `<robotweax_srt.h>` only for its documented native C boundary. Source-tree C++ headers are private. [Building][building] · [Integration guide][integration] · [Installed consumer][consumer]

When migrating from Haivision, inventory imports, options, event handling and callback assumptions, then compare them against the pinned API table and limitations. Rebuild against matching headers and update packaging and loader search paths. Recompile ABI 0.2 consumers for ABI 0.3. Keep Haivision and Robotweax in separate processes where they both export compatible `srt_*` symbols. Recheck admission callbacks if using groups, including the nonzero Robotweax admission domain required by the v0.3.5 contract. Retest partial I/O, epoll wakeups and teardown through the app's normal entry points. [API compatibility][api] · [Limits][limits] · [Integration guide][integration] · [Connection groups][groups]

## Provider verification

Record application and plugin identities, Robotweax product release (0.3.5), ABI line (0.3), API compatibility version (1.5.7), crypto backend, architecture, source/package revision and artifact hashes independently. Both this release and stock Haivision can report 1.5.7 via `srt_getversion()` or `pkg-config --modversion srt`. The opt-in `srt.pc` exposes compatibility to downstream configure tools and carries separate Robotweax version/ABI variables. Inspect `pkg-config --variable=pcfiledir srt`, `pkg-config --variable=robotweax_release srt`, CMake cache and the actual compile/link lines. Reconfigure from a clean build directory if switching providers. [Integration guide][integration] · [FFmpeg metadata][ffmpeg]

- **Dynamic linking.** Inspect the executable and transitive imports with `readelf -d` and loader resolution on Linux, `otool -L` plus rpaths on macOS, or PE imports (for example `dumpbin /DEPENDENTS`) on Windows. Inspect loaded modules during real SRT activity using process maps/loader tracing, `vmmap`/dyld diagnostics or Windows module inspection. If there can be multiple definitions, capture the module that supplies a representative resolved `srt_*` symbol using debugger, loader binding trace or in-process address-to-module lookup. Compare that resolved artifact and architecture against trusted package provenance. File basename, SONAME and search path alone are insufficient.
- **Plugins.** Identify the actual plugin first (for example `gst-inspect-1.0 srtsrc`, OBS module logs or VLC verbose module selection). Repeat dependency and binding inspection on the loading process and plugin, including FFmpeg/libavformat loaded by OBS. Test in the deployed environment and use an isolated plugin registry/prefix for experiments. The pinned GStreamer harness demonstrates checking symbol origin during real use. [GStreamer provider check][gst] · [OBS integration][obs] · [VLC integration][vlc]
- **Static linking.** Capture the final link map or linker trace showing which archive and object satisfied `srt_*` references. Verify the archive hash and source/build manifest, then bind that proof to the final executable or plugin hash and test that artifact. Symbols alone don't identify the provider. Runtime module lists won't show static archives. If an opaque binary has no provenance, report provider unverified and obtain build proof or rebuild.

Use `python3 scripts/verify_artifact.py --observed /resolved/library --expected /trusted/library` from the skill directory, or supply `--sha256` from trusted provenance. For a static build pass the archive from the final link evidence. The script compares bytes after discovery. Always retain the evidence that identified the observed path.

## Application paths at v0.3.5

| Application | Released scope and path | Other combinations |
| --- | --- | --- |
| FFmpeg | Rebuild the pinned source `3acec0a1af2dda0a0838689b8b8649e7deb080a0` with the opt-in SRT pkg-config compatibility file and `--enable-libsrt`. Confirm `-protocols` lists SRT input and output and resolve libavformat's binding. The tagged smoke covers Live IPv4, 1316-byte TS, clear/AES-128-CTR and elementary MPEG-2 byte comparison. [FFmpeg recipe][ffmpeg] · [Pin][workflows] | Verify additional codecs/audio, retail builds, adverse networks and extensions separately. The default adapter lacks a Robotweax AES-GCM AVOption. |
| GStreamer | Rebuild the pinned 1.28.7 plugin (`070125524a8422e29d3b69a372ed4f62fd343ffa`). On Linux/OpenSSL the qualification covers IPv4 Live, both roles and directions, clear/AES-128-CTR, media, listener reconnect and rejection. Select the plugin/provider pair with the isolated registry and prefix. [GStreamer recipe][gst] | macOS build recipe has separate evidence requirements. Windows and stock plugins need separate qualification. |
| OBS | Use the pinned OBS 32.2.2 source (`ba2f32bdf791005443988a4955e963663e16b1ed`) and pinned FFmpeg. Qualification is separate for Linux Xvfb/Mesa/OpenSSL modules and Qt desktop, Windows x64 experimental BCrypt with isolated legacy `srt.dll`, and locally built Apple Silicon OpenSSL. Check the per-platform module/desktop role, direction and lifecycle boundaries. [Linux modules][obs] · [Linux desktop][obs-desktop] · [Windows][obs-win] · [Windows desktop][obs-win-desktop] · [macOS][obs-mac] · [macOS desktop][obs-mac-desktop] | Existing desktop distributions, Windows ARM64/OpenSSL in OBS, Intel Mac and signed/notarized distribution are outside those claims. |
| VLC | Rebuild the existing modules from pinned 3.0.24-rc1 commit `6de05adcbaf2e8b85fe86aad4169393098628119` with the documented hash-guarded payload option patch. Headless Linux/OpenSSL qualification covers IPv4 Live MPEG-TS/MPEG-2 picture, clear/AES-128-CTR, input Caller/Listener and output Caller. [VLC recipe][vlc] | Audio, GUI, Windows/macOS, output Listener and other stock binaries need their own evidence or are unsupported by the pinned adapter. |

If a stock app is only the remote peer, keep its installed provider and test that exact peer build across the wire. The above recipes apply when embedding Robotweax into the app.

[release]: https://github.com/Robotweax/srt/releases/tag/v0.3.5
[integration]: https://github.com/Robotweax/srt/blob/v0.3.5/docs/integration.md
[api]: https://github.com/Robotweax/srt/blob/v0.3.5/docs/api-compatibility.md
[compat]: https://github.com/Robotweax/srt/blob/v0.3.5/docs/compatibility.md
[examples]: https://github.com/Robotweax/srt/tree/v0.3.5/examples
[testing]: https://github.com/Robotweax/srt/blob/v0.3.5/docs/testing.md
[ci]: https://github.com/Robotweax/srt/actions/runs/45266706013
[workflows]: https://github.com/Robotweax/srt/blob/v0.3.5/.github/workflows/ci.yml
[performance]: https://github.com/Robotweax/srt/blob/v0.3.5/docs/performance.md
[building]: https://github.com/Robotweax/srt/blob/v0.3.5/docs/building.md
[consumer]: https://github.com/Robotweax/srt/tree/v0.3.5/examples/installed-consumer
[limits]: https://github.com/Robotweax/srt/blob/v0.3.5/docs/limitations.md
[groups]: https://github.com/Robotweax/srt/blob/v0.3.5/docs/connection-groups.md
[ffmpeg]: https://github.com/Robotweax/srt/blob/v0.3.5/docs/ffmpeg-integration.md
[gst]: https://github.com/Robotweax/srt/blob/v0.3.5/docs/gstreamer-integration.md
[obs]: https://github.com/Robotweax/srt/blob/v0.3.5/docs/obs-integration.md
[obs-desktop]: https://github.com/Robotweax/srt/blob/v0.3.5/docs/obs-desktop.md
[obs-win]: https://github.com/Robotweax/srt/blob/v0.3.5/docs/obs-windows.md
[obs-win-desktop]: https://github.com/Robotweax/srt/blob/v0.3.5/docs/obs-windows-desktop.md
[obs-mac]: https://github.com/Robotweax/srt/blob/v0.3.5/docs/obs-macos.md
[obs-mac-desktop]: https://github.com/Robotweax/srt/blob/v0.3.5/docs/obs-macos-desktop.md
[vlc]: https://github.com/Robotweax/srt/blob/v0.3.5/docs/vlc-integration.md
