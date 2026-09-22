# OBS Windows integration

This profile builds the pinned OBS production `obs-ffmpeg` and `obs-x264`
modules on Windows x64 with one Robotweax compatibility DLL. Native MPEG-TS
output and FFmpeg-backed SRT input both import the same `srt.dll`; a second SRT
implementation is kept in a separate process only as the interoperability
peer.

The Windows profile complements the broader [Linux OBS qualification](obs-integration.md).
It is a headless module and media-path qualification, not a signed OBS installer
or a complete Qt desktop distribution.

## Pinned inputs and dependency boundary

The profile uses OBS Studio 32.2.2 at
`ba2f32bdf791005443988a4955e963663e16b1ed` and its declared Windows x64
dependency archive `windows-deps-2026-07-15-x64.zip`, SHA-256
`6f90e9598fa10cff5ad23cdcfae49b87868c07bf896b02cd464582b4ce2f2ba9`.
OBS downloads and verifies that archive through its own pinned build metadata.

The archive's FFmpeg `avformat-62.dll` imports `srt.dll`. The recipe builds
Robotweax as a shared, legacy-layout DLL and explicitly gives its import library
and headers to OBS. Because the headless build has no `obs-studio` executable to
invoke OBS's desktop bundler, the recipe assembles an isolated runtime from the
pinned dependency DLLs while explicitly excluding their `srt.dll`, then copies
the selected Robotweax DLL into that one slot. The assembled tree is rejected
unless its `srt.dll` is byte-identical to the selected Robotweax build. Runtime
module enumeration and PE dependency checks confirm that `obs-ffmpeg.dll` and
`avformat-62.dll` resolve through the one bundled provider.

This isolated profile uses Robotweax's BCrypt backend, so the compatibility DLL
has no separate OpenSSL runtime dependency. BCrypt remains an experimental
Robotweax backend; this qualification does not change the project's default
OpenSSL source build or qualify the OpenSSL Windows SDK inside OBS. The legacy
library name is confined to the dedicated OBS prefix and must not be installed
over another SRT distribution.

## Build and qualify

Use a current x64 Visual Studio C++ developer environment with CMake and
Python 3.10 or newer. Clone OBS into a dedicated, otherwise unmodified checkout
at the revision above. From the Robotweax repository:

```powershell
./tests/obs/build_windows.ps1 `
  -ObsSource C:/src/obs-studio `
  -WorkDirectory C:/build/robotweax-obs-windows
```

The work directory must not exist. The script never repurposes an existing
build tree. It creates separate Robotweax, OBS and reference-provider runtime
directories and retains qualification logs under `evidence`. The recipe
installs OBS's runtime and public Development headers/import library into its
isolated prefix before compiling the qualification peer.

OBS's Windows dependency setup normally downloads and checks Qt even when the
frontend is disabled. `prepare_windows_source.py` applies a hash-guarded
build-only change that selects only the pinned non-Qt dependency archive and
skips the Qt cross-compile check for `ENABLE_FRONTEND=OFF`. The pinned source
validation accepts either LF or Windows CRLF checkout line endings. The same
guarded preparation passes the pinned version and headless setting to OBS's
automatic x86 helper configuration.
Unknown or partially modified sources are rejected. The existing minimal OBS
plugin selection remains separately hash-guarded.

## Runtime evidence

The Windows smoke test exercises two production paths:

1. A synthetic moving I420 source with stereo audio is encoded by OBS x264/AAC
   and sent through the native SRT MPEG-TS output as a Robotweax Caller. The
   separately loaded reference `srt.dll` receives as Listener. The captured
   result must contain at least 100 MPEG-TS packets with at least 95 percent
   sync-byte alignment.
2. The reference provider sends that capture as an encrypted AES-CTR Caller to
   the OBS FFmpeg media source in Listener mode. OBS must report moving decoded
   video and non-silent decoded audio before normal shutdown.

The reference DLL is the original SRT provider from the immutable OBS dependency
archive. Its executable and DLL reside outside the OBS runtime directory, and
the harness rejects identical provider binaries. Test passphrases are public
synthetic fixtures, not credentials.

This initial profile does not qualify the Windows Qt frontend, installers,
ARM64, hardware encoders, capture devices, Stream ID authorization, AES-GCM,
Rendezvous, adverse networks, quantitative A/V synchronization, or long-duration
operation. Those claims require separate evidence.

OBS, FFmpeg, x264 and the reference SRT binary retain their upstream licenses.
CI uploads diagnostic text only, not third-party binaries.
