# Robotweax SRT

Robotweax SRT is an independent implementation of Secure Reliable Transport
(SRT). It provides the familiar public SRT C API, reliable low-latency UDP
transport, encryption, rendezvous connections, File/Stream mode, packet
filtering, and connection groups in a portable C++20 library.

The project favors explicit compatibility boundaries, bounded protocol state,
and reproducible tests. It is suitable for developers who want to embed SRT,
build transport tools, or evaluate an alternative implementation without
depending on implementation-specific C++ internals.

## Release status

Version **0.2.4** is a pre-1.0 maintenance release with documented
[qualification limits](docs/release-notes-0.2.4.md#qualification-limits-and-release-acceptance),
including outstanding independent cryptographic review.

| Axis | Robotweax SRT 0.2.4 |
| --- | --- |
| Project release | `0.2.4` |
| Shared-library ABI | `0.2` |
| Default public API target | SRT `1.5.7` |
| `srt_getversion()` | `1.5.7` |
| Supported handshake generation | HSv5 |
| Default encryption | AES-CTR |
| Optional encryption extension | AES-GCM, explicit build-time opt-in |

The project version, shared-library ABI, compatible API version, wire
handshake, and encryption profile are separate version axes. In particular,
`srt_getversion()` reports the compatible SRT API target, not the Robotweax
release number.

Version 0.2 is a pre-1.0 release. Rebuild applications and dependencies when
moving between ABI lines. See the [0.2.4 release notes](docs/release-notes-0.2.4.md)
and [0.1-to-0.2 migration guide](docs/migration-0.2.md).

## Highlights

- IPv4 and IPv6 Caller/Listener and Rendezvous connections over HSv5;
- Message and Stream I/O through `<srt/srt.h>`;
- blocking, nonblocking, timeout, callback, and epoll-style operation;
- LiveCC and FileCC congestion control;
- AES-CTR with 128-, 192-, or 256-bit keys and runtime key rotation;
- released, opt-in AES-GCM extension with authenticated payloads;
- Row, Column, and Matrix packet-filter FEC;
- Broadcast and Backup connection groups for supported Live use cases;
- Stream ID, socket-option, statistics, logging, and file-helper APIs;
- static and shared libraries on Linux, macOS, and Windows;
- CMake package and pkg-config metadata for installed consumers.

Robotweax implements a substantial SRT 1.5.7-compatible surface, but it does
not claim to be a universal drop-in replacement for every `libsrt` deployment.
Unsupported features and combinations fail explicitly. Review
[known limitations](docs/limitations.md) before deployment.

The current reference profile is the immutable official Haivision SRT v1.5.7
tag at commit `899348d8318eb9a3c5a5b6ec43c4a1114288773a`. Compatibility claims
remain feature- and topology-specific; older 1.5.5 and 1.5.6 references are
retained as focused backward/security lanes.

## Quick start

Requirements:

- CMake 3.20 or newer;
- a C++20 compiler;
- OpenSSL 3.x development files;
- Python 3.10 or newer when building the test suite.

Configure, build, and test an out-of-source Release tree:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

For multi-config generators such as Visual Studio, omit
`CMAKE_BUILD_TYPE` and select the configuration when building and testing:

```sh
cmake -S . -B build
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

The default static development build includes tests, protocol tools, and
microbenchmarks. For a lean installable build:

```sh
cmake -S . -B build-install \
  -DCMAKE_BUILD_TYPE=Release \
  -DROBOTWEAX_SRT_BUILD_TESTS=OFF \
  -DROBOTWEAX_SRT_BUILD_BENCHMARKS=OFF \
  -DROBOTWEAX_SRT_BUILD_TOOLS=OFF
cmake --build build-install --parallel
cmake --install build-install --prefix "$PWD/stage"
```

See [Building and installing](docs/building.md) for shared libraries, build
options, platform notes, and troubleshooting.

## Use from CMake

After installation, consume the exported package and stable public target:

```cmake
find_package(RobotweaxSRT 0.2 CONFIG REQUIRED)

add_executable(my_srt_app main.cpp)
target_link_libraries(my_srt_app PRIVATE RobotweaxSRT::srt)
```

If Robotweax SRT is installed under a custom prefix, add that prefix to
`CMAKE_PREFIX_PATH`. POSIX consumers may alternatively use:

```sh
pkg-config --cflags --libs robotweax-srt
```

The default library name is `robotweax-srt` (`librobotweax-srt` on Unix-like
systems, `robotweax-srt.dll` for Windows shared builds). Headers live below
`include/robotweax-srt`; package metadata supplies that include root, preserving
`#include <srt/srt.h>`. See [installation layouts](docs/building.md#installation-layouts)
for the optional legacy layout and migration guidance.

## Public API

Prefer the installed compatibility header:

```c
#include <srt/srt.h>
```

The flat `<srt.h>` include is also installed for source compatibility. Start
the process runtime before creating sockets and release it after all sockets
have closed:

```c
if (srt_startup() == SRT_ERROR) {
    /* inspect srt_getlasterror_str() */
}

SRTSOCKET socket = srt_create_socket();
/* Configure, bind/connect, and transfer data. */

(void)srt_close(socket);
(void)srt_cleanup();
```

Configure connection-stage options before `srt_bind`, `srt_listen`, or
`srt_connect`. Select Message or Stream semantics deliberately and use
`srt_sendmsg2`/`srt_recvmsg2` when message-control metadata is required.

Read the [Integration guide](docs/integration.md),
[public API compatibility](docs/api-compatibility.md), and
[socket-option reference](docs/socket-options.md) before porting an existing
application.

For a continuous MPEG-TS UDP → SRT → UDP relay, start with the
[live UDP/SRT bridge demo](docs/udp-srt-bridge.md).

For runnable public-API examples, build the opt-in
[`srt_message_demo`, `srt_file_demo`, and `srt_group_demo`](examples/README.md),
or copy the
[standalone installed-package consumer](examples/installed-consumer/README.md)
to validate a downstream CMake integration.
The three networked demos demonstrate Caller, Listener, and Rendezvous
operation; Message and Stream I/O; Stream ID; nonblocking epoll usage;
`srt_sendfile`/`srt_recvfile`; Broadcast and weighted Backup groups with a
controlled member failover; AES-CTR or opt-in AES-GCM; packet-filter
configuration; and `srt_bistats` without depending on private implementation
headers.

## Encryption profiles

The default build provides compatible AES-CTR behavior. Configure
`SRTO_PASSPHRASE`, `SRTO_PBKEYLEN`, and related rotation options before
connection establishment.

Robotweax SRT 0.2.0 also releases an explicit AES-GCM extension. It is enabled
with the historical build flag `ENABLE_AEAD_API_PREVIEW`:

```sh
cmake -S . -B build-gcm \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_AEAD_API_PREVIEW=ON
cmake --build build-gcm --parallel
ctest --test-dir build-gcm --output-on-failure
```

The flag name is retained for source and build compatibility; AES-GCM is a
released, version-pinned Robotweax 0.2 extension. A GCM-enabled installed CMake
target propagates the required compile definition to consumers. Do not mix
headers or binaries from default and GCM-enabled builds.

See [Encryption and key rotation](docs/encryption.md) for configuration,
negotiation, payload budgets, and supported combinations.

## Documentation

Start with the [documentation index](docs/README.md):

- [Getting started](docs/getting-started.md)
- [Building and installing](docs/building.md)
- [Integration guide](docs/integration.md)
- [FFmpeg integration](docs/ffmpeg-integration.md)
- [Testing](docs/testing.md)
- [Known limitations](docs/limitations.md)
- [File/Stream mode](docs/file-mode.md)
- [Rendezvous](docs/rendezvous.md)
- [Connection groups](docs/connection-groups.md)
- [Packet-filter FEC](docs/packet-filter.md)
- [Live timing](docs/live-timing.md)
- [Statistics](docs/statistics.md)
- [Logging](docs/logging.md)

## Platform scope

The supported build matrix covers current Linux, macOS, and Windows toolchains.
Network behavior is designed to be portable, but some native socket options
are platform-specific. For example, interface binding is Linux-only and
explicit IPv6 traffic-class setting is unavailable through Winsock. The
library returns a defined unsupported-operation error rather than reporting a
setting that was not applied.

Application owners remain responsible for topology, reconnection policy,
credential handling, operating-system tuning, and production qualification on
their own networks and workloads.

## Contributing and security

Bug reports and focused changes are welcome. Please read
[CONTRIBUTING.md](CONTRIBUTING.md) before opening a pull request. Report
security issues through the process in [SECURITY.md](SECURITY.md), not a public
issue.

Third-party attributions are listed in [THIRD_PARTY.md](THIRD_PARTY.md).
Robotweax SRT is distributed under the terms in [LICENSE](LICENSE).
