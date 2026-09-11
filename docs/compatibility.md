# Compatibility status

This document describes the public compatibility boundary of Robotweax SRT
0.2.4. A listed capability is part of the supported boundary only for the
roles, transport mode, address family, and encryption profile stated here.

Robotweax SRT is an independent implementation. Compatibility means that the
documented public behavior and wire exchange have been validated against the
stated SRT profile; it does not imply shared implementation code or universal
drop-in compatibility with every `libsrt` deployment.

## Version boundary

| Axis | Robotweax SRT 0.2.4 |
| --- | --- |
| Project version | `0.2.4` |
| Shared-library ABI line | `0.2` |
| Default public SRT API profile | Haivision SRT `1.5.7` |
| `srt_getversion()` | `1.5.7` |
| Production handshake | HSv5 |
| Default encryption | AES-CTR |
| Authenticated-encryption extension | AES-GCM, explicit and default-off |

Project version, shared-library ABI, public SRT API version, handshake
generation, and encryption profile are independent. In particular,
`srt_getversion()` reports the selected public SRT compatibility profile, not
the Robotweax release number.

The primary external reference is the unmodified official Haivision SRT
v1.5.7 tag at commit
`899348d8318eb9a3c5a5b6ec43c4a1114288773a`. Focused 1.5.5 backward and 1.5.6
security lanes remain separate evidence; they do not replace the 1.5.7 gate.

Robotweax SRT 0.2.4 does not establish positive HSv4 sessions. Applications
that require genuine HSv4 must remain on the immutable 0.1 line or upgrade the
peer. Valid unsupported legacy establishment attempts fail deterministically
instead of causing an automatic downgrade.

## Status terminology

- **Supported** means the capability is implemented for the stated scope and
  is covered by public behavior and interoperability validation.
- **Foundation** means the public contract is implemented, but not every
  behavior of every external SRT release is claimed.
- **Partial** means the listed subset is available; applications must not
  assume complete API or behavioral parity.
- **Not claimed** means the combination may be rejected or unqualified and
  must not be used as an interoperability guarantee.

## Transport and protocol matrix

| Capability | Status | Public scope in 0.2.4 |
| --- | --- | --- |
| HSv5 Caller/Listener | Supported | Blocking and nonblocking establishment, admission callbacks, connection completion, Stream ID, IPv4 and IPv6 |
| HSv5 Rendezvous | Supported | IPv4 and IPv6 simultaneous open with deterministic role resolution |
| Live/Message transport | Supported | Message boundaries, ordered and unordered delivery, ACK/NAK recovery, retransmission, pacing, TSBPD, TLPKTDROP, and DROPREQ |
| File/Stream transport | Supported | Caller/Listener and Rendezvous, partial reads/writes, FileCC, public file helpers, shutdown and zero-byte stream EOF |
| Sequence rollover | Supported | Wrap-safe DATA, ACK, NAK, retransmission, File/Stream, encryption, and FEC behavior across the 31-bit sequence boundary |
| IPv6 | Supported | Caller/Listener and Rendezvous, asymmetric MSS negotiation, Stream ID, statistics, Live/Message, File/Stream, encryption, and FEC |
| TSBPD and source time | Foundation | Negotiation, source-time mapping, release scheduling, clock-skew correction, and live timing behavior |
| LiveCC | Partial | Pacing and runtime bandwidth changes are implemented; complete parity with every reference congestion-control behavior is not claimed |
| FileCC | Supported | Clear and AES-CTR transfer in the default compatibility profile; Robotweax-to-Robotweax AES-GCM transfer in an extension-enabled build; Caller/Listener and Rendezvous, loss recovery, delayed ACK/flow-window handling, runtime bandwidth changes, and rollover. Cross-implementation GCM File/Stream is not claimed. |
| Packet filters and FEC | Supported | Row, Column, and recursive Matrix geometries, negotiation, recovery, rollover, clear and encrypted Live/Message operation |
| Broadcast groups | Supported | Member admission, mirrored messages, late join, redundant receive suppression, member callbacks, and group metadata |
| Backup groups | Supported | Weighted selection, stable failover, bounded replay, path replacement, member-local timing, and member-local encryption |
| ACK/NAK/KEEPALIVE | Supported | Periodic control, ACKACK/RTT sampling, receive-window updates, bounded loss reports, rollover-safe ranges, and stale-loss handling |
| Per-message TTL | Foundation | Finite and infinite TTL, expiry, DROPREQ, rollover, and repeated-NAK handling |
| TLPKTDROP/DROPREQ | Supported | Negotiated sender expiry and receiver-side gap skipping with cumulative progress |
| Statistics | Foundation | Stable public ABI, cumulative and interval snapshots, packet/byte/rate/buffer/RTT/loss/retransmission/FEC/encryption counters |

## Public API compatibility

| API area | Status | Boundary |
| --- | --- | --- |
| Startup, cleanup, sockets, bind, listen, accept, connect, close | Foundation | Normal lifecycle and concurrent use are implemented; complete process-level parity with all historical releases is not claimed |
| Message and Stream I/O | Partial | Supported modes and blocking/nonblocking behavior are available; unsupported flag combinations fail explicitly |
| Socket options | Supported for the current feature set | Option state, phase restrictions, inheritance, and connected getters follow the documented Robotweax 0.2 contract |
| Epoll | Partial | SRT sockets, groups, system sockets, level/edge readiness, timeouts, release wakeups, async connect/accept, message/error readiness, and group update events |
| Listener and connect callbacks | Supported | Pre-connection admission and one-shot nonblocking completion without invoking user code while internal socket/group locks are held |
| Logging control | Supported | Public log level, facilities, handler, stream, and flags with secret-safe cryptographic diagnostics |
| Connection Groups API | Supported for Broadcast and Backup | Group creation, configuration, connect, accept, membership, send/receive, state, callbacks, and epoll integration |

Applications should treat Robotweax SRT as a capability-defined implementation,
not infer support from the presence of a similarly named upstream function.
Unsupported or incoherent combinations return an explicit error.

## Encryption compatibility

### AES-CTR default profile

The default build exposes the SRT 1.5.7-compatible API and supports
AES-128/192/256-CTR with HSv5 KMREQ/KMRSP, even/odd keys, acknowledged runtime
rotation, immutable encrypted retransmission, Caller/Listener, Rendezvous,
Live/Message, File/Stream, FEC, and supported Connection Group behavior.

AES-CTR provides confidentiality but does not authenticate DATA payloads.

### AES-GCM extension profile

AES-GCM is released in 0.2.0 as an explicit Robotweax extension. It remains
absent from the default header profile and must be selected when building both
the library and its consumers:

```sh
cmake -S . -B build-gcm \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_AEAD_API_PREVIEW=ON
```

The flag name is historical; it is the supported build switch for the 0.2.0
extension. An extension-enabled package exposes `SRTO_CRYPTOMODE`, propagates
the required compile definition to CMake consumers, and publishes it through
pkg-config where available. Do not mix extension-disabled headers with an
extension-enabled binary interface.

The released Robotweax-to-Robotweax GCM scope includes:

- AES-128/192/256-GCM primitives and key slots;
- IPv4 and IPv6 Caller/Listener and Rendezvous;
- Live/Message and File/Stream transport;
- public Stream I/O and file helpers;
- acknowledged key rotation and immutable retransmission;
- Row, Column, and Matrix FEC over authenticated protected payloads;
- Broadcast and Backup groups with member-local keys and ciphertext; and
- binary and MPEG-TS/PCR timing profiles.

Positive interoperability with the pinned GCM-capable reference profile is
limited to AES-128-GCM Live/Message scenarios. Cross-implementation GCM
File/Stream, AES-192/256-GCM transfer, encrypted mixed-implementation groups,
and reference-side Caller/Listener rotation are not claimed.

The normative GCM option, negotiation, wire, authentication, retransmission,
FEC, and group rules are defined in the
[AES-GCM contract](aes-gcm-contract.md). Operational configuration is covered
by [Encryption and key rotation](encryption.md).

## Known limits

- Robotweax SRT 0.2.4 is a pre-1.0 release and does not promise binary
  compatibility with 0.1. Applications must be rebuilt against ABI line 0.2.
- The default public header intentionally matches the SRT 1.5.7 profile;
  AES-GCM does not constitute a complete SRT 1.6 API or ABI claim.
- Positive HSv4 establishment is not supported in the 0.2 release line.
- Complete behavioral parity for every lifecycle, epoll, statistics,
  congestion-control, and uncommon flag combination is not claimed.
- AES-GCM combinations outside the explicit matrix above are not qualified.
- Single-host and automated interoperability evidence does not replace
  application-specific validation on the intended network, operating system,
  workload, latency, and failure model.

For upgrades from 0.1, see the [0.2 migration guide](migration-0.2.md).
