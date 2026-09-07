# Robotweax SRT 0.2.0 release notes

Release date: **2026-08-27**

Project version: **0.2.0**

Shared-library ABI line: **0.2**

Default Haivision-compatible API profile: **SRT 1.5.5**

Robotweax SRT 0.2.0 retires positive HSv4 production establishment,
consolidates supported connections on HSv5, and completes the version-pinned
AES-GCM extension across the Robotweax single-socket, File/Stream, FEC,
Connection Group, timing, security, package, and platform evidence matrices.

This is an evidence-backed pre-1.0 release. It is not a claim that Robotweax is
a universal replacement for every `libsrt` deployment.

## Version and compatibility boundaries

The project release, library ABI, public SRT compatibility value, wire
generation, and AES-GCM extension are independent:

| Axis | Robotweax SRT 0.2.0 |
| --- | --- |
| Project release | `0.2.0` |
| Shared-library ABI | `0.2` |
| `srt_getversion()` | `1.5.5` |
| Production handshake | HSv5; coherent peer identity requires SRT 1.3.0 or newer |
| Default encryption profile | Haivision v1.5.5 AES-CTR behavior |
| AES-GCM API | Explicit Robotweax extension, default-off |

The 0.2 library is not promised to be binary-compatible with 0.1. Rebuild
applications and dependencies against the 0.2 package. Both lines currently
export the same exact 80 public C symbols, but they have distinct pre-1.0 ABI
identities.

## Enabling AES-GCM

The normal build intentionally keeps the frozen v1.5.5 header surface. Enable
the 0.2 AES-GCM extension explicitly for both the library and its consumers:

```sh
cmake -S . -B build-gcm \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_AEAD_API_PREVIEW=ON
cmake --build build-gcm
ctest --test-dir build-gcm --output-on-failure
```

The build guard retains its historical name because it matches the pinned
upstream API guard. Installed CMake targets propagate the definition, and the
Linux pkg-config file publishes it in `Cflags`. Do not mix a default-header
consumer with a GCM-enabled binary interface.

## Evidence-backed AES-GCM scope

- AES-128/192/256-GCM primitives and deterministic byte fixtures;
- exact AUTO/CTR/GCM negotiation, KMREQ/KMRSP, rejection, rotation, replay,
  tag, IV, AAD, protected-payload, and immutable-retransmission contracts;
- IPv4/IPv6 Live/Message Caller/Listener and Rendezvous, including
  bidirectional AES-128-GCM transfer against the pinned capable Haivision
  revision;
- Robotweax-to-Robotweax IPv4/IPv6 File/Stream Caller/Listener and Rendezvous;
- authenticated Row/Column/Matrix FEC over `ciphertext || tag`;
- Broadcast and Backup groups with member-local sessions, late join,
  ACK-suppressed replay, path failure, and replacement-local ciphertext;
- rotating binary and MPEG-TS/PCR timing profiles across supported modes;
- malformed-KM and authenticated-DATA fuzzing, ASan/UBSan, exact exports,
  installed consumers, and load/unload lifecycle on Linux, macOS, and Windows.

The authoritative machine-readable boundary is
[`compat/robotweax-0.2-aead.json`](../compat/robotweax-0.2-aead.json).

## Breaking changes from 0.1

- Genuine HSv4 production establishment and its installed native declarations
  are removed. Valid legacy attempts fail deterministically instead of
  triggering an automatic downgrade.
- The project and shared-library ABI move from 0.1.0/0.1 to 0.2.0/0.2.
- Native crypto provider implementations must satisfy the authenticated-cipher
  capability/factory contract or explicitly report GCM as unavailable.

See the [0.1-to-0.2 migration guide](migration-0.2.md) for application and
source migration details. The immutable `v0.1.0` tag remains the authority for
the retired positive HSv4 implementation and evidence.

## Explicit limits

- The GCM extension is not part of the default v1.5.5 public header and is not
  a complete SRT 1.6 compatibility claim.
- Pinned-reference GCM transfer evidence covers AES-128 Live/Message.
  AES-192/256 reference transfer and pinned-reference Live Caller/Listener
  rotation are not claimed.
- The pinned reference requires TSBPD for GCM and therefore rejects the exact
  Robotweax File/Stream bundle. Cross-implementation GCM File/Stream is not
  claimed.
- Encrypted mixed-reference Connection Groups are not claimed because the
  pinned public group configuration surface cannot express the required mode.
- Long-duration soak and fuzz campaigns, physical two-node fault evidence,
  dedicated-host scaling/performance targets, and hardware timestamp evidence
  remain future production gates.

## Release evidence and artifacts

Release `v0.2.0` was created from commit
`e28f5f317f1d497ba4d569a25c95cae94a389b9c`. Its release qualification covers
the default AES-CTR profile, the stated GCM interoperability scope,
cross-platform static and shared packages, sanitizer and fuzz checks, installed
package consumers, and the exact public export surface.

The immutable tag is the source authority for Robotweax SRT 0.2.0. GitHub's
tag source `.tar.gz` and `.zip` archives contain the corresponding release
sources; users should verify that downstream packages identify the same project
version and ABI line.
