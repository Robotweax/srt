# Robotweax SRT 0.2.2 release notes

Release date: **2026-08-31**

Project version: **0.2.2**

Shared-library ABI line: **0.2**

Default Haivision-compatible API profile: **SRT 1.5.7**

Robotweax SRT 0.2.2 is a compatibility-preserving patch release. It advances
the default public SRT API profile from 1.5.5 to 1.5.7, corrects the wire
representation of no-argument control packets, and rejects malformed control
and key-management input before it can affect connection state or liveness.

This is an evidence-backed pre-1.0 release. It is not a claim that Robotweax
is a universal replacement for every `libsrt` deployment.

## Version and compatibility boundaries

| Axis | Robotweax SRT 0.2.2 |
| --- | --- |
| Project release | `0.2.2` |
| Shared-library ABI | `0.2` |
| `srt_getversion()` | `1.5.7` |
| Primary reference | Haivision SRT v1.5.7, commit `899348d8318eb9a3c5a5b6ec43c4a1114288773a` |
| Production handshake | HSv5; coherent peer identity requires SRT 1.3.0 or newer |
| Default encryption profile | Haivision-compatible AES-CTR behavior |
| AES-GCM API | Explicit Robotweax 0.2 extension, default-off |

The shared-library ABI remains on line 0.2 and the exact 80-symbol public C
export set is unchanged. Applications already built for the 0.2 ABI do not
need source changes, although rebuilding is recommended for pre-1.0 releases.
The public header adds the append-only `SRT_KM_S_E_SIZE = 6` enum sentinel and
the platform-correct `SYSSOCKET_INVALID` constant from the 1.5.7 profile.

The machine-readable AES-GCM manifest remains the frozen 0.2.0 release
authority. Advancing the default compatibility profile does not redefine the
Robotweax AES-GCM extension or expose preview-only APIs by default.

## Wire and runtime corrections

- ACKACK, KEEPALIVE, SHUTDOWN, congestion-warning, and PEERERROR controls now
  carry the required four-byte zero word when they have no argument.
- Empty or non-32-bit-aligned control payloads are rejected before statistics,
  liveness timestamps, key state, or connection state can change.
- No-argument controls reject nonzero or surplus words rather than accepting a
  noncanonical representation.
- Malformed key-management requests and responses fail closed. Unknown state
  values at or above `SRT_KM_S_E_SIZE` are never admitted as peer key state.

## Reference interoperability

The primary interoperability profile uses the immutable official Haivision
SRT v1.5.7 tag. Focused v1.5.5 backward-encryption and v1.5.6 security lanes
remain explicitly version-scoped.

Qualified v1.5.7 profiles include bidirectional Caller/Listener and
Rendezvous operation, exact Message contracts, TSBPD, NAK and retransmission,
DROPREQ, AES-128/192/256 key rotation, Row/Column/Matrix FEC, Broadcast and
Backup groups, late join, PEERERROR isolation, close, and cleanup behavior.
Claims remain limited to the documented topologies and profiles.

Pinned Haivision v1.5.5 and v1.5.7 file helpers may complete a byte-identical
size-to-EOF transfer while the sender statistics snapshot trails by at most
three packets. The qualification harness recognizes this bounded quirk only
for those exact versions and continues to reject missing or mismatched data.

The FEC geometry ceiling remains 128 columns as a bounded-resource contract.
Haivision v1.5.7 accepts a broader option range; Robotweax does not claim full
option-range parity beyond its documented bounds.

## CI and artifact policy

- Reference versions are explicit harness inputs rather than implicit 1.5.5
  assumptions.
- The primary CI reference is pinned to the exact v1.5.7 commit.
- Legacy v1.5.5 and v1.5.6 evidence remains separate from the primary gate.
- Reference preparation artifacts use an explicit allowlist and contain only
  Robotweax-built tools. Robotweax does not redistribute Haivision binaries.

## Security properties

Negative tests cover empty, unaligned, noncanonical, and semantically invalid
control payloads, invalid key-state values, wrong passphrases, and malformed
key material. Rejected input does not refresh peer liveness or publish
provisional plaintext.

## Performance maintenance

- Send/receive buffer byte totals and timestamp spans use bounded incremental
  accounting instead of per-sample scans. Message-expiration scans are skipped
  when no buffered packet has a deadline.
- Readiness publication avoids the global waiter mutex and condition-variable
  broadcast when no epoll waiter is registered, while retaining generation-
  based wakeup semantics for active waiters.

## Explicit limits

Robotweax implements a substantial SRT 1.5.7-compatible surface, not every
API behavior or deployment combination. APIs marked `partial` or `excluded`
in `compat/srt-1.5.7-api.json` remain outside the complete compatibility
claim. Positive HSv4 establishment, universal drop-in ABI compatibility,
unbounded FEC geometries, and general SRT 1.6 compatibility are not claimed.

Review [Compatibility status](compatibility.md),
[Public API compatibility](api-compatibility.md), and
[Known limitations](limitations.md) before deployment.

## Release artifacts

The immutable `v0.2.2` tag is the source authority for this release. GitHub's
tag source `.tar.gz` and `.zip` archives are the intended project artifacts.
Robotweax does not redistribute Haivision SRT binaries.
