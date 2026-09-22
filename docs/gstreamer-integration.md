# GStreamer integration

Build GStreamer's existing `srt` plugin against Robotweax's shared C library
to use the standard `srtsrc`, `srtsink`, and `srt://` interfaces. This profile
uses unmodified GStreamer sources and the opt-in `srt.pc` metadata already
used by the [FFmpeg integration](ffmpeg-integration.md).

The current namespaced installation keeps `librobotweax-srt` and its headers
separate from Haivision's `libsrt`. The opt-in `srt.pc` selects those Robotweax
paths without renaming or replacing a system library.

The initial qualification covers IPv4 Live/Message streaming, AES-CTR,
Caller/Listener roles, and controlled application shutdown. Immediate sender
teardown is **not** a lossless end-of-stream mechanism; see the shutdown
boundary below before using this profile for finite media transfers.

## Reproducible build

The baseline is GStreamer **1.28.7**, commit
`070125524a8422e29d3b69a372ed4f62fd343ffa`. The CI workflow pins that commit,
the FFmpeg revision, and the Haivision SRT reference revision independently.
The Robotweax release and compatible SRT API version remain different
version axes; `srt.pc` exposes the latter.

Prerequisites are a C/C++ toolchain, CMake, OpenSSL 3, Python 3.10 or newer,
Meson 1.9.1, Ninja, pkg-config, GLib/GIO development files, Flex, and Bison
2.4 or newer. On Ubuntu, install the system dependencies with:

```sh
sudo apt-get install build-essential cmake ninja-build pkg-config \
  libglib2.0-dev libssl-dev flex bison python3-venv git
python3 -m venv /path/to/gst-tools
/path/to/gst-tools/bin/pip install meson==1.9.1
export PATH="/path/to/gst-tools/bin:$PATH"
```

On macOS, use the Homebrew versions of GLib, OpenSSL, Ninja and Bison. Apple's
bundled Bison is too old. Put `$(brew --prefix bison)/bin` on `PATH` for both
configuration and subsequent builds.

From the Robotweax repository, choose absolute paths for isolated prefixes:

```sh
robotweax_source="$PWD"
robotweax_prefix="$PWD/build-gst-stage/srt"
gst_prefix="$PWD/build-gst-stage/gstreamer"
gst_build="$PWD/build-gstreamer"

cmake -S . -B build-gst-srt \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON \
  -DROBOTWEAX_SRT_INSTALL_LAYOUT=namespaced \
  -DCMAKE_INSTALL_LIBDIR=lib \
  -DCMAKE_INSTALL_PREFIX="$robotweax_prefix" \
  -DROBOTWEAX_SRT_BUILD_BENCHMARKS=OFF \
  -DROBOTWEAX_SRT_BUILD_TOOLS=OFF \
  -DROBOTWEAX_SRT_INSTALL_LIBSRT_PKGCONFIG_COMPAT=ON
cmake --build build-gst-srt --parallel
cmake --install build-gst-srt

git clone --depth 1 --branch 1.28.7 \
  https://github.com/GStreamer/gstreamer.git /path/to/gstreamer-source
git -C /path/to/gstreamer-source rev-parse HEAD

tests/gstreamer/configure.sh /path/to/gstreamer-source \
  "$gst_build" "$robotweax_prefix" "$gst_prefix"
meson compile -C "$gst_build" -j 4
meson install -C "$gst_build" --no-rebuild
```

Confirm the reported Git revision matches the baseline above. The helper
requires a fresh build directory, verifies the selected `srt.pc`, disables
dependency-download fallbacks, and builds a minimal GStreamer installation
with the core elements, app elements, and SRT plugin. Production builds can
enable additional encoders, parsers, muxers, and device plugins.

The helper sets both isolated library prefixes as runtime search paths for
the C linker and, on macOS, the Objective-C linker used by GStreamer. This
also lets the installed Linux tools find their own GStreamer libraries.
Merely setting `PKG_CONFIG_PATH` does not establish runtime library paths.

## Select and verify the installed plugin

Use a separate registry and plugin search path:

```sh
export GST_PLUGIN_SYSTEM_PATH_1_0=""
export GST_PLUGIN_PATH_1_0="$gst_prefix/lib/gstreamer-1.0"
export GST_PLUGIN_SCANNER_1_0="$gst_prefix/libexec/gstreamer-1.0/gst-plugin-scanner"
export GST_REGISTRY_1_0="$gst_prefix/robotweax-registry.bin"

"$gst_prefix/bin/gst-inspect-1.0" srtsrc
"$gst_prefix/bin/gst-inspect-1.0" srtsink
```

Check the `Filename` in both reports. On Linux, inspect `libgstsrt.so` with
`ldd`; on macOS inspect `libgstsrt.dylib` with `otool -L` and its `LC_RPATH`
entries with `otool -l`. The qualification peer additionally resolves the
actual loaded `srt_startup` address with `dladdr` and fails if its library
directory differs from the requested provider. It does not link directly to
libsrt, so this check follows the plugin's own dependency.

Keep Robotweax and Haivision installations in different prefixes and
processes. Do not replace the library underneath a prebuilt distribution
plugin: rebuild against the selected provider's headers and library.

A receiver for an ongoing MPEG-TS stream can use:

```sh
"$gst_prefix/bin/gst-launch-1.0" \
  srtsrc uri="srt://127.0.0.1:9000?mode=listener" latency=80 \
  ! filesink location=received.ts
```

GStreamer's `latency` property uses milliseconds; FFmpeg's SRT `latency` URL
option uses microseconds. GStreamer's payload option is `payloadsize`, while
FFmpeg uses `payload_size`. Do not copy an entire FFmpeg query string into a
GStreamer pipeline without checking its option names and units.

## Qualification and shutdown boundary

Run the complete build and test procedure with clean, pinned source checkouts:

```sh
tests/gstreamer/build_and_test.sh /path/to/gstreamer-source \
  /path/to/ffmpeg-source /path/to/haivision-srt-source \
  "$PWD/build-gstreamer-qualification"
```

The script builds isolated Robotweax and Haivision GStreamer installations,
builds FFmpeg against Robotweax, and invokes the installed-API test driver.
It exercises:

- Robotweax to Robotweax and both directions with Haivision, with either
  source or sink listening, in cleartext and AES-CTR with a 128-bit key;
- byte-for-byte MPEG-TS comparison, including its final payload;
- FFmpeg to GStreamer and GStreamer to FFmpeg, checking the extracted MPEG-2
  elementary stream in cleartext and AES-CTR;
- nonzero GStreamer byte statistics;
- five start/stop cycles each for a source and a sink waiting for a peer;
- encrypted reconnection to a persistent listener, stream-ID rejection,
  wrong-passphrase rejection, and a valid connection after those failures.

The GStreamer-to-GStreamer tests keep the sender open until the receiver has
captured the complete expected byte sequence. FFmpeg-to-GStreamer captures a
known media prefix from a continuously looping sender, then stops that sender.
GStreamer-to-FFmpeg allows a documented 500-ms loopback drain interval at
80-ms negotiated latency before closing, and verifies the entire elementary
stream. These are explicit test shutdown policies, not delivery guarantees
for arbitrary networks.

In GStreamer 1.28.7, the receive adapter checks for `SRTS_BROKEN` before
attempting the next read, and the plugin disables SRT linger by default.
Immediate transition of a finite sender to `NULL` can therefore truncate
buffered input. This behavior was reproduced with both Robotweax 0.2.2 and
Haivision 1.5.7. Applications requiring lossless finite transfers need a
receiver-confirmed completion protocol before closing. A fixed sleep alone
is not such a protocol. This qualification does not claim lossless abrupt
disconnect, automatic caller reconnection, or preservation across a peer crash.

The relevant upstream integration behavior was inspected at the pinned
GStreamer revision above in
`subprojects/gst-plugins-bad/ext/srt/meson.build` and `gstsrtobject.c`.
No upstream transport implementation code was incorporated into Robotweax.

Logs and plugin descriptions are retained under the qualification directory's
`evidence/` folder. The GitHub job retains these diagnostics and both Meson
build logs as an artifact. The harness returns nonzero for any failed required
case and its result is included in the required CI gate.

IPv6, Rendezvous, FEC, connection groups, AES-GCM properties, impaired-network
and long-duration tests, and Windows packaging require separate qualification.
The baseline does not infer their GStreamer support from the core SRT tests.
