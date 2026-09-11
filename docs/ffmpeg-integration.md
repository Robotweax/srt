# FFmpeg integration

Robotweax SRT can provide the existing FFmpeg `srt://` protocol with an
independent SRT transport implementation. FFmpeg itself does not need a new
protocol adapter or a Robotweax-specific URL scheme. The integration uses
FFmpeg's existing `libavformat/libsrt.c` source against Robotweax's installed
`<srt/srt.h>` C API and shared `srt` library.

## Integration boundary

FFmpeg enables its SRT protocol by finding a pkg-config module named `srt`
with a compatible API version and the `srt_socket` symbol. Robotweax's
canonical metadata is named `robotweax-srt.pc` and reports the Robotweax
project version. These are intentionally different version axes.

An explicit install option adds a second metadata file for consumers that
require the conventional libsrt package identity:

| Metadata | Version field | Purpose |
| --- | --- | --- |
| `robotweax-srt.pc` | Robotweax release, currently `0.2.4` | Canonical Robotweax package discovery |
| `srt.pc` | Compatible SRT API, currently `1.5.7` | Opt-in FFmpeg/libsrt-compatible discovery |

The compatibility metadata also exposes `robotweax_release` and
`robotweax_abi` pkg-config variables. Do not use its `Version` field to select
a Robotweax release.

## Build and install Robotweax

The initial FFmpeg integration supports a shared Robotweax library. Configure
an isolated install tree and explicitly enable the compatibility metadata:

```sh
cmake -S . -B build-ffmpeg-srt \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=ON \
  -DROBOTWEAX_SRT_BUILD_BENCHMARKS=OFF \
  -DROBOTWEAX_SRT_BUILD_TOOLS=OFF \
  -DROBOTWEAX_SRT_INSTALL_LIBSRT_PKGCONFIG_COMPAT=ON
cmake --build build-ffmpeg-srt --parallel
cmake --install build-ffmpeg-srt --prefix "$PWD/ffmpeg-srt-stage"
```

The option is off by default. It currently fails configuration for a static
Robotweax build because FFmpeg's C-only configure probe cannot portably infer
the C++ runtime required by the static Robotweax archive.

Install the compatibility file only into an isolated prefix. An upstream
Haivision SRT installation may provide its own `srt.pc` and `libsrt`; mixing
both providers in one prefix makes dependency and runtime selection
ambiguous.

## Configure FFmpeg

For a normal FFmpeg build, point pkg-config at the isolated installation and
enable the existing libsrt protocol:

```sh
robotweax_source="$PWD"
robotweax_prefix="$robotweax_source/ffmpeg-srt-stage"
export PKG_CONFIG_PATH="$robotweax_prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

cd /path/to/ffmpeg
./configure \
  --enable-libsrt \
  --extra-ldflags="-Wl,-rpath,$robotweax_prefix/lib"
make -j2
```

Library directories may differ on non-default toolchains. Use the actual
install location rather than assuming `lib`. The repository helper configures
a deliberately minimal FFmpeg build for the automated gate:

```sh
"$robotweax_source/tests/ffmpeg/configure.sh" \
  /path/to/ffmpeg "$robotweax_prefix"
make -C /path/to/ffmpeg -j2 ffmpeg
"$robotweax_source/tests/ffmpeg/run_smoke.sh" /path/to/ffmpeg/ffmpeg
```

The helper disables assembly and unrelated FFmpeg components so the gate
tests the integration contract rather than the host's optional multimedia
dependencies. Production FFmpeg builds may enable their normal codecs,
formats, filters, and assembly paths.

## Verify provider selection

After building FFmpeg, verify all three layers:

1. `pkg-config --modversion srt` reports `1.5.7` from the isolated prefix.
2. `ffmpeg -protocols` lists `srt` as an input and output protocol.
3. `ldd ffmpeg`, `otool -L ffmpeg`, or the Windows import table resolves the
   SRT dependency to the intended Robotweax installation.

Do not replace the SRT shared library underneath a prebuilt FFmpeg binary.
Robotweax 0.2 uses its own shared-library ABI and SONAME line. Rebuild FFmpeg
against the matching Robotweax headers and library.

## Qualified MVP profile

The automated smoke gate exercises:

- FFmpeg caller and listener endpoints over IPv4 loopback;
- Live/Message transport with a 1,316-byte MPEG-TS payload setting;
- nonblocking SRT epoll readiness as used by libavformat;
- an unencrypted transfer;
- an AES-CTR transfer with a 128-bit key; and
- byte-for-byte comparison of the extracted MPEG-2 elementary stream.

The gate pins the FFmpeg source revision in `.github/workflows/ci.yml`. Update
that pin deliberately and let the complete gate pass before claiming support
for a newer FFmpeg revision.

This smoke profile is a regression boundary, not production qualification.
Deployment testing must still cover the actual FFmpeg build, peer
implementation, operating system, address family, latency, bitrate, loss,
reordering, reconnect behavior, and run duration.

The smoke harness retains its temporary directory on failure, including the
generated MPEG-TS fixture, received streams, elementary payloads, and process
logs. CI uploads only these logs and generated media as `ffmpeg-smoke-failure`
for seven days; no provider or FFmpeg binaries are included. Set
`ROBOTWEAX_FFMPEG_KEEP_ARTIFACTS=1` to retain successful local runs as well.

## FFmpeg URL examples

Listener input:

```sh
ffmpeg -i "srt://0.0.0.0:9000?mode=listener&latency=120000" \
  -c copy received.ts
```

Caller output:

```sh
ffmpeg -re -i input.ts -c copy -f mpegts \
  "srt://receiver.example:9000?mode=caller&latency=120000&payload_size=1316"
```

FFmpeg accepts additional documented SRT URL options such as `streamid`,
`passphrase`, `pbkeylen`, buffer sizes, and timeouts. Robotweax rejects
unsupported values rather than silently approximating them. Review
[Socket options](socket-options.md) and [Known limitations](limitations.md)
before exposing arbitrary URL options to users.

Passphrases placed in command-line URLs can be visible to process inspection,
shell history, and logs. Production applications embedding libavformat should
prefer protected configuration and must redact URLs before logging them.

## Future profiles

Qualify these separately instead of inferring them from the Live/MPEG-TS gate:

- IPv6 and dual-stack wildcard listeners;
- Rendezvous mode;
- File/Stream transport;
- packet-filter FEC;
- connection groups;
- long-duration and impaired-network tests; and
- the opt-in Robotweax AES-GCM extension.

The default FFmpeg SRT adapter has no Robotweax AES-GCM AVOption. Adding that
surface would require an explicit, versioned FFmpeg change and is outside the
initial drop-in integration.
