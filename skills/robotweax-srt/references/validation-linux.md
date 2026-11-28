# Local glibc Linux smoke validation

Validated on 2026-11-28 in a fresh Ubuntu 26.04.1 LTS arm64 container on the local Docker Desktop Linux VM. The guest kernel was `7.0.14-linuxkit`, Docker Engine was 29.9.3, and glibc was 2.43 (`libc6:arm64` `2.43-2ubuntu2.7`). The build used GCC 15.2.0 (`15.2.0-16ubuntu1`), Ninja 1.13.2 (`1.13.2-1`), CMake 4.2.3 (`4.2.3-2ubuntu2`), Python 3.14.4 (`python3.14` package `3.14.4-1ubuntu0.2`), and OpenSSL 3.5.5 (`libssl-dev` `3.5.5-1ubuntu3.7`). The smoke ran with Docker's external network disabled. The tested helper came from PR commit `d830cc8209a632e5469c8a3585f368aaad3e4693`.

## Exact builds

| Component | Build |
| --- | --- |
| Robotweax bridge and library | v0.3.5 at `fd9dac31675b12022579d28a748abb0839004cde`, Release, arm64, shared OpenSSL |
| Haivision peer and FFmpeg's linked SRT | Stock v1.5.7 at `899348d8318eb9a3c5a5b6ec43c4a1114288773a`, Release, arm64, shared OpenSSL |
| GStreamer peer | Ubuntu GStreamer 1.28.2, tools `1.28.2-1ubuntu0.1`, base `1.28.2-1ubuntu0.3`, good `1.28.2-2ubuntu0.4`, bad/SRT plugin `1.28.2-1ubuntu1.2`. The plugin loaded Ubuntu `libsrt1.5-gnutls:arm64` `1.5.4-3` with GnuTLS `3.8.12-2ubuntu1.1`. |
| FFmpeg peer | `3acec0a1af2dda0a0838689b8b8649e7deb080a0` (`git-2026-08-28-3acec0a`), raw-data muxer/demuxer, local stock Haivision v1.5.7 |
| Fixture generator and receiver decoder | Ubuntu FFmpeg 8.0.1, package `7:8.0.1-3ubuntu2` |

The `LD_DEBUG=libs` logs were filtered by the endpoint PID. The helper resolved each loaded SRT path and matched the bytes against its expected artifact, including GStreamer's plugin dependency. The packaged GStreamer peer and both local source-built peers have distinct matrix rows.

The local build image ID was `sha256:0c42a67fa274af212e36b679dd8e2f8cdde99270913ab7f42153f86c7a93228d`.

## Results

| Scenario | Result | Robotweax / peer exit |
| --- | --- | --- |
| Stock Haivision v1.5.7 | 8/8 passed | 0 / 0 |
| Ubuntu GStreamer 1.28.2 | 8/8 passed | 0 / 0 |
| Pinned FFmpeg with Haivision v1.5.7 | 8/8 passed | 0 / 255 (FFmpeg SIGINT exit) |
| GStreamer asked to match the Robotweax library | 8/8 rejected before media transfer | Helper exit 1 |

All 24 positive trials matched all 75 decoded MPEG-2 video frames and 125 decoded MP2 audio frames. Each peer ran the four Caller/Listener and send/receive combinations twice, after both endpoints closed and the same loopback ports could be reused. The profile was clear IPv4 Live/Message with 120 ms latency and 1316-byte MPEG-TS datagrams. FFmpeg used its raw data adapter as the SRT peer. The receiver decoded the media with the separate Ubuntu FFmpeg build. The FFmpeg capture hashes that differed from the fixture contained only extra MPEG-TS null packets from startup before the intact fixture. These trials cover endpoint reuse after process exit.

The negative check expected Robotweax bytes for GStreamer's loaded SRT library. The detected image was Ubuntu's Haivision-derived GnuTLS build, so all eight trials recorded a provider mismatch and produced no received media. The [compatibility matrix](compatibility-matrix.csv) records the individual sessions, hashes and exit statuses.

The review follow-up reran the corrected runner in a fresh container from the same local image with external networking disabled. All 24 A/V trials passed again. The verifier accepted the eight genuine provider mismatches and confirmed that they produced no received media. The setup-failure control used the minimal FFmpeg peer as the decoder. It exited 1 during fixture generation and produced zero trials, which the verifier rejected. The complete runner exited 0.

A later bind-diagnostic check reserved the selected SRT port with an independent loopback UDP socket before starting each listener. With Robotweax listening, the trial identified the bridge’s `srt_bind` error. With GStreamer listening, it identified the peer’s `Cannot bind` error. Both results included the selected loopback ports and the endpoint log line. The updated helper SHA-256 was `3933e0393876826bee23177a84fac9e1ea4e0affb9122ff08379486fa52abc4b`.

## Original run artifact hashes

```text
c7e0040ef86996b6cf94c3ecb45560accb8d72b643fe2c7f86772e06e49e4bab  /lab/skill/smoke_live.py
42cd00350153decec38e88747b3d98fe6db9eb8582e772f7f9bcee9e45e58fc0  /lab/skill/verify_artifact.py
917e4c147c260eef1563fd1b0579d6fbb59008fa5c62c0706096d5628a47fc58  /lab/rw-build/robotweax_srt_udp_bridge
1b5031fc61c54b589114f41d43e79b683dcef9cfec95d61ba3930d670f2ebc8f  /lab/rw-build/librobotweax-srt.so.0.3.5
cd7e1b17002e70244fc9dc993be864fb7ec2b9cb390378eac812db9b74147133  /usr/bin/gst-launch-1.0
d0df593064cb2fcfb32bba4d71ea8140d51cbb0050e58122c31cd34cedd6c62d  /usr/lib/aarch64-linux-gnu/gstreamer-1.0/libgstsrt.so
30809e52a305e43be311944bc95d784af5944d49d247182f7d06ec7160ec4210  /usr/lib/aarch64-linux-gnu/libsrt-gnutls.so.1.5
5e31ebca1bd2d10f507dd6b594c5953bdb9c5c8c61f3a8ee49681aec9622ada4  /lab/hv-build/srt-live-transmit
459b9e02526224b75a2570ea477456b70c3110b21af0f0e9f144de960124b1dc  /lab/hv-build/libsrt.so.1.5
cdc4330f39b46c364c6f341be09e35998c93db1e69fd1c82d29d13f33bf38860  /lab/ffmpeg-stock/ffmpeg
459b9e02526224b75a2570ea477456b70c3110b21af0f0e9f144de960124b1dc  /lab/hv-install/lib/libsrt.so.1.5
bb9604bbc4b83a1ad1d8fe1ca910f81531fcc6bd64210cf11c5c981d0f354aac  /usr/bin/ffmpeg
f7be64787ddc14ca9c371d926358eceb0e8f758872f01639d4a1504809672978  synthetic fixture.ts
```

## Reproduction

Run from the PR checkout. Create the Dockerfile and runner below inside the scratch directory.

```sh
lab=$(mktemp -d /tmp/robotweax-glibc.XXXXXXXX)
lab=$(cd "$lab" && pwd -P)
mkdir "$lab/robotweax" "$lab/skill" "$lab/evidence"
git archive fd9dac31675b12022579d28a748abb0839004cde | tar -x -C "$lab/robotweax"
cp skills/robotweax-srt/scripts/*.py "$lab/skill/"
```

`Dockerfile`

```dockerfile
FROM ubuntu@sha256:d66b366dac61b575bee2aef07e9c26e8070096fbe84a3d4d3eb0da069b5d76db
ENV DEBIAN_FRONTEND=noninteractive LC_ALL=C
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential ca-certificates cmake ninja-build pkg-config git libssl-dev \
    python3 ffmpeg gstreamer1.0-tools gstreamer1.0-plugins-base \
    gstreamer1.0-plugins-good gstreamer1.0-plugins-bad
WORKDIR /lab
COPY robotweax robotweax
COPY skill skill
RUN cmake -S robotweax -B rw-build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_SHARED_LIBS=ON -DROBOTWEAX_SRT_BUILD_EXAMPLES=ON \
      -DROBOTWEAX_SRT_BUILD_TESTS=OFF -DROBOTWEAX_SRT_BUILD_BENCHMARKS=OFF \
      -DROBOTWEAX_SRT_BUILD_TOOLS=OFF && \
    cmake --build rw-build --target robotweax_srt_udp_bridge --parallel 6
RUN git clone --depth 1 --branch v1.5.7 https://github.com/Haivision/srt.git haivision && \
    test "$(git -C haivision rev-parse HEAD)" = 899348d8318eb9a3c5a5b6ec43c4a1114288773a && \
    cmake -S haivision -B hv-build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=/lab/hv-install -DENABLE_STATIC=OFF && \
    cmake --build hv-build --parallel 6 && cmake --install hv-build
RUN git init ffmpeg-src && git -C ffmpeg-src fetch --depth 1 \
      https://github.com/FFmpeg/FFmpeg.git 3acec0a1af2dda0a0838689b8b8649e7deb080a0 && \
    git -C ffmpeg-src checkout --detach FETCH_HEAD && \
    test "$(git -C ffmpeg-src rev-parse HEAD)" = 3acec0a1af2dda0a0838689b8b8649e7deb080a0 && \
    mkdir ffmpeg-stock
WORKDIR /lab/ffmpeg-stock
RUN PKG_CONFIG_PATH=/lab/hv-install/lib/pkgconfig ../ffmpeg-src/configure \
      --disable-everything --disable-autodetect --disable-doc --disable-ffplay \
      --disable-ffprobe --disable-x86asm --enable-protocol=udp,libsrt,file \
      --enable-demuxer=data --enable-muxer=data --enable-libsrt \
      --extra-ldflags=-Wl,-rpath,/lab/hv-install/lib && make -j6 ffmpeg
WORKDIR /lab
```

`run-linux.sh`

```bash
#!/bin/bash
set -euo pipefail

pin=fd9dac31675b12022579d28a748abb0839004cde
reference=899348d8318eb9a3c5a5b6ec43c4a1114288773a
ffmpeg_pin=3acec0a1af2dda0a0838689b8b8649e7deb080a0
rw_library=/lab/rw-build/librobotweax-srt.so.0.3.5
plugin=$(gst-inspect-1.0 srt | awk '$1 == "Filename" { print $2 }')
gst_srt=$(ldd "$plugin" | awk '$1 ~ /^libsrt/ { print $3 }')
hv_srt=$(ldd /lab/hv-build/srt-live-transmit | awk '$1 ~ /^libsrt/ { print $3 }')
ff_srt=$(ldd /lab/ffmpeg-stock/ffmpeg | awk '$1 ~ /^libsrt/ { print $3 }')
gst_version=$(dpkg-query -W -f='${Version}' gstreamer1.0-plugins-bad)
gst_provider=$(dpkg-query -S "$(readlink -f "$gst_srt")" | sed 's/: \/.*//')
gst_provider_version=$(dpkg-query -W -f='${Version}' "$gst_provider")

{
  cat /etc/os-release
  uname -m
  uname -r
  getconf GNU_LIBC_VERSION
  gcc --version | sed -n '1p'
  cmake --version | sed -n '1p'
  python3 --version
  openssl version
  gst-launch-1.0 --version
  /usr/bin/ffmpeg -version | sed -n '1p'
  /lab/ffmpeg-stock/ffmpeg -version | sed -n '1p'
  dpkg-query -W libc6 gcc-15 cmake python3 libssl-dev ffmpeg gstreamer1.0-tools \
    gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-bad "$gst_provider"
  gst-inspect-1.0 srt
  ldd "$plugin"
  ldd /lab/rw-build/robotweax_srt_udp_bridge
  ldd /lab/hv-build/srt-live-transmit
  ldd /lab/ffmpeg-stock/ffmpeg
  sha256sum /lab/skill/*.py /lab/rw-build/robotweax_srt_udp_bridge "$rw_library" \
    /usr/bin/gst-launch-1.0 "$plugin" "$gst_srt" /lab/hv-build/srt-live-transmit "$hv_srt" \
    /lab/ffmpeg-stock/ffmpeg "$ff_srt" /usr/bin/ffmpeg
} > /evidence/manifest.txt

smoke() {
  python3 /lab/skill/smoke_live.py \
    --bridge /lab/rw-build/robotweax_srt_udp_bridge --robotweax-library "$rw_library" \
    --robotweax-build "v0.3.5:$pin:OpenSSL:shared" --decoder "${SMOKE_DECODER:-/usr/bin/ffmpeg}" "$@"
}

verify_provider_rejections() {
  local helper_exit=$1 evidence=$2
  test "$helper_exit" -eq 1 || return 1
  python3 - "$evidence" "$gst_srt" "$rw_library" <<'PYTHON'
import hashlib
import json
from pathlib import Path
import sys

evidence, actual, expected = map(Path, sys.argv[1:])
actual = actual.resolve(strict=True)
def digest(path):
    with path.open("rb") as f:
        return hashlib.file_digest(f, "sha256").hexdigest()

actual_hash, expected_hash = digest(actual), digest(expected)
mismatch = f"peer.log: loaded {actual} ({actual_hash}), expected {expected} ({expected_hash})"
rows = json.loads((evidence / "results.json").read_text())["results"]
expected_trials = {
    (role, direction, session)
    for role in ("caller", "listener")
    for direction in ("send", "receive")
    for session in (1, 2)
}
trials = {(row["robotweax_role"], row["robotweax_direction"], row["session"]) for row in rows}
if (
    actual_hash == expected_hash
    or len(rows) != len(expected_trials)
    or trials != expected_trials
    or any(row["status"] != "FAIL" or row.get("error") != mismatch for row in rows)
    or any(evidence.glob("*/received.ts"))
):
    sys.exit("Expected eight loader-confirmed provider mismatches and no received media")
print(f"{evidence.name}: verified {len(rows)} provider mismatches")
PYTHON
}

status=0
smoke --peer haivision --peer-bin /lab/hv-build/srt-live-transmit --peer-library "$hv_srt" \
  --peer-build "Haivision-v1.5.7:$reference:OpenSSL:shared" --output /evidence/haivision || status=1
smoke --peer gstreamer --peer-bin /usr/bin/gst-launch-1.0 --peer-library "$gst_srt" \
  --peer-build "Ubuntu:GStreamer-$gst_version:$gst_provider-$gst_provider_version" \
  --output /evidence/gstreamer || status=1
smoke --peer ffmpeg --peer-bin /lab/ffmpeg-stock/ffmpeg --peer-library "$ff_srt" \
  --peer-build "FFmpeg:$ffmpeg_pin:Haivision-v1.5.7:$reference:OpenSSL:shared" \
  --output /evidence/ffmpeg || status=1

negative_exit=0
smoke --peer gstreamer --peer-bin /usr/bin/gst-launch-1.0 --peer-library "$rw_library" \
  --peer-build expected-Robotweax-for-GStreamer --output /evidence/provider-negative || negative_exit=$?
verify_provider_rejections "$negative_exit" /evidence/provider-negative || status=1

# The minimal raw-data FFmpeg peer lacks the lavfi fixture generator.
setup_exit=0
SMOKE_DECODER=/lab/ffmpeg-stock/ffmpeg smoke --peer gstreamer \
  --peer-bin /usr/bin/gst-launch-1.0 --peer-library "$rw_library" \
  --peer-build expected-Robotweax-for-GStreamer --output /evidence/setup-negative \
  2>/evidence/setup-negative.stderr || setup_exit=$?
if test "$setup_exit" -eq 1 &&
   python3 -c 'import json, sys; sys.exit(json.load(open(sys.argv[1]))["results"] != [])' \
     /evidence/setup-negative/results.json &&
   ! verify_provider_rejections "$setup_exit" /evidence/setup-negative; then
  printf '%s\n' 'setup-negative: zero-trial fixture failure correctly rejected'
else
  status=1
fi
exit "$status"
```

```sh
docker build --no-cache --platform linux/arm64 --progress plain \
  -t robotweax-srt-glibc-smoke:local "$lab"

docker run --rm --platform linux/arm64 --network none \
  --mount "type=bind,source=$lab/evidence,target=/evidence" \
  --mount "type=bind,source=$lab/run-linux.sh,target=/lab/run-linux.sh,readonly" \
  robotweax-srt-glibc-smoke:local /bin/bash /lab/run-linux.sh
```

The image build fetches public Ubuntu packages and the two pinned upstream sources. The runner keeps socket traffic on the isolated loopback interface. The recorded package versions above describe the observed build. If the package mirror has advanced, compare the new package manifest and artifact hashes before treating the result as evidence for the same binaries.
