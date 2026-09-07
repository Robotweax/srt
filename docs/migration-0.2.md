# Migrating from Robotweax SRT 0.1 to 0.2

Release: **Robotweax SRT 0.2.0**

Historical baseline: **Robotweax SRT 0.1.0**

Robotweax SRT 0.2 makes HSv5 the only supported production handshake and adds
the released, default-off AES-GCM extension. Applications already using SRT
1.3.0-or-newer HSv5 peers normally need no handshake configuration change.

Applications that require genuine UDT-v4 transport plus post-connect
HSREQ/HSRSP must upgrade the peer before moving to 0.2 or remain on the
immutable `v0.1.0` release.

## Version boundaries

Keep these values separate:

| Axis | Robotweax SRT 0.2.0 |
| --- | --- |
| Project release | `0.2.0` |
| Shared-library ABI | `0.2` |
| Default compatible API | Haivision SRT `1.5.5` |
| `srt_getversion()` | `1.5.5` |
| Production handshake | HSv5 |
| Default cipher profile | AES-CTR |
| Optional authenticated profile | Robotweax AES-GCM extension |

The 0.2 ABI is not promised to be binary-compatible with the pre-1.0 0.1 ABI.
Rebuild applications and plugins against the 0.2 headers and package.

## HSv5 and minimum-version behavior

The public `SRTO_MINVERSION` default remains SRT 1.0.0 (`0x00010000`) for API
compatibility. The effective production floor follows from HSv5: an integrated
HSREQ/HSRSP identity must advertise SRT 1.3.0 or newer.

Version checks are ordered as follows:

| Peer observation | Result |
| --- | --- |
| Optional HSRSP echo absent | Continue; do not invent version zero. |
| Integrated HSv5 identity below 1.3.0 | `SRT_REJ_ROGUE` (`4`) |
| Coherent identity below configured `SRTO_MINVERSION` | `SRT_REJ_VERSION` (`8`) |
| Coherent identity meeting configured minimum | Version check succeeds; other validation still applies. |

The rule is the same for Caller, Listener, and Rendezvous. A rejected proposal
cannot publish peer metadata or create a connected runtime.

## Why version 4 still appears on the wire

HSv5 discovery deliberately starts with a version-4 INDUCTION. The numeric
field alone does not select legacy HSv4. A normal HSv5 Listener returns version
5 and the SRT magic value; the later CONCLUSION establishes HSv5.

Robotweax 0.2 therefore:

- continues to emit and accept the valid HSv5 discovery form;
- never automatically downgrades after recognizing a genuine legacy response;
- answers wire-ambiguous version-4/`UDT_DGRAM` discovery statelessly as HSv5;
- rejects a genuine legacy conclusion before listener callback, accepted-socket
  publication, or epoll readiness;
- prevents legacy `UDT_STREAM` discovery from starting a positive session; and
- rejects version-4 Rendezvous setup.

A valid unsupported legacy attempt reports `SRT_ECONNREJ` with
`SRT_REJ_VERSION`. Malformed or unauthenticated input may be dropped where a
reply would allocate state or amplify traffic.

## Public API and package changes

- The project and package version become `0.2.0`; the shared-library ABI line
  becomes `0.2`.
- The exact public C export count remains versioned, but pre-1.0 binary
  compatibility with 0.1 is not guaranteed.
- Installed positive-HSv4 native declarations and state machines are removed.
- `SRTO_SENDER` remains supported; data direction is independent of handshake
  generation.
- The default public header continues to expose the selected v1.5.5 surface.
- The optional `SRTO_CRYPTOMODE` declaration is present only when the library
  and consumer are built with `ENABLE_AEAD_API_PREVIEW=ON`.
- CMake consumers should request version 0.2:

  ```cmake
  find_package(RobotweaxSRT 0.2 CONFIG REQUIRED)
  target_link_libraries(my_application PRIVATE RobotweaxSRT::srt)
  ```

Use a clean build directory after upgrading; do not reuse a CMake cache created
for 0.1.

## Private native C++ crypto changes

The native C++ implementation surface is not installed and is not a public
compatibility API. Source-tree integrations that intentionally customize
`CryptoProvider` must be updated for the authenticated-cipher capability and
factory contract introduced in 0.2.

A provider may report no GCM capability; requesting GCM through such a provider
must fail closed. Implementations that advertise it must provide prepared
AES-GCM seal/open with output erasure on authentication failure.

The 0.2 native crypto model also adds:

- explicit configured and effective cipher modes;
- authenticated payload and payload-budget types;
- mode-aware MSS, IPv4/IPv6, FEC, and tag accounting;
- independent directional key slots and acknowledged rotation; and
- immutable protected DATA for retransmission.

See [Encryption and key rotation](encryption.md) and the
[AES-GCM extension contract](aes-gcm-contract.md).

## Enabling AES-GCM

AES-GCM is released as an explicit Robotweax extension but remains default-off.
Build both the library and any consumer that uses `SRTO_CRYPTOMODE` with:

```sh
cmake -S . -B build-gcm \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_AEAD_API_PREVIEW=ON
cmake --build build-gcm --parallel
```

The flag name is retained for source and package continuity; it does not mean
the 0.2 extension is an unreleased candidate.

Do not assume that enabling the API expands every interoperability claim. The
release supports the exact modes listed in [Compatibility status](compatibility.md)
and [Known limitations](limitations.md). In particular, default AES-CTR remains
the broad v1.5.5 profile, explicit GCM never falls back to CTR, and both peers
must agree on the extension mode.

## Application upgrade checklist

1. Confirm that every production peer supports HSv5 and advertises SRT 1.3.0
   or newer.
2. Remove includes or configuration that depend on positive HSv4 or
   `legacy_handshake.hpp`.
3. Rebuild all applications and plugins against the 0.2 headers and ABI.
4. Keep `SRTO_MINVERSION` at its compatible default unless the deployment
   intentionally requires a newer peer.
5. Test both blocking and asynchronous connection rejection; do not add an
   application-level automatic protocol downgrade.
6. Verify that rejected legacy traffic cannot invoke listener admission or
   create accept readiness.
7. Update custom native crypto providers or explicitly report no GCM
   capability.
8. Enable `ENABLE_AEAD_API_PREVIEW` only for components that deliberately use
   the matching GCM extension contract.
9. Revalidate MSS and payload sizing when GCM or FEC is enabled; the GCM tag and
   FEC reservation reduce application payload capacity.
10. Re-run application lifecycle, epoll, callback, File/Stream, timing, group,
    and encryption tests for every combination actually deployed.
11. Review [Known limitations](limitations.md) before replacing an existing
    `libsrt` package.

The immutable `v0.1.0` tag remains the historical authority for retired
positive-HSv4 code and evidence. Release 0.2 does not reinterpret HSv5 as an
approximation of that legacy transport.
