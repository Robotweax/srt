# srt-live-transmit with Robotweax SRT

This opt-in recipe builds Haivision's familiar `srt-live-transmit` application
against an **installed Robotweax SRT SDK** and adds `arqfirst=yes` to SRT output
URIs. The executable is named `srt-live-transmit-robotweax` so it can coexist
with a regular Haivision installation. The version banner identifies the
transport provider; SRT 1.5.7 is the compatibility version.

Use a current SDK containing the ARQ-first API and the listener capacity fix
from PR #376. The published Robotweax 0.3.1 SDK predates ARQ-first; its version
number alone does not establish availability. Configuration checks for the
required public API before building the application.

The regular library build does not fetch or build this application. The
recipe does not modify the library, change its CMake options or add an SDK
component. `robotweax_srt_udp_bridge` remains the independently authored bridge
for Robotweax-specific workflows; this integration provides the existing
application's URI-based interface.

## Build and test

Requirements: Git, Python 3.10+, CMake 3.20+, a C++20 compiler and OpenSSL 3+
development files. On Windows, use the same compiler architecture and OpenSSL
installation as the selected SDK. Run the following from the repository root:

```sh
./tools/python tests/live_transmit/build_and_test.py \
  --work ../srt-live-transmit-work
```

On Windows PowerShell, invoke `python tests/live_transmit/build_and_test.py`
with the same arguments instead of the POSIX `./tools/python` launcher.

The work directory must be outside the Robotweax checkout. This explicitly
requested command fetches the pinned application source, builds and installs
only the Robotweax library target into the work directory, builds the external
application and runs the functional checks. It does not run the Windows SDK
independent rebuild/release qualification.

To reuse an SDK already built and qualified, supply its installation prefix:

```sh
./tools/python tests/live_transmit/build_and_test.py \
  --work ../srt-live-transmit-app-work \
  --sdk-prefix /absolute/path/to/robotweax-sdk
```

This skips the library build. `--openssl-root /path/to/openssl` selects a
non-default OpenSSL installation. `--source /path/to/haivision-srt` reuses an
existing checkout at the exact pin below instead of fetching it. That checkout
must have the original LF bytes; when cloning on Windows, use
`git -c core.autocrlf=false clone ...` and disable line-ending conversion for
the checkout. `--jobs` controls build parallelism.

The executable's exact path is recorded in `app-build/app-path-Release.txt`.
For single-configuration generators it is normally
`app-build/srt-live-transmit-robotweax`; Visual Studio normally places the
`.exe` in `app-build/Release/`.

For a manual application-only build against an installed SDK:

```sh
cmake -S tests/live_transmit -B ../live-transmit-build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/absolute/path/to/robotweax-sdk \
  -DHAIVISION_SRT_SOURCE_DIR=/absolute/path/to/pinned-haivision-srt
cmake --build ../live-transmit-build --config Release --parallel 3
```

Preparation verifies every selected source file before generating an adapted
copy. Reusing unchanged inputs is idempotent. Modified source, modified
generated output or an updated adaptation requires a fresh application build
directory. The original upstream checkout stays unchanged.

## Try UDP → SRT → UDP

Use the generated executable path in place of `APP`. Start the receiver:

```sh
APP 'srt://127.0.0.1:9000?mode=listener&latency=120' \
    'udp://127.0.0.1:5001'
```

In another terminal, start the sender:

```sh
APP -v 'udp://127.0.0.1:5000' \
    'srt://127.0.0.1:9000?mode=caller&latency=120&arqfirst=yes'
```

Wait for `SRT target connected`, then feed UDP datagrams to port 5000.
The receiver forwards them to port 5001. For MPEG-TS, an existing FFmpeg source
and VLC player can supply and receive those datagrams. The application
transports bytes without transcoding. Stop both applications with Control-C.

The sender reads its policy back from the socket and reports:

```text
Robotweax ARQ-first=on tail-probe=off
```

`arqfirst=yes` supplies `fec,cols:8,rows:8,layout:even,arq:always` unless a
compatible `packetfilter` is explicitly configured. `arqfirst=no` selects the
always-on policy; to compare it with the same geometry, use:

```text
srt://127.0.0.1:9000?mode=caller&latency=120&arqfirst=no&packetfilter=fec,cols:8,rows:8,layout:even,arq:always
```

The option accepts `yes/no`, `on/off`, `true/false` and `1/0`. It is an SRT
output option. ARQ-first requires Live mode and an individual Caller/Listener
socket with TSBPD enabled and a compatible matrix filter. It enables no tail
probe. The application also checks the interpreted connection mode, including
Rendezvous selected indirectly through `adapter`.

For a Listener sender and Caller receiver, the receiving Caller must request
the matching packet filter. The supplied tests exercise this reversed role.
The application's bonding frontend is disabled (`ENABLE_BONDING=0`); use the
native [group tools](connection-groups.md) for group workflows. No broader
File, Rendezvous or application-feature qualification is implied by this Live
profile.

The adapted application's normal and verbose diagnostics omit URI query
values and redact passphrases and stream IDs in option output. Command-line
arguments can still be visible to local process inspection; use synthetic
credentials in shared demonstrations and review any logs before sharing.

## Qualification and CI

The harness sends 128 ordered, byte-identical 1,316-byte datagrams per case:

| Case | Configuration |
| --- | --- |
| Always-on control | Matrix 8×8, no injected loss |
| ARQ-first loss | Cleartext, eight original DATA packets dropped |
| ARQ-first encrypted loss | AES-256-CTR, eight original DATA packets dropped |
| Listener sender | Reversed Caller/Listener roles, eight originals dropped |

It checks read-back policy, tail probe off, exact delivery, proxy drop counts
and zero proxy queue overflow. Nine invalid configurations must fail promptly,
including invalid values, File and explicit/implicit Rendezvous activation,
disabled TSBPD, an incompatible row-only filter, group activation and input-side
options. The encrypted fixture uses a public synthetic passphrase and requires
that the application does not emit it in its logs. These are functional
loopback checks, not throughput or CPU benchmarks.

The dedicated CI matrix covers Linux, macOS and Windows and participates in
`Required CI gate`. Recipe changes select this small consumer job; production
library and package changes also select it. It remains separate from the SDK
release/signing workflows. CI retains only `build-identity.json` and
`runtime/results.json`, which record source identity, selected SDK library
hash, application hash and test results. Application binaries, upstream source,
prepared source, raw logs and build trees are not uploaded.

Local evidence for this recipe uses Apple Silicon macOS with OpenSSL 4.1.0.
Use the exact CI run's platform results when assessing Linux or Windows.

## Source and licensing boundary

Application baseline: Haivision SRT 1.5.7 at
`899348d8318eb9a3c5a5b6ec43c4a1114288773a`. The source checkout records the exact selected input files and SHA-256 hashes
in `tests/live_transmit/source-manifest.json`.
The upstream root `CMakeLists.txt` at that revision was also reviewed for
application build dependencies. The recipe compiles nine application
translation units and retains five upstream utility headers. It never builds
the Haivision transport or links Haivision `libsrt`.

The repository contains independently authored preparation/build/test scripts
and application support overlays. Upstream application and utility files are
fetched into a separate local work tree and retain their MPL-2.0 notices.
Adapted upstream files remain MPL-2.0; the independently authored overlays and
Robotweax transport retain MIT licensing. The prepared copy includes the
upstream license and a manifest of all generated file hashes.

This recipe creates local evaluation builds. There is no application install,
SDK packaging, signing, Homebrew formula or release-asset publication step.
The existing [third-party distribution boundary](../THIRD_PARTY.md) applies.
A separate distribution policy and the corresponding source/notice handling
must precede publishing application downloads.
