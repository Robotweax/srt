# Robotweax SRT 0.2 AES-GCM contract

Status: **released in Robotweax SRT 0.2.0; explicit and default-off**

Profile: **`robotweax-srt-0.2-aead`**

Contract date: **2026-08-27**

## Scope and compatibility boundary

This document is the normative AES-GCM extension contract for Robotweax SRT
0.2.0. It defines public option values, transport bundles, negotiation,
key-material suites, nonce and AAD derivation, tag placement, payload budgets,
rotation, retransmission, FEC, Connection Group behavior, and fail-closed error
semantics.

AES-GCM is not enabled by the default public SRT-compatible build. Robotweax
SRT 0.2.0 originally released this contract alongside the then-current SRT
1.5.5 profile; Robotweax 0.2.2 advanced that default
profile to SRT 1.5.7 without changing the frozen GCM extension. GCM is exposed
only by an extension-enabled library and matching consumer. The extension does
not claim complete SRT 1.6 API, ABI, behavior, or interoperability.

The machine-readable authorities are:

- [`compat/robotweax-0.2-aead.json`](../compat/robotweax-0.2-aead.json), which
  freezes the selected API and wire profile; and
- [`compat/robotweax-0.2-aead-fixtures.json`](../compat/robotweax-0.2-aead-fixtures.json),
  which freezes AES-128/192/256-GCM derivation and protected-payload vectors.

The profile manifest uses schema version 2. Version 2 replaces the pre-release
schema with explicit implementation and release-evidence records for 0.2.0.
Consumers must reject an unknown schema version instead of inferring field
semantics. The fixture document has its own independently versioned schema.

The wire profile is frozen against the public behavior, API, and wire design
represented by the pinned Haivision SRT revision
[`d99d2e1a3b1a213b03c7dfbea5133898935fdeea`](https://github.com/Haivision/srt/tree/d99d2e1a3b1a213b03c7dfbea5133898935fdeea)
and the pinned SRT protocol working copy
[`461a1fea85baaa4a928fa634d30c846dfd047ab8`](https://github.com/Haivision/srt-rfc/blob/461a1fea85baaa4a928fa634d30c846dfd047ab8/draft-sharabayko-srt.md).
The older preview that used CTR-style IV construction and host-order AAD is a
different wire profile and is not accepted.

The upstream implementation files reviewed while resolving GCM ambiguities are
listed at immutable URLs in the machine-readable manifest. They are retained
as provenance and compatibility evidence rather than concealed behind a
general clean-room claim. Robotweax code is independently authored under the
MIT License; no Haivision source is vendored, copied into this contract, or
included in Robotweax release packages. Haivision reference binaries and
helpers linked against them are development inputs only and are not published
as Robotweax GitHub Actions artifacts.

## Enabling the extension

Build the library explicitly with:

```sh
cmake -S . -B build-gcm \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_AEAD_API_PREVIEW=ON
cmake --build build-gcm --parallel
```

The build option retains its historical preview name. In Robotweax SRT 0.2.0,
it is the supported switch for the released, default-off extension.

An extension-enabled package exposes `SRTO_CRYPTOMODE`, `SRT_REJ_CRYPTO`, and
the corresponding enum layout. Installed CMake targets propagate the required
consumer definition; the Linux pkg-config metadata publishes it in `Cflags`.
Do not compile with default-profile headers and link against an
extension-enabled binary interface, or vice versa.

## Public option contract

`SRTO_CRYPTOMODE` is socket option 62. Its value is a pre-connection
`int32_t`:

| Value | Name | Meaning |
| ---: | --- | --- |
| `0` | `AUTO` | Retain compatible automatic selection; resolves to CTR unless an explicit GCM Caller is accepted by an AUTO Listener |
| `1` | `AES-CTR` | Require the compatible CTR suite |
| `2` | `AES-GCM` | Require the authenticated GCM suite |

After establishment, the getter reports the concrete effective value `1` or
`2`, never `AUTO`.

A Listener callback sees the mode inherited by the provisional accepted
socket and may override it before establishment completes. It cannot inspect
the Caller's requested mode directly at that point.

### Valid transport bundles

GCM accepts exactly these coherent bundles:

| Bundle | Congestion control | TSBPD | Public data API | FEC |
| --- | --- | --- | --- | --- |
| Live/Message | LiveCC | On | Message | Built-in Row/Column/Matrix filters allowed |
| File/Stream | FileCC | Off | Stream and file helpers | Not allowed |

Setting `SRTO_TRANSTYPE` changes the corresponding bundle atomically. An
inconsistent partial mutation while GCM is selected fails instead of silently
changing another transport property.

Caller/Listener and Rendezvous support both IPv4 and IPv6. Rendezvous requires
`SRTO_RENDEZVOUS` and explicit GCM on both peers; option-setting order does not
change the result.

Broadcast and Backup group handles accept explicit GCM before the first
connection. The option becomes a default for each member; it does not turn the
group into one shared cryptographic session.

## Negotiation

Caller/Listener negotiation is exact:

| Caller | Listener AUTO | Listener CTR | Listener GCM |
| --- | --- | --- | --- |
| AUTO | CTR | CTR | reject |
| CTR | CTR | CTR | reject |
| GCM | GCM | reject | GCM |

Rendezvous negotiation is exact and intentionally stricter:

| Initiator | Responder AUTO | Responder CTR | Responder GCM |
| --- | --- | --- | --- |
| AUTO | CTR | CTR | reject |
| CTR | CTR | CTR | reject |
| GCM | reject | reject | GCM |

A mismatch is never downgraded silently. It sets
`SRT_KM_S_BADCRYPTOMODE = 5` and rejects establishment with
`SRT_REJ_CRYPTO = 17`. An unavailable GCM provider or an incoherent transport
bundle fails before any GCM key is installed.

## Key material and suites

Only these cipher/authentication pairs are valid:

| Suite | Cipher byte | Authentication byte |
| --- | ---: | ---: |
| AES-CTR | `2` | `0` |
| AES-GCM | `4` | `1` |

Mixed or unknown pairs fail before provider work and cannot install, replace,
acknowledge, or roll back key state.

GCM supports 128-, 192-, and 256-bit stream keys. It retains the SRT security
mechanism's 16-byte salt, PBKDF2 derivation from the last eight salt bytes,
RFC 3394 wrapping, KMREQ/KMRSP exchange, even/odd key selectors, and bounded
key-generation history.

An empty passphrase disables encryption. A non-empty passphrase must be 10 to
80 bytes. Invalid lengths are rejected before copying or deriving key
material; an oversized passphrase is never truncated.

## Protected DATA layout

For a DATA packet with 31-bit sequence number `S`, 16-byte salt `N`, clear
16-byte SRT DATA header `H`, and plaintext `P`:

1. Form the 12-byte IV as
   `N[0:12] XOR (00 00 00 00 00 00 00 00 || uint32_be(S))`.
   The high bit of the 32-bit sequence word is zero.
2. Form 16-byte AAD from the four DATA-header words in network byte order.
   The AAD covers sequence, message boundary, in-order flag, key selector,
   message number, timestamp, and destination socket ID. The retransmission
   bit is forced to zero before authentication.
3. Seal `P` with AES-GCM using the selected key, IV, and AAD.
4. Append the complete 16-byte authentication tag. The SRT DATA body on the
   wire is `ciphertext || tag`.

No shortened tag, alternate IV length, host-order AAD, or surplus protected
bytes are accepted. The clear SRT header is authenticated but not encrypted.

The retransmission-bit normalization permits an ordinary retransmission to set
that wire bit while reusing the original protected bytes. Every other
authenticated header field must remain unchanged.

## MSS and payload budget

Payload capacity is computed only after `AUTO` resolves to a concrete mode.
The budget subtracts, in order:

1. the IPv4 or IPv6, UDP, and SRT DATA overhead from the negotiated MSS;
2. any required packet-filter recovery header; and
3. the complete 16-byte GCM tag.

For unfiltered GCM DATA, the validated plaintext ceilings include:

| Negotiated MSS | Address family | Maximum plaintext |
| ---: | --- | ---: |
| `1500` | IPv4 | `1440` bytes |
| `1500` | IPv6 | `1420` bytes |
| `1280` | IPv6 | `1200` bytes |

`SRTO_PAYLOADSIZE` cannot enlarge this ceiling. Application byte statistics
measure plaintext and exclude the 16-byte tag; wire-byte statistics include
the complete transmitted datagram.

## Rotation and key lifetime

Transmit and receive directions own separate even/odd key pairs. The initiator
creates the initial key, and the responder installs and confirms it during the
HSv5 exchange. Each Robotweax endpoint subsequently confirms independently
generated local transmit material through runtime KMREQ/KMRSP before sending
DATA. Initial handshake material is receive-only compatibility state, including
when only one endpoint has this direction-separation behavior. The key and salt
used for local DATA must not be cloned into the opposite transmit direction.
Later rotations are independent by direction. See the
[initial key-state and startup-latency contract](encryption.md#key-rotation).

At `refresh_rate - preannouncement` transmitted packets, the sender:

1. creates the inactive even or odd generation;
2. sends an RFC-3394-wrapped KMREQ;
3. retries the same KMREQ until it receives the matching KMRSP; and
4. changes the DATA key selector only at the acknowledged refresh boundary.

If confirmation is absent at the boundary, new DATA pauses while key exchange
continues. Already protected retransmissions remain eligible. The sender never
silently exceeds the configured key lifetime or uses an unacknowledged key.

Both current receive selectors and a fixed, bounded history of prior
generations remain available for delayed packets. Delayed control duplicates
may be acknowledged again without reinstalling old keys. Stale KMRSP messages
cannot acknowledge a newer rotation or roll state back. Packets outside the
bounded key history fail closed.

## Reliability and replay invariants

- A `(key, IV)` pair creates at most one cryptogram.
- First transmission stores the complete protected body and selector in the
  reliability slot.
- Retransmission reuses the original ciphertext, tag, and selector byte for
  byte; it does not invoke the provider again or advance key counters.
- A repeated preservation operation is accepted only if mode, selector,
  length, ciphertext, and tag are identical.
- Authentication completes before plaintext enters reliability ordering,
  TSBPD, replay history, Stream/Message delivery, or application buffers.
- A later authentic copy of a dropped packet may recover normally.
- Sequence comparisons, retained generations, and IV derivation remain
  wrap-safe across the 31-bit DATA sequence boundary.

## FEC invariants

Authenticated FEC is valid only for the Live/Message bundle.

- The sender seals DATA first; parity covers the complete
  `ciphertext || tag` body.
- The receiver reconstructs protected payload and every authenticated header
  field before attempting GCM open.
- The retransmission bit on a reconstructed packet remains excluded from AAD.
- Fragmented Message DATA is not emitted into authenticated FEC when the
  recovery header cannot reproduce all authenticated boundary fields.
- Corrupt parity or a corrupt reconstructed tag publishes no plaintext and
  does not become a terminal connection error by itself.

Row, Column, and recursive Matrix geometries are supported between Robotweax
peers. Positive cross-implementation recovery depends on the exact reference
geometry and direction; it must not be inferred from self-interoperability.

## Connection Group invariants

Each Broadcast or Backup member owns independent:

- mode negotiation and peer identity;
- salt, key generations, rotation schedule, and receive-key history;
- packet sequence and replay state; and
- protected payload and ciphertext.

Groups never copy traffic keys or ciphertext between paths. Broadcast seals
the logical message separately for each member. Backup replay retains bounded
logical plaintext and original message/timing metadata, then seals it once
through the selected replacement member. If required logical history has been
evicted, replay fails closed instead of sending incomplete or unauthenticated
data.

## Failure semantics

Configuration, key negotiation, and DATA authentication failures remain
distinct:

- unsupported mode, incompatible peer selection, or an inconsistent transport
  bundle rejects setup with the bad-mode state;
- malformed, mixed, or unsupported KM input is rejected without key-state
  mutation;
- a wrong passphrase retains the normal bad-secret establishment result;
- a missing or truncated tag, AAD mismatch, or authentication failure erases
  provisional output and drops that packet without publishing plaintext;
- failed authentication does not refresh peer liveness, advance reliability,
  or advance receive-key sequence state; and
- diagnostics never include passphrases, keys, salts, plaintext, ciphertext,
  or authentication tags.

An isolated DATA authentication failure is nonterminal so that an authentic
retransmission may recover. Repeated failures remain bounded by normal receive
processing and peer-idle timeout behavior; this does not weaken the
per-packet no-plaintext guarantee.

## Normative requirements

| ID | Requirement |
| --- | --- |
| `SRT-AEAD-001` | The default public SRT profile remains CTR-compatible and does not expose GCM. |
| `SRT-AEAD-002` | GCM public values, suites, negotiation outcomes, and wire behavior are revision-pinned. |
| `SRT-AEAD-003` | IV and AAD derivation use the current network-order profile and exclude only the retransmission bit. |
| `SRT-AEAD-004` | AES-128/192/256-GCM reproduce the frozen fixtures and publish no output on authentication failure. |
| `SRT-AEAD-005` | Only `(cipher=2, auth=0)` and `(cipher=4, auth=1)` are accepted. |
| `SRT-AEAD-006` | Negotiation and the connected effective option match the exact Caller/Listener and Rendezvous tables. |
| `SRT-AEAD-007` | The complete tag is carried, budgeted, validated, and retained with immutable retransmissions. |
| `SRT-AEAD-008` | Authentication precedes every plaintext publication boundary. |
| `SRT-AEAD-009` | FEC protects and reconstructs protected bytes before authentication. |
| `SRT-AEAD-010` | Every Connection Group member owns independent crypto and replay protection. |
| `SRT-AEAD-011` | Rotating GCM preserves source time, TSBPD release, egress, burst, MPEG-TS/PCR, replay, and payload-integrity semantics. |
| `SRT-AEAD-012` | Malformed KM and protected DATA remain bounded and fail closed without plaintext, key-state, adjacent-buffer, or secret-diagnostic exposure. |
| `SRT-AEAD-013` | Default and extension-enabled packages retain the recorded 0.2 ABI and exact public export contract on supported platforms. |

## Interoperability boundary

Robotweax-to-Robotweax operation covers the complete role, address-family,
Live/Message, File/Stream, FEC, group, rotation, timing, and key-size scope
described above.

Positive interoperability with the pinned GCM-capable reference profile is
limited to AES-128-GCM Live/Message. The following are intentionally not
claimed for cross-implementation GCM use in 0.2.0:

- File/Stream transfer;
- AES-192/256 payload transfer;
- encrypted mixed-implementation Connection Groups; and
- reference-side Caller/Listener rotation.

These limits do not change the fail-closed negotiation rules. Applications
must validate any deployment-specific combination outside the explicit public
matrix before production use.
