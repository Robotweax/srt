# Implementation validation

Local test date 2026-11-28. Host macOS 27.2.1 (26B131), arm64, AppleClang 21.0.0. CMake 4.4.4, Python 3.15.2. The implementation started from public main `75766e6b4ea8228bd5e4eefb69243cdce1ac55d2`. All Robotweax test builds used the v0.3.5 tag, `fd9dac31675b12022579d28a748abb0839004cde`. The final-tag release workflow concluded successfully at that SHA. The tag matrix records successful FFmpeg, GStreamer, VLC and OBS jobs with job-specific links. The qualification boundaries include decoded video only for the pinned Linux VLC input. The installed GStreamer 1.28.9 and locally built stock-FFmpeg peer have separate rows and providers. [v0.3.5 release][release] · [Final-tag CI][ci]

## Local results

The [compatibility matrix](compatibility-matrix.csv) contains the exact scope and per-trial results. The final smoke passed 8/8 for each of stock Haivision, installed GStreamer and pinned FFmpeg, 24 sessions total. Each receiver's 75 decoded MPEG-2 video frames and 125 decoded MP2 audio frames matched the fixture in order. All four role and direction pairs were tested twice, closing both endpoints and reusing the ports. Rejected run history includes early teardown ordering failures (GStreamer and Haivision) and early readiness/blocked input shutdown failures (FFmpeg). The final helper waits for connection before checking loaded images and signals both endpoints before waiting for process exit.

A separate in-process reconnect used the stock GStreamer Caller sender with one Robotweax receiver Listener process. With `--reconnect on --reconnect-delay-ms 100 --peer-idle-timeout-ms 1000`, the same listener process reopened its endpoint after the first peer exited and accepted a fresh peer. Both generations decoded 75/125 frames with matching hashes. The listener reported `STOPPED connections=2 retries=2` and exited 0. Reproduce by using the GStreamer smoke commands for Robotweax receive/Listener, adding the flags above to the bridge, and keeping its process and ports through two peer launches. Wait for `READY SRT listener` before each launch and wait for `CONNECTED` with the new generation before feeding the fixture. Decode and hash each independent receive capture. The bridge example reconnect behavior is scoped to the tagged example. [Bridge documentation][bridge]

The pinned File/Stream harness compared file hashes in both media directions over Caller/Listener and Rendezvous, including loss recovery, dynamic MAXBW in Rendezvous and zero-byte EOF checks. The local run skipped rollover, resilience and IPv6 Rendezvous suites. Two additional sender-as-Listener tests passed in both media directions with matching SHA-256 and zero-byte EOF. Four AES-CTR Live/Message byte-transfer baselines passed in both directions with explicit 128-bit keys and with default key length. The scheduler scorecard verified deterministic payloads on 1 and 8 connections at requested 2 shards. That scorecard uses independent UDP channels at the Caller and a shared channel at the Listener. Worker count was not independently observed, and these are functional results with no shard-sizing claim.

The loaded Homebrew SRT library failed the standalone comparison against the trusted Robotweax artifact. The smoke also failed all eight provider-negative trials when the expected GStreamer library was deliberately set to Robotweax. These were provider checks before fixture transfer. The required pinned-key production scenario was rejected by policy before network negotiation. The clear A/V lane ran independently.

## Artifact identities

| Local artifact | SHA-256 |
| --- | --- |
| Robotweax shared bridge | `b8f660124b4d74129ff343e2d10c6a20c5f48cabb7e2d84027175a58e9144d4b` |
| Robotweax shared library 0.3.5 / OpenSSL 3.6.5 | `2009061d4301889c4bb52d93c4f0dfb065fbec42e7f8bfefa5ec38ef092f791c` |
| Haivision build-tree srt-live-transmit | `6e60b9018eb49b656226c89801e4962f98f294119881ea4af4dd876e82ad5696` |
| Haivision build-tree 1.5.7 dylib / OpenSSL 4.1.0 | `c7425a3d16bbdf485c092d90d7c7082397dd1cf9287a1bafcd67e2f812fca611` |
| Haivision installed 1.5.7 dylib for FFmpeg / OpenSSL 4.1.0 | `2e0a1b1f952c8e185622b150354e61941b5ab5bba1522981fa9ecca3e30afb87` |
| Homebrew GStreamer 1.28.9 gst-launch-1.0 | `2cfc106584e002c7b1d418b0263cb3f3ac731093822b29ac004f4c4470f54b5f` |
| Homebrew 1.28.9 libgstsrt.dylib | `81448f75aeaca59b3aa096757601e81898ca55d29ad67a40cca9cd14494cc996` |
| Homebrew 1.5.7_1 libsrt dylib for GStreamer / OpenSSL 4.1.0 | `e8f0949c8eaabdeaae1bd68ad94dde251eba0ff443e8b978a3b6099c5c708152` |
| Pinned FFmpeg raw-data SRT peer | `50a8fcac3373e1d66941eabbda9fad132d8a6417990dd653462752a40d3f5ffd` |
| Homebrew FFmpeg 9.0.2_1 media generator and decoder | `7f6028378534aa10a2dfa07d807ddcc0d18c869bf5c2f64c89efcc18ee88c4ee` |
| Robotweax static archive from the File/Stream peer's final link command | `0246db68cbba9d81233ab8d90b390f4aacce6d87946fb1e3b1450bf168c5a6e2` |
| Robotweax static interop peer | `0d62011835e620357e74cee44174cb6898730079555731fe44c29d2e9eba2107` |
| Haivision interop peer | `5c8b6b5c908b09da3005f548b8782ac198a4df2135c3bfc7ba8e3d0bec148399` |
| Generated synthetic fixture | `f7be64787ddc14ca9c371d926358eceb0e8f758872f01639d4a1504809672978` |

## Reproduce the local runs

Start in a fresh scratch directory with the skill path exported as `SKILL` and `ROBOTWEAX_REPO` pointing to a clone containing v0.3.5. This is the macOS arm64 procedure. Capture a new artifact manifest on other hosts.

```sh
pin=fd9dac31675b12022579d28a748abb0839004cde
reference=899348d8318eb9a3c5a5b6ec43c4a1114288773a
ffmpeg_pin=3acec0a1af2dda0a0838689b8b8649e7deb080a0
test "$(git -C "$ROBOTWEAX_REPO" rev-parse 'v0.3.5^{}')" = "$pin"
mkdir robotweax
git -C "$ROBOTWEAX_REPO" archive "$pin" | tar -x -C robotweax
cmake -S robotweax -B rw-build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$PWD/rw-install" -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)" \
  -DBUILD_SHARED_LIBS=ON -DROBOTWEAX_SRT_BUILD_EXAMPLES=ON \
  -DROBOTWEAX_SRT_BUILD_TESTS=OFF -DROBOTWEAX_SRT_BUILD_BENCHMARKS=OFF \
  -DROBOTWEAX_SRT_BUILD_TOOLS=OFF
cmake --build rw-build --parallel 8
cmake --install rw-build

git clone --depth 1 --branch v1.5.7 https://github.com/Haivision/srt.git haivision
test "$(git -C haivision rev-parse HEAD)" = "$reference"
cmake -S haivision -B haivision-build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$PWD/haivision-install" \
  -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)" -DENABLE_STATIC=OFF
cmake --build haivision-build --parallel 6
cmake --install haivision-build
otool -L haivision-build/libsrt.1.5.dylib

git init ffmpeg
git -C ffmpeg fetch --depth 1 https://github.com/FFmpeg/FFmpeg.git "$ffmpeg_pin"
git -C ffmpeg checkout FETCH_HEAD
mkdir ffmpeg-stock-smoke
(
  cd ffmpeg-stock-smoke
  PKG_CONFIG_LIBDIR="$PWD/../haivision-install/lib/pkgconfig:$(brew --prefix openssl@4)/lib/pkgconfig" \
    ../ffmpeg/configure --disable-everything --disable-autodetect --disable-doc \
    --disable-ffplay --disable-ffprobe --disable-x86asm --enable-protocol=udp,libsrt,file \
    --enable-demuxer=data --enable-muxer=data --enable-libsrt \
    --extra-ldflags="-Wl,-rpath,$PWD/../haivision-install/lib"
  make -j8 ffmpeg
)

gst-inspect-1.0 srtsrc
gst-inspect-1.0 srtsink
gst_plugin="$(gst-inspect-1.0 srtsrc | awk '$1=="Filename"{print $2; exit}')"
otool -L "$gst_plugin"
python3 "$SKILL/scripts/verify_artifact.py" \
  --observed /opt/homebrew/opt/srt/lib/libsrt.1.5.dylib \
  --expected rw-build/librobotweax-srt.0.3.5.dylib
# Expected exit 1 and match=false when auditing GStreamer for Robotweax.

smoke() {
  python3 "$SKILL/scripts/smoke_live.py" \
    --bridge rw-build/robotweax_srt_udp_bridge --robotweax-library rw-build/librobotweax-srt.0.3.5.dylib \
    --robotweax-build "v0.3.5:$pin:OpenSSL:shared" --decoder /opt/homebrew/bin/ffmpeg "$@"
}
smoke --peer haivision --peer-bin haivision-build/srt-live-transmit \
  --peer-library haivision-build/libsrt.1.5.dylib \
  --peer-build "Haivision-v1.5.7:$reference" --output haivision-final
smoke --peer gstreamer --peer-bin /opt/homebrew/bin/gst-launch-1.0 \
  --peer-library /opt/homebrew/opt/srt/lib/libsrt.1.5.dylib \
  --peer-build Homebrew-GStreamer-1.28.9:Haivision-1.5.7 --output gstreamer-final
smoke --peer ffmpeg --peer-bin ffmpeg-stock-smoke/ffmpeg \
  --peer-library haivision-install/lib/libsrt.1.5.dylib \
  --peer-build "FFmpeg:$ffmpeg_pin:Haivision-v1.5.7" --output ffmpeg-final

cmake -S robotweax -B rw-static -DCMAKE_BUILD_TYPE=Release \
  -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)" -DBUILD_SHARED_LIBS=OFF \
  -DROBOTWEAX_SRT_BUILD_TESTS=OFF -DROBOTWEAX_SRT_BUILD_BENCHMARKS=ON -DROBOTWEAX_SRT_BUILD_TOOLS=ON
cmake --build rw-static --target robotweax_srt_interop_peer robotweax_srt_scalability_peer --parallel 8
c++ -std=c++17 -O2 -DROBOTWEAX_SRT_REFERENCE_DIAGNOSTICS=1 robotweax/interop/api_peer.cpp \
  -Ihaivision-install/include/srt -Lhaivision-install/lib -lsrt \
  -Wl,-rpath,"$PWD/haivision-install/lib" -o reference-api-peer
python3 robotweax/interop/run_file_interop.py \
  --robotweax-peer "$PWD/rw-static/robotweax_srt_interop_peer" \
  --reference-peer "$PWD/reference-api-peer" --expected-reference-version 1.5.7 \
  --bytes 262144 --skip-rollover --skip-resilience --skip-ipv6-rendezvous >file.log 2>&1
python3 robotweax/interop/run_encrypted_interop.py \
  --robotweax-peer "$PWD/rw-static/robotweax_srt_interop_peer" \
  --reference-peer "$PWD/reference-api-peer" --bytes 262144 --baseline-only >encrypted-baseline.log 2>&1
python3 robotweax/benchmarks/scalability_scorecard.py \
  --topology many-socket --profile robotweax-self \
  --robotweax-peer "$PWD/rw-static/robotweax_srt_scalability_peer" --robotweax-revision "$pin" \
  --scheduler-shards 2 --connections 1,8 --message-sizes 1316 --warmup 0 --iterations 1 \
  --output shard-control.json
```

For the File/Stream role reversal, generate a 262144-byte file using `random.Random(36).randbytes(262144)`. Start each pinned peer in turn as the sender with `listener --data-role sender --input fixture.bin` and use the other as `caller --data-role receiver --output received.bin --expect-eof`. Apply `--host 127.0.0.1 --port PORT --bytes 262144 --transport file --message-api false --max-bw 20000000 --timeout-ms 5000 --shutdown-grace-ms 250` to both. Verify both exit statuses, the EOF event and SHA-256. The local received digest for both rows was `7ac929dc030b4e6a90fe0ebec61765feec5640357df6393f0109178c365f8c7a`.

The Haivision build resolved OpenSSL via its default pkg-config discovery, which selected OpenSSL 4.1.0 despite the CMake root hint. Review build logs and loader data for the actual backend. Loader evidence distinguished the build-tree and installed copies of the same upstream release. Homebrew FFmpeg lacks SRT support and was used only for generation and decode. The locally compiled, pinned FFmpeg peer used its raw data muxer and demuxer with stock Haivision SRT.

## Artifact-only installation acceptance

Codex CLI 0.152.0 (GPT-5.6 Luna) and Claude Code 2.1.322 (Claude Haiku 5.5) each ran in a clean external Git repository at sample commit `4ace425d397b4a77898c1fb2b8bda41d6b338e45`. Each repo contained a tiny CMake target for `gst-inspect-1.0 srtsrc`, the deployment brief, relative fixture paths in `lab.env`, and an artifact manifest. There was no Robotweax source in either acceptance project. The only supplied Robotweax files were the tagged shared bridge and dylib. The bridge copy used `@loader_path` with an ad hoc signature for relocation and had SHA-256 `555926394d4f18e4128012f5ffa86978891e9158e697eb22a6e627b09936e071`. The library hash stayed the same.

Both agents invoked the canonical skill from their project's discovery directory. Each independently found the installed Haivision provider, passed all eight clear mixed-vendor sessions with 75/125 decoded frames per session, rejected REQUIRED peer identity against unmodified stock Haivision 1.5.7 without a fallback, and distinguished the shared-listener default from the 1/2/4/8 comparison for independent channels. Both left tracked files untouched. Earlier Codex attempts in a restricted macOS shell could not start GStreamer reliably and timed out with macOS service errors. The successful acceptance used an ordinary host shell inside the disposable sample project.

To reproduce, create a scratch Git project without library source, copy the skill to the documented discovery path, and stage only the build artifacts in `fixtures/`. On macOS for a build-tree bridge, replace its build-directory rpath with `@loader_path` in the copy (use `otool -l` to find the exact old rpath), re-sign the copy ad hoc, and place the matching ABI-named dylib symlink beside it. This fixture hashes the resulting executables and records the tag, platform, crypto backend and build flags in `fixtures/provenance.txt`. Use the fixture copies under `./fixtures/` as `BRIDGE` and `ROBOTWEAX_LIBRARY` in `lab.env`, and record paths for the installed `GST_LAUNCH`, `GST_INSPECT`, `BREW_SRT_LIBRARY` and `DECODER`.

The deployment brief requests Robotweax inside the installed GStreamer, a clear synthetic A/V lane against that GStreamer as a peer, and REQUIRED Robotweax pinned-key identity against unmodified stock Haivision for production. It compares a shared listener with 64 accepted sockets to eight independent UDP listeners at the same offered load and four CPUs of quota, with no measured capacity.

Save this prompt to `prompt.txt`:

> Use the installed project skill for the StudioRelay fixture. Inspect deployment.md, lab.env and fixtures/provenance.txt and verify that this repo contains no Robotweax source. Check the installed GStreamer provider, run the skill's documented helper for clear mixed-vendor A/V, and decide the separate REQUIRED peer-auth case and both shard topologies. Use ./acceptance as the helper's new output directory and let the helper create it. Use only the project files, supplied artifacts, and loopback. Do not seek another Robotweax checkout or modify tracked files. Give the evidence path and counts.

Run these in their respective sample checkouts. Compare `acceptance/results.json` to the expected clear matrix and keep the CLI transcript with it.

```sh
{ printf '%s\n' '$robotweax-srt'; cat prompt.txt; } |
  codex -a never exec -m gpt-5.6-luna -C "$(pwd -P)" \
    -c "projects.\"$(pwd -P)\".trust_level=\"trusted\"" -c 'model_reasoning_effort="low"' \
    --sandbox danger-full-access --ephemeral --json >codex.jsonl

claude -p --model haiku --permission-mode dontAsk \
  --allowedTools 'Read,Grep,Glob,Bash,Skill' --tools 'Read,Grep,Glob,Bash,Skill' \
  --setting-sources project --output-format stream-json --verbose \
  "$(printf '%s\n' '/robotweax-srt'; cat prompt.txt)" >claude.jsonl
```

## Remaining qualification

| Owner | Open evidence |
| --- | --- |
| Robotweax maintainers | Review with internal lesson owners and incorporate material cleared for public use. This implementation uses public evidence. |
| Customer integration owner | Qualify actual OBS, VLC and commercial devices and versions when available, plus the production FFmpeg/GStreamer package and exact codec, encryption and network profile. Local unavailable cases remain NOT RUN. |
| Customer security owner and Robotweax security maintainer | Resolve required identity with a compatible peer and suitable security qualification, or defer. Evaluate FEC, groups, GCM and peer authentication as separate target profiles when required. |
| Customer performance owner | Quantify shard count on target workloads with per-shard CPU, latency, loss and receiver goodput, and verify shared-link Live/File priority. |
| Skill maintainer | Run the new helper on glibc Linux and collect customer-facing lifecycle results. Existing Linux final-tag jobs use the release's own integration harnesses. |

[release]: https://github.com/Robotweax/srt/releases/tag/v0.3.5
[ci]: https://github.com/Robotweax/srt/actions/runs/45266706013
[bridge]: https://github.com/Robotweax/srt/blob/v0.3.5/docs/udp-srt-bridge.md
