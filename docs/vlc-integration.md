# VLC integration

Build VLC's existing SRT input and output modules against the namespaced
Robotweax shared library. No new VLC protocol, URL scheme, or transport plugin
is required. This is distinct from using an ordinary SRT-enabled VLC build as
a network peer: interoperability alone does not mean VLC uses Robotweax internally.

## Baseline and scope

The Linux qualification pins VLC commit
`6de05adcbaf2e8b85fe86aad4169393098628119` from the 3.0.x line (3.0.24-rc1),
not an unspecified installed VLC release. Its input supports Caller and
Listener; its SRT output uses Caller. Older VLC releases must be qualified
independently, particularly for listener support.

The build uses the existing opt-in `srt.pc`, whose version describes the SRT
API rather than the Robotweax release. It links to `librobotweax-srt` without
replacing Haivision's `libsrt`. All providers remain in separate install
prefixes and, for cross-provider tests, separate processes.

The initial profile is headless Linux, IPv4 loopback, Live/Message MPEG-TS,
MPEG-2 video, and cleartext or AES-CTR with a 128-bit key. It is not a GUI
installer, a general codec qualification, or a Windows/macOS package.

## Build the VLC integration

On Ubuntu 24.04, install the build dependencies:

```sh
sudo apt-get install build-essential cmake git pkg-config python3 \
  libssl-dev autoconf automake libtool gettext autopoint flex bison \
  libmpeg2-4-dev libdvbpsi-dev
```

From the Robotweax repository, choose isolated absolute paths:

```sh
robotweax_source="$PWD"
robotweax_prefix="$PWD/build-vlc-stage/srt"
vlc_prefix="$PWD/build-vlc-stage/vlc"
vlc_build="$PWD/build-vlc"

cmake -S . -B build-vlc-srt \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON \
  -DROBOTWEAX_SRT_INSTALL_LAYOUT=namespaced \
  -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_INSTALL_PREFIX="$robotweax_prefix" \
  -DROBOTWEAX_SRT_BUILD_BENCHMARKS=OFF -DROBOTWEAX_SRT_BUILD_TOOLS=OFF \
  -DROBOTWEAX_SRT_INSTALL_LIBSRT_PKGCONFIG_COMPAT=ON
cmake --build build-vlc-srt --parallel
cmake --install build-vlc-srt

git clone https://github.com/videolan/vlc.git /path/to/vlc-source
git -C /path/to/vlc-source checkout --detach \
  6de05adcbaf2e8b85fe86aad4169393098628119
tests/vlc/configure.sh /path/to/vlc-source "$vlc_build" \
  "$robotweax_prefix" "$vlc_prefix"
make -C "$vlc_build" -j4
make -C "$vlc_build" install
```

Use a fresh build directory and a dedicated writable VLC source checkout for
the compatibility adjustment below and bootstrap's generated build files.
The helper validates the selected `srt.pc`, disables
VLC's contrib prefix selection, and sets runtime paths for both installations.
It enables the SRT modules, TS demux/mux and libmpeg2 decoding. GUI, libavcodec
and libavformat integration are disabled in this qualification build; the
latter also avoids indirectly loading a second SRT provider through FFmpeg.

VLC's contrib recipes can otherwise supply Haivision SRT independently of
Robotweax. Do not replace a shared library underneath a prebuilt VLC plugin.

### Pinned VLC payload-option compatibility

At this VLC revision, the SRT input module registers `payload-size` as obsolete,
while the output module registers the same global option with a 1316-byte
default. In the x86-64 qualification, VLC selected the obsolete registration:
it ignored an explicit `--payload-size=1316` and passed a zero payload size to
the output module. The connection succeeded, but the module emitted no SRT
messages despite receiving MPEG-TS blocks from its muxer.

`configure.sh` runs `prepare_source.py` before bootstrap/configure. This removes
only the obsolete input registration, leaving the active output option as the
single owner. The preparation checks SHA-256 hashes of the original and
prepared `modules/access/srt.c`, is idempotent, and refuses unfamiliar or edited
source files. It modifies the dedicated VLC checkout, not Robotweax's transport
or public ABI. This adjustment is part of the qualified build; the unadjusted
VLC revision is not qualified for output by this guide. Revalidate it when
updating VLC instead of bypassing the source checks.

The test peer also sets `--payload-size=1316` explicitly. Both SRT modules remain
installed and discoverable; no plugin is hidden to avoid the conflict.

### Run the installed CLI

Run the installed CLI as a regular user, for example:

```sh
VLC_PLUGIN_PATH="$vlc_prefix/lib/vlc/plugins" \
  "$vlc_prefix/bin/vlc" --intf=dummy --ignore-config --no-plugins-cache \
  --no-audio --vout=dummy 'srt://127.0.0.1:9000?mode=listener'
```

This example decodes without displaying a window. Production builds can enable
their desired video output and codecs. VLC's `latency` is in milliseconds;
its options include `payload-size`, `key-length` (bytes), and `streamid`.
Do not assume FFmpeg's or GStreamer's complete URL option syntax applies.
The test driver supplies transport options at the libVLC instance level so
they also reach the stream-output module. Never put production passphrases
in diagnostic logs or public test fixtures.

## Automated qualification

The complete test build additionally needs Ninja, Meson 1.9.1,
`libglib2.0-dev`, and Python 3.10 or newer. It builds VLC and FFmpeg against
Robotweax, and GStreamer against pinned Haivision SRT 1.5.7. Use clean source
checkouts at the revisions recorded in `.github/workflows/ci.yml`:

```sh
tests/vlc/build_and_test.sh /path/to/vlc-source /path/to/gstreamer-source \
  /path/to/ffmpeg-source /path/to/haivision-srt-source \
  "$PWD/build-vlc-qualification"
```

The installed-API test driver uses libVLC video callbacks to hash the visible
I420 pixels of decoded synthetic video. Incoming live samples are checked
against a local decode of the same fixture. It also verifies outgoing MPEG-2
elementary payloads, cleartext/encrypted peers, Stream-ID and wrong-key
rejection with a subsequent valid connection, listener reconnection, and five
stop/restart cycles while waiting for a caller.

The reconnect case requires at least 15 distinct decoded frame hashes from a
different second video, so repeated old frames cannot count as recovery.
Frame hashes are written to separate `.frames` files to avoid interleaving
with libVLC's multithreaded diagnostics. The Linux gate has 15 required test
groups; a passing local run additionally confirms the package/ABI checks.

Runtime provider selection is checked through the installed VLC module's
`srt_startup` address, not through a direct SRT link in the test executable.
Logs also record the actual input/output modules chosen by libVLC. The
test rejects a second SRT library from another prefix in Linux process
mappings. The GStreamer peer independently verifies its Haivision provider.

These are bounded live-stream samples with controlled shutdown, not an EOF
or lossless reconnection protocol. Frame capture stops before player teardown;
decoder flush output during teardown is outside the capture window. The
reconnection test checks decoded media before and after the interruption, not
preservation of every frame during it. No audio, Rendezvous, IPv6, FEC, groups,
AES-GCM, impaired-network or long-duration support is inferred from these tests.

Test artifacts remain under `evidence/`. CI publishes logs and build diagnostics,
not VLC, Haivision, or codec binaries. A failed required case fails the VLC job
and the required CI gate.

## Upstream behavior and provenance

The integration was reviewed at the pinned VLC revision in `configure.ac`,
`contrib/src/srt/rules.mak`, `modules/access/srt.c`,
`modules/access/srt_common.h`, and `modules/access_output/srt.c`. Public
libVLC media/player headers define the test API. Relevant behaviors are
pkg-config discovery, the separate contrib provider, input/output role
differences, and interrupting an SRT epoll wait by removing its socket.
The payload-option investigation also inspected `src/libvlc-module.c`,
`src/stream_output/stream_output.c`, `modules/mux/mpeg/ts.c`, and
`modules/packetizer/mpegvideo.c` at the same revision. Local API tracing
confirmed that the muxer supplied TS blocks, the configured SRT payload size
was zero, and no SRT send occurred before the compatibility adjustment; with
the obsolete registration removed, the payload size was 1316 and sends succeeded.
The temporary tracing code is not part of the build or published artifacts.
GStreamer's `ext/srt/gstsrtelement.c` at the revision pinned in the
[GStreamer guide](gstreamer-integration.md) was checked for its `srtlib` log
category so wrong-key tests can retain the reference rejection reason.
No upstream transport implementation was copied into Robotweax.
