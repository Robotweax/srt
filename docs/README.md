# Robotweax SRT documentation

This directory contains the public documentation for building, integrating,
operating, and evaluating Robotweax SRT 0.2.3. It is written for application
developers and transport integrators using the installed public API.

## Start here

- [Getting started](getting-started.md) — prerequisites, first build, and the
  basic socket lifecycle.
- [Building and installing](building.md) — build profiles, CMake options,
  installation, package discovery, and platform notes.
- [Integration guide](integration.md) — public headers, endpoint roles, I/O,
  readiness, encryption, errors, and shutdown.
- [FFmpeg integration](ffmpeg-integration.md) — opt-in package discovery,
  reproducible builds, runtime verification, and the qualified media profile.
- [Testing](testing.md) — local verification and focused test selection.
- [Public API examples](../examples/README.md) — runnable Caller, Listener,
  Rendezvous, and Connection Group demonstrations for Message, File/Stream,
  Broadcast, and weighted Backup APIs.
- [Known limitations](limitations.md) — compatibility, protocol, platform, and
  production boundaries.

## Public API and configuration

- [Public SRT API compatibility](api-compatibility.md)
- [Socket options](socket-options.md)
- [Connect variants](connect-variants.md)
- [Statistics](statistics.md)
- [Logging](logging.md)

The preferred installed include is `<srt/srt.h>`. The exported CMake target is
`RobotweaxSRT::srt`. Source-tree C++ implementation headers are not installed
and are not a source- or binary-compatibility contract.

## Transport features

- [Encryption and key rotation](encryption.md)
- [AES-GCM extension contract](aes-gcm-contract.md)
- [File/Stream mode](file-mode.md)
- [Rendezvous](rendezvous.md)
- [Connection groups and bonding](connection-groups.md)
- [Packet-filter FEC](packet-filter.md)
- [Live timing](live-timing.md)

These feature documents describe their own option ordering, mode combinations,
and evidence boundaries. Check [Known limitations](limitations.md) before
combining features.

## Protocol and implementation reference

- [Architecture](architecture.md)
- [ACK and ACKACK semantics](acknowledgements.md)
- [Protocol edge-case policy](protocol-edge-cases.md)
- [Performance and scalability measurement](performance.md)

These pages document stable protocol behavior, resource contracts, and
reproducible measurement methods. Internal implementation details that are not
named as public contracts may change between pre-1.0 releases.

## Releases and migration

- [Robotweax SRT 0.2.3 release notes](release-notes-0.2.3.md)
- [Robotweax SRT 0.2.2 release notes](release-notes-0.2.2.md)
- [Robotweax SRT 0.2.1 release notes](release-notes-0.2.1.md)
- [Robotweax SRT 0.2.0 release notes](release-notes-0.2.0.md)
- [Compatibility matrix](compatibility.md)
- [Migrating from 0.1 to 0.2](migration-0.2.md)
- [Project changelog](../CHANGELOG.md)

Robotweax release numbers, the shared-library ABI, the compatible SRT API
value, and the wire handshake generation are independent. For 0.2.3, the ABI
line is 0.2, the default public API and `srt_getversion()` value are 1.5.7,
and supported connection establishment uses HSv5.

## Project policies

- [Contributing](../CONTRIBUTING.md)
- [Security policy](../SECURITY.md)
- [Third-party notices](../THIRD_PARTY.md)
- [License](../LICENSE)

When documentation and behavior appear to disagree, treat the installed public
headers and the versioned release artifacts as the integration boundary and
open an issue with a minimal reproducer.
