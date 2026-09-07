# Robotweax SRT 0.2.1 release notes

Release date: **2026-08-29**

Project version: **0.2.1**

Shared-library ABI line: **0.2**

Default Haivision-compatible API profile: **SRT 1.5.5**

Robotweax SRT 0.2.1 is a compatibility-preserving maintenance release. It
turns the 0.2 protocol implementation into a more complete public development
and integration package, adds qualified FFmpeg discovery, and fixes runtime
edge cases found while exercising clean downstream consumers.

This is an evidence-backed pre-1.0 release. It is not a claim that Robotweax is
a universal replacement for every `libsrt` deployment.

## Version and compatibility boundaries

| Axis | Robotweax SRT 0.2.1 |
| --- | --- |
| Project release | `0.2.1` |
| Shared-library ABI | `0.2` |
| `srt_getversion()` | `1.5.5` |
| Production handshake | HSv5; coherent peer identity requires SRT 1.3.0 or newer |
| Default encryption profile | Haivision v1.5.5 AES-CTR behavior |
| AES-GCM API | Explicit Robotweax 0.2 extension, default-off |

The exact 80-symbol public C export set and shared-library ABI are unchanged
from 0.2.0. The release introduces no new public API, socket option, handshake,
packet layout, counter semantic, or encryption contract. Applications already
built against the 0.2 ABI do not need source changes; rebuilding against the
new package remains recommended for pre-1.0 deployments.

The machine-readable AES-GCM manifest remains the frozen 0.2.0 release
authority. Its immutable upstream revisions, option values, byte fixtures,
capability limits, and default-off selection are not redefined by this patch.

## Public integration additions

- An opt-in `srt.pc` compatibility facade allows an unmodified FFmpeg build to
  discover a shared Robotweax library while `robotweax-srt.pc` remains the
  canonical package identity.
- A pinned minimal FFmpeg gate validates clear and AES-CTR MPEG-TS
  Caller/Listener transfers plus decoded elementary-stream integrity.
- Public-header-only Message, File/Stream, and Connection Group demos cover
  Caller, Listener, Rendezvous, Broadcast, weighted Backup, encryption,
  packet-filter configuration, epoll, file helpers, and statistics.
- The installed documentation includes a standalone external CMake consumer
  that is built and executed against both static and shared packages.

## Runtime corrections

- Public startup and socket creation now initialize Winsock on Windows, so a
  clean application no longer relies on unrelated process initialization.
- Epoll defers peer-shutdown errors until TSBPD-buffered input is drained,
  preventing event-driven consumers from truncating a received stream tail.
- The Connection Group demo uses bounded member linger so the final protocol
  acknowledgement is drained before orderly shutdown.

## Performance

Producing a non-destructive `srt_bistats` snapshot no longer scans an empty
receive window. The optimization targets connected sockets with no buffered
receive data and preserves counter, reset, missing-value, lifetime, and
thread-safety semantics. It should be treated as a measured hot-path
improvement, not as a platform-independent latency guarantee.

## Documentation and project policy

- Public documentation is organized around building, integration, testing,
  compatibility, limitations, migration, and runnable examples.
- Internal research, planning, and implementation knowledge no longer ships
  as public protocol documentation.
- Installed public sources carry machine-readable MIT identifiers and adjacent
  Doxygen contracts; third-party provenance and DCO requirements are explicit.
- Robotweax release workflows must not publish Haivision binaries as Robotweax
  artifacts.
- Reference-interoperability profiles are split and path-selected so unrelated
  pull requests do not always execute every external matrix.

## Explicit limits

All 0.2.0 protocol and deployment limits remain in force, including the
narrower pinned-reference AES-GCM scope, no cross-implementation GCM
File/Stream claim, no encrypted mixed-reference group claim, and no general SRT
1.6 compatibility claim. Dedicated-host scaling, long-duration soak and fuzz,
physical multi-node fault evidence, and hardware timestamp validation remain
future production gates.

Review [Compatibility status](compatibility.md),
[Public API compatibility](api-compatibility.md), and
[Known limitations](limitations.md) before deployment.

## Release artifacts

The immutable `v0.2.1` tag is the source authority for this release. GitHub's
tag source `.tar.gz` and `.zip` archives are the intended project artifacts.
Robotweax does not redistribute Haivision SRT binaries.
